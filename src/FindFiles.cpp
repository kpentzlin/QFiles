// FindFiles.cpp – Modul D: Dateien suchen
//
// Nicht modaler, veränderbarer Dialog. Die Suche läuft in einem Arbeitsthread; Treffer werden
// gebündelt (höchstens alle 100 ms) per PostMessageW an den Dialog gemeldet. Die Ergebnisliste
// ist eine virtuelle ListView (LVS_OWNERDATA) und per Spaltenklick sortierbar.
//
// Textsuche: Die Datei wird blockweise (1 MB) gelesen. Die Kodierung wird am ersten Block mit
// qf::DetectEncoding bestimmt; die Blöcke werden dekodiert und überlappend (Länge des Suchtextes − 1)
// durchsucht. Unvollständige Bytefolgen am Blockende (UTF-8, UTF-16, DBCS) werden in den nächsten
// Block übernommen. Binärdateien werden als ANSI und als UTF-16 LE (gerade und ungerade Ausrichtung)
// durchsucht.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Encoding.h"
#include "FileOps.h"
#include "Settings.h"
#include "Util.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace qf {

namespace {

constexpr wchar_t kSection[] = L"Suchen";
constexpr int kHistoryMax = 20;
constexpr UINT kMsgResults = WM_APP + 1;
constexpr UINT kMsgDone = WM_APP + 2;
constexpr uint64_t kOneDay = 864000000000ull; // 100-ns-Einheiten

enum : int {
    CID_LBL_PATTERN = 101,
    CID_PATTERN,
    CID_LBL_DIRS,
    CID_DIRS,
    CID_BROWSE,
    CID_LBL_TEXT,
    CID_TEXT,
    CID_MATCHCASE,
    CID_SUBDIRS,
    CID_HIDDEN,
    CID_DIRNAMES,
    CID_LBL_SIZEMIN,
    CID_SIZEMIN,
    CID_LBL_SIZEMAX,
    CID_SIZEMAX,
    CID_LBL_KB,
    CID_LBL_DATEMIN,
    CID_DATEMIN,
    CID_LBL_DATEMAX,
    CID_DATEMAX,
    CID_STOP,
    CID_RESULTS,
    CID_STATUS,
};

enum : int {
    CMD_GOTO = 1001,
    CMD_OPEN,
    CMD_EDIT,
    CMD_VIEW,
    CMD_COPYPATHS,
    CMD_COPYFILES,
    CMD_DELETE,
};

uint64_t FtToU64(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

// ---------------------------------------------------------------------------
// Textsuche in Dateien
// ---------------------------------------------------------------------------

class TextMatcher {
public:
    TextMatcher(const std::wstring& needle, bool matchCase) : matchCase_(matchCase) {
        needle_ = matchCase ? needle : ToLower(needle);
        for (wchar_t c : needle)
            if (c >= 0x80) asciiOnly_ = false;
        CPINFO ci{};
        if (GetCPInfo(CP_ACP, &ci)) ansiDbcs_ = ci.MaxCharSize > 1;
    }

    // true, wenn die Datei den Suchtext enthält. buf wird als Lesepuffer wiederverwendet.
    bool FileContains(const std::wstring& path, const std::atomic<bool>& stop, std::vector<uint8_t>& buf) const {
        if (needle_.empty()) return true;
        HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        constexpr DWORD kBlock = 1u << 20;
        buf.resize(kBlock);
        std::vector<Stream> streams;
        bool first = true, found = false;
        while (!found && !stop.load(std::memory_order_relaxed)) {
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), kBlock, &got, nullptr)) break;
            if (first) {
                first = false;
                if (got == 0) break;
                SetupStreams(buf.data(), got, streams);
            }
            if (got == 0) {
                // Dateiende: zurückgehaltene Restbytes verarbeiten
                for (auto& s : streams)
                    if (!s.carry.empty() && Feed(s, nullptr, 0, true)) found = true;
                break;
            }
            for (auto& s : streams)
                if (Feed(s, buf.data(), got, false)) {
                    found = true;
                    break;
                }
        }
        CloseHandle(h);
        return found;
    }

