#pragma once
// Integrierter Texteditor (Modul A).
//
// OpenTextEditor ist zusätzlich in Modules.h deklariert (identische Deklaration).

#include <string>

namespace qf {

// Öffnet ein eigenes, nicht modales Editorfenster (beliebig viele).
// path existiert -> laden (Kodierung automatisch), existiert nicht -> neue Datei mit diesem Namen,
// leer -> "Unbenannt". Ist die Datei bereits in einem Editorfenster offen, wird dieses nach vorn geholt.
void OpenTextEditor(const std::wstring& path);

// Schließt alle Editorfenster. Bei geänderten Dokumenten wird nachgefragt
// (Speichern / Nicht speichern / Abbrechen). Rückgabe false = Benutzer hat abgebrochen
// (die übrigen Editorfenster bleiben dann geöffnet).
bool CloseAllTextEditors();

} // namespace qf
