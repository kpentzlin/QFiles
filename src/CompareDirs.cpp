// Modul C: Verzeichnisse vergleichen (modaler Optionen-Dialog, Vergleich im Arbeitsthread).
//
// Ergebnis: Markierungen für die Einträge der obersten Ebene beider Verzeichnisse
// (Schlüssel = ToLower(Name)) und die laut Markieroption zu markierenden Namen.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Util.h"
#include "CompareCommon.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace qf {

namespace {

using cmpdetail::CompareFileTimes;
using cmpdetail::CompareFileContents;
using cmpdetail::ContentResult;

const wchar_t* const kSection = L"Verzeichnisvergleich";

enum : int {
    IDC_LBL_LEFT = 1001,
    IDC_PATH_LEFT,
    IDC_LBL_RIGHT,
    IDC_PATH_RIGHT,
    IDC_GRP_CRIT,
    IDC_CRIT_SIZEDATE,
    IDC_TOLERANCE,
    IDC_IGNORE_HOUR,
    IDC_CRIT_SIZE,
    IDC_CRIT_CONTENT,
    IDC_RECURSIVE,
    IDC_GRP_MARK,
    IDC_MARK_NEWER,
    IDC_MARK_DIFF,
    IDC_MARK_EQUAL,
    IDC_MARK_NONE,
    IDC_AUTOCLOSE,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_RUN,
};

constexpr UINT WM_CMP_PROGRESS = WM_APP + 41;
constexpr UINT WM_CMP_DONE = WM_APP + 42;

enum class Criterion { SizeDate = 0, Size = 1, Content = 2 };
enum class MarkMode { NewerMissing = 0, Different = 1, Equal = 2, None = 3 };

struct Settings {
    Criterion criterion = Criterion::SizeDate;
    bool tolerance = true;
    bool ignoreHour = false;
    bool recursive = false;
    MarkMode markMode = MarkMode::NewerMissing;
    bool autoClose = false;
};

// Ergebnis eines Eintrags der obersten Ebene
struct TopEntry {
    std::wstring nameLeft;   // Originalschreibweise (leer, wenn links nicht vorhanden)
    std::wstring nameRight;
    CompareMark markLeft = CompareMark::None;
    CompareMark markRight = CompareMark::None;
};

struct Outcome {
    std::vector<TopEntry> entries;
    bool cancelled = false;
    std::wstring error;      // schwerer Fehler (Verzeichnis nicht lesbar)
    int readErrors = 0;      // Dateien, die nicht gelesen werden konnten
    unsigned long long filesCompared = 0;
};

// ---------------- Vergleichslogik (läuft im Arbeitsthread) ----------------

class Comparer {
public:
    Comparer(const Settings& s, const std::atomic<bool>& cancel, HWND notify, std::mutex& mx, std::wstring& current)
        : s_(s), cancel_(cancel), notify_(notify), mx_(mx), current_(current) {}

    Outcome Run(const std::wstring& left, const std::wstring& right) {
        Outcome out;
        std::vector<DirEntry> L, R;
        DWORD err = 0;
        if (!ListDirectory(left, L, &err)) {
            out.error = L"Das Verzeichnis kann nicht gelesen werden:\n" + left + L"\n\n" + LastErrorMessage(err);
            return out;
        }
        if (!ListDirectory(right, R, &err)) {
            out.error = L"Das Verzeichnis kann nicht gelesen werden:\n" + right + L"\n\n" + LastErrorMessage(err);
            return out;
        }
        std::unordered_map<std::wstring, size_t> rightIndex;
        for (size_t i = 0; i < R.size(); ++i) rightIndex.emplace(ToLower(R[i].name), i);
        std::vector<bool> rightUsed(R.size(), false);

        for (auto& l : L) {
            if (cancel_.load()) {
                out.cancelled = true;
                return out;
            }
            TopEntry te;
            te.nameLeft = l.name;
            auto it = rightIndex.find(ToLower(l.name));
            if (it == rightIndex.end()) {
                te.markLeft = CompareMark::Missing;
                out.entries.push_back(std::move(te));
                continue;
            }
            const DirEntry& r = R[it->second];
            rightUsed[it->second] = true;
            te.nameRight = r.name;
            std::wstring pl = PathCombine(left, l.name), pr = PathCombine(right, r.name);
            int res = CompareEntry(pl, l, pr, r, te.markLeft, te.markRight);
            if (res < 0) {
                out.cancelled = true;
                return out;
            }
            out.entries.push_back(std::move(te));
        }
        for (size_t i = 0; i < R.size(); ++i) {
            if (rightUsed[i]) continue;
            TopEntry te;
            te.nameRight = R[i].name;
            te.markRight = CompareMark::Missing;
            out.entries.push_back(std::move(te));
        }
        out.readErrors = readErrors_;
        out.filesCompared = filesCompared_;
        return out;
    }

private:
    void ReportProgress(const std::wstring& path) {
        ULONGLONG now = GetTickCount64();
        if (now - lastReport_ < 100) return;
        lastReport_ = now;
        {
            std::lock_guard<std::mutex> lock(mx_);
            current_ = path;
        }
        PostMessageW(notify_, WM_CMP_PROGRESS, 0, 0);
    }

