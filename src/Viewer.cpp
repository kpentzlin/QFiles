// Viewer.cpp – Modul B: Anzeige-Steuerelement (Schnellansicht) und eigenständiges Anzeigefenster (F3).
//
// Das Steuerelement (Fensterklasse "QFilesViewer") zeigt eine Datei als Text (RichEdit, schreibgeschützt),
// Hex (HexView), Bild (WIC, Dekodierung in einem Hintergrund-Thread) oder über einen Windows-Vorschauhandler
// (IPreviewHandler) an; Verzeichnisse als kurze Übersicht. Eine Kopfzeile zeigt Name, Größe, Modus und Kodierung.

#include "App.h"
#include "Dialog.h"
#include "Encoding.h"
#include "HexView.h"
#include "Modules.h"
#include "Util.h"

#include <windowsx.h>
#include <commctrl.h>
#include <richedit.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propsys.h>
#include <wincodec.h>
#include <propidl.h>

// shlwapi.h definiert Makros, die mit Funktionen aus Util.h kollidieren
#ifdef PathCombine
#undef PathCombine
#endif
#ifdef StrToInt
#undef StrToInt
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace qf {

namespace {

constexpr wchar_t kViewerClass[] = L"QFilesViewer";
constexpr wchar_t kPreviewHostClass[] = L"QFilesPreviewHost";
constexpr wchar_t kViewerFrameClass[] = L"QFilesViewerFrame";
constexpr wchar_t kViewerSection[] = L"Anzeige";

constexpr UINT WM_QF_IMAGEDONE = WM_APP + 0x351;   // Hintergrund-Dekodierung fertig (wParam = Generation)

constexpr size_t kTextLimitWindow = 8u * 1024 * 1024;     // Text im Anzeigefenster: max. 8 MB
constexpr size_t kTextLimitEmbedded = 2u * 1024 * 1024;   // Text in der Schnellansicht: max. 2 MB (schneller Wechsel)
constexpr size_t kSampleSize = 64 * 1024;                  // Probe für die Binärerkennung
constexpr uint64_t kMaxImagePixels = 40ull * 1000 * 1000;  // größere Bilder werden verkleinert dekodiert

constexpr int kIdEdit = 1;
constexpr int kIdHex = 2;
constexpr int kIdPreviewHost = 3;

// Eigene GUID-Konstanten (vermeidet Abhängigkeiten von Import-Bibliotheken für GUID-Symbole)
const CLSID kClsidWicImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID kWicPixelFormat32bppPBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};
const wchar_t kPreviewHandlerIid[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

// ===================== Hilfsfunktionen =====================

// Minimaler COM-Zeiger (gibt die Referenz im Destruktor frei)
template <class T>
class Com {
public:
    Com() = default;
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return p_; }
    T** operator&() {
        Reset();
        return &p_;
    }
    T* Get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    T* Detach() {
        T* t = p_;
        p_ = nullptr;
        return t;
    }
    template <class U>
    HRESULT As(Com<U>& out) const {
        if (!p_) return E_POINTER;
        return p_->QueryInterface(IID_PPV_ARGS(&out));
    }

private:
    T* p_ = nullptr;
};

UINT SystemDpi() {
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? (UINT)dpi : 96;
}

// Kopie einer (für die System-DPI erzeugten) Schrift passend zur DPI des Fensters
HFONT CreateScaledFont(HFONT base, UINT dpi, int weight = 0) {
    LOGFONTW lf{};
    if (!base || !GetObjectW(base, sizeof(lf), &lf)) {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        lf = ncm.lfMessageFont;
    }
    UINT sys = SystemDpi();
    if (dpi && dpi != sys) lf.lfHeight = MulDiv(lf.lfHeight, (int)dpi, (int)sys);
    if (weight) lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

int FontHeight(HWND h, HFONT f) {
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return tm.tmHeight;
}

std::wstring LowerExt(const std::wstring& path) { return ToLower(PathExtension(path)); }

bool InList(const std::wstring& ext, std::initializer_list<const wchar_t*> list) {
    for (const wchar_t* e : list)
        if (ext == e) return true;
    return false;
}

bool IsImageExt(const std::wstring& ext) {
    return InList(ext, {L".bmp", L".dib", L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".gif", L".tif", L".tiff",
                        L".ico", L".wdp", L".jxr", L".hdp", L".heic", L".heif", L".webp", L".avif", L".dds", L".jxl"});
}

// Erweiterungen, die bevorzugt über einen Vorschauhandler angezeigt werden (sofern einer registriert ist)
bool IsPreferredPreviewExt(const std::wstring& ext) {
    return InList(ext, {L".pdf", L".doc", L".docx", L".docm", L".dot", L".dotx", L".xls", L".xlsx", L".xlsm", L".xlsb",
                        L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".rtf", L".htm", L".html", L".mht", L".mhtml",
                        L".msg", L".eml", L".vsd", L".vsdx", L".odt", L".ods", L".odp", L".xps", L".oxps", L".svg",
                        L".epub"});
}

// Ermittelt die CLSID des für die Erweiterung registrierten Vorschauhandlers (shellex\{8895b1c6-…}).
// AssocQueryString berücksichtigt ProgID, SystemFileAssociations und wahrgenommenen Typ. allowStar = auch
// einen für alle Dateien ("*") registrierten Handler akzeptieren (nur bei ausdrücklich gewählter Vorschau).
bool FindPreviewHandler(const std::wstring& ext, CLSID& clsid, bool allowStar = false) {
    if (ext.empty()) return false;
    for (int pass = 0; pass < (allowStar ? 2 : 1); ++pass) {
        wchar_t buf[64] = {};
        DWORD n = 64;
        ASSOCF flags = pass == 0 ? ASSOCF_NONE : ASSOCF_INIT_DEFAULTTOSTAR;
        if (SUCCEEDED(AssocQueryStringW(flags, ASSOCSTR_SHELLEXTENSION, ext.c_str(), kPreviewHandlerIid, buf, &n)) &&
            SUCCEEDED(CLSIDFromString(buf, &clsid)))
            return true;
    }
    return false;
}

bool HasPreviewHandler(const std::wstring& ext) {
    CLSID c;
    return FindPreviewHandler(ext, c);
}

// Abgeschnittene UTF-8-Sequenz am Ende entfernen (bei gekürzt geladenem Text)
size_t TrimIncompleteUtf8(const uint8_t* d, size_t n) {
    size_t i = n;
    int k = 0;
    while (i > 0 && k < 4 && (d[i - 1] & 0xC0) == 0x80) {
        --i;
        ++k;
    }
    if (i == 0) return n;
    uint8_t lead = d[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && n - (i - 1) < need) return i - 1;
    return n;
}

const wchar_t* EncodingShortName(TextEncoding e) {
    switch (e) {
    case TextEncoding::Utf8: return L"UTF-8";
    case TextEncoding::Utf8Bom: return L"UTF-8 (BOM)";
    case TextEncoding::Utf16LE: return L"UTF-16 LE";
    case TextEncoding::Utf16BE: return L"UTF-16 BE";
    case TextEncoding::Ansi: return L"ANSI";
    case TextEncoding::Oem: return L"OEM";
    }
    return L"";
}

// Fensterposition laden/speichern (Bildschirmkoordinaten)
bool LoadPlacement(const wchar_t* section, RECT& r, bool& maximized) {
    Config& c = App::Cfg();
    maximized = c.GetBool(section, L"Max", false);
    if (!c.Has(section, L"B") || !c.Has(section, L"H")) return false;
    int x = c.GetInt(section, L"X", 0), y = c.GetInt(section, L"Y", 0);
    int w = c.GetInt(section, L"B", 0), h = c.GetInt(section, L"H", 0);
    if (w < 200 || h < 150) return false;
    r = RECT{x, y, x + w, y + h};
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

void SavePlacement(HWND hwnd, const wchar_t* section) {
    Config& c = App::Cfg();
    bool zoomed = IsZoomed(hwnd) != FALSE;
    c.SetBool(section, L"Max", zoomed);
    if (zoomed || IsIconic(hwnd)) return;
    RECT r;
    GetWindowRect(hwnd, &r);
    c.SetInt(section, L"X", r.left);
    c.SetInt(section, L"Y", r.top);
    c.SetInt(section, L"B", r.right - r.left);
    c.SetInt(section, L"H", r.bottom - r.top);
}

// ===================== Bilddekodierung (Hintergrund-Thread) =====================

struct ImageJob {
    std::mutex m;
    std::wstring path;
    std::atomic<bool> cancel{false};
    bool done = false;
    bool ok = false;
    HBITMAP bmp = nullptr;   // 32 bpp PBGRA, top-down
    void* bits = nullptr;
    int w = 0, h = 0;        // Größe des dekodierten Bildes (nach Drehung/Verkleinerung)
    int origW = 0, origH = 0;
    bool hasAlpha = false;
    bool downscaled = false;
    UINT frames = 0;
    std::wstring format;
    std::wstring error;
    ~ImageJob() {
        if (bmp) DeleteObject(bmp);
    }
};

std::wstring HrText(HRESULT hr) {
    std::wstring msg = LastErrorMessage((DWORD)hr);
    if (msg.rfind(L"Fehler ", 0) == 0) {
        wchar_t buf[16];
        swprintf(buf, 16, L"0x%08X", (unsigned)hr);
        return std::wstring(L"Fehler ") + buf;
    }
    return msg;
}

std::wstring DecoderName(IWICBitmapDecoder* dec) {
    Com<IWICBitmapDecoderInfo> info;
    if (FAILED(dec->GetDecoderInfo(&info)) || !info) return L"";
    wchar_t buf[128] = {};
    UINT got = 0;
    if (FAILED(info->GetFriendlyName(128, buf, &got))) return L"";
    std::wstring n = buf;
    for (const wchar_t* suffix : {L" Decoder", L" decoder", L" Codec", L" codec"})
        if (EndsWithI(n, suffix)) n.resize(n.size() - wcslen(suffix));
    if (StartsWithI(n, L"Microsoft ")) n.erase(0, 10);
    if (EqualsI(n, L"WMPhoto")) n = L"JPEG XR";
    return Trim(n);
}

void DecodeImage(ImageJob& j) {
    auto fail = [&](const std::wstring& e) {
        std::lock_guard<std::mutex> lock(j.m);
        j.error = e;
        j.ok = false;
        j.done = true;
    };
    Com<IWICImagingFactory> f;
    HRESULT hr = CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (FAILED(hr)) return fail(L"WIC nicht verfügbar: " + HrText(hr));
    HANDLE file = CreateFileW(LongPath(j.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail(LastErrorMessage());
    struct HandleGuard {
        HANDLE h;
        ~HandleGuard() { CloseHandle(h); }
    } guard{file};

    Com<IWICBitmapDecoder> dec;
    hr = f->CreateDecoderFromFileHandle((ULONG_PTR)file, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) return fail(L"Kein passender Bild-Codec (" + HrText(hr) + L")");
    std::wstring format = DecoderName(dec.Get());
    UINT frames = 0;
    dec->GetFrameCount(&frames);
    Com<IWICBitmapFrameDecode> frame;
    hr = dec->GetFrame(0, &frame);
    if (FAILED(hr)) return fail(HrText(hr));
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return fail(L"Ungültige Bildgröße");
    if (j.cancel) return fail(L"");

    // EXIF-Ausrichtung (JPEG: /app1/ifd, TIFF: /ifd)
    USHORT orientation = 1;
    Com<IWICMetadataQueryReader> qr;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&qr)) && qr) {
        for (const wchar_t* q : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(qr->GetMetadataByName(q, &pv))) {
                if (pv.vt == VT_UI2) orientation = pv.uiVal;
                PropVariantClear(&pv);
                if (orientation != 1) break;
            }
        }
    }

    IWICBitmapSource* src = frame.Get();
    Com<IWICBitmapScaler> scaler;
    bool downscaled = false;
    if ((uint64_t)w * h > kMaxImagePixels) {
        double factor = std::sqrt((double)kMaxImagePixels / ((double)w * (double)h));
        UINT sw = std::max<UINT>(1, (UINT)(w * factor)), sh = std::max<UINT>(1, (UINT)(h * factor));
        if (SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(src, sw, sh, WICBitmapInterpolationModeFant))) {
            src = scaler.Get();
            downscaled = true;
        }
    }
    Com<IWICBitmapFlipRotator> rot;
    WICBitmapTransformOptions opt = WICBitmapTransformRotate0;
    switch (orientation) {
    case 2: opt = WICBitmapTransformFlipHorizontal; break;
    case 3: opt = WICBitmapTransformRotate180; break;
    case 4: opt = WICBitmapTransformFlipVertical; break;
    case 5: opt = (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
    case 6: opt = WICBitmapTransformRotate90; break;
    case 7: opt = (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
    case 8: opt = WICBitmapTransformRotate270; break;
    default: break;
    }
    if (opt != WICBitmapTransformRotate0 && SUCCEEDED(f->CreateBitmapFlipRotator(&rot)) &&
        SUCCEEDED(rot->Initialize(src, opt)))
        src = rot.Get();

    Com<IWICFormatConverter> conv;
    hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(src, kWicPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) return fail(L"Pixelformat kann nicht umgewandelt werden (" + HrText(hr) + L")");
    UINT cw = 0, ch = 0;
    conv->GetSize(&cw, &ch);
    if (!cw || !ch) return fail(L"Ungültige Bildgröße");
    if (j.cancel) return fail(L"");

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)cw;
    bi.bmiHeader.biHeight = -(LONG)ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return fail(L"Nicht genügend Speicher für das Bild");
    }
    UINT stride = cw * 4;
    hr = conv->CopyPixels(nullptr, stride, stride * ch, (BYTE*)bits);
    if (FAILED(hr) || j.cancel) {
        DeleteObject(bmp);
        return fail(j.cancel ? L"" : L"Bild kann nicht dekodiert werden (" + HrText(hr) + L")");
    }
    bool alpha = false;
    const uint8_t* p = (const uint8_t*)bits;
    for (size_t i = 3, n = (size_t)stride * ch; i < n; i += 4)
        if (p[i] != 255) {
            alpha = true;
            break;
        }
    GdiFlush();
    std::lock_guard<std::mutex> lock(j.m);
    j.bmp = bmp;
    j.bits = bits;
    j.w = (int)cw;
    j.h = (int)ch;
    bool swap = orientation >= 5 && orientation <= 8;
    j.origW = (int)(swap ? h : w);
    j.origH = (int)(swap ? w : h);
    j.hasAlpha = alpha;
    j.downscaled = downscaled;
    j.frames = frames;
    j.format = format;
    j.ok = true;
    j.done = true;
}

