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
    static constexpr int kInclude = 101;
    static constexpr int kExclude = 102;
    static constexpr int kDirs = 103;
    static constexpr int kClear = 104;
    PaneFilter filter;

protected:
    BOOL OnInit() override {
        const wchar_t* presets[] = {L"*",
                                    L"*.txt;*.log",
                                    L"*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.tif;*.webp",
                                    L"*.doc;*.docx;*.xls;*.xlsx;*.pdf",
                                    L"*.exe;*.dll;*.msi",
                                    L"*.zip;*.7z;*.rar"};
        for (auto p : presets) ComboAdd(kInclude, p);
        SetText(kInclude, filter.include.empty() ? L"*" : filter.include);
        SetText(kExclude, filter.exclude);
        SetCheck(kDirs, filter.applyToDirs);
        SetFocus(Item(kInclude));
        SendMessageW(Item(kInclude), CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
        return FALSE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == IDOK) {
            filter.include = Trim(GetText(kInclude));
            if (filter.include.empty()) filter.include = L"*";
            filter.exclude = Trim(GetText(kExclude));
            filter.applyToDirs = IsChecked(kDirs);
            End(IDOK);
            return TRUE;
        }
        if (id == kClear) {
            filter.include = L"*";
            filter.exclude.clear();
            filter.applyToDirs = false;
            End(IDOK);
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
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
        SetText(kVersion, L"Version " + Utf8ToWide(QFILES_VERSION_STRING) + L" (64-Bit)");
        SetText(kDesc, L"Dateimanager für Windows – nachempfunden Idoswin Pro.\nVoll Unicode-fähig, Win32/C++20.");
        SetText(kCfgPath, App::Cfg().FilePath());
        return TRUE;
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

bool EditFilter(HWND owner, PaneFilter& filter) {
    DialogTemplate t(L"Dateifilter", 280, 118);
    t.Label(-1, L"Nur Dateien &anzeigen, die diesen Mustern entsprechen (durch ; getrennt):", 7, 7, 266, 10);
    t.Combo(FilterDlg::kInclude, 7, 19, 266, 120, true);
    t.Label(-1, L"&Ausschließen (durch ; getrennt, leer = nichts ausschließen):", 7, 39, 266, 10);
    t.Edit(FilterDlg::kExclude, 7, 51, 266, 13);
    t.Check(FilterDlg::kDirs, L"Muster auch auf &Verzeichnisse anwenden", 7, 71, 266, 10);
    t.Button(FilterDlg::kClear, L"Filter auf&heben", 7, 97, 70, 14);
    t.DefButton(IDOK, L"OK", 166, 97, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 223, 97, 50, 14);
    FilterDlg dlg;
    dlg.filter = filter;
    if (dlg.DoModal(owner, t) != IDOK) return false;
    filter = dlg.filter;
    return true;
}

void ShowAbout(HWND owner) {
    DialogTemplate t(L"Über QFiles", 250, 130);
    t.Add(AboutDlg::kIcon, L"Static", L"", 10, 10, 32, 32, SS_ICON | SS_CENTERIMAGE);
    t.Label(AboutDlg::kTitle, L"", 52, 8, 190, 20);
    t.Label(AboutDlg::kVersion, L"", 52, 30, 190, 10);
    t.Label(AboutDlg::kDesc, L"", 52, 44, 190, 20);
    t.Label(AboutDlg::kCfgLabel, L"Einstellungsdatei:", 10, 72, 232, 10);
    t.Edit(AboutDlg::kCfgPath, 10, 84, 232, 13, ES_AUTOHSCROLL | ES_READONLY);
    t.DefButton(IDOK, L"OK", 192, 109, 50, 14);
    AboutDlg dlg;
    dlg.DoModal(owner, t);
}

} // namespace qf
