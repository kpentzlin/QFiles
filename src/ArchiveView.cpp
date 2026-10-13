// ArchiveView.cpp – Archivliste in der Dateianzeige (F11 auf einer gepackten Datei).
//
// Zeigt den Inhalt eines Archivs wie ein Dateiverzeichnis: Unterverzeichnisse öffnen (Eingabe/Doppelklick, „..“,
// Rücktaste), Spalten Name/Typ/Größe/Gepackt/Geändert/Methode mit Sortierung per Spaltenklick, Tippsuche, Suche nach
// Namen im ganzen Archiv (Strg+F), Herausziehen mit der Maus (Kopie), Strg+C, „Entpacken nach…“ und „Temporäre
// Kopie öffnen“. Das Archiv selbst wird nie verändert. Hintergrund blassgelb; ohne Fokus und mit der Option
// „Nicht aktive Listen grau hinterlegen“ dunkler abgetöntes Gelb.

#include "ArchiveCore.h"
#include "App.h"
#include "FileOps.h"
#include "Modules.h"
#include "Settings.h"
#include "ShellMenu.h"
#include "Util.h"

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qf {

namespace {

constexpr wchar_t kClass[] = L"QFilesArchiveView";
constexpr int kListId = 1;
enum Col { ColName, ColType, ColSize, ColPacked, ColDate, ColMethod, ColCount };
const wchar_t* const kTitles[ColCount] = {L"Name", L"Typ", L"Größe", L"Gepackt", L"Geändert", L"Methode"};
const int kWidths[ColCount] = {230, 50, 80, 80, 130, 90};
constexpr COLORREF kPale = RGB(255, 251, 214);    // blassgelb (aktiv)
constexpr COLORREF kToned = RGB(240, 230, 176);   // dunkler abgetöntes Gelb (nicht aktiv / Streifen)

enum : UINT { IDM_OPEN = 1, IDM_OPENTEMP, IDM_EXTRACT, IDM_COPY, IDM_SEARCH, IDM_UP, IDM_VIEW };

struct State {
    HWND hwnd = nullptr, list = nullptr;
    HFONT listFont = nullptr;
    std::wstring archive;
    std::vector<ArchiveEntry> entries;
    std::unordered_map<std::wstring, std::vector<int>> children;   // klein geschriebener Ordnerpfad -> Einträge
    std::wstring cur;                                               // aktueller Ordner ("" = oberste Ebene)
    bool search = false;
    std::wstring pattern;
    std::vector<int> rows;                                          // -1 = ".."
    int sortCol = ColName;
    bool sortAsc = true;
    int folderIcon = 0;
    std::map<std::wstring, int> icons;
    bool focused = false;
    std::wstring info;
};

State* Get(HWND h) { return h ? reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA)) : nullptr; }

HWND Owner(State& s) { return GetAncestor(s.hwnd, GA_ROOT); }

int InfoHeight(State& s) { return DpiScale(s.hwnd, 22); }

std::wstring ParentOf(const std::wstring& p) {
    size_t k = p.rfind(L'\\');
    return k == std::wstring::npos ? L"" : p.substr(0, k);
}

