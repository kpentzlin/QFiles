#include "DirTree.h"
#include "App.h"
#include "Location.h"
#include "ShellMenu.h"
#include "Util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <algorithm>

namespace qf {

namespace {
enum : UINT { WM_TREE_SCANITEM = WM_APP + 30, WM_TREE_SCANDONE = WM_APP + 31 };
struct TreeScanItem {
    HTREEITEM parent;
    unsigned gen;
    NetworkItem item;
};
struct TreeScanDone {
    HTREEITEM parent;
    unsigned gen;
    bool ok;
    DWORD error;
};
int StockIconIndex(SHSTOCKICONID id) {
    SHSTOCKICONINFO sii{sizeof(sii)};
    if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &sii))) return sii.iSysImageIndex;
    return 0;
}
} // namespace

bool DirTree::Create(HWND parent, int id, std::function<void(const std::wstring&)> onNavigate,
                     std::function<void()> onActivate) {
    onNavigate_ = std::move(onNavigate);
    onActivate_ = std::move(onActivate);
    tree_ = CreateWindowExW(0, WC_TREEVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                                TVS_TRACKSELECT | TVS_FULLROWSELECT,
                            0, 0, 100, 100, parent, (HMENU)(INT_PTR)id, App::Instance(), nullptr);
    if (!tree_) return false;
    SetWindowTheme(tree_, L"Explorer", nullptr);
    TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER | TVS_EX_AUTOHSCROLL, TVS_EX_DOUBLEBUFFER | TVS_EX_AUTOHSCROLL);
    SHFILEINFOW sfi{};
    HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    TreeView_SetImageList(tree_, il, TVSIL_NORMAL);
    folderIcon_ = StockIconIndex(SIID_FOLDER);
    folderOpenIcon_ = StockIconIndex(SIID_FOLDEROPEN);
    SendMessageW(tree_, WM_SETFONT, (WPARAM)App::UIFont(), TRUE);
    SetWindowSubclass(tree_, Subclass, 1, (DWORD_PTR)this);
    RegisterFileDropTarget(
        tree_, [this](POINT pt) { return DropDirAt(pt); }, [this]() { App::RefreshPanes(); });
    Populate();
    return true;
}

HTREEITEM DirTree::AddItem(HTREEITEM parent, const std::wstring& text, const std::wstring& path, int icon, int openIcon,
                           bool hasChildren) {
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.lParam = (LPARAM) new std::wstring(path);
    ins.item.iImage = icon;
    ins.item.iSelectedImage = openIcon;
    ins.item.cChildren = hasChildren ? 1 : 0;
    return TreeView_InsertItem(tree_, &ins);
}

void DirTree::Populate() {
    suppress_ = true;
    std::wstring sel = SelectedPath();
    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        if ((i == 0 || i == 1) && type == DRIVE_REMOVABLE) continue; // keine Diskettenlaufwerke
        SHSTOCKICONID sid = SIID_DRIVEFIXED;
        switch (type) {
        case DRIVE_REMOVABLE: sid = SIID_DRIVEREMOVE; break;
        case DRIVE_REMOTE: sid = SIID_DRIVENET; break;
        case DRIVE_CDROM: sid = SIID_DRIVECD; break;
        case DRIVE_RAMDISK: sid = SIID_DRIVERAM; break;
        default: break;
        }
        std::wstring label = root.substr(0, 2);
        if (type == DRIVE_REMOTE) {
            // Netzlaufwerk: verbundene Freigabe anzeigen (wie Idoswin Pro)
            std::wstring target = MappedDriveTarget(root);
            if (!target.empty()) label += L"  " + target;
        } else if (type == DRIVE_FIXED || type == DRIVE_RAMDISK || type == DRIVE_REMOVABLE) {
            wchar_t name[MAX_PATH + 1] = {};
            UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
            if (GetVolumeInformationW(root.c_str(), name, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0) && name[0])
                label += L"  " + std::wstring(name);
            SetErrorMode(oldMode);
        }
        int icon = StockIconIndex(sid);
        AddItem(TVI_ROOT, label, root, icon, icon, true);
    }
    // Netzwerk (aufklappbar: Rechner, darunter deren Freigaben)
    int net = StockIconIndex(SIID_MYNETWORK);
    AddItem(TVI_ROOT, kNetworkName, kNetworkRoot, net, net, true);
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    suppress_ = false;
    if (!sel.empty()) SelectPath(sel);
}

void DirTree::ApplyOptions() {
    std::wstring sel = SelectedPath();
    Populate();
    if (!sel.empty()) SelectPath(sel);
}

