// Drives.cpp – Laufwerksarten und -symbole (siehe Drives.h)
//
// Erkennung:
// - subst: QueryDosDevice liefert „\??\C:\Pfad“ bzw. „\??\UNC\Server\Freigabe“ (DefineDosDevice/subst)
// - Netzwerk: GetDriveType = DRIVE_REMOTE (Ziel über WNetGetConnection)
// - CD/DVD: DRIVE_CDROM
// - extern: DRIVE_REMOVABLE oder Datenträger am USB-, FireWire- oder SD-Bus (IOCTL_STORAGE_QUERY_PROPERTY; geht ohne
//   Administratorrechte, da das Volume ohne Zugriffsrechte geöffnet wird)
// - RAM-Disk: DRIVE_RAMDISK oder Gerätename, Hersteller-/Produktkennung bzw. Bezeichnung mit „RAM-Disk“, „RamDisk“,
//   „ImDisk“ o. Ä. (die meisten RAM-Disk-Treiber melden sich als Festplatte)
// Symbole: lokale Laufwerke mit dem Symbol der Shell (Windows-Logo beim Systemlaufwerk, BitLocker-Schloss), sonst
// die Standardsymbole von Windows für Netzlaufwerk, CD-Laufwerk, Wechseldatenträger und RAM-Disk.

#include "Drives.h"
#include "Util.h"

#include <shellapi.h>
#include <winioctl.h>
#include <winnetwk.h>

#include <algorithm>
#include <cwctype>

