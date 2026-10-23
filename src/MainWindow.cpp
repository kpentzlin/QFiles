#include "MainWindow.h"
#include "ArchiveCore.h"
#include "App.h"
#include "Cloud.h"
#include "Commands.h"
#include "Dialog.h"
#include "Favorites.h"
#include "RecycleBin.h"
#include "FileOps.h"
#include "Glyphs.h"
#include "HexView.h"
#include "Location.h"
#include "Modules.h"
#include "Remote.h"
#include "ShellMenu.h"
#include "TextEditor.h"
#include "Util.h"
#include "resource.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <windowsx.h>
#include <algorithm>
#include <thread>
#include <functional>

namespace qf {

namespace {

const wchar_t* kMainClass = L"QFilesMain";
MainWindow* g_main = nullptr;

enum : UINT_PTR { TIMER_FKEYMODE = 1, TIMER_QUICKVIEW = 2, TIMER_REMOTEEDIT = 3, TIMER_SAVESTATE = 4, TIMER_CLIPBOARD = 5 };
constexpr int kSplitterW = 5;
constexpr int kCaptionH = 26;  // = Kopfzeile der Listen
constexpr int kFKeyH = 26;
constexpr int kCmdH = 28;

struct SpecialFolder {
    const wchar_t* name;
    const GUID* id;
};
const SpecialFolder kSpecial[] = {
    {L"&Desktop", &FOLDERID_Desktop},
    {L"D&okumente", &FOLDERID_Documents},
    {L"Do&wnloads", &FOLDERID_Downloads},
    {L"&Bilder", &FOLDERID_Pictures},
    {L"&Musik", &FOLDERID_Music},
    {L"&Videos", &FOLDERID_Videos},
    {L"Ben&utzerprofil", &FOLDERID_Profile},
    {L"&AppData (Roaming)", &FOLDERID_RoamingAppData},
    {L"AppData (&Lokal)", &FOLDERID_LocalAppData},
    {L"&ProgramData", &FOLDERID_ProgramData},
    {L"P&rogramme", &FOLDERID_ProgramFiles},
    {L"Programme (&x86)", &FOLDERID_ProgramFilesX86},
    {L"&Startmenü", &FOLDERID_StartMenu},
    {L"Auto&start", &FOLDERID_Startup},
    {L"&Favoriten", &FOLDERID_Favorites},
    {L"&Senden an", &FOLDERID_SendTo},
    {L"&Windows", &FOLDERID_Windows},
    {L"S&ystem32", &FOLDERID_System},
    {L"&Temp", nullptr},
};
constexpr int kSpecialCount = (int)(sizeof(kSpecial) / sizeof(kSpecial[0]));

// Im Papierkorb nicht verfügbare Befehle (die Elemente heißen dort $R…; Wiederherstellen über eigene Befehle)
const std::vector<int>& RecycleBinBlocked() {
    static const std::vector<int> ids = {
        cmd::NewFolder, cmd::NewFile, cmd::NewTextFile, cmd::ClipPaste, cmd::ClipCut, cmd::ClipCopy, cmd::FindFiles,
        cmd::FindDuplicates, cmd::CreateZip, cmd::GoRoot, cmd::Rename, cmd::BatchRename, cmd::Edit, cmd::Attributes,
        cmd::Duplicate, cmd::SplitFile, cmd::JoinFiles, cmd::Wipe, cmd::FavoriteAdd, cmd::CompareDirs, cmd::SyncDirs,
        cmd::CompareFiles, cmd::OpenArchive, cmd::Undo};
    return ids;
}

bool IsArchive(const std::wstring& name) {
    return MatchAnyPattern(L"*.zip;*.7z;*.rar;*.tar;*.tgz;*.tar.gz;*.gz;*.bz2;*.tbz2;*.xz;*.txz;*.cab;*.iso;*.lzh;*.lha;*.arj",
                           name);
}

std::wstring Quote(const std::wstring& s) {
    if (s.find_first_of(L" \t&()[]{}^=;!'+,`~") == std::wstring::npos) return s;
    return L"\"" + s + L"\"";
}

void AddItem(HMENU m, int id, const wchar_t* text) { AppendMenuW(m, MF_STRING, id, text); }
void AddSep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }

// ===================== Kleine Dialoge (Umbenennen, Duplizieren, Auswahl) =====================

// Name eingeben; bei Dateien wird der Name ohne Erweiterung vorausgewählt.
// numbered: optionaler Knopf "Nummeriert", der einen nummerierten Namen einsetzt.
class NameDlg : public DialogBase {
public:
    std::wstring prompt, value;
    bool selectStem = true;
    std::function<std::wstring()> numbered;  // leer = kein Knopf
    BOOL OnInit() override {
        SetText(101, prompt);
        SetText(102, value);
        SelectStem();
        SetFocus(Item(102));
        return FALSE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == 103 && numbered) {
            SetText(102, numbered());
            SelectStem();
            SetFocus(Item(102));
            return TRUE;
        }
        if (id == IDOK) value = Trim(GetText(102));
        return DialogBase::OnCommand(id, code, ctl);
    }

private:
    void SelectStem() {
        std::wstring t = GetText(102);
        size_t dot = t.find_last_of(L'.');
        if (selectStem && dot != std::wstring::npos && dot > 0) SendMessageW(Item(102), EM_SETSEL, 0, dot);
        else SendMessageW(Item(102), EM_SETSEL, 0, -1);
    }
};

bool AskName(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value, bool selectStem,
             std::function<std::wstring()> numbered = nullptr) {
    DialogTemplate t(title, 280, 66);
    t.Label(101, L"", 7, 7, 266, 18);
    t.Edit(102, 7, 26, 266, 14, ES_AUTOHSCROLL);
    int x = 116;
    if (numbered) t.Button(103, L"&Nummeriert", 7, 46, 60, 14);
    t.DefButton(IDOK, L"OK", x + 52, 46, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", x + 107, 46, 50, 14);
    NameDlg dlg;
    dlg.prompt = prompt;
    dlg.value = value;
    dlg.selectStem = selectStem;
    dlg.numbered = std::move(numbered);
    if (dlg.DoModal(owner, t) != IDOK) return false;
    value = dlg.value;
    return !value.empty();
}

// Auswahl aus mehreren Möglichkeiten (Optionsfelder). Rückgabe: Index oder -1.
class ChoiceDlg : public DialogBase {
public:
    std::wstring prompt;
    std::vector<std::wstring> options;
    int choice = 0;
    BOOL OnInit() override {
        SetText(101, prompt);
        for (size_t i = 0; i < options.size(); ++i) SetText(200 + (int)i, options[i]);
        SetCheck(200, true);
        return TRUE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == IDOK)
            for (size_t i = 0; i < options.size(); ++i)
                if (IsChecked(200 + (int)i)) choice = (int)i;
        return DialogBase::OnCommand(id, code, ctl);
    }
};

// Namen ("Unterverzeichnis\\Datei") nach Unterverzeichnis gruppieren (Ansicht „Mit Unterverzeichnissen“)
std::vector<std::pair<std::wstring, std::vector<std::wstring>>> GroupBySubdir(const std::vector<std::wstring>& names) {
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> r;
    for (auto& n : names) {
        size_t pos = n.find_last_of(L'\\');
        std::wstring sub = pos == std::wstring::npos ? L"" : n.substr(0, pos);
        std::wstring base = pos == std::wstring::npos ? n : n.substr(pos + 1);
        auto it = std::find_if(r.begin(), r.end(), [&](const auto& g) { return EqualsI(g.first, sub); });
        if (it == r.end()) r.push_back({sub, {base}});
        else it->second.push_back(base);
    }
    return r;
}

// Dateigruppe umbenennen (je Unterverzeichnis ein Dialog). Markierung und Fokus werden auf die neuen Namen
// übertragen, damit die umbenannten Dateien markiert bleiben.
bool RenameGroupKeepSelection(HWND owner, FilePane& a) {
    auto names = a.SelectedOrFocusedNames();
    if (names.empty()) return false;
    auto selected = a.SelectedNames();
    std::wstring focused = a.FocusedName();
    std::vector<std::pair<std::wstring, std::wstring>> map;   // alter -> neuer relativer Name
    bool any = false;
    for (auto& [sub, group] : GroupBySubdir(names)) {
        std::vector<std::wstring> renamed;
        if (BatchRename(owner, PathCombine(a.Dir(), sub), group, &renamed)) any = true;
        std::wstring dir = PathCombine(a.Dir(), sub);
        for (size_t k = 0; k < group.size() && k < renamed.size(); ++k)
            if (group[k] != renamed[k]) {
                map.push_back({sub.empty() ? group[k] : sub + L"\\" + group[k],
                               sub.empty() ? renamed[k] : sub + L"\\" + renamed[k]});
                if (!a.IsVirtual()) RenameFavorite(PathCombine(dir, group[k]), PathCombine(dir, renamed[k]));
            }
    }
    if (!any) return false;
    auto mapName = [&](const std::wstring& n) {
        for (auto& [o, nn] : map)
            if (EqualsI(o, n)) return nn;
        return n;
    };
    a.Reload();
    std::wstring newFocus = focused.empty() ? L"" : mapName(focused);
    if (!selected.empty()) {
        for (auto& n : selected) n = mapName(n);
        a.SelectNames(selected, false);
        if (!newFocus.empty()) a.SetFocusName(newFocus);
    } else if (!newFocus.empty()) {
        a.FocusName(newFocus);
    }
    return true;
}

int AskChoice(HWND owner, const std::wstring& title, const std::wstring& prompt, const std::vector<std::wstring>& options) {
    int h = 48 + (int)options.size() * 14;
    DialogTemplate t(title, 300, h);
    t.Label(101, L"", 7, 7, 286, 10);
    for (size_t i = 0; i < options.size(); ++i)
        t.Radio(200 + (int)i, L"", 14, 22 + (int)i * 14, 279, 10, i == 0);
    t.DefButton(IDOK, L"OK", 186, h - 20, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 243, h - 20, 50, 14);
    ChoiceDlg dlg;
    dlg.prompt = prompt;
    dlg.options = options;
    if (dlg.DoModal(owner, t) != IDOK) return -1;
    return dlg.choice;
}


const wchar_t* PanePositionName(int i, bool twoPanes, bool splitCol) {
    static const wchar_t* withSplit[4] = {L"links oben", L"rechts oben", L"links unten", L"rechts unten"};
    if (!splitCol) return twoPanes ? (i % 2 == 0 ? L"links" : L"rechts") : L"";
    return withSplit[i];
}


} // namespace

// ===================== App-Dienste (Implementierung) =====================

void App::NavigateTo(const std::wstring& dir, const std::wstring& selectName) {
    if (g_main) g_main->NavigateActive(dir, selectName);
}
void App::NavigateToFile(const std::wstring& fullPath) {
    if (g_main) g_main->NavigateToFile(fullPath);
}
void App::MarkSearchResults(const std::vector<std::wstring>& dirs, const std::vector<std::wstring>& paths, bool invert,
                            bool subdirs) {
    if (g_main) g_main->MarkSearchResults(dirs, paths, invert, subdirs);
}
void App::RefreshPanes() {
    if (g_main) g_main->RefreshAll();
}
void App::UpdateViewButtons() {
    // nachgestellt: beim Fokuswechsel steht der neue Fokus erst danach fest
    if (g_main && g_main->Hwnd()) PostMessageW(g_main->Hwnd(), WM_APP + 2, 0, 0);
}
PaneContext App::ActivePaneContext() {
    if (g_main) return g_main->ActiveContext();
    return {};
}
void App::SaveSettings() {
    if (g_main) g_main->SaveState();
    Opt().Save(Cfg());
    if (!Cfg().Save()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            MsgError(MainWindow(), L"Die Einstellungen konnten nicht gespeichert werden:\n" + Cfg().FilePath() + L"\n\n" +
                                       LastErrorMessage());
        }
    }
}

bool MainWindowPreTranslate(MSG* msg) { return g_main && g_main->PreTranslate(msg); }

// ===================== Erzeugen =====================

bool MainWindow::Create(int nCmdShow) {
    g_main = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = App::Instance();
    wc.hIcon = App::BigIcon();
    wc.hIconSm = App::SmallIcon();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    const Config& c = App::Cfg();
    int x = c.GetInt(L"Fenster", L"X", CW_USEDEFAULT);
    int y = c.GetInt(L"Fenster", L"Y", CW_USEDEFAULT);
    int w = c.GetInt(L"Fenster", L"B", 1280);
    int h = c.GetInt(L"Fenster", L"H", 800);
    // Fenster muss auf einem Monitor liegen und in dessen Arbeitsbereich passen
    {
        RECT r{x == CW_USEDEFAULT ? 0 : x, y == CW_USEDEFAULT ? 0 : y, 0, 0};
        r.right = r.left + w;
        r.bottom = r.top + h;
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (!mon) {
            x = y = CW_USEDEFAULT;
            mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        }
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(mon, &mi)) {
            const RECT& wa = mi.rcWork;
            int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
            if (!c.Has(L"Fenster", L"B")) {
                w = std::min(w, waW * 92 / 100);
                h = std::min(h, waH * 92 / 100);
                x = wa.left + (waW - w) / 2;
                y = wa.top + (waH - h) / 2;
            } else {
                w = std::min(w, waW);
                h = std::min(h, waH);
                if (x != CW_USEDEFAULT) {
                    x = std::clamp(x, (int)wa.left, (int)wa.right - w);
                    y = std::clamp(y, (int)wa.top, (int)wa.bottom - h);
                }
            }
        }
    }
    hwnd_ = CreateWindowExW(0, kMainClass, L"QFiles", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr,
                            App::Instance(), this);
    if (!hwnd_) return false;
    App::SetMainWindow(hwnd_);

    BuildMenu();
    BuildToolbar();
    BuildAccelerators();
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd_,
                              (HMENU)(INT_PTR)cmd::IdStatus, App::Instance(), nullptr);

    tree_.Create(
        hwnd_, cmd::IdTree,
        [this](const std::wstring& p) {
            Active().Navigate(p);
        },
        [this]() {});
    BookmarkList::Callbacks cb;
    cb.navigate = [this](const std::wstring& p, bool other) {
        FilePane* target = &Active();
        if (other) {
            // Die Gegenseite zeigt evtl. die Schnellansicht: dann nicht die verdeckte Liste umschalten
            if (quickView_) SetQuickView(false);
            if (FilePane* o = Other()) target = o;
        }
        if (target->Navigate(p)) {
            SetActive(target->Index(), true);
        }
    };
    cb.changed = [this]() {
        SaveBookmarks(App::Cfg(), bookmarks_.Get());
        App::Cfg().Save();
    };
    cb.activated = []() {};
    cb.currentDir = [this]() { return Active().Dir(); };
    bookmarks_.Create(hwnd_, cmd::IdBookmarks, cb);
    bookmarks_.Set(LoadBookmarks(App::Cfg()));
    LoadFavorites(App::Cfg());
    bookmarks_.SetFavoritesCount(Favorites().size());

    // Befehlszeile
    cmdLabel_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_PATHELLIPSIS | SS_CENTERIMAGE, 0, 0,
                                0, 0, hwnd_, nullptr, App::Instance(), nullptr);
    SendMessageW(cmdLabel_, WM_SETFONT, (WPARAM)App::UIFont(), TRUE);
    cmdCombo_ = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                                0, 0, 100, 300, hwnd_, (HMENU)(INT_PTR)cmd::IdCommandLine, App::Instance(), nullptr);
    SendMessageW(cmdCombo_, WM_SETFONT, (WPARAM)App::UIFont(), TRUE);
    COMBOBOXINFO cbi{sizeof(cbi)};
    GetComboBoxInfo(cmdCombo_, &cbi);
    cmdEdit_ = cbi.hwndItem;
    if (cmdEdit_) {
        SetWindowSubclass(cmdEdit_, CmdEditSubclass, 1, (DWORD_PTR)this);
        SendMessageW(cmdEdit_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Befehlszeile (Strg+E) – Eingabe führt den Befehl im aktuellen Verzeichnis aus");
        DragAcceptFiles(cmdEdit_, TRUE);
    }
    cmdHistory_ = c.GetList(L"Befehlszeile", L"Befehl");
    for (auto& s : cmdHistory_) SendMessageW(cmdCombo_, CB_ADDSTRING, 0, (LPARAM)s.c_str());

    for (int i = 0; i < 4; ++i) {
        panes_[i] = std::make_unique<FilePane>();
        panes_[i]->Create(hwnd_, i, this);
    }
    LoadState();
    fkeyDefs_ = LoadFunctionKeys(App::Cfg());
    BuildFKeyCells();

    // Verzeichnisse aus der Befehlszeile: QFiles.exe [Verzeichnis1] [Verzeichnis2]
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> dirs;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (StartsWithI(a, L"/ini=") || StartsWithI(a, L"-ini=")) continue;
        dirs.push_back(a);
    }
    if (argv) LocalFree(argv);

    // Startverzeichnisse
    std::wstring defaults[4] = {L"C:\\", GetKnownFolder(FOLDERID_Documents), GetKnownFolder(FOLDERID_Profile),
                                GetKnownFolder(FOLDERID_Downloads)};
    for (int i = 0; i < 4; ++i) {
        std::wstring d = c.Get(L"Liste" + std::to_wstring(i + 1), L"Verzeichnis");
        if (i < 2 && i < (int)dirs.size()) d = dirs[i];
        // FTP/SFTP beim Start nur mit gespeicherter Anmeldung (keine Kennwortabfrage beim Programmstart)
        if (IsRemoteUrl(d) && !HasSavedRemoteLogin(d)) d.clear();
        if (d.empty() || !panes_[i]->Navigate(d, L"", true, false)) {
            if (!panes_[i]->Navigate(defaults[i], L"", true, false)) panes_[i]->Navigate(L"C:\\", L"", true, false);
        }
    }

    // Privater Modus beim Start: der gespeicherte Verlauf bleibt bis zum Ausschalten unverändert
    if (App::Opt().privateMode) {
        for (auto& p : panes_) p->BeginPrivate();
        cmdHistorySnap_ = cmdHistory_;
        privActive_ = true;
    }

    ApplyPaneVisibility();
    WINDOWPLACEMENT wp{sizeof(wp)};
    bool maximized = c.GetBool(L"Fenster", L"Max", false);
    ShowWindow(hwnd_, maximized ? SW_SHOWMAXIMIZED : nCmdShow);
    (void)wp;
    UpdateWindow(hwnd_);
    SetActive(active_, true);
    SetTimer(hwnd_, TIMER_FKEYMODE, 150, nullptr);
    SetTimer(hwnd_, TIMER_REMOTEEDIT, 2000, nullptr);
    // Alte temporäre Kopien aus Archiven (älter als ein Tag) im Hintergrund entfernen
    std::thread([] { CleanupArchiveTemp(); }).detach();
    // Ausgeschnittene Dateien grau darstellen: Änderungen der Zwischenablage beobachten
    AddClipboardFormatListener(hwnd_);
    OnClipboardChanged();
    return true;
}

void MainWindow::LoadState() {
    const Config& c = App::Cfg();
    const wchar_t* S = L"Fenster";
    twoPanes_ = c.GetBool(S, L"ZweiListen", true);
    split_[0] = c.GetBool(S, L"SplitLinks", false);
    split_[1] = c.GetBool(S, L"SplitRechts", false);
    colRatio_ = std::clamp(c.GetInt(S, L"Spaltenverhaeltnis", 500) / 1000.0, 0.1, 0.9);
    rowRatio_[0] = std::clamp(c.GetInt(S, L"SplitVerhaeltnisLinks", 500) / 1000.0, 0.1, 0.9);
    rowRatio_[1] = std::clamp(c.GetInt(S, L"SplitVerhaeltnisRechts", 500) / 1000.0, 0.1, 0.9);
    treeWidth_ = std::clamp(c.GetInt(S, L"BaumBreite", 220), 60, 1200);
    bookmarkWidth_ = std::clamp(c.GetInt(S, L"LesezeichenBreite", 150), 50, 1000);
    showToolbar_ = c.GetBool(S, L"Werkzeugleiste", true);
    {
        toolLayout_.clear();
        for (auto& k : Split(c.Get(L"Werkzeugleiste", L"Schaltflaechen"), L','))
            if (!Trim(k).empty()) toolLayout_.push_back(Trim(k));
        if (toolLayout_ == DefaultToolLayout()) toolLayout_.clear();
        if (!toolLayout_.empty()) ApplyToolbarLayout();
    }
    showFKeys_ = c.GetBool(S, L"Funktionstastenleiste", true);
    showCmdLine_ = c.GetBool(S, L"Befehlszeile", true);
    showStatus_ = c.GetBool(S, L"Statusleiste", true);
    active_ = std::clamp(c.GetInt(S, L"AktiveListe", 0), 0, 3);
    for (int i = 0; i < 4; ++i) panes_[i]->LoadState(c, L"Liste" + std::to_wstring(i + 1));
}

