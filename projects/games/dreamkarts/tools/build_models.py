#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26"]
# ///
"""Procedurally build DreamKarts' go-kart and Chao-like driver meshes.

Usage:
    uv run tools/build_models.py                      # writes assets/generated/models.h
    uv run tools/build_models.py --preview-json X.json  # also dumps meshes for
                                                        # tools/preview_models_blender.py

Conventions (see the header comments for the runtime contract):
  * Meters; +X = kart's right, +Y = up, +Z = forward. y = 0 is the ground.
  * For every triangle (a, b, c), cross(b - a, c - a) points out of the surface.
  * Vertices are stored relative to their part's pivot.
  * Every vertex carries a paint slot (0 fixed, 1 kart primary, 2 kart accent,
    3 Chao body, 4 Chao tip) and a 0..255 blend weight toward the Chao tip colour.

Everything is built in kart space from a handful of primitives (ellipsoid,
tube/capsule/cone/torus, lathe/tyre, loft/box) and assigned to a part. Hard
surface primitives get angle-based auto-smoothing (split normals at creases);
organic ones get analytic smooth normals. Identical vertices are welded per mesh.
Output is deterministic.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "assets" / "generated" / "models.h"

# --------------------------------------------------------------------------
# Parts, pivots and layout constants
# --------------------------------------------------------------------------

PART_NAMES = [
    "DK_PART_BODY", "DK_PART_WHEEL_FL", "DK_PART_WHEEL_FR", "DK_PART_WHEEL_RL",
    "DK_PART_WHEEL_RR", "DK_PART_STEER", "DK_PART_CHAO_BODY", "DK_PART_CHAO_HEAD",
    "DK_PART_EMOTE", "DK_PART_ARM_L", "DK_PART_ARM_R", "DK_PART_HAND_L", "DK_PART_HAND_R",
]
BODY, WFL, WFR, WRL, WRR, STEER, CBODY, CHEAD, EMOTE, ARM_L, ARM_R, HAND_L, HAND_R = range(len(PART_NAMES))

FRONT_R, FRONT_W, FRONT_X, FRONT_Z = 0.21, 0.17, 0.555, 0.625
REAR_R, REAR_W, REAR_X, REAR_Z = 0.25, 0.25, 0.575, -0.625

STEER_HUB = np.array([0.0, 0.61, 0.09])
_col = math.radians(32.0)                    # column angle above horizontal
STEER_AXIS = np.array([0.0, math.sin(_col), -math.cos(_col)])  # hub -> driver (up/back)
STEER_RIM_R = 0.15

HEAD_C = np.array([0.0, 1.09, -0.23])
HEAD_R = np.array([0.42, 0.36, 0.38])        # wider than tall; top at y = 1.45
TORSO_C = np.array([0.0, 0.50, -0.27])
TORSO_R = np.array([0.215, 0.24, 0.20])
EMOTE_C = np.array([0.0, 1.75, -0.20])
SHOULDERS = [np.array([s * 0.14, 0.60, -0.21]) for s in (-1.0, 1.0)]   # left, right

EXHAUST_TIPS = [np.array([s * 0.14, 0.38, -1.0]) for s in (-1.0, 1.0)]

PIVOTS = {
    BODY: np.zeros(3),
    WFL: np.array([-FRONT_X, FRONT_R, FRONT_Z]),
    WFR: np.array([FRONT_X, FRONT_R, FRONT_Z]),
    WRL: np.array([-REAR_X, REAR_R, REAR_Z]),
    WRR: np.array([REAR_X, REAR_R, REAR_Z]),
    STEER: STEER_HUB,
    CBODY: np.array([0.0, TORSO_C[1] - TORSO_R[1], TORSO_C[2]]),   # seat contact
    CHEAD: np.array([0.0, 0.73, -0.25]),                          # neck
    EMOTE: EMOTE_C,
    # Arms swing about the shoulder to wave and cheer; each hand grips the
    # wheel (following the steering) until its arm lets go.
    ARM_L: SHOULDERS[0],
    ARM_R: SHOULDERS[1],
}

# Mesh flags: bit0 unlit, bit1 drawn only while that arm is off the wheel.
FLAG_UNLIT = 1
FLAG_ARM_RAISED = 2

BUDGET_KART_LOD0 = 420
BUDGET_CHAO_LOD0 = 420
BUDGET_LOD1 = 330
BUDGET_LOD3 = 150
BUDGET_VERTS = 1640

# --------------------------------------------------------------------------
# Colours
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Paint:
    argb: int
    slot: int = 0
    tip: int = 0


def rgb(r: int, g: int, b: int) -> int:
    return 0xFF000000 | (r << 16) | (g << 8) | b


def lerp_rgb(a: int, b: int, t: float) -> int:
    out = 0xFF000000
    for sh in (16, 8, 0):
        ca, cb = (a >> sh) & 0xFF, (b >> sh) & 0xFF
        out |= int(round(ca + (cb - ca) * t)) << sh
    return out


RED = Paint(rgb(0xE8, 0x16, 0x1E), 1)
WHITE = Paint(rgb(0xF6, 0xF6, 0xF2), 2)
YELLOW = Paint(rgb(0xFF, 0xC8, 0x12))
GUNMETAL = Paint(rgb(0x4A, 0x4E, 0x58))
DARK = Paint(rgb(0x30, 0x32, 0x38))
SEAT = Paint(rgb(0x26, 0x26, 0x2C))
TYRE = Paint(rgb(0x1E, 0x1E, 0x22))
GRIP = Paint(rgb(0x22, 0x22, 0x26))
PIPE_HOLE = Paint(rgb(0x10, 0x10, 0x12))
HUB_CAP = Paint(rgb(0xFF, 0xF4, 0xC0))

CHAO_BODY_RGB = rgb(0x98, 0xD6, 0xFF)   # light sky blue
CHAO_TIP_RGB = rgb(0xFF, 0xE2, 0x48)    # yellow
EYE = Paint(rgb(0x14, 0x12, 0x22))
EYE_SHINE = Paint(rgb(0xFF, 0xFF, 0xFF))
MOUTH = Paint(rgb(0x4A, 0x16, 0x26))
EMOTE_BALL = Paint(rgb(0xFF, 0xE4, 0x40))
EMOTE_BANG = Paint(rgb(0xFF, 0xD0, 0x1C))
EMOTE_HEART = Paint(rgb(0xFF, 0x4C, 0x7C))


def chao(t: float) -> Paint:
    """Chao skin: blend weight t (0 = body colour, 1 = tip colour)."""
    t = min(1.0, max(0.0, t))
    w = int(round(t * 255))
    return Paint(lerp_rgb(CHAO_BODY_RGB, CHAO_TIP_RGB, w / 255.0), 4 if w >= 128 else 3, w)


def chrome(n: np.ndarray) -> Paint:
    """Fake chrome: bright when facing up, darker underneath."""
    t = 0.5 + 0.5 * float(n[1])
    return Paint(lerp_rgb(rgb(0x6C, 0x72, 0x7E), rgb(0xF4, 0xF8, 0xFF), t ** 0.8))


def smoothstep(e0: float, e1: float, x: float) -> float:
    t = min(1.0, max(0.0, (x - e0) / (e1 - e0)))
    return t * t * (3.0 - 2.0 * t)


# --------------------------------------------------------------------------
# Geometry container
# --------------------------------------------------------------------------


def unit(v) -> np.ndarray:
    v = np.asarray(v, dtype=float)
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-12 else v


class Geo:
    """Triangle soup with per-corner position, normal and paint."""

    def __init__(self) -> None:
        self.P: list[np.ndarray] = []
        self.N: list[np.ndarray | None] = []
        self.C: list[tuple[Paint, Paint, Paint]] = []

    def tri(self, ps, ns, cs) -> None:
        ps = np.array(ps, dtype=float)
        if np.linalg.norm(np.cross(ps[1] - ps[0], ps[2] - ps[0])) < 1e-8:
            return
        if isinstance(cs, Paint):
            cs = (cs, cs, cs)
        self.P.append(ps)
        self.N.append(None if ns is None else np.array([unit(n) for n in ns]))
        self.C.append(tuple(cs))

    def extend(self, other: "Geo") -> "Geo":
        self.P += [p.copy() for p in other.P]
        self.N += [None if n is None else n.copy() for n in other.N]
        self.C += list(other.C)
        return self

    def copy(self) -> "Geo":
        return Geo().extend(self)

    def __len__(self) -> int:
        return len(self.P)

    def xform(self, m=None, t=(0.0, 0.0, 0.0)) -> "Geo":
        m = np.eye(3) if m is None else np.asarray(m, dtype=float)
        t = np.asarray(t, dtype=float)
        nm = np.linalg.inv(m).T
        flip = np.linalg.det(m) < 0
        for i in range(len(self.P)):
            p = self.P[i] @ m.T + t
            n = self.N[i]
            if n is not None:
                n = n @ nm.T
                n = n / np.linalg.norm(n, axis=1, keepdims=True)
            c = self.C[i]
            if flip:
                p = p[[0, 2, 1]]
                n = None if n is None else n[[0, 2, 1]]
                c = (c[0], c[2], c[1])
            self.P[i], self.N[i], self.C[i] = p, n, c
        return self

    def mirrored_x(self) -> "Geo":
        return self.copy().xform(np.diag([-1.0, 1.0, 1.0]))

    def face_normals(self) -> np.ndarray:
        P = np.array(self.P)
        fn = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
        return fn / np.linalg.norm(fn, axis=1, keepdims=True)

    def autosmooth(self, angle_deg: float) -> "Geo":
        """Average face normals across edges flatter than angle_deg."""
        if not self.P:
            return self
        cos_t = math.cos(math.radians(angle_deg)) - 1e-6
        P = np.array(self.P)
        fn = self.face_normals()
        groups: dict[tuple, list[tuple[int, int]]] = defaultdict(list)
        for t in range(len(P)):
            for k in range(3):
                groups[_pkey(P[t, k])].append((t, k))

        def corner_angle(t: int, k: int) -> float:
            a, b, c = P[t, k], P[t, (k + 1) % 3], P[t, (k + 2) % 3]
            u, v = unit(b - a), unit(c - a)
            return math.acos(max(-1.0, min(1.0, float(np.dot(u, v)))))

        for t in range(len(P)):
            ns = []
            for k in range(3):
                acc = np.zeros(3)
                for t2, k2 in groups[_pkey(P[t, k])]:
                    if float(np.dot(fn[t], fn[t2])) >= cos_t:
                        acc += fn[t2] * corner_angle(t2, k2)
                ns.append(unit(acc))
            self.N[t] = np.array(ns)
        return self

    def flat(self) -> "Geo":
        fn = self.face_normals()
        for t in range(len(self.P)):
            self.N[t] = np.array([fn[t]] * 3)
        return self

    def repaint(self, fn) -> "Geo":
        """fn(centroid, face_normal, old_paint) -> Paint | None (keep)."""
        fns = self.face_normals()
        for t in range(len(self.P)):
            new = fn(self.P[t].mean(axis=0), fns[t], self.C[t][0])
            if new is not None:
                self.C[t] = (new, new, new)
        return self

    def repaint_corners(self, fn) -> "Geo":
        """fn(position, normal, old_paint) -> Paint, evaluated per corner."""
        for t in range(len(self.P)):
            self.C[t] = tuple(fn(self.P[t][k], self.N[t][k], self.C[t][k]) for k in range(3))
        return self

    def paint_corners(self, fn) -> "Geo":
        """fn(position, normal) -> Paint, evaluated per corner."""
        for t in range(len(self.P)):
            self.C[t] = tuple(fn(self.P[t][k], self.N[t][k]) for k in range(3))
        return self


def _pkey(p) -> tuple:
    return tuple(int(round(float(x) * 1e5)) for x in p)


def merge(*geos: Geo) -> Geo:
    out = Geo()
    for g in geos:
        out.extend(g)
    return out


def _resolve(paint, *args) -> Paint:
    return paint(*args) if callable(paint) else paint


def grid(g: Geo, P, N, C, closed: bool = True, face_paint=None) -> None:
    """Quads between successive rings. With rings running CCW around the
    direction of travel, (i,j),(i,j+1),(i+1,j+1) faces outward."""
    rows, cols = len(P), len(P[0])
    for i in range(rows - 1):
        for j in range(cols if closed else cols - 1):
            j1 = (j + 1) % cols
            quad = ((i, j), (i, j1), (i + 1, j1), (i + 1, j))
            fp = face_paint(i, j) if face_paint else None
            for a, b, c in ((0, 1, 2), (0, 2, 3)):
                idx = (quad[a], quad[b], quad[c])
                ps = [P[r][s] for r, s in idx]
                ns = None if N is None else [N[r][s] for r, s in idx]
                cs = fp if fp is not None else tuple(C[r][s] for r, s in idx)
                g.tri(ps, ns, cs)


def frame_from(normal, up=(0.0, 1.0, 0.0)) -> np.ndarray:
    """3x3 with columns (right, normal, right x normal); local +Y -> normal."""
    n = unit(normal)
    up = np.asarray(up, dtype=float)
    r = np.cross(up, n)
    if np.linalg.norm(r) < 1e-6:
        r = np.cross((0.0, 0.0, 1.0), n)
    r = unit(r)
    return np.column_stack([r, n, np.cross(r, n)])


def rot_x(a: float) -> np.ndarray:
    c, s = math.cos(a), math.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], dtype=float)


def rot_y(a: float) -> np.ndarray:
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], dtype=float)


def rot_z(a: float) -> np.ndarray:
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], dtype=float)


# --------------------------------------------------------------------------
# Primitives
# --------------------------------------------------------------------------


def ellipsoid(center, radii, segs: int, lats, paint, frame=None, phase: float = 0.0) -> Geo:
    """UV ellipsoid with poles on local +/-Y and analytic smooth normals.

    lats are polar angles in degrees from +Y (0) to -Y (180); a range that
    stops short of 180 gives an open patch. paint may be fn(local_unit_dir)."""
    radii = np.asarray(radii, dtype=float)
    P, N, C = [], [], []
    for th_d in lats:
        th = math.radians(th_d)
        rp, rn, rc = [], [], []
        for j in range(segs):
            ph = 2.0 * math.pi * j / segs + phase
            u = np.array([math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph)])
            rp.append(u * radii)
            rn.append(u / radii)
            rc.append(_resolve(paint, u))
        P.append(rp), N.append(rn), C.append(rc)
    g = Geo()
    grid(g, P, N, C)
    return g.xform(frame, center)


def geosphere(center, radii, freq: int, paint, frame=None) -> Geo:
    """Octahedral geodesic ellipsoid: 8 * freq^2 evenly spread triangles, so the
    silhouette has 4 * freq edges around the equator. Analytic smooth normals."""
    radii = np.asarray(radii, dtype=float)
    g = Geo()
    for sx in (1.0, -1.0):
        for sy in (1.0, -1.0):
            for sz in (1.0, -1.0):
                A, B, C = np.array([sx, 0, 0.0]), np.array([0, sy, 0.0]), np.array([0, 0, sz])
                if float(np.dot(np.cross(B - A, C - A), (sx, sy, sz))) < 0:
                    B, C = C, B

                def pt(i, j):
                    return unit(A + (B - A) * i / freq + (C - A) * j / freq)

                for i in range(freq):
                    for j in range(freq - i):
                        tris = [(pt(i, j), pt(i + 1, j), pt(i, j + 1))]
                        if i + j < freq - 1:
                            tris.append((pt(i + 1, j), pt(i + 1, j + 1), pt(i, j + 1)))
                        for us in tris:
                            g.tri([u * radii for u in us], [u / radii for u in us],
                                  tuple(_resolve(paint, u) for u in us))
    return g.xform(frame, center)


def tube(path, radii, sides: int, paint, cap0=None, cap1=None, closed_path=False,
         up=(0.0, 1.0, 0.0), phase: float = 0.0) -> Geo:
    """Swept circle along a polyline (capsule/cone/torus/pipe).

    paint may be fn(s) with s = 0..1 along the path. cap0/cap1 are Paints for
    flat end caps (None = open). A zero radius makes a pointed tip."""
    path = np.asarray(path, dtype=float)
    radii = list(radii)
    n = len(path)
    T = []
    for i in range(n):
        if closed_path:
            d = path[(i + 1) % n] - path[(i - 1) % n]
        else:
            d = path[min(i + 1, n - 1)] - path[max(i - 1, 0)]
        T.append(unit(d))
    up = np.asarray(up, dtype=float)
    U = [up - np.dot(up, T[0]) * T[0]]
    if np.linalg.norm(U[0]) < 1e-6:
        U[0] = np.cross(T[0], (1.0, 0.0, 0.0))
    U[0] = unit(U[0])
    for i in range(1, n):
        U.append(unit(U[-1] - np.dot(U[-1], T[i]) * T[i]))
    V = [np.cross(T[i], U[i]) for i in range(n)]
    s = [0.0]
    for i in range(1, n):
        s.append(s[-1] + float(np.linalg.norm(path[i] - path[i - 1])))
    total = s[-1] if s[-1] > 0 else 1.0

    P, N, C = [], [], []
    for i in range(n):
        if closed_path:
            drds = 0.0
        else:
            i0, i1 = max(i - 1, 0), min(i + 1, n - 1)
            drds = (radii[i1] - radii[i0]) / max(1e-9, s[i1] - s[i0])
        rp, rn, rc = [], [], []
        for j in range(sides):
            a = 2.0 * math.pi * j / sides + phase
            radial = math.cos(a) * U[i] + math.sin(a) * V[i]
            rp.append(path[i] + radii[i] * radial)
            rn.append(unit(radial - drds * T[i]))
            rc.append(_resolve(paint, s[i] / total))
        P.append(rp), N.append(rn), C.append(rc)
    if closed_path:
        P.append(P[0]), N.append(N[0]), C.append(C[0])
    g = Geo()
    grid(g, P, N, C)
    for ring, tan, cp, sign in ((P[0], T[0], cap0, -1), (P[-1], T[-1], cap1, 1)):
        if cp is None or closed_path:
            continue
        nn = [sign * tan] * 3
        for j in range(1, sides - 1):
            tri = (ring[0], ring[j], ring[j + 1]) if sign > 0 else (ring[0], ring[j + 1], ring[j])
            g.tri(tri, nn, cp)
    return g


def lathe(profile, segs: int, paint, smooth_deg: float, face_paint=None, phase: float = 0.0) -> Geo:
    """Surface of revolution about +X. profile = [(x, r), ...] running from the
    -X end over the outside to the +X end (r = 0 closes an end with a fan)."""
    P, C = [], []
    for k, (x, r) in enumerate(profile):
        rp, rc = [], []
        for j in range(segs):
            a = 2.0 * math.pi * j / segs + phase
            rp.append(np.array([x, r * math.cos(a), r * math.sin(a)]))
            rc.append(_resolve(paint, k))
        P.append(rp), C.append(rc)
    g = Geo()
    grid(g, P, None, C, face_paint=face_paint)
    return g.autosmooth(smooth_deg)


def loft(sections, paint, smooth_deg: float, closed_loop=True, cap0=True, cap1=True,
         face_paint=None, cap_paint=None) -> Geo:
    """Skin convex cross-sections. Each section's points run CCW around the
    direction from the first section to the last. closed_loop=False leaves the
    edge from the last point back to the first open (e.g. a hidden underside);
    caps still fill the whole convex outline."""
    secs = [np.asarray(s, dtype=float) for s in sections]
    # Make every outline run CCW around the direction of travel (Newell normal),
    # whatever order the caller listed it in; the open edge stays last -> first.
    travel = secs[-1].mean(axis=0) - secs[0].mean(axis=0)
    s0 = secs[0]
    newell = sum(np.cross(s0[i], s0[(i + 1) % len(s0)]) for i in range(len(s0)))
    if float(np.dot(newell, travel)) < 0:
        secs = [s[::-1].copy() for s in secs]
        if face_paint is not None:
            fp, k = face_paint, len(secs[0])
            face_paint = (lambda i, j: fp(i, k - 2 - j)) if not closed_loop else (lambda i, j: fp(i, (k - 2 - j) % k))
    C = [[paint] * len(s) for s in secs]
    g = Geo()
    grid(g, secs, None, C, closed=closed_loop, face_paint=face_paint)
    cp = cap_paint if cap_paint is not None else paint
    for sec, sign, use in ((secs[0], -1, cap0), (secs[-1], 1, cap1)):
        if not use:
            continue
        for j in range(1, len(sec) - 1):
            tri = (sec[0], sec[j], sec[j + 1]) if sign > 0 else (sec[0], sec[j + 1], sec[j])
            g.tri(tri, None, cp)
    return g.autosmooth(smooth_deg)


def rect(hx: float, hy: float, bevel: float = 0.0) -> list[tuple[float, float]]:
    """CCW rectangle (optionally chamfered) starting at the bottom-right."""
    if bevel <= 0:
        return [(hx, -hy), (hx, hy), (-hx, hy), (-hx, -hy)]
    b = bevel
    return [(hx, -hy + b), (hx, hy - b), (hx - b, hy), (-hx + b, hy),
            (-hx, hy - b), (-hx, -hy + b), (-hx + b, -hy), (hx - b, -hy)]


def prism(outline, z0: float, z1: float, paint, smooth_deg=35.0, closed_loop=True, **kw) -> Geo:
    """Extrude a CCW xy outline from z0 to z1."""
    secs = [[(x, y, z) for x, y in outline] for z in (z0, z1)]
    return loft(secs, paint, smooth_deg, closed_loop=closed_loop, **kw)


def box(center, size, paint, bevel=0.0, bottom=True, frame=None, smooth_deg=35.0, **kw) -> Geo:
    hx, hy, hz = (0.5 * s for s in size)
    outline = rect(hx, hy, bevel)
    closed = True
    if not bottom:
        # Open loop whose missing closing edge is the underside.
        if bevel > 0:
            outline = outline[-1:] + outline[:-1]
        closed = False
    g = prism(outline, -hz, hz, paint, smooth_deg, closed_loop=closed, **kw)
    return g.xform(frame, center)


def surface_point(center, radii, direction):
    """Point and outward normal on an ellipsoid along a direction from its centre."""
    d = unit(direction)
    t = 1.0 / math.sqrt(float(np.sum((d / radii) ** 2)))
    p = center + d * t
    return p, unit((p - center) / radii ** 2)


# --------------------------------------------------------------------------
# Kart
# --------------------------------------------------------------------------


def tyre(radius: float, width: float, segs: int) -> Geo:
    """Right-hand LOD0 wheel in axle space (outer face on +X): flat inner face,
    faceted tread, sloped shoulder down to a domed yellow hub with a bright cap."""
    w = 0.5 * width
    profile = [
        (-w, 0.0),                      # inner face centre
        (-w, radius),                   # inner edge
        (w - 0.22 * width, radius),     # tread
        (w, radius * 0.64),             # outer shoulder / sidewall
        (w + 0.025, 0.0),               # hub cap
    ]

    # Black rubber everywhere except the hub fan (band 3), which blends from
    # the yellow rim (ring 3) to a pale cap (ring 4).
    return lathe(profile, segs, lambda k: HUB_CAP if k == 4 else YELLOW, 40.0,
                 face_paint=lambda i, j: None if i == 3 else TYRE, phase=math.pi / segs)


def nose_section(z, hw, yb, yt):
    h = yt - yb
    return [(hw, yb, z), (hw, yb + 0.48 * h, z), (0.80 * hw, yb + 0.86 * h, z),
            (0.30 * hw, yt, z), (-0.30 * hw, yt, z), (-0.80 * hw, yb + 0.86 * h, z),
            (-hw, yb + 0.48 * h, z), (-hw, yb, z)]


def pod_section(z, s=1.0, drop=0.0):
    """Right side pod outline at z, open underneath. s shrinks towards the centre."""
    cx, cy = 0.465, 0.17
    pts = [(0.625, 0.075), (0.66, 0.175), (0.605, 0.28), (0.37, 0.305), (0.30, 0.26), (0.30, 0.075)]
    return [(cx + (x - cx) * s, cy + (y - cy) * s - drop, z) for x, y in pts]


def build_kart_lod0() -> dict[int, list[tuple[Geo, int]]]:
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    body = parts[BODY]

    # Nose cone / front cowl with a white racing stripe; dark dash facing the driver.
    nose = loft([
        nose_section(0.975, 0.16, 0.13, 0.225),
        nose_section(0.86, 0.29, 0.085, 0.29),
        nose_section(0.24, 0.29, 0.08, 0.475),
    ], RED, 50.0, closed_loop=False, face_paint=lambda i, j: WHITE if j == 3 else None)
    nose.repaint(lambda c, n, p: DARK if n[2] < -0.9 else None)
    body.append((nose, 0))

    # Front bumper bar.
    body.append((tube([(-0.47, 0.15, 0.70), (-0.34, 0.15, 0.985), (0.34, 0.15, 0.985), (0.47, 0.15, 0.70)],
                      [0.03] * 4, 4, YELLOW, phase=math.pi / 4), 0))

    # Floor pan (top only; pods and nose hide its edges).
    floor = Geo()
    fl = [(0.30, 0.10, 0.70), (0.30, 0.10, -0.80), (-0.30, 0.10, -0.80), (-0.30, 0.10, 0.70)]
    floor.tri((fl[0], fl[1], fl[2]), None, DARK)
    floor.tri((fl[0], fl[2], fl[3]), None, DARK)
    _orient(floor, lambda c: (0, 1, 0))
    body.append((floor.flat(), 0))

    # Side pods (right, mirrored left), white stripe along the upper outer face.
    pod = loft([pod_section(0.415, 0.62, 0.015), pod_section(-0.375, 0.92)], RED, 40.0, closed_loop=False,
               face_paint=lambda i, j: WHITE if j == 1 else None)
    body.append((pod, 0))
    body.append((pod.mirrored_x(), 0))

    # Seat pan and a red-shelled backrest with a dark cushion facing the driver.
    body.append((box((0, 0.17, -0.27), (0.40, 0.18, 0.40), SEAT, bottom=False), 0))
    hw, h, b = 0.23, 0.50, 0.07
    back = prism([(hw, 0.0), (hw, h - b), (hw - b, h), (-hw + b, h), (-hw, h - b), (-hw, 0.0)],
                 -0.04, 0.04, RED, closed_loop=False)
    back.repaint(lambda c, n, p: SEAT if n[2] > 0.9 else None)
    back.xform(rot_x(math.radians(-14)), (0, 0.19, -0.49))
    body.append((back, 0))

    # Engine block (gunmetal, yellow top) and chrome exhausts with dark open ends.
    eng = box((0, 0.27, -0.71), (0.40, 0.30, 0.27), GUNMETAL, bottom=False)
    eng.repaint(lambda c, n, p: YELLOW if n[1] > 0.9 else None)
    body.append((eng, 0))
    for tip in EXHAUST_TIPS:
        start = np.array([tip[0] * 0.75, 0.31, -0.76])
        pipe = tube([start, tip], [0.04, 0.052], 5, GUNMETAL, cap1=PIPE_HOLE, phase=math.pi / 2)
        pipe.paint_corners(lambda p, n: chrome(n))
        pipe.repaint(lambda c, n, p: PIPE_HOLE if n[2] < -0.8 else None)
        body.append((pipe, 0))

    # Rear axle (visible below the engine from the chase camera).
    body.append((tube([(-0.47, REAR_R, REAR_Z), (0.47, REAR_R, REAR_Z)], [0.03, 0.03], 4, DARK,
                      phase=math.pi / 4), 0))

    # Steering column (body part, ends under the wheel hub).
    col0 = STEER_HUB - 0.42 * STEER_AXIS
    body.append((tube([col0, STEER_HUB - 0.01 * STEER_AXIS], [0.022, 0.022], 4, DARK, phase=math.pi / 4), 0))

    parts[STEER].append((steering_wheel(6, 3), 0))

    for part, r, w, x in ((WFL, FRONT_R, FRONT_W, -1), (WFR, FRONT_R, FRONT_W, 1),
                          (WRL, REAR_R, REAR_W, -1), (WRR, REAR_R, REAR_W, 1)):
        g = tyre(r, w, 8)
        if x < 0:
            g = g.mirrored_x()
        parts[part].append((g.xform(None, PIVOTS[part]), 0))
    return parts


def steering_wheel(segs: int, sides: int) -> Geo:
    fr = frame_from(STEER_AXIS, up=(0, 1, 0))       # local Y = steer axis
    path = []
    for k in range(segs):
        a = 2 * math.pi * k / segs
        path.append(fr @ np.array([STEER_RIM_R * math.cos(a), 0.0, STEER_RIM_R * math.sin(a)]) + STEER_HUB)
    rim = tube(path, [0.026] * segs, sides, GRIP, closed_path=True, up=STEER_AXIS, phase=math.pi / sides)
    # Cross spoke (open ends are buried in the rim).
    sw, st = 0.05, 0.016
    spoke = prism(rect(sw / 2, st / 2), -STEER_RIM_R + 0.01, STEER_RIM_R - 0.01, YELLOW,
                  cap0=False, cap1=False)
    # prism x (width) -> in-plane up/down, y (thin) -> steer axis, z (length) -> across.
    spoke.xform(np.column_stack([fr[:, 2], fr[:, 1], fr[:, 0]]), STEER_HUB)
    return merge(rim, spoke)


def tyre_lod1(radius: float, width: float, segs: int, inner: bool) -> Geo:
    """Distant wheel: one sloped band of rubber (tread + shoulder) ending in the
    same yellow-to-pale domed hub as LOD0, plus an optional flat inner face (rear
    wheels need it; the front ones' inner sides are hidden by the nose and pods)."""
    w = 0.5 * width
    profile = [(-w, radius), (w, radius * 0.76), (w + 0.025, 0.0)]
    g = lathe(profile, segs, lambda k: HUB_CAP if k == 2 else YELLOW, 40.0,
              face_paint=lambda i, j: TYRE if i == 0 else None, phase=math.pi / segs)
    if inner:
        ring = [np.array([-w, radius * math.cos(a), radius * math.sin(a)])
                for a in (2 * math.pi * j / segs + math.pi / segs for j in range(segs))]
        cap = Geo()
        for j in range(1, segs - 1):
            cap.tri((ring[0], ring[j], ring[j + 1]), None, TYRE)
        _orient(cap, lambda c: (-1.0, 0.0, 0.0))
        g.extend(cap.flat())
    return g


def nose_section_lod1(z, hw, yb, yt):
    h = yt - yb
    return [(hw, yb, z), (hw, yb + 0.55 * h, z), (0.30 * hw, yt, z),
            (-0.30 * hw, yt, z), (-hw, yb + 0.55 * h, z), (-hw, yb, z)]


def pod_section_lod1(z, s=1.0, drop=0.0):
    cx, cy = 0.465, 0.17
    pts = [(0.625, 0.075), (0.66, 0.175), (0.605, 0.28), (0.30, 0.29), (0.30, 0.075)]
    return [(cx + (x - cx) * s, cy + (y - cy) * s - drop, z) for x, y in pts]


def build_kart_lod1() -> dict[int, list[tuple[Geo, int]]]:
    """Same silhouette and colour layout as LOD0 with fewer sides/sections."""
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    body = parts[BODY]

    # Back cap (the dash) omitted: it faces the driver and is hidden behind the Chao.
    nose = loft([nose_section_lod1(0.975, 0.20, 0.11, 0.24), nose_section_lod1(0.24, 0.29, 0.08, 0.475)],
                RED, 50.0, closed_loop=False, cap1=False,
                face_paint=lambda i, j: WHITE if j == 2 else None)
    body.append((nose, 0))

    body.append((tube([(-0.47, 0.15, 0.70), (0.0, 0.15, 1.02), (0.47, 0.15, 0.70)],
                      [0.032] * 3, 3, YELLOW, phase=math.pi / 2), 0))

    floor = Geo()
    fl = [(0.30, 0.10, 0.70), (0.30, 0.10, -0.80), (-0.30, 0.10, -0.80), (-0.30, 0.10, 0.70)]
    floor.tri((fl[0], fl[1], fl[2]), None, DARK)
    floor.tri((fl[0], fl[2], fl[3]), None, DARK)
    _orient(floor, lambda c: (0, 1, 0))
    body.append((floor.flat(), 0))

    pod = loft([pod_section_lod1(0.415, 0.62, 0.015), pod_section_lod1(-0.375, 0.92)], RED, 40.0,
               closed_loop=False, cap0=False, face_paint=lambda i, j: WHITE if j == 1 else None)
    body.append((pod, 0))
    body.append((pod.mirrored_x(), 0))

    hw, h = 0.23, 0.50
    back = prism([(hw, 0.0), (hw, h), (-hw, h), (-hw, 0.0)], -0.04, 0.04, RED, closed_loop=False)
    back.repaint(lambda c, n, p: SEAT if n[2] > 0.9 else None)
    back.xform(rot_x(math.radians(-14)), (0, 0.19, -0.49))
    body.append((back, 0))

    eng = box((0, 0.27, -0.71), (0.40, 0.30, 0.27), GUNMETAL, bottom=False, cap1=False)  # front hidden
    eng.repaint(lambda c, n, p: YELLOW if n[1] > 0.9 else None)
    body.append((eng, 0))
    for tip in EXHAUST_TIPS:
        start = np.array([tip[0] * 0.75, 0.31, -0.76])
        pipe = tube([start, tip], [0.045, 0.055], 3, GUNMETAL, cap1=PIPE_HOLE, phase=math.pi / 2)
        pipe.paint_corners(lambda p, n: chrome(n))
        pipe.repaint(lambda c, n, p: PIPE_HOLE if n[2] < -0.8 else None)
        body.append((pipe, 0))
    body.append((tube([(-0.47, REAR_R, REAR_Z), (0.47, REAR_R, REAR_Z)], [0.035, 0.035], 3, DARK,
                      phase=math.pi / 2), 0))

    for part, r, w, x in ((WFL, FRONT_R, FRONT_W, -1), (WFR, FRONT_R, FRONT_W, 1),
                          (WRL, REAR_R, REAR_W, -1), (WRR, REAR_R, REAR_W, 1)):
        g = tyre_lod1(r, w, 8, inner=part in (WRL, WRR))
        if x < 0:
            g = g.mirrored_x()
        parts[part].append((g.xform(None, PIVOTS[part]), 0))
    return parts


# --------------------------------------------------------------------------
# Chao
# --------------------------------------------------------------------------


def hand_positions() -> list[np.ndarray]:
    """Rim points at "ten to two", nudged towards the driver."""
    plane_up = unit(np.cross(STEER_AXIS, (1.0, 0.0, 0.0)))
    if plane_up[1] < 0:
        plane_up = -plane_up
    a = math.radians(62.0)
    return [STEER_HUB + STEER_RIM_R * (math.sin(a) * np.array([s, 0.0, 0.0]) + math.cos(a) * plane_up)
            + 0.012 * STEER_AXIS for s in (-1.0, 1.0)]


PIVOTS[HAND_L], PIVOTS[HAND_R] = hand_positions()


def eye_frames():
    """(centre, frame, radii) for each eye; frame local Y = outward, local Z = down."""
    out = []
    for s in (-1.0, 1.0):
        p, n = surface_point(HEAD_C, HEAD_R, (s * 0.40, 0.03, 1.0))
        fr = frame_from(n, up=(0, 1, 0)) @ rot_y(s * math.radians(-12))
        out.append((s, p - 0.016 * n, fr, np.array([0.074, 0.036, 0.102])))
    return out


def head_point_path():
    """Centre line and radii of the teardrop point that curls back off the crown."""
    top = HEAD_C[1] + HEAD_R[1]
    path = [(0, top - 0.14, -0.31), (0, top + 0.02, -0.35), (0, top + 0.115, -0.43),
            (0, top + 0.145, -0.545), (0, top + 0.09, -0.655)]
    return path, [0.145, 0.125, 0.085, 0.045, 0.0]


def wing_frame(s: float):
    """(long axis, root, frame) of the wing on side s; frame local Y runs along the wing."""
    d = unit((s * 0.90, 0.40, -0.35))      # out, a little up, back
    root = np.array([s * 0.09, 0.63, -0.40])
    return d, root, frame_from(d, up=(0, 0, -1))   # flat side faces back


def build_chao_lod0() -> dict[int, list[tuple[Geo, int]]]:
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)

    # ---- Head: big squashed sphere, glossy eyes, smile, curling head point.
    # Poles on +/-Z: the silhouette seen from behind (chase camera) or in front is
    # the full 14-segment ring; 30-degree bands give a 12-gon from the side/top.
    to_z = np.column_stack([(1.0, 0, 0), (0, 0, 1.0), (0, -1.0, 0)])   # local Y -> +Z, local Z -> -Y
    head = ellipsoid(HEAD_C, (HEAD_R[0], HEAD_R[2], HEAD_R[1]), 14, [0, 30, 60, 90, 120, 150, 180],
                     chao(0.0), frame=to_z, phase=-math.pi / 2)
    parts[CHEAD].append((head, 0))

    shine = Geo()
    for s, eye_c, fr, eye_r in eye_frames():
        parts[CHEAD].append((ellipsoid(eye_c, eye_r, 8, [0, 55, 100], EYE, frame=fr), 0))
        # Highlights: a big unlit oval upper-outer, a tiny triangle lower-inner.
        for off, rr, k in (((s * 0.32, 0.45), (0.021, 0.029), 5), ((s * -0.30, -0.48), (0.012, 0.012), 3)):
            d = unit((off[0], 0.86, -off[1]))
            t = 1.0 / math.sqrt(float(np.sum((d / eye_r) ** 2)))
            q = eye_c + fr @ (d * t)
            qn = unit(fr @ (d * t / eye_r ** 2))
            qf = frame_from(qn, up=(0, 1, 0))
            pts = [q + 0.004 * qn + qf @ np.array([rr[0] * math.cos(a), 0.0, rr[1] * math.sin(a)])
                   for a in (2 * math.pi * i / k + math.pi / 2 for i in range(k))]
            for i in range(1, k - 1):
                shine.tri((pts[0], pts[i + 1], pts[i]), [qn] * 3, EYE_SHINE)
    _orient(shine, lambda c: c - HEAD_C)
    parts[CHEAD].append((shine, 1))

    # Mouth: small dark crescent smile.
    mc, mn = surface_point(HEAD_C, HEAD_R, (0.0, -0.36, 1.0))
    mf = frame_from(mn)
    mright, mup = mf[:, 0], np.cross(mn, mf[:, 0])
    if mup[1] < 0:
        mup = -mup

    def on_head(x, y):
        q, qn = surface_point(HEAD_C, HEAD_R, mc + mright * x + mup * y - HEAD_C)
        return q + 0.004 * qn, qn

    U, L = [], []
    for k in range(5):
        u = -1.0 + 2.0 * k / 4
        U.append(on_head(0.068 * u, -0.018 * (1 - u * u) + 0.014 * u * u))
        L.append(on_head(0.068 * u, -0.056 * (1 - u * u) + 0.014 * u * u))
    mouth = Geo()
    for k in range(4):
        quad = (U[k], U[k + 1], L[k + 1], L[k])
        for a, b, c in ((0, 1, 2), (0, 2, 3)):
            mouth.tri([quad[a][0], quad[b][0], quad[c][0]], [quad[a][1], quad[b][1], quad[c][1]], MOUTH)
    _orient(mouth, lambda c: c - HEAD_C)
    parts[CHEAD].append((mouth, 0))

    # Head point: a teardrop rising from the crown and curling back.
    pt_path, pt_r = head_point_path()
    point = tube(pt_path, pt_r, 6, lambda s: chao(smoothstep(0.2, 0.9, s)), up=(1, 0, 0), phase=math.pi / 6)
    parts[CHEAD].append((point, 0))

    # ---- Body: small round torso, stubby arms and feet, tiny wings.
    parts[CBODY].append((ellipsoid(TORSO_C, TORSO_R, 6, [0, 60, 120, 180], chao(0.0), phase=math.pi / 2), 0))

    for s, hp, shoulder, arm_part, hand_part in zip((-1.0, 1.0), hand_positions(), SHOULDERS,
                                                    (ARM_L, ARM_R), (HAND_L, HAND_R)):
        wrist = hp - 0.045 * unit(hp - shoulder)
        mid = 0.5 * (shoulder + wrist) + np.array([s * 0.035, -0.01, 0.0])
        arm = tube([shoulder, mid, wrist], [0.064, 0.056, 0.048], 5,
                   lambda t: chao(0.75 * smoothstep(0.35, 1.0, t)), up=(0, 1, 0))
        parts[arm_part].append((arm, 0))
        parts[hand_part].append((ellipsoid(hp, (0.066, 0.062, 0.066), 5, [0, 90, 180], chao(0.95),
                                           phase=math.pi / 2), 0))

        foot_c = np.array([s * 0.11, 0.30, -0.03])
        foot = ellipsoid(foot_c, (0.075, 0.065, 0.12), 5, [0, 90, 180],
                         lambda u: chao(smoothstep(-0.2, 0.9, float(u[2]))), frame=rot_y(s * 0.25),
                         phase=math.pi / 2)
        parts[CBODY].append((foot, 0))

        d, root, fr = wing_frame(s)
        wing = ellipsoid(root + d * 0.11, (0.095, 0.125, 0.03), 5, [0, 70, 125, 180],
                         lambda u: chao(smoothstep(0.0, 1.0, float(u[1]))), frame=fr, phase=math.pi / 2)
        parts[CBODY].append((wing, 0))
    return parts