    // Vergleicht zwei gleichnamige Einträge. Rückgabe: 0 = ok, -1 = abgebrochen.
    int CompareEntry(const std::wstring& pl, const DirEntry& l, const std::wstring& pr, const DirEntry& r,
                     CompareMark& ml, CompareMark& mr) {
        if (l.IsDir() != r.IsDir()) {
            ml = mr = CompareMark::Different;
            return 0;
        }
        if (l.IsDir()) {
            bool reparse = (l.attributes | r.attributes) & FILE_ATTRIBUTE_REPARSE_POINT;
            if (!s_.recursive || reparse) {
                ml = mr = CompareMark::Equal; // nur nach Vorhandensein
                return 0;
            }
            int d = SubtreeDiffers(pl, pr);
            if (d < 0) return -1;
            ml = mr = d ? CompareMark::Different : CompareMark::Equal;
            return 0;
        }
        return CompareFile(pl, l, pr, r, ml, mr);
    }

    void MarkByDate(const DirEntry& l, const DirEntry& r, CompareMark& ml, CompareMark& mr) {
        int t = CompareFileTimes(l.modified, r.modified, s_.tolerance, s_.ignoreHour);
        if (t > 0) {
            ml = CompareMark::Newer;
            mr = CompareMark::Older;
        } else if (t < 0) {
            ml = CompareMark::Older;
            mr = CompareMark::Newer;
        } else {
            ml = mr = CompareMark::Different;
        }
    }

    int CompareFile(const std::wstring& pl, const DirEntry& l, const std::wstring& pr, const DirEntry& r,
                    CompareMark& ml, CompareMark& mr) {
        ++filesCompared_;
        switch (s_.criterion) {
        case Criterion::Size:
            ml = mr = (l.size == r.size) ? CompareMark::Equal : CompareMark::Different;
            return 0;
        case Criterion::SizeDate: {
            int t = CompareFileTimes(l.modified, r.modified, s_.tolerance, s_.ignoreHour);
            if (t == 0 && l.size == r.size)
                ml = mr = CompareMark::Equal;
            else
                MarkByDate(l, r, ml, mr);
            return 0;
        }
        case Criterion::Content: {
            if (l.size != r.size) {
                MarkByDate(l, r, ml, mr);
                return 0;
            }
            ReportProgress(pl);
            ContentResult c = CompareFileContents(pl, pr, cancel_);
            if (c == ContentResult::Cancelled) return -1;
            if (c == ContentResult::Equal) {
                ml = mr = CompareMark::Equal;
            } else {
                if (c == ContentResult::Error) ++readErrors_;
                MarkByDate(l, r, ml, mr);
                if (c == ContentResult::Error) ml = mr = CompareMark::Different;
            }
            return 0;
        }
        }
        return 0;
    }

