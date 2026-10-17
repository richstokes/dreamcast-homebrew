#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "pillow>=10.1", "matplotlib>=3.8"]
# ///
"""Draw DreamKarts' VMU art: the save file icon and the 48x32 LCD sprites.

The save icon is two 32x32 16-colour frames (the BIOS memory manager
animates them). The LCD art is 1-bit: hand-placed pixel sprites, a 3x5
pixel font and big digits and logo lettering rendered from the HUD's DejaVu
Sans Bold Oblique (Bitstream Vera licence). Writes to --output-dir:

    vmu_assets.h        icon palette and frames, LCD sprites and font
    previews/vmu_*.png  the icon frames and every sprite (git-ignored)
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def font_path(name: str) -> str:
    import matplotlib
    return os.path.join(os.path.dirname(matplotlib.__file__), "mpl-data", "fonts", "ttf", name)


def art(rows: list[str]) -> np.ndarray:
    return np.array([[c == "#" for c in r] for r in rows])


# --------------------------------------------------------------------------
# Save icon: a Chao in a red kart, emote ball bobbing and wheels turning.

ICON_KEYS = ".krRwgbByYsl"
ICON_PAL = [   # ARGB4444, in ICON_KEYS order; '.' is see-through
    0x0000, 0xF112, 0xFE33, 0xFA12, 0xFFFF, 0xF999, 0xF7BF, 0xF47D, 0xFFE3, 0xFDA1, 0x6000, 0xFFAA,
]


def icon_frame(f: int) -> list[str]:
    g = [["."] * 32 for _ in range(32)]

    def put(x, y, c):
        if 0 <= x < 32 and 0 <= y < 32:
            g[y][x] = c

    def inside(x, y, cx, cy, rx, ry):
        return ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2 <= 1

    def ell(cx, cy, rx, ry, shade, outline="k"):
        for y in range(32):
            for x in range(32):
                if not inside(x, y, cx, cy, rx, ry):
                    continue
                edge = any(not inside(xx, yy, cx, cy, rx, ry)
                           for xx, yy in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)))
                put(x, y, outline if edge and outline else shade(x, y))

    # Shadow on the ground.
    ell(16, 29.5, 15, 2.2, lambda x, y: "s", None)
    # Kart body: lit from the top left.
    ell(16, 19.5, 14, 4.5, lambda x, y: "r" if y < 20 else "R")
    for x in range(6, 10):
        put(x, 17, "w")
    put(5, 18, "w")
    # Exhaust pipe.
    for y in (15, 16):
        for x in (26, 27, 28):
            put(x, y, "g")
    put(28, 15, "w")
    # Chao: head shaded to the bottom right, eyes with a shine, cheeks.
    ell(16, 10.5, 6, 5, lambda x, y: "B" if (x - 16) + (y - 10.5) * 1.2 > 4.5 else "b")
    for ex in (13, 18):
        put(ex, 9, "w")
        put(ex, 10, "k")
    put(12, 11, "l")
    put(19, 11, "l")
    for x in (15, 16):
        put(x, 12, "k")
    # Emote ball floats up and down.
    by = 3 - f
    ell(16, by, 2.2, 2.2, lambda x, y: "Y" if x >= 16 and y >= by else "y", None)
    put(15, by - 1, "w")
    # Wheels: tyres and hubs whose bright spoke turns.
    for cx in (8.5, 23.5):
        ell(cx, 24.5, 4, 4, lambda x, y: "k", None)
        ell(cx, 24.5, 1.8, 1.8, lambda x, y: "g", None)
        hx, hy = int(cx), 24
        spokes = [(0, 0), (-1, 0), (0, -1), (-1, -1)]
        sx, sy = spokes[f * 3 % 4] if cx < 16 else spokes[(f * 3 + 1) % 4]
        put(hx + sx, hy + sy, "w")
    return ["".join(r) for r in g]


def icon_bytes(rows: list[str]) -> bytes:
    out = bytearray()
    for r in rows:
        for x in range(0, 32, 2):
            out.append(ICON_KEYS.index(r[x]) << 4 | ICON_KEYS.index(r[x + 1]))
    return bytes(out)


# --------------------------------------------------------------------------
# LCD sprites (1 bit, 48x32 screen).

def text_bits(s: str, size: int, thr: int = 110, ss: int = 4) -> np.ndarray:
    f = ImageFont.truetype(font_path("DejaVuSans-BoldOblique.ttf"), size * ss)
    w = int(f.getlength(s)) + size * ss
    big = Image.new("L", (w, size * ss * 2), 0)
    ImageDraw.Draw(big).text((size * ss // 2, 0), s, font=f, fill=255)
    a = np.array(big.resize((w // ss, size * 2), Image.BOX)) > thr
    ys, xs = np.nonzero(a)
    return a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


FONT = {
    "A": [".#.", "#.#", "###", "#.#", "#.#"], "B": ["##.", "#.#", "##.", "#.#", "##."],
    "C": [".##", "#..", "#..", "#..", ".##"], "D": ["##.", "#.#", "#.#", "#.#", "##."],
    "E": ["###", "#..", "##.", "#..", "###"], "F": ["###", "#..", "##.", "#..", "#.."],
    "G": [".##", "#..", "#.#", "#.#", ".##"], "H": ["#.#", "#.#", "###", "#.#", "#.#"],
    "I": ["###", ".#.", ".#.", ".#.", "###"], "J": ["..#", "..#", "..#", "#.#", ".#."],
    "K": ["#.#", "#.#", "##.", "#.#", "#.#"], "L": ["#..", "#..", "#..", "#..", "###"],
    "M": ["#...#", "##.##", "#.#.#", "#...#", "#...#"], "N": ["#..#", "##.#", "#.##", "#..#", "#..#"],
    "O": [".#.", "#.#", "#.#", "#.#", ".#."], "P": ["##.", "#.#", "##.", "#..", "#.."],
    "Q": [".#.", "#.#", "#.#", "##.", ".##"], "R": ["##.", "#.#", "##.", "#.#", "#.#"],
    "S": [".##", "#..", ".#.", "..#", "##."], "T": ["###", ".#.", ".#.", ".#.", ".#."],
    "U": ["#.#", "#.#", "#.#", "#.#", "###"], "V": ["#.#", "#.#", "#.#", "#.#", ".#."],
    "W": ["#...#", "#...#", "#.#.#", "##.##", "#...#"], "X": ["#.#", "#.#", ".#.", "#.#", "#.#"],
    "Y": ["#.#", "#.#", ".#.", ".#.", ".#."], "Z": ["###", "..#", ".#.", "#..", "###"],
    "0": ["###", "#.#", "#.#", "#.#", "###"], "1": [".#.", "##.", ".#.", ".#.", "###"],
    "2": ["##.", "..#", ".#.", "#..", "###"], "3": ["##.", "..#", ".#.", "..#", "##."],
    "4": ["#.#", "#.#", "###", "..#", "..#"], "5": ["###", "#..", "##.", "..#", "##."],
    "6": [".##", "#..", "###", "#.#", "###"], "7": ["###", "..#", ".#.", ".#.", ".#."],
    "8": ["###", "#.#", "###", "#.#", "###"], "9": ["###", "#.#", "###", "..#", "##."],
    ":": [".", "#", ".", "#", "."], ".": [".", ".", ".", ".", "#"], "/": ["..#", "..#", ".#.", "#..", "#.."],
    "!": ["#", "#", "#", ".", "#"], "-": ["...", "...", "###", "...", "..."], "'": ["#", "#", ".", ".", "."],
    "\"": ["#.#", "#.#", "...", "...", "..."], " ": ["..", "..", "..", "..", ".."],
}

KART = art([
    "...........##...............",
    "..........####..............",
    "...........##...............",
    ".........######.............",
    "........########............",
    ".......##########...........",
    ".......#######.###....##....",
    ".......#######.###...##.....",
    "###.....########....##......",
    "###......######....##.......",
    ".#.....##########.##........",
    ".##########################.",
    "############################",
    ".##########################.",
    "............................",
    "............................",
    "............................",
    "............................",
    "............................",
])
WHEELS = [
    art(["..###..", ".#####.", "###.###", "##...##", "###.###", ".#####.", "..###.."]),
    art(["..###..", ".#####.", "##.#.##", "###.###", "##.#.##", ".#####.", "..###.."]),
]


def kart(f: int) -> np.ndarray:
    k = KART.copy()
    for x0 in (3, 19):
        for j in range(0, 8):
            for i in range(-1, 8):
                y, x = 12 + j, x0 + i
                if y < k.shape[0] and 0 <= x < k.shape[1] and (i - 3) ** 2 + (j - 3) ** 2 <= 4.2 ** 2:
                    k[y, x] = False
        k[12:19, x0:x0 + 7] |= WHEELS[f]
    return k


SPRITES_ART = {
    "MINI": ["....##......", "...####.....", "...####..#..", "############", ".##......##.", ".##......##."],
    "TROPHY": [
        "##############", "##############", "#.##########.#", "#.##########.#", "#.##########.#",
        ".#.########.#.", "..#.######.#..", "....######....", ".....####.....", "......##......",
        "......##......", ".....####.....", "....######....", "....######....",
    ],
    "FLAG": [
        "#.##..##..##..", "#.##..##..##..", "#...##..##..##", "#...##..##..##", "#.##..##..##..",
        "#.##..##..##..", "#...##..##..##", "#...##..##..##", "#.............", "#.............",
        "#.............", "#.............", "#.............", "#.............",
    ],
    "PALM": [
        ".###.###.", "#...#...#", "...###...", "..#.#.#..", "....#....", "....#....",
        "...#.....", "...#.....", "...#.....", "...#.....", "..##.....",
    ],
    "PAUSE": ["#####...#####"] * 15,
}


def sprites() -> dict[str, np.ndarray]:
    s = {name: art(rows) for name, rows in SPRITES_ART.items()}
    s["KART0"], s["KART1"] = kart(0), kart(1)
    s["DREAM"], s["KARTS"] = text_bits("DREAM", 11), text_bits("KARTS", 11)
    s["ONLINE"] = text_bits("ONLINE", 11)
    s["GO"] = text_bits("GO!", 22)
    for d in "0123456789":
        s["BIG" + d] = text_bits(d, 28)
    for name, a in s.items():
        assert a.shape[1] <= 64 and a.shape[0] <= 32, name
    return s


# --------------------------------------------------------------------------
# Output

def c_bytes(data: bytes, indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(data), 16):
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    return "\n".join(lines)


def row_bits(row: np.ndarray) -> int:
    v = 0
    for i, on in enumerate(row):
        if on:
            v |= 1 << (63 - i)
    return v


def write_header(path: Path, icons: list[bytes], spr: dict[str, np.ndarray]) -> None:
    out = ["/* Generated by tools/build_vmu.py; do not edit. */",
           "#ifndef DK_VMU_ASSETS_H", "#define DK_VMU_ASSETS_H", "", "#include <stdint.h>", "",
           f"#define DK_VMU_ICON_FRAMES {len(icons)}",
           "static const uint16_t dk_vmu_icon_pal[16] = {",
           "    " + ", ".join(f"0x{c:04x}" for c in ICON_PAL + [0] * (16 - len(ICON_PAL))) + ",", "};",
           f"static const uint8_t dk_vmu_icon[{len(icons)}][512] = {{"]
    for b in icons:
        out += ["{", c_bytes(b), "},"]
    out += ["};", "",
            "/* 1-bit sprites: rows top to bottom, bit 63 is the leftmost pixel. */",
            "typedef struct { uint8_t w, h; const uint64_t *rows; } LcdSprite;", ""]
    names = list(spr)
    for name in names:
        a = spr[name]
        out.append(f"static const uint64_t lcd_rows_{name.lower()}[{a.shape[0]}] = {{")
        for r in a:
            out.append(f"    0x{row_bits(r):016x}ull,")
        out.append("};")
    out.append("enum {")
    out += [f"    LCD_{n}," for n in names]
    out += ["    LCD_SPRITE_COUNT", "};", "static const LcdSprite lcd_sprites[LCD_SPRITE_COUNT] = {"]
    out += [f"    {{{spr[n].shape[1]}, {spr[n].shape[0]}, lcd_rows_{n.lower()}}}," for n in names]
    out += ["};", "",
            "/* 3x5 pixel font for ' '..'Z': widths, then rows (bit 7 leftmost). */",
            "static const uint8_t lcd_font_w[59] = {"]
    chars = [chr(c) for c in range(32, 91)]
    out.append("    " + ", ".join(str(len(FONT[c][0])) if c in FONT else "0" for c in chars) + ",")
    out += ["};", "static const uint8_t lcd_font[59][5] = {"]
    for c in chars:
        rows = FONT.get(c, ["", "", "", "", ""])
        vals = [sum(0x80 >> i for i, ch in enumerate(r) if ch == "#") for r in rows]
        out.append("    {" + ", ".join(f"0x{v:02x}" for v in vals) + "},")
    out += ["};", "", "#endif", ""]
    path.write_text("\n".join(out))


def preview(path: Path, imgs: list[np.ndarray], scale: int = 6) -> None:
    on, off = np.array((32, 52, 40), np.uint8), np.array((176, 204, 168), np.uint8)
    w = sum(a.shape[1] * scale + 8 for a in imgs) + 8
    h = max(a.shape[0] for a in imgs) * scale + 16
    out = Image.new("RGB", (w, h), (60, 60, 60))
    x = 8
    for a in imgs:
        im = Image.fromarray(np.where(a[..., None], on, off).astype(np.uint8))
        out.paste(im.resize((a.shape[1] * scale, a.shape[0] * scale), Image.NEAREST), (x, 8))
        x += a.shape[1] * scale + 8
    out.save(path)


def icon_preview(path: Path, frames: list[list[str]], scale: int = 8) -> None:
    out = Image.new("RGBA", (len(frames) * (32 * scale + 16) + 16, 32 * scale + 32), (40, 60, 110, 255))
    for i, rows in enumerate(frames):
        im = Image.new("RGBA", (32, 32))
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                v = ICON_PAL[ICON_KEYS.index(c)]
                a, rr, gg, bb = (v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15
                im.putpixel((x, y), (rr * 17, gg * 17, bb * 17, a * 17))
        big = im.resize((32 * scale, 32 * scale), Image.NEAREST)
        out.alpha_composite(big, (16 + i * (32 * scale + 16), 16))
    out.save(path)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--output-dir", type=Path, default=Path("assets/generated"))
    a = ap.parse_args()
    frames = [icon_frame(f) for f in range(2)]
    spr = sprites()
    write_header(a.output_dir / "vmu_assets.h", [icon_bytes(f) for f in frames], spr)
    prev = a.output_dir / "previews"
    prev.mkdir(parents=True, exist_ok=True)
    icon_preview(prev / "vmu_icon.png", frames)
    preview(prev / "vmu_sprites.png", list(spr.values()))
    print(f"wrote {a.output_dir / 'vmu_assets.h'} ({len(spr)} sprites, {len(frames)} icon frames)")


if __name__ == "__main__":
    main()
