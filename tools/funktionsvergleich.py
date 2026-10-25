# Erzeugt „QFiles Funktionsvergleich.pdf“: QFiles im Vergleich mit Idoswin Pro und SpeedCommander.
# Aufruf: python tools/funktionsvergleich.py [Ausgabedatei]   (benötigt reportlab und die Schrift DejaVu Sans)
#
# Je Zeile: Funktion, QFiles, Idoswin Pro, SpeedCommander, Bemerkung, abweichend (True/False).
# Werte: "ja", "–" (nicht vorhanden bzw. in den Herstellerangaben nicht genannt), "teilw.", "?" (keine Angabe);
# ein Zusatz in Klammern ist erlaubt, z. B. "ja (Pro)".
# Farbe der Nummernspalte:
#   rot   – in QFiles nicht umgesetzt (QFiles-Wert beginnt mit „–“)
#   gelb  – in QFiles abweichend umgesetzt (abweichend = True)
#   grün  – nur in QFiles, in keinem der beiden anderen Programme („–“ bei beiden)
import datetime
import sys

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import Paragraph, SimpleDocTemplate, Spacer, Table, TableStyle, KeepTogether

QF_VERSION = "0.1.14"
ID_VERSION = "5.8 (Build 766, 2018)"
SC_VERSION = "22.30 (Juli 2026)"

A = True   # abweichend umgesetzt

