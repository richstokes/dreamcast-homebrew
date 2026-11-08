#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "pillow>=10.1", "scipy>=1.11"]
# ///
"""Paint Ridgeline's textures and pack them for the PowerVR.

The look is flat colour fields with soft painted shading, so most surfaces
are untextured vertex colour; the few textures here are near-white detail
(brush strokes, rock facets, bark) that the vertex colours tint, sprites
(sun, glows, flare rings, dust, clouds, grass tufts), the HUD's art and the
font. Everything is drawn from code with seeded noise. Writes to
--output-dir:

    textures.pak        twiddled RGB565/ARGB4444 textures (+mips), 32-byte aligned
    texture_assets.h    TEX_* ids and the size/format/offset table
    font_metrics.h      the HUD font's glyph table (engine/text.h)
    previews/*.png      every texture as painted (git-ignored)
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy.ndimage import gaussian_filter

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
from assetpak import TexSpec, to_image, write_font_metrics, write_texture_header, write_texture_pak  # noqa: E402
from fontatlas import LILITA, atlas, font_for_cap  # noqa: E402

GEN = Path(__file__).resolve().parent.parent / "assets" / "generated"
FONT_ROW_H = 42
FONT_CAP_H = 22
FONT_BASELINE = 30


def rng(seed):
    return np.random.default_rng(seed)


def tile_noise(n, period, seed):
    r = rng(seed)
    g = r.random((period, period))
    g = np.tile(g, (3, 3))
    up = np.array(Image.fromarray((g * 255).astype(np.uint8)).resize((n * 3, n * 3), Image.BICUBIC), dtype=float) / 255
    return up[n:2 * n, n:2 * n]


# ---------------------------------------------------------------------------
# Detail textures (near white, tinted by vertex colour)

def strokes(n, seed, count, length, width, angle, spread, lo, hi, wrap=True):
    """Short painted brush strokes over a mid field: a value map in 0..1."""
    r = rng(seed)
    img = Image.new("L", (n * 3, n * 3), int((lo + hi) * 0.5 * 255))
    d = ImageDraw.Draw(img)
    for _ in range(count):
        cx, cy = r.uniform(0, n), r.uniform(0, n)
        a = angle + r.normal(0, spread)
        L = length * r.uniform(0.6, 1.3)
        w = max(1, int(width * r.uniform(0.6, 1.4)))
        v = int(r.uniform(lo, hi) * 255)
        dx, dy = math.cos(a) * L / 2, math.sin(a) * L / 2
        for ox in (0, n, 2 * n) if wrap else (n,):
            for oy in (0, n, 2 * n) if wrap else (n,):
                d.line([(cx + ox - dx, cy + oy - dy), (cx + ox + dx, cy + oy + dy)], fill=v, width=w)
    img = img.filter(ImageFilter.GaussianBlur(0.7))
    a = np.array(img, dtype=float)[n:2 * n, n:2 * n] / 255
    return a


def tex_grass():
    """Painterly ground: brush strokes leaning one way, light so the baked
    vertex colours carry the palette."""
    n = 256
    v = strokes(n, 11, 2600, 16, 3, -1.2, 0.25, 0.80, 1.0)
    v2 = strokes(n, 12, 900, 26, 5, -1.0, 0.3, 0.84, 1.0)
    v = v * 0.6 + v2 * 0.4
    big = tile_noise(n, 4, 13)
    v *= 0.94 + 0.08 * big
    rgb = np.dstack([v * 255, v * 252, v * 240])
    return to_image(rgb)


def tex_rock():
    """Faceted stone: flat cells of slightly different value, crisp edges."""
    n = 128
    r = rng(21)
    pts = r.random((40, 2)) * n
    vals = r.uniform(0.80, 1.0, 40)
    yy, xx = np.mgrid[0:n, 0:n]
    best = np.full((n, n), 1e9)
    lab = np.zeros((n, n), dtype=int)
    for k, (px, py) in enumerate(pts):
        for ox in (-n, 0, n):
            for oy in (-n, 0, n):
                d = (xx - px - ox) ** 2 * 0.6 + (yy - py - oy) ** 2
                m = d < best
                best[m] = d[m]
                lab[m] = k
    v = vals[lab]
    edge = np.zeros_like(v)
    edge[:, 1:] += lab[:, 1:] != lab[:, :-1]
    edge[1:, :] += lab[1:, :] != lab[:-1, :]
    v = v - 0.08 * np.clip(edge, 0, 1)
    v = gaussian_filter(v, 0.6, mode="wrap")
    rgb = np.dstack([v * 250, v * 252, v * 255])
    return to_image(rgb)


def tex_bark():
    n = 64
    v = strokes(n, 31, 300, 30, 2, math.pi / 2, 0.05, 0.72, 1.0)
    rgb = np.dstack([v * 255, v * 245, v * 235])
    return to_image(rgb.repeat(2, axis=0)[:128] if False else rgb)


# ---------------------------------------------------------------------------
# Sprites

def radial(n):
    yy, xx = np.mgrid[0:n, 0:n]
    c = (n - 1) / 2
    return np.hypot(xx - c, yy - c) / (n / 2)


def tex_sun():
    """A big flat disc, soft just at its rim."""
    n = 64
    r = radial(n)
    a = np.clip((0.92 - r) / 0.06, 0, 1)
    rgb = np.dstack([np.full((n, n), 255.0), np.full((n, n), 250.0), np.full((n, n), 232.0)])
    return to_image(rgb, a * 255)


def tex_moon():
    """A full moon: a flat disc, soft at the rim, with faint grey maria."""
    n = 64
    r = radial(n)
    a = np.clip((0.9 - r) / 0.05, 0, 1)
    maria = tile_noise(n, 10, 32) * 0.6 + tile_noise(n, 5, 33) * 0.4
    shade = 1.0 - 0.22 * np.clip((maria - 0.45) / 0.3, 0, 1) - 0.08 * r * r
    rgb = np.dstack([232 * shade, 236 * shade, 244 * shade])
    return to_image(rgb, a * 255)


def tex_stars():
    """A patch of night sky: some forty stars, a few bright, soft-edged, on
    nothing (drawn additively); a dozen of these cover the sky."""
    n = 256
    r = rng(41)
    a = np.zeros((n, n))
    yy, xx = np.mgrid[0:n, 0:n]
    for _ in range(70):
        cx, cy = r.uniform(10, n - 10, 2)
        b = r.random() ** 3
        rad = 0.55 + 0.75 * b
        a += (0.4 + 0.6 * b) * np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * rad * rad))
    a = np.clip(a, 0, 1)
    rgb = np.dstack([np.full((n, n), 232.0), np.full((n, n), 238.0), np.full((n, n), 255.0)])
    return to_image(rgb, a * 255)


def tex_glow():
    n = 64
    r = radial(n)
    a = np.clip(1 - r, 0, 1) ** 2.2
    rgb = np.full((n, n, 3), 255.0)
    return to_image(rgb, a * 255)


def tex_ring():
    """Lens flare elements: a soft ring (left half) and a hexagon (right)."""
    n = 64
    r = radial(n)
    ring = np.exp(-((r - 0.78) / 0.07) ** 2) * 0.9 + np.clip(0.75 - r, 0, 1) * 0.15
    yy, xx = np.mgrid[0:n, 0:n]
    c = (n - 1) / 2
    ang = np.arctan2(yy - c, xx - c)
    hexr = np.cos(math.pi / 6) / np.cos(((ang + math.pi / 6) % (math.pi / 3)) - math.pi / 6)
    hexa = np.clip((hexr * 0.85 - r) / 0.05, 0, 1) * 0.55
    a = np.concatenate([ring, hexa], axis=1)
    rgb = np.full((n, 2 * n, 3), 255.0)
    return to_image(rgb, a * 255)


def tex_puff():
    """A soft painted puff of dust or spray."""
    n = 64
    r = radial(n)
    nz = tile_noise(n, 6, 41)
    a = np.clip(1 - r * (0.85 + 0.35 * nz), 0, 1) ** 1.4
    rgb = np.full((n, n, 3), 255.0)
    return to_image(rgb, a * 255)


def tex_clod():
    """A clod of mud, a stone or a lump of snow thrown off a spinning tyre:
    a lumpy hard-edged blob lit from above, white so the vertex colour
    tints it by surface."""
    n = 32
    yy, xx = np.mgrid[0:n, 0:n]
    c = (n - 1) / 2
    ang = np.arctan2(yy - c, xx - c)
    rr = np.hypot(xx - c, yy - c) / (n / 2)
    edge = 0.78 + 0.10 * np.sin(3 * ang + 0.7) + 0.06 * np.sin(5 * ang + 2.1) + 0.04 * np.sin(8 * ang)
    a = np.clip((edge - rr) * 12, 0, 1)
    nz = tile_noise(n, 8, 77)
    light = np.clip(0.78 - 0.32 * (yy - c) / (n / 2) - 0.12 * (xx - c) / (n / 2) + 0.18 * (nz - 0.5), 0.35, 1.0)
    rgb = np.repeat((light * 255)[..., None], 3, axis=2)
    return to_image(rgb, a * 255)


def tex_tread():
    """A tyre track, tiling along v: chevron lug prints, darker where the
    lugs pressed, fading out at both edges. White, tinted by surface."""
    w = h = 64    # square, for mipmaps
    yy, xx = np.mgrid[0:h, 0:w]
    u = (xx + 0.5) / w
    v = (yy + 0.5) / h
    chev = (v * 4 + np.abs(u - 0.5) * 1.3) % 1.0
    lug = np.clip((np.abs(chev - 0.5) - 0.18) * 9, 0, 1)
    side = np.clip(np.minimum(u, 1 - u) / 0.16, 0, 1) ** 1.5
    nz = tile_noise(w, 4, 91)
    a = side * (0.55 + 0.4 * lug) * (0.85 + 0.3 * nz)
    rgb = np.repeat((150 + 105 * (1 - lug))[..., None], 3, axis=2)
    return to_image(rgb, np.clip(a, 0, 1) * 255)


def tex_shadow():
    n = 64
    r = radial(n)
    a = np.clip((1 - r) / 0.45, 0, 1) ** 1.3
    rgb = np.zeros((n, n, 3))
    return to_image(rgb, a * 255)


def tex_cloud():
    """Four flat-bottomed painted cumulus, 128 x 64 each side by side:
    a row of puffs along the base, heaps piled on them toward the middle.
    Plain white: the game's vertex colours light the tops and shade the
    bases, and a gradient here would only pick up the packer's dither."""
    w, h = 128, 64
    r = rng(51)
    alphas = []
    for k in range(4):
        img = Image.new("RGBA", (w, h), (255, 255, 255, 0))
        d = ImageDraw.Draw(img)
        span = r.uniform(38, 50)
        mid = 64 + r.uniform(-6, 6)
        # The base row, overlapping, sitting on the flat bottom.
        n = r.integers(5, 7)
        for i in range(n):
            t = (i + 0.5) / n
            cx = mid + (t - 0.5) * 2 * span
            rr = r.uniform(10, 14)
            cy = 47 - rr * 0.8
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(255, 255, 255, 255))
        # Heaps: bigger puffs stacked up toward one side of the middle.
        peak_x = mid + r.uniform(-span * 0.35, span * 0.35)
        for j in range(r.integers(3, 5)):
            cx = peak_x + r.uniform(-span * 0.5, span * 0.5)
            dist = abs(cx - peak_x) / (span * 0.5)
            rr = r.uniform(13, 20) * (1 - 0.4 * dist)
            cy = 47 - rr * r.uniform(1.0, 1.35)
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(255, 255, 255, 255))
        top = r.uniform(8, 12)
        d.ellipse([peak_x - top, 47 - 34 - top + r.uniform(0, 6), peak_x + top, 47 - 34 + top + r.uniform(0, 6)],
                  fill=(255, 255, 255, 255))
        a = np.array(img)[..., 3].astype(float)
        a[48:, :] = 0
        alphas.append(gaussian_filter(a, 1.1))
    a = np.concatenate(alphas, axis=1)
    rgb = np.full((h, 4 * w, 3), 255.0)
    return to_image(rgb, a)