std::wstring LeafOf(const std::wstring& p) {
    size_t k = p.rfind(L'\\');
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

std::wstring DisplayName(const State& s, const ArchiveEntry& e) { return s.search ? e.path : LeafOf(e.path); }

std::wstring TypeOf(const ArchiveEntry& e) {
    if (e.isDir) return L"";
    std::wstring ext = PathExtension(LeafOf(e.path));
    return ext.empty() ? ext : ext.substr(1);
}

int IconFor(State& s, const ArchiveEntry& e) {
    if (e.isDir) return s.folderIcon;
    std::wstring ext = ToLower(PathExtension(LeafOf(e.path)));
    auto it = s.icons.find(ext);
    if (it != s.icons.end()) return it->second;
    SHFILEINFOW sfi{};
    std::wstring probe = L"datei" + ext;
    SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    s.icons[ext] = sfi.iIcon;
    return sfi.iIcon;
}

void UpdateColors(State& s) {
    if (!s.list) return;
    COLORREF bg = (!s.focused && App::Opt().grayInactive) ? kToned : kPale;
    ListView_SetBkColor(s.list, bg);
    ListView_SetTextBkColor(s.list, bg);
    InvalidateRect(s.list, nullptr, TRUE);
}

void UpdateInfo(State& s) {
    if (s.search) {
        s.info = L"Suche „" + s.pattern + L"“ in " + PathFileName(s.archive) + L": " + IntToStrGrouped(s.rows.size()) +
                 L" Treffer  ·  Esc oder Rücktaste: zurück";
    } else {
        size_t files = 0, dirs = 0;
        uint64_t total = 0;
        for (int r : s.rows) {
            if (r < 0) continue;
            const ArchiveEntry& e = s.entries[r];
            if (e.isDir)
                ++dirs;
            else {
                ++files;
                total += e.size;
            }
        }
        s.info = PathFileName(s.archive) + (s.cur.empty() ? L"" : L"\\" + s.cur) + L"  ·  " +
                 IntToStrGrouped(dirs) + L" Verz., " + IntToStrGrouped(files) + L" Dateien (" + FormatSize(total) + L")";
    }
    RECT r{0, 0, 0, InfoHeight(s)};
    GetClientRect(s.hwnd, &r);
    r.bottom = InfoHeight(s);
    InvalidateRect(s.hwnd, &r, FALSE);
}

void UpdateSortArrow(State& s) {
    HWND hdr = ListView_GetHeader(s.list);
    for (int i = 0; i < Header_GetItemCount(hdr); ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == s.sortCol) hi.fmt |= s.sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hi);
    }
}

void SortRows(State& s) {
    bool dirsFirst = App::Opt().dirsFirst;
    auto first = std::stable_partition(s.rows.begin(), s.rows.end(), [](int r) { return r < 0; });
    std::stable_sort(first, s.rows.end(), [&](int x, int y) {
        const ArchiveEntry& a = s.entries[x];
        const ArchiveEntry& b = s.entries[y];
        if (dirsFirst && a.isDir != b.isDir) return a.isDir;
        int c = 0;
        switch (s.sortCol) {
        case ColType: c = CompareI(TypeOf(a), TypeOf(b)); break;
        case ColSize: c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
        case ColPacked: c = a.packed < b.packed ? -1 : (a.packed > b.packed ? 1 : 0); break;
        case ColDate: c = CompareFileTime(&a.modified, &b.modified); break;
        case ColMethod: c = CompareI(a.method, b.method); break;
        default: break;
        }
        if (c == 0) c = CompareNatural(DisplayName(s, a), DisplayName(s, b));
        return s.sortAsc ? c < 0 : c > 0;
    });
    UpdateSortArrow(s);
}

