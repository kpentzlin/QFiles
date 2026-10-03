// Modul E – Archive.
//
// ShowArchive:
//   - ZIP: Zentralverzeichnis selbst lesen (inkl. ZIP64-Grundunterstützung, UTF-8-Namen bei Bit 11, sonst
//     Codepage 437, Info-ZIP-Unicode-Pfad 0x7075, Zeitstempel aus NTFS-/UT-Extrafeld). Entpacken über die
//     Windows-Shell (ZIP = Shell-Ordner „CompressedFolder“) mit IFileOperation (Fortschritt/Konflikte der Shell).
//   - Andere Formate (7z, rar, tar, tgz, gz, bz2, xz, cab, iso …): Windows-eigenes %SystemRoot%\System32\tar.exe
//     (bsdtar/libarchive, ab Windows 10 1803). Liste mit „tar -tvf“, Entpacken mit „tar -xf … -C …“.
// CreateZipArchive:
//   - leere ZIP-Datei (End-of-Central-Directory) schreiben, dann IFileOperation::CopyItems in das ZIP-Shell-Item.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Util.h"
#include "ToolsCommon.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace qf {

namespace {

// BHID_EnumItems (lokal definiert, damit keine Abhängigkeit von der uuid-Bibliothek der jeweiligen Toolchain besteht)
const GUID kBhidEnumItems = {0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ===================================================================================
// Archiveinträge
// ===================================================================================

struct ArcEntry {
    std::wstring name;        // Anzeige (mit '\')
    std::wstring rawName;     // wie im Archiv bzw. in der tar-Ausgabe (mit '/')
    bool isDir = false;
    bool encrypted = false;
    unsigned long long size = 0, packed = 0;
    bool hasPacked = false;
    bool hasDate = false;
    SYSTEMTIME date{};        // lokale Zeit
    std::wstring method;
    // vorbereitete Anzeigetexte
    std::wstring sizeText, packedText, dateText;
};

void PrepareTexts(ArcEntry& e) {
    e.sizeText = e.isDir ? L"" : FormatSizeBytes(e.size);
    e.packedText = (e.isDir || !e.hasPacked) ? L"" : FormatSizeBytes(e.packed);
    e.dateText = e.hasDate ? toolsdetail::FormatLocalSystemTime(e.date, false) : L"";
}

// ---------- ZIP ----------

uint16_t Le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t Le64(const uint8_t* p) { return (uint64_t)Le32(p) | ((uint64_t)Le32(p + 4) << 32); }

uint32_t Crc32(const uint8_t* data, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

std::wstring DecodeCp(const uint8_t* p, size_t n, UINT cp) {
    if (n == 0) return L"";
    int len = MultiByteToWideChar(cp, 0, (const char*)p, (int)n, nullptr, 0);
    std::wstring r(len > 0 ? len : 0, L'\0');
    if (len > 0) MultiByteToWideChar(cp, 0, (const char*)p, (int)n, r.data(), len);
    return r;
}

const wchar_t* ZipMethodName(uint16_t m) {
    switch (m) {
    case 0: return L"Gespeichert";
    case 1: return L"Shrunk";
    case 6: return L"Imploded";
    case 8: return L"Deflate";
    case 9: return L"Deflate64";
    case 12: return L"BZip2";
    case 14: return L"LZMA";
    case 93: return L"Zstandard";
    case 95: return L"XZ";
    case 96: return L"JPEG";
    case 97: return L"WavPack";
    case 98: return L"PPMd";
    case 99: return L"AES";
    default: return nullptr;
    }
}

class FileReader {
public:
    explicit FileReader(const std::wstring& path) {
        h_ = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li{};
            GetFileSizeEx(h_, &li);
            size_ = (uint64_t)li.QuadPart;
        }
    }
    ~FileReader() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    bool Ok() const { return h_ != INVALID_HANDLE_VALUE; }
    uint64_t Size() const { return size_; }
    bool ReadAt(uint64_t off, void* buf, size_t n) {
        if (off + n > size_) return false;
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return false;
        uint8_t* p = (uint8_t*)buf;
        while (n > 0) {
            DWORD chunk = (DWORD)std::min<size_t>(n, 1u << 26), got = 0;
            if (!ReadFile(h_, p, chunk, &got, nullptr) || got == 0) return false;
            p += got;
            n -= got;
        }
        return true;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

// Liest das Zentralverzeichnis. Rückgabe false, wenn keine (lesbare) ZIP-Datei.
bool ReadZipDirectory(const std::wstring& path, std::vector<ArcEntry>& out, std::wstring& error) {
    out.clear();
    FileReader f(path);
    if (!f.Ok()) {
        error = LastErrorMessage();
        return false;
    }
    const uint64_t fsize = f.Size();
    if (fsize < 22) {
        error = L"Datei zu klein für ein ZIP-Archiv.";
        return false;
    }
    size_t tailLen = (size_t)std::min<uint64_t>(fsize, 65535 + 22 + 20);
    std::vector<uint8_t> tail(tailLen);
    uint64_t tailOff = fsize - tailLen;
    if (!f.ReadAt(tailOff, tail.data(), tailLen)) {
        error = LastErrorMessage();
        return false;
    }
    // End of Central Directory von hinten suchen
    size_t eocd = SIZE_MAX;
    for (size_t i = tailLen - 22 + 1; i-- > 0;) {
        if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) {
            uint16_t commentLen = Le16(&tail[i + 20]);
            if (i + 22 + commentLen <= tailLen) {
                eocd = i;
                break;
            }
        }
    }
    if (eocd == SIZE_MAX) {
        error = L"Kein ZIP-Zentralverzeichnis gefunden.";
        return false;
    }
    uint64_t entries = Le16(&tail[eocd + 10]);
    uint64_t cdSize = Le32(&tail[eocd + 12]);
    uint64_t cdOffset = Le32(&tail[eocd + 16]);
    uint64_t eocdAbs = tailOff + eocd;
    uint64_t cdEnd = eocdAbs;   // Zentralverzeichnis endet vor EOCD bzw. vor ZIP64-Datensatz

    // ZIP64: Locator 20 Byte vor EOCD
    if (eocd >= 20 && tail[eocd - 20] == 'P' && tail[eocd - 19] == 'K' && tail[eocd - 18] == 6 && tail[eocd - 17] == 7) {
        uint64_t z64Off = Le64(&tail[eocd - 20 + 8]);
        uint8_t rec[56];
        if (f.ReadAt(z64Off, rec, sizeof(rec)) && rec[0] == 'P' && rec[1] == 'K' && rec[2] == 6 && rec[3] == 6) {
            entries = Le64(&rec[32]);
            cdSize = Le64(&rec[40]);
            cdOffset = Le64(&rec[48]);
            cdEnd = z64Off;
        } else {
            // ZIP64-Datensatz nicht an angegebener Stelle (z. B. vorangestellter SFX-Teil): direkt davor suchen
            uint64_t guess = eocdAbs - 20 - 56;
            if (eocdAbs >= 76 && f.ReadAt(guess, rec, sizeof(rec)) && rec[0] == 'P' && rec[1] == 'K' && rec[2] == 6 &&
                rec[3] == 6) {
                entries = Le64(&rec[32]);
                cdSize = Le64(&rec[40]);
                cdOffset = Le64(&rec[48]);
                cdEnd = guess;
            }
        }
    }
    if (cdSize > 512ull * 1024 * 1024 || cdSize > fsize) {
        error = L"Das Zentralverzeichnis ist ungültig.";
        return false;
    }
    // Versatz bei vorangestellten Daten (selbstentpackende Archive): CD liegt direkt vor cdEnd
    uint8_t sig[4];
    if (!f.ReadAt(cdOffset, sig, 4) || Le32(sig) != 0x02014b50u) {
        if (cdEnd >= cdSize && f.ReadAt(cdEnd - cdSize, sig, 4) && Le32(sig) == 0x02014b50u)
            cdOffset = cdEnd - cdSize;
        else if (entries != 0) {
            error = L"Das Zentralverzeichnis ist ungültig.";
            return false;
        }
    }
    std::vector<uint8_t> cd((size_t)cdSize);
    if (cdSize && !f.ReadAt(cdOffset, cd.data(), (size_t)cdSize)) {
        error = L"Das Zentralverzeichnis kann nicht gelesen werden.";
        return false;
    }

    size_t pos = 0;
    while (pos + 46 <= cd.size()) {
        const uint8_t* h = &cd[pos];
        if (Le32(h) != 0x02014b50u) break;
        uint16_t madeBy = Le16(h + 4);
        uint16_t flags = Le16(h + 8);
        uint16_t method = Le16(h + 10);
        uint16_t dosTime = Le16(h + 12);
        uint16_t dosDate = Le16(h + 14);
        uint64_t csize = Le32(h + 20);
        uint64_t usize = Le32(h + 24);
        uint16_t nlen = Le16(h + 28), xlen = Le16(h + 30), clen = Le16(h + 32);
        uint32_t extAttr = Le32(h + 38);
        if (pos + 46 + nlen + xlen + clen > cd.size()) break;
        const uint8_t* namePtr = h + 46;
        const uint8_t* extra = namePtr + nlen;

        ArcEntry e;
        e.rawName = (flags & 0x0800) ? Utf8ToWide(std::string_view((const char*)namePtr, nlen)) : DecodeCp(namePtr, nlen, 437);
        e.encrypted = (flags & 1) != 0;

        bool haveFt = false;
        FILETIME ft{};
        // Extrafelder
        for (size_t x = 0; x + 4 <= xlen;) {
            uint16_t id = Le16(extra + x), sz = Le16(extra + x + 2);
            const uint8_t* d = extra + x + 4;
            if (x + 4 + sz > xlen) break;
            if (id == 0x0001) {
                // ZIP64: nur Felder, die im Kopf 0xFFFFFFFF sind, in fester Reihenfolge
                size_t o = 0;
                if (usize == 0xFFFFFFFFu && o + 8 <= sz) {
                    usize = Le64(d + o);
                    o += 8;
                }
                if (csize == 0xFFFFFFFFu && o + 8 <= sz) {
                    csize = Le64(d + o);
                    o += 8;
                }
            } else if (id == 0x7075 && sz >= 5 && d[0] == 1) {
                // Info-ZIP Unicode-Pfad (gültig, wenn CRC des Kopfnamens passt)
                if (Le32(d + 1) == Crc32(namePtr, nlen))
                    e.rawName = Utf8ToWide(std::string_view((const char*)d + 5, sz - 5));
            } else if (id == 0x000A && sz >= 32) {
                // NTFS: reserviert(4), Tag 1, Größe 24, mtime, atime, ctime
                if (Le16(d + 4) == 1 && Le16(d + 6) >= 24) {
                    uint64_t mt = Le64(d + 8);
                    ft.dwLowDateTime = (DWORD)mt;
                    ft.dwHighDateTime = (DWORD)(mt >> 32);
                    haveFt = mt != 0;
                }
            } else if (id == 0x5455 && sz >= 5 && (d[0] & 1) && !haveFt) {
                // Extended Timestamp: Unix-Zeit (UTC)
                uint64_t unixT = Le32(d + 1);
                uint64_t t = unixT * 10000000ull + 116444736000000000ull;
                ft.dwLowDateTime = (DWORD)t;
                ft.dwHighDateTime = (DWORD)(t >> 32);
                haveFt = true;
            }
            x += 4 + sz;
        }

        e.isDir = (!e.rawName.empty() && (e.rawName.back() == L'/' || e.rawName.back() == L'\\')) ||
                  (((madeBy >> 8) == 0 || (madeBy >> 8) == 11 || (madeBy >> 8) == 14) && (extAttr & 0x10));
        e.size = usize;
        e.packed = csize;
        e.hasPacked = true;
        if (haveFt && toolsdetail::FileTimeToLocal(ft, e.date)) {
            e.hasDate = true;
        } else if (dosDate) {
            e.date = SYSTEMTIME{};
            e.date.wYear = (WORD)(1980 + (dosDate >> 9));
            e.date.wMonth = (WORD)std::clamp((dosDate >> 5) & 15, 1, 12);
            e.date.wDay = (WORD)std::clamp(dosDate & 31, 1, 31);
            e.date.wHour = (WORD)std::min(dosTime >> 11, 23);
            e.date.wMinute = (WORD)std::min((dosTime >> 5) & 63, 59);
            e.date.wSecond = (WORD)std::min((dosTime & 31) * 2, 59);
            e.hasDate = true;
        }
        const wchar_t* mn = ZipMethodName(method);
        e.method = mn ? mn : Format(L"Methode %d", (int)method);
        if (e.isDir) e.method.clear();
        if (e.encrypted) e.method += L" (verschlüsselt)";

        e.name = e.rawName;
        std::replace(e.name.begin(), e.name.end(), L'/', L'\\');
        while (!e.name.empty() && e.name.back() == L'\\') e.name.pop_back();
        if (!e.name.empty()) {
            PrepareTexts(e);
            out.push_back(std::move(e));
        }
        pos += 46 + nlen + xlen + clen;
    }
    return true;
}

bool LooksLikeZip(const std::wstring& path) {
    uint8_t b[4] = {};
    FileReader f(path);
    if (!f.Ok() || !f.ReadAt(0, b, 4)) return false;
    if (b[0] == 'P' && b[1] == 'K' && ((b[2] == 3 && b[3] == 4) || (b[2] == 5 && b[3] == 6) || (b[2] == 7 && b[3] == 8)))
        return true;
    // Selbstentpackende ZIPs (EXE mit angehängtem ZIP) über die Erweiterung zulassen
    std::wstring ext = ToLower(PathExtension(path));
    return ext == L".zip" || ext == L".jar" || ext == L".docx" || ext == L".xlsx" || ext == L".pptx" ||
           ext == L".odt" || ext == L".ods" || ext == L".epub" || ext == L".apk" || ext == L".nupkg" ||
           ext == L".vsix" || ext == L".exe";
}

// ---------- tar.exe ----------

std::wstring TarExePath() {
    wchar_t sys[MAX_PATH] = L"";
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring p = PathCombine(sys, L"tar.exe");
    return FileExists(p) ? p : L"";
}

std::wstring Quote(const std::wstring& s) {
    // Für die C-Laufzeit-Argumentzerlegung: abschließende Backslashes vor dem Anführungszeichen verdoppeln
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++bs;
        } else if (c == L'"') {
            r.append(bs + 1, L'\\');   // vorangehende Backslashes verdoppeln + Escape
            bs = 0;
        } else {
            bs = 0;
        }
        if (c == L'"')
            r += L'"';
        else
            r += c;
    }
    r.append(bs, L'\\');
    r += L'"';
    return r;
}

// Führt einen Befehl aus, sammelt stdout+stderr und kann über sink abgebrochen werden (Prozess wird beendet).
bool CaptureProcess(const std::wstring& commandLine, const std::wstring& dir, std::wstring& output, DWORD& exitCode,
                    toolsdetail::ProgressSink* sink) {
    output.clear();
    exitCode = 1;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    DWORD createErr = ok ? 0 : GetLastError();
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        SetLastError(createErr);
        return false;
    }
    std::string bytes;
    char buf[65536];
    bool cancelled = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, (DWORD)std::min<size_t>(sizeof(buf), avail), &got, nullptr) || got == 0) break;
            bytes.append(buf, got);
            continue;
        }
        if (sink && sink->Cancelled()) {
            TerminateProcess(pi.hProcess, 1);
            cancelled = true;
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
            // Rest lesen
            DWORD got = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                   ReadFile(rd, buf, (DWORD)std::min<size_t>(sizeof(buf), avail), &got, nullptr) && got > 0)
                bytes.append(buf, got);
            break;
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 5000);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // UTF-8 bevorzugen, sonst OEM-Codepage (Konsolenausgabe)
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    UINT cp = (n > 0 || bytes.empty()) ? CP_UTF8 : GetOEMCP();
    output = DecodeCp((const uint8_t*)bytes.data(), bytes.size(), cp);
    return !cancelled;
}

