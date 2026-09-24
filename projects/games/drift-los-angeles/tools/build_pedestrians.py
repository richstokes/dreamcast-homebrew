#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Author Drift Los Angeles's articulated pedestrians: atlas, meshes and previews.

Run ``uv run tools/build_pedestrians.py --atlas assets/source/pedestrian-atlas.png
--header pedestrian_data.h``.  The atlas is a 256x256 RGB sheet of tintable
regions: the game multiplies each body part by a per-pedestrian colour, so
faces, hair, clothing and denim are painted in light neutral tones with their
detail in the shading.  The mesh is a 16-part skeleton (torso, hips, head, three
hair styles, arms, hands, legs, shoes) exported at two detail levels.  Vertices
are stored relative to their part's pivot and every part carries its parent, so
the runtime composes a walk cycle or a ragdoll tumble from joint pitch angles.

``--preview-dir`` also writes posed OBJ/MTL files with pre-tinted atlases for a
Blender contact sheet (see tools/preview_pedestrians_blender.py).
"""

from __future__ import annotations

import argparse
import math
import random
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ATLAS = 256
REGION = 64
INSET = 1.5 / ATLAS

# ---------------------------------------------------------------------------
# Atlas regions.  Each is (x, y, width, height) in pixels.  Alternate regions
# that a part may swap to at runtime are laid out at fixed offsets from the
# default so the game only adds a constant to each vertex's u.
# ---------------------------------------------------------------------------
REGIONS = {
    "face": (0, 0, 64, 64),            # four variants across the row
    "torso": (0, 64, 64, 64),          # four styles across the row
    "sleeve": (0, 128, 64, 64),
    "skin_limb": (64, 128, 64, 64),
    "jeans_thigh": (128, 128, 64, 64),
    "jeans_shin": (192, 128, 64, 64),
    "hair": (0, 192, 64, 64),
    "cap": (64, 192, 64, 64),
    "shoe": (128, 192, 32, 32),
    "hand": (160, 192, 32, 32),
    "hips": (128, 224, 64, 32),
}

# Head ring heights, used both by the mesh and to place facial features.
HEAD_TOP = 1.785
HEAD_NECK = 1.45


def head_v(y: float) -> float:
    return (HEAD_TOP - y) / (HEAD_TOP - HEAD_NECK)


# ---------------------------------------------------------------------------
# Atlas painting
# ---------------------------------------------------------------------------
def noise_map(width: int, height: int, seed: int, cells: tuple[int, ...] = (4, 8, 16)) -> list[float]:
    rng = random.Random(seed)
    output = [0.0] * (width * height)
    total = 0.0
    for index, grid in enumerate(cells):
        weight = 1.0 / (index + 1)
        total += weight
        gw = max(1, width // grid)
        gh = max(1, height // grid)
        layer = Image.new("L", (max(1, width // gw), max(1, height // gh)))
        layer.putdata([rng.randrange(256) for _ in range(layer.width * layer.height)])
        layer = layer.resize((width, height), Image.Resampling.BICUBIC)
        for i, value in enumerate(layer.get_flattened_data() if hasattr(layer, "get_flattened_data") else layer.getdata()):
            output[i] += weight * (value / 127.5 - 1.0)
    return [value / total for value in output]


def clamp8(value: float) -> int:
    return max(0, min(255, int(round(value))))


def scale_rgb(color: tuple[int, int, int], factor: float) -> tuple[int, int, int]:
    return tuple(clamp8(c * factor) for c in color)


def fill_fabric(img: Image.Image, box: tuple[int, int, int, int], base: tuple[int, int, int],
                seed: int, amplitude: float = .07, folds: float = .06, grain: float = .0) -> None:
    """Base colour with soft cloth noise, vertical fold streaks and optional grain."""
    x0, y0, w, h = box
    pixels = img.load()
    soft = noise_map(w, h, seed)
    streaks = noise_map(w, 1, seed + 91, (8, 16, 32))
    rng = random.Random(seed + 7)
    for y in range(h):
        for x in range(w):
            factor = 1.0 + soft[y * w + x] * amplitude + streaks[x] * folds
            if grain:
                factor += (rng.random() - .5) * grain
            pixels[x0 + x, y0 + y] = scale_rgb(base, factor)


def darken_rect(img: Image.Image, box: tuple[int, int, int, int], factor: float) -> None:
    x0, y0, x1, y1 = box
    pixels = img.load()
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= x < img.width and 0 <= y < img.height:
                pixels[x, y] = scale_rgb(pixels[x, y], factor)


def paint_face(img: Image.Image, variant: int) -> None:
    """Head unwrap: u=0/1 at the nape, u=.5 at the nose; v=0 crown, v=1 neck."""
    x0 = variant * 64
    y0 = 0
    draw = ImageDraw.Draw(img)
    fill_fabric(img, (x0, y0, 64, 64), (242, 232, 224), 300 + variant, .035, .0)
    # Crown and nape read darker so hair and shadow blend into the skin.
    for y in range(0, 8):
        darken_rect(img, (x0, y0 + y, x0 + 63, y0 + y), .92 + y * .01)
    darken_rect(img, (x0, y0 + 56, x0 + 63, y0 + 63), .84)
    darken_rect(img, (x0, y0, x0 + 6, y0 + 63), .90)
    darken_rect(img, (x0 + 57, y0, x0 + 63, y0 + 63), .90)
    eye_y = y0 + int(64 * head_v(1.665))
    brow_y = eye_y - 4 - (variant == 2)
    nose_y = y0 + int(64 * head_v(1.615))
    mouth_y = y0 + int(64 * head_v(1.555))
    cx = x0 + 32
    dark = (52, 38, 30)
    # Eye sockets, whites, irises and brows.
    for side in (-1, 1):
        ex = cx + side * 7
        draw.ellipse((ex - 4, eye_y - 3, ex + 4, eye_y + 3), fill=(214, 196, 184))
        draw.ellipse((ex - 3, eye_y - 2, ex + 3, eye_y + 1), fill=(250, 250, 250))
        draw.rectangle((ex - 1, eye_y - 2, ex + 1, eye_y), fill=dark)
        draw.point((ex - 1, eye_y - 2), fill=(120, 110, 104))
        width = 2 if variant in (0, 3) else 3
        draw.line((ex - 4, brow_y, ex + 4, brow_y - (1 if side < 0 else 0)), fill=(70, 50, 38), width=width)
    # Nose bridge shadow, nostrils, mouth and chin crease.
    draw.line((cx, eye_y + 1, cx + 1, nose_y), fill=(216, 198, 184), width=2)
    draw.point((cx - 2, nose_y + 1), fill=(190, 166, 150))
    draw.point((cx + 2, nose_y + 1), fill=(190, 166, 150))
    if variant == 3:
        draw.arc((cx - 6, mouth_y - 4, cx + 6, mouth_y + 2), 10, 170, fill=(120, 74, 66), width=2)
    else:
        draw.line((cx - 5, mouth_y, cx + 5, mouth_y), fill=(128, 80, 72), width=2)
    draw.line((cx - 4, mouth_y + 2, cx + 4, mouth_y + 2), fill=(226, 200, 190), width=1)
    # Ears at a quarter turn each way.
    for ear_x in (x0 + 15, x0 + 49):
        draw.ellipse((ear_x - 3, eye_y - 3, ear_x + 3, eye_y + 6), fill=(224, 206, 194), outline=(206, 186, 172))
    if variant == 1:
        # Glasses.
        for side in (-1, 1):
            ex = cx + side * 7
            draw.rectangle((ex - 5, eye_y - 4, ex + 5, eye_y + 3), outline=(40, 36, 40))
        draw.line((cx - 2, eye_y - 1, cx + 2, eye_y - 1), fill=(40, 36, 40))
    if variant == 2:
        # Stubble over the jaw.
        rng = random.Random(77)
        pixels = img.load()
        for y in range(nose_y + 3, y0 + 58):
            for x in range(x0 + 18, x0 + 47):
                pixels[x, y] = scale_rgb(pixels[x, y], .90 - .05 * rng.random())


def paint_torso(img: Image.Image, style: int) -> None:
    """Torso unwrap: u=0/1 at the spine, u=.5 at the sternum; v=0 collar, v=1 belt."""
    x0 = style * 64
    y0 = 64
    draw = ImageDraw.Draw(img)
    cx = x0 + 32
    if style == 0:
        # Crew-neck tee with a chest print.
        fill_fabric(img, (x0, y0, 64, 64), (236, 236, 236), 400, .06, .07)
        draw.rectangle((x0, y0, x0 + 63, y0 + 2), fill=(196, 196, 196))
        draw.arc((cx - 9, y0 - 6, cx + 9, y0 + 8), 0, 180, fill=(186, 186, 186), width=2)
        draw.rounded_rectangle((cx - 9, y0 + 16, cx + 9, y0 + 30), 3, outline=(150, 150, 150), width=2)
        draw.ellipse((cx - 4, y0 + 19, cx + 4, y0 + 27), fill=(150, 150, 150))
        draw.line((x0 + 16, y0 + 4, x0 + 16, y0 + 63), fill=(214, 214, 214))
        draw.line((x0 + 48, y0 + 4, x0 + 48, y0 + 63), fill=(214, 214, 214))
        draw.rectangle((x0, y0 + 61, x0 + 63, y0 + 63), fill=(206, 206, 206))
    elif style == 1:
        # Hoodie: kangaroo pocket, drawstrings, heavy hood shadow at the back.
        fill_fabric(img, (x0, y0, 64, 64), (232, 232, 232), 401, .07, .09, .04)
        draw.rounded_rectangle((cx - 15, y0 + 38, cx + 15, y0 + 57), 3, outline=(170, 170, 170), width=2)
        draw.line((cx - 15, y0 + 46, cx - 9, y0 + 40), fill=(170, 170, 170), width=2)
        draw.line((cx + 15, y0 + 46, cx + 9, y0 + 40), fill=(170, 170, 170), width=2)
        for dx in (-3, 3):
            draw.line((cx + dx, y0 + 2, cx + dx + (1 if dx > 0 else -1), y0 + 22), fill=(252, 252, 252), width=1)
            draw.point((cx + dx + (1 if dx > 0 else -1), y0 + 23), fill=(120, 120, 120))
        for band in range(0, 16):
            darken_rect(img, (x0, y0 + band, x0 + 12, y0 + band), .78 + band * .012)
            darken_rect(img, (x0 + 51, y0 + band, x0 + 63, y0 + band), .78 + band * .012)
        draw.rectangle((x0, y0 + 60, x0 + 63, y0 + 63), fill=(200, 200, 200))
    elif style == 2:
        # Open jacket over a white shirt and tie.
        fill_fabric(img, (x0, y0, 64, 64), (208, 208, 212), 402, .05, .05)
        draw.polygon(((cx - 9, y0), (cx + 9, y0), (cx + 4, y0 + 40), (cx - 4, y0 + 40)), fill=(250, 250, 250))
        draw.polygon(((cx - 9, y0), (cx - 2, y0 + 1), (cx - 4, y0 + 40), (cx - 12, y0 + 44)), fill=(150, 150, 156))
        draw.polygon(((cx + 9, y0), (cx + 2, y0 + 1), (cx + 4, y0 + 40), (cx + 12, y0 + 44)), fill=(150, 150, 156))
        draw.polygon(((cx - 2, y0 + 3), (cx + 2, y0 + 3), (cx + 3, y0 + 34), (cx, y0 + 38), (cx - 3, y0 + 34)), fill=(64, 58, 70))
        for by in (44, 52):
            draw.ellipse((cx - 1, y0 + by, cx + 1, y0 + by + 2), fill=(90, 90, 96))
        draw.line((x0 + 8, y0 + 30, x0 + 20, y0 + 30), fill=(160, 160, 166))
        draw.line((x0 + 44, y0 + 30, x0 + 56, y0 + 30), fill=(160, 160, 166))
    else:
        # Striped polo with a collar.
        fill_fabric(img, (x0, y0, 64, 64), (236, 236, 236), 403, .04, .05)
        for y in range(y0 + 10, y0 + 64, 12):
            darken_rect(img, (x0, y, x0 + 63, y + 5), .70)
        draw.rectangle((x0, y0, x0 + 63, y0 + 3), fill=(210, 210, 210))
        draw.polygon(((cx - 8, y0), (cx, y0 + 12), (cx + 8, y0)), fill=(222, 222, 222))
        draw.line((cx, y0 + 2, cx, y0 + 12), fill=(150, 150, 150))
        draw.point((cx, y0 + 6), fill=(90, 90, 90))
        draw.point((cx, y0 + 10), fill=(90, 90, 90))


def paint_limbs(img: Image.Image) -> None:
    draw = ImageDraw.Draw(img)
    x0, y0, w, h = REGIONS["sleeve"]
    fill_fabric(img, (x0, y0, w, h), (232, 232, 232), 500, .06, .09)
    draw.rectangle((x0, y0 + 58, x0 + 63, y0 + 63), fill=(196, 196, 196))
    draw.line((x0 + 32, y0, x0 + 32, y0 + 57), fill=(214, 214, 214))
    x0, y0, w, h = REGIONS["skin_limb"]
    fill_fabric(img, (x0, y0, w, h), (240, 230, 222), 501, .03, .0)
    for y in range(0, 64):
        darken_rect(img, (x0, y0 + y, x0 + 63, y0 + y), .97 + .03 * math.cos(y / 64.0 * math.pi))
    darken_rect(img, (x0, y0, x0 + 12, y0 + 63), .91)
    for name, hem in (("jeans_thigh", False), ("jeans_shin", True)):
        x0, y0, w, h = REGIONS[name]
        fill_fabric(img, (x0, y0, w, h), (222, 226, 236), 502 + hem, .05, .05, .10)
        draw.line((x0 + 16, y0, x0 + 16, y0 + 63), fill=(242, 244, 248))
        draw.line((x0 + 48, y0, x0 + 48, y0 + 63), fill=(242, 244, 248))
        darken_rect(img, (x0, y0, x0 + 8, y0 + 63), .90)
        if hem:
            draw.rectangle((x0, y0 + 58, x0 + 63, y0 + 63), fill=(180, 186, 200))
            for y in range(0, 12):
                darken_rect(img, (x0, y0 + 24 + y, x0 + 63, y0 + 24 + y), .88 + .01 * y)
        else:
            draw.line((x0 + 22, y0 + 2, x0 + 26, y0 + 12), fill=(196, 202, 216), width=2)
            draw.line((x0 + 42, y0 + 2, x0 + 38, y0 + 12), fill=(196, 202, 216), width=2)
            for y in range(0, 8):
                darken_rect(img, (x0, y0 + y, x0 + 63, y0 + y), .84 + .02 * y)


def paint_head_toppers(img: Image.Image) -> None:
    draw = ImageDraw.Draw(img)
    x0, y0, w, h = REGIONS["hair"]
    fill_fabric(img, (x0, y0, w, h), (226, 218, 210), 600, .10, .0, .06)
    rng = random.Random(601)
    pixels = img.load()
    for _ in range(420):
        sx = rng.randrange(0, 64)
        sy = rng.randrange(0, 60)
        length = rng.randrange(3, 10)
        shade = rng.uniform(.55, .85)
        for step in range(length):
            x = sx + (step // 3 if rng.random() < .3 else 0)
            y = sy + step
            if 0 <= x < 64 and 0 <= y < 64:
                pixels[x0 + x, y0 + y] = scale_rgb(pixels[x0 + x, y0 + y], shade)
    darken_rect(img, (x0, y0 + 56, x0 + 63, y0 + 63), .80)
    x0, y0, w, h = REGIONS["cap"]
    fill_fabric(img, (x0, y0, w, h), (236, 236, 236), 602, .04, .03)
    for seam in (x0 + 16, x0 + 32, x0 + 48):
        draw.line((seam, y0, seam, y0 + 40), fill=(200, 200, 200))
    draw.ellipse((x0 + 26, y0 + 12, x0 + 38, y0 + 24), fill=(170, 170, 170))
    draw.ellipse((x0 + 29, y0 + 15, x0 + 35, y0 + 21), fill=(236, 236, 236))
    draw.rectangle((x0, y0 + 42, x0 + 63, y0 + 63), fill=(204, 204, 204))
    draw.rectangle((x0, y0 + 42, x0 + 63, y0 + 43), fill=(150, 150, 150))
    x0, y0, w, h = REGIONS["shoe"]
    fill_fabric(img, (x0, y0, w, h), (232, 232, 232), 603, .04, .0)
    draw.rectangle((x0, y0 + 24, x0 + 31, y0 + 31), fill=(58, 56, 60))
    draw.rectangle((x0, y0 + 22, x0 + 31, y0 + 23), fill=(170, 170, 170))
    for lace in range(4):
        draw.line((x0 + 10, y0 + 4 + lace * 4, x0 + 22, y0 + 4 + lace * 4), fill=(140, 140, 140))
    draw.rectangle((x0, y0, x0 + 31, y0 + 1), fill=(190, 190, 190))
    x0, y0, w, h = REGIONS["hand"]
    fill_fabric(img, (x0, y0, w, h), (240, 230, 222), 604, .03, .0)
    for finger in range(4):
        draw.line((x0 + 4 + finger * 7, y0 + 20, x0 + 4 + finger * 7, y0 + 31), fill=(206, 190, 180))
    x0, y0, w, h = REGIONS["hips"]
    fill_fabric(img, (x0, y0, w, h), (222, 226, 236), 605, .05, .05, .10)
    draw.rectangle((x0, y0, x0 + 63, y0 + 4), fill=(74, 62, 52))
    draw.rectangle((x0 + 29, y0, x0 + 35, y0 + 4), fill=(190, 170, 120))
    draw.line((x0 + 32, y0 + 5, x0 + 32, y0 + 31), fill=(200, 206, 220))
    draw.line((x0 + 16, y0 + 8, x0 + 16, y0 + 31), fill=(242, 244, 248))
    draw.line((x0 + 48, y0 + 8, x0 + 48, y0 + 31), fill=(242, 244, 248))


def build_atlas() -> Image.Image:
    img = Image.new("RGB", (ATLAS, ATLAS), (128, 128, 128))
    for variant in range(4):
        paint_face(img, variant)
        paint_torso(img, variant)
    paint_limbs(img)
    paint_head_toppers(img)
    # Unused cells hold mid grey so a stray UV never flashes white.
    ImageDraw.Draw(img).rectangle((192, 192, 255, 255), fill=(150, 150, 150))
    return img


# ---------------------------------------------------------------------------
# Mesh authoring
# ---------------------------------------------------------------------------
@dataclass
class Part:
    name: str
    parent: int
    pivot: tuple[float, float, float]
    region: str
    slot: str
    vertices: list[tuple[float, ...]] = field(default_factory=list)   # x y z u v nx ny nz
    faces: list[tuple[int, int, int]] = field(default_factory=list)

    def add_vertex(self, position, uv, normal) -> int:
        self.vertices.append((*position, *uv, *normal))
        return len(self.vertices) - 1


def normalize(v: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) or 1.0
    return (v[0] / length, v[1] / length, v[2] / length)


def region_uv(region: str, u: float, v: float) -> tuple[float, float]:
    x0, y0, w, h = REGIONS[region]
    u = min(max(u, 0.0), 1.0)
    v = min(max(v, 0.0), 1.0)
    return ((x0 + INSET * ATLAS + u * (w - 2 * INSET * ATLAS)) / ATLAS,
            (y0 + INSET * ATLAS + v * (h - 2 * INSET * ATLAS)) / ATLAS)


def add_tube(part: Part, rings: list[tuple[float, float, float, float, float]],
             segments: int, top_cap: bool = False, bottom_cap: bool = False,
             y_warp=None) -> None:
    """Rings are (y, rx, rz, z_offset, v).  u runs from the back around the
    right side to the front (u=.5) and back to the seam."""
    ring_starts = []
    for (y, rx, rz, zoff, v) in rings:
        start = len(part.vertices)
        ring_starts.append(start)
        for i in range(segments + 1):
            t = i / segments
            angle = 2.0 * math.pi * t
            x = rx * math.sin(angle)
            z = zoff - rz * math.cos(angle)
            yy = y + (y_warp(t) if y_warp else 0.0)
            normal = normalize((math.sin(angle) / max(rx, 1e-4), 0.0, -math.cos(angle) / max(rz, 1e-4)))
            part.add_vertex((x, yy - part.pivot[1], z - part.pivot[2]),
                            region_uv(part.region, t, v), normal)
    for r in range(len(rings) - 1):
        a0, a1 = ring_starts[r], ring_starts[r + 1]
        for i in range(segments):
            part.faces.append((a0 + i, a1 + i, a1 + i + 1))
            part.faces.append((a0 + i, a1 + i + 1, a0 + i + 1))
    if top_cap:
        y, rx, rz, zoff, v = rings[0]
        centre = part.add_vertex((0.0, y - part.pivot[1], zoff - part.pivot[2]),
                                 region_uv(part.region, .5, v), (0.0, 1.0, 0.0))
        start = ring_starts[0]
        for i in range(segments):
            part.faces.append((centre, start + i + 1, start + i))
    if bottom_cap:
        y, rx, rz, zoff, v = rings[-1]
        centre = part.add_vertex((0.0, y - part.pivot[1], zoff - part.pivot[2]),
                                 region_uv(part.region, .5, v), (0.0, -1.0, 0.0))
        start = ring_starts[-1]
        for i in range(segments):
            part.faces.append((centre, start + i, start + i + 1))


def add_box(part: Part, lo: tuple[float, float, float], hi: tuple[float, float, float],
            uv_box: tuple[float, float, float, float], top_front_drop: float = 0.0) -> None:
    """Axis-aligned box in model space with each face mapped to the same UV cell."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    u0, v0, u1, v1 = uv_box
    corners = {
        "lbb": (x0, y0, z0), "rbb": (x1, y0, z0), "lbf": (x0, y0, z1), "rbf": (x1, y0, z1),
        "ltb": (x0, y1, z0), "rtb": (x1, y1, z0),
        "ltf": (x0, y1 - top_front_drop, z1), "rtf": (x1, y1 - top_front_drop, z1),
    }
    faces = (
        (("ltb", "rtb", "rtf", "ltf"), (0.0, 1.0, 0.0)),      # top
        (("lbf", "rbf", "rbb", "lbb"), (0.0, -1.0, 0.0)),     # bottom
        (("ltf", "rtf", "rbf", "lbf"), (0.0, 0.0, 1.0)),      # front
        (("rtb", "ltb", "lbb", "rbb"), (0.0, 0.0, -1.0)),     # back
        (("ltb", "ltf", "lbf", "lbb"), (-1.0, 0.0, 0.0)),     # left
        (("rtf", "rtb", "rbb", "rbf"), (1.0, 0.0, 0.0)),      # right
    )
    for names, normal in faces:
        base = len(part.vertices)
        uvs = ((u0, v0), (u1, v0), (u1, v1), (u0, v1))
        for name, (u, v) in zip(names, uvs):
            c = corners[name]
            part.add_vertex((c[0] - part.pivot[0], c[1] - part.pivot[1], c[2] - part.pivot[2]),
                            region_uv(part.region, u, v), normal)
        part.faces.append((base, base + 1, base + 2))
        part.faces.append((base, base + 2, base + 3))


