// SplitJoin.cpp – Modul D: Datei teilen / Dateien zusammenfügen
//
// Teilen: "Name.ext" -> "Name.ext.001", "Name.ext.002", … (mindestens drei Ziffern, ab 1000 entsprechend mehr).
// Optional:
//   - "Name.ext.bat": Batchdatei (UTF-8 ohne BOM, "chcp 65001"), fügt die Teile per "copy /b" ohne QFiles zusammen
//   - "Name.ext.crc": Prüfsummendatei im SFV-Format (eine Zeile "Teilname CRC32" je Teil) plus Kommentarzeile
//     "; QFILES-ORIGINAL <Größe> <CRC32>" mit der Prüfsumme der Originaldatei
// Zusammenfügen: sucht ab "…001" alle fortlaufenden Teile, schreibt zunächst in eine temporäre Datei im
// Zielverzeichnis und benennt sie am Ende um. Ist eine .crc/.sfv-Datei vorhanden, werden die Teile und
// das Ergebnis geprüft.
//
// Beide Vorgänge laufen in einem Arbeitsthread (Fortschrittsbalken, Abbruch).

#include "Modules.h"
#include "App.h"
#include "Dialog.h"
#include "Encoding.h"
#include "FileOps.h"
#include "Settings.h"
#include "Util.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace qf {

namespace {

constexpr wchar_t kSplitSection[] = L"Teilen";
constexpr UINT kMsgProgress = WM_APP + 1;
constexpr UINT kMsgDone = WM_APP + 2;
constexpr DWORD kBlock = 4u << 20;     // 4 MB Kopierpuffer
constexpr uint64_t kMaxParts = 99999;

enum Result : int { ResRunning = 0, ResOk = 1, ResAborted = 2, ResError = 3, ResCrcMismatch = 4 };

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3, Polynom 0xEDB88320), Slicing-by-8
// ---------------------------------------------------------------------------

struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int k = 1; k < 8; ++k) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
    }
};

// Fortsetzbar: crc = Crc32Update(crc, …) beginnend mit 0.
uint32_t Crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
    static const CrcTables T;
    crc = ~crc;
    while (n >= 8) {
        uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = T.t[7][a & 0xFF] ^ T.t[6][(a >> 8) & 0xFF] ^ T.t[5][(a >> 16) & 0xFF] ^ T.t[4][a >> 24] ^
              T.t[3][b & 0xFF] ^ T.t[2][(b >> 8) & 0xFF] ^ T.t[1][(b >> 16) & 0xFF] ^ T.t[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) crc = T.t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::wstring CrcHex(uint32_t c) { return Format(L"%08X", (unsigned)c); }

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------

std::wstring PartName(const std::wstring& base, uint64_t n, size_t width = 3) {
    std::wstring num = std::to_wstring(n);
    if (num.size() < width) num.insert(0, width - num.size(), L'0');
    return base + L"." + num;
}

HANDLE OpenForRead(const std::wstring& path) {
    return CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

bool ReadFull(HANDLE h, uint8_t* p, DWORD n, DWORD& got) {
    got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(h, p + got, n - got, &r, nullptr)) return false;
        if (r == 0) break;
        got += r;
    }
    return true;
}

bool WriteAll(HANDLE h, const uint8_t* p, DWORD n) {
    DWORD done = 0;
    while (done < n) {
        DWORD w = 0;
        if (!WriteFile(h, p + done, n - done, &w, nullptr) || w == 0) return false;
        done += w;
    }
    return true;
}

// Freier Speicher im Verzeichnis (UINT64_MAX, wenn unbekannt)
uint64_t FreeSpace(const std::wstring& dir) {
    ULARGE_INTEGER avail{};
    std::wstring d = dir;
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    if (GetDiskFreeSpaceExW(LongPath(d).c_str(), &avail, nullptr, nullptr)) return avail.QuadPart;
    return UINT64_MAX;
}

// Zahl mit Komma oder Punkt als Dezimaltrennzeichen; -1 bei Fehler
double ParseNumber(const std::wstring& s) {
    std::wstring t = Trim(s);
    if (t.empty()) return -1;
    for (auto& c : t)
        if (c == L',') c = L'.';
    wchar_t* end = nullptr;
    double v = wcstod(t.c_str(), &end);
    if (!end || *end != 0 || v < 0) return -1;
    return v;
}

// Für Batchdateien: '%' muss verdoppelt werden (in Anführungszeichen sind & | < > ( ) bereits geschützt).
std::wstring BatchQuote(const std::wstring& name) { return L"\"" + ReplaceAll(name, L"%", L"%%") + L"\""; }

std::wstring BuildBatch(const std::wstring& base, uint64_t count) {
    const std::wstring target = BatchQuote(base);
    std::wstring b;
    b += L"@echo off\r\n";
    b += L"setlocal\r\n";
    b += L"for /f \"tokens=2 delims=:.\" %%a in ('chcp') do set \"QF_CP=%%a\"\r\n";
    b += L"chcp 65001 >nul\r\n";
    b += L"rem Fügt die Teile von " + target + L" wieder zusammen (erzeugt von QFiles).\r\n";
    b += L"cd /d \"%~dp0\"\r\n";
    b += L"if exist " + target + L" (\r\n";
    b += L"  echo Die Datei " + target + L" existiert bereits.\r\n";
    b += L"  goto ende\r\n";
    b += L")\r\n";
    b += L"echo Teile werden zusammengefügt …\r\n";
    // Befehlszeilen höchstens ca. 6000 Zeichen. Passen nicht alle Teile in eine Zeile, schreibt jede Zeile
    // in eine neue Zwischendatei (abwechselnd zwei Namen) – ohne an die eigene Zieldatei anzuhängen.
    const std::wstring tmp[2] = {BatchQuote(base + L".qfjoin1"), BatchQuote(base + L".qfjoin2")};
    std::vector<std::wstring> lines; // jeweils die Liste der Quellen einer Zeile
    for (uint64_t n = 1; n <= count;) {
        std::wstring src;
        bool firstPart = true;
        while (n <= count) {
            std::wstring part = BatchQuote(PartName(base, n));
            if (!firstPart && src.size() + part.size() > 6000) break;
            if (!firstPart) src += L" + ";
            src += part;
            firstPart = false;
            ++n;
        }
        lines.push_back(std::move(src));
    }
    if (lines.size() == 1) {
        b += L"copy /b " + lines[0] + L" " + target + L" >nul || goto fehler\r\n";
    } else {
        for (size_t k = 0; k < lines.size(); ++k) {
            const std::wstring& out = tmp[k % 2];
            if (k == 0) {
                b += L"copy /b " + lines[k] + L" " + out + L" >nul || goto fehler\r\n";
            } else {
                const std::wstring& prev = tmp[(k + 1) % 2];
                b += L"copy /b " + prev + L" + " + lines[k] + L" " + out + L" >nul || goto fehler\r\n";
                b += L"del " + prev + L"\r\n";
            }
        }
        b += L"ren " + tmp[(lines.size() - 1) % 2] + L" " + target + L" || goto fehler\r\n";
    }
    b += L"echo Fertig: " + target + L"\r\n";
    b += L"goto ende\r\n";
    b += L":fehler\r\n";
    b += L"echo Fehler beim Zusammenfügen.\r\n";
    b += L"if exist " + tmp[0] + L" del " + tmp[0] + L"\r\n";
    b += L"if exist " + tmp[1] + L" del " + tmp[1] + L"\r\n";
    b += L":ende\r\n";
    b += L"if defined QF_CP chcp %QF_CP% >nul\r\n";
    b += L"endlocal\r\n";
    b += L"pause\r\n";
    return b;
}

