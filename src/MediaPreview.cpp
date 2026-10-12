// MediaPreview.cpp – Vorschaubilder für PDF-Dateien (Windows.Data.Pdf) und Videos (Media Foundation).
// Siehe MediaPreview.h. Alle Systemfunktionen werden zur Laufzeit geladen (LoadLibrary/GetProcAddress), damit
// QFiles auch auf Systemen ohne diese Komponenten startet.

#include "MediaPreview.h"
#include "Util.h"

#include <inspectable.h>
#include <objidl.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

namespace qf {

namespace {

// ===================== Allgemeines =====================

template <class T>
class Ptr {
public:
    Ptr() = default;
    ~Ptr() { Reset(); }
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
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

private:
    T* p_ = nullptr;
};

const CLSID kClsidWic = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID kPbgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

constexpr int kSep = 2;                      // Trennstrich zwischen Seiten/Bildern (Pixel, schwarz)
constexpr uint32_t kBack = 0xFF404040;       // Hintergrund (wie die Bildanzeige)
constexpr uint32_t kSlot = 0xFF505050;       // Platzhalter für nicht gelesene Seiten
constexpr uint32_t kLine = 0xFF000000;

// Testhilfe: QFILES_TEST_MEDIADELAY=<ms> verzögert jede Seite bzw. jedes Einzelbild (Prüfen des Abbruchs mit Esc)
void TestDelay(const std::atomic<bool>& cancel) {
    static const int ms = [] {
        wchar_t buf[16] = {};
        return GetEnvironmentVariableW(L"QFILES_TEST_MEDIADELAY", buf, 16) ? _wtoi(buf) : 0;
    }();
    for (int t = 0; t < ms && !cancel; t += 20) Sleep(20);
}

std::wstring HexHr(HRESULT hr) {
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", (unsigned)hr);
    return buf;
}

std::wstring HrMessage(HRESULT hr) {
    std::wstring m = LastErrorMessage((DWORD)hr);
    if (m.empty() || m.rfind(L"Fehler ", 0) == 0) return L"Fehler " + HexHr(hr);
    return m;
}

// Zeichenfläche (DIB) für das Gesamtbild
struct Canvas {
    HBITMAP bmp = nullptr;
    uint32_t* px = nullptr;
    int w = 0, h = 0;
    bool Create(int cw, int ch) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cw;
        bi.bmiHeader.biHeight = -ch;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            bmp = nullptr;
            return false;
        }
        px = (uint32_t*)bits;
        w = cw;
        h = ch;
        std::fill(px, px + (size_t)w * h, kBack);
        return true;
    }
    void Fill(const RECT& r, uint32_t c) {
        for (int y = std::max<LONG>(0, r.top); y < std::min<LONG>(h, r.bottom); ++y)
            for (int x = std::max<LONG>(0, r.left); x < std::min<LONG>(w, r.right); ++x) px[(size_t)y * w + x] = c;
    }
    // Zentrierter, mehrzeiliger Text (GDI), danach deckend machen
    void Text(const RECT& r, const std::wstring& text, int fontPx) {
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        HFONT font = CreateFontW(-std::max(10, fontPx), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                 L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
        RECT box{r.left + 4, r.top + 4, r.right - 4, r.bottom - 4};
        RECT calc = box;
        DrawTextW(dc, text.c_str(), (int)text.size(), &calc, DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        int th = calc.bottom - calc.top;
        if (th < box.bottom - box.top) box.top += (box.bottom - box.top - th) / 2;
        DrawTextW(dc, text.c_str(), (int)text.size(), &box, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        SelectObject(dc, oldBmp);
        DeleteDC(dc);
        GdiFlush();
    }
    void MakeOpaque() {
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) px[i] |= 0xFF000000u;
    }
    void Detach(MediaResult& out) {
        MakeOpaque();
        GdiFlush();
        out.bmp = bmp;
        out.bits = px;
        out.w = w;
        out.h = h;
        bmp = nullptr;
        px = nullptr;
    }
    ~Canvas() {
        if (bmp) DeleteObject(bmp);
    }
};

// Kopiert ein WIC-Bild (beliebiges Format) skaliert an die Stelle dst der Zeichenfläche
bool DrawWic(IWICImagingFactory* wic, IWICBitmapSource* src, Canvas& c, const RECT& dst) {
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0 || dst.left < 0 || dst.top < 0 || dst.right > c.w || dst.bottom > c.h) return false;
    Ptr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, kPbgra, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT sw = 0, sh = 0;
    conv->GetSize(&sw, &sh);
    IWICBitmapSource* s = conv.Get();
    Ptr<IWICBitmapScaler> sc;
    if ((int)sw != dw || (int)sh != dh) {
        if (FAILED(wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(s, (UINT)dw, (UINT)dh, WICBitmapInterpolationModeFant)))
            return false;
        s = sc.Get();
    }
    UINT stride = (UINT)c.w * 4;
    BYTE* start = (BYTE*)(c.px + (size_t)dst.top * c.w + dst.left);
    UINT size = stride * (UINT)(dh - 1) + (UINT)dw * 4;
    return SUCCEEDED(s->CopyPixels(nullptr, stride, size, start));
}

// ===================== PDF (Windows.Data.Pdf) =====================

using HStringRaw = void*;

struct WinRtApi {
    HRESULT(WINAPI* getFactory)(HStringRaw, REFIID, void**) = nullptr;
    HRESULT(WINAPI* activate)(HStringRaw, IInspectable**) = nullptr;
    HRESULT(WINAPI* createString)(PCWSTR, UINT32, HStringRaw*) = nullptr;
    HRESULT(WINAPI* deleteString)(HStringRaw) = nullptr;
    HRESULT(WINAPI* streamOnFile)(PCWSTR, DWORD, REFIID, void**) = nullptr;     // CreateRandomAccessStreamOnFile
    HRESULT(WINAPI* streamOverRas)(IUnknown*, REFIID, void**) = nullptr;       // CreateStreamOverRandomAccessStream
    bool ok = false;
};

const WinRtApi& WinRt() {
    static WinRtApi api = [] {
        WinRtApi a;
        HMODULE combase = LoadLibraryW(L"combase.dll");
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (combase) {
            a.getFactory = (decltype(a.getFactory))(void*)GetProcAddress(combase, "RoGetActivationFactory");
            a.activate = (decltype(a.activate))(void*)GetProcAddress(combase, "RoActivateInstance");
            a.createString = (decltype(a.createString))(void*)GetProcAddress(combase, "WindowsCreateString");
            a.deleteString = (decltype(a.deleteString))(void*)GetProcAddress(combase, "WindowsDeleteString");
        }
        if (shcore) {
            a.streamOnFile = (decltype(a.streamOnFile))(void*)GetProcAddress(shcore, "CreateRandomAccessStreamOnFile");
            a.streamOverRas = (decltype(a.streamOverRas))(void*)GetProcAddress(shcore, "CreateStreamOverRandomAccessStream");
        }
        a.ok = a.getFactory && a.activate && a.createString && a.deleteString && a.streamOnFile && a.streamOverRas;
        return a;
    }();
    return api;
}

struct HStr {
    HStringRaw h = nullptr;
    explicit HStr(const wchar_t* s) { WinRt().createString(s, (UINT32)wcslen(s), &h); }
    ~HStr() {
        if (h) WinRt().deleteString(h);
    }
};

// Minimale ABI-Deklarationen (Windows.Foundation / Windows.Data.Pdf); nur die Reihenfolge der Methoden zählt
struct WinSize {
    float Width, Height;
};
struct WinRect {
    float X, Y, Width, Height;
};

struct IAsyncInfoRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;   // 0 Started, 1 Completed, 2 Canceled, 3 Error
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IAsyncOperationRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void**) = 0;
};
struct IAsyncActionRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};
struct IPdfPageRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IUnknown*, IAsyncActionRaw**) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IUnknown*, IUnknown*, IAsyncActionRaw**) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(IAsyncActionRaw**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(WinSize*) = 0;
};
struct IPdfDocumentRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32, IPdfPageRaw**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean*) = 0;
};
struct IPdfDocumentStaticsRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IUnknown*, IAsyncOperationRaw**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IUnknown*, HStringRaw, IAsyncOperationRaw**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IUnknown*, IAsyncOperationRaw**) = 0;
};
struct IPdfPageRenderOptionsRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(WinRect*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(WinRect) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32) = 0;
};

