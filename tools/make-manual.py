# Erzeugt „QFiles.pdf“: Gebrauchsanleitung und Programmbeschreibung, bebildert mit den Bildschirmfotos des
# automatischen GUI-Tests (tools/ci-guitest.ps1, Zweig „ci-screenshot“).
#
# Aufruf: python tools/make-manual.py [Ausgabedatei] [Verzeichnis mit screenshot*.png]
#   Vorgaben: QFiles.pdf und docs/shots. Die Bildschirmfotos lassen sich so holen:
#     git fetch origin ci-screenshot && mkdir -p docs/shots &&
#     for f in $(git ls-tree --name-only origin/ci-screenshot | grep png); do git show origin/ci-screenshot:$f > docs/shots/$f; done
# Benötigt reportlab, Pillow und die Schriften DejaVu Sans / DejaVu Sans Mono.
#
# Inhalt: Titelseite, Inhaltsverzeichnis, „Erste Schritte“ (hier im Skript), danach README.md (Überschriften,
# Absätze, Listen, Tabellen, Code). Die Bilder stehen am Ende des jeweiligen README-Abschnitts (Tabelle FIGURES).
# Bei jeder Programmänderung neu erzeugen (Vorgabe von Karl, 09.11.2026).
import datetime
import io
import os
import re
import sys

from PIL import Image as PILImage
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.fonts import addMapping
from reportlab.platypus import (BaseDocTemplate, Frame, Image, KeepTogether, PageBreak, PageTemplate, Paragraph,
                                Preformatted, Spacer, Table, TableStyle, CondPageBreak)
from reportlab.platypus.tableofcontents import TableOfContents

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = "/usr/share/fonts/truetype/dejavu"

# Bildschirmfotos je README-Abschnitt: (Datei, Bildunterschrift, Ausschnitt oder None = ganzer Bildschirm)
WIN = (6, 6, 994, 700)   # QFiles-Fenster auf dem 1024×768-Bildschirm des Testrechners (ohne Taskleiste)
FULL = (0, 0, 1024, 728)
FIGURES = {
    "Filter (Trichter in der Kopfzeile)": [
        ("screenshot40.png", "Filterauswahl: Masken „*.docx / *.xlsx“, Verzeichnisse „ohne“", WIN),
        ("screenshot41.png", "Aktiver Filter: Verzeichnisfeld hellrot mit Kurztext, Trichter hinterlegt", WIN),
        ("screenshot42.png", "Filterauswahl mit „.gitignore“ (gefundene Datei darunter) und Sortiervorgabe", WIN),
        ("screenshot43.png", "Git-Filter: Verzeichnisfeld hell-lila; das neue Lesezeichen „Projekt“ zeigt den Trichter", WIN),
        ("screenshot44.png", "Lesezeichen mit Filter geöffnet: Ansicht „Details“, sortiert nach Änderungsdatum absteigend", WIN),
    ],
    "Split-Funktion": [
        ("screenshot2.png", "Rechte Spalte geteilt: oben die Dateianzeige der links gewählten Datei, unten Liste 4", WIN),
    ],
    "Dateianzeige „in dem jeweils anderen Fenster“": [
        ("screenshot13.png", "PDF-Datei mit fünf Seiten in der Dateianzeige (F11)", WIN),
        ("screenshot15.png", "Video in der Dateianzeige mit Auflösung und Dauer", WIN),
    ],
    "Oberfläche": [
        ("screenshot6.png", "Optionen, Seite „Anzeige“", WIN),
        ("screenshot10.png", "Ansicht „Mit Unterverzeichnissen“ (Strg+Umschalt+5)", WIN),
        ("screenshot30.png", "Ansicht „Miniaturen“ (Kacheln)", WIN),
        ("screenshot32.png", "Werkzeugleiste anpassen", WIN),
        ("screenshot34.png", "Markieren durch Ziehen: Auswahlrahmen neben den Namen", WIN),
        ("screenshot39.png", "Mittlere Maustaste auf einer Datei: Auswahl „Öffnen mit“", WIN),
    ],
    "Favoriten": [
        ("screenshot24.png", "Favoriten in Liste 1 (hellgelb, mit Spalte „Verzeichnis“)", WIN),
    ],
    "Papierkorb": [
        ("screenshot28.png", "Ansicht „Papierkorb“ mit ursprünglichem Ort und Löschzeitpunkt", WIN),
        ("screenshot29.png", "Kontextmenü im Papierkorb", WIN),
    ],
    "Dateioperationen": [
        ("screenshot3.png", "Kontextmenü einer Datei: eigene Befehle oben, darunter das Explorer-Kontextmenü", FULL),
        ("screenshot22.png", "Ziehen in den Leerraum hinter einem Verzeichnisnamen: Abfrage des Ziels", WIN),
        ("screenshot25.png", "Namenskonflikt beim Kopieren: Ersetzen, mit neuem Namen ablegen oder abbrechen", WIN),
    ],
    "Netzwerk": [
        ("screenshot9.png", "Netzwerk durchsuchen", WIN),
    ],
    "FTP, SFTP und WebDAV": [
        ("screenshot8.png", "Links die Freigaben von \\\\localhost, rechts ein FTP-Zugang", WIN),
    ],
    "Anzeige und Hex-Editor": [
        ("screenshot17.png", "ZIP-Archiv als Liste in der Dateianzeige", WIN),
        ("screenshot21.png", "Eigenes Anzeigefenster (Kontextmenü „Anzeigen“)", WIN),
        ("screenshot27.png", "Programmdatei in der Dateianzeige: Symbol und Versionsinformationen", WIN),
    ],
    "Bedienungshilfen vereinfachen (0.1.21)": [
        ("screenshot45.png", "Optionen, Seite „Bedienung“, unten die Option „Bedienungshilfen vereinfachen“", WIN),
    ],
    "Kontextmenü-Erweiterungen ausschließen": [
        ("screenshot5.png", "Abgefangener Fehler einer Kontextmenü-Erweiterung", WIN),
    ],
}

