#include "BookmarkList.h"
#include "App.h"
#include "ShellMenu.h"
#include "Util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

namespace qf {

namespace {
enum : int { MenuOpen = 1, MenuOpenOther, MenuRename, MenuChangePath, MenuUp, MenuDown, MenuDelete, MenuAddCurrent };
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
            if (i >= 0 && i < (int)items_.size()) return items_[i].path;
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
    SHSTOCKICONINFO sii{sizeof(sii)};
    int folder = 0;
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &sii))) folder = sii.iSysImageIndex;
    int drive = folder;
    if (SUCCEEDED(SHGetStockIconInfo(SIID_DRIVEFIXED, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &sii))) drive = sii.iSysImageIndex;
    for (size_t i = 0; i < items_.size(); ++i) {
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t*>(items_[i].name.c_str());
        it.iImage = IsRootPath(items_[i].path) ? drive : folder;
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
    for (size_t i = 0; i < items_.size(); ++i)
        if (EqualsI(NormalizeDir(items_[i].path), NormalizeDir(path))) return (int)i;
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
        std::wstring p = BrowseForFolder(GetAncestor(list_, GA_ROOT), L"Neuer Pfad für „" + items_[i].name + L"“", items_[i].path);
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
            std::wstring s = items_[ti->iItem].path;
            if (!DirExists(s)) s += L"\n(nicht erreichbar)";
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