int MonthFromAbbr(const std::wstring& m) {
    static const wchar_t* names[] = {L"jan", L"feb", L"mar", L"apr", L"may", L"jun",
                                     L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};
    std::wstring l = ToLower(m);
    for (int i = 0; i < 12; ++i)
        if (l == names[i]) return i + 1;
    return 0;
}

// Zerlegt die Ausgabe von „tar -tvf“ (ls-ähnlich):
// "-rw-r--r--  0 user group   1234 Jan  1  2020 pfad/name"  bzw. "... Jan  1 12:34 pfad/name"
void ParseTarListing(const std::wstring& output, std::vector<ArcEntry>& out, std::wstring& errors) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    for (const auto& rawLine : Split(output, L'\n')) {
        std::wstring line = rawLine;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        // 8 Felder, dann der Name
        std::vector<std::wstring> tok;
        size_t i = 0, nameStart = std::wstring::npos;
        while (i < line.size() && tok.size() < 8) {
            while (i < line.size() && line[i] == L' ') ++i;
            size_t s = i;
            while (i < line.size() && line[i] != L' ') ++i;
            if (i > s) tok.push_back(line.substr(s, i - s));
        }
        if (tok.size() == 8 && i < line.size()) nameStart = i + 1;
        bool valid = tok.size() == 8 && nameStart != std::wstring::npos && tok[0].size() >= 10 &&
                     std::wstring(L"-dlhcbps").find(tok[0][0]) != std::wstring::npos;
        int month = valid ? MonthFromAbbr(tok[5]) : 0;
        if (!valid || month == 0) {
            if (!errors.empty()) errors += L"\n";
            errors += line;
            continue;
        }
        ArcEntry e;
        std::wstring name = line.substr(nameStart);
        if (tok[0][0] == L'l') {
            size_t arrow = name.find(L" -> ");
            if (arrow != std::wstring::npos) name.resize(arrow);
        } else if (tok[0][0] == L'h') {
            size_t link = name.find(L" link to ");
            if (link != std::wstring::npos) name.resize(link);
        }
        e.rawName = name;
        e.isDir = tok[0][0] == L'd' || (!name.empty() && name.back() == L'/');
        e.size = (unsigned long long)StrToInt(tok[4], 0);
        e.date = SYSTEMTIME{};
        e.date.wMonth = (WORD)month;
        e.date.wDay = (WORD)std::clamp((int)StrToInt(tok[6], 1), 1, 31);
        if (tok[7].find(L':') != std::wstring::npos) {
            // Uhrzeit statt Jahr: innerhalb der letzten 6 Monate
            e.date.wHour = (WORD)std::clamp((int)StrToInt(tok[7].substr(0, tok[7].find(L':')), 0), 0, 23);
            e.date.wMinute = (WORD)std::clamp((int)StrToInt(tok[7].substr(tok[7].find(L':') + 1), 0), 0, 59);
            e.date.wYear = now.wYear;
            if (e.date.wMonth > now.wMonth || (e.date.wMonth == now.wMonth && e.date.wDay > now.wDay)) e.date.wYear--;
        } else {
            e.date.wYear = (WORD)std::clamp((int)StrToInt(tok[7], 1980), 1601, 30827);
        }
        e.hasDate = true;
        e.method = tok[0];   // Rechte
        e.name = name;
        std::replace(e.name.begin(), e.name.end(), L'/', L'\\');
        while (!e.name.empty() && e.name.back() == L'\\') e.name.pop_back();
        if (e.name.empty()) continue;
        PrepareTexts(e);
        out.push_back(std::move(e));
    }
}

