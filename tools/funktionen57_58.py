# Erzeugt „Funktionen57_58.pdf“: Funktionen 57 (Shell-Namensraum) und 58 (Smartphones, Tablets, Kameras über MTP)
# aus dem Funktionsvergleich – Umsetzung in Idoswin Pro und SpeedCommander, Möglichkeiten für QFiles.
# Aufruf: python tools/funktionen57_58.py [Ausgabedatei]   (benötigt reportlab und die Schrift DejaVu Sans)
import datetime
import sys

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import KeepTogether, Paragraph, SimpleDocTemplate, Spacer, Table, TableStyle

QF_VERSION = "0.1.10"

HEAD = colors.HexColor("#d9e4f2")
CAT = colors.HexColor("#eef2f7")
RED = colors.HexColor("#f4a6a6")
YELLOW = colors.HexColor("#fbe68a")
GREEN = colors.HexColor("#a8dca8")
GRID = colors.HexColor("#9aa5b1")
NOTE = colors.HexColor("#fff8dc")

# ---------------------------------------------------------------------------------------------------------------
# Inhalt

INTRO = (
    "Diese Ausarbeitung vertieft zwei Zeilen des Funktionsvergleichs (Stand QFiles " + QF_VERSION + "), die in "
    "QFiles bisher fehlen. Sie beschreibt, was über die Umsetzung in Idoswin Pro und SpeedCommander bekannt ist, "
    "und stellt die Möglichkeiten für QFiles mit Aufwand, Nutzen und Risiken gegenüber. Grundlage sind die "
    "Funktionsseiten, Versionsgeschichten und Hilfeseiten der Hersteller (siehe Quellen) sowie der Quelltext von "
    "QFiles. Was die Hersteller nicht beschreiben (etwa die genaue Spaltenauswahl im Papierkorb von Idoswin), "
    "bleibt offen und ist als solches gekennzeichnet."
)

OVERVIEW = [
    ("57", "Shell-Namensraum (Dieser PC, Papierkorb, virtuelle Ordner)", "–", "teilw.", "ja",
     "Idoswin: Papierkorb-Anzeige"),
    ("58", "Smartphones, Tablets, Kameras (MTP)", "–", "–", "ja", ""),
]

TERMS = [
    ("Shell-Namensraum",
     "Die Ordnerhierarchie des Explorers. Sie umfasst neben dem Dateisystem auch Ordner ohne Pfad: Desktop, "
     "„Dieser PC“, Papierkorb, Bibliotheken, Netzwerk, Systemsteuerung sowie Geräte ohne Laufwerksbuchstaben. "
     "Elemente werden über Elementkennungen (PIDLs bzw. IShellItem) angesprochen, nicht über Pfade; Kopieren, "
     "Löschen usw. erledigt die Shell (IFileOperation) bzw. der jeweilige Ordner selbst."),
    ("MTP / PTP",
     "Media Transfer Protocol (Android-Smartphones und -Tablets, viele Mediaplayer) bzw. Picture Transfer Protocol "
     "(Kameras). Das Gerät gibt keinen Datenträger frei, sondern verwaltet seine Dateien selbst; Windows bindet es "
     "ohne Laufwerksbuchstaben ein und greift über die Programmierschnittstelle Windows Portable Devices (WPD) "
     "darauf zu. Im Explorer erscheint es unter „Dieser PC“."),
]

# ---- Funktion 57 ----

