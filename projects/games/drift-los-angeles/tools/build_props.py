#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Author Drift Los Angeles's textured street furniture: atlas and rig meshes.

Run ``uv run tools/build_props.py --atlas assets/source/prop-atlas.png
--header prop_data.h``.  Each prop is a compact single-part rig (parking
meter, newspaper box, bench, litter bin, hydrant, bollard, bus shelter, kiosk,
mailbox, dumpster, cafe table and chair) mapped onto painted atlas regions:
galvanised and painted metal, wood slats, dark glass, a meter face, a
newspaper window, a magazine rack and perforated mesh.  Painted-metal parts
are neutral so the game can tint them with district colours.  The meshes are
recorded into the static block cache, so they cost nothing per frame beyond
their triangles.
"""

from __future__ import annotations

import argparse
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw
from rig_export import Part, fix_winding, normalize, write_obj, write_rig_header

ATLAS = 256
INSET = 1.5 / ATLAS

REGIONS = {
    "metal_dark": (0, 0, 64, 64),
    "metal_paint": (64, 0, 64, 64),
    "wood": (128, 0, 64, 64),
    "glass": (192, 0, 64, 64),
    "meter_face": (0, 64, 32, 32),
    "coin_slot": (32, 64, 32, 32),
    "news_front": (64, 64, 64, 64),
    "mesh": (128, 64, 64, 64),
    "hydrant": (192, 64, 32, 64),
    "mailbox": (224, 64, 32, 64),
    "kiosk_panel": (0, 128, 128, 64),
    "kiosk_awning": (128, 128, 64, 32),
    "dumpster": (192, 128, 64, 64),
    "concrete": (128, 160, 64, 32),
    "sign_face": (0, 192, 64, 64),
    "table_top": (64, 192, 64, 64),
    "chair": (128, 192, 64, 64),
    "shelter_ad": (192, 192, 64, 64),
}

KINDS = ["meter", "newsbox", "bench", "bin", "hydrant", "bollard", "shelter", "kiosk",
         "mailbox", "dumpster", "table", "chair", "planter", "sign"]
PART_ENUM = ["BODY"]
SLOT_ENUM = ["FIXED", "TINT"]


def clamp8(v):
    return max(0, min(255, int(round(v))))


def scale_rgb(c, f):
    return tuple(clamp8(x * f) for x in c)


def fill(img, box, base, seed, noise=.06, streak=0.0, vertical=False):
    x0, y0, w, h = box
    px = img.load()
    rng = random.Random(seed)
    lines = [1.0 + (rng.random() - .5) * streak for _ in range(max(w, h))]
    for y in range(h):
        for x in range(w):
            f = 1.0 + (rng.random() - .5) * noise + (lines[y] if vertical else lines[x]) - 1.0
            px[x0 + x, y0 + y] = scale_rgb(base, f)


def paint_atlas() -> Image.Image:
    img = Image.new("RGB", (ATLAS, ATLAS), (120, 120, 120))
    d = ImageDraw.Draw(img)
    # Galvanised metal: cool grey with brushed streaks and a few rivets.
    fill(img, REGIONS["metal_dark"], (96, 100, 108), 1, .05, .10)
    x0, y0, w, h = REGIONS["metal_dark"]
    for rx, ry in ((8, 8), (56, 8), (8, 56), (56, 56)):
        d.ellipse((x0 + rx - 2, y0 + ry - 2, x0 + rx + 2, y0 + ry + 2), fill=(150, 154, 160), outline=(60, 62, 66))
    # Painted metal: light neutral with scratches, tinted by district.
    fill(img, REGIONS["metal_paint"], (232, 232, 232), 2, .05, .06)
    x0, y0, w, h = REGIONS["metal_paint"]
    rng = random.Random(3)
    for _ in range(14):
        sx, sy = rng.randrange(4, 60), rng.randrange(4, 60)
        d.line((sx + x0, sy + y0, sx + x0 + rng.randrange(-8, 8), sy + y0 + rng.randrange(-3, 3)), fill=(170, 170, 172))
    d.rectangle((x0, y0 + 58, x0 + 63, y0 + 63), fill=(150, 150, 150))
    # Wood slats.
    fill(img, REGIONS["wood"], (150, 104, 62), 4, .08, .16, vertical=True)
    x0, y0, w, h = REGIONS["wood"]
    for slat in range(0, 64, 16):
        d.line((x0, y0 + slat, x0 + 63, y0 + slat), fill=(70, 46, 26), width=2)
        d.line((x0, y0 + slat + 3, x0 + 63, y0 + slat + 3), fill=(190, 140, 90))
    # Dark glass with a sky reflection band.
    fill(img, REGIONS["glass"], (30, 40, 58), 5, .04)
    x0, y0, w, h = REGIONS["glass"]
    d.polygon(((x0, y0 + 14), (x0 + 63, y0 + 4), (x0 + 63, y0 + 16), (x0, y0 + 28)), fill=(90, 110, 140))
    d.rectangle((x0 + 30, y0, x0 + 33, y0 + 63), fill=(18, 22, 30))
    # Meter face and coin slot.
    x0, y0, w, h = REGIONS["meter_face"]
    d.rectangle((x0, y0, x0 + 31, y0 + 31), fill=(112, 112, 116))
    d.rounded_rectangle((x0 + 4, y0 + 3, x0 + 27, y0 + 21), 4, fill=(228, 226, 210), outline=(40, 40, 42))
    d.arc((x0 + 7, y0 + 6, x0 + 24, y0 + 22), 200, 340, fill=(190, 40, 30), width=3)
    d.line((x0 + 15, y0 + 14, x0 + 21, y0 + 9), fill=(30, 30, 30), width=2)
    d.rectangle((x0 + 10, y0 + 24, x0 + 21, y0 + 27), fill=(40, 40, 42))
    x0, y0, w, h = REGIONS["coin_slot"]
    d.rectangle((x0, y0, x0 + 31, y0 + 31), fill=(112, 112, 116))
    d.rectangle((x0 + 12, y0 + 8, x0 + 19, y0 + 24), fill=(40, 40, 42))
    d.ellipse((x0 + 9, y0 + 26, x0 + 22, y0 + 31), fill=(60, 60, 64))
    # Newspaper box front: window with folded papers.
    x0, y0, w, h = REGIONS["news_front"]
    fill(img, REGIONS["news_front"], (232, 232, 232), 6, .04)
    d.rectangle((x0 + 8, y0 + 6, x0 + 55, y0 + 40), fill=(24, 28, 36))
    for row in range(3):
        d.rectangle((x0 + 12, y0 + 10 + row * 10, x0 + 51, y0 + 16 + row * 10), fill=(214, 210, 196))
        d.line((x0 + 15, y0 + 12 + row * 10, x0 + 40, y0 + 12 + row * 10), fill=(60, 60, 60), width=2)
    d.rectangle((x0 + 22, y0 + 46, x0 + 41, y0 + 52), fill=(60, 60, 64))
    d.rectangle((x0, y0 + 58, x0 + 63, y0 + 63), fill=(150, 150, 150))
    # Perforated mesh for litter bins.
    x0, y0, w, h = REGIONS["mesh"]
    fill(img, REGIONS["mesh"], (76, 80, 84), 7, .04)
    for yy in range(4, 64, 8):
        for xx in range(4 + (yy // 8 % 2) * 4, 64, 8):
            d.ellipse((x0 + xx - 2, y0 + yy - 2, x0 + xx + 2, y0 + yy + 2), fill=(26, 28, 30))
    d.rectangle((x0, y0, x0 + 63, y0 + 5), fill=(120, 124, 130))
    # Hydrant: bright red rings; mailbox: blue with a slot.
    x0, y0, w, h = REGIONS["hydrant"]
    fill(img, REGIONS["hydrant"], (196, 34, 30), 8, .06, .05)
    for ring in (8, 30, 52):
        d.rectangle((x0, y0 + ring, x0 + 31, y0 + ring + 3), fill=(120, 20, 18))
    d.rectangle((x0, y0, x0 + 31, y0 + 3), fill=(230, 60, 50))
    x0, y0, w, h = REGIONS["mailbox"]
    fill(img, REGIONS["mailbox"], (36, 62, 138), 9, .05, .05)
    d.rectangle((x0 + 6, y0 + 10, x0 + 25, y0 + 14), fill=(14, 20, 40))
    d.rectangle((x0 + 4, y0 + 20, x0 + 27, y0 + 34), fill=(220, 222, 226))
    d.line((x0 + 8, y0 + 25, x0 + 23, y0 + 25), fill=(60, 60, 70))
    d.line((x0 + 8, y0 + 29, x0 + 20, y0 + 29), fill=(60, 60, 70))
    # Kiosk panel: magazine rack and stacked covers; awning stripes.
    x0, y0, w, h = REGIONS["kiosk_panel"]
    fill(img, REGIONS["kiosk_panel"], (58, 40, 30), 10, .05)
    rng = random.Random(11)
    for row in range(3):
        for col in range(8):
            cx, cy = x0 + 4 + col * 15, y0 + 4 + row * 20
            colour = (rng.randrange(120, 250), rng.randrange(60, 220), rng.randrange(40, 200))
            d.rectangle((cx, cy, cx + 12, cy + 16), fill=colour)
            d.rectangle((cx + 2, cy + 2, cx + 10, cy + 6), fill=(245, 245, 240))
            d.line((cx + 2, cy + 10, cx + 9, cy + 10), fill=(30, 30, 30))
        d.line((x0, y0 + 20 + row * 20, x0 + 127, y0 + 20 + row * 20), fill=(150, 120, 90), width=2)
    x0, y0, w, h = REGIONS["kiosk_awning"]
    for stripe in range(0, 64, 8):
        d.rectangle((x0 + stripe, y0, x0 + stripe + 7, y0 + 31), fill=(232, 232, 232) if (stripe // 8) % 2 else (200, 40, 40))
    d.rectangle((x0, y0 + 26, x0 + 63, y0 + 31), fill=(150, 150, 150))
    # Dumpster: painted steel with lid seam and stencil; concrete.
    x0, y0, w, h = REGIONS["dumpster"]
    fill(img, REGIONS["dumpster"], (232, 232, 232), 12, .06, .08)
    d.line((x0, y0 + 20, x0 + 63, y0 + 20), fill=(110, 110, 110), width=2)
    d.rectangle((x0 + 18, y0 + 30, x0 + 45, y0 + 46), outline=(90, 90, 90), width=2)
    d.rectangle((x0, y0 + 58, x0 + 63, y0 + 63), fill=(110, 110, 110))
    fill(img, REGIONS["concrete"], (168, 166, 158), 13, .10)
    # Street sign face, cafe table top, chair, shelter ad.
    x0, y0, w, h = REGIONS["sign_face"]
    d.rectangle((x0, y0, x0 + 63, y0 + 63), fill=(24, 90, 48))
    d.rectangle((x0 + 4, y0 + 22, x0 + 59, y0 + 42), fill=(240, 240, 240))
    for glyph in range(6):
        d.rectangle((x0 + 9 + glyph * 8, y0 + 27, x0 + 13 + glyph * 8, y0 + 37), fill=(24, 90, 48))
    x0, y0, w, h = REGIONS["table_top"]
    fill(img, REGIONS["table_top"], (236, 236, 236), 14, .04)
    d.ellipse((x0 + 2, y0 + 2, x0 + 61, y0 + 61), outline=(150, 150, 150), width=2)
    d.line((x0 + 32, y0 + 4, x0 + 32, y0 + 59), fill=(200, 200, 200))
    d.line((x0 + 4, y0 + 32, x0 + 59, y0 + 32), fill=(200, 200, 200))
    x0, y0, w, h = REGIONS["chair"]
    fill(img, REGIONS["chair"], (232, 232, 232), 15, .05)
    for bar in range(8, 64, 12):
        d.line((x0 + bar, y0, x0 + bar, y0 + 63), fill=(140, 140, 140), width=3)
    x0, y0, w, h = REGIONS["shelter_ad"]
    d.rectangle((x0, y0, x0 + 63, y0 + 63), fill=(250, 120, 40))
    d.ellipse((x0 + 14, y0 + 8, x0 + 50, y0 + 44), fill=(255, 230, 120))
    d.rectangle((x0 + 8, y0 + 50, x0 + 55, y0 + 58), fill=(30, 20, 60))
    return img


# ---------------------------------------------------------------------------
# Mesh helpers
# ---------------------------------------------------------------------------
def region_uv(region, u, v):
    x0, y0, w, h = REGIONS[region]
    u = min(max(u, 0.0), 1.0)
    v = min(max(v, 0.0), 1.0)
    return ((x0 + INSET * ATLAS + u * (w - 2 * INSET * ATLAS)) / ATLAS,
            (y0 + INSET * ATLAS + v * (h - 2 * INSET * ATLAS)) / ATLAS)


def box(part: Part, lo, hi, regions, skip=()):
    """Axis-aligned box; ``regions`` maps face names (top,bottom,front,back,left,right)
    to atlas regions, or a single region name for all."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    c = {"lbb": (x0, y0, z0), "rbb": (x1, y0, z0), "lbf": (x0, y0, z1), "rbf": (x1, y0, z1),
         "ltb": (x0, y1, z0), "rtb": (x1, y1, z0), "ltf": (x0, y1, z1), "rtf": (x1, y1, z1)}
    faces = {
        "top": (("ltb", "rtb", "rtf", "ltf"), (0, 1, 0)),
        "bottom": (("lbf", "rbf", "rbb", "lbb"), (0, -1, 0)),
        "front": (("ltf", "rtf", "rbf", "lbf"), (0, 0, 1)),
        "back": (("rtb", "ltb", "lbb", "rbb"), (0, 0, -1)),
        "left": (("ltb", "ltf", "lbf", "lbb"), (-1, 0, 0)),
        "right": (("rtf", "rtb", "rbb", "rbf"), (1, 0, 0)),
    }
    for name, (corners, normal) in faces.items():
        if name in skip:
            continue
        region = regions if isinstance(regions, str) else regions.get(name, regions.get("*", "metal_dark"))
        base = len(part.vertices)
        for corner, (u, v) in zip(corners, ((0, 0), (1, 0), (1, 1), (0, 1))):
            p = c[corner]
            part.add_vertex(p, region_uv(region, u, v), normal)
        part.faces.append((base, base + 1, base + 2))
        part.faces.append((base, base + 2, base + 3))


