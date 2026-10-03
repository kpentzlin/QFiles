// Modul C: Dateivergleich (nicht modales Top-Level-Fenster).
//
// - Textdateien: zeilenweiser Vergleich (Myers-O(ND), lineare Speichervariante mit Kostenheuristik
//   und Zeitlimit), nebeneinander in einer selbst gezeichneten, doppelt gepufferten Ansicht mit
//   Zeilennummern, Füllzeilen, zeichenweiser Hervorhebung und Übersichtsbalken.
// - Binärdateien: blockweiser Vergleich im Thread, Liste der ersten 10 000 Abweichungen.
// - Externes Vergleichsprogramm, falls in den Optionen eingetragen.

#include "Modules.h"
#include "App.h"
#include "Encoding.h"
#include "Settings.h"
#include "Util.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace qf {

namespace {

const wchar_t* const kSection = L"Dateivergleich";
const wchar_t* const kFrameClass = L"QFilesCompareFrame";
const wchar_t* const kViewClass = L"QFilesDiffView";

constexpr uint64_t kMaxTextBytes = 64ull << 20;      // größere Dateien nur binär vergleichen
constexpr size_t kMaxCharDiffLen = 2000;             // zeichenweiser Vergleich nur für kürzere Zeilen
constexpr size_t kMaxBinaryDiffs = 10000;            // angezeigte Abweichungen beim Binärvergleich
constexpr int kMaxDisplayChars = 5000;               // längere Zeilen werden abgeschnitten dargestellt
constexpr ULONGLONG kLineDiffTimeLimit = 15000;      // ms, danach vereinfachtes Ergebnis
constexpr ULONGLONG kCharDiffTimeLimit = 5000;       // ms für alle zeichenweisen Vergleiche zusammen
constexpr size_t kBinaryBlock = 1 << 20;             // 1 MB

enum : int {
    IDM_RECOMPARE = 40101,
    IDM_EDIT_LEFT,
    IDM_EDIT_RIGHT,
    IDM_SWAP,
    IDM_CLOSE,
    IDM_IGNORE_CASE,
    IDM_IGNORE_WS,
    IDM_IGNORE_BLANK,
    IDM_FORCE_BINARY,
    IDM_FIRST,
    IDM_PREV,
    IDM_NEXT,
    IDM_LAST,
};

enum : int { IDC_TOOLBAR = 100, IDC_STATUSBAR, IDC_VIEW, IDC_BININFO, IDC_BINLIST };

constexpr UINT WM_CF_PROGRESS = WM_APP + 61;   // wParam = Generation, lParam = Prozent
constexpr UINT WM_CF_DONE = WM_APP + 62;       // wParam = Generation

// ===================================================================================
// Diff-Kern: Myers, lineare Speichervariante ("middle snake") wie in GNU diff,
// mit Heuristik für teure Fälle, Zeitlimit und Abbruch.
// Eq(i, j) liefert true, wenn Element i der Folge A gleich Element j der Folge B ist.
// ===================================================================================

template <class Eq>
class DiffEngine {
public:
    DiffEngine(int n, int m, const Eq& eq, const std::atomic<bool>* cancel, ULONGLONG deadline)
        : n_(n), m_(m), eq_(eq), cancel_(cancel), deadline_(deadline) {
        size_t diags = (size_t)n + (size_t)m + 3;
        buf_.assign(diags * 2, 0);
        fd_ = buf_.data() + m + 1;
        bd_ = fd_ + diags;
        tooExpensive_ = 1;
        for (size_t d = diags; d != 0; d >>= 2) tooExpensive_ <<= 1;
        tooExpensive_ = std::max(tooExpensive_, 4096);
    }

    // Markiert geänderte Elemente (chA: gelöscht, chB: eingefügt). false = abgebrochen.
    bool Run(std::vector<uint8_t>& chA, std::vector<uint8_t>& chB) {
        chA.assign((size_t)n_, 0);
        chB.assign((size_t)m_, 0);
        struct Task {
            int xoff, xlim, yoff, ylim;
            bool minimal;
        };
        std::vector<Task> stack;
        stack.push_back({0, n_, 0, m_, false});
        while (!stack.empty()) {
            Task t = stack.back();
            stack.pop_back();
            int xoff = t.xoff, xlim = t.xlim, yoff = t.yoff, ylim = t.ylim;
            while (xoff < xlim && yoff < ylim && eq_(xoff, yoff)) {
                ++xoff;
                ++yoff;
            }
            while (xoff < xlim && yoff < ylim && eq_(xlim - 1, ylim - 1)) {
                --xlim;
                --ylim;
            }
            if (xoff == xlim) {
                for (int y = yoff; y < ylim; ++y) chB[(size_t)y] = 1;
            } else if (yoff == ylim) {
                for (int x = xoff; x < xlim; ++x) chA[(size_t)x] = 1;
            } else if (timeUp_) {
                // Zeitlimit überschritten: Rest als vollständig geändert betrachten
                for (int x = xoff; x < xlim; ++x) chA[(size_t)x] = 1;
                for (int y = yoff; y < ylim; ++y) chB[(size_t)y] = 1;
            } else {
                Partition p;
                Diag(xoff, xlim, yoff, ylim, t.minimal, p);
                if (cancelled_) return false;
                stack.push_back({p.xmid, xlim, p.ymid, ylim, p.hiMinimal});
                stack.push_back({xoff, p.xmid, yoff, p.ymid, p.loMinimal});
            }
        }
        return true;
    }

    bool Approximate() const { return timeUp_; }

private:
    struct Partition {
        int xmid = 0, ymid = 0;
        bool loMinimal = true, hiMinimal = true;
    };

    void Diag(int xoff, int xlim, int yoff, int ylim, bool findMinimal, Partition& part) {
        int* const fd = fd_;
        int* const bd = bd_;
        const int dmin = xoff - ylim;
        const int dmax = xlim - yoff;
        const int fmid = xoff - yoff;
        const int bmid = xlim - ylim;
        int fmin = fmid, fmax = fmid;
        int bmin = bmid, bmax = bmid;
        const bool odd = ((fmid - bmid) & 1) != 0;
        fd[fmid] = xoff;
        bd[bmid] = xlim;

        for (int c = 1;; ++c) {
            if ((c & 15) == 0) {
                if (cancel_ && cancel_->load(std::memory_order_relaxed)) {
                    cancelled_ = true;
                    part.xmid = xoff;
                    part.ymid = yoff;
                    return;
                }
                if (GetTickCount64() > deadline_) timeUp_ = true;
            }
            // Vorwärtssuche um einen Schritt erweitern
            if (fmin > dmin)
                fd[--fmin - 1] = -1;
            else
                ++fmin;
            if (fmax < dmax)
                fd[++fmax + 1] = -1;
            else
                --fmax;
            for (int d = fmax; d >= fmin; d -= 2) {
                int tlo = fd[d - 1], thi = fd[d + 1];
                int x0 = tlo < thi ? thi : tlo + 1;
                int x = x0, y = x0 - d;
                while (x < xlim && y < ylim && eq_(x, y)) {
                    ++x;
                    ++y;
                }
                fd[d] = x;
                if (odd && bmin <= d && d <= bmax && bd[d] <= x) {
                    part.xmid = x;
                    part.ymid = y;
                    part.loMinimal = part.hiMinimal = true;
                    return;
                }
            }
            // Rückwärtssuche um einen Schritt erweitern
            if (bmin > dmin)
                bd[--bmin - 1] = INT_MAX;
            else
                ++bmin;
            if (bmax < dmax)
                bd[++bmax + 1] = INT_MAX;
            else
                --bmax;
            for (int d = bmax; d >= bmin; d -= 2) {
                int tlo = bd[d - 1], thi = bd[d + 1];
                int x0 = tlo < thi ? tlo : thi - 1;
                int x = x0, y = x0 - d;
                while (xoff < x && yoff < y && eq_(x - 1, y - 1)) {
                    --x;
                    --y;
                }
                bd[d] = x;
                if (!odd && fmin <= d && d <= fmax && x <= fd[d]) {
                    part.xmid = x;
                    part.ymid = y;
                    part.loMinimal = part.hiMinimal = true;
                    return;
                }
            }
            if (findMinimal && !timeUp_) continue;

            // Heuristik: zu teuer (oder Zeitlimit) -> bestes bisheriges Zwischenergebnis verwenden
            if (c >= tooExpensive_ || timeUp_) {
                int fxybest = -1, fxbest = 0;
                for (int d = fmax; d >= fmin; d -= 2) {
                    int x = std::min(fd[d], xlim);
                    int y = x - d;
                    if (ylim < y) {
                        x = ylim + d;
                        y = ylim;
                    }
                    if (fxybest < x + y) {
                        fxybest = x + y;
                        fxbest = x;
                    }
                }
                int bxybest = INT_MAX, bxbest = 0;
                for (int d = bmax; d >= bmin; d -= 2) {
                    int x = std::max(xoff, bd[d]);
                    int y = x - d;
                    if (y < yoff) {
                        x = yoff + d;
                        y = yoff;
                    }
                    if (x + y < bxybest) {
                        bxybest = x + y;
                        bxbest = x;
                    }
                }
                if ((xlim + ylim) - bxybest < fxybest - (xoff + yoff)) {
                    part.xmid = fxbest;
                    part.ymid = fxybest - fxbest;
                    part.loMinimal = true;
                    part.hiMinimal = false;
                } else {
                    part.xmid = bxbest;
                    part.ymid = bxybest - bxbest;
                    part.loMinimal = false;
                    part.hiMinimal = true;
                }
                return;
            }
        }
    }

