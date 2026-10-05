#include "ShellMenu.h"
#include "App.h"
#include "FileOps.h"
#include "Util.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <dbghelp.h>
#include <future>
#include <new>

#undef PathCombine
#undef StrToInt

namespace qf {

namespace {

IContextMenu2* g_cm2 = nullptr;
IContextMenu3* g_cm3 = nullptr;

constexpr UINT kShellFirst = 1;
constexpr UINT kShellLast = 30000;

// ---- Schutz vor abstürzenden Shell-Erweiterungen ----
// Kontextmenü-Erweiterungen fremder Programme laufen in unserem Prozess. Wirft eine davon eine unbehandelte
// Ausnahme, soll nicht QFiles mit beendet werden: Mit MSVC werden die Aufrufe per SEH abgesichert. Für die
// Fehlersuche werden Aufruf, Menüeintrag, Ausnahme, C++-Ausnahmetyp/-text und die Module des Aufrufstapels
// festgehalten, gemeldet und in %ProgramData%\QFiles\QFiles-Kontextmenue.txt protokolliert.

struct FaultInfo {
    int count = 0;              // Anzahl abgefangener Ausnahmen in diesem Menüdurchlauf
    bool inQuery = false;       // Fehler beim Aufbau des Menüs (QueryContextMenu/GetUIObjectOf)
    std::wstring where;         // Aufruf + Menüeintrag
    unsigned code = 0;
    std::wstring address;       // Modul+Offset
    std::wstring cppInfo;       // C++-Ausnahmetyp und -text
    std::vector<std::wstring> stack;      // Modul+Offset je Rahmen
    std::vector<std::wstring> stackPaths; // vollständige Modulpfade (eindeutig, in Reihenfolge)
};
FaultInfo g_fault;
bool g_extensionFault = false;   // Fehler beim Aufbau -> Menü ohne Shell-Einträge
std::wstring g_where;            // aktuell laufender Aufruf (für die Meldung)
HMENU g_menu = nullptr;          // aktuell angezeigtes Kontextmenü
bool g_lastFaultExcluded = false; // letzte Ausnahme stammt (auch) aus einer ausgeschlossenen Erweiterung
std::vector<UINT> g_skipIds;     // Einträge, deren Zeichnen nicht mehr weitergeleitet wird

std::wstring CleanMenuText(const wchar_t* t) {
    std::wstring r;
    for (const wchar_t* p = t; *p; ++p) {
        if (*p == L'\t') break;
        if (*p == L'&') {
            if (p[1] == L'&') r += L'&', ++p;
            continue;
        }
        r += *p;
    }
    return r;
}

std::wstring FindItemText(HMENU m, UINT id, int depth = 0) {
    if (!m || depth > 8) return L"";
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; ++i) {
        wchar_t text[512] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = text;
        mi.cch = 511;
        if (!GetMenuItemInfoW(m, i, TRUE, &mi)) continue;
        if (!mi.hSubMenu && mi.wID == id) return CleanMenuText(text);
        if (mi.hSubMenu) {
            std::wstring r = FindItemText(mi.hSubMenu, id, depth + 1);
            if (!r.empty()) return CleanMenuText(text) + L" → " + r;
        }
    }
    return L"";
}

std::wstring FindSubmenuText(HMENU m, HMENU sub, int depth = 0) {
    if (!m || depth > 8) return L"";
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; ++i) {
        wchar_t text[512] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = text;
        mi.cch = 511;
        if (!GetMenuItemInfoW(m, i, TRUE, &mi) || !mi.hSubMenu) continue;
        if (mi.hSubMenu == sub) return CleanMenuText(text);
        std::wstring r = FindSubmenuText(mi.hSubMenu, sub, depth + 1);
        if (!r.empty()) return CleanMenuText(text) + L" → " + r;
    }
    return L"";
}

std::wstring DescribeMenuMsg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITMENUPOPUP: {
        std::wstring t = FindSubmenuText(g_menu, (HMENU)wp);
        return L"HandleMenuMsg (WM_INITMENUPOPUP) – Untermenü „" + (t.empty() ? std::wstring(L"?") : t) + L"“ wird geöffnet";
    }
    case WM_MEASUREITEM: {
        auto* mis = (MEASUREITEMSTRUCT*)lp;
        UINT id = mis ? mis->itemID : 0;
        return L"HandleMenuMsg (WM_MEASUREITEM) – Menüeintrag „" + FindItemText(g_menu, id) + L"“ (ID " + std::to_wstring(id) + L")";
    }
    case WM_DRAWITEM: {
        auto* dis = (DRAWITEMSTRUCT*)lp;
        UINT id = dis ? dis->itemID : 0;
        return L"HandleMenuMsg (WM_DRAWITEM) – Menüeintrag „" + FindItemText(g_menu, id) + L"“ (ID " + std::to_wstring(id) + L")";
    }
    case WM_MENUCHAR: return L"HandleMenuMsg (WM_MENUCHAR) – Zugriffstaste „" + std::wstring(1, (wchar_t)LOWORD(wp)) + L"“";
    }
    return L"HandleMenuMsg";
}

