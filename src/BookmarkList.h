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
    // Sortieren: „Netzwerk“, Standardverzeichnisse, Cloud, FTP/SFTP/WebDAV, Netzwerkpfade, lokal – je alphabetisch
    void SortByCategory();
    LRESULT OnNotify(NMHDR* nm);
    void Resize();
    // Schrift aus den Optionen (Lesezeichen-Schrift; leer = Oberflächenschrift), auch nach DPI-Wechsel
    void ApplyFont();
    // Eintrag „Favoriten“ ganz oben (dunkelroter Asterisk, hellgelb), nur wenn count > 0
    void SetFavoritesCount(size_t count);

private:
    void Rebuild(int select);
    int Selected() const;          // Index in items_ (-1: nichts oder „Favoriten“)
    int SelectedRow() const;       // Zeile der ListView
    // Zeilen: Index in items_ bzw. kRowFavorites / kRowLinux (automatische Einträge)
    static constexpr int kRowFavorites = -1, kRowLinux = -2, kRowNone = -100;
    int ItemOfRow(int row) const { return row >= 0 && row < (int)rows_.size() ? rows_[row] : kRowNone; }
    int RowOfItem(int item) const;
    bool IsFavRow(int row) const { return ItemOfRow(row) == kRowFavorites; }
    bool IsLinuxRow(int row) const { return ItemOfRow(row) == kRowLinux; }
    bool IsSpecialRow(int row) const { int k = ItemOfRow(row); return k == kRowFavorites || k == kRowLinux; }
    std::wstring RowPath(int row) const;   // Ziel einer Zeile (Lesezeichen, „Favoriten“, „Linux“), "" = keins
    void DrawFavoritesRow(NMLVCUSTOMDRAW* cd);
    int IconColumnWidth() const;
    int IconColumnItemAt(POINT screenPt) const;
    void ContextMenu(POINT pt);
    static LRESULT CALLBACK Subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

    HWND list_ = nullptr;
    HFONT font_ = nullptr;
    Callbacks cb_;
    std::vector<Bookmark> items_;
    std::vector<int> icons_;   // Symbol je Eintrag (für die Symbolspalte)
    std::vector<int> kinds_;   // Art je Eintrag (lokal, Netzwerk, FTP/SFTP, Standardverzeichnis, WebDAV, Cloud) – Farbe/Sortierung
    bool favRow_ = false;      // Eintrag „Favoriten“ sichtbar
    std::vector<int> rows_;    // Zeile -> Index in items_ bzw. kRowFavorites/kRowLinux
    std::vector<std::wstring> wsl_;   // Linux-Distributionen (WSL); nicht leer -> Eintrag „Linux“ unter „Netzwerk“
    int linuxIcon_ = -1;
    size_t favCount_ = 0;      // Anzahl der Favoriten (Tooltip)
};

} // namespace qf
