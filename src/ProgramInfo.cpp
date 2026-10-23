// ProgramInfo.cpp – Eigenschaften von Programmdateien (.exe, .dll) für die Dateianzeige (F11).
// Die PE-Strukturen werden selbst aus der Datei gelesen (unabhängig von der Bitbreite und ohne die Datei zu laden).

#include "ProgramInfo.h"

#include <shlobj.h>
#include <shellapi.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <map>

#include "Util.h"

namespace qf {

namespace {

using Props = std::vector<ProgramProperty>;

void Heading(Props& p, const std::wstring& text) { p.push_back({text, L"", true}); }
void Add(Props& p, const std::wstring& label, const std::wstring& value) {
    if (!value.empty()) p.push_back({label, value, false});
}

std::wstring Hex(uint64_t v, int digits = 0) {
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%0*llX", digits, (unsigned long long)v);
    return buf;
}

std::wstring Dec(uint64_t v) { return IntToStrGrouped(v); }

std::wstring SizeText(uint64_t v) {
    return v < 1024 ? FormatSizeBytes(v) + L" Bytes" : FormatSize(v) + L" (" + FormatSizeBytes(v) + L" Bytes)";
}

std::wstring Join(const std::vector<std::wstring>& parts, const wchar_t* sep = L", ") {
    std::wstring r;
    for (auto& s : parts) r += (r.empty() ? L"" : sep) + s;
    return r;
}

std::wstring TimeFromUnix(uint32_t t) {
    // 100-ns-Intervalle seit 1601
    ULARGE_INTEGER u;
    u.QuadPart = ((uint64_t)t + 11644473600ULL) * 10000000ULL;
    FILETIME ft{u.LowPart, u.HighPart};
    return FormatFileTime(ft, true);
}

// ---------------------------------------------------------------------------
// PE-Datei lesen

class PeReader {
public:
    explicit PeReader(const std::wstring& path) {
        h_ = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER sz{};
        if (h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &sz)) size_ = (uint64_t)sz.QuadPart;
    }
    ~PeReader() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    PeReader(const PeReader&) = delete;
    PeReader& operator=(const PeReader&) = delete;

    bool Ok() const { return h_ != INVALID_HANDLE_VALUE; }
    uint64_t Size() const { return size_; }

    bool Read(uint64_t off, void* buf, size_t n) const {
        if (!Ok() || off > size_ || n > size_ - off) return false;
        OVERLAPPED ov{};
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(h_, buf, (DWORD)n, &got, &ov) && got == n;
    }
    template <typename T> bool ReadT(uint64_t off, T& v) const { return Read(off, &v, sizeof(T)); }

    // Nullterminierte ASCII-Zeichenkette (höchstens max Zeichen)
    std::string ReadStr(uint64_t off, size_t max = 512) const {
        std::string r;
        char buf[64];
        while (r.size() < max) {
            size_t n = (size_t)std::min<uint64_t>(sizeof(buf), size_ > off ? size_ - off : 0);
            if (n == 0 || !Read(off, buf, n)) break;
            for (size_t i = 0; i < n; ++i) {
                if (buf[i] == 0 || r.size() >= max) return r;
                r += buf[i];
            }
            off += n;
        }
        return r;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

struct PeImage {
    const PeReader& f;
    IMAGE_FILE_HEADER fh{};
    bool pe64 = false;
    // Gemeinsame Felder beider optionaler Köpfe
    uint8_t linkerMajor = 0, linkerMinor = 0;
    uint32_t entry = 0, sizeOfImage = 0, checksum = 0, sizeOfHeaders = 0;
    uint64_t imageBase = 0, stackReserve = 0, stackCommit = 0, heapReserve = 0;
    uint16_t osMajor = 0, osMinor = 0, imgMajor = 0, imgMinor = 0, subMajor = 0, subMinor = 0;
    uint16_t subsystem = 0, dllChars = 0;
    std::vector<IMAGE_DATA_DIRECTORY> dirs;
    std::vector<IMAGE_SECTION_HEADER> sections;

    explicit PeImage(const PeReader& r) : f(r) {}

    bool Parse(std::wstring& err) {
        IMAGE_DOS_HEADER dos{};
        if (!f.ReadT(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
            err = L"keine Programmdatei (MZ-Kennung fehlt)";
            return false;
        }
        uint64_t nt = (uint32_t)dos.e_lfanew;
        DWORD sig = 0;
        if (!f.ReadT(nt, sig) || sig != IMAGE_NT_SIGNATURE) {
            err = L"kein Windows-Programm im PE-Format (MS-DOS, NE oder LE)";
            return false;
        }
        if (!f.ReadT(nt + 4, fh)) {
            err = L"Dateikopf unvollständig";
            return false;
        }
        uint64_t opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
        WORD magic = 0;
        if (fh.SizeOfOptionalHeader < 2 || !f.ReadT(opt, magic)) {
            err = L"optionaler Kopf fehlt (Objektdatei?)";
            return false;
        }
        uint32_t ndirs = 0;
        uint64_t dirOff = 0;
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            IMAGE_OPTIONAL_HEADER64 o{};
            f.Read(opt, &o, std::min<size_t>(sizeof(o), fh.SizeOfOptionalHeader));
            pe64 = true;
            linkerMajor = o.MajorLinkerVersion, linkerMinor = o.MinorLinkerVersion;
            entry = o.AddressOfEntryPoint, sizeOfImage = o.SizeOfImage, checksum = o.CheckSum;
            sizeOfHeaders = o.SizeOfHeaders;
            imageBase = o.ImageBase, stackReserve = o.SizeOfStackReserve, stackCommit = o.SizeOfStackCommit;
            heapReserve = o.SizeOfHeapReserve;
            osMajor = o.MajorOperatingSystemVersion, osMinor = o.MinorOperatingSystemVersion;
            imgMajor = o.MajorImageVersion, imgMinor = o.MinorImageVersion;
            subMajor = o.MajorSubsystemVersion, subMinor = o.MinorSubsystemVersion;
            subsystem = o.Subsystem, dllChars = o.DllCharacteristics;
            ndirs = o.NumberOfRvaAndSizes;
            dirOff = opt + offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
        } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            IMAGE_OPTIONAL_HEADER32 o{};
            f.Read(opt, &o, std::min<size_t>(sizeof(o), fh.SizeOfOptionalHeader));
            linkerMajor = o.MajorLinkerVersion, linkerMinor = o.MinorLinkerVersion;
            entry = o.AddressOfEntryPoint, sizeOfImage = o.SizeOfImage, checksum = o.CheckSum;
            sizeOfHeaders = o.SizeOfHeaders;
            imageBase = o.ImageBase, stackReserve = o.SizeOfStackReserve, stackCommit = o.SizeOfStackCommit;
            heapReserve = o.SizeOfHeapReserve;
            osMajor = o.MajorOperatingSystemVersion, osMinor = o.MinorOperatingSystemVersion;
            imgMajor = o.MajorImageVersion, imgMinor = o.MinorImageVersion;
            subMajor = o.MajorSubsystemVersion, subMinor = o.MinorSubsystemVersion;
            subsystem = o.Subsystem, dllChars = o.DllCharacteristics;
            ndirs = o.NumberOfRvaAndSizes;
            dirOff = opt + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
        } else {
            err = L"unbekanntes Format des optionalen Kopfs (" + Hex(magic, 4) + L")";
            return false;
        }
        ndirs = std::min<uint32_t>(ndirs, 16);
        uint64_t dirEnd = opt + fh.SizeOfOptionalHeader;
        for (uint32_t i = 0; i < ndirs && dirOff + (i + 1) * sizeof(IMAGE_DATA_DIRECTORY) <= dirEnd; ++i) {
            IMAGE_DATA_DIRECTORY d{};
            f.ReadT(dirOff + i * sizeof(d), d);
            dirs.push_back(d);
        }
        uint64_t secOff = opt + fh.SizeOfOptionalHeader;
        for (int i = 0; i < std::min<int>(fh.NumberOfSections, 96); ++i) {
            IMAGE_SECTION_HEADER sh{};
            if (!f.ReadT(secOff + i * sizeof(sh), sh)) break;
            sections.push_back(sh);
        }
        return true;
    }

    IMAGE_DATA_DIRECTORY Dir(int i) const { return i < (int)dirs.size() ? dirs[i] : IMAGE_DATA_DIRECTORY{}; }

    // RVA -> Dateiposition (0 = nicht in der Datei)
    uint64_t Off(uint32_t rva) const {
        if (rva == 0) return 0;
        if (rva < sizeOfHeaders) return rva;
        for (auto& s : sections) {
            uint32_t vs = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + vs) {
                uint32_t delta = rva - s.VirtualAddress;
                if (delta >= s.SizeOfRawData) return 0;
                return (uint64_t)s.PointerToRawData + delta;
            }
        }
        return 0;
    }