F57_IDOSWIN = [
    "Der Papierkorb erscheint als Eintrag im Verzeichnis- und im Dateifenster; die gelöschten Dateien werden "
    "angezeigt. Belegt ist das durch die Versionsgeschichte: Idoswin Free 3.5/3.6 und Idoswin Pro 5.6 "
    "(26.01.2008) beheben den Fehler, dass Dateien im Papierkorb nach der ersten Anzeige nicht mehr angezeigt "
    "wurden.",
    "Ein Rechtsklick auf den Papierkorb (im Datei- oder Verzeichnisfenster) zeigt das Kontextmenü des "
    "Desktop-Papierkorbs, also die Befehle von Windows wie „Papierkorb leeren“ (Idoswin Free 3.5).",
    "Der Verzeichnisbaum hat einen Knoten „Netzwerkumgebung“ (Leistungsverbesserung in 5.62, Korrektur des "
    "Kontextmenüs darunter in 5.8). Dazu kommt das Menü „Springen“ für Systemverzeichnisse (Startmenü, Favoriten, "
    "Eigene Dateien usw.).",
    "Desktop, „Arbeitsplatz“/„Dieser PC“, Bibliotheken, Systemsteuerung oder Geräte ohne Laufwerksbuchstaben "
    "werden in keiner Herstellerangabe erwähnt. Daher „teilweise“: nur Papierkorb (und Netzwerk).",
    "Wie Einträge im Papierkorb wiederhergestellt werden (eigener Befehl oder nur das Windows-Kontextmenü der "
    "einzelnen Datei) und welche Spalten (ursprünglicher Ort, Löschdatum) erscheinen, beschreibt der Hersteller "
    "nicht. Die letzte Version von Idoswin Pro ist 5.8 (2018).",
]

F57_SC = [
    "<b>Seit Version 13:</b> „direkter Zugriff auf den Desktop und die Elemente des Arbeitsplatzes“. Oberstes "
    "Element der Baumansicht ist wie im Explorer der Desktop; damit sind Datenträger ohne Laufwerksbuchstaben und "
    "die Bibliotheken von Windows 7 erreichbar. In Version 13 ließ sich das nur über "
    "<font face='DVM'>ShellNameSpace=0</font> in der INI-Datei abschalten (dann ohne Desktop und ohne Geräte).",
    "<b>Ab Version 14:</b> Option „Immer ‚Desktop‘ als obersten Eintrag in der Baumansicht anzeigen“ "
    "(Einstellungen → Aussehen → Dateien &amp; Ordner). Ausgeschaltet beginnt der Baum wieder mit den Laufwerken.",
    "<b>Laufwerk „0“:</b> Über den Laufwerksbuchstaben 0 wechselt man in den Desktop-Ordner, von dort nach "
    "„Dieser PC“ usw. Für schnellen Zugriff (etwa auf ein Gerät) empfiehlt der Hersteller einen Ordnerfavoriten.",
    "<b>Version 20:</b> Die Benutzerordner unter „Dieser PC“ (Dokumente, Bilder …) lassen sich in Baum- und "
    "Listenansicht ausblenden. Version 19.60 korrigiert das Löschen in den „virtuellen Dokumentenverzeichnissen“ "
    "unter „Dieser PC“.",
    "<b>Version 21 (RC1 27.11.2023):</b> eingebaute Unterstützung für Dateioperationen im Shell-Namensbereich, "
    "neue Ordner mit F7, Ordnerinformationen und Ordnergrößen auch dort, immer vollständiger "
    "Dateiname mit Endung, Shell-Elemente als Quelle und Ziel in Makros; Wechseldatenträger direkt unter dem "
    "Desktop werden ausgefiltert (sie stehen ohnehin unter „Dieser PC“). Spätere Versionen korrigieren u. a. "
    "„Shell-Spalten“ in der Zweigansicht (21.20), den Ersetzen-Dialog für Shell-Elemente (21.20) und einen Absturz "
    "beim Umbenennen im Shell-Namensbereich (21.51/22.00).",
    "<b>Papierkorb:</b> Löschen in den Papierkorb und „Sicheres Löschen“ gehören zum Dateimanagement. Ein "
    "eigener Papierkorb-Befehlssatz ist nicht beschrieben. Der Papierkorb dürfte – wie im Explorer – als "
    "Shell-Ordner über den Desktop erreichbar sein und mit den Mitteln des Shell-Namensbereichs (Shell-Spalten, "
    "Kontextmenü) angezeigt werden.",
]

