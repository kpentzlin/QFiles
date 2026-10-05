#include "Util.h"
#include "Dialog.h"

#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <commctrl.h>
#include <cstdarg>
#include <cwchar>
#include <algorithm>

// shlwapi.h definiert Makros, die mit qf::PathCombine/qf::StrToInt kollidieren
#undef PathCombine
#undef StrToInt

namespace qf {

// ===================== Zeichenketten =====================

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}

static std::wstring MapCase(std::wstring_view s, DWORD flag) {
    if (s.empty()) return {};
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, flag, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return std::wstring(s);
    std::wstring r(n, L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, flag, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr, 0);
    return r;
}

std::wstring ToLower(std::wstring_view s) { return MapCase(s, LCMAP_LOWERCASE); }
std::wstring ToUpper(std::wstring_view s) { return MapCase(s, LCMAP_UPPERCASE); }

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

int CompareI(std::wstring_view a, std::wstring_view b) {
    // Ordinaler Vergleich ohne Groß-/Kleinschreibung – genau wie das Dateisystem.
    // Leere Ansichten (data() kann nullptr sein) nicht an die API geben: sie liefert dann 0 (Fehler).
    if (a.empty() || b.empty()) return a.empty() ? (b.empty() ? 0 : -1) : 1;
    int r = CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE);
    return r - 2;
}

bool EqualsI(std::wstring_view a, std::wstring_view b) { return a.size() == b.size() && CompareI(a, b) == 0; }

int CompareNatural(const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()); }

bool StartsWithI(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && CompareI(s.substr(0, prefix.size()), prefix) == 0;
}

bool EndsWithI(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && CompareI(s.substr(s.size() - suffix.size()), suffix) == 0;
}

std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep, bool skipEmpty) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(sep, start);
        if (p == std::wstring_view::npos) p = s.size();
        std::wstring part(s.substr(start, p - start));
        if (!skipEmpty || !part.empty()) out.push_back(std::move(part));
        start = p + 1;
    }
    return out;
}

std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view sep) {
    std::wstring r;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) r += sep;
        r += parts[i];
    }
    return r;
}

std::wstring ReplaceAll(std::wstring s, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::wstring Format(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = _vscwprintf(fmt, copy);
    va_end(copy);
    std::wstring r;
    if (n > 0) {
        r.resize(n + 1);
        _vsnwprintf_s(r.data(), n + 1, _TRUNCATE, fmt, args);
        r.resize(n);
    }
    va_end(args);
    return r;
}

std::wstring IntToStr(long long v) { return std::to_wstring(v); }

std::wstring IntToStrGrouped(unsigned long long v) {
    std::wstring digits = std::to_wstring(v);
    wchar_t sep[8] = L".";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, 8);
    std::wstring r;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) r.insert(0, sep);
        r.insert(r.begin(), *it);
        ++count;
    }
    return r;
}

long long StrToInt(std::wstring_view s, long long def) {
    std::wstring t = Trim(s);
    if (t.empty()) return def;
    wchar_t* end = nullptr;
    long long v = wcstoll(t.c_str(), &end, 10);
    if (end == t.c_str()) return def;
    return v;
}

static bool WildRec(const wchar_t* p, const wchar_t* pe, const wchar_t* s, const wchar_t* se) {
    while (p < pe) {
        if (*p == L'*') {
            while (p < pe && *p == L'*') ++p;
            if (p == pe) return true;
            for (const wchar_t* t = s; t <= se; ++t)
                if (WildRec(p, pe, t, se)) return true;
            return false;
        }
        if (s == se) return false;
        if (*p != L'?' && CompareStringOrdinal(p, 1, s, 1, TRUE) != CSTR_EQUAL) return false;
        ++p;
        ++s;
    }
    return s == se;
}

bool WildcardMatch(std::wstring_view pattern, std::wstring_view name) {
    // "*.*" passt wie unter Windows auch auf Namen ohne Punkt.
    if (pattern == L"*.*" || pattern == L"*") return true;
    // "*." passt wie unter Windows auf Namen ohne Erweiterung
    if (pattern == L"*.") return name.find(L'.') == std::wstring_view::npos;
    return WildRec(pattern.data(), pattern.data() + pattern.size(), name.data(), name.data() + name.size());
}

