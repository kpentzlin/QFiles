// Automatische Tests für die Kernfunktionen von QFiles (laufen in GitHub Actions auf Windows).
// Geprüft werden Zeichenketten/Pfade, Kodierungen, Einstellungsdatei und Dateioperationen
// (IFileOperation: Kopieren, Verschieben, Umbenennen, Löschen, Rückgängig) – alles mit Unicode-Namen.

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <vector>

#include "App.h"
#include "Encoding.h"
#include "FileOps.h"
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

    Out(L"Bestanden: " + std::to_wstring(g_passed) + L", fehlgeschlagen: " + std::to_wstring(g_failed));
    OleUninitialize();
    return g_failed == 0 ? 0 : 1;
}
