#pragma once
// Orte, die keine gewöhnlichen Verzeichnisse sind:
//
//   Netzwerk (oberste Ebene)   "\\"                   – gefundene Rechner
//   Rechner im Netzwerk        "\\Server"             – Freigaben des Rechners
//   FTP/SFTP-Zugriff           "ftp://benutzer@host:21/pfad", "sftp://benutzer@host:22/pfad"
//
// UNC-Pfade (\\Server\Freigabe\…) sind gewöhnliche Verzeichnisse; über ".." geht es von der Freigabe zum Rechner
// und weiter zur obersten Netzwerkebene.

#include <string>

namespace qf {

extern const wchar_t* const kNetworkRoot;      // "\\"
extern const wchar_t* const kNetworkName;      // "Netzwerk" (Anzeigename)

bool IsNetworkRoot(const std::wstring& p);
bool IsNetworkServer(const std::wstring& p);   // "\\Server" (ohne Freigabe)
bool IsNetworkVirtual(const std::wstring& p);  // Netzwerk oder Rechner
bool IsUncShareRoot(const std::wstring& p);    // "\\Server\Freigabe"
bool IsRemoteUrl(const std::wstring& p);       // ftp:// oder sftp://
bool IsVirtualLocation(const std::wstring& p); // Netzwerk, Rechner oder FTP/SFTP (kein Win32-Dateisystem)
// Netzwerkpfad: UNC, Netzwerk/Rechner oder verbundenes Netzlaufwerk (für die Farbe in der Lesezeichenliste)
bool IsNetworkPath(const std::wstring& p);

// Normalisiert Netzwerk-/FTP-Angaben ("Netzwerk" -> "\\", "\\Server\" -> "\\Server", Schrägstriche im
// URL-Pfad). Rückgabe leer, wenn p kein besonderer Ort ist.
std::wstring NormalizeSpecialLocation(const std::wstring& p);

// Übergeordneter Ort ("" = keiner) und Kombination mit einem Namen – für alle Arten von Orten.
std::wstring LocationParent(const std::wstring& p);
std::wstring LocationCombine(const std::wstring& dir, const std::wstring& name);
std::wstring LocationFileName(const std::wstring& p);
bool LocationHasParent(const std::wstring& p);
// Anzeigename für Titel, Pfadfeld usw. ("\\" -> "Netzwerk")
std::wstring LocationDisplay(const std::wstring& p);

// ---- FTP/SFTP-Adressen ----
enum class RemoteProto { Ftp, Sftp };

struct RemoteUrl {
    RemoteProto proto = RemoteProto::Ftp;
    std::wstring user;     // leer = anonym (FTP)
    std::wstring host;
    int port = 0;          // 0 = Standard (21/22)
    std::wstring path = L"/";

    int EffectivePort() const { return port ? port : (proto == RemoteProto::Sftp ? 22 : 21); }
    // "sftp://benutzer@host:22" (ohne Pfad) – Schlüssel für Verbindungen und Zugangsdaten
    std::wstring ServerKey() const;
    std::wstring ToString() const;   // mit Pfad
};

bool ParseRemoteUrl(const std::wstring& s, RemoteUrl& out);
// Pfadteil normalisieren: beginnt mit '/', keine doppelten '/', "." und ".." aufgelöst, ohne '/' am Ende (außer "/").
std::wstring NormalizeRemotePath(const std::wstring& path);

} // namespace qf