[[maybe_unused]] std::wstring ModuleOfAddress(const void* addr, unsigned long long* offset, std::wstring* fullPath) {
    HMODULE mod = nullptr;
    *offset = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) &&
        mod) {
        wchar_t name[MAX_PATH] = {};
        GetModuleFileNameW(mod, name, MAX_PATH);
        if (fullPath) *fullPath = name;
        *offset = (unsigned long long)((const BYTE*)addr - (const BYTE*)mod);
        return PathFileName(name);
    }
    if (fullPath) fullPath->clear();
    return L"?";
}

const wchar_t* ExceptionCodeName(unsigned code) {
    switch (code) {
    case 0xC0000005: return L"Zugriffsverletzung";
    case 0xE06D7363: return L"C++-Ausnahme";
    case 0xC00000FD: return L"Stapelüberlauf";
    case 0xC0000374: return L"Heap beschädigt";
    case 0xC0000409: return L"Sicherheitsprüfung (Pufferüberlauf/Fast-Fail)";
    case 0xC000001D: return L"Ungültiger Befehl";
    case 0xC0000094: return L"Division durch null";
    case 0x80000003: return L"Haltepunkt";
    case 0xC0000008: return L"Ungültiges Handle";
    case 0x80010105: return L"RPC_E_SERVERFAULT";
    case 0x8001010E: return L"RPC_E_WRONG_THREAD";
    case 0x80010108: return L"RPC_E_DISCONNECTED";
    case 0x800706BA: return L"RPC-Server nicht verfügbar";
    case 0xC06D007E: return L"Verzögert geladenes Modul nicht gefunden";
    case 0xC06D007F: return L"Verzögert geladene Funktion nicht gefunden";
    }
    return L"";
}

bool IsSystemModule(const std::wstring& name) {
    static const wchar_t* sys[] = {L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll", L"ucrtbase.dll", L"user32.dll",
                                   L"win32u.dll", L"combase.dll", L"rpcrt4.dll", L"ole32.dll", L"oleaut32.dll",
                                   L"shell32.dll", L"windows.storage.dll", L"shlwapi.dll", L"shcore.dll",
                                   L"comctl32.dll", L"explorerframe.dll", L"msvcrt.dll", L"gdi32.dll", L"gdi32full.dll",
                                   L"sechost.dll", L"QFiles.exe"};
    for (auto s : sys)
        if (EqualsI(name, s)) return true;
    return StartsWithI(name, L"vcruntime") || StartsWithI(name, L"msvcp") || StartsWithI(name, L"api-ms-");
}

// ---- Ausgeschlossene Kontextmenü-Erweiterungen ----
// Erweiterungen, deren Name, Beschreibung oder DLL-Pfad ein Muster aus Optionen „KontextmenueAusschluss“ enthält
// (Standard: „ArchiCrypt“), werden in QFiles nicht geladen: Für ihre CLSIDs registriert QFiles prozesslokal
// (CoRegisterClassObject) eine leere Ersatz-Erweiterung, die keine Menüeinträge anlegt. Registrierung und Explorer
// bleiben unverändert. Sollte eine solche Erweiterung trotzdem geladen sein und einen Fehler verursachen, wird er
// ohne Meldung nur protokolliert und ihr Eintrag für den Rest der Sitzung ausgeblendet.

std::wstring LongPath(const std::wstring& p) {
    if (p.empty()) return p;
    DWORD n = GetLongPathNameW(p.c_str(), nullptr, 0);
    if (!n) return p;
    std::wstring r(n, L'\0');
    n = GetLongPathNameW(p.c_str(), r.data(), n);
    if (!n) return p;
    r.resize(n);
    return r;
}

std::vector<std::wstring> ExcludePatterns(const std::wstring& option) {
    std::vector<std::wstring> r;
    for (auto& p : Split(option, L';')) {
        std::wstring t = Trim(p);
        if (!t.empty()) r.push_back(ToLower(t));
    }
    return r;
}

bool MatchesExclude(const std::wstring& text, const std::vector<std::wstring>& patterns) {
    if (text.empty() || patterns.empty()) return false;
    std::wstring l = ToLower(text);
    for (auto& p : patterns)
        if (l.find(p) != std::wstring::npos) return true;
    return false;
}