def fix_winding(part: Part) -> None:
    """Orient every face so its geometric normal agrees with its vertex normals."""
    fixed = []
    for a, b, c in part.faces:
        va, vb, vc = part.vertices[a], part.vertices[b], part.vertices[c]
        ab = (vb[0] - va[0], vb[1] - va[1], vb[2] - va[2])
        ac = (vc[0] - va[0], vc[1] - va[1], vc[2] - va[2])
        n = (ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0])
        if n[0] ** 2 + n[1] ** 2 + n[2] ** 2 < 1e-12:
            continue
        avg = (va[5] + vb[5] + vc[5], va[6] + vb[6] + vc[6], va[7] + vb[7] + vc[7])
        if n[0] * avg[0] + n[1] * avg[1] + n[2] * avg[2] < 0.0:
            fixed.append((a, c, b))
        else:
            fixed.append((a, b, c))
    part.faces = fixed


def build_parts(detail: int) -> list[Part]:
    """detail 0 is the close-range mesh, detail 1 the mid-range one."""
    seg_body = 8 if detail == 0 else 6
    seg_head = 8 if detail == 0 else 6
    seg_limb = 6 if detail == 0 else 4
    parts: list[Part] = []

    def add(name, parent, pivot, region, slot) -> Part:
        part = Part(name, parent, pivot, region, slot)
        parts.append(part)
        return part

    torso = add("torso", -1, (0.0, .98, 0.0), "torso", "shirt")
    add_tube(torso, [
        (1.47, .085, .070, .0, .0),
        (1.43, .215, .110, .0, .06),
        (1.24, .190, .125, .01, .42),
        (.98, .165, .105, .0, 1.0),
    ] if detail == 0 else [
        (1.47, .085, .070, .0, .0),
        (1.43, .215, .110, .0, .06),
        (.98, .168, .108, .0, 1.0),
    ], seg_body, top_cap=True)
    hips = add("hips", 0, (0.0, .98, 0.0), "hips", "trousers")
    add_tube(hips, [
        (.98, .165, .105, .0, .0),
        (.83, .155, .102, .0, 1.0),
    ], seg_body, bottom_cap=True)

    head = add("head", 0, (0.0, 1.46, 0.0), "face", "skin")
    add_tube(head, [
        (HEAD_TOP, .030, .030, .0, head_v(HEAD_TOP)),
        (1.750, .076, .084, .0, head_v(1.750)),
        (1.665, .090, .100, .0, head_v(1.665)),
        (1.570, .078, .090, .012, head_v(1.570)),
        (1.505, .056, .062, .004, head_v(1.505)),
        (HEAD_NECK, .050, .050, .0, 1.0),
    ] if detail == 0 else [
        (HEAD_TOP, .030, .030, .0, head_v(HEAD_TOP)),
        (1.730, .082, .090, .0, head_v(1.730)),
        (1.620, .086, .096, .010, head_v(1.620)),
        (1.505, .054, .058, .0, head_v(1.505)),
        (HEAD_NECK, .050, .050, .0, 1.0),
    ], seg_head, top_cap=True)

    def hairline(bottom_back: float, bottom_front: float):
        return lambda t: (bottom_front - bottom_back) * (.5 - .5 * math.cos(2.0 * math.pi * t))

    hair_short = add("hair_short", 2, (0.0, 1.46, 0.0), "hair", "hair")
    add_tube(hair_short, [
        (1.805, .040, .040, -.010, .0),
        (1.760, .090, .100, -.012, .30),
        (1.600, .096, .104, -.018, 1.0),
    ], seg_head, top_cap=True)
    # Raise the front of the lowest ring above the brows so the face stays clear.
    warp = hairline(0.0, .11)
    lowest = len(hair_short.vertices) - (seg_head + 1) - 1
    for i in range(seg_head + 1):
        v = list(hair_short.vertices[lowest + i])
        v[1] += warp(i / seg_head)
        hair_short.vertices[lowest + i] = tuple(v)

    hair_long = add("hair_long", 2, (0.0, 1.46, 0.0), "hair", "hair")
    add_tube(hair_long, [
        (1.805, .040, .040, -.010, .0),
        (1.760, .094, .106, -.016, .25),
        (1.600, .104, .116, -.030, .68),
        (1.470, .098, .110, -.045, 1.0),
    ], seg_head, top_cap=True)
    warp = hairline(0.0, .24)
    lowest = len(hair_long.vertices) - (seg_head + 1) - 1
    for i in range(seg_head + 1):
        v = list(hair_long.vertices[lowest + i])
        v[1] += warp(i / seg_head)
        hair_long.vertices[lowest + i] = tuple(v)

    cap = add("cap", 2, (0.0, 1.46, 0.0), "cap", "accent")
    add_tube(cap, [
        (1.815, .030, .030, .0, .0),
        (1.775, .088, .098, .0, .30),
        (1.700, .097, .107, .0, .66),
    ], seg_head, top_cap=True)
    brim = len(cap.vertices)
    for (x, z, u, v) in ((-.075, .07, .0, .70), (.075, .07, 1.0, .70), (-.06, .19, .0, 1.0), (.06, .19, 1.0, 1.0)):
        cap.add_vertex((x, 1.700 - 1.46, z), region_uv("cap", u, v), (0.0, 1.0, 0.0))
    cap.faces.append((brim, brim + 1, brim + 3))
    cap.faces.append((brim, brim + 3, brim + 2))
    for (x, z, u, v) in ((-.075, .07, .0, .70), (.075, .07, 1.0, .70), (-.06, .19, .0, 1.0), (.06, .19, 1.0, 1.0)):
        cap.add_vertex((x, 1.695 - 1.46, z), region_uv("cap", u, v), (0.0, -1.0, 0.0))
    cap.faces.append((brim + 4, brim + 7, brim + 5))
    cap.faces.append((brim + 4, brim + 6, brim + 7))

    for side, sign in (("l", -1.0), ("r", 1.0)):
        upper = add(f"upper_arm_{side}", 0, (sign * .235, 1.40, 0.0), "sleeve", "sleeve")
        add_tube(upper, [
            (1.445, .034, .034, .0, .0),
            (1.400, .062, .062, .0, .14),
            (1.11, .044, .044, .0, 1.0),
        ] if detail == 0 else [
            (1.435, .056, .056, .0, .0),
            (1.11, .044, .044, .0, 1.0),
        ], seg_limb, top_cap=True)
        fore = add(f"forearm_{side}", len(parts) - 1, (sign * .235, 1.11, 0.0), "skin_limb", "forearm")
        add_tube(fore, [
            (1.11, .046, .046, .0, .0),
            (.86 if detail == 0 else .79, .034, .034, .0, 1.0),
        ], seg_limb)
        hand = add(f"hand_{side}", len(parts) - 1, (sign * .235, .86, 0.0), "hand", "skin")
        if detail == 0:
            add_box(hand, (sign * .235 - .034, .77, -.022), (sign * .235 + .034, .86, .028), (.0, .0, 1.0, 1.0))

    for side, sign in (("l", -1.0), ("r", 1.0)):
        thigh = add(f"thigh_{side}", 1, (sign * .10, .92, 0.0), "jeans_thigh", "trousers")
        add_tube(thigh, [
            (.94, .088, .092, .0, .0),
            (.72, .080, .082, .0, .50),
            (.50, .064, .066, .0, 1.0),
        ] if detail == 0 else [
            (.94, .088, .092, .0, .0),
            (.50, .064, .066, .0, 1.0),
        ], seg_limb, top_cap=True)
        shin = add(f"shin_{side}", len(parts) - 1, (sign * .10, .50, 0.0), "jeans_shin", "shin")
        add_tube(shin, [
            (.52, .066, .068, .0, .0),
            (.34, .058, .060, .0, .42),
            (.08, .046, .048, .0, 1.0),
        ] if detail == 0 else [
            (.52, .066, .068, .0, .0),
            (.08, .046, .048, .0, 1.0),
        ], seg_limb)
        shoe = add(f"shoe_{side}", len(parts) - 1, (sign * .10, .08, 0.0), "shoe", "shoe")
        add_box(shoe, (sign * .10 - .056, .0, -.075), (sign * .10 + .056, .085, .165), (.0, .0, 1.0, 1.0), top_front_drop=.03)

    for part in parts:
        fix_winding(part)
    return parts