const IID kIidAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidPdfDocumentStatics = {0x433a0b5f, 0xc007, 0x4788, {0x90, 0xf2, 0x08, 0x14, 0x3d, 0x92, 0x25, 0x99}};
const IID kIidPdfPageRenderOptions = {0x3c98056f, 0xb7cf, 0x4c29, {0x9a, 0x04, 0x52, 0xd9, 0x02, 0x67, 0xf4, 0x25}};
const IID kIidRandomAccessStream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

// Wartet auf eine asynchrone WinRT-Operation; bei cancel wird sie abgebrochen (Rückgabe E_ABORT)
HRESULT WaitAsync(IInspectable* op, const std::atomic<bool>& cancel) {
    Ptr<IAsyncInfoRaw> info;
    HRESULT hr = op->QueryInterface(kIidAsyncInfo, (void**)&info);
    if (FAILED(hr)) return hr;
    bool cancelled = false;
    for (;;) {
        int st = 0;
        if (FAILED(info->get_Status(&st))) return E_FAIL;
        if (st == 1) return S_OK;
        if (st == 2) return E_ABORT;
        if (st == 3) {
            HRESULT e = E_FAIL;
            info->get_ErrorCode(&e);
            return FAILED(e) ? e : E_FAIL;
        }
        if (cancel && !cancelled) {
            info->Cancel();
            cancelled = true;
        }
        Sleep(5);
    }
}

