#pragma once
// Verzeichnisbaum (links). Laufwerke als Wurzeln, Unterverzeichnisse werden beim Aufklappen gelesen.

#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "Network.h"

namespace qf {

class DirTree {
public:
    bool Create(HWND parent, int id, std::function<void(const std::wstring&)> onNavigate,
                std::function<void()> onActivate);
    HWND Hwnd() const { return tree_; }
    void Populate();                               // Laufwerke neu aufbauen
    void SelectPath(const std::wstring& path);     // aufklappen und markieren (ohne Navigation)
    void RefreshPath(const std::wstring& path);    // Unterverzeichnisse dieses Knotens neu lesen
    std::wstring SelectedPath() const;
    std::wstring DropDirAt(POINT screenPt) const;
    LRESULT OnNotify(NMHDR* nm);
    void ApplyOptions();

private:
    HTREEITEM AddItem(HTREEITEM parent, const std::wstring& text, const std::wstring& path, int icon, int openIcon,
                      bool hasChildren);
    void FillChildren(HTREEITEM item);
    void StartScan(HTREEITEM item);           // Netzwerk/Rechner: Kinder im Hintergrund suchen
    void CancelScans();                       // Esc
    void OnScanItem(LPARAM lp);
    void OnScanDone(LPARAM lp);
    HTREEITEM NetworkNode() const;
    std::wstring ItemPath(HTREEITEM item) const;
    HTREEITEM FindChild(HTREEITEM parent, const std::wstring& name) const;
    static LRESULT CALLBACK Subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

    HWND tree_ = nullptr;
    std::function<void(const std::wstring&)> onNavigate_;
    std::function<void()> onActivate_;
    bool suppress_ = false;
    int folderIcon_ = 0, folderOpenIcon_ = 0;
    struct Scan {
        std::shared_ptr<NetworkScan> scan;
        unsigned gen = 0;
        HTREEITEM placeholder = nullptr;
    };
    std::map<HTREEITEM, Scan> scans_;
    unsigned scanGen_ = 0;
};

} // namespace qf