def cylinder(part: Part, cx, cz, y0, y1, r0, r1, segments, region, cap_top=True, v0=0.0, v1=1.0):
    ring0 = len(part.vertices)
    for i in range(segments + 1):
        t = i / segments
        a = 2 * math.pi * t
        n = normalize((math.sin(a), 0.0, math.cos(a)))
        part.add_vertex((cx + math.sin(a) * r0, y0, cz + math.cos(a) * r0), region_uv(region, t, v1), n)
        part.add_vertex((cx + math.sin(a) * r1, y1, cz + math.cos(a) * r1), region_uv(region, t, v0), n)
    for i in range(segments):
        a0, b0 = ring0 + i * 2, ring0 + i * 2 + 1
        a1, b1 = ring0 + (i + 1) * 2, ring0 + (i + 1) * 2 + 1
        part.faces.append((a0, b0, b1))
        part.faces.append((a0, b1, a1))
    if cap_top:
        centre = part.add_vertex((cx, y1, cz), region_uv(region, .5, .5), (0, 1, 0))
        first = len(part.vertices)
        for i in range(segments):
            a = 2 * math.pi * i / segments
            part.add_vertex((cx + math.sin(a) * r1, y1, cz + math.cos(a) * r1),
                            region_uv(region, .5 + math.sin(a) * .45, .5 + math.cos(a) * .45), (0, 1, 0))
        for i in range(segments):
            part.faces.append((centre, first + i, first + (i + 1) % segments))


