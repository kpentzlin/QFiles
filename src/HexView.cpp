// HexView.cpp – Modul B: Hex-Anzeige-/Bearbeitungs-Steuerelement und Hex-Editor-Fenster.
//
// Die Datei wird nie komplett geladen: Das Steuerelement hält ein Lese-Handle offen und liest 64-KB-Seiten
// (kleiner LRU-Zwischenspeicher). Änderungen werden in einer Tabelle Offset -> neuer Wert gehalten und beim
// Speichern einzeln (zusammenhängende Bereiche am Stück) in die bestehende Datei geschrieben.

#include "HexView.h"
#include "App.h"
#include "Dialog.h"
#include "Modules.h"
#include "Util.h"

#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <vector>

namespace qf {

namespace {

constexpr wchar_t kHexViewClass[] = L"QFilesHexView";
constexpr wchar_t kHexEditorClass[] = L"QFilesHexEditor";

constexpr uint32_t kPageSize = 64 * 1024;          // Seitengröße des Lese-Zwischenspeichers
constexpr size_t kMaxPages = 32;                    // max. 2 MB zwischengespeichert
constexpr uint64_t kMaxCopyBytes = 16ull * 1024 * 1024;
constexpr size_t kSearchChunk = 4 * 1024 * 1024;    // Suchblockgröße
constexpr UINT_PTR kAutoScrollTimer = 1;

// ===================== Allgemeine Hilfsfunktionen =====================

std::wstring Hex(uint64_t v, int digits) {
    static const wchar_t kDigits[] = L"0123456789ABCDEF";
    std::wstring r((size_t)digits, L'0');
    for (int i = digits - 1; i >= 0; --i) {
        r[(size_t)i] = kDigits[v & 0xF];
        v >>= 4;
    }
    return r;
}

int HexDigitValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

UINT SystemDpi() {
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? (UINT)dpi : 96;
}

// Erzeugt eine Kopie der (für die System-DPI erzeugten) Schrift passend zur DPI des Fensters.
HFONT CreateScaledFont(HFONT base, UINT dpi) {
    LOGFONTW lf{};
    if (!base || !GetObjectW(base, sizeof(lf), &lf)) {
        lf.lfHeight = -13;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        lstrcpynW(lf.lfFaceName, L"Consolas", LF_FACESIZE);
    }
    UINT sys = SystemDpi();
    if (dpi && dpi != sys) lf.lfHeight = MulDiv(lf.lfHeight, (int)dpi, (int)sys);
    return CreateFontIndirectW(&lf);
}

// Zeichendarstellung CP1252; nicht druckbare Bytes als '.'
struct CharTable {
    wchar_t show[256];
    CharTable() {
        for (int b = 0; b < 256; ++b) {
            wchar_t w = L'.';
            if (b >= 0x20 && b < 0x7F) {
                w = (wchar_t)b;
            } else if (b >= 0xA0) {
                char c = (char)(unsigned char)b;
                wchar_t out = 0;
                if (MultiByteToWideChar(1252, 0, &c, 1, &out, 1) == 1 && out >= 0x20) w = out;
                if (b == 0xA0) w = L' ';
                if (b == 0xAD) w = L'-';
            } else if (b >= 0x80) {
                char c = (char)(unsigned char)b;
                wchar_t out = 0;
                // In CP1252 nicht belegte Codes (0x81, 0x8D, 0x8F, 0x90, 0x9D) liefern C1-Steuerzeichen -> '.'
                if (MultiByteToWideChar(1252, 0, &c, 1, &out, 1) == 1 && !(out >= 0x80 && out <= 0x9F)) w = out;
            }
            show[b] = w;
        }
    }
};

const CharTable& Chars() {
    static const CharTable t;
    return t;
}

// Zeichen -> Byte (CP1252) für die Eingabe in der Zeichenspalte
bool CharToByte(wchar_t ch, uint8_t& out) {
    if (ch >= 0x20 && ch < 0x7F) {
        out = (uint8_t)ch;
        return true;
    }
    if (ch < 0x80) return false;
    for (int b = 0x80; b < 256; ++b) {
        if (Chars().show[b] == ch) {
            out = (uint8_t)b;
            return true;
        }
    }
    return false;
}

// ===================== Zustand des Steuerelements =====================

struct Page {
    uint64_t index = UINT64_MAX;
    uint64_t lastUse = 0;
    std::vector<uint8_t> data;
};

struct UndoEntry {
    uint64_t offset;
    bool hadMod;         // vorher bereits geändert?
    uint8_t prevValue;   // vorheriger geänderter Wert
    uint64_t caret;      // Cursor vor der Änderung
    int nibble;
    bool charCol;
};

struct HexState {
    HWND hwnd = nullptr;
    bool readOnly = true;
    std::wstring path;
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
    std::wstring lastError;

    std::vector<Page> pages;
    uint64_t useCounter = 0;
    std::map<uint64_t, uint8_t> mods;   // geänderte Bytes
    std::vector<UndoEntry> undo;

    uint64_t caret = 0;
    uint64_t anchor = 0;
    bool selecting = false;
    bool charCol = false;
    int nibble = 0;                     // 0 = obere, 1 = untere Hexziffer
    uint64_t topLine = 0;
    int leftCol = 0;

    HFONT font = nullptr;
    int cw = 8, ch = 16, margin = 4;
    int wheelAccum = 0;
    bool dragging = false;
    int autoScrollDir = 0;

    // Suche
    std::vector<uint8_t> pattern;
    bool patternCase = true;
    std::wstring patternText;
};

HexState* GetState(HWND h) { return h ? reinterpret_cast<HexState*>(GetWindowLongPtrW(h, GWLP_USERDATA)) : nullptr; }

// Zuletzt verwendete Sucheinstellungen (für alle HexViews gemeinsam)
struct FindSettings {
    std::wstring text;
    int type = 0;            // 0 = Hex-Bytes, 1 = Text ANSI, 2 = Text UTF-8, 3 = Text UTF-16 LE
    bool matchCase = false;
    bool backward = false;
};
FindSettings g_find;

// ---------- Dateizugriff ----------

// Liest n Bytes ab off direkt aus der Datei (ohne Änderungen); fehlende Bytes (Datei verkürzt) = 0.
void ReadAt(HexState& s, uint64_t off, uint8_t* buf, size_t n) {
    size_t done = 0;
    if (s.file != INVALID_HANDLE_VALUE) {
        while (done < n) {
            OVERLAPPED ov{};
            uint64_t p = off + done;
            ov.Offset = (DWORD)(p & 0xFFFFFFFFu);
            ov.OffsetHigh = (DWORD)(p >> 32);
            DWORD chunk = (DWORD)std::min<size_t>(n - done, 16u * 1024 * 1024);
            DWORD got = 0;
            if (!ReadFile(s.file, buf + done, chunk, &got, &ov) || got == 0) break;
            done += got;
        }
    }
    if (done < n) memset(buf + done, 0, n - done);
}

void ClearCache(HexState& s) {
    s.pages.clear();
    s.useCounter = 0;
}

const uint8_t* PageData(HexState& s, uint64_t index) {
    for (auto& p : s.pages)
        if (p.index == index) {
            p.lastUse = ++s.useCounter;
            return p.data.data();
        }
    Page* target = nullptr;
    if (s.pages.size() < kMaxPages) {
        s.pages.emplace_back();
        target = &s.pages.back();
        target->data.resize(kPageSize);
    } else {
        target = &*std::min_element(s.pages.begin(), s.pages.end(),
                                    [](const Page& a, const Page& b) { return a.lastUse < b.lastUse; });
    }
    target->index = index;
    target->lastUse = ++s.useCounter;
    uint64_t start = index * kPageSize;
    size_t n = start < s.size ? (size_t)std::min<uint64_t>(kPageSize, s.size - start) : 0;
    ReadAt(s, start, target->data.data(), n);
    if (n < kPageSize) memset(target->data.data() + n, 0, kPageSize - n);
    return target->data.data();
}

uint8_t OriginalByte(HexState& s, uint64_t off) { return PageData(s, off / kPageSize)[off % kPageSize]; }

uint8_t GetByte(HexState& s, uint64_t off) {
    auto it = s.mods.find(off);
    if (it != s.mods.end()) return it->second;
    return OriginalByte(s, off);
}

// Liest einen Bereich über den Zwischenspeicher (für die Anzeige) inkl. Änderungen; mod[] markiert geänderte Bytes.
void ReadCached(HexState& s, uint64_t off, uint8_t* buf, bool* mod, size_t n) {
    size_t done = 0;
    while (done < n) {
        uint64_t p = off + done;
        const uint8_t* page = PageData(s, p / kPageSize);
        size_t inPage = (size_t)(p % kPageSize);
        size_t take = std::min<size_t>(n - done, kPageSize - inPage);
        memcpy(buf + done, page + inPage, take);
        done += take;
    }
    if (mod) memset(mod, 0, n * sizeof(bool));
    for (auto it = s.mods.lower_bound(off); it != s.mods.end() && it->first < off + n; ++it) {
        buf[it->first - off] = it->second;
        if (mod) mod[it->first - off] = true;
    }
}

// Liest einen (großen) Bereich direkt inkl. Änderungen (für Suche/Kopieren, ohne den Zwischenspeicher zu verdrängen).
void ReadDirect(HexState& s, uint64_t off, uint8_t* buf, size_t n) {
    ReadAt(s, off, buf, n);
    for (auto it = s.mods.lower_bound(off); it != s.mods.end() && it->first < off + n; ++it) buf[it->first - off] = it->second;
}

// ---------- Geometrie ----------

int OffDigits(const HexState& s) { return s.size > 0xFFFFFFFFull ? 16 : 8; }
int ColHex(const HexState& s) { return OffDigits(s) + 2; }
int HexPos(int i) { return i * 3 + (i >= 8 ? 1 : 0); }
int ColChar(const HexState& s) { return ColHex(s) + 16 * 3 + 2; }
int LineChars(const HexState& s) { return ColChar(s) + 16 + 1; }
int HeaderHeight(const HexState& s) { return s.ch + DpiScale(s.hwnd, 4); }
uint64_t TotalLines(const HexState& s) { return (s.size + 15) / 16; }

int VisibleLines(const HexState& s) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    int h = rc.bottom - HeaderHeight(s);
    return std::max(1, h / std::max(1, s.ch));
}

int VisibleCols(const HexState& s) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    return std::max(1, (int)(rc.right - s.margin) / std::max(1, s.cw));
}