    int n_, m_;
    const Eq& eq_;
    const std::atomic<bool>* cancel_;
    ULONGLONG deadline_;
    std::vector<int> buf_;
    int* fd_ = nullptr;
    int* bd_ = nullptr;
    int tooExpensive_ = 4096;
    bool timeUp_ = false;
    bool cancelled_ = false;
};

struct IntSeqEq {
    const int* a;
    const int* b;
    bool operator()(int i, int j) const { return a[i] == b[j]; }
};

struct CharSeqEq {
    const wchar_t* a;
    const wchar_t* b;
    bool operator()(int i, int j) const { return a[i] == b[j]; }
};

// ===================================================================================
// Datenmodelle
// ===================================================================================

struct LineRef {
    uint32_t off = 0, len = 0;
};

enum class RowKind : uint8_t { Equal, Changed, LeftOnly, RightOnly, Ignored };

struct Span {
    uint32_t start, len;
};

struct Row {
    int32_t l = -1, r = -1;          // Zeilenindex links/rechts, -1 = Füllzeile
    RowKind kind = RowKind::Equal;
    uint32_t spanL = 0, spanLn = 0;  // Hervorhebungen (Index/Anzahl in TextModel::spans)
    uint32_t spanR = 0, spanRn = 0;
};

enum : uint8_t { BlockChanged = 1, BlockLeft = 2, BlockRight = 4 };

struct Block {
    int first = 0, last = 0;         // Zeilen [first, last)
    uint8_t flags = 0;
};

struct TextModel {
    std::wstring textL, textR;
    std::vector<LineRef> linesL, linesR;
    std::vector<Row> rows;
    std::vector<Span> spans;
    std::vector<Block> blocks;
    TextEncoding encL = TextEncoding::Utf8, encR = TextEncoding::Utf8;
    LineEnding eolL = LineEnding::CRLF, eolR = LineEnding::CRLF;
    bool emptyL = false, emptyR = false;
    bool bytesIdentical = false;
    bool approximate = false;        // Zeitlimit beim Zeilenvergleich erreicht
    bool charDiffIncomplete = false; // Zeitlimit beim zeichenweisen Vergleich erreicht
    int changedRows = 0, leftOnlyRows = 0, rightOnlyRows = 0;
    int maxCols = 0;                 // längste Zeile (Spalten nach Tabulatorerweiterung)

    std::wstring_view LineL(int i) const { return {textL.data() + linesL[(size_t)i].off, linesL[(size_t)i].len}; }
    std::wstring_view LineR(int i) const { return {textR.data() + linesR[(size_t)i].off, linesR[(size_t)i].len}; }
};

struct BinDiff {
    uint64_t offset;
    uint8_t a, b;
};

struct BinaryModel {
    uint64_t sizeA = 0, sizeB = 0;
    uint64_t diffCount = 0;          // unterschiedliche Bytes im gemeinsamen Bereich
    std::vector<BinDiff> diffs;      // die ersten kMaxBinaryDiffs
};

struct CompareOptions {
    bool ignoreCase = false;
    bool ignoreWs = false;
    bool ignoreBlank = false;
    bool forceBinary = false;
    int tabWidth = 4;
};

struct CompareResult {
    bool text = false;
    std::unique_ptr<TextModel> model;
    BinaryModel bin;
    std::wstring error;
    std::wstring note;
    bool cancelled = false;
};

// ===================================================================================
// Textvergleich
// ===================================================================================

bool IsSpaceChar(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\v' || c == L'\f' || c == 0x00A0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

bool IsBlankLine(std::wstring_view s) {
    for (wchar_t c : s)
        if (!IsSpaceChar(c)) return false;
    return true;
}

void SplitLines(const std::wstring& t, std::vector<LineRef>& out) {
    out.clear();
    size_t start = 0, n = t.size();
    for (size_t i = 0; i < n; ++i) {
        wchar_t c = t[i];
        if (c == L'\r' || c == L'\n') {
            out.push_back({(uint32_t)start, (uint32_t)(i - start)});
            if (c == L'\r' && i + 1 < n && t[i + 1] == L'\n') ++i;
            start = i + 1;
        }
    }
    if (start < n) out.push_back({(uint32_t)start, (uint32_t)(n - start)});
}

int DisplayCols(std::wstring_view s, int tab) {
    int col = 0;
    for (wchar_t c : s) {
        if (c == L'\t')
            col += tab - (col % tab);
        else
            ++col;
        if (col >= kMaxDisplayChars) return kMaxDisplayChars;
    }
    return col;
}

// Fügt die Hervorhebungsbereiche einer Zeile an spans an.
void AppendSpans(const std::vector<uint8_t>& ch, std::wstring_view line, bool ignoreWs, std::vector<Span>& spans,
                 uint32_t& start, uint32_t& count) {
    start = (uint32_t)spans.size();
    size_t i = 0, n = std::min(ch.size(), line.size());
    auto marked = [&](size_t k) { return ch[k] && !(ignoreWs && IsSpaceChar(line[k])); };
    while (i < n) {
        if (!marked(i)) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && marked(j)) ++j;
        spans.push_back({(uint32_t)i, (uint32_t)(j - i)});
        i = j;
    }
    count = (uint32_t)spans.size() - start;
}

std::unique_ptr<TextModel> BuildTextModel(const std::vector<uint8_t>& ba, const std::vector<uint8_t>& bb,
                                          const CompareOptions& o, const std::atomic<bool>& cancel) {
    auto m = std::make_unique<TextModel>();
    m->bytesIdentical = ba.size() == bb.size() && (ba.empty() || std::memcmp(ba.data(), bb.data(), ba.size()) == 0);
    m->emptyL = ba.empty();
    m->emptyR = bb.empty();
    m->encL = DetectEncoding(ba.data(), ba.size(), nullptr, TextEncoding::Utf8);
    m->encR = DetectEncoding(bb.data(), bb.size(), nullptr, TextEncoding::Utf8);
    m->textL = DecodeText(ba.data(), ba.size(), m->encL);
    m->textR = DecodeText(bb.data(), bb.size(), m->encR);
    m->eolL = DetectLineEnding(m->textL, LineEnding::CRLF);
    m->eolR = DetectLineEnding(m->textR, LineEnding::CRLF);
    SplitLines(m->textL, m->linesL);
    SplitLines(m->textR, m->linesR);
    if (cancel.load()) return nullptr;

    // Zeilen auf Kennzahlen abbilden (gleicher normalisierter Inhalt = gleiche Zahl)
    std::unordered_map<std::wstring, int> ids;
    ids.reserve(m->linesL.size() + m->linesR.size());
    std::wstring key;
    auto idOf = [&](std::wstring_view s) {
        key.clear();
        if (o.ignoreWs) {
            for (wchar_t c : s)
                if (!IsSpaceChar(c)) key.push_back(c);
        } else {
            key.assign(s.data(), s.size());
        }
        if (o.ignoreCase && !key.empty()) key = ToLower(key);
        auto it = ids.find(key);
        if (it != ids.end()) return it->second;
        int id = (int)ids.size();
        ids.emplace(key, id);
        return id;
    };
    std::vector<int> seqA, seqB, mapA, mapB;   // Folge der Kennzahlen bzw. zugehörige Zeilenindizes
    for (int pass = 0; pass < 2; ++pass) {
        const auto& lines = pass ? m->linesR : m->linesL;
        auto& seq = pass ? seqB : seqA;
        auto& map = pass ? mapB : mapA;
        seq.reserve(lines.size());
        map.reserve(lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            if ((i & 4095) == 0 && cancel.load()) return nullptr;
            std::wstring_view v = pass ? m->LineR((int)i) : m->LineL((int)i);
            if (o.ignoreBlank && IsBlankLine(v)) continue;
            seq.push_back(idOf(v));
            map.push_back((int)i);
        }
    }
    ids.clear();

    std::vector<uint8_t> chA, chB;
    {
        IntSeqEq eq{seqA.data(), seqB.data()};
        DiffEngine<IntSeqEq> eng((int)seqA.size(), (int)seqB.size(), eq, &cancel, GetTickCount64() + kLineDiffTimeLimit);
        if (!eng.Run(chA, chB)) return nullptr;
        m->approximate = eng.Approximate();
    }

    // ---- Zeilen der Darstellung aufbauen ----
    const int nA = (int)seqA.size(), nB = (int)seqB.size();
    const int linesL = (int)m->linesL.size(), linesR = (int)m->linesR.size();
    int curL = 0, curR = 0;   // nächste noch nicht ausgegebene Originalzeile
    auto& rows = m->rows;
    rows.reserve((size_t)std::max(linesL, linesR) + 16);
    // Leerzeilen (bei "Leerzeilen ignorieren") bis zu den angegebenen Zeilen ausgeben
    auto flush = [&](int li, int rj) {
        while (curL < li || curR < rj) {
            Row r;
            r.l = curL < li ? curL++ : -1;
            r.r = curR < rj ? curR++ : -1;
            r.kind = RowKind::Ignored;
            rows.push_back(r);
        }
    };
    int i = 0, j = 0;
    while (i < nA || j < nB) {
        if (i < nA && j < nB && !chA[(size_t)i] && !chB[(size_t)j]) {
            int li = mapA[(size_t)i], rj = mapB[(size_t)j];
            flush(li, rj);
            Row r;
            r.l = li;
            r.r = rj;
            r.kind = RowKind::Equal;
            rows.push_back(r);
            curL = li + 1;
            curR = rj + 1;
            ++i;
            ++j;
            continue;
        }
        int i2 = i, j2 = j;
        while (i2 < nA && chA[(size_t)i2]) ++i2;
        while (j2 < nB && chB[(size_t)j2]) ++j2;
        if (i2 == i && j2 == j) {   // sollte nicht vorkommen – Sicherheitsnetz
            if (i < nA) chA[(size_t)i] = 1, i2 = i + 1;
            if (j < nB) chB[(size_t)j] = 1, j2 = j + 1;
        }
        Block b;
        b.first = (int)rows.size();
        int cntA = i2 - i, cntB = j2 - j, cnt = std::max(cntA, cntB);
        for (int k = 0; k < cnt; ++k) {
            int li = k < cntA ? mapA[(size_t)(i + k)] : -1;
            int rj = k < cntB ? mapB[(size_t)(j + k)] : -1;
            flush(li >= 0 ? li : curL, rj >= 0 ? rj : curR);
            Row r;
            r.l = li;
            r.r = rj;
            if (li >= 0 && rj >= 0) {
                r.kind = RowKind::Changed;
                b.flags |= BlockChanged;
                ++m->changedRows;
            } else if (li >= 0) {
                r.kind = RowKind::LeftOnly;
                b.flags |= BlockLeft;
                ++m->leftOnlyRows;
            } else {
                r.kind = RowKind::RightOnly;
                b.flags |= BlockRight;
                ++m->rightOnlyRows;
            }
            rows.push_back(r);
            if (li >= 0) curL = li + 1;
            if (rj >= 0) curR = rj + 1;
        }
        b.last = (int)rows.size();
        m->blocks.push_back(b);
        i = i2;
        j = j2;
    }
    flush(linesL, linesR);
    if (cancel.load()) return nullptr;

    // ---- Zeichenweiser Vergleich geänderter Zeilenpaare ----
    ULONGLONG charDeadline = GetTickCount64() + kCharDiffTimeLimit;
    std::wstring la, lb;
    std::vector<uint8_t> cA, cB;
    for (size_t k = 0; k < rows.size(); ++k) {
        Row& r = rows[k];
        if (r.kind != RowKind::Changed) continue;
        if ((k & 63) == 0) {
            if (cancel.load()) return nullptr;
            if (GetTickCount64() > charDeadline) {
                m->charDiffIncomplete = true;
                break;
            }
        }
        std::wstring_view va = m->LineL(r.l), vb = m->LineR(r.r);
        if (va.size() >= kMaxCharDiffLen || vb.size() >= kMaxCharDiffLen) continue;
        la.assign(va.data(), va.size());
        lb.assign(vb.data(), vb.size());
        if (o.ignoreCase) {
            std::wstring xa = ToLower(la), xb = ToLower(lb);
            if (xa.size() == la.size() && xb.size() == lb.size()) {   // nur bei längentreuer Umwandlung
                la.swap(xa);
                lb.swap(xb);
            }
        }
        CharSeqEq eq{la.data(), lb.data()};
        DiffEngine<CharSeqEq> eng((int)la.size(), (int)lb.size(), eq, &cancel, charDeadline);
        if (!eng.Run(cA, cB)) return nullptr;
        AppendSpans(cA, va, o.ignoreWs, m->spans, r.spanL, r.spanLn);
        AppendSpans(cB, vb, o.ignoreWs, m->spans, r.spanR, r.spanRn);
    }

    // ---- Breite der längsten Zeile ----
    for (size_t k = 0; k < m->linesL.size(); ++k) m->maxCols = std::max(m->maxCols, DisplayCols(m->LineL((int)k), o.tabWidth));
    for (size_t k = 0; k < m->linesR.size(); ++k) m->maxCols = std::max(m->maxCols, DisplayCols(m->LineR((int)k), o.tabWidth));
    return m;
}

// ===================================================================================
// Binärvergleich
// ===================================================================================

bool ReadFull(HANDLE h, uint8_t* p, DWORD n, DWORD& got) {
    got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(h, p + got, n - got, &r, nullptr)) return false;
        if (r == 0) break;
        got += r;
    }
    return true;
}

