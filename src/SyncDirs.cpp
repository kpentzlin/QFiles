// Modul C: Verzeichnisse synchronisieren (modaler, in der Größe veränderbarer Dialog mit Vorschau).
//
// Ablauf: "Vergleichen" ermittelt im Arbeitsthread die nötigen Aktionen und zeigt sie in einer
// ListView mit Kontrollkästchen. "Synchronisieren" führt die angehakten Aktionen über FileOps aus
// (Protokoll + Rückgängig) und vergleicht anschließend neu.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "FileOps.h"
#include "Util.h"
#include "CompareCommon.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qf {

namespace {

using cmpdetail::CompareFileContents;
using cmpdetail::CompareFileTimes;
using cmpdetail::ContentResult;

const wchar_t* const kSection = L"Synchronisieren";

enum : int {
    IDC_LBL_A = 1101,
    IDC_DIR_A,
    IDC_BROWSE_A,
    IDC_LBL_B,
    IDC_DIR_B,
    IDC_BROWSE_B,
    IDC_SWAP,
    IDC_GRP_DIR,
    IDC_DIR_ATOB,
    IDC_DIR_BTOA,
    IDC_DIR_BOTH,
    IDC_WARN,
    IDC_GRP_OPT,
    IDC_RECURSIVE,
    IDC_COPY_MISSING,
    IDC_REPLACE_OLDER,
    IDC_ONLY_EXISTING,
    IDC_MIRROR,
    IDC_CONTENT,
    IDC_TOLERANCE,
    IDC_HIDDEN,
    IDC_LBL_FILTER,
    IDC_FILTER,
    IDC_COMPARE,
    IDC_STATUS,
    IDC_LIST,
    IDC_ALL,
    IDC_NONE,
    IDC_SUMMARY,
    IDC_SYNC,
};

constexpr UINT WM_SYNC_PROGRESS = WM_APP + 51;
constexpr UINT WM_SYNC_DONE = WM_APP + 52;

enum class Direction { AtoB = 0, BtoA = 1, Both = 2 };
enum class Action : uint8_t { CopyToB, CopyToA, ReplaceB, ReplaceA, DeleteB, DeleteA };

struct SyncOptions {
    Direction direction = Direction::AtoB;
    bool recursive = true;
    bool copyMissing = true;
    bool replaceOlder = true;
    bool onlyExisting = false;
    bool mirror = false;
    bool content = false;
    bool tolerance = true;
    bool includeHidden = false;
    std::wstring filter = L"*";
};

struct SideInfo {
    bool exists = false;
    uint64_t size = 0;      // bei Verzeichnissen: Summe aller Dateien
    FILETIME time{};
};

struct SyncItem {
    Action action = Action::CopyToB;
    std::wstring rel;       // relativer Pfad (ohne abschließenden '\')
    bool isDir = false;
    SideInfo a, b;
    bool checked = true;
    // Bytes, die bei dieser Aktion kopiert werden (0 bei Löschen)
    uint64_t CopyBytes() const {
        switch (action) {
        case Action::CopyToB:
        case Action::ReplaceB: return a.size;
        case Action::CopyToA:
        case Action::ReplaceA: return b.size;
        default: return 0;
        }
    }
    bool IsCopy() const { return action != Action::DeleteA && action != Action::DeleteB; }
};

struct ScanResult {
    std::wstring rootA, rootB;
    std::vector<SyncItem> items;
    int skipped = 0;        // Ziel neuer / gleiches Datum mit anderem Inhalt / Datei-Verzeichnis-Konflikt
    int errors = 0;         // nicht lesbare Verzeichnisse/Dateien
    bool cancelled = false;
};

const wchar_t* ActionText(Action a) {
    switch (a) {
    case Action::CopyToB: return L"→ kopieren";
    case Action::CopyToA: return L"← kopieren";
    case Action::ReplaceB: return L"→ ersetzen";
    case Action::ReplaceA: return L"← ersetzen";
    case Action::DeleteB: return L"löschen in B";
    case Action::DeleteA: return L"löschen in A";
    }
    return L"";
}

// ---------------- Ermittlung der Aktionen (Arbeitsthread) ----------------

class Scanner {
public:
    Scanner(const SyncOptions& o, const std::atomic<bool>& cancel, HWND notify, std::mutex& mx, std::wstring& current)
        : o_(o), cancel_(cancel), notify_(notify), mx_(mx), current_(current) {
        filterActive_ = !cmpdetail::IsTrivialPattern(o_.filter);
        // ganze Verzeichnisse nur dann als Ganzes kopieren/löschen, wenn nichts ausgefiltert wird
        wholeDirs_ = !filterActive_ && o_.includeHidden;
    }

