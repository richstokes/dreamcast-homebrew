#!/usr/bin/env python3
"""Model Apple Valley's props in Blender and bake them for the game.

Run through Blender, not the host Python interpreter (``make
apple-valley-props`` does it):

    blender --background --python tools/build_apple_valley_props.py -- \
        --header assets/generated/apple_valley_props.h \
        --blend captures/apple_valley_props.blend --preview captures/apple-valley-props

Every prop is built procedurally with bmesh (the race control tower, the
shaded bleachers, pop-up pit canopies, shipping containers, the clubhouse,
Joshua trees, creosote, tyre stacks, a light pole, the starter's stand, a
length of windscreen fence, boulders, a kart trailer and the brown desert
mountain), given a material from the fixed list below (the game maps each to
a texture) and vertex colours, then cut into PowerVR strips per material and
written as a header, as tools/build_tsudoken_props.py does for Tsudoken Drift.
"""

from __future__ import annotations

import argparse
import math
import random
import sys
from pathlib import Path

import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
import blender_props  # noqa: E402
from blender_props import WHITE, mix, rgba, scale_col  # noqa: E402

# Material order is the game's AVM_* enum (world.c maps them to headers).
MATERIALS = ["flat", "metal", "crowd", "joshua", "jbark", "scrub", "banner", "stucco", "rock"]
# Punch-through (alpha tested) materials go in their own list in the game.
PT_MATERIALS = {"joshua", "scrub"}

ROOF_RED = rgba(150, 52, 40)
ROOF_UNDER = rgba(70, 40, 34)
GLASS = rgba(46, 70, 96)
GLASS_LIT = rgba(120, 160, 190)
STEEL = rgba(170, 174, 180)
ALU = rgba(206, 210, 214)
DARK = rgba(44, 44, 48)
TYRE = rgba(36, 36, 40)
TYRE_WHITE = rgba(214, 214, 208)
SHADE = rgba(232, 222, 196)
DOOR = rgba(88, 70, 56)
RUST = rgba(176, 82, 52)
TEAL = rgba(70, 128, 132)
SAND = rgba(210, 186, 140)


class Builder(blender_props.Builder):
    MATERIALS = MATERIALS


def pyramid_roof(bd, c, hw, hd, y, rise, col_top, col_under):
    """A four-sided pyramid roof over (hw, hd) about c from eave height y,
    with a dark sheet just under it."""
    cx, cz = c[0], c[2]
    apex = (cx, y + rise, cz)
    corners = [(cx - hw, y, cz - hd), (cx + hw, y, cz - hd), (cx + hw, y, cz + hd), (cx - hw, y, cz + hd)]
    for i in range(4):
        a, b_ = corners[i], corners[(i + 1) % 4]
        bd.poly([a, b_, apex], "flat", None, [col_top, col_top, scale_col(col_top, 1.1)])
        bd.poly([(a[0], a[1] - 0.12, a[2]), (b_[0], b_[1] - 0.12, b_[2]), (apex[0], apex[1] - 0.12, apex[2])], "flat",
                None, col_under)


# ---------------------------------------------------------------------------
# Buildings