def build_chao_lod1() -> dict[int, list[tuple[Geo, int]]]:
    """Same character as LOD0: round head with its curling point, oval eyes,
    arms reaching forward to where the (LOD0-only) wheel and hands are, wings."""
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    to_z = np.column_stack([(1.0, 0, 0), (0, 0, 1.0), (0, -1.0, 0)])
    parts[CHEAD].append((ellipsoid(HEAD_C, (HEAD_R[0], HEAD_R[2], HEAD_R[1]), 10, [0, 45, 90, 135, 180],
                                   chao(0.0), frame=to_z, phase=-math.pi / 2), 0))
    pt_path, pt_r = head_point_path()
    keep = [0, 2, 4]
    parts[CHEAD].append((tube([pt_path[i] for i in keep], [pt_r[i] for i in keep], 4,
                              lambda s: chao(smoothstep(0.2, 0.9, s)), up=(1, 0, 0), phase=math.pi / 4), 0))
    for s, eye_c, fr, eye_r in eye_frames():
        parts[CHEAD].append((ellipsoid(eye_c, eye_r, 4, [0, 95], EYE, frame=fr), 0))

    parts[CBODY].append((ellipsoid(TORSO_C, TORSO_R, 4, [0, 90, 180], chao(0.0), phase=math.pi / 4), 0))
    for s, hp, shoulder, arm_part in zip((-1.0, 1.0), hand_positions(), SHOULDERS, (ARM_L, ARM_R)):
        # Arm reaching to where LOD0's hand grips the wheel, ending in a blunt yellow fist.
        d = unit(hp - shoulder)
        arm = tube([shoulder, hp, hp + 0.05 * d], [0.062, 0.07, 0.0], 3,
                   lambda t: chao(0.95 * smoothstep(0.35, 0.85, t)), up=(0, 1, 0))
        parts[arm_part].append((arm, 0))
        wd, root, fr = wing_frame(s)
        parts[CBODY].append((ellipsoid(root + wd * 0.11, (0.095, 0.125, 0.03), 3, [0, 90, 180],
                                       lambda u: chao(smoothstep(0.0, 1.0, float(u[1]))), frame=fr,
                                       phase=math.pi / 2), 0))
    return parts