// Rückgabe false bei Fehler (error gesetzt) oder Abbruch.
bool BinaryCompare(const std::wstring& a, const std::wstring& b, BinaryModel& out, const std::atomic<bool>& cancel,
                   HWND notify, unsigned gen, std::wstring& error) {
    HANDLE ha = CreateFileW(LongPath(a).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (ha == INVALID_HANDLE_VALUE) {
        error = L"Datei A kann nicht geöffnet werden:\n" + a + L"\n\n" + LastErrorMessage();
        return false;
    }
    HANDLE hb = CreateFileW(LongPath(b).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hb == INVALID_HANDLE_VALUE) {
        error = L"Datei B kann nicht geöffnet werden:\n" + b + L"\n\n" + LastErrorMessage();
        CloseHandle(ha);
        return false;
    }
    LARGE_INTEGER sa{}, sb{};
    GetFileSizeEx(ha, &sa);
    GetFileSizeEx(hb, &sb);
    out.sizeA = (uint64_t)sa.QuadPart;
    out.sizeB = (uint64_t)sb.QuadPart;
    uint64_t common = std::min(out.sizeA, out.sizeB);
    std::vector<uint8_t> bufA(kBinaryBlock), bufB(kBinaryBlock);
    uint64_t pos = 0;
    int lastPercent = -1;
    bool ok = true;
    while (pos < common) {
        if (cancel.load()) {
            ok = false;
            break;
        }
        DWORD want = (DWORD)std::min<uint64_t>(kBinaryBlock, common - pos);
        DWORD ga = 0, gb = 0;
        if (!ReadFull(ha, bufA.data(), want, ga) || !ReadFull(hb, bufB.data(), want, gb)) {
            error = L"Lesefehler: " + LastErrorMessage();
            ok = false;
            break;
        }
        DWORD got = std::min(ga, gb);
        if (got == 0) break;
        if (std::memcmp(bufA.data(), bufB.data(), got) != 0) {
            for (DWORD k = 0; k < got; ++k) {
                if (bufA[k] != bufB[k]) {
                    ++out.diffCount;
                    if (out.diffs.size() < kMaxBinaryDiffs) out.diffs.push_back({pos + k, bufA[k], bufB[k]});
                }
            }
        }
        pos += got;
        int percent = common ? (int)(pos * 100 / common) : 100;
        if (percent != lastPercent) {
            lastPercent = percent;
            PostMessageW(notify, WM_CF_PROGRESS, gen, percent);
        }
        if (got < want) break;
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return ok;
}

// Arbeitsthread: entscheidet Text/Binär und vergleicht.
std::unique_ptr<CompareResult> RunCompare(const std::wstring& a, const std::wstring& b, const CompareOptions& o,
                                          const std::atomic<bool>& cancel, HWND notify, unsigned gen) {
    auto res = std::make_unique<CompareResult>();
    uint64_t sa = GetFileSize64(a), sb = GetFileSize64(b);
    if (sa == UINT64_MAX) {
        res->error = L"Datei A kann nicht gelesen werden:\n" + a + L"\n\n" + LastErrorMessage();
        return res;
    }
    if (sb == UINT64_MAX) {
        res->error = L"Datei B kann nicht gelesen werden:\n" + b + L"\n\n" + LastErrorMessage();
        return res;
    }
    bool tryText = !o.forceBinary;
    if (tryText && (sa > kMaxTextBytes || sb > kMaxTextBytes)) {
        tryText = false;
        res->note = L"Datei größer als " + FormatSize(kMaxTextBytes) + L" – Binärvergleich statt Textvergleich.";
    }
    if (tryText) {
        std::vector<uint8_t> ba, bb;
        if (!ReadFileBytes(a, ba, kMaxTextBytes + 1)) {
            res->error = L"Datei A kann nicht gelesen werden:\n" + a + L"\n\n" + LastErrorMessage();
            return res;
        }
        if (!ReadFileBytes(b, bb, kMaxTextBytes + 1)) {
            res->error = L"Datei B kann nicht gelesen werden:\n" + b + L"\n\n" + LastErrorMessage();
            return res;
        }
        if (!LooksBinary(ba.data(), ba.size()) && !LooksBinary(bb.data(), bb.size())) {
            res->model = BuildTextModel(ba, bb, o, cancel);
            if (!res->model) {
                res->cancelled = true;
                return res;
            }
            res->text = true;
            return res;
        }
    }
    std::wstring err;
    if (!BinaryCompare(a, b, res->bin, cancel, notify, gen, err)) {
        if (cancel.load())
            res->cancelled = true;
        else
            res->error = err;
    }
    return res;
}

// ===================================================================================
// Hilfsfunktionen
// ===================================================================================

std::wstring Hex(uint64_t v, int digits) {
    static const wchar_t kDigits[] = L"0123456789ABCDEF";
    std::wstring s((size_t)digits, L'0');
    for (int i = digits - 1; i >= 0 && v; --i) {
        s[(size_t)i] = kDigits[v & 15];
        v >>= 4;
    }
    return s;
}

std::wstring ByteChar(uint8_t c) {
    if ((c >= 0x20 && c < 0x7F) || c >= 0xA0) return std::wstring(1, (wchar_t)c);   // ASCII bzw. Latin-1
    return L"·";
}

// Schrift passend zur Fenster-DPI aus einer (für System-DPI erzeugten) Vorlage
HFONT CreateScaledFont(HFONT templ, HWND h) {
    LOGFONTW lf{};
    if (!templ || !GetObjectW(templ, sizeof(lf), &lf)) {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        lf = ncm.lfMessageFont;
    }
    HDC dc = GetDC(nullptr);
    int sysDpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    int dpi = (int)GetWindowDpi(h);
    if (sysDpi > 0 && dpi > 0 && dpi != sysDpi) lf.lfHeight = MulDiv(lf.lfHeight, dpi, sysDpi);
    return CreateFontIndirectW(&lf);
}

// ===================================================================================
// DiffView: selbst gezeichnete Ansicht beider Seiten
// ===================================================================================

const COLORREF kText = RGB(0, 0, 0);
const COLORREF kTextIgnored = RGB(150, 150, 150);
const COLORREF kBgEqual = RGB(255, 255, 255);
const COLORREF kBgChanged = RGB(255, 246, 200);
const COLORREF kHlChanged = RGB(255, 200, 80);
const COLORREF kBgLeft = RGB(255, 221, 221);
const COLORREF kBgRight = RGB(216, 244, 216);
const COLORREF kFillerBg = RGB(246, 246, 246);
const COLORREF kFillerHatch = RGB(215, 215, 215);
const COLORREF kGutterBg = RGB(242, 242, 242);
const COLORREF kGutterChanged = RGB(246, 230, 160);
const COLORREF kGutterLeft = RGB(242, 196, 196);
const COLORREF kGutterRight = RGB(190, 230, 190);
const COLORREF kGutterText = RGB(130, 130, 130);
const COLORREF kCurrentMarker = RGB(40, 90, 200);
const COLORREF kHeaderBg = RGB(230, 235, 242);
const COLORREF kDivider = RGB(185, 185, 185);
const COLORREF kMapBg = RGB(244, 244, 244);
const COLORREF kMapChanged = RGB(235, 180, 40);
const COLORREF kMapLeft = RGB(215, 85, 85);
const COLORREF kMapRight = RGB(70, 165, 70);
const COLORREF kMapView = RGB(70, 70, 70);

void FillSolid(HDC dc, const RECT& r, COLORREF c) {
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
}

class DiffView {
public:
    static void Register() {
        static bool done = false;
        if (done) return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Proc;
        wc.hInstance = App::Instance();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kViewClass;
        RegisterClassExW(&wc);
        done = true;
    }

    ~DiffView() {
        if (hatch_) DeleteObject(hatch_);
    }

    HWND Create(HWND parent, int id) {
        Register();
        hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, kViewClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
                                0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, App::Instance(), this);
        hatch_ = CreateHatchBrush(HS_BDIAGONAL, kFillerHatch);
        return hwnd_;
    }

    HWND Hwnd() const { return hwnd_; }

    void SetFont(HFONT f) {
        font_ = f;
        HDC dc = GetDC(hwnd_);
        HGDIOBJ old = SelectObject(dc, f);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        cw_ = std::max<int>(1, tm.tmAveCharWidth);
        lineH_ = std::max<int>(1, tm.tmHeight + DpiScale(hwnd_, 1));
        UpdateScrollBars();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetPaths(const std::wstring& a, const std::wstring& b) {
        pathA_ = a;
        pathB_ = b;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetModel(const TextModel* m, bool keepPos) {
        model_ = m;
        cur_ = -1;
        if (!keepPos) {
            top_ = 0;
            x_ = 0;
        }
        UpdateScrollBars();
        ScrollTo(top_, x_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetMessage(const std::wstring& msg) {
        message_ = msg;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Aktueller Unterschied (für Markierung); scroll = sichtbar machen
    void SetCurrentBlock(int b, bool scroll) {
        cur_ = b;
        if (scroll && model_ && b >= 0 && b < (int)model_->blocks.size()) {
            const Block& bl = model_->blocks[(size_t)b];
            int vis = VisibleRows();
            if (bl.first < top_ || bl.last > top_ + vis) ScrollTo(bl.first - vis / 3, x_);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int TopRow() const { return top_; }

    int VisibleRows() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        int h = rc.bottom - HeaderHeight();
        return std::max(1, h / lineH_);
    }

private:
    struct Geometry {
        int headerH, mapW, divW, paneW, gutterW, xR, avail;
    };

    int HeaderHeight() const { return lineH_ + DpiScale(hwnd_, 6); }

    Geometry Geo() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        Geometry g{};
        g.headerH = HeaderHeight();
        g.mapW = DpiScale(hwnd_, 14);
        g.divW = DpiScale(hwnd_, 3);
        g.avail = std::max<int>(0, rc.right - g.mapW);
        g.paneW = std::max(0, (g.avail - g.divW) / 2);
        g.xR = g.paneW + g.divW;
        int lines = model_ ? (int)std::max(model_->linesL.size(), model_->linesR.size()) : 0;
        int digits = 1;
        for (int v = std::max(lines, 1); v >= 10; v /= 10) ++digits;
        g.gutterW = (std::max(digits, 3) + 1) * cw_ + DpiScale(hwnd_, 6);
        return g;
    }

    int TextWidth(const Geometry& g) const { return std::max(0, g.paneW - g.gutterW); }

    int MaxTop() const {
        if (!model_) return 0;
        return std::max(0, (int)model_->rows.size() - VisibleRows());
    }

    int MaxX(const Geometry& g) const {
        if (!model_) return 0;
        return std::max(0, (model_->maxCols + 2) * cw_ - TextWidth(g));
    }

    void UpdateScrollBars() {
        if (!hwnd_) return;
        Geometry g = Geo();
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        si.nMin = 0;
        si.nMax = model_ ? std::max(0, (int)model_->rows.size() - 1) : 0;
        si.nPage = (UINT)VisibleRows();
        si.nPos = top_;
        SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
        si.nMax = model_ ? std::max(0, (model_->maxCols + 2) * cw_) : 0;
        si.nPage = (UINT)TextWidth(g);
        si.nPos = x_;
        SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
    }

    void ScrollTo(int top, int x) {
        Geometry g = Geo();
        top = std::clamp(top, 0, MaxTop());
        x = std::clamp(x, 0, MaxX(g));
        if (top == top_ && x == x_) {
            UpdateScrollBars();
            return;
        }
        top_ = top;
        x_ = x;
        UpdateScrollBars();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnVScroll(int code) {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd_, SB_VERT, &si);
        int vis = VisibleRows(), t = top_;
        switch (code) {
        case SB_LINEUP: t -= 1; break;
        case SB_LINEDOWN: t += 1; break;
        case SB_PAGEUP: t -= std::max(1, vis - 1); break;
        case SB_PAGEDOWN: t += std::max(1, vis - 1); break;
        case SB_TOP: t = 0; break;
        case SB_BOTTOM: t = MaxTop(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: t = si.nTrackPos; break;
        default: return;
        }
        ScrollTo(t, x_);
    }

    void OnHScroll(int code) {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd_, SB_HORZ, &si);
        Geometry g = Geo();
        int x = x_;
        switch (code) {
        case SB_LINELEFT: x -= 3 * cw_; break;
        case SB_LINERIGHT: x += 3 * cw_; break;
        case SB_PAGELEFT: x -= TextWidth(g) * 3 / 4; break;
        case SB_PAGERIGHT: x += TextWidth(g) * 3 / 4; break;
        case SB_LEFT: x = 0; break;
        case SB_RIGHT: x = MaxX(g); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: x = si.nTrackPos; break;
        default: return;
        }
        ScrollTo(top_, x);
    }

    void OnWheel(int delta, bool horizontal) {
        wheelAccum_ += delta;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        int steps = wheelAccum_ / WHEEL_DELTA;
        if (!steps) return;
        wheelAccum_ -= steps * WHEEL_DELTA;
        if (horizontal) {
            ScrollTo(top_, x_ + steps * 6 * cw_);
        } else if (lines == WHEEL_PAGESCROLL) {
            ScrollTo(top_ - steps * std::max(1, VisibleRows() - 1), x_);
        } else {
            ScrollTo(top_ - steps * (int)lines, x_);
        }
    }

    // Übersichtsbalken: Zeile zu y-Koordinate und zurück
    int MapRowFromY(int y) const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        int h = std::max(1, (int)rc.bottom);
        int rows = model_ ? (int)model_->rows.size() : 0;
        return (int)((long long)std::clamp(y, 0, h) * rows / h);
    }

    void MapJump(int y) {
        int row = MapRowFromY(y);
        ScrollTo(row - VisibleRows() / 2, x_);
    }

    // Zeile mit Tabulatorerweiterung, Steuerzeichen und Hervorhebungen zeichnen
    void DrawLineText(HDC dc, int x, int y, const RECT& clip, std::wstring_view line, const Span* sp, uint32_t nsp,
                      COLORREF hl, COLORREF fg) {
        int tab = std::clamp(App::Opt().editorTabWidth, 1, 16);
        disp_.clear();
        flags_.clear();
        uint32_t si = 0;
        size_t n = line.size();
        for (size_t i = 0; i < n && (int)disp_.size() < kMaxDisplayChars; ++i) {
            while (si < nsp && sp[si].start + sp[si].len <= i) ++si;
            uint8_t h = (si < nsp && sp[si].start <= i) ? 1 : 0;
            wchar_t c = line[i];
            if (c == L'\t') {
                size_t k = (size_t)tab - (disp_.size() % (size_t)tab);
                disp_.append(k, L' ');
                flags_.insert(flags_.end(), k, h);
            } else {
                if (c < 0x20 || c == 0x7F) c = 0x00B7; // Steuerzeichen als Mittelpunkt
                disp_.push_back(c);
                flags_.push_back(h);
            }
        }
        if (disp_.empty()) return;
        if (nsp) {
            dx_.resize(disp_.size());
            SIZE sz{};
            GetTextExtentExPointW(dc, disp_.data(), (int)disp_.size(), 0, nullptr, dx_.data(), &sz);
            size_t k = 0;
            while (k < disp_.size()) {
                if (!flags_[k]) {
                    ++k;
                    continue;
                }
                size_t e = k;
                while (e < disp_.size() && flags_[e]) ++e;
                RECT r{x + (k ? dx_[k - 1] : 0), y, x + dx_[e - 1], y + lineH_};
                RECT ir;
                if (IntersectRect(&ir, &r, &clip)) FillSolid(dc, ir, hl);
                k = e;
            }
        }
        SetTextColor(dc, fg);
        SetBkMode(dc, TRANSPARENT);
        ExtTextOutW(dc, x, y, ETO_CLIPPED, &clip, disp_.data(), (UINT)disp_.size(), nullptr);
        SetBkMode(dc, OPAQUE);
    }

    void PaintSide(HDC dc, const Geometry& g, int side, int rowIndex, int y, bool current) {
        const Row& row = model_->rows[(size_t)rowIndex];
        int x0 = side ? g.xR : 0;
        int line = side ? row.r : row.l;
        RECT gut{x0, y, x0 + g.gutterW, y + lineH_};
        RECT txt{x0 + g.gutterW, y, x0 + g.paneW, y + lineH_};
        COLORREF bg = kBgEqual, gbg = kGutterBg, fg = kText;
        switch (row.kind) {
        case RowKind::Equal: break;
        case RowKind::Ignored: fg = kTextIgnored; break;
        case RowKind::Changed:
            bg = kBgChanged;
            gbg = kGutterChanged;
            break;
        case RowKind::LeftOnly:
            bg = kBgLeft;
            gbg = kGutterLeft;
            break;
        case RowKind::RightOnly:
            bg = kBgRight;
            gbg = kGutterRight;
            break;
        }
        // Nummernspalte
        FillSolid(dc, gut, line >= 0 ? gbg : kGutterBg);
        if (line >= 0) {
            wchar_t num[16];
            int len = swprintf(num, 16, L"%d", line + 1);
            RECT nr = gut;
            nr.right -= DpiScale(hwnd_, 4);
            SetTextColor(dc, kGutterText);
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, num, len, &nr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SetBkMode(dc, OPAQUE);
        }
        if (current) {
            RECT m{x0, y, x0 + DpiScale(hwnd_, 3), y + lineH_};
            FillSolid(dc, m, kCurrentMarker);
        }
        // Text bzw. Füllzeile
        if (line < 0) {
            SetBkColor(dc, kFillerBg);
            FillRect(dc, &txt, hatch_);
            return;
        }
        FillSolid(dc, txt, bg);
        std::wstring_view v = side ? model_->LineR(line) : model_->LineL(line);
        const Span* sp = nullptr;
        uint32_t nsp = 0;
        if (row.kind == RowKind::Changed) {
            uint32_t s = side ? row.spanR : row.spanL;
            nsp = side ? row.spanRn : row.spanLn;
            if (nsp) sp = model_->spans.data() + s;
        }
        RECT clip = txt;
        clip.left += DpiScale(hwnd_, 2);
        DrawLineText(dc, clip.left - x_, y, clip, v, sp, nsp, kHlChanged, fg);
    }

    void Paint(HDC dc, const RECT& rc) {
        Geometry g = Geo();
        HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        FillSolid(dc, rc, kBgEqual);

        // Kopfzeile mit den Pfaden
        RECT hr{0, 0, g.avail, g.headerH};
        FillSolid(dc, hr, kHeaderBg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        for (int side = 0; side < 2; ++side) {
            int x0 = side ? g.xR : 0;
            RECT r{x0 + DpiScale(hwnd_, 6), 0, x0 + g.paneW - DpiScale(hwnd_, 4), g.headerH};
            std::wstring t = (side ? L"B: " : L"A: ") + (side ? pathB_ : pathA_);
            DrawTextW(dc, t.c_str(), (int)t.size(), &r, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        }
        SetBkMode(dc, OPAQUE);

        if (!model_) {
            RECT mr{0, g.headerH, g.avail, rc.bottom};
            SetTextColor(dc, kTextIgnored);
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, message_.c_str(), (int)message_.size(), &mr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SetBkMode(dc, OPAQUE);
        } else {
            int vis = VisibleRows() + 1;
            const Block* cb = (cur_ >= 0 && cur_ < (int)model_->blocks.size()) ? &model_->blocks[(size_t)cur_] : nullptr;
            for (int k = 0; k < vis; ++k) {
                int ri = top_ + k;
                if (ri >= (int)model_->rows.size()) break;
                int y = g.headerH + k * lineH_;
                bool current = cb && ri >= cb->first && ri < cb->last;
                PaintSide(dc, g, 0, ri, y, current);
                PaintSide(dc, g, 1, ri, y, current);
            }
        }
        // Trennlinie
        RECT dv{g.paneW, 0, g.xR, rc.bottom};
        FillSolid(dc, dv, kDivider);

        // Übersichtsbalken
        RECT mp{g.avail, 0, rc.right, rc.bottom};
        FillSolid(dc, mp, kMapBg);
        RECT ml{g.avail, 0, g.avail + 1, rc.bottom};
        FillSolid(dc, ml, kDivider);
        if (model_ && !model_->rows.empty()) {
            int h = std::max(1, (int)rc.bottom);
            long long rows = (long long)model_->rows.size();
            int pad = DpiScale(hwnd_, 3);
            for (const Block& b : model_->blocks) {
                int y1 = (int)(b.first * (long long)h / rows);
                int y2 = std::max(y1 + 2, (int)(b.last * (long long)h / rows));
                COLORREF c = (b.flags & BlockChanged) || ((b.flags & BlockLeft) && (b.flags & BlockRight)) ? kMapChanged
                             : (b.flags & BlockLeft)                                                       ? kMapLeft
                                                                                                           : kMapRight;
                RECT r{g.avail + pad, y1, rc.right - pad + 1, y2};
                FillSolid(dc, r, c);
            }
            int v1 = (int)(top_ * (long long)h / rows);
            int v2 = std::max(v1 + 3, (int)((top_ + VisibleRows()) * (long long)h / rows));
            RECT vr{g.avail + 1, v1, rc.right, std::min<int>(v2, rc.bottom)};
            HBRUSH fb = CreateSolidBrush(kMapView);
            FrameRect(dc, &vr, fb);
            DeleteObject(fb);
        }
        SelectObject(dc, oldFont);
    }

    void OnPaint() {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            Paint(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd_, &ps);
    }

    void OnKey(WPARAM vk) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        int vis = VisibleRows();
        switch (vk) {
        case VK_UP: ScrollTo(top_ - 1, x_); break;
        case VK_DOWN: ScrollTo(top_ + 1, x_); break;
        case VK_PRIOR: ScrollTo(top_ - std::max(1, vis - 1), x_); break;
        case VK_NEXT: ScrollTo(top_ + std::max(1, vis - 1), x_); break;
        case VK_HOME: ctrl ? ScrollTo(0, 0) : ScrollTo(top_, 0); break;
        case VK_END: ctrl ? ScrollTo(MaxTop(), x_) : ScrollTo(top_, MaxX(Geo())); break;
        case VK_LEFT: ScrollTo(top_, x_ - 3 * cw_); break;
        case VK_RIGHT: ScrollTo(top_, x_ + 3 * cw_); break;
        }
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        DiffView* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<DiffView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = reinterpret_cast<DiffView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
            return DefWindowProcW(h, msg, wp, lp);
        }
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_PAINT: OnPaint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            UpdateScrollBars();
            ScrollTo(top_, x_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_VSCROLL: OnVScroll(LOWORD(wp)); return 0;
        case WM_HSCROLL: OnHScroll(LOWORD(wp)); return 0;
        case WM_MOUSEWHEEL:
            OnWheel(GET_WHEEL_DELTA_WPARAM(wp), (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
            return 0;
        case WM_MOUSEHWHEEL: OnWheel(-GET_WHEEL_DELTA_WPARAM(wp), true); return 0;
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_KEYDOWN: OnKey(wp); return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            Geometry g = Geo();
            int x = GET_X_LPARAM(lp);
            if (x >= g.avail && model_) {
                mapDrag_ = true;
                SetCapture(hwnd_);
                MapJump(GET_Y_LPARAM(lp));
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (mapDrag_) MapJump(GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (mapDrag_) {
                mapDrag_ = false;
                if (msg == WM_LBUTTONUP) ReleaseCapture();
            }
            return 0;
        case WM_LBUTTONDBLCLK: {
            // Doppelklick: Datei der jeweiligen Seite im Editor öffnen
            Geometry g = Geo();
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (x < g.avail && y >= g.headerH)
                SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(x < g.xR ? IDM_EDIT_LEFT : IDM_EDIT_RIGHT, 0), 0);
            return 0;
        }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH hatch_ = nullptr;
    const TextModel* model_ = nullptr;
    std::wstring pathA_, pathB_, message_;
    int cw_ = 8, lineH_ = 16;
    int top_ = 0, x_ = 0;
    int cur_ = -1;
    int wheelAccum_ = 0;
    bool mapDrag_ = false;
    std::wstring disp_;
    std::vector<uint8_t> flags_;
    std::vector<int> dx_;
};

// ===================================================================================
// Hauptfenster des Dateivergleichs
// ===================================================================================

class CompareWindow {
public:
    CompareWindow(const std::wstring& a, const std::wstring& b) : pathA_(a), pathB_(b) {}

    bool Create() {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = Proc;
            wc.hInstance = App::Instance();
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.hIcon = App::BigIcon();
            wc.hIconSm = App::SmallIcon();
            wc.lpszClassName = kFrameClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                delete this;
                return false;
            }
            registered = true;
        }
        bool reachedNcCreate = false;
        ncCreateFlag_ = &reachedNcCreate;
        HMENU menu = BuildMenu();
        HWND h = CreateWindowExW(0, kFrameClass, L"Dateivergleich", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, menu, App::Instance(), this);
        if (!h) {
            // Nach WM_NCCREATE hat WM_NCDESTROY das Objekt bereits gelöscht (und das Menü zerstört)
            if (!reachedNcCreate) {
                DestroyMenu(menu);
                delete this;
            }
            return false;
        }
        ncCreateFlag_ = nullptr;
        // Gemerkte Fensterposition
        Config& c = App::Cfg();
        WINDOWPLACEMENT wp{sizeof(wp)};
        GetWindowPlacement(h, &wp);
        RECT r{c.GetInt(kSection, L"Left", 0), c.GetInt(kSection, L"Top", 0), c.GetInt(kSection, L"Right", 0),
               c.GetInt(kSection, L"Bottom", 0)};
        bool maximized = c.GetBool(kSection, L"Maximized", false);
        if (r.right - r.left > 200 && r.bottom - r.top > 150 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            wp.rcNormalPosition = r;
            wp.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            wp.flags = 0;
            SetWindowPlacement(h, &wp);
        } else {
            ShowWindow(h, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        }
        UpdateWindow(h);
        StartCompare(false);
        return true;
    }

private:
    static HMENU BuildMenu() {
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, IDM_RECOMPARE, L"&Neu vergleichen\tF5");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, IDM_EDIT_LEFT, L"Datei &A im Editor öffnen");
        AppendMenuW(file, MF_STRING, IDM_EDIT_RIGHT, L"Datei &B im Editor öffnen");
        AppendMenuW(file, MF_STRING, IDM_SWAP, L"Seiten &tauschen");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, IDM_CLOSE, L"&Schließen\tEsc");
        HMENU opt = CreatePopupMenu();
        AppendMenuW(opt, MF_STRING, IDM_IGNORE_CASE, L"&Groß-/Kleinschreibung ignorieren");
        AppendMenuW(opt, MF_STRING, IDM_IGNORE_WS, L"&Leerraum ignorieren");
        AppendMenuW(opt, MF_STRING, IDM_IGNORE_BLANK, L"Leer&zeilen ignorieren");
        AppendMenuW(opt, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(opt, MF_STRING, IDM_FORCE_BINARY, L"&Binärvergleich erzwingen");
        HMENU nav = CreatePopupMenu();
        AppendMenuW(nav, MF_STRING, IDM_FIRST, L"&Erster Unterschied\tAlt+Pos1");
        AppendMenuW(nav, MF_STRING, IDM_PREV, L"&Vorheriger Unterschied\tF7");
        AppendMenuW(nav, MF_STRING, IDM_NEXT, L"&Nächster Unterschied\tF8");
        AppendMenuW(nav, MF_STRING, IDM_LAST, L"&Letzter Unterschied\tAlt+Ende");
        HMENU bar = CreateMenu();
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&Datei");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)opt, L"&Vergleich");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)nav, L"&Navigation");
        return bar;
    }

    static HACCEL BuildAccelerators() {
        ACCEL a[] = {
            {FVIRTKEY, VK_F5, IDM_RECOMPARE},
            {FVIRTKEY, VK_F7, IDM_PREV},
            {FVIRTKEY, VK_F8, IDM_NEXT},
            {FVIRTKEY | FALT, VK_UP, IDM_PREV},
            {FVIRTKEY | FALT, VK_DOWN, IDM_NEXT},
            {FVIRTKEY | FALT, VK_HOME, IDM_FIRST},
            {FVIRTKEY | FALT, VK_END, IDM_LAST},
            {FVIRTKEY, VK_ESCAPE, IDM_CLOSE},
        };
        return CreateAcceleratorTableW(a, (int)(sizeof(a) / sizeof(a[0])));
    }

    void CreateToolbar() {
        toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"",
                                   WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS | CCS_TOP |
                                       CCS_NODIVIDER,
                                   0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)IDC_TOOLBAR, App::Instance(), nullptr);
        SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(toolbar_, TB_SETIMAGELIST, 0, 0);
        SendMessageW(toolbar_, TB_SETBITMAPSIZE, 0, MAKELPARAM(0, 0));   // reine Textschaltflächen
        SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);
        // Beschriftungen (doppelt nullterminierte Liste)
        static const wchar_t kStrings[] = L"Neu vergleichen\0◀ Vorheriger\0Nächster ▶\0Groß/klein ignorieren\0"
                                          L"Leerraum ignorieren\0Leerzeilen ignorieren\0Binär\0";
        int first = (int)SendMessageW(toolbar_, TB_ADDSTRINGW, 0, (LPARAM)kStrings);
        struct Def {
            int id;
            BYTE style;
        };
        const Def defs[] = {{IDM_RECOMPARE, BTNS_BUTTON}, {0, BTNS_SEP},          {IDM_PREV, BTNS_BUTTON},
                            {IDM_NEXT, BTNS_BUTTON},      {0, BTNS_SEP},          {IDM_IGNORE_CASE, BTNS_CHECK},
                            {IDM_IGNORE_WS, BTNS_CHECK},  {IDM_IGNORE_BLANK, BTNS_CHECK}, {0, BTNS_SEP},
                            {IDM_FORCE_BINARY, BTNS_CHECK}};
        std::vector<TBBUTTON> buttons;
        int s = first;
        for (const Def& d : defs) {
            TBBUTTON b{};
            b.idCommand = d.id;
            b.fsStyle = d.style;
            if (d.style != BTNS_SEP) {
                b.iBitmap = I_IMAGENONE;
                b.fsState = TBSTATE_ENABLED;
                b.fsStyle |= BTNS_AUTOSIZE | BTNS_SHOWTEXT;
                b.iString = s++;
            }
            buttons.push_back(b);
        }
        SendMessageW(toolbar_, TB_ADDBUTTONSW, (WPARAM)buttons.size(), (LPARAM)buttons.data());
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    }

    void OnCreate() {
        LoadSettings();
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)App::SmallIcon());
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)App::BigIcon());
        UpdateTitle();
        CreateToolbar();
        status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd_,
                                  (HMENU)(INT_PTR)IDC_STATUSBAR, App::Instance(), nullptr);
        view_.Create(hwnd_, IDC_VIEW);
        view_.SetPaths(pathA_, pathB_);
        binInfo_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd_,
                                   (HMENU)(INT_PTR)IDC_BININFO, App::Instance(), nullptr);
        binList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                   WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd_,
                                   (HMENU)(INT_PTR)IDC_BINLIST, App::Instance(), nullptr);
        ListView_SetExtendedListViewStyle(binList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        CreateFonts();
        struct Col {
            const wchar_t* title;
            int width;
            int fmt;
        };
        const Col cols[] = {{L"Offset (hex)", 130, LVCFMT_LEFT},
                            {L"Byte A", 60, LVCFMT_CENTER},
                            {L"Byte B", 60, LVCFMT_CENTER},
                            {L"Zeichen A", 70, LVCFMT_CENTER},
                            {L"Zeichen B", 70, LVCFMT_CENTER},
                            {L"Offset (dezimal)", 130, LVCFMT_RIGHT}};
        for (int i = 0; i < (int)(sizeof(cols) / sizeof(cols[0])); ++i) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            c.fmt = cols[i].fmt;
            c.cx = DpiScale(hwnd_, cols[i].width);
            c.pszText = const_cast<wchar_t*>(cols[i].title);
            ListView_InsertColumn(binList_, i, &c);
        }
        accel_ = BuildAccelerators();
        App::RegisterAccelerator(hwnd_, accel_);
        UpdateUi();
    }

    void CreateFonts() {
        HFONT oldMono = monoFont_, oldUi = uiFont_;
        monoFont_ = CreateScaledFont(App::MonoFont(), hwnd_);
        uiFont_ = CreateScaledFont(App::UIFont(), hwnd_);
        view_.SetFont(monoFont_);
        SendMessageW(binInfo_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
        SendMessageW(binList_, WM_SETFONT, (WPARAM)monoFont_, TRUE);
        if (toolbar_) {
            SendMessageW(toolbar_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
            SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
        }
        if (oldMono) DeleteObject(oldMono);
        if (oldUi) DeleteObject(oldUi);
    }

    void UpdateTitle() {
        std::wstring t = L"Dateivergleich – " + PathFileName(pathA_) + L" ↔ " + PathFileName(pathB_);
        SetWindowTextW(hwnd_, t.c_str());
    }

    void Layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT tr{}, sr{};
        GetWindowRect(toolbar_, &tr);
        GetWindowRect(status_, &sr);
        int top = tr.bottom - tr.top;
        int bottom = rc.bottom - (sr.bottom - sr.top);
        int w = rc.right;
        // Statusleiste: Unterschiede | Kodierung A | Kodierung B | Hinweis
        int parts[4];
        int p0 = DpiScale(hwnd_, 300), p1 = DpiScale(hwnd_, 190), p2 = DpiScale(hwnd_, 190);
        parts[0] = std::min(p0, w);
        parts[1] = std::min(parts[0] + p1, w);
        parts[2] = std::min(parts[1] + p2, w);
        parts[3] = -1;
        SendMessageW(status_, SB_SETPARTS, 4, (LPARAM)parts);
        int h = std::max(0, bottom - top);
        if (binaryMode_) {
            int pad = DpiScale(hwnd_, 6);
            int infoH = DpiScale(hwnd_, 64);
            MoveWindow(binInfo_, pad, top + pad, std::max(0, w - 2 * pad), infoH, TRUE);
            int ly = top + pad + infoH + pad;
            MoveWindow(binList_, 0, ly, w, std::max(0, bottom - ly), TRUE);
        } else {
            MoveWindow(view_.Hwnd(), 0, top, w, h, TRUE);
        }
    }

    void ShowMode(bool binary) {
        binaryMode_ = binary;
        ShowWindow(view_.Hwnd(), binary ? SW_HIDE : SW_SHOW);
        ShowWindow(binInfo_, binary ? SW_SHOW : SW_HIDE);
        ShowWindow(binList_, binary ? SW_SHOW : SW_HIDE);
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void SetStatus(int part, const std::wstring& text) {
        SendMessageW(status_, SB_SETTEXTW, part, (LPARAM)text.c_str());
    }

    // ---- Vergleich starten / Ergebnis übernehmen ----
    void StartCompare(bool keepPos) {
        StopWorker();
        keepPos_ = keepPos && model_ != nullptr;
        busy_ = true;
        cancel_ = false;
        unsigned gen = ++gen_;
        CompareOptions o = opt_;
        o.tabWidth = std::clamp(App::Opt().editorTabWidth, 1, 16);
        std::wstring a = pathA_, b = pathB_;
        HWND h = hwnd_;
        if (!keepPos_) {
            view_.SetModel(nullptr, false);
            model_.reset();
            if (!binaryMode_) view_.SetMessage(L"Vergleiche …");
        }
        SetStatus(0, L"Vergleiche …");
        SetStatus(3, L"");
        worker_ = std::thread([this, a, b, o, h, gen]() {
            std::unique_ptr<CompareResult> r = RunCompare(a, b, o, cancel_, h, gen);
            {
                std::lock_guard<std::mutex> lock(mx_);
                pending_ = std::move(r);
            }
            PostMessageW(h, WM_CF_DONE, gen, 0);
        });
    }

    void StopWorker() {
        if (worker_.joinable()) {
            cancel_ = true;
            worker_.join();
        }
        busy_ = false;
    }

    void OnDone() {
        if (worker_.joinable()) worker_.join();
        busy_ = false;
        std::unique_ptr<CompareResult> r;
        {
            std::lock_guard<std::mutex> lock(mx_);
            r = std::move(pending_);
        }
        if (!r || r->cancelled) {
            SetStatus(0, L"Vergleich abgebrochen.");
            return;
        }
        if (!r->error.empty()) {
            ShowMode(false);
            view_.SetModel(nullptr, false);
            model_.reset();
            view_.SetMessage(L"Fehler beim Lesen der Dateien.");
            SetStatus(0, L"Fehler");
            MsgError(hwnd_, r->error);
            return;
        }
        note_ = r->note;
        if (r->text) {
            bool keep = keepPos_;
            // Ansicht zuerst umhängen, dann das alte Modell freigeben
            std::unique_ptr<TextModel> old = std::move(model_);
            model_ = std::move(r->model);
            view_.SetModel(model_.get(), keep);
            old.reset();
            bin_ = BinaryModel();
            ListView_SetItemCountEx(binList_, 0, 0);
            ShowMode(false);
            curBlock_ = -1;
            if (!keep && !model_->blocks.empty()) {
                curBlock_ = 0;
                view_.SetCurrentBlock(0, true);
            }
        } else {
            view_.SetModel(nullptr, false);
            model_.reset();
            bin_ = std::move(r->bin);
            ListView_SetItemCountEx(binList_, (int)bin_.diffs.size(), 0);
            InvalidateRect(binList_, nullptr, TRUE);
            ShowMode(true);
            UpdateBinaryInfo();
        }
        UpdateStatus();
        UpdateUi();
    }

    void UpdateBinaryInfo() {
        std::wstring t = L"A: " + pathA_ + L"  –  " + FormatSizeBytes(bin_.sizeA) + L" Bytes\n";
        t += L"B: " + pathB_ + L"  –  " + FormatSizeBytes(bin_.sizeB) + L" Bytes\n";
        uint64_t common = std::min(bin_.sizeA, bin_.sizeB);
        if (bin_.diffCount == 0 && bin_.sizeA == bin_.sizeB) {
            t += L"Die Dateien sind identisch.";
        } else {
            t += L"Unterschiedliche Bytes: " + FormatSizeBytes(bin_.diffCount) + L" von " + FormatSizeBytes(common) +
                 L" verglichenen";
            if (bin_.sizeA != bin_.sizeB) {
                bool aLonger = bin_.sizeA > bin_.sizeB;
                uint64_t d = aLonger ? bin_.sizeA - bin_.sizeB : bin_.sizeB - bin_.sizeA;
                t += std::wstring(L" · Datei ") + (aLonger ? L"A" : L"B") + L" ist " + FormatSizeBytes(d) +
                     L" Bytes länger";
            }
            if (bin_.diffCount > bin_.diffs.size())
                t += L"\nAngezeigt werden die ersten " + IntToStrGrouped(bin_.diffs.size()) + L" Abweichungen.";
        }
        SetWindowTextW(binInfo_, t.c_str());
    }

    void UpdateStatus() {
        if (busy_) return;
        if (binaryMode_) {
            if (bin_.diffCount == 0 && bin_.sizeA == bin_.sizeB)
                SetStatus(0, L"Keine Unterschiede (binär identisch)");
            else
                SetStatus(0, L"Binärvergleich: " + IntToStrGrouped(bin_.diffCount) + L" Bytes unterschiedlich" +
                                 (bin_.sizeA != bin_.sizeB ? L", Größen verschieden" : L""));
            SetStatus(1, L"A: binär");
            SetStatus(2, L"B: binär");
            SetStatus(3, note_);
            return;
        }
        if (!model_) return;
        const TextModel& m = *model_;
        size_t n = m.blocks.size();
        std::wstring s;
        if (n == 0) {
            s = m.bytesIdentical ? L"Keine Unterschiede – Dateien sind identisch"
                                 : L"Keine Unterschiede (unter Berücksichtigung der Optionen)";
        } else {
            if (curBlock_ >= 0)
                s = L"Unterschied " + IntToStrGrouped((unsigned long long)curBlock_ + 1) + L" von " + IntToStrGrouped(n);
            else
                s = IntToStrGrouped(n) + (n == 1 ? L" Unterschied" : L" Unterschiede");
            s += L"  (" + IntToStrGrouped((unsigned long long)m.changedRows) + L" geändert, " +
                 IntToStrGrouped((unsigned long long)m.leftOnlyRows) + L" nur A, " +
                 IntToStrGrouped((unsigned long long)m.rightOnlyRows) + L" nur B)";
        }
        SetStatus(0, s);
        auto enc = [](TextEncoding e, LineEnding l, bool empty) {
            std::wstring r = EncodingName(e);
            if (!empty) r += std::wstring(L" · ") + LineEndingName(l);
            return r;
        };
        SetStatus(1, L"A: " + enc(m.encL, m.eolL, m.emptyL));
        SetStatus(2, L"B: " + enc(m.encR, m.eolR, m.emptyR));
        std::wstring note = note_;
        auto add = [&note](const std::wstring& x) {
            if (!note.empty()) note += L" · ";
            note += x;
        };
        if (m.approximate) add(L"Zeitlimit erreicht – Ergebnis vereinfacht");
        if (m.charDiffIncomplete) add(L"Zeichenweise Hervorhebung unvollständig");
        if (opt_.ignoreCase || opt_.ignoreWs || opt_.ignoreBlank) {
            std::wstring ig;
            if (opt_.ignoreCase) ig += L"Groß/klein";
            if (opt_.ignoreWs) ig += std::wstring(ig.empty() ? L"" : L", ") + L"Leerraum";
            if (opt_.ignoreBlank) ig += std::wstring(ig.empty() ? L"" : L", ") + L"Leerzeilen";
            add(L"Ignoriert: " + ig);
        }
        SetStatus(3, note);
    }

    void UpdateUi() {
        HMENU menu = GetMenu(hwnd_);
        auto check = [&](int id, bool on) {
            CheckMenuItem(menu, (UINT)id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            SendMessageW(toolbar_, TB_CHECKBUTTON, (WPARAM)id, MAKELPARAM(on ? TRUE : FALSE, 0));
        };
        check(IDM_IGNORE_CASE, opt_.ignoreCase);
        check(IDM_IGNORE_WS, opt_.ignoreWs);
        check(IDM_IGNORE_BLANK, opt_.ignoreBlank);
        check(IDM_FORCE_BINARY, opt_.forceBinary);
        bool nav = !binaryMode_ && model_ && !model_->blocks.empty();
        const int navIds[] = {IDM_FIRST, IDM_PREV, IDM_NEXT, IDM_LAST};
        for (int id : navIds) {
            EnableMenuItem(menu, (UINT)id, MF_BYCOMMAND | (nav ? MF_ENABLED : MF_GRAYED));
            SendMessageW(toolbar_, TB_ENABLEBUTTON, (WPARAM)id, MAKELPARAM(nav ? TRUE : FALSE, 0));
        }
    }

    void Navigate(int cmd) {
        if (binaryMode_ || !model_ || model_->blocks.empty()) {
            MessageBeep(MB_OK);
            return;
        }
        const auto& blocks = model_->blocks;
        int n = (int)blocks.size();
        int top = view_.TopRow(), vis = view_.VisibleRows();
        // Ist der aktuelle Unterschied nicht mehr sichtbar, gilt die Bildlaufposition als Ausgangspunkt
        bool curVisible = curBlock_ >= 0 && curBlock_ < n && blocks[(size_t)curBlock_].last > top &&
                          blocks[(size_t)curBlock_].first < top + vis;
        int target = curBlock_;
        switch (cmd) {
        case IDM_FIRST: target = 0; break;
        case IDM_LAST: target = n - 1; break;
        case IDM_NEXT:
            if (curVisible) {
                target = curBlock_ + 1;
            } else {
                target = n;
                for (int i = 0; i < n; ++i)
                    if (blocks[(size_t)i].first >= top) {
                        target = i;
                        break;
                    }
            }
            break;
        case IDM_PREV:
            if (curVisible) {
                target = curBlock_ - 1;
            } else {
                target = -1;
                for (int i = n - 1; i >= 0; --i)
                    if (blocks[(size_t)i].first < top) {
                        target = i;
                        break;
                    }
            }
            break;
        }
        if (target < 0 || target >= n) {
            MessageBeep(MB_OK);
            return;
        }
        curBlock_ = target;
        view_.SetCurrentBlock(target, true);
        UpdateStatus();
    }

    void OnCommand(int id) {
        switch (id) {
        case IDM_RECOMPARE: StartCompare(true); break;
        case IDM_EDIT_LEFT: OpenTextEditor(pathA_); break;
        case IDM_EDIT_RIGHT: OpenTextEditor(pathB_); break;
        case IDM_SWAP:
            std::swap(pathA_, pathB_);
            view_.SetPaths(pathA_, pathB_);
            UpdateTitle();
            StartCompare(false);
            break;
        case IDM_CLOSE: DestroyWindow(hwnd_); break;
        case IDM_IGNORE_CASE:
        case IDM_IGNORE_WS:
        case IDM_IGNORE_BLANK:
        case IDM_FORCE_BINARY:
            if (id == IDM_IGNORE_CASE) opt_.ignoreCase = !opt_.ignoreCase;
            if (id == IDM_IGNORE_WS) opt_.ignoreWs = !opt_.ignoreWs;
            if (id == IDM_IGNORE_BLANK) opt_.ignoreBlank = !opt_.ignoreBlank;
            if (id == IDM_FORCE_BINARY) opt_.forceBinary = !opt_.forceBinary;
            UpdateUi();
            StartCompare(id != IDM_FORCE_BINARY);
            break;
        case IDM_FIRST:
        case IDM_PREV:
        case IDM_NEXT:
        case IDM_LAST: Navigate(id); break;
        }
    }

    LRESULT OnNotify(NMHDR* nm) {
        if (nm->hwndFrom == binList_ && nm->code == LVN_GETDISPINFOW) {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
            if (!(di->item.mask & LVIF_TEXT)) return 0;
            size_t i = (size_t)di->item.iItem;
            if (i >= bin_.diffs.size()) return 0;
            const BinDiff& d = bin_.diffs[i];
            uint64_t maxSize = std::max(bin_.sizeA, bin_.sizeB);
            switch (di->item.iSubItem) {
            case 0: lvBuf_ = Hex(d.offset, maxSize > 0xFFFFFFFFull ? 16 : 8); break;
            case 1: lvBuf_ = Hex(d.a, 2); break;
            case 2: lvBuf_ = Hex(d.b, 2); break;
            case 3: lvBuf_ = ByteChar(d.a); break;
            case 4: lvBuf_ = ByteChar(d.b); break;
            case 5: lvBuf_ = IntToStrGrouped(d.offset); break;
            default: lvBuf_.clear(); break;
            }
            di->item.pszText = lvBuf_.data();
            return 0;
        }
        if (nm->code == TTN_GETDISPINFOW) {   // kommt vom Tooltip-Fenster der Werkzeugleiste
            auto* tt = reinterpret_cast<NMTTDISPINFOW*>(nm);
            switch (tt->hdr.idFrom) {
            case IDM_RECOMPARE: tt->lpszText = const_cast<wchar_t*>(L"Neu vergleichen (F5)"); break;
            case IDM_PREV: tt->lpszText = const_cast<wchar_t*>(L"Vorheriger Unterschied (F7, Alt+↑)"); break;
            case IDM_NEXT: tt->lpszText = const_cast<wchar_t*>(L"Nächster Unterschied (F8, Alt+↓)"); break;
            case IDM_FORCE_BINARY: tt->lpszText = const_cast<wchar_t*>(L"Binärvergleich erzwingen"); break;
            }
            return 0;
        }
        return 0;
    }

    void LoadSettings() {
        Config& c = App::Cfg();
        opt_.ignoreCase = c.GetBool(kSection, L"IgnoreCase", false);
        opt_.ignoreWs = c.GetBool(kSection, L"IgnoreWhitespace", false);
        opt_.ignoreBlank = c.GetBool(kSection, L"IgnoreBlankLines", false);
    }

    void SaveSettings() {
        Config& c = App::Cfg();
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (GetWindowPlacement(hwnd_, &wp)) {
            c.SetInt(kSection, L"Left", wp.rcNormalPosition.left);
            c.SetInt(kSection, L"Top", wp.rcNormalPosition.top);
            c.SetInt(kSection, L"Right", wp.rcNormalPosition.right);
            c.SetInt(kSection, L"Bottom", wp.rcNormalPosition.bottom);
            c.SetBool(kSection, L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED);
        }
        c.SetBool(kSection, L"IgnoreCase", opt_.ignoreCase);
        c.SetBool(kSection, L"IgnoreWhitespace", opt_.ignoreWs);
        c.SetBool(kSection, L"IgnoreBlankLines", opt_.ignoreBlank);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        CompareWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<CompareWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = h;
            if (self->ncCreateFlag_) *self->ncCreateFlag_ = true;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = reinterpret_cast<CompareWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            LRESULT r = DefWindowProcW(h, msg, wp, lp);
            delete self;
            return r;
        }
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE: OnCreate(); return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = DpiScale(hwnd_, 420);
            mmi->ptMinTrackSize.y = DpiScale(hwnd_, 260);
            return 0;
        }
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            CreateFonts();
            Layout();
            return 0;
        }
        case WM_SETFOCUS:
            SetFocus(binaryMode_ ? binList_ : view_.Hwnd());
            return 0;
        case WM_COMMAND: OnCommand(LOWORD(wp)); return 0;
        case WM_NOTIFY: return OnNotify(reinterpret_cast<NMHDR*>(lp));
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == binInfo_) {
                SetBkMode((HDC)wp, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        case WM_CF_PROGRESS:
            if ((unsigned)wp == gen_ && busy_) SetStatus(0, L"Binärvergleich … " + IntToStr((long long)lp) + L" %");
            return 0;
        case WM_CF_DONE:
            if ((unsigned)wp == gen_) OnDone();
            return 0;
        case WM_DESTROY:
            StopWorker();
            SaveSettings();
            App::UnregisterAccelerator(hwnd_);
            if (accel_) DestroyAcceleratorTable(accel_);
            accel_ = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

public:
    ~CompareWindow() {
        StopWorker();
        if (monoFont_) DeleteObject(monoFont_);
        if (uiFont_) DeleteObject(uiFont_);
    }

private:
    HWND hwnd_ = nullptr;
    bool* ncCreateFlag_ = nullptr;   // nur während CreateWindowExW gültig
    HWND toolbar_ = nullptr;
    HWND status_ = nullptr;
    HWND binInfo_ = nullptr;
    HWND binList_ = nullptr;
    DiffView view_;
    HACCEL accel_ = nullptr;
    HFONT monoFont_ = nullptr;
    HFONT uiFont_ = nullptr;

    std::wstring pathA_, pathB_;
    CompareOptions opt_;
    std::unique_ptr<TextModel> model_;
    BinaryModel bin_;
    bool binaryMode_ = false;
    bool busy_ = false;
    bool keepPos_ = false;
    int curBlock_ = -1;
    std::wstring note_;
    std::wstring lvBuf_;

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    unsigned gen_ = 0;
    std::mutex mx_;
    std::unique_ptr<CompareResult> pending_;   // geschützt durch mx_
};

// Externes Vergleichsprogramm starten. Platzhalter %1/%2 werden ersetzt, sonst Pfade angehängt.
void RunExternalTool(HWND owner, const std::wstring& tool, const std::wstring& a, const std::wstring& b) {
    std::wstring qa = L"\"" + a + L"\"", qb = L"\"" + b + L"\"";
    std::wstring t = Trim(tool), cmd;
    if (t.find(L"%1") != std::wstring::npos) {
        cmd = ReplaceAll(ReplaceAll(t, L"%1", qa), L"%2", qb);
    } else {
        // Programmpfad in Anführungszeichen setzen, falls nötig (nicht bei bereits zitierten Befehlen)
        if (!t.empty() && t[0] != L'"' && FileExists(t)) t = L"\"" + t + L"\"";
        cmd = t + L" " + qa + L" " + qb;
    }
    if (!RunProcess(cmd, L"")) MsgError(owner, L"Das Vergleichsprogramm konnte nicht gestartet werden:\n" + cmd +
                                                   L"\n\n" + LastErrorMessage());
}

} // namespace

void CompareFiles(HWND owner, const std::wstring& fileA, const std::wstring& fileB) {
    if (!App::Opt().compareTool.empty()) {
        RunExternalTool(owner, App::Opt().compareTool, fileA, fileB);
        return;
    }
    if (!FileExists(fileA)) {
        MsgError(owner, L"Datei nicht gefunden:\n" + fileA);
        return;
    }
    if (!FileExists(fileB)) {
        MsgError(owner, L"Datei nicht gefunden:\n" + fileB);
        return;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    // Das Objekt löscht sich selbst (WM_NCDESTROY bzw. bei Fehlschlag in Create)
    auto* w = new CompareWindow(fileA, fileB);
    if (!w->Create()) MsgError(owner, L"Das Vergleichsfenster konnte nicht geöffnet werden.");
}

} // namespace qf