void MainWindow::SaveState() {
    Config& c = App::Cfg();
    const wchar_t* S = L"Fenster";
    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(hwnd_, &wp);
    c.SetInt(S, L"X", wp.rcNormalPosition.left);
    c.SetInt(S, L"Y", wp.rcNormalPosition.top);
    c.SetInt(S, L"B", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
    c.SetInt(S, L"H", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
    c.SetBool(S, L"Max", wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed(hwnd_) ||
                             (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED)));
    c.SetBool(S, L"ZweiListen", twoPanes_);
    c.SetBool(S, L"SplitLinks", split_[0]);
    c.SetBool(S, L"SplitRechts", split_[1]);
    c.SetInt(S, L"Spaltenverhaeltnis", (int)(colRatio_ * 1000));
    c.SetInt(S, L"SplitVerhaeltnisLinks", (int)(rowRatio_[0] * 1000));
    c.SetInt(S, L"SplitVerhaeltnisRechts", (int)(rowRatio_[1] * 1000));
    c.SetInt(S, L"BaumBreite", treeWidth_);
    c.SetInt(S, L"LesezeichenBreite", bookmarkWidth_);
    c.SetBool(S, L"Werkzeugleiste", showToolbar_);
    c.Set(L"Werkzeugleiste", L"Schaltflaechen", Join(toolLayout_, L","));   // leer = Standard
    c.SetBool(S, L"Funktionstastenleiste", showFKeys_);
    c.SetBool(S, L"Befehlszeile", showCmdLine_);
    c.SetBool(S, L"Statusleiste", showStatus_);
    c.SetInt(S, L"AktiveListe", active_);
    for (int i = 0; i < 4; ++i) panes_[i]->SaveState(c, L"Liste" + std::to_wstring(i + 1));
    SaveBookmarks(c, bookmarks_.Get());
    SaveFavorites(c);
    c.SetList(L"Befehlszeile", L"Befehl", privActive_ ? cmdHistorySnap_ : cmdHistory_);
}

// Privater Modus ein/aus (Schaltfläche „P“, Werkzeuge, Optionen): Beim Einschalten wird der Verlauf festgehalten und
// bis zum Ausschalten so gespeichert; beim Ausschalten gilt wieder der festgehaltene Verlauf.
void MainWindow::SetPrivateMode(bool on) {
    App::Opt().privateMode = on;
    if (on != privActive_) {
        privActive_ = on;
        if (on) {
            for (auto& p : panes_) p->BeginPrivate();
            cmdHistorySnap_ = cmdHistory_;
        } else {
            for (auto& p : panes_) p->EndPrivate();
            cmdHistory_ = cmdHistorySnap_;
            cmdHistorySnap_.clear();
            std::wstring cur = GetWindowTextStr(cmdCombo_);
            SendMessageW(cmdCombo_, CB_RESETCONTENT, 0, 0);
            for (auto& h : cmdHistory_) SendMessageW(cmdCombo_, CB_ADDSTRING, 0, (LPARAM)h.c_str());
            SetWindowTextW(cmdCombo_, cur.c_str());
        }
    }
    for (auto& p : panes_) p->OnPrivateModeChanged();
    UpdatePrivateButton();
    App::SaveSettings();
}

// Werkzeuge → Verlauf löschen: Zurück/Vor-Listen, Verzeichnisverlauf, letzte Verzeichnisse je Laufwerk,
// Befehlszeile, Such- und Umbenennungsmuster (auch der im privaten Modus festgehaltene Stand)
void MainWindow::ClearAllHistory() {
    if (!MsgConfirm(hwnd_, L"Verlauf löschen?\n\nGelöscht werden die Zurück/Vor-Listen und der Verzeichnisverlauf der "
                           L"Listen, der Verlauf der Befehlszeile sowie die zuletzt verwendeten Suchmuster und "
                           L"Umbenennungsmasken."))
        return;
    for (auto& p : panes_) p->ClearHistory();
    cmdHistory_.clear();
    cmdHistorySnap_.clear();
    std::wstring cur = GetWindowTextStr(cmdCombo_);
    SendMessageW(cmdCombo_, CB_RESETCONTENT, 0, 0);
    SetWindowTextW(cmdCombo_, cur.c_str());
    Config& c = App::Cfg();
    c.SetList(L"Suchen", L"Muster", {});
    c.SetList(L"Suchen", L"Text", {});
    c.SetList(L"Umbenennen", L"Name", {});
    c.SetList(L"Umbenennen", L"Erw", {});
    App::SaveSettings();
    LogOperation(L"Verlauf gelöscht");
}

// Favoriten geändert: Lesezeichen „Favoriten“ ein-/ausblenden, Listen mit den Favoriten neu einlesen
void MainWindow::OnFavoritesChanged(bool reloadPanes) {
    bookmarks_.SetFavoritesCount(Favorites().size());
    if (!reloadPanes) return;
    for (int i = 0; i < 4; ++i)
        if (panes_[i]->IsFavorites() && !(quickView_ && i == quickViewPane_)) panes_[i]->Reload();
}

// ===================== Menü, Werkzeugleiste, Tastenkürzel =====================

void MainWindow::BuildMenu() {
    menu_ = CreateMenu();
    HMENU m = CreatePopupMenu();
    AddItem(m, cmd::Open, L"Ö&ffnen\tEingabe");
    AddItem(m, cmd::OpenWith, L"Öffnen &mit…");
    AddItem(m, cmd::View, L"&Anzeigen\tF11");
    AddItem(m, cmd::ViewWindow, L"Im Anzeige&fenster anzeigen\tUmschalt+F11");
    AddItem(m, cmd::Edit, L"&Bearbeiten\tF4");
    AddItem(m, cmd::HexEdit, L"Im &Hex-Editor bearbeiten\tAlt+F11");
    AddSep(m);
    AddItem(m, cmd::NewFile, L"&Neue Datei…\tF9");
    AddItem(m, cmd::NewTextFile, L"Neue &Textdatei…\tUmschalt+F9");
    AddItem(m, cmd::NewFolder, L"Neues &Verzeichnis…\tF8");
    AddItem(m, cmd::Duplicate, L"D&uplizieren…\tF10");
    AddSep(m);
    AddItem(m, cmd::Copy, L"&Kopieren…\tUmschalt+F5");
    AddItem(m, cmd::Move, L"Ve&rschieben…\tUmschalt+F6");
    AddItem(m, cmd::Rename, L"&Umbenennen\tF2");
    AddItem(m, cmd::BatchRename, L"&Dateigruppe umbenennen…\tStrg+M");
    AddItem(m, cmd::Delete, L"&Löschen\tEntf");
    AddItem(m, cmd::DeletePermanent, L"Endgültig lösc&hen\tUmschalt+Entf");
    AddItem(m, cmd::Wipe, L"Rad&ieren (überschreiben und löschen)…\tAlt+Entf");
    AddSep(m);
    AddItem(m, cmd::Attributes, L"Attribute und &Datum ändern…\tStrg+Umschalt+A");
    AddItem(m, cmd::Properties, L"&Eigenschaften…\tAlt+Eingabe");
    AddItem(m, cmd::FavoriteAdd, L"Als &Favorit kennzeichnen");
    AddItem(m, cmd::FavoriteRemove, L"Aus Fa&voriten entfernen");
    AddSep(m);
    AddItem(m, cmd::SplitFile, L"Datei &teilen…");
    AddItem(m, cmd::JoinFiles, L"Dateien &zusammenfügen…");
    AddItem(m, cmd::CreateZip, L"ZIP-Archiv &erstellen…\tAlt+F5");
    AddItem(m, cmd::OpenArchive, L"Archiv anzeigen/en&tpacken…\tAlt+F9");
    AddSep(m);
    AddItem(m, cmd::PrintList, L"Verzeichnisliste &drucken…\tStrg+P");
    AddSep(m);
    AddItem(m, cmd::Undo, L"&Rückgängig\tStrg+Z");
    AddSep(m);
    AddItem(m, cmd::Exit, L"B&eenden\tAlt+F4");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)m, L"&Datei");

    m = CreatePopupMenu();
    AddItem(m, cmd::ClipCut, L"&Ausschneiden\tStrg+X");
    AddItem(m, cmd::ClipCopy, L"&Kopieren\tStrg+C");
    AddItem(m, cmd::ClipPaste, L"&Einfügen\tStrg+V");
    AddSep(m);
    AddItem(m, cmd::SelectAll, L"Alles &markieren\tStrg+A");
    AddItem(m, cmd::SelectNone, L"Markierung auf&heben\tNum /");
    AddItem(m, cmd::InvertSelection, L"Markierung &umkehren\tF6");
    AddItem(m, cmd::SelectGroup, L"&Gruppe markieren…\tNum +");
    AddItem(m, cmd::DeselectGroup, L"Gruppe &abwählen…\tNum -");
    AddItem(m, cmd::SelectSameExt, L"Gleiche &Erweiterung markieren\tAlt+Num +");
    AddSep(m);
    AddItem(m, cmd::CopyPaths, L"Vollständige &Pfade kopieren\tStrg+Umschalt+C");
    AddItem(m, cmd::CopyNames, L"&Namen kopieren");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)m, L"&Bearbeiten");

    m = CreatePopupMenu();
    AddItem(m, cmd::TwoPanes, L"&Zwei Dateilisten\tStrg+Umschalt+L");
    AddItem(m, cmd::SplitToggle, L"Aktive Liste &teilen (Split)\tStrg+T");
    AddItem(m, cmd::QuickView, L"&Dateianzeige im anderen Fenster\tStrg+Q");
    AddSep(m);
    AddItem(m, cmd::ViewDetails, L"De&tails\tStrg+Umschalt+1");
    AddItem(m, cmd::ViewList, L"&Liste\tStrg+Umschalt+2");
    AddItem(m, cmd::ViewIcons, L"&Symbole\tStrg+Umschalt+3");
    AddItem(m, cmd::ViewThumbnails, L"&Miniaturansicht\tStrg+Umschalt+4");
    AddItem(m, cmd::ViewRecursive, L"Mit &Unterverzeichnissen\tStrg+Umschalt+5");
    AddSep(m);
    HMENU sort = CreatePopupMenu();
    AddItem(sort, cmd::SortName, L"&Name");
    AddItem(sort, cmd::SortExt, L"&Typ");
    AddItem(sort, cmd::SortSize, L"&Größe");
    AddItem(sort, cmd::SortDate, L"&Datum");
    AddItem(sort, cmd::SortAttr, L"&Attribute");
    AddItem(sort, cmd::SortCreated, L"&Erstellungsdatum");
    AddSep(sort);
    AddItem(sort, cmd::SortDescending, L"&Absteigend");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sort, L"&Sortieren nach");
    AddItem(m, cmd::ShowHidden, L"&Versteckte Dateien anzeigen\tStrg+H");
    AddItem(m, cmd::ShowDirSizes, L"Verzeichnis&größen anzeigen");
    AddItem(m, cmd::Filter, L"Datei&filter…\tStrg+Umschalt+F");
    AddSep(m);
    AddItem(m, cmd::ToggleToolbar, L"&Werkzeugleiste");
    AddItem(m, cmd::CustomizeToolbar, L"Werkzeugleiste an&passen…");
    AddItem(m, cmd::ToggleCommandLine, L"&Befehlszeile");
    AddItem(m, cmd::ToggleFKeyBar, L"&Funktionstastenleiste");
    AddItem(m, cmd::ToggleStatusBar, L"Status&leiste");
    AddSep(m);
    AddItem(m, cmd::Refresh, L"&Aktualisieren\tF5");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)m, L"&Ansicht");

    m = CreatePopupMenu();
    AddItem(m, cmd::GoBack, L"&Zurück\tAlt+←");
    AddItem(m, cmd::GoForward, L"&Vor\tAlt+→");
    AddItem(m, cmd::GoUp, L"Ü&bergeordnetes Verzeichnis\tRücktaste");
    AddItem(m, cmd::GoRoot, L"&Stammverzeichnis\tStrg+Rücktaste");
    AddItem(m, cmd::FocusPath, L"Verzeichnis &eingeben\tStrg+L");
    AddSep(m);
    AddItem(m, cmd::GoOtherSame, L"Gegenseite: &gleiches Verzeichnis\tStrg+Umschalt+O");
    AddItem(m, cmd::SwapPanes, L"Listen &tauschen\tStrg+U");
    AddSep(m);
    HMENU sp = CreatePopupMenu();
    for (int i = 0; i < kSpecialCount; ++i) AddItem(sp, cmd::GoSpecialFirst + i, kSpecial[i].name);
    AddSep(sp);
    AddItem(sp, cmd::ShowRecycleBin, L"Papier&korb");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sp, L"S&pezielle Verzeichnisse");
    AddSep(m);
    AddItem(m, cmd::NextPane, L"&Nächste Liste\tTab");
    AddItem(m, cmd::FocusPane1, L"Liste &1\tStrg+1");
    AddItem(m, cmd::FocusPane2, L"Liste &2\tStrg+2");
    AddItem(m, cmd::FocusPane3, L"Liste &3\tStrg+3");
    AddItem(m, cmd::FocusPane4, L"Liste &4\tStrg+4");
    AddItem(m, cmd::FocusTree, L"Verzeichnis&baum\tAlt+F1");
    AddItem(m, cmd::FocusBookmarks, L"&Lesezeichenliste\tAlt+F2");
    AddItem(m, cmd::FocusCommandLine, L"Be&fehlszeile\tStrg+E");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)m, L"&Gehe zu");

    bookmarkMenu_ = CreatePopupMenu();
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)bookmarkMenu_, L"&Lesezeichen");

    m = CreatePopupMenu();
    AddItem(m, cmd::FindFiles, L"Dateien &suchen…\tF3");
    AddItem(m, cmd::FindDuplicates, L"&Doppelte Dateien suchen…");
    AddSep(m);
    AddItem(m, cmd::CompareFiles, L"Dateien &vergleichen…\tStrg+K");
    AddItem(m, cmd::CompareDirs, L"Ver&zeichnisse vergleichen…\tF7");
    AddItem(m, cmd::ClearCompareMarks, L"Vergleichsmarkierungen &entfernen");
    AddItem(m, cmd::SyncDirs, L"Verzeichnisse s&ynchronisieren…\tStrg+Umschalt+Y");
    AddSep(m);
    AddItem(m, cmd::DirSizes, L"Verzeichnis&größen berechnen\tAlt+Umschalt+Eingabe");
    AddItem(m, cmd::DriveOverview, L"&Laufwerksübersicht…");
    AddItem(m, cmd::RecycleEmpty, L"Papierkorb &leeren…");
    AddItem(m, cmd::OpLog, L"Dateisystem&monitor (Protokoll)…");
    AddItem(m, cmd::CommandPrompt, L"&Eingabeaufforderung hier öffnen");
    AddSep(m);
    AddItem(m, cmd::PrivateMode, L"&Privater Modus (Verlauf nicht speichern)");
    AddItem(m, cmd::ClearHistory, L"&Verlauf löschen…");
    AddSep(m);
    AddItem(m, cmd::FunctionKeys, L"&Funktionstasten belegen…");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)m, L"&Werkzeuge");
    // „Optionen“ direkt in der Menüleiste
    AppendMenuW(menu_, MF_STRING, cmd::Options, L"&Optionen");

    // „Tastenkürzel“ direkt in der Menüleiste (F1)
    AppendMenuW(menu_, MF_STRING, cmd::Shortcuts, L"&Tastenkürzel");
    m = CreatePopupMenu();
    AddItem(m, cmd::Shortcuts, L"&Tastenkürzel\tF1");
    AddSep(m);
    AddItem(m, cmd::About, L"Ü&ber QFiles…");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)m, L"&Hilfe");
    SetMenu(hwnd_, menu_);
}

void MainWindow::BuildBookmarkMenu(HMENU m) {
    while (GetMenuItemCount(m) > 0) DeleteMenu(m, 0, MF_BYPOSITION);
    AddItem(m, cmd::BookmarkAdd, L"Gewähltes Verzeichnis &hinzufügen…\tStrg+D");
    AddItem(m, cmd::BookmarkNewRemote, L"Neuer &FTP/sFTP/WebDAV-Zugriff…");
    AddItem(m, cmd::BookmarkAddCloud, L"&Cloud-Speicher hinzufügen (Dropbox, Google Drive, OneDrive)…");
    AddItem(m, cmd::BookmarkSort, L"&Sortieren…");
    AddItem(m, cmd::FocusBookmarks, L"Lesezeichenliste &bearbeiten\tAlt+F2");
    const auto& b = bookmarks_.Get();
    if (!b.empty() || !Favorites().empty()) AddSep(m);
    if (!Favorites().empty()) AddItem(m, cmd::BookmarkFavorites, L"&*  Favoriten");
    for (size_t i = 0; i < b.size() && i <= (size_t)(cmd::BookmarkLast - cmd::BookmarkFirst); ++i) {
        std::wstring t = (i < 9 ? (L"&" + std::to_wstring(i + 1) + L"  ") : L"    ") + ReplaceAll(b[i].name, L"&", L"&&") +
                         L"\t" + ReplaceAll(LocationDisplay(b[i].path), L"&", L"&&");
        AddItem(m, cmd::BookmarkFirst + (int)i, t.c_str());
    }
}

void MainWindow::UpdateMenu(HMENU m) {
    if (m == bookmarkMenu_) {
        BuildBookmarkMenu(m);
        return;
    }
    auto check = [&](int id, bool on) { CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    auto enable = [&](int id, bool on) { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    FilePane& a = Active();
    check(cmd::TwoPanes, twoPanes_);
    check(cmd::SplitToggle, split_[active_ % 2]);
    check(cmd::QuickView, quickView_);
    HWND arcView = FocusedArchiveView();
    CheckMenuRadioItem(m, cmd::ViewDetails, cmd::ViewRecursive,
                       cmd::ViewDetails + (arcView ? ArchiveViewGetMode(arcView) : (int)a.View()), MF_BYCOMMAND);
    CheckMenuRadioItem(m, cmd::SortName, cmd::SortCreated, cmd::SortName + (int)a.Sort(), MF_BYCOMMAND);
    check(cmd::SortDescending, a.SortDescending());
    check(cmd::ShowHidden, App::Opt().showHidden);
    check(cmd::ShowDirSizes, App::Opt().showDirSizes);
    check(cmd::PrivateMode, App::Opt().privateMode);
    check(cmd::Filter, a.Filter().IsActive());
    check(cmd::ToggleToolbar, showToolbar_);
    check(cmd::ToggleCommandLine, showCmdLine_);
    check(cmd::ToggleFKeyBar, showFKeys_);
    check(cmd::ToggleStatusBar, showStatus_);
    bool hasSel = !a.SelectedOrFocusedNames().empty();
    bool focusedFile = !a.FocusedName().empty() && !a.FocusedIsDir();
    enable(cmd::Undo, CanUndo());
    std::wstring undoText = CanUndo() ? (L"&Rückgängig: " + UndoDescription() + L"\tStrg+Z") : L"&Rückgängig\tStrg+Z";
    ModifyMenuW(m, cmd::Undo, MF_BYCOMMAND | MF_STRING | (CanUndo() ? 0 : MF_GRAYED), cmd::Undo, undoText.c_str());
    enable(cmd::ClipPaste, ClipboardHasFiles());
    for (int id : {cmd::Copy, cmd::Move, cmd::Delete, cmd::DeletePermanent, cmd::Wipe, cmd::Duplicate, cmd::ClipCut, cmd::ClipCopy, cmd::Attributes,
                   cmd::CreateZip, cmd::CopyPaths, cmd::CopyNames, cmd::BatchRename, cmd::Rename, cmd::Properties})
        enable(id, hasSel);
    for (int id : {cmd::Edit, cmd::HexEdit, cmd::ViewWindow, cmd::SplitFile, cmd::OpenWith}) enable(id, focusedFile);
    enable(cmd::JoinFiles, focusedFile && EndsWithI(a.FocusedName(), L".001"));
    enable(cmd::OpenArchive, focusedFile && IsArchive(a.FocusedName()));
    enable(cmd::GoBack, a.CanGoBack());
    enable(cmd::GoForward, a.CanGoForward());
    enable(cmd::GoUp, LocationHasParent(a.Dir()));
    {
        // Favoriten: nur Dateien (keine Verzeichnisse, nicht auf FTP/SFTP/WebDAV)
        bool canAdd = false, allFav = false;
        if (!a.IsVirtual() && !a.IsSpecialList()) {
            auto paths = a.SelectedOrFocusedPaths();
            canAdd = !paths.empty();
            allFav = !paths.empty();
            for (auto& p : paths) {
                if (!CanBeFavorite(p)) canAdd = false;
                if (!IsFavorite(p)) allFav = false;
            }
        }
        enable(cmd::FavoriteAdd, canAdd && !allFav);
        enable(cmd::FavoriteRemove, (a.IsFavorites() && hasSel) || allFav);
        if (a.IsFavorites())
            for (int id : {cmd::NewFolder, cmd::NewFile, cmd::NewTextFile, cmd::ClipPaste, cmd::FindFiles, cmd::FindDuplicates,
                           cmd::DirSizes, cmd::CreateZip, cmd::PrintList, cmd::GoRoot})
                enable(id, false);
        if (a.IsRecycleBin())
            for (int id : RecycleBinBlocked()) enable(id, false);
    }
    if (a.IsVirtual()) {
        // Netzwerkebenen und FTP/SFTP: nicht unterstützte Befehle sperren
        std::vector<int> off = {cmd::Wipe, cmd::Duplicate, cmd::ClipCut, cmd::ClipCopy, cmd::ClipPaste, cmd::Attributes,
                                cmd::CreateZip, cmd::Properties, cmd::SplitFile, cmd::JoinFiles, cmd::OpenArchive,
                                cmd::PrintList, cmd::FindFiles, cmd::FindDuplicates, cmd::CompareFiles, cmd::CompareDirs,
                                cmd::SyncDirs, cmd::DirSizes, cmd::Undo};
        if (a.IsNetworkLevel())
            for (int id : {cmd::Copy, cmd::Move, cmd::Delete, cmd::DeletePermanent, cmd::Rename, cmd::BatchRename, cmd::Edit,
                           cmd::HexEdit, cmd::ViewWindow, cmd::OpenWith, cmd::NewFolder, cmd::NewFile, cmd::NewTextFile})
                off.push_back(id);
        for (int id : off) enable(id, false);
        if (a.IsRemote()) {
            // FTP/SFTP: Eigenschaften und Attribute zeigen die Unix-Zugriffsrechte (chmod)
            enable(cmd::Properties, true);
            enable(cmd::Attributes, true);
        }
    }
    enable(cmd::GoOtherSame, Other() != nullptr);
    enable(cmd::SwapPanes, Other() != nullptr);
    bool favSide = a.IsSpecialList() || (Other() && Other()->IsSpecialList());
    enable(cmd::CompareDirs, Other() != nullptr && !favSide);
    enable(cmd::SyncDirs, Other() != nullptr && !favSide);
    enable(cmd::ClearCompareMarks, a.HasMarks() || (Other() && Other()->HasMarks()));
    enable(cmd::FocusPane2, PaneVisible(1));
    enable(cmd::FocusPane3, PaneVisible(2));
    enable(cmd::FocusPane4, PaneVisible(3));
}

namespace {

// Zeichnet ein Symbol auf Schwarz und auf Weiß und berechnet daraus Farbe und Deckkraft je Bildpunkt (geglättete
// Kanten und Schrift ohne Farbsaum). px: 32-Bit-Streifen (oben beginnend), stride in Bildpunkten, x0 = Spalte der Zelle.
void RenderAlphaCell(uint32_t* px, int stride, int x0, int sz, const std::function<void(HDC, const RECT&)>& draw) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), sz, -sz, 1, 32, BI_RGB};
    void* bits[2] = {};
    HBITMAP bmp[2] = {CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits[0], nullptr, 0),
                      CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits[1], nullptr, 0)};
    if (!bmp[0] || !bmp[1] || !bits[0] || !bits[1]) {
        for (auto b : bmp)
            if (b) DeleteObject(b);
        return;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    RECT rc{0, 0, sz, sz};
    for (int k = 0; k < 2; ++k) {
        HGDIOBJ old = SelectObject(dc, bmp[k]);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(k == 0 ? BLACK_BRUSH : WHITE_BRUSH));
        draw(dc, rc);
        GdiFlush();
        SelectObject(dc, old);
    }
    DeleteDC(dc);
    auto* b = (const uint32_t*)bits[0];
    auto* w = (const uint32_t*)bits[1];
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            uint32_t cb = b[y * sz + x], cw = w[y * sz + x];
            int diff = 0, ch[3];
            for (int c = 0; c < 3; ++c) {
                int vb = (cb >> (8 * c)) & 0xFF, vw = (cw >> (8 * c)) & 0xFF;
                diff += vw - vb;
                ch[c] = vb;
            }
            int alpha = std::clamp(255 - diff / 3, 0, 255);
            uint32_t out = 0;
            if (alpha > 0) {
                for (int c = 0; c < 3; ++c) out |= (uint32_t)std::min(255, ch[c] * 255 / alpha) << (8 * c);
                out |= (uint32_t)alpha << 24;
            }
            px[y * stride + x0 + x] = out;
        }
    DeleteObject(bmp[0]);
    DeleteObject(bmp[1]);
}