    std::string StrAt(uint32_t rva, size_t max = 512) const {
        uint64_t o = Off(rva);
        return o ? f.ReadStr(o, max) : std::string();
    }
};

std::wstring MachineName(WORD m) {
    switch (m) {
    case IMAGE_FILE_MACHINE_I386: return L"x86 (32 Bit)";
    case IMAGE_FILE_MACHINE_AMD64: return L"x64 (AMD64)";
    case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
    case 0xA641: return L"ARM64EC";
    case 0xA64E: return L"ARM64X";
    case IMAGE_FILE_MACHINE_ARM: return L"ARM";
    case IMAGE_FILE_MACHINE_ARMNT: return L"ARM (Thumb-2)";
    case IMAGE_FILE_MACHINE_IA64: return L"Itanium (IA-64)";
    case IMAGE_FILE_MACHINE_UNKNOWN: return L"neutral";
    default: return L"unbekannt (" + Hex(m, 4) + L")";
    }
}

std::wstring SubsystemName(WORD s) {
    switch (s) {
    case IMAGE_SUBSYSTEM_NATIVE: return L"Native (Treiber/Systemprozess)";
    case IMAGE_SUBSYSTEM_WINDOWS_GUI: return L"Windows (grafische Oberfläche)";
    case IMAGE_SUBSYSTEM_WINDOWS_CUI: return L"Konsole";
    case IMAGE_SUBSYSTEM_OS2_CUI: return L"OS/2-Konsole";
    case IMAGE_SUBSYSTEM_POSIX_CUI: return L"POSIX-Konsole";
    case IMAGE_SUBSYSTEM_WINDOWS_CE_GUI: return L"Windows CE";
    case IMAGE_SUBSYSTEM_EFI_APPLICATION: return L"EFI-Anwendung";
    case IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER: return L"EFI-Boot-Treiber";
    case IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER: return L"EFI-Laufzeittreiber";
    case IMAGE_SUBSYSTEM_EFI_ROM: return L"EFI-ROM";
    case IMAGE_SUBSYSTEM_XBOX: return L"Xbox";
    case IMAGE_SUBSYSTEM_WINDOWS_BOOT_APPLICATION: return L"Windows-Startanwendung";
    default: return L"unbekannt (" + std::to_wstring(s) + L")";
    }
}

std::wstring ResourceTypeName(WORD id) {
    switch (id) {
    case 1: return L"Mauszeiger";
    case 2: return L"Bitmaps";
    case 3: return L"Symbole";
    case 4: return L"Menüs";
    case 5: return L"Dialoge";
    case 6: return L"Zeichenfolgen";
    case 7: return L"Schriftverzeichnis";
    case 8: return L"Schriften";
    case 9: return L"Tastenkürzel";
    case 10: return L"Rohdaten (RCDATA)";
    case 11: return L"Meldungstabelle";
    case 12: return L"Mauszeiger-Gruppen";
    case 14: return L"Symbolgruppen";
    case 16: return L"Versionsinfo";
    case 17: return L"DLGINCLUDE";
    case 19: return L"Plug & Play";
    case 20: return L"VXD";
    case 21: return L"animierte Mauszeiger";
    case 22: return L"animierte Symbole";
    case 23: return L"HTML";
    case 24: return L"Manifest";
    case 240: return L"DLGINIT";
    case 241: return L"Symbolleisten";
    default: return L"Typ " + std::to_wstring(id);
    }
}

// Ressourcentypen (oberste Ebene) mit Anzahl der Einträge
std::wstring ResourceSummary(const PeImage& pe) {
    IMAGE_DATA_DIRECTORY d = pe.Dir(IMAGE_DIRECTORY_ENTRY_RESOURCE);
    uint64_t base = pe.Off(d.VirtualAddress);
    if (!d.Size || !base) return L"";
    IMAGE_RESOURCE_DIRECTORY root{};
    if (!pe.f.ReadT(base, root)) return L"";
    int n = std::min(root.NumberOfNamedEntries + root.NumberOfIdEntries, 200);
    std::vector<std::wstring> parts;
    for (int i = 0; i < n; ++i) {
        IMAGE_RESOURCE_DIRECTORY_ENTRY e{};
        if (!pe.f.ReadT(base + sizeof(root) + i * sizeof(e), e)) break;
        std::wstring name;
        if (e.NameIsString) {
            WORD len = 0;
            uint64_t o = base + e.NameOffset;
            if (pe.f.ReadT(o, len) && len > 0 && len < 256) {
                name.resize(len);
                if (!pe.f.Read(o + 2, name.data(), len * sizeof(wchar_t))) name.clear();
            }
            if (name.empty()) name = L"(benannt)";
        } else {
            name = ResourceTypeName(e.Id);
        }
        // Anzahl der Einträge der zweiten Ebene
        if (e.DataIsDirectory) {
            IMAGE_RESOURCE_DIRECTORY sub{};
            if (pe.f.ReadT(base + e.OffsetToDirectory, sub)) {
                int cnt = sub.NumberOfNamedEntries + sub.NumberOfIdEntries;
                if (cnt > 1) name += L" (" + std::to_wstring(cnt) + L")";
            }
        }
        parts.push_back(name);
    }
    return Join(parts);
}

void AddPeHeader(Props& p, const PeImage& pe) {
    Heading(p, L"Programmformat (PE-Kopf)");
    Add(p, L"Format", pe.pe64 ? L"PE32+ (64 Bit)" : L"PE32 (32 Bit)");
    Add(p, L"Prozessor", MachineName(pe.fh.Machine));
    bool dll = pe.fh.Characteristics & IMAGE_FILE_DLL;
    std::wstring kind = dll ? L"Programmbibliothek (DLL)" : L"ausführbares Programm";
    if (pe.fh.Characteristics & IMAGE_FILE_SYSTEM) kind += L", Systemdatei";
    Add(p, L"Art", kind);
    Add(p, L"Subsystem", SubsystemName(pe.subsystem));
    Add(p, L"Mindestversion Subsystem", std::to_wstring(pe.subMajor) + L"." + std::to_wstring(pe.subMinor));
    Add(p, L"Betriebssystemversion", std::to_wstring(pe.osMajor) + L"." + std::to_wstring(pe.osMinor));
    if (pe.imgMajor || pe.imgMinor)
        Add(p, L"Image-Version", std::to_wstring(pe.imgMajor) + L"." + std::to_wstring(pe.imgMinor));
    Add(p, L"Linker-Version", std::to_wstring(pe.linkerMajor) + L"." + std::to_wstring(pe.linkerMinor));
    // Zeitstempel: bei reproduzierbaren Builds ein Hashwert statt eines Datums
    uint32_t ts = pe.fh.TimeDateStamp;
    time_t now = time(nullptr);
    if (ts == 0)
        Add(p, L"Erstellt (Linker)", L"– (nicht gesetzt)");
    else if (ts >= 631152000u && (time_t)ts <= now + 366 * 86400)   // 1990 … heute + 1 Jahr
        Add(p, L"Erstellt (Linker)", TimeFromUnix(ts));
    else
        Add(p, L"Erstellt (Linker)", L"– (reproduzierbarer Build, Kennung " + Hex(ts, 8) + L")");
    Add(p, L"Basisadresse", Hex(pe.imageBase, pe.pe64 ? 16 : 8));
    Add(p, L"Einsprungadresse (RVA)", pe.entry ? Hex(pe.entry, 8) : L"– (keine)");
    Add(p, L"Größe im Speicher", SizeText(pe.sizeOfImage));
    Add(p, L"Stapel (reserviert/zugesagt)", FormatSize(pe.stackReserve) + L" / " + FormatSize(pe.stackCommit));
    Add(p, L"Prüfsumme", pe.checksum ? Hex(pe.checksum, 8) : L"– (nicht gesetzt)");

    // Sicherheitsmerkmale
    std::vector<std::wstring> sec;
    WORD c = pe.dllChars;
    if (c & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) sec.push_back(L"ASLR");
    if (c & 0x0020) sec.push_back(L"ASLR mit hoher Entropie");
    if (c & IMAGE_DLLCHARACTERISTICS_NX_COMPAT) sec.push_back(L"DEP (NX)");
    if (c & IMAGE_DLLCHARACTERISTICS_GUARD_CF) sec.push_back(L"Control Flow Guard");
    if (c & IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY) sec.push_back(L"Signatur erzwungen");
    if (c & IMAGE_DLLCHARACTERISTICS_NO_SEH) sec.push_back(L"ohne SEH");
    if (c & IMAGE_DLLCHARACTERISTICS_APPCONTAINER) sec.push_back(L"AppContainer");
    Add(p, L"Sicherheitsmerkmale", sec.empty() ? L"– (keine)" : Join(sec));
    std::vector<std::wstring> other;
    if (pe.fh.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) other.push_back(L"große Adressen (> 2 GB)");
    if (c & IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE) other.push_back(L"Terminalserver-fähig");
    if (c & IMAGE_DLLCHARACTERISTICS_NO_ISOLATION) other.push_back(L"keine Isolation");
    if (c & IMAGE_DLLCHARACTERISTICS_NO_BIND) other.push_back(L"nicht binden");
    if (c & IMAGE_DLLCHARACTERISTICS_WDM_DRIVER) other.push_back(L"WDM-Treiber");
    if (pe.fh.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) other.push_back(L"ohne Relokationen");
    if (pe.fh.Characteristics & IMAGE_FILE_UP_SYSTEM_ONLY) other.push_back(L"nur Einprozessorsysteme");
    Add(p, L"Weitere Merkmale", Join(other));

    // .NET
    IMAGE_DATA_DIRECTORY clr = pe.Dir(IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR);
    if (clr.VirtualAddress && clr.Size) {
        std::wstring net = L"ja (verwalteter Code)";
        IMAGE_COR20_HEADER cor{};
        uint64_t o = pe.Off(clr.VirtualAddress);
        if (o && pe.f.ReadT(o, cor)) {
            uint64_t md = pe.Off(cor.MetaData.VirtualAddress);
            DWORD sig = 0, len = 0;
            if (md && pe.f.ReadT(md, sig) && sig == 0x424A5342 && pe.f.ReadT(md + 12, len) && len > 0 && len < 256) {
                std::string v = pe.f.ReadStr(md + 16, len);
                if (!v.empty()) net += L", Laufzeit " + Utf8ToWide(v);
            }
            std::vector<std::wstring> fl;
            if (cor.Flags & COMIMAGE_FLAGS_ILONLY) fl.push_back(L"nur IL");
            if (cor.Flags & COMIMAGE_FLAGS_32BITREQUIRED) fl.push_back(L"32 Bit erforderlich");
            if (cor.Flags & 0x00020000) fl.push_back(L"32 Bit bevorzugt");
            if (cor.Flags & COMIMAGE_FLAGS_STRONGNAMESIGNED) fl.push_back(L"starker Name");
            if (!fl.empty()) net += L" (" + Join(fl) + L")";
        }
        Add(p, L".NET", net);
    } else {
        Add(p, L".NET", L"nein (nativer Code)");
    }

    // Debug-Informationen (PDB)
    IMAGE_DATA_DIRECTORY dbg = pe.Dir(IMAGE_DIRECTORY_ENTRY_DEBUG);
    if (uint64_t o = pe.Off(dbg.VirtualAddress); o && dbg.Size) {
        int n = std::min<int>(dbg.Size / sizeof(IMAGE_DEBUG_DIRECTORY), 32);
        for (int i = 0; i < n; ++i) {
            IMAGE_DEBUG_DIRECTORY dd{};
            if (!pe.f.ReadT(o + i * sizeof(dd), dd)) break;
            if (dd.Type != IMAGE_DEBUG_TYPE_CODEVIEW || dd.PointerToRawData == 0) continue;
            DWORD cv = 0;
            if (pe.f.ReadT(dd.PointerToRawData, cv) && cv == 0x53445352) {   // "RSDS"
                std::string pdb = pe.f.ReadStr(dd.PointerToRawData + 24, 520);
                Add(p, L"Debug-Datei (PDB)", Utf8ToWide(pdb));
            }
            break;
        }
    }

    // Abschnitte
    std::vector<std::wstring> secs;
    for (auto& s : pe.sections) {
        char nm[9] = {};
        memcpy(nm, s.Name, 8);
        secs.push_back(Utf8ToWide(nm) + L" " + FormatSize(s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData));
    }
    Add(p, L"Abschnitte (" + std::to_wstring(pe.sections.size()) + L")", Join(secs));
    Add(p, L"Ressourcen", ResourceSummary(pe));

    // Zusatzdaten hinter dem Abbild (Installer, Archive) und eingebettete Signatur
    uint64_t end = pe.sizeOfHeaders;
    for (auto& s : pe.sections) end = std::max<uint64_t>(end, (uint64_t)s.PointerToRawData + s.SizeOfRawData);
    IMAGE_DATA_DIRECTORY cert = pe.Dir(IMAGE_DIRECTORY_ENTRY_SECURITY);   // Dateiposition, keine RVA
    uint64_t overlay = pe.f.Size() > end ? pe.f.Size() - end : 0;
    if (cert.VirtualAddress && cert.Size && cert.VirtualAddress >= end) overlay -= std::min<uint64_t>(overlay, cert.Size);
    if (overlay > 16) Add(p, L"Angehängte Daten", SizeText(overlay) + L" hinter dem Programmabbild (z. B. Installationsarchiv)");
}

// Importe (normal und verzögert): DLL-Name -> Anzahl Funktionen
void AddImports(Props& p, const PeImage& pe) {
    struct Imp {
        std::wstring dll;
        size_t count = 0;
        bool delay = false;
    };
    std::vector<Imp> imps;
    size_t ptrSize = pe.pe64 ? 8 : 4;
    auto countThunks = [&](uint32_t rva) {
        size_t n = 0;
        uint64_t o = pe.Off(rva);
        if (!o) return n;
        for (; n < 20000; ++n) {
            uint64_t v = 0;
            if (!pe.f.Read(o + n * ptrSize, &v, ptrSize) || v == 0) break;
        }
        return n;
    };
    IMAGE_DATA_DIRECTORY d = pe.Dir(IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (uint64_t o = pe.Off(d.VirtualAddress)) {
        for (int i = 0; i < 2000; ++i) {
            IMAGE_IMPORT_DESCRIPTOR id{};
            if (!pe.f.ReadT(o + i * sizeof(id), id) || (id.Name == 0 && id.FirstThunk == 0)) break;
            Imp imp;
            imp.dll = Utf8ToWide(pe.StrAt(id.Name, 260));
            imp.count = countThunks(id.OriginalFirstThunk ? id.OriginalFirstThunk : id.FirstThunk);
            imps.push_back(imp);
        }
    }
    IMAGE_DATA_DIRECTORY dl = pe.Dir(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (uint64_t o = pe.Off(dl.VirtualAddress)) {
        struct DelayDesc {
            DWORD attrs, dllName, hmod, iat, int_, boundIat, unloadIat, timeStamp;
        };
        for (int i = 0; i < 2000; ++i) {
            DelayDesc dd{};
            if (!pe.f.ReadT(o + i * sizeof(dd), dd) || dd.dllName == 0) break;
            Imp imp;
            imp.delay = true;
            imp.dll = Utf8ToWide(pe.StrAt(dd.dllName, 260));
            imp.count = countThunks(dd.int_);
            imps.push_back(imp);
        }
    }
    if (imps.empty()) return;
    size_t total = 0;
    for (auto& i : imps) total += i.count;
    Heading(p, L"Importierte Programmbibliotheken (" + std::to_wstring(imps.size()) + L" DLLs, " + Dec(total) +
                   L" Funktionen)");
    for (auto& i : imps)
        Add(p, i.dll.empty() ? L"?" : i.dll,
            std::to_wstring(i.count) + (i.count == 1 ? L" Funktion" : L" Funktionen") +
                (i.delay ? L" (verzögert geladen)" : L""));
}

void AddExports(Props& p, const PeImage& pe) {
    IMAGE_DATA_DIRECTORY d = pe.Dir(IMAGE_DIRECTORY_ENTRY_EXPORT);
    uint64_t o = pe.Off(d.VirtualAddress);
    IMAGE_EXPORT_DIRECTORY ed{};
    if (!o || !d.Size || !pe.f.ReadT(o, ed)) return;
    Heading(p, L"Exportierte Funktionen (" + Dec(ed.NumberOfFunctions) + L")");
    Add(p, L"Interner Name", Utf8ToWide(pe.StrAt(ed.Name, 260)));
    Add(p, L"Erste Ordinalzahl", std::to_wstring(ed.Base));
    Add(p, L"davon mit Namen", Dec(ed.NumberOfNames));
    uint64_t names = pe.Off(ed.AddressOfNames);
    uint64_t ords = pe.Off(ed.AddressOfNameOrdinals);
    const DWORD kMax = 2000;
    DWORD n = std::min<DWORD>(ed.NumberOfNames, kMax);
    for (DWORD i = 0; names && i < n; ++i) {
        DWORD rva = 0;
        WORD ord = 0;
        if (!pe.f.ReadT(names + i * 4, rva)) break;
        if (ords) pe.f.ReadT(ords + i * 2, ord);
        Add(p, L"#" + std::to_wstring(ed.Base + ord), Utf8ToWide(pe.StrAt(rva, 512)));
    }
    if (ed.NumberOfNames > kMax) Add(p, L"…", L"und " + Dec(ed.NumberOfNames - kMax) + L" weitere");
}

// ---------------------------------------------------------------------------
// Versionsressource (alle Einträge, alle Sprachen)

std::wstring StdVersionKey(const std::wstring& k) {
    static const std::map<std::wstring, std::wstring> names = {
        {L"CompanyName", L"Firma"},
        {L"FileDescription", L"Dateibeschreibung"},
        {L"FileVersion", L"Dateiversion"},
        {L"InternalName", L"Interner Name"},
        {L"LegalCopyright", L"Copyright"},
        {L"LegalTrademarks", L"Marken"},
        {L"OriginalFilename", L"Ursprünglicher Dateiname"},
        {L"ProductName", L"Produktname"},
        {L"ProductVersion", L"Produktversion"},
        {L"Comments", L"Kommentar"},
        {L"PrivateBuild", L"Private Version"},
        {L"SpecialBuild", L"Sonderversion"},
    };
    auto it = names.find(k);
    return it == names.end() ? k : it->second;
}

struct VerBlock {
    const BYTE* p = nullptr;
    WORD len = 0, valueLen = 0, type = 0;
    std::wstring key;
    const BYTE* value = nullptr;
    const BYTE* children = nullptr;
    const BYTE* end = nullptr;
};

const BYTE* Align4(const BYTE* base, const BYTE* q) { return base + (((q - base) + 3) & ~(size_t)3); }

bool ParseVerBlock(const BYTE* base, const BYTE* p, const BYTE* limit, VerBlock& b) {
    if (p + 6 > limit) return false;
    b.p = p;
    memcpy(&b.len, p, 2);
    memcpy(&b.valueLen, p + 2, 2);
    memcpy(&b.type, p + 4, 2);
    if (b.len < 6 || p + b.len > limit) return false;
    b.end = p + b.len;
    const wchar_t* k = reinterpret_cast<const wchar_t*>(p + 6);
    const wchar_t* kEnd = reinterpret_cast<const wchar_t*>(b.end);
    b.key.clear();
    while (k < kEnd && *k) b.key += *k++;
    if (k >= kEnd) return false;
    b.value = Align4(base, reinterpret_cast<const BYTE*>(k + 1));
    size_t vbytes = b.type == 1 ? (size_t)b.valueLen * 2 : b.valueLen;
    if (b.value > b.end) b.value = b.end;
    b.children = Align4(base, std::min(b.end, b.value + vbytes));
    if (b.children > b.end) b.children = b.end;
    return true;
}

template <typename F> void ForEachChild(const BYTE* base, const VerBlock& parent, F f) {
    const BYTE* q = parent.children;
    for (int guard = 0; q + 6 <= parent.end && guard < 1000; ++guard) {
        VerBlock c;
        if (!ParseVerBlock(base, q, parent.end, c)) break;
        f(c);
        q = Align4(base, c.end);
    }
}

std::wstring LanguageText(WORD lang, WORD cp) {
    wchar_t name[128] = {};
    VerLanguageNameW(lang, name, 128);
    std::wstring r = name[0] ? name : Hex(lang, 4);
    if (cp == 1200)
        r += L", Unicode";
    else if (cp)
        r += L", Codepage " + std::to_wstring(cp);
    return r;
}

std::wstring VersionNumber(DWORD ms, DWORD ls) {
    return std::to_wstring(HIWORD(ms)) + L"." + std::to_wstring(LOWORD(ms)) + L"." + std::to_wstring(HIWORD(ls)) + L"." +
           std::to_wstring(LOWORD(ls));
}

void AddVersionInfo(Props& p, const std::wstring& path) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) {
        Heading(p, L"Versionsinformationen");
        Add(p, L"", L"– (die Datei enthält keine Versionsinformationen)");
        return;
    }
    std::vector<BYTE> buf(size + 16);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return;
    const BYTE* base = buf.data();
    VerBlock root;
    if (!ParseVerBlock(base, base, base + size, root)) return;
    Heading(p, L"Versionsinformationen");
    if (root.valueLen >= sizeof(VS_FIXEDFILEINFO) && root.value + sizeof(VS_FIXEDFILEINFO) <= root.end) {
        VS_FIXEDFILEINFO fi{};
        memcpy(&fi, root.value, sizeof(fi));
        if (fi.dwSignature == 0xFEEF04BD) {
            Add(p, L"Dateiversion (Zahl)", VersionNumber(fi.dwFileVersionMS, fi.dwFileVersionLS));
            Add(p, L"Produktversion (Zahl)", VersionNumber(fi.dwProductVersionMS, fi.dwProductVersionLS));
            std::vector<std::wstring> fl;
            DWORD f = fi.dwFileFlags & fi.dwFileFlagsMask;
            if (f & VS_FF_DEBUG) fl.push_back(L"Debug-Version");
            if (f & VS_FF_PRERELEASE) fl.push_back(L"Vorabversion");
            if (f & VS_FF_PATCHED) fl.push_back(L"gepatcht");
            if (f & VS_FF_PRIVATEBUILD) fl.push_back(L"private Version");
            if (f & VS_FF_SPECIALBUILD) fl.push_back(L"Sonderversion");
            if (f & VS_FF_INFOINFERRED) fl.push_back(L"Angaben abgeleitet");
            Add(p, L"Kennzeichen", fl.empty() ? L"– (Release)" : Join(fl));
            std::wstring type;
            switch (fi.dwFileType) {
            case VFT_APP: type = L"Anwendung"; break;
            case VFT_DLL: type = L"Programmbibliothek (DLL)"; break;
            case VFT_DRV: type = L"Treiber"; break;
            case VFT_FONT: type = L"Schrift"; break;
            case VFT_VXD: type = L"virtueller Gerätetreiber"; break;
            case VFT_STATIC_LIB: type = L"statische Bibliothek"; break;
            default: type = L"unbekannt"; break;
            }
            Add(p, L"Dateityp (Version)", type);
            std::wstring os;
            switch (fi.dwFileOS) {
            case VOS_NT_WINDOWS32: os = L"Windows NT (Win32)"; break;
            case VOS_NT: os = L"Windows NT"; break;
            case VOS__WINDOWS32: os = L"Win32"; break;
            case VOS_DOS_WINDOWS32: os = L"Win32 unter MS-DOS"; break;
            case VOS_DOS_WINDOWS16: os = L"Windows 3.x (16 Bit)"; break;
            case VOS_UNKNOWN: os = L"unbekannt"; break;
            default: os = Hex(fi.dwFileOS, 8); break;
            }
            Add(p, L"Betriebssystem (Version)", os);
            if (fi.dwFileDateMS || fi.dwFileDateLS) {
                FILETIME ft{fi.dwFileDateLS, fi.dwFileDateMS};
                Add(p, L"Datum (Version)", FormatFileTime(ft, true));
            }
        }
    }
    ForEachChild(base, root, [&](const VerBlock& c) {
        if (c.key == L"StringFileInfo") {
            ForEachChild(base, c, [&](const VerBlock& table) {
                WORD lang = 0, cp = 0;
                if (table.key.size() == 8) {
                    lang = (WORD)wcstoul(table.key.substr(0, 4).c_str(), nullptr, 16);
                    cp = (WORD)wcstoul(table.key.substr(4).c_str(), nullptr, 16);
                }
                Add(p, L"Sprache", LanguageText(lang, cp));
                ForEachChild(base, table, [&](const VerBlock& s) {
                    std::wstring v;
                    const wchar_t* q = reinterpret_cast<const wchar_t*>(s.value);
                    const wchar_t* qEnd = reinterpret_cast<const wchar_t*>(s.end);
                    while (q < qEnd && *q) v += *q++;
                    // Steuerzeichen (Zeilenumbrüche in Kommentaren) als Leerzeichen
                    for (auto& ch : v)
                        if (ch < 32) ch = L' ';
                    if (!v.empty()) Add(p, StdVersionKey(s.key), v);
                });
            });
        } else if (c.key == L"VarFileInfo") {
            ForEachChild(base, c, [&](const VerBlock& var) {
                if (var.key != L"Translation") return;
                std::vector<std::wstring> langs;
                for (const BYTE* q = var.value; q + 4 <= var.end && q + 4 <= var.value + var.valueLen; q += 4) {
                    WORD lang = 0, cp = 0;
                    memcpy(&lang, q, 2);
                    memcpy(&cp, q + 2, 2);
                    langs.push_back(LanguageText(lang, cp));
                }
                if (langs.size() > 1) Add(p, L"Übersetzungen", Join(langs, L"; "));
            });
        }
    });
}

// ---------------------------------------------------------------------------
// Manifest (Ausführungsebene, DPI)

std::string XmlAttr(const std::string& xml, const char* element, const char* attr) {
    size_t e = xml.find(element);
    if (e == std::string::npos) return "";
    size_t close = xml.find('>', e);
    std::string tag = xml.substr(e, close == std::string::npos ? std::string::npos : close - e);
    std::string key = std::string(attr) + "=";
    size_t a = tag.find(key);
    if (a == std::string::npos) return "";
    a += key.size();
    if (a >= tag.size()) return "";
    char q = tag[a];
    if (q != '"' && q != '\'') return "";
    size_t b = tag.find(q, a + 1);
    return b == std::string::npos ? "" : tag.substr(a + 1, b - a - 1);
}

std::string XmlElementText(const std::string& xml, const char* element) {
    size_t e = xml.find(element);
    if (e == std::string::npos) return "";
    size_t start = xml.find('>', e);
    if (start == std::string::npos) return "";
    size_t end = xml.find('<', start + 1);
    if (end == std::string::npos) return "";
    std::string t = xml.substr(start + 1, end - start - 1);
    while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
    while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
    return t;
}

void AddManifest(Props& p, const std::wstring& path) {
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return;
    std::string xml;
    for (int id : {1, 2, 3}) {
        HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(24));
        if (!r) continue;
        HGLOBAL g = LoadResource(m, r);
        DWORD sz = SizeofResource(m, r);
        if (g && sz) {
            const char* d = static_cast<const char*>(LockResource(g));
            if (d) xml.assign(d, std::min<DWORD>(sz, 256 * 1024));
        }
        break;
    }
    FreeLibrary(m);
    if (xml.empty()) return;
    Heading(p, L"Manifest");
    std::string level = XmlAttr(xml, "requestedExecutionLevel", "level");
    if (level == "requireAdministrator")
        Add(p, L"Ausführungsebene", L"Administrator erforderlich (requireAdministrator)");
    else if (level == "highestAvailable")
        Add(p, L"Ausführungsebene", L"höchste verfügbare (highestAvailable)");
    else if (level == "asInvoker")
        Add(p, L"Ausführungsebene", L"wie Aufrufer (asInvoker)");
    else if (!level.empty())
        Add(p, L"Ausführungsebene", Utf8ToWide(level));
    std::string ui = XmlAttr(xml, "requestedExecutionLevel", "uiAccess");
    if (ui == "true") Add(p, L"UI-Zugriff", L"ja (uiAccess)");
    std::string dpi = XmlElementText(xml, "dpiAwareness");
    std::string dpiOld = XmlElementText(xml, "dpiAware");
    if (!dpi.empty())
        Add(p, L"DPI-Unterstützung", Utf8ToWide(dpi));
    else if (!dpiOld.empty())
        Add(p, L"DPI-Unterstützung", Utf8ToWide(dpiOld));
    if (xml.find("Microsoft.Windows.Common-Controls") != std::string::npos)
        Add(p, L"Steuerelemente", L"Common Controls 6 (visuelle Stile)");
    std::string lpa = XmlElementText(xml, "longPathAware");
    if (!lpa.empty()) Add(p, L"Lange Pfade", lpa == "true" ? L"ja (longPathAware)" : Utf8ToWide(lpa));
    std::string cp = XmlElementText(xml, "activeCodePage");
    if (!cp.empty()) Add(p, L"Codepage", Utf8ToWide(cp));
    std::vector<std::wstring> os;
    struct OsId {
        const char* guid;
        const wchar_t* name;
    };
    static const OsId ids[] = {{"e2011457-1546-43c5-a5fe-008deee3d3f0", L"Vista"},
                               {"35138b9a-5d96-4fbd-8e2d-a2440225f93a", L"7"},
                               {"4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38", L"8"},
                               {"1f676c76-80e1-4239-95bb-83d0f6d0da78", L"8.1"},
                               {"8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a", L"10/11"}};
    std::string lower = xml;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return (char)tolower(ch); });
    for (auto& i : ids)
        if (lower.find(i.guid) != std::string::npos) os.push_back(i.name);
    if (!os.empty()) Add(p, L"Unterstützte Windows-Versionen", Join(os));
}