def build_tower():
    """Race control by the start line, as in the photos: a white stucco
    tower, its glazed booth on top under a red pyramid roof, a balcony on
    the track side and the circuit's sign below the booth. Front (the
    track) is -z."""
    bd = Builder("tower")
    hw = 2.0
    bd.box((0, 2.0, 0), (hw, 2.0, hw), 0, "stucco", WHITE, uv_scale=0.5, top=False)
    bd.quad((-0.6, 0.0, -hw - 0.02), (0.6, 0.0, -hw - 0.02), (0.6, 2.1, -hw - 0.02), (-0.6, 2.1, -hw - 0.02), "flat",
            col=DOOR)
    # The sign across the front under the booth.
    bd.quad((-hw + 0.1, 2.6, -hw - 0.03), (hw - 0.1, 2.6, -hw - 0.03), (hw - 0.1, 3.6, -hw - 0.03),
            (-hw + 0.1, 3.6, -hw - 0.03), "banner", uv=((0, 1), (1, 1), (1, 0), (0, 0)))
    # The booth: a slab, posts at the corners and glass all round.
    bw = hw + 0.4
    bd.box((0, 4.1, 0), (bw, 0.1, bw), 0, "flat", ALU, bottom=True)
    for side in range(4):
        a = side * math.pi / 2
        n = Vector((math.sin(a), 0, math.cos(a)))
        t = Vector((math.cos(a), 0, -math.sin(a)))
        c = n * bw + Vector((0, 5.35, 0))
        pts = [c - t * bw - Vector((0, 1.15, 0)), c + t * bw - Vector((0, 1.15, 0)),
               c + t * bw + Vector((0, 1.15, 0)), c - t * bw + Vector((0, 1.15, 0))]
        bd.poly([tuple(p) for p in pts], "flat", None, [GLASS, GLASS, GLASS_LIT, GLASS_LIT])
    for sx in (-1, 1):
        for sz in (-1, 1):
            bd.box((sx * bw, 5.35, sz * bw), (0.09, 1.15, 0.09), 0, "flat", WHITE)
    bd.box((0, 6.6, 0), (bw + 0.05, 0.12, bw + 0.05), 0, "stucco", WHITE, uv_scale=0.5, bottom=True)
    pyramid_roof(bd, (0, 0, 0), bw + 0.7, bw + 0.7, 6.72, 1.9, ROOF_RED, ROOF_UNDER)
    bd.tube((0, 8.5, 0), (0, 9.1, 0), 0.06, 0.02, 4, "flat", DARK)
    # The balcony on the track side, with its rail.
    bd.box((0, 4.15, -bw - 0.7), (bw, 0.08, 0.7), 0, "flat", ALU, bottom=True)
    bd.box((0, 5.1, -bw - 1.36), (bw, 0.035, 0.035), 0, "flat", WHITE, bottom=True)
    for sx in (-1, 1):
        bd.box((sx * bw, 5.1, -bw - 0.7), (0.035, 0.035, 0.7), 0, "flat", WHITE, bottom=True)
    for x in (-bw, -bw / 3, bw / 3, bw):
        bd.box((x, 4.65, -bw - 1.36), (0.04, 0.45, 0.04), 0, "flat", WHITE)
    # The stair up the side.
    for k in range(8):
        y = 0.25 + k * 0.5
        bd.box((hw + 0.6, y, -hw + 0.4 + k * 0.42), (0.5, 0.05, 0.2), 0, "flat", STEEL, bottom=True)
    return bd.finish()


def build_stand():
    """A row of aluminium bleachers, 12 m long and five rows deep, full of
    spectators, under a canvas shade sail on four steel posts. The front
    row is at z = 0 facing -z."""
    bd = Builder("stand")
    W, rows, tread, rise = 6.0, 5, 0.85, 0.45
    y0 = 0.45
    for i in range(rows):
        z0, z1, y = i * tread, (i + 1) * tread, y0 + i * rise
        bd.quad((-W, y, z0), (W, y, z0), (W, y, z1), (-W, y, z1), "flat", col=ALU)
        bd.quad((-W, y, z1), (W, y, z1), (W, y + rise, z1), (-W, y + rise, z1), "flat", col=scale_col(ALU, 0.8))
        rep = 2 * W / 3.0
        bd.quad((-W, y + 0.02, z0 + 0.2), (W, y + 0.02, z0 + 0.2), (W, y + 1.1, z0 + 0.62), (-W, y + 1.1, z0 + 0.62),
                "crowd", uv=((0, 1), (rep, 1), (rep, 0), (0, 0)))
    back_z, top_y = rows * tread, y0 + rows * rise
    # The front riser and the open frame underneath.
    bd.quad((-W, 0, 0), (W, 0, 0), (W, y0, 0), (-W, y0, 0), "flat", col=scale_col(ALU, 0.7))
    for sx in (-1, 1):
        x = sx * W
        bd.poly([(x, 0, 0), (x, y0, 0), (x, top_y, back_z), (x, 0, back_z)], "flat", None, scale_col(STEEL, 0.75))
    # A back rail.
    bd.box((0, top_y + 0.9, back_z), (W, 0.03, 0.03), 0, "flat", STEEL, bottom=True)
    for x in (-W, -W / 2, 0, W / 2, W):
        bd.box((x, top_y + 0.45, back_z), (0.03, 0.45, 0.03), 0, "flat", STEEL)
    # The shade sail: posts at the corners, the canvas sloping to the back.
    zf, zb = -0.8, back_z + 0.6
    yf, yb = top_y + 2.6, top_y + 1.8
    for sx in (-1, 1):
        for z, y in ((zf, yf), (zb, yb)):
            bd.tube((sx * (W + 0.3), 0, z), (sx * (W + 0.3), y, z), 0.08, 0.08, 4, "flat", STEEL, smooth=False)
    sail = [(-W - 0.8, yf, zf), (W + 0.8, yf, zf), (W + 0.8, yb, zb), (-W - 0.8, yb, zb)]
    bd.poly(sail, "flat", None, SHADE)
    bd.poly([(p[0], p[1] - 0.04, p[2]) for p in sail], "flat", None, scale_col(SHADE, 0.72))
    return bd.finish()


