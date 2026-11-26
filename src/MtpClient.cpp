// Smartphones, Tablets, Kameras über Windows Portable Devices (WPD, Protokolle MTP und PTP) – siehe Mtp.h.
//
// Das Gerät erscheint als Dateisystem mit Pfaden "/Speicher/Ordner/Datei". WPD kennt keine Pfade, sondern
// Objekt-IDs; MtpFs bildet Pfade über einen Zwischenspeicher (Pfad -> Objekt-ID) ab, der beim Auflisten gefüllt
// wird. Das Geräteobjekt wird mit CLSID_PortableDeviceFTM erzeugt (frei threadfähig), damit Übertragungen im
// Arbeitsthread laufen können; jeder Aufruf initialisiert COM im aufrufenden Thread bei Bedarf selbst.
//
// Im MinGW-Build fehlen die WPD-Header (PortableDeviceApi.h); dort gibt es nur eine Ersatzfassung ohne Geräte.

#include "Mtp.h"
#include "RemoteFs.h"

#include <algorithm>
#include <mutex>
#include <thread>

#if (defined(_MSC_VER) || defined(QF_WPD_SYNTAX_CHECK)) && __has_include(<PortableDeviceApi.h>)
#define QF_HAVE_WPD 1
#include <objbase.h>
#include <oleauto.h>
#include <propidl.h>
#include <PortableDeviceApi.h>
#include <PortableDevice.h>
#include <wrl/client.h>
#include <functional>
#include <unordered_map>
#pragma comment(lib, "PortableDeviceGUIDs.lib")
using Microsoft::WRL::ComPtr;
#else
#define QF_HAVE_WPD 0
#endif

namespace qf {

namespace {

std::vector<MtpDevice>& DeviceCache() {
    static std::vector<MtpDevice> d;
    return d;
}
bool g_devicesRead = false;

std::wstring SafeName(std::wstring n) {
    for (auto& c : n)
        if (c == L'/' || c == L'\\' || c < 32) c = L'-';
    n = Trim(n);
    return n;
}

// "mtp://Name/…" -> "Name"
std::wstring DeviceNameOfUrl(const std::wstring& url) {
    if (!StartsWithI(url, L"mtp://")) return L"";
    std::wstring rest = url.substr(6);
    size_t slash = rest.find(L'/');
    return slash == std::wstring::npos ? rest : rest.substr(0, slash);
}

#if QF_HAVE_WPD

// COM im aufrufenden Thread (Arbeitsthreads der Übertragungen haben sonst keines)
struct ComScope {
    HRESULT hr;
    ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::vector<MtpDevice> Enumerate() {
    std::vector<MtpDevice> out;
    ComScope com;
    ComPtr<IPortableDeviceManager> mgr;
    if (FAILED(CoCreateInstance(CLSID_PortableDeviceManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr))))
        return out;
    mgr->RefreshDeviceList();
    DWORD n = 0;
    if (FAILED(mgr->GetDevices(nullptr, &n)) || n == 0) return out;
    std::vector<PWSTR> ids(n, nullptr);
    if (FAILED(mgr->GetDevices(ids.data(), &n))) return out;
    auto text = [&](PCWSTR id, HRESULT (STDMETHODCALLTYPE IPortableDeviceManager::*fn)(PCWSTR, WCHAR*, DWORD*)) {
        DWORD len = 0;
        if (FAILED((mgr.Get()->*fn)(id, nullptr, &len)) || len == 0) return std::wstring();
        std::wstring s(len, L'\0');
        if (FAILED((mgr.Get()->*fn)(id, s.data(), &len))) return std::wstring();
        s.resize(wcslen(s.c_str()));
        return s;
    };
    for (DWORD i = 0; i < n; ++i) {
        if (!ids[i]) continue;
        std::wstring id = ids[i];
        CoTaskMemFree(ids[i]);
        // Datenträger mit Laufwerksbuchstaben (WPD-Abbild von USB-Sticks/Karten) auslassen
        std::wstring low = ToLower(id);
        if (low.find(L"storage#volume") != std::wstring::npos || low.find(L"usbstor") != std::wstring::npos) continue;
        MtpDevice d;
        d.id = id;
        d.name = SafeName(text(id.c_str(), &IPortableDeviceManager::GetDeviceFriendlyName));
        d.description = text(id.c_str(), &IPortableDeviceManager::GetDeviceDescription);
        std::wstring maker = text(id.c_str(), &IPortableDeviceManager::GetDeviceManufacturer);
        if (d.name.empty()) d.name = SafeName(d.description);
        if (d.name.empty()) d.name = L"Gerät";
        if (!maker.empty() && ToLower(d.description).find(ToLower(maker)) == std::wstring::npos)
            d.description = d.description.empty() ? maker : maker + L" " + d.description;
        std::wstring all = ToLower(d.name + L" " + d.description);
        d.camera = all.find(L"camera") != std::wstring::npos || all.find(L"kamera") != std::wstring::npos;
        out.push_back(d);
    }
    // gleichnamige Geräte unterscheiden
    for (size_t i = 0; i < out.size(); ++i) {
        int k = 1;
        for (size_t j = i + 1; j < out.size(); ++j)
            if (EqualsI(out[j].name, out[i].name)) out[j].name += L" (" + std::to_wstring(++k) + L")";
    }
    return out;
}

#else

std::vector<MtpDevice> Enumerate() { return {}; }

#endif

} // namespace

bool MtpAvailable() { return QF_HAVE_WPD != 0; }

// Die Geräteliste wird nur im Hintergrund gelesen (WPD kann bei angeschlossenen, gesperrten Geräten – etwa einem
// iPhone, das auf „Vertrauen“ wartet – Sekunden brauchen); die Oberfläche arbeitet mit dem letzten Stand.
namespace {
std::mutex g_pendingMutex;
std::vector<MtpDevice> g_pending;
bool g_pendingReady = false, g_enumRunning = false, g_enumAgain = false;
HWND g_notify = nullptr;
UINT g_notifyMsg = 0;

void EnumThread() {
    for (;;) {
        std::vector<MtpDevice> now = Enumerate();
        HWND notify;
        UINT msg;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            g_pending = std::move(now);
            g_pendingReady = true;
            notify = g_notify;
            msg = g_notifyMsg;
            if (!g_enumAgain) {
                g_enumRunning = false;
                if (notify) PostMessageW(notify, msg, 0, 0);
                return;
            }
            g_enumAgain = false;
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
    }
}
} // namespace

const std::vector<MtpDevice>& MtpDevices() { return DeviceCache(); }

void MtpRefreshDevicesAsync(void* notifyHwnd, unsigned msg) {
    if (!MtpAvailable()) return;
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    g_notify = (HWND)notifyHwnd;
    g_notifyMsg = msg;
    if (g_enumRunning) {
        g_enumAgain = true;
        return;
    }
    g_enumRunning = true;
    std::thread(EnumThread).detach();
}

bool MtpTakeRefreshed() {
    std::vector<MtpDevice> now;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        if (!g_pendingReady) return false;
        g_pendingReady = false;
        now = std::move(g_pending);
    }
    bool first = !g_devicesRead;
    g_devicesRead = true;
    auto& old = DeviceCache();
    bool changed = now.size() != old.size();
    for (size_t i = 0; !changed && i < now.size(); ++i) changed = now[i].id != old[i].id || now[i].name != old[i].name;
    old = std::move(now);
    return changed || first;
}

