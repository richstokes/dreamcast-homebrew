#!/usr/bin/env python3
"""Model Bay City Hills' props in Blender and bake them for the game.

Run through Blender, not the host Python interpreter (``make sf-props``
does it):

    blender --background --python tools/build_sf_props.py -- \
        --header assets/generated/sf_props.h \
        --blend captures/sf_props.blend --preview captures/sf-props

Every prop is built procedurally with bmesh: the Golden Gate Bridge with
the headlands either side of the strait, Alcatraz, Coit Tower, the pyramid
tower downtown, a cable car, a bay ferry, the street lamps, date
palms, a fishing boat, a raft of sea lions, the wharf's sign arch and a
Monterey cypress. Each face gets a material from the fixed list below (the
game maps each to a texture) and vertex colours, then the meshes are cut
into PowerVR strips per material and written as a header, as
tools/build_apple_valley_props.py does for Apple Valley.
"""

from __future__ import annotations

import argparse
import math
import random
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
import blender_props  # noqa: E402
from blender_props import WHITE, mix, rgba, scale_col  # noqa: E402

# Material order is the game's SFM_* enum (src/world_sf.c maps them to headers).
MATERIALS = ["flat", "palm", "palmbark", "sign", "pier", "bldg", "cablecar"]
# Punch-through (alpha tested) materials go in their own list in the game.
PT_MATERIALS = {"palm"}

ORANGE = rgba(200, 66, 38)
ORANGE_D = rgba(150, 46, 30)
CONCRETE = rgba(196, 192, 182)
CREAM = rgba(238, 230, 212)
GLASS = rgba(44, 58, 76)
MAROON = rgba(128, 30, 36)
GOLD = rgba(206, 164, 66)
IRON = rgba(40, 70, 58)
DARK = rgba(36, 36, 40)
ROCK = rgba(128, 116, 100)
SCRUB = rgba(96, 112, 64)
HILL = rgba(150, 142, 92)
HILL_D = rgba(96, 108, 66)


class Builder(blender_props.Builder):
    MATERIALS = MATERIALS

    def tbox(self, y0, y1, c, h0, h1, mat, col=WHITE, col_top=None, top=True):
        """A box tapering from half extents h0 (x, z) at y0 to h1 at y1."""
        cx, cz = c
        lo = [(cx - h0[0], y0, cz - h0[1]), (cx + h0[0], y0, cz - h0[1]), (cx + h0[0], y0, cz + h0[1]),
              (cx - h0[0], y0, cz + h0[1])]
        hi = [(cx - h1[0], y1, cz - h1[1]), (cx + h1[0], y1, cz - h1[1]), (cx + h1[0], y1, cz + h1[1]),
              (cx - h1[0], y1, cz + h1[1])]
        for i in range(4):
            j = (i + 1) % 4
            self.poly([lo[i], lo[j], hi[j], hi[i]], mat, None, col)
        if top:
            self.poly(hi, mat, None, col_top or col)

    def blob(self, c, r, sides, rings, mat, col, col_top=None, squash=1.0, yaw=0.0, stretch=1.0):
        """A low ellipsoid: radius r across, squashed in y, stretched along
        its own z (turned by yaw); darker underneath."""
        col_top = col_top or col
        cy, sy = math.cos(yaw), math.sin(yaw)
        grid = []
        for k in range(rings + 1):
            phi = -math.pi / 2 + math.pi * k / rings
            row = []
            for i in range(sides):
                a = i / sides * math.tau
                x, z = math.cos(phi) * math.sin(a) * r, math.cos(phi) * math.cos(a) * r * stretch
                y = math.sin(phi) * r * squash
                row.append((c[0] + x * cy + z * sy, c[1] + y, c[2] - x * sy + z * cy))
            grid.append((row, mix(scale_col(col, 0.6), col_top, k / rings)))
        for k in range(rings):
            (r0, c0), (r1, c1) = grid[k], grid[k + 1]
            for i in range(sides):
                j = (i + 1) % sides
                self.poly([r0[i], r0[j], r1[j], r1[i]], mat, None, [c0, c0, c1, c1], smooth=True)


def scaled(bd, unit):
    """Shrink a prop modelled in metres to units of `unit` metres (the
    game scales it back up), so a big one fits the int16 export."""
    bmesh.ops.scale(bd.bm, vec=Vector((1 / unit, 1 / unit, 1 / unit)), verts=bd.bm.verts)


# ---------------------------------------------------------------------------
# The far scenery

def headland(bd, z0, z1, x0, x1, top, seed, sign):
    """A grassy headland: a heightfield over (x0..x1, z0..z1) rising from
    the water at the strait (z0 for sign +1) to top, lumpy, with cliffs
    at the waterline."""
    rnd = random.Random(seed)
    nx, nz = 8, 5
    bumps = [(rnd.uniform(x0, x1), rnd.uniform(z0, z1), rnd.uniform(60, 160), rnd.uniform(0.2, 0.5)) for _ in range(6)]
    def h(x, z):
        t = (z - z0) / (z1 - z0) if sign > 0 else (z1 - z) / (z1 - z0)
        y = top * min(1.0, t * 1.6) ** 0.7
        for bx, bz, br, bh in bumps:
            d = math.hypot(x - bx, z - bz) / br
            y += top * bh * max(0.0, 1 - d * d)
        e = min(x - x0, x1 - x) / (x1 - x0) * 4
        y *= min(1.0, max(0.0, e))
        return y - 6
    grid = [[(x0 + (x1 - x0) * i / nx, 0, z0 + (z1 - z0) * k / nz) for i in range(nx + 1)] for k in range(nz + 1)]
    grid = [[(x, h(x, z), z) for x, _, z in row] for row in grid]
    for k in range(nz):
        for i in range(nx):
            q = [grid[k][i], grid[k][i + 1], grid[k + 1][i + 1], grid[k + 1][i]]
            ys = sum(p[1] for p in q) / 4
            col = mix(HILL_D, HILL, min(1.0, max(0.0, ys / top)))
            if ys < 4:
                col = ROCK
            bd.poly(q, "flat", None, col, smooth=True)


