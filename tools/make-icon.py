# Erzeugt das frühere Programmsymbol (bis 0.1.13; seit 0.1.14 ist res/QFiles.ico vorgegeben – nicht überschreiben): grüne Sammelmappe, volle Breite, transparenter Hintergrund, deutliche Unterkante.
# Aufruf: python tools/make-icon.py [Ausgabe.ico] [Vorschau.png]
import os
import sys

from PIL import Image, ImageDraw

GREEN = (32, 255, 22)          # Grün des bisherigen Symbols
GREEN_BACK = (22, 196, 16)     # Rückwand (dunkler)
GREEN_LIGHT = (150, 255, 140)  # Lichtkante oben an der Vorderklappe
OUTLINE = (6, 70, 4)           # Umriss
BOTTOM = (3, 40, 2)            # Unterkante
PAPER = (255, 255, 250)
PAPER_LINE = (205, 205, 195)

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def render(size):
    ss = 16 if size <= 64 else 4
    W = size * ss
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def P(x, y):
        return (x * W, y * W)

    # Strichstärken: bei kleinen Größen mindestens etwa 1 Pixel
    ow = max(1.0, size / 32) * ss              # Umriss
    bw = max(1.6, size / 22) * ss              # Unterkante (deutlich)
    x0, x1 = 0.5 / size + ow / W / 2, 1 - 0.5 / size - ow / W / 2   # volle Breite
    top, tabTop, bottom = 0.20, 0.11, 0.90

    # Rückwand mit Reiter
    back = [P(x0, tabTop + 0.03), P(x0 + 0.03, tabTop), P(0.38, tabTop), P(0.45, top), P(x1 - 0.03, top),
            P(x1, top + 0.03), P(x1, bottom), P(x0, bottom)]
    d.polygon(back, fill=GREEN_BACK + (255,), outline=OUTLINE + (255,), width=round(ow))

    # Blätter (ab 24 px)
    if size >= 24:
        d.polygon([P(0.13, 0.27), P(0.86, 0.24), P(0.88, 0.52), P(0.14, 0.55)], fill=PAPER + (255,),
                  outline=PAPER_LINE + (255,), width=max(1, round(ow / 2)))
        if size >= 40:
            for k in range(3):
                y = 0.32 + k * 0.055
                d.line([P(0.22, y + 0.004), P(0.76, y - 0.006)], fill=PAPER_LINE + (255,), width=max(1, round(ow / 2)))

    # Vorderklappe (leicht nach vorn geneigt), volle Breite unten
    ft = 0.40
    front = [P(x0 + 0.05, ft), P(x1, ft), P(x1, bottom), P(x0, bottom)]
    d.polygon(front, fill=GREEN + (255,), outline=OUTLINE + (255,), width=round(ow))
    # Lichtkante
    if size >= 24:
        d.line([P(x0 + 0.07, ft + 0.035), P(x1 - 0.03, ft + 0.035)], fill=GREEN_LIGHT + (255,), width=max(1, round(ow)))
    # deutliche Unterkante
    yb = bottom * W - bw / 2 + ow / 2
    d.line([(x0 * W, yb), (x1 * W, yb)], fill=BOTTOM + (255,), width=round(bw))

    # vertikal mittig: Inhalt reicht von tabTop bis bottom
    img = img.resize((size, size), Image.LANCZOS)
    shift = round(((1 - bottom) - tabTop) / 2 * size)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(img, (0, shift))
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ico = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "res", "QFiles.ico")
    imgs = {s: render(s) for s in SIZES}
    imgs[256].save(ico, format="ICO", sizes=[(s, s) for s in SIZES],
                   append_images=[imgs[s] for s in SIZES if s != 256])
    if len(sys.argv) > 2:
        sheet = Image.new("RGBA", (sum(SIZES) + 10 * len(SIZES) + 10, 270), (255, 255, 255, 255))
        x = 10
        for s in SIZES:
            sheet.alpha_composite(imgs[s], (x, 10))
            x += s + 10
        dark = Image.new("RGBA", sheet.size, (40, 40, 40, 255))
        x = 10
        for s in SIZES:
            dark.alpha_composite(imgs[s], (x, 10))
            x += s + 10
        both = Image.new("RGBA", (sheet.width, 540))
        both.paste(sheet, (0, 0))
        both.paste(dark, (0, 270))
        both.save(sys.argv[2])
    print(ico)


if __name__ == "__main__":
    main()