// ---------- Hilfen für Dateisystem und Shell ----------

bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty() || DirExists(dir)) return true;
    std::wstring parent = PathParent(dir);
    if (!parent.empty() && parent != dir && !EnsureDirectory(parent)) return false;
    return CreateDirectoryW(LongPath(dir).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

IShellItem* ItemFromPath(const std::wstring& path) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return nullptr;
    return item;
}

IFileOperation* NewFileOperation(HWND owner, DWORD flags) {
    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return nullptr;
    op->SetOwnerWindow(owner);
    op->SetOperationFlags(flags);
    return op;
}

// Führt die Operation aus. Rückgabe: S_OK, E_ABORT (abgebrochen) oder Fehler.
HRESULT Perform(IFileOperation* op) {
    HRESULT hr = op->PerformOperations();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    if (SUCCEEDED(hr) && aborted) hr = E_ABORT;
    return hr;
}

std::wstring HrMessage(HRESULT hr) {
    if (hr == E_ABORT) return L"Der Vorgang wurde abgebrochen.";
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) return LastErrorMessage(HRESULT_CODE(hr));
    return LastErrorMessage((DWORD)hr);
}

// ===================================================================================
// Archiv-Dialog
// ===================================================================================

class ArchiveDlg : public DialogBase {
public:
    static constexpr int kList = 100;
    static constexpr int kStatus = 101;
    static constexpr int kDestLabel = 102;
    static constexpr int kDest = 103;
    static constexpr int kDestBrowse = 104;
    static constexpr int kSubDir = 105;
    static constexpr int kExtractAll = 110;
    static constexpr int kExtractSel = 111;
    static constexpr int kOpenItem = 112;

