#include "FileOps.h"
#include "App.h"
#include "Util.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <sherrors.h>
#include <deque>

namespace qf {

namespace {

// DeleteCreatedIfEmpty: neu angelegte Datei/Verzeichnis – beim Rückgängigmachen nur löschen, solange noch leer
// (sonst könnten inzwischen hineinkopierte bzw. geschriebene Inhalte verloren gehen).
enum class UndoType { DeleteCreated, DeleteCreatedIfEmpty, MoveBack, RenameBack };

struct UndoAction {
    UndoType type;
    std::wstring current;   // Pfad, der jetzt existiert
    std::wstring original;  // ursprünglicher Pfad (für MoveBack/RenameBack)
};

struct UndoStep {
    std::wstring description;
    std::vector<UndoAction> actions;
};

std::deque<UndoStep> g_undo;
bool g_nonInteractive = false;
int g_groupDepth = 0;
UndoStep g_group;

void PushUndo(UndoStep step) {
    if (step.actions.empty()) return;
    if (g_groupDepth > 0) {
        for (auto& a : step.actions) g_group.actions.push_back(std::move(a));
        return;
    }
    g_undo.push_back(std::move(step));
    while (g_undo.size() > 50) g_undo.pop_front();
}

std::wstring ItemPath(IShellItem* item) {
    std::wstring r;
    if (!item) return r;
    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        r = p;
        CoTaskMemFree(p);
    }
    return r;
}

// Sammelt neu erzeugte Elemente für Rückgängig und Protokoll.
class ProgressSink : public IFileOperationProgressSink {
public:
    std::vector<std::wstring> topSources; // nur diese Quellen interessieren (nicht die Unterelemente)
    std::vector<std::pair<std::wstring, std::wstring>> created; // (Quelle, neues Element)
    std::vector<std::wstring> deleted;
    bool aborted = false;
    virtual ~ProgressSink() = default;

    bool IsTop(const std::wstring& p) const {
        for (auto& s : topSources)
            if (EqualsI(s, p)) return true;
        return false;
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IFileOperationProgressSink)) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    // IFileOperationProgressSink
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT hr) override {
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED) aborted = true;
        return S_OK;
    }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* src, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override {
        Record(src, hr, created);
        return S_OK;
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem* src, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override {
        Record(src, hr, created);
        return S_OK;
    }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* item, HRESULT hr, IShellItem*) override {
        if (SUCCEEDED(hr)) {
            std::wstring p = ItemPath(item);
            if (IsTop(p)) deleted.push_back(p);
        }
        return S_OK;
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }

private:
    void Record(IShellItem* src, HRESULT hr, IShellItem* createdItem) {
        if (FAILED(hr) || !createdItem) return;
        std::wstring s = ItemPath(src);
        if (!IsTop(s)) return;
        created.emplace_back(s, ItemPath(createdItem));
    }
    ULONG ref_ = 1;
};

IShellItem* CreateItem(const std::wstring& path) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return nullptr;
    return item;
}

DWORD BaseFlags(unsigned flags) {
    DWORD f = FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT;
    if (g_nonInteractive) f |= FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (flags & OpNoConfirmOverwrite) f |= FOF_NOCONFIRMATION;
    if (flags & OpRenameOnCollision) f |= FOF_RENAMEONCOLLISION;
    if (flags & OpSilent) f |= FOF_SILENT;
    return f;
}

