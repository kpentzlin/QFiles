// FTP-Client (RFC 959, 2389, 2428, 3659) direkt über Winsock: UTF-8-Dateinamen, passiver Modus (EPSV/PASV),
// Verzeichnisse per MLSD (sonst LIST mit Unix- und DOS-Format), binäre Übertragung.
// Verschlüsseltes FTPS wird nicht unterstützt (dafür SFTP verwenden).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "RemoteFs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace qf::remote {

namespace {

struct Reply {
    int code = 0;
    std::string text;   // alle Zeilen
    bool Ok() const { return code >= 200 && code < 300; }
    bool Positive() const { return code >= 100 && code < 400; }
};

const int kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

int MonthIndex(const std::string& m) {
    static const char* names[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::string l = m;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    if (l.size() < 3) return -1;
    for (int i = 0; i < 12; ++i)
        if (l.compare(0, 3, names[i]) == 0) return i;
    // deutsche Kurznamen (manche Server)
    static const char* de[] = {"jan", "feb", "mär", "apr", "mai", "jun", "jul", "aug", "sep", "okt", "nov", "dez"};
    for (int i = 0; i < 12; ++i)
        if (l.compare(0, strlen(de[i]), de[i]) == 0) return i;
    return -1;
}

FILETIME MakeFileTime(int y, int mon, int d, int h, int mi, int s) {
    SYSTEMTIME st{};
    st.wYear = (WORD)y;
    st.wMonth = (WORD)mon;
    st.wDay = (WORD)d;
    st.wHour = (WORD)h;
    st.wMinute = (WORD)mi;
    st.wSecond = (WORD)s;
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    return ft;
}

std::vector<std::string> Tokens(const std::string& line, size_t maxTokens, size_t* restPos) {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < line.size() && t.size() < maxTokens) {
        while (i < line.size() && line[i] == ' ') ++i;
        size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > start) t.push_back(line.substr(start, i - start));
    }
    // Rest: nach genau einem Leerzeichen
    if (i < line.size() && line[i] == ' ') ++i;
    *restPos = i;
    return t;
}

class FtpFs : public RemoteFs {
public:
    ~FtpFs() override { Disconnect(); }

    bool Connect(const ConnectSettings& s, const ConnectPrompts* prompts, std::wstring& err) override {
        Disconnect();
        settings = s;
        ctrl_ = ConnectTcp(s.url.host, s.url.EffectivePort(), s.timeoutMs, err);
        if (ctrl_ == INVALID_SOCKET) return false;
        Reply r;
        if (!ReadReply(r, err)) return Fail(err);
        if (r.code != 220) return Fail(err = L"Unerwartete Begrüßung des Servers: " + Text(r));
        std::wstring user = s.url.user.empty() ? L"anonymous" : s.url.user;
        std::wstring pass = s.password;
        if (s.url.user.empty() && pass.empty()) pass = L"qfiles@";
        if (!Command("USER " + WideToUtf8(user), r, err)) return Fail(err);
        if (r.code == 331 || r.code == 332) {
            if (pass.empty() && prompts && prompts->askPassword) {
                if (!prompts->askPassword(L"Kennwort für " + user + L"@" + s.url.host + L":", pass))
                    return Fail(err = L"Anmeldung abgebrochen.");
                settings.password = pass;
            }
            if (!Command("PASS " + WideToUtf8(pass), r, err)) return Fail(err);
        }
        if (!r.Ok()) return Fail(err = L"Anmeldung fehlgeschlagen: " + Text(r));
        // Fähigkeiten
        if (Command("FEAT", r, err) && r.Ok()) {
            std::string f = r.text;
            for (auto& c : f) c = (char)toupper((unsigned char)c);
            utf8_ = f.find("UTF8") != std::string::npos;
            mlsd_ = f.find("MLSD") != std::string::npos || f.find("MLST") != std::string::npos;
            mfmt_ = f.find("MFMT") != std::string::npos;
        }
        if (Command("OPTS UTF8 ON", r, err) && r.Ok()) utf8_ = true;
        if (!Command("TYPE I", r, err) || !r.Ok()) return Fail(err = L"Binärmodus nicht verfügbar: " + Text(r));
        home_ = L"/";
        if (Command("PWD", r, err) && r.code == 257) {
            size_t a = r.text.find('"'), b = r.text.rfind('"');
            if (a != std::string::npos && b > a) home_ = NormalizeRemotePath(Decode(r.text.substr(a + 1, b - a - 1)));
        }
        return true;
    }

