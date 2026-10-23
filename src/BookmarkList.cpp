#include "BookmarkList.h"
#include "App.h"
#include "Cloud.h"
#include "Favorites.h"
#include "Glyphs.h"
#include "Location.h"
#include "Network.h"
#include "Remote.h"
#include "ShellMenu.h"
#include "Util.h"

#include <shellapi.h>
#include <windowsx.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <algorithm>

namespace qf {

namespace {
enum : int { MenuOpen = 1, MenuOpenOther, MenuRename, MenuChangePath, MenuUp, MenuDown, MenuDelete, MenuAddCurrent, MenuNewFtp, MenuSort };

int StockIndex(SHSTOCKICONID id) {
    SHSTOCKICONINFO sii{sizeof(sii)};
    return SUCCEEDED(SHGetStockIconInfo(id, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &sii)) ? sii.iSysImageIndex : 0;
}

// Hintergrundfarben der Lesezeichenliste
// (deutlich heller als die blaue Kopfzeile der aktiven Liste)
const COLORREF kNetworkBack = RGB(255, 228, 228);   // hellrot: Netzwerkpfade
const COLORREF kRemoteBack = RGB(220, 244, 220);    // hellgrün: FTP/SFTP
const COLORREF kWebDavBack = RGB(234, 250, 234);    // noch heller grün: WebDAV
const COLORREF kStandardBack = RGB(228, 240, 253);  // hellblau: Windows-Standardverzeichnisse
const COLORREF kCloudBack = RGB(226, 210, 248);     // hell-lila: Dropbox, Google Drive, OneDrive

enum Kind { KindLocal = 0, KindNetwork = 1, KindRemote = 2, KindStandard = 3, KindWebDav = 4, KindCloud = 5 };

// Windows-Standardverzeichnisse (Benutzerordner, öffentliche Ordner, Systemordner)
const std::vector<std::wstring>& StandardFolders() {
    static std::vector<std::wstring> list;
    static bool done = false;
    if (done) return list;
    done = true;
    const GUID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads, &FOLDERID_Pictures,
                         &FOLDERID_Music, &FOLDERID_Videos, &FOLDERID_Profile, &FOLDERID_Favorites, &FOLDERID_Links,
                         &FOLDERID_SavedGames, &FOLDERID_SavedSearches, &FOLDERID_Contacts,
                         &FOLDERID_CameraRoll, &FOLDERID_Screenshots, &FOLDERID_Public, &FOLDERID_PublicDesktop,
                         &FOLDERID_PublicDocuments, &FOLDERID_PublicDownloads, &FOLDERID_PublicPictures,
                         &FOLDERID_PublicMusic, &FOLDERID_PublicVideos, &FOLDERID_RoamingAppData,
                         &FOLDERID_LocalAppData, &FOLDERID_ProgramData, &FOLDERID_ProgramFiles,
                         &FOLDERID_ProgramFilesX86, &FOLDERID_StartMenu, &FOLDERID_Startup, &FOLDERID_SendTo,
                         &FOLDERID_Windows, &FOLDERID_System, &FOLDERID_Templates, &FOLDERID_UserProfiles};
    for (auto id : ids) {
        std::wstring p = GetKnownFolder(*id);
        if (!p.empty()) list.push_back(ToLower(NormalizeDir(p)));
    }
    std::wstring t = GetTempDir();
    if (!t.empty()) list.push_back(ToLower(NormalizeDir(t)));
    return list;
}

int KindOf(const std::wstring& p) {
    if (IsWebDavUrl(p)) return KindWebDav;
    if (IsRemoteUrl(p)) return KindRemote;
    if (IsNetworkPath(p)) return KindNetwork;
    std::wstring n = ToLower(NormalizeDir(p));
    for (auto& f : StandardFolders())
        if (f == n) return KindStandard;
    if (IsCloudPath(p)) return KindCloud;
    return KindLocal;
}

COLORREF KindColor(int k) {
    switch (k) {
    case KindNetwork: return kNetworkBack;
    case KindRemote: return kRemoteBack;
    case KindWebDav: return kWebDavBack;
    case KindStandard: return kStandardBack;
    case KindCloud: return kCloudBack;
    }
    return CLR_DEFAULT;
}
}