void Fill(State& s, const std::wstring& focusPath) {
    ListView_SetItemState(s.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(s.list, (int)s.rows.size(), 0);
    int focus = s.rows.empty() ? -1 : 0;
    for (size_t i = 0; i < s.rows.size() && !focusPath.empty(); ++i)
        if (s.rows[i] >= 0 && EqualsI(s.entries[s.rows[i]].path, focusPath)) focus = (int)i;
    if (focus >= 0) {
        ListView_SetItemState(s.list, focus, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
        ListView_EnsureVisible(s.list, focus, FALSE);
    }
    InvalidateRect(s.list, nullptr, TRUE);
    UpdateInfo(s);
}

void Navigate(State& s, const std::wstring& folder, const std::wstring& focusPath = L"") {
    s.search = false;
    s.cur = folder;
    s.rows.clear();
    if (!s.cur.empty()) s.rows.push_back(-1);
    auto it = s.children.find(ToLower(s.cur));
    if (it != s.children.end()) s.rows.insert(s.rows.end(), it->second.begin(), it->second.end());
    SortRows(s);
    Fill(s, focusPath);
}

void Build(State& s) {
    s.children.clear();
    // Verzeichnisse ergänzen, die nur aus den Pfaden der Dateien hervorgehen
    std::unordered_set<std::wstring> dirs;
    std::unordered_set<std::wstring> seen;
    std::vector<ArchiveEntry> unique;
    for (auto& e : s.entries) {
        std::wstring key = ToLower(e.path);
        if (!seen.insert(key + (e.isDir ? L"\\" : L"")).second) continue;   // doppelte Einträge (z. B. aktualisierte tar)
        if (e.isDir) dirs.insert(key);
        unique.push_back(std::move(e));
    }
    s.entries.swap(unique);
    size_t n = s.entries.size();
    for (size_t i = 0; i < n; ++i) {
        std::wstring p = s.entries[i].path;
        for (size_t k; (k = p.rfind(L'\\')) != std::wstring::npos;) {
            p.resize(k);
            if (dirs.insert(ToLower(p)).second) {
                ArchiveEntry d;
                d.path = p;
                d.isDir = true;
                d.synthetic = true;
                s.entries.push_back(d);
            }
        }
    }
    for (size_t i = 0; i < s.entries.size(); ++i) s.children[ToLower(ParentOf(s.entries[i].path))].push_back((int)i);
}

std::vector<int> SelectedEntries(State& s) {
    std::vector<int> r;
    for (int i = ListView_GetNextItem(s.list, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(s.list, i, LVNI_SELECTED))
        if (i < (int)s.rows.size() && s.rows[i] >= 0) r.push_back(s.rows[i]);
    return r;
}

int FocusedRow(State& s) {
    int i = ListView_GetNextItem(s.list, -1, LVNI_FOCUSED);
    if (i < 0) i = ListView_GetNextItem(s.list, -1, LVNI_SELECTED);
    return i >= 0 && i < (int)s.rows.size() ? i : -1;
}

// Gewählte Einträge in ein neues temporäres Verzeichnis entpacken; liefert Verzeichnis und Namen
bool ExtractToTemp(State& s, const std::vector<int>& sel, std::wstring& dir, std::vector<std::wstring>& names) {
    if (sel.empty()) return false;
    std::vector<std::wstring> paths;
    for (int i : sel) {
        paths.push_back(s.entries[i].path);
        names.push_back(LeafOf(s.entries[i].path));
    }
    dir = NewArchiveTempDir(s.archive);
    std::wstring err;
    if (!ExtractArchiveEntries(Owner(s), s.archive, s.entries, paths, dir, err)) {
        RemoveArchiveTempDir(dir);
        MsgError(Owner(s), L"Entpacken aus „" + PathFileName(s.archive) + L"“ nicht möglich:\n" + err);
        return false;
    }
    return true;
}

void OpenTemp(State& s, int entry) {
    std::wstring dir;
    std::vector<std::wstring> names;
    if (!ExtractToTemp(s, {entry}, dir, names)) return;
    std::wstring file = PathCombine(dir, names[0]);
    ShellOpen(Owner(s), file, L"", dir);
}

// „Anzeigen“: temporäre Kopie in einem eigenen Anzeigefenster in der Größe eines Listenfensters, zentriert über dem
// QFiles-Fenster
void ViewTemp(State& s, int entry) {
    std::wstring dir;
    std::vector<std::wstring> names;
    if (!ExtractToTemp(s, {entry}, dir, names)) return;
    HWND pane = GetParent(s.hwnd);
    for (HWND w = pane; w; w = GetParent(w)) {
        wchar_t cls[64] = {};
        GetClassNameW(w, cls, 64);
        if (wcscmp(cls, L"QFilesPane") == 0) {
            pane = w;
            break;
        }
    }
    RECT pr{};
    GetWindowRect(pane ? pane : s.hwnd, &pr);
    HWND main = App::MainWindow();
    if (!main || !IsWindowVisible(main)) main = Owner(s);
    RECT mr{};
    GetWindowRect(main, &mr);
    int w = std::max<int>(pr.right - pr.left, DpiScale(s.hwnd, 320)), h = std::max<int>(pr.bottom - pr.top, DpiScale(s.hwnd, 240));
    int x = (mr.left + mr.right - w) / 2, y = (mr.top + mr.bottom - h) / 2;
    // im sichtbaren Bereich des Monitors halten
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST), &mi)) {
        x = std::clamp<int>(x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - w));
        y = std::clamp<int>(y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - h));
    }
    RECT r{x, y, x + w, y + h};
    OpenViewerWindowAt(PathCombine(dir, names[0]), r);
}

