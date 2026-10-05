// Automatische Tests für die Kernfunktionen von QFiles (laufen in GitHub Actions auf Windows).
// Geprüft werden Zeichenketten/Pfade, Kodierungen, Einstellungsdatei und Dateioperationen
// (IFileOperation: Kopieren, Verschieben, Umbenennen, Löschen, Rückgängig) – alles mit Unicode-Namen.
// Netzwerk-/FTP-Adressen; FTP/SFTP-Clients gegen Testserver, wenn die Umgebungsvariablen gesetzt sind:
//   QFILES_TEST_FTP / QFILES_TEST_SFTP        = Adresse eines beschreibbaren Testkontos (z. B. ftp://test@127.0.0.1:2121/)
//   QFILES_TEST_FTP_PASS / QFILES_TEST_SFTP_PASS = Kennwort
//   QFILES_TEST_FTP_RO / QFILES_TEST_SFTP_RO  = nur lesbarer Server (z. B. sftp://demo@test.rebex.net/), Kennwort in *_RO_PASS

#include "RemoteFs.h"   // winsock2.h vor windows.h
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <vector>

#include "App.h"
#include "Encoding.h"
#include "FileOps.h"
#include "Location.h"
#include "Settings.h"
#include "Util.h"

using namespace qf;

static int g_failed = 0;
static int g_passed = 0;

static void Out(const std::wstring& s) {
    std::string u = WideToUtf8(s + L"\n");
    DWORD w = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u.data(), (DWORD)u.size(), &w, nullptr);
}

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            ++g_passed;                                                                   \
        } else {                                                                          \
            ++g_failed;                                                                   \
            Out(L"FEHLGESCHLAGEN: " L## #cond L"  (Zeile " + std::to_wstring(__LINE__) + L")"); \
        }                                                                                 \
    } while (0)

static std::wstring ReadAll(const std::wstring& p) {
    std::vector<uint8_t> b;
    ReadFileBytes(p, b);
    return DecodeText(b.data(), b.size(), DetectEncoding(b.data(), b.size()));
}

static void TestStrings() {
    CHECK(EqualsI(L"ÄÖÜ", L"äöü"));
    CHECK(WildcardMatch(L"*.txt", L"Datei €.TXT"));
    CHECK(!WildcardMatch(L"*.txt", L"Datei.txt.bak"));
    CHECK(MatchAnyPattern(L"*.doc;*.txt", L"a.txt"));
    CHECK(PathParent(L"C:\\a\\b") == L"C:\\a");
    CHECK(PathParent(L"C:\\a") == L"C:\\");
    CHECK(PathParent(L"C:\\").empty());
    CHECK(PathFileName(L"C:\\x\\日本語.txt") == L"日本語.txt");
    CHECK(PathExtension(L"C:\\x\\a.b.c") == L".c");
    CHECK(LastPathElement(L"C:\\") == L"C:");
    CHECK(LastPathElement(L"D:\\Projekte\\QFiles") == L"QFiles");
    CHECK(PathParent(L"\\\\server\\share\\dir") == L"\\\\server\\share");
    CHECK(IsRootPath(L"\\\\server\\share"));
    CHECK(Utf8ToWide(WideToUtf8(L"Grüße 😀 ✓")) == L"Grüße 😀 ✓");
    CHECK(CanonicalPath(L"C:\\") == L"C:\\");
}

