"""Renders the Lumen Studio application icon (resources/app/lumen.ico) from the logo shape.

Usage: python scripts/make-icon.py  (requires Pillow)
"""
import os
from PIL import Image, ImageDraw

ACCENT = (139, 108, 255, 255)
STAR = [(12, 2), (14.6, 8.4), (21, 11), (14.6, 13.6), (12, 20), (9.4, 13.6), (3, 11), (9.4, 8.4)]


def render(size: int) -> Image.Image:
    scale = 4  # supersampling
    s = size * scale
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=s * 0.22, fill=ACCENT)
    # The logo lives in a 24-unit box; centre it with a margin.
    unit = s * 0.8 / 24
    ox = (s - 24 * unit) / 2
    oy = (s - 24 * unit) / 2 + unit
    d.polygon([(ox + x * unit, oy + y * unit) for x, y in STAR], fill=(255, 255, 255, 255))
    r = 2 * unit
    cx, cy = ox + 12 * unit, oy + 11 * unit
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=ACCENT)
    return im.resize((size, size), Image.LANCZOS)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, 'resources', 'app', 'lumen.ico')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    render(256).save(out, sizes=[(n, n) for n in sizes])
    print(out)


if __name__ == '__main__':
    main()