PART_ENUM = [
    "TORSO", "HIPS", "HEAD", "HAIR_SHORT", "HAIR_LONG", "CAP",
    "UPPER_ARM_L", "FOREARM_L", "HAND_L", "UPPER_ARM_R", "FOREARM_R", "HAND_R",
    "THIGH_L", "SHIN_L", "SHOE_L", "THIGH_R", "SHIN_R", "SHOE_R",
]
SLOT_ENUM = ["SKIN", "SHIRT", "TROUSERS", "HAIR", "ACCENT", "SHOE", "SLEEVE", "FOREARM", "SHIN"]


# ---------------------------------------------------------------------------
# Header export
# ---------------------------------------------------------------------------
def write_header(path: Path, lods: list[list[Part]]) -> None:
    lines = [
        "/* Generated by tools/build_pedestrians.py. Do not edit by hand. */",
        "#ifndef DRIFT_LA_PEDESTRIAN_DATA_H",
        "#define DRIFT_LA_PEDESTRIAN_DATA_H",
        "",
        "#include <stdint.h>",
        "",
        "/* Positions are relative to the owning part's pivot; UVs address the",
        "   part's default atlas region. */",
        "typedef struct { float x, y, z; float u, v; float nx, ny, nz; } dla_ped_vertex_t;",
        "typedef struct { uint16_t a, b, c; } dla_ped_face_t;",
        "typedef struct {",
        "    int8_t parent;",
        "    uint8_t slot;",
        "    float pivot_x, pivot_y, pivot_z;",
        "    uint16_t first_vertex, vertex_count, first_face, face_count;",
        "} dla_ped_part_t;",
        "typedef struct {",
        "    const dla_ped_vertex_t *vertices;",
        "    const dla_ped_face_t *faces;",
        "    const dla_ped_part_t *parts;",
        "    uint16_t vertex_count, face_count, part_count;",
        "} dla_ped_mesh_t;",
        "",
        "enum { " + ", ".join(f"DLA_PED_{name}" for name in PART_ENUM) + ", DLA_PED_PART_COUNT };",
        "enum { " + ", ".join(f"DLA_PED_SLOT_{name}" for name in SLOT_ENUM) + " };",
        "",
        f"#define DLA_PED_ATLAS_REGION ({REGION / ATLAS:.6f}f)",
        f"#define DLA_PED_HEAD_TOP ({HEAD_TOP:.4f}f)",
        "",
    ]
    for lod, parts in enumerate(lods):
        assert [p.name.upper() for p in parts] == PART_ENUM, [p.name for p in parts]
        vertices: list[tuple[float, ...]] = []
        faces: list[tuple[int, int, int]] = []
        part_rows = []
        for part in parts:
            first_vertex = len(vertices)
            first_face = len(faces)
            vertices.extend(part.vertices)
            faces.extend((a + first_vertex, b + first_vertex, c + first_vertex) for a, b, c in part.faces)
            part_rows.append(
                f"    {{ {part.parent}, DLA_PED_SLOT_{part.slot.upper()}, "
                f"{part.pivot[0]:.4f}f, {part.pivot[1]:.4f}f, {part.pivot[2]:.4f}f, "
                f"{first_vertex}, {len(part.vertices)}, {first_face}, {len(part.faces)} }},")
        total_tris = len(faces)
        lines.append(f"/* Detail level {lod}: {len(vertices)} vertices, {total_tris} triangles. */")
        lines.append(f"static const dla_ped_vertex_t dla_ped_vertices_{lod}[{len(vertices)}] = {{")
        for v in vertices:
            lines.append("    { " + ", ".join(f"{value:.4f}f" for value in v) + " },")
        lines.append("};")
        lines.append(f"static const dla_ped_face_t dla_ped_faces_{lod}[{len(faces)}] = {{")
        for row in range(0, len(faces), 6):
            lines.append("    " + " ".join(f"{{{a},{b},{c}}}," for a, b, c in faces[row:row + 6]))
        lines.append("};")
        lines.append(f"static const dla_ped_part_t dla_ped_parts_{lod}[DLA_PED_PART_COUNT] = {{")
        lines.extend(part_rows)
        lines.append("};")
        lines.append("")
    lines.append("static const dla_ped_mesh_t dla_pedestrian_lods[2] = {")
    for lod, parts in enumerate(lods):
        vertex_count = sum(len(p.vertices) for p in parts)
        face_count = sum(len(p.faces) for p in parts)
        lines.append(f"    {{ dla_ped_vertices_{lod}, dla_ped_faces_{lod}, dla_ped_parts_{lod}, "
                     f"{vertex_count}, {face_count}, DLA_PED_PART_COUNT }},")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