private:
    enum class Kind { Utf8, Ansi, Utf16LE, Utf16BE };
    struct Stream {
        Kind kind = Kind::Utf8;
        size_t skip = 0;              // am Dateianfang zu überspringende Bytes (BOM, Ausrichtung)
        std::vector<uint8_t> carry;   // unvollständige Bytefolge vom vorigen Block
        std::vector<uint8_t> work;
        std::wstring tail;            // Überlappung zum vorigen Block
    };

    void SetupStreams(const uint8_t* d, size_t n, std::vector<Stream>& out) const {
        auto add = [&](Kind k, size_t skip) {
            Stream s;
            s.kind = k;
            s.skip = skip;
            out.push_back(std::move(s));
        };
        if (LooksBinary(d, n)) {
            add(Kind::Ansi, 0);
            add(Kind::Utf16LE, 0);
            add(Kind::Utf16LE, 1);
            if (!asciiOnly_) add(Kind::Utf8, 0);
            return;
        }
        size_t bom = 0;
        switch (DetectEncoding(d, n, &bom)) {
        case TextEncoding::Utf8Bom:
            add(Kind::Utf8, bom);
            break;
        case TextEncoding::Utf8:
            add(Kind::Utf8, 0);
            // Gemischte Dateien (erst ASCII, später ANSI-Umlaute) zusätzlich als ANSI durchsuchen
            if (!asciiOnly_) add(Kind::Ansi, 0);
            break;
        case TextEncoding::Utf16LE:
            add(Kind::Utf16LE, bom);
            break;
        case TextEncoding::Utf16BE:
            add(Kind::Utf16BE, bom);
            break;
        default:
            add(Kind::Ansi, 0);
            break;
        }
    }

    bool Feed(Stream& s, const uint8_t* data, size_t n, bool last) const {
        if (s.skip && n) {
            size_t k = std::min(s.skip, n);
            data += k;
            n -= k;
            s.skip -= k;
        }
        std::vector<uint8_t>& b = s.work;
        b.assign(s.carry.begin(), s.carry.end());
        if (n) b.insert(b.end(), data, data + n);
        s.carry.clear();
        size_t use = b.size();
        switch (s.kind) {
        case Kind::Utf16LE:
        case Kind::Utf16BE:
            use &= ~size_t(1);
            break;
        case Kind::Utf8:
            if (!last && use) {
                // unvollständige Sequenz am Ende zurückhalten
                size_t i = use, back = 0;
                while (i > 0 && back < 3 && (b[i - 1] & 0xC0) == 0x80) {
                    --i;
                    ++back;
                }
                if (i > 0) {
                    uint8_t lead = b[i - 1];
                    size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
                    if (need > 1 && back + 1 < need) use = i - 1;
                }
            }
            break;
        case Kind::Ansi:
            if (!last && ansiDbcs_) {
                size_t i = 0;
                while (i < use) {
                    if (IsDBCSLeadByteEx(CP_ACP, b[i])) {
                        if (i + 1 >= use) {
                            use = i;
                            break;
                        }
                        i += 2;
                    } else {
                        ++i;
                    }
                }
            }
            break;
        }
        if (use < b.size() && !last) s.carry.assign(b.begin() + (std::ptrdiff_t)use, b.end());

        std::wstring text = s.tail;
        if (use) {
            if (s.kind == Kind::Utf8 || s.kind == Kind::Ansi) {
                UINT cp = s.kind == Kind::Utf8 ? CP_UTF8 : CP_ACP;
                int m = MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(b.data()), (int)use, nullptr, 0);
                if (m > 0) {
                    size_t old = text.size();
                    text.resize(old + (size_t)m);
                    MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(b.data()), (int)use, &text[old], m);
                }
            } else {
                size_t units = use / 2;
                size_t old = text.size();
                text.resize(old + units);
                for (size_t k = 0; k < units; ++k) {
                    uint8_t lo = b[2 * k], hi = b[2 * k + 1];
                    if (s.kind == Kind::Utf16BE) std::swap(lo, hi);
                    text[old + k] = (wchar_t)(lo | (hi << 8));
                }
            }
        }
        if (!matchCase_) text = ToLower(text);
        bool found = text.find(needle_) != std::wstring::npos;
        size_t keep = needle_.size() - 1;
        s.tail = text.size() > keep ? text.substr(text.size() - keep) : text;
        return found;
    }

    std::wstring needle_;
    bool matchCase_ = false;
    bool asciiOnly_ = true;
    bool ansiDbcs_ = false;
};

// ---------------------------------------------------------------------------
// Suchparameter und Treffer
// ---------------------------------------------------------------------------

struct SearchParams {
    std::vector<std::wstring> dirs;
    std::wstring patterns;      // aufbereitet für MatchAnyPattern
    std::wstring text;
    bool matchCase = false, subdirs = true, hidden = false, dirNames = false;
    bool hasSizeMin = false, hasSizeMax = false;
    uint64_t sizeMin = 0, sizeMax = 0;
    bool hasDateMin = false, hasDateMax = false;
    uint64_t dateMin = 0, dateMax = 0; // UTC, dateMax exklusiv
};

struct Hit {
    std::wstring name, dir;
    uint64_t size = 0;
    FILETIME modified{};
    bool isDir = false;
    std::wstring Path() const { return PathCombine(dir, name); }
};

// "abc" ohne Platzhalter wird zu "*abc*"; leere Eingabe = alle Dateien.
std::wstring PreparePatterns(const std::wstring& input) {
    std::vector<std::wstring> out;
    for (auto& p : Split(input, L';')) {
        std::wstring t = Trim(p);
        if (t.empty()) continue;
        if (t.find_first_of(L"*?") == std::wstring::npos) t = L"*" + t + L"*";
        out.push_back(t);
    }
    return Join(out, L";");
}

