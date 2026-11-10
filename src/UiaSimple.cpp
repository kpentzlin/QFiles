// Vereinfachte UI-Automation-Darstellung (siehe UiaSimple.h). UIAutomationCore.dll wird dynamisch geladen.

#include "UiaSimple.h"

#include <ole2.h>
#include <oleacc.h>
#include <uiautomation.h>

namespace qf {

namespace {

using ReturnRawFn = LRESULT(WINAPI*)(HWND, WPARAM, LPARAM, IRawElementProviderSimple*);
using HostFromHwndFn = HRESULT(WINAPI*)(HWND, IRawElementProviderSimple**);

struct UiaApi {
    ReturnRawFn returnRaw = nullptr;
    HostFromHwndFn hostFromHwnd = nullptr;
    UiaApi() {
        if (HMODULE m = LoadLibraryW(L"UIAutomationCore.dll")) {
            returnRaw = (ReturnRawFn)(void*)GetProcAddress(m, "UiaReturnRawElementProvider");
            hostFromHwnd = (HostFromHwndFn)(void*)GetProcAddress(m, "UiaHostProviderFromHwnd");
        }
    }
    bool Ok() const { return returnRaw && hostFromHwnd; }
};

bool g_used = false;   // mindestens einmal beantwortet

UiaApi& Api() {
    static UiaApi api;
    return api;
}

// Ein Element ohne Kinder: Art, Name, Fokus; Lage und Fensterdaten liefert der Host-Provider des Fensters
class SimpleProvider : public IRawElementProviderSimple {
public:
    SimpleProvider(HWND h, std::wstring name, long controlType) : hwnd_(h), name_(std::move(name)), type_(controlType) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *out = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* ret) override {
        if (!ret) return E_POINTER;
        *ret = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** ret) override {
        if (!ret) return E_POINTER;
        *ret = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override {
        if (!v) return E_POINTER;
        VariantInit(v);
        switch (id) {
        case UIA_ControlTypePropertyId:
            v->vt = VT_I4;
            v->lVal = type_;
            break;
        case UIA_NamePropertyId:
            v->vt = VT_BSTR;
            v->bstrVal = SysAllocString(name_.c_str());
            break;
        case UIA_IsKeyboardFocusablePropertyId:
            v->vt = VT_BOOL;
            v->boolVal = VARIANT_TRUE;
            break;
        case UIA_HasKeyboardFocusPropertyId:
            v->vt = VT_BOOL;
            v->boolVal = GetFocus() == hwnd_ ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_HelpTextPropertyId:
            v->vt = VT_BSTR;
            v->bstrVal = SysAllocString(L"Vereinfachte Darstellung (QFiles-Option „Bedienungshilfen vereinfachen“)");
            break;
        default:
            break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
        if (!ret) return E_POINTER;
        *ret = nullptr;
        return Api().hostFromHwnd ? Api().hostFromHwnd(hwnd_, ret) : S_OK;
    }

private:
    virtual ~SimpleProvider() = default;
    LONG refs_ = 1;
    HWND hwnd_;
    std::wstring name_;
    long type_;
};

} // namespace

bool SimpleUiaGetObject(HWND h, WPARAM wp, LPARAM lp, const std::wstring& name, long controlType, LRESULT& result) {
    // UiaRootObjectId: UI-Automation-Clients; OBJID_CLIENT: MSAA-Clients (über die Brücke aus demselben Provider)
    LONG id = (LONG)lp;
    if (id != UiaRootObjectId && id != OBJID_CLIENT) return false;
    if (!Api().Ok()) return false;
    g_used = true;
    auto* p = new SimpleProvider(h, name, controlType);
    result = Api().returnRaw(h, wp, lp, p);
    p->Release();
    return true;
}

void SimpleUiaDisconnect(HWND h) {
    if (g_used && Api().returnRaw) Api().returnRaw(h, 0, 0, nullptr);
}

} // namespace qf
