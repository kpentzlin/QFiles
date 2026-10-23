# Erzeugt tools/testdata/exif-vorschau.heic: kleines HEIC-Bild (HEVC) mit EXIF-Vorschaubild (JPEG in IFD1).
# Benötigt pillow und pillow-heif (nur zum Erzeugen; das Ergebnis liegt im Repository).
import io
import os
import struct

import pillow_heif
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))


def exif_with_thumbnail(jpeg):
    # TIFF (Little Endian): IFD0 mit Orientation, IFD1 mit JPEGInterchangeFormat/-Length
    ifd0_off = 8
    ifd0 = struct.pack("<H", 1) + struct.pack("<HHII", 0x0112, 3, 1, 1)
    ifd1_off = ifd0_off + len(ifd0) + 4
    n1 = 3
    ifd1_size = 2 + 12 * n1 + 4
    jpeg_off = ifd1_off + ifd1_size
    ifd1 = struct.pack("<H", n1) + struct.pack("<HHII", 0x0103, 3, 1, 6) + \
        struct.pack("<HHII", 0x0201, 4, 1, jpeg_off) + struct.pack("<HHII", 0x0202, 4, 1, len(jpeg)) + \
        struct.pack("<I", 0)
    return b"II*\x00" + struct.pack("<I", ifd0_off) + ifd0 + struct.pack("<I", ifd1_off) + ifd1 + jpeg


def main():
    img = Image.new("RGB", (320, 240), (40, 120, 200))
    d = ImageDraw.Draw(img)
    d.ellipse((60, 40, 260, 200), fill=(250, 210, 40))
    thumb = Image.new("RGB", (160, 120), (200, 40, 40))   # rote Vorschau: unterscheidbar vom Bild
    ImageDraw.Draw(thumb).rectangle((40, 30, 120, 90), fill=(255, 255, 255))
    buf = io.BytesIO()
    thumb.save(buf, "JPEG", quality=80)
    exif = exif_with_thumbnail(buf.getvalue())
    out = os.path.join(HERE, "testdata", "exif-vorschau.heic")
    heif = pillow_heif.from_pillow(img)
    heif.save(out, quality=60, exif=exif)
    print(out, os.path.getsize(out))
    data = open(out, "rb").read()
    inc = os.path.join(HERE, "..", "tests", "HeicData.inc")
    with open(inc, "w", encoding="utf-8") as f:
        f.write("// Erzeugt mit tools/make-heic-sample.py – nicht von Hand ändern.\n")
        f.write("static const unsigned char kTestHeic[] = {\n")
        for i in range(0, len(data), 24):
            f.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 24]) + ",\n")
        f.write("};\n")


if __name__ == "__main__":
    main()