DATA = [
    ("Allgemein", [
        ("64-Bit-Programm (x64)", "ja", "?", "ja", "", False),
        ("ARM64-Version", "–", "–", "ja", "", False),
        ("Durchgehend Unicode (Dateinamen mit beliebigen Zeichen)", "ja", "?", "ja", "", False),
        ("Lange Pfade über 260 Zeichen", "ja", "?", "?", "", False),
        ("Eine EXE ohne Installation, portabel (INI neben der EXE)", "ja", "–", "ja",
         "Idoswin: Installation (auch ohne Administratorrechte); SC: portabel vom USB-Stick", False),
        ("HiDPI-Unterstützung (Per-Monitor-DPI)", "ja", "–", "ja",
         "Idoswin nur über die Kompatibilitätsoption von Windows", False),
        ("Einstellungen exportieren/importieren", "ja", "–", "ja", "QFiles: INI ohne Kennwörter; SC: Backup & Restore", False),
        ("Einstellungsassistent beim ersten Start", "–", "–", "ja", "", False),
        ("Automatische Update-Suche", "–", "–", "ja", "", False),
        ("Hilfe", "teilw.", "ja", "ja", "QFiles: Tastenkürzel-Übersicht (F1) und README; keine Hilfedatei", A),
        ("Absturzbericht mit Aufrufstapel und Minidump", "ja", "–", "–", "SC: Protokollierung (Logging) nach Kategorien", False),
        ("Fehlerhafte Kontextmenü-Erweiterungen abfangen und ausschließen", "ja", "–", "–", "", False),
        ("Oberfläche mehrsprachig", "–", "–", "ja", "QFiles und Idoswin nur deutsch", False),
    ]),
    ("Oberfläche und Layout", [
        ("Verzeichnisbaum mit allen Laufwerken und Netzwerk-Knoten", "ja", "ja", "ja", "SC: Baum je Ordnerfenster einblendbar", False),
        ("Zwei Dateilisten nebeneinander", "ja", "ja", "ja", "", False),
        ("Eine oder zwei Dateilisten umschaltbar", "ja", "ja", "ja", "", False),
        ("Dateilisten wahlweise untereinander", "–", "?", "ja", "QFiles: bewusst immer nebeneinander", False),
        ("Bis zu vier Dateilisten gleichzeitig (Split)", "ja", "–", "–", "", False),
        ("Vordefinierte Fensteraufteilungen (Ansichtenmanager)", "teilw.", "ja", "ja",
         "QFiles: feste Anordnung, nur eine/zwei Listen und Split; Idoswin: fünf Ansichten (Explorer, Commander …)", A),
        ("Layouts speichern und umschalten", "–", "ja", "ja", "Idoswin: Ansicht je Windows-Benutzer", False),
        ("Registerkarten (Tabs) je Liste", "–", "–", "ja", "SC: Basisordner, Navigationssperre, Farben je Tab", False),
        ("Lesezeichenliste als ständig sichtbare Spalte", "ja", "teilw.", "teilw.",
         "Idoswin: Lesezeichen-Menü und -Symbolleiste; SC: Ordnerfavoriten-Menü", A),
        ("Lesezeichen in der anderen Liste öffnen", "ja", "?", "ja",
         "QFiles: Symbolspalte bzw. Umschalt+Eingabe; SC: Ordner für das inaktive Fenster", False),
        ("Lesezeichen farbig nach Art (Standard-, Cloud-, FTP-, Netzwerkordner)", "ja", "–", "–", "", False),
        ("Lesezeichen nach Art sortieren", "ja", "–", "–", "", False),
        ("Cloud-Speicher-Ordner erkennen und als Lesezeichen anlegen", "ja", "–", "–", "Dropbox, Google Drive, OneDrive (mehrere Konten)", False),
        ("Linux-Distributionen (WSL) als Lesezeichen unter „Netzwerk“", "ja", "–", "–", "neu in QFiles 0.1.10", False),
        ("Dateien auf ein Lesezeichen ziehen", "ja", "?", "teilw.", "SC: QuickDrop-Symbole als Ziel", False),
        ("Laufwerksleiste je Liste", "ja", "ja", "ja", "Idoswin: Option „Zwei Laufwerkssymbolleisten“", False),
        ("Laufwerkssymbole nach Art (lokal, subst, Netz, CD, extern, RAM-Disk)", "ja", "–", "–", "mit Tooltip (Art, Ziel, Bezeichnung)", False),
        ("Füllstandswarnung an den Laufwerkssymbolen", "–", "–", "ja", "", False),
        ("Verzeichnisfeld mit Verlauf, Autovervollständigung, Umgebungsvariablen", "ja", "ja", "ja", "", False),
        ("Nicht vorhandenes Verzeichnis über das Adressfeld anlegen", "–", "ja", "–", "", False),
        ("Werkzeugleiste", "ja", "ja", "ja", "", False),
        ("Werkzeugleiste anpassen", "ja", "?", "ja", "neu in QFiles 0.1.12: gut 50 Schaltflächen, Reihenfolge, Trennlinien", False),
        ("Menüs und Symbolleisten anpassen", "–", "ja", "ja", "Idoswin: elf frei platzierbare Leisten; SC: mit Symboleditor", False),
        ("Befehlszeile im aktuellen Verzeichnis (mit Verlauf, Dateien hineinziehen)", "ja", "ja", "ja", "", False),
        ("Integriertes Ausgabefenster der Konsole (cmd/PowerShell)", "–", "?", "ja", "QFiles: Befehle laufen in einem eigenen Konsolenfenster", False),
        ("Funktionstastenleiste (anklickbar)", "ja", "?", "?", "QFiles: zeigt bei Strg bzw. Strg+Umschalt die programmierbaren Tasten", False),
        ("Tastenkürzel-Übersicht, Klick auf eine Zeile löst die Taste aus", "ja", "–", "–", "", False),
        ("Tastenkürzel frei belegen", "–", "–", "ja", "SC: auch zweistufige Kürzel", False),
        ("Statusleiste mit belegtem und freiem Speicher", "ja", "ja", "ja", "", False),
        ("Infotips in der Liste", "teilw.", "ja", "?", "QFiles: Name, Datum, Größe; Idoswin: erweitert (PDF, MP3, Ordnergröße)", A),
        ("Nicht aktive Listen grau hinterlegen", "ja", "–", "–", "je Liste in einer anderen Graustufe", False),
        ("Gestreifte Liste (abwechselnde Zeilenfarben)", "ja", "?", "ja", "", False),
        ("Gitternetz-/Trennlinien in der Detailansicht", "ja", "?", "ja", "", False),
        ("Designs, dunkler Modus, Farbschemata", "–", "–", "ja", "", False),
        ("Minimieren in den Infobereich (Tray)", "–", "ja", "–", "", False),
        ("Andockbare Informationsfenster (Ordnerinfo, Laufwerksstatus)", "–", "–", "ja", "", False),
        ("Fenster- und Listenzustand laufend gespeichert (auch nach Absturz erhalten)", "ja", "–", "–", "", False),
    ]),
    ("Navigation", [
        ("Zurück/Vor je Liste", "ja", "ja", "ja", "SC: je Registerkarte", False),
        ("Verlaufsliste per Rechtsklick auf Zurück/Vor, auch nach Neustart", "ja", "?", "?", "neu in QFiles 0.1.8: gespeichert", False),
        ("Maustasten „Zurück“/„Vor“ (5-Tasten-Maus)", "ja", "?", "?", "neu in QFiles 0.1.7", False),
        ("Verlauf der Verzeichnisse bleibt nach Neustart erhalten", "ja", "ja", "ja", "QFiles: Verlauf des Verzeichnisfelds", False),
        ("Übergeordnetes Verzeichnis, Stammverzeichnis („\\“)", "ja", "ja", "ja", "", False),
        ("Klick auf Laufwerk springt zum zuletzt besuchten Verzeichnis", "ja", "?", "?", "", False),
        ("Spezielle Verzeichnisse anspringen (Desktop, Dokumente, Downloads …)", "ja", "ja", "ja", "Idoswin: Menü „Springen“", False),
        ("Shell-Namensraum (Dieser PC, Papierkorb, virtuelle Ordner)", "teilw.", "teilw.", "ja",
         "QFiles und Idoswin: Papierkorb; QFiles (neu in 0.1.11): eigene Ansicht mit ursprünglichem Ort und Löschdatum, "
         "Wiederherstellen, endgültig löschen, leeren", A),
        ("Löschen in den Papierkorb rückgängig machen (Strg+Z)", "ja", "?", "?", "neu in QFiles 0.1.11", False),
        ("Smartphones, Tablets, Kameras (MTP)", "–", "–", "ja", "", False),
        ("Netzwerkübersicht (Rechner, Freigaben)", "ja", "ja", "ja", "QFiles: Suche im Hintergrund, abbrechbar", False),
        ("Netzlaufwerke verbinden/trennen", "–", "–", "ja", "", False),
        ("Zum Ziel einer Verknüpfung wechseln (.lnk, Junction, Symlink)", "–", "–", "ja", "", False),
        ("Schnellsuche durch Tippen in der Liste", "ja", "?", "ja", "", False),
        ("Verzeichnis eingeben bzw. auswählen (Dialog)", "ja", "ja", "ja", "", False),
        ("Privater Modus (kein Verlauf)", "ja", "–", "ja",
         "neu in QFiles 0.1.8: Schaltfläche „P“, Kopfzeilen grün; beim Ausschalten gilt wieder der Verlauf vom Einschalten", False),
        ("Verlauf löschen", "ja", "?", "?", "neu in QFiles 0.1.8", False),
        ("Als Explorer-Ersatz einrichten (Win+E), AutoPlay", "–", "–", "ja", "", False),
    ]),
    ("Dateilisten und Ansichten", [
        ("Detailansicht", "ja", "ja", "ja", "", False),
        ("Ansichten Liste und Symbole", "ja", "ja", "ja", "", False),
        ("Miniaturansicht", "ja", "ja", "ja",
         "QFiles: Windows-Shell, sonst WIC bzw. EXIF-Vorschau (HEIC); Vorausladen im Hintergrund (neu in 0.1.12); "
         "Idoswin: eigene Miniaturen für Bilder, Text, PDF", False),
        ("Ansicht „Mit Unterverzeichnissen“ (alle Dateien mit Teilpfad)", "ja", "–", "?", "", False),
        ("Spalten frei wählen (Explorer- und Metadatenspalten)", "teilw.", "ja", "ja",
         "QFiles: nur Typ, Attribute, Erstellt zuschaltbar", A),
        ("Metadaten-Spalten (Bildgröße, Dauer, Bitrate …)", "–", "ja", "ja", "", False),
        ("Sortieren nach Name, Typ, Größe, Datum, Attributen", "ja", "ja", "ja", "QFiles: natürliche Sortierung wie der Explorer", False),
        ("Mehrspaltige Sortierung", "–", "–", "ja", "", False),
        ("Gruppierte Anzeige", "–", "–", "ja", "", False),
        ("Verzeichnisse zuerst oder gemischt", "ja", "ja", "ja", "", False),
        ("Spalte „Erstellt“, Datum sekundengenau", "ja", "teilw.", "ja", "Idoswin: Erstellungsdatum bei Verzeichnissen", False),
        ("Relative Datumsanzeige (gestern, heute …)", "–", "–", "ja", "", False),
        ("Verzeichnisgrößen berechnen", "ja", "ja", "ja", "", False),
        ("Verzeichnisgrößen automatisch im Hintergrund", "ja", "–", "ja", "", False),
        ("Versteckte und Systemdateien ein-/ausblenden", "ja", "?", "ja", "", False),
        ("Versteckte und Systemdateien farbig (blau, rot, beides lila)", "ja", "?", "?", "neu in QFiles 0.1.11", False),
        ("Dateierweiterung ausblenden", "–", "ja", "?", "QFiles: eigene Spalte „Typ“", False),
        ("Dateifilter je Liste (Ein-/Ausschlussmuster)", "ja", "ja", "ja", "", False),
        ("Filter nach Größe, Alter, Attributen", "–", "ja", "ja", "", False),
        ("Vordefinierte und gespeicherte Filter", "–", "ja", "ja", "", False),
        ("Schnellfilter beim Tippen", "–", "–", "ja", "", False),
        ("Farbregeln für Dateien", "–", "–", "ja", "", False),
        ("Tags für Dateien und Ordner", "–", "–", "ja (Pro)", "", False),
        ("Ansichtseinstellung je Ordner automatisch", "–", "–", "ja", "", False),
        ("Overlay-Symbole (Cloud-Status, Versionsverwaltung)", "–", "ja", "ja", "", False),
        ("Ausgeschnittene Dateien grau, bis sie eingefügt sind", "ja", "?", "?", "", False),
        ("Automatische Aktualisierung bei Änderungen im Dateisystem", "ja", "?", "ja", "", False),
        ("Markierung und Position bleiben beim Aktualisieren erhalten", "ja", "?", "ja", "", False),
    ]),
    ("Markieren", [
        ("Markieren mit Leertaste/Einfg und Maus", "ja", "ja", "ja", "", False),
        ("Markieren durch Ziehen über die Dateien (Liste/Details)", "ja", "ja", "ja",
         "neu in QFiles 0.1.14: wie Idoswin Pro; SC: Auswahlrahmen", False),
        ("Mittlere Maustaste: Verzeichnis der Datei öffnen (Favoriten, mit Unterverzeichnissen)", "ja", "?", "?",
         "neu in QFiles 0.1.14; Datei wird dort markiert", False),
        ("Gruppe markieren/abwählen/umkehren (Muster)", "ja", "ja", "ja", "", False),
        ("Markieren über Verzeichnisvergleich", "ja", "ja", "ja", "", False),
        ("Auswahl speichern und wiederherstellen", "–", "–", "ja", "", False),
        ("Auswahl der Gegenseite übernehmen", "–", "–", "ja", "", False),
        ("Umbenannte Dateien bleiben markiert", "ja", "?", "ja", "neu in QFiles 0.1.7", False),
    ]),
    ("Dateioperationen", [
        ("Kopieren und Verschieben in die andere Liste", "ja", "ja", "ja", "", False),
        ("Fortschritt, Konfliktdialoge, Rechteerhöhung (UAC)", "ja", "?", "ja", "QFiles: über die Windows-Shell (IFileOperation)", False),
        ("Hintergrund-Warteschlange mit Pause und Priorität", "–", "–", "ja", "", False),
        ("Löschen in den Papierkorb oder endgültig", "ja", "ja", "ja", "", False),
        ("Sicheres Löschen (Radieren/Vernichten)", "ja", "ja (Pro)", "ja",
         "QFiles: 1 bis 5 Durchgänge mit Zufallsdaten (neu in 0.1.9)", False),
        ("Umbenennen direkt in der Liste", "ja", "ja", "ja", "QFiles: langsamer zweiter Klick; F2 öffnet den Dialog", False),
        ("Neues Verzeichnis (auch mehrstufig), neue Datei, neue Textdatei", "ja", "ja", "ja", "", False),
        ("Neu aus Dokumentvorlagen", "teilw.", "ja", "?", "QFiles: über das Explorer-Kontextmenü „Neu“", A),
        ("Duplizieren mit Namensvorschlag", "ja", "ja", "?", "", False),
        ("Namenskonflikt beim Kopieren/Verschieben: mit neuem Namen ablegen", "ja", "?", "?",
         "QFiles: einzelne Datei, vorbelegt „Name (1)“ (neu in 0.1.9)", False),
        ("Zwischenablage, kompatibel mit dem Explorer", "ja", "ja", "ja", "", False),
        ("Drag & Drop (Listen, Baum, Lesezeichen, andere Programme)", "ja", "ja", "ja", "", False),
        ("Ziehen in eine Liste: Ziel nach Spalte, Rückfrage im Leerraum", "ja", "–", "–", "", False),
        ("Rückgängig für Kopieren, Verschieben, Umbenennen, Anlegen", "ja", "ja", "teilw.", "SC: nur Mehrfaches Umbenennen", False),
        ("Attribute ändern, auch rekursiv", "ja", "ja", "ja", "", False),
        ("Zeitstempel ändern (Erstellt, Geändert, Zugriff), rekursiv", "ja", "ja", "ja", "", False),
        ("Datum aus EXIF-Aufnahmedatum", "ja", "ja", "?", "", False),
        ("Automatisch aufsteigende Dateizeiten", "–", "ja", "?", "", False),
        ("Dateien teilen und zusammenfügen", "ja", "ja", "ja", "QFiles: CRC-Prüfung, Batchdatei zum Zusammenfügen", False),
        ("Prüfsummen berechnen (CRC32, MD5, SHA)", "–", "–", "ja", "", False),
        ("Datei auf Lesbarkeit prüfen", "–", "ja", "?", "", False),
        ("Junctions, Hardlinks, symbolische Links erstellen", "–", "–", "ja", "", False),
        ("Erweiterte Kopieroptionen (Hardlinks, Sparse, Datenströme, SMB-Kompression)", "–", "–", "ja", "", False),
        ("Dateien per E-Mail senden", "teilw.", "ja (Pro)", "?", "QFiles: über „Senden an“ im Explorer-Kontextmenü", A),
        ("Explorer-Kontextmenü, „Öffnen mit“, Eigenschaften", "ja", "ja", "ja", "", False),
        ("Favoriten: Dateien kennzeichnen, eigene Liste mit vollständigem Pfad", "ja", "–", "teilw.",
         "neu in QFiles 0.1.8; SC: Dateiablage bzw. Tags (Pro)", False),
        ("Dateiablage (Sammelbehälter für Verweise)", "–", "–", "ja", "QFiles: Favoriten (dauerhaft, nur Dateien)", False),
        ("Verwaltung der Dateitypen-Zuordnungen", "–", "ja", "ja", "", False),
    ]),
    ("Dateigruppe umbenennen", [
        ("Mehrere Dateien nach Maske umbenennen, Live-Vorschau", "ja", "ja", "ja", "", False),
        ("Konfliktprüfung", "ja", "ja", "ja", "SC: Sprung zu Konflikten, Auto-Zähler", False),
        ("Platzhalter Name und Erweiterung mit Teilbereichen", "ja", "ja", "ja", "QFiles und SC: negative Positionen vom Ende", False),
        ("Zähler (Start, Schritt, Stellen, je Verzeichnis neu)", "ja", "ja", "ja", "", False),
        ("Platzhalter Änderungsdatum und Uhrzeit", "ja", "ja", "ja", "", False),
        ("Platzhalter aktuelles Datum/Uhrzeit", "–", "ja", "?", "", False),
        ("Platzhalter Name des Elternverzeichnisses", "ja", "ja", "?", "", False),
        ("Platzhalter EXIF-Daten und MP3-Tags", "–", "ja", "?", "", False),
        ("Platzhalter Zufallszahl", "–", "–", "ja", "", False),
        ("Suchen und Ersetzen, reguläre Ausdrücke", "ja", "teilw.", "ja", "Idoswin: ohne reguläre Ausdrücke", False),
        ("Groß-/Kleinschreibung umwandeln", "ja", "ja", "ja", "", False),
        ("Unterverzeichnisse und Verzeichnisnamen einbeziehen", "ja", "ja", "ja", "", False),
        ("Einstellungen als Profil speichern", "teilw.", "ja", "ja", "QFiles: Verlauf der Masken", A),
        ("Rückgängig", "ja", "ja", "ja", "", False),
    ]),
    ("Suchen", [
        ("Dateisuche nach Name, Größe, Datum, Attributen", "ja", "–", "ja", "", False),
        ("Inhaltssuche in Text (UTF-8, UTF-16, ANSI)", "ja", "–", "ja", "", False),
        ("Suche in Office-Dokumenten und PDF", "ja", "–", "ja",
         "QFiles: Office, OpenDocument, EPUB, PDF (neu in 0.1.11, auch verschlüsselt ohne Kennwort); SC: über IFilter", False),
        ("Hexadezimale bzw. binäre Inhaltssuche", "–", "–", "ja", "", False),
        ("Suchoperatoren UND/ODER/NICHT, reguläre Ausdrücke", "–", "–", "ja", "", False),
        ("Suche in Archiven", "–", "–", "ja", "", False),
        ("Everything-Integration", "–", "–", "ja (Pro)", "", False),
        ("Nur leere Ordner suchen", "–", "–", "ja", "", False),
        ("Treffer in die Liste übernehmen", "teilw.", "–", "ja",
         "QFiles: „Gehe zu“; neu in 0.1.11: „Schließen und markieren“ bzw. „… invers markieren“ in der Liste", A),
        ("Suchprofile", "–", "–", "ja", "", False),
        ("Doppelte Dateien finden", "ja", "ja", "ja", "QFiles: Größe, Hash, byteweise; Idoswin: über den Verzeichnisvergleich", False),
    ]),
    ("Vergleichen und Synchronisieren", [
        ("Verzeichnisse vergleichen, Ergebnis markiert", "ja", "ja", "ja", "", False),
        ("Kriterien Größe, Datum, Inhalt", "ja", "ja", "ja", "", False),
        ("Sommerzeit-Differenz (1 h) ignorieren, Zeittoleranz", "ja", "ja", "?", "", False),
        ("Unterverzeichnisse einbeziehen", "ja", "ja", "ja", "", False),
        ("Vergleich beliebiger zweier von vier Listen", "ja", "–", "–", "", False),
        ("Verzeichnisse synchronisieren (Vorschau, Filter, Spiegeln)", "ja", "ja", "ja", "", False),
        ("Synchronisieren in beide Richtungen", "ja", "?", "ja", "", False),
        ("NTFS-Sicherheitsattribute übertragen", "–", "–", "ja", "", False),
        ("Synchronisationsprotokoll, Kommandozeilen-Werkzeug", "–", "–", "ja", "", False),
        ("Textdateien zeilenweise nebeneinander vergleichen", "ja", "ja", "–", "SC: externes Vergleichsprogramm", False),
        ("Externes Vergleichsprogramm (Beyond Compare) aufrufen", "ja", "?", "ja",
         "neu in QFiles 0.1.10: Kontextmenü „Mit anderer Liste vergleichen“ bei gleichnamiger Datei in einer anderen "
         "Liste; Richtung am Symbol", False),
        ("Zeichengenaue Hervorhebung der Unterschiede", "ja", "–", "–", "", False),
        ("Vergleich wahlweise übereinander", "–", "ja", "–", "", False),
        ("Binärdateien vergleichen", "ja", "ja", "–", "", False),
        ("Dateien aus Archiven vergleichen", "–", "ja", "–", "", False),
    ]),
    ("Archive", [
        ("Archiv wie ein Verzeichnis anzeigen", "teilw.", "ja", "ja", "QFiles: in der Dateianzeige (F11) als Liste", A),
        ("ZIP erstellen", "ja", "ja", "ja", "QFiles: über die Windows-Shell", False),
        ("ZIP bearbeiten (hinzufügen, löschen, zurückschreiben)", "–", "ja", "ja", "", False),
        ("RAR, 7z, TAR, CAB, ISO anzeigen und entpacken", "ja", "teilw.", "ja", "QFiles: über tar.exe von Windows; Idoswin: RAR, ACE, CAB", False),
        ("Weitere Formate packen (7z, SQX …)", "–", "–", "ja", "", False),
        ("ACE-Archive", "–", "ja", "–", "", False),
        ("Kennwortgeschützte Archive öffnen, AES-Verschlüsselung", "–", "?", "ja", "QFiles: klare Meldung statt der Liste", False),
        ("SFX, mehrteilige Archive, Archivtest, Kommentare", "–", "–", "ja", "", False),
        ("Herausziehen per Drag & Drop und Zwischenablage", "ja", "ja", "ja", "", False),
        ("Im Archiv suchen", "ja", "–", "ja", "", False),
        ("Eintrag öffnen (temporäre Kopie)", "ja", "ja", "ja", "", False),
    ]),
    ("Anzeige und Editor", [
        ("Schnellansicht in der anderen Liste", "ja", "ja", "ja", "", False),
        ("Eigenes Anzeigefenster, blättert durch das Verzeichnis", "ja", "?", "ja", "SC: SpeedView", False),
        ("Text mit Kodierungserkennung", "ja", "ja", "ja", "", False),
        ("Bilder", "ja", "ja", "ja", "QFiles: WIC; Idoswin: eigene Formate und IrfanView; SC: WIC, RAW, IrfanView/XnView", False),
        ("PDF anzeigen", "ja", "ja", "ja", "Idoswin: über Adobe Reader", False),
        ("PDF: bis zu vier Seiten als Übersicht", "ja", "–", "–", "", False),
        ("Video: Einzelbilder als Übersicht mit Auflösung und Dauer", "ja", "–", "–", "", False),
        ("Audio und Video abspielen", "–", "ja", "ja", "", False),
        ("Office, HTML, RTF (Vorschauhandler)", "ja", "ja", "ja", "", False),
        ("Programmdatei-Viewer (Importe, Exporte, Zertifikate)", "ja", "–", "ja",
         "neu in QFiles 0.1.10: F11 auf .exe/.dll – Programmsymbol, Versionsinfo, PE-Kopf, Manifest, Signatur, "
         "Importe/Exporte, Windows-Eigenschaften", False),
        ("Zeilenfilter in der Anzeige", "–", "–", "ja", "", False),
        ("Hex-Ansicht und Hex-Editor", "ja", "ja", "ja", "", False),
        ("Hex-Editor: Bytes einfügen/löschen", "–", "?", "?", "QFiles: nur Überschreibmodus", False),
        ("Integrierter Texteditor, mehrere Fenster", "ja", "ja", "ja", "SC: SpeedEdit", False),
        ("Kodierungen UTF-8 (Standard ohne BOM), UTF-16, ANSI, OEM", "ja", "ja", "ja",
         "QFiles: neue und leere Dateien als UTF-8 ohne BOM", A),
        ("Zeilenenden CRLF, LF, CR", "ja", "ja", "?", "", False),
        ("Suchen, Ersetzen, Gehe zu Zeile", "ja", "?", "ja", "", False),
        ("Zeilen sortieren, doppelte Zeilen entfernen", "ja", "teilw.", "?", "Idoswin: markierten Block sortieren", False),
        ("Syntaxhervorhebung", "–", "ja", "ja", "", False),
        ("Spalten-/Blockmarkierung", "–", "ja", "ja", "", False),
        ("Lineale, Dateien mit fester Zeilenlänge", "–", "ja", "–", "", False),
        ("Erweiterte Editorfunktionen (Lesezeichen, AutoText, Gliederung, Änderungshistorie)", "–", "–", "ja", "", False),
        ("Drucken aus dem Editor", "ja", "ja", "ja", "SC: mit Druckvorschau", False),
        ("Meldung bei Änderung außerhalb des Editors", "ja", "?", "ja", "", False),
    ]),
    ("FTP, SFTP, WebDAV und Cloud", [
        ("FTP", "ja", "–", "ja", "", False),
        ("SFTP", "ja", "–", "ja", "", False),
        ("FTPS (FTP über SSL/TLS)", "–", "–", "ja", "", False),
        ("WebDAV", "ja", "–", "ja (Pro)", "QFiles: Basic, Digest, NTLM, Kerberos", False),
        ("Zugänge verwalten", "ja", "–", "ja", "QFiles: als Lesezeichen", False),
        ("Kennwörter verschlüsselt speichern", "ja", "–", "ja", "QFiles: Windows-DPAPI; SC: Hauptkennwort", False),
        ("SSH-Hostschlüssel prüfen und speichern", "ja", "–", "?", "", False),
        ("Anmeldung mit SSH-Agent (Pageant, OpenSSH-Agent)", "ja", "–", "?", "", False),
        ("Ed25519-Hostschlüssel", "–", "–", "ja", "", False),
        ("Kopieren zwischen zwei Servern", "ja", "–", "?", "", False),
        ("Bearbeiten mit automatischem Hochladen", "ja", "–", "?", "", False),
        ("Unix-Zugriffsrechte anzeigen und setzen (chmod)", "ja", "–", "ja", "neu in QFiles 0.1.7", False),
        ("Protokoll- und Statusfenster", "teilw.", "–", "ja", "QFiles: Dateisystemmonitor", A),
        ("Proxy, IPv6, Client-Zertifikate", "teilw.", "–", "ja", "QFiles: nur WebDAV mit den Proxy-Einstellungen von Windows", A),
        ("FTP-Befehlszeile", "–", "–", "ja", "", False),
        ("Cloud direkt (OneDrive, Dropbox, Box) ohne Anbieter-Programm", "–", "–", "ja (Pro)", "QFiles: über die Ordner der Desktop-Programme", False),
    ]),
    ("Netzwerk, Laufwerke, Werkzeuge", [
        ("Netzwerkpfade (UNC) wie lokale Verzeichnisse", "ja", "ja", "ja", "", False),
        ("Laufwerksübersicht", "ja", "ja", "ja", "", False),
        ("Datenträger formatieren und umbenennen", "teilw.", "–", "ja", "QFiles: über das Kontextmenü des Laufwerks", A),
        ("Schattenkopien, BitLocker entsperren", "–", "–", "ja", "", False),
        ("CD/DVD/Blu-ray brennen, ISO erstellen", "–", "–", "ja (Pro)", "", False),
        ("Dateisystemmonitor (Protokoll der Dateioperationen)", "ja", "ja", "teilw.",
         "QFiles: nur von QFiles ausgeführte Operationen; Idoswin: alle Änderungen", A),
        ("Verzeichnisliste drucken, als Text speichern", "ja", "ja", "ja", "", False),
        ("Programmierbare Funktionstasten mit Platzhaltern", "ja", "ja", "teilw.", "je 24 Tasten; SC: Anwendungsfavoriten", False),
        ("Programm-Menü / Anwendungsfavoriten", "teilw.", "ja", "ja", "QFiles: über die Funktionstasten", A),
        ("Makros, Skripte, Objektmodell", "–", "–", "ja", "", False),
        ("AddIn-Schnittstelle", "–", "–", "ja", "", False),
        ("Integrierter Webbrowser", "–", "–", "ja", "", False),
        ("Programme mit reduzierten Rechten starten", "–", "–", "ja", "", False),
    ]),
]

