#pragma once
// Modul B: HexView-Steuerelement (Fensterklasse "QFilesHexView") und Hex-Editor-Fenster.
//
// Das Steuerelement zeigt beliebig große Dateien (64-Bit-Offsets) seitenweise an: Die Datei wird nicht
// komplett geladen, sondern in 64-KB-Seiten gelesen und zwischengespeichert. Spalten: Offset
// (8 bzw. 16 Hexstellen bei Dateien > 4 GB), 16 Bytes hexadezimal, Zeichen (CP1252, nicht druckbare als '.').
//
// Schreibgeschützter Modus: reine Anzeige (z. B. Schnellansicht). Esc und Tab werden als WM_KEYDOWN an das
// Elternfenster weitergereicht.
// Bearbeitungsmodus: Überschreiben von Bytes (Hex- oder Zeichenspalte), Rückgängig, Speichern schreibt nur die
// geänderten Bytes in die bestehende Datei (in-place). Einfügen/Löschen von Bytes wird nicht unterstützt.
//
// Benachrichtigung an das Elternfenster: WM_COMMAND mit MAKEWPARAM(id, HVN_STATUS), lParam = HexView-HWND,
// sobald sich Cursor, Markierung, Datei oder Änderungsstatus geändert haben (z. B. für eine Statusleiste).

#include <windows.h>
#include <string>
#include <cstdint>

namespace qf {

constexpr WORD HVN_STATUS = 0x4801;   // Benachrichtigungscode (HIWORD(wParam) von WM_COMMAND)

struct HexViewStatus {
    bool open = false;          // Datei geöffnet
    bool readOnly = true;       // Steuerelement schreibgeschützt
    bool modified = false;      // ungespeicherte Änderungen vorhanden
    bool canUndo = false;
    uint64_t fileSize = 0;
    uint64_t caret = 0;         // Offset des Cursors
    bool hasByte = false;       // Cursor steht auf einem Byte (Datei nicht leer)
    uint8_t value = 0;          // Bytewert am Cursor
    bool hasSelection = false;
    uint64_t selStart = 0;      // Markierung (einschließlich)
    uint64_t selEnd = 0;
    bool charColumn = false;    // Cursor in der Zeichenspalte (sonst Hexspalte)
};

// Erzeugt das Steuerelement (WS_CHILD | WS_VISIBLE, Größe über MoveWindow setzen).
HWND CreateHexView(HWND parent, int id, bool readOnly);
// Öffnet eine Datei (schließt eine vorher geöffnete; ungespeicherte Änderungen gehen verloren).
// Rückgabe false bei Fehler; Fehlertext über HexViewLastError.
bool HexViewOpen(HWND hv, const std::wstring& path);
void HexViewClose(HWND hv);
// Liest die Datei neu ein (verwirft Änderungen), Cursorposition bleibt nach Möglichkeit erhalten.
bool HexViewReload(HWND hv);
std::wstring HexViewPath(HWND hv);
std::wstring HexViewLastError(HWND hv);
bool HexViewIsModified(HWND hv);
// Schreibt die geänderten Bytes in die Datei. Zeigt Fehlermeldungen selbst an. Rückgabe false bei Fehler.
bool HexViewSave(HWND hv);
bool HexViewCanUndo(HWND hv);
bool HexViewUndo(HWND hv);
// Kopiert die Markierung (bzw. das Byte am Cursor) – in der Hexspalte als Hex-Text, in der Zeichenspalte als Text.
void HexViewCopy(HWND hv);
void HexViewSelectAll(HWND hv);
// Gehe zu Offset (dezimal, 0x… oder …h hexadezimal, +/- relativ). Modal.
void HexViewGotoDialog(HWND hv);
// Suchen nach Hex-Bytes oder Text (ANSI/UTF-8/UTF-16LE). Modal; sucht sofort.
void HexViewFindDialog(HWND hv);
// Weitersuchen mit dem zuletzt gesuchten Muster (ohne Muster: Dialog).
bool HexViewFindNext(HWND hv, bool backward = false);
bool HexViewSetCaret(HWND hv, uint64_t offset);
HexViewStatus HexViewGetStatus(HWND hv);

// Hex-Editor-Fenster (nicht modal, beliebig viele) – OpenHexEditor ist in Modules.h deklariert.
// Schließt alle Hex-Editor-Fenster; fragt bei ungespeicherten Änderungen nach.
// Rückgabe false = vom Benutzer abgebrochen (mindestens ein Fenster bleibt offen).
bool CloseAllHexEditors();

} // namespace qf