bool BookmarkList::Create(HWND parent, int id, Callbacks cb) {
    cb_ = std::move(cb);
    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS | LVS_EDITLABELS | LVS_SHAREIMAGELISTS,
                            0, 0, 100, 100, parent, (HMENU)(INT_PTR)id, App::Instance(), nullptr);
    if (!list_) return false;
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_DOUBLEBUFFER | LVS_EX_ONECLICKACTIVATE | LVS_EX_UNDERLINEHOT);
    SHFILEINFOW sfi{};
    HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    ListView_SetImageList(list_, il, LVSIL_SMALL);
    ApplyFont();
    LVCOLUMNW c{};
    c.mask = LVCF_WIDTH;
    c.cx = 200;
    ListView_InsertColumn(list_, 0, &c);   // Name (mit Symbol)
    c.cx = IconColumnWidth();
    ListView_InsertColumn(list_, 1, &c);   // schmale Symbolspalte: Klick öffnet in der anderen Liste
    int order[2] = {1, 0};                 // Symbolspalte links anzeigen
    ListView_SetColumnOrderArray(list_, 2, order);
    SetWindowSubclass(list_, Subclass, 1, (DWORD_PTR)this);
    // Linux (WSL): Eintrag „Linux“ unter „Netzwerk“, Symbol wie im Explorer (Shell-Ordner „Linux“)
    wsl_ = WslDistributions();
    if (!wsl_.empty()) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        SHFILEINFOW sfiL{};
        if (SUCCEEDED(SHParseDisplayName(L"::{B2B4A4D1-2754-4140-A2EB-9A76D9D7CDC6}", nullptr, &pidl, 0, nullptr))) {
            if (SHGetFileInfoW((LPCWSTR)pidl, 0, &sfiL, sizeof(sfiL), SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                linuxIcon_ = sfiL.iIcon;
            CoTaskMemFree(pidl);
        }
        if (linuxIcon_ < 0) linuxIcon_ = StockIndex(SIID_SERVER);
    }
    RegisterFileDropTarget(
        list_,
        [this](POINT pt) -> std::wstring {
            LVHITTESTINFO ht{};
            ht.pt = pt;
            ScreenToClient(list_, &ht.pt);
            int i = ItemOfRow(ListView_HitTest(list_, &ht));
            if (i >= 0 && i < (int)items_.size() && !IsVirtualLocation(items_[i].path)) return items_[i].path;
            return L"";
        },
        []() { App::RefreshPanes(); });
    return true;
}

void BookmarkList::ApplyFont() {
    if (!list_) return;
    const Options& o = App::Opt();
    HFONT f = nullptr;
    if (!o.bookmarkFontName.empty()) {
        int h = -MulDiv(o.bookmarkFontSize > 0 ? o.bookmarkFontSize : 9, (int)GetWindowDpi(list_), 72);
        f = CreateFontW(h, 0, 0, 0, o.bookmarkFontWeight, o.bookmarkFontItalic, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, o.bookmarkFontName.c_str());
    }
    SendMessageW(list_, WM_SETFONT, (WPARAM)(f ? f : App::UIFont()), TRUE);
    if (font_) DeleteObject(font_);
    font_ = f;
    // Zeilenhöhe neu berechnen lassen (die ListView übernimmt sie erst beim Neuaufbau der Einträge)
    Rebuild(Selected());
}

int BookmarkList::IconColumnWidth() const { return DpiScale(list_, 22); }

void BookmarkList::Resize() {
    RECT rc;
    GetClientRect(list_, &rc);
    int iw = IconColumnWidth();
    ListView_SetColumnWidth(list_, 1, iw);
    ListView_SetColumnWidth(list_, 0, std::max<int>(0, rc.right - iw));
}

