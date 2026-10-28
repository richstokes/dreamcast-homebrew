#!/usr/bin/env python3
"""Model Tsudoken Drift's props in Blender and bake them for the game.

Run through Blender, not the host Python interpreter (``make tsudoken-props``
does it):

    blender --background --python tools/build_tsudoken_props.py -- \
        --header assets/generated/tsudoken_props.h \
        --blend captures/tsudoken_props.blend --preview captures/tsudoken-props

Every prop is built procedurally with bmesh (Mt Fuji, the cherry trees, torii
gates, stone lanterns, a pagoda, the grandstand modules, the race control
tower, the pit garages, a teahouse and an arched bridge), given a material
from the fixed list below (the game maps each to a texture) and vertex
colours, then cut into PowerVR strips per material and written as a header,
with a few solid boxes per prop for the spectator's cameras to keep clear of.
The game instances each prop where it likes, yawed and scaled, and lights the
vertices by their normals as it builds the world.

Coordinates: the game is y-up; Blender gets (x, -z, y) so the scene reads the
right way round in the viewport, and the export maps it back. Positions are
exported in centimetres (Mt Fuji in metres of a unit mountain the game scales
up), normals * 127, UVs in 1/256 and colours as bytes.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
import blender_props  # noqa: E402
from blender_props import WHITE, mix, rgba, scale_col  # noqa: E402

# Material order is the game's TKM_* enum (src/world_tsudoken.c maps them to headers).
MATERIALS = ["flat", "kawara", "crowd", "ishigaki", "sakura", "bark", "plaster", "banner", "pitwall"]
# Punch-through (alpha tested) materials go in their own list in the game.
PT_MATERIALS = {"sakura"}

VERMILION = (0.91, 0.28, 0.16, 1.0)
BLACK_LACQUER = (0.12, 0.10, 0.10, 1.0)
STONE = (0.74, 0.73, 0.70, 1.0)
DARK_WOOD = (0.30, 0.18, 0.12, 1.0)
PLANK = (0.72, 0.46, 0.26, 1.0)
CONCRETE = (0.70, 0.70, 0.72, 1.0)
ROOF_UNDER = (0.26, 0.20, 0.18, 1.0)
WINDOW = (0.22, 0.30, 0.42, 1.0)
LANTERN_RED = (0.86, 0.16, 0.12, 1.0)
GLOW = (1.0, 0.86, 0.50, 1.0)


class Builder(blender_props.Builder):
    MATERIALS = MATERIALS


# ---------------------------------------------------------------------------
# Roofs

def hip_roof(bd, c, hw, hd, y_eave, rise, ridge_frac, lift, mat="kawara", tile=1.6, under=ROOF_UNDER, segs=4):
    """A hipped roof over the rectangle (hw, hd) about c at eave height
    y_eave, rising to a ridge rectangle ridge_frac the size, the eaves
    swept up by lift at the corners. Faces are split along the eave so the
    sweep curves. The underside is a dark sheet just below."""
    cx, cz = c[0], c[2]
    def eave(t, side):
        # side 0 front (-z), 1 right (+x), 2 back (+z), 3 left (-x); t 0..1 along it
        if side == 0:
            p = (cx - hw + 2 * hw * t, 0, cz - hd)
        elif side == 1:
            p = (cx + hw, 0, cz - hd + 2 * hd * t)
        elif side == 2:
            p = (cx + hw - 2 * hw * t, 0, cz + hd)
        else:
            p = (cx - hw, 0, cz + hd - 2 * hd * t)
        y = y_eave + lift * (2 * abs(t - 0.5)) ** 2
        return (p[0], y, p[2])
    rw, rd = hw * ridge_frac, hd * ridge_frac
    def ridge(t, side):
        if side == 0:
            p = (cx - rw + 2 * rw * t, 0, cz - rd)
        elif side == 1:
            p = (cx + rw, 0, cz - rd + 2 * rd * t)
        elif side == 2:
            p = (cx + rw - 2 * rw * t, 0, cz + rd)
        else:
            p = (cx - rw, 0, cz + rd - 2 * rd * t)
        return (p[0], y_eave + rise, p[2])
    for side in range(4):
        length = 2 * hw if side % 2 == 0 else 2 * hd
        slope = math.hypot(hw - rw if side % 2 == 0 else hd - rd, rise)
        for k in range(segs):
            t0, t1 = k / segs, (k + 1) / segs
            e0, e1, r1, r0 = eave(t0, side), eave(t1, side), ridge(t1, side), ridge(t0, side)
            uv = [(t0 * length / tile, slope / tile), (t1 * length / tile, slope / tile), (t1 * length / tile, 0), (t0 * length / tile, 0)]
            bd.poly([e0, e1, r1, r0], mat, uv, WHITE)
            if under:
                d = 0.18
                bd.poly([(e0[0], e0[1] - d, e0[2]), (e1[0], e1[1] - d, e1[2]), (r1[0], r1[1] - d, r1[2]),
                         (r0[0], r0[1] - d, r0[2])], "flat", None, under)
    # The ridge cap.
    if ridge_frac > 0.01:
        y = y_eave + rise
        bd.quad((cx - rw, y + 0.12, cz - rd), (cx + rw, y + 0.12, cz - rd), (cx + rw, y + 0.12, cz + rd),
                (cx - rw, y + 0.12, cz + rd), "flat", col=BLACK_LACQUER)
        for sx in (-1, 1):
            bd.quad((cx + sx * rw, y, cz - rd), (cx + sx * rw, y + 0.12, cz - rd), (cx + sx * rw, y + 0.12, cz + rd),
                    (cx + sx * rw, y, cz + rd), "flat", col=BLACK_LACQUER)
        for sz in (-1, 1):
            bd.quad((cx - rw, y, cz + sz * rd), (cx - rw, y + 0.12, cz + sz * rd), (cx + rw, y + 0.12, cz + sz * rd),
                    (cx + rw, y, cz + sz * rd), "flat", col=BLACK_LACQUER)


# ---------------------------------------------------------------------------
# Props

def build_fuji(name="fuji", coarse=False):
    """A unit mountain: height 1, base radius 2.1, Fuji's profile (a cone
    whose slopes steepen gently toward a broad flat summit, not a funnel),
    a soft snow line and a crater. Hazy mauve slopes under pink snow, the
    sun's side a shade warmer; the game scales it up and draws it far
    away, without fog. The coarse one is for a split screen, which draws
    everything twice."""
    bd = Builder(name)
    # Rings crowd round the snow line so its gradient is soft; a coarse mesh
    # with a ragged, noisy snow line reads as a star of jagged triangles.
    segs = 28 if coarse else 56
    hs = ([0.0, 0.3, 0.5, 0.58, 0.64, 0.72, 0.85, 1.0] if coarse else
          [0.0, 0.16, 0.30, 0.42, 0.50, 0.55, 0.59, 0.63, 0.67, 0.72, 0.78, 0.85, 0.92, 1.0])
    R, P = 2.1, 1.3
    def radius(h, a):
        return max(R * (1 - h) ** P + 0.22 * h, 0.26)
    def snow_line(a):
        # Broad, gentle: the snow reaches a little further down a few faces.
        return 0.60 + 0.025 * math.sin(a * 2 + 1.0) + 0.015 * math.sin(a * 3 + 0.3)
    foot = rgba(156, 126, 170)
    slope = rgba(104, 84, 144)
    snow = rgba(255, 240, 236)
    snow_shade = rgba(222, 196, 218)
    sun_a = math.radians(240 - 212)   # the sun's bearing relative to the mountain's, from the circuit
    rings = []
    for h in hs:
        ring = []
        for i in range(segs):
            a = i / segs * math.tau
            r = radius(h, a)
            sl = snow_line(a)
            if h < sl - 0.06:
                col = mix(foot, slope, min(1.0, h / 0.56))
            elif h < sl + 0.04:
                t = (h - (sl - 0.06)) / 0.10
                col = mix(slope, snow_shade, t * t * (3 - 2 * t))
            else:
                col = mix(snow_shade, snow, min(1.0, (h - sl - 0.04) / 0.2))
            # The face toward the sun (the circuit sees the mountain from the
            # north-east, the sun is west of it) warms a touch.
            lit = 0.5 + 0.5 * math.cos(a - (math.pi + sun_a))
            col = mix(col, rgba(255, 214, 170), 0.18 * lit)
            ring.append(((math.sin(a) * r, h, math.cos(a) * r), col))
        rings.append(ring)
    for k in range(len(hs) - 1):
        for i in range(segs):
            j = (i + 1) % segs
            (p0, c0), (p1, c1) = rings[k][i], rings[k][j]
            (p2, c2), (p3, c3) = rings[k + 1][j], rings[k + 1][i]
            bd.poly([p0, p1, p2, p3], "flat", None, [c0, c1, c2, c3], smooth=True)
    # The summit: a shallow crater, its rim the top ring.
    bd.poly([p for p, _ in reversed(rings[-1])], "flat", None, snow_shade)
    return bd.finish()


def build_sakura(name, seed):
    """A cherry tree: a leaning trunk and three boughs in bark, the blossom
    as seven cut-out planes (vertical crosses, two tilted and one flat) in
    the alpha-tested blossom texture. About 4.5 m to the top of the trunk;
    the game scales each tree."""
    import random
    rng = random.Random(seed)
    bd = Builder(name)
    lean = Vector((rng.uniform(-0.25, 0.25), 0, rng.uniform(-0.25, 0.25)))
    base, top = Vector((0, -0.2, 0)), Vector((0, 2.6, 0)) + lean
    bark = WHITE
    bd.tube(base, top, 0.42, 0.26, 5, "bark", bark, bark, 1.0, 2.2, phase=0.3)
    boughs = []
    for k in range(2):
        a = k / 2 * math.tau + rng.uniform(-0.5, 0.5)
        out = Vector((math.sin(a), 0, math.cos(a)))
        end = top + out * rng.uniform(1.4, 2.0) + Vector((0, rng.uniform(1.1, 1.6), 0))
        bd.tube(top - Vector((0, 0.3, 0)), end, 0.17, 0.07, 3, "bark", bark, scale_col(bark, 0.9), 0.5, 1.0, phase=a)
        boughs.append(end)
    canopy_c = top + Vector((0, 1.65, 0)) + lean * 0.5
    r = rng.uniform(2.9, 3.4)
    def plane(centre, axis_u, axis_v, hu, hv, col_lo, col_hi):
        u, v = Vector(axis_u).normalized(), Vector(axis_v).normalized()
        pts = [centre - u * hu - v * hv, centre + u * hu - v * hv, centre + u * hu + v * hv, centre - u * hu + v * hv]
        bd.poly([tuple(p) for p in pts], "sakura", [(0, 1), (1, 1), (1, 0), (0, 0)], [col_lo, col_lo, col_hi, col_hi])
    lo, hi = rgba(214, 190, 200), WHITE
    for k in range(3):
        a = k / 3 * math.pi + rng.uniform(-0.15, 0.15)
        plane(canopy_c, (math.cos(a), 0, -math.sin(a)), (0, 1, 0), r, r * 0.72, lo, hi)
    for k in range(1):
        a = rng.uniform(0, math.tau)
        tilt = Vector((math.sin(a) * 0.6, 0.8, math.cos(a) * 0.6)).normalized()
        side = tilt.cross(Vector((0, 1, 0))).normalized()
        plane(canopy_c + Vector((0, 0.3 * k, 0)), side, side.cross(tilt), r * 0.95, r * 0.8, lo, hi)
    plane(canopy_c + Vector((0, 0.5, 0)), (1, 0, 0), (0, 0, 1), r * 1.05, r * 1.05, hi, hi)
    plane(canopy_c - Vector((0, 0.9, 0)), (1, 0, 0), (0, 0, 1), r * 0.8, r * 0.8, lo, lo)
    # Solid: the crown's dense middle, over the bare trunk.
    s0, s1 = (list(v) for v in bd.bounds())
    for a in (0, 2):
        c, h = (s0[a] + s1[a]) * 0.5, (s1[a] - s0[a]) * 0.3
        s0[a], s1[a] = c - h, c + h
    s0[1] += (s1[1] - s0[1]) * 0.35
    bd.solid(s0, s1)
    return bd.finish()


def build_torii():
    """A torii: two pillars leaning in a touch on black footings, the tie
    beam through them with the plaque, the island beam and the upswept
    black-topped lintel. Pillar height 6, span 7 between the pillars."""
    bd = Builder("torii")
    H, S = 6.0, 7.0
    for sx in (-1, 1):
        x0, x1 = sx * S / 2, sx * (S / 2 - 0.18)
        bd.tube((x0, 0.45, 0), (x1, H, 0), 0.34, 0.28, 8, "flat", VERMILION, VERMILION, smooth=True)
        bd.tube((x0, -0.3, 0), (x0, 0.45, 0), 0.44, 0.44, 8, "flat", BLACK_LACQUER, BLACK_LACQUER, smooth=True)
        bd.disc((x0, 0.45, 0), 0.44, 8, "flat", BLACK_LACQUER)
    # Nuki: the tie beam, square, through both pillars.
    bd.box((0, 4.35, 0), (S / 2 + 0.95, 0.22, 0.17), 0, "flat", VERMILION, bottom=True)
    for sx in (-1, 1):
        bd.box((sx * (S / 2 + 0.6), 4.35, 0), (0.08, 0.3, 0.26), 0, "flat", BLACK_LACQUER, bottom=True)   # wedges
    # Gakuzuka: the plaque between the beams.
    bd.box((0, 5.15, 0), (0.42, 0.58, 0.12), 0, "flat", BLACK_LACQUER, bottom=True,
           cols_by_face={"front": rgba(240, 200, 90), "back": rgba(240, 200, 90)})
    # Shimaki: the island beam.
    bd.box((0, 5.95, 0), (S / 2 + 1.15, 0.2, 0.3), 0, "flat", VERMILION, bottom=True)
    # Kasagi: the lintel, swept up toward its ends, black on top.
    xe, th, dp = S / 2 + 1.75, 0.42, 0.44
    stations = [-xe + 2 * xe * k / 5 for k in range(6)]
    def lift(x):
        return 0.55 * (abs(x) / xe) ** 2.6
    prev = None
    for x in stations:
        y0, y1 = 6.15 + lift(x) * 0.6, 6.15 + th + lift(x)
        cur = ((x, y0, -dp), (x, y0, dp), (x, y1, dp), (x, y1, -dp))
        if prev:
            bd.poly([prev[0], cur[0], cur[1], prev[1]], "flat", None, VERMILION)            # underside
            bd.poly([prev[1], cur[1], cur[2], prev[2]], "flat", None, VERMILION)            # +z face
            bd.poly([prev[3], cur[3], cur[0], prev[0]], "flat", None, VERMILION)            # -z face
            bd.poly([prev[2], cur[2], cur[3], prev[3]], "flat", None, BLACK_LACQUER)        # top
        prev = cur
    for x in (stations[0], stations[-1]):
        y0, y1 = 6.15 + lift(x) * 0.6, 6.15 + th + lift(x)
        bd.poly([(x, y0, -dp), (x, y0, dp), (x, y1, dp), (x, y1, -dp)], "flat", None, BLACK_LACQUER)
    # Solid: each pillar on its footing up to the tie beam, and the beams
    # across; a camera can look through between them.
    for sx in (-1, 1):
        bd.solid((sx * S / 2 - 0.44, -0.3, -0.44), (sx * S / 2 + 0.44, 4.35 - 0.22, 0.44))
    bd.solid((-xe, 4.35 - 0.3, -dp), (xe, 6.15 + th + lift(xe), dp))
    return bd.finish()


def build_lantern():
    """A stone lantern (ishidoro) about 2.7 m tall: slab, post, platform,
    the fire box with two lit windows, a swept hexagonal roof and a jewel.
    Kept to a few dozen faces: there are twenty round the circuit."""
    bd = Builder("lantern")
    stone, lit = STONE, GLOW
    def hex_prism(y0, y1, r0, r1, col=stone, cap=True):
        bd.tube((0, y0, 0), (0, y1, 0), r0, r1, 6, "flat", col, col, smooth=True, phase=math.pi / 6)
        if cap:
            bd.disc((0, y1, 0), r1, 6, "flat", col, phase=math.pi / 6)
    hex_prism(-0.3, 0.3, 0.6, 0.5)
    hex_prism(0.3, 1.45, 0.2, 0.17, cap=False)
    hex_prism(1.45, 1.68, 0.46, 0.52)
    # Fire box: six faces, two of them windows lit from inside.
    y0, y1, r = 1.68, 2.16, 0.36
    for i in range(6):
        a0, a1 = i / 6 * math.tau + math.pi / 6, (i + 1) / 6 * math.tau + math.pi / 6
        p0, p1 = (math.sin(a0) * r, 0, math.cos(a0) * r), (math.sin(a1) * r, 0, math.cos(a1) * r)
        col = lit if i in (0, 3) else stone
        bd.poly([(p0[0], y0, p0[2]), (p1[0], y0, p1[2]), (p1[0], y1, p1[2]), (p0[0], y1, p0[2])], "flat", None, col)
    # Roof: a hexagonal cap over the box, its rim turned up, dark beneath.
    rim, top = 0.7, 2.62
    for i in range(6):
        a0, a1 = i / 6 * math.tau + math.pi / 6, (i + 1) / 6 * math.tau + math.pi / 6
        e0, e1 = (math.sin(a0) * rim, y1 + 0.16, math.cos(a0) * rim), (math.sin(a1) * rim, y1 + 0.16, math.cos(a1) * rim)
        m = ((math.sin(a0) + math.sin(a1)) * 0.5 * rim * 0.95, y1 + 0.04, (math.cos(a0) + math.cos(a1)) * 0.5 * rim * 0.95)
        bd.poly([e0, m, (0, top, 0)], "flat", None, stone, smooth=True)
        bd.poly([m, e1, (0, top, 0)], "flat", None, stone, smooth=True)
        bd.poly([e0, (0, y1 + 0.02, 0), m], "flat", None, scale_col(stone, 0.7), smooth=True)
        bd.poly([m, (0, y1 + 0.02, 0), e1], "flat", None, scale_col(stone, 0.7), smooth=True)
    # Jewel: a small pyramid.
    j, jr = top + 0.02, 0.13
    for i in range(4):
        a0, a1 = i / 4 * math.tau, (i + 1) / 4 * math.tau
        bd.poly([(math.sin(a0) * jr, j, math.cos(a0) * jr), (math.sin(a1) * jr, j, math.cos(a1) * jr), (0, j + 0.3, 0)], "flat", None, stone, smooth=True)
    return bd.finish()


def build_pagoda():
    """A three-tier pagoda on a stone plinth: plaster bodies with dark
    corner posts and railed balconies, tiled hip roofs swept up at the
    corners, and a ringed spire. About 19 m tall."""
    bd = Builder("pagoda")
    bd.box((0, -0.45, 0), (4.4, 0.45, 4.4), 0, "ishigaki", WHITE, uv_scale=0.35)
    widths = [3.0, 2.5, 2.05]
    y = 0.0
    for k, w in enumerate(widths):
        body_h = 2.7 if k == 0 else 2.3
        bd.box((0, y + body_h / 2, 0), (w, body_h / 2, w), 0, "plaster", WHITE, uv_scale=0.5)
        for sx in (-1, 1):
            for sz in (-1, 1):
                bd.box((sx * w, y + body_h / 2, sz * w), (0.16, body_h / 2, 0.16), 0, "flat", DARK_WOOD)
        # A door on the ground floor, windows above.
        if k == 0:
            bd.quad((-0.7, y + 0.02, -w - 0.02), (0.7, y + 0.02, -w - 0.02), (0.7, y + 1.9, -w - 0.02), (-0.7, y + 1.9, -w - 0.02),
                    "flat", col=DARK_WOOD)
        else:
            for side in range(4):
                a = side * math.pi / 2
                n = Vector((math.sin(a), 0, math.cos(a)))
                t = Vector((math.cos(a), 0, -math.sin(a)))
                c = n * (w + 0.02) + Vector((0, y + body_h * 0.55, 0))
                pts = [c - t * 0.55 - Vector((0, 0.5, 0)), c + t * 0.55 - Vector((0, 0.5, 0)), c + t * 0.55 + Vector((0, 0.5, 0)),
                       c - t * 0.55 + Vector((0, 0.5, 0))]
                bd.poly([tuple(p) for p in pts], "flat", None, WINDOW)
        # Balcony rail round each upper tier.
        if k > 0:
            rw = w + 0.7
            bd.box((0, y + 0.08, 0), (rw, 0.08, rw), 0, "flat", DARK_WOOD, bottom=True)
            bd.box((0, y + 0.95, 0), (rw, 0.05, rw), 0, "flat", VERMILION, bottom=True)
        eave = w + 1.9
        hip_roof(bd, (0, 0, 0), eave, eave, y + body_h, 1.45, 0.42 if k < 2 else 0.0, 0.4, segs=3)
        y += body_h + 1.45 + 0.35
    # The spire: a pole with five rings and a flame finial.
    bd.tube((0, y - 0.6, 0), (0, y + 3.6, 0), 0.14, 0.1, 6, "flat", rgba(190, 160, 70), rgba(220, 190, 90))
    for i in range(3):
        ry = y + 0.5 + i * 0.8
        bd.tube((0, ry, 0), (0, ry + 0.14, 0), 0.62 - i * 0.1, 0.62 - i * 0.1, 6, "flat", rgba(210, 180, 80), rgba(210, 180, 80), smooth=True)
        bd.disc((0, ry + 0.14, 0), 0.62 - i * 0.1, 6, "flat", rgba(230, 200, 100))
    for i in range(4):
        a0, a1 = i / 4 * math.tau, (i + 1) / 4 * math.tau
        bd.poly([(math.sin(a0) * 0.3, y + 3.5, math.cos(a0) * 0.3), (math.sin(a1) * 0.3, y + 3.5, math.cos(a1) * 0.3), (0, y + 4.3, 0)],
                "flat", None, rgba(240, 210, 110))
    return bd.finish()


def build_stand():
    """One 24 m grandstand module: eight tiers of crowd on concrete steps
    behind a stone front wall, plaster side and back walls with the course
    banner across the back, and a tiled roof swept up at its front eave on
    four pillars, with a row of red paper lanterns hanging under it. The
    front row is at z = 0 facing -z (the game turns it to the road)."""
    bd = Builder("stand")
    W, tiers, tread, rise = 12.0, 8, 2.2, 1.0
    y0 = 1.0
    bd.box((0, y0 / 2, -0.35), (W, y0 / 2, 0.35), 0, "ishigaki", WHITE, uv_scale=0.5)
    for i in range(tiers):
        z0, z1, y = i * tread, (i + 1) * tread, y0 + i * rise
        # Tread, the riser up to the next, and the crowd leaning back on it.
        bd.quad((-W, y, z0), (W, y, z0), (W, y, z1), (-W, y, z1), "flat", col=CONCRETE)
        bd.quad((-W, y, z1), (W, y, z1), (W, y + rise, z1), (-W, y + rise, z1), "flat", col=scale_col(CONCRETE, 0.86))
        rep = 2 * W / 3.0
        bd.quad((-W, y + 0.02, z0 + 0.75), (W, y + 0.02, z0 + 0.75), (W, y + 1.3, z0 + 1.7), (-W, y + 1.3, z0 + 1.7),
                "crowd", uv=((0, 1), (rep, 1), (rep, 0), (0, 0)))
    back_z, top_y = tiers * tread, y0 + tiers * rise
    # Side walls: the stepped profile closed with one quad each.
    for sx in (-1, 1):
        x = sx * W
        bd.poly([(x, 0, 0), (x, y0, 0), (x, top_y, back_z), (x, 0, back_z)], "plaster",
                [(0, 0), (0, y0 / 2), (back_z / 2, top_y / 2), (back_z / 2, 0)])
    # Back wall with the banner across it above the top row.
    roof_back_y = top_y + 3.4
    bd.quad((-W, 0, back_z), (W, 0, back_z), (W, roof_back_y, back_z), (-W, roof_back_y, back_z), "plaster",
            uv=((0, 0), (W, 0), (W, roof_back_y / 2), (0, roof_back_y / 2)))
    bz = back_z - 0.05
    bd.quad((-W + 0.6, top_y + 0.4, bz), (W - 0.6, top_y + 0.4, bz), (W - 0.6, top_y + 2.6, bz), (-W + 0.6, top_y + 2.6, bz),
            "banner", uv=((0, 0.5), (1, 0.5), (1, 0), (0, 0)))
    # Pillars at the back holding the roof.
    for x in (-9.0, -3.0, 3.0, 9.0):
        bd.box((x, (top_y + roof_back_y + 0.4) / 2, back_z - 0.8), (0.28, (roof_back_y + 0.4 - top_y) / 2, 0.28), 0, "flat", VERMILION)
    # The roof: a sheet from behind the back wall out over the front row,
    # tiles on top, dark underneath, curling up at the front.
    zs = [back_z + 1.2, back_z - 6.0, back_z - 12.0, back_z - 16.0, back_z - 18.6]
    def roof_y(z):
        t = (back_z + 1.2 - z) / (back_z + 1.2 - zs[-1])
        return roof_back_y + 0.5 - 1.9 * t + 1.2 * max(0.0, t - 0.55) ** 2 / 0.2
    ox = W + 0.9
    for a, c in zip(zs, zs[1:]):
        ya, yc = roof_y(a), roof_y(c)
        bd.quad((-ox, ya, a), (ox, ya, a), (ox, yc, c), (-ox, yc, c), "kawara",
                uv=((0, a / 1.6), (2 * ox / 1.6, a / 1.6), (2 * ox / 1.6, c / 1.6), (0, c / 1.6)))
        bd.quad((-ox, ya - 0.3, a), (ox, ya - 0.3, a), (ox, yc - 0.3, c), (-ox, yc - 0.3, c), "flat", col=ROOF_UNDER)
        for sx in (-1, 1):
            x = sx * ox
            bd.quad((x, ya - 0.3, a), (x, ya, a), (x, yc, c), (x, yc - 0.3, c), "flat", col=BLACK_LACQUER)
    zf = zs[-1]
    yf = roof_y(zf)
    bd.quad((-ox, yf - 0.3, zf), (ox, yf - 0.3, zf), (ox, yf, zf), (-ox, yf, zf), "flat", col=BLACK_LACQUER)
    # Solid: the tiers stepping up back from the road, to the top of the
    # crowd on each, and the roof, so a camera can stand over the front rows.
    front, back, top = -1.0, back_z + 1.2, roof_back_y + 0.5
    def crowd_top(z):
        return y0 + int(z // tread) * rise + 1.3
    steps = (front, 6.0, 12.0, back)
    for a, c in zip(steps, steps[1:]):
        bd.solid((-ox, 0, a), (ox, crowd_top(c) if c < back else top, c))
    bd.solid((-ox, min(roof_y(z) for z in zs) - 0.3, front), (ox, top, back))
    return bd.finish()


def build_chochin():
    """The row of red paper lanterns under a grandstand module's front
    eave, a prop of its own so a split screen can leave them out. Placed
    at the module's origin."""
    bd = Builder("chochin")
    W, tiers, tread, rise = 12.0, 8, 2.2, 1.0
    back_z, top_y = tiers * tread, 1.0 + tiers * rise
    roof_back_y = top_y + 3.4
    zf = back_z - 18.6
    t = 1.0
    yf = roof_back_y + 0.5 - 1.9 * t + 1.2 * max(0.0, t - 0.55) ** 2 / 0.2
    for k in range(5):
        x = -W + 2.4 + k * (2 * W - 4.8) / 4
        lz, ly = zf + 1.2, yf - 1.25
        bd.tube((x, ly + 0.6, lz), (x, ly + 0.3, lz), 0.001, 0.32, 5, "flat", BLACK_LACQUER, LANTERN_RED)
        bd.tube((x, ly + 0.3, lz), (x, ly - 0.3, lz), 0.32, 0.32, 5, "flat", LANTERN_RED, LANTERN_RED)
        bd.tube((x, ly - 0.3, lz), (x, ly - 0.6, lz), 0.32, 0.001, 5, "flat", LANTERN_RED, BLACK_LACQUER)
    bd.solids = []   # inside the stand's roof box
    return bd.finish()