int XCol(const HexState& s, int col) { return s.margin + (col - s.leftCol) * s.cw; }

uint64_t MaxTop(const HexState& s) {
    uint64_t total = TotalLines(s);
    uint64_t vis = (uint64_t)VisibleLines(s);
    return total > vis ? total - vis : 0;
}

// Scrollleisten sind 32 Bit: bei sehr großen Dateien wird der Bereich skaliert.
uint64_t ScrollFactor(uint64_t maxTop) { return maxTop > 0x3FFFFFFFull ? maxTop / 0x3FFFFFFFull + 1 : 1; }

void UpdateScrollBars(HexState& s) {
    uint64_t maxTop = MaxTop(s);
    if (s.topLine > maxTop) s.topLine = maxTop;
    uint64_t f = ScrollFactor(maxTop);
    int vis = VisibleLines(s);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (int)(maxTop / f) + vis - 1;
    si.nPage = (UINT)vis;
    si.nPos = (int)(s.topLine / f);
    SetScrollInfo(s.hwnd, SB_VERT, &si, TRUE);

    int cols = VisibleCols(s);
    int lineW = LineChars(s);
    int maxLeft = std::max(0, lineW - cols);
    if (s.leftCol > maxLeft) s.leftCol = maxLeft;
    if (s.leftCol < 0) s.leftCol = 0;
    si.nMax = lineW - 1;
    si.nPage = (UINT)cols;
    si.nPos = s.leftCol;
    SetScrollInfo(s.hwnd, SB_HORZ, &si, TRUE);
}

void Notify(HexState& s) {
    HWND parent = GetParent(s.hwnd);
    if (parent)
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.hwnd), HVN_STATUS), (LPARAM)s.hwnd);
}

void Redraw(HexState& s) { InvalidateRect(s.hwnd, nullptr, FALSE); }

void SetTop(HexState& s, uint64_t top) {
    uint64_t maxTop = MaxTop(s);
    if (top > maxTop) top = maxTop;
    if (top != s.topLine) {
        s.topLine = top;
        UpdateScrollBars(s);
        Redraw(s);
    }
}

void ScrollLines(HexState& s, long long delta) {
    if (delta < 0) {
        uint64_t d = (uint64_t)(-delta);
        SetTop(s, s.topLine > d ? s.topLine - d : 0);
    } else {
        SetTop(s, s.topLine + (uint64_t)delta);
    }
}

void EnsureVisibleOffset(HexState& s, uint64_t off) {
    if (s.size == 0) return;
    uint64_t line = off / 16;
    int vis = VisibleLines(s);
    if (line < s.topLine)
        SetTop(s, line);
    else if (line >= s.topLine + (uint64_t)vis)
        SetTop(s, line - (uint64_t)vis + 1);
    // Waagerecht: aktive Spalte sichtbar machen
    int i = (int)(off % 16);
    int col = s.charCol ? ColChar(s) + i : ColHex(s) + HexPos(i);
    int cols = VisibleCols(s);
    int left = s.leftCol;
    if (col < left) left = col;
    if (col + 2 > left + cols) left = col + 2 - cols;
    if (left != s.leftCol) {
        s.leftCol = std::max(0, left);
        UpdateScrollBars(s);
        Redraw(s);
    }
}

void GetSelection(const HexState& s, uint64_t& a, uint64_t& b) {
    a = std::min(s.anchor, s.caret);
    b = std::max(s.anchor, s.caret);
}

void MoveCaret(HexState& s, uint64_t pos, bool extend) {
    if (s.size == 0) return;
    if (pos >= s.size) pos = s.size - 1;
    if (extend) {
        if (!s.selecting) {
            s.selecting = true;
            s.anchor = s.caret;
        }
    } else {
        s.selecting = false;
        s.anchor = pos;
    }
    s.caret = pos;
    s.nibble = 0;
    EnsureVisibleOffset(s, pos);
    Redraw(s);
    Notify(s);
}

// ---------- Öffnen / Schließen ----------

void CloseFile(HexState& s) {
    if (s.file != INVALID_HANDLE_VALUE) CloseHandle(s.file);
    s.file = INVALID_HANDLE_VALUE;
    s.path.clear();
    s.size = 0;
    ClearCache(s);
    s.mods.clear();
    s.undo.clear();
    s.caret = s.anchor = 0;
    s.selecting = false;
    s.nibble = 0;
    s.topLine = 0;
    s.leftCol = 0;
}

bool OpenFile(HexState& s, const std::wstring& path) {
    CloseFile(s);
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        s.lastError = LastErrorMessage();
        UpdateScrollBars(s);
        Redraw(s);
        Notify(s);
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    s.file = h;
    s.path = path;
    s.size = (uint64_t)sz.QuadPart;
    s.lastError.clear();
    UpdateScrollBars(s);
    Redraw(s);
    Notify(s);
    return true;
}

// ---------- Bearbeitung ----------

void SetByte(HexState& s, uint64_t off, uint8_t value) {
    auto it = s.mods.find(off);
    UndoEntry e{off, it != s.mods.end(), it != s.mods.end() ? it->second : (uint8_t)0, s.caret, s.nibble, s.charCol};
    s.undo.push_back(e);
    if (value == OriginalByte(s, off))
        s.mods.erase(off);
    else
        s.mods[off] = value;
}

bool Undo(HexState& s) {
    if (s.undo.empty()) return false;
    UndoEntry e = s.undo.back();
    s.undo.pop_back();
    if (e.hadMod)
        s.mods[e.offset] = e.prevValue;
    else
        s.mods.erase(e.offset);
    s.caret = std::min<uint64_t>(e.caret, s.size ? s.size - 1 : 0);
    s.anchor = s.caret;
    s.selecting = false;
    s.nibble = e.nibble;
    s.charCol = e.charCol;
    EnsureVisibleOffset(s, s.caret);
    Redraw(s);
    Notify(s);
    return true;
}

bool Save(HexState& s) {
    if (s.mods.empty()) return true;
    HWND owner = GetAncestor(s.hwnd, GA_ROOT);
    HANDLE w = CreateFileW(LongPath(s.path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) {
        MsgError(owner, L"Die Datei „" + PathFileName(s.path) + L"“ kann nicht gespeichert werden:\n" + LastErrorMessage());
        return false;
    }
    LARGE_INTEGER cur{};
    GetFileSizeEx(w, &cur);
    if (!s.mods.empty() && s.mods.rbegin()->first >= (uint64_t)cur.QuadPart) {
        CloseHandle(w);
        MsgError(owner, L"Die Datei „" + PathFileName(s.path) +
                            L"“ wurde zwischenzeitlich verkürzt. Die Änderungen können nicht gespeichert werden.");
        return false;
    }
    bool ok = true;
    DWORD err = 0;
    std::vector<uint8_t> run;
    auto it = s.mods.begin();
    while (it != s.mods.end() && ok) {
        uint64_t start = it->first;
        run.clear();
        uint64_t next = start;
        while (it != s.mods.end() && it->first == next && run.size() < (1u << 20)) {
            run.push_back(it->second);
            ++next;
            ++it;
        }
        OVERLAPPED ov{};
        ov.Offset = (DWORD)(start & 0xFFFFFFFFu);
        ov.OffsetHigh = (DWORD)(start >> 32);
        DWORD written = 0;
        if (!WriteFile(w, run.data(), (DWORD)run.size(), &written, &ov) || written != run.size()) {
            ok = false;
            err = GetLastError();
        }
    }
    CloseHandle(w);
    if (!ok) {
        MsgError(owner, L"Fehler beim Schreiben von „" + PathFileName(s.path) + L"“:\n" + LastErrorMessage(err));
        ClearCache(s);
        Redraw(s);
        return false;
    }
    LogOperation(L"Hex-Editor: " + s.path + L" gespeichert (" + IntToStrGrouped(s.mods.size()) + L" geänderte Bytes)");
    s.mods.clear();
    s.undo.clear();
    ClearCache(s);
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(s.file, &sz)) s.size = (uint64_t)sz.QuadPart;
    UpdateScrollBars(s);
    Redraw(s);
    Notify(s);
    return true;
}

