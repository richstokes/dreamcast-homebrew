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
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy.ndimage import gaussian_filter

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
from fontatlas import LILITA, atlas, font_for_cap  # noqa: E402
from pvr_texture import pack_texture  # noqa: E402

GEN = Path(__file__).resolve().parent.parent / "assets" / "generated"
FONT_ROW_H = 42
FONT_CAP_H = 22
FONT_BASELINE = 30


@dataclass
class Spec:
    name: str
    fmt: str
    mip: bool
    wrap: bool


def rng(seed):
    return np.random.default_rng(seed)


def to_img(rgb, alpha=None):
    rgb = np.clip(rgb, 0, 255).astype(np.uint8)
    if alpha is None:
        return Image.fromarray(rgb, "RGB")
    return Image.fromarray(np.dstack([rgb, np.clip(alpha, 0, 255).astype(np.uint8)]), "RGBA")


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
    return to_img(rgb)


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
    return to_img(rgb)


def tex_bark():
    n = 64
    v = strokes(n, 31, 300, 30, 2, math.pi / 2, 0.05, 0.72, 1.0)
    rgb = np.dstack([v * 255, v * 245, v * 235])
    return to_img(rgb.repeat(2, axis=0)[:128] if False else rgb)


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
    return to_img(rgb, a * 255)


def tex_glow():
    n = 64
    r = radial(n)
    a = np.clip(1 - r, 0, 1) ** 2.2
    rgb = np.full((n, n, 3), 255.0)
    return to_img(rgb, a * 255)


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
    return to_img(rgb, a * 255)


def tex_puff():
    """A soft painted puff of dust or spray."""
    n = 64
    r = radial(n)
    nz = tile_noise(n, 6, 41)
    a = np.clip(1 - r * (0.85 + 0.35 * nz), 0, 1) ** 1.4
    rgb = np.full((n, n, 3), 255.0)
    return to_img(rgb, a * 255)


def tex_shadow():
    n = 64
    r = radial(n)
    a = np.clip((1 - r) / 0.45, 0, 1) ** 1.3
    rgb = np.zeros((n, n, 3))
    return to_img(rgb, a * 255)


def tex_cloud():
    """A flat-bottomed painted cumulus, two tones."""
    w, h = 128, 64
    img = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    d = ImageDraw.Draw(img)
    r = rng(51)
    blobs = [(20, 40, 16), (40, 30, 22), (64, 24, 26), (88, 32, 20), (106, 40, 14), (52, 42, 16), (76, 42, 16)]
    for (cx, cy, rr) in blobs:
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(255, 255, 255, 255))
    a = np.array(img)[..., 3].astype(float)
    a[50:, :] = 0
    a = gaussian_filter(a, 1.2)
    yy = np.mgrid[0:h, 0:w][0]
    shade = 255 - np.clip((yy - 26) * 2.2, 0, 60)
    rgb = np.dstack([shade, shade, shade + 4])
    return to_img(rgb, a)


def tex_tuft():
    """A clump of tall grass as a dozen broad brush strokes fanning up from
    its root: light tips, darker toward the ground, white so the vertex
    colour tints it; punch-through alpha, so the edges are crisp."""
    n = 128
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = rng(61)
    strokes_ = []
    for k in range(15):
        x0 = n / 2 + r.normal(0, 13)
        ang = r.normal(0, 0.42)
        hgt = r.uniform(0.55, 0.97) * n
        strokes_.append((hgt, x0, ang))
    for hgt, x0, ang in sorted(strokes_, reverse=True):
        w = r.uniform(6, 10)
        pts = []
        for t in np.linspace(0, 1, 9):
            bend = ang * t * t
            pts.append((x0 + math.sin(bend) * hgt * t, n - 1 - math.cos(bend) * hgt * t, w * (1 - 0.85 * t)))
        tone = r.uniform(0.82, 1.0)
        for (xa, ya, wa), (xb, yb, wb) in zip(pts[:-1], pts[1:]):
            v = int(255 * tone * (0.62 + 0.38 * (n - ya) / n))
            d.line([(xa, ya), (xb, yb)], fill=(v, v, int(v * 0.92), 255), width=max(1, int((wa + wb) / 2)))
    return img


def tex_map():
    img = Image.open(GEN / "level_map.png").convert("RGB").resize((256, 256), Image.LANCZOS)
    return img