static void TestEncoding(const std::wstring& dir) {
    // Leere Datei: Standard UTF-8 ohne BOM
    uint8_t dummy = 0;
    CHECK(DetectEncoding(&dummy, 0) == TextEncoding::Utf8);
    std::wstring text = L"Zeile 1 äöü\r\nZeile 2 日本語 😀\r\n";
    for (int e = 0; e <= 3; ++e) {
        TextEncoding enc = (TextEncoding)e;
        std::wstring p = PathCombine(dir, L"kodierung_" + std::to_wstring(e) + L".txt");
        CHECK(SaveTextFile(p, text, enc, LineEnding::CRLF));
        TextFile tf;
        CHECK(LoadTextFile(p, tf));
        CHECK(tf.encoding == enc);
        CHECK(tf.text == text);
        CHECK(tf.lineEnding == LineEnding::CRLF);
    }
    // UTF-8 ohne BOM: erste Bytes dürfen nicht EF BB BF sein
    std::wstring p = PathCombine(dir, L"kodierung_0.txt");
    std::vector<uint8_t> b;
    ReadFileBytes(p, b);
    CHECK(b.size() >= 3 && !(b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF));
    CHECK(ConvertLineEndings(L"a\r\nb\nc\rd", LineEnding::LF) == L"a\nb\nc\nd");
    // Leere Datei laden -> Standardkodierung aus den Optionen (UTF-8)
    std::wstring empty = PathCombine(dir, L"leer.txt");
    WriteFileBytes(empty, "", 0);
    TextFile tf;
    CHECK(LoadTextFile(empty, tf));
    CHECK(tf.wasEmpty && tf.encoding == TextEncoding::Utf8);
}

static void TestConfig(const std::wstring& dir) {
    Config c;
    c.SetFilePath(PathCombine(dir, L"Unterordner\\Test.ini"));
    c.Set(L"Fenster", L"Pfad", L"C:\\Benutzer\\Jürgen\\日本");
    c.Set(L"Fenster", L"Mehrzeilig", L"a\nb\\nc");
    c.SetInt(L"Fenster", L"Zahl", -42);
    c.SetList(L"Liste", L"Eintrag", {L"eins", L"zwei", L"drei"});
    CHECK(c.Save());
    Config d;
    CHECK(d.Load(c.FilePath()));
    CHECK(d.Get(L"Fenster", L"Pfad") == L"C:\\Benutzer\\Jürgen\\日本");
    CHECK(d.Get(L"Fenster", L"Mehrzeilig") == L"a\nb\\nc");
    CHECK(d.GetInt(L"Fenster", L"Zahl") == -42);
    CHECK(d.GetList(L"Liste", L"Eintrag").size() == 3);
    std::vector<Bookmark> bm = {{L"Projekt ✓", L"D:\\Projekte"}, {L"Temp", L"C:\\Temp"}};
    SaveBookmarks(d, bm);
    auto bm2 = LoadBookmarks(d);
    CHECK(bm2.size() == 2 && bm2[0].name == L"Projekt ✓" && bm2[1].path == L"C:\\Temp");
    // Standardpfad liegt in %ProgramData%\QFiles
    std::wstring def = DetermineConfigPath();
    CHECK(EndsWithI(def, L"\\QFiles\\QFiles.ini"));
    Out(L"Einstellungsdatei (Standard): " + def);
}

