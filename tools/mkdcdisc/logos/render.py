#!/usr/bin/env python3
"""Render the MR license-screen logo every CDI built here carries.

The BIOS draws the MR image under the SEGA license line on a (192,192,192)
grey. mkdcdisc accepts a 320x90 PNG with at most 128 colours, and the
RLE-compressed MR must fit in 8 KiB (mkdcdisc only prints an error and
still builds the disc when it does not), so everything here is flat colour.

The picture is the crayon "Gotta go fast" drawing, redrawn flat: a wobbly
blue blob with spikes, wide eyes, red shoes, speed lines.

    uv run --with pillow tools/mkdcdisc/logos/render.py [out-dir]
"""
import math
import random
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

W, H = 320, 90
BG = (192, 192, 192)        # the BIOS license screen, measured in Flycast


def gotta_go_fast():
    rnd = random.Random(7)
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    blue, dark, red, black, white = (52, 78, 196), (30, 44, 140), (214, 40, 36), (20, 20, 20), (246, 246, 246)

    def wobble(cx, cy, rx, ry, n=28, j=1.6):
        pts = []
        for i in range(n):
            a = 2 * math.pi * i / n
            k = 1 + rnd.uniform(-j, j) / 40
            pts.append((cx + rx * k * math.cos(a), cy + ry * k * math.sin(a)))
        return pts

    def scrawl(pts, fill, width):
        d.line(pts, fill=fill, width=width, joint="curve")

    fx, fy = 236, 50          # body centre
    # Spikes behind the body, pointing up and back.
    for ang, ln in [(-150, 30), (-128, 32), (-105, 30), (-82, 26), (-60, 22), (-170, 24)]:
        a = math.radians(ang)
        bx, by = fx + 26 * math.cos(a), fy - 4 + 24 * math.sin(a)
        tip = (bx + ln * math.cos(a), by + ln * math.sin(a))
        side = math.radians(ang + 90)
        d.polygon([(bx + 6 * math.cos(side), by + 6 * math.sin(side)), tip,
                   (bx - 6 * math.cos(side), by - 6 * math.sin(side))], fill=blue, outline=dark)
    # Legs and shoes first so the body overlaps them.
    for lx, dx in [(fx - 10, -4), (fx + 12, 6)]:
        d.line([(lx, fy + 22), (lx + dx, fy + 34)], fill=blue, width=5)
        d.ellipse((lx + dx - 10, fy + 30, lx + dx + 12, fy + 39), fill=red, outline=black)
    # Body.
    body = wobble(fx, fy, 34, 30)
    d.polygon(body, fill=blue)
    scrawl(body + body[:1], dark, 2)
    # Arm reaching back with a little hand.
    scrawl([(fx - 30, fy + 6), (fx - 44, fy + 2), (fx - 54, fy + 8)], blue, 4)
    d.ellipse((fx - 59, fy + 4, fx - 50, fy + 13), fill=blue, outline=dark)
    # Eyes: big and close together, looking where he is going.
    for ex in (fx - 10, fx + 10):
        d.ellipse((ex - 9, fy - 20, ex + 9, fy - 2), fill=white, outline=black, width=2)
        d.ellipse((ex + 1, fy - 15, ex + 7, fy - 9), fill=black)
    # Mouth wide open with a tongue.
    mouth = [(fx - 15, fy + 5), (fx - 9, fy + 15), (fx, fy + 18), (fx + 9, fy + 15), (fx + 15, fy + 4)]
    d.polygon(mouth, fill=white)
    d.chord((fx - 8, fy + 9, fx + 6, fy + 21), 190, 350, fill=red)
    scrawl(mouth + mouth[:1], black, 2)
    # Speed lines.
    for y, x0, x1 in [(34, 128, 172), (46, 118, 176), (58, 134, 178), (70, 122, 170), (82, 140, 176)]:
        pts = [(x, y + rnd.uniform(-1.5, 1.5)) for x in range(x0, x1, 6)]
        scrawl(pts, black, 3)
    # Lettering, each word tilted a touch, like the original.
    f = ImageFont.truetype("/System/Library/Fonts/MarkerFelt.ttc", 26)
    for word, x, y, ang in [("Gotta go", 4, 2, 4), ("fast", 34, 42, -7)]:
        t = Image.new("L", (160, 50), 0)
        ImageDraw.Draw(t).text((4, 4), word, font=f, fill=255)
        t = t.rotate(ang, resample=Image.Resampling.NEAREST, expand=True)
        im.paste(black, (x, y), t.point(lambda v: 255 if v > 110 else 0))
    return im.quantize(colors=32, dither=Image.Dither.NONE).convert("RGB")


def mr_size(im):
    """Size of the MR file mkdcdisc will write: 30-byte header, 4 bytes per
    palette entry, then makeip's RLE (a lone byte costs 1, a run of 2-127
    costs 2, longer runs 3)."""
    raw = im.tobytes()
    px = [raw[i:i + 3] for i in range(0, len(raw), 3)]
    pal = {}
    idx = [pal.setdefault(c, len(pal)) for c in px]
    n, i = 30 + 4 * len(pal), 0
    while i < len(idx):
        run = 1
        while i + run < len(idx) and idx[i + run] == idx[i] and run < 0x17f:
            run += 1
        n += 1 if run == 1 else 2 if run < 0x80 else 3
        i += run
    return n


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent
    out.mkdir(parents=True, exist_ok=True)
    im = gotta_go_fast()
    im.save(out / "gotta-go-fast.png")
    print(f"gotta-go-fast: {len(im.getcolors(1 << 16))} colours, MR {mr_size(im)} bytes (limit 8192)")


if __name__ == "__main__":
    main()
