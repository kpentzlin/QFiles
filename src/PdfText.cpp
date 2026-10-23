// PdfText.cpp – Text aus PDF-Dateien für die Textsuche (Suchen, F3). Siehe PdfText.h.
//
// Vorgehen: Die Datei wird nach „n g obj … endobj“ durchsucht (ohne Querverweistabelle, damit auch beschädigte oder
// inkrementell geänderte Dateien gelesen werden; spätere Fassungen eines Objekts ersetzen frühere), Objektströme
// werden entpackt. Dann werden alle Seitenobjekte (/Type /Page) ausgewertet: Inhaltsströme samt Formular-XObjekten,
// Schriften aus den Ressourcen (auch geerbt). Die Reihenfolge der Seiten spielt für die Suche keine Rolle.
// Leerzeichen und Zeilenwechsel werden aus den Textpositionen geschätzt (Zeichenbreiten der Schrift).

#include "PdfText.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "DocText.h"
#include "Util.h"

namespace qf {

namespace {

constexpr size_t kMaxStream = 256u * 1024 * 1024;   // entpackte Größe je Strom
constexpr size_t kMaxText = 64u * 1024 * 1024;      // Zeichen insgesamt

// ===================== Objekte und Zerlegung =====================

struct Obj;
using P = std::shared_ptr<Obj>;
enum class K { Null, Bool, Num, Str, Name, Arr, Dict, Ref, Kw };

struct Obj {
    K k = K::Null;
    double num = 0;
    std::string s;   // Zeichenkette (Bytes), Name oder Schlüsselwort
    std::vector<P> arr;
    std::vector<std::pair<std::string, P>> dict;
    int rn = 0, rg = 0;   // Ref: Ziel; Strom: eigene Objektnummer/Generation (Entschlüsselung)
    bool stream = false;
    std::string raw;      // Rohdaten des Stroms
    P Get(const char* key) const {
        for (auto& kv : dict)
            if (kv.first == key) return kv.second;
        return nullptr;
    }
};

inline bool IsWs(uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
inline bool IsDelim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
           c == '%';
}
inline int HexVal(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

P MakeKw(const std::string& s) {
    auto o = std::make_shared<Obj>();
    o->k = K::Kw;
    o->s = s;
    return o;
}

struct Lexer {
    const uint8_t* p;
    const uint8_t* e;

    void SkipWs() {
        while (p < e) {
            if (IsWs(*p)) {
                ++p;
            } else if (*p == '%') {
                while (p < e && *p != '\n' && *p != '\r') ++p;
            } else {
                break;
            }
        }
    }

    bool ReadInt(long long& v) {
        const uint8_t* q = p;
        bool neg = false;
        if (q < e && (*q == '+' || *q == '-')) neg = *q++ == '-';
        if (q >= e || *q < '0' || *q > '9') return false;
        long long x = 0;
        while (q < e && *q >= '0' && *q <= '9') x = x * 10 + (*q++ - '0');
        if (q < e && !IsWs(*q) && !IsDelim(*q)) return false;
        v = neg ? -x : x;
        p = q;
        return true;
    }

    P Parse(int depth = 0) {
        SkipWs();
        if (p >= e || depth > 64) return nullptr;
        uint8_t c = *p;
        auto o = std::make_shared<Obj>();
        if (c == '/') {
            ++p;
            o->k = K::Name;
            while (p < e && !IsWs(*p) && !IsDelim(*p)) {
                if (*p == '#' && p + 2 < e && HexVal(p[1]) >= 0 && HexVal(p[2]) >= 0) {
                    o->s += (char)(HexVal(p[1]) * 16 + HexVal(p[2]));
                    p += 3;
                } else {
                    o->s += (char)*p++;
                }
            }
            return o;
        }
        if (c == '(') {
            ++p;
            o->k = K::Str;
            int level = 1;
            while (p < e) {
                uint8_t ch = *p++;
                if (ch == '\\') {
                    if (p >= e) break;
                    ch = *p++;
                    switch (ch) {
                    case 'n': o->s += '\n'; break;
                    case 'r': o->s += '\r'; break;
                    case 't': o->s += '\t'; break;
                    case 'b': o->s += '\b'; break;
                    case 'f': o->s += '\f'; break;
                    case '\r':
                        if (p < e && *p == '\n') ++p;
                        break;
                    case '\n': break;
                    default:
                        if (ch >= '0' && ch <= '7') {
                            int v = ch - '0';
                            for (int i = 0; i < 2 && p < e && *p >= '0' && *p <= '7'; ++i) v = v * 8 + (*p++ - '0');
                            o->s += (char)(v & 0xFF);
                        } else {
                            o->s += (char)ch;
                        }
                        break;
                    }
                } else if (ch == '(') {
                    ++level;
                    o->s += '(';
                } else if (ch == ')') {
                    if (--level == 0) break;
                    o->s += ')';
                } else {
                    o->s += (char)ch;
                }
            }
            return o;
        }
        if (c == '<') {
            if (p + 1 < e && p[1] == '<') {
                p += 2;
                o->k = K::Dict;
                for (;;) {
                    SkipWs();
                    if (p >= e) break;
                    if (p + 1 < e && p[0] == '>' && p[1] == '>') {
                        p += 2;
                        break;
                    }
                    P key = Parse(depth + 1);
                    if (!key) break;
                    if (key->k != K::Name) continue;   // fehlerhafter Eintrag: überspringen
                    P val = Parse(depth + 1);
                    if (!val) break;
                    if (val->k == K::Kw && (val->s == ">" || val->s == "]")) continue;
                    o->dict.emplace_back(key->s, val);
                }
                return o;
            }
            ++p;
            o->k = K::Str;
            int hi = -1;
            while (p < e && *p != '>') {
                int v = HexVal(*p++);
                if (v < 0) continue;
                if (hi < 0) {
                    hi = v;
                } else {
                    o->s += (char)(hi * 16 + v);
                    hi = -1;
                }
            }
            if (hi >= 0) o->s += (char)(hi * 16);
            if (p < e) ++p;
            return o;
        }
        if (c == '[') {
            ++p;
            o->k = K::Arr;
            for (;;) {
                SkipWs();
                if (p >= e) break;
                if (*p == ']') {
                    ++p;
                    break;
                }
                P v = Parse(depth + 1);
                if (!v) break;
                o->arr.push_back(v);
                if (o->arr.size() > 1000000) break;
            }
            return o;
        }
        if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
            ++p;
            if (c == '>' && p < e && *p == '>') ++p;
            return MakeKw(std::string(1, (char)c));
        }
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            // Zahl, ggf. „n g R“
            const uint8_t* start = p;
            long long a = 0;
            if (ReadInt(a)) {
                const uint8_t* save = p;
                SkipWs();
                long long g = 0;
                if (a >= 0 && ReadInt(g) && g >= 0) {
                    SkipWs();
                    if (p < e && *p == 'R' && (p + 1 >= e || IsWs(p[1]) || IsDelim(p[1]))) {
                        ++p;
                        o->k = K::Ref;
                        o->rn = (int)a;
                        o->rg = (int)g;
                        return o;
                    }
                }
                p = save;
                o->k = K::Num;
                o->num = (double)a;
                return o;
            }
            p = start;
            std::string t;
            while (p < e && !IsWs(*p) && !IsDelim(*p)) t += (char)*p++;
            o->k = K::Num;
            o->num = atof(t.c_str());
            return o;
        }
        std::string t;
        while (p < e && !IsWs(*p) && !IsDelim(*p)) t += (char)*p++;
        if (t.empty()) {   // unbekanntes Zeichen
            ++p;
            return MakeKw("");
        }
        if (t == "true" || t == "false") {
            o->k = K::Bool;
            o->num = t == "true";
            return o;
        }
        if (t == "null") return o;
        return MakeKw(t);
    }
};

// ===================== Kryptografie (Standard-Sicherheitshandler) =====================

std::string Hash(LPCWSTR alg, const std::string& data) {
    std::string out;
    BCRYPT_ALG_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&h, alg, nullptr, 0) != 0) return out;
    DWORD len = 0, cb = 0;
    BCryptGetProperty(h, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof(len), &cb, 0);
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (len && BCryptCreateHash(h, &hh, nullptr, 0, nullptr, 0, 0) == 0) {
        out.resize(len);
        BCryptHashData(hh, (PUCHAR)data.data(), (ULONG)data.size(), 0);
        if (BCryptFinishHash(hh, (PUCHAR)out.data(), len, 0) != 0) out.clear();
        BCryptDestroyHash(hh);
    }
    BCryptCloseAlgorithmProvider(h, 0);
    return out;
}

