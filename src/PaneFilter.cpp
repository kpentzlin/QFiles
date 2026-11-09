// Filter einer Dateiliste und .gitignore-Auswertung (siehe PaneFilter.h)

#include "PaneFilter.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "Util.h"

namespace qf {

// ===================== Glob wie in .gitignore =====================

namespace {

wchar_t Low(wchar_t c) { return (wchar_t)towlower(c); }

// [..]-Klasse ab p (p zeigt hinter '['); end = Position hinter ']' oder npos bei ungültiger Klasse
bool MatchClass(std::wstring_view p, size_t& pos, wchar_t c) {
    size_t i = pos;
    bool neg = false;
    if (i < p.size() && (p[i] == L'!' || p[i] == L'^')) {
        neg = true;
        ++i;
    }
    bool hit = false, first = true;
    while (i < p.size() && (first || p[i] != L']')) {
        first = false;
        wchar_t a = p[i];
        if (a == L'\\' && i + 1 < p.size()) a = p[++i];
        if (i + 2 < p.size() && p[i + 1] == L'-' && p[i + 2] != L']') {
            wchar_t b = p[i + 2];
            if (Low(c) >= Low(a) && Low(c) <= Low(b)) hit = true;
            i += 3;
        } else {
            if (Low(c) == Low(a)) hit = true;
            ++i;
        }
    }
    if (i >= p.size()) {
        pos = std::wstring_view::npos;
        return false;
    }
    pos = i + 1;   // hinter ']'
    return hit != neg;
}

bool GlobRec(std::wstring_view p, std::wstring_view t) {
    while (!p.empty()) {
        wchar_t c = p[0];
        if (c == L'*') {
            if (p.size() >= 2 && p[1] == L'*') {
                // "**": beliebig viele Zeichen einschließlich '/'
                std::wstring_view rest = p.substr(2);
                if (!rest.empty() && rest[0] == L'/') {
                    // "**/": null oder mehr Verzeichnisse
                    std::wstring_view r2 = rest.substr(1);
                    if (GlobRec(r2, t)) return true;
                    for (size_t i = 0; i < t.size(); ++i)
                        if (t[i] == L'/' && GlobRec(r2, t.substr(i + 1))) return true;
                    return false;
                }
                for (size_t i = 0; i <= t.size(); ++i)
                    if (GlobRec(rest, t.substr(i))) return true;
                return false;
            }
            std::wstring_view rest = p.substr(1);
            for (size_t i = 0; i <= t.size(); ++i) {
                if (GlobRec(rest, t.substr(i))) return true;
                if (i < t.size() && t[i] == L'/') break;
            }
            return false;
        }
        if (t.empty()) return false;
        if (c == L'?') {
            if (t[0] == L'/') return false;
        } else if (c == L'[') {
            size_t pos = 1;
            bool m = t[0] != L'/' && MatchClass(p, pos, t[0]);
            if (pos == std::wstring_view::npos) {
                if (t[0] != L'[') return false;   // ungültige Klasse: '[' wörtlich
                p = p.substr(1);
                t = t.substr(1);
                continue;
            }
            if (!m) return false;
            p = p.substr(pos);
            t = t.substr(1);
            continue;
        } else {
            if (c == L'\\' && p.size() >= 2) {
                p = p.substr(1);
                c = p[0];
            }
            if (Low(c) != Low(t[0])) return false;
        }
        p = p.substr(1);
        t = t.substr(1);
    }
    return t.empty();
}

std::wstring Slashes(std::wstring s) {
    std::replace(s.begin(), s.end(), L'\\', L'/');
    return s;
}

std::wstring TrimSlashes(std::wstring s) {
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && EqualsI(s.substr(0, prefix.size()), prefix);
}

} // namespace

bool GitGlobMatch(std::wstring_view pattern, std::wstring_view text) { return GlobRec(pattern, text); }

// ===================== GitIgnore =====================

void GitIgnore::AddRules(const std::wstring& text, const std::wstring& base) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) {
            if (line.back() == L' ' && line.size() >= 2 && line[line.size() - 2] == L'\\') break;
            line.pop_back();
        }
        if (line.empty() || line[0] == L'#') continue;
        Rule r;
        r.base = base;
        if (line[0] == L'!') {
            r.negate = true;
            line.erase(0, 1);
        } else if (line.size() >= 2 && line[0] == L'\\' && (line[1] == L'!' || line[1] == L'#')) {
            line.erase(0, 1);
        }
        if (!line.empty() && line.back() == L'/') {
            r.dirOnly = true;
            while (!line.empty() && line.back() == L'/') line.pop_back();
        }
        if (line.empty()) continue;
        if (line.find(L'/') != std::wstring::npos) {
            r.anchored = true;
            if (line[0] == L'/') line.erase(0, 1);
        }
        r.pattern = line;
        rules_.push_back(std::move(r));
    }
}