def build_bridge():
    """The Golden Gate Bridge, at about half its real size, across a strait
    along z with a headland at each end: two towers 105 m tall, their
    legs joined by portal struts, a 520 m main span and two side spans
    hung from cables that sag between the tower tops, the deck's
    stiffening truss, the anchorages and the concrete piers. Modelled in
    metres and exported in units of 30 m."""
    bd = Builder("bridge")
    deck_y0, deck_y1, hw = 27.0, 33.0, 13.0
    towers = (-260.0, 260.0)
    for tz in towers:
        bd.box((0, 2, tz), (16, 6, 9), 0, "flat", CONCRETE, top=True)
        for sx in (-1, 1):
            bd.tbox(8, 105, (sx * 11.0, tz), (2.8, 3.8), (1.9, 2.7), "flat", ORANGE, scale_col(ORANGE, 1.1))
        for y, hgt in ((38.0, 5.0), (66.0, 4.0), (96.0, 5.0)):
            bd.box((0, y, tz), (9.5, hgt / 2, 1.8), 0, "flat", ORANGE_D, bottom=True)
        bd.box((0, 104, tz), (13.5, 1.2, 3.2), 0, "flat", ORANGE, bottom=True)
    # The deck: road on top, the truss's orange sides and dark underside.
    z0, z1 = -430.0, 430.0
    bd.quad((-hw, deck_y1, z0), (hw, deck_y1, z0), (hw, deck_y1, z1), (-hw, deck_y1, z1), "flat", col=rgba(110, 106, 104))
    for sx in (-1, 1):
        bd.quad((sx * hw, deck_y0, z0), (sx * hw, deck_y0, z1), (sx * hw, deck_y1, z1), (sx * hw, deck_y1, z0), "flat",
                col=ORANGE)
    bd.quad((-hw, deck_y0, z1), (hw, deck_y0, z1), (hw, deck_y0, z0), (-hw, deck_y0, z0), "flat", col=ORANGE_D)
    # The cables: a parabola across the main span, the side spans down to
    # the anchorages, each a four-sided tube whose rings are shared from
    # one length to the next, so each side is one long strip.
    def cable(sx, za, zb, ya, yb, sag, segs):
        rings = []
        for i in range(segs + 1):
            t = i / segs
            y = ya + (yb - ya) * t - sag * 4 * t * (1 - t)
            dy = (yb - ya) - sag * 4 * (1 - 2 * t)
            tan = Vector((0, dy, zb - za)).normalized()
            up = Vector((1, 0, 0)).cross(tan).normalized()
            c = Vector((sx * 12.0, y, za + (zb - za) * t))
            rings.append([c + (Vector((1.3, 0, 0)) * math.cos(a) + up * 1.3 * math.sin(a))
                          for a in (k * math.pi / 2 + math.pi / 4 for k in range(4))])
        for i in range(segs):
            for k in range(4):
                j = (k + 1) % 4
                bd.poly([tuple(rings[i][k]), tuple(rings[i][j]), tuple(rings[i + 1][j]), tuple(rings[i + 1][k])], "flat",
                        None, ORANGE, smooth=True)
    for sx in (-1, 1):
        cable(sx, towers[0], towers[1], 106, 106, 70, 12)
        cable(sx, -415, towers[0], 36, 106, 14, 4)
        cable(sx, towers[1], 415, 106, 36, 14, 4)
        # Suspenders: a few thick enough to read at a distance.
        for k in range(1, 6):
            z = towers[0] + (towers[1] - towers[0]) * k / 6
            t = (z - towers[0]) / (towers[1] - towers[0])
            y = 106 - 70 * 4 * t * (1 - t)
            bd.box((sx * 12.0, (y + deck_y1) / 2, z), (0.35, (y - deck_y1) / 2, 0.35), 0, "flat", ORANGE_D, top=False)
    for az in (-420.0, 420.0):
        bd.box((0, 18, az), (16, 20, 14), 0, "flat", CONCRETE)
    headland(bd, -700, -420, -420, 420, 70, 11, -1)
    headland(bd, 420, 820, -460, 460, 150, 23, 1)
    for f in bd.bm.faces:
        f.smooth = True   # far off: shared vertices make long strips
    scaled(bd, 30.0)
    return bd.finish()


def build_alcatraz():
    """Alcatraz, at about half size: a rocky island with cliffs all round,
    scrub on its slopes, the cellhouse along the top, the lighthouse at
    one end, a water tower and the old barracks by the dock. Exported in
    units of 10 m."""
    bd = Builder("alcatraz")
    rnd = random.Random(31)
    rings = [(1.0, -4), (0.97, 8), (0.82, 18), (0.55, 24), (0.0, 26)]
    sides = 18
    pts = []
    for fr, y in rings:
        row = []
        for i in range(sides):
            a = i / sides * math.tau
            wob = 1 + 0.12 * math.sin(a * 3 + 0.5) + rnd.uniform(-0.04, 0.04) if fr < 1 else 1 + 0.12 * math.sin(a * 3 + 0.5)
            row.append((math.sin(a) * 120 * fr * wob, y + (rnd.uniform(-1.5, 1.5) if 0 < fr < 1 else 0),
                        math.cos(a) * 46 * fr * wob))
        pts.append(row)
    for k in range(len(rings) - 1):
        for i in range(sides):
            j = (i + 1) % sides
            col = ROCK if k < 1 else (SCRUB if k < 3 else mix(SCRUB, ROCK, 0.4))
            bd.poly([pts[k][i], pts[k][j], pts[k + 1][j], pts[k + 1][i]], "flat", None, col, smooth=k > 0)
    bd.box((0, 31, 4), (38, 6, 11), 0.05, "bldg", CREAM, uv_scale=0.12)
    bd.box((-10, 38, 4), (16, 1.5, 7), 0.05, "flat", rgba(160, 156, 150))
    bd.tbox(26, 52, (-46, 6), (2.6, 2.6), (1.8, 1.8), "flat", CREAM)
    bd.box((-46, 53.5, 6), (2.4, 1.6, 2.4), 0, "flat", GLASS)
    bd.box((48, 33, -10), (5, 4, 5), 0, "flat", rgba(200, 196, 188))
    bd.box((48, 44, -10), (6, 4, 6), 0, "flat", rgba(220, 216, 208))
    bd.box((70, 10, 22), (20, 6, 8), 0.4, "bldg", CREAM, uv_scale=0.12)
    for f in bd.bm.faces:
        f.smooth = True   # far off: shared vertices make long strips
    scaled(bd, 10.0)
    return bd.finish()


