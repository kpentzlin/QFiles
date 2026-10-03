#include "Encoding.h"
#include "App.h"
#include "Util.h"

#include <algorithm>

namespace qf {

const wchar_t* EncodingName(TextEncoding e) {
    switch (e) {
    case TextEncoding::Utf8: return L"UTF-8";
    case TextEncoding::Utf8Bom: return L"UTF-8 mit BOM";
    case TextEncoding::Utf16LE: return L"UTF-16 LE";
    case TextEncoding::Utf16BE: return L"UTF-16 BE";
    case TextEncoding::Ansi: return L"ANSI";
    case TextEncoding::Oem: return L"OEM (DOS)";
    }
    return L"?";
}

const wchar_t* LineEndingName(LineEnding e) {
    switch (e) {
    case LineEnding::CRLF: return L"CRLF (Windows)";
    case LineEnding::LF: return L"LF (Unix)";
    case LineEnding::CR: return L"CR (Mac klassisch)";
    }
    return L"?";
}

bool IsValidUtf8(const uint8_t* d, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = d[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        int len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) {
            // abgeschnittene Sequenz am Pufferende (z. B. Vorschau) tolerieren
            for (size_t k = i + 1; k < n; ++k)
                if ((d[k] & 0xC0) != 0x80) return false;
            return true;
        }
        for (int k = 1; k < len; ++k) {
            if ((d[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d[i + k] & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
            return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += len;
    }
    return true;
}

static bool LooksUtf16(const uint8_t* d, size_t n, bool& bigEndian) {
    if (n < 4) return false;
    size_t check = std::min<size_t>(n & ~size_t(1), 4096);
    size_t zeroEven = 0, zeroOdd = 0;
    for (size_t i = 0; i < check; i += 2) {
        if (d[i] == 0) ++zeroEven;
        if (d[i + 1] == 0) ++zeroOdd;
    }
    size_t pairs = check / 2;
    if (zeroOdd > pairs * 6 / 10 && zeroEven < pairs / 10) {
        bigEndian = false;
        return true;
    }
    if (zeroEven > pairs * 6 / 10 && zeroOdd < pairs / 10) {
        bigEndian = true;
        return true;
    }
    return false;
}

TextEncoding DetectEncoding(const uint8_t* d, size_t n, size_t* bomLength, TextEncoding defaultForEmpty) {
    if (bomLength) *bomLength = 0;
    if (n == 0) return defaultForEmpty;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
        if (bomLength) *bomLength = 3;
        return TextEncoding::Utf8Bom;
    }
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
        if (bomLength) *bomLength = 2;
        return TextEncoding::Utf16LE;
    }
    if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
        if (bomLength) *bomLength = 2;
        return TextEncoding::Utf16BE;
    }
    bool be = false;
    if (LooksUtf16(d, n, be)) return be ? TextEncoding::Utf16BE : TextEncoding::Utf16LE;
    // Reines ASCII oder gültiges UTF-8 -> UTF-8 (ohne BOM)
    if (IsValidUtf8(d, std::min<size_t>(n, 4 * 1024 * 1024))) return TextEncoding::Utf8;
    return TextEncoding::Ansi;
}

bool LooksBinary(const uint8_t* d, size_t n) {
    size_t check = std::min<size_t>(n, 8192);
    if (check == 0) return false;
    size_t bom = 0;
    TextEncoding e = DetectEncoding(d, check, &bom);
    if (e == TextEncoding::Utf16LE || e == TextEncoding::Utf16BE) return false;
    size_t ctrl = 0;
    for (size_t i = 0; i < check; ++i) {
        uint8_t c = d[i];
        if (c == 0) return true;
        if (c < 32 && c != 9 && c != 10 && c != 13 && c != 12 && c != 27 && c != 26 && c != 8) ++ctrl;
    }
    return ctrl * 20 > check; // mehr als 5 % Steuerzeichen
}

static std::wstring DecodeCodePage(UINT cp, const uint8_t* d, size_t n) {
    if (n == 0) return {};
    std::wstring r;
    // In Blöcken, da MultiByteToWideChar int-Längen erwartet
    size_t pos = 0;
    while (pos < n) {
        size_t chunk = std::min<size_t>(n - pos, 64 * 1024 * 1024);
        // UTF-8: nicht mitten in einer Sequenz trennen
        if (cp == CP_UTF8 && pos + chunk < n) {
            size_t full = chunk;
            while (chunk > 0 && (d[pos + chunk] & 0xC0) == 0x80) --chunk;
            if (chunk == 0) chunk = full; // nur Folgebytes (ungültig): sonst Endlosschleife
        }
        int m = MultiByteToWideChar(cp, 0, (const char*)d + pos, (int)chunk, nullptr, 0);
        size_t old = r.size();
        r.resize(old + m);
        MultiByteToWideChar(cp, 0, (const char*)d + pos, (int)chunk, r.data() + old, m);
        pos += chunk;
    }
    return r;
}

std::wstring DecodeText(const uint8_t* d, size_t n, TextEncoding enc) {
    switch (enc) {
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom:
        if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
            d += 3;
            n -= 3;
        }
        return DecodeCodePage(CP_UTF8, d, n);
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
        bool be = enc == TextEncoding::Utf16BE;
        if (n >= 2 && ((!be && d[0] == 0xFF && d[1] == 0xFE) || (be && d[0] == 0xFE && d[1] == 0xFF))) {
            d += 2;
            n -= 2;
        }
        std::wstring r(n / 2, L'\0');
        for (size_t i = 0; i < n / 2; ++i)
            r[i] = be ? (wchar_t)((d[2 * i] << 8) | d[2 * i + 1]) : (wchar_t)(d[2 * i] | (d[2 * i + 1] << 8));
        return r;
    }
    case TextEncoding::Ansi: return DecodeCodePage(CP_ACP, d, n);
    case TextEncoding::Oem: return DecodeCodePage(CP_OEMCP, d, n);
    }
    return {};
}

