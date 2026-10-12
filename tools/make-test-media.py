# Erzeugt Testdateien für die PDF- und Video-Anzeige (ohne Fremdbibliotheken).
# Aufruf: python tools/make-test-media.py <Zielverzeichnis>
#
# PDFs mit großen Seitennummern (Hoch-/Querformat gemischt) und unkomprimierte AVI-Videos, deren Einzelbilder die
# Zeit in Zehntelsekunden als große Ziffern zeigen (so lässt sich prüfen, welche Bilder angezeigt werden).
import os
import struct
import sys

A4 = (595, 842)
A4Q = (842, 595)


def make_pdf(path, sizes):
    objs = []

    def add(body):
        objs.append(body)
        return len(objs)

    font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>")
    pages_index = len(objs) + 1
    objs.append(None)  # Platzhalter für /Pages
    kids = []
    for i, (w, h) in enumerate(sizes, 1):
        text = ("BT /F1 %d Tf %d %d Td (%d) Tj ET\n" % (min(w, h) // 2, w // 3, h // 3, i)).encode()
        frame = ("4 w 20 20 %d %d re S\n" % (w - 40, h - 40)).encode()
        content = frame + text
        cobj = add(b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream")
        kids.append(add(("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] /Resources << /Font << /F1 %d 0 R >> >> "
                         "/Contents %d 0 R >>" % (pages_index, w, h, font, cobj)).encode()))
    objs[pages_index - 1] = ("<< /Type /Pages /Kids [%s] /Count %d >>"
                             % (" ".join("%d 0 R" % k for k in kids), len(kids))).encode()
    catalog = add(("<< /Type /Catalog /Pages %d 0 R >>" % pages_index).encode())
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for n, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % n + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offsets:
        out += b"%010d 00000 n \n" % o
    out += b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, catalog, xref)
    with open(path, "wb") as f:
        f.write(out)


# 5×7-Ziffern
GLYPHS = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    ".": ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    "s": ["00000", "00000", "01111", "10000", "01110", "00001", "11110"],
}


def frame_rgb(w, h, label, hue):
    # Hintergrundfarbe je Bild, darauf weiße Ziffern (BGR, Zeilen von unten nach oben)
    bg = [(hue * 37) % 200 + 30, (hue * 73) % 200 + 30, (hue * 151) % 200 + 30]
    img = [[list(bg) for _ in range(w)] for _ in range(h)]
    scale = max(2, min(w // (len(label) * 6 + 2), h // 9))
    x0 = (w - len(label) * 6 * scale) // 2
    y0 = (h - 7 * scale) // 2
    for ci, ch in enumerate(label):
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(row):
                if bit == "1":
                    for yy in range(scale):
                        for xx in range(scale):
                            x = x0 + (ci * 6 + gx) * scale + xx
                            y = y0 + gy * scale + yy
                            if 0 <= x < w and 0 <= y < h:
                                img[y][x] = [255, 255, 255]
    stride = (w * 3 + 3) & ~3
    data = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for px in img[y]:
            row += bytes(px)
        row += b"\0" * (stride - w * 3)
        data += row
    return bytes(data)


def chunk(fourcc, data):
    pad = b"\0" if len(data) % 2 else b""
    return fourcc + struct.pack("<I", len(data)) + data + pad


def make_avi(path, w, h, fps, seconds):
    n = int(fps * seconds)
    frames = [frame_rgb(w, h, "%.1fs" % (i / fps), i) for i in range(n)]
    size = len(frames[0])
    avih = struct.pack("<IIIIIIIIII16x", 1000000 // fps, size * fps, 0, 0x10, n, 0, 1, size, w, h)
    strh = struct.pack("<4s4sIHHIIIIIIIIhhhh", b"vids", b"DIB ", 0, 0, 0, 0, 1, fps, 0, n, size, 0xFFFFFFFF, 0,
                       0, 0, w, h)
    strf = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, size, 0, 0, 0, 0)
    hdrl = b"hdrl" + chunk(b"avih", avih) + chunk(b"LIST", b"strl" + chunk(b"strh", strh) + chunk(b"strf", strf))
    movi = bytearray(b"movi")
    index = bytearray()
    for f in frames:
        index += struct.pack("<4sIII", b"00db", 0x10, len(movi), len(f))
        movi += chunk(b"00db", f)
    body = b"AVI " + chunk(b"LIST", hdrl) + chunk(b"LIST", bytes(movi)) + chunk(b"idx1", bytes(index))
    with open(path, "wb") as fh:
        fh.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    make_pdf(os.path.join(out, "a-1 Seite.pdf"), [A4])
    make_pdf(os.path.join(out, "b-3 Seiten hoch.pdf"), [A4, A4, A4])
    make_pdf(os.path.join(out, "c-5 Seiten hoch.pdf"), [A4] * 5)
    make_pdf(os.path.join(out, "d-4 Seiten gemischt.pdf"), [A4Q, A4, A4, A4])
    make_pdf(os.path.join(out, "e-2 Seiten.pdf"), [A4, A4])
    make_avi(os.path.join(out, "f-quer.avi"), 192, 108, 10, 6)
    make_avi(os.path.join(out, "g-hoch.avi"), 108, 192, 10, 6)
    make_avi(os.path.join(out, "h-kurz.avi"), 192, 108, 10, 1.5)


if __name__ == "__main__":
    main()
