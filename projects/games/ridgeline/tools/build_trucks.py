#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26"]
# ///
"""Procedurally build Ridgeline's trucks (Cascade 84, Kestrel, Bobcat, FFR, Taco).

Usage:
    uv run tools/build_trucks.py                        # writes assets/generated/trucks.h
    uv run tools/build_trucks.py --preview-json X.json  # also dumps meshes for
                                                        # tools/preview_trucks_blender.py

Truck space: metres, +X = truck's right, +Y up, +Z forward; x = 0 on the centre
line, z = 0 midway between the axles, y = 0 on the ground at ride height. For
every triangle (a, b, c), cross(b - a, c - a) points out of the surface.

Parts: BODY (pivot = truck origin), WHEEL_L / WHEEL_R (pivot = wheel centre,
axle along X, rim face toward -X / +X), AXLE (unit length along X, -0.5..0.5,
the game scales X to the wheel-centre distance). Paint slots follow the
RL_PAINT_* enum; slots 1-7 store their default colour, and livery 0 equals
those defaults. Output is deterministic.
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
import meshasm  # noqa: E402
from meshasm import (check_mesh, index_rows, light_group_count, mesh_json, mesh_rows, pack,  # noqa: E402
                     position_count, vert_rows, weld, write_json)
from meshkit import (Geo, Paint, _orient, _pkey, box, ellipsoid, geosphere, lathe, lerp_rgb,  # noqa: E402
                     loft, merge, rgb, rot_x, rot_y, rot_z, tube, unit)

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "assets" / "generated" / "trucks.h"

TRUCK_NAMES = ["RL_TRUCK_CASCADE", "RL_TRUCK_KESTREL", "RL_TRUCK_BOBCAT", "RL_TRUCK_FFR",
               "RL_TRUCK_TACO"]
CASCADE, KESTREL, BOBCAT, FFR, TACO = 0, 1, 2, 3, 4
PART_NAMES = ["RL_PART_BODY", "RL_PART_WHEEL_L", "RL_PART_WHEEL_R", "RL_PART_AXLE"]
BODY, WHEEL_L, WHEEL_R, AXLE = range(4)
PAINT_NAMES = ["RL_PAINT_FIXED", "RL_PAINT_BODY", "RL_PAINT_ACCENT", "RL_PAINT_TRIM", "RL_PAINT_GLASS",
               "RL_PAINT_HEADLIGHT", "RL_PAINT_TAILLIGHT", "RL_PAINT_AUX"]
FIXED, S_BODY, S_ACCENT, S_TRIM, S_GLASS, S_HEAD, S_TAIL, S_AUX = range(8)

BUDGET = {   # (lod, part) -> max triangles
    (0, BODY): 1500, (0, WHEEL_L): 160, (0, WHEEL_R): 160, (0, AXLE): 64,
    (1, BODY): 450, (1, WHEEL_L): 48, (1, WHEEL_R): 48, (1, AXLE): 16,
    (2, BODY): 140, (2, WHEEL_L): 12, (2, WHEEL_R): 12,
}
BUDGET_LOD0_BODY_POSITIONS = 1100
BUDGET_LOD0_GROUPS = 550

# --------------------------------------------------------------------------
# Shared colours (FIXED unless noted)
# --------------------------------------------------------------------------


def fixed(r, g, b) -> Paint:
    return Paint(rgb(r, g, b), FIXED)


RUBBER = fixed(0x2A, 0x2A, 0x2D)
RUBBER_TREAD = fixed(0x34, 0x34, 0x37)
BLACK = fixed(0x26, 0x27, 0x2A)          # black plastic: flares, mirrors, grille
SATIN = fixed(0x36, 0x38, 0x3C)          # textured black steel: bumpers, rails
STEEL = fixed(0x44, 0x47, 0x4C)          # axles, frame
WELL = fixed(0x1A, 0x1A, 0x1C)           # wheel wells, underbody
UNDER = fixed(0x22, 0x22, 0x25)
INTERIOR = fixed(0x2E, 0x2E, 0x33)
SEAM = fixed(0x1E, 0x1E, 0x20)
AMBER = fixed(0xFF, 0x9C, 0x12)
REVERSE = fixed(0xF2, 0xF2, 0xEE)
SKIN = fixed(0xF2, 0xBC, 0x92)
HAIR = fixed(0x4A, 0x30, 0x22)
SHADES = fixed(0x18, 0x18, 0x1C)
RED_HOOK = fixed(0xD8, 0x1E, 0x1E)

GLASS_HI, GLASS_LO = rgb(0x8E, 0xB0, 0xCC), rgb(0x3C, 0x5A, 0x76)
GLASS = Paint(rgb(0x5C, 0x7C, 0x9A), S_GLASS)
HEAD_LENS = Paint(rgb(0xF4, 0xF6, 0xEC), S_HEAD)
TAIL_LENS = Paint(rgb(0xF4, 0x26, 0x24), S_TAIL)
AUX_LENS = Paint(rgb(0xFA, 0xF6, 0xDC), S_AUX)


def glass_at(y, y0, y1) -> Paint:
    t = min(1.0, max(0.0, (y - y0) / max(1e-6, y1 - y0)))
    return Paint(lerp_rgb(GLASS_LO, GLASS_HI, t), S_GLASS)


def _recolour(self: Geo, fn) -> Geo:
    """repaint() with fn(centroid, face_normal) -> Paint | None."""
    return self.repaint(lambda c, n, old: fn(c, n))


Geo.recolour = _recolour


def chrome(n) -> Paint:
    """Fake chrome from the normal: sky above a dark horizon, ground below."""
    ny = float(n[1])
    if ny > 0.25:
        c = lerp_rgb(rgb(0xB8, 0xC4, 0xD2), rgb(0xF6, 0xFA, 0xFF), min(1.0, (ny - 0.25) / 0.6))
    elif ny > -0.15:
        c = lerp_rgb(rgb(0x70, 0x78, 0x84), rgb(0xC8, 0xD2, 0xDE), (ny + 0.15) / 0.4)
    else:
        c = lerp_rgb(rgb(0x50, 0x56, 0x5E), rgb(0x70, 0x78, 0x84), max(0.0, (ny + 1.0) / 0.85))
    return Paint(c, FIXED)


def chrome_corners(g: Geo) -> Geo:
    return g.repaint_corners(lambda p, n, old: chrome(n))


# --------------------------------------------------------------------------
# Geometry helpers on top of meshkit
# --------------------------------------------------------------------------


def area2(pts) -> float:
    return 0.5 * sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1]
                     for i in range(len(pts)))


def _cross2(a, b) -> float:
    return a[0] * b[1] - a[1] * b[0]


def ear_clip(pts) -> list[tuple[int, int, int]]:
    """Triangulate a simple CCW polygon; picks the fattest ear each step."""
    pts = [np.asarray(p, dtype=float) for p in pts]
    idx = list(range(len(pts)))
    tris = []

    def inside(p, a, b, c):
        d1, d2, d3 = _cross2(b - a, p - a), _cross2(c - b, p - b), _cross2(a - c, p - c)
        return d1 > 1e-12 and d2 > 1e-12 and d3 > 1e-12

    def quality(a, b, c):
        def ang(p, q, r):
            u, v = q - p, r - p
            return math.acos(max(-1.0, min(1.0, float(np.dot(u, v) / (np.linalg.norm(u) * np.linalg.norm(v) + 1e-12)))))
        return min(ang(a, b, c), ang(b, c, a), ang(c, a, b))

    while len(idx) > 3:
        best, best_q = None, -1.0
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = pts[i0], pts[i1], pts[i2]
            if _cross2(b - a, c - a) <= 1e-12:
                continue
            if any(inside(pts[m], a, b, c) for m in idx if m not in (i0, i1, i2)):
                continue
            q = quality(a, b, c)
            if q > best_q + 1e-9:
                best, best_q = k, q
        if best is None:          # degenerate leftovers: clip anything
            best = 0
        k = best
        tris.append((idx[k - 1], idx[k], idx[(k + 1) % len(idx)]))
        idx.pop(k)
    tris.append((idx[0], idx[1], idx[2]))
    return tris


def offset_poly(pts, dists):
    """Move each edge of a CCW polygon inward by dists[edge] (edge i = pts[i] -> pts[i+1])."""
    n = len(pts)
    P = [np.asarray(p, dtype=float) for p in pts]
    out = []
    for i in range(n):
        e1 = unit(P[i] - P[i - 1])
        e2 = unit(P[(i + 1) % n] - P[i])
        n1 = np.array([-e1[1], e1[0]])
        n2 = np.array([-e2[1], e2[0]])
        d1, d2 = dists[i - 1], dists[i]
        a = P[i] + n1 * d1
        b = P[i] + n2 * d2
        den = _cross2(e1, e2)
        if abs(den) < 1e-6:
            out.append(0.5 * (a + b))
            continue
        t = _cross2(b - a, e2) / den
        q = a + e1 * t
        lim = 3.0 * max(d1, d2, 1e-9)
        if np.linalg.norm(q - P[i]) > lim:
            q = P[i] + unit(q - P[i]) * lim
        out.append(q)
    return out


AXES = {   # natural 2D coordinates -> (u, v) and basis U, V, A with U x V = A
    "x": (lambda p: (p[1], p[0]), np.array([0.0, 1, 0]), np.array([0.0, 0, 1]), np.array([1.0, 0, 0])),  # (z, y)
    "y": (lambda p: (p[1], p[0]), np.array([0.0, 0, 1]), np.array([1.0, 0, 0]), np.array([0.0, 1, 0])),  # (x, z)
    "z": (lambda p: (p[0], p[1]), np.array([1.0, 0, 0]), np.array([0.0, 1, 0]), np.array([0.0, 0, 1])),  # (x, y)
}


def smooth(g: Geo, angle: float, flat=None) -> Geo:
    """Angle-based smoothing where flat faces keep their own normal and pull
    the vertices of the bevels next to them onto it (seamless rounded edges)."""
    if not g.P:
        return g
    cos_t = math.cos(math.radians(angle)) - 1e-6
    P = np.array(g.P)
    fn = g.face_normals()
    flat = flat if flat is not None else [False] * len(P)
    groups: dict[tuple, list[tuple[int, int]]] = defaultdict(list)
    for t in range(len(P)):
        for k in range(3):
            groups[_pkey(P[t, k])].append((t, k))

    def corner_angle(t, k):
        a, b, c = P[t, k], P[t, (k + 1) % 3], P[t, (k + 2) % 3]
        u, v = unit(b - a), unit(c - a)
        return math.acos(max(-1.0, min(1.0, float(np.dot(u, v)))))

    for t in range(len(P)):
        if flat[t]:
            g.N[t] = np.array([fn[t]] * 3)
            continue
        ns = []
        for k in range(3):
            accf, accs = np.zeros(3), np.zeros(3)
            for t2, k2 in groups[_pkey(P[t, k])]:
                if float(np.dot(fn[t], fn[t2])) >= cos_t:
                    w = fn[t2] * corner_angle(t2, k2)
                    if flat[t2]:
                        accf += w
                    else:
                        accs += w
            ns.append(unit(accf if np.linalg.norm(accf) > 1e-9 else accs))
        g.N[t] = np.array(ns)
    return g


def _cap(g, ring3d, normal, paint, flags, flag=True):
    """Ear-clipped planar cap facing `normal`."""
    nrm = unit(normal)
    a = unit(np.cross(nrm, (0.0, 1.0, 0.0)) if abs(nrm[1]) < 0.9 else np.cross(nrm, (1.0, 0.0, 0.0)))
    b = np.cross(nrm, a)
    pts2 = [(float(np.dot(p, a)), float(np.dot(p, b))) for p in ring3d]
    order = list(range(len(ring3d)))
    if area2(pts2) < 0:
        order = order[::-1]
    pts2 = [pts2[i] for i in order]
    for i, j, k in ear_clip(pts2):
        before = len(g.P)
        g.tri([ring3d[order[i]], ring3d[order[j]], ring3d[order[k]]], None, paint)
        if len(g.P) > before:
            flags.append(flag)


def extrude(outline, axis: str, sections, paint, angle=50.0, edge_bevel=None, flat_len=0.22,
            caps=(True, True)) -> Geo:
    """Extrude a simple (possibly concave) 2D outline along an axis.

    outline: natural 2D points ((z, y) for "x", (x, z) for "y", (x, y) for "z").
    sections: [(w, inset), ...] with w increasing along the axis; inset moves the
    outline's edges inward (scaled per edge by edge_bevel(p, q), default 1), so
    a few sections with insets round off the edges. Caps and the faces of long
    edges in un-inset bands stay flat; everything else is smoothed."""
    to_uv, U, V, A = AXES[axis]
    nat = [tuple(map(float, p)) for p in outline]
    uv = [to_uv(p) for p in nat]
    if area2(uv) < 0:
        nat, uv = nat[::-1], uv[::-1]
    n = len(uv)
    eb = [1.0 if edge_bevel is None else float(edge_bevel(nat[i], nat[(i + 1) % n])) for i in range(n)]
    elen = [float(np.linalg.norm(np.subtract(uv[(i + 1) % n], uv[i]))) for i in range(n)]
    rings, insets = [], []
    for w, ins in sections:
        pts = offset_poly(uv, [ins * e for e in eb]) if ins > 0 else [np.asarray(p) for p in uv]
        rings.append([p[0] * U + p[1] * V + w * A for p in pts])
        insets.append(ins)
    g, flags = Geo(), []
    for i in range(len(rings) - 1):
        mid = insets[i] == 0 and insets[i + 1] == 0
        for j in range(n):
            j1 = (j + 1) % n
            quad = (rings[i][j], rings[i][j1], rings[i + 1][j1], rings[i + 1][j])
            for a, b, c in ((0, 1, 2), (0, 2, 3)):
                before = len(g.P)
                g.tri([quad[a], quad[b], quad[c]], None, paint)
                if len(g.P) > before:
                    flags.append(mid and elen[j] > flat_len)
    if caps[0]:
        _cap(g, rings[0], -A, paint, flags)
    if caps[1]:
        _cap(g, rings[-1], A, paint, flags)
    return smooth(g, angle, flags)


def round_sections(w0, w1, b, steps=2):
    """Sections for extrude() that round both ends with a quarter circle of radius b."""
    lo, hi = [], []
    for k in range(steps + 1):
        th = 0.5 * math.pi * k / steps
        lo.append((w0 + b * (1 - math.cos(th)), b * (1 - math.sin(th))))
    for w, ins in reversed(lo):
        hi.append((w1 - (w - w0), ins))
    return lo + hi


def sweep(path, profile, paint, angle=40.0, ups=None, up=(0.0, 1.0, 0.0), caps=(True, True),
          skip_edges=(), closed_path=False) -> Geo:
    """Sweep a closed 2D profile (a, b) along a 3D polyline; a runs along
    side = up x tangent, b along up. skip_edges are profile edges (i -> i+1 in
    the given order) left open, e.g. a face buried in the body."""
    path = [np.asarray(p, dtype=float) for p in path]
    n = len(path)
    prof = [tuple(map(float, p)) for p in profile]
    m = len(prof)
    edges_keep = [i not in skip_edges for i in range(m)]
    if area2(prof) < 0:
        prof = prof[::-1]
        edges_keep = [edges_keep[(m - 2 - i) % m] for i in range(m)]
    T, frames = [], []
    for k in range(n):
        if closed_path:
            tin, tout = unit(path[k] - path[k - 1]), unit(path[(k + 1) % n] - path[k])
        else:
            tin = unit(path[k] - path[k - 1]) if k > 0 else unit(path[1] - path[0])
            tout = unit(path[k + 1] - path[k]) if k < n - 1 else tin
        t = unit(tin + tout)
        u0 = np.asarray(ups[k] if ups is not None else up, dtype=float)
        u = unit(u0 - np.dot(u0, t) * t)
        s = np.cross(u, t)
        # mitre: stretch across the bend
        cosh = max(0.3, float(np.dot(tin, t)))
        bend = tout - tin
        bend = bend - np.dot(bend, t) * t
        bdir = unit(bend) if np.linalg.norm(bend) > 1e-9 else None
        ring = []
        for a, b in prof:
            o = a * s + b * u
            if bdir is not None:
                o = o + np.dot(o, bdir) * bdir * (1.0 / cosh - 1.0)
            ring.append(path[k] + o)
        frames.append(ring)
        T.append(t)
    g, flags = Geo(), []
    rows = n if closed_path else n - 1
    for i in range(rows):
        i1 = (i + 1) % n
        for j in range(m):
            if not edges_keep[j]:
                continue
            j1 = (j + 1) % m
            quad = (frames[i][j], frames[i][j1], frames[i1][j1], frames[i1][j])
            for a, b, c in ((0, 1, 2), (0, 2, 3)):
                before = len(g.P)
                g.tri([quad[a], quad[b], quad[c]], None, paint)
                if len(g.P) > before:
                    flags.append(False)
    if not closed_path:
        if caps[0]:
            _cap(g, frames[0], -T[0], paint, flags)
        if caps[1]:
            _cap(g, frames[-1], T[-1], paint, flags)
    return smooth(g, angle, flags)


def decal(pts, normal, paint, lift=0.003) -> Geo:
    """Flat polygon lifted off a surface along its normal."""
    nrm = unit(normal)
    ring = [np.asarray(p, dtype=float) + nrm * lift for p in pts]
    g, flags = Geo(), []
    _cap(g, ring, nrm, paint, flags)
    return g.flat()


def quad_x(x, z0, z1, y0, y1, paint, side=1.0, lift=0.003, slant=0.0) -> Geo:
    """Decal on a side face x = const (side = +1 faces +X). slant shifts the top edge in z."""
    pts = [(x, y0, z0), (x, y0, z1), (x, y1, z1 + slant), (x, y1, z0 + slant)]
    return decal(pts, (side, 0.0, 0.0), paint, lift)


def quad_z(z, x0, x1, y0, y1, paint, side=1.0, lift=0.003) -> Geo:
    pts = [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]
    return decal(pts, (0.0, 0.0, side), paint, lift)


def invert(g: Geo) -> Geo:
    for t in range(len(g.P)):
        g.P[t] = g.P[t][[0, 2, 1]]
        if g.N[t] is not None:
            g.N[t] = -g.N[t][[0, 2, 1]]
        c = g.C[t]
        g.C[t] = (c[0], c[2], c[1])
    return g


def drop(g: Geo, pred) -> Geo:
    """Remove triangles where pred(centroid, face_normal) is true."""
    fns = g.face_normals()
    out = Geo()
    for t in range(len(g.P)):
        if not pred(g.P[t].mean(axis=0), fns[t]):
            out.P.append(g.P[t])
            out.N.append(g.N[t])
            out.C.append(g.C[t])
    return out


def mirror_pair(g: Geo) -> Geo:
    return merge(g, g.mirrored_x())


def rbox(center, size, paint, bevel=0.0, angle=40.0, bottom=True) -> Geo:
    """Axis-aligned box rounded on its X edges (via extrude along X) or plain."""
    cx, cy, cz = center
    hx, hy, hz = (0.5 * s for s in size)
    outline = [(cz - hz, cy - hy), (cz + hz, cy - hy), (cz + hz, cy + hy), (cz - hz, cy + hy)]
    if bevel > 0:
        secs = round_sections(cx - hx, cx + hx, bevel, 1)
    else:
        secs = [(cx - hx, 0.0), (cx + hx, 0.0)]
    g = extrude(outline, "x", secs, paint, angle)
    if not bottom:
        g = drop(g, lambda c, n: n[1] < -0.99 and c[1] < cy - hy + 1e-4)
    return g


def lens_light(center, facing, radius, depth, segs, lens: Paint, body: Paint) -> Geo:
    """Round lamp: tapered housing with a domed lens on the front."""
    prof = [(-depth, 0.0), (-0.3 * depth, radius), (0.0, radius), (0.16 * radius, 0.0)]
    g = lathe(prof, segs, body, 35.0, face_paint=lambda i, j: lens if i == 2 else body, phase=math.pi / segs)
    # rotate +X onto facing
    f = unit(facing)
    if abs(f[1]) < 0.99:
        yaw = math.atan2(f[0], f[2])          # angle of facing around +Y measured from +Z
        m = rot_y(yaw) @ rot_y(-math.pi / 2)
    else:
        m = rot_z(math.pi / 2 if f[1] > 0 else -math.pi / 2)
    return g.xform(m, center)


# --------------------------------------------------------------------------
# Wheels and axle (shared builders)
# --------------------------------------------------------------------------


@dataclass
class WheelStyle:
    radius: float
    width: float
    lugs: int
    lug_pts: int          # 3 = V grooves, 4 = flat-bottomed grooves
    depth: float
    lug_frac: float
    lip_r: float          # rim lip radius (fraction of radius)
    dish_r: float         # rim dish radius (fraction of radius)
    rim: Paint
    rim_dark: Paint
    hub: Paint
    beadlock: bool


def wheel_lod0(ws: WheelStyle) -> Geo:
    """Right wheel (rim toward +X) at the wheel centre."""
    R, tw, N = ws.radius, 0.5 * ws.width, ws.lugs
    pitch = 2 * math.pi / N
    outline = []          # (r, theta)
    for j in range(N):
        a0 = j * pitch
        lf = ws.lug_frac
        if ws.lug_pts == 3:
            outline += [(R, a0), (R, a0 + lf * pitch), (R - ws.depth, a0 + (lf + 0.5 * (1 - lf)) * pitch)]
        else:
            g1 = 0.22 * (1 - lf)
            outline += [(R, a0), (R, a0 + lf * pitch), (R - ws.depth, a0 + (lf + g1) * pitch),
                        (R - ws.depth, a0 + (1 - g1) * pitch)]
    K = len(outline)

    def pt(x, r, th):
        return np.array([x, r * math.cos(th), r * math.sin(th)])

    ring_in = [pt(-tw, r, th) for r, th in outline]
    ring_out = [pt(tw, r, th) for r, th in outline]
    g = Geo()
    # tread band (lug tops lighter)
    for i in range(K):
        i1 = (i + 1) % K
        top = outline[i][0] == R and outline[i1][0] == R
        paint = RUBBER_TREAD if top else RUBBER
        a, b, c, d = ring_in[i], ring_in[i1], ring_out[i1], ring_out[i]
        g.tri([a, b, c], None, paint)
        g.tri([a, c, d], None, paint)
    tread = g
    # outer sidewall down to the rim lip
    side = Geo()
    lip_x, dish_x, hub_x = tw - 0.02, tw - 0.075, tw - 0.03
    lip = [pt(lip_x, ws.lip_r * R, j * pitch + 0.5 * ws.lug_frac * pitch) for j in range(N)]
    kp = ws.lug_pts
    for j in range(N):
        for m in range(kp):
            i = j * kp + m
            side.tri([ring_out[i], ring_out[(i + 1) % K], lip[j]], None, RUBBER)
        side.tri([ring_out[((j + 1) * kp) % K], lip[(j + 1) % N], lip[j]], None, RUBBER)
    _orient(side, lambda c: (1.0, 0.0, 0.0))
    # rim: lip band down into the dish, then a fan out to a domed hub
    rim = Geo()
    dish = [pt(dish_x, ws.dish_r * R, j * pitch + 0.5 * ws.lug_frac * pitch) for j in range(N)]
    hub = np.array([hub_x, 0.0, 0.0])
    for j in range(N):
        j1 = (j + 1) % N
        if ws.beadlock:
            band = ws.rim
            face = Paint(lerp_rgb(ws.rim.argb, rgb(0, 0, 0), 0.22), ws.rim.slot)
        else:
            band = ws.rim if j % 2 == 0 else ws.rim_dark
            face = ws.rim
        rim.tri([lip[j], lip[j1], dish[j1]], None, band)
        rim.tri([lip[j], dish[j1], dish[j]], None, band)
        rim.tri([dish[j], dish[j1], hub], None, (face, face, ws.hub))
    _orient(rim, lambda c: (1.0, 0.0, 0.0))
    # inner face
    inner = Geo()
    centre = np.array([-tw, 0.0, 0.0])
    for i in range(K):
        inner.tri([ring_in[i], centre, ring_in[(i + 1) % K]], None, (RUBBER, STEEL, RUBBER))
    _orient(inner, lambda c: (-1.0, 0.0, 0.0))
    return merge(smooth(tread, 30.0), smooth(side, 45.0), smooth(rim, 30.0), inner.flat())


def wheel_lod1(ws: WheelStyle) -> Geo:
    R, tw, N = ws.radius, 0.5 * ws.width, 8
    ph = math.pi / N

    def ring(x, r):
        return [np.array([x, r * math.cos(2 * math.pi * j / N + ph), r * math.sin(2 * math.pi * j / N + ph)])
                for j in range(N)]

    a, b = ring(-tw, R), ring(tw, R)
    lip = ring(tw - 0.03, ws.lip_r * R)
    hub = np.array([tw - 0.04, 0.0, 0.0])
    tread, side, rim, inner = Geo(), Geo(), Geo(), Geo()
    for j in range(N):
        j1 = (j + 1) % N
        tread.tri([a[j], a[j1], b[j1]], None, RUBBER)
        tread.tri([a[j], b[j1], b[j]], None, RUBBER)
        side.tri([b[j], b[j1], lip[j1]], None, RUBBER)
        side.tri([b[j], lip[j1], lip[j]], None, RUBBER)
        f = ws.rim if j % 2 == 0 or ws.beadlock else ws.rim_dark
        rim.tri([lip[j], lip[j1], hub], None, (f, f, ws.hub))
    _orient(side, lambda c: (1.0, 0.0, 0.0))
    _orient(rim, lambda c: (1.0, 0.0, 0.0))
    for j in range(1, N - 1):
        inner.tri([a[0], a[j], a[j + 1]], None, RUBBER)
    _orient(inner, lambda c: (-1.0, 0.0, 0.0))
    return merge(smooth(tread, 50.0), smooth(side, 30.0), rim.flat(), inner.flat())


def wheel_lod2(ws: WheelStyle) -> Geo:
    """Hexagonal bicone: reads as a wheel from the side and as a tyre edge-on."""
    R, tw, N = ws.radius, 0.5 * ws.width, 6
    ring = [np.array([0.0, R * math.cos(2 * math.pi * j / N), R * math.sin(2 * math.pi * j / N)]) for j in range(N)]
    out, inn = np.array([tw, 0.0, 0.0]), np.array([-tw, 0.0, 0.0])
    g = Geo()
    for j in range(N):
        j1 = (j + 1) % N
        g.tri([ring[j], ring[j1], out], None, (RUBBER, RUBBER, ws.rim))
        g.tri([ring[j1], ring[j], inn], None, RUBBER)
    _orient(g, lambda c: c)
    return smooth(g, 50.0)


def axle_lod0(span: float, wheel_inner: float) -> Geo:
    """Unit-length axle along X (the game scales X by `span`)."""
    k = 1.0 / span
    xe = 0.5 * span                       # wheel centre
    xi = wheel_inner - 0.02               # just inside the wheel's inner face
    beam = tube([(-xe, 0, 0), (-xi, 0, 0), (xi, 0, 0), (xe, 0, 0)], [0.085, 0.05, 0.05, 0.085], 6, STEEL,
                phase=math.pi / 6).autosmooth(35.0)
    pumpkin = ellipsoid((0.1 * span, 0.0, 0.015), (0.15, 0.15, 0.175), 6, [0, 62, 118, 180],
                        fixed(0x58, 0x5C, 0x62), frame=rot_z(math.pi / 2), phase=math.pi / 6)
    g = merge(beam, pumpkin)
    return g.xform(np.diag([k, 1.0, 1.0]))


def axle_lod1(span: float) -> Geo:
    k = 1.0 / span
    beam = tube([(-0.5 * span, 0, 0), (0.5 * span, 0, 0)], [0.06, 0.06], 4, STEEL, phase=math.pi / 4).flat()
    pumpkin = geosphere((0.1 * span, 0.0, 0.0), (0.15, 0.16, 0.17), 1, fixed(0x58, 0x5C, 0x62)).flat()
    return merge(beam, pumpkin).xform(np.diag([k, 1.0, 1.0]))


# --------------------------------------------------------------------------
# Shared body pieces
# --------------------------------------------------------------------------


def arch_points(cz, cy, rz, ry, y_foot, n=6, expo=2.6):
    """Superellipse wheel arch from the rear foot to the front foot, (z, y)."""
    s = ((y_foot - cy) / ry)
    s = max(-0.99, min(0.99, s))
    th_f = math.asin(math.copysign(abs(s) ** (expo / 2.0), s))
    pts = []
    for k in range(n + 1):
        th = (math.pi - th_f) + (2 * th_f - math.pi) * k / n
        c, sn = math.cos(th), math.sin(th)
        z = cz + rz * math.copysign(abs(c) ** (2.0 / expo), c)
        y = cy + ry * math.copysign(abs(sn) ** (2.0 / expo), sn)
        pts.append((z, y))
    pts[0] = (pts[0][0], y_foot)
    pts[-1] = (pts[-1][0], y_foot)
    return pts


def flare(arch, centre_zy, x_side, side, out, width, paint, lip=0.015, angle=40.0, wedge=False) -> Geo:
    """Fender flare swept round an arch on the side x = x_side (side = +1/-1)."""
    cz, cy = centre_zy
    path, ups = [], []
    for z, y in arch:
        r = unit((0.0, y - cy, z - cz))
        path.append((x_side, y, z))
        ups.append(r)
    # profile (a along side vector, b radial). side vector = up x tangent; find its sign vs X.
    t0 = unit(np.subtract(path[1], path[0]))
    sv = np.cross(ups[0], t0)
    sgn = 1.0 if sv[0] * side > 0 else -1.0
    if wedge:
        prof = [(-0.012 * sgn, -lip), (out * sgn, -lip), (-0.012 * sgn, width + 0.02)]
        return sweep(path, prof, paint, angle, ups=ups, skip_edges=(2,), caps=(True, True))
    prof = [(-0.012 * sgn, -lip), (out * sgn, -lip), (0.82 * out * sgn, width), (-0.012 * sgn, width + 0.02)]
    return sweep(path, prof, paint, angle, ups=ups, skip_edges=(3,), caps=(True, True))


def driver(x, y_shoulder, z, shirt: Paint, hat: Paint | None, steer_z, steer_y) -> Geo:
    """Head and shoulders behind a steering wheel."""
    g = Geo()
    g.extend(ellipsoid((x, y_shoulder - 0.07, z), (0.21, 0.17, 0.13), 6, [0, 55, 110], shirt, phase=math.pi / 6))
    hc = np.array([x, y_shoulder + 0.18, z + 0.01])
    g.extend(ellipsoid(hc, (0.10, 0.12, 0.11), 7, [0, 45, 90, 135, 180], SKIN, phase=math.pi / 7))
    if hat is not None:
        g.extend(ellipsoid(hc + (0, 0.03, -0.005), (0.108, 0.10, 0.117), 7, [0, 45, 85], hat, phase=math.pi / 7))
        g.extend(box(hc + (0, 0.045, 0.13), (0.17, 0.016, 0.10), hat))
    else:
        g.extend(ellipsoid(hc + (0, 0.025, -0.01), (0.107, 0.10, 0.115), 7, [0, 45, 85], HAIR, phase=math.pi / 7))
    g.extend(box(hc + (0, 0.012, 0.10), (0.16, 0.032, 0.02), SHADES))
    # steering wheel rim in front of the chest
    m = rot_x(math.radians(-60))
    rim = [m @ np.array([0.17 * math.cos(a), 0.0, 0.17 * math.sin(a)]) + (x, steer_y, steer_z)
           for a in (2 * math.pi * k / 6 for k in range(6))]
    g.extend(tube(rim, [0.02] * 6, 3, BLACK, closed_path=True, up=m @ np.array([0, 1.0, 0])).autosmooth(70.0))
    return g


def cab_glass(rings, paint_fn, window_fn, angle=30.0) -> Geo:
    """Greenhouse loft over rings (bottom to top, convex), roof cap on top.
    paint_fn(centroid, normal) -> Paint; window_fn(centroid, normal) -> True drops the face (open window)."""
    g = loft(rings, BODY_PLACEHOLDER, angle, cap0=False, cap1=True)
    g = smooth(g, angle)
    g.recolour(paint_fn)
    return drop(g, window_fn)


BODY_PLACEHOLDER = fixed(0xFF, 0x00, 0xFF)


def plan_ring(y, hw, zf, zb, c, side_z, hwr):
    """Greenhouse ring at height y, CCW about +Y: front edge (windshield),
    chamfered corners, each side broken at side_z (front to back, for window
    and pillar faces), back with a centre window of half-width hwr."""
    if not isinstance(side_z, (list, tuple)):
        side_z = [side_z]
    right = [(hw, y, zf - c)] + [(hw, y, z) for z in side_z] + [(hw, y, zb + c)]
    left = [(-x, yy, z) for x, yy, z in reversed(right)]
    return ([(-hw + c, y, zf), (hw - c, y, zf)] + right + [(hw - c, y, zb), (hwr, y, zb), (-hwr, y, zb),
                                                           (-hw + c, y, zb)] + left)


def shrink_ring(ring, d, dy=0.0):
    """Offset a convex plan ring inward by d (and move it by dy)."""
    xz = [(p[2], p[0]) for p in ring]          # (u, v) for the Y axis
    flip = area2(xz) < 0
    if flip:
        xz = xz[::-1]
    off = offset_poly(xz, [d] * len(xz))
    if flip:
        off = off[::-1]
    return [(float(v), ring[i][1] + dy, float(u)) for i, (u, v) in enumerate(off)]


# --------------------------------------------------------------------------
# CASCADE 84: early-80s compact 4x4 pickup
# --------------------------------------------------------------------------

C_R, C_TW = 0.41, 0.29
C_WX, C_WZ = 0.725, 1.30
C_HW = 0.80
C_ROCK = 0.62
C_BELT = 1.22
C_FRONT = 1.98
C_CABB = -0.56
C_BEDF, C_BEDB = -0.64, -2.13
C_ARCH = (0.55, 0.53)            # wheel arch half-length, height above the wheel centre

C_BODY_RGB = rgb(0xD8, 0x20, 0x1E)
C_STRIPE_RGB = [rgb(0xFF, 0xCE, 0x1E), rgb(0xFF, 0x84, 0x14), rgb(0xFF, 0x4E, 0x16)]   # top to bottom
C_ACCENT_RGB = C_STRIPE_RGB[1]
C_TRIM_RGB = rgb(0xF6, 0xEE, 0xD8)

C_BODY = Paint(C_BODY_RGB, S_BODY)
C_TRIM = Paint(C_TRIM_RGB, S_TRIM)
C_STRIPES = [Paint(c, S_ACCENT) for c in C_STRIPE_RGB]
C_SHIRT = fixed(0x2C, 0x6C, 0xC8)
C_HAT = fixed(0xF4, 0xF0, 0xE6)

C_WHEEL = WheelStyle(C_R, C_TW, 10, 3, 0.034, 0.60, 0.64, 0.40, Paint(C_TRIM_RGB, S_TRIM),
                     fixed(0x2C, 0x2C, 0x30), Paint(lerp_rgb(C_TRIM_RGB, 0xFF000000, 0.35), S_TRIM), beadlock=False)

C_HEADLIGHT_C = [(-0.585, 0.925, C_FRONT + 0.031), (0.585, 0.925, C_FRONT + 0.031)]
C_TAIL_C = [(-0.72, 0.92, C_BEDB - 0.03), (0.72, 0.92, C_BEDB - 0.03)]
C_AUX_X = [-0.48, -0.16, 0.16, 0.48]
C_ROLL_Z, C_ROLL_TOP = -0.83, 2.02
C_AUX_C = [(x, C_ROLL_TOP + 0.10, C_ROLL_Z + 0.03) for x in C_AUX_X]
C_EXHAUST = (0.52, 0.49, -2.08)
C_DRIVER = (-0.36, 1.42, -0.16)


def c_arch(z, lod, y_foot=C_ROCK):
    return arch_points(z, C_R, C_ARCH[0], C_ARCH[1], y_foot, n=(5, 3, 2)[lod])


def not_bottom(y):
    return lambda p, q: 0.0 if max(p[1], q[1]) < y else 1.0


def well_paint(cz, hw):
    """Paint the inside of a wheel arch tunnel dark, the rest untouched."""
    def paint(c, n):
        if n[1] < -0.7:
            return UNDER
        d = np.array([c[2] - cz, c[1] - C_R])
        if c[1] < C_R + C_ARCH[1] + 0.03 and abs(c[2] - cz) < C_ARCH[0] + 0.08 and \
                float(np.dot(n[[2, 1]], d)) < 0 and abs(c[0]) < hw:
            return WELL
        return None
    return paint


def cascade_lower(lod: int) -> Geo:
    """Front clip and cab below the beltline: one rounded extrusion across X."""
    outline = [(C_FRONT, 0.66), (C_FRONT, 1.105), (C_FRONT - 0.03, 1.14), (0.84, 1.205),
               (C_CABB, C_BELT), (C_CABB, C_ROCK)] + c_arch(C_WZ, lod)
    secs = [round_sections(-C_HW, C_HW, 0.05, 2), round_sections(-C_HW, C_HW, 0.05, 1),
            [(-C_HW, 0.0), (C_HW, 0.0)]][lod]
    g = extrude(outline, "x", secs, C_BODY, 50.0, edge_bevel=not_bottom(0.67))
    g.recolour(well_paint(C_WZ, C_HW - 0.02))
    return g.recolour(lambda c, n: INTERIOR if n[1] > 0.9 and C_CABB < c[2] < 0.84 and abs(c[0]) < C_HW - 0.03
                      else None)


def cascade_cab(lod: int) -> Geo:
    zb1 = -0.30
    y_sill = 1.31
    if lod == 2:
        rings = [[(-0.775, 1.20, 0.84), (0.775, 1.20, 0.84), (0.775, 1.20, -0.55), (-0.775, 1.20, -0.55)],
                 [(-0.67, 1.89, 0.32), (0.67, 1.89, 0.32), (0.67, 1.89, -0.50), (-0.67, 1.89, -0.50)]]
        g = loft(rings, C_TRIM, 20.0, cap0=False, cap1=True).flat()
        g.recolour(lambda c, n: GLASS if n[2] > 0.4 else None)
        for sgn in (1.0, -1.0):
            g.extend(decal([(sgn * 0.765, 1.34, 0.72), (sgn * 0.765, 1.34, -0.30), (sgn * 0.69, 1.82, -0.30),
                            (sgn * 0.69, 1.82, 0.40)], (sgn, 0.12, 0.0), GLASS, 0.004))
        g.extend(decal([(-0.40, 1.36, -0.551), (0.40, 1.36, -0.551), (0.38, 1.80, -0.515), (-0.38, 1.80, -0.515)],
                       (0, 0.05, -1), GLASS, 0.004))
        return g
    rings = [plan_ring(1.20, 0.775, 0.84, -0.55, 0.06, zb1, 0.44),
             plan_ring(y_sill, 0.765, 0.765, -0.548, 0.07, zb1, 0.44),
             plan_ring(1.85, 0.69, 0.34, -0.52, 0.10, zb1, 0.40),
             plan_ring(1.915, 0.635, 0.28, -0.47, 0.12, zb1, 0.36)]
    if lod == 1:
        rings = [rings[0], rings[1], plan_ring(1.905, 0.66, 0.31, -0.50, 0.11, zb1, 0.38)]
    y_top = rings[2][0][1]

    def paint(c, n):
        if c[1] < y_sill - 0.005:
            return C_BODY
        if c[1] < y_top - 0.002 and n[2] > 0.4 and abs(n[0]) < 0.35:
            return GLASS
        if n[1] > 0.5 or c[1] > y_top - 0.002:
            return C_TRIM
        if n[2] < -0.95 and abs(c[0]) < 0.38:
            return GLASS
        if lod > 0 and abs(n[0]) > 0.9 and c[2] > zb1:
            return GLASS
        return C_TRIM

    def window(c, n):
        return lod == 0 and abs(n[0]) > 0.95 and c[1] > y_sill and c[2] > zb1

    g = cab_glass(rings, paint, window)
    g.repaint_corners(lambda p, n, old: glass_at(p[1], y_sill, y_top) if old.slot == S_GLASS else old)
    if lod == 0:
        inner = [shrink_ring(rings[1], 0.03, -0.12), shrink_ring(rings[2], 0.03, -0.03)]
        g.extend(invert(loft(inner, INTERIOR, 20.0, cap0=False, cap1=True).flat()))
    return g


def cascade_bed(lod: int) -> Geo:
    G = Geo()
    hw = C_HW
    if lod == 2:
        bed = rbox((0, 0.92, (C_BEDF + C_BEDB) / 2), (2 * hw, 0.60, abs(C_BEDB - C_BEDF)), C_BODY, bottom=False)
        bed.recolour(lambda c, n: BLACK if n[1] > 0.9 else None)
        return G.extend(bed)
    wall = [(C_BEDF, C_BELT), (C_BEDB, C_BELT), (C_BEDB, C_ROCK)] + c_arch(-C_WZ, lod) + [(C_BEDF, C_ROCK)]
    # flat inner face, outer edges rounded (LOD0) or square
    secs = [(0.75, 0.0)] + round_sections(0.75, hw, 0.04, 2)[3:] if lod == 0 else [(0.75, 0.0), (hw, 0.0)]
    w = extrude(wall, "x", secs, C_BODY, 50.0, edge_bevel=not_bottom(0.67))
    w.recolour(well_paint(-C_WZ, 0.79))
    G.extend(mirror_pair(w))
    G.extend(rbox((0, 0.94, C_BEDF - 0.025), (1.50, 0.56, 0.05), C_BODY, bottom=False))
    floor = rbox((0, 0.84, (C_BEDF + C_BEDB) / 2), (1.50, 0.05, abs(C_BEDB - C_BEDF) - 0.02), BLACK)
    G.extend(drop(floor, lambda c, n: abs(n[0]) > 0.9 or abs(n[2]) > 0.9))
    post = drop(rbox((0.695, 0.93, C_BEDB + 0.05), (0.11, 0.58, 0.10), C_BODY, bottom=False),
                lambda c, n: n[0] > 0.9)
    G.extend(mirror_pair(post))
    G.extend(rbox((0, 0.945, C_BEDB + 0.025), (1.28, 0.51, 0.05), C_BODY, bevel=0.02 if lod == 0 else 0.0))
    if lod == 0:
        for k in range(3):
            x = -0.42 + 0.42 * k
            G.extend(decal([(x - 0.05, 0.865, C_BEDF - 0.06), (x + 0.05, 0.865, C_BEDF - 0.06),
                            (x + 0.05, 0.865, C_BEDB + 0.08), (x - 0.05, 0.865, C_BEDB + 0.08)],
                           (0, 1, 0), fixed(0x3A, 0x3B, 0x3F), 0.002))
        tub = drop(rbox((0.625, 0.93, -C_WZ), (0.25, 0.17, 1.02), C_BODY), lambda c, n: n[0] > 0.9)
        G.extend(mirror_pair(tub.recolour(lambda c, n: WELL if n[1] < -0.9 else None)))
        G.extend(quad_z(C_BEDB, -0.09, 0.09, 1.10, 1.135, SEAM, side=-1.0))
    return G


def cascade_body(lod: int) -> Geo:
    G = Geo()
    hw = C_HW
    G.extend(cascade_lower(lod))
    G.extend(cascade_cab(lod))
    G.extend(cascade_bed(lod))

    # ---- stripes (80s tri-colour) and panel detail
    if lod == 0:
        bands = [(1.087, 1.115), (1.049, 1.077), (1.011, 1.039)]
        for paint, (y0, y1) in zip(C_STRIPES, bands):
            for sgn in (1.0, -1.0):
                G.extend(quad_x(sgn * hw, C_CABB + 0.05, 1.80 + (y0 - 1.0) * 0.8, y0, y1, paint, sgn))
                G.extend(quad_x(sgn * hw, C_BEDB + 0.045, C_BEDF - 0.04, y0, y1, paint, sgn))
            G.extend(quad_z(C_BEDB, -0.64, 0.64, y0, y1, paint, side=-1.0))
        for sgn in (1.0, -1.0):
            for z in (0.83, -0.29):
                G.extend(quad_x(sgn * hw, z - 0.005, z + 0.005, 0.66, 1.17, SEAM, sgn, lift=0.004))
            G.extend(rbox((sgn * (hw + 0.012), 1.145, -0.20), (0.024, 0.026, 0.13), BLACK))
            x = sgn * 0.70
            G.extend(decal([(x - 0.006, 1.207, 0.86), (x + 0.006, 1.207, 0.86),
                            (x + 0.006, 1.143, C_FRONT - 0.04), (x - 0.006, 1.143, C_FRONT - 0.04)],
                           (0, 0.998, 0.055), SEAM, 0.004))
        G.extend(decal([(-0.66, 1.207, 0.86), (0.66, 1.207, 0.86), (0.66, 1.203, 0.93), (-0.66, 1.203, 0.93)],
                       (0, 0.998, 0.055), BLACK, 0.003))
        for x0 in (-0.52, 0.02):
            G.extend(decal([(x0, 1.335, 0.79), (x0 + 0.42, 1.335, 0.79), (x0 + 0.42, 1.357, 0.773),
                            (x0, 1.357, 0.773)], (0, 0.70, 0.71), BLACK, 0.004))
    elif lod == 1:
        for sgn in (1.0, -1.0):
            G.extend(quad_x(sgn * hw, C_CABB + 0.04, 1.82, 1.011, 1.115, C_STRIPES[1], sgn))
            G.extend(quad_x(sgn * hw, C_BEDB + 0.04, C_BEDF - 0.04, 1.011, 1.115, C_STRIPES[1], sgn))

    # ---- fender flares and mud flaps
    if lod < 2:
        for cz, foot in ((C_WZ, C_ROCK), (-C_WZ, C_ROCK)):
            G.extend(mirror_pair(flare(c_arch(cz, lod, foot), (cz, C_R), hw, 1.0, 0.075, 0.075, BLACK,
                                       wedge=lod > 0)))
    if lod == 0:
        for z in (C_WZ - 0.52, -C_WZ - 0.52):
            G.extend(mirror_pair(drop(rbox((0.715, 0.47, z), (0.27, 0.30, 0.02), BLACK),
                                      lambda c, n: n[1] > 0.9)))

    # ---- front end: grille, headlights, bumper, hooks
    zf = C_FRONT
    if lod == 0:
        G.extend(chrome_corners(rbox((0, 0.925, zf + 0.012), (1.52, 0.25, 0.026), BLACK)))
        G.extend(quad_z(zf + 0.025, -0.40, 0.40, 0.825, 1.025, BLACK, lift=0.002))
        for k in range(4):
            slat = rbox((0, 0.85 + 0.05 * k, zf + 0.034), (0.78, 0.016, 0.014), BLACK)
            G.extend(chrome_corners(drop(slat, lambda c, n: n[2] < -0.9 or abs(n[0]) > 0.9)))
        for sgn in (1.0, -1.0):
            x0, x1 = sorted((sgn * 0.465, sgn * 0.705))
            G.extend(quad_z(zf + 0.025, x0, x1, 0.855, 0.995, HEAD_LENS, lift=0.004))
    else:
        G.extend(quad_z(zf, -0.40, 0.40, 0.83, 1.02, BLACK))
        for sgn in (1.0, -1.0):
            x0, x1 = sorted((sgn * 0.46, sgn * 0.71))
            G.extend(quad_z(zf, x0, x1, 0.85, 1.0, HEAD_LENS))

    if lod < 2:
        bump = [(-0.86, 0.67, zf - 0.02), (-0.80, 0.67, zf + 0.11), (0.80, 0.67, zf + 0.11), (0.86, 0.67, zf - 0.02)]
        prof = [(-0.05, -0.085), (0.04, -0.085), (0.05, -0.06), (0.05, 0.06), (0.04, 0.085), (-0.05, 0.085)] \
            if lod == 0 else [(-0.05, -0.085), (0.05, -0.085), (0.05, 0.085), (-0.05, 0.085)]
        G.extend(chrome_corners(sweep(bump, prof, BLACK, 40.0)))
    else:
        G.extend(chrome_corners(rbox((0, 0.67, zf + 0.06), (1.70, 0.17, 0.10), BLACK, bottom=False)))
    if lod == 0:
        for sgn in (1.0, -1.0):
            x0, x1 = sorted((sgn * 0.56, sgn * 0.72))
            G.extend(quad_z(zf + 0.16, x0, x1, 0.625, 0.665, AMBER, lift=0.0015))
            hook = tube([(sgn * 0.40, 0.57, zf + 0.06), (sgn * 0.40, 0.53, zf + 0.18), (sgn * 0.40, 0.595, zf + 0.24)],
                        [0.022, 0.022, 0.022], 3, RED_HOOK, cap1=RED_HOOK, up=(1, 0, 0), phase=math.pi / 2)
            G.extend(hook.autosmooth(50.0))

    # ---- rear: taillights, bumper
    for sgn in (1.0, -1.0):
        tc = C_TAIL_C[0 if sgn < 0 else 1]
        if lod == 0:
            bezel = rbox((tc[0], tc[1] + 0.03, C_BEDB - 0.008), (0.15, 0.32, 0.016), BLACK)
            G.extend(chrome_corners(drop(bezel, lambda c, n: n[2] > 0.9)))
            lens = rbox((tc[0], tc[1] + 0.035, C_BEDB - 0.022), (0.12, 0.26, 0.014), TAIL_LENS)
            G.extend(drop(lens, lambda c, n: n[2] > 0.9 or abs(n[0]) > 0.9))
            G.extend(quad_z(C_BEDB - 0.029, tc[0] - 0.06, tc[0] + 0.06, 0.835, 0.875, AMBER, side=-1.0, lift=0.002))
            G.extend(quad_z(C_BEDB - 0.029, tc[0] - 0.06, tc[0] + 0.06, 0.80, 0.83, REVERSE, side=-1.0,
                            lift=0.002))
        else:
            G.extend(quad_z(C_BEDB, tc[0] - 0.07, tc[0] + 0.07, 0.76, 1.08, TAIL_LENS, side=-1.0))
    if lod == 0:
        rb = [(0.84, 0.60, C_BEDB - 0.01), (0.80, 0.60, C_BEDB - 0.09), (-0.80, 0.60, C_BEDB - 0.09),
              (-0.84, 0.60, C_BEDB - 0.01)]
        rprof = [(-0.04, -0.07), (0.04, -0.07), (0.05, -0.05), (0.05, 0.07), (-0.04, 0.07)] if lod == 0 else \
            [(-0.04, -0.07), (0.05, -0.07), (0.05, 0.07), (-0.04, 0.07)]
        G.extend(chrome_corners(sweep(rb, rprof, BLACK, 40.0)))
    else:
        G.extend(chrome_corners(rbox((0, 0.60, C_BEDB - 0.05), (1.66, 0.14, 0.09), BLACK, bottom=False)))

    # ---- roll bar with four round lights
    if lod < 2:
        bar = [(-0.70, 0.88, C_ROLL_Z), (-0.70, C_ROLL_TOP - 0.10, C_ROLL_Z), (-0.60, C_ROLL_TOP, C_ROLL_Z),
               (0.60, C_ROLL_TOP, C_ROLL_Z), (0.70, C_ROLL_TOP - 0.10, C_ROLL_Z), (0.70, 0.88, C_ROLL_Z)]
        G.extend(tube(bar, [0.036] * 6, (5, 3)[lod], BLACK, up=(0, 0, 1),
                      phase=(math.pi / 2, math.pi / 2)[lod]).autosmooth(50.0))
        for c in C_AUX_C:
            if lod == 0:
                G.extend(lens_light(c, (0, 0, 1), 0.088, 0.10, 8, AUX_LENS, BLACK))
            else:
                lamp = rbox((c[0], c[1], c[2] - 0.04), (0.16, 0.16, 0.09), BLACK, bottom=False)
                G.extend(lamp.recolour(lambda cc, n: AUX_LENS if n[2] > 0.9 else None))
    else:
        G.extend(rbox((0, C_ROLL_TOP + 0.07, C_ROLL_Z), (1.30, 0.17, 0.12), BLACK, bottom=False).recolour(
            lambda c, n: AUX_LENS if n[2] > 0.9 else None))

    # ---- mirrors, drip rails, antenna
    if lod == 0:
        for sgn in (1.0, -1.0):
            head = rbox((sgn * 0.93, 1.42, 0.69), (0.10, 0.14, 0.045), BLACK)
            G.extend(head.recolour(lambda c, n: GLASS if n[2] < -0.9 else None))
            if lod == 0:
                G.extend(tube([(sgn * 0.76, 1.30, 0.73), (sgn * 0.89, 1.37, 0.70)], [0.014, 0.014], 4, BLACK,
                              up=(0, 0, 1), phase=math.pi / 4).flat())
    if lod == 0:
        for sgn in (1.0, -1.0):
            rail = [(sgn * 0.735, 1.31, 0.735), (sgn * 0.645, 1.853, 0.295), (sgn * 0.697, 1.853, 0.235),
                    (sgn * 0.697, 1.853, -0.47)]
            G.extend(chrome_corners(sweep(rail, [(-0.008, -0.008), (0.008, -0.008), (0.008, 0.008),
                                                 (-0.008, 0.008)], BLACK, 40.0, up=(sgn, 0.3, 0))))
            # chrome bed rail on stanchions
            bed_rail = [(sgn * 0.775, 1.20, -0.98), (sgn * 0.775, 1.30, -1.03), (sgn * 0.775, 1.30, -1.98),
                        (sgn * 0.775, 1.20, -2.03)]
            G.extend(chrome_corners(tube(bed_rail, [0.017] * 4, 4, BLACK, up=(0, 1, 0),
                                         phase=math.pi / 4).autosmooth(50.0)))
            # side marker lamps
            G.extend(quad_x(sgn * hw, 1.85, 1.93, 0.86, 0.90, AMBER, sgn, lift=0.003))
            G.extend(quad_x(sgn * hw, -2.05, -1.97, 0.86, 0.90, fixed(0xC8, 0x14, 0x14), sgn, lift=0.003))
        G.extend(tube([(0.79, 1.26, 0.82), (0.83, 2.28, 0.74)], [0.006, 0.003], 3, BLACK, phase=0.0).flat())

    # ---- underbody: frame rails, skid, tank, exhaust
    if lod == 0:
        for sgn in (1.0, -1.0):
            G.extend(rbox((sgn * 0.42, 0.55, -0.08), (0.08, 0.14, 3.9), UNDER))
        G.extend(rbox((0, 0.56, 0.0), (0.70, 0.12, 1.30), UNDER))
        G.extend(rbox((0, 0.57, -1.70), (0.70, 0.12, 0.60), STEEL))
        G.extend(tube([(0.25, 0.52, 0.70), (0.30, 0.49, 0.0), (0.52, 0.49, -0.70), C_EXHAUST],
                      [0.03] * 4, 4, STEEL, cap1=SEAM, up=(0, 1, 0), phase=math.pi / 4).autosmooth(50.0))
    elif lod == 1:
        G.extend(rbox((0, 0.56, -0.1), (0.84, 0.12, 3.9), UNDER, bottom=False))

    # ---- driver and dash
    if lod == 0:
        G.extend(rbox((0, 1.27, 0.62), (1.40, 0.16, 0.30), INTERIOR, bottom=False))
        G.extend(driver(C_DRIVER[0], C_DRIVER[1], C_DRIVER[2], C_SHIRT, C_HAT, 0.33, 1.44))
    elif lod == 1:
        G.extend(ellipsoid((C_DRIVER[0], C_DRIVER[1] + 0.18, C_DRIVER[2]), (0.10, 0.12, 0.11), 4, [0, 90, 180],
                           SKIN, phase=math.pi / 4))
    return G


def shade_low(g: Geo, slot: int, y0: float, y1: float, lo: float, steps: int = 3) -> Geo:
    """Darken a paint slot toward the bottom (baked occlusion), in a few steps so
    it adds few lighting groups. The game keeps relative brightness within a slot."""
    def fn(p, n, old):
        if old.slot != slot:
            return old
        t = min(1.0, max(0.0, (float(p[1]) - y0) / (y1 - y0)))
        t = round(t * steps) / steps
        k = lo + (1.0 - lo) * t
        r, gg, b = (old.argb >> 16) & 0xFF, (old.argb >> 8) & 0xFF, old.argb & 0xFF
        return Paint(rgb(int(round(r * k)), int(round(gg * k)), int(round(b * k))), slot)
    return g.repaint_corners(fn)


def cascade_parts(lod: int) -> dict[int, Geo]:
    if lod == 0:
        w, a = wheel_lod0(C_WHEEL), axle_lod0(2 * C_WX, C_WX - 0.5 * C_TW)
    elif lod == 1:
        w, a = wheel_lod1(C_WHEEL), axle_lod1(2 * C_WX)
    else:
        w, a = wheel_lod2(C_WHEEL), None
    body = cascade_body(lod)
    if lod == 0:
        shade_low(body, S_BODY, C_ROCK, 1.0, 0.82)
    parts = {BODY: body, WHEEL_R: w, WHEEL_L: w.mirrored_x()}
    if a is not None:
        parts[AXLE] = a
    return parts


CASCADE_INFO = {
    "name": "CASCADE 84",
    "wheel_pos": [(-C_WX, C_R, C_WZ), (C_WX, C_R, C_WZ), (-C_WX, C_R, -C_WZ), (C_WX, C_R, -C_WZ)],
    "wheel_radius": C_R, "wheel_width": C_TW,
    "headlight": C_HEADLIGHT_C, "headlight_r": 0.09,
    "aux": [(x, y, z + 0.016) for x, y, z in C_AUX_C],
    "taillight": [(x, y + 0.035, C_BEDB - 0.03) for x, y, _z in C_TAIL_C],
    "exhaust": C_EXHAUST,
    "driver_eye": (C_DRIVER[0], C_DRIVER[1] + 0.20, C_DRIVER[2] + 0.10),
    "liveries": [
        ("SUNSET RED", C_BODY_RGB, C_ACCENT_RGB, C_TRIM_RGB, 0),
        ("SURF WHITE", rgb(0xF2, 0xF2, 0xEE), rgb(0x22, 0x9C, 0xE8), rgb(0x24, 0x3C, 0x8C), 0),
        ("DESERT TAN", rgb(0xD8, 0xB4, 0x7A), rgb(0xE8, 0x6A, 0x1C), rgb(0x5A, 0x3A, 0x22), 0),
        ("MIDNIGHT", rgb(0x1E, 0x2A, 0x5A), rgb(0xFF, 0xB0, 0x1C), rgb(0xE8, 0xE8, 0xE4), 0),
    ],
}


# --------------------------------------------------------------------------
# KESTREL: modern two-door off-road SUV with a removable hardtop
# --------------------------------------------------------------------------

K_R, K_TW = 0.45, 0.34
K_WX, K_WZ = 0.84, 1.275
K_HW = 0.90
K_ROCK = 0.66
K_BELT = 1.25
K_FRONT = 1.95
K_REAR = -1.86
K_COWL = 0.80
K_ARCH = (0.60, 0.57)
K_SIDE_Z = [-0.46, -0.60, -1.52]      # door window end, B pillar end, quarter window end
K_ROOF = 1.92

K_BODY_RGB = rgb(0xFF, 0x62, 0x10)
K_ACCENT_RGB = rgb(0xF4, 0xF4, 0xF0)
K_TRIM_RGB = rgb(0xB4, 0x8C, 0x4E)

K_BODY = Paint(K_BODY_RGB, S_BODY)
K_TOP = Paint(K_ACCENT_RGB, S_ACCENT)
K_RIM = Paint(K_TRIM_RGB, S_TRIM)
K_SHIRT = fixed(0x2A, 0x9C, 0x84)

K_WHEEL = WheelStyle(K_R, K_TW, 8, 4, 0.052, 0.56, 0.62, 0.44, K_RIM, fixed(0x30, 0x30, 0x34),
                     Paint(lerp_rgb(K_TRIM_RGB, 0xFF000000, 0.6), S_TRIM), beadlock=True)
K_SPARE = WheelStyle(K_R, K_TW * 0.9, 8, 3, 0.05, 0.56, 0.62, 0.44, K_RIM, fixed(0x30, 0x30, 0x34),
                     Paint(lerp_rgb(K_TRIM_RGB, 0xFF000000, 0.6), S_TRIM), beadlock=True)

K_HEAD_C = [(-0.665, 1.01, K_FRONT + 0.04), (0.665, 1.01, K_FRONT + 0.04)]
K_TAIL_C = [(-0.83, 1.02, K_REAR - 0.02), (0.83, 1.02, K_REAR - 0.02)]
K_BAR_Z, K_BAR_Y = 0.30, K_ROOF + 0.13
K_AUX_C = [(x, K_BAR_Y, K_BAR_Z + 0.055) for x in (-0.48, -0.16, 0.16, 0.48)]
K_EXHAUST = (0.55, 0.56, -1.80)
K_DRIVER = (-0.40, 1.46, -0.22)
K_SPARE_C = (0.0, 1.03, K_REAR - 0.21)


def k_arch(z, lod):
    return arch_points(z, K_R, K_ARCH[0], K_ARCH[1], K_ROCK, n=(6, 3, 2)[lod], expo=4.0)


def k_well(cz, hw):
    def paint(c, n):
        if n[1] < -0.7:
            return UNDER
        d = np.array([c[2] - cz, c[1] - K_R])
        if c[1] < K_R + K_ARCH[1] + 0.03 and abs(c[2] - cz) < K_ARCH[0] + 0.08 and \
                float(np.dot(n[[2, 1]], d)) < 0 and abs(c[0]) < hw:
            return WELL
        return None
    return paint


def round_lamp(center, r_out, r_mid, r_in, segs, bezel: Paint, ring: Paint, lens: Paint, dome=0.02) -> Geo:
    """Flat round headlamp facing +Z: bezel annulus, bright ring, domed lens."""
    cx, cy, cz = center

    def circ(r, z):
        return [np.array([cx + r * math.cos(a), cy + r * math.sin(a), z])
                for a in (2 * math.pi * k / segs + math.pi / segs for k in range(segs))]
    o, m, i = circ(r_out, cz), circ(r_mid, cz + 0.006), circ(r_in, cz + 0.010)
    tip = np.array([cx, cy, cz + 0.010 + dome])
    g = Geo()
    for k in range(segs):
        k1 = (k + 1) % segs
        g.tri([o[k], o[k1], m[k1]], None, bezel)
        g.tri([o[k], m[k1], m[k]], None, bezel)
        if ring is not None:
            g.tri([m[k], m[k1], i[k1]], None, ring)
            g.tri([m[k], i[k1], i[k]], None, ring)
            g.tri([i[k], i[k1], tip], None, lens)
        else:
            g.tri([m[k], m[k1], tip], None, lens)
    _orient(g, lambda c: (0.0, 0.0, 1.0))
    return smooth(g, 35.0)


def kestrel_lower(lod: int) -> Geo:
    outline = [(K_FRONT, 0.70), (K_FRONT, 1.175), (K_FRONT - 0.035, 1.21), (K_COWL, 1.245),
               (K_REAR, K_BELT), (K_REAR, 0.70)] + k_arch(-K_WZ, lod) + k_arch(K_WZ, lod)
    secs = [round_sections(-K_HW, K_HW, 0.06, 2), round_sections(-K_HW, K_HW, 0.06, 1),
            [(-K_HW, 0.0), (K_HW, 0.0)]][lod]
    g = extrude(outline, "x", secs, K_BODY, 50.0, edge_bevel=not_bottom(0.71))
    g.recolour(k_well(K_WZ, K_HW - 0.02))
    g.recolour(k_well(-K_WZ, K_HW - 0.02))
    return g.recolour(lambda c, n: INTERIOR if n[1] > 0.9 and K_REAR < c[2] < K_COWL and abs(c[0]) < K_HW - 0.03
                      else None)


def kestrel_cab(lod: int) -> Geo:
    y_sill = 1.33
    zA, zB, zC = K_SIDE_Z
    if lod == 2:
        rings = [[(-0.86, 1.23, K_COWL), (0.86, 1.23, K_COWL), (0.86, 1.23, -1.84), (-0.86, 1.23, -1.84)],
                 [(-0.79, K_ROOF, 0.33), (0.79, K_ROOF, 0.33), (0.79, K_ROOF, -1.78), (-0.79, K_ROOF, -1.78)]]
        g = loft(rings, K_TOP, 20.0, cap0=False, cap1=True).flat()
        g.recolour(lambda c, n: GLASS if n[2] > 0.4 else None)
        for sgn in (1.0, -1.0):
            g.extend(decal([(sgn * 0.855, 1.36, 0.70), (sgn * 0.855, 1.36, -1.50), (sgn * 0.80, 1.85, -1.50),
                            (sgn * 0.80, 1.85, 0.40)], (sgn, 0.1, 0.0), GLASS, 0.004))
        return g
    rings = [plan_ring(1.22, 0.865, K_COWL + 0.02, -1.845, 0.06, K_SIDE_Z, 0.52),
             plan_ring(y_sill, 0.86, 0.74, -1.84, 0.07, K_SIDE_Z, 0.52),
             plan_ring(1.86, 0.815, 0.37, -1.80, 0.09, K_SIDE_Z, 0.50),
             plan_ring(K_ROOF, 0.77, 0.31, -1.75, 0.11, K_SIDE_Z, 0.46)]
    if lod == 1:
        rings = [rings[0], rings[1], plan_ring(1.91, 0.80, 0.34, -1.78, 0.10, K_SIDE_Z, 0.48)]
    y_top = rings[2][0][1]

    def paint(c, n):
        if c[1] < y_sill - 0.005:
            return K_BODY
        if c[1] < y_top - 0.002 and n[2] > 0.4 and abs(n[0]) < 0.35:
            return GLASS
        if n[1] > 0.5 or c[1] > y_top - 0.002:
            return K_TOP
        if n[2] > 0.3:
            return BLACK                      # windshield frame
        if n[2] < -0.95 and abs(c[0]) < 0.50:
            return GLASS
        if abs(n[0]) > 0.95 and zC < c[2] < zB:
            return GLASS                      # rear quarter window
        if lod > 0 and abs(n[0]) > 0.95 and c[2] > zA:
            return GLASS
        return K_TOP

    def window(c, n):
        return lod == 0 and abs(n[0]) > 0.95 and c[1] > y_sill and c[2] > zA

    g = cab_glass(rings, paint, window)
    g.repaint_corners(lambda p, n, old: glass_at(p[1], y_sill, y_top) if old.slot == S_GLASS else old)
    if lod == 0:
        inner = [shrink_ring(rings[1], 0.03, -0.12), shrink_ring(rings[2], 0.03, -0.03)]
        g.extend(invert(loft(inner, INTERIOR, 20.0, cap0=False, cap1=True).flat()))
    return g


def kestrel_body(lod: int) -> Geo:
    G = Geo()
    hw = K_HW
    G.extend(kestrel_lower(lod))
    G.extend(kestrel_cab(lod))
    zf, zr = K_FRONT, K_REAR

    # ---- flares, rock rails
    if lod < 2:
        for cz in (K_WZ, -K_WZ):
            G.extend(mirror_pair(flare(k_arch(cz, lod), (cz, K_R), hw, 1.0, 0.11, 0.095, BLACK, lip=0.02)))
        if lod == 0:
            rail = sweep([(0.955, 0.645, 0.64), (0.955, 0.645, -0.64)],
                         [(-0.05, -0.04), (0.035, -0.04), (0.05, -0.015), (0.05, 0.03), (0.03, 0.045),
                          (-0.05, 0.045)], SATIN, 35.0, up=(0, 1, 0))
            G.extend(mirror_pair(rail))
        else:
            G.extend(mirror_pair(rbox((0.95, 0.645, 0.0), (0.10, 0.085, 1.28), SATIN, bottom=False)))
    elif lod == 2:
        for cz in (K_WZ, -K_WZ):
            top = K_R + K_ARCH[1]
            G.extend(mirror_pair(decal([(hw + 0.07, top - 0.03, cz - 0.62), (hw + 0.07, top - 0.03, cz + 0.62),
                                        (hw, top + 0.08, cz + 0.62), (hw, top + 0.08, cz - 0.62)],
                                       (0.85, 0.53, 0.0), BLACK, 0.0)))
            G.extend(mirror_pair(decal([(hw + 0.07, K_ROCK + 0.02, cz - 0.62), (hw + 0.07, K_ROCK + 0.02, cz + 0.62),
                                        (hw + 0.07, top - 0.03, cz + 0.62), (hw + 0.07, top - 0.03, cz - 0.62)],
                                       (1, 0, 0), BLACK, 0.0)))

    # ---- panel detail: door shut lines, hood shut line, handles, trail sights, mirrors
    if lod == 0:
        for sgn in (1.0, -1.0):
            for z in (K_COWL - 0.02, K_SIDE_Z[0] + 0.005):
                G.extend(quad_x(sgn * hw, z - 0.005, z + 0.005, 0.72, 1.19, SEAM, sgn, lift=0.004))
            G.extend(quad_x(sgn * hw, K_COWL - 0.02, zf - 0.07, 1.166, 1.176, SEAM, sgn, lift=0.004))
            G.extend(rbox((sgn * (hw + 0.012), 1.13, -0.30), (0.024, 0.03, 0.15), BLACK))
            G.extend(quad_x(sgn * hw, 1.73, 1.83, 0.86, 0.90, AMBER, sgn, lift=0.004))
            G.extend(quad_x(sgn * hw, 0.86, 1.06, 1.06, 1.10, SATIN, sgn, lift=0.004))
            sight = rbox((sgn * 0.80, 1.222, zf - 0.13), (0.07, 0.035, 0.16), BLACK)
            G.extend(drop(sight, lambda c, n: n[1] < -0.9))
        G.extend(decal([(-0.80, 1.248, K_COWL + 0.02), (0.80, 1.248, K_COWL + 0.02), (0.80, 1.244, K_COWL + 0.10),
                        (-0.80, 1.244, K_COWL + 0.10)], (0, 0.999, 0.035), BLACK, 0.003))
        for x0 in (-0.60, 0.02):
            G.extend(decal([(x0, 1.352, 0.737), (x0 + 0.52, 1.352, 0.737), (x0 + 0.52, 1.374, 0.722),
                            (x0, 1.374, 0.722)], (0, 0.55, 0.83), BLACK, 0.004))
        # removable hardtop panel seams above the front seats
        G.extend(decal([(-0.70, K_ROOF, -0.47), (0.70, K_ROOF, -0.47), (0.70, K_ROOF, -0.455),
                        (-0.70, K_ROOF, -0.455)], (0, 1, 0), SEAM, 0.003))
        G.extend(decal([(-0.006, K_ROOF, 0.27), (0.006, K_ROOF, 0.27), (0.006, K_ROOF, -0.46),
                        (-0.006, K_ROOF, -0.46)], (0, 1, 0), SEAM, 0.003))
        # fuel door
        G.extend(quad_x(-hw, -1.30 + 0.26, -1.30 + 0.40, 1.06, 1.18, fixed(0x24, 0x24, 0x27), -1.0, lift=0.003))
    if lod == 0:
        for sgn in (1.0, -1.0):
            head = rbox((sgn * 1.02, 1.43, 0.62), (0.12, 0.15, 0.06), BLACK)
            G.extend(head.recolour(lambda c, n: GLASS if n[2] < -0.9 else None))
            if lod == 0:
                G.extend(rbox((sgn * 0.925, 1.36, 0.64), (0.09, 0.06, 0.08), BLACK))

    # ---- front: grille band with round headlamps, bumper, winch
    if lod < 2:
        G.extend(rbox((0, 1.005, zf + 0.012), (1.70, 0.33, 0.028), K_TOP, bevel=0.012 if lod == 0 else 0.0))
        G.extend(quad_z(zf + 0.026, -0.50, 0.50, 0.885, 1.165, fixed(0x2C, 0x2D, 0x31), lift=0.002))
    else:
        G.extend(quad_z(zf, -0.85, 0.85, 0.84, 1.17, K_TOP))
    if lod == 0:
        for k, y in enumerate((0.935, 1.005, 1.075)):
            slat = rbox((0, y, zf + 0.032), (0.80, 0.03, 0.014), fixed(0x48, 0x4A, 0x50))
            G.extend(drop(slat, lambda c, n: n[2] < -0.9 or abs(n[0]) > 0.9))

        for c in K_HEAD_C:
            G.extend(round_lamp((c[0], c[1], zf + 0.026), 0.135, 0.112, 0.07, 10, SATIN,
                                Paint(rgb(0xFF, 0xFF, 0xF6), S_HEAD), HEAD_LENS))
    elif lod == 1:
        for c in K_HEAD_C:
            ring = [(c[0] + 0.12 * math.cos(a), c[1] + 0.12 * math.sin(a), zf + 0.026)
                    for a in (2 * math.pi * k / 6 for k in range(6))]
            G.extend(decal(ring, (0, 0, 1), HEAD_LENS, 0.004))
    else:
        for c in K_HEAD_C:
            G.extend(quad_z(zf + 0.002, c[0] - 0.10, c[0] + 0.10, c[1] - 0.10, c[1] + 0.10, HEAD_LENS))

    bump = [(-0.99, 0.74, zf - 0.04), (-0.86, 0.74, zf + 0.12), (0.86, 0.74, zf + 0.12), (0.99, 0.74, zf - 0.04)]
    if lod == 0:
        prof = [(-0.06, -0.10), (0.05, -0.10), (0.06, -0.08), (0.06, 0.07), (0.03, 0.10), (-0.06, 0.10)]
        G.extend(sweep(bump, prof, SATIN, 35.0))
        G.extend(lathe([(-0.22, 0.0), (-0.22, 0.075), (0.22, 0.075), (0.22, 0.0)], 8, STEEL, 30.0,
                       phase=math.pi / 8).xform(None, (0, 0.77, zf + 0.20)))
        G.extend(rbox((0, 0.77, zf + 0.25), (0.20, 0.09, 0.03), fixed(0x9A, 0x9E, 0xA4), bevel=0.01))
        G.extend(tube([(0, 0.75, zf + 0.26), (0, 0.72, zf + 0.30)], [0.012, 0.012], 3, RED_HOOK, cap1=RED_HOOK,
                      up=(1, 0, 0)).flat())
        for sgn in (1.0, -1.0):
            x = sgn * 0.62
            shackle = tube([(x, 0.70, zf + 0.15), (x, 0.66, zf + 0.21), (x, 0.72, zf + 0.21)], [0.018] * 3, 4,
                           RED_HOOK, cap1=RED_HOOK, up=(1, 0, 0), phase=math.pi / 4)
            G.extend(shackle.autosmooth(60.0))
        G.extend(rbox((0, 0.60, zf - 0.10), (1.10, 0.06, 0.40), SATIN, bottom=False))
    else:
        G.extend(rbox((0, 0.74, zf + 0.07), (1.94, 0.20, 0.12), SATIN, bottom=False))

    # ---- rear: taillights, gate, spare, bumper
    for sgn in (1.0, -1.0):
        tc = K_TAIL_C[0 if sgn < 0 else 1]
        if lod < 2:
            lens = rbox((tc[0], tc[1], zr - 0.012), (0.10, 0.36, 0.03), TAIL_LENS)
            lens.recolour(lambda c, n, s=sgn: None if n[2] < -0.9 or n[0] * s > 0.9 else BLACK)
            G.extend(drop(lens, lambda c, n: n[2] > 0.9))
            if lod == 0:
                G.extend(quad_z(zr - 0.027, tc[0] - 0.035, tc[0] + 0.035, 0.88, 0.93, REVERSE, side=-1.0,
                                lift=0.002))
        else:
            G.extend(quad_z(zr, tc[0] - 0.05, tc[0] + 0.05, 0.84, 1.20, TAIL_LENS, side=-1.0))
    if lod == 0:
        for x in (-0.74, 0.74):
            G.extend(quad_z(zr, x - 0.005, x + 0.005, 0.74, 1.23, SEAM, side=-1.0, lift=0.003))
        G.extend(quad_z(zr, -0.06, 0.06, 0.80, 0.83, SEAM, side=-1.0, lift=0.003))
    if lod == 0:
        spare = drop(wheel_lod0(K_SPARE), lambda c, n: n[0] < -0.95)
        G.extend(spare.xform(rot_y(math.pi / 2), K_SPARE_C))
    else:
        segs = (8, 6)[lod - 1]
        spare = lathe([(-0.15, K_R), (0.15, K_R * 0.92), (0.17, 0.0)], segs, RUBBER, 50.0,
                      face_paint=lambda i, j: K_RIM if i == 1 else None, phase=math.pi / segs)
        G.extend(spare.xform(rot_y(math.pi / 2), K_SPARE_C))
    rb = [(0.98, 0.70, zr + 0.02), (0.88, 0.70, zr - 0.08), (-0.88, 0.70, zr - 0.08), (-0.98, 0.70, zr + 0.02)]
    if lod == 0:
        rprof = [(-0.05, -0.08), (0.05, -0.08), (0.06, -0.06), (0.06, 0.08), (-0.05, 0.08)] if lod == 0 else \
            [(-0.05, -0.08), (0.06, -0.08), (0.06, 0.08), (-0.05, 0.08)]
        G.extend(sweep(rb, rprof, SATIN, 35.0))
    else:
        G.extend(rbox((0, 0.70, zr - 0.03), (1.90, 0.16, 0.10), SATIN, bottom=False))

    # ---- roof rack and light bar
    yr = K_ROOF + 0.05
    if lod < 2:
        loop = [(0.70, yr, 0.22), (0.70, yr, -1.62), (-0.70, yr, -1.62), (-0.70, yr, 0.22)]
        G.extend(tube(loop, [0.022] * 4, (4, 3)[lod], BLACK, closed_path=True, up=(0, 1, 0),
                      phase=math.pi / 4).autosmooth(50.0))
        for z in ((-0.40, -1.00) if lod == 0 else ()):
            G.extend(drop(rbox((0, yr, z), (1.40, 0.03, 0.05), BLACK), lambda c, n: abs(n[0]) > 0.9))
        if lod == 0:
            for x in (0.70, -0.70):
                G.extend(drop(rbox((x, K_ROOF - 0.01, -0.70), (0.035, 0.08, 1.70), BLACK),
                              lambda c, n: abs(n[1]) > 0.9))
        bar = rbox((0, K_BAR_Y, K_BAR_Z), (1.24, 0.10, 0.09), BLACK, bevel=0.02 if lod == 0 else 0.0)
        G.extend(drop(bar, lambda c, n: n[1] < -0.9 and lod > 0))
        for c in K_AUX_C:
            G.extend(quad_z(K_BAR_Z + 0.045, c[0] - 0.145, c[0] + 0.145, c[1] - 0.032, c[1] + 0.032, AUX_LENS,
                            lift=0.003))
    else:
        G.extend(rbox((0, K_BAR_Y, K_BAR_Z), (1.24, 0.10, 0.09), BLACK, bottom=False).recolour(
            lambda c, n: AUX_LENS if n[2] > 0.9 else None))

    # ---- underbody
    if lod == 0:
        for sgn in (1.0, -1.0):
            G.extend(rbox((sgn * 0.45, 0.60, -0.05), (0.09, 0.14, 3.6), UNDER))
        G.extend(rbox((0, 0.62, 0.0), (0.80, 0.12, 1.30), UNDER))
        G.extend(tube([(0.30, 0.56, 0.70), (0.35, 0.53, 0.0), (0.55, 0.54, -0.60), K_EXHAUST],
                      [0.035] * 4, 4, STEEL, cap1=SEAM, up=(0, 1, 0), phase=math.pi / 4).autosmooth(50.0))
    elif lod == 1:
        G.extend(rbox((0, 0.62, -0.05), (0.90, 0.12, 3.5), UNDER, bottom=False))

    # ---- driver and dash
    if lod == 0:
        G.extend(rbox((0, 1.30, 0.58), (1.60, 0.16, 0.30), INTERIOR, bottom=False))
        G.extend(driver(K_DRIVER[0], K_DRIVER[1], K_DRIVER[2], K_SHIRT, None, 0.30, 1.52))
    elif lod == 1:
        G.extend(ellipsoid((K_DRIVER[0], K_DRIVER[1] + 0.18, K_DRIVER[2]), (0.10, 0.12, 0.11), 4, [0, 90, 180],
                           SKIN, phase=math.pi / 4))
    return G


def kestrel_parts(lod: int) -> dict[int, Geo]:
    if lod == 0:
        w, a = wheel_lod0(K_WHEEL), axle_lod0(2 * K_WX, K_WX - 0.5 * K_TW)
    elif lod == 1:
        w, a = wheel_lod1(K_WHEEL), axle_lod1(2 * K_WX)
    else:
        w, a = wheel_lod2(K_WHEEL), None
    body = kestrel_body(lod)
    if lod == 0:
        shade_low(body, S_BODY, K_ROCK, 1.02, 0.82)
    parts = {BODY: body, WHEEL_R: w, WHEEL_L: w.mirrored_x()}
    if a is not None:
        parts[AXLE] = a
    return parts


KESTREL_INFO = {
    "name": "KESTREL",
    "wheel_pos": [(-K_WX, K_R, K_WZ), (K_WX, K_R, K_WZ), (-K_WX, K_R, -K_WZ), (K_WX, K_R, -K_WZ)],
    "wheel_radius": K_R, "wheel_width": K_TW,
    "headlight": K_HEAD_C, "headlight_r": 0.11,
    "aux": K_AUX_C,
    "taillight": K_TAIL_C,
    "exhaust": K_EXHAUST,
    "driver_eye": (K_DRIVER[0], K_DRIVER[1] + 0.20, K_DRIVER[2] + 0.10),
    "liveries": [
        ("EMBER ORANGE", K_BODY_RGB, K_ACCENT_RGB, K_TRIM_RGB, 0),
        ("VOLT BLUE", rgb(0x1C, 0x6C, 0xE8), rgb(0x26, 0x26, 0x2A), rgb(0xC8, 0xCC, 0xD2), 0),
        ("LIME ROCK", rgb(0x8E, 0xD0, 0x1E), rgb(0x26, 0x26, 0x2A), rgb(0x30, 0x30, 0x34), 0),
        ("SANDSTORM", rgb(0xD4, 0xB0, 0x78), rgb(0xF4, 0xF4, 0xF0), rgb(0x50, 0x54, 0x5A), 0),
    ],
}


# --------------------------------------------------------------------------
# BOBCAT: short two-door 4x4, top off and doors on, with a padded roll cage
# --------------------------------------------------------------------------

B_R, B_TW = 0.42, 0.30
B_WX, B_WZ = 0.80, 1.21
B_HW = 0.80              # tub half-width
B_ROCK = 0.60
B_BELT = 1.24            # door and tub top
B_FRONT = 1.80           # grille face
B_REAR = -1.78
B_COWL = 0.80            # hood's rear edge
B_DASH = 0.52            # dash face: the cockpit runs from here back
B_FLOOR = 0.80           # footwell floor
B_RISER = -0.52          # floor steps up to the rear seat and the load floor
B_CARGO = 1.00
B_WALL = 0.04            # door and tailgate skin
B_ARCH_TOP = 0.96
B_HOOD_HW, B_HOOD_Y = 0.66, 1.235
B_WS = ((0.70, B_BELT + 0.02), (0.37, 1.83))     # windshield frame base and top centreline (z, y)
B_WS_HW = (0.71, 0.69)
B_HOOP_Z, B_CAGE_Y = -0.38, 1.80
B_REAR_HOOP_Z = -1.42
B_CAGE_X = 0.70

B_BODY_RGB = rgb(0x6A, 0x68, 0x62)
B_FLARE_RGB = rgb(0x30, 0x30, 0x33)
B_RIM_RGB = rgb(0x46, 0x48, 0x4C)

B_BODY = Paint(B_BODY_RGB, S_BODY)
B_FLARE = Paint(B_FLARE_RGB, S_ACCENT)
B_RIM = Paint(B_RIM_RGB, S_TRIM)
B_SHIRT = fixed(0xC8, 0x4A, 0x26)
B_HAT = fixed(0x3A, 0x52, 0x3A)
PAD = fixed(0x22, 0x22, 0x24)            # cage padding
SEAT = fixed(0x3E, 0x3E, 0x42)

B_WHEEL = WheelStyle(B_R, B_TW, 8, 4, 0.046, 0.58, 0.64, 0.42, B_RIM, fixed(0x1A, 0x1A, 0x1C),
                     Paint(lerp_rgb(B_RIM_RGB, 0xFF000000, 0.4), S_TRIM), beadlock=False)
B_SPARE = WheelStyle(B_R, B_TW * 0.9, 8, 3, 0.044, 0.58, 0.64, 0.42, B_RIM, fixed(0x1A, 0x1A, 0x1C),
                     Paint(lerp_rgb(B_RIM_RGB, 0xFF000000, 0.4), S_TRIM), beadlock=False)

B_HEAD_C = [(-0.505, 1.02, B_FRONT + 0.03), (0.505, 1.02, B_FRONT + 0.03)]
B_TAIL_C = [(-0.735, 1.02, B_REAR - 0.02), (0.735, 1.02, B_REAR - 0.02)]
B_AUX_C = [(-0.80, 1.36, 0.70), (0.80, 1.36, 0.70)]     # lamps on the windshield hinges
B_EXHAUST = (0.48, 0.50, -1.84)
B_DRIVER = (-0.38, 1.44, -0.16)
B_SPARE_C = (0.0, 1.04, B_REAR - 0.22)


def b_arch(cz, lod):
    """Trapezoid wheel arch, rear foot to front foot, (z, y)."""
    t = B_ARCH_TOP
    pts = ([(-0.57, B_ROCK), (-0.52, 0.76), (-0.42, 0.89), (-0.30, t), (0.30, t), (0.42, 0.89), (0.52, 0.76),
            (0.57, B_ROCK)],
           [(-0.57, B_ROCK), (-0.44, 0.89), (-0.30, t), (0.30, t), (0.44, 0.89), (0.57, B_ROCK)],
           [(-0.57, B_ROCK), (-0.36, t), (0.36, t), (0.57, B_ROCK)])[lod]
    return [(cz + z, y) for z, y in pts]


def b_well(c, n):
    if n[1] < -0.7:
        return UNDER
    for cz in (B_WZ, -B_WZ):
        d = np.array([c[2] - cz, c[1] - B_R])
        if B_ROCK - 0.02 < c[1] < B_ARCH_TOP + 0.03 and abs(c[2] - cz) < 0.62 and abs(c[0]) < B_HW - 0.02 and \
                float(np.dot(n[[2, 1]], d)) < 0:
            return WELL
    return None


def bobcat_lower(lod: int) -> Geo:
    """Front clip and tub below the beltline, one extrusion across X: the
    fender shelf beside the hood, the cowl, then the open cockpit (dash face,
    footwell, rear seat riser, load floor) down to the sills."""
    cockpit = [(B_DASH, B_BELT), (B_DASH, B_FLOOR), (B_RISER, B_FLOOR), (B_RISER, B_CARGO), (B_REAR, B_CARGO)]
    if lod == 2:
        cockpit = [(B_REAR, B_BELT)]          # far away: a solid tub with a dark top
    nose = [(B_FRONT, 1.04), (0.88, 1.08), (0.80, B_BELT)] if lod < 2 else [(B_FRONT, B_HOOD_Y)]
    outline = [(B_FRONT, 0.72)] + nose + cockpit + \
        [(B_REAR, B_ROCK)] + b_arch(-B_WZ, lod) + b_arch(B_WZ, lod)
    g = extrude(outline, "x", [(-B_HW, 0.0), (B_HW, 0.0)], B_BODY, 40.0)
    g.recolour(b_well)

    def cockpit(c, n):
        if abs(c[0]) > B_HW - 0.01 or c[2] < B_REAR + 0.01:
            return None
        if c[2] < B_DASH + 0.01 and (n[1] > 0.9 or (abs(n[2]) > 0.9 and lod < 2)):
            return INTERIOR                       # floor, riser, load floor, dash face
        if n[1] > 0.9 and c[1] > B_BELT - 0.01 and c[2] < 0.80:
            return INTERIOR                       # dash top
        return None
    return g.recolour(cockpit)


def bobcat_tub_walls(lod: int) -> Geo:
    """Door and quarter skins round the cockpit, and the tailgate."""
    G = Geo()
    wall = [(B_DASH, B_FLOOR), (B_DASH, B_BELT), (B_REAR, B_BELT), (B_REAR, B_CARGO), (B_RISER, B_CARGO),
            (B_RISER, B_FLOOR)]
    w = extrude(wall, "x", [(B_HW - B_WALL, 0.0), (B_HW, 0.0)], B_BODY, 40.0)
    w.recolour(lambda c, n: INTERIOR if n[0] < -0.9 or (abs(n[2]) > 0.9 and c[1] < B_BELT - 0.05) else None)
    if lod == 2:
        w = drop(w, lambda c, n: n[0] < -0.9)
    G.extend(mirror_pair(w))
    gate = rbox((0, 0.5 * (B_CARGO + B_BELT), B_REAR + 0.5 * B_WALL), (2 * (B_HW - B_WALL), B_BELT - B_CARGO, B_WALL),
                B_BODY, bottom=False)
    G.extend(gate.recolour(lambda c, n: INTERIOR if n[2] > 0.9 else None))
    return G


def bobcat_hood(lod: int) -> Geo:
    zf = B_FRONT
    outline = [(zf, 1.00), (zf, B_HOOD_Y - 0.035), (zf - 0.035, B_HOOD_Y), (B_COWL, B_HOOD_Y + 0.008),
               (B_COWL, 1.00)]
    secs = [round_sections(-B_HOOD_HW, B_HOOD_HW, 0.03, 1), [(-B_HOOD_HW, 0.0), (B_HOOD_HW, 0.0)],
            [(-B_HOOD_HW, 0.0), (B_HOOD_HW, 0.0)]][lod]
    g = extrude(outline, "x", secs, B_BODY, 40.0, edge_bevel=not_bottom(1.01))
    return drop(g, lambda c, n: n[1] < -0.9)


def bobcat_windshield(lod: int) -> Geo:
    (zb, yb), (zt, yt) = B_WS
    hb, ht = B_WS_HW
    nrm = unit((0.0, zb - zt, yt - yb))          # faces forward and up
    if lod == 2:
        return decal([(-hb, yb, zb), (hb, yb, zb), (ht, yt, zt), (-ht, yt, zt)], nrm, GLASS, 0.0)
    G = Geo()
    frame = [(-hb, yb, zb), (-ht, yt, zt), (ht, yt, zt), (hb, yb, zb)]
    prof = [(-0.035, -0.028), (0.035, -0.028), (0.035, 0.028), (-0.035, 0.028)]
    G.extend(sweep(frame, prof, B_BODY, 40.0, up=nrm, caps=(True, True)))
    # the glass sits back in the frame
    up_in = unit(np.subtract((0.0, yt, zt), (0.0, yb, zb)))
    a, b = 0.035, 0.035

    def at(x, base):
        p = np.array([x, yb, zb]) if base else np.array([x, yt, zt])
        return p + up_in * (a if base else -b)
    glass = decal([at(-hb + 0.035, True), at(hb - 0.035, True), at(ht - 0.035, False), at(-ht + 0.035, False)],
                  nrm, GLASS, -0.006)
    glass.repaint_corners(lambda p, n, old: glass_at(p[1], yb, yt))
    G.extend(glass)
    if lod == 0:
        for x0 in (-0.52, 0.04):
            G.extend(decal([at(x0, True) + up_in * 0.02, at(x0 + 0.46, True) + up_in * 0.06,
                            at(x0 + 0.46, True) + up_in * 0.075, at(x0, True) + up_in * 0.035], nrm, BLACK, 0.0))
    return G


def bobcat_cage(lod: int) -> Geo:
    """Padded sport bar: a hoop behind the front seats, side rails from the
    windshield header back to a rear hoop, a bar across the back."""
    G = Geo()
    x, yc = B_CAGE_X, B_CAGE_Y
    if lod == 2:
        for sgn in (1.0, -1.0):
            rail = rbox((sgn * x, yc, 0.5 * (B_WS[1][0] + B_REAR_HOOP_Z)), (0.08, 0.08, B_WS[1][0] - B_REAR_HOOP_Z),
                        PAD, bottom=False)
            G.extend(drop(rail, lambda c, n, s=sgn: n[0] * s < -0.9 or abs(n[2]) > 0.9))
        G.extend(rbox((0, yc, B_HOOP_Z), (2 * x, 0.08, 0.08), PAD, bottom=False))
        return G
    sides, r = (4, 3)[lod], 0.042
    ph = math.pi / sides
    zt = B_WS[1][0] - 0.04
    hoop = [(-x - 0.02, B_BELT, B_HOOP_Z - 0.02), (-x - 0.02, yc - 0.12, B_HOOP_Z), (-x + 0.05, yc - 0.01, B_HOOP_Z),
            (x - 0.05, yc - 0.01, B_HOOP_Z), (x + 0.02, yc - 0.12, B_HOOP_Z), (x + 0.02, B_BELT, B_HOOP_Z - 0.02)]
    if lod == 1:
        hoop = [hoop[0], (-x, yc, B_HOOP_Z), (x, yc, B_HOOP_Z), hoop[-1]]
    G.extend(tube(hoop, [r] * len(hoop), sides, PAD, up=(0, 0, 1), phase=ph).autosmooth(55.0))
    for sgn in (1.0, -1.0):
        rail = [(sgn * x, B_WS[1][1] - 0.02, zt), (sgn * x, yc + 0.01, B_HOOP_Z), (sgn * x, yc - 0.02, B_REAR_HOOP_Z + 0.08),
                (sgn * x, yc - 0.10, B_REAR_HOOP_Z - 0.04), (sgn * x, B_BELT, B_REAR_HOOP_Z - 0.14)]
        if lod == 1:
            rail = [rail[0], rail[1], (sgn * x, yc - 0.03, B_REAR_HOOP_Z), rail[-1]]
        G.extend(tube(rail, [r] * len(rail), sides, PAD, cap0=PAD, up=(1, 0, 0), phase=ph).autosmooth(55.0))
        if lod == 0:
            # harness bar from the hoop down to the rear wheel tub
            G.extend(tube([(sgn * (x - 0.04), yc - 0.08, B_HOOP_Z - 0.04), (sgn * (x - 0.02), B_CARGO + 0.24, -1.05)],
                          [0.032, 0.032], 4, PAD, cap1=PAD, up=(1, 0, 0), phase=math.pi / 4).autosmooth(55.0))
    rear = [(-x, yc - 0.03, B_REAR_HOOP_Z), (x, yc - 0.03, B_REAR_HOOP_Z)]
    G.extend(tube(rear, [r * 0.9] * 2, sides, PAD, up=(0, 0, 1), phase=ph).autosmooth(55.0))
    return G


def bobcat_seats(lod: int) -> Geo:
    G = Geo()
    if lod == 2:
        return G
    for x in (-0.38, 0.38):
        base = (x, B_FLOOR + 0.24, -0.20)
        if lod == 0:
            G.extend(box(base, (0.48, 0.13, 0.50), SEAT, bottom=False))
            G.extend(box((x, B_FLOOR + 0.62, -0.47), (0.48, 0.62, 0.12), SEAT, frame=rot_x(math.radians(-12))))
            G.extend(box((x, B_FLOOR + 1.00, -0.53), (0.26, 0.17, 0.10), SEAT, frame=rot_x(math.radians(-12))))
        else:
            G.extend(rbox((x, B_FLOOR + 0.55, -0.40), (0.46, 0.70, 0.30), SEAT, bottom=False))
    if lod == 0:
        G.extend(box((0, B_CARGO + 0.10, -0.86), (1.24, 0.12, 0.44), SEAT, bottom=False))
        G.extend(box((0, B_CARGO + 0.40, -1.10), (1.24, 0.52, 0.12), SEAT, frame=rot_x(math.radians(-10))))
    else:
        G.extend(rbox((0, B_CARGO + 0.28, -1.00), (1.24, 0.56, 0.30), SEAT, bottom=False))
    return G


def bobcat_body(lod: int) -> Geo:
    G = Geo()
    hw = B_HW
    zf, zr = B_FRONT, B_REAR
    G.extend(bobcat_lower(lod))
    if lod < 2:
        G.extend(bobcat_tub_walls(lod))
    if lod < 2:
        G.extend(bobcat_hood(lod))
    G.extend(bobcat_windshield(lod))
    G.extend(bobcat_cage(lod))
    G.extend(bobcat_seats(lod))

    # ---- flares (wide and flat, square-cut), rock rails
    if lod < 2:
        for cz in (B_WZ, -B_WZ):
            G.extend(mirror_pair(flare(b_arch(cz, 2 * lod), (cz, B_R), hw, 1.0, 0.13, 0.10, B_FLARE, lip=0.02,
                                       angle=30.0, wedge=lod > 0)))
        if lod == 0:
            rail = sweep([(0.85, 0.60, 0.60), (0.85, 0.60, -0.60)],
                         [(-0.05, -0.035), (0.03, -0.035), (0.045, -0.015), (0.045, 0.03), (0.025, 0.04),
                          (-0.05, 0.04)], SATIN, 35.0, up=(0, 1, 0))
            G.extend(mirror_pair(rail))
        else:
            G.extend(mirror_pair(rbox((0.84, 0.60, 0.0), (0.09, 0.075, 1.20), SATIN, bottom=False)))
    else:
        for cz in (B_WZ, -B_WZ):
            top = B_ARCH_TOP
            G.extend(mirror_pair(decal([(hw + 0.09, top - 0.02, cz - 0.60), (hw + 0.09, top - 0.02, cz + 0.60),
                                        (hw, top + 0.09, cz + 0.60), (hw, top + 0.09, cz - 0.60)],
                                       (0.77, 0.64, 0.0), B_FLARE, 0.0)))
            G.extend(mirror_pair(decal([(hw + 0.09, B_ROCK + 0.04, cz - 0.60), (hw + 0.09, B_ROCK + 0.04, cz + 0.60),
                                        (hw + 0.09, top - 0.02, cz + 0.60), (hw + 0.09, top - 0.02, cz - 0.60)],
                                       (1, 0, 0), B_FLARE, 0.0)))

    # ---- grille: body-colour panel, vertical slots, round lamps outboard
    if lod < 2:
        G.extend(rbox((0, 0.98, zf + 0.015), (2 * B_HOOD_HW - 0.02, 0.44, 0.03), B_BODY,
                      bevel=0.012 if lod == 0 else 0.0, bottom=False))
    else:
        G.extend(quad_z(zf, -0.64, 0.64, 0.76, 1.20, B_BODY))
    gz = zf + 0.03
    if lod == 0:
        for k in range(-4, 5):
            x = 0.074 * k
            G.extend(quad_z(gz, x - 0.021, x + 0.021, 0.81, 1.17, fixed(0x1C, 0x1C, 0x1E), lift=0.002))
        for c in B_HEAD_C:
            G.extend(round_lamp((c[0], c[1], gz), 0.118, 0.098, 0.062, 8, BLACK,
                                Paint(rgb(0xFF, 0xFF, 0xF6), S_HEAD), HEAD_LENS))
            ring = [(c[0] + 0.035 * math.cos(a), 0.86 + 0.035 * math.sin(a), gz)
                    for a in (2 * math.pi * k / 6 for k in range(6))]
            G.extend(decal(ring, (0, 0, 1), AMBER, 0.003))
    elif lod == 1:
        G.extend(quad_z(gz, -0.34, 0.34, 0.81, 1.17, fixed(0x1C, 0x1C, 0x1E), lift=0.002))
        for c in B_HEAD_C:
            ring = [(c[0] + 0.11 * math.cos(a), c[1] + 0.11 * math.sin(a), gz)
                    for a in (2 * math.pi * k / 6 for k in range(6))]
            G.extend(decal(ring, (0, 0, 1), HEAD_LENS, 0.004))
    else:
        G.extend(quad_z(zf + 0.002, -0.32, 0.32, 0.82, 1.16, BLACK))
        for c in B_HEAD_C:
            G.extend(quad_z(zf + 0.004, c[0] - 0.09, c[0] + 0.09, c[1] - 0.09, c[1] + 0.09, HEAD_LENS))

    # ---- front bumper, hooks
    bump = [(-0.90, 0.70, zf - 0.02), (-0.82, 0.70, zf + 0.10), (0.82, 0.70, zf + 0.10), (0.90, 0.70, zf - 0.02)]
    if lod == 0:
        prof = [(-0.05, -0.09), (0.04, -0.09), (0.05, -0.07), (0.05, 0.07), (0.03, 0.09), (-0.05, 0.09)]
        G.extend(sweep(bump, prof, SATIN, 35.0))
        for sgn in (1.0, -1.0):
            fog = [(sgn * 0.46 + 0.04 * math.cos(a), 0.70 + 0.04 * math.sin(a), zf + 0.152)
                   for a in (2 * math.pi * k / 6 for k in range(6))]
            G.extend(decal(fog, (0, 0, 1), fixed(0xD8, 0xDA, 0xD4), 0.002))
            hook = tube([(sgn * 0.62, 0.68, zf + 0.10), (sgn * 0.62, 0.62, zf + 0.20), (sgn * 0.62, 0.70, zf + 0.23)],
                        [0.02] * 3, 4, RED_HOOK, cap1=RED_HOOK, up=(1, 0, 0), phase=math.pi / 4)
            G.extend(hook.autosmooth(60.0))
        G.extend(rbox((0, 0.58, zf - 0.12), (1.00, 0.05, 0.36), SATIN, bottom=False))
    else:
        G.extend(rbox((0, 0.70, zf + 0.05), (1.80, 0.18, 0.10), SATIN, bottom=False))

    # ---- rear: taillights, spare on the tailgate, bumper
    for sgn in (1.0, -1.0):
        tc = B_TAIL_C[0 if sgn < 0 else 1]
        if lod < 2:
            lens = rbox((tc[0], tc[1], zr - 0.012), (0.10, 0.30, 0.03), TAIL_LENS)
            lens.recolour(lambda c, n, s=sgn: None if n[2] < -0.9 or n[0] * s > 0.9 else BLACK)
            G.extend(drop(lens, lambda c, n: n[2] > 0.9))
            if lod == 0:
                G.extend(quad_z(zr - 0.027, tc[0] - 0.035, tc[0] + 0.035, 0.895, 0.935, REVERSE, side=-1.0,
                                lift=0.002))
                G.extend(quad_z(zr - 0.027, tc[0] - 0.035, tc[0] + 0.035, 1.105, 1.15, AMBER, side=-1.0,
                                lift=0.002))
        else:
            G.extend(quad_z(zr, tc[0] - 0.05, tc[0] + 0.05, 0.87, 1.17, TAIL_LENS, side=-1.0))
    if lod == 0:
        for x in (-0.66, 0.66):
            G.extend(quad_z(zr, x - 0.005, x + 0.005, 0.70, 1.23, SEAM, side=-1.0, lift=0.003))
        spare = drop(wheel_lod0(B_SPARE), lambda c, n: n[0] < -0.95)
        G.extend(spare.xform(rot_y(math.pi / 2), B_SPARE_C))
        G.extend(rbox((0, B_SPARE_C[1], zr - 0.04), (0.18, 0.18, 0.08), BLACK))
    else:
        segs = 6
        spare = lathe([(-0.14, B_R), (0.14, B_R * 0.92), (0.16, 0.0)], segs, RUBBER, 50.0,
                      face_paint=lambda i, j: B_RIM if i == 1 else None, phase=math.pi / segs)
        G.extend(spare.xform(rot_y(math.pi / 2), B_SPARE_C))
    rb = [(0.92, 0.66, zr + 0.02), (0.84, 0.66, zr - 0.08), (-0.84, 0.66, zr - 0.08), (-0.92, 0.66, zr + 0.02)]
    if lod == 0:
        G.extend(sweep(rb, [(-0.05, -0.07), (0.05, -0.07), (0.06, -0.05), (0.06, 0.07), (-0.05, 0.07)], SATIN, 35.0))
    else:
        G.extend(rbox((0, 0.66, zr - 0.03), (1.80, 0.14, 0.10), SATIN, bottom=False))

    # ---- doors: shut lines, exposed hinges, handles; mirrors on the hinges; hood latches; lamps on the
    # windshield hinges; antenna
    if lod == 0:
        for sgn in (1.0, -1.0):
            for z in (0.74, -0.40):
                G.extend(quad_x(sgn * hw, z - 0.005, z + 0.005, 0.64, B_BELT - 0.01, SEAM, sgn, lift=0.004))
            for y in (0.84, 1.13):
                G.extend(quad_x(sgn * hw, 0.68, 0.75, y - 0.04, y + 0.04, BLACK, sgn, lift=0.006))
            G.extend(rbox((sgn * (hw + 0.012), 1.13, -0.30), (0.024, 0.03, 0.13), BLACK))
            head = rbox((sgn * 0.98, 1.36, 0.66), (0.09, 0.15, 0.05), BLACK)
            G.extend(head.recolour(lambda c, n: GLASS if n[2] < -0.9 else None))
            G.extend(tube([(sgn * (hw + 0.02), 1.16, 0.70), (sgn * 0.93, 1.30, 0.67)], [0.014, 0.014], 4, BLACK,
                          up=(0, 0, 1), phase=math.pi / 4).flat())
            G.extend(rbox((sgn * (B_HOOD_HW + 0.012), 1.17, 1.62), (0.024, 0.07, 0.05), BLACK))
            G.extend(quad_x(sgn * hw, 1.62, 1.72, 0.92, 0.95, AMBER, sgn, lift=0.004))
        for c in B_AUX_C:
            G.extend(lens_light(c, (0, 0, 1), 0.065, 0.08, 6, AUX_LENS, BLACK))
            G.extend(rbox((c[0], c[1] - 0.08, c[2] - 0.03), (0.03, 0.10, 0.03), BLACK))
        G.extend(tube([(0.80, 1.06, 1.20), (0.82, 2.02, 1.12)], [0.006, 0.003], 3, BLACK, phase=0.0).flat())
        G.extend(quad_x(-hw, -1.30, -1.18, 1.06, 1.18, fixed(0x24, 0x24, 0x27), -1.0, lift=0.003))
    elif lod == 1:
        for c in B_AUX_C:
            G.extend(quad_z(c[2], c[0] - 0.06, c[0] + 0.06, c[1] - 0.06, c[1] + 0.06, AUX_LENS))

    # ---- underbody
    if lod == 0:
        for sgn in (1.0, -1.0):
            G.extend(rbox((sgn * 0.42, 0.54, -0.02), (0.08, 0.13, 3.40), UNDER))
        G.extend(rbox((0, 0.55, 0.05), (0.72, 0.10, 1.10), UNDER))
        G.extend(tube([(0.28, 0.50, 0.65), (0.32, 0.48, -0.10), (0.48, 0.49, -0.75), B_EXHAUST],
                      [0.032] * 4, 4, STEEL, cap1=SEAM, up=(0, 1, 0), phase=math.pi / 4).autosmooth(50.0))
    elif lod == 1:
        G.extend(rbox((0, 0.55, -0.05), (0.84, 0.10, 3.30), UNDER, bottom=False))

    # ---- driver, steering wheel
    if lod == 0:
        G.extend(driver(B_DRIVER[0], B_DRIVER[1], B_DRIVER[2], B_SHIRT, B_HAT, 0.26, 1.42))
    elif lod == 1:
        G.extend(ellipsoid((B_DRIVER[0], B_DRIVER[1] + 0.18, B_DRIVER[2]), (0.10, 0.12, 0.11), 4, [0, 90, 180],
                           SKIN, phase=math.pi / 4))
    return G


def bobcat_parts(lod: int) -> dict[int, Geo]:
    if lod == 0:
        w, a = wheel_lod0(B_WHEEL), axle_lod0(2 * B_WX, B_WX - 0.5 * B_TW)
    elif lod == 1:
        w, a = wheel_lod1(B_WHEEL), axle_lod1(2 * B_WX)
    else:
        w, a = wheel_lod2(B_WHEEL), None
    body = bobcat_body(lod)
    if lod == 0:
        shade_low(body, S_BODY, B_ROCK, 1.02, 0.82)
    parts = {BODY: body, WHEEL_R: w, WHEEL_L: w.mirrored_x()}
    if a is not None:
        parts[AXLE] = a
    return parts


BOBCAT_INFO = {
    "name": "BOBCAT",
    "wheel_pos": [(-B_WX, B_R, B_WZ), (B_WX, B_R, B_WZ), (-B_WX, B_R, -B_WZ), (B_WX, B_R, -B_WZ)],
    "wheel_radius": B_R, "wheel_width": B_TW,
    "headlight": B_HEAD_C, "headlight_r": 0.10,
    "aux": B_AUX_C,
    "taillight": B_TAIL_C,
    "exhaust": B_EXHAUST,
    "driver_eye": (B_DRIVER[0], B_DRIVER[1] + 0.20, B_DRIVER[2] + 0.10),
    "liveries": [
        ("GRANITE", B_BODY_RGB, B_FLARE_RGB, B_RIM_RGB, 0),
        ("BRICK RED", rgb(0xA8, 0x24, 0x1E), B_FLARE_RGB, rgb(0xB8, 0xBC, 0xC2), 0),
        ("OLIVE DRAB", rgb(0x5C, 0x64, 0x3C), rgb(0x5C, 0x64, 0x3C), rgb(0x26, 0x26, 0x28), 0),
        ("GOLDENROD", rgb(0xF0, 0xB4, 0x1C), B_FLARE_RGB, rgb(0x26, 0x26, 0x28), 0),
    ],
}


# --------------------------------------------------------------------------
# FFR: mid-90s midsize extended-cab pickup with a long bed
# --------------------------------------------------------------------------

D_R, D_TW = 0.40, 0.27
D_WX, D_WZ = 0.735, 1.59
D_HW = 0.84
D_ROCK = 0.60
D_BELT = 1.24             # cab beltline
D_BED_TOP = 1.22
D_FRONT = 2.34            # grille face
D_COWL = 1.04             # hood's rear edge, windshield base
D_CABB = -0.62
D_BEDF, D_BEDB = -0.70, -2.56
D_ARCH = (0.53, 0.50)     # wheel arch half-length, height above the wheel centre
D_SIDE_Z = [-0.02, -0.10, -0.46]   # door window end, B pillar end, quarter window end
D_ROOF = 1.80
D_LAMP_Y = (0.86, 0.975)  # headlamp band

D_BODY_RGB = rgb(0xC8, 0x1A, 0x1E)
D_STRIPE_RGB = rgb(0xB4, 0xB8, 0xC0)
D_RIM_RGB = rgb(0xE6, 0xE6, 0xE2)

D_BODY = Paint(D_BODY_RGB, S_BODY)
D_STRIPE = Paint(D_STRIPE_RGB, S_ACCENT)
D_RIM = Paint(D_RIM_RGB, S_TRIM)
D_SHIRT = fixed(0x6A, 0x2C, 0x30)
D_HAT = fixed(0xC4, 0xB0, 0x86)

D_WHEEL = WheelStyle(D_R, D_TW, 10, 3, 0.036, 0.62, 0.64, 0.42, D_RIM, fixed(0x30, 0x30, 0x34),
                     Paint(lerp_rgb(D_RIM_RGB, 0xFF000000, 0.30), S_TRIM), beadlock=False)

D_HEAD_C = [(-0.56, 0.918, D_FRONT + 0.01), (0.56, 0.918, D_FRONT + 0.01)]
D_TAIL_C = [(-0.765, 0.98, D_BEDB - 0.03), (0.765, 0.98, D_BEDB - 0.03)]
D_AUX_C = [(-0.52, 0.555, D_FRONT + 0.035), (0.52, 0.555, D_FRONT + 0.035)]   # fog lamps under the bumper
D_EXHAUST = (0.62, 0.47, -2.30)
D_DRIVER = (-0.38, 1.40, 0.18)


def d_arch(z, lod):
    return arch_points(z, D_R, D_ARCH[0], D_ARCH[1], D_ROCK, n=(5, 3, 2)[lod])


def d_well(cz, hw):
    def paint(c, n):
        if n[1] < -0.7:
            return UNDER
        d = np.array([c[2] - cz, c[1] - D_R])
        if c[1] < D_R + D_ARCH[1] + 0.03 and abs(c[2] - cz) < D_ARCH[0] + 0.08 and \
                float(np.dot(n[[2, 1]], d)) < 0 and abs(c[0]) < hw:
            return WELL
        return None
    return paint


def d_hood_y(z):
    """Hood top along the centre line, falling gently from the cowl to the nose."""
    return 1.19 + (z - D_COWL) * (1.09 - 1.19) / (D_FRONT - 0.22 - D_COWL)


def ffr_lower(lod: int) -> Geo:
    """Front clip and cab below the beltline, one rounded extrusion across X."""
    outline = [(D_FRONT, 0.64), (D_FRONT, 0.99), (D_FRONT - 0.07, 1.06), (D_FRONT - 0.22, 1.09),
               (D_COWL, 1.19), (D_CABB, D_BELT), (D_CABB, D_ROCK)] + d_arch(D_WZ, lod)
    secs = [round_sections(-D_HW, D_HW, 0.06, 2), round_sections(-D_HW, D_HW, 0.06, 1),
            [(-D_HW, 0.0), (D_HW, 0.0)]][lod]
    g = extrude(outline, "x", secs, D_BODY, 50.0, edge_bevel=not_bottom(0.65))
    g.recolour(d_well(D_WZ, D_HW - 0.02))
    return g.recolour(lambda c, n: INTERIOR if n[1] > 0.9 and D_CABB < c[2] < D_COWL and abs(c[0]) < D_HW - 0.03
                      else None)


def ffr_cab(lod: int) -> Geo:
    y_sill = 1.30
    zA, zB, zC = D_SIDE_Z
    if lod == 2:
        rings = [[(-0.80, 1.18, D_COWL), (0.80, 1.18, D_COWL), (0.80, 1.18, D_CABB), (-0.80, 1.18, D_CABB)],
                 [(-0.70, D_ROOF, 0.36), (0.70, D_ROOF, 0.36), (0.70, D_ROOF, -0.56), (-0.70, D_ROOF, -0.56)]]
        g = loft(rings, D_BODY, 20.0, cap0=False, cap1=True).flat()
        g.recolour(lambda c, n: GLASS if n[2] > 0.4 else None)
        for sgn in (1.0, -1.0):
            g.extend(decal([(sgn * 0.79, 1.32, 0.92), (sgn * 0.79, 1.32, -0.44), (sgn * 0.72, 1.74, -0.44),
                            (sgn * 0.72, 1.74, 0.40)], (sgn, 0.12, 0.0), GLASS, 0.004))
        return g
    rings = [plan_ring(1.18, 0.81, D_COWL, D_CABB, 0.06, D_SIDE_Z, 0.50),
             plan_ring(y_sill, 0.80, 0.96, D_CABB + 0.002, 0.07, D_SIDE_Z, 0.50),
             plan_ring(1.74, 0.74, 0.40, D_CABB + 0.03, 0.10, D_SIDE_Z, 0.46),
             plan_ring(D_ROOF, 0.69, 0.32, D_CABB + 0.07, 0.12, D_SIDE_Z, 0.42)]
    if lod == 1:
        rings = [rings[0], rings[1], plan_ring(1.79, 0.72, 0.35, D_CABB + 0.05, 0.11, D_SIDE_Z, 0.44)]
    y_top = rings[2][0][1]

    def paint(c, n):
        if c[1] < y_sill - 0.005:
            return D_BODY
        if c[1] < y_top - 0.002 and n[2] > 0.4 and abs(n[0]) < 0.35:
            return GLASS
        if n[1] > 0.5 or c[1] > y_top - 0.002:
            return D_BODY
        if n[2] > 0.3:
            return BLACK                      # windshield frame
        if n[2] < -0.95 and abs(c[0]) < 0.46:
            return GLASS
        if abs(n[0]) > 0.95 and zC < c[2] < zB:
            return GLASS                      # extended cab's small quarter window
        if lod > 0 and abs(n[0]) > 0.95 and c[2] > zA:
            return GLASS
        if abs(n[0]) > 0.95 and zB <= c[2] <= zA:
            return BLACK                      # B pillar
        return D_BODY

    def window(c, n):
        return lod == 0 and abs(n[0]) > 0.95 and c[1] > y_sill and c[2] > zA

    g = cab_glass(rings, paint, window)
    g.repaint_corners(lambda p, n, old: glass_at(p[1], y_sill, y_top) if old.slot == S_GLASS else old)
    if lod == 0:
        inner = [shrink_ring(rings[1], 0.03, -0.12), shrink_ring(rings[2], 0.03, -0.03)]
        g.extend(invert(loft(inner, INTERIOR, 20.0, cap0=False, cap1=True).flat()))
    return g


def ffr_bed(lod: int) -> Geo:
    G = Geo()
    hw = D_HW
    xi = hw - 0.05                       # bed wall inner face
    if lod == 2:
        bed = rbox((0, 0.91, (D_BEDF + D_BEDB) / 2), (2 * hw, 0.62, abs(D_BEDB - D_BEDF)), D_BODY, bottom=False)
        bed.recolour(lambda c, n: BLACK if n[1] > 0.9 else None)
        return G.extend(bed)
    wall = [(D_BEDF, D_BED_TOP), (D_BEDB, D_BED_TOP), (D_BEDB, D_ROCK)] + d_arch(-D_WZ, lod) + [(D_BEDF, D_ROCK)]
    secs = [(xi, 0.0)] + round_sections(xi, hw, 0.04, 2)[3:] if lod == 0 else [(xi, 0.0), (hw, 0.0)]
    w = extrude(wall, "x", secs, D_BODY, 50.0, edge_bevel=not_bottom(0.65))
    w.recolour(d_well(-D_WZ, hw - 0.01))
    G.extend(mirror_pair(w))
    G.extend(rbox((0, 0.93, D_BEDF - 0.025), (2 * xi, 0.58, 0.05), D_BODY, bottom=False))
    floor = rbox((0, 0.83, (D_BEDF + D_BEDB) / 2), (2 * xi, 0.05, abs(D_BEDB - D_BEDF) - 0.02), BLACK)
    G.extend(drop(floor, lambda c, n: abs(n[0]) > 0.9 or abs(n[2]) > 0.9))
    post = drop(rbox((xi - 0.055, 0.93, D_BEDB + 0.05), (0.11, 0.58, 0.10), D_BODY, bottom=False),
                lambda c, n: n[0] > 0.9)
    G.extend(mirror_pair(post))
    gate_hw = xi - 0.11
    G.extend(rbox((0, 0.94, D_BEDB + 0.025), (2 * gate_hw, 0.54, 0.05), D_BODY, bevel=0.02 if lod == 0 else 0.0))
    if lod == 0:
        # ribbed liner, wheel tubs, gate handle
        for k in range(4):
            x = -0.48 + 0.32 * k
            G.extend(decal([(x - 0.04, 0.855, D_BEDF - 0.06), (x + 0.04, 0.855, D_BEDF - 0.06),
                            (x + 0.04, 0.855, D_BEDB + 0.08), (x - 0.04, 0.855, D_BEDB + 0.08)],
                           (0, 1, 0), fixed(0x34, 0x35, 0x39), 0.002))
        tub = drop(rbox((xi - 0.125, 0.93, -D_WZ), (0.25, 0.17, 1.00), D_BODY), lambda c, n: n[0] > 0.9)
        G.extend(mirror_pair(tub.recolour(lambda c, n: WELL if n[1] < -0.9 else None)))
        G.extend(quad_z(D_BEDB, -0.12, 0.12, 1.13, 1.16, BLACK, side=-1.0))
    return G


def ffr_body(lod: int) -> Geo:
    G = Geo()
    hw = D_HW
    zf, zr = D_FRONT, D_BEDB
    G.extend(ffr_lower(lod))
    G.extend(ffr_cab(lod))
    G.extend(ffr_bed(lod))

    # ---- pinstripes down the flank, broken at the cab-to-bed gap
    bands = [(0.955, 0.978), (0.934, 0.941)] if lod == 0 else [(0.934, 0.978)] if lod == 1 else []
    for y0, y1 in bands:
        for sgn in (1.0, -1.0):
            G.extend(quad_x(sgn * hw, D_CABB + 0.03, zf - 0.10, y0, y1, D_STRIPE, sgn))
            G.extend(quad_x(sgn * hw, zr + 0.06, D_BEDF - 0.03, y0, y1, D_STRIPE, sgn))

    # ---- panel detail: door, hood, handles, markers, cowl, wipers, fuel door
    if lod == 0:
        for sgn in (1.0, -1.0):
            for z in (D_COWL - 0.04, D_SIDE_Z[0] + 0.005):
                G.extend(quad_x(sgn * hw, z - 0.005, z + 0.005, 0.66, 1.20, SEAM, sgn, lift=0.004))
            G.extend(rbox((sgn * (hw + 0.012), 1.13, 0.10), (0.024, 0.028, 0.14), BLACK))
            G.extend(quad_x(sgn * hw, zr + 0.06, zr + 0.14, 0.86, 0.90, fixed(0xC8, 0x14, 0x14), sgn, lift=0.004))
            x = sgn * 0.72
            za, zb = D_COWL + 0.05, zf - 0.24
            G.extend(decal([(x - 0.006, d_hood_y(za), za), (x + 0.006, d_hood_y(za), za),
                            (x + 0.006, d_hood_y(zb), zb), (x - 0.006, d_hood_y(zb), zb)],
                           (0, 0.996, 0.092), SEAM, 0.004))
        G.extend(decal([(-0.70, 1.19, D_COWL), (0.70, 1.19, D_COWL), (0.70, d_hood_y(D_COWL + 0.08), D_COWL + 0.08),
                        (-0.70, d_hood_y(D_COWL + 0.08), D_COWL + 0.08)], (0, 0.996, 0.092), BLACK, 0.003))
        for x0 in (-0.52, 0.04):
            G.extend(decal([(x0, 1.325, 0.928), (x0 + 0.44, 1.325, 0.928), (x0 + 0.44, 1.345, 0.903),
                            (x0, 1.345, 0.903)], (0, 0.786, 0.618), BLACK, 0.004))
        G.extend(quad_x(-hw, -1.02, -0.88, 1.02, 1.13, fixed(0x24, 0x24, 0x27), -1.0, lift=0.003))
        for sgn in (1.0, -1.0):
            head = rbox((sgn * 0.94, 1.40, 0.86), (0.10, 0.15, 0.05), BLACK)
            G.extend(head.recolour(lambda c, n: GLASS if n[2] < -0.9 else None))
            G.extend(tube([(sgn * 0.80, 1.31, 0.90), (sgn * 0.90, 1.37, 0.87)], [0.014, 0.014], 4, BLACK,
                          up=(0, 0, 1), phase=math.pi / 4).flat())
        G.extend(tube([(0.80, 1.06, 1.86), (0.84, 1.98, 1.80)], [0.006, 0.003], 3, BLACK, phase=0.0).flat())

    # ---- black flares
    if lod < 2:
        for cz in (D_WZ, -D_WZ):
            G.extend(mirror_pair(flare(d_arch(cz, lod), (cz, D_R), hw, 1.0, 0.06, 0.07, BLACK, wedge=lod > 0)))

    # ---- front: grille with a chrome surround, wraparound lamps, chrome bumper, air dam and fog lamps
    ly0, ly1 = D_LAMP_Y
    if lod == 0:
        G.extend(chrome_corners(rbox((0, 0.918, zf + 0.010), (0.84, 0.15, 0.022), BLACK)))
        G.extend(quad_z(zf + 0.0215, -0.39, 0.39, 0.862, 0.974, fixed(0x1C, 0x1C, 0x1F), lift=0.002))
        for y in (0.892, 0.944):
            slat = rbox((0, y, zf + 0.028), (0.76, 0.012, 0.012), BLACK)
            G.extend(chrome_corners(drop(slat, lambda c, n: n[2] < -0.9 or abs(n[0]) > 0.9)))
        for sgn in (1.0, -1.0):
            x0, x1 = sorted((sgn * 0.43, sgn * 0.69))
            G.extend(quad_z(zf, x0, x1, ly0, ly1, HEAD_LENS, lift=0.004))
            x0, x1 = sorted((sgn * 0.69, sgn * 0.78))
            G.extend(quad_z(zf, x0, x1, ly0, ly1, AMBER, lift=0.004))
            G.extend(quad_x(sgn * hw, zf - 0.16, zf - 0.06, ly0, ly1, AMBER, sgn, lift=0.004))
    else:
        G.extend(quad_z(zf, -0.40, 0.40, 0.86, 0.976, BLACK))
        for sgn in (1.0, -1.0):
            x0, x1 = sorted((sgn * 0.42, sgn * 0.78))
            G.extend(quad_z(zf, x0, x1, ly0, ly1, HEAD_LENS))
    if lod < 2:
        bump = [(-0.88, 0.70, zf - 0.04), (-0.82, 0.70, zf + 0.09), (0.82, 0.70, zf + 0.09), (0.88, 0.70, zf - 0.04)]
        prof = [(-0.05, -0.075), (0.04, -0.075), (0.05, -0.05), (0.05, 0.05), (0.04, 0.075), (-0.05, 0.075)] \
            if lod == 0 else [(-0.05, -0.075), (0.05, -0.075), (0.05, 0.075), (-0.05, 0.075)]
        G.extend(chrome_corners(sweep(bump, prof, BLACK, 40.0)))
        dam = rbox((0, 0.555, zf - 0.01), (1.48, 0.13, 0.10), BLACK, bottom=False)
        G.extend(drop(dam, lambda c, n: n[2] < -0.9))
        for c in D_AUX_C:
            if lod == 0:
                G.extend(lens_light((c[0], c[1], zf + 0.035), (0, 0, 1), 0.052, 0.05, 8, AUX_LENS, BLACK))
            else:
                G.extend(quad_z(zf + 0.041, c[0] - 0.05, c[0] + 0.05, c[1] - 0.04, c[1] + 0.04, AUX_LENS))
    else:
        G.extend(chrome_corners(rbox((0, 0.68, zf + 0.05), (1.74, 0.20, 0.10), BLACK, bottom=False)))
    if lod == 0:
        for sgn in (1.0, -1.0):
            hook = tube([(sgn * 0.36, 0.56, zf + 0.04), (sgn * 0.36, 0.51, zf + 0.15), (sgn * 0.36, 0.57, zf + 0.21)],
                        [0.02, 0.02, 0.02], 3, BLACK, cap1=BLACK, up=(1, 0, 0), phase=math.pi / 2)
            G.extend(hook.autosmooth(50.0))

    # ---- rear: tall corner taillights, chrome step bumper
    for sgn in (1.0, -1.0):
        tc = D_TAIL_C[0 if sgn < 0 else 1]
        if lod == 0:
            bezel = rbox((tc[0], tc[1], zr - 0.008), (0.14, 0.38, 0.016), BLACK)
            G.extend(drop(bezel, lambda c, n: n[2] > 0.9))
            lens = rbox((tc[0], tc[1] + 0.03, zr - 0.022), (0.11, 0.28, 0.014), TAIL_LENS)
            G.extend(drop(lens, lambda c, n: n[2] > 0.9 or abs(n[0]) > 0.9))
            G.extend(quad_z(zr - 0.029, tc[0] - 0.055, tc[0] + 0.055, 0.82, 0.86, REVERSE, side=-1.0, lift=0.002))
        else:
            G.extend(quad_z(zr, tc[0] - 0.065, tc[0] + 0.065, 0.80, 1.16, TAIL_LENS, side=-1.0))
    if lod < 2:
        rb = [(0.86, 0.60, zr - 0.01), (0.82, 0.60, zr - 0.11), (-0.82, 0.60, zr - 0.11), (-0.86, 0.60, zr - 0.01)]
        rprof = [(-0.04, -0.07), (0.04, -0.07), (0.05, -0.05), (0.05, 0.07), (-0.04, 0.07)] if lod == 0 else \
            [(-0.04, -0.07), (0.05, -0.07), (0.05, 0.07), (-0.04, 0.07)]
        G.extend(chrome_corners(sweep(rb, rprof, BLACK, 40.0)))
        if lod == 0:
            G.extend(rbox((0, 0.672, zr - 0.07), (0.50, 0.012, 0.10), fixed(0x2A, 0x2A, 0x2D)))
    else:
        G.extend(chrome_corners(rbox((0, 0.60, zr - 0.06), (1.72, 0.14, 0.10), BLACK, bottom=False)))

    # ---- underbody: frame rails, skid, tank, exhaust out behind the right rear wheel
    if lod == 0:
        for sgn in (1.0, -1.0):
            G.extend(rbox((sgn * 0.42, 0.53, -0.10), (0.08, 0.14, 4.70), UNDER))
        G.extend(rbox((0, 0.55, 0.40), (0.70, 0.12, 1.30), UNDER))
        G.extend(rbox((-0.10, 0.56, -0.95), (0.62, 0.14, 0.55), STEEL))
        G.extend(tube([(0.25, 0.50, 0.90), (0.30, 0.48, 0.0), (0.45, 0.47, -1.00), (0.48, 0.47, -2.06), D_EXHAUST],
                      [0.03] * 5, 4, STEEL, cap1=SEAM, up=(0, 1, 0), phase=math.pi / 4).autosmooth(50.0))
    elif lod == 1:
        G.extend(rbox((0, 0.54, -0.10), (0.84, 0.12, 4.6), UNDER, bottom=False))

    # ---- driver and dash
    if lod == 0:
        G.extend(rbox((0, 1.25, 0.76), (1.50, 0.16, 0.28), INTERIOR, bottom=False))
        G.extend(driver(D_DRIVER[0], D_DRIVER[1], D_DRIVER[2], D_SHIRT, D_HAT, 0.66, 1.42))
    elif lod == 1:
        G.extend(ellipsoid((D_DRIVER[0], D_DRIVER[1] + 0.18, D_DRIVER[2]), (0.10, 0.12, 0.11), 4, [0, 90, 180],
                           SKIN, phase=math.pi / 4))
    return G


def ffr_parts(lod: int) -> dict[int, Geo]:
    if lod == 0:
        w, a = wheel_lod0(D_WHEEL), axle_lod0(2 * D_WX, D_WX - 0.5 * D_TW)
    elif lod == 1:
        w, a = wheel_lod1(D_WHEEL), axle_lod1(2 * D_WX)
    else:
        w, a = wheel_lod2(D_WHEEL), None
    body = ffr_body(lod)
    if lod == 0:
        shade_low(body, S_BODY, D_ROCK, 1.0, 0.82)
    parts = {BODY: body, WHEEL_R: w, WHEEL_L: w.mirrored_x()}
    if a is not None:
        parts[AXLE] = a
    return parts


FFR_INFO = {
    "name": "FFR",
    "wheel_pos": [(-D_WX, D_R, D_WZ), (D_WX, D_R, D_WZ), (-D_WX, D_R, -D_WZ), (D_WX, D_R, -D_WZ)],
    "wheel_radius": D_R, "wheel_width": D_TW,
    "headlight": D_HEAD_C, "headlight_r": 0.08,
    "aux": D_AUX_C,
    "taillight": D_TAIL_C,
    "exhaust": D_EXHAUST,
    "driver_eye": (D_DRIVER[0], D_DRIVER[1] + 0.20, D_DRIVER[2] + 0.10),
    "liveries": [
        ("CANDY RED", D_BODY_RGB, D_STRIPE_RGB, D_RIM_RGB, 0),
        ("BRIGHT WHITE", rgb(0xF0, 0xF0, 0xEC), rgb(0x2A, 0x5C, 0xA8), D_RIM_RGB, 0),
        ("TEAL", rgb(0x14, 0x78, 0x80), rgb(0xC4, 0xC8, 0xCE), D_RIM_RGB, 0),
        ("WILLOW", rgb(0x6C, 0x80, 0x6A), rgb(0xD6, 0xC4, 0x98), rgb(0xCC, 0xCC, 0xC6), 0),
    ],
}


# --------------------------------------------------------------------------
# TACO: 2026 midsize double-cab pickup with a short bed
# --------------------------------------------------------------------------

T_R, T_TW = 0.42, 0.29
T_WX, T_WZ = 0.82, 1.675
T_HW = 0.94
T_ROCK = 0.64
T_BELT = 1.38             # cab beltline
T_BED_TOP = 1.38
T_FRONT = 2.50            # grille face
T_COWL = 1.22             # hood's rear edge, windshield base
T_CABB = -0.92
T_BEDF, T_BEDB = -1.00, -2.67
T_ARCH = (0.57, 0.53)     # wheel arch half-length, height above the wheel centre
T_SIDE_Z = [0.08, 0.0, -0.72]      # front door window end, B pillar end, rear door window end
T_ROOF = 1.90
T_LAMP_Y = (1.15, 1.245)   # headlamps, where they wrap onto the fenders
T_HOOD = (1.40, 1.35)     # hood height at the cowl and where it turns down into the nose
T_NOSE = T_FRONT - 0.26   # ...which is here
T_SILL, T_GLASS_TOP = 1.44, 1.84       # the windows' bottom and top edges
T_WS = (1.14, 0.58)       # windshield base and top (z)

T_BODY_RGB = rgb(0x2A, 0x8E, 0xB6)
T_GRAPHIC_RGB = rgb(0x20, 0x22, 0x26)
T_RIM_RGB = rgb(0x9A, 0x9E, 0xA4)

T_BODY = Paint(T_BODY_RGB, S_BODY)
T_GRAPHIC = Paint(T_GRAPHIC_RGB, S_ACCENT)
T_RIM = Paint(T_RIM_RGB, S_TRIM)
T_SHIRT = fixed(0xE0, 0xD8, 0xC4)
T_HAT = fixed(0x2C, 0x34, 0x4A)
T_SKID = fixed(0x9A, 0x9E, 0xA4)

# machined spokes on a black wheel
T_WHEEL = WheelStyle(T_R, T_TW, 8, 4, 0.046, 0.58, 0.66, 0.44, T_RIM, fixed(0x1C, 0x1C, 0x1F),
                     fixed(0x1C, 0x1C, 0x1F), beadlock=False)

T_HEAD_C = [(-0.72, 1.18, T_FRONT + 0.01), (0.72, 1.18, T_FRONT + 0.01)]
T_TAIL_C = [(-0.865, 1.13, T_BEDB - 0.03), (0.865, 1.13, T_BEDB - 0.03)]
T_AUX_C = [(-0.70, 0.79, T_FRONT + 0.125), (0.70, 0.79, T_FRONT + 0.125)]   # fog lamps in the bumper
T_EXHAUST = (0.66, 0.52, -2.40)
T_DRIVER = (-0.42, 1.53, 0.30)


def t_arch(z, lod):
    return arch_points(z, T_R, T_ARCH[0], T_ARCH[1], T_ROCK, n=(6, 3, 2)[lod], expo=5.0)


def t_well(cz, hw):
    def paint(c, n):
        if n[1] < -0.7:
            return UNDER
        d = np.array([c[2] - cz, c[1] - T_R])
        if c[1] < T_R + T_ARCH[1] + 0.03 and abs(c[2] - cz) < T_ARCH[0] + 0.08 and \
                float(np.dot(n[[2, 1]], d)) < 0 and abs(c[0]) < hw:
            return WELL
        return None
    return paint


def t_hood_y(z):
    """Hood top along the centre line: tall and nearly flat, as modern trucks are."""
    return T_HOOD[0] + (z - T_COWL) * (T_HOOD[1] - T_HOOD[0]) / (T_NOSE - T_COWL)


T_HOOD_N = (0.0, T_NOSE - T_COWL, T_HOOD[0] - T_HOOD[1])     # the hood's normal, for decals on it


def t_hood_quad(x0, x1, z0, z1, paint, lift=0.004) -> Geo:
    return decal([(x0, t_hood_y(z0), z0), (x1, t_hood_y(z0), z0), (x1, t_hood_y(z1), z1), (x0, t_hood_y(z1), z1)],
                 T_HOOD_N, paint, lift)


def t_grille(hw_top, hw_mid, hw_foot, y0, y_mid, y1, z):
    """The grille's outline: a hexagon, widest between the headlamps."""
    return [(-hw_foot, y0, z), (hw_foot, y0, z), (hw_mid, y_mid, z), (hw_top, y1, z), (-hw_top, y1, z),
            (-hw_mid, y_mid, z)]


