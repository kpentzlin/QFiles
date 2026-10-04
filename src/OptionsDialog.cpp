// Modul E – Optionen-Dialog mit Registerkarten.
//
// Umsetzung: ein Tab-Control, die Steuerelemente aller Seiten liegen direkt im Dialog und werden je nach
// gewählter Registerkarte ein-/ausgeblendet. Die Seitenzugehörigkeit ergibt sich aus dem ID-Bereich:
// Seite n hat die IDs 1000 + n*100 … 1099 + n*100.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Encoding.h"
#include "Settings.h"
#include "Util.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <string>
#include <vector>

namespace qf {

namespace {

constexpr int kTab = 100;
constexpr int kCfgLabel = 150;
constexpr int kCfgPath = 151;
constexpr int kCfgOpen = 152;

constexpr int kPageCount = 5;
const wchar_t* const kPageNames[kPageCount] = {L"Anzeige", L"Bedienung", L"Dateianzeige", L"Editor", L"Programme"};

// ---- Seite 0: Anzeige ----
constexpr int kShowHidden = 1001;
constexpr int kShowSystem = 1002;
constexpr int kDirsFirst = 1003;
constexpr int kGridLines = 1004;
constexpr int kSizeBytes = 1005;
constexpr int kFullRow = 1006;
constexpr int kColType = 1007;
constexpr int kColAttr = 1008;
constexpr int kThumbLabel = 1009;
constexpr int kThumbEdit = 1010;
constexpr int kThumbSpin = 1011;
constexpr int kListFontText = 1013;
constexpr int kListFontBtn = 1014;
constexpr int kDateSeconds = 1015;
constexpr int kColCreated = 1016;
constexpr int kDisplayGroup1 = 1017;   // eigene IDs: 1015/1016 gehören den Kontrollkästchen
constexpr int kDisplayGroup2 = 1018;
// ---- Seite 1: Bedienung ----
constexpr int kConfirmDelete = 1101;
constexpr int kRecycle = 1102;
constexpr int kConfirmOverwrite = 1103;
constexpr int kSingleClickTree = 1104;
constexpr int kTreeFollows = 1105;
constexpr int kAutoRefresh = 1106;
constexpr int kEnterEditor = 1107;
constexpr int kSaveOnExit = 1108;
constexpr int kTextExtLabel = 1109;
constexpr int kTextExt = 1110;
constexpr int kHistoryLabel = 1111;
constexpr int kHistoryEdit = 1112;
constexpr int kHistorySpin = 1113;
// ---- Seite 2: Dateianzeige ----
constexpr int kQvGroup = 1201;
constexpr int kQvOther = 1202;
constexpr int kQvWindow = 1203;
constexpr int kQvHint = 1204;
// ---- Seite 3: Editor ----
constexpr int kEdFontLabel = 1301;
constexpr int kEdFontText = 1302;
constexpr int kEdFontBtn = 1303;
constexpr int kTabLabel = 1304;
constexpr int kTabEdit = 1305;
constexpr int kTabSpin = 1306;
constexpr int kWordWrap = 1307;
constexpr int kEncLabel = 1308;
constexpr int kEncCombo = 1309;
constexpr int kEolLabel = 1310;
constexpr int kEolCombo = 1311;
// ---- Seite 4: Programme ----
constexpr int kExtEditorLabel = 1401;
constexpr int kExtEditor = 1402;
constexpr int kExtEditorBtn = 1403;
constexpr int kExtViewerLabel = 1404;
constexpr int kExtViewer = 1405;
constexpr int kExtViewerBtn = 1406;
constexpr int kCompareLabel = 1407;
constexpr int kCompare = 1408;
constexpr int kCompareBtn = 1409;
constexpr int kProgHint = 1410;
constexpr int kExcludeLabel = 1411;
constexpr int kExclude = 1412;
constexpr int kExcludeHint = 1413;

int PageOfId(int id) {
    if (id < 1000 || id >= 1000 + kPageCount * 100) return -1;
    return (id - 1000) / 100;
}

std::wstring FontText(const std::wstring& name, int size) { return name + L", " + std::to_wstring(size) + L" pt"; }

class OptionsDlg : public DialogBase {
public:
    Options opt;

protected:
    BOOL OnInit() override {
        // Registerkarten
        HWND tab = Item(kTab);
        for (int i = 0; i < kPageCount; ++i) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<wchar_t*>(kPageNames[i]);
            TabCtrl_InsertItem(tab, i, &ti);
        }
        // Tab-Control nach hinten, damit es die Seitenelemente nicht überdeckt
        SetWindowPos(tab, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        // Anzeige
        SetCheck(kShowHidden, opt.showHidden);
        SetCheck(kShowSystem, opt.showSystem);
        SetCheck(kDirsFirst, opt.dirsFirst);
        SetCheck(kGridLines, opt.showGridLines);
        SetCheck(kSizeBytes, opt.sizeInBytes);
        SetCheck(kFullRow, opt.fullRowSelect);
        SetCheck(kColType, opt.showExtensionsColumn);
        SetCheck(kColAttr, opt.showAttributesColumn);
        SetCheck(kDateSeconds, opt.dateWithSeconds);
        SetCheck(kColCreated, opt.showCreatedColumn);
        InitSpin(kThumbSpin, kThumbEdit, 32, 256, opt.thumbnailSize);
        listFont_ = opt.listFontName;
        listSize_ = opt.listFontSize;
        SetText(kListFontText, FontText(listFont_, listSize_));
        // Bedienung
        SetCheck(kConfirmDelete, opt.confirmDelete);
        SetCheck(kRecycle, opt.useRecycleBin);
        SetCheck(kConfirmOverwrite, opt.confirmOverwrite);
        SetCheck(kSingleClickTree, opt.singleClickTree);
        SetCheck(kTreeFollows, opt.treeFollowsPane);
        SetCheck(kAutoRefresh, opt.autoRefresh);
        SetCheck(kEnterEditor, opt.enterOpensEditorForText);
        SetCheck(kSaveOnExit, opt.saveOnExit);
        SetText(kTextExt, opt.textExtensions);
        InitSpin(kHistorySpin, kHistoryEdit, 5, 500, opt.historySize);
        // Dateianzeige
        CheckRadioButton(hwnd_, kQvOther, kQvWindow,
                         opt.quickViewTarget == QuickViewTarget::Window ? kQvWindow : kQvOther);
        // Editor
        edFont_ = opt.editorFontName;
        edSize_ = opt.editorFontSize;
        SetText(kEdFontText, FontText(edFont_, edSize_));
        InitSpin(kTabSpin, kTabEdit, 1, 16, opt.editorTabWidth);
        SetCheck(kWordWrap, opt.editorWordWrap);
        for (int e = 0; e <= (int)TextEncoding::Oem; ++e) {
            std::wstring name = EncodingName((TextEncoding)e);
            if ((TextEncoding)e == TextEncoding::Utf8) name = L"UTF-8 (ohne BOM) – Standard";
            ComboAdd(kEncCombo, name, e);
        }
        ComboSetSel(kEncCombo, (int)opt.newFileEncoding);
        for (int e = 0; e <= (int)LineEnding::CR; ++e) ComboAdd(kEolCombo, LineEndingName((LineEnding)e), e);
        ComboSetSel(kEolCombo, (int)opt.newFileLineEnding);
        // Programme
        SetText(kExtEditor, opt.externalEditor);
        SetText(kExtViewer, opt.externalViewer);
        SetText(kCompare, opt.compareTool);
        SetText(kExclude, opt.shellExtExclude);
        // Einstellungsdatei
        SetText(kCfgPath, App::Cfg().FilePath());

        ShowPage(0);
        return TRUE;
    }