# --------------------------------------------------------------------------
# Emotes
# --------------------------------------------------------------------------


def heart_outline(k: int, size: float) -> list[np.ndarray]:
    pts = []
    for i in range(k):
        t = 2 * math.pi * i / k
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append(np.array([x, y + 2.5, 0.0]) * (size / 32.0))
    return pts


def puffy_heart(k: int, size: float, depth: float) -> Geo:
    """Pillow heart facing +/-Z (readable from behind and in front)."""
    outline = heart_outline(k, size)
    g = Geo()
    for sign in (1.0, -1.0):
        c = np.array([0.0, -0.05 * size, sign * depth])
        for i in range(k):
            a, b = outline[i], outline[(i + 1) % k]
            # outline runs clockwise seen from +Z (x = sin^3 t), so (c, b, a) faces +Z.
            g.tri((c, b, a) if sign > 0 else (c, a, b), None, EMOTE_HEART)
    _orient(g, lambda cen: np.array([0.0, 0.0, cen[2]]))
    return g.autosmooth(80.0)


def build_emotes(lod: int) -> dict[int, list[Geo]]:
    """Returns {variant: [geo, ...]} in emote-pivot space."""
    out: dict[int, list[Geo]] = {}
    if lod == 0:
        out[1] = [geosphere((0, 0, 0), (0.09, 0.09, 0.09), 2, EMOTE_BALL)]
        bar = tube([(0, 0.17, 0), (0, 0.0, 0)], [0.052, 0.03], 6, EMOTE_BANG, cap0=EMOTE_BANG, cap1=EMOTE_BANG)
        bar.autosmooth(50.0)
        dot = ellipsoid((0, -0.085, 0), (0.04, 0.04, 0.04), 5, [0, 90, 180], EMOTE_BANG)
        out[2] = [bar, dot]
        out[3] = [puffy_heart(12, 0.21, 0.05)]
    else:
        out[1] = [ellipsoid((0, 0, 0), (0.09, 0.09, 0.09), 6, [0, 90, 180], EMOTE_BALL)]
        bar = tube([(0, 0.17, 0), (0, 0.0, 0)], [0.055, 0.03], 3, EMOTE_BANG, cap0=EMOTE_BANG,
                   phase=math.pi / 2).autosmooth(50.0)
        dot = tube([(0, -0.12, 0), (0, -0.05, 0)], [0.05, 0.0], 3, EMOTE_BANG, cap0=EMOTE_BANG,
                   phase=math.pi / 2)
        out[2] = [bar, dot]
        out[3] = [puffy_heart(8, 0.21, 0.05)]
    return out