bool DayStartUtc(SYSTEMTIME st, uint64_t& out) {
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    SYSTEMTIME utc{};
    FILETIME ft{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc) || !SystemTimeToFileTime(&utc, &ft)) return false;
    out = FtToU64(ft);
    return true;
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

class FindFilesDialog : public DialogBase {
public:
    explicit FindFilesDialog(const std::wstring& startDir) : startDir_(startDir) {}

protected:
    BOOL OnInit() override;
    BOOL OnCommand(int id, int code, HWND ctl) override;
    INT_PTR OnNotify(NMHDR* nm) override;
    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnDestroy() override;

private:
    void StartSearch();
    void StopWorker();
    void Run(SearchParams p, HWND target);
    void Drain();
    void SetRunning(bool on);
    void UpdateStatus(const std::wstring& curDir);
    void SortResults();
    void UpdateSortArrow();
    std::vector<size_t> SelectedIndices() const;
    std::vector<std::wstring> SelectedPaths() const;
    void ShowContextMenu(POINT pt);
    void DoCommand(int cmd);
    void LoadSettings();
    void SaveSettings();
    void FillHistory(int comboId, const std::vector<std::wstring>& list);
    static void AddToHistory(std::vector<std::wstring>& list, const std::wstring& value);

    std::wstring startDir_;
    std::vector<Hit> results_;
    std::vector<std::wstring> histPattern_, histText_;
    int sortCol_ = -1;
    bool sortAsc_ = true;
    bool running_ = false;
    std::wstring dispBuf_;

    // Arbeitsthread
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> postPending_{false};
    std::atomic<uint64_t> scanned_{0};
    std::mutex mtx_;
    std::vector<Hit> pending_;   // geschützt durch mtx_
    std::wstring curDir_;        // geschützt durch mtx_
};

BOOL FindFilesDialog::OnInit() {
    LvAddColumn(CID_RESULTS, L"Name", 130);
    LvAddColumn(CID_RESULTS, L"Verzeichnis", 200);
    LvAddColumn(CID_RESULTS, L"Größe", 55, LVCFMT_RIGHT);
    LvAddColumn(CID_RESULTS, L"Geändert", 70);
    if (App::Opt().showGridLines)
        ListView_SetExtendedListViewStyleEx(Item(CID_RESULTS), LVS_EX_GRIDLINES, LVS_EX_GRIDLINES);

    DateTime_SetSystemtime(Item(CID_DATEMIN), GDT_NONE, nullptr);
    DateTime_SetSystemtime(Item(CID_DATEMAX), GDT_NONE, nullptr);
    SendMessageW(Item(CID_SIZEMIN), EM_LIMITTEXT, 12, 0);
    SendMessageW(Item(CID_SIZEMAX), EM_LIMITTEXT, 12, 0);
    SetText(CID_DIRS, startDir_);
    LoadSettings();

    SetAnchor(CID_PATTERN, AnchorTopLeftRight);
    SetAnchor(CID_DIRS, AnchorTopLeftRight);
    SetAnchor(CID_BROWSE, AnchorTopRight);
    SetAnchor(CID_TEXT, AnchorTopLeftRight);
    SetAnchor(IDOK, AnchorTopRight);
    SetAnchor(CID_STOP, AnchorTopRight);
    SetAnchor(IDCANCEL, AnchorTopRight);
    SetAnchor(CID_RESULTS, AnchorAll);
    SetAnchor(CID_STATUS, AnchorBottomLeftRight);
    EnableResizing();

    SetRunning(false);
    SetText(CID_STATUS, L"Bereit.");
    return TRUE;
}

void FindFilesDialog::FillHistory(int comboId, const std::vector<std::wstring>& list) {
    HWND c = Item(comboId);
    std::wstring cur = GetWindowTextStr(c);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (auto& s : list) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SetWindowTextW(c, cur.c_str());
}

void FindFilesDialog::AddToHistory(std::vector<std::wstring>& list, const std::wstring& value) {
    if (value.empty()) return;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::wstring& s) { return s == value; }), list.end());
    list.insert(list.begin(), value);
    if ((int)list.size() > kHistoryMax) list.resize(kHistoryMax);
}

void FindFilesDialog::LoadSettings() {
    Config& c = App::Cfg();
    histPattern_ = c.GetList(kSection, L"Muster");
    histText_ = c.GetList(kSection, L"Text");
    FillHistory(CID_PATTERN, histPattern_);
    FillHistory(CID_TEXT, histText_);
    SetText(CID_PATTERN, histPattern_.empty() ? L"*" : histPattern_.front());
    SetCheck(CID_SUBDIRS, c.GetBool(kSection, L"Unterverzeichnisse", true));
    SetCheck(CID_MATCHCASE, c.GetBool(kSection, L"GrossKlein", false));
    SetCheck(CID_HIDDEN, c.GetBool(kSection, L"Versteckte", App::Opt().showHidden));
    SetCheck(CID_DIRNAMES, c.GetBool(kSection, L"Verzeichnisnamen", false));
}