    BOOL OnCommand(int id, int code, HWND ctl) override {
        switch (id) {
        case kListFontBtn:
            if (ChooseFontFor(listFont_, listSize_)) SetText(kListFontText, FontText(listFont_, listSize_));
            return TRUE;
        case kEdFontBtn:
            if (ChooseFontFor(edFont_, edSize_)) SetText(kEdFontText, FontText(edFont_, edSize_));
            return TRUE;
        case kExtEditorBtn: BrowseProgram(kExtEditor, L"Externen Editor auswählen"); return TRUE;
        case kExtViewerBtn: BrowseProgram(kExtViewer, L"Externe Anzeige auswählen"); return TRUE;
        case kCompareBtn: BrowseProgram(kCompare, L"Vergleichsprogramm auswählen"); return TRUE;
        case kCfgOpen: {
            std::wstring dir = PathParent(App::Cfg().FilePath());
            if (!dir.empty()) ShellOpen(hwnd_, dir);
            return TRUE;
        }
        case IDOK:
            Store();
            End(IDOK);
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }

    INT_PTR OnNotify(NMHDR* nm) override {
        if (nm->idFrom == kTab && nm->code == TCN_SELCHANGE) {
            ShowPage(TabCtrl_GetCurSel(Item(kTab)));
            return 0;
        }
        return 0;
    }

private:
    void InitSpin(int spin, int edit, int lo, int hi, int value) {
        HWND s = Item(spin);
        SendMessageW(s, UDM_SETBUDDY, (WPARAM)Item(edit), 0);
        SendMessageW(s, UDM_SETRANGE32, lo, hi);
        SendMessageW(s, UDM_SETPOS32, 0, std::clamp(value, lo, hi));
    }
    int SpinValue(int edit, int lo, int hi, int def) {
        long long v = GetInt(edit, def);
        return (int)std::clamp<long long>(v, lo, hi);
    }

    // Zugriffstasten (&x) gelten nur auf der sichtbaren Seite: Windows berücksichtigt bei Alt+Taste auch
    // ausgeblendete Steuerelemente und schaltet bei mehrfach vergebenen Buchstaben ein Kontrollkästchen nicht um,
    // sondern setzt nur den Fokus. Daher tragen Beschriftungen ausgeblendeter Seiten ihr '&' nicht.
    static std::wstring StripMnemonic(const std::wstring& t) {
        std::wstring r;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == L'&') {
                if (i + 1 < t.size() && t[i + 1] == L'&') r += L"&&", ++i;
                continue;
            }
            r += t[i];
        }
        return r;
    }

