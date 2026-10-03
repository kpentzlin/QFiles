#pragma once
// Lesezeichenliste: senkrecht zwischen Verzeichnisbaum und Dateilisten, immer sichtbar.

#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <string>
#include <vector>

#include "Settings.h"

namespace qf {

class BookmarkList {
public:
    struct Callbacks {
        std::function<void(const std::wstring& path, bool otherPane)> navigate;
        std::function<void()> changed;     // Liste geändert -> speichern
        std::function<void()> activated;
        std::function<std::wstring()> currentDir; // Verzeichnis der aktiven Liste
    };
    bool Create(HWND parent, int id, Callbacks cb);
    HWND Hwnd() const { return list_; }
    void Set(const std::vector<Bookmark>& b);
    const std::vector<Bookmark>& Get() const { return items_; }
    int FindPath(const std::wstring& path) const;   // -1 wenn nicht vorhanden
    void Append(const std::wstring& name, const std::wstring& path);
    void DeleteSelected();
    void RenameSelected();
    void MoveSelected(int delta);
    LRESULT OnNotify(NMHDR* nm);
    void Resize();

private:
    void Rebuild(int select);
    int Selected() const;
    void ContextMenu(POINT pt);
    static LRESULT CALLBACK Subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

    HWND list_ = nullptr;
    Callbacks cb_;
    std::vector<Bookmark> items_;
};

} // namespace qf
