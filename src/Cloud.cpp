#include "Cloud.h"
#include "Util.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdlib>
#include <iterator>
#include <mutex>
#include <thread>

namespace qf {

namespace {

void AddFolder(std::vector<CloudFolder>& out, const std::wstring& service, const std::wstring& name, const std::wstring& path) {
    if (path.empty() || !DirExists(path)) return;
    std::wstring n = NormalizeDir(path);
    for (auto& f : out)
        if (EqualsI(f.path, n)) return;
    out.push_back({service, name, n});
}

std::wstring RegString(HKEY root, const std::wstring& key, const wchar_t* value) {
    wchar_t buf[2048] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &cb) !=
        ERROR_SUCCESS)
        return L"";
    return buf;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)std::size(buf));
    return n && n < std::size(buf) ? std::wstring(buf, n) : L"";
}

// Dropbox: info.json enthält {"personal": {"path": "C:\\Users\\…\\Dropbox", …}, "business": {…}}
void DetectDropbox(std::vector<CloudFolder>& out) {
    for (const wchar_t* base : {L"APPDATA", L"LOCALAPPDATA"}) {
        std::wstring dir = Env(base);
        if (dir.empty()) continue;
        std::vector<uint8_t> data;
        if (!ReadFileBytes(PathCombine(dir, L"Dropbox\\info.json"), data)) continue;
        std::string json(data.begin(), data.end());
        for (const char* kind : {"personal", "business"}) {
            size_t k = json.find(std::string("\"") + kind + "\"");
            if (k == std::string::npos) continue;
            size_t p = json.find("\"path\"", k);
            if (p == std::string::npos) continue;
            size_t q1 = json.find('"', json.find(':', p) + 1);
            if (q1 == std::string::npos) continue;
            std::string v;
            for (size_t i = q1 + 1; i < json.size() && json[i] != '"'; ++i) {
                if (json[i] == '\\' && i + 1 < json.size()) {
                    char c = json[++i];
                    if (c == 'u' && i + 4 < json.size()) {
                        wchar_t w = (wchar_t)strtol(json.substr(i + 1, 4).c_str(), nullptr, 16);
                        v += WideToUtf8(std::wstring(1, w));
                        i += 4;
                    } else {
                        v += c;
                    }
                } else {
                    v += json[i];
                }
            }
            AddFolder(out, L"Dropbox", std::string(kind) == "business" ? L"Dropbox (Firma)" : L"Dropbox", Utf8ToWide(v));
        }
    }
    AddFolder(out, L"Dropbox", L"Dropbox", PathCombine(GetKnownFolder(FOLDERID_Profile), L"Dropbox"));
}

// Google Drive für den Desktop: eigenes Laufwerk mit der Bezeichnung „Google Drive“ (darin „Meine Ablage“/„My Drive“);
// ältere Version (Backup and Sync): Ordner „Google Drive“ im Benutzerprofil
// scanVolumes: Laufwerk „Google Drive“ an der Volumebezeichnung erkennen. Das fragt jedes Laufwerk ab (weckt schlafende
// Platten, wartet auf langsame Laufwerke) und läuft deshalb nur im Hintergrund bzw. auf ausdrücklichen Wunsch.
// Netzlaufwerke werden übersprungen (Google Drive für Desktop bindet ein lokales Laufwerk ein).
void DetectGoogleDrive(std::vector<CloudFolder>& out, bool scanVolumes) {
    DWORD mask = scanVolumes ? GetLogicalDrives() : 0;
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    for (int i = 2; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t label[MAX_PATH + 1] = {}, fs[MAX_PATH + 1] = {};
        if (!GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, fs, MAX_PATH + 1)) continue;
        if (!EqualsI(label, L"Google Drive")) continue;
        bool any = false;
        for (const wchar_t* sub : {L"Meine Ablage", L"My Drive"}) {
            std::wstring p = root + sub;
            if (DirExists(p)) {
                AddFolder(out, L"Google Drive", L"Google Drive", p);
                any = true;
            }
        }
        if (!any) AddFolder(out, L"Google Drive", L"Google Drive", root);
    }
    SetErrorMode(oldMode);
    AddFolder(out, L"Google Drive", L"Google Drive", PathCombine(GetKnownFolder(FOLDERID_Profile), L"Google Drive"));
}