    std::wstring archive;
    std::wstring extractDir;
    bool isZip = false;
    std::wstring tarExe;
    std::vector<ArcEntry> entries;
    bool extracted = false;

protected:
    BOOL OnInit() override {
        HWND lv = Item(kList);
        LvAddColumn(kList, L"Name", 200);
        LvAddColumn(kList, L"Größe", 56, LVCFMT_RIGHT);
        LvAddColumn(kList, L"Gepackt", 56, LVCFMT_RIGHT);
        LvAddColumn(kList, L"Datum", 72);
        LvAddColumn(kList, isZip ? L"Methode" : L"Rechte", 60);
        SHFILEINFOW sfi{};
        HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"x", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                                                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        if (il) ListView_SetImageList(lv, il, LVSIL_SMALL);
        folderIcon_ = sfi.iIcon;
        ListView_SetItemCountEx(lv, (int)entries.size(), 0);

        // Statuszeile
        unsigned long long total = 0, packed = 0;
        int files = 0, dirs = 0;
        std::map<std::wstring, int> tops;
        for (const auto& e : entries) {
            if (e.isDir)
                ++dirs;
            else {
                ++files;
                total += e.size;
                packed += e.packed;
            }
            tops[ToLower(e.name.substr(0, e.name.find(L'\\')))]++;
        }
        std::wstring st = Format(L"%d Datei(en), %d Verzeichnis(se), ", files, dirs) + FormatSize(total);
        if (isZip && total > 0)
            st += Format(L", gepackt %d %%", (int)(packed * 100.0 / (double)total + 0.5));
        st += isZip ? L"  –  ZIP" : L"  –  über tar.exe";
        SetText(kStatus, st);

        SetText(kDest, extractDir);
        SetText(kSubDir, L"In &Unterverzeichnis „" + PathStem(archive) + L"“ entpacken");
        SetCheck(kSubDir, tops.size() > 1);

        SetAnchor(kList, AnchorAll);
        SetAnchor(kStatus, AnchorBottomLeftRight);
        SetAnchor(kDestLabel, AnchorBottomLeft);
        SetAnchor(kDest, AnchorBottomLeftRight);
        SetAnchor(kDestBrowse, AnchorBottomRight);
        SetAnchor(kSubDir, AnchorBottomLeft);
        SetAnchor(kExtractAll, AnchorBottomLeft);
        SetAnchor(kExtractSel, AnchorBottomLeft);
        SetAnchor(kOpenItem, AnchorBottomLeft);
        SetAnchor(IDCANCEL, AnchorBottomRight);
        EnableResizing();
        UpdateButtons();
        SetFocus(lv);
        return FALSE;
    }

