// FTP/SFTP: Zugänge, Verbindungsverwaltung und Dateioperationen (siehe Remote.h).
#include "RemoteFs.h"   // zuerst (winsock2.h vor windows.h)

#include "Remote.h"
#include "App.h"
#include "Dialog.h"
#include "ToolsCommon.h"

#include <wincrypt.h>
#include <dpapi.h>
#include <shlobj.h>

#include <algorithm>
#include <map>
#include <set>

namespace qf {

using namespace remote;
using toolsdetail::ProgressSink;

namespace {

const wchar_t* kAccessSection = L"FTP-Zugaenge";

// ===================== Zugangsdaten =====================

std::wstring ProtectPassword(const std::wstring& pw) {
    if (pw.empty()) return L"";
    DATA_BLOB in{(DWORD)(pw.size() * sizeof(wchar_t)), (BYTE*)pw.data()}, out{};
    if (!CryptProtectData(&in, L"QFiles", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return L"";
    std::string b = Base64Encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return Utf8ToWide(b);
}

std::wstring UnprotectPassword(const std::wstring& stored) {
    if (stored.empty()) return L"";
    std::vector<unsigned char> raw;
    if (!Base64Decode(WideToUtf8(stored), raw) || raw.empty()) return L"";
    DATA_BLOB in{(DWORD)raw.size(), raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return L"";
    std::wstring pw((const wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return pw;
}

// Im Lauf der Sitzung eingegebene (nicht gespeicherte) Kennwörter
std::map<std::wstring, std::wstring>& SessionPasswords() {
    static std::map<std::wstring, std::wstring> m;
    return m;
}

void SaveAccessSecrets(const RemoteAccess& a) {
    Config& c = App::Cfg();
    std::wstring key = a.url.ServerKey();
    c.Set(kAccessSection, key + L"|Kennwort", a.savePassword ? ProtectPassword(a.password) : L"");
    c.Set(kAccessSection, key + L"|Schluessel", a.keyFile);
    c.Save();
    if (!a.password.empty()) SessionPasswords()[key] = a.password;
}

std::wstring KnownHostsFile() { return PathCombine(PathParent(App::Cfg().FilePath()), L"QFiles-Hostschluessel.txt"); }

// ===================== Dialoge =====================

class PasswordDlg : public DialogBase {
public:
    std::wstring prompt, password;
    bool save = false;

protected:
    BOOL OnInit() override {
        SetText(101, prompt);
        SetFocus(Item(102));
        return FALSE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == IDOK) {
            password = GetText(102);
            save = IsChecked(103);
        }
        return DialogBase::OnCommand(id, code, ctl);
    }
};

bool AskPassword(HWND owner, const std::wstring& prompt, std::wstring& password, bool& save) {
    DialogTemplate t(L"Anmeldung", 280, 80);
    t.Label(101, L"", 7, 7, 266, 18);
    t.Edit(102, 7, 27, 266, 14, ES_AUTOHSCROLL | ES_PASSWORD);
    t.Check(103, L"Kennwort &speichern (verschlüsselt, nur für diesen Windows-Benutzer)", 7, 45, 266, 10);
    t.DefButton(IDOK, L"OK", 166, 61, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 223, 61, 50, 14);
    PasswordDlg dlg;
    dlg.prompt = prompt;
    if (dlg.DoModal(owner, t) != IDOK) return false;
    password = dlg.password;
    save = dlg.save;
    return true;
}

ConnectPrompts MakePrompts(HWND owner, const std::wstring& serverKey, bool* savePw) {
    ConnectPrompts p;
    p.confirmHostKey = [owner](const std::wstring& host, const std::wstring& type, const std::wstring& fp, bool changed) {
        std::wstring msg;
        UINT flags = MB_YESNO;
        if (changed) {
            msg = L"WARNUNG: Der Hostschlüssel von " + host + L" hat sich geändert!\n\n"
                  L"Das kann auf einen Angriff hindeuten (jemand gibt sich als der Server aus) – oder der Server wurde "
                  L"neu eingerichtet.\n\nNeuer Schlüssel: " + type + L"\nFingerabdruck: " + fp +
                  L"\n\nTrotzdem verbinden und den neuen Schlüssel speichern?";
            flags |= MB_ICONWARNING | MB_DEFBUTTON2;
        } else {
            msg = L"Der Server " + host + L" ist noch unbekannt.\n\nSchlüssel: " + type + L"\nFingerabdruck: " + fp +
                  L"\n\nPrüfen Sie den Fingerabdruck (z. B. beim Betreiber). Verbinden und den Schlüssel speichern?";
            flags |= MB_ICONQUESTION;
        }
        return MessageBoxW(owner, msg.c_str(), L"QFiles – SFTP-Hostschlüssel", flags) == IDYES;
    };
    p.askPassword = [owner, savePw](const std::wstring& prompt, std::wstring& pw) {
        bool save = false;
        if (!AskPassword(owner, prompt, pw, save)) return false;
        if (savePw) *savePw = save;
        return true;
    };
    (void)serverKey;
    return p;
}

// ===================== Verbindungen =====================

std::map<std::wstring, std::shared_ptr<RemoteFs>>& Pool() {
    static std::map<std::wstring, std::shared_ptr<RemoteFs>> pool;
    return pool;
}

ConnectSettings SettingsFor(const RemoteUrl& url) {
    RemoteAccess a = LoadRemoteAccess(url.ToString());
    ConnectSettings s;
    s.url = url;
    s.password = a.password;
    auto sp = SessionPasswords().find(url.ServerKey());
    if (s.password.empty() && sp != SessionPasswords().end()) s.password = sp->second;
    s.keyFile = a.keyFile;
    s.knownHostsFile = KnownHostsFile();
    return s;
}

// Verbindung holen bzw. aufbauen (UI-Thread, mit Rückfragen)
std::shared_ptr<RemoteFs> Acquire(HWND owner, const RemoteUrl& url, std::wstring& err) {
    std::wstring key = url.ServerKey();
    auto& pool = Pool();
    auto it = pool.find(key);
    if (it != pool.end()) {
        std::unique_lock<std::recursive_mutex> lock(it->second->mutex(), std::try_to_lock);
        if (!lock.owns_lock()) {
            err = L"Die Verbindung ist gerade mit einer anderen Übertragung beschäftigt.";
            return nullptr;
        }
        if (it->second->Connected()) return it->second;
    }
    std::shared_ptr<RemoteFs> fs = it != pool.end() ? it->second
                                                    : std::shared_ptr<RemoteFs>(url.proto == RemoteProto::Sftp ? CreateSftpFs()
                                                                                                                : CreateFtpFs());
    std::lock_guard<std::recursive_mutex> lock(fs->mutex());
    bool savePw = false;
    ConnectPrompts prompts = MakePrompts(owner, key, &savePw);
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool ok = fs->Connect(SettingsFor(url), &prompts, err);
    SetCursor(old);
    if (!ok) return nullptr;
    // Eingegebenes Kennwort merken (Sitzung) bzw. speichern
    if (!fs->settings.password.empty()) {
        SessionPasswords()[key] = fs->settings.password;
        if (savePw) {
            RemoteAccess a = LoadRemoteAccess(url.ToString());
            a.password = fs->settings.password;
            a.savePassword = true;
            SaveAccessSecrets(a);
        }
    }
    pool[key] = fs;
    LogOperation(L"Verbunden mit " + key);
    return fs;
}

// Im Arbeitsthread: Verbindung ggf. ohne Rückfragen erneuern
bool Reconnect(RemoteFs& fs, std::wstring& err) {
    if (fs.Connected()) return true;
    return fs.Connect(fs.settings, nullptr, err);
}

// Führt op aus; bei Verbindungsverlust einmal neu verbinden und wiederholen
template <class F>
bool WithRetry(RemoteFs& fs, std::wstring& err, F op) {
    std::lock_guard<std::recursive_mutex> lock(fs.mutex());
    if (!Reconnect(fs, err)) return false;
    if (op(err)) return true;
    if (fs.Connected()) return false;
    std::wstring err2;
    if (!Reconnect(fs, err2)) return false;
    return op(err);
}

// ===================== Orte für Übertragungen =====================

struct Loc {
    bool remote = false;
    std::shared_ptr<RemoteFs> fs;
    RemoteUrl url;          // bei remote (url.path = Server-Pfad)
    std::wstring local;     // bei lokal
    std::wstring Display() const { return remote ? url.ToString() : local; }
    Loc Child(const std::wstring& name) const {
        Loc c = *this;
        if (remote) c.url.path = NormalizeRemotePath(url.path + L"/" + name);
        else c.local = PathCombine(local, name);
        return c;
    }
    std::wstring Name() const { return remote ? LocationFileName(url.ToString()) : PathFileName(local); }
};

bool MakeLoc(HWND owner, const std::wstring& location, Loc& out, std::wstring& err) {
    out = Loc{};
    if (IsRemoteUrl(location)) {
        if (!ParseRemoteUrl(location, out.url)) {
            err = L"Ungültige Adresse: " + location;
            return false;
        }
        out.remote = true;
        out.fs = Acquire(owner, out.url, err);
        return out.fs != nullptr;
    }
    out.local = location;
    return true;
}

struct Job {
    Loc src, dst;
    bool isDir = false;
    uint64_t size = 0;
    FILETIME mtime{};
    int top = 0;            // Index des obersten Quellelements (für Verschieben)
};

bool LocalDeleteTree(const std::wstring& path, std::wstring& err) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(LongPath(path).c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::vector<DirEntry> entries;
            ListDirectory(path, entries);
            for (auto& e : entries)
                if (!LocalDeleteTree(PathCombine(path, e.name), err)) return false;
        }
        if (!RemoveDirectoryW(LongPath(path).c_str())) {
            err = path + L": " + LastErrorMessage();
            return false;
        }
        return true;
    }
    if (!::DeleteFileW(LongPath(path).c_str())) {
        err = path + L": " + LastErrorMessage();
        return false;
    }
    return true;
}

bool RemoteDeleteTree(RemoteFs& fs, const std::wstring& path, bool isDir, ProgressSink* sink, std::wstring& err) {
    if (sink && sink->Cancelled()) {
        err = L"Abgebrochen.";
        return false;
    }
    if (!isDir) return WithRetry(fs, err, [&](std::wstring& e) { return fs.DeleteFile(path, e); });
    std::vector<DirEntry> entries;
    if (!WithRetry(fs, err, [&](std::wstring& e) { return fs.List(path, entries, e); })) return false;
    for (auto& e : entries) {
        if (sink) sink->SetText(L"Lösche " + e.name + L" …");
        if (!RemoteDeleteTree(fs, NormalizeRemotePath(path + L"/" + e.name), e.IsDir(), sink, err)) return false;
    }
    return WithRetry(fs, err, [&](std::wstring& e) { return fs.RemoveDir(path, e); });
}

bool LocExists(Loc& l, bool* isDir) {
    if (l.remote) {
        DirEntry d;
        std::wstring err;
        bool ok = WithRetry(*l.fs, err, [&](std::wstring&) { return l.fs->Stat(l.url.path, d); });
        if (ok && isDir) *isDir = d.IsDir();
        return ok;
    }
    DWORD a = GetFileAttributesW(LongPath(l.local).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (isDir) *isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

std::wstring TempDirFor(const std::wstring& key) {
    // Eigener Unterordner je Server/Verzeichnis, damit gleichnamige Dateien sich nicht überschreiben
    uint32_t h = 2166136261u;
    for (wchar_t c : key) h = (h ^ (uint32_t)c) * 16777619u;
    std::wstring dir = PathCombine(PathCombine(GetTempDir(), L"QFiles-FTP"), Format(L"%08X", h));
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

// Eine Datei übertragen
bool CopyFileJob(const Job& j, ProgressSink& sink, uint64_t baseDone, uint64_t total, std::wstring& err) {
    auto progress = [&](uint64_t done) {
        if (total > 0) sink.SetPercent((int)std::min<uint64_t>(100, (baseDone + done) * 100 / total));
        return !sink.Cancelled();
    };
    if (j.src.remote && !j.dst.remote) {
        HANDLE h = CreateFileW(LongPath(j.dst.local).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = j.dst.local + L": " + LastErrorMessage();
            return false;
        }
        bool ok = WithRetry(*j.src.fs, err, [&](std::wstring& e) {
            SetFilePointer(h, 0, nullptr, FILE_BEGIN);
            SetEndOfFile(h);
            return j.src.fs->Download(j.src.url.path, h, progress, e);
        });
        if (ok) SetFileTime(h, nullptr, nullptr, &j.mtime);
        CloseHandle(h);
        if (!ok) ::DeleteFileW(LongPath(j.dst.local).c_str());
        return ok;
    }
    if (!j.src.remote && j.dst.remote) {
        HANDLE h = CreateFileW(LongPath(j.src.local).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = j.src.local + L": " + LastErrorMessage();
            return false;
        }
        bool ok = WithRetry(*j.dst.fs, err, [&](std::wstring& e) {
            SetFilePointer(h, 0, nullptr, FILE_BEGIN);
            return j.dst.fs->Upload(h, j.dst.url.path, progress, e);
        });
        CloseHandle(h);
        if (ok) {
            std::lock_guard<std::recursive_mutex> lock(j.dst.fs->mutex());
            j.dst.fs->SetModTime(j.dst.url.path, j.mtime);
        }
        return ok;
    }
    if (j.src.remote && j.dst.remote) {
        // Server -> Server: über eine temporäre Datei
        std::wstring tmp = PathCombine(TempDirFor(L"transfer"), j.src.Name());
        Job down = j;
        down.dst = Loc{};
        down.dst.local = tmp;
        if (!CopyFileJob(down, sink, baseDone, total * 2, err)) return false;
        Job up = j;
        up.src = Loc{};
        up.src.local = tmp;
        bool ok = CopyFileJob(up, sink, baseDone + j.size, total * 2, err);
        ::DeleteFileW(LongPath(tmp).c_str());
        return ok;
    }
    // lokal -> lokal (nicht über FTP)
    if (!CopyFileExW(LongPath(j.src.local).c_str(), LongPath(j.dst.local).c_str(), nullptr, nullptr, nullptr, 0)) {
        err = j.src.local + L": " + LastErrorMessage();
        return false;
    }
    return true;
}

bool MakeDirLoc(Loc& l, std::wstring& err) {
    bool isDir = false;
    if (LocExists(l, &isDir)) {
        if (isDir) return true;
        err = l.Display() + L": Es gibt bereits eine Datei mit diesem Namen.";
        return false;
    }
    if (l.remote) return WithRetry(*l.fs, err, [&](std::wstring& e) { return l.fs->MakeDir(l.url.path, e); });
    if (!CreateDirectoryW(LongPath(l.local).c_str(), nullptr)) {
        err = l.local + L": " + LastErrorMessage();
        return false;
    }
    return true;
}

// Plan aufstellen: Verzeichnisse rekursiv auflösen
bool PlanItem(const Loc& src, const Loc& dst, const DirEntry& e, int top, std::vector<Job>& jobs, ProgressSink& sink,
              std::wstring& err) {
    if (sink.Cancelled()) {
        err = L"Abgebrochen.";
        return false;
    }
    Job j{src, dst, e.IsDir(), e.size, e.modified, top};
    jobs.push_back(j);
    if (!e.IsDir()) return true;
    sink.SetText(L"Ermittle Dateien: " + src.Display());
    std::vector<DirEntry> entries;
    if (src.remote) {
        if (!WithRetry(*src.fs, err, [&](std::wstring& er) { return src.fs->List(src.url.path, entries, er); })) {
            err = src.Display() + L": " + err;
            return false;
        }
    } else if (!(e.attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        DWORD le = 0;
        if (!ListDirectory(src.local, entries, &le)) {
            err = src.local + L": " + LastErrorMessage(le);
            return false;
        }
    }
    for (auto& c : entries)
        if (!PlanItem(src.Child(c.name), dst.Child(c.name), c, top, jobs, sink, err)) return false;
    return true;
}

bool StatLoc(Loc& l, DirEntry& e, std::wstring& err) {
    if (l.remote) {
        bool ok = WithRetry(*l.fs, err, [&](std::wstring&) { return l.fs->Stat(l.url.path, e); });
        if (!ok && err.empty()) err = l.Display() + L": nicht gefunden.";
        e.name = l.Name();
        return ok;
    }
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(LongPath(l.local).c_str(), GetFileExInfoStandard, &d)) {
        err = l.local + L": " + LastErrorMessage();
        return false;
    }
    e.name = PathFileName(l.local);
    e.attributes = d.dwFileAttributes;
    e.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    e.modified = d.ftLastWriteTime;
    return true;
}

} // namespace

// ===================== Öffentliche Funktionen =====================

RemoteAccess LoadRemoteAccess(const std::wstring& url) {
    RemoteAccess a;
    ParseRemoteUrl(url, a.url);
    const Config& c = App::Cfg();
    std::wstring key = a.url.ServerKey();
    a.password = UnprotectPassword(c.Get(kAccessSection, key + L"|Kennwort"));
    a.savePassword = !a.password.empty();
    a.keyFile = c.Get(kAccessSection, key + L"|Schluessel");
    return a;
}

bool HasSavedRemoteLogin(const std::wstring& url) {
    RemoteAccess a = LoadRemoteAccess(url);
    return !a.password.empty() || !a.keyFile.empty() || (a.url.proto == RemoteProto::Ftp && a.url.user.empty());
}

bool EditRemoteAccess(HWND owner, RemoteAccess& access, bool isNew) {
    enum { kName = 101, kProto, kHost, kPort, kUser, kPass, kSave, kDir, kKey, kKeyBtn, kTest, kHint, kKeyLabel };
    class Dlg : public DialogBase {
    public:
        RemoteAccess a;
        bool isNew = true;

    protected:
        BOOL OnInit() override {
            SetText(kName, a.name);
            ComboAdd(kProto, L"SFTP (SSH, verschlüsselt)", 1);
            ComboAdd(kProto, L"FTP (unverschlüsselt)", 0);
            ComboSetSel(kProto, a.url.proto == RemoteProto::Sftp ? 0 : 1);
            SetText(kHost, a.url.host);
            SetText(kPort, std::to_wstring(a.url.EffectivePort()));
            SetText(kUser, a.url.user);
            SetText(kPass, a.password);
            SetCheck(kSave, a.savePassword || isNew);
            SetText(kDir, a.url.path == L"/" && isNew ? L"" : a.url.path);
            SetText(kKey, a.keyFile);
            UpdateProto();
            SetFocus(Item(isNew ? kHost : kName));
            return FALSE;
        }
        void UpdateProto() {
            bool sftp = ComboSel(kProto) == 0;
            Enable(kKey, sftp);
            Enable(kKeyBtn, sftp);
            Enable(kKeyLabel, sftp);
        }
        bool Read(RemoteAccess& out, bool quiet) {
            out = a;
            out.name = Trim(GetText(kName));
            out.url.proto = ComboSel(kProto) == 0 ? RemoteProto::Sftp : RemoteProto::Ftp;
            std::wstring host = Trim(GetText(kHost));
            // Vollständige Adresse im Serverfeld akzeptieren
            RemoteUrl parsed;
            if (ParseRemoteUrl(host, parsed)) {
                out.url = parsed;
                SetText(kHost, parsed.host);
                if (!parsed.user.empty()) SetText(kUser, parsed.user);
                SetText(kPort, std::to_wstring(parsed.EffectivePort()));
                ComboSetSel(kProto, parsed.proto == RemoteProto::Sftp ? 0 : 1);
                if (parsed.path != L"/") SetText(kDir, parsed.path);
                host = parsed.host;
            }
            out.url.host = host;
            out.url.port = (int)GetInt(kPort, 0);
            out.url.user = Trim(GetText(kUser));
            out.password = GetText(kPass);
            out.savePassword = IsChecked(kSave);
            std::wstring dir = Trim(GetText(kDir));
            out.url.path = NormalizeRemotePath(dir.empty() ? L"/" : dir);
            out.keyFile = out.url.proto == RemoteProto::Sftp ? Trim(GetText(kKey)) : L"";
            if (out.url.host.empty() || out.url.host.find_first_of(L" /\\") != std::wstring::npos) {
                if (!quiet) MsgError(hwnd_, L"Bitte einen gültigen Servernamen oder eine IP-Adresse eingeben.");
                return false;
            }
            if (out.url.port <= 0 || out.url.port > 65535) {
                if (!quiet) MsgError(hwnd_, L"Ungültiger Port.");
                return false;
            }
            if (out.url.proto == RemoteProto::Sftp && out.url.user.empty()) {
                if (!quiet) MsgError(hwnd_, L"Für SFTP ist ein Benutzername erforderlich.");
                return false;
            }
            if (out.url.port == (out.url.proto == RemoteProto::Sftp ? 22 : 21)) out.url.port = 0;
            if (out.name.empty()) out.name = out.url.user.empty() ? out.url.host : (out.url.user + L"@" + out.url.host);
            return true;
        }
        // Verbindung testen; liefert das Startverzeichnis des Servers
        bool Test(const RemoteAccess& r, std::wstring& home, std::wstring& err) {
            ConnectSettings s;
            s.url = r.url;
            s.password = r.password;
            s.keyFile = r.keyFile;
            s.knownHostsFile = KnownHostsFile();
            std::unique_ptr<RemoteFs> fs = r.url.proto == RemoteProto::Sftp ? CreateSftpFs() : CreateFtpFs();
            bool savePw = false;
            ConnectPrompts p = MakePrompts(hwnd_, r.url.ServerKey(), &savePw);
            HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
            bool ok = fs->Connect(s, &p, err);
            if (ok) {
                home = fs->HomeDir();
                if (!fs->settings.password.empty() && r.password.empty()) SetText(kPass, fs->settings.password);
                std::vector<DirEntry> entries;
                std::wstring listErr;
                if (r.url.path != L"/" && !fs->List(r.url.path, entries, listErr)) {
                    err = L"Anmeldung erfolgreich, aber das Startverzeichnis „" + r.url.path + L"“ ist nicht lesbar:\n" + listErr;
                    ok = false;
                }
                fs->Disconnect();
            }
            SetCursor(old);
            return ok;
        }
        BOOL OnCommand(int id, int code, HWND ctl) override {
            if (id == kProto && code == CBN_SELCHANGE) {
                int port = (int)GetInt(kPort, 0);
                bool sftp = ComboSel(kProto) == 0;
                if (port == 0 || port == 21 || port == 22) SetText(kPort, sftp ? L"22" : L"21");
                UpdateProto();
                return TRUE;
            }
            if (id == kKeyBtn) {
                std::wstring f = OpenFileDialog(hwnd_, L"Privaten Schlüssel wählen", GetKnownFolder(FOLDERID_Profile) + L"\\.ssh",
                                                L"Alle Dateien|*.*");
                if (!f.empty()) SetText(kKey, f);
                return TRUE;
            }
            if (id == kTest) {
                RemoteAccess r;
                if (!Read(r, false)) return TRUE;
                std::wstring home, err;
                if (Test(r, home, err))
                    MsgInfo(hwnd_, L"Verbindung erfolgreich.\nStartverzeichnis des Servers: " + home);
                else
                    MsgError(hwnd_, L"Verbindung fehlgeschlagen:\n" + err);
                return TRUE;
            }
            if (id == IDOK) {
                RemoteAccess r;
                if (!Read(r, false)) return TRUE;
                if (Trim(GetText(kDir)).empty()) {
                    // Startverzeichnis = Anmeldeverzeichnis des Servers: einmal verbinden und erfragen
                    std::wstring home, err;
                    if (Test(r, home, err)) {
                        r.url.path = home;
                        r.password = GetText(kPass);
                    } else if (!MsgConfirm(hwnd_, L"Verbindung fehlgeschlagen:\n" + err +
                                                      L"\n\nZugang trotzdem speichern (Startverzeichnis „/“)?")) {
                        return TRUE;
                    }
                }
                a = r;
                End(IDOK);
                return TRUE;
            }
            return DialogBase::OnCommand(id, code, ctl);
        }
    };
    DialogTemplate t(isNew ? L"Neuer FTP/sFTP-Zugriff" : L"FTP/sFTP-Zugriff bearbeiten", 280, 222);
    int y = 7;
    t.Label(-1, L"&Name (Lesezeichen):", 7, y + 2, 80, 10);
    t.Edit(kName, 90, y, 183, 14, ES_AUTOHSCROLL);
    y += 20;
    t.Label(-1, L"&Protokoll:", 7, y + 2, 80, 10);
    t.Combo(kProto, 90, y, 183, 60);
    y += 20;
    t.Label(-1, L"&Server:", 7, y + 2, 80, 10);
    t.Edit(kHost, 90, y, 130, 14, ES_AUTOHSCROLL);
    t.Label(-1, L"P&ort:", 226, y + 2, 20, 10);
    t.Edit(kPort, 248, y, 25, 14, ES_NUMBER);
    y += 20;
    t.Label(-1, L"&Benutzer:", 7, y + 2, 80, 10);
    t.Edit(kUser, 90, y, 183, 14, ES_AUTOHSCROLL);
    y += 20;
    t.Label(-1, L"&Kennwort:", 7, y + 2, 80, 10);
    t.Edit(kPass, 90, y, 183, 14, ES_AUTOHSCROLL | ES_PASSWORD);
    y += 17;
    t.Check(kSave, L"Kennwort spei&chern (verschlüsselt)", 90, y, 183, 10);
    y += 16;
    t.Label(-1, L"S&tartverzeichnis:", 7, y + 2, 80, 10);
    t.Edit(kDir, 90, y, 183, 14, ES_AUTOHSCROLL);
    y += 20;
    t.Label(kKeyLabel, L"Sch&lüsseldatei (SFTP):", 7, y + 2, 82, 10);
    t.Edit(kKey, 90, y, 160, 14, ES_AUTOHSCROLL);
    t.Button(kKeyBtn, L"…", 254, y, 19, 14);
    y += 20;
    t.Label(kHint,
            L"Leeres Startverzeichnis = Anmeldeverzeichnis des Servers. Ohne Kennwort wird beim Verbinden gefragt; bei FTP "
            L"ohne Benutzer wird anonym angemeldet. FTP überträgt Kennwort und Daten unverschlüsselt.",
            7, y, 266, 26);
    t.Button(kTest, L"&Verbindung testen", 7, 201, 70, 14);
    t.DefButton(IDOK, L"OK", 166, 201, 50, 14);
    t.Button(IDCANCEL, L"Abbrechen", 223, 201, 50, 14);
    Dlg dlg;
    dlg.a = access;
    dlg.isNew = isNew;
    if (dlg.DoModal(owner, t) != IDOK) return false;
    // Alte Verbindung (geänderte Zugangsdaten) schließen
    std::wstring oldKey = access.url.ServerKey();
    access = dlg.a;
    auto& pool = Pool();
    for (const std::wstring& k : {oldKey, access.url.ServerKey()}) {
        auto it = pool.find(k);
        if (it != pool.end()) {
            std::unique_lock<std::recursive_mutex> lock(it->second->mutex(), std::try_to_lock);
            if (lock.owns_lock()) {
                it->second->Disconnect();
                lock.unlock();
                pool.erase(it);
            }
        }
    }
    SessionPasswords().erase(access.url.ServerKey());
    SaveAccessSecrets(access);
    return true;
}

bool RemoteListDirectory(HWND owner, const std::wstring& url, std::vector<DirEntry>& out, std::wstring& err) {
    RemoteUrl u;
    if (!ParseRemoteUrl(url, u)) {
        err = L"Ungültige Adresse.";
        return false;
    }
    auto fs = Acquire(owner, u, err);
    if (!fs) return false;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool ok = WithRetry(*fs, err, [&](std::wstring& e) { return fs->List(u.path, out, e); });
    SetCursor(old);
    return ok;
}

bool RemoteExists(HWND owner, const std::wstring& url) {
    Loc l;
    std::wstring err;
    if (!MakeLoc(owner, url, l, err)) return false;
    return LocExists(l, nullptr);
}

bool RemoteRename(HWND owner, const std::wstring& url, const std::wstring& newName) {
    Loc l;
    std::wstring err;
    if (newName.find_first_of(L"/\\") != std::wstring::npos) {
        MsgError(owner, L"Der Name darf keinen Schrägstrich enthalten.");
        return false;
    }
    if (!MakeLoc(owner, url, l, err)) {
        MsgError(owner, err);
        return false;
    }
    std::wstring parent = l.url.path.substr(0, l.url.path.find_last_of(L'/'));
    std::wstring target = NormalizeRemotePath(parent + L"/" + newName);
    if (!WithRetry(*l.fs, err, [&](std::wstring& e) { return l.fs->Rename(l.url.path, target, e); })) {
        MsgError(owner, L"„" + l.Name() + L"“ kann nicht umbenannt werden:\n" + err);
        return false;
    }
    LogOperation(L"Umbenannt: " + url + L" -> " + newName);
    return true;
}

bool RemoteMakeDir(HWND owner, const std::wstring& dirUrl, const std::wstring& name) {
    Loc l;
    std::wstring err;
    if (!MakeLoc(owner, dirUrl, l, err)) {
        MsgError(owner, err);
        return false;
    }
    // Mehrere Ebenen ("a\b" oder "a/b") nacheinander anlegen
    std::wstring rel = ReplaceAll(name, L"\\", L"/");
    Loc cur = l;
    for (auto& part : Split(rel, L'/')) {
        cur = cur.Child(part);
        if (!MakeDirLoc(cur, err)) {
            MsgError(owner, L"Das Verzeichnis kann nicht angelegt werden:\n" + err);
            return false;
        }
    }
    LogOperation(L"Verzeichnis angelegt: " + cur.Display());
    return true;
}

bool RemoteCreateFile(HWND owner, const std::wstring& dirUrl, const std::wstring& name) {
    Loc l;
    std::wstring err;
    if (!MakeLoc(owner, dirUrl, l, err)) {
        MsgError(owner, err);
        return false;
    }
    Loc f = l.Child(name);
    std::wstring tmp = PathCombine(TempDirFor(L"neu"), L"leer.tmp");
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        MsgError(owner, LastErrorMessage());
        return false;
    }
    bool ok = WithRetry(*f.fs, err, [&](std::wstring& e) {
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        return f.fs->Upload(h, f.url.path, nullptr, e);
    });
    CloseHandle(h);
    ::DeleteFileW(tmp.c_str());
    if (!ok) MsgError(owner, L"„" + name + L"“ kann nicht angelegt werden:\n" + err);
    else LogOperation(L"Datei angelegt: " + f.Display());
    return ok;
}

bool RemoteDelete(HWND owner, const std::wstring& dirUrl, const std::vector<std::wstring>& names, bool confirm) {
    if (names.empty()) return false;
    Loc dir;
    std::wstring err;
    if (!MakeLoc(owner, dirUrl, dir, err)) {
        MsgError(owner, err);
        return false;
    }
    std::wstring what = names.size() == 1 ? (L"„" + names[0] + L"“") : (std::to_wstring(names.size()) + L" Elemente");
    if (confirm && !MsgConfirm(owner, what + L" auf dem Server endgültig löschen?\n(Verzeichnisse mit ihrem gesamten Inhalt.)"))
        return false;
    // Art der Elemente bestimmen
    std::vector<DirEntry> entries;
    if (!WithRetry(*dir.fs, err, [&](std::wstring& e) { return dir.fs->List(dir.url.path, entries, e); })) {
        MsgError(owner, err);
        return false;
    }
    std::vector<std::wstring> errors;
    std::shared_ptr<RemoteFs> fs = dir.fs;
    toolsdetail::RunWithProgress(owner, L"Löschen", L"Lösche …", [&](ProgressSink& sink) {
        for (auto& n : names) {
            if (sink.Cancelled()) break;
            bool isDir = false;
            for (auto& e : entries)
                if (e.name == n) isDir = e.IsDir();
            sink.SetText(L"Lösche " + n + L" …");
            std::wstring e;
            if (!RemoteDeleteTree(*fs, NormalizeRemotePath(dir.url.path + L"/" + n), isDir, &sink, e))
                errors.push_back(n + L": " + e);
            else
                LogOperation(L"Gelöscht: " + dir.Child(n).Display());
        }
    });
    if (!errors.empty()) {
        if (errors.size() > 15) errors.resize(15);
        MsgError(owner, L"Nicht alles konnte gelöscht werden:\n\n" + Join(errors, L"\n"));
    }
    return errors.empty();
}

bool RemoteTransfer(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& targetDir, bool move) {
    if (sources.empty()) return false;
    std::wstring err;
    Loc dst;
    if (!MakeLoc(owner, targetDir, dst, err)) {
        MsgError(owner, err);
        return false;
    }
    std::vector<Loc> srcs;
    for (auto& s : sources) {
        Loc l;
        if (!MakeLoc(owner, s, l, err)) {
            MsgError(owner, err);
            return false;
        }
        if (l.remote && dst.remote && l.fs == dst.fs && LocationParent(s) == targetDir) {
            MsgError(owner, L"Quelle und Ziel sind dasselbe Verzeichnis.");
            return false;
        }
        srcs.push_back(l);
    }
    // Bereits vorhandene Elemente im Ziel?
    std::vector<std::wstring> existing;
    std::set<std::wstring> targetNames;
    if (dst.remote) {
        std::vector<DirEntry> entries;
        if (!WithRetry(*dst.fs, err, [&](std::wstring& e) { return dst.fs->List(dst.url.path, entries, e); })) {
            MsgError(owner, L"Das Zielverzeichnis ist nicht lesbar:\n" + err);
            return false;
        }
        for (auto& e : entries) targetNames.insert(e.name);
    }
    for (auto& s : srcs) {
        std::wstring n = s.Name();
        bool exists = dst.remote ? targetNames.count(n) > 0 : PathExists(PathCombine(dst.local, n));
        if (exists) existing.push_back(n);
    }
    bool overwrite = true;
    if (!existing.empty()) {
        std::wstring list = existing.size() == 1 ? (L"„" + existing[0] + L"“ existiert") : (std::to_wstring(existing.size()) + L" Elemente existieren");
        int r = MsgYesNoCancel(owner, list + L" bereits im Ziel.\n\nJa = überschreiben\nNein = vorhandene überspringen\nAbbrechen = nichts tun");
        if (r == IDCANCEL) return false;
        overwrite = r == IDYES;
    }
    std::vector<std::wstring> errors;
    bool cancelled = false;
    toolsdetail::RunWithProgress(owner, move ? L"Verschieben" : L"Kopieren", L"Ermittle Dateien …", [&](ProgressSink& sink) {
        std::vector<Job> jobs;
        for (int i = 0; i < (int)srcs.size(); ++i) {
            DirEntry e;
            std::wstring er;
            if (!StatLoc(srcs[i], e, er)) {
                errors.push_back(er);
                continue;
            }
            if (!overwrite && std::find(existing.begin(), existing.end(), srcs[i].Name()) != existing.end() && !e.IsDir())
                continue;
            if (!PlanItem(srcs[i], dst.Child(srcs[i].Name()), e, i, jobs, sink, er)) {
                errors.push_back(er);
                if (sink.Cancelled()) return;
            }
        }
        uint64_t total = 0;
        for (auto& j : jobs)
            if (!j.isDir) total += j.size;
        uint64_t done = 0;
        std::vector<bool> topFailed(srcs.size(), false);
        int fileCount = 0, fileIndex = 0;
        for (auto& j : jobs)
            if (!j.isDir) ++fileCount;
        for (auto& j : jobs) {
            if (sink.Cancelled()) {
                cancelled = true;
                return;
            }
            std::wstring er;
            if (j.isDir) {
                if (!MakeDirLoc(j.dst, er)) {
                    errors.push_back(er);
                    topFailed[j.top] = true;
                }
                continue;
            }
            ++fileIndex;
            sink.SetText((move ? L"Verschiebe " : L"Kopiere ") + j.src.Name() + L"  (" + std::to_wstring(fileIndex) + L" von " +
                         std::to_wstring(fileCount) + L")");
            if (!overwrite && LocExists(j.dst, nullptr)) {
                done += j.size;
                continue;
            }
            if (!CopyFileJob(j, sink, done, total, er)) {
                if (sink.Cancelled()) {
                    cancelled = true;
                    return;
                }
                errors.push_back(j.src.Display() + L": " + er);
                topFailed[j.top] = true;
            }
            done += j.size;
        }
        // Verschieben: erfolgreich übertragene Quellen löschen
        if (move) {
            for (size_t i = 0; i < srcs.size(); ++i) {
                if (topFailed[i] || sink.Cancelled()) continue;
                bool planned = false;
                for (auto& j : jobs)
                    if (j.top == (int)i) planned = true;
                if (!planned) continue;
                std::wstring er;
                sink.SetText(L"Entferne Quelle " + srcs[i].Name() + L" …");
                bool isDir = false;
                for (auto& j : jobs)
                    if (j.top == (int)i) {
                        isDir = j.isDir;
                        break;
                    }
                bool ok = srcs[i].remote ? RemoteDeleteTree(*srcs[i].fs, srcs[i].url.path, isDir, &sink, er)
                                         : LocalDeleteTree(srcs[i].local, er);
                if (!ok) errors.push_back(L"Quelle nicht entfernt: " + er);
            }
        }
    });
    LogOperation(std::wstring(move ? L"Verschoben" : L"Kopiert") + L" nach " + targetDir + L": " +
                 std::to_wstring(sources.size()) + L" Element(e)" + (cancelled ? L" (abgebrochen)" : L""));
    if (!errors.empty()) {
        size_t n = errors.size();
        if (errors.size() > 15) errors.resize(15);
        MsgError(owner, L"Bei " + std::to_wstring(n) + L" Element(en) ist ein Fehler aufgetreten:\n\n" + Join(errors, L"\n"));
    }
    return errors.empty() && !cancelled;
}

bool RemoteDownloadTemp(HWND owner, const std::wstring& url, std::wstring& localPath, std::wstring& err, uint64_t maxBytes) {
    Loc l;
    if (!MakeLoc(owner, url, l, err)) return false;
    DirEntry e;
    if (!StatLoc(l, e, err)) return false;
    if (e.IsDir()) {
        err = L"Verzeichnis.";
        return false;
    }
    if (e.size > maxBytes) {
        err = L"Datei zu groß (" + FormatSize(e.size) + L").";
        return false;
    }
    std::wstring dir = TempDirFor(LocationParent(url));
    localPath = PathCombine(dir, l.Name());
    // Bereits aktuell vorhanden?
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(LongPath(localPath).c_str(), GetFileExInfoStandard, &d)) {
        uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        if (size == e.size && CompareFileTime(&d.ftLastWriteTime, &e.modified) == 0 && (e.modified.dwLowDateTime || e.modified.dwHighDateTime))
            return true;
    }
    Job j;
    j.src = l;
    j.dst.local = localPath;
    j.size = e.size;
    j.mtime = e.modified;
    bool ok = false;
    if (e.size < 2 * 1024 * 1024) {
        // Kleine Dateien ohne Fortschrittsdialog
        ProgressSink sink;
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        ok = CopyFileJob(j, sink, 0, 0, err);
        SetCursor(old);
    } else {
        toolsdetail::RunWithProgress(owner, L"Herunterladen", L"Lade " + l.Name() + L" …",
                                     [&](ProgressSink& sink) { ok = CopyFileJob(j, sink, 0, e.size, err); });
    }
    return ok;
}

// ===================== Bearbeiten mit Rückübertragung =====================

namespace {
struct EditSession {
    std::wstring local, url;
    FILETIME lastWrite{};
    bool failed = false;
};
std::vector<EditSession>& EditSessions() {
    static std::vector<EditSession> v;
    return v;
}
bool LocalWriteTime(const std::wstring& path, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &d)) return false;
    ft = d.ftLastWriteTime;
    return true;
}
} // namespace

void RemoteEditRegister(const std::wstring& localPath, const std::wstring& url) {
    auto& v = EditSessions();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const EditSession& s) { return EqualsI(s.local, localPath); }), v.end());
    EditSession s{localPath, url};
    LocalWriteTime(localPath, s.lastWrite);
    v.push_back(s);
}