namespace qf {

namespace {

int StockIndex(SHSTOCKICONID id) {
    SHSTOCKICONINFO sii{sizeof(sii)};
    return SUCCEEDED(SHGetStockIconInfo(id, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &sii)) ? sii.iSysImageIndex : 0;
}

bool ContainsAnyI(const std::wstring& text, std::initializer_list<const wchar_t*> words) {
    std::wstring l = ToLower(text);
    for (const wchar_t* w : words)
        if (l.find(w) != std::wstring::npos) return true;
    return false;
}

bool LooksLikeRamDisk(const std::wstring& text) {
    return ContainsAnyI(text, {L"ramdisk", L"ram disk", L"ram-disk", L"ramdrive", L"ram drive", L"imdisk", L"primo ram",
                               L"softperfect ram", L"ultimate ram"});
}

// Bus und Kennung des Datenträgers hinter dem Laufwerk
bool QueryStorage(wchar_t letter, STORAGE_BUS_TYPE& bus, bool& removableMedia, std::wstring& vendorProduct) {
    wchar_t path[] = L"\\\\.\\X:";
    path[4] = letter;
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    alignas(8) BYTE buf[1024] = {};
    DWORD got = 0;
    bool ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, nullptr) &&
              got >= sizeof(STORAGE_DEVICE_DESCRIPTOR);
    CloseHandle(h);
    if (!ok) return false;
    auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    bus = d->BusType;
    removableMedia = d->RemovableMedia != FALSE;
    auto str = [&](DWORD off) -> std::wstring {
        if (off == 0 || off >= got) return L"";
        const char* p = reinterpret_cast<const char*>(buf + off);
        size_t n = strnlen(p, got - off);
        return Trim(std::wstring(p, p + n));
    };
    vendorProduct = str(d->VendorIdOffset) + L" " + str(d->ProductIdOffset);
    return true;
}

DriveDesc Describe(wchar_t letter) {
    DriveDesc d;
    d.root = std::wstring(1, letter) + L":\\";
    d.type = GetDriveTypeW(d.root.c_str());
    // Zielgerät (subst erkennen, RAM-Disk-Treiber am Gerätenamen)
    wchar_t dev[1024] = {};
    std::wstring device;
    wchar_t name[3] = {letter, L':', 0};
    if (QueryDosDeviceW(name, dev, 1024)) device = dev;
    bool subst = false;
    if (device.rfind(L"\\??\\", 0) == 0) {
        std::wstring t = device.substr(4);
        if (t.size() >= 2 && t[1] == L':') {
            subst = true;
            d.target = t;
        } else if (StartsWithI(t, L"UNC\\")) {
            subst = true;
            d.target = L"\\\\" + t.substr(4);
        }
    }
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (d.type == DRIVE_FIXED || d.type == DRIVE_RAMDISK || d.type == DRIVE_REMOVABLE || subst) {
        wchar_t label[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(d.root.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) d.label = label;
    }
    if (subst) {
        d.kind = DriveKind::Subst;
    } else if (d.type == DRIVE_REMOTE) {
        d.kind = DriveKind::Network;
        wchar_t buf[1024] = {};
        DWORD len = 1023;
        if (WNetGetConnectionW(name, buf, &len) == NO_ERROR) d.target = buf;
    } else if (d.type == DRIVE_CDROM) {
        d.kind = DriveKind::CdRom;
    } else if (d.type == DRIVE_RAMDISK) {
        d.kind = DriveKind::RamDisk;
    } else {
        STORAGE_BUS_TYPE bus = BusTypeUnknown;
        bool removableMedia = false;
        std::wstring ident;
        bool storage = QueryStorage(letter, bus, removableMedia, ident);
        if (LooksLikeRamDisk(device) || LooksLikeRamDisk(ident) || LooksLikeRamDisk(d.label))
            d.kind = DriveKind::RamDisk;
        else if (d.type == DRIVE_REMOVABLE ||
                 (storage && (bus == BusTypeUsb || bus == BusType1394 || bus == BusTypeSd || bus == BusTypeMmc)))
            d.kind = DriveKind::External;
        else
            d.kind = DriveKind::Local;
    }
    SetErrorMode(oldMode);
    switch (d.kind) {
    case DriveKind::Local: {
        // Symbol der Shell: Systemlaufwerk mit Windows-Logo, BitLocker mit Schloss
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(d.root.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            d.icon = sfi.iIcon;
        else
            d.icon = StockIndex(SIID_DRIVEFIXED);
        break;
    }
    case DriveKind::Subst:
        d.icon = StockIndex(d.type == DRIVE_REMOTE ? SIID_DRIVENET : SIID_DRIVEFIXED);
        d.linkOverlay = true;
        break;
    case DriveKind::Network: d.icon = StockIndex(SIID_DRIVENET); break;
    case DriveKind::CdRom: d.icon = StockIndex(SIID_DRIVECD); break;
    case DriveKind::External: d.icon = StockIndex(SIID_DRIVEREMOVE); break;
    case DriveKind::RamDisk: d.icon = StockIndex(SIID_DRIVERAM); break;
    }
    return d;
}

} // namespace

const std::vector<DriveDesc>& ListDrives() {
    static std::vector<DriveDesc> cache;
    static DWORD cacheMask = 0;
    static ULONGLONG cacheTime = 0;
    DWORD mask = GetLogicalDrives();
    ULONGLONG now = GetTickCount64();
    if (cacheTime != 0 && mask == cacheMask && now - cacheTime < 2000) return cache;
    cache.clear();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t letter = (wchar_t)(L'A' + i);
        if (i < 2) {
            wchar_t root[] = {letter, L':', L'\\', 0};
            if (GetDriveTypeW(root) == DRIVE_REMOVABLE) continue;   // keine Diskettenlaufwerke
        }
        cache.push_back(Describe(letter));
    }
    cacheMask = mask;
    cacheTime = now;
    return cache;
}

std::wstring DriveKindText(DriveKind kind) {
    switch (kind) {
    case DriveKind::Local: return L"Lokales Laufwerk";
    case DriveKind::Subst: return L"subst-Laufwerk";
    case DriveKind::Network: return L"Netzlaufwerk";
    case DriveKind::CdRom: return L"CD/DVD-Laufwerk";
    case DriveKind::External: return L"Externes Laufwerk";
    case DriveKind::RamDisk: return L"RAM-Disk";
    }
    return L"";
}

std::wstring DriveDescription(const DriveDesc& d) {
    std::wstring s = d.root.substr(0, 2) + L" – " + DriveKindText(d.kind);
    if (!d.target.empty()) s += (d.kind == DriveKind::Subst ? L" → " : L" ") + d.target;
    if (!d.label.empty()) s += L" – " + d.label;
    return s;
}

void DrawDriveIcon(HIMAGELIST smallImages, const DriveDesc& d, HDC dc, int x, int y, int size) {
    if (!smallImages) return;
    ImageList_DrawEx(smallImages, d.icon, dc, x, y, size, size, CLR_NONE, CLR_NONE, ILD_TRANSPARENT);
    if (d.linkOverlay) {
        static int link = -1;
        if (link < 0) link = StockIndex(SIID_LINK);
        ImageList_DrawEx(smallImages, link, dc, x, y, size, size, CLR_NONE, CLR_NONE, ILD_TRANSPARENT);
    }
}

} // namespace qf
