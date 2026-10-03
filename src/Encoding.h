#pragma once
// Textkodierungen: Erkennung, Dekodierung, Kodierung, Zeilenenden.
// Unterstützt: UTF-8 (mit/ohne BOM), UTF-16 LE/BE (mit BOM), ANSI (Systemcodepage), OEM (Konsolencodepage).

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#include "Settings.h"

namespace qf {

const wchar_t* EncodingName(TextEncoding e);   // "UTF-8", "UTF-8 mit BOM", …
const wchar_t* LineEndingName(LineEnding e);   // "CRLF (Windows)", …

// Erkennt die Kodierung. bomLength erhält die Länge einer vorhandenen BOM.
// Ohne BOM: gültiges UTF-8 -> Utf8, UTF-16 ohne BOM (viele Nullbytes an ungeraden/geraden Stellen) -> Utf16LE/BE,
// sonst Ansi. Leere Daten -> defaultForEmpty.
TextEncoding DetectEncoding(const uint8_t* data, size_t size, size_t* bomLength = nullptr,
                            TextEncoding defaultForEmpty = TextEncoding::Utf8);
bool IsValidUtf8(const uint8_t* data, size_t size);
// Heuristik: enthält die Datei Binärdaten? (Nullbytes außerhalb von UTF-16, Steuerzeichen)
bool LooksBinary(const uint8_t* data, size_t size);

// Dekodiert (eine vorhandene BOM wird übersprungen).
std::wstring DecodeText(const uint8_t* data, size_t size, TextEncoding enc);
// Kodiert inkl. BOM (für Utf8Bom, Utf16LE, Utf16BE). unmappable wird true, wenn Zeichen verloren gehen (ANSI/OEM).
std::vector<uint8_t> EncodeText(const std::wstring& text, TextEncoding enc, bool* unmappable = nullptr);

LineEnding DetectLineEnding(const std::wstring& text, LineEnding def = LineEnding::CRLF);
// Wandelt alle Zeilenenden (CRLF, LF, CR) in das gewünschte um.
std::wstring ConvertLineEndings(const std::wstring& text, LineEnding eol);

struct TextFile {
    std::wstring text;           // Inhalt mit Original-Zeilenenden
    TextEncoding encoding = TextEncoding::Utf8;
    LineEnding lineEnding = LineEnding::CRLF;
    bool wasEmpty = false;       // leere Datei -> Standardkodierung (UTF-8 ohne BOM)
};
// Lädt eine Textdatei. forced != nullptr erzwingt eine Kodierung.
bool LoadTextFile(const std::wstring& path, TextFile& out, const TextEncoding* forced = nullptr);
// Speichert text (Zeilenenden werden in eol umgewandelt).
bool SaveTextFile(const std::wstring& path, const std::wstring& text, TextEncoding enc, LineEnding eol,
                  bool* unmappable = nullptr);

} // namespace qf
