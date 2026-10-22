#pragma once
// Favoriten: als Favorit gekennzeichnete Dateien (keine Verzeichnisse, keine FTP/SFTP/WebDAV-Dateien).
//
// Die Liste enthält vollständige Pfade (mit Laufwerksbuchstaben bzw. UNC-Pfad) und steht im Abschnitt [Favoriten]
// der Einstellungsdatei. Angezeigt wird sie über den Ort „Favoriten“ (Lesezeichen „Favoriten“ ganz oben) in einer
// Liste wie „Mit Unterverzeichnissen“, jedoch mit dem vollständigen Verzeichnis statt des Unterverzeichnisses.
// Nicht mehr vorhandene Dateien entfallen beim Aufbau der Liste stillschweigend.

#include <windows.h>
#include <string>
#include <vector>

#include "Settings.h"

namespace qf {

// Hintergrund der Favoritenliste und des Lesezeichens „Favoriten“ (hellgelb)
constexpr COLORREF kFavoritesBack = RGB(255, 251, 216);
// Nachricht an das Hauptfenster: Favoriten geändert (wParam 1 = Listen mit den Favoriten neu einlesen)
constexpr UINT WM_QF_FAVORITES_CHANGED = WM_APP + 40;

const std::vector<std::wstring>& Favorites();
void LoadFavorites(const Config& c);
void SaveFavorites(Config& c);
bool IsFavorite(const std::wstring& path);
// Fügt Dateien hinzu bzw. entfernt sie; Rückgabe: Anzahl tatsächlich geänderter Einträge
int AddFavorites(const std::vector<std::wstring>& paths);
int RemoveFavorites(const std::vector<std::wstring>& paths, bool reloadPanes = true);
// Umbenannte bzw. verschobene Datei nachführen (keine Wirkung, wenn oldPath kein Favorit ist)
bool RenameFavorite(const std::wstring& oldPath, const std::wstring& newPath);
// Kann die Datei als Favorit gekennzeichnet werden? (vorhandene lokale Datei bzw. Datei auf einer Netzwerkfreigabe)
bool CanBeFavorite(const std::wstring& path);

} // namespace qf
