// SFTP-Client über libssh2 (WinCNG): Hostschlüssel-Prüfung mit eigener Liste bekannter Server,
// Anmeldung per Schlüsseldatei, SSH-Agent (Windows-OpenSSH/Pageant), Kennwort oder keyboard-interactive.
#include "RemoteFs.h"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace qf::remote {

namespace {

std::once_flag g_initOnce;

const wchar_t* HostKeyTypeName(int type) {
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return L"ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_DSS: return L"ssh-dss";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return L"ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return L"ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return L"ecdsa-sha2-nistp521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return L"ssh-ed25519";
    }
    return L"unbekannt";
}

std::wstring SftpStatusText(unsigned long code) {
    switch (code) {
    case LIBSSH2_FX_NO_SUCH_FILE: return L"Datei oder Verzeichnis nicht gefunden.";
    case LIBSSH2_FX_PERMISSION_DENIED: return L"Zugriff verweigert.";
    case LIBSSH2_FX_FAILURE: return L"Vorgang fehlgeschlagen (z. B. Verzeichnis nicht leer oder Ziel existiert).";
    case LIBSSH2_FX_NO_CONNECTION:
    case LIBSSH2_FX_CONNECTION_LOST: return L"Verbindung unterbrochen.";
    case LIBSSH2_FX_OP_UNSUPPORTED: return L"Vom Server nicht unterstützt.";
    case LIBSSH2_FX_FILE_ALREADY_EXISTS: return L"Existiert bereits.";
    case LIBSSH2_FX_WRITE_PROTECT: return L"Schreibgeschützt.";
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
    case LIBSSH2_FX_QUOTA_EXCEEDED: return L"Kein Platz mehr auf dem Server.";
    case LIBSSH2_FX_DIR_NOT_EMPTY: return L"Verzeichnis nicht leer.";
    case LIBSSH2_FX_NOT_A_DIRECTORY: return L"Kein Verzeichnis.";
    case LIBSSH2_FX_INVALID_FILENAME: return L"Ungültiger Dateiname.";
    }
    return L"SFTP-Fehler " + std::to_wstring(code) + L".";
}

// ---- Bekannte Hostschlüssel: Zeilen "host:port<TAB>Typ<TAB>Base64(Schlüssel)" (UTF-8) ----
struct KnownHost {
    std::wstring hostPort, type;
    std::string key;
};

std::vector<KnownHost> ReadKnownHosts(const std::wstring& file) {
    std::vector<KnownHost> r;
    std::vector<uint8_t> data;
    if (file.empty() || !ReadFileBytes(file, data)) return r;
    std::wstring text = Utf8ToWide(std::string(data.begin(), data.end()));
    for (auto& line : Split(text, L'\n')) {
        auto parts = Split(Trim(line), L'\t');
        if (parts.size() == 3) r.push_back({parts[0], parts[1], WideToUtf8(parts[2])});
    }
    return r;
}

void WriteKnownHosts(const std::wstring& file, const std::vector<KnownHost>& hosts) {
    std::string out;
    for (auto& h : hosts) out += WideToUtf8(h.hostPort + L"\t" + h.type + L"\t") + h.key + "\n";
    WriteFileBytes(file, out.data(), out.size());
}

class SftpFs : public RemoteFs {
public:
    ~SftpFs() override { Disconnect(); }