void FindFilesDialog::SaveSettings() {
    Config& c = App::Cfg();
    c.SetList(kSection, L"Muster", histPattern_);
    c.SetList(kSection, L"Text", histText_);
    c.SetBool(kSection, L"Unterverzeichnisse", IsChecked(CID_SUBDIRS));
    c.SetBool(kSection, L"GrossKlein", IsChecked(CID_MATCHCASE));
    c.SetBool(kSection, L"Versteckte", IsChecked(CID_HIDDEN));
    c.SetBool(kSection, L"Verzeichnisnamen", IsChecked(CID_DIRNAMES));
}

void FindFilesDialog::SetRunning(bool on) {
    running_ = on;
    static const int inputs[] = {CID_PATTERN, CID_DIRS,    CID_BROWSE,  CID_TEXT,    CID_MATCHCASE, CID_SUBDIRS,
                                 CID_HIDDEN,  CID_DIRNAMES, CID_SIZEMIN, CID_SIZEMAX, CID_DATEMIN,   CID_DATEMAX};
    for (int id : inputs) Enable(id, !on);
    Enable(IDOK, !on);
    Enable(CID_STOP, on);
    // Standardschaltfläche: während der Suche keine
    SendMessageW(hwnd_, DM_SETDEFID, on ? CID_STOP : IDOK, 0);
    // Fokus nachführen, falls das fokussierte Element gerade deaktiviert wurde (nur im aktiven Dialog)
    if (GetActiveWindow() == hwnd_) {
        HWND f = GetFocus();
        bool lost = !f || !IsWindowEnabled(f) || (!on && f == Item(CID_STOP));
        if (lost) {
            HWND to = on ? Item(CID_STOP) : (ListView_GetItemCount(Item(CID_RESULTS)) > 0 ? Item(CID_RESULTS) : Item(CID_PATTERN));
            SendMessageW(hwnd_, WM_NEXTDLGCTL, (WPARAM)to, TRUE);
        }
    }
}

void FindFilesDialog::StartSearch() {
    if (running_) return;
    SearchParams p;
    // Verzeichnisse
    for (auto& d : Split(GetText(CID_DIRS), L';')) {
        std::wstring t = Trim(d);
        if (t.empty()) continue;
        std::wstring n = NormalizeDir(t);
        if (!DirExists(n)) {
            MsgError(hwnd_, L"Das Verzeichnis „" + t + L"“ wurde nicht gefunden.");
            SetFocus(Item(CID_DIRS));
            return;
        }
        bool dup = false;
        for (auto& x : p.dirs)
            if (EqualsI(x, n)) dup = true;
        if (!dup) p.dirs.push_back(n);
    }
    if (p.dirs.empty()) {
        MsgError(hwnd_, L"Bitte mindestens ein Verzeichnis angeben.");
        SetFocus(Item(CID_DIRS));
        return;
    }
    std::wstring patternInput = Trim(GetText(CID_PATTERN));
    p.patterns = PreparePatterns(patternInput);
    p.text = GetText(CID_TEXT);
    p.matchCase = IsChecked(CID_MATCHCASE);
    p.subdirs = IsChecked(CID_SUBDIRS);
    p.hidden = IsChecked(CID_HIDDEN);
    p.dirNames = IsChecked(CID_DIRNAMES);
    std::wstring smin = Trim(GetText(CID_SIZEMIN)), smax = Trim(GetText(CID_SIZEMAX));
    if (!smin.empty()) {
        p.hasSizeMin = true;
        p.sizeMin = (uint64_t)std::max<long long>(0, StrToInt(smin)) * 1024ull;
    }
    if (!smax.empty()) {
        p.hasSizeMax = true;
        p.sizeMax = (uint64_t)std::max<long long>(0, StrToInt(smax)) * 1024ull;
    }
    if (p.hasSizeMin && p.hasSizeMax && p.sizeMin > p.sizeMax) {
        MsgError(hwnd_, L"Die Mindestgröße ist größer als die Höchstgröße.");
        return;
    }
    SYSTEMTIME st{};
    if (DateTime_GetSystemtime(Item(CID_DATEMIN), &st) == GDT_VALID) p.hasDateMin = DayStartUtc(st, p.dateMin);
    if (DateTime_GetSystemtime(Item(CID_DATEMAX), &st) == GDT_VALID) {
        p.hasDateMax = DayStartUtc(st, p.dateMax);
        p.dateMax += kOneDay; // ganzer Tag einschließlich
    }
    if (p.hasDateMin && p.hasDateMax && p.dateMin >= p.dateMax) {
        MsgError(hwnd_, L"Das Anfangsdatum liegt nach dem Enddatum.");
        return;
    }

    // Verlauf
    AddToHistory(histPattern_, patternInput);
    AddToHistory(histText_, p.text);
    FillHistory(CID_PATTERN, histPattern_);
    FillHistory(CID_TEXT, histText_);
    SaveSettings();

    // Zurücksetzen
    results_.clear();
    ListView_SetItemCountEx(Item(CID_RESULTS), 0, 0);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_.clear();
        curDir_.clear();
    }
    stop_ = false;
    postPending_ = false;
    scanned_ = 0;
    SetRunning(true);
    SetText(CID_STATUS, L"Suche läuft …");
    HWND target = hwnd_;
    worker_ = std::thread([this, params = std::move(p), target]() mutable { Run(std::move(params), target); });
}