// AES-CBC ohne Auffüllen (Länge ein Vielfaches von 16)
std::string AesCbc(const std::string& key, const std::string& iv, const std::string& data, bool encrypt) {
    std::string out;
    if (data.empty() || data.size() % 16 || iv.size() != 16) return out;
    BCRYPT_ALG_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return out;
    BCryptSetProperty(h, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    BCRYPT_KEY_HANDLE kh = nullptr;
    if (BCryptGenerateSymmetricKey(h, &kh, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) == 0) {
        std::string ivc = iv;
        out.resize(data.size());
        ULONG got = 0;
        NTSTATUS st = encrypt ? BCryptEncrypt(kh, (PUCHAR)data.data(), (ULONG)data.size(), nullptr, (PUCHAR)ivc.data(),
                                              16, (PUCHAR)out.data(), (ULONG)out.size(), &got, 0)
                              : BCryptDecrypt(kh, (PUCHAR)data.data(), (ULONG)data.size(), nullptr, (PUCHAR)ivc.data(),
                                              16, (PUCHAR)out.data(), (ULONG)out.size(), &got, 0);
        if (st != 0) out.clear();
        else out.resize(got);
        BCryptDestroyKey(kh);
    }
    BCryptCloseAlgorithmProvider(h, 0);
    return out;
}

std::string Rc4(const std::string& key, const std::string& data) {
    uint8_t S[256];
    for (int i = 0; i < 256; ++i) S[i] = (uint8_t)i;
    if (key.empty()) return data;
    for (int i = 0, j = 0; i < 256; ++i) {
        j = (j + S[i] + (uint8_t)key[i % key.size()]) & 0xFF;
        std::swap(S[i], S[j]);
    }
    std::string out(data.size(), '\0');
    for (size_t n = 0, i = 0, j = 0; n < data.size(); ++n) {
        i = (i + 1) & 0xFF;
        j = (j + S[i]) & 0xFF;
        std::swap(S[i], S[j]);
        out[n] = (char)(data[n] ^ S[(S[i] + S[j]) & 0xFF]);
    }
    return out;
}

// Algorithmus 2.B (PDF 2.0, R6) für das leere Benutzerkennwort
std::string Hash2B(const std::string& pwd, const std::string& salt, const std::string& udata) {
    std::string k = Hash(BCRYPT_SHA256_ALGORITHM, pwd + salt + udata);
    if (k.size() != 32) return "";
    for (int round = 0;; ++round) {
        std::string k1;
        std::string unit = pwd + k + udata;
        for (int i = 0; i < 64; ++i) k1 += unit;
        std::string ee = AesCbc(k.substr(0, 16), k.substr(16, 16), k1, true);
        if (ee.size() < 16) return "";
        int sum = 0;
        for (int i = 0; i < 16; ++i) sum += (uint8_t)ee[i];
        switch (sum % 3) {
        case 0: k = Hash(BCRYPT_SHA256_ALGORITHM, ee); break;
        case 1: k = Hash(BCRYPT_SHA384_ALGORITHM, ee); break;
        default: k = Hash(BCRYPT_SHA512_ALGORITHM, ee); break;
        }
        if (k.size() < 32) return "";
        if (round >= 63 && (uint8_t)ee.back() <= round - 32) break;
        if (round > 2000) break;
    }
    return k.substr(0, 32);
}

struct Crypt {
    bool on = false;
    bool aes = false;
    bool v5 = false;
    bool identity = false;   // Ströme unverschlüsselt (/StmF /Identity)
    std::string key;

    std::string Decrypt(const std::string& data, int num, int gen) const {
        if (!on || identity) return data;
        std::string k = key;
        if (!v5) {
            std::string m = key;
            m += (char)(num & 0xFF);
            m += (char)((num >> 8) & 0xFF);
            m += (char)((num >> 16) & 0xFF);
            m += (char)(gen & 0xFF);
            m += (char)((gen >> 8) & 0xFF);
            if (aes) m += "sAlT";
            k = Hash(BCRYPT_MD5_ALGORITHM, m).substr(0, std::min<size_t>(key.size() + 5, 16));
        }
        if (!aes) return Rc4(k, data);
        if (data.size() < 32) return "";
        size_t n = (data.size() - 16) / 16 * 16;
        std::string out = AesCbc(k, data.substr(0, 16), data.substr(16, n), false);
        if (!out.empty()) {   // PKCS#7 entfernen (tolerant)
            uint8_t pad = (uint8_t)out.back();
            if (pad >= 1 && pad <= 16 && pad <= out.size()) out.resize(out.size() - pad);
        }
        return out;
    }
};

// ===================== Dokument =====================

struct Font {
    bool twoByte = false;
    std::vector<std::tuple<uint32_t, uint32_t, int>> ranges;   // Codebereiche (von, bis, Bytes)
    std::unordered_map<uint32_t, std::wstring> toUni;
    std::wstring simple[256];
    bool hasSimple = false;
    std::unordered_map<uint32_t, double> widths;   // 1/1000 Geviert
    double defWidth = 500;
};

class PdfDoc {
public:
    PdfDoc(const uint8_t* d, size_t n, const std::atomic<bool>* stop) : d_(d), n_(n), stop_(stop) {}

    bool Run(std::wstring& text);

private:
    bool Stopped() const { return stop_ && stop_->load(std::memory_order_relaxed); }
    void ScanObjects();
    void ReadObjectStreams();
    void SetupCrypt();
    P Resolve(P o, int depth = 0) const;
    P Dict(P o) const {
        o = Resolve(o);
        return o && o->k == K::Dict ? o : nullptr;
    }
    std::string NameOf(P o) const {
        o = Resolve(o);
        return o && o->k == K::Name ? o->s : std::string();
    }
    double NumOf(P o, double def = 0) const {
        o = Resolve(o);
        return o && o->k == K::Num ? o->num : def;
    }
    std::string DecodeStream(P s);
    std::shared_ptr<Font> GetFont(P fontObj);
    void ParseCMap(const std::string& data, Font& f);
    void RunContent(const std::string& data, P resources, int depth);
    void Emit(const std::wstring& s);
    void Space();
    void NewLine();

    const uint8_t* d_;
    size_t n_;
    const std::atomic<bool>* stop_;
    std::map<int, P> objs_;
    std::vector<P> trailers_;
    Crypt crypt_;
    std::unordered_map<const Obj*, std::shared_ptr<Font>> fonts_;
    std::set<const Obj*> formsActive_;
    std::wstring out_;

    // Textzustand
    std::shared_ptr<Font> font_;
    double fontSize_ = 10, leading_ = 0;
    double ta_ = 1, td_ = 1;           // Skalierung der Textmatrix (horizontal, vertikal)
    double lineE_ = 0, lineF_ = 0;     // Zeilenanfang (Benutzerraum)
    double curX_ = 0;                  // Fortschritt seit Zeilenanfang (Textraum)
    double lastE_ = 0, lastF_ = 0;     // Ende des zuletzt ausgegebenen Texts (Geräteraum, über die CTM)
    bool haveLast_ = false;
    struct Ctm {
        double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    };
    Ctm ctm_;
    std::vector<Ctm> ctmStack_;
};

P PdfDoc::Resolve(P o, int depth) const {
    while (o && o->k == K::Ref && depth++ < 32) {
        auto it = objs_.find(o->rn);
        o = it == objs_.end() ? nullptr : it->second;
    }
    return o;
}

void PdfDoc::ScanObjects() {
    const uint8_t* b = d_;
    const uint8_t* end = d_ + n_;
    size_t pos = 0;
    while (pos + 3 < n_) {
        if (Stopped()) return;
        const uint8_t* hit = (const uint8_t*)memchr(b + pos, 'o', n_ - pos);
        if (!hit) break;
        size_t at = hit - b;
        pos = at + 1;
        if (at + 3 > n_ || memcmp(hit, "obj", 3) != 0) {
            // „trailer“?
            continue;
        }
        if (at + 3 < n_ && !IsWs(hit[3]) && !IsDelim(hit[3])) continue;
        // rückwärts: Generation und Objektnummer
        long long i = (long long)at - 1;
        if (i < 0 || !IsWs(b[i])) continue;
        while (i >= 0 && IsWs(b[i])) --i;
        long long ge = i;
        while (i >= 0 && b[i] >= '0' && b[i] <= '9') --i;
        if (i == ge || i < 0 || !IsWs(b[i])) continue;
        int gen = atoi(std::string((const char*)b + i + 1, (size_t)(ge - i)).c_str());
        while (i >= 0 && IsWs(b[i])) --i;
        long long ne = i;
        while (i >= 0 && b[i] >= '0' && b[i] <= '9') --i;
        if (i == ne || ne - i > 10) continue;
        if (i >= 0 && !IsWs(b[i]) && !IsDelim(b[i])) continue;
        int num = atoi(std::string((const char*)b + i + 1, (size_t)(ne - i)).c_str());
        Lexer lx{hit + 3, end};
        P o = lx.Parse();
        if (!o) break;
        lx.SkipWs();
        if (o->k == K::Dict && lx.p + 6 <= end && memcmp(lx.p, "stream", 6) == 0) {
            const uint8_t* s = lx.p + 6;
            if (s < end && *s == '\r') ++s;
            if (s < end && *s == '\n') ++s;
            const uint8_t* se = nullptr;
            P len = o->Get("Length");
            if (len && len->k == K::Num && len->num >= 0 && s + (size_t)len->num <= end) {
                const uint8_t* q = s + (size_t)len->num;
                Lexer chk{q, end};
                chk.SkipWs();
                if (chk.p + 9 <= end && memcmp(chk.p, "endstream", 9) == 0) se = q;
            }
            if (!se) {
                static const char kEnd[] = "endstream";
                const uint8_t* q = std::search(s, end, kEnd, kEnd + 9);
                if (q == end) break;
                se = q;
                if (se > s && se[-1] == '\n') --se;
                if (se > s && se[-1] == '\r') --se;
            }
            o->stream = true;
            o->raw.assign((const char*)s, se - s);
            o->rn = num;
            o->rg = gen;
            pos = (se - b) + 9;
        } else {
            pos = lx.p - b;
        }
        objs_[num] = o;
        P type = o->k == K::Dict ? o->Get("Type") : nullptr;
        if (type && type->k == K::Name && type->s == "XRef") trailers_.push_back(o);
    }
    // klassische Trailer
    static const char kTrailer[] = "trailer";
    for (const uint8_t* q = b; (q = std::search(q, end, kTrailer, kTrailer + 7)) != end; q += 7) {
        Lexer lx{q + 7, end};
        P t = lx.Parse();
        if (t && t->k == K::Dict) trailers_.push_back(t);
    }
}

void PdfDoc::ReadObjectStreams() {
    std::vector<P> streams;
    for (auto& [num, o] : objs_)
        if (o->stream && NameOf(o->Get("Type")) == "ObjStm") streams.push_back(o);
    for (auto& s : streams) {
        if (Stopped()) return;
        std::string data = DecodeStream(s);
        int n = (int)NumOf(s->Get("N"));
        int first = (int)NumOf(s->Get("First"));
        if (n <= 0 || first <= 0 || (size_t)first > data.size()) continue;
        const uint8_t* b = (const uint8_t*)data.data();
        Lexer hdr{b, b + first};
        std::vector<std::pair<int, int>> entries;
        for (int i = 0; i < n && i < 1000000; ++i) {
            long long num = 0, off = 0;
            hdr.SkipWs();
            if (!hdr.ReadInt(num)) break;
            hdr.SkipWs();
            if (!hdr.ReadInt(off)) break;
            entries.emplace_back((int)num, (int)off);
        }
        for (auto& [num, off] : entries) {
            if ((size_t)first + off >= data.size() || objs_.count(num)) continue;
            Lexer lx{b + first + off, b + data.size()};
            P o = lx.Parse();
            if (o) objs_[num] = o;
        }
    }
}

void PdfDoc::SetupCrypt() {
    P enc, id;
    for (auto it = trailers_.rbegin(); it != trailers_.rend(); ++it) {
        if (!enc) enc = (*it)->Get("Encrypt");
        if (!id) id = (*it)->Get("ID");
    }
    P e = Dict(enc);
    if (!e) return;
    crypt_.on = true;
    if (NameOf(e->Get("Filter")) != "Standard") {
        // fremder Sicherheitshandler (z. B. Zertifikate): ohne Schlüssel liefern die Ströme keinen Text
        crypt_.v5 = true;
        crypt_.aes = true;
        return;
    }
    int v = (int)NumOf(e->Get("V")), r = (int)NumOf(e->Get("R"));
    int lenBits = (int)NumOf(e->Get("Length"), 40);
    P o = Resolve(e->Get("O")), u = Resolve(e->Get("U"));
    std::string O = o && o->k == K::Str ? o->s : "", U = u && u->k == K::Str ? u->s : "";
    if (v >= 4) {
        std::string stmf = NameOf(e->Get("StmF"));
        crypt_.identity = stmf.empty() || stmf == "Identity";   // Vorgabe laut Spezifikation: Identity
        P cf = Dict(e->Get("CF"));
        P f = cf ? Dict(cf->Get(stmf.c_str())) : nullptr;
        std::string cfm = f ? NameOf(f->Get("CFM")) : "";
        crypt_.aes = cfm == "AESV2" || cfm == "AESV3";
        if (f && f->Get("Length")) {
            int l = (int)NumOf(f->Get("Length"));
            lenBits = l <= 32 ? l * 8 : l;   // teils Bytes, teils Bits
        }
        if (cfm == "None") crypt_.identity = true;
    }
    if (r >= 5) {
        crypt_.v5 = true;
        crypt_.aes = true;
        P ue = Resolve(e->Get("UE"));
        if (U.size() < 48 || !ue || ue->k != K::Str || ue->s.size() < 32) return;
        std::string salt = U.substr(40, 8);
        std::string h = r == 5 ? Hash(BCRYPT_SHA256_ALGORITHM, salt) : Hash2B("", salt, "");
        if (h.size() != 32) return;
        crypt_.key = AesCbc(h, std::string(16, '\0'), ue->s.substr(0, 32), false);
        return;
    }
    static const uint8_t kPad[32] = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
                                     0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
                                     0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};
    int n = r <= 2 ? 5 : std::clamp(lenBits / 8, 5, 16);
    std::string m((const char*)kPad, 32);
    m += O.substr(0, 32);
    int pv = (int)NumOf(e->Get("P"));
    for (int i = 0; i < 4; ++i) m += (char)((pv >> (8 * i)) & 0xFF);
    P idArr = Resolve(id);
    if (idArr && idArr->k == K::Arr && !idArr->arr.empty()) {
        P id0 = Resolve(idArr->arr[0]);
        if (id0 && id0->k == K::Str) m += id0->s;
    }
    P em = Resolve(e->Get("EncryptMetadata"));
    if (r >= 4 && em && em->k == K::Bool && !em->num) m += std::string(4, '\xFF');
    std::string k = Hash(BCRYPT_MD5_ALGORITHM, m);
    if (k.size() != 16) return;
    if (r >= 3)
        for (int i = 0; i < 50; ++i) k = Hash(BCRYPT_MD5_ALGORITHM, k.substr(0, n));
    crypt_.key = k.substr(0, n);
}

