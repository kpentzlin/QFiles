#include "Settings.h"
#include "Util.h"

#include <shlobj.h>
#include <knownfolders.h>

namespace qf {

// ===================== Config =====================

// Werte werden normalerweise unverändert geschrieben (Pfade bleiben lesbar).
// Nur Werte mit Zeilenumbrüchen oder führendem/abschließendem Leerraum bzw. Anführungszeichen
// werden in Anführungszeichen mit Escape-Sequenzen (\\ \n \r \") gespeichert.
static std::wstring EscapeValue(const std::wstring& v) {
    bool needQuote = v.find_first_of(L"\r\n") != std::wstring::npos ||
                     (!v.empty() && (iswspace(v.front()) || iswspace(v.back()) || v.front() == L'"'));
    if (!needQuote) return v;
    std::wstring r = L"\"";
    for (wchar_t c : v) {
        if (c == L'\\') r += L"\\\\";
        else if (c == L'\n') r += L"\\n";
        else if (c == L'\r') r += L"\\r";
        else if (c == L'"') r += L"\\\"";
        else r += c;
    }
    return r + L"\"";
}

static std::wstring UnescapeValue(const std::wstring& raw) {
    std::wstring v = raw;
    if (v.size() < 2 || v.front() != L'"' || v.back() != L'"') return v;
    v = v.substr(1, v.size() - 2);
    std::wstring r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == L'\\' && i + 1 < v.size()) {
            wchar_t n = v[i + 1];
            if (n == L'\\') { r += L'\\'; ++i; continue; }
            if (n == L'n') { r += L'\n'; ++i; continue; }
            if (n == L'r') { r += L'\r'; ++i; continue; }
            if (n == L'"') { r += L'"'; ++i; continue; }
        }
        r += v[i];
    }
    return r;
}

bool Config::Load(const std::wstring& path) {
    path_ = path;
    sections_.clear();
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) return false;
    std::string_view sv((const char*)bytes.data(), bytes.size());
    if (sv.size() >= 3 && (uint8_t)sv[0] == 0xEF && (uint8_t)sv[1] == 0xBB && (uint8_t)sv[2] == 0xBF) sv.remove_prefix(3);
    std::wstring text = Utf8ToWide(sv);
    Section* current = nullptr;
    for (auto& rawLine : Split(text, L'\n', false)) {
        std::wstring line = rawLine;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = Trim(line);
        if (t.empty() || t[0] == L';' || t[0] == L'#') continue;
        if (t.front() == L'[' && t.back() == L']') {
            std::wstring name = t.substr(1, t.size() - 2);
            current = Find(name);
            if (!current) {
                sections_.push_back({name, {}});
                current = &sections_.back();
            }
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || !current) continue;
        std::wstring key = Trim(line.substr(0, eq));
        // Leerraum um unquotierte Werte ignorieren ("Schlüssel = Wert" von Hand bearbeitet); Werte mit
        // bedeutsamem Leerraum am Rand schreibt Save() immer in Anführungszeichen.
        std::wstring value = UnescapeValue(Trim(line.substr(eq + 1)));
        bool found = false;
        for (auto& e : current->entries)
            if (EqualsI(e.first, key)) {
                e.second = value;
                found = true;
            }
        if (!found) current->entries.emplace_back(key, value);
    }
    return true;
}