bool RunTransfer(HWND owner, const std::vector<CopyJob>& jobs, bool move, unsigned flags) {
    if (jobs.empty()) return true;
    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
        MsgError(owner, L"Die Dateioperation konnte nicht gestartet werden (IFileOperation).");
        return false;
    }
    op->SetOwnerWindow(owner);
    op->SetOperationFlags(BaseFlags(flags));
    auto* sink = new ProgressSink();
    DWORD cookie = 0;
    op->Advise(sink, &cookie);

    std::vector<bool> existedBefore;
    int queued = 0;
    for (auto& j : jobs) {
        IShellItem* src = CreateItem(j.source);
        IShellItem* dst = CreateItem(j.destDir);
        if (!dst && !j.destDir.empty()) {
            SHCreateDirectoryExW(owner, j.destDir.c_str(), nullptr);
            dst = CreateItem(j.destDir);
        }
        if (!src || !dst) {
            if (src) src->Release();
            if (dst) dst->Release();
            MsgError(owner, L"Element nicht gefunden:\n" + (src ? j.destDir : j.source));
            continue;
        }
        sink->topSources.push_back(j.source);
        const wchar_t* newName = j.newName.empty() ? nullptr : j.newName.c_str();
        HRESULT hr = move ? op->MoveItem(src, dst, newName, nullptr) : op->CopyItem(src, dst, newName, nullptr);
        if (SUCCEEDED(hr)) ++queued;
        src->Release();
        dst->Release();
    }
    // Ziele, die schon vorher existierten (Zusammenführen/Überschreiben), nicht rückgängig machen
    std::vector<std::wstring> preExisting;
    for (auto& j : jobs) {
        std::wstring target = PathCombine(j.destDir, j.newName.empty() ? PathFileName(j.source) : j.newName);
        if (PathExists(target)) preExisting.push_back(target);
    }

    HRESULT hr = queued ? op->PerformOperations() : E_FAIL;
    if (queued && FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) && hr != COPYENGINE_E_USER_CANCELLED)
        MsgError(owner, std::wstring(move ? L"Verschieben" : L"Kopieren") + L" fehlgeschlagen:\n" + LastErrorMessage((DWORD)hr));
    BOOL anyAborted = FALSE;
    op->GetAnyOperationsAborted(&anyAborted);
    op->Unadvise(cookie);
    op->Release();

    UndoStep step;
    step.description = move ? L"Verschieben" : L"Kopieren";
    for (auto& [src, created] : sink->created) {
        LogOperation(std::wstring(move ? L"Verschoben: " : L"Kopiert: ") + src + L" → " + created);
        // Sicherheit: Rückgängig nur für Elemente, die wirklich im Zielverzeichnis eines Auftrags neu entstanden
        // sind und nicht mit der Quelle identisch sind – nie etwas anderes löschen oder verschieben.
        bool inTarget = false;
        for (auto& j : jobs)
            if (EqualsI(j.source, src) && EqualsI(NormalizeDir(PathParent(created)), NormalizeDir(j.destDir))) inTarget = true;
        if (!inTarget || EqualsI(created, src)) continue;
        bool pre = false;
        for (auto& p : preExisting)
            if (EqualsI(p, created)) pre = true;
        if (pre || (flags & OpNoUndo)) continue;
        if (move)
            step.actions.push_back({UndoType::MoveBack, created, src});
        else
            step.actions.push_back({UndoType::DeleteCreated, created, L""});
    }
    if (!step.actions.empty()) {
        step.description += L" (" + std::to_wstring(step.actions.size()) + L" Element" +
                            (step.actions.size() == 1 ? L")" : L"e)");
        PushUndo(std::move(step));
    }
    sink->Release();
    return SUCCEEDED(hr) && !anyAborted;
}

} // namespace

bool CopyItems(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destDir, unsigned flags) {
    std::vector<CopyJob> jobs;
    for (auto& s : sources) {
        // Kopieren in dasselbe Verzeichnis: automatisch "Name (2)" erzeugen
        if (EqualsI(PathParent(s), destDir))
            jobs.push_back({s, destDir, MakeUniqueName(destDir, PathFileName(s))});
        else
            jobs.push_back({s, destDir, L""});
    }
    return RunTransfer(owner, jobs, false, flags);
}

bool MoveItems(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destDir, unsigned flags) {
    std::vector<CopyJob> jobs;
    for (auto& s : sources) {
        if (EqualsI(PathParent(s), destDir)) continue; // nichts zu tun
        jobs.push_back({s, destDir, L""});
    }
    return RunTransfer(owner, jobs, true, flags);
}

bool CopyJobs(HWND owner, const std::vector<CopyJob>& jobs, unsigned flags) { return RunTransfer(owner, jobs, false, flags); }
bool MoveJobs(HWND owner, const std::vector<CopyJob>& jobs, unsigned flags) { return RunTransfer(owner, jobs, true, flags); }