// ---------- Kopieren ----------

void Copy(HexState& s) {
    if (s.size == 0) return;
    uint64_t a = s.caret, b = s.caret;
    if (s.selecting) GetSelection(s, a, b);
    uint64_t n = b - a + 1;
    HWND owner = GetAncestor(s.hwnd, GA_ROOT);
    if (n > kMaxCopyBytes) {
        MsgError(owner, L"Die Markierung ist zu groß zum Kopieren (höchstens " + FormatSize(kMaxCopyBytes) + L").");
        return;
    }
    std::vector<uint8_t> buf((size_t)n);
    ReadDirect(s, a, buf.data(), (size_t)n);
    std::wstring text;
    if (s.charCol) {
        text.reserve((size_t)n);
        for (uint8_t c : buf) {
            if (c == 9 || c == 10 || c == 13)
                text += (wchar_t)c;
            else
                text += Chars().show[c];
        }
    } else {
        text.reserve((size_t)n * 3 + (size_t)n / 8);
        for (size_t i = 0; i < buf.size(); ++i) {
            if (i) text += (i % 16 == 0) ? L"\r\n" : L" ";
            text += Hex(buf[i], 2);
        }
    }
    ClipboardSetText(owner, text);
}

void SelectAll(HexState& s) {
    if (s.size == 0) return;
    s.anchor = 0;
    s.caret = s.size - 1;
    s.selecting = true;
    s.nibble = 0;
    EnsureVisibleOffset(s, s.caret);
    Redraw(s);
    Notify(s);
}

// ---------- Gehe zu ----------

// Offset analysieren: dezimal, 0x…/$… oder …h hexadezimal (Hex-Buchstaben => hexadezimal), +/- relativ zu base.
bool ParseOffset(const std::wstring& input, uint64_t base, uint64_t& out) {
    std::wstring t;
    for (wchar_t c : Trim(input))
        if (c != L' ' && c != L'\'' && c != L'_') t += c;
    if (t.empty()) return false;
    int sign = 0;
    if (t[0] == L'+' || t[0] == L'-') {
        sign = t[0] == L'+' ? 1 : -1;
        t.erase(0, 1);
    }
    bool hex = false;
    if (t.size() > 2 && t[0] == L'0' && (t[1] == L'x' || t[1] == L'X')) {
        hex = true;
        t.erase(0, 2);
    } else if (!t.empty() && t[0] == L'$') {
        hex = true;
        t.erase(0, 1);
    } else if (!t.empty() && (t.back() == L'h' || t.back() == L'H')) {
        hex = true;
        t.pop_back();
    }
    if (t.empty()) return false;
    if (!hex)
        for (wchar_t c : t)
            if (HexDigitValue(c) >= 10) hex = true;
    uint64_t v = 0;
    for (wchar_t c : t) {
        int d = HexDigitValue(c);
        if (d < 0 || (!hex && d >= 10)) return false;
        uint64_t mul = hex ? 16 : 10;
        if (v > (UINT64_MAX - (uint64_t)d) / mul) return false;
        v = v * mul + (uint64_t)d;
    }
    if (sign > 0) {
        if (v > UINT64_MAX - base) return false;
        v = base + v;
    } else if (sign < 0) {
        v = v > base ? 0 : base - v;
    }
    out = v;
    return true;
}

void GotoDialog(HexState& s) {
    if (s.size == 0) return;
    HWND owner = GetAncestor(s.hwnd, GA_ROOT);
    static std::wstring last;
    std::wstring value = last.empty() ? L"0x" + Hex(s.caret, OffDigits(s)) : last;
    std::wstring hint = L"Gültiger Bereich: 0 – " + IntToStrGrouped(s.size - 1) + L" (0x" + Hex(s.size - 1, OffDigits(s)) +
                        L"). Mit + oder - relativ zur aktuellen Position.";
    if (!InputBox(owner, L"Gehe zu Offset", L"Offset (dezimal oder hexadezimal mit 0x… bzw. …h):", value, hint)) return;
    uint64_t off = 0;
    if (!ParseOffset(value, s.caret, off)) {
        MsgError(owner, L"Ungültiger Offset: " + value);
        return;
    }
    last = value;
    if (off >= s.size) {
        MsgError(owner, L"Der Offset liegt hinter dem Dateiende.");
        return;
    }
    MoveCaret(s, off, false);
}

// ---------- Suchen ----------

void LowerAscii(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] >= 'A' && p[i] <= 'Z') p[i] = (uint8_t)(p[i] + 32);
}

bool BuildPattern(HWND owner, const FindSettings& f, std::vector<uint8_t>& pat) {
    pat.clear();
    if (f.text.empty()) return false;
    switch (f.type) {
    case 0: {
        std::wstring digits;
        for (const auto& tok : Split(ReplaceAll(ReplaceAll(f.text, L",", L" "), L";", L" "), L' ')) {
            std::wstring t = tok;
            if (t.size() > 2 && t[0] == L'0' && (t[1] == L'x' || t[1] == L'X')) t.erase(0, 2);
            size_t added = 0;
            for (wchar_t c : t) {
                if (c == L'\t' || c == L'-') continue;
                if (HexDigitValue(c) < 0) {
                    MsgError(owner, L"Ungültige Hex-Bytes: „" + f.text + L"“\nBeispiel: 4D 5A 90 00");
                    return false;
                }
                digits += c;
                ++added;
            }
            if (added == 1) digits.insert(digits.size() - 1, 1, L'0');   // einzelne Ziffer = ein Byte („A“ = 0A)
        }
        if (digits.empty() || digits.size() % 2) {
            MsgError(owner, L"Ungerade Anzahl Hexziffern: „" + f.text + L"“");
            return false;
        }
        for (size_t i = 0; i < digits.size(); i += 2)
            pat.push_back((uint8_t)(HexDigitValue(digits[i]) * 16 + HexDigitValue(digits[i + 1])));
        break;
    }
    case 1: {
        int n = WideCharToMultiByte(CP_ACP, 0, f.text.c_str(), (int)f.text.size(), nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            pat.resize((size_t)n);
            WideCharToMultiByte(CP_ACP, 0, f.text.c_str(), (int)f.text.size(), (char*)pat.data(), n, nullptr, nullptr);
        }
        break;
    }
    case 2: {
        std::string u = WideToUtf8(f.text);
        pat.assign(u.begin(), u.end());
        break;
    }
    default:
        for (wchar_t c : f.text) {
            pat.push_back((uint8_t)(c & 0xFF));
            pat.push_back((uint8_t)((c >> 8) & 0xFF));
        }
        break;
    }
    return !pat.empty();
}

