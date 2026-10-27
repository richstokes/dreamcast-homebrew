#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "pillow>=10.1", "matplotlib>=3.8"]
# ///
"""Procedurally paint every DreamKarts texture and pack it for the PowerVR.

Everything is drawn from code (seeded noise, shapes, Lilita One from
tools/fonts/ for the menus' lettering and the DejaVu fonts that ship with
matplotlib for trackside signs), so the art is original and the output is
reproducible. Writes to --output-dir:

    textures.pak        twiddled RGB565/ARGB4444 textures (+mips), 32-byte aligned
    texture_assets.h    TEX_* ids and the size/format/offset table
    font_metrics.h      HUD font glyph rects, countdown cells, title logo letter cells
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


def tex_stone(n=128):
    """Weathered sandstone blocks in staggered courses (Temple Hill ruins)."""
    rows, cols = 4, 2
    y = np.arange(n)[:, None].repeat(n, 1)
    x = np.arange(n)[None, :].repeat(n, 0)
    bh, bw = n // rows, n // cols
    row = y // bh
    xs = (x + (row % 2) * (bw // 2)) % n
    col = xs // bw
    fy, fx = y % bh, xs % bw
    mortar = (fy < 3) | (fx < 3)
    block = row * cols + col
    tone = np.array([0.86, 1.0, 0.93, 1.06, 0.9, 0.97, 1.03, 0.88])[block % 8]
    noise = fbm(n, n, 4, 5, 131, 0.55)
    pits = periodic_noise(n, n, 16, 137) > 0.8
    t = np.clip(0.55 * noise + 0.45 * tone - 0.35, 0, 1)
    rgb = lerp_color((150, 128, 96), (222, 200, 156), t)
    # Bevel: lit top-left edges, shaded bottom-right edges of each block.
    rgb[(fy >= 3) & (fy < 5) & ~mortar] *= 1.12
    rgb[(fy > bh - 3) & ~mortar] *= 0.8
    rgb[(fx > bw - 3) & ~mortar] *= 0.84
    rgb[pits & ~mortar] *= 0.85
    rgb[mortar] = rgb[mortar] * 0.55 + np.array([40, 34, 26]) * 0.45
    return to_image(np.clip(rgb, 0, 255))


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


def tex_retrosun(n=128):
    """A retrowave sun: yellow at the top to hot pink at the bottom, with
    horizontal gaps from just above its middle that widen towards the
    bottom."""
    yy, xx = np.mgrid[0:n, 0:n]
    u = (xx + 0.5) / n * 2 - 1
    v = (yy + 0.5) / n * 2 - 1   # -1 top, +1 bottom
    r = np.hypot(u, v)
    alpha = np.clip((0.97 - r) * n * 0.5, 0, 1)
    for i in range(6):
        c, hw = -0.12 + i * 0.17, 0.02 + 0.014 * i
        alpha *= np.clip((np.abs(v - c) - hw) * n * 0.5, 0, 1)
    t = np.clip((v + 1) / 2, 0, 1)[..., None]
    top, mid, bot = np.array([255, 238, 120.0]), np.array([255, 150, 70.0]), np.array([255, 48, 140.0])
    rgb = np.where(t < 0.5, top + (mid - top) * (t / 0.5), mid + (bot - mid) * ((t - 0.5) / 0.5))
    return to_image(rgb, alpha * 255)


def tex_smoke(n=64):
    base = radial(n, lambda r: np.clip(1 - r, 0, 1) ** 1.4)
    noise = fbm(n, n, 4, 3, 131)
    a = np.clip(base * (0.6 + 0.8 * noise), 0, 1)
    return to_image(np.full((n, n, 3), 255.0), a * 255)


def tex_shadow(n=64):
    """Kart footprint seen from above: u across the kart, v along it. The
    quad is 1.76 m by 2.4 m (models.c), so the kart's 1.3 m by 2.1 m body is
    a rounded box whose soft edge fades out by 0.8 by 0.9, and darker
    contact patches under the four tyres."""
    yy, xx = np.mgrid[0:n, 0:n]
    x = (xx - (n - 1) / 2) / (n / 2)
    y = (yy - (n - 1) / 2) / (n / 2)
    hx, hy, rc = 0.56, 0.66, 0.30
    qx = np.abs(x) - (hx - rc)
    qy = np.abs(y) - (hy - rc)
    sd = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - rc
    t = np.clip(1 - (sd + 0.06) / 0.30, 0, 1)
    body = t * t * (3 - 2 * t)
    wheel = np.zeros_like(x)
    for wx in (-0.64, 0.64):
        for wy in (-0.52, 0.52):
            w = np.clip(1 - np.hypot((x - wx) / 0.17, (y - wy) / 0.24), 0, 1)
            wheel = np.maximum(wheel, w ** 0.7)
    a = np.clip(body * 0.78 + wheel * 0.22, 0, 1)
    return to_image(np.zeros((n, n, 3)), a * 255)


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
FONT_ROW_H = 42
FONT_SIZE = 30
FONT_CAP_H = 22      # cap height every UI font is sized to, so hud_text scales stay put
FONT_BASELINE = 30   # baseline row within the 42 px glyph row


# The menus' lettering: Lilita One (OFL, tools/fonts/) for the HUD font,
# logo and countdown. The outline is 2 px: 3 px pushed Q's tail out of the
# 42 px glyph row.
UI_FONT = str(Path(__file__).resolve().parent / "fonts" / "LilitaOne-Regular.ttf")
UI_OUTLINE = 2
LOGO_SIZES = (110, 94)   # capitals, lowercase
LOGO_OUTLINE = 7


def ui_font() -> ImageFont.FreeTypeFont:
    """The HUD font at the size whose capitals stand FONT_CAP_H tall (the
    DejaVu Bold Oblique the game shipped with, at 30 px), so every hud_text
    scale in the game keeps its size whatever the font."""
    best = None
    for size in range(20, 48):
        f = ImageFont.truetype(UI_FONT, size)
        bb = f.getbbox("H")
        cap = bb[3] - bb[1]
        if best is None or abs(cap - FONT_CAP_H) < abs(best[0] - FONT_CAP_H):
            best = (cap, f)
    return best[1]


def tex_font():
    """White glyphs with a dark outline; tint with vertex colour at runtime.

    The glyphs and their outline overhang any fixed cell (DejaVu's W is
    38 px wide, J starts 5 px left of the pen), so each glyph is packed into
    its own rect: its ink plus one transparent texel each side, which keeps
    bilinear filtering from sampling a neighbour. Returns the atlas and, per
    glyph, (x, row, width, left offset from the pen, advance)."""
    img = Image.new("RGBA", (512, 256), (255, 255, 255, 0))
    font = ui_font()
    stroke = UI_OUTLINE
    pad = 16
    # The baseline sits FONT_BASELINE rows down, as the original font's did,
    # unless a tall ascender or deep descender (plus the outline) would leave
    # the row: then the whole alphabet shifts to fit.
    ascent = font.getmetrics()[0]
    ty = FONT_BASELINE - ascent
    lo, hi = FONT_ROW_H, 0
    for ch in FONT_CHARS:
        if ch == " ":
            continue
        cell = Image.new("L", (64 + 2 * pad, FONT_ROW_H * 3), 0)
        ImageDraw.Draw(cell).text((pad + 3, FONT_ROW_H + ty), ch, font=font, fill=255,
                                  stroke_width=stroke, stroke_fill=255)
        ys = np.nonzero(np.array(cell))[0]
        if len(ys):
            lo, hi = min(lo, ys.min() - FONT_ROW_H), max(hi, ys.max() - FONT_ROW_H)
    if lo < 1:
        ty += 1 - lo
    elif hi > FONT_ROW_H - 2:
        ty -= hi - (FONT_ROW_H - 2)
    glyphs = []
    x = row = 0
    for ch in FONT_CHARS:
        adv = int(round(font.getlength(ch))) + 2
        if ch == " ":
            glyphs.append((0, 0, 0, 0, adv))
            continue
        cell = Image.new("RGBA", (64 + 2 * pad, FONT_ROW_H), (255, 255, 255, 0))
        ImageDraw.Draw(cell).text((pad + 3, ty), ch, font=font, fill=(255, 255, 255, 255),
                                  stroke_width=stroke, stroke_fill=(16, 22, 48, 255))
        alpha = np.array(cell)[..., 3]
        ys, xs = np.nonzero(alpha)
        if not len(ys):   # a glyph the font lacks (DEL) draws as a blank
            glyphs.append((0, 0, 0, 0, adv))
            continue
        assert ys.min() >= 1 and ys.max() <= FONT_ROW_H - 2, (ch, ys.min(), ys.max())
        x0, x1 = xs.min() - 1, xs.max() + 2      # rect with a clear texel each side
        w = x1 - x0
        if x + w > img.width:
            x, row = 0, row + 1
        assert (row + 1) * FONT_ROW_H <= img.height, "font atlas full"
        img.alpha_composite(cell.crop((x0, 0, x1, FONT_ROW_H)), (x, row * FONT_ROW_H))
        glyphs.append((x, row, w, x0 - pad, adv))
        x += w
    # Tabular digits: the font's are proportional (its 1 is narrower than
    # its 0), which made a ticking race clock change width every frame. Each
    # digit takes the widest one's advance, its ink centred in it, so any
    # reading keeps its width whatever its digits.
    tab = max(glyphs[ord(c) - 32][4] for c in "0123456789")
    for c in "0123456789":
        gx, grow, gw, gox, gadv = glyphs[ord(c) - 32]
        glyphs[ord(c) - 32] = (gx, grow, gw, gox + (tab - gadv) // 2, tab)
    return img, glyphs


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
    cap_px, low_px = LOGO_SIZES
    font = ImageFont.truetype(UI_FONT, low_px)
    cap = ImageFont.truetype(UI_FONT, cap_px)
    stops = [(0.0, (255, 252, 190)), (0.38, (255, 214, 40)), (0.62, (255, 150, 20)), (1.0, (230, 70, 20))]
    widths = []
    for i, ch in enumerate(LOGO_LETTERS):
        f = cap if ch.isupper() else font
        layer = gradient_text_layer((LOGO_CELL, LOGO_CELL), ch, f, stops, (20, 60, 190), LOGO_OUTLINE)
        img.alpha_composite(layer, ((i % 4) * LOGO_CELL, (i // 4) * LOGO_CELL))
        widths.append(int(f.getlength(ch)) + 10)
    return img, widths


# Countdown cells in the top row of the bigtext texture: x, width (128 tall).
BIGTEXT_CELLS = [(0, 112), (112, 112), (224, 112), (336, 176)]


def tex_bigtext():
    """Countdown and finish lettering: 3, 2, 1, GO! and FINISH!.

    Each cell is wide enough for its lettering's outline and drop shadow, so
    nothing is cut off at a cell edge; a font whose lettering would clip is
    stepped down in size until it fits."""
    img = Image.new("RGBA", (512, 256), (0, 0, 0, 0))
    warm = [(0.0, (255, 255, 210)), (0.45, (255, 220, 50)), (1.0, (240, 90, 20))]
    cool = [(0.0, (230, 255, 255)), (0.5, (90, 220, 255)), (1.0, (30, 110, 230))]

    def fitted(size, txt, w, stops, outline, outline_w):
        while True:
            layer = gradient_text_layer((w, 128), txt, ImageFont.truetype(UI_FONT, size), stops, outline, outline_w)
            a = np.array(layer)[..., 3]
            if not (a[0].any() or a[-1].any() or a[:, 0].any() or a[:, -1].any()):
                return layer
            size -= 2

    layers = [fitted(100, txt, w, warm, (150, 20, 20), 7) for txt, (_, w) in zip("321", BIGTEXT_CELLS)]
    layers.append(fitted(62, "GO!", BIGTEXT_CELLS[3][1], cool, (10, 40, 140), 6))
    for layer, (x, _) in zip(layers, BIGTEXT_CELLS):
        img.alpha_composite(layer, (x, 0))
    img.alpha_composite(fitted(78, "FINISH!", 512, warm, (150, 20, 20), 7), (0, 128))
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


# ---------------------------------------------------------------------------
# Neon Skyway (night, cyberpunk). Each draws from its own seeded generator so
# the shared RNG, and every texture above, stays as it was.

SKYROAD_PHASES = 16
CYAN, MAGENTA = np.array([60, 220, 255.0]), np.array([255, 50, 200.0])
FOG_NIGHT = (20, 16, 56)


def side_colour(d):
    """Cyan on the left of the road, through violet, to magenta on the right."""
    t = np.clip((np.asarray(d, dtype=float) + 9.6) / 19.2, 0, 1)[..., None]
    mid = np.array([150, 70, 255.0])
    return np.where(t < 0.5, CYAN + (mid - CYAN) * (t * 2), mid + (MAGENTA - mid) * (t * 2 - 1))


def tex_skyroad(phase, n=64):
    """The sky road across its full 19.2 m width (u) and one repeat (~8 m)
    along it (v): a dark glossy deck under a neon grid, then an edge strip
    of running lights (cyan left, magenta right) and a bright lip. Phase
    moves a chevron of light and the running lights forward one sixteenth
    of a repeat."""
    d = ((np.arange(n) + 0.5) / n - 0.5) * 19.2           # metres from the centreline
    v = (np.arange(n) + 0.5) / n
    ph = phase / SKYROAD_PHASES
    deck = np.array([16, 10, 46.0]) * (0.85 + 0.3 * fbm(n, n, 4, 3, 4003))[..., None]
    deck *= (1.0 + 0.35 * np.exp(-(d / 6.0) ** 2))[None, :, None]          # sheen down the middle
    col = side_colour(d)[None, :, :]
    # The grid: six lanes along, a cross line every quarter repeat (~2 m).
    lane_w = 16.0 / 6
    along = np.exp(-((((d + 8.0) / lane_w + 0.5) % 1.0 - 0.5) * lane_w / 0.16) ** 2) * (np.abs(d) < 7.8)
    cross = np.exp(-((((v * 4) + 0.5) % 1.0 - 0.5) / 0.07) ** 2)
    grid = np.maximum(along[None, :] * 0.85, cross[:, None] * 0.45 * (np.abs(d) < 8.0)[None, :])
    # Travelling chevron: tip forward at the centre, the edges lag.
    wave_v = (v[:, None] - ph + np.abs(d)[None, :] / 9.6 * 0.22) % 1.0
    wave_v = np.minimum(wave_v, 1 - wave_v)
    pulse = np.exp(-(wave_v / 0.06) ** 2) * (np.abs(d) < 8.0)[None, :]
    rgb = deck + col * (grid * (0.75 + 0.6 * pulse))[..., None] + \
        (CYAN * 0.55 + 60)[None, None, :] * (pulse * 0.55)[..., None]
    # Edge strips: dark glass with running lights four to a repeat.
    edge = np.abs(d) >= 8.0
    run = (v[:, None] * 4 - ph * 4) % 1.0
    run = np.minimum(run, 1 - run)
    dots = np.exp(-(run / 0.12) ** 2) * np.exp(-((np.abs(d)[None, :] - 8.8) / 0.32) ** 2)
    lit = np.where(d[None, :, None] < 0, CYAN, MAGENTA)
    edge_rgb = np.array([10, 8, 30.0]) + dots[..., None] * lit
    lip = np.abs(d) > 9.25
    edge_rgb[:, lip] = np.where(d[lip, None] < 0, CYAN * 0.6 + 100, MAGENTA * 0.6 + 100)
    rgb = np.where(edge[None, :, None], edge_rgb, rgb)
    return to_image(rgb)


def sky_elevation(v):
    """Elevation in degrees at sky texture row v (0 = zenith, 1 = horizon):
    the 45 degrees above the horizon get 70% of the rows, so stars keep
    about the same density across and up where the camera looks."""
    f = 1.0 - np.asarray(v, dtype=float)
    return np.where(f < 0.7, f / 0.7 * 45.0, 45.0 + (f - 0.7) / 0.3 * 45.0)


def tex_nightsky(w=256, h=256):
    """A sky dome band: v=0 is the zenith, v=1 the horizon (which matches the
    night fog). Six repeats go round the sky, so stars stay small. Deep blue
    overhead and a magenta glow from the city along the horizon."""
    rng = np.random.default_rng(4011)
    v = (np.arange(h) + 0.5) / h
    el = sky_elevation(v)
    stops = [(0.0, FOG_NIGHT), (2.0, (96, 26, 110)), (5.0, (44, 22, 92)), (12.0, (14, 20, 70)),
             (30.0, (6, 10, 40)), (90.0, (2, 3, 14))]
    grad = np.zeros((h, 3))
    for (e0, c0), (e1, c1) in zip(stops, stops[1:]):
        m = (el >= e0) & (el <= e1)
        t = (el[m] - e0) / (e1 - e0)
        grad[m] = np.array(c0) + (np.array(c1) - np.array(c0)) * (t * t * (3 - 2 * t))[:, None]
    rgb = np.repeat(grad[:, None, :], w, axis=1)
    # A faint band of the galaxy, crossing the tile.
    yy, xx = np.mgrid[0:h, 0:w]
    band_c = 0.45 * h + 0.18 * h * np.sin(xx / w * 2 * math.pi)
    band = np.exp(-((yy - band_c) / (0.10 * h)) ** 2) * (0.4 + 0.8 * fbm(w, h, 8, 4, 4013))
    band *= np.clip((el[:, None] - 6.0) / 20.0, 0, 1)
    rgb += band[..., None] * np.array([16, 24, 56])
    # Stars, fading into the horizon glow.
    n_stars = 1400
    sx = rng.integers(0, w, n_stars)
    sy = rng.integers(0, h, n_stars)
    mag = rng.random(n_stars) ** 3
    tint = rng.choice([0, 1, 2], n_stars, p=[0.7, 0.2, 0.1])
    tints = np.array([[255, 255, 255], [170, 210, 255], [255, 200, 240]], dtype=float)
    for x, y, m, t in zip(sx, sy, mag, tint):
        fade = np.clip((el[y] - 3.0) / 12.0, 0, 1)
        b = (0.30 + 0.70 * m) * fade
        rgb[y, x] = rgb[y, x] * (1 - b) + tints[t] * b
        if m > 0.8:    # the brightest get a small cross
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx2, yy2 = (x + dx) % w, min(max(y + dy, 0), h - 1)
                rgb[yy2, xx2] = rgb[yy2, xx2] * (1 - b * 0.4) + tints[t] * b * 0.4
    return to_image(rgb)


def tex_skyline(w=256, h=64):
    """The far city on the horizon: tower silhouettes with lit windows and
    neon crowns, hazy since it is far off."""
    rng = np.random.default_rng(4021)
    rgb = np.zeros((h, w, 3))
    a = np.zeros((h, w))
    x = 0
    neon = [(255, 50, 200), (60, 220, 255), (150, 70, 255), (255, 120, 230)]
    windows = [np.array([120, 210, 255.0]), np.array([255, 120, 220.0]), np.array([255, 210, 150.0])]
    while x < w:
        bw = int(rng.integers(5, 14))
        top = int(rng.integers(int(h * 0.08), int(h * 0.75)))
        body = np.array([14, 16, 44], dtype=float) * (0.8 + 0.4 * rng.random())
        x1 = min(x + bw, w)
        a[top:, x:x1] = 1.0
        rgb[top:, x:x1] = body
        for yy in range(top + 2, h, 3):
            for xx in range(x + 1, x1 - 1, 2):
                if rng.random() < 0.34:
                    c = windows[0] if rng.random() < 0.5 else windows[int(rng.integers(1, 3))]
                    rgb[yy, xx] = c * (0.55 + 0.45 * rng.random())
        c = np.array(neon[int(rng.integers(0, len(neon)))], dtype=float)
        if rng.random() < 0.6:
            rgb[top, x:x1] = c
            if top + 1 < h:
                rgb[top + 1, x:x1] = c * 0.6
        else:
            mx = (x + x1) // 2
            a[max(top - 4, 0):top, mx] = 1.0
            rgb[max(top - 4, 0):top, mx] = (40, 40, 80)
            rgb[max(top - 5, 0), mx] = (255, 60, 90)
            a[max(top - 5, 0), mx] = 1.0
        x = x1 + int(rng.integers(0, 3))
    v = (np.arange(h) / (h - 1))[:, None, None]
    haze = np.clip((v - 0.45) / 0.55, 0, 1) * 0.65 + 0.15
    rgb = rgb * (1 - haze) + np.array(FOG_NIGHT) * 1.6 * haze
    return to_image(rgb, a * 255)


def tex_moon(n=64):
    """A crescent moon, pale gold."""
    yy, xx = np.mgrid[0:n, 0:n]
    x = (xx - (n - 1) / 2) / (n * 0.42)
    y = (yy - (n - 1) / 2) / (n * 0.42)
    disc = np.clip((1.0 - np.hypot(x, y)) * n * 0.25, 0, 1)
    bite = np.clip((1.0 - np.hypot(x - 0.42, y + 0.22) / 0.88) * n * 0.25, 0, 1)
    a = np.clip(disc - bite, 0, 1)
    lum = 0.88 + 0.12 * fbm(n, n, 4, 3, 4031)
    rgb = lum[..., None] * np.array([255, 236, 160])
    return to_image(rgb, a * 255)


def tex_tower(n=64):
    """A tower's facade: dark glass in a 16 x 16 grid of floors and bays,
    a little under half the windows lit (cool white, cyan, warm or pink),
    with a lit floor here and there."""
    rng = np.random.default_rng(4051)
    rgb = np.zeros((n, n, 3)) + np.array([10, 10, 28.0])
    cols = [np.array(c, dtype=float) for c in ((200, 230, 255), (90, 210, 255), (255, 200, 140), (255, 110, 210))]
    for r in range(16):
        floor_lit = rng.random() < 0.12
        rgb[r * 4 + 3, :] = (20, 20, 48)
        for c in range(16):
            if floor_lit or rng.random() < 0.42:
                col = cols[int(rng.choice(4, p=[0.4, 0.3, 0.2, 0.1]))] * (0.45 + 0.55 * rng.random())
            else:
                col = np.array([20, 22, 50.0])
            rgb[r * 4:r * 4 + 2, c * 4:c * 4 + 3] = col
    return to_image(rgb)


SIGN_WORDS = [("DREAMKARTS", CYAN), ("CHAO CHAO", MAGENTA), ("NEON 24H", np.array([150, 90, 255.0])),
              ("SKYWAY", np.array([255, 120, 230.0]))]


def tex_signs(w=128, h=128):
    """Four neon signs, one per 32-pixel row, glowing on black panels."""
    s = 4
    img = Image.new("RGB", (w * s, h * s), (6, 4, 16))
    font = ImageFont.truetype(font_path("DejaVuSans-Bold.ttf"), 22 * s)
    glow = Image.new("RGB", img.size, (0, 0, 0))
    gd, d = ImageDraw.Draw(glow), ImageDraw.Draw(img)
    for i, (word, c) in enumerate(SIGN_WORDS):
        cy = (i * 32 + 16) * s
        col = tuple(int(x) for x in c)
        bbox = d.textbbox((0, 0), word, font=font)
        tw = bbox[2] - bbox[0]
        scale_x = min(1.0, (w * s - 16 * s) / tw)
        layer = Image.new("RGB", (tw + 8 * s, 32 * s), (0, 0, 0))
        ImageDraw.Draw(layer).text((4 * s - bbox[0], 16 * s - (bbox[1] + bbox[3]) // 2), word, font=font, fill=col)
        layer = layer.resize((int(layer.width * scale_x), layer.height), Image.Resampling.LANCZOS)
        glow.paste(layer, ((w * s - layer.width) // 2, cy - 16 * s))
        gd.rectangle([2 * s, cy - 14 * s, w * s - 2 * s, cy + 14 * s], outline=col, width=s)
    halo = glow.filter(ImageFilter.GaussianBlur(4 * s))
    out = np.clip(np.asarray(img, dtype=float) + np.asarray(halo, dtype=float) * 1.3 +
                  np.asarray(glow, dtype=float), 0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGB").resize((w, h), Image.Resampling.LANCZOS)


def tex_cloudsea(n=128):
    """Tops of the cloud sea far below the road: puffy and tileable."""
    base = fbm(n, n, 4, 5, 4041, gain=0.55)
    puffs = np.clip((base - 0.38) * 2.4, 0, 1) ** 0.8
    lum = 0.35 + 0.65 * puffs
    return to_image(np.repeat((lum * 255)[..., None], 3, axis=2))


# ---------------------------------------------------------------------------
# Spiral Canyon (a spring valley: meadows, blue rock, a cobbled road and
# trails of orange beads).

ORANGE = np.array([255, 150, 50.0])
ORANGE_DARK = np.array([205, 90, 20.0])
GRASS = np.array([118, 192, 100.0])


def tex_canyonroad(n=64):
    """The canyon road's cobbles over 16 m (u) and one ~8 m repeat (v): eight
    across, six along, sand in the joints; the curbs beside it are the
    shared curb texture, as on the day tracks."""
    d = ((np.arange(n) + 0.5) / n - 0.5) * 16.0
    v = (np.arange(n) + 0.5) / n
    cob_w, cob_l = 16.0 / 8, 1.0 / 6
    across = ((d + 8.0) / cob_w) % 1.0
    along = (v / cob_l) % 1.0
    across = np.where((np.floor(v / cob_l) % 2)[:, None] == 1, (across[None, :] + 0.5) % 1.0, across[None, :])
    edge = np.minimum(np.minimum(across, 1 - across) * cob_w, np.minimum(along, 1 - along)[:, None] * cob_l * 8.0)
    joint = np.clip(1.0 - edge / 0.16, 0, 1)
    tone = fbm(n, n, 6, 3, 5003)
    stone = lerp_color((214, 192, 150), (240, 224, 186), tone)
    stone *= (1.0 - 0.08 * np.abs(d / 8.0))[None, :, None]
    sand = np.array([176, 150, 110.0])
    rgb = stone * (1 - joint)[..., None] + sand[None, None, :] * joint[..., None]
    return to_image(rgb)


def tex_meadow(n=128):
    """An island's top: vivid spring grass with light streaks and a
    scatter of flowers."""
    rng = np.random.default_rng(5041)
    g = fbm(n, n, 6, 4, 5043)
    streak = np.clip((fbm(n, n, 3, 2, 5045) - 0.5) * 5, 0, 1)
    rgb = lerp_color((88, 168, 80), (142, 214, 104), g) + streak[..., None] * np.array([50, 44, 18.0])
    flowers = [(255, 150, 200), (255, 236, 110), (255, 255, 255), (255, 170, 80), (190, 150, 255)]
    for _ in range(110):
        x, y = int(rng.integers(1, n - 1)), int(rng.integers(1, n - 1))
        c = np.array(flowers[int(rng.integers(0, len(flowers)))], dtype=float)
        rgb[y, x] = c
        rgb[y, (x + 1) % n] = rgb[y, (x + 1) % n] * 0.4 + c * 0.6
        rgb[(y + 1) % n, x] = rgb[(y + 1) % n, x] * 0.4 + c * 0.6
    return to_image(rgb)


def tex_islandrock(n=64):
    """Slate: cool grey-blue rock streaked with white, cracked."""
    base = fbm(n, n, 5, 4, 5051)
    streak = np.clip((fbm(n, n, 3, 3, 5053) - 0.52) * 6, 0, 1)
    cracks = np.clip(1 - np.abs(fbm(n, n, 6, 3, 5055) - 0.5) * 10, 0, 1)
    rgb = lerp_color((122, 128, 152), (204, 208, 220), base) + streak[..., None] * np.array([44, 44, 32.0])
    rgb *= (1 - 0.3 * cracks)[..., None]
    return to_image(rgb)


# ---------------------------------------------------------------------------
# Spiral Canyon's hill-zone touches: checkered loops and outcrops,
# sunflowers and gold rings.

def tex_checker_rock(c0, c1, seed, n=64):
    """Checkered rock: four cells across, each with a soft darker edge and a
    little grain, tileable."""
    yy, xx = np.mgrid[0:n, 0:n]
    cell = n // 4
    which = ((xx // cell) + (yy // cell)) % 2
    rgb = np.where(which[..., None] == 0, np.array(c0, dtype=float), np.array(c1, dtype=float))
    ex = np.minimum(xx % cell, cell - 1 - xx % cell)
    ey = np.minimum(yy % cell, cell - 1 - yy % cell)
    edge = np.clip(1.0 - np.minimum(ex, ey) / 1.5, 0, 1) * 0.22
    rgb = rgb * (1 - edge)[..., None]
    rgb *= (0.94 + 0.12 * fbm(n, n, 8, 2, seed))[..., None]
    return to_image(rgb)


def tex_waterfall(w=64, h=64):
    """Falling water: white streaks over pale blue, tileable along v so it
    can scroll, thinner and more see-through between the streaks."""
    rng = np.random.default_rng(5201)
    yy, xx = np.mgrid[0:h, 0:w]
    streak = np.zeros((h, w))
    for _ in range(14):
        x0 = rng.integers(0, w)
        width = 0.6 + rng.random() * 1.4
        wob = rng.random() * 6.28
        xs = (x0 + 1.2 * np.sin(yy / h * 2 * math.pi + wob)) % w
        dx = np.minimum(np.abs(xx - xs), w - np.abs(xx - xs))
        streak = np.maximum(streak, np.exp(-(dx / width) ** 2) * (0.6 + 0.4 * rng.random()))
    grain = fbm(w, h, 2, 3, 5203)
    a = np.clip(0.35 + 0.65 * streak, 0, 1) * (0.8 + 0.2 * grain)
    rgb = lerp_color((150, 200, 240), (255, 255, 255), np.clip(streak * 1.2, 0, 1))
    return to_image(rgb, a * 255)


def tex_sunflower(n=64):
    """A sunflower head: a ring of yellow petals round a brown centre."""
    yy, xx = np.mgrid[0:n, 0:n]
    x = (xx - (n - 1) / 2) / (n / 2)
    y = (yy - (n - 1) / 2) / (n / 2)
    r = np.hypot(x, y)
    ang = np.arctan2(y, x)
    petal = 0.62 + 0.3 * np.abs(np.cos(ang * 8))
    a = (r < petal).astype(float)
    rgb = np.where((r < 0.36)[..., None], np.array([110, 70, 30.0]), np.array([255, 214, 40.0]))
    rgb = rgb * (1 - 0.25 * np.clip((r - 0.5) / 0.4, 0, 1))[..., None]
    seeds = ((xx * 7 + yy * 3) % 5 == 0) & (r < 0.34)
    rgb[seeds] = (80, 48, 20)
    return to_image(rgb, a * 255)


def tex_totem(n=64):
    """Totem pole carving, no faces: four 32-pixel cells (one per block,
    chosen by UV), each natural wood with a carved rim top and bottom and a
    painted band: red and black chevrons, a teal diamond lattice, stacked
    stripes, and stepped triangles."""
    c = n // 2
    yy, xx = np.mgrid[0:c, 0:c]
    u = (xx + 0.5) / c
    v = (yy + 0.5) / c
    wood = np.array([186, 128, 74.0]) * (0.9 + 0.1 * np.sin(u * 2 * math.pi * 5))[..., None]
    red, black, teal, cream = (np.array(x, dtype=float) for x in ((196, 52, 36), (36, 28, 24), (40, 150, 150), (238, 220, 180)))

    def rimmed(rgb):
        rim = (v < 0.12) | (v > 0.88)
        rgb[rim] = rgb[rim] * 0.62
        groove = (np.abs(v - 0.12) < 0.03) | (np.abs(v - 0.88) < 0.03)
        rgb[groove] = black
        return rgb

    cells = []
    # Chevrons pointing up, red edged in black.
    rgb = wood.copy()
    zig = v - (0.5 - 0.22 * np.abs((u * 2 % 1.0) - 0.5) * 2)
    rgb[(zig > -0.1) & (zig < 0.1)] = red
    rgb[((zig > -0.14) & (zig <= -0.1)) | ((zig >= 0.1) & (zig < 0.14))] = black
    cells.append(rimmed(rgb))
    # A lattice of teal diamonds outlined black.
    rgb = wood.copy()
    dm = np.abs((u * 3 % 1.0) - 0.5) * 1.15 + np.abs(v - 0.5) * 1.3
    rgb[dm < 0.3] = teal
    rgb[(dm >= 0.3) & (dm < 0.36)] = black
    rgb[dm < 0.1] = cream
    cells.append(rimmed(rgb))
    # Stacked stripes, red, cream and black, with notches.
    rgb = wood.copy()
    for y0, y1, col in ((0.24, 0.36, red), (0.42, 0.5, cream), (0.56, 0.64, black), (0.7, 0.78, red)):
        rgb[(v >= y0) & (v < y1)] = col
    notch = (np.abs((u * 4 % 1.0) - 0.5) < 0.08) & (v > 0.42) & (v < 0.5)
    rgb[notch] = black
    cells.append(rimmed(rgb))
    # Stepped triangles (a sawtooth), teal over red.
    rgb = wood.copy()
    saw = (u * 3 % 1.0)
    step = np.floor(saw * 4) / 4
    rgb[(v > 0.2) & (v < 0.2 + step * 0.32)] = teal
    rgb[(v > 0.8 - step * 0.32) & (v < 0.8)] = red
    rgb[(np.abs(v - 0.5) < 0.03)] = black
    cells.append(rimmed(rgb))
    out = np.zeros((n, n, 3))
    for k, cell in enumerate(cells):
        out[(k // 2) * c:(k // 2 + 1) * c, (k % 2) * c:(k % 2 + 1) * c] = cell
    out *= (0.95 + 0.08 * fbm(n, n, 8, 2, 7101))[..., None]
    return to_image(out)


def tex_bush(n=64):
    """A hill-zone bush: rounded clusters of leaves, bright on their tops
    and deep green between, tileable."""
    rng = np.random.default_rng(7301)
    yy, xx = np.mgrid[0:n, 0:n]
    light = np.zeros((n, n))
    for _ in range(46):
        cx, cy, r = rng.random() * n, rng.random() * n, 4 + rng.random() * 5
        dx = (xx - cx + n / 2) % n - n / 2
        dy = (yy - cy + n / 2) % n - n / 2
        d = np.hypot(dx, dy) / r
        blob = np.clip(1 - d, 0, 1)
        light = np.maximum(light, blob * (0.6 + 0.4 * np.clip(-dy / r, 0, 1)))
    rgb = lerp_color((34, 112, 40), (120, 214, 80), light)
    rgb *= (0.92 + 0.12 * fbm(n, n, 8, 2, 7303))[..., None]
    return to_image(rgb)


# ---------------------------------------------------------------------------
# Tsudoken Drift (a flat club circuit under Mt Fuji at golden hour: cherry
# blossom, paddies, tyre walls, stone and tile). Painted in daylight
# colours; the game grades them warm.

def tex_tkroad(n=256):
    """Dark fresh asphalt with a pinkish volcanic aggregate, two faint
    rubbered lines and crisp white edge lines."""
    rng = np.random.default_rng(7001)
    noise = fbm(n, n, 10, 5, 7003)
    fine = rng.random((n, n))
    base = 84 + 22 * (noise - 0.5) + 14 * (fine - 0.5)
    rgb = np.dstack([base * 1.06, base * 0.98, base * 1.0])
    speck = rng.random((n, n)) > 0.982
    rgb[speck] = rgb[speck] * 1.5 + np.array([24, 8, 4])
    dark = rng.random((n, n)) > 0.99
    rgb[dark] = rgb[dark] * 0.55
    u = (np.arange(n) + 0.5) / n
    groove = np.exp(-((u - 0.36) / 0.05) ** 2) + np.exp(-((u - 0.64) / 0.05) ** 2)
    rgb *= (1 - 0.10 * groove)[None, :, None]
    edge = ((u > 0.012) & (u < 0.04)) | ((u > 0.96) & (u < 0.988))
    rgb[:, edge] = np.array([240, 238, 232]) * (0.92 + 0.08 * fine[:, edge, None])
    return to_image(rgb)


def tex_tkcurb(n=64):
    """Red and white curb blocks, each block rounded off with a shaded
    lip, so the curbs read as cast concrete rather than paint."""
    v = (np.arange(n) + 0.5) / n
    u = (np.arange(n) + 0.5) / n
    red = (np.floor(v * 2) % 2 == 0)
    rgb = np.repeat(np.where(red[:, None, None], np.array([214, 44, 36.0]), np.array([240, 236, 228.0])), n, axis=1)
    lip = np.clip(1 - np.abs((v * 2 % 1.0) - 0.5) * 2.6, 0, 1)[:, None]
    rgb = rgb * (0.72 + 0.28 * lip)[..., None]
    slope = (0.9 + 0.2 * u)[None, :, None]
    rgb = rgb * slope
    rgb *= (0.94 + 0.1 * fbm(n, n, 6, 2, 7011))[..., None]
    return to_image(rgb)


def tex_verge(n=128):
    """Mown grass verges: yellow-green with mower stripes."""
    g = fbm(n, n, 8, 4, 7021)
    stripe = 0.5 + 0.5 * np.sin((np.arange(n) + 0.5) / n * 2 * math.pi * 4)[None, :]
    rgb = lerp_color((112, 160, 62), (170, 208, 92), g)
    rgb *= (0.9 + 0.14 * stripe)[..., None]
    return to_image(rgb)


def tex_paddy(n=128):
    """Rice paddies from above, one tile 24 m across: a 3 by 3 patchwork of
    greens with earth bunds between and the odd glint of water."""
    rng = np.random.default_rng(7031)
    yy, xx = np.mgrid[0:n, 0:n]
    u, v = (xx + 0.5) / n, (yy + 0.5) / n
    cells = 3
    ci, cj = np.floor(u * cells).astype(int), np.floor(v * cells).astype(int)
    greens = np.array([[120, 176, 70], [96, 160, 80], [150, 190, 72], [108, 170, 60], [132, 184, 90],
                       [88, 152, 64], [140, 180, 60], [116, 168, 76], [150, 196, 96]], dtype=float)
    rgb = greens[(ci + cj * cells) % 9]
    rows = 0.5 + 0.5 * np.sin(v * cells * 2 * math.pi * 7)
    rgb *= (0.9 + 0.12 * rows)[..., None]
    rgb *= (0.92 + 0.14 * fbm(n, n, 10, 3, 7033))[..., None]
    fu, fv = u * cells % 1.0, v * cells % 1.0
    bund = (np.minimum(fu, 1 - fu) < 0.035) | (np.minimum(fv, 1 - fv) < 0.035)
    rgb[bund] = np.array([150, 120, 80.0]) * (0.9 + 0.2 * fbm(n, n, 8, 2, 7035))[bund][:, None]
    glint = (rng.random((n, n)) > 0.994) & ~bund
    rgb[glint] = np.array([190, 220, 240.0])
    return to_image(rgb)


def tex_hoarding(w=128, h=128):
    """The trackside barrier over 8 m: a galvanised Armco beam along the
    foot on dark posts every 4 m, and a band of advertising boards above
    it, indigo with gold rules, one board TSUDOKEN, the next the sun and
    blossom emblem. Horizontal lines only, so it stays clean at range."""
    s = 4
    img = Image.new("RGB", (w * s, h * s), (190, 196, 204))
    d = ImageDraw.Draw(img)
    board_h = int(h * 0.46) * s
    # The boards.
    d.rectangle([0, 0, w * s, board_h], fill=(34, 28, 86))
    for x0, x1 in ((2 * s, 62 * s), (66 * s, 126 * s)):
        d.rectangle([x0, 2 * s, x1, board_h - 2 * s], outline=(255, 212, 70), width=s)
    font = ImageFont.truetype(font_path("DejaVuSans-Bold.ttf"), 18 * s)
    bbox = d.textbbox((0, 0), "TSUDOKEN", font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    layer = Image.new("RGB", (tw + 2 * s, th + 2 * s), (34, 28, 86))
    ImageDraw.Draw(layer).text((s - bbox[0], s - bbox[1]), "TSUDOKEN", font=font, fill=(255, 212, 70))
    layer = layer.resize((56 * s, int(board_h * 0.62)), Image.Resampling.LANCZOS)
    img.paste(layer, (4 * s, (board_h - layer.height) // 2))
    # The emblem board: a red sun disc with five blossoms across it.
    cx, cy, r = 96 * s, board_h // 2, int(board_h * 0.34)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(214, 44, 48))
    for k in range(5):
        x = 72 * s + k * 12 * s
        y = cy + (int(board_h * 0.18) if k % 2 else -int(board_h * 0.18))
        for q in range(5):
            a = q / 5 * 2 * math.pi
            px, py = x + math.cos(a) * 2.2 * s, y + math.sin(a) * 2.2 * s
            d.ellipse([px - 1.8 * s, py - 1.8 * s, px + 1.8 * s, py + 1.8 * s], fill=(255, 200, 220))
        d.ellipse([x - 0.9 * s, y - 0.9 * s, x + 0.9 * s, y + 0.9 * s], fill=(255, 120, 150))
    # The beam: two ridges of a W-section, highlights above, shadow below.
    beam_top = board_h + 2 * s
    for yy in range(beam_top, h * s):
        t = (yy - beam_top) / max(1, h * s - beam_top)
        ridge = 0.5 + 0.5 * math.cos(t * 2 * math.pi * 2 + math.pi)
        tone = int(150 + 70 * ridge)
        d.line([(0, yy), (w * s, yy)], fill=(tone, tone + 4, tone + 10))
    d.line([(0, beam_top), (w * s, beam_top)], fill=(236, 240, 244), width=s)
    d.line([(0, h * s - s), (w * s, h * s - s)], fill=(90, 92, 98), width=s)
    # Posts every 4 m, at the boards' joins, behind the boards and the beam.
    for k in range(2):
        x = k * 64 * s
        d.rectangle([x - 2 * s, 0, x + 2 * s, h * s], fill=(58, 60, 68))
        d.rectangle([x - s, 0, x, h * s], fill=(96, 98, 106))
    out = img.resize((w, h), Image.Resampling.LANCZOS)
    arr = np.asarray(out, dtype=float) * (0.96 + 0.06 * fbm(w, h, 8, 2, 7043))[..., None]
    return to_image(arr)


def tex_ishigaki(n=128):
    """A castle wall of fitted dark stone blocks, mortar-less, mossy at the
    joints: the ramp skirts, plinths and the grandstand's base."""
    rng = np.random.default_rng(7051)
    yy, xx = np.mgrid[0:n, 0:n]
    rows = 5
    rh = n / rows
    rgb = np.zeros((n, n, 3))
    joint = np.zeros((n, n))
    for r in range(rows):
        off = rng.random() * n
        widths = []
        x = 0
        while x < n * 1.5:
            w = rng.uniform(0.9, 1.6) * rh * 1.6
            widths.append((x, w))
            x += w
        for x0, w in widths:
            xa = (x0 + off) % n
            dx = (xx - xa) % n
            inx = dx < w
            iny = (yy >= r * rh) & (yy < (r + 1) * rh)
            block = inx & iny
            tone = rng.uniform(0.8, 1.15)
            cx = (dx - w / 2) / (w / 2)
            cy = (yy - (r + 0.5) * rh) / (rh / 2)
            dome = np.clip(1 - 0.28 * (cx ** 2 + cy ** 2), 0, 1)
            col = np.array([118, 120, 118.0]) * tone
            rgb[block] = (col * dome[..., None])[block]
            edge = np.maximum(np.abs(cx), np.abs(cy)) > 0.82
            joint[block & edge] = 1
    rgb *= (0.9 + 0.2 * fbm(n, n, 10, 3, 7053))[..., None]
    moss = (joint > 0) & (fbm(n, n, 6, 2, 7055) > 0.55)
    rgb[joint > 0] = rgb[joint > 0] * 0.55
    rgb[moss] = np.array([80, 110, 60.0])
    return to_image(rgb)


