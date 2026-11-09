#pragma once
// PaneFilter.cpp – Filter einer Dateiliste (Trichter in der Kopfleiste): Masken, Verzeichnisse, Datum, Größe,
// Attribute, „nur gemäß .gitignore zum Projekt gehörig“ und Sortierung. Alle gewählten Bedingungen gelten zusammen
// („und“). Dazu die .gitignore-Auswertung (Muster wie bei git: !, /, **, *, ?, [..]).

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace qf {

struct DirEntry;

// .gitignore-Dateien des Verzeichnisses und der direkt übergeordneten Verzeichnisse (bis zum Projektverzeichnis mit
// „.git“ bzw. bis zur Laufwerkswurzel), in der Reihenfolge von oben nach unten ausgewertet.
class GitIgnore {
public:
    // false, wenn keine .gitignore gefunden wurde
    bool Load(const std::wstring& dir);
    // Regeln direkt aus Text (für Tests): base = Verzeichnis der .gitignore relativ zum Wurzelverzeichnis ("" oder "a/b")
    void AddRules(const std::wstring& text, const std::wstring& base = L"");
    void SetRoot(const std::wstring& root) { root_ = root; }
    bool Empty() const { return rules_.empty(); }
    // relPath: Pfad relativ zum Wurzelverzeichnis mit '/' (z. B. "src/main.cpp"); prüft auch die übergeordneten
    // Verzeichnisse (ein ausgeschlossenes Verzeichnis schließt seinen Inhalt aus)
    bool Ignored(const std::wstring& relPath, bool isDir) const;
    // vollständiger Pfad
    bool IgnoredPath(const std::wstring& fullPath, bool isDir) const;
    const std::wstring& Root() const { return root_; }
    const std::vector<std::wstring>& Files() const { return files_; }   // gefundene .gitignore (oben zuerst)

private:
    struct Rule {
        std::wstring base;      // Verzeichnis der .gitignore relativ zur Wurzel ("" oder "a/b")
        std::wstring pattern;   // ohne führendes '!' und ohne abschließendes '/'
        bool negate = false, dirOnly = false, anchored = false;
    };
    int MatchOne(const std::wstring& rel, bool isDir) const;   // 1 ignoriert, -1 wieder aufgenommen, 0 keine Regel
    std::wstring root_;
    std::vector<Rule> rules_;
    std::vector<std::wstring> files_;
};

// Glob wie in .gitignore: '*' und '?' ohne '/', '**' über Verzeichnisgrenzen, [abc] / [!a-z]; ohne Groß/klein
bool GitGlobMatch(std::wstring_view pattern, std::wstring_view text);

enum class FilterSort {
    None = 0, NameAsc, NameDesc, TypeAsc, TypeDesc, SizeAsc, SizeDesc, CreatedAsc, CreatedDesc, ModifiedAsc, ModifiedDesc
};

struct PaneFilter {
    bool active = false;
    std::wstring masks = L"*";      // mehrere durch '/' (oder ';') getrennt, „oder“
    std::wstring exclude;           // auszuschließende Masken (durch '/' oder ';' getrennt)
    int dirMode = 0;                // Verzeichnisse: 0 alle, 1 ohne, 2 laut Maske
    // Datum: 0 egal, 1 von/bis (Tageszahlen JJJJMMTT, 0 = offen), 2 seit days Tagen
    int modMode = 0, modFrom = 0, modTo = 0, modDays = 7;
    int crMode = 0, crFrom = 0, crTo = 0, crDays = 7;
    int64_t sizeMin = -1, sizeMax = -1;   // Bytes, -1 = offen
    int attr[4] = {-1, -1, -1, -1};       // schreibgeschützt, versteckt, System, Archiv: -1 egal, 0 nicht, 1 gesetzt
    bool git = false;               // nur gemäß .gitignore zum Projekt gehörig
    int sort = 0;                   // FilterSort

    bool MaskActive() const;
    // Kurztext für die Pfadzeile, z. B. „*.docx / *.xlsx, geändert seit 7 Tagen, Git“
    std::wstring ShortText() const;
    std::wstring ToString() const;              // für INI und Lesezeichen
    static bool FromString(const std::wstring& s, PaneFilter& out);
};

// Prüft einen Eintrag (Datei oder Verzeichnis); fullPath für den Git-Filter; git darf nullptr sein.
// nowDay = heutiger Tag (Tage seit 1601) für „seit N Tagen“.
bool FilterAccepts(const PaneFilter& f, const DirEntry& e, const std::wstring& fullPath, const GitIgnore* git);

// Hilfen: Tageszahl JJJJMMTT aus FILETIME (Ortszeit), Größe aus Text („10 KB“, „1,5 MB“), Größe als Text
int DayNumber(const FILETIME& ft);
int TodayNumber();
int AddDays(int yyyymmdd, int days);
bool ParseSizeText(const std::wstring& text, int64_t& out);
std::wstring SizeShortText(int64_t bytes);

} // namespace qf
