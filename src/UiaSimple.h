#pragma once
// UiaSimple.cpp – vereinfachte Bedienungshilfen-Darstellung (UI Automation / MSAA) für große Steuerelemente.
// Programme, die die Oberfläche über UI Automation auslesen (Sprachausgabe, KI-Bildschirmsteuerungen wie
// „codex-computer-use“), lesen sonst bei jedem Klick alle Einträge der Dateilisten über den Haupt-Thread aus; bei
// großen Verzeichnissen hängt QFiles dann jeweils mehrere Sekunden. Mit der Option „Bedienungshilfen vereinfachen“
// melden Dateilisten und Verzeichnisbaum nur sich selbst (Art, Name, Fokus), nicht jeden Eintrag.

#include <windows.h>
#include <string>

namespace qf {

// Für WM_GETOBJECT: true = beantwortet (result setzen und zurückgeben). controlType: UIA_ListControlTypeId usw.
bool SimpleUiaGetObject(HWND h, WPARAM wp, LPARAM lp, const std::wstring& name, long controlType, LRESULT& result);
// Bei WM_DESTROY/WM_NCDESTROY: UI Automation die Verbindung zum Fenster lösen lassen
void SimpleUiaDisconnect(HWND h);

} // namespace qf