bool MtpDevicesKnown() { return g_devicesRead; }

const MtpDevice* MtpDeviceForUrl(const std::wstring& url) {
    std::wstring name = DeviceNameOfUrl(url);
    if (name.empty()) return nullptr;
    for (auto& d : MtpDevices())
        if (EqualsI(d.name, name)) return &d;
    return nullptr;
}

std::wstring MtpDeviceUrl(const MtpDevice& d) { return L"mtp://" + d.name + L"/"; }

namespace remote {

#if QF_HAVE_WPD

namespace {

std::wstring HrText(HRESULT hr) {
    // (HRESULT_FROM_WIN32 ist im Windows-SDK eine Inline-Funktion – daher keine switch-Marken)
    auto is = [hr](DWORD code) { return hr == HRESULT_FROM_WIN32(code); };
    if (hr == E_ACCESSDENIED)
        return L"Zugriff verweigert – bitte das Gerät entsperren und dort „Dateiübertragung“ (MTP) wählen.";
    if (is(ERROR_DEVICE_NOT_CONNECTED) || is(ERROR_NOT_READY) || is(ERROR_GEN_FAILURE) || is(ERROR_BAD_COMMAND) ||
        is(ERROR_DEV_NOT_EXIST))
        return L"Das Gerät antwortet nicht (getrennt oder gesperrt?).";
    if (is(ERROR_NOT_SUPPORTED) || hr == E_NOTIMPL) return L"Das Gerät unterstützt diese Funktion nicht.";
    if (is(ERROR_FILE_NOT_FOUND) || is(ERROR_NOT_FOUND)) return L"Nicht gefunden.";
    if (is(ERROR_DISK_FULL)) return L"Auf dem Gerät ist nicht genug Platz.";
    if (is(ERROR_CANCELLED)) return L"Abgebrochen.";
    if (is(ERROR_TIMEOUT)) return L"Zeitüberschreitung – das Gerät antwortet nicht.";
    std::wstring m = LastErrorMessage((DWORD)hr);
    return m.empty() ? Format(L"Fehler 0x%08X", (unsigned)hr) : m;
}

bool LostConnection(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) || hr == HRESULT_FROM_WIN32(ERROR_NOT_READY) ||
           hr == HRESULT_FROM_WIN32(ERROR_GEN_FAILURE) || hr == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) ||
           hr == HRESULT_FROM_WIN32(ERROR_DEV_NOT_EXIST) || hr == E_HANDLE;
}