    BOOL OnCommand(int id, int code, HWND ctl) override {
        switch (id) {
        case kDestBrowse: {
            std::wstring d = BrowseForFolder(hwnd_, L"Zielverzeichnis auswählen", GetText(kDest));
            if (!d.empty()) SetText(kDest, d);
            return TRUE;
        }
        case kExtractAll:
            if (Extract(true)) End(IDOK);
            return TRUE;
        case kExtractSel:
            if (Extract(false)) End(IDOK);
            return TRUE;
        case kOpenItem:
        case IDOK:
            OpenFocused();
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }

    INT_PTR OnNotify(NMHDR* nm) override {
        if (nm->idFrom != kList) return 0;
        switch (nm->code) {
        case LVN_GETDISPINFOW: {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
            int i = di->item.iItem;
            if (i < 0 || i >= (int)entries.size()) return 0;
            const ArcEntry& e = entries[i];
            if (di->item.mask & LVIF_TEXT) {
                const std::wstring* s = nullptr;
                switch (di->item.iSubItem) {
                case 0: s = &e.name; break;
                case 1: s = &e.sizeText; break;
                case 2: s = &e.packedText; break;
                case 3: s = &e.dateText; break;
                case 4: s = &e.method; break;
                }
                if (s && di->item.pszText && di->item.cchTextMax > 0)
                    lstrcpynW(di->item.pszText, s->c_str(), di->item.cchTextMax);
            }
            if (di->item.mask & LVIF_IMAGE) di->item.iImage = IconFor(e);
            return 0;
        }
        case LVN_COLUMNCLICK: {
            auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
            Sort(lv->iSubItem);
            return 0;
        }
        case NM_DBLCLK:
            OpenFocused();
            return 0;
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED:
            UpdateButtons();
            return 0;
        }
        return 0;
    }

private:
    int IconFor(const ArcEntry& e) {
        if (e.isDir) return folderIcon_;
        std::wstring ext = ToLower(PathExtension(e.name));
        auto it = iconCache_.find(ext);
        if (it != iconCache_.end()) return it->second;
        SHFILEINFOW sfi{};
        std::wstring probe = L"datei" + ext;
        SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        iconCache_[ext] = sfi.iIcon;
        return sfi.iIcon;
    }

    void Sort(int col) {
        if (col == sortCol_)
            sortAsc_ = !sortAsc_;
        else {
            sortCol_ = col;
            sortAsc_ = true;
        }
        auto key = [&](const ArcEntry& a, const ArcEntry& b) -> int {
            switch (sortCol_) {
            case 1: return a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
            case 2: return a.packed < b.packed ? -1 : (a.packed > b.packed ? 1 : 0);
            case 3: {
                FILETIME fa{}, fb{};
                SystemTimeToFileTime(&a.date, &fa);
                SystemTimeToFileTime(&b.date, &fb);
                return CompareFileTime(&fa, &fb);
            }
            case 4: return CompareI(a.method, b.method);
            default: return CompareNatural(a.name, b.name);
            }
        };
        std::stable_sort(entries.begin(), entries.end(), [&](const ArcEntry& a, const ArcEntry& b) {
            int c = key(a, b);
            if (c == 0) c = CompareNatural(a.name, b.name);
            return sortAsc_ ? c < 0 : c > 0;
        });
        HWND lv = Item(kList);
        ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        InvalidateRect(lv, nullptr, FALSE);
        // Sortierpfeil im Spaltenkopf
        HWND hdr = ListView_GetHeader(lv);
        for (int i = 0; i < Header_GetItemCount(hdr); ++i) {
            HDITEMW hi{};
            hi.mask = HDI_FORMAT;
            Header_GetItem(hdr, i, &hi);
            hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (i == sortCol_) hi.fmt |= sortAsc_ ? HDF_SORTUP : HDF_SORTDOWN;
            Header_SetItem(hdr, i, &hi);
        }
    }

    void UpdateButtons() {
        HWND lv = Item(kList);
        bool anySel = ListView_GetSelectedCount(lv) > 0;
        Enable(kExtractSel, anySel);
        Enable(kOpenItem, anySel);
        Enable(kExtractAll, !entries.empty());
    }

    std::vector<int> SelectedIndices() {
        std::vector<int> r;
        HWND lv = Item(kList);
        for (int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(lv, i, LVNI_SELECTED))
            if (i < (int)entries.size()) r.push_back(i);
        return r;
    }

    std::wstring DestinationDir() {
        std::wstring dest = Trim(GetText(kDest));
        if (dest.empty()) return L"";
        dest = NormalizeDir(dest);
        if (IsChecked(kSubDir)) dest = PathCombine(dest, PathStem(archive));
        return dest;
    }

    // Entpacken (alles oder Auswahl). Rückgabe true bei Erfolg.
    bool Extract(bool all) {
        std::wstring dest = DestinationDir();
        if (dest.empty()) {
            MsgError(hwnd_, L"Bitte ein Zielverzeichnis angeben.");
            return false;
        }
        if (!EnsureDirectory(dest)) {
            MsgError(hwnd_, L"Das Zielverzeichnis kann nicht angelegt werden:\n" + dest + L"\n" + LastErrorMessage());
            return false;
        }
        std::vector<int> sel;
        if (!all) {
            sel = SelectedIndices();
            if (sel.empty()) return false;
            // Elemente entfernen, deren übergeordnetes Verzeichnis ebenfalls gewählt ist
            std::vector<std::wstring> dirs;
            for (int i : sel)
                if (entries[i].isDir) dirs.push_back(ToLower(entries[i].name) + L"\\");
            std::vector<int> filtered;
            for (int i : sel) {
                std::wstring n = ToLower(entries[i].name);
                bool covered = false;
                for (const auto& d : dirs)
                    if (n.size() > d.size() && n.compare(0, d.size(), d) == 0) covered = true;
                if (!covered) filtered.push_back(i);
            }
            sel.swap(filtered);
        }
        bool ok = isZip ? ExtractZip(all, sel, dest, false) : ExtractTar(all, sel, dest);
        if (ok) {
            extracted = true;
            LogOperation(L"Archiv entpackt: " + archive + L" → " + dest +
                         (all ? L"" : Format(L" (%d Element(e))", (int)sel.size())));
            App::RefreshPanes();
        }
        return ok;
    }

    // ZIP über die Shell. preservePaths: Unterverzeichnisstruktur der Auswahl beibehalten.
    bool ExtractZip(bool all, const std::vector<int>& sel, const std::wstring& dest, bool silent) {
        DWORD flags = FOF_NOCONFIRMMKDIR;
        if (silent) flags |= FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
        IFileOperation* op = NewFileOperation(hwnd_, flags);
        if (!op) {
            MsgError(hwnd_, L"Die Shell-Dateioperation ist nicht verfügbar.");
            return false;
        }
        HRESULT hr = S_OK;
        std::vector<std::wstring> missing;
        if (all) {
            IShellItem* zip = ItemFromPath(archive);
            IShellItem* to = ItemFromPath(dest);
            IEnumShellItems* en = nullptr;
            if (zip && to) hr = zip->BindToHandler(nullptr, kBhidEnumItems, IID_PPV_ARGS(&en));
            else hr = E_FAIL;
            if (SUCCEEDED(hr) && en) hr = op->CopyItems(en, to);
            SafeRelease(en);
            SafeRelease(zip);
            SafeRelease(to);
        } else {
            for (int i : sel) {
                const ArcEntry& e = entries[i];
                std::wstring parentRel = PathParent(e.name);
                std::wstring target = parentRel.empty() || silent ? dest : PathCombine(dest, parentRel);
                if (!EnsureDirectory(target)) {
                    missing.push_back(e.name);
                    continue;
                }
                IShellItem* src = ItemFromPath(archive + L"\\" + e.name);
                IShellItem* to = ItemFromPath(target);
                if (src && to)
                    op->CopyItem(src, to, nullptr, nullptr);
                else
                    missing.push_back(e.name);
                SafeRelease(src);
                SafeRelease(to);
            }
            if (missing.size() == sel.size()) hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) hr = Perform(op);
        SafeRelease(op);
        if (!missing.empty()) {
            std::wstring msg = L"Folgende Elemente konnten im ZIP-Ordner der Shell nicht gefunden werden "
                               L"(z. B. wegen abweichender Zeichenkodierung der Namen). "
                               L"Bitte ggf. „Alles entpacken“ verwenden:\n";
            for (size_t k = 0; k < missing.size() && k < 15; ++k) msg += missing[k] + L"\n";
            if (missing.size() > 15) msg += L"…";
            MsgError(hwnd_, msg);
        }
        if (FAILED(hr)) {
            if (hr != E_ABORT && missing.empty())
                MsgError(hwnd_, L"Das Archiv konnte nicht entpackt werden:\n" + HrMessage(hr));
            return false;
        }
        return missing.empty();
    }

    // Andere Formate über tar.exe
    bool ExtractTar(bool all, const std::vector<int>& sel, const std::wstring& dest) {
        std::wstring base = Quote(tarExe) + L" -xf " + Quote(archive) + L" -C " + Quote(dest);
        std::vector<std::wstring> commands;
        if (all) {
            commands.push_back(base);
        } else {
            std::wstring cmd = base;
            bool hasMember = false;
            for (int i : sel) {
                std::wstring member = L" " + Quote(entries[i].rawName);
                if (hasMember && cmd.size() + member.size() > 30000) {
                    commands.push_back(cmd);
                    cmd = base;
                }
                cmd += member;
                hasMember = true;
            }
            commands.push_back(cmd);
        }
        std::wstring output;
        DWORD exitCode = 0;
        bool started = true;
        DWORD startErr = 0;
        bool completed = toolsdetail::RunWithProgress(
            hwnd_, L"Entpacken", L"Entpacke " + PathFileName(archive) + L" …", [&](toolsdetail::ProgressSink& sink) {
                for (const auto& c : commands) {
                    std::wstring out;
                    DWORD code = 0;
                    if (!CaptureProcess(c, dest, out, code, &sink)) {
                        if (!sink.Cancelled()) {
                            started = false;
                            startErr = GetLastError();
                        }
                        return;
                    }
                    output += out;
                    if (code != 0) {
                        exitCode = code;
                        return;
                    }
                }
            });
        if (!completed) {
            MsgInfo(hwnd_, L"Das Entpacken wurde abgebrochen. Bereits entpackte Dateien bleiben erhalten.");
            App::RefreshPanes();
            return false;
        }
        if (!started) {
            MsgError(hwnd_, L"tar.exe konnte nicht gestartet werden:\n" + LastErrorMessage(startErr));
            return false;
        }
        if (exitCode != 0) {
            MsgError(hwnd_, Format(L"tar.exe meldet einen Fehler (Code %u):\n", (unsigned)exitCode) + Trim(output));
            App::RefreshPanes();
            return false;
        }
        return true;
    }

    // Element mit dem Fokus in ein temporäres Verzeichnis entpacken und mit der Shell öffnen.
    void OpenFocused() {
        HWND lv = Item(kList);
        int i = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
        if (i < 0) i = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
        if (i < 0 || i >= (int)entries.size()) return;
        if (entries[i].isDir) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        std::wstring temp = PathCombine(GetTempDir(), L"QFiles_Archiv");
        temp = PathCombine(temp, MakeUniqueName(temp, PathStem(archive)));
        if (!EnsureDirectory(temp)) {
            MsgError(hwnd_, L"Temporäres Verzeichnis kann nicht angelegt werden:\n" + temp);
            return;
        }
        std::vector<int> one{i};
        bool ok = isZip ? ExtractZip(false, one, temp, true) : ExtractTar(false, one, temp);
        if (!ok) return;
        std::wstring file = isZip ? PathCombine(temp, PathFileName(entries[i].name)) : PathCombine(temp, entries[i].name);
        if (!FileExists(file)) {
            MsgError(hwnd_, L"Die entpackte Datei wurde nicht gefunden:\n" + file);
            return;
        }
        ShellOpen(hwnd_, file);
    }

    int folderIcon_ = 0;
    std::map<std::wstring, int> iconCache_;
    int sortCol_ = -1;
    bool sortAsc_ = true;
};

// ===================================================================================
// ZIP erstellen
// ===================================================================================

class CreateZipDlg : public DialogBase {
public:
    static constexpr int kInfo = 101;
    static constexpr int kPath = 102;
    static constexpr int kBrowse = 103;