// Rendert eine PDF-Seite auf die Größe von dst und kopiert sie in die Zeichenfläche
HRESULT RenderPdfPage(IPdfPageRaw* page, IWICImagingFactory* wic, Canvas& c, const RECT& dst, const std::atomic<bool>& cancel) {
    const WinRtApi& rt = WinRt();
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    Ptr<IInspectable> optObj;
    HRESULT hr = rt.activate(HStr(L"Windows.Data.Pdf.PdfPageRenderOptions").h, &optObj);
    if (FAILED(hr)) return hr;
    Ptr<IPdfPageRenderOptionsRaw> opt;
    hr = optObj->QueryInterface(kIidPdfPageRenderOptions, (void**)&opt);
    if (FAILED(hr)) return hr;
    opt->put_DestinationWidth((UINT32)dw);
    opt->put_DestinationHeight((UINT32)dh);
    Ptr<IInspectable> memObj;
    hr = rt.activate(HStr(L"Windows.Storage.Streams.InMemoryRandomAccessStream").h, &memObj);
    if (FAILED(hr)) return hr;
    Ptr<IUnknown> mem;
    hr = memObj->QueryInterface(kIidRandomAccessStream, (void**)&mem);
    if (FAILED(hr)) return hr;
    Ptr<IAsyncActionRaw> act;
    hr = page->RenderWithOptionsToStreamAsync(mem.Get(), opt.Get(), &act);
    if (FAILED(hr)) return hr;
    hr = WaitAsync(act.Get(), cancel);
    if (FAILED(hr)) return hr;
    Ptr<IStream> stm;
    hr = rt.streamOverRas(mem.Get(), IID_IStream, (void**)&stm);
    if (FAILED(hr)) return hr;
    LARGE_INTEGER zero{};
    stm->Seek(zero, STREAM_SEEK_SET, nullptr);
    Ptr<IWICBitmapDecoder> dec;
    hr = wic->CreateDecoderFromStream(stm.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) return hr;
    Ptr<IWICBitmapFrameDecode> frame;
    hr = dec->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;
    return DrawWic(wic, frame.Get(), c, dst) ? S_OK : E_FAIL;
}