// VT_DATE (Ortszeit des Geräts) <-> FILETIME (UTC)
bool DateToFileTime(double date, FILETIME& ft) {
    SYSTEMTIME local{}, utc{};
    if (!VariantTimeToSystemTime(date, &local)) return false;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return false;
    return SystemTimeToFileTime(&utc, &ft) != FALSE;
}

bool FileTimeToDate(const FILETIME& ft, double& date) {
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return false;
    return SystemTimeToVariantTime(&local, &date) != FALSE;
}

struct ObjInfo {
    std::wstring id, name;
    bool isDir = false, hidden = false;
    uint64_t size = 0;
    FILETIME modified{}, created{};
};

ObjInfo InfoFromValues(IPortableDeviceValues* v) {
    ObjInfo o;
    LPWSTR s = nullptr;
    if (SUCCEEDED(v->GetStringValue(WPD_OBJECT_ID, &s)) && s) {
        o.id = s;
        CoTaskMemFree(s);
        s = nullptr;
    }
    if (SUCCEEDED(v->GetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, &s)) && s) {
        o.name = s;
        CoTaskMemFree(s);
        s = nullptr;
    }
    if (o.name.empty() && SUCCEEDED(v->GetStringValue(WPD_OBJECT_NAME, &s)) && s) {
        o.name = s;
        CoTaskMemFree(s);
        s = nullptr;
    }
    if (o.name.empty()) o.name = o.id;
    o.name = SafeName(o.name);
    GUID ct{};
    if (SUCCEEDED(v->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &ct)))
        o.isDir = IsEqualGUID(ct, WPD_CONTENT_TYPE_FOLDER) || IsEqualGUID(ct, WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT);
    ULONGLONG sz = 0;
    if (SUCCEEDED(v->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, &sz))) o.size = sz;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(v->GetValue(WPD_OBJECT_DATE_MODIFIED, &pv)) && pv.vt == VT_DATE) DateToFileTime(pv.date, o.modified);
    PropVariantClear(&pv);
    PropVariantInit(&pv);
    if (SUCCEEDED(v->GetValue(WPD_OBJECT_DATE_CREATED, &pv)) && pv.vt == VT_DATE) DateToFileTime(pv.date, o.created);
    PropVariantClear(&pv);
    if (o.created.dwLowDateTime == 0 && o.created.dwHighDateTime == 0) o.created = o.modified;
    BOOL hidden = FALSE;
    if (SUCCEEDED(v->GetBoolValue(WPD_OBJECT_ISHIDDEN, &hidden))) o.hidden = hidden != FALSE;
    return o;
}

ComPtr<IPortableDeviceKeyCollection> InfoKeys() {
    ComPtr<IPortableDeviceKeyCollection> keys;
    if (FAILED(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&keys))))
        return nullptr;
    const PROPERTYKEY* list[] = {&WPD_OBJECT_ID, &WPD_OBJECT_NAME, &WPD_OBJECT_ORIGINAL_FILE_NAME, &WPD_OBJECT_CONTENT_TYPE,
                                 &WPD_OBJECT_SIZE, &WPD_OBJECT_DATE_MODIFIED, &WPD_OBJECT_DATE_CREATED, &WPD_OBJECT_ISHIDDEN};
    for (auto k : list) keys->Add(*k);
    return keys;
}