# --------------------------------------------------------------------------
# Far LOD (lod 3): the cheapest model, used beyond ~40 m (1P) / ~25 m (split screen)
# --------------------------------------------------------------------------


def tyre_lod3(radius: float, width: float, segs: int) -> Geo:
    """Closed drum with a tapered outer sidewall framing a yellow hub. The tread
    keeps the tube's smooth normals (one shared vertex per corner) so the far
    wheel costs few vertices; the flat end caps are separate polygons."""
    w = 0.5 * width
    tread = tube([(-w, 0, 0), (w, 0, 0)], [radius, radius * 0.74], segs, TYRE, up=(0, 1, 0),
                 phase=math.pi / segs)
    caps = tube([(-w, 0, 0), (w, 0, 0)], [radius, radius * 0.74], segs, TYRE, cap0=TYRE, cap1=YELLOW,
                up=(0, 1, 0), phase=math.pi / segs)
    assert len(caps) == 2 * segs + 2 * (segs - 2)   # side quads first, then the two cap fans
    caps.P, caps.N, caps.C = caps.P[2 * segs:], caps.N[2 * segs:], caps.C[2 * segs:]   # drop its side quads
    return tread.extend(caps)


def build_kart_lod3() -> dict[int, list[tuple[Geo, int]]]:
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    body = parts[BODY]

    def sec(z, hw, yb, yt):
        return [(hw, yb, z), (0.0, yt, z), (-hw, yb, z)]

    nose = loft([sec(0.975, 0.17, 0.10, 0.25), sec(0.24, 0.29, 0.08, 0.49)], RED, 50.0, closed_loop=False,
                cap1=False)
    body.append((nose, 0))
    # Both side pods + floor as one slab, white sides like LOD0's pod stripes.
    pods = box((0, 0.18, 0.02), (1.30, 0.22, 0.78), RED, bottom=False)
    pods.repaint(lambda c, n, p: WHITE if n[1] < 0.9 and abs(n[0]) > 0.9 else None)
    body.append((pods, 0))
    # Backrest + engine as one block: red shell, yellow top, and a back face that
    # shades from the red backrest down to the gunmetal engine (free: same vertices).
    blk = box((0, 0.36, -0.66), (0.44, 0.50, 0.38), RED, bottom=False, cap1=False)  # front hidden by Chao
    blk.repaint(lambda c, n, p: YELLOW if n[1] > 0.9 else None)
    blk.repaint_corners(lambda p, n, old: (RED if p[1] > 0.4 else GUNMETAL) if n[2] < -0.9 else old)
    body.append((blk, 0))
    for part, r, w, x in ((WFL, FRONT_R, FRONT_W, -1), (WFR, FRONT_R, FRONT_W, 1),
                          (WRL, REAR_R, REAR_W, -1), (WRR, REAR_R, REAR_W, 1)):
        g = tyre_lod3(r, w, 6 if part in (WRL, WRR) else 5)
        if x < 0:
            g = g.mirrored_x()
        parts[part].append((g.xform(None, PIVOTS[part]), 0))
    return parts