// Liegt der Punkt (Bildschirmkoordinaten) in der Symbolspalte? Liefert den Eintrag oder -1.
int BookmarkList::IconColumnItemAt(POINT screenPt) const {
    LVHITTESTINFO ht{};
    ht.pt = screenPt;
    ScreenToClient(list_, &ht.pt);
    int i = ListView_SubItemHitTest(list_, &ht);   // Zeile
    return (i >= 0 && i < ListView_GetItemCount(list_) && ht.iSubItem == 1) ? i : -1;
}

void BookmarkList::Set(const std::vector<Bookmark>& b) {
    items_ = b;
    Rebuild(-1);
}

void BookmarkList::SetFavoritesCount(size_t count) {
    favCount_ = count;
    bool show = count > 0;
    if (show == favRow_) {
        InvalidateRect(list_, nullptr, FALSE);
        return;
    }
    bool favSel = IsFavRow(SelectedRow());
    int sel = Selected();
    favRow_ = show;
    Rebuild(sel);
    if (favSel && favRow_) ListView_SetItemState(list_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void BookmarkList::Rebuild(int select) {
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    int folder = StockIndex(SIID_FOLDER), drive = StockIndex(SIID_DRIVEFIXED);
    // Zeilenfolge: „Favoriten“ (falls vorhanden), Lesezeichen; „Linux“ direkt unter „Netzwerk“ (ohne „Netzwerk“ oben)
    rows_.clear();
    if (favRow_) rows_.push_back(kRowFavorites);
    bool linuxDone = wsl_.empty();
    bool hasNetwork = false;
    for (auto& b : items_)
        if (IsNetworkRoot(b.path)) hasNetwork = true;
    if (!linuxDone && !hasNetwork) {
        rows_.push_back(kRowLinux);
        linuxDone = true;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        rows_.push_back((int)i);
        if (!linuxDone && IsNetworkRoot(items_[i].path)) {
            rows_.push_back(kRowLinux);
            linuxDone = true;
        }
    }
    kinds_.assign(items_.size(), 0);
    icons_.assign(items_.size(), folder);
    for (size_t i = 0; i < items_.size(); ++i) {
        const std::wstring& p = items_[i].path;
        int icon = folder;
        kinds_[i] = KindOf(p);
        if (kinds_[i] == KindRemote || kinds_[i] == KindWebDav) {
            icon = StockIndex(SIID_WORLD);
        } else if (kinds_[i] == KindNetwork) {
            icon = IsNetworkRoot(p) ? StockIndex(SIID_MYNETWORK)
                   : IsNetworkServer(p) ? StockIndex(SIID_SERVER)
                   : IsUncShareRoot(p) ? StockIndex(SIID_SERVERSHARE)
                   : (IsRootPath(p) ? StockIndex(SIID_DRIVENET) : folder);
        } else if (IsRootPath(p)) {
            icon = drive;
        }
        icons_[i] = icon;
    }
    for (size_t row = 0; row < rows_.size(); ++row) {
        int k = rows_[row];
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = (int)row;
        // „Favoriten“: Symbol und Hintergrund zeichnet NM_CUSTOMDRAW (Ordnersymbol nur für die Zeilengeometrie)
        it.pszText = const_cast<wchar_t*>(k == kRowFavorites ? kFavoritesLocation : k == kRowLinux ? L"Linux" : items_[k].name.c_str());
        it.iImage = k == kRowFavorites ? folder : k == kRowLinux ? linuxIcon_ : icons_[k];
        ListView_InsertItem(list_, &it);
    }
    if (select >= 0 && select < (int)items_.size()) {
        int row = RowOfItem(select);
        ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, row, FALSE);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    Resize();
}

int BookmarkList::RowOfItem(int item) const {
    for (size_t r = 0; r < rows_.size(); ++r)
        if (rows_[r] == item) return (int)r;
    return -1;
}

std::wstring BookmarkList::RowPath(int row) const {
    int k = ItemOfRow(row);
    if (k == kRowFavorites) return kFavoritesLocation;
    if (k == kRowLinux) return kWslRoot;
    return k >= 0 && k < (int)items_.size() ? items_[k].path : L"";
}

int BookmarkList::SelectedRow() const { return ListView_GetNextItem(list_, -1, LVNI_SELECTED); }

int BookmarkList::Selected() const {
    int i = ItemOfRow(SelectedRow());
    return i >= 0 && i < (int)items_.size() ? i : -1;
}

int BookmarkList::FindPath(const std::wstring& path) const {
    auto norm = [](const std::wstring& p) {
        std::wstring s = NormalizeSpecialLocation(p);
        return s.empty() ? NormalizeDir(p) : s;
    };
    std::wstring target = norm(path);
    for (size_t i = 0; i < items_.size(); ++i)
        if (EqualsI(norm(items_[i].path), target)) return (int)i;
    return -1;
}

void BookmarkList::Append(const std::wstring& name, const std::wstring& path) {
    items_.push_back({name, path});
    Rebuild((int)items_.size() - 1);
    if (cb_.changed) cb_.changed();
}

void BookmarkList::SortByCategory() {
    // Reihenfolge: „Netzwerk“, Standardverzeichnisse (blau), Cloud (lila), FTP/SFTP/WebDAV (grün),
    // übrige Netzwerkpfade (rot), lokal; innerhalb jeder Gruppe alphabetisch nach Namen
    auto rank = [](const Bookmark& b) {
        if (IsNetworkRoot(b.path)) return 0;
        switch (KindOf(b.path)) {
        case KindStandard: return 1;
        case KindCloud: return 2;
        case KindRemote:
        case KindWebDav: return 3;
        case KindNetwork: return 4;
        default: return 5;
        }
    };
    int sel = Selected();
    std::wstring selPath = sel >= 0 ? items_[sel].path : L"";
    std::stable_sort(items_.begin(), items_.end(), [&](const Bookmark& a, const Bookmark& b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int c = CompareNatural(a.name, b.name);
        if (c != 0) return c < 0;
        return CompareI(a.path, b.path) < 0;
    });
    int newSel = -1;
    for (size_t i = 0; i < items_.size(); ++i)
        if (!selPath.empty() && items_[i].path == selPath) newSel = (int)i;
    Rebuild(newSel);
    if (cb_.changed) cb_.changed();
}

void BookmarkList::DeleteSelected() {
    int i = Selected();
    if (i < 0) return;
    if (!MsgConfirm(GetAncestor(list_, GA_ROOT), L"Lesezeichen „" + items_[i].name + L"“ entfernen?")) return;
    items_.erase(items_.begin() + i);
    Rebuild(std::min(i, (int)items_.size() - 1));
    if (cb_.changed) cb_.changed();
}

void BookmarkList::RenameSelected() {
    int i = Selected();
    if (i < 0) return;
    SetFocus(list_);
    ListView_EditLabel(list_, RowOfItem(i));
}

void BookmarkList::MoveSelected(int delta) {
    int i = Selected();
    int j = i + delta;
    if (i < 0 || j < 0 || j >= (int)items_.size()) return;
    std::swap(items_[i], items_[j]);
    Rebuild(j);
    if (cb_.changed) cb_.changed();
}

void BookmarkList::ContextMenu(POINT pt) {
    int i = Selected();
    bool fav = IsSpecialRow(SelectedRow());   // „Favoriten“ bzw. „Linux“
    std::wstring specialPath = RowPath(SelectedRow());
    HMENU m = CreatePopupMenu();
    UINT has = i >= 0 ? MF_STRING : (MF_STRING | MF_GRAYED);
    UINT canOpen = (i >= 0 || fav) ? MF_STRING : (MF_STRING | MF_GRAYED);
    AppendMenuW(m, canOpen, MenuOpen, L"In aktiver Liste &öffnen\tEingabe");
    AppendMenuW(m, canOpen, MenuOpenOther, L"In &anderer Liste öffnen\tUmschalt+Eingabe");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, has, MenuRename, L"&Umbenennen\tF2");
    AppendMenuW(m, has, MenuChangePath, L"&Pfad ändern…");
    AppendMenuW(m, (i > 0) ? MF_STRING : MF_GRAYED, MenuUp, L"Nach &oben\tAlt+↑");
    AppendMenuW(m, (i >= 0 && i + 1 < (int)items_.size()) ? MF_STRING : MF_GRAYED, MenuDown, L"Nach u&nten\tAlt+↓");
    AppendMenuW(m, has, MenuDelete, L"&Entfernen\tEntf");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MenuAddCurrent, L"Aktuelles Verzeichnis &hinzufügen\tStrg+D");
    AppendMenuW(m, MF_STRING, MenuNewFtp, L"Neuer &FTP/sFTP/WebDAV-Zugriff…");
    AppendMenuW(m, items_.size() > 1 ? MF_STRING : MF_GRAYED, MenuSort, L"&Sortieren…");
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, GetAncestor(list_, GA_ROOT), nullptr);
    DestroyMenu(m);
    switch (id) {
    case MenuOpen:
    case MenuOpenOther:
        if (cb_.navigate && (i >= 0 || fav)) cb_.navigate(fav ? specialPath : items_[i].path, id == MenuOpenOther);
        break;
    case MenuRename: RenameSelected(); break;
    case MenuChangePath: {
        if (i < 0) break;
        if (IsRemoteUrl(items_[i].path)) {
            // FTP/SFTP: Zugangsdialog
            RemoteAccess a = LoadRemoteAccess(items_[i].path);
            a.name = items_[i].name;
            if (EditRemoteAccess(GetAncestor(list_, GA_ROOT), a, false)) {
                items_[i].name = a.name;
                items_[i].path = a.url.ToString();
                Rebuild(i);
                if (cb_.changed) cb_.changed();
            }
            break;
        }
        std::wstring start = IsVirtualLocation(items_[i].path) ? L"" : items_[i].path;
        std::wstring p = BrowseForFolder(GetAncestor(list_, GA_ROOT), L"Neuer Pfad für „" + items_[i].name + L"“", start);
        if (!p.empty()) {
            items_[i].path = p;
            Rebuild(i);
            if (cb_.changed) cb_.changed();
        }
        break;
    }
    case MenuUp: MoveSelected(-1); break;
    case MenuDown: MoveSelected(1); break;
    case MenuDelete: DeleteSelected(); break;
    case MenuAddCurrent: PostMessageW(App::MainWindow(), WM_COMMAND, 40400 /* cmd::BookmarkAdd */, 0); break;
    case MenuNewFtp: PostMessageW(App::MainWindow(), WM_COMMAND, 40401 /* cmd::BookmarkNewRemote */, 0); break;
    case MenuSort: PostMessageW(App::MainWindow(), WM_COMMAND, 40402 /* cmd::BookmarkSort */, 0); break;
    }
}

