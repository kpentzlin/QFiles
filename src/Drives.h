#pragma once
// Laufwerke für Kopfzeile der Listen und Verzeichnisbaum: Art (lokal, subst, Netzwerk, CD/DVD, extern, RAM-Disk),
// Symbol, Bezeichnung und Ziel. Diskettenlaufwerke (A:, B:) werden nicht aufgeführt.

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

namespace qf {

enum class DriveKind {
    Local,      // eingebaute Festplatte/SSD
    Subst,      // mit „subst“ (DefineDosDevice) auf ein Verzeichnis abgebildet
    Network,    // Netzlaufwerk
    CdRom,      // CD/DVD/BD
    External,   // Wechseldatenträger, USB-Stick, externe Festplatte (USB, FireWire, SD)
    RamDisk,    // RAM-Disk
};

struct DriveDesc {
    std::wstring root;     // "C:\"
    UINT type = 0;         // GetDriveType
    DriveKind kind = DriveKind::Local;
    std::wstring label;    // Volumebezeichnung (nur bei lokalen Laufwerken, RAM-Disks und Wechseldatenträgern)
    std::wstring target;   // subst: Zielverzeichnis, Netzwerk: verbundene Freigabe
    int icon = 0;          // Index in der kleinen Systemabbildungsliste
    bool linkOverlay = false;   // Verknüpfungspfeil zeichnen (subst)
};

// Alle Laufwerke (Ergebnis wird kurz zwischengespeichert, da Listen und Baum es gleichzeitig abfragen)
const std::vector<DriveDesc>& ListDrives();

// Art als Text („Lokales Laufwerk“, „subst-Laufwerk“ …)
std::wstring DriveKindText(DriveKind kind);
// Zeile für Tooltips: „C: – Lokales Laufwerk – System“ bzw. „S: – subst → C:\Daten“
std::wstring DriveDescription(const DriveDesc& d);

// Symbol zeichnen (mit Verknüpfungspfeil bei subst)
void DrawDriveIcon(HIMAGELIST smallImages, const DriveDesc& d, HDC dc, int x, int y, int size);

} // namespace qf