// OneDrive: Umgebungsvariablen, Registrierung (alle Konten), bekannter Ordner
void DetectOneDrive(std::vector<CloudFolder>& out) {
    AddFolder(out, L"OneDrive", L"OneDrive", Env(L"OneDriveConsumer"));
    std::wstring commercial = Env(L"OneDriveCommercial");
    if (!commercial.empty()) {
        std::wstring n = PathFileName(NormalizeDir(commercial));   // "OneDrive - Firma"
        AddFolder(out, L"OneDrive", ReplaceAll(n, L" - ", L" – "), commercial);
    }
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive\\Accounts", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD cch = 255;
            if (RegEnumKeyExW(h, i, sub, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring key = std::wstring(L"Software\\Microsoft\\OneDrive\\Accounts\\") + sub;
            std::wstring folder = RegString(HKEY_CURRENT_USER, key, L"UserFolder");
            if (folder.empty()) continue;
            std::wstring n = PathFileName(NormalizeDir(folder));
            AddFolder(out, L"OneDrive", EqualsI(n, L"OneDrive") ? L"OneDrive" : ReplaceAll(n, L" - ", L" – "), folder);
        }
        RegCloseKey(h);
    }
    AddFolder(out, L"OneDrive", L"OneDrive", Env(L"OneDrive"));
    AddFolder(out, L"OneDrive", L"OneDrive", GetKnownFolder(FOLDERID_SkyDrive));
}

std::vector<CloudFolder> Detect(bool scanVolumes) {
    std::vector<CloudFolder> list;
    DetectDropbox(list);
    DetectGoogleDrive(list, scanVolumes);
    DetectOneDrive(list);
    return list;
}

std::vector<CloudFolder> g_list;
bool g_done = false;
std::mutex g_bgMutex;
std::vector<CloudFolder> g_bgResult;
bool g_bgReady = false, g_bgStarted = false;
HWND g_notify = nullptr;
UINT g_notifyMsg = 0;

} // namespace

// Erster Aufruf: schnelle Erkennung (ohne Laufwerksabfrage) und Suche nach dem Google-Drive-Laufwerk im Hintergrund
// (Ergebnis über CloudTakeDetected); refresh: vollständig und sofort (Befehl „Cloud-Lesezeichen hinzufügen“)
const std::vector<CloudFolder>& DetectCloudFolders(bool refresh) {
    if (refresh) {
        g_list = Detect(true);
        g_done = true;
        return g_list;
    }
    if (g_done) return g_list;
    g_done = true;
    g_list = Detect(false);
    std::lock_guard<std::mutex> lock(g_bgMutex);
    if (!g_bgStarted) {
        g_bgStarted = true;
        std::thread([] {
            std::vector<CloudFolder> full = Detect(true);
            HWND h;
            UINT m;
            {
                std::lock_guard<std::mutex> lock(g_bgMutex);
                g_bgResult = std::move(full);
                g_bgReady = true;
                h = g_notify;
                m = g_notifyMsg;
            }
            if (h) PostMessageW(h, m, 0, 0);
        }).detach();
    }
    return g_list;
}

void SetCloudNotifyWindow(void* hwnd, unsigned msg) {
    std::lock_guard<std::mutex> lock(g_bgMutex);
    g_notify = (HWND)hwnd;
    g_notifyMsg = msg;
}

bool CloudTakeDetected() {
    std::vector<CloudFolder> full;
    {
        std::lock_guard<std::mutex> lock(g_bgMutex);
        if (!g_bgReady) return false;
        g_bgReady = false;
        full = std::move(g_bgResult);
    }
    bool changed = full.size() != g_list.size();
    for (size_t i = 0; !changed && i < full.size(); ++i) changed = !EqualsI(full[i].path, g_list[i].path);
    g_list = std::move(full);
    return changed;
}


bool IsCloudPath(const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':') return false;
    std::wstring p = NormalizeDir(path);
    for (auto& f : DetectCloudFolders()) {
        if (EqualsI(p, f.path)) return true;
        std::wstring prefix = f.path;
        if (prefix.back() != L'\\') prefix += L'\\';
        if (StartsWithI(p, prefix)) return true;
    }
    return false;
}

} // namespace qf
