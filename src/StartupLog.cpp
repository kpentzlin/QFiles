#include "StartupLog.h"
#include "Util.h"

#include <windows.h>
#include <vector>

namespace qf {

namespace {
struct Mark {
    double ms;
    std::wstring phase;
};
std::vector<Mark> g_marks;
bool g_written = false;

double MsSinceProcessStart() {
    FILETIME create{}, exit{}, kernel{}, user{}, now{};
    GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = create.dwLowDateTime;
    a.HighPart = create.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (double)(b.QuadPart - a.QuadPart) / 10000.0 : 0.0;
}
} // namespace

void StartupMark(const std::wstring& phase) {
    if (!g_written) g_marks.push_back({MsSinceProcessStart(), phase});
}

void StartupLogWrite(const std::wstring& path) {
    if (g_written) return;
    g_written = true;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wstring s = Format(L"QFiles %s – Start am %04u-%02u-%02u %02u:%02u:%02u\r\n", L"" QFILES_VERSION_STRING, st.wYear,
                            st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    s += L"Zeit seit Prozessstart (ms), Dauer des Schritts (ms), Schritt\r\n";
    double prev = 0;
    for (auto& m : g_marks) {
        s += Format(L"%8.0f  %8.0f  ", m.ms, m.ms - prev) + m.phase + L"\r\n";
        prev = m.ms;
    }
    std::string u8 = "\xEF\xBB\xBF" + WideToUtf8(s);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(h);
}

} // namespace qf
