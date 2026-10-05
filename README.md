# QFiles

QFiles ist ein Dateimanager für Windows (64 Bit), nachempfunden **Idoswin Pro** (<https://www.idoswin.de/>).
Er ist in C++20 mit dem reinen Win32-API geschrieben: kein MFC, kein Qt, keine Fremdbibliotheken, eine einzige
`QFiles.exe` ohne Installation.

QFiles ist durchgehend Unicode-fähig. Dateinamen mit beliebigen Zeichen (Umlaute, Akzente, CJK, Emoji,
Surrogatpaare) werden korrekt angezeigt, umbenannt, kopiert und durchsucht. Lange Pfade über 260 Zeichen werden
ebenfalls unterstützt.

![QFiles unter Windows: Liste 1 links, rechts geteilt (Split) in Liste 2 mit Dateianzeige und Liste 4](docs/screenshot.png)

*Bildschirmfoto aus dem automatischen Starttest unter Windows Server 2025: links Verzeichnisbaum und Lesezeichen, Liste 1 mit Unicode-Verzeichnisnamen, rechts geteilte Spalte – oben die Dateianzeige der in Liste 1 gewählten Datei, unten Liste 4.*

---

## Abweichungen von Idoswin Pro

| Bereich | Umsetzung in QFiles |
|---|---|
| Name, Symbol | „QFiles“ mit eigenem Programmsymbol (`res/QFiles.ico`) |
| Unicode | Vollständig Unicode (UTF-16 intern), insbesondere Anzeige und Bearbeitung von Dateinamen |
| Texteditor | Neue und **leere Dateien werden standardmäßig als UTF-8 ohne BOM** gespeichert; vorhandene Dateien behalten ihre Kodierung |
| Disketten | Alle diskettenbezogenen Funktionen entfallen (keine Diskettenlaufwerke A:/B:, kein Formatieren/Kopieren von Disketten, keine Diskettengrößen beim Teilen) |
| Zwei Dateilisten | Immer **senkrecht nebeneinander**, Verzeichnisbaum links |
| Dateianzeige | Standardeinstellung „**in dem jeweils anderen Fenster**“ (Schnellansicht ersetzt die Gegenseite) |
| Kopfzeile jeder Liste | Laufwerksymbole, rechtsbündig **Lesezeichen hinzufügen** und ganz rechts **Split** |
| Pfadzeile jeder Liste | Ganz rechts ein **Kreis**: grün gefüllt = aktive Liste, hohl = nicht aktiv; ein Klick darauf macht die Liste aktiv |
| Lesezeichen | Als **senkrechte Liste zwischen Verzeichnisbaum und Dateilisten**, immer sichtbar (nicht abwählbar), statt im Kopfzeilenbereich |
| Split | Teilt eine Liste waagerecht: unten erscheint Liste 3 (links) bzw. Liste 4 (rechts) mit gleichem Kopfbereich |
| Einstellungen | Standardmäßig in `%ProgramData%\QFiles\QFiles.ini` |

### Lesezeichen-Symbol in der Kopfzeile

Ein Klick auf das Lesezeichen-Symbol (rechts in der Kopfzeile jeder Liste, links neben dem Split-Symbol, auch mit
Strg+D) fügt das in
dieser Liste gewählte Verzeichnis der Lesezeichenliste hinzu:

- Es erscheint eine Abfrage nach dem Namen. Sie ist mit dem letzten Glied des Verzeichnisnamens vorbelegt
  (bei Laufwerken z. B. `D:`).
- Steht das Verzeichnis schon in der Lesezeichenliste, zeigt die Abfrage einen Hinweis mit dem vorhandenen Namen.
- „Abbrechen“ fügt nichts hinzu.
- Das neue Lesezeichen kommt ans **Ende** der Lesezeichenliste.

### Split-Funktion

- Das **Split-Symbol** ganz rechts in der Kopfzeile (oder Strg+T) teilt die Liste. Die untere Hälfte ist ein eigenes,
  vollwertiges Listenfenster: das „dritte“ (links) bzw. „vierte“ (rechts).
- Das untere Fenster hat denselben Kopfbereich: Laufwerksymbole, rechts Lesezeichen- und Split-Symbol, darunter das
  Verzeichnis-Textfeld mit Wahlsymbolen (Zurück, Vor, Übergeordnet, Verzeichnis wählen) und dem Aktiv-Kreis.
- Im oberen Fenster wird das Split-Symbol zum **Split-aufheben-Symbol**. Im unteren Fenster erscheint es nur so.
  Ein Klick in einem der beiden hebt die Teilung auf.
- Beide Spalten lassen sich unabhängig teilen. Bis zu vier Listen sind gleichzeitig sichtbar; die Teilerhöhe ist
  mit der Maus verschiebbar.

### Dateianzeige „in dem jeweils anderen Fenster“

F11 (oder Strg+Q) schaltet die Schnellansicht ein. Die jeweils andere Liste zeigt dann den Inhalt der Datei, auf
der in der aktiven Liste der Fokus steht. Die Anzeige folgt beim Blättern und lässt sich mit Esc beenden.

| Dateityp | Darstellung |
|---|---|
| Text | Kodierung wird automatisch erkannt |
| Bilder | über WIC |
| Binärdateien | hexadezimal |
| PDF, Office, HTML, RTF … | über die Windows-Vorschauhandler |
| Verzeichnisse | Übersicht |

Unter *Werkzeuge → Optionen → Dateianzeige* lässt sich stattdessen „in eigenem Fenster“ wählen.

---

## Funktionsumfang

### Oberfläche

- Verzeichnisbaum links. Er folgt der aktiven Liste; ein Klick öffnet das Verzeichnis in der aktiven Liste.
- Im Baum unten der aufklappbare Knoten **Netzwerk** (wie in Idoswin Pro), Netzlaufwerke mit ihrer Freigabe
  (z. B. `N:  \\fritz.box\FRITZ.NAS\media1`), Wechseldatenträger mit ihrer Bezeichnung.
- Lesezeichenliste daneben:
  - Ein Klick öffnet das Lesezeichen in der aktiven Liste, Umschalt+Eingabe in der anderen.
  - Netzwerkpfade (UNC-Pfade, Netzwerk, Rechner, Netzlaufwerke) erscheinen mit **hellroter**, FTP/SFTP-Zugänge mit
    **hellblauer** Hintergrundfarbe.
  - Vorgegeben ist das Lesezeichen **Netzwerk**: Es zeigt die oberste Netzwerkebene (siehe unten).
  - Umbenennen mit F2, Entfernen mit Entf, Verschieben mit Alt+↑/↓, Pfad über das Kontextmenü ändern.
  - Dateien lassen sich auf ein Lesezeichen ziehen.
- Eine oder zwei Dateilisten (Strg+Umschalt+L), jede teilbar (Split). Die aktive Liste ist farbig markiert.
- Je Liste:
  - Laufwerksymbole: Ein Klick springt zum zuletzt besuchten Verzeichnis des Laufwerks; ein Rechtsklick öffnet das
    Kontextmenü des Laufwerks.
  - Verzeichnis-Textfeld mit Verlauf und Autovervollständigung; Umgebungsvariablen wie `%USERPROFILE%` sind erlaubt.
  - Zurück/Vor, Übergeordnet, Verzeichnis wählen.
  - Statuszeile: Anzahl, Größe, Markierung, Filter, belegter und freier Speicher des Laufwerks bzw. der Freigabe.
- Ansichten: Details, Liste, Symbole, Miniaturansicht (über die Windows-Shell, Größe einstellbar).
- Dateidatum wahlweise sekundengenau; zusätzliche Spalte „Erstellt“ (Erstellungsdatum) neben „Geändert“
  (beides unter *Werkzeuge → Optionen → Anzeige*).
- Sortieren nach Name, Typ, Größe, Datum, Erstellungsdatum oder Attributen, auf- und absteigend. Namen werden natürlich sortiert,
  wie im Explorer.
- Versteckte und Systemdateien ein-/ausblendbar. Dateifilter je Liste mit Ein- und Ausschlussmustern.
- Automatische Aktualisierung bei Änderungen im Dateisystem.
- Werkzeugleiste.
- Befehlszeile: führt Befehle im aktuellen Verzeichnis in einem Konsolenfenster aus. `cd` und `X:` wechseln das
  Verzeichnis der Liste. Mit Verlauf; Dateien lassen sich hineinziehen.
- Funktionstastenleiste. Bei gedrückter Strg- bzw. Strg+Umschalt-Taste zeigt sie die Belegung der programmierbaren
  Tasten.
- Statusleiste.

### Dateioperationen

- Kopieren (Umschalt+F5) und Verschieben (Umschalt+F6) in die andere Liste.
- Umbenennen mit F2: immer der vollständige Umbenennungsdialog (auch für eine einzelne Datei). Direkt in der Liste
  umbenennen: langsamer zweiter Klick auf den Namen.
- Neues Verzeichnis (F8), neue leere Datei (F9), neue Textdatei mit Editor (Umschalt+F4).
- **Duplizieren** (F10): Namensvorschlag „Name - Kopie.ext“; der Knopf „Nummeriert“ setzt „Name (1).ext“ ein bzw.
  zählt hoch, wenn es den Namen schon gibt oder er bereits auf „(n)“ endet.
- Löschen in den Papierkorb (Entf) oder endgültig (Umschalt+Entf). In der Werkzeugleiste stehen dafür drei Symbole:
  Papierkorb (Löschen in den Papierkorb), Kreuz (endgültig löschen), Radiergummi (Radieren).
- **Radieren** (Alt+Entf): Der Inhalt wird mit kryptografisch zufälligen Daten überschrieben, die Datei auf Länge 0
  gekürzt, in einen Zufallsnamen umbenannt und gelöscht; Verzeichnisse rekursiv. Nicht rückgängig zu machen.
  Hinweis: Auf SSDs und bei Schattenkopien/Cloud-Synchronisierung kann der alte Inhalt physisch erhalten bleiben.
- Ausführung über die Windows-Shell (`IFileOperation`): Fortschrittsanzeige, Konfliktdialoge, Rechteerhöhung.
- **Rückgängig** (Strg+Z) für Kopieren, Verschieben, Umbenennen und Anlegen.
- Drag & Drop zwischen Listen, Baum, Lesezeichen und anderen Programmen. Rechte Maustaste fragt nach Kopieren oder
  Verschieben.
- Zwischenablage: Ausschneiden, Kopieren, Einfügen, kompatibel mit dem Explorer.
- Explorer-Kontextmenü, „Öffnen mit“, Eigenschaften (Alt+Eingabe).
- Attribute und Zeitstempel ändern, auch rekursiv. Das Datum lässt sich aus dem EXIF-Aufnahmedatum übernehmen.
- Verzeichnisgrößen berechnen (Leertaste auf einem Verzeichnis oder Alt+Umschalt+Eingabe für alle).

### Netzwerk

- Netzwerkpfade (`\\Server\Freigabe\…`) funktionieren überall wie lokale Verzeichnisse.
- **Netzwerk** (Baumknoten, Lesezeichen oder Eingabe `Netzwerk` bzw. `\\` im Verzeichnisfeld) zeigt die gefundenen
  Rechner: Rechner verbundener Netzlaufwerke, die klassische Netzwerkumgebung und die Netzwerkerkennung von Windows.
  Ein Rechner (`\\Server`) zeigt seine Freigaben, eine Freigabe ihren Inhalt; „..“ führt wieder nach oben.
- Die Suche läuft im Hintergrund, die Einträge erscheinen nach und nach. **Esc** bricht sie ab; angezeigt bleiben
  die bis dahin gefundenen Einträge. F5 sucht erneut.

### FTP und SFTP

- **Lesezeichen → Neuer FTP/sFTP-Zugriff…** legt einen Zugang als Lesezeichen an: Protokoll (SFTP oder FTP),
  Server, Port, Benutzer, Kennwort, Startverzeichnis (leer = Anmeldeverzeichnis des Servers) und für SFTP optional
  eine Schlüsseldatei. „Verbindung testen“ prüft die Angaben. Bearbeiten über das Kontextmenü der Lesezeichenliste
  („Pfad ändern…“).
- Adressen: `sftp://benutzer@server[:port]/pfad` bzw. `ftp://benutzer@server[:port]/pfad` – auch direkt im
  Verzeichnisfeld; FTP ohne Benutzer meldet sich anonym an.
- In der Liste: Verzeichnisse öffnen, Kopieren und Verschieben in die andere Liste (hoch- und herunterladen, auch
  zwischen zwei Servern, Verzeichnisse mit Inhalt, Zeitstempel bleiben erhalten), Löschen, Umbenennen (F2), neues
  Verzeichnis (F8), neue Datei (F9), Anzeigen (F11, auch in der Dateianzeige im anderen Fenster), Bearbeiten (F4) und
  Öffnen: Die Datei wird in ein temporäres Verzeichnis geladen; nach dem Speichern lädt QFiles sie automatisch wieder
  hoch.
- Ist ein FTP/SFTP-Verzeichnis gewählt, entfällt in der Kopfzeile der Liste das Lesezeichensymbol (Zugänge entstehen
  über den Menüpunkt; Strg+D öffnet den Zugangsdialog mit dem aktuellen Verzeichnis).
- Kennwörter werden nur gespeichert, wenn gewünscht – verschlüsselt mit der Windows-Datenschutz-API (DPAPI), nur für
  den angemeldeten Windows-Benutzer lesbar. Sonst fragt QFiles einmal je Sitzung.
- SFTP: Beim ersten Verbinden wird der Fingerabdruck des Hostschlüssels angezeigt und nach Bestätigung in
  `%ProgramData%\QFiles\QFiles-Hostschluessel.txt` gespeichert; ein geänderter Schlüssel wird deutlich gemeldet.
  Anmeldung mit Schlüsseldatei, SSH-Agent (Windows-OpenSSH-Agent, Pageant), Kennwort oder keyboard-interactive.
- FTP überträgt Kennwort und Daten unverschlüsselt (Hinweis in der Statuszeile); FTP-Dateinamen in UTF-8.

### Werkzeuge

| Werkzeug | Funktion |
|---|---|
| Dateigruppe umbenennen (Strg+M) | Masken mit Platzhaltern (siehe unten), Zähler, Suchen/Ersetzen (auch regulärer Ausdruck), Groß-/Kleinschreibung, Unterverzeichnisse, Live-Vorschau mit Konfliktprüfung; ein Rückgängig-Schritt |
| Dateien suchen (F3) | Name, Text in UTF-8/UTF-16/ANSI, Größe, Datum, Attribute; Ergebnisliste mit „Gehe zu“ |
| Doppelte Dateien | Gleicher Inhalt: Größe, dann Hash, dann byteweiser Vergleich |
| Dateien vergleichen (Strg+K) | Text zeilenweise nebeneinander mit Farbmarkierung und zeichengenauer Hervorhebung; Binärvergleich |
| Verzeichnisse vergleichen (F7) | Vergleicht die aktive Liste mit einer anderen (bei Split mit Auswahlfrage, z. B. links oben mit links unten oder rechts oben); markiert gleiche, unterschiedliche, neuere und fehlende Dateien farbig und wählt sie zum Kopieren aus |
| Verzeichnisse synchronisieren (Strg+Umschalt+Y) | In eine oder beide Richtungen, mit Vorschau, Filter, Spiegeln |
| Datei teilen / zusammenfügen | Mit CRC-Prüfsumme und Batchdatei zum Zusammenfügen ohne QFiles |
| Archive | ZIP anzeigen, entpacken und erstellen (Windows-Shell); 7z, RAR, TAR, CAB, ISO u. a. anzeigen und entpacken über das Windows-eigene `tar.exe` |
| Laufwerksübersicht | Belegung aller Laufwerke |
| Dateisystemmonitor | Protokoll aller von QFiles ausgeführten Dateioperationen |
| Verzeichnisliste | Drucken, als Textdatei speichern oder in die Zwischenablage, wahlweise mit Unterverzeichnissen |
| Spezielle Verzeichnisse | Desktop, Dokumente, Downloads, AppData, Startmenü u. a. |

### Programmierbare Funktionstasten

QFiles hat 24 programmierbare Funktionstasten: **Strg+F1 … F12** und **Strg+Umschalt+F1 … F12**. Belegt werden sie
unter *Werkzeuge → Funktionstasten belegen*. Eine Taste kann ein Programm, ein Dokument, eine URL oder einen
Konsolenbefehl starten.

Platzhalter in Parametern und Arbeitsverzeichnis:

| Platzhalter | Bedeutung |
|---|---|
| `%P` | aktuelles Verzeichnis |
| `%O` | Verzeichnis der Gegenseite |
| `%F` | Fokusdatei mit Pfad |
| `%N` | Name der Fokusdatei |
| `%E` | Name ohne Erweiterung |
| `%S` | markierte Dateien mit Pfad |
| `%M` | markierte Namen |
| `%L` | Listendatei (UTF-8) |
| `%%` | Prozentzeichen |

### Integrierter Texteditor (F4)

- RichEdit im Klartextmodus, beliebig viele Fenster.
- Kodierungen: UTF-8 (Standard für neue und leere Dateien, ohne BOM), UTF-8 mit BOM, UTF-16 LE/BE, ANSI, OEM.
- Zeilenenden CRLF, LF und CR; beim Speichern wählbar.
- Suchen, Ersetzen, Gehe zu Zeile.
- **Zeilen sortieren** (auf-/absteigend, ohne Groß-/Kleinschreibung, natürlich), doppelte Zeilen entfernen,
  Groß-/Kleinschreibung umwandeln, Tabulatoren und Leerzeichen umwandeln.
- Drucken.
- Meldet, wenn die Datei außerhalb des Editors geändert wurde.

### Anzeige und Hex-Editor

- Anzeigefenster (Umschalt+F11) für Text, Bilder (Zoom), Hex und Vorschauhandler; blättert durch die Dateien des
  Verzeichnisses.
- Hex-Editor (Alt+F11) für beliebig große Dateien. Er arbeitet im Überschreibmodus, hebt Änderungen hervor, hat
  Rückgängig und kann suchen (Hex/Text) sowie zu einem Offset springen.

### Platzhalter beim Umbenennen von Dateigruppen

| Platzhalter | Bedeutung |
|---|---|
| `[N]` | Name ohne Erweiterung |
| `[N3]` | 3. Zeichen |
| `[N2-5]` | Zeichen 2 bis 5 |
| `[N2-]` | ab dem 2. Zeichen |
| `[N-3]` | die letzten 3 Zeichen |
| `[N2,4]` | 4 Zeichen ab dem 2. |
| `[E]` | Erweiterung (Bereiche wie bei `[N]`) |
| `[C]` | Zähler (Start, Schritt, Stellen einstellbar) |
| `[D]` | Änderungsdatum JJJJMMTT |
| `[d]` | Änderungsdatum JJJJ-MM-TT |
| `[T]` | Uhrzeit hhmmss |
| `[t]` | Uhrzeit hh-mm-ss |
| `[P]` | Name des Elternverzeichnisses |
| `[[`, `]]` | eckige Klammern |

---

## Tastenkürzel (Auswahl)

Funktionstasten (auch in der Fußzeile anklickbar):

| Taste | Funktion |
|---|---|
| F2 | Umbenennen (Dialog; mehrere Dateien: Dateigruppe umbenennen) |
| F3 | Suchen |
| F4 | Bearbeiten |
| F5 | Aktualisieren (Verzeichnisse neu einlesen, auch bei abgeschalteter automatischer Aktualisierung) |
| F6 | Markierung umkehren |
| F7 | Vergleich (Verzeichnisse) |
| F8 | Neues Verzeichnis |
| F9 | Neue Datei (Umschalt+F4: neue Textdatei) |
| F10 | Duplizieren |
| F11 | Anzeigen |

Weitere:

| Taste | Funktion |
|---|---|
| Eingabe / Rücktaste | Öffnen / übergeordnetes Verzeichnis |
| Tab, Strg+1…4 | Nächste Liste, Liste 1…4 |
| Umschalt+F5 · Umschalt+F6 | Kopieren · Verschieben in die andere Liste |
| Entf · Umschalt+Entf · Alt+Entf | Löschen (Papierkorb) · endgültig löschen · Radieren |
| Umschalt+F11 · Alt+F11 | Anzeigefenster · Hex-Editor |
| Strg+T | Split / Split aufheben |
| Strg+Q | Dateianzeige im anderen Fenster |
| Strg+D | Gewähltes Verzeichnis den Lesezeichen hinzufügen |
| Strg+Z | Rückgängig |
| Strg+K · Strg+Umschalt+Y | Dateien vergleichen · Synchronisieren |
| Strg+L / Strg+G · Strg+E | Verzeichnis eingeben · Befehlszeile |
| Alt+F1 · Alt+F2 | Verzeichnisbaum · Lesezeichenliste |
| Leertaste / Einfg · Num+ / Num− / Num* | Markieren · Gruppe markieren / abwählen / umkehren |
| Umschalt+F10 | Kontextmenü |
| F1 | Vollständige Übersicht aller Tastenkürzel |

---|---|
| Eingabe / Rücktaste | Öffnen / übergeordnetes Verzeichnis |
| Tab, Strg+1…4 | Nächste Liste, Liste 1…4 |
| F2 · F3 · F4 | Umbenennen · Anzeigen · Bearbeiten |
| F5 · F6 · F7 · F8/Entf | Kopieren · Verschieben · Neues Verzeichnis · Löschen |
| Umschalt+F3 · Alt+F3 · Umschalt+F4 | Anzeigefenster · Hex-Editor · Neue Textdatei |
| Strg+T | Split / Split aufheben |
| Strg+Q | Dateianzeige im anderen Fenster |
| Strg+D | Gewähltes Verzeichnis den Lesezeichen hinzufügen |
| Strg+Z | Rückgängig |
| Strg+F · Strg+K · Strg+Umschalt+K | Suchen · Dateien vergleichen · Verzeichnisse vergleichen |
| Strg+L / Strg+G · Strg+E | Verzeichnis eingeben · Befehlszeile |
| Alt+F1 · Alt+F2 | Verzeichnisbaum · Lesezeichenliste |
| Leertaste / Einfg · Num+ / Num− / Num* | Markieren · Gruppe markieren / abwählen / umkehren |
| F1 | Vollständige Übersicht aller Tastenkürzel |

---

## Einstellungen

Gespeichert werden:

- alle Optionen,
- Fensterposition und -größe,
- Breiten von Baum und Lesezeichenliste,
- Teilungsverhältnisse und Split-Zustand,
- die zuletzt in den **vier Listenfenstern** angezeigten Verzeichnisse mit Ansicht, Sortierung, Spaltenbreiten,
  Filter und Verlauf,
- die Lesezeichen,
- die Funktionstasten,
- der Befehlszeilen-Verlauf.

Speicherort:

1. **Standard:** `%ProgramData%\QFiles\QFiles.ini` (z. B. `C:\ProgramData\QFiles\QFiles.ini`), Textdatei in UTF-8.
2. Portabel: Liegt eine `QFiles.ini` neben der `QFiles.exe`, wird diese verwendet.
3. Befehlszeile: `QFiles.exe /ini="D:\Pfad\eigene.ini"`.

Weitere Befehlszeilenargumente: `QFiles.exe [Verzeichnis Liste 1] [Verzeichnis Liste 2]`.

---

## Bauen

Voraussetzung: CMake ≥ 3.20 und Visual Studio 2022 (MSVC, x64) oder MinGW-w64 (gcc ≥ 13).

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\QFilesTests.exe
```

Cross-Build unter Linux mit MinGW-w64:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

GitHub Actions (`.github/workflows/build.yml`) baut bei jedem Push mit MSVC und MinGW. Auf Windows laufen dabei die
automatischen Tests (Kodierungen, Einstellungsdatei, Dateioperationen mit Unicode-Namen und langen Pfaden,
Netzwerk- und FTP-Adressen, FTP gegen einen lokalen Testserver, SFTP/FTP lesend gegen den öffentlichen Testserver
test.rebex.net) und ein Oberflächentest mit Bildschirmfotos (u. a. Freigaben von `\\localhost` und eine FTP-Liste). Wird auf GitHub ein Release veröffentlicht, hängt der Workflow `QFiles.exe` und `QFiles-x64.zip` an.

### Aufbau des Quelltexts

| Datei | Inhalt |
|---|---|
| `src/main.cpp`, `MainWindow.*` | Hauptfenster, Layout, Menüs, Befehle, Tastenkürzel, Befehlszeile, Funktionstastenleiste |
| `src/FilePane.*` | Listenfenster mit Kopfzeile (Laufwerke, Split, Lesezeichen), Pfadzeile, virtueller ListView, Schnellansicht |
| `src/DirTree.*`, `BookmarkList.*` | Verzeichnisbaum, Lesezeichenliste |
| `src/FileOps.*`, `ShellMenu.*` | Dateioperationen mit Rückgängig, Zwischenablage, Kontextmenü, Drag & Drop |
| `src/Location.*`, `Network.*` | Netzwerk- und FTP-Adressen, Suche nach Rechnern und Freigaben |
| `src/Remote.*`, `RemoteFs.h`, `FtpClient.cpp`, `SftpClient.cpp`, `RemoteCommon.cpp` | FTP/SFTP: Zugänge, Verbindungen, Übertragungen |
| `third_party/libssh2` | libssh2 1.11.1 (BSD-Lizenz) für SFTP, gebaut mit der Windows-Kryptografie |
| `src/Settings.*`, `App.*` | Einstellungsdatei, Optionen, globale Dienste, Dateisystemmonitor |
| `src/Util.*`, `Encoding.*`, `Dialog.*`, `Glyphs.*` | Hilfsfunktionen, Textkodierungen, Dialoge ohne Ressourcendatei, Symbole |
| `src/TextEditor.*` | Texteditor |
| `src/Viewer.cpp`, `HexView.*` | Anzeige, Hex-Editor |
| `src/Compare*.cpp`, `SyncDirs.cpp` | Datei- und Verzeichnisvergleich, Synchronisieren |
| `src/BatchRename.cpp`, `FindFiles.cpp`, `Duplicates.cpp`, `SplitJoin.cpp` | Umbenennen, Suchen, Doppelte, Teilen |
| `src/Tools.cpp`, `Attributes.cpp`, `FunctionKeys.cpp`, `Archive.cpp`, `PrintList.cpp`, `OptionsDialog.cpp` | Weitere Werkzeuge und Optionen |

---

## Absturzberichte

Stürzt QFiles ab, wird neben der Einstellungsdatei ein Bericht mit Aufrufstapel gespeichert
(`%ProgramData%\QFiles\QFiles-Absturz.txt`, dazu ein Minidump `QFiles-Absturz.dmp`). Wirft beim Kontextmenü eine
Shell-Erweiterung eines anderen Programms eine unbehandelte Ausnahme, wird das abgefangen: QFiles läuft weiter und
meldet Aufruf, betroffenen Menüeintrag, Ausnahmecode, bei C++-Ausnahmen Typ und Text sowie die Module auf dem
Aufrufstapel mit der vermutlich verursachenden DLL. Die vollständigen Angaben werden an
`%ProgramData%\QFiles\QFiles-Kontextmenue.txt` angehängt.

### Kontextmenü-Erweiterungen ausschließen

Unter *Werkzeuge → Optionen → Programme* („Kontextmenü-Erweiterungen nicht laden“, in der INI-Datei
`[Optionen] KontextmenueAusschluss=`) stehen Muster, durch `;` getrennt. Kontextmenü- und Drag-&-Drop-Erweiterungen,
deren Name, Beschreibung oder DLL-Pfad (in Langform, ohne Groß-/Kleinschreibung) eines der Muster enthält, lädt
QFiles nicht; ihre Einträge fehlen im Kontextmenü von QFiles. Dazu registriert QFiles nur innerhalb des eigenen
Prozesses eine leere Ersatz-Erweiterung für deren Klassen-IDs – die Registrierung und der Explorer bleiben
unverändert. Voreinstellung ist `ArchiCrypt` (die RAM-Disk-Erweiterung `URDShellExt.dll` von ArchiCrypt Ultimate
RAM-Disk wirft beim Zeichnen ihres Eintrags „RAM-Disk erstellen“ Ausnahmen). Verursacht eine ausgeschlossene
Erweiterung dennoch einen Fehler, wird er nur protokolliert, ohne Meldung, und ihr Eintrag für den Rest der Sitzung
ausgeblendet. Ein leeres Feld schaltet den Ausschluss ab.

## Bekannte Einschränkungen

- Der Hex-Editor arbeitet nur im Überschreibmodus (kein Einfügen/Löschen von Bytes).
- ACE-Archive werden nicht unterstützt. RAR, 7z und andere Formate lassen sich nur anzeigen und entpacken, und nur
  soweit `tar.exe` (ab Windows 10 1803) sie kennt. Dabei können Namen außerhalb der Systemcodepage in der
  Listenansicht verfälscht werden; „Alles entpacken“ ist davon nicht betroffen.
- FTP: nur passiver Modus, kein verschlüsseltes FTPS (dafür SFTP verwenden). SFTP-Server, die ausschließlich
  Ed25519-Hostschlüssel anbieten, werden nicht unterstützt (RSA- und ECDSA-Schlüssel, wie bei OpenSSH üblich, schon).
- Für FTP/SFTP-Verzeichnisse stehen nicht alle Werkzeuge zur Verfügung (z. B. Vergleichen, Synchronisieren, Suchen,
  Attribute, Drag & Drop, Rückgängig); QFiles meldet das.
- Welche Rechner unter „Netzwerk“ erscheinen, hängt von der Netzwerkerkennung von Windows ab.
- Die Oberfläche ist deutsch; eine Sprachumschaltung gibt es nicht.