// ---------------------------------------------------------------------------
// Digitale Signatur (eingebettet oder über einen Windows-Katalog)

std::wstring CertName(PCCERT_CONTEXT cert, DWORD flags) {
    wchar_t buf[512] = {};
    CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, buf, 512);
    return buf;
}

std::wstring TrustText(LONG r) {
    switch ((HRESULT)r) {
    case S_OK: return L"gültig";
    case TRUST_E_NOSIGNATURE: return L"nicht signiert";
    case TRUST_E_BAD_DIGEST: return L"ungültig – die Datei wurde nach dem Signieren verändert";
    case TRUST_E_SUBJECT_NOT_TRUSTED: return L"nicht vertrauenswürdig";
    case TRUST_E_EXPLICIT_DISTRUST: return L"ausdrücklich als nicht vertrauenswürdig markiert";
    case CERT_E_EXPIRED: return L"Zertifikat abgelaufen";
    case CERT_E_REVOKED: return L"Zertifikat widerrufen";
    case CERT_E_UNTRUSTEDROOT: return L"Stammzertifikat nicht vertrauenswürdig";
    case CERT_E_CHAINING: return L"Zertifikatkette unvollständig";
    case TRUST_E_SUBJECT_FORM_UNKNOWN: return L"Format nicht unterstützt";
    case CRYPT_E_SECURITY_SETTINGS: return L"durch Sicherheitseinstellungen nicht geprüft";
    default: return L"ungültig (" + Hex((uint32_t)r, 8) + L")";
    }
}

