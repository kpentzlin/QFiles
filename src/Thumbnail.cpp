// Thumbnail.cpp – Miniaturen für die Kachelansicht der Listen. Siehe Thumbnail.h.

#include "Thumbnail.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>

#include "Util.h"

namespace qf {

namespace {

// Bildformate, die WIC (mit installiertem Codec) dekodieren kann
bool IsWicImageExt(const std::wstring& path) {
    static const wchar_t* exts[] = {L".bmp", L".dib", L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".gif", L".tif",
                                    L".tiff", L".ico", L".wdp", L".jxr", L".hdp", L".heic", L".heif", L".hif", L".avif",
                                    L".webp", L".dds", L".jxl", L".dng", L".cr2", L".cr3", L".nef", L".arw", L".orf",
                                    L".rw2", L".raf", L".pef", L".srw"};
    std::wstring e = ToLower(PathExtension(path));
    for (auto x : exts)
        if (e == x) return true;
    return false;
}

bool IsHeifExt(const std::wstring& path) {
    std::wstring e = ToLower(PathExtension(path));
    return e == L".heic" || e == L".heif" || e == L".hif" || e == L".avif";
}

HBITMAP ShellThumbnail(const std::wstring& path, int size, SIIGBF flags) {
    IShellItemImageFactory* f = nullptr;
    HBITMAP bmp = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) && f) {
        SIZE sz{size, size};
        if (FAILED(f->GetImage(sz, flags, &bmp))) bmp = nullptr;
        f->Release();
    }
    return bmp;
}

// Erstes Einzelbild eines WIC-Decoders, auf size eingepasst, als 32-Bit-DIB (vormultipliziertes Alpha)
HBITMAP BitmapFromDecoder(IWICImagingFactory* wic, IWICBitmapDecoder* dec, int size) {
    HBITMAP result = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(dec->GetFrame(0, &frame)) || !frame) return nullptr;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w && h) {
        double scale = std::min(1.0, std::min((double)size / w, (double)size / h));
        UINT tw = std::max<UINT>(1, (UINT)(w * scale)), th = std::max<UINT>(1, (UINT)(h * scale));
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
            BITMAPINFO bi{};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), (LONG)tw, -(LONG)th, 1, 32, BI_RGB};
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp && bits && SUCCEEDED(conv->CopyPixels(nullptr, tw * 4, tw * 4 * th, (BYTE*)bits))) {
                // AddThumbnail behandelt „alle Alpha = 0“ als deckend – hier ist Alpha gesetzt
                result = bmp;
            } else if (bmp) {
                DeleteObject(bmp);
            }
        }
        if (conv) conv->Release();
        if (scaler) scaler->Release();
    }
    frame->Release();
    return result;
}

HBITMAP WicThumbnailFile(const std::wstring& path, int size) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic)
        return nullptr;
    HBITMAP bmp = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(LongPath(path).c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) && dec) {
        bmp = BitmapFromDecoder(wic, dec, size);
        dec->Release();
    }
    wic->Release();
    return bmp;
}

HBITMAP WicThumbnailMemory(const std::vector<uint8_t>& data, int size) {
    IWICImagingFactory* wic = nullptr;
    if (data.empty() ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic)
        return nullptr;
    HBITMAP bmp = nullptr;
    IWICStream* stream = nullptr;
    if (SUCCEEDED(wic->CreateStream(&stream)) && stream &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), (DWORD)data.size()))) {
        IWICBitmapDecoder* dec = nullptr;
        if (SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && dec) {
            bmp = BitmapFromDecoder(wic, dec, size);
            dec->Release();
        }
    }
    if (stream) stream->Release();
    wic->Release();
    return bmp;
}

// ---- HEIF (ISO-BMFF): Exif-Element suchen, darin IFD1 mit JPEG-Vorschaubild ----

struct Be {
    const uint8_t* d;
    size_t n;
    bool Has(size_t o, size_t len) const { return o <= n && len <= n - o; }
    uint64_t U(size_t o, int bytes) const {
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | d[o + i];
        return v;
    }
};

