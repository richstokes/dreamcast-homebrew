#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "pillow>=10.1", "scipy>=1.11"]
# ///
"""Paint Kaiju Lunch Break's textures and pack them for the PowerVR.

The look is a comic book: flat colour fields, hard two-tone shading and
black ink lines. Everything is drawn from code at four times its size and
scaled down, so the ink stays crisp. Wall and ground textures are near
white where the vertex colour tints them. Writes to --output-dir:

    textures.pak        twiddled RGB565/ARGB4444 textures (+mips), 32-byte aligned
    texture_assets.h    TEX_* ids and the size/format/offset table
    font_metrics.h      the font's glyph table (engine/text.h)
    previews/*.png      every texture as painted (git-ignored)
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
from assetpak import TexSpec, write_font_metrics, write_texture_header, write_texture_pak  # noqa: E402
from fontatlas import LILITA, atlas, font_for_cap  # noqa: E402
from inkdraw import INK, SS, Canvas, blob, rgba, star  # noqa: E402

GEN = Path(__file__).resolve().parent.parent / "assets" / "generated"
FONT_ROW_H = 42
FONT_CAP_H = 22
FONT_BASELINE = 30


def rng(seed):
    return np.random.default_rng(seed)


# ---------------------------------------------------------------------------
# Buildings: a tile is two bays by two floors, near white for the tint.

GLASS = (96, 158, 222)
GLASS_HI = (176, 216, 248)
GLASS_DK = (58, 98, 170)
LIT = (255, 226, 120)


def window(c, x0, y0, x1, y1, fill=GLASS, ink=1.0, shine=True):
    c.rect(x0, y0, x1, y1, fill=fill, ink=ink)
    if shine and fill != LIT:
        w, h = x1 - x0, y1 - y0
        c.poly([(x0 + ink, y0 + h * 0.55), (x0 + w * 0.55, y0 + ink), (x0 + w * 0.8, y0 + ink), (x0 + ink, y0 + h * 0.85)],
               fill=GLASS_HI)


def wall_office():
    c = Canvas(64, 64, (246, 244, 240))
    k = 0
    for fy in range(2):
        for bx in range(2):
            x, y = bx * 32, fy * 32
            window(c, x + 5, y + 6, x + 27, y + 24, fill=LIT if k == 2 else GLASS)
            c.line([(x + 16, y + 6), (x + 16, y + 24)], w=0.8)
            k += 1
        c.line([(0, fy * 32 + 30.5), (64, fy * 32 + 30.5)], col=(190, 186, 196), w=1.6)
    return c.done()


def wall_apartment():
    c = Canvas(64, 64, (250, 246, 238))
    cols = [GLASS, (236, 150, 150), GLASS, LIT]
    k = 0
    for fy in range(2):
        for bx in range(2):
            x, y = bx * 32, fy * 32
            window(c, x + 9, y + 7, x + 23, y + 23, fill=cols[k], shine=cols[k] == GLASS)
            c.rect(x + 7, y + 23, x + 25, y + 26, fill=(214, 206, 196), ink=0.8)       # sill
            if k == 0:
                c.rect(x + 5, y + 7, x + 9, y + 23, fill=(120, 170, 130), ink=0.7)     # shutter
            k += 1
    c.line([(0, 31.5), (64, 31.5)], col=(205, 198, 190), w=1.0)
    c.line([(0, 63.5), (64, 63.5)], col=(205, 198, 190), w=1.0)
    return c.done()


def wall_brick():
    c = Canvas(64, 64, (244, 232, 224))
    for row in range(16):
        y = row * 4
        c.line([(0, y + 0.5), (64, y + 0.5)], col=(208, 186, 176), w=0.7)
        for bx in range(5):
            x = bx * 16 + (8 if row & 1 else 0)
            c.line([(x, y), (x, y + 4)], col=(208, 186, 176), w=0.7)
    k = 0
    for fy in range(2):
        for bx in range(2):
            x, y = bx * 32, fy * 32
            fill = LIT if k == 1 else GLASS_DK
            c.d.pieslice([(x + 9) * SS, (y + 4) * SS, (x + 23) * SS, (y + 16) * SS], 180, 360, fill=rgba(fill), outline=rgba(INK), width=SS)
            window(c, x + 9, y + 10, x + 23, y + 25, fill=fill, shine=False)
            c.line([(x + 16, y + 5), (x + 16, y + 25)], w=0.8)
            k += 1
    return c.done()


def wall_ribbon():
    c = Canvas(64, 64, (248, 248, 246))
    for fy in range(2):
        y = fy * 32
        c.rect(0, y + 8, 64, y + 22, fill=GLASS_DK)
        c.line([(0, y + 8), (64, y + 8)], w=1.2)
        c.line([(0, y + 22), (64, y + 22)], w=1.2)
        for x in (0, 16, 32, 48):
            c.line([(x + 0.5, y + 8), (x + 0.5, y + 22)], w=0.8)
        c.poly([(6, y + 21), (20, y + 9), (30, y + 9), (16, y + 21)], fill=GLASS)
        c.poly([(40, y + 21), (50, y + 9), (54, y + 9), (44, y + 21)], fill=GLASS)
        c.rect(34 if fy else 2, y + 9, 46 if fy else 14, y + 21, fill=LIT if fy else None)
    return c.done()


def wall_glass():
    c = Canvas(64, 64, (150, 214, 236))
    c.poly([(0, 64), (0, 30), (30, 0), (64, 0), (64, 6), (6, 64)], fill=(196, 236, 250))
    c.poly([(30, 64), (64, 30), (64, 44), (44, 64)], fill=(118, 184, 220))
    for i in range(0, 65, 16):
        c.line([(i, 0), (i, 64)], w=1.0)
        c.line([(0, i), (64, i)], w=1.0)
    return c.done()


def shopfront():
    """Street level: two shops a tile, awnings over glass."""
    c = Canvas(64, 64, (236, 230, 222))
    for bx, (a, b) in enumerate((((232, 80, 72), (250, 246, 240)), ((70, 150, 200), (250, 246, 240)))):
        x = bx * 32
        window(c, x + 4, 26, x + 20, 56, fill=(150, 206, 232))
        c.rect(x + 21, 28, x + 29, 62, fill=(96, 78, 70), ink=1.0)                    # door
        c.ellipse(x + 27, 46, 0.9, 0.9, fill=LIT)
        for s in range(4):                                                             # awning
            c.poly([(x + 2 + s * 7, 12), (x + 9 + s * 7, 12), (x + 9.5 + s * 7, 25), (x + 1.5 + s * 7, 25)],
                   fill=a if s % 2 == 0 else b)
        c.poly([(x + 2, 12), (x + 30, 12), (x + 31, 25), (x + 1, 25)], ink=1.0)
        c.rect(x + 3, 3, x + 29, 10, fill=(255, 220, 110) if bx else (250, 246, 240), ink=0.9)   # sign
        c.line([(x + 7, 6.5), (x + 25, 6.5)], w=1.4)
    c.line([(0, 62.5), (64, 62.5)], w=2.0)
    return c.done()


def roof():
    c = Canvas(64, 64, (226, 222, 226))
    r = rng(5)
    for _ in range(26):
        x, y = r.uniform(6, 58), r.uniform(6, 58)
        c.line([(x, y), (x + r.uniform(1, 3), y + r.uniform(-0.5, 0.5))], col=(200, 196, 204), w=0.8)
    c.rect(0, 0, 64, 64, ink=3.0)
    c.rect(3, 3, 61, 61, ink=0.8)
    return c.done()


# ---------------------------------------------------------------------------
# Ground

ASPHALT = (104, 106, 124)


def road():
    """u across the road, v along it."""
    c = Canvas(64, 64, ASPHALT)
    c.rect(0, 0, 5, 64, fill=(214, 208, 200))          # kerbs
    c.rect(59, 0, 64, 64, fill=(214, 208, 200))
    c.line([(5, 0), (5, 64)], w=1.2)
    c.line([(59, 0), (59, 64)], w=1.2)
    c.line([(9, 0), (9, 64)], col=(236, 236, 240), w=0.9)
    c.line([(55, 0), (55, 64)], col=(236, 236, 240), w=0.9)
    c.rect(31, 6, 33, 26, fill=(255, 214, 90))
    c.rect(31, 38, 33, 58, fill=(255, 214, 90))
    return c.done()


def crossing():
    c = Canvas(64, 64, ASPHALT)
    for i in range(6):
        t = 9 + i * 8
        for (x0, y0, x1, y1) in ((t, 1, t + 4, 9), (t, 55, t + 4, 63), (1, t, 9, t + 4), (55, t, 63, t + 4)):
            c.rect(x0, y0, x1, y1, fill=(240, 240, 244))
    return c.done()


def pavement():
    c = Canvas(64, 64, (228, 222, 212))
    for i in range(0, 65, 16):
        c.line([(i, 0), (i, 64)], col=(196, 188, 180), w=0.8)
        c.line([(0, i), (64, i)], col=(196, 188, 180), w=0.8)
    c.rect(20, 36, 28, 44, fill=(170, 164, 160), ink=0.7)
    return c.done()


def grass():
    c = Canvas(64, 64, (126, 204, 110))
    r = rng(11)
    for _ in range(18):
        x, y = r.uniform(2, 62), r.uniform(4, 62)
        for ox in (x, x - 64, x + 64):
            c.line([(ox - 2, y), (ox - 1, y - 3), (ox, y), (ox + 1, y - 4), (ox + 2, y)], col=(84, 168, 84), w=0.9)
    return c.done()


def water():
    c = Canvas(64, 64, (64, 176, 214))
    r = rng(3)
    for k in range(7):
        y = 4 + k * 9
        x = r.uniform(0, 64)
        for ox in (x - 64, x, x + 64):
            pts = [(ox + i * 2, y + math.sin(i * 0.9) * 1.4) for i in range(11)]
            c.line(pts, col=(214, 246, 255) if k % 2 else (40, 140, 190), w=1.0)
    return c.done()


def sand():
    c = Canvas(64, 64, (244, 226, 170))
    r = rng(9)
    for _ in range(40):
        x, y = r.uniform(0, 64), r.uniform(0, 64)
        c.ellipse(x, y, 0.7, 0.7, fill=(222, 198, 140))
    return c.done()


def rubble():
    c = Canvas(64, 64, (150, 144, 150))
    r = rng(21)
    for _ in range(34):
        x, y, s = r.uniform(0, 64), r.uniform(0, 64), r.uniform(4, 10)
        a0 = r.uniform(0, 6.28)
        n = int(r.integers(4, 7))
        shade = int(r.integers(0, 3))
        col = ((232, 226, 222), (190, 184, 190), (120, 112, 124))[shade]
        for ox in (-64, 0, 64):
            for oy in (-64, 0, 64):
                pts = [(x + ox + math.cos(a0 + i * 6.28 / n) * s * (0.7 + 0.3 * ((i * 7) % 3) / 2),
                        y + oy + math.sin(a0 + i * 6.28 / n) * s * (0.7 + 0.3 * ((i * 5) % 3) / 2)) for i in range(n)]
                c.poly(pts, fill=col, ink=0.9)
    return c.done()


def halftone():
    """Cast shadows: ink blue with a dot screen, as a comic prints them."""
    c = Canvas(32, 32, (58, 56, 96))
    for j in range(4):
        for i in range(4):
            c.ellipse(i * 8 + (4 if j & 1 else 0) + 2, j * 8 + 4, 1.7, 1.7, fill=(86, 84, 130))
            c.ellipse(i * 8 + (4 if j & 1 else 0) + 2 - 32, j * 8 + 4, 1.7, 1.7, fill=(86, 84, 130))
    return c.done()


def ramp():
    """The cel ramp: u is how far a surface faces the sun."""
    img = Image.new("RGB", (64, 8))
    for x in range(64):
        t = x / 63
        col = (128, 120, 186) if t < 0.5 else (218, 214, 236) if t < 0.74 else (255, 255, 255)
        for y in range(8):
            img.putpixel((x, y), col)
    return img


# ---------------------------------------------------------------------------
# Sprites (alpha)

def puff():
    c = Canvas(64, 64, (255, 255, 255), alpha=True)
    lobes = [(32, 36, 17), (19, 40, 11), (45, 40, 12), (25, 24, 12), (40, 23, 11), (32, 46, 13)]
    blob(c, lobes, (255, 255, 255), 2.2)
    # The shaded underside, cut to the cloud.
    shade = Canvas(64, 64, (0, 0, 0), alpha=True)
    for x, y, r in lobes:
        shade.ellipse(x - 1, y + 7, r - 3, r - 4, fill=(206, 200, 226))
    cut = Canvas(64, 64, (0, 0, 0), alpha=True)
    for x, y, r in lobes:
        cut.ellipse(x + 2, y - 3, r - 2.5, r - 2.5, fill=(255, 255, 255))
    a = np.array(c.img)
    s, k = np.array(shade.img), np.array(cut.img)
    m = (s[..., 3] > 0) & (k[..., 3] == 0) & (a[..., 0] > 200) & (a[..., 3] > 0)
    a[m, :3] = s[m, :3]
    c.img = Image.fromarray(a, "RGBA")
    return c.done("RGBA")


def boom():
    c = Canvas(64, 64, (255, 200, 60), alpha=True)
    c.poly(star(32, 32, 30, 17, 9, 0.2), fill=(240, 80, 50), ink=2.0)
    c.poly(star(32, 32, 22, 12, 9, 0.5), fill=(255, 190, 60), ink=1.2)
    c.poly(star(32, 32, 13, 7, 7, 0.1), fill=(255, 250, 220))
    return c.done("RGBA")


def pow_star():
    """A white burst behind the comic words."""
    c = Canvas(64, 64, (255, 255, 255), alpha=True)
    c.poly(star(32, 32, 31, 20, 11, 0.15), fill=(255, 255, 255), ink=2.4)
    return c.done("RGBA")


def fire():
    c = Canvas(64, 64, (255, 160, 40), alpha=True)
    c.poly([(32, 2), (42, 20), (50, 12), (56, 38), (48, 60), (16, 60), (8, 40), (14, 18), (22, 28)], fill=(244, 96, 40), ink=2.0)
    c.poly([(32, 18), (40, 34), (45, 30), (46, 46), (40, 58), (24, 58), (18, 44), (22, 32), (27, 38)], fill=(255, 196, 60))
    c.poly([(32, 36), (37, 46), (36, 56), (28, 56), (27, 46)], fill=(255, 250, 210))
    return c.done("RGBA")


def ring():
    c = Canvas(64, 64, (255, 255, 255), alpha=True)
    c.ellipse(32, 32, 31, 31, fill=INK)
    c.ellipse(32, 32, 29, 29, fill=(255, 255, 255))
    c.ellipse(32, 32, 24, 24, fill=INK)
    c.d.ellipse([10 * SS, 10 * SS, 54 * SS, 54 * SS], fill=(0, 0, 0, 0))
    return c.done("RGBA")


def glow():
    n = 32
    y, x = np.mgrid[0:n, 0:n]
    d = np.hypot(x - 15.5, y - 15.5) / 15.5
    a = np.clip(1.0 - d, 0, 1) ** 1.5
    out = np.zeros((n, n, 4), np.uint8)
    out[..., :3] = 255
    out[..., 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def beam():
    """u across the beam, v along it: a white core in hard bands, inked."""
    img = Image.new("RGBA", (32, 8), (0, 0, 0, 0))
    for x in range(32):
        d = abs(x - 15.5) / 15.5
        col = (255, 255, 255, 255) if d < 0.34 else (150, 240, 255, 255) if d < 0.62 else (60, 150, 255, 255) if d < 0.84 else (22, 18, 34, 255) if d < 0.95 else (0, 0, 0, 0)
        for y in range(8):
            img.putpixel((x, y), col)
    return img


def blobshadow():
    c = Canvas(64, 64, (58, 56, 96), alpha=True)
    c.ellipse(32, 32, 30, 30, fill=(58, 56, 96))
    for j in range(8):
        for i in range(8):
            x, y = i * 8 + (4 if j & 1 else 0) + 2, j * 8 + 4
            if math.hypot(x - 32, y - 32) < 27:
                c.ellipse(x, y, 1.7, 1.7, fill=(86, 84, 130))
    return c.done("RGBA")


def person():
    """A tiny citizen, arms up, white for the tint."""
    c = Canvas(16, 32, (255, 255, 255), alpha=True)
    c.line([(8, 13), (3, 5)], w=3.6)
    c.line([(8, 13), (13, 5)], w=3.6)
    c.line([(8, 20), (5, 30)], w=4.0)
    c.line([(8, 20), (11, 30)], w=4.0)
    c.rect(4.5, 11, 11.5, 22, fill=INK)
    c.ellipse(8, 7, 4, 4, fill=INK)
    c.line([(8, 13), (3.4, 5.6)], col=(255, 255, 255), w=1.6)
    c.line([(8, 13), (12.6, 5.6)], col=(255, 255, 255), w=1.6)
    c.line([(8, 20), (5.4, 29)], col=(90, 90, 120), w=2.0)
    c.line([(8, 20), (10.6, 29)], col=(90, 90, 120), w=2.0)
    c.rect(5.6, 12, 10.4, 21, fill=(255, 255, 255))
    c.ellipse(8, 7, 2.9, 2.9, fill=(255, 224, 196))
    return c.done("RGBA")


def tree():
    c = Canvas(64, 64, (90, 190, 96), alpha=True)
    c.rect(28, 40, 36, 63, fill=(150, 104, 72), ink=1.6)
    lobes = [(32, 24, 16), (20, 30, 11), (44, 30, 11), (26, 14, 10), (39, 14, 10)]
    blob(c, lobes, (96, 200, 104), 2.0)
    for x, y, r in lobes[:3]:
        c.d.pieslice([(x - r + 3) * SS, (y - r + 5) * SS, (x + r - 3) * SS, (y + r - 1) * SS], 20, 160, fill=rgba((62, 160, 92)))
    return c.done("RGBA")


def cloud():
    c = Canvas(128, 64, (255, 255, 255), alpha=True)
    lobes = [(40, 38, 18), (64, 30, 22), (90, 38, 17), (24, 44, 11), (106, 45, 10)]
    blob(c, lobes, (255, 255, 255), 2.2)
    c.rect(18, 44, 112, 56, fill=(255, 255, 255))
    c.line([(16, 56.5), (114, 56.5)], w=2.2)
    c.line([(30, 49), (98, 49)], col=(206, 214, 240), w=2.4)
    a = np.array(c.img)
    a[58 * SS:, :, 3] = 0
    c.img = Image.fromarray(a, "RGBA")
    return c.done("RGBA")


def ads(font):
    """Four rooftop billboards, 2 x 2."""
    c = Canvas(128, 128, (255, 255, 255))
    panels = [("KAIJU", "KOLA", (232, 64, 64), (255, 240, 220)), ("MEGA", "NOODLE", (255, 206, 70), INK),
              ("DINO", "DONUTS", (250, 140, 190), (255, 255, 255)), ("STOMP", "GYM", (70, 190, 170), (255, 255, 255))]
    f = ImageFont.truetype(LILITA, 15 * SS)
    for i, (a, b, bg, fg) in enumerate(panels):
        x, y = (i % 2) * 64, (i // 2) * 64
        c.rect(x, y, x + 64, y + 64, fill=bg, ink=2.4)
        for k, word in enumerate((a, b)):
            w = c.d.textlength(word, font=f)
            c.d.text(((x + 32) * SS - w / 2, (y + 8 + k * 24) * SS), word, font=f, fill=rgba(fg),
                     stroke_width=SS, stroke_fill=rgba(INK))
    return c.done()


def logo():
    """The title, two tilted lines of chunky lettering."""
    W, H = 512, 256
    img = Image.new("RGBA", (W * 2, H * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f1, f2 = ImageFont.truetype(LILITA, 236), ImageFont.truetype(LILITA, 142)
    for text, f, y, fill in (("KAIJU", f1, 10, (126, 232, 120)), ("LUNCH BREAK", f2, 262, (255, 214, 80))):
        w = d.textlength(text, font=f)
        x = (W * 2 - w) / 2
        d.text((x + 10, y + 12), text, font=f, fill=(22, 18, 34, 255), stroke_width=14, stroke_fill=(22, 18, 34, 255))
        d.text((x, y), text, font=f, fill=fill + (255,), stroke_width=12, stroke_fill=(22, 18, 34, 255))
        d.text((x, y), text, font=f, fill=fill + (255,), stroke_width=4, stroke_fill=(255, 255, 255, 255))
        d.text((x, y), text, font=f, fill=fill + (255,))
    img = img.rotate(4, resample=Image.BICUBIC)
    return img.resize((W, H), Image.LANCZOS)


def font():
    return atlas(font_for_cap(LILITA, FONT_CAP_H), FONT_ROW_H, FONT_BASELINE, 2, outline_rgba=INK + (255,))


# ---------------------------------------------------------------------------

def build(out: Path):
    out.mkdir(parents=True, exist_ok=True)
    previews = out / "previews"
    previews.mkdir(exist_ok=True)
    font_img, glyphs, cap = font()
    O, A = "RGB565", "ARGB4444"
    items = [
        (TexSpec("ramp", O, False, False), ramp()),
        (TexSpec("wall_office", O, True, True), wall_office()),
        (TexSpec("wall_apt", O, True, True), wall_apartment()),
        (TexSpec("wall_brick", O, True, True), wall_brick()),
        (TexSpec("wall_ribbon", O, True, True), wall_ribbon()),
        (TexSpec("wall_glass", O, True, True), wall_glass()),
        (TexSpec("shop", O, True, True), shopfront()),
        (TexSpec("roof", O, True, False), roof()),
        (TexSpec("road", O, True, True), road()),
        (TexSpec("cross", O, True, True), crossing()),
        (TexSpec("pave", O, True, True), pavement()),
        (TexSpec("grass", O, True, True), grass()),
        (TexSpec("water", O, True, True), water()),
        (TexSpec("sand", O, True, True), sand()),
        (TexSpec("rubble", O, True, True), rubble()),
        (TexSpec("halftone", O, True, True), halftone()),
        (TexSpec("ads", O, True, False), ads(None)),
        (TexSpec("puff", A, True, False, alpha_ref=0.5), puff()),
        (TexSpec("boom", A, True, False, alpha_ref=0.5), boom()),
        (TexSpec("pow", A, True, False, alpha_ref=0.5), pow_star()),
        (TexSpec("fire", A, True, False, alpha_ref=0.5), fire()),
        (TexSpec("ring", A, True, False, alpha_ref=0.5), ring()),
        (TexSpec("tree", A, True, False, alpha_ref=0.5), tree()),
        (TexSpec("blobshadow", A, True, False, alpha_ref=0.5), blobshadow()),
        (TexSpec("glow", A, False, False), glow()),
        (TexSpec("beam", A, False, False), beam()),
        (TexSpec("person", A, False, False), person()),
        (TexSpec("cloud", A, False, False), cloud()),
        (TexSpec("logo", A, False, False), logo()),
        (TexSpec("font", A, False, False), font_img),
    ]
    table, _ = write_texture_pak(out, previews, items)
    write_texture_header(out, "KL", table)
    write_font_metrics(
        out, "KL", font_img, glyphs, cap, FONT_ROW_H,
        glyph_comment=["/* ASCII 32..127: atlas x, row, rect width, rect left from the pen, advance (eng_glyph_t). */"])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, default=GEN)
    build(p.parse_args().output_dir.resolve())


if __name__ == "__main__":
    main()
