// Modul E – Werkzeuge: Laufwerksübersicht, Dateifilter, Info-Dialog.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Settings.h"
#include "Util.h"
#include "ToolsCommon.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifndef QFILES_VERSION_STRING
#define QFILES_VERSION_STRING "0.0.0"
#endif

namespace qf {

namespace {

// ===================================================================================
// Laufwerksübersicht
// ===================================================================================

struct DriveInfo {
    std::wstring root;       // "C:\"
    UINT type = DRIVE_UNKNOWN;
    bool queried = false;    // Abfrage abgeschlossen
    bool ready = false;      // Volume bereit
    std::wstring label;
    std::wstring fileSystem;
    unsigned long long total = 0, freeBytes = 0;
    int icon = -1;
};

// Gemeinsamer Zustand zwischen Dialog und Abfrage-Threads. Threads können länger leben als der Dialog
// (hängende Netzlaufwerke), daher shared_ptr und Generationszähler.
struct DriveState {
    std::mutex m;
    HWND hwnd = nullptr;
    unsigned generation = 0;
    std::vector<DriveInfo> drives;
};

constexpr UINT kMsgDriveDone = WM_APP + 21;

const wchar_t* DriveTypeName(UINT type) {
    switch (type) {
    case DRIVE_FIXED: return L"Festplatte";
    case DRIVE_REMOVABLE: return L"Wechseldatenträger";
    case DRIVE_REMOTE: return L"Netzlaufwerk";
    case DRIVE_CDROM: return L"CD/DVD";
    case DRIVE_RAMDISK: return L"RAM-Disk";
    case DRIVE_NO_ROOT_DIR: return L"Kein Stammverzeichnis";
    default: return L"Unbekannt";
    }
}

// Abfrage eines Laufwerks (läuft in eigenem Thread)
void QueryDrive(std::shared_ptr<DriveState> state, unsigned generation, size_t index, std::wstring root) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Keine Systemmeldungen „Datenträger einlegen“
    DWORD oldMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);

    DriveInfo info;
    wchar_t label[MAX_PATH + 1] = L"", fs[MAX_PATH + 1] = L"";
    DWORD serial = 0, maxLen = 0, flags = 0;
    if (GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, &serial, &maxLen, &flags, fs, MAX_PATH + 1)) {
        info.ready = true;
        info.label = label;
        info.fileSystem = fs;
        ULARGE_INTEGER avail{}, total{}, freeAll{};
        if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &freeAll)) {
            info.total = total.QuadPart;
            info.freeBytes = freeAll.QuadPart;
        }
    }
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(root.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) info.icon = sfi.iIcon;

    SetThreadErrorMode(oldMode, nullptr);
    if (SUCCEEDED(hr)) CoUninitialize();

    std::lock_guard<std::mutex> lock(state->m);
    if (state->generation != generation || !state->hwnd || index >= state->drives.size()) return;
    DriveInfo& d = state->drives[index];
    d.queried = true;
    d.ready = info.ready;
    d.label = info.label;
    d.fileSystem = info.fileSystem;
    d.total = info.total;
    d.freeBytes = info.freeBytes;
    if (info.icon >= 0) d.icon = info.icon;
    PostMessageW(state->hwnd, kMsgDriveDone, generation, (LPARAM)index);
}

class DriveOverviewDlg : public DialogBase {
public:
    static constexpr int kList = 100;
    static constexpr int kRefresh = 101;
    static constexpr int kStatus = 102;
    static constexpr int kOpen = 103;
    static constexpr int kColBar = 7;          // Spalte mit Belegungsbalken
    static constexpr LPARAM kSumRow = -1;

protected:
    BOOL OnInit() override {
        state_ = std::make_shared<DriveState>();
        state_->hwnd = hwnd_;
        HWND lv = Item(kList);
        LvAddColumn(kList, L"Laufwerk", 48);
        LvAddColumn(kList, L"Typ", 70);
        LvAddColumn(kList, L"Bezeichnung", 80);
        LvAddColumn(kList, L"Dateisystem", 46);
        LvAddColumn(kList, L"Gesamt", 50, LVCFMT_RIGHT);
        LvAddColumn(kList, L"Frei", 50, LVCFMT_RIGHT);
        LvAddColumn(kList, L"Belegt", 50, LVCFMT_RIGHT);
        LvAddColumn(kList, L"Belegung", 70);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
                                                  LVS_EX_GRIDLINES);
        // System-Imagelist (geteilt, daher LVS_SHAREIMAGELISTS)
        SHFILEINFOW sfi{};
        HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                                                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        if (il) ListView_SetImageList(lv, il, LVSIL_SMALL);
        folderIcon_ = sfi.iIcon;

