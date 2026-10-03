// Modul E – Programmierbare Funktionstasten (Strg+F1…F12, Strg+Umschalt+F1…F12).
//
// Platzhalter in Parametern und Arbeitsverzeichnis:
//   %P aktuelles Verzeichnis            %O Verzeichnis der Gegenseite
//   %F voller Pfad der Fokusdatei       %N Name der Fokusdatei       %E Name ohne Erweiterung
//   %S markierte Dateien, voller Pfad, je in Anführungszeichen (ohne Markierung: Fokusdatei)
//   %M markierte Namen in Anführungszeichen (ohne Markierung: Fokusdatei)
//   %L Pfad einer temporären UTF-8-Listendatei (eine Zeile je markierter Datei, voller Pfad)
//   %% Prozentzeichen

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Settings.h"
#include "Util.h"
#include "ToolsCommon.h"

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

namespace qf {

namespace {

std::wstring KeyName(int index) {
    if (index < 12) return L"Strg+F" + std::to_wstring(index + 1);
    return L"Strg+Umschalt+F" + std::to_wstring(index - 11);
}

// Alte Listendateien (älter als ein Tag) im Temp-Verzeichnis entfernen.
void CleanupOldListFiles(const std::wstring& tempDir) {
    std::vector<DirEntry> entries;
    if (!ListDirectory(tempDir, entries)) return;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    const unsigned long long oneDay = 24ull * 3600ull * 10000000ull;
    for (const auto& e : entries) {
        if (e.IsDir() || !StartsWithI(e.name, L"QFiles_Liste_") || !EndsWithI(e.name, L".txt")) continue;
        ULARGE_INTEGER t;
        t.LowPart = e.modified.dwLowDateTime;
        t.HighPart = e.modified.dwHighDateTime;
        if (now.QuadPart > t.QuadPart && now.QuadPart - t.QuadPart > oneDay)
            DeleteFileW(LongPath(PathCombine(tempDir, e.name)).c_str());
    }
}

// Schreibt die Listendatei für %L (UTF-8 ohne BOM, CRLF). Rückgabe: Pfad oder "" bei Fehler.
std::wstring WriteListFile(const std::vector<std::wstring>& paths) {
    std::wstring tempDir = GetTempDir();
    CleanupOldListFiles(tempDir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring name = Format(L"QFiles_Liste_%04d%02d%02d_%02d%02d%02d_%u.txt", st.wYear, st.wMonth, st.wDay, st.wHour,
                               st.wMinute, st.wSecond, (unsigned)(GetTickCount() % 100000));
    name = MakeUniqueName(tempDir, name);
    std::wstring path = PathCombine(tempDir, name);
    std::wstring text;
    for (const auto& p : paths) text += p + L"\r\n";
    std::string bytes = WideToUtf8(text);
    if (!WriteFileBytes(path, bytes.data(), bytes.size())) return L"";
    return path;
}

// Ersetzt die Platzhalter. listFile wird bei Bedarf (einmalig) erzeugt.
std::wstring ExpandPlaceholders(const std::wstring& s, const PaneContext& ctx, std::wstring& listFile, bool& ok) {
    std::wstring r;
    const std::wstring focusPath = ctx.focused.empty() ? L"" : PathCombine(ctx.dir, ctx.focused);
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c != L'%' || i + 1 >= s.size()) {
            r += c;
            continue;
        }
        wchar_t k = (wchar_t)towupper(s[i + 1]);
        switch (k) {
        case L'%': r += L'%'; break;
        case L'P': r += ctx.dir; break;
        case L'O': r += ctx.otherDir; break;
        case L'F': r += focusPath; break;
        case L'N': r += ctx.focused; break;
        case L'E': r += ctx.focused.empty() ? L"" : PathStem(ctx.focused); break;
        case L'S': {
            std::wstring list;
            for (const auto& n : ctx.SelectedOrFocused()) {
                if (!list.empty()) list += L' ';
                list += L"\"" + PathCombine(ctx.dir, n) + L"\"";
            }
            r += list;
            break;
        }
        case L'M': {
            std::wstring list;
            for (const auto& n : ctx.SelectedOrFocused()) {
                if (!list.empty()) list += L' ';
                list += L"\"" + n + L"\"";
            }
            r += list;
            break;
        }
        case L'L': {
            if (listFile.empty()) {
                std::vector<std::wstring> paths;
                for (const auto& n : ctx.SelectedOrFocused()) paths.push_back(PathCombine(ctx.dir, n));
                listFile = WriteListFile(paths);
                if (listFile.empty()) ok = false;
            }
            r += listFile;
            break;
        }
        default:
            // Unbekannter Platzhalter (z. B. Umgebungsvariable): unverändert übernehmen
            r += c;
            r += s[i + 1];
            break;
        }
        ++i;
    }
    return r;
}

std::wstring ExpandEnv(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (!n) return s;
    std::wstring r(n, L'\0');
    ExpandEnvironmentStringsW(s.c_str(), r.data(), n);
    r.resize(n - 1);
    return r;
}

// ===================================================================================
// Konfigurationsdialog
// ===================================================================================

class FunctionKeysDlg : public DialogBase {
public:
    static constexpr int kList = 100;
    static constexpr int kLabel = 110;
    static constexpr int kProgram = 111;
    static constexpr int kProgramBrowse = 112;
    static constexpr int kParams = 113;
    static constexpr int kDir = 114;
    static constexpr int kDirBrowse = 115;
    static constexpr int kConsole = 116;
    static constexpr int kClear = 117;
    static constexpr int kHelp = 118;
    static constexpr int kCaption = 119;
    static constexpr int kTest = 120;

