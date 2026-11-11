"""The land beyond a level's map (src/outland.c): a ring of ground 768 m
deep on a 24 m grid, seen from the high ground and never driven. A
generator draws its landforms on this grid; the kit here joins them to
the map's own border, lights them as the game does and packs the lot.

The grid is ON x ON vertices, the map the middle 33 x 33 of them: those
are never drawn, but the ring's vertices on the map's edge are, and they
stand exactly where the map's own are.
"""

from __future__ import annotations

import math
import struct

import numpy as np
from scipy.ndimage import gaussian_filter, map_coordinates
from scipy.spatial import cKDTree

from levelkit import D, H_BASE, H_STEP, HZ_AZ0, HZ_DIRS, HZ_STEP, ORG, SIZE, resample

CELL = 24.0
RING = 32                                   # cells of it beyond each edge
MAP = int(SIZE / CELL)                      # cells across the map
ON = MAP + 2 * RING + 1                     # 97 vertices a side
OORG = ORG - RING * CELL                    # -1152 m
OHZ = (ON - 1) // 2 + 1
oxs = OORG + CELL * np.arange(ON)
OX, OZ = np.meshgrid(oxs, oxs)              # [j, i]: x of column i, z of row j
OFLAT = np.stack([OX.ravel(), OZ.ravel()], axis=1)
EDGE = SIZE / 2
# Metres outside the map, and which vertices are the map's own.
OUT = np.hypot(np.maximum(np.abs(OX) - EDGE, 0), np.maximum(np.abs(OZ) - EDGE, 0))
INNER = (np.abs(OX) < EDGE - 1) & (np.abs(OZ) < EDGE - 1)
BORDER = (OUT == 0) & ~INNER


def chaikin(poly, iters=3):
    p = np.asarray(poly, dtype=float)
    for _ in range(iters):
        q = [p[0]]
        for a, b in zip(p[:-1], p[1:]):
            q.append(a * 0.75 + b * 0.25)
            q.append(a * 0.25 + b * 0.75)
        q.append(p[-1])
        p = np.array(q)
    return p


def line_dist(line, smooth=3, step=3.0):
    """Distance from every vertex to a line, how far along it (m), and the
    line's points."""
    pts = resample(chaikin(line, smooth), step)
    d, idx = cKDTree(pts).query(OFLAT)
    return d.reshape(ON, ON), (idx * step).reshape(ON, ON), pts


def side_dist(line, left=True, smooth=3):
    """Signed distance to an open line: positive to its left (looking
    along it), or its right."""
    pts = resample(chaikin(line, smooth), 3.0)
    d, idx = cKDTree(pts).query(OFLAT)
    tg = np.gradient(pts, axis=0)
    rel = OFLAT - pts[idx]
    cr = tg[idx, 0] * rel[:, 1] - tg[idx, 1] * rel[:, 0]
    s = np.where(cr > 0, d, -d).reshape(ON, ON)
    return s if left else -s


def extrude(field):
    """A field of the map's (N x N or N x N x C) at every vertex here: the
    map's own inside it, its border's value carried straight out beyond."""
    gi = (np.clip(OX, ORG, -ORG) - ORG) / D
    gj = (np.clip(OZ, ORG, -ORG) - ORG) / D
    if field.ndim == 3:
        return np.dstack([map_coordinates(field[..., c], [gj, gi], order=1) for c in range(field.shape[2])])
    return map_coordinates(field, [gj, gi], order=1)


def normals(h):
    gz, gx = np.gradient(h, CELL)
    n = np.dstack([-gx, np.ones_like(h), -gz])
    return n / np.linalg.norm(n, axis=2, keepdims=True)


def cliff(d, rise, talus=0.3, slope=0.74):
    """The height gained crossing a rim (d metres inside it), for this
    grid: the talus piled against the wall's foot, then all of the wall at
    once. A wall is then one cell across wherever the rim falls among the
    vertices, which is as near sheer as the grid can draw it."""
    rise = np.broadcast_to(np.asarray(rise, dtype=float), d.shape)
    tw = np.maximum(talus * rise / slope, 1e-3)
    t = np.clip((d + tw) / tw, 0, 1)
    return talus * rise * t ** 1.3 + (1 - talus) * rise * (d > 0)