# Kapitel „Erste Schritte“ (vor dem README-Inhalt)
FIRST_STEPS = """## Erste Schritte

### Starten

QFiles wird nicht installiert: `QFiles.exe` in ein beliebiges Verzeichnis legen und starten. Die Einstellungen
stehen in `%ProgramData%\\QFiles\\QFiles.ini` (siehe Kapitel „Einstellungen“). Beim Start lassen sich die
Verzeichnisse der Listen 1 und 2 angeben, z. B. `QFiles.exe "C:\\Daten" "D:\\Projekte"`.

### Aufbau des Hauptfensters

- **Menüleiste und Werkzeugleiste** oben; die Werkzeugleiste ist anpassbar (*Ansicht → Werkzeugleiste anpassen*).
- **Verzeichnisbaum** links mit allen Laufwerken und dem Netzwerk.
- **Lesezeichenliste** daneben: ein Klick öffnet das Lesezeichen in der aktiven Liste, ein Klick auf das Symbol
  links davon in der anderen Liste.
- **Zwei Dateilisten** nebeneinander, jede mit Kopfzeile (Laufwerksymbole; rechts Lesezeichen hinzufügen,
  Filter-Trichter und Split) und Pfadzeile (Verzeichnisfeld mit Verlauf, Zurück, Vor, Übergeordnet, Wurzel,
  Verzeichnis wählen und dem Kreis für die aktive Liste). Mit Split (Strg+T) wird jede Liste waagerecht geteilt.
- **Befehlszeile**, **Funktionstastenleiste** (F2 … F11, anklickbar) und **Statuszeile** unten.

### Grundbedienung

- Eingabetaste oder Doppelklick öffnet Verzeichnisse und Dateien, die Rücktaste wechselt eine Ebene höher.
- Tab wechselt zur nächsten Liste; die aktive Liste ist am grün gefüllten Kreis zu erkennen.
- Markieren mit Leertaste/Einfg, mit der Maus oder durch Ziehen eines Rahmens neben den Namen.
- Kopieren und Verschieben in die andere Liste mit Umschalt+F5 bzw. Umschalt+F6 oder per Drag & Drop.
- F11 zeigt die gewählte Datei in der anderen Liste an (Text, Bilder, PDF, Videos, Archive, Programmdateien).
- Der Trichter in der Kopfzeile (Strg+Umschalt+F) filtert die Liste nach Masken, Datum, Größe, Attributen oder
  „.gitignore“ und gibt eine Sortierung vor.
- F1 zeigt alle Tastenkürzel.
"""