// ---- Filter ----

bool LzwDecode(const std::string& in, std::string& out, bool earlyChange) {
    std::vector<std::string> table;
    auto reset = [&] {
        table.clear();
        for (int i = 0; i < 256; ++i) table.emplace_back(1, (char)i);
        table.emplace_back();   // 256 Clear
        table.emplace_back();   // 257 EOD
    };
    reset();
    int bits = 9;
    uint32_t buf = 0;
    int have = 0;
    std::string prev;
    bool first = true;
    for (size_t i = 0; i < in.size();) {
        while (have < bits && i < in.size()) {
            buf = (buf << 8) | (uint8_t)in[i++];
            have += 8;
        }
        if (have < bits) break;
        uint32_t code = (buf >> (have - bits)) & ((1u << bits) - 1);
        have -= bits;
        if (code == 256) {
            reset();
            bits = 9;
            first = true;
            continue;
        }
        if (code == 257) break;
        std::string entry;
        if (code < table.size() && (code < 256 || !table[code].empty())) {
            entry = table[code];
        } else if (code == table.size() && !first) {
            entry = prev + prev[0];
        } else {
            return false;
        }
        out += entry;
        if (out.size() > kMaxStream) return false;
        if (!first) table.push_back(prev + entry[0]);
        first = false;
        prev = entry;
        size_t next = table.size() + (earlyChange ? 1 : 0);
        if (next >= 512 && bits < 10) bits = 10;
        if (next >= 1024 && bits < 11) bits = 11;
        if (next >= 2048 && bits < 12) bits = 12;
    }
    return true;
}

