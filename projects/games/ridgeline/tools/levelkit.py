"""Shared pieces of Ridgeline's level generators (build_level.py): the grid,
noise, route search and carving, obstacles' helpers, lighting inputs, the
packed grid and the generated header.

World: +X east, +Y up, +Z north, metres.
"""

from __future__ import annotations

import heapq
import math
import struct
from pathlib import Path

import lz4.block
import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter, map_coordinates
from scipy.spatial import cKDTree

N = 257                  # vertices per side
D = 3.0                  # metres between vertices
ORG = -384.0             # world x/z of vertex 0
SIZE = D * (N - 1)       # 768 m

# Surfaces: friction, rolling resistance, softness and colour live in the game.
SURFACES = ["GRASS", "FOREST", "DIRT", "GRAVEL", "ROCK", "MUD", "SCREE", "SNOW", "WETROCK", "SAND", "SLICKROCK"]
S = {n: i for i, n in enumerate(SURFACES)}

SUN = np.array([-0.803, 0.375, 0.464])   # a late sun in the west-north-west (world.c palette)
SUN = SUN / np.linalg.norm(SUN)
# The horizon map: bearings (degrees from north) the sun's arc covers, and
# the half-resolution grid it is sampled on.
HZ_AZ0, HZ_STEP, HZ_DIRS = 45, 15, 21     # 45 .. 345
HZ_N = (N - 1) // 2 + 1
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
        # Wading is water_cost; deeper than a truck's intake is never cheap
        # (the creek bed once took a short cut through the lake's edge).
        deep = (water > -999) & (water - h > 0.7)
        extra = extra + np.where(deep, max(water_cost, 60.0), np.where(water > -999, water_cost, 0.0))
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


def own(r):
    """How many of route r's points are its own (the rest follow another)."""
    return r.get("own", len(r["pts"]))


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


def off_line(r, k, off, half):
    """A boulder's offset across route r at point k, moved out to the edge
    on a hairpin: rocks across a tight climbing turn knocked trucks off
    the line and down the bank (the creek bed by the lake)."""
    if r["turn"][min(k, len(r["turn"]) - 1)] > 0.3 and abs(off) < half + 0.6:
        return math.copysign(half + 0.6, off if off != 0 else 1.0)
    return off


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


# ---------------------------------------------------------------------------
# Set pieces and things to find, added once the trees are placed so the
# seeded placement before them stays as it was. Each takes out the trees
# in its way; every tree's height is read again from the final ground.

PROP = {n: i for i, n in enumerate(["CABIN", "SIGN", "TABLE", "CAIRN", "TOWER", "BRIDGE", "WRECK", "ARCH", "TENT",
                                    "KIOSK", "RAMADA", "PANEL", "TRACKS", "ADIT", "PLATE"])}
BRIDGE_HX, BRIDGE_HZ = 2.9, 7.0     # deck half width and half length (world.c draws the same)


def frame(r, k):
    """Point k of route r with its unit tangent and right-hand normal."""
    pts = r["pts"]
    tg = pts[min(len(pts) - 1, k + 2)] - pts[max(0, k - 2)]
    tg = tg / (np.linalg.norm(tg) + 1e-9)
    return pts[k], tg, np.array([tg[1], -tg[0]])


def find_spot(h, water, avoid_pts, near, radius, min_route=26.0, max_slope=12.0):
    """The gentlest dry spot within radius of near, well off every route;
    the search widens and the limits ease off until there is one."""
    for k in range(6):
        p = _find_spot(h, water, avoid_pts, near, radius * (1 + 0.5 * k), max(12.0, min_route - 3 * k), max_slope + 2 * k)
        if p is not None:
            return p
    raise RuntimeError(f"no spot near {near}")


def _find_spot(h, water, avoid_pts, near, radius, min_route, max_slope):
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


def horizon_map(h):
    """The skyline's elevation (degrees, as u8 half-degrees) in HZ_DIRS
    bearings from every other vertex: the sun is in a hill's shadow where it
    sits below that. Marched out to 600 m."""
    gi, gj = np.meshgrid(np.arange(0, N, 2, dtype=float), np.arange(0, N, 2, dtype=float))
    h0 = h[::2, ::2] + 0.5
    out = np.zeros((HZ_DIRS, HZ_N, HZ_N), dtype=np.uint8)
    for d in range(HZ_DIRS):
        az = math.radians(HZ_AZ0 + d * HZ_STEP)
        sx, sz = math.sin(az), math.cos(az)
        best = np.full(h0.shape, -90.0)
        for k in range(1, 201):
            dist = 3.0 * k
            hh = map_coordinates(h, [gj + sz * dist / D, gi + sx * dist / D], order=1, mode="nearest")
            best = np.maximum(best, np.degrees(np.arctan2(hh - h0, dist)))
        out[d] = np.clip(np.round(best * 2), 0, 255).astype(np.uint8)
    return out


