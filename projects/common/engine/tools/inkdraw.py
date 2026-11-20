"""Inked drawing for comic-style textures: flat fills with a black line.

A Canvas is a texture drawn at SS times its size and scaled down, so ink
lines stay crisp at 64 texels. Coordinates are texels. Used by Kaiju Lunch
Break's tools/build_textures.py.
"""

from __future__ import annotations

import math

from PIL import Image, ImageDraw

SS = 4
INK = (22, 18, 34)


class Canvas:
    """A w x h texture drawn at SS times its size; coordinates are texels."""

    def __init__(self, w, h, bg, alpha=False):
        self.w, self.h = w, h
        self.img = Image.new("RGBA", (w * SS, h * SS), tuple(bg) + ((0,) if alpha else (255,)))
        self.d = ImageDraw.Draw(self.img)

    def _s(self, pts):
        return [(x * SS, y * SS) for x, y in pts]

    def rect(self, x0, y0, x1, y1, fill=None, ink=0.0):
        self.d.rectangle([x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1], fill=rgba(fill),
                         outline=rgba(INK) if ink else None, width=max(1, int(ink * SS)))

    def poly(self, pts, fill=None, ink=0.0):
        self.d.polygon(self._s(pts), fill=rgba(fill))
        if ink:
            self.d.line(self._s(list(pts) + [pts[0]]), fill=rgba(INK), width=max(1, int(ink * SS)), joint="curve")

    def line(self, pts, col=INK, w=1.0):
        self.d.line(self._s(pts), fill=rgba(col), width=max(1, int(w * SS)), joint="curve")

    def ellipse(self, cx, cy, rx, ry, fill=None, ink=0.0):
        self.d.ellipse([(cx - rx) * SS, (cy - ry) * SS, (cx + rx) * SS, (cy + ry) * SS], fill=rgba(fill),
                       outline=rgba(INK) if ink else None, width=max(1, int(ink * SS)))

    def done(self, mode="RGB"):
        out = self.img.resize((self.w, self.h), Image.LANCZOS)
        return out.convert(mode) if mode == "RGB" else out


def rgba(c):
    if c is None:
        return None
    return tuple(c) + (255,) if len(c) == 3 else tuple(c)


def blob(c, lobes, fill, ink):
    for pass_ in (0, 1):
        for x, y, r in lobes:
            if pass_ == 0:
                c.ellipse(x, y, r + ink, r + ink, fill=INK)
            else:
                c.ellipse(x, y, r, r, fill=fill)


def star(cx, cy, r0, r1, n, rot=0.0):
    return [(cx + math.cos(rot + i * math.pi / n) * (r0 if i % 2 == 0 else r1),
             cy + math.sin(rot + i * math.pi / n) * (r0 if i % 2 == 0 else r1)) for i in range(n * 2)]