std::string PdfDoc::DecodeStream(P s) {
    if (!s || !s->stream) return "";
    std::string data = s->raw;
    P type = s->Get("Type");
    bool isXref = type && type->k == K::Name && type->s == "XRef";
    if (crypt_.on && !isXref) data = crypt_.Decrypt(data, s->rn, s->rg);
    P f = Resolve(s->Get("Filter"));
    std::vector<std::string> filters;
    if (f && f->k == K::Name) filters.push_back(f->s);
    if (f && f->k == K::Arr)
        for (auto& x : f->arr) filters.push_back(NameOf(x));
    P parms = Resolve(s->Get("DecodeParms"));
    for (size_t fi = 0; fi < filters.size(); ++fi) {
        const std::string& name = filters[fi];
        P pr = parms && parms->k == K::Arr ? (fi < parms->arr.size() ? Dict(parms->arr[fi]) : nullptr) : Dict(parms);
        std::string out;
        if (name == "FlateDecode" || name == "Fl") {
            const uint8_t* b = (const uint8_t*)data.data();
            size_t n = data.size();
            if (n >= 2 && (b[0] & 0x0F) == 8 && ((b[0] << 8) | b[1]) % 31 == 0) {
                b += 2;
                n -= 2;
            }
            if (!InflateRaw(b, n, out, kMaxStream) && out.empty()) return "";
            if (pr && NumOf(pr->Get("Predictor"), 1) >= 10) {
                // PNG-Prädiktor (selten bei Inhalten, üblich bei Querverweisströmen)
                int cols = (int)NumOf(pr->Get("Columns"), 1), colors = (int)NumOf(pr->Get("Colors"), 1);
                int bpc = (int)NumOf(pr->Get("BitsPerComponent"), 8);
                size_t bpp = std::max(1, colors * bpc / 8), row = (size_t)(cols * colors * bpc + 7) / 8;
                std::string res, prevRow(row, '\0');
                for (size_t pos = 0; pos + 1 + row <= out.size(); pos += 1 + row) {
                    int ftype = (uint8_t)out[pos];
                    std::string cur = out.substr(pos + 1, row);
                    for (size_t i = 0; i < row; ++i) {
                        int a = i >= bpp ? (uint8_t)cur[i - bpp] : 0, up = (uint8_t)prevRow[i];
                        int c = i >= bpp ? (uint8_t)prevRow[i - bpp] : 0;
                        int x = (uint8_t)cur[i];
                        switch (ftype) {
                        case 1: x += a; break;
                        case 2: x += up; break;
                        case 3: x += (a + up) / 2; break;
                        case 4: {
                            int pp = a + up - c, pa = abs(pp - a), pb = abs(pp - up), pc = abs(pp - c);
                            x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c);
                            break;
                        }
                        default: break;
                        }
                        cur[i] = (char)x;
                    }
                    res += cur;
                    prevRow = cur;
                }
                out = res;
            }
        } else if (name == "LZWDecode" || name == "LZW") {
            if (!LzwDecode(data, out, !pr || NumOf(pr->Get("EarlyChange"), 1) != 0) && out.empty()) return "";
        } else if (name == "ASCIIHexDecode" || name == "AHx") {
            int hi = -1;
            for (char c : data) {
                if (c == '>') break;
                int v = HexVal((uint8_t)c);
                if (v < 0) continue;
                if (hi < 0) hi = v;
                else {
                    out += (char)(hi * 16 + v);
                    hi = -1;
                }
            }
            if (hi >= 0) out += (char)(hi * 16);
        } else if (name == "ASCII85Decode" || name == "A85") {
            uint32_t tuple = 0;
            int cnt = 0;
            size_t i = 0;
            if (data.size() >= 2 && data[0] == '<' && data[1] == '~') i = 2;
            for (; i < data.size(); ++i) {
                char c = data[i];
                if (c == '~') break;
                if (IsWs((uint8_t)c)) continue;
                if (c == 'z' && cnt == 0) {
                    out.append(4, '\0');
                    continue;
                }
                if (c < '!' || c > 'u') continue;
                tuple = tuple * 85 + (c - '!');
                if (++cnt == 5) {
                    for (int k = 3; k >= 0; --k) out += (char)((tuple >> (8 * k)) & 0xFF);
                    tuple = 0;
                    cnt = 0;
                }
            }
            if (cnt > 1) {
                for (int k = cnt; k < 5; ++k) tuple = tuple * 85 + 84;
                for (int k = 3; k >= 5 - cnt; --k) out += (char)((tuple >> (8 * k)) & 0xFF);
            }
        } else if (name == "RunLengthDecode" || name == "RL") {
            for (size_t i = 0; i < data.size();) {
                int len = (uint8_t)data[i++];
                if (len == 128) break;
                if (len < 128) {
                    out.append(data, i, std::min<size_t>(len + 1, data.size() - i));
                    i += len + 1;
                } else if (i < data.size()) {
                    out.append(257 - len, data[i++]);
                }
            }
        } else if (name == "Crypt") {
            out = data;   // Identity
        } else {
            return "";   // Bildfilter (DCT, JPX, CCITT, JBIG2): kein Text
        }
        data.swap(out);
    }
    return data;
}