def light_inputs(alb_ao, nrm, h, water, flags=None):
    """Per vertex, 8 bytes: albedo with the ambient occlusion folded in
    (u8 rgb), the normal's x and z (s8, y is recovered), how far under
    water it lies (u8, 0..1 over 2.5 m), flags (1: on a cliff's face, so
    the far ground's coarser grids take their colour from it), one spare."""
    rec = np.zeros((N * N, 8), dtype=np.uint8)
    rec[:, 0:3] = np.clip(alb_ao, 0, 255).reshape(-1, 3).astype(np.uint8)
    rec[:, 3] = np.clip(np.round(nrm[..., 0] * 127), -127, 127).astype(np.int8).ravel().view(np.uint8)
    rec[:, 4] = np.clip(np.round(nrm[..., 2] * 127), -127, 127).astype(np.int8).ravel().view(np.uint8)
    deep = np.clip((water - h) / 2.5, 0, 1)
    rec[:, 5] = np.round(deep * 255).astype(np.uint8).ravel()
    if flags is not None:
        rec[:, 6] = flags.ravel()
    return rec.tobytes()


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


# ---------------------------------------------------------------------------
# Output: the packed grid and the generated header (src/level.h, src/level.c)

H_BASE = -32.0           # heights are stored as u16 steps of H_STEP above this
H_STEP = 1.0 / 128.0
ROCK_FLAG = {"SMOOTH": 1, "CAP": 2, "DARK": 4}


def _plane_residual(a):
    """What is left of a after predicting each value from its left, upper
    and upper-left neighbours (a plane through them), over the last two
    axes."""
    a = a.astype(np.int64)
    p = a.copy()
    p[..., 1:, 1:] = a[..., 1:, 1:] - (a[..., 1:, :-1] + a[..., :-1, 1:] - a[..., :-1, :-1])
    p[..., 0, 1:] = a[..., 0, 1:] - a[..., 0, :-1]
    p[..., 1:, 0] = a[..., 1:, 0] - a[..., :-1, 0]
    return p


def _lz(data):
    return lz4.block.compress(bytes(data), mode="high_compression", compression=12, store_size=False)


def _u16_planes(q):
    """A grid of u16s as plane residuals, zigzagged, low bytes then high."""
    r = _plane_residual(q)
    assert np.abs(r).max() < 32768
    zz = ((r << 1) ^ (r >> 63)).astype("<u2").reshape(-1)
    return zz.view(np.uint8).reshape(-1, 2).T.copy().tobytes()


def quantise(h):
    q = np.round((h - H_BASE) / H_STEP)
    assert q.min() >= 0 and q.max() < 65535, f"heights {h.min():.1f}..{h.max():.1f} m outside the packed range"
    return q.astype(np.int64)


def pack_level(h, water, surf, light, hz):
    """The grid as level.c unpacks it: "RLVL", version, the heights' base
    and step, the five sections' packed sizes, then each as an LZ4 block
    (padded to 4 bytes): heights and the water's surface as u16 planes
    (water 0: none), the surface ids, the relighting inputs as seven planes
    of residuals (r, g, b, nx, nz, deep, flags) and the horizon map's."""
    qh = quantise(h)
    wet = water > -999
    qw = np.where(wet, quantise(np.where(wet, water, 0.0)) + 1, 0)
    rec = np.frombuffer(light, dtype=np.uint8).reshape(N, N, 8)
    planes = np.stack([rec[..., k] for k in range(7)])
    sections = [
        _u16_planes(qh),
        _u16_planes(qw),
        surf.astype(np.uint8).tobytes(),
        (_plane_residual(planes) & 255).astype(np.uint8).tobytes(),
        (_plane_residual(hz) & 255).astype(np.uint8).tobytes(),
    ]
    packed = [_lz(sec) for sec in sections]
    out = b"RLVL" + struct.pack("<Iff", 1, H_BASE, H_STEP) + struct.pack("<5I", *[len(p) for p in packed])
    for p in packed:
        out += p + b"\0" * ((-len(p)) % 4)
    return out


def raw_level(h, argb, water, surf, light, hz):
    """The grid unpacked, as the game holds it (and as level.bin was before
    it was packed): for checking a generator change against an old build."""
    surf_bytes = surf.astype(np.uint8).tobytes()
    surf_bytes += b"\0" * ((-len(surf_bytes)) % 4)
    ground = np.zeros((N * N, 2), dtype="<u4")
    ground[:, 0] = h.astype("<f4").ravel().view("<u4")
    ground[:, 1] = argb.ravel()
    return ground.tobytes() + water.astype("<f4").tobytes() + surf_bytes + light + hz.tobytes()