bool MatchAnyPattern(const std::wstring& patterns, const std::wstring& name) {
    auto list = Split(patterns, L';');
    if (list.empty()) return true;
    for (auto& p : list) {
        std::wstring t = Trim(p);
        if (!t.empty() && WildcardMatch(t, name)) return true;
    }
    return false;
}

// ===================== Pfade =====================

std::wstring PathCombine(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
}

std::wstring PathParent(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    if (IsRootPath(p)) return L"";
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    std::wstring parent = p.substr(0, pos);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    // UNC: \\server\share bleibt Wurzel
    if (parent.size() <= 2 && StartsWithI(p, L"\\\\")) return L"";
    if (StartsWithI(p, L"\\\\")) {
        // Anzahl der Glieder prüfen: \\server\share\x -> \\server\share
        auto parts = Split(parent.substr(2), L'\\');
        if (parts.size() < 2) return L"";
    }
    return parent;
}

std::wstring PathFileName(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return p;
    return p.substr(pos + 1);
}

std::wstring PathExtension(const std::wstring& path) {
    std::wstring name = PathFileName(path);
    size_t pos = name.find_last_of(L'.');
    if (pos == std::wstring::npos || pos == 0) return L"";
    return name.substr(pos);
}

std::wstring PathStem(const std::wstring& path) {
    std::wstring name = PathFileName(path);
    size_t pos = name.find_last_of(L'.');
    if (pos == std::wstring::npos || pos == 0) return name;
    return name.substr(0, pos);
}

std::wstring PathRoot(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return path.substr(0, 2) + L"\\";
    if (StartsWithI(path, L"\\\\")) {
        auto parts = Split(path.substr(2), L'\\');
        if (parts.size() >= 2) return L"\\\\" + parts[0] + L"\\" + parts[1] + L"\\";
    }
    return L"";
}

bool IsRootPath(const std::wstring& path) {
    if (path.size() == 2 && path[1] == L':') return true;
    if (path.size() == 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) return true;
    if (StartsWithI(path, L"\\\\")) {
        auto parts = Split(path.substr(2), L'\\');
        return parts.size() <= 2;
    }
    return false;
}

std::wstring NormalizeDir(const std::wstring& path) {
    std::wstring p = Trim(path);
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
    for (auto& c : p) if (c == L'/') c = L'\\';
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n) {
        std::wstring full(n, L'\0');
        DWORD m = GetFullPathNameW(p.c_str(), n, full.data(), nullptr);
        full.resize(m);
        p = full;
    }
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    if (p.size() == 3 && p[1] == L':') p[0] = towupper(p[0]);
    return p;
}