def build_canopy(name, col):
    """A 3 m pop-up canopy in the pits: four thin legs, a peaked top with
    a valance round its edge."""
    bd = Builder(name)
    h, hw, peak = 2.1, 1.5, 0.75
    for sx in (-1, 1):
        for sz in (-1, 1):
            bd.box((sx * hw, h / 2, sz * hw), (0.03, h / 2, 0.03), 0, "flat", STEEL)
    corners = [(-hw, h + 0.2, -hw), (hw, h + 0.2, -hw), (hw, h + 0.2, hw), (-hw, h + 0.2, hw)]
    apex = (0, h + 0.2 + peak, 0)
    for i in range(4):
        a, b_ = corners[i], corners[(i + 1) % 4]
        bd.poly([a, b_, apex], "flat", None, [col, col, scale_col(col, 1.15)])
        # The valance hangs straight down 0.25 m.
        bd.poly([(a[0], a[1] - 0.25, a[2]), (b_[0], b_[1] - 0.25, b_[2]), b_, a], "flat", None, scale_col(col, 0.85))
    return bd.finish()


def build_container(name, col):
    """A 20 ft shipping container: corrugated sides tinted, doors at one end."""
    bd = Builder(name)
    L, W, H = 3.05, 1.22, 2.6
    bd.box((0, H / 2, 0), (L, H / 2, W), 0, "metal", col, uv_scale=0.6)
    for sz in (-1, 1):
        bd.box((L + 0.02, H / 2, sz * 0.62), (0.02, H / 2 - 0.05, 0.58), 0, "flat", scale_col(col, 0.8))
    for x in (-L, L):
        for sz in (-1, 1):
            bd.box((x, H / 2, sz * W), (0.08, H / 2, 0.08), 0, "flat", scale_col(col, 0.6))
    return bd.finish()


def build_clubhouse():
    """The clubhouse and kart shop: a low stucco block with a flat roof and
    parapet, a metal awning on posts along the front, dark shop windows and
    the circuit's sign on the fascia. Front is -z."""
    bd = Builder("clubhouse")
    hw, hd, H = 7.0, 3.6, 3.6
    bd.box((0, H / 2, 0), (hw, H / 2, hd), 0, "stucco", WHITE, uv_scale=0.4)
    bd.box((0, H + 0.25, 0), (hw + 0.1, 0.25, hd + 0.1), 0, "stucco", scale_col(WHITE, 0.94), uv_scale=0.4)
    for x0 in (-6.2, -2.2, 2.6):
        bd.quad((x0, 0.6, -hd - 0.02), (x0 + 3.4, 0.6, -hd - 0.02), (x0 + 3.4, 2.4, -hd - 0.02), (x0, 2.4, -hd - 0.02),
                "flat", col=GLASS)
    bd.quad((0.9, 0, -hd - 0.03), (2.1, 0, -hd - 0.03), (2.1, 2.3, -hd - 0.03), (0.9, 2.3, -hd - 0.03), "flat",
            col=DOOR)
    # The sign stands on the roof's front edge, clear of the parapet, on a
    # framed board the texture's shape (4:1).
    sz = -hd - 0.15
    bd.box((0, 4.55, sz), (3.35, 0.9, 0.05), 0, "flat", rgba(60, 60, 66))
    bd.quad((-3.2, 3.75, sz - 0.06), (3.2, 3.75, sz - 0.06), (3.2, 5.35, sz - 0.06), (-3.2, 5.35, sz - 0.06),
            "banner", uv=((0, 1), (1, 1), (1, 0), (0, 0)))
    # The awning, sloping out from under the sign.
    a0, a1 = (-hw, 2.7, -hd), (hw, 2.5, -hd - 2.6)
    bd.poly([(a0[0], a0[1], a0[2]), (hw, a0[1], a0[2]), (hw, a1[1], a1[2]), (-hw, a1[1], a1[2])], "metal",
            [(0, 0), (4, 0), (4, 1), (0, 1)], STEEL)
    bd.poly([(-hw, a0[1] - 0.05, a0[2]), (hw, a0[1] - 0.05, a0[2]), (hw, a1[1] - 0.05, a1[2]),
             (-hw, a1[1] - 0.05, a1[2])], "flat", None, scale_col(STEEL, 0.5))
    for x in (-hw + 0.3, -hw / 3, hw / 3, hw - 0.3):
        bd.box((x, 1.25, -hd - 2.45), (0.06, 1.25, 0.06), 0, "flat", STEEL)
    # A swamp cooler on the roof.
    bd.box((3.5, H + 0.95, 1.0), (0.7, 0.45, 0.6), 0, "metal", STEEL, uv_scale=0.8)
    return bd.finish()