bool IsExcludedModulePath(const std::wstring& path) {
    return MatchesExclude(LongPath(path), ExcludePatterns(App::Opt().shellExtExclude));
}

std::wstring RegString(HKEY root, const std::wstring& sub, const wchar_t* value) {
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, sub.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &cb) != ERROR_SUCCESS)
        return L"";
    return buf;
}

struct ExcludedExtension {
    CLSID clsid;
    std::wstring description;   // Handler-Name, Beschreibung, DLL
};

// Durchsucht die Kontextmenü- und Drag&Drop-Handler aller Dateitypen nach passenden Erweiterungen.
std::vector<ExcludedExtension> FindExcludedExtensions(const std::vector<std::wstring>& patterns) {
    std::vector<ExcludedExtension> result;
    if (patterns.empty()) return result;
    std::vector<std::wstring> classKeys = {L"Directory\\Background", L"DesktopBackground"};
    {
        HKEY sfa = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"SystemFileAssociations", 0, KEY_READ, &sfa) == ERROR_SUCCESS) {
            wchar_t name[512];
            for (DWORD i = 0;; ++i) {
                DWORD cch = 511;
                if (RegEnumKeyExW(sfa, i, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                classKeys.push_back(std::wstring(L"SystemFileAssociations\\") + name);
            }
            RegCloseKey(sfa);
        }
        wchar_t name[512];
        for (DWORD i = 0;; ++i) {
            DWORD cch = 511;
            LONG rc = RegEnumKeyExW(HKEY_CLASSES_ROOT, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_MORE_DATA) continue;
            if (rc != ERROR_SUCCESS) break;
            if (_wcsicmp(name, L"CLSID") == 0 || _wcsicmp(name, L"Interface") == 0 || _wcsicmp(name, L"TypeLib") == 0)
                continue;
            classKeys.push_back(name);
        }
    }
    std::vector<CLSID> seen;
    for (auto& ck : classKeys) {
        for (const wchar_t* kind : {L"\\shellex\\ContextMenuHandlers", L"\\shellex\\DragDropHandlers"}) {
            HKEY h = nullptr;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, (ck + kind).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
            wchar_t handler[512];
            for (DWORD i = 0;; ++i) {
                DWORD cch = 511;
                if (RegEnumKeyExW(h, i, handler, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                std::wstring clsidText = RegString(h, handler, nullptr);
                CLSID clsid;
                if (FAILED(CLSIDFromString(clsidText.c_str(), &clsid)) && FAILED(CLSIDFromString(handler, &clsid))) continue;
                if (std::find(seen.begin(), seen.end(), clsid) != seen.end()) continue;
                seen.push_back(clsid);
                wchar_t guid[64] = {};
                StringFromGUID2(clsid, guid, 63);
                std::wstring clsKey = std::wstring(L"CLSID\\") + guid;
                std::wstring desc = RegString(HKEY_CLASSES_ROOT, clsKey, nullptr);
                std::wstring dll = RegString(HKEY_CLASSES_ROOT, clsKey + L"\\InprocServer32", nullptr);
                if (!dll.empty()) {
                    wchar_t exp[1024] = {};
                    if (ExpandEnvironmentStringsW(dll.c_str(), exp, 1023)) dll = exp;
                    if (dll.size() >= 2 && dll.front() == L'"' && dll.back() == L'"') dll = dll.substr(1, dll.size() - 2);
                    dll = LongPath(dll);
                }
                if (MatchesExclude(handler, patterns) || MatchesExclude(desc, patterns) || MatchesExclude(dll, patterns))
                    result.push_back({clsid, std::wstring(handler) + L" " + guid + (desc.empty() ? L"" : L" „" + desc + L"“") +
                                                 (dll.empty() ? L"" : L" – " + dll)});
            }
            RegCloseKey(h);
        }
    }
    return result;
}

// Leere Ersatz-Erweiterung: legt keine Einträge an.
class NullMenuExtension : public IShellExtInit, public IContextMenu {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit) *ppv = static_cast<IShellExtInit*>(this);
        else if (riid == IID_IContextMenu) *ppv = static_cast<IContextMenu*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject*, HKEY) override { return S_OK; }
    STDMETHODIMP QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override { return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0); }
    STDMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO*) override { return E_FAIL; }
    STDMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) override { return E_NOTIMPL; }

private:
    virtual ~NullMenuExtension() = default;
    LONG ref_ = 1;
};

class NullMenuFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // statisches Objekt
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* ext = new (std::nothrow) NullMenuExtension();
        if (!ext) return E_OUTOFMEMORY;
        HRESULT hr = ext->QueryInterface(riid, ppv);
        ext->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};