static void TestFileOps(const std::wstring& base) {
    SetFileOpsNonInteractive(true);
    std::wstring src = PathCombine(base, L"Quelle ✓ 日本");
    std::wstring dst = PathCombine(base, L"Ziel Ünïcödé");
    CreateDirectoryW(src.c_str(), nullptr);
    CreateDirectoryW(dst.c_str(), nullptr);
    std::wstring f1 = PathCombine(src, L"Datei €.txt");
    std::wstring f2 = PathCombine(src, L"😀 Emoji.txt");
    CHECK(SaveTextFile(f1, L"Inhalt 1", TextEncoding::Utf8, LineEnding::CRLF));
    CHECK(SaveTextFile(f2, L"Inhalt 2", TextEncoding::Utf8, LineEnding::CRLF));
    std::wstring sub = PathCombine(src, L"Unterverzeichnis");
    CHECK(CreateFolder(nullptr, src, L"Unterverzeichnis"));
    CHECK(DirExists(sub));

    // Kopieren
    CHECK(CopyItems(nullptr, {f1, f2, sub}, dst, OpSilent));
    CHECK(FileExists(PathCombine(dst, L"Datei €.txt")));
    CHECK(FileExists(PathCombine(dst, L"😀 Emoji.txt")));
    CHECK(DirExists(PathCombine(dst, L"Unterverzeichnis")));
    CHECK(ReadAll(PathCombine(dst, L"Datei €.txt")) == L"Inhalt 1");
    // Rückgängig: Kopien werden entfernt (Papierkorb)
    CHECK(CanUndo());
    CHECK(UndoLast(nullptr));
    CHECK(!FileExists(PathCombine(dst, L"Datei €.txt")));
    CHECK(!DirExists(PathCombine(dst, L"Unterverzeichnis")));
    CHECK(FileExists(f1));

    // Kopieren in dasselbe Verzeichnis -> "Name (2)"
    CHECK(CopyItems(nullptr, {f1}, src, OpSilent));
    CHECK(FileExists(PathCombine(src, L"Datei € (2).txt")));

    // Verschieben + Rückgängig
    CHECK(MoveItems(nullptr, {f2}, dst, OpSilent));
    CHECK(!FileExists(f2));
    CHECK(FileExists(PathCombine(dst, L"😀 Emoji.txt")));
    CHECK(UndoLast(nullptr));
    CHECK(FileExists(f2));
    CHECK(!FileExists(PathCombine(dst, L"😀 Emoji.txt")));

    // Umbenennen + Rückgängig
    CHECK(RenameItem(nullptr, f1, L"Umbenannt ß.txt"));
    std::wstring renamed = PathCombine(src, L"Umbenannt ß.txt");
    CHECK(FileExists(renamed) && !FileExists(f1));
    CHECK(UndoLast(nullptr));
    CHECK(FileExists(f1) && !FileExists(renamed));
    // Ungültiger Name wird abgelehnt
    CHECK(!RenameItem(nullptr, f1, L"a:b.txt"));

    // Gruppe (zwei Umbenennungen) als ein Schritt
    BeginUndoGroup(L"Gruppe");
    CHECK(RenameItem(nullptr, f1, L"g1.txt"));
    CHECK(RenameItem(nullptr, f2, L"g2.txt"));
    EndUndoGroup();
    CHECK(UndoDescription() == L"Gruppe");
    CHECK(UndoLast(nullptr));
    CHECK(FileExists(f1) && FileExists(f2));

    // Kopieraufträge mit Zielnamen (Synchronisieren)
    CHECK(CopyJobs(nullptr, {{f1, PathCombine(dst, L"neu\\tief"), L"kopie.txt"}}, OpSilent | OpNoConfirmOverwrite));
    CHECK(FileExists(PathCombine(dst, L"neu\\tief\\kopie.txt")));

    // Endgültig löschen
    CHECK(DeleteItems(nullptr, {PathCombine(dst, L"neu")}, false, false));
    CHECK(!DirExists(PathCombine(dst, L"neu")));

    // Neue leere Datei
    CHECK(CreateEmptyFile(nullptr, dst, L"Neue Textdatei.txt"));
    CHECK(GetFileSize64(PathCombine(dst, L"Neue Textdatei.txt")) == 0);

    // Lange Pfade (> 260 Zeichen)
    std::wstring longDir = dst;
    while (longDir.size() < 300) {
        longDir = PathCombine(longDir, L"Langer Verzeichnisname ✓");
        CreateDirectoryW(LongPath(longDir).c_str(), nullptr);
    }
    CHECK(DirExists(longDir));
    std::wstring longFile = PathCombine(longDir, L"datei.txt");
    CHECK(SaveTextFile(longFile, L"lang", TextEncoding::Utf8, LineEnding::CRLF));
    CHECK(FileExists(longFile));
    std::vector<DirEntry> entries;
    CHECK(ListDirectory(longDir, entries) && entries.size() == 1);
}