def build_trailer():
    """An enclosed kart trailer: a white box on two axles with a hitch."""
    bd = Builder("trailer")
    L, W, H = 3.0, 1.2, 1.4
    bd.box((0, 0.65 + H, 0), (L, H, W), 0, "metal", rgba(236, 236, 232), uv_scale=0.4)
    bd.quad((-L, 1.0, -W - 0.02), (L, 1.0, -W - 0.02), (L, 1.25, -W - 0.02), (-L, 1.25, -W - 0.02), "flat",
            col=rgba(200, 44, 36))
    bd.quad((L, 1.0, W + 0.02), (-L, 1.0, W + 0.02), (-L, 1.25, W + 0.02), (L, 1.25, W + 0.02), "flat",
            col=rgba(200, 44, 36))
    for x in (-0.5, 0.5):
        for sz in (-1, 1):
            bd.tube((x, 0.34, sz * (W + 0.02)), (x, 0.34, sz * (W + 0.24)), 0.34, 0.34, 6, "flat", TYRE, smooth=False)
    bd.box((L + 0.8, 0.45, 0), (0.8, 0.05, 0.05), 0, "flat", DARK, bottom=True)
    return bd.finish()


# ---------------------------------------------------------------------------
# Trackside

def build_flagstand():
    """The starter's stand beside the line: a raised platform on four legs
    with a rail, a ladder and a little sun roof."""
    bd = Builder("flagstand")
    h, hw = 2.4, 0.9
    for sx in (-1, 1):
        for sz in (-1, 1):
            bd.box((sx * hw, h / 2, sz * hw), (0.05, h / 2, 0.05), 0, "flat", STEEL)
    bd.box((0, h, 0), (hw + 0.1, 0.06, hw + 0.1), 0, "flat", ALU, bottom=True)
    bd.box((0, h + 0.95, -hw - 0.05), (hw + 0.05, 0.03, 0.03), 0, "flat", WHITE, top=False)
    for sx in (-1, 1):
        bd.box((sx * (hw + 0.05), h + 0.95, 0), (0.03, 0.03, hw + 0.05), 0, "flat", WHITE, top=False)
        bd.box((sx * (hw + 0.05), h + 1.5, -hw), (0.04, 1.5, 0.04), 0, "flat", STEEL)
        bd.box((sx * (hw + 0.05), h + 1.5, hw), (0.04, 1.5, 0.04), 0, "flat", STEEL)
    bd.quad((-hw - 0.3, h + 3.0, -hw - 0.3), (hw + 0.3, h + 3.0, -hw - 0.3), (hw + 0.3, h + 2.8, hw + 0.3),
            (-hw - 0.3, h + 2.8, hw + 0.3), "flat", col=rgba(226, 40, 30))
    bd.quad((-hw - 0.3, h + 2.96, -hw - 0.3), (hw + 0.3, h + 2.96, -hw - 0.3), (hw + 0.3, h + 2.76, hw + 0.3),
            (-hw - 0.3, h + 2.76, hw + 0.3), "flat", col=ROOF_UNDER)
    for k in range(4):
        y = 0.4 + k * 0.55
        bd.box((0, y, hw + 0.3), (0.3, 0.025, 0.025), 0, "flat", STEEL, top=False)
    for sx in (-1, 1):
        bd.box((sx * 0.3, h / 2, hw + 0.3), (0.03, h / 2, 0.03), 0, "flat", STEEL)
    return bd.finish()


