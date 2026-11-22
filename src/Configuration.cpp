#include "Configuration.h"
#include "Util.h"

#include <algorithm>
#include <cmath>

namespace qf {

namespace {

// '|' trennt die Felder, '=' Schlüssel und Wert, ';' Ansicht und Verzeichnis; diese Zeichen und '%' im Verzeichnis
// werden als %XX geschrieben
std::wstring Escape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'%' || c == L'|' || c == L';' || c == L'=' || c < 32) {
            wchar_t buf[8];
            swprintf(buf, 8, L"%%%02X", (unsigned)c & 0xFF);
            r += buf;
        } else {
            r += c;
        }
    }
    return r;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            r += (wchar_t)wcstol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            r += s[i];
        }
    }
    return r;
}

int Permille(double v) { return (int)std::lround(std::clamp(v, 0.1, 0.9) * 1000); }

} // namespace

const wchar_t* PaneViewName(int view) {
    switch (view) {
    case 1: return L"Liste";
    case 2: return L"Symbole";
    case 3: return L"Miniaturen";
    case 4: return L"Mit Unterverzeichnissen";
    default: return L"Details";
    }
}

std::wstring PanePlaceName(int index, bool twoPanes, bool splitColumn) {
    int col = index % 2;
    bool bottom = index >= 2;
    std::wstring s = twoPanes ? (col == 0 ? L"links" : L"rechts") : L"";
    if (splitColumn) s += std::wstring(s.empty() ? L"" : L" ") + (bottom ? L"unten" : L"oben");
    return s.empty() ? L"einzige Liste" : s;
}

std::wstring LayoutConfig::ToString() const {
    std::wstring s = L"v1|zwei=" + std::to_wstring(twoPanes ? 1 : 0) + L"|teilL=" + std::to_wstring(split[0] ? 1 : 0) +
                     L"|teilR=" + std::to_wstring(split[1] ? 1 : 0) + L"|spalten=" + std::to_wstring(Permille(colRatio)) +
                     L"|zeilenL=" + std::to_wstring(Permille(rowRatio[0])) + L"|zeilenR=" +
                     std::to_wstring(Permille(rowRatio[1])) + L"|aktiv=" + std::to_wstring(active + 1);
    for (int i = 0; i < 4; ++i)
        if (panes[i].used)
            s += L"|liste" + std::to_wstring(i + 1) + L"=" + std::to_wstring(panes[i].view) + L";" + Escape(panes[i].dir);
    return s;
}

bool LayoutConfig::FromString(const std::wstring& s, LayoutConfig& out) {
    auto parts = Split(s, L'|');
    if (parts.empty() || parts[0] != L"v1") return false;
    LayoutConfig c;
    bool anyPane = false;
    for (size_t i = 1; i < parts.size(); ++i) {
        size_t eq = parts[i].find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = parts[i].substr(0, eq), v = parts[i].substr(eq + 1);
        int n = _wtoi(v.c_str());
        if (k == L"zwei") c.twoPanes = n != 0;
        else if (k == L"teilL") c.split[0] = n != 0;
        else if (k == L"teilR") c.split[1] = n != 0;
        else if (k == L"spalten") c.colRatio = std::clamp(n / 1000.0, 0.1, 0.9);
        else if (k == L"zeilenL") c.rowRatio[0] = std::clamp(n / 1000.0, 0.1, 0.9);
        else if (k == L"zeilenR") c.rowRatio[1] = std::clamp(n / 1000.0, 0.1, 0.9);
        else if (k == L"aktiv") c.active = std::clamp(n - 1, 0, 3);
        else if (k.size() == 6 && StartsWithI(k, L"liste") && k[5] >= L'1' && k[5] <= L'4') {
            size_t semi = v.find(L';');
            if (semi == std::wstring::npos) continue;
            Pane& p = c.panes[k[5] - L'1'];
            p.used = true;
            p.view = std::clamp(_wtoi(v.substr(0, semi).c_str()), 0, 4);
            p.dir = Unescape(v.substr(semi + 1));
            anyPane = anyPane || !p.dir.empty();
        }
    }
    if (!anyPane) return false;
    out = c;
    return true;
}

std::wstring LayoutConfig::Describe() const {
    std::wstring s = twoPanes ? L"Zwei Listen nebeneinander" : L"Eine Liste";
    if (split[0] && (split[1] && twoPanes)) s += L", beide geteilt";
    else if (split[0]) s += twoPanes ? L", links geteilt" : L", geteilt";
    else if (split[1] && twoPanes) s += L", rechts geteilt";
    for (int i = 0; i < 4; ++i) {
        if (!panes[i].used) continue;
        std::wstring place = PanePlaceName(i, twoPanes, split[i % 2]);
        if (!place.empty()) place[0] = towupper(place[0]);
        s += L"\n" + place + L": " + panes[i].dir + L" (" + PaneViewName(panes[i].view) + L")";
        if (i == active) s += L" – aktiv";
    }
    return s;
}

} // namespace qf