// Kästen in [pos, end): Aufruf f(Typ, Nutzdatenbeginn, Ende); Rückgabe false = Abbruch
template <typename F> void ForBoxes(const Be& b, size_t pos, size_t end, F f) {
    for (int guard = 0; pos + 8 <= end && guard < 10000; ++guard) {
        uint64_t size = b.U(pos, 4);
        std::string type((const char*)b.d + pos + 4, 4);
        size_t hdr = 8;
        if (size == 1) {
            if (!b.Has(pos, 16)) return;
            size = b.U(pos + 8, 8);
            hdr = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < hdr || size > end - pos) return;
        if (!f(type, pos + hdr, pos + (size_t)size)) return;
        pos += (size_t)size;
    }
}

bool ExifIfd1Jpeg(const uint8_t* tiff, size_t n, std::vector<uint8_t>& jpeg) {
    if (n < 8) return false;
    bool le = tiff[0] == 'I' && tiff[1] == 'I';
    if (!le && !(tiff[0] == 'M' && tiff[1] == 'M')) return false;
    auto u16 = [&](size_t o) -> uint32_t {
        if (o + 2 > n) return 0;
        return le ? (tiff[o] | (tiff[o + 1] << 8)) : ((tiff[o] << 8) | tiff[o + 1]);
    };
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > n) return 0;
        return le ? (tiff[o] | (tiff[o + 1] << 8) | (tiff[o + 2] << 16) | ((uint32_t)tiff[o + 3] << 24))
                  : (((uint32_t)tiff[o] << 24) | (tiff[o + 1] << 16) | (tiff[o + 2] << 8) | tiff[o + 3]);
    };
    if (u16(2) != 42) return false;
    size_t ifd0 = u32(4);
    if (ifd0 + 2 > n) return false;
    size_t cnt0 = u16(ifd0);
    size_t ifd1 = u32(ifd0 + 2 + 12 * cnt0);
    if (!ifd1 || ifd1 + 2 > n) return false;
    size_t cnt1 = u16(ifd1);
    uint32_t off = 0, len = 0;
    for (size_t i = 0; i < cnt1 && i < 512; ++i) {
        size_t e = ifd1 + 2 + 12 * i;
        uint32_t tag = u16(e);
        if (tag == 0x0201) off = u32(e + 8);
        if (tag == 0x0202) len = u32(e + 8);
    }
    if (!off || !len || off >= n || len > n - off || len < 4) return false;
    if (tiff[off] != 0xFF || tiff[off + 1] != 0xD8) return false;
    jpeg.assign(tiff + off, tiff + off + len);
    return true;
}

} // namespace