// ---- Schriften ----

std::wstring Utf16BE(const std::string& b) {
    std::wstring w;
    for (size_t i = 0; i + 1 < b.size(); i += 2) w += (wchar_t)(((uint8_t)b[i] << 8) | (uint8_t)b[i + 1]);
    return w;
}

// Textzeichenkette (Dokumentinformationen, Kommentare): UTF-16BE mit BOM, sonst PDFDocEncoding (≈ Latin-1)
std::wstring TextString(const std::string& s) {
    if (s.size() >= 2 && (uint8_t)s[0] == 0xFE && (uint8_t)s[1] == 0xFF) return Utf16BE(s.substr(2));
    std::wstring w;
    for (unsigned char ch : s) w += (wchar_t)ch;
    return w;
}

uint32_t CodeOf(const std::string& b) {
    uint32_t c = 0;
    for (size_t i = 0; i < b.size() && i < 4; ++i) c = (c << 8) | (uint8_t)b[i];
    return c;
}

// Glyphennamen (Adobe Glyph List, Auswahl) -> Unicode
std::wstring GlyphToUnicode(std::string name) {
    size_t dot = name.find('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    if (name.size() == 1 && isalpha((unsigned char)name[0])) return std::wstring(1, (wchar_t)name[0]);
    if (name.find('_') != std::string::npos) {   // Ligaturen „f_f_i“
        std::wstring r;
        size_t start = 0;
        for (;;) {
            size_t u = name.find('_', start);
            r += GlyphToUnicode(name.substr(start, u == std::string::npos ? std::string::npos : u - start));
            if (u == std::string::npos) break;
            start = u + 1;
        }
        return r;
    }
    if (name.size() >= 7 && name.compare(0, 3, "uni") == 0 && (name.size() - 3) % 4 == 0) {
        std::wstring r;
        for (size_t i = 3; i + 4 <= name.size(); i += 4) r += (wchar_t)wcstoul(Utf8ToWide(name.substr(i, 4)).c_str(), nullptr, 16);
        return r;
    }
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u' && isxdigit((unsigned char)name[1])) {
        unsigned long cp = wcstoul(Utf8ToWide(name.substr(1)).c_str(), nullptr, 16);
        if (cp < 0x10000) return std::wstring(1, (wchar_t)cp);
        cp -= 0x10000;
        return std::wstring{(wchar_t)(0xD800 + (cp >> 10)), (wchar_t)(0xDC00 + (cp & 0x3FF))};
    }
    static const std::unordered_map<std::string, const wchar_t*> table = {
        {"space", L" "}, {"exclam", L"!"}, {"quotedbl", L"\""}, {"numbersign", L"#"}, {"dollar", L"$"},
        {"percent", L"%"}, {"ampersand", L"&"}, {"quotesingle", L"'"}, {"quoteright", L"’"},
        {"parenleft", L"("}, {"parenright", L")"}, {"asterisk", L"*"}, {"plus", L"+"}, {"comma", L","},
        {"hyphen", L"-"}, {"period", L"."}, {"slash", L"/"}, {"zero", L"0"}, {"one", L"1"}, {"two", L"2"},
        {"three", L"3"}, {"four", L"4"}, {"five", L"5"}, {"six", L"6"}, {"seven", L"7"}, {"eight", L"8"},
        {"nine", L"9"}, {"colon", L":"}, {"semicolon", L";"}, {"less", L"<"}, {"equal", L"="}, {"greater", L">"},
        {"question", L"?"}, {"at", L"@"}, {"bracketleft", L"["}, {"backslash", L"\\"}, {"bracketright", L"]"},
        {"asciicircum", L"^"}, {"underscore", L"_"}, {"grave", L"`"}, {"quoteleft", L"‘"},
        {"braceleft", L"{"}, {"bar", L"|"}, {"braceright", L"}"}, {"asciitilde", L"~"},
        {"quotedblleft", L"“"}, {"quotedblright", L"”"}, {"quotesinglbase", L"‚"},
        {"quotedblbase", L"„"}, {"endash", L"–"}, {"emdash", L"—"}, {"bullet", L"•"},
        {"ellipsis", L"…"}, {"germandbls", L"ß"}, {"ae", L"æ"}, {"AE", L"Æ"},
        {"oe", L"œ"}, {"OE", L"Œ"}, {"oslash", L"ø"}, {"Oslash", L"Ø"}, {"dotlessi", L"ı"},
        {"fi", L"fi"}, {"fl", L"fl"}, {"ff", L"ff"}, {"ffi", L"ffi"}, {"ffl", L"ffl"}, {"Euro", L"€"},
        {"section", L"§"}, {"paragraph", L"¶"}, {"degree", L"°"}, {"copyright", L"©"},
        {"registered", L"®"}, {"trademark", L"™"}, {"guillemotleft", L"«"},
        {"guillemotright", L"»"}, {"guilsinglleft", L"‹"}, {"guilsinglright", L"›"},
        {"minus", L"−"}, {"multiply", L"×"}, {"divide", L"÷"}, {"periodcentered", L"·"},
        {"nbspace", L" "}, {"nonbreakingspace", L" "}, {"sterling", L"£"}, {"yen", L"¥"},
        {"cent", L"¢"}, {"currency", L"¤"}, {"eth", L"ð"}, {"Eth", L"Ð"},
        {"thorn", L"þ"}, {"Thorn", L"Þ"}, {"exclamdown", L"¡"}, {"questiondown", L"¿"},
        {"mu", L"µ"}, {"dagger", L"†"}, {"daggerdbl", L"‡"}, {"perthousand", L"‰"},
        {"florin", L"ƒ"}, {"ordfeminine", L"ª"}, {"ordmasculine", L"º"}, {"onehalf", L"½"},
        {"onequarter", L"¼"}, {"threequarters", L"¾"}, {"plusminus", L"±"},
        {"logicalnot", L"¬"}, {"brokenbar", L"¦"}, {"macron", L"¯"}, {"dieresis", L"¨"},
        {"acute", L"´"}, {"cedilla", L"¸"}, {"onesuperior", L"¹"}, {"twosuperior", L"²"},
        {"threesuperior", L"³"}, {"lslash", L"ł"}, {"Lslash", L"Ł"}, {"dcroat", L"đ"},
        {"Dcroat", L"Đ"}, {"circumflex", L"ˆ"}, {"tilde", L"˜"}, {"caron", L"ˇ"},
        {"ring", L"˚"}, {"breve", L"˘"}, {"ogonek", L"˛"}, {"dotaccent", L"˙"},
        {"hungarumlaut", L"˝"}, {"fraction", L"⁄"},
    };
    auto it = table.find(name);
    if (it != table.end()) return it->second;
    // Buchstabe + Akzent („adieresis“ -> ä): zusammengesetzt über Unicode-Normalisierung
    static const std::pair<const char*, wchar_t> accents[] = {
        {"dieresis", 0x308}, {"acute", 0x301}, {"grave", 0x300}, {"circumflex", 0x302}, {"tilde", 0x303},
        {"ring", 0x30A}, {"cedilla", 0x327}, {"caron", 0x30C}, {"macron", 0x304}, {"breve", 0x306},
        {"ogonek", 0x328}, {"dotaccent", 0x307}, {"hungarumlaut", 0x30B}, {"commaaccent", 0x326}};
    for (auto& [suffix, comb] : accents) {
        size_t sl = strlen(suffix);
        if (name.size() == sl + 1 && name.compare(1, sl, suffix) == 0 && isalpha((unsigned char)name[0])) {
            wchar_t src[3] = {(wchar_t)name[0], comb, 0};
            wchar_t dst[8] = {};
            int n = NormalizeString(NormalizationC, src, 2, dst, 8);
            if (n > 0) return std::wstring(dst, n);
        }
    }
    return L"";
}

