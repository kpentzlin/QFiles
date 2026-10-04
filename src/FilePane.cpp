#include "FilePane.h"
#include "App.h"
#include "Commands.h"
#include "FileOps.h"
#include "Glyphs.h"
#include "ShellMenu.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <algorithm>
#include <cstdlib>
#include <unordered_map>

#undef PathCombine
#undef StrToInt

namespace qf {

namespace {

const wchar_t* kPaneClass = L"QFilesPane";

enum : UINT {
    WM_APP_DIRCHANGED = WM_APP + 1,
    WM_APP_THUMB = WM_APP + 2,
    WM_APP_DIRSIZE = WM_APP + 3,
    WM_APP_RENAMESEL = WM_APP + 4,
    WM_APP_DIRSIZE_DONE = WM_APP + 5,
};

enum : UINT_PTR { TIMER_REFRESH = 1, TIMER_STATUS = 2 };

enum ColumnId { ColName = 0, ColExt = 1, ColSize = 2, ColDate = 3, ColAttr = 4, ColCreated = 5, ColCount = 6 };
const int kDefaultWidths[ColCount] = {260, 60, 90, 120, 50, 120};
const wchar_t* kColumnTitles[ColCount] = {L"Name", L"Typ", L"Größe", L"Geändert", L"Attr.", L"Erstellt"};
// Anzeigereihenfolge: Erstellt steht direkt neben Geändert
const int kColumnOrder[ColCount] = {ColName, ColExt, ColSize, ColDate, ColCreated, ColAttr};

constexpr int kIdList = 10;
constexpr int kIdPath = 11;
constexpr int kIdViewer = 12;

constexpr int kHeaderH = 26;
constexpr int kPathH = 26;
constexpr int kStatusH = 20;
constexpr int kNavBtnW = 24;

struct DirSizeResult {
    unsigned gen;
    std::wstring name;
    uint64_t size;
};

struct ThumbResult {
    unsigned gen;
    int index;
    HBITMAP bmp;
};

bool IsSystemImageListInit = false;
HIMAGELIST g_sysSmall = nullptr;
HIMAGELIST g_sysLarge = nullptr;

void InitSystemImageLists() {
    if (IsSystemImageListInit) return;
    IsSystemImageListInit = true;
    SHFILEINFOW sfi{};
    g_sysSmall = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    g_sysLarge = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
}

// Symbolindex je Erweiterung zwischenspeichern
std::unordered_map<std::wstring, int> g_extIcons;
int g_folderIcon = -1;

bool HasOwnIcon(const std::wstring& ext) {
    static const wchar_t* exts[] = {L".exe", L".lnk", L".ico", L".url", L".cpl", L".scr", L".msc", L".appref-ms", L".cur", L".ani"};
    for (auto e : exts)
        if (EqualsI(ext, e)) return true;
    return false;
}

int StockIcon(SHSTOCKICONID id) {
    SHSTOCKICONINFO sii{sizeof(sii)};
    if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &sii))) return sii.iSysImageIndex;
    return 0;
}

std::wstring ExtOf(const std::wstring& name) {
    size_t p = name.find_last_of(L'.');
    if (p == std::wstring::npos || p == 0) return L"";
    return name.substr(p);
}

void CopyToBuf(wchar_t* buf, int cch, const std::wstring& s) {
    if (!buf || cch <= 0) return;
    size_t n = std::min<size_t>(s.size(), (size_t)cch - 1);
    memcpy(buf, s.data(), n * sizeof(wchar_t));
    buf[n] = 0;
}


} // namespace

// =====================================================================

FilePane::FilePane() {
    colWidths_.assign(kDefaultWidths, kDefaultWidths + ColCount);
}

FilePane::~FilePane() {
    StopWatch();
    StopThumbWorker();
    if (sizeCancel_) *sizeCancel_ = true;
    if (listFont_) DeleteObject(listFont_);
    if (thumbList_) ImageList_Destroy(thumbList_);
}

bool FilePane::Create(HWND parent, int index, IPaneHost* host) {
    index_ = index;
    host_ = host;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = App::Instance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kPaneClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    InitSystemImageLists();
    hwnd_ = CreateWindowExW(0, kPaneClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 100, 100, parent,
                            (HMENU)(INT_PTR)(cmd::IdPaneBase + index * 100), App::Instance(), this);
    if (!hwnd_) return false;

    pathCombo_ = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 0,
                                 100, 300, hwnd_, (HMENU)(INT_PTR)kIdPath, App::Instance(), nullptr);
    SendMessageW(pathCombo_, WM_SETFONT, (WPARAM)App::UIFont(), TRUE);
    COMBOBOXINFO cbi{sizeof(cbi)};
    GetComboBoxInfo(pathCombo_, &cbi);
    pathEdit_ = cbi.hwndItem;
    if (pathEdit_) {
        SetWindowSubclass(pathEdit_, PathEditSubclass, 1, (DWORD_PTR)this);
        SHAutoComplete(pathEdit_, SHACF_FILESYS_DIRS | SHACF_AUTOSUGGEST_FORCE_ON);
    }

    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
                                LVS_EDITLABELS | LVS_SHAREIMAGELISTS | LVS_AUTOARRANGE,
                            0, 0, 100, 100, hwnd_, (HMENU)(INT_PTR)kIdList, App::Instance(), nullptr);
    SetWindowSubclass(list_, ListSubclass, 1, (DWORD_PTR)this);
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetImageList(list_, g_sysSmall, LVSIL_SMALL);
    ListView_SetImageList(list_, g_sysLarge, LVSIL_NORMAL);
    ListView_SetCallbackMask(list_, LVIS_CUT);
    CreateListFont();
    SetupColumns();
    ApplyViewStyle();

    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                               hwnd_, nullptr, App::Instance(), nullptr);

    RegisterFileDropTarget(
        list_, [this](POINT pt) { return DropTargetDirAt(pt); },
        [this]() {
            Reload();
            if (host_) host_->OnPaneFilesDropped(this);
        });
    UpdateDrives();
    return true;
}

void FilePane::SetBounds(const RECT& rc) {
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void FilePane::Show(bool show) { ShowWindow(hwnd_, show ? SW_SHOW : SW_HIDE); }

void FilePane::CreateListFont() {
    const Options& o = App::Opt();
    int h = -MulDiv(o.listFontSize, (int)GetWindowDpi(hwnd_), 72);
    HFONT font = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, o.listFontName.c_str());
    // Erst die neue Schrift setzen, dann die alte freigeben (die Liste darf nie eine gelöschte Schrift halten)
    SendMessageW(list_, WM_SETFONT, (WPARAM)font, TRUE);
    if (listFont_) DeleteObject(listFont_);
    listFont_ = font;
}

void FilePane::SetupColumns() {
    // Vorhandene Spalten entfernen
    HWND header = ListView_GetHeader(list_);
    while (Header_GetItemCount(header) > 0) ListView_DeleteColumn(list_, 0);
    const Options& o = App::Opt();
    int pos = 0;
    colIds_.clear();
    for (int k = 0; k < ColCount; ++k) {
        int id = kColumnOrder[k];
        if (id == ColExt && !o.showExtensionsColumn) continue;
        if (id == ColAttr && !o.showAttributesColumn) continue;
        if (id == ColCreated && !o.showCreatedColumn) continue;
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        c.fmt = (id == ColSize) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = ToPx(colWidths_[id]);
        if (id == ColDate || id == ColCreated) {
            // Datumsspalten mindestens so breit, dass ein Datum (ggf. mit Sekunden) vollständig sichtbar ist
            SYSTEMTIME st{2026, 12, 0, 28, 23, 58, 58, 0};
            FILETIME ft, utc;
            SystemTimeToFileTime(&st, &ft);
            LocalFileTimeToFileTime(&ft, &utc);
            std::wstring sample = FormatFileTime(utc, o.dateWithSeconds);
            int need = ListView_GetStringWidth(list_, sample.c_str()) + ToPx(16);
            if (c.cx < need) c.cx = need;
        }
        c.pszText = const_cast<wchar_t*>(kColumnTitles[id]);
        c.iSubItem = id;
        ListView_InsertColumn(list_, pos++, &c);
        colIds_.push_back(id);  // Spaltenindex -> Spalten-ID (LVN_GETDISPINFO liefert den Index)
    }
    // Sortierpfeil
    int n = Header_GetItemCount(header);
    for (int i = 0; i < n; ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT | HDI_LPARAM;
        Header_GetItem(header, i, &hi);
        LVCOLUMNW c{};
        c.mask = LVCF_SUBITEM;
        ListView_GetColumn(list_, i, &c);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (c.iSubItem == (int)sortKey_) hi.fmt |= sortDesc_ ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(header, i, &hi);
    }
}

