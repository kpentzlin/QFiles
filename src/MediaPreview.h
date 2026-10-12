#pragma once
// MediaPreview.cpp – Vorschaubilder für PDF-Dateien und Videos in der Dateianzeige (F11).
//
// PDF: Seiten über Windows.Data.Pdf (Windows 10/11). 1 Seite wie ein Bild; ab 4 Seiten im Hochformat die ersten
// 4 Seiten in 2 Zeilen zu je 2 Seiten (3 Hochformatseiten ebenso); sonst die ersten 2 Seiten übereinander.
// Video: Einzelbilder über Media Foundation. Unter 2 Sekunden das mittlere Bild; Querformat: Bilder nach 1 s, in der
// Mitte und 1 s vor Schluss übereinander; sonst nach 1 s, nach 1/3, nach 2/3 und 1 s vor Schluss in 2 Zeilen.
// Seiten bzw. Bilder sind durch 2 Pixel breite schwarze Striche getrennt. Beide Schnittstellen werden zur Laufzeit
// geladen; fehlen sie (z. B. Windows Server ohne Media Foundation), wird das gemeldet.

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>

namespace qf {

enum class MediaKind { None = 0, Pdf = 1, Video = 2 };

// Art der Vorschau anhand der Dateiendung
MediaKind MediaKindForPath(const std::wstring& path);

struct MediaResult {
    HBITMAP bmp = nullptr;      // Gesamtbild: 32 bpp, top-down, deckend (nullptr = nur Text)
    void* bits = nullptr;
    int w = 0, h = 0;
    std::wstring band;          // Video: Kopfzeile mit Auflösung und Dauer
    std::wstring format;        // für die Infozeile, z. B. "PDF · 12 Seiten"
    std::wstring message;       // Text statt Bild (Video abgebrochen/Fehler, PDF-Fehler)
    bool cancelled = false;     // mit Esc abgebrochen (unvollständig, nicht zwischenspeichern)
    bool failed = false;        // Fehler (message gesetzt)
    bool unavailable = false;   // Windows.Data.Pdf bzw. Media Foundation fehlt
};

// Erstellt das Gesamtbild im Arbeitsthread (COM muss initialisiert sein, MTA). boxW/boxH: Fläche, in die das Bild
// passen soll (Pixel). cancel: Abbruch (Esc). progress: Fortschrittstext für die Anzeige.
void RenderMediaPreview(MediaKind kind, const std::wstring& path, int boxW, int boxH, const std::atomic<bool>& cancel,
                        const std::function<void(const std::wstring&)>& progress, MediaResult& out);

} // namespace qf
