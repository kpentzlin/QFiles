// RecycleBin.cpp – Papierkorb als eigene Ansicht. Siehe RecycleBin.h.

#include "RecycleBin.h"

#include <shlobj.h>
#include <shlguid.h>
#include <shellapi.h>
#include <sddl.h>
#include <shlwapi.h>
#undef PathCombine   // Makro aus shlwapi.h; hier gilt qf::PathCombine

#include <algorithm>
#include <iterator>

#include "App.h"
#include "Dialog.h"
#include "FileOps.h"
#include "Util.h"

namespace qf {

namespace {

// Pfad der $I-Datei zu einem $R-Element
std::wstring InfoPathOf(const std::wstring& rPath) {
    std::wstring dir = PathParent(rPath), name = PathFileName(rPath);
    if (name.size() < 2 || (name[1] != L'R' && name[1] != L'r') || name[0] != L'$') return L"";
    return PathCombine(dir, L"$I" + name.substr(2));
}

bool ReadInfoFile(const std::wstring& iPath, std::wstring& original, uint64_t& size, FILETIME& deleted) {
    std::vector<uint8_t> d;
    if (!ReadFileBytes(iPath, d, 64 * 1024) || d.size() < 28) return false;
    auto u64 = [&](size_t o) {
        uint64_t v = 0;
        memcpy(&v, d.data() + o, 8);
        return v;
    };
    uint64_t version = u64(0);
    size = u64(8);
    uint64_t t = u64(16);
    deleted.dwLowDateTime = (DWORD)t;
    deleted.dwHighDateTime = (DWORD)(t >> 32);
    const wchar_t* p = nullptr;
    size_t maxChars = 0;
    if (version == 1) {
        p = reinterpret_cast<const wchar_t*>(d.data() + 24);
        maxChars = (d.size() - 24) / 2;
    } else if (version == 2) {
        uint32_t n = 0;
        memcpy(&n, d.data() + 24, 4);
        p = reinterpret_cast<const wchar_t*>(d.data() + 28);
        maxChars = std::min<size_t>(n, (d.size() - 28) / 2);
    } else {
        return false;
    }
    original.clear();
    for (size_t i = 0; i < maxChars && p[i]; ++i) original += p[i];
    return !original.empty();
}

void FillFileData(RecycledItem& it) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(LongPath(it.path).c_str(), GetFileExInfoStandard, &fad)) {
        it.attributes = fad.dwFileAttributes;
        it.created = fad.ftCreationTime;
        it.modified = fad.ftLastWriteTime;
        it.accessed = fad.ftLastAccessTime;
        if (!(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && it.size == 0)
            it.size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
}

std::wstring StrRetText(STRRET& sr, PCUITEMID_CHILD child) {
    wchar_t buf[MAX_PATH * 4] = {};
    if (FAILED(StrRetToBufW(&sr, child, buf, (UINT)std::size(buf)))) return L"";
    return buf;
}

// Über die Shell (wie im Explorer). false = Papierkorb-Ordner nicht verfügbar.
bool ListViaShell(std::vector<RecycledItem>& out) {
    PIDLIST_ABSOLUTE binPidl = nullptr;
    if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &binPidl)) || !binPidl) return false;
    IShellFolder* desktop = nullptr;
    IShellFolder2* bin = nullptr;
    bool ok = false;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop))) {
        if (SUCCEEDED(desktop->BindToObject(binPidl, nullptr, IID_PPV_ARGS(&bin))) && bin) {
            IEnumIDList* en = nullptr;
            HRESULT hr = bin->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en);
            if (hr == S_FALSE) ok = true;   // leer
            if (SUCCEEDED(hr) && en) {
                ok = true;
                PITEMID_CHILD child = nullptr;
                while (en->Next(1, &child, nullptr) == S_OK) {
                    RecycledItem it;
                    STRRET sr{};
                    if (SUCCEEDED(bin->GetDisplayNameOf(child, SHGDN_FORPARSING, &sr))) it.path = StrRetText(sr, child);
                    bool haveInfo = !it.path.empty() && RecycledInfo(it.path, it);
                    if (!haveInfo) {
                        // ohne $I-Datei: Angaben der Shell (ursprünglicher Ort, Löschdatum)
                        if (SUCCEEDED(bin->GetDisplayNameOf(child, SHGDN_INFOLDER, &sr))) it.name = StrRetText(sr, child);
                        if (!it.path.empty() && PathExtension(it.name).empty()) it.name += PathExtension(it.path);
                        SHCOLUMNID from{PSGUID_DISPLACED, PID_DISPLACED_FROM}, date{PSGUID_DISPLACED, PID_DISPLACED_DATE};
                        VARIANT v;
                        VariantInit(&v);
                        if (SUCCEEDED(bin->GetDetailsEx(child, &from, &v)) && v.vt == VT_BSTR && v.bstrVal) it.origDir = v.bstrVal;
                        VariantClear(&v);
                        if (SUCCEEDED(bin->GetDetailsEx(child, &date, &v)) && v.vt == VT_DATE) {
                            SYSTEMTIME st{}, utc{};
                            if (VariantTimeToSystemTime(v.date, &st) && TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc))
                                SystemTimeToFileTime(&utc, &it.deleted);
                        }
                        VariantClear(&v);
                        if (!it.path.empty()) FillFileData(it);
                    }
                    CoTaskMemFree(child);
                    if (!it.path.empty() && !it.name.empty()) out.push_back(std::move(it));
                }
                en->Release();
            }
            bin->Release();
        }
        desktop->Release();
    }
    CoTaskMemFree(binPidl);
    return ok;
}

