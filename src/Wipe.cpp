// Radieren: Dateien vor dem endgültigen Löschen mit Zufallsdaten überschreiben (Alt+Entf).
// Ablauf je Datei: Schreibschutz aufheben, gesamten Inhalt mit kryptografisch zufälligen Daten überschreiben
// (Durchschreiben auf den Datenträger), auf Länge 0 kürzen, in einen zufälligen Namen umbenennen, löschen.
// Verzeichnisse werden rekursiv radiert und anschließend entfernt.
//
// Hinweis: Auf SSDs und Dateisystemen mit Kopien (Schattenkopien, Komprimierung, Cloud-Synchronisierung)
// kann das Überschreiben physisch an anderer Stelle landen; dann bleiben alte Daten unter Umständen lesbar.

#include "FileOps.h"
#include "App.h"
#include "ToolsCommon.h"
#include "Util.h"

#include <bcrypt.h>
#include <algorithm>
#include <vector>

namespace qf {

namespace {

constexpr size_t kChunk = 1 << 20; // 1 MB

bool RandomBytes(uint8_t* p, size_t n) {
    return BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::wstring RandomName(size_t len) {
    static const wchar_t* chars = L"abcdefghijklmnopqrstuvwxyz0123456789";
    std::vector<uint8_t> r(len);
    RandomBytes(r.data(), r.size());
    std::wstring s;
    for (auto b : r) s += chars[b % 36];
    return s;
}

struct WipeJob {
    std::vector<std::wstring> files;   // alle Dateien (vollständige Pfade)
    std::vector<std::wstring> dirs;    // Verzeichnisse (innere zuerst)
    uint64_t totalBytes = 0;
};

void Collect(const std::wstring& path, WipeJob& job) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::vector<DirEntry> entries;
            ListDirectory(path, entries);
            for (auto& e : entries) Collect(PathCombine(path, e.name), job);
        }
        job.dirs.push_back(path); // nach dem Inhalt -> innere Verzeichnisse zuerst
    } else {
        job.files.push_back(path);
        uint64_t s = GetFileSize64(path);
        if (s != UINT64_MAX) job.totalBytes += s;
    }
}

// Liefert "" bei Erfolg, sonst Fehlermeldung.
std::wstring WipeFile(const std::wstring& path, std::vector<uint8_t>& buf, uint64_t& doneBytes, uint64_t total,
                      toolsdetail::ProgressSink& sink) {
    std::wstring lp = LongPath(path);
    SetFileAttributesW(lp.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileW(lp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return LastErrorMessage();
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    uint64_t remaining = (uint64_t)size.QuadPart;
    std::wstring err;
    while (remaining > 0) {
        if (sink.Cancelled()) {
            err = L"abgebrochen";
            break;
        }
        DWORD n = (DWORD)std::min<uint64_t>(remaining, buf.size());
        if (!RandomBytes(buf.data(), n)) {
            err = L"Zufallsdaten konnten nicht erzeugt werden";
            break;
        }
        DWORD written = 0;
        if (!WriteFile(h, buf.data(), n, &written, nullptr) || written != n) {
            err = LastErrorMessage();
            break;
        }
        remaining -= n;
        doneBytes += n;
        if (total) sink.SetPercent((int)(doneBytes * 100 / total));
    }
    if (err.empty()) {
        FlushFileBuffers(h);
        // Länge 0, damit die ursprüngliche Größe nicht mehr erkennbar ist
        LARGE_INTEGER zero{};
        SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
    }
    CloseHandle(h);
    if (!err.empty()) return err;
    // Namen verschleiern, dann löschen
    std::wstring target = PathCombine(PathParent(path), RandomName(std::max<size_t>(8, PathFileName(path).size())));
    std::wstring finalPath = MoveFileExW(lp.c_str(), LongPath(target).c_str(), 0) ? target : path;
    if (!DeleteFileW(LongPath(finalPath).c_str())) return LastErrorMessage();
    return L"";
}

} // namespace

bool WipeItems(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    std::wstring what = paths.size() == 1 ? (L"„" + PathFileName(paths[0]) + L"“")
                                          : (std::to_wstring(paths.size()) + L" Elemente");
    std::wstring q = what +
                     L" radieren?\n\nDer Inhalt aller betroffenen Dateien wird mit Zufallsdaten überschrieben und "
                     L"anschließend endgültig gelöscht (auch in Unterverzeichnissen).\n\n"
                     L"Dies kann NICHT rückgängig gemacht werden – auch nicht über den Papierkorb.";
    if (MessageBoxW(owner, q.c_str(), L"Radieren", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return false;

    WipeJob job;
    for (auto& p : paths) Collect(p, job);
    std::vector<std::wstring> errors;
    size_t wipedFiles = 0;
    bool completed = toolsdetail::RunWithProgress(owner, L"Radieren", L"Überschreibe Dateien …",
                                                  [&](toolsdetail::ProgressSink& sink) {
        std::vector<uint8_t> buf(kChunk);
        uint64_t done = 0;
        for (auto& f : job.files) {
            if (sink.Cancelled()) return;
            sink.SetText(PathFileName(f));
            std::wstring e = WipeFile(f, buf, done, job.totalBytes, sink);
            if (e.empty()) ++wipedFiles;
            else errors.push_back(f + L": " + e);
        }
        for (auto& d : job.dirs) {
            if (sink.Cancelled()) return;
            std::wstring target = PathCombine(PathParent(d), RandomName(std::max<size_t>(8, PathFileName(d).size())));
            std::wstring finalPath = MoveFileExW(LongPath(d).c_str(), LongPath(target).c_str(), 0) ? target : d;
            if (!RemoveDirectoryW(LongPath(finalPath).c_str())) errors.push_back(d + L": " + LastErrorMessage());
        }
    });
    LogOperation(L"Radiert: " + std::to_wstring(wipedFiles) + L" Datei(en) – " + Join(paths, L"; "));
    if (!completed)
        MsgInfo(owner, L"Das Radieren wurde abgebrochen. Bereits bearbeitete Dateien sind überschrieben und gelöscht.");
    if (!errors.empty()) {
        if (errors.size() > 20) {
            size_t more = errors.size() - 20;
            errors.resize(20);
            errors.push_back(L"… und " + std::to_wstring(more) + L" weitere");
        }
        MsgError(owner, L"Nicht alle Elemente konnten radiert werden:\n\n" + Join(errors, L"\n"));
    }
    return wipedFiles > 0 || !job.dirs.empty();
}

} // namespace qf
