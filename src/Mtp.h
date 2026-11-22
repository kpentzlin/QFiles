#pragma once
// Smartphones, Tablets, Kameras (MTP/PTP über Windows Portable Devices, WPD).
//
// Adressen wie bei FTP/WebDAV: "mtp://Gerätename/Speicher/Ordner/Datei" (siehe Location.h, RemoteProto::Mtp).
// Der Zugriff läuft über die Schnittstelle RemoteFs (MtpClient.cpp) und damit über dieselben Wege wie FTP/SFTP:
// Auflisten, Kopieren/Verschieben in beide Richtungen mit Fortschritt, Löschen, Ordner anlegen, Umbenennen,
// Anzeigen/Bearbeiten über eine temporäre Kopie.
//
// Hier: die angeschlossenen Geräte (für Laufwerksleiste, Verzeichnisbaum und Lesezeichen). Massenspeicher mit
// Laufwerksbuchstaben (USB-Sticks, Speicherkarten) werden nicht aufgeführt – die erscheinen als Laufwerke.

#include <string>
#include <vector>

namespace qf {

struct MtpDevice {
    std::wstring id;            // PnP-Kennung (für IPortableDevice::Open)
    std::wstring name;          // Anzeigename, eindeutig, ohne '/' (Teil der Adresse)
    std::wstring description;   // Gerätebeschreibung bzw. Hersteller
    bool camera = false;        // Kamera (anderes Symbol)
};

// Angeschlossene Geräte (zwischengespeichert; MtpRefreshDevices liest neu)
const std::vector<MtpDevice>& MtpDevices();
// Neu einlesen; Rückgabe true, wenn sich die Liste geändert hat
bool MtpRefreshDevices();
// Gerät zu einer Adresse ("mtp://Name/…"); nullptr = nicht angeschlossen
const MtpDevice* MtpDeviceForUrl(const std::wstring& url);
// "mtp://Name/"
std::wstring MtpDeviceUrl(const MtpDevice& d);
// Zugriff über WPD in diesem Programm verfügbar (MSVC-Build; im MinGW-Build fehlen die WPD-Header)
bool MtpAvailable();

} // namespace qf
