#pragma once
// PdfText.cpp – Text aus PDF-Dateien für die Textsuche (Suchen, F3).
//
// Liest alle Objekte (auch aus Objektströmen), entpackt die Inhaltsströme (Flate, LZW, ASCIIHex, ASCII85), wertet die
// Textoperatoren aus und übersetzt die Zeichencodes über ToUnicode-Tabellen bzw. die Kodierung der Schrift
// (WinAnsi, MacRoman, Standard, Differences). Mit leerem Benutzerkennwort verschlüsselte PDFs (RC4, AES-128,
// AES-256) werden entschlüsselt. Bilder (gescannte Seiten) enthalten keinen Text.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace qf {

// false = keine lesbare PDF-Datei (dann wird die Datei wie jede andere durchsucht)
bool ExtractPdfText(const uint8_t* data, size_t size, std::wstring& text, const std::atomic<bool>* stop = nullptr);

} // namespace qf