void FilePane::ApplyViewStyle() {
    const Options& o = App::Opt();
    DWORD ex = LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_INFOTIP | LVS_EX_HEADERDRAGDROP;
    if (o.fullRowSelect) ex |= LVS_EX_FULLROWSELECT;
    if (o.showGridLines) ex |= LVS_EX_GRIDLINES;
    ListView_SetExtendedListViewStyle(list_, ex);
    switch (view_) {
    case PaneView::Details: ListView_SetView(list_, LV_VIEW_DETAILS); break;
    case PaneView::List: ListView_SetView(list_, LV_VIEW_LIST); break;
    case PaneView::Icons:
        ListView_SetImageList(list_, g_sysLarge, LVSIL_NORMAL);
        ListView_SetView(list_, LV_VIEW_ICON);
        break;
    case PaneView::Thumbnails: {
        int size = ToPx(o.thumbnailSize);
        if (!thumbList_ || thumbSize_ != size) {
            StopThumbWorker();
            if (thumbList_) ImageList_Destroy(thumbList_);
            thumbSize_ = size;
            thumbList_ = ImageList_Create(size, size, ILC_COLOR32, 64, 64);
            // Platzhalter: hellgrauer Rahmen auf transparentem Grund (vormultipliziertes Alpha)
            BITMAPINFO bi{};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bits) {
                auto* px = (uint32_t*)bits;
                int m = size / 8;
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x) {
                        bool inside = x >= m && x < size - m && y >= m && y < size - m;
                        bool border = inside && (x == m || x == size - m - 1 || y == m || y == size - m - 1);
                        px[y * size + x] = border ? 0xFFC8C8C8u : (inside ? 0xFFF4F4F4u : 0x00000000u);
                    }
            }
            ImageList_Add(thumbList_, bmp, nullptr);
            DeleteObject(bmp);
            for (auto& it : items_) it.thumb = -1;
        }
        ListView_SetImageList(list_, thumbList_, LVSIL_NORMAL);
        ListView_SetView(list_, LV_VIEW_ICON);
        ListView_SetIconSpacing(list_, size + ToPx(24), size + ToPx(40));
        break;
    }
    }
    if (view_ == PaneView::Icons) ListView_SetIconSpacing(list_, -1, -1);
    if (view_ == PaneView::Icons || view_ == PaneView::Thumbnails) ListView_Arrange(list_, LVA_DEFAULT);
}