SKIP_SECTIONS = set()   # README-Abschnitte, die im Handbuch fehlen sollen (derzeit keine)


def version():
    with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(QFiles VERSION ([0-9.]+)", f.read())
    return m.group(1) if m else "?"


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def inline(text):
    """Markdown-Inline (Code, fett, kursiv, Links) in reportlab-Absatzauszeichnung."""
    out = []
    pos = 0
    for m in re.finditer(r"`([^`]+)`", text):
        out.append(("t", text[pos:m.start()]))
        out.append(("c", m.group(1)))
        pos = m.end()
    out.append(("t", text[pos:]))
    res = ""
    for kind, s in out:
        if kind == "c":
            res += '<font face="DVM" color="#7a2d00">' + esc(s) + "</font>"
            continue
        s = esc(s)
        s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", lambda m: m.group(1), s)
        s = re.sub(r"&lt;(https?://[^&]+)&gt;", r'<link href="\1" color="#1a4fa0">\1</link>', s)
        s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
        s = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<i>\1</i>", s)
        res += s
    # Links mit Code im Text: [`x`](y) -> Code ohne Klammern
    res = re.sub(r"\[(<font[^>]*>[^<]*</font>)\]\([^)]*\)", r"\1", res)
    return res


class ManualDoc(BaseDocTemplate):
    def __init__(self, filename, ver, **kw):
        super().__init__(filename, **kw)
        self.ver = ver
        frame = Frame(self.leftMargin, self.bottomMargin, self.width, self.height, id="f")
        self.addPageTemplates([PageTemplate(id="title", frames=[frame], onPage=self.no_footer),
                               PageTemplate(id="body", frames=[frame], onPage=self.footer)])

    def no_footer(self, canvas, doc):
        pass

    def footer(self, canvas, doc):
        canvas.saveState()
        canvas.setFont("DV", 8)
        canvas.setFillColor(colors.HexColor("#666666"))
        canvas.drawString(self.leftMargin, 12 * mm, f"QFiles {self.ver} – Gebrauchsanleitung und Programmbeschreibung")
        canvas.drawRightString(self.leftMargin + self.width, 12 * mm, f"Seite {doc.page}")
        canvas.setStrokeColor(colors.HexColor("#cccccc"))
        canvas.line(self.leftMargin, 15 * mm, self.leftMargin + self.width, 15 * mm)
        canvas.restoreState()

    def afterFlowable(self, flowable):
        if isinstance(flowable, Paragraph) and getattr(flowable, "toc_level", None) is not None:
            text = flowable.getPlainText()
            key = "h%d" % id(flowable)
            self.canv.bookmarkPage(key)
            self.canv.addOutlineEntry(text, key, level=flowable.toc_level, closed=flowable.toc_level > 0)
            self.notify("TOCEntry", (flowable.toc_level, text, self.page, key))


def build(out, shots):
    for name, file in (("DV", "DejaVuSans.ttf"), ("DVB", "DejaVuSans-Bold.ttf"), ("DVI", "DejaVuSans-Oblique.ttf"),
                       ("DVBI", "DejaVuSans-BoldOblique.ttf"), ("DVM", "DejaVuSansMono.ttf")):
        pdfmetrics.registerFont(TTFont(name, os.path.join(FONT_DIR, file)))
    addMapping("DV", 0, 0, "DV")
    addMapping("DV", 1, 0, "DVB")
    addMapping("DV", 0, 1, "DVI")
    addMapping("DV", 1, 1, "DVBI")

    ver = version()
    blue = colors.HexColor("#1a3f73")
    body = ParagraphStyle("body", fontName="DV", fontSize=9.4, leading=12.6, spaceAfter=5)
    h1 = ParagraphStyle("h1", parent=body, fontName="DVB", fontSize=17, leading=21, spaceBefore=4, spaceAfter=10,
                        textColor=blue)
    h2 = ParagraphStyle("h2", parent=body, fontName="DVB", fontSize=12.5, leading=16, spaceBefore=10, spaceAfter=5,
                        textColor=blue)
    cell = ParagraphStyle("cell", parent=body, fontSize=8.4, leading=10.6, spaceAfter=0)
    cellb = ParagraphStyle("cellb", parent=cell, fontName="DVB")
    caption = ParagraphStyle("cap", parent=body, fontName="DVI", fontSize=8.4, leading=10.6, alignment=TA_CENTER,
                             textColor=colors.HexColor("#444444"), spaceBefore=3, spaceAfter=10)
    code = ParagraphStyle("code", fontName="DVM", fontSize=8, leading=10, backColor=colors.HexColor("#f3f3f3"),
                          borderPadding=4, spaceBefore=4, spaceAfter=8, leftIndent=4)

    doc = ManualDoc(out, ver, pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm, topMargin=18 * mm,
                    bottomMargin=22 * mm, title="QFiles – Gebrauchsanleitung und Programmbeschreibung",
                    author="QFiles", subject=f"QFiles {ver}")
    story = []
    fig_no = [0]
    used = set()

    # Titelseite
    try:
        ico = PILImage.open(os.path.join(ROOT, "res", "QFiles.ico"))
        ico.size = max(ico.info.get("sizes", [ico.size]))
        buf = io.BytesIO()
        ico.convert("RGBA").save(buf, "PNG")
        buf.seek(0)
        story += [Spacer(1, 40 * mm), Image(buf, 42 * mm, 42 * mm), Spacer(1, 10 * mm)]
    except Exception:
        story.append(Spacer(1, 90 * mm))
    tstyle = ParagraphStyle("t", parent=body, fontName="DVB", fontSize=30, leading=36, alignment=TA_CENTER, textColor=blue)
    sstyle = ParagraphStyle("s", parent=body, fontSize=14, leading=19, alignment=TA_CENTER)
    story.append(Paragraph("QFiles", tstyle))
    story.append(Spacer(1, 4 * mm))
    story.append(Paragraph("Gebrauchsanleitung und Programmbeschreibung", sstyle))
    story.append(Spacer(1, 10 * mm))
    story.append(Paragraph(f"Version {ver} · Stand {datetime.date.today().strftime('%d.%m.%Y')}",
                           ParagraphStyle("v", parent=body, fontSize=11, alignment=TA_CENTER)))
    story.append(Spacer(1, 50 * mm))
    story.append(Paragraph("Dateimanager für Windows (64 Bit), nachempfunden Idoswin Pro. Erzeugt aus README.md und "
                           "den Bildschirmfotos des automatischen Oberflächentests (Windows Server 2025, 1024×768).",
                           ParagraphStyle("n", parent=body, fontSize=8.5, alignment=TA_CENTER,
                                          textColor=colors.HexColor("#555555"))))
    from reportlab.platypus.doctemplate import NextPageTemplate
    story += [NextPageTemplate("body"), PageBreak()]

    # Inhaltsverzeichnis
    toc = TableOfContents()
    toc.levelStyles = [ParagraphStyle("toc0", parent=body, fontName="DVB", fontSize=10, leading=14, leftIndent=0),
                       ParagraphStyle("toc1", parent=body, fontSize=9, leading=11.5, leftIndent=12)]
    toc.dotsMinLevel = 0
    story += [Paragraph("Inhalt", h1), toc, PageBreak()]

    def heading(text, level):
        p = Paragraph(inline(text), h1 if level == 0 else h2)
        p.toc_level = level
        return p

    def figure(file, cap, box):
        path = os.path.join(shots, file)
        if not os.path.exists(path):
            return []
        im = PILImage.open(path).convert("RGB")
        if box and im.size == (1024, 768):
            im = im.crop(box)
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=82, optimize=True)
        buf.seek(0)
        w = doc.width
        h = w * im.size[1] / im.size[0]
        fig_no[0] += 1
        used.add(file)
        return [KeepTogether([Image(buf, w, h), Paragraph(f"Abbildung {fig_no[0]}: {esc(cap)}", caption)])]

    def render(md, section_hook):
        lines = md.split("\n")
        i = 0
        para = []
        current = None

        def flush():
            if para:
                story.append(Paragraph(inline(" ".join(x.strip() for x in para)), body))
                para.clear()

        def end_section():
            if current and current in FIGURES:
                for f, c, b in FIGURES[current]:
                    story.extend(figure(f, c, b))

        skipping = False
        while i < len(lines):
            line = lines[i]
            m = re.match(r"^(#{1,3})\s+(.*)$", line)
            if m:
                flush()
                end_section()
                level = len(m.group(1))
                text = m.group(2).strip()
                current = text
                skipping = text in SKIP_SECTIONS
                if level == 1:
                    current = None   # Programmname: Titelseite
                elif not skipping:
                    if level == 2:
                        story.append(CondPageBreak(60 * mm))
                    story.append(heading(text, 0 if level == 2 else 1))
                i += 1
                continue
            if skipping:
                i += 1
                continue
            if line.startswith("```"):
                flush()
                j = i + 1
                buf = []
                while j < len(lines) and not lines[j].startswith("```"):
                    buf.append(lines[j])
                    j += 1
                story.append(Preformatted("\n".join(buf), code))
                i = j + 1
                continue
            if re.match(r"^\s*---+\s*$", line):
                flush()
                i += 1
                continue
            im = re.match(r"^!\[([^\]]*)\]\(([^)]+)\)", line.strip())
            if im:
                flush()
                f = os.path.basename(im.group(2))
                if os.path.exists(os.path.join(shots, f)):
                    story.extend(figure(f, im.group(1), WIN))
                # folgende kursive Bildunterschrift überspringen
                k = i + 1
                while k < len(lines) and not lines[k].strip():
                    k += 1
                if k < len(lines) and lines[k].startswith("*") and lines[k].rstrip().endswith("*"):
                    i = k + 1
                else:
                    i += 1
                continue
            if line.startswith("|"):
                flush()
                rows = []
                while i < len(lines) and lines[i].startswith("|"):
                    cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                    if not all(re.match(r"^:?-+:?$", c) for c in cells):
                        rows.append(cells)
                    i += 1
                n = max(len(r) for r in rows)
                data = []
                for r_i, r in enumerate(rows):
                    r = r + [""] * (n - len(r))
                    data.append([Paragraph(inline(c), cellb if r_i == 0 else cell) for c in r])
                widths = [doc.width * 0.3, doc.width * 0.7] if n == 2 else [doc.width / n] * n
                t = Table(data, colWidths=widths, repeatRows=1)
                t.setStyle(TableStyle([
                    ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#dde6f3")),
                    ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#b8c4d6")),
                    ("VALIGN", (0, 0), (-1, -1), "TOP"),
                    ("TOPPADDING", (0, 0), (-1, -1), 2.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 2.5),
                    ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f7f9fc")]),
                ]))
                story.extend([t, Spacer(1, 6)])
                continue
            lm = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", line)
            if lm:
                flush()
                indent = len(lm.group(1))
                marker = lm.group(2)
                text = [lm.group(3)]
                j = i + 1
                while j < len(lines) and lines[j].strip() and not re.match(r"^\s*([-*]|\d+\.)\s+", lines[j]) \
                        and not lines[j].startswith("|") and not lines[j].startswith("#") \
                        and len(lines[j]) - len(lines[j].lstrip()) > indent:
                    text.append(lines[j].strip())
                    j += 1
                level = indent // 2
                bullet = "•" if marker in "-*" else marker
                st = ParagraphStyle("li%d" % level, parent=body, leftIndent=12 + level * 14, bulletIndent=2 + level * 14,
                                    spaceAfter=2.5)
                story.append(Paragraph(inline(" ".join(text)), st, bulletText=bullet))
                i = j
                if i < len(lines) and not lines[i].strip():
                    story.append(Spacer(1, 3))
                continue
            if not line.strip():
                flush()
                i += 1
                continue
            para.append(line)
            i += 1
        flush()
        end_section()

    with open(os.path.join(ROOT, "README.md"), encoding="utf-8") as f:
        readme = f.read()
    # Einleitung des README als Kapitel „Überblick“, danach „Erste Schritte“
    readme = readme.replace("# QFiles\n", "# QFiles\n\n## Überblick\n", 1)
    m = re.search(r"^## ", readme[readme.index("## Überblick") + 5:], re.M)
    cut = readme.index("## Überblick") + 5 + m.start() if m else len(readme)
    readme = readme[:cut] + FIRST_STEPS + "\n" + readme[cut:]
    render(readme, None)

    doc.multiBuild(story)
    missing = [f for sec in FIGURES.values() for f, _, _ in sec if f not in used]
    print(f"{out}: Version {ver}, {doc.page} Seiten, {fig_no[0]} Abbildungen"
          + (f"; fehlende Bilder: {', '.join(missing)}" if missing else ""))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "QFiles.pdf")
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs", "shots")
    build(out, shots)