T_GRILLE = (0.50, 0.57, 0.44, 0.88, 1.06, 1.215)


def taco_lower(lod: int) -> Geo:
    """Front clip and cab below the beltline, one rounded extrusion across X."""
    outline = [(T_FRONT, 0.72), (T_FRONT, 1.27), (T_FRONT - 0.05, 1.33), (T_NOSE, T_HOOD[1]),
               (T_COWL, T_HOOD[0]), (T_CABB, T_BELT), (T_CABB, T_ROCK)] + t_arch(T_WZ, lod)
    secs = [round_sections(-T_HW, T_HW, 0.06, 2), round_sections(-T_HW, T_HW, 0.06, 1),
            [(-T_HW, 0.0), (T_HW, 0.0)]][lod]
    g = extrude(outline, "x", secs, T_BODY, 50.0, edge_bevel=not_bottom(0.73))
    g.recolour(t_well(T_WZ, T_HW - 0.02))
    return g.recolour(lambda c, n: INTERIOR if n[1] > 0.9 and T_CABB < c[2] < T_COWL and abs(c[0]) < T_HW - 0.03
                      else None)


def taco_cab(lod: int) -> Geo:
    y_sill = T_SILL
    zA, zB, zC = T_SIDE_Z
    if lod == 2:
        rings = [[(-0.90, 1.34, T_COWL), (0.90, 1.34, T_COWL), (0.90, 1.34, T_CABB), (-0.90, 1.34, T_CABB)],
                 [(-0.78, T_ROOF, 0.52), (0.78, T_ROOF, 0.52), (0.78, T_ROOF, -0.86), (-0.78, T_ROOF, -0.86)]]
        g = loft(rings, T_BODY, 20.0, cap0=False, cap1=True).flat()
        g.recolour(lambda c, n: GLASS if n[2] > 0.4 else None)
        for sgn in (1.0, -1.0):
            g.extend(decal([(sgn * 0.885, 1.48, 1.06), (sgn * 0.885, 1.48, -0.74), (sgn * 0.805, 1.84, -0.74),
                            (sgn * 0.805, 1.84, 0.58)], (sgn, 0.2, 0.0), GLASS, 0.004))
        return g
    rings = [plan_ring(1.32, 0.91, T_COWL, T_CABB, 0.06, T_SIDE_Z, 0.56),
             plan_ring(y_sill, 0.90, T_WS[0], T_CABB + 0.002, 0.07, T_SIDE_Z, 0.56),
             plan_ring(T_GLASS_TOP, 0.83, T_WS[1], T_CABB + 0.03, 0.10, T_SIDE_Z, 0.52),
             plan_ring(T_ROOF, 0.78, 0.50, T_CABB + 0.07, 0.12, T_SIDE_Z, 0.48)]
    if lod == 1:
        rings = [rings[0], rings[1], plan_ring(1.89, 0.81, 0.53, T_CABB + 0.05, 0.11, T_SIDE_Z, 0.50)]
    y_top = rings[2][0][1]

    def paint(c, n):
        if c[1] < y_sill - 0.005:
            return T_BODY
        if c[1] < y_top - 0.002 and n[2] > 0.4 and abs(n[0]) < 0.35:
            return GLASS
        if n[1] > 0.5 or c[1] > y_top - 0.002:
            return T_BODY
        if n[2] > 0.3:
            return BLACK                      # windshield frame
        if n[2] < -0.95 and abs(c[0]) < 0.52:
            return GLASS
        if abs(n[0]) > 0.95 and zC < c[2] < zB:
            return GLASS                      # rear door window
        if lod > 0 and abs(n[0]) > 0.95 and c[2] > zA:
            return GLASS
        if abs(n[0]) > 0.95 and zB <= c[2] <= zA:
            return BLACK                      # B pillar
        return T_BODY

    def window(c, n):
        return lod == 0 and abs(n[0]) > 0.95 and c[1] > y_sill and c[2] > zA

    g = cab_glass(rings, paint, window)
    g.repaint_corners(lambda p, n, old: glass_at(p[1], y_sill, y_top) if old.slot == S_GLASS else old)
    if lod == 0:
        inner = [shrink_ring(rings[1], 0.03, -0.12), shrink_ring(rings[2], 0.03, -0.03)]
        g.extend(invert(loft(inner, INTERIOR, 20.0, cap0=False, cap1=True).flat()))
    return g