bool HeifExifThumbnail(const std::vector<uint8_t>& file, std::vector<uint8_t>& jpeg) {
    Be b{file.data(), file.size()};
    uint32_t exifId = 0;
    bool found = false;
    uint64_t exifOff = 0, exifLen = 0;
    ForBoxes(b, 0, b.n, [&](const std::string& type, size_t p, size_t end) {
        if (type != "meta") return true;
        if (!b.Has(p, 4)) return false;
        // 1. iinf: Element vom Typ „Exif“
        ForBoxes(b, p + 4, end, [&](const std::string& t, size_t q, size_t qEnd) {
            if (t != "iinf" || !b.Has(q, 6)) return true;
            int v = b.d[q];
            size_t r = q + 4 + (v == 0 ? 2 : 4);
            ForBoxes(b, r, qEnd, [&](const std::string& it, size_t s, size_t) {
                if (it != "infe" || !b.Has(s, 12)) return true;
                int iv = b.d[s];
                if (iv < 2) return true;
                size_t x = s + 4;
                uint32_t id = (uint32_t)b.U(x, iv == 2 ? 2 : 4);
                x += (iv == 2 ? 2 : 4) + 2;
                if (b.Has(x, 4) && memcmp(b.d + x, "Exif", 4) == 0) {
                    exifId = id;
                    found = true;
                    return false;
                }
                return true;
            });
            return !found;
        });
        if (!found) return false;
        // 2. iloc: Lage des Elements in der Datei
        found = false;
        ForBoxes(b, p + 4, end, [&](const std::string& t, size_t q, size_t qEnd) {
            if (t != "iloc" || !b.Has(q, 8)) return true;
            int v = b.d[q];
            size_t r = q + 4;
            int offSize = b.d[r] >> 4, lenSize = b.d[r] & 15, baseSize = b.d[r + 1] >> 4;
            int idxSize = (v == 1 || v == 2) ? (b.d[r + 1] & 15) : 0;
            r += 2;
            uint32_t count = (uint32_t)b.U(r, v < 2 ? 2 : 4);
            r += v < 2 ? 2 : 4;
            for (uint32_t i = 0; i < count && r < qEnd; ++i) {
                if (!b.Has(r, 16)) return false;
                uint32_t id = (uint32_t)b.U(r, v < 2 ? 2 : 4);
                r += v < 2 ? 2 : 4;
                int method = 0;
                if (v == 1 || v == 2) {
                    method = (int)(b.U(r, 2) & 15);
                    r += 2;
                }
                r += 2;   // data_reference_index
                uint64_t base = baseSize ? b.U(r, baseSize) : 0;
                r += baseSize;
                uint32_t extents = (uint32_t)b.U(r, 2);
                r += 2;
                for (uint32_t e = 0; e < extents; ++e) {
                    if (!b.Has(r, idxSize + offSize + lenSize)) return false;
                    r += idxSize;
                    uint64_t off = offSize ? b.U(r, offSize) : 0;
                    r += offSize;
                    uint64_t len = lenSize ? b.U(r, lenSize) : 0;
                    r += lenSize;
                    if (id == exifId && method == 0 && e == 0) {
                        exifOff = base + off;
                        exifLen = len ? len : (b.n > exifOff ? b.n - exifOff : 0);
                        found = true;
                    }
                }
                if (found) return false;
            }
            return false;
        });
        return false;
    });
    if (!found || exifOff >= b.n || exifLen < 8 || exifLen > b.n - exifOff) return false;
    const uint8_t* ex = b.d + exifOff;
    size_t n = (size_t)exifLen;
    // 4 Byte Versatz zum TIFF-Kopf (meist 0 oder 6 für „Exif\0\0“), dann TIFF
    size_t hdr = (size_t)Be{ex, n}.U(0, 4);
    for (size_t start : {4 + hdr, (size_t)4, (size_t)10, (size_t)0}) {
        if (start + 8 > n) continue;
        if ((ex[start] == 'I' && ex[start + 1] == 'I') || (ex[start] == 'M' && ex[start + 1] == 'M'))
            if (ExifIfd1Jpeg(ex + start, n - start, jpeg)) return true;
    }
    return false;
}

HBITMAP CreateThumbnail(const std::wstring& path, int size, std::wstring* how) {
    auto done = [&](HBITMAP bmp, const wchar_t* h) {
        if (bmp && how) *how = h;
        return bmp;
    };
    // Testhilfe: QFILES_TEST_NOSHELLTHUMB überspringt die Miniaturen der Shell (prüft WIC/EXIF)
    static const bool noShell = GetEnvironmentVariableW(L"QFILES_TEST_NOSHELLTHUMB", nullptr, 0) > 0;
    if (!noShell)
        if (HBITMAP bmp = ShellThumbnail(path, size, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY))
            return done(bmp, L"Shell");
    if (IsWicImageExt(path))
        if (HBITMAP bmp = WicThumbnailFile(path, size)) return done(bmp, L"WIC");
    if (IsHeifExt(path)) {
        std::vector<uint8_t> file, jpeg;
        if (ReadFileBytes(path, file, 64u * 1024 * 1024) && HeifExifThumbnail(file, jpeg))
            if (HBITMAP bmp = WicThumbnailMemory(jpeg, size)) return done(bmp, L"EXIF");
    }
    if (HBITMAP bmp = ShellThumbnail(path, size, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY))
        return done(bmp, L"Symbol");
    return done(ShellThumbnail(path, size, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT), L"Symbol");
}

} // namespace qf