def build_tyres():
    """A stack of five old tyres, one painted white."""
    bd = Builder("tyres")
    for k in range(5):
        y = k * 0.22
        col = TYRE_WHITE if k == 3 else scale_col(TYRE, 1.0 + 0.1 * (k % 2))
        bd.tube((0, y, 0), (0, y + 0.21, 0), 0.33, 0.33, 7, "flat", col, col, smooth=True, phase=k * 0.4)
    bd.disc((0, 1.1, 0), 0.33, 7, "flat", scale_col(TYRE, 0.7), phase=1.6)
    return bd.finish()


def build_pole():
    """A floodlight pole: a 10 m galvanised mast with a cross-arm and two
    lamp heads, for the night races the club runs."""
    bd = Builder("pole")
    bd.tube((0, 0, 0), (0, 10.0, 0), 0.16, 0.09, 5, "flat", STEEL, scale_col(STEEL, 1.1))
    bd.box((0, 9.9, 0), (1.3, 0.06, 0.06), 0, "flat", STEEL, bottom=True)
    for x in (-1.1, 1.1):
        bd.box((x, 9.7, -0.15), (0.35, 0.22, 0.12), 0, "flat", DARK, bottom=True)
        bd.quad((x - 0.3, 9.52, -0.28), (x + 0.3, 9.52, -0.28), (x + 0.3, 9.88, -0.28), (x - 0.3, 9.88, -0.28), "flat",
                col=rgba(250, 248, 220))
    return bd.finish()


def build_fence():
    """12 m of black windscreen fence on steel posts, the shade cloth the
    spectator areas are lined with."""
    bd = Builder("fence")
    L, H = 6.0, 1.8
    bd.quad((-L, 0.05, 0), (L, 0.05, 0), (L, H, 0), (-L, H, 0), "flat", col=rgba(30, 32, 34))
    for k in range(5):
        x = -L + k * L / 2
        bd.box((x, H / 2 + 0.05, 0.05), (0.04, H / 2 + 0.05, 0.04), 0, "flat", STEEL)
    return bd.finish()


def build_rocks():
    """A pile of desert boulders: four lumpy, rounded granite rocks."""
    bd = Builder("rocks")
    rng = random.Random(5)
    for cx, cz, r in ((0, 0, 1.3), (1.5, 0.6, 0.9), (-1.2, 0.8, 0.8), (0.4, -1.1, 0.6)):
        segs, rings = 7, 4
        pts = []
        for i in range(rings + 1):
            el = -0.25 + (math.pi / 2 + 0.25) * i / rings
            ring = []
            for j in range(segs):
                a = j / segs * math.tau
                k = r * (0.8 + 0.35 * rng.random())
                ring.append((cx + math.cos(el) * math.sin(a) * k, max(0.0, math.sin(el) * k * 0.8) - 0.1,
                             cz + math.cos(el) * math.cos(a) * k))
            pts.append(ring)
        top = (cx, r * 0.85, cz)
        for i in range(rings):
            for j in range(segs):
                q = (j + 1) % segs
                u0, u1 = j / segs * 3, (j + 1) / segs * 3
                bd.poly([pts[i][j], pts[i][q], pts[i + 1][q], pts[i + 1][j]], "rock",
                        [(u0, i / 2), (u1, i / 2), (u1, (i + 1) / 2), (u0, (i + 1) / 2)], WHITE, smooth=True)
        for j in range(segs):
            q = (j + 1) % segs
            bd.poly([pts[rings][j], pts[rings][q], top], "rock", [(0, 0), (1, 0), (0.5, 0.5)], WHITE, smooth=True)
    return bd.finish()


# ---------------------------------------------------------------------------
# The desert