F57_QFILES = [
    "Der Verzeichnisbaum beginnt mit den Laufwerken; dazu kommen „Netzwerk“ (Rechner und Freigaben, über die "
    "Shell gesucht) und die Lesezeichenliste mit „Favoriten“ und – neu in 0.1.10 – „Linux“ (WSL).",
    "Gelöscht wird wahlweise in den Papierkorb (Option „Papierkorb verwenden“, IFileOperation mit "
    "FOFX_RECYCLEONDELETE). Den Inhalt des Papierkorbs zeigt QFiles nicht; Wiederherstellen geht nur im Explorer. "
    "Das Löschen ist auch nicht in der eigenen Rückgängig-Liste (diese kennt Kopieren, Verschieben, Umbenennen "
    "und Anlegen).",
    "Brauchbare Bausteine sind vorhanden: Orte ohne Win32-Pfad (Netzwerk, FTP/SFTP/WebDAV, Favoriten) mit eigener "
    "Aufzählung in <font face='DVM'>FilePane</font>; die Favoritenansicht mit erzwungener Darstellung und "
    "Zusatzspalte („Verzeichnis“); das Explorer-Kontextmenü über <font face='DVM'>IShellFolder::GetUIObjectOf</font> "
    "mit Schutz vor fehlerhaften Erweiterungen (<font face='DVM'>ShellMenu.cpp</font>).",
]

F57_OPTIONS = [
    ("A", "Shell-Ordner im Explorer öffnen",
     "Menü „Gehe zu → Windows-Ordner“ bzw. Lesezeichen, die den Explorer mit "
     "<font face='DVM'>shell:RecycleBinFolder</font>, <font face='DVM'>shell:MyComputerFolder</font>, "
     "<font face='DVM'>shell:Libraries</font>, <font face='DVM'>shell:ControlPanelFolder</font> usw. starten.",
     "sehr gering", "gering – nur ein Absprung, keine Bedienung in QFiles", "keines"),
    ("B", "Papierkorb als eigene Ansicht",
     "Pseudo-Ort „Papierkorb“ (wie „Favoriten“) in Baum, Lesezeichen und Menü. Spalten: Name, ursprünglicher "
     "Ort, gelöscht am, Größe, Typ. Befehle: Wiederherstellen (auch an anderen Ort per Ziehen/Kopieren in eine "
     "Liste), endgültig löschen, Papierkorb leeren; Statuszeile mit Anzahl und Größe "
     "(<font face='DVM'>SHQueryRecycleBin</font>). Dazu „Löschen rückgängig“ für in QFiles gelöschte Dateien.",
     "mittel", "hoch – häufig gebraucht, auch Idoswin hatte es", "gering"),
    ("C", "Desktop und „Dieser PC“ als Wurzel",
     "Wie SpeedCommander ab Version 13. Allgemeine Shell-Ansicht: neue Ortsart für Shell-Elemente (Parsing-Name, z. B. "
     "<font face='DVM'>::{20D04FE0-3AEA-1069-A2D8-08002B30309D}</font>), Aufzählung über IShellItem, Spalten aus "
     "dem Eigenschaftenspeicher, Dateioperationen über IFileOperation mit IShellItem-Quellen und -Zielen, "
     "Kontextmenü über PIDLs. Baum wahlweise mit „Desktop“ als oberstem Eintrag.",
     "hoch", "hoch – Bibliotheken, Geräte ohne Buchstaben, Papierkorb, Systemsteuerung in einem",
     "mittel bis hoch – langsame oder fehlerhafte Erweiterungen, viele Sonderfälle"),
]

