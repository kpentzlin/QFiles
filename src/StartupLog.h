#pragma once
// Startzeiten: Zeitpunkte der Startphasen (ms seit dem Erzeugen des Prozesses), geschrieben nach
// QFiles-Startzeiten.txt neben der Einstellungsdatei – zum Auffinden langsamer Schritte beim Programmstart.

#include <string>

namespace qf {

void StartupMark(const std::wstring& phase);
// Einmal schreiben (weitere Aufrufe ohne Wirkung)
void StartupLogWrite(const std::wstring& path);

} // namespace qf