    bool Connected() const override { return ctrl_ != INVALID_SOCKET; }

    void Disconnect() override {
        if (ctrl_ != INVALID_SOCKET) {
            std::string q = "QUIT\r\n";
            send(ctrl_, q.data(), (int)q.size(), 0);
            closesocket(ctrl_);
            ctrl_ = INVALID_SOCKET;
        }
        inbuf_.clear();
    }

    std::wstring HomeDir() override { return home_; }

    bool List(const std::wstring& path, std::vector<DirEntry>& out, std::wstring& err) override {
        out.clear();
        std::string data;
        bool viaMlsd = false;
        if (mlsd_) {
            if (!Transfer("MLSD " + Encode(path), &data, nullptr, nullptr, err, true)) {
                if (mlsdRejected_) mlsd_ = false;
                else return false;
            } else {
                viaMlsd = true;
            }
        }
        if (!viaMlsd) {
            Reply r;
            if (!Command("CWD " + Encode(path), r, err)) return false;
            if (!r.Ok()) {
                err = L"Verzeichnis nicht gefunden: " + Text(r);
                return false;
            }
            if (!Transfer("LIST", &data, nullptr, nullptr, err, false)) return false;
        }
        size_t pos = 0;
        while (pos < data.size()) {
            size_t e = data.find('\n', pos);
            if (e == std::string::npos) e = data.size();
            std::string line = data.substr(pos, e - pos);
            pos = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            DirEntry d;
            if (viaMlsd ? ParseMlsd(line, d) : ParseList(line, d)) out.push_back(std::move(d));
        }
        return true;
    }

    bool Stat(const std::wstring& path, DirEntry& out) override {
        std::wstring err;
        std::wstring parent = path.substr(0, path.find_last_of(L'/'));
        if (parent.empty()) parent = L"/";
        std::wstring name = path.substr(path.find_last_of(L'/') + 1);
        if (name.empty()) {
            out = DirEntry{};
            out.name = L"/";
            out.attributes = FILE_ATTRIBUTE_DIRECTORY;
            return true;
        }
        std::vector<DirEntry> entries;
        if (!List(parent, entries, err)) return false;
        for (auto& e : entries)
            if (e.name == name) {
                out = e;
                return true;
            }
        return false;
    }

    bool Download(const std::wstring& path, HANDLE localFile, const ProgressFn& progress, std::wstring& err) override {
        return Transfer("RETR " + Encode(path), nullptr, localFile, &progress, err, false, false);
    }

    bool Upload(HANDLE localFile, const std::wstring& path, const ProgressFn& progress, std::wstring& err) override {
        return Transfer("STOR " + Encode(path), nullptr, localFile, &progress, err, false, true);
    }

    bool SetModTime(const std::wstring& path, const FILETIME& mtime) override {
        if (!mfmt_) return false;
        SYSTEMTIME st;
        FileTimeToSystemTime(&mtime, &st);
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        Reply r;
        std::wstring err;
        return Command(std::string("MFMT ") + buf + " " + Encode(path), r, err) && r.Ok();
    }

    bool DeleteFile(const std::wstring& path, std::wstring& err) override { return Simple("DELE " + Encode(path), err); }
    bool RemoveDir(const std::wstring& path, std::wstring& err) override { return Simple("RMD " + Encode(path), err); }
    bool MakeDir(const std::wstring& path, std::wstring& err) override { return Simple("MKD " + Encode(path), err); }

    bool Rename(const std::wstring& from, const std::wstring& to, std::wstring& err) override {
        Reply r;
        if (!Command("RNFR " + Encode(from), r, err)) return false;
        if (r.code != 350) {
            err = Text(r);
            return false;
        }
        return Simple("RNTO " + Encode(to), err);
    }

private:
    bool Fail(const std::wstring&) {
        Disconnect();
        return false;
    }