TUFT_SS = 4   # drawn at 4x and box-filtered down, for smooth alpha edges


def _blade(d, hgt, x0, ang, w, rgb, n):
    """One brush stroke of grass, fanning out from the root at x0; rgb is
    its colour at the root, lightening toward the tip."""
    S = TUFT_SS
    pts = []
    for t in np.linspace(0, 1, 13):
        bend = ang * t * t
        pts.append((x0 + math.sin(bend) * hgt * t, n - 1 - math.cos(bend) * hgt * t, w * (1 - 0.85 * t)))
    for (xa, ya, wa), (xb, yb, wb) in zip(pts[:-1], pts[1:]):
        k = 0.62 + 0.38 * (n - ya) / n
        col = tuple(int(min(255, c * k)) for c in rgb) + (255,)
        ww = max(1.0, (wa + wb) / 2) * S
        d.line([(xa * S, ya * S), (xb * S, yb * S)], fill=col, width=max(1, int(ww)))
        d.ellipse([xb * S - ww / 2, yb * S - ww / 2, xb * S + ww / 2, yb * S + ww / 2], fill=col)
    return pts


def _tuft_canvas(n):
    img = Image.new("RGBA", (n * TUFT_SS, n * TUFT_SS), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def _tuft_finish(img, n):
    """Box down to n x n with premultiplied colour, so edge texels keep
    the blade's colour rather than fading to the black background."""
    a = np.asarray(img, dtype=float)
    al = a[..., 3:4] / 255.0
    pm = np.concatenate([a[..., :3] * al, al], axis=2)
    pm = pm.reshape(n, TUFT_SS, n, TUFT_SS, 4).mean(axis=(1, 3))
    rgb = pm[..., :3] / np.maximum(pm[..., 3:4], 1e-6)
    return to_image(rgb, pm[..., 3] * 255.0)


def tex_tuft(variant):
    """Clumps of tall grass, white so the vertex colour tints them, each
    a set of brush strokes fanning up from the root, light at the tips and
    darker toward the ground. Punch-through, so edges stay crisp; the mips
    keep their coverage at the alpha test so far tufts don't thin out.
      0: a dozen broad blades
      1: thin blades and drooping seed heads (drawn taller)
      2: a low, wide spray of short blades (drawn squatter)
      3: broad blades, some turned rust for the autumn"""
    n = 128
    img, d = _tuft_canvas(n)
    r = rng(61 + variant)
    strokes = []
    if variant == 0:
        for _ in range(15):
            tone = r.uniform(0.82, 1.0)
            strokes.append((r.uniform(0.55, 0.97) * n, n / 2 + r.normal(0, 13), r.normal(0, 0.42),
                            r.uniform(6, 10), (255 * tone, 255 * tone, 235 * tone), 0))
    elif variant == 1:
        for _ in range(11):
            tone = r.uniform(0.72, 0.9)
            strokes.append((r.uniform(0.45, 0.85) * n, n / 2 + r.normal(0, 11), r.normal(0, 0.4),
                            r.uniform(3.5, 6), (225 * tone, 255 * tone, 200 * tone), 0))
        for _ in range(6):
            strokes.append((r.uniform(0.82, 0.98) * n, n / 2 + r.normal(0, 9), r.normal(0, 0.55),
                            2.2, (255, 240, 205), 1))
    elif variant == 2:
        for _ in range(24):
            tone = r.uniform(0.8, 1.0)
            ang = float(np.clip(r.normal(0, 0.7), -1.05, 1.05))
            strokes.append((r.uniform(0.5, 0.95) * n, n / 2 + r.normal(0, 16) + ang * 6, ang,
                            r.uniform(4.5, 7.5), (255 * tone, 255 * tone, 235 * tone), 0))
    else:
        for k in range(14):
            tone = r.uniform(0.82, 1.0)
            rust = (255 * tone, 178 * tone, 120 * tone) if k % 2 else (255 * tone, 236 * tone, 200 * tone)
            strokes.append((r.uniform(0.5, 0.95) * n, n / 2 + r.normal(0, 13), r.normal(0, 0.5),
                            r.uniform(6, 10), rust, 0))
    for hgt, x0, ang, w, rgb, seed in sorted(strokes, key=lambda s: -s[0]):
        pts = _blade(d, hgt, x0, ang, w, rgb, n)
        if seed:
            # Oat-like spikelets hanging off the last third of the stem.
            S = TUFT_SS
            for i, (x, y, _) in enumerate(pts[7:]):
                side = -1 if i % 2 else 1
                for j in range(3):
                    sx, sy = x + side * (1.5 + j * 0.8), y + 2.5 + j * 3.2
                    d.polygon([(sx * S, (sy - 2.6) * S), ((sx + 1.5) * S, sy * S),
                               (sx * S, (sy + 2.6) * S), ((sx - 1.5) * S, sy * S)], fill=(255, 248, 225, 255))
    return _tuft_finish(img, n)


def tex_map():
    img = Image.open(GEN / "level_map.png").convert("RGB").resize((256, 256), Image.LANCZOS)
    return img


def tex_dial():
    """A rev counter face, 0 to 6000 over 270 degrees: a long tick every
    thousand, a short one between."""
    n = 128
    img = Image.new("RGBA", (n * 4, n * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = n * 2
    R = n * 2 - 10
    d.ellipse([c - R, c - R, c + R, c + R], fill=(20, 24, 30, 170))
    d.ellipse([c - R, c - R, c + R, c + R], outline=(255, 255, 255, 255), width=10)
    a0, a1 = math.radians(135), math.radians(405)
    for k in range(13):
        a = a0 + (a1 - a0) * k / 12
        L = 56 if k % 2 == 0 else 34
        p0 = (c + math.cos(a) * (R - 18), c + math.sin(a) * (R - 18))
        p1 = (c + math.cos(a) * (R - 18 - L), c + math.sin(a) * (R - 18 - L))
        d.line([p0, p1], fill=(255, 255, 255, 255), width=12 if k % 2 == 0 else 8)
    img = img.resize((n, n), Image.LANCZOS)
    return img


def tex_icons():
    """Inclinometer silhouettes: a truck from the side (left half) and from
    behind (right half), white."""
    w, h = 256, 128
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    W = (255, 255, 255, 255)
    # Side: cab and bed, two wheels.
    d.rounded_rectangle([14, 52, 118, 84], 6, fill=W)
    d.polygon([(38, 52), (46, 26), (80, 26), (86, 52)], fill=W)
    for cx in (34, 98):
        d.ellipse([cx - 15, 70, cx + 15, 100], fill=W)
    d.ellipse([34 - 7, 78, 34 + 7, 92], fill=(0, 0, 0, 0))
    d.ellipse([98 - 7, 78, 98 + 7, 92], fill=(0, 0, 0, 0))
    # Rear: body, roof, two wheels.
    d.rounded_rectangle([150, 50, 234, 86], 6, fill=W)
    d.polygon([(160, 50), (168, 24), (216, 24), (224, 50)], fill=W)
    d.rounded_rectangle([144, 76, 162, 104], 4, fill=W)
    d.rounded_rectangle([222, 76, 240, 104], 4, fill=W)
    img = img.filter(ImageFilter.GaussianBlur(0.6))
    return img


def tex_logo():
    """RIDGELINE in chunky poster letters: cream fill fading to mustard,
    a deep forest-green outline and a soft drop shadow."""
    w, h = 512, 128
    text = "RIDGELINE"
    mask = Image.new("L", (w, h), 0)
    md = ImageDraw.Draw(mask)
    for size in range(104, 60, -2):     # as big as fits inside the outline
        font = ImageFont.truetype(LILITA, size)
        bb = md.textbbox((0, 0), text, font=font)
        if bb[2] - bb[0] <= w - 30:
            break
    tx = (w - (bb[2] - bb[0])) / 2 - bb[0]
    ty = (h - (bb[3] - bb[1])) / 2 - bb[1] - 4
    md.text((tx, ty), text, font=font, fill=255)
    out = Image.new("L", (w, h), 0)
    ImageDraw.Draw(out).text((tx, ty), text, font=font, fill=255, stroke_width=7, stroke_fill=255)
    sh = out.filter(ImageFilter.GaussianBlur(4))
    layer = np.zeros((h, w, 4))
    s = np.array(sh, dtype=float)
    layer[..., 3] = np.clip(np.roll(np.roll(s, 6, 0), 4, 1) * 0.55, 0, 255)
    o = np.array(out, dtype=float) / 255
    green = np.array([32, 64, 44], dtype=float)
    layer[..., :3] = layer[..., :3] * (1 - o[..., None]) + green * o[..., None]
    layer[..., 3] = np.maximum(layer[..., 3], o * 255)
    m = np.array(mask, dtype=float) / 255
    yy = np.mgrid[0:h, 0:w][0]
    t = np.clip((yy - ty - 10) / 80.0, 0, 1)[..., None]
    fill = np.array([255, 246, 214]) * (1 - t) + np.array([236, 172, 52]) * t
    layer[..., :3] = layer[..., :3] * (1 - m[..., None]) + fill * m[..., None]
    return to_image(layer[..., :3], layer[..., 3])


def tex_font():
    return atlas(font_for_cap(LILITA, FONT_CAP_H), FONT_ROW_H, FONT_BASELINE, 2, outline_rgba=(20, 28, 24, 255))


# ---------------------------------------------------------------------------

def build(out: Path):
    out.mkdir(parents=True, exist_ok=True)
    previews = out / "previews"
    previews.mkdir(exist_ok=True)
    font_img, glyphs, cap = tex_font()
    items = [
        (TexSpec("grass", "RGB565", True, True), tex_grass()),
        (TexSpec("rock", "RGB565", True, True), tex_rock()),
        (TexSpec("bark", "RGB565", True, True), tex_bark()),
        *[(TexSpec(f"tuft{k}", "ARGB4444", True, False, alpha_ref=0x60 / 255), tex_tuft(k)) for k in range(4)],
        (TexSpec("sun", "ARGB4444", False, False), tex_sun()),
        (TexSpec("moon", "ARGB4444", False, False), tex_moon()),
        (TexSpec("stars", "ARGB4444", False, False), tex_stars()),
        (TexSpec("glow", "ARGB4444", True, False), tex_glow()),
        (TexSpec("ring", "ARGB4444", False, False), tex_ring()),
        (TexSpec("puff", "ARGB4444", True, False), tex_puff()),
        (TexSpec("shadow", "ARGB4444", True, False), tex_shadow()),
        (TexSpec("clod", "ARGB4444", True, False), tex_clod()),
        (TexSpec("tread", "ARGB4444", True, True), tex_tread()),
        (TexSpec("cloud", "ARGB4444", False, False), tex_cloud()),
        (TexSpec("map", "RGB565", True, False), tex_map()),
        (TexSpec("dial", "ARGB4444", False, False), tex_dial()),
        (TexSpec("icons", "ARGB4444", False, False), tex_icons()),
        (TexSpec("logo", "ARGB4444", False, False), tex_logo()),
        (TexSpec("font", "ARGB4444", False, False), font_img),
    ]
    table, _ = write_texture_pak(out, previews, items)
    write_texture_header(out, "RL", table)
    write_font_metrics(
        out, "RL", font_img, glyphs, cap, FONT_ROW_H,
        glyph_comment=["/* ASCII 32..127: atlas x, row, rect width, rect left from the pen, advance (eng_glyph_t). */"])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, default=GEN)
    build(p.parse_args().output_dir.resolve())


if __name__ == "__main__":
    main()
