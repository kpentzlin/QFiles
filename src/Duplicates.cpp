// Duplicates.cpp – Modul D: Doppelte Dateien suchen (gleicher Inhalt)
//
// Nicht modaler, veränderbarer Dialog. Ablauf im Arbeitsthread:
//   1. Dateien sammeln (Muster, Mindestgröße, 0-Byte-Dateien werden ignoriert)
//   2. nach Größe gruppieren
//   3. xxHash64 der ersten 64 KB
//   4. xxHash64 der ganzen Datei (nur falls größer als 64 KB)
//   5. byteweiser Vergleich zur Bestätigung
// Bestätigte Gruppen werden gebündelt per PostMessageW an den Dialog gemeldet und in einer
// ListView mit Gruppen (ListView-Gruppen) und Kontrollkästchen angezeigt.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "FileOps.h"
#include "Settings.h"
#include "Util.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qf {

namespace {

constexpr wchar_t kSection[] = L"Doppelte";
constexpr UINT kMsgProgress = WM_APP + 1;
constexpr UINT kMsgDone = WM_APP + 2;
constexpr uint64_t kPartialBytes = 64 * 1024;
constexpr DWORD kReadBlock = 1u << 20;

enum : int {
    CID_LBL_DIRS = 101,
    CID_DIRS,
    CID_BROWSE,
    CID_LBL_PATTERN,
    CID_PATTERN,
    CID_LBL_MINSIZE,
    CID_MINSIZE,
    CID_LBL_BYTES,
    CID_SUBDIRS,
    CID_HIDDEN,
    CID_STOP,
    CID_LIST,
    CID_SUMMARY,
    CID_AUTOMARK,
    CID_UNMARK,
    CID_DELETE,
    CID_GOTO,
    CID_STATUS,
};

enum : int {
    CMD_KEEP_OLDEST = 1001,
    CMD_KEEP_NEWEST,
    CMD_KEEP_FIRST,
    CMD_GOTO,
    CMD_OPEN,
    CMD_VIEW,
    CMD_COPYPATHS,
    CMD_CHECK,
    CMD_UNCHECK,
};

// ---------------------------------------------------------------------------
// xxHash64 (eigene Implementierung, blockweise)
// ---------------------------------------------------------------------------

class Xxh64 {
public:
    explicit Xxh64(uint64_t seed = 0) { Reset(seed); }
    void Reset(uint64_t seed) {
        seed_ = seed;
        v1_ = seed + P1 + P2;
        v2_ = seed + P2;
        v3_ = seed;
        v4_ = seed - P1;
        total_ = 0;
        memSize_ = 0;
    }
    void Update(const uint8_t* p, size_t n) {
        total_ += n;
        if (memSize_ + n < 32) {
            if (n) std::memcpy(mem_ + memSize_, p, n);
            memSize_ += n;
            return;
        }
        const uint8_t* end = p + n;
        if (memSize_) {
            size_t fill = 32 - memSize_;
            std::memcpy(mem_ + memSize_, p, fill);
            v1_ = Round(v1_, Read64(mem_));
            v2_ = Round(v2_, Read64(mem_ + 8));
            v3_ = Round(v3_, Read64(mem_ + 16));
            v4_ = Round(v4_, Read64(mem_ + 24));
            p += fill;
            memSize_ = 0;
        }
        while (p + 32 <= end) {
            v1_ = Round(v1_, Read64(p));
            v2_ = Round(v2_, Read64(p + 8));
            v3_ = Round(v3_, Read64(p + 16));
            v4_ = Round(v4_, Read64(p + 24));
            p += 32;
        }
        if (p < end) {
            memSize_ = (size_t)(end - p);
            std::memcpy(mem_, p, memSize_);
        }
    }
    uint64_t Digest() const {
        uint64_t h;
        if (total_ >= 32) {
            h = Rotl(v1_, 1) + Rotl(v2_, 7) + Rotl(v3_, 12) + Rotl(v4_, 18);
            h = Merge(h, v1_);
            h = Merge(h, v2_);
            h = Merge(h, v3_);
            h = Merge(h, v4_);
        } else {
            h = seed_ + P5;
        }
        h += total_;
        const uint8_t* p = mem_;
        const uint8_t* end = mem_ + memSize_;
        while (p + 8 <= end) {
            h ^= Round(0, Read64(p));
            h = Rotl(h, 27) * P1 + P4;
            p += 8;
        }
        if (p + 4 <= end) {
            h ^= (uint64_t)Read32(p) * P1;
            h = Rotl(h, 23) * P2 + P3;
            p += 4;
        }
        while (p < end) {
            h ^= (uint64_t)(*p) * P5;
            h = Rotl(h, 11) * P1;
            ++p;
        }
        h ^= h >> 33;
        h *= P2;
        h ^= h >> 29;
        h *= P3;
        h ^= h >> 32;
        return h;
    }

private:
    static constexpr uint64_t P1 = 11400714785074694791ull;
    static constexpr uint64_t P2 = 14029467366897019727ull;
    static constexpr uint64_t P3 = 1609587929392839161ull;
    static constexpr uint64_t P4 = 9650029242287828579ull;
    static constexpr uint64_t P5 = 2870177450012600261ull;
    static uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    static uint64_t Read64(const uint8_t* p) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
    static uint32_t Read32(const uint8_t* p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static uint64_t Round(uint64_t acc, uint64_t input) {
        acc += input * P2;
        acc = Rotl(acc, 31);
        acc *= P1;
        return acc;
    }
    static uint64_t Merge(uint64_t acc, uint64_t val) {
        val = Round(0, val);
        acc ^= val;
        acc = acc * P1 + P4;
        return acc;
    }
    uint64_t seed_ = 0, v1_ = 0, v2_ = 0, v3_ = 0, v4_ = 0, total_ = 0;
    uint8_t mem_[32]{};
    size_t memSize_ = 0;
};

HANDLE OpenForRead(const std::wstring& path) {
    return CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

// Liest bis zu n Bytes (wiederholt, bis voll oder Dateiende). Rückgabe false bei Lesefehler.
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

// ---------------------------------------------------------------------------
// Daten
// ---------------------------------------------------------------------------

struct FoundFile {
    std::wstring path;
    uint64_t size = 0;
    FILETIME modified{};
};

struct DupFile {
    std::wstring name, dir;
    uint64_t size = 0;
    FILETIME modified{};
    int group = -1;
    bool removed = false; // nicht mehr in der Liste
    std::wstring Path() const { return PathCombine(dir, name); }
};

struct DupGroup {
    uint64_t size = 0;
    std::vector<size_t> files; // Indizes in files_ (Reihenfolge = Fundreihenfolge)
    bool removed = false;
};

struct DupParams {
    std::vector<std::wstring> dirs;
    std::wstring patterns;
    uint64_t minSize = 1;
    bool subdirs = true, hidden = false;
};

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

class DuplicatesDialog : public DialogBase {
public:
    explicit DuplicatesDialog(const std::wstring& startDir) : startDir_(startDir) {}

protected:
    BOOL OnInit() override;
    BOOL OnCommand(int id, int code, HWND ctl) override;
    INT_PTR OnNotify(NMHDR* nm) override;
    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnDestroy() override;

private:
    // Oberfläche
    void Start();
    void SetRunning(bool on);
    void Drain();
    void AddGroups(std::vector<std::vector<FoundFile>>& groups);
    std::wstring GroupHeader(int g) const;
    void UpdateSummary();
    void UpdateProgressStatus();
    size_t FileOfItem(int item) const;
    int FocusedItem() const;
    void AutoMark(int mode);
    void SetAllChecks(bool on, bool selectedOnly);
    void DeleteMarked();
    void RemoveMissing();
    void ShowContextMenu(POINT pt);
    void DoCommand(int cmd);
    void LoadSettings();
    void SaveSettings();

    // Arbeitsthread
    void Run(DupParams p);
    void Tick(bool force);
    bool HashFile(const std::wstring& path, uint64_t maxBytes, std::vector<uint8_t>& buf, uint64_t& out);
    int SameContent(const std::wstring& a, const std::wstring& b, std::vector<uint8_t>& ba, std::vector<uint8_t>& bb);
    void Emit(const std::vector<FoundFile>& all, const std::vector<size_t>& idx);

    std::wstring startDir_;
    std::vector<DupFile> files_;
    std::vector<DupGroup> groups_;
    bool running_ = false;
    bool bulk_ = false;
    std::wstring dispBuf_;

    std::thread worker_;
    HWND target_ = nullptr;
    ULONGLONG lastPost_ = 0;                 // nur Arbeitsthread
    std::atomic<bool> stop_{false};
    std::atomic<bool> postPending_{false};
    std::atomic<int> phase_{0};              // 1 = sammeln, 2 = vergleichen
    std::atomic<uint64_t> filesFound_{0};
    std::atomic<uint64_t> sizeGroupsDone_{0};
    std::atomic<uint64_t> sizeGroupsTotal_{0};
    std::atomic<uint64_t> bytesRead_{0};
    std::atomic<uint64_t> readErrors_{0};
    std::mutex mtx_;
    std::vector<std::vector<FoundFile>> pending_; // geschützt durch mtx_
    std::wstring curDir_;                         // geschützt durch mtx_
};

BOOL DuplicatesDialog::OnInit() {
    HWND lv = Item(CID_LIST);
    LvAddColumn(CID_LIST, L"Name", 130);
    LvAddColumn(CID_LIST, L"Verzeichnis", 190);
    LvAddColumn(CID_LIST, L"Größe", 60, LVCFMT_RIGHT);
    LvAddColumn(CID_LIST, L"Geändert", 72);
    ListView_SetExtendedListViewStyleEx(lv, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
    ListView_EnableGroupView(lv, TRUE);

    SetText(CID_DIRS, startDir_);
    LoadSettings();

    SetAnchor(CID_DIRS, AnchorTopLeftRight);
    SetAnchor(CID_BROWSE, AnchorTopRight);
    SetAnchor(IDOK, AnchorTopRight);
    SetAnchor(CID_STOP, AnchorTopRight);
    SetAnchor(IDCANCEL, AnchorTopRight);
    SetAnchor(CID_LIST, AnchorAll);
    SetAnchor(CID_SUMMARY, AnchorBottomLeftRight);
    SetAnchor(CID_AUTOMARK, AnchorBottomLeft);
    SetAnchor(CID_UNMARK, AnchorBottomLeft);
    SetAnchor(CID_DELETE, AnchorBottomLeft);
    SetAnchor(CID_GOTO, AnchorBottomLeft);
    SetAnchor(CID_STATUS, AnchorBottomLeftRight);
    EnableResizing();

    SetRunning(false);
    UpdateSummary();
    SetText(CID_STATUS, L"Bereit.");
    return TRUE;
}

void DuplicatesDialog::LoadSettings() {
    Config& c = App::Cfg();
    SetText(CID_PATTERN, c.Get(kSection, L"Muster", L"*"));
    SetText(CID_MINSIZE, c.Get(kSection, L"Mindestgroesse", L"1"));
    SetCheck(CID_SUBDIRS, c.GetBool(kSection, L"Unterverzeichnisse", true));
    SetCheck(CID_HIDDEN, c.GetBool(kSection, L"Versteckte", false));
}

void DuplicatesDialog::SaveSettings() {
    Config& c = App::Cfg();
    c.Set(kSection, L"Muster", GetText(CID_PATTERN));
    c.Set(kSection, L"Mindestgroesse", GetText(CID_MINSIZE));
    c.SetBool(kSection, L"Unterverzeichnisse", IsChecked(CID_SUBDIRS));
    c.SetBool(kSection, L"Versteckte", IsChecked(CID_HIDDEN));
}

void DuplicatesDialog::SetRunning(bool on) {
    running_ = on;
    static const int inputs[] = {CID_DIRS, CID_BROWSE, CID_PATTERN, CID_MINSIZE, CID_SUBDIRS, CID_HIDDEN, CID_DELETE};
    for (int id : inputs) Enable(id, !on);
    Enable(IDOK, !on);
    Enable(CID_STOP, on);
    SendMessageW(hwnd_, DM_SETDEFID, on ? CID_STOP : IDOK, 0);
    // Fokus nachführen, falls das fokussierte Element gerade deaktiviert wurde (nur im aktiven Dialog)
    if (GetActiveWindow() == hwnd_) {
        HWND f = GetFocus();
        bool lost = !f || !IsWindowEnabled(f) || (!on && f == Item(CID_STOP));
        if (lost) {
            HWND to = on ? Item(CID_STOP) : (ListView_GetItemCount(Item(CID_LIST)) > 0 ? Item(CID_LIST) : Item(CID_DIRS));
            SendMessageW(hwnd_, WM_NEXTDLGCTL, (WPARAM)to, TRUE);
        }
    }
}

void DuplicatesDialog::Start() {
    if (running_) return;
    DupParams p;
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
        return;
    }
    p.patterns = Trim(GetText(CID_PATTERN));
    p.minSize = (uint64_t)std::max<long long>(1, GetInt(CID_MINSIZE, 1)); // 0-Byte-Dateien immer ignorieren
    p.subdirs = IsChecked(CID_SUBDIRS);
    p.hidden = IsChecked(CID_HIDDEN);
    SaveSettings();

    // Liste leeren
    HWND lv = Item(CID_LIST);
    bulk_ = true;
    ListView_DeleteAllItems(lv);
    ListView_RemoveAllGroups(lv);
    bulk_ = false;
    files_.clear();
    groups_.clear();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_.clear();
        curDir_.clear();
    }
    stop_ = false;
    postPending_ = false;
    phase_ = 1;
    filesFound_ = 0;
    sizeGroupsDone_ = 0;
    sizeGroupsTotal_ = 0;
    bytesRead_ = 0;
    readErrors_ = 0;
    target_ = hwnd_;
    lastPost_ = 0;
    SetRunning(true);
    UpdateSummary();
    UpdateProgressStatus();
    worker_ = std::thread([this, params = std::move(p)]() mutable { Run(std::move(params)); });
}

// ---------------- Arbeitsthread ----------------

void DuplicatesDialog::Tick(bool force) {
    ULONGLONG now = GetTickCount64();
    if (!force && now - lastPost_ < 100) return;
    lastPost_ = now;
    if (!postPending_.exchange(true)) PostMessageW(target_, kMsgProgress, 0, 0);
}

bool DuplicatesDialog::HashFile(const std::wstring& path, uint64_t maxBytes, std::vector<uint8_t>& buf, uint64_t& out) {
    HANDLE h = OpenForRead(path);
    if (h == INVALID_HANDLE_VALUE) {
        readErrors_++;
        return false;
    }
    buf.resize(kReadBlock);
    Xxh64 x;
    uint64_t done = 0;
    bool ok = true;
    while (done < maxBytes) {
        if (stop_) {
            ok = false;
            break;
        }
        DWORD want = (DWORD)std::min<uint64_t>(kReadBlock, maxBytes - done);
        DWORD got = 0;
        if (!ReadFull(h, buf.data(), want, got)) {
            ok = false;
            readErrors_++;
            break;
        }
        if (got == 0) break;
        x.Update(buf.data(), got);
        done += got;
        bytesRead_ += got;
        Tick(false);
    }
    CloseHandle(h);
    out = x.Digest();
    return ok;
}

// 1 = gleich, 0 = verschieden, -1 = Fehler/Abbruch
int DuplicatesDialog::SameContent(const std::wstring& a, const std::wstring& b, std::vector<uint8_t>& ba,
                                  std::vector<uint8_t>& bb) {
    HANDLE ha = OpenForRead(a);
    if (ha == INVALID_HANDLE_VALUE) {
        readErrors_++;
        return -1;
    }
    HANDLE hb = OpenForRead(b);
    if (hb == INVALID_HANDLE_VALUE) {
        CloseHandle(ha);
        readErrors_++;
        return -1;
    }
    ba.resize(kReadBlock);
    bb.resize(kReadBlock);
    int result = 1;
    for (;;) {
        if (stop_) {
            result = -1;
            break;
        }
        DWORD ga = 0, gb = 0;
        if (!ReadFull(ha, ba.data(), kReadBlock, ga) || !ReadFull(hb, bb.data(), kReadBlock, gb)) {
            readErrors_++;
            result = -1;
            break;
        }
        bytesRead_ += (uint64_t)ga + gb;
        Tick(false);
        if (ga != gb || std::memcmp(ba.data(), bb.data(), ga) != 0) {
            result = 0;
            break;
        }
        if (ga < kReadBlock) break; // beide am Ende
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return result;
}

void DuplicatesDialog::Emit(const std::vector<FoundFile>& all, const std::vector<size_t>& idx) {
    std::vector<FoundFile> g;
    std::vector<size_t> sorted = idx;
    std::sort(sorted.begin(), sorted.end()); // Fundreihenfolge (Reihenfolge der Verzeichnisse)
    for (size_t i : sorted) g.push_back(all[i]);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_.push_back(std::move(g));
    }
    Tick(false);
}

void DuplicatesDialog::Run(DupParams p) {
    // Phase 1: Dateien sammeln
    std::vector<FoundFile> files;
    std::unordered_set<std::wstring> seen;
    std::vector<std::wstring> stack(p.dirs.rbegin(), p.dirs.rend());
    while (!stack.empty() && !stop_) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            curDir_ = dir;
        }
        Tick(false);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(LongPath(PathCombine(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> subdirs;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            const DWORD a = fd.dwFileAttributes;
            if (!p.hidden && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                if (p.subdirs && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) subdirs.push_back(PathCombine(dir, n));
                continue;
            }
            const uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (size == 0 || size < p.minSize) continue;
            std::wstring name = n;
            if (!MatchAnyPattern(p.patterns, name)) continue;
            FoundFile f;
            f.path = PathCombine(dir, name);
            if (!seen.insert(ToUpper(f.path)).second) continue; // überlappende Verzeichnisangaben
            f.size = size;
            f.modified = fd.ftLastWriteTime;
            files.push_back(std::move(f));
            filesFound_++;
        } while (!stop_ && FindNextFileW(h, &fd));
        FindClose(h);
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(std::move(*it));
    }
    seen.clear();

    // Phase 2: nach Größe gruppieren, größte zuerst
    phase_ = 2;
    std::unordered_map<uint64_t, std::vector<size_t>> bySize;
    for (size_t i = 0; i < files.size() && !stop_; ++i) bySize[files[i].size].push_back(i);
    std::vector<std::pair<uint64_t, std::vector<size_t>>> cand;
    for (auto& kv : bySize)
        if (kv.second.size() >= 2) cand.emplace_back(kv.first, std::move(kv.second));
    bySize.clear();
    std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    sizeGroupsTotal_ = cand.size();
    Tick(true);

    std::vector<uint8_t> buf1, buf2;
    for (auto& c : cand) {
        if (stop_) break;
        const uint64_t size = c.first;
        const uint64_t partLen = std::min<uint64_t>(size, kPartialBytes);
        // Hash der ersten 64 KB
        std::unordered_map<uint64_t, std::vector<size_t>> byPart;
        for (size_t idx : c.second) {
            if (stop_) break;
            uint64_t hv = 0;
            if (HashFile(files[idx].path, partLen, buf1, hv)) byPart[hv].push_back(idx);
        }
        for (auto& pg : byPart) {
            if (stop_) break;
            if (pg.second.size() < 2) continue;
            std::vector<std::vector<size_t>> fullGroups;
            if (size > partLen) {
                // vollständiger Hash
                std::unordered_map<uint64_t, std::vector<size_t>> byFull;
                for (size_t idx : pg.second) {
                    if (stop_) break;
                    uint64_t hv = 0;
                    if (HashFile(files[idx].path, size, buf1, hv)) byFull[hv].push_back(idx);
                }
                for (auto& fg : byFull)
                    if (fg.second.size() >= 2) fullGroups.push_back(std::move(fg.second));
            } else {
                fullGroups.push_back(std::move(pg.second));
            }
            // Byteweise bestätigen
            for (auto& fg : fullGroups) {
                std::vector<size_t> rest = fg;
                while (rest.size() >= 2 && !stop_) {
                    std::vector<size_t> same{rest[0]}, other;
                    for (size_t k = 1; k < rest.size() && !stop_; ++k) {
                        int r = SameContent(files[rest[0]].path, files[rest[k]].path, buf1, buf2);
                        if (r == 1)
                            same.push_back(rest[k]);
                        else if (r == 0)
                            other.push_back(rest[k]);
                    }
                    if (same.size() >= 2 && !stop_) Emit(files, same);
                    rest.swap(other);
                }
            }
        }
        sizeGroupsDone_++;
        Tick(false);
    }
    Tick(true);
    PostMessageW(target_, kMsgDone, 0, 0);
}

// ---------------- Oberfläche ----------------

std::wstring DuplicatesDialog::GroupHeader(int g) const {
    const DupGroup& grp = groups_[(size_t)g];
    size_t n = 0;
    for (size_t f : grp.files)
        if (!files_[f].removed) ++n;
    return L"Gruppe " + std::to_wstring(g + 1) + L" – " + std::to_wstring(n) + L" Dateien zu je " + FormatSize(grp.size) +
           L" (" + FormatSizeBytes(grp.size) + L" Bytes), verschwendet: " +
           FormatSize(grp.size * (n > 0 ? n - 1 : 0));
}

void DuplicatesDialog::AddGroups(std::vector<std::vector<FoundFile>>& groups) {
    HWND lv = Item(CID_LIST);
    bulk_ = true;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    for (auto& g : groups) {
        if (g.size() < 2) continue;
        int gid = (int)groups_.size();
        DupGroup grp;
        grp.size = g.front().size;
        for (auto& f : g) {
            DupFile df;
            df.name = PathFileName(f.path);
            df.dir = PathParent(f.path);
            df.size = f.size;
            df.modified = f.modified;
            df.group = gid;
            grp.files.push_back(files_.size());
            files_.push_back(std::move(df));
        }
        groups_.push_back(std::move(grp));

        std::wstring header = GroupHeader(gid);
        LVGROUP lg{};
        lg.cbSize = sizeof(lg);
        lg.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
        lg.iGroupId = gid + 1;
        lg.pszHeader = header.data();
        lg.state = LVGS_COLLAPSIBLE;
        lg.stateMask = LVGS_COLLAPSIBLE;
        ListView_InsertGroup(lv, -1, &lg);
        for (size_t fi : groups_[(size_t)gid].files) {
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
            it.iItem = INT_MAX;
            it.iGroupId = gid + 1;
            it.lParam = (LPARAM)fi;
            it.pszText = LPSTR_TEXTCALLBACKW;
            int row = ListView_InsertItem(lv, &it);
            if (row < 0) continue;
            for (int col = 1; col < 4; ++col) ListView_SetItemText(lv, row, col, LPSTR_TEXTCALLBACKW);
        }
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
    bulk_ = false;
    UpdateSummary();
}

void DuplicatesDialog::Drain() {
    postPending_ = false;
    std::vector<std::vector<FoundFile>> got;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        got.swap(pending_);
    }
    if (!got.empty()) AddGroups(got);
    if (running_) UpdateProgressStatus();
}

void DuplicatesDialog::UpdateProgressStatus() {
    std::wstring s;
    if (phase_ == 1) {
        std::wstring cur;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            cur = curDir_;
        }
        s = L"Dateien werden gesammelt: " + IntToStrGrouped(filesFound_.load()) + L" Dateien";
        if (!cur.empty()) s += L" – " + cur;
    } else {
        s = L"Inhalte werden verglichen: Größengruppe " + IntToStrGrouped(sizeGroupsDone_.load()) + L" von " +
            IntToStrGrouped(sizeGroupsTotal_.load()) + L", " + FormatSize(bytesRead_.load()) + L" gelesen";
    }
    SetText(CID_STATUS, s);
}

void DuplicatesDialog::UpdateSummary() {
    size_t groups = 0, dupFiles = 0, marked = 0;
    uint64_t wasted = 0, markedBytes = 0;
    for (auto& g : groups_) {
        if (g.removed) continue;
        size_t n = 0;
        for (size_t f : g.files)
            if (!files_[f].removed) ++n;
        if (n < 2) continue;
        ++groups;
        dupFiles += n;
        wasted += g.size * (n - 1);
    }
    HWND lv = Item(CID_LIST);
    int count = ListView_GetItemCount(lv);
    for (int i = 0; i < count; ++i) {
        if (!ListView_GetCheckState(lv, i)) continue;
        size_t f = FileOfItem(i);
        if (f < files_.size()) {
            ++marked;
            markedBytes += files_[f].size;
        }
    }
    std::wstring s = IntToStrGrouped(groups) + (groups == 1 ? L" Gruppe" : L" Gruppen") + L" mit " +
                     IntToStrGrouped(dupFiles) + L" Dateien, verschwendeter Speicher: " + FormatSize(wasted);
    if (marked)
        s += L"   ·   markiert: " + IntToStrGrouped(marked) + (marked == 1 ? L" Datei (" : L" Dateien (") +
             FormatSize(markedBytes) + L")";
    SetText(CID_SUMMARY, s);
}

size_t DuplicatesDialog::FileOfItem(int item) const {
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = item;
    if (!ListView_GetItem(Item(CID_LIST), &it)) return (size_t)-1;
    return (size_t)it.lParam;
}

int DuplicatesDialog::FocusedItem() const {
    HWND lv = Item(CID_LIST);
    int f = ListView_GetNextItem(lv, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (f < 0) f = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    return f;
}

// mode: CMD_KEEP_OLDEST / CMD_KEEP_NEWEST / CMD_KEEP_FIRST – je Gruppe bleibt eine Datei unmarkiert
void DuplicatesDialog::AutoMark(int mode) {
    std::unordered_set<size_t> keep;
    for (auto& g : groups_) {
        if (g.removed) continue;
        size_t best = (size_t)-1;
        for (size_t f : g.files) {
            if (files_[f].removed) continue;
            if (best == (size_t)-1) {
                best = f;
                if (mode == CMD_KEEP_FIRST) break;
                continue;
            }
            int c = CompareFileTime(&files_[f].modified, &files_[best].modified);
            if ((mode == CMD_KEEP_OLDEST && c < 0) || (mode == CMD_KEEP_NEWEST && c > 0)) best = f;
        }
        if (best != (size_t)-1) keep.insert(best);
    }
    HWND lv = Item(CID_LIST);
    bulk_ = true;
    int count = ListView_GetItemCount(lv);
    for (int i = 0; i < count; ++i) ListView_SetCheckState(lv, i, keep.count(FileOfItem(i)) == 0);
    bulk_ = false;
    UpdateSummary();
}

void DuplicatesDialog::SetAllChecks(bool on, bool selectedOnly) {
    HWND lv = Item(CID_LIST);
    bulk_ = true;
    int count = ListView_GetItemCount(lv);
    for (int i = 0; i < count; ++i)
        if (!selectedOnly || (ListView_GetItemState(lv, i, LVIS_SELECTED) & LVIS_SELECTED))
            ListView_SetCheckState(lv, i, on);
    bulk_ = false;
    UpdateSummary();
}

// Entfernt Listeneinträge, deren Datei nicht mehr existiert, und Gruppen mit weniger als zwei Dateien.
void DuplicatesDialog::RemoveMissing() {
    HWND lv = Item(CID_LIST);
    bulk_ = true;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    for (auto& f : files_)
        if (!f.removed && !PathExists(f.Path())) f.removed = true;
    for (size_t g = 0; g < groups_.size(); ++g) {
        DupGroup& grp = groups_[g];
        if (grp.removed) continue;
        size_t n = 0;
        for (size_t f : grp.files)
            if (!files_[f].removed) ++n;
        if (n < 2) {
            for (size_t f : grp.files) files_[f].removed = true;
            grp.removed = true;
        }
    }
    for (int i = ListView_GetItemCount(lv) - 1; i >= 0; --i) {
        size_t f = FileOfItem(i);
        if (f < files_.size() && files_[f].removed) ListView_DeleteItem(lv, i);
    }
    for (size_t g = 0; g < groups_.size(); ++g) {
        if (groups_[g].removed) {
            ListView_RemoveGroup(lv, (int)g + 1);
        } else {
            std::wstring header = GroupHeader((int)g);
            LVGROUP lg{};
            lg.cbSize = sizeof(lg);
            lg.mask = LVGF_HEADER;
            lg.pszHeader = header.data();
            ListView_SetGroupInfo(lv, (int)g + 1, &lg);
        }
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
    bulk_ = false;
    UpdateSummary();
}

void DuplicatesDialog::DeleteMarked() {
    HWND lv = Item(CID_LIST);
    std::vector<std::wstring> paths;
    std::unordered_map<int, size_t> checkedPerGroup;
    int count = ListView_GetItemCount(lv);
    for (int i = 0; i < count; ++i) {
        if (!ListView_GetCheckState(lv, i)) continue;
        size_t f = FileOfItem(i);
        if (f >= files_.size()) continue;
        paths.push_back(files_[f].Path());
        checkedPerGroup[files_[f].group]++;
    }
    if (paths.empty()) {
        MsgInfo(hwnd_, L"Es sind keine Dateien markiert.");
        return;
    }
    // Warnung, wenn in einer Gruppe alle Dateien markiert sind (es bliebe kein Exemplar übrig)
    size_t allMarked = 0;
    for (auto& kv : checkedPerGroup) {
        size_t n = 0;
        for (size_t f : groups_[(size_t)kv.first].files)
            if (!files_[f].removed) ++n;
        if (kv.second >= n) ++allMarked;
    }
    if (allMarked &&
        !MsgConfirm(hwnd_, L"In " + IntToStrGrouped(allMarked) +
                               (allMarked == 1 ? L" Gruppe sind" : L" Gruppen sind") +
                               L" alle Dateien markiert – davon bliebe kein Exemplar übrig.\n\nTrotzdem löschen?"))
        return;
    DeleteItems(hwnd_, paths, App::Opt().useRecycleBin, true);
    RemoveMissing();
    if (!App::Opt().autoRefresh) App::RefreshPanes();
}

void DuplicatesDialog::ShowContextMenu(POINT pt) {
    if (ListView_GetSelectedCount(Item(CID_LIST)) == 0) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_GOTO, L"&Gehe zu\tEnter");
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Ö&ffnen");
    AppendMenuW(m, MF_STRING, CMD_VIEW, L"&Anzeigen\tF3");
    AppendMenuW(m, MF_STRING, CMD_COPYPATHS, L"&Pfade kopieren\tStrg+Umschalt+C");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_CHECK, L"Auswahl &markieren");
    AppendMenuW(m, MF_STRING, CMD_UNCHECK, L"Markierung der Auswahl auf&heben");
    SetMenuDefaultItem(m, CMD_GOTO, FALSE);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd) DoCommand(cmd);
}