def tex_kawara(n=64):
    """Kawara roof tiles: rows of grey-blue pan tiles with a round cap
    along each row's foot, lit from above."""
    yy, xx = np.mgrid[0:n, 0:n]
    rows, per = 4, 4
    v = (yy + 0.5) / n * rows % 1.0
    u = (xx + 0.5) / n * per % 1.0
    rgb = np.zeros((n, n, 3)) + np.array([92, 100, 118.0])
    curve = 0.5 + 0.5 * np.cos(u * 2 * math.pi)
    rgb *= (0.78 + 0.3 * curve)[..., None]
    rgb *= (0.7 + 0.3 * np.clip(v * 3, 0, 1))[..., None]
    cap = v > 0.82
    rgb[cap] = (np.array([70, 76, 92.0]) * (0.7 + 0.5 * curve)[..., None])[cap]
    rgb[(v > 0.78) & (v <= 0.82)] = np.array([160, 168, 190.0])
    rgb *= (0.94 + 0.1 * fbm(n, n, 8, 2, 7061))[..., None]
    return to_image(rgb)


def tex_sakura(n=128):
    """A cloud of cherry blossom for the canopy planes: clusters of small
    five-petal flowers, pale pink to white, over open sky (alpha)."""
    rng = np.random.default_rng(7071)
    s = 2
    img = Image.new("RGBA", (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy, R = n * s / 2, n * s / 2, n * s * 0.46
    for _ in range(360):
        a, rr = rng.random() * 2 * math.pi, math.sqrt(rng.random()) * R
        x, y = cx + math.cos(a) * rr, cy + math.sin(a) * rr * 0.9
        if rng.random() > 0.3 + 0.7 * (1 - rr / R):
            continue
        tone = rng.random()
        col = (int(255 - 20 * tone), int(200 - 60 * tone), int(215 - 40 * tone), 255)
        pr = 2.6 * s + rng.random() * 1.8 * s
        rot = rng.random() * 2 * math.pi
        for k in range(5):
            pa = rot + k / 5 * 2 * math.pi
            px, py = x + math.cos(pa) * pr * 0.7, y + math.sin(pa) * pr * 0.7
            d.ellipse([px - pr * 0.55, py - pr * 0.55, px + pr * 0.55, py + pr * 0.55], fill=col)
        d.ellipse([x - pr * 0.3, y - pr * 0.3, x + pr * 0.3, y + pr * 0.3], fill=(255, 120, 150, 255))
    for _ in range(40):   # a few dark twigs showing through
        a, rr = rng.random() * 2 * math.pi, math.sqrt(rng.random()) * R * 0.8
        x, y = cx + math.cos(a) * rr, cy + math.sin(a) * rr
        d.line([(x, y), (x + rng.uniform(-8, 8) * s, y + rng.uniform(-8, 8) * s)], fill=(70, 50, 50, 255), width=s)
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_sakurafar(n=128):
    """A whole cherry tree for the far sprites: a dark trunk under a round
    pink crown, lighter on top."""
    rng = np.random.default_rng(7081)
    s = 2
    img = Image.new("RGBA", (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = n * s
    d.polygon([(w * 0.46, w), (w * 0.54, w), (w * 0.52, w * 0.55), (w * 0.48, w * 0.55)], fill=(60, 44, 44, 255))
    for _ in range(70):
        a, rr = rng.random() * 2 * math.pi, math.sqrt(rng.random()) * w * 0.36
        x, y = w / 2 + math.cos(a) * rr, w * 0.4 + math.sin(a) * rr * 0.8
        r = w * (0.05 + 0.06 * rng.random())
        light = 0.5 + 0.5 * (1 - (y / w) / 0.8)
        col = (int(230 + 25 * light), int(160 + 60 * light), int(185 + 40 * light), 255)
        d.ellipse([x - r, y - r, x + r, y + r], fill=col)
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_sakurabark(n=64):
    """Cherry bark: dark grey-brown with the horizontal lenticel bands."""
    g = fbm(n, n, 6, 3, 7091)
    rgb = lerp_color((58, 48, 46), (96, 80, 72), g)
    v = (np.arange(n) + 0.5) / n
    bands = np.clip(1 - np.abs(((v * 7) % 1.0) - 0.5) * 9, 0, 1)[:, None]
    rgb = rgb * (1 - 0.35 * bands)[..., None] + bands[..., None] * np.array([150, 120, 96.0]) * 0.5
    return to_image(rgb)


def tex_petal(n=32):
    """One cherry petal: a pink teardrop with a notched tip."""
    yy, xx = np.mgrid[0:n, 0:n]
    u, v = (xx + 0.5) / n - 0.5, (yy + 0.5) / n - 0.5
    body = (u ** 2 / 0.09 + v ** 2 / 0.18) < 1
    notch = (v < -0.28) & (np.abs(u) < 0.07 * (-(v + 0.28) / 0.14 + 0.2))
    a = np.where(body & ~notch, 255, 0)
    rgb = lerp_color((255, 170, 200), (255, 235, 240), np.clip((v + 0.5), 0, 1))
    return to_image(rgb, a)


def tex_tkcrowd(n=64):
    """A festival crowd: happi coats and yukata in indigo, red and white,
    some holding paper fans up."""
    rng = np.random.default_rng(7101)
    rgb = np.zeros((n, n, 3)) + np.array([64, 60, 72])
    img = Image.fromarray(rgb.astype(np.uint8))
    d = ImageDraw.Draw(img)
    palette = [(40, 50, 120), (210, 50, 50), (240, 240, 240), (40, 50, 120), (230, 120, 60), (90, 160, 200),
               (250, 230, 90), (60, 120, 80)]
    for row in range(8):
        for col in range(8):
            cx = col * 8 + 4 + rng.integers(-1, 2)
            cy = row * 8 + 3
            c = palette[rng.integers(0, len(palette))]
            d.ellipse([cx - 3, cy + 1, cx + 3, cy + 6], fill=c)
            skin = tuple(int(x) for x in rng.choice([[250, 215, 180], [220, 170, 130], [150, 105, 75]]))
            d.ellipse([cx - 2, cy - 3, cx + 2, cy + 1], fill=skin)
            if rng.random() < 0.3:
                d.ellipse([cx + 2, cy - 5, cx + 5, cy - 2], fill=(250, 250, 240))
                d.line([(cx + 3, cy - 2), (cx + 3, cy + 1)], fill=(80, 60, 50))
    return img


def tex_plaster(n=64):
    """White lime plaster between dark timber uprights, machiya style."""
    g = fbm(n, n, 6, 3, 7111)
    rgb = lerp_color((226, 220, 206), (244, 240, 230), g)
    u = (np.arange(n) + 0.5) / n
    post = ((u * 2) % 1.0) < 0.1
    rgb[:, post] = np.array([62, 42, 34.0]) * (0.9 + 0.2 * g[:, post, None])
    return to_image(rgb)


def tex_tkbanner():
    """The course banner: TSUDOKEN DRIFT in gold over deep indigo, a red
    sun disc behind the lettering, and cherry blossoms along a lower band
    of waves."""
    img = Image.new("RGB", (256, 128), (34, 28, 86))
    d = ImageDraw.Draw(img)
    for y in range(128):
        t = (y % 64) / 63
        d.line([(0, y), (256, y)], fill=(int(34 + 20 * t), int(28 + 14 * t), int(86 + 40 * t)))
    d.ellipse([98, 2, 158, 62], fill=(214, 44, 48))
    font = ImageFont.truetype(font_path("DejaVuSans-Bold.ttf"), 25)
    stops = [(0.0, (255, 250, 220)), (0.5, (255, 212, 70)), (1.0, (230, 130, 30))]
    top = gradient_text_layer((256, 64), "TSUDOKEN DRIFT", font, stops, (30, 20, 60), 4, shadow=False)
    out = img.convert("RGBA")
    out.alpha_composite(top, (0, 0))
    d = ImageDraw.Draw(out)
    # The lower band: waves and blossom.
    for k in range(9):
        x = k * 32 - 8
        d.arc([x, 86, x + 40, 126], 200, 340, fill=(120, 150, 230, 255), width=3)
        d.arc([x + 16, 92, x + 56, 132], 200, 340, fill=(90, 120, 210, 255), width=3)
    rng = np.random.default_rng(7121)
    for k in range(10):
        x, y = 12 + k * 25 + int(rng.integers(-4, 5)), 72 + int(rng.integers(0, 10))
        for p in range(5):
            a = p / 5 * 2 * math.pi
            px, py = x + math.cos(a) * 4, y + math.sin(a) * 4
            d.ellipse([px - 3, py - 3, px + 3, py + 3], fill=(255, 200, 220, 255))
        d.ellipse([x - 1.5, y - 1.5, x + 1.5, y + 1.5], fill=(255, 120, 150, 255))
    d.rectangle([0, 0, 255, 3], fill=(255, 212, 70, 255))
    d.rectangle([0, 61, 255, 66], fill=(255, 212, 70, 255))
    d.rectangle([0, 124, 255, 127], fill=(255, 212, 70, 255))
    return out.convert("RGB")


def tex_pitwall(n=128):
    """The pit garages' front over 24 m: four grey roller doors with red
    stripes under a numbered sign band, white walls between."""
    s = 2
    img = Image.new("RGB", (n * s, n * s), (236, 232, 222))
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(font_path("DejaVuSans-Bold.ttf"), 18 * s)
    d.rectangle([0, 0, n * s, 22 * s], fill=(40, 44, 70))
    for k in range(4):
        x0 = (k * 32 + 4) * s
        x1 = x0 + 24 * s
        d.rectangle([x0, 28 * s, x1, n * s], fill=(150, 152, 160))
        for y in range(30 * s, n * s, 6 * s):
            d.line([(x0, y), (x1, y)], fill=(118, 120, 128), width=s)
        d.rectangle([x0, 60 * s, x1, 66 * s], fill=(200, 48, 40))
        d.text((x0 + 8 * s, 2 * s), str(k + 1), font=font, fill=(250, 214, 70))
        d.rectangle([x0 - 2 * s, 24 * s, x1 + 2 * s, 28 * s], fill=(70, 50, 40))
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_sundisc(n=64):
    """A soft-edged sun for the golden hour: white heart, gold rim."""
    yy, xx = np.mgrid[0:n, 0:n]
    r = np.hypot(xx + 0.5 - n / 2, yy + 0.5 - n / 2) / (n / 2)
    a = np.clip((0.92 - r) / 0.12, 0, 1) * 255
    rgb = lerp_color((255, 252, 230), (255, 190, 90), np.clip(r / 0.9, 0, 1) ** 2)
    return to_image(rgb, a)


def tex_stream(n=128):
    """Clear stream water: blue-green with ripples and sun flecks, and a
    koi swimming across one corner."""
    rng = np.random.default_rng(7131)
    g = fbm(n, n, 5, 4, 7133)
    ripple = 0.5 + 0.5 * np.sin(((np.arange(n) + 0.5) / n * 2 * math.pi * 6)[None, :] + g * 6)
    rgb = lerp_color((40, 110, 130), (90, 170, 170), g) + ripple[..., None] * np.array([30, 40, 40.0])
    img = to_image(rgb)
    d = ImageDraw.Draw(img)
    for _ in range(60):
        x, y = int(rng.integers(0, n)), int(rng.integers(0, n))
        d.line([(x, y), (x + 3, y)], fill=(210, 240, 240))
    d.ellipse([20, 86, 44, 98], fill=(240, 120, 40))
    d.polygon([(44, 92), (52, 86), (52, 98)], fill=(240, 120, 40))
    d.ellipse([30, 88, 36, 94], fill=(250, 250, 240))
    return img


# ---------------------------------------------------------------------------
# Apple Valley: a kart club circuit in the Mojave high desert

def tex_avroad(n=128):
    """Sun-bleached desert asphalt: grey with a warm cast, pale aggregate,
    black tar snakes where the cracks were sealed, rubber streaked along the
    lines karts take and a drift of tan dust toward both edges."""
    rng = np.random.default_rng(8001)
    noise = fbm(n, n, 8, 5, 8003)
    fine = rng.random((n, n))
    base = 116 + 24 * (noise - 0.5) + 16 * (fine - 0.5)
    rgb = np.dstack([base * 1.03, base * 1.0, base * 0.97])
    speck = rng.random((n, n)) > 0.978
    rgb[speck] = rgb[speck] * 1.32 + np.array([10, 8, 4])
    pit = rng.random((n, n)) > 0.988
    rgb[pit] = rgb[pit] * 0.62
    # Tar snakes: the contours of tileable noise fields, broken up by a
    # second field so they read as separate sealed cracks, not loops.
    for seed, period, w in ((8005, 4, 0.006), (8007, 6, 0.005)):
        f = periodic_noise(n, n, period, seed) + 0.12 * periodic_noise(n, n, period * 4, seed + 1)
        keep = periodic_noise(n, n, 3, seed + 2) > 0.48
        snake = (np.abs(f - 0.55) < w) & keep
        rgb[snake] = np.array([40, 40, 44.0]) * (0.9 + 0.2 * fine[snake, None])
    u = (np.arange(n) + 0.5) / n
    # Rubber: streaks along the road on the usual lines, broken up.
    streak = np.zeros(n)
    for c, wd in ((0.30, 0.05), (0.44, 0.03), (0.62, 0.05), (0.76, 0.03)):
        streak += np.exp(-((u - c) / wd) ** 2)
    along = 0.55 + 0.45 * periodic_noise(n, n, 4, 8009)
    rgb *= (1 - 0.16 * streak[None, :] * along)[..., None]
    # Wind-blown dust gathers at the edges.
    dust = np.clip(1 - np.minimum(u, 1 - u) / 0.12, 0, 1) ** 2
    dust2 = dust[None, :] * (0.6 + 0.4 * fbm(n, n, 6, 2, 8011))
    rgb = rgb * (1 - 0.45 * dust2[..., None]) + np.array([196, 170, 130.0]) * 0.45 * dust2[..., None]
    return to_image(rgb)


def tex_avcurb(n=64):
    """The track's concrete edge: pale cast blocks 1.2 m long with dark
    joints, scuffed with rubber, a rounded lip on the outside."""
    rng = np.random.default_rng(8021)
    v = (np.arange(n) + 0.5) / n
    u = (np.arange(n) + 0.5) / n
    g = fbm(n, n, 6, 3, 8023)
    rgb = lerp_color((176, 172, 162), (214, 210, 198), g)
    blocks = 2
    jv = (v * blocks) % 1.0
    joint = (jv < 0.035) | (jv > 0.965)
    rgb[joint, :] = np.array([92, 90, 88.0])
    # Alternate blocks a shade apart, poured on different days.
    alt = (np.floor(v * blocks) % 2 == 1)
    rgb[alt & ~joint] *= 0.93
    lip = np.clip((u - 0.78) / 0.22, 0, 1)
    rgb *= (1 - 0.28 * lip ** 2)[None, :, None]
    scuff = (rng.random((n, n)) > 0.93) & (g > 0.55)
    rgb[scuff] *= 0.75
    return to_image(rgb)


def tex_avdirt(n=64):
    """Run-off: packed decomposed granite, tan with pale grit, darker
    pebbles and the odd kart track pressed into it."""
    rng = np.random.default_rng(8031)
    g = fbm(n, n, 8, 4, 8033)
    rgb = lerp_color((176, 150, 112), (214, 190, 148), g)
    grit = rng.random((n, n))
    rgb *= (0.9 + 0.18 * grit)[..., None]
    peb = rng.random((n, n)) > 0.985
    rgb[peb] = rgb[peb] * 0.62
    pale = rng.random((n, n)) > 0.99
    rgb[pale] = np.array([236, 226, 204.0])
    track = 0.5 + 0.5 * np.sin(((np.arange(n) + 0.5) / n * 2 * math.pi * 3)[None, :] + 3 * fbm(n, n, 3, 2, 8035))
    rgb *= (1 - 0.06 * (track > 0.94))[..., None]
    return to_image(rgb)


def tex_avdesert(n=128):
    """The Mojave floor from above, one tile 24 m across: pale sand, gravel
    patches, wash streaks and dark olive creosote bushes dotted about with
    their shadows."""
    rng = np.random.default_rng(8041)
    g = fbm(n, n, 6, 5, 8043)
    rgb = lerp_color((190, 164, 124), (226, 204, 164), g)
    gravel = fbm(n, n, 4, 3, 8045) > 0.6
    rgb[gravel] = rgb[gravel] * 0.86 + np.array([6, 6, 10.0])
    wash = np.abs(periodic_noise(n, n, 2, 8047) - 0.5) < 0.05
    rgb[wash] = rgb[wash] * 1.06
    rgb *= (0.93 + 0.12 * rng.random((n, n)))[..., None]
    img = to_image(rgb)
    d = ImageDraw.Draw(img)
    for _ in range(26):
        x, y = rng.uniform(0, n), rng.uniform(0, n)
        r = rng.uniform(1.4, 3.2)
        for ox in (-n, 0, n):
            for oy in (-n, 0, n):
                cx, cy = x + ox, y + oy
                d.ellipse([cx - r + 1.2, cy - r * 0.7 + 1.6, cx + r + 1.2, cy + r * 0.7 + 1.6], fill=(150, 128, 98))
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(92, 104, 58))
                d.ellipse([cx - r * 0.5, cy - r * 0.6, cx + r * 0.2, cy], fill=(124, 136, 76))
    return img


def tex_avtyres(w=128, h=128):
    """The tyre wall over 8 m, 1.1 m high: stacks of tyres laid flat and
    bolted together, so each tyre shows as a band with rounded ends, worn
    black; every fourth stack is painted white so the line of the wall
    reads at speed."""
    s = 4
    W, H = w * s, h * s
    img = Image.new("RGB", (W, H), (18, 18, 20))
    d = ImageDraw.Draw(img)
    cols, rows = 12, 5
    cw, rh = W / cols, H / rows
    for c in range(cols):
        painted = c % 4 == 1
        base = (222, 222, 216) if painted else (46, 46, 50)
        for r in range(rows):
            x0, y0 = c * cw + s, r * rh + s
            x1, y1 = (c + 1) * cw - s, (r + 1) * rh - s
            # The tread face: rounded at both ends, lit across the middle.
            for k in range(int(y1 - y0)):
                t = k / max(1, y1 - y0 - 1)
                bulge = math.sin(t * math.pi)
                tone = tuple(int(min(255, v * (0.45 + 0.7 * bulge))) for v in base)
                inset = (1 - bulge) * cw * 0.12
                d.line([(x0 + inset, y0 + k), (x1 - inset, y0 + k)], fill=tone)
            # A worn highlight along the tread.
            hl = tuple(int(min(255, v * 1.25 + 14)) for v in base)
            d.line([(x0 + cw * 0.2, y0 + rh * 0.38), (x1 - cw * 0.2, y0 + rh * 0.38)], fill=hl, width=s)
    out = img.resize((w, h), Image.Resampling.LANCZOS)
    arr = np.asarray(out, dtype=float) * (0.94 + 0.1 * fbm(w, h, 8, 2, 8051))[..., None]
    # Desert dust settled on the lower tyres.
    vv = (np.arange(h) + 0.5) / h
    dust = np.clip((vv - 0.6) / 0.4, 0, 1)[:, None, None]
    arr = arr * (1 - 0.35 * dust) + np.array([186, 160, 120.0]) * 0.35 * dust
    return to_image(arr)


def tex_avmetal(n=64):
    """Corrugated galvanised sheet, vertical ridges, a little streaked; the
    props tint it (container red, pale roofs)."""
    u = (np.arange(n) + 0.5) / n
    ridge = 0.5 + 0.5 * np.cos(u * 2 * math.pi * 8)
    g = fbm(n, n, 4, 3, 8061)
    base = 196 + 40 * (ridge[None, :] - 0.5) + 16 * (g - 0.5)
    streak = periodic_noise(n, n, 16, 8063)[:, :] * 0.5 + 0.5
    base *= 0.94 + 0.08 * streak
    rgb = np.dstack([base, base * 1.0, base * 1.02])
    return to_image(rgb)


def tex_avcrowd(n=64):
    """Race-day bleachers: families in t-shirts, ball caps and sun hats."""
    rng = np.random.default_rng(8071)
    img = Image.new("RGB", (n, n), (150, 152, 156))
    d = ImageDraw.Draw(img)
    palette = [(230, 60, 50), (250, 210, 70), (70, 140, 220), (245, 245, 240), (40, 44, 52), (100, 190, 110),
               (240, 140, 60), (150, 90, 200)]
    for row in range(8):
        d.line([(0, row * 8 + 7), (n, row * 8 + 7)], fill=(196, 198, 204))
        for col in range(8):
            if rng.random() < 0.12:
                continue
            cx = col * 8 + 4 + int(rng.integers(-1, 2))
            cy = row * 8 + 3
            c = palette[int(rng.integers(0, len(palette)))]
            d.ellipse([cx - 3, cy + 1, cx + 3, cy + 7], fill=c)
            skin = tuple(int(x) for x in rng.choice([[246, 206, 170], [210, 160, 120], [150, 104, 72], [96, 66, 46]]))
            d.ellipse([cx - 2, cy - 3, cx + 2, cy + 1], fill=skin)
            hat = rng.random()
            if hat < 0.35:
                hc = palette[int(rng.integers(0, len(palette)))]
                d.rectangle([cx - 2, cy - 4, cx + 2, cy - 2], fill=hc)
                d.line([(cx, cy - 2), (cx + 3, cy - 2)], fill=hc)
            elif hat < 0.5:
                d.ellipse([cx - 4, cy - 4, cx + 4, cy - 1], fill=(226, 206, 160))
    return img


def tex_avjoshua(n=128):
    """A Joshua tree's leaf cluster for the cut-out planes: a burst of stiff
    dagger leaves from a dark core, olive to yellow-green at the tips, the
    old leaves hanging grey below (alpha)."""
    rng = np.random.default_rng(8081)
    s = 2
    img = Image.new("RGBA", (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = n * s / 2, n * s * 0.55
    # The dead skirt.
    for _ in range(60):
        a = rng.uniform(0.25 * math.pi, 0.75 * math.pi)
        L = rng.uniform(0.18, 0.34) * n * s
        x1, y1 = cx + math.cos(a) * L * 0.6, cy + math.sin(a) * L
        d.line([(cx + rng.uniform(-6, 6) * s, cy), (x1, y1)], fill=(126, 112, 92, 255), width=2 * s)
    # Live leaves, longest sideways and up.
    for _ in range(150):
        a = rng.uniform(-math.pi, 0.15 * math.pi) if rng.random() < 0.85 else rng.uniform(0, math.pi)
        L = rng.uniform(0.24, 0.46) * n * s * (0.7 if math.sin(a) > 0 else 1.0)
        x1, y1 = cx + math.cos(a) * L, cy + math.sin(a) * L * 0.85
        t = rng.random()
        col = (int(92 + 60 * t), int(118 + 50 * t), int(54 + 16 * t), 255)
        w = max(1, int(rng.uniform(1.6, 3.0) * s))
        d.line([(cx, cy), (x1, y1)], fill=col, width=w)
        d.line([((cx + x1 * 3) / 4, (cy + y1 * 3) / 4), (x1, y1)], fill=(186, 196, 96, 255), width=max(1, w - s))
    d.ellipse([cx - 6 * s, cy - 6 * s, cx + 6 * s, cy + 6 * s], fill=(70, 80, 44, 255))
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_avjbark(n=64):
    """A Joshua tree's trunk: shaggy with dead leaves pointing down, grey
    brown, the bare older wood showing through in places."""
    rng = np.random.default_rng(8091)
    s = 2
    img = Image.new("RGB", (n * s, n * s), (104, 90, 72))
    d = ImageDraw.Draw(img)
    for _ in range(700):
        x, y = rng.uniform(0, n * s), rng.uniform(0, n * s)
        L = rng.uniform(4, 10) * s
        t = rng.random()
        col = (int(90 + 70 * t), int(80 + 56 * t), int(64 + 40 * t))
        for ox in (-n * s, 0, n * s):
            for oy in (-n * s, 0, n * s):
                d.line([(x + ox, y + oy), (x + ox + rng.uniform(-1.5, 1.5) * s, y + oy + L)], fill=col, width=s)
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_avscrub(n=64):
    """A creosote bush seen from the side: a loose dome of small olive
    leaves on grey stems (alpha)."""
    rng = np.random.default_rng(8101)
    s = 2
    img = Image.new("RGBA", (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = n * s
    for _ in range(18):
        x0 = w / 2 + rng.uniform(-0.12, 0.12) * w
        a = rng.uniform(-2.4, -0.7)
        L = rng.uniform(0.3, 0.55) * w
        d.line([(x0, w), (x0 + math.cos(a) * L, w + math.sin(a) * L)], fill=(110, 100, 86, 255), width=s)
    for _ in range(260):
        a = rng.uniform(math.pi, 2 * math.pi)
        r = math.sqrt(rng.random()) * 0.46 * w
        x, y = w / 2 + math.cos(a) * r, w * 0.98 + math.sin(a) * r * 1.6
        if y < w * 0.08:
            continue
        t = rng.random()
        col = (int(78 + 50 * t), int(96 + 46 * t), int(46 + 20 * t), 255)
        rr = rng.uniform(1.2, 2.4) * s
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=col)
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_avbanner():
    """The circuit's sign, after its logo: APPLE VALLEY in heavy slanted red
    over SPEEDWAY in wide black capitals, on white between red rules.
    Drawn at four times the size and filtered down, so the strokes stay
    solid at 256 x 64."""
    s = 4
    W, H = 256 * s, 64 * s
    bg, red, ink = (250, 250, 246), (210, 30, 24), (18, 18, 20)
    img = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(img)

    def fit(text, size, box_w, box_h, fill, stroke):
        font = ImageFont.truetype(font_path("DejaVuSans-Bold.ttf"), size)
        bb = d.textbbox((0, 0), text, font=font, stroke_width=stroke)
        layer = Image.new("RGB", (bb[2] - bb[0], bb[3] - bb[1]), bg)
        ImageDraw.Draw(layer).text((-bb[0], -bb[1]), text, font=font, fill=fill, stroke_width=stroke, stroke_fill=fill)
        return layer.resize((box_w, box_h), Image.Resampling.LANCZOS)

    top = fit("APPLE VALLEY", 120, 236 * s, 34 * s, red, 1)
    # A forward slant, like the logo.
    top = top.transform(top.size, Image.Transform.AFFINE, (1, 0.2, -7 * s, 0, 1, 0), Image.Resampling.BICUBIC,
                        fillcolor=bg)
    img.paste(top, (10 * s, 5 * s))
    low = fit("S P E E D W A Y", 60, 150 * s, 13 * s, ink, s)
    img.paste(low, ((W - low.width) // 2, 43 * s))
    d.rectangle([0, 0, W, 2 * s], fill=red)
    d.rectangle([0, H - 3 * s, W, H], fill=red)
    return img.resize((256, 64), Image.Resampling.LANCZOS)


def tex_avstucco(n=64):
    """White stucco, a little sun-yellowed, with a faint trowel swirl."""
    g = fbm(n, n, 8, 4, 8111)
    swirl = periodic_noise(n, n, 6, 8113)
    rgb = lerp_color((222, 216, 202), (246, 242, 232), 0.6 * g + 0.4 * swirl)
    v = (np.arange(n) + 0.5) / n
    rgb *= (1 - 0.06 * np.clip((v - 0.8) / 0.2, 0, 1))[:, None, None]
    return to_image(rgb)


def tex_avrock(n=64):
    """Weathered desert granite: tan and grey grains, dark cracks, the
    rust of desert varnish."""
    rng = np.random.default_rng(8121)
    g = fbm(n, n, 4, 5, 8123, 0.55)
    rgb = lerp_color((122, 104, 88), (196, 176, 150), g)
    grain = rng.random((n, n))
    rgb *= (0.88 + 0.22 * grain)[..., None]
    f = periodic_noise(n, n, 6, 8125) + 0.15 * periodic_noise(n, n, 24, 8126)
    crack = (np.abs(f - 0.55) < 0.012) & (periodic_noise(n, n, 3, 8128) > 0.45)
    rgb[crack] *= 0.68
    varnish = fbm(n, n, 3, 2, 8127) > 0.62
    rgb[varnish] = rgb[varnish] * 0.8 + np.array([30, 12, 0.0])
    return to_image(rgb)


def tex_avfar(n=64):
    """A whole Joshua tree for the far sprites: a shaggy trunk forking
    into crooked arms, each ending in a leaf burst."""
    rng = np.random.default_rng(8131)
    s = 2
    img = Image.new("RGBA", (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = n * s
    trunk = (110, 96, 78, 255)
    d.line([(w * 0.5, w), (w * 0.49, w * 0.55)], fill=trunk, width=int(w * 0.06))
    tips = []
    for a, L in ((-2.3, 0.28), (-1.6, 0.34), (-0.9, 0.3), (-2.0, 0.2), (-1.1, 0.22)):
        x0, y0 = w * 0.49, w * 0.55
        mx, my = x0 + math.cos(a) * w * L * 0.5, y0 + math.sin(a) * w * L * 0.5 + w * 0.03
        x1, y1 = x0 + math.cos(a) * w * L, y0 + math.sin(a) * w * L
        d.line([(x0, y0), (mx, my), (x1, y1)], fill=trunk, width=int(w * 0.035), joint="curve")
        tips.append((x1, y1))
    for x, y in tips:
        for _ in range(30):
            a = rng.uniform(-math.pi, 0.3)
            L = rng.uniform(0.04, 0.08) * w
            d.line([(x, y), (x + math.cos(a) * L, y + math.sin(a) * L)], fill=(118, 140, 66, 255), width=s * 2)
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_avtumble(n=64):
    """A tumbleweed: a ball of thin, tangled, sun-bleached twigs, denser
    round the rim, see-through between (alpha)."""
    rng = np.random.default_rng(8141)
    s = 4
    W = n * s
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c, R = W / 2, W * 0.46
    for _ in range(150):
        # A twig: a short random walk that stays inside the ball.
        a, r = rng.uniform(0, 2 * math.pi), R * math.sqrt(rng.random())
        x, y = c + math.cos(a) * r, c + math.sin(a) * r
        h = rng.uniform(0, 2 * math.pi)
        pts = [(x, y)]
        for k in range(5):
            h += rng.uniform(-0.9, 0.9)
            x, y = x + math.cos(h) * W * 0.07, y + math.sin(h) * W * 0.07
            if math.hypot(x - c, y - c) > R:
                break
            pts.append((x, y))
        if len(pts) < 2:
            continue
        t = rng.random()
        col = (int(150 + 60 * t), int(122 + 50 * t), int(84 + 40 * t), 255)
        d.line(pts, fill=col, width=s, joint="curve")
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_avdust(n=32):
    """A puff of desert dust for the dust devils: a soft round cloud with a
    swirl of streaks through it (alpha)."""
    yy, xx = np.mgrid[0:n, 0:n]
    u, v = (xx + 0.5) / n - 0.5, (yy + 0.5) / n - 0.5
    r = np.hypot(u, v) * 2
    ang = np.arctan2(v, u)
    swirl = 0.5 + 0.5 * np.sin(ang * 3 + r * 9)
    a = np.clip(1 - r, 0, 1) ** 1.1 * (0.6 + 0.4 * swirl) * (0.8 + 0.2 * fbm(n, n, 6, 2, 8151))
    rgb = lerp_color((176, 150, 116), (226, 206, 170), np.clip(1 - r, 0, 1))
    return to_image(rgb, a * 255)


def tex_avmtn(n=32):
    """The brown mountain's rock, as detail over its vertex colours: pale
    and dark gullies running down the slope, faint strata across them, and
    scree speckle. Mostly light grey, so the colours underneath lead."""
    rng = np.random.default_rng(8161)
    u = (np.arange(n) + 0.5) / n
    g = fbm(n, n, 6, 4, 8163)
    gully = periodic_noise(n, n, 6, 8165)
    gully = np.abs(np.sin((u[None, :] * 7 + 0.6 * gully) * math.pi))      # vertical streaks, wandering
    strata = 0.5 + 0.5 * np.sin((np.arange(n)[:, None] / n) * 2 * math.pi * 5 + 3 * g)
    lum = 196 + 70 * (gully - 0.5) + 16 * (strata - 0.5) + 30 * (g - 0.5)
    lum = lum * (0.92 + 0.16 * rng.random((n, n)))
    rgb = np.dstack([lum * 1.04, lum, lum * 0.96])
    return to_image(rgb)


def tex_avrange(n=32):
    """The snowy range far off, as detail over its vertex colours: near
    the ridge (v = 0) bright snow with faint couloirs, lower down darker
    rock ribs of uneven width running down into the haze."""
    u = (np.arange(n) + 0.5) / n
    v = (np.arange(n) + 0.5) / n
    wob = periodic_noise(n, n, 3, 8173)
    ribs = np.abs(np.sin((u[None, :] * 5 + 0.8 * wob) * math.pi)) ** 0.5
    fine = periodic_noise(n, n, 8, 8175)
    depth = np.clip((v[:, None] - 0.12) / 0.5, 0, 1)          # how far below the ridge
    lum = 255 - depth * (95 - 75 * ribs) - 25 * (fine - 0.5) * depth
    rgb = np.dstack([lum, lum, lum])
    return to_image(rgb)


def build(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    previews = out / "previews"
    previews.mkdir(exist_ok=True)
    font_img, glyphs = tex_font()
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
        (Spec("shadow", "ARGB4444", True, False), tex_shadow()),
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
        (Spec("stone", "RGB565", True, True), tex_stone()),
        *[(Spec(f"skyroad_{i}", "RGB565", True, True), tex_skyroad(i)) for i in range(SKYROAD_PHASES)],
        (Spec("nightsky", "RGB565", False, True), tex_nightsky()),
        (Spec("skyline", "ARGB4444", False, True), tex_skyline()),
        (Spec("moon", "ARGB4444", False, False), tex_moon()),
        (Spec("cloudsea", "RGB565", True, True), tex_cloudsea()),
        (Spec("tower", "RGB565", True, True), tex_tower()),
        (Spec("signs", "RGB565", True, False), tex_signs()),
        (Spec("canyonroad", "RGB565", True, True), tex_canyonroad()),
        (Spec("meadow", "RGB565", True, True), tex_meadow()),
        (Spec("islandrock", "RGB565", True, True), tex_islandrock()),
        (Spec("ehchecker", "RGB565", True, True), tex_checker_rock((112, 62, 54), (206, 146, 92), 5113)),
        (Spec("sunflower", "ARGB4444", True, False), tex_sunflower()),
        (Spec("totem", "RGB565", False, False), tex_totem()),
        (Spec("bush", "RGB565", True, True), tex_bush()),
        (Spec("waterfall", "ARGB4444", True, True), tex_waterfall()),
        (Spec("retrosun", "ARGB4444", True, False), tex_retrosun()),
        (Spec("tkroad", "RGB565", True, True), tex_tkroad()),
        (Spec("tkcurb", "RGB565", True, True), tex_tkcurb()),
        (Spec("verge", "RGB565", True, True), tex_verge()),
        (Spec("paddy", "RGB565", True, True), tex_paddy()),
        (Spec("hoarding", "RGB565", True, True), tex_hoarding()),
        (Spec("ishigaki", "RGB565", True, True), tex_ishigaki()),
        (Spec("kawara", "RGB565", True, True), tex_kawara()),
        (Spec("sakura", "ARGB4444", True, False), tex_sakura()),
        (Spec("sakurafar", "ARGB4444", True, False), tex_sakurafar()),
        (Spec("sakurabark", "RGB565", True, True), tex_sakurabark()),
        (Spec("petal", "ARGB4444", False, False), tex_petal()),
        (Spec("tkcrowd", "RGB565", True, True), tex_tkcrowd()),
        (Spec("plaster", "RGB565", True, True), tex_plaster()),
        (Spec("tkbanner", "RGB565", False, False), tex_tkbanner()),
        (Spec("pitwall", "RGB565", True, True), tex_pitwall()),
        (Spec("sundisc", "ARGB4444", True, False), tex_sundisc()),
        (Spec("stream", "RGB565", True, True), tex_stream()),
        (Spec("avroad", "RGB565", True, True), tex_avroad()),
        (Spec("avcurb", "RGB565", True, True), tex_avcurb()),
        (Spec("avdirt", "RGB565", True, True), tex_avdirt()),
        (Spec("avdesert", "RGB565", True, True), tex_avdesert()),
        (Spec("avtyres", "RGB565", True, True), tex_avtyres()),
        (Spec("avmetal", "RGB565", True, True), tex_avmetal()),
        (Spec("avcrowd", "RGB565", True, True), tex_avcrowd()),
        (Spec("avjoshua", "ARGB4444", True, False), tex_avjoshua()),
        (Spec("avjbark", "RGB565", True, True), tex_avjbark()),
        (Spec("avscrub", "ARGB4444", True, False), tex_avscrub()),
        (Spec("avbanner", "RGB565", False, False), tex_avbanner()),
        (Spec("avstucco", "RGB565", True, True), tex_avstucco()),
        (Spec("avrock", "RGB565", True, True), tex_avrock()),
        (Spec("avfar", "ARGB4444", True, False), tex_avfar()),
        (Spec("avtumble", "ARGB4444", True, False), tex_avtumble()),
        (Spec("avdust", "ARGB4444", True, False), tex_avdust()),
        (Spec("avmtn", "RGB565", True, True), tex_avmtn()),
        (Spec("avrange", "RGB565", True, True), tex_avrange()),
    ]
    pak = bytearray()
    table = []
    for spec, img in items:
        img.save(previews / f"tex-{spec.name}.png")
        # The opaque textures' mips keep their mid-sized detail (lanczos):
        # with no anisotropic filtering, the console shows the road, curbs
        # and walls at small mip levels, and area filtering turned those flat.
        words = pack_texture(img, spec.fmt, mipmap=spec.mip, wrap=spec.wrap,
                             mip_filter="lanczos" if spec.fmt == "RGB565" else "box")
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
         f"#define FONT_ROW_H {FONT_ROW_H}", "#define FONT_TEX_W 512", "#define FONT_TEX_H 256",
         "/* ASCII 32..127 at native size: atlas x, row, rect width, rect left",
         "   relative to the pen, and pen advance. */",
         "typedef struct { uint16_t x; uint8_t row, w; int8_t ox; uint8_t adv; } dk_glyph_t;",
         "static const dk_glyph_t font_glyphs[96] = {"]
    for i in range(0, 96, 4):
        m.append("    " + " ".join(f"{{{x}, {r}, {w}, {ox}, {adv}}}," for x, r, w, ox, adv in glyphs[i:i + 4]))
    m += ["};", "",
          "/* Countdown cells (3, 2, 1, GO!) in the bigtext texture's top row: x, width. */",
          "static const uint16_t bigtext_cells[4][2] = {" +
          ", ".join(f"{{{x}, {w}}}" for x, w in BIGTEXT_CELLS) + "};", "",
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