    // 1 = unterscheidet sich, 0 = gleich, -1 = abgebrochen. Bricht beim ersten Unterschied ab.
    int SubtreeDiffers(const std::wstring& dl, const std::wstring& dr) {
        if (cancel_.load()) return -1;
        ReportProgress(dl);
        std::vector<DirEntry> L, R;
        if (!ListDirectory(dl, L) || !ListDirectory(dr, R)) {
            ++readErrors_;
            return 1;
        }
        if (L.size() != R.size()) return 1;
        std::unordered_map<std::wstring, size_t> rightIndex;
        for (size_t i = 0; i < R.size(); ++i) rightIndex.emplace(ToLower(R[i].name), i);
        // erst die billigen Prüfungen (Vorhandensein, Typ, Größe/Datum), dann Inhalte und Unterverzeichnisse
        std::vector<std::pair<size_t, size_t>> deferred;
        for (size_t i = 0; i < L.size(); ++i) {
            auto it = rightIndex.find(ToLower(L[i].name));
            if (it == rightIndex.end()) return 1;
            const DirEntry& l = L[i];
            const DirEntry& r = R[it->second];
            if (l.IsDir() != r.IsDir()) return 1;
            if (l.IsDir()) {
                if (!((l.attributes | r.attributes) & FILE_ATTRIBUTE_REPARSE_POINT)) deferred.push_back({i, it->second});
                continue;
            }
            if (s_.criterion == Criterion::Content) {
                if (l.size != r.size) return 1;
                deferred.push_back({i, it->second});
                continue;
            }
            CompareMark ml, mr;
            CompareFile(PathCombine(dl, l.name), l, PathCombine(dr, r.name), r, ml, mr);
            if (ml != CompareMark::Equal) return 1;
        }
        for (auto& [li, ri] : deferred) {
            const DirEntry& l = L[li];
            const DirEntry& r = R[ri];
            std::wstring pl = PathCombine(dl, l.name), pr = PathCombine(dr, r.name);
            if (l.IsDir()) {
                int d = SubtreeDiffers(pl, pr);
                if (d != 0) return d;
            } else {
                CompareMark ml, mr;
                if (CompareFile(pl, l, pr, r, ml, mr) < 0) return -1;
                if (ml != CompareMark::Equal) return 1;
            }
        }
        return 0;
    }

    const Settings& s_;
    const std::atomic<bool>& cancel_;
    HWND notify_;
    std::mutex& mx_;
    std::wstring& current_;
    ULONGLONG lastReport_ = 0;
    int readErrors_ = 0;
    unsigned long long filesCompared_ = 0;
};

// Zusammenfassung für die Anzeige im Dialog
std::wstring Summarize(const Outcome& o) {
    int equal = 0, newerL = 0, newerR = 0, diff = 0, onlyL = 0, onlyR = 0;
    for (auto& e : o.entries) {
        if (e.markLeft == CompareMark::Missing) ++onlyL;
        else if (e.markRight == CompareMark::Missing) ++onlyR;
        else if (e.markLeft == CompareMark::Equal) ++equal;
        else if (e.markLeft == CompareMark::Newer) ++newerL;
        else if (e.markRight == CompareMark::Newer) ++newerR;
        else ++diff;
    }
    if (o.entries.empty()) return L"Beide Verzeichnisse sind leer.";
    std::vector<std::wstring> parts;
    auto add = [&](int n, const wchar_t* what) {
        if (n) parts.push_back(IntToStrGrouped((unsigned long long)n) + L" " + what);
    };
    add(equal, L"gleich");
    add(newerL, L"neuer links");
    add(newerR, L"neuer rechts");
    add(diff, L"unterschiedlich");
    add(onlyL, L"nur links");
    add(onlyR, L"nur rechts");
    std::wstring s;
    if (equal == (int)o.entries.size())
        s = L"Keine Unterschiede – alle " + IntToStrGrouped(o.entries.size()) + L" Einträge sind gleich.";
    else
        s = Join(parts, L" · ");
    if (o.readErrors)
        s += L"\n" + IntToStrGrouped((unsigned long long)o.readErrors) + L" Element(e) konnten nicht gelesen werden.";
    return s;
}

// ---------------- Dialog ----------------

class CompareDirsDialog : public DialogBase {
public:
    CompareDirsDialog(const std::wstring& left, const std::wstring& right) : left_(left), right_(right) {}
    ~CompareDirsDialog() override { StopWorker(); }

