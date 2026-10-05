#include "Network.h"
#include "Location.h"
#include "Util.h"

#include <windows.h>
#include <lm.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winnetwk.h>
#include <algorithm>
#include <thread>
#include <vector>

#undef PathCombine
#undef StrToInt

namespace qf {

namespace {

// Gemeldete Namen (ohne Groß-/Kleinschreibung) nur einmal weitergeben
class Deduplicator {
public:
    bool Add(const std::wstring& name) {
        std::wstring l = ToLower(name);
        if (std::find(seen_.begin(), seen_.end(), l) != seen_.end()) return false;
        seen_.push_back(l);
        return true;
    }

private:
    std::vector<std::wstring> seen_;
};

// "\\server\share\x" -> {"server", "share"}
bool SplitUnc(const std::wstring& unc, std::wstring& server, std::wstring& share) {
    if (!StartsWithI(unc, L"\\\\")) return false;
    auto parts = Split(unc.substr(2), L'\\');
    if (parts.empty()) return false;
    server = parts[0];
    share = parts.size() > 1 ? parts[1] : L"";
    return true;
}

// Verbundene Netzlaufwerke: (Server, Freigabe)
std::vector<std::pair<std::wstring, std::wstring>> MappedShares() {
    std::vector<std::pair<std::wstring, std::wstring>> r;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        std::wstring target = MappedDriveTarget(root);
        std::wstring server, share;
        if (SplitUnc(target, server, share)) r.push_back({server, share});
    }
    return r;
}

std::wstring StrRetText(STRRET& sr, PCUITEMID_CHILD child) {
    wchar_t buf[MAX_PATH * 2] = {};
    if (FAILED(StrRetToBufW(&sr, child, buf, (UINT)std::size(buf)))) return L"";
    return buf;
}

// Kinder eines Shell-Ordners (Netzwerk bzw. \\Server): (Analysename, Anzeigename)
void EnumShellFolder(PIDLIST_ABSOLUTE pidl, const NetworkScan& scan,
                     const std::function<void(const std::wstring&, const std::wstring&)>& cb) {
    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop))) return;
    IShellFolder* folder = nullptr;
    HRESULT hr = desktop->BindToObject(pidl, nullptr, IID_PPV_ARGS(&folder));
    desktop->Release();
    if (FAILED(hr) || !folder) return;
    IEnumIDList* e = nullptr;
    if (folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e) == S_OK && e) {
        PITEMID_CHILD child = nullptr;
        while (!scan.cancel && e->Next(1, &child, nullptr) == S_OK) {
            STRRET sp{}, sn{};
            std::wstring parsing, display;
            if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &sp))) parsing = StrRetText(sp, child);
            if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &sn))) display = StrRetText(sn, child);
            CoTaskMemFree(child);
            cb(parsing, display);
        }
        e->Release();
    }
    folder->Release();
}

// WNet-Aufzählung (klassische Netzwerkumgebung, Arbeitsgruppen/Domänen)
void EnumWNet(LPNETRESOURCEW container, int depth, const NetworkScan& scan,
              const std::function<void(const std::wstring&, const std::wstring&)>& server) {
    if (depth > 4 || scan.cancel) return;
    HANDLE h = nullptr;
    if (WNetOpenEnumW(RESOURCE_GLOBALNET, RESOURCETYPE_DISK, 0, container, &h) != NO_ERROR) return;
    std::vector<BYTE> buf(64 * 1024);
    for (;;) {
        if (scan.cancel) break;
        DWORD count = (DWORD)-1, size = (DWORD)buf.size();
        DWORD rc = WNetEnumResourceW(h, &count, buf.data(), &size);
        if (rc != NO_ERROR) break;
        auto* res = (LPNETRESOURCEW)buf.data();
        for (DWORD i = 0; i < count && !scan.cancel; ++i) {
            if (res[i].dwDisplayType == RESOURCEDISPLAYTYPE_SERVER && res[i].lpRemoteName) {
                server(res[i].lpRemoteName, res[i].lpComment ? res[i].lpComment : L"");
            } else if (res[i].dwUsage & RESOURCEUSAGE_CONTAINER) {
                EnumWNet(&res[i], depth + 1, scan, server);
            }
        }
    }
    WNetCloseEnum(h);
}