def build_coit():
    """Coit Tower: a fluted concrete shaft on a terrace, the ring of tall
    arched openings near the top and the parapet over them. The flutes
    are light and shade in alternate vertex colours round one smooth
    tube, so the shaft is one strip."""
    bd = Builder("coit")
    bd.tube((0, -3, 0), (0, 2.2, 0), 10.5, 10.0, 16, "flat", scale_col(CREAM, 0.9), CREAM)
    bd.disc((0, 2.2, 0), 10.0, 16, "flat", rgba(210, 204, 190), flip=True)
    sides = 32
    ring0, ring1 = [], []
    for i in range(sides):
        a = i / sides * math.tau
        r0, r1 = (5.6, 5.2) if i % 2 == 0 else (5.3, 4.9)
        ring0.append((math.sin(a) * r0, 2.2, math.cos(a) * r0))
        ring1.append((math.sin(a) * r1, 33.0, math.cos(a) * r1))
    for i in range(sides):
        j = (i + 1) % sides
        c0 = CREAM if i % 2 == 0 else scale_col(CREAM, 0.84)
        c1 = CREAM if j % 2 == 0 else scale_col(CREAM, 0.84)
        bd.poly([ring0[i], ring0[j], ring1[j], ring1[i]], "flat", None, [c0, c1, c1, c0], smooth=True)
    # The openings: a dark band round the top storey.
    bd.tube((0, 33.0, 0), (0, 33.6, 0), 5.25, 5.25, 16, "flat", CREAM)
    for i in range(16):
        a0, a1 = i / 16 * math.tau, (i + 1) / 16 * math.tau
        p = [(math.sin(a) * 5.25, y, math.cos(a) * 5.25) for a in (a0, a1) for y in (33.6, 37.6)]
        bd.poly([p[0], p[2], p[3], p[1]], "flat", None, [GLASS, GLASS, GLASS, GLASS], smooth=True)
    bd.tube((0, 37.6, 0), (0, 38.0, 0), 5.25, 5.25, 16, "flat", CREAM)
    bd.tube((0, 38.0, 0), (0, 39.6, 0), 5.8, 5.8, 16, "flat", CREAM)
    bd.disc((0, 39.6, 0), 5.8, 16, "flat", rgba(200, 194, 180), flip=True)
    bd.tube((0, 39.6, 0), (0, 41.5, 0), 3.6, 3.2, 12, "flat", CREAM)
    bd.disc((0, 41.5, 0), 3.2, 12, "flat", rgba(200, 194, 180), flip=True)
    return bd.finish()


def build_pyramid():
    """The pyramid tower downtown: a slender four-sided spire of white
    precast, 110 m tall, the two wings that rise out of its sides for the
    lifts and stairs, and the lit crown at the tip."""
    bd = Builder("pyramid")
    H, hb = 96.0, 11.0
    base = [(-hb, 0, -hb), (hb, 0, -hb), (hb, 0, hb), (-hb, 0, hb)]
    apex = (0, H, 0)
    for i in range(4):
        a, b_ = base[i], base[(i + 1) % 4]
        col = scale_col(rgba(232, 232, 228), 0.9 + 0.08 * (i % 2))
        bd.poly([a, b_, apex], "bldg", [(0, 6), (2.5, 6), (1.25, 0)], [col, col, col])
    for sx in (-1, 1):
        x0 = sx * hb * (1 - 46 / H)
        bd.poly([(x0, 46, -3.5), (x0, 46, 3.5), (sx * 1.0, 84, 3.5), (sx * 1.0, 84, -3.5)], "flat", None,
                rgba(220, 220, 216))
        bd.poly([(x0 + sx * 2.2, 46, -3.5), (sx * 1.0 + sx * 1.0, 84, -3.5), (sx * 1.0, 84, -3.5), (x0, 46, -3.5)],
                "flat", None, rgba(200, 200, 196))
        bd.poly([(x0, 46, 3.5), (sx * 1.0, 84, 3.5), (sx * 2.0, 84, 3.5), (x0 + sx * 2.2, 46, 3.5)], "flat", None,
                rgba(200, 200, 196))
        bd.poly([(x0 + sx * 2.2, 46, -3.5), (x0 + sx * 2.2, 46, 3.5), (sx * 2.0, 84, 3.5), (sx * 2.0, 84, -3.5)],
                "flat", None, rgba(236, 236, 232))
    bd.tube((0, H - 4, 0), (0, H + 16, 0), 0.6, 0.05, 4, "flat", rgba(250, 248, 230))
    # A base podium of dark glass.
    bd.box((0, 3, 0), (hb + 3, 3, hb + 3), 0, "flat", GLASS)
    return bd.finish()


# ---------------------------------------------------------------------------
# The street

# The cable car's texture atlas (tex_sfcablecar): each cell's (u0, v0, u1, v1).
CC_LOWER = (0.0, 0.0, 1.0, 0.25)       # the maroon side, full length
CC_CABIN = (0.0, 0.25, 1.0, 0.5)       # the cabin's side, its 4 m
CC_DASH = (0.0, 0.5, 0.5, 0.75)        # the end below the open section
CC_CABEND = (0.5, 0.5, 1.0, 0.75)      # the cabin's ends
CC_SIGN = (0.0, 0.75, 0.5, 0.875)      # the roof's destination sign
CC_CLERE = (0.5, 0.75, 1.0, 0.875)     # the clerestory's windows
CC_ROOF = (0.0, 0.875, 0.5, 1.0)       # roof boards
CC_WOOD = (0.5, 0.875, 1.0, 1.0)       # benches and running boards