def faces(h, drop=13.0):
    """Vertices at the foot of a wall (a cell from one far above them),
    and the way out of the wall there: coloured and lit as cliff. The rim
    stays the top it is the edge of, so tops read level to their edges
    (levelkit's faces(), for this grid)."""
    big = np.zeros_like(h)
    for dj, di in ((0, 1), (1, 0), (0, -1), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
        nb = np.roll(np.roll(h, dj, axis=0), di, axis=1)
        big = np.maximum(big, (nb - h) / math.hypot(dj, di))
    face = big > drop
    face[0, :] = face[-1, :] = face[:, 0] = face[:, -1] = False
    gz, gx = np.gradient(gaussian_filter(h, 0.7), CELL)
    g = np.hypot(gx, gz) + 1e-6
    return face, -gx / g, -gz / g


def horizon_map(h):
    """The skyline's elevation (u8 half-degrees) in the game's HZ_DIRS
    bearings from every other vertex, marched out to 720 m."""
    gi, gj = np.meshgrid(np.arange(0, ON, 2, dtype=float), np.arange(0, ON, 2, dtype=float))
    h0 = h[::2, ::2] + 1.0
    out = np.zeros((HZ_DIRS, OHZ, OHZ), dtype=np.uint8)
    for d in range(HZ_DIRS):
        az = math.radians(HZ_AZ0 + d * HZ_STEP)
        sx, sz = math.sin(az), math.cos(az)
        best = np.full(h0.shape, -90.0)
        for k in range(1, 61):
            dist = 12.0 * k
            hh = map_coordinates(h, [gj + sz * dist / CELL, gi + sx * dist / CELL], order=1, mode="nearest")
            best = np.maximum(best, np.degrees(np.arctan2(hh - h0, dist)))
        out[d] = np.clip(np.round(best * 2), 0, 255).astype(np.uint8)
    return out


def lit(alb, nrm, hz, az, el, sky, sun):
    """Colours as the game lights them (terrain_relight): az, el the
    sun's bearing and elevation in degrees, sky and sun its weights."""
    a, e = math.radians(az), math.radians(el)
    L = np.array([math.sin(a) * math.cos(e), math.sin(e), math.cos(a) * math.cos(e)])
    d = np.clip(nrm @ L, 0, 1)
    u = np.clip((az - HZ_AZ0) / HZ_STEP, 0, HZ_DIRS - 1.001)
    k = int(u)
    n = nrm.shape[0]
    hzf = hz[k].astype(float) * (1 - (u - k)) + hz[k + 1].astype(float) * (u - k)
    hzf = map_coordinates(hzf, np.meshgrid(np.arange(n) / 2.0, np.arange(n) / 2.0, indexing="ij"), order=1, mode="nearest")
    sh = np.clip((el * 2 - hzf) / 6.0 + 0.5, 0, 1)
    up = 0.55 + 0.45 * nrm[..., 1]
    return np.clip(alb * (np.asarray(sky)[None, None, :] * up[..., None] + np.asarray(sun)[None, None, :] * (d * sh)[..., None]), 0, 255)


def ribbon(line, half, level, smooth=3, step=24.0):
    """A river's surface as pairs of points down its banks: half its
    half-width and level its height, each a function of the metres along
    it. [(xl, zl, xr, zr, y)]"""
    pts = resample(chaikin(line, smooth), step)
    tg = np.gradient(pts, axis=0)
    tg /= np.linalg.norm(tg, axis=1, keepdims=True) + 1e-9
    out = []
    for k, (p, t) in enumerate(zip(pts, tg)):
        s = k * step
        n = np.array([-t[1], t[0]]) * half(s)
        out.append((p[0] + n[0], p[1] + n[1], p[0] - n[0], p[1] - n[1], level(s)))
    return out


def mesh(h, rgb, step=1):
    """Triangles and their colours for a picture (level_view): the ring's
    cells, not the map's."""
    P = np.dstack([OX, h, OZ])[::step, ::step]
    C = rgb[::step, ::step]
    a, b, c, e = P[:-1, :-1], P[:-1, 1:], P[1:, :-1], P[1:, 1:]
    cx = (a[..., 0] + e[..., 0]) * 0.5
    cz = (a[..., 2] + e[..., 2]) * 0.5
    m = (np.abs(cx) > EDGE) | (np.abs(cz) > EDGE)
    ca, cb, cc, ce = C[:-1, :-1][m], C[:-1, 1:][m], C[1:, :-1][m], C[1:, 1:][m]
    t1 = np.stack([a[m], e[m], b[m]], axis=1)
    t2 = np.stack([a[m], c[m], e[m]], axis=1)
    return np.concatenate([t1, t2]), np.concatenate([(ca + ce + cb) / 3.0, (ca + cc + ce) / 3.0])


def ribbon_mesh(rib, rgb):
    tris, cols = [], []
    for a, b in zip(rib[:-1], rib[1:]):
        p0, p1 = (a[0], a[4], a[1]), (a[2], a[4], a[3])
        q0, q1 = (b[0], b[4], b[1]), (b[2], b[4], b[3])
        tris += [[p0, q1, p1], [p0, q0, q1], [p0, p1, q1], [p0, q1, q0]]
        cols += [rgb] * 4
    return np.array(tris, dtype=float), np.array(cols, dtype=float)


def pack(h, alb, nrm, fade, hz, ribbons):
    """The ring for the game: a header, a record a vertex (height as the
    level's own u16 steps, albedo, the normal's x and z, how far it fades
    into the haze at the outer edge), the horizon map, the rivers."""
    q = np.clip(np.round((h - H_BASE) / H_STEP), 0, 65535).astype("<u2")
    rec = np.zeros(ON * ON, dtype=[("h", "<u2"), ("rgb", "u1", 3), ("nx", "i1"), ("nz", "i1"), ("fade", "u1")])
    rec["h"] = q.ravel()
    rec["rgb"] = np.clip(np.round(alb), 0, 255).reshape(-1, 3).astype(np.uint8)
    rec["nx"] = np.clip(np.round(nrm[..., 0] * 127), -127, 127).ravel().astype(np.int8)
    rec["nz"] = np.clip(np.round(nrm[..., 2] * 127), -127, 127).ravel().astype(np.int8)
    rec["fade"] = np.clip(np.round(fade * 255), 0, 255).ravel().astype(np.uint8)
    verts = rec.tobytes()
    assert len(verts) == ON * ON * 8
    hzb = hz.tobytes()
    hzb += b"\0" * (-len(hzb) % 4)
    pairs = b"".join(struct.pack("<5f", *p) for r in ribbons for p in r)
    runs, first = b"", 0
    for r in ribbons:
        runs += struct.pack("<HH", first, len(r))
        first += len(r)
    off_hz = 48 + len(verts)
    off_rib = off_hz + len(hzb)
    off_runs = off_rib + len(pairs)
    head = b"RLOU" + struct.pack("<IHHffffIIIII", 1, ON, OHZ, CELL, OORG, H_BASE, H_STEP, first, len(ribbons), off_hz, off_rib,
                                 off_runs)
    assert len(head) == 48
    return head + verts + hzb + pairs + runs