static void TestLocations() {
    CHECK(IsNetworkRoot(L"\\\\"));
    CHECK(IsNetworkRoot(L"Netzwerk"));
    CHECK(IsNetworkServer(L"\\\\fritz.box"));
    CHECK(IsNetworkServer(L"\\\\fritz.box\\"));
    CHECK(!IsNetworkServer(L"\\\\fritz.box\\FRITZ.NAS"));
    CHECK(!IsNetworkServer(L"\\\\?\\C:\\x"));
    CHECK(IsUncShareRoot(L"\\\\fritz.box\\FRITZ.NAS"));
    CHECK(LocationParent(L"\\\\fritz.box\\FRITZ.NAS") == L"\\\\fritz.box");
    CHECK(LocationParent(L"\\\\fritz.box") == L"\\\\");
    CHECK(LocationParent(L"\\\\").empty());
    CHECK(LocationParent(L"\\\\srv\\share\\dir") == L"\\\\srv\\share");
    CHECK(LocationCombine(L"\\\\", L"srv") == L"\\\\srv");
    CHECK(LocationCombine(L"\\\\srv", L"share") == L"\\\\srv\\share");
    CHECK(NormalizeSpecialLocation(L"Netzwerk") == L"\\\\");
    CHECK(NormalizeSpecialLocation(L"\\\\srv\\") == L"\\\\srv");
    CHECK(NormalizeSpecialLocation(L"C:\\x").empty());
    CHECK(LocationFileName(L"\\\\srv") == L"srv");
    CHECK(LocationDisplay(L"\\\\") == L"Netzwerk");
    RemoteUrl u;
    CHECK(ParseRemoteUrl(L"sftp://karl@nas.local:2222/home/karl/Ünïcödé ✓", u));
    CHECK(u.proto == RemoteProto::Sftp && u.user == L"karl" && u.host == L"nas.local" && u.port == 2222);
    CHECK(u.path == L"/home/karl/Ünïcödé ✓");
    CHECK(u.ServerKey() == L"sftp://karl@nas.local:2222");
    CHECK(ParseRemoteUrl(L"ftp://ftp.example.org", u) && u.path == L"/" && u.EffectivePort() == 21 && u.user.empty());
    CHECK(u.ToString() == L"ftp://ftp.example.org/");
    CHECK(ParseRemoteUrl(L"ftp://a%40b@[::1]:21/x//y/../z/", u) && u.user == L"a@b" && u.host == L"::1" && u.path == L"/x/z");
    CHECK(u.ToString() == L"ftp://a%40b@[::1]/x/z");
    CHECK(!ParseRemoteUrl(L"http://x/", u));
    CHECK(LocationParent(L"sftp://k@h/a/b") == L"sftp://k@h/a");
    CHECK(LocationParent(L"sftp://k@h/a") == L"sftp://k@h/");
    CHECK(LocationParent(L"sftp://k@h/").empty());
    CHECK(LocationCombine(L"sftp://k@h/", L"x y") == L"sftp://k@h/x y");
    CHECK(LocationCombine(L"sftp://k@h/a", L"b") == L"sftp://k@h/a/b");
    CHECK(LocationFileName(L"ftp://h/a/Grüße.txt") == L"Grüße.txt");
    CHECK(IsVirtualLocation(L"ftp://h/") && IsVirtualLocation(L"\\\\") && !IsVirtualLocation(L"\\\\srv\\share"));
    CHECK(NormalizeRemotePath(L"\\a\\.\\b\\..\\c") == L"/a/c");
    std::vector<unsigned char> raw;
    CHECK(remote::Base64Decode(remote::Base64Encode((const unsigned char*)"QFiles!", 7), raw) && raw.size() == 7 &&
          memcmp(raw.data(), "QFiles!", 7) == 0);
    CHECK(remote::FileTimeToUnixTime(remote::UnixTimeToFileTime(1700000000)) == 1700000000);
}