F57_B_DETAIL = [
    "<b>B1 – über die Shell:</b> Den Papierkorb-Ordner (<font face='DVM'>FOLDERID_RecycleBinFolder</font>) mit "
    "<font face='DVM'>SHGetKnownFolderItem</font> öffnen und die Elemente aufzählen. Ursprünglicher Ort und "
    "Löschdatum liefern die Eigenschaften des Papierkorbs (FMTID_Displaced: PID_DISPLACED_FROM, "
    "PID_DISPLACED_DATE), Größe und Typ der Eigenschaftenspeicher. Wiederherstellen über das Verb „undelete“ des "
    "Kontextmenüs der markierten Elemente, Leeren über <font face='DVM'>SHEmptyRecycleBin</font>. Vorteil: alle "
    "Papierkörbe (alle Laufwerke, auch Netzwerk-Umleitungen der Benutzerordner), genau wie im Explorer.",
    "<b>B2 – direkt aus den Papierkorbdateien:</b> Je Laufwerk liegt in "
    "<font face='DVM'>\\$Recycle.Bin\\&lt;SID&gt;\\</font> zu jeder gelöschten Datei ein Paar "
    "<font face='DVM'>$I…</font> (Kopf: Version, Größe, Löschzeit, ursprünglicher Pfad) und "
    "<font face='DVM'>$R…</font> (Inhalt). QFiles kann diese Dateien selbst lesen und zum Wiederherstellen "
    "<font face='DVM'>$R…</font> an den alten Ort verschieben (mit dem eigenen Namenskonflikt-Dialog) und "
    "<font face='DVM'>$I…</font> löschen. Vorteil: schnell, ohne Shell-Erweiterungen, auf Wunsch je Laufwerk; "
    "Nachteil: Format nicht offiziell dokumentiert (Version 1 bis Windows 8.1, Version 2 ab Windows 10), "
    "Netzwerkpapierkörbe fehlen, der Explorer bemerkt Änderungen erst beim nächsten Öffnen.",
    "<b>Empfehlung:</b> B1 als Grundlage (verlässlich, alle Papierkörbe); einzelne Befehle können B2 nutzen, z. B. "
    "„Löschen rückgängig“, weil QFiles die eben gelöschten Pfade kennt. Die Darstellung übernimmt die Technik der "
    "Favoritenansicht (Zusatzspalten, erzwungene Ansicht, eigene Hintergrundfarbe).",
]

# ---- Funktion 58 ----

F58_IDOSWIN = [
    "Keine Unterstützung beschrieben. Idoswin Pro 5.8 stammt von 2018; da es den Shell-Namensraum außer Papierkorb "
    "und Netzwerk nicht zeigt, sind Geräte ohne Laufwerksbuchstaben nicht erreichbar.",
]

F58_SC = [
    "<b>Über den Shell-Namensraum, seit Version 13:</b> Die Funktionsübersicht nennt „direkter Zugriff auf "
    "Desktop, Computer und virtuelle Ordner (z. B. Tablets und Smartphones)“. Ein eigener Gerätezugriff (eigene "
    "WPD-Schnittstelle) ist nicht beschrieben.",
    "<b>Bedienung laut Hilfeseite „Zugriff auf Tablet und Smartphone“:</b> Die Laufwerksleiste zeigt nur "
    "Laufwerke mit Buchstaben, das Gerät also nicht. Man wechselt mit dem Laufwerksbuchstaben 0 in den "
    "Desktop-Ordner, dann nach „Computer“ bzw. „Dieser PC“ und dort auf das Gerät; für schnellen Zugriff legt man "
    "einen Ordnerfavoriten an.",
    "<b>Dateioperationen:</b> Seit Version 21 mit den eingebauten Operationen des Shell-Namensbereichs (mit "
    "eigenem Ersetzen-Dialog für Shell-Elemente, korrigiert in 21.20). Version 21.30 (11.11.2024) behebt, dass "
    "beim Kopieren leerer MTP-Ordner das Zielverzeichnis nicht erstellt wurde – SpeedCommander kopiert "
    "MTP-Inhalte also offenbar selbst, Element für Element.",
    "Einschränkungen (Datum, Umbenennen, Geschwindigkeit, Bearbeiten an Ort und Stelle) nennt der Hersteller "
    "nicht.",
]

F58_TECH = [
    "Kein Laufwerksbuchstabe, keine Win32-Pfade: Dateien sind Objekte mit Kennungen; ein Pfad muss Ebene für Ebene "
    "über die Namen der Kindobjekte aufgelöst werden.",
    "Lesen und Schreiben nur als ganzer Datenstrom (Herunterladen/Hochladen). Programme können eine Datei nicht "
    "direkt öffnen; Anzeigen und Bearbeiten brauchen eine temporäre Kopie – wie bei FTP/SFTP in QFiles.",
    "Langsames Auflisten großer Ordner (z. B. DCIM mit Tausenden Fotos), Übertragungen nacheinander, eine "
    "Sitzung je Gerät. Viele Geräte unterstützen kein Umbenennen und kein Setzen des Änderungsdatums; kopierte "
    "Dateien erhalten dann das Datum der Übertragung.",
    "Android gibt den Speicher erst frei, wenn das Gerät entsperrt und im USB-Menü „Dateiübertragung“ gewählt "
    "ist; vorher ist das Gerät sichtbar, aber leer. Kameras (PTP) erlauben oft nur Lesen und Löschen.",
]

