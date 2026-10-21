// Favorites.cpp – Liste der Favoriten (siehe Favorites.h)

#include "Favorites.h"
#include "App.h"
#include "Location.h"
#include "Util.h"

#include <algorithm>

namespace qf {

namespace {

std::vector<std::wstring> g_favorites;
constexpr wchar_t kSection[] = L"Favoriten";

int Find(const std::wstring& path) {
    for (size_t i = 0; i < g_favorites.size(); ++i)
        if (EqualsI(g_favorites[i], path)) return (int)i;
    return -1;
}

// Speichern und das Hauptfenster benachrichtigen (Lesezeichen „Favoriten“, Listen mit den Favoriten)
void Changed(bool reloadPanes) {
    SaveFavorites(App::Cfg());
    App::Cfg().Save();
    if (HWND w = App::MainWindow()) PostMessageW(w, WM_QF_FAVORITES_CHANGED, reloadPanes ? 1 : 0, 0);
}

} // namespace

const std::vector<std::wstring>& Favorites() { return g_favorites; }

void LoadFavorites(const Config& c) {
    g_favorites.clear();
    for (auto& p : c.GetList(kSection, L"Datei"))
        if (!p.empty() && Find(p) < 0) g_favorites.push_back(p);
}

void SaveFavorites(Config& c) {
    c.ClearSection(kSection);
    c.SetList(kSection, L"Datei", g_favorites);
}

bool IsFavorite(const std::wstring& path) { return Find(path) >= 0; }

bool CanBeFavorite(const std::wstring& path) {
    if (path.empty() || IsVirtualLocation(path) || !IsAbsolutePath(path)) return false;
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int AddFavorites(const std::vector<std::wstring>& paths) {
    int n = 0;
    for (auto& p : paths)
        if (CanBeFavorite(p) && Find(p) < 0) {
            g_favorites.push_back(p);
            ++n;
        }
    if (n) Changed(true);
    return n;
}

int RemoveFavorites(const std::vector<std::wstring>& paths, bool reloadPanes) {
    int n = 0;
    for (auto& p : paths) {
        int i = Find(p);
        if (i >= 0) {
            g_favorites.erase(g_favorites.begin() + i);
            ++n;
        }
    }
    if (n) Changed(reloadPanes);
    return n;
}

bool RenameFavorite(const std::wstring& oldPath, const std::wstring& newPath) {
    int i = Find(oldPath);
    if (i < 0 || oldPath == newPath) return false;
    int j = Find(newPath);
    if (j >= 0 && j != i) g_favorites.erase(g_favorites.begin() + i);   // Ziel ist schon Favorit
    else g_favorites[i] = newPath;
    Changed(true);
    return true;
}

} // namespace qf
