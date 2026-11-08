#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "scipy>=1.11", "pillow>=10.1"]
# ///
"""Build Ridgeline's level: Cascade Ridge, a Pacific Northwest mountain.

A meadow to play in at the south end, a trailhead, and three routes up the
mountain that meet at an alpine lake before the last climb to the summit:

  logging road   gravel then dirt, gentle grades, long switchbacks (east)
  ridge line     steep rock, ledges and boulders (centre)
  creek bed      up the creek's valley: water, mud, logs, wet rock (west)

Routes are found by A* over the heightfield with a grade limit per route,
so switchbacks come out of the terrain, then carved into it. Obstacles
grow with the distance along each route. Everything is seeded, so the
output is reproducible.

World: +X east, +Y up, +Z north, metres. Writes to --out:

    level.bin       heights (f32) beside baked vertex colours (u32 argb),
                    water surface (f32, -1000 none) and surface ids (u8),
                    each N x N, row-major by z then x
    level.h         grid constants, surfaces, routes, obstacles, trees,
                    props, spawn points and viewpoints
    level_map.png   the minimap's picture (packed by build_textures.py)

and a preview picture to --preview.
"""

from __future__ import annotations

import argparse
import heapq
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter, map_coordinates
from scipy.spatial import cKDTree

N = 257                  # vertices per side
D = 3.0                  # metres between vertices
ORG = -384.0             # world x/z of vertex 0
SIZE = D * (N - 1)       # 768 m

PEAK = np.array([30.0, 140.0])
TRAILHEAD = np.array([8.0, -204.0])
SPAWN = np.array([0.0, -278.0])

# Surfaces: friction, rolling resistance, softness and colour live in the game.
SURFACES = ["GRASS", "FOREST", "DIRT", "GRAVEL", "ROCK", "MUD", "SCREE", "SNOW", "WETROCK"]
S = {n: i for i, n in enumerate(SURFACES)}

# The autumn palette: mustard gold, forest green, cool grey, pale sky.
ALBEDO = {
    "GRASS": (214, 166, 58),
    "FOREST": (92, 104, 56),
    "DIRT": (168, 124, 80),
    "GRAVEL": (176, 162, 136),
    "ROCK": (138, 146, 154),
    "MUD": (96, 72, 50),
    "SCREE": (156, 150, 140),
    "SNOW": (238, 242, 246),
    "WETROCK": (104, 112, 116),
}
SUN = np.array([-0.803, 0.375, 0.464])   # a late sun in the west-north-west (world.c palette)
SUN = SUN / np.linalg.norm(SUN)
HAZE = (214, 228, 236)


# ---------------------------------------------------------------------------
# Noise

