#!/usr/bin/env python3
"""Makes the LiveArea images in sce_sys/ from assets/icon-source.png and the bundled Inter font.

The Vita only shows these when they're 8-bit indexed PNGs, so each is quantized to a palette.
  python3 scripts/make-assets.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FACE = Image.open(ROOT / "assets/icon-source.png").convert("RGBA")
BOLD = str(ROOT / "assets/fonts/Inter-SemiBold.ttf")
REGULAR = str(ROOT / "assets/fonts/Inter-Regular.ttf")
BG = (0, 0, 0)
CARD = (28, 28, 30)


def face(size: int, radius: int) -> Image.Image:
    """The yellow face, cropped square and rounded."""
    w, h = FACE.size
    side = min(w, h)
    square = FACE.crop(((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2))
    square = square.resize((size, size), Image.LANCZOS)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius, fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(square, (0, 0), mask)
    return out


def indexed(img: Image.Image, path: Path) -> None:
    img.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT, dither=Image.NONE).save(path, optimize=True)
    print(f"wrote {path.relative_to(ROOT)} {img.size[0]}x{img.size[1]}")


def icon0() -> None:
    # the bubble on the home screen is round, so the face fills the square
    w, h = FACE.size
    side = min(w, h)
    square = FACE.crop(((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2))
    indexed(square.resize((128, 128), Image.LANCZOS), ROOT / "sce_sys/icon0.png")


def card(size, face_size, left, gap, title_size, sub_size, sub):
    """The face on the left, the name and a line or two beside it, centered as a group."""
    w, h = size
    img = Image.new("RGB", size, BG)
    d = ImageDraw.Draw(img)
    title = ImageFont.truetype(BOLD, title_size)
    small = ImageFont.truetype(REGULAR, sub_size)
    text_w = max([d.textlength("Macapad", font=title)] + [d.textlength(s, font=small) for s in sub])
    if left is None:
        left = int((w - face_size - gap - text_w) / 2)
    pic = face(face_size, face_size // 5)
    img.paste(pic, (left, (h - face_size) // 2), pic)
    x = left + face_size + gap
    top = h // 2 - (title_size + sub_size * len(sub)) // 2 - sub_size // 3
    d.text((x, top), "Macapad", font=title, fill=(255, 255, 255))
    for i, line in enumerate(sub):
        d.text((x, top + title_size * 1.25 + i * sub_size * 1.3), line, font=small, fill=(152, 152, 159))
    return img


def bg() -> None:
    indexed(card((840, 500), 220, None, 40, 72, 28, ["A macro pad for your Mac"]), ROOT / "sce_sys/livearea/contents/bg.png")


def startup() -> None:
    indexed(card((280, 158), 96, None, 14, 30, 14, ["A macro pad", "for your Mac"]), ROOT / "sce_sys/livearea/contents/startup.png")


if __name__ == "__main__":
    icon0()
    bg()
    startup()
