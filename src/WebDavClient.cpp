// WebDAV-Client (RFC 4918) über WinHTTP: HTTPS mit Zertifikatprüfung durch Windows, Anmeldung per Basic,
// Digest, NTLM oder Negotiate; Verzeichnisse per PROPFIND, Übertragung per GET/PUT, dazu DELETE, MKCOL, MOVE.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "RemoteFs.h"

#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace qf::remote {

namespace {

const char* kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/>"
    "<d:creationdate/></d:prop></d:propfind>";

std::wstring WinHttpErrorText(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        return L"Das Zertifikat des Servers ist ungültig oder nicht vertrauenswürdig.";
    case ERROR_WINHTTP_TIMEOUT: return L"Zeitüberschreitung.";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"Server nicht gefunden.";
    case ERROR_WINHTTP_CANNOT_CONNECT: return L"Verbindung zum Server nicht möglich.";
    case ERROR_WINHTTP_CONNECTION_ERROR: return L"Verbindung unterbrochen.";
    }
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), code, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring r = n && buf ? Trim(buf) : (L"WinHTTP-Fehler " + std::to_wstring(code));
    if (buf) LocalFree(buf);
    return r;
}

std::wstring HttpStatusText(DWORD status) {
    switch (status) {
    case 401: return L"Anmeldung fehlgeschlagen (401).";
    case 403: return L"Zugriff verweigert (403).";
    case 404: return L"Nicht gefunden (404).";
    case 405: return L"Vom Server nicht erlaubt (405) – z. B. existiert das Ziel bereits.";
    case 409: return L"Konflikt (409) – übergeordnetes Verzeichnis fehlt.";
    case 412: return L"Ziel existiert bereits (412).";
    case 423: return L"Gesperrt (423).";
    case 507: return L"Kein Platz mehr auf dem Server (507).";
    }
    return L"HTTP-Status " + std::to_wstring(status) + L".";
}

// Pfad für die URL: UTF-8, Prozentkodierung außer für unbedenkliche Zeichen und '/'
std::wstring EncodePath(const std::wstring& path) {
    std::string u = WideToUtf8(path);
    std::wstring r;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') r += (wchar_t)c;
        else {
            r += L'%';
            r += (wchar_t)hex[c >> 4];
            r += (wchar_t)hex[c & 15];
        }
    }
    return r;
}

std::string PercentDecode(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            r += s[i];
        }
    }
    return r;
}

std::string XmlDecode(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            r += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos) {
            r += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") r += '&';
        else if (ent == "lt") r += '<';
        else if (ent == "gt") r += '>';
        else if (ent == "quot") r += '"';
        else if (ent == "apos") r += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                                                  : strtoul(ent.c_str() + 1, nullptr, 10);
            std::wstring w;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                w += (wchar_t)(0xD800 + (cp >> 10));
                w += (wchar_t)(0xDC00 + (cp & 0x3FF));
            } else {
                w += (wchar_t)cp;
            }
            r += WideToUtf8(w);
        } else {
            r += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return r;
}