    std::wstring dir;
    std::vector<std::wstring> names;
    std::wstring zipPath;

protected:
    BOOL OnInit() override {
        if (names.size() == 1)
            SetText(kInfo, L"Element: " + PathCombine(dir, names[0]));
        else
            SetText(kInfo, Format(L"%d Elemente aus ", (int)names.size()) + dir);
        SetText(kPath, zipPath);
        HWND e = Item(kPath);
        SetFocus(e);
        // Namen ohne Erweiterung markieren
        std::wstring stem = PathStem(zipPath);
        size_t start = zipPath.size() - PathFileName(zipPath).size();
        SendMessageW(e, EM_SETSEL, start, start + stem.size());
        return FALSE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == kBrowse) {
            std::wstring p = SaveFileDialog(hwnd_, L"ZIP-Archiv erstellen", GetText(kPath), L"ZIP-Archive|*.zip");
            if (!p.empty()) SetText(kPath, p);
            return TRUE;
        }
        if (id == IDOK) {
            std::wstring p = Trim(GetText(kPath));
            if (p.empty()) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            if (PathParent(p).empty()) p = PathCombine(PathParent(zipPath), p);   // nur Name eingegeben
            if (PathExtension(p).empty()) p += L".zip";
            zipPath = p;
            End(IDOK);
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }
};

} // namespace