def write_header(path, area, lv):
    """level_<area>.h: lv's lists as static tables and the RlLevel that
    points at them (src/level.c includes it). lv holds name, blurb,
    base_ft, lake_y, start_y, summit (x, y, z), routes, spheres, capsules,
    boxes, rock_boxes, posts, trees, props, views, spawns, mists, hawks,
    herds and rocks (each a dict: rings [(x, y, z, ra, rb)], sides, flags,
    seed)."""
    a = area
    L = []
    w = L.append
    w("/* Generated by tools/build_level.py. Do not edit. */")
    routes = lv["routes"]
    assert len(routes) == 5, "five routes: the approach, three ways up, the summit trail"
    for k, r in enumerate(routes):
        pts = r["pts"][::4]
        ys = r["y"][::4]
        w(f"static const lvl_pt_t {a}_route{k}[{len(pts)}] = {{")
        for row in range(0, len(pts), 4):
            w("    " + " ".join(f"{{{p[0]:.1f}f, {y:.2f}f, {p[1]:.1f}f}}," for p, y in zip(pts[row:row + 4], ys[row:row + 4])))
        w("};")
    w(f"static const lvl_route_t {a}_routes[{len(routes)}] = {{")
    for k, r in enumerate(routes):
        w(f'    {{"{r["name"]}", "{r["short"]}", {a}_route{k}, {len(r["pts"][::4])}, {r["difficulty"]}, {r["half"]:.2f}f}},')
    w("};\n")

    def table(ctype, name, rows, fmt, limit=None):
        assert limit is None or len(rows) <= limit, f"{name}: {len(rows)} is over the game's {limit}"
        if not rows:
            w(f"#define {a}_{name} NULL")
            return
        w(f"static const {ctype} {a}_{name}[{len(rows)}] = {{")
        for row in rows:
            w("    " + fmt(row) + ",")
        w("};")

    def floats(prec):
        return lambda row: "{" + ", ".join(f"{v:.{prec}f}f" for v in row) + "}"
    table("lvl_sphere_t", "spheres", lv["spheres"], floats(2), 8191)
    table("lvl_capsule_t", "capsules", lv["capsules"], floats(2), 8191)
    table("lvl_box_t", "boxes", lv["boxes"], floats(3), 96)
    table("lvl_post_t", "posts", lv["posts"], floats(3))
    table("lvl_tree_t", "trees", lv["trees"],
          lambda t: f"{{{t[0]:.1f}f, {t[1]:.2f}f, {t[2]:.1f}f, {t[3]}, {{0}}, {t[4]:.2f}f, {t[5]:.2f}f}}", 2048)
    table("lvl_prop_t", "props", lv["props"],
          lambda p: f"{{{p[0]:.1f}f, {p[1]:.2f}f, {p[2]:.1f}f, {p[3]:.3f}f, {p[4]}, {p[5] if len(p) > 5 else 0}, {{0}}}}", 48)
    table("lvl_view_t", "views", lv["views"], lambda v: f'{{{v[0]:.1f}f, {v[1]:.2f}f, {v[2]:.1f}f, "{v[3]}"}}', 16)
    w(f"static const float {a}_spawns[{len(lv['spawns'])}][4] = {{")
    for s_ in lv["spawns"]:
        w(f"    {{{s_[0]:.1f}f, {s_[1]:.2f}f, {s_[2]:.1f}f, {s_[3]:.3f}f}},")
    w("};")
    table("lvl_mist_t", "mists", lv["mists"], floats(1))
    table("lvl_hawk_t", "hawks", lv["hawks"], floats(1))
    if lv["herds"]:
        w(f"static const float {a}_herds[{len(lv['herds'])}][2] = {{")
        for q in lv["herds"]:
            w(f"    {{{q[0]:.1f}f, {q[1]:.1f}f}},")
        w("};")
    else:
        w(f"#define {a}_herds NULL")
    rings, rocks = [], []
    for rk in lv["rocks"]:
        flags = 0
        for f in rk.get("flags", ()):
            flags |= ROCK_FLAG[f]
        rocks.append((len(rings), len(rk["rings"]), rk.get("sides", 8), flags, rk["seed"] & 0xffff))
        rings += list(rk["rings"])
    assert len(rings) < 65536
    table("lvl_ring_t", "rings", rings, floats(2))
    table("lvl_rock_t", "rocks", rocks, lambda r: "{" + ", ".join(str(v) for v in r) + "}")
    sx, sy, sz = lv["summit"]
    w(f"\nstatic const RlLevel rl_level_{a} = {{")
    w(f'    .name = "{lv["name"]}", .blurb = "{lv["blurb"]}", .base_ft = {lv["base_ft"]},')
    w(f"    .lake_y = {lv['lake_y']:.2f}f, .start_y = {lv['start_y']:.2f}f,")
    w(f"    .summit_x = {sx:.1f}f, .summit_y = {sy:.2f}f, .summit_z = {sz:.1f}f,")
    names = ["routes", "spheres", "capsules", "boxes", "posts", "trees", "props", "views", "spawns", "mists", "hawks",
             "herds", "rings", "rocks"]
    w("    " + " ".join(f".{n} = {a}_{n}," for n in names))
    counts = {n: len(lv[n]) for n in names if n not in ("rings", "rocks")}
    counts["rings"], counts["rocks"] = len(rings), len(rocks)
    w("    " + " ".join(f".n{n} = {counts[n]}," for n in names) + f" .nrock_boxes = {lv['rock_boxes']},")
    w("};")
    path.write_text("\n".join(L) + "\n")