std::wstring LongPath(const std::wstring& path) {
    if (path.size() < MAX_PATH - 12) return path;
    if (StartsWithI(path, L"\\\\?\\")) return path;
    if (StartsWithI(path, L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring CanonicalPath(const std::wstring& path) {
    if (path.empty()) return path;
    std::wstring full = path;
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (n) {
        std::wstring buf(n, L'\0');
        DWORD m = GetFullPathNameW(path.c_str(), n, buf.data(), nullptr);
        if (m && m < n) {
            buf.resize(m);
            full = buf;
        }
    }
    if (full.find(L'~') == std::wstring::npos) return full;
    std::wstring lp = LongPath(full);
    bool prefixed = lp.size() != full.size();
    DWORD len = GetLongPathNameW(lp.c_str(), nullptr, 0);
    if (!len) return full;
    std::wstring out(len, L'\0');
    DWORD got = GetLongPathNameW(lp.c_str(), out.data(), len);
    if (!got || got >= len) return full;
    out.resize(got);
    if (prefixed) {
        if (StartsWithI(out, L"\\\\?\\UNC\\")) out = L"\\\\" + out.substr(8);
        else if (StartsWithI(out, L"\\\\?\\")) out = out.substr(4);
    }
    return out;
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(LongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring LastPathElement(const std::wstring& dir) {
    if (IsRootPath(dir)) {
        if (dir.size() >= 2 && dir[1] == L':') return dir.substr(0, 2);
        std::wstring t = dir;
        while (!t.empty() && t.back() == L'\\') t.pop_back();
        return PathFileName(t);
    }
    return PathFileName(dir);
}

std::wstring MakeUniqueName(const std::wstring& dir, const std::wstring& name) {
    if (!PathExists(PathCombine(dir, name))) return name;
    std::wstring stem = name, ext;
    size_t pos = name.find_last_of(L'.');
    if (pos != std::wstring::npos && pos > 0) {
        stem = name.substr(0, pos);
        ext = name.substr(pos);
    }
    for (int i = 2; i < 100000; ++i) {
        std::wstring cand = stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (!PathExists(PathCombine(dir, cand))) return cand;
    }
    return name;
}

std::wstring GetExeDir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    return PathParent(buf);
}

std::wstring GetKnownFolder(const GUID& id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring GetTempDir() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    std::wstring r(buf, n);
    while (r.size() > 3 && r.back() == L'\\') r.pop_back();
    return r;
}

// ===================== Formatierung =====================

std::wstring FormatSize(unsigned long long bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    wchar_t num[64];
    swprintf(num, 64, v < 10 ? L"%.2f" : (v < 100 ? L"%.1f" : L"%.0f"), v);
    // Dezimaltrennzeichen des Gebietsschemas
    wchar_t dec[8] = L",";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, 8);
    std::wstring s = num;
    size_t p = s.find(L'.');
    if (p != std::wstring::npos) s.replace(p, 1, dec);
    return s + L" " + units[u];
}

std::wstring FormatSizeBytes(unsigned long long bytes) { return IntToStrGrouped(bytes); }

std::wstring FormatFileTime(const FILETIME& ft, bool withSeconds) {
    if (ft.dwHighDateTime == 0 && ft.dwLowDateTime == 0) return L"";
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t date[64], time[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, withSeconds ? 0 : TIME_NOSECONDS, &st, nullptr, time, 64);
    return std::wstring(date) + L" " + time;
}

std::wstring FormatAttributes(DWORD a) {
    std::wstring r;
    r += (a & FILE_ATTRIBUTE_READONLY) ? L'R' : L'-';
    r += (a & FILE_ATTRIBUTE_HIDDEN) ? L'H' : L'-';
    r += (a & FILE_ATTRIBUTE_SYSTEM) ? L'S' : L'-';
    r += (a & FILE_ATTRIBUTE_ARCHIVE) ? L'A' : L'-';
    if (a & FILE_ATTRIBUTE_COMPRESSED) r += L'C';
    if (a & FILE_ATTRIBUTE_ENCRYPTED) r += L'E';
    return r;
}

std::wstring LastErrorMessage(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring r = buf ? Trim(buf) : (L"Fehler " + std::to_wstring(err));
    if (buf) LocalFree(buf);
    return r;
}

// ===================== Dateien =====================

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, uint64_t maxBytes) {
    out.clear();
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    uint64_t toRead = std::min<uint64_t>((uint64_t)size.QuadPart, maxBytes);
    if (toRead > (uint64_t)SIZE_MAX / 2) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)toRead);
    size_t done = 0;
    while (done < out.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(out.size() - done, 1 << 24);
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    out.resize(done);
    CloseHandle(h);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // Schreibgeschützte/versteckte Dateien: Attribute erhalten und erneut versuchen
        DWORD a = GetFileAttributesW(LongPath(path).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
            h = CreateFileW(LongPath(path).c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, 0, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) return false;
    }
    const uint8_t* p = (const uint8_t*)data;
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        DWORD chunk = (DWORD)std::min<size_t>(size - done, 1 << 24);
        DWORD written = 0;
        if (!WriteFile(h, p + done, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        done += written;
    }
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok) SetLastError(err);
    return ok;
}

uint64_t GetFileSize64(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &d)) return UINT64_MAX;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

