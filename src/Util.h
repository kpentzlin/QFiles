#pragma once
// Allgemeine Hilfsfunktionen: Zeichenketten, Pfade, Formatierung, Dateien.
// Alle Zeichenketten sind UTF-16 (std::wstring); Umwandlung nach/von UTF-8 nur an Dateigrenzen.

#include <windows.h>
#include <shtypes.h>
#include <knownfolders.h>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace qf {

// ---------- Zeichenketten ----------
std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);
std::wstring ToLower(std::wstring_view s);          // Unicode-korrekt (LCMapStringEx)
std::wstring ToUpper(std::wstring_view s);
std::wstring Trim(std::wstring_view s);
bool EqualsI(std::wstring_view a, std::wstring_view b); // Unicode, ohne Groß-/Kleinschreibung
int CompareI(std::wstring_view a, std::wstring_view b);
int CompareNatural(const std::wstring& a, const std::wstring& b); // wie Explorer (StrCmpLogicalW)
bool StartsWithI(std::wstring_view s, std::wstring_view prefix);
bool EndsWithI(std::wstring_view s, std::wstring_view suffix);
std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep, bool skipEmpty = true);
std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view sep);
std::wstring ReplaceAll(std::wstring s, std::wstring_view from, std::wstring_view to);
std::wstring Format(const wchar_t* fmt, ...);
std::wstring IntToStr(long long v);
std::wstring IntToStrGrouped(unsigned long long v); // 1.234.567 (Gebietsschema)
long long StrToInt(std::wstring_view s, long long def = 0);

// Platzhaltervergleich (* und ?), ohne Groß-/Kleinschreibung. Mehrere Muster durch ';' getrennt.
bool WildcardMatch(std::wstring_view pattern, std::wstring_view name);
bool MatchAnyPattern(const std::wstring& patterns, const std::wstring& name);

// ---------- Pfade ----------
std::wstring PathCombine(const std::wstring& dir, const std::wstring& name);
std::wstring PathParent(const std::wstring& path);        // "" wenn kein Elternverzeichnis
std::wstring PathFileName(const std::wstring& path);       // letztes Glied
std::wstring PathExtension(const std::wstring& path);      // inkl. Punkt, "" wenn keine
std::wstring PathStem(const std::wstring& path);           // Name ohne Erweiterung
std::wstring PathRoot(const std::wstring& path);           // "C:\" bzw. "\\server\share\"
std::wstring NormalizeDir(const std::wstring& path);       // vollständiger Pfad ohne abschließenden '\' (außer Wurzel)
bool IsRootPath(const std::wstring& path);
std::wstring LongPath(const std::wstring& path);           // \\?\-Präfix für lange Pfade (für Win32-Datei-APIs)
// Vollständiger Pfad mit Langnamen statt 8.3-Kurznamen (z. B. RUNNER~1 -> runneradmin); nur für vorhandene Pfade.
std::wstring CanonicalPath(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool PathExists(const std::wstring& path);
std::wstring LastPathElement(const std::wstring& dir);     // für Lesezeichennamen: "C:\" -> "C:"
std::wstring MakeUniqueName(const std::wstring& dir, const std::wstring& name); // "Name (2).txt"
std::wstring GetExeDir();
std::wstring GetKnownFolder(const GUID& id);
std::wstring GetTempDir();

// ---------- Formatierung ----------
std::wstring FormatSize(unsigned long long bytes);          // automatische Einheit (B, KB, MB, GB, TB)
std::wstring FormatSizeBytes(unsigned long long bytes);     // 1.234.567
std::wstring FormatFileTime(const FILETIME& ft, bool withSeconds = false); // lokale Zeit, Gebietsschema
std::wstring FormatAttributes(DWORD attr);                   // "RHSA"
std::wstring LastErrorMessage(DWORD err = GetLastError());

// ---------- Dateien ----------
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, uint64_t maxBytes = UINT64_MAX);
bool WriteFileBytes(const std::wstring& path, const void* data, size_t size);
uint64_t GetFileSize64(const std::wstring& path);   // UINT64_MAX bei Fehler

struct DirEntry {
    std::wstring name;
    DWORD attributes = 0;
    uint64_t size = 0;
    FILETIME created{}, modified{}, accessed{};
    bool IsDir() const { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};
// Liest ein Verzeichnis (ohne "." und ".."). Rückgabe false bei Fehler (z. B. Zugriff verweigert).
bool ListDirectory(const std::wstring& dir, std::vector<DirEntry>& out, DWORD* errorOut = nullptr);
// Rekursive Aufzählung: callback(fullPath, entry) – Rückgabe false bricht ab.
template <class F>
bool WalkDirectory(const std::wstring& dir, F&& callback, bool recursive = true) {
    std::vector<DirEntry> entries;
    if (!ListDirectory(dir, entries)) return true;
    for (auto& e : entries) {
        std::wstring full = PathCombine(dir, e.name);
        if (!callback(full, e)) return false;
        if (recursive && e.IsDir() && !(e.attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (!WalkDirectory(full, callback, recursive)) return false;
        }
    }
    return true;
}

// ---------- UI-Helfer ----------
// Für automatische Tests: Meldungen nach stderr statt MessageBox, Rückfragen = Ja.
void SetMessageBoxesSuppressed(bool on);
void MsgError(HWND owner, const std::wstring& text);
void MsgInfo(HWND owner, const std::wstring& text);
bool MsgConfirm(HWND owner, const std::wstring& text);              // Ja/Nein
int MsgYesNoCancel(HWND owner, const std::wstring& text);           // IDYES/IDNO/IDCANCEL
std::wstring GetWindowTextStr(HWND h);
int DpiScale(HWND h, int value);                                     // Wert für 96 DPI -> aktuelle DPI
UINT GetWindowDpi(HWND h);
bool ClipboardSetText(HWND owner, const std::wstring& text);
std::wstring ClipboardGetText(HWND owner);
// Datei-Dialoge (Unicode, IFileDialog). Rückgabe "" bei Abbruch.
std::wstring BrowseForFolder(HWND owner, const std::wstring& title, const std::wstring& initialDir);
std::wstring OpenFileDialog(HWND owner, const std::wstring& title, const std::wstring& initialDir,
                            const std::wstring& filterSpec = L"Alle Dateien|*.*");
std::wstring SaveFileDialog(HWND owner, const std::wstring& title, const std::wstring& initialPath,
                            const std::wstring& filterSpec = L"Alle Dateien|*.*");
// Einfache Eingabeabfrage (modal). Rückgabe false bei Abbruch.
// selLength >= 0: nur die ersten selLength Zeichen markieren (z. B. Dateiname ohne Endung), sonst alles.
bool InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value,
              const std::wstring& hint = L"", int selLength = -1);
// Länge des Dateinamens ohne Endung („Neue Textdatei.txt“ → 14); ohne Endung die ganze Länge
int FileStemLength(const std::wstring& name);

// Startet ein Programm bzw. öffnet ein Dokument mit der Shell.
bool ShellOpen(HWND owner, const std::wstring& file, const std::wstring& params = L"",
               const std::wstring& dir = L"", const wchar_t* verb = nullptr);
bool RunProcess(const std::wstring& commandLine, const std::wstring& dir, bool wait = false,
                DWORD* exitCode = nullptr, bool hidden = false);
// Führt einen Befehl aus und liefert die Ausgabe (stdout+stderr) als Text (OEM/UTF-8 erkannt).
bool RunAndCapture(const std::wstring& commandLine, const std::wstring& dir, std::wstring& output,
                   DWORD* exitCode = nullptr);

} // namespace qf