def taco_bed(lod: int) -> Geo:
    G = Geo()
    hw = T_HW
    xi = hw - 0.06                       # bed wall inner face
    y_mid, y_h = 0.5 * (T_ROCK + T_BED_TOP), T_BED_TOP - T_ROCK
    if lod == 2:
        bed = rbox((0, y_mid, (T_BEDF + T_BEDB) / 2), (2 * hw, y_h, abs(T_BEDB - T_BEDF)), T_BODY, bottom=False)
        bed.recolour(lambda c, n: BLACK if n[1] > 0.9 else None)
        return G.extend(bed)
    wall = [(T_BEDF, T_BED_TOP), (T_BEDB, T_BED_TOP), (T_BEDB, T_ROCK)] + t_arch(-T_WZ, lod) + [(T_BEDF, T_ROCK)]
    secs = [(xi, 0.0)] + round_sections(xi, hw, 0.04, 2)[3:] if lod == 0 else [(xi, 0.0), (hw, 0.0)]
    w = extrude(wall, "x", secs, T_BODY, 50.0, edge_bevel=not_bottom(0.65))
    w.recolour(t_well(-T_WZ, hw - 0.01))
    # black caps along the bed rails
    w.recolour(lambda c, n: BLACK if n[1] > 0.9 and c[1] > T_BED_TOP - 0.01 else None)
    # ...and a black composite liner inside: walls, bulkhead, tubs and the gate's inner face
    w.recolour(lambda c, n: BLACK if n[0] < -0.9 and c[1] > T_R + T_ARCH[1] else None)
    G.extend(mirror_pair(w))
    head = rbox((0, y_mid, T_BEDF - 0.025), (2 * xi, y_h, 0.05), T_BODY, bottom=False)
    G.extend(head.recolour(lambda c, n: BLACK if n[2] < -0.9 else None))
    floor = rbox((0, 0.94, (T_BEDF + T_BEDB) / 2), (2 * xi, 0.05, abs(T_BEDB - T_BEDF) - 0.02), BLACK)
    G.extend(drop(floor, lambda c, n: abs(n[0]) > 0.9 or abs(n[2]) > 0.9))
    post = drop(rbox((xi - 0.055, y_mid, T_BEDB + 0.05), (0.11, y_h, 0.10), T_BODY, bottom=False),
                lambda c, n: n[0] > 0.9)
    G.extend(mirror_pair(post))
    gate_hw = xi - 0.11
    gate = rbox((0, y_mid + 0.01, T_BEDB + 0.025), (2 * gate_hw, y_h - 0.04, 0.05), T_BODY,
                bevel=0.02 if lod == 0 else 0.0)
    G.extend(gate.recolour(lambda c, n: BLACK if n[2] > 0.9 else None))
    if lod == 0:
        # wheel tubs, the gate's pressed panel and its handle
        tub = drop(rbox((xi - 0.125, 1.03, -T_WZ), (0.25, 0.17, 1.08), BLACK), lambda c, n: n[0] > 0.9)
        G.extend(mirror_pair(tub.recolour(lambda c, n: WELL if n[1] < -0.9 else None)))
        for y in (0.82, 1.19):
            G.extend(quad_z(T_BEDB, -0.60, 0.60, y - 0.005, y + 0.005, SEAM, side=-1.0))
        G.extend(quad_z(T_BEDB, -0.11, 0.11, 1.27, 1.305, BLACK, side=-1.0))
    return G