F58_OPTIONS = [
    ("A", "Über die Shell",
     "Wie SpeedCommander. Teil von Option C bei Funktion 57: Geräte unter „Dieser PC“ aufzählen und Ordner als Shell-Elemente "
     "anzeigen; Größe und Datum aus dem Eigenschaftenspeicher; Kopieren über IFileOperation mit IShellItem.",
     "nur zusammen mit 57 C sinnvoll",
     "Fortschritt und Konflikte zeigt Windows (eigener Dialog nur mit Aufwand wie SpeedCommander 21)"),
    ("B", "Eigener WPD-Zugriff als weiteres Protokoll",
     "Neue Ortsart <font face='DVM'>mtp://Gerät/Interner Speicher/DCIM</font> neben ftp://, sftp://, dav://. "
     "Ein WPD-Client implementiert die vorhandene Schnittstelle <font face='DVM'>RemoteFs</font> "
     "(Auflisten, Herunter-/Hochladen mit Fortschritt, Löschen, Ordner anlegen, Umbenennen wo möglich). Damit "
     "gelten automatisch Kopieren mit Fortschritt und Namenskonflikt-Dialog, Bearbeiten mit Zurückschreiben, "
     "Lesezeichen und Verlauf. Geräte erscheinen im Baum unter einem Knoten „Geräte“, in der Laufwerksleiste "
     "mit Telefon-/Kamerasymbol und auf Wunsch als Lesezeichen (eigene Farbe).",
     "mittel bis hoch (etwa im Umfang des SFTP-Clients)",
     "volle Kontrolle; Fehlermeldungen wie „Gerät gesperrt – bitte entsperren und Dateiübertragung wählen“"),
    ("C", "Android Debug Bridge (ADB)",
     "Zugriff über <font face='DVM'>adb.exe</font> (Dateiliste, push/pull). Schnell, echte Datums- und "
     "Rechteangaben, voller Dateibaum.",
     "mittel",
     "nur Android, Entwickleroptionen und USB-Debugging nötig, zusätzliches Programm – als Ergänzung, nicht als "
     "Ersatz"),
    ("D", "Explorer öffnen",
     "Gerät in „Dieser PC“ im Explorer öffnen (wie 57 A).", "sehr gering", "kein Arbeiten in QFiles"),
]

F58_B_DETAIL = [
    "Geräteliste: <font face='DVM'>IPortableDeviceManager::GetDevices</font>, Anzeigename und Hersteller je "
    "Gerät; Änderungen (Anstecken/Abziehen) über <font face='DVM'>WM_DEVICECHANGE</font> bzw. "
    "WPD-Ereignisse – wie die Laufwerksleiste heute auf neue Laufwerke reagiert.",
    "Verbinden: <font face='DVM'>IPortableDevice::Open</font> mit Client-Angaben (Name „QFiles“, Version). "
    "Speicher (Interner Speicher, SD-Karte) sind die oberste Ebene unter dem Gerät.",
    "Auflisten: <font face='DVM'>IPortableDeviceContent::EnumObjects</font>, Eigenschaften "
    "(<font face='DVM'>WPD_OBJECT_ORIGINAL_FILE_NAME</font>, <font face='DVM'>WPD_OBJECT_SIZE</font>, "
    "<font face='DVM'>WPD_OBJECT_DATE_MODIFIED</font>, Inhaltstyp Ordner/Datei) gesammelt über "
    "<font face='DVM'>IPortableDevicePropertiesBulk</font>. Ein Zwischenspeicher Pfad → Objektkennung vermeidet "
    "wiederholtes Suchen; Auflisten im Hintergrund wie bei FTP.",
    "Herunterladen: <font face='DVM'>IPortableDeviceResources::GetStream</font> (WPD_RESOURCE_DEFAULT) in eine "
    "lokale Datei. Hochladen: <font face='DVM'>CreateObjectWithPropertiesAndData</font>; Ordner: "
    "<font face='DVM'>CreateObjectWithPropertiesOnly</font>; Löschen: <font face='DVM'>Delete</font>; Umbenennen: "
    "<font face='DVM'>SetValues</font> auf den Dateinamen, wenn das Gerät es erlaubt (sonst klare Meldung).",
    "Testen: Eine automatische Prüfung auf dem CI-Rechner ist mangels Gerät kaum möglich (die WPD-Beispieltreiber "
    "von Microsoft ließen sich voraussichtlich nur mit Testsignierung installieren); getestet wird von Hand mit "
    "einem Android-Smartphone und einer Kamera. Auch unter Wine ist WPD nicht prüfbar.",
]

