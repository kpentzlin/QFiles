#pragma once
// Dateioperationen über IFileOperation (Unicode, Fortschrittsanzeige, Papierkorb, Konfliktdialoge der Shell)
// mit eigener Rückgängig-Funktion für Kopieren, Verschieben, Umbenennen und Anlegen.
// Alle Operationen werden im Dateisystemmonitor protokolliert (LogOperation).

#include <windows.h>
#include <string>
#include <vector>
#include <utility>

namespace qf {

enum OpFlags : unsigned {
    OpNone = 0,
    OpNoConfirmOverwrite = 1,  // vorhandene Ziele ohne Rückfrage überschreiben
    OpRenameOnCollision = 2,   // bei Namenskonflikt automatisch umbenennen ("Kopie")
    OpSilent = 4,              // kein Fortschrittsdialog
    OpNoUndo = 8,              // nicht in die Rückgängig-Liste
};

// Kopiert/verschiebt Dateien und Verzeichnisse (vollständige Pfade) nach destDir.
bool CopyItems(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destDir, unsigned flags = OpNone);
bool MoveItems(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destDir, unsigned flags = OpNone);
// Kopieren mit individuellem Zielverzeichnis je Quelle (z. B. Synchronisieren). Optional neuer Name (leer = gleich).
struct CopyJob {
    std::wstring source;
    std::wstring destDir;
    std::wstring newName;
};
bool CopyJobs(HWND owner, const std::vector<CopyJob>& jobs, unsigned flags = OpNone);
bool MoveJobs(HWND owner, const std::vector<CopyJob>& jobs, unsigned flags = OpNone);
// Löschen (Papierkorb gemäß Option bzw. permanent).
bool DeleteItems(HWND owner, const std::vector<std::wstring>& paths, bool recycle, bool confirm);
// Umbenennen (nur Name, kein Pfad). Zeigt Fehler an.
bool RenameItem(HWND owner, const std::wstring& path, const std::wstring& newName);
// Neues Verzeichnis / neue leere Datei anlegen (Rückgängig: wieder löschen).
bool CreateFolder(HWND owner, const std::wstring& dir, const std::wstring& name);
bool CreateEmptyFile(HWND owner, const std::wstring& dir, const std::wstring& name);

// ---- Rückgängig ----
// Mehrere Einzeloperationen (z. B. Umbenennen einer Dateigruppe) zu einem Rückgängig-Schritt zusammenfassen.
void BeginUndoGroup(const std::wstring& description);
void EndUndoGroup();
bool CanUndo();
std::wstring UndoDescription();
bool UndoLast(HWND owner);

// Für automatische Tests: keine Rückfragen und keine Fortschrittsanzeige.
void SetFileOpsNonInteractive(bool on);

// ---- Zwischenablage (Dateien, CF_HDROP + "Preferred DropEffect") ----
bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths, bool cut);
// Liefert Dateien aus der Zwischenablage; cut = true bei "Ausschneiden".
bool ClipboardGetFiles(HWND owner, std::vector<std::wstring>& paths, bool& cut);
bool ClipboardHasFiles();

} // namespace qf