def build_chao_lod3() -> dict[int, list[tuple[Geo, int]]]:
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    parts[CHEAD].append((geosphere(HEAD_C, HEAD_R, 2, chao(0.0)), 0))
    top = HEAD_C[1] + HEAD_R[1]
    pt = tube([(0, top - 0.08, -0.31), (0, top + 0.15, -0.53)], [0.12, 0.0], 3,
              lambda s: chao(s), phase=math.pi / 2, up=(1, 0, 0))
    parts[CHEAD].append((pt, 0))
    eyes = Geo()
    for s, eye_c, fr, eye_r in eye_frames():
        n = fr[:, 1]
        c = eye_c + 0.02 * n
        q = [c + fr @ np.array(v) for v in ((0.075, 0, 0), (0, 0, -0.10), (-0.075, 0, 0), (0, 0, 0.10))]
        eyes.tri((q[0], q[1], q[2]), [n] * 3, EYE)
        eyes.tri((q[0], q[2], q[3]), [n] * 3, EYE)
    _orient(eyes, lambda c: c - HEAD_C)
    parts[CHEAD].append((eyes, 0))
    parts[CBODY].append((ellipsoid(TORSO_C, TORSO_R, 3, [0, 90, 180], chao(0.0), phase=math.pi / 2), 0))
    # No arms while driving (the head hides them from behind); a raised arm
    # (wave, cheer) shows as a short three-sided stub with a yellow fist end.
    for hp, shoulder, arm_part in zip(hand_positions(), SHOULDERS, (ARM_L, ARM_R)):
        arm = tube([shoulder, hp], [0.06, 0.068], 3, lambda t: chao(0.6 * t), cap1=chao(0.95),
                   up=(0, 1, 0), phase=math.pi / 2)
        parts[arm_part].append((arm, FLAG_ARM_RAISED))
    return parts