        SetAnchor(kList, AnchorAll);
        SetAnchor(kStatus, AnchorBottomLeftRight);
        SetAnchor(kRefresh, AnchorBottomLeft);
        SetAnchor(kOpen, AnchorBottomRight);
        SetAnchor(IDCANCEL, AnchorBottomRight);
        EnableResizing();
        Refresh();
        SetFocus(lv);
        return FALSE;
    }

    BOOL OnCommand(int id, int code, HWND ctl) override {
        switch (id) {
        case kRefresh:
            Refresh();
            return TRUE;
        case IDOK:
        case kOpen:
            OpenSelected();
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }

    INT_PTR OnNotify(NMHDR* nm) override {
        if (nm->idFrom != kList) return 0;
        if (nm->code == NM_DBLCLK) {
            OpenSelected();
            return 1;
        }
        if (nm->code == NM_CUSTOMDRAW) return CustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
        return 0;
    }

    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == kMsgDriveDone) {
            std::lock_guard<std::mutex> lock(state_->m);
            if ((unsigned)wp == state_->generation && (size_t)lp < state_->drives.size()) {
                UpdateRow((size_t)lp, state_->drives[(size_t)lp]);
                UpdateSum();
            }
            return TRUE;
        }
        return FALSE;
    }

    void OnDestroy() override {
        std::lock_guard<std::mutex> lock(state_->m);
        state_->hwnd = nullptr;
        state_->generation++;
    }