    void ShowPage(int page) {
        if (page < 0 || page >= kPageCount) page = 0;
        TabCtrl_SetCurSel(Item(kTab), page);
        if (!mnemonicsCollected_) {
            mnemonicsCollected_ = true;
            for (HWND c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                if (PageOfId(GetDlgCtrlID(c)) < 0) continue;
                wchar_t cls[32] = {};
                GetClassNameW(c, cls, 31);
                if (_wcsicmp(cls, L"Button") != 0 && _wcsicmp(cls, L"Static") != 0) continue;
                int len = GetWindowTextLengthW(c);
                std::wstring text(len + 1, L'\0');
                text.resize(GetWindowTextW(c, text.data(), len + 1));
                if (text.find(L'&') != std::wstring::npos) mnemonicTexts_.push_back({c, text});
            }
        }
        for (HWND c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            int p = PageOfId(GetDlgCtrlID(c));
            if (p < 0) continue;
            ShowWindow(c, p == page ? SW_SHOW : SW_HIDE);
        }
        for (auto& [c, text] : mnemonicTexts_)
            SetWindowTextW(c, PageOfId(GetDlgCtrlID(c)) == page ? text.c_str() : StripMnemonic(text).c_str());
    }

    bool ChooseFontFor(std::wstring& name, int& size) {
        LOGFONTW lf{};
        HDC dc = GetDC(hwnd_);
        lf.lfHeight = -MulDiv(size, GetDeviceCaps(dc, LOGPIXELSY), 72);
        ReleaseDC(hwnd_, dc);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        for (size_t i = 0; i < name.size() && i + 1 < LF_FACESIZE; ++i) lf.lfFaceName[i] = name[i];
        CHOOSEFONTW cf{};
        cf.lStructSize = sizeof(cf);
        cf.hwndOwner = hwnd_;
        cf.lpLogFont = &lf;
        cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_NOSCRIPTSEL;
        if (!ChooseFontW(&cf)) return false;
        name = lf.lfFaceName;
        size = std::max(1, (cf.iPointSize + 5) / 10);
        return true;
    }

