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

// ---- Schutz vor abstürzenden Shell-Erweiterungen ----
// Kontextmenü-Erweiterungen fremder Programme laufen in unserem Prozess. Stürzt eine davon ab, soll nicht
// QFiles mit beendet werden: Mit MSVC werden die Aufrufe per SEH abgesichert, das Menü wird verworfen und das
// verursachende Modul gemeldet.
bool g_extensionFault = false;
std::wstring g_faultModule;

#ifdef _MSC_VER
int FaultFilter(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod) && mod)
        GetModuleFileNameW(mod, name, MAX_PATH);
    g_faultModule = name;
    g_extensionFault = true;
    return EXCEPTION_EXECUTE_HANDLER;
}
#define QF_TRY __try
#define QF_EXCEPT __except (FaultFilter(GetExceptionInformation()))
#else
#define QF_TRY if (true)
#define QF_EXCEPT else
#endif

HRESULT SafeQueryContextMenu(IContextMenu* cm, HMENU menu, UINT index, UINT flags) {
    QF_TRY { return cm->QueryContextMenu(menu, index, kShellFirst, kShellLast, flags); }
    QF_EXCEPT { return E_FAIL; }
}
HRESULT SafeGetVerb(IContextMenu* cm, UINT_PTR idx, wchar_t* buf, UINT cch) {
    QF_TRY { return cm->GetCommandString(idx, GCS_VERBW, nullptr, (LPSTR)buf, cch); }
    QF_EXCEPT { return E_FAIL; }
}
HRESULT SafeInvoke(IContextMenu* cm, CMINVOKECOMMANDINFOEX* ici) {
    QF_TRY { return cm->InvokeCommand((LPCMINVOKECOMMANDINFO)ici); }
    QF_EXCEPT { return E_FAIL; }
}
HRESULT SafeHandleMenuMsg2(IContextMenu3* cm, UINT msg, WPARAM wp, LPARAM lp, LRESULT* r) {
    QF_TRY { return cm->HandleMenuMsg2(msg, wp, lp, r); }
    QF_EXCEPT { return E_FAIL; }
}
HRESULT SafeHandleMenuMsg(IContextMenu2* cm, UINT msg, WPARAM wp, LPARAM lp) {
    QF_TRY { return cm->HandleMenuMsg(msg, wp, lp); }
    QF_EXCEPT { return E_FAIL; }
}
void SafeRelease(IUnknown* u) {
    if (!u || g_extensionFault) return; // nach einem Fehler das Objekt lieber nicht mehr anfassen
    QF_TRY { u->Release(); }
    QF_EXCEPT {}
}
HRESULT SafeGetUIObjectOf(IShellFolder* f, HWND owner, UINT n, PCUITEMID_CHILD_ARRAY a, IContextMenu** cm) {
    QF_TRY { return f->GetUIObjectOf(owner, n, a, IID_IContextMenu, nullptr, (void**)cm); }
    QF_EXCEPT { return E_FAIL; }
}

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
    if (names.empty() && IsRootPath(dir)) {
        // Laufwerk: Menü des Laufwerks selbst (Auswerfen, Formatieren, Eigenschaften …) über "Dieser PC"
        std::wstring d = dir;
        if (d.size() == 2) d += L'\\';
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(d.c_str(), nullptr, &pidl, 0, nullptr))) {
            IShellFolder* parent = nullptr;
            PCUITEMID_CHILD child = nullptr;
            if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child))) {
                SafeGetUIObjectOf(parent, owner, 1, &child, &cm);
                parent->Release();
            }
            CoTaskMemFree(pidl);
        }
    }
    if (!cm && si.Init(owner, dir, names)) {
        if (names.empty())
            si.folder->CreateViewObject(owner, IID_PPV_ARGS(&cm));
        else
            SafeGetUIObjectOf(si.folder, owner, (UINT)si.children.size(), (PCUITEMID_CHILD_ARRAY)si.children.data(), &cm);
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
        SafeQueryContextMenu(cm, menu, GetMenuItemCount(menu), flags);
        if (!g_extensionFault) {
            cm->QueryInterface(IID_PPV_ARGS(&g_cm3));
            if (!g_cm3) cm->QueryInterface(IID_PPV_ARGS(&g_cm2));
        }
    }
    if (g_extensionFault) {
        // Erweiterung abgestürzt: nur die eigenen Einträge anbieten
        DestroyMenu(menu);
        menu = CreatePopupMenu();
        for (int i = 0; i < extraCount; ++i) {
            wchar_t text[256] = {};
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STRING | MIIM_STATE;
            mi.dwTypeData = text;
            mi.cch = 255;
            GetMenuItemInfoW(extra, i, TRUE, &mi);
            mi.dwTypeData = text;
            InsertMenuItemW(menu, i, TRUE, &mi);
        }
        cm = nullptr;
        extraCount = 0;
    }
    int result = 0;
    UINT id = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
    SafeRelease(g_cm3);
    g_cm3 = nullptr;
    SafeRelease(g_cm2);
    g_cm2 = nullptr;
    if (g_extensionFault) cm = nullptr;
    if (id >= 40000) {
        result = (int)id;
    } else if (id >= kShellFirst && id <= kShellLast && cm) {
        wchar_t verb[128] = {};
        if (FAILED(SafeGetVerb(cm, id - kShellFirst, verb, 127))) verb[0] = 0;
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
            SafeInvoke(cm, &ici);
            LogOperation(L"Kontextmenü „" + std::wstring(verb[0] ? verb : L"Befehl") + L"“ in " + dir);
        }
    }
    SafeRelease(cm);
    if (g_extensionFault) {
        g_extensionFault = false;
        std::wstring msg = L"Eine Kontextmenü-Erweiterung eines anderen Programms ist abgestürzt und wurde übergangen:\n\n" +
                           g_faultModule + L"\n\nQFiles läuft weiter. Das Explorer-Kontextmenü steht für diese Elemente "
                                           L"eventuell nur eingeschränkt zur Verfügung.";
        LogOperation(L"Kontextmenü-Erweiterung abgestürzt: " + g_faultModule);
        MsgError(owner, msg);
    }
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
        if (SUCCEEDED(SafeHandleMenuMsg2(g_cm3, msg, wp, lp, &r))) {
            *result = r;
            return true;
        }
    } else if (g_cm2) {
        if (SUCCEEDED(SafeHandleMenuMsg(g_cm2, msg, wp, lp))) {
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