struct CrcInfo {
    bool loaded = false;
    std::unordered_map<std::wstring, uint32_t> parts; // Teilname (groß) -> CRC
    bool hasOriginal = false;
    uint64_t originalSize = 0;
    uint32_t originalCrc = 0;
};

bool ParseHex32(const std::wstring& s, uint32_t& v) {
    std::wstring t = Trim(s);
    if (t.size() != 8) return false;
    v = 0;
    for (wchar_t c : t) {
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= (uint32_t)(c - L'0');
        else if (c >= L'a' && c <= L'f') v |= (uint32_t)(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') v |= (uint32_t)(c - L'A' + 10);
        else return false;
    }
    return true;
}

bool LoadCrcFile(const std::wstring& path, CrcInfo& info) {
    std::vector<uint8_t> data;
    if (!ReadFileBytes(path, data, 16u << 20)) return false;
    size_t bom = 0;
    TextEncoding enc = DetectEncoding(data.data(), data.size(), &bom);
    std::wstring text = DecodeText(data.data(), data.size(), enc);
    for (auto& raw : Split(text, L'\n')) {
        std::wstring line = Trim(raw);
        if (line.empty()) continue;
        if (line[0] == L';') {
            const std::wstring tag = L"; QFILES-ORIGINAL ";
            if (StartsWithI(line, tag)) {
                auto f = Split(line.substr(tag.size()), L' ');
                uint32_t c = 0;
                if (f.size() >= 2 && ParseHex32(f[1], c)) {
                    info.hasOriginal = true;
                    info.originalSize = (uint64_t)StrToInt(f[0], 0);
                    info.originalCrc = c;
                }
            }
            continue;
        }
        size_t pos = line.find_last_of(L" \t");
        if (pos == std::wstring::npos) continue;
        uint32_t c = 0;
        if (!ParseHex32(line.substr(pos + 1), c)) continue;
        std::wstring name = Trim(line.substr(0, pos));
        if (!name.empty()) info.parts[ToUpper(name)] = c;
    }
    info.loaded = true;
    return true;
}

void RefreshIfNeeded() {
    if (!App::Opt().autoRefresh) App::RefreshPanes();
}

// ---------------------------------------------------------------------------
// Gemeinsame Basis: Arbeitsthread mit Fortschritt
// ---------------------------------------------------------------------------

class WorkerDialog : public DialogBase {
protected:

    void Tick(bool force) {
        ULONGLONG now = GetTickCount64();
        if (!force && now - lastPost_ < 100) return;
        lastPost_ = now;
        if (!postPending_.exchange(true)) PostMessageW(target_, kMsgProgress, 0, 0);
    }
    void SetStatusFromWorker(const std::wstring& s) {
        std::lock_guard<std::mutex> lock(mtx_);
        status_ = s;
    }
    void Fail(const std::wstring& msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        error_ = msg;
    }
    void ShowProgress(int progressId, int statusId) {
        postPending_ = false;
        SendMessageW(Item(progressId), PBM_SETPOS, (WPARAM)permille_.load(), 0);
        std::wstring s;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            s = status_;
        }
        SetText(statusId, s);
    }
    void JoinWorker() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
    }
    std::wstring ErrorText() {
        std::lock_guard<std::mutex> lock(mtx_);
        return error_;
    }
    void ResetWorkerState() {
        stop_ = false;
        postPending_ = false;
        permille_ = 0;
        result_ = ResRunning;
        lastPost_ = 0;
        target_ = hwnd_;
        std::lock_guard<std::mutex> lock(mtx_);
        status_.clear();
        error_.clear();
    }

    std::thread worker_;
    HWND target_ = nullptr;
    ULONGLONG lastPost_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> postPending_{false};
    std::atomic<int> permille_{0};
    std::atomic<int> result_{ResRunning};
    std::mutex mtx_;
    std::wstring status_, error_;
    bool running_ = false;
    bool reported_ = false;
};

// ---------------------------------------------------------------------------
// Teilen
// ---------------------------------------------------------------------------

enum : int {
    SID_LBL_SRC = 101,
    SID_SRC,
    SID_LBL_SIZE,
    SID_SIZE,
    SID_LBL_TARGET,
    SID_TARGET,
    SID_BROWSE,
    SID_GROUP,
    SID_R_PRESET,
    SID_PRESET,
    SID_R_CUSTOM,
    SID_CUSTOM,
    SID_UNIT,
    SID_R_COUNT,
    SID_COUNT,
    SID_INFO,
    SID_BATCH,
    SID_CRC,
    SID_PROGRESS,
    SID_STATUS,
};

struct Preset {
    const wchar_t* label;
    uint64_t bytes;
};
const Preset kPresets[] = {
    {L"10 MB", 10ull << 20},
    {L"100 MB", 100ull << 20},
    {L"650 MB (CD)", 650ull << 20},
    {L"700 MB (CD)", 700ull << 20},
    {L"2 GB", 2ull << 30},
    {L"4 GB − 1 Byte (FAT32)", 4294967295ull},
    {L"4,7 GB (DVD)", 4700372992ull},
    {L"8,5 GB (DVD DL)", 8543666176ull},
    {L"25 GB (BD)", 25025314816ull},
};
const wchar_t* kUnits[] = {L"Bytes", L"KB", L"MB", L"GB"};
const uint64_t kUnitFactor[] = {1ull, 1ull << 10, 1ull << 20, 1ull << 30};

