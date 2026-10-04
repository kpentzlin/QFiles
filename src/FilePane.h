#pragma once
// Ein Dateilistenfenster ("Liste 1–4").
//
// Aufbau (von oben nach unten):
//   Kopfzeile:  [Laufwerksymbole …]                      [Split bzw. Split aufheben] [Lesezeichen +]
//   Pfadzeile:  [Verzeichnis-Textfeld mit Verlauf ▼] [◄] [►] [▲] [Verzeichnis wählen]
//   Liste:      ListView (virtuell) – oder die Schnellansicht (Dateianzeige) im "jeweils anderen Fenster"
//   Statuszeile

#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "Modules.h"
#include "Util.h"

namespace qf {

class FilePane;

class IPaneHost {
public:
    virtual ~IPaneHost() = default;
    virtual void OnPaneActivated(FilePane* p) = 0;
    virtual void OnPaneDirChanged(FilePane* p) = 0;
    virtual void OnPaneFocusItemChanged(FilePane* p) = 0;
    virtual void OnPaneSplitButton(FilePane* p) = 0;
    virtual void OnPaneBookmarkButton(FilePane* p) = 0;
    virtual void OnPaneOpenItem(FilePane* p) = 0;                       // Eingabe/Doppelklick auf Datei
    virtual void OnPaneContextMenu(FilePane* p, POINT screenPt, bool onItems) = 0;
    virtual void OnPaneFilesDropped(FilePane* p) = 0;                    // nach Drag & Drop: andere Listen aktualisieren
    virtual bool IsPaneActive(const FilePane* p) const = 0;
};

enum class PaneView { Details = 0, List = 1, Icons = 2, Thumbnails = 3 };
enum class SortKey { Name = 0, Ext = 1, Size = 2, Date = 3, Attr = 4, Created = 5 };
enum class SplitButton { Split, Unsplit };

class FilePane {
public:
    FilePane();
    ~FilePane();

    bool Create(HWND parent, int index, IPaneHost* host);
    HWND Hwnd() const { return hwnd_; }
    HWND ListHwnd() const { return list_; }
    HWND PathEditHwnd() const { return pathEdit_; }
    int Index() const { return index_; }
    void SetBounds(const RECT& rc);
    void Show(bool show);
    bool IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

    // Navigation
    bool Navigate(const std::wstring& dir, const std::wstring& focusName = L"", bool addHistory = true,
                  bool showErrors = true);
    void Reload();                         // neu einlesen, Markierung/Fokus erhalten
    void GoUp();
    void GoRoot();
    void GoBack();
    void GoForward();
    bool CanGoBack() const { return histPos_ > 0; }
    bool CanGoForward() const { return histPos_ + 1 < (int)hist_.size(); }
    const std::wstring& Dir() const { return dir_; }

    // Auswahl
    std::vector<std::wstring> SelectedNames() const;       // ohne ".."
    std::vector<std::wstring> SelectedPaths() const;
    std::wstring FocusedName() const;                       // "" bei ".." oder nichts
    std::wstring FocusedPath() const;
    bool FocusedIsDir() const;
    bool FocusedIsParent() const;
    // Markierte, sonst Fokus (ohne "..")
    std::vector<std::wstring> SelectedOrFocusedNames() const;
    std::vector<std::wstring> SelectedOrFocusedPaths() const;
    void SelectNames(const std::vector<std::wstring>& names, bool focusFirst = true);
    void FocusName(const std::wstring& name);
    void SelectAll();
    void SelectNone();
    void InvertSelection();
    void SelectPattern(const std::wstring& patterns, bool select);
    void SelectSameExtension();
    PaneContext Context() const;

    // Darstellung
    void SetView(PaneView v);
    PaneView View() const { return view_; }
    void SetSort(SortKey k, bool descending);
    SortKey Sort() const { return sortKey_; }
    bool SortDescending() const { return sortDesc_; }
    void SetFilter(const PaneFilter& f);
    const PaneFilter& Filter() const { return filter_; }
    void SetMarks(const CompareMarks& marks);
    void ClearMarks();
    bool HasMarks() const { return !marks_.empty(); }
    void SetSplitButton(SplitButton b);
    void SetActiveLook(bool active);       // aktive Liste hervorheben
    void ApplyOptions();                   // Schrift, Gitternetz, versteckte Dateien …
    void UpdateDrives();
    void OnDpiChanged();

    // Aktionen
    void BeginRename();
    void ComputeDirSizes(bool selectedOnly);
    void ToggleSelectFocused(bool moveDown);
    void FocusList();
    void FocusPath();

    // Schnellansicht ("Dateianzeige im jeweils anderen Fenster")
    void SetQuickView(bool on);
    bool QuickViewOn() const { return quickView_; }
    void QuickViewLoad(const std::wstring& path);

