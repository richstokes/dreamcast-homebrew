"""Bitmap font atlases for the engine's text module (engine/text.h).

White glyphs with a dark outline, tinted by vertex colour at run time, each
packed into its own rect (its ink plus a clear texel each side, so bilinear
filtering never samples a neighbour), one row every row_h texels. Per glyph
of ASCII 32..127: (atlas x, row, rect width, rect left relative to the pen,
pen advance), the engine's eng_glyph_t. Digits get tabular advances so a
ticking number keeps its width.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

FONTS = Path(__file__).resolve().parent / "fonts"
LILITA = str(FONTS / "LilitaOne-Regular.ttf")
CHARS = [chr(c) for c in range(32, 128)]


def font_for_cap(path: str, cap_h: int) -> ImageFont.FreeTypeFont:
    """The font at the size whose capitals stand cap_h tall."""
    best = None
    for size in range(10, 96):
        f = ImageFont.truetype(path, size)
        bb = f.getbbox("H")
        cap = bb[3] - bb[1]
        if best is None or abs(cap - cap_h) < abs(best[0] - cap_h):
            best = (cap, f)
    return best[1]


def atlas(font: ImageFont.FreeTypeFont, row_h: int, baseline: int, outline: int,
          outline_rgba=(16, 22, 48, 255), size=(512, 256)):
    """Returns the atlas image, the glyph table and the capitals' (top,
    bottom) rows within a glyph row."""
    img = Image.new("RGBA", size, (255, 255, 255, 0))
    stroke = outline
    pad = 16
    # The baseline sits baseline rows down unless a tall ascender or deep
    # descender (plus the outline) would leave the row: then the whole
    # alphabet shifts to fit.
    ascent = font.getmetrics()[0]
    ty = baseline - ascent
    lo, hi = row_h, 0
    for ch in CHARS:
        if ch == " ":
            continue
        cell = Image.new("L", (64 + 2 * pad, row_h * 3), 0)
        ImageDraw.Draw(cell).text((pad + 3, row_h + ty), ch, font=font, fill=255,
                                  stroke_width=stroke, stroke_fill=255)
        ys = np.nonzero(np.array(cell))[0]
        if len(ys):
            lo, hi = min(lo, ys.min() - row_h), max(hi, ys.max() - row_h)
    if lo < 1:
        ty += 1 - lo
    elif hi > row_h - 2:
        ty -= hi - (row_h - 2)
    glyphs = []
    x = row = 0
    for ch in CHARS:
        adv = int(round(font.getlength(ch))) + 2
        if ch == " ":
            glyphs.append((0, 0, 0, 0, adv))
            continue
        cell = Image.new("RGBA", (64 + 2 * pad, row_h), (255, 255, 255, 0))
        ImageDraw.Draw(cell).text((pad + 3, ty), ch, font=font, fill=(255, 255, 255, 255),
                                  stroke_width=stroke, stroke_fill=outline_rgba)
        alpha = np.array(cell)[..., 3]
        ys, xs = np.nonzero(alpha)
        if not len(ys):   # a glyph the font lacks (DEL) draws as a blank
            glyphs.append((0, 0, 0, 0, adv))
            continue
        assert ys.min() >= 1 and ys.max() <= row_h - 2, (ch, ys.min(), ys.max())
        x0, x1 = xs.min() - 1, xs.max() + 2      # rect with a clear texel each side
        w = x1 - x0
        if x + w > img.width:
            x, row = 0, row + 1
        assert (row + 1) * row_h <= img.height, "font atlas full"
        img.alpha_composite(cell.crop((x0, 0, x1, row_h)), (x, row * row_h))
        glyphs.append((x, row, w, x0 - pad, adv))
        x += w
    # Tabular digits: proportional ones made a ticking clock change width
    # every frame. Each digit takes the widest one's advance, ink centred.
    tab = max(glyphs[ord(c) - 32][4] for c in "0123456789")
    for c in "0123456789":
        gx, grow, gw, gox, gadv = glyphs[ord(c) - 32]
        glyphs[ord(c) - 32] = (gx, grow, gw, gox + (tab - gadv) // 2, tab)
    # Where the capitals stand in the row (H's white fill, outline aside).
    hx, hrow, hw, _, _ = glyphs[ord("H") - 32]
    cell = np.array(img)[hrow * row_h:(hrow + 1) * row_h, hx:hx + hw]
    fill = np.nonzero((cell[..., 3] > 128) & (cell[..., 0] > 200))[0]
    cap = (int(fill.min()), int(fill.max()) + 1)
    return img, glyphs, cap