bool GitIgnore::Load(const std::wstring& dirIn) {
    rules_.clear();
    files_.clear();
    root_.clear();
    std::wstring dir = TrimSlashes(dirIn);
    if (dir.empty()) return false;
    // Kette der Verzeichnisse von dir aufwärts; Projektwurzel = erstes Verzeichnis mit „.git“
    std::vector<std::wstring> chain;
    std::wstring d = dir, repo;
    for (int guard = 0; guard < 256 && !d.empty(); ++guard) {
        chain.push_back(d);
        if (DirExists(PathCombine(d, L".git")) || FileExists(PathCombine(d, L".git"))) {
            repo = d;
            break;
        }
        std::wstring parent = TrimSlashes(PathParent(d));
        if (parent.empty() || EqualsI(parent, d)) break;
        d = parent;
    }
    std::reverse(chain.begin(), chain.end());   // oben zuerst
    std::vector<std::pair<std::wstring, std::wstring>> found;   // Verzeichnis, Inhalt
    for (auto& c : chain) {
        std::wstring f = PathCombine(c, L".gitignore");
        std::vector<uint8_t> bytes;
        if (FileExists(f) && ReadFileBytes(f, bytes, 4 * 1024 * 1024)) {
            std::string s(bytes.begin(), bytes.end());
            found.push_back({c, Utf8ToWide(s)});
            files_.push_back(f);
        }
    }
    if (found.empty()) return false;
    root_ = !repo.empty() ? repo : found.front().first;
    if (!repo.empty()) {
        std::vector<uint8_t> bytes;
        std::wstring ex = PathCombine(repo, L".git\\info\\exclude");
        if (FileExists(ex) && ReadFileBytes(ex, bytes, 1024 * 1024)) AddRules(Utf8ToWide(std::string(bytes.begin(), bytes.end())));
    }
    for (auto& [c, text] : found) {
        std::wstring base;
        if (c.size() > root_.size() && StartsWithI(c, root_)) base = Slashes(TrimSlashes(c.substr(root_.size() + 1)));
        while (!base.empty() && base[0] == L'/') base.erase(0, 1);
        AddRules(text, base);
    }
    return true;
}

int GitIgnore::MatchOne(const std::wstring& rel, bool isDir) const {
    int result = 0;
    for (auto& r : rules_) {
        if (r.dirOnly && !isDir) continue;
        std::wstring sub = rel;
        if (!r.base.empty()) {
            if (!(rel.size() > r.base.size() && StartsWithI(rel, r.base) && rel[r.base.size()] == L'/')) continue;
            sub = rel.substr(r.base.size() + 1);
        }
        bool m;
        if (r.anchored) {
            m = GitGlobMatch(r.pattern, sub);
        } else {
            size_t slash = sub.rfind(L'/');
            m = GitGlobMatch(r.pattern, slash == std::wstring::npos ? sub : sub.substr(slash + 1));
        }
        if (m) result = r.negate ? -1 : 1;
    }
    return result;
}