def taco_body(lod: int) -> Geo:
    G = Geo()
    hw = T_HW
    zf, zr = T_FRONT, T_BEDB
    G.extend(taco_lower(lod))
    G.extend(taco_cab(lod))
    G.extend(taco_bed(lod))

    # ---- a small slanted graphic high on the bedside, and the hood's power bulge
    if lod < 2:
        for sgn in (1.0, -1.0):
            G.extend(quad_x(sgn * hw, zr + 0.20, zr + 0.46, 1.21, 1.31, T_GRAPHIC, sgn, slant=0.05))
            if lod == 0:
                G.extend(quad_x(sgn * hw, zr + 0.51, zr + 0.55, 1.21, 1.31, T_GRAPHIC, sgn, slant=0.05))
    if lod == 0:
        z0, z1 = T_COWL + 0.14, T_NOSE - 0.14
        bulge = rbox((0, t_hood_y(z0) - 0.005, 0.5 * (z0 + z1)), (0.76, 0.05, z1 - z0), T_BODY, bevel=0.025)
        G.extend(drop(bulge, lambda c, n: n[1] < -0.9))

    # ---- panel detail: doors, handles, cowl, wipers, mirrors, fuel door, roof fin
    if lod == 0:
        for sgn in (1.0, -1.0):
            for z in (T_COWL - 0.06, T_SIDE_Z[0] - 0.04, T_SIDE_Z[2] - 0.03):
                G.extend(quad_x(sgn * hw, z - 0.005, z + 0.005, 0.70, 1.34, SEAM, sgn, lift=0.004))
            for z in (0.18, -0.58):
                G.extend(rbox((sgn * (hw + 0.012), 1.25, z), (0.024, 0.03, 0.15), BLACK))
            G.extend(quad_x(sgn * hw, zf - 0.27, zf - 0.07, T_LAMP_Y[0], T_LAMP_Y[1], HEAD_LENS, sgn, lift=0.004))
            G.extend(quad_x(sgn * hw, zf - 0.33, zf - 0.28, T_LAMP_Y[0] + 0.02, T_LAMP_Y[1], AMBER, sgn, lift=0.004))
            head = rbox((sgn * 1.06, 1.54, 1.02), (0.11, 0.17, 0.06), BLACK)
            G.extend(head.recolour(lambda c, n: GLASS if n[2] < -0.9 else None))
            G.extend(tube([(sgn * 0.90, 1.46, 1.06), (sgn * 1.01, 1.51, 1.03)], [0.016, 0.016], 4, BLACK,
                          up=(0, 0, 1), phase=math.pi / 4).flat())
        G.extend(t_hood_quad(-0.80, 0.80, T_COWL, T_COWL + 0.08, BLACK, 0.003))
        rake = (T_WS[1] - T_WS[0]) / (T_GLASS_TOP - T_SILL)     # the windshield's lean, z per y
        for x0 in (-0.58, 0.04):
            ya, yb = T_SILL + 0.025, T_SILL + 0.047
            za, zb = T_WS[0] + 0.025 * rake, T_WS[0] + 0.047 * rake
            G.extend(decal([(x0, ya, za), (x0 + 0.50, ya, za), (x0 + 0.50, yb, zb), (x0, yb, zb)],
                           (0, -rake, 1.0), BLACK, 0.004))
        G.extend(quad_x(-hw, -1.52, -1.38, 1.15, 1.27, fixed(0x24, 0x24, 0x27), -1.0, lift=0.003))
        G.extend(drop(rbox((0, T_ROOF + 0.03, -0.62), (0.05, 0.06, 0.18), BLACK), lambda c, n: n[1] < -0.9))

    # ---- black flares and rock sliders
    if lod < 2:
        for cz in (T_WZ, -T_WZ):
            G.extend(mirror_pair(flare(t_arch(cz, lod), (cz, T_R), hw, 1.0, 0.05, 0.13, BLACK, wedge=lod > 0)))
        z0, z1 = T_WZ - T_ARCH[0] - 0.06, -T_WZ + T_ARCH[0] + 0.06
        if lod == 0:
            rail = sweep([(hw + 0.02, T_ROCK - 0.01, z0), (hw + 0.02, T_ROCK - 0.01, z1)],
                         [(-0.05, -0.04), (0.035, -0.04), (0.05, -0.015), (0.05, 0.03), (0.03, 0.045),
                          (-0.05, 0.045)], SATIN, 35.0, up=(0, 1, 0))
            G.extend(mirror_pair(rail))
        else:
            G.extend(mirror_pair(rbox((hw + 0.015, T_ROCK - 0.01, 0.5 * (z0 + z1)), (0.10, 0.085, z0 - z1), SATIN,
                                      bottom=False)))

    # ---- front: a hexagonal grille between slim upswept lamps, vents under them, and a painted bumper
    # with a black intake, fog lamps and a skid plate
    ly0, ly1 = T_LAMP_Y

    def lamp(sgn, y_in, y_out, h_in, h_out):
        return [(sgn * 0.55, y_in, zf), (sgn * 0.89, y_out, zf), (sgn * 0.89, y_out + h_out, zf),
                (sgn * 0.55, y_in + h_in, zf)]
    if lod == 0:
        G.extend(decal(t_grille(*T_GRILLE, zf), (0, 0, 1), BLACK, 0.012))
        G.extend(decal(t_grille(0.46, 0.52, 0.41, 0.91, 1.06, 1.185, zf), (0, 0, 1), fixed(0x16, 0x16, 0x19), 0.015))
        for y, w, h in ((0.97, 0.90, 0.018), (1.06, 1.02, 0.04), (1.14, 0.96, 0.018)):
            slat = rbox((0, y, zf + 0.022), (w, h, 0.012), fixed(0x3C, 0x3E, 0x44))
            G.extend(drop(slat, lambda c, n: n[2] < -0.9 or abs(n[0]) > 0.9))
        for sgn in (1.0, -1.0):
            G.extend(decal(lamp(sgn, 1.125, ly0, 0.075, ly1 - ly0), (0, 0, 1), HEAD_LENS, 0.004))
            G.extend(decal(lamp(sgn, 1.10, ly0 - 0.025, 0.016, 0.016), (0, 0, 1), REVERSE, 0.004))   # running light
            x0, x1 = sorted((sgn * 0.65, sgn * 0.82))
            G.extend(quad_z(zf, x0, x1, 0.93, 1.07, BLACK, lift=0.004))
    else:
        G.extend(decal(t_grille(*T_GRILLE, zf), (0, 0, 1), BLACK))
        for sgn in (1.0, -1.0):
            G.extend(decal(lamp(sgn, 1.10, ly0 - 0.025, 0.10, ly1 - ly0 + 0.025), (0, 0, 1), HEAD_LENS))
    zb = zf + 0.12                       # the bumper's face
    if lod == 0:
        bump = [(-0.99, 0.80, zf - 0.08), (-0.88, 0.80, zf + 0.07), (0.88, 0.80, zf + 0.07), (0.99, 0.80, zf - 0.08)]
        prof = [(-0.05, -0.11), (0.04, -0.11), (0.05, -0.09), (0.05, 0.08), (0.02, 0.11), (-0.05, 0.11)]
        G.extend(sweep(bump, prof, T_BODY, 35.0))
        G.extend(rbox((0, 0.64, zf + 0.02), (0.84, 0.07, 0.30), T_SKID, bottom=False))
        for sgn in (1.0, -1.0):
            x = sgn * 0.36
            hook = tube([(x, 0.72, zb - 0.01), (x, 0.69, zb + 0.07), (x, 0.75, zb + 0.07)], [0.018] * 3, 4,
                        RED_HOOK, cap1=RED_HOOK, up=(1, 0, 0), phase=math.pi / 4)
            G.extend(hook.autosmooth(60.0))
    else:
        G.extend(rbox((0, 0.80, zf + 0.06), (1.96, 0.22, 0.12), T_BODY, bottom=False))
    if lod < 2:
        G.extend(quad_z(zb, -0.54, 0.54, 0.70, 0.86, fixed(0x1A, 0x1A, 0x1D), lift=0.004))
        for c in T_AUX_C:
            G.extend(quad_z(zb, c[0] - 0.07, c[0] + 0.07, c[1] - 0.03, c[1] + 0.03, AUX_LENS, lift=0.004))

    # ---- rear: tall taillights wrapping onto the bedsides, a lip on the gate, a painted bumper with a
    # black step
    for sgn in (1.0, -1.0):
        tc = T_TAIL_C[0 if sgn < 0 else 1]
        if lod == 0:
            bezel = rbox((tc[0], tc[1], zr - 0.008), (0.14, 0.46, 0.016), BLACK)
            G.extend(drop(bezel, lambda c, n: n[2] > 0.9))
            lens = rbox((tc[0], tc[1] + 0.03, zr - 0.022), (0.11, 0.34, 0.014), TAIL_LENS)
            G.extend(drop(lens, lambda c, n: n[2] > 0.9 or abs(n[0]) > 0.9))
            G.extend(quad_z(zr - 0.029, tc[0] - 0.055, tc[0] + 0.055, 0.94, 0.98, REVERSE, side=-1.0, lift=0.002))
        else:
            G.extend(quad_z(zr, tc[0] - 0.065, tc[0] + 0.065, 0.92, 1.34, TAIL_LENS, side=-1.0))
        if lod < 2:
            G.extend(quad_x(sgn * hw, zr + 0.01, zr + 0.12, 0.99, 1.33, TAIL_LENS, sgn, lift=0.004))
    rb = [(0.99, 0.72, zr + 0.04), (0.90, 0.72, zr - 0.08), (-0.90, 0.72, zr - 0.08), (-0.99, 0.72, zr + 0.04)]
    if lod == 0:
        G.extend(sweep(rb, [(-0.05, -0.10), (0.05, -0.10), (0.06, -0.08), (0.06, 0.10), (-0.05, 0.10)], T_BODY, 35.0))
        G.extend(rbox((0, 0.826, zr - 0.07), (1.10, 0.012, 0.11), BLACK))
        G.extend(rbox((0, T_BED_TOP - 0.025, zr - 0.012), (1.52, 0.05, 0.05), T_BODY, bevel=0.012))
    else:
        G.extend(rbox((0, 0.72, zr - 0.04), (1.94, 0.20, 0.10), T_BODY, bottom=False))
    if lod < 2:
        G.extend(quad_z(zr - (0.13 if lod == 0 else 0.09), -0.55, 0.55, 0.62, 0.80, fixed(0x1A, 0x1A, 0x1D), side=-1.0,
                        lift=0.004))

    # ---- underbody: frame rails, transfer case skid, tank, exhaust out behind the right rear wheel
    if lod == 0:
        for sgn in (1.0, -1.0):
            G.extend(rbox((sgn * 0.45, 0.57, -0.10), (0.09, 0.14, 4.80), UNDER))
        G.extend(rbox((0, 0.59, 0.50), (0.76, 0.12, 1.40), UNDER))
        G.extend(rbox((-0.12, 0.60, -1.00), (0.62, 0.14, 0.60), STEEL))
        G.extend(tube([(0.28, 0.54, 1.00), (0.32, 0.52, 0.0), (0.50, 0.52, -1.05), (0.54, 0.52, -2.15), T_EXHAUST],
                      [0.032] * 5, 4, STEEL, cap1=SEAM, up=(0, 1, 0), phase=math.pi / 4).autosmooth(50.0))
    elif lod == 1:
        G.extend(rbox((0, 0.58, -0.10), (0.90, 0.12, 4.7), UNDER, bottom=False))

    # ---- driver and dash
    if lod == 0:
        G.extend(rbox((0, 1.39, 0.94), (1.70, 0.16, 0.28), INTERIOR, bottom=False))
        G.extend(driver(T_DRIVER[0], T_DRIVER[1], T_DRIVER[2], T_SHIRT, T_HAT, 0.80, 1.55))
    elif lod == 1:
        G.extend(ellipsoid((T_DRIVER[0], T_DRIVER[1] + 0.18, T_DRIVER[2]), (0.10, 0.12, 0.11), 4, [0, 90, 180],
                           SKIN, phase=math.pi / 4))
    return G