// Rückruf für das gesammelte Lesen der Eigenschaften (IPortableDevicePropertiesBulk)
class BulkCallback final : public IPortableDevicePropertiesBulkCallback {
public:
    explicit BulkCallback(std::vector<ObjInfo>& out) : out_(out) { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~BulkCallback() {
        if (done_) CloseHandle(done_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPortableDevicePropertiesBulkCallback) {
            *ppv = static_cast<IPortableDevicePropertiesBulkCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE OnStart(REFGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(REFGUID, IPortableDeviceValuesCollection* values) override {
        DWORD n = 0;
        if (!values || FAILED(values->GetCount(&n))) return S_OK;
        for (DWORD i = 0; i < n; ++i) {
            ComPtr<IPortableDeviceValues> v;
            if (SUCCEEDED(values->GetAt(i, &v)) && v) out_.push_back(InfoFromValues(v.Get()));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEnd(REFGUID, HRESULT status) override {
        status_ = status;
        SetEvent(done_);
        return S_OK;
    }
    HANDLE Done() const { return done_; }
    HRESULT Status() const { return status_; }

private:
    LONG ref_ = 1;
    std::vector<ObjInfo>& out_;
    HANDLE done_ = nullptr;
    HRESULT status_ = S_OK;
};

std::wstring ParentPath(const std::wstring& p) {
    size_t pos = p.find_last_of(L'/');
    if (pos == std::wstring::npos || pos == 0) return L"/";
    return p.substr(0, pos);
}
std::wstring LeafName(const std::wstring& p) {
    size_t pos = p.find_last_of(L'/');
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}
std::wstring ChildPath(const std::wstring& dir, const std::wstring& name) {
    return dir == L"/" ? L"/" + name : dir + L"/" + name;
}

class MtpFs : public RemoteFs {
public:
    ~MtpFs() override { Disconnect(); }

    bool Connect(const ConnectSettings& s, const ConnectPrompts*, std::wstring& err) override {
        ComScope com;
        settings = s;
        Disconnect();
        // eigene Aufzählung (kann im Arbeitsthread laufen – die Liste der Oberfläche bleibt unberührt)
        std::vector<MtpDevice> devices = Enumerate();
        const MtpDevice* d = nullptr;
        for (auto& x : devices)
            if (EqualsI(x.name, s.url.host)) d = &x;
        if (!d) {
            err = L"Das Gerät „" + s.url.host +
                  L"“ ist nicht angeschlossen (bzw. nicht im Modus „Dateiübertragung“ oder gesperrt).";
            return false;
        }
        deviceId_ = d->id;
        ComPtr<IPortableDeviceValues> client;
        if (FAILED(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client)))) {
            err = L"Windows Portable Devices ist nicht verfügbar.";
            return false;
        }
        client->SetStringValue(WPD_CLIENT_NAME, L"QFiles");
        client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
        client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
        client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
        client->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION);
        ComPtr<IPortableDevice> dev;
        HRESULT hr = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dev));
        if (SUCCEEDED(hr)) hr = dev->Open(deviceId_.c_str(), client.Get());
        if (hr == E_ACCESSDENIED) {
            // nur lesend (manche Geräte erlauben keinen Schreibzugriff)
            client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS, GENERIC_READ);
            hr = dev->Open(deviceId_.c_str(), client.Get());
            readOnly_ = SUCCEEDED(hr);
        }
        if (FAILED(hr)) {
            err = L"„" + s.url.host + L"“: " + HrText(hr);
            return false;
        }
        dev_ = dev;
        ids_.clear();
        return true;
    }
    bool Connected() const override { return dev_ != nullptr; }
    void Disconnect() override {
        if (dev_) {
            ComScope com;
            dev_->Close();
            dev_.Reset();
        }
        ids_.clear();
    }
    std::wstring HomeDir() override { return L"/"; }

    bool List(const std::wstring& path, std::vector<DirEntry>& out, std::wstring& err) override {
        ComScope com;
        out.clear();
        std::wstring id;
        if (!Resolve(path, id, err)) return false;
        std::vector<ObjInfo> kids;
        if (!Children(id, kids, err)) return false;
        // Zwischenspeicher dieses Verzeichnisses erneuern
        std::wstring prefix = path == L"/" ? L"/" : path + L"/";
        for (auto it = ids_.begin(); it != ids_.end();) {
            const std::wstring& k = it->first;
            if (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0 &&
                k.find(L'/', prefix.size()) == std::wstring::npos)
                it = ids_.erase(it);
            else
                ++it;
        }
        for (auto& o : kids) {
            ids_[ChildPath(path, o.name)] = o.id;
            DirEntry e;
            e.name = o.name;
            e.attributes = o.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (o.hidden) e.attributes |= FILE_ATTRIBUTE_HIDDEN;
            if (readOnly_) e.attributes |= FILE_ATTRIBUTE_READONLY;
            e.size = o.isDir ? 0 : o.size;
            e.modified = o.modified;
            e.created = o.created;
            e.accessed = o.modified;
            out.push_back(e);
        }
        if (path == L"/" && out.empty()) {
            err = L"Auf dem Gerät ist kein Speicher sichtbar – bitte das Gerät entsperren und dort „Dateiübertragung“ "
                  L"(MTP) wählen, dann aktualisieren (F5).";
            return false;
        }
        return true;
    }

    bool Stat(const std::wstring& path, DirEntry& out) override {
        ComScope com;
        out = DirEntry{};
        if (path == L"/") {
            out.name = settings.url.host;
            out.attributes = FILE_ATTRIBUTE_DIRECTORY;
            return Connected();
        }
        ObjInfo o;
        std::wstring err;
        if (!Info(path, o, err)) return false;
        out.name = o.name;
        out.attributes = o.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        out.size = o.isDir ? 0 : o.size;
        out.modified = out.accessed = o.modified;
        out.created = o.created;
        return true;
    }

