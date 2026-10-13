#pragma once
// Archive lesen und einzelne Einträge entpacken (für die Archivliste in der Dateianzeige, F11).
// ZIP liest QFiles selbst (Zentralverzeichnis; Entpacken von „Gespeichert“ und „Deflate“), alle übrigen Formate
// (7z, RAR, TAR, GZ, BZ2, XZ, CAB, ISO …) über das Windows-eigene tar.exe (libarchive). Das Archiv wird dabei nie
// verändert.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace qf {

struct ArchiveEntry {
    std::wstring path;          // relativer Pfad im Archiv mit '\' (ohne führendes "./")
    bool isDir = false;
    bool synthetic = false;     // Verzeichnis ohne eigenen Eintrag (aus den Pfaden der Dateien erschlossen)
    bool encrypted = false;
    uint64_t size = 0, packed = 0;
    bool hasPacked = false;
    bool hasDate = false;
    FILETIME modified{};        // UTC
    std::wstring method;        // ZIP: Packverfahren, sonst Rechte (tar)
    // intern
    std::wstring rawName;       // Name wie im Archiv (tar)
    bool zip = false;
    uint16_t zipMethod = 0;
    uint32_t crc = 0;
    uint64_t localOffset = 0;
};

// Gepackte Datei (nach der Endung)?
bool IsArchiveFile(const std::wstring& path);

// Inhaltsverzeichnis lesen. passwordProtected: ohne Kennwort nicht lesbar. cancel: Abbruch (Esc).
bool ReadArchive(const std::wstring& archive, std::vector<ArchiveEntry>& out, std::wstring& error, bool& passwordProtected,
                 const std::atomic<bool>* cancel = nullptr);

// Entpackt die gewählten Einträge (Pfade wie ArchiveEntry::path; Verzeichnisse mit Inhalt) nach destDir. Die gewählten
// Elemente liegen danach direkt in destDir (ohne ihren Pfad im Archiv). Mit Fortschrittsanzeige.
bool ExtractArchiveEntries(HWND owner, const std::wstring& archive, const std::vector<ArchiveEntry>& entries,
                           const std::vector<std::wstring>& selected, const std::wstring& destDir, std::wstring& error);

// Neues temporäres Verzeichnis für entpackte Einträge (unter %TEMP%\QFiles_Archiv)
std::wstring NewArchiveTempDir(const std::wstring& archive);
// Temporäres Verzeichnis wieder entfernen (nur unterhalb von %TEMP%\QFiles_Archiv)
void RemoveArchiveTempDir(const std::wstring& dir);
// Alte temporäre Verzeichnisse (älter als ein Tag) entfernen
void CleanupArchiveTemp();

// ---- Archivliste (Kindfenster der Dateianzeige) ----
HWND CreateArchiveView(HWND parent, int id);
void ArchiveViewSetContent(HWND view, const std::wstring& archive, std::vector<ArchiveEntry> entries);
void ArchiveViewClear(HWND view);
HWND ArchiveViewList(HWND view);   // ListView (Fokusziel)

} // namespace qf