SOURCES = [
    "QFiles: Quelltext und README (Version " + QF_VERSION + ")",
    "Idoswin Pro: idoswin.de – Funktionen (idosfunc.htm), Aktuelles (idosnews.htm), Bildschirmfotos und "
    "Detailseiten, Idoswin Free (idosfree.htm), Zusatzprogramme (idosadd.htm); ergänzend computerwoche.de, "
    "pcwelt.de, heise.de",
    "SpeedCommander: speedproject.de – Funktionen im Detail (/sc/funktionen/), Übersicht (/sc/), Neuerungen "
    "(/neu/), Versionsgeschichte (/sc/versionsgeschichte/), Hilfe-FAQ; ergänzend netzwelt.de",
]

RED = colors.HexColor("#f4a6a6")
YELLOW = colors.HexColor("#fbe68a")
GREEN = colors.HexColor("#a8dca8")
HEAD = colors.HexColor("#d9e4f2")
CAT = colors.HexColor("#eef2f7")


def mark(q, i, s, abw):
    if q.startswith("–"):
        return RED
    if i.startswith("–") and s.startswith("–"):
        return GREEN
    if abw:
        return YELLOW
    return None


def main(out):
    pdfmetrics.registerFont(TTFont("DV", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"))
    pdfmetrics.registerFont(TTFont("DVB", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"))
    from reportlab.lib.fonts import addMapping
    addMapping("DV", 0, 0, "DV")
    addMapping("DV", 1, 0, "DVB")
    base = ParagraphStyle("b", fontName="DV", fontSize=7.6, leading=9.4, alignment=TA_LEFT)
    small = ParagraphStyle("s", parent=base, fontSize=6.8, leading=8.4, textColor=colors.HexColor("#333333"))
    head = ParagraphStyle("h", parent=base, fontName="DVB", fontSize=8)
    title = ParagraphStyle("t", parent=base, fontName="DVB", fontSize=16, leading=20, spaceAfter=4)
    sub = ParagraphStyle("st", parent=base, fontSize=9, leading=12)

    doc = SimpleDocTemplate(out, pagesize=landscape(A4), leftMargin=12 * mm, rightMargin=12 * mm,
                            topMargin=12 * mm, bottomMargin=12 * mm, title="Funktionsvergleich QFiles – Idoswin Pro – SpeedCommander",
                            author="QFiles")
    story = []
    story.append(Paragraph("Funktionsvergleich: QFiles – Idoswin Pro – SpeedCommander", title))
    today = datetime.date.today().strftime("%d.%m.%Y")
    story.append(Paragraph(
        f"QFiles {QF_VERSION} · Idoswin Pro {ID_VERSION}, letzte Version · SpeedCommander {SC_VERSION}, "
        f"Ausgaben Standard und Pro · Stand {today}", sub))
    story.append(Spacer(1, 3 * mm))

    rows_total = sum(len(r) for _, r in DATA)
    cnt = {"r": 0, "y": 0, "g": 0}
    for _, rows in DATA:
        for (n, q, i, s, note, abw) in rows:
            c = mark(q, i, s, abw)
            if c is RED: cnt["r"] += 1
            elif c is YELLOW: cnt["y"] += 1
            elif c is GREEN: cnt["g"] += 1

    legend = Table([
        ["", Paragraph("<b>Farbe der Nummernspalte</b>", base), "", Paragraph("<b>Einträge</b>", base)],
        ["", Paragraph("in QFiles nicht umgesetzt", base), "", Paragraph(str(cnt["r"]), base)],
        ["", Paragraph("in QFiles abweichend umgesetzt", base), "", Paragraph(str(cnt["y"]), base)],
        ["", Paragraph("nur in QFiles, in keinem der beiden anderen Programme", base), "", Paragraph(str(cnt["g"]), base)],
        ["", Paragraph("ohne Farbe: in QFiles vorhanden wie in mindestens einem der anderen Programme", base), "",
         Paragraph(str(rows_total - cnt["r"] - cnt["y"] - cnt["g"]), base)],
    ], colWidths=[8 * mm, 120 * mm, 4 * mm, 18 * mm])
    legend.setStyle(TableStyle([
        ("BACKGROUND", (0, 1), (0, 1), RED), ("BACKGROUND", (0, 2), (0, 2), YELLOW), ("BACKGROUND", (0, 3), (0, 3), GREEN),
        ("BOX", (0, 1), (0, 4), 0.4, colors.grey), ("INNERGRID", (0, 1), (0, 4), 0.4, colors.grey),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"), ("TOPPADDING", (0, 0), (-1, -1), 1.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 1.5),
    ]))
    story.append(legend)
    story.append(Spacer(1, 2 * mm))
    story.append(Paragraph(
        "Werte: <b>ja</b> = vorhanden · <b>teilw.</b> = eingeschränkt · <b>–</b> = nicht vorhanden bzw. in den "
        "ausgewerteten Herstellerangaben nicht genannt · <b>?</b> = keine verlässliche Angabe gefunden. "
        "Grün setzt voraus, dass beide anderen Programme „–“ haben; ein „?“ zählt nicht als fehlend. "
        "Grundlage für Idoswin Pro und SpeedCommander sind die Funktionsbeschreibungen, Versionsgeschichten und "
        "Hilfeseiten der Hersteller (siehe Quellen am Ende); nicht beschriebene Funktionen können dennoch vorhanden sein.",
        small))
    story.append(Spacer(1, 4 * mm))

    widths = [11 * mm, 88 * mm, 22 * mm, 24 * mm, 33 * mm, 95 * mm]
    header = [Paragraph(h, head) for h in ("Nr.", "Funktion", "QFiles", "Idoswin Pro", "SpeedCommander", "Bemerkung")]
    table = [header]
    style = [
        ("BACKGROUND", (0, 0), (-1, 0), HEAD),
        ("GRID", (0, 0), (-1, -1), 0.3, colors.HexColor("#9aa5b1")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("TOPPADDING", (0, 0), (-1, -1), 1.6), ("BOTTOMPADDING", (0, 0), (-1, -1), 1.6),
        ("LEFTPADDING", (0, 0), (-1, -1), 3), ("RIGHTPADDING", (0, 0), (-1, -1), 3),
    ]
    nr = 0
    for cat, rows in DATA:
        r = len(table)
        table.append([Paragraph(f"<b>{cat}</b>", base), "", "", "", "", ""])
        style += [("SPAN", (0, r), (-1, r)), ("BACKGROUND", (0, r), (-1, r), CAT)]
        for (n, q, i, s, note, abw) in rows:
            nr += 1
            r = len(table)
            table.append([Paragraph(str(nr), base), Paragraph(n, base), Paragraph(q, base), Paragraph(i, base),
                          Paragraph(s, base), Paragraph(note, small)])
            c = mark(q, i, s, abw)
            if c is not None:
                style.append(("BACKGROUND", (0, r), (0, r), c))
    t = Table(table, colWidths=widths, repeatRows=1)
    t.setStyle(TableStyle(style))
    story.append(t)

    story.append(Spacer(1, 5 * mm))
    story.append(Paragraph("<b>Quellen</b>", base))
    for s in SOURCES:
        story.append(Paragraph("• " + s, small))

    def footer(canvas, d):
        canvas.saveState()
        canvas.setFont("DV", 7)
        canvas.setFillColor(colors.grey)
        canvas.drawRightString(landscape(A4)[0] - 12 * mm, 7 * mm, f"Seite {d.page}")
        canvas.drawString(12 * mm, 7 * mm, "Funktionsvergleich QFiles – Idoswin Pro – SpeedCommander")
        canvas.restoreState()

    doc.build(story, onFirstPage=footer, onLaterPages=footer)
    print(f"{out}: {nr} Funktionen, rot {cnt['r']}, gelb {cnt['y']}, grün {cnt['g']}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "QFiles Funktionsvergleich.pdf")