void Up(State& s) {
    if (s.search) {
        Navigate(s, s.cur);
        return;
    }
    if (s.cur.empty()) return;
    std::wstring from = s.cur;
    Navigate(s, ParentOf(s.cur), from);
}

void OpenRow(State& s, int row) {
    if (row < 0 || row >= (int)s.rows.size()) return;
    int e = s.rows[row];
    if (e < 0) {
        Up(s);
    } else if (s.entries[e].isDir) {
        Navigate(s, s.entries[e].path);
    } else {
        OpenTemp(s, e);
    }
}

void Search(State& s) {
    std::wstring p = s.pattern;
    bool ok = InputBox(Owner(s), L"Im Archiv suchen", L"Name (Platzhalter * und ? möglich; mehrere durch ; getrennt):", p);
    SetFocus(s.list);
    if (!ok) return;
    p = Trim(p);
    if (p.empty()) return;
    s.pattern = p;
    std::wstring pat;
    for (auto& part : Split(p, L';')) {
        std::wstring t = Trim(part);
        if (t.empty()) continue;
        if (t.find_first_of(L"*?") == std::wstring::npos) t = L"*" + t + L"*";
        pat += (pat.empty() ? L"" : L";") + t;
    }
    s.search = true;
    s.rows.clear();
    for (size_t i = 0; i < s.entries.size(); ++i)
        if (MatchAnyPattern(pat, LeafOf(s.entries[i].path))) s.rows.push_back((int)i);
    SortRows(s);
    Fill(s, L"");
}

void ExtractTo(State& s) {
    auto sel = SelectedEntries(s);
    if (sel.empty()) return;
    PaneContext ctx = App::ActivePaneContext();
    std::wstring start = ctx.dir.size() >= 2 && ctx.dir[1] == L':' ? ctx.dir : PathParent(s.archive);
    std::wstring dest = BrowseForFolder(Owner(s), L"Entpacken nach", start);
    if (dest.empty()) return;
    std::vector<std::wstring> paths, existing;
    for (int i : sel) {
        paths.push_back(s.entries[i].path);
        if (PathExists(PathCombine(dest, LeafOf(s.entries[i].path)))) existing.push_back(LeafOf(s.entries[i].path));
    }
    if (!existing.empty() &&
        !MsgConfirm(Owner(s), L"Im Ziel gibt es bereits: " + Join(existing, L", ") + L"\n\nÜberschreiben?"))
        return;
    std::wstring err;
    if (!ExtractArchiveEntries(Owner(s), s.archive, s.entries, paths, dest, err))
        MsgError(Owner(s), L"Entpacken nicht möglich:\n" + err);
    App::RefreshPanes();
}

void CopyToClipboard(State& s) {
    std::wstring dir;
    std::vector<std::wstring> names;
    if (!ExtractToTemp(s, SelectedEntries(s), dir, names)) return;
    std::vector<std::wstring> paths;
    for (auto& n : names) paths.push_back(PathCombine(dir, n));
    ClipboardSetFiles(Owner(s), paths, false);
}

void BeginDrag(State& s) {
    auto sel = SelectedEntries(s);
    if (sel.empty()) return;
    std::wstring dir;
    std::vector<std::wstring> names;
    if (!ExtractToTemp(s, sel, dir, names)) return;
    StartFileDrag(s.list, dir, names, DROPEFFECT_COPY);   // nur Kopie: das Archiv bleibt unverändert
}

