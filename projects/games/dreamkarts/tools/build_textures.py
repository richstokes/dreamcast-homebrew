#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "pillow>=10.1", "matplotlib>=3.8"]
# ///
"""Procedurally paint every DreamKarts texture and pack it for the PowerVR.

Everything is drawn from code (seeded noise, shapes, Lilita One from the
engine's tools/fonts/ for the menus' lettering and the DejaVu fonts that ship with
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
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
from assetpak import TexSpec, to_image, write_font_metrics, write_texture_header, write_texture_pak  # noqa: E402
from fontatlas import LILITA, atlas, font_for_cap  # noqa: E402

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
    planks = 6
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
    """Four fair-weather cumulus, one to each 128x64 quarter: a row of puffs
    on a flat base with bigger ones heaped on top, each lit like a ball from
    high up so the tops are white and the creases and the base a soft sky
    blue. The base row is squashed wide so it reads as one flat bottom.
    Puffs are opaque with feathered, noisy edges, so on the console the
    4-bit alpha only dithers along the outline."""
    cw, ch = 128, 64
    yy, xx = np.mgrid[0:ch, 0:cw].astype(float)
    light = np.array([-0.35, -0.75, 0.56])
    light /= np.linalg.norm(light)
    lit, shadow = np.array([255, 255, 255.0]), np.array([168, 186, 216.0])
    rgb = np.zeros((h, w, 3))
    alpha = np.zeros((h, w))
    for cell in range(4):
        rng = np.random.default_rng(111 + cell)
        edge = fbm(cw, ch, 16, 3, 113 + cell)
        base = rng.uniform(50, 55)
        left, right = rng.uniform(16, 24), rng.uniform(104, 112)
        puffs = []
        # Heaped tops first (behind), then the row along the base in front.
        for _ in range(rng.integers(2, 5)):
            r = rng.uniform(12, 19)
            cx = rng.uniform(left + 18, right - 18)
            puffs.append((cx, base - r * rng.uniform(0.9, 1.3), r))
        # Fill any notch between neighbouring heaps, which reads as two
        # clouds glued together: a lower puff in the valley.
        heaps = sorted(puffs)
        for (x0, y0, r0), (x1, y1, r1) in zip(heaps, heaps[1:]):
            if x1 - x0 > (r0 + r1) * 0.7:
                rb = max(r0, r1) * 0.8
                puffs.append(((x0 + r0 + x1 - r1) * 0.5, max(y0, y1) + rb * 0.25, rb))
        # Small bumps round the top of each heap, behind it, so only their
        # caps break the outline.
        bumps = []
        for cx, cy, r in puffs:
            for _ in range(5):
                a = rng.uniform(-2.8, -0.35)
                bumps.append((cx + math.cos(a) * r * 0.8, cy + math.sin(a) * r * 0.8, r * rng.uniform(0.28, 0.42), 1.0))
        puffs = bumps + [(cx, cy, r, 1.0) for cx, cy, r in puffs]
        n = rng.integers(4, 6)
        for k in range(n):
            r = rng.uniform(9, 13)
            cx = left + 6 + (right - left - 12) * (k + rng.uniform(0.3, 0.7)) / n
            puffs.append((cx, base - r * rng.uniform(0.3, 0.55), r, 1.6))
        acc = np.zeros((ch, cw, 3))
        cov_all = np.zeros((ch, cw))
        for cx, cy, r, stretch in puffs:
            dx, dy = (xx - cx) / (r * stretch), (yy - cy) / r
            d = np.hypot(dx, dy) + (edge - 0.5) * 0.3
            cov = np.clip((1.0 - d) / 0.18, 0, 1)
            cov = cov * cov * (3 - 2 * cov)
            nz = np.sqrt(np.clip(1 - dx * dx - dy * dy, 0, 1))
            lam = np.clip(dx * light[0] + dy * light[1] + nz * light[2], 0, 1)
            low = np.clip((yy - (base - 14)) / 14, 0, 1)        # shade toward the base
            t = np.clip(0.4 + 0.7 * lam - 0.35 * low, 0, 1)[..., None]
            col = shadow + (lit - shadow) * t
            acc = col * cov[..., None] + acc * (1 - cov[..., None])
            cov_all = cov + cov_all * (1 - cov)
        # A flat underside that thins out over a few texels, a little uneven.
        line = base + (edge[0] - 0.5) * 3.0
        cut = np.clip((line[None, :] + 1.0 - yy) / 5.0, 0, 1)
        cov_all *= cut * cut * (3 - 2 * cut)
        col = np.where(cov_all[..., None] > 0.002, acc / np.maximum(cov_all, 1e-3)[..., None], shadow)
        col = np.clip(col, 0, 255)
        # Clear texels round the quarter, so filtering never reaches a neighbour.
        cov_all[:2], cov_all[-2:], cov_all[:, :2], cov_all[:, -2:] = 0, 0, 0, 0
        oy, ox = (cell >> 1) * ch, (cell & 1) * cw
        rgb[oy:oy + ch, ox:ox + cw] = col
        alpha[oy:oy + ch, ox:ox + cw] = np.clip(cov_all, 0, 1)
    return to_image(rgb, alpha * 255)


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


# The menus' lettering: Lilita One (OFL, the engine's tools/fonts/) for the HUD font,
# logo and countdown. The outline is 2 px: 3 px pushed Q's tail out of the
# 42 px glyph row.
UI_FONT = LILITA
UI_OUTLINE = 2
LOGO_SIZES = (110, 94)   # capitals, lowercase
LOGO_OUTLINE = 7


def ui_font() -> ImageFont.FreeTypeFont:
    """The HUD font at the size whose capitals stand FONT_CAP_H tall (the
    DejaVu Bold Oblique the game shipped with, at 30 px), so every hud_text
    scale in the game keeps its size whatever the font."""
    return font_for_cap(UI_FONT, FONT_CAP_H)


def tex_font():
    """White glyphs with a dark outline; tint with vertex colour at runtime
    (the engine's fontatlas). Returns the atlas and, per glyph, (x, row,
    width, left offset from the pen, advance), and the capitals' rows."""
    return atlas(ui_font(), FONT_ROW_H, FONT_BASELINE, UI_OUTLINE)


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