std::string TrimA(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool ParseHttpDate(const std::string& s, FILETIME& ft) {
    SYSTEMTIME st{};
    std::wstring w = Utf8ToWide(s);
    if (!WinHttpTimeToSystemTime(w.c_str(), &st)) return false;
    return SystemTimeToFileTime(&st, &ft) != FALSE;
}

bool ParseIsoDate(const std::string& s, FILETIME& ft) {
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return false;
    SYSTEMTIME st{};
    st.wYear = (WORD)y;
    st.wMonth = (WORD)mo;
    st.wDay = (WORD)d;
    st.wHour = (WORD)h;
    st.wMinute = (WORD)mi;
    st.wSecond = (WORD)sec;
    return SystemTimeToFileTime(&st, &ft) != FALSE;
}

struct DavEntry {
    std::wstring path;     // dekodierter Server-Pfad
    bool collection = false;
    uint64_t size = 0;
    FILETIME modified{}, created{};
};

// Einfacher, namensraum-unabhängiger Leser für multistatus-Antworten
std::vector<DavEntry> ParseMultistatus(const std::string& xml) {
    std::vector<DavEntry> out;
    DavEntry cur;
    bool inResponse = false, inResourceType = false;
    std::string textElem;
    size_t textStart = 0;
    size_t i = 0;
    while ((i = xml.find('<', i)) != std::string::npos) {
        if (xml.compare(i, 4, "<!--") == 0) {
            size_t e = xml.find("-->", i);
            i = e == std::string::npos ? xml.size() : e + 3;
            continue;
        }
        if (xml.compare(i, 2, "<?") == 0 || xml.compare(i, 2, "<!") == 0) {
            size_t e = xml.find('>', i);
            i = e == std::string::npos ? xml.size() : e + 1;
            continue;
        }
        size_t e = xml.find('>', i);
        if (e == std::string::npos) break;
        std::string tag = xml.substr(i + 1, e - i - 1);
        bool closing = !tag.empty() && tag[0] == '/';
        bool selfClosing = !tag.empty() && tag.back() == '/';
        if (closing) tag = tag.substr(1);
        if (selfClosing) tag.pop_back();
        size_t sp = tag.find_first_of(" \t\r\n");
        std::string name = sp == std::string::npos ? tag : tag.substr(0, sp);
        size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(colon + 1);
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        if (closing) {
            if (name == textElem && !textElem.empty()) {
                std::string v = TrimA(XmlDecode(xml.substr(textStart, i - textStart)));
                if (name == "href") {
                    // absolute URL -> Pfad
                    size_t scheme = v.find("://");
                    if (scheme != std::string::npos) {
                        size_t slash = v.find('/', scheme + 3);
                        v = slash == std::string::npos ? "/" : v.substr(slash);
                    }
                    cur.path = NormalizeRemotePath(Utf8ToWide(PercentDecode(v)));
                } else if (name == "getcontentlength") {
                    cur.size = _strtoui64(v.c_str(), nullptr, 10);
                } else if (name == "getlastmodified") {
                    ParseHttpDate(v, cur.modified);
                } else if (name == "creationdate") {
                    ParseIsoDate(v, cur.created);
                }
                textElem.clear();
            }
            if (name == "resourcetype") inResourceType = false;
            if (name == "response" && inResponse) {
                if (!cur.path.empty()) out.push_back(cur);
                inResponse = false;
            }
        } else {
            if (name == "response") {
                cur = DavEntry{};
                inResponse = true;
            } else if (name == "resourcetype") {
                inResourceType = !selfClosing;
            } else if (name == "collection" && inResourceType) {
                cur.collection = true;
            } else if (inResponse && !selfClosing &&
                       (name == "href" || name == "getcontentlength" || name == "getlastmodified" || name == "creationdate")) {
                textElem = name;
                textStart = e + 1;
            }
        }
        i = e + 1;
    }
    return out;
}

struct Response {
    DWORD status = 0;
    std::string body;
};

class WebDavFs : public RemoteFs {
public:
    ~WebDavFs() override { Disconnect(); }

    bool Connect(const ConnectSettings& s, const ConnectPrompts* prompts, std::wstring& err) override {
        Disconnect();
        settings = s;
        secure_ = s.url.proto == RemoteProto::WebDavs;
        session_ = WinHttpOpen(L"QFiles/" QFILES_VERSION_STRING, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_)
            session_ = WinHttpOpen(L"QFiles", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) return Fail(err = WinHttpErrorText(GetLastError()));
        WinHttpSetTimeouts(session_, s.timeoutMs, s.timeoutMs, s.timeoutMs, s.timeoutMs * 3);
        connect_ = WinHttpConnect(session_, s.url.host.c_str(), (INTERNET_PORT)s.url.EffectivePort(), 0);
        if (!connect_) return Fail(err = WinHttpErrorText(GetLastError()));
        user_ = s.url.user;
        pass_ = s.password;
        for (int attempt = 0; attempt < 4; ++attempt) {
            Response r;
            if (!Propfind(DirPath(s.url.path), L"0", r, err)) return Fail(err);
            if (r.status == 401) {
                if (!prompts || !prompts->askPassword) return Fail(err = L"Anmeldung erforderlich (Kennwort).");
                std::wstring prompt = (attempt > 0 || !pass_.empty() ? L"Anmeldung fehlgeschlagen. " : L"") + std::wstring(L"Kennwort für ") +
                                      (user_.empty() ? L"" : user_ + L"@") + s.url.host + L":";
                if (user_.empty()) {
                    // WebDAV ohne Benutzername: Anmeldung trotzdem verlangt
                    return Fail(err = L"Der Server verlangt eine Anmeldung – bitte einen Benutzernamen angeben.");
                }
                std::wstring pw;
                if (!prompts->askPassword(prompt, pw)) return Fail(err = L"Anmeldung abgebrochen.");
                pass_ = pw;
                settings.password = pw;
                continue;
            }
            if (r.status == 207 || r.status == 200 || r.status == 404 || r.status == 301 || r.status == 302) {
                connected_ = true;
                return true;
            }
            if (r.status == 405 || r.status == 501)
                return Fail(err = L"Der Server unterstützt kein WebDAV (" + std::to_wstring(r.status) + L").");
            return Fail(err = HttpStatusText(r.status));
        }
        return Fail(err = L"Anmeldung fehlgeschlagen.");
    }

    bool Connected() const override { return connected_; }

    void Disconnect() override {
        if (connect_) WinHttpCloseHandle(connect_);
        if (session_) WinHttpCloseHandle(session_);
        connect_ = session_ = nullptr;
        connected_ = false;
    }

    std::wstring HomeDir() override { return NormalizeRemotePath(settings.url.path); }

    bool List(const std::wstring& path, std::vector<DirEntry>& out, std::wstring& err) override {
        out.clear();
        Response r;
        if (!Propfind(DirPath(path), L"1", r, err)) return false;
        if (r.status != 207) return StatusFail(r.status, err);
        std::wstring self = NormalizeRemotePath(path);
        for (auto& e : ParseMultistatus(r.body)) {
            if (e.path == self) continue;
            DirEntry d;
            d.name = e.path.substr(e.path.find_last_of(L'/') + 1);
            if (d.name.empty()) continue;
            d.attributes = e.collection ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (d.name[0] == L'.') d.attributes |= FILE_ATTRIBUTE_HIDDEN;
            d.size = e.collection ? 0 : e.size;
            d.modified = e.modified;
            d.created = (e.created.dwLowDateTime || e.created.dwHighDateTime) ? e.created : e.modified;
            out.push_back(std::move(d));
        }
        return true;
    }

    bool Stat(const std::wstring& path, DirEntry& out) override {
        Response r;
        std::wstring err;
        if (!Propfind(path, L"0", r, err) || r.status != 207) return false;
        auto entries = ParseMultistatus(r.body);
        if (entries.empty()) return false;
        out = DirEntry{};
        out.name = path.substr(path.find_last_of(L'/') + 1);
        out.attributes = entries[0].collection ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        out.size = entries[0].collection ? 0 : entries[0].size;
        out.modified = out.created = entries[0].modified;
        return true;
    }

    bool Download(const std::wstring& path, HANDLE localFile, const ProgressFn& progress, std::wstring& err) override {
        Response r;
        if (!Request(L"GET", path, {}, nullptr, nullptr, 0, localFile, &progress, r, err)) return false;
        return r.status == 200 || StatusFail(r.status, err);
    }

    bool Upload(HANDLE localFile, const std::wstring& path, const ProgressFn& progress, std::wstring& err) override {
        LARGE_INTEGER size{};
        GetFileSizeEx(localFile, &size);
        Response r;
        if (!Request(L"PUT", path, {L"Content-Type: application/octet-stream"}, nullptr, localFile, (uint64_t)size.QuadPart,
                     nullptr, &progress, r, err))
            return false;
        return r.status == 200 || r.status == 201 || r.status == 204 || StatusFail(r.status, err);
    }

    bool SetModTime(const std::wstring&, const FILETIME&) override { return false; }  // bei WebDAV nicht setzbar

    bool DeleteFile(const std::wstring& path, std::wstring& err) override { return Simple(L"DELETE", path, {}, err); }
    bool RemoveDir(const std::wstring& path, std::wstring& err) override {
        // DELETE auf eine Sammlung löscht bei WebDAV rekursiv – wie bei FTP/SFTP nur leere Verzeichnisse entfernen
        std::vector<DirEntry> content;
        if (!List(path, content, err)) return false;
        if (!content.empty()) {
            err = L"Das Verzeichnis ist nicht leer.";
            return false;
        }
        return Simple(L"DELETE", DirPath(path), {}, err);
    }
    bool MakeDir(const std::wstring& path, std::wstring& err) override { return Simple(L"MKCOL", DirPath(path), {}, err); }

    bool Rename(const std::wstring& from, const std::wstring& to, std::wstring& err) override {
        DirEntry st;
        bool dir = Stat(from, st) && st.IsDir();
        std::wstring dest = std::wstring(secure_ ? L"https://" : L"http://") +
                            (settings.url.host.find(L':') != std::wstring::npos ? L"[" + settings.url.host + L"]" : settings.url.host) +
                            L":" + std::to_wstring(settings.url.EffectivePort()) + EncodePath(dir ? DirPath(to) : to);
        return Simple(L"MOVE", dir ? DirPath(from) : from, {L"Destination: " + dest, L"Overwrite: F"}, err);
    }

private:
    static std::wstring DirPath(const std::wstring& p) {
        std::wstring n = NormalizeRemotePath(p);
        return n == L"/" ? n : n + L"/";
    }

    bool Fail(const std::wstring&) {
        Disconnect();
        return false;
    }

    bool StatusFail(DWORD status, std::wstring& err) {
        err = HttpStatusText(status);
        if (status == 401) connected_ = false;
        return false;
    }

    bool Simple(const wchar_t* verb, const std::wstring& path, const std::vector<std::wstring>& headers, std::wstring& err) {
        Response r;
        if (!Request(verb, path, headers, nullptr, nullptr, 0, nullptr, nullptr, r, err)) return false;
        if (r.status >= 200 && r.status < 300) return true;
        return StatusFail(r.status, err);
    }

    bool Propfind(const std::wstring& path, const wchar_t* depth, Response& r, std::wstring& err) {
        std::string body = kPropfindBody;
        return Request(L"PROPFIND", path, {std::wstring(L"Depth: ") + depth, L"Content-Type: application/xml; charset=utf-8"},
                       &body, nullptr, 0, nullptr, nullptr, r, err);
    }

    static DWORD PickScheme(DWORD supported) {
        for (DWORD s : {WINHTTP_AUTH_SCHEME_NEGOTIATE, WINHTTP_AUTH_SCHEME_NTLM, WINHTTP_AUTH_SCHEME_DIGEST, WINHTTP_AUTH_SCHEME_BASIC})
            if (supported & s) return s;
        return 0;
    }

    // Eine Anfrage; bei 401 einmal mit Anmeldedaten wiederholen. body bzw. uploadFile = Inhalt;
    // downloadFile: Antwort (bei 200) dorthin schreiben, sonst in r.body.
    bool Request(const wchar_t* verb, const std::wstring& path, const std::vector<std::wstring>& headers, const std::string* body,
                 HANDLE uploadFile, uint64_t uploadSize, HANDLE downloadFile, const ProgressFn* progress, Response& r,
                 std::wstring& err) {
        if (!connect_) {
            err = L"Keine Verbindung zum Server.";
            return false;
        }
        HINTERNET req = WinHttpOpenRequest(connect_, verb, EncodePath(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0);
        if (!req) {
            err = WinHttpErrorText(GetLastError());
            return false;
        }
        for (auto& h : headers) WinHttpAddRequestHeaders(req, h.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        // Bekanntes Verfahren gleich mitschicken (Basic/NTLM/Negotiate; Digest braucht erst die Rückfrage des Servers)
        if (scheme_ && scheme_ != WINHTTP_AUTH_SCHEME_DIGEST && !user_.empty())
            WinHttpSetCredentials(req, WINHTTP_AUTH_TARGET_SERVER, scheme_, user_.c_str(), pass_.c_str(), nullptr);
        DWORD total = 0;
        bool big = false;
        if (body) total = (DWORD)body->size();
        else if (uploadFile) {
            if (uploadSize > 0xFFFFFFF0ull) {
                big = true;
                std::wstring cl = L"Content-Length: " + std::to_wstring(uploadSize);
                WinHttpAddRequestHeaders(req, cl.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            } else {
                total = (DWORD)uploadSize;
            }
        }
        std::vector<char> buf(64 * 1024);
        // Bei 401 werden die Anmeldedaten an derselben Anfrage gesetzt und die Anfrage erneut gesendet
        // (so verlangt es WinHTTP für Digest und die mehrstufigen Verfahren NTLM/Negotiate).
        DWORD lastScheme = 0;
        for (int round = 0; round < 4; ++round) {
            BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                         body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA, body ? (DWORD)body->size() : 0,
                                         big ? WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH : total, 0);
            if (ok && uploadFile) {
                SetFilePointer(uploadFile, 0, nullptr, FILE_BEGIN);
                uint64_t done = 0;
                for (;;) {
                    DWORD got = 0;
                    if (!ReadFile(uploadFile, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
                        err = L"Lesefehler: " + LastErrorMessage();
                        WinHttpCloseHandle(req);
                        return false;
                    }
                    if (!got) break;
                    DWORD written = 0;
                    if (!WinHttpWriteData(req, buf.data(), got, &written)) {
                        ok = FALSE;
                        break;
                    }
                    done += got;
                    if (progress && *progress && !(*progress)(done)) {
                        err = L"Abgebrochen.";
                        WinHttpCloseHandle(req);
                        return false;
                    }
                }
            }
            if (ok) ok = WinHttpReceiveResponse(req, nullptr);
            if (!ok) {
                DWORD e = GetLastError();
                err = WinHttpErrorText(e);
                WinHttpCloseHandle(req);
                if (e == ERROR_WINHTTP_CONNECTION_ERROR || e == ERROR_WINHTTP_TIMEOUT) connected_ = false;
                return false;
            }
            DWORD status = 0, len = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                &len, WINHTTP_NO_HEADER_INDEX);
            if (status == 401 && !user_.empty() && round < 3) {
                DWORD supported = 0, first = 0, target = 0;
                if (WinHttpQueryAuthSchemes(req, &supported, &first, &target)) {
                    DWORD sch = PickScheme(supported);
                    // Dasselbe Verfahren erneut abgelehnt (außer bei mehrstufigen Verfahren): Kennwort falsch
                    bool multiStep = sch == WINHTTP_AUTH_SCHEME_NTLM || sch == WINHTTP_AUTH_SCHEME_NEGOTIATE;
                    if (sch && (sch != lastScheme || multiStep) &&
                        WinHttpSetCredentials(req, target ? target : WINHTTP_AUTH_TARGET_SERVER, sch, user_.c_str(), pass_.c_str(),
                                              nullptr)) {
                        lastScheme = sch;
                        scheme_ = sch;
                        // Antwort der Rückfrage verwerfen, dann erneut senden
                        DWORD got = 0;
                        while (WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got) && got) {}
                        continue;
                    }
                }
            }
            r.status = status;
            r.body.clear();
            bool toFile = downloadFile && status == 200;
            uint64_t done = 0;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) {
                    err = WinHttpErrorText(GetLastError());
                    WinHttpCloseHandle(req);
                    return false;
                }
                if (!got) break;
                if (toFile) {
                    DWORD w = 0;
                    if (!WriteFile(downloadFile, buf.data(), got, &w, nullptr) || w != got) {
                        err = L"Schreibfehler: " + LastErrorMessage();
                        WinHttpCloseHandle(req);
                        return false;
                    }
                    done += got;
                    if (progress && *progress && !(*progress)(done)) {
                        err = L"Abgebrochen.";
                        WinHttpCloseHandle(req);
                        return false;
                    }
                } else if (r.body.size() < 64u * 1024 * 1024) {
                    r.body.append(buf.data(), got);
                }
            }
            WinHttpCloseHandle(req);
            return true;
        }
        WinHttpCloseHandle(req);
        r.status = 401;
        return true;
    }

    HINTERNET session_ = nullptr, connect_ = nullptr;
    bool connected_ = false, secure_ = true;
    DWORD scheme_ = 0;
    std::wstring user_, pass_;
};

} // namespace

std::unique_ptr<RemoteFs> CreateWebDavFs() { return std::make_unique<WebDavFs>(); }

} // namespace qf::remote