    std::vector<FunctionKey> keys;

protected:
    BOOL OnInit() override {
        LvAddColumn(kList, L"Taste", 80);
        LvAddColumn(kList, L"Beschriftung", 90);
        LvAddColumn(kList, L"Programm", 140);
        LvAddColumn(kList, L"Parameter", 100);
        for (int i = 0; i < kFunctionKeyCount; ++i)
            LvAddRow(kList, {KeyName(i), keys[i].label, keys[i].program, keys[i].params}, i);
        SetText(kHelp,
                L"Platzhalter (in Parametern und Arbeitsverzeichnis):  %P aktuelles Verzeichnis,  %O Verzeichnis der "
                L"Gegenseite,  %F Fokusdatei mit Pfad,  %N Name der Fokusdatei,  %E Name ohne Erweiterung,  %S markierte "
                L"Dateien mit Pfad (in Anführungszeichen),  %M markierte Namen (in Anführungszeichen),  %L temporäre "
                L"Listendatei (UTF-8, ein Pfad je Zeile),  %% Prozentzeichen.  Programm kann auch ein Dokument, eine "
                L"Verknüpfung oder eine URL sein. Leeres Arbeitsverzeichnis = aktuelles Verzeichnis.");

        SetAnchor(kList, AnchorAll);
        const int bottomLeftRight[] = {kLabel, kProgram, kParams, kDir, kHelp};
        for (int id : bottomLeftRight) SetAnchor(id, AnchorBottomLeftRight);
        const int bottomRight[] = {kProgramBrowse, kDirBrowse, IDOK, IDCANCEL};
        for (int id : bottomRight) SetAnchor(id, AnchorBottomRight);
        const int bottomLeft[] = {kConsole, kClear, kCaption, kTest, 201, 202, 203, 204};
        for (int id : bottomLeft) SetAnchor(id, AnchorBottomLeft);
        EnableResizing();

        HWND lv = Item(kList);
        ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        LoadFields(0);
        SetFocus(lv);
        return FALSE;
    }

    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (loading_) return FALSE;
        switch (id) {
        case kLabel:
        case kProgram:
        case kParams:
        case kDir:
            if (code == EN_CHANGE) StoreFields();
            return TRUE;
        case kConsole:
            if (code == BN_CLICKED) StoreFields();
            return TRUE;
        case kProgramBrowse: {
            std::wstring cur = GetText(kProgram);
            std::wstring f = OpenFileDialog(hwnd_, L"Programm auswählen", cur.empty() ? L"" : PathParent(cur),
                                            L"Programme|*.exe;*.com;*.bat;*.cmd;*.ps1;*.lnk|Alle Dateien|*.*");
            if (!f.empty()) {
                SetText(kProgram, f);
                if (GetText(kLabel).empty()) SetText(kLabel, PathStem(f));
            }
            return TRUE;
        }
        case kDirBrowse: {
            std::wstring d = BrowseForFolder(hwnd_, L"Arbeitsverzeichnis auswählen", GetText(kDir));
            if (!d.empty()) SetText(kDir, d);
            return TRUE;
        }
        case kClear:
            if (current_ >= 0) {
                keys[current_] = FunctionKey{};
                LoadFields(current_);
                UpdateRow(current_);
            }
            return TRUE;
        case kTest:
            if (current_ >= 0) {
                if (keys[current_].IsEmpty()) {
                    MessageBeep(MB_ICONWARNING);
                    return TRUE;
                }
                // Test mit dem Kontext der aktiven Liste; dafür vorübergehend die aktuelle Belegung nutzen
                PaneContext ctx = App::ActivePaneContext();
                RunKey(hwnd_, current_, keys[current_], ctx);
            }
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }

    INT_PTR OnNotify(NMHDR* nm) override {
        if (nm->idFrom == kList && nm->code == LVN_ITEMCHANGED) {
            auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
            if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED))
                LoadFields((int)lv->lParam);
        }
        return 0;
    }

public:
    // Führt eine (ggf. noch nicht gespeicherte) Belegung aus.
    static void RunKey(HWND owner, int index, const FunctionKey& key, const PaneContext& ctx);

private:
    void LoadFields(int index) {
        if (index < 0 || index >= kFunctionKeyCount) return;
        loading_ = true;
        current_ = index;
        const FunctionKey& k = keys[index];
        SetText(kCaption, L"Belegung für " + KeyName(index) + L":");
        SetText(kLabel, k.label);
        SetText(kProgram, k.program);
        SetText(kParams, k.params);
        SetText(kDir, k.startDir);
        SetCheck(kConsole, k.console);
        loading_ = false;
    }
    void StoreFields() {
        if (current_ < 0) return;
        FunctionKey& k = keys[current_];
        k.label = GetText(kLabel);
        k.program = Trim(GetText(kProgram));
        k.params = GetText(kParams);
        k.startDir = Trim(GetText(kDir));
        k.console = IsChecked(kConsole);
        UpdateRow(current_);
    }
    void UpdateRow(int index) {
        HWND lv = Item(kList);
        LVFINDINFOW fi{};
        fi.flags = LVFI_PARAM;
        fi.lParam = index;
        int row = ListView_FindItem(lv, -1, &fi);
        if (row < 0) return;
        ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(keys[index].label.c_str()));
        ListView_SetItemText(lv, row, 2, const_cast<wchar_t*>(keys[index].program.c_str()));
        ListView_SetItemText(lv, row, 3, const_cast<wchar_t*>(keys[index].params.c_str()));
    }

    int current_ = -1;
    bool loading_ = false;
};

