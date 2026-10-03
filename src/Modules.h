#pragma once
// Schnittstellen der Funktionsmodule. Das Hauptfenster ruft nur diese Funktionen auf.
// Jedes Modul liegt in eigenen .cpp/.h-Dateien (siehe Kommentar je Block).
//
// Gemeinsame Konventionen:
// - Alle Texte deutsch, alle Zeichenketten UTF-16 (std::wstring), Dateinamen voll Unicode.
// - owner = Elternfenster für modale Dialoge.
// - Dialoge über DialogTemplate/DialogBase (Dialog.h); nicht modale Fenster registrieren sich bei App.
// - Dateioperationen über FileOps.h (Protokoll + Rückgängig).

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#include "App.h"
#include "Settings.h"

namespace qf {

// ===================== TextEditor.cpp (Modul A) =====================
// Öffnet ein eigenes Editorfenster (nicht modal, beliebig viele).
// path existiert  -> Datei laden (Kodierung automatisch erkennen).
// path existiert nicht -> neue Datei mit diesem Namen (wird beim Speichern angelegt).
// path leer -> "Unbenannt".
// Leere/neue Dateien: Kodierung = Options::newFileEncoding (Standard UTF-8 ohne BOM).
void OpenTextEditor(const std::wstring& path);

// ===================== Viewer.cpp / HexView.cpp (Modul B) =====================
// Preview = Windows-Vorschauhandler (IPreviewHandler) für PDF, Office, HTML, RTF … sofern installiert.
enum class ViewerMode { Auto = 0, Text = 1, Hex = 2, Image = 3, Preview = 4 };
// Anzeige-Steuerelement (Kindfenster) für die Schnellansicht im jeweils anderen Listenfenster.
HWND CreateFileViewer(HWND parent, int id);
// Lädt eine Datei (leer = leeren). Bei Verzeichnissen: Übersicht (Anzahl Dateien, Gesamtgröße).
void FileViewerLoad(HWND viewer, const std::wstring& path);
void FileViewerSetMode(HWND viewer, ViewerMode mode);
ViewerMode FileViewerGetMode(HWND viewer);
// Eigenständiges Anzeigefenster (F3), nicht modal.
void OpenViewerWindow(const std::wstring& path);
// Hex-Editor-Fenster (nicht modal) zum Bearbeiten von Binärdateien.
void OpenHexEditor(const std::wstring& path);

// ===================== CompareFiles.cpp / CompareDirs.cpp / SyncDirs.cpp (Modul C) =====================
// Vergleicht zwei Dateien (Text zeilenweise mit Farbmarkierung, sonst binär). Nicht modales Fenster.
void CompareFiles(HWND owner, const std::wstring& fileA, const std::wstring& fileB);

enum class CompareMark : uint8_t {
    None = 0,
    Equal,        // gleich
    Different,    // gleicher Name, unterschiedlich
    Newer,        // neuer als die Gegenseite
    Older,        // älter als die Gegenseite
    Missing,      // nur auf dieser Seite vorhanden
    Duplicate,    // Inhalt identisch mit einer anderen Datei (Doppelte)
};
// Schlüssel = Dateiname in Kleinbuchstaben (ToLower), Wert = Markierung
using CompareMarks = std::unordered_map<std::wstring, CompareMark>;
// Optionen-Dialog + Vergleich der beiden Verzeichnisse. Rückgabe false bei Abbruch.
// selectLeft/selectRight erhalten die Namen, die laut gewählter Option markiert werden sollen.
bool CompareDirectories(HWND owner, const std::wstring& left, const std::wstring& right, CompareMarks& marksLeft,
                        CompareMarks& marksRight, std::vector<std::wstring>& selectLeft,
                        std::vector<std::wstring>& selectRight);
// Synchronisieren zweier Verzeichnisse (modal, mit Vorschau). Rückgabe true, wenn etwas geändert wurde.
bool SyncDirectories(HWND owner, const std::wstring& left, const std::wstring& right);

// ===================== BatchRename.cpp / FindFiles.cpp / Duplicates.cpp / SplitJoin.cpp (Modul D) =====================
// Dateigruppe umbenennen (modal, Vorschau). names = Namen im Verzeichnis dir. Rückgabe true bei Änderungen.
bool BatchRename(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names);
// Dateien suchen (nicht modal). Ergebnis-Doppelklick -> App::NavigateToFile.
void FindFiles(HWND owner, const std::wstring& startDir);
// Doppelte Dateien (gleicher Inhalt) suchen (nicht modal).
void FindDuplicates(HWND owner, const std::wstring& startDir);
// Datei in Teile zerlegen (modal). Rückgabe true bei Erfolg.
bool SplitFile(HWND owner, const std::wstring& file, const std::wstring& targetDir);
// Teile (Name.001, Name.002 …) wieder zusammenfügen (modal).
bool JoinFiles(HWND owner, const std::wstring& firstPart, const std::wstring& targetDir);

// ===================== Tools.cpp u. a. (Modul E) =====================
// Laufwerksübersicht (modal): Typ, Bezeichnung, Dateisystem, Gesamt, Frei, Belegung. Doppelklick -> NavigateTo.
void DriveOverview(HWND owner);
// Attribute und Zeitstempel ändern (modal), optional rekursiv, optional Datum aus EXIF (JPEG).
bool ChangeAttributes(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names);
// Funktionstasten konfigurieren (modal). Speichert in App::Cfg() (SaveFunctionKeys).
bool ConfigureFunctionKeys(HWND owner);
// Funktionstaste ausführen (index 0–23). Platzhalter siehe Settings.h/README.
void ExecuteFunctionKey(HWND owner, int index, const PaneContext& ctx);
// Verzeichnisliste drucken oder als Textdatei speichern (modal), optional mit Unterverzeichnissen.
void PrintDirectoryListing(HWND owner, const std::wstring& dir);
// Archive (ZIP, 7z, TAR, CAB, RAR*) über das Windows-eigene tar.exe: Inhalt anzeigen/entpacken (modal).
void ShowArchive(HWND owner, const std::wstring& archive, const std::wstring& extractDir);
// ZIP-Archiv aus Auswahl erstellen (modal). Rückgabe true bei Erfolg.
bool CreateZipArchive(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names,
                      const std::wstring& targetDir);
// Optionen-Dialog (modal, mit Registerkarten). Rückgabe true, wenn geändert (Hauptfenster aktualisiert dann).
bool ShowOptionsDialog(HWND owner);

struct PaneFilter {
    std::wstring include = L"*";     // Muster, durch ';' getrennt
    std::wstring exclude;            // Muster, durch ';' getrennt
    bool applyToDirs = false;        // Muster auch auf Verzeichnisse anwenden
    bool IsActive() const { return !(include.empty() || include == L"*" || include == L"*.*") || !exclude.empty(); }
};
// Dateifilter bearbeiten (modal). Rückgabe true bei OK.
bool EditFilter(HWND owner, PaneFilter& filter);
// Info-Dialog
void ShowAbout(HWND owner);

} // namespace qf