void FindFilesDialog::StopWorker() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

// Arbeitsthread: Verzeichnisse iterativ (Tiefensuche) durchlaufen.
void FindFilesDialog::Run(SearchParams p, HWND target) {
    std::unique_ptr<TextMatcher> matcher;
    if (!p.text.empty()) matcher = std::make_unique<TextMatcher>(p.text, p.matchCase);
    std::vector<uint8_t> buf;
    ULONGLONG lastPost = GetTickCount64();
    std::vector<Hit> local;
    auto flush = [&](bool force) {
        ULONGLONG now = GetTickCount64();
        if (!force && now - lastPost < 100) return;
        lastPost = now;
        if (!local.empty()) {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto& h : local) pending_.push_back(std::move(h));
            local.clear();
        }
        if (!postPending_.exchange(true)) PostMessageW(target, kMsgResults, 0, 0);
    };
    auto dateOk = [&](uint64_t t) {
        if (p.hasDateMin && t < p.dateMin) return false;
        if (p.hasDateMax && t >= p.dateMax) return false;
        return true;
    };

    std::vector<std::wstring> stack(p.dirs.rbegin(), p.dirs.rend());
    while (!stack.empty() && !stop_) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            curDir_ = dir;
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(LongPath(PathCombine(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> subdirs;
        do {
            if (stop_) break;
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            const DWORD a = fd.dwFileAttributes;
            if (!p.hidden && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            const bool isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wstring name = n;
            const uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            const uint64_t mt = FtToU64(fd.ftLastWriteTime);
            if (isDir) {
                if (p.subdirs && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) subdirs.push_back(PathCombine(dir, name));
                if (p.dirNames && !matcher && !p.hasSizeMin && !p.hasSizeMax && dateOk(mt) &&
                    MatchAnyPattern(p.patterns, name)) {
                    Hit hit;
                    hit.name = std::move(name);
                    hit.dir = dir;
                    hit.modified = fd.ftLastWriteTime;
                    hit.isDir = true;
                    local.push_back(std::move(hit));
                }
            } else {
                scanned_.fetch_add(1, std::memory_order_relaxed);
                if (!MatchAnyPattern(p.patterns, name)) continue;
                if (p.hasSizeMin && size < p.sizeMin) continue;
                if (p.hasSizeMax && size > p.sizeMax) continue;
                if (!dateOk(mt)) continue;
                if (matcher) {
                    flush(false);
                    if (!matcher->FileContains(PathCombine(dir, name), stop_, buf)) continue;
                }
                Hit hit;
                hit.name = std::move(name);
                hit.dir = dir;
                hit.size = size;
                hit.modified = fd.ftLastWriteTime;
                local.push_back(std::move(hit));
            }
            flush(false);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(std::move(*it));
        flush(false);
    }
    flush(true);
    PostMessageW(target, kMsgDone, 0, 0);
}

void FindFilesDialog::UpdateStatus(const std::wstring& curDir) {
    std::wstring s = IntToStrGrouped(results_.size()) + L" Treffer, " +
                     IntToStrGrouped(scanned_.load()) + L" Dateien durchsucht";
    if (running_ && !curDir.empty()) s += L", aktuelles Verzeichnis: " + curDir;
    SetText(CID_STATUS, s);
}

void FindFilesDialog::Drain() {
    postPending_ = false;
    std::vector<Hit> got;
    std::wstring cur;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        got.swap(pending_);
        cur = curDir_;
    }
    if (!got.empty()) {
        results_.reserve(results_.size() + got.size());
        for (auto& h : got) results_.push_back(std::move(h));
        ListView_SetItemCountEx(Item(CID_RESULTS), (int)results_.size(), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    }
    UpdateStatus(cur);
}

std::vector<size_t> FindFilesDialog::SelectedIndices() const {
    std::vector<size_t> r;
    HWND lv = Item(CID_RESULTS);
    int i = -1;
    while ((i = ListView_GetNextItem(lv, i, LVNI_SELECTED)) >= 0)
        if ((size_t)i < results_.size()) r.push_back((size_t)i);
    return r;
}

std::vector<std::wstring> FindFilesDialog::SelectedPaths() const {
    std::vector<std::wstring> r;
    for (size_t i : SelectedIndices()) r.push_back(results_[i].Path());
    return r;
}

int CompareHits(const Hit& a, const Hit& b, int col) {
    int r = 0;
    switch (col) {
    case 0:
        r = CompareNatural(a.name, b.name);
        if (!r) r = CompareI(a.dir, b.dir);
        break;
    case 1:
        r = CompareI(a.dir, b.dir);
        if (!r) r = CompareNatural(a.name, b.name);
        break;
    case 2:
        if (a.isDir != b.isDir)
            r = a.isDir ? -1 : 1;
        else
            r = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        break;
    case 3:
        r = CompareFileTime(&a.modified, &b.modified);
        break;
    }
    return r;
}

void FindFilesDialog::SortResults() {
    if (sortCol_ < 0 || results_.empty()) {
        UpdateSortArrow();
        return;
    }
    HWND lv = Item(CID_RESULTS);
    std::vector<char> sel(results_.size(), 0);
    for (size_t i : SelectedIndices()) sel[i] = 1;
    int focus = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
    std::vector<size_t> order(results_.size());
    std::iota(order.begin(), order.end(), size_t(0));
    const int col = sortCol_;
    const bool asc = sortAsc_;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        int r = CompareHits(results_[x], results_[y], col);
        return asc ? r < 0 : r > 0;
    });
    std::vector<Hit> sorted;
    sorted.reserve(results_.size());
    std::vector<char> newSel(results_.size(), 0);
    int newFocus = -1;
    for (size_t k = 0; k < order.size(); ++k) {
        sorted.push_back(std::move(results_[order[k]]));
        newSel[k] = sel[order[k]];
        if ((int)order[k] == focus) newFocus = (int)k;
    }
    results_.swap(sorted);
    ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (size_t k = 0; k < newSel.size(); ++k)
        if (newSel[k]) ListView_SetItemState(lv, (int)k, LVIS_SELECTED, LVIS_SELECTED);
    if (newFocus >= 0) {
        ListView_SetItemState(lv, newFocus, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_EnsureVisible(lv, newFocus, FALSE);
    }
    InvalidateRect(lv, nullptr, FALSE);
    UpdateSortArrow();
}

void FindFilesDialog::UpdateSortArrow() {
    HWND hdr = ListView_GetHeader(Item(CID_RESULTS));
    int n = Header_GetItemCount(hdr);
    for (int i = 0; i < n; ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sortCol_) hi.fmt |= sortAsc_ ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hi);
    }
}