SIGN_WORDS = [("DREAMKARTS", CYAN), ("ARCADE", MAGENTA), ("NEON 24H", np.array([150, 90, 255.0])),
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


# ---------------------------------------------------------------------------
# Bay City Hills: San Francisco's steep streets, the crooked street and the
# wharf

def sf_asphalt(w, h, seed, base=104):
    """Old city asphalt, a little blue-grey: aggregate, sealed cracks and
    darker rectangles where the street was cut open and patched."""
    rng = np.random.default_rng(seed)
    noise = fbm(w, h, 8, 5, seed + 1)
    fine = rng.random((h, w))
    lum = base + 22 * (noise - 0.5) + 14 * (fine - 0.5)
    rgb = np.dstack([lum * 0.98, lum * 1.0, lum * 1.05])
    speck = rng.random((h, w)) > 0.982
    rgb[speck] = rgb[speck] * 1.3
    pit = rng.random((h, w)) > 0.988
    rgb[pit] = rgb[pit] * 0.62
    # Patches: utility cuts, squared off, a shade darker and smoother.
    for _ in range(5):
        pw, ph = int(rng.integers(w // 10, w // 4)), int(rng.integers(h // 12, h // 4))
        x0, y0 = int(rng.integers(0, w)), int(rng.integers(0, h))
        ys = (np.arange(y0, y0 + ph) % h)[:, None]
        xs = (np.arange(x0, x0 + pw) % w)[None, :]
        rgb[ys, xs] = rgb[ys, xs] * 0.8 + 6
        rgb[ys, x0 % w] *= 0.75
        rgb[ys, (x0 + pw - 1) % w] *= 0.75
        rgb[y0 % h, xs] *= 0.75
        rgb[(y0 + ph - 1) % h, xs] *= 0.75
    # Sealed cracks.
    f = periodic_noise(w, h, 5, seed + 3) + 0.15 * periodic_noise(w, h, 17, seed + 4)
    keep = periodic_noise(w, h, 3, seed + 5) > 0.5
    crack = (np.abs(f - 0.5) < 0.006) & keep
    rgb[crack] = np.array([44, 44, 48.0])
    return rgb, fine


def tex_sfroad(n=256):
    """A two-lane city street over 13 m (u) and 16 m (v): grey asphalt,
    oil darkening down the middle of each lane, the double yellow centre
    line, and a manhole cover."""
    rgb, fine = sf_asphalt(n, n, 9001)
    u = (np.arange(n) + 0.5) / n
    oil = np.exp(-((u - 0.27) / 0.05) ** 2) + np.exp(-((u - 0.73) / 0.05) ** 2)
    along = 0.6 + 0.4 * periodic_noise(n, n, 4, 9005)
    rgb *= (1 - 0.14 * oil[None, :] * along)[..., None]
    # Double yellow: 15 cm lines, 12 cm apart, worn in places.
    m = 13.0
    for c in (0.5 - 0.135 / m, 0.5 + 0.135 / m):
        line = np.abs(u - c) < 0.075 / m
        wear = 0.8 + 0.2 * periodic_noise(n, n, 12, 9007)[:, line]
        rgb[:, line] = np.array([236, 188, 40.0]) * wear[..., None]
    img = to_image(rgb)
    d = ImageDraw.Draw(img)
    for cx, cy in ((0.27 * n, 0.3 * n), (0.73 * n, 0.8 * n)):
        r = 0.32 / m * n
        d.ellipse([cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1], fill=(70, 70, 74))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(92, 88, 84))
        for k in range(-2, 3):
            d.line([cx - r * 0.8, cy + k * r * 0.35, cx + r * 0.8, cy + k * r * 0.35], fill=(64, 62, 60))
    return img


def tex_sfrails(n=128):
    """Hyde Street: the same asphalt with two cable car tracks down the
    middle, each a concrete trackway with two polished rails and the
    cable's slot between them."""
    rgb, fine = sf_asphalt(n, n, 9011, base=100)
    m = 13.0
    u = (np.arange(n) + 0.5) / n
    x = (u - 0.5) * m                     # metres from the centreline
    for c in (-1.75, 1.75):
        band = np.abs(x - c) < 0.95
        conc = 150 + 18 * (fbm(n, n, 6, 3, 9013)[:, band] - 0.5)
        rgb[:, band] = np.dstack([conc, conc * 0.98, conc * 0.95])
        for r in (c - 0.535, c + 0.535):
            rail = np.abs(x - r) < 0.05
            rgb[:, rail] = np.array([200, 204, 210.0]) * (0.9 + 0.1 * fine[:, rail, None])
            edge = (np.abs(x - r) >= 0.05) & (np.abs(x - r) < 0.11)
            rgb[:, edge] *= 0.55
        slot = np.abs(x - c) < 0.03
        rgb[:, slot] = np.array([30, 30, 32.0])
    # Joints across the trackway every 2 m.
    v = (np.arange(n) + 0.5) / n * 16.0
    joint = (v % 2.0) < 0.07
    trk = (np.abs(np.abs(x) - 1.75) < 0.95)
    rgb[np.ix_(joint, trk)] *= 0.8
    return to_image(rgb)


def tex_sfbrick(n=256):
    """The crooked street's red brick pavers, laid in running bond across
    the road with a darker soldier course along each edge, weathered and
    rubbed paler down the wheel tracks. 256 across the 13 m road, so a
    brick is ~8 x 5 texels and still reads as brick a few mips down."""
    rng = np.random.default_rng(9021)
    s = 2
    N = n * s
    img = Image.new("RGB", (N, N), (92, 60, 52))
    d = ImageDraw.Draw(img)
    rows = 64                    # 16 m / 0.25 m
    bw = N / 32                  # 13 m / 0.4 m
    rh = N / rows
    edge = N * 0.045             # the soldier course, ~0.6 m
    for r in range(rows):
        off = (r % 2) * bw * 0.5
        for c in range(-1, 33):
            x0, y0 = c * bw + off, r * rh
            t = rng.random()
            col = (int(140 + 62 * t + rng.integers(-12, 12)), int(56 + 30 * t), int(42 + 20 * t))
            d.rectangle([x0 + s * 0.8, y0 + s * 0.8, x0 + bw - s * 0.8, y0 + rh - s * 0.8], fill=col)
    # Soldier courses: bricks end-on along each edge, darker.
    for side in (0, 1):
        x0 = 0 if side == 0 else N - edge
        d.rectangle([x0, 0, x0 + edge, N], fill=(70, 40, 34))
        for r in range(int(N / (rh * 1.6))):
            y0 = r * rh * 1.6
            t = rng.random()
            d.rectangle([x0 + s, y0 + s, x0 + edge - s, y0 + rh * 1.6 - s],
                        fill=(int(108 + 30 * t), int(44 + 14 * t), int(36 + 10 * t)))
    img = img.resize((n, n), Image.Resampling.LANCZOS)
    rgb = np.asarray(img).astype(float)
    u = (np.arange(n) + 0.5) / n
    rub = np.exp(-((u - 0.3) / 0.08) ** 2) + np.exp(-((u - 0.7) / 0.08) ** 2)
    rgb = rgb * (1 + 0.12 * rub[None, :, None]) * (0.9 + 0.2 * fbm(n, n, 8, 3, 9023))[..., None]
    return to_image(rgb)


def tex_sfcurb(n=64):
    """The kerb and gutter, along the road: grey concrete cast in 1.2 m
    lengths, the joints dark, grime along the bottom. The same across u
    (the left kerb samples only its first column)."""
    v = (np.arange(n) + 0.5) / n
    g = fbm(n, n, 6, 3, 9031)
    rgb = lerp_color((150, 150, 148), (186, 186, 182), g)
    jv = (v * 2) % 1.0
    joint = (jv < 0.03) | (jv > 0.97)
    rgb[joint, :] = np.array([86, 86, 88.0])
    rgb *= (0.92 + 0.12 * np.random.default_rng(9033).random((n, n)))[..., None]
    return to_image(rgb)


def tex_sfwalk(n=64):
    """Sidewalk from above, 6 m a tile: 1.5 m concrete slabs with scored
    joints, the odd one replaced paler, and a few stains."""
    rng = np.random.default_rng(9041)
    g = fbm(n, n, 6, 3, 9043)
    rgb = lerp_color((176, 174, 168), (204, 202, 196), g)
    slab = n // 4
    for i in range(4):
        for j in range(4):
            if rng.random() < 0.2:
                rgb[j * slab:(j + 1) * slab, i * slab:(i + 1) * slab] *= 1.06
    idx = np.arange(n)
    joint = (idx % slab == 0)
    rgb[joint, :] *= 0.72
    rgb[:, joint] *= 0.72
    for _ in range(6):
        x, y, r = rng.uniform(0, n), rng.uniform(0, n), rng.uniform(1.5, 4)
        yy, xx = np.mgrid[0:n, 0:n]
        dd = np.hypot((xx - x + n / 2) % n - n / 2, (yy - y + n / 2) % n - n / 2)
        rgb *= (1 - 0.18 * np.clip(1 - dd / r, 0, 1))[..., None]
    return to_image(rgb)


def tex_sfrailing(w=128, h=128):
    """The sidewalk's railing over 4 m, 1 m high: a concrete kerb wall
    below, green painted iron above (posts every metre, a top rail and
    thin balusters) in front of the hedges behind."""
    s = 4
    W, H = w * s, h * s
    img = Image.new("RGB", (W, H), (38, 64, 40))
    d = ImageDraw.Draw(img)
    rng = np.random.default_rng(9051)
    for _ in range(400):
        x, y, r = rng.uniform(0, W), rng.uniform(0, H * 0.6), rng.uniform(2, 6) * s
        c = int(rng.integers(40, 90))
        d.ellipse([x - r, y - r, x + r, y + r], fill=(c // 2, c, c // 2 + 6))
    iron = (30, 72, 52)
    hi = (70, 120, 92)
    top = 0.06 * H
    wall = 0.58 * H
    for k in range(33):
        x = k * W / 32
        d.rectangle([x - 0.25 * s, top, x + 0.25 * s, wall], fill=iron)
    for k in range(5):
        x = k * W / 4
        d.rectangle([x - 1.2 * s, top - 0.02 * H, x + 1.2 * s, wall], fill=iron)
        d.line([x - 0.6 * s, top, x - 0.6 * s, wall], fill=hi, width=s)
    d.rectangle([0, top - 0.03 * H, W, top + 0.05 * H], fill=iron)
    d.line([0, top - 0.02 * H, W, top - 0.02 * H], fill=hi, width=s)
    d.rectangle([0, wall - 0.04 * H, W, wall], fill=iron)
    img = img.resize((w, h), Image.Resampling.LANCZOS)
    rgb = np.asarray(img).astype(float)
    g = fbm(w, h, 8, 3, 9053)
    y = (np.arange(h) + 0.5) / h
    conc = y > 0.58
    base = lerp_color((168, 166, 160), (198, 196, 190), g)
    grime = np.clip((y - 0.85) / 0.15, 0, 1)
    base *= (1 - 0.25 * grime)[:, None, None]
    rgb[conc] = base[conc]
    rgb[(y > 0.58) & (y < 0.62)] *= 0.8
    return to_image(rgb)


def tex_sfpier(n=128):
    """The pier's deck over 13 m (u) and 16 m (v): weathered planks laid
    across, grey-brown with dark gaps and rows of nail heads."""
    rng = np.random.default_rng(9061)
    v = (np.arange(n) + 0.5) / n
    plank = np.floor(v * 48).astype(int)          # 33 cm planks
    tone = rng.random(64)[plank]
    grain = fbm(n, n, 8, 4, 9063)
    lum = 118 + 34 * tone[:, None] + 26 * (grain - 0.5)
    rgb = np.dstack([lum * 1.04, lum * 0.98, lum * 0.9])
    gap = (v * 48) % 1.0 < 0.1
    rgb[gap, :] *= 0.45
    u = (np.arange(n) + 0.5) / n
    for c in (0.1, 0.3, 0.5, 0.7, 0.9):
        nail = np.abs(u - c) < 0.006
        rgb[np.ix_(~gap & ((v * 48) % 1.0 > 0.45) & ((v * 48) % 1.0 < 0.6), nail)] *= 0.55
    # A darker wear lane down each wheel track.
    rub = np.exp(-((u - 0.3) / 0.07) ** 2) + np.exp(-((u - 0.7) / 0.07) ** 2)
    rgb *= (1 - 0.1 * rub)[None, :, None]
    return to_image(rgb)


def tex_sfpierrail(w=128, h=128):
    """The pier's edge over 4 m, 1 m high: a timber bulwark of vertical
    boards in old blue-grey paint, a white cap rail on top."""
    rng = np.random.default_rng(9071)
    u = (np.arange(w) + 0.5) / w
    y = (np.arange(h) + 0.5) / h
    board = np.floor(u * 24).astype(int)
    tone = rng.random(32)[board]
    g = fbm(w, h, 8, 3, 9073)
    base = lerp_color((96, 120, 138), (128, 150, 166), g * 0.6 + 0.4 * tone[None, :])
    seam = (u * 24) % 1.0 < 0.08
    base[:, seam] *= 0.6
    flake = (fbm(w, h, 16, 3, 9075) > 0.62)
    base[flake] = base[flake] * 0.6 + np.array([140, 112, 84.0]) * 0.4
    cap = y < 0.14
    base[cap] = np.array([236, 234, 226.0])
    base[(y >= 0.14) & (y < 0.18)] *= 0.5
    post = ((u * 2) % 1.0 < 0.05)
    base[np.ix_(~cap, post)] *= 0.75
    return to_image(base)


SF_FACADES = [
    # body, trim, accent
    ((128, 172, 210), (246, 244, 236), (40, 60, 110)),
    ((240, 214, 120), (250, 248, 240), (70, 112, 70)),
    ((236, 158, 176), (250, 240, 226), (120, 36, 60)),
    ((152, 198, 160), (246, 246, 238), (40, 84, 56)),
    ((188, 162, 216), (248, 246, 240), (84, 50, 120)),
    ((242, 232, 212), (255, 255, 250), (40, 110, 120)),
    ((242, 172, 122), (250, 242, 228), (110, 60, 40)),
    ((128, 190, 192), (248, 246, 240), (170, 74, 44)),
]


def sf_facade_cell(body, trim, accent, seed, s=4):
    """One Victorian row house's front, 7 m by 12 m in 64 x 128 pixels
    (drawn s times larger): a garage and the steps up to a portico on the
    street storey, a bay window and a single window on each of the two
    floors above, and a bracketed cornice with a false front on top."""
    W, H = 64 * s, 128 * s
    px, py = W / 7.0, H / 12.0                     # pixels a metre
    rng = np.random.default_rng(seed)
    img = Image.new("RGB", (W, H), body)
    d = ImageDraw.Draw(img)

    def rect(x0, y0, x1, y1, col):                 # metres, y up from the street
        d.rectangle([x0 * px, H - y1 * py, x1 * px, H - y0 * py], fill=col)

    shade = tuple(int(c * 0.82) for c in body)
    # Siding: horizontal clapboard lines.
    for k in range(int(12 / 0.18)):
        y = k * 0.18
        d.line([0, H - y * py, W, H - y * py], fill=tuple(int(c * 0.93) for c in body), width=max(1, s // 2))
    # Street storey: garage door (left), steps and door under a portico (right).
    rect(0.4, 0.0, 3.4, 2.5, trim)
    rect(0.55, 0.0, 3.25, 2.35, tuple(int(c * 0.9) for c in trim))
    for k in range(4):
        rect(0.55, 0.12 + k * 0.58, 3.25, 0.16 + k * 0.58, tuple(int(c * 0.75) for c in trim))
    for k in range(6):
        rect(4.2 + k * 0.12, k * 0.45, 6.6, k * 0.45 + 0.45, (170, 160, 150) if k % 2 else (190, 182, 170))
    rect(4.6, 0.6, 6.4, 3.1, trim)
    rect(4.95, 0.6, 6.05, 2.75, accent)
    rect(5.05, 1.9, 5.95, 2.6, (60, 80, 100))
    rect(4.4, 3.0, 6.6, 3.2, trim)
    # Two floors: a three-sided bay (drawn flat) and a single tall window.
    for f in range(2):
        y0 = 3.4 + f * 3.3
        rect(0.3, y0 - 0.2, 3.7, y0 + 2.9, trim)
        rect(0.45, y0 - 0.05, 3.55, y0 + 2.75, shade)
        for k, (a, b_) in enumerate(((0.55, 1.35), (1.55, 2.45), (2.65, 3.45))):
            rect(a, y0 + 0.2, b_, y0 + 2.45, trim)
            glass = (52 + 10 * k, 74 + 10 * k, 98 + 12 * k)
            rect(a + 0.08, y0 + 0.28, b_ - 0.08, y0 + 2.37, glass)
            sky = (150, 180, 206)
            d.polygon([(a * px + 0.08 * px, H - (y0 + 2.37) * py), ((a + b_) / 2 * px, H - (y0 + 2.37) * py),
                       (a * px + 0.08 * px, H - (y0 + 1.6) * py)], fill=sky)
            rect(a + 0.08, y0 + 1.3, b_ - 0.08, y0 + 1.37, trim)
        rect(0.3, y0 + 2.75, 3.7, y0 + 2.95, accent)
        rect(4.6, y0 + 0.2, 6.2, y0 + 2.5, trim)
        rect(4.7, y0 + 0.3, 6.1, y0 + 2.35, (58, 80, 104))
        d.polygon([(4.7 * px, H - (y0 + 2.35) * py), (5.4 * px, H - (y0 + 2.35) * py), (4.7 * px, H - (y0 + 1.5) * py)],
                  fill=(150, 180, 206))
        rect(4.7, y0 + 1.3, 6.1, y0 + 1.37, trim)
        d.polygon([(4.45 * px, H - (y0 + 2.55) * py), (6.35 * px, H - (y0 + 2.55) * py), (5.4 * px, H - (y0 + 3.0) * py)],
                  fill=trim)
        rect(4.5, y0 + 0.05, 6.3, y0 + 0.2, trim)
    # Cornice: frieze, brackets, a deep moulding, then the false front.
    rect(0.0, 10.0, 7.0, 10.35, accent)
    for k in range(8):
        x = 0.25 + k * 0.92
        rect(x, 10.0, x + 0.22, 10.75, trim)
    rect(0.0, 10.75, 7.0, 11.05, trim)
    rect(0.0, 11.05, 7.0, 11.75, body)
    rect(0.0, 11.75, 7.0, 12.0, trim)
    rect(2.6, 11.15, 4.4, 11.65, accent)
    rect(0.0, 0.0, 0.18, 10.0, trim)
    rect(6.82, 0.0, 7.0, 10.0, trim)
    return img


def tex_sffacade():
    """Eight Victorian row houses in their painted colours, four across
    and two down (64 x 128 each); the game picks one per house."""
    s = 4
    img = Image.new("RGB", (256 * s, 256 * s))
    for i, (body, trim, accent) in enumerate(SF_FACADES):
        cell = sf_facade_cell(body, trim, accent, 9100 + i, s)
        img.paste(cell, ((i % 4) * 64 * s, (i // 4) * 128 * s))
    return img.resize((256, 256), Image.Resampling.LANCZOS)


def tex_sfbldg(n=128):
    """Plain stucco apartment blocks for the city behind the streets, 8 m
    across and two 3.2 m floors a tile: white walls (vertex colours tint
    them) and a grid of sash windows with ledges."""
    s = 4
    N = n * s
    img = Image.new("RGB", (N, N), (232, 230, 224))
    d = ImageDraw.Draw(img)
    p = N / 8.0
    rng = np.random.default_rng(9201)
    for f in range(2):
        for c in range(4):
            x0, y0 = (c * 2 + 0.5) * p, (f * 3.2 + 0.8) * p * (8.0 / 6.4)
            ww, wh = 1.0 * p, 1.6 * p * (8.0 / 6.4)
            d.rectangle([x0 - 0.1 * p, y0 - 0.1 * p, x0 + ww + 0.1 * p, y0 + wh + 0.2 * p], fill=(250, 250, 246))
            t = rng.random()
            d.rectangle([x0, y0, x0 + ww, y0 + wh], fill=(int(50 + 40 * t), int(66 + 40 * t), int(86 + 40 * t)))
            d.rectangle([x0, y0 + wh * 0.48, x0 + ww, y0 + wh * 0.52], fill=(240, 240, 236))
            d.rectangle([x0 - 0.15 * p, y0 + wh + 0.15 * p, x0 + ww + 0.15 * p, y0 + wh + 0.3 * p], fill=(200, 198, 192))
    img = img.resize((n, n), Image.Resampling.LANCZOS)
    rgb = np.asarray(img).astype(float) * (0.94 + 0.08 * fbm(n, n, 8, 3, 9203))[..., None]
    return to_image(rgb)


def tex_sfroof(n=128):
    """The city from above on the same street grid as sfstreet, for the
    ground where no blocks stand (away from the course, out to the hills):
    each block's ring of houses painted on as narrow flat roofs in tar
    grey, gravel and the odd terracotta round the backyards."""
    return tex_sfstreet(n, roofs=True)


def tex_sfstreet(n=128, roofs=False):
    """The ground under the city, one block of the street grid a tile (47 m
    east-west, 65 m north-south, as world_sf.c lays the blocks): asphalt
    streets with zebra crossings at the corners, sidewalks, and inside them
    backyard lawns with fence lines and trees (the houses stand on the
    edges; where none do, it reads as a pocket park)."""
    rng = np.random.default_rng(9231)
    s = 4
    N = n * s
    W, H = 47.0, 65.0
    px, pz = N / W, N / H
    img = Image.new("RGB", (N, N), (92, 92, 96))
    d = ImageDraw.Draw(img)
    def rect(x0, z0, x1, z1, c):
        d.rectangle([x0 * px, z0 * pz, x1 * px - 1, z1 * pz - 1], fill=c)
    # Sidewalks, then the block inside them: the ring the houses stand on
    # paved (where one is left out, a forecourt), the yards inside it.
    rect(3.0, 3.0, W - 3.0, H - 3.0, (188, 186, 180))
    rect(4.5, 4.5, W - 4.5, H - 4.5, (150, 146, 138))
    rect(14.5, 14.5, W - 14.5, H - 14.5, (96, 128, 72))
    # Back fences between the lots, out from each edge to mid-block.
    fence = (78, 104, 60)
    x = 4.5
    while x < W - 4.5 - 3:
        x += rng.uniform(6.5, 8.0)
        rect(x - 0.12, 4.5, x + 0.12, H / 2 - 1, fence)
        rect(x - 0.12, H / 2 + 1, x + 0.12, H - 4.5, fence)
    z = 14.5
    while z < H - 14.5 - 3:
        z += rng.uniform(6.5, 8.0)
        rect(4.5, z - 0.12, W / 2 - 1, z + 0.12, fence)
        rect(W / 2 + 1, z - 0.12, W - 4.5, z + 0.12, fence)
    rect(W / 2 - 0.12, 14.5, W / 2 + 0.12, H - 14.5, fence)
    # Patios and the odd shed along the backs.
    for _ in range(14):
        cx, cz = rng.uniform(15, W - 15), rng.uniform(15, H - 15)
        w, h = rng.uniform(1.5, 3.2), rng.uniform(1.5, 3.0)
        c = int(rng.integers(150, 200))
        rect(cx - w / 2, cz - h / 2, cx + w / 2, cz + h / 2, (c, c - 4, c - 12))
    # Trees in the yards.
    for _ in range(26):
        cx, cz, r = rng.uniform(14, W - 14), rng.uniform(14, H - 14), rng.uniform(1.4, 2.8)
        d.ellipse([(cx - r + 0.5) * px, (cz - r + 0.7) * pz, (cx + r + 0.5) * px, (cz + r + 0.7) * pz], fill=(52, 72, 44))
        d.ellipse([(cx - r) * px, (cz - r) * pz, (cx + r) * px, (cz + r) * pz], fill=(62, 96, 52))
        d.ellipse([(cx - r * 0.6) * px, (cz - r * 0.7) * pz, (cx + r * 0.2) * px, cz * pz], fill=(88, 124, 66))
    if roofs:
        cols = [(134, 132, 127), (156, 152, 144), (180, 176, 166), (200, 196, 184), (142, 138, 136), (170, 156, 136),
                (164, 112, 92), (214, 210, 200)]
        def lot(x0, z0, x1, z1):
            c = cols[int(rng.integers(0, len(cols)))]
            rect(x0, z0, x1, z1, (70, 70, 74))
            rect(x0 + 0.3, z0 + 0.3, x1 - 0.3, z1 - 0.3, c)
        D = 10.0
        x = 4.5
        while x < W - 4.5 - 0.5:
            w = min(rng.uniform(6.5, 15.0), W - 4.5 - x)
            lot(x, 4.5, x + w, 4.5 + D)
            lot(x, H - 4.5 - D, x + w, H - 4.5)
            x += w
        z = 4.5 + D
        while z < H - 4.5 - D - 0.5:
            w = min(rng.uniform(6.5, 15.0), H - 4.5 - D - z)
            lot(4.5, z, 4.5 + D, z + w)
            lot(W - 4.5 - D, z, W - 4.5, z + w)
            z += w
    # Zebra crossings over each street just short of the crossroads, and
    # the crossroads' own darker patch of asphalt.
    zebra = (226, 226, 220)
    for c0 in (0.0, 1.0):
        for c1 in (0.0, 1.0):
            cx, cz = c0 * W, c1 * H
            sx, sz = (1 if c0 == 0 else -1), (1 if c1 == 0 else -1)
            for k in range(6):
                o = -2.5 + k * 1.0
                # Across the east-west street (the band along z = cz).
                xa = cx + sx * 4.8
                rect(min(xa, xa + sx * 2.2), cz + o - 0.25 if c1 == 0 else cz - o - 0.25,
                     max(xa, xa + sx * 2.2), cz + o + 0.25 if c1 == 0 else cz - o + 0.25, zebra)
                za = cz + sz * 4.8
                rect(cx + o - 0.25 if c0 == 0 else cx - o - 0.25, min(za, za + sz * 2.2),
                     cx + o + 0.25 if c0 == 0 else cx - o + 0.25, max(za, za + sz * 2.2), zebra)
    img = img.resize((n, n), Image.Resampling.LANCZOS)
    rgb = np.asarray(img).astype(float) * (0.92 + 0.12 * fbm(n, n, 8, 3, 9233))[..., None]
    return to_image(rgb)


def tex_sfstall():
    """The pier's market stalls, one front a cell, four down (6 m by 3 m
    each, 256 x 64): a sign board in the stall's colour with its name, a
    striped awning with a scalloped edge, the service window with what it
    sells on the counter, and painted boards below."""
    s = 2
    W, H = 256 * s, 64 * s
    img = Image.new("RGB", (W, H * 4))
    stalls = [("CRAB", (214, 72, 56)), ("CHOWDER", (60, 116, 190)), ("SOURDOUGH", (226, 168, 40)), ("GIFTS", (70, 150, 96))]
    rng = np.random.default_rng(9251)
    font = ImageFont.truetype(font_path("DejaVuSans-Bold.ttf"), 22 * s)
    for i, (name, col) in enumerate(stalls):
        cell = Image.new("RGB", (W, H), (244, 240, 230))
        d = ImageDraw.Draw(cell)
        p, q = W / 6.0, H / 3.0   # pixels a metre across and down
        d.rectangle([0, 0, W, 0.62 * q], fill=col)
        d.rectangle([0, 0.62 * q, W, 0.68 * q], fill=(250, 248, 240))
        bb = d.textbbox((0, 0), name, font=font)
        tw, th = bb[2] - bb[0], bb[3] - bb[1]
        k = min((W * 0.8) / tw, 0.5 * q / th)
        layer = Image.new("RGB", (tw, th), col)
        ImageDraw.Draw(layer).text((-bb[0], -bb[1]), name, font=font, fill=(255, 255, 250))
        layer = layer.resize((max(1, int(tw * k)), max(1, int(th * k))), Image.Resampling.LANCZOS)
        cell.paste(layer, (int((W - layer.width) / 2), int(0.31 * q - layer.height / 2)))
        # The awning: stripes 0.5 m wide, scalloped along its foot.
        a0, a1 = 0.68 * q, 1.15 * q
        for k2 in range(12):
            x0 = k2 * 0.5 * p
            c = col if k2 % 2 == 0 else (250, 248, 240)
            d.rectangle([x0, a0, x0 + 0.5 * p, a1], fill=c)
            d.ellipse([x0, a1 - 0.12 * q, x0 + 0.5 * p, a1 + 0.12 * q], fill=c)
        # The window: dark inside, a lit shelf, the wares on the counter.
        w0, w1 = 1.35 * q, 2.15 * q
        d.rectangle([0.3 * p, w0, W - 0.3 * p, w1], fill=(52, 44, 40))
        d.rectangle([0.3 * p, w0 + 0.12 * q, W - 0.3 * p, w0 + 0.16 * q], fill=(120, 104, 88))
        for x in np.arange(0.6, 5.5, 0.55):
            cx, cy = x * p + rng.uniform(-0.05, 0.05) * p, w1 - 0.14 * q
            if name == "CRAB":
                d.ellipse([cx - 0.2 * p, cy - 0.1 * q, cx + 0.2 * p, cy + 0.1 * q], fill=(230, 84, 54))
                d.ellipse([cx - 0.26 * p, cy - 0.16 * q, cx - 0.14 * p, cy - 0.06 * q], fill=(230, 84, 54))
                d.ellipse([cx + 0.14 * p, cy - 0.16 * q, cx + 0.26 * p, cy - 0.06 * q], fill=(230, 84, 54))
            elif name == "CHOWDER":
                d.ellipse([cx - 0.2 * p, cy - 0.16 * q, cx + 0.2 * p, cy + 0.1 * q], fill=(196, 150, 92))
                d.ellipse([cx - 0.15 * p, cy - 0.14 * q, cx + 0.15 * p, cy - 0.04 * q], fill=(246, 236, 210))
            elif name == "SOURDOUGH":
                d.ellipse([cx - 0.22 * p, cy - 0.14 * q, cx + 0.22 * p, cy + 0.1 * q], fill=(206, 152, 84))
                d.line([cx - 0.1 * p, cy - 0.1 * q, cx + 0.05 * p, cy + 0.02 * q], fill=(236, 210, 160), width=s)
            else:
                c = [(240, 200, 80), (120, 180, 230), (230, 120, 150), (250, 250, 250)][int(rng.integers(0, 4))]
                d.rectangle([cx - 0.16 * p, cy - 0.3 * q, cx + 0.16 * p, cy + 0.1 * q], fill=c)
        d.rectangle([0.1 * p, w1, W - 0.1 * p, w1 + 0.18 * q], fill=(250, 248, 240))
        d.rectangle([0.1 * p, w1 + 0.18 * q, W - 0.1 * p, w1 + 0.24 * q], fill=(170, 160, 150))
        # Boards below.
        for y in np.arange(2.45, 3.0, 0.18):
            d.line([0, y * q, W, y * q], fill=(206, 200, 190), width=s)
        img.paste(cell, (0, i * H))
    return img.resize((256, 256), Image.Resampling.LANCZOS)


def tex_sfsigns():
    """Signs, eight 256 x 32 boards down a 256 x 256 atlas: six pier
    numbers for the Embarcadero's warehouse fronts (cream board, dark
    green letters, a thin border), then the green street name blades,
    LOMBARD ST and HYDE ST."""
    s = 4
    W, H = 256 * s, 32 * s
    img = Image.new("RGB", (W, H * 8))
    font = ImageFont.truetype(font_path("DejaVuSans-Bold.ttf"), 22 * s)
    boards = [(f"PIER {n}", (238, 232, 212), (34, 74, 52)) for n in (45, 43, 41, 39, 35, 33)]
    boards += [("LOMBARD ST", (30, 108, 64), (250, 250, 244)), ("HYDE ST", (30, 108, 64), (250, 250, 244))]
    for i, (text, bg, fg) in enumerate(boards):
        cell = Image.new("RGB", (W, H), bg)
        d = ImageDraw.Draw(cell)
        d.rectangle([2 * s, 2 * s, W - 3 * s, H - 3 * s], outline=fg, width=s + 1)
        bb = d.textbbox((0, 0), text, font=font)
        tw, th = bb[2] - bb[0], bb[3] - bb[1]
        layer = Image.new("RGB", (tw, th), bg)
        ImageDraw.Draw(layer).text((-bb[0], -bb[1]), text, font=font, fill=fg)
        k = min((W * 0.86) / tw, (H * 0.6) / th)
        layer = layer.resize((int(tw * k), int(th * k)), Image.Resampling.LANCZOS)
        cell.paste(layer, ((W - layer.width) // 2, (H - layer.height) // 2))
        img.paste(cell, (0, i * H))
    return img.resize((256, 256), Image.Resampling.LANCZOS)


def tex_sfgrass(n=64):
    """Pioneer Park's lawn, 6 m a tile: mown grass in two greens with the
    odd darker clump, tileable."""
    g = fbm(n, n, 6, 4, 9241)
    rgb = lerp_color((70, 112, 52), (112, 152, 74), g)
    stripe = 0.94 + 0.06 * (np.arange(n) // (n // 4) % 2)
    rgb *= stripe[None, :, None]
    return to_image(rgb)


def tex_sfhedge(n=128):
    """The crooked street's beds: dark green hedge leaves crowded with
    hydrangea heads in pink, violet, blue and white, tileable."""
    rng = np.random.default_rng(9221)
    s = 2
    N = n * s
    g = fbm(N, N, 12, 4, 9223)
    rgb = lerp_color((34, 70, 34), (78, 128, 62), g)
    img = to_image(rgb)
    d = ImageDraw.Draw(img)
    heads = [(232, 132, 176), (198, 128, 214), (128, 150, 230), (244, 236, 244), (220, 96, 140)]
    for _ in range(170):
        x, y, r = rng.uniform(0, N), rng.uniform(0, N), rng.uniform(3, 7) * s
        c = heads[int(rng.integers(0, len(heads)))]
        for ox in (-N, 0, N):
            for oy in (-N, 0, N):
                cx, cy = x + ox, y + oy
                d.ellipse([cx - r, cy - r + 1.5 * s, cx + r, cy + r + 1.5 * s], fill=tuple(int(k * 0.55) for k in c))
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=c)
                d.ellipse([cx - r * 0.5, cy - r * 0.7, cx + r * 0.2, cy - r * 0.1],
                          fill=tuple(min(255, int(k * 1.12)) for k in c))
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_sfshed(n=128):
    """A pier shed's front over 8 m, 8 m high: pale concrete pilasters, a
    big arched doorway dark inside, a cornice and a plain sign band.
    White, so vertex colours tint each shed."""
    s = 4
    N = n * s
    img = Image.new("RGB", (N, N), (226, 224, 216))
    d = ImageDraw.Draw(img)
    p = N / 8.0
    d.rectangle([0, 0, N, 0.9 * p], fill=(244, 242, 236))
    d.rectangle([0, 0.9 * p, N, 1.05 * p], fill=(170, 168, 160))
    d.rectangle([0.6 * p, 1.4 * p, 7.4 * p, 2.3 * p], fill=(250, 250, 246))
    d.rectangle([0.6 * p, 2.25 * p, 7.4 * p, 2.35 * p], fill=(150, 148, 140))
    d.rectangle([1.4 * p, 3.6 * p, 6.6 * p, 8 * p], fill=(250, 248, 242))
    d.ellipse([1.4 * p, 2.8 * p, 6.6 * p, 4.6 * p], fill=(250, 248, 242))
    d.rectangle([1.7 * p, 3.7 * p, 6.3 * p, 8 * p], fill=(54, 50, 50))
    d.ellipse([1.7 * p, 3.0 * p, 6.3 * p, 4.4 * p], fill=(54, 50, 50))
    for k in range(5):
        x = 1.7 * p + k * 1.15 * p
        d.line([x, 3.1 * p, x, 5.0 * p], fill=(120, 140, 150), width=s)
    d.line([1.7 * p, 5.0 * p, 6.3 * p, 5.0 * p], fill=(120, 140, 150), width=s)
    for x in (0.0, 7.4 * p):
        d.rectangle([x, 1.05 * p, x + 0.6 * p, N], fill=(236, 234, 228))
    img = img.resize((n, n), Image.Resampling.LANCZOS)
    rgb = np.asarray(img).astype(float) * (0.92 + 0.1 * fbm(n, n, 8, 3, 9233))[..., None]
    return to_image(rgb)


def tex_sfsign():
    """The wharf's sign: a ship's wheel with a red crab in it, and
    FISHERMAN'S WHARF in white on navy between red rules."""
    s = 4
    W, H = 256 * s, 64 * s
    navy, red, white = (24, 44, 96), (214, 52, 40), (250, 248, 240)
    img = Image.new("RGB", (W, H), navy)
    d = ImageDraw.Draw(img)
    cx, cy, R = 32 * s, 32 * s, 26 * s
    for k in range(8):
        a = k * math.pi / 4
        d.line([cx, cy, cx + math.cos(a) * (R + 4 * s), cy + math.sin(a) * (R + 4 * s)], fill=(200, 150, 70), width=3 * s)
        ex, ey = cx + math.cos(a) * (R + 4 * s), cy + math.sin(a) * (R + 4 * s)
        d.ellipse([ex - 2.5 * s, ey - 2.5 * s, ex + 2.5 * s, ey + 2.5 * s], fill=(200, 150, 70))
    d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=(200, 150, 70), width=4 * s)
    d.ellipse([cx - R + 5 * s, cy - R + 5 * s, cx + R - 5 * s, cy + R - 5 * s], fill=(236, 232, 220))
    # The crab: a body, eyes, claws and legs.
    d.ellipse([cx - 11 * s, cy - 5 * s, cx + 11 * s, cy + 8 * s], fill=red)
    for sx in (-1, 1):
        d.ellipse([cx + sx * 4 * s - 2 * s, cy - 10 * s, cx + sx * 4 * s + 2 * s, cy - 6 * s], fill=red)
        d.ellipse([cx + sx * 15 * s - 5 * s, cy - 14 * s, cx + sx * 15 * s + 5 * s, cy - 5 * s], fill=red)
        d.line([cx + sx * 9 * s, cy - 2 * s, cx + sx * 14 * s, cy - 7 * s], fill=red, width=3 * s)
        for k in range(3):
            d.line([cx + sx * 9 * s, cy + (2 + 2 * k) * s, cx + sx * (17 + k) * s, cy + (6 + 4 * k) * s], fill=red,
                   width=2 * s)
    font = ImageFont.truetype(font_path("DejaVuSerif-Bold.ttf"), 30 * s)
    bb = d.textbbox((0, 0), "FISHERMAN'S WHARF", font=font)
    layer = Image.new("RGB", (bb[2] - bb[0], bb[3] - bb[1]), navy)
    ImageDraw.Draw(layer).text((-bb[0], -bb[1]), "FISHERMAN'S WHARF", font=font, fill=white)
    layer = layer.resize((176 * s, 26 * s), Image.Resampling.LANCZOS)
    img.paste(layer, (70 * s, 19 * s))
    d.rectangle([66 * s, 6 * s, W - 4 * s, 9 * s], fill=red)
    d.rectangle([66 * s, 55 * s, W - 4 * s, 58 * s], fill=red)
    return img.resize((256, 64), Image.Resampling.LANCZOS)


def tex_sfbay(n=128):
    """The bay: cold grey-green water, choppy, with darker troughs and pale
    wind-blown streaks, tileable."""
    g = fbm(n, n, 6, 5, 9241)
    chop = fbm(n, n, 16, 3, 9243)
    rgb = lerp_color((176, 196, 196), (236, 246, 244), 0.5 * g + 0.5 * chop)
    streak = np.abs(periodic_noise(n, n, 3, 9245) - 0.5) < 0.025
    rgb[streak] = rgb[streak] * 1.08
    return to_image(rgb)


def tex_sffog(n=64):
    """A bank of sea fog for the Golden Gate: a soft white heap, densest
    low in the middle and fading out to every edge, its top lumpy (alpha)."""
    yy, xx = np.mgrid[0:n, 0:n]
    u, v = (xx + 0.5) / n - 0.5, (yy + 0.5) / n
    lump = fbm(n, n, 4, 3, 9251)
    top = 0.25 + 0.3 * lump
    across = np.exp(-(u / 0.26) ** 2)
    up = np.clip((v - top) / 0.4, 0, 1) ** 1.5
    foot = np.clip((1.0 - v) / 0.25, 0, 1)
    a = across * up * foot * 0.85
    rgb = lerp_color((210, 218, 228), (252, 252, 252), np.clip(1.3 - v, 0, 1))
    return to_image(rgb, a * 255)


def tex_sfpalm(n=128):
    """A Canary Island date palm's frond for the crown's cut-out planes:
    a stiff yellow midrib from the base (v = 1) to the tip (v = 0), with
    narrow dark olive leaflets crowded along both sides."""
    s = 4
    img = Image.new("RGBA", (n * s, n * s), (70, 100, 40, 0))
    d = ImageDraw.Draw(img)
    cx = n * s / 2
    rng = np.random.default_rng(9261)
    for i in range(34):
        t = i / 33
        y = (n * 0.97 - t * n * 0.94) * s
        length = n * 0.44 * math.sin(math.pi * (0.1 + 0.88 * t)) ** 0.6 * s
        for side in (-1, 1):
            sh = 0.75 + 0.35 * rng.random()
            col = (int(74 * sh), int(108 * sh), int(44 * sh), 255)
            tip = (cx + side * length, y + length * 0.34)
            d.line([(cx, y), tip], fill=col, width=int(2.2 * s))
    d.line([(cx, n * 0.99 * s), (cx, n * 0.02 * s)], fill=(196, 168, 72, 255), width=3 * s)
    return img.resize((n, n), Image.Resampling.LANCZOS)


def tex_sfpalmbark(n=64):
    """The date palm's trunk: the diamond pattern of old leaf bases, warm
    brown with darker seams, two diamonds round and four up a tile."""
    yy, xx = np.mgrid[0:n, 0:n]
    u, v = (xx + 0.5) / n, (yy + 0.5) / n
    a = (u * 4 + v * 4) % 1.0
    b = (u * 4 - v * 4) % 1.0
    seam = np.minimum(np.minimum(a, 1 - a), np.minimum(b, 1 - b))
    g = fbm(n, n, 8, 3, 9271)
    rgb = lerp_color((96, 70, 44), (168, 128, 84), np.clip(seam * 5, 0, 1) * (0.7 + 0.3 * g))
    return to_image(rgb)


SF_CC_MAROON = (128, 24, 34)
SF_CC_CREAM = (238, 226, 190)
SF_CC_GOLD = (214, 172, 72)


def tex_sfcablecar(n=256):
    """Hyde Street's cable car, as an atlas for its model (tools/
    build_sf_props.py maps each face to its cell): the maroon lower side
    with gold pinstriped panels and HYDE ST in gold over its length (top
    quarter), the cream cabin side with five framed windows (second
    quarter); then the dash end with its number and the cabin's end
    windows, the roof's destination sign and the clerestory's little
    windows, and the roof boards and the benches' varnished wood."""
    s = 4
    N = n * s
    img = Image.new("RGB", (N, N), SF_CC_CREAM)
    d = ImageDraw.Draw(img)
    M, C, G = SF_CC_MAROON, SF_CC_CREAM, SF_CC_GOLD
    q = N // 4
    serif = font_path("DejaVuSerif-Bold.ttf")

    def text(box, msg, fill, bg):
        x0, y0, x1, y1 = box
        font = ImageFont.truetype(serif, 40 * s)
        bb = d.textbbox((0, 0), msg, font=font)
        layer = Image.new("RGB", (bb[2] - bb[0], bb[3] - bb[1]), bg)
        ImageDraw.Draw(layer).text((-bb[0], -bb[1]), msg, font=font, fill=fill)
        layer = layer.resize((int(x1 - x0), int(y1 - y0)), Image.Resampling.LANCZOS)
        img.paste(layer, (int(x0), int(y0)))

    # A: the lower side, 8.4 m long, 1.1 m high.
    d.rectangle([0, 0, N, q], fill=M)
    d.rectangle([0, 0, N, 3 * s], fill=G)
    d.rectangle([0, q - 6 * s, N, q], fill=(70, 18, 22))
    panels = [(0.02, 0.24), (0.26, 0.74), (0.76, 0.98)]
    for a, b in panels:
        x0, x1 = a * N, b * N
        d.rounded_rectangle([x0, 10 * s, x1, q - 14 * s], radius=6 * s, outline=G, width=2 * s)
        d.rounded_rectangle([x0 + 5 * s, 15 * s, x1 - 5 * s, q - 19 * s], radius=4 * s, outline=G, width=s)
    text((0.33 * N, 20 * s, 0.67 * N, q - 24 * s), "HYDE  ST", G, M)
    for a, b in (panels[0], panels[2]):
        text(((a + 0.07) * N, 22 * s, (b - 0.07) * N, q - 26 * s), "17", G, M)
    # B: the cabin side, 4 m of it: a cream frame, five windows.
    y0, y1 = q, 2 * q
    d.rectangle([0, y0, N, y1], fill=C)
    d.rectangle([0, y0, N, y0 + 6 * s], fill=G)
    d.rectangle([0, y1 - 8 * s, N, y1], fill=M)
    for k in range(5):
        wx0 = (0.04 + k * 0.19) * N
        wx1 = wx0 + 0.15 * N
        wy0, wy1 = y0 + 14 * s, y1 - 18 * s
        d.rectangle([wx0 - 3 * s, wy0 - 3 * s, wx1 + 3 * s, wy1 + 3 * s], fill=(110, 60, 36))
        d.rectangle([wx0, wy0, wx1, wy1], fill=(40, 52, 66))
        d.polygon([(wx0, wy0), (wx0 + (wx1 - wx0) * 0.6, wy0), (wx0, wy0 + (wy1 - wy0) * 0.7)], fill=(120, 148, 170))
        d.rectangle([wx0, (wy0 + wy1) / 2 - s, wx1, (wy0 + wy1) / 2 + s], fill=(110, 60, 36))
    for k in range(6):
        x = (0.02 + k * 0.19) * N
        d.rectangle([x, y0 + 8 * s, x + 2 * s, y1 - 10 * s], fill=G)
    # C: the dash end (left) and the cabin's end (right).
    y0, y1 = 2 * q, 3 * q
    d.rectangle([0, y0, N // 2, y1], fill=M)
    d.rectangle([0, y0, N // 2, y0 + 3 * s], fill=G)
    d.rounded_rectangle([12 * s, y0 + 10 * s, N // 2 - 12 * s, y1 - 14 * s], radius=6 * s, outline=G, width=2 * s)
    cx, cy, r = N // 4, (y0 + y1) // 2 - 2 * s, 18 * s
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=C, outline=G, width=2 * s)
    text((cx - 12 * s, cy - 11 * s, cx + 12 * s, cy + 11 * s), "17", M, C)
    d.rectangle([N // 2, y0, N, y1], fill=C)
    d.rectangle([N // 2, y1 - 8 * s, N, y1], fill=M)
    for k in range(2):
        wx0 = N // 2 + (0.06 + k * 0.23) * N
        wx1 = wx0 + 0.15 * N
        d.rectangle([wx0 - 3 * s, y0 + 7 * s, wx1 + 3 * s, y1 - 13 * s], fill=(110, 60, 36))
        d.rectangle([wx0, y0 + 10 * s, wx1, y1 - 16 * s], fill=(40, 52, 66))
        d.polygon([(wx0, y0 + 10 * s), (wx0 + 0.09 * N, y0 + 10 * s), (wx0, y0 + 40 * s)], fill=(120, 148, 170))
    # D: the destination sign (left) and the clerestory's windows (right).
    y0, y1 = 3 * q, 3 * q + q // 2
    d.rectangle([0, y0, N // 2, y1], fill=(24, 22, 22))
    d.rectangle([0, y0, N // 2, y0 + 2 * s], fill=G)
    d.rectangle([0, y1 - 2 * s, N // 2, y1], fill=G)
    text((16 * s, y0 + 6 * s, N // 2 - 16 * s, y1 - 6 * s), "HYDE ST", (250, 240, 210), (24, 22, 22))
    d.rectangle([N // 2, y0, N, y1], fill=C)
    for k in range(8):
        wx0 = N // 2 + (k * 0.0625 + 0.008) * N
        d.rectangle([wx0, y0 + 6 * s, wx0 + 0.045 * N, y1 - 6 * s], fill=(52, 62, 74))
    # E: the roof's boards (left) and varnished wood (right).
    y0, y1 = 3 * q + q // 2, N
    for k in range(12):
        x0 = k * (N // 2) / 12
        t = 0.94 + 0.06 * ((k * 7) % 3)
        d.rectangle([x0, y0, x0 + (N // 2) / 12, y1], fill=tuple(int(c * t) for c in (240, 236, 224)))
        d.line([x0, y0, x0, y1], fill=(200, 196, 186), width=s)
    for k in range(8):
        y = y0 + k * (y1 - y0) / 8
        t = 0.9 + 0.1 * ((k * 5) % 3)
        d.rectangle([N // 2, y, N, y + (y1 - y0) / 8], fill=tuple(int(c * t) for c in (150, 92, 48)))
        d.line([N // 2, y, N, y], fill=(96, 56, 30), width=s)
    img = img.resize((n, n), Image.Resampling.LANCZOS)
    rgb = np.asarray(img).astype(float) * (0.95 + 0.06 * fbm(n, n, 16, 2, 9281))[..., None]
    return to_image(rgb)


# Shopfronts and the plainer buildings of North Beach and Telegraph Hill,
# 7 m by 12 m each like the Victorians: four shops with flats above and
# four kinds of flats. (shop sign, upper body, trim, shop frame, sign
# colours, awning stripes) for the shops.
SF_SHOPS = [
    ("CAFFE", (232, 220, 196), (250, 246, 236), (40, 86, 60), ((40, 86, 60), (250, 246, 236)), ((200, 40, 40), (248, 244, 236))),
    ("DELI", (168, 72, 58), (240, 232, 214), (240, 232, 214), ((240, 232, 214), (176, 34, 34)), ((40, 120, 70), (248, 244, 236))),
    ("BAKERY", (238, 214, 150), (250, 246, 236), (34, 48, 96), ((34, 48, 96), (232, 196, 90)), ((50, 90, 180), (248, 244, 236))),
    ("BOOKS", (150, 170, 140), (240, 236, 222), (86, 52, 32), ((110, 30, 36), (232, 196, 90)), ((110, 30, 36), (110, 30, 36))),
]


def sf_facade2_cell(style, s=4):
    W, H = 64 * s, 128 * s
    px, py = W / 7.0, H / 12.0
    rng = np.random.default_rng(9300 + style)

    def rect(d, x0, y0, x1, y1, col):
        d.rectangle([x0 * px, H - y1 * py, x1 * px, H - y0 * py], fill=col)

    def window(d, x0, y0, x1, y1, frame, glass=(52, 70, 92), pediment=None):
        rect(d, x0 - 0.12, y0 - 0.12, x1 + 0.12, y1 + 0.12, frame)
        rect(d, x0, y0, x1, y1, glass)
        d.polygon([(x0 * px, H - y1 * py), ((x0 + (x1 - x0) * 0.55) * px, H - y1 * py), (x0 * px, H - (y0 + (y1 - y0) * 0.45) * py)],
                  fill=(140, 170, 196))
        rect(d, x0, (y0 + y1) / 2 - 0.04, x1, (y0 + y1) / 2 + 0.04, frame)
        if pediment:
            d.polygon([((x0 - 0.2) * px, H - (y1 + 0.15) * py), ((x1 + 0.2) * px, H - (y1 + 0.15) * py),
                       ((x0 + x1) / 2 * px, H - (y1 + 0.55) * py)], fill=pediment)

    if style < 4:
        sign, body, trim, frame, (sign_bg, sign_fg), (aw_a, aw_b) = SF_SHOPS[style]
        img = Image.new("RGB", (W, H), body)
        d = ImageDraw.Draw(img)
        # The shop: frame, a big window either side of the door, a kick plate.
        rect(d, 0.0, 0.0, 7.0, 3.9, frame)
        rect(d, 0.35, 0.55, 3.35, 2.75, (46, 60, 72))
        rect(d, 4.65, 0.55, 6.65, 2.75, (46, 60, 72))
        for x0, x1 in ((0.35, 3.35), (4.65, 6.65)):
            d.polygon([(x0 * px, H - 2.75 * py), ((x0 + 1.3) * px, H - 2.75 * py), (x0 * px, H - 1.6 * py)], fill=(130, 160, 180))
            rect(d, x0, 0.4, x1, 0.55, trim)
            # Goods in the window: a few warm blobs.
            for k in range(4):
                bx = x0 + 0.3 + k * (x1 - x0 - 0.6) / 3
                c = tuple(int(v) for v in rng.integers(120, 230, 3))
                d.ellipse([(bx - 0.2) * px, H - 1.15 * py, (bx + 0.2) * px, H - 0.75 * py], fill=c)
        rect(d, 3.6, 0.0, 4.4, 2.6, (60, 40, 30))
        rect(d, 3.68, 1.4, 4.32, 2.5, (52, 70, 92))
        # The awning's valance (the awning itself is a model) and the sign.
        for k in range(10):
            rect(d, k * 0.7, 2.95, k * 0.7 + 0.7, 3.35, aw_a if k % 2 == 0 else aw_b)
        rect(d, 0.6, 3.45, 6.4, 3.85, sign_bg)
        font = ImageFont.truetype(font_path("DejaVuSans-Bold.ttf"), 40 * s)
        dd = ImageDraw.Draw(img)
        bb = dd.textbbox((0, 0), sign, font=font)
        layer = Image.new("RGB", (bb[2] - bb[0], bb[3] - bb[1]), sign_bg)
        ImageDraw.Draw(layer).text((-bb[0], -bb[1]), sign, font=font, fill=sign_fg)
        tw = min(5.4, 0.9 * len(sign))
        layer = layer.resize((int(tw * px), int(0.32 * py)), Image.Resampling.LANCZOS)
        img.paste(layer, (int((3.5 - tw / 2) * px), int(H - 3.81 * py)))
        # Two floors of flats: three sash windows each, a fire escape on the deli.
        for f in range(2):
            y0 = 4.5 + f * 3.0
            for k in range(3):
                window(d, 0.6 + k * 2.15, y0, 1.9 + k * 2.15, y0 + 2.0, trim, pediment=trim if style % 2 else None)
        if style == 1:
            for f in range(2):
                y = 4.3 + f * 3.0
                rect(d, 0.3, y, 4.6, y + 0.08, (30, 30, 32))
                for k in range(8):
                    rect(d, 0.3 + k * 0.6, y, 0.34 + k * 0.6, y + 0.9, (30, 30, 32))
                rect(d, 0.3, y + 0.86, 4.6, y + 0.92, (30, 30, 32))
        rect(d, 0.0, 10.4, 7.0, 10.8, trim)
        rect(d, 0.0, 10.8, 7.0, 11.7, body)
        rect(d, 0.0, 11.7, 7.0, 12.0, trim)
        return img
    if style == 4:
        # Edwardian flats: flat-fronted, grey-blue, white trim, a garage and
        # an entry, two floors of paired sash windows under pediments.
        body, trim = (176, 188, 202), (246, 246, 242)
        img = Image.new("RGB", (W, H), body)
        d = ImageDraw.Draw(img)
        rect(d, 0.4, 0.0, 3.3, 2.5, trim)
        rect(d, 0.55, 0.0, 3.15, 2.35, (210, 212, 214))
        for k in range(4):
            rect(d, 0.55, 0.15 + k * 0.58, 3.15, 0.19 + k * 0.58, (170, 172, 176))
        rect(d, 4.4, 0.0, 6.4, 3.0, trim)
        rect(d, 4.75, 0.0, 6.05, 2.7, (36, 56, 90))
        for f in range(3):
            y0 = 3.4 + f * 2.35
            for k in range(3):
                window(d, 0.55 + k * 2.15, y0, 1.95 + k * 2.15, y0 + 1.75, trim, pediment=trim if f == 0 else None)
        rect(d, 0.0, 10.6, 7.0, 11.0, trim)
        for k in range(10):
            rect(d, 0.2 + k * 0.7, 10.3, 0.4 + k * 0.7, 10.6, trim)
        rect(d, 0.0, 11.0, 7.0, 12.0, body)
        rect(d, 0.0, 11.8, 7.0, 12.0, trim)
        return img
    if style == 5:
        # Art deco flats: white stucco, fluted pilasters, a stepped parapet,
        # ribbon windows and an entrance under a little canopy.
        body = (236, 234, 226)
        img = Image.new("RGB", (W, H), (200, 196, 186))   # the recessed wall behind the stepped top
        d = ImageDraw.Draw(img)
        d.rectangle([0, H - 10.8 * py, W, H], fill=body)
        d.rectangle([1.5 * px, H - 11.5 * py, 5.5 * px, H], fill=body)
        d.rectangle([2.7 * px, H - 12.0 * py, 4.3 * px, H], fill=body)
        for x in (0.2, 3.3, 6.4):
            rect(d, x, 0.0, x + 0.4, 11.0 if x == 3.3 else 10.8, (214, 210, 200))
            rect(d, x + 0.18, 0.0, x + 0.22, 10.8, (190, 186, 176))
        for f in range(4):
            y0 = 2.0 + f * 2.2
            rect(d, 0.8, y0, 3.1, y0 + 1.4, (56, 72, 92))
            rect(d, 3.9, y0, 6.2, y0 + 1.4, (56, 72, 92))
            for x in (0.8, 3.9):
                d.polygon([(x * px, H - (y0 + 1.4) * py), ((x + 1.0) * px, H - (y0 + 1.4) * py), (x * px, H - (y0 + 0.6) * py)],
                          fill=(140, 170, 196))
                for k in range(1, 4):
                    rect(d, x + k * 0.58, y0, x + k * 0.58 + 0.05, y0 + 1.4, (236, 234, 226))
        rect(d, 2.9, 0.0, 4.1, 1.7, (40, 44, 52))
        rect(d, 2.5, 1.7, 4.5, 1.9, (120, 160, 150))
        return img
    if style == 6:
        # A brick loft: red brick in courses, tall arched windows below,
        # steel-framed ones above, a painted sign band.
        img = Image.new("RGB", (W, H), (150, 70, 52))
        d = ImageDraw.Draw(img)
        for k in range(int(12 / 0.25)):
            y = H - k * 0.25 * py
            d.line([0, y, W, y], fill=(118, 54, 42), width=max(1, s // 2))
            off = (k % 2) * 0.3
            for j in range(13):
                x = (off + j * 0.6) * px
                d.line([x, y, x, y - 0.25 * py], fill=(118, 54, 42), width=max(1, s // 2))
        for x0 in (0.4, 2.55, 4.7):
            d.rectangle([x0 * px, H - 2.6 * py, (x0 + 1.9) * px, H - 0.3 * py], fill=(36, 40, 44))
            d.ellipse([x0 * px, H - 3.55 * py, (x0 + 1.9) * px, H - 1.65 * py], fill=(36, 40, 44))
            for k in range(1, 3):
                rect(d, x0 + k * 0.63, 0.3, x0 + k * 0.63 + 0.05, 3.3, (90, 90, 96))
        rect(d, 0.0, 3.7, 7.0, 4.3, (226, 214, 180))
        for f in range(2):
            y0 = 4.8 + f * 2.8
            for x0 in (0.5, 2.6, 4.7):
                rect(d, x0, y0, x0 + 1.8, y0 + 2.0, (44, 50, 58))
                rect(d, x0, y0 + 0.95, x0 + 1.8, y0 + 1.05, (90, 90, 96))
                rect(d, x0 + 0.88, y0, x0 + 0.92, y0 + 2.0, (90, 90, 96))
                rect(d, x0 - 0.1, y0 - 0.15, x0 + 1.9, y0, (200, 190, 170))
        rect(d, 0.0, 11.2, 7.0, 12.0, (130, 60, 46))
        rect(d, 0.0, 11.1, 7.0, 11.25, (200, 190, 170))
        return img
    # Mission revival: tan stucco, an arched entry, iron balconies and a
    # red tile eave along the top.
    body = (222, 196, 156)
    img = Image.new("RGB", (W, H), body)
    d = ImageDraw.Draw(img)
    d.rectangle([2.5 * px, H - 2.4 * py, 4.5 * px, H], fill=(70, 46, 32))
    d.ellipse([2.5 * px, H - 3.4 * py, 4.5 * px, H - 1.4 * py], fill=(70, 46, 32))
    for x0 in (0.5, 5.2):
        window(d, x0, 0.8, x0 + 1.3, 2.6, (150, 110, 80))
    for f in range(2):
        y0 = 4.0 + f * 3.0
        for x0 in (0.6, 2.8, 5.0):
            d.rectangle([x0 * px, H - (y0 + 1.6) * py, (x0 + 1.4) * px, H - y0 * py], fill=(52, 66, 84))
            d.ellipse([x0 * px, H - (y0 + 2.3) * py, (x0 + 1.4) * px, H - (y0 + 0.9) * py], fill=(52, 66, 84))
            rect(d, x0 - 0.2, y0 - 0.1, x0 + 1.6, y0 + 0.05, (30, 30, 30))
            for k in range(6):
                rect(d, x0 - 0.15 + k * 0.33, y0, x0 - 0.12 + k * 0.33, y0 + 0.7, (30, 30, 30))
            rect(d, x0 - 0.2, y0 + 0.7, x0 + 1.6, y0 + 0.78, (30, 30, 30))
    for k in range(18):
        x = k * 0.4
        d.ellipse([x * px, H - 11.4 * py, (x + 0.42) * px, H - 10.9 * py], fill=(180, 80, 50))
    rect(d, 0.0, 11.15, 7.0, 12.0, (170, 72, 46))
    rect(d, 0.0, 10.7, 7.0, 10.9, (160, 130, 96))
    return img


def tex_sffacade2():
    """North Beach and Telegraph Hill: a cafe, a deli, a bakery and a
    bookshop with flats over them, Edwardian flats, art deco flats, a
    brick loft and a mission revival block, four across and two down
    (64 x 128 each), as tex_sffacade lays out the Victorians."""
    s = 4
    img = Image.new("RGB", (256 * s, 256 * s))
    for i in range(8):
        img.paste(sf_facade2_cell(i, s), ((i % 4) * 64 * s, (i // 4) * 128 * s))
    return img.resize((256, 256), Image.Resampling.LANCZOS)


def build(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    previews = out / "previews"
    previews.mkdir(exist_ok=True)
    font_img, glyphs, cap = tex_font()
    logo_img, logo_w = tex_logo()
    items: list[tuple[TexSpec, Image.Image]] = [
        (TexSpec("road", "RGB565", True, True), tex_road()),
        (TexSpec("boardwalk", "RGB565", True, True), tex_boardwalk()),
        (TexSpec("curb", "RGB565", True, True), tex_curb()),
        (TexSpec("sand", "RGB565", True, True), tex_sand()),
        (TexSpec("grass", "RGB565", True, True), tex_grass()),
        (TexSpec("barrier", "RGB565", True, True), tex_barrier()),
        (TexSpec("wood", "RGB565", True, True), tex_wood()),
        (TexSpec("water", "RGB565", True, True), tex_water()),
        (TexSpec("foam", "ARGB4444", True, True), tex_foam()),
        (TexSpec("rock", "RGB565", True, True), tex_rock()),
        (TexSpec("bark", "RGB565", True, True), tex_bark()),
        (TexSpec("frond", "ARGB4444", True, False), tex_frond()),
        (TexSpec("palm_far", "ARGB4444", True, False), tex_palm_far()),
        (TexSpec("cloud", "ARGB4444", False, False), tex_cloud()),
        (TexSpec("mountains", "ARGB4444", False, True), tex_mountains()),
        (TexSpec("glow", "ARGB4444", True, False), tex_glow()),
        (TexSpec("smoke", "ARGB4444", True, False), tex_smoke()),
        (TexSpec("shadow", "ARGB4444", True, False), tex_shadow()),
        (TexSpec("star", "ARGB4444", True, False), tex_star()),
        (TexSpec("flare", "ARGB4444", True, False), tex_flare()),
        (TexSpec("flame", "ARGB4444", True, False), tex_flame()),
        (TexSpec("dash", "RGB565", True, True), tex_dash()),
        (TexSpec("checker", "RGB565", True, True), tex_checker()),
        (TexSpec("thatch", "RGB565", True, True), tex_thatch()),
        (TexSpec("crowd", "RGB565", True, True), tex_crowd()),
        (TexSpec("banner", "RGB565", False, False), tex_banner()),
        (TexSpec("font", "ARGB4444", False, False), font_img),
        (TexSpec("logo", "ARGB4444", False, False), logo_img),
        (TexSpec("bigtext", "ARGB4444", False, False), tex_bigtext()),
        (TexSpec("stone", "RGB565", True, True), tex_stone()),
        *[(TexSpec(f"skyroad_{i}", "RGB565", True, True), tex_skyroad(i)) for i in range(SKYROAD_PHASES)],
        (TexSpec("nightsky", "RGB565", False, True), tex_nightsky()),
        (TexSpec("skyline", "ARGB4444", False, True), tex_skyline()),
        (TexSpec("moon", "ARGB4444", False, False), tex_moon()),
        (TexSpec("cloudsea", "RGB565", True, True), tex_cloudsea()),
        (TexSpec("tower", "RGB565", True, True), tex_tower()),
        (TexSpec("signs", "RGB565", True, False), tex_signs()),
        (TexSpec("canyonroad", "RGB565", True, True), tex_canyonroad()),
        (TexSpec("meadow", "RGB565", True, True), tex_meadow()),
        (TexSpec("islandrock", "RGB565", True, True), tex_islandrock()),
        (TexSpec("ehchecker", "RGB565", True, True), tex_checker_rock((112, 62, 54), (206, 146, 92), 5113)),
        (TexSpec("sunflower", "ARGB4444", True, False), tex_sunflower()),
        (TexSpec("totem", "RGB565", False, False), tex_totem()),
        (TexSpec("bush", "RGB565", True, True), tex_bush()),
        (TexSpec("waterfall", "ARGB4444", True, True), tex_waterfall()),
        (TexSpec("retrosun", "ARGB4444", True, False), tex_retrosun()),
        (TexSpec("tkroad", "RGB565", True, True), tex_tkroad()),
        (TexSpec("tkcurb", "RGB565", True, True), tex_tkcurb()),
        (TexSpec("verge", "RGB565", True, True), tex_verge()),
        (TexSpec("paddy", "RGB565", True, True), tex_paddy()),
        (TexSpec("hoarding", "RGB565", True, True), tex_hoarding()),
        (TexSpec("ishigaki", "RGB565", True, True), tex_ishigaki()),
        (TexSpec("kawara", "RGB565", True, True), tex_kawara()),
        (TexSpec("sakura", "ARGB4444", True, False), tex_sakura()),
        (TexSpec("sakurafar", "ARGB4444", True, False), tex_sakurafar()),
        (TexSpec("sakurabark", "RGB565", True, True), tex_sakurabark()),
        (TexSpec("petal", "ARGB4444", False, False), tex_petal()),
        (TexSpec("tkcrowd", "RGB565", True, True), tex_tkcrowd()),
        (TexSpec("plaster", "RGB565", True, True), tex_plaster()),
        (TexSpec("tkbanner", "RGB565", False, False), tex_tkbanner()),
        (TexSpec("pitwall", "RGB565", True, True), tex_pitwall()),
        (TexSpec("sundisc", "ARGB4444", True, False), tex_sundisc()),
        (TexSpec("stream", "RGB565", True, True), tex_stream()),
        (TexSpec("avroad", "RGB565", True, True), tex_avroad()),
        (TexSpec("avcurb", "RGB565", True, True), tex_avcurb()),
        (TexSpec("avdirt", "RGB565", True, True), tex_avdirt()),
        (TexSpec("avdesert", "RGB565", True, True), tex_avdesert()),
        (TexSpec("avtyres", "RGB565", True, True), tex_avtyres()),
        (TexSpec("avmetal", "RGB565", True, True), tex_avmetal()),
        (TexSpec("avcrowd", "RGB565", True, True), tex_avcrowd()),
        (TexSpec("avjoshua", "ARGB4444", True, False), tex_avjoshua()),
        (TexSpec("avjbark", "RGB565", True, True), tex_avjbark()),
        (TexSpec("avscrub", "ARGB4444", True, False), tex_avscrub()),
        (TexSpec("avbanner", "RGB565", False, False), tex_avbanner()),
        (TexSpec("avstucco", "RGB565", True, True), tex_avstucco()),
        (TexSpec("avrock", "RGB565", True, True), tex_avrock()),
        (TexSpec("avfar", "ARGB4444", True, False), tex_avfar()),
        (TexSpec("avtumble", "ARGB4444", True, False), tex_avtumble()),
        (TexSpec("avdust", "ARGB4444", True, False), tex_avdust()),
        (TexSpec("avmtn", "RGB565", True, True), tex_avmtn()),
        (TexSpec("avrange", "RGB565", True, True), tex_avrange()),
        (TexSpec("sfroad", "RGB565", True, True), tex_sfroad()),
        (TexSpec("sfrails", "RGB565", True, True), tex_sfrails()),
        (TexSpec("sfbrick", "RGB565", True, True), tex_sfbrick()),
        (TexSpec("sfcurb", "RGB565", True, True), tex_sfcurb()),
        (TexSpec("sfwalk", "RGB565", True, True), tex_sfwalk()),
        (TexSpec("sfrailing", "RGB565", True, True), tex_sfrailing()),
        (TexSpec("sfpier", "RGB565", True, True), tex_sfpier()),
        (TexSpec("sfpierrail", "RGB565", True, True), tex_sfpierrail()),
        (TexSpec("sffacade", "RGB565", True, True), tex_sffacade()),
        (TexSpec("sfbldg", "RGB565", True, True), tex_sfbldg()),
        (TexSpec("sfroof", "RGB565", True, True), tex_sfroof()),
        (TexSpec("sfhedge", "RGB565", True, True), tex_sfhedge()),
        (TexSpec("sfshed", "RGB565", True, True), tex_sfshed()),
        (TexSpec("sfsign", "RGB565", False, False), tex_sfsign()),
        (TexSpec("sfbay", "RGB565", True, True), tex_sfbay()),
        (TexSpec("sffog", "ARGB4444", True, False), tex_sffog()),
        (TexSpec("sfpalm", "ARGB4444", True, False), tex_sfpalm()),
        (TexSpec("sfpalmbark", "RGB565", True, True), tex_sfpalmbark()),
        (TexSpec("sfcablecar", "RGB565", True, False), tex_sfcablecar()),
        (TexSpec("sffacade2", "RGB565", True, True), tex_sffacade2()),
        (TexSpec("sfstreet", "RGB565", True, True), tex_sfstreet()),
        (TexSpec("sfgrass", "RGB565", True, True), tex_sfgrass()),
        (TexSpec("sfstall", "RGB565", True, True), tex_sfstall()),
        (TexSpec("sfsigns", "RGB565", True, False), tex_sfsigns()),
    ]
    table, size = write_texture_pak(out, previews, items)
    write_texture_header(out, "DK", table, tail=[f"#define DK_TEXTURES_BYTES {size}u", ""])
    write_font_metrics(
        out, "DK", font_img, glyphs, cap, FONT_ROW_H, glyph_type="dk_glyph_t",
        cap_comment=["/* The capitals' rows within a glyph row (top edge, bottom edge). */"],
        glyph_comment=["/* ASCII 32..127 at native size: atlas x, row, rect width, rect left",
                       "   relative to the pen, and pen advance. */"],
        tail=["/* Countdown cells (3, 2, 1, GO!) in the bigtext texture's top row: x, width. */",
              "static const uint16_t bigtext_cells[4][2] = {" +
              ", ".join(f"{{{x}, {w}}}" for x, w in BIGTEXT_CELLS) + "};", "",
              f'#define LOGO_LETTERS "{LOGO_LETTERS}"', f"#define LOGO_CELL {LOGO_CELL}",
              "static const uint8_t logo_advance[8] = {" + ", ".join(str(w) for w in logo_w) + "};",
              ""])


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, required=True)
    args = p.parse_args()
    build(args.output_dir.resolve())


if __name__ == "__main__":
    main()