def build_joshua(name, seed):
    """A Joshua tree: a shaggy trunk forking into crooked arms that bend
    upward, each ending in a burst of dagger leaves (three cut-out planes
    crossing). About 5 m tall; the game scales each tree."""
    rng = random.Random(seed)
    bd = Builder(name)
    trunk_top = Vector((rng.uniform(-0.2, 0.2), 2.3, rng.uniform(-0.2, 0.2)))
    bd.tube((0, -0.1, 0), trunk_top, 0.32, 0.24, 6, "jbark", WHITE, WHITE, 1.0, 1.6)
    tips = []
    narms = rng.choice((3, 4))
    for k in range(narms):
        a = k / narms * math.tau + rng.uniform(-0.4, 0.4)
        out = Vector((math.sin(a), 0, math.cos(a)))
        elbow = trunk_top + out * rng.uniform(0.8, 1.3) + Vector((0, rng.uniform(0.3, 0.7), 0))
        tip = elbow + out * rng.uniform(0.2, 0.5) + Vector((0, rng.uniform(0.8, 1.4), 0))
        bd.tube(trunk_top - Vector((0, 0.2, 0)), elbow, 0.17, 0.14, 4, "jbark", WHITE, WHITE, 0.5, 1.0, phase=a)
        bd.tube(elbow, tip, 0.14, 0.12, 4, "jbark", WHITE, WHITE, 0.5, 1.0, phase=a)
        tips.append(tip)
        # Sometimes a short second fork off the arm.
        if rng.random() < 0.5:
            t2 = elbow + Vector((rng.uniform(-0.6, 0.6), rng.uniform(0.6, 1.0), rng.uniform(-0.6, 0.6)))
            bd.tube(elbow, t2, 0.11, 0.09, 4, "jbark", WHITE, WHITE, 0.5, 1.0, phase=a)
            tips.append(t2)
    lo, hi = rgba(190, 196, 170), WHITE
    for tip in tips:
        r = rng.uniform(0.7, 0.9)
        for q in range(2):
            a = q / 2 * math.pi + rng.uniform(0, 1.0)
            u = Vector((math.cos(a), 0, -math.sin(a)))
            c = tip + Vector((0, r * 0.15, 0))
            pts = [c - u * r - Vector((0, r, 0)), c + u * r - Vector((0, r, 0)), c + u * r + Vector((0, r, 0)),
                   c - u * r + Vector((0, r, 0))]
            bd.poly([tuple(p) for p in pts], "joshua", [(0, 1), (1, 1), (1, 0), (0, 0)], [lo, lo, hi, hi])
        c = tip + Vector((0, r * 0.25, 0))
        bd.poly([tuple(c + Vector((-r, 0, -r))), tuple(c + Vector((r, 0, -r))), tuple(c + Vector((r, 0, r))),
                 tuple(c + Vector((-r, 0, r)))], "joshua", [(0, 1), (1, 1), (1, 0), (0, 0)], hi)
    return bd.finish()


def build_scrub():
    """A creosote bush: two cut-out planes crossing, 1.8 m across."""
    bd = Builder("scrub")
    for a in (0.3, 0.3 + math.pi / 2):
        u = Vector((math.cos(a), 0, -math.sin(a)))
        pts = [-u * 0.9, u * 0.9, u * 0.9 + Vector((0, 1.3, 0)), -u * 0.9 + Vector((0, 1.3, 0))]
        bd.poly([tuple(p + Vector((0, -0.05, 0))) for p in pts], "scrub", [(0, 1), (1, 1), (1, 0), (0, 0)],
                [rgba(200, 200, 190), rgba(200, 200, 190), WHITE, WHITE])
    return bd.finish()


