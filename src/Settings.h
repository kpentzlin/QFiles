#pragma once
// Einstellungen von QFiles.
//
// Speicherort (Standard): %ProgramData%\QFiles\QFiles.ini (UTF-8 ohne BOM, INI-Syntax).
// Alternativ: Befehlszeile /ini="Pfad" oder eine QFiles.ini neben der QFiles.exe (portabler Betrieb).
//
// Config ist ein allgemeiner Abschnitt/Schlüssel-Speicher (Reihenfolge bleibt erhalten).
// Options enthält die typisierten Programmoptionen; Bookmarks und FunctionKeys stehen in eigenen Abschnitten.

#include <windows.h>
#include <string>
#include <vector>
#include <utility>

namespace qf {

class Config {
public:
    bool Load(const std::wstring& path);
    bool Save() const;
    const std::wstring& FilePath() const { return path_; }
    void SetFilePath(const std::wstring& p) { path_ = p; }

    std::wstring Get(const std::wstring& section, const std::wstring& key, const std::wstring& def = L"") const;
    int GetInt(const std::wstring& section, const std::wstring& key, int def = 0) const;
    bool GetBool(const std::wstring& section, const std::wstring& key, bool def = false) const;
    bool Has(const std::wstring& section, const std::wstring& key) const;
    void Set(const std::wstring& section, const std::wstring& key, const std::wstring& value);
    void SetInt(const std::wstring& section, const std::wstring& key, int value);
    void SetBool(const std::wstring& section, const std::wstring& key, bool value);
    void ClearSection(const std::wstring& section);
    // Liste unter Schlüsseln prefix1..prefixN (z. B. "Dir1", "Dir2" …)
    std::vector<std::wstring> GetList(const std::wstring& section, const std::wstring& prefix) const;
    void SetList(const std::wstring& section, const std::wstring& prefix, const std::vector<std::wstring>& values);
    // Alle Schlüssel/Werte eines Abschnitts
    std::vector<std::pair<std::wstring, std::wstring>> GetSection(const std::wstring& section) const;

private:
    struct Section {
        std::wstring name;
        std::vector<std::pair<std::wstring, std::wstring>> entries;
    };
    Section* Find(const std::wstring& name);
    const Section* Find(const std::wstring& name) const;
    std::vector<Section> sections_;
    std::wstring path_;
};

enum class QuickViewTarget { OtherPane = 0, Window = 1 };
enum class TextEncoding { Utf8 = 0, Utf8Bom = 1, Utf16LE = 2, Utf16BE = 3, Ansi = 4, Oem = 5 };
enum class LineEnding { CRLF = 0, LF = 1, CR = 2 };

struct Options {
    // Anzeige
    bool showHidden = false;
    bool showSystem = false;
    bool dirsFirst = true;
    bool showGridLines = false;
    bool sizeInBytes = false;          // Größe immer in Bytes statt automatischer Einheit
    bool fullRowSelect = true;
    bool showExtensionsColumn = true;
    bool showAttributesColumn = true;
    bool showCreatedColumn = false;    // zusätzliche Spalte Erstellungsdatum (neben Änderungsdatum)
    bool dateWithSeconds = false;      // Dateidatum sekundengenau
    int thumbnailSize = 96;            // Pixel
    std::wstring listFontName = L"Segoe UI";
    int listFontSize = 9;              // Punkt
    // Bedienung
    bool confirmDelete = true;
    bool useRecycleBin = true;
    bool confirmOverwrite = true;      // (IFileOperation fragt selbst bei Konflikten)
    bool singleClickTree = true;       // Klick im Baum zeigt Verzeichnis in aktiver Liste
    bool treeFollowsPane = true;       // Baum folgt dem Verzeichnis der aktiven Liste
    bool autoRefresh = true;           // Verzeichnisüberwachung
    bool enterOpensEditorForText = false;
    QuickViewTarget quickViewTarget = QuickViewTarget::OtherPane; // "Dateianzeige: in dem jeweils anderen Fenster"
    std::wstring textExtensions = L"*.txt;*.log;*.ini;*.cfg;*.conf;*.md;*.csv;*.xml;*.json;*.yml;*.yaml;*.htm;*.html;*.css;*.js;*.ts;*.c;*.cpp;*.h;*.hpp;*.cs;*.java;*.py;*.pl;*.rb;*.php;*.sql;*.bat;*.cmd;*.ps1;*.sh;*.reg;*.inf;*.nfo;*.tex;*.rc;*.cmake;*.gitignore";
    // Editor
    std::wstring editorFontName = L"Consolas";
    int editorFontSize = 10;
    int editorTabWidth = 4;
    bool editorWordWrap = false;
    TextEncoding newFileEncoding = TextEncoding::Utf8;   // leere/neue Dateien: UTF-8 ohne BOM
    LineEnding newFileLineEnding = LineEnding::CRLF;
    // Externe Programme
    std::wstring externalEditor;       // leer = integrierter Editor
    std::wstring externalViewer;       // leer = integrierte Anzeige
    std::wstring compareTool;          // leer = integrierter Vergleich
    // Sonstiges
    int historySize = 30;
    bool saveOnExit = true;

    void Load(const Config& c);
    void Save(Config& c) const;
};

struct Bookmark {
    std::wstring name;
    std::wstring path;
};

struct FunctionKey {
    std::wstring label;      // Beschriftung
    std::wstring program;    // Programm, Dokument oder Befehl
    std::wstring params;     // Parameter mit Platzhaltern (%P %F %N %S %O …)
    std::wstring startDir;   // Arbeitsverzeichnis (leer = aktuelles Verzeichnis)
    bool console = false;    // in Konsolenfenster ausführen (cmd /k)
    bool IsEmpty() const { return program.empty(); }
};

// 24 programmierbare Funktionstasten: Index 0–11 = Strg+F1…F12, 12–23 = Strg+Umschalt+F1…F12
constexpr int kFunctionKeyCount = 24;

std::vector<Bookmark> LoadBookmarks(const Config& c);
void SaveBookmarks(Config& c, const std::vector<Bookmark>& b);
std::vector<FunctionKey> LoadFunctionKeys(const Config& c);
void SaveFunctionKeys(Config& c, const std::vector<FunctionKey>& keys);

// Ermittelt den Pfad der Einstellungsdatei (Befehlszeile, portabel, %ProgramData%\QFiles\QFiles.ini)
std::wstring DetermineConfigPath();

} // namespace qf