bool GitIgnore::Ignored(const std::wstring& relIn, bool isDir) const {
    std::wstring rel = Slashes(relIn);
    while (!rel.empty() && rel[0] == L'/') rel.erase(0, 1);
    while (!rel.empty() && rel.back() == L'/') rel.pop_back();
    if (rel.empty()) return false;
    // „.git“ selbst gehört nie zum Projektinhalt
    size_t start = 0;
    for (;;) {
        size_t slash = rel.find(L'/', start);
        std::wstring part = rel.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
        if (EqualsI(part, L".git")) return true;
        if (slash == std::wstring::npos) break;
        // übergeordnetes Verzeichnis ausgeschlossen -> Inhalt ausgeschlossen
        if (MatchOne(rel.substr(0, slash), true) == 1) return true;
        start = slash + 1;
    }
    return MatchOne(rel, isDir) == 1;
}

bool GitIgnore::IgnoredPath(const std::wstring& fullPath, bool isDir) const {
    if (root_.empty()) return false;
    std::wstring p = TrimSlashes(fullPath);
    if (!(p.size() > root_.size() && StartsWithI(p, root_) && (p[root_.size()] == L'\\' || p[root_.size()] == L'/')))
        return false;
    return Ignored(p.substr(root_.size() + 1), isDir);
}

// ===================== Hilfen =====================