void FilePane::ApplyOptions() {
    CreateListFont();
    SetupColumns();
    ApplyViewStyle();
    if (!dir_.empty()) Reload();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void FilePane::OnDpiChanged() {
    CreateListFont();
    SetupColumns();
    if (view_ == PaneView::Thumbnails) {
        thumbSize_ = 0;
        ApplyViewStyle();
        ListView_RedrawItems(list_, 0, (int)items_.size());
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// ===================== Laufwerke =====================

void FilePane::UpdateDrives() {
    drives_.clear();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        // Diskettenlaufwerke werden nicht unterstützt
        if ((i == 0 || i == 1) && type == DRIVE_REMOVABLE) continue;
        SHSTOCKICONID sid = SIID_DRIVEFIXED;
        switch (type) {
        case DRIVE_REMOVABLE: sid = SIID_DRIVEREMOVE; break;
        case DRIVE_REMOTE: sid = SIID_DRIVENET; break;
        case DRIVE_CDROM: sid = SIID_DRIVECD; break;
        case DRIVE_RAMDISK: sid = SIID_DRIVERAM; break;
        default: sid = SIID_DRIVEFIXED; break;
        }
        drives_.push_back({root, StockIcon(sid), type});
    }
    BuildHitRects();
    InvalidateRect(hwnd_, &rcHeader_, FALSE);
}

// ===================== Layout und Zeichnen =====================

void FilePane::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int hh = ToPx(kHeaderH), ph = ToPx(kPathH), sh = ToPx(kStatusH);
    rcHeader_ = {0, 0, rc.right, hh};
    rcPath_ = {0, hh, rc.right, hh + ph};
    rcStatus_ = {0, std::max<LONG>(hh + ph, rc.bottom - sh), rc.right, rc.bottom};
    rcList_ = {0, hh + ph, rc.right, rcStatus_.top};
    int btnW = ToPx(kNavBtnW);
    int comboW = std::max<int>(ToPx(40), rc.right - 5 * btnW - ToPx(6));
    // Combobox vertikal zentriert
    RECT cr;
    GetWindowRect(pathCombo_, &cr);
    int ch = cr.bottom - cr.top;
    SetWindowPos(pathCombo_, nullptr, ToPx(2), rcPath_.top + (ph - ch) / 2, comboW, ToPx(300), SWP_NOZORDER | SWP_NOACTIVATE);
    // Eine Combobox markiert beim Größenändern ihren Text – Markierung wieder aufheben
    if (pathEdit_ && GetFocus() != pathEdit_) {
        int len = GetWindowTextLengthW(pathEdit_);
        SendMessageW(pathEdit_, EM_SETSEL, len, len);
    }
    HWND content = quickView_ && viewer_ ? viewer_ : list_;
    SetWindowPos(content, nullptr, rcList_.left, rcList_.top, rcList_.right - rcList_.left, rcList_.bottom - rcList_.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    BuildHitRects();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FilePane::BuildHitRects() {
    hits_.clear();
    hot_ = nullptr;
    if (!hwnd_) return;
    int hh = ToPx(kHeaderH);
    int pad = ToPx(2);
    int bs = hh - 2 * pad; // quadratische Schaltfläche
    int x = rcHeader_.right - pad;
    // ganz rechts: Split, links daneben: Lesezeichen
    hits_.push_back({HitArea::Split, -1, {x - bs, pad, x, pad + bs}});
    x -= bs + pad;
    hits_.push_back({HitArea::Bookmark, -1, {x - bs, pad, x, pad + bs}});
    int rightLimit = x - bs - ToPx(6);
    // Laufwerke von links
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, App::UIFont());
    int dx = pad + ToPx(2);
    for (int i = 0; i < (int)drives_.size(); ++i) {
        wchar_t letter[2] = {drives_[i].root[0], 0};
        SIZE sz{};
        GetTextExtentPoint32W(dc, letter, 1, &sz);
        int w = ToPx(16) + ToPx(3) + sz.cx + ToPx(8);
        if (dx + w > rightLimit) break;
        hits_.push_back({HitArea::Drive, i, {dx, pad, dx + w, pad + bs}});
        dx += w + ToPx(1);
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    // Pfadzeile: Schaltflächen rechts neben der Combobox
    int btnW = ToPx(kNavBtnW);
    int px = rcPath_.right - ToPx(2);
    int ph = rcPath_.bottom - rcPath_.top;
    int top = rcPath_.top + ToPx(2), bottom = rcPath_.top + ph - ToPx(2);
    // ganz rechts: Kreis für aktive/inaktive Liste
    hits_.push_back({HitArea::Active, -1, {px - btnW, top, px, bottom}});
    px -= btnW;
    HitArea order[4] = {HitArea::Browse, HitArea::Up, HitArea::Forward, HitArea::Back};
    for (HitArea a : order) {
        hits_.push_back({a, -1, {px - btnW, top, px, bottom}});
        px -= btnW;
    }
    UpdateTooltips();
}

void FilePane::UpdateTooltips() {
    if (!tooltip_) return;
    // Alle Werkzeuge entfernen und neu anlegen
    int n = (int)SendMessageW(tooltip_, TTM_GETTOOLCOUNT, 0, 0);
    for (int i = n - 1; i >= 0; --i) {
        TOOLINFOW ti{sizeof(ti)};
        if (SendMessageW(tooltip_, TTM_ENUMTOOLSW, i, (LPARAM)&ti)) SendMessageW(tooltip_, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    UINT id = 1;
    for (auto& h : hits_) {
        std::wstring text;
        switch (h.area) {
        case HitArea::Drive: text = L"Laufwerk " + drives_[h.drive].root.substr(0, 2); break;
        case HitArea::Split:
            text = splitButton_ == SplitButton::Split ? L"Split: Liste teilen (Strg+T)" : L"Split aufheben (Strg+T)";
            break;
        case HitArea::Bookmark: text = L"Gewähltes Verzeichnis den Lesezeichen hinzufügen (Strg+D)"; break;
        case HitArea::Back: text = L"Zurück (Alt+←)"; break;
        case HitArea::Forward: text = L"Vor (Alt+→)"; break;
        case HitArea::Up: text = L"Übergeordnetes Verzeichnis (Rücktaste)"; break;
        case HitArea::Browse: text = L"Verzeichnis wählen…"; break;
        case HitArea::Active: text = L"Aktive Liste (grün) – Klick macht diese Liste aktiv"; break;
        default: break;
        }
        TOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = id++;
        ti.rect = h.rc;
        ti.lpszText = const_cast<wchar_t*>(text.c_str());
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

const FilePane::HitRect* FilePane::HitTest(POINT pt) const {
    for (auto& h : hits_)
        if (PtInRect(&h.rc, pt)) return &h;
    return nullptr;
}

void FilePane::DrawGlyph(HDC dc, HitArea area, const RECT& rc, bool hot) {
    if (hot) {
        HBRUSH b = CreateSolidBrush(Blend(GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 190));
        FillRect(dc, &rc, b);
        DeleteObject(b);
        HBRUSH fr = CreateSolidBrush(Blend(GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 80));
        FrameRect(dc, &rc, fr);
        DeleteObject(fr);
    }
    COLORREF col = GetSysColor(COLOR_BTNTEXT);
    Glyph g = Glyph::Back;
    bool enabled = true;
    switch (area) {
    case HitArea::Back: g = Glyph::Back; enabled = CanGoBack(); break;
    case HitArea::Forward: g = Glyph::Forward; enabled = CanGoForward(); break;
    case HitArea::Up: g = Glyph::Up; enabled = !dir_.empty() && !IsRootPath(dir_); break;
    case HitArea::Browse: g = Glyph::Browse; break;
    case HitArea::Split: g = splitButton_ == SplitButton::Split ? Glyph::Split : Glyph::Unsplit; break;
    case HitArea::Bookmark: g = Glyph::Bookmark; break;
    default: return;
    }
    if (!enabled) col = GetSysColor(COLOR_GRAYTEXT);
    qf::DrawGlyph(dc, g, rc, col);
}

void FilePane::PaintHeader(HDC dc, const RECT& rc) {
    COLORREF face = GetSysColor(COLOR_BTNFACE);
    COLORREF bg = activeLook_ ? Blend(GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 150) : face;
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &rc, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, App::UIFont());
    std::wstring curRoot = PathRoot(dir_);
    for (auto& h : hits_) {
        if (h.area == HitArea::Drive) {
            const DriveInfo& d = drives_[h.drive];
            bool current = !curRoot.empty() && EqualsI(curRoot, d.root);
            bool hot = hot_ == &h;
            if (current || hot) {
                COLORREF c = current ? RGB(255, 255, 255) : Blend(GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 190);
                HBRUSH hb = CreateSolidBrush(c);
                FillRect(dc, &h.rc, hb);
                DeleteObject(hb);
                HBRUSH fr = CreateSolidBrush(current ? GetSysColor(COLOR_HIGHLIGHT) : Blend(GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 80));
                FrameRect(dc, &h.rc, fr);
                DeleteObject(fr);
            }
            int iconSize = ToPx(16);
            int iy = (h.rc.top + h.rc.bottom - iconSize) / 2;
            if (g_sysSmall) ImageList_DrawEx(g_sysSmall, d.icon, dc, h.rc.left + ToPx(3), iy, iconSize, iconSize, CLR_NONE, CLR_NONE, ILD_TRANSPARENT);
            RECT tr = h.rc;
            tr.left += ToPx(3) + iconSize + ToPx(3);
            wchar_t letter[2] = {d.root[0], 0};
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            DrawTextW(dc, letter, 1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        } else if (h.area == HitArea::Split || h.area == HitArea::Bookmark) {
            DrawGlyph(dc, h.area, h.rc, hot_ == &h);
        }
    }
    // Trennlinie unten
    HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, rc.left, rc.bottom - 1, nullptr);
    LineTo(dc, rc.right, rc.bottom - 1);
    SelectObject(dc, op);
    DeleteObject(pen);
    SelectObject(dc, oldFont);
}

void FilePane::PaintNavButtons(HDC dc) {
    HBRUSH b = GetSysColorBrush(COLOR_BTNFACE);
    FillRect(dc, &rcPath_, b);
    for (auto& h : hits_)
        if (h.area == HitArea::Back || h.area == HitArea::Forward || h.area == HitArea::Up || h.area == HitArea::Browse)
            DrawGlyph(dc, h.area, h.rc, hot_ == &h);
        else if (h.area == HitArea::Active) {
            // Kreis: grün gefüllt = aktive Liste, hohl = nicht aktiv (Klick aktiviert)
            int w = h.rc.right - h.rc.left, hgt = h.rc.bottom - h.rc.top;
            int d = std::max(8, std::min(w, hgt) * 55 / 100);
            int cx = (h.rc.left + h.rc.right) / 2, cy = (h.rc.top + h.rc.bottom) / 2;
            if (hot_ == &h && !activeLook_) {
                HBRUSH hb = CreateSolidBrush(Blend(GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 190));
                FillRect(dc, &h.rc, hb);
                DeleteObject(hb);
            }
            COLORREF line = activeLook_ ? RGB(20, 120, 40) : RGB(110, 110, 110);
            HPEN pen = CreatePen(PS_SOLID, std::max(1, ToPx(2) / 2 + (activeLook_ ? 0 : 1)), line);
            HBRUSH br = activeLook_ ? CreateSolidBrush(RGB(40, 180, 70)) : (HBRUSH)GetStockObject(NULL_BRUSH);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
            Ellipse(dc, cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
            if (activeLook_) DeleteObject(br);
        }
}

void FilePane::PaintStatus(HDC dc, const RECT& rc) {
    HBRUSH b = GetSysColorBrush(COLOR_BTNFACE);
    FillRect(dc, &rc, b);
    HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, rc.left, rc.top, nullptr);
    LineTo(dc, rc.right, rc.top);
    SelectObject(dc, op);
    DeleteObject(pen);
    HGDIOBJ oldFont = SelectObject(dc, App::UIFont());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    RECT tr = rc;
    tr.left += ToPx(4);
    tr.right -= ToPx(4);
    DrawTextW(dc, statusText_.c_str(), (int)statusText_.size(), &tr,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void FilePane::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // Doppelpufferung
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    PaintHeader(mem, rcHeader_);
    PaintNavButtons(mem);
    PaintStatus(mem, rcStatus_);
    BitBlt(dc, 0, 0, rc.right, rcHeader_.bottom, mem, 0, 0, SRCCOPY);
    BitBlt(dc, 0, rcPath_.top, rc.right, rcPath_.bottom - rcPath_.top, mem, 0, rcPath_.top, SRCCOPY);
    BitBlt(dc, 0, rcStatus_.top, rc.right, rcStatus_.bottom - rcStatus_.top, mem, 0, rcStatus_.top, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void FilePane::SetActiveLook(bool active) {
    if (activeLook_ == active) return;
    activeLook_ = active;
    InvalidateRect(hwnd_, &rcHeader_, FALSE);
    InvalidateRect(hwnd_, &rcPath_, FALSE);
}

void FilePane::SetSplitButton(SplitButton b) {
    splitButton_ = b;
    UpdateTooltips();
    InvalidateRect(hwnd_, &rcHeader_, FALSE);
}

void FilePane::OnClickArea(const HitRect& h) {
    if (host_) host_->OnPaneActivated(this);
    switch (h.area) {
    case HitArea::Drive: {
        const std::wstring& root = drives_[h.drive].root;
        std::wstring target = root;
        for (auto& p : lastDirPerDrive_)
            if (EqualsI(p.first, root) && DirExists(p.second)) target = p.second;
        if (EqualsI(PathRoot(dir_), root)) target = root; // erneuter Klick auf aktuelles Laufwerk -> Wurzel
        Navigate(target);
        break;
    }
    case HitArea::Split:
        // Der Host setzt den Fokus selbst; diese Liste kann danach unsichtbar sein (Split aufgehoben)
        if (host_) host_->OnPaneSplitButton(this);
        return;
    case HitArea::Bookmark:
        if (host_) host_->OnPaneBookmarkButton(this);
        break;
    case HitArea::Back: GoBack(); break;
    case HitArea::Forward: GoForward(); break;
    case HitArea::Up: GoUp(); break;
    case HitArea::Browse: {
        std::wstring d = BrowseForFolder(GetAncestor(hwnd_, GA_ROOT), L"Verzeichnis wählen", dir_);
        if (!d.empty()) Navigate(d);
        break;
    }
    default: break;
    }
    FocusList();
}

// ===================== Verzeichnis lesen =====================

bool FilePane::ReadDirectory(const std::wstring& dir, DWORD* err) {
    std::vector<DirEntry> entries;
    if (!ListDirectory(dir, entries, err)) return false;
    const Options& o = App::Opt();
    std::vector<Item> items;
    items.reserve(entries.size() + 1);
    if (!IsRootPath(dir)) {
        Item up;
        up.isParent = true;
        up.e.name = L"..";
        up.e.attributes = FILE_ATTRIBUTE_DIRECTORY;
        items.push_back(up);
    }
    for (auto& e : entries) {
        if (!o.showHidden && (e.attributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        if (!o.showSystem && (e.attributes & FILE_ATTRIBUTE_SYSTEM) && (e.attributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        if (filter_.IsActive() && (!e.IsDir() || filter_.applyToDirs)) {
            if (!filter_.include.empty() && !MatchAnyPattern(filter_.include, e.name)) continue;
            if (!filter_.exclude.empty() && MatchAnyPattern(filter_.exclude, e.name)) continue;
        }
        Item it;
        it.e = std::move(e);
        auto m = marks_.find(ToLower(it.e.name));
        if (m != marks_.end()) it.mark = m->second;
        items.push_back(std::move(it));
    }
    // Neue Elemente fordern ihre Miniaturen neu an: alte Bilder (außer dem Platzhalter 0) verwerfen,
    // sonst wächst die Bildliste bei jedem Neueinlesen (z. B. durch die Verzeichnisüberwachung) unbegrenzt.
    if (thumbList_) ImageList_SetImageCount(thumbList_, 1);
    items_ = std::move(items);
    return true;
}

void FilePane::SortItems() {
    const bool dirsFirst = App::Opt().dirsFirst;
    const SortKey key = sortKey_;
    const bool desc = sortDesc_;
    std::stable_sort(items_.begin(), items_.end(), [&](const Item& a, const Item& b) {
        if (a.isParent != b.isParent) return a.isParent;
        if (dirsFirst && a.e.IsDir() != b.e.IsDir()) return a.e.IsDir();
        int c = 0;
        switch (key) {
        case SortKey::Name: break;
        case SortKey::Ext:
            if (!(a.e.IsDir() && b.e.IsDir())) c = CompareI(ExtOf(a.e.name), ExtOf(b.e.name));
            break;
        case SortKey::Size: {
            uint64_t sa = a.e.IsDir() ? (a.dirSize == UINT64_MAX ? 0 : a.dirSize) : a.e.size;
            uint64_t sb = b.e.IsDir() ? (b.dirSize == UINT64_MAX ? 0 : b.dirSize) : b.e.size;
            c = sa < sb ? -1 : (sa > sb ? 1 : 0);
            break;
        }
        case SortKey::Date: c = CompareFileTime(&a.e.modified, &b.e.modified); break;
        case SortKey::Created: c = CompareFileTime(&a.e.created, &b.e.created); break;
        case SortKey::Attr: c = (int)(a.e.attributes & 0xFF) - (int)(b.e.attributes & 0xFF); break;
        }
        if (c == 0) c = CompareNatural(a.e.name, b.e.name);
        return desc ? c > 0 : c < 0;
    });
}

int FilePane::FindItem(const std::wstring& name) const {
    for (size_t i = 0; i < items_.size(); ++i)
        if (EqualsI(items_[i].e.name, name)) return (int)i;
    return -1;
}

void FilePane::FillList(const std::wstring& focusName, const std::vector<std::wstring>& selected, int topIndex) {
    generation_++;
    {
        std::lock_guard<std::mutex> lock(thumbMutex_);
        thumbQueue_.clear();
    }
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(list_, (int)items_.size(), 0);
    for (auto& s : selected) {
        int i = FindItem(s);
        if (i >= 0 && !items_[i].isParent) ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
    }
    int focus = focusName.empty() ? -1 : FindItem(focusName);
    if (focus < 0 && !items_.empty()) focus = 0;
    if (focus >= 0) {
        ListView_SetItemState(list_, focus, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_SetSelectionMark(list_, focus);
        if (topIndex > 0 && view_ == PaneView::Details) {
            // Bisherige Bildlaufposition wiederherstellen
            RECT ir{};
            ListView_GetItemRect(list_, 0, &ir, LVIR_BOUNDS);
            int cur = ListView_GetTopIndex(list_);
            ListView_Scroll(list_, 0, (topIndex - cur) * (ir.bottom - ir.top));
        }
        ListView_EnsureVisible(list_, focus, FALSE);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
    UpdateStatus();
}

bool FilePane::Navigate(const std::wstring& rawDir, const std::wstring& focusName, bool addHistory, bool showErrors) {
    std::wstring dir = NormalizeDir(rawDir);
    if (dir.empty()) return false;
    // Datei angegeben -> deren Verzeichnis, Datei fokussieren
    std::wstring focus = focusName;
    if (FileExists(dir)) {
        focus = PathFileName(dir);
        dir = PathParent(dir);
    }
    bool sameDir = EqualsI(dir, dir_);
    std::wstring oldDir = dir_;
    DWORD err = 0;
    if (!ReadDirectory(dir, &err)) {
        if (showErrors) {
            std::wstring msg = L"Das Verzeichnis „" + dir + L"“ kann nicht geöffnet werden:\n" + LastErrorMessage(err);
            MsgError(GetAncestor(hwnd_, GA_ROOT), msg);
        }
        // items_ ist unverändert (ReadDirectory ändert bei Fehler nichts) und passt weiter zur ListView.
        // Ein erneutes Einlesen hier würde items_ unsortiert und mit anderer Anzahl als die ListView hinterlassen.
        return false;
    }
    if (!sameDir) {
        ClearMarks();
        for (auto& it : items_) it.mark = CompareMark::None;
        // Beim Wechsel nach oben: das bisherige Unterverzeichnis fokussieren
        if (focus.empty() && !oldDir.empty() && EqualsI(PathParent(oldDir), dir)) focus = PathFileName(oldDir);
    }
    dir_ = dir;
    SortItems();
    FillList(focus, {}, 0);
    if (!sameDir) {
        if (addHistory) {
            // Vorwärtsliste abschneiden
            if (histPos_ + 1 < (int)hist_.size()) hist_.erase(hist_.begin() + histPos_ + 1, hist_.end());
            hist_.push_back(dir_);
            if (hist_.size() > 100) hist_.erase(hist_.begin());
            histPos_ = (int)hist_.size() - 1;
        }
        AddHistory(dir_);
        // Letztes Verzeichnis je Laufwerk merken
        std::wstring root = PathRoot(dir_);
        bool found = false;
        for (auto& p : lastDirPerDrive_)
            if (EqualsI(p.first, root)) {
                p.second = dir_;
                found = true;
            }
        if (!found) lastDirPerDrive_.emplace_back(root, dir_);
        StartWatch();
        if (sizeCancel_) *sizeCancel_ = true;
    }
    // Freier Speicher
    ULARGE_INTEGER freeAvail{};
    freeKnown_ = GetDiskFreeSpaceExW(dir_.c_str(), &freeAvail, nullptr, nullptr) != 0;
    freeBytes_ = freeAvail.QuadPart;
    UpdatePathCombo();
    UpdateStatus();
    InvalidateRect(hwnd_, &rcHeader_, FALSE);
    InvalidateRect(hwnd_, &rcPath_, FALSE);
    if (host_) {
        host_->OnPaneDirChanged(this);
        host_->OnPaneFocusItemChanged(this);
    }
    return true;
}

void FilePane::Reload() {
    if (dir_.empty()) return;
    std::wstring focus;
    int fi = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (fi >= 0 && fi < (int)items_.size()) focus = items_[fi].e.name;
    std::vector<std::wstring> sel;
    int i = -1;
    while ((i = ListView_GetNextItem(list_, i, LVNI_SELECTED)) >= 0)
        if (i < (int)items_.size()) sel.push_back(items_[i].e.name);
    int top = ListView_GetTopIndex(list_);
    // Verzeichnisgrößen erhalten
    std::unordered_map<std::wstring, uint64_t> sizes;
    for (auto& it : items_)
        if (it.dirSize != UINT64_MAX) sizes[ToLower(it.e.name)] = it.dirSize;
    DWORD err = 0;
    if (!ReadDirectory(dir_, &err)) {
        // Verzeichnis existiert nicht mehr: zum nächsten vorhandenen Elternverzeichnis
        std::wstring old = dir_;
        // Vorhandenes, aber nicht lesbares Verzeichnis (z. B. Zugriff verweigert): zum Elternverzeichnis
        std::wstring p = DirExists(dir_) ? PathParent(dir_) : dir_;
        while (!p.empty() && !DirExists(p)) p = PathParent(p);
        if (p.empty()) p = L"C:\\";
        dir_.clear();
        if (!Navigate(p, L"", true, false) && !Navigate(L"C:\\", L"", true, false)) dir_ = old;
        return;
    }
    for (auto& it : items_) {
        auto s = sizes.find(ToLower(it.e.name));
        if (s != sizes.end()) it.dirSize = s->second;
    }
    SortItems();
    // Fokus: gelöschtes Element -> Nachfolger an gleicher Position
    if (!focus.empty() && FindItem(focus) < 0 && fi >= 0 && !items_.empty())
        focus = items_[std::min<int>(fi, (int)items_.size() - 1)].e.name;
    FillList(focus, sel, top);
    ULARGE_INTEGER freeAvail{};
    freeKnown_ = GetDiskFreeSpaceExW(dir_.c_str(), &freeAvail, nullptr, nullptr) != 0;
    freeBytes_ = freeAvail.QuadPart;
    UpdateStatus();
    if (host_) host_->OnPaneFocusItemChanged(this);
}

void FilePane::GoUp() {
    if (dir_.empty() || IsRootPath(dir_)) return;
    Navigate(PathParent(dir_), PathFileName(dir_));
}

void FilePane::GoRoot() {
    std::wstring r = PathRoot(dir_);
    if (!r.empty()) Navigate(r);
}

void FilePane::GoBack() {
    while (histPos_ > 0) {
        --histPos_;
        if (Navigate(hist_[histPos_], L"", false, false)) return;
        hist_.erase(hist_.begin() + histPos_);
    }
}

void FilePane::GoForward() {
    while (histPos_ + 1 < (int)hist_.size()) {
        ++histPos_;
        if (Navigate(hist_[histPos_], L"", false, false)) return;
        hist_.erase(hist_.begin() + histPos_);
        --histPos_;
    }
}

void FilePane::AddHistory(const std::wstring& dir) {
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const std::wstring& s) { return EqualsI(s, dir); }),
                  recent_.end());
    recent_.insert(recent_.begin(), dir);
    size_t maxN = (size_t)std::max(5, App::Opt().historySize);
    if (recent_.size() > maxN) recent_.resize(maxN);
}

void FilePane::UpdatePathCombo() {
    SendMessageW(pathCombo_, CB_RESETCONTENT, 0, 0);
    for (auto& r : recent_) SendMessageW(pathCombo_, CB_ADDSTRING, 0, (LPARAM)r.c_str());
    SetWindowTextW(pathCombo_, dir_.c_str());
    // Keine Markierung im Textfeld, Schreibmarke ans Ende (zeigt das letzte Glied des Pfads)
    if (pathEdit_ && GetFocus() != pathEdit_) {
        int len = GetWindowTextLengthW(pathEdit_);
        SendMessageW(pathEdit_, EM_SETSEL, len, len);
    }
}

std::wstring FilePane::ItemFullPath(const Item& it) const {
    if (it.isParent) return PathParent(dir_);
    return PathCombine(dir_, it.e.name);
}

int FilePane::ItemIconIndex(Item& it) {
    if (it.icon >= 0) return it.icon;
    SHFILEINFOW sfi{};
    if (it.isParent) {
        it.icon = StockIcon(SIID_FOLDERBACK);
        return it.icon;
    }
    if (it.e.IsDir()) {
        if (g_folderIcon < 0) {
            SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                           SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
            g_folderIcon = sfi.iIcon;
        }
        it.icon = g_folderIcon;
        return it.icon;
    }
    std::wstring ext = ToLower(ExtOf(it.e.name));
    if (HasOwnIcon(ext)) {
        if (SHGetFileInfoW(LongPath(PathCombine(dir_, it.e.name)).c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            it.icon = sfi.iIcon;
        else
            it.icon = 0;
        return it.icon;
    }
    auto f = g_extIcons.find(ext);
    if (f != g_extIcons.end()) {
        it.icon = f->second;
        return it.icon;
    }
    SHGetFileInfoW(ext.empty() ? L"datei" : (L"x" + ext).c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    g_extIcons[ext] = sfi.iIcon;
    it.icon = sfi.iIcon;
    return it.icon;
}

void FilePane::OnGetDispInfo(NMLVDISPINFOW* di) {
    int i = di->item.iItem;
    if (i < 0 || i >= (int)items_.size()) return;
    Item& it = items_[i];
    if (di->item.mask & LVIF_TEXT) {
        std::wstring s;
        int sub = di->item.iSubItem;
        int colId = (sub >= 0 && sub < (int)colIds_.size()) ? colIds_[sub] : sub;
        switch (colId) {
        case ColName: s = it.e.name; break;
        case ColExt:
            if (it.isParent) s = L"";
            else if (it.e.IsDir()) s = L"<Verz>";
            else {
                s = ExtOf(it.e.name);
                if (!s.empty()) s = s.substr(1);
            }
            break;
        case ColSize:
            if (it.isParent) s = L"";
            else if (it.e.IsDir()) s = it.dirSize == UINT64_MAX ? L"" : (App::Opt().sizeInBytes ? FormatSizeBytes(it.dirSize) : FormatSize(it.dirSize));
            else s = App::Opt().sizeInBytes ? FormatSizeBytes(it.e.size) : FormatSize(it.e.size);
            break;
        case ColDate:
            if (!it.isParent) s = FormatFileTime(it.e.modified, App::Opt().dateWithSeconds);
            break;
        case ColCreated:
            if (!it.isParent) s = FormatFileTime(it.e.created, App::Opt().dateWithSeconds);
            break;
        case ColAttr:
            if (!it.isParent) s = FormatAttributes(it.e.attributes);
            break;
        }
        CopyToBuf(di->item.pszText, di->item.cchTextMax, s);
    }
    if (di->item.mask & LVIF_IMAGE) {
        if (view_ == PaneView::Thumbnails) {
            if (it.thumb >= 0) {
                di->item.iImage = it.thumb;
            } else {
                di->item.iImage = 0;
                if (it.thumb == -1 && !it.isParent) {
                    it.thumb = -2;
                    RequestThumb(i);
                }
            }
        } else {
            di->item.iImage = ItemIconIndex(it);
        }
    }
    if (di->item.mask & LVIF_STATE) {
        di->item.state = 0;
        if (!it.isParent && (it.e.attributes & FILE_ATTRIBUTE_HIDDEN)) di->item.state |= LVIS_CUT;
        di->item.stateMask |= LVIS_CUT;
    }
}

LRESULT FilePane::OnCustomDraw(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        int i = (int)cd->nmcd.dwItemSpec;
        if (i < 0 || i >= (int)items_.size()) return CDRF_DODEFAULT;
        const Item& it = items_[i];
        bool changed = false;
        switch (it.mark) {
        case CompareMark::Equal: cd->clrTextBk = RGB(235, 235, 235); changed = true; break;
        case CompareMark::Different: cd->clrTextBk = RGB(255, 245, 180); changed = true; break;
        case CompareMark::Newer: cd->clrTextBk = RGB(200, 240, 200); changed = true; break;
        case CompareMark::Older: cd->clrTextBk = RGB(255, 215, 200); changed = true; break;
        case CompareMark::Missing: cd->clrTextBk = RGB(205, 225, 255); changed = true; break;
        case CompareMark::Duplicate: cd->clrTextBk = RGB(235, 215, 250); changed = true; break;
        default: break;
        }
        if (!it.isParent && (it.e.attributes & FILE_ATTRIBUTE_HIDDEN)) {
            cd->clrText = GetSysColor(COLOR_GRAYTEXT);
            changed = true;
        } else if (!it.isParent && (it.e.attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED))) {
            cd->clrText = (it.e.attributes & FILE_ATTRIBUTE_ENCRYPTED) ? RGB(0, 128, 0) : RGB(0, 0, 200);
            changed = true;
        }
        return changed ? CDRF_NEWFONT : CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

void FilePane::UpdateStatus() {
    size_t files = 0, dirs = 0, selFiles = 0, selDirs = 0;
    uint64_t total = 0, selTotal = 0;
    for (auto& it : items_) {
        if (it.isParent) continue;
        if (it.e.IsDir()) {
            ++dirs;
            if (it.dirSize != UINT64_MAX) total += it.dirSize;
        } else {
            ++files;
            total += it.e.size;
        }
    }
    int i = -1;
    while ((i = ListView_GetNextItem(list_, i, LVNI_SELECTED)) >= 0) {
        if (i >= (int)items_.size() || items_[i].isParent) continue;
        if (items_[i].e.IsDir()) {
            ++selDirs;
            if (items_[i].dirSize != UINT64_MAX) selTotal += items_[i].dirSize;
        } else {
            ++selFiles;
            selTotal += items_[i].e.size;
        }
    }
    std::wstring s = IntToStr((long long)dirs) + L" Verz., " + IntToStr((long long)files) + L" Dateien (" + FormatSize(total) + L")";
    if (selFiles + selDirs)
        s += L"  –  markiert: " + IntToStr((long long)(selFiles + selDirs)) + L" (" + FormatSize(selTotal) + L")";
    if (filter_.IsActive()) s += L"  –  Filter: " + filter_.include + (filter_.exclude.empty() ? L"" : (L" ohne " + filter_.exclude));
    if (freeKnown_) s += L"  –  frei: " + FormatSize(freeBytes_);
    statusText_ = s;
    InvalidateRect(hwnd_, &rcStatus_, FALSE);
}

// ===================== Auswahl =====================

std::vector<std::wstring> FilePane::SelectedNames() const {
    std::vector<std::wstring> r;
    int i = -1;
    while ((i = ListView_GetNextItem(list_, i, LVNI_SELECTED)) >= 0)
        if (i < (int)items_.size() && !items_[i].isParent) r.push_back(items_[i].e.name);
    return r;
}

std::vector<std::wstring> FilePane::SelectedPaths() const {
    std::vector<std::wstring> r;
    for (auto& n : SelectedNames()) r.push_back(PathCombine(dir_, n));
    return r;
}

std::wstring FilePane::FocusedName() const {
    int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (i < 0 || i >= (int)items_.size() || items_[i].isParent) return L"";
    return items_[i].e.name;
}

std::wstring FilePane::FocusedPath() const {
    std::wstring n = FocusedName();
    return n.empty() ? L"" : PathCombine(dir_, n);
}

bool FilePane::FocusedIsDir() const {
    int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    return i >= 0 && i < (int)items_.size() && items_[i].e.IsDir();
}

bool FilePane::FocusedIsParent() const {
    int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    return i >= 0 && i < (int)items_.size() && items_[i].isParent;
}

std::vector<std::wstring> FilePane::SelectedOrFocusedNames() const {
    auto s = SelectedNames();
    if (s.empty()) {
        std::wstring f = FocusedName();
        if (!f.empty()) s.push_back(f);
    }
    return s;
}

std::vector<std::wstring> FilePane::SelectedOrFocusedPaths() const {
    std::vector<std::wstring> r;
    for (auto& n : SelectedOrFocusedNames()) r.push_back(PathCombine(dir_, n));
    return r;
}

void FilePane::SelectNames(const std::vector<std::wstring>& names, bool focusFirst) {
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
    int first = -1;
    for (auto& n : names) {
        int i = FindItem(n);
        if (i >= 0 && !items_[i].isParent) {
            ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
            if (first < 0 || i < first) first = i;
        }
    }
    if (focusFirst && first >= 0) {
        ListView_SetItemState(list_, first, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_EnsureVisible(list_, first, FALSE);
    }
    UpdateStatus();
}

void FilePane::FocusName(const std::wstring& name) {
    int i = FindItem(name);
    if (i < 0) return;
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(list_, i, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    ListView_SetSelectionMark(list_, i);
    ListView_EnsureVisible(list_, i, FALSE);
}

void FilePane::SelectAll() {
    ListView_SetItemState(list_, -1, LVIS_SELECTED, LVIS_SELECTED);
    if (!items_.empty() && items_[0].isParent) ListView_SetItemState(list_, 0, 0, LVIS_SELECTED);
    UpdateStatus();
}

void FilePane::SelectNone() {
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
    UpdateStatus();
}

void FilePane::InvertSelection() {
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    for (int i = 0; i < (int)items_.size(); ++i) {
        if (items_[i].isParent) continue;
        UINT st = ListView_GetItemState(list_, i, LVIS_SELECTED);
        ListView_SetItemState(list_, i, st & LVIS_SELECTED ? 0 : LVIS_SELECTED, LVIS_SELECTED);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, FALSE);
    UpdateStatus();
}

void FilePane::SelectPattern(const std::wstring& patterns, bool select) {
    for (int i = 0; i < (int)items_.size(); ++i) {
        if (items_[i].isParent) continue;
        if (MatchAnyPattern(patterns, items_[i].e.name))
            ListView_SetItemState(list_, i, select ? LVIS_SELECTED : 0, LVIS_SELECTED);
    }
    UpdateStatus();
}

void FilePane::SelectSameExtension() {
    std::wstring f = FocusedName();
    if (f.empty()) return;
    std::wstring ext = ExtOf(f);
    SelectPattern(ext.empty() ? L"*." : (L"*" + ext), true);
}

PaneContext FilePane::Context() const {
    PaneContext c;
    c.dir = dir_;
    c.selected = SelectedNames();
    c.focused = FocusedName();
    return c;
}

void FilePane::ToggleSelectFocused(bool moveDown) {
    int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (i < 0 || i >= (int)items_.size()) return;
    if (!items_[i].isParent) {
        UINT st = ListView_GetItemState(list_, i, LVIS_SELECTED);
        ListView_SetItemState(list_, i, st & LVIS_SELECTED ? 0 : LVIS_SELECTED, LVIS_SELECTED);
    }
    if (moveDown && i + 1 < (int)items_.size()) {
        ListView_SetItemState(list_, i + 1, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_EnsureVisible(list_, i + 1, FALSE);
    }
    UpdateStatus();
}

// ===================== Darstellung =====================

void FilePane::SetView(PaneView v) {
    view_ = v;
    if (v != PaneView::Thumbnails) StopThumbWorker();
    ApplyViewStyle();
    int f = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    InvalidateRect(list_, nullptr, TRUE);
    if (f >= 0) ListView_EnsureVisible(list_, f, FALSE);
}

void FilePane::SetSort(SortKey k, bool descending) {
    sortKey_ = k;
    sortDesc_ = descending;
    std::wstring focus = FocusedName();
    auto sel = SelectedNames();
    SortItems();
    SetupColumns();
    FillList(focus, sel, 0);
}

void FilePane::SetFilter(const PaneFilter& f) {
    filter_ = f;
    Reload();
}

void FilePane::SetMarks(const CompareMarks& marks) {
    marks_ = marks;
    for (auto& it : items_) {
        auto m = marks_.find(ToLower(it.e.name));
        it.mark = m != marks_.end() ? m->second : CompareMark::None;
    }
    InvalidateRect(list_, nullptr, FALSE);
}

void FilePane::ClearMarks() {
    marks_.clear();
    for (auto& it : items_) it.mark = CompareMark::None;
    if (list_) InvalidateRect(list_, nullptr, FALSE);
}

void FilePane::BeginRename() {
    int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (i < 0 || i >= (int)items_.size() || items_[i].isParent) return;
    SetFocus(list_);
    ListView_EnsureVisible(list_, i, FALSE);
    ListView_EditLabel(list_, i);
}

void FilePane::FocusList() {
    if (quickView_ && viewer_) return;
    SetFocus(list_);
}

void FilePane::FocusPath() {
    SetFocus(pathCombo_);
    if (pathEdit_) SendMessageW(pathEdit_, EM_SETSEL, 0, -1);
}

void FilePane::OpenFocused() {
    int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (i < 0 || i >= (int)items_.size()) return;
    if (items_[i].isParent) {
        GoUp();
        return;
    }
    if (items_[i].e.IsDir()) {
        Navigate(PathCombine(dir_, items_[i].e.name));
        return;
    }
    if (host_) host_->OnPaneOpenItem(this);
}

std::wstring FilePane::DropTargetDirAt(POINT screenPt) const {
    if (dir_.empty()) return L"";
    LVHITTESTINFO ht{};
    ht.pt = screenPt;
    ScreenToClient(list_, &ht.pt);
    int i = ListView_HitTest(list_, &ht);
    if (i >= 0 && i < (int)items_.size() && (ht.flags & LVHT_ONITEM)) {
        const Item& it = items_[i];
        if (it.isParent) return PathParent(dir_);
        if (it.e.IsDir()) return PathCombine(dir_, it.e.name);
    }
    return dir_;
}

// ===================== Schnellansicht =====================

void FilePane::SetQuickView(bool on) {
    if (quickView_ == on) return;
    quickView_ = on;
    if (on) {
        if (!viewer_) viewer_ = CreateFileViewer(hwnd_, kIdViewer);
        ShowWindow(list_, SW_HIDE);
        if (viewer_) ShowWindow(viewer_, SW_SHOW);
        statusText_ = L"Dateianzeige";
    } else {
        if (viewer_) {
            FileViewerLoad(viewer_, L"");
            ShowWindow(viewer_, SW_HIDE);
        }
        ShowWindow(list_, SW_SHOW);
        UpdateStatus();
    }
    Layout();
}

void FilePane::QuickViewLoad(const std::wstring& path) {
    if (!quickView_ || !viewer_) return;
    FileViewerLoad(viewer_, path);
    statusText_ = path.empty() ? L"Dateianzeige" : (L"Dateianzeige: " + path);
    InvalidateRect(hwnd_, &rcStatus_, FALSE);
}

// ===================== Verzeichnisgrößen =====================

void FilePane::ComputeDirSizes(bool selectedOnly) {
    std::vector<std::wstring> names;
    if (selectedOnly) {
        for (auto& n : SelectedOrFocusedNames()) {
            int i = FindItem(n);
            if (i >= 0 && items_[i].e.IsDir()) names.push_back(n);
        }
    } else {
        for (auto& it : items_)
            if (!it.isParent && it.e.IsDir()) names.push_back(it.e.name);
    }
    if (names.empty()) return;
    if (sizeCancel_) *sizeCancel_ = true;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    sizeCancel_ = cancel;
    unsigned gen = generation_;
    HWND hwnd = hwnd_;
    std::wstring dir = dir_;
    statusText_ = L"Berechne Verzeichnisgrößen …";
    InvalidateRect(hwnd_, &rcStatus_, FALSE);
    std::thread([=]() {
        for (auto& n : names) {
            if (*cancel) return;
            uint64_t total = 0;
            WalkDirectory(PathCombine(dir, n), [&](const std::wstring&, const DirEntry& e) {
                if (!e.IsDir()) total += e.size;
                return !*cancel;
            });
            if (*cancel) return;
            auto* r = new DirSizeResult{gen, n, total};
            if (!PostMessageW(hwnd, WM_APP_DIRSIZE, 0, (LPARAM)r)) delete r;
        }
        PostMessageW(hwnd, WM_APP_DIRSIZE_DONE, gen, 0);
    }).detach();
}

// ===================== Miniaturen =====================

void FilePane::RequestThumb(int index) {
    {
        std::lock_guard<std::mutex> lock(thumbMutex_);
        thumbQueue_.push_front({generation_, index, ItemFullPath(items_[index])});
        thumbStop_ = false;
    }
    if (!thumbThread_.joinable()) {
        HWND hwnd = hwnd_;
        int size = thumbSize_;
        thumbThread_ = std::thread([this, hwnd, size]() {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (;;) {
                ThumbRequest req;
                {
                    std::unique_lock<std::mutex> lock(thumbMutex_);
                    thumbCv_.wait(lock, [&] { return thumbStop_ || !thumbQueue_.empty(); });
                    if (thumbStop_) break;
                    req = thumbQueue_.front();
                    thumbQueue_.pop_front();
                }
                if (req.gen != generation_) continue;
                HBITMAP bmp = nullptr;
                IShellItemImageFactory* f = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(req.path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
                    SIZE sz{size, size};
                    f->GetImage(sz, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &bmp);
                    f->Release();
                }
                auto* r = new ThumbResult{req.gen, req.index, bmp};
                if (!PostMessageW(hwnd, WM_APP_THUMB, 0, (LPARAM)r)) {
                    if (bmp) DeleteObject(bmp);
                    delete r;
                }
            }
            CoUninitialize();
        });
    }
    thumbCv_.notify_one();
}

void FilePane::StopThumbWorker() {
    if (thumbThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(thumbMutex_);
            thumbStop_ = true;
            thumbQueue_.clear();
        }
        thumbCv_.notify_all();
        thumbThread_.join();
        thumbStop_ = false;
    }
}

int FilePane::AddThumbnail(HBITMAP src) {
    // Quellpixel als 32-Bit-DIB (top-down) holen
    BITMAP bm{};
    if (!GetObject(src, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return 0;
    int sw = bm.bmWidth, sh = std::abs(bm.bmHeight);
    std::vector<uint32_t> px((size_t)sw * sh);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), sw, -sh, 1, 32, BI_RGB};
    HDC screen = GetDC(nullptr);
    int lines = GetDIBits(screen, src, 0, sh, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (lines <= 0) return 0;
    // Bitmaps ohne Alphakanal (alle Alpha = 0) sind deckend
    bool hasAlpha = false;
    if (bm.bmBitsPixel == 32)
        for (uint32_t p : px)
            if (p & 0xFF000000u) {
                hasAlpha = true;
                break;
            }
    if (!hasAlpha)
        for (auto& p : px) p |= 0xFF000000u;
    void* srcBits = nullptr;
    HBITMAP tmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &srcBits, nullptr, 0);
    if (!tmp || !srcBits) {
        if (tmp) DeleteObject(tmp);
        return 0;
    }
    memcpy(srcBits, px.data(), px.size() * 4);
    // Zielbild: quadratisch, Seitenverhältnis erhalten, zentriert
    int s = thumbSize_;
    double scale = std::min(1.0, std::min((double)s / sw, (double)s / sh));
    int w = std::max(1, (int)(sw * scale)), h = std::max(1, (int)(sh * scale));
    BITMAPINFO ci{};
    ci.bmiHeader = {sizeof(BITMAPINFOHEADER), s, -s, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP canvas = CreateDIBSection(nullptr, &ci, DIB_RGB_COLORS, &bits, nullptr, 0);
    int index = 0;
    if (canvas && bits) {
        memset(bits, 0, (size_t)s * s * 4);
        HDC dc = CreateCompatibleDC(nullptr);
        HDC sdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o1 = SelectObject(dc, canvas);
        HGDIOBJ o2 = SelectObject(sdc, tmp);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        SetStretchBltMode(dc, HALFTONE);
        AlphaBlend(dc, (s - w) / 2, (s - h) / 2, w, h, sdc, 0, 0, sw, sh, bf);
        SelectObject(dc, o1);
        SelectObject(sdc, o2);
        DeleteDC(dc);
        DeleteDC(sdc);
        index = ImageList_Add(thumbList_, canvas, nullptr);
        if (index < 0) index = 0;
    }
    if (canvas) DeleteObject(canvas);
    DeleteObject(tmp);
    return index;
}

void FilePane::OnThumbReady(WPARAM, LPARAM lp) {
    auto* r = (ThumbResult*)lp;
    if (r->gen == generation_ && r->index >= 0 && r->index < (int)items_.size() && thumbList_) {
        int index = 0;
        if (r->bmp) {
            index = AddThumbnail(r->bmp);
            DeleteObject(r->bmp);
        }
        items_[r->index].thumb = index >= 0 ? index : 0;
        ListView_RedrawItems(list_, r->index, r->index);
    } else if (r->bmp) {
        DeleteObject(r->bmp);
    }
    delete r;
}

// ===================== Verzeichnisüberwachung =====================

void FilePane::StartWatch() {
    StopWatch();
    if (!App::Opt().autoRefresh || dir_.empty()) return;
    watchStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stop = watchStop_;
    HWND hwnd = hwnd_;
    std::wstring dir = dir_;
    watchThread_ = std::thread([stop, hwnd, dir]() {
        HANDLE ch = FindFirstChangeNotificationW(
            LongPath(dir).c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES);
        if (ch == INVALID_HANDLE_VALUE) return;
        HANDLE hs[2] = {stop, ch};
        for (;;) {
            DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0 + 1) break;
            PostMessageW(hwnd, WM_APP_DIRCHANGED, 0, 0);
            if (!FindNextChangeNotification(ch)) break;
        }
        FindCloseChangeNotification(ch);
    });
}

void FilePane::StopWatch() {
    if (watchStop_) {
        SetEvent(watchStop_);
        if (watchThread_.joinable()) watchThread_.join();
        CloseHandle(watchStop_);
        watchStop_ = nullptr;
    }
}

// ===================== Einstellungen =====================

void FilePane::LoadState(const Config& c, const std::wstring& s) {
    view_ = (PaneView)std::clamp(c.GetInt(s, L"Ansicht", 0), 0, 3);
    sortKey_ = (SortKey)std::clamp(c.GetInt(s, L"Sortierung", 0), 0, 5);
    sortDesc_ = c.GetBool(s, L"Absteigend", false);
    auto widths = Split(c.Get(s, L"Spalten"), L',');
    for (int i = 0; i < ColCount && i < (int)widths.size(); ++i) {
        int w = (int)StrToInt(widths[i], kDefaultWidths[i]);
        if (w >= 20 && w <= 2000) colWidths_[i] = w;
    }
    filter_.include = c.Get(s, L"FilterEin", L"*");
    filter_.exclude = c.Get(s, L"FilterAus", L"");
    filter_.applyToDirs = c.GetBool(s, L"FilterVerzeichnisse", false);
    recent_ = c.GetList(s, L"Verlauf");
    SetupColumns();
    ApplyViewStyle();
}

void FilePane::SaveState(Config& c, const std::wstring& s) const {
    c.Set(s, L"Verzeichnis", dir_);
    c.SetInt(s, L"Ansicht", (int)view_);
    c.SetInt(s, L"Sortierung", (int)sortKey_);
    c.SetBool(s, L"Absteigend", sortDesc_);
    // Aktuelle Spaltenbreiten (in 96-DPI-Pixeln)
    std::vector<int> widths = colWidths_;
    HWND header = ListView_GetHeader(list_);
    int n = Header_GetItemCount(header);
    UINT dpi = GetWindowDpi(hwnd_);
    for (int i = 0; i < n; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_SUBITEM | LVCF_WIDTH;
        if (ListView_GetColumn(list_, i, &col) && col.iSubItem >= 0 && col.iSubItem < ColCount)
            widths[col.iSubItem] = MulDiv(col.cx, 96, (int)dpi);
    }
    std::vector<std::wstring> ws;
    for (int w : widths) ws.push_back(std::to_wstring(w));
    c.Set(s, L"Spalten", Join(ws, L","));
    c.Set(s, L"FilterEin", filter_.include);
    c.Set(s, L"FilterAus", filter_.exclude);
    c.SetBool(s, L"FilterVerzeichnisse", filter_.applyToDirs);
    c.SetList(s, L"Verlauf", recent_);
}

// ===================== Nachrichten =====================

LRESULT CALLBACK FilePane::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    FilePane* self;
    if (msg == WM_NCCREATE) {
        self = (FilePane*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (FilePane*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    if (self) return self->Proc(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK FilePane::ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = (FilePane*)ref;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0) {
            if (GetKeyState(VK_SHIFT) < 0) {
                // Umschalt+Eingabe: mit Standardprogramm öffnen
                std::wstring p = self->FocusedPath();
                if (!p.empty()) ShellOpen(GetAncestor(h, GA_ROOT), p, L"", self->dir_);
            } else {
                self->OpenFocused();
            }
            return 0;
        }
        if (wp == VK_INSERT && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_SHIFT) >= 0) {
            self->ToggleSelectFocused(true);
            return 0;
        }
        if (wp == VK_SPACE && GetKeyState(VK_CONTROL) >= 0) {
            // Leertaste: Markierung umschalten, bei Verzeichnissen Größe berechnen
            if (self->FocusedIsDir() && !self->FocusedIsParent()) self->ComputeDirSizes(true);
            self->ToggleSelectFocused(false);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L' ' || wp == L'\r') return 0; // kein Signalton
        break;
    case WM_SETFOCUS:
        if (self->host_) self->host_->OnPaneActivated(self);
        break;
    case WM_NCDESTROY:
        RevokeFileDropTarget(h);
        RemoveWindowSubclass(h, ListSubclass, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK FilePane::PathEditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = (FilePane*)ref;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            std::wstring t = Trim(GetWindowTextStr(h));
            // Umgebungsvariablen erlauben (%USERPROFILE% …)
            wchar_t buf[32768];
            DWORD n = ExpandEnvironmentStringsW(t.c_str(), buf, 32768);
            if (n > 0 && n < 32768) t = buf;
            if (!t.empty() && self->Navigate(t)) self->FocusList();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetWindowTextW(self->pathCombo_, self->dir_.c_str());
            self->FocusList();
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == 27) return 0;
        break;
    case WM_SETFOCUS:
        if (self->host_) self->host_->OnPaneActivated(self);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, PathEditSubclass, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT FilePane::OnNotify(NMHDR* nm) {
    if (nm->hwndFrom == ListView_GetHeader(list_)) {
        if (nm->code == HDN_ENDTRACKW || nm->code == HDN_DIVIDERDBLCLICKW) {
            // Breiten werden beim Speichern ausgelesen
        }
        return 0;
    }
    if (nm->hwndFrom != list_) return 0;
    switch (nm->code) {
    case LVN_GETDISPINFOW:
        OnGetDispInfo((NMLVDISPINFOW*)nm);
        return 0;
    case LVN_ODFINDITEMW: {
        auto* fi = (NMLVFINDITEMW*)nm;
        if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !fi->lvfi.psz) return -1;
        std::wstring needle = fi->lvfi.psz;
        int n = (int)items_.size();
        if (n == 0) return -1;
        int start = fi->iStart;
        if (start < 0 || start >= n) start = 0;
        for (int k = 0; k < n; ++k) {
            int i = (start + k) % n;
            if (items_[i].isParent) continue;
            if (StartsWithI(items_[i].e.name, needle)) return i;
        }
        return -1;
    }
    case LVN_ITEMCHANGED: {
        auto* lv = (NMLISTVIEW*)nm;
        if (lv->uChanged & LVIF_STATE) {
            if ((lv->uNewState ^ lv->uOldState) & LVIS_FOCUSED) {
                if ((lv->uNewState & LVIS_FOCUSED) && host_) host_->OnPaneFocusItemChanged(this);
            }
            if ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED) SetTimer(hwnd_, TIMER_STATUS, 60, nullptr);
        }
        return 0;
    }
    case LVN_ODSTATECHANGED:
        SetTimer(hwnd_, TIMER_STATUS, 60, nullptr);
        return 0;
    case NM_DBLCLK: {
        auto* ia = (NMITEMACTIVATE*)nm;
        if (ia->iItem >= 0) OpenFocused();
        else GoUp(); // Doppelklick auf freie Fläche: eine Ebene höher
        return 0;
    }
    case NM_RETURN:
        return 0; // in der Unterklasse behandelt
    case LVN_COLUMNCLICK: {
        auto* lv = (NMLISTVIEW*)nm;
        int sub = lv->iSubItem;
        SortKey k = (SortKey)((sub >= 0 && sub < (int)colIds_.size()) ? colIds_[sub] : sub);
        SetSort(k, k == sortKey_ ? !sortDesc_ : (k == SortKey::Date || k == SortKey::Size || k == SortKey::Created));
        return 0;
    }
    case LVN_BEGINLABELEDITW: {
        auto* di = (NMLVDISPINFOW*)nm;
        if (di->item.iItem < 0 || di->item.iItem >= (int)items_.size() || items_[di->item.iItem].isParent) return TRUE;
        HWND edit = ListView_GetEditControl(list_);
        if (edit) {
            SendMessageW(edit, EM_LIMITTEXT, 255, 0);
            PostMessageW(hwnd_, WM_APP_RENAMESEL, 0, (LPARAM)edit);
        }
        return FALSE;
    }
    case LVN_ENDLABELEDITW: {
        auto* di = (NMLVDISPINFOW*)nm;
        int i = di->item.iItem;
        if (!di->item.pszText || i < 0 || i >= (int)items_.size()) return FALSE;
        std::wstring newName = Trim(di->item.pszText);
        std::wstring old = items_[i].e.name;
        if (newName.empty() || newName == old) return FALSE;
        if (RenameItem(GetAncestor(hwnd_, GA_ROOT), PathCombine(dir_, old), newName)) {
            items_[i].e.name = newName;
            items_[i].icon = -1;
            SortItems();
            FillList(newName, {newName}, ListView_GetTopIndex(list_));
            if (host_) host_->OnPaneFocusItemChanged(this);
        }
        return FALSE;
    }
    case LVN_BEGINDRAG:
    case LVN_BEGINRDRAG: {
        auto names = SelectedNames();
        if (names.empty()) return 0;
        StartFileDrag(hwnd_, dir_, names);
        Reload();
        if (host_) host_->OnPaneFilesDropped(this);
        return 0;
    }
    case NM_RCLICK: {
        auto* ia = (NMITEMACTIVATE*)nm;
        POINT pt = ia->ptAction;
        ClientToScreen(list_, &pt);
        bool onItem = ia->iItem >= 0 && !(ia->iItem < (int)items_.size() && items_[ia->iItem].isParent);
        if (!onItem) SelectNone();
        if (host_) host_->OnPaneContextMenu(this, pt, onItem);
        return 0;
    }
    case NM_CUSTOMDRAW:
        return OnCustomDraw((NMLVCUSTOMDRAW*)nm);
    case LVN_GETINFOTIPW: {
        auto* ti = (NMLVGETINFOTIPW*)nm;
        if (ti->iItem >= 0 && ti->iItem < (int)items_.size() && !items_[ti->iItem].isParent) {
            const Item& it = items_[ti->iItem];
            std::wstring s = it.e.name + L"\n" + FormatFileTime(it.e.modified, true);
            if (!it.e.IsDir()) s += L"\n" + FormatSizeBytes(it.e.size) + L" Bytes";
            CopyToBuf(ti->pszText, ti->cchTextMax, s);
        }
        return 0;
    }
    case NM_SETFOCUS:
        if (host_) host_->OnPaneActivated(this);
        return 0;
    }
    return 0;
}

LRESULT FilePane::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        Paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const HitRect* h = HitTest(pt);
        if (h != hot_) {
            hot_ = h;
            InvalidateRect(hwnd_, &rcHeader_, FALSE);
            InvalidateRect(hwnd_, &rcPath_, FALSE);
        }
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hot_) {
            hot_ = nullptr;
            InvalidateRect(hwnd_, &rcHeader_, FALSE);
            InvalidateRect(hwnd_, &rcPath_, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (host_) host_->OnPaneActivated(this);
        if (const HitRect* h = HitTest(pt)) {
            HitRect copy = *h;
            OnClickArea(copy);
        } else {
            FocusList();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const HitRect* h = HitTest(pt);
        if (h && h->area == HitArea::Drive) {
            std::wstring root = drives_[h->drive].root;
            ClientToScreen(hwnd_, &pt);
            // Kontextmenü des Laufwerks: übergeordnetes Element ist "Dieser PC"
            bool rename = false;
            ShowShellContextMenu(GetAncestor(hwnd_, GA_ROOT), root, {}, pt, nullptr, &rename);
        }
        return 0;
    }
    case WM_NOTIFY:
        return OnNotify((NMHDR*)lp);
    case WM_CONTEXTMENU:
        // Tastatur (Umschalt+F10 / Kontextmenütaste): Menü am Fokuselement
        if ((HWND)wp == list_ && lp == -1) {
            int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
            POINT pt{ToPx(20), ToPx(20)};
            bool onItem = i >= 0 && i < (int)items_.size() && !items_[i].isParent;
            if (i >= 0) {
                RECT r{};
                ListView_EnsureVisible(list_, i, FALSE);
                if (ListView_GetItemRect(list_, i, &r, LVIR_LABEL)) pt = {r.left + ToPx(8), r.bottom};
            }
            ClientToScreen(list_, &pt);
            if (onItem && ListView_GetSelectedCount(list_) == 0)
                ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
            if (host_) host_->OnPaneContextMenu(this, pt, onItem);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == kIdPath) {
            if (HIWORD(wp) == CBN_SELENDOK) {
                int sel = (int)SendMessageW(pathCombo_, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)recent_.size()) {
                    std::wstring target = recent_[sel];
                    if (Navigate(target)) PostMessageW(hwnd_, WM_APP + 10, 0, 0);
                }
            } else if (HIWORD(wp) == CBN_SETFOCUS) {
                if (host_) host_->OnPaneActivated(this);
            }
        }
        return 0;
    case WM_APP + 10:
        FocusList();
        return 0;
    case WM_KEYDOWN:
        // Vom Anzeige-Steuerelement weitergereicht
        if (wp == VK_ESCAPE) {
            PostMessageW(App::MainWindow(), WM_COMMAND, cmd::QuickView, 0);
            return 0;
        }
        if (wp == VK_TAB) {
            PostMessageW(App::MainWindow(), WM_COMMAND, cmd::NextPane, 0);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == TIMER_REFRESH) {
            KillTimer(hwnd_, TIMER_REFRESH);
            // Nicht während der Bearbeitung eines Namens neu laden
            if (!ListView_GetEditControl(list_)) Reload();
            else SetTimer(hwnd_, TIMER_REFRESH, 500, nullptr);
        } else if (wp == TIMER_STATUS) {
            KillTimer(hwnd_, TIMER_STATUS);
            if (!quickView_) UpdateStatus();
        }
        return 0;
    case WM_APP_DIRCHANGED:
        SetTimer(hwnd_, TIMER_REFRESH, 300, nullptr);
        return 0;
    case WM_APP_THUMB:
        OnThumbReady(wp, lp);
        return 0;
    case WM_APP_DIRSIZE: {
        auto* r = (DirSizeResult*)lp;
        if (r->gen == generation_) {
            int i = FindItem(r->name);
            if (i >= 0) {
                items_[i].dirSize = r->size;
                ListView_RedrawItems(list_, i, i);
            }
        }
        delete r;
        return 0;
    }
    case WM_APP_DIRSIZE_DONE:
        if ((unsigned)wp == generation_) {
            if (sortKey_ == SortKey::Size) SetSort(sortKey_, sortDesc_);
            UpdateStatus();
        }
        return 0;
    case WM_APP_RENAMESEL: {
        HWND edit = (HWND)lp;
        if (IsWindow(edit)) {
            std::wstring t = GetWindowTextStr(edit);
            size_t dot = t.find_last_of(L'.');
            int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
            bool isDir = i >= 0 && i < (int)items_.size() && items_[i].e.IsDir();
            if (dot != std::wstring::npos && dot > 0 && !isDir) SendMessageW(edit, EM_SETSEL, 0, dot);
            else SendMessageW(edit, EM_SETSEL, 0, -1);
        }
        return 0;
    }
    case WM_DESTROY:
        StopWatch();
        StopThumbWorker();
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace qf