// ===================================================================================
// Öffentliche Funktionen
// ===================================================================================

void ShowArchive(HWND owner, const std::wstring& archive, const std::wstring& extractDir) {
    if (!FileExists(archive)) {
        MsgError(owner, L"Das Archiv wurde nicht gefunden:\n" + archive);
        return;
    }
    ArchiveDlg dlg;
    dlg.archive = archive;
    dlg.extractDir = extractDir.empty() ? PathParent(archive) : extractDir;
    dlg.tarExe = TarExePath();

    std::wstring zipError;
    if (LooksLikeZip(archive) && ReadZipDirectory(archive, dlg.entries, zipError)) {
        dlg.isZip = true;
    } else {
        // Andere Formate (oder nicht lesbares ZIP) über tar.exe
        if (dlg.tarExe.empty()) {
            MsgError(owner, L"„" + PathFileName(archive) +
                                L"“ kann nicht geöffnet werden.\n\nFür dieses Format wird das Windows-Programm tar.exe "
                                L"benötigt (enthalten ab Windows 10 Version 1803), es wurde aber nicht gefunden." +
                                (zipError.empty() ? L"" : L"\n\nZIP: " + zipError));
            return;
        }
        std::wstring output;
        DWORD exitCode = 0;
        bool started = true;
        DWORD startErr = 0;
        std::wstring cmd = Quote(dlg.tarExe) + L" -tvf " + Quote(archive);
        bool completed = toolsdetail::RunWithProgress(owner, L"Archiv lesen", L"Lese " + PathFileName(archive) + L" …",
                                                      [&](toolsdetail::ProgressSink& sink) {
                                                          if (!CaptureProcess(cmd, PathParent(archive), output, exitCode,
                                                                              &sink) &&
                                                              !sink.Cancelled()) {
                                                              started = false;
                                                              startErr = GetLastError();
                                                          }
                                                      });
        if (!completed) return;
        if (!started) {
            MsgError(owner, L"tar.exe konnte nicht gestartet werden:\n" + LastErrorMessage(startErr));
            return;
        }
        std::wstring errors;
        ParseTarListing(output, dlg.entries, errors);
        if (dlg.entries.empty()) {
            MsgError(owner, L"„" + PathFileName(archive) +
                                L"“ wird von tar.exe nicht unterstützt oder ist beschädigt." +
                                (errors.empty() ? L"" : L"\n\n" + Trim(errors)));
            return;
        }
        if (exitCode != 0 && !errors.empty())
            MsgError(owner, L"tar.exe meldet Fehler beim Lesen des Archivs (Liste möglicherweise unvollständig):\n\n" +
                                Trim(errors));
    }

    DialogTemplate t(L"Archiv – " + PathFileName(archive), 460, 262, DialogTemplate::kResizable);
    t.ListView(ArchiveDlg::kList, 7, 7, 446, 176, LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_SHAREIMAGELISTS);
    t.Label(ArchiveDlg::kStatus, L"", 7, 188, 446, 10, SS_ENDELLIPSIS);
    t.Label(ArchiveDlg::kDestLabel, L"&Ziel:", 7, 205, 30, 10);
    t.Edit(ArchiveDlg::kDest, 40, 203, 392, 13);
    t.Button(ArchiveDlg::kDestBrowse, L"…", 435, 203, 18, 13);
    t.Check(ArchiveDlg::kSubDir, L"", 40, 221, 300, 10);
    t.Button(ArchiveDlg::kExtractAll, L"&Alles entpacken", 7, 241, 70, 14);
    t.Button(ArchiveDlg::kExtractSel, L"Aus&wahl entpacken", 81, 241, 75, 14);
    t.Button(ArchiveDlg::kOpenItem, L"Ö&ffnen", 160, 241, 50, 14);
    t.Button(IDCANCEL, L"Schließen", 403, 241, 50, 14);
    dlg.DoModal(owner, t);
}