// Papierblatt mit umgeknickter Ecke und deutlichem Buchstaben (Times New Roman, fett)
void DrawPageLetter(HDC dc, const RECT& rc, const wchar_t* letter, COLORREF letterColor) {
    int sz = rc.right - rc.left;
    int l = sz * 3 / 16, r = sz - sz * 3 / 16 - 1, t = 0, bt = sz - 1, f = std::max(3, sz * 5 / 16);
    POINT page[5] = {{l, t}, {r - f, t}, {r, t + f}, {r, bt}, {l, bt}};
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(110, 110, 110));
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, white);
    Polygon(dc, page, 5);
    HBRUSH fold = CreateSolidBrush(RGB(215, 215, 215));
    SelectObject(dc, fold);
    POINT corner[3] = {{r - f, t}, {r - f, t + f}, {r, t + f}};
    Polygon(dc, corner, 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(white);
    DeleteObject(fold);
    HFONT font = CreateFontW(-(sz * 13 / 16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_ROMAN, L"Times New Roman");
    HGDIOBJ of = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, letterColor);
    RECT tr{l, t + sz / 8, r + 1, bt + 1};
    DrawTextW(dc, letter, 1, &tr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(font);
}

// „P“ in einem abgerundeten Viereck: grün gefüllt = privater Modus eingeschaltet, sonst leer
void DrawPrivateP(HDC dc, const RECT& rc, bool on) {
    int sz = rc.right - rc.left;
    int in = std::max(1, sz / 16);
    int pw = std::max(1, sz / 12);
    HPEN pen = CreatePen(PS_SOLID, pw, on ? RGB(20, 120, 40) : RGB(90, 90, 90));
    HBRUSH br = on ? CreateSolidBrush(RGB(40, 175, 70)) : (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    RoundRect(dc, in, in, sz - in, sz - in, sz / 3, sz / 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    if (on) DeleteObject(br);
    HFONT font = CreateFontW(-(sz * 11 / 16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, on ? RGB(255, 255, 255) : RGB(60, 60, 60));
    RECT tr = rc;
    DrawTextW(dc, L"P", 1, &tr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(font);
}


// Beyond Compare: Pfad von BCompare.exe ("" = nicht installiert). QFILES_BCOMPARE (Umgebung) ersetzt die Suche (Tests).
std::wstring BeyondComparePath() {
    wchar_t env[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"QFILES_BCOMPARE", env, MAX_PATH * 2) && FileExists(env)) return env;
    const wchar_t* keys[] = {L"SOFTWARE\\Scooter Software\\Beyond Compare 5", L"SOFTWARE\\Scooter Software\\Beyond Compare 4",
                             L"SOFTWARE\\Scooter Software\\Beyond Compare 3", L"SOFTWARE\\Scooter Software\\Beyond Compare"};
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
        for (auto k : keys)
            for (REGSAM view : {(REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY}) {
                HKEY h = nullptr;
                if (RegOpenKeyExW(root, k, 0, KEY_READ | view, &h) != ERROR_SUCCESS) continue;
                for (auto v : {L"ExePath", L"ExePath64"}) {
                    wchar_t buf[MAX_PATH * 2] = {};
                    DWORD cb = sizeof(buf) - sizeof(wchar_t);
                    if (RegGetValueW(h, nullptr, v, RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS && FileExists(buf)) {
                        RegCloseKey(h);
                        return buf;
                    }
                }
                RegCloseKey(h);
            }
    {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD cb = sizeof(buf) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\BCompare.exe", nullptr,
                         RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) {
            std::wstring p = buf;
            if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
            if (FileExists(p)) return p;
        }
    }
    for (auto base : {GetKnownFolder(FOLDERID_ProgramFiles), GetKnownFolder(FOLDERID_ProgramFilesX86)})
        for (auto dir : {L"Beyond Compare 5", L"Beyond Compare 4", L"Beyond Compare 3"}) {
            std::wstring p = PathCombine(PathCombine(base, dir), L"BCompare.exe");
            if (!base.empty() && FileExists(p)) return p;
        }
    return L"";
}

// Symbol „Mit anderer Liste vergleichen“: schwarzer Kreis, zur Hälfte bzw. zum Viertel grau in Richtung der anderen
// Liste (dx/dy: -1 links/oben, 1 rechts/unten). 32 Bit mit vormultipliziertem Alpha (Menübild).
HBITMAP CompareDirectionBitmap(int size, int dx, int dy) {
    std::vector<uint32_t> px((size_t)size * size, 0);
    RenderAlphaCell(px.data(), size, 0, size, [&](HDC d, const RECT& r) {
        int sz = r.right - r.left;
        int in = std::max(1, sz / 16);
        int l = in, t = in, rr = sz - in, b = sz - in;
        int cx = sz / 2, cy = sz / 2;
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        HGDIOBJ ob = SelectObject(d, black), op = SelectObject(d, GetStockObject(NULL_PEN));
        Ellipse(d, l, t, rr + 1, b + 1);
        HBRUSH gray = CreateSolidBrush(RGB(175, 175, 175));
        SelectObject(d, gray);
        int g = std::max(1, sz / 9);   // schwarzer Rand
        int gl = l + g, gt = t + g, gr = rr - g + 1, gb = b - g + 1;
        // Pie zeichnet gegen den Uhrzeigersinn vom ersten zum zweiten Punkt
        POINT top{cx, gt - 2}, bottom{cx, gb + 2}, left{gl - 2, cy}, right{gr + 2, cy};
        POINT a{}, e{};
        if (dx != 0 && dy != 0) {          // Viertel
            if (dx > 0 && dy < 0) a = right, e = top;
            else if (dx < 0 && dy < 0) a = top, e = left;
            else if (dx < 0 && dy > 0) a = left, e = bottom;
            else a = bottom, e = right;
        } else if (dx != 0) {              // Hälfte links/rechts
            if (dx < 0) a = top, e = bottom;
            else a = bottom, e = top;
        } else {                           // Hälfte oben/unten
            if (dy < 0) a = right, e = left;
            else a = left, e = right;
        }
        Pie(d, gl, gt, gr, gb, a.x, a.y, e.x, e.y);
        SelectObject(d, ob);
        SelectObject(d, op);
        DeleteObject(black);
        DeleteObject(gray);
    });
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return bmp;
    auto* out = (uint32_t*)bits;
    for (size_t i = 0; i < px.size(); ++i) {
        uint32_t c = px[i], al = c >> 24;
        uint32_t r = ((c >> 16) & 0xFF) * al / 255, g = ((c >> 8) & 0xFF) * al / 255, b = (c & 0xFF) * al / 255;
        out[i] = (al << 24) | (r << 16) | (g << 8) | b;
    }
    return bmp;
}

std::wstring DirectionText(int dx, int dy) {
    std::wstring h = dx < 0 ? L"links" : dx > 0 ? L"rechts" : L"";
    std::wstring v = dy < 0 ? L"oben" : dy > 0 ? L"unten" : L"";
    return h.empty() ? v : v.empty() ? h : h + L" " + v;
}

} // namespace

void MainWindow::BuildToolbar() {
    toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                               WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | TBSTYLE_LIST | CCS_NODIVIDER |
                                   CCS_NORESIZE | CCS_NOPARENTALIGN,
                               0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)cmd::IdToolbar, App::Instance(), nullptr);
    SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DOUBLEBUFFER);
    bool large = GetWindowDpi(hwnd_) >= 120;
    TBADDBITMAP ab{HINST_COMMCTRL, (UINT_PTR)(large ? IDB_STD_LARGE_COLOR : IDB_STD_SMALL_COLOR)};
    int offStd = 0;
    SendMessageW(toolbar_, TB_ADDBITMAP, 0, (LPARAM)&ab);
    int nStd = 15;
    TBADDBITMAP av{HINST_COMMCTRL, (UINT_PTR)(large ? IDB_VIEW_LARGE_COLOR : IDB_VIEW_SMALL_COLOR)};
    int offView = (int)SendMessageW(toolbar_, TB_ADDBITMAP, 0, (LPARAM)&av);
    if (offView <= 0) offView = nStd;
    TBADDBITMAP ah{HINST_COMMCTRL, (UINT_PTR)(large ? IDB_HIST_LARGE_COLOR : IDB_HIST_SMALL_COLOR)};
    int offHist = (int)SendMessageW(toolbar_, TB_ADDBITMAP, 0, (LPARAM)&ah);
    if (offHist <= 0) offHist = offView + 13;
    // Eigene Symbole, 32 Bit mit Alpha: Papierkorb (Windows-Standardsymbol), Radiergummi, „Mit Unterverzeichnissen“,
    // Neue Datei/Textdatei, „P“ aus/ein und die Symbole der zusätzlich wählbaren Schaltflächen (Werkzeugleiste anpassen)
    int custom = -1;
    const int cells = 27;
    {
        int sz = large ? 24 : 16;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), sz * cells, -sz, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP strip = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (strip && bits) {
            auto* px = (uint32_t*)bits;
            memset(bits, 0, (size_t)sz * cells * sz * 4);
            HDC dc = CreateCompatibleDC(nullptr);
            HGDIOBJ old = SelectObject(dc, strip);
            // Zelle 0: Papierkorb
            SHSTOCKICONINFO sii{sizeof(sii)};
            if (SUCCEEDED(SHGetStockIconInfo(SIID_RECYCLERFULL, SHGSI_ICON | (large ? SHGSI_LARGEICON : SHGSI_SMALLICON), &sii)) ||
                SUCCEEDED(SHGetStockIconInfo(SIID_RECYCLER, SHGSI_ICON | (large ? SHGSI_LARGEICON : SHGSI_SMALLICON), &sii))) {
                DrawIconEx(dc, 0, 0, sii.hIcon, sz, sz, 0, nullptr, DI_NORMAL);
                DestroyIcon(sii.hIcon);
            }
            // Zellen 1 und 2: Radiergummi und „Mit Unterverzeichnissen“ auf Schlüsselfarbe zeichnen, danach in Alpha
            // umwandeln
            for (int y = 0; y < sz; ++y)
                for (int x = sz; x < 3 * sz; ++x) px[y * sz * cells + x] = 0x00FF00FF;
            GdiFlush();
            RECT cell{sz, 0, 2 * sz, sz};
            DrawGlyph(dc, Glyph::Eraser, cell, RGB(0, 0, 0));
            RECT cell2{2 * sz, 0, 3 * sz, sz};
            DrawGlyph(dc, Glyph::TreeList, cell2, RGB(0, 0, 0));
            GdiFlush();
            for (int y = 0; y < sz; ++y)
                for (int x = sz; x < 3 * sz; ++x) {
                    uint32_t& c = px[y * sz * cells + x];
                    c = ((c & 0x00FFFFFF) == 0x00FF00FF) ? 0 : (c | 0xFF000000u);
                }
            SelectObject(dc, old);
            // Zellen 3–6: geglättet gezeichnet (Papierblatt mit N bzw. T, „P“ aus/ein)
            int stride = sz * cells;
            RenderAlphaCell(px, stride, 3 * sz, sz, [](HDC d, const RECT& r) { DrawPageLetter(d, r, L"N", RGB(20, 55, 150)); });
            RenderAlphaCell(px, stride, 4 * sz, sz, [](HDC d, const RECT& r) { DrawPageLetter(d, r, L"T", RGB(150, 35, 20)); });
            RenderAlphaCell(px, stride, 5 * sz, sz, [](HDC d, const RECT& r) { DrawPrivateP(d, r, false); });
            RenderAlphaCell(px, stride, 6 * sz, sz, [](HDC d, const RECT& r) { DrawPrivateP(d, r, true); });
            // Zellen 7–26: zusätzliche Schaltflächen
            auto stock = [&](int idx, SHSTOCKICONID id) {
                RenderAlphaCell(px, stride, idx * sz, sz, [&](HDC d, const RECT&) {
                    SHSTOCKICONINFO si{sizeof(si)};
                    if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICON | (large ? SHGSI_LARGEICON : SHGSI_SMALLICON), &si))) {
                        DrawIconEx(d, 0, 0, si.hIcon, sz, sz, 0, nullptr, DI_NORMAL);
                        DestroyIcon(si.hIcon);
                    }
                });
            };
            auto page = [&](int idx, const wchar_t* letter, COLORREF color) {
                RenderAlphaCell(px, stride, idx * sz, sz, [&](HDC d, const RECT& r) { DrawPageLetter(d, r, letter, color); });
            };
            auto glyph = [&](int idx, Glyph g) {
                RenderAlphaCell(px, stride, idx * sz, sz, [&](HDC d, const RECT& r) { DrawGlyph(d, g, r, RGB(30, 30, 30)); });
            };
            RenderAlphaCell(px, stride, 7 * sz, sz, [&](HDC d, const RECT&) {   // Aktualisieren: Kreispfeil
                int w = std::max(2, sz / 8), m = sz / 6 + w / 2;
                HPEN pen = CreatePen(PS_SOLID, w, RGB(30, 110, 50));
                HGDIOBJ op = SelectObject(d, pen), ob = SelectObject(d, GetStockObject(NULL_BRUSH));
                int c = sz / 2;
                // offener Kreis von rechts oben (Lücke) gegen den Uhrzeigersinn
                Arc(d, m, m, sz - m, sz - m, c + (sz - 2 * m) / 4, m, sz - m, c - (sz - 2 * m) / 8);
                SelectObject(d, op);
                DeleteObject(pen);
                HBRUSH br = CreateSolidBrush(RGB(30, 110, 50));
                HPEN np = (HPEN)GetStockObject(NULL_PEN);
                SelectObject(d, br);
                SelectObject(d, np);
                // Pfeilspitze am Ende des Bogens (rechts), nach oben zeigend
                int r = (sz - 2 * m) / 2;
                int ax = c + (int)(r * 0.94), ay = c - (int)(r * 0.34);
                int hs = std::max(3, sz / 4);
                POINT tri[3] = {{ax - hs + 1, ay + 1}, {ax, ay - hs}, {ax + hs - 1, ay + 1}};
                Polygon(d, tri, 3);
                SelectObject(d, ob);
                DeleteObject(br);
            });
            stock(8, SIID_RENAME);
            page(9, L"M", RGB(20, 55, 150));
            page(10, L"A", RGB(20, 110, 40));
            page(11, L"#", RGB(120, 30, 120));
            stock(12, SIID_ZIPFILE);
            stock(13, SIID_PRINTER);
            page(14, L"=", RGB(20, 55, 150));
            page(15, L"≠", RGB(150, 35, 20));
            page(16, L"Σ", RGB(20, 110, 40));
            stock(17, SIID_DRIVEFIXED);
            RenderAlphaCell(px, stride, 18 * sz, sz, [&](HDC d, const RECT&) {   // Eingabeaufforderung: Symbol von cmd.exe
                wchar_t sys[MAX_PATH] = {};
                GetSystemDirectoryW(sys, MAX_PATH);
                HICON iconBig = nullptr, iconSmall = nullptr;   // („small“ ist bei MSVC ein Makro)
                if (ExtractIconExW((std::wstring(sys) + L"\\cmd.exe").c_str(), 0, &iconBig, &iconSmall, 1) > 0) {
                    HICON ic = large ? iconBig : iconSmall;
                    if (!ic) ic = iconBig ? iconBig : iconSmall;
                    if (ic) DrawIconEx(d, 0, 0, ic, sz, sz, 0, nullptr, DI_NORMAL);
                }
                if (iconBig) DestroyIcon(iconBig);
                if (iconSmall) DestroyIcon(iconSmall);
            });
            page(19, L"L", RGB(90, 90, 90));
            stock(20, SIID_RECYCLER);
            RenderAlphaCell(px, stride, 21 * sz, sz, [](HDC d, const RECT& r) {   // Favoriten: Asterisk
                DrawAsterisk(d, r, RGB(150, 20, 20), GetPixel(d, 0, 0));
            });
            RenderAlphaCell(px, stride, 22 * sz, sz, [&](HDC d, const RECT&) {   // Dateifilter: Trichter
                HBRUSH br = CreateSolidBrush(RGB(60, 110, 190));
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(30, 60, 120));
                HGDIOBJ ob = SelectObject(d, br), op = SelectObject(d, pen);
                int m = sz / 8;
                POINT f[6] = {{m, m + 1}, {sz - m - 1, m + 1}, {sz / 2 + sz / 10, sz / 2}, {sz / 2 + sz / 10, sz - m - 1},
                              {sz / 2 - sz / 10, sz - m - 3}, {sz / 2 - sz / 10, sz / 2}};
                Polygon(d, f, 6);
                SelectObject(d, ob);
                SelectObject(d, op);
                DeleteObject(br);
                DeleteObject(pen);
            });
            RenderAlphaCell(px, stride, 23 * sz, sz, [&](HDC d, const RECT&) {   // versteckte Dateien: Auge
                HPEN pen = CreatePen(PS_SOLID, std::max(1, sz / 12), RGB(40, 40, 40));
                HGDIOBJ op = SelectObject(d, pen), ob = SelectObject(d, GetStockObject(WHITE_BRUSH));
                int m = sz / 10;
                Ellipse(d, m, sz / 4, sz - m, sz - sz / 4);
                HBRUSH iris = CreateSolidBrush(RGB(30, 90, 210));
                SelectObject(d, iris);
                Ellipse(d, sz / 2 - sz / 6, sz / 2 - sz / 6, sz / 2 + sz / 6 + 1, sz / 2 + sz / 6 + 1);
                SelectObject(d, op);
                SelectObject(d, ob);
                DeleteObject(pen);
                DeleteObject(iris);
            });
            glyph(24, Glyph::Split);
            page(25, L"F", RGB(20, 55, 150));
            stock(26, SIID_HELP);
            DeleteDC(dc);
            TBADDBITMAP ac{nullptr, (UINT_PTR)strip};
            custom = (int)SendMessageW(toolbar_, TB_ADDBITMAP, cells, (LPARAM)&ac);
        }
    }
    auto img = [&](int cell, int fallback) { return custom >= 0 ? custom + cell : fallback; };
    imgPrivateOff_ = img(5, offStd + STD_PROPERTIES);
    imgPrivateOn_ = img(6, offStd + STD_PROPERTIES);

    // Alle wählbaren Schaltflächen (Schlüssel in der Einstellungsdatei, Befehl, Symbol, Tooltip)
    toolDefs_ = {
        {L"Zurueck", cmd::GoBack, offHist + HIST_BACK, L"Zurück (Alt+←)"},
        {L"Vor", cmd::GoForward, offHist + HIST_FORWARD, L"Vor (Alt+→)"},
        {L"Hoch", cmd::GoUp, offView + VIEW_PARENTFOLDER, L"Übergeordnetes Verzeichnis (Rücktaste)"},
        {L"Aktualisieren", cmd::Refresh, img(7, offView + VIEW_DETAILS), L"Aktualisieren (Strg+R)"},
        {L"Kopieren", cmd::Copy, offStd + STD_COPY, L"Kopieren in die andere Liste (Umschalt+F5)"},
        {L"Verschieben", cmd::Move, offStd + STD_CUT, L"Verschieben in die andere Liste (Umschalt+F6)"},
        {L"NeuesVerzeichnis", cmd::NewFolder, offView + VIEW_NEWFOLDER, L"Neues Verzeichnis (F8)"},
        {L"NeueDatei", cmd::NewFile, img(3, offStd + STD_FILENEW), L"Neue Datei (F9)"},
        {L"NeueTextdatei", cmd::NewTextFile, img(4, offStd + STD_FILENEW), L"Neue Textdatei (Umschalt+F9)"},
        {L"Umbenennen", cmd::Rename, img(8, offStd + STD_REPLACE), L"Umbenennen (F2)"},
        {L"GruppeUmbenennen", cmd::BatchRename, img(9, offStd + STD_REPLACE), L"Dateigruppe umbenennen (Strg+M)"},
        {L"Duplizieren", cmd::Duplicate, offStd + STD_COPY, L"Duplizieren (F10)"},
        {L"Loeschen", cmd::Delete, img(0, offStd + STD_DELETE), L"Löschen – in den Papierkorb (Entf)"},
        {L"EndgueltigLoeschen", cmd::DeletePermanent, offStd + STD_DELETE, L"Endgültig löschen (Umschalt+Entf)"},
        {L"Radieren", cmd::Wipe, img(1, offStd + STD_DELETE), L"Radieren – mit Zufallsdaten überschreiben und löschen (Alt+Entf)"},
        {L"Rueckgaengig", cmd::Undo, offStd + STD_UNDO, L"Rückgängig (Strg+Z)"},
        {L"Ausschneiden", cmd::ClipCut, offStd + STD_CUT, L"Ausschneiden – Zwischenablage (Strg+X)"},
        {L"KopierenZwischenablage", cmd::ClipCopy, offStd + STD_COPY, L"Kopieren – Zwischenablage (Strg+C)"},
        {L"Einfuegen", cmd::ClipPaste, offStd + STD_PASTE, L"Einfügen – Zwischenablage (Strg+V)"},
        {L"Attribute", cmd::Attributes, img(10, offStd + STD_PROPERTIES), L"Attribute/Datum ändern (Strg+Umschalt+A)"},
        {L"Anzeigen", cmd::View, offStd + STD_PRINTPRE, L"Anzeigen (F11)"},
        {L"Bearbeiten", cmd::Edit, offStd + STD_FILENEW, L"Bearbeiten (F4)"},
        {L"HexEditor", cmd::HexEdit, img(11, offStd + STD_FILENEW), L"Im Hex-Editor bearbeiten (Alt+F11)"},
        {L"Eigenschaften", cmd::Properties, offStd + STD_PROPERTIES, L"Eigenschaften (Alt+Eingabe)"},
        {L"ZipErstellen", cmd::CreateZip, img(12, offStd + STD_FILESAVE), L"ZIP-Archiv erstellen (Alt+F5)"},
        {L"Drucken", cmd::PrintList, img(13, offStd + STD_PRINT), L"Verzeichnisliste drucken (Strg+P)"},
        {L"Suchen", cmd::FindFiles, offStd + STD_FIND, L"Dateien suchen (F3)"},
        {L"Doppelte", cmd::FindDuplicates, img(14, offStd + STD_FIND), L"Doppelte Dateien suchen"},
        {L"DateienVergleichen", cmd::CompareFiles, img(15, offStd + STD_REPLACE), L"Dateien vergleichen (Strg+K)"},
        {L"VerzeichnisseVergleichen", cmd::CompareDirs, offStd + STD_REPLACE, L"Verzeichnisse vergleichen (F7)"},
        {L"Synchronisieren", cmd::SyncDirs, offStd + STD_REDOW, L"Verzeichnisse synchronisieren (Strg+Umschalt+Y)"},
        {L"Verzeichnisgroessen", cmd::DirSizes, img(16, offStd + STD_PROPERTIES), L"Verzeichnisgrößen berechnen (Alt+Umschalt+Eingabe)"},
        {L"Laufwerke", cmd::DriveOverview, img(17, offStd + STD_PROPERTIES), L"Laufwerksübersicht"},
        {L"Eingabeaufforderung", cmd::CommandPrompt, img(18, offStd + STD_PROPERTIES), L"Eingabeaufforderung im aktuellen Verzeichnis"},
        {L"Dateisystemmonitor", cmd::OpLog, img(19, offStd + STD_PROPERTIES), L"Dateisystemmonitor (Protokoll)"},
        {L"Papierkorb", cmd::ShowRecycleBin, img(20, offStd + STD_DELETE), L"Papierkorb anzeigen"},
        {L"Favoriten", cmd::BookmarkFavorites, img(21, offHist + HIST_FAVORITES), L"Favoriten anzeigen"},
        {L"Details", cmd::ViewDetails, offView + VIEW_DETAILS, L"Details (Strg+Umschalt+1)"},
        {L"Liste", cmd::ViewList, offView + VIEW_LIST, L"Liste (Strg+Umschalt+2)"},
        {L"Symbole", cmd::ViewIcons, offView + VIEW_SMALLICONS, L"Symbole (Strg+Umschalt+3)"},
        {L"Miniaturen", cmd::ViewThumbnails, offView + VIEW_LARGEICONS, L"Miniaturansicht (Strg+Umschalt+4)"},
        {L"MitUnterverzeichnissen", cmd::ViewRecursive, img(2, offView + VIEW_DETAILS), L"Mit Unterverzeichnissen (Strg+Umschalt+5)"},
        {L"Dateifilter", cmd::Filter, img(22, offView + VIEW_SORTTYPE), L"Dateifilter (Strg+Umschalt+F)"},
        {L"Versteckte", cmd::ShowHidden, img(23, offView + VIEW_SORTTYPE), L"Versteckte Dateien anzeigen (Strg+H)"},
        {L"ZweiListen", cmd::TwoPanes, offHist + HIST_VIEWTREE, L"Eine/zwei Dateilisten (Strg+Umschalt+L)"},
        {L"Teilen", cmd::SplitToggle, img(24, offHist + HIST_VIEWTREE), L"Liste teilen (Strg+T)"},
        {L"Schnellansicht", cmd::QuickView, offView + VIEW_VIEWMENU, L"Dateianzeige im anderen Fenster (Strg+Q)"},
        {L"LesezeichenHinzufuegen", cmd::BookmarkAdd, offHist + HIST_ADDTOFAVORITES, L"Lesezeichen hinzufügen (Strg+D)"},
        {L"Funktionstasten", cmd::FunctionKeys, img(25, offStd + STD_PROPERTIES), L"Funktionstasten belegen"},
        {L"Optionen", cmd::Options, offStd + STD_PROPERTIES, L"Optionen"},
        {L"Tastenkuerzel", cmd::Shortcuts, img(26, offStd + STD_HELP), L"Tastenkürzel (F1)"},
        {L"PrivaterModus", cmd::PrivateMode, App::Opt().privateMode ? imgPrivateOn_ : imgPrivateOff_,
         L"Privater Modus: Verlauf nicht speichern (grün = eingeschaltet)"},
    };
    toolTips_.clear();
    for (auto& d : toolDefs_) toolTips_.emplace_back(d.cmd, d.tip);
    SendMessageW(toolbar_, WM_SETFONT, (WPARAM)App::UIFont(), TRUE);
    ApplyToolbarLayout();
}

namespace {

// Dialog „Werkzeugleiste anpassen“: links die verfügbaren, rechts die gewählten Schaltflächen (mit Trennlinien)
class ToolbarDlg : public DialogBase {
public:
    struct Def {
        std::wstring key, text;
        int image;
    };
    ToolbarDlg(std::vector<Def> defs, std::vector<std::wstring> layout, std::vector<std::wstring> defaults, HIMAGELIST il)
        : defs_(std::move(defs)), layout_(std::move(layout)), defaults_(std::move(defaults)), il_(il) {}
    const std::vector<std::wstring>& Layout() const { return layout_; }

