#pragma once
// Gespeicherte Konfigurationen (Schaltfläche „Konfiguration speichern“, Lesezeichen mit hell-ockerfarbigem Hintergrund):
// Aufteilung der Listenfenster (eine/zwei Spalten, geteilt, Verhältnisse), aktive Liste und je sichtbarer Liste
// Verzeichnis und Ansicht. Gespeichert als Text im Lesezeichen (Einstellungsdatei, [Lesezeichen] KonfigurationN).

#include <string>

namespace qf {

struct LayoutConfig {
    struct Pane {
        bool used = false;       // Liste war sichtbar (nur diese werden wiederhergestellt)
        int view = 0;            // PaneView (0 Details, 1 Liste, 2 Symbole, 3 Miniaturen, 4 Mit Unterverzeichnissen)
        std::wstring dir;
    };
    bool twoPanes = true;
    bool split[2] = {false, false};
    double colRatio = 0.5;
    double rowRatio[2] = {0.5, 0.5};
    int active = 0;
    Pane panes[4];

    std::wstring ToString() const;
    static bool FromString(const std::wstring& s, LayoutConfig& out);
    // Mehrzeilige Beschreibung (Tooltip): Aufteilung, dann je Liste Lage, Verzeichnis, Ansicht
    std::wstring Describe() const;
};

// Lage einer Liste (0–3) für Texte: „links“, „rechts oben“ …
std::wstring PanePlaceName(int index, bool twoPanes, bool splitColumn);
// Name einer Ansicht (PaneView als Zahl)
const wchar_t* PaneViewName(int view);

} // namespace qf