def taco_parts(lod: int) -> dict[int, Geo]:
    if lod == 0:
        w, a = wheel_lod0(T_WHEEL), axle_lod0(2 * T_WX, T_WX - 0.5 * T_TW)
    elif lod == 1:
        w, a = wheel_lod1(T_WHEEL), axle_lod1(2 * T_WX)
    else:
        w, a = wheel_lod2(T_WHEEL), None
    body = taco_body(lod)
    if lod == 0:
        shade_low(body, S_BODY, T_ROCK, 1.06, 0.82)
    parts = {BODY: body, WHEEL_R: w, WHEEL_L: w.mirrored_x()}
    if a is not None:
        parts[AXLE] = a
    return parts


TACO_INFO = {
    "name": "TACO",
    "wheel_pos": [(-T_WX, T_R, T_WZ), (T_WX, T_R, T_WZ), (-T_WX, T_R, -T_WZ), (T_WX, T_R, -T_WZ)],
    "wheel_radius": T_R, "wheel_width": T_TW,
    "headlight": T_HEAD_C, "headlight_r": 0.08,
    "aux": T_AUX_C,
    "taillight": T_TAIL_C,
    "exhaust": T_EXHAUST,
    "driver_eye": (T_DRIVER[0], T_DRIVER[1] + 0.20, T_DRIVER[2] + 0.10),
    "liveries": [
        ("TIDE BLUE", T_BODY_RGB, T_GRAPHIC_RGB, T_RIM_RGB, 0),
        ("ICE WHITE", rgb(0xEE, 0xF0, 0xEE), rgb(0x20, 0x22, 0x26), rgb(0x9A, 0x74, 0x44), 0),
        ("GRAPHITE", rgb(0x4C, 0x50, 0x56), rgb(0xD4, 0x34, 0x26), rgb(0x3A, 0x3C, 0x40), 0),
        ("TERRA", rgb(0xC4, 0x5C, 0x24), rgb(0x20, 0x22, 0x26), T_RIM_RGB, 0),
    ],
}