    bool Download(const std::wstring& path, HANDLE localFile, const ProgressFn& progress, std::wstring& err) override {
        ComScope com;
        ObjInfo o;
        if (!Info(path, o, err)) return false;
        if (o.isDir) {
            err = L"„" + o.name + L"“ ist ein Ordner.";
            return false;
        }
        ComPtr<IPortableDeviceContent> content;
        ComPtr<IPortableDeviceResources> res;
        HRESULT hr = dev_->Content(&content);
        if (SUCCEEDED(hr)) hr = content->Transfer(&res);
        DWORD optimal = 0;
        ComPtr<IStream> stream;
        if (SUCCEEDED(hr)) hr = res->GetStream(o.id.c_str(), WPD_RESOURCE_DEFAULT, STGM_READ, &optimal, &stream);
        if (FAILED(hr)) return Fail(hr, o.name, err);
        DWORD chunk = std::clamp<DWORD>(optimal ? optimal : 262144, 65536, 4u << 20);
        std::vector<BYTE> buf(chunk);
        uint64_t done = 0;
        for (;;) {
            ULONG got = 0;
            hr = stream->Read(buf.data(), chunk, &got);
            if (FAILED(hr)) return Fail(hr, o.name, err);
            if (got == 0) break;
            DWORD written = 0;
            if (!WriteFile(localFile, buf.data(), got, &written, nullptr) || written != got) {
                err = LastErrorMessage();
                return false;
            }
            done += got;
            if (progress && !progress(done)) {
                err = L"Abgebrochen.";
                return false;
            }
            if (hr == S_FALSE) break;
        }
        return true;
    }

