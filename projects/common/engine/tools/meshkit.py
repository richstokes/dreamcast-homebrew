"""Procedural low-poly meshes for rigid models (vehicles, karts, props).

Shared by the games' model builders (DreamKarts' tools/build_models.py,
Ridgeline's tools/build_trucks.py). A Geo is a triangle soup with
per-corner position, normal and Paint; the primitives below (ellipsoid,
geosphere, tube, lathe, loft, prism, box) build them, autosmooth() splits
normals at creases, and the game's builder welds, checks and writes them.

Conventions: metres, +Y up; for every triangle (a, b, c), cross(b - a, c - a)
points out of the surface. Everything is deterministic.
"""

from __future__ import annotations

import math
from collections import defaultdict
from dataclasses import dataclass

import numpy as np


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


def smoothstep(e0: float, e1: float, x: float) -> float:
    t = min(1.0, max(0.0, (x - e0) / (e1 - e0)))
    return t * t * (3.0 - 2.0 * t)


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