void DirTree::FillChildren(HTREEITEM item) {
    // Vorhandene Kinder entfernen
    HTREEITEM c = TreeView_GetChild(tree_, item);
    while (c) {
        HTREEITEM next = TreeView_GetNextSibling(tree_, c);
        TreeView_DeleteItem(tree_, c);
        c = next;
    }
    std::wstring path = ItemPath(item);
    if (IsNetworkVirtual(path)) {
        StartScan(item);
        return;
    }
    std::vector<DirEntry> entries;
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    ListDirectory(path, entries);
    SetErrorMode(oldMode);
    const Options& o = App::Opt();
    std::vector<std::wstring> dirs;
    for (auto& e : entries) {
        if (!e.IsDir()) continue;
        if (!o.showHidden && (e.attributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        if (!o.showSystem && (e.attributes & FILE_ATTRIBUTE_SYSTEM) && (e.attributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        dirs.push_back(e.name);
    }
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return CompareNatural(a, b) < 0; });
    for (auto& d : dirs) AddItem(item, d, PathCombine(path, d), folderIcon_, folderOpenIcon_, true);
    if (dirs.empty()) {
        TVITEMW it{};
        it.mask = TVIF_CHILDREN;
        it.hItem = item;
        it.cChildren = 0;
        TreeView_SetItem(tree_, &it);
    }
}

void DirTree::StartScan(HTREEITEM item) {
    auto old = scans_.find(item);
    if (old != scans_.end() && old->second.scan) old->second.scan->cancel = true;
    std::wstring path = ItemPath(item);
    // Platzhalter während der Suche (ohne Pfad: führt nirgendwohin)
    TVINSERTSTRUCTW ins{};
    ins.hParent = item;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    std::wstring text = IsNetworkRoot(path) ? L"Suche Rechner … (Esc bricht ab)" : L"Lese Freigaben … (Esc bricht ab)";
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.lParam = 0;
    ins.item.cChildren = 0;
    HTREEITEM placeholder = TreeView_InsertItem(tree_, &ins);
    unsigned gen = ++scanGen_;
    HWND tree = tree_;
    Scan sc;
    sc.gen = gen;
    sc.placeholder = placeholder;
    sc.scan = StartNetworkScan(
        path, App::Opt().showHidden,
        [tree, item, gen](const NetworkItem& it) {
            auto* m = new TreeScanItem{item, gen, it};
            if (!PostMessageW(tree, WM_TREE_SCANITEM, 0, (LPARAM)m)) delete m;
        },
        [tree, item, gen](bool ok, DWORD error) {
            auto* m = new TreeScanDone{item, gen, ok, error};
            if (!PostMessageW(tree, WM_TREE_SCANDONE, 0, (LPARAM)m)) delete m;
        });
    scans_[item] = sc;
}

void DirTree::OnScanItem(LPARAM lp) {
    std::unique_ptr<TreeScanItem> m((TreeScanItem*)lp);
    auto it = scans_.find(m->parent);
    if (it == scans_.end() || it->second.gen != m->gen) return;
    std::wstring parentPath = ItemPath(m->parent);
    bool servers = IsNetworkRoot(parentPath);
    std::wstring path = servers ? (L"\\\\" + m->item.name) : (parentPath + L"\\" + m->item.name);
    int icon = StockIconIndex(servers ? SIID_SERVER : SIID_SERVERSHARE);
    TVINSERTSTRUCTW ins{};
    ins.hParent = m->parent;
    ins.hInsertAfter = TVI_SORT;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    ins.item.pszText = const_cast<wchar_t*>(m->item.name.c_str());
    ins.item.lParam = (LPARAM) new std::wstring(path);
    ins.item.iImage = ins.item.iSelectedImage = icon;
    ins.item.cChildren = 1;
    TreeView_InsertItem(tree_, &ins);
}

void DirTree::OnScanDone(LPARAM lp) {
    std::unique_ptr<TreeScanDone> m((TreeScanDone*)lp);
    auto it = scans_.find(m->parent);
    if (it == scans_.end() || it->second.gen != m->gen) return;
    HTREEITEM placeholder = it->second.placeholder;
    scans_.erase(it);
    if (placeholder) TreeView_DeleteItem(tree_, placeholder);
    if (!TreeView_GetChild(tree_, m->parent)) {
        // Nichts gefunden bzw. Rechner nicht erreichbar
        TVINSERTSTRUCTW ins{};
        ins.hParent = m->parent;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM;
        std::wstring text = m->ok ? L"(nichts gefunden)" : (L"(nicht erreichbar: " + LastErrorMessage(m->error) + L")");
        ins.item.pszText = const_cast<wchar_t*>(text.c_str());
        ins.item.lParam = 0;
        TreeView_InsertItem(tree_, &ins);
    }
}

void DirTree::CancelScans() {
    for (auto& [item, sc] : scans_) {
        if (sc.scan) sc.scan->cancel = true;
        if (sc.placeholder) {
            TVITEMW t{};
            t.mask = TVIF_TEXT;
            t.hItem = sc.placeholder;
            std::wstring text = L"(Suche abgebrochen)";
            t.pszText = const_cast<wchar_t*>(text.c_str());
            TreeView_SetItem(tree_, &t);
        }
    }
    scans_.clear();
}

HTREEITEM DirTree::NetworkNode() const {
    for (HTREEITEM c = TreeView_GetRoot(tree_); c; c = TreeView_GetNextSibling(tree_, c))
        if (IsNetworkRoot(ItemPath(c))) return c;
    return nullptr;
}

std::wstring DirTree::ItemPath(HTREEITEM item) const {
    if (!item) return L"";
    TVITEMW it{};
    it.mask = TVIF_PARAM;
    it.hItem = item;
    if (!TreeView_GetItem(tree_, &it) || !it.lParam) return L"";
    return *(std::wstring*)it.lParam;
}

std::wstring DirTree::SelectedPath() const { return ItemPath(TreeView_GetSelection(tree_)); }

HTREEITEM DirTree::FindChild(HTREEITEM parent, const std::wstring& name) const {
    HTREEITEM c = parent ? TreeView_GetChild(tree_, parent) : TreeView_GetRoot(tree_);
    while (c) {
        std::wstring p = ItemPath(c);
        std::wstring last = parent ? PathFileName(p) : p;
        if (EqualsI(last, name)) return c;
        c = TreeView_GetNextSibling(tree_, c);
    }
    return nullptr;
}

void DirTree::SelectPath(const std::wstring& path) {
    if (path.empty()) return;
    if (IsNetworkRoot(path) || StartsWithI(path, L"\\\\")) {
        // Netzwerk: nur bereits gefundene Knoten markieren (keine neue Suche anstoßen)
        HTREEITEM item = NetworkNode();
        if (!item) return;
        if (!IsNetworkRoot(path)) {
            for (auto& part : Split(path.substr(2), L'\\')) {
                HTREEITEM child = nullptr;
                for (HTREEITEM c = TreeView_GetChild(tree_, item); c; c = TreeView_GetNextSibling(tree_, c)) {
                    TVITEMW t{};
                    wchar_t buf[512] = {};
                    t.mask = TVIF_TEXT;
                    t.hItem = c;
                    t.pszText = buf;
                    t.cchTextMax = 511;
                    TreeView_GetItem(tree_, &t);
                    if (EqualsI(buf, part) && !ItemPath(c).empty()) {
                        child = c;
                        break;
                    }
                }
                if (!child) break;
                item = child;
            }
        }
        suppress_ = true;
        TreeView_SelectItem(tree_, item);
        TreeView_EnsureVisible(tree_, item);
        suppress_ = false;
        return;
    }
    std::wstring root = PathRoot(path);
    if (root.empty()) return;
    suppress_ = true;
    HTREEITEM item = FindChild(nullptr, root);
    if (item) {
        std::wstring rest = path.substr(std::min(root.size(), path.size()));
        for (auto& part : Split(rest, L'\\')) {
            TVITEMW it{};
            it.mask = TVIF_STATE;
            it.hItem = item;
            it.stateMask = TVIS_EXPANDEDONCE;
            TreeView_GetItem(tree_, &it);
            if (!(it.state & TVIS_EXPANDEDONCE) || !TreeView_GetChild(tree_, item)) TreeView_Expand(tree_, item, TVE_EXPAND);
            else TreeView_Expand(tree_, item, TVE_EXPAND);
            HTREEITEM child = FindChild(item, part);
            if (!child) {
                // Liste kann veraltet sein: neu einlesen
                FillChildren(item);
                child = FindChild(item, part);
            }
            if (!child) break;
            item = child;
        }
        TreeView_SelectItem(tree_, item);
        TreeView_EnsureVisible(tree_, item);
    }
    suppress_ = false;
}

void DirTree::RefreshPath(const std::wstring& path) {
    std::wstring sel = SelectedPath();
    HTREEITEM item = nullptr;
    std::wstring root = PathRoot(path);
    if (root.empty() || StartsWithI(root, L"\\\\")) return;
    item = FindChild(nullptr, root);
    if (!item) return;
    std::wstring rest = path.substr(std::min(root.size(), path.size()));
    for (auto& part : Split(rest, L'\\')) {
        HTREEITEM child = FindChild(item, part);
        if (!child) return;
        item = child;
    }
    TVITEMW it{};
    it.mask = TVIF_STATE;
    it.hItem = item;
    it.stateMask = TVIS_EXPANDED;
    TreeView_GetItem(tree_, &it);
    if (it.state & TVIS_EXPANDED) {
        suppress_ = true;
        FillChildren(item);
        suppress_ = false;
        if (!sel.empty()) SelectPath(sel);
    } else {
        // Beim nächsten Aufklappen neu lesen
        TVITEMW c{};
        c.mask = TVIF_STATE | TVIF_CHILDREN;
        c.hItem = item;
        c.stateMask = TVIS_EXPANDEDONCE;
        c.state = 0;
        c.cChildren = 1;
        TreeView_SetItem(tree_, &c);
    }
}

std::wstring DirTree::DropDirAt(POINT screenPt) const {
    TVHITTESTINFO ht{};
    ht.pt = screenPt;
    ScreenToClient(tree_, &ht.pt);
    HTREEITEM item = TreeView_HitTest(tree_, &ht);
    if (!item || !(ht.flags & TVHT_ONITEM)) return L"";
    std::wstring p = ItemPath(item);
    return IsNetworkVirtual(p) ? L"" : p;
}

LRESULT DirTree::OnNotify(NMHDR* nm) {
    switch (nm->code) {
    case TVN_ITEMEXPANDINGW: {
        auto* tv = (NMTREEVIEWW*)nm;
        if (tv->action == TVE_EXPAND) {
            TVITEMW it{};
            it.mask = TVIF_STATE;
            it.hItem = tv->itemNew.hItem;
            it.stateMask = TVIS_EXPANDEDONCE;
            TreeView_GetItem(tree_, &it);
            if (!(it.state & TVIS_EXPANDEDONCE) || !TreeView_GetChild(tree_, tv->itemNew.hItem)) {
                HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
                SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
                FillChildren(tv->itemNew.hItem);
                SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
                SetCursor(old);
            }
        }
        return FALSE;
    }
    case TVN_DELETEITEMW: {
        auto* tv = (NMTREEVIEWW*)nm;
        delete (std::wstring*)tv->itemOld.lParam;
        auto sc = scans_.find(tv->itemOld.hItem);
        if (sc != scans_.end()) {
            if (sc->second.scan) sc->second.scan->cancel = true;
            scans_.erase(sc);
        }
        return 0;
    }
    case TVN_SELCHANGEDW: {
        auto* tv = (NMTREEVIEWW*)nm;
        if (suppress_) return 0;
        if (tv->action == TVC_BYMOUSE && App::Opt().singleClickTree) {
            std::wstring p = ItemPath(tv->itemNew.hItem);
            if (!p.empty() && onNavigate_) onNavigate_(p);
        } else if (tv->action == TVC_BYKEYBOARD && App::Opt().singleClickTree) {
            // Tastatur: kurz verzögert navigieren, damit schnelles Blättern flüssig bleibt
            SetTimer(tree_, 1, 350, nullptr);
        }
        return 0;
    }
    case NM_DBLCLK: {
        std::wstring p = SelectedPath();
        if (!p.empty() && onNavigate_) onNavigate_(p);
        return 0;
    }
    case NM_RCLICK: {
        POINT pt;
        GetCursorPos(&pt);
        TVHITTESTINFO ht{};
        ht.pt = pt;
        ScreenToClient(tree_, &ht.pt);
        HTREEITEM item = TreeView_HitTest(tree_, &ht);
        if (item) {
            std::wstring p = ItemPath(item);
            if (!p.empty() && !IsNetworkVirtual(p)) {
                bool rename = false;
                if (IsRootPath(p))
                    ShowShellContextMenu(GetAncestor(tree_, GA_ROOT), p, {}, pt, nullptr, &rename);
                else
                    ShowShellContextMenu(GetAncestor(tree_, GA_ROOT), PathParent(p), {PathFileName(p)}, pt, nullptr, &rename);
            }
        }
        return TRUE;
    }
    case NM_SETFOCUS:
        if (onActivate_) onActivate_();
        return 0;
    case TVN_KEYDOWN: {
        auto* kd = (NMTVKEYDOWN*)nm;
        if (kd->wVKey == VK_RETURN) {
            std::wstring p = SelectedPath();
            if (!p.empty() && onNavigate_) onNavigate_(p);
            return TRUE;
        }
        return 0;
    }
    }
    return 0;
}

LRESULT CALLBACK DirTree::Subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = (DirTree*)ref;
    switch (msg) {
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(h, 1);
            std::wstring p = self->SelectedPath();
            if (!p.empty() && self->onNavigate_) self->onNavigate_(p);
            return 0;
        }
        break;
    case WM_TREE_SCANITEM:
        self->OnScanItem(lp);
        return 0;
    case WM_TREE_SCANDONE:
        self->OnScanDone(lp);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !self->scans_.empty()) {
            self->CancelScans();
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        if (lp && ((MSG*)lp)->message == WM_KEYDOWN && ((MSG*)lp)->wParam == VK_RETURN) return DLGC_WANTALLKEYS;
        break;
    case WM_CHAR:
        if (wp == L'\r') return 0;
        break;
    case WM_NCDESTROY:
        RevokeFileDropTarget(h);
        RemoveWindowSubclass(h, Subclass, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

} // namespace qf
