// BatchRename.cpp – Modul D: Dateigruppe umbenennen
//
// Modaler, in der Größe veränderbarer Dialog mit Namens- und Erweiterungsmaske, Zähler,
// Suchen/Ersetzen (auch reguläre Ausdrücke), Groß-/Kleinschreibung, optional rekursiv,
// Live-Vorschau mit Konfliktprüfung und zweiphasiger Ausführung (Zyklen, reine
// Groß-/Kleinschreibungsänderungen) als ein Rückgängig-Schritt.
//
// Platzhalter-Syntax (Positionen zählen Unicode-Zeichen, d. h. Surrogatpaare als ein Zeichen;
// negative Positionen zählen vom Ende, -1 = letztes Zeichen):
//   [N]        Name ohne Erweiterung
//   [N3]       3. Zeichen des Namens
//   [N2-5]     Zeichen 2 bis 5
//   [N2-]      ab dem 2. Zeichen bis zum Ende
//   [N-3]      die letzten 3 Zeichen
//   [N2,4]     4 Zeichen ab dem 2. Zeichen
//   [N2--2]    vom 2. bis zum vorletzten Zeichen
//   [E] …      Erweiterung ohne Punkt, Bereichsangaben wie bei [N]
//   [C]        Zähler (Start, Schrittweite, Stellenzahl in eigenen Feldern)
//   [D]        Änderungsdatum JJJJMMTT (Ortszeit)
//   [T]        Änderungsuhrzeit hhmmss (Ortszeit)
//   [d] / [t]  Änderungsdatum JJJJ-MM-TT / Uhrzeit hh-mm-ss (Ortszeit)
//   [P]        Name des Elternverzeichnisses
//   [[  ]]     eckige Klammer auf / zu
// Groß-/Kleinbuchstaben der Platzhalterbuchstaben sind gleichwertig ([n] = [N]).

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "FileOps.h"
#include "Settings.h"
#include "Util.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qf {

namespace {

constexpr wchar_t kSection[] = L"Umbenennen";
constexpr int kHistoryMax = 15;
constexpr UINT_PTR kTimerPreview = 1;

enum : int {
    CID_LBL_NAME = 101,
    CID_NAMEMASK,
    CID_LBL_EXT,
    CID_EXTMASK,
    CID_INS_N,
    CID_INS_NRANGE,
    CID_INS_E,
    CID_INS_C,
    CID_INS_D,
    CID_INS_DD,
    CID_INS_T,
    CID_INS_TD,
    CID_INS_P,
    CID_INS_LB,
    CID_INS_RB,
    CID_RESET,
    CID_HELP,
    CID_GRP_COUNTER,
    CID_LBL_START,
    CID_START,
    CID_SPIN_START,
    CID_LBL_STEP,
    CID_STEP,
    CID_SPIN_STEP,
    CID_LBL_DIGITS,
    CID_DIGITS,
    CID_SPIN_DIGITS,
    CID_GRP_REPLACE,
    CID_LBL_SEARCH,
    CID_SEARCH,
    CID_LBL_REPLACE,
    CID_REPLACE,
    CID_MATCHCASE,
    CID_REGEX,
    CID_FIRSTONLY,
    CID_GRP_CASE,
    CID_LBL_CASENAME,
    CID_CASENAME,
    CID_LBL_CASEEXT,
    CID_CASEEXT,
    CID_RECURSIVE,
    CID_PERDIR,
    CID_RENAMEDIRS,
    CID_LIST,
    CID_STATUS,
};

// ---------------------------------------------------------------------------
// Masken
// ---------------------------------------------------------------------------

enum class TokKind { Literal, Name, Ext, Counter, Date, Time, DateDash, TimeDash, Parent };
enum class RangeKind { Whole, Single, Range, OpenEnd, Count, LastN };

struct Tok {
    TokKind kind = TokKind::Literal;
    std::wstring text;              // nur Literal
    RangeKind range = RangeKind::Whole;
    int a = 0, b = 0;               // Positionen/Anzahl je nach RangeKind
};

// Liest eine ganze Zahl mit optionalem Minus. Rückgabe false, wenn keine Ziffer folgt.
bool ParseSigned(const std::wstring& s, size_t& i, int& v) {
    bool neg = false;
    size_t p = i;
    if (p < s.size() && s[p] == L'-') {
        neg = true;
        ++p;
    }
    if (p >= s.size() || s[p] < L'0' || s[p] > L'9') return false;
    long long x = 0;
    while (p < s.size() && s[p] >= L'0' && s[p] <= L'9') {
        x = x * 10 + (s[p] - L'0');
        if (x > 100000) x = 100000;
        ++p;
    }
    v = (int)(neg ? -x : x);
    i = p;
    return true;
}

// Bereichsangabe nach dem Buchstaben N bzw. E auswerten.
bool ParseRange(const std::wstring& r, Tok& t) {
    if (r.empty()) {
        t.range = RangeKind::Whole;
        return true;
    }
    size_t i = 0;
    int a = 0;
    if (!ParseSigned(r, i, a) || a == 0) return false;
    if (i == r.size()) {
        if (a < 0) {
            t.range = RangeKind::LastN;   // [N-3] = die letzten 3 Zeichen
            t.a = -a;
        } else {
            t.range = RangeKind::Single;
            t.a = a;
        }
        return true;
    }
    if (r[i] == L',') {
        ++i;
        int n = 0;
        if (!ParseSigned(r, i, n) || n < 0 || i != r.size()) return false;
        t.range = RangeKind::Count;
        t.a = a;
        t.b = n;
        return true;
    }
    if (r[i] == L'-') {
        ++i;
        if (i == r.size()) {
            t.range = RangeKind::OpenEnd;
            t.a = a;
            return true;
        }
        int b = 0;
        if (!ParseSigned(r, i, b) || b == 0 || i != r.size()) return false;
        t.range = RangeKind::Range;
        t.a = a;
        t.b = b;
        return true;
    }
    return false;
}

bool CompileMask(const std::wstring& mask, std::vector<Tok>& out, std::wstring& err) {
    out.clear();
    std::wstring lit;
    auto flush = [&]() {
        if (!lit.empty()) {
            Tok t;
            t.kind = TokKind::Literal;
            t.text = lit;
            out.push_back(std::move(t));
            lit.clear();
        }
    };
    for (size_t i = 0; i < mask.size(); ++i) {
        wchar_t c = mask[i];
        if (c == L'[') {
            if (i + 1 < mask.size() && mask[i + 1] == L'[') {
                lit += L'[';
                ++i;
                continue;
            }
            size_t close = mask.find(L']', i + 1);
            if (close == std::wstring::npos) {
                err = L"Fehlende schließende Klammer „]“ (für eine eckige Klammer „[[“ verwenden).";
                return false;
            }
            std::wstring content = mask.substr(i + 1, close - i - 1);
            if (content.empty()) {
                err = L"Leerer Platzhalter „[]“.";
                return false;
            }
            Tok t;
            wchar_t k = (wchar_t)towupper(content[0]);
            std::wstring rest = content.substr(1);
            bool ok = true;
            switch (k) {
            case L'N':
                t.kind = TokKind::Name;
                ok = ParseRange(rest, t);
                break;
            case L'E':
                t.kind = TokKind::Ext;
                ok = ParseRange(rest, t);
                break;
            case L'C':
                t.kind = TokKind::Counter;
                ok = rest.empty();
                break;
            case L'D':
                // [D] = JJJJMMTT, [d] = JJJJ-MM-TT
                t.kind = content[0] == L'd' ? TokKind::DateDash : TokKind::Date;
                ok = rest.empty();
                break;
            case L'T':
                // [T] = hhmmss, [t] = hh-mm-ss
                t.kind = content[0] == L't' ? TokKind::TimeDash : TokKind::Time;
                ok = rest.empty();
                break;
            case L'P':
                t.kind = TokKind::Parent;
                ok = rest.empty();
                break;
            default:
                ok = false;
            }
            if (!ok) {
                err = L"Unbekannter oder ungültiger Platzhalter „[" + content + L"]“.";
                return false;
            }
            flush();
            out.push_back(std::move(t));
            i = close;
        } else if (c == L']') {
            // "]]" = eine Klammer; eine einzelne "]" wird ebenfalls wörtlich übernommen
            lit += L']';
            if (i + 1 < mask.size() && mask[i + 1] == L']') ++i;
        } else {
            lit += c;
        }
    }
    flush();
    return true;
}

// Anfangsindizes der Unicode-Zeichen (Surrogatpaare zusammen) plus Endindex.
std::vector<size_t> CodePointOffsets(const std::wstring& s) {
    std::vector<size_t> o;
    o.reserve(s.size() + 1);
    for (size_t i = 0; i < s.size();) {
        o.push_back(i);
        if (IS_HIGH_SURROGATE(s[i]) && i + 1 < s.size() && IS_LOW_SURROGATE(s[i + 1]))
            i += 2;
        else
            ++i;
    }
    o.push_back(s.size());
    return o;
}

// Teilzeichenkette nach Zeichenpositionen (1-basiert, inklusive), außerhalb liegende Teile werden abgeschnitten.
std::wstring SubCp(const std::wstring& s, const std::vector<size_t>& off, long long from, long long to) {
    long long len = (long long)off.size() - 1;
    if (from < 1) from = 1;
    if (to > len) to = len;
    if (from > to) return L"";
    return s.substr(off[(size_t)from - 1], off[(size_t)to] - off[(size_t)from - 1]);
}

std::wstring ApplyRange(const std::wstring& s, const Tok& t) {
    if (t.range == RangeKind::Whole) return s;
    std::vector<size_t> off = CodePointOffsets(s);
    long long len = (long long)off.size() - 1;
    auto pos = [len](int p) -> long long { return p > 0 ? p : len + 1 + p; };
    switch (t.range) {
    case RangeKind::Single: {
        long long p = pos(t.a);
        return SubCp(s, off, p, p);
    }
    case RangeKind::Range:
        return SubCp(s, off, pos(t.a), pos(t.b));
    case RangeKind::OpenEnd:
        return SubCp(s, off, pos(t.a), len);
    case RangeKind::Count: {
        long long p = pos(t.a);
        return SubCp(s, off, p, p + t.b - 1);
    }
    case RangeKind::LastN:
        return SubCp(s, off, len - t.a + 1, len);
    default:
        return s;
    }
}

std::wstring FormatCounter(long long v, int digits) {
    bool neg = v < 0;
    unsigned long long a = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
    std::wstring s = std::to_wstring(a);
    if ((int)s.size() < digits) s.insert(0, (size_t)(digits - (int)s.size()), L'0');
    if (neg) s.insert(0, 1, L'-');
    return s;
}

// ---------------------------------------------------------------------------
// Groß-/Kleinschreibung
// ---------------------------------------------------------------------------

enum CaseMode { CaseKeep = 0, CaseLower, CaseUpper, CaseFirst, CaseWords };

bool IsWordChar(const std::wstring& s, size_t i) {
    wchar_t c = s[i];
    if (IS_HIGH_SURROGATE(c) || IS_LOW_SURROGATE(c)) return true; // Zeichen außerhalb der BMP als Wortzeichen
    return IsCharAlphaNumericW(c) != FALSE || c == L'\'' || c == 0x2019;
}

std::wstring ApplyCase(const std::wstring& s, int mode) {
    switch (mode) {
    case CaseLower:
        return ToLower(s);
    case CaseUpper:
        return ToUpper(s);
    case CaseFirst: {
        std::wstring r = ToLower(s);
        std::vector<size_t> off = CodePointOffsets(r);
        for (size_t k = 0; k + 1 < off.size(); ++k) {
            size_t b = off[k], e = off[k + 1];
            if (IS_HIGH_SURROGATE(r[b]) || IsCharAlphaW(r[b])) {
                std::wstring up = ToUpper(r.substr(b, e - b));
                r.replace(b, e - b, up);
                break;
            }
        }
        return r;
    }
    case CaseWords: {
        std::wstring r;
        std::vector<size_t> off = CodePointOffsets(s);
        bool inWord = false;
        for (size_t k = 0; k + 1 < off.size(); ++k) {
            size_t b = off[k], e = off[k + 1];
            std::wstring ch = s.substr(b, e - b);
            bool word = IsWordChar(s, b);
            if (word && !inWord)
                r += ToUpper(ch);
            else if (word)
                r += ToLower(ch);
            else
                r += ch;
            inWord = word;
        }
        return r;
    }
    default:
        return s;
    }
}

// ---------------------------------------------------------------------------
// Suchen/Ersetzen
// ---------------------------------------------------------------------------

struct ReplaceSpec {
    bool active = false;
    std::wstring find, repl;
    bool matchCase = false, regex = false, firstOnly = false;
    std::wregex re;
};

// Sucht find in s ab pos; ohne Groß-/Kleinschreibung ordinal wie das Dateisystem.
size_t FindLiteral(const std::wstring& s, const std::wstring& find, size_t pos, bool matchCase) {
    if (find.empty() || s.size() < find.size()) return std::wstring::npos;
    if (matchCase) return s.find(find, pos);
    for (size_t i = pos; i + find.size() <= s.size(); ++i) {
        if (CompareStringOrdinal(s.data() + i, (int)find.size(), find.data(), (int)find.size(), TRUE) == CSTR_EQUAL)
            return i;
    }
    return std::wstring::npos;
}

std::wstring ApplyReplace(const std::wstring& s, const ReplaceSpec& rs) {
    if (!rs.active) return s;
    if (rs.regex) {
        auto flags = rs.firstOnly ? std::regex_constants::format_first_only : std::regex_constants::format_default;
        return std::regex_replace(s, rs.re, rs.repl, flags);
    }
    std::wstring r;
    size_t pos = 0;
    for (;;) {
        size_t f = FindLiteral(s, rs.find, pos, rs.matchCase);
        if (f == std::wstring::npos) break;
        r.append(s, pos, f - pos);
        r += rs.repl;
        pos = f + rs.find.size();
        if (rs.firstOnly) break;
    }
    r.append(s, pos, std::wstring::npos);
    return r;
}

// ---------------------------------------------------------------------------
// Namensprüfung
// ---------------------------------------------------------------------------

std::wstring CheckName(const std::wstring& n) {
    if (n.empty()) return L"Leerer Name";
    if (n.size() > 255) return L"Name zu lang";
    for (wchar_t c : n)
        if (c < 32 || std::wcschr(L"\\/:*?\"<>|", c)) return L"Ungültiges Zeichen";
    if (n.back() == L'.' || n.back() == L' ') return L"Punkt/Leerzeichen am Ende";
    // Reservierte Gerätenamen (auch mit Erweiterung)
    std::wstring stem = ToUpper(n.substr(0, n.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    static const wchar_t* reserved[] = {L"CON", L"PRN", L"AUX", L"NUL"};
    for (auto* r : reserved)
        if (stem == r) return L"Reservierter Gerätename";
    if (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) && stem[3] >= L'1' &&
        stem[3] <= L'9')
        return L"Reservierter Gerätename";
    return L"";
}

int PathDepth(const std::wstring& dir) {
    int d = 0;
    for (wchar_t c : dir)
        if (c == L'\\') ++d;
    return d;
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

enum ItemState { StateOk = 0, StateSame = 1, StateConflict = 2 };

struct RenItem {
    std::wstring dir;       // vollständiges Verzeichnis
    std::wstring name;      // aktueller Name
    bool isDir = false;
    FILETIME modified{};
    std::wstring newName;
    int state = StateSame;
    std::wstring note;
};

class BatchRenameDialog : public DialogBase {
public:
    BatchRenameDialog(const std::wstring& dir, const std::vector<std::wstring>& names) : dir_(dir), names_(names) {}
    bool Changed() const { return changed_; }

protected:
    BOOL OnInit() override;
    BOOL OnCommand(int id, int code, HWND ctl) override;
    INT_PTR OnNotify(NMHDR* nm) override;
    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnDestroy() override { KillTimer(hwnd_, kTimerPreview); }

private:
    void Collect();
    void CollectDir(const std::wstring& path, bool renameDirs, int level);
    void AddItem(const std::wstring& dir, const std::wstring& name, bool isDir, const FILETIME& ft);
    void SchedulePreview() { SetTimer(hwnd_, kTimerPreview, 150, nullptr); }
    void UpdatePreview();
    const std::unordered_set<std::wstring>& ExistingNames(const std::wstring& dir);
    void InsertPlaceholder(const std::wstring& text, int selectFrom = -1, int selectLen = 0);
    HWND ComboEdit(int comboId) const;
    bool Execute();
    void LoadSettings();
    void SaveSettings();
    void FillHistory(int comboId, const std::vector<std::wstring>& list);
    static void AddToHistory(std::vector<std::wstring>& list, const std::wstring& value);

    std::wstring dir_;
    std::vector<std::wstring> names_;
    std::vector<RenItem> items_;
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> existing_; // Verzeichnis (groß) -> Namen (groß)
    std::vector<std::wstring> histName_, histExt_;
    bool maskError_ = true;
    int renameCount_ = 0;
    int conflictCount_ = 0;
    bool changed_ = false;
    bool collectedRecursive_ = false;
    bool collectedDirs_ = true;
    // Zuletzt bearbeitete Maske und deren Auswahl (für die Platzhalter-Knöpfe)
    int lastMask_ = CID_NAMEMASK;
    DWORD selStart_ = (DWORD)-1, selEnd_ = (DWORD)-1;
};

const wchar_t* kHelpText =
    L"[N] Name ohne Erweiterung  ·  [N3] 3. Zeichen  ·  [N2-5] Zeichen 2 bis 5  ·  [N2-] ab dem 2. Zeichen  ·  "
    L"[N-3] die letzten 3 Zeichen  ·  [N2,4] 4 Zeichen ab dem 2.  ·  [N2--2] vom 2. bis zum vorletzten Zeichen "
    L"(negative Positionen zählen vom Ende)  ·  [E] Erweiterung ohne Punkt (Bereiche wie bei [N])  ·  [C] Zähler  ·  "
    L"[D] Änderungsdatum JJJJMMTT  ·  [d] Änderungsdatum JJJJ-MM-TT  ·  [T] Uhrzeit hhmmss  ·  [t] Uhrzeit hh-mm-ss  ·  "
    L"[P] Elternverzeichnis  ·  [[ und ]] eckige Klammern";

BOOL BatchRenameDialog::OnInit() {
    // Spalten der Vorschau
    LvAddColumn(CID_LIST, L"Alter Name", 125);
    LvAddColumn(CID_LIST, L"Neuer Name", 125);
    LvAddColumn(CID_LIST, L"Verzeichnis", 130);
    LvAddColumn(CID_LIST, L"Hinweis", 70);

    static const wchar_t* caseNames[] = {L"unverändert", L"kleinbuchstaben", L"GROSSBUCHSTABEN",
                                         L"Erster Buchstabe groß", L"Jedes Wort Groß"};
    for (auto* c : caseNames) {
        ComboAdd(CID_CASENAME, c);
        ComboAdd(CID_CASEEXT, c);
    }
    // Zähler-Drehfelder
    SendMessageW(Item(CID_SPIN_START), UDM_SETRANGE32, (WPARAM)-999999999, 999999999);
    SendMessageW(Item(CID_SPIN_STEP), UDM_SETRANGE32, (WPARAM)-999999, 999999);
    SendMessageW(Item(CID_SPIN_DIGITS), UDM_SETRANGE32, 1, 12);
    SendMessageW(Item(CID_START), EM_LIMITTEXT, 10, 0);
    SendMessageW(Item(CID_STEP), EM_LIMITTEXT, 7, 0);
    SendMessageW(Item(CID_DIGITS), EM_LIMITTEXT, 2, 0);

    LoadSettings();

    SetAnchor(CID_NAMEMASK, AnchorTopLeftRight);
    SetAnchor(CID_LBL_EXT, AnchorTopRight);
    SetAnchor(CID_EXTMASK, AnchorTopRight);
    SetAnchor(CID_RESET, AnchorTopRight);
    SetAnchor(CID_HELP, AnchorTopLeftRight);
    SetAnchor(CID_LIST, AnchorAll);
    SetAnchor(CID_STATUS, AnchorBottomLeftRight);
    SetAnchor(IDOK, AnchorBottomRight);
    SetAnchor(IDCANCEL, AnchorBottomRight);
    EnableResizing();

    Collect();
    UpdatePreview();
    return TRUE;
}

void BatchRenameDialog::FillHistory(int comboId, const std::vector<std::wstring>& list) {
    HWND c = Item(comboId);
    std::wstring cur = GetWindowTextStr(c);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (auto& s : list) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SetWindowTextW(c, cur.c_str());
}

void BatchRenameDialog::AddToHistory(std::vector<std::wstring>& list, const std::wstring& value) {
    if (value.empty()) return;
    list.erase(std::remove(list.begin(), list.end(), value), list.end());
    list.insert(list.begin(), value);
    if ((int)list.size() > kHistoryMax) list.resize(kHistoryMax);
}

void BatchRenameDialog::LoadSettings() {
    Config& c = App::Cfg();
    histName_ = c.GetList(kSection, L"Name");
    histExt_ = c.GetList(kSection, L"Erw");
    // Masken beginnen immer mit den Standardwerten; die zuletzt verwendeten stehen in der Liste.
    SetText(CID_NAMEMASK, L"[N]");
    SetText(CID_EXTMASK, L"[E]");
    FillHistory(CID_NAMEMASK, histName_);
    FillHistory(CID_EXTMASK, histExt_);
    SetInt(CID_START, c.GetInt(kSection, L"ZaehlerStart", 1));
    SetInt(CID_STEP, c.GetInt(kSection, L"ZaehlerSchritt", 1));
    SetInt(CID_DIGITS, std::clamp(c.GetInt(kSection, L"ZaehlerStellen", 3), 1, 12));
    SetCheck(CID_MATCHCASE, c.GetBool(kSection, L"GrossKlein", false));
    SetCheck(CID_REGEX, c.GetBool(kSection, L"Regex", false));
    SetCheck(CID_FIRSTONLY, c.GetBool(kSection, L"NurErstes", false));
    ComboSetSel(CID_CASENAME, std::clamp(c.GetInt(kSection, L"SchreibungName", 0), 0, 4));
    ComboSetSel(CID_CASEEXT, std::clamp(c.GetInt(kSection, L"SchreibungErw", 0), 0, 4));
    SetCheck(CID_RECURSIVE, false); // bewusst nicht gemerkt (Sicherheit)
    SetCheck(CID_PERDIR, c.GetBool(kSection, L"ZaehlerJeVerzeichnis", true));
    SetCheck(CID_RENAMEDIRS, c.GetBool(kSection, L"Verzeichnisse", true));
}

void BatchRenameDialog::SaveSettings() {
    Config& c = App::Cfg();
    AddToHistory(histName_, GetText(CID_NAMEMASK));
    AddToHistory(histExt_, GetText(CID_EXTMASK));
    c.SetList(kSection, L"Name", histName_);
    c.SetList(kSection, L"Erw", histExt_);
    c.SetInt(kSection, L"ZaehlerStart", (int)GetInt(CID_START, 1));
    c.SetInt(kSection, L"ZaehlerSchritt", (int)GetInt(CID_STEP, 1));
    c.SetInt(kSection, L"ZaehlerStellen", (int)GetInt(CID_DIGITS, 3));
    c.SetBool(kSection, L"GrossKlein", IsChecked(CID_MATCHCASE));
    c.SetBool(kSection, L"Regex", IsChecked(CID_REGEX));
    c.SetBool(kSection, L"NurErstes", IsChecked(CID_FIRSTONLY));
    c.SetInt(kSection, L"SchreibungName", ComboSel(CID_CASENAME));
    c.SetInt(kSection, L"SchreibungErw", ComboSel(CID_CASEEXT));
    c.SetBool(kSection, L"ZaehlerJeVerzeichnis", IsChecked(CID_PERDIR));
    c.SetBool(kSection, L"Verzeichnisse", IsChecked(CID_RENAMEDIRS));
}

void BatchRenameDialog::AddItem(const std::wstring& dir, const std::wstring& name, bool isDir, const FILETIME& ft) {
    RenItem it;
    it.dir = dir;
    it.name = name;
    it.isDir = isDir;
    it.modified = ft;
    items_.push_back(std::move(it));
}

void BatchRenameDialog::CollectDir(const std::wstring& path, bool renameDirs, int level) {
    if (level > 200) return; // Schutz vor extrem tiefen Strukturen
    std::vector<DirEntry> entries;
    if (!ListDirectory(path, entries)) return;
    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& a, const DirEntry& b) { return CompareNatural(a.name, b.name) < 0; });
    // Erst die Dateien, dann die Unterverzeichnisse (rekursiv)
    for (auto& e : entries)
        if (!e.IsDir()) AddItem(path, e.name, false, e.modified);
    for (auto& e : entries) {
        if (!e.IsDir()) continue;
        if (renameDirs) AddItem(path, e.name, true, e.modified);
        if (!(e.attributes & FILE_ATTRIBUTE_REPARSE_POINT)) CollectDir(PathCombine(path, e.name), renameDirs, level + 1);
    }
}

void BatchRenameDialog::Collect() {
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    items_.clear();
    existing_.clear();
    collectedRecursive_ = IsChecked(CID_RECURSIVE);
    collectedDirs_ = IsChecked(CID_RENAMEDIRS);
    for (auto& n : names_) {
        std::wstring full = PathCombine(dir_, n);
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(LongPath(full).c_str(), GetFileExInfoStandard, &d)) continue;
        bool isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!isDir || collectedDirs_) AddItem(dir_, n, isDir, d.ftLastWriteTime);
        if (isDir && collectedRecursive_ && !(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            CollectDir(full, collectedDirs_, 0);
    }
    SetCursor(old);
}

const std::unordered_set<std::wstring>& BatchRenameDialog::ExistingNames(const std::wstring& dir) {
    std::wstring key = ToUpper(dir);
    auto it = existing_.find(key);
    if (it != existing_.end()) return it->second;
    std::unordered_set<std::wstring> set;
    std::vector<DirEntry> entries;
    if (ListDirectory(dir, entries))
        for (auto& e : entries) set.insert(ToUpper(e.name));
    return existing_.emplace(key, std::move(set)).first->second;
}

void BatchRenameDialog::UpdatePreview() {
    KillTimer(hwnd_, kTimerPreview);
    if (IsChecked(CID_RECURSIVE) != collectedRecursive_ || IsChecked(CID_RENAMEDIRS) != collectedDirs_) Collect();

    std::wstring err;
    std::vector<Tok> nameToks, extToks;
    if (!CompileMask(GetText(CID_NAMEMASK), nameToks, err))
        err = L"Namensmaske: " + err;
    else if (!CompileMask(GetText(CID_EXTMASK), extToks, err))
        err = L"Erweiterungsmaske: " + err;

    ReplaceSpec rs;
    rs.find = GetText(CID_SEARCH);
    rs.repl = GetText(CID_REPLACE);
    rs.matchCase = IsChecked(CID_MATCHCASE);
    rs.regex = IsChecked(CID_REGEX);
    rs.firstOnly = IsChecked(CID_FIRSTONLY);
    rs.active = !rs.find.empty();
    if (err.empty() && rs.active && rs.regex) {
        try {
            auto f = std::regex_constants::ECMAScript;
            if (!rs.matchCase) f |= std::regex_constants::icase;
            rs.re.assign(rs.find, f);
        } catch (const std::regex_error&) {
            err = L"Ungültiger regulärer Ausdruck.";
        }
    }

    const long long start = GetInt(CID_START, 1);
    const long long step = GetInt(CID_STEP, 1);
    const int digits = (int)std::clamp<long long>(GetInt(CID_DIGITS, 1), 1, 12);
    const int caseName = std::max(0, ComboSel(CID_CASENAME));
    const int caseExt = std::max(0, ComboSel(CID_CASEEXT));
    const bool perDir = IsChecked(CID_PERDIR);

    std::unordered_map<std::wstring, long long> dirCounter;
    long long globalCounter = 0;
    renameCount_ = 0;
    conflictCount_ = 0;

    for (auto& it : items_) {
        it.newName.clear();
        it.note.clear();
        it.state = StateSame;
        long long idx = perDir ? dirCounter[ToUpper(it.dir)]++ : globalCounter++;
        if (!err.empty()) continue;

        std::wstring stem = it.name, ext;
        size_t dot = it.name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            stem = it.name.substr(0, dot);
            ext = it.name.substr(dot + 1);
        }
        std::wstring dateStr, timeStr, dateDashStr, timeDashStr;
        {
            SYSTEMTIME utc{}, loc{};
            if (FileTimeToSystemTime(&it.modified, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) {
                dateStr = Format(L"%04u%02u%02u", (unsigned)loc.wYear, (unsigned)loc.wMonth, (unsigned)loc.wDay);
                timeStr = Format(L"%02u%02u%02u", (unsigned)loc.wHour, (unsigned)loc.wMinute, (unsigned)loc.wSecond);
                dateDashStr = Format(L"%04u-%02u-%02u", (unsigned)loc.wYear, (unsigned)loc.wMonth, (unsigned)loc.wDay);
                timeDashStr = Format(L"%02u-%02u-%02u", (unsigned)loc.wHour, (unsigned)loc.wMinute, (unsigned)loc.wSecond);
            }
        }
        const std::wstring counterStr = FormatCounter(start + step * idx, digits);
        auto expand = [&](const std::vector<Tok>& toks) {
            std::wstring r;
            for (auto& t : toks) {
                switch (t.kind) {
                case TokKind::Literal: r += t.text; break;
                case TokKind::Name: r += ApplyRange(stem, t); break;
                case TokKind::Ext: r += ApplyRange(ext, t); break;
                case TokKind::Counter: r += counterStr; break;
                case TokKind::Date: r += dateStr; break;
                case TokKind::Time: r += timeStr; break;
                case TokKind::DateDash: r += dateDashStr; break;
                case TokKind::TimeDash: r += timeDashStr; break;
                case TokKind::Parent: r += LastPathElement(it.dir); break;
                }
            }
            return r;
        };
        std::wstring n = expand(nameToks);
        std::wstring e = expand(extToks);
        std::wstring full = e.empty() ? n : n + L"." + e;
        if (rs.active) {
            try {
                full = ApplyReplace(full, rs);
            } catch (const std::regex_error&) {
                err = L"Fehler beim Anwenden des regulären Ausdrucks.";
                it.newName.clear();
                continue;
            }
        }
        // Groß-/Kleinschreibung getrennt für Name und Erweiterung
        if (caseName != CaseKeep || caseExt != CaseKeep) {
            size_t d2 = full.find_last_of(L'.');
            if (d2 != std::wstring::npos && d2 > 0)
                full = ApplyCase(full.substr(0, d2), caseName) + L"." + ApplyCase(full.substr(d2 + 1), caseExt);
            else
                full = ApplyCase(full, caseName);
        }
        it.newName = Trim(full); // führende/abschließende Leerzeichen entfernt auch RenameItem
    }

    if (err.empty()) {
        // 1. Gültigkeit und doppelte Zielnamen je Verzeichnis
        std::unordered_map<std::wstring, int> targets; // "VERZ\NAME" (groß) -> Anzahl
        std::unordered_map<std::wstring, std::unordered_set<std::wstring>> sources; // Verzeichnis -> Quellnamen
        for (auto& it : items_) {
            std::wstring dk = ToUpper(it.dir);
            targets[dk + L'\\' + ToUpper(it.newName)]++;
            sources[dk].insert(ToUpper(it.name));
        }
        for (auto& it : items_) {
            std::wstring dk = ToUpper(it.dir);
            std::wstring nu = ToUpper(it.newName);
            std::wstring bad = CheckName(it.newName);
            if (!bad.empty()) {
                it.state = StateConflict;
                it.note = bad;
            } else if (targets[dk + L'\\' + nu] > 1) {
                it.state = StateConflict;
                it.note = L"Doppelter Zielname";
            } else if (it.newName == it.name) {
                it.state = StateSame;
                it.note = L"unverändert";
            } else if (ExistingNames(it.dir).count(nu) && !sources[dk].count(nu)) {
                it.state = StateConflict;
                it.note = L"Ziel existiert bereits";
            } else {
                it.state = StateOk;
            }
            if (it.state == StateConflict) ++conflictCount_;
            if (it.state == StateOk) ++renameCount_;
        }
    }
    maskError_ = !err.empty();

    HWND lv = Item(CID_LIST);
    ListView_SetItemCountEx(lv, (int)items_.size(), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    InvalidateRect(lv, nullptr, FALSE);

    std::wstring status;
    if (maskError_)
        status = err;
    else if (items_.empty())
        status = L"Keine Elemente zum Umbenennen.";
    else {
        status = IntToStrGrouped(items_.size()) + (items_.size() == 1 ? L" Element, " : L" Elemente, ") +
                 IntToStrGrouped((unsigned long long)renameCount_) + L" werden umbenannt";
        if (conflictCount_)
            status += L", " + IntToStrGrouped((unsigned long long)conflictCount_) +
                      (conflictCount_ == 1 ? L" Konflikt (rot markiert)" : L" Konflikte (rot markiert)");
    }
    SetText(CID_STATUS, status);
    Enable(IDOK, !maskError_ && conflictCount_ == 0 && renameCount_ > 0);
}

HWND BatchRenameDialog::ComboEdit(int comboId) const {
    COMBOBOXINFO cbi{};
    cbi.cbSize = sizeof(cbi);
    if (GetComboBoxInfo(Item(comboId), &cbi) && cbi.hwndItem) return cbi.hwndItem;
    return FindWindowExW(Item(comboId), nullptr, L"Edit", nullptr);
}

void BatchRenameDialog::InsertPlaceholder(const std::wstring& text, int selectFrom, int selectLen) {
    HWND edit = ComboEdit(lastMask_);
    if (!edit) return;
    SetFocus(edit);
    if (selStart_ == (DWORD)-1) {
        int len = GetWindowTextLengthW(edit);
        SendMessageW(edit, EM_SETSEL, len, len);
    } else {
        SendMessageW(edit, EM_SETSEL, selStart_, selEnd_);
    }
    DWORD s = 0, e = 0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)text.c_str());
    if (selectFrom >= 0) SendMessageW(edit, EM_SETSEL, s + selectFrom, s + selectFrom + selectLen);
    SendMessageW(edit, EM_GETSEL, (WPARAM)&selStart_, (LPARAM)&selEnd_);
    SchedulePreview();
}