    enum { kAvail = 101, kAdd, kRemove, kSep, kCur, kUp, kDown, kDefault, kLblAvail, kLblCur };

protected:
    BOOL OnInit() override {
        for (int id : {kAvail, kCur}) {
            HWND lv = Item(id);
            ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_SetImageList(lv, il_, LVSIL_SMALL);
            RECT rc;
            GetClientRect(lv, &rc);
            LVCOLUMNW c{};
            c.mask = LVCF_WIDTH;
            c.cx = rc.right - GetSystemMetrics(SM_CXVSCROLL);
            ListView_InsertColumn(lv, 0, &c);
        }
        Fill(0, 0);
        return TRUE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        switch (id) {
        case kAdd: Add(); return TRUE;
        case kRemove: Remove(); return TRUE;
        case kSep: Insert(L"-"); return TRUE;
        case kUp: Move(-1); return TRUE;
        case kDown: Move(1); return TRUE;
        case kDefault:
            layout_ = defaults_;
            Fill(0, 0);
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }
    INT_PTR OnNotify(NMHDR* nm) override {
        if (nm->code == NM_DBLCLK) {
            if (nm->idFrom == kAvail) Add();
            if (nm->idFrom == kCur) Remove();
        }
        if (nm->code == LVN_ITEMCHANGED) UpdateButtons();
        return 0;
    }

private:
    const Def* Find(const std::wstring& key) const {
        for (auto& d : defs_)
            if (d.key == key) return &d;
        return nullptr;
    }
    static int Sel(HWND lv) { return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }
    void AddRow(HWND lv, int row, const std::wstring& text, int image) {
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = row;
        it.pszText = const_cast<wchar_t*>(text.c_str());
        it.iImage = image;
        ListView_InsertItem(lv, &it);
    }
    void Fill(int selAvail, int selCur) {
        HWND a = Item(kAvail), c = Item(kCur);
        SendMessageW(a, WM_SETREDRAW, FALSE, 0);
        SendMessageW(c, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(a);
        ListView_DeleteAllItems(c);
        avail_.clear();
        for (auto& d : defs_)
            if (std::find(layout_.begin(), layout_.end(), d.key) == layout_.end()) {
                AddRow(a, (int)avail_.size(), d.text, d.image);
                avail_.push_back(d.key);
            }
        for (size_t i = 0; i < layout_.size(); ++i) {
            const Def* d = Find(layout_[i]);
            AddRow(c, (int)i, layout_[i] == L"-" ? L"──────── Trennlinie ────────" : d ? d->text : layout_[i],
                   d ? d->image : I_IMAGENONE);
        }
        auto select = [](HWND lv, int i) {
            int n = ListView_GetItemCount(lv);
            if (n == 0) return;
            i = std::clamp(i, 0, n - 1);
            ListView_SetItemState(lv, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(lv, i, FALSE);
        };
        select(a, selAvail);
        select(c, selCur);
        SendMessageW(a, WM_SETREDRAW, TRUE, 0);
        SendMessageW(c, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(a, nullptr, TRUE);
        InvalidateRect(c, nullptr, TRUE);
        UpdateButtons();
    }
    void UpdateButtons() {
        int a = Sel(Item(kAvail)), c = Sel(Item(kCur));
        Enable(kAdd, a >= 0);
        Enable(kRemove, c >= 0);
        Enable(kUp, c > 0);
        Enable(kDown, c >= 0 && c + 1 < (int)layout_.size());
    }
    void Insert(const std::wstring& key) {
        int c = Sel(Item(kCur));
        size_t pos = c >= 0 ? (size_t)c + 1 : layout_.size();
        layout_.insert(layout_.begin() + pos, key);
        Fill(Sel(Item(kAvail)), (int)pos);
    }
    void Add() {
        int a = Sel(Item(kAvail));
        if (a < 0 || a >= (int)avail_.size()) return;
        Insert(avail_[a]);
    }
    void Remove() {
        int c = Sel(Item(kCur));
        if (c < 0 || c >= (int)layout_.size()) return;
        layout_.erase(layout_.begin() + c);
        Fill(Sel(Item(kAvail)), c);
    }
    void Move(int dir) {
        int c = Sel(Item(kCur));
        int t = c + dir;
        if (c < 0 || t < 0 || t >= (int)layout_.size()) return;
        std::swap(layout_[c], layout_[t]);
        Fill(Sel(Item(kAvail)), t);
    }

    std::vector<Def> defs_;
    std::vector<std::wstring> layout_, defaults_, avail_;
    HIMAGELIST il_;
};

} // namespace

void MainWindow::CustomizeToolbar() {
    if (!toolbar_) return;
    std::vector<ToolbarDlg::Def> defs;
    for (auto& d : toolDefs_)
        defs.push_back({d.key, d.tip, d.cmd == cmd::PrivateMode ? (App::Opt().privateMode ? imgPrivateOn_ : imgPrivateOff_)
                                                                 : d.image});
    HIMAGELIST il = (HIMAGELIST)SendMessageW(toolbar_, TB_GETIMAGELIST, 0, 0);
    ToolbarDlg dlg(defs, toolLayout_.empty() ? DefaultToolLayout() : toolLayout_, DefaultToolLayout(), il);
    DialogTemplate t(L"Werkzeugleiste anpassen", 430, 262);
    t.Label(ToolbarDlg::kLblAvail, L"&Verfügbare Schaltflächen:", 7, 7, 170, 9);
    t.ListView(ToolbarDlg::kAvail, 7, 18, 170, 200,
               LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS | WS_TABSTOP);
    t.Button(ToolbarDlg::kAdd, L"&Hinzufügen →", 183, 70, 64, 14);
    t.Button(ToolbarDlg::kRemove, L"← &Entfernen", 183, 88, 64, 14);
    t.Button(ToolbarDlg::kSep, L"&Trennlinie", 183, 114, 64, 14);
    t.Label(ToolbarDlg::kLblCur, L"&Schaltflächen der Werkzeugleiste:", 253, 7, 170, 9);
    t.ListView(ToolbarDlg::kCur, 253, 18, 170, 200,
               LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS | WS_TABSTOP);
    t.Button(ToolbarDlg::kUp, L"Nach &oben", 253, 222, 60, 14);
    t.Button(ToolbarDlg::kDown, L"Nach &unten", 317, 222, 60, 14);
    t.Button(ToolbarDlg::kDefault, L"Standar&d", 7, 222, 60, 14);
    t.DefButton(IDOK, L"OK", 319, 242, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 373, 242, 50, 14);
    if (dlg.DoModal(hwnd_, t) != IDOK) return;
    toolLayout_ = dlg.Layout();
    if (toolLayout_ == DefaultToolLayout()) toolLayout_.clear();
    ApplyToolbarLayout();
    UpdatePrivateButton();
    App::SaveSettings();
}

// Standardbelegung der Werkzeugleiste ("-" = Trennlinie)
std::vector<std::wstring> MainWindow::DefaultToolLayout() {
    return {L"Zurueck", L"Vor", L"Hoch", L"-", L"Kopieren", L"Verschieben", L"NeuesVerzeichnis", L"NeueDatei",
            L"NeueTextdatei", L"Loeschen", L"EndgueltigLoeschen", L"Radieren", L"Rueckgaengig", L"-", L"Anzeigen",
            L"Bearbeiten", L"Eigenschaften", L"-", L"Suchen", L"VerzeichnisseVergleichen", L"Synchronisieren", L"-",
            L"Details", L"Liste", L"Symbole", L"Miniaturen", L"MitUnterverzeichnissen", L"-", L"ZweiListen",
            L"Schnellansicht", L"-", L"LesezeichenHinzufuegen", L"Optionen", L"-", L"PrivaterModus"};
}

// Schaltflächen nach toolLayout_ (leer = Standard) neu aufbauen
void MainWindow::ApplyToolbarLayout() {
    if (!toolbar_) return;
    while (SendMessageW(toolbar_, TB_BUTTONCOUNT, 0, 0) > 0) SendMessageW(toolbar_, TB_DELETEBUTTON, 0, 0);
    std::vector<std::wstring> layout = toolLayout_.empty() ? DefaultToolLayout() : toolLayout_;
    std::vector<TBBUTTON> tb;
    bool lastSep = true;
    for (auto& key : layout) {
        TBBUTTON t{};
        if (key == L"-") {
            if (lastSep) continue;   // keine doppelten Trennlinien
            t.fsStyle = BTNS_SEP;
            lastSep = true;
        } else {
            auto it = std::find_if(toolDefs_.begin(), toolDefs_.end(), [&](const ToolDef& d) { return d.key == key; });
            if (it == toolDefs_.end()) continue;   // unbekannt (ältere/neuere Version)
            t.iBitmap = it->cmd == cmd::PrivateMode ? (App::Opt().privateMode ? imgPrivateOn_ : imgPrivateOff_) : it->image;
            t.idCommand = it->cmd;
            t.fsState = TBSTATE_ENABLED;
            t.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
            if (it->cmd >= cmd::ViewDetails && it->cmd <= cmd::ViewRecursive) t.fsStyle |= BTNS_CHECK;   // Zustand setzt UpdateViewButtons
            t.iString = -1;
            lastSep = false;
        }
        tb.push_back(t);
    }
    while (!tb.empty() && (tb.back().fsStyle & BTNS_SEP)) tb.pop_back();
    if (!tb.empty()) SendMessageW(toolbar_, TB_ADDBUTTONSW, tb.size(), (LPARAM)tb.data());
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    UpdateViewButtons();
    if (hwnd_ && IsWindowVisible(hwnd_)) Layout();
}

// Symbol „P“: grün gefüllt, solange der private Modus eingeschaltet ist
void MainWindow::UpdatePrivateButton() {
    if (!toolbar_ || imgPrivateOff_ < 0) return;
    SendMessageW(toolbar_, TB_CHANGEBITMAP, cmd::PrivateMode,
                 MAKELPARAM(App::Opt().privateMode ? imgPrivateOn_ : imgPrivateOff_, 0));
}

void MainWindow::BuildAccelerators() {
    std::vector<ACCEL> a;
    auto add = [&](BYTE flags, WORD key, int id) { a.push_back({(BYTE)(flags | FVIRTKEY), key, (WORD)id}); };
    // Funktionstasten F2–F11 (Fußzeile)
    add(0, VK_F2, cmd::Rename);
    add(0, VK_F3, cmd::FindFiles);
    add(0, VK_F4, cmd::Edit);
    add(0, VK_F5, cmd::Refresh);
    add(0, VK_F6, cmd::InvertSelection);
    add(0, VK_F7, cmd::CompareDirs);
    add(0, VK_F8, cmd::NewFolder);
    add(0, VK_F9, cmd::NewFile);
    add(0, VK_F10, cmd::Duplicate);      // F10 wird zusätzlich in PreTranslate behandelt (Systemtaste)
    add(0, VK_F11, cmd::View);
    // Weitere Belegungen
    add(FSHIFT, VK_F11, cmd::ViewWindow);
    add(FALT, VK_F11, cmd::HexEdit);
    add(FSHIFT, VK_F9, cmd::NewTextFile);
    add(FSHIFT, VK_F5, cmd::Copy);
    add(FSHIFT, VK_F6, cmd::Move);
    add(0, VK_DELETE, cmd::Delete);
    add(FSHIFT, VK_DELETE, cmd::DeletePermanent);
    add(FALT, VK_DELETE, cmd::Wipe);
    add(FALT, VK_RETURN, cmd::Properties);
    add(FALT | FSHIFT, VK_RETURN, cmd::DirSizes);
    add(FCONTROL, VK_RETURN, cmd::CmdLineInsertName);
    add(FCONTROL | FSHIFT, 'A', cmd::Attributes);
    add(FCONTROL, 'M', cmd::BatchRename);
    add(FALT, VK_F5, cmd::CreateZip);
    add(FALT, VK_F9, cmd::OpenArchive);
    add(FCONTROL, 'P', cmd::PrintList);
    add(FCONTROL, 'Z', cmd::Undo);
    add(FCONTROL, 'X', cmd::ClipCut);
    add(FCONTROL, 'C', cmd::ClipCopy);
    add(FCONTROL, 'V', cmd::ClipPaste);
    add(FCONTROL, 'A', cmd::SelectAll);
    add(0, VK_DIVIDE, cmd::SelectNone);
    add(0, VK_MULTIPLY, cmd::InvertSelection);
    add(0, VK_ADD, cmd::SelectGroup);
    add(0, VK_SUBTRACT, cmd::DeselectGroup);
    add(FALT, VK_ADD, cmd::SelectSameExt);
    add(FCONTROL | FSHIFT, 'C', cmd::CopyPaths);
    add(FCONTROL | FSHIFT, 'L', cmd::TwoPanes);
    add(FCONTROL, 'T', cmd::SplitToggle);
    add(FCONTROL, 'Q', cmd::QuickView);
    add(FCONTROL | FSHIFT, '1', cmd::ViewDetails);
    add(FCONTROL | FSHIFT, '2', cmd::ViewList);
    add(FCONTROL | FSHIFT, '3', cmd::ViewIcons);
    add(FCONTROL | FSHIFT, '4', cmd::ViewThumbnails);
    add(FCONTROL | FSHIFT, '5', cmd::ViewRecursive);
    add(FCONTROL, 'H', cmd::ShowHidden);
    add(FCONTROL | FSHIFT, 'F', cmd::Filter);
    add(FCONTROL, 'R', cmd::Refresh);
    add(FALT, VK_LEFT, cmd::GoBack);
    add(FALT, VK_RIGHT, cmd::GoForward);
    add(0, VK_BACK, cmd::GoUp);
    add(FCONTROL, VK_BACK, cmd::GoRoot);
    add(FCONTROL, 'L', cmd::FocusPath);
    add(FCONTROL, 'G', cmd::FocusPath);
    add(FCONTROL | FSHIFT, 'O', cmd::GoOtherSame);
    add(FCONTROL, 'U', cmd::SwapPanes);
    add(0, VK_TAB, cmd::NextPane);
    add(FCONTROL, '1', cmd::FocusPane1);
    add(FCONTROL, '2', cmd::FocusPane2);
    add(FCONTROL, '3', cmd::FocusPane3);
    add(FCONTROL, '4', cmd::FocusPane4);
    add(FALT, VK_F1, cmd::FocusTree);
    add(FALT, VK_F2, cmd::FocusBookmarks);
    add(FCONTROL, 'E', cmd::FocusCommandLine);
    add(FCONTROL, 'D', cmd::BookmarkAdd);
    add(FCONTROL, 'K', cmd::CompareFiles);
    add(FCONTROL | FSHIFT, 'K', cmd::CompareDirs);
    add(FCONTROL | FSHIFT, 'Y', cmd::SyncDirs);
    add(0, VK_F1, cmd::Shortcuts);
    for (int i = 0; i < 12; ++i) {
        add(FCONTROL, (WORD)(VK_F1 + i), cmd::FKeyFirst + i);
        add(FCONTROL | FSHIFT, (WORD)(VK_F1 + i), cmd::FKeyFirst + 12 + i);
    }
    accel_ = CreateAcceleratorTableW(a.data(), (int)a.size());
}

bool MainWindow::PreTranslate(MSG* msg) {
    if (!accel_ || !msg->hwnd) return false;
    // F10 (ohne Umschalt/Strg/Alt) ist eine Systemtaste (Menüleiste) – hier als "Duplizieren" verwenden
    if (msg->message == WM_SYSKEYDOWN && msg->wParam == VK_F10 && GetAncestor(msg->hwnd, GA_ROOT) == hwnd_ &&
        GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0) {
        OnCommand(cmd::Duplicate);
        return true;
    }
    if (msg->message == WM_SYSKEYUP && msg->wParam == VK_F10 && GetAncestor(msg->hwnd, GA_ROOT) == hwnd_ &&
        GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_CONTROL) >= 0)
        return true; // kein Aktivieren der Menüleiste
    if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) return false;
    if (GetAncestor(msg->hwnd, GA_ROOT) != hwnd_) return false;
    // Esc bricht den Aufbau einer PDF-/Video-Anzeige in der Dateianzeige ab (von überall im Hauptfenster)
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE && quickView_ && quickViewPane_ >= 0 &&
        panes_[quickViewPane_] && panes_[quickViewPane_]->QuickViewCancelBuild())
        return true;
    // In Textfeldern (Pfad, Befehlszeile, Umbenennen, Anzeige) keine Tasten abfangen, die dort gebraucht werden
    wchar_t cls[64] = {};
    GetClassNameW(GetFocus(), cls, 64);
    bool inEdit = _wcsicmp(cls, L"Edit") == 0 || _wcsnicmp(cls, L"RichEdit", 8) == 0 || _wcsicmp(cls, L"RICHEDIT50W") == 0;
    if (inEdit) {
        UINT vk = (UINT)msg->wParam;
        bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        bool fkey = vk >= VK_F1 && vk <= VK_F24;
        if (!ctrl && !alt && !fkey && vk != VK_TAB) return false;
        if (ctrl && !alt && (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X' || vk == 'Z' || vk == 'Y' || vk == VK_BACK ||
                             vk == VK_LEFT || vk == VK_RIGHT || vk == VK_HOME || vk == VK_END || vk == VK_DELETE ||
                             vk == VK_INSERT))
            return false;
        if (shift && !ctrl && !alt && (vk == VK_DELETE || vk == VK_INSERT)) return false;
        if (vk == VK_TAB && ListView_GetEditControl(Active().ListHwnd())) return false;
    } else if (quickView_ && quickViewPane_ >= 0 && IsChild(panes_[quickViewPane_]->Hwnd(), GetFocus())) {
        // Fokus in der Schnellansicht (z. B. Hex-Anzeige): Strg+C/Strg+A/Entf usw. gehören der Anzeige,
        // nicht den Dateien der aktiven Liste
        UINT vk = (UINT)msg->wParam;
        bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        bool fkey = vk >= VK_F1 && vk <= VK_F24;
        if (!ctrl && !alt && !fkey && vk != VK_TAB) return false;
        if (ctrl && !alt && (vk == 'A' || vk == 'C' || vk == 'F' || vk == VK_INSERT || vk == VK_HOME || vk == VK_END))
            return false;   // Strg+F: Suche in der Archivliste
    } else if (GetFocus() == tree_.Hwnd()) {
        // Ziffernblock +, -, * klappen im Verzeichnisbaum auf/zu (statt Gruppe markieren/abwählen/umkehren)
        UINT vk = (UINT)msg->wParam;
        if ((vk == VK_ADD || vk == VK_SUBTRACT || vk == VK_MULTIPLY) && GetKeyState(VK_CONTROL) >= 0 &&
            GetKeyState(VK_MENU) >= 0)
            return false;
    }
    return TranslateAcceleratorW(hwnd_, accel_, msg) != 0;
}

// ===================== Funktionstastenleiste =====================

void MainWindow::BuildFKeyCells() {
    fkeys_.clear();
    if (fkeyMode_ == 0) {
        const struct {
            const wchar_t* k;
            const wchar_t* l;
            int c;
        } std[] = {{L"F2", L"Umbenennen", cmd::Rename},        {L"F3", L"Suchen", cmd::FindFiles},
                   {L"F4", L"Bearbeiten", cmd::Edit},          {L"F5", L"Aktualisieren", cmd::Refresh},
                   {L"F6", L"Markierung umkehren", cmd::InvertSelection},
                   {L"F7", L"Vergleich", cmd::CompareDirs},    {L"F8", L"Neues Verzeichnis", cmd::NewFolder},
                   {L"F9", L"Neue Datei", cmd::NewFile},       {L"F10", L"Duplizieren", cmd::Duplicate},
                   {L"F11", L"Anzeigen", cmd::View}};
        const wchar_t* shortLabels[] = {L"Umben.", L"Suchen", L"Bearb.", L"Aktual.", L"Umkehren",
                                        L"Vergl.", L"Neues Verz.", L"Neue Datei", L"Dupliz.", L"Anzeigen"};
        int si = 0;
        for (auto& s : std) fkeys_.push_back({s.k, s.l, shortLabels[si++], s.c, {}});
    } else {
        int base = fkeyMode_ == 1 ? 0 : 12;
        for (int i = 0; i < 12; ++i) {
            const FunctionKey& k = fkeyDefs_[base + i];
            std::wstring key = (fkeyMode_ == 1 ? L"Strg+F" : L"S+Strg+F") + std::to_wstring(i + 1);
            std::wstring label = k.IsEmpty() ? L"–" : (k.label.empty() ? PathStem(k.program) : k.label);
            fkeys_.push_back({key, label, label, cmd::FKeyFirst + base + i, {}});
        }
    }
    Layout();
}

// ===================== Layout =====================

bool MainWindow::PaneVisible(int i) const {
    int col = i % 2;
    bool bottom = i >= 2;
    if (col == 1 && !twoPanes_) return false;
    if (bottom && !split_[col]) return false;
    return true;
}

void MainWindow::ApplyPaneVisibility() {
    for (int i = 0; i < 4; ++i) panes_[i]->Show(PaneVisible(i));
    for (int col = 0; col < 2; ++col) {
        SplitButton b = split_[col] ? SplitButton::Unsplit : SplitButton::Split;
        panes_[col]->SetSplitButton(b);
        panes_[col + 2]->SetSplitButton(SplitButton::Unsplit);
    }
    if (!PaneVisible(active_)) active_ = active_ % 2 == 1 && !twoPanes_ ? 0 : active_ % 2;
    if (!PaneVisible(active_)) active_ = 0;
    if (quickView_ && (quickViewPane_ < 0 || !PaneVisible(quickViewPane_) || OtherIndex(active_) != quickViewPane_))
        UpdateQuickView();
    Layout();
}

void MainWindow::Layout() {
    if (!hwnd_ || !panes_[3]) return; // erst wenn alle vier Listen erzeugt sind
    RECT rc;
    GetClientRect(hwnd_, &rc);
    auto S = [&](int v) { return DpiScale(hwnd_, v); };
    int top = 0, bottom = rc.bottom;
    // Werkzeugleiste
    if (showToolbar_) {
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
        DWORD sz = (DWORD)SendMessageW(toolbar_, TB_GETBUTTONSIZE, 0, 0);
        int th = HIWORD(sz) + S(4);
        SetWindowPos(toolbar_, nullptr, 0, 0, rc.right, th, SWP_NOZORDER | SWP_SHOWWINDOW);
        top = th;
    } else {
        ShowWindow(toolbar_, SW_HIDE);
    }
    // Statusleiste
    if (showStatus_) {
        SendMessageW(status_, WM_SIZE, 0, 0);
        ShowWindow(status_, SW_SHOW);
        RECT sr;
        GetWindowRect(status_, &sr);
        bottom -= sr.bottom - sr.top;
        int parts[2] = {std::max<int>(100, rc.right - S(260)), -1};
        SendMessageW(status_, SB_SETPARTS, 2, (LPARAM)parts);
    } else {
        ShowWindow(status_, SW_HIDE);
    }
    // Funktionstastenleiste
    if (showFKeys_) {
        int h = S(kFKeyH);
        rcFKeys_ = {0, bottom - h, rc.right, bottom};
        bottom -= h;
        // Breiten nach Textlänge verteilen, damit lange Beschriftungen nicht abgeschnitten werden
        int n = (int)fkeys_.size();
        std::vector<int> need(n);
        int total = 0;
        HDC dc = GetDC(hwnd_);
        HGDIOBJ of = SelectObject(dc, App::UIFont());
        int avail0 = rc.right - S(4);
        for (int pass = 0; pass < 2; ++pass) {
            fkeyShort_ = pass == 1;
            total = 0;
            for (int i = 0; i < n; ++i) {
                const std::wstring& lab = fkeyShort_ ? fkeys_[i].shortLabel : fkeys_[i].label;
                SIZE a{}, b{};
                GetTextExtentPoint32W(dc, fkeys_[i].key.c_str(), (int)fkeys_[i].key.size(), &a);
                GetTextExtentPoint32W(dc, lab.c_str(), (int)lab.size(), &b);
                need[i] = a.cx + b.cx + S(16);
                total += need[i];
            }
            if (total <= avail0) break;  // Langtexte passen
        }
        SelectObject(dc, of);
        ReleaseDC(hwnd_, dc);
        int avail = rc.right - S(4);
        int x = S(2);
        for (int i = 0; i < n; ++i) {
            int w = total > 0 ? (int)((long long)avail * need[i] / total) : 0;
            if (i == n - 1) w = rc.right - S(2) - x;
            fkeys_[i].rc = {x, rcFKeys_.top + S(2), x + w - S(2), rcFKeys_.bottom - S(2)};
            x += w;
        }
    } else {
        rcFKeys_ = {};
    }
    // Befehlszeile
    if (showCmdLine_) {
        int h = S(kCmdH);
        rcCmd_ = {0, bottom - h, rc.right, bottom};
        bottom -= h;
        int labelW = std::min<int>(S(320), rc.right / 3);
        SetWindowPos(cmdLabel_, nullptr, S(4), rcCmd_.top + S(3), labelW, h - S(6), SWP_NOZORDER | SWP_SHOWWINDOW);
        RECT cr;
        GetWindowRect(cmdCombo_, &cr);
        int ch = cr.bottom - cr.top;
        SetWindowPos(cmdCombo_, nullptr, S(8) + labelW, rcCmd_.top + (h - ch) / 2, rc.right - labelW - S(12), S(300),
                     SWP_NOZORDER | SWP_SHOWWINDOW);
        if (cmdEdit_ && GetFocus() != cmdEdit_) {
            int len = GetWindowTextLengthW(cmdEdit_);
            SendMessageW(cmdEdit_, EM_SETSEL, len, len);
        }
    } else {
        rcCmd_ = {};
        ShowWindow(cmdLabel_, SW_HIDE);
        ShowWindow(cmdCombo_, SW_HIDE);
    }
    rcContent_ = {0, top, rc.right, bottom};
    int sw = S(kSplitterW);
    int capH = S(kCaptionH);
    int width = rc.right;
    int treeW = std::min(S(treeWidth_), std::max(S(60), width / 3));
    int bookW = std::min(S(bookmarkWidth_), std::max(S(50), width / 4));
    int x = 0;
    rcTreeCap_ = {x, top, x + treeW, top + capH};
    SetWindowPos(tree_.Hwnd(), nullptr, x, top + capH, treeW, std::max(0, bottom - top - capH), SWP_NOZORDER);
    x += treeW;
    rcSplitTree_ = {x, top, x + sw, bottom};
    x += sw;
    rcBookCap_ = {x, top, x + bookW, top + capH};
    SetWindowPos(bookmarks_.Hwnd(), nullptr, x, top + capH, bookW, std::max(0, bottom - top - capH), SWP_NOZORDER);
    x += bookW;
    rcSplitBook_ = {x, top, x + sw, bottom};
    x += sw;
    int areaL = x, areaR = rc.right;
    int areaW = std::max(0, areaR - areaL);
    if (twoPanes_) {
        int leftW = std::max(S(80), (int)((areaW - sw) * colRatio_));
        leftW = std::min(leftW, std::max(S(80), areaW - sw - S(80)));
        rcColumns_[0] = {areaL, top, areaL + leftW, bottom};
        rcSplitCol_ = {areaL + leftW, top, areaL + leftW + sw, bottom};
        rcColumns_[1] = {areaL + leftW + sw, top, areaR, bottom};
    } else {
        rcColumns_[0] = {areaL, top, areaR, bottom};
        rcSplitCol_ = {};
        rcColumns_[1] = {};
    }
    for (int col = 0; col < 2; ++col) {
        if (col == 1 && !twoPanes_) {
            rcSplitRow_[1] = {};
            continue;
        }
        RECT c = rcColumns_[col];
        if (split_[col]) {
            int h = c.bottom - c.top;
            int topH = std::clamp((int)((h - sw) * rowRatio_[col]), S(80), std::max(S(80), h - sw - S(80)));
            panes_[col]->SetBounds({c.left, c.top, c.right, c.top + topH});
            rcSplitRow_[col] = {c.left, c.top + topH, c.right, c.top + topH + sw};
            panes_[col + 2]->SetBounds({c.left, c.top + topH + sw, c.right, c.bottom});
        } else {
            panes_[col]->SetBounds(c);
            rcSplitRow_[col] = {};
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    HBRUSH face = GetSysColorBrush(COLOR_BTNFACE);
    // Splitter, Überschriften, Funktionstasten, Befehlszeile
    for (const RECT* r : {&rcSplitTree_, &rcSplitBook_, &rcSplitCol_, &rcSplitRow_[0], &rcSplitRow_[1], &rcCmd_})
        FillRect(dc, r, face);
    // dünne, unauffällige Trennlinie zwischen linkem und rechtem Listenbereich
    if (rcSplitCol_.right > rcSplitCol_.left) {
        HPEN pen = CreatePen(PS_SOLID, 1, Blend(GetSysColor(COLOR_BTNFACE), RGB(0, 0, 0), 45));
        HGDIOBJ op = SelectObject(dc, pen);
        int lx = (rcSplitCol_.left + rcSplitCol_.right) / 2;
        MoveToEx(dc, lx, rcSplitCol_.top, nullptr);
        LineTo(dc, lx, rcSplitCol_.bottom);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    auto S = [&](int v) { return DpiScale(hwnd_, v); };
    HGDIOBJ oldFont = SelectObject(dc, App::UIFont());
    SetBkMode(dc, TRANSPARENT);
    auto caption = [&](const RECT& r, const wchar_t* text, SHSTOCKICONID icon) {
        FillRect(dc, &r, face);
        SHSTOCKICONINFO sii{sizeof(sii)};
        int iconSize = S(16);
        int tx = r.left + S(4);
        if (SUCCEEDED(SHGetStockIconInfo(icon, SHGSI_ICON | SHGSI_SMALLICON, &sii))) {
            DrawIconEx(dc, tx, (r.top + r.bottom - iconSize) / 2, sii.hIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            DestroyIcon(sii.hIcon);
            tx += iconSize + S(4);
        }
        RECT tr{tx, r.top, r.right - S(2), r.bottom};
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, r.left, r.bottom - 1, nullptr);
        LineTo(dc, r.right, r.bottom - 1);
        SelectObject(dc, op);
        DeleteObject(pen);
    };
    caption(rcTreeCap_, L"Verzeichnisse", SIID_DRIVEFIXED);
    caption(rcBookCap_, L"Lesezeichen", SIID_FOLDER);
    if (showFKeys_) {
        FillRect(dc, &rcFKeys_, face);
        for (size_t i = 0; i < fkeys_.size(); ++i) {
            RECT r = fkeys_[i].rc;
            UINT state = DFCS_BUTTONPUSH | ((int)i == fkeyPressed_ ? DFCS_PUSHED : 0);
            DrawFrameControl(dc, &r, DFC_BUTTON, state);
            RECT kr = r;
            kr.left += S(4);
            SetTextColor(dc, RGB(0, 70, 160));
            SIZE ks{};
            GetTextExtentPoint32W(dc, fkeys_[i].key.c_str(), (int)fkeys_[i].key.size(), &ks);
            DrawTextW(dc, fkeys_[i].key.c_str(), -1, &kr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
            RECT lr = r;
            lr.left += S(4) + ks.cx + S(5);
            lr.right -= S(2);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            const std::wstring& lab = fkeyShort_ ? fkeys_[i].shortLabel : fkeys_[i].label;
            DrawTextW(dc, lab.c_str(), -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    SelectObject(dc, oldFont);
}

MainWindow::Splitter MainWindow::HitSplitter(POINT pt) const {
    if (PtInRect(&rcSplitTree_, pt)) return Splitter::Tree;
    if (PtInRect(&rcSplitBook_, pt)) return Splitter::Bookmarks;
    if (twoPanes_ && PtInRect(&rcSplitCol_, pt)) return Splitter::Columns;
    if (split_[0] && PtInRect(&rcSplitRow_[0], pt)) return Splitter::RowLeft;
    if (twoPanes_ && split_[1] && PtInRect(&rcSplitRow_[1], pt)) return Splitter::RowRight;
    return Splitter::None;
}

// ===================== Aktive Liste, Split, Schnellansicht =====================

int MainWindow::OtherIndex(int i) const {
    int col = i % 2;
    bool bottom = i >= 2;
    if (twoPanes_) {
        int other = 1 - col;
        int cand = other + (bottom ? 2 : 0);
        if (PaneVisible(cand)) return cand;
        return other;
    }
    // Eine Spalte: andere Zeile derselben Spalte (bei Split)
    if (split_[col]) return bottom ? col : col + 2;
    return -1;
}

FilePane* MainWindow::Other() {
    int o = OtherIndex(active_);
    return o >= 0 && PaneVisible(o) ? panes_[o].get() : nullptr;
}

std::wstring MainWindow::OtherDir() {
    // Nur gewöhnliche Verzeichnisse (Ziel für Entpacken, Teilen usw.); FTP/Netzwerkebenen -> ""
    FilePane* o = Other();
    return o && !o->IsVirtual() && !o->IsSpecialList() ? o->Dir() : L"";
}

void MainWindow::SetActive(int index, bool focus) {
    if (index < 0 || index > 3 || !PaneVisible(index)) return;
    if (quickView_ && index == quickViewPane_) return; // Anzeigefenster kann nicht aktiv werden
    bool changed = active_ != index;
    active_ = index;
    for (int i = 0; i < 4; ++i) panes_[i]->SetActiveLook(i == active_);
    if (focus) panes_[active_]->FocusList();
    if (changed && quickView_) UpdateQuickView();
    UpdateTitle();
    UpdateCmdLabel();
    UpdateStatusBar();
    if (App::Opt().treeFollowsPane) tree_.SelectPath(Active().Dir());
}

void MainWindow::SetSplit(int column, bool on) {
    if (split_[column] == on) return;
    split_[column] = on;
    if (on) {
        // Untere Liste zeigt zunächst dasselbe Verzeichnis
        FilePane& bottom = *panes_[column + 2];
        if (bottom.Dir().empty() || (!bottom.IsVirtual() && !DirExists(bottom.Dir())))
            bottom.Navigate(panes_[column]->Dir(), L"", true, false);
    } else if (active_ == column + 2) {
        active_ = column;
    }
    if (quickView_) SetQuickView(false);
    ApplyPaneVisibility();
    SetActive(on ? column + 2 : column, true);
}

void MainWindow::SetTwoPanes(bool on) {
    if (twoPanes_ == on) return;
    if (quickView_) SetQuickView(false);
    twoPanes_ = on;
    if (!on && active_ % 2 == 1) active_ = active_ - 1;
    ApplyPaneVisibility();
    SetActive(active_, true);
}

void MainWindow::SetQuickView(bool on, bool focus) {
    if (on) {
        int o = OtherIndex(active_);
        if (o < 0 || !PaneVisible(o)) {
            // Kein anderes Fenster: eigenes Anzeigefenster
            std::wstring p = Active().FocusedPath();
            if (!p.empty()) OpenViewerWindow(p);
            return;
        }
        quickView_ = true;
        quickViewPane_ = o;
        panes_[o]->SetQuickView(true);
        UpdateQuickView();
    } else {
        quickView_ = false;
        if (quickViewPane_ >= 0) panes_[quickViewPane_]->SetQuickView(false);
        quickViewPane_ = -1;
    }
    if (focus) Active().FocusList();
}

void MainWindow::UpdateQuickView() {
    if (!quickView_) return;
    int o = OtherIndex(active_);
    if (o != quickViewPane_) {
        if (quickViewPane_ >= 0) panes_[quickViewPane_]->SetQuickView(false);
        if (o < 0 || !PaneVisible(o)) {
            quickView_ = false;
            quickViewPane_ = -1;
            return;
        }
        quickViewPane_ = o;
        panes_[o]->SetQuickView(true);
    }
    KillTimer(hwnd_, TIMER_QUICKVIEW);
    SetTimer(hwnd_, TIMER_QUICKVIEW, 120, nullptr);
}

void MainWindow::UpdateTitle() {
    std::wstring t = L"QFiles – " + LocationDisplay(Active().Dir());
    SetWindowTextW(hwnd_, t.c_str());
    UpdateViewButtons();
}

// Ansichtssymbole der Werkzeugleiste: Ansicht der aktiven Liste eingedrückt zeigen
// Archivliste der Dateianzeige mit dem Tastaturfokus: Die Ansichtssymbole wirken dann auf sie
HWND MainWindow::FocusedArchiveView() const {
    HWND f = GetFocus();
    if (!f || !IsChild(hwnd_, f)) return nullptr;
    return ArchiveViewFromWindow(f);
}

void MainWindow::UpdateViewButtons() {
    if (!toolbar_ || !panes_[active_]) return;
    HWND arc = FocusedArchiveView();
    int cur = cmd::ViewDetails + (arc ? ArchiveViewGetMode(arc) : (int)Active().View());
    bool changed = false;
    for (int id = cmd::ViewDetails; id <= cmd::ViewRecursive; ++id) {
        bool want = id == cur;
        if (!!SendMessageW(toolbar_, TB_ISBUTTONCHECKED, id, 0) != want) {
            SendMessageW(toolbar_, TB_CHECKBUTTON, id, MAKELONG(want, 0));
            changed = true;
        }
    }
    if (changed) InvalidateRect(toolbar_, nullptr, TRUE);
}

void MainWindow::UpdateCmdLabel() {
    std::wstring t = LocationDisplay(Active().Dir()) + L">";
    SetWindowTextW(cmdLabel_, t.c_str());
}

void MainWindow::UpdateStatusBar() {
    FilePane& a = Active();
    std::wstring left;
    std::wstring p = a.FocusedPath();
    DirEntry fe;
    if (a.IsRecycleBin()) {
        left = a.RecycleStatusText();
    } else if (a.IsVirtual() && a.FocusedEntry(fe)) {
        left = fe.name;
        if (!a.IsNetworkLevel()) {
            if (!fe.IsDir()) left += L"   " + FormatSizeBytes(fe.size) + L" Bytes";
            left += L"   " + FormatFileTime(fe.modified, true);
            std::wstring mode = RemoteModeText(fe);   // FTP/SFTP: Unix-Rechte
            if (!mode.empty()) left += L"   " + mode;
        }
    } else if (!p.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &d)) {
            uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
            left = PathFileName(p);
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) left += L"   " + FormatSizeBytes(size) + L" Bytes";
            left += L"   " + FormatFileTime(d.ftLastWriteTime, true) + L"   " + FormatAttributes(d.dwFileAttributes);
        }
    }
    SendMessageW(status_, SB_SETTEXTW, 0, (LPARAM)left.c_str());
    std::wstring right = L"Liste " + std::to_wstring(active_ + 1);
    if (quickView_) right += L" · Dateianzeige in Liste " + std::to_wstring(quickViewPane_ + 1);
    SendMessageW(status_, SB_SETTEXTW, 1, (LPARAM)right.c_str());
}

// ===================== IPaneHost =====================

void MainWindow::OnPaneActivated(FilePane* p) {
    // Kopfleiste der Liste mit der Dateianzeige betätigt (Laufwerk, Verzeichnisfeld, Schaltflächen): erst die Dateianzeige
    // beenden (wie F11), dann wird diese Liste aktiv. Den Fokus setzt der Aufrufer (z. B. bleibt er im Verzeichnisfeld).
    if (quickView_ && p->Index() == quickViewPane_) SetQuickView(false, false);
    if (p->Index() != active_) SetActive(p->Index(), false);
}

void MainWindow::OnPaneDirChanged(FilePane* p) {
    if (p->Index() == active_) {
        UpdateTitle();
        UpdateCmdLabel();
        if (App::Opt().treeFollowsPane) tree_.SelectPath(p->Dir());
    }
}

void MainWindow::OnPaneFocusItemChanged(FilePane* p) {
    if (p->Index() != active_) return;
    UpdateStatusBar();
    if (quickView_) UpdateQuickView();
}

void MainWindow::OnPaneSplitButton(FilePane* p) {
    int col = p->Index() % 2;
    SetSplit(col, !split_[col]);
}

void MainWindow::OnPaneBookmarkButton(FilePane* p) {
    SetActive(p->Index(), false);
    AddBookmarkFor(*p);
}

void MainWindow::AddBookmarkFor(FilePane& p) {
    std::wstring dir = p.Dir();
    if (dir.empty()) return;
    if (p.IsRemote()) {
        // FTP/SFTP: Lesezeichen über den Zugangsdialog (mit diesem Verzeichnis als Start)
        RemoteAccess a = LoadRemoteAccess(dir);
        a.name = LocationFileName(dir);
        if (EditRemoteAccess(hwnd_, a, true)) bookmarks_.Append(a.name, a.url.ToString());
        return;
    }
    std::wstring name = IsVirtualLocation(dir) ? LocationFileName(dir) : LastPathElement(dir);
    std::wstring hint;
    int existing = bookmarks_.FindPath(dir);
    if (existing >= 0)
        hint = L"Hinweis: Dieses Verzeichnis steht bereits als „" + bookmarks_.Get()[existing].name +
               L"“ in der Lesezeichenliste.";
    if (!InputBox(hwnd_, L"Lesezeichen hinzufügen", L"Name des Lesezeichens für „" + LocationDisplay(dir) + L"“:", name, hint))
        return;
    name = Trim(name);
    if (name.empty()) name = IsVirtualLocation(dir) ? LocationFileName(dir) : LastPathElement(dir);
    bookmarks_.Append(name, dir);
}

void MainWindow::OnPaneOpenItem(FilePane* p) { OpenFileItem(*p); }

void MainWindow::OpenFileItem(FilePane& p) {
    std::wstring path = p.FocusedPath();
    if (path.empty()) return;
    if (p.IsRemote()) {
        RemoteOpenFocused(p, RemoteOpenMode::Open);
        return;
    }
    const Options& o = App::Opt();
    std::wstring name = PathFileName(path);
    if (IsArchive(name)) {
        std::wstring target = OtherDir();
        ShowArchive(hwnd_, path, target.empty() ? PathParent(path) : target);
        return;
    }
    if (o.enterOpensEditorForText && MatchAnyPattern(o.textExtensions, name)) {
        OpenTextEditor(path);
        return;
    }
    ShellOpen(hwnd_, path, L"", p.WorkingDir());
}

void MainWindow::OnPaneContextMenu(FilePane* p, POINT pt, bool onItems) {
    SetActive(p->Index(), false);
    if (p->IsVirtual()) {
        ShowVirtualContextMenu(*p, pt, onItems);
        return;
    }
    if (p->IsRecycleBin()) {
        // Papierkorb: eigene Befehle statt des Explorer-Menüs (die Elemente heißen dort $R…)
        HMENU m = CreatePopupMenu();
        if (onItems && !p->SelectedOrFocusedNames().empty()) {
            AddItem(m, cmd::RecycleRestore, L"&Wiederherstellen");
            if (!OtherDir().empty()) {
                AddItem(m, cmd::Move, L"Wiederherstellen in die &andere Liste\tUmschalt+F6");
                AddItem(m, cmd::Copy, L"&Kopieren in die andere Liste\tUmschalt+F5");
            }
            if (!p->FocusedIsDir()) AddItem(m, cmd::View, L"A&nzeigen\tF11");
            AddItem(m, cmd::DeletePermanent, L"&Endgültig löschen\tEntf");
            AddSep(m);
        }
        AddItem(m, cmd::RecycleEmpty, L"Papierkorb &leeren…");
        AddItem(m, cmd::Refresh, L"&Aktualisieren\tF5");
        UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr);
        DestroyMenu(m);
        if (id) OnCommand((int)id);
        return;
    }
    if (p->IsFavorites() && !onItems) {
        // Favoriten: kein Verzeichnis – nur Aktualisieren
        HMENU m = CreatePopupMenu();
        AddItem(m, cmd::Refresh, L"&Aktualisieren\tF5");
        UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr);
        DestroyMenu(m);
        if (id) OnCommand((int)id);
        return;
    }
    HMENU extra = CreatePopupMenu();
    std::vector<std::wstring> names;
    if (onItems) {
        names = p->SelectedOrFocusedNames();
        bool file = !p->FocusedIsDir();
        if (file) {
            AddItem(extra, cmd::View, L"&Anzeigen\tF11");
            AddItem(extra, cmd::Edit, L"&Bearbeiten\tF4");
        }
        AddItem(extra, cmd::Copy, L"&Kopieren in andere Liste\tUmschalt+F5");
        AddItem(extra, cmd::Move, L"&Verschieben in andere Liste\tUmschalt+F6");
        if (!p->IsFavorites()) AddItem(extra, cmd::Duplicate, L"D&uplizieren…\tF10");
        AddItem(extra, cmd::Rename, L"&Umbenennen…\tF2");
        AddItem(extra, cmd::Attributes, L"Attribute/&Datum ändern…");
        if (file && IsArchive(p->FocusedName())) AddItem(extra, cmd::OpenArchive, L"Archiv anzeigen/en&tpacken…");
        if (!p->IsFavorites()) AddItem(extra, cmd::CreateZip, L"&ZIP-Archiv erstellen…");
        // Favoriten: nur Dateien; in der Favoritenliste stattdessen „Aus Favoriten entfernen“
        if (p->IsFavorites()) {
            AddItem(extra, cmd::FavoriteRemove, L"Aus &Favoriten entfernen");
            AddItem(extra, cmd::FavoriteGotoDir, L"Zum &Verzeichnis der Datei wechseln");
        } else {
            bool allFiles = !names.empty(), allFav = !names.empty();
            for (auto& n : names) {
                std::wstring full = PathCombine(p->Dir(), n);
                if (!CanBeFavorite(full)) allFiles = false;
                if (!IsFavorite(full)) allFav = false;
            }
            if (allFiles) {
                if (allFav) AppendMenuW(extra, MF_STRING | MF_CHECKED | MF_GRAYED, cmd::FavoriteAdd, L"Als &Favorit gekennzeichnet");
                else AddItem(extra, cmd::FavoriteAdd, L"Als &Favorit kennzeichnen");
            }
        }
    } else {
        AddItem(extra, cmd::NewFolder, L"Neues &Verzeichnis…\tF8");
        AddItem(extra, cmd::NewFile, L"&Neue Datei…\tF9");
        AddItem(extra, cmd::NewTextFile, L"Neue &Textdatei…\tUmschalt+F9");
        if (ClipboardHasFiles()) AddItem(extra, cmd::ClipPaste, L"&Einfügen\tStrg+V");
        AddItem(extra, cmd::Refresh, L"&Aktualisieren\tF5");
        AddItem(extra, cmd::BookmarkAdd, L"Den &Lesezeichen hinzufügen\tStrg+D");
    }
    // Mit anderer Liste vergleichen (Beyond Compare): einzelne Datei, gleichnamige Datei im Verzeichnis einer anderen
    // sichtbaren Liste; mehrere Listen unterscheidbar am Symbol (Richtung der anderen Liste)
    std::vector<HBITMAP> menuBitmaps;
    if (onItems && names.size() == 1 && !p->FocusedIsDir()) {
        std::wstring mine = PathCombine(p->Dir(), names[0]);
        std::vector<int> others;
        if (FileExists(mine) && !BeyondComparePath().empty())
            for (int i = 0; i < 4; ++i) {
                if (i == p->Index() || !PaneVisible(i) || (quickView_ && i == quickViewPane_)) continue;
                FilePane& o = *panes_[i];
                if (o.IsVirtual() || o.IsSpecialList() || o.Dir().empty()) continue;
                std::wstring other = PathCombine(o.Dir(), PathFileName(mine));
                if (FileExists(other) && !EqualsI(CanonicalPath(other), CanonicalPath(mine))) others.push_back(i);
            }
        if (!others.empty()) AppendMenuW(extra, MF_SEPARATOR, 0, nullptr);
        int iconSize = DpiScale(hwnd_, 16);
        for (int i : others) {
            int dx = i % 2 - p->Index() % 2, dy = i / 2 - p->Index() / 2;
            std::wstring text = L"Mit anderer Liste &vergleichen";
            if (others.size() > 1) text += L" (" + DirectionText(dx, dy) + L")";
            HBITMAP bmp = CompareDirectionBitmap(iconSize, dx, dy);
            menuBitmaps.push_back(bmp);
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP;
            mi.wID = cmd::CompareWithPane0 + i;
            mi.dwTypeData = text.data();
            mi.hbmpItem = bmp;
            InsertMenuItemW(extra, GetMenuItemCount(extra), TRUE, &mi);
        }
    }
    bool rename = false;
    int id = ShowShellContextMenu(hwnd_, p->Dir(), names, pt, extra, &rename);
    DestroyMenu(extra);
    for (HBITMAP b : menuBitmaps) DeleteObject(b);
    if (rename) p->BeginRename();
    else if (id) OnCommand(id);
    else ReloadVisible();
}

void MainWindow::OnPaneFilesDropped(FilePane* p) { ReloadVisible(); }

void MainWindow::OnPaneLayoutChanged(FilePane*) { ScheduleStateSave(); }


// Zwischenablage geändert: ausgeschnittene Dateien neu bestimmen und die Listen neu zeichnen
void MainWindow::OnClipboardChanged() {
    bool had = ClipboardHasCut();
    if (!ClipboardRefreshCut(hwnd_)) {
        SetTimer(hwnd_, TIMER_CLIPBOARD, 250, nullptr);   // gerade von einem anderen Programm geöffnet
        return;
    }
    if (!had && !ClipboardHasCut()) return;
    for (auto& p : panes_)
        if (p) p->RedrawList();
}

// Fensterposition und -größe, Ansichten und Spaltenbreiten werden nicht nur beim Beenden gespeichert, sondern auch
// kurz nach jeder Änderung – so bleiben sie auch erhalten, wenn QFiles nicht regulär beendet wird.
void MainWindow::ScheduleStateSave() {
    if (hwnd_ && App::Opt().saveOnExit) SetTimer(hwnd_, TIMER_SAVESTATE, 1500, nullptr);
}

bool MainWindow::IsPaneActive(const FilePane* p) const { return p->Index() == active_; }

// ===================== Dienste =====================

void MainWindow::NavigateActive(const std::wstring& dir, const std::wstring& select) {
    if (Active().Navigate(dir, select)) {
        if (!select.empty()) Active().FocusName(select);
        SetForegroundWindow(hwnd_);
        Active().FocusList();
    }
}

void MainWindow::NavigateToFile(const std::wstring& path) {
    if (DirExists(path) && !FileExists(path)) {
        NavigateActive(path, L"");
        return;
    }
    NavigateActive(PathParent(path), PathFileName(path));
}

void MainWindow::MarkSearchResults(const std::vector<std::wstring>& dirs, const std::vector<std::wstring>& paths,
                                   bool invert, bool subdirs) {
    std::wstring target;
    for (auto& d : dirs)
        if (EqualsI(NormalizeDir(d), NormalizeDir(Active().Dir()))) target = Active().Dir();
    if (target.empty() && !dirs.empty()) target = dirs.front();
    if (target.empty()) return;
    if (!EqualsI(NormalizeDir(target), NormalizeDir(Active().Dir())) && !Active().Navigate(target, L""))
        return;
    Active().MarkPaths(paths, invert, subdirs);
    SetForegroundWindow(hwnd_);
    Active().FocusList();
}

void MainWindow::RefreshAll() {
    ReloadVisible();
}

// Alle sichtbaren Listen außer der aktiven neu einlesen (die aktive wurde bereits gezielt aktualisiert)
void MainWindow::ReloadOthers() {
    for (int i = 0; i < 4; ++i)
        if (i != active_ && PaneVisible(i) && !(quickView_ && i == quickViewPane_)) panes_[i]->Reload();
}

void MainWindow::ReloadVisible() {
    for (int i = 0; i < 4; ++i)
        if (PaneVisible(i) && !(quickView_ && i == quickViewPane_)) panes_[i]->Reload();
    bookmarks_.UpdateRecycleIcon();
    if (!Active().Dir().empty()) tree_.RefreshPath(Active().Dir());
}

PaneContext MainWindow::ActiveContext() const {
    PaneContext c = Active().Context();
    int o = OtherIndex(active_);
    if (o >= 0 && PaneVisible(o)) c.otherDir = panes_[o]->Dir();
    return c;
}

void MainWindow::ApplyOptionsAll() {
    for (auto& p : panes_) p->ApplyOptions();
    tree_.ApplyOptions();
    bookmarks_.ApplyFont();
    if (App::Opt().treeFollowsPane) tree_.SelectPath(Active().Dir());
}

MainWindow::FocusArea MainWindow::CurrentFocusArea() const {
    HWND f = GetFocus();
    if (!f) return FocusArea::Other;
    if (f == tree_.Hwnd() || IsChild(tree_.Hwnd(), f)) return FocusArea::Tree;
    if (f == bookmarks_.Hwnd() || IsChild(bookmarks_.Hwnd(), f)) return FocusArea::Bookmarks;
    if (f == cmdEdit_ || f == cmdCombo_) return FocusArea::CommandLine;
    return FocusArea::Pane;
}

// ===================== Befehle =====================

void MainWindow::CopyOrMove(bool move) {
    FilePane& a = Active();
    auto paths = a.SelectedOrFocusedPaths();
    if (paths.empty()) return;
    std::wstring target = OtherDir();
    if (target.empty()) target = a.Dir();
    std::wstring what = paths.size() == 1 ? (L"„" + PathFileName(paths[0]) + L"“") : (std::to_wstring(paths.size()) + L" Elemente");
    std::wstring prompt = what + (move ? L" verschieben nach:" : L" kopieren nach:");
    if (!InputBox(hwnd_, move ? L"Verschieben" : L"Kopieren", prompt, target)) return;
    target = Trim(target);
    if (target.size() >= 2 && target.front() == L'"' && target.back() == L'"') target = target.substr(1, target.size() - 2);
    if (target.empty()) return;
    // Relative Angaben beziehen sich auf das Verzeichnis der aktiven Liste (nicht auf das Arbeitsverzeichnis des Prozesses)
    if (!(target.size() >= 2 && (target[1] == L':' || StartsWithI(target, L"\\\\")))) {
        if (target.front() == L'\\') target = PathCombine(PathRoot(a.Dir()), target.substr(1));
        else target = PathCombine(a.Dir(), target);
    }
    target = NormalizeDir(target);
    if (!DirExists(target)) {
        if (!MsgConfirm(hwnd_, L"Das Zielverzeichnis „" + target + L"“ existiert nicht. Anlegen?")) return;
        if (SHCreateDirectoryExW(hwnd_, target.c_str(), nullptr) != ERROR_SUCCESS) {
            MsgError(hwnd_, L"Das Verzeichnis kann nicht angelegt werden.");
            return;
        }
    }
    if (move) {
        MoveItems(hwnd_, paths, target);
        // Verschobene Favoriten nachführen
        for (auto& p : paths)
            if (IsFavorite(p) && !PathExists(p)) {
                std::wstring moved = PathCombine(target, PathFileName(p));
                if (FileExists(moved)) RenameFavorite(p, moved);
            }
    } else {
        CopyItems(hwnd_, paths, target);
    }
    a.SelectNone();
    ReloadVisible();
}

void MainWindow::DeleteSelection(bool permanent) {
    FocusArea area = CurrentFocusArea();
    if (area == FocusArea::Bookmarks) {
        bookmarks_.DeleteSelected();
        return;
    }
    if (area == FocusArea::Tree) {
        std::wstring p = tree_.SelectedPath();
        if (p.empty() || IsRootPath(p)) return;
        if (DeleteItems(hwnd_, {p}, !permanent && App::Opt().useRecycleBin, true)) {
            tree_.RefreshPath(PathParent(p));
            ReloadVisible();
        }
        return;
    }
    FilePane& a = Active();
    auto paths = a.SelectedOrFocusedPaths();
    if (paths.empty()) return;
    bool recycle = !permanent && App::Opt().useRecycleBin;
    bool confirm = permanent || App::Opt().confirmDelete;
    DeleteItems(hwnd_, paths, recycle, confirm);
    ReloadVisible();
}

void MainWindow::RunCommandLine() {
    std::wstring cmdText = Trim(GetWindowTextStr(cmdCombo_));
    if (cmdText.empty()) return;
    // Verlauf
    cmdHistory_.erase(std::remove(cmdHistory_.begin(), cmdHistory_.end(), cmdText), cmdHistory_.end());
    cmdHistory_.insert(cmdHistory_.begin(), cmdText);
    if (cmdHistory_.size() > 50) cmdHistory_.resize(50);
    SendMessageW(cmdCombo_, CB_RESETCONTENT, 0, 0);
    for (auto& s : cmdHistory_) SendMessageW(cmdCombo_, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SetWindowTextW(cmdCombo_, L"");
    FilePane& a = Active();
    // "cd Pfad" bzw. "X:" wechseln das Verzeichnis der aktiven Liste
    std::wstring lower = ToLower(cmdText);
    if (lower == L"cd" || StartsWithI(cmdText, L"cd ") || StartsWithI(cmdText, L"cd\\") || StartsWithI(cmdText, L"chdir ")) {
        std::wstring arg = Trim(cmdText.substr(lower.find_first_of(L" \\") == std::wstring::npos ? 2 : (StartsWithI(cmdText, L"chdir") ? 6 : 3)));
        if (StartsWithI(cmdText, L"cd\\")) arg = L"\\" + arg;
        if (StartsWithI(arg, L"/d ")) arg = Trim(arg.substr(3));
        if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
        if (arg.empty()) return;
        std::wstring target;
        // "\" bzw. "\x": relativ zur Wurzel des aktuellen Laufwerks
        if (arg.front() == L'\\' && !StartsWithI(arg, L"\\\\")) target = PathCombine(PathRoot(a.Dir()), arg.substr(1));
        else if (arg.size() >= 2 && (arg[1] == L':' || StartsWithI(arg, L"\\\\"))) target = arg;
        else target = PathCombine(a.Dir(), arg);
        a.Navigate(target);
        a.FocusList();
        return;
    }
    if (cmdText.size() == 2 && cmdText[1] == L':') {
        a.Navigate(cmdText + L"\\");
        a.FocusList();
        return;
    }
    // In einem neuen Konsolenfenster ausführen (bleibt offen)
    wchar_t comspec[MAX_PATH] = L"cmd.exe";
    GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    std::wstring line = std::wstring(L"\"") + comspec + L"\" /k " + cmdText;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                       nullptr, a.WorkingDir().empty() ? nullptr : a.WorkingDir().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LogOperation(L"Befehl in " + a.Dir() + L": " + cmdText);
    } else {
        MsgError(hwnd_, L"Der Befehl kann nicht ausgeführt werden:\n" + LastErrorMessage());
    }
}

void MainWindow::OpenSpecialFolder(int index) {
    if (index < 0 || index >= kSpecialCount) return;
    std::wstring p = kSpecial[index].id ? GetKnownFolder(*kSpecial[index].id) : GetTempDir();
    if (p.empty()) {
        MsgError(hwnd_, L"Dieses Verzeichnis ist auf diesem System nicht vorhanden.");
        return;
    }
    Active().Navigate(p);
    Active().FocusList();
}

namespace {

// Ein Tastenkürzel je Zeile; Klick auf die Zeile löst die Taste aus (siehe MainWindow::ShowShortcuts)
struct ShortcutRow {
    std::wstring keys, desc;
    UINT vk = 0;            // 0 = Gruppenüberschrift
    bool ctrl = false, shift = false, alt = false;
};

constexpr UINT WM_QF_SENDKEY = WM_APP + 3;   // wParam = virtuelle Taste, lParam = Bit 0 Strg, 1 Umschalt, 2 Alt

std::vector<ShortcutRow> ShortcutRows(const std::vector<FunctionKey>& fkeys) {
    std::vector<ShortcutRow> r;
    auto group = [&](const wchar_t* g) { r.push_back({g, L"", 0}); };
    auto key = [&](const std::wstring& k, const std::wstring& d, UINT vk, bool c = false, bool sh = false, bool al = false) {
        r.push_back({k, d, vk, c, sh, al});
    };
    group(L"Funktionstasten (Fußzeile)");
    key(L"F1", L"Tastenkürzel (diese Übersicht)", VK_F1);
    key(L"F2", L"Umbenennen (Dialog; bei mehreren markierten Dateien: Dateigruppe umbenennen)", VK_F2);
    key(L"F3", L"Dateien suchen", VK_F3);
    key(L"F4", L"Bearbeiten (Texteditor)", VK_F4);
    key(L"F5", L"Aktualisieren (Verzeichnisse neu einlesen)", VK_F5);
    key(L"F6", L"Markierung umkehren", VK_F6);
    key(L"F7", L"Verzeichnisse vergleichen (bei Split mit Auswahl der Vergleichsliste)", VK_F7);
    key(L"F8", L"Neues Verzeichnis", VK_F8);
    key(L"F9", L"Neue Datei", VK_F9);
    key(L"Umschalt+F9", L"Neue Textdatei (öffnet den Editor)", VK_F9, false, true);
    key(L"F10", L"Duplizieren (Name + „ - Kopie“, Knopf „Nummeriert“)", VK_F10);
    key(L"F11", L"Anzeigen (Dateianzeige im anderen Fenster)", VK_F11);
    key(L"Umschalt+F11", L"Anzeigefenster", VK_F11, false, true);
    key(L"Alt+F11", L"Hex-Editor", VK_F11, false, false, true);
    group(L"Dateilisten");
    key(L"Eingabe", L"Öffnen (Verzeichnis wechseln, Archiv anzeigen, Datei mit Standardprogramm)", VK_RETURN);
    key(L"Umschalt+Eingabe", L"Mit Standardprogramm öffnen", VK_RETURN, false, true);
    key(L"Rücktaste", L"Übergeordnetes Verzeichnis", VK_BACK);
    key(L"Strg+Rücktaste", L"Stammverzeichnis (oberste Ebene des Laufwerks)", VK_BACK, true);
    key(L"Alt+←", L"Zurück", VK_LEFT, false, false, true);
    key(L"Alt+→", L"Vor", VK_RIGHT, false, false, true);
    key(L"Tab", L"Nächste Liste", VK_TAB);
    for (int i = 1; i <= 4; ++i) key(L"Strg+" + std::to_wstring(i), L"Liste " + std::to_wstring(i), (UINT)('0' + i), true);
    key(L"Leertaste", L"Markieren (bei Verzeichnissen wird die Größe berechnet)", VK_SPACE);
    key(L"Einfg", L"Markieren und weiter", VK_INSERT);
    key(L"Num +", L"Gruppe markieren", VK_ADD);
    key(L"Num -", L"Gruppe abwählen", VK_SUBTRACT);
    key(L"Num *", L"Markierung umkehren", VK_MULTIPLY);
    key(L"Num /", L"Markierung aufheben", VK_DIVIDE);
    key(L"Strg+A", L"Alles markieren", 'A', true);
    key(L"Umschalt+F10", L"Kontextmenü", VK_F10, false, true);
    group(L"Dateien");
    key(L"Umschalt+F5", L"Kopieren in die andere Liste", VK_F5, false, true);
    key(L"Umschalt+F6", L"Verschieben in die andere Liste", VK_F6, false, true);
    key(L"Entf", L"Löschen (Papierkorb)", VK_DELETE);
    key(L"Umschalt+Entf", L"Endgültig löschen", VK_DELETE, false, true);
    key(L"Alt+Entf", L"Radieren", VK_DELETE, false, false, true);
    key(L"Strg+M", L"Dateigruppe umbenennen", 'M', true);
    key(L"Alt+Eingabe", L"Eigenschaften", VK_RETURN, false, false, true);
    key(L"Alt+Umschalt+Eingabe", L"Verzeichnisgrößen berechnen", VK_RETURN, false, true, true);
    key(L"Strg+Umschalt+A", L"Attribute und Datum ändern", 'A', true, true);
    key(L"Alt+F5", L"ZIP erstellen", VK_F5, false, false, true);
    key(L"Alt+F9", L"Archiv anzeigen und entpacken", VK_F9, false, false, true);
    key(L"Strg+X", L"Ausschneiden (Zwischenablage)", 'X', true);
    key(L"Strg+C", L"Kopieren (Zwischenablage)", 'C', true);
    key(L"Strg+V", L"Einfügen (Zwischenablage)", 'V', true);
    key(L"Strg+Z", L"Rückgängig (Kopieren, Verschieben, Umbenennen, Anlegen)", 'Z', true);
    group(L"Ansicht");
    key(L"Strg+T", L"Aktive Liste teilen (Split) / Teilung aufheben", 'T', true);
    key(L"Strg+Umschalt+L", L"Eine / zwei Dateilisten", 'L', true, true);
    key(L"Strg+Q", L"Dateianzeige im jeweils anderen Fenster ein/aus", 'Q', true);
    const wchar_t* views[] = {L"Details", L"Liste", L"Symbole", L"Miniaturansicht", L"Mit Unterverzeichnissen"};
    for (int i = 0; i < 5; ++i) key(L"Strg+Umschalt+" + std::to_wstring(i + 1), std::wstring(L"Ansicht: ") + views[i], (UINT)('1' + i), true, true);
    key(L"Strg+H", L"Versteckte Dateien ein/aus", 'H', true);
    key(L"Strg+Umschalt+F", L"Dateifilter", 'F', true, true);
    key(L"Strg+R", L"Aktualisieren", 'R', true);
    group(L"Navigation und Werkzeuge");
    key(L"Strg+L", L"Verzeichnis eingeben", 'L', true);
    key(L"Strg+G", L"Verzeichnis eingeben", 'G', true);
    key(L"Strg+E", L"Befehlszeile", 'E', true);
    key(L"Strg+Eingabe", L"Name in die Befehlszeile übernehmen", VK_RETURN, true);
    key(L"Alt+F1", L"Verzeichnisbaum", VK_F1, false, false, true);
    key(L"Alt+F2", L"Lesezeichenliste", VK_F2, false, false, true);
    key(L"Strg+D", L"Gewähltes Verzeichnis den Lesezeichen hinzufügen", 'D', true);
    key(L"Strg+U", L"Listen tauschen", 'U', true);
    key(L"Strg+Umschalt+O", L"Gegenseite auf dasselbe Verzeichnis", 'O', true, true);
    key(L"Strg+K", L"Dateien vergleichen", 'K', true);
    key(L"Strg+Umschalt+Y", L"Verzeichnisse synchronisieren", 'Y', true, true);
    key(L"Strg+P", L"Verzeichnisliste drucken", 'P', true);
    group(L"Programmierbare Funktionstasten (Werkzeuge → Funktionstasten belegen)");
    for (int i = 0; i < kFunctionKeyCount; ++i) {
        bool shift = i >= 12;
        std::wstring k = std::wstring(shift ? L"Strg+Umschalt+F" : L"Strg+F") + std::to_wstring(i % 12 + 1);
        std::wstring d = L"–";
        if (i < (int)fkeys.size() && !fkeys[i].IsEmpty())
            d = fkeys[i].label.empty() ? PathStem(fkeys[i].program) : fkeys[i].label;
        key(k, d, (UINT)(VK_F1 + i % 12), true, shift);
    }
    return r;
}

// Taste so senden, als würde sie jetzt gedrückt (mit Strg/Umschalt/Alt)
void SendKeyStroke(UINT vk, bool ctrl, bool shift, bool alt) {
    auto extended = [](UINT k) {
        return k == VK_LEFT || k == VK_RIGHT || k == VK_UP || k == VK_DOWN || k == VK_INSERT || k == VK_DELETE ||
               k == VK_HOME || k == VK_END || k == VK_PRIOR || k == VK_NEXT || k == VK_DIVIDE;
    };
    std::vector<INPUT> in;
    auto add = [&](UINT k, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = (WORD)k;
        i.ki.wScan = (WORD)MapVirtualKeyW(k, MAPVK_VK_TO_VSC);
        i.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended(k) ? KEYEVENTF_EXTENDEDKEY : 0);
        in.push_back(i);
    };
    if (ctrl) add(VK_CONTROL, false);
    if (shift) add(VK_SHIFT, false);
    if (alt) add(VK_MENU, false);
    add(vk, false);
    add(vk, true);
    if (alt) add(VK_MENU, true);
    if (shift) add(VK_SHIFT, true);
    if (ctrl) add(VK_CONTROL, true);
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

} // namespace

void MainWindow::ShowShortcuts() {
    class ShortcutDlg : public DialogBase {
    public:
        std::vector<ShortcutRow> rows;
        int chosen = -1;
        HFONT bold = nullptr;
        BOOL OnInit() override {
            HWND lv = Item(100);
            ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_TRACKSELECT |
                                                      LVS_EX_ONECLICKACTIVATE | LVS_EX_UNDERLINEHOT);
            SetWindowTheme(lv, L"Explorer", nullptr);
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.cx = DpiScale(hwnd_, 170);
            c.pszText = const_cast<wchar_t*>(L"Taste");
            ListView_InsertColumn(lv, 0, &c);
            c.cx = DpiScale(hwnd_, 520);
            c.pszText = const_cast<wchar_t*>(L"Funktion (Klick: Fenster schließen und Taste auslösen)");
            ListView_InsertColumn(lv, 1, &c);
            for (size_t i = 0; i < rows.size(); ++i) {
                LVITEMW it{};
                it.mask = LVIF_TEXT | LVIF_PARAM;
                it.iItem = (int)i;
                it.lParam = (LPARAM)i;
                it.pszText = const_cast<wchar_t*>(rows[i].keys.c_str());
                ListView_InsertItem(lv, &it);
                ListView_SetItemText(lv, (int)i, 1, const_cast<wchar_t*>(rows[i].desc.c_str()));
            }
            LOGFONTW lf{};
            GetObjectW((HFONT)SendMessageW(lv, WM_GETFONT, 0, 0), sizeof(lf), &lf);
            lf.lfWeight = FW_BOLD;
            bold = CreateFontIndirectW(&lf);
            SetAnchor(100, AnchorAll);
            SetAnchor(IDOK, AnchorBottomRight);
            EnableResizing();
            RestorePlacement(L"TastenkuerzelFenster");
            SetFocus(lv);
            ListView_SetItemState(lv, 1, LVIS_FOCUSED, LVIS_FOCUSED);
            return FALSE;
        }
        void Choose(int item) {
            if (item < 0 || item >= (int)rows.size() || rows[item].vk == 0) return;
            chosen = item;
            End(IDOK);
        }
        BOOL OnCommand(int id, int code, HWND ctl) override {
            // Eingabetaste in der Liste: Zeile auslösen (statt nur zu schließen)
            if (id == IDOK && GetFocus() == Item(100)) {
                int f = ListView_GetNextItem(Item(100), -1, LVNI_FOCUSED);
                if (f >= 0 && f < (int)rows.size() && rows[f].vk != 0) {
                    Choose(f);
                    return TRUE;
                }
                return TRUE;
            }
            return DialogBase::OnCommand(id, code, ctl);
        }
        INT_PTR OnMessage(UINT msg, WPARAM, LPARAM lp) override {
            if (msg == WM_NOTIFY) {
                auto* nm = (NMHDR*)lp;
                if (nm->idFrom != 100) return FALSE;
                if (nm->code == LVN_ITEMACTIVATE) {
                    Choose(((NMITEMACTIVATE*)lp)->iItem);
                    return TRUE;
                }
                if (nm->code == NM_CUSTOMDRAW) {
                    auto* cd = (NMLVCUSTOMDRAW*)lp;
                    LRESULT r = CDRF_DODEFAULT;
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) r = CDRF_NOTIFYITEMDRAW;
                    else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        size_t i = (size_t)cd->nmcd.dwItemSpec;
                        if (i < rows.size() && rows[i].vk == 0) {
                            // Gruppenüberschrift: fett, hellblau hinterlegt
                            SelectObject(cd->nmcd.hdc, bold);
                            cd->clrTextBk = Blend(GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 150);
                            cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
                            r = CDRF_NEWFONT;
                        }
                    }
                    SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, r);
                    return TRUE;
                }
            }
            return FALSE;
        }
        void OnDestroy() override {
            SavePlacement(L"TastenkuerzelFenster");
            if (bold) DeleteObject(bold);
        }
    };
    DialogTemplate t(L"Tastenkürzel", 460, 320, DialogTemplate::kResizable);
    t.Custom(100, WC_LISTVIEWW, 7, 7, 446, 286,
             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS);
    t.DefButton(IDOK, L"Schließen", 393, 299, 60, 14);
    ShortcutDlg dlg;
    dlg.rows = ShortcutRows(fkeyDefs_);
    dlg.DoModal(hwnd_, t);
    if (dlg.chosen >= 0) {
        const ShortcutRow& r = dlg.rows[dlg.chosen];
        // nach dem Schließen (Fokus wieder in der Liste) wie einen echten Tastendruck auslösen
        PostMessageW(hwnd_, WM_QF_SENDKEY, r.vk, (r.ctrl ? 1 : 0) | (r.shift ? 2 : 0) | (r.alt ? 4 : 0));
    }
}