    void Run(ScanResult& r) {
        res_ = &r;
        ScanDir(L"", true, true);
        if (cancel_.load()) r.cancelled = true;
    }

private:
    std::wstring FullA(const std::wstring& rel) const { return rel.empty() ? res_->rootA : PathCombine(res_->rootA, rel); }
    std::wstring FullB(const std::wstring& rel) const { return rel.empty() ? res_->rootB : PathCombine(res_->rootB, rel); }

    void ReportProgress(const std::wstring& rel) {
        ULONGLONG now = GetTickCount64();
        if (now - lastReport_ < 100) return;
        lastReport_ = now;
        {
            std::lock_guard<std::mutex> lock(mx_);
            current_ = rel;
        }
        PostMessageW(notify_, WM_SYNC_PROGRESS, 0, 0);
    }

    bool Hidden(const DirEntry& e) const {
        return !o_.includeHidden && (e.attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
    }

    // Eintrag berücksichtigen? (versteckt, Filter, Unterverzeichnisse, Verknüpfungen)
    bool Accept(const DirEntry& e) const {
        if (Hidden(e)) return false;
        if (e.IsDir()) {
            if (!o_.recursive) return false;
            if (e.attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false; // Junctions/Symlinks nicht verfolgen
            return true;
        }
        if (filterActive_ && !MatchAnyPattern(o_.filter, e.name)) return false;
        return true;
    }

    uint64_t TreeSize(const std::wstring& dir) {
        uint64_t total = 0;
        WalkDirectory(dir, [&](const std::wstring&, const DirEntry& e) {
            if (!e.IsDir()) total += e.size;
            return !cancel_.load();
        });
        return total;
    }

    static SideInfo Info(const DirEntry& e) {
        SideInfo s;
        s.exists = true;
        s.size = e.size;
        s.time = e.modified;
        return s;
    }

    void Add(Action act, const std::wstring& rel, bool isDir, const DirEntry* ea, const DirEntry* eb) {
        SyncItem it;
        it.action = act;
        it.rel = rel;
        it.isDir = isDir;
        if (ea) it.a = Info(*ea);
        if (eb) it.b = Info(*eb);
        if (isDir) {
            if (ea) it.a.size = TreeSize(FullA(rel));
            if (eb) it.b.size = TreeSize(FullB(rel));
        }
        res_->items.push_back(std::move(it));
    }

    // Eintrag existiert nur auf einer Seite (fromA = nur in A)
    void HandleOneSided(const std::wstring& rel, const DirEntry& e, bool fromA) {
        bool towardsOther = o_.direction == Direction::Both ||
                            (fromA ? o_.direction == Direction::AtoB : o_.direction == Direction::BtoA);
        bool deleteHere = o_.mirror && o_.direction != Direction::Both &&
                          (fromA ? o_.direction == Direction::BtoA : o_.direction == Direction::AtoB);
        bool copy = towardsOther && o_.copyMissing && !o_.onlyExisting;
        if (!copy && !deleteHere) return;
        Action act = copy ? (fromA ? Action::CopyToB : Action::CopyToA) : (fromA ? Action::DeleteA : Action::DeleteB);
        if (e.IsDir()) {
            if (wholeDirs_) {
                Add(act, rel, true, fromA ? &e : nullptr, fromA ? nullptr : &e);
            } else {
                // Filter/versteckte Dateien: einzeln behandeln
                ScanDir(rel, fromA, !fromA);
            }
        } else {
            Add(act, rel, false, fromA ? &e : nullptr, fromA ? nullptr : &e);
        }
    }

    void HandleBoth(const std::wstring& rel, const DirEntry& ea, const DirEntry& eb) {
        if (ea.IsDir() != eb.IsDir()) {
            ++res_->skipped;
            return;
        }
        if (ea.IsDir()) {
            ScanDir(rel, true, true);
            return;
        }
        if (!o_.replaceOlder) return;
        int t = CompareFileTimes(ea.modified, eb.modified, o_.tolerance, false);
        bool differs;
        if (o_.content) {
            if (ea.size != eb.size) {
                differs = true;
            } else {
                ReportProgress(rel);
                ContentResult c = CompareFileContents(FullA(rel), FullB(rel), cancel_);
                if (c == ContentResult::Cancelled) return;
                if (c == ContentResult::Error) {
                    ++res_->errors;
                    return;
                }
                differs = c == ContentResult::Different;
            }
        } else {
            differs = ea.size != eb.size || t != 0;
        }
        if (!differs) return;
        switch (o_.direction) {
        case Direction::AtoB:
            if (t >= 0) Add(Action::ReplaceB, rel, false, &ea, &eb);   // Quelle neuer (oder gleich alt, aber anders)
            else ++res_->skipped;
            break;
        case Direction::BtoA:
            if (t <= 0) Add(Action::ReplaceA, rel, false, &ea, &eb);
            else ++res_->skipped;
            break;
        case Direction::Both:
            if (t > 0) Add(Action::ReplaceB, rel, false, &ea, &eb);
            else if (t < 0) Add(Action::ReplaceA, rel, false, &ea, &eb);
            else ++res_->skipped;  // gleiches Datum, anderer Inhalt: Richtung unklar
            break;
        }
    }

    void ScanDir(const std::wstring& rel, bool hasA, bool hasB) {
        if (cancel_.load()) return;
        ReportProgress(rel);
        std::vector<DirEntry> A, B;
        if (hasA && !ListDirectory(FullA(rel), A)) {
            ++res_->errors;
            return;
        }
        if (hasB && !ListDirectory(FullB(rel), B)) {
            ++res_->errors;
            return;
        }
        // nach Namen (ohne Groß-/Kleinschreibung) zusammenführen, sortiert
        std::map<std::wstring, std::pair<int, int>> merged;
        for (int i = 0; i < (int)A.size(); ++i)
            if (Accept(A[i])) merged[ToLower(A[i].name)].first = i + 1;
        for (int i = 0; i < (int)B.size(); ++i)
            if (Accept(B[i])) merged[ToLower(B[i].name)].second = i + 1;
        for (auto& [key, idx] : merged) {
            if (cancel_.load()) return;
            const DirEntry* ea = idx.first ? &A[idx.first - 1] : nullptr;
            const DirEntry* eb = idx.second ? &B[idx.second - 1] : nullptr;
            std::wstring name = ea ? ea->name : eb->name;
            std::wstring r = rel.empty() ? name : rel + L"\\" + name;
            if (ea && eb)
                HandleBoth(r, *ea, *eb);
            else if (ea)
                HandleOneSided(r, *ea, true);
            else
                HandleOneSided(r, *eb, false);
        }
    }

    const SyncOptions& o_;
    const std::atomic<bool>& cancel_;
    HWND notify_;
    std::mutex& mx_;
    std::wstring& current_;
    ScanResult* res_ = nullptr;
    bool filterActive_ = false;
    bool wholeDirs_ = false;
    ULONGLONG lastReport_ = 0;
};

// Liegt inner innerhalb von outer (oder ist gleich)?
bool IsSameOrInside(const std::wstring& inner, const std::wstring& outer) {
    std::wstring i = NormalizeDir(inner), o = NormalizeDir(outer);
    if (EqualsI(i, o)) return true;
    if (!o.empty() && o.back() != L'\\') o += L'\\';
    return StartsWithI(i, o);
}

// ---------------- Dialog ----------------

class SyncDialog : public DialogBase {
public:
    SyncDialog(const std::wstring& a, const std::wstring& b) : initA_(a), initB_(b) {}
    ~SyncDialog() override { StopWorker(); }
    bool Changed() const { return changed_; }

protected:
    BOOL OnInit() override {
        initializing_ = true;
        LoadOptions();
        SetText(IDC_DIR_A, initA_);
        SetText(IDC_DIR_B, initB_);
        ApplyOptionsToControls();

        HWND lv = Item(IDC_LIST);
        LvAddColumn(IDC_LIST, L"Aktion", 62);
        LvAddColumn(IDC_LIST, L"Relativer Pfad", 170);
        LvAddColumn(IDC_LIST, L"Größe A", 48, LVCFMT_RIGHT);
        LvAddColumn(IDC_LIST, L"Größe B", 48, LVCFMT_RIGHT);
        LvAddColumn(IDC_LIST, L"Datum A", 68);
        LvAddColumn(IDC_LIST, L"Datum B", 68);
        // LvAddColumn setzt die erweiterten Stile – Kontrollkästchen danach ergänzen
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
                                                  LVS_EX_CHECKBOXES);

        SetAnchor(IDC_DIR_A, AnchorTopLeftRight);
        SetAnchor(IDC_DIR_B, AnchorTopLeftRight);
        SetAnchor(IDC_BROWSE_A, AnchorTopRight);
        SetAnchor(IDC_BROWSE_B, AnchorTopRight);
        SetAnchor(IDC_SWAP, AnchorTopRight);
        SetAnchor(IDC_GRP_OPT, AnchorTopLeftRight);
        SetAnchor(IDC_FILTER, AnchorTopLeftRight);
        SetAnchor(IDC_STATUS, AnchorTopLeftRight);
        SetAnchor(IDC_LIST, AnchorAll);
        SetAnchor(IDC_ALL, AnchorBottomLeft);
        SetAnchor(IDC_NONE, AnchorBottomLeft);
        SetAnchor(IDC_SUMMARY, AnchorBottomLeftRight);
        SetAnchor(IDC_SYNC, AnchorBottomRight);
        SetAnchor(IDCANCEL, AnchorBottomRight);
        EnableResizing();
        cmpdetail::RestoreDialogSize(hwnd_, kSection);

        SetText(IDC_STATUS, L"„Vergleichen“ ermittelt die nötigen Aktionen.");
        UpdateEnabling();
        UpdateSummary();
        initializing_ = false;
        SendMessageW(hwnd_, DM_SETDEFID, IDC_COMPARE, 0);
        SetFocus(Item(IDC_COMPARE));
        return FALSE;
    }

    BOOL OnCommand(int id, int code, HWND ctl) override {
        switch (id) {
        case IDC_BROWSE_A:
        case IDC_BROWSE_B: {
            int edit = id == IDC_BROWSE_A ? IDC_DIR_A : IDC_DIR_B;
            std::wstring d = BrowseForFolder(hwnd_, id == IDC_BROWSE_A ? L"Verzeichnis A wählen" : L"Verzeichnis B wählen",
                                             GetText(edit));
            if (!d.empty()) SetText(edit, d);
            return TRUE;
        }
        case IDC_SWAP: {
            std::wstring a = GetText(IDC_DIR_A), b = GetText(IDC_DIR_B);
            SetText(IDC_DIR_A, b);
            SetText(IDC_DIR_B, a);
            return TRUE;
        }
        case IDC_DIR_A:
        case IDC_DIR_B:
        case IDC_FILTER:
            if (code == EN_CHANGE) Invalidate();
            return TRUE;
        case IDC_DIR_ATOB:
        case IDC_DIR_BTOA:
        case IDC_DIR_BOTH:
        case IDC_RECURSIVE:
        case IDC_COPY_MISSING:
        case IDC_REPLACE_OLDER:
        case IDC_ONLY_EXISTING:
        case IDC_MIRROR:
        case IDC_CONTENT:
        case IDC_TOLERANCE:
        case IDC_HIDDEN:
            if (code == BN_CLICKED) {
                UpdateEnabling();
                Invalidate();
            }
            return TRUE;
        case IDC_COMPARE:
            if (running_)
                CancelScan();
            else
                StartScan();
            return TRUE;
        case IDC_ALL:
        case IDC_NONE:
            SetAllChecks(id == IDC_ALL);
            return TRUE;
        case IDC_SYNC:
            if (!running_) Synchronize();
            return TRUE;
        case IDOK:
            // Eingabetaste: Standardschaltfläche
            if (!running_) StartScan();
            return TRUE;
        case IDCANCEL:
            StopWorker();
            ReadOptions();
            SaveOptions();
            End(changed_ ? IDOK : IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR OnNotify(NMHDR* nm) override {
        if (nm->idFrom != IDC_LIST) return 0;
        if (nm->code == LVN_ITEMCHANGED) {
            auto* p = reinterpret_cast<NMLISTVIEW*>(nm);
            if (filling_ || p->iItem < 0) return 0;
            if ((p->uChanged & LVIF_STATE) && ((p->uNewState ^ p->uOldState) & LVIS_STATEIMAGEMASK)) {
                size_t idx = (size_t)p->lParam;
                if (idx < scan_.items.size()) {
                    scan_.items[idx].checked = ListView_GetCheckState(Item(IDC_LIST), p->iItem) != FALSE;
                    UpdateSummary();
                }
            }
            return 0;
        }
        if (nm->code == NM_DBLCLK) {
            // Doppelklick auf eine Datei, die auf beiden Seiten existiert: Dateivergleich
            auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
            if (ia->iItem < 0) return 0;
            LVITEMW it{};
            it.mask = LVIF_PARAM;
            it.iItem = ia->iItem;
            if (!ListView_GetItem(Item(IDC_LIST), &it)) return 0;
            size_t idx = (size_t)it.lParam;
            if (idx >= scan_.items.size()) return 0;
            const SyncItem& s = scan_.items[idx];
            if (!s.isDir && s.a.exists && s.b.exists)
                CompareFiles(hwnd_, PathCombine(scan_.rootA, s.rel), PathCombine(scan_.rootB, s.rel));
            return 0;
        }
        return 0;
    }

    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        switch (msg) {
        case WM_SYNC_PROGRESS: {
            if (!running_) return TRUE;
            std::wstring cur;
            {
                std::lock_guard<std::mutex> lock(mx_);
                cur = current_;
            }
            SetText(IDC_STATUS, L"Vergleiche: " + (cur.empty() ? std::wstring(L"\\") : cur));
            return TRUE;
        }
        case WM_SYNC_DONE:
            if ((unsigned)wp == generation_) OnScanDone();
            return TRUE;
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == Item(IDC_WARN)) {
                HDC dc = (HDC)wp;
                SetTextColor(dc, RGB(192, 0, 0));
                SetBkMode(dc, TRANSPARENT);
                return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
            }
            return FALSE;
        }
        return FALSE;
    }

    void OnDestroy() override {
        StopWorker();
        cmpdetail::SaveWindowSize(hwnd_, kSection);
    }

private:
    // ---- Optionen ----
    void LoadOptions() {
        Config& c = App::Cfg();
        o_.direction = (Direction)std::clamp(c.GetInt(kSection, L"Direction", 0), 0, 2);
        o_.recursive = c.GetBool(kSection, L"Recursive", true);
        o_.copyMissing = c.GetBool(kSection, L"CopyMissing", true);
        o_.replaceOlder = c.GetBool(kSection, L"ReplaceOlder", true);
        o_.onlyExisting = c.GetBool(kSection, L"OnlyExisting", false);
        o_.mirror = false; // Spiegeln (Löschen) bewusst nicht vorbelegen
        o_.content = c.GetBool(kSection, L"Content", false);
        o_.tolerance = c.GetBool(kSection, L"Tolerance", true);
        o_.includeHidden = c.GetBool(kSection, L"Hidden", false);
        o_.filter = c.Get(kSection, L"Filter", L"*");
    }

    void SaveOptions() {
        Config& c = App::Cfg();
        c.SetInt(kSection, L"Direction", (int)o_.direction);
        c.SetBool(kSection, L"Recursive", o_.recursive);
        c.SetBool(kSection, L"CopyMissing", o_.copyMissing);
        c.SetBool(kSection, L"ReplaceOlder", o_.replaceOlder);
        c.SetBool(kSection, L"OnlyExisting", o_.onlyExisting);
        c.SetBool(kSection, L"Content", o_.content);
        c.SetBool(kSection, L"Tolerance", o_.tolerance);
        c.SetBool(kSection, L"Hidden", o_.includeHidden);
        c.Set(kSection, L"Filter", o_.filter);
    }

    void ApplyOptionsToControls() {
        CheckRadioButton(hwnd_, IDC_DIR_ATOB, IDC_DIR_BOTH,
                         o_.direction == Direction::BtoA   ? IDC_DIR_BTOA
                         : o_.direction == Direction::Both ? IDC_DIR_BOTH
                                                           : IDC_DIR_ATOB);
        SetCheck(IDC_RECURSIVE, o_.recursive);
        SetCheck(IDC_COPY_MISSING, o_.copyMissing);
        SetCheck(IDC_REPLACE_OLDER, o_.replaceOlder);
        SetCheck(IDC_ONLY_EXISTING, o_.onlyExisting);
        SetCheck(IDC_MIRROR, o_.mirror);
        SetCheck(IDC_CONTENT, o_.content);
        SetCheck(IDC_TOLERANCE, o_.tolerance);
        SetCheck(IDC_HIDDEN, o_.includeHidden);
        SetText(IDC_FILTER, o_.filter);
    }

    void ReadOptions() {
        o_.direction = IsChecked(IDC_DIR_BTOA)   ? Direction::BtoA
                       : IsChecked(IDC_DIR_BOTH) ? Direction::Both
                                                 : Direction::AtoB;
        o_.recursive = IsChecked(IDC_RECURSIVE);
        o_.copyMissing = IsChecked(IDC_COPY_MISSING);
        o_.replaceOlder = IsChecked(IDC_REPLACE_OLDER);
        o_.onlyExisting = IsChecked(IDC_ONLY_EXISTING);
        o_.mirror = IsChecked(IDC_MIRROR) && o_.direction != Direction::Both;
        o_.content = IsChecked(IDC_CONTENT);
        o_.tolerance = IsChecked(IDC_TOLERANCE);
        o_.includeHidden = IsChecked(IDC_HIDDEN);
        o_.filter = Trim(GetText(IDC_FILTER));
        if (o_.filter.empty()) o_.filter = L"*";
    }

    void UpdateEnabling() {
        bool both = IsChecked(IDC_DIR_BOTH);
        bool idle = !running_;
        static const int optionIds[] = {IDC_DIR_A,         IDC_DIR_B,    IDC_BROWSE_A, IDC_BROWSE_B, IDC_SWAP,
                                        IDC_DIR_ATOB,      IDC_DIR_BTOA, IDC_DIR_BOTH, IDC_RECURSIVE,
                                        IDC_REPLACE_OLDER, IDC_ONLY_EXISTING, IDC_CONTENT, IDC_HIDDEN, IDC_FILTER};
        for (int id : optionIds) Enable(id, idle);
        Enable(IDC_COPY_MISSING, idle && !IsChecked(IDC_ONLY_EXISTING));
        Enable(IDC_MIRROR, idle && !both);
        Enable(IDC_TOLERANCE, idle);   // auch beim Inhaltsvergleich: entscheidet über die Richtung (neuer/älter)
        bool mirrorActive = IsChecked(IDC_MIRROR) && !both;
        Show(IDC_WARN, mirrorActive);
        if (mirrorActive)
            SetText(IDC_WARN, IsChecked(IDC_DIR_BTOA) ? L"Achtung: Spiegeln löscht Dateien in A, die in B fehlen!"
                                                      : L"Achtung: Spiegeln löscht Dateien in B, die in A fehlen!");
        SetText(IDC_COMPARE, running_ ? L"Abbrechen" : L"&Vergleichen");
        bool any = false;
        for (auto& it : scan_.items)
            if (it.checked) {
                any = true;
                break;
            }
        Enable(IDC_SYNC, idle && any);
        Enable(IDC_ALL, idle && !scan_.items.empty());
        Enable(IDC_NONE, idle && !scan_.items.empty());
    }

    // Optionen oder Verzeichnisse geändert: bisherige Vorschau ist ungültig
    void Invalidate() {
        if (initializing_ || running_) return;
        if (scan_.items.empty() && !haveScan_) return;
        ClearList();
        haveScan_ = false;
        SetText(IDC_STATUS, L"Einstellungen geändert – bitte erneut vergleichen.");
        UpdateSummary();
        UpdateEnabling();
    }

    void ClearList() {
        filling_ = true;
        ListView_DeleteAllItems(Item(IDC_LIST));
        filling_ = false;
        scan_.items.clear();
    }

    // ---- Vergleich ----
    void StartScan() {
        ReadOptions();
        std::wstring a = Trim(GetText(IDC_DIR_A)), b = Trim(GetText(IDC_DIR_B));
        if (a.empty() || !DirExists(a)) {
            MsgError(hwnd_, L"Verzeichnis A existiert nicht:\n" + a);
            SetFocus(Item(IDC_DIR_A));
            return;
        }
        if (b.empty() || !DirExists(b)) {
            MsgError(hwnd_, L"Verzeichnis B existiert nicht:\n" + b);
            SetFocus(Item(IDC_DIR_B));
            return;
        }
        a = NormalizeDir(a);
        b = NormalizeDir(b);
        if (EqualsI(a, b)) {
            MsgError(hwnd_, L"Verzeichnis A und B sind identisch.");
            return;
        }
        if (o_.recursive && (IsSameOrInside(a, b) || IsSameOrInside(b, a))) {
            MsgError(hwnd_, L"Ein Verzeichnis liegt innerhalb des anderen. Mit Unterverzeichnissen ist das nicht möglich.");
            return;
        }
        StopWorker();
        ClearList();
        haveScan_ = false;
        running_ = true;
        cancel_ = false;
        UpdateEnabling();
        UpdateSummary();
        SetText(IDC_STATUS, L"Vergleiche …");
        unsigned gen = ++generation_;
        SyncOptions o = o_;
        HWND h = hwnd_;
        worker_ = std::thread([this, o, a, b, h, gen]() {
            ScanResult r;
            r.rootA = a;
            r.rootB = b;
            Scanner sc(o, cancel_, h, mx_, current_);
            sc.Run(r);
            {
                std::lock_guard<std::mutex> lock(mx_);
                pending_ = std::move(r);
            }
            PostMessageW(h, WM_SYNC_DONE, gen, 0);
        });
    }

    void CancelScan() {
        cancel_ = true;
        SetText(IDC_STATUS, L"Wird abgebrochen …");
    }

    void StopWorker() {
        if (worker_.joinable()) {
            cancel_ = true;
            worker_.join();
        }
        running_ = false;
    }

    void OnScanDone() {
        if (worker_.joinable()) worker_.join();
        running_ = false;
        ScanResult r;
        {
            std::lock_guard<std::mutex> lock(mx_);
            r = std::move(pending_);
        }
        if (r.cancelled) {
            SetText(IDC_STATUS, L"Vergleich abgebrochen.");
            UpdateEnabling();
            return;
        }
        scan_ = std::move(r);
        haveScan_ = true;
        FillList();
        std::wstring s;
        if (scan_.items.empty())
            s = L"Keine Aktionen nötig – die Verzeichnisse sind synchron.";
        else
            s = L"Vergleich abgeschlossen: " + IntToStrGrouped(scan_.items.size()) +
                (scan_.items.size() == 1 ? L" Aktion." : L" Aktionen.");
        if (scan_.skipped)
            s += L"  " + IntToStrGrouped((unsigned long long)scan_.skipped) +
                 L" übersprungen (Ziel neuer, gleiches Datum mit anderem Inhalt oder Datei/Verzeichnis-Konflikt).";
        if (scan_.errors) s += L"  " + IntToStrGrouped((unsigned long long)scan_.errors) + L" nicht lesbar.";
        SetText(IDC_STATUS, s);
        UpdateEnabling();
        UpdateSummary();
        // Standardschaltfläche bleibt "Vergleichen" – Synchronisieren nur bewusst per Klick/Alt+S
        SendMessageW(hwnd_, DM_SETDEFID, IDC_COMPARE, 0);
        if (!scan_.items.empty()) SendMessageW(hwnd_, WM_NEXTDLGCTL, (WPARAM)Item(IDC_LIST), TRUE);
    }

    static std::wstring SizeText(const SideInfo& s) { return s.exists ? FormatSize(s.size) : std::wstring(); }
    static std::wstring TimeText(const SideInfo& s, bool isDir) {
        return (s.exists && !isDir) ? FormatFileTime(s.time, true) : std::wstring();
    }

    void FillList() {
        HWND lv = Item(IDC_LIST);
        filling_ = true;
        SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(lv);
        ListView_SetItemCount(lv, (int)scan_.items.size());
        for (size_t i = 0; i < scan_.items.size(); ++i) {
            const SyncItem& s = scan_.items[i];
            std::vector<std::wstring> cells = {ActionText(s.action), s.isDir ? s.rel + L"\\" : s.rel, SizeText(s.a),
                                               SizeText(s.b), TimeText(s.a, s.isDir), TimeText(s.b, s.isDir)};
            int row = LvAddRow(IDC_LIST, cells, (LPARAM)i);
            ListView_SetCheckState(lv, row, TRUE);
        }
        for (auto& s : scan_.items) s.checked = true;
        SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(lv, nullptr, TRUE);
        filling_ = false;
    }

    void SetAllChecks(bool on) {
        HWND lv = Item(IDC_LIST);
        filling_ = true;
        int n = ListView_GetItemCount(lv);
        for (int i = 0; i < n; ++i) ListView_SetCheckState(lv, i, on);
        for (auto& s : scan_.items) s.checked = on;
        filling_ = false;
        UpdateSummary();
        UpdateEnabling();
    }

    void UpdateSummary() {
        size_t checked = 0, copies = 0, deletes = 0;
        unsigned long long bytes = 0;
        for (auto& s : scan_.items) {
            if (!s.checked) continue;
            ++checked;
            if (s.IsCopy()) {
                ++copies;
                bytes += s.CopyBytes();
            } else {
                ++deletes;
            }
        }
        std::wstring t;
        if (!scan_.items.empty()) {
            t = IntToStrGrouped(checked) + L" von " + IntToStrGrouped(scan_.items.size()) + L" ausgewählt · Kopieren: " +
                IntToStrGrouped(copies) + L" (" + FormatSize(bytes) + L")";
            if (deletes) t += L" · Löschen: " + IntToStrGrouped(deletes);
        }
        SetText(IDC_SUMMARY, t);
        bool idle = !running_;
        Enable(IDC_SYNC, idle && checked > 0);
    }

    // ---- Ausführen ----
    void Synchronize() {
        std::vector<CopyJob> jobs;
        std::vector<std::wstring> deletes;
        std::vector<std::wstring> dirsToCreate;
        bool deleteInA = false, deleteInB = false;
        for (auto& s : scan_.items) {
            if (!s.checked) continue;
            std::wstring pa = PathCombine(scan_.rootA, s.rel), pb = PathCombine(scan_.rootB, s.rel);
            switch (s.action) {
            case Action::CopyToB:
            case Action::ReplaceB:
                jobs.push_back({pa, PathParent(pb), L""});
                break;
            case Action::CopyToA:
            case Action::ReplaceA:
                jobs.push_back({pb, PathParent(pa), L""});
                break;
            case Action::DeleteB:
                deletes.push_back(pb);
                deleteInB = true;
                break;
            case Action::DeleteA:
                deletes.push_back(pa);
                deleteInA = true;
                break;
            }
        }
        if (jobs.empty() && deletes.empty()) return;

        bool doDelete = !deletes.empty();
        if (doDelete) {
            bool recycle = App::Opt().useRecycleBin;
            std::wstring where = deleteInA && deleteInB ? L"in A und B" : deleteInA ? L"in Verzeichnis A" : L"in Verzeichnis B";
            std::wstring q = IntToStrGrouped(deletes.size()) + (deletes.size() == 1 ? L" Element " : L" Elemente ") + where +
                             (recycle ? L" werden in den Papierkorb verschoben."
                                      : L" werden ENDGÜLTIG gelöscht (kein Papierkorb).");
            if (!jobs.empty()) {
                q += L"\n\nJa = synchronisieren und löschen\nNein = nur kopieren, nichts löschen\nAbbrechen = nichts tun";
                int r = MsgYesNoCancel(hwnd_, q);
                if (r == IDCANCEL) return;
                doDelete = r == IDYES;
            } else {
                if (!MsgConfirm(hwnd_, q + L"\n\nFortfahren?")) return;
            }
        }

        // Zielverzeichnisse vorher anlegen
        for (auto& j : jobs) {
            if (DirExists(j.destDir)) continue;
            int rc = SHCreateDirectoryExW(hwnd_, j.destDir.c_str(), nullptr);
            if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
                MsgError(hwnd_, L"Das Verzeichnis kann nicht angelegt werden:\n" + j.destDir + L"\n\n" + LastErrorMessage(rc));
                return;
            }
        }

        BeginUndoGroup(L"Synchronisieren");
        bool ok = true;
        if (!jobs.empty()) {
            ok = CopyJobs(hwnd_, jobs, OpNoConfirmOverwrite) && ok;
            changed_ = true;
        }
        if (doDelete && !deletes.empty()) {
            ok = DeleteItems(hwnd_, deletes, App::Opt().useRecycleBin, false) && ok;
            changed_ = true;
        }
        EndUndoGroup();
        LogOperation(L"Synchronisiert: " + scan_.rootA + L" ⇄ " + scan_.rootB);
        if (!ok) SetText(IDC_STATUS, L"Nicht alle Aktionen konnten ausgeführt werden.");
        // Vorschau neu berechnen
        StartScan();
    }

    std::wstring initA_, initB_;
    SyncOptions o_;
    ScanResult scan_;
    bool haveScan_ = false;
    bool filling_ = false;
    bool initializing_ = false;
    bool changed_ = false;

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    bool running_ = false;
    unsigned generation_ = 0;
    std::mutex mx_;
    std::wstring current_;   // geschützt durch mx_
    ScanResult pending_;     // geschützt durch mx_
};

} // namespace

bool SyncDirectories(HWND owner, const std::wstring& left, const std::wstring& right) {
    DialogTemplate t(L"Verzeichnisse synchronisieren", 460, 330, DialogTemplate::kResizable);
    t.Label(IDC_LBL_A, L"Verzeichnis &A (links):", 7, 9, 82, 9);
    t.Edit(IDC_DIR_A, 90, 7, 321, 12);
    t.Button(IDC_BROWSE_A, L"…", 414, 7, 18, 12);
    t.Label(IDC_LBL_B, L"Verzeichnis &B (rechts):", 7, 25, 82, 9);
    t.Edit(IDC_DIR_B, 90, 23, 321, 12);
    t.Button(IDC_BROWSE_B, L"…", 414, 23, 18, 12);
    t.Button(IDC_SWAP, L"⇄", 436, 7, 17, 28);

    t.Group(IDC_GRP_DIR, L"Richtung", 7, 40, 100, 52);
    t.Radio(IDC_DIR_ATOB, L"A → B", 15, 52, 88, 10, true);
    t.Radio(IDC_DIR_BTOA, L"B → A", 15, 64, 88, 10);
    t.Radio(IDC_DIR_BOTH, L"Beide Richtungen", 15, 76, 88, 10);
    t.Label(IDC_WARN, L"", 7, 96, 100, 24);

    t.Group(IDC_GRP_OPT, L"Optionen", 112, 40, 341, 80);
    t.Check(IDC_RECURSIVE, L"Unterverzeichnisse einbeziehen", 120, 52, 165, 10, WS_GROUP);
    t.Check(IDC_COPY_MISSING, L"Fehlende Dateien kopieren", 120, 64, 165, 10);
    t.Check(IDC_REPLACE_OLDER, L"Ältere Dateien durch neuere ersetzen", 120, 76, 165, 10);
    t.Check(IDC_ONLY_EXISTING, L"Nur vorhandene Dateien aktualisieren", 120, 88, 165, 10);
    t.Check(IDC_MIRROR, L"Im Ziel löschen, was in der Quelle fehlt (Spiegeln)", 120, 100, 165, 16, BS_MULTILINE);
    t.Check(IDC_CONTENT, L"Inhalt vergleichen statt Datum/Größe", 290, 52, 158, 10);
    t.Check(IDC_TOLERANCE, L"Zeittoleranz 2 Sekunden", 290, 64, 158, 10);
    t.Check(IDC_HIDDEN, L"Versteckte/Systemdateien einbeziehen", 290, 76, 158, 10);
    t.Label(IDC_LBL_FILTER, L"Dateifilter:", 290, 92, 40, 9);
    t.Edit(IDC_FILTER, 332, 90, 115, 12);

    t.DefButton(IDC_COMPARE, L"&Vergleichen", 7, 125, 60, 14);
    t.Label(IDC_STATUS, L"", 72, 127, 381, 9, SS_ENDELLIPSIS);
    t.ListView(IDC_LIST, 7, 143, 446, 162, LVS_REPORT | LVS_SHOWSELALWAYS);

    t.Button(IDC_ALL, L"&Alle", 7, 310, 40, 14);
    t.Button(IDC_NONE, L"&Keine", 50, 310, 40, 14);
    t.Label(IDC_SUMMARY, L"", 95, 313, 240, 9, SS_ENDELLIPSIS);
    t.Button(IDC_SYNC, L"&Synchronisieren", 340, 310, 62, 14);
    t.Button(IDCANCEL, L"Schließen", 405, 310, 48, 14);

    SyncDialog dlg(left, right);
    dlg.DoModal(owner, t);
    return dlg.Changed();
}

} // namespace qf