void FindFilesDialog::ShowContextMenu(POINT pt) {
    auto idx = SelectedIndices();
    if (idx.empty()) return;
    bool anyFile = false;
    for (size_t i : idx)
        if (!results_[i].isDir) anyFile = true;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_GOTO, L"&Gehe zu\tEnter");
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Ö&ffnen");
    AppendMenuW(m, MF_STRING | (anyFile ? 0 : MF_GRAYED), CMD_EDIT, L"&Bearbeiten\tF4");
    AppendMenuW(m, MF_STRING, CMD_VIEW, L"&Anzeigen\tF3");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_COPYPATHS, L"&Pfade kopieren\tStrg+Umschalt+C");
    AppendMenuW(m, MF_STRING, CMD_COPYFILES, L"&Dateien in Zwischenablage\tStrg+C");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_DELETE, L"&Löschen …\tEntf");
    SetMenuDefaultItem(m, CMD_GOTO, FALSE);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd) DoCommand(cmd);
}

void FindFilesDialog::DoCommand(int cmd) {
    HWND lv = Item(CID_RESULTS);
    auto idx = SelectedIndices();
    if (idx.empty()) return;
    switch (cmd) {
    case CMD_GOTO: {
        int f = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
        size_t i = (f >= 0 && (size_t)f < results_.size() &&
                    (ListView_GetItemState(lv, f, LVIS_SELECTED) & LVIS_SELECTED))
                       ? (size_t)f
                       : idx.front();
        App::NavigateToFile(results_[i].Path());
        break;
    }
    case CMD_OPEN:
        if (idx.size() > 10 &&
            !MsgConfirm(hwnd_, IntToStrGrouped(idx.size()) + L" Elemente öffnen?"))
            return;
        for (size_t i : idx) ShellOpen(hwnd_, results_[i].Path());
        break;
    case CMD_EDIT: {
        size_t n = 0;
        for (size_t i : idx)
            if (!results_[i].isDir) ++n;
        if (n > 10 && !MsgConfirm(hwnd_, IntToStrGrouped(n) + L" Dateien im Editor öffnen?")) return;
        for (size_t i : idx)
            if (!results_[i].isDir) OpenTextEditor(results_[i].Path());
        break;
    }
    case CMD_VIEW:
        OpenViewerWindow(results_[idx.front()].Path());
        break;
    case CMD_COPYPATHS: {
        std::vector<std::wstring> paths = SelectedPaths();
        ClipboardSetText(hwnd_, Join(paths, L"\r\n"));
        break;
    }
    case CMD_COPYFILES:
        ClipboardSetFiles(hwnd_, SelectedPaths(), false);
        break;
    case CMD_DELETE: {
        std::vector<std::wstring> paths = SelectedPaths();
        DeleteItems(hwnd_, paths, App::Opt().useRecycleBin, App::Opt().confirmDelete);
        // Nicht mehr vorhandene Einträge entfernen (auch bei teilweisem Erfolg)
        std::vector<char> gone(results_.size(), 0);
        bool any = false;
        for (size_t i : idx)
            if (!PathExists(results_[i].Path())) {
                gone[i] = 1;
                any = true;
            }
        // Treffer unterhalb gelöschter Verzeichnisse ebenfalls entfernen
        for (size_t i : idx) {
            if (!gone[i] || !results_[i].isDir) continue;
            std::wstring prefix = results_[i].Path() + L"\\";
            for (size_t k = 0; k < results_.size(); ++k)
                if (!gone[k] && (EqualsI(results_[k].dir, results_[i].Path()) || StartsWithI(results_[k].dir, prefix)))
                    gone[k] = 1;
        }
        if (any) {
            std::vector<Hit> keep;
            keep.reserve(results_.size());
            for (size_t k = 0; k < results_.size(); ++k)
                if (!gone[k]) keep.push_back(std::move(results_[k]));
            results_.swap(keep);
            ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemCountEx(lv, (int)results_.size(), 0);
            InvalidateRect(lv, nullptr, FALSE);
            UpdateStatus(L"");
            if (!App::Opt().autoRefresh) App::RefreshPanes();
        }
        break;
    }
    }
}