bool DeleteItems(HWND owner, const std::vector<std::wstring>& paths, bool recycle, bool confirm) {
    if (paths.empty()) return true;
    if (confirm) {
        std::wstring what = paths.size() == 1 ? (L"„" + PathFileName(paths[0]) + L"“")
                                              : (std::to_wstring(paths.size()) + L" Elemente");
        std::wstring q = recycle ? (what + L" in den Papierkorb verschieben?")
                                 : (what + L" endgültig löschen?\n\nDies kann nicht rückgängig gemacht werden.");
        if (MessageBoxW(owner, q.c_str(), L"Löschen", MB_YESNO | (recycle ? MB_ICONQUESTION : MB_ICONWARNING) |
                                                          MB_DEFBUTTON1) != IDYES)
            return false;
    }
    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return false;
    op->SetOwnerWindow(owner);
    DWORD f = FOF_NOCONFIRMATION | FOFX_SHOWELEVATIONPROMPT;
    if (recycle) f |= FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO;
    else if (!g_nonInteractive) f |= FOF_WANTNUKEWARNING;
    if (g_nonInteractive) f |= FOF_SILENT | FOF_NOERRORUI;
    op->SetOperationFlags(f);
    auto* sink = new ProgressSink();
    DWORD cookie = 0;
    op->Advise(sink, &cookie);
    for (auto& p : paths) {
        IShellItem* item = CreateItem(p);
        if (!item) continue;
        sink->topSources.push_back(p);
        op->DeleteItem(item, nullptr);
        item->Release();
    }
    HRESULT hr = op->PerformOperations();
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) && hr != COPYENGINE_E_USER_CANCELLED)
        MsgError(owner, L"Löschen fehlgeschlagen:\n" + LastErrorMessage((DWORD)hr));
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    op->Unadvise(cookie);
    op->Release();
    for (auto& d : sink->deleted) LogOperation((recycle ? L"In Papierkorb: " : L"Gelöscht: ") + d);
    sink->Release();
    return SUCCEEDED(hr) && !aborted;
}

bool RenameItem(HWND owner, const std::wstring& path, const std::wstring& newName) {
    std::wstring name = Trim(newName);
    if (name.empty() || name == PathFileName(path)) return true;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MsgError(owner, L"Ein Dateiname darf keines der folgenden Zeichen enthalten:\n\\ / : * ? \" < > |");
        return false;
    }
    std::wstring target = PathCombine(PathParent(path), name);
    if (!EqualsI(target, path) && PathExists(target)) {
        MsgError(owner, L"„" + name + L"“ existiert bereits.");
        return false;
    }
    if (!MoveFileExW(LongPath(path).c_str(), LongPath(target).c_str(), 0)) {
        MsgError(owner, L"Umbenennen von „" + PathFileName(path) + L"“ nicht möglich:\n" + LastErrorMessage());
        return false;
    }
    LogOperation(L"Umbenannt: " + path + L" → " + name);
    UndoStep step;
    step.description = L"Umbenennen";
    step.actions.push_back({UndoType::RenameBack, target, path});
    PushUndo(std::move(step));
    SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), target.c_str());
    return true;
}

bool CreateFolder(HWND owner, const std::wstring& dir, const std::wstring& name) {
    std::wstring full = PathCombine(dir, Trim(name));
    int r = SHCreateDirectoryExW(owner, full.c_str(), nullptr);
    if (r != ERROR_SUCCESS) {
        MsgError(owner, L"Verzeichnis „" + name + L"“ kann nicht angelegt werden:\n" + LastErrorMessage(r));
        return false;
    }
    LogOperation(L"Verzeichnis angelegt: " + full);
    PushUndo({L"Verzeichnis anlegen", {{UndoType::DeleteCreatedIfEmpty, full, L""}}});
    return true;
}

bool CreateEmptyFile(HWND owner, const std::wstring& dir, const std::wstring& name) {
    std::wstring full = PathCombine(dir, Trim(name));
    HANDLE h = CreateFileW(LongPath(full).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        MsgError(owner, L"Datei „" + name + L"“ kann nicht angelegt werden:\n" + LastErrorMessage());
        return false;
    }
    CloseHandle(h);
    LogOperation(L"Datei angelegt: " + full);
    PushUndo({L"Datei anlegen", {{UndoType::DeleteCreatedIfEmpty, full, L""}}});
    return true;
}

void BeginUndoGroup(const std::wstring& description) {
    if (g_groupDepth++ == 0) {
        g_group = UndoStep();
        g_group.description = description;
    }
}

void EndUndoGroup() {
    if (g_groupDepth <= 0) return;
    if (--g_groupDepth == 0) {
        UndoStep s = std::move(g_group);
        g_group = UndoStep();
        PushUndo(std::move(s));
    }
}

bool CanUndo() { return !g_undo.empty(); }
void SetFileOpsNonInteractive(bool on) {
    g_nonInteractive = on;
    SetMessageBoxesSuppressed(on);
}
std::wstring UndoDescription() { return g_undo.empty() ? L"" : g_undo.back().description; }

