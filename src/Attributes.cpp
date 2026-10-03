// Modul E – Eigenschaften (Attribute und Zeitstempel) ändern, optional rekursiv,
// optional Datum aus dem EXIF-Aufnahmedatum (JPEG/TIFF) übernehmen.

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Util.h"
#include "ToolsCommon.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

namespace qf {

namespace {

using toolsdetail::FileTimeToLocal;
using toolsdetail::LocalToFileTime;

// ===================================================================================
// EXIF-Parser (JPEG APP1 "Exif" bzw. TIFF-Datei)
// ===================================================================================

// Wahlfreier Lesezugriff auf den TIFF-Block: entweder aus dem Speicher (JPEG-APP1) oder aus der Datei (TIFF).
class TiffSource {
public:
    TiffSource(const uint8_t* mem, size_t size) : mem_(mem), size_(size) {}
    TiffSource(HANDLE file, uint64_t size) : file_(file), size_((size_t)std::min<uint64_t>(size, 0xFFFFFFFFull)) {}

    bool Read(uint32_t offset, void* buf, size_t n) const {
        if ((uint64_t)offset + n > size_) return false;
        if (mem_) {
            memcpy(buf, mem_ + offset, n);
            return true;
        }
        LARGE_INTEGER li;
        li.QuadPart = offset;
        if (!SetFilePointerEx(file_, li, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        return ReadFile(file_, buf, (DWORD)n, &got, nullptr) && got == n;
    }
    bool little = true;
    bool U16(uint32_t off, uint16_t& v) const {
        uint8_t b[2];
        if (!Read(off, b, 2)) return false;
        v = little ? (uint16_t)(b[0] | (b[1] << 8)) : (uint16_t)((b[0] << 8) | b[1]);
        return true;
    }
    bool U32(uint32_t off, uint32_t& v) const {
        uint8_t b[4];
        if (!Read(off, b, 4)) return false;
        v = little ? (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24)
                   : ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
        return true;
    }

private:
    const uint8_t* mem_ = nullptr;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    size_t size_ = 0;
};

struct IfdEntry {
    uint16_t tag = 0, type = 0;
    uint32_t count = 0;
    uint32_t valueOffset = 0;   // Position des Wertfelds (für Werte <= 4 Byte) innerhalb des TIFF-Blocks
};

// Liest die Einträge eines IFD (mit Plausibilitätsgrenzen).
bool ReadIfd(const TiffSource& src, uint32_t ifdOffset, std::vector<IfdEntry>& out) {
    out.clear();
    uint16_t n = 0;
    if (!src.U16(ifdOffset, n) || n == 0 || n > 1000) return false;
    for (uint16_t i = 0; i < n; ++i) {
        uint32_t pos = ifdOffset + 2 + i * 12u;
        IfdEntry e;
        if (!src.U16(pos, e.tag) || !src.U16(pos + 2, e.type) || !src.U32(pos + 4, e.count)) return false;
        e.valueOffset = pos + 8;
        out.push_back(e);
    }
    return true;
}

// ASCII-Wert eines Eintrags (Typ 2)
bool ReadAscii(const TiffSource& src, const IfdEntry& e, std::string& out) {
    if (e.type != 2 || e.count == 0 || e.count > 256) return false;
    uint32_t off = e.valueOffset;
    if (e.count > 4 && !src.U32(e.valueOffset, off)) return false;
    std::vector<char> buf(e.count);
    if (!src.Read(off, buf.data(), e.count)) return false;
    out.assign(buf.data(), strnlen(buf.data(), e.count));
    return true;
}

// "JJJJ:MM:TT hh:mm:ss" -> SYSTEMTIME (lokale Zeit)
bool ParseExifDate(const std::string& s, SYSTEMTIME& st) {
    if (s.size() < 19) return false;
    int v[6] = {};
    const int pos[6] = {0, 5, 8, 11, 14, 17};
    const int len[6] = {4, 2, 2, 2, 2, 2};
    for (int k = 0; k < 6; ++k) {
        int x = 0;
        for (int j = 0; j < len[k]; ++j) {
            char c = s[pos[k] + j];
            if (c < '0' || c > '9') return false;
            x = x * 10 + (c - '0');
        }
        v[k] = x;
    }
    if (v[0] < 1601 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 60)
        return false;
    st = SYSTEMTIME{};
    st.wYear = (WORD)v[0];
    st.wMonth = (WORD)v[1];
    st.wDay = (WORD)v[2];
    st.wHour = (WORD)v[3];
    st.wMinute = (WORD)v[4];
    st.wSecond = (WORD)std::min(v[5], 59);
    FILETIME test;
    return SystemTimeToFileTime(&st, &test) != FALSE;   // prüft z. B. 31.02.
}

// Sucht im TIFF-Block: IFD0 -> ExifIFD (0x8769) -> DateTimeOriginal (0x9003), dann DateTimeDigitized (0x9004),
// Rückfall DateTime (0x0132) aus IFD0.
bool ExifDateFromTiff(TiffSource& src, SYSTEMTIME& st) {
    uint8_t hdr[8];
    if (!src.Read(0, hdr, 8)) return false;
    if (hdr[0] == 'I' && hdr[1] == 'I')
        src.little = true;
    else if (hdr[0] == 'M' && hdr[1] == 'M')
        src.little = false;
    else
        return false;
    uint16_t magic = 0;
    uint32_t ifd0 = 0;
    if (!src.U16(2, magic) || magic != 42 || !src.U32(4, ifd0)) return false;
    std::vector<IfdEntry> entries;
    if (!ReadIfd(src, ifd0, entries)) return false;

    std::string fallback;
    uint32_t exifIfd = 0;
    for (const auto& e : entries) {
        if (e.tag == 0x0132) ReadAscii(src, e, fallback);
        if (e.tag == 0x8769) src.U32(e.valueOffset, exifIfd);
    }
    if (exifIfd) {
        std::vector<IfdEntry> exif;
        if (ReadIfd(src, exifIfd, exif)) {
            std::string original, digitized;
            for (const auto& e : exif) {
                if (e.tag == 0x9003) ReadAscii(src, e, original);
                if (e.tag == 0x9004) ReadAscii(src, e, digitized);
            }
            if (ParseExifDate(original, st)) return true;
            if (ParseExifDate(digitized, st)) return true;
        }
    }
    return ParseExifDate(fallback, st);
}

bool ReadExact(HANDLE h, void* buf, DWORD n) {
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, nullptr) && got == n;
}

// Liefert das EXIF-Aufnahmedatum (lokale Zeit) einer JPEG- oder TIFF-Datei.
bool ReadExifDate(const std::wstring& path, SYSTEMTIME& st) {
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    LARGE_INTEGER fileSize{};
    GetFileSizeEx(h, &fileSize);
    uint8_t head[4];
    if (ReadExact(h, head, 4)) {
        if (head[0] == 0xFF && head[1] == 0xD8) {
            // JPEG: Segmente durchlaufen bis APP1 "Exif"
            uint64_t pos = 2;
            for (int guard = 0; guard < 500 && pos + 4 <= (uint64_t)fileSize.QuadPart; ++guard) {
                LARGE_INTEGER li;
                li.QuadPart = (LONGLONG)pos;
                SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
                uint8_t m[4];
                if (!ReadExact(h, m, 4) || m[0] != 0xFF) break;
                uint8_t marker = m[1];
                if (marker == 0xFF) {        // Füllbyte
                    pos += 1;
                    continue;
                }
                if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
                    pos += 2;                // Marker ohne Länge
                    continue;
                }
                if (marker == 0xD9 || marker == 0xDA) break;   // Bildende/Bilddaten: kein EXIF mehr zu erwarten
                uint16_t len = (uint16_t)((m[2] << 8) | m[3]);
                if (len < 2) break;
                if (marker == 0xE1 && len > 8) {
                    std::vector<uint8_t> seg(len - 2);
                    if (!ReadExact(h, seg.data(), (DWORD)seg.size())) break;
                    if (memcmp(seg.data(), "Exif\0\0", 6) == 0) {
                        TiffSource src(seg.data() + 6, seg.size() - 6);
                        ok = ExifDateFromTiff(src, st);
                        if (ok) break;
                    }
                }
                pos += 2 + (uint64_t)len;
            }
        } else if ((head[0] == 'I' && head[1] == 'I' && head[2] == 42 && head[3] == 0) ||
                   (head[0] == 'M' && head[1] == 'M' && head[2] == 0 && head[3] == 42)) {
            TiffSource src(h, (uint64_t)fileSize.QuadPart);
            ok = ExifDateFromTiff(src, st);
        }
    }
    CloseHandle(h);
    return ok;
}

bool IsExifCandidate(const std::wstring& name) {
    std::wstring ext = ToLower(PathExtension(name));
    return ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif" || ext == L".tif" ||
           ext == L".tiff" || ext == L".dng" || ext == L".nef" || ext == L".cr2" || ext == L".arw";
}

// ===================================================================================
// Dialog
// ===================================================================================

constexpr DWORD kSettableMask = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                                FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE;

struct AttrBox {
    int id;
    DWORD flag;
    const wchar_t* text;
};
const AttrBox kAttrBoxes[] = {
    {110, FILE_ATTRIBUTE_READONLY, L"&Schreibgeschützt"},
    {111, FILE_ATTRIBUTE_HIDDEN, L"&Versteckt"},
    {112, FILE_ATTRIBUTE_SYSTEM, L"S&ystem"},
    {113, FILE_ATTRIBUTE_ARCHIVE, L"&Archiv"},
    {114, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, L"&Nicht indiziert"},
};

struct TimeRow {
    int check, date, time;
    const wchar_t* text;
};
const TimeRow kTimeRows[] = {
    {120, 121, 122, L"&Erstellt:"},
    {123, 124, 125, L"&Geändert:"},
    {126, 127, 128, L"&Letzter Zugriff:"},
};

constexpr int kInfo = 101;
constexpr int kNow = 129;
constexpr int kExif = 130;
constexpr int kExifCreated = 131;
constexpr int kRecursive = 132;

// Gewünschte Änderungen
struct ChangeSpec {
    DWORD setMask = 0, clearMask = 0;
    bool setTime[3] = {false, false, false};   // Erstellt, Geändert, Zugriff
    FILETIME time[3] = {};
    bool exif = false, exifCreated = false;
    bool recursive = false;
};

struct ChangeResult {
    int changed = 0;
    int noExif = 0;
    std::vector<std::wstring> errors;
    std::vector<std::wstring> log;
};

class AttributesDlg : public DialogBase {
public:
    std::wstring dir;
    std::vector<std::wstring> names;
    ChangeSpec spec;

protected:
    BOOL OnInit() override {
        // Info-Zeile
        if (names.size() == 1)
            SetText(kInfo, PathCombine(dir, names[0]));
        else
            SetText(kInfo, Format(L"%d Elemente in ", (int)names.size()) + dir);

        // Aktuelle Attribute ermitteln (gleich bei allen -> Häkchen/leer, sonst unbestimmt)
        DWORD andMask = 0xFFFFFFFF, orMask = 0;
        bool anyDir = false, anyValid = false;
        WIN32_FILE_ATTRIBUTE_DATA first{};
        bool haveFirst = false;
        for (const auto& n : names) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!GetFileAttributesExW(LongPath(PathCombine(dir, n)).c_str(), GetFileExInfoStandard, &d)) continue;
            andMask &= d.dwFileAttributes;
            orMask |= d.dwFileAttributes;
            anyValid = true;
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) anyDir = true;
            if (!haveFirst) {
                first = d;
                haveFirst = true;
            }
        }
        for (const auto& b : kAttrBoxes) {
            int state = BST_INDETERMINATE;
            if (anyValid) {
                if (andMask & b.flag)
                    state = BST_CHECKED;
                else if (!(orMask & b.flag))
                    state = BST_UNCHECKED;
            }
            CheckDlgButton(hwnd_, b.id, state);
            initial_[&b - kAttrBoxes] = state;
        }