bool EscapePressed() {
    if (!(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) return false;
    // Tastendruck verwerfen, damit er nicht anschließend das Fenster schließt
    MSG m;
    while (PeekMessageW(&m, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {
    }
    return true;
}

bool DoFind(HexState& s, bool backward) {
    HWND owner = GetAncestor(s.hwnd, GA_ROOT);
    if (s.pattern.empty() || s.size == 0) return false;
    const size_t m = s.pattern.size();
    std::vector<uint8_t> pat = s.pattern;
    if (!s.patternCase) LowerAscii(pat.data(), pat.size());

    uint64_t selA = s.caret, selB = s.caret;
    if (s.selecting) GetSelection(s, selA, selB);
    bool found = false, cancelled = false;
    uint64_t foundAt = 0;
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::vector<uint8_t> buf(kSearchChunk + m);

    if (m <= s.size) {
        if (!backward) {
            uint64_t pos = s.selecting ? selA + 1 : s.caret;
            std::boyer_moore_horspool_searcher<std::vector<uint8_t>::iterator> searcher(pat.begin(), pat.end());
            while (pos + m <= s.size) {
                size_t len = (size_t)std::min<uint64_t>(kSearchChunk + m - 1, s.size - pos);
                ReadDirect(s, pos, buf.data(), len);
                if (!s.patternCase) LowerAscii(buf.data(), len);
                auto end = buf.begin() + (ptrdiff_t)len;
                auto it = std::search(buf.begin(), end, searcher);
                if (it != end) {
                    found = true;
                    foundAt = pos + (uint64_t)(it - buf.begin());
                    break;
                }
                if (len < kSearchChunk + m - 1) break;
                pos += kSearchChunk;
                if (EscapePressed()) {
                    cancelled = true;
                    break;
                }
            }
        } else {
            uint64_t limit = s.selecting ? selA : s.caret;   // Treffer muss vor limit beginnen
            if (limit > 0) {
                uint64_t end = std::min<uint64_t>(s.size, limit - 1 + m);
                while (end >= m) {
                    uint64_t begin = end > kSearchChunk + m - 1 ? end - (kSearchChunk + m - 1) : 0;
                    size_t len = (size_t)(end - begin);
                    ReadDirect(s, begin, buf.data(), len);
                    if (!s.patternCase) LowerAscii(buf.data(), len);
                    auto last = buf.begin() + (ptrdiff_t)len;
                    auto it = std::find_end(buf.begin(), last, pat.begin(), pat.end());
                    if (it != last) {
                        found = true;
                        foundAt = begin + (uint64_t)(it - buf.begin());
                        break;
                    }
                    if (begin == 0) break;
                    end = begin + m - 1;
                    if (EscapePressed()) {
                        cancelled = true;
                        break;
                    }
                }
            }
        }
    }
    SetCursor(oldCursor);
    if (cancelled) return false;
    if (!found) {
        MsgInfo(owner, L"„" + s.patternText + L"“ wurde nicht gefunden" +
                           (backward ? L" (Dateianfang erreicht)." : L" (Dateiende erreicht)."));
        return false;
    }
    s.anchor = foundAt;
    s.caret = foundAt + m - 1;
    s.selecting = true;
    s.nibble = 0;
    EnsureVisibleOffset(s, s.caret);
    EnsureVisibleOffset(s, s.anchor);
    Redraw(s);
    Notify(s);
    return true;
}

enum : int {
    IDC_FIND_TEXT = 101,
    IDC_FIND_HEX = 110,
    IDC_FIND_ANSI = 111,
    IDC_FIND_UTF8 = 112,
    IDC_FIND_UTF16 = 113,
    IDC_FIND_CASE = 120,
    IDC_FIND_BACK = 121,
};

class HexFindDialog : public DialogBase {
public:
    FindSettings settings;

protected:
    BOOL OnInit() override {
        SetText(IDC_FIND_TEXT, settings.text);
        CheckRadioButton(hwnd_, IDC_FIND_HEX, IDC_FIND_UTF16, IDC_FIND_HEX + std::clamp(settings.type, 0, 3));
        SetCheck(IDC_FIND_CASE, settings.matchCase);
        SetCheck(IDC_FIND_BACK, settings.backward);
        UpdateControls();
        SendMessageW(Item(IDC_FIND_TEXT), EM_SETSEL, 0, -1);
        SetFocus(Item(IDC_FIND_TEXT));
        return FALSE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id >= IDC_FIND_HEX && id <= IDC_FIND_UTF16) {
            UpdateControls();
            return TRUE;
        }
        if (id == IDOK) {
            settings.text = GetText(IDC_FIND_TEXT);
            for (int i = 0; i < 4; ++i)
                if (IsChecked(IDC_FIND_HEX + i)) settings.type = i;
            settings.matchCase = IsChecked(IDC_FIND_CASE);
            settings.backward = IsChecked(IDC_FIND_BACK);
            if (settings.text.empty()) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            End(IDOK);
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }

private:
    void UpdateControls() { Enable(IDC_FIND_CASE, !IsChecked(IDC_FIND_HEX)); }
};

void FindDialog(HexState& s) {
    if (s.file == INVALID_HANDLE_VALUE) return;
    HWND owner = GetAncestor(s.hwnd, GA_ROOT);
    DialogTemplate t(L"Suchen", 250, 118);
    t.Label(100, L"Suchen &nach:", 7, 9, 50, 8);
    t.Edit(IDC_FIND_TEXT, 60, 7, 183, 14);
    t.Group(102, L"Art", 7, 26, 176, 62);
    t.Radio(IDC_FIND_HEX, L"&Hex-Bytes (z. B. 4D 5A 90 00)", 14, 37, 165, 10, true);
    t.Radio(IDC_FIND_ANSI, L"Text (&ANSI)", 14, 49, 165, 10);
    t.Radio(IDC_FIND_UTF8, L"Text (&UTF-8)", 14, 61, 165, 10);
    t.Radio(IDC_FIND_UTF16, L"Text (UTF-1&6 LE)", 14, 73, 165, 10);
    t.Check(IDC_FIND_CASE, L"&Groß-/Kleinschreibung beachten", 7, 93, 176, 10);
    t.Check(IDC_FIND_BACK, L"&Rückwärts suchen", 7, 105, 176, 10);
    t.DefButton(IDOK, L"Suchen", 193, 28, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 193, 46, 50, 14);
    HexFindDialog dlg;
    dlg.settings = g_find;
    if (s.selecting && g_find.text.empty()) {
        // Vorbelegung mit einer kurzen Markierung als Hex-Bytes
        uint64_t a, b;
        GetSelection(s, a, b);
        if (b - a < 32) {
            std::wstring hx;
            for (uint64_t o = a; o <= b; ++o) hx += (hx.empty() ? L"" : L" ") + Hex(GetByte(s, o), 2);
            dlg.settings.text = hx;
            dlg.settings.type = 0;
        }
    }
    if (dlg.DoModal(owner, t) != IDOK) return;
    g_find = dlg.settings;
    std::vector<uint8_t> pat;
    if (!BuildPattern(owner, g_find, pat)) return;
    s.pattern = pat;
    s.patternCase = g_find.type == 0 ? true : g_find.matchCase;
    s.patternText = g_find.text;
    DoFind(s, g_find.backward);
}

bool FindNext(HexState& s, bool backward) {
    if (s.pattern.empty()) {
        // Muster aus den letzten globalen Einstellungen übernehmen, sonst Dialog
        std::vector<uint8_t> pat;
        if (!g_find.text.empty() && BuildPattern(GetAncestor(s.hwnd, GA_ROOT), g_find, pat)) {
            s.pattern = pat;
            s.patternCase = g_find.type == 0 ? true : g_find.matchCase;
            s.patternText = g_find.text;
        } else {
            FindDialog(s);
            return !s.pattern.empty();
        }
    }
    return DoFind(s, backward);
}

// ---------- Zeichnen ----------

void Paint(HexState& s, HDC target) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    int W = std::max<int>(1, rc.right), H = std::max<int>(1, rc.bottom);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, s.font);

    const bool focused = GetFocus() == s.hwnd;
    const COLORREF cText = GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF cBg = GetSysColor(COLOR_WINDOW);
    const COLORREF cGray = GetSysColor(COLOR_GRAYTEXT);
    const COLORREF cSelBg = focused ? GetSysColor(COLOR_HIGHLIGHT) : RGB(0xCC, 0xE4, 0xF7);
    const COLORREF cSelText = focused ? GetSysColor(COLOR_HIGHLIGHTTEXT) : cText;
    const COLORREF cMod = RGB(0xD0, 0x10, 0x10);
    const COLORREF cZero = RGB((GetRValue(cText) + 2 * GetRValue(cBg)) / 3, (GetGValue(cText) + 2 * GetGValue(cBg)) / 3,
                               (GetBValue(cText) + 2 * GetBValue(cBg)) / 3);
    const COLORREF cLine = RGB((GetRValue(cText) + 6 * GetRValue(cBg)) / 7, (GetGValue(cText) + 6 * GetGValue(cBg)) / 7,
                               (GetBValue(cText) + 6 * GetBValue(cBg)) / 7);

    HBRUSH bgBrush = CreateSolidBrush(cBg);
    FillRect(dc, &rc, bgBrush);
    DeleteObject(bgBrush);

    const int hdr = HeaderHeight(s);
    const int pad = (hdr - s.ch) / 2;
    const int digits = OffDigits(s);
    const int colHex = ColHex(s), colChar = ColChar(s);

    // Kopfzeile
    RECT hr{0, 0, W, hdr};
    FillRect(dc, &hr, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    TextOutW(dc, XCol(s, 0), pad, L"Offset", 6);
    for (int i = 0; i < 16; ++i) {
        std::wstring h = Hex((uint64_t)i, 2);
        TextOutW(dc, XCol(s, colHex + HexPos(i)), pad, h.c_str(), 2);
    }
    TextOutW(dc, XCol(s, colChar), pad, L"0123456789ABCDEF", 16);
    SetBkMode(dc, OPAQUE);

    // Trennlinien
    HPEN pen = CreatePen(PS_SOLID, 1, cLine);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    int x1 = XCol(s, digits + 1), x2 = XCol(s, colChar - 1) + s.cw / 2;
    MoveToEx(dc, x1, hdr, nullptr);
    LineTo(dc, x1, H);
    MoveToEx(dc, x2, hdr, nullptr);
    LineTo(dc, x2, H);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    HPEN shadow = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
    oldPen = SelectObject(dc, shadow);
    MoveToEx(dc, 0, hdr - 1, nullptr);
    LineTo(dc, W, hdr - 1);
    SelectObject(dc, oldPen);
    DeleteObject(shadow);

    IntersectClipRect(dc, 0, hdr, W, H);
    if (s.file == INVALID_HANDLE_VALUE || s.size == 0) {
        SetTextColor(dc, cGray);
        SetBkColor(dc, cBg);
        const wchar_t* msg = s.file == INVALID_HANDLE_VALUE ? L"(keine Datei)" : L"(Datei ist leer)";
        TextOutW(dc, XCol(s, 0), hdr + s.ch / 2, msg, (int)wcslen(msg));
    } else {
        uint64_t total = TotalLines(s);
        int vis = VisibleLines(s) + 1;
        uint64_t selA = 0, selB = 0;
        if (s.selecting) GetSelection(s, selA, selB);
        HBRUSH frameBrush = CreateSolidBrush(cGray);
        for (int r = 0; r < vis; ++r) {
            uint64_t line = s.topLine + (uint64_t)r;
            if (line >= total) break;
            int y = hdr + r * s.ch;
            uint64_t off = line * 16;
            int n = (int)std::min<uint64_t>(16, s.size - off);
            uint8_t raw[16];
            bool mod[16];
            ReadCached(s, off, raw, mod, (size_t)n);

            std::wstring offText = Hex(off, digits);
            SetTextColor(dc, cGray);
            SetBkColor(dc, cBg);
            TextOutW(dc, XCol(s, 0), y, offText.c_str(), (int)offText.size());

            for (int i = 0; i < n; ++i) {
                uint64_t o = off + (uint64_t)i;
                bool sel = s.selecting && o >= selA && o <= selB;
                bool nextSel = sel && i + 1 < n && o + 1 <= selB;
                COLORREF fg = sel ? cSelText : (mod[i] ? cMod : (raw[i] == 0 ? cZero : cText));
                if (sel && mod[i] && !focused) fg = cMod;
                COLORREF bg = sel ? cSelBg : cBg;
                SetTextColor(dc, fg);
                SetBkColor(dc, bg);
                std::wstring hx = Hex(raw[i], 2);
                int cx = colHex + HexPos(i);
                int wChars = 2 + (nextSel ? (i == 7 ? 2 : 1) : 0);
                RECT cell{XCol(s, cx), y, XCol(s, cx + wChars), y + s.ch};
                ExtTextOutW(dc, cell.left, y, ETO_OPAQUE | ETO_CLIPPED, &cell, hx.c_str(), 2, nullptr);
                wchar_t c = Chars().show[raw[i]];
                RECT cell2{XCol(s, colChar + i), y, XCol(s, colChar + i + 1), y + s.ch};
                ExtTextOutW(dc, cell2.left, y, ETO_OPAQUE | ETO_CLIPPED, &cell2, &c, 1, nullptr);
            }

            // Cursor
            if (s.caret / 16 == line) {
                int i = (int)(s.caret % 16);
                int cx = colHex + HexPos(i);
                RECT hexCell{XCol(s, cx), y, XCol(s, cx + 2), y + s.ch};
                RECT charCell{XCol(s, colChar + i), y, XCol(s, colChar + i + 1), y + s.ch};
                RECT active = s.charCol ? charCell : RECT{XCol(s, cx + s.nibble), y, XCol(s, cx + s.nibble + 1), y + s.ch};
                RECT inactive = s.charCol ? hexCell : charCell;
                if (focused) {
                    wchar_t c;
                    if (s.charCol) {
                        c = Chars().show[raw[i]];
                    } else {
                        std::wstring hx = Hex(raw[i], 2);
                        c = hx[(size_t)s.nibble];
                    }
                    SetTextColor(dc, cBg);
                    SetBkColor(dc, mod[i] ? cMod : cText);
                    ExtTextOutW(dc, active.left, y, ETO_OPAQUE | ETO_CLIPPED, &active, &c, 1, nullptr);
                } else {
                    FrameRect(dc, &active, frameBrush);
                }
                FrameRect(dc, &inactive, frameBrush);
            }
        }
        DeleteObject(frameBrush);
    }
    SelectClipRgn(dc, nullptr);

    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---------- Maus ----------

struct HitResult {
    uint64_t offset = 0;
    bool charCol = false;
    int nibble = 0;
    bool inData = false;   // Klick in Hex- oder Zeichenspalte
};

HitResult HitTest(HexState& s, int x, int y) {
    HitResult r;
    r.charCol = s.charCol;
    if (s.size == 0) return r;
    int hdr = HeaderHeight(s);
    long long row = (y - hdr) >= 0 ? (y - hdr) / s.ch : -1 - (hdr - y - 1) / s.ch;
    long long line = (long long)s.topLine + row;
    uint64_t total = TotalLines(s);
    if (line < 0) line = 0;
    if ((uint64_t)line >= total) line = (long long)total - 1;
    int xr = x - s.margin;
    int col = (xr >= 0 ? xr / s.cw : -1) + s.leftCol;
    int i = 0;
    int colHex = ColHex(s), colChar = ColChar(s);
    if (col >= colChar - 1) {
        i = std::clamp(col - colChar, 0, 15);
        r.charCol = true;
        r.inData = true;
    } else if (col >= colHex) {
        int rel = col - colHex;
        i = rel >= 25 ? (rel - 1) / 3 : rel / 3;
        i = std::clamp(i, 0, 15);
        r.nibble = (rel - HexPos(i)) == 1 ? 1 : 0;
        r.charCol = false;
        r.inData = true;
    } else {
        i = 0;
    }
    uint64_t off = (uint64_t)line * 16 + (uint64_t)i;
    if (off >= s.size) {
        off = s.size - 1;
        r.nibble = 0;
    }
    r.offset = off;
    return r;
}

void DragTo(HexState& s, int x, int y) {
    HitResult h = HitTest(s, x, y);
    if (h.offset != s.caret || !s.selecting) {
        if (h.offset == s.anchor && !s.selecting) return;
        MoveCaret(s, h.offset, true);
    }
}

void ContextMenu(HexState& s, int x, int y) {
    enum { CM_COPY = 1, CM_SELALL, CM_GOTO, CM_FIND, CM_FINDNEXT, CM_UNDO, CM_HEXCOL, CM_CHARCOL };
    HMENU m = CreatePopupMenu();
    if (!s.readOnly) {
        AppendMenuW(m, MF_STRING | (s.undo.empty() ? MF_GRAYED : 0), CM_UNDO, L"&Rückgängig\tStrg+Z");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    UINT dis = s.size == 0 ? MF_GRAYED : 0;
    AppendMenuW(m, MF_STRING | dis, CM_COPY, s.charCol ? L"&Kopieren (als Text)\tStrg+C" : L"&Kopieren (als Hex)\tStrg+C");
    AppendMenuW(m, MF_STRING | dis, CM_SELALL, L"&Alles markieren\tStrg+A");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.charCol ? 0 : MF_CHECKED), CM_HEXCOL, L"&Hex-Spalte");
    AppendMenuW(m, MF_STRING | (s.charCol ? MF_CHECKED : 0), CM_CHARCOL, L"&Zeichen-Spalte");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | dis, CM_GOTO, L"&Gehe zu Offset…\tStrg+G");
    AppendMenuW(m, MF_STRING | dis, CM_FIND, L"&Suchen…\tStrg+F");
    AppendMenuW(m, MF_STRING | dis, CM_FINDNEXT, L"&Weitersuchen\tF3");
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, s.hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case CM_COPY: Copy(s); break;
    case CM_SELALL: SelectAll(s); break;
    case CM_GOTO: GotoDialog(s); break;
    case CM_FIND: FindDialog(s); break;
    case CM_FINDNEXT: FindNext(s, false); break;
    case CM_UNDO: Undo(s); break;
    case CM_HEXCOL:
    case CM_CHARCOL:
        s.charCol = cmd == CM_CHARCOL;
        s.nibble = 0;
        Redraw(s);
        Notify(s);
        break;
    default: break;
    }
}

