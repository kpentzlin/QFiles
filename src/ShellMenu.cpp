#include "ShellMenu.h"
#include "App.h"
#include "FileOps.h"
#include "Util.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>

#undef PathCombine
#undef StrToInt

namespace qf {

namespace {

IContextMenu2* g_cm2 = nullptr;
IContextMenu3* g_cm3 = nullptr;

constexpr UINT kShellFirst = 1;
constexpr UINT kShellLast = 30000;

// Liefert IShellFolder des Verzeichnisses und die relativen PIDLs der Namen.
struct ShellItems {
    IShellFolder* folder = nullptr;
    std::vector<PITEMID_CHILD> children;
    PIDLIST_ABSOLUTE dirPidl = nullptr;
    ~ShellItems() {
        for (auto p : children) CoTaskMemFree(p);
        if (folder) folder->Release();
        if (dirPidl) CoTaskMemFree(dirPidl);
    }
    bool Init(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names) {
        std::wstring d = dir;
        if (d.size() == 2 && d[1] == L':') d += L'\\';
        if (FAILED(SHParseDisplayName(d.c_str(), nullptr, &dirPidl, 0, nullptr))) return false;
        if (FAILED(SHBindToObject(nullptr, dirPidl, nullptr, IID_PPV_ARGS(&folder)))) return false;
        for (auto& n : names) {
            PIDLIST_RELATIVE child = nullptr;
            ULONG eaten = 0;
            if (FAILED(folder->ParseDisplayName(owner, nullptr, const_cast<LPWSTR>(n.c_str()), &eaten, &child, nullptr)))
                return false;
            children.push_back((PITEMID_CHILD)child);
        }
        return true;
    }
};

} // namespace

int ShowShellContextMenu(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names, POINT pt,
                         HMENU extra, bool* renameRequested) {
    if (renameRequested) *renameRequested = false;
    ShellItems si;
    IContextMenu* cm = nullptr;
    if (si.Init(owner, dir, names)) {
        if (names.empty())
            si.folder->CreateViewObject(owner, IID_PPV_ARGS(&cm));
        else
            si.folder->GetUIObjectOf(owner, (UINT)si.children.size(), (PCUITEMID_CHILD_ARRAY)si.children.data(),
                                     IID_IContextMenu, nullptr, (void**)&cm);
    }
    HMENU menu = CreatePopupMenu();
    // Eigene Einträge oben
    int extraCount = extra ? GetMenuItemCount(extra) : 0;
    for (int i = 0; i < extraCount; ++i) {
        wchar_t text[256] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STRING | MIIM_STATE | MIIM_SUBMENU;
        mi.dwTypeData = text;
        mi.cch = 255;
        GetMenuItemInfoW(extra, i, TRUE, &mi);
        mi.dwTypeData = text;
        InsertMenuItemW(menu, i, TRUE, &mi);
    }
    if (cm) {
        if (extraCount) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        UINT flags = CMF_NORMAL | CMF_EXPLORE | (names.empty() ? 0 : CMF_CANRENAME);
        if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
        cm->QueryContextMenu(menu, GetMenuItemCount(menu), kShellFirst, kShellLast, flags);
        cm->QueryInterface(IID_PPV_ARGS(&g_cm3));
        if (!g_cm3) cm->QueryInterface(IID_PPV_ARGS(&g_cm2));
    }
    int result = 0;
    UINT id = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
    if (g_cm3) {
        g_cm3->Release();
        g_cm3 = nullptr;
    }
    if (g_cm2) {
        g_cm2->Release();
        g_cm2 = nullptr;
    }
    if (id >= 40000) {
        result = (int)id;
    } else if (id >= kShellFirst && id <= kShellLast && cm) {
        wchar_t verb[128] = {};
        if (FAILED(cm->GetCommandString(id - kShellFirst, GCS_VERBW, nullptr, (LPSTR)verb, 127))) verb[0] = 0;
        if (_wcsicmp(verb, L"rename") == 0) {
            if (renameRequested) *renameRequested = true;
        } else {
            CMINVOKECOMMANDINFOEX ici{sizeof(ici)};
            ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            if (GetKeyState(VK_CONTROL) < 0) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
            if (GetKeyState(VK_SHIFT) < 0) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
            ici.hwnd = owner;
            ici.lpVerb = MAKEINTRESOURCEA(id - kShellFirst);
            ici.lpVerbW = MAKEINTRESOURCEW(id - kShellFirst);
            ici.lpDirectoryW = dir.c_str();
            ici.nShow = SW_SHOWNORMAL;
            ici.ptInvoke = pt;
            cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&ici);
            LogOperation(L"Kontextmenü „" + std::wstring(verb[0] ? verb : L"Befehl") + L"“ in " + dir);
        }
    }
    if (cm) cm->Release();
    // Eigene Untermenüs gehören dem Aufrufer: vor dem Zerstören lösen
    for (int i = 0; i < extraCount; ++i) {
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_SUBMENU;
        mi.hSubMenu = nullptr;
        SetMenuItemInfoW(menu, i, TRUE, &mi);
    }
    DestroyMenu(menu);
    return result;
}