# ---------------------------------------------------------------------------
# Preview: posed OBJ files with pre-tinted atlases
# ---------------------------------------------------------------------------
SKIN_TONES = [(1.0, .82, .68), (.96, .74, .58), (.82, .58, .42), (.62, .42, .30), (.44, .29, .20)]
SHIRT_COLORS = [(.95, .22, .20), (.10, .70, .86), (.96, .66, .12), (.58, .26, .86),
                (.24, .78, .40), (.92, .92, .92), (.16, .18, .24), (.96, .48, .72)]
TROUSER_COLORS = [(.36, .44, .64), (.16, .17, .20), (.64, .56, .40), (.46, .47, .52)]
HAIR_COLORS = [(.14, .10, .08), (.36, .22, .12), (.86, .72, .40), (.60, .26, .12), (.70, .70, .72)]
SHOE_COLORS = [(.95, .95, .95), (.16, .16, .18), (.86, .16, .16)]


@dataclass
class Look:
    skin: tuple[float, float, float]
    shirt: tuple[float, float, float]
    trousers: tuple[float, float, float]
    hair: tuple[float, float, float]
    shoe: tuple[float, float, float]
    accent: tuple[float, float, float]
    style: int
    face: int
    topper: int      # 0 short hair, 1 long hair, 2 cap, 3 bald
    shorts: bool
    height: float