// ---------- Tastatur ----------

void OnKeyDown(HexState& s, WPARAM vk, LPARAM lp) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    HWND parent = GetParent(s.hwnd);
    switch (vk) {
    case VK_ESCAPE:
        if (parent) SendMessageW(parent, WM_KEYDOWN, vk, lp);
        return;
    case VK_TAB:
        if (s.readOnly || ctrl) {
            if (parent) SendMessageW(parent, WM_KEYDOWN, vk, lp);
        } else {
            s.charCol = !s.charCol;
            s.nibble = 0;
            EnsureVisibleOffset(s, s.caret);
            Redraw(s);
            Notify(s);
        }
        return;
    case VK_F3:
        FindNext(s, shift);
        return;
    default: break;
    }
    if (ctrl) {
        switch (vk) {
        case 'C':
        case VK_INSERT: Copy(s); return;
        case 'A': SelectAll(s); return;
        case 'G': GotoDialog(s); return;
        case 'F': FindDialog(s); return;
        case 'Z':
            if (!s.readOnly) Undo(s);
            return;
        default: break;
        }
    }
    if (s.size == 0) return;
    const uint64_t last = s.size - 1;
    const uint64_t page = (uint64_t)VisibleLines(s);
    switch (vk) {
    case VK_LEFT:
    case VK_BACK:
        if (!s.charCol && s.nibble == 1 && !shift) {
            s.nibble = 0;
            Redraw(s);
            return;
        }
        MoveCaret(s, s.caret > 0 ? s.caret - 1 : 0, shift && vk == VK_LEFT);
        break;
    case VK_RIGHT: MoveCaret(s, std::min(s.caret + 1, last), shift); break;
    case VK_UP:
        if (ctrl)
            ScrollLines(s, -1);
        else
            MoveCaret(s, s.caret >= 16 ? s.caret - 16 : s.caret, shift);
        break;
    case VK_DOWN:
        if (ctrl)
            ScrollLines(s, 1);
        else if (s.caret + 16 <= last)
            MoveCaret(s, s.caret + 16, shift);
        else if (s.caret / 16 < last / 16)
            MoveCaret(s, last, shift);
        break;
    case VK_PRIOR: {
        uint64_t d = page * 16;
        uint64_t pos = s.caret >= d ? s.caret - d : s.caret % 16;
        ScrollLines(s, -(long long)page);
        MoveCaret(s, pos, shift);
        break;
    }
    case VK_NEXT: {
        uint64_t d = page * 16;
        uint64_t pos = s.caret + d <= last ? s.caret + d : last;
        ScrollLines(s, (long long)page);
        MoveCaret(s, pos, shift);
        break;
    }
    case VK_HOME: MoveCaret(s, ctrl ? 0 : s.caret - s.caret % 16, shift); break;
    case VK_END: MoveCaret(s, ctrl ? last : std::min(s.caret - s.caret % 16 + 15, last), shift); break;
    default: break;
    }
}