# --------------------------------------------------------------------------
# Assembly, welding, checks, output
# --------------------------------------------------------------------------


@dataclass
class Mesh(meshasm.Mesh):
    truck: int
    part: int
    lod: int


NORMAL_TOL = {0: 9.0, 1: 7.0, 2: 6.0}    # degrees, by lod (see meshasm.cluster_normals)


def build_mesh(truck, part, lod, g: Geo) -> Mesh:
    name = f"{TRUCK_NAMES[truck]} {PART_NAMES[part]} lod{lod}"
    verts, index = weld([g], name, digits=5, normal_tol=NORMAL_TOL[lod], drop_collapsed=True)
    return Mesh(name, verts, index, truck, part, lod)


TRUCKS = [(CASCADE, cascade_parts, CASCADE_INFO), (KESTREL, kestrel_parts, KESTREL_INFO),
          (BOBCAT, bobcat_parts, BOBCAT_INFO), (FFR, ffr_parts, FFR_INFO), (TACO, taco_parts, TACO_INFO)]


def assemble() -> list[Mesh]:
    meshes = []
    for truck, fn, _info in TRUCKS:
        for lod in (0, 1, 2):
            parts = fn(lod)
            for part in sorted(parts):
                m = build_mesh(truck, part, lod, parts[part])
                check_mesh(m, len(PAINT_NAMES))
                meshes.append(m)
    return meshes