    void BrowseProgram(int edit, const std::wstring& title) {
        std::wstring cur = GetText(edit);
        std::wstring f = OpenFileDialog(hwnd_, title, cur.empty() ? L"" : PathParent(cur),
                                        L"Programme|*.exe;*.com;*.bat;*.cmd|Alle Dateien|*.*");
        if (!f.empty()) SetText(edit, f);
    }

    void Store() {
        opt.showHidden = IsChecked(kShowHidden);
        opt.showSystem = IsChecked(kShowSystem);
        opt.dirsFirst = IsChecked(kDirsFirst);
        opt.showGridLines = IsChecked(kGridLines);
        opt.sizeInBytes = IsChecked(kSizeBytes);
        opt.fullRowSelect = IsChecked(kFullRow);
        opt.showExtensionsColumn = IsChecked(kColType);
        opt.showAttributesColumn = IsChecked(kColAttr);
        opt.dateWithSeconds = IsChecked(kDateSeconds);
        opt.showCreatedColumn = IsChecked(kColCreated);
        opt.thumbnailSize = SpinValue(kThumbEdit, 32, 256, opt.thumbnailSize);
        opt.listFontName = listFont_;
        opt.listFontSize = listSize_;

        opt.confirmDelete = IsChecked(kConfirmDelete);
        opt.useRecycleBin = IsChecked(kRecycle);
        opt.confirmOverwrite = IsChecked(kConfirmOverwrite);
        opt.singleClickTree = IsChecked(kSingleClickTree);
        opt.treeFollowsPane = IsChecked(kTreeFollows);
        opt.autoRefresh = IsChecked(kAutoRefresh);
        opt.enterOpensEditorForText = IsChecked(kEnterEditor);
        opt.saveOnExit = IsChecked(kSaveOnExit);
        std::wstring ext = GetText(kTextExt);
        ext.erase(std::remove_if(ext.begin(), ext.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }), ext.end());
        ext = Trim(ext);
        if (!ext.empty()) opt.textExtensions = ext;
        opt.historySize = SpinValue(kHistoryEdit, 5, 500, opt.historySize);

        opt.quickViewTarget = IsChecked(kQvWindow) ? QuickViewTarget::Window : QuickViewTarget::OtherPane;

        opt.editorFontName = edFont_;
        opt.editorFontSize = edSize_;
        opt.editorTabWidth = SpinValue(kTabEdit, 1, 16, opt.editorTabWidth);
        opt.editorWordWrap = IsChecked(kWordWrap);
        int enc = ComboSel(kEncCombo);
        if (enc >= 0) opt.newFileEncoding = (TextEncoding)ComboData(kEncCombo, enc);
        int eol = ComboSel(kEolCombo);
        if (eol >= 0) opt.newFileLineEnding = (LineEnding)ComboData(kEolCombo, eol);

        opt.externalEditor = Trim(GetText(kExtEditor));
        opt.externalViewer = Trim(GetText(kExtViewer));
        opt.compareTool = Trim(GetText(kCompare));
        opt.shellExtExclude = Trim(GetText(kExclude));
    }

    bool mnemonicsCollected_ = false;
    std::vector<std::pair<HWND, std::wstring>> mnemonicTexts_;   // Steuerelemente mit Zugriffstaste
    std::wstring listFont_, edFont_;
    int listSize_ = 9, edSize_ = 10;
};

} // namespace

