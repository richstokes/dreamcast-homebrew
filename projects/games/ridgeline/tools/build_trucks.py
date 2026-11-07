#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26"]
# ///
"""Procedurally build Ridgeline's two trucks (Cascade 84, Kestrel).

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

TRUCK_NAMES = ["RL_TRUCK_CASCADE", "RL_TRUCK_KESTREL"]
CASCADE, KESTREL = 0, 1
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


TRUCKS = [(CASCADE, cascade_parts, CASCADE_INFO), (KESTREL, kestrel_parts, KESTREL_INFO)]


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
        "    const char *name;                 /* \"CASCADE 84\", \"KESTREL\" */",
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
