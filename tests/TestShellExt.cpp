// Test-Kontextmenü-Erweiterung für den automatischen GUI-Test (nur CI, wird nicht ausgeliefert).
//
// Die DLL stellt zwei Klassen bereit, die je einen Menüeintrag anlegen:
//   {6E3A0C41-8F0B-4C55-9C1D-51A1E5F0A001} -> „QFiles-Testeintrag A“
//   {6E3A0C41-8F0B-4C55-9C1D-51A1E5F0A002} -> „QFiles-Testeintrag B“
// Der GUI-Test registriert A mit einem DLL-Pfad, der „ArchiCrypt“ enthält (muss in QFiles fehlen), und B mit einem
// unauffälligen Pfad (muss erscheinen – Nachweis, dass Erweiterungen überhaupt geladen werden).

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace {

const CLSID kClsidA = {0x6E3A0C41, 0x8F0B, 0x4C55, {0x9C, 0x1D, 0x51, 0xA1, 0xE5, 0xF0, 0xA0, 0x01}};
const CLSID kClsidB = {0x6E3A0C41, 0x8F0B, 0x4C55, {0x9C, 0x1D, 0x51, 0xA1, 0xE5, 0xF0, 0xA0, 0x02}};

LONG g_objects = 0;

class TestMenu : public IShellExtInit, public IContextMenu {
public:
    explicit TestMenu(const wchar_t* text) : text_(text) { InterlockedIncrement(&g_objects); }
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
    STDMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT, UINT flags) override {
        if (flags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        InsertMenuW(menu, index, MF_BYPOSITION | MF_STRING, first, text_);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1);
    }
    STDMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO*) override { return S_OK; }
    STDMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) override { return E_NOTIMPL; }

private:
    virtual ~TestMenu() { InterlockedDecrement(&g_objects); }
    LONG ref_ = 1;
    const wchar_t* text_;
};

class Factory : public IClassFactory {
public:
    explicit Factory(const wchar_t* text) : text_(text) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* m = new TestMenu(text_);
        HRESULT hr = m->QueryInterface(riid, ppv);
        m->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }

private:
    const wchar_t* text_;
};

Factory g_factoryA(L"QFiles-Testeintrag A");
Factory g_factoryB(L"QFiles-Testeintrag B");

} // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (clsid == kClsidA) return g_factoryA.QueryInterface(riid, ppv);
    if (clsid == kClsidB) return g_factoryB.QueryInterface(riid, ppv);
    if (ppv) *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }
