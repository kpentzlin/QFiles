// Integrierter Texteditor von QFiles (Modul A).
//
// Jedes Editorfenster ist ein eigenes, nicht modales Top-Level-Fenster mit einem RichEdit-4.1-Steuerelement
// (Msftedit.dll) im Klartextmodus, Menü, Statusleiste, Suchen/Ersetzen (FindTextW/ReplaceTextW),
// Zeilenoperationen (Sortieren usw.), Kodierungs- und Zeilenendeauswahl sowie einfachem GDI-Druck.
//
// Hinweise zum RichEdit-Steuerelement:
// - Intern speichert RichEdit Zeilenenden als einzelnes CR. Zeichenpositionen (cp) beziehen sich auf diesen
//   Rohtext (UTF-16-Einheiten, Surrogatpaare zählen als 2). Für Zeilenoperationen wird daher der Rohtext
//   (GT_DEFAULT) verwendet, zum Speichern der Text mit CRLF (GT_USECRLF), der anschließend in das gewählte
//   Zeilenende umgewandelt wird.
// - Die Schrift wird per WM_SETFONT mit einer zur Fenster-DPI passenden HFONT gesetzt; dadurch stimmt die
//   Darstellung unabhängig davon, mit welcher DPI RichEdit intern rechnet.

#include "TextEditor.h"
#include "Modules.h"
#include "App.h"
#include "Util.h"
#include "Encoding.h"
#include "Settings.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <richedit.h>
#include <tom.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace qf {

namespace {

// ===================== Konstanten =====================

constexpr wchar_t kClassName[] = L"QFilesTextEditor";
constexpr wchar_t kCfgSection[] = L"Editor";
constexpr wchar_t kAppTitle[] = L"QFiles Editor";
constexpr UINT WM_APP_CHECKFILE = WM_APP + 1;   // nach Aktivierung: Datei extern geändert?
constexpr UINT WM_APP_OPENDROPPED = WM_APP + 2; // abgelegte Dateien öffnen (nach Ende der Drop-Verarbeitung)
constexpr int kIdEdit = 100;
constexpr int kIdStatus = 101;
constexpr LONG kMaxTextChars = 0x3FFFFFFF;      // RichEdit-Grenze (Zeichen)
constexpr uint64_t kLargeFileWarn = 200ull * 1024 * 1024;

enum : int {
    // Datei
    ID_NEW = 1001,
    ID_OPEN,
    ID_SAVE,
    ID_SAVEAS,
    ID_RELOAD,
    ID_PRINT,
    ID_CLOSE,
    ID_RELOADENC = 1020, // + Index der Kodierung (0–5)
    // Bearbeiten
    ID_UNDO = 1100,
    ID_REDO,
    ID_CUT,
    ID_COPY,
    ID_PASTE,
    ID_DELETE,
    ID_SELALL,
    ID_FIND,
    ID_FINDNEXT,
    ID_FINDPREV,
    ID_REPLACE,
    ID_GOTO,
    ID_DATETIME,
    // Text
    ID_SORT_ASC = 1200,
    ID_SORT_DESC,
    ID_SORT_ASC_CI,
    ID_SORT_DESC_CI,
    ID_SORT_NAT_ASC,
    ID_SORT_NAT_DESC,
    ID_DEDUP,
    ID_REVERSE,
    ID_UPPER,
    ID_LOWER,
    ID_TITLE,
    ID_TAB2SPC,
    ID_SPC2TAB,
    ID_TRIM,
    // Kodierung
    ID_ENC = 1300, // + Index (0–5)
    ID_EOL = 1310, // + Index (0–2)
    // Ansicht
    ID_WRAP = 1400,
    ID_FONT,
    ID_STATUSBAR,
};

constexpr int kEncCount = 6;
const TextEncoding kEncodings[kEncCount] = {TextEncoding::Utf8,    TextEncoding::Utf8Bom, TextEncoding::Utf16LE,
                                            TextEncoding::Utf16BE, TextEncoding::Ansi,    TextEncoding::Oem};
constexpr int kEolCount = 3;
const LineEnding kLineEndings[kEolCount] = {LineEnding::CRLF, LineEnding::LF, LineEnding::CR};

// IID von ITextDocument (TOM) – lokal definiert, da nicht in allen Import-Bibliotheken enthalten.
const GUID kIID_ITextDocument = {0x8CC497C0, 0xA1DF, 0x11CE, {0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D}};

int EncodingIndex(TextEncoding e) {
    for (int i = 0; i < kEncCount; ++i)
        if (kEncodings[i] == e) return i;
    return 0;
}

int LineEndingIndex(LineEnding e) {
    for (int i = 0; i < kEolCount; ++i)
        if (kLineEndings[i] == e) return i;
    return 0;
}

[[maybe_unused]] const wchar_t* LineEndingShort(LineEnding e) {
    switch (e) {
    case LineEnding::CRLF: return L"CRLF";
    case LineEnding::LF: return L"LF";
    case LineEnding::CR: return L"CR";
    }
    return L"?";
}

// ===================== Hilfsfunktionen =====================

// Vollständiger Pfad (für den Vergleich, ob eine Datei bereits geöffnet ist).
std::wstring FullPath(const std::wstring& p) {
    if (p.empty()) return p;
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n == 0) return p;
    std::wstring r(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, r.data(), nullptr);
    if (n == 0 || n >= r.size()) return p;
    r.resize(n);
    return r;
}

bool GetWriteTime(const std::wstring& path, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    ft = d.ftLastWriteTime;
    return true;
}

UINT SystemDpi() {
    using Fn = UINT(WINAPI*)();
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
    if (fn) return fn();
    HDC dc = GetDC(nullptr);
    UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}

bool IsBreak(wchar_t c) { return c == L'\r' || c == L'\n'; }

// Zerlegt Text in Zeilen (CR, LF, CRLF). trailing = Text endet mit einem Zeilenende.
struct LineList {
    std::vector<std::wstring> lines;
    bool trailing = false;
};

LineList SplitLines(const std::wstring& s) {
    LineList r;
    size_t start = 0;
    size_t i = 0;
    while (i < s.size()) {
        if (IsBreak(s[i])) {
            r.lines.emplace_back(s, start, i - start);
            if (s[i] == L'\r' && i + 1 < s.size() && s[i + 1] == L'\n') ++i;
            ++i;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < s.size() || r.lines.empty() || !IsBreak(s.back()))
        r.lines.emplace_back(s, start, s.size() - start);
    else
        r.trailing = true;
    if (!s.empty() && IsBreak(s.back())) r.trailing = true;
    return r;
}

// Fügt Zeilen mit CR (RichEdit-intern) wieder zusammen.
std::wstring JoinLines(const LineList& l) {
    size_t total = 0;
    for (auto& s : l.lines) total += s.size() + 1;
    std::wstring r;
    r.reserve(total + 1);
    for (size_t i = 0; i < l.lines.size(); ++i) {
        if (i) r += L'\r';
        r += l.lines[i];
    }
    if (l.trailing) r += L'\r';
    return r;
}

bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }

// Tabulatoren einer Zeile in Leerzeichen umwandeln (spaltengenau, Surrogatpaare = 1 Spalte).
std::wstring ExpandTabs(const std::wstring& line, int tw) {
    if (line.find(L'\t') == std::wstring::npos) return line;
    std::wstring r;
    r.reserve(line.size() + 16);
    int col = 0;
    for (wchar_t c : line) {
        if (c == L'\t') {
            int n = tw - (col % tw);
            r.append((size_t)n, L' ');
            col += n;
        } else {
            r += c;
            if (!IsLowSurrogate(c)) ++col;
        }
    }
    return r;
}

// Folgen von Leerzeichen, die bis zu einem Tabstopp reichen, durch Tabulatoren ersetzen (wie "unexpand -a").
std::wstring CompressSpaces(const std::wstring& line, int tw) {
    if (line.find(L' ') == std::wstring::npos) return line;
    std::wstring r;
    r.reserve(line.size());
    int col = 0;
    int run = 0; // ausstehende Leerzeichen
    for (wchar_t c : line) {
        if (c == L' ') {
            ++run;
            ++col;
            if (col % tw == 0) {
                if (run >= 2)
                    r += L'\t';
                else
                    r.append((size_t)run, L' ');
                run = 0;
            }
        } else if (c == L'\t') {
            run = 0; // Leerzeichen vor einem Tabulator sind überflüssig
            r += L'\t';
            col += tw - (col % tw);
        } else {
            r.append((size_t)run, L' ');
            run = 0;
            r += c;
            if (!IsLowSurrogate(c)) ++col;
        }
    }
    r.append((size_t)run, L' ');
    return r;
}

std::wstring TrimRight(const std::wstring& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;
    return s.substr(0, e);
}

bool IsNonSpacing(wchar_t c) {
    WORD t = 0;
    return GetStringTypeW(CT_CTYPE3, &c, 1, &t) && (t & C3_NONSPACING);
}

// "Erster Buchstabe Groß": jedes Wort beginnt mit einem Großbuchstaben, der Rest klein.
std::wstring TitleCase(const std::wstring& s) {
    std::wstring lower = ToLower(s);
    if (lower.size() != s.size()) lower = s;
    std::wstring r;
    r.reserve(lower.size());
    bool newWord = true;
    size_t i = 0;
    while (i < lower.size()) {
        size_t len = (IsHighSurrogate(lower[i]) && i + 1 < lower.size() && IsLowSurrogate(lower[i + 1])) ? 2 : 1;
        wchar_t c = lower[i];
        bool wordChar = len == 2 || IsCharAlphaNumericW(c) || (!newWord && (c == L'\'' || c == 0x2019)) ||
                        (!newWord && IsNonSpacing(c));
        std::wstring ch = lower.substr(i, len);
        if (wordChar && newWord) {
            std::wstring up = ToUpper(ch);
            r += up.empty() ? ch : up;
            newWord = false;
        } else {
            r += ch;
            if (!wordChar) newWord = true;
        }
        i += len;
    }
    return r;
}

bool IsWordChar(wchar_t c) {
    return c == L'_' || IsCharAlphaNumericW(c) || IsHighSurrogate(c) || IsLowSurrogate(c);
}

// Schrift für die Statusleiste passend zur DPI (App::UIFont() gilt für die System-DPI).
HFONT CreateUIFontForDpi(UINT dpi) {
    LOGFONTW lf{};
    if (!GetObjectW(App::UIFont(), sizeof(lf), &lf)) return nullptr;
    lf.lfHeight = MulDiv(lf.lfHeight, (int)dpi, (int)SystemDpi());
    return CreateFontIndirectW(&lf);
}

std::wstring g_lastFind;     // zuletzt gesuchter Text (für neue Fenster)
std::wstring g_lastReplace;
DWORD g_lastFindFlags = FR_DOWN;
UINT g_findMsg = 0;
HMODULE g_msftedit = nullptr;
bool g_classRegistered = false;

class EditorWindow;
std::vector<EditorWindow*> g_editors;
EditorWindow* g_pendingCreate = nullptr;

LRESULT CALLBACK EditSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

// ===================== Editorfenster =====================

class EditorWindow {
public:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    bool Create();
    bool Open(const std::wstring& path);   // path voll qualifiziert oder leer
    void Show();
    bool QueryClose();
    void BringToFront();
    void ApplyFont();                       // nach Änderung von App::Opt() (Schrift/Tabweite)

    HWND hwnd() const { return hwnd_; }
    const std::wstring& Path() const { return path_; }

private:
    friend LRESULT CALLBACK EditSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void OnCreate();
    void OnDestroy();
    void OnCommand(int id);
    void Layout();
    void UpdateStatusParts();
    void UpdateUIFont();
    void UpdateMenuState();
    void ShowContextMenu(int x, int y);

    // Text
    std::wstring GetEditText(bool crlf, bool selectionOnly) const;
    std::wstring GetRawText() const { return GetEditText(false, false); }
    LONG TextLength() const;
    void SetEditText(const std::wstring& text);
    bool IsModified() const { return SendMessageW(edit_, EM_GETMODIFY, 0, 0) != 0; }
    void SetModified(bool m) { SendMessageW(edit_, EM_SETMODIFY, m ? TRUE : FALSE, 0); }
    CHARRANGE GetSel() const {
        CHARRANGE cr{0, 0};
        SendMessageW(edit_, EM_EXGETSEL, 0, (LPARAM)&cr);
        return cr;
    }
    void SetSel(LONG a, LONG b) {
        CHARRANGE cr{a, b};
        SendMessageW(edit_, EM_EXSETSEL, 0, (LPARAM)&cr);
    }
    LONG CaretPos() const;
    void ReplaceRange(LONG s, LONG e, const std::wstring& text, bool selectResult);
    void PastePlain();
    bool IsEmptyAndUnchanged() const { return !IsModified() && TextLength() == 0; }

    // Darstellung
    void ApplyTabs();
    void ApplyWrap();

    // Datei
    bool LoadFile(const std::wstring& path, const TextEncoding* forced);
    void ReloadKeepPosition(const TextEncoding* forced);
    void NewDocument(const std::wstring& path);
    bool Save();
    bool SaveAs();
    bool SaveTo(const std::wstring& path, TextEncoding enc, LineEnding eol);
    void UpdateFileTime();
    void CheckExternalChange();
    void OpenFileCommand();
    void OpenOrReuse(const std::vector<std::wstring>& files);
    void HandleDrop(HDROP drop);
    void Print();

    // Suchen/Ersetzen
    void ShowFindDialog(bool replace);
    LRESULT OnFindMessage(FINDREPLACEW* fr);
    bool FindNext(bool down);
    void ReplaceOne();
    void ReplaceAll();
    void GoToLine();

    // Textoperationen
    void TransformText(bool wholeLines, const std::function<std::wstring(const std::wstring&)>& f);
    void TransformLines(const std::function<void(std::vector<std::wstring>&)>& f);
    void SortLines(int mode);

    // Anzeige
    std::wstring DisplayName() const { return path_.empty() ? std::wstring(L"Unbenannt") : PathFileName(path_); }
    void UpdateTitle(bool force = false);
    void UpdateStatus();
    void SetStatusMessage(const std::wstring& msg);
    HWND MsgOwner() const;

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HWND status_ = nullptr;
    HMENU menu_ = nullptr;
    HMENU encMenu_ = nullptr;
    HMENU eolMenu_ = nullptr;
    HACCEL accel_ = nullptr;
    HFONT font_ = nullptr;
    HFONT uiFont_ = nullptr;   // nur wenn eigene (DPI-abhängige) Schrift erzeugt wurde
    ITextDocument* doc_ = nullptr;
    UINT dpi_ = 96;

    std::wstring path_;
    TextEncoding enc_ = TextEncoding::Utf8;
    LineEnding eol_ = LineEnding::CRLF;
    FILETIME fileTime_{};
    bool haveFileTime_ = false;

    bool wordWrap_ = false;
    bool statusVisible_ = true;
    bool overwrite_ = false;
    bool titleModified_ = false;
    bool titleSet_ = false;
    bool checkingFile_ = false;
    bool prompting_ = false;
    std::wstring statusMsg_;
    std::vector<std::wstring> droppedFiles_;

    // Suchen/Ersetzen
    HWND findDlg_ = nullptr;
    FINDREPLACEW fr_{};
    wchar_t findBuf_[512] = {};
    wchar_t replBuf_[512] = {};
};

EditorWindow* FindEditor(const std::wstring& fullPath) {
    if (fullPath.empty()) return nullptr;
    for (auto* e : g_editors)
        if (EqualsI(e->Path(), fullPath)) return e;
    return nullptr;
}

bool IsLiveEditor(EditorWindow* e) { return std::find(g_editors.begin(), g_editors.end(), e) != g_editors.end(); }

// ---------- Erzeugen ----------

HMENU BuildMenu(HMENU& encMenu, HMENU& eolMenu) {
    HMENU bar = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_NEW, L"&Neu\tStrg+N");
    AppendMenuW(file, MF_STRING, ID_OPEN, L"Ö&ffnen…\tStrg+O");
    AppendMenuW(file, MF_STRING, ID_SAVE, L"&Speichern\tStrg+S");
    AppendMenuW(file, MF_STRING, ID_SAVEAS, L"Speichern &unter…\tStrg+Umschalt+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_RELOAD, L"Neu &laden");
    HMENU reloadEnc = CreatePopupMenu();
    for (int i = 0; i < kEncCount; ++i) AppendMenuW(reloadEnc, MF_STRING, ID_RELOADENC + i, EncodingName(kEncodings[i]));
    AppendMenuW(file, MF_POPUP, (UINT_PTR)reloadEnc, L"Neu laden mit &Kodierung");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_PRINT, L"&Drucken…\tStrg+P");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_CLOSE, L"S&chließen\tStrg+W");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&Datei");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, ID_UNDO, L"&Rückgängig\tStrg+Z");
    AppendMenuW(edit, MF_STRING, ID_REDO, L"&Wiederholen\tStrg+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, ID_CUT, L"&Ausschneiden\tStrg+X");
    AppendMenuW(edit, MF_STRING, ID_COPY, L"&Kopieren\tStrg+C");
    AppendMenuW(edit, MF_STRING, ID_PASTE, L"E&infügen\tStrg+V");
    AppendMenuW(edit, MF_STRING, ID_DELETE, L"&Löschen\tEntf");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, ID_SELALL, L"Alles &markieren\tStrg+A");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, ID_FIND, L"&Suchen…\tStrg+F");
    AppendMenuW(edit, MF_STRING, ID_FINDNEXT, L"&Weitersuchen\tF3");
    AppendMenuW(edit, MF_STRING, ID_FINDPREV, L"Rückwärts s&uchen\tUmschalt+F3");
    AppendMenuW(edit, MF_STRING, ID_REPLACE, L"&Ersetzen…\tStrg+H");
    AppendMenuW(edit, MF_STRING, ID_GOTO, L"&Gehe zu Zeile…\tStrg+G");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, ID_DATETIME, L"&Datum/Uhrzeit einfügen\tF5");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Bearbeiten");

    HMENU text = CreatePopupMenu();
    HMENU sort = CreatePopupMenu();
    AppendMenuW(sort, MF_STRING, ID_SORT_ASC, L"&Aufsteigend");
    AppendMenuW(sort, MF_STRING, ID_SORT_DESC, L"A&bsteigend");
    AppendMenuW(sort, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sort, MF_STRING, ID_SORT_ASC_CI, L"Aufsteigend, &ohne Groß-/Kleinschreibung");
    AppendMenuW(sort, MF_STRING, ID_SORT_DESC_CI, L"Absteigend, oh&ne Groß-/Kleinschreibung");
    AppendMenuW(sort, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sort, MF_STRING, ID_SORT_NAT_ASC, L"&Natürlich aufsteigend (Datei2 < Datei10)");
    AppendMenuW(sort, MF_STRING, ID_SORT_NAT_DESC, L"Natürlich a&bsteigend");
    AppendMenuW(text, MF_POPUP, (UINT_PTR)sort, L"Zeilen &sortieren");
    AppendMenuW(text, MF_STRING, ID_DEDUP, L"&Doppelte Zeilen entfernen");
    AppendMenuW(text, MF_STRING, ID_REVERSE, L"Zeilen &umkehren");
    AppendMenuW(text, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(text, MF_STRING, ID_UPPER, L"&GROSSBUCHSTABEN");
    AppendMenuW(text, MF_STRING, ID_LOWER, L"&kleinbuchstaben");
    AppendMenuW(text, MF_STRING, ID_TITLE, L"&Erster Buchstabe Groß");
    AppendMenuW(text, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(text, MF_STRING, ID_TAB2SPC, L"&Tabulatoren → Leerzeichen");
    AppendMenuW(text, MF_STRING, ID_SPC2TAB, L"&Leerzeichen → Tabulatoren");
    AppendMenuW(text, MF_STRING, ID_TRIM, L"Leerzeichen am &Zeilenende entfernen");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)text, L"&Text");

    encMenu = CreatePopupMenu();
    for (int i = 0; i < kEncCount; ++i)
        AppendMenuW(encMenu, MF_STRING, ID_ENC + i, EncodingName(kEncodings[i]));
    AppendMenuW(encMenu, MF_SEPARATOR, 0, nullptr);
    eolMenu = CreatePopupMenu();
    for (int i = 0; i < kEolCount; ++i)
        AppendMenuW(eolMenu, MF_STRING, ID_EOL + i, LineEndingName(kLineEndings[i]));
    AppendMenuW(encMenu, MF_POPUP, (UINT_PTR)eolMenu, L"&Zeilenende");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)encMenu, L"&Kodierung");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, ID_WRAP, L"&Zeilenumbruch");
    AppendMenuW(view, MF_STRING, ID_FONT, L"&Schriftart…");
    AppendMenuW(view, MF_STRING, ID_STATUSBAR, L"S&tatusleiste");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&Ansicht");
    return bar;
}

HACCEL BuildAccelerators() {
    ACCEL a[] = {
        {FCONTROL | FVIRTKEY, 'N', ID_NEW},
        {FCONTROL | FVIRTKEY, 'O', ID_OPEN},
        {FCONTROL | FVIRTKEY, 'S', ID_SAVE},
        {FCONTROL | FSHIFT | FVIRTKEY, 'S', ID_SAVEAS},
        {FCONTROL | FVIRTKEY, 'P', ID_PRINT},
        {FCONTROL | FVIRTKEY, 'W', ID_CLOSE},
        {FCONTROL | FVIRTKEY, 'Z', ID_UNDO},
        {FCONTROL | FVIRTKEY, 'Y', ID_REDO},
        {FCONTROL | FSHIFT | FVIRTKEY, 'Z', ID_REDO},
        {FCONTROL | FVIRTKEY, 'V', ID_PASTE},
        {FCONTROL | FVIRTKEY, 'A', ID_SELALL},
        {FCONTROL | FVIRTKEY, 'F', ID_FIND},
        {FVIRTKEY, VK_F3, ID_FINDNEXT},
        {FSHIFT | FVIRTKEY, VK_F3, ID_FINDPREV},
        {FCONTROL | FVIRTKEY, 'H', ID_REPLACE},
        {FCONTROL | FVIRTKEY, 'G', ID_GOTO},
        {FVIRTKEY, VK_F5, ID_DATETIME},
    };
    return CreateAcceleratorTableW(a, (int)(sizeof(a) / sizeof(a[0])));
}

bool EnsureInitialized() {
    if (!g_msftedit) {
        g_msftedit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!g_msftedit) g_msftedit = LoadLibraryW(L"Msftedit.dll");
        if (!g_msftedit) {
            MsgError(App::MainWindow(), L"Das RichEdit-Steuerelement (Msftedit.dll) kann nicht geladen werden.\n\n" +
                                            LastErrorMessage());
            return false;
        }
    }
    if (!g_findMsg) g_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
    if (!g_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = EditorWindow::WndProc;
        wc.hInstance = App::Instance();
        wc.hIcon = App::BigIcon();
        wc.hIconSm = App::SmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MsgError(App::MainWindow(), L"Die Fensterklasse des Editors kann nicht registriert werden.\n\n" +
                                            LastErrorMessage());
            return false;
        }
        g_classRegistered = true;
    }
    return true;
}

bool EditorWindow::Create() {
    menu_ = BuildMenu(encMenu_, eolMenu_);
    wordWrap_ = App::Opt().editorWordWrap;
    statusVisible_ = App::Cfg().GetBool(kCfgSection, L"Statusleiste", true);
    g_pendingCreate = this;
    HWND h = CreateWindowExW(WS_EX_ACCEPTFILES, kClassName, kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, DpiScale(nullptr, 900), DpiScale(nullptr, 680), nullptr,
                             menu_, App::Instance(), this);
    if (!h && g_pendingCreate == this && menu_) {
        DestroyMenu(menu_); // Menü wurde keinem Fenster zugeordnet
        menu_ = nullptr;
    }
    return h != nullptr;
}

LRESULT CALLBACK EditorWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    EditorWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<EditorWindow*>(cs->lpCreateParams);
        self->hwnd_ = h;
        if (g_pendingCreate == self) g_pendingCreate = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        g_editors.push_back(self);
    } else {
        self = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        LRESULT r = DefWindowProcW(h, msg, wp, lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), self), g_editors.end());
        delete self;
        return r;
    }
    return self->Handle(msg, wp, lp);
}

void EditorWindow::OnCreate() {
    dpi_ = GetWindowDpi(hwnd_);
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)App::SmallIcon());
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)App::BigIcon());

    edit_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                ES_AUTOHSCROLL | ES_NOHIDESEL,
                            0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)kIdEdit, App::Instance(), nullptr);
    if (!edit_) return;
    // Klartextmodus muss vor dem ersten Text gesetzt werden
    SendMessageW(edit_, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);
    SendMessageW(edit_, EM_EXLIMITTEXT, 0, kMaxTextChars);
    SendMessageW(edit_, EM_SETUNDOLIMIT, 500, 0);
#ifdef TO_ADVANCEDTYPOGRAPHY
    SendMessageW(edit_, EM_SETTYPOGRAPHYOPTIONS, TO_ADVANCEDTYPOGRAPHY, TO_ADVANCEDTYPOGRAPHY);
#endif
    SendMessageW(edit_, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_DROPFILES);
    DragAcceptFiles(edit_, TRUE);
    SetWindowSubclass(edit_, EditSubclassProc, 1, (DWORD_PTR)this);

    // TOM-Schnittstelle (Standard-Tabweite, logische Zeilennummer bei Zeilenumbruch)
    IUnknown* ole = nullptr;
    if (SendMessageW(edit_, EM_GETOLEINTERFACE, 0, (LPARAM)&ole) && ole) {
        void* p = nullptr;
        if (SUCCEEDED(ole->QueryInterface(kIID_ITextDocument, &p))) doc_ = static_cast<ITextDocument*>(p);
        ole->Release();
    }

    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | SBARS_SIZEGRIP | SBARS_TOOLTIPS |
                                                                (statusVisible_ ? WS_VISIBLE : 0),
                              0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)kIdStatus, App::Instance(), nullptr);
    UpdateUIFont();
    ApplyFont();

    accel_ = BuildAccelerators();
    if (accel_) App::RegisterAccelerator(hwnd_, accel_);

    fr_.lStructSize = sizeof(fr_);
    fr_.hwndOwner = hwnd_;
    fr_.lpstrFindWhat = findBuf_;
    fr_.lpstrReplaceWith = replBuf_;
    // Puffer hat 512 Zeichen; 512 ist sowohl als Byte- als auch als Zeichenangabe sicher
    fr_.wFindWhatLen = 512;
    fr_.wReplaceWithLen = 512;
    fr_.Flags = g_lastFindFlags;
    lstrcpynW(findBuf_, g_lastFind.c_str(), 512);
    lstrcpynW(replBuf_, g_lastReplace.c_str(), 512);
}

void EditorWindow::OnDestroy() {
    // Fensterposition für das nächste Editorfenster merken
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        Config& c = App::Cfg();
        const RECT& r = wp.rcNormalPosition;
        c.SetInt(kCfgSection, L"X", r.left);
        c.SetInt(kCfgSection, L"Y", r.top);
        c.SetInt(kCfgSection, L"B", r.right - r.left);
        c.SetInt(kCfgSection, L"H", r.bottom - r.top);
        bool max = IsZoomed(hwnd_) || (IsIconic(hwnd_) && (wp.flags & WPF_RESTORETOMAXIMIZED));
        c.SetBool(kCfgSection, L"Max", max);
    }
    if (findDlg_) {
        App::UnregisterModeless(findDlg_);
        if (IsWindow(findDlg_)) DestroyWindow(findDlg_);
        findDlg_ = nullptr;
    }
    App::UnregisterAccelerator(hwnd_);
    if (accel_) DestroyAcceleratorTable(accel_);
    accel_ = nullptr;
    if (doc_) doc_->Release();
    doc_ = nullptr;
    if (edit_) SendMessageW(edit_, WM_SETFONT, 0, FALSE);
    if (font_) DeleteObject(font_);
    font_ = nullptr;
    if (status_) SendMessageW(status_, WM_SETFONT, 0, FALSE);
    if (uiFont_) DeleteObject(uiFont_);
    uiFont_ = nullptr;
}

void EditorWindow::Show() {
    // Position des zuletzt geschlossenen Editors, bei mehreren Fenstern versetzt
    Config& c = App::Cfg();
    bool max = c.GetBool(kCfgSection, L"Max", false);
    int w = c.GetInt(kCfgSection, L"B", 0), hgt = c.GetInt(kCfgSection, L"H", 0);
    if (w >= 200 && hgt >= 150 && c.Has(kCfgSection, L"X")) {
        int x = c.GetInt(kCfgSection, L"X", 0), y = c.GetInt(kCfgSection, L"Y", 0);
        int others = (int)g_editors.size() - 1;
        RECT r{x, y, x + w, y + hgt};
        if (others > 0) {
            int off = DpiScale(hwnd_, 20) * others;
            RECT moved{x + off, y + off, x + off + w, y + off + hgt};
            HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            // nur versetzen, solange das Fenster noch in den Arbeitsbereich passt (sonst wieder oben links)
            if (mon && GetMonitorInfoW(mon, &mi) && moved.right <= mi.rcWork.right && moved.bottom <= mi.rcWork.bottom)
                r = moved;
        }
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            GetWindowPlacement(hwnd_, &wp);
            wp.flags = 0;
            wp.showCmd = SW_HIDE;
            wp.rcNormalPosition = r;
            SetWindowPlacement(hwnd_, &wp);
        }
    }
    ShowWindow(hwnd_, max ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    SetFocus(edit_);
}

void EditorWindow::BringToFront() {
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

// ---------- Layout, Schriften ----------

void EditorWindow::UpdateUIFont() {
    if (!status_) return;
    HFONT f = App::UIFont();
    HFONT own = nullptr;
    if (dpi_ != SystemDpi()) {
        own = CreateUIFontForDpi(dpi_);
        if (own) f = own;
    }
    SendMessageW(status_, WM_SETFONT, (WPARAM)f, FALSE);
    if (uiFont_) DeleteObject(uiFont_);
    uiFont_ = own;
}

void EditorWindow::UpdateStatusParts() {
    if (!status_) return;
    int widths[6];
    int x = 0;
    const int w[5] = {160, 110, 125, 70, 70};
    for (int i = 0; i < 5; ++i) {
        x += DpiScale(hwnd_, w[i]);
        widths[i] = x;
    }
    widths[5] = -1;
    SendMessageW(status_, SB_SETPARTS, 6, (LPARAM)widths);
}

void EditorWindow::Layout() {
    if (!edit_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int sh = 0;
    if (status_ && statusVisible_) {
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT sr;
        GetWindowRect(status_, &sr);
        sh = sr.bottom - sr.top;
        UpdateStatusParts();
    }
    MoveWindow(edit_, 0, 0, rc.right, std::max(0, (int)rc.bottom - sh), TRUE);
}

void EditorWindow::ApplyFont() {
    if (!edit_) return;
    const Options& o = App::Opt();
    int size = o.editorFontSize > 0 ? o.editorFontSize : 10;
    std::wstring name = o.editorFontName.empty() ? std::wstring(L"Consolas") : o.editorFontName;
    HFONT f = CreateFontW(-MulDiv(size, (int)dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          name.c_str());
    if (!f) return;
    bool mod = IsModified();
    SendMessageW(edit_, WM_SETFONT, (WPARAM)f, TRUE);
    if (font_) DeleteObject(font_);
    font_ = f;
    ApplyTabs();
    ApplyWrap(); // erzwingt Neuformatierung
    SetModified(mod);
}

void EditorWindow::ApplyTabs() {
    if (!edit_ || !font_) return;
    int tw = std::clamp(App::Opt().editorTabWidth, 1, 32);
    HDC dc = GetDC(edit_);
    HGDIOBJ old = SelectObject(dc, font_);
    SIZE sz{};
    GetTextExtentPoint32W(dc, L" ", 1, &sz);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(edit_, dc);
    int emPx = tm.tmHeight - tm.tmInternalLeading;
    if (emPx <= 0) emPx = tm.tmHeight;

    // Tabweite in Punkt, bezogen auf die von RichEdit verwendete Schriftgröße (DPI-unabhängig)
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_SIZE;
    SendMessageW(edit_, EM_GETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    double pts;
    if (cf.yHeight > 0 && emPx > 0)
        pts = (double)tw * sz.cx * (cf.yHeight / 20.0) / emPx;
    else
        pts = (double)tw * sz.cx * 72.0 / (double)dpi_;
    if (pts <= 0) pts = 36;

    bool mod = IsModified();
    bool done = false;
    if (doc_) done = SUCCEEDED(doc_->SetDefaultTabStop((float)pts));
    if (!done) {
        // Ersatz: explizite Tabstopps (maximal 32)
        PARAFORMAT pf{};
        pf.cbSize = sizeof(pf);
        pf.dwMask = PFM_TABSTOPS;
        pf.cTabCount = MAX_TAB_STOPS;
        for (int i = 0; i < MAX_TAB_STOPS; ++i) pf.rgxTabs[i] = (LONG)((i + 1) * pts * 20.0);
        CHARRANGE cr = GetSel();
        SetSel(0, -1);
        SendMessageW(edit_, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
        SetSel(cr.cpMin, cr.cpMax);
    }
    SetModified(mod);
}

void EditorWindow::ApplyWrap() {
    if (!edit_) return;
    SendMessageW(edit_, EM_SETTARGETDEVICE, 0, wordWrap_ ? 0 : 1);
}

// ---------- Text ----------

LONG EditorWindow::TextLength() const {
    GETTEXTLENGTHEX gl{};
    gl.flags = GTL_PRECISE | GTL_NUMCHARS;
    gl.codepage = 1200;
    LRESULT n = SendMessageW(edit_, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    return n > 0 ? (LONG)n : 0;
}

std::wstring EditorWindow::GetEditText(bool crlf, bool selectionOnly) const {
    LONG len = 0;
    if (selectionOnly) {
        CHARRANGE cr = GetSel();
        len = cr.cpMax - cr.cpMin;
        if (crlf) len *= 2; // ungünstigster Fall: jedes Zeichen ein Zeilenende
    } else {
        GETTEXTLENGTHEX gl{};
        gl.flags = GTL_PRECISE | GTL_NUMCHARS | (crlf ? GTL_USECRLF : 0);
        gl.codepage = 1200;
        len = (LONG)SendMessageW(edit_, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    }
    if (len <= 0) return {};
    std::wstring buf((size_t)len + 1, L'\0');
    GETTEXTEX gt{};
    gt.cb = (DWORD)(((size_t)len + 1) * sizeof(wchar_t));
    gt.flags = (crlf ? GT_USECRLF : GT_DEFAULT) | (selectionOnly ? GT_SELECTION : 0);
    gt.codepage = 1200;
    LRESULT n = SendMessageW(edit_, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)buf.data());
    if (n < 0) n = 0;
    buf.resize(std::min((size_t)n, (size_t)len));
    return buf;
}

void EditorWindow::SetEditText(const std::wstring& text) {
    SendMessageW(edit_, WM_SETREDRAW, FALSE, 0);
    SETTEXTEX st{};
    st.flags = ST_DEFAULT;
    st.codepage = 1200;
    SendMessageW(edit_, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)text.c_str());
    SendMessageW(edit_, EM_EMPTYUNDOBUFFER, 0, 0);
    SetModified(false);
    SetSel(0, 0);
    SendMessageW(edit_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(edit_, nullptr, TRUE);
}

// Position der Einfügemarke (aktives Ende der Markierung)
LONG EditorWindow::CaretPos() const {
    CHARRANGE cr = GetSel();
    if (cr.cpMin == cr.cpMax) return cr.cpMin;
    if (doc_) {
        ITextSelection* sel = nullptr;
        if (SUCCEEDED(doc_->GetSelection(&sel)) && sel) {
            long flags = 0;
            sel->GetFlags(&flags);
            sel->Release();
            return (flags & tomSelStartActive) ? cr.cpMin : cr.cpMax;
        }
    }
    return cr.cpMax;
}

void EditorWindow::ReplaceRange(LONG s, LONG e, const std::wstring& text, bool selectResult) {
    LONG oldCaret = GetSel().cpMin;
    int first = (int)SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(edit_, WM_SETREDRAW, FALSE, 0);
    SetSel(s, e);
    SendMessageW(edit_, EM_REPLACESEL, TRUE, (LPARAM)text.c_str());
    if (selectResult) {
        SetSel(s, s + (LONG)text.size());
    } else {
        LONG c = std::min(oldCaret, TextLength());
        SetSel(c, c);
        int now = (int)SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(edit_, EM_LINESCROLL, 0, first - now);
    }
    SendMessageW(edit_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(edit_, nullptr, TRUE);
    UpdateTitle();
    UpdateStatus();
}

void EditorWindow::PastePlain() {
    // Nur Klartext einfügen
    SendMessageW(edit_, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
}

// ---------- Titel und Statusleiste ----------

void EditorWindow::UpdateTitle(bool force) {
    bool mod = IsModified();
    if (!force && titleSet_ && mod == titleModified_) return;
    titleModified_ = mod;
    titleSet_ = true;
    std::wstring t = DisplayName() + (mod ? L"*" : L"") + L" – " + kAppTitle;
    SetWindowTextW(hwnd_, t.c_str());
    if (status_) SendMessageW(status_, SB_SETTEXTW, 4, (LPARAM)(mod ? L"Geändert" : L""));
}

void EditorWindow::SetStatusMessage(const std::wstring& msg) {
    statusMsg_ = msg;
    if (status_) SendMessageW(status_, SB_SETTEXTW, 5, (LPARAM)msg.c_str());
}

void EditorWindow::UpdateStatus() {
    if (!status_ || !edit_) return;
    LONG caret = CaretPos();
    LONG line = 0, lineStart = 0;
    bool done = false;
    if (wordWrap_ && doc_) {
        // Bei Zeilenumbruch: logische Zeile = Absatz
        ITextRange* r = nullptr;
        if (SUCCEEDED(doc_->Range(caret, caret, &r)) && r) {
            long idx = 1, start = caret;
            r->GetIndex(tomParagraph, &idx);
            r->StartOf(tomParagraph, tomMove, nullptr);
            r->GetStart(&start);
            r->Release();
            line = idx - 1;
            lineStart = start;
            done = true;
        }
    }
    if (!done) {
        line = (LONG)SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, caret);
        lineStart = (LONG)SendMessageW(edit_, EM_LINEINDEX, line, 0);
        if (lineStart < 0 || lineStart > caret) lineStart = caret;
    }
    // Spalte: Zeichen (Surrogatpaare = 1) mit expandierten Tabulatoren
    LONG col = caret - lineStart;
    if (col > 0 && col <= 20000) {
        std::wstring buf((size_t)col + 1, L'\0');
        TEXTRANGEW tr{};
        tr.chrg.cpMin = lineStart;
        tr.chrg.cpMax = caret;
        tr.lpstrText = buf.data();
        LRESULT n = SendMessageW(edit_, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
        if (n > 0) {
            int tw = std::clamp(App::Opt().editorTabWidth, 1, 32);
            LONG c = 0;
            for (LRESULT i = 0; i < n; ++i) {
                wchar_t ch = buf[(size_t)i];
                if (ch == L'\t')
                    c += tw - (c % tw);
                else if (!IsLowSurrogate(ch))
                    ++c;
            }
            col = c;
        }
    }
    std::wstring pos = Format(L"Zeile %ld, Spalte %ld", (long)line + 1, (long)col + 1);
    SendMessageW(status_, SB_SETTEXTW, 0, (LPARAM)pos.c_str());
    SendMessageW(status_, SB_SETTEXTW, 1, (LPARAM)EncodingName(enc_));
    SendMessageW(status_, SB_SETTEXTW, 2, (LPARAM)LineEndingName(eol_));
    SendMessageW(status_, SB_SETTEXTW, 3, (LPARAM)(overwrite_ ? L"Überschr." : L"Einfg"));
    SendMessageW(status_, SB_SETTEXTW, 4, (LPARAM)(IsModified() ? L"Geändert" : L""));
    std::wstring last = statusMsg_.empty() ? (path_.empty() ? std::wstring(L"Unbenannt") : path_) : statusMsg_;
    SendMessageW(status_, SB_SETTEXTW, 5, (LPARAM)last.c_str());
    SendMessageW(status_, SB_SETTIPTEXTW, 5, (LPARAM)(path_.empty() ? L"Unbenannt" : path_.c_str()));
}

HWND EditorWindow::MsgOwner() const {
    if (IsWindowVisible(hwnd_)) return hwnd_;
    return App::MainWindow();
}

// ---------- Menüzustand ----------

void EditorWindow::UpdateMenuState() {
    if (!menu_) return;
    CHARRANGE cr = GetSel();
    bool sel = cr.cpMin != cr.cpMax;
    auto en = [&](int id, bool on) { EnableMenuItem(menu_, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    en(ID_UNDO, SendMessageW(edit_, EM_CANUNDO, 0, 0) != 0);
    en(ID_REDO, SendMessageW(edit_, EM_CANREDO, 0, 0) != 0);
    en(ID_CUT, sel);
    en(ID_COPY, sel);
    en(ID_DELETE, sel);
    en(ID_PASTE, IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT));
    bool onDisk = !path_.empty() && FileExists(path_);
    en(ID_RELOAD, onDisk);
    for (int i = 0; i < kEncCount; ++i) en(ID_RELOADENC + i, onDisk);
    CheckMenuItem(menu_, ID_WRAP, MF_BYCOMMAND | (wordWrap_ ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu_, ID_STATUSBAR, MF_BYCOMMAND | (statusVisible_ ? MF_CHECKED : MF_UNCHECKED));
    if (encMenu_) CheckMenuRadioItem(encMenu_, 0, kEncCount - 1, EncodingIndex(enc_), MF_BYPOSITION);
    if (eolMenu_) CheckMenuRadioItem(eolMenu_, 0, kEolCount - 1, LineEndingIndex(eol_), MF_BYPOSITION);
}

void EditorWindow::ShowContextMenu(int x, int y) {
    if (x == -1 && y == -1) {
        // Tastatur: an der Einfügemarke
        POINTL pt{0, 0};
        SendMessageW(edit_, EM_POSFROMCHAR, (WPARAM)&pt, CaretPos());
        POINT p{pt.x, pt.y};
        ClientToScreen(edit_, &p);
        x = p.x;
        y = p.y;
    }
    CHARRANGE cr = GetSel();
    bool sel = cr.cpMin != cr.cpMax;
    auto flag = [](bool on) -> UINT { return MF_STRING | (on ? MF_ENABLED : MF_GRAYED); };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, flag(SendMessageW(edit_, EM_CANUNDO, 0, 0) != 0), ID_UNDO, L"&Rückgängig");
    AppendMenuW(m, flag(SendMessageW(edit_, EM_CANREDO, 0, 0) != 0), ID_REDO, L"&Wiederholen");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, flag(sel), ID_CUT, L"&Ausschneiden");
    AppendMenuW(m, flag(sel), ID_COPY, L"&Kopieren");
    AppendMenuW(m, flag(IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)), ID_PASTE,
                L"E&infügen");
    AppendMenuW(m, flag(sel), ID_DELETE, L"&Löschen");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_SELALL, L"Alles &markieren");
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, x, y, 0, hwnd_, nullptr);
    DestroyMenu(m);
}

// ---------- Datei ----------

void EditorWindow::NewDocument(const std::wstring& path) {
    path_ = path;
    enc_ = App::Opt().newFileEncoding;
    eol_ = App::Opt().newFileLineEnding;
    haveFileTime_ = false;
    statusMsg_.clear();
    SetEditText(L"");
    UpdateTitle(true);
    UpdateStatus();
}

bool EditorWindow::Open(const std::wstring& path) {
    if (path.empty() || !FileExists(path)) {
        NewDocument(path);
        return true;
    }
    return LoadFile(path, nullptr);
}

bool EditorWindow::LoadFile(const std::wstring& path, const TextEncoding* forced) {
    uint64_t size = GetFileSize64(path);
    if (size != UINT64_MAX && size > kLargeFileWarn) {
        if (!MsgConfirm(MsgOwner(), L"Die Datei „" + path + L"“ ist sehr groß (" + FormatSize(size) +
                                        L").\nDas Laden kann lange dauern und viel Speicher belegen.\n\nTrotzdem öffnen?"))
            return false;
    }
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    TextFile tf;
    bool ok = LoadTextFile(path, tf, forced);
    DWORD err = ok ? 0 : GetLastError();
    SetCursor(oldCursor);
    if (!ok) {
        MsgError(MsgOwner(), L"Die Datei „" + path + L"“ kann nicht gelesen werden.\n\n" + LastErrorMessage(err));
        return false;
    }
    if (tf.text.size() > (size_t)kMaxTextChars) {
        MsgError(MsgOwner(), L"Die Datei „" + path + L"“ ist zu groß für den Editor.");
        return false;
    }
    if (tf.text.find(L'\0') != std::wstring::npos) {
        if (!MsgConfirm(MsgOwner(), L"Die Datei „" + path +
                                        L"“ enthält Nullzeichen und ist vermutlich keine Textdatei.\n"
                                        L"Nullzeichen werden als Leerzeichen angezeigt; beim Speichern wird die Datei "
                                        L"dadurch verändert.\n\nTrotzdem öffnen?"))
            return false;
        std::replace(tf.text.begin(), tf.text.end(), L'\0', L' ');
    }
    path_ = path;
    enc_ = tf.encoding;
    eol_ = tf.lineEnding;
    if (tf.wasEmpty && !forced) {
        // leere Datei: Standardkodierung für neue Dateien (UTF-8 ohne BOM)
        enc_ = App::Opt().newFileEncoding;
        eol_ = App::Opt().newFileLineEnding;
    }
    statusMsg_.clear();
    SetEditText(tf.text);
    UpdateFileTime();
    UpdateTitle(true);
    UpdateStatus();
    return true;
}

void EditorWindow::ReloadKeepPosition(const TextEncoding* forced) {
    if (path_.empty()) return;
    CHARRANGE cr = GetSel();
    int first = (int)SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (!LoadFile(path_, forced)) return;
    LONG len = TextLength();
    SetSel(std::min(cr.cpMin, len), std::min(cr.cpMax, len));
    int now = (int)SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(edit_, EM_LINESCROLL, 0, first - now);
}

void EditorWindow::UpdateFileTime() {
    haveFileTime_ = !path_.empty() && GetWriteTime(path_, fileTime_);
}

void EditorWindow::CheckExternalChange() {
    if (checkingFile_ || prompting_ || path_.empty() || !haveFileTime_) return;
    FILETIME ft;
    if (!GetWriteTime(path_, ft)) return; // gelöscht oder nicht erreichbar: nichts tun
    if (CompareFileTime(&ft, &fileTime_) == 0) return;
    fileTime_ = ft; // nur einmal fragen
    checkingFile_ = true;
    std::wstring msg = L"Die Datei „" + path_ + L"“ wurde außerhalb des Editors geändert.\n\nNeu laden?";
    if (IsModified()) msg += L"\n\nAchtung: Ihre ungespeicherten Änderungen gehen dabei verloren.";
    if (MsgConfirm(hwnd_, msg)) ReloadKeepPosition(nullptr);
    checkingFile_ = false;
}

bool EditorWindow::Save() {
    if (path_.empty()) return SaveAs();
    return SaveTo(path_, enc_, eol_);
}

bool EditorWindow::SaveAs() {
    std::wstring initial = path_;
    if (initial.empty()) {
        std::wstring dir = App::ActivePaneContext().dir;
        initial = dir.empty() ? std::wstring(L"Unbenannt.txt") : PathCombine(dir, L"Unbenannt.txt");
    }
    std::wstring p = SaveFileDialog(hwnd_, L"Speichern unter", initial,
                                    L"Alle Dateien (*.*)|*.*|Textdateien (*.txt)|*.txt");
    if (p.empty()) return false;
    p = FullPath(p);
    EditorWindow* other = FindEditor(p);
    if (other && other != this) {
        MsgError(hwnd_, L"Die Datei „" + p + L"“ ist bereits in einem anderen Editorfenster geöffnet.");
        return false;
    }
    return SaveTo(p, enc_, eol_);
}

bool EditorWindow::SaveTo(const std::wstring& path, TextEncoding enc, LineEnding eol) {
    DWORD attr = GetFileAttributesW(LongPath(path).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
        MsgError(hwnd_, L"Die Datei „" + path + L"“ ist schreibgeschützt und kann nicht gespeichert werden.\n\n" +
                            LastErrorMessage(ERROR_ACCESS_DENIED));
        if (MsgConfirm(hwnd_, L"Unter einem anderen Namen speichern?")) return SaveAs();
        return false;
    }
    std::wstring text = ConvertLineEndings(GetEditText(true, false), eol);
    bool unmappable = false;
    std::vector<uint8_t> bytes = EncodeText(text, enc, &unmappable);
    if (unmappable && (enc == TextEncoding::Ansi || enc == TextEncoding::Oem)) {
        std::wstring name = EncodingName(enc);
        std::wstring msg = L"Der Text enthält Zeichen, die in der Kodierung „" + name +
                           L"“ nicht darstellbar sind und beim Speichern verloren gingen (Ersatzzeichen „?“).\n\n"
                           L"Stattdessen als UTF-8 speichern?\n\n"
                           L"Ja\t– als UTF-8 speichern\nNein\t– trotzdem als " + name +
                           L" speichern\nAbbrechen\t– nicht speichern";
        int r = MessageBoxW(hwnd_, msg.c_str(), kAppTitle, MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return false;
        if (r == IDYES) {
            enc = TextEncoding::Utf8;
            bytes = EncodeText(text, enc, nullptr);
        }
    }
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = WriteFileBytes(path, bytes.data(), bytes.size());
    DWORD err = ok ? 0 : GetLastError();
    SetCursor(oldCursor);
    if (!ok) {
        MsgError(hwnd_, L"Die Datei „" + path + L"“ kann nicht gespeichert werden.\n\n" + LastErrorMessage(err));
        if (err == ERROR_ACCESS_DENIED || err == ERROR_WRITE_PROTECT || err == ERROR_SHARING_VIOLATION ||
            err == ERROR_LOCK_VIOLATION || err == ERROR_PATH_NOT_FOUND) {
            if (MsgConfirm(hwnd_, L"Unter einem anderen Namen speichern?")) return SaveAs();
        }
        return false;
    }
    path_ = path;
    enc_ = enc;
    eol_ = eol;
    SetModified(false);
    UpdateFileTime();
    LogOperation(L"Gespeichert: " + path);
    statusMsg_.clear();
    UpdateTitle(true);
    UpdateStatus();
    return true;
}

bool EditorWindow::QueryClose() {
    if (!edit_ || !IsModified()) return true;
    if (prompting_) return false;
    prompting_ = true;
    BringToFront();
    int r = MsgYesNoCancel(hwnd_, L"Die Änderungen an „" + DisplayName() + L"“ speichern?");
    bool ok = r == IDNO || (r == IDYES && Save());
    prompting_ = false;
    return ok;
}

// Öffnet Dateien: die erste im aktuellen Fenster, wenn es leer und unverändert ist, sonst in neuen Fenstern.
void EditorWindow::OpenOrReuse(const std::vector<std::wstring>& files) {
    bool reuse = IsEmptyAndUnchanged();
    for (const auto& f : files) {
        std::wstring full = FullPath(f);
        if (FindEditor(full) || !reuse) {
            OpenTextEditor(full);
            continue;
        }
        reuse = false;
        LoadFile(full, nullptr);
    }
}

void EditorWindow::OpenFileCommand() {
    std::wstring dir = path_.empty() ? App::ActivePaneContext().dir : PathParent(path_);
    std::wstring p = OpenFileDialog(hwnd_, L"Öffnen", dir, L"Alle Dateien (*.*)|*.*|Textdateien (*.txt)|*.txt");
    if (p.empty()) return;
    OpenOrReuse({p});
}

void EditorWindow::HandleDrop(HDROP drop) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> files;
    for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring s((size_t)len + 1, L'\0');
        UINT got = DragQueryFileW(drop, i, s.data(), len + 1);
        s.resize(got);
        if (!s.empty() && FileExists(s)) files.push_back(s);
    }
    if (files.empty()) return;
    // Erst nach Abschluss der Drop-Verarbeitung (RichEdit/Shell) öffnen
    droppedFiles_.insert(droppedFiles_.end(), files.begin(), files.end());
    PostMessageW(hwnd_, WM_APP_OPENDROPPED, 0, 0);
}

// ---------- Drucken ----------

void EditorWindow::Print() {
    CHARRANGE cr = GetSel();
    bool hasSel = cr.cpMin != cr.cpMax;
    PRINTDLGW pd{};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = hwnd_;
    pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_USEDEVMODECOPIESANDCOLLATE | (hasSel ? 0 : PD_NOSELECTION);
    pd.nCopies = 1;
    if (!PrintDlgW(&pd)) {
        DWORD e = CommDlgExtendedError();
        if (e) MsgError(hwnd_, Format(L"Der Druckdialog kann nicht angezeigt werden (Fehler %lu).", (unsigned long)e));
        return;
    }
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    HDC dc = pd.hDC;
    if (!dc) return;
    bool selOnly = (pd.Flags & PD_SELECTION) != 0;
    std::wstring text = GetEditText(true, selOnly);

    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const Options& o = App::Opt();
    int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY);
    int horz = GetDeviceCaps(dc, HORZRES), vert = GetDeviceCaps(dc, VERTRES);
    int physW = GetDeviceCaps(dc, PHYSICALWIDTH), physH = GetDeviceCaps(dc, PHYSICALHEIGHT);
    int offX = GetDeviceCaps(dc, PHYSICALOFFSETX), offY = GetDeviceCaps(dc, PHYSICALOFFSETY);
    if (physW <= 0) physW = horz;
    if (physH <= 0) physH = vert;
    // Ränder 2 cm (bezogen auf das Blatt), umgerechnet auf den bedruckbaren Bereich
    int mX = MulDiv(dpiX, 20, 254), mY = MulDiv(dpiY, 20, 254);
    int left = std::max(mX - offX, 0);
    int top = std::max(mY - offY, 0);
    int right = horz - std::max(mX - (physW - horz - offX), 0);
    int bottom = vert - std::max(mY - (physH - vert - offY), 0);
    if (right - left < dpiX || bottom - top < dpiY) {
        left = 0;
        top = 0;
        right = horz;
        bottom = vert;
    }

    std::wstring face = o.editorFontName.empty() ? std::wstring(L"Consolas") : o.editorFontName;
    int pt = o.editorFontSize > 0 ? o.editorFontSize : 10;
    HFONT body = CreateFontW(-MulDiv(pt, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, face.c_str());
    HFONT head = CreateFontW(-MulDiv(pt, dpiY, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, face.c_str());
    HGDIOBJ oldFont = SelectObject(dc, body);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int lineH = tm.tmHeight + tm.tmExternalLeading;
    if (lineH <= 0) lineH = MulDiv(pt, dpiY, 72);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));

    std::wstring docName = DisplayName();
    DOCINFOW di{};
    di.cbSize = sizeof(di);
    di.lpszDocName = docName.c_str();
    bool ok = StartDocW(dc, &di) > 0;
    int page = 0;
    int y = bottom; // erzwingt neue Seite beim ersten Text
    int bodyTop = top + 2 * lineH;
    bool pageOpen = false;

    auto newPage = [&]() -> bool {
        if (pageOpen && EndPage(dc) <= 0) return false;
        if (StartPage(dc) <= 0) return false;
        pageOpen = true;
        ++page;
        SelectObject(dc, head);
        TextOutW(dc, left, top, docName.c_str(), (int)docName.size());
        std::wstring pg = Format(L"Seite %d", page);
        SIZE sz{};
        GetTextExtentPoint32W(dc, pg.c_str(), (int)pg.size(), &sz);
        TextOutW(dc, right - sz.cx, top, pg.c_str(), (int)pg.size());
        int ly = top + lineH + lineH / 4;
        HPEN pen = CreatePen(PS_SOLID, std::max(1, dpiY / 150), RGB(0, 0, 0));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, left, ly, nullptr);
        LineTo(dc, right, ly);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        SelectObject(dc, body);
        y = bodyTop;
        return true;
    };

    int tw = std::clamp(o.editorTabWidth, 1, 32);
    int width = right - left;
    LineList ll = SplitLines(text);
    for (size_t li = 0; ok && li < ll.lines.size(); ++li) {
        // Seitenvorschub (\f) erzwingt eine neue Seite
        std::vector<std::wstring> parts = Split(ll.lines[li], L'\f', false);
        if (parts.empty()) parts.push_back(L"");
        for (size_t pi = 0; ok && pi < parts.size(); ++pi) {
            if (pi > 0) y = bottom;
            std::wstring line = ExpandTabs(parts[pi], tw);
            size_t pos = 0;
            do {
                if (y + lineH > bottom && !newPage()) {
                    ok = false;
                    break;
                }
                size_t rest = line.size() - pos;
                if (rest == 0) {
                    y += lineH;
                    break;
                }
                int fit = 0;
                SIZE sz{};
                int cnt = (int)std::min<size_t>(rest, 8192);
                GetTextExtentExPointW(dc, line.c_str() + pos, cnt, width, &fit, nullptr, &sz);
                if (fit <= 0) fit = 1;
                if ((size_t)fit < rest) {
                    // nicht innerhalb eines Surrogatpaars trennen, möglichst an einem Leerzeichen
                    if (fit > 1 && IsHighSurrogate(line[pos + fit - 1])) --fit;
                    for (int k = fit - 1; k > 0; --k) {
                        if (line[pos + k] == L' ') {
                            fit = k + 1;
                            break;
                        }
                    }
                }
                TextOutW(dc, left, y, line.c_str() + pos, fit);
                pos += (size_t)fit;
                y += lineH;
            } while (pos < line.size());
        }
    }
    if (ok && page == 0) ok = newPage(); // leeres Dokument: eine Seite mit Kopfzeile
    if (pageOpen && ok) ok = EndPage(dc) > 0;
    if (ok)
        EndDoc(dc);
    else
        AbortDoc(dc);
    SelectObject(dc, oldFont);
    DeleteObject(body);
    DeleteObject(head);
    DeleteDC(dc);
    SetCursor(oldCursor);
    if (!ok) MsgError(hwnd_, L"Fehler beim Drucken.\n\n" + LastErrorMessage());
}

// ---------- Suchen/Ersetzen ----------

void EditorWindow::ShowFindDialog(bool replace) {
    // Markierter Text (einzeilig) als Suchbegriff übernehmen
    CHARRANGE cr = GetSel();
    if (cr.cpMax > cr.cpMin && cr.cpMax - cr.cpMin < 500) {
        std::wstring s = GetEditText(false, true);
        if (!s.empty() && s.find_first_of(L"\r\n") == std::wstring::npos) lstrcpynW(findBuf_, s.c_str(), 512);
    }
    if (findDlg_) {
        App::UnregisterModeless(findDlg_);
        if (IsWindow(findDlg_)) DestroyWindow(findDlg_);
        findDlg_ = nullptr;
    }
    fr_.Flags &= (FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD);
    if (replace) fr_.Flags |= FR_DOWN;
    findDlg_ = replace ? ReplaceTextW(&fr_) : FindTextW(&fr_);
    if (findDlg_) App::RegisterModeless(findDlg_);
}

LRESULT EditorWindow::OnFindMessage(FINDREPLACEW* fr) {
    if (fr->Flags & FR_DIALOGTERM) {
        if (findDlg_) App::UnregisterModeless(findDlg_);
        findDlg_ = nullptr;
        SetFocus(edit_);
        return 0;
    }
    g_lastFind = findBuf_;
    g_lastReplace = replBuf_;
    g_lastFindFlags = fr->Flags & (FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD);
    if (fr->Flags & FR_REPLACEALL)
        ReplaceAll();
    else if (fr->Flags & FR_REPLACE)
        ReplaceOne();
    else if (fr->Flags & FR_FINDNEXT)
        FindNext((fr->Flags & FR_DOWN) != 0);
    return 0;
}

bool EditorWindow::FindNext(bool down) {
    std::wstring what = findBuf_;
    if (what.empty()) {
        ShowFindDialog(false);
        return false;
    }
    CHARRANGE cr = GetSel();
    FINDTEXTEXW ft{};
    ft.lpstrText = what.c_str();
    WPARAM flags = (fr_.Flags & (FR_MATCHCASE | FR_WHOLEWORD)) | (down ? FR_DOWN : 0);
    if (down) {
        ft.chrg.cpMin = cr.cpMax;
        ft.chrg.cpMax = -1;
    } else {
        ft.chrg.cpMin = cr.cpMin;
        ft.chrg.cpMax = 0;
    }
    LRESULT pos = SendMessageW(edit_, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
    bool wrapped = false;
    if (pos < 0) {
        // am Anfang bzw. Ende fortsetzen
        if (down) {
            ft.chrg.cpMin = 0;
            ft.chrg.cpMax = -1;
        } else {
            ft.chrg.cpMin = TextLength();
            ft.chrg.cpMax = 0;
        }
        pos = SendMessageW(edit_, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
        wrapped = pos >= 0;
    }
    if (pos < 0) {
        HWND owner = (findDlg_ && IsWindowVisible(findDlg_)) ? findDlg_ : hwnd_;
        MessageBoxW(owner, (L"„" + what + L"“ wurde nicht gefunden.").c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
        return false;
    }
    SetSel(ft.chrgText.cpMin, ft.chrgText.cpMax);
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    if (wrapped) SetStatusMessage(down ? L"Suche am Anfang fortgesetzt." : L"Suche am Ende fortgesetzt.");
    return true;
}

void EditorWindow::ReplaceOne() {
    std::wstring what = findBuf_;
    if (what.empty()) return;
    CHARRANGE cr = GetSel();
    if (cr.cpMax > cr.cpMin) {
        std::wstring sel = GetEditText(false, true);
        bool match = (fr_.Flags & FR_MATCHCASE) ? sel == what : EqualsI(sel, what);
        if (match) {
            SendMessageW(edit_, EM_REPLACESEL, TRUE, (LPARAM)replBuf_);
            UpdateTitle();
        }
    }
    FindNext(true);
}

void EditorWindow::ReplaceAll() {
    std::wstring what = findBuf_;
    std::wstring with = replBuf_;
    if (what.empty()) return;
    bool matchCase = (fr_.Flags & FR_MATCHCASE) != 0;
    bool wholeWord = (fr_.Flags & FR_WHOLEWORD) != 0;
    std::wstring raw = GetRawText();
    std::wstring hay = raw, needle = what;
    if (!matchCase) {
        // Kleinschreibung behält die Länge (einfache Abbildung); sonst Rückfall auf exakten Vergleich
        std::wstring lh = ToLower(raw), ln = ToLower(what);
        if (lh.size() == raw.size() && ln.size() == what.size()) {
            hay = std::move(lh);
            needle = std::move(ln);
        }
    }
    std::wstring out;
    out.reserve(raw.size());
    size_t pos = 0, count = 0, last = 0;
    while (pos <= hay.size()) {
        size_t f = hay.find(needle, pos);
        if (f == std::wstring::npos) break;
        if (wholeWord) {
            bool okL = f == 0 || !IsWordChar(hay[f - 1]);
            bool okR = f + needle.size() >= hay.size() || !IsWordChar(hay[f + needle.size()]);
            if (!okL || !okR) {
                pos = f + 1;
                continue;
            }
        }
        out.append(raw, last, f - last);
        out += with;
        last = f + needle.size();
        pos = last;
        ++count;
    }
    if (count == 0) {
        HWND owner = (findDlg_ && IsWindowVisible(findDlg_)) ? findDlg_ : hwnd_;
        MessageBoxW(owner, (L"„" + what + L"“ wurde nicht gefunden.").c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }
    out.append(raw, last, std::wstring::npos);
    ReplaceRange(0, (LONG)raw.size(), out, false);
    std::wstring msg = IntToStr((long long)count) + L" Vorkommen ersetzt.";
    SetStatusMessage(msg);
    HWND owner = (findDlg_ && IsWindowVisible(findDlg_)) ? findDlg_ : hwnd_;
    MessageBoxW(owner, msg.c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
}

void EditorWindow::GoToLine() {
    std::wstring raw = GetRawText();
    // Zeilenanfänge ermitteln (CR, LF, CRLF)
    std::vector<LONG> starts{0};
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\r' && i + 1 < raw.size() && raw[i + 1] == L'\n') ++i;
        if (IsBreak(raw[i])) starts.push_back((LONG)(i + 1));
    }
    LONG caret = CaretPos();
    size_t cur = (size_t)(std::upper_bound(starts.begin(), starts.end(), caret) - starts.begin());
    std::wstring value = IntToStr((long long)cur);
    std::wstring prompt = L"Zeilennummer (1 – " + IntToStr((long long)starts.size()) + L"):";
    if (!InputBox(hwnd_, L"Gehe zu Zeile", prompt, value)) return;
    long long n = StrToInt(Trim(value), 0);
    if (n < 1) {
        MsgError(hwnd_, L"Bitte eine gültige Zeilennummer eingeben.");
        return;
    }
    if ((unsigned long long)n > starts.size()) n = (long long)starts.size();
    LONG p = starts[(size_t)n - 1];
    SetSel(p, p);
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    SetFocus(edit_);
}

// ---------- Textoperationen ----------

// Wendet f auf die Markierung (bei wholeLines auf ganze Zeilen erweitert) oder, ohne Markierung, auf den
// gesamten Text an.
void EditorWindow::TransformText(bool wholeLines, const std::function<std::wstring(const std::wstring&)>& f) {
    CHARRANGE cr = GetSel();
    std::wstring raw = GetRawText();
    LONG n = (LONG)raw.size();
    bool hadSel = cr.cpMin != cr.cpMax;
    LONG s = 0, e = n;
    if (hadSel) {
        s = std::clamp(std::min(cr.cpMin, cr.cpMax), (LONG)0, n);
        e = std::clamp(std::max(cr.cpMin, cr.cpMax), (LONG)0, n);
        if (cr.cpMax < 0) e = n;
        if (wholeLines) {
            while (s > 0 && !IsBreak(raw[(size_t)s - 1])) --s;
            // endet die Markierung am Zeilenanfang, gehört diese Zeile nicht dazu
            if (!(e > s && IsBreak(raw[(size_t)e - 1])))
                while (e < n && !IsBreak(raw[(size_t)e])) ++e;
        }
    }
    std::wstring seg = raw.substr((size_t)s, (size_t)(e - s));
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring out = f(seg);
    SetCursor(oldCursor);
    if (out == seg) {
        SetStatusMessage(L"Keine Änderung.");
        return;
    }
    ReplaceRange(s, e, out, hadSel);
}

void EditorWindow::TransformLines(const std::function<void(std::vector<std::wstring>&)>& f) {
    TransformText(true, [&](const std::wstring& seg) {
        LineList ll = SplitLines(seg);
        f(ll.lines);
        return JoinLines(ll);
    });
}

void EditorWindow::SortLines(int mode) {
    TransformLines([mode](std::vector<std::wstring>& lines) {
        std::function<int(const std::wstring&, const std::wstring&)> cmp;
        switch (mode) {
        case ID_SORT_ASC:
        case ID_SORT_DESC:
            cmp = [](const std::wstring& a, const std::wstring& b) {
                int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, a.c_str(), (int)a.size(), b.c_str(),
                                        (int)b.size(), nullptr, nullptr, 0);
                if (r == 0) return a.compare(b);
                return r - CSTR_EQUAL;
            };
            break;
        case ID_SORT_ASC_CI:
        case ID_SORT_DESC_CI:
            cmp = [](const std::wstring& a, const std::wstring& b) { return CompareI(a, b); };
            break;
        default:
            cmp = [](const std::wstring& a, const std::wstring& b) { return CompareNatural(a, b); };
            break;
        }
        bool desc = mode == ID_SORT_DESC || mode == ID_SORT_DESC_CI || mode == ID_SORT_NAT_DESC;
        std::stable_sort(lines.begin(), lines.end(), [&](const std::wstring& a, const std::wstring& b) {
            return desc ? cmp(b, a) < 0 : cmp(a, b) < 0;
        });
    });
}

// ---------- Befehle ----------

void EditorWindow::OnCommand(int id) {
    const int tw = std::clamp(App::Opt().editorTabWidth, 1, 32);
    switch (id) {
    case ID_NEW: OpenTextEditor(L""); break;
    case ID_OPEN: OpenFileCommand(); break;
    case ID_SAVE: Save(); break;
    case ID_SAVEAS: SaveAs(); break;
    case ID_RELOAD:
        if (path_.empty() || !FileExists(path_)) break;
        if (IsModified() && !MsgConfirm(hwnd_, L"Die Datei neu laden? Ihre Änderungen gehen dabei verloren.")) break;
        ReloadKeepPosition(nullptr);
        break;
    case ID_PRINT: Print(); break;
    case ID_CLOSE: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;

    case ID_UNDO: SendMessageW(edit_, EM_UNDO, 0, 0); break;
    case ID_REDO: SendMessageW(edit_, EM_REDO, 0, 0); break;
    case ID_CUT: SendMessageW(edit_, WM_CUT, 0, 0); break;
    case ID_COPY: SendMessageW(edit_, WM_COPY, 0, 0); break;
    case ID_PASTE: PastePlain(); break;
    case ID_DELETE: SendMessageW(edit_, WM_CLEAR, 0, 0); break;
    case ID_SELALL: SetSel(0, -1); break;
    case ID_FIND: ShowFindDialog(false); break;
    case ID_REPLACE: ShowFindDialog(true); break;
    case ID_FINDNEXT: FindNext(true); break;
    case ID_FINDPREV: FindNext(false); break;
    case ID_GOTO: GoToLine(); break;
    case ID_DATETIME: {
        wchar_t d[128] = {}, t[64] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, d, 128, nullptr);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, t, 64);
        std::wstring s = std::wstring(d) + L" " + t;
        SendMessageW(edit_, EM_REPLACESEL, TRUE, (LPARAM)s.c_str());
        break;
    }

    case ID_SORT_ASC:
    case ID_SORT_DESC:
    case ID_SORT_ASC_CI:
    case ID_SORT_DESC_CI:
    case ID_SORT_NAT_ASC:
    case ID_SORT_NAT_DESC: SortLines(id); break;
    case ID_DEDUP:
        TransformLines([](std::vector<std::wstring>& lines) {
            std::unordered_set<std::wstring> seen;
            std::vector<std::wstring> out;
            out.reserve(lines.size());
            for (auto& l : lines)
                if (seen.insert(l).second) out.push_back(l);
            lines.swap(out);
        });
        break;
    case ID_REVERSE:
        TransformLines([](std::vector<std::wstring>& lines) { std::reverse(lines.begin(), lines.end()); });
        break;
    case ID_UPPER: TransformText(false, [](const std::wstring& s) { return ToUpper(s); }); break;
    case ID_LOWER: TransformText(false, [](const std::wstring& s) { return ToLower(s); }); break;
    case ID_TITLE: TransformText(false, [](const std::wstring& s) { return TitleCase(s); }); break;
    case ID_TAB2SPC:
        TransformLines([tw](std::vector<std::wstring>& lines) {
            for (auto& l : lines) l = ExpandTabs(l, tw);
        });
        break;
    case ID_SPC2TAB:
        TransformLines([tw](std::vector<std::wstring>& lines) {
            for (auto& l : lines) l = CompressSpaces(l, tw);
        });
        break;
    case ID_TRIM:
        TransformLines([](std::vector<std::wstring>& lines) {
            for (auto& l : lines) l = TrimRight(l);
        });
        break;

    case ID_WRAP:
        wordWrap_ = !wordWrap_;
        App::Opt().editorWordWrap = wordWrap_;
        ApplyWrap();
        SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
        UpdateStatus();
        break;
    case ID_FONT: {
        const Options& o = App::Opt();
        LOGFONTW lf{};
        HDC dc = GetDC(nullptr);
        lf.lfHeight = -MulDiv(o.editorFontSize > 0 ? o.editorFontSize : 10, GetDeviceCaps(dc, LOGPIXELSY), 72);
        ReleaseDC(nullptr, dc);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpynW(lf.lfFaceName, o.editorFontName.c_str(), LF_FACESIZE);
        CHOOSEFONTW cf{};
        cf.lStructSize = sizeof(cf);
        cf.hwndOwner = hwnd_;
        cf.lpLogFont = &lf;
        cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;
        if (ChooseFontW(&cf)) {
            App::Opt().editorFontName = lf.lfFaceName;
            App::Opt().editorFontSize = std::max(1, (cf.iPointSize + 5) / 10);
            for (auto* e : g_editors) e->ApplyFont(); // Einstellung gilt für alle Editorfenster
            UpdateStatus();
        }
        break;
    }
    case ID_STATUSBAR:
        statusVisible_ = !statusVisible_;
        App::Cfg().SetBool(kCfgSection, L"Statusleiste", statusVisible_);
        ShowWindow(status_, statusVisible_ ? SW_SHOW : SW_HIDE);
        Layout();
        UpdateStatus();
        break;

    default:
        if (id >= ID_RELOADENC && id < ID_RELOADENC + kEncCount) {
            if (path_.empty() || !FileExists(path_)) break;
            if (IsModified() && !MsgConfirm(hwnd_, L"Die Datei neu laden? Ihre Änderungen gehen dabei verloren."))
                break;
            TextEncoding e = kEncodings[id - ID_RELOADENC];
            ReloadKeepPosition(&e);
        } else if (id >= ID_ENC && id < ID_ENC + kEncCount) {
            TextEncoding e = kEncodings[id - ID_ENC];
            if (e != enc_) {
                enc_ = e;
                SetModified(true); // Speichern erforderlich, damit die neue Kodierung wirksam wird
                UpdateTitle();
                UpdateStatus();
            }
        } else if (id >= ID_EOL && id < ID_EOL + kEolCount) {
            LineEnding e = kLineEndings[id - ID_EOL];
            if (e != eol_) {
                eol_ = e;
                SetModified(true);
                UpdateTitle();
                UpdateStatus();
            }
        }
        break;
    }
}

// ---------- Nachrichten ----------

LRESULT EditorWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_findMsg && g_findMsg != 0) return OnFindMessage(reinterpret_cast<FINDREPLACEW*>(lp));

    switch (msg) {
    case WM_CREATE: OnCreate(); return 0;

    case WM_SIZE: Layout(); return 0;

    case WM_SETFOCUS:
        if (edit_) SetFocus(edit_);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !HIWORD(wp)) PostMessageW(hwnd_, WM_APP_CHECKFILE, 0, 0);
        break;

    case WM_APP_CHECKFILE: CheckExternalChange(); return 0;

    case WM_APP_OPENDROPPED: {
        std::vector<std::wstring> files;
        files.swap(droppedFiles_);
        if (!files.empty()) {
            SetForegroundWindow(hwnd_);
            OpenOrReuse(files);
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = DpiScale(hwnd_, 300);
        mmi->ptMinTrackSize.y = DpiScale(hwnd_, 200);
        return 0;
    }

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateUIFont();
        ApplyFont();
        Layout();
        UpdateStatus();
        return 0;
    }

    case WM_INITMENUPOPUP: UpdateMenuState(); return 0;

    case WM_COMMAND:
        if (lp && (HWND)lp == edit_) {
            if (HIWORD(wp) == EN_CHANGE) {
                UpdateTitle();
                UpdateStatus();
            }
            return 0;
        }
        if (lp == 0) {
            OnCommand(LOWORD(wp));
            return 0;
        }
        break;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->hwndFrom == edit_) {
            if (nm->code == EN_SELCHANGE) {
                statusMsg_.clear();
                UpdateStatus();
                return 0;
            }
            if (nm->code == EN_DROPFILES) {
                auto* df = reinterpret_cast<ENDROPFILES*>(lp);
                HandleDrop((HDROP)df->hDrop);
                return 0; // nicht in den Text einfügen
            }
        }
        break;
    }

    case WM_CONTEXTMENU:
        if ((HWND)wp == edit_) {
            ShowContextMenu((short)LOWORD(lp), (short)HIWORD(lp));
            return 0;
        }
        break;

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        HandleDrop(drop);
        DragFinish(drop);
        return 0;
    }

    case WM_CLOSE:
        // Bei Änderungen nachfragen; bei Abbruch das Schließen verweigern (nicht an DefWindowProc weiterreichen)
        if (QueryClose()) DestroyWindow(hwnd_);
        return 0;

    case WM_QUERYENDSESSION: return QueryClose() ? TRUE : FALSE;

    case WM_DESTROY: OnDestroy(); return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// Unterklasse des RichEdit: Einfügemodus verfolgen, Formatierungs-Tastenkürzel unterdrücken,
// Einfügen nur als Klartext.
LRESULT CALLBACK EditSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<EditorWindow*>(ref);
    switch (msg) {
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        bool shift = GetKeyState(VK_SHIFT) < 0;
        bool alt = GetKeyState(VK_MENU) < 0; // Strg+Alt = AltGr: nicht abfangen
        if (wp == VK_INSERT && !ctrl && !shift && !alt) {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            self->overwrite_ = !self->overwrite_;
            self->UpdateStatus();
            return r;
        }
        if (ctrl && !alt) {
            // RichEdit-Formatierungskürzel würden im Klartextmodus den ganzen Text umformatieren
            switch (wp) {
            case 'B': case 'I': case 'U': case 'E': case 'J': case 'L': case 'R':
            case '1': case '2': case '5': case '0':
            case VK_OEM_PLUS: case VK_OEM_COMMA: case VK_OEM_PERIOD:
                return 0;
            case 'A':
                if (shift) return 0;
                break;
            default: break;
            }
        }
        break;
    }
    case WM_CHAR:
        // Strg+I erzeugt ein Tabulatorzeichen – unterdrücken (Tab-Taste selbst bleibt)
        if (wp == L'\t' && GetKeyState(VK_CONTROL) < 0) return 0;
        break;
    case WM_PASTE: self->PastePlain(); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(h, EditSubclassProc, id); break;
    default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

} // namespace

// ===================== Öffentliche Funktionen =====================

void OpenTextEditor(const std::wstring& pathIn) {
    if (!EnsureInitialized()) return;
    std::wstring path = FullPath(pathIn);
    if (!path.empty()) {
        if (EditorWindow* e = FindEditor(path)) {
            e->BringToFront();
            return;
        }
        if (DirExists(path)) {
            MsgError(App::MainWindow(), L"„" + path + L"“ ist ein Verzeichnis und kann nicht bearbeitet werden.");
            return;
        }
    }
    auto* ed = new EditorWindow();
    if (!ed->Create()) {
        DWORD err = GetLastError();
        // Wurde WM_NCCREATE nie erreicht, gehört das Objekt noch uns; sonst hat WM_NCDESTROY es gelöscht
        if (g_pendingCreate == ed) {
            g_pendingCreate = nullptr;
            delete ed;
        }
        MsgError(App::MainWindow(), L"Das Editorfenster kann nicht erzeugt werden.\n\n" + LastErrorMessage(err));
        return;
    }
    HWND h = ed->hwnd();
    if (!GetDlgItem(h, kIdEdit)) {
        MsgError(App::MainWindow(), L"Das Bearbeitungsfeld (RichEdit) kann nicht erzeugt werden.");
        DestroyWindow(h);
        return;
    }
    if (!ed->Open(path)) {
        DestroyWindow(h); // löscht ed (WM_NCDESTROY)
        return;
    }
    ed->Show();
}

bool CloseAllTextEditors() {
    std::vector<EditorWindow*> list = g_editors;
    for (EditorWindow* e : list) {
        if (!IsLiveEditor(e)) continue;
        if (!e->QueryClose()) return false;
        DestroyWindow(e->hwnd());
    }
    return true;
}

} // namespace qf