static std::vector<uint8_t> EncodeCodePage(UINT cp, const std::wstring& t, bool* unmappable) {
    std::vector<uint8_t> r;
    if (t.empty()) return r;
    BOOL used = FALSE;
    BOOL* pUsed = (cp == CP_UTF8) ? nullptr : &used;
    int n = WideCharToMultiByte(cp, 0, t.data(), (int)t.size(), nullptr, 0, nullptr, pUsed);
    r.resize(n);
    WideCharToMultiByte(cp, 0, t.data(), (int)t.size(), (char*)r.data(), n, nullptr, pUsed);
    if (unmappable) *unmappable = used != FALSE;
    return r;
}

std::vector<uint8_t> EncodeText(const std::wstring& text, TextEncoding enc, bool* unmappable) {
    if (unmappable) *unmappable = false;
    std::vector<uint8_t> r;
    switch (enc) {
    case TextEncoding::Utf8: return EncodeCodePage(CP_UTF8, text, nullptr);
    case TextEncoding::Utf8Bom: {
        r = {0xEF, 0xBB, 0xBF};
        auto b = EncodeCodePage(CP_UTF8, text, nullptr);
        r.insert(r.end(), b.begin(), b.end());
        return r;
    }
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
        bool be = enc == TextEncoding::Utf16BE;
        r.reserve(2 + text.size() * 2);
        r.push_back(be ? 0xFE : 0xFF);
        r.push_back(be ? 0xFF : 0xFE);
        for (wchar_t c : text) {
            if (be) {
                r.push_back((uint8_t)(c >> 8));
                r.push_back((uint8_t)(c & 0xFF));
            } else {
                r.push_back((uint8_t)(c & 0xFF));
                r.push_back((uint8_t)(c >> 8));
            }
        }
        return r;
    }
    case TextEncoding::Ansi: return EncodeCodePage(CP_ACP, text, unmappable);
    case TextEncoding::Oem: return EncodeCodePage(CP_OEMCP, text, unmappable);
    }
    return r;
}

LineEnding DetectLineEnding(const std::wstring& t, LineEnding def) {
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == L'\r') return (i + 1 < t.size() && t[i + 1] == L'\n') ? LineEnding::CRLF : LineEnding::CR;
        if (t[i] == L'\n') return LineEnding::LF;
    }
    return def;
}

std::wstring ConvertLineEndings(const std::wstring& t, LineEnding eol) {
    const wchar_t* nl = eol == LineEnding::CRLF ? L"\r\n" : (eol == LineEnding::LF ? L"\n" : L"\r");
    std::wstring r;
    r.reserve(t.size() + t.size() / 20);
    for (size_t i = 0; i < t.size(); ++i) {
        wchar_t c = t[i];
        if (c == L'\r') {
            if (i + 1 < t.size() && t[i + 1] == L'\n') ++i;
            r += nl;
        } else if (c == L'\n') {
            r += nl;
        } else {
            r += c;
        }
    }
    return r;
}

bool LoadTextFile(const std::wstring& path, TextFile& out, const TextEncoding* forced) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) return false;
    out.wasEmpty = bytes.empty();
    TextEncoding def = App::Opt().newFileEncoding;
    out.encoding = forced ? *forced : DetectEncoding(bytes.data(), bytes.size(), nullptr, def);
    out.text = DecodeText(bytes.data(), bytes.size(), out.encoding);
    out.lineEnding = DetectLineEnding(out.text, App::Opt().newFileLineEnding);
    return true;
}

bool SaveTextFile(const std::wstring& path, const std::wstring& text, TextEncoding enc, LineEnding eol, bool* unmappable) {
    std::wstring t = ConvertLineEndings(text, eol);
    auto bytes = EncodeText(t, enc, unmappable);
    return WriteFileBytes(path, bytes.data(), bytes.size());
}

} // namespace qf