    bool HasResult() const { return hasResult_; }
    const Outcome& Result() const { return result_; }
    const Settings& GetSettings() const { return s_; }

protected:
    BOOL OnInit() override {
        LoadSettings();
        SetText(IDC_PATH_LEFT, left_);
        SetText(IDC_PATH_RIGHT, right_);
        CheckRadioButton(hwnd_, IDC_CRIT_SIZEDATE, IDC_CRIT_CONTENT,
                         s_.criterion == Criterion::Size      ? IDC_CRIT_SIZE
                         : s_.criterion == Criterion::Content ? IDC_CRIT_CONTENT
                                                              : IDC_CRIT_SIZEDATE);
        SetCheck(IDC_TOLERANCE, s_.tolerance);
        SetCheck(IDC_IGNORE_HOUR, s_.ignoreHour);
        SetCheck(IDC_RECURSIVE, s_.recursive);
        static const int markIds[] = {IDC_MARK_NEWER, IDC_MARK_DIFF, IDC_MARK_EQUAL, IDC_MARK_NONE};
        for (int id : markIds) SetCheck(id, false);
        SetCheck(markIds[(int)s_.markMode], true);
        SetCheck(IDC_AUTOCLOSE, s_.autoClose);
        Enable(IDOK, false);
        Show(IDC_PROGRESS, false);
        UpdateEnabling();
        SendMessageW(hwnd_, DM_SETDEFID, IDC_RUN, 0);
        SetFocus(Item(IDC_RUN));
        return FALSE;
    }

