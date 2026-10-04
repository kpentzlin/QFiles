// QFiles – Dateimanager für Windows (64 Bit, Unicode), nachempfunden Idoswin Pro.
// Einstiegspunkt: Initialisierung, Einstellungen laden, Hauptfenster, Nachrichtenschleife.

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <knownfolders.h>

#include "App.h"
#include "MainWindow.h"
#include "Settings.h"
#include "ShellMenu.h"
#include "Util.h"

namespace qf {
bool MainWindowPreTranslate(MSG* msg);
void InstallCrashHandler(const std::wstring& reportDir);
}

using namespace qf;

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    App::SetInstance(hInst);
    // Keine Fehlerdialoge des Systems für nicht bereite Laufwerke
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (FAILED(OleInitialize(nullptr))) {
        MessageBoxW(nullptr, L"COM/OLE konnte nicht initialisiert werden.", L"QFiles", MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc),
                             ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_DATE_CLASSES | ICC_USEREX_CLASSES |
                                 ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES |
                                 ICC_TREEVIEW_CLASSES | ICC_LINK_CLASS | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);

    // Einstellungen: standardmäßig %ProgramData%\QFiles\QFiles.ini
    std::wstring cfgPath = DetermineConfigPath();
    InstallCrashHandler(PathParent(cfgPath));
    bool firstRun = !App::Cfg().Load(cfgPath);
    App::Cfg().SetFilePath(cfgPath);
    App::Opt().Load(App::Cfg());
    PrefetchShellExtensionExclusion();
    if (firstRun) {
        // Erste Lesezeichen als Vorgabe
        std::vector<Bookmark> b;
        auto add = [&](const wchar_t* name, const GUID& id) {
            std::wstring p = GetKnownFolder(id);
            if (!p.empty()) b.push_back({name, p});
        };
        add(L"Desktop", FOLDERID_Desktop);
        add(L"Dokumente", FOLDERID_Documents);
        add(L"Downloads", FOLDERID_Downloads);
        add(L"Bilder", FOLDERID_Pictures);
        SaveBookmarks(App::Cfg(), b);
    }

    MainWindow main;
    if (!main.Create(nCmdShow)) {
        MessageBoxW(nullptr, L"Das Hauptfenster konnte nicht erzeugt werden.", L"QFiles", MB_ICONERROR);
        OleUninitialize();
        return 1;
    }
    if (firstRun) App::SaveSettings();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (MainWindowPreTranslate(&msg)) continue;
        if (App::PreTranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return (int)msg.wParam;
}