NullMenuFactory g_nullFactory;

struct ExclusionState {
    std::wstring option;                   // Musterliste, für die registriert wurde
    bool applied = false;
    std::vector<DWORD> cookies;
    std::vector<std::wstring> excluded;    // Beschreibungen (für das Protokoll)
    std::vector<std::wstring> learnedTexts; // Einträge, deren Erweiterung trotz Ausschluss einen Fehler verursacht hat
};
ExclusionState g_excl;
std::future<std::vector<ExcludedExtension>> g_exclPrefetch;
std::wstring g_exclPrefetchOption;

void ApplyExtensionExclusion() {
    const std::wstring& option = App::Opt().shellExtExclude;
    if (g_excl.applied && g_excl.option == option) return;
    for (DWORD c : g_excl.cookies) CoRevokeClassObject(c);
    g_excl.cookies.clear();
    g_excl.excluded.clear();
    std::vector<ExcludedExtension> found;
    if (g_exclPrefetch.valid() && g_exclPrefetchOption == option) {
        found = g_exclPrefetch.get();
    } else {
        if (g_exclPrefetch.valid()) g_exclPrefetch = {};   // wartet auf den veralteten Suchlauf
        found = FindExcludedExtensions(ExcludePatterns(option));
    }
    for (auto& e : found) {
        DWORD cookie = 0;
        HRESULT hr = CoRegisterClassObject(e.clsid, &g_nullFactory, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &cookie);
        if (SUCCEEDED(hr)) g_excl.cookies.push_back(cookie);
        g_excl.excluded.push_back(e.description + (SUCCEEDED(hr) ? L"" : Format(L" (Registrierung fehlgeschlagen: 0x%08X)", (unsigned)hr)));
        LogOperation(L"Kontextmenü-Erweiterung ausgeschlossen: " + g_excl.excluded.back());
    }
    g_excl.option = option;
    g_excl.applied = true;
}

// Entfernt Einträge, deren Erweiterung trotz Ausschluss geladen wurde und einen Fehler verursacht hat.
void RemoveLearnedItems(HMENU menu) {
    if (g_excl.learnedTexts.empty()) return;
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i) {
        wchar_t text[512] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE;
        mi.dwTypeData = text;
        mi.cch = 511;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mi) || (mi.fType & MFT_SEPARATOR) || mi.wID < kShellFirst || mi.wID > kShellLast)
            continue;
        std::wstring t = CleanMenuText(text);
        if (!t.empty() && std::find(g_excl.learnedTexts.begin(), g_excl.learnedTexts.end(), t) != g_excl.learnedTexts.end())
            DeleteMenu(menu, i, MF_BYPOSITION);
    }
    // doppelte Trennlinien bereinigen
    bool lastSep = true;
    for (int i = 0; i < GetMenuItemCount(menu);) {
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_FTYPE;
        GetMenuItemInfoW(menu, i, TRUE, &mi);
        bool sep = (mi.fType & MFT_SEPARATOR) != 0;
        if (sep && lastSep) {
            DeleteMenu(menu, i, MF_BYPOSITION);
            continue;
        }
        lastSep = sep;
        ++i;
    }
    int n = GetMenuItemCount(menu);
    if (n > 0) {
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_FTYPE;
        GetMenuItemInfoW(menu, n - 1, TRUE, &mi);
        if (mi.fType & MFT_SEPARATOR) DeleteMenu(menu, n - 1, MF_BYPOSITION);
    }
}

// Nur für automatische Tests (Umgebungsvariable QFILES_TEST_MENULOG = Datei): Menüeinträge protokollieren.
void TestLogMenu(HMENU menu) {
    wchar_t path[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"QFILES_TEST_MENULOG", path, MAX_PATH)) return;
    std::wstring s = L"--- Kontextmenü ---\r\n";
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        wchar_t text[512] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_STRING | MIIM_FTYPE;
        mi.dwTypeData = text;
        mi.cch = 511;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mi)) continue;
        s += (mi.fType & MFT_SEPARATOR) ? std::wstring(L"----") : CleanMenuText(text);
        s += L"\r\n";
    }
    for (auto& e : g_excl.excluded) s += L"Ausgeschlossen: " + e + L"\r\n";
    std::vector<uint8_t> old;
    ReadFileBytes(path, old);
    std::string u = std::string(old.begin(), old.end()) + WideToUtf8(s);
    WriteFileBytes(path, u.data(), u.size());
}