void RenderPdf(const std::wstring& path, int boxW, int boxH, const std::atomic<bool>& cancel,
               const std::function<void(const std::wstring&)>& progress, MediaResult& out) {
    const std::wstring name = PathFileName(path);
    auto fail = [&](const std::wstring& why, bool unavailable = false) {
        out.failed = true;
        out.unavailable = unavailable;
        out.message = name + L"\nPDF kann nicht angezeigt werden: " + why;
    };
    const WinRtApi& rt = WinRt();
    if (!rt.ok) return fail(L"Windows.Data.Pdf ist nicht verfügbar.", true);
    Ptr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(kClsidWic, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return fail(L"WIC nicht verfügbar (" + HrMessage(hr) + L")");
    Ptr<IPdfDocumentStaticsRaw> statics;
    hr = rt.getFactory(HStr(L"Windows.Data.Pdf.PdfDocument").h, kIidPdfDocumentStatics, (void**)&statics);
    if (FAILED(hr)) return fail(L"Windows.Data.Pdf ist nicht verfügbar (" + HexHr(hr) + L").", true);
    Ptr<IUnknown> file;
    hr = rt.streamOnFile(path.c_str(), 0 /* FileAccessMode::Read */, kIidRandomAccessStream, (void**)&file);
    if (FAILED(hr)) return fail(HrMessage(hr));
    if (progress) progress(L"PDF wird geöffnet …");
    Ptr<IAsyncOperationRaw> op;
    hr = statics->LoadFromStreamAsync(file.Get(), &op);
    if (SUCCEEDED(hr)) hr = WaitAsync(op.Get(), cancel);
    if (hr == E_ABORT || cancel) {
        out.cancelled = true;
        out.message = name + L"\nSeite 1";
        return;
    }
    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD) || hr == (HRESULT)0x8007052B)
            return fail(L"Die Datei ist kennwortgeschützt.");
        return fail(L"Die Datei ist beschädigt oder kein gültiges PDF (" + HexHr(hr) + L").");
    }
    Ptr<IPdfDocumentRaw> doc;
    hr = op->GetResults((void**)&doc);
    if (FAILED(hr) || !doc) return fail(HrMessage(hr));
    UINT32 count = 0;
    doc->get_PageCount(&count);
    if (!count) return fail(L"Das Dokument enthält keine Seiten.");
    out.format = L"PDF · " + std::to_wstring(count) + (count == 1 ? L" Seite" : L" Seiten");

    // Seiten und Größen (in DIPs, Drehung berücksichtigt)
    int avail = (int)std::min<UINT32>(count, 4);
    std::vector<IPdfPageRaw*> pages(avail, nullptr);
    struct Guard {
        std::vector<IPdfPageRaw*>& v;
        ~Guard() {
            for (auto* p : v)
                if (p) p->Release();
        }
    } guard{pages};
    std::vector<WinSize> sz(avail, WinSize{595.0f, 842.0f});
    for (int i = 0; i < avail; ++i) {
        if (SUCCEEDED(doc->GetPage((UINT32)i, &pages[i])) && pages[i]) {
            WinSize s{};
            if (SUCCEEDED(pages[i]->get_Size(&s)) && s.Width > 0 && s.Height > 0) sz[i] = s;
        }
    }
    auto portrait = [&](int i) { return sz[i].Height > sz[i].Width; };
    // Aufteilung: 1 = eine Seite, 2 = zwei übereinander, 4 = 2×2 (bei 3 Seiten bleibt das vierte Feld leer)
    int layout, shown;
    if (count == 1) {
        layout = 1;
        shown = 1;
    } else if (count >= 4 && portrait(0) && portrait(1) && portrait(2) && portrait(3)) {
        layout = 4;
        shown = 4;
    } else if (count == 3 && portrait(0) && portrait(1) && portrait(2)) {
        layout = 4;
        shown = 3;
    } else {
        layout = 2;
        shown = 2;
    }
    boxW = std::max(64, boxW);
    boxH = std::max(64, boxH);
    // Felder in DIPs, dann gemeinsamer Maßstab
    std::vector<RECT> pageRects(shown);
    int W = 0, H = 0;
    std::vector<RECT> lines;
    if (layout == 1) {
        double s = std::min(boxW / (double)sz[0].Width, boxH / (double)sz[0].Height);
        W = std::max(1, (int)std::floor(sz[0].Width * s));
        H = std::max(1, (int)std::floor(sz[0].Height * s));
        pageRects[0] = RECT{0, 0, W, H};
    } else if (layout == 2) {
        double maxW = std::max(sz[0].Width, sz[1].Width), sumH = (double)sz[0].Height + sz[1].Height;
        double s = std::min(boxW / maxW, (boxH - kSep) / sumH);
        W = std::max(1, (int)std::floor(maxW * s));
        int h0 = std::max(1, (int)std::floor(sz[0].Height * s)), h1 = std::max(1, (int)std::floor(sz[1].Height * s));
        H = h0 + kSep + h1;
        int w0 = std::max(1, (int)std::floor(sz[0].Width * s)), w1 = std::max(1, (int)std::floor(sz[1].Width * s));
        pageRects[0] = RECT{(W - w0) / 2, 0, (W - w0) / 2 + w0, h0};
        pageRects[1] = RECT{(W - w1) / 2, h0 + kSep, (W - w1) / 2 + w1, H};
        lines.push_back(RECT{0, h0, W, h0 + kSep});
    } else {
        auto pw = [&](int i) { return i < shown ? (double)sz[i].Width : 0.0; };
        auto ph = [&](int i) { return i < shown ? (double)sz[i].Height : 0.0; };
        double c0 = std::max(pw(0), pw(2)), c1 = std::max(pw(1), pw(3));
        double r0 = std::max(ph(0), ph(1)), r1 = std::max(ph(2), ph(3));
        double s = std::min((boxW - kSep) / (c0 + c1), (boxH - kSep) / (r0 + r1));
        int cw0 = std::max(1, (int)std::floor(c0 * s)), cw1 = std::max(1, (int)std::floor(c1 * s));
        int rh0 = std::max(1, (int)std::floor(r0 * s)), rh1 = std::max(1, (int)std::floor(r1 * s));
        W = cw0 + kSep + cw1;
        H = rh0 + kSep + rh1;
        RECT cells[4] = {{0, 0, cw0, rh0}, {cw0 + kSep, 0, W, rh0}, {0, rh0 + kSep, cw0, H}, {cw0 + kSep, rh0 + kSep, W, H}};
        for (int i = 0; i < shown; ++i) {
            int w = std::max(1, (int)std::floor(sz[i].Width * s)), h = std::max(1, (int)std::floor(sz[i].Height * s));
            const RECT& c = cells[i];
            int x = c.left + (c.right - c.left - w) / 2, y = c.top + (c.bottom - c.top - h) / 2;
            pageRects[i] = RECT{x, y, x + w, y + h};
        }
        lines.push_back(RECT{cw0, 0, cw0 + kSep, H});
        lines.push_back(RECT{0, rh0, W, rh0 + kSep});
    }
    Canvas canvas;
    if (!canvas.Create(W, H)) return fail(L"Nicht genügend Speicher.");
    for (auto& l : lines) canvas.Fill(l, kLine);

    std::vector<bool> done(shown, false);
    std::vector<std::wstring> pageError(shown);
    for (int i = 0; i < shown && !cancel; ++i) {
        if (progress)
            progress(L"Seite " + std::to_wstring(i + 1) + L" von " + std::to_wstring(shown) + L" wird gelesen …");
        TestDelay(cancel);
        if (cancel) break;
        if (!pages[i]) {
            pageError[i] = L"nicht lesbar";
            continue;
        }
        hr = RenderPdfPage(pages[i], wic.Get(), canvas, pageRects[i], cancel);
        if (SUCCEEDED(hr))
            done[i] = true;
        else if (hr != E_ABORT)
            pageError[i] = HexHr(hr);
    }
    // Nicht gelesene Seiten: Dateiname und Seitennummer
    for (int i = 0; i < shown; ++i) {
        if (done[i]) continue;
        if (cancel) out.cancelled = true;
        const RECT& r = pageRects[i];
        canvas.Fill(r, kSlot);
        std::wstring t = name + L"\nSeite " + std::to_wstring(i + 1);
        if (!pageError[i].empty()) t += L"\n(Fehler " + pageError[i] + L")";
        int fontPx = std::clamp((int)(r.bottom - r.top) / 20, 12, 28);
        canvas.Text(r, t, fontPx);
    }
    canvas.Detach(out);
}