bool HandleShellMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (msg != WM_INITMENUPOPUP && msg != WM_DRAWITEM && msg != WM_MEASUREITEM && msg != WM_MENUCHAR) return false;
    if (g_cm3) {
        LRESULT r = 0;
        if (SUCCEEDED(g_cm3->HandleMenuMsg2(msg, wp, lp, &r))) {
            *result = r;
            return true;
        }
    } else if (g_cm2) {
        if (SUCCEEDED(g_cm2->HandleMenuMsg(msg, wp, lp))) {
            *result = 0;
            return true;
        }
    }
    return false;
}

void ShowShellProperties(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names) {
    if (names.empty()) {
        SHObjectProperties(owner, SHOP_FILEPATH, dir.c_str(), nullptr);
        return;
    }
    if (names.size() == 1) {
        SHObjectProperties(owner, SHOP_FILEPATH, PathCombine(dir, names[0]).c_str(), nullptr);
        return;
    }
    IDataObject* data = CreateShellDataObject(owner, dir, names);
    if (data) {
        SHMultiFileProperties(data, 0);
        data->Release();
    }
}

IDataObject* CreateShellDataObject(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names) {
    ShellItems si;
    if (names.empty() || !si.Init(owner, dir, names)) return nullptr;
    IDataObject* data = nullptr;
    si.folder->GetUIObjectOf(owner, (UINT)si.children.size(), (PCUITEMID_CHILD_ARRAY)si.children.data(),
                             IID_IDataObject, nullptr, (void**)&data);
    return data;
}