    bool Upload(HANDLE localFile, const std::wstring& path, const ProgressFn& progress, std::wstring& err) override {
        ComScope com;
        if (!Writable(err)) return false;
        std::wstring parent = ParentPath(path), name = LeafName(path);
        if (parent == L"/") {
            err = L"Dateien bitte in einen Speicher des Geräts (z. B. „Interner Speicher“) legen, nicht in die oberste Ebene.";
            return false;
        }
        std::wstring pid;
        if (!Resolve(parent, pid, err)) return false;
        // Vorhandene gleichnamige Datei ersetzen (WPD legt sonst eine zweite gleichnamige an)
        std::vector<ObjInfo> kids;
        if (Children(pid, kids, err)) {
            for (auto& k : kids) {
                if (k.name != name) continue;
                if (k.isDir) {
                    err = L"„" + name + L"“ ist auf dem Gerät ein Ordner.";
                    return false;
                }
                if (!DeleteIds({k.id}, false, err)) return false;
            }
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(localFile, &size);
        FILETIME mtime{};
        GetFileTime(localFile, nullptr, nullptr, &mtime);
        ComPtr<IStream> stream;
        DWORD optimal = 0;
        HRESULT hr = E_FAIL;
        for (int attempt = 0; attempt < 2 && !stream; ++attempt) {
            ComPtr<IPortableDeviceValues> v;
            if (FAILED(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&v)))) break;
            v->SetStringValue(WPD_OBJECT_PARENT_ID, pid.c_str());
            v->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, name.c_str());
            v->SetStringValue(WPD_OBJECT_NAME, name.c_str());
            v->SetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, (ULONGLONG)size.QuadPart);
            v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, WPD_CONTENT_TYPE_GENERIC_FILE);
            v->SetGuidValue(WPD_OBJECT_FORMAT, WPD_OBJECT_FORMAT_UNSPECIFIED);
            double date = 0;
            if (attempt == 0 && FileTimeToDate(mtime, date)) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                pv.vt = VT_DATE;
                pv.date = date;
                v->SetValue(WPD_OBJECT_DATE_MODIFIED, &pv);
            }
            ComPtr<IPortableDeviceContent> content;
            hr = dev_->Content(&content);
            if (SUCCEEDED(hr)) hr = content->CreateObjectWithPropertiesAndData(v.Get(), &stream, &optimal, nullptr);
            if (FAILED(hr)) stream.Reset();   // zweiter Versuch ohne Änderungsdatum
        }
        if (!stream) return Fail(hr, name, err);
        DWORD chunk = std::clamp<DWORD>(optimal ? optimal : 262144, 65536, 4u << 20);
        std::vector<BYTE> buf(chunk);
        uint64_t done = 0;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(localFile, buf.data(), chunk, &got, nullptr)) {
                err = LastErrorMessage();
                stream->Revert();
                return false;
            }
            if (got == 0) break;
            ULONG written = 0;
            hr = stream->Write(buf.data(), got, &written);
            if (FAILED(hr) || written != got) {
                stream->Revert();
                return Fail(FAILED(hr) ? hr : E_FAIL, name, err);
            }
            done += got;
            if (progress && !progress(done)) {
                stream->Revert();
                err = L"Abgebrochen.";
                return false;
            }
        }
        hr = stream->Commit(STGC_DEFAULT);
        if (FAILED(hr)) return Fail(hr, name, err);
        ComPtr<IPortableDeviceDataStream> ds;
        LPWSTR newId = nullptr;
        if (SUCCEEDED(stream.As(&ds)) && SUCCEEDED(ds->GetObjectID(&newId)) && newId) {
            ids_[path] = newId;
            CoTaskMemFree(newId);
        } else {
            ids_.erase(path);
        }
        return true;
    }

    bool SetModTime(const std::wstring& path, const FILETIME& mtime) override {
        ComScope com;
        ObjInfo o;
        std::wstring err;
        double date = 0;
        if (!Info(path, o, err) || !FileTimeToDate(mtime, date)) return false;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_DATE;
        pv.date = date;
        return SetProp(o.id, WPD_OBJECT_DATE_MODIFIED, pv, err);
    }

    bool DeleteFile(const std::wstring& path, std::wstring& err) override {
        ComScope com;
        if (!Writable(err)) return false;
        ObjInfo o;
        if (!Info(path, o, err)) return false;
        if (!DeleteIds({o.id}, false, err)) return false;
        Forget(path);
        return true;
    }
    bool RemoveDir(const std::wstring& path, std::wstring& err) override {
        ComScope com;
        if (!Writable(err)) return false;
        ObjInfo o;
        if (!Info(path, o, err)) return false;
        // Inhalte löscht der Aufrufer vorher einzeln (wie bei FTP); leere Ordner ohne Rekursion entfernen
        if (!DeleteIds({o.id}, false, err) && !DeleteIds({o.id}, true, err)) return false;
        Forget(path);
        return true;
    }
    bool MakeDir(const std::wstring& path, std::wstring& err) override {
        ComScope com;
        if (!Writable(err)) return false;
        std::wstring parent = ParentPath(path), name = LeafName(path);
        if (parent == L"/") {
            err = L"In der obersten Ebene des Geräts lassen sich keine Ordner anlegen.";
            return false;
        }
        std::wstring pid;
        if (!Resolve(parent, pid, err)) return false;
        ComPtr<IPortableDeviceValues> v;
        if (FAILED(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&v)))) return false;
        v->SetStringValue(WPD_OBJECT_PARENT_ID, pid.c_str());
        v->SetStringValue(WPD_OBJECT_NAME, name.c_str());
        v->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, name.c_str());
        v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, WPD_CONTENT_TYPE_FOLDER);
        v->SetGuidValue(WPD_OBJECT_FORMAT, WPD_OBJECT_FORMAT_PROPERTIES_ONLY);
        ComPtr<IPortableDeviceContent> content;
        LPWSTR newId = nullptr;
        HRESULT hr = dev_->Content(&content);
        if (SUCCEEDED(hr)) hr = content->CreateObjectWithPropertiesOnly(v.Get(), &newId);
        if (FAILED(hr)) return Fail(hr, name, err);
        if (newId) {
            ids_[path] = newId;
            CoTaskMemFree(newId);
        }
        return true;
    }
    bool Rename(const std::wstring& from, const std::wstring& to, std::wstring& err) override {
        ComScope com;
        if (!Writable(err)) return false;
        ObjInfo o;
        if (!Info(from, o, err)) return false;
        std::wstring fromParent = ParentPath(from), toParent = ParentPath(to);
        if (fromParent != toParent) {
            // Verschieben auf dem Gerät (nur wenn das Gerät es kann; sonst kopiert der Aufrufer)
            std::wstring dest;
            if (!Resolve(toParent, dest, err)) return false;
            ComPtr<IPortableDeviceContent> content;
            ComPtr<IPortableDevicePropVariantCollection> coll, results;
            HRESULT hr = dev_->Content(&content);
            if (SUCCEEDED(hr)) hr = MakeIdCollection({o.id}, coll);
            if (SUCCEEDED(hr)) hr = content->Move(coll.Get(), dest.c_str(), &results);
            if (FAILED(hr)) return Fail(hr, o.name, err);
            Forget(from);
        }
        if (LeafName(to) != o.name) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_LPWSTR;
            std::wstring n = LeafName(to);
            pv.pwszVal = n.data();
            if (!SetProp(o.id, WPD_OBJECT_ORIGINAL_FILE_NAME, pv, err) && !SetProp(o.id, WPD_OBJECT_NAME, pv, err)) {
                err = L"Das Gerät erlaubt das Umbenennen von „" + o.name + L"“ nicht.";
                return false;
            }
        }
        Forget(from);
        ids_[to] = o.id;
        return true;
    }