    bool Connect(const ConnectSettings& s, const ConnectPrompts* prompts, std::wstring& err) override {
        Disconnect();
        settings = s;
        std::call_once(g_initOnce, []() { libssh2_init(0); });
        sock_ = ConnectTcp(s.url.host, s.url.EffectivePort(), s.timeoutMs, err);
        if (sock_ == INVALID_SOCKET) return false;
        session_ = libssh2_session_init();
        if (!session_) return Fail(err = L"SSH-Sitzung konnte nicht angelegt werden.");
        libssh2_session_set_blocking(session_, 1);
        libssh2_session_set_timeout(session_, s.timeoutMs);
        if (libssh2_session_handshake(session_, sock_) != 0) return Fail(err = L"SSH-Verbindungsaufbau fehlgeschlagen: " + LastError());
        if (!CheckHostKey(prompts, err)) return Fail(err);
        if (!Authenticate(prompts, err)) return Fail(err);
        sftp_ = libssh2_sftp_init(session_);
        if (!sftp_) return Fail(err = L"Der Server bietet kein SFTP an: " + LastError());
        char buf[4096] = {};
        int n = libssh2_sftp_realpath(sftp_, ".", buf, sizeof(buf) - 1);
        home_ = n > 0 ? NormalizeRemotePath(Utf8ToWide(std::string(buf, n))) : L"/";
        return true;
    }

    bool Connected() const override { return sftp_ != nullptr; }

