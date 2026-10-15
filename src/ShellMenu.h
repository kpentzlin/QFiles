#pragma once
// Explorer-Kontextmenü, Eigenschaften-Dialog und Drag & Drop für Dateien.

#include <windows.h>
#include <objidl.h>
#include <functional>
#include <string>
#include <vector>

namespace qf {

// Zeigt das Explorer-Kontextmenü für names im Verzeichnis dir (names leer = Hintergrundmenü des Verzeichnisses).
// extra: eigene Einträge (Befehls-IDs >= 40000), die oben eingefügt werden (Menü wird nicht freigegeben).
// Rückgabe: gewählte eigene Befehls-ID oder 0. *renameRequested = true, wenn "Umbenennen" gewählt wurde.
int ShowShellContextMenu(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names, POINT screenPt,
                         HMENU extra, bool* renameRequested);
// Sucht im Hintergrund die nach Optionen „KontextmenueAusschluss“ auszuschließenden Kontextmenü-Erweiterungen
// (einmal beim Programmstart aufrufen; das Kontextmenü wartet beim ersten Aufruf ggf. auf das Ergebnis).
void PrefetchShellExtensionExclusion();
// Muss aus der Fensterprozedur des owner-Fensters aufgerufen werden (Untermenüs wie "Senden an", "Neu").
bool HandleShellMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// Eigenschaften (Explorer-Dialog) für ein oder mehrere Elemente.
void ShowShellProperties(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names);

// Datenobjekt der Shell für Elemente (für Drag & Drop). Aufrufer gibt frei (Release).
IDataObject* CreateShellDataObject(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names);

// ---- Drag & Drop ----
// Startet das Ziehen der Elemente (Quelle). Rückgabe: ausgeführter Effekt (DROPEFFECT_*).
DWORD StartFileDrag(HWND hwnd, const std::wstring& dir, const std::vector<std::wstring>& names,
                    DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);

// Registriert hwnd als Ablageziel für Dateien. resolver liefert das Zielverzeichnis für einen Bildschirmpunkt
// (leer = Ablage nicht möglich; auch FTP/SFTP/WebDAV-Adressen). done wird nach erfolgreicher Ablage aufgerufen.
// listPane: Regel der Dateilisten – Ziehen verschiebt, mit Umschalt oder Strg wird kopiert. Sonst wie im Explorer
// (gleiches Laufwerk verschieben, anderes kopieren; Strg = kopieren, Umschalt = verschieben).
void RegisterFileDropTarget(HWND hwnd, std::function<std::wstring(POINT)> resolver, std::function<void()> done,
                            bool listPane = false);

// Ziel für die Dateilisten: dir = Zielverzeichnis; alt (nicht leer) = Zweifelsfall – der Mauszeiger steht im Leerraum
// hinter dem Namen eines Unterverzeichnisses (alt). Beim Ablegen wird dann gefragt, welches gemeint ist.
struct DropTargetInfo {
    std::wstring dir;
    std::wstring alt;
};
// Wie oben, mit Zweifelsfall-Abfrage. leave wird beim Verlassen und nach dem Ablegen aufgerufen (Hervorhebung zurück).
void RegisterFileDropTargetEx(HWND hwnd, std::function<DropTargetInfo(POINT)> resolver, std::function<void()> done,
                              std::function<void()> leave, bool listPane);
void RevokeFileDropTarget(HWND hwnd);

} // namespace qf