        // Zeitstempel vorbelegen: bei einem Element mit dessen Werten, sonst mit jetzt
        SYSTEMTIME now;
        GetLocalTime(&now);
        const FILETIME* fts[3] = {&first.ftCreationTime, &first.ftLastWriteTime, &first.ftLastAccessTime};
        for (int i = 0; i < 3; ++i) {
            SYSTEMTIME st = now;
            if (names.size() == 1 && haveFirst) {
                SYSTEMTIME loc;
                if (FileTimeToLocal(*fts[i], loc)) st = loc;
            }
            DateTime_SetSystemtime(Item(kTimeRows[i].date), GDT_VALID, &st);
            DateTime_SetSystemtime(Item(kTimeRows[i].time), GDT_VALID, &st);
        }
        Enable(kRecursive, anyDir);
        UpdateEnabled();
        return TRUE;
    }

    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == kNow) {
            SYSTEMTIME now;
            GetLocalTime(&now);
            for (const auto& r : kTimeRows) {
                DateTime_SetSystemtime(Item(r.date), GDT_VALID, &now);
                DateTime_SetSystemtime(Item(r.time), GDT_VALID, &now);
            }
            // Ohne gewählten Zeitstempel ist „Jetzt“ für das Änderungsdatum gemeint
            if (!IsChecked(kTimeRows[0].check) && !IsChecked(kTimeRows[1].check) && !IsChecked(kTimeRows[2].check))
                SetCheck(kTimeRows[1].check, true);
            UpdateEnabled();
            return TRUE;
        }
        if (code == BN_CLICKED && (id == kTimeRows[0].check || id == kTimeRows[1].check || id == kTimeRows[2].check ||
                                   id == kExif)) {
            UpdateEnabled();
            return TRUE;
        }
        if (id == IDOK) {
            if (!Collect()) {
                MsgInfo(hwnd_, L"Es wurde keine Änderung ausgewählt.");
                return TRUE;
            }
            End(IDOK);
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }

private:
    void UpdateEnabled() {
        for (const auto& r : kTimeRows) {
            bool on = IsChecked(r.check);
            Enable(r.date, on);
            Enable(r.time, on);
        }
        Enable(kExifCreated, IsChecked(kExif));
    }

    // Übernimmt die Dialogwerte in spec; false = nichts zu tun.
    bool Collect() {
        spec = ChangeSpec{};
        for (size_t i = 0; i < std::size(kAttrBoxes); ++i) {
            int st = (int)IsDlgButtonChecked(hwnd_, kAttrBoxes[i].id);
            if (st == initial_[i] || st == BST_INDETERMINATE) continue;   // unverändert
            if (st == BST_CHECKED)
                spec.setMask |= kAttrBoxes[i].flag;
            else
                spec.clearMask |= kAttrBoxes[i].flag;
        }
        for (int i = 0; i < 3; ++i) {
            if (!IsChecked(kTimeRows[i].check)) continue;
            SYSTEMTIME d{}, t{};
            DateTime_GetSystemtime(Item(kTimeRows[i].date), &d);
            DateTime_GetSystemtime(Item(kTimeRows[i].time), &t);
            SYSTEMTIME st = d;
            st.wHour = t.wHour;
            st.wMinute = t.wMinute;
            st.wSecond = t.wSecond;
            st.wMilliseconds = 0;
            if (LocalToFileTime(st, spec.time[i])) spec.setTime[i] = true;
        }
        spec.exif = IsChecked(kExif);
        spec.exifCreated = spec.exif && IsChecked(kExifCreated);
        spec.recursive = IsWindowEnabled(Item(kRecursive)) && IsChecked(kRecursive);
        return spec.setMask || spec.clearMask || spec.setTime[0] || spec.setTime[1] || spec.setTime[2] || spec.exif;
    }

    int initial_[std::size(kAttrBoxes)] = {};
};