void DuplicatesDialog::DoCommand(int cmd) {
    HWND lv = Item(CID_LIST);
    std::vector<std::wstring> sel;
    int i = -1;
    while ((i = ListView_GetNextItem(lv, i, LVNI_SELECTED)) >= 0) {
        size_t f = FileOfItem(i);
        if (f < files_.size()) sel.push_back(files_[f].Path());
    }
    switch (cmd) {
    case CMD_GOTO: {
        int f = FocusedItem();
        size_t fi = f >= 0 ? FileOfItem(f) : (size_t)-1;
        if (fi < files_.size()) App::NavigateToFile(files_[fi].Path());
        break;
    }
    case CMD_OPEN:
        if (sel.size() > 10 && !MsgConfirm(hwnd_, IntToStrGrouped(sel.size()) + L" Dateien öffnen?")) return;
        for (auto& p : sel) ShellOpen(hwnd_, p);
        break;
    case CMD_VIEW: {
        int f = FocusedItem();
        size_t fi = f >= 0 ? FileOfItem(f) : (size_t)-1;
        if (fi < files_.size()) OpenViewerWindow(files_[fi].Path());
        break;
    }
    case CMD_COPYPATHS:
        if (!sel.empty()) ClipboardSetText(hwnd_, Join(sel, L"\r\n"));
        break;
    case CMD_CHECK: SetAllChecks(true, true); break;
    case CMD_UNCHECK: SetAllChecks(false, true); break;
    case CMD_KEEP_OLDEST:
    case CMD_KEEP_NEWEST:
    case CMD_KEEP_FIRST: AutoMark(cmd); break;
    }
}