private:
    void Refresh() {
        HWND lv = Item(kList);
        ListView_DeleteAllItems(lv);
        std::vector<DriveInfo> drives;
        DWORD len = GetLogicalDriveStringsW(0, nullptr);
        std::vector<wchar_t> buf(len + 2, 0);
        GetLogicalDriveStringsW(len + 1, buf.data());
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
            DriveInfo d;
            d.root = p;
            d.type = GetDriveTypeW(p);
            // Diskettenlaufwerke (A:/B: als Wechseldatenträger) werden nicht abgefragt und nicht angezeigt.
            wchar_t letter = (wchar_t)towupper(d.root[0]);
            if (d.type == DRIVE_REMOVABLE && (letter == L'A' || letter == L'B')) continue;
            drives.push_back(d);
        }
        unsigned gen;
        {
            std::lock_guard<std::mutex> lock(state_->m);
            gen = ++state_->generation;
            state_->drives = drives;
        }
        for (size_t i = 0; i < drives.size(); ++i) {
            const DriveInfo& d = drives[i];
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            it.iItem = (int)i;
            std::wstring name = d.root;
            if (name.size() > 2 && name.back() == L'\\') name.pop_back();
            it.pszText = name.data();
            it.lParam = (LPARAM)i;
            it.iImage = folderIcon_;
            int row = ListView_InsertItem(lv, &it);
            ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(DriveTypeName(d.type)));
            ListView_SetItemText(lv, row, 2, const_cast<wchar_t*>(L"…"));
        }
        // Summenzeile
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem = (int)drives.size();
        it.pszText = const_cast<wchar_t*>(L"Summe");
        it.lParam = kSumRow;
        it.iImage = -1;
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, (int)drives.size(), 1, const_cast<wchar_t*>(L"Lokale Festplatten"));
        SetText(kStatus, Format(L"%d Laufwerke – Abfrage läuft …", (int)drives.size()));
        pending_ = (int)drives.size();

        for (size_t i = 0; i < drives.size(); ++i) {
            std::thread(QueryDrive, state_, gen, i, drives[i].root).detach();
        }
        if (drives.empty()) {
            std::lock_guard<std::mutex> lock(state_->m);
            UpdateSum();
        }
    }

    int RowOf(size_t index) {
        LVFINDINFOW fi{};
        fi.flags = LVFI_PARAM;
        fi.lParam = (LPARAM)index;
        return ListView_FindItem(Item(kList), -1, &fi);
    }

    void UpdateRow(size_t index, const DriveInfo& d) {
        HWND lv = Item(kList);
        int row = RowOf(index);
        if (row < 0) return;
        if (d.icon >= 0) {
            LVITEMW it{};
            it.mask = LVIF_IMAGE;
            it.iItem = row;
            it.iImage = d.icon;
            ListView_SetItem(lv, &it);
        }
        auto set = [&](int col, const std::wstring& s) {
            ListView_SetItemText(lv, row, col, const_cast<wchar_t*>(s.c_str()));
        };
        if (!d.ready) {
            set(2, L"(nicht bereit)");
            for (int c = 3; c <= kColBar; ++c) set(c, L"");
        } else {
            set(2, d.label);
            set(3, d.fileSystem);
            if (d.total > 0) {
                set(4, FormatSize(d.total));
                set(5, FormatSize(d.freeBytes));
                set(6, FormatSize(d.total - std::min(d.total, d.freeBytes)));
                set(kColBar, Format(L"%d %%", Percent(d.total, d.freeBytes)));
            } else {
                for (int c = 4; c <= kColBar; ++c) set(c, L"");
            }
        }
        if (pending_ > 0) --pending_;
    }

    static int Percent(unsigned long long total, unsigned long long freeBytes) {
        if (total == 0) return 0;
        unsigned long long used = total - std::min(total, freeBytes);
        return (int)((used * 100.0) / (double)total + 0.5);
    }

    // Summe aller lokalen Festplatten (Aufrufer hält state_->m)
    void UpdateSum() {
        HWND lv = Item(kList);
        LVFINDINFOW fi{};
        fi.flags = LVFI_PARAM;
        fi.lParam = kSumRow;
        int row = ListView_FindItem(lv, -1, &fi);
        unsigned long long total = 0, freeBytes = 0;
        int count = 0;
        for (const auto& d : state_->drives) {
            if (d.type == DRIVE_FIXED && d.queried && d.ready && d.total > 0) {
                total += d.total;
                freeBytes += d.freeBytes;
                ++count;
            }
        }
        if (row >= 0) {
            auto set = [&](int col, const std::wstring& s) {
                ListView_SetItemText(lv, row, col, const_cast<wchar_t*>(s.c_str()));
            };
            set(1, Format(L"Lokale Festplatten (%d)", count));
            if (total > 0) {
                set(4, FormatSize(total));
                set(5, FormatSize(freeBytes));
                set(6, FormatSize(total - freeBytes));
                set(kColBar, Format(L"%d %%", Percent(total, freeBytes)));
            }
        }
        int n = (int)state_->drives.size();
        if (pending_ > 0)
            SetText(kStatus, Format(L"%d Laufwerke – Abfrage läuft (%d ausstehend) …", n, pending_));
        else
            SetText(kStatus, Format(L"%d Laufwerke. Doppelklick wechselt zum Laufwerk.", n));
    }

    void OpenSelected() {
        HWND lv = Item(kList);
        int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
        if (row < 0) return;
        LVITEMW it{};
        it.mask = LVIF_PARAM;
        it.iItem = row;
        ListView_GetItem(lv, &it);
        if (it.lParam == kSumRow) return;
        std::wstring root;
        {
            std::lock_guard<std::mutex> lock(state_->m);
            if ((size_t)it.lParam >= state_->drives.size()) return;
            const DriveInfo& d = state_->drives[(size_t)it.lParam];
            if (d.queried && !d.ready) {
                MessageBeep(MB_ICONWARNING);
                return;
            }
            root = d.root;
        }
        App::NavigateTo(root);
        End(IDOK);
    }

    INT_PTR CustomDraw(NMLVCUSTOMDRAW* cd) {
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            // Summenzeile fett
            if (cd->nmcd.lItemlParam == kSumRow) {
                if (!boldFont_) {
                    LOGFONTW lf{};
                    HFONT f = (HFONT)SendMessageW(cd->nmcd.hdr.hwndFrom, WM_GETFONT, 0, 0);
                    GetObjectW(f ? (HGDIOBJ)f : GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
                    lf.lfWeight = FW_BOLD;
                    boldFont_ = CreateFontIndirectW(&lf);
                }
                SelectObject(cd->nmcd.hdc, boldFont_);
                return CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT;
            }
            return CDRF_NOTIFYSUBITEMDRAW;
        }
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            if (cd->iSubItem != kColBar) return CDRF_DODEFAULT;
            HWND lv = cd->nmcd.hdr.hwndFrom;
            int row = (int)cd->nmcd.dwItemSpec;
            wchar_t text[32] = L"";
            ListView_GetItemText(lv, row, kColBar, text, 32);
            if (!text[0]) return CDRF_DODEFAULT;
            int pct = _wtoi(text);
            RECT rc;
            ListView_GetSubItemRect(lv, row, kColBar, LVIR_BOUNDS, &rc);
            HDC dc = cd->nmcd.hdc;
            bool selected = (ListView_GetItemState(lv, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            HBRUSH bg = GetSysColorBrush(selected ? (GetFocus() == lv ? COLOR_HIGHLIGHT : COLOR_BTNFACE) : COLOR_WINDOW);
            FillRect(dc, &rc, bg);
            int m = DpiScale(lv, 3);
            RECT bar{rc.left + m, rc.top + m, rc.right - m, rc.bottom - m};
            if (bar.right > bar.left && bar.bottom > bar.top) {
                HBRUSH frameBrush = CreateSolidBrush(RGB(160, 160, 160));
                FrameRect(dc, &bar, frameBrush);
                DeleteObject(frameBrush);
                RECT inner{bar.left + 1, bar.top + 1, bar.right - 1, bar.bottom - 1};
                HBRUSH back = CreateSolidBrush(RGB(240, 240, 240));
                FillRect(dc, &inner, back);
                DeleteObject(back);
                RECT fill = inner;
                fill.right = inner.left + MulDiv(inner.right - inner.left, std::clamp(pct, 0, 100), 100);
                COLORREF col = pct >= 90 ? RGB(218, 38, 38) : (pct >= 75 ? RGB(240, 160, 30) : RGB(38, 140, 218));
                HBRUSH fb = CreateSolidBrush(col);
                FillRect(dc, &fill, fb);
                DeleteObject(fb);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(0, 0, 0));
                DrawTextW(dc, text, -1, &inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            return CDRF_SKIPDEFAULT;
        }
        }
        return CDRF_DODEFAULT;
    }

public:
    ~DriveOverviewDlg() override {
        if (boldFont_) DeleteObject(boldFont_);
    }

private:
    std::shared_ptr<DriveState> state_;
    int folderIcon_ = 0;
    int pending_ = 0;
    HFONT boldFont_ = nullptr;
};