// Zeile „Favoriten“: Symbolspalte grau mit dunkelrotem Asterisk, Name hellgelb mit Asterisk als Symbol
void BookmarkList::DrawFavoritesRow(NMLVCUSTOMDRAW* cd) {
    HDC dc = cd->nmcd.hdc;
    const COLORREF star = RGB(150, 0, 0);
    RECT row{}, icon{}, label{}, col{};
    ListView_GetItemRect(list_, 0, &row, LVIR_BOUNDS);
    ListView_GetItemRect(list_, 0, &icon, LVIR_ICON);
    ListView_GetItemRect(list_, 0, &label, LVIR_LABEL);
    ListView_GetSubItemRect(list_, 0, 1, LVIR_BOUNDS, &col);
    FillRect(dc, &col, GetSysColorBrush(COLOR_BTNFACE));
    DrawAsterisk(dc, col, star, GetSysColor(COLOR_BTNFACE));
    RECT name{std::max(col.right, row.left), row.top, row.right, row.bottom};
    if (col.left > row.left && col.right >= row.right) name = {row.left, row.top, col.left, row.bottom};
    bool selected = (ListView_GetItemState(list_, 0, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    bool focus = GetFocus() == list_;
    COLORREF back = kFavoritesBack;
    if (selected) back = focus ? RGB(204, 232, 255) : RGB(217, 217, 217);
    // wie die übrigen farbigen Lesezeichen: Hintergrundfarbe hinter dem Namen, Symbol auf Fensterhintergrund
    RECT fill = name;
    if (!selected) {
        RECT iconArea{name.left, name.top, std::max(name.left, label.left), name.bottom};
        FillRect(dc, &iconArea, GetSysColorBrush(COLOR_WINDOW));
        fill.left = std::max(name.left, label.left);
    }
    HBRUSH b = CreateSolidBrush(back);
    FillRect(dc, &fill, b);
    DeleteObject(b);
    if (selected && focus) {
        HBRUSH fr = CreateSolidBrush(RGB(153, 209, 255));
        FrameRect(dc, &name, fr);
        DeleteObject(fr);
    }
    if (icon.right > icon.left) DrawAsterisk(dc, icon, star, selected ? back : GetSysColor(COLOR_WINDOW));
    HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(list_, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    RECT tr = label;
    tr.left += DpiScale(list_, 2);
    DrawTextW(dc, kFavoritesLocation, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

LRESULT BookmarkList::OnNotify(NMHDR* nm) {
    switch (nm->code) {
    case LVN_ITEMACTIVATE: {
        auto* ia = (NMITEMACTIVATE*)nm;
        int row = ia->iItem >= 0 ? ia->iItem : SelectedRow();
        std::wstring target = RowPath(row);
        if (!target.empty() && cb_.navigate) {
            bool other = (ia->uKeyFlags & LVKF_SHIFT) != 0;
            // Klick auf das Symbol in der linken Spalte: in der anderen Liste öffnen (nicht bei der Eingabetaste)
            if (GetKeyState(VK_RETURN) >= 0) {
                DWORD mp = GetMessagePos();
                if (IconColumnItemAt(POINT{GET_X_LPARAM(mp), GET_Y_LPARAM(mp)}) == row) other = true;
            }
            cb_.navigate(target, other);
        }
        return 0;
    }
    case NM_RCLICK: {
        POINT pt;
        GetCursorPos(&pt);
        ContextMenu(pt);
        return TRUE;
    }
    case LVN_GETINFOTIPW: {
        auto* ti = (NMLVGETINFOTIPW*)nm;
        int k = ItemOfRow(ti->iItem);
        if (IsFavRow(ti->iItem)) {
            std::wstring s = L"Favoriten: " + std::to_wstring(favCount_) + (favCount_ == 1 ? L" Datei" : L" Dateien") +
                             L"\n(Kontextmenü einer Datei: „Als Favorit kennzeichnen“)";
            wcsncpy_s(ti->pszText, ti->cchTextMax, s.c_str(), _TRUNCATE);
        } else if (IsLinuxRow(ti->iItem)) {
            std::wstring s = L"Linux (WSL): " + Join(wsl_, L", ") + L"\n" + kWslRoot;
            wcsncpy_s(ti->pszText, ti->cchTextMax, s.c_str(), _TRUNCATE);
        } else if (k >= 0 && k < (int)items_.size()) {
            std::wstring s = LocationDisplay(items_[k].path);
            // Netzwerk und FTP nicht prüfen (nicht erreichbare Server würden die Oberfläche blockieren)
            if (kinds_.size() > (size_t)k && (kinds_[k] == KindRemote || kinds_[k] == KindWebDav))
            {
                RemoteUrl u;
                if (ParseRemoteUrl(s, u)) s += L"\nZugang: " + std::wstring(RemoteProtoName(u.proto));
            }
            else if (!(kinds_.size() > (size_t)k && kinds_[k] == KindNetwork) && !DirExists(s))
                s += L"\n(nicht erreichbar)";
            wcsncpy_s(ti->pszText, ti->cchTextMax, s.c_str(), _TRUNCATE);
        }
        return 0;
    }
    case LVN_BEGINLABELEDITW:
        return IsSpecialRow(((NMLVDISPINFOW*)nm)->item.iItem) ? TRUE : FALSE;   // „Favoriten“/„Linux“ nicht umbenennen
    case LVN_ENDLABELEDITW: {
        auto* di = (NMLVDISPINFOW*)nm;
        int k = ItemOfRow(di->item.iItem);
        if (di->item.pszText && k >= 0 && k < (int)items_.size()) {
            std::wstring n = Trim(di->item.pszText);
            if (!n.empty()) {
                items_[k].name = n;
                if (cb_.changed) cb_.changed();
                return TRUE;
            }
        }
        return FALSE;
    }
    case LVN_KEYDOWN: {
        auto* kd = (NMLVKEYDOWN*)nm;
        if (kd->wVKey == VK_DELETE) DeleteSelected();
        else if (kd->wVKey == VK_F2) RenameSelected();
        return 0;
    }
    case NM_SETFOCUS:
        if (cb_.activated) cb_.activated();
        return 0;
    case NM_CUSTOMDRAW: {
        auto* cd = (NMLVCUSTOMDRAW*)nm;
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPREPAINT:
            if (IsFavRow((int)cd->nmcd.dwItemSpec)) {
                DrawFavoritesRow(cd);
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            int row = (int)cd->nmcd.dwItemSpec;
            int k = ItemOfRow(row);
            size_t i = k >= 0 ? (size_t)k : SIZE_MAX;
            if (cd->iSubItem == 1) {
                // Symbolspalte: hellgrau wie Kopf- und Fußleisten, Symbol des Lesezeichens mittig
                RECT rc{};
                ListView_GetSubItemRect(list_, row, 1, LVIR_BOUNDS, &rc);
                FillRect(cd->nmcd.hdc, &rc, GetSysColorBrush(COLOR_BTNFACE));
                HIMAGELIST il = ListView_GetImageList(list_, LVSIL_SMALL);
                int sz = DpiScale(list_, 16);
                int iconIndex = k == kRowLinux ? linuxIcon_ : (i < icons_.size() ? icons_[i] : -1);
                if (il && iconIndex >= 0)
                    ImageList_DrawEx(il, iconIndex, cd->nmcd.hdc, (rc.left + rc.right - sz) / 2, (rc.top + rc.bottom - sz) / 2,
                                     sz, sz, CLR_NONE, CLR_NONE, ILD_TRANSPARENT);
                return CDRF_SKIPDEFAULT;
            }
            if (k == kRowLinux) {   // „Linux“ wie „Netzwerk“
                cd->clrTextBk = kNetworkBack;
                cd->clrText = RGB(0, 0, 0);
                return CDRF_NEWFONT;
            }
            if (i < kinds_.size() && kinds_[i]) {
                cd->clrTextBk = KindColor(kinds_[i]);
                cd->clrText = RGB(0, 0, 0);
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        case CDDS_POSTPAINT: {
            // Symbolspalte auch unterhalb der Einträge grau
            RECT client{};
            GetClientRect(list_, &client);
            int top = 0;
            int n = ListView_GetItemCount(list_);
            if (n > 0) {
                RECT last{};
                if (ListView_GetItemRect(list_, n - 1, &last, LVIR_BOUNDS)) top = std::max<int>(0, last.bottom);
            }
            RECT strip{0, top, IconColumnWidth(), client.bottom};
            if (strip.top < strip.bottom) FillRect(cd->nmcd.hdc, &strip, GetSysColorBrush(COLOR_BTNFACE));
            return CDRF_DODEFAULT;
        }
        }
        return CDRF_DODEFAULT;
    }
    }
    return 0;
}

LRESULT CALLBACK BookmarkList::Subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = (BookmarkList*)ref;
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (wp == VK_UP) {
            self->MoveSelected(-1);
            return 0;
        }
        if (wp == VK_DOWN) {
            self->MoveSelected(1);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            std::wstring target = self->RowPath(self->SelectedRow());
            if (!target.empty() && self->cb_.navigate) self->cb_.navigate(target, GetKeyState(VK_SHIFT) < 0);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\r') return 0;
        break;
    case WM_SIZE: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        self->Resize();
        return r;
    }
    case WM_NCDESTROY:
        RevokeFileDropTarget(h);
        RemoveWindowSubclass(h, Subclass, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

} // namespace qf