void ContextMenu(State& s, POINT pt) {
    auto sel = SelectedEntries(s);
    int row = FocusedRow(s);
    HMENU m = CreatePopupMenu();
    bool oneFile = sel.size() == 1 && !s.entries[sel[0]].isDir;
    bool oneDir = sel.size() == 1 && s.entries[sel[0]].isDir;
    bool onParent = sel.empty() && row >= 0 && s.rows[row] < 0;
    if (oneFile && HasOwnViewer(s.entries[sel[0]].path)) AppendMenuW(m, MF_STRING, IDM_VIEW, L"&Anzeigen");
    if (oneFile) AppendMenuW(m, MF_STRING, IDM_OPENTEMP, L"Temporäre Kopie &öffnen\tEingabe");
    if (oneDir || onParent) AppendMenuW(m, MF_STRING, IDM_OPEN, L"Ö&ffnen\tEingabe");
    if (!sel.empty()) {
        AppendMenuW(m, MF_STRING, IDM_EXTRACT, L"&Entpacken nach…");
        AppendMenuW(m, MF_STRING, IDM_COPY, L"&Kopieren\tStrg+C");
    }
    if (GetMenuItemCount(m) > 0) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SEARCH, L"Im Archiv &suchen…\tStrg+F");
    if (s.search || !s.cur.empty()) AppendMenuW(m, MF_STRING, IDM_UP, s.search ? L"Suche &beenden\tEsc" : L"Ü&bergeordneter Ordner\tRücktaste");
    if (oneFile || oneDir) SetMenuDefaultItem(m, oneFile ? IDM_OPENTEMP : IDM_OPEN, FALSE);
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, s.hwnd, nullptr);
    DestroyMenu(m);
    // Danach den Fokus wieder in die Liste setzen – aber nicht, wenn inzwischen ein anderes Fenster aktiv ist (z. B. das
    // Anzeigefenster von „Anzeigen“): SetFocus würde das QFiles-Fenster wieder aktivieren und es davor schieben.
    struct Refocus {
        State& s;
        UINT id;
        ~Refocus() {
            if (id == IDM_VIEW || !IsWindowVisible(s.list)) return;
            if (GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT)) SetFocus(s.list);
        }
    } refocus{s, id};
    switch (id) {
    case IDM_OPENTEMP: OpenTemp(s, sel[0]); break;
    case IDM_VIEW: ViewTemp(s, sel[0]); break;
    case IDM_OPEN: OpenRow(s, row); break;
    case IDM_EXTRACT: ExtractTo(s); break;
    case IDM_COPY: CopyToClipboard(s); break;
    case IDM_SEARCH: Search(s); break;
    case IDM_UP: Up(s); break;
    default: break;
    }
}

void CreateListFont(State& s) {
    const Options& o = App::Opt();
    int h = -MulDiv(o.listFontSize, (int)GetWindowDpi(s.hwnd), 72);
    HFONT f = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, o.listFontName.c_str());
    SendMessageW(s.list, WM_SETFONT, (WPARAM)f, TRUE);
    if (s.listFont) DeleteObject(s.listFont);
    s.listFont = f;
}