BOOL BatchRenameDialog::OnCommand(int id, int code, HWND ctl) {
    switch (id) {
    case CID_NAMEMASK:
    case CID_EXTMASK:
        if (code == CBN_EDITCHANGE || code == CBN_SELCHANGE) SchedulePreview();
        if (code == CBN_SETFOCUS) {
            lastMask_ = id;
        } else if (code == CBN_KILLFOCUS) {
            lastMask_ = id;
            HWND edit = ComboEdit(id);
            if (edit) SendMessageW(edit, EM_GETSEL, (WPARAM)&selStart_, (LPARAM)&selEnd_);
        }
        return TRUE;
    case CID_START:
    case CID_STEP:
    case CID_DIGITS:
    case CID_SEARCH:
    case CID_REPLACE:
        if (code == EN_CHANGE) SchedulePreview();
        return TRUE;
    case CID_CASENAME:
    case CID_CASEEXT:
        if (code == CBN_SELCHANGE) SchedulePreview();
        return TRUE;
    case CID_MATCHCASE:
    case CID_REGEX:
    case CID_FIRSTONLY:
    case CID_PERDIR:
        SchedulePreview();
        return TRUE;
    case CID_RECURSIVE:
    case CID_RENAMEDIRS:
        UpdatePreview(); // sammelt neu ein
        return TRUE;
    case CID_INS_N: InsertPlaceholder(L"[N]"); return TRUE;
    case CID_INS_NRANGE: InsertPlaceholder(L"[N1-3]", 2, 3); return TRUE;
    case CID_INS_E: InsertPlaceholder(L"[E]"); return TRUE;
    case CID_INS_C: InsertPlaceholder(L"[C]"); return TRUE;
    case CID_INS_D: InsertPlaceholder(L"[D]"); return TRUE;
    case CID_INS_T: InsertPlaceholder(L"[T]"); return TRUE;
    case CID_INS_DD: InsertPlaceholder(L"[d]"); return TRUE;
    case CID_INS_TD: InsertPlaceholder(L"[t]"); return TRUE;
    case CID_INS_P: InsertPlaceholder(L"[P]"); return TRUE;
    case CID_INS_LB: InsertPlaceholder(L"[["); return TRUE;
    case CID_INS_RB: InsertPlaceholder(L"]]"); return TRUE;
    case CID_RESET:
        SetText(CID_NAMEMASK, L"[N]");
        SetText(CID_EXTMASK, L"[E]");
        SetText(CID_SEARCH, L"");
        SetText(CID_REPLACE, L"");
        ComboSetSel(CID_CASENAME, 0);
        ComboSetSel(CID_CASEEXT, 0);
        selStart_ = selEnd_ = (DWORD)-1;
        UpdatePreview();
        return TRUE;
    case IDOK:
        UpdatePreview(); // ausstehende Änderungen sicher übernehmen
        if (maskError_ || conflictCount_ > 0 || renameCount_ == 0) return TRUE;
        if (Execute()) {
            SaveSettings();
            End(IDOK);
        }
        return TRUE;
    case IDCANCEL:
        End(IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR BatchRenameDialog::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == kTimerPreview) {
        UpdatePreview();
        return TRUE;
    }
    return FALSE;
}

INT_PTR BatchRenameDialog::OnNotify(NMHDR* nm) {
    if (nm->idFrom != CID_LIST) return 0;
    if (nm->code == LVN_GETDISPINFOW) {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        if ((di->item.mask & LVIF_TEXT) && di->item.iItem >= 0 && di->item.iItem < (int)items_.size()) {
            const RenItem& it = items_[(size_t)di->item.iItem];
            const std::wstring* s = &it.name;
            switch (di->item.iSubItem) {
            case 1: s = &it.newName; break;
            case 2: s = &it.dir; break;
            case 3: s = &it.note; break;
            }
            di->item.pszText = const_cast<wchar_t*>(s->c_str());
        }
        return 0;
    }
    if (nm->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            size_t i = (size_t)cd->nmcd.dwItemSpec;
            if (i < items_.size()) {
                if (items_[i].state == StateConflict) {
                    cd->clrText = RGB(192, 0, 0);
                    cd->clrTextBk = RGB(255, 225, 225);
                    return CDRF_NEWFONT;
                }
                if (items_[i].state == StateSame && !maskError_) {
                    cd->clrText = GetSysColor(COLOR_GRAYTEXT);
                    return CDRF_NEWFONT;
                }
            }
        }
        return 0;
    }
    return 0;
}