bool Config::Save() const {
    if (path_.empty()) return false;
    std::wstring dir = PathParent(path_);
    if (!dir.empty() && !DirExists(dir)) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring out = L"; QFiles – Einstellungen (UTF-8)\r\n";
    for (auto& s : sections_) {
        out += L"\r\n[" + s.name + L"]\r\n";
        for (auto& e : s.entries) out += e.first + L"=" + EscapeValue(e.second) + L"\r\n";
    }
    std::string bytes = WideToUtf8(out);
    // Erst in temporäre Datei schreiben, dann ersetzen (kein Datenverlust bei Absturz)
    std::wstring tmp = path_ + L".tmp";
    if (!WriteFileBytes(tmp, bytes.data(), bytes.size())) return false;
    return MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

Config::Section* Config::Find(const std::wstring& name) {
    for (auto& s : sections_)
        if (EqualsI(s.name, name)) return &s;
    return nullptr;
}

const Config::Section* Config::Find(const std::wstring& name) const {
    for (auto& s : sections_)
        if (EqualsI(s.name, name)) return &s;
    return nullptr;
}

std::wstring Config::Get(const std::wstring& section, const std::wstring& key, const std::wstring& def) const {
    if (auto* s = Find(section))
        for (auto& e : s->entries)
            if (EqualsI(e.first, key)) return e.second;
    return def;
}

bool Config::Has(const std::wstring& section, const std::wstring& key) const {
    if (auto* s = Find(section))
        for (auto& e : s->entries)
            if (EqualsI(e.first, key)) return true;
    return false;
}

int Config::GetInt(const std::wstring& section, const std::wstring& key, int def) const {
    if (!Has(section, key)) return def;
    return (int)StrToInt(Get(section, key), def);
}

bool Config::GetBool(const std::wstring& section, const std::wstring& key, bool def) const {
    if (!Has(section, key)) return def;
    std::wstring v = ToLower(Trim(Get(section, key)));
    return v == L"1" || v == L"true" || v == L"ja" || v == L"yes";
}

void Config::Set(const std::wstring& section, const std::wstring& key, const std::wstring& value) {
    Section* s = Find(section);
    if (!s) {
        sections_.push_back({section, {}});
        s = &sections_.back();
    }
    for (auto& e : s->entries)
        if (EqualsI(e.first, key)) {
            e.second = value;
            return;
        }
    s->entries.emplace_back(key, value);
}

void Config::SetInt(const std::wstring& section, const std::wstring& key, int value) {
    Set(section, key, std::to_wstring(value));
}
void Config::SetBool(const std::wstring& section, const std::wstring& key, bool value) {
    Set(section, key, value ? L"1" : L"0");
}

void Config::ClearSection(const std::wstring& section) {
    if (Section* s = Find(section)) s->entries.clear();
}

std::vector<std::wstring> Config::GetList(const std::wstring& section, const std::wstring& prefix) const {
    std::vector<std::wstring> r;
    for (int i = 1;; ++i) {
        std::wstring key = prefix + std::to_wstring(i);
        if (!Has(section, key)) break;
        r.push_back(Get(section, key));
    }
    return r;
}

void Config::SetList(const std::wstring& section, const std::wstring& prefix, const std::vector<std::wstring>& values) {
    // Alte Einträge mit diesem Präfix entfernen
    if (Section* s = Find(section)) {
        std::vector<std::pair<std::wstring, std::wstring>> keep;
        for (auto& e : s->entries) {
            bool isList = StartsWithI(e.first, prefix) && e.first.size() > prefix.size();
            if (isList) {
                for (size_t i = prefix.size(); i < e.first.size(); ++i)
                    if (!iswdigit(e.first[i])) {
                        isList = false;
                        break;
                    }
            }
            if (!isList) keep.push_back(e);
        }
        s->entries = keep;
    }
    for (size_t i = 0; i < values.size(); ++i) Set(section, prefix + std::to_wstring(i + 1), values[i]);
}

std::vector<std::pair<std::wstring, std::wstring>> Config::GetSection(const std::wstring& section) const {
    if (auto* s = Find(section)) return s->entries;
    return {};
}

// ===================== Options =====================

void Options::Load(const Config& c) {
    const wchar_t* S = L"Optionen";
    Options d;
    showHidden = c.GetBool(S, L"VersteckteAnzeigen", d.showHidden);
    showSystem = c.GetBool(S, L"SystemdateienAnzeigen", d.showSystem);
    dirsFirst = c.GetBool(S, L"VerzeichnisseZuerst", d.dirsFirst);
    showGridLines = c.GetBool(S, L"Gitternetz", d.showGridLines);
    sizeInBytes = c.GetBool(S, L"GroesseInBytes", d.sizeInBytes);
    fullRowSelect = c.GetBool(S, L"GanzeZeileMarkieren", d.fullRowSelect);
    showExtensionsColumn = c.GetBool(S, L"SpalteTyp", d.showExtensionsColumn);
    showAttributesColumn = c.GetBool(S, L"SpalteAttribute", d.showAttributesColumn);
    showCreatedColumn = c.GetBool(S, L"SpalteErstellt", d.showCreatedColumn);
    dateWithSeconds = c.GetBool(S, L"DatumSekunden", d.dateWithSeconds);
    thumbnailSize = c.GetInt(S, L"Miniaturgroesse", d.thumbnailSize);
    listFontName = c.Get(S, L"ListenSchrift", d.listFontName);
    listFontSize = c.GetInt(S, L"ListenSchriftgroesse", d.listFontSize);
    confirmDelete = c.GetBool(S, L"LoeschenBestaetigen", d.confirmDelete);
    useRecycleBin = c.GetBool(S, L"Papierkorb", d.useRecycleBin);
    confirmOverwrite = c.GetBool(S, L"UeberschreibenBestaetigen", d.confirmOverwrite);
    singleClickTree = c.GetBool(S, L"BaumEinfachklick", d.singleClickTree);
    treeFollowsPane = c.GetBool(S, L"BaumFolgtListe", d.treeFollowsPane);
    autoRefresh = c.GetBool(S, L"AutoAktualisieren", d.autoRefresh);
    enterOpensEditorForText = c.GetBool(S, L"EingabeOeffnetEditor", d.enterOpensEditorForText);
    quickViewTarget = (QuickViewTarget)c.GetInt(S, L"Dateianzeige", (int)d.quickViewTarget);
    textExtensions = c.Get(S, L"Textdateien", d.textExtensions);
    editorFontName = c.Get(S, L"EditorSchrift", d.editorFontName);
    editorFontSize = c.GetInt(S, L"EditorSchriftgroesse", d.editorFontSize);
    editorTabWidth = c.GetInt(S, L"EditorTabweite", d.editorTabWidth);
    editorWordWrap = c.GetBool(S, L"EditorZeilenumbruch", d.editorWordWrap);
    newFileEncoding = (TextEncoding)c.GetInt(S, L"NeueDateiKodierung", (int)d.newFileEncoding);
    newFileLineEnding = (LineEnding)c.GetInt(S, L"NeueDateiZeilenende", (int)d.newFileLineEnding);
    externalEditor = c.Get(S, L"ExternerEditor", d.externalEditor);
    externalViewer = c.Get(S, L"ExterneAnzeige", d.externalViewer);
    compareTool = c.Get(S, L"Vergleichsprogramm", d.compareTool);
    shellExtExclude = c.Get(S, L"KontextmenueAusschluss", d.shellExtExclude);
    historySize = c.GetInt(S, L"Verlaufslaenge", d.historySize);
    saveOnExit = c.GetBool(S, L"BeimBeendenSpeichern", d.saveOnExit);
    if (thumbnailSize < 32) thumbnailSize = 32;
    if (thumbnailSize > 256) thumbnailSize = 256;
    if (editorTabWidth < 1 || editorTabWidth > 16) editorTabWidth = 4;
    if (historySize < 5) historySize = 5;
    // Aufzählungen aus der INI-Datei begrenzen (ungültige Kodierung würde beim Speichern eine leere Datei schreiben)
    if ((int)quickViewTarget < 0 || (int)quickViewTarget > 1) quickViewTarget = d.quickViewTarget;
    if ((int)newFileEncoding < 0 || (int)newFileEncoding > (int)TextEncoding::Oem) newFileEncoding = d.newFileEncoding;
    if ((int)newFileLineEnding < 0 || (int)newFileLineEnding > (int)LineEnding::CR) newFileLineEnding = d.newFileLineEnding;
}

void Options::Save(Config& c) const {
    const wchar_t* S = L"Optionen";
    c.SetBool(S, L"VersteckteAnzeigen", showHidden);
    c.SetBool(S, L"SystemdateienAnzeigen", showSystem);
    c.SetBool(S, L"VerzeichnisseZuerst", dirsFirst);
    c.SetBool(S, L"Gitternetz", showGridLines);
    c.SetBool(S, L"GroesseInBytes", sizeInBytes);
    c.SetBool(S, L"GanzeZeileMarkieren", fullRowSelect);
    c.SetBool(S, L"SpalteTyp", showExtensionsColumn);
    c.SetBool(S, L"SpalteAttribute", showAttributesColumn);
    c.SetBool(S, L"SpalteErstellt", showCreatedColumn);
    c.SetBool(S, L"DatumSekunden", dateWithSeconds);
    c.SetInt(S, L"Miniaturgroesse", thumbnailSize);
    c.Set(S, L"ListenSchrift", listFontName);
    c.SetInt(S, L"ListenSchriftgroesse", listFontSize);
    c.SetBool(S, L"LoeschenBestaetigen", confirmDelete);
    c.SetBool(S, L"Papierkorb", useRecycleBin);
    c.SetBool(S, L"UeberschreibenBestaetigen", confirmOverwrite);
    c.SetBool(S, L"BaumEinfachklick", singleClickTree);
    c.SetBool(S, L"BaumFolgtListe", treeFollowsPane);
    c.SetBool(S, L"AutoAktualisieren", autoRefresh);
    c.SetBool(S, L"EingabeOeffnetEditor", enterOpensEditorForText);
    c.SetInt(S, L"Dateianzeige", (int)quickViewTarget);
    c.Set(S, L"Textdateien", textExtensions);
    c.Set(S, L"EditorSchrift", editorFontName);
    c.SetInt(S, L"EditorSchriftgroesse", editorFontSize);
    c.SetInt(S, L"EditorTabweite", editorTabWidth);
    c.SetBool(S, L"EditorZeilenumbruch", editorWordWrap);
    c.SetInt(S, L"NeueDateiKodierung", (int)newFileEncoding);
    c.SetInt(S, L"NeueDateiZeilenende", (int)newFileLineEnding);
    c.Set(S, L"ExternerEditor", externalEditor);
    c.Set(S, L"ExterneAnzeige", externalViewer);
    c.Set(S, L"Vergleichsprogramm", compareTool);
    c.Set(S, L"KontextmenueAusschluss", shellExtExclude);
    c.SetInt(S, L"Verlaufslaenge", historySize);
    c.SetBool(S, L"BeimBeendenSpeichern", saveOnExit);
}

std::vector<Bookmark> LoadBookmarks(const Config& c) {
    std::vector<Bookmark> r;
    auto names = c.GetList(L"Lesezeichen", L"Name");
    auto paths = c.GetList(L"Lesezeichen", L"Pfad");
    for (size_t i = 0; i < names.size() && i < paths.size(); ++i) r.push_back({names[i], paths[i]});
    return r;
}

void SaveBookmarks(Config& c, const std::vector<Bookmark>& b) {
    c.ClearSection(L"Lesezeichen");
    std::vector<std::wstring> names, paths;
    for (auto& x : b) {
        names.push_back(x.name);
        paths.push_back(x.path);
    }
    c.SetList(L"Lesezeichen", L"Name", names);
    c.SetList(L"Lesezeichen", L"Pfad", paths);
}

std::vector<FunctionKey> LoadFunctionKeys(const Config& c) {
    std::vector<FunctionKey> keys(kFunctionKeyCount);
    for (int i = 0; i < kFunctionKeyCount; ++i) {
        std::wstring p = L"T" + std::to_wstring(i + 1) + L"_";
        keys[i].label = c.Get(L"Funktionstasten", p + L"Text");
        keys[i].program = c.Get(L"Funktionstasten", p + L"Programm");
        keys[i].params = c.Get(L"Funktionstasten", p + L"Parameter");
        keys[i].startDir = c.Get(L"Funktionstasten", p + L"Verzeichnis");
        keys[i].console = c.GetBool(L"Funktionstasten", p + L"Konsole");
    }
    return keys;
}

void SaveFunctionKeys(Config& c, const std::vector<FunctionKey>& keys) {
    c.ClearSection(L"Funktionstasten");
    for (int i = 0; i < (int)keys.size() && i < kFunctionKeyCount; ++i) {
        if (keys[i].IsEmpty()) continue;
        std::wstring p = L"T" + std::to_wstring(i + 1) + L"_";
        c.Set(L"Funktionstasten", p + L"Text", keys[i].label);
        c.Set(L"Funktionstasten", p + L"Programm", keys[i].program);
        c.Set(L"Funktionstasten", p + L"Parameter", keys[i].params);
        c.Set(L"Funktionstasten", p + L"Verzeichnis", keys[i].startDir);
        c.SetBool(L"Funktionstasten", p + L"Konsole", keys[i].console);
    }
}

std::wstring DetermineConfigPath() {
    // 1. Befehlszeile: /ini="Pfad" oder /ini=Pfad
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring fromCmd;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (StartsWithI(a, L"/ini=") || StartsWithI(a, L"-ini=")) fromCmd = a.substr(5);
    }
    if (argv) LocalFree(argv);
    if (!fromCmd.empty()) return fromCmd;
    // 2. Portabel: QFiles.ini neben der exe
    std::wstring portable = PathCombine(GetExeDir(), L"QFiles.ini");
    if (FileExists(portable)) return portable;
    // 3. Standard: %ProgramData%\QFiles\QFiles.ini
    std::wstring pd = GetKnownFolder(FOLDERID_ProgramData);
    if (pd.empty()) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
        pd = (n && n < MAX_PATH) ? std::wstring(buf, n) : L"C:\\ProgramData";
    }
    return PathCombine(PathCombine(pd, L"QFiles"), L"QFiles.ini");
}

} // namespace qf