def build_emotes_lod3() -> dict[int, list[Geo]]:
    out: dict[int, list[Geo]] = {}
    out[1] = [ellipsoid((0, 0, 0), (0.09, 0.09, 0.09), 4, [0, 90, 180], EMOTE_BALL)]
    bar = tube([(0, 0.17, 0), (0, 0.0, 0)], [0.05, 0.0], 3, EMOTE_BANG, cap0=EMOTE_BANG)
    dot = tube([(0, -0.12, 0), (0, -0.05, 0)], [0.05, 0.0], 3, EMOTE_BANG, cap0=EMOTE_BANG)
    out[2] = [bar, dot]
    g = Geo()
    o = heart_outline(12, 0.21)
    pts = [o[6], o[8], o[10], o[0], o[2], o[4]]   # tip, left side, left lobe, cusp, right lobe, right side
    for sign in (1.0, -1.0):
        z = np.array([0, 0, sign * 0.025])
        for i in range(1, 5):
            g.tri((pts[0] + z, pts[i] + z, pts[i + 1] + z), None, EMOTE_HEART)
    _orient(g, lambda cen: np.array([0, 0, cen[2]]))
    out[3] = [g.flat()]
    return out


def _orient(g: Geo, outward) -> None:
    """Flip triangles whose winding disagrees with an outward direction hint."""
    fns = g.face_normals()
    for t in range(len(g.P)):
        c = g.P[t].mean(axis=0)
        if float(np.dot(fns[t], outward(c))) < 0:
            g.P[t] = g.P[t][[0, 2, 1]]
            if g.N[t] is not None:
                g.N[t] = g.N[t][[0, 2, 1]]
            cc = g.C[t]
            g.C[t] = (cc[0], cc[2], cc[1])


