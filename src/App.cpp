#include "App.h"
#include "Dialog.h"
#include "Util.h"
#include "resource.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace qf {

namespace {
HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_smallIcon = nullptr;
HICON g_bigIcon = nullptr;
HFONT g_uiFont = nullptr;
HFONT g_monoFont = nullptr;
Config g_config;
Options g_options;
std::vector<HWND> g_modeless;
std::map<HWND, HACCEL> g_accels;
} // namespace

HINSTANCE App::Instance() { return g_inst; }
void App::SetInstance(HINSTANCE h) { g_inst = h; }
HWND App::MainWindow() { return g_main; }
void App::SetMainWindow(HWND h) { g_main = h; }

HICON App::SmallIcon() {
    if (!g_smallIcon)
        g_smallIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    return g_smallIcon;
}

HICON App::BigIcon() {
    if (!g_bigIcon)
        g_bigIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    return g_bigIcon;
}

Config& App::Cfg() { return g_config; }
Options& App::Opt() { return g_options; }

HFONT App::UIFont() {
    if (!g_uiFont) {
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return g_uiFont;
}

HFONT App::MonoFont() {
    if (!g_monoFont) {
        HDC dc = GetDC(nullptr);
        int h = -MulDiv(10, GetDeviceCaps(dc, LOGPIXELSY), 72);
        ReleaseDC(nullptr, dc);
        g_monoFont = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }
    return g_monoFont;
}

void App::RegisterModeless(HWND h) {
    if (std::find(g_modeless.begin(), g_modeless.end(), h) == g_modeless.end()) g_modeless.push_back(h);
}

void App::UnregisterModeless(HWND h) { g_modeless.erase(std::remove(g_modeless.begin(), g_modeless.end(), h), g_modeless.end()); }

void App::RegisterAccelerator(HWND topLevel, HACCEL accel) { g_accels[topLevel] = accel; }
void App::UnregisterAccelerator(HWND topLevel) { g_accels.erase(topLevel); }

bool App::PreTranslate(MSG* msg) {
    HWND root = msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT) : nullptr;
    if (root) {
        auto it = g_accels.find(root);
        if (it != g_accels.end() && it->second && TranslateAcceleratorW(root, it->second, msg)) return true;
        for (HWND h : g_modeless)
            if (h == root && IsDialogMessageW(h, msg)) return true;
    }
    return false;
}

// ===================== Dateisystemmonitor =====================

namespace {
std::mutex g_logMutex;
std::vector<std::wstring> g_log;
HWND g_logWindow = nullptr;
constexpr int kLogList = 100;

class LogDialog : public DialogBase {
protected:
    BOOL OnInit() override {
        g_logWindow = hwnd_;
        SendMessageW(Item(kLogList), WM_SETFONT, (WPARAM)App::UIFont(), TRUE);
        std::lock_guard<std::mutex> lock(g_logMutex);
        for (auto& l : g_log) SendMessageW(Item(kLogList), LB_ADDSTRING, 0, (LPARAM)l.c_str());
        int n = (int)SendMessageW(Item(kLogList), LB_GETCOUNT, 0, 0);
        SendMessageW(Item(kLogList), LB_SETTOPINDEX, n > 0 ? n - 1 : 0, 0);
        SetAnchor(kLogList, AnchorAll);
        SetAnchor(101, AnchorBottomLeft);
        SetAnchor(102, AnchorBottomLeft);
        SetAnchor(IDCANCEL, AnchorBottomRight);
        EnableResizing();
        return TRUE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == 101) {
            std::lock_guard<std::mutex> lock(g_logMutex);
            g_log.clear();
            SendMessageW(Item(kLogList), LB_RESETCONTENT, 0, 0);
            return TRUE;
        }
        if (id == 102) {
            std::wstring path = SaveFileDialog(hwnd_, L"Protokoll speichern", L"QFiles-Protokoll.txt",
                                               L"Textdateien|*.txt|Alle Dateien|*.*");
            if (!path.empty()) {
                std::wstring text;
                {
                    std::lock_guard<std::mutex> lock(g_logMutex);
                    text = Join(g_log, L"\r\n") + L"\r\n";
                }
                std::string u = WideToUtf8(text);
                if (!WriteFileBytes(path, u.data(), u.size())) MsgError(hwnd_, LastErrorMessage());
            }
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }
    void OnDestroy() override { g_logWindow = nullptr; }
};
} // namespace

void LogOperation(const std::wstring& text) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring line = Format(L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond) + text;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.push_back(line);
        if (g_log.size() > 5000) g_log.erase(g_log.begin(), g_log.begin() + 1000);
    }
    if (g_logWindow) {
        HWND list = GetDlgItem(g_logWindow, kLogList);
        int i = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
        SendMessageW(list, LB_SETTOPINDEX, i, 0);
    }
}

void ShowOperationLog(HWND owner) {
    if (g_logWindow) {
        ShowWindow(g_logWindow, SW_RESTORE);
        SetForegroundWindow(g_logWindow);
        return;
    }
    DialogTemplate t(L"Dateisystemmonitor – Protokoll der Dateioperationen", 400, 220,
                     DialogTemplate::kResizable & ~DS_CENTER);
    t.List(kLogList, 7, 7, 386, 186, LBS_NOSEL | WS_HSCROLL);
    t.Button(101, L"Leeren", 7, 199, 60, 14);
    t.Button(102, L"Speichern…", 72, 199, 60, 14);
    t.Button(IDCANCEL, L"Schließen", 333, 199, 60, 14);
    auto* dlg = new LogDialog();
    dlg->CreateModeless(owner, t, true);
}

} // namespace qf