struct SplitJob {
    std::wstring src, dir, base;
    uint64_t size = 0, partSize = 0, count = 0;
    bool batch = false, crc = false;
};

class SplitDialog : public WorkerDialog {
public:
    SplitDialog(const std::wstring& file, const std::wstring& targetDir) : file_(file), targetDir_(targetDir) {}
    bool Succeeded() const { return success_; }

protected:
    BOOL OnInit() override;
    BOOL OnCommand(int id, int code, HWND ctl) override;
    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnDestroy() override;

private:
    int Mode() const { return IsChecked(SID_R_CUSTOM) ? 1 : (IsChecked(SID_R_COUNT) ? 2 : 0); }
    uint64_t PartSize() const;
    uint64_t PartCount(uint64_t ps) const { return ps ? (fileSize_ + ps - 1) / ps : 0; }
    void UpdateInfo();
    void UpdateModeControls();
    void SetRunning(bool on);
    void Start();
    void Run(SplitJob job);
    int RunInner(const SplitJob& job, std::vector<std::wstring>& created);
    void Report();
    void SaveSettings();

    std::wstring file_, targetDir_;
    uint64_t fileSize_ = 0;
    bool success_ = false;
    SplitJob job_;
};

BOOL SplitDialog::OnInit() {
    fileSize_ = GetFileSize64(file_);
    SetText(SID_SRC, file_);
    SetText(SID_SIZE, FormatSize(fileSize_) + L" (" + FormatSizeBytes(fileSize_) + L" Bytes)");
    SetText(SID_TARGET, targetDir_);
    for (auto& p : kPresets) ComboAdd(SID_PRESET, p.label);
    for (auto* u : kUnits) ComboAdd(SID_UNIT, u);
    SendMessageW(Item(SID_PROGRESS), PBM_SETRANGE32, 0, 1000);

    Config& c = App::Cfg();
    int mode = std::clamp(c.GetInt(kSplitSection, L"Modus", 0), 0, 2);
    ComboSetSel(SID_PRESET, std::clamp(c.GetInt(kSplitSection, L"Vorgabe", 1), 0, (int)(sizeof(kPresets) / sizeof(kPresets[0])) - 1));
    SetText(SID_CUSTOM, c.Get(kSplitSection, L"Wert", L"50"));
    ComboSetSel(SID_UNIT, std::clamp(c.GetInt(kSplitSection, L"Einheit", 2), 0, 3));
    SetText(SID_COUNT, c.Get(kSplitSection, L"Anzahl", L"2"));
    SetCheck(SID_BATCH, c.GetBool(kSplitSection, L"Batch", false));
    SetCheck(SID_CRC, c.GetBool(kSplitSection, L"Pruefsumme", true));
    SetCheck(SID_R_PRESET, mode == 0);
    SetCheck(SID_R_CUSTOM, mode == 1);
    SetCheck(SID_R_COUNT, mode == 2);
    UpdateModeControls();
    UpdateInfo();
    return TRUE;
}

void SplitDialog::SaveSettings() {
    Config& c = App::Cfg();
    c.SetInt(kSplitSection, L"Modus", Mode());
    c.SetInt(kSplitSection, L"Vorgabe", ComboSel(SID_PRESET));
    c.Set(kSplitSection, L"Wert", GetText(SID_CUSTOM));
    c.SetInt(kSplitSection, L"Einheit", ComboSel(SID_UNIT));
    c.Set(kSplitSection, L"Anzahl", GetText(SID_COUNT));
    c.SetBool(kSplitSection, L"Batch", IsChecked(SID_BATCH));
    c.SetBool(kSplitSection, L"Pruefsumme", IsChecked(SID_CRC));
}

uint64_t SplitDialog::PartSize() const {
    switch (Mode()) {
    case 0: {
        int i = ComboSel(SID_PRESET);
        if (i < 0 || i >= (int)(sizeof(kPresets) / sizeof(kPresets[0]))) return 0;
        return kPresets[i].bytes;
    }
    case 1: {
        double v = ParseNumber(GetText(SID_CUSTOM));
        int u = ComboSel(SID_UNIT);
        if (v <= 0 || u < 0 || u > 3) return 0;
        double bytes = v * (double)kUnitFactor[u];
        if (bytes < 1 || bytes > 1.8e19) return 0;
        return (uint64_t)bytes;
    }
    default: {
        long long n = GetInt(SID_COUNT, 0);
        if (n < 1 || (uint64_t)n > fileSize_) return 0;
        return (fileSize_ + (uint64_t)n - 1) / (uint64_t)n;
    }
    }
}

void SplitDialog::UpdateModeControls() {
    int m = Mode();
    if (running_) return;
    Enable(SID_PRESET, m == 0);
    Enable(SID_CUSTOM, m == 1);
    Enable(SID_UNIT, m == 1);
    Enable(SID_COUNT, m == 2);
}

void SplitDialog::UpdateInfo() {
    uint64_t ps = PartSize();
    uint64_t n = PartCount(ps);
    std::wstring info;
    bool ok = false;
    if (fileSize_ == 0 || fileSize_ == UINT64_MAX)
        info = L"Die Datei ist leer oder kann nicht gelesen werden.";
    else if (ps == 0)
        info = L"Bitte eine gültige Teilgröße bzw. Anzahl angeben.";
    else if (n < 2)
        info = L"Die Datei ist nicht größer als die Teilgröße – es gibt nichts zu teilen.";
    else if (n > kMaxParts)
        info = L"Zu viele Teile (" + IntToStrGrouped(n) + L"; höchstens " + IntToStrGrouped(kMaxParts) + L").";
    else {
        uint64_t last = fileSize_ - ps * (n - 1);
        info = L"Ergibt " + IntToStrGrouped(n) + L" Teile zu je " + FormatSize(ps);
        if (last != ps) info += L" (letzter Teil: " + FormatSize(last) + L")";
        info += L".";
        ok = true;
    }
    SetText(SID_INFO, info);
    if (!running_) Enable(IDOK, ok);
}

void SplitDialog::SetRunning(bool on) {
    running_ = on;
    static const int ids[] = {SID_TARGET, SID_BROWSE, SID_R_PRESET, SID_PRESET, SID_R_CUSTOM, SID_CUSTOM,
                              SID_UNIT,   SID_R_COUNT, SID_COUNT,   SID_BATCH,  SID_CRC};
    for (int id : ids) Enable(id, !on);
    Enable(IDOK, !on);
    if (!on) {
        UpdateModeControls();
        UpdateInfo();
    }
}