// Wendet die Änderungen auf ein Element an.
void ApplyToItem(const std::wstring& path, bool isDir, const ChangeSpec& spec, ChangeResult& res) {
    std::wstring lp = LongPath(path);
    bool changedSomething = false;

    // Zeitstempel (vor den Attributen, falls „Schreibgeschützt“ gesetzt wird – FILE_WRITE_ATTRIBUTES geht aber auch so)
    FILETIME ft[3];
    bool use[3] = {spec.setTime[0], spec.setTime[1], spec.setTime[2]};
    for (int i = 0; i < 3; ++i) ft[i] = spec.time[i];
    if (spec.exif && !isDir && IsExifCandidate(path)) {
        SYSTEMTIME st;
        FILETIME ef;
        if (ReadExifDate(path, st) && LocalToFileTime(st, ef)) {
            ft[1] = ef;
            use[1] = true;
            if (spec.exifCreated) {
                ft[0] = ef;
                use[0] = true;
            }
        } else {
            res.noExif++;
        }
    }
    if (use[0] || use[1] || use[2]) {
        HANDLE h = CreateFileW(lp.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, isDir ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            res.errors.push_back(path + L": " + LastErrorMessage());
        } else {
            if (SetFileTime(h, use[0] ? &ft[0] : nullptr, use[2] ? &ft[2] : nullptr, use[1] ? &ft[1] : nullptr)) {
                std::wstring what;
                const wchar_t* names[3] = {L"Erstellt", L"Geändert", L"Zugriff"};
                for (int i = 0; i < 3; ++i) {
                    if (!use[i]) continue;
                    SYSTEMTIME loc;
                    if (!what.empty()) what += L", ";
                    what += names[i];
                    if (FileTimeToLocal(ft[i], loc)) what += L" " + toolsdetail::FormatLocalSystemTime(loc, true);
                }
                res.log.push_back(L"Zeitstempel geändert: " + path + L" (" + what + L")");
                changedSomething = true;
            } else {
                res.errors.push_back(path + L": " + LastErrorMessage());
            }
            CloseHandle(h);
        }
    }

    // Attribute
    if (spec.setMask || spec.clearMask) {
        DWORD old = GetFileAttributesW(lp.c_str());
        if (old == INVALID_FILE_ATTRIBUTES) {
            res.errors.push_back(path + L": " + LastErrorMessage());
        } else {
            DWORD nw = ((old & kSettableMask) | spec.setMask) & ~spec.clearMask;
            if (nw != (old & kSettableMask)) {
                if (SetFileAttributesW(lp.c_str(), nw ? nw : FILE_ATTRIBUTE_NORMAL)) {
                    res.log.push_back(L"Attribute geändert: " + path + L" (" + FormatAttributes(old) + L" → " +
                                      FormatAttributes(nw | (old & FILE_ATTRIBUTE_DIRECTORY)) + L")");
                    changedSomething = true;
                } else {
                    res.errors.push_back(path + L": " + LastErrorMessage());
                }
            }
        }
    }
    if (changedSomething) res.changed++;
}

} // namespace