def build_tower():
    """Race control by the start line: three storeys of plaster with a
    window band each, on a stone plinth, under a swept tiled roof."""
    bd = Builder("tower")
    hw = 2.6   # a slim tower, for the same narrow infield as the pits
    bd.box((0, -0.4, 0), (hw + 0.6, 0.4, hw + 0.6), 0, "ishigaki", WHITE, uv_scale=0.35)
    for k in range(3):
        y = k * 3.4
        bd.box((0, y + 1.7, 0), (hw, 1.7, hw), 0, "plaster", WHITE, uv_scale=0.5)
        for side in range(4):
            a = side * math.pi / 2
            n = Vector((math.sin(a), 0, math.cos(a)))
            t = Vector((math.cos(a), 0, -math.sin(a)))
            c = n * (hw + 0.02) + Vector((0, y + 2.1, 0))
            pts = [c - t * (hw - 0.5) - Vector((0, 0.65, 0)), c + t * (hw - 0.5) - Vector((0, 0.65, 0)),
                   c + t * (hw - 0.5) + Vector((0, 0.65, 0)), c - t * (hw - 0.5) + Vector((0, 0.65, 0))]
            bd.poly([tuple(p) for p in pts], "flat", None, WINDOW if k < 2 else scale_col(WINDOW, 1.5))
        bd.box((0, y + 3.4 + 0.1, 0), (hw + 0.4, 0.1, hw + 0.4), 0, "flat", DARK_WOOD, bottom=True)
    # A balcony with a rail on the top floor's front.
    bd.box((0, 6.9, -hw - 0.9), (hw + 0.3, 0.1, 0.9), 0, "flat", DARK_WOOD, bottom=True)
    bd.box((0, 7.6, -hw - 1.75), (hw + 0.3, 0.04, 0.04), 0, "flat", VERMILION, bottom=True)
    for x in (-hw - 0.2, 0, hw + 0.2):
        bd.box((x, 7.3, -hw - 1.75), (0.05, 0.35, 0.05), 0, "flat", VERMILION)
    hip_roof(bd, (0, 0, 0), hw + 1.4, hw + 1.4, 10.2, 2.0, 0.3, 0.45, segs=3)
    return bd.finish()