bool UndoLast(HWND owner) {
    if (g_undo.empty()) return false;
    UndoStep step = std::move(g_undo.back());
    g_undo.pop_back();
    if (!g_nonInteractive && !MsgConfirm(owner, L"„" + step.description + L"“ rückgängig machen?")) {
        g_undo.push_back(std::move(step));
        return false;
    }
    std::vector<std::wstring> toDelete;
    std::vector<CopyJob> moveBack;
    bool ok = true;
    // In umgekehrter Reihenfolge rückgängig machen
    for (auto it = step.actions.rbegin(); it != step.actions.rend(); ++it) {
        switch (it->type) {
        case UndoType::DeleteCreated:
            if (PathExists(it->current)) toDelete.push_back(it->current);
            break;
        case UndoType::DeleteCreatedIfEmpty: {
            DWORD a = GetFileAttributesW(LongPath(it->current).c_str());
            if (a == INVALID_FILE_ATTRIBUTES) break;
            std::vector<DirEntry> entries;
            bool empty = (a & FILE_ATTRIBUTE_DIRECTORY) ? (ListDirectory(it->current, entries) && entries.empty())
                                                        : GetFileSize64(it->current) == 0;
            if (empty) {
                toDelete.push_back(it->current);
            } else {
                MsgError(owner, L"„" + it->current + L"“ ist nicht mehr leer und wird nicht gelöscht.");
                ok = false;
            }
            break;
        }
        case UndoType::MoveBack:
            // Nie ein inzwischen am Ursprungsort vorhandenes (fremdes) Element überschreiben
            if (PathExists(it->original)) {
                MsgError(owner, L"Rückgängig nicht möglich: „" + it->original + L"“ existiert bereits.");
                ok = false;
                break;
            }
            moveBack.push_back({it->current, PathParent(it->original), PathFileName(it->original)});
            break;
        case UndoType::RenameBack:
            if (!MoveFileExW(LongPath(it->current).c_str(), LongPath(it->original).c_str(), 0)) {
                MsgError(owner, L"Rückgängig nicht möglich für „" + it->current + L"“:\n" + LastErrorMessage());
                ok = false;
            } else {
                LogOperation(L"Rückgängig (Umbenennen): " + it->current + L" → " + PathFileName(it->original));
            }
            break;
        }
    }
    if (!moveBack.empty()) ok = RunTransfer(owner, moveBack, true, OpNoUndo) && ok;
    if (!toDelete.empty()) ok = DeleteItems(owner, toDelete, true, false) && ok;
    return ok;
}

// ===================== Zwischenablage =====================

bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths, bool cut) {
    if (paths.empty()) return false;
    size_t chars = 1;
    for (auto& p : paths) chars += p.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!g) return false;
    auto* df = (DROPFILES*)GlobalLock(g);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* p = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
    for (auto& s : paths) {
        memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        p += s.size() + 1;
    }
    *p = 0;
    GlobalUnlock(g);

    HGLOBAL ge = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (ge) {
        *(DWORD*)GlobalLock(ge) = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        GlobalUnlock(ge);
    }
    if (!OpenClipboard(owner)) {
        GlobalFree(g);
        if (ge) GlobalFree(ge);
        return false;
    }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, g);
    if (ge) SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), ge);
    // Zusätzlich als Text (Pfade)
    std::wstring text = Join(paths, L"\r\n");
    HGLOBAL gt = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (gt) {
        memcpy(GlobalLock(gt), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(gt);
        SetClipboardData(CF_UNICODETEXT, gt);
    }
    CloseClipboard();
    return true;
}

bool ClipboardGetFiles(HWND owner, std::vector<std::wstring>& paths, bool& cut) {
    paths.clear();
    cut = false;
    if (!OpenClipboard(owner)) return false;
    HDROP drop = (HDROP)GetClipboardData(CF_HDROP);
    if (drop) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(len + 1, L'\0');
            DragQueryFileW(drop, i, s.data(), len + 1);
            s.resize(len);
            paths.push_back(s);
        }
        HANDLE he = GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        if (he) {
            DWORD* pe = (DWORD*)GlobalLock(he);
            if (pe) cut = (*pe & DROPEFFECT_MOVE) != 0;
            GlobalUnlock(he);
        }
    }
    CloseClipboard();
    return !paths.empty();
}

bool ClipboardHasFiles() { return IsClipboardFormatAvailable(CF_HDROP) != FALSE; }

} // namespace qf
