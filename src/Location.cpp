#include "Location.h"
#include "Util.h"

#include <windows.h>
#include <vector>

namespace qf {

const wchar_t* const kNetworkRoot = L"\\\\";
const wchar_t* const kNetworkName = L"Netzwerk";

namespace {

// Rest nach "\\" ohne abschließende Backslashes
std::wstring UncRest(const std::wstring& p) {
    std::wstring r = p.substr(2);
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

bool StartsUnc(const std::wstring& p) {
    return p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/') &&
           !(p.size() >= 3 && (p[2] == L'?' || p[2] == L'.'));
}

} // namespace

bool IsNetworkRoot(const std::wstring& p) {
    if (p == L"\\\\" || p == L"//") return true;
    return EqualsI(p, kNetworkName);
}

bool IsNetworkServer(const std::wstring& p) {
    if (!StartsUnc(p)) return false;
    std::wstring r = UncRest(p);
    return !r.empty() && r.find_first_of(L"\\/") == std::wstring::npos;
}

bool IsNetworkVirtual(const std::wstring& p) { return IsNetworkRoot(p) || IsNetworkServer(p); }

bool IsUncShareRoot(const std::wstring& p) {
    if (!StartsUnc(p)) return false;
    auto parts = Split(UncRest(p), L'\\');
    return parts.size() == 2;
}

bool IsRemoteUrl(const std::wstring& p) { return StartsWithI(p, L"ftp://") || StartsWithI(p, L"sftp://"); }

bool IsVirtualLocation(const std::wstring& p) { return IsNetworkVirtual(p) || IsRemoteUrl(p); }

bool IsNetworkPath(const std::wstring& p) {
    if (IsNetworkVirtual(p) || StartsUnc(p)) return true;
    if (p.size() >= 2 && p[1] == L':') {
        std::wstring root = p.substr(0, 2) + L"\\";
        return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    }
    return false;
}

std::wstring NormalizeSpecialLocation(const std::wstring& raw) {
    std::wstring p = Trim(raw);
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
    if (IsNetworkRoot(p)) return kNetworkRoot;
    if (IsNetworkServer(p)) return L"\\\\" + UncRest(p);
    if (IsRemoteUrl(p)) {
        RemoteUrl u;
        if (ParseRemoteUrl(p, u)) return u.ToString();
    }
    return L"";
}

std::wstring LocationParent(const std::wstring& p) {
    if (IsNetworkRoot(p)) return L"";
    if (IsNetworkServer(p)) return kNetworkRoot;
    if (IsUncShareRoot(p)) {
        auto parts = Split(UncRest(p), L'\\');
        return L"\\\\" + parts[0];
    }
    if (IsRemoteUrl(p)) {
        RemoteUrl u;
        if (!ParseRemoteUrl(p, u) || u.path == L"/") return L"";
        size_t pos = u.path.find_last_of(L'/');
        u.path = pos == 0 ? L"/" : u.path.substr(0, pos);
        return u.ToString();
    }
    return PathParent(p);
}

bool LocationHasParent(const std::wstring& p) { return !LocationParent(p).empty(); }

std::wstring LocationCombine(const std::wstring& dir, const std::wstring& name) {
    if (IsRemoteUrl(dir)) {
        if (dir.back() == L'/') return dir + name;
        return dir + L'/' + name;
    }
    if (IsNetworkRoot(dir)) return std::wstring(kNetworkRoot) + name;
    return PathCombine(dir, name);
}

std::wstring LocationFileName(const std::wstring& p) {
    if (IsNetworkRoot(p)) return kNetworkName;
    if (IsRemoteUrl(p)) {
        RemoteUrl u;
        if (!ParseRemoteUrl(p, u)) return p;
        if (u.path == L"/") return u.host;
        size_t pos = u.path.find_last_of(L'/');
        return u.path.substr(pos + 1);
    }
    if (IsNetworkServer(p)) return UncRest(p);
    return PathFileName(p);
}

std::wstring LocationDisplay(const std::wstring& p) {
    if (IsNetworkRoot(p)) return kNetworkName;
    return p;
}

// ===================== FTP/SFTP-Adressen =====================

std::wstring NormalizeRemotePath(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p)
        if (c == L'\\') c = L'/';
    std::vector<std::wstring> out;
    for (auto& part : Split(p, L'/')) {
        if (part == L".") continue;
        if (part == L"..") {
            if (!out.empty()) out.pop_back();
            continue;
        }
        out.push_back(part);
    }
    std::wstring r;
    for (auto& part : out) r += L"/" + part;
    return r.empty() ? L"/" : r;
}

std::wstring RemoteUrl::ServerKey() const {
    std::wstring s = proto == RemoteProto::Sftp ? L"sftp://" : L"ftp://";
    if (!user.empty()) s += ReplaceAll(ReplaceAll(user, L"%", L"%25"), L"@", L"%40") + L"@";
    s += host.find(L':') != std::wstring::npos ? (L"[" + host + L"]") : host;
    if (port && port != (proto == RemoteProto::Sftp ? 22 : 21)) s += L":" + std::to_wstring(port);
    return s;
}

std::wstring RemoteUrl::ToString() const { return ServerKey() + NormalizeRemotePath(path); }

bool ParseRemoteUrl(const std::wstring& s, RemoteUrl& out) {
    std::wstring t = Trim(s);
    RemoteUrl u;
    size_t start;
    if (StartsWithI(t, L"sftp://")) {
        u.proto = RemoteProto::Sftp;
        start = 7;
    } else if (StartsWithI(t, L"ftp://")) {
        u.proto = RemoteProto::Ftp;
        start = 6;
    } else {
        return false;
    }
    size_t slash = t.find(L'/', start);
    std::wstring auth = t.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
    u.path = slash == std::wstring::npos ? L"/" : t.substr(slash);
    size_t at = auth.rfind(L'@');
    if (at != std::wstring::npos) {
        u.user = ReplaceAll(ReplaceAll(auth.substr(0, at), L"%40", L"@"), L"%25", L"%");
        auth = auth.substr(at + 1);
    }
    if (!auth.empty() && auth[0] == L'[') {
        size_t close = auth.find(L']');
        if (close == std::wstring::npos) return false;
        u.host = auth.substr(1, close - 1);
        std::wstring rest = auth.substr(close + 1);
        if (!rest.empty() && rest[0] == L':') u.port = (int)StrToInt(rest.substr(1), 0);
    } else {
        size_t colon = auth.rfind(L':');
        if (colon != std::wstring::npos) {
            u.port = (int)StrToInt(auth.substr(colon + 1), 0);
            auth = auth.substr(0, colon);
        }
        u.host = auth;
    }
    if (u.host.empty() || u.port < 0 || u.port > 65535) return false;
    u.path = NormalizeRemotePath(u.path);
    out = u;
    return true;
}

} // namespace qf
