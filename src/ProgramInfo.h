#pragma once
// ProgramInfo.cpp – Eigenschaften von Programmdateien (.exe, .dll) für die Dateianzeige (F11):
// Datei, Versionsressource (alle Einträge), PE-Kopf (Prozessor, Subsystem, Sicherheitsmerkmale, .NET, Abschnitte,
// Importe, Exporte, Ressourcen, Manifest), digitale Signatur und die Windows-Eigenschaften (wie im Explorer).

#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

namespace qf {

struct ProgramProperty {
    std::wstring label, value;
    bool heading = false;   // Abschnittsüberschrift (nur label)
};

// .exe/.dll (nur nach Endung)
bool IsProgramFile(const std::wstring& path);

// Liest alle Eigenschaften (darf in einem Hintergrund-Thread laufen; initialisiert COM selbst).
std::vector<ProgramProperty> ReadProgramProperties(const std::wstring& path, const std::atomic<bool>* cancel = nullptr);

// Erstes Programmsymbol der Datei in der gewünschten Größe; nullptr, wenn die Datei keines enthält.
HICON LoadProgramIcon(const std::wstring& path, int size);

// Zeilenweise Textfassung (für die Zwischenablage)
std::wstring ProgramPropertiesText(const std::wstring& path, const std::vector<ProgramProperty>& props);

} // namespace qf