LRESULT OnNotify(State& s, NMHDR* nm) {
    if (nm->hwndFrom != s.list) return 0;
    switch (nm->code) {
    case LVN_GETDISPINFOW: {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        int r = di->item.iItem;
        if (r < 0 || r >= (int)s.rows.size()) return 0;
        int e = s.rows[r];
        if (di->item.mask & LVIF_TEXT) {
            std::wstring t;
            if (e < 0) {
                if (di->item.iSubItem == ColName) t = L"..";
            } else {
                const ArchiveEntry& a = s.entries[e];
                const Options& o = App::Opt();
                switch (di->item.iSubItem) {
                case ColName: t = DisplayName(s, a); break;
                case ColType: t = TypeOf(a); break;
                case ColSize:
                    if (!a.isDir) t = o.sizeInBytes ? FormatSizeBytes(a.size) : FormatSize(a.size);
                    break;
                case ColPacked:
                    if (!a.isDir && a.hasPacked) t = o.sizeInBytes ? FormatSizeBytes(a.packed) : FormatSize(a.packed);
                    break;
                case ColDate:
                    if (a.hasDate) t = FormatFileTime(a.modified, o.dateWithSeconds);
                    break;
                case ColMethod: t = a.method; break;
                }
            }
            if (di->item.pszText && di->item.cchTextMax > 0) lstrcpynW(di->item.pszText, t.c_str(), di->item.cchTextMax);
        }
        if (di->item.mask & LVIF_IMAGE) di->item.iImage = e < 0 ? s.folderIcon : IconFor(s, s.entries[e]);
        return 0;
    }
    case LVN_ODFINDITEMW: {
        auto* fi = reinterpret_cast<NMLVFINDITEMW*>(nm);
        if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !fi->lvfi.psz) return -1;
        int n = (int)s.rows.size();
        if (n == 0) return -1;
        int start = fi->iStart < 0 || fi->iStart >= n ? 0 : fi->iStart;
        for (int k = 0; k < n; ++k) {
            int i = (start + k) % n;
            if (s.rows[i] >= 0 && StartsWithI(LeafOf(s.entries[s.rows[i]].path), fi->lvfi.psz)) return i;
        }
        return -1;
    }
    case LVN_COLUMNCLICK: {
        int col = reinterpret_cast<NMLISTVIEW*>(nm)->iSubItem;
        if (col == s.sortCol)
            s.sortAsc = !s.sortAsc;
        else {
            s.sortCol = col;
            s.sortAsc = true;
        }
        int f = FocusedRow(s);
        std::wstring focus = f >= 0 && s.rows[f] >= 0 ? s.entries[s.rows[f]].path : L"";
        SortRows(s);
        Fill(s, focus);
        return 0;
    }
    case NM_DBLCLK: {
        int r = reinterpret_cast<NMITEMACTIVATE*>(nm)->iItem;
        if (r >= 0) OpenRow(s, r);
        return 0;
    }
    case NM_RETURN:
        OpenRow(s, FocusedRow(s));
        if (IsWindowVisible(s.list)) SetFocus(s.list);
        return 0;
    case NM_RCLICK: {
        POINT pt;
        GetCursorPos(&pt);
        ContextMenu(s, pt);
        return 1;   // kein zusätzliches WM_CONTEXTMENU
    }
    case LVN_BEGINDRAG:
    case LVN_BEGINRDRAG: BeginDrag(s); return 0;
    case LVN_KEYDOWN: {
        auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (kd->wVKey == VK_BACK) Up(s);
        else if (ctrl && kd->wVKey == 'F') Search(s);
        else if (ctrl && kd->wVKey == 'C') CopyToClipboard(s);
        else if (ctrl && kd->wVKey == 'A') {
            ListView_SetItemState(s.list, -1, LVIS_SELECTED, LVIS_SELECTED);
        }
        else if (kd->wVKey == VK_ESCAPE) {
            if (s.search) Up(s);
            else if (HWND p = GetParent(s.hwnd)) SendMessageW(p, WM_KEYDOWN, VK_ESCAPE, 0);
        }
        // Kontextmenütaste und Umschalt+F10 kommen als WM_CONTEXTMENU (siehe Proc)
        return 0;
    }
    case NM_SETFOCUS:
    case NM_KILLFOCUS:
        s.focused = nm->code == NM_SETFOCUS;
        UpdateColors(s);
        return 0;
    case NM_CUSTOMDRAW: {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            // Gestreift (Option): im aktiven Zustand jede zweite Zeile im abgetönten Gelb
            if (App::Opt().striped && s.focused && (cd->nmcd.dwItemSpec % 2) == 1) {
                cd->clrTextBk = kToned;
                return CDRF_NEWFONT;
            }
        }
        return CDRF_DODEFAULT;
    }
    }
    return 0;
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* s = Get(hwnd);
    switch (msg) {
    case WM_NCCREATE:
        s = new State();
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        break;
    case WM_CREATE: {
        s->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA |
                                      LVS_SHAREIMAGELISTS,
                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)kListId, App::Instance(), nullptr);
        ListView_SetExtendedListViewStyle(s->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
                                                       (App::Opt().showGridLines ? LVS_EX_GRIDLINES : 0));
        SetWindowTheme(s->list, L"Explorer", nullptr);
        SHFILEINFOW sfi{};
        HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"x", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                                                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        if (il) ListView_SetImageList(s->list, il, LVSIL_SMALL);
        s->folderIcon = sfi.iIcon;
        for (int c = 0; c < ColCount; ++c) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
            col.fmt = (c == ColSize || c == ColPacked) ? LVCFMT_RIGHT : LVCFMT_LEFT;
            col.cx = DpiScale(hwnd, kWidths[c]);
            col.pszText = const_cast<wchar_t*>(kTitles[c]);
            col.iSubItem = c;
            ListView_InsertColumn(s->list, c, &col);
        }
        CreateListFont(*s);
        UpdateColors(*s);
        return 0;
    }
    case WM_SIZE: {
        int ih = InfoHeight(*s);
        MoveWindow(s->list, 0, ih, LOWORD(lp), std::max(0, (int)HIWORD(lp) - ih), TRUE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETFOCUS: SetFocus(s->list); return 0;
    case WM_CONTEXTMENU: {
        // Tastatur (Kontextmenütaste, Umschalt+F10): Menü am Fokuseintrag; Maus: an der Mausposition
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (lp == -1) {
            RECT rc{};
            int f = FocusedRow(*s);
            pt = {DpiScale(hwnd, 20), DpiScale(hwnd, 20)};
            if (f >= 0) {
                ListView_EnsureVisible(s->list, f, FALSE);
                if (ListView_GetItemRect(s->list, f, &rc, LVIR_LABEL)) pt = {rc.left + DpiScale(hwnd, 8), rc.bottom};
            }
            ClientToScreen(s->list, &pt);
        }
        ContextMenu(*s, pt);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.bottom = InfoHeight(*s);
        HBRUSH b = CreateSolidBrush(kToned);
        FillRect(dc, &rc, b);
        DeleteObject(b);
        HGDIOBJ old = SelectObject(dc, App::UIFont());
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(60, 50, 10));
        RECT tr{rc.left + DpiScale(hwnd, 6), rc.top, rc.right - DpiScale(hwnd, 6), rc.bottom};
        DrawTextW(dc, s->info.c_str(), (int)s->info.size(), &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_PATH_ELLIPSIS);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NOTIFY: return OnNotify(*s, reinterpret_cast<NMHDR*>(lp));
    case WM_DESTROY:
        if (s->listFont) DeleteObject(s->listFont);
        s->listFont = nullptr;
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete s;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

HWND CreateArchiveView(HWND parent, int id) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = App::Instance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, kClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                           App::Instance(), nullptr);
}

void ArchiveViewSetContent(HWND view, const std::wstring& archive, std::vector<ArchiveEntry> entries) {
    State* s = Get(view);
    if (!s) return;
    s->archive = archive;
    s->entries = std::move(entries);
    s->pattern.clear();
    Build(*s);
    CreateListFont(*s);
    UpdateColors(*s);
    Navigate(*s, L"");
}

void ArchiveViewClear(HWND view) {
    State* s = Get(view);
    if (!s) return;
    s->entries.clear();
    s->children.clear();
    s->rows.clear();
    s->cur.clear();
    s->search = false;
    ListView_SetItemCountEx(s->list, 0, 0);
}

HWND ArchiveViewList(HWND view) {
    State* s = Get(view);
    return s ? s->list : nullptr;
}

} // namespace qf