    void Disconnect() override {
        if (sftp_) {
            libssh2_sftp_shutdown(sftp_);
            sftp_ = nullptr;
        }
        if (session_) {
            libssh2_session_disconnect(session_, "QFiles beendet die Verbindung");
            libssh2_session_free(session_);
            session_ = nullptr;
        }
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    std::wstring HomeDir() override { return home_; }

    bool List(const std::wstring& path, std::vector<DirEntry>& out, std::wstring& err) override {
        out.clear();
        if (!Ready(err)) return false;
        std::string p = WideToUtf8(path);
        LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_opendir(sftp_, p.c_str());
        if (!h) return SftpFail(err);
        std::vector<char> name(4096), longentry(4096);
        for (;;) {
            LIBSSH2_SFTP_ATTRIBUTES a{};
            int n = libssh2_sftp_readdir_ex(h, name.data(), name.size() - 1, longentry.data(), longentry.size() - 1, &a);
            if (n == 0) break;
            if (n < 0) {
                libssh2_sftp_closedir(h);
                return SftpFail(err);
            }
            std::string nm(name.data(), n);
            if (nm == "." || nm == "..") continue;
            DirEntry d;
            d.name = Utf8ToWide(nm);
            Fill(d, a);
            if ((a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISLNK(a.permissions)) {
                // Verknüpfung: Ziel bestimmt Datei oder Verzeichnis
                LIBSSH2_SFTP_ATTRIBUTES t{};
                std::string full = (p == "/" ? "" : p) + "/" + nm;
                if (libssh2_sftp_stat(sftp_, full.c_str(), &t) == 0) Fill(d, t);
            }
            if (!d.name.empty() && d.name[0] == L'.') d.attributes |= FILE_ATTRIBUTE_HIDDEN;
            out.push_back(std::move(d));
        }
        libssh2_sftp_closedir(h);
        return true;
    }

    bool Stat(const std::wstring& path, DirEntry& out) override {
        std::wstring err;
        if (!Ready(err)) return false;
        LIBSSH2_SFTP_ATTRIBUTES a{};
        std::string p = WideToUtf8(path);
        if (libssh2_sftp_stat(sftp_, p.c_str(), &a) != 0) return false;
        out = DirEntry{};
        out.name = path.substr(path.find_last_of(L'/') + 1);
        Fill(out, a);
        return true;
    }

    bool Download(const std::wstring& path, HANDLE localFile, const ProgressFn& progress, std::wstring& err) override {
        if (!Ready(err)) return false;
        std::string p = WideToUtf8(path);
        LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(sftp_, p.c_str(), LIBSSH2_FXF_READ, 0);
        if (!h) return SftpFail(err);
        std::vector<char> buf(64 * 1024);
        uint64_t total = 0;
        bool ok = true;
        for (;;) {
            ssize_t n = libssh2_sftp_read(h, buf.data(), buf.size());
            if (n == 0) break;
            if (n < 0) {
                ok = SftpFail(err);
                break;
            }
            DWORD w = 0;
            if (!WriteFile(localFile, buf.data(), (DWORD)n, &w, nullptr) || w != (DWORD)n) {
                err = L"Schreibfehler: " + LastErrorMessage();
                ok = false;
                break;
            }
            total += (uint64_t)n;
            if (progress && !progress(total)) {
                err = L"Abgebrochen.";
                ok = false;
                break;
            }
        }
        libssh2_sftp_close(h);
        return ok;
    }

    bool Upload(HANDLE localFile, const std::wstring& path, const ProgressFn& progress, std::wstring& err) override {
        if (!Ready(err)) return false;
        std::string p = WideToUtf8(path);
        LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(sftp_, p.c_str(), LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                                   LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP |
                                                       LIBSSH2_SFTP_S_IROTH);
        if (!h) return SftpFail(err);
        std::vector<char> buf(64 * 1024);
        uint64_t total = 0;
        bool ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(localFile, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
                err = L"Lesefehler: " + LastErrorMessage();
                ok = false;
                break;
            }
            if (got == 0) break;
            size_t sent = 0;
            while (sent < got) {
                ssize_t n = libssh2_sftp_write(h, buf.data() + sent, got - sent);
                if (n < 0) {
                    ok = SftpFail(err);
                    break;
                }
                sent += (size_t)n;
            }
            if (!ok) break;
            total += got;
            if (progress && !progress(total)) {
                err = L"Abgebrochen.";
                ok = false;
                break;
            }
        }
        if (libssh2_sftp_close(h) != 0 && ok) ok = SftpFail(err);
        return ok;
    }

    bool SetModTime(const std::wstring& path, const FILETIME& mtime) override {
        std::wstring err;
        if (!Ready(err)) return false;
        LIBSSH2_SFTP_ATTRIBUTES a{};
        a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
        a.atime = a.mtime = (unsigned long)FileTimeToUnixTime(mtime);
        std::string p = WideToUtf8(path);
        return libssh2_sftp_setstat(sftp_, p.c_str(), &a) == 0;
    }

    bool DeleteFile(const std::wstring& path, std::wstring& err) override {
        if (!Ready(err)) return false;
        std::string p = WideToUtf8(path);
        return libssh2_sftp_unlink(sftp_, p.c_str()) == 0 || SftpFail(err);
    }

    bool RemoveDir(const std::wstring& path, std::wstring& err) override {
        if (!Ready(err)) return false;
        std::string p = WideToUtf8(path);
        return libssh2_sftp_rmdir(sftp_, p.c_str()) == 0 || SftpFail(err);
    }

    bool MakeDir(const std::wstring& path, std::wstring& err) override {
        if (!Ready(err)) return false;
        std::string p = WideToUtf8(path);
        return libssh2_sftp_mkdir(sftp_, p.c_str(), LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP |
                                                        LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH) == 0 ||
               SftpFail(err);
    }

    bool Rename(const std::wstring& from, const std::wstring& to, std::wstring& err) override {
        if (!Ready(err)) return false;
        std::string f = WideToUtf8(from), t = WideToUtf8(to);
        int rc = libssh2_sftp_rename_ex(sftp_, f.c_str(), (unsigned)f.size(), t.c_str(), (unsigned)t.size(),
                                        LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE);
        return rc == 0 || SftpFail(err);
    }

private:
    bool Ready(std::wstring& err) {
        if (sftp_) return true;
        err = L"Keine Verbindung zum Server.";
        return false;
    }

    bool Fail(const std::wstring&) {
        Disconnect();
        return false;
    }

    std::wstring LastError() {
        char* msg = nullptr;
        int len = 0;
        if (!session_) return L"";
        libssh2_session_last_error(session_, &msg, &len, 0);
        return msg ? Utf8ToWide(std::string(msg, len)) : L"";
    }

    // Fehler einer SFTP-Operation; bei Verbindungsverlust Sitzung schließen (nächster Zugriff verbindet neu)
    bool SftpFail(std::wstring& err) {
        int e = session_ ? libssh2_session_last_errno(session_) : 0;
        if (e == LIBSSH2_ERROR_SFTP_PROTOCOL && sftp_) {
            err = SftpStatusText(libssh2_sftp_last_error(sftp_));
        } else {
            err = L"SSH-Fehler: " + LastError();
            if (e == LIBSSH2_ERROR_SOCKET_SEND || e == LIBSSH2_ERROR_SOCKET_RECV || e == LIBSSH2_ERROR_SOCKET_DISCONNECT ||
                e == LIBSSH2_ERROR_TIMEOUT || e == LIBSSH2_ERROR_SOCKET_TIMEOUT)
                Disconnect();
        }
        return false;
    }

    static void Fill(DirEntry& d, const LIBSSH2_SFTP_ATTRIBUTES& a) {
        bool dir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions);
        d.attributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if ((a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && !(a.permissions & LIBSSH2_SFTP_S_IWUSR))
            d.attributes |= FILE_ATTRIBUTE_READONLY;
        d.size = dir ? 0 : ((a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (uint64_t)a.filesize : 0);
        if (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
            d.modified = UnixTimeToFileTime((int64_t)a.mtime);
            d.accessed = UnixTimeToFileTime((int64_t)a.atime);
            d.created = d.modified;
        }
    }

    bool CheckHostKey(const ConnectPrompts* prompts, std::wstring& err) {
        size_t len = 0;
        int type = 0;
        const char* key = libssh2_session_hostkey(session_, &len, &type);
        if (!key || !len) {
            err = L"Der Server hat keinen Hostschlüssel geliefert.";
            return false;
        }
        std::string keyB64 = Base64Encode((const unsigned char*)key, len);
        const char* hash = libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA256);
        std::wstring fingerprint =
            hash ? (L"SHA256:" + Utf8ToWide(Base64Encode((const unsigned char*)hash, 32, false))) : L"(unbekannt)";
        std::wstring hostPort = settings.url.host + L":" + std::to_wstring(settings.url.EffectivePort());
        std::wstring typeName = HostKeyTypeName(type);
        auto hosts = ReadKnownHosts(settings.knownHostsFile);
        bool changed = false;
        for (auto& h : hosts) {
            if (!EqualsI(h.hostPort, hostPort) || h.type != typeName) continue;
            if (h.key == keyB64) return true;
            changed = true;
        }
        if (!prompts || !prompts->confirmHostKey) {
            err = changed ? L"Der Hostschlüssel des Servers hat sich geändert – Verbindung abgelehnt."
                          : L"Unbekannter Server – der Hostschlüssel muss zuerst bestätigt werden.";
            return false;
        }
        if (!prompts->confirmHostKey(hostPort, typeName, fingerprint, changed)) {
            err = L"Verbindung abgebrochen (Hostschlüssel nicht bestätigt).";
            return false;
        }
        hosts.erase(std::remove_if(hosts.begin(), hosts.end(),
                                   [&](const KnownHost& h) { return EqualsI(h.hostPort, hostPort) && h.type == typeName; }),
                    hosts.end());
        hosts.push_back({hostPort, typeName, keyB64});
        if (!settings.knownHostsFile.empty()) WriteKnownHosts(settings.knownHostsFile, hosts);
        return true;
    }

    static void KbdCallback(const char*, int, const char*, int, int numPrompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT*,
                            LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract) {
        auto* self = (SftpFs*)*abstract;
        std::string pw = WideToUtf8(self->kbdPassword_);
        for (int i = 0; i < numPrompts; ++i) {
            responses[i].text = (char*)malloc(pw.size() + 1);
            if (responses[i].text) {
                memcpy(responses[i].text, pw.data(), pw.size() + 1);
                responses[i].length = (unsigned)pw.size();
            }
        }
    }

    bool Authenticate(const ConnectPrompts* prompts, std::wstring& err) {
        std::string user = WideToUtf8(settings.url.user);
        if (user.empty()) {
            err = L"Für SFTP ist ein Benutzername erforderlich.";
            return false;
        }
        const char* list = libssh2_userauth_list(session_, user.c_str(), (unsigned)user.size());
        if (!list) {
            if (libssh2_userauth_authenticated(session_)) return true;
            err = L"Anmeldung nicht möglich: " + LastError();
            return false;
        }
        std::string methods = list;
        // 1. Schlüsseldatei
        if (!settings.keyFile.empty() && methods.find("publickey") != std::string::npos) {
            std::string key = WideToUtf8(settings.keyFile);
            std::string pass = WideToUtf8(settings.password);
            // Pfad in der Systemcodepage (libssh2 öffnet mit fopen)
            int n = WideCharToMultiByte(CP_ACP, 0, settings.keyFile.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string keyAcp(n > 0 ? n - 1 : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_ACP, 0, settings.keyFile.c_str(), -1, keyAcp.data(), n, nullptr, nullptr);
            if (libssh2_userauth_publickey_fromfile_ex(session_, user.c_str(), (unsigned)user.size(), nullptr, keyAcp.c_str(),
                                                       pass.empty() ? nullptr : pass.c_str()) == 0)
                return true;
            err = L"Anmeldung mit Schlüsseldatei fehlgeschlagen: " + LastError();
        }
        // 2. SSH-Agent
        if (methods.find("publickey") != std::string::npos) {
            if (LIBSSH2_AGENT* agent = libssh2_agent_init(session_)) {
                bool ok = false;
                if (libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
                    libssh2_agent_publickey* id = nullptr;
                    libssh2_agent_publickey* prev = nullptr;
                    while (libssh2_agent_get_identity(agent, &id, prev) == 0) {
                        if (libssh2_agent_userauth(agent, user.c_str(), id) == 0) {
                            ok = true;
                            break;
                        }
                        prev = id;
                    }
                    libssh2_agent_disconnect(agent);
                }
                libssh2_agent_free(agent);
                if (ok) return true;
            }
        }
        // 3. Kennwort / keyboard-interactive
        bool pw = methods.find("password") != std::string::npos;
        bool kbd = methods.find("keyboard-interactive") != std::string::npos;
        if (!pw && !kbd) {
            if (err.empty()) err = L"Der Server erlaubt keine Anmeldung mit Kennwort (angeboten: " + Utf8ToWide(methods) + L").";
            return false;
        }
        std::wstring password = settings.keyFile.empty() ? settings.password : L"";
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (password.empty() || attempt > 0) {
                if (!prompts || !prompts->askPassword) {
                    err = L"Kennwort erforderlich.";
                    return false;
                }
                std::wstring prompt = (attempt > 0 ? L"Anmeldung fehlgeschlagen. " : L"") + std::wstring(L"Kennwort für ") +
                                      settings.url.user + L"@" + settings.url.host + L":";
                if (!prompts->askPassword(prompt, password)) {
                    err = L"Anmeldung abgebrochen.";
                    return false;
                }
            }
            std::string p8 = WideToUtf8(password);
            int rc = -1;
            if (pw) rc = libssh2_userauth_password_ex(session_, user.c_str(), (unsigned)user.size(), p8.c_str(), (unsigned)p8.size(), nullptr);
            if (rc != 0 && kbd) {
                kbdPassword_ = password;
                *libssh2_session_abstract(session_) = this;
                rc = libssh2_userauth_keyboard_interactive_ex(session_, user.c_str(), (unsigned)user.size(), KbdCallback);
                kbdPassword_.clear();
            }
            if (rc == 0) {
                settings.password = password;
                return true;
            }
            err = L"Anmeldung fehlgeschlagen: " + LastError();
            if (!prompts || !prompts->askPassword) return false;
        }
        return false;
    }

    SOCKET sock_ = INVALID_SOCKET;
    LIBSSH2_SESSION* session_ = nullptr;
    LIBSSH2_SFTP* sftp_ = nullptr;
    std::wstring home_ = L"/";
    std::wstring kbdPassword_;
};

} // namespace

std::unique_ptr<RemoteFs> CreateSftpFs() { return std::make_unique<SftpFs>(); }

} // namespace qf::remote