std::wstring CurrentUserSid() {
    std::wstring r;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return r;
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<uint8_t> buf(len ? len : 1);
    if (len && GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s) && s) {
            r = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return r;
}

// Ohne Shell: $Recycle.Bin der lokalen Laufwerke direkt lesen
void ListDirect(std::vector<RecycledItem>& out) {
    std::wstring sid = CurrentUserSid();
    if (sid.empty()) return;
    wchar_t drives[512] = {};
    DWORD n = GetLogicalDriveStringsW(511, drives);
    for (const wchar_t* d = drives; n && *d; d += wcslen(d) + 1) {
        UINT type = GetDriveTypeW(d);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        std::wstring dir = std::wstring(d) + L"$Recycle.Bin\\" + sid;
        std::vector<DirEntry> entries;
        if (!ListDirectory(dir, entries)) continue;
        for (auto& e : entries) {
            if (e.name.size() < 3 || e.name[0] != L'$' || (e.name[1] != L'I' && e.name[1] != L'i')) continue;
            RecycledItem it;
            it.path = PathCombine(dir, L"$R" + e.name.substr(2));
            if (PathExists(it.path) && RecycledInfo(it.path, it)) out.push_back(std::move(it));
        }
    }
}

} // namespace

bool IsRecycledPath(const std::wstring& path) {
    std::wstring name = PathFileName(path);
    if (name.size() < 3 || name[0] != L'$' || (name[1] != L'R' && name[1] != L'r')) return false;
    return ToLower(path).find(L"\\$recycle.bin\\") != std::wstring::npos;
}

bool RecycledInfo(const std::wstring& rPath, RecycledItem& out) {
    std::wstring iPath = InfoPathOf(rPath);
    if (iPath.empty()) return false;
    std::wstring original;
    uint64_t size = 0;
    FILETIME deleted{};
    if (!ReadInfoFile(iPath, original, size, deleted)) return false;
    out.path = rPath;
    out.name = PathFileName(original);
    out.origDir = PathParent(original);
    out.deleted = deleted;
    out.size = size;
    FillFileData(out);
    return true;
}

bool ListRecycleBin(std::vector<RecycledItem>& out, std::wstring& err) {
    out.clear();
    if (ListViaShell(out)) return true;
    ListDirect(out);
    return true;
}