# --------------------------------------------------------------------------
# Assembly, welding, checks, output
# --------------------------------------------------------------------------


@dataclass
class Mesh:
    part: int
    lod: int
    variant: int
    flags: int
    verts: list   # (pos[3] local, nrm_i8[3], paint, argb, tip)
    indices: list


def build_mesh(part: int, lod: int, variant: int, flags: int, geos: list[Geo]) -> Mesh:
    pivot = PIVOTS[part]
    verts, index, lookup = [], [], {}
    for g in geos:
        for t in range(len(g.P)):
            assert g.N[t] is not None, f"part {PART_NAMES[part]} has a triangle without normals"
            for k in range(3):
                p = g.P[t][k] - pivot
                n = g.N[t][k]
                ni = tuple(int(max(-127, min(127, round(float(x) * 127.0)))) for x in n)
                c = g.C[t][k]
                key = (_pkey(p), ni, c.argb, c.slot, c.tip)
                if key not in lookup:
                    lookup[key] = len(verts)
                    verts.append(([round(float(x), 6) + 0.0 for x in p], ni, c.slot, c.argb, c.tip))
                index.append(lookup[key])
    return Mesh(part, lod, variant, flags, verts, index)


def check_mesh(m: Mesh) -> None:
    name = f"{PART_NAMES[m.part]} lod{m.lod} var{m.variant}"
    assert len(m.indices) % 3 == 0
    assert len(m.verts) < 65536
    P = np.array([v[0] for v in m.verts])
    Nq = np.array([v[1] for v in m.verts], dtype=float) / 127.0
    for t in range(0, len(m.indices), 3):
        a, b, c = m.indices[t:t + 3]
        assert len({a, b, c}) == 3, f"{name}: repeated index in triangle {t // 3}"
        fn = np.cross(P[b] - P[a], P[c] - P[a])
        area = 0.5 * float(np.linalg.norm(fn))
        assert area > 1e-7, f"{name}: degenerate triangle {t // 3} (area {area:g})"
        mean_n = Nq[a] + Nq[b] + Nq[c]
        assert float(np.dot(fn, mean_n)) > 0, f"{name}: triangle {t // 3} winding disagrees with its normals"
    for v in m.verts:
        assert v[2] in (0, 1, 2, 3, 4)
        assert 0 <= v[4] <= 255
        assert (v[3] >> 24) == 0xFF


def assemble() -> list[Mesh]:
    groups: dict[tuple[int, int, int, int], list[Geo]] = defaultdict(list)

    for lod, kart, chao_, emotes in ((0, build_kart_lod0(), build_chao_lod0(), build_emotes(0)),
                                     (1, build_kart_lod1(), build_chao_lod1(), build_emotes(1)),
                                     (3, build_kart_lod3(), build_chao_lod3(), build_emotes_lod3())):
        for src in (kart, chao_):
            for part, items in src.items():
                for g, flags in items:
                    groups[(part, lod, 0, flags)].append(g)
        for variant, geos in emotes.items():
            for g in geos:
                groups[(EMOTE, lod, variant, 0)].append(g.copy().xform(None, EMOTE_C))

    meshes = []
    for (part, lod, variant, flags) in sorted(groups, key=lambda k: (k[1], k[0], k[2], k[3])):
        m = build_mesh(part, lod, variant, flags, groups[(part, lod, variant, flags)])
        check_mesh(m)
        meshes.append(m)
    return meshes