RECOMMEND = [
    ("1", "Papierkorb-Ansicht (57 B, über die Shell)",
     "Größter Nutzen bei überschaubarem Aufwand; schließt die Lücke zu Idoswin. Dazu „Löschen rückgängig“. "
     "Im CI-Test prüfbar (Datei löschen, im Papierkorb finden, wiederherstellen)."),
    ("2", "Explorer-Absprünge für weitere Windows-Ordner (57 A)",
     "Bibliotheken, Systemsteuerung, „Dieser PC“ ohne eigenen Code für deren Inhalt."),
    ("3", "Mobilgeräte über eigenen WPD-Zugriff (58 B)",
     "Passt zur vorhandenen FTP/SFTP/WebDAV-Schicht; Geräte unter einem Baumknoten „Geräte“ und in der "
     "Laufwerksleiste. Erfüllt den häufigsten Anwendungsfall (Fotos vom Handy kopieren) mit QFiles-eigenen "
     "Dialogen."),
    ("4", "Allgemeiner Shell-Namensraum (57 C) – nur bei Bedarf",
     "Erst sinnvoll, wenn nach 1–3 noch Shell-Ordner fehlen, die man in QFiles bearbeiten will. Hoher "
     "Umbauaufwand, weil Listen, Verlauf, Lesezeichen und Dateioperationen heute auf Pfaden bzw. URLs beruhen."),
]

SOURCES = [
    ("Idoswin Pro – Aktuelles (Versionsgeschichte 5.x)", "https://www.idoswin.de/idosnews.htm"),
    ("Idoswin Free – Änderungen &amp; neue Funktionen (3.5/3.6)", "http://www.idoswin.de/iw32v2.htm"),
    ("Idoswin Pro – Programmfunktionen", "https://www.idoswin.de/idosfunc.htm"),
    ("SpeedCommander – Funktionen im Detail", "https://www.speedproject.de/sc/funktionen/"),
    ("SpeedCommander – Was ist neu?", "https://www.speedproject.de/neu/"),
    ("SpeedCommander – Versionsgeschichte", "https://www.speedproject.de/sc/versionsgeschichte/"),
    ("SpeedCommander-Hilfe – Zugriff auf Tablet und Smartphone",
     "https://www.speedproject.de/hilfe/zugriff-auf-tablet-und-smartphone/"),
    ("SpeedCommander-Hilfe – Baumansicht nur für das aktuelle Laufwerk",
     "https://www.speedproject.de/hilfe/baumansicht-nur-fuer-das-aktuelle-laufwerk/"),
    ("QFiles – Quelltext " + QF_VERSION + " (FilePane, Location, RemoteFs, FileOps, ShellMenu, Network)",
     "https://github.com/kpentzlin/QFiles"),
]


# ---------------------------------------------------------------------------------------------------------------
# Satz

