#pragma once
// FTP/SFTP für die Dateilisten: Zugänge (Lesezeichen „Neuer FTP/sFTP-Zugriff“), Verbindungen, Dateioperationen.
//
// Adressen: "ftp://benutzer@host[:port]/pfad" bzw. "sftp://benutzer@host[:port]/pfad" (siehe Location.h).
// Kennwörter werden – wenn gewünscht – mit DPAPI (nur für den angemeldeten Windows-Benutzer lesbar)
// verschlüsselt in der Einstellungsdatei gespeichert, Abschnitt [FTP-Zugaenge].
// Bekannte SFTP-Hostschlüssel: %ProgramData%\QFiles\QFiles-Hostschluessel.txt (neben der Einstellungsdatei).
//
// Alle Funktionen sind für den UI-Thread gedacht: Rückfragen (Kennwort, Hostschlüssel) und Fortschritt
// werden dort angezeigt; längere Übertragungen laufen in einem Arbeitsthread mit Fortschrittsdialog.

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#include "Location.h"
#include "Util.h"

namespace qf {

struct RemoteAccess {
    std::wstring name;          // Name des Lesezeichens
    RemoteUrl url;              // Server, Benutzer, Port, Startverzeichnis
    std::wstring password;      // leer = beim Verbinden fragen
    bool savePassword = true;
    std::wstring keyFile;       // SFTP: privater Schlüssel (optional)
};

// Dialog „Neuer FTP/sFTP-Zugriff“ bzw. Zugang bearbeiten. Speichert Kennwort/Schlüsseldatei in den Einstellungen.
bool EditRemoteAccess(HWND owner, RemoteAccess& access, bool isNew);
RemoteAccess LoadRemoteAccess(const std::wstring& url);
// Anmeldung ohne Rückfrage möglich (Kennwort gespeichert, Schlüsseldatei oder anonym)?
bool HasSavedRemoteLogin(const std::wstring& url);

bool RemoteListDirectory(HWND owner, const std::wstring& url, std::vector<DirEntry>& out, std::wstring& err);
bool RemoteRename(HWND owner, const std::wstring& url, const std::wstring& newName);
bool RemoteMakeDir(HWND owner, const std::wstring& dirUrl, const std::wstring& name);
bool RemoteCreateFile(HWND owner, const std::wstring& dirUrl, const std::wstring& name);
bool RemoteExists(HWND owner, const std::wstring& url);
// Löscht names in dirUrl (Verzeichnisse rekursiv); confirm = vorher nachfragen
bool RemoteDelete(HWND owner, const std::wstring& dirUrl, const std::vector<std::wstring>& names, bool confirm);
// Kopieren/Verschieben zwischen lokalen Verzeichnissen und FTP/SFTP (in beide Richtungen und zwischen Servern).
// sources: vollständige Orte (lokale Pfade oder Adressen); targetDir: lokales Verzeichnis oder Adresse.
bool RemoteTransfer(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& targetDir, bool move);
// Datei in ein temporäres Verzeichnis laden (Anzeigen, Öffnen, Bearbeiten). Größere Dateien als maxBytes
// werden abgelehnt (Rückgabe false, err gesetzt). quiet = keine Meldung bei Fehlern.
bool RemoteDownloadTemp(HWND owner, const std::wstring& url, std::wstring& localPath, std::wstring& err,
                        uint64_t maxBytes = UINT64_MAX);

// Bearbeiten: die lokale Kopie wird beobachtet und nach jedem Speichern hochgeladen.
void RemoteEditRegister(const std::wstring& localPath, const std::wstring& url);
// Regelmäßig aufrufen (Zeitgeber des Hauptfensters). Rückgabe: Text für die Statusleiste ("" = nichts geschehen).
std::wstring RemoteEditPoll(HWND owner);

void RemoteDisconnectAll();

} // namespace qf