void OnChar(HexState& s, wchar_t c) {
    if (s.readOnly || s.size == 0 || c < 0x20 || (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0)) return;
    if (!s.charCol) {
        int v = HexDigitValue(c);
        if (v < 0) {
            MessageBeep(MB_OK);
            return;
        }
        uint8_t old = GetByte(s, s.caret);
        uint8_t nb = s.nibble == 0 ? (uint8_t)((v << 4) | (old & 0x0F)) : (uint8_t)((old & 0xF0) | v);
        SetByte(s, s.caret, nb);
        if (s.nibble == 0) {
            s.nibble = 1;
        } else {
            s.nibble = 0;
            if (s.caret + 1 < s.size) ++s.caret;
        }
    } else {
        uint8_t b = 0;
        if (!CharToByte(c, b)) {
            MessageBeep(MB_OK);
            return;
        }
        SetByte(s, s.caret, b);
        if (s.caret + 1 < s.size) ++s.caret;
    }
    s.selecting = false;
    s.anchor = s.caret;
    EnsureVisibleOffset(s, s.caret);
    Redraw(s);
    Notify(s);
}

void UpdateFont(HexState& s) {
    HFONT f = CreateScaledFont(App::MonoFont(), GetWindowDpi(s.hwnd));
    if (s.font) DeleteObject(s.font);
    s.font = f;
    HDC dc = GetDC(s.hwnd);
    HGDIOBJ old = SelectObject(dc, s.font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SIZE sz{};
    GetTextExtentPoint32W(dc, L"0", 1, &sz);
    SelectObject(dc, old);
    ReleaseDC(s.hwnd, dc);
    s.cw = std::max<int>(1, sz.cx);
    s.ch = std::max<int>(1, tm.tmHeight + tm.tmExternalLeading);
    s.margin = DpiScale(s.hwnd, 4);
    UpdateScrollBars(s);
    Redraw(s);
}

LRESULT CALLBACK HexViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HexState* s = GetState(hwnd);
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        s = new HexState();
        s->hwnd = hwnd;
        s->readOnly = cs->lpCreateParams != nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE: UpdateFont(*s); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kAutoScrollTimer);
        CloseFile(*s);
        if (s->font) DeleteObject(s->font);
        s->font = nullptr;
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete s;
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_DPICHANGED_AFTERPARENT: UpdateFont(*s); return 0;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE: Redraw(*s); return 0;
    case WM_SIZE:
        UpdateScrollBars(*s);
        Redraw(*s);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(*s, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        Redraw(*s);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS | (s->readOnly ? 0 : DLGC_WANTTAB);
    case WM_KEYDOWN: OnKeyDown(*s, wp, lp); return 0;
    case WM_CHAR: OnChar(*s, (wchar_t)wp); return 0;
    case WM_VSCROLL: {
        uint64_t maxTop = MaxTop(*s);
        uint64_t f = ScrollFactor(maxTop);
        int page = VisibleLines(*s);
        switch (LOWORD(wp)) {
        case SB_LINEUP: ScrollLines(*s, -1); break;
        case SB_LINEDOWN: ScrollLines(*s, 1); break;
        case SB_PAGEUP: ScrollLines(*s, -page); break;
        case SB_PAGEDOWN: ScrollLines(*s, page); break;
        case SB_TOP: SetTop(*s, 0); break;
        case SB_BOTTOM: SetTop(*s, maxTop); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS | SIF_RANGE | SIF_PAGE;
            GetScrollInfo(hwnd, SB_VERT, &si);
            uint64_t top = (uint64_t)std::max(0, si.nTrackPos) * f;
            if (si.nTrackPos >= si.nMax - (int)si.nPage + 1) top = maxTop;
            SetTop(*s, top);
            break;
        }
        default: break;
        }
        return 0;
    }
    case WM_HSCROLL: {
        int cols = VisibleCols(*s);
        int left = s->leftCol;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: left -= 1; break;
        case SB_LINERIGHT: left += 1; break;
        case SB_PAGELEFT: left -= cols; break;
        case SB_PAGERIGHT: left += cols; break;
        case SB_LEFT: left = 0; break;
        case SB_RIGHT: left = LineChars(*s); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_HORZ, &si);
            left = si.nTrackPos;
            break;
        }
        default: break;
        }
        s->leftCol = std::clamp(left, 0, std::max(0, LineChars(*s) - cols));
        UpdateScrollBars(*s);
        Redraw(*s);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        s->wheelAccum += GET_WHEEL_DELTA_WPARAM(wp);
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL || lines == 0) lines = (UINT)VisibleLines(*s);
        while (s->wheelAccum >= WHEEL_DELTA) {
            ScrollLines(*s, -(long long)lines);
            s->wheelAccum -= WHEEL_DELTA;
        }
        while (s->wheelAccum <= -WHEEL_DELTA) {
            ScrollLines(*s, (long long)lines);
            s->wheelAccum += WHEEL_DELTA;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        SetFocus(hwnd);
        if (s->size == 0) return 0;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (y < HeaderHeight(*s)) return 0;
        HitResult h = HitTest(*s, x, y);
        if (wp & MK_SHIFT) {
            MoveCaret(*s, h.offset, true);
        } else {
            if (h.inData) s->charCol = h.charCol;
            MoveCaret(*s, h.offset, false);
            if (h.inData && !h.charCol) s->nibble = h.nibble;
        }
        s->dragging = true;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (s->dragging) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int dir = y < HeaderHeight(*s) ? -1 : (y >= rc.bottom ? 1 : 0);
            if (dir != s->autoScrollDir) {
                s->autoScrollDir = dir;
                if (dir)
                    SetTimer(hwnd, kAutoScrollTimer, 50, nullptr);
                else
                    KillTimer(hwnd, kAutoScrollTimer);
            }
            DragTo(*s, x, y);
        }
        return 0;
    case WM_TIMER:
        if (wp == kAutoScrollTimer && s->dragging && s->autoScrollDir) {
            ScrollLines(*s, s->autoScrollDir);
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            DragTo(*s, pt.x, pt.y);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s->dragging) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        s->dragging = false;
        s->autoScrollDir = 0;
        KillTimer(hwnd, kAutoScrollTimer);
        return 0;
    case WM_CONTEXTMENU: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x == -1 && y == -1) {
            POINT pt{XCol(*s, ColHex(*s)), HeaderHeight(*s)};
            ClientToScreen(hwnd, &pt);
            x = pt.x;
            y = pt.y;
        }
        ContextMenu(*s, x, y);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterHexViewClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = HexViewProc;
    wc.hInstance = App::Instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = kHexViewClass;
    RegisterClassExW(&wc);
    done = true;
}

// ===================== Hex-Editor-Fenster =====================

enum : UINT {
    IDM_HX_SAVE = 1001,
    IDM_HX_RELOAD,
    IDM_HX_CLOSE,
    IDM_HX_UNDO,
    IDM_HX_COPY,
    IDM_HX_SELALL,
    IDM_HX_GOTO,
    IDM_HX_FIND,
    IDM_HX_FINDNEXT,
    IDM_HX_FINDPREV,
    IDM_HX_COLUMN,
};

constexpr int kIdHex = 100;
constexpr int kIdStatus = 101;
constexpr wchar_t kHexSection[] = L"HexEditor";

struct EditorState {
    HWND hwnd = nullptr;
    HWND hex = nullptr;
    HWND status = nullptr;
    HACCEL accel = nullptr;
    HFONT statusFont = nullptr;
    bool fileReadOnly = false;
};

std::vector<HWND> g_editors;

