# Draws BooVI's application icon, assets/boovi.ico: a flat violet "B" on the dark sage card,
# the colours of qml/Veyra/Theme.qml (card #17141D, accent #9B7BEA). Pure shapes, no font,
# so the output is the same on every machine. Usage: py scripts/fork/make-icon.py
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
CARD = (0x17, 0x14, 0x1D, 255)
ACCENT = (0x9B, 0x7B, 0xEA, 255)
RIGHT = (False, True, True, False)  # rounded on the right only: the stem side stays square
SIZES = [16, 24, 32, 48, 64, 128, 256]
S = 1024  # drawn large, then reduced per size


def letter_b(draw: ImageDraw.ImageDraw) -> None:
    # The stem and two bowls; the lower bowl a little wider, as in most sans-serif capitals.
    left, top, bottom, stem = 300, 232, 792, 112
    mid = (top + bottom) // 2
    upper_right, lower_right = 664, 712
    draw.rounded_rectangle((left, top, upper_right, mid + stem // 2), radius=(mid + stem // 2 - top) // 2 - 1, corners=RIGHT, fill=ACCENT)
    draw.rounded_rectangle((left, mid - stem // 2, lower_right, bottom), radius=(bottom - mid + stem // 2) // 2 - 1, corners=RIGHT, fill=ACCENT)
    # Counters, punched back out in the card colour.
    draw.rounded_rectangle((left + stem, top + stem, upper_right - stem, mid - stem // 2),
                           radius=(mid - stem // 2 - top - stem) // 2 - 1, corners=RIGHT, fill=CARD)
    draw.rounded_rectangle((left + stem, mid + stem // 2, lower_right - stem, bottom - stem),
                           radius=(bottom - stem - mid - stem // 2) // 2 - 1, corners=RIGHT, fill=CARD)


def main() -> None:
    big = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    draw = ImageDraw.Draw(big)
    draw.rounded_rectangle((32, 32, S - 32, S - 32), radius=200, fill=CARD)
    letter_b(draw)
    out = ROOT / "assets" / "boovi.ico"
    big.resize((256, 256), Image.LANCZOS).save(out, format="ICO", sizes=[(n, n) for n in SIZES])
    big.resize((512, 512), Image.LANCZOS).save(ROOT / "assets" / "boovi.png")
    print(f"wrote {out.relative_to(ROOT)} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