// Führt die Umbenennung aus. Verzeichnisse werden von innen nach außen bearbeitet, damit die Pfade der
// noch ausstehenden Elemente gültig bleiben. Innerhalb eines Verzeichnisses: zuerst alle direkt möglichen
// Umbenennungen; verbleibende (Zyklen, Vertauschungen, reine Groß-/Kleinschreibung) über temporäre Namen.
bool BatchRenameDialog::Execute() {
    struct Op {
        std::wstring dir, original, cur, target;
        int depth = 0;
    };
    std::vector<Op> ops;
    for (auto& it : items_)
        if (it.state == StateOk) ops.push_back({it.dir, it.name, it.name, it.newName, PathDepth(it.dir)});
    std::stable_sort(ops.begin(), ops.end(), [](const Op& a, const Op& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        return CompareI(a.dir, b.dir) < 0;
    });

    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    BeginUndoGroup(L"Dateigruppe umbenennen");
    int done = 0;
    bool ok = true;
    unsigned tempCounter = 0;

    for (size_t g = 0; g < ops.size() && ok;) {
        size_t gEnd = g;
        while (gEnd < ops.size() && EqualsI(ops[gEnd].dir, ops[g].dir)) ++gEnd;
        const std::wstring dir = ops[g].dir;

        std::vector<Op*> pending;
        std::unordered_map<std::wstring, int> occupied; // aktuelle Namen (groß) der ausstehenden Elemente
        std::unordered_set<std::wstring> targetSet;
        for (size_t k = g; k < gEnd; ++k) {
            pending.push_back(&ops[k]);
            occupied[ToUpper(ops[k].cur)]++;
            targetSet.insert(ToUpper(ops[k].target));
        }
        auto release = [&](const std::wstring& name) {
            auto f = occupied.find(ToUpper(name));
            if (f != occupied.end() && --f->second <= 0) occupied.erase(f);
        };
        // Direkt mögliche Umbenennungen, bis sich nichts mehr bewegt
        auto runDirect = [&]() -> bool {
            bool progress = true;
            while (progress && !pending.empty()) {
                progress = false;
                for (size_t k = 0; k < pending.size();) {
                    Op* o = pending[k];
                    if (occupied.count(ToUpper(o->target))) {
                        ++k;
                        continue;
                    }
                    if (!RenameItem(hwnd_, PathCombine(dir, o->cur), o->target)) return false;
                    release(o->cur);
                    o->cur = o->target;
                    ++done;
                    pending.erase(pending.begin() + (std::ptrdiff_t)k);
                    progress = true;
                }
            }
            return true;
        };

        ok = runDirect();
        if (ok && !pending.empty()) {
            // Phase 1: verbleibende Elemente auf eindeutige temporäre Namen umbenennen
            for (Op* o : pending) {
                std::wstring temp;
                for (;;) {
                    temp = Format(L"~qfren%08X%04X.tmp", (unsigned)(GetTickCount64() & 0xFFFFFFFFu), ++tempCounter & 0xFFFF);
                    if (!targetSet.count(ToUpper(temp)) && !PathExists(PathCombine(dir, temp))) break;
                }
                if (!RenameItem(hwnd_, PathCombine(dir, o->cur), temp)) {
                    ok = false;
                    break;
                }
                release(o->cur);
                occupied[ToUpper(temp)]++;
                o->cur = temp;
            }
            // Phase 2: temporäre Namen auf die Zielnamen
            if (ok) ok = runDirect() && pending.empty();
            if (!ok) {
                // Elemente mit temporärem Namen nach Möglichkeit auf den ursprünglichen Namen zurücksetzen
                for (Op* o : pending) {
                    if (o->cur == o->original || o->cur == o->target) continue;
                    if (!PathExists(PathCombine(dir, o->original)))
                        RenameItem(hwnd_, PathCombine(dir, o->cur), o->original);
                }
            }
        }
        g = gEnd;
    }
    EndUndoGroup();
    SetCursor(oldCursor);

    if (done > 0) changed_ = true;
    if (!ok) {
        // Zustand neu einlesen, damit die Vorschau wieder stimmt
        if (done > 0) {
            // Bereits umbenannte Namen in der Auswahl nachführen (nur oberste Ebene)
            for (auto& o : ops)
                if (EqualsI(o.dir, dir_) && o.cur == o.target)
                    for (auto& n : names_)
                        if (n == o.original) n = o.target;
        }
        Collect();
        UpdatePreview();
        return false;
    }
    return true;
}

} // namespace