// ===================== Netzwerkebenen und FTP/SFTP =====================

void MainWindow::RemoteOpenFocused(FilePane& p, RemoteOpenMode mode) {
    std::wstring url = p.FocusedPath();
    if (url.empty() || p.FocusedIsDir()) return;
    std::wstring local, err;
    if (!RemoteDownloadTemp(hwnd_, url, local, err)) {
        if (err != L"Abgebrochen.") MsgError(hwnd_, L"„" + LocationFileName(url) + L"“ kann nicht geladen werden:\n" + err);
        return;
    }
    const Options& o = App::Opt();
    switch (mode) {
    case RemoteOpenMode::Open:
        if (IsArchive(local)) {
            std::wstring target = OtherDir();
            ShowArchive(hwnd_, local, target.empty() ? PathParent(local) : target);
        } else if (o.enterOpensEditorForText && MatchAnyPattern(o.textExtensions, PathFileName(local))) {
            RemoteEditRegister(local, url);
            OpenTextEditor(local);
        } else {
            // Mit dem Standardprogramm öffnen; Änderungen werden nach dem Speichern hochgeladen
            RemoteEditRegister(local, url);
            ShellOpen(hwnd_, local, L"", PathParent(local));
        }
        break;
    case RemoteOpenMode::View:
        if (!o.externalViewer.empty()) RunProcess(Quote(o.externalViewer) + L" \"" + local + L"\"", PathParent(local));
        else OpenViewerWindow(local);
        break;
    case RemoteOpenMode::Edit:
        RemoteEditRegister(local, url);
        if (!o.externalEditor.empty()) RunProcess(L"\"" + o.externalEditor + L"\" \"" + local + L"\"", PathParent(local));
        else OpenTextEditor(local);
        break;
    case RemoteOpenMode::Hex:
        RemoteEditRegister(local, url);
        OpenHexEditor(local);
        break;
    case RemoteOpenMode::OpenWith: {
        RemoteEditRegister(local, url);
        OPENASINFO oi{};
        oi.pcszFile = local.c_str();
        oi.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
        SHOpenWithDialog(hwnd_, &oi);
        break;
    }
    }
}