    BOOL OnCommand(int id, int code, HWND ctl) override {
        switch (id) {
        case IDC_CRIT_SIZEDATE:
        case IDC_CRIT_SIZE:
        case IDC_CRIT_CONTENT:
            if (code == BN_CLICKED) UpdateEnabling();
            return TRUE;
        case IDC_RUN:
            if (!running_) StartCompare();
            return TRUE;
        case IDOK:
            if (running_) return TRUE;
            if (hasResult_) {
                ReadSettings();
                SaveSettings();
                End(IDOK);
            }
            return TRUE;
        case IDCANCEL:
            if (running_) {
                cancel_ = true;
                SetText(IDC_STATUS, L"Wird abgebrochen …");
                return TRUE;
            }
            ReadSettings();
            SaveSettings();
            End(IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_CMP_PROGRESS) {
            if ((unsigned)wp == generation_ || wp == 0) {
                std::wstring cur;
                {
                    std::lock_guard<std::mutex> lock(mx_);
                    cur = current_;
                }
                // relativ zum linken Verzeichnis anzeigen
                if (cur.size() > left_.size() && StartsWithI(cur, left_)) {
                    size_t skip = left_.size();
                    while (skip < cur.size() && cur[skip] == L'\\') ++skip;
                    cur = cur.substr(skip);
                }
                if (running_ && !cancel_) SetText(IDC_STATUS, L"Vergleiche: " + cur);
            }
            return TRUE;
        }
        if (msg == WM_CMP_DONE) {
            if ((unsigned)wp != generation_) return TRUE;
            OnDone();
            return TRUE;
        }
        return FALSE;
    }

    void OnDestroy() override { StopWorker(); }

private:
    void UpdateEnabling() {
        bool sizeDate = IsChecked(IDC_CRIT_SIZEDATE) || IsChecked(IDC_CRIT_CONTENT);
        Enable(IDC_TOLERANCE, sizeDate && !running_);
        Enable(IDC_IGNORE_HOUR, sizeDate && !running_);
    }

    void EnableOptions(bool on) {
        static const int ids[] = {IDC_CRIT_SIZEDATE, IDC_CRIT_SIZE,  IDC_CRIT_CONTENT, IDC_RECURSIVE,
                                  IDC_MARK_NEWER,    IDC_MARK_DIFF,  IDC_MARK_EQUAL,   IDC_MARK_NONE,
                                  IDC_AUTOCLOSE,     IDC_RUN};
        for (int id : ids) Enable(id, on);
        UpdateEnabling();
    }

    void ReadSettings() {
        s_.criterion = IsChecked(IDC_CRIT_SIZE)      ? Criterion::Size
                       : IsChecked(IDC_CRIT_CONTENT) ? Criterion::Content
                                                     : Criterion::SizeDate;
        s_.tolerance = IsChecked(IDC_TOLERANCE);
        s_.ignoreHour = IsChecked(IDC_IGNORE_HOUR);
        s_.recursive = IsChecked(IDC_RECURSIVE);
        s_.markMode = IsChecked(IDC_MARK_DIFF)    ? MarkMode::Different
                      : IsChecked(IDC_MARK_EQUAL) ? MarkMode::Equal
                      : IsChecked(IDC_MARK_NONE)  ? MarkMode::None
                                                  : MarkMode::NewerMissing;
        s_.autoClose = IsChecked(IDC_AUTOCLOSE);
    }

    void LoadSettings() {
        Config& c = App::Cfg();
        s_.criterion = (Criterion)std::clamp(c.GetInt(kSection, L"Criterion", 0), 0, 2);
        s_.tolerance = c.GetBool(kSection, L"Tolerance", true);
        s_.ignoreHour = c.GetBool(kSection, L"IgnoreHour", false);
        s_.recursive = c.GetBool(kSection, L"Recursive", false);
        s_.markMode = (MarkMode)std::clamp(c.GetInt(kSection, L"MarkMode", 0), 0, 3);
        s_.autoClose = c.GetBool(kSection, L"AutoClose", false);
    }

    void SaveSettings() {
        Config& c = App::Cfg();
        c.SetInt(kSection, L"Criterion", (int)s_.criterion);
        c.SetBool(kSection, L"Tolerance", s_.tolerance);
        c.SetBool(kSection, L"IgnoreHour", s_.ignoreHour);
        c.SetBool(kSection, L"Recursive", s_.recursive);
        c.SetInt(kSection, L"MarkMode", (int)s_.markMode);
        c.SetBool(kSection, L"AutoClose", s_.autoClose);
    }

    void StartCompare() {
        StopWorker();
        ReadSettings();
        running_ = true;
        cancel_ = false;
        hasResult_ = false;
        Enable(IDOK, false);
        EnableOptions(false);
        SetText(IDCANCEL, L"Abbrechen");
        HWND prog = Item(IDC_PROGRESS);
        SetWindowLongPtrW(prog, GWL_STYLE, GetWindowLongPtrW(prog, GWL_STYLE) | PBS_MARQUEE);
        SendMessageW(prog, PBM_SETMARQUEE, TRUE, 30);
        Show(IDC_PROGRESS, true);
        SetText(IDC_STATUS, L"Vergleiche …");
        unsigned gen = ++generation_;
        Settings s = s_;
        std::wstring left = left_, right = right_;
        HWND h = hwnd_;
        worker_ = std::thread([this, s, left, right, h, gen]() {
            Comparer cmp(s, cancel_, h, mx_, current_);
            Outcome o = cmp.Run(left, right);
            if (cancel_.load()) o.cancelled = true;
            {
                std::lock_guard<std::mutex> lock(mx_);
                pending_ = std::move(o);
            }
            PostMessageW(h, WM_CMP_DONE, gen, 0);
        });
    }

    void StopWorker() {
        if (worker_.joinable()) {
            cancel_ = true;
            worker_.join();
        }
        running_ = false;
    }

    void OnDone() {
        if (worker_.joinable()) worker_.join();
        running_ = false;
        Outcome o;
        {
            std::lock_guard<std::mutex> lock(mx_);
            o = std::move(pending_);
        }
        SendMessageW(Item(IDC_PROGRESS), PBM_SETMARQUEE, FALSE, 0);
        Show(IDC_PROGRESS, false);
        EnableOptions(true);
        SetText(IDCANCEL, L"Abbrechen");
        if (!o.error.empty()) {
            SetText(IDC_STATUS, L"Fehler beim Lesen.");
            MsgError(hwnd_, o.error);
            return;
        }
        if (o.cancelled) {
            SetText(IDC_STATUS, L"Vergleich abgebrochen.");
            return;
        }
        result_ = std::move(o);
        hasResult_ = true;
        if (s_.autoClose) {
            SaveSettings();
            End(IDOK);
            return;
        }
        SetText(IDC_STATUS, Summarize(result_));
        Enable(IDOK, true);
        SendMessageW(hwnd_, DM_SETDEFID, IDOK, 0);
        SendMessageW(hwnd_, WM_NEXTDLGCTL, (WPARAM)Item(IDOK), TRUE);
    }

    std::wstring left_, right_;
    Settings s_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    bool running_ = false;
    unsigned generation_ = 0;
    std::mutex mx_;
    std::wstring current_;   // aktuell verglichener Pfad (geschützt durch mx_)
    Outcome pending_;        // Ergebnis des Threads (geschützt durch mx_)
    Outcome result_;
    bool hasResult_ = false;
};

} // namespace

bool CompareDirectories(HWND owner, const std::wstring& left, const std::wstring& right, CompareMarks& marksLeft,
                        CompareMarks& marksRight, std::vector<std::wstring>& selectLeft,
                        std::vector<std::wstring>& selectRight) {
    if (left.empty() || right.empty() || !DirExists(left) || !DirExists(right)) {
        MsgError(owner, L"Zum Vergleichen müssen in beiden Listen gültige Verzeichnisse angezeigt werden.");
        return false;
    }
    if (EqualsI(NormalizeDir(left), NormalizeDir(right))) {
        MsgInfo(owner, L"Beide Listen zeigen dasselbe Verzeichnis:\n" + left);
        return false;
    }

    DialogTemplate t(L"Verzeichnisse vergleichen", 300, 240);
    t.Label(IDC_LBL_LEFT, L"Links:", 7, 8, 40, 9);
    t.Label(IDC_PATH_LEFT, L"", 50, 8, 243, 9, SS_PATHELLIPSIS);
    t.Label(IDC_LBL_RIGHT, L"Rechts:", 7, 20, 40, 9);
    t.Label(IDC_PATH_RIGHT, L"", 50, 20, 243, 9, SS_PATHELLIPSIS);

    t.Group(IDC_GRP_CRIT, L"Vergleichskriterium", 7, 34, 286, 78);
    t.Radio(IDC_CRIT_SIZEDATE, L"Größe und Datum", 15, 46, 270, 10, true);
    t.Check(IDC_TOLERANCE, L"Zeittoleranz 2 Sekunden (FAT, Netzlaufwerke)", 27, 58, 258, 10);
    t.Check(IDC_IGNORE_HOUR, L"Zeitzonen-/Sommerzeitdifferenz von genau 1 Stunde ignorieren", 27, 70, 258, 10);
    t.Radio(IDC_CRIT_SIZE, L"Nur Größe", 15, 84, 270, 10);
    t.Radio(IDC_CRIT_CONTENT, L"Inhalt (byteweise vergleichen)", 15, 97, 270, 10);

    t.Check(IDC_RECURSIVE, L"Unterverzeichnisse einbeziehen (sonst nur nach Vorhandensein)", 7, 118, 286, 10, WS_GROUP);

    t.Group(IDC_GRP_MARK, L"Nach dem Vergleich markieren", 7, 133, 286, 40);
    t.Radio(IDC_MARK_NEWER, L"Neuere und fehlende Dateien", 15, 145, 135, 10, true);
    t.Radio(IDC_MARK_DIFF, L"Unterschiedliche", 155, 145, 130, 10);
    t.Radio(IDC_MARK_EQUAL, L"Gleiche", 15, 158, 135, 10);
    t.Radio(IDC_MARK_NONE, L"Keine", 155, 158, 130, 10);

    t.Check(IDC_AUTOCLOSE, L"Dialog nach dem Vergleich sofort schließen", 7, 179, 286, 10, WS_GROUP);
    t.Progress(IDC_PROGRESS, 7, 193, 286, 7);
    t.Label(IDC_STATUS, L"", 7, 202, 286, 17, SS_EDITCONTROL);

    t.DefButton(IDC_RUN, L"&Vergleichen", 129, 220, 52, 14);
    t.Button(IDOK, L"OK", 185, 220, 52, 14);
    t.Button(IDCANCEL, L"Abbrechen", 241, 220, 52, 14);

    CompareDirsDialog dlg(left, right);
    if (dlg.DoModal(owner, t) != IDOK || !dlg.HasResult()) return false;

    const Outcome& o = dlg.Result();
    const MarkMode mode = dlg.GetSettings().markMode;
    marksLeft.clear();
    marksRight.clear();
    selectLeft.clear();
    selectRight.clear();

    auto selected = [mode](CompareMark m) {
        switch (mode) {
        case MarkMode::NewerMissing: return m == CompareMark::Newer || m == CompareMark::Missing;
        case MarkMode::Different: return m != CompareMark::Equal && m != CompareMark::None;
        case MarkMode::Equal: return m == CompareMark::Equal;
        case MarkMode::None: return false;
        }
        return false;
    };
    for (auto& e : o.entries) {
        if (!e.nameLeft.empty()) {
            marksLeft[ToLower(e.nameLeft)] = e.markLeft;
            if (selected(e.markLeft)) selectLeft.push_back(e.nameLeft);
        }
        if (!e.nameRight.empty()) {
            marksRight[ToLower(e.nameRight)] = e.markRight;
            if (selected(e.markRight)) selectRight.push_back(e.nameRight);
        }
    }
    return true;
}

} // namespace qf
