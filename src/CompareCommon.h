#pragma once
// Interne Hilfsfunktionen für Modul C (Verzeichnisvergleich und Synchronisieren).
// Nur von CompareDirs.cpp und SyncDirs.cpp eingebunden.

#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "Util.h"
#include "App.h"

namespace qf::cmpdetail {

// FILETIME als 64-Bit-Zahl (100-ns-Einheiten)
inline long long FileTimeToInt(const FILETIME& f) {
    return (long long)(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime);
}

// Vergleicht zwei Änderungszeiten.
// Rückgabe: 1 = a neuer, -1 = a älter, 0 = gleich (innerhalb der Toleranz).
// tolerance2s: Abweichungen bis 2 s gelten als gleich (FAT, Netzlaufwerke).
// ignoreHour: eine Abweichung von genau 1 h (± Toleranz) gilt als gleich (Zeitzone/Sommerzeit).
inline int CompareFileTimes(const FILETIME& a, const FILETIME& b, bool tolerance2s, bool ignoreHour) {
    const long long kSecond = 10000000LL;
    long long d = FileTimeToInt(a) - FileTimeToInt(b);
    long long ad = d < 0 ? -d : d;
    long long tol = tolerance2s ? 2 * kSecond : 0;
    if (ad <= tol) return 0;
    if (ignoreHour) {
        long long h = ad - 3600 * kSecond;
        if (h < 0) h = -h;
        // ohne Toleranz trotzdem Sekundenbruchteile zulassen (Rundung beim Kopieren)
        if (h <= (tol ? tol : kSecond)) return 0;
    }
    return d > 0 ? 1 : -1;
}

enum class ContentResult { Equal, Different, Error, Cancelled };

// Byteweiser Inhaltsvergleich zweier Dateien in Blöcken (max. 1 MB). Abbruch über cancel.
// bytesDone (optional) wird laufend um die verglichenen Bytes erhöht.
inline ContentResult CompareFileContents(const std::wstring& a, const std::wstring& b, const std::atomic<bool>& cancel,
                                         std::atomic<uint64_t>* bytesDone = nullptr) {
    HANDLE ha = CreateFileW(LongPath(a).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (ha == INVALID_HANDLE_VALUE) return ContentResult::Error;
    HANDLE hb = CreateFileW(LongPath(b).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hb == INVALID_HANDLE_VALUE) {
        CloseHandle(ha);
        return ContentResult::Error;
    }
    LARGE_INTEGER sa{}, sb{};
    GetFileSizeEx(ha, &sa);
    GetFileSizeEx(hb, &sb);
    ContentResult result = ContentResult::Equal;
    if (sa.QuadPart != sb.QuadPart) {
        result = ContentResult::Different;
    } else {
        const size_t kBlock = 1 << 20;
        size_t bufSize = (size_t)std::min<long long>((long long)kBlock, std::max<long long>(sa.QuadPart, 1));
        std::vector<uint8_t> ba(bufSize), bb(bufSize);
        for (;;) {
            if (cancel.load(std::memory_order_relaxed)) {
                result = ContentResult::Cancelled;
                break;
            }
            DWORD ra = 0, rb = 0;
            if (!ReadFile(ha, ba.data(), (DWORD)bufSize, &ra, nullptr) ||
                !ReadFile(hb, bb.data(), (DWORD)bufSize, &rb, nullptr)) {
                result = ContentResult::Error;
                break;
            }
            if (ra != rb) {
                result = ContentResult::Different;
                break;
            }
            if (ra == 0) break;
            if (std::memcmp(ba.data(), bb.data(), ra) != 0) {
                result = ContentResult::Different;
                break;
            }
            if (bytesDone) bytesDone->fetch_add(ra, std::memory_order_relaxed);
        }
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return result;
}

// Dateifilter ohne Wirkung? ("", "*", "*.*")
inline bool IsTrivialPattern(const std::wstring& p) {
    std::wstring t = Trim(p);
    return t.empty() || t == L"*" || t == L"*.*";
}

// DPI-unabhängige Fenstergröße (bezogen auf 96 DPI) merken bzw. wiederherstellen
inline void SaveWindowSize(HWND h, const std::wstring& section) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(h, &wp)) return;
    UINT dpi = GetWindowDpi(h);
    if (dpi == 0) dpi = 96;
    RECT r = wp.rcNormalPosition;
    App::Cfg().SetInt(section, L"Width", MulDiv(r.right - r.left, 96, (int)dpi));
    App::Cfg().SetInt(section, L"Height", MulDiv(r.bottom - r.top, 96, (int)dpi));
    App::Cfg().SetBool(section, L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED);
}

// Stellt die gemerkte Größe eines (zentrierten) Dialogs wieder her; nie kleiner als die aktuelle Größe.
inline void RestoreDialogSize(HWND h, const std::wstring& section) {
    int w = App::Cfg().GetInt(section, L"Width", 0), hh = App::Cfg().GetInt(section, L"Height", 0);
    RECT r;
    GetWindowRect(h, &r);
    int cw = r.right - r.left, ch = r.bottom - r.top;
    if (w > 0 && hh > 0) {
        int nw = std::max(cw, DpiScale(h, w)), nh = std::max(ch, DpiScale(h, hh));
        // auf den Arbeitsbereich des Monitors begrenzen und zentriert lassen
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            nw = std::min<int>(nw, mi.rcWork.right - mi.rcWork.left);
            nh = std::min<int>(nh, mi.rcWork.bottom - mi.rcWork.top);
            int x = r.left - (nw - cw) / 2, y = r.top - (nh - ch) / 2;
            x = std::max<int>(mi.rcWork.left, std::min<int>(x, mi.rcWork.right - nw));
            y = std::max<int>(mi.rcWork.top, std::min<int>(y, mi.rcWork.bottom - nh));
            SetWindowPos(h, nullptr, x, y, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

} // namespace qf::cmpdetail