EditorState* GetEditor(HWND h) { return reinterpret_cast<EditorState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

// Fensterposition laden/speichern (Bildschirmkoordinaten)
bool LoadPlacement(const wchar_t* section, RECT& r, bool& maximized) {
    Config& c = App::Cfg();
    maximized = c.GetBool(section, L"Max", false);
    if (!c.Has(section, L"B") || !c.Has(section, L"H")) return false;
    int x = c.GetInt(section, L"X", 0), y = c.GetInt(section, L"Y", 0);
    int w = c.GetInt(section, L"B", 0), h = c.GetInt(section, L"H", 0);
    if (w < 200 || h < 150) return false;
    r = RECT{x, y, x + w, y + h};
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

void SavePlacement(HWND hwnd, const wchar_t* section) {
    Config& c = App::Cfg();
    bool zoomed = IsZoomed(hwnd) != FALSE;
    c.SetBool(section, L"Max", zoomed);
    if (zoomed || IsIconic(hwnd)) return;
    RECT r;
    GetWindowRect(hwnd, &r);
    c.SetInt(section, L"X", r.left);
    c.SetInt(section, L"Y", r.top);
    c.SetInt(section, L"B", r.right - r.left);
    c.SetInt(section, L"H", r.bottom - r.top);
}

void UpdateEditorTitle(EditorState& e) {
    std::wstring path = HexViewPath(e.hex);
    std::wstring title = (HexViewIsModified(e.hex) ? L"*" : L"") + PathFileName(path) +
                         (e.fileReadOnly ? L" [schreibgeschützt]" : L"") + L" – QFiles Hex-Editor";
    SetWindowTextW(e.hwnd, title.c_str());
}

void LayoutStatusParts(EditorState& e) {
    int w[5];
    int x = 0;
    const int widths[4] = {230, 230, 250, 190};
    for (int i = 0; i < 4; ++i) {
        x += DpiScale(e.hwnd, widths[i]);
        w[i] = x;
    }
    w[4] = -1;
    SendMessageW(e.status, SB_SETPARTS, 5, (LPARAM)w);
}

void UpdateEditorStatus(EditorState& e) {
    HexViewStatus st = HexViewGetStatus(e.hex);
    int digits = st.fileSize > 0xFFFFFFFFull ? 16 : 8;
    std::wstring p0, p1, p2, p3, p4;
    if (st.hasByte) {
        p0 = L"Offset: 0x" + Hex(st.caret, digits) + L" (" + IntToStrGrouped(st.caret) + L")";
        wchar_t c = Chars().show[st.value];
        p1 = L"Wert: 0x" + Hex(st.value, 2) + L" (" + std::to_wstring((int)st.value) + L")  ‚" + std::wstring(1, c) + L"‘";
    }
    if (st.hasSelection) {
        uint64_t n = st.selEnd - st.selStart + 1;
        p2 = L"Markiert: " + IntToStrGrouped(n) + (n == 1 ? L" Byte" : L" Bytes") + L" (0x" + Hex(st.selStart, digits) +
             L"–0x" + Hex(st.selEnd, digits) + L")";
    } else {
        p2 = st.charColumn ? L"Zeichenspalte" : L"Hexspalte";
    }
    p3 = L"Größe: " + IntToStrGrouped(st.fileSize) + L" Bytes";
    p4 = st.modified ? L"Geändert" : (e.fileReadOnly ? L"Schreibgeschützt" : L"");
    const std::wstring* parts[5] = {&p0, &p1, &p2, &p3, &p4};
    for (int i = 0; i < 5; ++i) SendMessageW(e.status, SB_SETTEXTW, (WPARAM)i, (LPARAM)parts[i]->c_str());
}

void UpdateStatusFont(EditorState& e) {
    HFONT f = CreateScaledFont(App::UIFont(), GetWindowDpi(e.hwnd));
    SendMessageW(e.status, WM_SETFONT, (WPARAM)f, TRUE);
    if (e.statusFont) DeleteObject(e.statusFont);
    e.statusFont = f;
    LayoutStatusParts(e);
}

void LayoutEditor(EditorState& e) {
    RECT rc;
    GetClientRect(e.hwnd, &rc);
    SendMessageW(e.status, WM_SIZE, 0, 0);
    RECT sr{};
    GetWindowRect(e.status, &sr);
    int sh = sr.bottom - sr.top;
    MoveWindow(e.hex, 0, 0, rc.right, std::max<int>(0, rc.bottom - sh), TRUE);
}

HMENU CreateEditorMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_HX_SAVE, L"&Speichern\tStrg+S");
    AppendMenuW(file, MF_STRING, IDM_HX_RELOAD, L"&Neu laden\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_HX_CLOSE, L"S&chließen\tEsc");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&Datei");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_HX_UNDO, L"&Rückgängig\tStrg+Z");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_HX_COPY, L"&Kopieren\tStrg+C");
    AppendMenuW(edit, MF_STRING, IDM_HX_SELALL, L"&Alles markieren\tStrg+A");
    AppendMenuW(edit, MF_STRING, IDM_HX_COLUMN, L"Hex-/&Zeichenspalte wechseln\tTab");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_HX_GOTO, L"&Gehe zu Offset…\tStrg+G");
    AppendMenuW(edit, MF_STRING, IDM_HX_FIND, L"&Suchen…\tStrg+F");
    AppendMenuW(edit, MF_STRING, IDM_HX_FINDNEXT, L"&Weitersuchen\tF3");
    AppendMenuW(edit, MF_STRING, IDM_HX_FINDPREV, L"R&ückwärts weitersuchen\tUmschalt+F3");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Bearbeiten");
    return bar;
}

HACCEL CreateEditorAccel() {
    ACCEL a[] = {
        {FVIRTKEY | FCONTROL, 'S', (WORD)IDM_HX_SAVE},
        {FVIRTKEY, VK_F5, (WORD)IDM_HX_RELOAD},
        {FVIRTKEY, VK_ESCAPE, (WORD)IDM_HX_CLOSE},
        {FVIRTKEY | FCONTROL, 'Z', (WORD)IDM_HX_UNDO},
        {FVIRTKEY | FCONTROL, 'C', (WORD)IDM_HX_COPY},
        {FVIRTKEY | FCONTROL, VK_INSERT, (WORD)IDM_HX_COPY},
        {FVIRTKEY | FCONTROL, 'A', (WORD)IDM_HX_SELALL},
        {FVIRTKEY | FCONTROL, 'G', (WORD)IDM_HX_GOTO},
        {FVIRTKEY | FCONTROL, 'F', (WORD)IDM_HX_FIND},
        {FVIRTKEY, VK_F3, (WORD)IDM_HX_FINDNEXT},
        {FVIRTKEY | FSHIFT, VK_F3, (WORD)IDM_HX_FINDPREV},
    };
    return CreateAcceleratorTableW(a, (int)(sizeof(a) / sizeof(a[0])));
}

bool FileIsReadOnly(const std::wstring& path) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
}