BOOL DuplicatesDialog::OnCommand(int id, int code, HWND ctl) {
    switch (id) {
    case IDOK:
        if (GetFocus() == Item(CID_LIST)) {
            DoCommand(CMD_GOTO);
            return TRUE;
        }
        Start();
        return TRUE;
    case CID_STOP:
        // Während der Suche ist "Stopp" Standardschaltfläche: Enter in der Liste bleibt "Gehe zu"
        if (GetFocus() == Item(CID_LIST)) {
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
        std::wstring d = BrowseForFolder(hwnd_, L"Verzeichnis für die Suche nach doppelten Dateien", init);
        if (!d.empty()) {
            // mit Umschalt hinzufügen, sonst ersetzen
            std::wstring cur = Trim(GetText(CID_DIRS));
            if (GetKeyState(VK_SHIFT) < 0 && !cur.empty())
                SetText(CID_DIRS, cur + L";" + d);
            else
                SetText(CID_DIRS, d);
        }
        return TRUE;
    }
    case CID_AUTOMARK: {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, CMD_KEEP_OLDEST, L"In jeder Gruppe alle außer der Ä&ltesten markieren");
        AppendMenuW(m, MF_STRING, CMD_KEEP_NEWEST, L"In jeder Gruppe alle außer der &Neuesten markieren");
        AppendMenuW(m, MF_STRING, CMD_KEEP_FIRST,
                    L"In jeder Gruppe alle außer der &Ersten markieren (Reihenfolge der Verzeichnisse)");
        RECT rc{};
        GetWindowRect(Item(CID_AUTOMARK), &rc);
        int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, rc.left, rc.top, 0, hwnd_,
                                      nullptr);
        DestroyMenu(m);
        if (cmd) AutoMark(cmd);
        return TRUE;
    }
    case CID_UNMARK:
        SetAllChecks(false, false);
        return TRUE;
    case CID_DELETE:
        DeleteMarked();
        return TRUE;
    case CID_GOTO:
        DoCommand(CMD_GOTO);
        return TRUE;
    case IDCANCEL:
        End(IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR DuplicatesDialog::OnNotify(NMHDR* nm) {
    if (nm->idFrom != CID_LIST) return 0;
    switch (nm->code) {
    case LVN_GETDISPINFOW: {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        if (!(di->item.mask & LVIF_TEXT)) return 0;
        size_t f = FileOfItem(di->item.iItem);
        if (f >= files_.size()) return 0;
        const DupFile& df = files_[f];
        switch (di->item.iSubItem) {
        case 0: dispBuf_ = df.name; break;
        case 1: dispBuf_ = df.dir; break;
        case 2: dispBuf_ = App::Opt().sizeInBytes ? FormatSizeBytes(df.size) : FormatSize(df.size); break;
        case 3: dispBuf_ = FormatFileTime(df.modified); break;
        default: dispBuf_.clear();
        }
        di->item.pszText = const_cast<wchar_t*>(dispBuf_.c_str());
        return 0;
    }
    case LVN_ITEMCHANGED: {
        auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
        if (!bulk_ && (lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK))
            UpdateSummary();
        return 0;
    }
    case NM_DBLCLK: {
        auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
        if (ia->iItem >= 0) {
            size_t f = FileOfItem(ia->iItem);
            if (f < files_.size()) App::NavigateToFile(files_[f].Path());
        }
        return 0;
    }
    case LVN_KEYDOWN: {
        auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (kd->wVKey == VK_F3)
            DoCommand(CMD_VIEW);
        else if (kd->wVKey == 'C' && ctrl && shift)
            DoCommand(CMD_COPYPATHS);
        else if (kd->wVKey == 'A' && ctrl)
            ListView_SetItemState(Item(CID_LIST), -1, LVIS_SELECTED, LVIS_SELECTED);
        return 0;
    }
    }
    return 0;
}

INT_PTR DuplicatesDialog::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kMsgProgress:
        Drain();
        return TRUE;
    case kMsgDone: {
        Drain();
        if (worker_.joinable()) worker_.join();
        bool aborted = stop_.load();
        SetRunning(false);
        size_t groups = 0;
        for (auto& g : groups_)
            if (!g.removed) ++groups;
        std::wstring s = (aborted ? L"Abgebrochen: " : L"Fertig: ") + IntToStrGrouped(filesFound_.load()) +
                         L" Dateien geprüft, " + IntToStrGrouped(groups) +
                         (groups == 1 ? L" Gruppe doppelter Dateien gefunden." : L" Gruppen doppelter Dateien gefunden.");
        if (readErrors_.load()) s += L" " + IntToStrGrouped(readErrors_.load()) + L" Dateien konnten nicht gelesen werden.";
        SetText(CID_STATUS, s);
        return TRUE;
    }
    case WM_CONTEXTMENU: {
        HWND lv = Item(CID_LIST);
        if ((HWND)wp != lv) return FALSE;
        POINT pt{(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
        if (lp == -1) {
            int f = FocusedItem();
            RECT rc{};
            if (f >= 0 && ListView_GetItemRect(lv, f, &rc, LVIR_LABEL))
                pt = {rc.left + DpiScale(hwnd_, 8), rc.bottom};
            else
                pt = {DpiScale(hwnd_, 8), DpiScale(hwnd_, 8)};
            ClientToScreen(lv, &pt);
        }
        ShowContextMenu(pt);
        return TRUE;
    }
    }
    return FALSE;
}

void DuplicatesDialog::OnDestroy() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    SaveSettings();
}

} // namespace