int DayNumber(const FILETIME& ft) {
    if (!ft.dwLowDateTime && !ft.dwHighDateTime) return 0;
    FILETIME lt;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return 0;
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

int TodayNumber() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

int AddDays(int ymd, int days) {
    SYSTEMTIME st{};
    st.wYear = (WORD)(ymd / 10000);
    st.wMonth = (WORD)(ymd / 100 % 100);
    st.wDay = (WORD)(ymd % 100);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return ymd;
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart += (ULONGLONG)((long long)days * 864000000000LL);
    ft = {u.LowPart, u.HighPart};
    if (!FileTimeToSystemTime(&ft, &st)) return ymd;
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

bool ParseSizeText(const std::wstring& textIn, int64_t& out) {
    std::wstring t = Trim(textIn);
    out = -1;
    if (t.empty()) return true;
    std::wstring num, unit;
    for (wchar_t c : t) {
        if (iswdigit(c) || c == L',' || c == L'.') num += c == L',' ? L'.' : c;
        else if (!iswspace(c)) unit += (wchar_t)towupper(c);
    }
    if (num.empty()) return false;
    double v = _wtof(num.c_str());
    double mul = 1;
    if (unit.empty() || unit == L"B" || unit == L"BYTE" || unit == L"BYTES") mul = 1;
    else if (unit == L"K" || unit == L"KB" || unit == L"KIB") mul = 1024.0;
    else if (unit == L"M" || unit == L"MB" || unit == L"MIB") mul = 1024.0 * 1024;
    else if (unit == L"G" || unit == L"GB" || unit == L"GIB") mul = 1024.0 * 1024 * 1024;
    else if (unit == L"T" || unit == L"TB" || unit == L"TIB") mul = 1024.0 * 1024 * 1024 * 1024;
    else return false;
    out = (int64_t)(v * mul + 0.5);
    return true;
}

std::wstring SizeShortText(int64_t b) {
    if (b < 0) return L"";
    const wchar_t* units[] = {L"Bytes", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1024 && u < 4 && std::fmod(v, 1024) == 0) {
        v /= 1024;
        ++u;
    }
    if (u == 0 && b >= 1024) {
        // nicht glatt teilbar: auf eine Nachkommastelle
        while (v >= 1024 && u < 4) {
            v /= 1024;
            ++u;
        }
        std::wstring s = Format(L"%.1f", v);
        std::replace(s.begin(), s.end(), L'.', L',');
        return s + L" " + units[u];
    }
    return Format(L"%lld", (long long)v) + L" " + units[u];
}

namespace {

std::vector<std::wstring> MaskList(const std::wstring& masks) {
    std::vector<std::wstring> r;
    std::wstring cur;
    for (wchar_t c : masks) {
        if (c == L'/' || c == L';' || c == L'|') {
            if (!Trim(cur).empty()) r.push_back(Trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!Trim(cur).empty()) r.push_back(Trim(cur));
    return r;
}

bool MatchMasks(const std::wstring& masks, const std::wstring& name, bool emptyResult) {
    auto list = MaskList(masks);
    if (list.empty()) return emptyResult;
    for (auto& m : list)
        if (WildcardMatch(m, name)) return true;
    return false;
}

std::wstring DateText(int ymd) { return Format(L"%02d.%02d.%04d", ymd % 100, ymd / 100 % 100, ymd / 10000); }

std::wstring DatePart(const wchar_t* what, int mode, int from, int to, int days) {
    if (mode == 2) return std::wstring(what) + L" seit " + std::to_wstring(days) + (days == 1 ? L" Tag" : L" Tagen");
    if (mode != 1 || (!from && !to)) return L"";
    if (from && to) return std::wstring(what) + L" " + DateText(from) + L"–" + DateText(to);
    if (from) return std::wstring(what) + L" ab " + DateText(from);
    return std::wstring(what) + L" bis " + DateText(to);
}

const wchar_t* kSortNames[] = {L"",        L"Name ↑",     L"Name ↓",      L"Typ ↑",       L"Typ ↓",       L"Größe ↑",
                               L"Größe ↓", L"Erstellt ↑", L"Erstellt ↓", L"Geändert ↑", L"Geändert ↓"};

} // namespace

bool PaneFilter::MaskActive() const {
    std::wstring m = Trim(masks);
    return !(m.empty() || m == L"*" || m == L"*.*") || !Trim(exclude).empty();
}

std::wstring PaneFilter::ShortText() const {
    std::vector<std::wstring> parts;
    std::wstring m = Trim(masks);
    if (!(m.empty() || m == L"*" || m == L"*.*")) parts.push_back(Join(MaskList(m), L" / "));
    if (!Trim(exclude).empty()) parts.push_back(L"ohne " + Join(MaskList(exclude), L" / "));
    if (dirMode == 1) parts.push_back(L"ohne Verzeichnisse");
    else if (dirMode == 2) parts.push_back(L"Verzeichnisse laut Maske");
    std::wstring d = DatePart(L"geändert", modMode, modFrom, modTo, modDays);
    if (!d.empty()) parts.push_back(d);
    d = DatePart(L"erstellt", crMode, crFrom, crTo, crDays);
    if (!d.empty()) parts.push_back(d);
    if (sizeMin >= 0 && sizeMax >= 0) parts.push_back(L"Größe " + SizeShortText(sizeMin) + L"–" + SizeShortText(sizeMax));
    else if (sizeMin >= 0) parts.push_back(L"Größe ≥ " + SizeShortText(sizeMin));
    else if (sizeMax >= 0) parts.push_back(L"Größe ≤ " + SizeShortText(sizeMax));
    std::wstring a;
    const wchar_t letters[4] = {L'R', L'H', L'S', L'A'};
    for (int i = 0; i < 4; ++i)
        if (attr[i] >= 0) a += std::wstring(a.empty() ? L"" : L" ") + (attr[i] ? L"+" : L"−") + letters[i];
    if (!a.empty()) parts.push_back(L"Attribute " + a);
    if (git) parts.push_back(L"Git");
    if (sort > 0 && sort <= 10) parts.push_back(std::wstring(L"sortiert: ") + kSortNames[sort]);
    if (parts.empty()) return L"Filter";
    return Join(parts, L", ");
}

std::wstring PaneFilter::ToString() const {
    auto clean = [](std::wstring s) {
        std::replace(s.begin(), s.end(), L'|', L'/');
        return s;
    };
    return Format(L"v1|on=%d|m=", active ? 1 : 0) + clean(masks) + L"|x=" + clean(exclude) +
           Format(L"|d=%d|mm=%d|mf=%d|mt=%d|md=%d|cm=%d|cf=%d|ct=%d|cd=%d|smin=%lld|smax=%lld|a=%d,%d,%d,%d|g=%d|s=%d",
                  dirMode, modMode, modFrom, modTo, modDays, crMode, crFrom, crTo, crDays, (long long)sizeMin,
                  (long long)sizeMax, attr[0], attr[1], attr[2], attr[3], git ? 1 : 0, sort);
}

bool PaneFilter::FromString(const std::wstring& s, PaneFilter& out) {
    if (s.rfind(L"v1|", 0) != 0) return false;
    PaneFilter f;
    size_t pos = 3;
    while (pos <= s.size()) {
        size_t bar = s.find(L'|', pos);
        std::wstring part = s.substr(pos, bar == std::wstring::npos ? std::wstring::npos : bar - pos);
        pos = bar == std::wstring::npos ? s.size() + 1 : bar + 1;
        size_t eq = part.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = part.substr(0, eq), v = part.substr(eq + 1);
        long long n = _wtoi64(v.c_str());
        if (k == L"on") f.active = n != 0;
        else if (k == L"m") f.masks = v;
        else if (k == L"x") f.exclude = v;
        else if (k == L"d") f.dirMode = std::clamp((int)n, 0, 2);
        else if (k == L"mm") f.modMode = std::clamp((int)n, 0, 2);
        else if (k == L"mf") f.modFrom = (int)n;
        else if (k == L"mt") f.modTo = (int)n;
        else if (k == L"md") f.modDays = std::max(0, (int)n);
        else if (k == L"cm") f.crMode = std::clamp((int)n, 0, 2);
        else if (k == L"cf") f.crFrom = (int)n;
        else if (k == L"ct") f.crTo = (int)n;
        else if (k == L"cd") f.crDays = std::max(0, (int)n);
        else if (k == L"smin") f.sizeMin = n;
        else if (k == L"smax") f.sizeMax = n;
        else if (k == L"g") f.git = n != 0;
        else if (k == L"s") f.sort = std::clamp((int)n, 0, 10);
        else if (k == L"a") {
            auto vals = Split(v, L',');
            for (int i = 0; i < 4 && i < (int)vals.size(); ++i) f.attr[i] = std::clamp(_wtoi(vals[i].c_str()), -1, 1);
        }
    }
    out = f;
    return true;
}

bool FilterAccepts(const PaneFilter& f, const DirEntry& e, const std::wstring& fullPath, const GitIgnore* git) {
    if (!f.active) return true;
    bool isDir = e.IsDir();
    if (f.git && git && !git->Root().empty() && git->IgnoredPath(fullPath, isDir)) return false;
    if (isDir) {
        if (f.dirMode == 1) return false;
        if (f.dirMode == 2) return MatchMasks(f.masks, e.name, true) && !MatchMasks(f.exclude, e.name, false);
        return true;
    }
    if (!MatchMasks(f.masks, e.name, true)) return false;
    if (MatchMasks(f.exclude, e.name, false)) return false;
    auto dateOk = [](int mode, int from, int to, int days, const FILETIME& ft) {
        if (mode == 0) return true;
        int d = DayNumber(ft);
        if (!d) return false;
        if (mode == 2) return d >= AddDays(TodayNumber(), -std::max(0, days));
        if (from && d < from) return false;
        if (to && d > to) return false;
        return true;
    };
    if (!dateOk(f.modMode, f.modFrom, f.modTo, f.modDays, e.modified)) return false;
    if (!dateOk(f.crMode, f.crFrom, f.crTo, f.crDays, e.created)) return false;
    if (f.sizeMin >= 0 && (int64_t)e.size < f.sizeMin) return false;
    if (f.sizeMax >= 0 && (int64_t)e.size > f.sizeMax) return false;
    const DWORD bits[4] = {FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_ARCHIVE};
    for (int i = 0; i < 4; ++i) {
        if (f.attr[i] < 0) continue;
        bool has = (e.attributes & bits[i]) != 0;
        if (has != (f.attr[i] == 1)) return false;
    }
    return true;
}

} // namespace qf
