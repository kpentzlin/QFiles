// Hängererkennung: Ein Wächter-Thread prüft jede Sekunde, ob das Hauptfenster noch Nachrichten verarbeitet
// (SendMessageTimeout mit WM_NULL). Reagiert es länger als 5 Sekunden nicht, hält er den Haupt-Thread kurz an, liest
// dessen Aufrufstapel und setzt ihn sofort fort; erst danach werden die Adressen in Modul- und Funktionsnamen
// übersetzt (so bleibt der Wächter auch dann frei, wenn der Haupt-Thread gerade eine Sperre hält). Proben nach 5, 15
// und 30 Sekunden, dazu Beginn, Ende und Dauer des Hängers:
//   %ProgramData%\QFiles\QFiles-Haenger.txt (neueste Einträge am Ende, ältere werden gekürzt)
// (Kein Minidump: MiniDumpWriteDump im eigenen Prozess blieb im Test hängen.)
// Funktionsnamen von QFiles erscheinen, wenn QFiles.pdb neben der exe liegt; bei Windows-DLLs genügen die Exporte.

#include <windows.h>
#include <dbghelp.h>
#include <cstdint>
#include <string>
#include <vector>

#include "App.h"
#include "Util.h"

namespace qf {

namespace {

std::wstring g_hangDir;
HANDLE g_mainThread = nullptr;
DWORD g_mainThreadId = 0;
bool g_symInit = false;

std::wstring ModuleOfAddr(DWORD64 addr, DWORD64* base) {
    HMODULE mod = nullptr;
    *base = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(ULONG_PTR)addr, &mod) &&
        mod) {
        wchar_t name[MAX_PATH] = {};
        GetModuleFileNameW(mod, name, MAX_PATH);
        *base = (DWORD64)(ULONG_PTR)mod;
        return PathFileName(name);
    }
    return L"?";
}

// Aufrufstapel des angehaltenen Haupt-Threads: nur Adressen, ohne Speicheranforderung und ohne Sperren
int CaptureMainStack(DWORD64* pcs, int max) {
    int n = 0;
#if defined(_M_X64) || defined(__x86_64__)
    if (SuspendThread(g_mainThread) == (DWORD)-1) return 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(g_mainThread, &ctx)) {
        for (; n < max; ++n) {
            if (!ctx.Rip) break;
            pcs[n] = ctx.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {
                // Blattfunktion: Rücksprungadresse liegt oben auf dem Stapel
                if (!ctx.Rsp) break;
                ctx.Rip = *(DWORD64*)ctx.Rsp;
                ctx.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
    }
    ResumeThread(g_mainThread);
#else
    (void)pcs;
    (void)max;
#endif
    return n;
}

std::wstring Symbolize(const DWORD64* pcs, int n) {
    HANDLE proc = GetCurrentProcess();
    if (!g_symInit) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        g_symInit = SymInitializeW(proc, GetExeDir().c_str(), TRUE) != FALSE;
    } else {
        SymRefreshModuleList(proc);
    }
    std::wstring out;
    for (int i = 0; i < n; ++i) {
        DWORD64 pc = pcs[i], mb = 0;
        std::wstring m = ModuleOfAddr(pc, &mb);
        std::wstring line = Format(L"  %2d  %s+0x%llX", i, m.c_str(), (unsigned long long)(pc - mb));
        if (g_symInit) {
            alignas(SYMBOL_INFOW) char buf[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
            auto* sym = (SYMBOL_INFOW*)buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            if (SymFromAddrW(proc, pc, &disp, sym))
                line += L"  " + std::wstring(sym->Name) + Format(L"+0x%llX", (unsigned long long)disp);
            IMAGEHLP_LINEW64 li{};
            li.SizeOfStruct = sizeof(li);
            DWORD d32 = 0;
            if (SymGetLineFromAddrW64(proc, pc, &d32, &li) && li.FileName)
                line += L"  (" + PathFileName(li.FileName) + L":" + std::to_wstring(li.LineNumber) + L")";
        }
        out += line + L"\r\n";
    }
    return out;
}

std::wstring NowText() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// An die Berichtsdatei anhängen; über 256 KB wird die ältere Hälfte verworfen
void AppendReport(const std::wstring& text) {
    std::wstring path = PathCombine(g_hangDir, L"QFiles-Haenger.txt");
    std::vector<uint8_t> old;
    ReadFileBytes(path, old);
    std::string s(old.begin(), old.end());
    if (s.size() > 256 * 1024) {
        size_t cut = s.find("\r\n\r\n", s.size() / 2);
        s = cut == std::string::npos ? std::string() : s.substr(cut + 4);
    }
    s += WideToUtf8(text);
    WriteFileBytes(path, s.data(), s.size());
}

DWORD WINAPI WatchThread(void*) {
    const DWORD kProbeMs = 1000;        // Abstand der Prüfungen
    const DWORD kAnswerMs = 1000;       // so lange darf eine Prüfung dauern
    const DWORD sampleAt[] = {5, 15, 30};   // Sekunden ohne Rückmeldung bis zur jeweiligen Probe
    ULONGLONG hangStart = 0;
    int samples = 0;
    for (;;) {
        Sleep(kProbeMs);
        HWND main = App::MainWindow();
        if (!main || !IsWindow(main)) {
            hangStart = 0;
            continue;
        }
        DWORD_PTR res = 0;
        bool ok = SendMessageTimeoutW(main, WM_NULL, 0, 0, SMTO_NORMAL, kAnswerMs, &res) != 0;
        ULONGLONG now = GetTickCount64();
        if (ok) {
            if (hangStart && samples > 0) {
                ULONGLONG secs = (now - hangStart) / 1000;
                AppendReport(L"Ende: " + NowText() + Format(L" – Dauer etwa %llu s\r\n\r\n", (unsigned long long)secs));
            }
            hangStart = 0;
            samples = 0;
            continue;
        }
        if (!hangStart) {
            hangStart = now - kAnswerMs;   // seit Beginn der unbeantworteten Prüfung
            samples = 0;
        }
        DWORD secs = (DWORD)((now - hangStart) / 1000);
        if (samples < (int)(sizeof(sampleAt) / sizeof(sampleAt[0])) && secs >= sampleAt[samples]) {
            DWORD64 pcs[64] = {};
            int n = CaptureMainStack(pcs, 64);
            std::wstring text;
            if (samples == 0)
                text = L"QFiles " QFILES_VERSION_STRING L" – Hänger: Hauptfenster reagiert nicht (bemerkt " + NowText() +
                       L")\r\n";
            text += Format(L"Probe %d nach %u s – Aufrufstapel des Haupt-Threads:\r\n", samples + 1, secs) + Symbolize(pcs, n);
            AppendReport(text);
            ++samples;
        }
    }
}

} // namespace

void StartHangWatch(const std::wstring& reportDir) {
    if (GetEnvironmentVariableW(L"QFILES_NO_HANGWATCH", nullptr, 0)) return;
    g_hangDir = reportDir;
    g_mainThreadId = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (!g_mainThread) return;
    HANDLE h = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(h);
    }
}

} // namespace qf