def _hash(ix, iz, seed):
    h = (ix * 374761393 + iz * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def vnoise(x, z, seed):
    """Smooth value noise in [0, 1] at lattice spacing 1."""
    x0 = np.floor(x).astype(np.int64)
    z0 = np.floor(z).astype(np.int64)
    tx = x - x0
    tz = z - z0
    tx = tx * tx * (3 - 2 * tx)
    tz = tz * tz * (3 - 2 * tz)
    a = _hash(x0, z0, seed)
    b = _hash(x0 + 1, z0, seed)
    c = _hash(x0, z0 + 1, seed)
    d = _hash(x0 + 1, z0 + 1, seed)
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz


def fbm(x, z, scale, octaves, seed, gain=0.5):
    t = np.zeros_like(x)
    amp, norm, f = 1.0, 0.0, 1.0 / scale
    for o in range(octaves):
        t += amp * vnoise(x * f, z * f, seed + o * 31)
        norm += amp
        amp *= gain
        f *= 2.03
    return t / norm


def ridged(x, z, scale, octaves, seed):
    t = np.zeros_like(x)
    amp, norm, f = 1.0, 0.0, 1.0 / scale
    for o in range(octaves):
        n = 1.0 - np.abs(vnoise(x * f, z * f, seed + o * 17) * 2 - 1)
        t += amp * n * n
        norm += amp
        amp *= 0.5
        f *= 2.1
    return t / norm


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


# ---------------------------------------------------------------------------
# Grid helpers

xs = ORG + D * np.arange(N)
X, Z = np.meshgrid(xs, xs)       # X[j, i] = x of column i, Z[j, i] = z of row j


def to_grid(p):
    return (p[..., 0] - ORG) / D, (p[..., 1] - ORG) / D


def sample(field, p, order=1):
    gi, gj = to_grid(np.asarray(p, dtype=float))
    return map_coordinates(field, [np.atleast_1d(gj), np.atleast_1d(gi)], order=order, mode="nearest")


def normals(h):
    gz, gx = np.gradient(h, D)
    n = np.dstack([-gx, np.ones_like(h), -gz])
    return n / np.linalg.norm(n, axis=2, keepdims=True)


def slope_deg(h):
    return np.degrees(np.arccos(np.clip(normals(h)[..., 1], -1, 1)))


# ---------------------------------------------------------------------------
# Terrain

def base_terrain():
    rng_warp_x = fbm(X, Z, 160, 3, 11) - 0.5
    rng_warp_z = fbm(X, Z, 160, 3, 12) - 0.5
    dx = X - PEAK[0] + 60 * rng_warp_x
    dz = (Z - PEAK[1]) / 0.86 + 60 * rng_warp_z
    r = np.hypot(dx, dz)
    # Concave like a real peak: gentle foothills, steeper near the top.
    m = np.clip(1 - r / 400.0, 0, 1)
    mount = 138.0 * m ** 1.6
    crag = ridged(X, Z, 70, 4, 21)
    mount += (crag - 0.45) * 16.0 * smoothstep(0.35, 0.8, m)
    mount *= 0.25 + 0.75 * smoothstep(-250, -150, Z)
    # A shoulder ridge running south-east from the peak.
    sx, sz = X - (PEAK[0] + 80), Z - (PEAK[1] - 90)
    shoulder = np.exp(-((sx * 0.7 + sz * 0.7) ** 2) / (2 * 55.0 ** 2) - ((sx * 0.7 - sz * 0.7) ** 2) / (2 * 120.0 ** 2))
    mount += 16.0 * shoulder
    meadow = 3.2 * fbm(X, Z, 90, 3, 3) + 1.4 * fbm(X, Z, 28, 2, 4) - 2.0
    h = meadow + mount
    # Hills round the edge keep the drive on the map and frame the views.
    e = np.maximum(np.abs(X), np.abs(Z))
    rim = smoothstep(300, 384, e)
    h += rim * (70.0 + 50.0 * ridged(X, Z, 60, 3, 41))
    return h


CREEK = np.array([[-50, 60], [-80, 40], [-110, 4], [-124, -40], [-112, -84], [-92, -122], [-74, -160],
                  [-62, -200], [-58, -236], [-62, -262]], dtype=float)
LAKE_C = np.array([-42.0, 66.0])
LAKE_R = 26.0
POND_C = np.array([-64.0, -268.0])
POND_R = 16.0
MUD_WALLOW = np.array([62.0, -262.0])


def resample(poly, step):
    poly = np.asarray(poly, dtype=float)
    seg = np.linalg.norm(np.diff(poly, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    t = np.arange(0, s[-1], step)
    out = np.stack([np.interp(t, s, poly[:, k]) for k in range(poly.shape[1])], axis=1)
    return np.vstack([out, poly[-1:]])


def bench(h, c, r, blend):
    """Flatten a round shelf into the hillside at its centre's height."""
    y = float(sample(h, c)[0])
    d = np.hypot(X - c[0], Z - c[1])
    w = smoothstep(r + blend, r, d)
    h[:] = h * (1 - w) + (y + (h - y) * 0.08) * w


def carve_water(h):
    """The lake's basin, the creek's channel down to the meadow pond. Returns
    the water surface height (-1000 where dry)."""
    water = np.full_like(h, -1000.0)
    lake_level = float(sample(h, LAKE_C)[0]) - 3.0
    d = np.hypot(X - LAKE_C[0], Z - LAKE_C[1])
    bowl = lake_level - 2.6 * np.clip(1 - (d / LAKE_R) ** 2, 0, 1) - 0.3
    inside = d < LAKE_R * 1.35
    h[inside] = np.minimum(h[inside], np.maximum(bowl[inside], lake_level - 2.9) + smoothstep(LAKE_R, LAKE_R * 1.35, d[inside]) * 6)
    water[d < LAKE_R + 2] = lake_level

    # The creek bed falls steadily from the lake to the pond.
    pts = resample(CREEK, 1.0)
    ground = sample(h, pts)
    bed = np.minimum.accumulate(np.minimum(ground - 2.2, lake_level - 0.6))
    bed = gaussian_filter(bed, 6, mode="nearest")
    bed = np.minimum.accumulate(bed)
    tree = cKDTree(pts)
    flat = np.stack([X.ravel(), Z.ravel()], axis=1)
    dist, idx = tree.query(flat, distance_upper_bound=26.0)
    ok = np.isfinite(dist)
    dist = dist[ok]
    idx = idx[ok]
    b = bed[idx]
    jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
    half = 5.0
    # Channel: flat bed, banks rising to the ground over ~14 m.
    target = b + np.where(dist < half, 0.0, (dist - half) * 0.55)
    cur = h[jj, ii]
    h[jj, ii] = np.minimum(cur, target)
    wet = dist < half + 0.8
    water[jj[wet], ii[wet]] = np.maximum(water[jj[wet], ii[wet]], b[wet] + 0.45)

    pond_level = float(bed[-1]) + 0.45
    d = np.hypot(X - POND_C[0], Z - POND_C[1])
    m = d < POND_R * 1.6
    h[m] = np.minimum(h[m], pond_level - 1.2 * np.clip(1 - (d[m] / POND_R) ** 2, 0, 1) - 0.25 + smoothstep(POND_R, POND_R * 1.6, d[m]) * 1.6)
    water[d < POND_R + 1.5] = np.maximum(water[d < POND_R + 1.5], pond_level)
    # Only where the water is above the ground.
    water[water < h + 0.05] = -1000.0
    return water, lake_level


# ---------------------------------------------------------------------------
# Routes

NEIGH = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1),
         (2, 1), (2, -1), (-2, 1), (-2, -1), (1, 2), (1, -2), (-1, 2), (-1, -2)]


def astar(h, start, goal, gmax, avoid=None, water=None, water_cost=40.0, turn=60.0):
    """Grade-limited A* between world points on a 6 m grid, with the
    heading as part of the state: grades past gmax cost a lot, and turning
    costs turn * (1 - cos) metres, so a route that must switchback does it
    in long legs and proper hairpins. avoid adds a cost per metre."""
    hc = h[::2, ::2]
    M = hc.shape[0]
    DC = D * 2
    extra = np.zeros((N, N)) if avoid is None else avoid.copy()
    if water is not None:
        extra = extra + np.where(water > -999, water_cost, 0.0)
    extra = extra[::2, ::2]

    def g2(p):
        return int(round((p[0] - ORG) / DC)), int(round((p[1] - ORG) / DC))
    si, sj = g2(start)
    gi, gj = g2(goal)
    nd = len(NEIGH)
    ang = [math.atan2(dj, di) for di, dj in NEIGH]
    # A turn sharper than ~95 degrees in one step is not allowed: a hairpin
    # takes at least two steps, so it has a radius a truck can drive.
    turn_cost = np.array([[turn * (1 - math.cos(a - b)) if math.cos(a - b) > -0.09 else 1e12 for b in ang]
                          for a in ang])
    INF = 1e18
    best = np.full((M, M, nd), INF)
    came = {}
    heap = []
    for d in range(nd):
        best[sj, si, d] = 0
        heapq.heappush(heap, (math.hypot(gi - si, gj - sj) * DC, 0.0, si, sj, d))
    end = None
    while heap:
        f, g, i, j, d = heapq.heappop(heap)
        if g > best[j, i, d]:
            continue
        if i == gi and j == gj:
            end = (i, j, d)
            break
        tc = turn_cost[d]
        for nd_, (di, dj) in enumerate(NEIGH):
            ni, nj = i + di, j + dj
            if not (1 <= ni < M - 1 and 1 <= nj < M - 1):
                continue
            L = math.hypot(di, dj) * DC
            grade = abs(hc[nj, ni] - hc[j, i]) / L
            c = L * (1.0 + 3.0 * grade * grade + extra[nj, ni]) + tc[nd_]
            if grade > gmax:
                c += L * 300.0 * (grade - gmax)
            ng = g + c
            if ng < best[nj, ni, nd_]:
                best[nj, ni, nd_] = ng
                came[(ni, nj, nd_)] = (i, j, d)
                heapq.heappush(heap, (ng + math.hypot(gi - ni, gj - nj) * DC, ng, ni, nj, nd_))
    path = []
    s_ = end
    while s_ is not None:
        path.append((ORG + s_[0] * DC, ORG + s_[1] * DC))
        s_ = came.get(s_)
    path = path[::-1]
    path[0] = tuple(start)
    path[-1] = tuple(goal)
    return np.array(path)


def smooth_path(p, iters=3):
    for _ in range(iters):
        q = [p[0]]
        for a, b in zip(p[:-1], p[1:]):
            q.append(a * 0.75 + b * 0.25)
            q.append(a * 0.25 + b * 0.75)
        q.append(p[-1])
        p = np.array(q)
    return p


def turniness(pts, span=7):
    """0 on a straight, 1 where the route turns 90 degrees or more within
    about span metres either side (a hairpin)."""
    n = len(pts)
    a = pts[np.clip(np.arange(n) - span, 0, n - 1)]
    b = pts[np.clip(np.arange(n) + span, 0, n - 1)]
    d0 = pts - a
    d1 = b - pts
    l0 = np.linalg.norm(d0, axis=1) + 1e-6
    l1 = np.linalg.norm(d1, axis=1) + 1e-6
    cosang = np.clip(np.einsum("ij,ij->i", d0, d1) / (l0 * l1), -1, 1)
    t = np.clip(np.arccos(cosang) / (math.pi / 2), 0, 1)
    return gaussian_filter(t, 3, mode="nearest")


def route_profile(h, pts, gmax, sigma=5.0, turn=None):
    """Centreline heights: the ground smoothed, then held to the grade, which
    eases to a near-level landing round each hairpin (so the two legs of a
    switchback never leave a step between them)."""
    y = sample(h, pts)
    y = gaussian_filter(y, sigma, mode="nearest")
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    glim = gmax * (1.0 - 0.8 * (turn if turn is not None else np.zeros(len(pts))))
    for _ in range(6):
        for k in range(1, len(y)):
            lim = glim[k] * seg[k - 1]
            y[k] = np.clip(y[k], y[k - 1] - lim, y[k - 1] + lim)
        for k in range(len(y) - 2, -1, -1):
            lim = glim[k] * seg[k]
            y[k] = np.clip(y[k], y[k + 1] - lim, y[k + 1] + lim)
    return y


def carve_route(h, pts, y, half):
    """half: per point, wider at hairpins."""
    tree = cKDTree(pts)
    flat = np.stack([X.ravel(), Z.ravel()], axis=1)
    reach = float(np.max(half)) + 22.0
    dist, idx = tree.query(flat, distance_upper_bound=reach)
    ok = np.isfinite(dist)
    jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
    dist = dist[ok]
    yt = y[idx[ok]]
    half = half[idx[ok]]
    cur = h[jj, ii]
    diff = cur - yt
    # Cut and fill slopes about 1:1, blended out past the shoulder.
    shoulder = 2.0 + np.abs(diff) * 0.9
    w = smoothstep(half, half + shoulder, dist)
    h[jj, ii] = yt + diff * w


ROUTE_DEFS = [
    # name, waypoints after the fork, grade limit, half width
    ("LOGGING ROAD", [(100, -100), (150, -30), (110, 30)], 0.15, 3.4),
    ("RIDGE LINE", [(14, -78), (8, -30), (4, 12)], 0.40, 2.6),
    ("CREEK BED", [(-80, -118), (-116, -50), (-100, 0), (-70, 36)], 0.26, 3.0),
]
FORK = np.array([8.0, -136.0])
HIGH_MEADOW = np.array([2.0, 44.0])


def build_routes(h, water):
    routes = []
    avoid = np.zeros((N, N))

    def leg(a, b, gmax, half, avoid_w=1.0, wcost=40.0):
        p = astar(h, a, b, gmax, avoid=avoid * avoid_w, water=water, water_cost=wcost)
        return p

    # The forest road from the trailhead to the fork, shared by all three.
    main = leg(TRAILHEAD, FORK, 0.2, 3.6)
    segs = [("FOREST ROAD", [main], 0.2, 3.6)]
    # The routes from the fork keep off the forest road (one doubling back
    # down it at another height would carve a step into it).
    d, _ = cKDTree(main).query(np.stack([X.ravel(), Z.ravel()], axis=1))
    avoid += np.exp(-(d.reshape(N, N) / 14.0) ** 2) * 6.0
    for name, wps, gmax, half in ROUTE_DEFS:
        pts = [FORK] + [np.array(w, dtype=float) for w in wps] + [HIGH_MEADOW]
        legs = []
        for a, b in zip(pts[:-1], pts[1:]):
            legs.append(leg(a, b, gmax, half, wcost=4.0 if name == "CREEK BED" else 40.0))
        segs.append((name, legs, gmax, half))
        # Keep the next route away from this one.
        full = np.vstack(legs)
        tree = cKDTree(full)
        d, _ = tree.query(np.stack([X.ravel(), Z.ravel()], axis=1))
        avoid += (np.exp(-(d.reshape(N, N) / 26.0) ** 2) * 3.0)
    summit = leg(HIGH_MEADOW, PEAK, 0.30, 2.8)
    segs.append(("SUMMIT TRAIL", [summit], 0.30, 2.8))
    for name, legs, gmax, half in segs:
        raw = np.vstack([legs[0]] + [lg[1:] for lg in legs[1:]])
        pts = resample(smooth_path(raw, 3), 1.0)
        routes.append({"name": name, "pts": pts, "gmax": gmax, "half": half})
    return routes


# ---------------------------------------------------------------------------
# Obstacles and scenery

def ledge(r, k, step, half_w, rng):
    """A rock ledge across route r at point k: its top stands step above
    the trail where you meet it (the downhill edge) and runs level uphill
    until the trail has climbed to it, so the step is the same however
    steep the trail."""
    pts, y = r["pts"], r["y"]
    j = min(len(pts) - 2, k + 1)
    tg = pts[j] - pts[k]
    tg = tg / (np.linalg.norm(tg) + 1e-9)
    grade = max(0.04, (y[min(len(y) - 1, k + 3)] - y[k]) / 3.0)
    depth = float(np.clip(step / grade + 0.3, 0.7, 2.6))
    top = y[k] + step
    c = pts[k] + tg * depth * 0.5
    yaw = math.atan2(tg[0], tg[1]) + rng.uniform(-0.2, 0.2)
    return (c[0], top - 1.0, c[1], half_w, 1.0, depth * 0.5, yaw)


def place_obstacles(h, water, routes, rng):
    spheres, capsules, boxes, mud = [], [], [], []
    on_trail = set()
    # name -> list of (kind, spacing)
    for r in routes:
        pts, y, half = r["pts"], r["y"], r["half"]
        n = len(pts)
        s = np.arange(n, dtype=float)
        name = r["name"]
        tang = np.gradient(pts, axis=0)
        tang /= np.linalg.norm(tang, axis=1, keepdims=True) + 1e-9
        side = np.stack([tang[:, 1], -tang[:, 0]], axis=1)   # right of travel
        k = 30
        while k < n - 30:
            t = k / n
            p, tg, sd = pts[k], tang[k], side[k]
            yaw = math.atan2(tg[0], tg[1])
            ground = y[k]
            if name == "FOREST ROAD":
                if rng.random() < 0.5:
                    off = (half + 1.0 + rng.random() * 2.0) * (1 if rng.random() < 0.5 else -1)
                    c = p + sd * off
                    rr = 0.5 + rng.random() * 0.6
                    spheres.append((c[0], float(sample(h, c)[0]) - rr * 0.35, c[1], rr))
                k += int(26 + rng.random() * 20)
                continue
            diff = t ** 1.15
            if name == "LOGGING ROAD":
                roll = rng.random()
                if roll < 0.35:
                    # Ruts: a muddy stretch.
                    mud.append((p[0], p[1], half + 1.0 + 3 * diff, 0.08 + 0.10 * diff))
                    k += int(20 + rng.random() * 12)
                elif roll < 0.55 and t > 0.25:
                    # A fallen log across the road.
                    ang = yaw + math.pi / 2 + rng.uniform(-0.35, 0.35)
                    L = half * 2 + 3.0
                    rr = 0.16 + 0.16 * diff
                    a = p - np.array([math.sin(ang), math.cos(ang)]) * L / 2
                    b = p + np.array([math.sin(ang), math.cos(ang)]) * L / 2
                    capsules.append((a[0], ground + rr * 0.75, a[1], b[0], ground + rr * 0.75, b[1], rr))
                    k += int(30 + rng.random() * 20)
                else:
                    off = (half + 0.6 + rng.random() * 2.5) * (1 if rng.random() < 0.5 else -1)
                    c = p + sd * off
                    rr = 0.4 + rng.random() * 0.9
                    spheres.append((c[0], float(sample(h, c)[0]) - rr * 0.3, c[1], rr))
                    k += int(16 + rng.random() * 16)
            elif name == "RIDGE LINE":
                roll = rng.random()
                grade_here = abs(y[min(n - 1, k + 4)] - y[k]) / 4.0
                if roll < 0.45 and grade_here < 0.25:
                    # A rock ledge across the trail.
                    boxes.append(ledge(r, k, 0.10 + 0.32 * diff + rng.random() * 0.06, half + 0.8, rng))
                    k += int(10 + rng.random() * 10)
                elif roll < 0.85:
                    # Boulders: on the edges early, across the line later.
                    for _ in range(1 + int(diff * 3)):
                        off = rng.uniform(-half - 1, half + 1) * (1.0 if diff > 0.35 else 0.0) or \
                            (half + 0.7) * (1 if rng.random() < 0.5 else -1)
                        c = p + sd * off + tg * rng.uniform(-2, 2)
                        rr = 0.35 + 0.75 * diff * rng.random() + 0.2 * rng.random()
                        on_trail.add(len(spheres))
                        spheres.append((c[0], float(sample(h, c)[0]) - rr * 0.45, c[1], rr))
                    k += int(9 + rng.random() * 10)
                else:
                    k += 8
            elif name == "CREEK BED":
                roll = rng.random()
                if roll < 0.3:
                    mud.append((p[0], p[1], half + 1.5 + 2.5 * diff, 0.10 + 0.12 * diff))
                    k += int(14 + rng.random() * 12)
                elif roll < 0.55:
                    ang = yaw + math.pi / 2 + rng.uniform(-0.5, 0.5)
                    L = half * 2 + 4.0
                    rr = 0.18 + 0.2 * diff
                    a = p - np.array([math.sin(ang), math.cos(ang)]) * L / 2
                    b = p + np.array([math.sin(ang), math.cos(ang)]) * L / 2
                    capsules.append((a[0], ground + rr * 0.7, a[1], b[0], ground + rr * 0.7, b[1], rr))
                    k += int(22 + rng.random() * 16)
                else:
                    for _ in range(2 + int(diff * 3)):
                        c = p + sd * rng.uniform(-half - 1.5, half + 1.5) + tg * rng.uniform(-3, 3)
                        rr = 0.3 + 0.55 * diff * rng.random() + 0.15 * rng.random()
                        on_trail.add(len(spheres))
                        spheres.append((c[0], float(sample(h, c)[0]) - rr * 0.5, c[1], rr))
                    k += int(12 + rng.random() * 10)
            else:   # SUMMIT TRAIL
                roll = rng.random()
                grade_here = abs(y[min(n - 1, k + 4)] - y[k]) / 4.0
                if roll < 0.4 and grade_here < 0.25:
                    boxes.append(ledge(r, k, 0.18 + 0.27 * diff + rng.random() * 0.08, half + 0.6, rng))
                    k += int(9 + rng.random() * 8)
                else:
                    for _ in range(1 + int(diff * 2)):
                        c = p + sd * rng.uniform(-half - 1.0, half + 1.0) + tg * rng.uniform(-2, 2)
                        rr = 0.35 + 0.6 * rng.random()
                        on_trail.add(len(spheres))
                        spheres.append((c[0], float(sample(h, c)[0]) - rr * 0.45, c[1], rr))
                    k += int(10 + rng.random() * 10)
    # The meadow's playground: logs, a rock pile, the mud wallow.
    for (cx, cz, ang, L, rr) in [(40, -252, 0.4, 7.0, 0.3), (52, -246, 0.5, 6.0, 0.22), (-30, -226, 1.3, 8.0, 0.35),
                                 (100, -222, 2.0, 7.0, 0.28)]:
        g = float(sample(h, np.array([cx, cz]))[0])
        dx, dz = math.sin(ang) * L / 2, math.cos(ang) * L / 2
        capsules.append((cx - dx, g + rr * 0.8, cz - dz, cx + dx, g + rr * 0.8, cz + dz, rr))
    for _ in range(14):
        c = np.array([118.0, -262.0]) + rng.normal(0, 7, 2)
        rr = 0.5 + rng.random() * 1.1
        spheres.append((c[0], float(sample(h, c)[0]) - rr * 0.35, c[1], rr))
    mud.append((MUD_WALLOW[0], MUD_WALLOW[1], 13.0, 0.35))
    return clear_roads(spheres, routes, on_trail), capsules, boxes, mud


def clear_roads(spheres, routes, keep_on):
    """Boulders meant for a road's verge, kept off every road (a hairpin's
    other leg runs close by)."""
    pts = np.vstack([r["pts"] for r in routes])
    halfs = np.concatenate([r["halfs"] for r in routes])
    tree = cKDTree(pts)
    out = []
    for k, s_ in enumerate(spheres):
        if k in keep_on:
            out.append(s_)
            continue
        d, i = tree.query((s_[0], s_[2]))
        if d > halfs[i] + s_[3] + 0.6:
            out.append(s_)
    return out


def apply_mud(h, mud):
    surf_mud = np.zeros_like(h, dtype=bool)
    for (cx, cz, r, depth) in mud:
        d = np.hypot(X - cx, Z - cz)
        n = fbm(X, Z, 6, 2, int(cx * 7 + cz) & 0xffff)
        rr = r * (0.75 + 0.5 * n)
        inside = d < rr
        h -= depth * smoothstep(rr, rr * 0.4, d) * inside
        surf_mud |= inside
    return surf_mud


def place_trees(h, water, routes, surf, rng, spheres, capsules, boxes):
    sl = slope_deg(h)
    pts_all = np.vstack([r["pts"] for r in routes])
    rtree = cKDTree(pts_all)
    d_route, _ = rtree.query(np.stack([X.ravel(), Z.ravel()], axis=1))
    d_route = d_route.reshape(N, N)
    elev = h
    forest = fbm(X, Z, 70, 4, 77)
    # Forest on the mid slopes, clumps in the meadow, thinning to the treeline.
    dens = smoothstep(0.42, 0.62, forest) * 0.9 + 0.06
    dens *= smoothstep(-200, -120, Z) * 0.85 + 0.15
    dens *= 1 - smoothstep(170, 205, elev)
    dens *= 1 - smoothstep(33, 42, sl)
    dens *= smoothstep(0, 1, (d_route - 6.0) / 6.0)
    dens[water > -999] = 0
    obstacles = np.array([(s[0], s[2]) for s in spheres] + [((c[0] + c[3]) / 2, (c[2] + c[5]) / 2) for c in capsules] +
                         [(b[0], b[2]) for b in boxes] + [tuple(SPAWN)])
    otree = cKDTree(obstacles)
    trees = []
    cell = 9.0
    for gz in np.arange(ORG + 6, -ORG - 6, cell):
        for gx in np.arange(ORG + 6, -ORG - 6, cell):
            p = np.array([gx + rng.uniform(-3.5, 3.5), gz + rng.uniform(-3.5, 3.5)])
            dd = float(sample(dens, p)[0])
            if rng.random() > dd:
                continue
            if otree.query(p)[0] < 4.0:
                continue
            if np.hypot(*(p - SPAWN)) < 30 or np.hypot(*(p - TRAILHEAD)) < 14:
                continue
            e = float(sample(elev, p)[0])
            low = e < 60
            roll = rng.random()
            if roll < (0.30 if low else 0.18):
                kind = 1      # larch / aspen: mustard gold
            elif low and roll < 0.40:
                kind = 2      # vine maple: orange-red
            else:
                kind = 0      # Douglas fir / hemlock: forest green
            scale = rng.uniform(0.75, 1.25) * (1.0 - 0.3 * smoothstep(120, 200, e))
            trees.append((p[0], e, p[1], kind, scale, rng.uniform(0, 2 * math.pi)))
    return trees


# ---------------------------------------------------------------------------
# Set pieces and things to find, added once the trees are placed so the
# seeded placement before them stays as it was. Each takes out the trees
# in its way; every tree's height is read again from the final ground.

PROP = {n: i for i, n in enumerate(["CABIN", "SIGN", "TABLE", "CAIRN", "TOWER", "BRIDGE", "WRECK", "ARCH", "TENT"])}
BRIDGE_HX, BRIDGE_HZ = 2.9, 7.0     # deck half width and half length (world.c draws the same)


def frame(r, k):
    """Point k of route r with its unit tangent and right-hand normal."""
    pts = r["pts"]
    tg = pts[min(len(pts) - 1, k + 2)] - pts[max(0, k - 2)]
    tg = tg / (np.linalg.norm(tg) + 1e-9)
    return pts[k], tg, np.array([tg[1], -tg[0]])


def find_spot(h, water, avoid_pts, near, radius, min_route=26.0, max_slope=12.0):
    """The gentlest dry spot within radius of near, well off every route."""
    sl = slope_deg(h)
    tree = cKDTree(avoid_pts)
    best, best_score = None, 1e9
    for dz in np.arange(-radius, radius + 1, 3.0):
        for dx in np.arange(-radius, radius + 1, 3.0):
            p = near + np.array([dx, dz])
            if abs(p[0]) > 290 or abs(p[1]) > 290:
                continue
            g = to_grid(p)
            i, j = int(round(g[0])), int(round(g[1]))
            if water[max(0, j - 3):j + 4, max(0, i - 3):i + 4].max() > -999:
                continue
            slope = float(sl[max(0, j - 2):j + 3, max(0, i - 2):i + 3].max())
            if slope > max_slope or tree.query(p)[0] < min_route:
                continue
            score = np.hypot(dx, dz) + slope * 2.0
            if score < best_score:
                best, best_score = p, score
    return best


def clear_trees(trees, p, r):
    trees[:] = [t for t in trees if np.hypot(t[0] - p[0], t[2] - p[1]) > r]


def set_pieces(h, water, surf, routes, trees, spheres, capsules, boxes, prop_boxes, rng):
    """Returns (props, views, tracks): props as (x, z, yaw, kind, y or None
    for the ground), views as (x, z, name), extra map tracks. Rock ledges go
    in boxes, props' collision boxes in prop_boxes."""
    props, views, tracks = [], [], []
    log = routes[1]
    flat = np.stack([X.ravel(), Z.ravel()], axis=1)

    def band(c, tg, nm):
        d = flat - c
        return (d @ tg).reshape(N, N), (d @ nm).reshape(N, N)

    def keep_roads(k_here, span=25):
        """1 away from roads, 0 on any road but the stretch around
        log["pts"][k_here]: a gully meant for one crossing must not cut a
        switchback's other leg."""
        pts = [r["pts"] for r in routes if r is not log] + [log["pts"][:max(0, k_here - span)],
                                                             log["pts"][k_here + span:]]
        pts = np.vstack([q for q in pts if len(q)])
        d = cKDTree(pts).query(flat)[0].reshape(N, N)
        return smoothstep(5.0, 11.0, d)

    # A washout across the logging road: a muddy gully with a trickle in it.
    c, tg, nm = frame(log, 300)
    u, v = band(c, tg, nm)
    W, D = 4.5, 1.15
    prof = np.where(np.abs(u) < W, np.cos(np.pi * u / (2 * W)) ** 2, 0.0) * smoothstep(17, 10, np.abs(v)) * keep_roads(300)
    h -= D * prof
    trickle = (np.abs(u) < 1.0) & (np.abs(v) < 14) & (prof > 0.5)
    water[trickle] = np.maximum(water[trickle], h[trickle] + 0.18)
    surf[(np.abs(u) < 2.6) & (np.abs(v) < 9) & (prof > 0.2)] = S["MUD"]
    clear_trees(trees, c, 10)

    # A ravine further up, crossed on a log bridge.
    c, tg, nm = frame(log, 820)
    y0 = float(log["y"][820])
    u, v = band(c, tg, nm)
    W, D = 5.2, 3.4
    prof = np.clip(1 - np.abs(u) / W, 0, 1) ** 0.7 * smoothstep(34, 24, np.abs(v)) * keep_roads(820)
    h[:] = np.minimum(h, h - D * prof)
    creek = (np.abs(u) < 1.4) & (np.abs(v) < 30) & (prof > 0.5)
    water[creek] = np.maximum(water[creek], h[creek] + 0.35)
    surf[(np.abs(u) < 3.5) & (np.abs(v) < 30) & (prof > 0.2)] = S["WETROCK"]
    yaw = math.atan2(tg[0], tg[1])
    prop_boxes.append((c[0], y0 + 0.06 - 0.15, c[1], BRIDGE_HX, 0.15, BRIDGE_HZ, yaw))
    for side in (-1, 1):
        e = c + nm * side * (BRIDGE_HX - 0.15)
        a, b = e - tg * BRIDGE_HZ, e + tg * BRIDGE_HZ
        capsules.append((a[0], y0 + 0.3, a[1], b[0], y0 + 0.3, b[1], 0.14))
    props.append((c[0], c[1], yaw, PROP["BRIDGE"], y0 + 0.06))
    clear_trees(trees, c, 12)

    # The shortcut: a steep rocky track from the road's first long leg
    # straight up to where it comes back round, ~480 m of road saved.
    a_k, b_k = 148, 680
    pa, pb = log["pts"][a_k], log["pts"][b_k]
    # Bent off the straight line so it climbs at about a third, not 40%.
    mid = (pa + pb) / 2 + np.array([-(pb - pa)[1], (pb - pa)[0]]) * 0.32
    pts = resample(smooth_path(np.array([pa, mid, pb]), 3), 1.0)
    ya, yb = float(log["y"][a_k]), float(log["y"][b_k])
    s_ = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))])
    t = s_ / s_[-1]
    ease = 5.0 / s_[-1]     # level off over the last few metres at each end
    y = ya + (yb - ya) * np.interp(t, [0, ease, 1 - ease, 1], [0, ease * 0.5, 1 - ease * 0.5, 1])
    sc = {"pts": pts, "y": y, "half": 2.3}
    carve_route(h, pts, y, np.full(len(pts), 2.3))
    near = cKDTree(pts).query(flat)[0].reshape(N, N) < 3.2
    surf[near] = np.where(slope_deg(h)[near] > 22, S["ROCK"], S["DIRT"])
    for k in (len(pts) // 3, 2 * len(pts) // 3):
        boxes.append(ledge(sc, k, 0.22, 2.9, rng))
    for k in range(10, len(pts) - 10, 9):
        p_, tg_, nm_ = frame(sc, k)
        q = p_ + nm_ * rng.choice([-1, 1]) * (2.6 + rng.random())
        rr = 0.4 + rng.random() * 0.5
        spheres.append((q[0], float(sample(h, q)[0]) - rr * 0.3, q[1], rr))
    for k in range(len(pts)):
        clear_trees(trees, pts[k], 6.5) if k % 3 == 0 else None
    p_, tg_, nm_ = frame(sc, 4)
    props.append(tuple(p_ + nm_ * 4.0) + (math.atan2(tg_[0], tg_[1]), PROP["SIGN"], None))
    tracks.append(pts)
    sg = round(float(np.max(np.abs(np.diff(y)) / np.maximum(np.linalg.norm(np.diff(pts, axis=0), axis=1), 1e-3))) * 100)
    print(f"  shortcut {s_[-1]:.0f} m, climbs {yb - ya:.1f} m, steepest {sg}%")

    all_pts = np.vstack([r["pts"] for r in routes] + [pts])

    # A boulder field on a rise in the meadow's west, a cairn on top.
    g = find_spot(h, water, all_pts, np.array([-130.0, -250.0]), 30, max_slope=10)
    d = np.hypot(X - g[0], Z - g[1])
    h += 2.4 * smoothstep(20, 4, d)
    surf[d < 15] = S["ROCK"]
    for _ in range(46):
        q = g + rng.normal(0, 6.0, 2)
        if np.hypot(*(q - g)) < 2.2:
            continue
        rr = 0.45 + rng.random() * 0.85
        spheres.append((q[0], float(sample(h, q)[0]) - rr * 0.4, q[1], rr))
    clear_trees(trees, g, 24)
    props.append((g[0], g[1], 0.0, PROP["CAIRN"], None))
    views.append((g[0], g[1], "BOULDER FIELD"))

    # A deep ford on the creek, away from the creek bed route, with the
    # stone arch on the far bank to cross for.
    creek_pts = resample(CREEK, 1.0)
    cr = cKDTree(routes[3]["pts"])
    far = [p_ for p_ in creek_pts[len(creek_pts) // 2:] if cr.query(p_)[0] > 40 and water[tuple(np.round(to_grid(p_)[::-1]).astype(int))] > -999]
    f = far[len(far) // 2]
    d = np.hypot(X - f[0], Z - f[1])
    pool = (d < 7) & (water > -999)
    h[pool] -= 0.3 * smoothstep(7, 3, d[pool])
    k = int(np.argmin(np.linalg.norm(creek_pts - f, axis=1)))
    ctg = creek_pts[min(k + 3, len(creek_pts) - 1)] - creek_pts[max(k - 3, 0)]
    ctg /= np.linalg.norm(ctg)
    cnm = np.array([ctg[1], -ctg[0]])
    east = cnm if cnm[0] > 0 else -cnm
    sign_p = f + east * 9.0
    props.append((sign_p[0], sign_p[1], math.atan2(east[0], east[1]), PROP["SIGN"], None))
    arch = find_spot(h, water, all_pts, f - east * 34, 16, max_slope=11)
    ayaw = math.atan2(east[0], east[1])
    for side in (-1, 1):
        q = arch + np.array([math.cos(ayaw), -math.sin(ayaw)]) * side * 3.4
        gq = float(sample(h, q)[0])
        prop_boxes.append((q[0], gq + 2.0, q[1], 0.8, 2.0, 1.0, ayaw))
    props.append((arch[0], arch[1], ayaw, PROP["ARCH"], None))
    clear_trees(trees, arch, 12)
    views.append((arch[0], arch[1], "STONE ARCH"))
    print(f"  ford at {f[0]:.0f},{f[1]:.0f}, arch at {arch[0]:.0f},{arch[1]:.0f}")

    # An old pickup left in the woods east of the logging road.
    wk = find_spot(h, water, all_pts, np.array([250.0, -120.0]), 40, min_route=30)
    wyaw = rng.uniform(0, 2 * math.pi)
    prop_boxes.append((wk[0], float(sample(h, wk)[0]) + 0.9, wk[1], 0.9, 0.9, 2.2, wyaw))
    props.append((wk[0], wk[1], wyaw, PROP["WRECK"], None))
    clear_trees(trees, wk, 10)
    views.append((wk[0], wk[1], "OLD TIMER"))

    # A camp up past the lake: a tent, a fire ring, logs to sit on.
    cp = find_spot(h, water, all_pts, np.array([-60.0, 100.0]), 60, min_route=20, max_slope=14)
    cyaw = rng.uniform(0, 2 * math.pi)
    props.append((cp[0], cp[1], cyaw, PROP["TENT"], None))
    for side in (-1, 1):
        q = cp + np.array([math.sin(cyaw), math.cos(cyaw)]) * 4.0 + np.array([math.cos(cyaw), -math.sin(cyaw)]) * side * 1.6
        gq = float(sample(h, q)[0])
        dx, dz = math.sin(cyaw) * 0.9, math.cos(cyaw) * 0.9
        capsules.append((q[0] - dx, gq + 0.18, q[1] - dz, q[0] + dx, gq + 0.18, q[1] + dz, 0.18))
    clear_trees(trees, cp, 9)
    views.append((cp[0], cp[1], "HIGH CAMP"))
    print(f"  boulder field {g[0]:.0f},{g[1]:.0f}, wreck {wk[0]:.0f},{wk[1]:.0f}, camp {cp[0]:.0f},{cp[1]:.0f}")
    return props, views, tracks


# ---------------------------------------------------------------------------
# Surfaces and colours

def surfaces(h, water, routes, mud_mask):
    sl = slope_deg(h)
    n = fbm(X, Z, 18, 3, 91)
    forest = fbm(X, Z, 70, 4, 77)
    surf = np.full((N, N), S["GRASS"], dtype=np.uint8)
    surf[(smoothstep(0.42, 0.62, forest) > 0.5) & (Z > -200)] = S["FOREST"]
    surf[(sl > 38 + 6 * n) | ((h > 172 + 16 * n) & (sl > 26))] = S["ROCK"]
    surf[(h > 178 + 14 * n) & (sl <= 26)] = S["SCREE"]
    nrm = normals(h)
    snow = (h > 190 + 10 * fbm(X, Z, 25, 3, 92)) & (nrm[..., 2] > -0.1) & (sl < 38)
    surf[snow] = S["SNOW"]
    flat = np.stack([X.ravel(), Z.ravel()], axis=1)
    for r in routes:
        tree = cKDTree(r["pts"])
        d, idx = tree.query(flat, distance_upper_bound=float(np.max(r["halfs"])) + 1.2)
        ok = np.isfinite(d)
        ok[ok] = d[ok] < r["halfs"][idx[ok]] + 1.2
        jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
        t = idx[ok] / len(r["pts"])
        name = r["name"]
        if name == "FOREST ROAD":
            v = np.full(len(jj), S["GRAVEL"])
        elif name == "LOGGING ROAD":
            v = np.where(t < 0.45, S["GRAVEL"], S["DIRT"])
        elif name == "RIDGE LINE":
            v = np.where(sl[jj, ii] > 18, S["ROCK"], S["DIRT"])
        elif name == "CREEK BED":
            v = np.where(n[jj, ii] > 0.55, S["WETROCK"], S["DIRT"])
        else:
            v = np.where(sl[jj, ii] > 20, S["ROCK"], S["SCREE"])
        surf[jj, ii] = v
    surf[mud_mask] = S["MUD"]
    # The creek's bed and banks.
    surf[(water > -999) & (water - h < 1.6) & (h > 20)] = S["WETROCK"]
    return surf


def sun_shadow(h):
    """1 lit, 0 in a hill's shadow: march from each vertex toward the sun."""
    lit = np.ones_like(h)
    d2 = math.hypot(SUN[0], SUN[2])
    step = D * 0.75
    rise = SUN[1] / d2 * step
    sx, sz = SUN[0] / d2 * step, SUN[2] / d2 * step
    gi, gj = np.meshgrid(np.arange(N, dtype=float), np.arange(N, dtype=float))
    cur = h.copy() + 0.6
    for k in range(1, 140):
        pi = gi + sx * k / D
        pj = gj + sz * k / D
        hh = map_coordinates(h, [pj, pi], order=1, mode="nearest")
        cur_k = h + 0.6 + rise * k
        lit = np.minimum(lit, np.clip((cur_k - hh) / 2.5 + 1.0, 0, 1))
    return gaussian_filter(lit, 1.0)


def colours(h, water, surf):
    nrm = normals(h)
    lam = np.clip(np.einsum("jik,k->ji", nrm, SUN), 0, 1)
    sh = sun_shadow(h)
    ao = np.clip(1.0 + (h - gaussian_filter(h, 4)) * 0.06, 0.72, 1.08)
    n1 = fbm(X, Z, 9, 3, 101)
    n2 = fbm(X, Z, 40, 3, 102)
    alb = np.zeros((N, N, 3))
    for name, idx in S.items():
        alb[surf == idx] = ALBEDO[name]
    # Soft blends at the edges between surfaces (the grid's colours are
    # interpolated anyway), and painterly variation in hue and value.
    alb = np.dstack([gaussian_filter(alb[..., c], 0.7) for c in range(3)])
    grass = (surf == S["GRASS"])
    tint = np.dstack([np.full((N, N), 1.0), 0.94 + 0.12 * n2, 0.85 + 0.3 * n2])
    alb = np.where(grass[..., None], alb * tint, alb)
    alb *= (0.9 + 0.2 * n1)[..., None]
    sky = np.array([0.62, 0.66, 0.72])
    sun = np.array([1.0, 0.93, 0.80])
    light = sky[None, None, :] * (0.55 + 0.45 * nrm[..., 1:2]) + sun[None, None, :] * (lam * sh)[..., None] * 0.62
    light *= ao[..., None]
    rgb = alb * light
    # Under the water, the bed tints toward the water's colour.
    deep = np.clip((water - h) / 2.5, 0, 1)
    rgb = rgb * (1 - 0.5 * deep[..., None]) + np.array([52, 92, 98]) * 0.5 * deep[..., None]
    rgb = np.clip(rgb, 0, 255).astype(np.uint32)
    argb = (0xFF << 24) | (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]
    return argb.astype(np.uint32), rgb


# ---------------------------------------------------------------------------
# Output

def write_header(path, routes, spheres, capsules, boxes, rock_boxes, trees, props, views, spawns, lake_level, summit_y, start_y):
    L = []
    w = L.append
    w("/* Generated by tools/build_level.py. Do not edit. */")
    w("#ifndef RL_LEVEL_H\n#define RL_LEVEL_H\n#include <stdint.h>\n")
    w(f"#define LEVEL_N {N}\n#define LEVEL_D {D:.1f}f\n#define LEVEL_ORG {ORG:.1f}f\n#define LEVEL_SIZE {SIZE:.1f}f")
    w("/* level.bin: {f32 height, u32 argb}[N*N] (side by side: drawing the ground reads both),")
    w("   f32 water[N*N] (-1000: none), u8 surface[N*N]. */")
    w("#define LEVEL_BIN_GROUND 0")
    w(f"#define LEVEL_BIN_WATER {N * N * 8}")
    w(f"#define LEVEL_BIN_SURF {N * N * 12}")
    w("enum { " + ", ".join(f"SURF_{n}" for n in SURFACES) + ", SURF_COUNT };")
    w(f"#define LEVEL_LAKE_Y {lake_level:.2f}f\n#define LEVEL_START_Y {start_y:.2f}f\n#define LEVEL_SUMMIT_Y {summit_y:.2f}f")
    w(f"#define LEVEL_SUMMIT_X {PEAK[0]:.1f}f\n#define LEVEL_SUMMIT_Z {PEAK[1]:.1f}f\n")
    # Routes: points every ~4 m with heights, for the AI, the map and progress.
    w("typedef struct { float x, y, z; } lvl_pt_t;")
    w("typedef struct { const char *name; const lvl_pt_t *pts; uint16_t n; uint8_t difficulty; float half; } lvl_route_t;")
    for k, r in enumerate(routes):
        pts = r["pts"][::4]
        ys = r["y"][::4]
        w(f"static const lvl_pt_t lvl_route{k}[{len(pts)}] = {{")
        for row in range(0, len(pts), 4):
            w("    " + " ".join(f"{{{p[0]:.1f}f, {y:.2f}f, {p[1]:.1f}f}}," for p, y in zip(pts[row:row + 4], ys[row:row + 4])))
        w("};")
    diffs = {"FOREST ROAD": 0, "LOGGING ROAD": 1, "CREEK BED": 2, "RIDGE LINE": 3, "SUMMIT TRAIL": 3}
    w(f"#define LVL_ROUTES {len(routes)}")
    w("static const lvl_route_t lvl_routes[LVL_ROUTES] = {")
    for k, r in enumerate(routes):
        w(f'    {{"{r["name"]}", lvl_route{k}, {len(r["pts"][::4])}, {diffs[r["name"]]}, {r["half"]:.2f}f}},')
    w("};\n")
    w("/* Obstacles the trucks collide with. Spheres (boulders): centre, radius. */")
    w("typedef struct { float x, y, z, r; } lvl_sphere_t;")
    w("/* Capsules (logs): end points, radius. */")
    w("typedef struct { float ax, ay, az, bx, by, bz, r; } lvl_capsule_t;")
    w("/* Boxes (rock ledges, buildings): centre, half extents, yaw about Y. */")
    w("typedef struct { float x, y, z, hx, hy, hz, yaw; } lvl_box_t;")
    w(f"#define LVL_SPHERES {len(spheres)}\nstatic const lvl_sphere_t lvl_spheres[LVL_SPHERES] = {{")
    for s in spheres:
        w(f"    {{{s[0]:.2f}f, {s[1]:.2f}f, {s[2]:.2f}f, {s[3]:.2f}f}},")
    w("};")
    w(f"#define LVL_CAPSULES {len(capsules)}\nstatic const lvl_capsule_t lvl_capsules[LVL_CAPSULES] = {{")
    for c in capsules:
        w("    {" + ", ".join(f"{v:.2f}f" for v in c) + "},")
    w("};")
    w("/* The first LVL_ROCK_BOXES are rock ledges, drawn as rock; the rest are props' (a cabin, a bridge deck). */")
    w(f"#define LVL_ROCK_BOXES {rock_boxes}")
    w(f"#define LVL_BOXES {len(boxes)}\nstatic const lvl_box_t lvl_boxes[LVL_BOXES] = {{")
    for b in boxes:
        w("    {" + ", ".join(f"{v:.3f}f" for v in b) + "},")
    w("};\n")
    w("/* Trees: base, kind (0 fir, 1 larch, 2 maple), scale, turn. */")
    w("typedef struct { float x, y, z; uint8_t kind; uint8_t pad[3]; float scale, yaw; } lvl_tree_t;")
    w(f"#define LVL_TREES {len(trees)}\nstatic const lvl_tree_t lvl_trees[LVL_TREES] = {{")
    for t in trees:
        w(f"    {{{t[0]:.1f}f, {t[1]:.2f}f, {t[2]:.1f}f, {t[3]}, {{0}}, {t[4]:.2f}f, {t[5]:.2f}f}},")
    w("};\n")
    w("enum { " + ", ".join(f"PROP_{n}" for n in PROP) + ", PROP_KIND_COUNT };")
    w(f"#define LVL_BRIDGE_HX {BRIDGE_HX:.2f}f\n#define LVL_BRIDGE_HZ {BRIDGE_HZ:.2f}f")
    w("typedef struct { float x, y, z, yaw; uint8_t kind; uint8_t pad[3]; } lvl_prop_t;")
    w(f"#define LVL_PROPS {len(props)}\nstatic const lvl_prop_t lvl_props[LVL_PROPS] = {{")
    for p in props:
        w(f"    {{{p[0]:.1f}f, {p[1]:.2f}f, {p[2]:.1f}f, {p[3]:.3f}f, {p[4]}, {{0}}}},")
    w("};\n")
    w("/* Viewpoints to find: where, and the name shown. */")
    w("typedef struct { float x, y, z; const char *name; } lvl_view_t;")
    w(f"#define LVL_VIEWS {len(views)}\nstatic const lvl_view_t lvl_views[LVL_VIEWS] = {{")
    for v in views:
        w(f'    {{{v[0]:.1f}f, {v[1]:.2f}f, {v[2]:.1f}f, "{v[3]}"}},')
    w("};\n")
    w("/* Start grid: position and heading (radians, 0 = north). */")
    w(f"#define LVL_SPAWNS {len(spawns)}\nstatic const float lvl_spawns[LVL_SPAWNS][4] = {{")
    for s in spawns:
        w(f"    {{{s[0]:.1f}f, {s[1]:.2f}f, {s[2]:.1f}f, {s[3]:.3f}f}},")
    w("};\n\n#endif")
    path.write_text("\n".join(L) + "\n")


def preview_image(rgb, water, routes, spheres, capsules, boxes, trees, views, path):
    img = Image.fromarray(np.flipud(rgb.astype(np.uint8)), "RGB").resize((N * 3, N * 3), Image.NEAREST)
    d = ImageDraw.Draw(img)

    def px(x, z):
        return ((x - ORG) / D * 3, (N - 1 - (z - ORG) / D) * 3)
    cols = [(255, 255, 255), (60, 140, 255), (255, 60, 60), (40, 220, 220), (255, 120, 0)]
    for k, r in enumerate(routes):
        d.line([px(p[0], p[1]) for p in r["pts"][::2]], fill=cols[k % len(cols)], width=2)
    for t in trees[::1]:
        c = [(20, 80, 30), (250, 210, 40), (230, 90, 30)][t[3]]
        x, y = px(t[0], t[2])
        d.point((x, y), fill=c)
    for s in spheres:
        x, y = px(s[0], s[2])
        d.ellipse([x - 1, y - 1, x + 1, y + 1], fill=(80, 80, 80))
    for c in capsules:
        d.line([px(c[0], c[2]), px(c[3], c[5])], fill=(120, 70, 30), width=2)
    for b in boxes:
        x, y = px(b[0], b[2])
        d.rectangle([x - 1, y - 1, x + 1, y + 1], fill=(200, 0, 200))
    for v in views:
        x, y = px(v[0], v[2])
        d.ellipse([x - 4, y - 4, x + 4, y + 4], outline=(255, 0, 255), width=2)
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)


def minimap(rgb, water, routes, tracks, path):
    """256 x 256: the terrain's colours, lightened like a park map, with the
    routes drawn on."""
    img = Image.fromarray(np.flipud(rgb.astype(np.uint8)), "RGB").resize((512, 512), Image.LANCZOS)
    a = np.asarray(img).astype(float)
    a = a * 0.75 + np.array([246, 238, 214]) * 0.25
    img = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), "RGB")
    d = ImageDraw.Draw(img)

    def px(x, z):
        return ((x - ORG) / SIZE * 512, (1 - (z - ORG) / SIZE) * 512)
    for r in routes:
        d.line([px(p[0], p[1]) for p in r["pts"][::3]], fill=(90, 50, 30), width=6)
        d.line([px(p[0], p[1]) for p in r["pts"][::3]], fill=(250, 240, 210), width=2)
    for t in tracks:
        d.line([px(p[0], p[1]) for p in t[::3]], fill=(110, 70, 40), width=3)
    img = img.resize((256, 256), Image.LANCZOS)
    img.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--preview", type=Path, default=None)
    args = ap.parse_args()
    rng = np.random.default_rng(2026)

    h = base_terrain()
    bench(h, np.array([-18.0, 54.0]), 46.0, 34.0)
    water, lake_level = carve_water(h)
    routes = build_routes(h, water)
    for r in routes:
        r["turn"] = turniness(r["pts"])
        r["y"] = route_profile(h, r["pts"], r["gmax"], turn=r["turn"])
        r["halfs"] = r["half"] * (1.0 + 0.9 * r["turn"])
    # Where routes meet, they meet at one height: each climb starts where
    # the forest road ends, the summit trail where the climbs end.
    def pin(r, k_end, y_to, span=18.0):
        s_ = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(r["pts"], axis=0), axis=1))])
        dist = s_ if k_end == 0 else s_[-1] - s_
        k = 0 if k_end == 0 else -1
        r["y"] += (y_to - r["y"][k]) * np.clip(1 - dist / span, 0, 1)
    for r in routes[1:4]:
        pin(r, 0, routes[0]["y"][-1])
    meet = float(np.mean([r["y"][-1] for r in routes[1:4]]))
    for r in routes[1:4]:
        pin(r, -1, meet)
    pin(routes[4], 0, meet)
    for r in routes:
        carve_route(h, r["pts"], r["y"], r["halfs"])
    spheres, capsules, boxes, mud = place_obstacles(h, water, routes, rng)
    mud_mask = apply_mud(h, mud)
    # The creek stays wadeable (a route carved down into its bed would
    # leave a hole deeper than a truck's air intake); the lake and the pond
    # keep their depth.
    still = (np.hypot(X - LAKE_C[0], Z - LAKE_C[1]) < LAKE_R + 4) | (np.hypot(X - POND_C[0], Z - POND_C[1]) < POND_R + 4)
    creek = (water > -999) & ~still
    h[creek] = np.maximum(h[creek], water[creek] - 0.5)
    water[water < h + 0.05] = -1000.0
    surf = surfaces(h, water, routes, mud_mask)
    trees = place_trees(h, water, routes, surf, rng, spheres, capsules, boxes)
    prop_boxes = []
    extra_props, extra_views, tracks = set_pieces(h, water, surf, routes, trees, spheres, capsules, boxes, prop_boxes,
                                                  np.random.default_rng(77))
    rock_boxes = len(boxes)
    boxes += prop_boxes
    water[water < h + 0.05] = -1000.0
    trees = [(t[0], float(sample(h, np.array([t[0], t[2]]))[0]), t[2], t[3], t[4], t[5]) for t in trees]
    argb, rgb = colours(h, water, surf)

    def gy(x, z):
        return float(sample(h, np.array([x, z]))[0])
    # Props: the ranger cabin and a picnic table by the start, signs at the
    # trailhead and the fork, cairns at the viewpoints, the summit's tower.
    props = [(-28, gy(-28, -292), -292, 0.3, 0), (-14, gy(-14, -276), -276, 1.2, 2),
             (TRAILHEAD[0] + 5, gy(TRAILHEAD[0] + 5, TRAILHEAD[1]), TRAILHEAD[1], 0.0, 1),
             (FORK[0] - 6, gy(FORK[0] - 6, FORK[1]), FORK[1], 0.4, 1),
             (HIGH_MEADOW[0] + 6, gy(HIGH_MEADOW[0] + 6, HIGH_MEADOW[1]), HIGH_MEADOW[1], 0.8, 1)]
    boxes.append((-28.0, gy(-28, -292) + 1.6, -292.0, 4.2, 1.8, 3.2, 0.3))   # the cabin
    summit_y = gy(PEAK[0], PEAK[1])
    props.append((PEAK[0], summit_y, PEAK[1], 0.2, 4))
    for (px_, pz_, yaw_, kind_, y_) in extra_props:
        props.append((px_, gy(px_, pz_) if y_ is None else y_, pz_, yaw_, kind_))
    for lx, lz in ((-1.8, -1.8), (1.8, -1.8), (-1.8, 1.8), (1.8, 1.8)):   # the tower's legs, room to drive under
        # Wider than drawn: the body's contact points are ~1 m apart and
        # would slip past a post as thin as the leg.
        boxes.append((PEAK[0] + lx, gy(PEAK[0] + lx, PEAK[1] + lz) + 3.0, PEAK[1] + lz, 0.45, 3.0, 0.45, 0.0))
    # Viewpoints along each route, high on it, where the view opens out.
    views = []
    names = {"LOGGING ROAD": "EAGLE BLUFF", "RIDGE LINE": "KNIFE EDGE", "CREEK BED": "CEDAR FALLS",
             "SUMMIT TRAIL": "SUMMIT", "FOREST ROAD": "FIR HOLLOW"}
    for r in routes:
        if r["name"] == "SUMMIT TRAIL":
            p = r["pts"][-1]
        else:
            k = int(len(r["pts"]) * 0.82)
            p = r["pts"][k] + np.array([0.0, 0.0])
        views.append((p[0], gy(p[0], p[1]), p[1], names[r["name"]]))
    for v in views[:-1]:
        props.append((v[0] + 4.0, gy(v[0] + 4.0, v[2]), v[2], 0.0, 3))
    views.append((LAKE_C[0] + LAKE_R + 6, gy(LAKE_C[0] + LAKE_R + 6, LAKE_C[1]), LAKE_C[1], "MIRROR LAKE"))
    for (vx, vz, name) in extra_views:
        views.append((vx, gy(vx, vz), vz, name))
    spawns = []
    for k in range(4):
        sx = SPAWN[0] + (k - 1.5) * 6.0
        sz = SPAWN[1] - abs(k - 1.5) * 2.0
        spawns.append((sx, gy(sx, sz), sz, 0.0))

    args.out.mkdir(parents=True, exist_ok=True)
    surf_bytes = surf.astype(np.uint8).tobytes()
    surf_bytes += b"\0" * ((-len(surf_bytes)) % 4)
    ground = np.zeros((N * N, 2), dtype="<u4")
    ground[:, 0] = h.astype("<f4").ravel().view("<u4")
    ground[:, 1] = argb.ravel()
    blob = ground.tobytes() + water.astype("<f4").tobytes() + surf_bytes
    (args.out / "level.bin").write_bytes(blob)
    write_header(args.out / "level.h", routes, spheres, capsules, boxes, rock_boxes, trees, props, views, spawns, lake_level,
                 summit_y, gy(SPAWN[0], SPAWN[1]))
    minimap(rgb, water, routes, tracks, args.out / "level_map.png")
    if args.preview:
        preview_image(rgb, water, routes, spheres, capsules, boxes, trees, views, args.preview)
    print(f"heights {h.min():.1f}..{h.max():.1f} m, lake {lake_level:.1f} m, summit {summit_y:.1f} m")
    for r in routes:
        seg = np.linalg.norm(np.diff(r["pts"], axis=0), axis=1)
        grade = np.abs(np.diff(r["y"])) / np.maximum(seg, 1e-3)
        print(f"  {r['name']:13s} {seg.sum():6.0f} m, climbs {r['y'][-1] - r['y'][0]:6.1f} m, "
              f"grade p95 {np.percentile(grade, 95) * 100:4.0f}% max {grade.max() * 100:4.0f}%")
    print(f"  {len(spheres)} boulders, {len(capsules)} logs, {len(boxes)} boxes, {len(trees)} trees, "
          f"{len(views)} viewpoints, level.bin {len(blob)} bytes")


if __name__ == "__main__":
    main()
