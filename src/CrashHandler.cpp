// Absturzbehandlung: Bei einer unbehandelten Ausnahme wird ein Bericht mit Stacktrace
// (Funktionsnamen und Zeilen, sofern die PDB-Datei neben der exe liegt) sowie ein Minidump geschrieben:
//   %ProgramData%\QFiles\QFiles-Absturz.txt und QFiles-Absturz.dmp (neben der Einstellungsdatei)

#include <windows.h>
#include <dbghelp.h>
#include <string>

#include "App.h"
#include "Util.h"

namespace qf {

namespace {

std::wstring g_reportDir;

std::wstring ModuleOf(DWORD64 addr, DWORD64* base) {
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

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;

    std::wstring dir = g_reportDir.empty() ? GetTempDir() : g_reportDir;
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring txtPath = PathCombine(dir, L"QFiles-Absturz.txt");
    std::wstring dmpPath = PathCombine(dir, L"QFiles-Absturz.dmp");

    std::wstring report = L"QFiles " QFILES_VERSION_STRING L" – Absturzbericht\r\n";
    SYSTEMTIME st;
    GetLocalTime(&st);
    report += Format(L"Zeit: %04d-%02d-%02d %02d:%02d:%02d\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond);
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    DWORD64 base = 0;
    std::wstring mod = ModuleOf((DWORD64)(ULONG_PTR)er->ExceptionAddress, &base);
    report += Format(L"Ausnahme: 0x%08X an %s+0x%llX\r\n", (unsigned)er->ExceptionCode, mod.c_str(),
                     (unsigned long long)((DWORD64)(ULONG_PTR)er->ExceptionAddress - base));
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        report += Format(L"Zugriffsverletzung (%s) bei Adresse 0x%llX\r\n",
                         er->ExceptionInformation[0] == 0 ? L"Lesen" : (er->ExceptionInformation[0] == 1 ? L"Schreiben" : L"Ausführen"),
                         (unsigned long long)er->ExceptionInformation[1]);
    report += L"\r\nAufrufstapel:\r\n";

    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    std::wstring searchPath = GetExeDir();
    bool symOk = SymInitializeW(proc, searchPath.c_str(), TRUE) != FALSE;

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
#if defined(_M_X64) || defined(__x86_64__)
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#else
    DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = ctx.Pc;
    frame.AddrFrame.Offset = ctx.Fp;
    frame.AddrStack.Offset = ctx.Sp;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(machine, proc, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr))
            break;
        DWORD64 pc = frame.AddrPC.Offset;
        if (!pc) break;
        DWORD64 mb = 0;
        std::wstring m = ModuleOf(pc, &mb);
        std::wstring line = Format(L"  %2d  %s+0x%llX", i, m.c_str(), (unsigned long long)(pc - mb));
        if (symOk) {
            alignas(SYMBOL_INFOW) char buf[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
            auto* sym = (SYMBOL_INFOW*)buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            if (SymFromAddrW(proc, pc, &disp, sym)) line += L"  " + std::wstring(sym->Name);
            IMAGEHLP_LINEW64 li{};
            li.SizeOfStruct = sizeof(li);
            DWORD d32 = 0;
            if (SymGetLineFromAddrW64(proc, pc, &d32, &li) && li.FileName)
                line += L"  (" + PathFileName(li.FileName) + L":" + std::to_wstring(li.LineNumber) + L")";
        }
        report += line + L"\r\n";
    }
    if (symOk) SymCleanup(proc);

    std::string u = WideToUtf8(report);
    WriteFileBytes(txtPath, u.data(), u.size());

    HANDLE f = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(proc, GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    // Ausgabe auch auf stderr (für automatische Tests)
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), u.data(), (DWORD)u.size(), &written, nullptr);
    if (!GetEnvironmentVariableW(L"QFILES_NO_CRASH_DIALOG", nullptr, 0)) {
        std::wstring msg = L"QFiles wurde wegen eines Programmfehlers beendet.\n\nEin Bericht wurde gespeichert:\n" + txtPath +
                           L"\n\nBitte diese Datei an den Entwickler senden.";
        MessageBoxW(nullptr, msg.c_str(), L"QFiles – Absturz", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void InstallCrashHandler(const std::wstring& reportDir) {
    g_reportDir = reportDir;
    SetUnhandledExceptionFilter(CrashFilter);
}

} // namespace qf
