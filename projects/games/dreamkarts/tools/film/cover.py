"""A 1280x720 YouTube cover from a clean Flycast still (DK_FILM_CLEAN build).

usage: uv run --with pillow --with numpy --with matplotlib tools/film/cover.py
           <still.png> <out.png> [--y 0.5] [--zoom 1] [--side right] [--ribbon TEXT] [--tag TEXT]

The still is a Flycast window grab (title bar on top, the game's 1440x1080
between thin side bars); its game area is cropped to 16:9 (--y picks the
band, 0 top to 1 bottom), warmed up a little and vignetted, with the
title-screen logo on top and a ribbon along the bottom."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont
import numpy as np

import cards

W, H = 1280, 720


def game_area(im):
    """The 1440x1080 game picture inside a 1488-wide window grab."""
    w, h = im.size
    side = (w - 1440) // 2
    return im.crop((side + 4, h - 1080 + 4, side + 1436, h - 4))


def background(still, y, zoom=1.0):
    g = game_area(Image.open(still).convert('RGB'))
    cw = int(g.width / zoom)
    band = int(cw * 9 / 16)
    left = (g.width - cw) // 2
    top = int((g.height - band) * y)
    bg = g.crop((left, top, left + cw, top + band)).resize((W, H), Image.LANCZOS)
    bg = ImageEnhance.Color(bg).enhance(1.18)
    bg = ImageEnhance.Contrast(bg).enhance(1.06)
    # Vignette: darken the corners so the lettering stands out.
    yy, xx = np.mgrid[0:H, 0:W]
    r = np.sqrt(((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2) / (H / 2)) ** 2)
    shade = np.clip((r - 0.65) / 0.75, 0, 1) * 0.55
    arr = np.asarray(bg).astype(float) * (1 - shade[..., None])
    return Image.fromarray(arr.astype(np.uint8)).convert('RGBA')


def shadow(layer, blur=10, alpha=0.8, offset=(6, 8)):
    a = layer.split()[3].filter(ImageFilter.GaussianBlur(blur)).point(lambda v: int(v * alpha))
    s = Image.new('RGBA', layer.size, (5, 8, 30, 0))
    s.putalpha(a)
    out = Image.new('RGBA', (layer.width + offset[0] + 2 * blur, layer.height + offset[1] + 2 * blur), (0, 0, 0, 0))
    out.alpha_composite(s, (offset[0] + blur, offset[1] + blur))
    out.alpha_composite(layer, (blur, blur))
    return out


def ribbon(text, size, fill_top, fill_bottom, edge=(255, 255, 255)):
    """A slanted arcade banner with white rims and italic lettering."""
    f = ImageFont.truetype(cards.FONT, size)
    l, t, r, b = f.getbbox(text)
    pad_x, pad_y = int(size * 0.7), int(size * 0.32)
    w, h = r - l + 2 * pad_x, b - t + 2 * pad_y
    slant = int(h * 0.35)
    img = Image.new('RGBA', (w + slant, h), (0, 0, 0, 0))
    grad = Image.new('RGBA', (w + slant, h))
    gd = ImageDraw.Draw(grad)
    for yy in range(h):
        k = yy / max(h - 1, 1)
        gd.line([(0, yy), (w + slant, yy)], fill=tuple(int(a + (c - a) * k) for a, c in zip(fill_top, fill_bottom)) + (255,))
    mask = Image.new('L', img.size, 0)
    ImageDraw.Draw(mask).polygon([(slant, 0), (w + slant, 0), (w, h), (0, h)], fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    rim = max(3, size // 12)
    d.line([(slant, 0), (w + slant, 0)], fill=edge + (255,), width=rim)
    d.line([(0, h - 1), (w, h - 1)], fill=edge + (255,), width=rim)
    d.text((pad_x + slant // 2 - l, pad_y - t), text, font=f, fill=(255, 255, 255, 255),
           stroke_width=max(2, size // 14), stroke_fill=(30, 10, 10, 255))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('still')
    ap.add_argument('out')
    ap.add_argument('--y', type=float, default=1.0)
    ap.add_argument('--zoom', type=float, default=1.0)
    ap.add_argument('--ribbon', default='NEW DREAMCAST KART RACER')
    ap.add_argument('--side', choices=['left', 'right'], default='right')
    ap.add_argument('--tag', default='ONLINE  ·  SPLIT SCREEN  ·  5 TRACKS')
    a = ap.parse_args()
    c = background(a.still, a.y, a.zoom)
    logo = shadow(cards.logo(118))
    c.alpha_composite(logo, ((W - logo.width) // 2, -18))
    rb = shadow(ribbon(a.ribbon, 50, (235, 40, 30), (170, 15, 20)), blur=6, offset=(4, 6))
    x = (lambda img: W - img.width + 10) if a.side == 'right' else (lambda img: -10)
    c.alpha_composite(rb, (x(rb), H - rb.height - 78))
    if a.tag:
        tg = shadow(ribbon(a.tag, 30, (30, 110, 230), (15, 55, 160)), blur=5, offset=(3, 5))
        c.alpha_composite(tg, (x(tg), H - tg.height - 18))
    c.convert('RGB').save(a.out, quality=92)
    print(a.out, Path(a.out).stat().st_size // 1024, 'KiB')


if __name__ == '__main__':
    main()