BOOL FindFilesDialog::OnCommand(int id, int code, HWND ctl) {
    switch (id) {
    case IDOK:
        // Enter in der Ergebnisliste = "Gehe zu"
        if (GetFocus() == Item(CID_RESULTS)) {
            DoCommand(CMD_GOTO);
            return TRUE;
        }
        StartSearch();
        return TRUE;
    case CID_STOP:
        // Während der Suche ist "Stopp" Standardschaltfläche: Enter in der Liste bleibt "Gehe zu"
        if (GetFocus() == Item(CID_RESULTS)) {
            DoCommand(CMD_GOTO);
            return TRUE;
        }
        stop_ = true;
        Enable(CID_STOP, false);
        SetText(CID_STATUS, L"Wird angehalten …");
        return TRUE;
    case CID_BROWSE: {
        auto dirs = Split(GetText(CID_DIRS), L';');
        std::wstring init = dirs.empty() ? startDir_ : Trim(dirs.front());
        std::wstring d = BrowseForFolder(hwnd_, L"Suchen in", init);
        if (!d.empty()) SetText(CID_DIRS, d);
        return TRUE;
    }
    case IDCANCEL:
        End(IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR FindFilesDialog::OnNotify(NMHDR* nm) {
    if (nm->idFrom != CID_RESULTS) return 0;
    switch (nm->code) {
    case LVN_GETDISPINFOW: {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        if (!(di->item.mask & LVIF_TEXT) || di->item.iItem < 0 || (size_t)di->item.iItem >= results_.size()) return 0;
        const Hit& h = results_[(size_t)di->item.iItem];
        switch (di->item.iSubItem) {
        case 0: dispBuf_ = h.name; break;
        case 1: dispBuf_ = h.dir; break;
        case 2:
            dispBuf_ = h.isDir ? L"<Verzeichnis>" : (App::Opt().sizeInBytes ? FormatSizeBytes(h.size) : FormatSize(h.size));
            break;
        case 3: dispBuf_ = FormatFileTime(h.modified); break;
        default: dispBuf_.clear();
        }
        di->item.pszText = const_cast<wchar_t*>(dispBuf_.c_str());
        return 0;
    }
    case LVN_ODFINDITEMW: {
        auto* fi = reinterpret_cast<NMLVFINDITEMW*>(nm);
        if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !fi->lvfi.psz || results_.empty()) return -1;
        std::wstring s = fi->lvfi.psz;
        size_t n = results_.size();
        size_t start = fi->iStart >= 0 ? (size_t)fi->iStart % n : 0;
        for (size_t k = 0; k < n; ++k) {
            size_t i = (start + k) % n;
            if (StartsWithI(results_[i].name, s)) return (INT_PTR)i;
        }
        return -1;
    }
    case LVN_COLUMNCLICK: {
        int col = reinterpret_cast<NMLISTVIEW*>(nm)->iSubItem;
        if (col == sortCol_)
            sortAsc_ = !sortAsc_;
        else {
            sortCol_ = col;
            sortAsc_ = true;
        }
        SortResults();
        return 0;
    }
    case NM_DBLCLK: {
        auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
        if (ia->iItem >= 0 && (size_t)ia->iItem < results_.size()) App::NavigateToFile(results_[(size_t)ia->iItem].Path());
        return 0;
    }
    case LVN_KEYDOWN: {
        auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        switch (kd->wVKey) {
        case VK_DELETE: DoCommand(CMD_DELETE); break;
        case VK_F3: DoCommand(CMD_VIEW); break;
        case VK_F4: DoCommand(CMD_EDIT); break;
        case 'C':
            if (ctrl) DoCommand(shift ? CMD_COPYPATHS : CMD_COPYFILES);
            break;
        case 'A':
            if (ctrl) ListView_SetItemState(Item(CID_RESULTS), -1, LVIS_SELECTED, LVIS_SELECTED);
            break;
        }
        return 0;
    }
    }
    return 0;
}

INT_PTR FindFilesDialog::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kMsgResults:
        Drain();
        return TRUE;
    case kMsgDone: {
        Drain();
        if (worker_.joinable()) worker_.join();
        bool aborted = stop_.load();
        SetRunning(false);
        if (sortCol_ >= 0) SortResults();
        std::wstring s = (aborted ? L"Abgebrochen: " : L"Fertig: ") + IntToStrGrouped(results_.size()) + L" Treffer, " +
                         IntToStrGrouped(scanned_.load()) + L" Dateien durchsucht.";
        SetText(CID_STATUS, s);
        return TRUE;
    }
    case WM_CONTEXTMENU: {
        HWND lv = Item(CID_RESULTS);
        if ((HWND)wp != lv) return FALSE;
        POINT pt{(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
        if (lp == -1) {
            int f = ListView_GetNextItem(lv, -1, LVNI_FOCUSED | LVNI_SELECTED);
            if (f < 0) f = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
            RECT rc{};
            if (f >= 0 && ListView_GetItemRect(lv, f, &rc, LVIR_LABEL)) {
                pt = {rc.left + DpiScale(hwnd_, 8), rc.bottom};
            } else {
                pt = {DpiScale(hwnd_, 8), DpiScale(hwnd_, 8)};
            }
            ClientToScreen(lv, &pt);
        }
        ShowContextMenu(pt);
        return TRUE;
    }
    }
    return FALSE;
}

void FindFilesDialog::OnDestroy() {
    StopWorker();
    SaveSettings();
}

} // namespace

void FindFiles(HWND owner, const std::wstring& startDir) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    DialogTemplate t(L"Dateien suchen", 470, 318, DialogTemplate::kResizable);
    t.Label(CID_LBL_PATTERN, L"&Name:", 7, 9, 50, 8);
    t.Combo(CID_PATTERN, 60, 7, 330, 150, true);
    t.Label(CID_LBL_DIRS, L"Suchen &in:", 7, 25, 50, 8);
    t.Edit(CID_DIRS, 60, 23, 314, 12);
    t.Button(CID_BROWSE, L"…", 376, 22, 14, 14);
    t.Label(CID_LBL_TEXT, L"&Text enthält:", 7, 41, 50, 8);
    t.Combo(CID_TEXT, 60, 39, 330, 150, true);
    t.Check(CID_MATCHCASE, L"&Groß-/Kleinschreibung beachten", 60, 55, 140, 10);
    t.Check(CID_SUBDIRS, L"&Unterverzeichnisse", 60, 68, 80, 10);
    t.Check(CID_HIDDEN, L"Versteckte/S&ystemdateien", 145, 68, 105, 10);
    t.Check(CID_DIRNAMES, L"Auch &Verzeichnisse finden", 255, 68, 110, 10);
    t.Label(CID_LBL_SIZEMIN, L"Größe v&on:", 7, 85, 50, 8);
    t.Edit(CID_SIZEMIN, 60, 83, 50, 12, ES_AUTOHSCROLL | ES_NUMBER);
    t.Label(CID_LBL_SIZEMAX, L"&bis:", 115, 85, 16, 8);
    t.Edit(CID_SIZEMAX, 133, 83, 50, 12, ES_AUTOHSCROLL | ES_NUMBER);
    t.Label(CID_LBL_KB, L"KB", 187, 85, 16, 8);
    t.Label(CID_LBL_DATEMIN, L"&Datum von:", 7, 101, 50, 8);
    t.DateTime(CID_DATEMIN, 60, 99, 75, 13, DTS_SHOWNONE | DTS_SHORTDATEFORMAT);
    t.Label(CID_LBL_DATEMAX, L"bis:", 140, 101, 16, 8);
    t.DateTime(CID_DATEMAX, 158, 99, 75, 13, DTS_SHOWNONE | DTS_SHORTDATEFORMAT);
    t.DefButton(IDOK, L"&Starten", 400, 6, 63, 14);
    t.Button(CID_STOP, L"Sto&pp", 400, 23, 63, 14);
    t.Button(IDCANCEL, L"Schließen", 400, 40, 63, 14);
    t.ListView(CID_RESULTS, 7, 118, 456, 180, LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA);
    t.Label(CID_STATUS, L"", 7, 303, 456, 8, SS_ENDELLIPSIS);

    auto* dlg = new FindFilesDialog(startDir);
    if (!dlg->CreateModeless(owner, t, true)) delete dlg;
}

} // namespace qf
