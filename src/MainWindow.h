#pragma once
// Hauptfenster von QFiles.
//
// Aufteilung (links nach rechts):
//   [Verzeichnisbaum] | [Lesezeichen] | [Liste 1 / Liste 3 (Split)] | [Liste 2 / Liste 4 (Split)]
// Bei zwei Dateilisten stehen diese immer senkrecht nebeneinander, der Verzeichnisbaum links.
// Darunter: Befehlszeile, Funktionstastenleiste, Statusleiste.

#include <windows.h>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "BookmarkList.h"
#include "DirTree.h"
#include "FilePane.h"

namespace qf {

class MainWindow : public IPaneHost {
public:
    bool Create(int nCmdShow);
    HWND Hwnd() const { return hwnd_; }
    // Tastenkürzel des Hauptfensters (mit Rücksicht auf Textfelder). true = verarbeitet.
    bool PreTranslate(MSG* msg);

    // Dienste für App
    void NavigateActive(const std::wstring& dir, const std::wstring& select);
    void NavigateToFile(const std::wstring& path);
    void RefreshAll();
    PaneContext ActiveContext() const;
    void SaveState();

    // IPaneHost
    void OnPaneActivated(FilePane* p) override;
    void OnPaneDirChanged(FilePane* p) override;
    void OnPaneFocusItemChanged(FilePane* p) override;
    void OnPaneSplitButton(FilePane* p) override;
    void OnPaneBookmarkButton(FilePane* p) override;
    void OnPaneOpenItem(FilePane* p) override;
    void OnPaneContextMenu(FilePane* p, POINT screenPt, bool onItems) override;
    void OnPaneFilesDropped(FilePane* p) override;
    bool IsPaneActive(const FilePane* p) const override;

private:
    enum class Splitter { None, Tree, Bookmarks, Columns, RowLeft, RowRight };
    struct FKeyCell {
        std::wstring key, label;
        int command;
        RECT rc;
    };

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK CmdEditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);
    void OnCommand(int id);
    void Layout();
    void Paint(HDC dc);
    Splitter HitSplitter(POINT pt) const;
    void BuildMenu();
    void UpdateMenu(HMENU menu);
    void BuildBookmarkMenu(HMENU menu);
    void BuildToolbar();
    void BuildAccelerators();
    void BuildFKeyCells();
    void UpdateTitle();
    void UpdateStatusBar();
    void UpdateCmdLabel();
    void LoadState();
    void ApplyPaneVisibility();
    void SetActive(int index, bool focus = true);
    void SetSplit(int column, bool on);
    void SetTwoPanes(bool on);
    void SetQuickView(bool on);
    void UpdateQuickView();
    int OtherIndex(int index) const;
    FilePane& Active() { return *panes_[active_]; }
    const FilePane& Active() const { return *panes_[active_]; }
    FilePane* Other();
    std::wstring OtherDir();
    bool PaneVisible(int i) const;
    void ReloadVisible();
    void AddBookmarkFor(FilePane& p);
    void RunCommandLine();
    void CopyOrMove(bool move);
    void DeleteSelection(bool permanent);
    void ShowShortcuts();
    void OpenSpecialFolder(int index);
    void OpenFileItem(FilePane& p);
    void ApplyOptionsAll();
    enum class FocusArea { Pane, Tree, Bookmarks, CommandLine, Other };
    FocusArea CurrentFocusArea() const;

    HWND hwnd_ = nullptr;
    HWND toolbar_ = nullptr;
    HWND status_ = nullptr;
    HWND cmdLabel_ = nullptr;
    HWND cmdCombo_ = nullptr;
    HWND cmdEdit_ = nullptr;
    HMENU menu_ = nullptr;
    HMENU bookmarkMenu_ = nullptr;
    HACCEL accel_ = nullptr;
    DirTree tree_;
    BookmarkList bookmarks_;
    std::array<std::unique_ptr<FilePane>, 4> panes_;

    int active_ = 0;
    bool twoPanes_ = true;
    bool split_[2] = {false, false};
    double colRatio_ = 0.5;
    double rowRatio_[2] = {0.5, 0.5};
    int treeWidth_ = 220;      // 96-DPI-Pixel
    int bookmarkWidth_ = 150;
    bool showToolbar_ = true, showFKeys_ = true, showCmdLine_ = true, showStatus_ = true;
    bool quickView_ = false;
    int quickViewPane_ = -1;

    // Layout-Ergebnisse
    RECT rcContent_{}, rcTreeCap_{}, rcBookCap_{}, rcFKeys_{}, rcCmd_{};
    RECT rcSplitTree_{}, rcSplitBook_{}, rcSplitCol_{}, rcSplitRow_[2]{};
    RECT rcColumns_[2]{};
    Splitter dragging_ = Splitter::None;
    int dragOffset_ = 0;
    std::vector<FKeyCell> fkeys_;
    int fkeyMode_ = 0;          // 0 normal, 1 Strg, 2 Strg+Umschalt
    int fkeyPressed_ = -1;
    std::vector<std::wstring> cmdHistory_;
    std::vector<FunctionKey> fkeyDefs_;
    std::vector<std::pair<int, std::wstring>> toolTips_;  // Befehl -> Tooltip der Werkzeugleiste
};

} // namespace qf