void DecodeWorker(std::shared_ptr<ImageJob> job, HWND notify, UINT gen) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DecodeImage(*job);
    if (SUCCEEDED(co)) CoUninitialize();
    PostMessageW(notify, WM_QF_IMAGEDONE, (WPARAM)gen, 0);
}

// ===================== Zustand des Anzeige-Steuerelements =====================

enum class Eff { None, Message, Text, Hex, Image, Preview, Directory };

struct ViewerState {
    HWND hwnd = nullptr;
    bool embedded = true;          // Schnellansicht (sonst Anzeigefenster)
    ViewerMode mode = ViewerMode::Auto;
    Eff eff = Eff::None;
    std::wstring path;
    uint64_t fileSize = 0;
    FILETIME lastWrite{};
    DWORD attributes = 0;
    std::wstring message;          // Eff::Message
    std::wstring note;             // Zusatzhinweis in der Kopfzeile

    // Text
    HWND edit = nullptr;
    bool richEdit = false;
    bool wrap = false;
    int forcedEnc = -1;            // -1 = automatisch, sonst TextEncoding
    TextEncoding textEnc = TextEncoding::Utf8;
    bool truncated = false;
    size_t textLimit = kTextLimitEmbedded;

    // Hex
    HWND hex = nullptr;

    // Vorschauhandler
    HWND previewHost = nullptr;
    IPreviewHandler* preview = nullptr;

    // Bild
    std::shared_ptr<ImageJob> job;
    UINT gen = 0;
    bool imgLoading = false;
    HBITMAP img = nullptr;
    void* imgBits = nullptr;
    int imgW = 0, imgH = 0, origW = 0, origH = 0;
    bool imgAlpha = false, imgDownscaled = false;
    UINT imgFrames = 0;
    std::wstring imgFormat;
    HBITMAP scaled = nullptr;
    int scaledW = 0, scaledH = 0;
    bool fit = true;
    double zoom = 1.0;
    int panX = 0, panY = 0;
    bool dragging = false;
    POINT dragStart{};
    int dragPanX = 0, dragPanY = 0;
    IWICImagingFactory* wic = nullptr;
    HBRUSH checker = nullptr;

    // Verzeichnis
    std::vector<std::pair<std::wstring, std::wstring>> dirInfo;

    // Darstellung
    HFONT uiFont = nullptr, uiBold = nullptr, monoFont = nullptr;
    int headerH = 20;
    int lineH = 16;
    int wheelAccum = 0;

    // Textsuche
    std::wstring findText;
    bool findCase = false, findWord = false, findBack = false;

    // Rückrufe des Anzeigefensters
    std::function<void(int)> navigate;   // +1 / -1: nächste/vorige Datei
    std::function<void()> changed;       // Datei/Modus geändert
};

ViewerState* GetViewer(HWND h) { return h ? reinterpret_cast<ViewerState*>(GetWindowLongPtrW(h, GWLP_USERDATA)) : nullptr; }

RECT ContentRect(ViewerState& s) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    rc.top = std::min<LONG>(rc.bottom, s.headerH);
    return rc;
}

HWND FocusTarget(ViewerState& s) {
    if (s.eff == Eff::Text && s.edit) return s.edit;
    if (s.eff == Eff::Hex && s.hex) return s.hex;
    return s.hwnd;
}

bool HasFocusInside(ViewerState& s) {
    HWND f = GetFocus();
    return f && (f == s.hwnd || IsChild(s.hwnd, f));
}

// ---------- Bild ----------

void FreeScaled(ViewerState& s) {
    if (s.scaled) DeleteObject(s.scaled);
    s.scaled = nullptr;
    s.scaledW = s.scaledH = 0;
}

void FreeImage(ViewerState& s) {
    FreeScaled(s);
    if (s.img) DeleteObject(s.img);
    s.img = nullptr;
    s.imgBits = nullptr;
    s.imgW = s.imgH = s.origW = s.origH = 0;
    s.imgAlpha = s.imgDownscaled = false;
    s.imgFrames = 0;
    s.imgFormat.clear();
}

void CancelJob(ViewerState& s) {
    if (s.job) s.job->cancel = true;
    s.job.reset();
    ++s.gen;
    s.imgLoading = false;
}

double FitScale(const ViewerState& s, int cw, int ch) {
    if (!s.imgW || !s.imgH || cw <= 0 || ch <= 0) return 1.0;
    double f = std::min((double)cw / s.imgW, (double)ch / s.imgH);
    return std::min(1.0, f);
}

double CurrentScale(const ViewerState& s, int cw, int ch) { return s.fit ? FitScale(s, cw, ch) : s.zoom; }

struct ImageGeom {
    double scale;
    int dw, dh;   // angezeigte Größe
    int x0, y0;   // linke obere Ecke (Client-Koordinaten)
};

ImageGeom ComputeGeom(ViewerState& s) {
    RECT cr = ContentRect(s);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    ImageGeom g{};
    g.scale = CurrentScale(s, cw, ch);
    g.dw = std::max(1, (int)std::lround(s.imgW * g.scale));
    g.dh = std::max(1, (int)std::lround(s.imgH * g.scale));
    s.panX = std::clamp(s.panX, 0, std::max(0, g.dw - cw));
    s.panY = std::clamp(s.panY, 0, std::max(0, g.dh - ch));
    g.x0 = g.dw <= cw ? (int)cr.left + (cw - g.dw) / 2 : (int)cr.left - s.panX;
    g.y0 = g.dh <= ch ? (int)cr.top + (ch - g.dh) / 2 : (int)cr.top - s.panY;
    return g;
}

bool EnsureScaled(ViewerState& s, int dw, int dh) {
    if (s.scaled && s.scaledW == dw && s.scaledH == dh) return true;
    FreeScaled(s);
    if (!s.img || !s.imgBits) return false;
    if (!s.wic && FAILED(CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s.wic))))
        return false;
    Com<IWICBitmap> src;
    UINT stride = (UINT)s.imgW * 4;
    if (FAILED(s.wic->CreateBitmapFromMemory((UINT)s.imgW, (UINT)s.imgH, kWicPixelFormat32bppPBGRA, stride,
                                             stride * (UINT)s.imgH, (BYTE*)s.imgBits, &src)))
        return false;
    Com<IWICBitmapScaler> sc;
    if (FAILED(s.wic->CreateBitmapScaler(&sc)) ||
        FAILED(sc->Initialize(src.Get(), (UINT)dw, (UINT)dh, WICBitmapInterpolationModeFant)))
        return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return false;
    }
    if (FAILED(sc->CopyPixels(nullptr, (UINT)dw * 4, (UINT)dw * 4 * (UINT)dh, (BYTE*)bits))) {
        DeleteObject(bmp);
        return false;
    }
    GdiFlush();
    s.scaled = bmp;
    s.scaledW = dw;
    s.scaledH = dh;
    return true;
}