void FindDuplicates(HWND owner, const std::wstring& startDir) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    DialogTemplate t(L"Doppelte Dateien suchen", 470, 316, DialogTemplate::kResizable);
    t.Label(CID_LBL_DIRS, L"&Verzeichnisse:", 7, 9, 55, 8);
    t.Edit(CID_DIRS, 65, 7, 309, 12);
    t.Button(CID_BROWSE, L"…", 376, 6, 14, 14);
    t.Label(CID_LBL_PATTERN, L"&Muster:", 7, 25, 55, 8);
    t.Edit(CID_PATTERN, 65, 23, 100, 12);
    t.Label(CID_LBL_MINSIZE, L"Mindest&größe:", 172, 25, 50, 8);
    t.Edit(CID_MINSIZE, 224, 23, 60, 12, ES_AUTOHSCROLL | ES_NUMBER);
    t.Label(CID_LBL_BYTES, L"Bytes", 288, 25, 30, 8);
    t.Check(CID_SUBDIRS, L"&Unterverzeichnisse", 65, 40, 85, 10);
    t.Check(CID_HIDDEN, L"Versteckte/&Systemdateien", 155, 40, 110, 10);
    t.DefButton(IDOK, L"S&tarten", 400, 6, 63, 14);
    t.Button(CID_STOP, L"Sto&pp", 400, 23, 63, 14);
    t.Button(IDCANCEL, L"Schließen", 400, 40, 63, 14);
    t.ListView(CID_LIST, 7, 57, 456, 205, LVS_REPORT | LVS_SHOWSELALWAYS);
    t.Label(CID_SUMMARY, L"", 7, 266, 456, 8, SS_ENDELLIPSIS);
    t.Button(CID_AUTOMARK, L"&Automatisch markieren …", 7, 279, 100, 14);
    t.Button(CID_UNMARK, L"Markierungen auf&heben", 110, 279, 90, 14);
    t.Button(CID_DELETE, L"Markierte &löschen …", 203, 279, 80, 14);
    t.Button(CID_GOTO, L"&Gehe zu", 286, 279, 50, 14);
    t.Label(CID_STATUS, L"", 7, 300, 456, 8, SS_ENDELLIPSIS);

    auto* dlg = new DuplicatesDialog(startDir);
    if (!dlg->CreateModeless(owner, t, true)) delete dlg;
}

} // namespace qf