    // Einstellungen
    void LoadState(const Config& c, const std::wstring& section);
    void SaveState(Config& c, const std::wstring& section) const;

    // Für DragDrop: Zielverzeichnis an einem Bildschirmpunkt (Ordner unter dem Mauszeiger oder aktuelles Verzeichnis)
    std::wstring DropTargetDirAt(POINT screenPt) const;

private:
    struct Item {
        DirEntry e;
        bool isParent = false;
        int icon = -1;                 // Systemsymbol-Index
        int thumb = -1;                // Index in der Miniatur-Bildliste, -1 = nicht angefordert, -2 = angefordert
        uint64_t dirSize = UINT64_MAX; // berechnete Verzeichnisgröße
        CompareMark mark = CompareMark::None;
    };
    enum class HitArea { None, Drive, Split, Bookmark, Back, Forward, Up, Browse, Active };
    struct HitRect {
        HitArea area;
        int drive;          // Laufwerksindex bei Drive
        RECT rc;
    };
    struct DriveInfo {
        std::wstring root;  // "C:\"
        int icon;
        UINT type;
    };

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
    static LRESULT CALLBACK PathEditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnNotify(NMHDR* nm);
    LRESULT OnCustomDraw(NMLVCUSTOMDRAW* cd);
    void OnGetDispInfo(NMLVDISPINFOW* di);
    void Layout();
    void Paint(HDC dc);
    void PaintHeader(HDC dc, const RECT& rc);
    void PaintNavButtons(HDC dc);
    void PaintStatus(HDC dc, const RECT& rc);
    void DrawGlyph(HDC dc, HitArea area, const RECT& rc, bool hot);
    void BuildHitRects();
    const HitRect* HitTest(POINT pt) const;
    void OnClickArea(const HitRect& h);
    void UpdateTooltips();

    bool ReadDirectory(const std::wstring& dir, DWORD* err);
    void SortItems();
    void FillList(const std::wstring& focusName, const std::vector<std::wstring>& selected, int topIndex);
    int FindItem(const std::wstring& name) const;
    int ItemIconIndex(Item& it);
    std::wstring ItemFullPath(const Item& it) const;
    void UpdateStatus();
    void UpdatePathCombo();
    void AddHistory(const std::wstring& dir);
    void CreateListFont();
    void SetupColumns();
    void ApplyViewStyle();
    void OpenFocused();
    void StartWatch();
    void StopWatch();
    void RequestThumb(int index);
    void StopThumbWorker();
    void OnThumbReady(WPARAM wp, LPARAM lp);
    int AddThumbnail(HBITMAP bmp);
    int ToPx(int v) const { return DpiScale(hwnd_, v); }

    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND pathCombo_ = nullptr;
    HWND pathEdit_ = nullptr;
    HWND viewer_ = nullptr;
    HWND tooltip_ = nullptr;
    IPaneHost* host_ = nullptr;
    int index_ = 0;

    std::wstring dir_;
    std::vector<Item> items_;
    std::vector<DriveInfo> drives_;
    std::vector<HitRect> hits_;
    const HitRect* hot_ = nullptr;
    bool tracking_ = false;
    bool activeLook_ = false;
    SplitButton splitButton_ = SplitButton::Split;
    bool quickView_ = false;

    PaneView view_ = PaneView::Details;
    SortKey sortKey_ = SortKey::Name;
    bool sortDesc_ = false;
    PaneFilter filter_;
    CompareMarks marks_;
    std::vector<int> colWidths_;   // in 96-DPI-Pixeln
    std::vector<int> colIds_;      // Spaltenindex -> Spalten-ID

    std::vector<std::wstring> hist_;     // Zurück/Vor-Liste
    int histPos_ = -1;
    std::vector<std::wstring> recent_;   // Verlauf für die Combobox (zuletzt besucht zuerst)
    std::vector<std::pair<std::wstring, std::wstring>> lastDirPerDrive_;

    HFONT listFont_ = nullptr;
    HIMAGELIST thumbList_ = nullptr;
    int thumbSize_ = 0;

    RECT rcHeader_{}, rcPath_{}, rcList_{}, rcStatus_{};
    std::wstring statusText_;
    uint64_t freeBytes_ = 0;
    bool freeKnown_ = false;

    // Verzeichnisüberwachung
    std::thread watchThread_;
    HANDLE watchStop_ = nullptr;

    // Miniaturen
    struct ThumbRequest {
        unsigned gen;
        int index;
        std::wstring path;
    };
    std::thread thumbThread_;
    std::mutex thumbMutex_;
    std::condition_variable thumbCv_;
    std::deque<ThumbRequest> thumbQueue_;
    bool thumbStop_ = false;
    std::atomic<unsigned> generation_{1};

    // Verzeichnisgrößen
    std::shared_ptr<std::atomic<bool>> sizeCancel_;
};

} // namespace qf