def look_for_seed(seed: int) -> Look:
    rng = random.Random(seed)
    style = rng.randrange(4)
    shirt = rng.choice(SHIRT_COLORS)
    return Look(
        skin=rng.choice(SKIN_TONES), shirt=shirt, trousers=rng.choice(TROUSER_COLORS),
        hair=rng.choice(HAIR_COLORS), shoe=rng.choice(SHOE_COLORS),
        accent=rng.choice(SHIRT_COLORS), style=style, face=rng.randrange(4),
        topper=rng.randrange(4), shorts=rng.random() < .3 and style != 2, height=rng.uniform(.92, 1.08),
    )


def part_tint_and_offset(part: Part, look: Look) -> tuple[tuple[float, float, float], tuple[float, float]]:
    """Mirror of the runtime rules: which colour and which atlas region a part uses."""
    r = REGION / ATLAS
    if part.slot == "skin":
        if part.region == "face":
            return look.skin, (look.face * r, 0.0)
        return look.skin, (0.0, 0.0)
    if part.slot == "shirt":
        return look.shirt, (look.style * r, 0.0)
    if part.slot == "trousers":
        return look.trousers, (0.0, 0.0)
    if part.slot == "hair":
        return look.hair, (0.0, 0.0)
    if part.slot == "accent":
        return look.accent, (0.0, 0.0)
    if part.slot == "shoe":
        return look.shoe, (0.0, 0.0)
    if part.slot == "sleeve":
        # Polo and tee show skin below a short sleeve region; hoodie/jacket stay covered.
        return look.shirt, (0.0, 0.0)
    if part.slot == "forearm":
        if look.style in (1, 2):
            return look.shirt, (-r, 0.0)      # sleeve region sits one cell left of skin
        return look.skin, (0.0, 0.0)
    if part.slot == "shin":
        if look.shorts:
            return look.skin, (-2 * r, 0.0)   # skin_limb sits two cells left of jeans_shin
        return look.trousers, (0.0, 0.0)
    raise ValueError(part.slot)