def main(out):
    pdfmetrics.registerFont(TTFont("DV", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"))
    pdfmetrics.registerFont(TTFont("DVB", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"))
    pdfmetrics.registerFont(TTFont("DVI", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf"))
    pdfmetrics.registerFont(TTFont("DVM", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"))
    from reportlab.lib.fonts import addMapping
    addMapping("DV", 0, 0, "DV")
    addMapping("DV", 1, 0, "DVB")
    addMapping("DV", 0, 1, "DVI")
    addMapping("DV", 1, 1, "DVB")

    base = ParagraphStyle("b", fontName="DV", fontSize=8.8, leading=12.2, alignment=TA_LEFT)
    small = ParagraphStyle("s", parent=base, fontSize=7.8, leading=10.2)
    cell = ParagraphStyle("c", parent=base, fontSize=7.8, leading=10)
    cellh = ParagraphStyle("ch", parent=cell, fontName="DVB")
    bullet = ParagraphStyle("bl", parent=base, leftIndent=10, bulletIndent=0, spaceAfter=2.5)
    title = ParagraphStyle("t", parent=base, fontName="DVB", fontSize=17, leading=21, spaceAfter=3)
    sub = ParagraphStyle("st", parent=base, fontSize=9.5, leading=13, textColor=colors.HexColor("#333333"))
    h1 = ParagraphStyle("h1", parent=base, fontName="DVB", fontSize=13, leading=17, spaceBefore=9, spaceAfter=4,
                        textColor=colors.HexColor("#1f3b5c"))
    h2 = ParagraphStyle("h2", parent=base, fontName="DVB", fontSize=10.2, leading=14, spaceBefore=6, spaceAfter=3)

    W = A4[0] - 36 * mm

    def bullets(items):
        return [Paragraph(t, bullet, bulletText="•") for t in items]

    def grid(rows, widths, header=True, extra=None):
        t = Table(rows, colWidths=widths, repeatRows=1 if header else 0)
        st = [
            ("GRID", (0, 0), (-1, -1), 0.3, GRID),
            ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ("TOPPADDING", (0, 0), (-1, -1), 2.2), ("BOTTOMPADDING", (0, 0), (-1, -1), 2.4),
            ("LEFTPADDING", (0, 0), (-1, -1), 3.5), ("RIGHTPADDING", (0, 0), (-1, -1), 3.5),
        ]
        if header:
            st.append(("BACKGROUND", (0, 0), (-1, 0), HEAD))
        t.setStyle(TableStyle(st + (extra or [])))
        return t

    P = lambda t, s=cell: Paragraph(t, s)

    doc = SimpleDocTemplate(out, pagesize=A4, leftMargin=18 * mm, rightMargin=18 * mm, topMargin=16 * mm,
                            bottomMargin=16 * mm, title="QFiles – Funktionen 57 und 58",
                            subject="Shell-Namensraum und Mobilgeräte (MTP): Umsetzung in Idoswin Pro und "
                                    "SpeedCommander, Möglichkeiten für QFiles", author="QFiles")
    s = []
    s.append(Paragraph("Funktionen 57 und 58: Shell-Namensraum und Mobilgeräte", title))
    today = datetime.date.today().strftime("%d.%m.%Y")
    s.append(Paragraph(f"Ergänzung zu „QFiles Funktionsvergleich.pdf“ · QFiles {QF_VERSION} · Stand {today}", sub))
    s.append(Spacer(1, 4 * mm))
    s.append(Paragraph(INTRO, base))
    s.append(Spacer(1, 3 * mm))

    rows = [[P(h, cellh) for h in ("Nr.", "Funktion", "QFiles", "Idoswin Pro", "SpeedCommander", "Bemerkung")]]
    for r in OVERVIEW:
        rows.append([P(x) for x in r])
    s.append(grid(rows, [10 * mm, 57 * mm, 14 * mm, 21 * mm, 33 * mm, W - 135 * mm],
                  extra=[("BACKGROUND", (0, 1), (0, 2), RED)]))
    s.append(Spacer(1, 3 * mm))
    rows = [[P("<b>" + a + "</b>"), P(b)] for a, b in TERMS]
    s.append(grid(rows, [34 * mm, W - 34 * mm], header=False, extra=[("BACKGROUND", (0, 0), (0, -1), CAT)]))

    # 57
    s.append(Paragraph("1 · Funktion 57 – Shell-Namensraum", h1))
    s.append(Paragraph("1.1 Idoswin Pro: Papierkorb und Netzwerkumgebung", h2))
    s += bullets(F57_IDOSWIN)
    s.append(Paragraph("1.2 SpeedCommander: Desktop als Wurzel, alle Shell-Ordner", h2))
    s += bullets(F57_SC)
    s.append(Paragraph("1.3 Stand in QFiles " + QF_VERSION, h2))
    s += bullets(F57_QFILES)
    s.append(Paragraph("1.4 Möglichkeiten für QFiles", h2))
    rows = [[P(h, cellh) for h in ("", "Möglichkeit", "Umsetzung", "Aufwand", "Nutzen", "Risiko")]]
    for k, n, d, a, nu, ri in F57_OPTIONS:
        rows.append([P("<b>" + k + "</b>"), P("<b>" + n + "</b>"), P(d), P(a), P(nu), P(ri)])
    s.append(grid(rows, [6 * mm, 30 * mm, W - 6 * mm - 30 * mm - 17 * mm - 30 * mm - 25 * mm, 17 * mm, 30 * mm,
                         25 * mm]))
    s.append(Spacer(1, 2.5 * mm))
    s.append(Paragraph("Zwei Wege für die Papierkorb-Ansicht (Möglichkeit B)", h2))
    s += bullets(F57_B_DETAIL)

    # 58
    s.append(Paragraph("2 · Funktion 58 – Smartphones, Tablets, Kameras (MTP)", h1))
    s.append(Paragraph("2.1 Idoswin Pro", h2))
    s += bullets(F58_IDOSWIN)
    s.append(Paragraph("2.2 SpeedCommander: Geräte als Shell-Ordner", h2))
    s += bullets(F58_SC)
    s.append(Paragraph("2.3 Was MTP-Geräte von Laufwerken unterscheidet", h2))
    s += bullets(F58_TECH)
    s.append(Paragraph("2.4 Möglichkeiten für QFiles", h2))
    rows = [[P(h, cellh) for h in ("", "Möglichkeit", "Umsetzung", "Aufwand", "Bewertung")]]
    for k, n, d, a, b in F58_OPTIONS:
        rows.append([P("<b>" + k + "</b>"), P("<b>" + n + "</b>"), P(d), P(a), P(b)])
    s.append(grid(rows, [6 * mm, 30 * mm, W - 6 * mm - 30 * mm - 25 * mm - 42 * mm, 25 * mm, 42 * mm]))
    s.append(Spacer(1, 2.5 * mm))
    s.append(Paragraph("Bausteine für Möglichkeit B (Windows Portable Devices)", h2))
    s += bullets(F58_B_DETAIL)

    # Empfehlung
    s.append(Paragraph("3 · Empfehlung und Reihenfolge", h1))
    rows = [[P(h, cellh) for h in ("Stufe", "Inhalt", "Begründung")]]
    for a, b, c in RECOMMEND:
        rows.append([P("<b>" + a + "</b>"), P("<b>" + b + "</b>"), P(c)])
    s.append(KeepTogether([grid(rows, [12 * mm, 55 * mm, W - 67 * mm])]))
    s.append(Spacer(1, 3 * mm))
    note = Table([[Paragraph(
        "Mit Stufe 1 würde Funktion 57 im Vergleich von „–“ auf „teilw.“ steigen (wie Idoswin), mit Stufe 3 "
        "Funktion 58 auf „ja“. Ein „ja“ bei 57 setzt Stufe 4 voraus.", cell)]], colWidths=[W])
    note.setStyle(TableStyle([("BACKGROUND", (0, 0), (-1, -1), NOTE), ("BOX", (0, 0), (-1, -1), 0.3, GRID),
                              ("LEFTPADDING", (0, 0), (-1, -1), 5), ("TOPPADDING", (0, 0), (-1, -1), 4),
                              ("BOTTOMPADDING", (0, 0), (-1, -1), 4)]))
    s.append(note)

    s.append(Paragraph("Quellen", h1))
    for t, u in SOURCES:
        s.append(Paragraph(f"• {t}: <link href='{u}' color='#1f4e8c'>{u}</link>", small))

    def footer(canvas, d):
        canvas.saveState()
        canvas.setFont("DV", 7)
        canvas.setFillColor(colors.grey)
        canvas.drawString(18 * mm, 9 * mm, "QFiles – Funktionen 57 und 58: Shell-Namensraum und Mobilgeräte")
        canvas.drawRightString(A4[0] - 18 * mm, 9 * mm, f"Seite {d.page}")
        canvas.restoreState()

    doc.build(s, onFirstPage=footer, onLaterPages=footer)
    print(out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "Funktionen57_58.pdf")
