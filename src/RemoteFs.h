#pragma once
// Interne Schnittstelle der FTP/SFTP/WebDAV-Clients (nur Remote.cpp, FtpClient.cpp, SftpClient.cpp, WebDavClient.cpp, Tests).
//
// Alle Pfade sind absolute Server-Pfade ("/pfad/datei"), UTF-16. Alle Aufrufe sind blockierend und nicht
// thread-sicher; Remote.cpp serialisiert sie über mutex().

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Location.h"
#include "Util.h"

namespace qf::remote {

// Fortschritt einer Übertragung: übertragene Bytes dieser Datei; Rückgabe false = abbrechen
using ProgressFn = std::function<bool(uint64_t bytesDone)>;

// Rückfragen beim Verbindungsaufbau (nur im UI-Thread möglich; im Arbeitsthread nullptr)
struct ConnectPrompts {
    // Hostschlüssel prüfen: known = bekannter Schlüssel (leer = unbekannt). Rückgabe true = verbinden.
    std::function<bool(const std::wstring& host, const std::wstring& keyType, const std::wstring& fingerprint,
                       bool changed)>
        confirmHostKey;
    // Kennwort erfragen (Eingabe abgebrochen = false)
    std::function<bool(const std::wstring& prompt, std::wstring& password)> askPassword;
};

struct ConnectSettings {
    RemoteUrl url;
    std::wstring password;
    std::wstring keyFile;        // SFTP: privater Schlüssel (optional)
    std::wstring knownHostsFile; // SFTP: bekannte Hostschlüssel
    bool passive = true;         // FTP
    int timeoutMs = 20000;
};

class RemoteFs {
public:
    virtual ~RemoteFs() = default;
    virtual bool Connect(const ConnectSettings& s, const ConnectPrompts* prompts, std::wstring& err) = 0;
    virtual bool Connected() const = 0;
    virtual void Disconnect() = 0;
    virtual std::wstring HomeDir() = 0;    // Startverzeichnis nach der Anmeldung ("/" wenn unbekannt)
    virtual bool List(const std::wstring& path, std::vector<DirEntry>& out, std::wstring& err) = 0;
    // Status eines Pfads: false = existiert nicht (oder Fehler)
    virtual bool Stat(const std::wstring& path, DirEntry& out) = 0;
    virtual bool Download(const std::wstring& path, HANDLE localFile, const ProgressFn& progress, std::wstring& err) = 0;
    virtual bool Upload(HANDLE localFile, const std::wstring& path, const ProgressFn& progress, std::wstring& err) = 0;
    virtual bool SetModTime(const std::wstring& path, const FILETIME& mtime) = 0;
    virtual bool DeleteFile(const std::wstring& path, std::wstring& err) = 0;
    virtual bool RemoveDir(const std::wstring& path, std::wstring& err) = 0;
    virtual bool MakeDir(const std::wstring& path, std::wstring& err) = 0;
    virtual bool Rename(const std::wstring& from, const std::wstring& to, std::wstring& err) = 0;

    std::recursive_mutex& mutex() { return mutex_; }
    // Die gewählten Einstellungen (für erneutes Verbinden ohne Rückfragen)
    ConnectSettings settings;

private:
    std::recursive_mutex mutex_;
};

std::unique_ptr<RemoteFs> CreateFtpFs();
std::unique_ptr<RemoteFs> CreateSftpFs();
std::unique_ptr<RemoteFs> CreateWebDavFs();
// Passender Client für das Protokoll
std::unique_ptr<RemoteFs> CreateRemoteFs(RemoteProto proto);

// ---- gemeinsame Hilfen ----
bool WinsockInit();
// TCP-Verbindung mit Zeitlimit; Rückgabe INVALID_SOCKET bei Fehler (err gefüllt)
SOCKET ConnectTcp(const std::wstring& host, int port, int timeoutMs, std::wstring& err);
std::wstring SocketErrorText(int code);
FILETIME UnixTimeToFileTime(int64_t t);
int64_t FileTimeToUnixTime(const FILETIME& ft);
std::string Base64Encode(const unsigned char* data, size_t len, bool pad = true);
bool Base64Decode(const std::string& s, std::vector<unsigned char>& out);

} // namespace qf::remote