def walk_pose(phase: float) -> dict[str, float]:
    s = math.sin(phase)
    c = math.cos(phase)
    return {
        "root_y": abs(s) * .025,
        "torso": -.06,
        "head": .04,
        "upper_arm_l": -s * .55, "upper_arm_r": s * .55,
        "forearm_l": .45 + max(0.0, -s) * .35, "forearm_r": .45 + max(0.0, s) * .35,
        "thigh_l": s * .58, "thigh_r": -s * .58,
        "shin_l": -(.12 + max(0.0, c) * .95), "shin_r": -(.12 + max(0.0, -c) * .95),
    }


def compose(parts: list[Part], angles: dict[str, float], root_pitch: float = 0.0,
            root_y: float = 0.0) -> list[tuple[float, float, float, float]]:
    """Per-part (cos, sin, ty, tz): model = (x, ty + c*y + s*z, tz - s*y + c*z)."""
    transforms: list[tuple[float, float, float, float]] = []
    for index, part in enumerate(parts):
        angle = angles.get(part.name, 0.0)
        if part.name.startswith("hair") or part.name == "cap":
            angle = 0.0
        if part.parent < 0:
            # Root rotates about the body centre so a tumble reads as a whole-body spin.
            centre_y = .90
            rc, rs = math.cos(root_pitch), math.sin(root_pitch)
            c, s = math.cos(angle), math.sin(angle)
            # T(pivot) R(angle) composed under T(centre) R(root) T(-centre)
            d = part.pivot[1] - centre_y
            ty = centre_y + root_y + rc * d
            tz = -rs * d
            transforms.append((rc * c - rs * s, rc * s + rs * c, ty, tz))
        else:
            pc, ps, pty, ptz = transforms[part.parent]
            parent = parts[part.parent]
            dy = part.pivot[1] - parent.pivot[1]
            dz = part.pivot[2] - parent.pivot[2]
            c, s = math.cos(angle), math.sin(angle)
            transforms.append((pc * c - ps * s, pc * s + ps * c,
                               pty + pc * dy + ps * dz, ptz - ps * dy + pc * dz))
    return transforms


