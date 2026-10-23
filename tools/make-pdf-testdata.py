# Erzeugt tests/PdfTextData.inc: Test-PDFs für die PDF-Textsuche (PdfText.cpp).
# Benötigt reportlab, qpdf und die Schrift DejaVu Sans. Aufruf: python tools/make-pdf-testdata.py
import os
import subprocess
import sys
import tempfile

from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "tests", "PdfTextData.inc")


def simple(path):
    c = canvas.Canvas(path, pagesize=A4, pageCompression=1)
    c.setTitle("Testtitel Quartalsbericht")
    c.setFont("Helvetica", 12)
    c.drawString(72, 750, "Grüße aus Köln – Straße 12")
    c.drawString(72, 730, "Hallo Welt")
    c.showPage()
    c.setFont("Times-Roman", 14)
    c.drawString(72, 750, "Zweite Seite mit €uro")
    c.save()


def truetype(path):
    pdfmetrics.registerFont(TTFont("DV", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"))
    c = canvas.Canvas(path, pagesize=A4, pageCompression=1)
    c.setFont("DV", 11)
    c.drawString(72, 750, "Žluťoučký kůň úpěl ďábelské ódy")
    c.drawString(72, 730, "Ünïcödé ✓ Ελληνικά")
    c.save()


def handmade(path):
    # Unkomprimiert: TJ mit Unterschneidung, Wortabstände über Td, Differences-Kodierung, Formular-XObjekt
    content = (b"BT /F1 12 Tf 72 700 Td [(Gr) 20 (\\374) -10 (\\337e)] TJ 45 0 Td (zusammen) Tj "
               b"0 -20 Td (Erste) Tj 40 0 Td (Zeile) Tj ET "
               b"BT /F2 12 Tf 72 600 Td (ABC) Tj ET /X1 Do")
    form = b"BT /F1 10 Tf 72 500 Td (Text im Formular) Tj ET"
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 /Resources << /Font << /F1 5 0 R /F2 6 0 R >> "
        b"/XObject << /X1 7 0 R >> >> >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding "
        b"/FirstChar 32 /LastChar 255 /Widths [" + b" ".join([b"556"] * 224) + b"] >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        b"/Encoding << /Differences [65 /adieresis /germandbls /uni20AC] >> >>",
        b"<< /Type /XObject /Subtype /Form /BBox [0 0 595 842] /Length %d >>\nstream\n" % len(form) + form +
        b"\nendstream",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offs = []
    for i, o in enumerate(objs, 1):
        offs.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    x = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        out += b"%010d 00000 n \n" % o
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
    with open(path, "wb") as f:
        f.write(out)


def qpdf(args):
    subprocess.run(["qpdf"] + args, check=True)


def main():
    tmp = tempfile.mkdtemp()
    p = lambda n: os.path.join(tmp, n)
    simple(p("einfach.pdf"))
    truetype(p("ttf.pdf"))
    handmade(p("hand.pdf"))
    qpdf(["--allow-weak-crypto", "--encrypt", "", "eigner", "128", "--use-aes=n", "--", p("einfach.pdf"), p("rc4.pdf")])
    qpdf(["--encrypt", "", "eigner", "128", "--use-aes=y", "--", p("einfach.pdf"), p("aes128.pdf")])
    qpdf(["--encrypt", "", "eigner", "256", "--", p("ttf.pdf"), p("aes256.pdf")])
    qpdf(["--object-streams=generate", p("ttf.pdf"), p("objstm.pdf")])
    names = ["einfach", "ttf", "hand", "rc4", "aes128", "aes256", "objstm"]
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("// Erzeugt mit tools/make-pdf-testdata.py – nicht von Hand ändern.\n")
        for n in names:
            data = open(p(n + ".pdf"), "rb").read()
            ident = "kPdf" + n[0].upper() + n[1:]
            f.write("static const unsigned char %s[] = {\n" % ident)
            for i in range(0, len(data), 24):
                f.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 24]) + ",\n")
            f.write("};\n")
    print(OUT)


if __name__ == "__main__":
    sys.exit(main())