// Prüft mit WinVerifyTrust und liest den Signierer; Rückgabe: Ergebnis von WinVerifyTrust
LONG VerifyAndDescribe(WINTRUST_DATA& wd, Props& p, const std::wstring& how) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if ((HRESULT)r != TRUST_E_NOSIGNATURE) {
        Add(p, L"Signatur", TrustText(r) + how);
        if (CRYPT_PROVIDER_DATA* prov = WTHelperProvDataFromStateData(wd.hWVTStateData)) {
            if (CRYPT_PROVIDER_SGNR* sg = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0)) {
                if (CRYPT_PROVIDER_CERT* pc = WTHelperGetProvCertFromChain(sg, 0); pc && pc->pCert) {
                    Add(p, L"Signiert von", CertName(pc->pCert, 0));
                    Add(p, L"Ausgestellt von", CertName(pc->pCert, CERT_NAME_ISSUER_FLAG));
                    Add(p, L"Zertifikat gültig", FormatFileTime(pc->pCert->pCertInfo->NotBefore) + L" bis " +
                                                      FormatFileTime(pc->pCert->pCertInfo->NotAfter));
                }
                if (sg->csCounterSigners > 0) Add(p, L"Zeitstempel", FormatFileTime(sg->sftVerifyAsOf, true));
                if (sg->psSigner) {
                    const char* alg = sg->psSigner->HashAlgorithm.pszObjId;
                    if (alg) {
                        PCCRYPT_OID_INFO oi = CryptFindOIDInfo(CRYPT_OID_INFO_OID_KEY, (void*)alg, 0);
                        if (oi && oi->pwszName) Add(p, L"Hashverfahren", oi->pwszName);
                    }
                }
            }
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return r;
}