def posed_vertices(parts: list[Part], transforms, look: Look, yaw: float = 0.0):
    out = []
    for index, part in enumerate(parts):
        c, s, ty, tz = transforms[index]
        for v in part.vertices:
            x, y, z = v[0], v[1], v[2]
            my = ty + c * y + s * z
            mz = tz - s * y + c * z
            mx = (x + part.pivot[0]) * look.height
            my *= look.height
            mz *= look.height
            wx = mx * math.cos(yaw) + mz * math.sin(yaw)
            wz = -mx * math.sin(yaw) + mz * math.cos(yaw)
            out.append((wx, my, wz))
    return out


def write_preview(directory: Path, atlas: Image.Image, lods: list[list[Part]]) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    variants = [(seed, look_for_seed(seed)) for seed in range(8)]
    for column, (seed, look) in enumerate(variants):
        lod = 1 if column >= 6 else 0
        parts = lods[lod]
        tinted = atlas.copy()
        pixels = tinted.load()
        # Pre-tint every region this look uses, respecting per-part region offsets.
        tint_by_region: dict[tuple[int, int], tuple[float, float, float]] = {}
        for part in parts:
            if part.name in ("hair_short", "hair_long", "cap"):
                if ("hair_short", "hair_long", "cap")[look.topper if look.topper < 3 else 0] != part.name:
                    continue
            tint, (du, dv) = part_tint_and_offset(part, look)
            x0, y0, w, h = REGIONS[part.region]
            cell = (x0 + int(round(du * ATLAS)), y0 + int(round(dv * ATLAS)))
            tint_by_region[cell] = tint
        sizes = {(rx, ry): (rw, rh) for (rx, ry, rw, rh) in REGIONS.values()}
        for (cx, cy), tint in tint_by_region.items():
            w, h = sizes.get((cx, cy), (64, 64))
            for y in range(cy, min(cy + h, ATLAS)):
                for x in range(cx, min(cx + w, ATLAS)):
                    p = pixels[x, y]
                    pixels[x, y] = (clamp8(p[0] * tint[0]), clamp8(p[1] * tint[1]), clamp8(p[2] * tint[2]))
        tinted.save(directory / f"variant{column}.png")
        phase = column * 1.1
        if column == 7:
            transforms = compose(parts, {"upper_arm_l": -2.3, "upper_arm_r": -2.0, "forearm_l": .6,
                                         "forearm_r": .9, "thigh_l": .7, "thigh_r": .2, "shin_l": -.9,
                                         "shin_r": -.5}, root_pitch=math.pi * .5, root_y=-.77)
        else:
            pose = walk_pose(phase)
            transforms = compose(parts, pose, 0.0, pose["root_y"])
        world = posed_vertices(parts, transforms, look, yaw=0.0)
        obj = [f"mtllib variant{column}.mtl", f"usemtl variant{column}", "o pedestrian"]
        for (x, y, z) in world:
            obj.append(f"v {x + column * .9:.5f} {y:.5f} {z:.5f}")
        offset = 0
        uvs, normals, faces = [], [], []
        for part in parts:
            tint, (du, dv) = part_tint_and_offset(part, look)
            for v in part.vertices:
                uvs.append(f"vt {v[3] + du:.5f} {1.0 - (v[4] + dv):.5f}")
                normals.append(f"vn {v[5]:.4f} {v[6]:.4f} {v[7]:.4f}")
            skip = part.name in ("hair_short", "hair_long", "cap") and \
                (look.topper == 3 or ("hair_short", "hair_long", "cap")[look.topper] != part.name)
            if not skip:
                for a, b, c in part.faces:
                    faces.append("f " + " ".join(f"{i + offset + 1}/{i + offset + 1}/{i + offset + 1}" for i in (a, b, c)))
            offset += len(part.vertices)
        obj.extend(uvs)
        obj.extend(normals)
        obj.extend(faces)
        (directory / f"variant{column}.obj").write_text("\n".join(obj) + "\n", encoding="utf-8")
        (directory / f"variant{column}.mtl").write_text(
            f"newmtl variant{column}\nKd 1 1 1\nmap_Kd variant{column}.png\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--atlas", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    atlas = build_atlas()
    args.atlas.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(args.atlas, optimize=True)
    lods = [build_parts(0), build_parts(1)]
    write_header(args.header, lods)
    for lod, parts in enumerate(lods):
        print(f"pedestrian LOD{lod}: {sum(len(p.vertices) for p in parts)} vertices, "
              f"{sum(len(p.faces) for p in parts)} triangles")
    if args.preview_dir:
        write_preview(args.preview_dir, atlas, lods)


if __name__ == "__main__":
    main()
