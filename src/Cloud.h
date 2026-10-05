#pragma once
// Cloud-Speicher (Dropbox, Google Drive, Microsoft OneDrive) über die Ordner ihrer Windows-Programme.
//
// QFiles greift nicht selbst über die Web-Schnittstellen der Anbieter zu (dafür wäre je Anbieter eine
// registrierte Anwendung mit Anmeldung über den Browser nötig). Stattdessen werden die Ordner erkannt, die die
// Desktop-Programme der Anbieter bereitstellen (Dropbox-Ordner, Google-Drive-Laufwerk, OneDrive-Ordner); sie
// verhalten sich wie gewöhnliche Verzeichnisse und werden vom jeweiligen Programm synchronisiert.

#include <string>
#include <vector>

namespace qf {

struct CloudFolder {
    std::wstring service;   // "Dropbox", "Google Drive", "OneDrive"
    std::wstring name;      // Vorschlag für den Lesezeichennamen (z. B. "OneDrive – Firma")
    std::wstring path;
};

// Gefundene Cloud-Ordner (zwischengespeichert; refresh = neu suchen)
const std::vector<CloudFolder>& DetectCloudFolders(bool refresh = false);
// Liegt der Pfad in einem Cloud-Ordner (oder ist er einer)?
bool IsCloudPath(const std::wstring& path);

} // namespace qf