void AddSignature(Props& p, const std::wstring& path, bool embedded) {
    Heading(p, L"Digitale Signatur");
    if (embedded) {
        WINTRUST_FILE_INFO fi{};
        fi.cbStruct = sizeof(fi);
        fi.pcwszFilePath = path.c_str();
        WINTRUST_DATA wd{};
        wd.dwUnionChoice = WTD_CHOICE_FILE;
        wd.pFile = &fi;
        if ((HRESULT)VerifyAndDescribe(wd, p, L" (in der Datei)") != TRUST_E_NOSIGNATURE) return;
    }
    // Katalogsignatur (z. B. Windows-Systemdateien)
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    bool done = false;
    if (h != INVALID_HANDLE_VALUE) {
        using Acquire2 = BOOL(WINAPI*)(HCATADMIN*, const GUID*, PCWSTR, PCCERT_STRONG_SIGN_PARA, DWORD);
        using Hash2 = BOOL(WINAPI*)(HCATADMIN, HANDLE, DWORD*, BYTE*, DWORD);
        HMODULE wt = GetModuleHandleW(L"wintrust.dll");
        auto acquire2 = wt ? reinterpret_cast<Acquire2>(GetProcAddress(wt, "CryptCATAdminAcquireContext2")) : nullptr;
        auto hash2 = wt ? reinterpret_cast<Hash2>(GetProcAddress(wt, "CryptCATAdminCalcHashFromFileHandle2")) : nullptr;
        GUID driverAction = DRIVER_ACTION_VERIFY;
        for (int pass = 0; pass < 2 && !done; ++pass) {   // SHA-256-Kataloge, dann SHA-1
            HCATADMIN admin = nullptr;
            BYTE hash[64];
            DWORD cb = sizeof(hash);
            bool ok;
            if (pass == 0) {
                if (!acquire2 || !hash2) continue;
                ok = acquire2(&admin, &driverAction, L"SHA256", nullptr, 0) && hash2(admin, h, &cb, hash, 0);
            } else {
                ok = CryptCATAdminAcquireContext(&admin, &driverAction, 0) &&
                     CryptCATAdminCalcHashFromFileHandle(h, &cb, hash, 0);
            }
            if (ok) {
                HCATINFO cat = CryptCATAdminEnumCatalogFromHash(admin, hash, cb, 0, nullptr);
                if (cat) {
                    CATALOG_INFO ci{};
                    ci.cbStruct = sizeof(ci);
                    if (CryptCATCatalogInfoFromContext(cat, &ci, 0)) {
                        std::wstring tag;
                        for (DWORD i = 0; i < cb; ++i) {
                            wchar_t b[3];
                            swprintf(b, 3, L"%02X", hash[i]);
                            tag += b;
                        }
                        WINTRUST_CATALOG_INFO wci{};
                        wci.cbStruct = sizeof(wci);
                        wci.pcwszCatalogFilePath = ci.wszCatalogFile;
                        wci.pcwszMemberTag = tag.c_str();
                        wci.pcwszMemberFilePath = path.c_str();
                        wci.hMemberFile = h;
                        wci.pbCalculatedFileHash = hash;
                        wci.cbCalculatedFileHash = cb;
                        wci.hCatAdmin = pass == 0 ? admin : nullptr;
                        WINTRUST_DATA wd{};
                        wd.dwUnionChoice = WTD_CHOICE_CATALOG;
                        wd.pCatalog = &wci;
                        VerifyAndDescribe(wd, p, L" (Windows-Katalog)");
                        Add(p, L"Katalogdatei", ci.wszCatalogFile);
                        done = true;
                    }
                    CryptCATAdminReleaseCatalogContext(admin, cat, 0);
                }
            }
            if (admin) CryptCATAdminReleaseContext(admin, 0);
        }
        CloseHandle(h);
    }
    if (!done) Add(p, L"Signatur", L"nicht signiert");
}