private:
    ComPtr<IPortableDevice> dev_;
    std::wstring deviceId_;
    bool readOnly_ = false;
    std::unordered_map<std::wstring, std::wstring> ids_;   // Pfad -> Objekt-ID

    bool Fail(HRESULT hr, const std::wstring& what, std::wstring& err) {
        err = (what.empty() ? L"" : L"„" + what + L"“: ") + HrText(hr);
        if (LostConnection(hr)) {
            dev_.Reset();   // WithRetry verbindet neu
            ids_.clear();
        }
        return false;
    }
    bool Writable(std::wstring& err) {
        if (!dev_) {
            err = L"Keine Verbindung zum Gerät.";
            return false;
        }
        if (readOnly_) {
            err = L"Das Gerät erlaubt nur Lesezugriff.";
            return false;
        }
        return true;
    }
    void Forget(const std::wstring& path) {
        std::wstring prefix = path + L"/";
        for (auto it = ids_.begin(); it != ids_.end();) {
            if (it->first == path || (it->first.size() > prefix.size() && it->first.compare(0, prefix.size(), prefix) == 0))
                it = ids_.erase(it);
            else
                ++it;
        }
    }
    static HRESULT MakeIdCollection(const std::vector<std::wstring>& ids, ComPtr<IPortableDevicePropVariantCollection>& coll) {
        HRESULT hr = CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&coll));
        if (FAILED(hr)) return hr;
        for (auto& id : ids) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_LPWSTR;
            pv.pwszVal = const_cast<LPWSTR>(id.c_str());
            hr = coll->Add(&pv);   // Add kopiert den Wert
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }
    bool DeleteIds(const std::vector<std::wstring>& ids, bool recursive, std::wstring& err) {
        ComPtr<IPortableDeviceContent> content;
        ComPtr<IPortableDevicePropVariantCollection> coll;
        HRESULT hr = dev_->Content(&content);
        if (SUCCEEDED(hr)) hr = MakeIdCollection(ids, coll);
        if (SUCCEEDED(hr))
            hr = content->Delete(recursive ? PORTABLE_DEVICE_DELETE_WITH_RECURSION : PORTABLE_DEVICE_DELETE_NO_RECURSION,
                                 coll.Get(), nullptr);
        return SUCCEEDED(hr) || Fail(hr, L"", err);
    }
    bool SetProp(const std::wstring& id, const PROPERTYKEY& key, const PROPVARIANT& value, std::wstring& err) {
        ComPtr<IPortableDeviceContent> content;
        ComPtr<IPortableDeviceProperties> props;
        ComPtr<IPortableDeviceValues> v, results;
        HRESULT hr = dev_->Content(&content);
        if (SUCCEEDED(hr)) hr = content->Properties(&props);
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&v));
        if (SUCCEEDED(hr)) hr = v->SetValue(key, &value);
        if (SUCCEEDED(hr)) hr = props->SetValues(id.c_str(), v.Get(), &results);
        if (SUCCEEDED(hr) && results) {
            HRESULT one = S_OK;
            if (SUCCEEDED(results->GetErrorValue(key, &one)) && FAILED(one)) hr = one;
        }
        return SUCCEEDED(hr) || Fail(hr, L"", err);
    }

    // Kinder eines Objekts mit Eigenschaften (gesammelt über PropertiesBulk, sonst einzeln)
    bool Children(const std::wstring& parentId, std::vector<ObjInfo>& out, std::wstring& err) {
        out.clear();
        if (!dev_) {
            err = L"Keine Verbindung zum Gerät.";
            return false;
        }
        ComPtr<IPortableDeviceContent> content;
        ComPtr<IEnumPortableDeviceObjectIDs> en;
        HRESULT hr = dev_->Content(&content);
        if (SUCCEEDED(hr)) hr = content->EnumObjects(0, parentId.c_str(), nullptr, &en);
        if (FAILED(hr)) return Fail(hr, L"", err);
        std::vector<std::wstring> ids;
        for (;;) {
            PWSTR batch[128] = {};
            ULONG got = 0;
            hr = en->Next(128, batch, &got);
            for (ULONG i = 0; i < got; ++i) {
                if (batch[i]) ids.push_back(batch[i]);
                CoTaskMemFree(batch[i]);
            }
            if (FAILED(hr)) return Fail(hr, L"", err);
            if (hr == S_FALSE || got == 0) break;
        }
        if (ids.empty()) return true;
        ComPtr<IPortableDeviceProperties> props;
        hr = content->Properties(&props);
        if (FAILED(hr)) return Fail(hr, L"", err);
        auto keys = InfoKeys();
        if (!keys) {
            err = L"Windows Portable Devices ist nicht verfügbar.";
            return false;
        }
        // gesammelt
        ComPtr<IPortableDevicePropertiesBulk> bulk;
        if (ids.size() > 4 && SUCCEEDED(props.As(&bulk))) {
            ComPtr<IPortableDevicePropVariantCollection> coll;
            if (SUCCEEDED(MakeIdCollection(ids, coll))) {
                auto* cb = new BulkCallback(out);
                GUID ctx{};
                hr = bulk->QueueGetValuesByObjectList(coll.Get(), keys.Get(), cb, &ctx);
                if (SUCCEEDED(hr)) hr = bulk->Start(ctx);
                if (SUCCEEDED(hr)) {
                    HANDLE h = cb->Done();
                    DWORD idx = 0;
                    HRESULT w = CoWaitForMultipleHandles(0, 120000, 1, &h, &idx);
                    if (w == RPC_S_CALLPENDING) {
                        bulk->Cancel(ctx);
                        WaitForSingleObject(h, 5000);
                    }
                    hr = w == RPC_S_CALLPENDING ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : cb->Status();
                }
                cb->Release();
                if (SUCCEEDED(hr) && out.size() == ids.size()) return true;
                out.clear();   // nicht vollständig: einzeln lesen
            }
        }
        for (auto& id : ids) {
            ComPtr<IPortableDeviceValues> v;
            hr = props->GetValues(id.c_str(), keys.Get(), &v);
            if (FAILED(hr)) {
                if (LostConnection(hr)) return Fail(hr, L"", err);
                continue;
            }
            ObjInfo o = InfoFromValues(v.Get());
            if (o.id.empty()) o.id = id;
            out.push_back(o);
        }
        return true;
    }

    // Pfad -> Objekt-ID (über den Zwischenspeicher, fehlende Ebenen werden aufgelistet)
    bool Resolve(const std::wstring& path, std::wstring& id, std::wstring& err) {
        if (!dev_) {
            err = L"Keine Verbindung zum Gerät.";
            return false;
        }
        if (path.empty() || path == L"/") {
            id = WPD_DEVICE_OBJECT_ID;
            return true;
        }
        auto it = ids_.find(path);
        if (it != ids_.end()) {
            id = it->second;
            return true;
        }
        std::wstring parent = ParentPath(path), name = LeafName(path), pid;
        if (!Resolve(parent, pid, err)) return false;
        std::vector<ObjInfo> kids;
        if (!Children(pid, kids, err)) return false;
        const ObjInfo* match = nullptr;
        for (auto& k : kids) {
            ids_[ChildPath(parent, k.name)] = k.id;
            if (k.name == name) match = &k;
        }
        if (!match)
            for (auto& k : kids)
                if (EqualsI(k.name, name)) match = &k;
        if (!match) {
            err = L"„" + name + L"“ wurde auf dem Gerät nicht gefunden.";
            return false;
        }
        id = match->id;
        return true;
    }

    // Eigenschaften eines Pfads; bei veraltetem Zwischenspeicher einmal neu auflösen
    bool Info(const std::wstring& path, ObjInfo& o, std::wstring& err) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::wstring id;
            if (!Resolve(path, id, err)) return false;
            ComPtr<IPortableDeviceContent> content;
            ComPtr<IPortableDeviceProperties> props;
            ComPtr<IPortableDeviceValues> v;
            auto keys = InfoKeys();
            HRESULT hr = dev_->Content(&content);
            if (SUCCEEDED(hr)) hr = content->Properties(&props);
            if (SUCCEEDED(hr) && keys) hr = props->GetValues(id.c_str(), keys.Get(), &v);
            if (SUCCEEDED(hr) && v) {
                o = InfoFromValues(v.Get());
                if (o.id.empty()) o.id = id;
                return true;
            }
            if (LostConnection(hr)) return Fail(hr, LeafName(path), err);
            Forget(path);
            err = L"„" + LeafName(path) + L"“: " + HrText(hr);
        }
        return false;
    }
};

} // namespace