def build_peak(name="peak", coarse=False):
    """A unit desert mountain like the one behind the track in the photos:
    a height field over a disc 2.4 across, 1 high at a rounded summit with
    a lower shoulder to the east and a knob to the west. Sharp-crested
    ridges with V gullies between run down every flank, twisting a little
    as they go, the gullies in shadow; dark varnished rock above pale fans
    of scree at the foot. UVs wrap the rock texture round it (u) and down
    it (v), so its streaks follow the gullies. The game scales it up and
    draws it far away, without fog."""
    bd = Builder(name)
    segs = 24 if coarse else 48
    rings = 6 if coarse else 12
    R = 1.2
    peaks = ((0.0, 0.0, 1.0, 0.55), (0.55, 0.12, 0.88, 0.42), (-0.52, -0.15, 0.62, 0.34), (0.2, -0.48, 0.5, 0.32))
    def ridge(a, t):
        """0 in a gully to 1 on a crest; the crests twist as they fall."""
        a2 = a + 0.6 * t
        return (0.5 * (1 - abs(math.sin(a2 * 6 + 0.6))) ** 1.5 + 0.3 * (1 - abs(math.sin(a2 * 11 + 2.1))) ** 1.5
                + 0.2 * (1 - abs(math.sin(a2 * 19 + 4.0))) ** 2)
    def height(x, z):
        h = 0.0
        for px, pz, ph, pr in peaks:
            d2 = ((x - px) ** 2 + (z - pz) ** 2) / (pr * pr)
            h = max(h, ph * math.exp(-d2 * 0.9))
        a = math.atan2(x, z)
        t = math.hypot(x, z) / R
        # The ridges cut deepest down the middle of the slopes.
        relief = 0.26 * min(1.0, t * 3.0)
        h = h * (1 - relief + relief * ridge(a, t)) * (1 - t ** 3)
        return max(0.0, h)
    foot = rgba(214, 186, 142)
    mid = rgba(158, 118, 88)
    dark = rgba(104, 74, 60)
    top = rgba(136, 100, 78)
    sun = rgba(240, 198, 150)
    grid = []
    for k in range(rings + 1):
        t = (k / rings) ** 0.8
        r = R * t
        ring = []
        for i in range(segs if k else 1):
            a = i / segs * math.tau
            x, z = math.sin(a) * r, math.cos(a) * r
            h = height(x, z)
            if h < 0.08:
                col = mix(foot, mid, h / 0.08)
            elif h < 0.55:
                col = mix(mid, dark, (h - 0.08) / 0.47)
            else:
                col = mix(dark, top, min(1.0, (h - 0.55) / 0.45))
            col = scale_col(col, 0.74 + 0.4 * ridge(a, t))    # crests catch the light, gullies sit in shadow
            lit = 0.5 + 0.5 * math.cos(a - 4.4)   # the west face catches the afternoon sun
            col = mix(col, sun, 0.2 * lit)
            ring.append(((x, h, z), col, h * 3.0))
        grid.append(ring)
    U = 6.0 / segs   # the texture six times round
    (pc, cc, vc) = grid[0][0]
    for i in range(segs):
        j = (i + 1) % segs
        (p1, c1, v1), (p2, c2, v2) = grid[1][i], grid[1][j]
        bd.poly([pc, p1, p2], "flat", [((i + 0.5) * U, vc), (i * U, v1), ((i + 1) * U, v2)], [cc, c1, c2], smooth=True)
    for k in range(1, rings):
        for i in range(segs):
            j = (i + 1) % segs
            (p0, c0, v0), (p1, c1, v1) = grid[k + 1][i], grid[k + 1][j]
            (p2, c2, v2), (p3, c3, v3) = grid[k][j], grid[k][i]
            bd.poly([p0, p1, p2, p3], "flat", [(i * U, v0), ((i + 1) * U, v1), ((i + 1) * U, v2), (i * U, v3)],
                    [c0, c1, c2, c3], smooth=True)
    return bd.finish()


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--blend", type=Path)
    ap.add_argument("--preview", type=Path)
    a = ap.parse_args(argv)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    for name in MATERIALS:
        bpy.data.materials.new(name)
    props = [
        ("avp_peak", build_peak(), False),
        ("avp_peak_lo", build_peak("peak_lo", True), False),
        ("avp_tower", build_tower(), True),
        ("avp_stand", build_stand(), True),
        ("avp_canopy_r", build_canopy("canopy_r", rgba(214, 48, 40)), True),
        ("avp_canopy_b", build_canopy("canopy_b", rgba(40, 96, 200)), True),
        ("avp_canopy_y", build_canopy("canopy_y", rgba(240, 196, 50)), True),
        ("avp_container_r", build_container("container_r", RUST), True),
        ("avp_container_t", build_container("container_t", TEAL), True),
        ("avp_clubhouse", build_clubhouse(), True),
        ("avp_trailer", build_trailer(), True),
        ("avp_flagstand", build_flagstand(), True),
        ("avp_tyres", build_tyres(), True),
        ("avp_pole", build_pole(), True),
        ("avp_fence", build_fence(), True),
        ("avp_rocks", build_rocks(), True),
        ("avp_joshua_a", build_joshua("joshua_a", 7), True),
        ("avp_joshua_b", build_joshua("joshua_b", 19), True),
        ("avp_scrub", build_scrub(), True),
    ]
    blender_props.bake(props, MATERIALS, PT_MATERIALS, a.header, a.blend, a.preview,
                       script="tools/build_apple_valley_props.py", guard="DK_APPLE_VALLEY_PROPS_H",
                       label="apple valley prop", unit_note="the mountain: millimetres of a unit mountain",
                       mat_prefix="AVM", prop_prefix="AVP", table="av_props")


main()
