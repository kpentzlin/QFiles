#include "BookmarkList.h"
#include "App.h"
#include "Location.h"
#include "Remote.h"
#include "ShellMenu.h"
#include "Util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <algorithm>

namespace qf {

namespace {
enum : int { MenuOpen = 1, MenuOpenOther, MenuRename, MenuChangePath, MenuUp, MenuDown, MenuDelete, MenuAddCurrent, MenuNewFtp };

int StockIndex(SHSTOCKICONID id) {
    SHSTOCKICONINFO sii{sizeof(sii)};
    return SUCCEEDED(SHGetStockIconInfo(id, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &sii)) ? sii.iSysImageIndex : 0;
}

// Hintergrundfarben der Lesezeichenliste
const COLORREF kNetworkBack = RGB(255, 214, 214);  // hellrot: Netzwerkpfade
const COLORREF kRemoteBack = RGB(208, 228, 255);   // hellblau: FTP/SFTP
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
    SendMessageW(list_, WM_SETFONT, (WPARAM)App::UIFont(), TRUE);
    LVCOLUMNW c{};
    c.mask = LVCF_WIDTH;
    c.cx = 200;
    ListView_InsertColumn(list_, 0, &c);
    SetWindowSubclass(list_, Subclass, 1, (DWORD_PTR)this);
    RegisterFileDropTarget(
        list_,
        [this](POINT pt) -> std::wstring {
            LVHITTESTINFO ht{};
            ht.pt = pt;
            ScreenToClient(list_, &ht.pt);
            int i = ListView_HitTest(list_, &ht);
            if (i >= 0 && i < (int)items_.size() && !IsVirtualLocation(items_[i].path)) return items_[i].path;
            return L"";
        },
        []() { App::RefreshPanes(); });
    return true;
}

void BookmarkList::Resize() {
    RECT rc;
    GetClientRect(list_, &rc);
    ListView_SetColumnWidth(list_, 0, rc.right);
}

void BookmarkList::Set(const std::vector<Bookmark>& b) {
    items_ = b;
    Rebuild(-1);
}

void BookmarkList::Rebuild(int select) {
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    int folder = StockIndex(SIID_FOLDER), drive = StockIndex(SIID_DRIVEFIXED);
    kinds_.assign(items_.size(), 0);
    for (size_t i = 0; i < items_.size(); ++i) {
        const std::wstring& p = items_[i].path;
        int icon = folder;
        if (IsRemoteUrl(p)) {
            kinds_[i] = 2;
            icon = StockIndex(SIID_WORLD);
        } else if (IsNetworkPath(p)) {
            kinds_[i] = 1;
            icon = IsNetworkRoot(p) ? StockIndex(SIID_MYNETWORK)
                   : IsNetworkServer(p) ? StockIndex(SIID_SERVER)
                   : IsUncShareRoot(p) ? StockIndex(SIID_SERVERSHARE)
                   : (IsRootPath(p) ? StockIndex(SIID_DRIVENET) : folder);
        } else if (IsRootPath(p)) {
            icon = drive;
        }
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t*>(items_[i].name.c_str());
        it.iImage = icon;
        ListView_InsertItem(list_, &it);
    }
    if (select >= 0 && select < (int)items_.size()) {
        ListView_SetItemState(list_, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, select, FALSE);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    Resize();
}

int BookmarkList::Selected() const { return ListView_GetNextItem(list_, -1, LVNI_SELECTED); }

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
    ListView_EditLabel(list_, i);
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
    HMENU m = CreatePopupMenu();
    UINT has = i >= 0 ? MF_STRING : (MF_STRING | MF_GRAYED);
    AppendMenuW(m, has, MenuOpen, L"In aktiver Liste &öffnen\tEingabe");
    AppendMenuW(m, has, MenuOpenOther, L"In &anderer Liste öffnen\tUmschalt+Eingabe");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, has, MenuRename, L"&Umbenennen\tF2");
    AppendMenuW(m, has, MenuChangePath, L"&Pfad ändern…");
    AppendMenuW(m, (i > 0) ? MF_STRING : MF_GRAYED, MenuUp, L"Nach &oben\tAlt+↑");
    AppendMenuW(m, (i >= 0 && i + 1 < (int)items_.size()) ? MF_STRING : MF_GRAYED, MenuDown, L"Nach u&nten\tAlt+↓");
    AppendMenuW(m, has, MenuDelete, L"&Entfernen\tEntf");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MenuAddCurrent, L"Aktuelles Verzeichnis &hinzufügen\tStrg+D");
    AppendMenuW(m, MF_STRING, MenuNewFtp, L"Neuer &FTP/sFTP-Zugriff…");
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, GetAncestor(list_, GA_ROOT), nullptr);
    DestroyMenu(m);
    switch (id) {
    case MenuOpen:
        if (i >= 0 && cb_.navigate) cb_.navigate(items_[i].path, false);
        break;
    case MenuOpenOther:
        if (i >= 0 && cb_.navigate) cb_.navigate(items_[i].path, true);
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
    }
}

LRESULT BookmarkList::OnNotify(NMHDR* nm) {
    switch (nm->code) {
    case LVN_ITEMACTIVATE: {
        auto* ia = (NMITEMACTIVATE*)nm;
        int i = ia->iItem >= 0 ? ia->iItem : Selected();
        if (i >= 0 && i < (int)items_.size() && cb_.navigate) {
            bool other = (ia->uKeyFlags & LVKF_SHIFT) != 0;
            cb_.navigate(items_[i].path, other);
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
        if (ti->iItem >= 0 && ti->iItem < (int)items_.size()) {
            std::wstring s = LocationDisplay(items_[ti->iItem].path);
            // Netzwerk und FTP nicht prüfen (nicht erreichbare Server würden die Oberfläche blockieren)
            if (kinds_.size() > (size_t)ti->iItem && kinds_[ti->iItem] == 2)
                s += StartsWithI(s, L"sftp://") ? L"\nSFTP-Zugang" : L"\nFTP-Zugang (unverschlüsselt)";
            else if (!(kinds_.size() > (size_t)ti->iItem && kinds_[ti->iItem] == 1) && !DirExists(s))
                s += L"\n(nicht erreichbar)";
            wcsncpy_s(ti->pszText, ti->cchTextMax, s.c_str(), _TRUNCATE);
        }
        return 0;
    }
    case LVN_BEGINLABELEDITW:
        return FALSE;
    case LVN_ENDLABELEDITW: {
        auto* di = (NMLVDISPINFOW*)nm;
        if (di->item.pszText && di->item.iItem >= 0 && di->item.iItem < (int)items_.size()) {
            std::wstring n = Trim(di->item.pszText);
            if (!n.empty()) {
                items_[di->item.iItem].name = n;
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
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            size_t i = (size_t)cd->nmcd.dwItemSpec;
            if (i < kinds_.size() && kinds_[i]) {
                cd->clrTextBk = kinds_[i] == 2 ? kRemoteBack : kNetworkBack;
                cd->clrText = RGB(0, 0, 0);
                return CDRF_NEWFONT;
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
            int i = self->Selected();
            if (i >= 0 && self->cb_.navigate) self->cb_.navigate(self->items_[i].path, GetKeyState(VK_SHIFT) < 0);
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
