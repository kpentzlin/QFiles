#pragma once
// Netzwerk: Rechner und Freigaben finden (für den Knoten „Netzwerk“ im Baum und die Netzwerkebenen der Listen).
//
// Beide Suchen können lange dauern (nicht erreichbare Rechner, Netzwerkerkennung). Sie laufen daher in einem
// eigenen Thread; gefundene Einträge werden einzeln gemeldet, und die Suche lässt sich abbrechen (Esc).

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace qf {

struct NetworkItem {
    std::wstring name;      // Rechnername (ohne \\) bzw. Name der Freigabe
    std::wstring comment;   // Beschreibung, falls bekannt
};

// Zustand einer laufenden Suche. cancel = true beendet die Meldungen (der Thread kann noch in einem
// blockierenden Netzwerkaufruf hängen, meldet danach aber nichts mehr).
struct NetworkScan {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
};

// Startet die Suche nach Rechnern (location = "\\") bzw. nach den Freigaben eines Rechners ("\\Server").
// found und finished werden im Suchthread aufgerufen (typisch: PostMessage an ein Fenster).
// finished(ok, error): ok = false, wenn der Rechner nicht erreichbar war (error = Win32-Fehlercode).
std::shared_ptr<NetworkScan> StartNetworkScan(const std::wstring& location, bool includeHidden,
                                              std::function<void(const NetworkItem&)> found,
                                              std::function<void(bool ok, DWORD error)> finished);

// Rechner/Freigaben aus verbundenen Netzlaufwerken (sofort, ohne Netzwerkzugriff) – z. B. für die Anzeige
// des Netzlaufwerks im Baum: "N:" -> "\\fritz.box\FRITZ.NAS\media1". Leer bei lokalen Laufwerken.
std::wstring MappedDriveTarget(const std::wstring& driveRoot);

} // namespace qf