def build_pits():
    """A 24 m run of pit garages: roller doors along the front (the pit
    wall texture), plaster elsewhere, a tiled awning along the front and a
    low parapet round the roof. The front is at z = 0 facing -z."""
    bd = Builder("pits")
    W, D, H = 12.0, 1.7, 4.4   # shallow: the infield between the start straight and turn 1's exit is only ~9 m wide
    bd.box((0, H / 2, D), (W, H / 2, D), 0, "plaster", WHITE, uv_scale=0.5,
           top=False)
    bd.quad((-W, 0, -0.02), (W, 0, -0.02), (W, H, -0.02), (-W, H, -0.02), "pitwall", uv=((0, 1), (1, 1), (1, 0), (0, 0)))
    bd.quad((-W, H, 0), (W, H, 0), (W, H, 2 * D), (-W, H, 2 * D), "flat", col=rgba(96, 98, 104))
    bd.box((0, H + 0.3, D), (W, 0.3, D), 0, "flat", rgba(120, 122, 128), top=False, bottom=False)
    bd.quad((-W, H + 0.6, 0), (W, H + 0.6, 0), (W, H + 0.6, 2 * D), (-W, H + 0.6, 2 * D), "flat", col=rgba(130, 132, 138))
    # The awning.
    for (za, ya), (zc, yc) in (((0.0, 4.1), (-1.4, 3.75)), ((-1.4, 3.75), (-2.6, 3.6))):
        bd.quad((-W - 0.3, ya, za), (W + 0.3, ya, za), (W + 0.3, yc, zc), (-W - 0.3, yc, zc), "kawara",
                uv=((0, 0), ((2 * W + 0.6) / 1.6, 0), ((2 * W + 0.6) / 1.6, 1), (0, 1)))
        bd.quad((-W - 0.3, ya - 0.2, za), (W + 0.3, ya - 0.2, za), (W + 0.3, yc - 0.2, zc), (-W - 0.3, yc - 0.2, zc), "flat", col=ROOF_UNDER)
    for x in (-W + 0.4, -W / 3, W / 3, W - 0.4):
        bd.box((x, 1.75, -2.4), (0.12, 1.75, 0.12), 0, "flat", VERMILION)
    return bd.finish()