// ---------------------------------------------------------------------------
// Windows-Eigenschaften (Eigenschaftenspeicher der Shell, wie im Explorer)

void AddShellProperties(Props& p, const std::wstring& path) {
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_BESTEFFORT, IID_PPV_ARGS(&store))) || !store)
        return;
    std::vector<ProgramProperty> rows;
    DWORD n = 0;
    store->GetCount(&n);
    for (DWORD i = 0; i < n && i < 1000; ++i) {
        PROPERTYKEY key{};
        if (FAILED(store->GetAt(i, &key))) continue;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (FAILED(store->GetValue(key, &pv)) || pv.vt == VT_EMPTY || pv.vt == VT_NULL) {
            PropVariantClear(&pv);
            continue;
        }
        std::wstring label, value;
        IPropertyDescription* desc = nullptr;
        if (SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARGS(&desc))) && desc) {
            PWSTR dn = nullptr;
            if (SUCCEEDED(desc->GetDisplayName(&dn)) && dn) {
                label = dn;
                CoTaskMemFree(dn);
            }
            if (label.empty()) {
                PWSTR cn = nullptr;
                if (SUCCEEDED(desc->GetCanonicalName(&cn)) && cn) {
                    label = cn;
                    CoTaskMemFree(cn);
                }
            }
            PWSTR fv = nullptr;
            if (SUCCEEDED(desc->FormatForDisplay(pv, PDFF_DEFAULT, &fv)) && fv) {
                value = fv;
                CoTaskMemFree(fv);
            }
            desc->Release();
        }
        if (value.empty()) {
            PWSTR sv = nullptr;
            if (SUCCEEDED(PropVariantToStringAlloc(pv, &sv)) && sv) {
                value = sv;
                CoTaskMemFree(sv);
            }
        }
        PropVariantClear(&pv);
        if (label.empty()) {
            wchar_t g[64] = {};
            StringFromGUID2(key.fmtid, g, 64);
            label = std::wstring(g) + L" " + std::to_wstring(key.pid);
        }
        // Steuerzeichen und Richtungszeichen (Datumsangaben) entfernen
        std::wstring clean;
        for (wchar_t ch : value)
            if (ch >= 32 && ch != 0x200E && ch != 0x200F && ch != 0x202A && ch != 0x202C) clean += ch;
        if (!clean.empty()) rows.push_back({label, clean, false});
    }
    store->Release();
    if (rows.empty()) return;
    std::sort(rows.begin(), rows.end(),
              [](const ProgramProperty& a, const ProgramProperty& b) { return CompareNatural(a.label, b.label) < 0; });
    Heading(p, L"Windows-Eigenschaften (" + std::to_wstring(rows.size()) + L")");
    p.insert(p.end(), rows.begin(), rows.end());
}

} // namespace