// ===================================================================================
// Dateifilter
// ===================================================================================

class FilterDlg : public DialogBase {
public:
    enum : int {
        kMask = 101, kExclude, kDirAll, kDirNone, kDirMask,
        kModAny, kModRange, kModDays, kModFrom, kModTo, kModDaysVal,
        kCrAny, kCrRange, kCrDays, kCrFrom, kCrTo, kCrDaysVal,
        kSizeMin, kSizeMax, kAttrR, kAttrH, kAttrS, kAttrA, kGit, kGitInfo, kSort, kReset
    };
    PaneFilter filter;
    std::wstring dir;
    bool gitFound = false;
    std::wstring gitText;

protected:
    BOOL OnInit() override {
        // Maskenverlauf
        auto hist = App::Cfg().GetList(L"Filter", L"Masken");
        const wchar_t* presets[] = {L"*.docx / *.xlsx / *.pptx / *.pdf", L"*.jpg / *.jpeg / *.png / *.gif / *.heic",
                                    L"*.txt / *.md / *.log", L"*.cpp / *.h"};
        for (auto& h : hist) ComboAdd(kMask, h);
        for (auto p : presets) {
            bool dup = false;
            for (auto& h : hist) dup = dup || EqualsI(h, p);
            if (!dup) ComboAdd(kMask, p);
        }
        const wchar_t* days[] = {L"1", L"2", L"3", L"7", L"14", L"30", L"90", L"365"};
        for (auto d : days) {
            ComboAdd(kModDaysVal, d);
            ComboAdd(kCrDaysVal, d);
        }
        const wchar_t* sorts[] = {L"keine Vorgabe", L"Name aufsteigend", L"Name absteigend", L"Typ aufsteigend",
                                  L"Typ absteigend", L"Größe aufsteigend", L"Größe absteigend",
                                  L"Erstellt aufsteigend", L"Erstellt absteigend", L"Geändert aufsteigend",
                                  L"Geändert absteigend"};
        for (auto s : sorts) ComboAdd(kSort, s);
        Load(filter);
        SetText(kGitInfo, gitText);
        Enable(kGit, gitFound);
        SetFocus(Item(kMask));
        SendMessageW(Item(kMask), CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
        return FALSE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == IDOK) {
            PaneFilter f;
            if (!Store(f)) return TRUE;
            f.active = true;
            filter = f;
            // Maskenverlauf (neueste zuerst, höchstens 20)
            std::wstring m = Trim(f.masks);
            if (!m.empty() && m != L"*") {
                auto hist = App::Cfg().GetList(L"Filter", L"Masken");
                hist.erase(std::remove_if(hist.begin(), hist.end(), [&](const std::wstring& h) { return EqualsI(h, m); }),
                           hist.end());
                hist.insert(hist.begin(), m);
                if (hist.size() > 20) hist.resize(20);
                App::Cfg().SetList(L"Filter", L"Masken", hist);
            }
            End(IDOK);
            return TRUE;
        }
        if (id == kReset) {
            PaneFilter f;
            Load(f);
            return TRUE;
        }
        if (code == BN_CLICKED && (id == kModAny || id == kModRange || id == kModDays || id == kCrAny ||
                                   id == kCrRange || id == kCrDays)) {
            UpdateEnables();
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }

private:
    static void SetDate(HWND dt, int ymd) {
        if (!ymd) {
            SendMessageW(dt, DTM_SETSYSTEMTIME, GDT_NONE, 0);
            return;
        }
        SYSTEMTIME st{};
        st.wYear = (WORD)(ymd / 10000);
        st.wMonth = (WORD)(ymd / 100 % 100);
        st.wDay = (WORD)(ymd % 100);
        SendMessageW(dt, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
    }
    static int GetDate(HWND dt) {
        SYSTEMTIME st{};
        if (SendMessageW(dt, DTM_GETSYSTEMTIME, 0, (LPARAM)&st) != GDT_VALID) return 0;
        return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
    }
    void Load(const PaneFilter& f) {
        SetText(kMask, f.masks.empty() ? L"*" : f.masks);
        SetText(kExclude, f.exclude);
        CheckRadioButton(hwnd_, kDirAll, kDirMask, kDirAll + std::clamp(f.dirMode, 0, 2));
        CheckRadioButton(hwnd_, kModAny, kModDays, kModAny + std::clamp(f.modMode, 0, 2));
        CheckRadioButton(hwnd_, kCrAny, kCrDays, kCrAny + std::clamp(f.crMode, 0, 2));
        int today = TodayNumber();
        SetDate(Item(kModFrom), f.modMode == 1 ? f.modFrom : AddDays(today, -30));
        SetDate(Item(kModTo), f.modMode == 1 ? f.modTo : 0);
        SetDate(Item(kCrFrom), f.crMode == 1 ? f.crFrom : AddDays(today, -30));
        SetDate(Item(kCrTo), f.crMode == 1 ? f.crTo : 0);
        SetText(kModDaysVal, std::to_wstring(f.modDays > 0 ? f.modDays : 7));
        SetText(kCrDaysVal, std::to_wstring(f.crDays > 0 ? f.crDays : 7));
        SetText(kSizeMin, f.sizeMin >= 0 ? SizeShortText(f.sizeMin) : L"");
        SetText(kSizeMax, f.sizeMax >= 0 ? SizeShortText(f.sizeMax) : L"");
        const int ids[4] = {kAttrR, kAttrH, kAttrS, kAttrA};
        for (int i = 0; i < 4; ++i)
            CheckDlgButton(hwnd_, ids[i], f.attr[i] < 0 ? BST_INDETERMINATE : f.attr[i] ? BST_CHECKED : BST_UNCHECKED);
        SetCheck(kGit, f.git && gitFound);
        ComboSetSel(kSort, std::clamp(f.sort, 0, 10));
        UpdateEnables();
    }
    void UpdateEnables() {
        bool mr = IsChecked(kModRange), md = IsChecked(kModDays), cr = IsChecked(kCrRange), cd = IsChecked(kCrDays);
        Enable(kModFrom, mr);
        Enable(kModTo, mr);
        Enable(kModDaysVal, md);
        Enable(kCrFrom, cr);
        Enable(kCrTo, cr);
        Enable(kCrDaysVal, cd);
    }
    bool Store(PaneFilter& f) {
        f.masks = Trim(GetText(kMask));
        if (f.masks.empty()) f.masks = L"*";
        f.exclude = Trim(GetText(kExclude));
        f.dirMode = IsChecked(kDirNone) ? 1 : IsChecked(kDirMask) ? 2 : 0;
        f.modMode = IsChecked(kModRange) ? 1 : IsChecked(kModDays) ? 2 : 0;
        f.crMode = IsChecked(kCrRange) ? 1 : IsChecked(kCrDays) ? 2 : 0;
        f.modFrom = GetDate(Item(kModFrom));
        f.modTo = GetDate(Item(kModTo));
        f.crFrom = GetDate(Item(kCrFrom));
        f.crTo = GetDate(Item(kCrTo));
        f.modDays = std::max(0, _wtoi(GetText(kModDaysVal).c_str()));
        f.crDays = std::max(0, _wtoi(GetText(kCrDaysVal).c_str()));
        if (f.modMode == 1 && !f.modFrom && !f.modTo) f.modMode = 0;
        if (f.crMode == 1 && !f.crFrom && !f.crTo) f.crMode = 0;
        if (!ParseSizeText(GetText(kSizeMin), f.sizeMin) || !ParseSizeText(GetText(kSizeMax), f.sizeMax)) {
            MsgError(hwnd_, L"Die Größe ist ungültig. Beispiele: 500, 10 KB, 1,5 MB, 2 GB.");
            SetFocus(Item(kSizeMin));
            return false;
        }
        const int ids[4] = {kAttrR, kAttrH, kAttrS, kAttrA};
        for (int i = 0; i < 4; ++i) {
            UINT st = IsDlgButtonChecked(hwnd_, ids[i]);
            f.attr[i] = st == BST_INDETERMINATE ? -1 : st == BST_CHECKED ? 1 : 0;
        }
        f.git = gitFound && IsChecked(kGit);
        f.sort = std::max(0, (int)SendMessageW(Item(kSort), CB_GETCURSEL, 0, 0));
        return true;
    }
};

// ===================================================================================
// Über QFiles
// ===================================================================================

class AboutDlg : public DialogBase {
public:
    static constexpr int kIcon = 101;
    static constexpr int kTitle = 102;
    static constexpr int kVersion = 103;
    static constexpr int kDesc = 104;
    static constexpr int kCfgLabel = 105;
    static constexpr int kCfgPath = 106;

protected:
    BOOL OnInit() override {
        SendMessageW(Item(kIcon), STM_SETICON, (WPARAM)App::BigIcon(), 0);
        LOGFONTW lf{};
        HFONT f = (HFONT)SendMessageW(hwnd_, WM_GETFONT, 0, 0);
        GetObjectW(f ? (HGDIOBJ)f : GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
        lf.lfHeight = lf.lfHeight * 2;
        lf.lfWeight = FW_BOLD;
        titleFont_ = CreateFontIndirectW(&lf);
        SendMessageW(Item(kTitle), WM_SETFONT, (WPARAM)titleFont_, TRUE);
        SetText(kTitle, L"QFiles");
        std::wstring build;
#if defined(QFILES_BUILD_ID) && defined(QFILES_BUILD_DATE)
        build = Utf8ToWide(QFILES_BUILD_DATE);
        if (*QFILES_BUILD_ID) build += L", Stand " + Utf8ToWide(QFILES_BUILD_ID);
        build = L"\nErstellt " + build;
#endif
        SetText(kVersion, L"Version " + Utf8ToWide(QFILES_VERSION_STRING) + L" (64-Bit)" + build);
        SetText(kDesc, L"Dateimanager für Windows\nVoll Unicode-fähig, 64 Bit, Win32/C++20");
        SetText(kCfgPath, App::Cfg().FilePath());
        SetFocus(Item(IDOK));
        return FALSE;
    }
    void OnDestroy() override {
        if (titleFont_) DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }

private:
    HFONT titleFont_ = nullptr;
};

} // namespace

// ===================================================================================
// Öffentliche Funktionen
// ===================================================================================

void DriveOverview(HWND owner) {
    DialogTemplate t(L"Laufwerksübersicht", 500, 220, DialogTemplate::kResizable);
    t.ListView(DriveOverviewDlg::kList, 7, 7, 486, 182, LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_SHAREIMAGELISTS);
    t.Label(DriveOverviewDlg::kStatus, L"", 7, 196, 230, 10, SS_ENDELLIPSIS);
    t.Button(DriveOverviewDlg::kRefresh, L"&Aktualisieren", 240, 194, 64, 14);
    t.DefButton(DriveOverviewDlg::kOpen, L"&Wechseln", 366, 194, 60, 14);
    t.Button(IDCANCEL, L"Schließen", 433, 194, 60, 14);
    DriveOverviewDlg dlg;
    dlg.DoModal(owner, t);
}

bool EditFilter(HWND owner, PaneFilter& filter, const std::wstring& dir) {
    using D = FilterDlg;
    const int W = 330, H = 318;
    DialogTemplate t(L"Filter", W, H);
    const DWORD tri = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTO3STATE;
    const DWORD dts = DTS_SHORTDATECENTURYFORMAT | DTS_SHOWNONE;
    t.Label(-1, L"&Maske (mehrere durch / getrennt, z. B. *.docx / *.xlsx):", 7, 7, W - 14, 10);
    t.Combo(D::kMask, 7, 18, W - 14, 140, true);
    t.Label(-1, L"A&usschließen:", 7, 37, 52, 10);
    t.Edit(D::kExclude, 60, 35, W - 67, 13);
    t.Group(-1, L"Verzeichnisse", 7, 52, W - 14, 24);
    t.Radio(D::kDirAll, L"&alle", 14, 62, 60, 10, true);
    t.Radio(D::kDirNone, L"&ohne", 80, 62, 60, 10);
    t.Radio(D::kDirMask, L"&laut Maske", 146, 62, 80, 10);
    // Datum der letzten Änderung und der Erstellung: beliebig / von–bis / seit N Tagen
    auto dateGroup = [&](int y, const wchar_t* title, int any, int range, int days, int from, int to, int val,
                         const wchar_t* sinceText) {
        t.Group(-1, title, 7, y, W - 14, 42);
        t.Radio(any, L"beliebig", 14, y + 12, 50, 10, true);
        t.Radio(range, L"von", 14, y + 26, 28, 10);
        t.DateTime(from, 44, y + 24, 72, 13, dts);
        t.Label(-1, L"bis", 120, y + 26, 14, 10);
        t.DateTime(to, 136, y + 24, 72, 13, dts);
        t.Radio(days, sinceText, 70, y + 12, 70, 10);
        t.Combo(val, 142, y + 10, 36, 120, true);
        t.Label(-1, L"Tagen", 182, y + 12, 30, 10);
    };
    dateGroup(80, L"Letzte Änderung", D::kModAny, D::kModRange, D::kModDays, D::kModFrom, D::kModTo, D::kModDaysVal,
              L"&geändert seit");
    dateGroup(126, L"Erstellung", D::kCrAny, D::kCrRange, D::kCrDays, D::kCrFrom, D::kCrTo, D::kCrDaysVal,
              L"&erstellt seit");
    t.Group(-1, L"Dateigröße", 7, 172, W - 14, 26);
    t.Label(-1, L"&von", 14, 184, 16, 10);
    t.Edit(D::kSizeMin, 32, 182, 60, 13);
    t.Label(-1, L"b&is", 98, 184, 14, 10);
    t.Edit(D::kSizeMax, 114, 182, 60, 13);
    t.Label(-1, L"(leer = offen; z. B. 10 KB, 1,5 MB)", 180, 184, W - 194, 10);
    t.Group(-1, L"Attribute (gefülltes Kästchen = egal)", 7, 202, W - 14, 24);
    t.Add(D::kAttrR, L"Button", L"&schreibgeschützt", 14, 212, 76, 10, tri);
    t.Add(D::kAttrH, L"Button", L"verstec&kt", 94, 212, 56, 10, tri);
    t.Add(D::kAttrS, L"Button", L"S&ystem", 154, 212, 50, 10, tri);
    t.Add(D::kAttrA, L"Button", L"A&rchiv", 208, 212, 50, 10, tri);
    t.Group(-1, L"Git", 7, 230, W - 14, 36);
    t.Check(D::kGit, L"Nur gemäß .gi&tignore zum Projekt gehörige Dateien", 14, 240, W - 28, 10);
    t.Label(D::kGitInfo, L"", 26, 252, W - 40, 10, SS_PATHELLIPSIS | SS_NOPREFIX);
    t.Label(-1, L"Sortieru&ng:", 7, 274, 44, 10);
    t.Combo(D::kSort, 52, 272, 120, 160);
    t.Button(D::kReset, L"&Zurücksetzen", 7, H - 21, 60, 14);
    t.DefButton(IDOK, L"Filter an", W - 121, H - 21, 54, 14);
    t.Button(IDCANCEL, L"Abbrechen", W - 61, H - 21, 54, 14);
    FilterDlg dlg;
    dlg.filter = filter;
    dlg.dir = dir;
    GitIgnore gi;
    dlg.gitFound = !dir.empty() && gi.Load(dir);
    if (dlg.gitFound) {
        std::wstring files;
        for (auto& f : gi.Files()) files += (files.empty() ? L"" : L"; ") + f;
        dlg.gitText = L"Gefunden: " + files;
    } else {
        dlg.gitText = L"(keine .gitignore in diesem oder einem übergeordneten Verzeichnis)";
    }
    if (dlg.DoModal(owner, t) != IDOK) return false;
    filter = dlg.filter;
    return true;
}

void ShowAbout(HWND owner) {
    DialogTemplate t(L"Über QFiles", 260, 166);
    t.Add(AboutDlg::kIcon, L"Static", L"", 10, 10, 32, 32, SS_ICON | SS_CENTERIMAGE);
    t.Label(AboutDlg::kTitle, L"", 52, 6, 200, 28);
    t.Label(AboutDlg::kVersion, L"", 52, 36, 200, 20);
    t.Label(AboutDlg::kDesc, L"", 52, 60, 200, 30);
    t.Label(AboutDlg::kCfgLabel, L"Einstellungsdatei:", 10, 98, 242, 12);
    t.Edit(AboutDlg::kCfgPath, 10, 111, 242, 14, ES_AUTOHSCROLL | ES_READONLY);
    t.DefButton(IDOK, L"OK", 202, 144, 50, 14);
    AboutDlg dlg;
    dlg.DoModal(owner, t);
}

} // namespace qf