DWORD StartFileDrag(HWND hwnd, const std::wstring& dir, const std::vector<std::wstring>& names) {
    IDataObject* data = CreateShellDataObject(hwnd, dir, names);
    if (!data) return DROPEFFECT_NONE;
    DWORD effect = DROPEFFECT_NONE;
    SHDoDragDrop(hwnd, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    data->Release();
    return effect;
}

// ===================== Ablageziel =====================

namespace {

class DropTarget : public IDropTarget {
public:
    DropTarget(HWND hwnd, std::function<std::wstring(POINT)> resolver, std::function<void()> done)
        : hwnd_(hwnd), resolver_(std::move(resolver)), done_(std::move(done)) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }
    virtual ~DropTarget() {
        if (helper_) helper_->Release();
    }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }

    IFACEMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        files_.clear();
        rightButton_ = (keys & MK_RBUTTON) != 0;
        ReadFiles(data);
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->DragEnter(hwnd_, data, &p, *effect);
        }
        *effect = Decide(keys, pt, *effect);
        return S_OK;
    }
    IFACEMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        *effect = Decide(keys, pt, *effect);
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->DragOver(&p, *effect);
        }
        return S_OK;
    }
    IFACEMETHODIMP DragLeave() override {
        if (helper_) helper_->DragLeave();
        files_.clear();
        return S_OK;
    }
    IFACEMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->Drop(data, &p, *effect);
        }
        if (files_.empty()) ReadFiles(data);
        DWORD eff = Decide(keys, pt, *effect);
        std::wstring target = resolver_ ? resolver_(POINT{pt.x, pt.y}) : L"";
        if (eff == DROPEFFECT_NONE || target.empty() || files_.empty()) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        if (rightButton_) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Hierher &kopieren");
            AppendMenuW(m, MF_STRING, 2, L"Hierher &verschieben");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 3, L"&Abbrechen");
            SetMenuDefaultItem(m, eff == DROPEFFECT_MOVE ? 2 : 1, FALSE);
            UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr);
            DestroyMenu(m);
            if (id == 1) eff = DROPEFFECT_COPY;
            else if (id == 2) eff = DROPEFFECT_MOVE;
            else eff = DROPEFFECT_NONE;
        }
        HWND owner = GetAncestor(hwnd_, GA_ROOT);
        auto files = files_;
        files_.clear();
        if (eff == DROPEFFECT_MOVE) MoveItems(owner, files, target);
        else if (eff == DROPEFFECT_COPY) CopyItems(owner, files, target);
        // Für die Quelle: Verschieben bereits erledigt -> DROPEFFECT_NONE melden, damit sie nicht selbst löscht
        *effect = (eff == DROPEFFECT_MOVE) ? DROPEFFECT_NONE : eff;
        if (done_) done_();
        return S_OK;
    }

private:
    void ReadFiles(IDataObject* data) {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (SUCCEEDED(data->GetData(&fe, &sm))) {
            HDROP drop = (HDROP)GlobalLock(sm.hGlobal);
            if (drop) {
                UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    UINT len = DragQueryFileW(drop, i, nullptr, 0);
                    std::wstring s(len + 1, L'\0');
                    DragQueryFileW(drop, i, s.data(), len + 1);
                    s.resize(len);
                    files_.push_back(s);
                }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
        }
    }

    DWORD Decide(DWORD keys, POINTL pt, DWORD allowed) {
        if (files_.empty()) return DROPEFFECT_NONE;
        std::wstring target = resolver_ ? resolver_(POINT{pt.x, pt.y}) : L"";
        if (target.empty()) return DROPEFFECT_NONE;
        // Nicht in sich selbst bzw. in dasselbe Verzeichnis
        bool sameDir = true;
        for (auto& f : files_) {
            if (EqualsI(f, target) || StartsWithI(target, f + L"\\")) return DROPEFFECT_NONE;
            if (!EqualsI(PathParent(f), target)) sameDir = false;
        }
        if (sameDir) return DROPEFFECT_NONE;
        DWORD eff;
        if ((keys & MK_CONTROL) && (keys & MK_SHIFT)) eff = DROPEFFECT_COPY;
        else if (keys & MK_SHIFT) eff = DROPEFFECT_MOVE;
        else if (keys & MK_CONTROL) eff = DROPEFFECT_COPY;
        else eff = EqualsI(PathRoot(files_[0]), PathRoot(target)) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        if (!(allowed & eff)) eff = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return eff;
    }

    ULONG ref_ = 1;
    HWND hwnd_;
    std::function<std::wstring(POINT)> resolver_;
    std::function<void()> done_;
    IDropTargetHelper* helper_ = nullptr;
    std::vector<std::wstring> files_;
    bool rightButton_ = false;
};

} // namespace

void RegisterFileDropTarget(HWND hwnd, std::function<std::wstring(POINT)> resolver, std::function<void()> done) {
    auto* t = new DropTarget(hwnd, std::move(resolver), std::move(done));
    RegisterDragDrop(hwnd, t);
    t->Release(); // RegisterDragDrop hält eine eigene Referenz
}

void RevokeFileDropTarget(HWND hwnd) { RevokeDragDrop(hwnd); }

} // namespace qf