def tex_dial():
    """A gauge face: ring, ticks every tenth, longer every half."""
    n = 128
    img = Image.new("RGBA", (n * 4, n * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = n * 2
    R = n * 2 - 10
    d.ellipse([c - R, c - R, c + R, c + R], fill=(20, 24, 30, 170))
    d.ellipse([c - R, c - R, c + R, c + R], outline=(255, 255, 255, 255), width=10)
    a0, a1 = math.radians(135), math.radians(405)
    for k in range(11):
        a = a0 + (a1 - a0) * k / 10
        L = 56 if k % 5 == 0 else 34
        p0 = (c + math.cos(a) * (R - 18), c + math.sin(a) * (R - 18))
        p1 = (c + math.cos(a) * (R - 18 - L), c + math.sin(a) * (R - 18 - L))
        d.line([p0, p1], fill=(255, 255, 255, 255), width=12 if k % 5 == 0 else 8)
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
    return to_img(layer[..., :3], layer[..., 3])


def tex_font():
    return atlas(font_for_cap(LILITA, FONT_CAP_H), FONT_ROW_H, FONT_BASELINE, 2, outline_rgba=(20, 28, 24, 255))


# ---------------------------------------------------------------------------

def build(out: Path):
    out.mkdir(parents=True, exist_ok=True)
    previews = out / "previews"
    previews.mkdir(exist_ok=True)
    font_img, glyphs, cap = tex_font()
    items = [
        (Spec("grass", "RGB565", True, True), tex_grass()),
        (Spec("rock", "RGB565", True, True), tex_rock()),
        (Spec("bark", "RGB565", True, True), tex_bark()),
        (Spec("tuft", "ARGB4444", True, False), tex_tuft()),
        (Spec("sun", "ARGB4444", False, False), tex_sun()),
        (Spec("glow", "ARGB4444", True, False), tex_glow()),
        (Spec("ring", "ARGB4444", False, False), tex_ring()),
        (Spec("puff", "ARGB4444", True, False), tex_puff()),
        (Spec("shadow", "ARGB4444", True, False), tex_shadow()),
        (Spec("cloud", "ARGB4444", False, False), tex_cloud()),
        (Spec("map", "RGB565", True, False), tex_map()),
        (Spec("dial", "ARGB4444", False, False), tex_dial()),
        (Spec("icons", "ARGB4444", False, False), tex_icons()),
        (Spec("logo", "ARGB4444", False, False), tex_logo()),
        (Spec("font", "ARGB4444", False, False), font_img),
    ]
    pak = bytearray()
    table = []
    for spec, img in items:
        img.save(previews / f"tex-{spec.name}.png")
        words = pack_texture(img, spec.fmt, mipmap=spec.mip, wrap=spec.wrap,
                             mip_filter="lanczos" if spec.fmt == "RGB565" else "box")
        data = np.array(words, dtype="<u2").tobytes()
        while len(pak) % 32:
            pak.append(0)
        table.append((spec, img.width, img.height, len(pak), len(data)))
        pak += data
        print(f"  {spec.name:8s} {img.width}x{img.height} {spec.fmt}{' mip' if spec.mip else ''} {len(data)} bytes")
    (out / "textures.pak").write_bytes(bytes(pak))
    h = ["/* Generated by tools/build_textures.py. Do not edit. */",
         "#ifndef RL_TEXTURE_ASSETS_H", "#define RL_TEXTURE_ASSETS_H", "#include <stdint.h>", "",
         "#define RL_TEXFMT_RGB565 0", "#define RL_TEXFMT_ARGB4444 1", "",
         "typedef struct { uint32_t offset, bytes; uint16_t w, h; uint8_t fmt, mip, pad[2]; } rl_tex_asset_t;",
         "", "enum {"]
    for spec, *_ in table:
        h.append(f"    TEX_{spec.name.upper()},")
    h += ["    TEX_COUNT", "};", "", "static const rl_tex_asset_t rl_tex_assets[TEX_COUNT] = {"]
    for spec, w, hh, off, size in table:
        fmt = "RL_TEXFMT_ARGB4444" if spec.fmt == "ARGB4444" else "RL_TEXFMT_RGB565"
        h.append(f"    {{{off}u, {size}u, {w}, {hh}, {fmt}, {int(spec.mip)}, {{0, 0}}}},  /* {spec.name} */")
    h += ["};", "", "#endif", ""]
    (out / "texture_assets.h").write_text("\n".join(h))
    m = ["/* Generated by tools/build_textures.py. Do not edit. */",
         "#ifndef RL_FONT_METRICS_H", "#define RL_FONT_METRICS_H", "#include <stdint.h>", "",
         f"#define FONT_ROW_H {FONT_ROW_H}", "#define FONT_TEX_W 512", "#define FONT_TEX_H 256",
         f"#define FONT_CAP_TOP {cap[0]}", f"#define FONT_CAP_BOTTOM {cap[1]}",
         "/* ASCII 32..127: atlas x, row, rect width, rect left from the pen, advance (eng_glyph_t). */",
         "static const struct { uint16_t x; uint8_t row, w; int8_t ox; uint8_t adv; } font_glyphs[96] = {"]
    for i in range(0, 96, 4):
        m.append("    " + " ".join(f"{{{x}, {r}, {w}, {ox}, {adv}}}," for x, r, w, ox, adv in glyphs[i:i + 4]))
    m += ["};", "", "#endif", ""]
    (out / "font_metrics.h").write_text("\n".join(m))
    print(f"textures.pak: {len(pak)} bytes, {len(table)} textures")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, default=GEN)
    build(p.parse_args().output_dir.resolve())


if __name__ == "__main__":
    main()