bool ShowOptionsDialog(HWND owner) {
    // Abmessungen in DLU
    const int W = 340, H = 268;
    const int px = 18, pw = W - 2 * px;   // Inhaltsbereich der Seiten
    const int py = 28;
    DialogTemplate t(L"Optionen", W, H);
    t.Tab(kTab, 7, 7, W - 14, 206);

    // ---- Anzeige ----
    t.Group(kDisplayGroup1, L"Dateilisten", px - 4, py, pw + 8, 104);
    t.Check(kShowHidden, L"&Versteckte Dateien anzeigen", px + 4, py + 12, 150, 10);
    t.Check(kShowSystem, L"&Systemdateien anzeigen", px + 4, py + 25, 150, 10);
    t.Check(kDirsFirst, L"Verzeichnisse &zuerst", px + 4, py + 38, 150, 10);
    t.Check(kGridLines, L"&Gitternetzlinien", px + 4, py + 51, 150, 10);
    t.Check(kDateSeconds, L"Dateidatum se&kundengenau", px + 4, py + 64, 150, 10);
    t.Check(kSizeBytes, L"Größe immer in &Bytes", px + 160, py + 12, 140, 10);
    t.Check(kFullRow, L"Ganze Zei&le markieren", px + 160, py + 25, 140, 10);
    t.Check(kColType, L"Spalte „&Typ“ anzeigen", px + 160, py + 38, 140, 10);
    t.Check(kColAttr, L"Spalte „&Attribute“ anzeigen", px + 160, py + 51, 140, 10);
    t.Check(kColCreated, L"Spalte „&Erstellt“ anzeigen", px + 160, py + 64, 150, 10);
    t.Label(kThumbLabel, L"&Miniaturgröße (32–256 Pixel):", px + 4, py + 86, 110, 10);
    t.Edit(kThumbEdit, px + 118, py + 84, 40, 13, ES_NUMBER);
    t.UpDown(kThumbSpin, 0, 0, 10, 13);
    t.Group(kDisplayGroup2, L"Schrift der Dateilisten", px - 4, py + 112, pw + 8, 34);
    t.Edit(kListFontText, px + 4, py + 126, 190, 13, ES_AUTOHSCROLL | ES_READONLY);
    t.Button(kListFontBtn, L"Sc&hriftart …", px + 200, py + 125, 70, 14);

    // ---- Bedienung ----
    t.Check(kConfirmDelete, L"&Löschen bestätigen", px, py, 150, 10);
    t.Check(kRecycle, L"&Papierkorb verwenden", px, py + 13, 150, 10);
    t.Check(kConfirmOverwrite, L"&Überschreiben bestätigen", px, py + 26, 150, 10);
    t.Check(kSaveOnExit, L"Einstellungen beim &Beenden speichern", px, py + 39, 150, 10);
    t.Check(kSingleClickTree, L"&Einfachklick im Baum öffnet Verzeichnis", px + 155, py, 150, 10);
    t.Check(kTreeFollows, L"Baum &folgt der aktiven Liste", px + 155, py + 13, 150, 10);
    t.Check(kAutoRefresh, L"Verzeichnisse automatisch &aktualisieren", px + 155, py + 26, 150, 10);
    t.Check(kEnterEditor, L"Eingabetaste öffnet &Textdateien im Editor", px + 155, py + 39, 150, 10);
    t.Label(kTextExtLabel, L"Textdateien (&Muster, durch ; getrennt):", px, py + 60, pw, 10);
    t.MultiEdit(kTextExt, px, py + 72, pw, 52, 0);
    t.Label(kHistoryLabel, L"Länge des &Verlaufs (Einträge):", px, py + 135, 120, 10);
    t.Edit(kHistoryEdit, px + 124, py + 133, 40, 13, ES_NUMBER);
    t.UpDown(kHistorySpin, 0, 0, 10, 13);

    // ---- Dateianzeige ----
    t.Group(kQvGroup, L"Dateianzeige (Schnellansicht)", px - 4, py, pw + 8, 50);
    t.Radio(kQvOther, L"in dem jeweils &anderen Fenster (Standard)", px + 4, py + 14, 250, 10, true);
    t.Radio(kQvWindow, L"in &eigenem Fenster", px + 4, py + 29, 250, 10);
    t.Label(kQvHint,
            L"Bei „in dem jeweils anderen Fenster“ zeigt die gegenüberliegende Dateiliste den Inhalt der Datei mit "
            L"dem Fokus an (Text, Bild, Hex oder Vorschauhandler).",
            px, py + 58, pw, 30);

    // ---- Editor ----
    t.Label(kEdFontLabel, L"&Schrift:", px, py + 2, 80, 10);
    t.Edit(kEdFontText, px + 84, py, 140, 13, ES_AUTOHSCROLL | ES_READONLY);
    t.Button(kEdFontBtn, L"Schrift&art …", px + 230, py - 1, 70, 14);
    t.Label(kTabLabel, L"&Tabulatorweite:", px, py + 22, 80, 10);
    t.Edit(kTabEdit, px + 84, py + 20, 36, 13, ES_NUMBER);
    t.UpDown(kTabSpin, 0, 0, 10, 13);
    t.Check(kWordWrap, L"&Zeilenumbruch", px, py + 42, 150, 10);
    t.Label(kEncLabel, L"&Kodierung neuer/leerer Dateien:", px, py + 62, 120, 10);
    t.Combo(kEncCombo, px + 124, py + 60, 150, 100);
    t.Label(kEolLabel, L"Zeilen&ende neuer Dateien:", px, py + 80, 120, 10);
    t.Combo(kEolCombo, px + 124, py + 78, 150, 80);

    // ---- Programme ----
    t.Label(kExtEditorLabel, L"Externer &Editor (leer = integrierter Editor):", px, py, pw, 10);
    t.Edit(kExtEditor, px, py + 12, pw - 22, 13);
    t.Button(kExtEditorBtn, L"…", px + pw - 18, py + 12, 18, 13);
    t.Label(kExtViewerLabel, L"Externe &Anzeige (leer = integrierte Anzeige):", px, py + 32, pw, 10);
    t.Edit(kExtViewer, px, py + 44, pw - 22, 13);
    t.Button(kExtViewerBtn, L"…", px + pw - 18, py + 44, 18, 13);
    t.Label(kCompareLabel, L"Externes &Vergleichsprogramm (leer = integrierter Vergleich):", px, py + 64, pw, 10);
    t.Edit(kCompare, px, py + 76, pw - 22, 13);
    t.Button(kCompareBtn, L"…", px + pw - 18, py + 76, 18, 13);
    t.Label(kProgHint,
            L"Ist kein Programm eingetragen, verwendet QFiles den integrierten Editor, die integrierte Anzeige "
            L"bzw. den integrierten Dateivergleich.",
            px, py + 98, pw, 20);
    t.Label(kExcludeLabel, L"Kontextmenü-Erweiterungen &nicht laden (Muster, durch ; getrennt):", px, py + 128, pw, 10);
    t.Edit(kExclude, px, py + 140, pw, 13, ES_AUTOHSCROLL);
    t.Label(kExcludeHint,
            L"Verglichen mit Name, Beschreibung und DLL-Pfad der Erweiterung, z. B. „ArchiCrypt“. Die Einträge "
            L"solcher Erweiterungen fehlen dann im Kontextmenü von QFiles (der Explorer bleibt unverändert).",
            px, py + 158, pw, 20);

    // ---- Unten: Einstellungsdatei ----
    t.Label(kCfgLabel, L"Einstellungsdatei:", 7, 220, 70, 10);
    t.Edit(kCfgPath, 78, 218, W - 78 - 72, 13, ES_AUTOHSCROLL | ES_READONLY);
    t.Button(kCfgOpen, L"&Ordner öffnen", W - 67, 217, 60, 14);
    t.DefButton(IDOK, L"OK", W - 121, H - 21, 54, 14);
    t.Button(IDCANCEL, L"Abbrechen", W - 61, H - 21, 54, 14);

    OptionsDlg dlg;
    dlg.opt = App::Opt();
    if (dlg.DoModal(owner, t) != IDOK) return false;
    App::Opt() = dlg.opt;
    App::Opt().Save(App::Cfg());
    if (!App::Cfg().Save())
        MsgError(owner, L"Die Einstellungen konnten nicht gespeichert werden:\n" + App::Cfg().FilePath());
    return true;
}

} // namespace qf