bool ListDirectory(const std::wstring& dir, std::vector<DirEntry>& out, DWORD* errorOut) {
    out.clear();
    std::wstring pattern = LongPath(PathCombine(dir, L"*"));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (errorOut) *errorOut = e;
        return e == ERROR_FILE_NOT_FOUND; // leeres Laufwerk
    }
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        DirEntry e;
        e.name = fd.cFileName;
        e.attributes = fd.dwFileAttributes;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.created = fd.ftCreationTime;
        e.modified = fd.ftLastWriteTime;
        e.accessed = fd.ftLastAccessTime;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (errorOut) *errorOut = 0;
    return true;
}

// ===================== UI-Helfer =====================

static bool g_suppressMessages = false;
void SetMessageBoxesSuppressed(bool on) { g_suppressMessages = on; }
static void LogSuppressed(const wchar_t* kind, const std::wstring& text) {
    std::string u = WideToUtf8(std::wstring(kind) + L": " + text + L"\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), u.data(), (DWORD)u.size(), &written, nullptr);
}
void MsgError(HWND owner, const std::wstring& text) {
    if (g_suppressMessages) return LogSuppressed(L"FEHLER", text);
    MessageBoxW(owner, text.c_str(), L"QFiles", MB_OK | MB_ICONERROR);
}
void MsgInfo(HWND owner, const std::wstring& text) {
    if (g_suppressMessages) return LogSuppressed(L"INFO", text);
    MessageBoxW(owner, text.c_str(), L"QFiles", MB_OK | MB_ICONINFORMATION);
}
bool MsgConfirm(HWND owner, const std::wstring& text) {
    if (g_suppressMessages) return true;
    return MessageBoxW(owner, text.c_str(), L"QFiles", MB_YESNO | MB_ICONQUESTION) == IDYES;
}
int MsgYesNoCancel(HWND owner, const std::wstring& text) {
    if (g_suppressMessages) return IDYES;
    return MessageBoxW(owner, text.c_str(), L"QFiles", MB_YESNOCANCEL | MB_ICONQUESTION);
}

std::wstring GetWindowTextStr(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    n = GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}

UINT GetWindowDpi(HWND h) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && h) return fn(h);
    HDC dc = GetDC(nullptr);
    UINT dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return dpi;
}

int DpiScale(HWND h, int value) { return MulDiv(value, (int)GetWindowDpi(h), 96); }

bool ClipboardSetText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        memcpy(GlobalLock(g), text.c_str(), bytes);
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
    return g != nullptr;
}

std::wstring ClipboardGetText(HWND owner) {
    std::wstring r;
    if (!OpenClipboard(owner)) return r;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) r = p;
        GlobalUnlock(h);
    }
    CloseClipboard();
    return r;
}

static std::vector<COMDLG_FILTERSPEC> ParseFilter(const std::wstring& spec, std::vector<std::wstring>& storage) {
    storage = Split(spec, L'|', false);
    std::vector<COMDLG_FILTERSPEC> r;
    for (size_t i = 0; i + 1 < storage.size(); i += 2) r.push_back({storage[i].c_str(), storage[i + 1].c_str()});
    return r;
}

static void SetDialogFolder(IFileDialog* dlg, const std::wstring& dir) {
    if (dir.empty()) return;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dlg->SetFolder(item);
        item->Release();
    }
}

static std::wstring DialogResult(IFileDialog* dlg) {
    std::wstring r;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            r = p;
            CoTaskMemFree(p);
        }
        item->Release();
    }
    return r;
}

std::wstring BrowseForFolder(HWND owner, const std::wstring& title, const std::wstring& initialDir) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return L"";
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title.c_str());
    SetDialogFolder(dlg, initialDir);
    std::wstring r;
    if (SUCCEEDED(dlg->Show(owner))) r = DialogResult(dlg);
    dlg->Release();
    return r;
}

std::wstring OpenFileDialog(HWND owner, const std::wstring& title, const std::wstring& initialDir,
                            const std::wstring& filterSpec) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return L"";
    std::vector<std::wstring> storage;
    auto specs = ParseFilter(filterSpec, storage);
    if (!specs.empty()) dlg->SetFileTypes((UINT)specs.size(), specs.data());
    dlg->SetTitle(title.c_str());
    SetDialogFolder(dlg, initialDir);
    std::wstring r;
    if (SUCCEEDED(dlg->Show(owner))) r = DialogResult(dlg);
    dlg->Release();
    return r;
}