SF_MAROON = MAROON
SKIN = [rgba(236, 196, 160), rgba(190, 140, 100), rgba(120, 82, 56), rgba(250, 214, 180)]
SHIRTS = [rgba(214, 60, 52), rgba(52, 110, 196), rgba(248, 210, 70), rgba(70, 160, 96), rgba(240, 240, 236),
          rgba(150, 80, 170)]


def build_cablecar():
    """Hyde Street's cable car, 8.4 m long, front +z: the maroon body on
    two trucks and their wheels, running boards along both sides, the
    closed cabin in the middle with its windows and the open sections at
    either end, their back-to-back benches and posts, the curved roof
    overhanging the ends with its clerestory and destination signs, and
    headlamps; riders on the benches and hanging off the running boards.
    The livery is the texture atlas (CC_* cells)."""
    bd = Builder("cablecar")
    L, W, CAB = 4.2, 1.25, 2.0
    Y0, YB, YR = 0.55, 1.65, 3.28     # body foot, top of the lower body, roof's eaves

    def face(bl, br, tr, tl, cell, mat="cablecar", col=WHITE):
        """A face seen from outside: corners bottom-left, bottom-right,
        top-right, top-left, the cell stretched over it."""
        u0, v0, u1, v1 = cell
        bd.poly([bl, br, tr, tl], mat, [(u0, v1), (u1, v1), (u1, v0), (u0, v0)], col)

    def sub(cell, a, b):
        """The part of a cell from a to b across it (0-1)."""
        u0, v0, u1, v1 = cell
        return (u0 + (u1 - u0) * a, v0, u0 + (u1 - u0) * b, v1)

    # The lower body: sides along its length, the dash at each end.
    for sx in (-1, 1):
        zl = -L if sx > 0 else L   # z at the viewer's left, seen from outside (the game's axes are left-handed)
        face((sx * W, Y0, zl), (sx * W, Y0, -zl), (sx * W, YB, -zl), (sx * W, YB, zl), CC_LOWER)
    for sz in (-1, 1):
        xl = W if sz > 0 else -W
        face((xl, Y0, sz * L), (-xl, Y0, sz * L), (-xl, YB, sz * L), (xl, YB, sz * L), CC_DASH)
    bd.quad((-W, YB, -L), (W, YB, -L), (W, YB, L), (-W, YB, L), "cablecar", uv=((0.52, 0.99), (0.98, 0.99), (0.98, 0.88), (0.52, 0.88)))
    # The cabin: windows down both sides, its ends facing the open sections.
    for sx in (-1, 1):
        zl = -CAB if sx > 0 else CAB
        face((sx * W, YB, zl), (sx * W, YB, -zl), (sx * W, YR, -zl), (sx * W, YR, zl), CC_CABIN)
    for sz in (-1, 1):
        xl = W if sz > 0 else -W
        face((xl, YB, sz * CAB), (-xl, YB, sz * CAB), (-xl, YR, sz * CAB), (xl, YR, sz * CAB), CC_CABEND)
    # The open sections: back-to-back benches, posts up to the roof.
    for sz in (-1, 1):
        zc = sz * (CAB + L) * 0.5
        hl = (L - CAB) * 0.5 - 0.1
        for sx in (-1, 1):
            bd.box((sx * 0.38, YB + 0.22, zc), (0.26, 0.04, hl), 0, "flat", rgba(150, 92, 48), bottom=True)
        bd.box((0, YB + 0.5, zc), (0.08, 0.32, hl), 0, "flat", SF_MAROON)
        for zp in (CAB + 0.75, CAB + 1.5, L - 0.05):
            for sx in (-1, 1):
                bd.box((sx * (W - 0.05), (YB + YR) * 0.5, sz * zp), (0.05, (YR - YB) * 0.5, 0.05), 0, "flat", SF_MAROON,
                       top=False)
        bd.box((0, (YB + YR) * 0.5, sz * (L - 0.05)), (W, 0.04, 0.04), 0, "flat", GOLD, top=False)
    # Running boards, wood, the length of the car on both sides.
    for sx in (-1, 1):
        bd.box((sx * (W + 0.2), Y0 + 0.02, 0), (0.2, 0.04, L - 0.15), 0, "cablecar", WHITE, uv_scale=0.01, bottom=True)
    # Trucks and wheels.
    for zt in (-2.7, 2.7):
        bd.box((0, 0.36, zt), (0.85, 0.2, 1.0), 0, "flat", DARK, bottom=True)
        for za in (-0.55, 0.55):
            for sx in (-1, 1):
                pts = [(sx * 0.9, 0.34 + 0.34 * math.sin(a), zt + za + 0.34 * math.cos(a))
                       for a in (k / 8 * math.tau for k in range(8))]
                if sx < 0:
                    pts.reverse()
                bd.poly(pts, "flat", None, rgba(52, 50, 50))
    # The roof: a shallow arch overhanging the ends, the clerestory on it.
    RW, RL, RISE = W + 0.2, L + 0.25, 0.2
    xs = [-RW + 2 * RW * k / 6 for k in range(7)]
    ys = [YR + RISE * (1 - (x / RW) ** 2) for x in xs]
    for k in range(6):
        a, b_ = (xs[k], ys[k]), (xs[k + 1], ys[k + 1])
        u0 = CC_ROOF[0] + (CC_ROOF[2] - CC_ROOF[0]) * k / 6
        u1 = CC_ROOF[0] + (CC_ROOF[2] - CC_ROOF[0]) * (k + 1) / 6
        bd.poly([(a[0], a[1], -RL), (b_[0], b_[1], -RL), (b_[0], b_[1], RL), (a[0], a[1], RL)], "cablecar",
                [(u0, CC_ROOF[3]), (u1, CC_ROOF[3]), (u1, CC_ROOF[1]), (u0, CC_ROOF[1])], WHITE, smooth=True)
    bd.quad((-RW, YR - 0.02, RL), (RW, YR - 0.02, RL), (RW, YR - 0.02, -RL), (-RW, YR - 0.02, -RL), "flat",
            col=rgba(96, 40, 32))
    for sz in (-1, 1):
        pts = [(x, y, sz * RL) for x, y in zip(xs, ys)]
        if sz < 0:
            pts.reverse()
        bd.poly(pts, "flat", None, rgba(226, 216, 190))
    CW, CY0, CY1, CL = 0.62, YR + RISE - 0.04, YR + RISE + 0.26, L - 0.5
    for sx in (-1, 1):
        zl = -CL if sx > 0 else CL
        face((sx * CW, CY0, zl), (sx * CW, CY0, -zl), (sx * CW, CY1, -zl), (sx * CW, CY1, zl), CC_CLERE)
    bd.quad((-CW, CY1, -CL), (CW, CY1, -CL), (CW, CY1, CL), (-CW, CY1, CL), "flat", col=rgba(232, 224, 200))
    # Destination signs above each end, a headlamp under each at the
    # roof's edge.
    for sz in (-1, 1):
        z = sz * (CL + 0.02)
        xl = 0.62 if sz > 0 else -0.62
        face((xl, CY0, z), (-xl, CY0, z), (-xl, CY1 + 0.05, z), (xl, CY1 + 0.05, z), CC_SIGN)
        hz = sz * (RL - 0.1)
        bd.tube((0, YR - 0.12, hz), (0, YR - 0.12, hz + sz * 0.16), 0.15, 0.13, 8, "flat", DARK)
        lens = [(0.12 * math.cos(a), YR - 0.12 + 0.12 * math.sin(a), hz + sz * 0.165) for a in (k / 8 * math.tau for k in range(8))]
        bd.poly(lens if sz > 0 else lens[::-1], "flat", None, rgba(255, 246, 200))
        bd.box((0, Y0 + 0.05, sz * (L + 0.08)), (W - 0.1, 0.08, 0.08), 0, "flat", DARK)
    # Riders: two on each end's benches facing out, and one hanging off
    # each side's running board, holding a post.
    rnd = random.Random(71)
    def rider(x, y, z, out, standing):
        shirt, skin = rnd.choice(SHIRTS), rnd.choice(SKIN)
        if standing:
            bd.box((x, y + 0.45, z), (0.13, 0.42, 0.13), 0, "flat", rgba(50, 54, 70))
            bd.box((x, y + 1.18, z), (0.19, 0.32, 0.15), 0, "flat", shirt)
            hy = y + 1.68
        else:
            bd.box((x + out * 0.18, y + 0.12, z), (0.2, 0.08, 0.14), 0, "flat", rgba(50, 54, 70))
            bd.box((x, y + 0.48, z), (0.17, 0.3, 0.15), 0, "flat", shirt)
            hy = y + 0.95
        bd.box((x, hy, z), (0.12, 0.13, 0.12), 0, "flat", skin)
    for sz in (-1, 1):
        for sx in (-1, 1):
            rider(sx * 0.42, YB + 0.26, sz * (CAB + 0.7 + 0.6 * rnd.random()), sx, False)
        sx = 1 if sz > 0 else -1
        rider(sx * (W + 0.22), Y0 + 0.06, sz * (CAB + 0.85), sx, True)
    # The small flat-coloured parts (riders, benches, posts, trucks) are
    # shaded smooth so their faces share vertices and chain into a few
    # long strips; the livery's faces stay crisp.
    flat = MATERIALS.index("flat")
    for f in bd.bm.faces:
        if f.material_index == flat:
            f.smooth = True
            for loop in f.loops:
                loop[bd.uv].uv = (0.0, 0.0)   # untextured: no seams between faces
    return bd.finish()