void MainWindow::RemoteCopyOrMove(bool move) {
    FilePane& a = Active();
    auto sources = a.SelectedOrFocusedPaths();
    if (sources.empty()) return;
    FilePane* o = Other();
    std::wstring target;
    if (o && !o->IsNetworkLevel() && !(quickView_ && o->Index() == quickViewPane_)) target = o->Dir();
    std::wstring what = sources.size() == 1 ? (L"„" + LocationFileName(sources[0]) + L"“") : (std::to_wstring(sources.size()) + L" Elemente");
    if (!InputBox(hwnd_, move ? L"Verschieben" : L"Kopieren",
                  what + (move ? L" verschieben nach (Verzeichnis oder ftp://…, sftp://…, dav(s)://…):" : L" kopieren nach (Verzeichnis oder ftp://…, sftp://…, dav(s)://…):"),
                  target))
        return;
    target = Trim(target);
    if (target.size() >= 2 && target.front() == L'"' && target.back() == L'"') target = target.substr(1, target.size() - 2);
    if (target.empty()) return;
    if (IsNetworkVirtual(target)) {
        MsgError(hwnd_, L"Bitte eine Freigabe bzw. ein Verzeichnis als Ziel angeben.");
        return;
    }
    if (!IsRemoteUrl(target)) {
        if (!(target.size() >= 2 && (target[1] == L':' || StartsWithI(target, L"\\\\")))) {
            if (a.IsVirtual()) {
                MsgError(hwnd_, L"Bitte einen vollständigen Zielpfad angeben.");
                return;
            }
            target = PathCombine(a.Dir(), target);
        }
        target = NormalizeDir(target);
        if (!DirExists(target)) {
            if (!MsgConfirm(hwnd_, L"Das Zielverzeichnis „" + target + L"“ existiert nicht. Anlegen?")) return;
            if (SHCreateDirectoryExW(hwnd_, target.c_str(), nullptr) != ERROR_SUCCESS) {
                MsgError(hwnd_, L"Das Verzeichnis kann nicht angelegt werden.");
                return;
            }
        }
        if (!a.IsRemote()) {
            // lokal -> lokal: gewöhnliches Kopieren
            if (move) MoveItems(hwnd_, sources, target);
            else CopyItems(hwnd_, sources, target);
            a.SelectNone();
            ReloadVisible();
            return;
        }
    } else {
        target = NormalizeSpecialLocation(target);
    }
    RemoteTransfer(hwnd_, sources, target, move);
    a.SelectNone();
    ReloadVisible();
}

