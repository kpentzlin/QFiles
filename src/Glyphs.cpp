#include "Glyphs.h"

#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <vector>

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
    case Glyph::EnlargeTop:
    case Glyph::EnlargeBottom: {
        // Zwei übereinanderliegende Listen mit kleinem Abstand: die eigene (oben bzw. unten) drei Viertel hoch,
        // hellblau mit Winkel zur kleineren hin, die andere ein weißer Streifen. Dünne graublaue Linien.
        bool top = g == Glyph::EnlargeTop;
        bool off = color == GetSysColor(COLOR_GRAYTEXT);
        int H = std::max(10, std::min(w, h) * 7 / 10);
        int W = H * 7 / 8;
        int L = cx - W / 2, R = L + W, T = cy - H / 2, B = T + H;
        int gap = std::max(2, u * 2);
        int big = (H - gap) * 3 / 4;
        COLORREF line = off ? color : RGB(110, 120, 135);
        COLORREF fill = off ? RGB(225, 225, 225) : RGB(214, 228, 247);
        COLORREF acc = off ? color : RGB(40, 105, 200);
        int bt = top ? T : B - big, bb = top ? T + big : B;          // eigene Liste
        int st = top ? T + big + gap : T, sb = top ? B : B - big - gap;   // andere Liste
        Box(dc, L, bt, R, bb, fill, line, u);
        Box(dc, L, st, R, sb, RGB(255, 255, 255), line, u);
        int cw = std::max(3, W / 4), ch = cw;   // Winkel 90°, gut erkennbar
        int cyv = (bt + bb) / 2 + (top ? ch / 2 : -ch / 2);   // Spitze zur anderen Liste hin
        POINT v[3] = {{cx - cw, top ? cyv - ch : cyv + ch}, {cx, cyv}, {cx + cw, top ? cyv - ch : cyv + ch}};
        Lines(dc, v, 3, acc, std::max(2, u * 2));
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
    case Glyph::Filter: {
        // Trichter (blau): breiter Rand oben, Hals unten
        int full = std::min(w, h);
        int ox = rc.left + (w - full) / 2, oy = rc.top + (h - full) / 2;
        auto P = [&](double x, double y) { return POINT{ox + (int)(x * full / 16.0 + 0.5), oy + (int)(y * full / 16.0 + 0.5)}; };
        POINT f[6] = {P(2.0, 2.5), P(14.0, 2.5), P(9.3, 8.2), P(9.3, 13.8), P(6.7, 12.4), P(6.7, 8.2)};
        Poly(dc, f, 6, RGB(70, 125, 205), RGB(30, 60, 120), std::max(1, full / 16));
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

void DrawAsterisk(HDC dc, const RECT& rc, COLORREF color, COLORREF back) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int s = std::min(w, h) * 4 / 5;
    if (s < 6) return;
    // Viermal größer zeichnen und verkleinert ausgeben (geglättete Kanten ohne GDI+)
    const int k = 4;
    int S = s * k;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, S, S);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT all{0, 0, S, S};
    HBRUSH bb = CreateSolidBrush(back);
    FillRect(mem, &all, bb);
    DeleteObject(bb);
    double c = S / 2.0, r = S / 2.0 - k;
    int width = std::max(k, S / 12);  // schlank
    for (int i = 0; i < 3; ++i) {
        double a = 3.14159265358979 / 2 + i * 3.14159265358979 / 3;
        POINT p[2] = {{(int)(c + r * cos(a)), (int)(c - r * sin(a))}, {(int)(c - r * cos(a)), (int)(c + r * sin(a))}};
        Lines(mem, p, 2, color, width);
    }
    int x = (rc.left + rc.right - s) / 2, y = (rc.top + rc.bottom - s) / 2;
    int oldMode = SetStretchBltMode(dc, HALFTONE);
    POINT org;
    SetBrushOrgEx(dc, 0, 0, &org);
    StretchBlt(dc, x, y, s, s, mem, 0, 0, S, S, SRCCOPY);
    SetBrushOrgEx(dc, org.x, org.y, nullptr);
    SetStretchBltMode(dc, oldMode);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}