def build_ferry():
    """A bay commuter ferry, 40 m long, bow +z: a catamaran's twin white
    hulls with a blue line and a dark waterline under a broad main deck,
    two enclosed passenger decks with their window bands, the upper one
    shorter with an open deck aft, the wheelhouse forward on top with
    its windows raked, a radar mast and the rails round the open decks.
    Untextured: its faces share vertices across the hull's colours."""
    bd = Builder("ferry")
    WHITE_H, BLUE, GLASS_D = rgba(242, 244, 246), rgba(30, 90, 170), rgba(40, 56, 72)
    BOOT = rgba(60, 64, 70)
    # Twin hulls: a box each, pointed at the bow.
    for sx in (-1, 1):
        cx = sx * 4.0
        hull = [(-1.6, -19.0), (1.6, -19.0), (1.6, 15.0), (0.0, 20.5), (-1.6, 15.0)]
        n = len(hull)
        for i in range(n):
            (x0, z0), (x1, z1) = hull[i], hull[(i + 1) % n]
            for y0, y1, c0, c1 in ((-1.2, 0.3, BOOT, BOOT), (0.3, 1.4, WHITE_H, WHITE_H), (1.4, 1.9, BLUE, BLUE),
                                   (1.9, 2.6, WHITE_H, WHITE_H)):
                bd.poly([(cx + x0, y0, z0), (cx + x1, y0, z1), (cx + x1, y1, z1), (cx + x0, y1, z0)], "flat", None,
                        [c0, c0, c1, c1])
    # The main deck spanning both, its edge in blue.
    bd.box((0, 2.75, -1.0), (5.8, 0.15, 18.5), 0, "flat", WHITE_H, bottom=True)
    # Lower passenger deck: white with a dark window band.
    def deck(y0, y1, hw, z0, z1, band):
        bd.box((0, (y0 + y1) / 2, (z0 + z1) / 2), (hw, (y1 - y0) / 2, (z1 - z0) / 2), 0, "flat", WHITE_H)
        b0, b1 = band
        for sx in (-1, 1):
            x = sx * (hw + 0.06)   # proud of the wall: closer, they fought it for depth at a distance
            bd.quad((x, b0, z0 + 0.6), (x, b0, z1 - 0.6), (x, b1, z1 - 0.6), (x, b1, z0 + 0.6), "flat", col=GLASS_D)
        bd.quad((-hw + 0.5, b0, z1 + 0.06), (hw - 0.5, b0, z1 + 0.06), (hw - 0.5, b1, z1 + 0.06), (-hw + 0.5, b1, z1 + 0.06),
                "flat", col=GLASS_D)
        bd.quad((hw - 0.5, b0, z0 - 0.06), (-hw + 0.5, b0, z0 - 0.06), (-hw + 0.5, b1, z0 - 0.06), (hw - 0.5, b1, z0 - 0.06),
                "flat", col=GLASS_D)
    deck(2.9, 5.6, 5.2, -12.0, 13.0, (3.6, 5.1))
    deck(5.6, 8.1, 4.6, -6.0, 9.5, (6.2, 7.6))
    # The wheelhouse forward on top, its windows wrapping round.
    bd.box((0, 9.1, 7.0), (3.0, 1.0, 1.9), 0, "flat", WHITE_H)
    bd.quad((-2.9, 8.6, 8.96), (2.9, 8.6, 8.96), (2.9, 9.7, 8.96), (-2.9, 9.7, 8.96), "flat", col=GLASS_D)
    for sx in (-1, 1):
        bd.quad((sx * 3.06, 8.6, 5.6), (sx * 3.06, 8.6, 8.8), (sx * 3.06, 9.7, 8.8), (sx * 3.06, 9.7, 5.6), "flat",
                col=GLASS_D)
    bd.box((0, 10.2, 7.0), (3.3, 0.1, 2.2), 0, "flat", BLUE, bottom=True)
    # Radar mast, the radar bar and a light.
    bd.box((0, 11.6, 6.4), (0.12, 1.4, 0.12), 0, "flat", WHITE_H, top=False)
    bd.box((0, 12.4, 6.4), (1.1, 0.08, 0.15), 0, "flat", rgba(60, 60, 64))
    bd.box((0, 13.05, 6.4), (0.15, 0.15, 0.15), 0, "flat", rgba(255, 250, 220))
    # Rails round the open upper deck aft and the bow's foredeck.
    for z0, z1, y in ((-12.0, -6.0, 6.6), (13.0, 17.5, 3.8)):
        for sx in (-1, 1):
            bd.box((sx * 5.0, y, (z0 + z1) / 2), (0.05, 0.5, (z1 - z0) / 2), 0, "flat", rgba(220, 224, 228), top=True)
    # Funnel-like exhaust housings aft, either side.
    for sx in (-1, 1):
        bd.box((sx * 3.4, 9.0, -4.5), (0.6, 0.9, 1.2), 0, "flat", BLUE)
    for f in bd.bm.faces:
        f.smooth = True
        for loop in f.loops:
            loop[bd.uv].uv = (0.0, 0.0)
    return bd.finish()


