# Erzeugt Testdateien für die Kachelansicht (Miniaturen): viele PNG-Bilder mit langen Namen, Videos (AVI) und ein
# HEIC-Bild mit EXIF-Vorschaubild. Ohne Fremdbibliotheken. Aufruf: python tools/make-test-thumbs.py <Verzeichnis>
import colorsys
import os
import shutil
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from importlib import import_module

media = import_module("make-test-media")


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "QFilesKacheln"
    os.makedirs(out, exist_ok=True)
    for i in range(150):
        r, g, b = colorsys.hsv_to_rgb((i * 0.07) % 1.0, 0.6, 0.9)
        name = "Urlaubsfoto vom Strand in Italien Nummer %03d.png" % i if i % 3 else "Bild %03d.png" % i
        png(os.path.join(out, name), 120 + (i % 5) * 20, 90, (int(r * 255), int(g * 255), int(b * 255)))
    # Handy-Dateinamen ohne Leerzeichen (GUID): Beschriftung einzeilig, Name gekürzt, Endung bleibt
    for i, guid in enumerate(["3BADD6DD-ED1C-4F1E-BC1C-336024F6A1B2", "3BADD6DD-ED1C-4F1E-BC1C-336024F6A1B3",
                              "5BB0B9C8-2F0D-4349-A1D7-4DF152902C44"]):
        png(os.path.join(out, guid + ".png"), 160, 120, (200, 140 + 30 * i, 60))
    media.make_avi(os.path.join(out, "Video mit einem recht langen Namen.avi"), 192, 108, 10, 2)
    media.make_avi(os.path.join(out, "Kurz.avi"), 108, 192, 10, 1)
    shutil.copy(os.path.join(HERE, "testdata", "exif-vorschau.heic"), os.path.join(out, "Handyfoto.heic"))
    print(out)


if __name__ == "__main__":
    main()