def build_teahouse():
    """A small teahouse: plaster walls on a stone footing, a wooden
    veranda along the front, a dark sliding door and a deep tiled hip roof."""
    bd = Builder("teahouse")
    hw, hd, H = 4.4, 3.4, 3.0
    bd.box((0, -0.3, 0), (hw + 0.3, 0.3, hd + 0.3), 0, "ishigaki", WHITE, uv_scale=0.4)
    bd.box((0, H / 2, 0), (hw, H / 2, hd), 0, "plaster", WHITE, uv_scale=0.5, top=False)
    bd.quad((-1.1, 0.0, -hd - 0.03), (1.1, 0.0, -hd - 0.03), (1.1, 2.2, -hd - 0.03), (-1.1, 2.2, -hd - 0.03), "flat", col=DARK_WOOD)
    bd.quad((-1.1 + 0.08, 0.3, -hd - 0.04), (-0.08, 0.3, -hd - 0.04), (-0.08, 2.1, -hd - 0.04), (-1.1 + 0.08, 2.1, -hd - 0.04), "flat",
            col=rgba(236, 226, 200))
    for x in (-3.4, 2.6):
        bd.quad((x, 1.0, -hd - 0.03), (x + 1.3, 1.0, -hd - 0.03), (x + 1.3, 2.0, -hd - 0.03), (x, 2.0, -hd - 0.03), "flat", col=rgba(236, 226, 200))
    # Veranda and its posts.
    bd.box((0, 0.3, -hd - 0.9), (hw + 0.6, 0.08, 0.9), 0, "flat", PLANK, bottom=True)
    for x in (-hw - 0.4, -hw / 2, hw / 2, hw + 0.4):
        bd.box((x, 1.7, -hd - 1.6), (0.1, 1.5, 0.1), 0, "flat", DARK_WOOD)
    hip_roof(bd, (0, 0, -0.5), hw + 1.8, hd + 2.0, H + 0.2, 1.7, 0.35, 0.4, segs=4)
    return bd.finish()