def tri_count(meshes, lod, parts) -> int:
    """Triangles drawn for the given parts at a LOD while driving, taking the
    largest emote variant (raised-arm-only meshes are reported separately)."""
    fixed = sum(len(m.indices) // 3 for m in meshes if m.lod == lod and m.part in parts and m.variant == 0
                and not m.flags & FLAG_ARM_RAISED)
    var = defaultdict(int)
    for m in meshes:
        if m.lod == lod and m.part in parts and m.variant != 0:
            var[m.variant] += len(m.indices) // 3
    return fixed + (max(var.values()) if var else 0)


KART_PARTS = {BODY, WFL, WFR, WRL, WRR, STEER}
CHAO_PARTS = {CBODY, CHEAD, EMOTE, ARM_L, ARM_R, HAND_L, HAND_R}


def report(meshes: list[Mesh]) -> list[str]:
    lines = []
    for lod in (0, 1, 3):
        for part in range(len(PART_NAMES)):
            for m in meshes:
                if m.lod == lod and m.part == part:
                    lines.append(f"  lod{lod} {PART_NAMES[part]:<20} var{m.variant} flags{m.flags}: "
                                 f"{len(m.indices) // 3:4d} tris {len(m.verts):4d} verts")
    k0 = tri_count(meshes, 0, KART_PARTS)
    c0 = tri_count(meshes, 0, CHAO_PARTS)
    l1 = tri_count(meshes, 1, KART_PARTS | CHAO_PARTS)
    l3 = tri_count(meshes, 3, KART_PARTS | CHAO_PARTS)
    l3_arms = sum(len(m.indices) // 3 for m in meshes if m.lod == 3 and m.flags & FLAG_ARM_RAISED)
    nv = sum(len(m.verts) for m in meshes)
    ni = sum(len(m.indices) for m in meshes)
    lines.append(f"  LOD0 kart {k0} tris (budget {BUDGET_KART_LOD0}), Chao incl. hands + largest emote "
                 f"{c0} tris (budget {BUDGET_CHAO_LOD0})")
    lines.append(f"  LOD1 kart+Chao incl. largest emote {l1} tris (budget {BUDGET_LOD1})")
    lines.append(f"  LOD3 kart+Chao incl. largest emote {l3} tris (budget {BUDGET_LOD3}), "
                 f"+{l3_arms} with both arms raised")
    lines.append(f"  total {nv} verts (budget {BUDGET_VERTS}), {ni} indices, {len(meshes)} meshes")
    print("\n".join(lines))
    errors = []
    if k0 > BUDGET_KART_LOD0:
        errors.append("kart LOD0 over budget")
    if c0 > BUDGET_CHAO_LOD0:
        errors.append("Chao LOD0 over budget")
    if l1 > BUDGET_LOD1:
        errors.append("LOD1 over budget")
    if l3 > BUDGET_LOD3:
        errors.append("LOD3 over budget")
    if nv > BUDGET_VERTS:
        errors.append("vertex budget exceeded")
    return errors


def cf(x: float) -> str:
    s = f"{x:.6g}"
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def write_header(meshes: list[Mesh], path: Path) -> None:
    verts, indices, descs = [], [], []
    for m in meshes:
        descs.append((len(verts), len(m.verts), len(indices), len(m.indices), m.part, m.lod, m.variant, m.flags))
        verts += m.verts
        indices += m.indices
    assert len(verts) < 65536 and len(indices) < 65536

    out = [
        "/* Generated by tools/build_models.py. Do not edit. */",
        "#ifndef DK_MODELS_H",
        "#define DK_MODELS_H",
        "#include <stdint.h>",
        "typedef struct { float x, y, z; int8_t nx, ny, nz; uint8_t paint; uint32_t argb; uint8_t tip; "
        "uint8_t pad[3]; } dk_mvert_t; /* normal*127; tip = 0..255 blend toward tip colour for paint 3/4 */",
        "typedef struct { uint16_t first_vert, vert_count, first_index, index_count; uint8_t part, lod, "
        "variant, flags; } dk_mesh_t; /* flags bit0 = unlit (emissive/eye highlights), bit1 = drawn only while that arm is raised */",
        "enum { " + ", ".join(PART_NAMES) + ", DK_PART_COUNT };",
        "/* Part pivots in kart space (m): world = kart * translate(pivot) * part_rotation * vertex. */",
        "static const float dk_part_pivot[DK_PART_COUNT][3] = {",
    ]
    for i, name in enumerate(PART_NAMES):
        p = PIVOTS[i]
        out.append(f"    {{ {cf(p[0])}, {cf(p[1])}, {cf(p[2])} }}, /* {name} */")
    out.append("};")
    a = STEER_AXIS
    out.append("/* Steering column axis (unit), pointing from the wheel hub up and back towards the driver. */")
    out.append(f"static const float dk_steer_axis[3] = {{ {cf(a[0])}, {cf(a[1])}, {cf(a[2])} }};")
    out.append("static const float dk_exhaust_pos[2][3] = {  /* exhaust pipe tips, kart space (left, right) */")
    for t in EXHAUST_TIPS:
        out.append(f"    {{ {cf(t[0])}, {cf(t[1])}, {cf(t[2])} }},")
    out.append("};")
    out.append(f"static const float dk_wheel_radius[4] = {{ {cf(FRONT_R)}, {cf(FRONT_R)}, {cf(REAR_R)}, "
               f"{cf(REAR_R)} }};    /* FL, FR, RL, RR */")
    out.append(f"#define DK_MODEL_VERT_COUNT {len(verts)}")
    out.append(f"#define DK_MODEL_INDEX_COUNT {len(indices)}")
    out.append(f"#define DK_MODEL_MESH_COUNT {len(descs)}")
    out.append("static const dk_mvert_t dk_model_verts[DK_MODEL_VERT_COUNT] = {")
    for p, n, slot, argb, tip in verts:
        out.append(f"    {{ {cf(p[0])}, {cf(p[1])}, {cf(p[2])}, {n[0]}, {n[1]}, {n[2]}, {slot}, "
                   f"0x{argb:08X}u, {tip}, {{ 0, 0, 0 }} }},")
    out.append("};")
    out.append("static const uint16_t dk_model_indices[DK_MODEL_INDEX_COUNT] = {  "
               "/* indices are relative to the mesh's first_vert */")
    for i in range(0, len(indices), 18):
        out.append("    " + ", ".join(str(x) for x in indices[i:i + 18]) + ",")
    out.append("};")
    out.append("static const dk_mesh_t dk_model_meshes[DK_MODEL_MESH_COUNT] = {")
    for d in descs:
        out.append(f"    {{ {d[0]}, {d[1]}, {d[2]}, {d[3]}, {PART_NAMES[d[4]]}, {d[5]}, {d[6]}, {d[7]} }},")
    out.append("};")
    out.append("#endif")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out) + "\n")


def write_preview_json(meshes: list[Mesh], path: Path) -> None:
    data = {
        "parts": PART_NAMES,
        "pivots": {i: [float(x) for x in PIVOTS[i]] for i in range(len(PART_NAMES))},
        "exhaust": [[float(x) for x in t] for t in EXHAUST_TIPS],
        "meshes": [{
            "part": m.part, "lod": m.lod, "variant": m.variant, "flags": m.flags,
            "pos": [v[0] for v in m.verts],
            "nrm": [[c / 127.0 for c in v[1]] for v in m.verts],
            "argb": [v[3] for v in m.verts],
            "idx": m.indices,
        } for m in meshes],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=HEADER)
    ap.add_argument("--preview-json", type=Path, help="also dump meshes for preview_models_blender.py")
    args = ap.parse_args()
    meshes = assemble()
    errors = report(meshes)
    if args.preview_json:
        write_preview_json(meshes, args.preview_json)
        print(f"wrote {args.preview_json}")
    if errors:
        raise SystemExit("error: " + "; ".join(errors) + f" (not writing {args.out})")
    write_header(meshes, args.out)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