def report(meshes: list[Mesh]) -> list[str]:
    errors = []
    for truck, _fn, info in TRUCKS:
        print(f"{info['name']}:")
        total_groups = 0
        for m in meshes:
            if m.truck != truck:
                continue
            nt = len(m.indices) // 3
            budget = BUDGET.get((m.lod, m.part))
            npos, ngr = position_count(m), light_group_count(m)
            if m.lod == 0:
                total_groups += ngr
            print(f"  lod{m.lod} {PART_NAMES[m.part]:<16} {nt:5d} tris (budget {budget}) {len(m.verts):5d} verts "
                  f"{npos:5d} positions {ngr:4d} lighting groups")
            if budget is not None and nt > budget:
                errors.append(f"{info['name']} {PART_NAMES[m.part]} lod{m.lod} over budget ({nt} > {budget})")
            if m.lod == 0 and m.part == BODY and npos > BUDGET_LOD0_BODY_POSITIONS:
                errors.append(f"{info['name']} body lod0 has {npos} positions (> {BUDGET_LOD0_BODY_POSITIONS})")
        print(f"  lod0 lighting groups, all meshes: {total_groups} (target < {BUDGET_LOD0_GROUPS})")
        if total_groups > BUDGET_LOD0_GROUPS:
            errors.append(f"{info['name']} has {total_groups} lod0 lighting groups")
    nv = sum(len(m.verts) for m in meshes)
    ni = sum(len(m.indices) for m in meshes)
    print(f"total {nv} verts, {ni} indices, {len(meshes)} meshes")
    return errors