void SplitDialog::Start() {
    if (running_) return;
    uint64_t ps = PartSize();
    uint64_t n = PartCount(ps);
    if (ps == 0 || n < 2 || n > kMaxParts) return;
    std::wstring dir = Trim(GetText(SID_TARGET));
    if (dir.empty() || !DirExists(dir)) {
        MsgError(hwnd_, L"Das Zielverzeichnis „" + dir + L"“ existiert nicht.");
        SetFocus(Item(SID_TARGET));
        return;
    }
    dir = NormalizeDir(dir);
    SplitJob j;
    j.src = file_;
    j.dir = dir;
    j.base = PathFileName(file_);
    j.size = fileSize_;
    j.partSize = ps;
    j.count = n;
    j.batch = IsChecked(SID_BATCH);
    j.crc = IsChecked(SID_CRC);

    // Vorhandene Teile/Zusatzdateien?
    std::vector<std::wstring> exist;
    for (uint64_t i = 1; i <= n && exist.size() < 5; ++i)
        if (PathExists(PathCombine(dir, PartName(j.base, i)))) exist.push_back(PartName(j.base, i));
    if (j.batch && PathExists(PathCombine(dir, j.base + L".bat"))) exist.push_back(j.base + L".bat");
    if (j.crc && PathExists(PathCombine(dir, j.base + L".crc"))) exist.push_back(j.base + L".crc");
    if (!exist.empty() &&
        !MsgConfirm(hwnd_, L"Im Zielverzeichnis sind bereits Dateien vorhanden, z. B.:\n\n" + Join(exist, L"\n") +
                               L"\n\nÜberschreiben?"))
        return;
    uint64_t free = FreeSpace(dir);
    if (free != UINT64_MAX && free < fileSize_ &&
        !MsgConfirm(hwnd_, L"Auf dem Ziellaufwerk sind nur " + FormatSize(free) + L" frei, benötigt werden " +
                               FormatSize(fileSize_) + L".\n\nTrotzdem fortfahren?"))
        return;

    SaveSettings();
    job_ = j;
    ResetWorkerState();
    reported_ = false;
    SetRunning(true);
    SetText(SID_STATUS, L"Wird geteilt …");
    worker_ = std::thread([this, j]() { Run(j); });
}

int SplitDialog::RunInner(const SplitJob& j, std::vector<std::wstring>& created) {
    HANDLE src = OpenForRead(j.src);
    if (src == INVALID_HANDLE_VALUE) {
        Fail(L"Die Quelldatei kann nicht geöffnet werden:\n" + LastErrorMessage());
        return ResError;
    }
    std::vector<uint8_t> buf(kBlock);
    std::vector<uint32_t> partCrc;
    uint32_t total = 0;
    uint64_t done = 0;
    int res = ResOk;
    for (uint64_t i = 1; i <= j.count && res == ResOk; ++i) {
        const std::wstring name = PartName(j.base, i);
        const std::wstring path = PathCombine(j.dir, name);
        SetStatusFromWorker(L"Teil " + IntToStrGrouped(i) + L" von " + IntToStrGrouped(j.count) + L": " + name);
        Tick(true);
        HANDLE out = CreateFileW(LongPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            Fail(L"Der Teil „" + name + L"“ kann nicht angelegt werden:\n" + LastErrorMessage());
            res = ResError;
            break;
        }
        created.push_back(path);
        uint64_t remain = std::min<uint64_t>(j.partSize, j.size - done);
        uint32_t pc = 0;
        while (remain > 0) {
            if (stop_) {
                res = ResAborted;
                break;
            }
            DWORD want = (DWORD)std::min<uint64_t>(kBlock, remain);
            DWORD got = 0;
            if (!ReadFull(src, buf.data(), want, got) || got == 0) {
                Fail(L"Fehler beim Lesen der Quelldatei:\n" + LastErrorMessage());
                res = ResError;
                break;
            }
            if (!WriteAll(out, buf.data(), got)) {
                Fail(L"Fehler beim Schreiben von „" + name + L"“ (Datenträger voll?):\n" + LastErrorMessage());
                res = ResError;
                break;
            }
            pc = Crc32Update(pc, buf.data(), got);
            total = Crc32Update(total, buf.data(), got);
            remain -= got;
            done += got;
            permille_ = (int)(done * 1000 / (j.size ? j.size : 1));
            Tick(false);
        }
        CloseHandle(out);
        partCrc.push_back(pc);
    }
    CloseHandle(src);
    if (res != ResOk) return res;

    if (j.batch) {
        SetStatusFromWorker(L"Batchdatei wird geschrieben …");
        std::string u = WideToUtf8(BuildBatch(j.base, j.count));
        std::wstring path = PathCombine(j.dir, j.base + L".bat");
        if (!WriteFileBytes(path, u.data(), u.size())) {
            Fail(L"Die Batchdatei kann nicht geschrieben werden:\n" + LastErrorMessage());
            return ResError;
        }
        created.push_back(path);
    }
    if (j.crc) {
        SetStatusFromWorker(L"Prüfsummendatei wird geschrieben …");
        std::wstring t;
        t += L"; Prüfsummen (CRC32), erzeugt von QFiles – SFV-kompatibel\r\n";
        t += L"; Originaldatei: " + j.base + L"\r\n";
        t += L"; QFILES-ORIGINAL " + std::to_wstring(j.size) + L" " + CrcHex(total) + L"\r\n";
        for (size_t k = 0; k < partCrc.size(); ++k) t += PartName(j.base, k + 1) + L" " + CrcHex(partCrc[k]) + L"\r\n";
        std::string u = WideToUtf8(t);
        std::wstring path = PathCombine(j.dir, j.base + L".crc");
        if (!WriteFileBytes(path, u.data(), u.size())) {
            Fail(L"Die Prüfsummendatei kann nicht geschrieben werden:\n" + LastErrorMessage());
            return ResError;
        }
        created.push_back(path);
    }
    return ResOk;
}

void SplitDialog::Run(SplitJob j) {
    std::vector<std::wstring> created;
    int res = RunInner(j, created);
    if (res != ResOk) {
        // Abbruch/Fehler: alle in diesem Lauf erzeugten (unvollständigen) Dateien löschen
        for (auto& p : created) DeleteFileW(LongPath(p).c_str());
    }
    permille_ = res == ResOk ? 1000 : permille_.load();
    result_ = res;
    PostMessageW(target_, kMsgDone, 0, 0);
}

