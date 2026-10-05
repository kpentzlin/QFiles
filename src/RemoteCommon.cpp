// Gemeinsame Hilfen der FTP/SFTP-Clients: Winsock, Verbindungsaufbau mit Zeitlimit, Base64, Zeitumrechnung.
#include "RemoteFs.h"

#include <mutex>

namespace qf::remote {

bool WinsockInit() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, []() {
        WSADATA wsa;
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    });
    return ok;
}

std::wstring SocketErrorText(int code) {
    switch (code) {
    case WSAETIMEDOUT: return L"Zeitüberschreitung.";
    case WSAECONNREFUSED: return L"Verbindung abgelehnt (Dienst läuft nicht oder Port falsch).";
    case WSAECONNRESET: return L"Verbindung vom Server zurückgesetzt.";
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH: return L"Server nicht erreichbar.";
    case WSAHOST_NOT_FOUND: return L"Server nicht gefunden.";
    }
    return LastErrorMessage((DWORD)code);
}

SOCKET ConnectTcp(const std::wstring& host, int port, int timeoutMs, std::wstring& err) {
    if (!WinsockInit()) {
        err = L"Netzwerk (Winsock) nicht verfügbar.";
        return INVALID_SOCKET;
    }
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* res = nullptr;
    std::wstring portText = std::to_wstring(port);
    int rc = GetAddrInfoW(host.c_str(), portText.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        err = L"Server „" + host + L"“ nicht gefunden: " + SocketErrorText(rc);
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    int lastErr = 0;
    for (ADDRINFOW* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) {
            lastErr = WSAGetLastError();
            continue;
        }
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int c = connect(s, a->ai_addr, (int)a->ai_addrlen);
        bool ok = c == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(s, &wr);
            FD_SET(s, &ex);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            int sel = select(0, nullptr, &wr, &ex, &tv);
            if (sel > 0 && FD_ISSET(s, &wr)) {
                int soerr = 0, len = sizeof(soerr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
                ok = soerr == 0;
                if (!ok) lastErr = soerr;
            } else if (sel == 0) {
                lastErr = WSAETIMEDOUT;
            } else {
                int soerr = 0, len = sizeof(soerr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
                lastErr = soerr ? soerr : WSAECONNREFUSED;
            }
        } else if (!ok) {
            lastErr = WSAGetLastError();
        }
        if (ok) {
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
            DWORD to = (DWORD)timeoutMs;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
            BOOL nodelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
            break;
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    FreeAddrInfoW(res);
    if (s == INVALID_SOCKET)
        err = L"Verbindung zu " + host + L":" + std::to_wstring(port) + L" fehlgeschlagen: " + SocketErrorText(lastErr);
    return s;
}

FILETIME UnixTimeToFileTime(int64_t t) {
    uint64_t v = (uint64_t)(t * 10000000LL + 116444736000000000LL);
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)v;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    return ft;
}

int64_t FileTimeToUnixTime(const FILETIME& ft) {
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((v - 116444736000000000ULL) / 10000000ULL);
}

std::string Base64Encode(const unsigned char* data, size_t len, bool pad) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i < len) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        if (i + 1 < len) out += tbl[(v >> 6) & 63];
        else if (pad) out += '=';
        if (pad) out += '=';
    }
    return out;
}

bool Base64Decode(const std::string& s, std::vector<unsigned char>& out) {
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        else return false;
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((v >> bits) & 0xFF));
        }
    }
    return true;
}

} // namespace qf::remote
