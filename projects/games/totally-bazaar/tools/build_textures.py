#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13", "numpy>=1.26"]
# ///
"""Compose Totally Bazaar's textures and pack them for the PowerVR.

Inputs: Blender renders in assets/source/renders (see render_art_blender.py)
and the Inter font that ships with Blender. Outputs:

    assets/generated/textures.pak       16-bit texels, 32-byte aligned
    assets/generated/texture_assets.h   ids, formats, font metrics
    assets/generated/texture_assets.c   tables describing the pak
    assets/generated/previews/*.png     8-bit previews of every texture
"""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_PATH = "/Applications/Blender.app/Contents/Resources/5.2/datafiles/fonts/Inter.woff2"
GOODS = ["diamond", "gold", "silver", "cloth", "spice", "leather", "camel"]
CARD_COLORS = {
    "diamond": (214, 30, 64),
    "gold": (238, 160, 26),
    "silver": (92, 120, 168),
    "cloth": (142, 58, 196),
    "spice": (46, 160, 70),
    "leather": (150, 78, 36),
    "camel": (24, 168, 190),
}
CARD_W, CARD_H = 128, 180          # visible card inside a 128x256 texture
SS = 4                             # supersampling factor for vector art

BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9],
                  [15, 7, 13, 5]], dtype=np.float32) / 16.0 - 0.5


def font(size: int, weight: int = 900) -> ImageFont.FreeTypeFont:
    f = ImageFont.truetype(FONT_PATH, size)
    try:
        f.set_variation_by_axes([min(32, max(14, size)), weight])
    except Exception:
        pass
    return f


# ------------------------------------------------------------ conversion

def quantize(img: Image.Image, fmt: str) -> np.ndarray:
    a = np.asarray(img.convert("RGBA"), dtype=np.float32)
    h, w = a.shape[:2]
    d = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w]
    r, g, b, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]

    def q(ch, bits, dither=True):
        levels = (1 << bits) - 1
        v = ch / 255.0 * levels + (d if dither else 0.0)
        return np.clip(np.rint(v), 0, levels).astype(np.uint16)

    if fmt == "RGB565":
        out = (q(r, 5) << 11) | (q(g, 6) << 5) | q(b, 5)
    elif fmt == "ARGB1555":
        alpha = (al >= 128).astype(np.uint16)
        out = (alpha << 15) | (q(r, 5) << 10) | (q(g, 5) << 5) | q(b, 5)
        out = np.where(alpha == 1, out, 0).astype(np.uint16)
    elif fmt == "ARGB4444":
        out = (q(al, 4, False) << 12) | (q(r, 4) << 8) | (q(g, 4) << 4) | q(b, 4)
    else:
        raise ValueError(fmt)
    return out.astype("<u2")


def preview(arr: np.ndarray, fmt: str) -> Image.Image:
    a = arr.astype(np.uint32)
    if fmt == "RGB565":
        r = (a >> 11 & 31) * 255 // 31
        g = (a >> 5 & 63) * 255 // 63
        b = (a & 31) * 255 // 31
        al = np.full_like(a, 255)
    elif fmt == "ARGB1555":
        al = (a >> 15) * 255
        r = (a >> 10 & 31) * 255 // 31
        g = (a >> 5 & 31) * 255 // 31
        b = (a & 31) * 255 // 31
    else:
        al = (a >> 12 & 15) * 17
        r = (a >> 8 & 15) * 17
        g = (a >> 4 & 15) * 17
        b = (a & 15) * 17
    return Image.fromarray(np.dstack([r, g, b, al]).astype(np.uint8), "RGBA")


# ------------------------------------------------------------ drawing utils

def rounded_mask(w, h, r, ss=SS):
    m = Image.new("L", (w * ss, h * ss), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w * ss - 1, h * ss - 1),
                                        radius=r * ss, fill=255)
    return m.resize((w, h), Image.LANCZOS)


def radial(w, h, cx, cy, radius, inner, outer, power=1.0):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / radius
    t = np.clip(d, 0, 1) ** power
    inner = np.array(inner, np.float32)
    outer = np.array(outer, np.float32)
    rgb = inner[None, None, :] * (1 - t[..., None]) + outer[None, None, :] * t[..., None]
    return rgb


def mix(c1, c2, t):
    return tuple(int(round(a * (1 - t) + b * t)) for a, b in zip(c1, c2))


def load_render(name: str, src: Path) -> Image.Image:
    path = src / "renders" / f"{name}.png"
    img = Image.open(path).convert("RGBA")
    bbox = img.getbbox()
    return img.crop(bbox) if bbox else img


def fit(img: Image.Image, box_w: int, box_h: int) -> Image.Image:
    s = min(box_w / img.width, box_h / img.height)
    return img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))),
                      Image.LANCZOS)