void SplitDialog::Report() {
    if (reported_ || result_ != ResOk) return;
    reported_ = true;
    success_ = true;
    std::wstring extra;
    if (job_.batch) extra += L", Batchdatei";
    if (job_.crc) extra += L", Prüfsummendatei";
    LogOperation(L"Datei geteilt: " + job_.src + L" → " + IntToStrGrouped(job_.count) + L" Teile in " + job_.dir + extra);
    RefreshIfNeeded();
}

BOOL SplitDialog::OnCommand(int id, int code, HWND ctl) {
    switch (id) {
    case SID_R_PRESET:
    case SID_R_CUSTOM:
    case SID_R_COUNT:
        UpdateModeControls();
        UpdateInfo();
        return TRUE;
    case SID_PRESET:
    case SID_UNIT:
        if (code == CBN_SELCHANGE) UpdateInfo();
        return TRUE;
    case SID_CUSTOM:
    case SID_COUNT:
        if (code == EN_CHANGE) UpdateInfo();
        return TRUE;
    case SID_BROWSE: {
        std::wstring d = BrowseForFolder(hwnd_, L"Zielverzeichnis für die Teile", GetText(SID_TARGET));
        if (!d.empty()) SetText(SID_TARGET, d);
        return TRUE;
    }
    case IDOK:
        Start();
        return TRUE;
    case IDCANCEL:
        if (running_) {
            stop_ = true;
            SetText(SID_STATUS, L"Wird abgebrochen …");
            return TRUE;
        }
        End(IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR SplitDialog::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgProgress) {
        ShowProgress(SID_PROGRESS, SID_STATUS);
        return TRUE;
    }
    if (msg == kMsgDone) {
        if (worker_.joinable()) worker_.join();
        ShowProgress(SID_PROGRESS, SID_STATUS);
        int res = result_;
        if (res == ResOk) {
            Report();
            MsgInfo(hwnd_, L"Die Datei wurde in " + IntToStrGrouped(job_.count) + L" Teile zerlegt.");
            End(IDOK);
            return TRUE;
        }
        SetRunning(false);
        SendMessageW(Item(SID_PROGRESS), PBM_SETPOS, 0, 0);
        if (res == ResAborted) {
            SetText(SID_STATUS, L"Abgebrochen – die erzeugten Teile wurden gelöscht.");
        } else {
            SetText(SID_STATUS, L"Fehler – die erzeugten Teile wurden gelöscht.");
            MsgError(hwnd_, ErrorText());
        }
        return TRUE;
    }
    return FALSE;
}

void SplitDialog::OnDestroy() {
    JoinWorker();
    Report(); // falls der Dialog genau beim Abschluss geschlossen wurde
}

// ---------------------------------------------------------------------------
// Zusammenfügen
// ---------------------------------------------------------------------------

enum : int {
    JID_LBL_FIRST = 101,
    JID_FIRST,
    JID_LBL_PARTS,
    JID_PARTS,
    JID_TOTAL,
    JID_LBL_TARGET,
    JID_TARGET,
    JID_BROWSE,
    JID_CHECKCRC,
    JID_PROGRESS,
    JID_STATUS,
};

enum PartCheck : int { PartUnknown = 0, PartOk = 1, PartBad = 2, PartNoEntry = 3 };

bool FindParts(const std::wstring& first, std::wstring& baseName, std::vector<std::wstring>& parts,
               std::vector<uint64_t>& sizes, std::wstring& err) {
    std::wstring name = PathFileName(first);
    std::wstring dir = PathParent(first);
    size_t pos = name.find_last_of(L'.');
    std::wstring digits = (pos == std::wstring::npos) ? L"" : name.substr(pos + 1);
    bool allDigits = !digits.empty() && digits.size() <= 6 &&
                     std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (pos == std::wstring::npos || pos == 0 || !allDigits) {
        err = L"„" + name + L"“ ist kein Teil einer geteilten Datei (erwartet: Name.001).";
        return false;
    }
    size_t width = digits.size();
    baseName = name.substr(0, pos);
    uint64_t n = 1;
    if (!FileExists(PathCombine(dir, PartName(baseName, 1, width)))) n = (uint64_t)StrToInt(digits, 1);
    for (;; ++n) {
        std::wstring p = PathCombine(dir, PartName(baseName, n, width));
        if (!FileExists(p)) break;
        parts.push_back(p);
        sizes.push_back(GetFileSize64(p));
        if (parts.size() > kMaxParts) break;
    }
    if (parts.empty()) {
        err = L"Es wurden keine Teile von „" + baseName + L"“ gefunden.";
        return false;
    }
    return true;
}

struct JoinJob {
    std::vector<std::wstring> parts;
    std::vector<uint64_t> sizes;
    std::wstring temp, target;
    bool check = false;
    CrcInfo crc;
};

class JoinDialog : public WorkerDialog {
public:
    JoinDialog(const std::wstring& firstPart, const std::wstring& targetDir, std::wstring baseName,
               std::vector<std::wstring> parts, std::vector<uint64_t> sizes)
        : first_(firstPart), targetDir_(targetDir), base_(std::move(baseName)), parts_(std::move(parts)),
          sizes_(std::move(sizes)) {}
    bool Succeeded() const { return success_; }

protected:
    BOOL OnInit() override;
    BOOL OnCommand(int id, int code, HWND ctl) override;
    INT_PTR OnNotify(NMHDR* nm) override;
    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnDestroy() override;

private:
    void SetRunning(bool on);
    void Start();
    void Run(JoinJob job);
    int RunInner(JoinJob& job);
    bool Finish(); // temporäre Datei -> Ziel; protokollieren
    void UpdateTotal();

    std::wstring first_, targetDir_, base_;
    std::vector<std::wstring> parts_;
    std::vector<uint64_t> sizes_;
    std::vector<int> partCheck_;      // geschützt durch mtx_ während des Laufs
    std::wstring crcFile_;
    CrcInfo crc_;
    bool success_ = false;
    bool totalCrcOk_ = true;
    bool totalCrcChecked_ = false;
    std::wstring temp_, target_path_;
    std::wstring dispBuf_;
};

BOOL JoinDialog::OnInit() {
    LvAddColumn(JID_PARTS, L"Teil", 170);
    LvAddColumn(JID_PARTS, L"Größe", 65, LVCFMT_RIGHT);
    LvAddColumn(JID_PARTS, L"Prüfsumme", 60);
    HWND lv = Item(JID_PARTS);
    for (size_t i = 0; i < parts_.size(); ++i) {
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)i;
        it.lParam = (LPARAM)i;
        it.pszText = LPSTR_TEXTCALLBACKW;
        int row = ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, row, 1, LPSTR_TEXTCALLBACKW);
        ListView_SetItemText(lv, row, 2, LPSTR_TEXTCALLBACKW);
    }
    partCheck_.assign(parts_.size(), PartUnknown);
    SetText(JID_FIRST, first_);
    std::wstring dir = targetDir_.empty() ? PathParent(first_) : targetDir_;
    SetText(JID_TARGET, PathCombine(dir, base_));
    SendMessageW(Item(JID_PROGRESS), PBM_SETRANGE32, 0, 1000);

    // Prüfsummendatei suchen
    std::wstring partsDir = PathParent(first_);
    for (const wchar_t* ext : {L".crc", L".sfv"}) {
        std::wstring p = PathCombine(partsDir, base_ + ext);
        if (FileExists(p) && LoadCrcFile(p, crc_)) {
            crcFile_ = p;
            break;
        }
    }
    if (!crcFile_.empty()) {
        SetText(JID_CHECKCRC, L"&Prüfsummen prüfen (" + PathFileName(crcFile_) + L")");
        SetCheck(JID_CHECKCRC, true);
    } else {
        SetText(JID_CHECKCRC, L"Prüfsummen prüfen (keine Prüfsummendatei gefunden)");
        Enable(JID_CHECKCRC, false);
    }
    UpdateTotal();

    SetAnchor(JID_FIRST, AnchorTopLeftRight);
    SetAnchor(JID_PARTS, AnchorAll);
    SetAnchor(JID_TOTAL, AnchorBottomLeftRight);
    SetAnchor(JID_LBL_TARGET, AnchorBottomLeft);
    SetAnchor(JID_TARGET, AnchorBottomLeftRight);
    SetAnchor(JID_BROWSE, AnchorBottomRight);
    SetAnchor(JID_CHECKCRC, AnchorBottomLeftRight);
    SetAnchor(JID_PROGRESS, AnchorBottomLeftRight);
    SetAnchor(JID_STATUS, AnchorBottomLeftRight);
    SetAnchor(IDOK, AnchorBottomRight);
    SetAnchor(IDCANCEL, AnchorBottomRight);
    EnableResizing();
    return TRUE;
}

