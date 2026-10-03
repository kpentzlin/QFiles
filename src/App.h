#pragma once
// Globale Programmdienste von QFiles.
//
// - Instanz, Hauptfenster, Programmsymbole, Schriften
// - Einstellungen (Config + Options)
// - Registrierung nicht modaler Fenster (IsDialogMessage) und Tastenkürzel-Tabellen pro Fenster
// - Dienste des Hauptfensters für die Module (Navigation, Aktualisieren, Kontext der aktiven Liste)
// - Protokoll der Dateioperationen ("Dateisystemmonitor")

#include <windows.h>
#include <string>
#include <vector>

#include "Settings.h"

namespace qf {

// Zustand der aktiven Dateiliste – für Funktionstasten, Module usw.
struct PaneContext {
    std::wstring dir;                    // aktuelles Verzeichnis der aktiven Liste
    std::vector<std::wstring> selected;  // markierte Namen (nur Namen, ohne Pfad); leer = keine Markierung
    std::wstring focused;                // Name des Eintrags mit dem Fokus (kann leer sein)
    std::wstring otherDir;               // Verzeichnis der Gegenseite (andere Liste)
    std::vector<std::wstring> SelectedOrFocused() const {
        if (!selected.empty()) return selected;
        if (!focused.empty()) return {focused};
        return {};
    }
};

class App {
public:
    static HINSTANCE Instance();
    static void SetInstance(HINSTANCE h);
    static HWND MainWindow();
    static void SetMainWindow(HWND h);
    static HICON SmallIcon();
    static HICON BigIcon();

    static Config& Cfg();
    static Options& Opt();
    static void SaveSettings();        // Hauptfenster-Zustand + Options + Config auf Platte

    // Schriften (vom Hauptfenster passend zur DPI erzeugt)
    static HFONT UIFont();             // Oberflächenschrift (Segoe UI)
    static HFONT MonoFont();           // Festbreitenschrift für Anzeige/Hex (Consolas)

    // Nicht modale Dialoge/Fenster: werden in der Nachrichtenschleife mit IsDialogMessage behandelt
    static void RegisterModeless(HWND h);
    static void UnregisterModeless(HWND h);
    // Tastenkürzel pro Hauptfenster (Top-Level), z. B. für Editor-Fenster
    static void RegisterAccelerator(HWND topLevel, HACCEL accel);
    static void UnregisterAccelerator(HWND topLevel);
    // Aufruf in der Nachrichtenschleife; true = Nachricht wurde verarbeitet
    static bool PreTranslate(MSG* msg);

    // ---- Dienste des Hauptfensters (implementiert in MainWindow.cpp) ----
    // Zeigt dir in der aktiven Liste an; selectName wird markiert und sichtbar gemacht.
    static void NavigateTo(const std::wstring& dir, const std::wstring& selectName = L"");
    // Zeigt den Ordner einer Datei an und markiert die Datei.
    static void NavigateToFile(const std::wstring& fullPath);
    // Alle Listen und den Baum neu einlesen.
    static void RefreshPanes();
    static PaneContext ActivePaneContext();
};

// ---- Dateisystemmonitor: Protokoll aller von QFiles ausgeführten Dateioperationen ----
void LogOperation(const std::wstring& text);
void ShowOperationLog(HWND owner);

} // namespace qf
