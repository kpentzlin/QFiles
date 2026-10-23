#pragma once
// RecycleBin.cpp – Papierkorb als eigene Ansicht (Ort „Papierkorb“).
//
// Gelöschte Elemente liegen je Laufwerk in „\$Recycle.Bin\<SID>\“ als Paar „$R…“ (Inhalt, Datei oder Verzeichnis)
// und „$I…“ (Kopf: Größe, Löschzeit, ursprünglicher Pfad). Die Liste kommt von der Shell (wie im Explorer, auch
// umgeleitete Benutzerordner); die Angaben je Element liest QFiles aus der $I-Datei (sonst aus den
// Shell-Eigenschaften). Wiederherstellen und endgültiges Löschen erledigt QFiles selbst über die $R/$I-Dateien.

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace qf {

struct RecycledItem {
    std::wstring path;      // $R-Datei bzw. -Verzeichnis (vollständiger Pfad)
    std::wstring name;      // ursprünglicher Name
    std::wstring origDir;   // ursprüngliches Verzeichnis
    FILETIME deleted{};     // Zeitpunkt des Löschens (UTC)
    uint64_t size = 0;      // Größe (bei Verzeichnissen: Gesamtgröße laut $I-Datei)
    DWORD attributes = 0;
    FILETIME created{}, modified{}, accessed{};
};

// Alle Elemente des Papierkorbs (aller Laufwerke). false = Papierkorb nicht lesbar (err).
bool ListRecycleBin(std::vector<RecycledItem>& out, std::wstring& err);

// Ist path ein Element im Papierkorb ($R…-Datei/-Verzeichnis in einem $Recycle.Bin-Ordner)?
bool IsRecycledPath(const std::wstring& path);
// Angaben zu einem $R-Pfad aus der zugehörigen $I-Datei.
bool RecycledInfo(const std::wstring& rPath, RecycledItem& out);

// Wiederherstellen: targetDir leer = an den ursprünglichen Ort (fehlende Verzeichnisse werden angelegt),
// sonst in targetDir (unter dem ursprünglichen Namen). copy = Kopie, das Element bleibt im Papierkorb.
// Rückgabe: Pfade der wiederhergestellten Elemente.
std::vector<std::wstring> RestoreRecycled(HWND owner, const std::vector<std::wstring>& rPaths,
                                          const std::wstring& targetDir = L"", bool copy = false);
// Endgültig löschen (fragt nach, wenn confirm).
bool DeleteRecycled(HWND owner, const std::vector<std::wstring>& rPaths, bool confirm);
// Papierkorb leeren (fragt nach).
bool EmptyRecycleBinAsk(HWND owner);
// Anzahl und Größe aller Elemente (alle Laufwerke)
bool QueryRecycleBin(uint64_t& items, uint64_t& bytes);

} // namespace qf