def truck_info(truck: int, meshes: list[Mesh], info: dict) -> dict:
    # LOD1 has the body's bulk (bumpers, rack, lights, spare) without antenna and mirrors
    body = next(m for m in meshes if m.truck == truck and m.part == BODY and m.lod == 1)
    P = np.array([v[0] for v in body.verts])
    lo, hi = P.min(axis=0), P.max(axis=0)
    out = dict(info)
    out["body_min"], out["body_max"] = [round(float(x), 3) for x in lo], [round(float(x), 3) for x in hi]
    return out


def cf(x: float) -> str:
    return meshasm.cf(x, 5)


def v3(p) -> str:
    return meshasm.cf3(p, 5)


def write_header(meshes: list[Mesh], infos: list[dict], path: Path) -> None:
    verts, indices, descs = pack(meshes)
    assert len(verts) < 65536 and len(indices) < 65536 * 4
    assert all(d[0] < 65536 and d[2] < 65536 for d in descs), "mesh offsets overflow uint16"
    out = [
        "typedef struct { float x, y, z; int8_t nx, ny, nz; uint8_t paint; uint32_t argb; } rl_mvert_t;   "
        "/* normal * 127 */",
        "typedef struct { uint16_t first_vert, vert_count, first_index, index_count; uint8_t truck, part, lod, "
        "flags; } rl_mesh_t;",
        "enum { " + ", ".join(TRUCK_NAMES) + ", RL_TRUCK_COUNT };",
        "enum { " + ", ".join(PART_NAMES) + ", RL_PART_COUNT };",
        "enum { " + ", ".join(PAINT_NAMES) + ", RL_PAINT_COUNT };",
        "#define RL_LIVERIES 4",
        "typedef struct {",
        "    const char *name;                 /* \"CASCADE 84\", \"KESTREL\", \"BOBCAT\", \"FFR\", \"TACO\" */",
        "    float wheel_pos[4][3];            /* FL, FR, RL, RR wheel centres at ride height */",
        "    float wheel_radius, wheel_width;",
        "    float body_min[3], body_max[3];   /* a box round the body (not wheels), for collisions */",
        "    float headlight[2][3];            /* left, right main headlight lens centres */",
        "    float headlight_r;                /* lens radius, for the glow sprite */",
        "    float aux_light[4][3];            /* roof/bar lights, n_aux of them */",
        "    int n_aux;",
        "    float taillight[2][3];",
        "    float exhaust[3];                 /* tailpipe tip */",
        "    float driver_eye[3];              /* for an in-cab camera */",
        "    const char *livery_name[RL_LIVERIES];",
        "    uint32_t livery[RL_LIVERIES][4];  /* argb for slots BODY, ACCENT, TRIM, and FIXED-black tint "
        "(use 0 if unused) */",
        "} rl_truck_info_t;",
        "static const rl_truck_info_t rl_truck_info[RL_TRUCK_COUNT] = {",
    ]
    for info in infos:
        aux = list(info["aux"]) + [(0.0, 0.0, 0.0)] * (4 - len(info["aux"]))
        out.append("    {")
        out.append(f"        \"{info['name']}\",")
        out.append("        { " + ", ".join(v3(p) for p in info["wheel_pos"]) + " },")
        out.append(f"        {cf(info['wheel_radius'])}, {cf(info['wheel_width'])},")
        out.append(f"        {v3(info['body_min'])}, {v3(info['body_max'])},")
        out.append("        { " + ", ".join(v3(p) for p in info["headlight"]) + " },")
        out.append(f"        {cf(info['headlight_r'])},")
        out.append("        { " + ", ".join(v3(p) for p in aux) + " },")
        out.append(f"        {len(info['aux'])},")
        out.append("        { " + ", ".join(v3(p) for p in info["taillight"]) + " },")
        out.append(f"        {v3(info['exhaust'])},")
        out.append(f"        {v3(info['driver_eye'])},")
        out.append("        { " + ", ".join(f"\"{lv[0]}\"" for lv in info["liveries"]) + " },")
        out.append("        { " + ", ".join("{ " + ", ".join(f"0x{c:08X}u" for c in lv[1:]) + " }"
                                            for lv in info["liveries"]) + " },")
        out.append("    },")
    out.append("};")
    out.append(f"#define RL_TRUCK_VERT_COUNT {len(verts)}")
    out.append(f"#define RL_TRUCK_INDEX_COUNT {len(indices)}")
    out.append(f"#define RL_TRUCK_MESH_COUNT {len(descs)}")
    out.append("static const rl_mvert_t rl_truck_verts[RL_TRUCK_VERT_COUNT] = {")
    out += vert_rows(verts, sig=5)
    out.append("};")
    out.append("static const uint16_t rl_truck_indices[RL_TRUCK_INDEX_COUNT] = {   /* per mesh, relative to its "
               "first_vert */")
    out += index_rows(indices, 20)
    out.append("};")
    out.append("static const rl_mesh_t rl_truck_meshes[RL_TRUCK_MESH_COUNT] = {     /* flags: 0 */")
    out += mesh_rows(descs, lambda m: f"{TRUCK_NAMES[m.truck]}, {PART_NAMES[m.part]}, {m.lod}, 0")
    out.append("};")
    meshasm.write_header(path, "tools/build_trucks.py", "RL_TRUCKS_H", out)


def write_preview_json(meshes: list[Mesh], infos: list[dict], path: Path) -> None:
    write_json(path, {
        "trucks": [{
            "name": info["name"],
            "wheel_pos": [list(map(float, p)) for p in info["wheel_pos"]],
            "liveries": [[lv[0]] + list(lv[1:]) for lv in info["liveries"]],
            "headlight": [list(map(float, p)) for p in info["headlight"]],
            "aux": [list(map(float, p)) for p in info["aux"]],
            "taillight": [list(map(float, p)) for p in info["taillight"]],
            "exhaust": list(map(float, info["exhaust"])),
            "driver_eye": list(map(float, info["driver_eye"])),
        } for info in infos],
        "meshes": [{"truck": m.truck, "part": m.part, "lod": m.lod, **mesh_json(m)} for m in meshes],
    })


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=HEADER)
    ap.add_argument("--preview-json", type=Path, help="also dump meshes for preview_trucks_blender.py")
    args = ap.parse_args()
    meshes = assemble()
    errors = report(meshes)
    infos = [truck_info(t, meshes, info) for t, _fn, info in TRUCKS]
    if args.preview_json:
        write_preview_json(meshes, infos, args.preview_json)
        print(f"wrote {args.preview_json}")
    if errors:
        raise SystemExit("error: " + "; ".join(errors) + f" (not writing {args.out})")
    write_header(meshes, infos, args.out)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