void MainWindow::ShowVirtualContextMenu(FilePane& p, POINT pt, bool onItems) {
    HMENU m = CreatePopupMenu();
    if (p.IsNetworkLevel()) {
        if (onItems) {
            AddItem(m, cmd::Open, L"Ö&ffnen\tEingabe");
            AddItem(m, cmd::CopyPaths, L"&Pfad kopieren");
            AddSep(m);
        }
        AddItem(m, cmd::Refresh, L"&Aktualisieren (erneut suchen)\tF5");
        AddItem(m, cmd::BookmarkAdd, L"Den &Lesezeichen hinzufügen\tStrg+D");
    } else {
        if (onItems) {
            bool file = !p.FocusedIsDir();
            AddItem(m, cmd::Open, L"Ö&ffnen\tEingabe");
            if (file) {
                AddItem(m, cmd::View, L"&Anzeigen\tF11");
                AddItem(m, cmd::Edit, L"&Bearbeiten\tF4");
                AddItem(m, cmd::OpenWith, L"Öffnen &mit…");
            }
            AddSep(m);
            AddItem(m, cmd::Copy, L"&Kopieren in andere Liste\tUmschalt+F5");
            AddItem(m, cmd::Move, L"&Verschieben in andere Liste\tUmschalt+F6");
            AddItem(m, cmd::Rename, L"&Umbenennen…\tF2");
            AddItem(m, cmd::Delete, L"&Löschen\tEntf");
            AddSep(m);
            AddItem(m, cmd::CopyPaths, L"&Pfad kopieren");
            AddItem(m, cmd::CopyNames, L"&Name kopieren");
            if (p.IsRemote()) {
                AddSep(m);
                AddItem(m, cmd::Properties, L"&Eigenschaften…\tAlt+Eingabe");
            }
        } else {
            AddItem(m, cmd::NewFolder, L"Neues &Verzeichnis…\tF8");
            AddItem(m, cmd::NewFile, L"&Neue Datei…\tF9");
            AddItem(m, cmd::NewTextFile, L"Neue &Textdatei…\tUmschalt+F9");
            AddItem(m, cmd::Refresh, L"&Aktualisieren\tF5");
        }
    }
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(m);
    if (id) OnCommand((int)id);
}

bool MainWindow::HandleRecycleBinCommand(int id) {
    FilePane& a = Active();
    FilePane* o = Other();
    const auto& blocked = RecycleBinBlocked();
    if (std::find(blocked.begin(), blocked.end(), id) != blocked.end()) {
        MsgInfo(hwnd_, L"Diese Funktion ist im Papierkorb nicht verfügbar.");
        return true;
    }
    switch (id) {
    case cmd::Delete:
    case cmd::DeletePermanent: {
        auto paths = a.SelectedOrFocusedPaths();
        if (paths.empty()) return true;
        DeleteRecycled(hwnd_, paths, true);
        a.Reload();
        ReloadOthers();
        bookmarks_.UpdateRecycleIcon();
        return true;
    }
    case cmd::RecycleRestore: {
        auto paths = a.SelectedOrFocusedPaths();
        if (paths.empty()) return true;
        auto done = RestoreRecycled(hwnd_, paths);
        ReloadVisible();
        for (auto& d : done) tree_.RefreshPath(PathParent(d));
        return true;
    }
    case cmd::Copy:
    case cmd::Move: {
        auto paths = a.SelectedOrFocusedPaths();
        if (paths.empty()) return true;
        std::wstring target = o && !o->IsVirtual() && !o->IsSpecialList() ? o->Dir() : L"";
        if (target.empty()) {
            MsgInfo(hwnd_, L"Die andere Liste zeigt kein gewöhnliches Verzeichnis.");
            return true;
        }
        RestoreRecycled(hwnd_, paths, target, id == cmd::Copy);
        ReloadVisible();
        return true;
    }
    default: return false;
    }
}

bool MainWindow::HandleVirtualCommand(int id) {
    FilePane& a = Active();
    FilePane* o = Other();
    FocusArea area = CurrentFocusArea();
    bool paneFocus = area == FocusArea::Pane || area == FocusArea::Other || area == FocusArea::CommandLine;
    // Kopieren/Verschieben mit einem FTP/SFTP-Verzeichnis auf einer Seite
    if ((id == cmd::Copy || id == cmd::Move) && (a.IsRemote() || (o && o->IsRemote() && !a.IsNetworkLevel()))) {
        RemoteCopyOrMove(id == cmd::Move);
        return true;
    }
    // Favoriten: kein gewöhnliches Verzeichnis
    if ((id == cmd::CompareDirs || id == cmd::SyncDirs) && (a.IsFavorites() || (o && o->IsFavorites()))) {
        MsgInfo(hwnd_, L"Die Favoriten lassen sich nicht vergleichen oder synchronisieren.");
        return true;
    }
    if (a.IsRecycleBin() && paneFocus && HandleRecycleBinCommand(id)) return true;
    if (o && o->IsRecycleBin() && !a.IsRecycleBin() && (id == cmd::Copy || id == cmd::Move)) {
        MsgInfo(hwnd_, L"In den Papierkorb gelangen Dateien und Verzeichnisse über „Löschen“ (Entf).");
        return true;
    }
    if (a.IsFavorites() && paneFocus) {
        switch (id) {
        case cmd::NewFolder: case cmd::NewFile: case cmd::NewTextFile: case cmd::ClipPaste: case cmd::FindFiles:
        case cmd::FindDuplicates: case cmd::DirSizes: case cmd::CreateZip: case cmd::PrintList: case cmd::Duplicate:
            MsgInfo(hwnd_, L"Diese Funktion ist in den Favoriten nicht verfügbar.");
            return true;
        default: break;
        }
    }
    if (!a.IsVirtual()) {
        if ((id == cmd::Copy || id == cmd::Move) && o && o->IsNetworkLevel()) {
            MsgInfo(hwnd_, L"Die andere Liste zeigt die Netzwerkübersicht. Bitte dort zuerst eine Freigabe öffnen.");
            return true;
        }
        return false;
    }
    if (!paneFocus && (id == cmd::Delete || id == cmd::DeletePermanent || id == cmd::Rename || id == cmd::Properties))
        return false;   // Baum bzw. Lesezeichenliste
    if ((id >= cmd::BookmarkFirst && id <= cmd::BookmarkLast) || (id >= cmd::GoSpecialFirst && id <= cmd::GoSpecialLast) ||
        (id >= cmd::FKeyFirst && id <= cmd::FKeyLast))
        return false;
    switch (id) {
    // überall erlaubt: Navigation, Ansicht, Markierung, Lesezeichen, Einstellungen
    case cmd::GoBack: case cmd::GoForward: case cmd::GoUp: case cmd::GoRoot: case cmd::GoPath: case cmd::FocusPath:
    case cmd::GoOtherSame: case cmd::SwapPanes: case cmd::NextPane: case cmd::FocusPane1: case cmd::FocusPane2:
    case cmd::FocusPane3: case cmd::FocusPane4: case cmd::FocusTree: case cmd::FocusBookmarks: case cmd::FocusCommandLine:
    case cmd::CmdLineInsertName: case cmd::TwoPanes: case cmd::SplitToggle: case cmd::QuickView: case cmd::ViewDetails:
    case cmd::ViewList: case cmd::ViewIcons: case cmd::ViewThumbnails: case cmd::ViewRecursive: case cmd::SortName: case cmd::SortExt:
    case cmd::SortSize: case cmd::SortDate: case cmd::SortAttr: case cmd::SortCreated: case cmd::SortDescending:
    case cmd::ShowHidden: case cmd::ShowDirSizes: case cmd::Filter: case cmd::Refresh: case cmd::ToggleToolbar: case cmd::ToggleFKeyBar:
    case cmd::CustomizeToolbar:
    case cmd::ToggleCommandLine: case cmd::ToggleStatusBar: case cmd::SelectAll: case cmd::SelectNone:
    case cmd::InvertSelection: case cmd::SelectGroup: case cmd::DeselectGroup: case cmd::SelectSameExt:
    case cmd::CopyPaths: case cmd::CopyNames: case cmd::BookmarkAdd: case cmd::BookmarkNewRemote: case cmd::BookmarkSort:
    case cmd::BookmarkAddCloud: case cmd::BookmarkFavorites: case cmd::PrivateMode: case cmd::ClearHistory:
    case cmd::ShowRecycleBin: case cmd::RecycleEmpty:
    case cmd::FunctionKeys:
    case cmd::Options: case cmd::Shortcuts: case cmd::About: case cmd::Exit: case cmd::ClearCompareMarks: case cmd::OpLog:
    case cmd::DriveOverview: case cmd::CommandPrompt:
        return false;
    case cmd::Open:
        if (a.FocusedIsParent()) a.GoUp();
        else if (a.FocusedIsDir()) a.Navigate(a.FocusedPath());
        else if (a.IsRemote()) RemoteOpenFocused(a, RemoteOpenMode::Open);
        return true;
    case cmd::View:
        if (App::Opt().quickViewTarget == QuickViewTarget::OtherPane && Other()) return false;  // Schnellansicht
        if (a.IsRemote()) RemoteOpenFocused(a, RemoteOpenMode::View);
        return true;
    default: break;
    }
    if (a.IsNetworkLevel()) {
        MsgInfo(hwnd_, L"Diese Funktion ist in der Netzwerkübersicht nicht verfügbar. Bitte eine Freigabe öffnen.");
        return true;
    }
    // ---- FTP/SFTP ----
    switch (id) {
    case cmd::ViewWindow: RemoteOpenFocused(a, RemoteOpenMode::View); return true;
    case cmd::Edit: RemoteOpenFocused(a, RemoteOpenMode::Edit); return true;
    case cmd::HexEdit: RemoteOpenFocused(a, RemoteOpenMode::Hex); return true;
    case cmd::OpenWith: RemoteOpenFocused(a, RemoteOpenMode::OpenWith); return true;
    case cmd::NewFolder: {
        std::wstring name = L"Neues Verzeichnis";
        if (!InputBox(hwnd_, L"Neues Verzeichnis", L"Name des neuen Verzeichnisses in „" + a.Dir() + L"“:", name)) return true;
        name = Trim(name);
        if (name.empty()) return true;
        if (RemoteMakeDir(hwnd_, a.Dir(), name)) {
            a.Reload();
            a.FocusName(Split(ReplaceAll(name, L"\\", L"/"), L'/')[0]);
        }
        return true;
    }
    case cmd::NewFile:
    case cmd::NewTextFile: {
        bool text = id == cmd::NewTextFile;
        std::wstring name = text ? L"Neue Textdatei.txt" : L"Neue Datei";
        if (!InputBox(hwnd_, text ? L"Neue Textdatei" : L"Neue Datei",
                      L"Name der neuen " + std::wstring(text ? L"Textdatei" : L"(leeren) Datei") + L" in „" + a.Dir() + L"“:", name,
                      L"", text ? FileStemLength(name) : -1))
            return true;
        name = Trim(name);
        if (name.empty()) return true;
        std::wstring url = LocationCombine(a.Dir(), name);
        if (RemoteExists(hwnd_, url)) {
            if (!text) {
                MsgError(hwnd_, L"„" + name + L"“ existiert bereits.");
                return true;
            }
        } else if (!RemoteCreateFile(hwnd_, a.Dir(), name)) {
            return true;
        }
        a.Reload();
        a.FocusName(name);
        if (text) RemoteOpenFocused(a, RemoteOpenMode::Edit);
        return true;
    }
    case cmd::Rename:
    case cmd::BatchRename: {
        auto names = a.SelectedOrFocusedNames();
        if (names.empty()) return true;
        if (names.size() > 1) {
            MsgInfo(hwnd_, L"Auf FTP/SFTP/WebDAV-Servern kann jeweils ein Element umbenannt werden.");
            return true;
        }
        std::wstring name = names[0];
        if (!InputBox(hwnd_, L"Umbenennen", L"Neuer Name für „" + names[0] + L"“:", name)) return true;
        name = Trim(name);
        if (name.empty() || name == names[0]) return true;
        if (RemoteRename(hwnd_, LocationCombine(a.Dir(), names[0]), name)) {
            a.Reload();
            a.FocusName(name);
        }
        return true;
    }
    case cmd::Delete:
    case cmd::DeletePermanent: {
        auto names = a.SelectedOrFocusedNames();
        if (names.empty()) return true;
        RemoteDelete(hwnd_, a.Dir(), names, true);
        ReloadVisible();
        return true;
    }
    case cmd::Properties:
    case cmd::Attributes: {
        // Eigenschaften mit Unix-Zugriffsrechten (chmod)
        auto entries = a.EntriesByNames(a.SelectedOrFocusedNames());
        if (entries.empty()) return true;
        auto sel = a.SelectedNames();
        if (RemoteProperties(hwnd_, a.Dir(), entries)) {
            a.Reload();
            if (!sel.empty()) a.SelectNames(sel, false);
        }
        return true;
    }
    }
    MsgInfo(hwnd_, L"Diese Funktion ist für FTP/SFTP/WebDAV-Verzeichnisse nicht verfügbar.");
    return true;
}

