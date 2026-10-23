#pragma once
// Thumbnail.cpp – Miniaturen für die Kachelansicht der Listen.
//
// Reihenfolge: Miniatur der Shell (Explorer-Cache, Miniaturanbieter) – sonst selbst über WIC dekodiert (Bildformate,
// deren Codec installiert ist) – bei HEIC/HEIF ohne HEVC-Decoder das in den EXIF-Daten eingebettete JPEG-Vorschaubild –
// zuletzt das Symbol des Dateityps.

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace qf {

// Im Arbeitsthread aufrufen (COM muss initialisiert sein). how (optional): „Shell“, „WIC“, „EXIF“ oder „Symbol“.
HBITMAP CreateThumbnail(const std::wstring& path, int size, std::wstring* how = nullptr);

// Für Tests: eingebettetes EXIF-Vorschaubild (JPEG) einer HEIF-Datei im Speicher
bool HeifExifThumbnail(const std::vector<uint8_t>& file, std::vector<uint8_t>& jpeg);

} // namespace qf