void UpdateChecker(ViewerState& s) {
    if (s.checker) DeleteObject(s.checker);
    int c = std::max(4, DpiScale(s.hwnd, 8));
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 2 * c, 2 * c);
    HGDIOBJ old = SelectObject(dc, bmp);
    HBRUSH light = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    HBRUSH dark = CreateSolidBrush(RGB(0xE4, 0xE4, 0xE4));
    RECT r{0, 0, 2 * c, 2 * c};
    FillRect(dc, &r, light);
    RECT a{0, 0, c, c}, b{c, c, 2 * c, 2 * c};
    FillRect(dc, &a, dark);
    FillRect(dc, &b, dark);
    SelectObject(dc, old);
    s.checker = CreatePatternBrush(bmp);
    DeleteObject(bmp);
    DeleteObject(light);
    DeleteObject(dark);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

void PaintImage(ViewerState& s, HDC dc, const RECT& cr) {
    HBRUSH bg = CreateSolidBrush(RGB(0x40, 0x40, 0x40));
    FillRect(dc, &cr, bg);
    DeleteObject(bg);
    if (!s.img) return;
    ImageGeom g = ComputeGeom(s);
    RECT imgRect{g.x0, g.y0, g.x0 + g.dw, g.y0 + g.dh};
    RECT vis;
    if (!IntersectRect(&vis, &imgRect, &cr)) return;
    int visW = vis.right - vis.left, visH = vis.bottom - vis.top;
    if (s.imgAlpha) {
        if (!s.checker) UpdateChecker(s);
        SetBrushOrgEx(dc, g.x0, g.y0, nullptr);
        FillRect(dc, &vis, s.checker);
    }
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC src = CreateCompatibleDC(dc);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, vis.left, vis.top, vis.right, vis.bottom);
    if (g.dw == s.imgW && g.dh == s.imgH) {
        HGDIOBJ old = SelectObject(src, s.img);
        if (s.imgAlpha)
            AlphaBlend(dc, vis.left, vis.top, visW, visH, src, vis.left - g.x0, vis.top - g.y0, visW, visH, bf);
        else
            BitBlt(dc, vis.left, vis.top, visW, visH, src, vis.left - g.x0, vis.top - g.y0, SRCCOPY);
        SelectObject(src, old);
    } else if (g.scale < 1.0 && EnsureScaled(s, g.dw, g.dh)) {
        HGDIOBJ old = SelectObject(src, s.scaled);
        if (s.imgAlpha)
            AlphaBlend(dc, vis.left, vis.top, visW, visH, src, vis.left - g.x0, vis.top - g.y0, visW, visH, bf);
        else
            BitBlt(dc, vis.left, vis.top, visW, visH, src, vis.left - g.x0, vis.top - g.y0, SRCCOPY);
        SelectObject(src, old);
    } else {
        // Vergrößerung (oder Rückfall): nur den sichtbaren Ausschnitt strecken
        int sx0 = std::clamp((int)std::floor((vis.left - g.x0) / g.scale), 0, s.imgW - 1);
        int sy0 = std::clamp((int)std::floor((vis.top - g.y0) / g.scale), 0, s.imgH - 1);
        int sx1 = std::clamp((int)std::ceil((vis.right - g.x0) / g.scale), sx0 + 1, s.imgW);
        int sy1 = std::clamp((int)std::ceil((vis.bottom - g.y0) / g.scale), sy0 + 1, s.imgH);
        int dx0 = g.x0 + (int)std::lround(sx0 * g.scale), dx1 = g.x0 + (int)std::lround(sx1 * g.scale);
        int dy0 = g.y0 + (int)std::lround(sy0 * g.scale), dy1 = g.y0 + (int)std::lround(sy1 * g.scale);
        HGDIOBJ old = SelectObject(src, s.img);
        SetStretchBltMode(dc, g.scale >= 1.0 ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        if (s.imgAlpha)
            AlphaBlend(dc, dx0, dy0, dx1 - dx0, dy1 - dy0, src, sx0, sy0, sx1 - sx0, sy1 - sy0, bf);
        else
            StretchBlt(dc, dx0, dy0, dx1 - dx0, dy1 - dy0, src, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
        SelectObject(src, old);
    }
    RestoreDC(dc, saved);
    DeleteDC(src);

    // Info unten links
    std::wstring info = IntToStrGrouped((unsigned long long)s.origW) + L" × " + IntToStrGrouped((unsigned long long)s.origH);
    if (!s.imgFormat.empty()) info += L"  ·  " + s.imgFormat;
    double shown = g.scale * (double)s.imgW / std::max(1, s.origW);
    info += L"  ·  " + std::to_wstring((int)std::lround(shown * 100.0)) + L" %";
    if (s.imgFrames > 1) info += L"  ·  " + std::to_wstring(s.imgFrames) + L" Bilder";
    HGDIOBJ oldFont = SelectObject(dc, s.uiFont);
    RECT tr{0, 0, 0, 0};
    DrawTextW(dc, info.c_str(), (int)info.size(), &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int pad = DpiScale(s.hwnd, 4), m = DpiScale(s.hwnd, 6);
    RECT box{cr.left + m, cr.bottom - m - (tr.bottom - tr.top) - 2 * pad, cr.left + m + (tr.right - tr.left) + 2 * pad,
             cr.bottom - m};
    HBRUSH boxBrush = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
    FillRect(dc, &box, boxBrush);
    DeleteObject(boxBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
    RECT textRect{box.left + pad, box.top + pad, box.right - pad, box.bottom - pad};
    DrawTextW(dc, info.c_str(), (int)info.size(), &textRect, DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void ZoomAt(ViewerState& s, double newScale, POINT anchor) {
    if (!s.img) return;
    newScale = std::clamp(newScale, 0.01, 32.0);
    ImageGeom g = ComputeGeom(s);
    RECT cr = ContentRect(s);
    double ix = (anchor.x - g.x0) / g.scale, iy = (anchor.y - g.y0) / g.scale;
    s.fit = false;
    s.zoom = newScale;
    // Bildpunkt unter dem Anker bleibt an seiner Stelle (sofern das Bild größer als die Fläche ist)
    s.panX = (int)std::lround(ix * newScale) - (anchor.x - cr.left);
    s.panY = (int)std::lround(iy * newScale) - (anchor.y - cr.top);
    ComputeGeom(s);   // begrenzt panX/panY
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

POINT ContentCenter(ViewerState& s) {
    RECT cr = ContentRect(s);
    return POINT{(cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2};
}

void ZoomStep(ViewerState& s, bool in) {
    if (!s.img) return;
    double cur = ComputeGeom(s).scale;
    ZoomAt(s, in ? cur * 1.25 : cur / 1.25, ContentCenter(s));
}

void ZoomFit(ViewerState& s) {
    s.fit = true;
    s.panX = s.panY = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void Zoom100(ViewerState& s) {
    if (!s.img) return;
    ZoomAt(s, 1.0, ContentCenter(s));
}

void Pan(ViewerState& s, int dx, int dy) {
    if (!s.img) return;
    s.panX += dx;
    s.panY += dy;
    ComputeGeom(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool ImagePannable(ViewerState& s) {
    if (s.eff != Eff::Image || !s.img) return false;
    ImageGeom g = ComputeGeom(s);
    RECT cr = ContentRect(s);
    return g.dw > cr.right - cr.left || g.dh > cr.bottom - cr.top;
}

// ---------- Laden ----------

void StopPreview(ViewerState& s) {
    if (s.preview) {
        s.preview->Unload();
        s.preview->Release();
        s.preview = nullptr;
    }
    if (s.previewHost) ShowWindow(s.previewHost, SW_HIDE);
}

void Layout(ViewerState& s) {
    RECT cr = ContentRect(s);
    int w = cr.right - cr.left, h = cr.bottom - cr.top;
    for (HWND c : {s.edit, s.hex, s.previewHost})
        if (c) MoveWindow(c, cr.left, cr.top, w, h, TRUE);
    if (s.preview) {
        RECT r{0, 0, w, h};
        s.preview->SetRect(&r);
    }
}

// Zeigt genau ein Kindfenster (oder keines). Ein ausgeblendetes Textfeld wird geleert (Speicher freigeben);
// das geschieht erst hier, damit beim Wechsel Text -> Text nichts flackert.
void ShowChild(ViewerState& s, HWND which) {
    for (HWND c : {s.edit, s.hex, s.previewHost}) {
        if (!c) continue;
        if (c == which) {
            if (!IsWindowVisible(c)) ShowWindow(c, SW_SHOW);
        } else {
            if (IsWindowVisible(c)) ShowWindow(c, SW_HIDE);
            if (c == s.edit && GetWindowTextLengthW(c) > 0) SetWindowTextW(c, L"");
        }
    }
}

void Unload(ViewerState& s) {
    CancelJob(s);
    FreeImage(s);
    StopPreview(s);
    if (s.hex) HexViewClose(s.hex);
    s.dirInfo.clear();
    s.message.clear();
    s.note.clear();
    s.truncated = false;
    s.fit = true;
    s.zoom = 1.0;
    s.panX = s.panY = 0;
    s.eff = Eff::None;
}

bool ReadSample(const std::wstring& path, std::vector<uint8_t>& data) { return ReadFileBytes(path, data, kSampleSize); }

void SetMessage(ViewerState& s, const std::wstring& text) {
    ShowChild(s, nullptr);
    s.eff = Eff::Message;
    s.message = text;
}

void ApplyWrap(ViewerState& s) {
    if (s.edit && s.richEdit) SendMessageW(s.edit, EM_SETTARGETDEVICE, 0, s.wrap ? 0 : 1);
}

bool ShowText(ViewerState& s) {
    std::vector<uint8_t> data;
    if (!ReadFileBytes(s.path, data, s.textLimit)) {
        SetMessage(s, L"Die Datei kann nicht gelesen werden:\n" + LastErrorMessage());
        return false;
    }
    s.truncated = s.fileSize > data.size();
    size_t n = data.size();
    if (s.truncated) n = TrimIncompleteUtf8(data.data(), n);
    TextEncoding enc = s.forcedEnc >= 0 ? (TextEncoding)s.forcedEnc : DetectEncoding(data.data(), n, nullptr, TextEncoding::Utf8);
    if (s.truncated && (enc == TextEncoding::Utf16LE || enc == TextEncoding::Utf16BE)) n = data.size() & ~(size_t)1;
    std::wstring text = DecodeText(data.data(), n, enc);
    if (s.truncated && !text.empty() && IS_HIGH_SURROGATE(text.back())) text.pop_back();
    std::replace(text.begin(), text.end(), L'\0', L' ');
    s.textEnc = enc;
    SetWindowTextW(s.edit, text.c_str());
    SendMessageW(s.edit, EM_SETSEL, 0, 0);
    SendMessageW(s.edit, EM_SCROLLCARET, 0, 0);
    ShowChild(s, s.edit);
    s.eff = Eff::Text;
    if (s.truncated) s.note = L"nur die ersten " + FormatSize(s.textLimit) + L" geladen";
    return true;
}

bool ShowHex(ViewerState& s) {
    if (!HexViewOpen(s.hex, s.path)) {
        SetMessage(s, L"Die Datei kann nicht geöffnet werden:\n" + HexViewLastError(s.hex));
        return false;
    }
    ShowChild(s, s.hex);
    s.eff = Eff::Hex;
    return true;
}

// Rückfall, wenn Bild oder Vorschau nicht möglich sind: Hex bei Binärdaten, sonst Text.
void ShowFallback(ViewerState& s, const std::wstring& why) {
    std::vector<uint8_t> sample;
    bool binary = ReadSample(s.path, sample) && LooksBinary(sample.data(), sample.size());
    if (binary)
        ShowHex(s);
    else
        ShowText(s);
    if (s.eff == Eff::Text || s.eff == Eff::Hex) s.note = why + (s.note.empty() ? L"" : L"; " + s.note);
}

bool ShowPreview(ViewerState& s, std::wstring& err) {
    CLSID clsid;
    if (!FindPreviewHandler(LowerExt(s.path), clsid, s.mode == ViewerMode::Preview)) {
        err = L"kein Vorschauhandler registriert";
        return false;
    }
    Com<IPreviewHandler> ph;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ph));
    if (FAILED(hr)) {
        err = L"Vorschauhandler kann nicht gestartet werden (" + HrText(hr) + L")";
        return false;
    }
    bool init = false;
    {
        Com<IInitializeWithFile> iwf;
        if (SUCCEEDED(ph.As(iwf))) init = SUCCEEDED(iwf->Initialize(s.path.c_str(), STGM_READ));
    }
    if (!init) {
        Com<IInitializeWithStream> iws;
        if (SUCCEEDED(ph.As(iws))) {
            Com<IStream> stm;
            if (SUCCEEDED(SHCreateStreamOnFileEx(LongPath(s.path).c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
                                                 FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stm)))
                init = SUCCEEDED(iws->Initialize(stm.Get(), STGM_READ));
        }
    }
    if (!init) {
        Com<IInitializeWithItem> iwi;
        if (SUCCEEDED(ph.As(iwi))) {
            Com<IShellItem> item;
            if (SUCCEEDED(SHCreateItemFromParsingName(s.path.c_str(), nullptr, IID_PPV_ARGS(&item))))
                init = SUCCEEDED(iwi->Initialize(item.Get(), STGM_READ));
        }
    }
    if (!init) {
        err = L"Vorschauhandler kann die Datei nicht öffnen";
        return false;
    }
    Com<IPreviewHandlerVisuals> visuals;
    if (SUCCEEDED(ph.As(visuals))) {
        visuals->SetBackgroundColor(GetSysColor(COLOR_WINDOW));
        visuals->SetTextColor(GetSysColor(COLOR_WINDOWTEXT));
    }
    ShowChild(s, s.previewHost);
    Layout(s);
    RECT r;
    GetClientRect(s.previewHost, &r);
    HWND focusBefore = GetFocus();
    hr = ph->SetWindow(s.previewHost, &r);
    if (SUCCEEDED(hr)) hr = ph->DoPreview();
    // Manche Vorschauhandler ziehen den Fokus an sich – in der Schnellansicht soll er in der Dateiliste bleiben.
    if (GetFocus() != focusBefore && focusBefore && IsWindow(focusBefore)) {
        if (focusBefore == s.hwnd || IsChild(s.hwnd, focusBefore))
            SetFocus(s.hwnd);
        else
            SetFocus(focusBefore);
    }
    if (FAILED(hr)) {
        ph->Unload();
        ShowWindow(s.previewHost, SW_HIDE);
        err = L"Vorschau fehlgeschlagen (" + HrText(hr) + L")";
        return false;
    }
    s.preview = ph.Detach();
    s.eff = Eff::Preview;
    return true;
}

void StartImage(ViewerState& s) {
    ShowChild(s, nullptr);
    s.eff = Eff::Image;
    s.imgLoading = true;
    auto job = std::make_shared<ImageJob>();
    job->path = s.path;
    s.job = job;
    UINT gen = ++s.gen;
    try {
        std::thread(DecodeWorker, job, s.hwnd, gen).detach();
    } catch (...) {
        // Kein Thread verfügbar: im UI-Thread dekodieren (COM ist hier initialisiert)
        DecodeImage(*job);
        PostMessageW(s.hwnd, WM_QF_IMAGEDONE, (WPARAM)gen, 0);
    }
}

void OnImageDone(ViewerState& s, UINT gen) {
    if (gen != s.gen || !s.job) return;
    std::shared_ptr<ImageJob> job = s.job;
    bool ok;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(job->m);
        if (!job->done) return;
        ok = job->ok;
        error = job->error;
        if (ok) {
            s.img = job->bmp;
            job->bmp = nullptr;
            s.imgBits = job->bits;
            s.imgW = job->w;
            s.imgH = job->h;
            s.origW = job->origW;
            s.origH = job->origH;
            s.imgAlpha = job->hasAlpha;
            s.imgDownscaled = job->downscaled;
            s.imgFrames = job->frames;
            s.imgFormat = job->format;
        }
    }
    s.job.reset();
    s.imgLoading = false;
    if (ok) {
        if (s.imgDownscaled) s.note = L"zur Anzeige verkleinert dekodiert";
    } else {
        bool hadFocus = HasFocusInside(s);
        std::wstring why = L"Bild kann nicht angezeigt werden" + (error.empty() ? L"" : L": " + error);
        std::wstring err;
        if (!(s.mode == ViewerMode::Auto && HasPreviewHandler(LowerExt(s.path)) && ShowPreview(s, err)))
            ShowFallback(s, why);
        Layout(s);
        if (hadFocus) SetFocus(FocusTarget(s));
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (s.changed) s.changed();
}

void LoadDirectory(ViewerState& s, const WIN32_FILE_ATTRIBUTE_DATA& fad) {
    ShowChild(s, nullptr);
    s.eff = Eff::Directory;
    std::vector<DirEntry> entries;
    DWORD err = 0;
    bool ok = ListDirectory(s.path, entries, &err);
    uint64_t files = 0, dirs = 0, total = 0, hidden = 0;
    for (auto& e : entries) {
        if (e.IsDir())
            ++dirs;
        else {
            ++files;
            total += e.size;
        }
        if (e.attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ++hidden;
    }
    std::wstring name = PathFileName(s.path);
    if (name.empty()) name = s.path;
    s.dirInfo.push_back({L"Verzeichnis", name});
    s.dirInfo.push_back({L"Pfad", s.path});
    if (!ok) {
        s.dirInfo.push_back({L"Inhalt", L"kann nicht gelesen werden: " + LastErrorMessage(err)});
    } else {
        s.dirInfo.push_back({L"Dateien", IntToStrGrouped(files)});
        s.dirInfo.push_back({L"Unterverzeichnisse", IntToStrGrouped(dirs)});
        if (hidden) s.dirInfo.push_back({L"davon versteckt/System", IntToStrGrouped(hidden)});
        s.dirInfo.push_back({L"Größe der Dateien", FormatSize(total) + L" (" + FormatSizeBytes(total) + L" Bytes)"});
        s.dirInfo.push_back({L"", L"(nur direkt enthaltene Dateien, ohne Unterverzeichnisse)"});
    }
    s.dirInfo.push_back({L"Erstellt", FormatFileTime(fad.ftCreationTime, true)});
    s.dirInfo.push_back({L"Geändert", FormatFileTime(fad.ftLastWriteTime, true)});
    s.dirInfo.push_back({L"Attribute", FormatAttributes(fad.dwFileAttributes)});
    if (IsRootPath(s.path)) {
        ULARGE_INTEGER avail{}, totalBytes{}, freeBytes{};
        if (GetDiskFreeSpaceExW(s.path.c_str(), &avail, &totalBytes, &freeBytes)) {
            s.dirInfo.push_back({L"Laufwerk gesamt", FormatSize(totalBytes.QuadPart)});
            s.dirInfo.push_back({L"Laufwerk frei", FormatSize(freeBytes.QuadPart)});
        }
    }
}

ViewerMode DetectMode(ViewerState& s) {
    std::wstring ext = LowerExt(s.path);
    if (IsImageExt(ext)) return ViewerMode::Image;
    if (s.fileSize == 0) return ViewerMode::Text;
    bool handler = HasPreviewHandler(ext);
    if (handler && IsPreferredPreviewExt(ext)) return ViewerMode::Preview;
    std::vector<uint8_t> sample;
    if (!ReadSample(s.path, sample)) return ViewerMode::Hex;
    if (LooksBinary(sample.data(), sample.size())) {
        bool textExt = MatchAnyPattern(App::Opt().textExtensions, PathFileName(s.path));
        return handler && !textExt ? ViewerMode::Preview : ViewerMode::Hex;
    }
    return ViewerMode::Text;
}

void LoadPath(ViewerState& s, const std::wstring& path, bool force) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    bool exists = !path.empty() && GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &fad);
    DWORD attrErr = exists ? 0 : GetLastError();
    uint64_t size = exists ? (((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) : 0;
    if (!force && exists && path == s.path && s.eff != Eff::None && s.eff != Eff::Message &&
        CompareFileTime(&fad.ftLastWriteTime, &s.lastWrite) == 0 && size == s.fileSize)
        return;   // unverändert (z. B. Aktualisierung der Liste)

    bool hadFocus = HasFocusInside(s);
    Unload(s);
    s.path = path;
    s.fileSize = size;
    s.lastWrite = exists ? fad.ftLastWriteTime : FILETIME{};
    s.attributes = exists ? fad.dwFileAttributes : 0;

    if (path.empty()) {
        ShowChild(s, nullptr);
        s.eff = Eff::None;
    } else if (!exists) {
        SetMessage(s, L"„" + PathFileName(path) + L"“ kann nicht gelesen werden:\n" + LastErrorMessage(attrErr));
    } else if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        LoadDirectory(s, fad);
    } else {
        ViewerMode m = s.mode == ViewerMode::Auto ? DetectMode(s) : s.mode;
        switch (m) {
        case ViewerMode::Text: ShowText(s); break;
        case ViewerMode::Hex: ShowHex(s); break;
        case ViewerMode::Image: StartImage(s); break;
        case ViewerMode::Preview: {
            std::wstring err;
            if (!ShowPreview(s, err)) ShowFallback(s, L"Vorschau nicht möglich: " + err);
            break;
        }
        default: ShowText(s); break;
        }
    }
    Layout(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (hadFocus) SetFocus(FocusTarget(s));
    if (s.changed) s.changed();
}

// ---------- Fonts / DPI ----------

void UpdateFonts(ViewerState& s) {
    UINT dpi = GetWindowDpi(s.hwnd);
    HFONT ui = CreateScaledFont(App::UIFont(), dpi);
    HFONT bold = CreateScaledFont(App::UIFont(), dpi, FW_SEMIBOLD);
    HFONT mono = CreateScaledFont(App::MonoFont(), dpi);
    if (s.edit) SendMessageW(s.edit, WM_SETFONT, (WPARAM)mono, TRUE);
    for (HFONT f : {s.uiFont, s.uiBold, s.monoFont})
        if (f) DeleteObject(f);
    s.uiFont = ui;
    s.uiBold = bold;
    s.monoFont = mono;
    s.lineH = FontHeight(s.hwnd, ui);
    s.headerH = s.lineH + DpiScale(s.hwnd, 8);
    if (s.checker) {
        DeleteObject(s.checker);
        s.checker = nullptr;
    }
    Layout(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// ---------- Zeichnen ----------

std::wstring ModeText(const ViewerState& s) {
    switch (s.eff) {
    case Eff::Text: return std::wstring(L"Text · ") + EncodingShortName(s.textEnc);
    case Eff::Hex: return L"Hex";
    case Eff::Image: return s.imgLoading ? L"Bild (wird geladen)" : L"Bild";
    case Eff::Preview: return L"Vorschau";
    case Eff::Directory: return L"Verzeichnis";
    default: return L"";
    }
}

void PaintHeader(ViewerState& s, HDC dc, int width) {
    RECT hr{0, 0, width, s.headerH};
    FillRect(dc, &hr, GetSysColorBrush(COLOR_BTNFACE));
    HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, 0, s.headerH - 1, nullptr);
    LineTo(dc, width, s.headerH - 1);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    if (s.path.empty()) return;

    std::wstring name = PathFileName(s.path);
    if (name.empty()) name = s.path;
    std::wstring info;
    if (s.eff != Eff::Directory && s.eff != Eff::Message) info = FormatSize(s.fileSize);
    std::wstring mode = ModeText(s);
    if (!mode.empty()) info += (info.empty() ? L"" : L"  ·  ") + mode;
    if (!s.note.empty()) info += (info.empty() ? L"" : L"  ·  ") + s.note;

    int pad = DpiScale(s.hwnd, 6);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    HGDIOBJ oldFont = SelectObject(dc, s.uiFont);
    RECT ir{0, 0, 0, 0};
    DrawTextW(dc, info.c_str(), (int)info.size(), &ir, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int infoW = std::min<int>(ir.right - ir.left, std::max(0, width / 2));
    RECT infoRect{width - pad - infoW, 0, width - pad, s.headerH - 1};
    DrawTextW(dc, info.c_str(), (int)info.size(), &infoRect,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, s.uiBold);
    RECT nameRect{pad, 0, std::max<int>(pad, infoRect.left - 2 * pad), s.headerH - 1};
    DrawTextW(dc, name.c_str(), (int)name.size(), &nameRect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_PATH_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void PaintMessage(ViewerState& s, HDC dc, const RECT& cr, const std::wstring& text) {
    FillRect(dc, &cr, GetSysColorBrush(COLOR_WINDOW));
    if (text.empty()) return;
    int m = DpiScale(s.hwnd, 16);
    RECT r{cr.left + m, cr.top + m, cr.right - m, cr.bottom - m};
    if (r.right <= r.left || r.bottom <= r.top) return;
    HGDIOBJ oldFont = SelectObject(dc, s.uiFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    RECT calc = r;
    DrawTextW(dc, text.c_str(), (int)text.size(), &calc, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    int th = calc.bottom - calc.top;
    if (th < r.bottom - r.top) r.top += (r.bottom - r.top - th) / 3;
    DrawTextW(dc, text.c_str(), (int)text.size(), &r, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void PaintDirectory(ViewerState& s, HDC dc, const RECT& cr) {
    FillRect(dc, &cr, GetSysColorBrush(COLOR_WINDOW));
    HGDIOBJ oldFont = SelectObject(dc, s.uiFont);
    SetBkMode(dc, TRANSPARENT);
    int m = DpiScale(s.hwnd, 12);
    int labelW = 0;
    for (auto& [label, value] : s.dirInfo) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &sz);
        labelW = std::max<int>(labelW, sz.cx);
    }
    labelW += DpiScale(s.hwnd, 16);
    int y = cr.top + m;
    int lineH = s.lineH + DpiScale(s.hwnd, 4);
    for (auto& [label, value] : s.dirInfo) {
        RECT lr{cr.left + m, y, cr.left + m + labelW, y + lineH};
        RECT vr{cr.left + m + labelW, y, cr.right - m, y + lineH};
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, label.c_str(), (int)label.size(), &lr, DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SelectObject(dc, &label == &s.dirInfo.front().first ? s.uiBold : s.uiFont);
        DrawTextW(dc, value.c_str(), (int)value.size(), &vr, DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
        SelectObject(dc, s.uiFont);
        y += lineH;
    }
    SelectObject(dc, oldFont);
}

void Paint(ViewerState& s, HDC target) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    int W = std::max<int>(1, rc.right), H = std::max<int>(1, rc.bottom);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    PaintHeader(s, dc, W);
    RECT cr = ContentRect(s);
    switch (s.eff) {
    case Eff::Image:
        if (s.imgLoading)
            PaintMessage(s, dc, cr, L"Bild wird geladen…");
        else
            PaintImage(s, dc, cr);
        break;
    case Eff::Directory: PaintDirectory(s, dc, cr); break;
    case Eff::Message: PaintMessage(s, dc, cr, s.message); break;
    case Eff::None: PaintMessage(s, dc, cr, L""); break;
    default: FillRect(dc, &cr, GetSysColorBrush(COLOR_WINDOW)); break;   // von Kindfenstern verdeckt
    }
    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---------- Textsuche ----------

enum : int { IDC_TF_TEXT = 101, IDC_TF_CASE = 110, IDC_TF_WORD = 111, IDC_TF_BACK = 112 };

class TextFindDialog : public DialogBase {
public:
    std::wstring text;
    bool matchCase = false, wholeWord = false, backward = false;

protected:
    BOOL OnInit() override {
        SetText(IDC_TF_TEXT, text);
        SetCheck(IDC_TF_CASE, matchCase);
        SetCheck(IDC_TF_WORD, wholeWord);
        SetCheck(IDC_TF_BACK, backward);
        SendMessageW(Item(IDC_TF_TEXT), EM_SETSEL, 0, -1);
        SetFocus(Item(IDC_TF_TEXT));
        return FALSE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == IDOK) {
            text = GetText(IDC_TF_TEXT);
            matchCase = IsChecked(IDC_TF_CASE);
            wholeWord = IsChecked(IDC_TF_WORD);
            backward = IsChecked(IDC_TF_BACK);
            if (text.empty()) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            End(IDOK);
            return TRUE;
        }
        return DialogBase::OnCommand(id, code, ctl);
    }
};

bool TextFind(ViewerState& s, bool backward) {
    if (s.eff != Eff::Text || !s.edit || s.findText.empty()) return false;
    HWND owner = GetAncestor(s.hwnd, GA_ROOT);
    if (!s.richEdit) {
        // Rückfall für das einfache EDIT-Steuerelement
        std::wstring all = GetWindowTextStr(s.edit);
        DWORD a = 0, b = 0;
        SendMessageW(s.edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        auto eq = [&](wchar_t x, wchar_t y) { return s.findCase ? x == y : CharLowerW((LPWSTR)(UINT_PTR)x) == CharLowerW((LPWSTR)(UINT_PTR)y); };
        size_t pos = std::wstring::npos;
        if (!backward) {
            auto it = std::search(all.begin() + std::min<size_t>(b, all.size()), all.end(), s.findText.begin(), s.findText.end(), eq);
            if (it != all.end()) pos = (size_t)(it - all.begin());
        } else {
            auto end = all.begin() + std::min<size_t>(a, all.size());
            auto it = std::find_end(all.begin(), end, s.findText.begin(), s.findText.end(), eq);
            if (it != end) pos = (size_t)(it - all.begin());
        }
        if (pos == std::wstring::npos) {
            MsgInfo(owner, L"„" + s.findText + L"“ wurde nicht gefunden.");
            return false;
        }
        SendMessageW(s.edit, EM_SETSEL, pos, pos + s.findText.size());
        SendMessageW(s.edit, EM_SCROLLCARET, 0, 0);
        SetFocus(s.edit);
        return true;
    }
    CHARRANGE cr{};
    SendMessageW(s.edit, EM_EXGETSEL, 0, (LPARAM)&cr);
    FINDTEXTEXW ft{};
    if (!backward) {
        ft.chrg.cpMin = cr.cpMax;
        ft.chrg.cpMax = -1;
    } else {
        ft.chrg.cpMin = cr.cpMin;
        ft.chrg.cpMax = 0;
    }
    ft.lpstrText = s.findText.c_str();
    WPARAM flags = (backward ? 0 : FR_DOWN) | (s.findCase ? FR_MATCHCASE : 0) | (s.findWord ? FR_WHOLEWORD : 0);
    LRESULT pos = SendMessageW(s.edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
    if (pos < 0) {
        MsgInfo(owner, L"„" + s.findText + L"“ wurde nicht gefunden" +
                           (backward ? L" (Textanfang erreicht)." : L" (Textende erreicht)."));
        return false;
    }
    SendMessageW(s.edit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
    SendMessageW(s.edit, EM_SCROLLCARET, 0, 0);
    SetFocus(s.edit);
    return true;
}

void TextFindDialogShow(ViewerState& s) {
    if (s.eff != Eff::Text) return;
    DialogTemplate t(L"Suchen", 250, 78);
    t.Label(100, L"Suchen &nach:", 7, 9, 50, 8);
    t.Edit(IDC_TF_TEXT, 60, 7, 127, 14);
    t.Check(IDC_TF_CASE, L"&Groß-/Kleinschreibung beachten", 7, 28, 180, 10);
    t.Check(IDC_TF_WORD, L"Nur ganze &Wörter", 7, 41, 180, 10);
    t.Check(IDC_TF_BACK, L"&Rückwärts suchen", 7, 54, 180, 10);
    t.DefButton(IDOK, L"Suchen", 193, 7, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 193, 25, 50, 14);
    TextFindDialog dlg;
    dlg.text = s.findText;
    if (dlg.text.empty() && s.edit && s.richEdit) {
        // Vorbelegung mit einer kurzen einzeiligen Markierung
        CHARRANGE cr{};
        SendMessageW(s.edit, EM_EXGETSEL, 0, (LPARAM)&cr);
        if (cr.cpMax > cr.cpMin && cr.cpMax - cr.cpMin < 200) {
            std::wstring sel((size_t)(cr.cpMax - cr.cpMin) + 1, L'\0');
            LRESULT n = SendMessageW(s.edit, EM_GETSELTEXT, 0, (LPARAM)sel.data());
            sel.resize((size_t)std::max<LRESULT>(0, n));
            if (sel.find_first_of(L"\r\n") == std::wstring::npos) dlg.text = sel;
        }
    }
    dlg.matchCase = s.findCase;
    dlg.wholeWord = s.findWord;
    dlg.backward = s.findBack;
    if (dlg.DoModal(GetAncestor(s.hwnd, GA_ROOT), t) != IDOK) return;
    s.findText = dlg.text;
    s.findCase = dlg.matchCase;
    s.findWord = dlg.wholeWord;
    s.findBack = dlg.backward;
    TextFind(s, s.findBack);
}

// ---------- Unterklasse für das Textfeld: Esc/Tab an das Steuerelement weiterreichen ----------

LRESULT CALLBACK EditSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_TAB) {
            SendMessageW(GetParent(h), WM_KEYDOWN, wp, lp);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == 27 || wp == 9) return 0;
        break;
    case WM_GETDLGCODE: return DefSubclassProc(h, msg, wp, lp) & ~(LRESULT)(DLGC_WANTTAB);
    case WM_CONTEXTMENU: {
        HMENU m = CreatePopupMenu();
        DWORD a = 0, b = 0;
        SendMessageW(h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        AppendMenuW(m, MF_STRING | (a != b ? 0 : MF_GRAYED), 1, L"&Kopieren\tStrg+C");
        AppendMenuW(m, MF_STRING, 2, L"&Alles markieren\tStrg+A");
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x == -1 && y == -1) {
            POINT pt{0, 0};
            ClientToScreen(h, &pt);
            x = pt.x;
            y = pt.y;
        }
        int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, h, nullptr);
        DestroyMenu(m);
        if (cmd == 1) SendMessageW(h, WM_COPY, 0, 0);
        if (cmd == 2) SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_NCDESTROY: RemoveWindowSubclass(h, EditSubclassProc, id); break;
    default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void CreateChildren(ViewerState& s) {
    static HMODULE richEditLib = LoadLibraryW(L"Msftedit.dll");
    DWORD style = WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                  ES_NOHIDESEL | WS_TABSTOP;
    if (richEditLib)
        s.edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", style, 0, 0, 0, 0, s.hwnd, (HMENU)(INT_PTR)kIdEdit,
                                 App::Instance(), nullptr);
    s.richEdit = s.edit != nullptr;
    if (!s.edit)
        s.edit = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 0, 0, s.hwnd, (HMENU)(INT_PTR)kIdEdit, App::Instance(),
                                 nullptr);
    if (s.edit) {
        if (s.richEdit) {
            SendMessageW(s.edit, EM_SETTEXTMODE, TM_PLAINTEXT | TM_SINGLELEVELUNDO | TM_MULTICODEPAGE, 0);
            SendMessageW(s.edit, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
            SendMessageW(s.edit, EM_SETUNDOLIMIT, 0, 0);
            SendMessageW(s.edit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_WINDOW));
            SendMessageW(s.edit, EM_SETEVENTMASK, 0, 0);
        } else {
            SendMessageW(s.edit, EM_SETLIMITTEXT, 0, 0);
        }
        SetWindowSubclass(s.edit, EditSubclassProc, 1, 0);
    }
    s.hex = CreateHexView(s.hwnd, kIdHex, true);
    if (s.hex) ShowWindow(s.hex, SW_HIDE);
    s.wrap = App::Cfg().GetBool(kViewerSection, L"Umbruch", false);
    s.previewHost = CreateWindowExW(0, kPreviewHostClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                    s.hwnd, (HMENU)(INT_PTR)kIdPreviewHost, App::Instance(), nullptr);
    ApplyWrap(s);
}

void OnViewerKeyDown(ViewerState& s, WPARAM vk, LPARAM lp) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    int step = DpiScale(s.hwnd, 48);
    switch (vk) {
    case VK_ESCAPE:
    case VK_TAB:
        if (HWND p = GetParent(s.hwnd)) SendMessageW(p, WM_KEYDOWN, vk, lp);
        return;
    case VK_NEXT:
    case VK_PRIOR:
        if (s.navigate && (s.eff == Eff::Image || s.eff == Eff::Directory || s.eff == Eff::Message ||
                           s.eff == Eff::None || s.eff == Eff::Preview)) {
            s.navigate(vk == VK_NEXT ? 1 : -1);
        } else if (s.eff == Eff::Image) {
            RECT cr = ContentRect(s);
            Pan(s, 0, vk == VK_NEXT ? (cr.bottom - cr.top) : -(cr.bottom - cr.top));
        }
        return;
    default: break;
    }
    if (s.eff != Eff::Image) return;
    switch (vk) {
    case VK_LEFT: Pan(s, -step, 0); break;
    case VK_RIGHT: Pan(s, step, 0); break;
    case VK_UP: Pan(s, 0, -step); break;
    case VK_DOWN: Pan(s, 0, step); break;
    case VK_HOME: Pan(s, -1000000, -1000000); break;
    case VK_END: Pan(s, 1000000, 1000000); break;
    case VK_ADD:
    case VK_OEM_PLUS:
        if (!ctrl) ZoomStep(s, true);
        break;
    case VK_SUBTRACT:
    case VK_OEM_MINUS:
        if (!ctrl) ZoomStep(s, false);
        break;
    default: break;
    }
}

LRESULT CALLBACK PreviewHostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE) {
        // Vorschau an neue Größe anpassen
        ViewerState* s = GetViewer(GetParent(hwnd));
        if (s && s->preview) {
            RECT r{0, 0, LOWORD(lp), HIWORD(lp)};
            s->preview->SetRect(&r);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ViewerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ViewerState* s = GetViewer(hwnd);
    if (msg == WM_NCCREATE) {
        s = new ViewerState();
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        CreateChildren(*s);
        UpdateFonts(*s);
        return 0;
    case WM_DESTROY:
        Unload(*s);
        if (s->wic) s->wic->Release();
        s->wic = nullptr;
        if (s->edit) SendMessageW(s->edit, WM_SETFONT, 0, FALSE);
        for (HFONT f : {s->uiFont, s->uiBold, s->monoFont})
            if (f) DeleteObject(f);
        s->uiFont = s->uiBold = s->monoFont = nullptr;
        if (s->checker) DeleteObject(s->checker);
        s->checker = nullptr;
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete s;
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_DPICHANGED_AFTERPARENT: UpdateFonts(*s); return 0;
    case WM_SIZE:
        Layout(*s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(*s, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        if (s->edit && s->richEdit) SendMessageW(s->edit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_WINDOW));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_QF_IMAGEDONE: OnImageDone(*s, (UINT)wp); return 0;
    case WM_SETFOCUS: {
        HWND t = FocusTarget(*s);
        if (t != hwnd && IsWindowVisible(t)) SetFocus(t);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN: OnViewerKeyDown(*s, wp, lp); return 0;
    case WM_COMMAND:
        // Benachrichtigungen der Kindfenster (HexView-Status, EN_*) werden hier nicht benötigt.
        return 0;
    case WM_MOUSEWHEEL: {
        if (s->eff != Eff::Image || !s->img) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            double cur = ComputeGeom(*s).scale;
            ZoomAt(*s, delta > 0 ? cur * 1.25 : cur / 1.25, pt);
        } else if (ImagePannable(*s)) {
            Pan(*s, 0, -delta * DpiScale(hwnd, 60) / WHEEL_DELTA);
        } else if (s->navigate) {
            s->wheelAccum += delta;
            if (std::abs(s->wheelAccum) >= WHEEL_DELTA) {
                int dir = s->wheelAccum > 0 ? -1 : 1;
                s->wheelAccum = 0;
                s->navigate(dir);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (ImagePannable(*s)) {
            s->dragging = true;
            s->dragStart = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            s->dragPanX = s->panX;
            s->dragPanY = s->panY;
            SetCapture(hwnd);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        // Doppelklick: zwischen Einpassen und 100 % umschalten
        if (s->eff == Eff::Image && s->img) {
            if (s->fit && std::fabs(ComputeGeom(*s).scale - 1.0) > 1e-6)
                ZoomAt(*s, 1.0, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            else
                ZoomFit(*s);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (s->dragging) {
            s->panX = s->dragPanX - (GET_X_LPARAM(lp) - s->dragStart.x);
            s->panY = s->dragPanY - (GET_Y_LPARAM(lp) - s->dragStart.y);
            ComputeGeom(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s->dragging) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED: s->dragging = false; return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd && ImagePannable(*s)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterViewerClasses() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ViewerProc;
    wc.hInstance = App::Instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kViewerClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW ph{};
    ph.cbSize = sizeof(ph);
    ph.lpfnWndProc = PreviewHostProc;
    ph.hInstance = App::Instance();
    ph.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ph.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    ph.lpszClassName = kPreviewHostClass;
    RegisterClassExW(&ph);
    done = true;
}

// ===================== Anzeigefenster (F3) =====================

enum : UINT {
    IDM_V_OPEN = 2001,
    IDM_V_RELOAD,
    IDM_V_EDIT,
    IDM_V_HEXEDIT,
    IDM_V_SHELLOPEN,
    IDM_V_NEXT,
    IDM_V_PREV,
    IDM_V_CLOSE,
    IDM_V_MODE_AUTO,      // + (int)ViewerMode
    IDM_V_MODE_TEXT,
    IDM_V_MODE_HEX,
    IDM_V_MODE_IMAGE,
    IDM_V_MODE_PREVIEW,
    IDM_V_WRAP,
    IDM_V_ENC_AUTO,
    IDM_V_ENC_UTF8,
    IDM_V_ENC_UTF16LE,
    IDM_V_ENC_UTF16BE,
    IDM_V_ENC_ANSI,
    IDM_V_ENC_OEM,
    IDM_V_IMG_FIT,
    IDM_V_IMG_100,
    IDM_V_IMG_IN,
    IDM_V_IMG_OUT,
    IDM_V_FIND,
    IDM_V_FINDNEXT,
    IDM_V_FINDPREV,
    IDM_V_GOTO,
};

constexpr int kIdViewer = 100;

struct FrameState {
    HWND hwnd = nullptr;
    HWND viewer = nullptr;
    HACCEL accel = nullptr;
};

std::vector<HWND> g_frames;

FrameState* GetFrame(HWND h) { return reinterpret_cast<FrameState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void UpdateFrameTitle(FrameState& f) {
    ViewerState* s = GetViewer(f.viewer);
    std::wstring name = s ? PathFileName(s->path) : L"";
    if (s && name.empty()) name = s->path;
    std::wstring title = name.empty() ? L"QFiles Anzeige" : name + L" – QFiles Anzeige";
    SetWindowTextW(f.hwnd, title.c_str());
}

HMENU CreateFrameMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_V_OPEN, L"Ö&ffnen…\tStrg+O");
    AppendMenuW(file, MF_STRING, IDM_V_RELOAD, L"&Neu laden\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_V_EDIT, L"Im &Editor bearbeiten\tF4");
    AppendMenuW(file, MF_STRING, IDM_V_HEXEDIT, L"Im &Hex-Editor bearbeiten\tUmschalt+F4");
    AppendMenuW(file, MF_STRING, IDM_V_SHELLOPEN, L"Mit &Standardprogramm öffnen\tUmschalt+Eingabe");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_V_NEXT, L"N&ächste Datei\tStrg+→");
    AppendMenuW(file, MF_STRING, IDM_V_PREV, L"&Vorige Datei\tStrg+←");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_V_CLOSE, L"S&chließen\tEsc");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&Datei");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_V_MODE_AUTO, L"&Automatisch\t1");
    AppendMenuW(view, MF_STRING, IDM_V_MODE_TEXT, L"&Text\t2");
    AppendMenuW(view, MF_STRING, IDM_V_MODE_HEX, L"&Hex\t3");
    AppendMenuW(view, MF_STRING, IDM_V_MODE_IMAGE, L"&Bild\t4");
    AppendMenuW(view, MF_STRING, IDM_V_MODE_PREVIEW, L"&Vorschau\t5");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_V_WRAP, L"&Zeilenumbruch\tW");
    HMENU enc = CreatePopupMenu();
    AppendMenuW(enc, MF_STRING, IDM_V_ENC_AUTO, L"&Automatisch");
    AppendMenuW(enc, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(enc, MF_STRING, IDM_V_ENC_UTF8, L"UTF-&8");
    AppendMenuW(enc, MF_STRING, IDM_V_ENC_UTF16LE, L"UTF-16 &LE");
    AppendMenuW(enc, MF_STRING, IDM_V_ENC_UTF16BE, L"UTF-16 &BE");
    AppendMenuW(enc, MF_STRING, IDM_V_ENC_ANSI, L"A&NSI");
    AppendMenuW(enc, MF_STRING, IDM_V_ENC_OEM, L"&OEM (DOS)");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)enc, L"Text-&Kodierung");
    HMENU img = CreatePopupMenu();
    AppendMenuW(img, MF_STRING, IDM_V_IMG_FIT, L"&Einpassen\tStrg+0");
    AppendMenuW(img, MF_STRING, IDM_V_IMG_100, L"&100 %\tStrg+1");
    AppendMenuW(img, MF_STRING, IDM_V_IMG_IN, L"&Vergrößern\tStrg++");
    AppendMenuW(img, MF_STRING, IDM_V_IMG_OUT, L"Ver&kleinern\tStrg+-");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)img, L"B&ild");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&Ansicht");

    HMENU find = CreatePopupMenu();
    AppendMenuW(find, MF_STRING, IDM_V_FIND, L"&Suchen…\tStrg+F");
    AppendMenuW(find, MF_STRING, IDM_V_FINDNEXT, L"&Weitersuchen\tF3");
    AppendMenuW(find, MF_STRING, IDM_V_FINDPREV, L"&Rückwärts weitersuchen\tUmschalt+F3");
    AppendMenuW(find, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(find, MF_STRING, IDM_V_GOTO, L"&Gehe zu Offset…\tStrg+G");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)find, L"&Suchen");
    return bar;
}

HACCEL CreateFrameAccel() {
    ACCEL a[] = {
        {FVIRTKEY | FCONTROL, 'O', (WORD)IDM_V_OPEN},
        {FVIRTKEY, VK_F5, (WORD)IDM_V_RELOAD},
        {FVIRTKEY, VK_F4, (WORD)IDM_V_EDIT},
        {FVIRTKEY | FSHIFT, VK_F4, (WORD)IDM_V_HEXEDIT},
        {FVIRTKEY | FSHIFT, VK_RETURN, (WORD)IDM_V_SHELLOPEN},
        {FVIRTKEY | FCONTROL, VK_RIGHT, (WORD)IDM_V_NEXT},
        {FVIRTKEY | FCONTROL, VK_LEFT, (WORD)IDM_V_PREV},
        {FVIRTKEY, VK_ESCAPE, (WORD)IDM_V_CLOSE},
        {FVIRTKEY, '1', (WORD)IDM_V_MODE_AUTO},
        {FVIRTKEY, '2', (WORD)IDM_V_MODE_TEXT},
        {FVIRTKEY, '3', (WORD)IDM_V_MODE_HEX},
        {FVIRTKEY, '4', (WORD)IDM_V_MODE_IMAGE},
        {FVIRTKEY, '5', (WORD)IDM_V_MODE_PREVIEW},
        {FVIRTKEY, 'W', (WORD)IDM_V_WRAP},
        {FVIRTKEY | FCONTROL, '0', (WORD)IDM_V_IMG_FIT},
        {FVIRTKEY | FCONTROL, VK_NUMPAD0, (WORD)IDM_V_IMG_FIT},
        {FVIRTKEY | FCONTROL, '1', (WORD)IDM_V_IMG_100},
        {FVIRTKEY | FCONTROL, VK_NUMPAD1, (WORD)IDM_V_IMG_100},
        {FVIRTKEY | FCONTROL, VK_OEM_PLUS, (WORD)IDM_V_IMG_IN},
        {FVIRTKEY | FCONTROL, VK_ADD, (WORD)IDM_V_IMG_IN},
        {FVIRTKEY | FCONTROL, VK_OEM_MINUS, (WORD)IDM_V_IMG_OUT},
        {FVIRTKEY | FCONTROL, VK_SUBTRACT, (WORD)IDM_V_IMG_OUT},
        {FVIRTKEY | FCONTROL, 'F', (WORD)IDM_V_FIND},
        {FVIRTKEY, VK_F3, (WORD)IDM_V_FINDNEXT},
        {FVIRTKEY | FSHIFT, VK_F3, (WORD)IDM_V_FINDPREV},
        {FVIRTKEY | FCONTROL, 'G', (WORD)IDM_V_GOTO},
    };
    return CreateAcceleratorTableW(a, (int)(sizeof(a) / sizeof(a[0])));
}

void NavigateFile(FrameState& f, int dir) {
    ViewerState* s = GetViewer(f.viewer);
    if (!s || s->path.empty()) return;
    std::wstring folder = PathParent(s->path);
    if (folder.empty()) return;
    std::wstring cur = PathFileName(s->path);
    bool imagesOnly = s->eff == Eff::Image;
    std::vector<DirEntry> entries;
    if (!ListDirectory(folder, entries)) return;
    const Options& o = App::Opt();
    std::vector<std::wstring> names;
    for (auto& e : entries) {
        if (e.IsDir()) continue;
        bool isCur = EqualsI(e.name, cur);
        if (!isCur) {
            if (!o.showHidden && (e.attributes & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!o.showSystem && (e.attributes & FILE_ATTRIBUTE_SYSTEM)) continue;
            if (imagesOnly && !IsImageExt(LowerExt(e.name))) continue;
        }
        names.push_back(e.name);
    }
    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) { return CompareNatural(a, b) < 0; });
    long long idx = -1;
    for (size_t i = 0; i < names.size(); ++i)
        if (EqualsI(names[i], cur)) {
            idx = (long long)i;
            break;
        }
    long long next;
    if (idx >= 0) {
        next = idx + dir;
    } else {
        // aktuelle Datei nicht (mehr) vorhanden: Einfügeposition bestimmen
        long long pos = (long long)names.size();
        for (size_t i = 0; i < names.size(); ++i)
            if (CompareNatural(names[i], cur) > 0) {
                pos = (long long)i;
                break;
            }
        next = dir > 0 ? pos : pos - 1;
    }
    if (next < 0 || next >= (long long)names.size()) {
        MessageBeep(MB_OK);
        return;
    }
    LoadPath(*s, PathCombine(folder, names[(size_t)next]), true);
    UpdateFrameTitle(f);
}

void FrameCommand(FrameState& f, UINT id) {
    ViewerState* s = GetViewer(f.viewer);
    if (!s) return;
    bool isFile = !s->path.empty() && !(s->attributes & FILE_ATTRIBUTE_DIRECTORY) && s->eff != Eff::Message;
    switch (id) {
    case IDM_V_OPEN: {
        std::wstring dir = s->path.empty() ? App::ActivePaneContext().dir : PathParent(s->path);
        std::wstring p = OpenFileDialog(f.hwnd, L"Datei anzeigen", dir);
        if (!p.empty()) {
            LoadPath(*s, p, true);
            UpdateFrameTitle(f);
        }
        break;
    }
    case IDM_V_RELOAD: LoadPath(*s, s->path, true); break;
    case IDM_V_EDIT:
        if (isFile) OpenTextEditor(s->path);
        break;
    case IDM_V_HEXEDIT:
        if (isFile) OpenHexEditor(s->path);
        break;
    case IDM_V_SHELLOPEN:
        if (!s->path.empty()) ShellOpen(f.hwnd, s->path, L"", PathParent(s->path));
        break;
    case IDM_V_NEXT: NavigateFile(f, 1); break;
    case IDM_V_PREV: NavigateFile(f, -1); break;
    case IDM_V_CLOSE: PostMessageW(f.hwnd, WM_CLOSE, 0, 0); break;
    case IDM_V_MODE_AUTO:
    case IDM_V_MODE_TEXT:
    case IDM_V_MODE_HEX:
    case IDM_V_MODE_IMAGE:
    case IDM_V_MODE_PREVIEW: FileViewerSetMode(f.viewer, (ViewerMode)(id - IDM_V_MODE_AUTO)); break;
    case IDM_V_WRAP:
        s->wrap = !s->wrap;
        ApplyWrap(*s);
        App::Cfg().SetBool(kViewerSection, L"Umbruch", s->wrap);
        break;
    case IDM_V_ENC_AUTO:
    case IDM_V_ENC_UTF8:
    case IDM_V_ENC_UTF16LE:
    case IDM_V_ENC_UTF16BE:
    case IDM_V_ENC_ANSI:
    case IDM_V_ENC_OEM: {
        static const int map[] = {-1, (int)TextEncoding::Utf8, (int)TextEncoding::Utf16LE, (int)TextEncoding::Utf16BE,
                                  (int)TextEncoding::Ansi, (int)TextEncoding::Oem};
        s->forcedEnc = map[id - IDM_V_ENC_AUTO];
        if (s->forcedEnc >= 0 && s->eff != Eff::Text) s->mode = ViewerMode::Text;
        if (isFile) LoadPath(*s, s->path, true);
        break;
    }
    case IDM_V_IMG_FIT:
        if (s->eff == Eff::Image) ZoomFit(*s);
        break;
    case IDM_V_IMG_100:
        if (s->eff == Eff::Image) Zoom100(*s);
        break;
    case IDM_V_IMG_IN:
        if (s->eff == Eff::Image) ZoomStep(*s, true);
        break;
    case IDM_V_IMG_OUT:
        if (s->eff == Eff::Image) ZoomStep(*s, false);
        break;
    case IDM_V_FIND:
        if (s->eff == Eff::Text)
            TextFindDialogShow(*s);
        else if (s->eff == Eff::Hex)
            HexViewFindDialog(s->hex);
        else
            MessageBeep(MB_OK);
        break;
    case IDM_V_FINDNEXT:
    case IDM_V_FINDPREV: {
        bool back = id == IDM_V_FINDPREV;
        if (s->eff == Eff::Text) {
            if (s->findText.empty())
                TextFindDialogShow(*s);
            else
                TextFind(*s, back);
        } else if (s->eff == Eff::Hex) {
            HexViewFindNext(s->hex, back);
        } else {
            MessageBeep(MB_OK);
        }
        break;
    }
    case IDM_V_GOTO:
        if (s->eff == Eff::Hex)
            HexViewGotoDialog(s->hex);
        else
            MessageBeep(MB_OK);
        break;
    default: break;
    }
}

void FrameInitMenu(FrameState& f, HMENU m) {
    ViewerState* s = GetViewer(f.viewer);
    if (!s) return;
    bool isFile = !s->path.empty() && !(s->attributes & FILE_ATTRIBUTE_DIRECTORY) && s->eff != Eff::Message;
    CheckMenuRadioItem(m, IDM_V_MODE_AUTO, IDM_V_MODE_PREVIEW, IDM_V_MODE_AUTO + (UINT)s->mode, MF_BYCOMMAND);
    CheckMenuItem(m, IDM_V_WRAP, MF_BYCOMMAND | (s->wrap ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(m, IDM_V_WRAP, MF_BYCOMMAND | (s->richEdit ? MF_ENABLED : MF_GRAYED));
    UINT encId = IDM_V_ENC_AUTO;
    switch (s->forcedEnc) {
    case (int)TextEncoding::Utf8:
    case (int)TextEncoding::Utf8Bom: encId = IDM_V_ENC_UTF8; break;
    case (int)TextEncoding::Utf16LE: encId = IDM_V_ENC_UTF16LE; break;
    case (int)TextEncoding::Utf16BE: encId = IDM_V_ENC_UTF16BE; break;
    case (int)TextEncoding::Ansi: encId = IDM_V_ENC_ANSI; break;
    case (int)TextEncoding::Oem: encId = IDM_V_ENC_OEM; break;
    default: break;
    }
    CheckMenuRadioItem(m, IDM_V_ENC_AUTO, IDM_V_ENC_OEM, encId, MF_BYCOMMAND);
    UINT img = (s->eff == Eff::Image && s->img) ? MF_ENABLED : MF_GRAYED;
    for (UINT id : {IDM_V_IMG_FIT, IDM_V_IMG_100, IDM_V_IMG_IN, IDM_V_IMG_OUT}) EnableMenuItem(m, id, MF_BYCOMMAND | img);
    UINT findable = (s->eff == Eff::Text || s->eff == Eff::Hex) ? MF_ENABLED : MF_GRAYED;
    for (UINT id : {IDM_V_FIND, IDM_V_FINDNEXT, IDM_V_FINDPREV}) EnableMenuItem(m, id, MF_BYCOMMAND | findable);
    EnableMenuItem(m, IDM_V_GOTO, MF_BYCOMMAND | (s->eff == Eff::Hex ? MF_ENABLED : MF_GRAYED));
    UINT file = isFile ? MF_ENABLED : MF_GRAYED;
    for (UINT id : {IDM_V_EDIT, IDM_V_HEXEDIT}) EnableMenuItem(m, id, MF_BYCOMMAND | file);
    UINT any = s->path.empty() ? MF_GRAYED : MF_ENABLED;
    for (UINT id : {IDM_V_SHELLOPEN, IDM_V_RELOAD, IDM_V_NEXT, IDM_V_PREV}) EnableMenuItem(m, id, MF_BYCOMMAND | any);
}

LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    FrameState* f = GetFrame(hwnd);
    switch (msg) {
    case WM_NCCREATE:
        f = new FrameState();
        f->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)f);
        break;
    case WM_CREATE: {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)App::SmallIcon());
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)App::BigIcon());
        f->viewer = CreateFileViewer(hwnd, kIdViewer);
        if (ViewerState* s = GetViewer(f->viewer)) {
            s->embedded = false;
            s->textLimit = kTextLimitWindow;
            s->navigate = [hwnd](int dir) {
                if (FrameState* fs = GetFrame(hwnd)) NavigateFile(*fs, dir);
            };
            s->changed = [hwnd]() {
                if (FrameState* fs = GetFrame(hwnd)) UpdateFrameTitle(*fs);
            };
        }
        f->accel = CreateFrameAccel();
        App::RegisterAccelerator(hwnd, f->accel);
        g_frames.push_back(hwnd);
        return 0;
    }
    case WM_SIZE:
        if (f && f->viewer) MoveWindow(f->viewer, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        if (wp == SIZE_MAXIMIZED || wp == SIZE_RESTORED) SavePlacement(hwnd, kViewerSection);
        return 0;
    case WM_EXITSIZEMOVE: SavePlacement(hwnd, kViewerSection); return 0;
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETFOCUS:
        if (f && f->viewer) SetFocus(f->viewer);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !HIWORD(wp) && f && f->viewer) {
            HWND focus = GetFocus();
            if (!focus || !(focus == f->viewer || IsChild(f->viewer, focus))) SetFocus(f->viewer);
        }
        return 0;
    case WM_INITMENUPOPUP:
        if (f) FrameInitMenu(*f, (HMENU)wp);
        return 0;
    case WM_COMMAND:
        if (f && lp == 0) {
            FrameCommand(*f, LOWORD(wp));
            return 0;
        }
        break;
    case WM_KEYDOWN:
        // vom Anzeige-Steuerelement weitergereichte Tasten
        if (wp == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_DESTROY:
        SavePlacement(hwnd, kViewerSection);
        App::UnregisterAccelerator(hwnd);
        if (f) {
            if (f->accel) DestroyAcceleratorTable(f->accel);
            f->accel = nullptr;
            if (ViewerState* s = GetViewer(f->viewer)) {
                s->navigate = nullptr;
                s->changed = nullptr;
            }
        }
        g_frames.erase(std::remove(g_frames.begin(), g_frames.end(), hwnd), g_frames.end());
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete f;
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterFrameClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = App::Instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = App::BigIcon();
    wc.hIconSm = App::SmallIcon();
    wc.lpszClassName = kViewerFrameClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

// ===================== Öffentliche Schnittstelle (Modules.h) =====================

HWND CreateFileViewer(HWND parent, int id) {
    RegisterViewerClasses();
    return CreateWindowExW(0, kViewerClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 0, 0, parent,
                           (HMENU)(INT_PTR)id, App::Instance(), nullptr);
}

void FileViewerLoad(HWND viewer, const std::wstring& path) {
    if (ViewerState* s = GetViewer(viewer)) LoadPath(*s, path, false);
}

void FileViewerSetMode(HWND viewer, ViewerMode mode) {
    ViewerState* s = GetViewer(viewer);
    if (!s) return;
    s->mode = mode;
    if (mode != ViewerMode::Text && mode != ViewerMode::Auto) s->forcedEnc = -1;
    if (!s->path.empty()) LoadPath(*s, s->path, true);
}

ViewerMode FileViewerGetMode(HWND viewer) {
    ViewerState* s = GetViewer(viewer);
    return s ? s->mode : ViewerMode::Auto;
}

void OpenViewerWindow(const std::wstring& path) {
    RegisterFrameClass();
    RECT r{};
    bool maximized = false;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = CW_USEDEFAULT, h = CW_USEDEFAULT;
    if (LoadPlacement(kViewerSection, r, maximized)) {
        // weitere Fenster leicht versetzt öffnen
        int offset = (int)(g_frames.size() % 8) * GetSystemMetrics(SM_CYCAPTION);
        x = r.left + offset;
        y = r.top + offset;
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    HWND hwnd = CreateWindowExW(0, kViewerFrameClass, L"QFiles Anzeige", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h,
                                nullptr, CreateFrameMenu(), App::Instance(), nullptr);
    if (!hwnd) return;
    FrameState* f = GetFrame(hwnd);
    if (f && f->viewer) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(f->viewer, 0, 0, rc.right, rc.bottom, FALSE);
    }
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    if (f && f->viewer) {
        SetFocus(f->viewer);
        if (ViewerState* s = GetViewer(f->viewer)) LoadPath(*s, path, true);
        UpdateFrameTitle(*f);
    }
}

} // namespace qf