std::vector<std::wstring> RestoreRecycled(HWND owner, const std::vector<std::wstring>& rPaths, const std::wstring& targetDir,
                                          bool copy) {
    std::vector<std::wstring> restored;
    std::vector<CopyJob> jobs;
    std::vector<std::pair<std::wstring, std::wstring>> pairs;   // ($R, Ziel)
    std::vector<std::wstring> unknown;
    for (auto& r : rPaths) {
        RecycledItem it;
        if (!RecycledInfo(r, it)) {
            unknown.push_back(PathFileName(r));
            continue;
        }
        std::wstring dir = targetDir.empty() ? it.origDir : targetDir;
        if (!DirExists(dir)) {
            int rc = SHCreateDirectoryExW(owner, LongPath(dir).c_str(), nullptr);
            if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
                MsgError(owner, L"Das Verzeichnis „" + dir + L"“ kann nicht angelegt werden:\n" + LastErrorMessage(rc));
                continue;
            }
        }
        jobs.push_back({r, dir, it.name});
        pairs.emplace_back(r, PathCombine(dir, it.name));
    }
    if (!unknown.empty())
        MsgError(owner, L"Zu folgenden Elementen fehlen die Angaben des Papierkorbs (ursprünglicher Ort):\n" +
                            Join(unknown, L"\n"));
    if (jobs.empty()) return restored;
    if (copy)
        CopyJobs(owner, jobs, OpNoUndo);
    else
        MoveJobs(owner, jobs, OpNoUndo);
    for (auto& [r, target] : pairs) {
        if (!copy && PathExists(r)) continue;   // nicht verschoben (abgebrochen, übersprungen)
        if (!copy) DeleteFileW(LongPath(InfoPathOf(r)).c_str());
        restored.push_back(target);
        LogOperation((copy ? L"Aus dem Papierkorb kopiert: " : L"Wiederhergestellt: ") + target);
    }
    return restored;
}

bool DeleteRecycled(HWND owner, const std::vector<std::wstring>& rPaths, bool confirm) {
    if (rPaths.empty()) return true;
    if (confirm) {
        std::wstring what;
        if (rPaths.size() == 1) {
            RecycledItem it;
            what = L"„" + (RecycledInfo(rPaths[0], it) ? it.name : PathFileName(rPaths[0])) + L"“";
        } else {
            what = std::to_wstring(rPaths.size()) + L" Elemente";
        }
        if (MessageBoxW(owner, (what + L" endgültig löschen?\n\nDies kann nicht rückgängig gemacht werden.").c_str(),
                        L"Aus dem Papierkorb löschen", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return false;
    }
    bool ok = DeleteItems(owner, rPaths, false, false);
    for (auto& r : rPaths)
        if (!PathExists(r)) DeleteFileW(LongPath(InfoPathOf(r)).c_str());
    return ok;
}

bool QueryRecycleBin(uint64_t& items, uint64_t& bytes) {
    SHQUERYRBINFO qi{sizeof(qi)};
    if (FAILED(SHQueryRecycleBinW(nullptr, &qi))) return false;
    items = (uint64_t)qi.i64NumItems;
    bytes = (uint64_t)qi.i64Size;
    return true;
}

bool EmptyRecycleBinAsk(HWND owner) {
    uint64_t items = 0, bytes = 0;
    bool known = QueryRecycleBin(items, bytes);
    if (known && items == 0) {
        MsgInfo(owner, L"Der Papierkorb ist leer.");
        return false;
    }
    std::wstring q = known ? L"Papierkorb leeren – " + IntToStrGrouped(items) + (items == 1 ? L" Element (" : L" Elemente (") +
                                 FormatSize(bytes) + L") endgültig löschen?"
                           : std::wstring(L"Papierkorb leeren – alle Elemente endgültig löschen?");
    if (MessageBoxW(owner, (q + L"\n\nDies kann nicht rückgängig gemacht werden.").c_str(), L"Papierkorb leeren",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return false;
    HRESULT hr = SHEmptyRecycleBinW(owner, nullptr, SHERB_NOCONFIRMATION | SHERB_NOSOUND);
    if (FAILED(hr) && hr != E_UNEXPECTED) {   // E_UNEXPECTED: war bereits leer
        MsgError(owner, L"Der Papierkorb konnte nicht geleert werden:\n" + LastErrorMessage((DWORD)hr));
        return false;
    }
    LogOperation(L"Papierkorb geleert");
    return true;
}

} // namespace qf