void MainWindow::OnCommand(int id) {
    FilePane& a = Active();
    if (HandleVirtualCommand(id)) {
        UpdateStatusBar();
        return;
    }
    if (id >= cmd::BookmarkFirst && id <= cmd::BookmarkLast) {
        size_t i = (size_t)(id - cmd::BookmarkFirst);
        if (i < bookmarks_.Get().size()) {
            a.Navigate(bookmarks_.Get()[i].path);
            a.FocusList();
        }
        return;
    }
    if (id >= cmd::GoSpecialFirst && id <= cmd::GoSpecialLast) {
        OpenSpecialFolder(id - cmd::GoSpecialFirst);
        return;
    }
    if (id >= cmd::FKeyFirst && id <= cmd::FKeyLast) {
        fkeyDefs_ = LoadFunctionKeys(App::Cfg());
        int k = id - cmd::FKeyFirst;
        if (k < (int)fkeyDefs_.size() && fkeyDefs_[k].IsEmpty()) {
            MessageBeep(MB_ICONASTERISK);
            return;
        }
        ExecuteFunctionKey(hwnd_, k, ActiveContext());
        return;
    }
    switch (id) {
    // ---- Datei ----
    case cmd::Open:
        if (a.FocusedIsParent()) a.GoUp();
        else if (a.FocusedIsDir()) a.Navigate(a.FocusedPath());
        else OpenFileItem(a);
        break;
    case cmd::OpenWith: {
        std::wstring p = a.FocusedPath();
        if (!p.empty()) {
            OPENASINFO oi{};
            oi.pcszFile = p.c_str();
            oi.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
            SHOpenWithDialog(hwnd_, &oi);
        }
        break;
    }
    case cmd::View: {
        if (App::Opt().quickViewTarget == QuickViewTarget::OtherPane && Other()) {
            SetQuickView(!quickView_);
        } else {
            std::wstring p = a.FocusedPath();
            if (p.empty()) break;
            if (!App::Opt().externalViewer.empty())
                RunProcess(Quote(App::Opt().externalViewer) + L" \"" + p + L"\"", a.WorkingDir());
            else
                OpenViewerWindow(p);
        }
        break;
    }
    case cmd::ViewWindow: {
        std::wstring p = a.FocusedPath();
        if (!p.empty()) OpenViewerWindow(p);
        break;
    }
    case cmd::HexEdit: {
        std::wstring p = a.FocusedPath();
        if (!p.empty() && !a.FocusedIsDir()) OpenHexEditor(p);
        break;
    }
    case cmd::Edit: {
        std::wstring p = a.FocusedPath();
        if (p.empty() || a.FocusedIsDir()) break;
        if (!App::Opt().externalEditor.empty())
            RunProcess(L"\"" + App::Opt().externalEditor + L"\" \"" + p + L"\"", a.WorkingDir());
        else
            OpenTextEditor(p);
        break;
    }
    case cmd::NewTextFile: {
        std::wstring name = MakeUniqueName(a.Dir(), L"Neue Textdatei.txt");
        if (!InputBox(hwnd_, L"Neue Textdatei", L"Name der neuen Textdatei (UTF-8 ohne BOM):", name, L"", FileStemLength(name)))
            break;
        name = Trim(name);
        if (name.empty()) break;
        std::wstring full = PathCombine(a.Dir(), name);
        if (!PathExists(full)) {
            if (!CreateEmptyFile(hwnd_, a.Dir(), name)) break;
            a.Reload();
            a.FocusName(name);
        }
        if (!App::Opt().externalEditor.empty()) RunProcess(L"\"" + App::Opt().externalEditor + L"\" \"" + full + L"\"", a.Dir());
        else OpenTextEditor(full);
        break;
    }
    case cmd::NewFolder: {
        std::wstring name = MakeUniqueName(a.Dir(), L"Neues Verzeichnis");
        if (!InputBox(hwnd_, L"Neues Verzeichnis", L"Name des neuen Verzeichnisses in „" + a.Dir() + L"“:", name)) break;
        name = Trim(name);
        if (name.empty()) break;
        if (CreateFolder(hwnd_, a.Dir(), name)) {
            a.Reload();
            a.FocusName(Split(name, L'\\')[0]);
            tree_.RefreshPath(a.Dir());
        }
        break;
    }
    case cmd::Copy: CopyOrMove(false); break;
    case cmd::Move: CopyOrMove(true); break;
    case cmd::Rename: {
        // F2: vollständiger Umbenennungsdialog (auch für eine einzelne Datei).
        // Direktes Umbenennen in der Liste: langsamer zweiter Klick auf den Namen.
        if (CurrentFocusArea() == FocusArea::Bookmarks) {
            bookmarks_.RenameSelected();
            break;
        }
        // Ansicht „Mit Unterverzeichnissen“: je Unterverzeichnis ein Umbenennungsdialog
        if (RenameGroupKeepSelection(hwnd_, a)) {
            ReloadOthers();
            tree_.RefreshPath(a.Dir());
        }
        break;
    }
    case cmd::NewFile: {
        std::wstring name = MakeUniqueName(a.Dir(), L"Neue Datei");
        if (!AskName(hwnd_, L"Neue Datei", L"Name der neuen (leeren) Datei in „" + a.Dir() + L"“:", name, false)) break;
        if (PathExists(PathCombine(a.Dir(), name))) {
            MsgError(hwnd_, L"„" + name + L"“ existiert bereits.");
            break;
        }
        if (CreateEmptyFile(hwnd_, a.Dir(), name)) {
            a.Reload();
            a.FocusName(name);
        }
        break;
    }
    case cmd::Duplicate: {
        auto names = a.SelectedOrFocusedNames();
        if (names.empty()) break;
        std::wstring lastNew;
        for (auto& rel : names) {
            std::wstring src = PathCombine(a.Dir(), rel);
            std::wstring srcDir = PathParent(src);   // bei „Mit Unterverzeichnissen“ das Unterverzeichnis
            std::wstring n = PathFileName(rel);
            bool isDir = DirExists(src);
            std::wstring stem = n, ext;
            size_t dot = n.find_last_of(L'.');
            if (!isDir && dot != std::wstring::npos && dot > 0) {
                stem = n.substr(0, dot);
                ext = n.substr(dot);
            }
            std::wstring target = stem + L" - Kopie" + ext;
            std::wstring dir = srcDir;
            if (!AskName(hwnd_, L"Duplizieren", L"Name des Duplikats von „" + n + L"“:", target, !isDir,
                         [dir, n, isDir]() { return NumberedName(dir, n, isDir); }))
                break;
            if (PathExists(PathCombine(srcDir, target))) {
                MsgError(hwnd_, L"„" + target + L"“ existiert bereits.");
                continue;
            }
            if (CopyJobs(hwnd_, {{src, srcDir, target}})) {
                std::wstring sub = PathParent(PathCombine(L"X:\\", rel)).substr(3);
                lastNew = sub.empty() ? target : PathCombine(sub, target);
            }
        }
        a.Reload();
        if (!lastNew.empty()) a.FocusName(lastNew);
        break;
    }
    case cmd::Wipe: {
        if (CurrentFocusArea() == FocusArea::Bookmarks) break;
        auto paths = a.SelectedOrFocusedPaths();
        if (paths.empty()) break;
        if (WipeItems(hwnd_, paths)) {
            ReloadVisible();
            tree_.RefreshPath(a.Dir());
        }
        break;
    }
    case cmd::BatchRename: {
        if (RenameGroupKeepSelection(hwnd_, a)) {
            ReloadOthers();
            tree_.RefreshPath(a.Dir());
        }
        break;
    }
    case cmd::Delete: DeleteSelection(false); break;
    case cmd::DeletePermanent: DeleteSelection(true); break;
    case cmd::Properties: {
        if (CurrentFocusArea() == FocusArea::Tree) {
            std::wstring p = tree_.SelectedPath();
            if (!p.empty()) ShowShellProperties(hwnd_, p, {});
            break;
        }
        auto names = a.SelectedOrFocusedNames();
        ShowShellProperties(hwnd_, a.Dir(), names);
        break;
    }
    case cmd::Attributes: {
        auto names = a.SelectedOrFocusedNames();
        if (!names.empty() && ChangeAttributes(hwnd_, a.Dir(), names)) a.Reload();
        break;
    }
    case cmd::SplitFile: {
        std::wstring p = a.FocusedPath();
        if (p.empty() || a.FocusedIsDir()) break;
        std::wstring target = OtherDir();
        if (SplitFile(hwnd_, p, target.empty() ? a.Dir() : target)) ReloadVisible();
        break;
    }
    case cmd::JoinFiles: {
        std::wstring p = a.FocusedPath();
        if (p.empty() || a.FocusedIsDir()) break;
        std::wstring target = OtherDir();
        if (JoinFiles(hwnd_, p, target.empty() ? a.Dir() : target)) ReloadVisible();
        break;
    }
    case cmd::CreateZip: {
        auto names = a.SelectedOrFocusedNames();
        if (names.empty()) break;
        std::wstring target = OtherDir();
        if (CreateZipArchive(hwnd_, a.Dir(), names, target.empty() ? a.Dir() : target)) ReloadVisible();
        break;
    }
    case cmd::OpenArchive: {
        std::wstring p = a.FocusedPath();
        if (p.empty() || a.FocusedIsDir()) break;
        std::wstring target = OtherDir();
        ShowArchive(hwnd_, p, target.empty() ? a.Dir() : target);
        break;
    }
    case cmd::PrintList: PrintDirectoryListing(hwnd_, a.Dir()); break;
    case cmd::Undo:
        if (CanUndo()) {
            UndoLast(hwnd_);
            ReloadVisible();
        } else {
            MessageBeep(MB_ICONASTERISK);
        }
        break;
    case cmd::Exit: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;

    // ---- Bearbeiten ----
    case cmd::ClipCut:
    case cmd::ClipCopy: {
        auto paths = a.SelectedOrFocusedPaths();
        if (!paths.empty()) ClipboardSetFiles(hwnd_, paths, id == cmd::ClipCut);
        break;
    }
    case cmd::ClipPaste: {
        std::vector<std::wstring> files;
        bool cut = false;
        if (!ClipboardGetFiles(hwnd_, files, cut)) break;
        if (cut) {
            // Zwischenablage nur nach erfolgreichem Verschieben leeren (bei Abbruch/Fehler erneut einfügbar)
            if (MoveItems(hwnd_, files, a.Dir()) && OpenClipboard(hwnd_)) {
                EmptyClipboard();
                CloseClipboard();
            }
        } else {
            CopyItems(hwnd_, files, a.Dir());
        }
        ReloadVisible();
        break;
    }
    case cmd::SelectAll: a.SelectAll(); break;
    case cmd::SelectNone: a.SelectNone(); break;
    case cmd::InvertSelection: a.InvertSelection(); break;
    case cmd::SelectGroup:
    case cmd::DeselectGroup: {
        static std::wstring last = L"*.*";
        std::wstring pat = last;
        bool sel = id == cmd::SelectGroup;
        if (!InputBox(hwnd_, sel ? L"Gruppe markieren" : L"Gruppe abwählen",
                      L"Muster (mehrere durch ; getrennt, z. B. *.txt;*.doc):", pat))
            break;
        last = pat;
        a.SelectPattern(pat, sel);
        break;
    }
    case cmd::SelectSameExt: a.SelectSameExtension(); break;
    case cmd::CopyPaths:
    case cmd::CopyNames: {
        auto names = a.SelectedOrFocusedNames();
        if (names.empty()) break;
        std::vector<std::wstring> lines;
        for (auto& n : names) lines.push_back(id == cmd::CopyPaths ? LocationCombine(a.Dir(), n) : n);
        ClipboardSetText(hwnd_, Join(lines, L"\r\n"));
        break;
    }

    // ---- Ansicht ----
    case cmd::TwoPanes: SetTwoPanes(!twoPanes_); break;
    case cmd::SplitToggle: {
        int col = active_ % 2;
        SetSplit(col, !split_[col]);
        break;
    }
    case cmd::QuickView: SetQuickView(!quickView_); break;
    case cmd::ViewDetails:
    case cmd::ViewList:
    case cmd::ViewIcons:
    case cmd::ViewThumbnails:
    case cmd::ViewRecursive:
        if (HWND arc = FocusedArchiveView()) {
            // aktive Archivliste in der Dateianzeige
            ArchiveViewSetMode(arc, id - cmd::ViewDetails);
            UpdateViewButtons();
            break;
        }
        a.SetView((PaneView)(id - cmd::ViewDetails));
        UpdateViewButtons();
        break;
    case cmd::SortName: a.SetSort(SortKey::Name, a.SortDescending()); break;
    case cmd::SortExt: a.SetSort(SortKey::Ext, a.SortDescending()); break;
    case cmd::SortSize: a.SetSort(SortKey::Size, a.SortDescending()); break;
    case cmd::SortDate: a.SetSort(SortKey::Date, a.SortDescending()); break;
    case cmd::SortAttr: a.SetSort(SortKey::Attr, a.SortDescending()); break;
    case cmd::SortCreated: a.SetSort(SortKey::Created, a.SortDescending()); break;
    case cmd::SortDescending: a.SetSort(a.Sort(), !a.SortDescending()); break;
    case cmd::ShowHidden:
        App::Opt().showHidden = !App::Opt().showHidden;
        App::Opt().showSystem = App::Opt().showHidden && App::Opt().showSystem;
        ApplyOptionsAll();
        break;
    case cmd::ShowDirSizes:
        App::Opt().showDirSizes = !App::Opt().showDirSizes;
        for (auto& p : panes_) p->UpdateDirSizesOption();
        break;
    case cmd::Filter: {
        PaneFilter f = a.Filter();
        if (EditFilter(hwnd_, f)) a.SetFilter(f);
        break;
    }
    case cmd::Refresh:
        for (auto& p : panes_) p->UpdateDrives();
        tree_.Populate();
        ReloadVisible();
        tree_.SelectPath(a.Dir());
        break;
    case cmd::ToggleToolbar: showToolbar_ = !showToolbar_; Layout(); break;
    case cmd::CustomizeToolbar: CustomizeToolbar(); break;
    case cmd::ToggleFKeyBar: showFKeys_ = !showFKeys_; Layout(); break;
    case cmd::ToggleCommandLine: showCmdLine_ = !showCmdLine_; Layout(); break;
    case cmd::ToggleStatusBar: showStatus_ = !showStatus_; Layout(); break;

    // ---- Gehe zu ----
    case cmd::GoBack: a.GoBack(); break;
    case cmd::GoForward: a.GoForward(); break;
    case cmd::GoUp: a.GoUp(); break;
    case cmd::GoRoot: a.GoRoot(); break;
    case cmd::GoPath:
    case cmd::FocusPath: a.FocusPath(); break;
    case cmd::GoOtherSame:
        if (FilePane* o = Other()) o->Navigate(a.Dir());
        break;
    case cmd::SwapPanes:
        if (FilePane* o = Other()) {
            std::wstring d1 = a.Dir(), d2 = o->Dir();
            a.Navigate(d2);
            o->Navigate(d1);
        }
        break;
    case cmd::NextPane: {
        for (int k = 1; k <= 4; ++k) {
            int i = (active_ + k) % 4;
            if (PaneVisible(i) && !(quickView_ && i == quickViewPane_)) {
                SetActive(i, true);
                break;
            }
        }
        break;
    }
    case cmd::FocusPane1:
    case cmd::FocusPane2:
    case cmd::FocusPane3:
    case cmd::FocusPane4: SetActive(id - cmd::FocusPane1, true); break;
    case cmd::FocusTree: SetFocus(tree_.Hwnd()); break;
    case cmd::FocusBookmarks: SetFocus(bookmarks_.Hwnd()); break;
    case cmd::FocusCommandLine:
        if (!showCmdLine_) {
            showCmdLine_ = true;
            Layout();
        }
        SetFocus(cmdCombo_);
        break;
    case cmd::CmdLineInsertName: {
        std::wstring n = a.FocusedName();
        if (n.empty()) break;
        if (!showCmdLine_) {
            showCmdLine_ = true;
            Layout();
        }
        std::wstring t = GetWindowTextStr(cmdCombo_);
        if (!t.empty() && t.back() != L' ') t += L' ';
        t += Quote(n) + L" ";
        SetWindowTextW(cmdCombo_, t.c_str());
        SetFocus(cmdCombo_);
        if (cmdEdit_) SendMessageW(cmdEdit_, EM_SETSEL, t.size(), t.size());
        break;
    }

    // ---- Lesezeichen ----
    case cmd::BookmarkAdd: AddBookmarkFor(a); break;
    case cmd::BookmarkSort:
        if (bookmarks_.Get().size() < 2) break;
        if (MsgConfirm(hwnd_, L"Lesezeichen sortieren?\n\nReihenfolge: „Netzwerk“, Windows-Standardverzeichnisse (blau), "
                              L"Cloud-Speicher (lila), FTP/SFTP/WebDAV-Zugänge (grün), übrige Netzwerkpfade (rot), "
                              L"lokale Verzeichnisse – innerhalb jeder Gruppe alphabetisch."))
            bookmarks_.SortByCategory();
        break;
    case cmd::BookmarkAddCloud: {
        const auto& clouds = DetectCloudFolders(true);
        if (clouds.empty()) {
            MsgInfo(hwnd_, L"Es wurde kein Cloud-Speicher gefunden.\n\nQFiles nutzt die Ordner der Windows-Programme der "
                           L"Anbieter: Dropbox, „Google Drive für den Desktop“ bzw. Microsoft OneDrive. Ist eines davon "
                           L"installiert und angemeldet, erscheint sein Ordner hier.");
            break;
        }
        std::vector<std::wstring> opts;
        for (auto& c : clouds) {
            bool has = bookmarks_.FindPath(c.path) >= 0;
            opts.push_back(c.name + L":  " + c.path + (has ? L"   (bereits Lesezeichen)" : L""));
        }
        if (clouds.size() > 1) opts.push_back(L"Alle hinzufügen");
        int c = AskChoice(hwnd_, L"Cloud-Speicher hinzufügen", L"Gefundene Cloud-Ordner:", opts);
        if (c < 0) break;
        for (size_t i = 0; i < clouds.size(); ++i) {
            if (c != (int)i && c != (int)clouds.size()) continue;
            if (bookmarks_.FindPath(clouds[i].path) >= 0) continue;
            bookmarks_.Append(clouds[i].name, clouds[i].path);
        }
        break;
    }
    case cmd::BookmarkNewRemote: {
        RemoteAccess acc;
        acc.url.proto = RemoteProto::Sftp;
        if (EditRemoteAccess(hwnd_, acc, true)) {
            bookmarks_.Append(acc.name, acc.url.ToString());
            if (a.Navigate(acc.url.ToString())) a.FocusList();
        }
        break;
    }

    // ---- Werkzeuge ----
    case cmd::FindFiles: FindFiles(hwnd_, a.Dir()); break;
    case cmd::FindDuplicates: FindDuplicates(hwnd_, a.Dir()); break;
    case cmd::CompareFiles: {
        std::wstring f1, f2;
        auto sel = a.SelectedNames();
        if (sel.size() >= 2) {
            f1 = PathCombine(a.Dir(), sel[0]);
            f2 = PathCombine(a.Dir(), sel[1]);
        } else {
            f1 = a.FocusedPath();
            if (f1.empty() || a.FocusedIsDir()) {
                MsgInfo(hwnd_, L"Bitte zwei Dateien markieren oder eine Datei auswählen.");
                break;
            }
            if (FilePane* o = Other()) {
                std::wstring same = PathCombine(o->Dir(), PathFileName(f1));
                std::wstring of = o->FocusedPath();
                if (FileExists(same)) f2 = same;
                else if (!of.empty() && FileExists(of)) f2 = of;
            }
            if (f2.empty()) f2 = OpenFileDialog(hwnd_, L"Vergleichen mit …", OtherDir().empty() ? a.Dir() : OtherDir());
        }
        if (!f1.empty() && !f2.empty()) qf::CompareFiles(hwnd_, f1, f2);
        break;
    }
    case cmd::CompareDirs: {
        if (Other() && Other()->IsVirtual()) {
            MsgInfo(hwnd_, L"Verzeichnisse auf FTP/SFTP/WebDAV-Servern bzw. die Netzwerkübersicht können nicht verglichen werden.");
            break;
        }
        // Vergleich der aktiven Liste mit einer anderen. Gibt es mehrere (Split), wird gefragt:
        // zuerst die andere Liste derselben Spalte, dann die gegenüberliegende.
        std::vector<int> cands;
        int col = active_ % 2, row = active_ / 2;
        int order[3] = {col + (1 - row) * 2, (1 - col) + row * 2, (1 - col) + (1 - row) * 2};
        for (int i : order)
            if (i != active_ && PaneVisible(i) && !(quickView_ && i == quickViewPane_)) cands.push_back(i);
        if (cands.empty()) {
            MsgInfo(hwnd_, L"Zum Vergleichen wird eine zweite Dateiliste benötigt (zwei Listen oder Split).");
            break;
        }
        int pick = cands[0];
        if (cands.size() > 1) {
            std::vector<std::wstring> opts;
            for (int i : cands) {
                std::wstring pos = PanePositionName(i, twoPanes_, split_[i % 2]);
                opts.push_back(L"Liste " + std::to_wstring(i + 1) + (pos.empty() ? L"" : (L" (" + pos + L")")) + L":  " +
                               panes_[i]->Dir());
            }
            std::wstring self = PanePositionName(active_, twoPanes_, split_[col]);
            int c = AskChoice(hwnd_, L"Verzeichnisse vergleichen",
                              L"Liste " + std::to_wstring(active_ + 1) + (self.empty() ? L"" : (L" (" + self + L")")) +
                                  L" vergleichen mit:",
                              opts);
            if (c < 0) break;
            pick = cands[c];
        }
        FilePane* p1 = &a;
        FilePane* p2 = panes_[pick].get();
        // links = kleinere Spalte; bei gleicher Spalte die obere
        bool swap = (pick % 2 < col) || (pick % 2 == col && pick < active_);
        FilePane* left = swap ? p2 : p1;
        FilePane* right = swap ? p1 : p2;
        CompareMarks ml, mr;
        std::vector<std::wstring> sl, sr;
        if (CompareDirectories(hwnd_, left->Dir(), right->Dir(), ml, mr, sl, sr)) {
            left->SetMarks(ml);
            right->SetMarks(mr);
            left->SelectNames(sl);
            right->SelectNames(sr);
        }
        break;
    }
    case cmd::ClearCompareMarks:
        for (auto& p : panes_) p->ClearMarks();
        break;
    case cmd::SyncDirs: {
        FilePane* o = Other();
        if (!o) break;
        if (o->IsVirtual()) {
            MsgInfo(hwnd_, L"Mit FTP/SFTP/WebDAV-Verzeichnissen bzw. der Netzwerkübersicht kann nicht synchronisiert werden.");
            break;
        }
        FilePane* left = (active_ % 2 == 0) ? &a : o;
        FilePane* right = (left == &a) ? o : &a;
        if (SyncDirectories(hwnd_, left->Dir(), right->Dir())) ReloadVisible();
        break;
    }
    case cmd::DirSizes: a.ComputeDirSizes(false); break;
    case cmd::DriveOverview: DriveOverview(hwnd_); break;
    case cmd::OpLog: ShowOperationLog(hwnd_); break;
    case cmd::CommandPrompt: {
        wchar_t comspec[MAX_PATH] = L"cmd.exe";
        GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
        std::wstring line = std::wstring(L"\"") + comspec + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::wstring wd = a.WorkingDir();
        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                           wd.empty() ? nullptr : wd.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        break;
    }
    case cmd::FunctionKeys:
        if (ConfigureFunctionKeys(hwnd_)) {
            fkeyDefs_ = LoadFunctionKeys(App::Cfg());
            App::Cfg().Save();
            BuildFKeyCells();
        }
        break;
    case cmd::Options: {
        bool wasPrivate = privActive_;
        if (ShowOptionsDialog(hwnd_)) {
            bookmarks_.Set(LoadBookmarks(App::Cfg()));   // ggf. importierte Lesezeichen
            if (App::Opt().privateMode != wasPrivate) SetPrivateMode(App::Opt().privateMode);
            ApplyOptionsAll();
            Layout();
        }
        break;
    }
    case cmd::PrivateMode: SetPrivateMode(!App::Opt().privateMode); break;
    case cmd::CompareWithPane0: case cmd::CompareWithPane0 + 1: case cmd::CompareWithPane0 + 2: case cmd::CompareWithPane3: {
        // Beyond Compare: die Datei der weiter links (bzw. oben) stehenden Liste links
        int o = id - cmd::CompareWithPane0;
        std::wstring bc = BeyondComparePath();
        std::wstring mine = a.FocusedPath();
        if (bc.empty() || mine.empty() || o == active_) break;
        std::wstring other = PathCombine(panes_[o]->Dir(), PathFileName(mine));
        if (!FileExists(other)) break;
        bool otherFirst = (o % 2 < active_ % 2) || (o % 2 == active_ % 2 && o / 2 < active_ / 2);
        std::wstring left = otherFirst ? other : mine, right = otherFirst ? mine : other;
        RunProcess(Quote(bc) + L" " + Quote(left) + L" " + Quote(right), PathParent(mine));
        LogOperation(L"Vergleich mit Beyond Compare: " + left + L" ↔ " + right);
        break;
    }
    case cmd::ClearHistory: ClearAllHistory(); break;

    // ---- Papierkorb ----
    case cmd::ShowRecycleBin:
        a.Navigate(kRecycleBinLocation);
        a.FocusList();
        break;
    case cmd::RecycleEmpty:
        if (EmptyRecycleBinAsk(hwnd_)) {
            ReloadVisible();
            bookmarks_.UpdateRecycleIcon();
        }
        break;

    // ---- Favoriten ----
    case cmd::BookmarkFavorites:
        if (Favorites().empty()) break;
        a.Navigate(kFavoritesLocation);
        a.FocusList();
        break;
    case cmd::FavoriteAdd: {
        if (a.IsVirtual() || a.IsFavorites()) break;
        auto paths = a.SelectedOrFocusedPaths();
        std::vector<std::wstring> files;
        for (auto& p : paths)
            if (CanBeFavorite(p)) files.push_back(p);
        if (files.empty()) {
            MsgInfo(hwnd_, L"Als Favorit lassen sich nur Dateien kennzeichnen (keine Verzeichnisse, keine FTP/SFTP/WebDAV-Dateien).");
            break;
        }
        int n = AddFavorites(files);
        if (n == 0) MessageBeep(MB_ICONASTERISK);
        break;
    }
    case cmd::FavoriteRemove: {
        auto paths = a.SelectedOrFocusedPaths();
        if (paths.empty()) break;
        if (a.IsFavorites()) {
            // Nachfolger fokussieren
            RemoveFavorites(paths, false);
            a.Reload();
            OnFavoritesChanged(false);
            for (int i = 0; i < 4; ++i)
                if (i != active_ && panes_[i]->IsFavorites()) panes_[i]->Reload();
        } else {
            RemoveFavorites(paths);
        }
        break;
    }
    case cmd::FavoriteGotoDir: {
        std::wstring p = a.FocusedPath();
        if (p.empty()) break;
        if (a.Navigate(PathParent(p), PathFileName(p))) {
            a.FocusName(PathFileName(p));
            a.FocusList();
        }
        break;
    }

    // ---- Hilfe ----
    case cmd::Shortcuts: ShowShortcuts(); break;
    case cmd::About: ShowAbout(hwnd_); break;
    }
    UpdateStatusBar();
}

// ===================== Fensterprozedur =====================

LRESULT CALLBACK MainWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self;
    if (msg == WM_NCCREATE) {
        self = (MainWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (MainWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    if (self) return self->Proc(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::CmdEditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = (MainWindow*)ref;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN && GetKeyState(VK_CONTROL) >= 0) {
            self->RunCommandLine();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetWindowTextW(self->cmdCombo_, L"");
            self->Active().FocusList();
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == 27) return 0;
        break;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::wstring t = GetWindowTextStr(self->cmdCombo_);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(len + 1, L'\0');
            DragQueryFileW(drop, i, s.data(), len + 1);
            s.resize(len);
            if (!t.empty() && t.back() != L' ') t += L' ';
            t += Quote(s);
        }
        DragFinish(drop);
        SetWindowTextW(self->cmdCombo_, t.c_str());
        return 0;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, CmdEditSubclass, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT MainWindow::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT shellResult = 0;
    if (HandleShellMenuMessage(msg, wp, lp, &shellResult)) return shellResult;
    switch (msg) {
    case WM_APPCOMMAND: {
        // Maustasten „Zurück“/„Vor“ (5-Tasten-Maus, XButton1/2) und Browser-Tasten der Tastatur: wie die
        // Schaltflächen „<“ und „>“ in der Kopfleiste der aktiven Liste. Kommt von DefWindowProc der Kindfenster.
        int c = GET_APPCOMMAND_LPARAM(lp);
        if (c == APPCOMMAND_BROWSER_BACKWARD || c == APPCOMMAND_BROWSER_FORWARD) {
            SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(c == APPCOMMAND_BROWSER_BACKWARD ? cmd::GoBack : cmd::GoForward, 0), 0);
            return TRUE;
        }
        break;
    }
    case WM_SIZE:
        Layout();
        if (wp == SIZE_MAXIMIZED || (wp == SIZE_RESTORED && wasMaximized_)) ScheduleStateSave();
        if (wp != SIZE_MINIMIZED) wasMaximized_ = wp == SIZE_MAXIMIZED;
        return 0;
    case WM_EXITSIZEMOVE:
        ScheduleStateSave();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize = {DpiScale(hwnd_, 640), DpiScale(hwnd_, 400)};
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        Paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd_) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            Splitter s = HitSplitter(pt);
            if (s != Splitter::None) {
                bool vertical = s == Splitter::RowLeft || s == Splitter::RowRight;
                SetCursor(LoadCursor(nullptr, vertical ? IDC_SIZENS : IDC_SIZEWE));
                return TRUE;
            }
            for (size_t i = 0; i < fkeys_.size(); ++i)
                if (PtInRect(&fkeys_[i].rc, pt)) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        Splitter s = HitSplitter(pt);
        if (s != Splitter::None) {
            dragging_ = s;
            SetCapture(hwnd_);
            return 0;
        }
        if (showFKeys_) {
            for (size_t i = 0; i < fkeys_.size(); ++i)
                if (PtInRect(&fkeys_[i].rc, pt)) {
                    fkeyPressed_ = (int)i;
                    SetCapture(hwnd_);
                    InvalidateRect(hwnd_, &rcFKeys_, FALSE);
                    return 0;
                }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (dragging_ == Splitter::None) return 0;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        UINT dpi = GetWindowDpi(hwnd_);
        auto to96 = [&](int px) { return MulDiv(px, 96, (int)dpi); };
        int sw = DpiScale(hwnd_, kSplitterW);
        switch (dragging_) {
        case Splitter::Tree:
            treeWidth_ = std::clamp(to96(pt.x - sw / 2), 60, 1200);
            break;
        case Splitter::Bookmarks:
            bookmarkWidth_ = std::clamp(to96(pt.x - rcSplitTree_.right - sw / 2), 50, 1000);
            break;
        case Splitter::Columns: {
            int areaL = rcSplitBook_.right, areaW = rcContent_.right - areaL - sw;
            if (areaW > 0) colRatio_ = std::clamp((double)(pt.x - areaL - sw / 2) / areaW, 0.1, 0.9);
            break;
        }
        case Splitter::RowLeft:
        case Splitter::RowRight: {
            int col = dragging_ == Splitter::RowLeft ? 0 : 1;
            int h = rcColumns_[col].bottom - rcColumns_[col].top - sw;
            if (h > 0) rowRatio_[col] = std::clamp((double)(pt.y - rcColumns_[col].top - sw / 2) / h, 0.1, 0.9);
            break;
        }
        default: break;
        }
        Layout();
        UpdateWindow(hwnd_);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (dragging_ != Splitter::None) {
            dragging_ = Splitter::None;
            ReleaseCapture();
            ScheduleStateSave();
            return 0;
        }
        if (fkeyPressed_ >= 0) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int i = fkeyPressed_;
            fkeyPressed_ = -1;
            ReleaseCapture();
            InvalidateRect(hwnd_, &rcFKeys_, FALSE);
            if (i < (int)fkeys_.size() && PtInRect(&fkeys_[i].rc, pt)) {
                Active().FocusList();
                OnCommand(fkeys_[i].command);
            }
        }
        return 0;
    }
    case WM_COMMAND:
        if (lp) {
            // Nur die Werkzeugleiste sendet Befehle; Benachrichtigungen anderer Steuerelemente ignorieren
            if ((HWND)lp == toolbar_) OnCommand(LOWORD(wp));
            return 0;
        }
        OnCommand(LOWORD(wp));
        return 0;
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)lp;
        if (nm->hwndFrom == tree_.Hwnd()) return tree_.OnNotify(nm);
        if (nm->hwndFrom == bookmarks_.Hwnd()) return bookmarks_.OnNotify(nm);
        if (nm->hwndFrom == toolbar_ && nm->code == NM_RCLICK) {
            // Rechtsklick auf die Werkzeugleiste: anpassen bzw. ausblenden
            HMENU m = CreatePopupMenu();
            AddItem(m, cmd::CustomizeToolbar, L"Werkzeugleiste an&passen…");
            AddItem(m, cmd::ToggleToolbar, L"Werkzeugleiste an&zeigen");
            POINT pt;
            GetCursorPos(&pt);
            UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr);
            DestroyMenu(m);
            if (id) OnCommand((int)id);
            return TRUE;
        }
        if (nm->code == TTN_GETDISPINFOW) {
            // Tooltips der Werkzeugleiste (Mauszeiger über einem Symbol)
            auto* di = (NMTTDISPINFOW*)nm;
            for (auto& [id, text] : toolTips_)
                if ((UINT_PTR)id == nm->idFrom) {
                    di->lpszText = const_cast<wchar_t*>(text.c_str());
                    di->hinst = nullptr;
                    break;
                }
            return 0;
        }
        return 0;
    }
    case WM_INITMENUPOPUP:
        UpdateMenu((HMENU)wp);
        return 0;
    case WM_CLIPBOARDUPDATE:
        OnClipboardChanged();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_CLIPBOARD) {
            KillTimer(hwnd_, TIMER_CLIPBOARD);
            OnClipboardChanged();
        } else if (wp == TIMER_SAVESTATE) {
            KillTimer(hwnd_, TIMER_SAVESTATE);
            App::SaveSettings();
        } else if (wp == TIMER_FKEYMODE) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
            int mode = (GetForegroundWindow() == hwnd_ && ctrl && !alt) ? (shift ? 2 : 1) : 0;
            if (mode != fkeyMode_) {
                fkeyMode_ = mode;
                if (mode) fkeyDefs_ = LoadFunctionKeys(App::Cfg());
                BuildFKeyCells();
                InvalidateRect(hwnd_, &rcFKeys_, FALSE);
            }
        } else if (wp == TIMER_REMOTEEDIT) {
            std::wstring msg = RemoteEditPoll(hwnd_);
            if (!msg.empty()) SendMessageW(status_, SB_SETTEXTW, 0, (LPARAM)msg.c_str());
        } else if (wp == TIMER_QUICKVIEW) {
            KillTimer(hwnd_, TIMER_QUICKVIEW);
            if (quickView_ && quickViewPane_ >= 0) {
                FilePane& a = Active();
                std::wstring p = (a.FocusedIsParent() || (a.FocusedIsDir() && a.IsVirtual())) ? L"" : a.FocusedPath();
                if (a.IsNetworkLevel()) p.clear();
                if (a.IsRemote() && !p.empty()) {
                    // FTP/SFTP: Datei (bis 50 MB) zwischenspeichern und anzeigen
                    std::wstring local, err;
                    if (RemoteDownloadTemp(hwnd_, p, local, err, 50ull * 1024 * 1024)) p = local;
                    else p.clear();
                }
                panes_[quickViewPane_]->QuickViewLoad(p);
            }
            UpdateStatusBar();
        }
        return 0;
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            for (auto& p : panes_) p->UpdateDrives();
            tree_.Populate();
            tree_.SelectPath(Active().Dir());
        }
        return TRUE;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        for (auto& p : panes_) p->OnDpiChanged();
        Layout();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && panes_[active_]) {
            HWND f = GetFocus();
            if (!f || !IsChild(hwnd_, f)) PostMessageW(hwnd_, WM_APP + 1, 0, 0);
        }
        break;
    case WM_APP + 2:
        UpdateViewButtons();
        return 0;
    case WM_QF_FAVORITES_CHANGED:
        OnFavoritesChanged(wp != 0);
        return 0;
    case WM_QF_SENDKEY:
        SendKeyStroke((UINT)wp, (lp & 1) != 0, (lp & 2) != 0, (lp & 4) != 0);
        return 0;
    case WM_APP + 1:
        // nur, wenn das Hauptfenster noch aktiv ist: Inzwischen kann ein anderes Fenster dieses Programms aktiv
        // geworden sein (z. B. das Anzeigefenster von „Anzeigen“ in der Archivliste); SetFocus würde das Hauptfenster
        // sonst wieder davor holen.
        if (GetActiveWindow() == hwnd_) Active().FocusList();
        return 0;
    case WM_CLOSE:
        if (!CloseAllTextEditors()) return 0;
        if (!CloseAllHexEditors()) return 0;
        if (App::Opt().saveOnExit) App::SaveSettings();
        else {
            SaveBookmarks(App::Cfg(), bookmarks_.Get());
            App::Cfg().Save();
        }
        DestroyWindow(hwnd_);
        return 0;
    case WM_QUERYENDSESSION:
        App::SaveSettings();
        return TRUE;
    case WM_DESTROY:
        KillTimer(hwnd_, TIMER_FKEYMODE);
        if (accel_) DestroyAcceleratorTable(accel_);
        accel_ = nullptr;
        g_main = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace qf
