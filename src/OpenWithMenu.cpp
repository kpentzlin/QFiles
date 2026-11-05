// „Öffnen mit“-Auswahl (mittlere Maustaste in den Dateilisten): dieselben Programme wie im Untermenü „Öffnen mit“
// des Explorer-Kontextmenüs (empfohlene Programme laut SHAssocEnumHandlers), dazu „Andere App auswählen…“
// (Windows-Dialog „Öffnen mit“) und „Abbrechen“.

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <string>
#include <vector>

#include "Util.h"

namespace qf {

namespace {

// Symbol als 32-Bit-Bitmap mit vormultipliziertem Alpha (für Menüeinträge)
HBITMAP MenuBitmapFromIcon(HICON icon, int size) {
    if (!icon) return nullptr;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    HBITMAP result = nullptr;
    IWICBitmap* src = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, &src)) && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(src, size, size, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits && SUCCEEDED(conv->CopyPixels(nullptr, size * 4, size * size * 4, (BYTE*)bits))) result = bmp;
        else if (bmp) DeleteObject(bmp);
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (src) src->Release();
    wic->Release();
    return result;
}

HICON HandlerIcon(IAssocHandler* h) {
    LPWSTR iconPath = nullptr;
    int index = 0;
    HICON smallIcon = nullptr;
    if (SUCCEEDED(h->GetIconLocation(&iconPath, &index)) && iconPath) {
        std::wstring p = iconPath;
        CoTaskMemFree(iconPath);
        if (!p.empty() && p[0] == L'@') {
            // indirekte Angabe (Store-Apps): über die Shell auflösen
            wchar_t out[MAX_PATH] = {};
            if (SUCCEEDED(SHLoadIndirectString(p.c_str(), out, MAX_PATH, nullptr))) p = out;
        }
        ExtractIconExW(p.c_str(), index, nullptr, &smallIcon, 1);
    }
    return smallIcon;
}

} // namespace

bool ShowOpenWithMenu(HWND owner, const std::wstring& path, POINT screenPt) {
    std::wstring ext;
    size_t dot = path.find_last_of(L"\\/.");
    if (dot != std::wstring::npos && path[dot] == L'.') ext = path.substr(dot);

    std::vector<IAssocHandler*> handlers;
    if (!ext.empty()) {
        IEnumAssocHandlers* en = nullptr;
        if (SUCCEEDED(SHAssocEnumHandlers(ext.c_str(), ASSOC_FILTER_RECOMMENDED, &en)) && en) {
            IAssocHandler* h = nullptr;
            ULONG got = 0;
            while (en->Next(1, &h, &got) == S_OK && got == 1 && h) {
                handlers.push_back(h);
                if (handlers.size() >= 40) break;
            }
            en->Release();
        }
    }

    constexpr UINT kOther = 0x7F00, kCancel = 0x7F01, kFirst = 1;
    HMENU m = CreatePopupMenu();
    std::vector<HBITMAP> bitmaps;
    int iconSize = GetSystemMetrics(SM_CXSMICON);
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, (L"Öffnen mit – " + PathFileName(path)).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < handlers.size(); ++i) {
        LPWSTR name = nullptr;
        std::wstring text;
        if (SUCCEEDED(handlers[i]->GetUIName(&name)) && name) {
            text = name;
            CoTaskMemFree(name);
        }
        if (text.empty()) text = L"(ohne Namen)";
        // & im Programmnamen nicht als Zugriffstaste deuten
        std::wstring shown;
        for (wchar_t c : text) {
            if (c == L'&') shown += L'&';
            shown += c;
        }
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE;
        mi.fType = MFT_STRING;
        mi.wID = kFirst + (UINT)i;
        mi.dwTypeData = shown.data();
        HICON icon = HandlerIcon(handlers[i]);
        if (HBITMAP bmp = MenuBitmapFromIcon(icon, iconSize)) {
            mi.fMask |= MIIM_BITMAP;
            mi.hbmpItem = bmp;
            bitmaps.push_back(bmp);
        }
        if (icon) DestroyIcon(icon);
        InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
    }
    if (!handlers.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kOther, L"&Andere App auswählen…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kCancel, L"Abbrechen");

    SetForegroundWindow(owner);
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screenPt.x, screenPt.y, 0, owner,
                                    nullptr);
    DestroyMenu(m);
    for (HBITMAP b : bitmaps) DeleteObject(b);

    bool done = false;
    if (cmd >= kFirst && cmd < kFirst + handlers.size()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            IDataObject* data = nullptr;
            if (SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))) && data) {
                done = SUCCEEDED(handlers[cmd - kFirst]->Invoke(data));
                data->Release();
            }
            item->Release();
        }
        if (!done) MsgError(owner, L"Das Programm konnte nicht gestartet werden:\n" + path);
    } else if (cmd == kOther) {
        OPENASINFO oi{path.c_str(), nullptr, OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC};
        done = SUCCEEDED(SHOpenWithDialog(owner, &oi));
    }
    for (auto* h : handlers) h->Release();
    return done;
}

} // namespace qf