    std::wstring Text(const Reply& r) const {
        std::string t = r.text;
        while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) t.pop_back();
        return Decode(t);
    }

    std::string Encode(const std::wstring& s) const {
        if (utf8_) return WideToUtf8(s);
        int n = WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string r(n, '\0');
        WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
        return r;
    }

    std::wstring Decode(const std::string& s) const {
        // UTF-8, wenn gültig; sonst Systemcodepage (ältere Server ohne UTF-8)
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
        if (n > 0 || s.empty()) return Utf8ToWide(s);
        n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring r(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), r.data(), n);
        return r;
    }

    bool SendLine(const std::string& line, std::wstring& err) {
        if (ctrl_ == INVALID_SOCKET) {
            err = L"Keine Verbindung zum Server.";
            return false;
        }
        std::string l = line + "\r\n";
        size_t done = 0;
        while (done < l.size()) {
            int n = send(ctrl_, l.data() + done, (int)(l.size() - done), 0);
            if (n <= 0) {
                err = L"Verbindung unterbrochen: " + SocketErrorText(WSAGetLastError());
                Disconnect();
                return false;
            }
            done += n;
        }
        return true;
    }

    bool ReadLine(std::string& line, std::wstring& err) {
        for (;;) {
            size_t e = inbuf_.find('\n');
            if (e != std::string::npos) {
                line = inbuf_.substr(0, e);
                inbuf_.erase(0, e + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            char buf[4096];
            int n = recv(ctrl_, buf, sizeof(buf), 0);
            if (n <= 0) {
                err = n == 0 ? L"Der Server hat die Verbindung getrennt." : (L"Keine Antwort vom Server: " + SocketErrorText(WSAGetLastError()));
                Disconnect();
                return false;
            }
            inbuf_.append(buf, n);
        }
    }

    bool ReadReply(Reply& r, std::wstring& err) {
        r = Reply{};
        std::string line;
        if (!ReadLine(line, err)) return false;
        if (line.size() < 3 || !isdigit((unsigned char)line[0])) {
            err = L"Ungültige Antwort des Servers.";
            Disconnect();
            return false;
        }
        r.code = atoi(line.substr(0, 3).c_str());
        r.text = line + "\n";
        if (line.size() > 3 && line[3] == '-') {
            std::string end = line.substr(0, 3) + " ";
            for (;;) {
                if (!ReadLine(line, err)) return false;
                r.text += line + "\n";
                if (line.compare(0, 4, end) == 0) break;
            }
        }
        if (r.code == 421) {
            err = L"Der Server hat die Verbindung beendet: " + Text(r);
            Disconnect();
            return false;
        }
        return true;
    }

    bool Command(const std::string& cmd, Reply& r, std::wstring& err) {
        if (!SendLine(cmd, err)) return false;
        return ReadReply(r, err);
    }

    bool Simple(const std::string& cmd, std::wstring& err) {
        Reply r;
        if (!Command(cmd, r, err)) return false;
        if (!r.Ok()) {
            err = Text(r);
            return false;
        }
        return true;
    }

    // Peer-Adresse der Steuerverbindung mit anderem Port
    bool PeerWithPort(int port, sockaddr_storage& sa, int& len) {
        len = sizeof(sa);
        if (getpeername(ctrl_, (sockaddr*)&sa, &len) != 0) return false;
        if (sa.ss_family == AF_INET) ((sockaddr_in*)&sa)->sin_port = htons((u_short)port);
        else if (sa.ss_family == AF_INET6) ((sockaddr_in6*)&sa)->sin6_port = htons((u_short)port);
        else return false;
        return true;
    }

    SOCKET OpenData(std::wstring& err) {
        Reply r;
        int port = -1;
        if (!noEpsv_) {
            if (!Command("EPSV", r, err)) return INVALID_SOCKET;
            if (r.code == 229) {
                size_t a = r.text.find("|||");
                if (a != std::string::npos) port = atoi(r.text.c_str() + a + 3);
            } else {
                noEpsv_ = true;
            }
        }
        if (port <= 0) {
            if (!Command("PASV", r, err)) return INVALID_SOCKET;
            if (r.code != 227) {
                err = L"Passiver Modus nicht möglich: " + Text(r);
                return INVALID_SOCKET;
            }
            size_t a = r.text.find('(');
            int h1, h2, h3, h4, p1, p2;
            const char* s = a != std::string::npos ? r.text.c_str() + a + 1 : nullptr;
            if (!s) {
                // manche Server ohne Klammern: Zahlen nach dem Text suchen
                s = r.text.c_str() + 4;
                while (*s && !isdigit((unsigned char)*s)) ++s;
            }
            if (sscanf(s, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
                err = L"Ungültige PASV-Antwort: " + Text(r);
                return INVALID_SOCKET;
            }
            port = p1 * 256 + p2;
        }
        sockaddr_storage sa{};
        int len = 0;
        if (!PeerWithPort(port, sa, len)) {
            err = L"Adresse des Servers unbekannt.";
            return INVALID_SOCKET;
        }
        SOCKET d = socket(sa.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (d == INVALID_SOCKET) {
            err = SocketErrorText(WSAGetLastError());
            return INVALID_SOCKET;
        }
        DWORD to = (DWORD)settings.timeoutMs;
        setsockopt(d, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
        setsockopt(d, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
        if (connect(d, (sockaddr*)&sa, len) != 0) {
            err = L"Datenverbindung fehlgeschlagen: " + SocketErrorText(WSAGetLastError());
            closesocket(d);
            return INVALID_SOCKET;
        }
        return d;
    }

    // Datenübertragung: Liste in *text bzw. Datei (Download: in file schreiben, Upload: aus file lesen)
    bool Transfer(const std::string& cmd, std::string* text, HANDLE file, const ProgressFn* progress, std::wstring& err,
                  bool isMlsd, bool upload = false) {
        SOCKET d = OpenData(err);
        if (d == INVALID_SOCKET) return false;
        Reply r;
        if (!Command(cmd, r, err)) {
            closesocket(d);
            return false;
        }
        if (r.code != 125 && r.code != 150) {
            closesocket(d);
            if (isMlsd && r.code >= 500 && r.code < 503) mlsdRejected_ = true;
            err = Text(r);
            return false;
        }
        bool ok = true;
        bool cancelled = false;
        uint64_t total = 0;
        std::vector<char> buf(64 * 1024);
        if (upload) {
            for (;;) {
                DWORD got = 0;
                if (!ReadFile(file, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
                    err = L"Lesefehler: " + LastErrorMessage();
                    ok = false;
                    break;
                }
                if (got == 0) break;
                size_t sent = 0;
                while (sent < got) {
                    int n = send(d, buf.data() + sent, (int)(got - sent), 0);
                    if (n <= 0) {
                        err = L"Übertragung unterbrochen: " + SocketErrorText(WSAGetLastError());
                        ok = false;
                        break;
                    }
                    sent += n;
                }
                if (!ok) break;
                total += got;
                if (progress && *progress && !(*progress)(total)) {
                    cancelled = true;
                    break;
                }
            }
            shutdown(d, SD_SEND);
        } else {
            for (;;) {
                int n = recv(d, buf.data(), (int)buf.size(), 0);
                if (n == 0) break;
                if (n < 0) {
                    err = L"Übertragung unterbrochen: " + SocketErrorText(WSAGetLastError());
                    ok = false;
                    break;
                }
                if (text) {
                    text->append(buf.data(), n);
                } else {
                    DWORD w = 0;
                    if (!WriteFile(file, buf.data(), (DWORD)n, &w, nullptr) || w != (DWORD)n) {
                        err = L"Schreibfehler: " + LastErrorMessage();
                        ok = false;
                        break;
                    }
                }
                total += n;
                if (progress && *progress && !(*progress)(total)) {
                    cancelled = true;
                    break;
                }
            }
        }
        closesocket(d);
        if (cancelled || !ok) {
            // Abbruch: Steuerverbindung in unbekanntem Zustand – neu verbinden beim nächsten Zugriff
            if (cancelled) err = L"Abgebrochen.";
            Disconnect();
            return false;
        }
        if (!ReadReply(r, err)) return false;
        if (!r.Ok()) {
            err = Text(r);
            return false;
        }
        return true;
    }

    bool ParseMlsd(const std::string& line, DirEntry& d) const {
        size_t sp = line.find(' ');
        if (sp == std::string::npos) return false;
        std::string facts = line.substr(0, sp);
        std::string name = line.substr(sp + 1);
        if (name.empty() || name == "." || name == "..") return false;
        bool isDir = false;
        for (auto& f : Split(Utf8ToWide(facts), L';')) {
            size_t eq = f.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring k = ToLower(f.substr(0, eq)), v = f.substr(eq + 1);
            if (k == L"type") {
                std::wstring t = ToLower(v);
                if (t == L"cdir" || t == L"pdir") return false;
                isDir = t == L"dir";
            } else if (k == L"size" || k == L"sizd") {
                d.size = (uint64_t)_wcstoui64(v.c_str(), nullptr, 10);
            } else if (k == L"modify" && v.size() >= 14) {
                int y = _wtoi(v.substr(0, 4).c_str()), mo = _wtoi(v.substr(4, 2).c_str()), da = _wtoi(v.substr(6, 2).c_str());
                int h = _wtoi(v.substr(8, 2).c_str()), mi = _wtoi(v.substr(10, 2).c_str()), s = _wtoi(v.substr(12, 2).c_str());
                d.modified = MakeFileTime(y, mo, da, h, mi, s);
            }
        }
        d.name = Decode(name);
        d.attributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (isDir) d.size = 0;
        if (!d.name.empty() && d.name[0] == L'.') d.attributes |= FILE_ATTRIBUTE_HIDDEN;
        d.created = d.modified;
        return true;
    }

    bool ParseList(const std::string& line, DirEntry& d) const {
        if (line.compare(0, 5, "total") == 0) return false;
        size_t rest = 0;
        if (isdigit((unsigned char)line[0])) {
            // DOS/IIS: 01-31-21  10:15PM       <DIR>          name
            auto t = Tokens(line, 3, &rest);
            if (t.size() < 3 || rest >= line.size()) return false;
            int mo = 0, da = 0, y = 0, h = 0, mi = 0;
            if (sscanf(t[0].c_str(), "%d-%d-%d", &mo, &da, &y) != 3) return false;
            if (y < 100) y += y < 70 ? 2000 : 1900;
            char ampm[3] = {};
            sscanf(t[1].c_str(), "%d:%d%2s", &h, &mi, ampm);
            if ((ampm[0] == 'P' || ampm[0] == 'p') && h < 12) h += 12;
            if ((ampm[0] == 'A' || ampm[0] == 'a') && h == 12) h = 0;
            d.modified = MakeFileTime(y, mo, da, h, mi, 0);
            if (t[2] == "<DIR>") d.attributes = FILE_ATTRIBUTE_DIRECTORY;
            else {
                d.attributes = FILE_ATTRIBUTE_NORMAL;
                d.size = _strtoui64(t[2].c_str(), nullptr, 10);
            }
            d.name = Decode(line.substr(rest));
        } else {
            // Unix: drwxr-xr-x  2 user group 4096 Jan  1 12:00 name
            auto t = Tokens(line, 8, &rest);
            if (t.size() < 8 || rest >= line.size()) return false;
            char type = line[0];
            // Gruppe kann fehlen (7 Felder vor dem Datum): Größe ist das letzte Zahlenfeld vor dem Monat
            int mIdx = -1;
            for (int i = 3; i < (int)t.size(); ++i)
                if (MonthIndex(t[i]) >= 0 && i >= 1 && isdigit((unsigned char)t[i - 1][0])) {
                    mIdx = i;
                    break;
                }
            if (mIdx < 0) return false;
            // Feldaufteilung neu berechnen: Name beginnt nach Monat, Tag, Zeit/Jahr
            size_t need = (size_t)mIdx + 3;
            auto t2 = Tokens(line, need, &rest);
            if (t2.size() < need || rest >= line.size()) return false;
            d.size = _strtoui64(t2[mIdx - 1].c_str(), nullptr, 10);
            int mon = MonthIndex(t2[mIdx]) + 1;
            int day = atoi(t2[mIdx + 1].c_str());
            const std::string& ty = t2[mIdx + 2];
            SYSTEMTIME now;
            GetSystemTime(&now);
            int year = now.wYear, h = 0, mi = 0;
            if (ty.find(':') != std::string::npos) {
                sscanf(ty.c_str(), "%d:%d", &h, &mi);
                // Datum in der Zukunft -> Vorjahr
                if (mon > now.wMonth || (mon == now.wMonth && day > now.wDay + 1)) --year;
            } else {
                year = atoi(ty.c_str());
            }
            if (mon < 1 || mon > 12 || day < 1 || day > 31) return false;
            if (day > kMonthDays[mon - 1] && !(mon == 2 && day == 29)) day = kMonthDays[mon - 1];
            d.modified = MakeFileTime(year, mon, day, h, mi, 0);
            std::string name = line.substr(rest);
            if (type == 'l') {
                size_t arrow = name.find(" -> ");
                if (arrow != std::string::npos) name = name.substr(0, arrow);
            }
            if (name == "." || name == "..") return false;
            d.name = Decode(name);
            d.attributes = type == 'd' ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (type == 'd') d.size = 0;
        }
        if (d.name.empty()) return false;
        if (d.name[0] == L'.') d.attributes |= FILE_ATTRIBUTE_HIDDEN;
        d.created = d.modified;
        return true;
    }

    SOCKET ctrl_ = INVALID_SOCKET;
    std::string inbuf_;
    std::wstring home_ = L"/";
    bool utf8_ = false, mlsd_ = false, mfmt_ = false, noEpsv_ = false, mlsdRejected_ = false;
};

} // namespace

std::unique_ptr<RemoteFs> CreateFtpFs() { return std::make_unique<FtpFs>(); }

} // namespace qf::remote
