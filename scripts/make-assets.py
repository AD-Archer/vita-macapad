#!/usr/bin/env python3
"""Makes the LiveArea images in sce_sys/ (and a preview in screenshots/) from assets/icon-source.png and Inter.

The Vita only shows these when they're 8-bit indexed PNGs, so each is quantized to a palette.
  python3 scripts/make-assets.py
"""
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FACE = Image.open(ROOT / "assets/icon-source.png").convert("RGBA")
BOLD = str(ROOT / "assets/fonts/Inter-SemiBold.ttf")
REGULAR = str(ROOT / "assets/fonts/Inter-Regular.ttf")


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
    # Max coverage keeps the few bright accent colors (median cut turns the greens teal), and the
    # image is then dithered onto that palette so the fading keys don't band in 256 colors.
    rgb = img.convert("RGB")
    palette = rgb.quantize(colors=256, method=Image.Quantize.MAXCOVERAGE)
    rgb.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)
    print(f"wrote {path.relative_to(ROOT)} {img.size[0]}x{img.size[1]}")


def icon0() -> None:
    # the bubble on the home screen is round, so the face fills the square
    w, h = FACE.size
    side = min(w, h)
    square = FACE.crop(((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2))
    indexed(square.resize((128, 128), Image.LANCZOS), ROOT / "sce_sys/icon0.png")


# Where the LiveArea draws the gate (startup.png) over bg.png in style a1 (Vita3K's layout table:
# gate at screen 340,183, background at 60,44). No style puts the gate further left.
GATE_X, GATE_Y, GATE_W, GATE_H = 280, 139, 280, 158

# The background is a pad of keys, sized so the gate covers exactly 3x2 of them: it reads as one
# big key on the pad. The name sits in the same-sized gap to its right.
GAP = 12
KEY_W = (GATE_W - 2 * GAP) / 3
KEY_H = (GATE_H - GAP) / 2
PITCH_X, PITCH_Y = KEY_W + GAP, KEY_H + GAP
TEXT_X = GATE_X + 3 * PITCH_X  # the gap for the name starts one gate-width to the right
W, H, SS = 840, 500, 2  # drawn at 2x, then scaled down, for smooth corners

YELLOW = (255, 214, 10)
BACKDROP = (10, 10, 12)
GREEN, TEAL, BLUE, ORANGE, PURPLE, PINK = (48, 209, 88), (100, 210, 255), (10, 132, 255), (255, 159, 10), (191, 90, 242), (255, 55, 95)


def key_cells():
    """Every key on the pad as (col, row, x, y), in background pixels."""
    c0 = -int(GATE_X // PITCH_X) - 1
    r0 = -int(GATE_Y // PITCH_Y) - 1
    for row in range(r0, r0 + 9):
        for col in range(c0, c0 + 11):
            x, y = GATE_X + col * PITCH_X, GATE_Y + row * PITCH_Y
            if x + KEY_W > 0 and x < W and y + KEY_H > 0 and y < H:
                yield col, row, x, y


def glyph(d, kind, cx, cy, s, color):
    """A few of the app's key icons, simplified, centered on (cx, cy), about s px across."""
    if kind == "play":
        d.polygon([(cx - s * 0.32, cy - s * 0.42), (cx - s * 0.32, cy + s * 0.42), (cx + s * 0.4, cy)], fill=color)
    elif kind == "pause":
        for dx in (-0.3, 0.1):
            d.rounded_rectangle((cx + dx * s, cy - s * 0.42, cx + (dx + 0.2) * s, cy + s * 0.42), s * 0.05, fill=color)
    elif kind == "next":
        for dx in (-0.5, -0.05):
            d.polygon([(cx + dx * s, cy - s * 0.32), (cx + dx * s, cy + s * 0.32), (cx + (dx + 0.45) * s, cy)], fill=color)
        d.rectangle((cx + 0.42 * s, cy - s * 0.32, cx + 0.52 * s, cy + s * 0.32), fill=color)
    elif kind == "search":
        r = s * 0.28
        d.ellipse((cx - r - s * 0.1, cy - r - s * 0.1, cx + r - s * 0.1, cy + r - s * 0.1), outline=color, width=int(s * 0.12))
        d.line((cx + s * 0.14, cy + s * 0.14, cx + s * 0.42, cy + s * 0.42), fill=color, width=int(s * 0.14))
    elif kind == "globe":
        r = s * 0.45
        w = int(s * 0.09)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=color, width=w)
        d.ellipse((cx - r * 0.45, cy - r, cx + r * 0.45, cy + r), outline=color, width=w)
        d.line((cx - r, cy, cx + r, cy), fill=color, width=w)
    elif kind == "keys":
        d.rounded_rectangle((cx - s * 0.6, cy - s * 0.36, cx + s * 0.6, cy + s * 0.36), s * 0.1, outline=color, width=int(s * 0.08))
        for row in range(2):
            for col in range(4):
                x = cx - s * 0.42 + col * s * 0.23
                y = cy - s * 0.2 + row * s * 0.17
                d.rectangle((x, y, x + s * 0.13, y + s * 0.1), fill=color)
    elif kind == "shortcut":
        d.rounded_rectangle((cx - s * 0.5, cy - s * 0.5, cx + s * 0.2, cy + s * 0.2), s * 0.16, fill=color)
        d.rounded_rectangle((cx - s * 0.2, cy - s * 0.2, cx + s * 0.5, cy + s * 0.5), s * 0.16, fill=color)
    elif kind == "app":
        d.rounded_rectangle((cx - s * 0.45, cy - s * 0.45, cx + s * 0.45, cy + s * 0.45), s * 0.2, fill=color)
    elif kind == "plus":
        t = s * 0.12
        d.rounded_rectangle((cx - s * 0.45, cy - t / 2, cx + s * 0.45, cy + t / 2), t / 2, fill=color)
        d.rounded_rectangle((cx - t / 2, cy - s * 0.45, cx + t / 2, cy + s * 0.45), t / 2, fill=color)


# What a few keys around the gate show, by (col, row) relative to the gate's top-left key.
ACCENTS = {
    (-1, 0): ("play", GREEN), (-2, 0): ("next", GREEN), (-1, 1): ("search", TEAL), (-2, 1): ("globe", TEAL),
    (-1, -1): ("keys", ORANGE), (0, -1): ("pause", GREEN), (1, -1): ("app", BLUE), (2, -1): ("shortcut", PURPLE),
    (-1, 2): ("shortcut", PURPLE), (0, 2): ("app", PINK), (1, 2): ("globe", BLUE), (2, 2): ("plus", ORANGE),
    (-3, 0): ("search", TEAL), (-3, 1): ("app", BLUE), (3, -1): ("keys", ORANGE), (3, 2): ("next", GREEN),
    (4, -1): ("play", GREEN), (4, 2): ("search", TEAL), (-2, -1): ("plus", ORANGE), (-2, 2): ("pause", GREEN),
}


def falloff(x, y):
    """How much a key at (x, y) shows: full by the gate, fading toward the edges of the LiveArea."""
    gx, gy = GATE_X + GATE_W / 2, GATE_Y + GATE_H / 2
    d = (((x - gx) / 430) ** 2 + ((y - gy) / 300) ** 2) ** 0.5
    return max(0.0, min(1.0, 1.15 - d))


def scene() -> np.ndarray:
    """The background without the name: plain dark, with the pad of keys."""
    w, h = W * SS, H * SS
    img = np.empty((h, w, 3), np.float32)
    img[:] = BACKDROP

    keys = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(keys)
    gate_col, gate_row = 0, 0
    for col, row, x, y in key_cells():
        rel = (col - gate_col, row - gate_row)
        in_gate = 0 <= rel[0] < 3 and 0 <= rel[1] < 2
        in_text = 3 <= rel[0] < 6 and 0 <= rel[1] < 2
        if in_gate or in_text:
            continue
        a = falloff(x + KEY_W / 2, y + KEY_H / 2)
        if a <= 0.02:
            continue
        box = (x * SS, y * SS, (x + KEY_W) * SS, (y + KEY_H) * SS)
        d.rounded_rectangle(box, 14 * SS, fill=(34, 34, 37, int(255 * a)))
        # a hairline of light around the edge, so the keys read on the dark
        d.rounded_rectangle(box, 14 * SS, outline=(60, 60, 64, int(160 * a)), width=SS)
        if rel in ACCENTS:
            kind, color = ACCENTS[rel]
            glyph(d, kind, (x + KEY_W / 2) * SS, (y + KEY_H / 2) * SS, 30 * SS, (*color, int(235 * a)))
    k = np.asarray(keys).astype(np.float32)
    alpha = k[..., 3:4] / 255
    img = img * (1 - alpha) + k[..., :3] * alpha
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")
    return out.resize((W, H), Image.LANCZOS)


def bg() -> None:
    """The pad, with the name in the gap to the right of the gate."""
    img = scene()
    d = ImageDraw.Draw(img)
    title = ImageFont.truetype(BOLD, 48)
    small = ImageFont.truetype(REGULAR, 18)
    x = TEXT_X + 6
    mid = GATE_Y + GATE_H / 2
    d.text((x, mid - 8), "Macapad", font=title, fill=(255, 255, 255), anchor="ls")
    d.rounded_rectangle((x + 2, mid + 6, x + 38, mid + 10), 2, fill=YELLOW)
    d.text((x + 1, mid + 36), "A macro pad", font=small, fill=(152, 152, 159), anchor="ls")
    d.text((x + 1, mid + 59), "for your Mac", font=small, fill=(152, 152, 159), anchor="ls")
    indexed(img, ROOT / "sce_sys/livearea/contents/bg.png")


def startup() -> None:
    """The gate: the same scene behind it (so it sits in the pad seamlessly), the face in the middle."""
    img = scene().crop((GATE_X, GATE_Y, GATE_X + GATE_W, GATE_Y + GATE_H)).convert("RGBA")
    size = 124
    # a soft shadow under the face, so it lifts off the background
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        ((GATE_W - size) // 2, (GATE_H - size) // 2 + 6, (GATE_W + size) // 2, (GATE_H + size) // 2 + 6), size // 5, fill=(0, 0, 0, 150))
    img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(8)))
    pic = face(size, size // 5)
    img.alpha_composite(pic, ((GATE_W - size) // 2, (GATE_H - size) // 2))
    indexed(img, ROOT / "sce_sys/livearea/contents/startup.png")


def preview() -> None:
    """screenshots/livearea.png for the README: the background with the gate where the Vita puts it."""
    img = Image.open(ROOT / "sce_sys/livearea/contents/bg.png").convert("RGB")
    img.paste(Image.open(ROOT / "sce_sys/livearea/contents/startup.png").convert("RGB"), (GATE_X, GATE_Y))
    path = ROOT / "screenshots/livearea.png"
    img.save(path, optimize=True)
    print(f"wrote {path.relative_to(ROOT)} {img.size[0]}x{img.size[1]}")


if __name__ == "__main__":
    icon0()
    bg()
    startup()
    preview()