void JoinDialog::UpdateTotal() {
    uint64_t total = 0;
    for (auto s : sizes_)
        if (s != UINT64_MAX) total += s;
    std::wstring t = IntToStrGrouped(parts_.size()) + (parts_.size() == 1 ? L" Teil" : L" Teile") +
                     L", Gesamtgröße " + FormatSize(total) + L" (" + FormatSizeBytes(total) + L" Bytes)";
    if (crc_.hasOriginal && crc_.originalSize != total)
        t += L" – laut Prüfsummendatei erwartet: " + FormatSizeBytes(crc_.originalSize) + L" Bytes";
    SetText(JID_TOTAL, t);
}

void JoinDialog::SetRunning(bool on) {
    running_ = on;
    Enable(JID_TARGET, !on);
    Enable(JID_BROWSE, !on);
    Enable(JID_CHECKCRC, !on && !crcFile_.empty());
    Enable(IDOK, !on);
}

void JoinDialog::Start() {
    if (running_) return;
    std::wstring target = Trim(GetText(JID_TARGET));
    if (target.empty()) {
        MsgError(hwnd_, L"Bitte eine Zieldatei angeben.");
        return;
    }
    std::wstring dir = PathParent(target);
    if (dir.empty() || !DirExists(dir)) {
        MsgError(hwnd_, L"Das Zielverzeichnis „" + dir + L"“ existiert nicht.");
        return;
    }
    target = PathCombine(NormalizeDir(dir), PathFileName(target));
    for (auto& p : parts_)
        if (EqualsI(p, target)) {
            MsgError(hwnd_, L"Die Zieldatei darf keiner der Teile sein.");
            return;
        }
    if (DirExists(target)) {
        MsgError(hwnd_, L"„" + target + L"“ ist ein Verzeichnis.");
        return;
    }
    if (FileExists(target) && !MsgConfirm(hwnd_, L"„" + target + L"“ existiert bereits.\n\nÜberschreiben?")) return;
    for (size_t i = 0; i < sizes_.size(); ++i)
        if (sizes_[i] == UINT64_MAX) {
            MsgError(hwnd_, L"Der Teil „" + PathFileName(parts_[i]) + L"“ kann nicht gelesen werden.");
            return;
        }
    uint64_t total = 0;
    for (auto s : sizes_) total += s;
    uint64_t free = FreeSpace(dir);
    if (free != UINT64_MAX && free < total &&
        !MsgConfirm(hwnd_, L"Auf dem Ziellaufwerk sind nur " + FormatSize(free) + L" frei, benötigt werden " +
                               FormatSize(total) + L".\n\nTrotzdem fortfahren?"))
        return;

    JoinJob j;
    j.parts = parts_;
    j.sizes = sizes_;
    j.target = target;
    j.temp = PathCombine(PathParent(target), MakeUniqueName(PathParent(target), PathFileName(target) + L".qfjoin"));
    j.check = IsChecked(JID_CHECKCRC) && !crcFile_.empty();
    if (j.check) j.crc = crc_;
    temp_ = j.temp;
    target_path_ = j.target;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        partCheck_.assign(parts_.size(), PartUnknown);
    }
    InvalidateRect(Item(JID_PARTS), nullptr, FALSE);
    ResetWorkerState();
    reported_ = false;
    totalCrcChecked_ = j.check && j.crc.hasOriginal;
    totalCrcOk_ = true;
    SetRunning(true);
    SetText(JID_STATUS, L"Wird zusammengefügt …");
    worker_ = std::thread([this, job = std::move(j)]() mutable { Run(std::move(job)); });
}

