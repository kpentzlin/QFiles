#include "Glyphs.h"

#include <algorithm>

namespace qf {

COLORREF Blend(COLORREF a, COLORREF b, int alphaB) {
    auto mix = [&](int x, int y) { return (x * (255 - alphaB) + y * alphaB) / 255; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

COLORREF Lighten(COLORREF c, int amount) { return Blend(c, RGB(255, 255, 255), amount); }

namespace {

struct GdiSel {
    HDC dc;
    HGDIOBJ old;
    GdiSel(HDC d, HGDIOBJ o) : dc(d), old(SelectObject(d, o)) {}
    ~GdiSel() { SelectObject(dc, old); }
};

void Poly(HDC dc, const POINT* pts, int n, COLORREF fill, COLORREF line, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, line);
    HBRUSH br = CreateSolidBrush(fill);
    {
        GdiSel sp(dc, pen), sb(dc, br);
        Polygon(dc, pts, n);
    }
    DeleteObject(pen);
    DeleteObject(br);
}

void Lines(HDC dc, const POINT* pts, int n, COLORREF line, int width) {
    LOGBRUSH lb{BS_SOLID, line, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, width, &lb, 0, nullptr);
    {
        GdiSel sp(dc, pen);
        Polyline(dc, pts, n);
    }
    DeleteObject(pen);
}

void Box(HDC dc, int l, int t, int r, int b, COLORREF fill, COLORREF line, int width) {
    POINT p[4] = {{l, t}, {r, t}, {r, b}, {l, b}};
    Poly(dc, p, 4, fill, line, width);
}

} // namespace

void DrawGlyph(HDC dc, Glyph g, const RECT& rc, COLORREF color) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int s = std::min(w, h) * 6 / 10;  // Symbolgröße
    if (s < 8) s = 8;
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    int u = std::max(1, s / 8);       // Einheit / Linienstärke
    int half = s / 2;
    int l = cx - half, t = cy - half, r = cx + half, b = cy + half;

    switch (g) {
    case Glyph::Back: {
        POINT p[3] = {{r - u, t + u}, {l + u, cy}, {r - u, b - u}};
        Lines(dc, p, 3, color, u * 2);
        break;
    }
    case Glyph::Forward: {
        POINT p[3] = {{l + u, t + u}, {r - u, cy}, {l + u, b - u}};
        Lines(dc, p, 3, color, u * 2);
        break;
    }
    case Glyph::Up: {
        POINT p[3] = {{l + u, cy + u}, {cx, t + u}, {r - u, cy + u}};
        Lines(dc, p, 3, color, u * 2);
        POINT s2[2] = {{cx, t + 2 * u}, {cx, b - u}};
        Lines(dc, s2, 2, color, u * 2);
        break;
    }
    case Glyph::Browse: {
        // Ordner
        COLORREF fill = RGB(250, 214, 110);
        POINT p[6] = {{l, t + 2 * u}, {cx - u, t + 2 * u}, {cx + u, t + 4 * u}, {r, t + 4 * u}, {r, b - u}, {l, b - u}};
        Poly(dc, p, 6, fill, Blend(fill, RGB(0, 0, 0), 120), std::max(1, u / 2));
        POINT d[3] = {{cx - 2 * u, cy + 2 * u}, {cx, cy + 2 * u}, {cx + 2 * u, cy + 2 * u}};
        for (auto& q : d) {
            RECT dot{q.x - u / 2 - 1, q.y - u / 2 - 1, q.x + u / 2 + 1, q.y + u / 2 + 1};
            HBRUSH br = CreateSolidBrush(Blend(fill, RGB(0, 0, 0), 160));
            FillRect(dc, &dot, br);
            DeleteObject(br);
        }
        break;
    }
    case Glyph::Refresh: {
        HPEN pen = CreatePen(PS_SOLID, u * 2, color);
        GdiSel sp(dc, pen);
        GdiSel sb(dc, GetStockObject(NULL_BRUSH));
        Arc(dc, l + u, t + u, r - u, b - u, r - u, cy, cx, t + u);
        POINT a[3] = {{r - 4 * u, cy - u}, {r - u, cy + 2 * u}, {r + u, cy - 2 * u}};
        Poly(dc, a, 3, color, color, 1);
        DeleteObject(pen);
        break;
    }
    case Glyph::Split: {
        // Rahmen mit waagerechter Teilung: zwei Listen übereinander
        COLORREF fill = RGB(255, 255, 255);
        Box(dc, l, t, r, b, fill, color, std::max(1, u));
        RECT top{l + u + 1, t + u + 1, r - u, t + 2 * u + 2};
        HBRUSH br = CreateSolidBrush(Lighten(color, 120));
        FillRect(dc, &top, br);
        RECT bot{l + u + 1, cy + u + 1, r - u, cy + 2 * u + 2};
        FillRect(dc, &bot, br);
        DeleteObject(br);
        POINT m[2] = {{l, cy}, {r, cy}};
        Lines(dc, m, 2, color, std::max(1, u));
        break;
    }
    case Glyph::Unsplit: {
        // Rahmen ohne Teilung, Pfeile zur Mitte hin (Teilung aufheben)
        COLORREF fill = RGB(255, 255, 255);
        Box(dc, l, t, r, b, fill, color, std::max(1, u));
        POINT a1[3] = {{cx - 2 * u, t + 2 * u}, {cx + 2 * u, t + 2 * u}, {cx, cy - u}};
        POINT a2[3] = {{cx - 2 * u, b - 2 * u}, {cx + 2 * u, b - 2 * u}, {cx, cy + u}};
        COLORREF red = RGB(200, 40, 40);
        Poly(dc, a1, 3, red, red, 1);
        Poly(dc, a2, 3, red, red, 1);
        POINT m[2] = {{l + 2 * u, cy}, {r - 2 * u, cy}};
        Lines(dc, m, 2, red, std::max(1, u / 2));
        break;
    }
    case Glyph::Bookmark: {
        // Lesezeichenband (gelb) mit Pluszeichen
        COLORREF fill = RGB(255, 200, 40);
        COLORREF line = RGB(170, 120, 0);
        int bl = l + u, br = r - 2 * u;
        int bmx = (bl + br) / 2;
        POINT p[5] = {{bl, t}, {br, t}, {br, b}, {bmx, b - 3 * u}, {bl, b}};
        Poly(dc, p, 5, fill, line, std::max(1, u / 2));
        // Plus unten rechts
        int px = r - u, py = b - u, ps = 2 * u + 1;
        RECT bg{px - ps - u, py - ps - u, px + u, py + u};
        HBRUSH wb = CreateSolidBrush(RGB(40, 150, 40));
        HBRUSH old = (HBRUSH)SelectObject(dc, wb);
        HPEN np = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, bg.left, bg.top, bg.right, bg.bottom);
        SelectObject(dc, old);
        SelectObject(dc, np);
        DeleteObject(wb);
        int mx = (bg.left + bg.right) / 2, my = (bg.top + bg.bottom) / 2, k = std::max(2, u + 1);
        POINT h1[2] = {{mx - k, my}, {mx + k, my}};
        POINT v1[2] = {{mx, my - k}, {mx, my + k}};
        Lines(dc, h1, 2, RGB(255, 255, 255), std::max(1, u));
        Lines(dc, v1, 2, RGB(255, 255, 255), std::max(1, u));
        break;
    }
    case Glyph::Eraser: {
        // Radiergummi, schräg gestellt: rosa Körper, blaue Manschette, Wischspur darunter
        int full = std::min(w, h);
        int ox = rc.left + (w - full) / 2, oy = rc.top + (h - full) / 2;
        auto P = [&](double x, double y) { return POINT{ox + (int)(x * full / 16.0 + 0.5), oy + (int)(y * full / 16.0 + 0.5)}; };
        COLORREF outline = RGB(90, 50, 60);
        POINT body[4] = {P(6.5, 2.0), P(14.0, 9.5), P(10.5, 13.0), P(3.0, 5.5)};
        Poly(dc, body, 4, RGB(240, 120, 150), outline, 1);
        POINT sleeve[4] = {P(3.0, 5.5), P(10.5, 13.0), P(8.5, 15.0), P(1.0, 7.5)};
        Poly(dc, sleeve, 4, RGB(70, 120, 210), outline, 1);
        POINT trail[2] = {P(9.5, 15.2), P(15.5, 15.2)};
        Lines(dc, trail, 2, RGB(120, 120, 120), std::max(1, full / 16));
        break;
    }
    case Glyph::TreeList: {
        // Ansicht „Mit Unterverzeichnissen“: Ordner oben, darunter eingerückt zwei Dateien mit Verbindungslinien
        int full = std::min(w, h);
        int ox = rc.left + (w - full) / 2, oy = rc.top + (h - full) / 2;
        auto P = [&](double x, double y) { return POINT{ox + (int)(x * full / 16.0 + 0.5), oy + (int)(y * full / 16.0 + 0.5)}; };
        int lw = std::max(1, full / 16);
        POINT folder[6] = {P(0.5, 1.5), P(3.5, 1.5), P(4.5, 2.5), P(8.5, 2.5), P(8.5, 6.5), P(0.5, 6.5)};
        Poly(dc, folder, 6, RGB(244, 204, 90), RGB(160, 115, 30), lw);
        POINT tree[3] = {P(3.0, 6.5), P(3.0, 13.5), P(6.0, 13.5)};
        Lines(dc, tree, 3, RGB(110, 110, 110), lw);
        POINT branch[2] = {P(3.0, 9.5), P(6.0, 9.5)};
        Lines(dc, branch, 2, RGB(110, 110, 110), lw);
        for (double y : {7.5, 11.5}) {
            POINT sheet[5] = {P(6.5, y), P(9.0, y), P(10.5, y + 1.5), P(10.5, y + 4.0), P(6.5, y + 4.0)};
            Poly(dc, sheet, 5, RGB(255, 255, 255), RGB(70, 100, 160), lw);
            POINT text[2] = {P(12.0, y + 2.0), P(15.5, y + 2.0)};
            Lines(dc, text, 2, RGB(60, 90, 150), lw);
        }
        POINT top[2] = {P(10.0, 4.5), P(15.5, 4.5)};
        Lines(dc, top, 2, RGB(60, 90, 150), lw);
        break;
    }
    }
}

} // namespace qf
