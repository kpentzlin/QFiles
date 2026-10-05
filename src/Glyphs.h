#pragma once
// Selbst gezeichnete Symbole (GDI, DPI-unabhängig) für Kopfzeilen- und Pfadzeilen-Schaltflächen.

#include <windows.h>

namespace qf {

enum class Glyph { Back, Forward, Up, Browse, Refresh, Split, Unsplit, Bookmark, Eraser, TreeList };

// Zeichnet das Symbol zentriert in rc. color = Linienfarbe.
void DrawGlyph(HDC dc, Glyph g, const RECT& rc, COLORREF color);

// Hellt eine Farbe auf (amount 0..255 Richtung Weiß)
COLORREF Lighten(COLORREF c, int amount);
COLORREF Blend(COLORREF a, COLORREF b, int alphaB); // alphaB 0..255

} // namespace qf