static std::wstring EnvStr(const wchar_t* name) {
    wchar_t buf[2048] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n && n < 2048 ? std::wstring(buf, n) : L"";
}

// Ablauf gegen einen FTP/SFTP-Server. writable: Verzeichnis anlegen, hochladen, umbenennen, löschen.
static void TestRemoteServer(const std::wstring& urlText, const std::wstring& password, bool writable, const std::wstring& base) {
    RemoteUrl url;
    if (!ParseRemoteUrl(urlText, url)) {
        CHECK(!"Testadresse ungültig");
        return;
    }
    Out(L"Server-Test: " + url.ToString() + (writable ? L" (schreibend)" : L" (lesend)"));
    auto fs = url.proto == RemoteProto::Sftp ? remote::CreateSftpFs() : remote::CreateFtpFs();
    remote::ConnectSettings cs;
    cs.url = url;
    cs.password = password;
    cs.knownHostsFile = base + L"\\known_hosts.txt";
    cs.timeoutMs = 20000;
    remote::ConnectPrompts prompts;
    int hostKeyPrompts = 0;
    prompts.confirmHostKey = [&](const std::wstring& host, const std::wstring& type, const std::wstring& fp, bool changed) {
        ++hostKeyPrompts;
        Out(L"  Hostschlüssel " + host + L": " + type + L" " + fp + (changed ? L" (GEÄNDERT)" : L""));
        return !changed;
    };
    std::wstring err;
    bool connected = fs->Connect(cs, &prompts, err);
    if (!connected) Out(L"  Verbindung fehlgeschlagen: " + err);
    if (!connected && !writable) {
        // Öffentlicher Testserver (nur lesend) nicht erreichbar: kein Fehler der Tests
        Out(L"  WARNUNG: öffentlicher Testserver nicht erreichbar – Test übersprungen.");
        return;
    }
    CHECK(connected);
    if (!connected) return;
    Out(L"  Startverzeichnis: " + fs->HomeDir());
    std::wstring dir = url.path == L"/" ? fs->HomeDir() : url.path;
    std::vector<DirEntry> entries;
    bool listed = fs->List(dir, entries, err);
    if (!listed) Out(L"  Liste fehlgeschlagen: " + err);
    CHECK(listed);
    Out(L"  " + std::to_wstring(entries.size()) + L" Einträge in " + dir);
    if (url.proto == RemoteProto::Sftp) {
        // Zweite Verbindung: Hostschlüssel ist jetzt bekannt -> keine Rückfrage
        auto fs2 = remote::CreateSftpFs();
        int before = hostKeyPrompts;
        bool c2 = fs2->Connect(cs, &prompts, err);
        if (!c2) Out(L"  Zweite Verbindung fehlgeschlagen: " + err);
        CHECK(c2);
        CHECK(hostKeyPrompts == before);
        fs2->Disconnect();
    }
    if (!writable) {
        // Erste Datei herunterladen
        for (auto& e : entries) {
            if (e.IsDir() || e.size > 1024 * 1024) continue;
            std::wstring local = base + L"\\dl-" + e.name;
            HANDLE h = CreateFileW(local.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            uint64_t got = 0;
            bool ok = fs->Download(NormalizeRemotePath(dir + L"/" + e.name), h, [&](uint64_t n) { got = n; return true; }, err);
            CloseHandle(h);
            if (!ok) Out(L"  Download fehlgeschlagen: " + err);
            CHECK(ok);
            CHECK(GetFileSize64(local) == e.size);
            Out(L"  heruntergeladen: " + e.name + L" (" + std::to_wstring(e.size) + L" Bytes)");
            break;
        }
        fs->Disconnect();
        return;
    }
    // Schreibend: Unicode-Verzeichnis und -Datei
    std::wstring tdir = NormalizeRemotePath(dir + L"/QFiles Test Ünïcödé ✓ " + std::to_wstring(GetTickCount64() % 100000));
    CHECK(fs->MakeDir(tdir, err));
    std::wstring content = L"Hallo FTP – äöü 日本語 😀\r\n";
    std::string data = WideToUtf8(content);
    for (int i = 0; i < 2000; ++i) data += "Zeile " + std::to_string(i) + "\r\n";   // > 64 KB (mehrere Blöcke)
    std::wstring local = base + L"\\upload Grüße.txt";
    CHECK(WriteFileBytes(local, data.data(), data.size()));
    HANDLE h = CreateFileW(local.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) Out(L"  Testdatei nicht lesbar: " + local + L": " + LastErrorMessage());
    std::wstring rfile = tdir + L"/Grüße 日本語 ✓.txt";
    bool up = fs->Upload(h, rfile, nullptr, err);
    CloseHandle(h);
    if (!up) Out(L"  Upload fehlgeschlagen: " + err);
    CHECK(up);
    FILETIME ft = remote::UnixTimeToFileTime(1600000000);
    fs->SetModTime(rfile, ft);
    std::vector<DirEntry> list;
    CHECK(fs->List(tdir, list, err));
    bool found = false;
    for (auto& e : list)
        if (e.name == L"Grüße 日本語 ✓.txt") {
            found = true;
            CHECK(e.size == data.size());
            CHECK(!e.IsDir());
        }
    CHECK(found);
    DirEntry st;
    CHECK(fs->Stat(rfile, st) && st.size == data.size());
    std::wstring local2 = base + L"\\download.txt";
    h = CreateFileW(local2.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(fs->Download(rfile, h, nullptr, err));
    CloseHandle(h);
    std::vector<uint8_t> back;
    ReadFileBytes(local2, back);
    CHECK(std::string(back.begin(), back.end()) == data);
    std::wstring renamed = tdir + L"/umbenannt €.txt";
    CHECK(fs->Rename(rfile, renamed, err));
    CHECK(!fs->Stat(rfile, st));
    CHECK(fs->Stat(renamed, st));
    CHECK(fs->MakeDir(tdir + L"/sub", err));
    CHECK(!fs->RemoveDir(tdir, err));   // nicht leer
    CHECK(fs->DeleteFile(renamed, err));
    CHECK(fs->RemoveDir(tdir + L"/sub", err));
    CHECK(fs->RemoveDir(tdir, err));
    CHECK(!fs->Stat(tdir, st));
    fs->Disconnect();
    Out(L"  Server-Test beendet.");
}

static void TestRemote(const std::wstring& base) {
    struct V {
        const wchar_t* url;
        const wchar_t* pass;
        bool writable;
    } vars[] = {{L"QFILES_TEST_FTP", L"QFILES_TEST_FTP_PASS", true},
                {L"QFILES_TEST_SFTP", L"QFILES_TEST_SFTP_PASS", true},
                {L"QFILES_TEST_FTP_RO", L"QFILES_TEST_FTP_RO_PASS", false},
                {L"QFILES_TEST_SFTP_RO", L"QFILES_TEST_SFTP_RO_PASS", false}};
    for (auto& v : vars) {
        std::wstring url = EnvStr(v.url);
        if (!url.empty()) TestRemoteServer(url, EnvStr(v.pass), v.writable, base);
    }
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    OleInitialize(nullptr);
    App::SetInstance(GetModuleHandleW(nullptr));
    std::wstring base = PathCombine(GetTempDir(), L"QFilesTest ✓ " + std::to_wstring(GetTickCount64()));
    CreateDirectoryW(base.c_str(), nullptr);
    Out(L"Testverzeichnis: " + base);

    TestStrings();
    TestEncoding(base);
    TestConfig(base);
    TestFileOps(base);
    TestLocations();
    TestRemote(base);

    Out(L"Bestanden: " + std::to_wstring(g_passed) + L", fehlgeschlagen: " + std::to_wstring(g_failed));
    OleUninitialize();
    return g_failed == 0 ? 0 : 1;
}