void RenderAlphaCell(uint32_t* px, int stride, int x0, int sz, const std::function<void(HDC, const RECT&)>& draw) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), sz, -sz, 1, 32, BI_RGB};
    void* bits[2] = {};
    HBITMAP bmp[2] = {CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits[0], nullptr, 0),
                      CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits[1], nullptr, 0)};
    if (!bmp[0] || !bmp[1] || !bits[0] || !bits[1]) {
        for (auto b : bmp)
            if (b) DeleteObject(b);
        return;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    RECT rc{0, 0, sz, sz};
    for (int k = 0; k < 2; ++k) {
        HGDIOBJ old = SelectObject(dc, bmp[k]);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(k == 0 ? BLACK_BRUSH : WHITE_BRUSH));
        draw(dc, rc);
        GdiFlush();
        SelectObject(dc, old);
    }
    DeleteDC(dc);
    auto* b = (const uint32_t*)bits[0];
    auto* w = (const uint32_t*)bits[1];
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            uint32_t cb = b[y * sz + x], cw = w[y * sz + x];
            int diff = 0, ch[3];
            for (int c = 0; c < 3; ++c) {
                int vb = (cb >> (8 * c)) & 0xFF, vw = (cw >> (8 * c)) & 0xFF;
                diff += vw - vb;
                ch[c] = vb;
            }
            int alpha = std::clamp(255 - diff / 3, 0, 255);
            uint32_t out = 0;
            if (alpha > 0) {
                for (int c = 0; c < 3; ++c) out |= (uint32_t)std::min(255, ch[c] * 255 / alpha) << (8 * c);
                out |= (uint32_t)alpha << 24;
            }
            px[y * stride + x0 + x] = out;
        }
    DeleteObject(bmp[0]);
    DeleteObject(bmp[1]);
}

void DrawConfigSymbol(HDC dc, const RECT& rc) {
    int sz = std::min(rc.right - rc.left, rc.bottom - rc.top);
    int x0 = rc.left + (rc.right - rc.left - sz) / 2, y0 = rc.top + (rc.bottom - rc.top - sz) / 2;
    int lw = std::max(1, sz / 16);
    int l = x0 + lw / 2, r = x0 + sz - (lw + 1) / 2;
    int t = y0 + sz / 8, b = y0 + sz - sz / 8;
    const COLORREF line = RGB(70, 60, 45), title = RGB(200, 150, 62), active = RGB(205, 228, 250);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), tb = CreateSolidBrush(title), ab = CreateSolidBrush(active);
    RECT all{l, t, r, b};
    FillRect(dc, &all, white);
    int th = std::max(2, sz * 3 / 16);
    RECT bar{l, t, r, t + th};
    FillRect(dc, &bar, tb);
    int mx = (l + r) / 2, my = (t + th + b) / 2;
    RECT act{mx, t + th, r, my};
    FillRect(dc, &act, ab);
    HPEN pen = CreatePen(PS_SOLID, lw, line);
    {
        GdiSel sp(dc, pen), sb(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, l, t, r + 1, b + 1);
        MoveToEx(dc, l, t + th, nullptr);
        LineTo(dc, r, t + th);
        MoveToEx(dc, mx, t + th, nullptr);
        LineTo(dc, mx, b);
        MoveToEx(dc, mx, my, nullptr);
        LineTo(dc, r, my);
    }
    DeleteObject(pen);
    DeleteObject(white);
    DeleteObject(tb);
    DeleteObject(ab);
}

const wchar_t* FolderColorName(int index) {
    static const wchar_t* names[kFolderColorCount] = {L"gelb",  L"hell-ocker", L"braun",      L"orange", L"hellrot",
                                                      L"rot",   L"hellgrün",   L"grün",       L"hellblau", L"blau",
                                                      L"helltürkis", L"türkis", L"hell-lila", L"lila"};
    return index >= 0 && index < kFolderColorCount ? names[index] : L"";
}