void PdfDoc::ParseCMap(const std::string& data, Font& f) {
    Lexer lx{(const uint8_t*)data.data(), (const uint8_t*)data.data() + data.size()};
    std::vector<P> ops;
    for (;;) {
        P t = lx.Parse();
        if (!t) break;
        if (t->k != K::Kw) {
            ops.push_back(t);
            continue;
        }
        const std::string& kw = t->s;
        if (kw == "begincodespacerange" || kw == "beginbfchar" || kw == "beginbfrange") {
            ops.clear();
            std::string endKw = "end" + kw.substr(5);
            for (;;) {
                P x = lx.Parse();
                if (!x || (x->k == K::Kw && x->s == endKw)) break;
                ops.push_back(x);
            }
            if (kw == "begincodespacerange") {
                for (size_t i = 0; i + 1 < ops.size(); i += 2)
                    if (ops[i]->k == K::Str && ops[i + 1]->k == K::Str && !ops[i]->s.empty())
                        f.ranges.emplace_back(CodeOf(ops[i]->s), CodeOf(ops[i + 1]->s), (int)ops[i]->s.size());
            } else if (kw == "beginbfchar") {
                for (size_t i = 0; i + 1 < ops.size(); i += 2) {
                    if (ops[i]->k != K::Str) continue;
                    std::wstring dst = ops[i + 1]->k == K::Str ? Utf16BE(ops[i + 1]->s)
                                       : ops[i + 1]->k == K::Name ? GlyphToUnicode(ops[i + 1]->s) : L"";
                    f.toUni[CodeOf(ops[i]->s)] = dst;
                }
            } else {
                for (size_t i = 0; i + 2 < ops.size(); i += 3) {
                    if (ops[i]->k != K::Str || ops[i + 1]->k != K::Str) continue;
                    uint32_t lo = CodeOf(ops[i]->s), hi = CodeOf(ops[i + 1]->s);
                    if (hi < lo || hi - lo > 65535) continue;
                    if (ops[i + 2]->k == K::Str) {
                        std::wstring base = Utf16BE(ops[i + 2]->s);
                        if (base.empty()) continue;
                        for (uint32_t c = lo; c <= hi; ++c) {
                            std::wstring w = base;
                            w.back() = (wchar_t)(w.back() + (c - lo));
                            f.toUni[c] = w;
                        }
                    } else if (ops[i + 2]->k == K::Arr) {
                        auto& a = ops[i + 2]->arr;
                        for (uint32_t c = lo; c <= hi && c - lo < a.size(); ++c)
                            if (a[c - lo]->k == K::Str) f.toUni[c] = Utf16BE(a[c - lo]->s);
                    }
                }
            }
            ops.clear();
            continue;
        }
        ops.clear();
    }
}