#ifdef _MSC_VER
// C++-Ausnahme (0xE06D7363): Typnamen aus den ThrowInfo-Daten lesen, bei std::exception auch what().
void ReadCppException(const EXCEPTION_RECORD* er, char* types, size_t typesLen, char* what, size_t whatLen) {
    types[0] = 0;
    what[0] = 0;
    __try {
        if (er->NumberParameters < 4) return;
        const BYTE* base = (const BYTE*)er->ExceptionInformation[3];
        const int* ti = (const int*)er->ExceptionInformation[2];
        if (!base || !ti) return;
        const int* cta = (const int*)(base + ti[3]);
        int n = cta[0];
        size_t pos = 0;
        bool isStd = false;
        for (int i = 0; i < n && i < 6; ++i) {
            const int* ct = (const int*)(base + cta[1 + i]);
            const char* name = (const char*)(base + ct[1] + 2 * sizeof(void*));
            char und[256] = {};
            const char* shown = name;
            if (name[0] == '.' && UnDecorateSymbolName(name + 1, und, sizeof(und), UNDNAME_NO_ARGUMENTS | UNDNAME_32_BIT_DECODE))
                shown = und;
            if (strstr(name, "exception@std@@")) isStd = true;
            for (const char* p = (pos ? " < " : ""); *p && pos + 1 < typesLen; ++p) types[pos++] = *p;
            for (const char* p = shown; *p && pos + 1 < typesLen; ++p) types[pos++] = *p;
            types[pos] = 0;
        }
        if (isStd) {
            // MSVC: std::exception = { vftable, __std_exception_data { const char* _What; bool _DoFree; } }
            const BYTE* obj = (const BYTE*)er->ExceptionInformation[1];
            const char* w = *(const char* const*)(obj + sizeof(void*));
            if (w) {
                size_t k = 0;
                for (; w[k] && k + 1 < whatLen; ++k) what[k] = w[k];
                what[k] = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WalkStack(const CONTEXT* ctxIn, std::vector<std::wstring>& frames, std::vector<std::wstring>& paths) {
    static bool symInit = false;
    HANDLE proc = GetCurrentProcess();
    if (!symInit) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitializeW(proc, nullptr, TRUE);
        symInit = true;
    }
    CONTEXT ctx = *ctxIn;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            !frame.AddrPC.Offset)
            break;
        unsigned long long off = 0;
        std::wstring full;
        std::wstring mod = ModuleOfAddress((const void*)frame.AddrPC.Offset, &off, &full);
        frames.push_back(Format(L"%s+0x%llX", mod.c_str(), off));
        if (!full.empty() && std::find(paths.begin(), paths.end(), full) == paths.end()) paths.push_back(full);
    }
}

int FaultFilter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    {
        std::vector<std::wstring> frames, paths;
        WalkStack(ep->ContextRecord, frames, paths);
        g_lastFaultExcluded = false;
        for (auto& p : paths)
            if (!IsSystemModule(PathFileName(p)) && IsExcludedModulePath(p)) g_lastFaultExcluded = true;
    }
    if (g_fault.count++ == 0) {
        g_fault.where = g_where;
        g_fault.code = er->ExceptionCode;
        unsigned long long off = 0;
        std::wstring mod = ModuleOfAddress(er->ExceptionAddress, &off, nullptr);
        g_fault.address = Format(L"%s+0x%llX", mod.c_str(), off);
        if (er->ExceptionCode == 0xE06D7363) {
            char types[600], what[600];
            ReadCppException(er, types, sizeof(types), what, sizeof(what));
            if (types[0]) g_fault.cppInfo = L"Typ: " + Utf8ToWide(types);
            if (what[0]) g_fault.cppInfo += L"\nText: " + Utf8ToWide(what);
        } else if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2) {
            g_fault.cppInfo = Format(L"%s an Adresse 0x%llX", er->ExceptionInformation[0] == 0 ? L"Lesen" :
                                     (er->ExceptionInformation[0] == 1 ? L"Schreiben" : L"Ausführen"),
                                     (unsigned long long)er->ExceptionInformation[1]);
        }
        WalkStack(ep->ContextRecord, g_fault.stack, g_fault.stackPaths);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#define QF_TRY __try
#define QF_EXCEPT __except (FaultFilter(GetExceptionInformation()))
#else
#define QF_TRY if (true)
#define QF_EXCEPT else
#endif

// Rohaufrufe mit SEH-Schutz: nur einfache Datentypen (MSVC erlaubt __try nicht zusammen mit Objekten,
// die abgebaut werden müssen). Die Beschreibung des Aufrufs setzen die Safe…-Funktionen darüber.
HRESULT RawQuery(IContextMenu* cm, HMENU menu, UINT index, UINT flags) {
    QF_TRY { return cm->QueryContextMenu(menu, index, kShellFirst, kShellLast, flags); }
    QF_EXCEPT { return E_FAIL; }
}
HRESULT RawGetVerb(IContextMenu* cm, UINT_PTR idx, wchar_t* buf, UINT cch) {
    QF_TRY { return cm->GetCommandString(idx, GCS_VERBW, nullptr, (LPSTR)buf, cch); }
    QF_EXCEPT { return E_FAIL; }
}
HRESULT RawInvoke(IContextMenu* cm, CMINVOKECOMMANDINFOEX* ici) {
    QF_TRY { return cm->InvokeCommand((LPCMINVOKECOMMANDINFO)ici); }
    QF_EXCEPT { return E_FAIL; }
}
HRESULT RawMenuMsg2(IContextMenu3* cm, UINT msg, WPARAM wp, LPARAM lp, LRESULT* r) {
    QF_TRY { return cm->HandleMenuMsg2(msg, wp, lp, r); }
    QF_EXCEPT { return E_FAIL; }
}
HRESULT RawMenuMsg(IContextMenu2* cm, UINT msg, WPARAM wp, LPARAM lp) {
    QF_TRY { return cm->HandleMenuMsg(msg, wp, lp); }
    QF_EXCEPT { return E_FAIL; }
}
void RawRelease(IUnknown* u) {
    QF_TRY { u->Release(); }
    QF_EXCEPT {}
}
HRESULT RawGetUIObjectOf(IShellFolder* f, HWND owner, UINT n, PCUITEMID_CHILD_ARRAY a, IContextMenu** cm) {
    QF_TRY { return f->GetUIObjectOf(owner, n, a, IID_IContextMenu, nullptr, (void**)cm); }
    QF_EXCEPT { return E_FAIL; }
}

// Nur für automatische Tests (Umgebungsvariable QFILES_TEST_SHELLFAULT): simuliert eine fehlerhafte Erweiterung.
#ifdef _MSC_VER
__declspec(noinline) void ThrowTestException() { throw std::runtime_error("Testausnahme einer simulierten Erweiterung"); }
void RawTestFault() {
    QF_TRY { ThrowTestException(); }
    QF_EXCEPT {}
}
#else
void RawTestFault() {}
#endif

HRESULT SafeQueryContextMenu(IContextMenu* cm, HMENU menu, UINT index, UINT flags) {
    g_where = L"QueryContextMenu (Aufbau des Kontextmenüs)";
    int before = g_fault.count;
    if (GetEnvironmentVariableW(L"QFILES_TEST_SHELLFAULT", nullptr, 0)) RawTestFault();
    HRESULT hr = RawQuery(cm, menu, index, flags);
    if (g_fault.count != before) {
        g_fault.inQuery = true;
        g_extensionFault = true;
    }
    return hr;
}
HRESULT SafeGetVerb(IContextMenu* cm, UINT_PTR idx, wchar_t* buf, UINT cch) {
    g_where = L"GetCommandString – Menüeintrag „" + FindItemText(g_menu, (UINT)idx + kShellFirst) + L"“";
    return RawGetVerb(cm, idx, buf, cch);
}
HRESULT SafeInvoke(IContextMenu* cm, CMINVOKECOMMANDINFOEX* ici, const std::wstring& what) {
    g_where = L"InvokeCommand – Menüeintrag „" + what + L"“";
    return RawInvoke(cm, ici);
}
UINT MenuMsgItemId(UINT msg, LPARAM lp) {
    if (msg == WM_MEASUREITEM && lp) return ((MEASUREITEMSTRUCT*)lp)->itemID;
    if (msg == WM_DRAWITEM && lp) return ((DRAWITEMSTRUCT*)lp)->itemID;
    return 0;
}
// Nach einem Fehler einer ausgeschlossenen Erweiterung beim Zeichnen: Eintrag nicht mehr weiterleiten und in
// späteren Menüs dieser Sitzung ausblenden.
void LearnExcludedFault(UINT msg, LPARAM lp, int before) {
    if (g_fault.count == before || !g_lastFaultExcluded) return;
    UINT id = MenuMsgItemId(msg, lp);
    if (!id) return;
    g_skipIds.push_back(id);
    std::wstring t = FindItemText(g_menu, id);
    size_t arrow = t.rfind(L" → ");
    if (arrow != std::wstring::npos) t = t.substr(arrow + 3);
    if (!t.empty() && std::find(g_excl.learnedTexts.begin(), g_excl.learnedTexts.end(), t) == g_excl.learnedTexts.end())
        g_excl.learnedTexts.push_back(t);
}
HRESULT SafeHandleMenuMsg2(IContextMenu3* cm, UINT msg, WPARAM wp, LPARAM lp, LRESULT* r) {
    g_where = DescribeMenuMsg(msg, wp, lp);
    int before = g_fault.count;
    HRESULT hr = RawMenuMsg2(cm, msg, wp, lp, r);
    LearnExcludedFault(msg, lp, before);
    return hr;
}
HRESULT SafeHandleMenuMsg(IContextMenu2* cm, UINT msg, WPARAM wp, LPARAM lp) {
    g_where = DescribeMenuMsg(msg, wp, lp);
    int before = g_fault.count;
    HRESULT hr = RawMenuMsg(cm, msg, wp, lp);
    LearnExcludedFault(msg, lp, before);
    return hr;
}
void SafeRelease(IUnknown* u) {
    if (!u || g_extensionFault) return; // nach einem Fehler beim Aufbau das Objekt nicht mehr anfassen
    g_where = L"Release (Freigabe des Kontextmenüs)";
    RawRelease(u);
}
HRESULT SafeGetUIObjectOf(IShellFolder* f, HWND owner, UINT n, PCUITEMID_CHILD_ARRAY a, IContextMenu** cm) {
    g_where = L"GetUIObjectOf (Erzeugen des Kontextmenüs)";
    int before = g_fault.count;
    HRESULT hr = RawGetUIObjectOf(f, owner, n, a, cm);
    if (g_fault.count != before) {
        g_fault.inQuery = true;
        g_extensionFault = true;
    }
    return hr;
}

std::wstring WindowsVersion() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v{sizeof(v)};
    if (fn && fn(&v) == 0) return Format(L"Windows %lu.%lu Build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return L"Windows ?";
}

// Meldung + Protokolldatei zur abgefangenen Ausnahme
void ReportFault(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names) {
    bool excluded = false;
    for (auto& p : g_fault.stackPaths) {
        p = LongPath(p);
        if (!IsSystemModule(PathFileName(p)) && IsExcludedModulePath(p)) excluded = true;
    }
    std::wstring suspect;
    for (auto& p : g_fault.stackPaths)
        if (!IsSystemModule(PathFileName(p))) {
            suspect = p;
            break;
        }
    std::wstring r;
    r += L"Aufruf: " + g_fault.where + L"\n";
    const wchar_t* cn = ExceptionCodeName(g_fault.code);
    r += Format(L"Ausnahme: 0x%08X", g_fault.code) + (cn[0] ? (L" (" + std::wstring(cn) + L")") : L"") + L"\n";
    r += L"Fehleradresse: " + g_fault.address + L"\n";
    if (!g_fault.cppInfo.empty()) r += g_fault.cppInfo + L"\n";
    r += L"Vermutlich verursacht von: " + (suspect.empty() ? std::wstring(L"(nicht ermittelbar)") : suspect) + L"\n";
    if (g_fault.count > 1) r += L"Abgefangene Ausnahmen in diesem Menü: " + std::to_wstring(g_fault.count) + L"\n";
    r += L"Verzeichnis: " + dir + L"\n";
    if (!names.empty()) r += L"Elemente: " + Join(names, L", ") + L"\n";
    r += L"Aufrufstapel:\n";
    for (size_t i = 0; i < g_fault.stack.size() && i < 25; ++i) r += L"  " + g_fault.stack[i] + L"\n";
    r += L"Beteiligte Module:\n";
    for (auto& p : g_fault.stackPaths) r += L"  " + p + L"\n";
    if (excluded)
        r += L"Ausgeschlossene Erweiterung (Optionen → Programme): Fehler nur protokolliert, keine Meldung angezeigt.\n";
    r += WindowsVersion() + L", QFiles " QFILES_VERSION_STRING L"\n";

    // Protokolldatei (anhängen)
    std::wstring logPath = PathCombine(PathParent(App::Cfg().FilePath()), L"QFiles-Kontextmenue.txt");
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring entry = Format(L"===== %04d-%02d-%02d %02d:%02d:%02d =====\r\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                                st.wMinute, st.wSecond) +
                         ReplaceAll(r, L"\n", L"\r\n") + L"\r\n";
    std::vector<uint8_t> old;
    ReadFileBytes(logPath, old);
    std::string u = std::string(old.begin(), old.end()) + WideToUtf8(entry);
    if (old.empty()) u = "\xEF\xBB\xBF" + u;
    WriteFileBytes(logPath, u.data(), u.size());
    LogOperation(L"Kontextmenü-Erweiterung: Ausnahme abgefangen – " + g_fault.where);
    if (excluded) return;

    // Meldung: Zusammenfassung (Aufrufstapel und Modulliste vollständig nur in der Protokolldatei)
    std::wstring shortText = r.substr(0, r.find(L"Aufrufstapel:"));
    shortText += L"Module auf dem Aufrufstapel: ";
    for (size_t i = 0; i < g_fault.stackPaths.size(); ++i) shortText += (i ? L", " : L"") + PathFileName(g_fault.stackPaths[i]);
    shortText += L"\n";
    std::wstring msg = L"Eine Kontextmenü-Erweiterung eines anderen Programms hat einen Fehler verursacht. QFiles hat "
                       L"ihn abgefangen und läuft weiter.\n\n" + shortText +
                       L"\nVollständige Angaben (mit Aufrufstapel) in:\n" + logPath + L"\n(Strg+C kopiert den Text dieser Meldung.)";
    MessageBoxW(owner, msg.c_str(), L"QFiles – Fehler in Kontextmenü-Erweiterung", MB_OK | MB_ICONWARNING);
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

int ShowShellContextMenu(HWND owner, const std::wstring& dirIn, const std::vector<std::wstring>& namesIn, POINT pt,
                         HMENU extra, bool* renameRequested) {
    // Namen mit Unterverzeichnis (Ansicht „Mit Unterverzeichnissen“): gemeinsamer Ordner -> dessen Menü;
    // Elemente aus verschiedenen Ordnern -> nur die eigenen Einträge
    std::wstring dir = dirIn;
    std::vector<std::wstring> names = namesIn;
    if (!names.empty() && names[0].find(L'\\') != std::wstring::npos) {
        std::wstring sub = PathParent(PathCombine(L"X:\\", names[0]));
        bool same = true;
        std::vector<std::wstring> base;
        for (auto& n : names) {
            if (!EqualsI(PathParent(PathCombine(L"X:\\", n)), sub)) same = false;
            base.push_back(PathFileName(n));
        }
        if (same) {
            dir = PathCombine(dirIn, sub.substr(3));
            names = base;
        } else {
            dir.clear();
        }
    } else {
        for (auto& n : names)
            if (n.find(L'\\') != std::wstring::npos) dir.clear();
    }
    if (renameRequested) *renameRequested = false;
    g_fault = FaultInfo();
    g_extensionFault = false;
    g_skipIds.clear();
    ApplyExtensionExclusion();
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
    if (!cm && !dir.empty() && si.Init(owner, dir, names)) {
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
            RemoveLearnedItems(menu);
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
    g_menu = menu;
    TestLogMenu(menu);
    UINT id = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
    SafeRelease(g_cm3);
    g_cm3 = nullptr;
    SafeRelease(g_cm2);
    g_cm2 = nullptr;
    if (g_extensionFault) cm = nullptr;  // nur bei Fehler im Menüaufbau; Fehler beim Zeichnen verhindern den Befehl nicht
    std::wstring chosenText = id ? FindItemText(menu, id) : L"";
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
            SafeInvoke(cm, &ici, chosenText + (verb[0] ? (L"“, Verb „" + std::wstring(verb)) : L""));
            LogOperation(L"Kontextmenü „" + std::wstring(verb[0] ? verb : L"Befehl") + L"“ in " + dir);
        }
    }
    SafeRelease(cm);
    g_menu = nullptr;
    if (g_fault.count > 0) ReportFault(owner, dir, names);
    g_extensionFault = false;
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
    if (UINT id = MenuMsgItemId(msg, lp); id && std::find(g_skipIds.begin(), g_skipIds.end(), id) != g_skipIds.end()) {
        *result = TRUE;   // Eintrag einer ausgeschlossenen, fehlerhaften Erweiterung: nicht mehr weiterleiten
        return true;
    }
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

void PrefetchShellExtensionExclusion() {
    g_exclPrefetchOption = App::Opt().shellExtExclude;
    auto patterns = ExcludePatterns(g_exclPrefetchOption);
    if (patterns.empty()) return;
    g_exclPrefetch = std::async(std::launch::async, [patterns] { return FindExcludedExtensions(patterns); });
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
    bool nested = false;
    for (auto& n : names)
        if (n.find(L'\\') != std::wstring::npos) nested = true;
    if (nested) {
        // Elemente aus verschiedenen Unterverzeichnissen (Ansicht „Mit Unterverzeichnissen“): über absolute PIDLs
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (auto& n : names) {
            PIDLIST_ABSOLUTE p = nullptr;
            if (SUCCEEDED(SHParseDisplayName(PathCombine(dir, n).c_str(), nullptr, &p, 0, nullptr))) pidls.push_back(p);
        }
        IDataObject* data = nullptr;
        IShellItemArray* arr = nullptr;
        if (!pidls.empty() &&
            SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &arr))) {
            arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
            arr->Release();
        }
        for (auto p : pidls) CoTaskMemFree(p);
        return data;
    }
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