bool BatchRename(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names) {
    if (names.empty()) return false;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    const DWORD numStyle = ES_AUTOHSCROLL;
    const DWORD spinStyle = UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_AUTOBUDDY;

    DialogTemplate t(L"Dateigruppe umbenennen", 470, 330, DialogTemplate::kResizable);
    t.Label(CID_LBL_NAME, L"&Name:", 7, 9, 42, 8);
    t.Combo(CID_NAMEMASK, 52, 7, 262, 150, true);
    t.Label(CID_LBL_EXT, L"E&rweiterung:", 322, 9, 46, 8);
    t.Combo(CID_EXTMASK, 370, 7, 93, 150, true);

    int x = 52;
    auto ins = [&](int id, const wchar_t* text, int w) {
        t.Button(id, text, x, 24, w, 14);
        x += w + 3;
    };
    ins(CID_INS_N, L"[N]", 26);
    ins(CID_INS_NRANGE, L"[N#-#]", 34);
    ins(CID_INS_E, L"[E]", 26);
    ins(CID_INS_C, L"[C]", 26);
    ins(CID_INS_D, L"[D]", 26);
    ins(CID_INS_DD, L"[d]", 26);
    ins(CID_INS_T, L"[T]", 26);
    ins(CID_INS_TD, L"[t]", 26);
    ins(CID_INS_P, L"[P]", 26);
    ins(CID_INS_LB, L"[[", 22);
    ins(CID_INS_RB, L"]]", 22);
    t.Button(CID_RESET, L"Zurücksetzen", 393, 24, 70, 14);
    t.Label(CID_HELP, kHelpText, 7, 43, 456, 32);

    // Zähler
    t.Group(CID_GRP_COUNTER, L"Zähler [C]", 7, 78, 118, 66);
    t.Label(CID_LBL_START, L"&Start:", 13, 94, 36, 8);
    t.Edit(CID_START, 52, 92, 66, 12, numStyle);
    t.UpDown(CID_SPIN_START, 0, 0, 0, 0, spinStyle);
    t.Label(CID_LBL_STEP, L"Schr&itt:", 13, 109, 36, 8);
    t.Edit(CID_STEP, 52, 107, 66, 12, numStyle);
    t.UpDown(CID_SPIN_STEP, 0, 0, 0, 0, spinStyle);
    t.Label(CID_LBL_DIGITS, L"Ste&llen:", 13, 124, 36, 8);
    t.Edit(CID_DIGITS, 52, 122, 66, 12, numStyle | ES_NUMBER);
    t.UpDown(CID_SPIN_DIGITS, 0, 0, 0, 0, spinStyle);

    // Suchen und Ersetzen
    t.Group(CID_GRP_REPLACE, L"Suchen und Ersetzen (im neuen Namen)", 131, 78, 214, 66);
    t.Label(CID_LBL_SEARCH, L"Su&chen:", 137, 94, 36, 8);
    t.Edit(CID_SEARCH, 176, 92, 163, 12);
    t.Label(CID_LBL_REPLACE, L"&Ersetzen:", 137, 109, 36, 8);
    t.Edit(CID_REPLACE, 176, 107, 163, 12);
    t.Check(CID_MATCHCASE, L"Groß/&klein", 137, 125, 50, 10);
    t.Check(CID_REGEX, L"Re&gulärer Ausdruck", 189, 125, 76, 10);
    t.Check(CID_FIRSTONLY, L"Nur erstes &Vork.", 267, 125, 72, 10);

    // Groß-/Kleinschreibung
    t.Group(CID_GRP_CASE, L"Groß-/Kleinschreibung", 351, 78, 112, 66);
    t.Label(CID_LBL_CASENAME, L"Na&me:", 357, 94, 28, 8);
    t.Combo(CID_CASENAME, 386, 92, 71, 120);
    t.Label(CID_LBL_CASEEXT, L"Er&w.:", 357, 109, 28, 8);
    t.Combo(CID_CASEEXT, 386, 107, 71, 120);

    // Optionen
    t.Check(CID_RECURSIVE, L"Unterverzeichnisse ein&beziehen", 7, 150, 120, 10);
    t.Check(CID_PERDIR, L"Zähler je Ver&zeichnis neu beginnen", 131, 150, 132, 10);
    t.Check(CID_RENAMEDIRS, L"Verzeichnisnamen än&dern", 267, 150, 110, 10);

    t.ListView(CID_LIST, 7, 164, 456, 140, LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA);
    t.Label(CID_STATUS, L"", 7, 312, 330, 8, SS_ENDELLIPSIS);
    t.DefButton(IDOK, L"&Umbenennen", 343, 309, 58, 14);
    t.Button(IDCANCEL, L"Abbrechen", 405, 309, 58, 14);

    BatchRenameDialog dlg(NormalizeDir(dir), names);
    dlg.DoModal(owner, t);
    return dlg.Changed();
}

} // namespace qf