std::shared_ptr<Font> PdfDoc::GetFont(P fontObj) {
    P fd = Dict(fontObj);
    if (!fd) return nullptr;
    auto it = fonts_.find(fd.get());
    if (it != fonts_.end()) return it->second;
    auto f = std::make_shared<Font>();
    fonts_[fd.get()] = f;
    std::string sub = NameOf(fd->Get("Subtype"));
    if (sub == "Type0") {
        f->twoByte = true;
        f->defWidth = 1000;
        P desc = Resolve(fd->Get("DescendantFonts"));
        P cid = desc && desc->k == K::Arr && !desc->arr.empty() ? Dict(desc->arr[0]) : nullptr;
        if (cid) {
            f->defWidth = NumOf(cid->Get("DW"), 1000);
            P w = Resolve(cid->Get("W"));
            if (w && w->k == K::Arr) {
                auto& a = w->arr;
                for (size_t i = 0; i < a.size();) {
                    P x = Resolve(a[i]);
                    if (!x || x->k != K::Num) break;
                    uint32_t c0 = (uint32_t)x->num;
                    P y = i + 1 < a.size() ? Resolve(a[i + 1]) : nullptr;
                    if (y && y->k == K::Arr) {
                        for (size_t j = 0; j < y->arr.size() && j < 65536; ++j) f->widths[c0 + (uint32_t)j] = NumOf(y->arr[j]);
                        i += 2;
                    } else if (y && y->k == K::Num && i + 2 < a.size()) {
                        uint32_t c1 = (uint32_t)y->num;
                        double wv = NumOf(a[i + 2]);
                        for (uint32_t c = c0; c <= c1 && c - c0 < 65536; ++c) f->widths[c] = wv;
                        i += 3;
                    } else {
                        break;
                    }
                }
            }
        }
        // ohne ToUnicode: UCS-2-Kodierungen direkt verwenden
        std::string enc = NameOf(fd->Get("Encoding"));
        if (enc.find("UCS2") != std::string::npos || enc.find("UTF16") != std::string::npos) {
            f->hasSimple = false;
            f->ranges.emplace_back(0, 0xFFFF, 2);
            for (uint32_t c = 0x20; c < 0xFFFF; ++c)
                if (!(c >= 0xD800 && c < 0xE000)) f->toUni[c] = std::wstring(1, (wchar_t)c);
        }
    } else {
        // einfache Schrift: Grundkodierung + Differences
        P encObj = Resolve(fd->Get("Encoding"));
        std::string baseEnc = encObj && encObj->k == K::Name ? encObj->s : "";
        P encDict = encObj && encObj->k == K::Dict ? encObj : nullptr;
        if (encDict) baseEnc = NameOf(encDict->Get("BaseEncoding"));
        UINT cp = baseEnc == "MacRomanEncoding" ? 10000 : 1252;
        for (int c = 0x20; c < 256; ++c) {
            char b = (char)c;
            wchar_t w = 0;
            if (MultiByteToWideChar(cp, 0, &b, 1, &w, 1) == 1 && w != 0xFFFD) f->simple[c] = std::wstring(1, w);
        }
        if (baseEnc == "StandardEncoding" || baseEnc.empty()) {
            f->simple[0x27] = L"’";
            f->simple[0x60] = L"‘";
        }
        if (encDict) {
            P diff = Resolve(encDict->Get("Differences"));
            if (diff && diff->k == K::Arr) {
                int code = 0;
                for (auto& x : diff->arr) {
                    P y = Resolve(x);
                    if (!y) continue;
                    if (y->k == K::Num) code = (int)y->num;
                    else if (y->k == K::Name && code >= 0 && code < 256) f->simple[code++] = GlyphToUnicode(y->s);
                }
            }
        }
        f->hasSimple = true;
        int firstChar = (int)NumOf(fd->Get("FirstChar"));
        P ws = Resolve(fd->Get("Widths"));
        if (ws && ws->k == K::Arr)
            for (size_t i = 0; i < ws->arr.size() && i < 256; ++i) f->widths[(uint32_t)(firstChar + i)] = NumOf(ws->arr[i]);
        P desc = Dict(fd->Get("FontDescriptor"));
        if (desc) f->defWidth = NumOf(desc->Get("MissingWidth"), 500);
        if (f->defWidth <= 0) f->defWidth = 500;
        if (sub == "Type3") {
            // Glyphenmaße in der Schriftmatrix: grob auf 1/1000 umrechnen
            P fm = Resolve(fd->Get("FontMatrix"));
            if (fm && fm->k == K::Arr && !fm->arr.empty()) {
                double sx = NumOf(fm->arr[0]) * 1000;
                for (auto& [c, w] : f->widths) w *= sx;
            }
        }
    }
    P tu = Resolve(fd->Get("ToUnicode"));
    if (tu && tu->stream) ParseCMap(DecodeStream(tu), *f);
    return f;
}

// ---- Textausgabe ----

void PdfDoc::Emit(const std::wstring& s) {
    if (out_.size() + s.size() > kMaxText) return;
    for (wchar_t c : s) {
        if (c == 0 || c == 0xFFFD) continue;
        if (c < 0x20 && c != '\n' && c != '\t') c = L' ';
        out_ += c;
    }
}

void PdfDoc::Space() {
    if (!out_.empty() && out_.back() != L' ' && out_.back() != L'\n') out_ += L' ';
}

void PdfDoc::NewLine() {
    if (!out_.empty() && out_.back() == L' ') out_.back() = L'\n';
    else if (!out_.empty() && out_.back() != L'\n') out_ += L'\n';
}