bool IsProgramFile(const std::wstring& path) {
    std::wstring ext = ToLower(PathExtension(path));
    return ext == L".exe" || ext == L".dll";
}

HICON LoadProgramIcon(const std::wstring& path, int size) {
    HICON icon = nullptr;
    UINT id = 0;
    UINT n = PrivateExtractIconsW(path.c_str(), 0, size, size, &icon, &id, 1, LR_DEFAULTCOLOR);
    if (n == 0 || n == 0xFFFFFFFF || !icon) return nullptr;
    return icon;
}

std::vector<ProgramProperty> ReadProgramProperties(const std::wstring& path, const std::atomic<bool>* cancel) {
    auto cancelled = [&] { return cancel && cancel->load(); };
    Props p;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    Heading(p, L"Datei");
    Add(p, L"Name", PathFileName(path));
    Add(p, L"Verzeichnis", PathParent(path));
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &fad)) {
        uint64_t size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        Add(p, L"Größe", SizeText(size));
        Add(p, L"Erstellt", FormatFileTime(fad.ftCreationTime, true));
        Add(p, L"Geändert", FormatFileTime(fad.ftLastWriteTime, true));
        Add(p, L"Letzter Zugriff", FormatFileTime(fad.ftLastAccessTime, true));
        Add(p, L"Attribute", FormatAttributes(fad.dwFileAttributes));
    }
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME)) Add(p, L"Typ", sfi.szTypeName);

    if (!cancelled()) AddVersionInfo(p, path);

    bool embeddedSig = false;
    if (!cancelled()) {
        PeReader f(path);
        PeImage pe(f);
        std::wstring err;
        if (!f.Ok()) {
            Heading(p, L"Programmformat (PE-Kopf)");
            Add(p, L"", L"Datei kann nicht gelesen werden: " + LastErrorMessage(GetLastError()));
        } else if (!pe.Parse(err)) {
            Heading(p, L"Programmformat (PE-Kopf)");
            Add(p, L"", err);
        } else {
            AddPeHeader(p, pe);
            IMAGE_DATA_DIRECTORY cert = pe.Dir(IMAGE_DIRECTORY_ENTRY_SECURITY);
            embeddedSig = cert.VirtualAddress && cert.Size;
            if (!cancelled()) AddManifest(p, path);
            if (!cancelled()) AddImports(p, pe);
            if (!cancelled()) AddExports(p, pe);
        }
    }
    if (!cancelled()) {
        // Signatur vor den langen Listen einordnen (gleich nach dem PE-Kopf/Manifest)
        Props sig;
        AddSignature(sig, path, embeddedSig);
        auto pos = std::find_if(p.begin(), p.end(), [](const ProgramProperty& x) {
            return x.heading && (x.label.rfind(L"Importierte", 0) == 0 || x.label.rfind(L"Exportierte", 0) == 0);
        });
        p.insert(pos, sig.begin(), sig.end());
    }
    if (!cancelled()) AddShellProperties(p, path);
    if (SUCCEEDED(co)) CoUninitialize();
    return p;
}

std::wstring ProgramPropertiesText(const std::wstring& path, const std::vector<ProgramProperty>& props) {
    std::wstring t = path + L"\r\n";
    for (auto& x : props) {
        if (x.heading)
            t += L"\r\n" + x.label + L"\r\n";
        else
            t += L"  " + (x.label.empty() ? std::wstring() : x.label + L": ") + x.value + L"\r\n";
    }
    return t;
}

} // namespace qf