LRESULT CALLBACK HexEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditorState* e = GetEditor(hwnd);
    switch (msg) {
    case WM_NCCREATE: {
        e = new EditorState();
        e->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)e);
        break;
    }
    case WM_CREATE: {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)App::SmallIcon());
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)App::BigIcon());
        e->hex = CreateHexView(hwnd, kIdHex, false);
        e->status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                    hwnd, (HMENU)(INT_PTR)kIdStatus, App::Instance(), nullptr);
        UpdateStatusFont(*e);
        e->accel = CreateEditorAccel();
        App::RegisterAccelerator(hwnd, e->accel);
        g_editors.push_back(hwnd);
        return 0;
    }
    case WM_SIZE:
        if (e) {
            LayoutEditor(*e);
            LayoutStatusParts(*e);
            if (wp == SIZE_MAXIMIZED || wp == SIZE_RESTORED) SavePlacement(hwnd, kHexSection);
        }
        return 0;
    case WM_EXITSIZEMOVE: SavePlacement(hwnd, kHexSection); return 0;
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateStatusFont(*e);
        LayoutEditor(*e);
        return 0;
    }
    case WM_SETFOCUS:
        if (e && e->hex) SetFocus(e->hex);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && e && e->hex) SetFocus(e->hex);
        return 0;
    case WM_INITMENUPOPUP: {
        HMENU m = (HMENU)wp;
        HexViewStatus st = HexViewGetStatus(e->hex);
        EnableMenuItem(m, IDM_HX_SAVE, MF_BYCOMMAND | (st.modified ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(m, IDM_HX_UNDO, MF_BYCOMMAND | (st.canUndo ? MF_ENABLED : MF_GRAYED));
        UINT has = st.fileSize ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(m, IDM_HX_COPY, MF_BYCOMMAND | has);
        EnableMenuItem(m, IDM_HX_SELALL, MF_BYCOMMAND | has);
        EnableMenuItem(m, IDM_HX_GOTO, MF_BYCOMMAND | has);
        EnableMenuItem(m, IDM_HX_FIND, MF_BYCOMMAND | has);
        EnableMenuItem(m, IDM_HX_FINDNEXT, MF_BYCOMMAND | has);
        EnableMenuItem(m, IDM_HX_FINDPREV, MF_BYCOMMAND | has);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == kIdHex && HIWORD(wp) == HVN_STATUS) {
            UpdateEditorStatus(*e);
            UpdateEditorTitle(*e);
            return 0;
        }
        HexState* hs = GetState(e->hex);
        switch (id) {
        case IDM_HX_SAVE:
            if (HexViewSave(e->hex)) {
                e->fileReadOnly = FileIsReadOnly(HexViewPath(e->hex));
                UpdateEditorTitle(*e);
                UpdateEditorStatus(*e);
            }
            return 0;
        case IDM_HX_RELOAD:
            if (HexViewIsModified(e->hex) &&
                !MsgConfirm(hwnd, L"Alle ungespeicherten Änderungen verwerfen und die Datei neu laden?"))
                return 0;
            if (!HexViewReload(e->hex)) MsgError(hwnd, HexViewLastError(e->hex));
            e->fileReadOnly = FileIsReadOnly(HexViewPath(e->hex));
            UpdateEditorTitle(*e);
            UpdateEditorStatus(*e);
            return 0;
        case IDM_HX_CLOSE: PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case IDM_HX_UNDO: HexViewUndo(e->hex); return 0;
        case IDM_HX_COPY: HexViewCopy(e->hex); return 0;
        case IDM_HX_SELALL: HexViewSelectAll(e->hex); return 0;
        case IDM_HX_COLUMN:
            if (hs) {
                hs->charCol = !hs->charCol;
                hs->nibble = 0;
                EnsureVisibleOffset(*hs, hs->caret);
                Redraw(*hs);
                Notify(*hs);
            }
            return 0;
        case IDM_HX_GOTO: HexViewGotoDialog(e->hex); return 0;
        case IDM_HX_FIND: HexViewFindDialog(e->hex); return 0;
        case IDM_HX_FINDNEXT: HexViewFindNext(e->hex, false); return 0;
        case IDM_HX_FINDPREV: HexViewFindNext(e->hex, true); return 0;
        default: break;
        }
        break;
    }
    case WM_KEYDOWN:
        // Vom HexView weitergereichte Esc-Taste (falls nicht über die Tastenkürzel behandelt)
        if (wp == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        if (e && HexViewIsModified(e->hex)) {
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            int r = MsgYesNoCancel(hwnd, L"Die Datei „" + PathFileName(HexViewPath(e->hex)) +
                                             L"“ wurde geändert.\n\nSollen die Änderungen gespeichert werden?");
            if (r == IDCANCEL) return 0;
            if (r == IDYES && !HexViewSave(e->hex)) return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        SavePlacement(hwnd, kHexSection);
        App::UnregisterAccelerator(hwnd);
        if (e) {
            if (e->accel) DestroyAcceleratorTable(e->accel);
            e->accel = nullptr;
            if (e->status) SendMessageW(e->status, WM_SETFONT, 0, FALSE);
            if (e->statusFont) DeleteObject(e->statusFont);
            e->statusFont = nullptr;
        }
        g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), hwnd), g_editors.end());
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete e;
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterHexEditorClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HexEditorProc;
    wc.hInstance = App::Instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = App::BigIcon();
    wc.hIconSm = App::SmallIcon();
    wc.lpszClassName = kHexEditorClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

// ===================== Öffentliche Schnittstelle: HexView =====================

HWND CreateHexView(HWND parent, int id, bool readOnly) {
    RegisterHexViewClass();
    // lpCreateParams != nullptr bedeutet schreibgeschützt
    return CreateWindowExW(0, kHexViewClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0,
                           0, 0, parent, (HMENU)(INT_PTR)id, App::Instance(), readOnly ? (LPVOID)1 : nullptr);
}

bool HexViewOpen(HWND hv, const std::wstring& path) {
    HexState* s = GetState(hv);
    return s && OpenFile(*s, path);
}

void HexViewClose(HWND hv) {
    if (HexState* s = GetState(hv)) {
        CloseFile(*s);
        UpdateScrollBars(*s);
        Redraw(*s);
        Notify(*s);
    }
}

bool HexViewReload(HWND hv) {
    HexState* s = GetState(hv);
    if (!s || s->path.empty()) return false;
    std::wstring path = s->path;
    uint64_t caret = s->caret, top = s->topLine;
    bool charCol = s->charCol;
    if (!OpenFile(*s, path)) return false;
    s->charCol = charCol;
    if (s->size) {
        s->topLine = top;
        UpdateScrollBars(*s);
        MoveCaret(*s, std::min(caret, s->size - 1), false);
    }
    return true;
}

std::wstring HexViewPath(HWND hv) {
    HexState* s = GetState(hv);
    return s ? s->path : std::wstring();
}

std::wstring HexViewLastError(HWND hv) {
    HexState* s = GetState(hv);
    return s ? s->lastError : std::wstring();
}

bool HexViewIsModified(HWND hv) {
    HexState* s = GetState(hv);
    return s && !s->mods.empty();
}

bool HexViewSave(HWND hv) {
    HexState* s = GetState(hv);
    return s && Save(*s);
}

bool HexViewCanUndo(HWND hv) {
    HexState* s = GetState(hv);
    return s && !s->undo.empty();
}

bool HexViewUndo(HWND hv) {
    HexState* s = GetState(hv);
    return s && !s->readOnly && Undo(*s);
}

void HexViewCopy(HWND hv) {
    if (HexState* s = GetState(hv)) Copy(*s);
}

void HexViewSelectAll(HWND hv) {
    if (HexState* s = GetState(hv)) SelectAll(*s);
}

void HexViewGotoDialog(HWND hv) {
    if (HexState* s = GetState(hv)) GotoDialog(*s);
}

void HexViewFindDialog(HWND hv) {
    if (HexState* s = GetState(hv)) FindDialog(*s);
}

bool HexViewFindNext(HWND hv, bool backward) {
    HexState* s = GetState(hv);
    return s && FindNext(*s, backward);
}

bool HexViewSetCaret(HWND hv, uint64_t offset) {
    HexState* s = GetState(hv);
    if (!s || offset >= s->size) return false;
    MoveCaret(*s, offset, false);
    return true;
}

HexViewStatus HexViewGetStatus(HWND hv) {
    HexViewStatus st;
    HexState* s = GetState(hv);
    if (!s) return st;
    st.open = s->file != INVALID_HANDLE_VALUE;
    st.readOnly = s->readOnly;
    st.modified = !s->mods.empty();
    st.canUndo = !s->undo.empty();
    st.fileSize = s->size;
    st.caret = s->caret;
    st.hasByte = st.open && s->size > 0;
    if (st.hasByte) st.value = GetByte(*s, s->caret);
    st.hasSelection = s->selecting;
    if (s->selecting) GetSelection(*s, st.selStart, st.selEnd);
    st.charColumn = s->charCol;
    return st;
}

// ===================== Öffentliche Schnittstelle: Hex-Editor =====================

void OpenHexEditor(const std::wstring& path) {
    if (path.empty()) return;
    // Ist die Datei bereits in einem Hex-Editor geöffnet? Dann dieses Fenster aktivieren.
    for (HWND h : g_editors) {
        EditorState* e = GetEditor(h);
        if (e && EqualsI(HexViewPath(e->hex), path)) {
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
            return;
        }
    }
    if (DirExists(path)) {
        MsgError(App::MainWindow(), L"„" + PathFileName(path) + L"“ ist ein Verzeichnis und kann nicht im Hex-Editor geöffnet werden.");
        return;
    }
    RegisterHexEditorClass();
    RECT r{};
    bool maximized = false;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = CW_USEDEFAULT, h = CW_USEDEFAULT;
    if (LoadPlacement(kHexSection, r, maximized)) {
        int offset = (int)(g_editors.size() % 8) * GetSystemMetrics(SM_CYCAPTION);
        x = r.left + offset;
        y = r.top + offset;
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    HWND hwnd = CreateWindowExW(0, kHexEditorClass, L"QFiles Hex-Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w,
                                h, nullptr, CreateEditorMenu(), App::Instance(), nullptr);
    if (!hwnd) return;
    EditorState* e = GetEditor(hwnd);
    if (!e || !HexViewOpen(e->hex, path)) {
        MsgError(App::MainWindow(), L"Die Datei „" + PathFileName(path) + L"“ kann nicht geöffnet werden:\n" +
                                        (e ? HexViewLastError(e->hex) : std::wstring()));
        DestroyWindow(hwnd);
        return;
    }
    e->fileReadOnly = FileIsReadOnly(path);
    LayoutEditor(*e);
    UpdateEditorTitle(*e);
    UpdateEditorStatus(*e);
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetFocus(e->hex);
}

bool CloseAllHexEditors() {
    std::vector<HWND> list = g_editors;
    for (HWND h : list) {
        if (!IsWindow(h)) continue;
        SendMessageW(h, WM_CLOSE, 0, 0);
        if (IsWindow(h)) return false;
    }
    return true;
}

} // namespace qf