def build_lamp():
    """A street lamp after the city's old ornamental posts: a fluted iron
    base, a tall tapering pole, a curled arm and a frosted globe."""
    bd = Builder("lamp")
    bd.tube((0, 0, 0), (0, 0.9, 0), 0.24, 0.16, 6, "flat", IRON, scale_col(IRON, 1.2), smooth=False)
    bd.tube((0, 0.9, 0), (0, 5.4, 0), 0.08, 0.06, 6, "flat", IRON, scale_col(IRON, 1.3))
    bd.tube((0, 5.3, 0), (0, 5.4, -0.9), 0.05, 0.04, 4, "flat", IRON)
    bd.tube((0, 5.0, -0.9), (0, 5.4, -0.9), 0.05, 0.05, 4, "flat", IRON)
    bd.blob((0, 4.75, -0.9), 0.28, 6, 4, "flat", rgba(250, 246, 226), rgba(255, 255, 250), squash=1.3)
    return bd.finish()


def build_palm():
    """A Canary Island date palm like the Embarcadero's: a thick, straight
    trunk of old leaf bases, a knob where the crown starts and a dense
    head of stiff, arching fronds."""
    bd = Builder("palm")
    H = 8.0
    bd.tube((0, 0, 0), (0, H, 0), 0.5, 0.42, 8, "palmbark", WHITE, u_rep=2.0, v_rep=H / 1.6)
    bd.blob((0, H + 0.2, 0), 0.75, 8, 3, "flat", rgba(110, 96, 50), rgba(140, 130, 64), squash=0.8)
    n = 16
    for i in range(n):
        a = i / n * math.tau + (0.2 if i % 2 else 0)
        up = 0.55 if i % 2 else 0.15
        d = Vector((math.sin(a), 0, math.cos(a)))
        side = Vector((math.cos(a), 0, -math.sin(a)))
        base = Vector((0, H + 0.3, 0))
        mid = base + d * 2.2 + Vector((0, 1.3 * up + 0.4, 0))
        tip = base + d * 4.3 + Vector((0, -1.0 + up, 0))
        w = 0.95
        pts = [(base, 1.0), (mid, 0.5), (tip, 0.0)]
        for (p0, v0), (p1, v1) in zip(pts, pts[1:]):
            bd.poly([tuple(p0 - side * w * 0.4), tuple(p0 + side * w * 0.4), tuple(p1 + side * w), tuple(p1 - side * w)],
                    "palm", [(0, v0), (1, v0), (1, v1), (0, v1)], WHITE)
    return bd.finish()


def build_cypress():
    """A Monterey cypress: a short leaning trunk and a broad, flat-topped,
    windswept crown of dark green clumps."""
    bd = Builder("cypress")
    bd.tube((0, 0, 0), (0.6, 3.2, 0.2), 0.35, 0.25, 6, "flat", rgba(80, 62, 48))
    green, top = rgba(42, 74, 44), rgba(70, 108, 62)
    for c, r, sq in (((0.6, 4.2, 0.2), 3.0, 0.45), ((-1.6, 3.7, -0.6), 2.2, 0.5), ((2.4, 3.9, 0.8), 2.0, 0.5),
                     ((0.4, 5.2, -0.6), 2.0, 0.45)):
        bd.blob(c, r, 8, 3, "flat", green, top, squash=sq)
    return bd.finish()