void PdfDoc::RunContent(const std::string& data, P resources, int depth) {
    if (depth > 8) return;
    P res = Dict(resources);
    P fontDict = res ? Dict(res->Get("Font")) : nullptr;
    P xobjDict = res ? Dict(res->Get("XObject")) : nullptr;
    Lexer lx{(const uint8_t*)data.data(), (const uint8_t*)data.data() + data.size()};
    std::vector<P> ops;
    int counter = 0;

    // Neuer Zeilenanfang (Benutzerraum): Leerzeichen bzw. Zeilenwechsel aus dem Abstand schätzen
    // Benutzerraum -> Geräteraum (aktuelle Transformationsmatrix)
    auto devX = [&](double x, double y) { return ctm_.a * x + ctm_.c * y + ctm_.e; };
    auto devY = [&](double x, double y) { return ctm_.b * x + ctm_.d * y + ctm_.f; };
    auto moveTo = [&](double e, double f) {
        double em = std::fabs(fontSize_) > 0.01 ? std::fabs(fontSize_) : 10;
        double sx = std::max(1e-6, std::hypot(ctm_.a, ctm_.b)), sy = std::max(1e-6, std::hypot(ctm_.c, ctm_.d));
        if (haveLast_) {
            double x = devX(e, f), y = devY(e, f);
            if (std::fabs(y - lastF_) > 0.5 * em * std::max(0.01, std::fabs(td_)) * sy) {
                NewLine();
            } else {
                double gap = x - lastE_;
                if (gap > 0.15 * em * std::fabs(ta_) * sx || gap < -2 * em * std::fabs(ta_) * sx) Space();
            }
        }
        lineE_ = e;
        lineF_ = f;
        curX_ = 0;
    };
    auto show = [&](const std::string& bytes) {
        Font* f = font_.get();
        std::wstring s;
        double adv = 0;
        for (size_t i = 0; i < bytes.size();) {
            int len = f && f->twoByte ? 2 : 1;
            if (f && !f->ranges.empty()) {
                for (int n = 1; n <= 4; ++n) {
                    if (i + n > bytes.size()) break;
                    uint32_t c = CodeOf(bytes.substr(i, n));
                    bool ok = false;
                    for (auto& [lo, hi, nb] : f->ranges)
                        if (nb == n && c >= lo && c <= hi) ok = true;
                    if (ok) {
                        len = n;
                        break;
                    }
                }
            }
            if (i + len > bytes.size()) len = (int)(bytes.size() - i);
            uint32_t code = CodeOf(bytes.substr(i, len));
            i += len;
            if (!f) {
                s += (wchar_t)code;
                adv += 500;
                continue;
            }
            auto t = f->toUni.find(code);
            if (t != f->toUni.end()) s += t->second;
            else if (!f->twoByte && f->hasSimple && code < 256) s += f->simple[code];
            auto w = f->widths.find(code);
            adv += w != f->widths.end() && w->second > 0 ? w->second : f->defWidth;
        }
        Emit(s);
        curX_ += adv / 1000.0 * fontSize_;
        lastE_ = devX(lineE_ + ta_ * curX_, lineF_);
        lastF_ = devY(lineE_ + ta_ * curX_, lineF_);
        haveLast_ = true;
    };

    for (;;) {
        if ((++counter & 0x3FF) == 0 && Stopped()) return;
        P t = lx.Parse();
        if (!t) break;
        if (t->k != K::Kw) {
            ops.push_back(t);
            if (ops.size() > 64) ops.erase(ops.begin());
            continue;
        }
        const std::string& op = t->s;
        size_t n = ops.size();
        auto num = [&](size_t fromEnd) { return n >= fromEnd && ops[n - fromEnd]->k == K::Num ? ops[n - fromEnd]->num : 0.0; };
        if (op == "q") {
            if (ctmStack_.size() < 256) ctmStack_.push_back(ctm_);
        } else if (op == "Q") {
            if (!ctmStack_.empty()) {
                ctm_ = ctmStack_.back();
                ctmStack_.pop_back();
            }
        } else if (op == "cm" && n >= 6) {
            Ctm m{num(6), num(5), num(4), num(3), num(2), num(1)}, o = ctm_;
            ctm_.a = m.a * o.a + m.b * o.c;
            ctm_.b = m.a * o.b + m.b * o.d;
            ctm_.c = m.c * o.a + m.d * o.c;
            ctm_.d = m.c * o.b + m.d * o.d;
            ctm_.e = m.e * o.a + m.f * o.c + o.e;
            ctm_.f = m.e * o.b + m.f * o.d + o.f;
        } else if (op == "BT") {
            ta_ = td_ = 1;
            lineE_ = lineF_ = 0;
            curX_ = 0;
        } else if (op == "ET") {
            // Position bleibt für die Schätzung des nächsten Textblocks erhalten
        } else if (op == "Tf" && n >= 2) {
            if (ops[n - 2]->k == K::Name && fontDict) font_ = GetFont(fontDict->Get(ops[n - 2]->s.c_str()));
            fontSize_ = num(1);
        } else if (op == "TL") {
            leading_ = num(1);
        } else if (op == "Tm" && n >= 6) {
            ta_ = std::hypot(num(6), num(5));
            td_ = std::hypot(num(4), num(3));
            if (ta_ < 1e-6) ta_ = 1;
            if (td_ < 1e-6) td_ = 1;
            moveTo(num(2), num(1));
        } else if ((op == "Td" || op == "TD") && n >= 2) {
            if (op == "TD") leading_ = -num(1);
            moveTo(lineE_ + ta_ * num(2), lineF_ + td_ * num(1));
        } else if (op == "T*") {
            moveTo(lineE_, lineF_ - td_ * leading_);
            NewLine();
        } else if (op == "Tj" && n >= 1 && ops[n - 1]->k == K::Str) {
            show(ops[n - 1]->s);
        } else if ((op == "'" || op == "\"") && n >= 1 && ops[n - 1]->k == K::Str) {
            moveTo(lineE_, lineF_ - td_ * leading_);
            NewLine();
            show(ops[n - 1]->s);
        } else if (op == "TJ" && n >= 1 && ops[n - 1]->k == K::Arr) {
            for (auto& x : ops[n - 1]->arr) {
                if (x->k == K::Str) {
                    show(x->s);
                } else if (x->k == K::Num) {
                    curX_ -= x->num / 1000.0 * fontSize_;
                    if (x->num < -200) Space();
                }
            }
        } else if (op == "Do" && n >= 1 && ops[n - 1]->k == K::Name && xobjDict) {
            P xo = Resolve(xobjDict->Get(ops[n - 1]->s.c_str()));
            if (xo && xo->stream && NameOf(xo->Get("Subtype")) == "Form" && !formsActive_.count(xo.get())) {
                formsActive_.insert(xo.get());
                P r = xo->Get("Resources");
                Ctm saved = ctm_;
                size_t stackSize = ctmStack_.size();
                P fm = Resolve(xo->Get("Matrix"));
                if (fm && fm->k == K::Arr && fm->arr.size() == 6) {
                    Ctm m{NumOf(fm->arr[0]), NumOf(fm->arr[1]), NumOf(fm->arr[2]), NumOf(fm->arr[3]),
                          NumOf(fm->arr[4]), NumOf(fm->arr[5])};
                    Ctm o = ctm_;
                    ctm_.a = m.a * o.a + m.b * o.c;
                    ctm_.b = m.a * o.b + m.b * o.d;
                    ctm_.c = m.c * o.a + m.d * o.c;
                    ctm_.d = m.c * o.b + m.d * o.d;
                    ctm_.e = m.e * o.a + m.f * o.c + o.e;
                    ctm_.f = m.e * o.b + m.f * o.d + o.f;
                }
                RunContent(DecodeStream(xo), r ? r : resources, depth + 1);
                ctm_ = saved;
                ctmStack_.resize(std::min(stackSize, ctmStack_.size()));
                formsActive_.erase(xo.get());
            }
        } else if (op == "ID") {
            // Inline-Bild überspringen: Binärdaten bis „EI“
            const uint8_t* q = lx.p + 1;
            while (q + 2 <= lx.e) {
                if (q[0] == 'E' && q[1] == 'I' && IsWs(q[-1]) && (q + 2 == lx.e || IsWs(q[2]))) break;
                ++q;
            }
            lx.p = std::min(lx.e, q + 2);
        }
        ops.clear();
    }
}

bool PdfDoc::Run(std::wstring& text) {
    if (n_ < 8) return false;
    // %PDF- in den ersten 1024 Bytes
    const char* sig = "%PDF-";
    if (std::search(d_, d_ + std::min<size_t>(n_, 1024), sig, sig + 5) == d_ + std::min<size_t>(n_, 1024)) return false;
    ScanObjects();
    if (Stopped()) return false;
    SetupCrypt();
    ReadObjectStreams();
    int pages = 0;
    for (auto& [num, o] : objs_) {
        if (Stopped()) return false;
        if (o->k != K::Dict || NameOf(o->Get("Type")) != "Page") continue;
        ++pages;
        // Ressourcen (ggf. geerbt)
        P res = o->Get("Resources");
        P node = o;
        for (int i = 0; !res && node && i < 32; ++i) {
            node = Dict(node->Get("Parent"));
            if (node) res = node->Get("Resources");
        }
        std::string content;
        P c = Resolve(o->Get("Contents"));
        if (c && c->stream) {
            content = DecodeStream(c);
        } else if (c && c->k == K::Arr) {
            for (auto& x : c->arr) {
                P s = Resolve(x);
                if (s && s->stream) content += DecodeStream(s) + "\n";
            }
        }
        font_.reset();
        fontSize_ = 10;
        haveLast_ = false;
        ctm_ = Ctm{};
        ctmStack_.clear();
        RunContent(content, res, 0);
        NewLine();
        // Kommentare und Formularfelder der Seite
        P annots = Resolve(o->Get("Annots"));
        if (annots && annots->k == K::Arr && !crypt_.on) {
            for (auto& a : annots->arr) {
                P ad = Dict(a);
                if (!ad) continue;
                for (const char* key : {"Contents", "V"}) {
                    P s = Resolve(ad->Get(key));
                    if (s && s->k == K::Str && !s->s.empty()) {
                        Emit(TextString(s->s));
                        NewLine();
                    }
                }
            }
        }
    }
    if (pages == 0) return false;
    // Dokumentinformationen (Titel, Autor …), nur unverschlüsselt
    if (!crypt_.on) {
        P info;
        for (auto it = trailers_.rbegin(); it != trailers_.rend() && !info; ++it) info = Dict((*it)->Get("Info"));
        if (info) {
            for (const char* key : {"Title", "Subject", "Author", "Keywords"}) {
                P s = Resolve(info->Get(key));
                if (!s || s->k != K::Str) continue;
                Emit(TextString(s->s));
                NewLine();
            }
        }
    }
    text = std::move(out_);
    return true;
}

} // namespace

bool ExtractPdfText(const uint8_t* data, size_t size, std::wstring& text, const std::atomic<bool>* stop) {
    try {
        PdfDoc doc(data, size, stop);
        return doc.Run(text);
    } catch (...) {
        return false;
    }
}

} // namespace qf