def build_bridge():
    """A red arched footbridge (taiko-bashi): 14 m span, 3 m wide, planked
    deck on an arc with red rails on posts. Along x, centred at the origin."""
    bd = Builder("bridge")
    L, hw, N = 7.0, 1.5, 8
    def deck_y(x):
        return 2.3 * (1 - (x / L) ** 2) + 0.2
    xs = [-L + 2 * L * k / N for k in range(N + 1)]
    for a, c in zip(xs, xs[1:]):
        ya, yc = deck_y(a), deck_y(c)
        bd.quad((a, ya, -hw), (c, yc, -hw), (c, yc, hw), (a, ya, hw), "flat", col=PLANK)
        bd.quad((a, ya - 0.35, -hw), (c, yc - 0.35, -hw), (c, yc - 0.35, hw), (a, ya - 0.35, hw), "flat", col=scale_col(PLANK, 0.6))
        for sz in (-1, 1):
            z = sz * hw
            bd.quad((a, ya - 0.35, z), (c, yc - 0.35, z), (c, yc, z), (a, ya, z), "flat", col=VERMILION)
            # The rail: a top beam between posts.
            bd.quad((a, ya + 1.05, z), (c, yc + 1.05, z), (c, yc + 1.2, z), (a, ya + 1.2, z), "flat", col=VERMILION)
    for x in xs[::2]:
        y = deck_y(x)
        for sz in (-1, 1):
            bd.box((x, y + 0.6, sz * hw), (0.09, 0.6, 0.09), 0, "flat", VERMILION)
            bd.tube((x, y + 1.2, sz * hw), (x, y + 1.45, sz * hw), 0.14, 0.001, 4, "flat", BLACK_LACQUER, BLACK_LACQUER)
    # Abutments.
    for sx in (-1, 1):
        bd.box((sx * (L + 0.8), 0.3, 0), (0.8, 0.6, hw + 0.3), 0, "ishigaki", WHITE, uv_scale=0.5)
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
        ("tkp_fuji", build_fuji(), False),
        ("tkp_fuji_lo", build_fuji("fuji_lo", True), False),
        ("tkp_sakura_a", build_sakura("sakura_a", 11), True),
        ("tkp_sakura_b", build_sakura("sakura_b", 23), True),
        ("tkp_torii", build_torii(), True),
        ("tkp_lantern", build_lantern(), True),
        ("tkp_pagoda", build_pagoda(), True),
        ("tkp_stand", build_stand(), True),
        ("tkp_chochin", build_chochin(), True),
        ("tkp_tower", build_tower(), True),
        ("tkp_pits", build_pits(), True),
        ("tkp_teahouse", build_teahouse(), True),
        ("tkp_bridge", build_bridge(), True),
    ]
    blender_props.bake(props, MATERIALS, PT_MATERIALS, a.header, a.blend, a.preview,
                       script="tools/build_tsudoken_props.py", guard="DK_TSUDOKEN_PROPS_H", label="tsudoken prop",
                       unit_note="Mt Fuji: millimetres of a unit mountain", mat_prefix="TKM", prop_prefix="TKP",
                       table="dk_props")

main()