bool CreateZipArchive(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names,
                      const std::wstring& targetDir) {
    if (names.empty()) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    std::wstring baseName;
    if (names.size() == 1) {
        std::wstring full = PathCombine(dir, names[0]);
        baseName = DirExists(full) ? names[0] : PathStem(names[0]);
    } else {
        baseName = LastPathElement(dir);
    }
    for (auto& c : baseName)
        if (c == L':' || c == L'\\' || c == L'/') c = L'_';
    if (baseName.empty()) baseName = L"Archiv";
    std::wstring tdir = targetDir.empty() ? dir : targetDir;

    DialogTemplate t(L"ZIP-Archiv erstellen", 300, 82);
    t.Label(CreateZipDlg::kInfo, L"", 7, 7, 286, 10, SS_PATHELLIPSIS);
    t.Label(-1, L"&ZIP-Datei:", 7, 25, 286, 10);
    t.Edit(CreateZipDlg::kPath, 7, 37, 264, 13);
    t.Button(CreateZipDlg::kBrowse, L"…", 275, 37, 18, 13);
    t.DefButton(IDOK, L"Erstellen", 186, 61, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 243, 61, 50, 14);
    CreateZipDlg dlg;
    dlg.dir = dir;
    dlg.names = names;
    dlg.zipPath = PathCombine(tdir, baseName + L".zip");
    if (dlg.DoModal(owner, t) != IDOK) return false;
    std::wstring zip = dlg.zipPath;

    // Ziel darf nicht in einem der Quellverzeichnisse liegen bzw. eine Quelle sein
    std::vector<std::wstring> sources;
    for (const auto& n : names) {
        std::wstring src = PathCombine(dir, n);
        if (EqualsI(src, zip)) {
            MsgError(owner, L"Die ZIP-Datei darf nicht eines der zu packenden Elemente sein.");
            return false;
        }
        if (DirExists(src) && StartsWithI(zip, src + L"\\")) {
            MsgError(owner, L"Die ZIP-Datei darf nicht in einem der zu packenden Verzeichnisse liegen.");
            return false;
        }
        sources.push_back(src);
    }

    bool created = false;
    if (FileExists(zip)) {
        int r = MsgYesNoCancel(owner, L"„" + zip +
                                          L"“ existiert bereits.\n\nJa = Datei ersetzen\nNein = Elemente zum vorhandenen "
                                          L"Archiv hinzufügen\nAbbrechen = nichts tun");
        if (r == IDCANCEL) return false;
        if (r == IDYES) {
            if (!DeleteFileW(LongPath(zip).c_str())) {
                MsgError(owner, L"„" + zip + L"“ kann nicht ersetzt werden:\n" + LastErrorMessage());
                return false;
            }
        }
    }
    if (!FileExists(zip)) {
        if (!EnsureDirectory(PathParent(zip))) {
            MsgError(owner, L"Das Zielverzeichnis kann nicht angelegt werden:\n" + PathParent(zip));
            return false;
        }
        // Leeres ZIP: nur End-of-Central-Directory (22 Byte)
        uint8_t empty[22] = {'P', 'K', 5, 6};
        if (!WriteFileBytes(zip, empty, sizeof(empty))) {
            MsgError(owner, L"„" + zip + L"“ kann nicht angelegt werden:\n" + LastErrorMessage());
            return false;
        }
        created = true;
    }

    // Quellen als IShellItemArray
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& s : sources) {
        PIDLIST_ABSOLUTE p = ILCreateFromPathW(s.c_str());
        if (p) pidls.push_back(p);
    }
    HRESULT hr = E_FAIL;
    if (!pidls.empty()) {
        IShellItemArray* arr = nullptr;
        IShellItem* dest = ItemFromPath(zip);
        IFileOperation* op = NewFileOperation(owner, FOF_NOCONFIRMMKDIR);
        hr = SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &arr);
        if (SUCCEEDED(hr) && dest && op) {
            hr = op->CopyItems(arr, dest);
            if (SUCCEEDED(hr)) hr = Perform(op);
        } else if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
        SafeRelease(op);
        SafeRelease(dest);
        SafeRelease(arr);
    }
    for (auto p : pidls) ILFree(p);

    if (FAILED(hr)) {
        // Neu angelegtes, leer gebliebenes Archiv wieder entfernen
        if (created && GetFileSize64(zip) <= 22) DeleteFileW(LongPath(zip).c_str());
        if (hr != E_ABORT) MsgError(owner, L"Das ZIP-Archiv konnte nicht erstellt werden:\n" + HrMessage(hr));
        App::RefreshPanes();
        return false;
    }
    LogOperation((created ? L"ZIP-Archiv erstellt: " : L"Zum ZIP-Archiv hinzugefügt: ") + zip +
                 Format(L" (%d Element(e) aus ", (int)sources.size()) + dir + L")");
    App::RefreshPanes();
    return true;
}

} // namespace qf