def drop_shadow(img: Image.Image, offset=(0, 4), blur=4, alpha=150):
    pad = blur * 3 + max(abs(offset[0]), abs(offset[1]))
    base = Image.new("RGBA", (img.width + pad * 2, img.height + pad * 2), (0, 0, 0, 0))
    sh = Image.new("RGBA", base.size, (0, 0, 0, 0))
    a = img.split()[3].point(lambda v: v * alpha // 255)
    black = Image.new("RGBA", img.size, (20, 6, 12, 255))
    black.putalpha(a)
    sh.paste(black, (pad + offset[0], pad + offset[1]), black)
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    base = Image.alpha_composite(base, sh)
    base.alpha_composite(img, (pad, pad))
    return base


def outline(img: Image.Image, width: int, color=(30, 8, 20)):
    a = img.split()[3]
    grown = a.filter(ImageFilter.MaxFilter(width * 2 + 1))
    base = Image.new("RGBA", img.size, color + (0,))
    base.putalpha(grown)
    base.alpha_composite(img)
    return base


# ------------------------------------------------------------ card faces

def sunburst(w, h, cx, cy, rays, c1, c2):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    ang = np.arctan2(y - cy, x - cx)
    t = (np.sin(ang * rays) > 0).astype(np.float32)
    return t


def card_frame(color, w=CARD_W, h=CARD_H) -> Image.Image:
    W, H = w * SS, h * SS
    light = mix(color, (255, 255, 255), 0.55)
    dark = mix(color, (0, 0, 0), 0.55)
    field = radial(W, H, W * 0.5, H * 0.46, W * 0.85, light, dark, 1.2)
    rays = sunburst(W, H, W * 0.5, H * 0.46, 12, None, None)
    field = field * (1.0 + 0.10 * rays[..., None])
    field = np.clip(field, 0, 255).astype(np.uint8)
    img = Image.fromarray(field, "RGB").convert("RGBA")
    d = ImageDraw.Draw(img)
    gold = (255, 206, 92)
    # Ornamental inner frame: double gold line with corner lozenges.
    m = 9 * SS
    d.rounded_rectangle((m, m, W - m, H - m), radius=8 * SS, outline=gold,
                        width=int(1.6 * SS))
    m2 = 12 * SS
    d.rounded_rectangle((m2, m2, W - m2, H - m2), radius=6 * SS,
                        outline=mix(gold, dark, 0.45), width=int(0.8 * SS))
    for cx, cy in ((m, m), (W - m, m), (m, H - m), (W - m, H - m)):
        r = 5 * SS
        d.polygon([(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)],
                  fill=gold, outline=dark)
    # Paisley-ish arches top and bottom.
    for yy, flip in ((m2 + 2 * SS, 1), (H - m2 - 2 * SS, -1)):
        for i in range(5):
            x0 = W * (0.2 + 0.15 * i)
            d.arc((x0 - 6 * SS, yy - 6 * SS * flip if flip > 0 else yy, x0 + 6 * SS,
                   yy + 6 * SS if flip > 0 else yy + 6 * SS * -flip),
                  0 if flip > 0 else 180, 180 if flip > 0 else 360,
                  fill=mix(gold, color, 0.3), width=int(0.8 * SS))
    # Cream outer border.
    border = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bd = ImageDraw.Draw(border)
    bd.rounded_rectangle((0, 0, W - 1, H - 1), radius=11 * SS, fill=(255, 244, 220, 255))
    bd.rounded_rectangle((4 * SS, 4 * SS, W - 4 * SS, H - 4 * SS), radius=8 * SS,
                         fill=(0, 0, 0, 0))
    bd.rounded_rectangle((4 * SS, 4 * SS, W - 4 * SS, H - 4 * SS), radius=8 * SS,
                         outline=(40, 14, 24, 255), width=int(1.2 * SS))
    img.alpha_composite(border)
    img = img.resize((w, h), Image.LANCZOS)
    img.putalpha(rounded_mask(w, h, 11))
    return img


def make_card(name: str, src: Path) -> Image.Image:
    color = CARD_COLORS[name]
    card = card_frame(color)
    icon = load_render(name, src)
    big = fit(icon, 112, 104)
    big = drop_shadow(big, (0, 3), 3, 160)
    card.alpha_composite(big, ((CARD_W - big.width) // 2,
                               int(CARD_H * 0.48 - big.height / 2)))
    # Corner indices so overlapped cards stay readable.
    pip = fit(icon, 26, 22)
    pip = outline(pip, 1, (255, 244, 220))
    card.alpha_composite(pip, (8, 8))
    pip2 = pip.rotate(180)
    card.alpha_composite(pip2, (CARD_W - 8 - pip2.width, CARD_H - 8 - pip2.height))
    # Name ribbon along the bottom.
    label = name.upper() + ("S" if name in ("diamond", "camel") else "")
    size = 15
    f = font(size)
    while f.getlength(label) > CARD_W - 44 and size > 9:
        size -= 1
        f = font(size)
    tw = f.getlength(label)
    ribbon_y = CARD_H - 40
    d = ImageDraw.Draw(card)
    d.rounded_rectangle((18, ribbon_y, CARD_W - 18, ribbon_y + 20), radius=9,
                        fill=mix(color, (0, 0, 0), 0.45) + (235,),
                        outline=(255, 206, 92, 255), width=1)
    d.text(((CARD_W - tw) / 2, ribbon_y + 1 + (15 - size) / 2), label, font=f, fill=(255, 250, 235),
           stroke_width=0)
    tex = Image.new("RGBA", (128, 256), (0, 0, 0, 0))
    tex.alpha_composite(card, (0, 0))
    return tex


def make_card_back(src: Path) -> Image.Image:
    W, H = CARD_W * SS, CARD_H * SS
    base = radial(W, H, W / 2, H / 2, W * 0.9, (64, 30, 140), (18, 8, 52), 1.1)
    img = Image.fromarray(base.astype(np.uint8), "RGB").convert("RGBA")
    d = ImageDraw.Draw(img)
    gold = (255, 196, 70)
    # Jali lattice.
    step = 14 * SS
    for k in range(-H // step - 2, W // step + H // step + 2):
        x = k * step
        d.line((x, 0, x + H, H), fill=(110, 60, 190), width=int(1.0 * SS))
        d.line((x, H, x + H, 0), fill=(110, 60, 190), width=int(1.0 * SS))
    for i in range(-2, W // step + 3):
        for j in range(-2, H // step + 3):
            cx = i * step + (step // 2 if j % 2 else 0)
            cy = j * step // 2 * 2 // 2
            cy = j * (step // 2)
            if (i + j) % 2 == 0:
                d.ellipse((cx - 1.6 * SS, cy - 1.6 * SS, cx + 1.6 * SS, cy + 1.6 * SS),
                          fill=gold)
    # Central medallion.
    cx, cy, r = W / 2, H / 2, 40 * SS
    for rr, col in ((r + 6 * SS, gold), (r + 3 * SS, (40, 14, 80)), (r, (220, 40, 110))):
        d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), fill=col)
    petals = 16
    for k in range(petals):
        a = 2 * math.pi * k / petals
        px, py = cx + math.cos(a) * (r + 10 * SS), cy + math.sin(a) * (r + 10 * SS)
        d.ellipse((px - 4 * SS, py - 4 * SS, px + 4 * SS, py + 4 * SS), fill=gold)
    img = img.resize((CARD_W, CARD_H), Image.LANCZOS)
    camel = fit(load_render("camel", src), 60, 46)
    camel = drop_shadow(camel, (0, 2), 2, 160)
    img.alpha_composite(camel, ((CARD_W - camel.width) // 2, CARD_H // 2 - camel.height // 2 - 6))
    d = ImageDraw.Draw(img)
    f = font(12)
    t = "BAZAAR"
    d.text(((CARD_W - f.getlength(t)) / 2, CARD_H / 2 + 17), t, font=f,
           fill=(255, 230, 150), stroke_width=1, stroke_fill=(60, 10, 40))
    border = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bd = ImageDraw.Draw(border)
    bd.rounded_rectangle((0, 0, W - 1, H - 1), radius=11 * SS, fill=(255, 220, 130, 255))
    bd.rounded_rectangle((4 * SS, 4 * SS, W - 4 * SS, H - 4 * SS), radius=8 * SS,
                         fill=(0, 0, 0, 0))
    bd.rounded_rectangle((4 * SS, 4 * SS, W - 4 * SS, H - 4 * SS), radius=8 * SS,
                         outline=(40, 10, 30, 255), width=int(1.2 * SS))
    img.alpha_composite(border.resize((CARD_W, CARD_H), Image.LANCZOS))
    img.putalpha(rounded_mask(CARD_W, CARD_H, 11))
    tex = Image.new("RGBA", (128, 256), (0, 0, 0, 0))
    tex.alpha_composite(img, (0, 0))
    return tex


# ------------------------------------------------------------ atlas

def coin(size, rim, face, icon=None, text=None, text_color=(255, 255, 255)):
    W = size * SS
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = W / 2
    # Hexagonal-ish token like the board game, with a bevelled rim.
    def poly(r, rot=math.pi / 6):
        return [(c + r * math.cos(rot + k * math.pi / 3), c + r * math.sin(rot + k * math.pi / 3))
                for k in range(6)]
    d.polygon(poly(W * 0.49), fill=(30, 10, 20))
    d.polygon(poly(W * 0.46), fill=rim)
    d.polygon(poly(W * 0.38), fill=mix(rim, (0, 0, 0), 0.35))
    d.polygon(poly(W * 0.355), fill=face)
    # Highlight.
    hl = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    ImageDraw.Draw(hl).ellipse((W * 0.2, W * 0.1, W * 0.8, W * 0.5), fill=(255, 255, 255, 55))
    img.alpha_composite(hl.filter(ImageFilter.GaussianBlur(W * 0.04)))
    img = img.resize((size, size), Image.LANCZOS)
    if icon is not None:
        ic = fit(icon, int(size * 0.5), int(size * 0.34))
        img.alpha_composite(ic, ((size - ic.width) // 2, int(size * 0.16)))
    if text:
        f = font(int(size * 0.34))
        tw = f.getlength(text)
        ImageDraw.Draw(img).text(((size - tw) / 2, size * 0.36), text, font=f,
                                 fill=text_color, stroke_width=2, stroke_fill=(30, 8, 20))
    return img


def star(size):
    W = size * SS
    img = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(img)
    c = W / 2
    pts = []
    for k in range(8):
        r = W * 0.48 if k % 2 == 0 else W * 0.1
        a = k * math.pi / 4
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=255)
    img = img.filter(ImageFilter.GaussianBlur(W * 0.02))
    glow = Image.new("L", (W, W), 0)
    ImageDraw.Draw(glow).ellipse((W * 0.3, W * 0.3, W * 0.7, W * 0.7), fill=200)
    glow = glow.filter(ImageFilter.GaussianBlur(W * 0.08))
    a = np.maximum(np.asarray(img), np.asarray(glow))
    out = Image.new("RGBA", (W, W), (255, 255, 255, 0))
    out.putalpha(Image.fromarray(a))
    return out.resize((size, size), Image.LANCZOS)


def glow_circle(size):
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    d = np.sqrt((x - size / 2 + 0.5) ** 2 + (y - size / 2 + 0.5) ** 2) / (size / 2)
    a = np.clip(1 - d, 0, 1) ** 1.6 * 255
    out = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    out.putalpha(Image.fromarray(a.astype(np.uint8)))
    return out


def panel_tile(size):
    """White rounded square with a soft bright rim, sliced 9 ways in-game."""
    W = size * SS
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, W - 1, W - 1), radius=W * 0.3, fill=(255, 255, 255, 255))
    d.rounded_rectangle((W * 0.06, W * 0.06, W * 0.94, W * 0.94), radius=W * 0.25,
                        fill=(255, 255, 255, 190))
    return img.resize((size, size), Image.LANCZOS)


def button(size, letter, color, pill=False):
    W = size * SS
    if pill:
        img = Image.new("RGBA", (W * 2, W), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((SS * 2, W * 0.18, W * 2 - SS * 2, W * 0.82), radius=W * 0.3,
                            fill=(20, 10, 20), outline=(255, 255, 255), width=int(SS * 1.5))
        img = img.resize((size * 2, size), Image.LANCZOS)
        f = font(int(size * 0.42))
        bb = f.getbbox(letter)
        ImageDraw.Draw(img).text(((size * 2 - f.getlength(letter)) / 2,
                                  (size - (bb[3] - bb[1])) / 2 - bb[1]), letter,
                                 font=f, fill=(255, 255, 255))
        return img
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if pill:
        d.rounded_rectangle((W * 0.02, W * 0.22, W * 0.98, W * 0.78), radius=W * 0.28,
                            fill=(20, 10, 20), outline=(255, 255, 255), width=int(SS * 1.5))
    else:
        d.ellipse((W * 0.03, W * 0.03, W * 0.97, W * 0.97), fill=(25, 10, 20))
        d.ellipse((W * 0.1, W * 0.1, W * 0.9, W * 0.9), fill=color)
        d.ellipse((W * 0.22, W * 0.14, W * 0.78, W * 0.48), fill=mix(color, (255, 255, 255), 0.45))
    img = img.resize((size, size), Image.LANCZOS)
    f = font(int(size * (0.3 if pill else 0.62)))
    tw = f.getlength(letter)
    bb = f.getbbox(letter)
    th = bb[3] - bb[1]
    ImageDraw.Draw(img).text(((size - tw) / 2, (size - th) / 2 - bb[1]), letter, font=f,
                             fill=(255, 255, 255), stroke_width=1 if not pill else 0,
                             stroke_fill=(30, 10, 20))
    return img


def dpad(size):
    W = size * SS
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    a, b = W * 0.34, W * 0.66
    d.rectangle((a, W * 0.04, b, W * 0.96), fill=(25, 10, 20))
    d.rectangle((W * 0.04, a, W * 0.96, b), fill=(25, 10, 20))
    a, b = W * 0.39, W * 0.61
    d.rectangle((a, W * 0.09, b, W * 0.91), fill=(230, 230, 240))
    d.rectangle((W * 0.09, a, W * 0.91, b), fill=(230, 230, 240))
    return img.resize((size, size), Image.LANCZOS)


def seal(size):
    W = size * SS
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = W / 2
    pts = []
    for k in range(32):
        r = W * (0.49 if k % 2 == 0 else 0.42)
        a = k * math.pi / 16
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=(140, 20, 40))
    d.ellipse((W * 0.14, W * 0.14, W * 0.86, W * 0.86), fill=(255, 200, 70))
    d.ellipse((W * 0.2, W * 0.2, W * 0.8, W * 0.8), fill=(220, 40, 70))
    pts = []
    for k in range(10):
        r = W * (0.25 if k % 2 == 0 else 0.11)
        a = -math.pi / 2 + k * math.pi / 5
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=(255, 220, 110))
    return img.resize((size, size), Image.LANCZOS)


def make_atlas(src: Path):
    atlas = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    icons = {g: load_render(g, src) for g in GOODS}
    rims = [(255, 205, 90), (255, 205, 90), (200, 210, 230), (255, 205, 90),
            (255, 205, 90), (220, 160, 100)]
    for i, g in enumerate(GOODS[:6]):
        face = mix(CARD_COLORS[g], (0, 0, 0), 0.15)
        atlas.alpha_composite(coin(64, rims[i], face, icons[g]), ((i % 4) * 64, (i // 4) * 64))
    bronze = (200, 120, 60)
    for i, n in enumerate(("x3", "x4", "x5")):
        cell = 6 + i
        atlas.alpha_composite(coin(64, (255, 205, 90), bronze, None, n, (255, 240, 200)),
                              ((cell % 4) * 64, (cell // 4) * 64))
    atlas.alpha_composite(coin(64, (255, 205, 90), CARD_COLORS["camel"], icons["camel"]),
                          (64, 128))
    atlas.alpha_composite(seal(64), (128, 128))
    atlas.alpha_composite(star(64), (192, 128))
    atlas.alpha_composite(glow_circle(64), (0, 192))
    atlas.alpha_composite(panel_tile(64), (64, 192))
    # Cell 14: A B X Y buttons (Dreamcast colours).
    for i, (l, col) in enumerate((("A", (226, 40, 50)), ("B", (40, 100, 230)),
                                  ("X", (240, 190, 20)), ("Y", (40, 170, 70)))):
        atlas.alpha_composite(button(32, l, col), (128 + (i % 2) * 32, 192 + (i // 2) * 32))
    # Cell 15: START pill (64x32), camel mini, coin mini.
    atlas.alpha_composite(button(32, "START", (0, 0, 0), pill=True), (192, 192))
    camel_mini = outline(fit(icons["camel"], 30, 28), 1, (40, 14, 24))
    atlas.alpha_composite(camel_mini, (192 + (32 - camel_mini.width) // 2, 224 + (32 - camel_mini.height) // 2))
    atlas.alpha_composite(coin(32, (255, 205, 90), (240, 170, 40)), (224, 224))
    return atlas


# ------------------------------------------------------------ font

FONT_CHARS = [chr(c) for c in range(32, 127)]


def make_font(size: int, stroke: int, tex_w: int, tex_h: int):
    f = font(size)
    ascent, descent = f.getmetrics()
    line_h = ascent + descent + stroke * 2
    img = Image.new("RGBA", (tex_w, tex_h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    glyphs = []
    x, y = 1, 1
    for ch in FONT_CHARS:
        adv = f.getlength(ch)
        if ch == " ":
            glyphs.append((0, 0, 0, 0, 0, 0, round(adv)))
            continue
        l, t, r, b = d.textbbox((0, 0), ch, font=f, stroke_width=stroke)
        w, h = r - l + 2, b - t + 2
        if x + w >= tex_w:
            x = 1
            y += line_h + 2
        if y + h >= tex_h:
            raise SystemExit(f"font atlas {tex_w}x{tex_h} too small for size {size}")
        d.text((x - l + 1, y - t + 1), ch, font=f, fill=(255, 255, 255, 255),
               stroke_width=stroke, stroke_fill=(20, 6, 16, 255))
        glyphs.append((x, y, w, h, l - 1 + 0, t - 1, round(adv)))
        x += w + 1
    return img, glyphs, line_h, ascent


# ------------------------------------------------------------ backgrounds

def table_background():
    W, H = 640, 480
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    base = np.array((112, 14, 52), np.float32)
    # Diamond lattice rug pattern.
    s = 40.0
    u = (x + y) / s
    v = (x - y) / s
    fu = np.abs(u - np.round(u))
    fv = np.abs(v - np.round(v))
    line = np.clip(1.0 - np.minimum(fu, fv) * 14, 0, 1)
    node = np.clip(1.0 - np.sqrt(fu ** 2 + fv ** 2) * 6, 0, 1)
    cu = np.abs((u + 0.5) - np.round(u + 0.5))
    cv = np.abs((v + 0.5) - np.round(v + 0.5))
    flower = np.clip(1.0 - (np.abs(cu) + np.abs(cv)) * 5.5, 0, 1)
    rgb = base[None, None, :] * np.ones((H, W, 1), np.float32)
    rgb = rgb * (1 - 0.35 * line[..., None]) + np.array((70, 6, 34)) * 0.35 * line[..., None]
    rgb = rgb * (1 - flower[..., None] * 0.6) + np.array((176, 40, 86)) * flower[..., None] * 0.6
    rgb = rgb * (1 - node[..., None]) + np.array((214, 150, 60)) * node[..., None]
    # Vignette and a warm glow centred on the market.
    d = np.sqrt(((x - W / 2) / (W * 0.62)) ** 2 + ((y - H * 0.47) / (H * 0.7)) ** 2)
    rgb *= np.clip(1.25 - d * 0.75, 0.35, 1.25)[..., None]
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    dr = ImageDraw.Draw(img)
    # Woven border bands.
    for inset, col, wdt in ((3, (214, 150, 60), 3), (9, (20, 120, 130), 3), (14, (214, 150, 60), 1)):
        dr.rectangle((inset, inset, W - 1 - inset, H - 1 - inset), outline=col, width=wdt)
    # Striped market awning across the top with scalloped hem.
    aw = Image.new("RGBA", (W * SS, 30 * SS), (0, 0, 0, 0))
    ad = ImageDraw.Draw(aw)
    stripe = 32 * SS
    for i in range(W * SS // stripe + 1):
        col = (236, 60, 92) if i % 2 == 0 else (255, 214, 96)
        ad.rectangle((i * stripe, 0, (i + 1) * stripe, 18 * SS), fill=col)
        ad.ellipse((i * stripe, 6 * SS, (i + 1) * stripe, 30 * SS), fill=col)
    shade = Image.new("RGBA", aw.size, (0, 0, 0, 0))
    ImageDraw.Draw(shade).rectangle((0, 0, W * SS, 5 * SS), fill=(80, 0, 30, 90))
    aw.alpha_composite(shade)
    aw = aw.resize((W, 30), Image.LANCZOS)
    aw = drop_shadow(aw, (0, 3), 3, 140)
    img.alpha_composite(aw, (-((aw.width - W) // 2), -((aw.height - 30) // 2)))
    # Static HUD panels, baked here so they cost nothing per frame on the
    # PowerVR (keep in sync with the layout constants in src/main.c).
    def panel(x, y, w, h, r, rgba):
        layer = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
        ImageDraw.Draw(layer).rounded_rectangle(
            (x * SS, y * SS, (x + w) * SS, (y + h) * SS), radius=r * SS, fill=rgba)
        img.alpha_composite(layer.resize((W, H), Image.LANCZOS))

    panel(126, 104, 416, 126, 16, (16, 4, 24, 96))      # market
    panel(106, 296, 524, 120, 16, (16, 4, 24, 96))      # hand
    panel(550, 100, 86, 196, 14, (32, 8, 40, 192))      # bazaar
    bar = Image.new("RGBA", (W, 56), (0, 0, 0, 0))
    for yy in range(56):
        t = yy / 55.0
        a = int(192 + (240 - 192) * t)
        col = (int(32 + (16 - 32) * t), int(8 + (4 - 8) * t), int(40 + (16 - 40) * t), a)
        ImageDraw.Draw(bar).line((0, yy, W, yy), fill=col)
    img.alpha_composite(bar, (0, 424))
    return img.convert("RGB").resize((512, 512), Image.LANCZOS)


def title_background(src: Path):
    W, H = 640, 480
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    stops = [(0.0, (22, 6, 52)), (0.35, (96, 18, 110)), (0.58, (226, 50, 110)),
             (0.72, (255, 128, 64)), (0.8, (255, 206, 120))]
    t = y / (H * 1.0)
    rgb = np.zeros((H, W, 3), np.float32)
    for i in range(len(stops) - 1):
        t0, c0 = stops[i]
        t1, c1 = stops[i + 1]
        m = (t >= t0) & (t < t1)
        k = ((t - t0) / (t1 - t0))[..., None]
        rgb = np.where(m[..., None], np.array(c0) * (1 - k) + np.array(c1) * k, rgb)
    rgb = np.where((t >= stops[-1][0])[..., None], np.array(stops[-1][1], np.float32), rgb)
    img = Image.fromarray(rgb.astype(np.uint8), "RGB").convert("RGBA")
    d = ImageDraw.Draw(img)
    rng = np.random.default_rng(2026)
    for _ in range(120):
        sx, sy = rng.uniform(0, W), rng.uniform(0, H * 0.4)
        r = rng.uniform(0.4, 1.4)
        a = int(rng.uniform(120, 255) * (1 - sy / (H * 0.45)))
        d.ellipse((sx - r, sy - r, sx + r, sy + r), fill=(255, 240, 255, a))
    # Retro striped sun.
    sun = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sun)
    cx, cy, r = 320 * SS, 300 * SS, 110 * SS
    sd.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 214, 110, 255))
    for k in range(7):
        yy = cy + (10 + k * 13) * SS
        sd.rectangle((cx - r, yy, cx + r, yy + (2 + k * 1.3) * SS), fill=(0, 0, 0, 0))
    sun = sun.resize((W, H), Image.LANCZOS)
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((320 - 160, 300 - 160, 320 + 160, 300 + 160), fill=(255, 150, 90, 110))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(40)))
    img.alpha_composite(sun)
    # Palace skyline: domes, minarets, chhatris and arches.
    sky = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    kd = ImageDraw.Draw(sky)
    far = (120, 30, 96, 255)
    near = (46, 8, 46, 255)

    def dome(x0, base, w, h, col):
        kd.rectangle((x0 * SS, base * SS - h * 0.35 * SS, (x0 + w) * SS, 480 * SS), fill=col)
        kd.ellipse((x0 * SS, (base - h) * SS, (x0 + w) * SS, (base - h * 0.2) * SS), fill=col)
        kd.polygon([((x0 + w / 2 - 2) * SS, (base - h * 0.95) * SS), ((x0 + w / 2) * SS, (base - h - 12) * SS),
                    ((x0 + w / 2 + 2) * SS, (base - h * 0.95) * SS)], fill=col)

    def minaret(x0, base, h, col):
        kd.rectangle(((x0 - 5) * SS, (base - h) * SS, (x0 + 5) * SS, 480 * SS), fill=col)
        kd.rectangle(((x0 - 8) * SS, (base - h * 0.7) * SS, (x0 + 8) * SS, (base - h * 0.7 + 4) * SS), fill=col)
        kd.ellipse(((x0 - 8) * SS, (base - h - 12) * SS, (x0 + 8) * SS, (base - h + 4) * SS), fill=col)
        kd.polygon([((x0 - 1.5) * SS, (base - h - 10) * SS), (x0 * SS, (base - h - 24) * SS),
                    ((x0 + 1.5) * SS, (base - h - 10) * SS)], fill=col)

    for x0, w, h in ((20, 70, 50), (150, 90, 70), (420, 100, 64), (560, 70, 46)):
        dome(x0, 360, w, h, far)
    for x0, h in ((120, 110), (260, 90), (390, 120), (540, 96)):
        minaret(x0, 360, h, far)
    # Near wall with arches (Hawa Mahal-ish honeycomb).
    kd.rectangle((0, 372 * SS, W * SS, H * SS), fill=near)
    for i in range(16):
        ax = i * 42 + 6
        kd.rounded_rectangle((ax * SS, 352 * SS, (ax + 30) * SS, 380 * SS), radius=12 * SS, fill=near)
    for x0, w, h in ((60, 110, 84), (250, 140, 104), (470, 110, 84)):
        dome(x0, 372, w, h, near)
    for x0, h in ((40, 130), (205, 150), (430, 150), (600, 130)):
        minaret(x0, 372, h, near)
    windows = (255, 180, 90, 255)
    for i in range(16):
        ax = i * 42 + 15
        kd.rounded_rectangle((ax * SS, 392 * SS, (ax + 12) * SS, 412 * SS), radius=6 * SS, fill=windows)
        kd.rounded_rectangle((ax * SS, 428 * SS, (ax + 12) * SS, 448 * SS), radius=6 * SS,
                             fill=(220, 110, 70, 255))
    sky = sky.resize((W, H), Image.LANCZOS)
    img.alpha_composite(sky)
    return img.convert("RGB").resize((512, 512), Image.LANCZOS)


def make_logo(src: Path):
    logo = load_render("logo", src)
    logo = fit(logo, 470, 200)
    logo = outline(logo, 4, (40, 6, 40))
    out = Image.new("RGBA", (512, 256), (0, 0, 0, 0))
    out.alpha_composite(fit(logo, 508, 252) if logo.width > 508 or logo.height > 252 else logo,
                        ((512 - min(logo.width, 508)) // 2, (256 - min(logo.height, 252)) // 2))
    return out


# ------------------------------------------------------------ output

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-dir", type=Path, required=True)
    ap.add_argument("--output-dir", type=Path, required=True)
    args = ap.parse_args()
    src, out = args.source_dir, args.output_dir
    out.mkdir(parents=True, exist_ok=True)
    (out / "previews").mkdir(exist_ok=True)

    textures = []  # (enum name, image, fmt)
    for g in GOODS:
        textures.append((f"CARD_{g.upper()}", make_card(g, src), "ARGB1555"))
    textures.append(("CARD_BACK", make_card_back(src), "ARGB1555"))
    textures.append(("ATLAS", make_atlas(src), "ARGB4444"))
    small_img, small_glyphs, small_lh, small_asc = make_font(18, 2, 256, 256)
    big_img, big_glyphs, big_lh, big_asc = make_font(36, 3, 512, 512)
    textures.append(("FONT_SMALL", small_img, "ARGB4444"))
    textures.append(("FONT_BIG", big_img, "ARGB4444"))
    textures.append(("TABLE", table_background(), "RGB565"))
    textures.append(("TITLE", title_background(src), "RGB565"))
    textures.append(("LOGO", make_logo(src), "ARGB1555"))

    pak = bytearray()
    entries = []
    for name, img, fmt in textures:
        arr = quantize(img, fmt)
        preview(arr, fmt).save(out / "previews" / f"{name.lower()}.png")
        while len(pak) % 32:
            pak.append(0)
        entries.append((name, len(pak), img.width, img.height, fmt))
        pak += arr.tobytes()
    (out / "textures.pak").write_bytes(bytes(pak))

    fmt_enum = {"RGB565": "JP_TEX_RGB565", "ARGB1555": "JP_TEX_ARGB1555",
                "ARGB4444": "JP_TEX_ARGB4444"}
    h = ["/* Generated by tools/build_textures.py. Do not edit by hand. */",
         "#ifndef BAZAAR_TEXTURE_ASSETS_H", "#define BAZAAR_TEXTURE_ASSETS_H", "",
         "#include <stdint.h>", "",
         "typedef enum { JP_TEX_RGB565 = 0, JP_TEX_ARGB1555, JP_TEX_ARGB4444 } jp_tex_format_t;",
         "", "typedef struct {", "    uint32_t offset;", "    uint16_t width;",
         "    uint16_t height;", "    jp_tex_format_t format;", "} jp_texture_asset_t;", "",
         "typedef struct {", "    uint16_t x, y;", "    uint8_t w, h;", "    int8_t ox, oy;",
         "    uint8_t advance;", "} jp_glyph_t;", "",
         "typedef struct {", "    const jp_glyph_t *glyphs; /* ASCII 32..126 */",
         "    uint16_t line_height;", "    uint16_t ascent;", "    uint16_t tex;",
         "} jp_font_asset_t;", "", "typedef enum {"]
    for i, e in enumerate(entries):
        h.append(f"    JP_TEXID_{e[0]} = {i},")
    h += [f"    JP_TEXTURE_COUNT = {len(entries)}", "} jp_texture_id_t;", "",
          f"#define JP_CARD_TEX_W {CARD_W}", f"#define JP_CARD_TEX_H {CARD_H}", "",
          "/* Atlas cells are 64x64 in a 4x4 grid; see build_textures.py. */",
          "enum {", "    JP_ATLAS_TOKEN0 = 0, /* six goods tokens, cells 0-5 */",
          "    JP_ATLAS_BONUS3 = 6, JP_ATLAS_BONUS4 = 7, JP_ATLAS_BONUS5 = 8,",
          "    JP_ATLAS_CAMEL_TOKEN = 9, JP_ATLAS_SEAL = 10, JP_ATLAS_STAR = 11,",
          "    JP_ATLAS_GLOW = 12, JP_ATLAS_PANEL = 13, JP_ATLAS_BUTTONS = 14,",
          "    JP_ATLAS_MISC = 15", "};", "",
          "extern const jp_texture_asset_t jp_texture_assets[JP_TEXTURE_COUNT];",
          "extern const jp_font_asset_t jp_font_small;",
          "extern const jp_font_asset_t jp_font_big;",
          "extern const uint8_t jp_textures_pak[];", "", "#endif", ""]
    (out / "texture_assets.h").write_text("\n".join(h))

    c = ["/* Generated by tools/build_textures.py. Do not edit by hand. */",
         '#include "texture_assets.h"', "",
         "const jp_texture_asset_t jp_texture_assets[JP_TEXTURE_COUNT] = {"]
    for name, off, w, hh, fmt in entries:
        c.append(f"    {{ {off}u, {w}, {hh}, {fmt_enum[fmt]} }}, /* {name} */")
    c.append("};")
    for label, glyphs, lh, asc, tex in (("small", small_glyphs, small_lh, small_asc, "FONT_SMALL"),
                                        ("big", big_glyphs, big_lh, big_asc, "FONT_BIG")):
        c.append("")
        c.append(f"static const jp_glyph_t glyphs_{label}[95] = {{")
        for (x, y, w, gh, ox, oy, adv) in glyphs:
            c.append(f"    {{ {x}, {y}, {w}, {gh}, {ox}, {oy}, {adv} }},")
        c.append("};")
        c.append(f"const jp_font_asset_t jp_font_{label} = {{ glyphs_{label}, {lh}, {asc}, JP_TEXID_{tex} }};")
    (out / "texture_assets.c").write_text("\n".join(c) + "\n")
    print(f"textures.pak: {len(pak)} bytes, {len(entries)} textures")


if __name__ == "__main__":
    main()
