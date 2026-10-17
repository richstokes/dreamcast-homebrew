#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "pillow>=10.1", "matplotlib>=3.8"]
# ///
"""Procedurally paint every DreamKarts texture and pack it for the PowerVR.

Everything is drawn from code (seeded noise, shapes and the DejaVu fonts that
ship with matplotlib, Bitstream Vera licence), so the art is original and the
output is reproducible. Writes to --output-dir:

    textures.pak        twiddled RGB565/ARGB4444 textures (+mips), 32-byte aligned
    texture_assets.h    TEX_* ids and the size/format/offset table
    font_metrics.h      glyph advances for the HUD font, title logo letter cells
    previews/*.png      every texture as painted (git-ignored)
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pvr_texture import pack_texture  # noqa: E402

RNG = np.random.default_rng(1999)


def font_path(name: str) -> str:
    import matplotlib
    return os.path.join(os.path.dirname(matplotlib.__file__), "mpl-data", "fonts", "ttf", name)


# ---------------------------------------------------------------------------
# Noise and colour helpers

def periodic_noise(w: int, h: int, period: int, seed: int) -> np.ndarray:
    """Tileable smooth value noise in [0,1] with `period` cells across."""
    rng = np.random.default_rng(seed)
    py = max(1, round(period * h / w))
    grid = rng.random((py, period))
    ys = np.arange(h) * py / h
    xs = np.arange(w) * period / w
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    ty = ys - y0
    tx = xs - x0
    ty = ty * ty * (3 - 2 * ty)
    tx = tx * tx * (3 - 2 * tx)
    y1 = (y0 + 1) % py
    x1 = (x0 + 1) % period
    a = grid[np.ix_(y0, x0)]
    b = grid[np.ix_(y0, x1)]
    c = grid[np.ix_(y1, x0)]
    d = grid[np.ix_(y1, x1)]
    top = a + (b - a) * tx[None, :]
    bot = c + (d - c) * tx[None, :]
    return top + (bot - top) * ty[:, None]


def fbm(w: int, h: int, base: int, octaves: int, seed: int, gain: float = 0.5) -> np.ndarray:
    total = np.zeros((h, w))
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        total += amp * periodic_noise(w, h, base * (2 ** o), seed + o * 101)
        norm += amp
        amp *= gain
    return total / norm


def to_image(rgb: np.ndarray, alpha: np.ndarray | None = None) -> Image.Image:
    rgb = np.clip(rgb, 0, 255).astype(np.uint8)
    if alpha is None:
        return Image.fromarray(rgb, "RGB")
    a = np.clip(alpha, 0, 255).astype(np.uint8)
    return Image.fromarray(np.dstack([rgb, a]), "RGBA")


def lerp_color(c0, c1, t):
    c0 = np.array(c0, dtype=float)
    c1 = np.array(c1, dtype=float)
    return c0 + (c1 - c0) * t[..., None]


# ---------------------------------------------------------------------------
# Surfaces

def tex_road(n=256):
    noise = fbm(n, n, 8, 5, 11)
    fine = RNG.random((n, n))
    base = 112 + 26 * (noise - 0.5) + 16 * (fine - 0.5)
    rgb = np.dstack([base * 1.0, base * 1.0, base * 1.05])
    # Aggregate speckles.
    speck = RNG.random((n, n)) > 0.985
    rgb[speck] = rgb[speck] * 1.35
    dark = RNG.random((n, n)) > 0.988
    rgb[dark] = rgb[dark] * 0.6
    u = (np.arange(n) + 0.5) / n
    # Rubbered racing grooves.
    groove = np.exp(-((u - 0.33) / 0.06) ** 2) + np.exp(-((u - 0.67) / 0.06) ** 2)
    rgb *= (1 - 0.13 * groove)[None, :, None]
    # White edge lines.
    edge = ((u > 0.018) & (u < 0.052)) | ((u > 0.948) & (u < 0.982))
    rgb[:, edge] = np.array([236, 236, 230]) * (0.93 + 0.07 * fine[:, edge, None])
    return to_image(rgb)


def tex_boardwalk(n=256):
    planks = 16
    v = (np.arange(n) + 0.5) / n
    u = (np.arange(n) + 0.5) / n
    plank_id = np.floor(v * planks).astype(int)
    rng = np.random.default_rng(5)
    tint = rng.uniform(0.85, 1.12, planks)
    grain = fbm(n, n, 4, 4, 23)
    streak = periodic_noise(n, n, 64, 31)
    base = np.array([214, 172, 118], dtype=float)
    shade = tint[plank_id][:, None] * (0.86 + 0.18 * grain + 0.08 * (streak - 0.5))
    rgb = base[None, None, :] * shade[..., None]
    frac = v * planks - plank_id
    gap = (frac < 0.07) | (frac > 0.96)
    rgb[gap] = rgb[gap] * 0.35
    # Nails near the joists.
    for j in (0.12, 0.5, 0.88):
        for p in range(planks):
            cy = int((p + 0.5) / planks * n)
            cx = int(j * n)
            rgb[cy - 1:cy + 1, cx - 1:cx + 1] = (90, 80, 72)
    # White painted edge boards.
    edge = (u < 0.045) | (u > 0.955)
    rgb[:, edge] = np.array([240, 238, 228]) * (0.94 + 0.06 * grain[:, edge, None])
    return to_image(rgb)


def tex_curb(n=64):
    v = (np.arange(n) + 0.5) / n
    u = (np.arange(n) + 0.5) / n
    red = np.floor(v * 2).astype(int) % 2 == 0
    rgb = np.where(red[:, None, None], np.array([214, 38, 32]), np.array([246, 246, 240])).astype(float)
    rgb = np.broadcast_to(rgb, (n, n, 3)).copy()
    bevel = 0.82 + 0.18 * np.sin(u * math.pi)
    rgb *= bevel[None, :, None]
    rgb *= 0.95 + 0.05 * RNG.random((n, n))[..., None]
    return to_image(rgb)


def tex_sand(n=128):
    noise = fbm(n, n, 4, 5, 41)
    grains = RNG.random((n, n))
    ripple = np.sin((np.arange(n)[None, :] * 2 * math.pi * 3 / n) +
                    6.0 * periodic_noise(n, n, 3, 43)) * 0.5 + 0.5
    t = 0.55 * noise + 0.25 * ripple
    rgb = lerp_color((226, 206, 160), (250, 238, 204), t)
    rgb *= (0.93 + 0.12 * grains)[..., None]
    shells = RNG.random((n, n)) > 0.996
    rgb[shells] = (255, 250, 240)
    return to_image(rgb)


def tex_grass(n=128):
    noise = fbm(n, n, 4, 5, 51)
    blades = periodic_noise(n, n, 48, 53)
    fine = RNG.random((n, n))
    t = 0.5 * noise + 0.3 * blades + 0.2 * fine
    rgb = lerp_color((58, 138, 52), (128, 196, 74), t)
    flowers = RNG.random((n, n)) > 0.997
    rgb[flowers] = (255, 236, 120)
    return to_image(rgb)


def tex_barrier(n=128):
    """Padded track barrier: 2 panels per texture, v runs top (0) to bottom."""
    u = (np.arange(n) + 0.5) / n
    v = (np.arange(n) + 0.5) / n
    panel = np.floor(u * 2).astype(int) % 2
    cols = np.array([[30, 150, 230], [250, 250, 245]], dtype=float)
    rgb = cols[panel][None, :, :].repeat(n, axis=0)
    # Yellow top rim, dark rubber skirt.
    rgb[v < 0.14] = (255, 208, 40)
    rgb[v > 0.86] = (52, 56, 70)
    rim_shade = 0.75 + 0.25 * np.sin(np.clip((v - 0.14) / 0.72, 0, 1) * math.pi)
    rgb[(v >= 0.14) & (v <= 0.86)] *= rim_shade[(v >= 0.14) & (v <= 0.86)][:, None, None]
    seam = np.abs((u * 2) % 1.0 - 0.5) > 0.485
    rgb[:, seam] *= 0.55
    rgb *= (0.95 + 0.05 * RNG.random((n, n)))[..., None]
    # A white chevron on the blue panels.
    img = to_image(rgb)
    d = ImageDraw.Draw(img)
    for k in range(2):
        if k % 2 == 0:
            cx = int((k + 0.5) * n / 2)
            d.polygon([(cx - 14, 36), (cx + 2, 64), (cx - 14, 92), (cx - 4, 92), (cx + 12, 64), (cx - 4, 36)],
                      fill=(255, 255, 255))
    return img


def tex_wood(n=64):
    grain = fbm(n, n, 2, 4, 61)
    streak = periodic_noise(n, n, 32, 63)
    t = 0.6 * grain + 0.4 * streak
    rgb = lerp_color((150, 104, 62), (206, 160, 104), t)
    return to_image(rgb)


def tex_water(n=128):
    x = np.arange(n)[None, :] / n * 2 * math.pi
    y = np.arange(n)[:, None] / n * 2 * math.pi
    warp = fbm(n, n, 3, 3, 71) * 4.0
    w1 = np.sin(x * 3 + y * 2 + warp)
    w2 = np.sin(x * -2 + y * 5 + warp * 1.3)
    w3 = np.sin(x * 7 - y * 3 + warp * 0.7)
    caustic = np.abs(w1 + w2 * 0.8 + w3 * 0.5)
    lines = np.clip(1.0 - caustic / 0.45, 0, 1) ** 2
    t = fbm(n, n, 4, 4, 73)
    rgb = lerp_color((44, 168, 206), (86, 206, 228), t)
    rgb += (lines * 120)[..., None] * np.array([0.8, 1.0, 1.0])
    return to_image(rgb)


def tex_foam(n=64):
    noise = fbm(n, n, 4, 4, 81)
    lace = fbm(n, n, 8, 3, 83)
    v = (np.arange(n) + 0.5) / n
    # Opaque at the shore edge (v = 0), lacy fading to the sea (v = 1).
    edge = np.clip(1.15 - v * 1.25, 0, 1)[:, None]
    a = np.clip((noise * 0.6 + lace * 0.6 - 0.45) * 3.2, 0, 1) * edge + np.clip(0.35 - v * 3, 0, 1)[:, None]
    rgb = np.full((n, n, 3), 255.0)
    return to_image(rgb, np.clip(a, 0, 1) * 255)


def tex_rock(n=128):
    noise = fbm(n, n, 4, 6, 91, 0.55)
    cracks = np.abs(periodic_noise(n, n, 5, 93) - 0.5) < 0.012
    strata = np.sin(np.arange(n)[:, None] / n * 2 * math.pi * 6 + noise * 5) * 0.5 + 0.5
    t = 0.65 * noise + 0.35 * strata
    rgb = lerp_color((128, 102, 82), (196, 168, 132), t)
    rgb[cracks] *= 0.82
    rgb *= (0.92 + 0.16 * RNG.random((n, n)))[..., None]
    return to_image(rgb)


def tex_bark(n=64):
    v = (np.arange(n) + 0.5) / n
    rings = (np.sin(v * 2 * math.pi * 6) * 0.5 + 0.5) ** 3
    noise = fbm(n, n, 4, 3, 101)
    t = 0.5 * noise + 0.5 * (1 - rings[:, None])
    rgb = lerp_color((112, 82, 50), (176, 140, 96), t)
    return to_image(rgb)


def tex_frond(n=128):
    """Palm leaf: stem along v from base (v=1) to tip (v=0), leaflets both sides."""
    img = Image.new("RGBA", (n * 4, n * 4), (60, 130, 40, 0))
    d = ImageDraw.Draw(img)
    s = 4
    cx = n * s / 2
    leaflets = 22
    for i in range(leaflets):
        t = i / (leaflets - 1)
        y = (n * 0.96 - t * n * 0.92) * s
        length = (n * 0.47) * math.sin(math.pi * (0.12 + 0.86 * t)) ** 0.7 * s
        droop = 0.55 + 0.25 * t
        shade = 0.85 + 0.15 * ((i % 3) / 2)
        col = (int(70 * shade), int(160 * shade), int(52 * shade), 255)
        for side in (-1, 1):
            tip = (cx + side * length, y + length * droop * 0.5)
            mid = (cx + side * length * 0.5, y + length * droop * 0.12)
            d.polygon([(cx, y - 3 * s), mid, tip, (mid[0], mid[1] + 5 * s), (cx, y + 4 * s)], fill=col)
            d.line([(cx, y), mid, tip], fill=(46, 112, 34, 255), width=s)
    d.line([(cx, n * 0.98 * s), (cx, n * 0.03 * s)], fill=(140, 150, 70, 255), width=3 * s)
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_palm_far(n=128):
    img = Image.new("RGBA", (n * 2, n * 2), (60, 130, 40, 0))
    d = ImageDraw.Draw(img)
    s = 2
    top = (n * 1.05, n * 0.55)
    # Curved trunk.
    pts = []
    for i in range(20):
        t = i / 19
        x = n * (1.0 + 0.10 * math.sin(t * 2.2)) * 1.0
        y = (n * 2 - 4) - t * (n * 2 - 4 - top[1])
        pts.append((x, y))
    for i in range(len(pts) - 1):
        w = int(12 - 6 * i / len(pts))
        d.line([pts[i], pts[i + 1]], fill=(130, 98, 62, 255), width=w)
    cx, cy = pts[-1]
    for k in range(9):
        a = -math.pi + k * math.pi / 8 + 0.1
        for j in range(12):
            t = j / 11
            r = 30 + 80 * t
            x = cx + math.cos(a) * r
            y = cy + math.sin(a) * r * 0.45 + 70 * t * t
            d.ellipse([x - 10 * (1 - t) - 4, y - 6, x + 10 * (1 - t) + 4, y + 6],
                      fill=(int(60 + 30 * (k % 2)), int(150 + 20 * (k % 2)), 50, 255))
    img = img.resize((n, n), Image.Resampling.LANCZOS)
    return img


def tex_cloud(w=256, h=128):
    img = np.zeros((h, w, 4))
    yy, xx = np.mgrid[0:h, 0:w]
    rng = np.random.default_rng(111)
    for cell in range(2):
        ox = cell * 128
        dens = np.zeros((h, w))
        for _ in range(16):
            cx = ox + rng.uniform(26, 102)
            cy = rng.uniform(52, 80)
            r = rng.uniform(14, 30)
            dens += np.exp(-(((xx - cx) ** 2) / (r * r * 1.4) + ((yy - cy) ** 2) / (r * r)))
        mask = (xx >= ox + 2) & (xx < ox + 126)
        window = np.clip(np.sin(np.clip((xx - ox - 2) / 124.0, 0, 1) * math.pi), 0, 1) ** 0.6
        vwin = np.clip(np.sin(np.clip(yy / (h - 1.0), 0, 1) * math.pi), 0, 1) ** 0.5
        dens = np.where(mask, dens * window * vwin, 0)
        img[..., 3] += np.clip(dens * 1.3 - 0.15, 0, 1)
        shade = np.clip(1.0 - (yy - 40) / 70.0, 0.72, 1.0)
        img[..., 0] += np.where(mask, 255 * shade, 0)
        img[..., 1] += np.where(mask, 255 * shade, 0)
        img[..., 2] += np.where(mask, 255 * np.clip(shade + 0.06, 0, 1), 0)
    noise = fbm(w, h, 8, 3, 113)
    img[..., 3] *= 0.8 + 0.4 * noise
    return to_image(img[..., :3], np.clip(img[..., 3], 0, 1) * 255)


def tex_mountains(w=256, h=64):
    x = np.arange(w) / w
    ridge = np.zeros(w)
    rng = np.random.default_rng(121)
    for k, amp in ((2, 0.30), (5, 0.16), (11, 0.08), (23, 0.04)):
        ph = rng.uniform(0, 2 * math.pi)
        ridge += amp * (np.sin(x * 2 * math.pi * k + ph) * 0.5 + 0.5)
    ridge = 0.12 + ridge * 0.8
    yy = np.arange(h)[:, None] / h
    height = 1.0 - yy
    alpha = np.clip((ridge[None, :] - height) * 60, 0, 1)
    t = np.clip(height / ridge[None, :], 0, 1)
    rgb = lerp_color((96, 156, 150), (150, 196, 206), t)
    rgb *= 0.92 + 0.12 * fbm(w, h, 8, 3, 123)[..., None]
    return to_image(rgb, alpha * 255)


def radial(n, fn):
    yy, xx = np.mgrid[0:n, 0:n]
    r = np.hypot(xx - (n - 1) / 2, yy - (n - 1) / 2) / (n / 2)
    return fn(r)


def tex_glow(n=64):
    a = radial(n, lambda r: np.clip(np.exp(-r * r * 4.5) * 1.15 - 0.02, 0, 1))
    return to_image(np.full((n, n, 3), 255.0), a * 255)


def tex_smoke(n=64):
    base = radial(n, lambda r: np.clip(1 - r, 0, 1) ** 1.4)
    noise = fbm(n, n, 4, 3, 131)
    a = np.clip(base * (0.6 + 0.8 * noise), 0, 1)
    return to_image(np.full((n, n, 3), 255.0), a * 255)


def tex_shadow(n=32):
    a = radial(n, lambda r: np.clip(1.0 - r, 0, 1) ** 0.8)
    return to_image(np.zeros((n, n, 3)), a * 200)


def tex_star(n=64):
    yy, xx = np.mgrid[0:n, 0:n]
    x = (xx - (n - 1) / 2) / (n / 2)
    y = (yy - (n - 1) / 2) / (n / 2)
    r = np.hypot(x, y)
    spikes = np.exp(-np.abs(x) * 22) * np.exp(-np.abs(y) * 1.6) + np.exp(-np.abs(y) * 22) * np.exp(-np.abs(x) * 1.6)
    a = np.clip(spikes + np.exp(-r * r * 30), 0, 1)
    return to_image(np.full((n, n, 3), 255.0), a * 255)


def tex_flare(n=64):
    a = radial(n, lambda r: np.clip(np.exp(-((r - 0.78) / 0.10) ** 2) * 0.9 + np.clip(0.75 - r, 0, 1) * 0.35, 0, 1))
    return to_image(np.full((n, n, 3), 255.0), a * 255)


def tex_flame(n=64):
    yy, xx = np.mgrid[0:n, 0:n]
    x = (xx - (n - 1) / 2) / (n / 2)
    v = yy / (n - 1)               # v = 0 nozzle, 1 tail
    width = 0.75 * (1 - v) ** 0.6 + 0.05
    core = np.clip(1 - np.abs(x) / width, 0, 1)
    a = np.clip(core * 1.6, 0, 1) * np.clip((1 - v) * 1.4, 0, 1)
    t = np.clip(core * (1 - v) * 1.5, 0, 1)
    rgb = lerp_color((255, 90, 20), (255, 250, 210), t)
    return to_image(rgb, a * 255)


def tex_dash(n=64):
    img = Image.new("RGB", (n * 4, n * 4), (24, 60, 170))
    d = ImageDraw.Draw(img)
    s = 4
    d.rectangle([0, 0, n * s, n * s], fill=(20, 52, 150))
    d.rectangle([4 * s, 0, (n - 4) * s, n * s], fill=(34, 86, 210))
    for k in range(2):
        y0 = k * n * s / 2
        pts = [(8 * s, y0 + 26 * s), (32 * s, y0 + 4 * s), (56 * s, y0 + 26 * s),
               (56 * s, y0 + 34 * s), (32 * s, y0 + 14 * s), (8 * s, y0 + 34 * s)]
        d.polygon(pts, fill=(255, 214, 40))
        d.line(pts + [pts[0]], fill=(255, 140, 20), width=2 * s)
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_checker(n=32):
    yy, xx = np.mgrid[0:n, 0:n]
    c = ((xx // 8) + (yy // 8)) % 2
    rgb = np.where(c[..., None] == 0, np.array([24, 24, 28]), np.array([245, 245, 245])).astype(float)
    return to_image(rgb)


def tex_thatch(n=64):
    v = (np.arange(n) + 0.5) / n
    strands = periodic_noise(n, n, 40, 141)
    rows = (np.sin(v * 2 * math.pi * 4) * 0.5 + 0.5)[:, None]
    t = 0.55 * strands + 0.45 * rows
    rgb = lerp_color((150, 112, 54), (226, 190, 110), t)
    return to_image(rgb)


def tex_crowd(n=64):
    rng = np.random.default_rng(151)
    rgb = np.zeros((n, n, 3)) + np.array([60, 70, 110])
    palette = np.array([[240, 80, 70], [250, 210, 60], [80, 180, 240], [250, 250, 250],
                        [120, 220, 120], [255, 140, 200], [250, 160, 60]], dtype=float)
    img = Image.fromarray(rgb.astype(np.uint8))
    d = ImageDraw.Draw(img)
    for row in range(8):
        for col in range(8):
            cx = col * 8 + 4 + rng.integers(-1, 2)
            cy = row * 8 + 3
            c = tuple(int(x) for x in palette[rng.integers(0, len(palette))])
            d.ellipse([cx - 3, cy + 1, cx + 3, cy + 6], fill=c)
            skin = tuple(int(x) for x in rng.choice([[250, 210, 170], [200, 150, 110], [130, 90, 60]]))
            d.ellipse([cx - 2, cy - 3, cx + 2, cy + 1], fill=skin)
    return img


# ---------------------------------------------------------------------------
# Text: HUD font, banners, title logo, countdown

FONT_CHARS = [chr(c) for c in range(32, 128)]
FONT_COLS, FONT_ROWS = 16, 6
FONT_CELL_W, FONT_CELL_H = 32, 42


def tex_font():
    """White glyphs with a dark outline; tint with vertex colour at runtime."""
    w, h = FONT_COLS * FONT_CELL_W, FONT_ROWS * FONT_CELL_H
    img = Image.new("RGBA", (512, 256), (255, 255, 255, 0))
    font = ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), 30)
    d = ImageDraw.Draw(img)
    advances = []
    for i, ch in enumerate(FONT_CHARS):
        cx = (i % FONT_COLS) * FONT_CELL_W
        cy = (i // FONT_COLS) * FONT_CELL_H
        d.text((cx + 3, cy + 2), ch, font=font, fill=(255, 255, 255, 255),
               stroke_width=3, stroke_fill=(16, 22, 48, 255))
        advances.append(int(round(font.getlength(ch))) + 2)
    assert w <= 512 and h <= 256
    return img, advances


def gradient_text_layer(size, text, font, stops, outline, outline_w, highlight=True, shadow=True):
    """Chunky arcade lettering: drop shadow, thick outline, vertical gradient."""
    w, h = size
    mask = Image.new("L", size, 0)
    md = ImageDraw.Draw(mask)
    bbox = md.textbbox((0, 0), text, font=font)
    tx = (w - (bbox[2] - bbox[0])) / 2 - bbox[0]
    ty = (h - (bbox[3] - bbox[1])) / 2 - bbox[1]
    md.text((tx, ty), text, font=font, fill=255)
    out_mask = Image.new("L", size, 0)
    ImageDraw.Draw(out_mask).text((tx, ty), text, font=font, fill=255, stroke_width=outline_w, stroke_fill=255)
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    if shadow:
        sh = out_mask.filter(ImageFilter.GaussianBlur(3))
        shadow_img = Image.new("RGBA", size, (10, 14, 40, 0))
        shadow_img.putalpha(sh.point(lambda a: int(a * 0.75)))
        layer.alpha_composite(shadow_img, (5, 7))
    ol = Image.new("RGBA", size, outline + (0,))
    ol.putalpha(out_mask)
    layer.alpha_composite(ol)
    ys = np.linspace(0, 1, h)[:, None]
    grad = np.zeros((h, w, 3))
    for i in range(len(stops) - 1):
        p0, c0 = stops[i]
        p1, c1 = stops[i + 1]
        t = np.clip((ys - p0) / max(p1 - p0, 1e-6), 0, 1)
        inside = (ys >= p0) & (ys <= p1)
        grad = np.where(inside[..., None], lerp_color(c0, c1, np.broadcast_to(t, (h, w))), grad)
    fill = Image.fromarray(np.clip(grad, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    fill.putalpha(mask)
    layer.alpha_composite(fill)
    if highlight:
        hl = mask.filter(ImageFilter.GaussianBlur(1)).point(lambda a: 255 if a > 200 else 0)
        shifted = Image.new("L", size, 0)
        shifted.paste(hl, (0, 3))
        rim = np.clip(np.array(hl, dtype=float) - np.array(shifted, dtype=float), 0, 255)
        rim_img = Image.new("RGBA", size, (255, 255, 255, 0))
        rim_img.putalpha(Image.fromarray((rim * 0.9).astype(np.uint8)))
        layer.alpha_composite(rim_img)
    return layer


LOGO_LETTERS = "DreamKts"   # unique glyphs of "DreamKarts"
LOGO_CELL = 128


def tex_logo():
    img = Image.new("RGBA", (512, 256), (0, 0, 0, 0))
    font = ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), 96)
    cap = ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), 112)
    stops = [(0.0, (255, 252, 190)), (0.38, (255, 214, 40)), (0.62, (255, 150, 20)), (1.0, (230, 70, 20))]
    widths = []
    for i, ch in enumerate(LOGO_LETTERS):
        f = cap if ch.isupper() else font
        layer = gradient_text_layer((LOGO_CELL, LOGO_CELL), ch, f, stops, (20, 60, 190), 7)
        img.alpha_composite(layer, ((i % 4) * LOGO_CELL, (i // 4) * LOGO_CELL))
        widths.append(int(f.getlength(ch)) + 10)
    return img, widths


def tex_bigtext():
    """Countdown and finish lettering: 3, 2, 1 (128 cells), GO! and FINISH!."""
    img = Image.new("RGBA", (512, 256), (0, 0, 0, 0))
    big = ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), 100)
    mid = ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), 78)
    warm = [(0.0, (255, 255, 210)), (0.45, (255, 220, 50)), (1.0, (240, 90, 20))]
    cool = [(0.0, (230, 255, 255)), (0.5, (90, 220, 255)), (1.0, (30, 110, 230))]
    for i, txt in enumerate("321"):
        layer = gradient_text_layer((128, 128), txt, big, warm, (150, 20, 20), 7)
        img.alpha_composite(layer, (i * 128, 0))
    go = gradient_text_layer((128, 128), "GO!", ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), 62),
                             cool, (10, 40, 140), 6)
    img.alpha_composite(go, (384, 0))
    fin = gradient_text_layer((512, 128), "FINISH!", mid, warm, (150, 20, 20), 7)
    img.alpha_composite(fin, (0, 128))
    return img


def tex_banner():
    img = Image.new("RGB", (256, 128), (20, 70, 190))
    d = ImageDraw.Draw(img)
    for y in range(128):
        t = (y % 64) / 63
        c = (int(20 + 30 * t), int(80 + 60 * t), int(200 + 40 * t))
        d.line([(0, y), (256, y)], fill=c)
    font = ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), 34)
    small = ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), 27)
    stops = [(0.0, (255, 255, 210)), (0.5, (255, 214, 40)), (1.0, (245, 120, 20))]
    top = gradient_text_layer((256, 64), "DREAMKARTS", font, stops, (10, 30, 110), 4, shadow=False)
    bot = gradient_text_layer((256, 64), "EMERALD BEACH", small, [(0, (255, 255, 255)), (1, (180, 240, 255))],
                              (10, 30, 110), 4, shadow=False)
    out = img.convert("RGBA")
    out.alpha_composite(top, (0, 0))
    out.alpha_composite(bot, (0, 64))
    d = ImageDraw.Draw(out)
    d.rectangle([0, 0, 255, 3], fill=(255, 255, 255, 255))
    d.rectangle([0, 60, 255, 67], fill=(255, 255, 255, 255))
    d.rectangle([0, 124, 255, 127], fill=(255, 255, 255, 255))
    return out.convert("RGB")


# ---------------------------------------------------------------------------

@dataclass
class Spec:
    name: str
    fmt: str          # RGB565 or ARGB4444
    mip: bool
    wrap: bool


def build(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    previews = out / "previews"
    previews.mkdir(exist_ok=True)
    font_img, font_adv = tex_font()
    logo_img, logo_w = tex_logo()
    items: list[tuple[Spec, Image.Image]] = [
        (Spec("road", "RGB565", True, True), tex_road()),
        (Spec("boardwalk", "RGB565", True, True), tex_boardwalk()),
        (Spec("curb", "RGB565", True, True), tex_curb()),
        (Spec("sand", "RGB565", True, True), tex_sand()),
        (Spec("grass", "RGB565", True, True), tex_grass()),
        (Spec("barrier", "RGB565", True, True), tex_barrier()),
        (Spec("wood", "RGB565", True, True), tex_wood()),
        (Spec("water", "RGB565", True, True), tex_water()),
        (Spec("foam", "ARGB4444", True, True), tex_foam()),
        (Spec("rock", "RGB565", True, True), tex_rock()),
        (Spec("bark", "RGB565", True, True), tex_bark()),
        (Spec("frond", "ARGB4444", True, False), tex_frond()),
        (Spec("palm_far", "ARGB4444", True, False), tex_palm_far()),
        (Spec("cloud", "ARGB4444", False, False), tex_cloud()),
        (Spec("mountains", "ARGB4444", False, True), tex_mountains()),
        (Spec("glow", "ARGB4444", True, False), tex_glow()),
        (Spec("smoke", "ARGB4444", True, False), tex_smoke()),
        (Spec("shadow", "ARGB4444", False, False), tex_shadow()),
        (Spec("star", "ARGB4444", True, False), tex_star()),
        (Spec("flare", "ARGB4444", True, False), tex_flare()),
        (Spec("flame", "ARGB4444", True, False), tex_flame()),
        (Spec("dash", "RGB565", True, True), tex_dash()),
        (Spec("checker", "RGB565", True, True), tex_checker()),
        (Spec("thatch", "RGB565", True, True), tex_thatch()),
        (Spec("crowd", "RGB565", True, True), tex_crowd()),
        (Spec("banner", "RGB565", False, False), tex_banner()),
        (Spec("font", "ARGB4444", False, False), font_img),
        (Spec("logo", "ARGB4444", False, False), logo_img),
        (Spec("bigtext", "ARGB4444", False, False), tex_bigtext()),
    ]
    pak = bytearray()
    table = []
    for spec, img in items:
        img.save(previews / f"tex-{spec.name}.png")
        words = pack_texture(img, spec.fmt, mipmap=spec.mip, wrap=spec.wrap)
        data = np.array(words, dtype="<u2").tobytes()
        while len(pak) % 32:
            pak.append(0)
        table.append((spec, img.width, img.height, len(pak), len(data)))
        pak += data
        print(f"  {spec.name:10s} {img.width}x{img.height} {spec.fmt}{' mip' if spec.mip else ''} {len(data)} bytes")
    (out / "textures.pak").write_bytes(bytes(pak))

    h = ["/* Generated by tools/build_textures.py. Do not edit. */",
         "#ifndef DK_TEXTURE_ASSETS_H", "#define DK_TEXTURE_ASSETS_H", "#include <stdint.h>", "",
         "#define DK_TEXFMT_RGB565 0", "#define DK_TEXFMT_ARGB4444 1", "",
         "typedef struct { uint32_t offset, bytes; uint16_t w, h; uint8_t fmt, mip, pad[2]; } dk_tex_asset_t;",
         "", "enum {"]
    for spec, *_ in table:
        h.append(f"    TEX_{spec.name.upper()},")
    h += ["    TEX_COUNT", "};", "",
          "static const dk_tex_asset_t dk_tex_assets[TEX_COUNT] = {"]
    for spec, w, hh, off, size in table:
        fmt = "DK_TEXFMT_ARGB4444" if spec.fmt == "ARGB4444" else "DK_TEXFMT_RGB565"
        h.append(f"    {{{off}u, {size}u, {w}, {hh}, {fmt}, {int(spec.mip)}, {{0, 0}}}},  /* {spec.name} */")
    h += ["};", "", f"#define DK_TEXTURES_BYTES {len(pak)}u", "", "#endif", ""]
    (out / "texture_assets.h").write_text("\n".join(h))

    m = ["/* Generated by tools/build_textures.py. Do not edit. */",
         "#ifndef DK_FONT_METRICS_H", "#define DK_FONT_METRICS_H", "#include <stdint.h>", "",
         f"#define FONT_COLS {FONT_COLS}", f"#define FONT_CELL_W {FONT_CELL_W}",
         f"#define FONT_CELL_H {FONT_CELL_H}", "#define FONT_TEX_W 512", "#define FONT_TEX_H 256",
         "/* Pixel advance of ASCII 32..127 at native size (cell height 42). */",
         "static const uint8_t font_advance[96] = {"]
    for i in range(0, 96, 16):
        m.append("    " + ", ".join(str(a) for a in font_adv[i:i + 16]) + ",")
    m += ["};", "",
          f'#define LOGO_LETTERS "{LOGO_LETTERS}"', f"#define LOGO_CELL {LOGO_CELL}",
          "static const uint8_t logo_advance[8] = {" + ", ".join(str(w) for w in logo_w) + "};",
          "", "#endif", ""]
    (out / "font_metrics.h").write_text("\n".join(m))
    print(f"textures.pak: {len(pak)} bytes, {len(table)} textures")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, required=True)
    args = p.parse_args()
    build(args.output_dir.resolve())


if __name__ == "__main__":
    main()
