// DocText.cpp – Text aus ZIP-basierten Office-Dokumenten (siehe DocText.h).

#include "DocText.h"
#include "Util.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace qf {

// ===================== Deflate (RFC 1951) =====================

namespace {

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool err = false;
    int Bits(int need) {
        while (cnt < need) {
            if (pos >= n) {
                err = true;
                return 0;
            }
            buf |= (uint32_t)p[pos++] << cnt;
            cnt += 8;
        }
        int v = (int)(buf & ((1u << need) - 1));
        buf >>= need;
        cnt -= need;
        return v;
    }
};

struct Huffman {
    short count[16] = {};
    short symbol[288] = {};
};

// Kanonischen Huffman-Code aus den Codelängen aufbauen. Rückgabe < 0 = überbestimmt (ungültig).
int BuildHuffman(Huffman& h, const short* lengths, int n) {
    std::fill(std::begin(h.count), std::end(h.count), (short)0);
    for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16] = {};
    for (int len = 1; len < 15; ++len) offs[len + 1] = (short)(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (lengths[s]) h.symbol[offs[lengths[s]]++] = (short)s;
    return left;
}

int Decode(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= br.Bits(1);
        if (br.err) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const unsigned short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool InflateCodes(BitReader& br, std::string& out, size_t maxOut, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = Decode(br, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= maxOut) return false;
            out.push_back((char)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym] + br.Bits(kLenExtra[sym]);
            int ds = Decode(br, dist);
            if (ds < 0 || ds >= 30) return false;
            size_t d = kDistBase[ds] + (size_t)br.Bits(kDistExtra[ds]);
            if (br.err || d > out.size() || out.size() + (size_t)len > maxOut) return false;
            size_t from = out.size() - d;
            for (int k = 0; k < len; ++k) out.push_back(out[from + k]);
        }
    }
}

} // namespace