void FunctionKeysDlg::RunKey(HWND owner, int index, const FunctionKey& key, const PaneContext& ctx) {
    if (key.program.empty()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    std::wstring listFile;
    bool ok = true;
    std::wstring program = ExpandEnv(key.program);
    std::wstring params = ExpandPlaceholders(key.params, ctx, listFile, ok);
    std::wstring dir = key.startDir.empty() ? ctx.dir : ExpandEnv(ExpandPlaceholders(key.startDir, ctx, listFile, ok));
    if (!ok) {
        MsgError(owner, L"Die temporäre Listendatei (%L) konnte nicht angelegt werden.");
        return;
    }
    if (!dir.empty() && !DirExists(dir)) dir = ctx.dir;

    std::wstring caption = KeyName(index);
    if (!key.label.empty()) caption += L" (" + key.label + L")";
    LogOperation(L"Funktionstaste " + caption + L": " + program + (params.empty() ? L"" : L" " + params) +
                 (key.console ? L"  [Konsole]" : L""));

    if (key.console) {
        // cmd /k "programm params" – cmd entfernt das äußere Anführungszeichenpaar
        wchar_t comspec[MAX_PATH] = L"";
        std::wstring cmdExe;
        if (GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH) && comspec[0])
            cmdExe = comspec;
        else {
            wchar_t sys[MAX_PATH] = L"";
            GetSystemDirectoryW(sys, MAX_PATH);
            cmdExe = PathCombine(sys, L"cmd.exe");
        }
        std::wstring inner = toolsdetail::QuoteIfNeeded(program);
        if (!params.empty()) inner += L" " + params;
        std::wstring cmdLine = L"\"" + cmdExe + L"\" /k \"" + inner + L"\"";
        if (!RunProcess(cmdLine, dir)) MsgError(owner, L"Die Konsole konnte nicht gestartet werden:\n" + LastErrorMessage());
    } else {
        ShellOpen(owner, program, params, dir);
    }
}

} // namespace

bool ConfigureFunctionKeys(HWND owner) {
    DialogTemplate t(L"Funktionstasten belegen", 420, 300, DialogTemplate::kResizable);
    t.ListView(FunctionKeysDlg::kList, 7, 7, 406, 120, LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL);
    t.Label(FunctionKeysDlg::kCaption, L"", 7, 133, 200, 10);
    t.Label(201, L"&Beschriftung:", 7, 149, 70, 10);
    t.Edit(FunctionKeysDlg::kLabel, 80, 147, 333, 13);
    t.Label(202, L"&Programm:", 7, 166, 70, 10);
    t.Edit(FunctionKeysDlg::kProgram, 80, 164, 313, 13);
    t.Button(FunctionKeysDlg::kProgramBrowse, L"…", 396, 164, 17, 13);
    t.Label(203, L"P&arameter:", 7, 183, 70, 10);
    t.Edit(FunctionKeysDlg::kParams, 80, 181, 333, 13);
    t.Label(204, L"Arbeits&verzeichnis:", 7, 200, 70, 10);
    t.Edit(FunctionKeysDlg::kDir, 80, 198, 313, 13);
    t.Button(FunctionKeysDlg::kDirBrowse, L"…", 396, 198, 17, 13);
    t.Check(FunctionKeysDlg::kConsole, L"In &Konsolenfenster ausführen (cmd /k)", 80, 216, 200, 10);
    t.Label(FunctionKeysDlg::kHelp, L"", 7, 232, 406, 40);
    t.Button(FunctionKeysDlg::kClear, L"Belegung &löschen", 7, 279, 70, 14);
    t.Button(FunctionKeysDlg::kTest, L"&Testen", 82, 279, 50, 14);
    t.DefButton(IDOK, L"OK", 306, 279, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 363, 279, 50, 14);

    FunctionKeysDlg dlg;
    dlg.keys = LoadFunctionKeys(App::Cfg());
    dlg.keys.resize(kFunctionKeyCount);
    if (dlg.DoModal(owner, t) != IDOK) return false;
    SaveFunctionKeys(App::Cfg(), dlg.keys);
    if (!App::Cfg().Save()) MsgError(owner, L"Die Einstellungen konnten nicht gespeichert werden:\n" + App::Cfg().FilePath());
    return true;
}

void ExecuteFunctionKey(HWND owner, int index, const PaneContext& ctx) {
    if (index < 0 || index >= kFunctionKeyCount) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    std::vector<FunctionKey> keys = LoadFunctionKeys(App::Cfg());
    if (index >= (int)keys.size() || keys[index].IsEmpty()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    FunctionKeysDlg::RunKey(owner, index, keys[index], ctx);
}

} // namespace qf
