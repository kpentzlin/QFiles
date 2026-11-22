#pragma once
// Selbst gezeichnete Symbole (GDI, DPI-unabhängig) für Kopfzeilen- und Pfadzeilen-Schaltflächen.

#include <windows.h>
#include <cstdint>
#include <functional>

namespace qf {

enum class Glyph { Back, Forward, Up, Browse, Refresh, Split, Unsplit, Bookmark, Eraser, TreeList, Filter,
                   EnlargeTop, EnlargeBottom };   // geteilte Spalte: obere bzw. untere Liste auf drei Viertel

// Zeichnet das Symbol zentriert in rc. color = Linienfarbe.
void DrawGlyph(HDC dc, Glyph g, const RECT& rc, COLORREF color);

// Schlanker sechsstrahliger Stern (Asterisk) mittig in rc, geglättet auf dem Hintergrund back (Favoriten)
void DrawAsterisk(HDC dc, const RECT& rc, COLORREF color, COLORREF back);

// Zeichnet ein Symbol auf Schwarz und auf Weiß und berechnet daraus Farbe und Deckkraft je Bildpunkt (geglättete
// Kanten und Schrift ohne Farbsaum). px: 32-Bit-Streifen (oben beginnend, ARGB ohne Vormultiplikation), stride in
// Bildpunkten, x0 = Spalte der Zelle.
void RenderAlphaCell(uint32_t* px, int stride, int x0, int sz, const std::function<void(HDC, const RECT&)>& draw);

// Symbol „Konfiguration“ (Fenster mit drei Listen, hell-ockerfarbige Titelleiste): Werkzeugleiste und Lesezeichen
void DrawConfigSymbol(HDC dc, const RECT& rc);

// Ordnerfarben für Lesezeichen (Kontextmenü „Symbolfarbe“): 0 = gelb (Windows-Ordnersymbol unverändert)
constexpr int kFolderColorCount = 14;
const wchar_t* FolderColorName(int index);
// Windows-Ordnersymbol (klein) in der Farbe index umgefärbt, size x size Bildpunkte; nullptr bei index 0 oder Fehler.
// Der Aufrufer gibt das Symbol mit DestroyIcon frei.
HICON CreateTintedFolderIcon(int size, int index);

// Hellt eine Farbe auf (amount 0..255 Richtung Weiß)
COLORREF Lighten(COLORREF c, int amount);
COLORREF Blend(COLORREF a, COLORREF b, int alphaB); // alphaB 0..255

} // namespace qf