std::wstring RemoteEditPoll(HWND owner) {
    std::wstring status;
    static bool busy = false;
    if (busy) return L"";
    busy = true;
    for (auto& s : EditSessions()) {
        FILETIME ft{};
        if (!LocalWriteTime(s.local, ft) || CompareFileTime(&ft, &s.lastWrite) == 0) continue;
        // Datei noch in Benutzung (wird gerade geschrieben)?
        HANDLE h = CreateFileW(LongPath(s.local).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        CloseHandle(h);
        RemoteUrl u;
        if (!ParseRemoteUrl(s.url, u)) continue;
        std::wstring err;
        auto fs = Acquire(owner, u, err);
        bool ok = false;
        if (fs) {
            Job j;
            j.src.local = s.local;
            j.dst.remote = true;
            j.dst.fs = fs;
            j.dst.url = u;
            j.mtime = ft;
            ProgressSink sink;
            ok = CopyFileJob(j, sink, 0, 0, err);
        }
        s.lastWrite = ft;
        if (ok) {
            status = L"Hochgeladen: " + s.url;
            LogOperation(L"Bearbeitete Datei hochgeladen: " + s.url);
        } else {
            MsgError(owner, L"Die bearbeitete Datei konnte nicht zum Server übertragen werden:\n" + s.url + L"\n\n" + err +
                                L"\n\nDie lokale Kopie bleibt erhalten:\n" + s.local);
        }
    }
    busy = false;
    return status;
}

void RemoteDisconnectAll() {
    for (auto& p : Pool()) {
        std::unique_lock<std::recursive_mutex> lock(p.second->mutex(), std::try_to_lock);
        if (lock.owns_lock()) p.second->Disconnect();
    }
    Pool().clear();
}

} // namespace qf