void ScanServers(const std::shared_ptr<NetworkScan>& scan, const std::function<void(const NetworkItem&)>& found) {
    Deduplicator dedup;
    auto report = [&](const std::wstring& unc, const std::wstring& comment) {
        if (scan->cancel) return;
        std::wstring server, share;
        if (!SplitUnc(unc, server, share) || server.empty()) return;
        if (dedup.Add(server)) found({server, comment});
    };
    // 1. Rechner verbundener Netzlaufwerke (sofort)
    for (auto& m : MappedShares()) report(L"\\\\" + m.first, L"");
    // 2. Klassische Netzwerkumgebung
    EnumWNet(nullptr, 0, *scan, report);
    // 3. Netzwerkordner der Shell (Netzwerkerkennung, WSD)
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!scan->cancel && SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_NetworkFolder, 0, nullptr, &pidl))) {
        EnumShellFolder(pidl, *scan, [&](const std::wstring& parsing, const std::wstring&) { report(parsing, L""); });
        CoTaskMemFree(pidl);
    }
}

bool ScanShares(const std::wstring& server, bool includeHidden, const std::shared_ptr<NetworkScan>& scan,
                const std::function<void(const NetworkItem&)>& found, DWORD* errorOut) {
    Deduplicator dedup;
    bool any = false;
    // Freigaben verbundener Netzlaufwerke dieses Rechners (auch WebDAV, das NetShareEnum nicht kennt)
    for (auto& m : MappedShares())
        if (EqualsI(m.first, server) && !m.second.empty() && dedup.Add(m.second)) {
            found({m.second, L""});
            any = true;
        }
    std::wstring unc = L"\\\\" + server;
    PSHARE_INFO_1 info = nullptr;
    DWORD read = 0, total = 0, resume = 0;
    NET_API_STATUS st;
    DWORD lastError = 0;
    bool netOk = false;
    do {
        st = NetShareEnum(const_cast<LPWSTR>(unc.c_str()), 1, (LPBYTE*)&info, MAX_PREFERRED_LENGTH, &read, &total, &resume);
        if (st == NERR_Success || st == ERROR_MORE_DATA) {
            netOk = true;
            for (DWORD i = 0; i < read && !scan->cancel; ++i) {
                DWORD type = info[i].shi1_type;
                if ((type & 0xFF) != STYPE_DISKTREE) continue;
                if ((type & STYPE_SPECIAL) && !includeHidden) continue;
                std::wstring name = info[i].shi1_netname ? info[i].shi1_netname : L"";
                if (name.empty() || (!includeHidden && name.back() == L'$')) continue;
                if (dedup.Add(name)) {
                    found({name, info[i].shi1_remark ? info[i].shi1_remark : L""});
                    any = true;
                }
            }
            NetApiBufferFree(info);
            info = nullptr;
        } else {
            lastError = st;
        }
    } while (st == ERROR_MORE_DATA && !scan->cancel);
    if (!netOk && !scan->cancel) {
        // Ersatz: Shell (z. B. Samba-Server, die NetShareEnum verweigern)
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(unc.c_str(), nullptr, &pidl, 0, nullptr))) {
            EnumShellFolder(pidl, *scan, [&](const std::wstring& parsing, const std::wstring&) {
                std::wstring sv, share;
                if (SplitUnc(parsing, sv, share) && !share.empty() && (includeHidden || share.back() != L'$') &&
                    dedup.Add(share)) {
                    found({share, L""});
                    any = true;
                }
            });
            CoTaskMemFree(pidl);
            if (any) lastError = 0;
        }
    }
    if (errorOut) *errorOut = lastError;
    return any || netOk;
}

} // namespace

std::wstring MappedDriveTarget(const std::wstring& driveRoot) {
    if (driveRoot.size() < 2 || driveRoot[1] != L':') return L"";
    std::wstring root = driveRoot.substr(0, 2) + L"\\";
    if (GetDriveTypeW(root.c_str()) != DRIVE_REMOTE) return L"";
    wchar_t buf[1024] = {};
    DWORD len = 1023;
    std::wstring local = driveRoot.substr(0, 2);
    if (WNetGetConnectionW(local.c_str(), buf, &len) != NO_ERROR) return L"";
    return buf;
}

std::shared_ptr<NetworkScan> StartNetworkScan(const std::wstring& location, bool includeHidden,
                                              std::function<void(const NetworkItem&)> found,
                                              std::function<void(bool ok, DWORD error)> finished) {
    auto scan = std::make_shared<NetworkScan>();
    std::wstring loc = location;
    std::thread([scan, loc, includeHidden, found, finished]() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto report = [&](const NetworkItem& it) {
            if (!scan->cancel) found(it);
        };
        bool ok = true;
        DWORD err = 0;
        if (IsNetworkRoot(loc)) {
            ScanServers(scan, report);
        } else if (IsNetworkServer(loc)) {
            ok = ScanShares(loc.substr(2), includeHidden, scan, report, &err);
        }
        scan->done = true;
        if (!scan->cancel) finished(ok, err);
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
    return scan;
}

} // namespace qf