namespace {

void RgbToHsl(double r, double g, double b, double& h, double& s, double& l) {
    double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2;
    if (mx - mn < 1e-6) {
        h = s = 0;
        return;
    }
    double d = mx - mn;
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
}

double HueToRgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

void HslToRgb(double h, double s, double l, double& r, double& g, double& b) {
    if (s <= 0) {
        r = g = b = l;
        return;
    }
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q, hh = h / 360;
    r = HueToRgb(p, q, hh + 1.0 / 3);
    g = HueToRgb(p, q, hh);
    b = HueToRgb(p, q, hh - 1.0 / 3);
}

} // namespace

HICON CreateTintedFolderIcon(int size, int index) {
    // Zielfarben (mittlerer Farbton des Ordners); Index wie FolderColorName
    static const COLORREF targets[kFolderColorCount] = {
        0,                   RGB(226, 196, 136), RGB(150, 98, 52),  RGB(246, 148, 36),  RGB(246, 142, 142),
        RGB(214, 44, 44),    RGB(150, 214, 128), RGB(56, 158, 60),  RGB(140, 190, 242), RGB(46, 108, 212),
        RGB(128, 222, 216),  RGB(24, 162, 164),  RGB(202, 166, 236), RGB(138, 66, 190)};
    if (index <= 0 || index >= kFolderColorCount || size <= 0) return nullptr;
    std::vector<uint32_t> px((size_t)size * size, 0);
    RenderAlphaCell(px.data(), size, 0, size, [&](HDC d, const RECT&) {
        SHSTOCKICONINFO si{sizeof(si)};
        UINT flags = SHGSI_ICON | (size > GetSystemMetrics(SM_CXSMICON) ? SHGSI_LARGEICON : SHGSI_SMALLICON);
        if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, flags, &si))) {
            DrawIconEx(d, 0, 0, si.hIcon, size, size, 0, nullptr, DI_NORMAL);
            DestroyIcon(si.hIcon);
        }
    });
    // Ordnerfläche: gelbe bis orange Bildpunkte (Farbton 15°–75°, nicht grau); deren mittlere Helligkeit und Sättigung
    // werden auf die Zielfarbe gelegt, Abstufungen (Schattierung, Kanten) bleiben erhalten
    struct Hsl { double h, s, l; };
    std::vector<Hsl> hsl(px.size());
    std::vector<bool> tint(px.size(), false);
    double sumL = 0, sumS = 0, wsum = 0;
    for (size_t i = 0; i < px.size(); ++i) {
        uint32_t c = px[i];
        double a = (c >> 24) / 255.0;
        if (a <= 0) continue;
        Hsl& v = hsl[i];
        RgbToHsl(((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, v.h, v.s, v.l);
        if (v.s > 0.15 && v.h >= 15 && v.h <= 75) {
            tint[i] = true;
            sumL += v.l * a;
            sumS += v.s * a;
            wsum += a;
        }
    }
    if (wsum <= 0) return nullptr;
    double l0 = sumL / wsum, s0 = std::max(0.05, sumS / wsum);
    COLORREF t = targets[index];
    double th, ts, tl;
    RgbToHsl(GetRValue(t) / 255.0, GetGValue(t) / 255.0, GetBValue(t) / 255.0, th, ts, tl);
    for (size_t i = 0; i < px.size(); ++i) {
        if (!tint[i]) continue;
        const Hsl& v = hsl[i];
        double l = std::clamp(tl + (v.l - l0), 0.0, 1.0);
        double s = std::clamp(ts * v.s / s0, 0.0, 1.0);
        double r, g, b;
        HslToRgb(th, s, l, r, g, b);
        px[i] = (px[i] & 0xFF000000u) | ((uint32_t)std::lround(r * 255) << 16) | ((uint32_t)std::lround(g * 255) << 8) |
                (uint32_t)std::lround(b * 255);
    }
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    memcpy(bits, px.data(), px.size() * 4);
    std::vector<BYTE> maskBits((size_t)((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

} // namespace qf