def build_boat():
    """A wharf fishing boat, 13 m: a white hull with a red line and a
    pointed bow, the wheelhouse, a mast with its boom and the trolling
    poles folded up either side. Bow +z."""
    bd = Builder("boat")
    hull = [(-1.9, -6.0), (1.9, -6.0), (2.0, 1.5), (1.4, 4.6), (0.0, 6.6), (-1.4, 4.6), (-2.0, 1.5)]
    top, wl = 1.6, -0.2
    n = len(hull)
    for i in range(n):
        (x0, z0), (x1, z1) = hull[i], hull[(i + 1) % n]
        bd.poly([(x0 * 0.85, wl - 0.6, z0 * 0.95), (x1 * 0.85, wl - 0.6, z1 * 0.95), (x1, top, z1), (x0, top, z0)],
                "flat", None, [rgba(40, 60, 90), rgba(40, 60, 90), WHITE, WHITE])
        bd.poly([(x0 * 1.01, top - 0.55, z0 * 1.005), (x1 * 1.01, top - 0.55, z1 * 1.005), (x1 * 1.01, top - 0.35, z1 * 1.005),
                 (x0 * 1.01, top - 0.35, z0 * 1.005)], "flat", None, rgba(196, 40, 36))
    bd.poly([(x, top - 0.1, z) for x, z in reversed(hull)], "pier", None, WHITE)
    bd.box((0, top + 1.1, 0.5), (1.3, 1.1, 1.6), 0, "flat", WHITE)
    # The windows stand 5 cm proud of the wheelhouse: flush, they fought it for depth.
    bd.box((0, top + 1.6, 1.05), (1.35, 0.35, 1.1), 0, "flat", GLASS, top=False)
    bd.box((0, top + 2.25, 0.5), (1.45, 0.08, 1.75), 0, "flat", rgba(60, 120, 150))
    bd.tube((0, top, -2.0), (0, top + 7.5, -2.0), 0.08, 0.05, 4, "flat", rgba(220, 220, 216))
    bd.tube((0, top + 1.5, -2.0), (0, top + 1.3, -5.6), 0.06, 0.05, 4, "flat", rgba(220, 220, 216))
    for sx in (-1, 1):
        bd.tube((sx * 1.7, top, 0.0), (sx * 1.0, top + 9.0, -0.5), 0.06, 0.03, 4, "flat", rgba(200, 200, 196))
    return bd.finish()


SL_BACK = rgba(92, 66, 46)      # wet fur along the back
SL_BELLY = rgba(168, 130, 92)    # drier, paler underneath
SL_FLIP = rgba(58, 42, 32)
SL_POSES = {
    # Stations from tail to snout: along, centre height, half width, half height (metres, an adult).
    "lying": [(-1.05, 0.10, 0.05, 0.04), (-0.80, 0.17, 0.17, 0.14), (-0.45, 0.25, 0.30, 0.24), (0.00, 0.30, 0.36, 0.29),
              (0.40, 0.30, 0.33, 0.27), (0.72, 0.27, 0.21, 0.19), (0.95, 0.28, 0.17, 0.15), (1.15, 0.27, 0.10, 0.09),
              (1.26, 0.26, 0.04, 0.04)],
    "up": [(-1.05, 0.10, 0.05, 0.04), (-0.80, 0.17, 0.17, 0.14), (-0.45, 0.26, 0.30, 0.24), (0.00, 0.33, 0.35, 0.29),
           (0.36, 0.46, 0.31, 0.27), (0.58, 0.72, 0.20, 0.18), (0.72, 0.96, 0.17, 0.15), (0.90, 1.08, 0.10, 0.09),
           (1.00, 1.12, 0.04, 0.04)],
}


def sealion(bd, base, yaw, pose, scale, curl=0.0):
    """One sea lion hauled out on a raft at base (its deck height in y),
    facing yaw: a body lofted from tail to snout, dark along the back and
    paler underneath, the front flippers propping it (planted under its
    chest when its head is up), the rear flippers splayed out behind, eyes
    and a nose. curl bends it sideways."""
    f = Vector((math.sin(yaw), 0, math.cos(yaw)))
    r = Vector((math.cos(yaw), 0, -math.sin(yaw)))
    up = Vector((0, 1, 0))
    b = Vector(base)
    def at(x, y, z):
        return tuple(b + (r * (x + curl * (z / 1.2) ** 2) + up * y + f * z) * scale)
    st = SL_POSES[pose]
    sides = 6
    rings = []
    for along, yc, hw, hh in st:
        ring = []
        for k in range(sides):
            a = k / sides * math.tau
            ring.append((at(math.cos(a) * hw, yc + math.sin(a) * hh, along), mix(SL_BELLY, SL_BACK, (math.sin(a) + 1) / 2)))
        rings.append(ring)
    for q in range(len(rings) - 1):
        for k in range(sides):
            j = (k + 1) % sides
            (p0, c0), (p1, c1) = rings[q][k], rings[q][j]
            (p2, c2), (p3, c3) = rings[q + 1][j], rings[q + 1][k]
            bd.poly([p0, p1, p2, p3], "flat", None, [c0, c1, c2, c3], smooth=True)
    # The tail and the snout close to a point as triangles of the body's
    # own rows, so they chain into its strips.
    for q, along in ((0, st[0][0] - 0.06), (len(rings) - 1, st[-1][0] + 0.05)):
        tip = at(0, st[q][1], along)
        for k in range(sides):
            j = (k + 1) % sides
            pts = [rings[q][k][0], rings[q][j][0], tip]
            bd.poly(pts if q else pts[::-1], "flat", None, [rings[q][k][1], rings[q][j][1], SL_BACK], smooth=True)
    # Front flippers, from the chest's sides down to the deck.
    fa, fy, fw, fh = st[4] if pose == "up" else st[3]
    for sx in (-1, 1):
        if pose == "up":
            pts = [at(sx * fw * 0.8, fy, fa + 0.08), at(sx * fw * 0.8, fy - 0.12, fa - 0.08),
                   at(sx * (fw + 0.12), 0.02, fa - 0.05), at(sx * (fw + 0.18), 0.02, fa + 0.22)]
        else:
            pts = [at(sx * fw * 0.85, fy - 0.05, fa + 0.12), at(sx * fw * 0.85, fy - 0.12, fa - 0.08),
                   at(sx * (fw + 0.36), 0.03, fa - 0.18), at(sx * (fw + 0.42), 0.03, fa + 0.16)]
        bd.poly(pts if sx > 0 else pts[::-1], "flat", None, SL_FLIP)
    # Rear flippers splayed behind the tail.
    for sx in (-1, 1):
        pts = [at(sx * 0.03, 0.09, -0.95), at(sx * 0.40, 0.03, -1.42), at(sx * 0.16, 0.03, -1.50)]
        bd.poly(pts if sx > 0 else pts[::-1], "flat", None, SL_FLIP)
    # Eyes on the head's sides and the nose at the tip of the snout.
    ha, hy, hw, hh = st[6]
    for sx in (-1, 1):
        c = Vector((sx * hw * 0.92, hy + hh * 0.3, ha + 0.04))
        pts = [at(c.x, c.y - 0.03, c.z - 0.03), at(c.x, c.y - 0.03, c.z + 0.03), at(c.x, c.y + 0.03, c.z + 0.03),
               at(c.x, c.y + 0.03, c.z - 0.03)]
        bd.poly(pts if sx > 0 else pts[::-1], "flat", None, rgba(16, 12, 10))
    na, ny, nw, nh = st[-1]
    pts = [at(-0.035, ny - 0.03, na + 0.03), at(0.035, ny - 0.03, na + 0.03), at(0.035, ny + 0.03, na + 0.03),
           at(-0.035, ny + 0.03, na + 0.03)]
    bd.poly(pts, "flat", None, rgba(20, 14, 12))