std::unique_ptr<RemoteFs> CreateMtpFs() { return std::make_unique<MtpFs>(); }

#else

namespace {
class MtpFsStub : public RemoteFs {
public:
    bool Connect(const ConnectSettings&, const ConnectPrompts*, std::wstring& err) override {
        err = L"Geräte (MTP) werden in diesem Build nicht unterstützt.";
        return false;
    }
    bool Connected() const override { return false; }
    void Disconnect() override {}
    std::wstring HomeDir() override { return L"/"; }
    bool List(const std::wstring&, std::vector<DirEntry>&, std::wstring& err) override { return No(err); }
    bool Stat(const std::wstring&, DirEntry&) override { return false; }
    bool Download(const std::wstring&, HANDLE, const ProgressFn&, std::wstring& err) override { return No(err); }
    bool Upload(HANDLE, const std::wstring&, const ProgressFn&, std::wstring& err) override { return No(err); }
    bool SetModTime(const std::wstring&, const FILETIME&) override { return false; }
    bool DeleteFile(const std::wstring&, std::wstring& err) override { return No(err); }
    bool RemoveDir(const std::wstring&, std::wstring& err) override { return No(err); }
    bool MakeDir(const std::wstring&, std::wstring& err) override { return No(err); }
    bool Rename(const std::wstring&, const std::wstring&, std::wstring& err) override { return No(err); }

private:
    static bool No(std::wstring& err) {
        err = L"Geräte (MTP) werden in diesem Build nicht unterstützt.";
        return false;
    }
};
} // namespace

std::unique_ptr<RemoteFs> CreateMtpFs() { return std::make_unique<MtpFsStub>(); }

#endif

} // namespace remote

} // namespace qf