// ===================== Video (Media Foundation) =====================

const GUID kMfReaderVideoProcessing = {0xfb394f3d, 0xccf1, 0x42ee, {0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d}};
const GUID kMfMtMajorType = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
const GUID kMfMtSubtype = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
const GUID kMfMediaTypeVideo = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kMfVideoFormatRgb32 = {0x00000016, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kMfVideoFormatRgb24 = {0x00000014, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kMfVideoFormatNv12 = {0x3231564E, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

enum class PixFmt { Rgb32, Rgb24, Nv12 };
const GUID kMfMtFrameSize = {0x1652c33d, 0xd6b2, 0x4012, {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
const GUID kMfMtDefaultStride = {0x644b4e48, 0x1e02, 0x4516, {0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6}};
const GUID kMfPdDuration = {0x6c990d33, 0xbb8e, 0x477a, {0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a}};
const GUID kMfMtMinDisplayAperture = {0xd7388766, 0x18fe, 0x48c6, {0xa1, 0x77, 0xee, 0x89, 0x48, 0x67, 0xc8, 0xc4}};
const GUID kMfMtVideoRotation = {0xc380465d, 0x2271, 0x428c, {0x9b, 0x83, 0xec, 0xea, 0x3b, 0x4a, 0x85, 0xc1}};

struct MfApi {
    HRESULT(WINAPI* startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* shutdown)() = nullptr;
    HRESULT(WINAPI* createAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* createMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* createReader)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
    bool ok = false;
};

const MfApi& Mf() {
    static MfApi api = [] {
        MfApi a;
        HMODULE plat = LoadLibraryW(L"mfplat.dll");
        HMODULE rw = LoadLibraryW(L"mfreadwrite.dll");
        if (plat) {
            a.startup = (decltype(a.startup))(void*)GetProcAddress(plat, "MFStartup");
            a.shutdown = (decltype(a.shutdown))(void*)GetProcAddress(plat, "MFShutdown");
            a.createAttributes = (decltype(a.createAttributes))(void*)GetProcAddress(plat, "MFCreateAttributes");
            a.createMediaType = (decltype(a.createMediaType))(void*)GetProcAddress(plat, "MFCreateMediaType");
        }
        if (rw) a.createReader = (decltype(a.createReader))(void*)GetProcAddress(rw, "MFCreateSourceReaderFromURL");
        a.ok = a.startup && a.shutdown && a.createAttributes && a.createMediaType && a.createReader;
        return a;
    }();
    return api;
}

struct Frame {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
};

std::wstring FormatDuration(int64_t hns) {
    int64_t total = (hns + 5000000) / 10000000;   // gerundete Sekunden
    if (hns > 0 && hns < 100000000) {              // unter 10 s: mit Zehntelsekunden ("1,5 s")
        wchar_t buf[32];
        swprintf(buf, 32, L"%.1f s", hns / 1e7);
        for (wchar_t* c = buf; *c; ++c)
            if (*c == L'.') *c = L',';
        return buf;
    }
    int64_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
    wchar_t buf[32];
    if (h > 0)
        swprintf(buf, 32, L"%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    else
        swprintf(buf, 32, L"%lld:%02lld", (long long)m, (long long)s);
    return buf;
}

inline uint8_t Clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// YUV (BT.601, begrenzter Bereich) -> BGRA
inline uint32_t YuvToBgra(int y, int u, int v) {
    int c = (y - 16) * 298, d = u - 128, e = v - 128;
    int r = Clamp8((c + 409 * e + 128) >> 8), g = Clamp8((c - 100 * d - 208 * e + 128) >> 8), b = Clamp8((c + 516 * d + 128) >> 8);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Kopiert das Bild einer Probe (RGB32, RGB24 oder NV12) in f, beschnitten auf den sichtbaren Bereich und gedreht
bool CopySample(IMFSample* sample, PixFmt fmt, int width, int height, LONG defStride, const RECT& vis, UINT32 rotation,
                Frame& f) {
    Ptr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    Ptr<IMF2DBuffer> buf2d;
    bool locked2d = false;
    BYTE* data = nullptr;
    if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&buf2d))) && SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
        locked2d = true;
    } else {
        DWORD maxLen = 0, cur = 0;
        if (FAILED(buf->Lock(&data, &maxLen, &cur))) return false;
        int bpp = fmt == PixFmt::Rgb32 ? 4 : fmt == PixFmt::Rgb24 ? 3 : 1;
        LONG stride = defStride ? defStride : (LONG)((width * bpp + (fmt == PixFmt::Rgb24 ? 3 : 0)) & ~(fmt == PixFmt::Rgb24 ? 3 : 0));
        pitch = stride;
        scan0 = stride < 0 ? data + (size_t)(-stride) * (height - 1) : data;
        size_t need = (size_t)std::labs(stride) * height * (fmt == PixFmt::Nv12 ? 3 : 2) / 2;
        if (need > cur) {
            buf->Unlock();
            return false;
        }
    }
    int vw = vis.right - vis.left, vh = vis.bottom - vis.top;
    std::vector<uint32_t> src((size_t)vw * vh);
    for (int y = 0; y < vh; ++y) {
        int sy = y + vis.top;
        const BYTE* row = scan0 + (ptrdiff_t)pitch * sy;
        uint32_t* dst = src.data() + (size_t)y * vw;
        if (fmt == PixFmt::Rgb32) {
            const uint32_t* r32 = (const uint32_t*)row + vis.left;
            for (int x = 0; x < vw; ++x) dst[x] = r32[x] | 0xFF000000u;
        } else if (fmt == PixFmt::Rgb24) {
            const BYTE* r24 = row + vis.left * 3;
            for (int x = 0; x < vw; ++x)
                dst[x] = 0xFF000000u | ((uint32_t)r24[3 * x + 2] << 16) | ((uint32_t)r24[3 * x + 1] << 8) | r24[3 * x];
        } else {
            // NV12: Y-Ebene, darunter U/V verschränkt mit halber Auflösung
            const BYTE* uv = scan0 + (ptrdiff_t)pitch * height + (ptrdiff_t)pitch * (sy / 2);
            for (int x = 0; x < vw; ++x) {
                int sx = x + vis.left;
                dst[x] = YuvToBgra(row[sx], uv[(sx & ~1)], uv[(sx & ~1) + 1]);
            }
        }
    }
    if (locked2d)
        buf2d->Unlock2D();
    else
        buf->Unlock();
    // Drehung (Uhrzeigersinn)
    if (rotation == 90 || rotation == 270) {
        f.w = vh;
        f.h = vw;
        f.px.assign((size_t)f.w * f.h, 0);
        for (int y = 0; y < vh; ++y)
            for (int x = 0; x < vw; ++x) {
                int nx = rotation == 90 ? vh - 1 - y : y;
                int ny = rotation == 90 ? x : vw - 1 - x;
                f.px[(size_t)ny * f.w + nx] = src[(size_t)y * vw + x];
            }
    } else if (rotation == 180) {
        f.w = vw;
        f.h = vh;
        f.px.assign(src.rbegin(), src.rend());
    } else {
        f.w = vw;
        f.h = vh;
        f.px = std::move(src);
    }
    return true;
}

void RenderVideo(const std::wstring& path, int boxW, int boxH, const std::atomic<bool>& cancel,
                 const std::function<void(const std::wstring&)>& progress, MediaResult& out) {
    const std::wstring name = PathFileName(path);
    auto fail = [&](const std::wstring& why, bool unavailable = false) {
        out.failed = true;
        out.unavailable = unavailable;
        out.message = name + L"\nFrame-Anzeige nicht möglich" + (why.empty() ? L"" : L":\n" + why);
    };
    auto cancelled = [&] {
        out.cancelled = true;
        out.message = name + L"\nFrame-Anzeige abgebrochen";
    };
    const MfApi& mf = Mf();
    if (!mf.ok) return fail(L"Media Foundation ist auf diesem System nicht verfügbar.", true);
    HRESULT hr = mf.startup(0x00020070 /* MF_VERSION */, 1 /* MFSTARTUP_LITE */);
    if (FAILED(hr)) return fail(L"Media Foundation kann nicht gestartet werden (" + HexHr(hr) + L").", true);
    struct MfGuard {
        const MfApi& mf;
        ~MfGuard() { mf.shutdown(); }
    } mfGuard{mf};
    Ptr<IWICImagingFactory> wic;
    hr = CoCreateInstance(kClsidWic, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return fail(L"WIC nicht verfügbar (" + HrMessage(hr) + L")");

    if (progress) progress(L"Video wird geöffnet …");
    Ptr<IMFAttributes> attrs;
    if (SUCCEEDED(mf.createAttributes(&attrs, 1))) attrs->SetUINT32(kMfReaderVideoProcessing, TRUE);
    Ptr<IMFSourceReader> reader;
    hr = mf.createReader(path.c_str(), attrs.Get(), &reader);
    if (FAILED(hr)) return fail(L"Format wird nicht unterstützt oder Datei beschädigt (" + HexHr(hr) + L").");
    if (cancel) return cancelled();
    const DWORD kVideo = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = reader->SetStreamSelection(kVideo, TRUE);
    if (FAILED(hr)) return fail(L"Die Datei enthält keine Videospur.");

    // Ursprüngliches Format: Auflösung und Drehung
    UINT32 rotation = 0;
    UINT64 nativeSize = 0;
    {
        Ptr<IMFMediaType> native;
        if (SUCCEEDED(reader->GetNativeMediaType(kVideo, 0, &native))) {
            native->GetUINT64(kMfMtFrameSize, &nativeSize);
            native->GetUINT32(kMfMtVideoRotation, &rotation);
        }
    }
    rotation %= 360;
    // Ausgabeformat: RGB32 (Videoverarbeitung von Media Foundation), sonst NV12 bzw. RGB24 (selbst umgerechnet)
    PixFmt fmt = PixFmt::Rgb32;
    {
        const std::pair<const GUID*, PixFmt> candidates[] = {
            {&kMfVideoFormatRgb32, PixFmt::Rgb32}, {&kMfVideoFormatNv12, PixFmt::Nv12}, {&kMfVideoFormatRgb24, PixFmt::Rgb24}};
        HRESULT first = E_FAIL;
        bool set = false;
        for (auto& [sub, pf] : candidates) {
            Ptr<IMFMediaType> want;
            hr = mf.createMediaType(&want);
            if (SUCCEEDED(hr)) hr = want->SetGUID(kMfMtMajorType, kMfMediaTypeVideo);
            if (SUCCEEDED(hr)) hr = want->SetGUID(kMfMtSubtype, *sub);
            if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(kVideo, nullptr, want.Get());
            if (SUCCEEDED(hr)) {
                fmt = pf;
                set = true;
                break;
            }
            if (first == E_FAIL) first = hr;
        }
        if (!set) return fail(L"Kein passender Video-Decoder (" + HexHr(first) + L").");
    }
    Ptr<IMFMediaType> cur;
    hr = reader->GetCurrentMediaType(kVideo, &cur);
    if (FAILED(hr)) return fail(HexHr(hr));
    UINT64 frameSize = 0;
    cur->GetUINT64(kMfMtFrameSize, &frameSize);
    int width = (int)(frameSize >> 32), height = (int)(frameSize & 0xFFFFFFFF);
    if (width <= 0 || height <= 0) return fail(L"Ungültige Bildgröße.");
    UINT32 strideRaw = 0;
    LONG defStride = SUCCEEDED(cur->GetUINT32(kMfMtDefaultStride, &strideRaw)) ? (LONG)(INT32)strideRaw : 0;
    RECT vis{0, 0, width, height};
    {
        MFVideoArea area{};
        UINT32 got = 0;
        if (SUCCEEDED(cur->GetBlob(kMfMtMinDisplayAperture, (UINT8*)&area, sizeof(area), &got)) && got == sizeof(area)) {
            RECT a{area.OffsetX.value, area.OffsetY.value, area.OffsetX.value + area.Area.cx, area.OffsetY.value + area.Area.cy};
            if (a.left >= 0 && a.top >= 0 && a.right <= width && a.bottom <= height && a.right > a.left && a.bottom > a.top)
                vis = a;
        }
    }
    int dispW = vis.right - vis.left, dispH = vis.bottom - vis.top;
    if (nativeSize) {
        int nw = (int)(nativeSize >> 32), nh = (int)(nativeSize & 0xFFFFFFFF);
        if (nw > 0 && nh > 0 && std::abs(nw - dispW) <= 16 && std::abs(nh - dispH) <= 16) {
            dispW = nw;   // Anzeige wie im Explorer (z. B. 1920 × 1080 statt 1920 × 1088)
            dispH = nh;
        }
    }
    if (rotation == 90 || rotation == 270) std::swap(dispW, dispH);

    // Dauer
    int64_t duration = 0;
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, kMfPdDuration, &pv)) &&
            pv.vt == VT_UI8)
            duration = (int64_t)pv.uhVal.QuadPart;
        PropVariantClear(&pv);
    }
    out.band = L"Auflösung " + IntToStrGrouped((unsigned long long)dispW) + L" × " + IntToStrGrouped((unsigned long long)dispH) +
               L"  ·  Dauer " + (duration > 0 ? FormatDuration(duration) : std::wstring(L"unbekannt"));
    std::wstring ext = ToLower(PathExtension(path));
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
    out.format = L"Video" + (ext.empty() ? std::wstring() : L" · " + ToUpper(ext));

    // Zeitpunkte
    const int64_t sec = 10000000;
    std::vector<int64_t> times;
    bool landscape = dispW > dispH;
    if (duration <= 0)
        times = {0};
    else if (duration < 2 * sec)
        times = {duration / 2};
    else if (landscape)
        times = {sec, duration / 2, duration - sec};
    else
        times = {sec, duration / 3, duration * 2 / 3, duration - sec};

    std::vector<Frame> frames(times.size());
    for (size_t i = 0; i < times.size(); ++i) {
        if (cancel) return cancelled();
        if (progress)
            progress(L"Frame " + std::to_wstring(i + 1) + L" von " + std::to_wstring(times.size()) + L" wird gelesen …");
        TestDelay(cancel);
        if (cancel) return cancelled();
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = times[i];
        reader->SetCurrentPosition(GUID_NULL, pos);
        Ptr<IMFSample> best;
        for (int n = 0; n < 2000; ++n) {
            if (cancel) return cancelled();
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            Ptr<IMFSample> sample;
            hr = reader->ReadSample(kVideo, 0, &stream, &flags, &ts, &sample);
            if (FAILED(hr)) break;
            if (sample) {
                best.Reset();
                IMFSample* raw = sample.Get();
                raw->AddRef();
                *(&best) = raw;
                if (ts + sec / 50 >= times[i]) break;   // Zeitpunkt erreicht (Toleranz 20 ms)
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                Ptr<IMFMediaType> nt;
                if (SUCCEEDED(reader->GetCurrentMediaType(kVideo, &nt))) {
                    UINT64 fs = 0;
                    nt->GetUINT64(kMfMtFrameSize, &fs);
                    if (fs) {
                        width = (int)(fs >> 32);
                        height = (int)(fs & 0xFFFFFFFF);
                        vis = RECT{0, 0, width, height};
                    }
                    defStride = SUCCEEDED(nt->GetUINT32(kMfMtDefaultStride, &strideRaw)) ? (LONG)(INT32)strideRaw : 0;
                }
            }
        }
        if (!best) {
            if (FAILED(hr)) return fail(L"Video kann nicht dekodiert werden (" + HexHr(hr) + L").");
            if (i == 0) return fail(L"Keine Einzelbilder gefunden.");
            frames[i] = frames[i - 1];
            continue;
        }
        if (!CopySample(best.Get(), fmt, width, height, defStride, vis, rotation, frames[i]))
            return fail(L"Einzelbild kann nicht gelesen werden.");
    }

    // Gesamtbild: 1 Bild, 3 übereinander oder 2×2
    boxW = std::max(64, boxW);
    boxH = std::max(64, boxH);
    int fw = frames[0].w, fh = frames[0].h;
    int cols = frames.size() == 4 ? 2 : 1, rows = frames.size() == 4 ? 2 : (int)frames.size();
    double s = std::min((boxW - kSep * (cols - 1)) / (double)(fw * cols), (boxH - kSep * (rows - 1)) / (double)(fh * rows));
    int cw = std::max(1, (int)std::floor(fw * s)), ch = std::max(1, (int)std::floor(fh * s));
    int W = cw * cols + kSep * (cols - 1), H = ch * rows + kSep * (rows - 1);
    Canvas canvas;
    if (!canvas.Create(W, H)) return fail(L"Nicht genügend Speicher.");
    if (cols == 2) canvas.Fill(RECT{cw, 0, cw + kSep, H}, kLine);
    for (int r = 1; r < rows; ++r) canvas.Fill(RECT{0, r * (ch + kSep) - kSep, W, r * (ch + kSep)}, kLine);
    for (size_t i = 0; i < frames.size(); ++i) {
        int c = (int)i % cols, r = (int)i / cols;
        RECT dst{c * (cw + kSep), r * (ch + kSep), c * (cw + kSep) + cw, r * (ch + kSep) + ch};
        Frame& f = frames[i];
        Ptr<IWICBitmap> bmp;
        if (SUCCEEDED(wic->CreateBitmapFromMemory((UINT)f.w, (UINT)f.h, kPbgra, (UINT)f.w * 4, (UINT)(f.px.size() * 4),
                                                  (BYTE*)f.px.data(), &bmp)))
            DrawWic(wic.Get(), bmp.Get(), canvas, dst);
    }
    canvas.Detach(out);
}

} // namespace

MediaKind MediaKindForPath(const std::wstring& path) {
    std::wstring ext = ToLower(PathExtension(path));
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
    if (ext == L"pdf") return MediaKind::Pdf;
    static const wchar_t* video[] = {L"mp4", L"m4v", L"mov", L"avi", L"wmv", L"asf", L"mkv", L"webm", L"mpg",
                                     L"mpeg", L"m2v", L"ts", L"mts", L"m2ts", L"3gp", L"3g2", L"flv", L"vob"};
    for (const wchar_t* v : video)
        if (ext == v) return MediaKind::Video;
    return MediaKind::None;
}

void RenderMediaPreview(MediaKind kind, const std::wstring& path, int boxW, int boxH, const std::atomic<bool>& cancel,
                        const std::function<void(const std::wstring&)>& progress, MediaResult& out) {
    if (kind == MediaKind::Pdf)
        RenderPdf(path, boxW, boxH, cancel, progress, out);
    else if (kind == MediaKind::Video)
        RenderVideo(path, boxW, boxH, cancel, progress, out);
}

} // namespace qf