bool ChangeAttributes(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names) {
    if (names.empty()) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    DialogTemplate t(L"Eigenschaften ändern", 300, 250);
    t.Label(kInfo, L"", 7, 7, 286, 10, SS_PATHELLIPSIS);

    t.Group(-1, L"Attribute", 7, 21, 286, 46);
    for (size_t i = 0; i < std::size(kAttrBoxes); ++i) {
        int col = (int)(i % 3), row = (int)(i / 3);
        t.Add(kAttrBoxes[i].id, L"Button", kAttrBoxes[i].text, 15 + col * 92, 33 + row * 14, 88, 10,
              WS_TABSTOP | BS_AUTO3STATE);
    }

    t.Group(-1, L"Zeitstempel (lokale Zeit)", 7, 72, 286, 76);
    for (int i = 0; i < 3; ++i) {
        int y = 85 + i * 18;
        t.Check(kTimeRows[i].check, kTimeRows[i].text, 15, y + 2, 72, 10);
        t.DateTime(kTimeRows[i].date, 90, y, 80, 13, DTS_SHORTDATECENTURYFORMAT);
        t.DateTime(kTimeRows[i].time, 175, y, 60, 13, DTS_TIMEFORMAT | DTS_UPDOWN);
    }
    t.Button(kNow, L"&Jetzt", 240, 85, 46, 14);

    t.Check(kExif, L"Datum aus E&XIF-Aufnahmedatum übernehmen (JPEG/TIFF) – setzt das Änderungsdatum", 7, 154, 286, 10);
    t.Check(kExifCreated, L"auch das Erstellungsdatum setzen", 19, 167, 274, 10);
    t.Check(kRecursive, L"&Unterverzeichnisse einbeziehen", 7, 184, 286, 10);
    t.Label(-1, L"Kästchen mit drei Zuständen: Häkchen = setzen, leer = löschen, grau = unverändert.", 7, 202, 286, 18);
    t.DefButton(IDOK, L"OK", 186, 229, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 243, 229, 50, 14);

    AttributesDlg dlg;
    dlg.dir = dir;
    dlg.names = names;
    if (dlg.DoModal(owner, t) != IDOK) return false;

    const ChangeSpec spec = dlg.spec;
    ChangeResult res;
    bool completed = toolsdetail::RunWithProgress(
        owner, L"Eigenschaften ändern", L"Wird bearbeitet …", [&](toolsdetail::ProgressSink& sink) {
            for (const auto& n : names) {
                if (sink.Cancelled()) return;
                std::wstring full = PathCombine(dir, n);
                DWORD a = GetFileAttributesW(LongPath(full).c_str());
                if (a == INVALID_FILE_ATTRIBUTES) {
                    res.errors.push_back(full + L": " + LastErrorMessage());
                    continue;
                }
                bool isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
                sink.SetText(full);
                ApplyToItem(full, isDir, spec, res);
                if (isDir && spec.recursive && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    WalkDirectory(full, [&](const std::wstring& path, const DirEntry& e) {
                        if (sink.Cancelled()) return false;
                        sink.SetText(path);
                        ApplyToItem(path, e.IsDir(), spec, res);
                        return true;
                    });
                }
            }
        });

    // Protokoll im Hauptthread schreiben
    for (const auto& line : res.log) LogOperation(line);

    std::wstring msg;
    if (!completed) msg = L"Der Vorgang wurde abgebrochen.\n";
    if (spec.exif && res.noExif > 0)
        msg += Format(L"Bei %d Datei(en) wurde kein EXIF-Aufnahmedatum gefunden.\n", res.noExif);
    if (!res.errors.empty()) {
        msg += Format(L"%d Fehler:\n", (int)res.errors.size());
        size_t shown = std::min<size_t>(res.errors.size(), 20);
        for (size_t i = 0; i < shown; ++i) msg += res.errors[i] + L"\n";
        if (res.errors.size() > shown) msg += L"…\n";
        MsgError(owner, msg);
    } else if (!msg.empty()) {
        MsgInfo(owner, msg);
    }
    if (res.changed > 0) App::RefreshPanes();
    return res.changed > 0;
}

} // namespace qf