def build_prop(kind: str) -> Part:
    part = Part("body", -1, (0.0, 0.0, 0.0), "metal_dark", "fixed")
    if kind == "meter":
        cylinder(part, 0, 0, 0, 1.05, .028, .028, 6, "metal_dark", cap_top=False)
        box(part, (-.085, 1.05, -.07), (.085, 1.36, .07),
            {"front": "meter_face", "back": "coin_slot", "*": "metal_dark"})
        box(part, (-.06, 1.36, -.05), (.06, 1.42, .05), "metal_dark", skip=("bottom",))
    elif kind == "newsbox":
        part.slot = "tint"
        box(part, (-.24, .08, -.20), (.24, .98, .20),
            {"front": "news_front", "*": "metal_paint"}, skip=("bottom",))
        box(part, (-.20, 0, -.16), (.20, .08, .16), "metal_dark", skip=("bottom", "top"))
    elif kind == "bench":
        box(part, (-.85, .42, -.22), (.85, .48, .22), "wood", skip=("bottom",))
        box(part, (-.85, .50, -.30), (.85, .90, -.24), "wood", skip=("bottom",))
        for x in (-.72, .72):
            box(part, (x - .03, 0, -.28), (x + .03, .42, .22), "metal_dark", skip=("bottom", "top"))
            box(part, (x - .03, .42, -.30), (x + .03, .92, -.24), "metal_dark", skip=("bottom",))
    elif kind == "bin":
        cylinder(part, 0, 0, 0, .86, .27, .30, 8, "mesh", cap_top=False)
        cylinder(part, 0, 0, .86, .98, .31, .29, 8, "metal_dark", cap_top=True)
    elif kind == "hydrant":
        cylinder(part, 0, 0, 0, .12, .16, .16, 8, "hydrant", cap_top=False, v0=.85, v1=1.0)
        cylinder(part, 0, 0, .12, .62, .12, .12, 8, "hydrant", cap_top=False, v0=.15, v1=.85)
        cylinder(part, 0, 0, .62, .76, .13, .06, 8, "hydrant", cap_top=True, v0=0, v1=.15)
        box(part, (-.22, .34, -.05), (.22, .44, .05), "hydrant")
        box(part, (-.05, .30, .12), (.05, .40, .22), "hydrant")
    elif kind == "bollard":
        cylinder(part, 0, 0, 0, .85, .11, .10, 8, "metal_dark", cap_top=True)
    elif kind == "shelter":
        for x in (-1.6, 1.6):
            box(part, (x - .04, 0, -.76), (x + .04, 2.5, -.68), "metal_dark", skip=("bottom",))
        box(part, (-1.9, 2.5, -.95), (1.9, 2.62, .85), {"top": "metal_paint", "*": "metal_dark"})
        box(part, (-1.72, .30, -.74), (1.72, 2.5, -.70), "glass", skip=("bottom",))
        box(part, (-1.72, 1.10, -.70), (-1.1, 2.3, -.66), "shelter_ad", skip=("bottom",))
        box(part, (-1.1, .48, -.62), (1.4, .54, -.28), "wood", skip=("bottom",))
    elif kind == "kiosk":
        part.slot = "tint"
        box(part, (-1.1, 0, -.75), (1.1, 2.15, .75),
            {"front": "kiosk_panel", "left": "kiosk_panel", "right": "kiosk_panel", "*": "metal_paint"},
            skip=("bottom",))
        box(part, (-1.2, 2.15, -.85), (1.2, 2.32, .85), {"top": "metal_dark", "*": "kiosk_awning"})
        box(part, (-1.25, 1.95, .75), (1.25, 2.05, 1.30), {"*": "kiosk_awning"}, skip=("back",))
    elif kind == "mailbox":
        box(part, (-.26, .72, -.22), (.26, 1.20, .22), "mailbox", skip=("bottom",))
        box(part, (-.20, 1.20, -.16), (.20, 1.30, .16), "mailbox", skip=("bottom",))
        for x in (-.18, .18):
            box(part, (x - .03, 0, -.03), (x + .03, .72, .03), "metal_dark", skip=("bottom", "top"))
    elif kind == "dumpster":
        part.slot = "tint"
        box(part, (-.95, .18, -.62), (.95, 1.18, .62), "dumpster", skip=("bottom",))
        box(part, (-.98, 1.18, -.64), (.98, 1.30, .0), "dumpster")
        box(part, (-.98, 1.18, .0), (.98, 1.36, .64), "dumpster")
        for x in (-.75, .75):
            box(part, (x - .08, 0, -.55), (x + .08, .18, -.40), "metal_dark", skip=("bottom",))
            box(part, (x - .08, 0, .40), (x + .08, .18, .55), "metal_dark", skip=("bottom",))
    elif kind == "table":
        cylinder(part, 0, 0, 0, .04, .28, .28, 8, "metal_dark", cap_top=True)
        cylinder(part, 0, 0, .04, .72, .03, .03, 6, "metal_dark", cap_top=False)
        cylinder(part, 0, 0, .72, .76, .42, .42, 8, "table_top", cap_top=True)
    elif kind == "chair":
        part.slot = "tint"
        box(part, (-.22, .42, -.22), (.22, .46, .22), "chair", skip=("bottom",))
        box(part, (-.22, .46, -.24), (.22, .90, -.20), "chair", skip=("bottom",))
        for x, z in ((-.18, -.18), (.18, -.18), (-.18, .18), (.18, .18)):
            box(part, (x - .02, 0, z - .02), (x + .02, .42, z + .02), "metal_dark", skip=("bottom", "top"))
    elif kind == "planter":
        box(part, (-.55, 0, -.55), (.55, .55, .55), "concrete", skip=("bottom",))
        cylinder(part, 0, 0, .55, .62, .40, .40, 6, "wood", cap_top=True)
    elif kind == "sign":
        cylinder(part, 0, 0, 0, 2.4, .03, .03, 6, "metal_dark", cap_top=False)
        box(part, (-.02, 2.05, -.42), (.02, 2.35, .42), {"left": "sign_face", "right": "sign_face", "*": "metal_dark"})
    fix_winding(part)
    return part


