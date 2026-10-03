#pragma once
// Befehls-IDs des Hauptfensters (Menü, Werkzeugleiste, Tastenkürzel).

namespace qf::cmd {

enum : int {
    // Datei
    Open = 40001,
    OpenWith,
    View,              // F3 – gemäß Option "Dateianzeige"
    ViewWindow,        // Umschalt+F3 – eigenes Anzeigefenster
    HexEdit,           // Alt+F3
    Edit,              // F4
    NewTextFile,       // Umschalt+F4
    NewFolder,         // F7
    Copy,              // F5
    Move,              // F6
    Rename,            // F2
    BatchRename,       // Strg+M
    Delete,            // Entf / F8
    DeletePermanent,   // Umschalt+Entf
    Properties,        // Alt+Eingabe
    Attributes,        // Strg+Umschalt+A
    SplitFile,
    JoinFiles,
    CreateZip,         // Alt+F5
    OpenArchive,       // Alt+F9
    PrintList,         // Strg+P
    Undo,              // Strg+Z
    Exit,

    // Bearbeiten
    ClipCut = 40100,
    ClipCopy,
    ClipPaste,
    SelectAll,
    SelectNone,
    InvertSelection,
    SelectGroup,
    DeselectGroup,
    SelectSameExt,
    CopyPaths,
    CopyNames,

    // Ansicht
    TwoPanes = 40200,
    SplitToggle,       // Strg+T – aktive Spalte teilen / Teilung aufheben
    ViewDetails,
    ViewList,
    ViewIcons,
    ViewThumbnails,
    SortName,
    SortExt,
    SortSize,
    SortDate,
    SortAttr,
    SortDescending,
    ShowHidden,
    Filter,
    QuickView,         // Strg+Q – Schnellansicht im jeweils anderen Fenster
    Refresh,           // Strg+R
    ToggleToolbar,
    ToggleFKeyBar,
    ToggleCommandLine,
    ToggleStatusBar,

    // Gehe zu
    GoBack = 40300,
    GoForward,
    GoUp,
    GoRoot,
    GoPath,            // Strg+G – Verzeichnis eingeben
    GoOtherSame,       // Gegenseite auf dasselbe Verzeichnis
    SwapPanes,         // Strg+U
    NextPane,          // Tab
    FocusPane1,        // Strg+1 … Strg+4
    FocusPane2,
    FocusPane3,
    FocusPane4,
    FocusTree,
    FocusBookmarks,
    FocusCommandLine,  // Strg+E
    FocusPath,         // Strg+L
    CmdLineInsertName, // Strg+Eingabe – Name in die Befehlszeile
    GoSpecialFirst = 40350,   // Spezialverzeichnisse
    GoSpecialLast = 40379,

    // Lesezeichen
    BookmarkAdd = 40400,
    BookmarkFirst = 40410,    // dynamische Einträge
    BookmarkLast = 40499,

    // Werkzeuge
    FindFiles = 40500,
    FindDuplicates,
    CompareFiles,
    CompareDirs,
    SyncDirs,
    ClearCompareMarks,
    DirSizes,
    DriveOverview,
    OpLog,
    CommandPrompt,
    FunctionKeys,
    Options,

    // Hilfe
    Shortcuts = 40600,
    About,

    // Funktionstasten (24)
    FKeyFirst = 40700,
    FKeyLast = 40723,

    // Interne Steuerelement-IDs
    IdTree = 1001,
    IdBookmarks,
    IdToolbar,
    IdStatus,
    IdCommandLine,
    IdFKeyBar,
    IdPaneBase = 1100,        // + Index*100
};

} // namespace qf::cmd