std::wstring SaveFileDialog(HWND owner, const std::wstring& title, const std::wstring& initialPath,
                            const std::wstring& filterSpec) {
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return L"";
    std::vector<std::wstring> storage;
    auto specs = ParseFilter(filterSpec, storage);
    if (!specs.empty()) dlg->SetFileTypes((UINT)specs.size(), specs.data());
    dlg->SetTitle(title.c_str());
    if (!initialPath.empty()) {
        if (DirExists(initialPath)) {
            SetDialogFolder(dlg, initialPath);
        } else {
            SetDialogFolder(dlg, PathParent(initialPath));
            dlg->SetFileName(PathFileName(initialPath).c_str());
        }
    }
    std::wstring r;
    if (SUCCEEDED(dlg->Show(owner))) r = DialogResult(dlg);
    dlg->Release();
    return r;
}

namespace {
class InputDlg : public DialogBase {
public:
    std::wstring prompt, value, hint;
    int selLength = -1;
    BOOL OnInit() override {
        SetText(101, prompt);
        SetText(102, value);
        SetText(103, hint);
        SendMessageW(Item(102), EM_SETSEL, 0, selLength >= 0 ? selLength : -1);
        SetFocus(Item(102));
        return FALSE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == IDOK) value = GetText(102);
        return DialogBase::OnCommand(id, code, ctl);
    }
};
} // namespace

int FileStemLength(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || name.find_first_of(L"\\/", dot) != std::wstring::npos) return (int)name.size();
    return (int)dot;
}

bool InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value,
              const std::wstring& hint, int selLength) {
    DialogTemplate t(title, 260, hint.empty() ? 70 : 90);
    t.Label(101, L"", 7, 7, 246, 10);
    t.Edit(102, 7, 20, 246, 14, ES_AUTOHSCROLL);
    int y = 40;
    if (!hint.empty()) {
        t.Label(103, L"", 7, 38, 246, 18);
        y = 60;
    } else {
        t.Label(103, L"", 0, 0, 0, 0);
    }
    t.DefButton(IDOK, L"OK", 146, y + 5, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 203, y + 5, 50, 14);
    InputDlg dlg;
    dlg.prompt = prompt;
    dlg.value = value;
    dlg.hint = hint;
    dlg.selLength = selLength;
    if (dlg.DoModal(owner, t) != IDOK) return false;
    value = dlg.value;
    return true;
}

bool ShellOpen(HWND owner, const std::wstring& file, const std::wstring& params, const std::wstring& dir,
               const wchar_t* verb) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = verb;
    sei.lpFile = file.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;
    DWORD err = GetLastError();
    if (err == ERROR_NO_ASSOCIATION && !verb) {
        // "Öffnen mit"-Dialog anbieten
        OPENASINFO oi{};
        oi.pcszFile = file.c_str();
        oi.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
        return SUCCEEDED(SHOpenWithDialog(owner, &oi));
    }
    if (err != ERROR_CANCELLED) MsgError(owner, L"„" + file + L"“ kann nicht geöffnet werden:\n" + LastErrorMessage(err));
    return false;
}

bool RunProcess(const std::wstring& commandLine, const std::wstring& dir, bool wait, DWORD* exitCode, bool hidden) {
    STARTUPINFOW si{sizeof(si)};
    if (hidden) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | (hidden ? CREATE_NO_WINDOW : 0), nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi))
        return false;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool RunAndCapture(const std::wstring& commandLine, const std::wstring& dir, std::wstring& output, DWORD* exitCode) {
    output.clear();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    std::string bytes;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) bytes.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // UTF-8 bevorzugen, sonst OEM-Codepage (Konsolenausgabe)
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    UINT cp = n > 0 || bytes.empty() ? CP_UTF8 : GetOEMCP();
    n = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    output.resize(n);
    MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), output.data(), n);
    return true;
}

} // namespace qf