def write_preview(directory: Path, atlas: Image.Image, meshes: dict[str, list[Part]]) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    atlas.save(directory / "props.png")
    column = 0
    for kind in KINDS:
        parts = meshes[f"dla_prop_{kind}"]
        yaw = .6
        c, s = math.cos(yaw), math.sin(yaw)
        world = []
        for part in parts:
            for v in part.vertices:
                x, y, z = v[0], v[1], v[2]
                world.append((x * c + z * s, y, -x * s + z * c))
        (directory / f"variant{column}.png").write_bytes((directory / "props.png").read_bytes())
        write_obj(directory / f"variant{column}.obj", f"variant{column}", world, parts,
                  [(0.0, 0.0)] * len(parts), [False] * len(parts), x_offset=column * 2.4)
        column += 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--atlas", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    atlas = paint_atlas()
    args.atlas.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(args.atlas, optimize=True)
    meshes = {}
    for kind in KINDS:
        part = build_prop(kind)
        meshes[f"dla_prop_{kind}"] = [part]
        print(f"{kind}: {len(part.vertices)} vertices, {len(part.faces)} triangles")
    write_rig_header(
        args.header, "DRIFT_LA_PROP_DATA_H", "tools/build_props.py",
        "DLA_PROP_", PART_ENUM, "DLA_PROP_SLOT_", SLOT_ENUM, meshes,
        ["enum { " + ", ".join(f"DLA_PROP_KIND_{k.upper()}" for k in KINDS) + ", DLA_PROP_KIND_COUNT };"],
        "dla_props", "[DLA_PROP_KIND_COUNT]", [[f"dla_prop_{kind}"] for kind in KINDS])
    if args.preview_dir:
        write_preview(args.preview_dir, atlas, meshes)


if __name__ == "__main__":
    main()