bool InflateRaw(const uint8_t* data, size_t size, std::string& out, size_t maxOut) {
    out.clear();
    BitReader br{data, size};
    int last = 0;
    do {
        last = br.Bits(1);
        int type = br.Bits(2);
        if (br.err) return false;
        if (type == 0) {
            // Gespeichert: auf Bytegrenze, LEN/NLEN
            br.buf = 0;
            br.cnt = 0;
            if (br.pos + 4 > size) return false;
            unsigned len = data[br.pos] | (data[br.pos + 1] << 8);
            unsigned nlen = data[br.pos + 2] | (data[br.pos + 3] << 8);
            br.pos += 4;
            if ((len ^ 0xFFFF) != nlen || br.pos + len > size || out.size() + len > maxOut) return false;
            out.append((const char*)data + br.pos, len);
            br.pos += len;
        } else if (type == 1) {
            static Huffman fixedLit, fixedDist;
            static bool init = false;
            if (!init) {
                short l[288];
                for (int s = 0; s < 144; ++s) l[s] = 8;
                for (int s = 144; s < 256; ++s) l[s] = 9;
                for (int s = 256; s < 280; ++s) l[s] = 7;
                for (int s = 280; s < 288; ++s) l[s] = 8;
                BuildHuffman(fixedLit, l, 288);
                for (int s = 0; s < 30; ++s) l[s] = 5;
                BuildHuffman(fixedDist, l, 30);
                init = true;
            }
            if (!InflateCodes(br, out, maxOut, fixedLit, fixedDist)) return false;
        } else if (type == 2) {
            int nlen = br.Bits(5) + 257, ndist = br.Bits(5) + 1, ncode = br.Bits(4) + 4;
            if (br.err || nlen > 286 || ndist > 30) return false;
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            short lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = (short)br.Bits(3);
            Huffman lencode;
            if (BuildHuffman(lencode, lengths, 19) != 0) return false;
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = Decode(br, lencode);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[idx++] = (short)sym;
                } else {
                    short len = 0;
                    int rep;
                    if (sym == 16) {
                        if (idx == 0) return false;
                        len = lengths[idx - 1];
                        rep = 3 + br.Bits(2);
                    } else if (sym == 17) {
                        rep = 3 + br.Bits(3);
                    } else {
                        rep = 11 + br.Bits(7);
                    }
                    if (idx + rep > nlen + ndist) return false;
                    while (rep--) lengths[idx++] = len;
                }
            }
            if (lengths[256] == 0) return false;
            Huffman lit, dist;
            int e1 = BuildHuffman(lit, lengths, nlen);
            if (e1 < 0 || (e1 > 0 && nlen - lit.count[0] != 1)) return false;
            int e2 = BuildHuffman(dist, lengths + nlen, ndist);
            if (e2 < 0 || (e2 > 0 && ndist - dist.count[0] != 1)) return false;
            if (!InflateCodes(br, out, maxOut, lit, dist)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

// ===================== ZIP und XML =====================

namespace {

enum class DocFamily { None, Word, Excel, PowerPoint, OpenDocument, Epub };

DocFamily FamilyOf(const std::wstring& path) {
    std::wstring ext = ToLower(PathExtension(path));
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
    for (const wchar_t* e : {L"docx", L"docm", L"dotx", L"dotm"})
        if (ext == e) return DocFamily::Word;
    for (const wchar_t* e : {L"xlsx", L"xlsm", L"xltx", L"xltm"})
        if (ext == e) return DocFamily::Excel;
    for (const wchar_t* e : {L"pptx", L"pptm", L"ppsx", L"ppsm", L"potx", L"potm"})
        if (ext == e) return DocFamily::PowerPoint;
    for (const wchar_t* e : {L"odt", L"ods", L"odp", L"odg", L"ott", L"ots", L"otp", L"odm"})
        if (ext == e) return DocFamily::OpenDocument;
    if (ext == L"epub") return DocFamily::Epub;
    return DocFamily::None;
}

bool EndsWith(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// Welche Teile des Archivs Text enthalten (Formatvorlagen, Designs usw. bleiben außen vor)
bool WantedPart(DocFamily f, std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](char c) { return (char)tolower((unsigned char)c); });
    switch (f) {
    case DocFamily::Word:
        if (!StartsWith(name, "word/") || !EndsWith(name, ".xml") || name.find('/', 5) != std::string::npos) return false;
        for (const char* p : {"word/document", "word/header", "word/footer", "word/footnotes", "word/endnotes", "word/comments"})
            if (StartsWith(name, p)) return true;
        return false;
    case DocFamily::Excel:
        return name == "xl/sharedstrings.xml" || (StartsWith(name, "xl/worksheets/sheet") && EndsWith(name, ".xml")) ||
               (StartsWith(name, "xl/comments") && EndsWith(name, ".xml"));
    case DocFamily::PowerPoint:
        return (StartsWith(name, "ppt/slides/slide") || StartsWith(name, "ppt/notesslides/") || StartsWith(name, "ppt/comments/")) &&
               EndsWith(name, ".xml");
    case DocFamily::OpenDocument: return name == "content.xml" || name == "styles.xml";
    case DocFamily::Epub: return EndsWith(name, ".xhtml") || EndsWith(name, ".html") || EndsWith(name, ".htm");
    default: return false;
    }
}

uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// Lokaler Name eines Tags ("w:p" -> "p")
std::string LocalName(const char* s, size_t n) {
    std::string name(s, n);
    size_t c = name.find(':');
    return c == std::string::npos ? name : name.substr(c + 1);
}

void AppendCodepoint(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// XML (UTF-8) -> reiner Text (UTF-8): Tags entfernen, Entitäten auflösen, Absätze/Zellen trennen
void XmlToText(const std::string& xml, std::string& out) {
    size_t i = 0, n = xml.size();
    while (i < n) {
        char c = xml[i];
        if (c == '<') {
            size_t end = xml.find('>', i);
            if (end == std::string::npos) break;
            if (xml.compare(i, 4, "<!--") == 0) {
                size_t e = xml.find("-->", i);
                i = e == std::string::npos ? n : e + 3;
                continue;
            }
            if (xml.compare(i, 9, "<![CDATA[") == 0) {
                size_t e = xml.find("]]>", i);
                size_t stop = e == std::string::npos ? n : e;
                out.append(xml, i + 9, stop - (i + 9));
                i = e == std::string::npos ? n : e + 3;
                continue;
            }
            // Tagname
            size_t s = i + 1;
            bool closing = s < end && xml[s] == '/';
            if (closing) ++s;
            size_t e = s;
            while (e < end && xml[e] != ' ' && xml[e] != '/' && xml[e] != '\t' && xml[e] != '\r' && xml[e] != '\n') ++e;
            std::string name = LocalName(xml.data() + s, e - s);
            bool selfClosing = end > i && xml[end - 1] == '/';
            // Trenner: Absatz, Tabulator, Zeilenumbruch, Tabellenzelle/-zeile, Folientext, Tabellenblattzelle
            if (name == "p" || name == "h" || name == "tr" || name == "row" || name == "br" || name == "cr" ||
                name == "line-break" || name == "div" || name == "li") {
                if (closing || selfClosing || name == "br" || name == "cr" || name == "line-break") out.push_back('\n');
            } else if (name == "tab" || name == "tc" || name == "c" || name == "si" || name == "s" || name == "td" ||
                       name == "table-cell" || name == "th") {
                if (closing || selfClosing || name == "tab" || name == "s") out.push_back(' ');
            }
            i = end + 1;
        } else if (c == '&') {
            size_t semi = xml.find(';', i);
            if (semi == std::string::npos || semi - i > 10) {
                out.push_back(c);
                ++i;
                continue;
            }
            std::string ent = xml.substr(i + 1, semi - i - 1);
            if (ent == "amp") out.push_back('&');
            else if (ent == "lt") out.push_back('<');
            else if (ent == "gt") out.push_back('>');
            else if (ent == "quot") out.push_back('"');
            else if (ent == "apos") out.push_back('\'');
            else if (ent == "nbsp") AppendCodepoint(out, 0xA0);
            else if (!ent.empty() && ent[0] == '#') {
                uint32_t cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? (uint32_t)strtoul(ent.c_str() + 2, nullptr, 16)
                                                                                    : (uint32_t)strtoul(ent.c_str() + 1, nullptr, 10);
                AppendCodepoint(out, cp);
            } else {
                out.append(xml, i, semi - i + 1);
            }
            i = semi + 1;
        } else {
            out.push_back(c);
            ++i;
        }
    }
}

constexpr size_t kMaxPart = 256u * 1024 * 1024;   // entpackte Größe je Teil
constexpr size_t kMaxFile = 512u * 1024 * 1024;   // Größe der Archivdatei

bool ExtractFromZip(DocFamily fam, const uint8_t* d, size_t n, std::wstring& text, const std::atomic<bool>* stop) {
    if (n < 22) return false;
    // Ende des zentralen Verzeichnisses suchen (höchstens 64 KB Kommentar)
    size_t minPos = n > 22 + 65535 ? n - 22 - 65535 : 0;
    size_t eocd = std::string::npos;
    for (size_t p = n - 22 + 1; p-- > minPos;)
        if (U32(d + p) == 0x06054b50) {
            eocd = p;
            break;
        }
    if (eocd == std::string::npos) return false;
    uint16_t entries = U16(d + eocd + 10);
    uint32_t cdSize = U32(d + eocd + 12), cdOff = U32(d + eocd + 16);
    if ((size_t)cdOff + cdSize > n) return false;
    std::string all;
    bool any = false;
    size_t p = cdOff;
    for (uint16_t k = 0; k < entries; ++k) {
        if (stop && *stop) return false;
        if (p + 46 > n || U32(d + p) != 0x02014b50) return false;
        uint16_t method = U16(d + p + 10);
        uint32_t csize = U32(d + p + 20), usize = U32(d + p + 24);
        uint16_t nameLen = U16(d + p + 28), extraLen = U16(d + p + 30), commentLen = U16(d + p + 32);
        uint32_t local = U32(d + p + 42);
        if (p + 46 + nameLen > n) return false;
        std::string name((const char*)d + p + 46, nameLen);
        p += 46 + (size_t)nameLen + extraLen + commentLen;
        if (!WantedPart(fam, name)) continue;
        if ((size_t)local + 30 > n || U32(d + local) != 0x04034b50) continue;
        size_t dataOff = (size_t)local + 30 + U16(d + local + 26) + U16(d + local + 28);
        if (csize == 0xFFFFFFFFu || dataOff + csize > n) continue;   // ZIP64-Teile nicht unterstützt
        std::string part;
        if (method == 0) {
            part.assign((const char*)d + dataOff, csize);
        } else if (method == 8) {
            if (!InflateRaw(d + dataOff, csize, part, std::min<size_t>(kMaxPart, (size_t)usize + 1024))) continue;
        } else {
            continue;
        }
        XmlToText(part, all);
        all.push_back('\n');
        any = true;
    }
    if (!any) return false;
    text = Utf8ToWide(all);
    return true;
}

} // namespace

bool IsDocumentContainer(const std::wstring& path) { return FamilyOf(path) != DocFamily::None; }

bool ExtractDocumentTextFromMemory(const std::wstring& nameForType, const std::vector<uint8_t>& data, std::wstring& text) {
    DocFamily f = FamilyOf(nameForType);
    if (f == DocFamily::None) return false;
    return ExtractFromZip(f, data.data(), data.size(), text, nullptr);
}

bool ExtractDocumentText(const std::wstring& path, std::wstring& text, const std::atomic<bool>* stop) {
    DocFamily f = FamilyOf(path);
    if (f == DocFamily::None) return false;
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 22 && (uint64_t)size.QuadPart <= kMaxFile;
    std::vector<uint8_t> data;
    if (ok) {
        data.resize((size_t)size.QuadPart);
        size_t done = 0;
        while (done < data.size()) {
            DWORD got = 0;
            DWORD want = (DWORD)std::min<size_t>(data.size() - done, 16u << 20);
            if (!ReadFile(h, data.data() + done, want, &got, nullptr) || got == 0) {
                ok = false;
                break;
            }
            done += got;
            if (stop && *stop) {
                ok = false;
                break;
            }
        }
    }
    CloseHandle(h);
    return ok && ExtractFromZip(f, data.data(), data.size(), text, stop);
}

} // namespace qf