int JoinDialog::RunInner(JoinJob& j) {
    HANDLE out = CreateFileW(LongPath(j.temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        Fail(L"Die Zieldatei kann nicht angelegt werden:\n" + LastErrorMessage());
        return ResError;
    }
    uint64_t total = 0;
    for (auto s : j.sizes) total += s;
    std::vector<uint8_t> buf(kBlock);
    uint64_t done = 0;
    uint32_t crcAll = 0;
    bool anyBad = false;
    int res = ResOk;
    for (size_t i = 0; i < j.parts.size() && res == ResOk; ++i) {
        const std::wstring name = PathFileName(j.parts[i]);
        SetStatusFromWorker(L"Teil " + IntToStrGrouped(i + 1) + L" von " + IntToStrGrouped(j.parts.size()) + L": " + name);
        Tick(true);
        HANDLE in = OpenForRead(j.parts[i]);
        if (in == INVALID_HANDLE_VALUE) {
            Fail(L"Der Teil „" + name + L"“ kann nicht geöffnet werden:\n" + LastErrorMessage());
            res = ResError;
            break;
        }
        uint32_t pc = 0;
        for (;;) {
            if (stop_) {
                res = ResAborted;
                break;
            }
            DWORD got = 0;
            if (!ReadFull(in, buf.data(), kBlock, got)) {
                Fail(L"Fehler beim Lesen von „" + name + L"“:\n" + LastErrorMessage());
                res = ResError;
                break;
            }
            if (got == 0) break;
            if (!WriteAll(out, buf.data(), got)) {
                Fail(L"Fehler beim Schreiben der Zieldatei (Datenträger voll?):\n" + LastErrorMessage());
                res = ResError;
                break;
            }
            pc = Crc32Update(pc, buf.data(), got);
            crcAll = Crc32Update(crcAll, buf.data(), got);
            done += got;
            permille_ = (int)(total ? std::min<uint64_t>(1000, done * 1000 / total) : 1000);
            Tick(false);
        }
        CloseHandle(in);
        if (res == ResOk && j.check) {
            auto f = j.crc.parts.find(ToUpper(name));
            int st = PartNoEntry;
            if (f != j.crc.parts.end()) st = f->second == pc ? PartOk : PartBad;
            if (st == PartBad) anyBad = true;
            std::lock_guard<std::mutex> lock(mtx_);
            partCheck_[i] = st;
        }
    }
    if (!FlushFileBuffers(out) && res == ResOk) {
        Fail(L"Fehler beim Schreiben der Zieldatei:\n" + LastErrorMessage());
        res = ResError;
    }
    CloseHandle(out);
    if (res != ResOk) {
        DeleteFileW(LongPath(j.temp).c_str());
        return res;
    }
    bool totalBad = j.check && j.crc.hasOriginal && (j.crc.originalCrc != crcAll || j.crc.originalSize != done);
    if (totalBad || anyBad) {
        totalCrcOk_ = !totalBad;
        return ResCrcMismatch; // temporäre Datei bleibt bis zur Entscheidung erhalten
    }
    if (!MoveFileExW(LongPath(j.temp).c_str(), LongPath(j.target).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        Fail(L"Die Zieldatei kann nicht angelegt werden:\n" + LastErrorMessage());
        DeleteFileW(LongPath(j.temp).c_str());
        return ResError;
    }
    return ResOk;
}

void JoinDialog::Run(JoinJob j) {
    int res = RunInner(j);
    if (res == ResOk) permille_ = 1000;
    result_ = res;
    PostMessageW(target_, kMsgDone, 0, 0);
}

bool JoinDialog::Finish() {
    if (reported_) return true;
    reported_ = true;
    success_ = true;
    LogOperation(L"Dateien zusammengefügt: " + IntToStrGrouped(parts_.size()) + L" Teile (" + PathFileName(first_) +
                 L" …) → " + target_path_);
    RefreshIfNeeded();
    return true;
}

BOOL JoinDialog::OnCommand(int id, int code, HWND ctl) {
    switch (id) {
    case JID_BROWSE: {
        std::wstring p = SaveFileDialog(hwnd_, L"Zieldatei", GetText(JID_TARGET));
        if (!p.empty()) SetText(JID_TARGET, p);
        return TRUE;
    }
    case IDOK:
        Start();
        return TRUE;
    case IDCANCEL:
        if (running_) {
            stop_ = true;
            SetText(JID_STATUS, L"Wird abgebrochen …");
            return TRUE;
        }
        End(success_ ? IDOK : IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR JoinDialog::OnNotify(NMHDR* nm) {
    if (nm->idFrom != JID_PARTS || nm->code != LVN_GETDISPINFOW) return 0;
    auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
    if (!(di->item.mask & LVIF_TEXT)) return 0;
    size_t i = (size_t)di->item.iItem;
    if (i >= parts_.size()) return 0;
    switch (di->item.iSubItem) {
    case 0: dispBuf_ = PathFileName(parts_[i]); break;
    case 1: dispBuf_ = sizes_[i] == UINT64_MAX ? L"?" : FormatSizeBytes(sizes_[i]); break;
    case 2: {
        int st = PartUnknown;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (i < partCheck_.size()) st = partCheck_[i];
        }
        dispBuf_ = st == PartOk ? L"OK" : st == PartBad ? L"FEHLER" : st == PartNoEntry ? L"kein Eintrag" : L"";
        break;
    }
    default: dispBuf_.clear();
    }
    di->item.pszText = const_cast<wchar_t*>(dispBuf_.c_str());
    return 0;
}

INT_PTR JoinDialog::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgProgress) {
        ShowProgress(JID_PROGRESS, JID_STATUS);
        InvalidateRect(Item(JID_PARTS), nullptr, FALSE);
        return TRUE;
    }
    if (msg != kMsgDone) return FALSE;
    if (worker_.joinable()) worker_.join();
    ShowProgress(JID_PROGRESS, JID_STATUS);
    InvalidateRect(Item(JID_PARTS), nullptr, FALSE);
    int res = result_;
    SetRunning(false);
    switch (res) {
    case ResOk: {
        Finish();
        std::wstring msgText = L"Die Teile wurden zu „" + PathFileName(target_path_) + L"“ zusammengefügt.";
        if (totalCrcChecked_) msgText += L"\n\nDie Prüfsumme stimmt.";
        MsgInfo(hwnd_, msgText);
        End(IDOK);
        return TRUE;
    }
    case ResCrcMismatch: {
        std::wstring q = totalCrcOk_ ? L"Die Prüfsumme mindestens eines Teils stimmt nicht (siehe Liste)."
                                     : L"Die Prüfsumme der zusammengefügten Datei stimmt nicht mit der "
                                       L"Prüfsummendatei überein.";
        q += L"\n\nZieldatei trotzdem behalten?";
        if (MsgConfirm(hwnd_, q)) {
            if (MoveFileExW(LongPath(temp_).c_str(), LongPath(target_path_).c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                Finish();
                End(IDOK);
                return TRUE;
            }
            MsgError(hwnd_, L"Die Zieldatei kann nicht angelegt werden:\n" + LastErrorMessage());
        }
        DeleteFileW(LongPath(temp_).c_str());
        SendMessageW(Item(JID_PROGRESS), PBM_SETPOS, 0, 0);
        SetText(JID_STATUS, L"Prüfsummenfehler – die Zieldatei wurde nicht angelegt.");
        return TRUE;
    }
    case ResAborted:
        SendMessageW(Item(JID_PROGRESS), PBM_SETPOS, 0, 0);
        SetText(JID_STATUS, L"Abgebrochen – die unvollständige Zieldatei wurde gelöscht.");
        return TRUE;
    default:
        SendMessageW(Item(JID_PROGRESS), PBM_SETPOS, 0, 0);
        SetText(JID_STATUS, L"Fehler.");
        MsgError(hwnd_, ErrorText());
        return TRUE;
    }
}

void JoinDialog::OnDestroy() {
    JoinWorker();
    int res = result_;
    if (res == ResOk && !temp_.empty())
        Finish(); // Abschluss fiel mit dem Schließen zusammen
    else if (res == ResCrcMismatch && !reported_ && !temp_.empty())
        DeleteFileW(LongPath(temp_).c_str());
}

} // namespace

// ---------------------------------------------------------------------------
// Öffentliche Funktionen
// ---------------------------------------------------------------------------

bool SplitFile(HWND owner, const std::wstring& file, const std::wstring& targetDir) {
    if (!FileExists(file)) {
        MsgError(owner, L"„" + file + L"“ ist keine Datei.");
        return false;
    }
    uint64_t size = GetFileSize64(file);
    if (size == 0 || size == UINT64_MAX) {
        MsgError(owner, L"Die Datei „" + PathFileName(file) + L"“ ist leer oder kann nicht gelesen werden.");
        return false;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    DialogTemplate t(L"Datei teilen", 300, 209);
    t.Label(SID_LBL_SRC, L"Quelldatei:", 7, 8, 60, 8);
    t.Label(SID_SRC, L"", 70, 8, 223, 8, SS_PATHELLIPSIS);
    t.Label(SID_LBL_SIZE, L"Größe:", 7, 20, 60, 8);
    t.Label(SID_SIZE, L"", 70, 20, 223, 8);
    t.Label(SID_LBL_TARGET, L"&Zielverzeichnis:", 7, 36, 60, 8);
    t.Edit(SID_TARGET, 70, 34, 205, 12);
    t.Button(SID_BROWSE, L"…", 279, 33, 14, 14);
    t.Group(SID_GROUP, L"Teilgröße", 7, 52, 286, 64);
    t.Radio(SID_R_PRESET, L"&Vorgabe:", 13, 65, 70, 10, true);
    t.Combo(SID_PRESET, 86, 63, 150, 150);
    t.Radio(SID_R_CUSTOM, L"&Benutzerdefiniert:", 13, 81, 70, 10);
    t.Edit(SID_CUSTOM, 86, 79, 70, 12);
    t.Combo(SID_UNIT, 160, 79, 50, 100);
    t.Radio(SID_R_COUNT, L"&Anzahl Teile:", 13, 97, 70, 10);
    t.Edit(SID_COUNT, 86, 95, 40, 12, ES_AUTOHSCROLL | ES_NUMBER);
    t.Label(SID_INFO, L"", 7, 121, 286, 8, WS_GROUP | SS_ENDELLIPSIS);
    t.Check(SID_BATCH, L"Batch&datei zum Zusammenfügen ohne QFiles erzeugen (Name.bat)", 7, 134, 286, 10, WS_GROUP);
    t.Check(SID_CRC, L"&Prüfsummendatei erzeugen (CRC32, Name.crc)", 7, 147, 286, 10);
    t.Progress(SID_PROGRESS, 7, 162, 286, 10);
    t.Label(SID_STATUS, L"", 7, 176, 286, 8, SS_ENDELLIPSIS);
    t.DefButton(IDOK, L"&Teilen", 179, 188, 55, 14);
    t.Button(IDCANCEL, L"Abbrechen", 238, 188, 55, 14);

    std::wstring dir = targetDir.empty() ? PathParent(file) : targetDir;
    SplitDialog dlg(file, dir);
    dlg.DoModal(owner, t);
    return dlg.Succeeded();
}

bool JoinFiles(HWND owner, const std::wstring& firstPart, const std::wstring& targetDir) {
    std::wstring base, err;
    std::vector<std::wstring> parts;
    std::vector<uint64_t> sizes;
    if (!FindParts(firstPart, base, parts, sizes, err)) {
        MsgError(owner, err);
        return false;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    DialogTemplate t(L"Dateien zusammenfügen", 320, 217, DialogTemplate::kResizable);
    t.Label(JID_LBL_FIRST, L"Erster Teil:", 7, 8, 55, 8);
    t.Label(JID_FIRST, L"", 65, 8, 248, 8, SS_PATHELLIPSIS);
    t.Label(JID_LBL_PARTS, L"Gefundene Teile:", 7, 21, 100, 8);
    t.ListView(JID_PARTS, 7, 31, 306, 90, LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER);
    t.Label(JID_TOTAL, L"", 7, 125, 306, 8, SS_ENDELLIPSIS);
    t.Label(JID_LBL_TARGET, L"&Zieldatei:", 7, 140, 45, 8);
    t.Edit(JID_TARGET, 55, 138, 240, 12);
    t.Button(JID_BROWSE, L"…", 299, 137, 14, 14);
    t.Check(JID_CHECKCRC, L"&Prüfsummen prüfen", 7, 155, 306, 10);
    t.Progress(JID_PROGRESS, 7, 170, 306, 10);
    t.Label(JID_STATUS, L"", 7, 184, 306, 8, SS_ENDELLIPSIS);
    t.DefButton(IDOK, L"Z&usammenfügen", 193, 196, 60, 14);
    t.Button(IDCANCEL, L"Abbrechen", 257, 196, 56, 14);

    JoinDialog dlg(firstPart, targetDir, base, std::move(parts), std::move(sizes));
    dlg.DoModal(owner, t);
    return dlg.Succeeded();
}

} // namespace qf
