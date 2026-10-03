// Modul E – Verzeichnisliste drucken, als Textdatei (UTF-8 mit BOM) speichern oder in die Zwischenablage kopieren.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Util.h"
#include "ToolsCommon.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <string>
#include <vector>

namespace qf {

namespace {

struct ListOptions {
    bool recursive = false;
    bool colSize = true;
    bool colDate = true;
    bool colAttr = false;
    bool listDirs = true;
    bool dirsFirst = true;
    int sortMode = 0;               // 0 Name, 1 Erweiterung, 2 Größe, 3 Datum
    std::wstring pattern = L"*";
};

struct ListEntry {
    std::wstring name;
    bool isDir = false;
    uint64_t size = 0;
    FILETIME modified{};
    DWORD attr = 0;
};

struct DirBlock {
    std::wstring path;
    std::vector<ListEntry> entries;
    uint64_t bytes = 0;
    int files = 0;
    int dirs = 0;
};

// Eine Ausgabezeile (für Text und Druck gemeinsam)
struct OutLine {
    enum Kind { Text, Heading, Entry, Summary, Blank } kind = Text;
    std::wstring name;     // Text/Überschrift/Name
    std::wstring size, date, attr;
};

void SortEntries(std::vector<ListEntry>& v, const ListOptions& o) {
    std::stable_sort(v.begin(), v.end(), [&](const ListEntry& a, const ListEntry& b) {
        if (o.dirsFirst && a.isDir != b.isDir) return a.isDir;
        switch (o.sortMode) {
        case 1: {
            int c = CompareI(PathExtension(a.name), PathExtension(b.name));
            if (c != 0) return c < 0;
            break;
        }
        case 2:
            if (a.size != b.size) return a.size < b.size;
            break;
        case 3: {
            LONG c = CompareFileTime(&a.modified, &b.modified);
            if (c != 0) return c < 0;
            break;
        }
        default: break;
        }
        return CompareNatural(a.name, b.name) < 0;
    });
}

// Liest dir (und ggf. Unterverzeichnisse) ein. Rückgabe false bei Abbruch.
bool CollectDir(const std::wstring& dir, const ListOptions& o, toolsdetail::ProgressSink& sink,
                std::vector<DirBlock>& blocks) {
    if (sink.Cancelled()) return false;
    sink.SetText(dir);
    std::vector<DirEntry> raw;
    DirBlock block;
    block.path = dir;
    ListDirectory(dir, raw);
    std::vector<std::wstring> subdirs;
    for (const auto& e : raw) {
        if (e.IsDir()) {
            if (!(e.attributes & FILE_ATTRIBUTE_REPARSE_POINT)) subdirs.push_back(e.name);
            if (!o.listDirs) continue;
            block.entries.push_back({e.name, true, 0, e.modified, e.attributes});
            block.dirs++;
        } else {
            if (!o.pattern.empty() && !MatchAnyPattern(o.pattern, e.name)) continue;
            block.entries.push_back({e.name, false, e.size, e.modified, e.attributes});
            block.files++;
            block.bytes += e.size;
        }
    }
    SortEntries(block.entries, o);
    blocks.push_back(std::move(block));
    if (o.recursive) {
        std::sort(subdirs.begin(), subdirs.end(),
                  [](const std::wstring& a, const std::wstring& b) { return CompareNatural(a, b) < 0; });
        for (const auto& s : subdirs)
            if (!CollectDir(PathCombine(dir, s), o, sink, blocks)) return false;
    }
    return true;
}

std::wstring NowText() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return toolsdetail::FormatLocalSystemTime(st, false);
}

std::vector<OutLine> BuildLines(const std::wstring& root, const std::vector<DirBlock>& blocks, const ListOptions& o) {
    std::vector<OutLine> out;
    auto text = [&](OutLine::Kind k, const std::wstring& s) {
        OutLine l;
        l.kind = k;
        l.name = s;
        out.push_back(l);
    };
    text(OutLine::Heading, L"Verzeichnisliste: " + root);
    text(OutLine::Text, L"Stand: " + NowText());
    std::wstring info = L"Muster: " + (o.pattern.empty() ? std::wstring(L"*") : o.pattern);
    if (o.recursive) info += L" – mit Unterverzeichnissen";
    text(OutLine::Text, info);
    text(OutLine::Blank, L"");

    uint64_t totalBytes = 0;
    int totalFiles = 0, totalDirs = 0;
    for (const auto& b : blocks) {
        if (o.recursive) text(OutLine::Heading, b.path);
        for (const auto& e : b.entries) {
            OutLine l;
            l.kind = OutLine::Entry;
            l.name = e.isDir ? L"[" + e.name + L"]" : e.name;
            if (o.colSize) l.size = e.isDir ? L"<DIR>" : FormatSizeBytes(e.size);
            if (o.colDate) l.date = FormatFileTime(e.modified, false);
            if (o.colAttr) l.attr = FormatAttributes(e.attr);
            out.push_back(l);
        }
        text(OutLine::Summary, Format(L"%d Datei(en), %d Verzeichnis(se), ", b.files, b.dirs) + FormatSizeBytes(b.bytes) +
                                   L" Bytes");
        text(OutLine::Blank, L"");
        totalBytes += b.bytes;
        totalFiles += b.files;
        totalDirs += b.dirs;
    }
    if (o.recursive && blocks.size() > 1)
        text(OutLine::Heading, Format(L"Gesamt: %d Datei(en), %d Verzeichnis(se), ", totalFiles, totalDirs) +
                                   FormatSizeBytes(totalBytes) + L" Bytes (" + FormatSize(totalBytes) + L")");
    return out;
}

std::wstring PadRight(const std::wstring& s, size_t w) { return s.size() >= w ? s : s + std::wstring(w - s.size(), L' '); }
std::wstring PadLeft(const std::wstring& s, size_t w) { return s.size() >= w ? s : std::wstring(w - s.size(), L' ') + s; }

// Textform mit festen Spaltenbreiten (CRLF)
std::wstring LinesToText(const std::vector<OutLine>& lines) {
    size_t nameW = 10, sizeW = 0, dateW = 0;
    for (const auto& l : lines) {
        if (l.kind != OutLine::Entry) continue;
        nameW = std::max(nameW, l.name.size());
        sizeW = std::max(sizeW, l.size.size());
        dateW = std::max(dateW, l.date.size());
    }
    nameW = std::min<size_t>(nameW, 100);
    std::wstring r;
    for (const auto& l : lines) {
        switch (l.kind) {
        case OutLine::Entry: {
            std::wstring s = L"  " + PadRight(l.name, nameW);
            if (sizeW) s += L"  " + PadLeft(l.size, sizeW);
            if (dateW) s += L"  " + PadRight(l.date, dateW);
            if (!l.attr.empty()) s += L"  " + l.attr;
            while (!s.empty() && s.back() == L' ') s.pop_back();
            r += s;
            break;
        }
        case OutLine::Summary: r += L"  " + l.name; break;
        case OutLine::Heading:
            r += l.name + L"\r\n" + std::wstring(std::min<size_t>(l.name.size(), 100), L'-');
            break;
        default: r += l.name; break;
        }
        r += L"\r\n";
    }
    return r;
}

// ---------- Drucken ----------

bool PrintLines(HWND owner, const std::wstring& root, const std::vector<OutLine>& lines) {
    PRINTDLGEXW pd{};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = owner;
    pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION | PD_NOCURRENTPAGE | PD_USEDEVMODECOPIESANDCOLLATE;
    pd.nCopies = 1;
    pd.nStartPage = START_PAGE_GENERAL;
    HRESULT hr = PrintDlgExW(&pd);
    auto cleanup = [&]() {
        if (pd.hDC) DeleteDC(pd.hDC);
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
    };
    if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC) {
        cleanup();
        return false;
    }
    HDC dc = pd.hDC;
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    const int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY);
    const int pageW = GetDeviceCaps(dc, PHYSICALWIDTH), pageH = GetDeviceCaps(dc, PHYSICALHEIGHT);
    const int offX = GetDeviceCaps(dc, PHYSICALOFFSETX), offY = GetDeviceCaps(dc, PHYSICALOFFSETY);
    // Ränder 15 mm (bzw. bedruckbarer Bereich), Koordinaten relativ zum bedruckbaren Bereich
    const int margin = MulDiv(15, dpiX * 10, 254);
    const int marginY = MulDiv(15, dpiY * 10, 254);
    const int left = std::max(0, margin - offX);
    const int top = std::max(0, marginY - offY);
    const int right = std::min(GetDeviceCaps(dc, HORZRES), pageW - margin - offX);
    const int bottom = std::min(GetDeviceCaps(dc, VERTRES), pageH - marginY - offY);

    auto makeFont = [&](int pt, int weight, bool italic) {
        return CreateFontW(-MulDiv(pt, dpiY, 72), 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT fNormal = makeFont(9, FW_NORMAL, false);
    HFONT fBold = makeFont(9, FW_BOLD, false);
    HFONT fItalic = makeFont(9, FW_NORMAL, true);
    HFONT fSmall = makeFont(8, FW_NORMAL, false);
    HFONT oldFont = (HFONT)SelectObject(dc, fNormal);

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int lineH = tm.tmHeight + tm.tmExternalLeading;
    const int gap = MulDiv(4, dpiX * 10, 254);   // 4 mm Spaltenabstand

    // Spaltenbreiten aus den längsten Werten
    auto textW = [&](const std::wstring& s) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
        return (int)sz.cx;
    };
    int sizeW = 0, dateW = 0, attrW = 0;
    for (const auto& l : lines) {
        if (l.kind != OutLine::Entry) continue;
        if (!l.size.empty()) sizeW = std::max(sizeW, textW(l.size));
        if (!l.date.empty()) dateW = std::max(dateW, textW(l.date));
        if (!l.attr.empty()) attrW = std::max(attrW, textW(l.attr));
    }
    int xAttr = right - attrW;
    int xDate = (attrW ? xAttr - gap : right) - dateW;
    int xSize = (dateW ? xDate - gap : (attrW ? xAttr - gap : right)) - sizeW;
    int nameRight = (sizeW ? xSize : (dateW ? xDate : (attrW ? xAttr : right))) - gap;
    const int indent = MulDiv(3, dpiX * 10, 254);

    std::wstring docName = L"Verzeichnisliste – " + root;
    DOCINFOW di{};
    di.cbSize = sizeof(di);
    di.lpszDocName = docName.c_str();
    bool ok = StartDocW(dc, &di) > 0;
    const std::wstring stamp = NowText();
    int page = 0;
    int y = bottom;   // erzwingt neue Seite
    auto newPage = [&]() {
        if (page > 0) EndPage(dc);
        StartPage(dc);
        ++page;
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, fSmall);
        std::wstring pg = Format(L"Seite %d", page);
        RECT r{left, top, right, top + lineH};
        DrawTextW(dc, pg.c_str(), -1, &r, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        RECT r2{left, top, right - textW(pg) - gap, top + lineH};
        std::wstring head = root + L"   –   " + stamp;
        DrawTextW(dc, head.c_str(), -1, &r2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
        int ly = top + lineH + lineH / 4;
        HPEN pen = CreatePen(PS_SOLID, std::max(1, dpiY / 150), RGB(0, 0, 0));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, left, ly, nullptr);
        LineTo(dc, right, ly);
        SelectObject(dc, op);
        DeleteObject(pen);
        y = ly + lineH / 2;
    };

    if (ok) {
        for (size_t i = 0; i < lines.size(); ++i) {
            const OutLine& l = lines[i];
            if (y + lineH > bottom) {
                if (l.kind == OutLine::Blank) continue;   // Leerzeile nicht an den Seitenanfang
                newPage();
            }
            switch (l.kind) {
            case OutLine::Blank: break;
            case OutLine::Heading: {
                SelectObject(dc, fBold);
                RECT r{left, y, right, y + lineH};
                DrawTextW(dc, l.name.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
                break;
            }
            case OutLine::Summary: {
                SelectObject(dc, fItalic);
                RECT r{left + indent, y, right, y + lineH};
                DrawTextW(dc, l.name.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                break;
            }
            case OutLine::Entry: {
                SelectObject(dc, fNormal);
                RECT rn{left + indent, y, std::max(left + indent, nameRight), y + lineH};
                DrawTextW(dc, l.name.c_str(), -1, &rn, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                if (sizeW) {
                    RECT r{xSize, y, xSize + sizeW, y + lineH};
                    DrawTextW(dc, l.size.c_str(), -1, &r, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
                }
                if (dateW) {
                    RECT r{xDate, y, xDate + dateW, y + lineH};
                    DrawTextW(dc, l.date.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                }
                if (attrW) {
                    RECT r{xAttr, y, xAttr + attrW, y + lineH};
                    DrawTextW(dc, l.attr.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                }
                break;
            }
            default: {
                SelectObject(dc, fNormal);
                RECT r{left, y, right, y + lineH};
                DrawTextW(dc, l.name.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                break;
            }
            }
            y += lineH;
        }
        if (page == 0) newPage();
        EndPage(dc);
        ok = EndDoc(dc) > 0;
    }
    DWORD err = GetLastError();
    SelectObject(dc, oldFont);
    DeleteObject(fNormal);
    DeleteObject(fBold);
    DeleteObject(fItalic);
    DeleteObject(fSmall);
    SetCursor(oldCursor);
    cleanup();
    if (!ok) {
        MsgError(owner, L"Der Druck ist fehlgeschlagen:\n" + LastErrorMessage(err));
        return false;
    }
    LogOperation(Format(L"Verzeichnisliste gedruckt (%d Seite(n)): ", page) + root);
    return true;
}

// ---------- Dialog ----------

class PrintListDlg : public DialogBase {
public:
    static constexpr int kDirLabel = 101;
    static constexpr int kRecursive = 110;
    static constexpr int kColSize = 111;
    static constexpr int kColDate = 112;
    static constexpr int kColAttr = 113;
    static constexpr int kPattern = 114;
    static constexpr int kListDirs = 115;
    static constexpr int kSort = 116;
    static constexpr int kDirsFirst = 117;
    static constexpr int kPrint = 120;
    static constexpr int kSave = 121;
    static constexpr int kClipboard = 122;

    std::wstring dir;

protected:
    BOOL OnInit() override {
        SetText(kDirLabel, dir);
        ListOptions o;
        SetCheck(kRecursive, o.recursive);
        SetCheck(kColSize, o.colSize);
        SetCheck(kColDate, o.colDate);
        SetCheck(kColAttr, o.colAttr);
        SetCheck(kListDirs, o.listDirs);
        SetCheck(kDirsFirst, o.dirsFirst);
        SetText(kPattern, o.pattern);
        ComboAdd(kSort, L"Name");
        ComboAdd(kSort, L"Erweiterung");
        ComboAdd(kSort, L"Größe");
        ComboAdd(kSort, L"Datum");
        ComboSetSel(kSort, 0);
        return TRUE;
    }

    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == kPrint || id == kSave || id == kClipboard) {
            std::vector<OutLine> lines;
            if (!Build(lines)) return TRUE;
            bool done = false;
            if (id == kPrint) {
                done = PrintLines(hwnd_, dir, lines);
            } else if (id == kSave) {
                std::wstring name = LastPathElement(dir);
                for (auto& c : name)
                    if (c == L':' || c == L'\\') c = L'_';
                std::wstring path = SaveFileDialog(hwnd_, L"Verzeichnisliste speichern",
                                                   PathCombine(dir, name + L" - Verzeichnisliste.txt"),
                                                   L"Textdateien (UTF-8)|*.txt|Alle Dateien|*.*");
                if (path.empty()) return TRUE;
                if (PathExtension(path).empty()) path += L".txt";
                std::string bytes = "\xEF\xBB\xBF" + WideToUtf8(LinesToText(lines));
                if (WriteFileBytes(path, bytes.data(), bytes.size())) {
                    LogOperation(L"Verzeichnisliste gespeichert: " + path);
                    App::RefreshPanes();
                    done = true;
                } else {
                    MsgError(hwnd_, L"„" + path + L"“ konnte nicht geschrieben werden:\n" + LastErrorMessage());
                }
            } else {
                done = ClipboardSetText(hwnd_, LinesToText(lines));
                if (!done) MsgError(hwnd_, L"Die Zwischenablage ist nicht verfügbar.");
            }
            if (done) End(IDOK);
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }

private:
    bool Build(std::vector<OutLine>& lines) {
        ListOptions o;
        o.recursive = IsChecked(kRecursive);
        o.colSize = IsChecked(kColSize);
        o.colDate = IsChecked(kColDate);
        o.colAttr = IsChecked(kColAttr);
        o.listDirs = IsChecked(kListDirs);
        o.dirsFirst = IsChecked(kDirsFirst);
        o.sortMode = std::max(0, ComboSel(kSort));
        o.pattern = Trim(GetText(kPattern));
        if (o.pattern.empty()) o.pattern = L"*";
        std::vector<DirBlock> blocks;
        const std::wstring root = dir;
        bool ok = toolsdetail::RunWithProgress(hwnd_, L"Verzeichnisliste", L"Verzeichnisse werden gelesen …",
                                               [&](toolsdetail::ProgressSink& sink) { CollectDir(root, o, sink, blocks); });
        if (!ok) return false;
        lines = BuildLines(dir, blocks, o);
        return true;
    }
};

} // namespace

void PrintDirectoryListing(HWND owner, const std::wstring& dir) {
    if (dir.empty() || !DirExists(dir)) {
        MsgError(owner, L"Das Verzeichnis ist nicht verfügbar:\n" + dir);
        return;
    }
    DialogTemplate t(L"Verzeichnisliste drucken/speichern", 300, 196);
    t.Label(-1, L"Verzeichnis:", 7, 7, 50, 10);
    t.Label(PrintListDlg::kDirLabel, L"", 60, 7, 233, 10, SS_PATHELLIPSIS);
    t.Group(-1, L"Inhalt", 7, 21, 286, 72);
    t.Check(PrintListDlg::kRecursive, L"&Unterverzeichnisse einbeziehen", 15, 33, 250, 10);
    t.Check(PrintListDlg::kListDirs, L"&Verzeichnisse mit auflisten", 15, 46, 250, 10);
    t.Label(-1, L"Nur Dateien mit &Muster:", 15, 62, 90, 10);
    t.Edit(PrintListDlg::kPattern, 108, 60, 177, 13);
    t.Label(-1, L"(z. B. *.jpg;*.png – mehrere durch ; getrennt)", 108, 76, 177, 10);
    t.Group(-1, L"Spalten und Sortierung", 7, 98, 286, 64);
    t.Check(PrintListDlg::kColSize, L"&Größe", 15, 110, 60, 10);
    t.Check(PrintListDlg::kColDate, L"&Datum", 80, 110, 60, 10);
    t.Check(PrintListDlg::kColAttr, L"&Attribute", 145, 110, 70, 10);
    t.Label(-1, L"&Sortierung nach:", 15, 128, 70, 10);
    t.Combo(PrintListDlg::kSort, 88, 126, 90, 80);
    t.Check(PrintListDlg::kDirsFirst, L"Verzeichnisse &zuerst", 15, 145, 150, 10);
    t.Button(PrintListDlg::kPrint, L"&Drucken …", 7, 175, 55, 14);
    t.Button(PrintListDlg::kSave, L"Als &Textdatei speichern …", 66, 175, 100, 14);
    t.Button(PrintListDlg::kClipboard, L"In &Zwischenablage", 170, 175, 70, 14);
    t.Button(IDCANCEL, L"Schließen", 243, 175, 50, 14);
    PrintListDlg dlg;
    dlg.dir = dir;
    dlg.DoModal(owner, t);
}

} // namespace qf