def build_sealions(name, hx, hz, herd, seed):
    """A floating dock of grey planks on dark pontoons, a pale rubber
    fender round its edge and a cleat at each corner, with a herd of sea
    lions hauled out on it: (x, z, facing, pose, size, curl) each."""
    bd = Builder(name)
    bd.box((0, 0.25, 0), (hx, 0.3, hz), 0, "flat", rgba(40, 52, 70), bottom=False, top=False)
    bd.quad((-hx, 0.55, -hz), (hx, 0.55, -hz), (hx, 0.55, hz), (-hx, 0.55, hz), "pier",
            uv=((0, 0), (hx / 6.8, 0), (hx / 6.8, hz / 8.0), (0, hz / 8.0)))
    for sz in (-1, 1):
        bd.box((0, 0.5, sz * (hz + 0.06)), (hx + 0.12, 0.09, 0.07), 0, "flat", rgba(214, 214, 206))
    for sx in (-1, 1):
        bd.box((sx * (hx + 0.06), 0.5, 0), (0.07, 0.09, hz), 0, "flat", rgba(214, 214, 206))
        for sz in (-1, 1):
            bd.box((sx * (hx - 0.25), 0.62, sz * (hz - 0.25)), (0.12, 0.07, 0.05), 0, "flat", rgba(30, 30, 32))
    for x, z, yaw, pose, size, curl in herd:
        sealion(bd, (x, 0.55, z), yaw, pose, size, curl)
    return bd.finish()


SL_HERD_A = [
    (-2.0, -1.3, 0.3, "lying", 1.0, 0.0), (-0.4, -1.4, -0.2, "up", 1.05, 0.0), (1.6, -1.2, 2.9, "lying", 0.95, 0.2),
    (-2.2, 0.9, 1.8, "lying", 1.0, -0.25), (-0.3, 0.6, 3.6, "lying", 1.1, 0.0), (1.4, 1.0, 0.8, "up", 0.95, 0.0),
    (2.6, 0.2, 4.6, "lying", 0.6, 0.15),
]
SL_HERD_B = [
    (-1.1, -0.6, 1.2, "lying", 1.0, 0.2), (0.9, 0.7, 4.0, "up", 1.0, 0.0), (1.2, -0.9, 2.4, "lying", 0.65, 0.0),
    (-1.5, 1.0, 5.6, "lying", 0.9, -0.2),
]


def build_sign_arch():
    """The wharf's sign over the road: two lattice-look iron posts outside
    the railings, a crossbeam and the 16 m board between them, the sign
    on both faces. The road runs along z."""
    bd = Builder("sign_arch")
    X, top = 10.35, 9.6
    for sx in (-1, 1):
        bd.box((sx * X, top / 2, 0), (0.2, top / 2, 0.25), 0, "flat", rgba(54, 70, 96))
        bd.box((sx * X, 0.4, 0), (0.25, 0.4, 0.35), 0, "flat", CONCRETE)
    bd.box((0, 6.0, 0), (X, 0.18, 0.2), 0, "flat", rgba(54, 70, 96), bottom=True)
    bd.box((0, top - 0.05, 0), (X, 0.12, 0.3), 0, "flat", rgba(54, 70, 96), bottom=True)
    for sz, uv in ((-1, ((0, 1), (1, 1), (1, 0), (0, 0))), (1, ((1, 1), (0, 1), (0, 0), (1, 0)))):
        z = sz * 0.12
        a, b_ = (-8.0, 6.1, z), (8.0, 6.1, z)
        bd.quad(a, b_, (8.0, 9.5, z), (-8.0, 9.5, z), "sign", uv=uv)
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
        ("sfp_bridge", build_bridge(), False),
        ("sfp_alcatraz", build_alcatraz(), False),
        ("sfp_coit", build_coit(), True),
        ("sfp_pyramid", build_pyramid(), True),
        ("sfp_cablecar", build_cablecar(), True),
        ("sfp_lamp", build_lamp(), True),
        ("sfp_palm", build_palm(), True),
        ("sfp_cypress", build_cypress(), True),
        ("sfp_boat", build_boat(), True),
        ("sfp_ferry", build_ferry(), True),
        ("sfp_sealions", build_sealions("sealions", 3.4, 2.4, SL_HERD_A, 53), True),
        ("sfp_sealions_b", build_sealions("sealions_b", 2.5, 1.8, SL_HERD_B, 59), True),
        ("sfp_sign_arch", build_sign_arch(), True),
    ]
    blender_props.bake(props, MATERIALS, PT_MATERIALS, a.header, a.blend, a.preview,
                       script="tools/build_sf_props.py", guard="DK_SF_PROPS_H",
                       label="bay city prop",
                       unit_note="the bridge in units of 30 m, Alcatraz of 10 m: millimetres of those",
                       mat_prefix="SFM", prop_prefix="SFP", table="sf_props")


main()
