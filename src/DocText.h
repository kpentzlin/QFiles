#pragma once
// DocText.cpp – Text aus Office-Dokumenten für die Textsuche (Suchen, F3).
//
// Unterstützt werden die ZIP-basierten Formate: Word/Excel/PowerPoint ab 2007 (.docx, .xlsx, .pptx und Varianten),
// OpenDocument (.odt, .ods, .odp, .odg und Vorlagen), EPUB sowie PDF (PdfText.cpp). Die enthaltenen XML-Teile mit dem eigentlichen
// Inhalt werden entpackt (Deflate, eigene Implementierung nach RFC 1951) und von Tags befreit; Absätze, Zellen und
// Zeilenumbrüche werden zu Leerzeichen bzw. Zeilenwechseln.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace qf {

// Ist die Datei ein unterstütztes Dokumentformat (nach der Endung)?
bool IsDocumentContainer(const std::wstring& path);

// Liest den Text des Dokuments. false = kein gültiges Dokument (dann gewöhnlich durchsuchen).
bool ExtractDocumentText(const std::wstring& path, std::wstring& text, const std::atomic<bool>* stop = nullptr);

// Für Tests: Text aus einem Dokument im Speicher (Dateiname nur für die Formaterkennung)
bool ExtractDocumentTextFromMemory(const std::wstring& nameForType, const std::vector<uint8_t>& data, std::wstring& text);

// Deflate entpacken (roh, ohne zlib-Kopf). false bei Fehlern oder wenn mehr als maxOut Bytes entstehen würden.
bool InflateRaw(const uint8_t* data, size_t size, std::string& out, size_t maxOut);

} // namespace qf
