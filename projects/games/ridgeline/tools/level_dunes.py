"""Ridgeline's third area: Moses Lake, sand dunes on the Columbia Basin.

A dune field of grey basalt sand between a reservoir full of drowned
dunes and a rim of basalt, as it is south of Moses Lake in Washington.
The wind is the game's, out of the south-west, and it has shaped
everything: every dune climbs gently from the south-west to a crest and
drops down a slip face at the angle sand rests at to the north-east;
barchans keep their horns downwind, parabolic dunes trail their arms
upwind where brush has pinned them, and the big dune at the top has
climbed the rimrock and stands on the bench above it.

Laid out by hand, as Mesa Grande is: the shore, the rim and every big
dune are lines drawn here. From the flats by the water three ways cross
the dune field to the rimrock and meet on it:

  shore road   east: the firm damp sand along the inlet, a dike across
               the marsh, a graded track up through sage and old dunes
  powerline    centre: the service track under the wires, down the
               troughs and over the saddles, soft on every climb
  razorback    west: straight into the open sand: up the big dunes, along
               a crest, through a bowl, deep sand most of the way

and one line climbs the big dune from there to its crest.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from scipy.ndimage import gaussian_filter, gaussian_filter1d, grey_opening
from scipy.spatial import cKDTree

from levelkit import *  # noqa: F401,F403
import level_view
import outland as ol

# ---------------------------------------------------------------------------
# The plan (x east, z north, metres)

WIND = np.array([0.78, 0.62])
WIND = WIND / np.linalg.norm(WIND)          # the game's (world.h): toward the north-east
CREST = np.array([-WIND[1], WIND[0]])       # along a crest, to the north-west
REPOSE = math.tan(math.radians(32.0))       # dry sand's angle of rest

SPAWN = np.array([0.0, -276.0])
FORK = np.array([8.0, -198.0])
MEET = np.array([-16.0, 116.0])
SUMMIT = np.array([96.0, 236.0])

LAKE_Y = 0.0
FLATS_Y = 1.6
BENCH_Y = 31.0

# The shore, west to east and then north up the inlet: water to its right.
SHORE = [(-470, -326), (-384, -324), (-330, -332), (-282, -322), (-236, -310), (-190, -316), (-150, -328),
         (-104, -322), (-62, -316), (-24, -320), (20, -318), (62, -324), (108, -330), (150, -320), (196, -314),
         (240, -322), (282, -318), (318, -300), (338, -268), (332, -230), (344, -196), (336, -160), (348, -124),
         (340, -92), (352, -66), (384, -52), (470, -40)]
# Drowned dunes: islands in the lake (x, z, half-length along the crest, height).
ISLANDS = [(-62, -353, 26, 3.4), (146, -360, 22, 3.0), (236, -352, 16, 2.4), (-204, -358, 24, 3.2),
           (-300, -366, 14, 2.0), (52, -370, 12, 1.8), (-128, -372, 12, 1.6), (322, -362, 14, 2.0)]
# The bar out to the first of them: wadeable.
BAR = [(-36, -322), (-44, -334), (-54, -344)]

# The rimrock, west to east: the bench is to its left (north).
RIM = [(-470, 196), (-384, 190), (-300, 182), (-210, 168), (-136, 150), (-84, 122), (-34, 102), (20, 96),
       (64, 80), (110, 70), (150, 46), (190, 34), (232, 10), (280, -2), (330, -28), (384, -50), (470, -66)]

# The dune field's ridges, each drawn from its south-east end to its
# north-west along the crest: (x, z, height). They lie across the wind a
# hundred metres apart, sand piled into peaks with saddles between, and
# the troughs between them are the ways through.
RIDGES = [
    # Z: the far west.
    [(-196, -222, 0), (-222, -188, 7.5), (-250, -152, 12.5), (-286, -112, 14.5), (-318, -70, 10), (-352, -24, 12),
     (-384, 14, 7.5), (-420, 60, 4)],
    # A: its crest is the razorback.
    [(-66, -220, 0), (-88, -192, 8), (-116, -160, 12), (-150, -124, 13.5), (-190, -80, 9.5), (-232, -32, 16),
     (-268, 12, 13), (-300, 52, 9), (-336, 96, 11), (-384, 150, 6)],
    # B: the biggest of them.
    [(26, -168, 0), (8, -138, 5.5), (-18, -106, 12.5), (-58, -60, 17), (-104, -8, 20.5), (-140, 36, 12.5),
     (-180, 84, 18), (-222, 136, 12.5), (-262, 184, 7.5), (-300, 236, 2.5)],
    # C.
    [(118, -136, 0), (94, -102, 8), (66, -66, 11), (36, -28, 12), (10, 6, 5.5), (-14, 36, 10), (-40, 66, 6), (-56, 86, 0)],
    # D: lower, half grown over.
    [(206, -112, 0), (180, -78, 5), (156, -44, 7.5), (128, -6, 5.5), (104, 26, 6.5), (88, 50, 0)],
]
# Dunes on their own: (x, z of the middle of the crest, half-length,
# height, bend, lean). A bend above zero swings the ends downwind (a
# barchan and its horns), below zero upwind (a parabolic dune and the
# arms brush has pinned, which keep some height to their ends). Lean
# turns the crest (radians).
DUNES = [
    # Barchans out on the flats, ahead of the field.
    (-150, -238, 26, 4.2, 0.30, 0.0),
    (96, -246, 22, 3.6, 0.32, 0.1),
    (204, -232, 30, 5.0, 0.28, -0.05),
    (-262, -250, 20, 3.0, 0.30, 0.0),
    # The east's old dunes.
    (182, -168, 46, 5.5, -0.34, 0.0),
    (258, -128, 52, 6.5, -0.30, 0.08),
    (236, -58, 44, 5.5, -0.32, -0.06),
    (292, 2, 40, 4.5, -0.30, 0.0),
    (170, 6, 34, 4.0, -0.28, 0.1),
]
# The king: on the bench, its long back down over the rimrock.
KING = [(234, 92, 0), (204, 126, 10), (170, 162, 20), (136, 198, 31), (96, 236, 39), (60, 270, 31), (28, 302, 33),
        (-6, 338, 23), (-42, 376, 13), (-82, 422, 6), (-122, 470, 0)]


# ---------------------------------------------------------------------------
# Dunes

# A crest drawn where it falls between the ground's vertices comes out as a
# row of teeth: one vertex on it, the next a step down its slip face. So
# each crest is moved on to the mesh, where the game can draw it as an
# edge (it splits each of this area's cells the way its heights lie:
# LvlLook.fit_split), and the sand within SNAP_FAR of it moves with it.
SNAP = 2                        # the grid a crest is put on, in cells: the 6 m one the ground is drawn on past 50 m
SNAP_NEAR, SNAP_FAR = 8.0, 40.0


def snap_crest(pts, hs):
    """A crest moved on to the mesh: from vertex to neighbouring vertex of
    the 6 m grid, so it is an unbroken line of edges on that grid and on
    the 3 m one. Returns its points a metre apart, their heights, and the
    point of the crest as drawn that each stands for."""
    step = D * SNAP
    g = np.round((pts - ORG) / step).astype(int)
    # The vertices the crest passes nearest, in order, and the points of
    # it nearest each.
    chain, where = [], []
    for k, v in enumerate(map(tuple, g)):
        if not chain or chain[-1] != v:
            chain.append(v)
            where.append([k, k])
        else:
            where[-1][1] = k

    def beside(a, b):
        return max(abs(b[0] - a[0]), abs(b[1] - a[1])) <= 1
    # Corners the mesh can cut go: a step out and back, two steps where
    # one would do.
    changed = True
    while changed:
        changed = False
        out, w = [chain[0]], [where[0]]
        k = 1
        while k < len(chain):
            if k + 1 < len(chain) and beside(out[-1], chain[k + 1]):
                changed = True
            elif chain[k] != out[-1]:
                out.append(chain[k])
                w.append(where[k])
            k += 1
        chain, where = out, w
    if len(chain) < 2:
        return pts, hs, pts
    v = np.array(chain, dtype=float) * step + ORG
    # Where along the crest as drawn each vertex stands (its ends stay
    # the crest's ends).
    at = np.array([0.5 * (a + b) for a, b in where])
    at[0], at[-1] = 0, len(pts) - 1
    s = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(v, axis=0), axis=1))])
    u = np.linspace(0, s[-1], int(s[-1]) + 1)
    k = np.interp(u, s, at)
    n = np.arange(len(pts))
    back = np.stack([np.interp(k, n, pts[:, 0]), np.interp(k, n, pts[:, 1])], axis=1)
    return np.stack([np.interp(u, s, v[:, 0]), np.interp(u, s, v[:, 1])], axis=1), np.interp(k, n, hs), back


def crest_line(x, z, half, height, bend, lean):
    """A dune's crest: points a metre apart from its south-east end to its
    north-west, and the height at each."""
    ca, sa = math.cos(lean), math.sin(lean)
    c = np.array([CREST[0] * ca - CREST[1] * sa, CREST[0] * sa + CREST[1] * ca])
    w = np.array([c[1], -c[0]])
    t = np.linspace(-1, 1, 41)
    pts = np.array([x, z]) + np.outer(t * half, c) + np.outer(bend * half * t * t, w)
    if bend >= 0:
        hs = height * np.clip(1 - np.abs(t) ** 2.4, 0, 1) ** 0.85
    else:
        hs = height * (0.42 + 0.58 * np.clip(1 - np.abs(t) ** 1.6, 0, 1)) * np.clip((1 - np.abs(t)) * 6, 0, 1)
    s = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))])
    n = int(s[-1]) + 1
    u = np.linspace(0, s[-1], n)
    return np.stack([np.interp(u, s, pts[:, 0]), np.interp(u, s, pts[:, 1])], axis=1), np.interp(u, s, hs)


def ridge_line(ctrl, seed):
    """A ridge's crest from its control points: rounded off, wandering a
    little, a point a metre."""
    c = np.array(ctrl, dtype=float)
    line = chaikin(c[:, :2], 3)
    pts = resample(line, 1.0)
    at = np.array([cKDTree(pts).query(q)[1] for q in c[:, :2]])
    hs = np.interp(np.arange(len(pts)), at, c[:, 2])
    hs = gaussian_filter1d(hs, 6.0, mode="nearest") * np.clip(np.minimum(np.arange(len(pts)), len(pts) - 1 - np.arange(len(pts))) / 14.0, 0, 1)
    s = np.arange(len(pts), dtype=float)
    wob = 5.0 * np.sin(s / 31.0 + seed) + 3.0 * np.sin(s / 13.0 + 2.3 * seed)
    pts = pts + np.outer(wob * np.clip(hs / 6.0, 0, 1), WIND)
    return pts, hs


def dune_profile(pts, hs, stoss=6.0, reach=260.0):
    """A dune's height over the ground at every vertex: a long back
    climbing with the wind to the crest, the slip face beyond it. Returns
    the height, how far down the slip face each vertex is (m, 0 off it)
    and how far it is from the crest."""
    # The dune as drawn, but each vertex near the crest takes the height
    # of the place it would be had the crest not been moved on to the mesh
    # (snap_crest): the mesh's line of edges is then the crest exactly,
    # and the dune's shape either side of it is unchanged.
    spts, _, back = snap_crest(pts, hs)
    sd, si = cKDTree(spts).query(FLAT, distance_upper_bound=SNAP_FAR)
    near = np.isfinite(sd)
    si = np.where(near, si, 0)
    at = FLAT + (back[si] - spts[si]) * np.where(near, smoothstep(SNAP_FAR, SNAP_NEAR, sd), 0.0)[:, None]
    d, idx = cKDTree(pts).query(at, distance_upper_bound=reach)
    ok = np.isfinite(d)
    idx = np.where(ok, idx, 0)
    d = np.where(ok, d, reach)
    tg = np.gradient(pts, axis=0)
    rel = at - pts[idx]
    lee = tg[idx, 0] * rel[:, 1] - tg[idx, 1] * rel[:, 0] < 0
    H = hs[idx]
    L = np.maximum(H * stoss, 8.0)
    back = H * np.cos(0.5 * math.pi * np.clip(d / L, 0, 1)) ** 2
    face = H - REPOSE * d
    y = np.where(lee, face, back)
    y = np.where(ok, np.maximum(y, 0), 0)
    slip = np.where(lee & (y > 0), d, 0.0)
    return y.reshape(N, N), slip.reshape(N, N), d.reshape(N, N)


def sand_waves(amp, length, seed, sharp=0.72):
    """Small dunes riding the big ones: rows across the wind, each a long
    back and a short lee, their crests wandering."""
    p = X * WIND[0] + Z * WIND[1]
    q = X * CREST[0] + Z * CREST[1]
    ph = p / length + 1.1 * (fbm(q, p * 0.3, 46, 3, seed) - 0.5) * 2 + 0.25 * (fbm(X, Z, 19, 2, seed + 5) - 0.5)
    t = ph - np.floor(ph)
    saw = np.where(t < sharp, t / sharp, (1 - t) / (1 - sharp))
    saw = saw * saw * (3 - 2 * saw)
    # Rows come and go along their length.
    gap = smoothstep(0.3, 0.6, fbm(q, p, 60, 3, seed + 9))
    return amp * saw * gap


def settle(h, sand, iters=40):
    """Sand cannot stand steeper than its angle of rest: what is, slides."""
    lim = REPOSE * D
    for _ in range(iters):
        moved = False
        for dj, di, k in ((0, 1, 1.0), (1, 0, 1.0), (1, 1, 1.4142), (1, -1, 1.4142)):
            a = h
            b = np.roll(np.roll(h, -dj, axis=0), -di, axis=1)
            diff = a - b
            over = np.abs(diff) - lim * k
            m = (over > 0.02) & sand & np.roll(np.roll(sand, -dj, axis=0), -di, axis=1)
            m[-1, :] = m[:, -1] = m[:, 0] = False
            if not m.any():
                continue
            moved = True
            mv = np.where(m, 0.25 * over * np.sign(diff), 0.0)
            h = h - mv
            h = h + np.roll(np.roll(mv, dj, axis=0), di, axis=1)
        if not moved:
            break
    return h


def round_off(h, f, sand, routes):
    """Sand keeps an edge only where the wind makes one, along a dune's
    crest and down its slip face. Any other (the brink of a bank a track
    was cut through, the crease where one drift lies against another) is
    blown round. The tracks and their shoulders stay as they were laid."""
    keep = (f["slip"] > 0) | (f["dcrest"] < 7.0) | ~sand
    for r in routes:
        d, _ = cKDTree(r["pts"]).query(FLAT, distance_upper_bound=r["half"] + 7.0)
        keep |= np.isfinite(d).reshape(N, N)
    w = 1.0 - np.clip(gaussian_filter(keep.astype(float), 1.0) * 2.0, 0, 1)
    return h + (gaussian_filter(h, 1.15) - h) * w


# ---------------------------------------------------------------------------
# The ground

def base_terrain():
    f = {}
    dshore = side_dist(SHORE, left=True)
    f["dshore"] = dshore
    # Along the rim's trend: the sand sheet climbs from the flats to the
    # rimrock's foot, but stays low beside the inlet.
    n = Z + 0.39 * X
    inland = smoothstep(14, 150, dshore)
    sheet = FLATS_Y + 0.6 * smoothstep(-320, -215, Z) + 17.5 * smoothstep(-205, 92, n) ** 1.15 * inland
    sheet += 0.5 * (fbm(X, Z, 70, 3, 601) - 0.5) * 2 * smoothstep(-230, -180, Z)
    # West of where the rimrock shows the sand has buried it to the top.
    bury = smoothstep(30, -50, X)
    drim = side_dist(RIM, left=True) + 3.0 * (fbm(X, Z, 34, 3, 603) - 0.5) * 2 + 1.6 * (ridged(X, Z, 12, 2, 604) - 0.5) * 2
    f["drim"] = drim
    bench = BENCH_Y + 2.2 * (fbm(X, Z, 80, 3, 605) - 0.5) * 2 + 0.012 * np.clip(drim, 0, 300)
    sheet = sheet + (bench - 1.2 - sheet) * bury * smoothstep(-150, -10, drim)
    f["cliff"] = np.maximum(bench - sheet, 0.0)
    h = sheet + scarp(drim, f["cliff"], talus=0.3, slope=0.62, face=6.4)
    f["bench"] = bench
    return h, f


def build_dunes(h, f):
    """The dune field on the ground so far."""
    drim, dshore = f["drim"], f["dshore"]
    lift = np.zeros((N, N))
    slip = np.zeros((N, N))
    lines = [ridge_line(r, k + 1.0) for k, r in enumerate(RIDGES)] + [crest_line(*d) for d in DUNES]
    lines.append(ridge_line(KING, 7.0))
    dcrest = np.full((N, N), 999.0)
    for k, (pts, hs) in enumerate(lines):
        y, s, d = dune_profile(pts, hs, stoss=5.6 if k == len(lines) - 1 else 5.6 + 0.4 * math.sin(k * 1.7))
        slip = np.where(y > lift, s, slip)
        dcrest = np.where(y > lift, d, dcrest)
        lift = np.maximum(lift, y)
    f["dcrest"] = dcrest
    f["crests"] = [snap_crest(pts, hs)[:2] for pts, hs in lines]     # where they are on the ground
    # Where the small dunes run: from behind the flats to the rimrock,
    # thinning to nothing toward the water and the brush.
    field = smoothstep(-226, -190, Z) * smoothstep(24, 70, dshore)
    open_sand = np.maximum(smoothstep(170, 70, X) * (drim < 6), (drim > 0) * smoothstep(1.2, 3.0, lift))
    f["field"], f["open"] = field, open_sand
    waves = sand_waves(1.0, 36.0, 611) * (0.12 + 0.38 * open_sand)
    waves *= field * (slip == 0) * smoothstep(8, -30, drim)
    # (calmer on the big dunes' backs, which are swept smooth toward the crest)
    waves *= 1.0 - 0.7 * smoothstep(2.0, 8.0, lift)
    h = h + lift * smoothstep(14, 40, dshore) + waves
    f["lift"] = lift
    f["slip"] = slip
    return h


def hollow(h, x, z, r, depth, squash=1.0, ang=0.0):
    """A blowout: a bowl the wind has scoured, its rim rounded."""
    dx, dz = X - x, Z - z
    ca, sa = math.cos(ang), math.sin(ang)
    u, v = dx * ca + dz * sa, (-dx * sa + dz * ca) / squash
    q = np.clip(1 - (u * u + v * v) / (r * r), 0, 1)
    h -= depth * q ** 1.5
    return q


POND_RIM = 1.6


def pond_site(h, x, z, r, tracks, f):
    """Where a pond wanted at (x, z) can lie: the nearest ground whose
    rim would stand no more than POND_RIM over its water anywhere, clear of
    the tracks, the lake and the slip faces; failing that, the levellest
    within 30 m. A pond wants a floor to sit on, and the sand has moved
    since its place was picked."""
    best, at = 1e9, (x, z)
    ring = (np.hypot(X, Z) > r - 1.5) & (np.hypot(X, Z) < r + 4.5)
    oj, oi = np.nonzero(ring)
    oj, oi = oj - (N - 1) // 2, oi - (N - 1) // 2
    c = np.round(to_grid(np.array([x, z]))).astype(int)
    for dj in range(-10, 11):
        for di in range(-10, 11):
            i, j = c[0] + di, c[1] + dj
            jj, ii = j + oj, i + oi
            if jj.min() < 2 or ii.min() < 2 or jj.max() > N - 3 or ii.max() > N - 3:
                continue
            px, pz = ORG + i * D, ORG + j * D
            off = math.hypot(px - x, pz - z)
            if off > 30.0 or tracks.query((px, pz))[0] < r + 9.0 or f["dshore"][j, i] < r + 12.0:
                continue
            if f["slip"][jj, ii].any() or f["drag"][jj, ii].any():
                continue
            ys = h[jj, ii]
            rim = float(ys.max() - ys.min())
            cost = off if rim <= POND_RIM else 100.0 + rim
            if cost < best:
                best, at = cost, (px, pz)
    return at


def seat_pond(h, x, z, r, depth):
    """A pond in a hollow of its own at (x, z): its water a hand below the
    lowest ground around it, so no bank is under water, and its bed cut
    down from there; the banks above the water keep their heights at the
    rim and come down to it inside. Returns the footprint and the level."""
    d = np.hypot(X - x, Z - z)
    q = np.clip(1 - (d / r) ** 2, 0, 1)
    level = float(h[(d >= r) & (d < r + 4.5)].min()) - 0.12
    bed = level - depth * q ** 1.5
    t = smoothstep(0.0, 0.4, q)
    h[:] = np.minimum(h, h * (1 - t) + bed * t)
    return q, level


def carve_lake(h, f):
    """The reservoir along the south and up the east side as an inlet:
    shelving sand, islands that were dunes, a bar to wade to the nearest.
    Returns the water's surface."""
    d = -f["dshore"] + 2.5 * (fbm(X, Z, 26, 3, 621) - 0.5) * 2      # metres out from the shore
    bed = LAKE_Y - np.clip(d * 0.055, 0, 0.9) - 2.0 * smoothstep(14, 60, d)
    beach = smoothstep(-26, 0, d)
    h = np.where(d > 0, np.minimum(h, bed), h * (1 - beach) + np.minimum(h, LAKE_Y + 0.25 + 0.05 * -d) * beach)
    # The inlet silts up into marsh toward its head: knee deep at most.
    marsh = smoothstep(-230, -150, Z) * (X > 250)
    f["marsh"] = marsh * (d > -8)
    h = np.where(d > 0, h * (1 - marsh) + np.maximum(h, LAKE_Y - 0.26 - 0.12 * fbm(X, Z, 14, 2, 623)) * marsh, h)
    isl = np.zeros((N, N))
    for k, (x, z, half, height) in enumerate(ISLANDS):
        pts, hs = crest_line(x, z, half, height + 2.6, 0.2, 0.1 * math.sin(k * 2.1))
        y, _, _ = dune_profile(pts, hs, stoss=7.0, reach=120.0)
        top = LAKE_Y - 2.6 + y + np.where(y > 0, 0.3 * (fbm(X, Z, 11, 2, 625 + k) - 0.5), 0)
        isl = np.maximum(isl, (top > h) * (top > LAKE_Y - 0.3))
        h = np.maximum(h, top)
    f["island"] = isl > 0
    bd, _, _ = line_dist(BAR)
    h = np.where(bd < 9, np.maximum(h, LAKE_Y - 0.34 - 0.04 * bd), h)
    water = np.where(h < LAKE_Y - 0.04, LAKE_Y, -1000.0)
    water[f["dshore"] > 30] = -1000.0
    return h, water


# ---------------------------------------------------------------------------
# Routes: drawn here, the ground trued up to them

R_APPROACH = [(0, -262, None), (2, -246, None), (5, -230, None), (7, -214, None), (8, -198, None)]
# East: along the foot of the dunes to the inlet, up its shore, then the
# grade: a ramp of sand and talus against the rimrock, and back west
# along the bench.
R_SHORE = [
    (8, -198, None), (44, -206, None), (92, -212, None), (136, -206, None), (168, -214, None), (214, -206, None),
    (256, -196, None), (290, -180, None), (304, -150, None), (300, -120, None), (282, -96, None), (258, -76, None),
    (234, -52, None), (216, -26, None), (204, -4, 19.5), (190, 14, 21.6), (172, 30, 24.4), (154, 46, 27.6),
    (136, 62, 30.6), (110, 80, None), (76, 96, None), (38, 110, None), (8, 118, None), (-16, 116, None),
]
# Centre: under the wires. Round the end of the second ridge, up the
# trough behind it, over the third at its saddle, along to the ramp the
# sand has built against the rimrock.
R_POWER = [
    (8, -198, None), (26, -182, None), (40, -162, None), (34, -138, None), (20, -112, None), (4, -86, None),
    (-10, -62, None), (-6, -38, None), (6, -14, None), (14, 6, None), (30, 20, None), (40, 40, None),
    (44, 62, None), (40, 82, None), (26, 100, None), (4, 112, None), (-16, 116, None),
]
# West: on to the first ridge's tail and along its crest (filled in from
# the crest itself), down its slip face into the bowl, out up the big
# ridge's back to its saddle, down and across to the rimrock.
R_RAZOR_IN = [(8, -198, None), (-18, -206, None), (-44, -216, None)]
R_RAZOR_OUT = [(-214, -8, None), (-196, 8, None), (-178, 22, None), (-158, 34, None), (-140, 40, None),
               (-120, 52, None), (-98, 68, None), (-76, 86, None), (-48, 104, None), (-16, 116, None)]
RAZOR_CREST = (1, (-70, -214), (-236, -28))    # ridge A, between these two points
R_SUMMIT = [(-16, 116, None), (-14, 140, None), (4, 162, None), (32, 176, None), (56, 196, None), (76, 220, None),
            (96, 236, None)]
BOWL = (-198, 4, 32.0, 6.5)
RAMP = ((34, 90), (56, 82))        # the rim's edge the powerline's ramp of sand tops out along
GRADE = ((150, 50), (176, 40))     # and the one the shore road climbs


def sand_ramp(h, a, b, top, grade=0.2):
    """A climbing dune: sand banked against the rimrock to its top between
    a and b, fanning down and out from there."""
    a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    e = b - a
    t = np.clip(((X - a[0]) * e[0] + (Z - a[1]) * e[1]) / (e @ e), 0, 1)
    d = np.hypot(X - (a[0] + e[0] * t), Z - (a[1] + e[1] * t))
    fill = top - grade * d - 0.0012 * d * d + 0.5 * (fbm(X, Z, 20, 2, 641) - 0.5)
    m = fill > h
    np.maximum(h, fill, out=h)
    return m


def finish_routes(h, routes):
    """Each route's turns, widths and heights, and the ground trued up
    under it."""
    for r in routes:
        r["turn"] = turniness(r["pts"])
        r["halfs"] = r["half"] * (1.0 + 0.8 * r["turn"])
        seg = np.linalg.norm(np.diff(r["pts"], axis=0), axis=1)
        y = r["y"]
        glim = r["gmax"] * (1.0 - 0.7 * r["turn"])
        for _ in range(4):
            for k in range(1, len(y)):
                lim = glim[k] * seg[k - 1]
                y[k] = np.clip(y[k], y[k - 1] - lim, y[k - 1] + lim)
            for k in range(len(y) - 2, -1, -1):
                lim = glim[k] * seg[k]
                y[k] = np.clip(y[k], y[k + 1] - lim, y[k + 1] + lim)
    appr, shore, razor, power, summit = routes
    fork_y = appr["y"][-1]
    meet_y = float(np.mean([r["y"][-1] for r in (shore, razor, power)]))

    def pin(r, end, y_to, span=24.0):
        n = len(r["pts"])
        dist = np.arange(n, dtype=float) if end == 0 else np.arange(n - 1, -1, -1, dtype=float)
        r["y"] += (y_to - r["y"][0 if end == 0 else -1]) * np.clip(1 - dist / span, 0, 1)
    for r in (shore, razor, power):
        pin(r, 0, fork_y)
        pin(r, -1, meet_y)
    pin(summit, 0, meet_y)
    # Tracks over sand are only the strip the tyres have packed; the shore
    # road is built, cut and filled.
    lay_route(h, razor, 3.5)
    lay_route(h, power, 4.5)
    lay_route(h, summit, 4.0)
    lay_route(h, shore, 10.0)
    lay_route(h, appr, 5.0)


def rollers(h, r, s0, s1, amp, length):
    """Whoops: the track between points s0 and s1 of route r beaten into a
    row of humps."""
    pts = r["pts"]
    d, idx = cKDTree(pts).query(FLAT, distance_upper_bound=r["half"] + 4.0)
    ok = np.isfinite(d)
    idx = np.where(ok, idx, 0).reshape(N, N)
    d = np.where(ok, d, 99.0).reshape(N, N)
    env = smoothstep(s0, s0 + 10, idx) * smoothstep(s1, s1 - 10, idx)
    wave = amp * np.sin(2 * math.pi * (idx - s0) / length) * env * smoothstep(r["half"] + 3.5, r["half"], d)
    h += wave
    k = np.arange(len(pts))
    r["y"] += amp * np.sin(2 * math.pi * (k - s0) / length) * smoothstep(s0, s0 + 10, k) * smoothstep(s1, s1 - 10, k)
    return (wave != 0)


# The flats: somewhere to play before the dunes.
PLAY_BOWL = (-84, -244, 27.0, 5.0)
MUD_BOG = (-196, -276, 26.0, 0.9)
POND = (158, -188, 17.0, 2.2)          # in the hollow of the first old dune
PONDS = [(266, -150, 12.0, 1.6), (-300, -206, 13.0, 1.5), (84, -42, 10.0, 1.4)]
# And out among the big dunes, where a trough's floor reaches the water
# table (each goes to the lowest ground near the place given): potholes.
POTHOLES = [(-160, -44, 11.0, 1.5), (-296, -70, 12.0, 1.6), (-250, 96, 10.0, 1.4)]
DRAG = ((44, -300), (214, -292))
PUDDLES = [(-232, -262, 9.0, 0.5), (-168, -292, 7.0, 0.45), (-246, -290, 8.0, 0.5), (-150, -262, 6.0, 0.4)]
GROVE_DUNE = (258, -128)               # the old dune (in DUNES) burying its olives
PYLON_FROM, PYLON_DIR, PYLON_GAP = (146.0, -360.0), (-0.31, 0.95), 122.0


def build_ground():
    h, f = base_terrain()
    h = build_dunes(h, f)
    f["bowl"] = hollow(h, *BOWL) > 0.02
    f["play_bowl"] = hollow(h, *PLAY_BOWL) > 0.02
    top = float(sample(f["bench"], np.array(RAMP[0]))[0]) + 0.3
    f["ramp"] = sand_ramp(h, RAMP[0], RAMP[1], top)
    f["ramp"] |= sand_ramp(h, GRADE[0], GRADE[1], float(sample(f["bench"], np.array(GRADE[0]))[0]) + 0.2, 0.17)
    h = settle(h, (f["lift"] > 0.2) | f["bowl"] | f["play_bowl"])
    # Ponds where a hollow reaches the water table, which stands higher
    # under the dunes than the lake does.
    ponds = []
    holes = []
    for (x, z, r, depth) in POTHOLES:
        m = (np.hypot(X - x, Z - z) < 34) & (f["slip"] == 0)
        k = np.argmin(np.where(m, f["lift"] + 0.02 * np.hypot(X - x, Z - z), 1e9))
        holes.append((float(X.ravel()[k]), float(Z.ravel()[k]), r, depth))
    f["potholes"] = holes
    for (x, z, r, depth) in [POND] + PONDS + holes:
        q = hollow(h, x, z, r, depth, 0.8, 0.9)
        level = float(h[q > 0.5].max()) + 0.1
        ponds.append((q, level))
    # The mud flats' wallow.
    q = hollow(h, *MUD_BOG, 0.6, 0.2)
    ponds.append((q, float(h[q > 0.25].max())))
    f["bog"] = q > 0.02
    for (x, z, r, depth) in PUDDLES:
        q = hollow(h, x, z, r, depth, 0.7, 0.5)
        ponds.append((q, float(h[q > 0.3].max())))
        f["bog"] |= q > 0.02
    # The drag strip: dead level.
    a, b = np.array(DRAG[0], float), np.array(DRAG[1], float)
    e = b - a
    t = np.clip(((X - a[0]) * e[0] + (Z - a[1]) * e[1]) / (e @ e), 0, 1)
    dd = np.hypot(X - (a[0] + e[0] * t), Z - (a[1] + e[1] * t))
    w = smoothstep(13, 7, dd)
    h[:] = h * (1 - w) + (FLATS_Y + 0.25) * w
    f["drag"] = dd < 8

    crest = f["crests"][RAZOR_CREST[0]][0]
    i0 = int(np.argmin(np.linalg.norm(crest - np.array(RAZOR_CREST[1], float), axis=1)))
    i1 = int(np.argmin(np.linalg.norm(crest - np.array(RAZOR_CREST[2], float), axis=1)))
    # (the track keeps a tyre's width back from the brink)
    along = [(p[0] - WIND[0] * 2.2, p[1] - WIND[1] * 2.2, None) for p in crest[i0:i1:14]]
    razor_ctrl = R_RAZOR_IN + along + R_RAZOR_OUT
    routes = [route_line(h, R_APPROACH, 0.10, "CAMP ROAD"), route_line(h, R_SHORE, 0.12, "SHORE ROAD"),
              route_line(h, razor_ctrl, 0.30, "RAZORBACK"), route_line(h, R_POWER, 0.20, "POWERLINE"),
              route_line(h, R_SUMMIT, 0.24, "KING DUNE")]
    appr, shore, razor, power, summit = routes
    appr.update(half=3.6, difficulty=0, short="CAMP")
    shore.update(half=3.4, difficulty=1, short="SHORE")
    razor.update(half=3.0, difficulty=3, short="RAZORBACK")
    power.update(half=3.2, difficulty=2, short="POWERLINE")
    summit.update(half=3.2, difficulty=3, short="KING DUNE")
    finish_routes(h, routes)
    # A track cut through a crest leaves walls no sand would stand in:
    # they slump to its angle of rest, and the track is trued up again.
    rock = (f["cliff"] > 2.5) & (f["drim"] > -9) & (f["drim"] < 14) & ~f["ramp"]
    h = settle(h, ~rock & (f["dshore"] > 8), iters=60)
    # Where two tracks close on each other their cuts leave a blade of
    # ground a vertex wide standing between them: it goes.
    beside = np.zeros((N, N), dtype=int)
    for r in routes:
        d, _ = cKDTree(r["pts"]).query(FLAT, distance_upper_bound=14.0)
        beside += np.isfinite(d).reshape(N, N)
    blade = (beside >= 2) & (f["slip"] == 0) & (f["dcrest"] > 7.0)
    h = np.where(blade, grey_opening(h, size=(3, 3)), h)
    h = round_off(h, f, ~rock & (f["dshore"] > 8), routes)
    for r, sh in ((razor, 2.0), (power, 2.5), (summit, 2.5), (shore, 4.0), (appr, 3.0)):
        lay_route(h, r, sh)
    k0 = int(np.argmin(np.linalg.norm(power["pts"] - np.array([36.0, -146.0]), axis=1)))
    k1 = int(np.argmin(np.linalg.norm(power["pts"] - np.array([2.0, -82.0]), axis=1)))
    f["rollers"] = rollers(h, power, k0, k1, 0.34, 10.0)
    h, water = carve_lake(h, f)
    # The ponds are seated now the ground is done. (Their hollows were
    # scoured out before the sand settled, each with its water at the
    # height of its upper lip: on a dune's flank that stood the pond out of
    # the hillside like a hill of its own.)
    n_ponds = 1 + len(PONDS) + len(POTHOLES)
    tracks = cKDTree(np.vstack([r["pts"] for r in routes]))
    f["pond"] = np.zeros((N, N), dtype=bool)
    f["ponds"] = []
    for (x, z, r, depth) in [POND] + PONDS + holes:
        x, z = pond_site(h, x, z, r, tracks, f)
        q, level = seat_pond(h, x, z, r, depth)
        water[(q > 0) & (h < level - 0.04)] = level
        f["pond"] |= q > 0
        f["ponds"].append((x, z, r, depth))
    f["potholes"] = f["ponds"][1 + len(PONDS):]
    for q, level in ponds[n_ponds:]:
        wet = (q > 0) & (h < level - 0.04)
        water[wet] = level
    settle_report(h, water)
    for (x, z, r, depth), was in zip(f["ponds"], [POND] + PONDS + holes):
        wet = (np.hypot(X - x, Z - z) < r) & (water > -999)
        print(f"  pond at ({x:.0f}, {z:.0f}) (asked for at ({was[0]:.0f}, {was[1]:.0f})): {int(wet.sum())} wet vertices, "
              f"{float((water - h)[wet].max()) if wet.any() else 0:.1f} m deep")
    return h, water, f, routes


# ---------------------------------------------------------------------------
# Surfaces and colours

def near_water(water, r):
    """Metres to the nearest water, up to r."""
    wet = np.stack([X[water > -999], Z[water > -999]], axis=1)
    d, _ = cKDTree(wet).query(FLAT, distance_upper_bound=r)
    return np.where(np.isfinite(d), d, r).reshape(N, N)


def surfaces(h, water, f, routes):
    sl = slope_deg(h)
    lift, slip, drim, dshore = f["lift"], f["slip"], f["drim"], f["dshore"]
    dw = near_water(water, 40.0)
    f["dw"] = dw
    surf = np.full((N, N), S["SAND"], dtype=np.uint8)
    # Where the brush has taken hold (the game's GRASS, for how it drives
    # and what grows): the east's old dunes, the bench, the margins.
    brush = fbm(X, Z, 58, 3, 651)
    east = smoothstep(96, 190, X + 0.3 * Z + 40 * (brush - 0.5))
    on_bench = (drim > 4) & (lift < 2.5)
    veg = (east > 0.5) | on_bench | ((brush > 0.6) & (f["open"] < 0.5))
    veg &= (sl < 20) & (slip == 0) & ~((lift > 3.5) & (f["open"] > 0.5))
    # (one of the old dunes is on the move again, into the olives)
    gx, gz = GROVE_DUNE[0], GROVE_DUNE[1]
    veg &= ~((np.hypot(X - gx, Z - gz) < 62) & (lift > 1.2))
    # Round each pothole in the open sand, a ring of green.
    for (x, z, r, _) in f["potholes"]:
        veg |= (np.hypot(X - x, Z - z) < r + 13 + 5 * (brush - 0.5)) & (sl < 15) & (slip == 0) & (lift < 3.0)
    surf[veg] = S["GRASS"]
    # The flats: packed and damp, hard as a road. So are the floors of the
    # troughs between the dunes, where the water table is not far down.
    flats = (Z < -212 - 10 * (fbm(X, Z, 40, 2, 653) - 0.5)) & (lift < 0.5)
    floor = (f["field"] > 0.9) & (lift < 0.55) & (sl < 5.5) & (drim < -6) & (f["open"] > 0.3)
    floor = gaussian_filter(floor.astype(float), 1.0) > 0.55
    surf[flats | floor] = S["HARDPACK"]
    surf[(dw < 9) & (sl < 12)] = S["HARDPACK"]            # wet sand at the water's edge
    # Deep, dry, loose: every slip face and the hollow at its foot, the
    # bowls, and the big dunes' crests, where the wind drops what it is
    # carrying.
    deep = (slip > 0) | f["bowl"] | f["play_bowl"]
    deep |= (lift > 5.0) & (f["dcrest"] < 13 + 6 * (brush - 0.5)) & (f["open"] > 0.5) & (surf == S["SAND"])
    deep |= f["ramp"] & (sl > 6)
    foot = gaussian_filter((slip > 0).astype(float), 1.6) > 0.12
    deep |= foot & (surf != S["GRASS"]) & (dw > 6)
    surf[deep] = S["DEEPSAND"]
    # Mud: the flats west of the camp, the marsh's edge, round the ponds.
    mudflat = smoothstep(-110, -150, X) * smoothstep(-236, -252, Z) * (dshore > 4) * (fbm(X, Z, 30, 3, 655) > 0.42)
    surf[(mudflat > 0.5) & (lift < 0.4)] = S["MUD"]
    surf[f["bog"]] = S["MUD"]
    surf[(f["marsh"] > 0.3) & (dw < 5)] = S["MUD"]
    surf[f["pond"] & (dw < 3.5)] = S["MUD"]
    # The rimrock: basalt, talus under it.
    face = f["face"]
    surf[(sl > 30) & (drim > -30) & (drim < 14) & (f["cliff"] > 3) & (lift < 1.0) & ~f["ramp"]] = S["SCREE"]
    surf[face] = S["ROCK"]
    surf[f["knob"]] = S["ROCK"]
    surf[f["drag"]] = S["HARDPACK"]
    appr, shore, razor, power, summit = routes
    for r in routes:
        d, idx = cKDTree(r["pts"]).query(FLAT, distance_upper_bound=float(np.max(r["halfs"])) + 1.0)
        ok = np.isfinite(d)
        ok[ok] = d[ok] < r["halfs"][idx[ok]] + 0.6
        jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
        k = idx[ok]
        cur = surf[jj, ii]
        if r is appr:
            v = np.full(len(jj), S["HARDPACK"])
        elif r is shore:
            # Graded: gravel on the grade and the bench, packed sand below.
            v = np.where(drim[jj, ii] > -70, S["GRAVEL"], np.where(cur == S["GRASS"], S["DIRT"], S["HARDPACK"]))
            v = np.where(cur == S["MUD"], S["MUD"], v)
        elif r is power:
            # A service track: packed where it runs level, churned to
            # loose sand on every climb.
            g = np.abs(np.gradient(gaussian_filter1d(r["y"], 3.0)))
            v = np.where(g[k] > 0.075, S["SAND"], S["HARDPACK"])
            # (deep only where it crosses a crest on the level)
            v = np.where((cur == S["DEEPSAND"]) & (g[k] < 0.09), S["DEEPSAND"], v)
        else:
            # Over the dunes: deep where the dune is, but on anything
            # steep the traffic has beaten the track down to plain sand,
            # which is what makes it climbable (and why leaving it there
            # is a mistake).
            g = np.abs(np.gradient(gaussian_filter1d(r["y"], 2.0)))
            v = np.where((cur == S["HARDPACK"]) | (cur == S["GRASS"]), S["SAND"], cur)
            v = np.where((cur == S["DEEPSAND"]) & (g[k] > 0.13), S["SAND"], v)
        v = np.where(water[jj, ii] > -999, cur, v)
        surf[jj, ii] = v
    return surf


SAND = np.array([186.0, 170.0, 142.0])          # dry: basalt and quartz, salt and pepper
SAND_PALE = np.array([202.0, 187.0, 158.0])     # sifted fine down a slip face
SAND_DARK = np.array([140.0, 131.0, 116.0])     # where the wind has left the heavy black grains
SAND_DAMP = np.array([150.0, 137.0, 113.0])
SAND_WET = np.array([104.0, 95.0, 78.0])
MUD = np.array([86.0, 76.0, 60.0])
MUD_DRY = np.array([190.0, 178.0, 152.0])       # silt dried pale and hard
STEPPE = np.array([170.0, 152.0, 110.0])        # sandy soil under bunchgrass gone to straw
CHEAT = np.array([196.0, 164.0, 92.0])          # cheatgrass, gold by midsummer
SAGE = np.array([128.0, 132.0, 104.0])
BASALT = np.array([70.0, 60.0, 56.0])
BASALT_RUST = np.array([118.0, 80.0, 54.0])
TALUS = np.array([92.0, 84.0, 80.0])
GRAVEL = np.array([160.0, 150.0, 134.0])
WATER_RGB = (58, 92, 108)


def dune_normals(h):
    """Normals that keep a dune's edge: the brink takes the back's (so the
    crest is a lit line), the slip face below it the face's own, rather
    than each a blend of both."""
    bx = (h - np.roll(h, 1, axis=1)) / D
    fx = (np.roll(h, -1, axis=1) - h) / D
    bz = (h - np.roll(h, 1, axis=0)) / D
    fz = (np.roll(h, -1, axis=0) - h) / D
    up = bx * WIND[0] + bz * WIND[1]           # the slope coming up to the vertex, with the wind
    dn = fx * WIND[0] + fz * WIND[1]           # and going on from it
    gx, gz = 0.5 * (bx + fx), 0.5 * (bz + fz)
    brink = (dn < -0.36) & (up > -0.2)
    steep_b = np.hypot(bx, bz) > np.hypot(fx, fz)
    face = (np.maximum(np.hypot(bx, bz), np.hypot(fx, fz)) > 0.42) & ~brink
    gx = np.where(brink, bx, np.where(face, np.where(steep_b, bx, fx), gx))
    gz = np.where(brink, bz, np.where(face, np.where(steep_b, bz, fz), gz))
    n = np.dstack([-gx, np.ones_like(h), -gz])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    n[0, :] = n[-1, :] = n[:, 0] = n[:, -1] = (0, 1, 0)
    return n, brink


def colours(h, water, surf, f, routes):
    """The ground's albedo by surface; then lit as the game lights it, for
    the map and the previews."""
    nrm, brink = dune_normals(h)
    face, fx, fz = f["face"], f["face_x"], f["face_z"]
    wall = np.dstack([fx * 0.94, np.full_like(h, 0.34), fz * 0.94])
    nrm = np.where(face[..., None], wall, nrm)
    sl = slope_deg(h)
    p = X * WIND[0] + Z * WIND[1]
    q = X * CREST[0] + Z * CREST[1]
    n1 = fbm(X, Z, 9, 3, 101)
    n2 = fbm(X, Z, 44, 3, 102)
    # Streaks the wind lays down its own way: long along it, narrow across.
    streak = fbm(p * 0.22, q, 9, 3, 661)
    lag = smoothstep(0.52, 0.72, streak)[..., None]
    alb = np.zeros((N, N, 3))
    sand = SAND * (1 - 0.55 * lag) + SAND_DARK * 0.55 * lag
    alb[:] = sand
    # Deep sand is the pale, sifted stuff.
    m = surf == S["DEEPSAND"]
    alb[m] = (SAND_PALE * (1 - 0.2 * lag) + SAND * 0.2 * lag)[m]
    # Packed sand: darker with damp, mottled where it has dried.
    dry = smoothstep(0.4, 0.62, fbm(X, Z, 17, 3, 663))[..., None]
    pack = SAND_DAMP * (1 - 0.6 * dry) + MUD_DRY * 0.6 * dry
    m = surf == S["HARDPACK"]
    alb[m] = pack[m]
    wetk = smoothstep(7.0, 1.0, f["dw"])[..., None]
    alb = np.where((f["dw"] < 7)[..., None] & (surf != S["ROCK"])[..., None], alb * (1 - wetk) + SAND_WET * wetk, alb)
    m = surf == S["MUD"]
    alb[m] = (MUD * (1 - 0.5 * dry) + MUD_DRY * 0.5 * dry * 0.85)[m]
    # Brush country: straw, gold where cheatgrass has it, grey-green
    # where the sage stands thick.
    cheat = smoothstep(0.45, 0.7, fbm(X, Z, 36, 3, 665))[..., None]
    sagek = smoothstep(0.5, 0.72, fbm(X, Z, 24, 3, 667))[..., None] * 0.6
    stp = STEPPE * (1 - 0.6 * cheat) + CHEAT * 0.6 * cheat
    stp = stp * (1 - sagek) + SAGE * sagek
    m = surf == S["GRASS"]
    alb[m] = stp[m]
    alb[surf == S["DIRT"]] = (166, 150, 122)
    alb[surf == S["GRAVEL"]] = GRAVEL
    # Basalt: near black, rusted in panels, streaked down its columns.
    along = X * fz - Z * fx
    col = 0.74 + 0.36 * vnoise(along / 2.2, np.round(np.arctan2(fz, fx) * 2.5), 669) ** 0.8
    rust = smoothstep(0.5, 0.75, vnoise(along / 17.0, h / 9.0, 671))[..., None]
    rock = (BASALT * (1 - 0.6 * rust) + BASALT_RUST * 0.6 * rust) * np.where(face, col, 0.86 + 0.24 * n1)[..., None]
    m = surf == S["ROCK"]
    alb[m] = rock[m]
    m = surf == S["SCREE"]
    alb[m] = (TALUS * (0.86 + 0.28 * n1[..., None]))[m]
    soft = np.dstack([gaussian_filter(alb[..., c], 0.8) for c in range(3)])
    keep = face | (surf == S["DEEPSAND"]) | brink
    alb = np.where(keep[..., None], alb, soft)
    # Green by the water: the damp margin of every pond and the marsh.
    green = smoothstep(9.0, 2.0, f["dw"]) * (sl < 12) * (f["pond"] | (f["marsh"] > 0.2) | (fbm(X, Z, 22, 2, 673) > 0.55))
    alb = alb * (1 - 0.34 * green[..., None]) + np.array([104.0, 118.0, 70.0]) * 0.34 * green[..., None]
    alb *= (0.94 + 0.12 * n1)[..., None] * (0.96 + 0.08 * n2)[..., None]
    # Where everyone drives: sand churned paler in two ruts on the tracks,
    # a darker packed band on the flats.
    wear = np.zeros((N, N))
    for r in routes:
        d, idx = cKDTree(r["pts"]).query(FLAT, distance_upper_bound=7.0)
        ok = np.isfinite(d).reshape(N, N)
        wear = np.maximum(wear, np.where(ok, np.clip(1 - d.reshape(N, N) / (r["half"] + 1.5), 0, 1), 0.0))
    wear = gaussian_filter(wear, 0.8)
    soft_s = ((surf == S["SAND"]) | (surf == S["DEEPSAND"]))[..., None]
    alb = np.where(soft_s, alb * (1 + 0.07 * wear[..., None]), alb * (1 - 0.1 * wear[..., None]))
    # The drag strip: two lanes worn dark.
    alb = np.where(f["drag"][..., None], alb * 0.9, alb)
    ao = np.clip(1.0 + (h - gaussian_filter(h, 4)) * 0.05, 0.76, 1.1)
    ao = np.where(face, np.clip(ao, 0.84, 0.96), ao)
    lam = np.clip(np.einsum("jik,k->ji", nrm, SUN), 0, 1)
    sh = sun_shadow(h)
    sky = np.array([0.62, 0.66, 0.72])
    sun = np.array([1.0, 0.93, 0.80])
    light = sky[None, None, :] * (0.55 + 0.45 * nrm[..., 1:2]) + sun[None, None, :] * (lam * sh)[..., None] * 0.62
    light *= ao[..., None]
    rgb = alb * light
    deep = np.clip((water - h) / 2.5, 0, 1)
    rgb = rgb * (1 - 0.5 * deep[..., None]) + np.array(WATER_RGB) * 0.5 * deep[..., None]
    rgb = np.clip(rgb, 0, 255)
    qq = rgb.astype(np.uint32)
    argb = (0xFF << 24) | (qq[..., 0] << 16) | (qq[..., 1] << 8) | qq[..., 2]
    return argb.astype(np.uint32), rgb, alb * ao[..., None], nrm


# ---------------------------------------------------------------------------
# Rimrock outcrops and the rest of what stands on the ground

# Basalt left standing clear of the sand: (outline, rise).
KNOBS = [
    ([(10, 140), (26, 134), (40, 142), (38, 158), (22, 162), (8, 154)], 6.0),      # by the meet: Scab Rock
    ([(252, 40), (276, 34), (292, 46), (286, 62), (262, 64), (248, 54)], 7.0),
    ([(336, 60), (356, 58), (366, 74), (352, 88), (334, 80)], 6.0),
]


def knobs(h, f):
    kn = np.zeros((N, N), dtype=bool)
    for k, (outline, rise) in enumerate(KNOBS):
        d = plateau_dist(outline, 2) + 1.0 * (ridged(X, Z, 8, 2, 681 + k) - 0.5) * 2
        c = np.mean(np.array(outline, dtype=float), axis=0)
        base = float(sample(h, c)[0])
        top = base + rise + 0.6 * (fbm(X, Z, 9, 2, 685 + k) - 0.5) + scarp(d - 9, 1.8, talus=0.0, face=3.2)
        # Below its talus the foot goes on down as an apron of sand until
        # it meets the ground: on a slope a level foot would stand out of
        # the downhill side as a shelf.
        rise = top - base + 2.0
        apron = 0.42 * np.minimum(d + 0.22 * rise / 0.62, 0.0)
        y = np.where(d > -60, base - 2.0 + scarp(d, rise, talus=0.22, slope=0.62, face=3.2) + apron, -1e9)
        kn |= (y > h + 0.3) & (d > -1.0)
        np.maximum(h, y, out=h)
    f["knob"] = kn


VIEWS = [
    ("camp-north", (0, 8, -300), (10, 20, 0)),
    ("camp-lake", (10, 7, -262), (-40, 0, -400)),
    ("flats-west", (40, 6, -250), (-300, 8, -230)),
    ("flats-east", (-40, 6, -250), (300, 10, -200)),
    ("fork", (8, 7, -214), (-10, 20, -60)),
    ("razor", (-120, 16, -170), (-230, 20, -30)),
    ("bowl", (-236, 30, -40), (-150, 14, 50)),
    ("trough", (30, 8, -170), (0, 14, -60)),
    ("rollers", (24, 9, -120), (-20, 14, -20)),
    ("ramp", (40, 22, 30), (30, 34, 110)),
    ("shore-road", (296, 5, -196), (310, 14, -40)),
    ("grade", (290, 12, -70), (200, 34, 40)),
    ("bench", (150, 38, 80), (-20, 44, 110)),
    ("meet", (-40, 38, 70), (50, 60, 200)),
    ("summit-south", (58, 70, 214), (0, 0, -300)),
    ("summit-ne", (50, 68, 196), (200, 30, 330)),
    ("air-se", (330, 170, -340), (0, 20, 40)),
    ("air-sw", (-330, 170, -340), (0, 20, 60)),
    ("air-north", (40, 200, 380), (0, 10, 0)),
    ("air-west", (-380, 120, 0), (0, 20, 20)),
]


def preview(h, rgb, water, path, extra=None, land=(), **kw):
    ims = []
    for name, pos, tgt in VIEWS + (extra or []):
        p = list(pos)
        g = float(sample(h, np.array([p[0], p[2]]))[0])
        p[1] = max(p[1], g + 2.2)
        ims.append(level_view.render(h, rgb, p, tgt, water=water, extra=land, far=1500.0, haze=(206, 212, 214),
                                     sky=((92, 150, 214), (214, 222, 226)), water_rgb=WATER_RGB, rock_rgb=(86, 76, 72), **kw))
    level_view.sheet(ims, 4, path)


def place_trees(h, water, surf, f, routes, rng):
    """Russian olives (0) where their roots reach water: the shore, the
    ponds, the inlet; willows (2) with their feet in it, and on the
    islands; a few cottonwoods (1) at the camp and by the marsh."""
    sl = slope_deg(h)
    dw = f["dw"]
    d_route = cKDTree(np.vstack([r["pts"] for r in routes])).query(FLAT)[0].reshape(N, N)
    grove = fbm(X, Z, 34, 3, 691)
    trees = []

    def ok(p, lim=14.0, road=7.0):
        g = to_grid(p)
        i, j = int(round(g[0])), int(round(g[1]))
        if not (2 <= i < N - 2 and 2 <= j < N - 2):
            return False
        return water[j, i] < -999 and d_route[j, i] > road and sl[j, i] < lim and not f["drag"][j, i]

    for gz in np.arange(ORG + 6, -ORG - 6, 7.0):
        for gx in np.arange(ORG + 6, -ORG - 6, 7.0):
            p = np.array([gx + rng.uniform(-3, 3), gz + rng.uniform(-3, 3)])
            g = to_grid(p)
            i, j = int(round(g[0])), int(round(g[1]))
            if not ok(p) or np.hypot(*(p - SPAWN)) < 34:
                continue
            w = dw[j, i]
            if f["island"][j, i]:
                if rng.random() < 0.5 and h[j, i] > LAKE_Y + 0.25:
                    trees.append((p[0], 0.0, p[1], 2, rng.uniform(0.7, 1.15), rng.uniform(0, 6.28), 0.0))
                continue
            dens = 0.0
            if 2.5 < w < 20:
                dens = 0.5 * smoothstep(0.42, 0.62, grove[j, i])
                if f["pond"][j, i] or f["marsh"][j, i] > 0.2:
                    dens = max(dens, 0.34)
            elif surf[j, i] == S["GRASS"] and grove[j, i] > 0.7:
                dens = 0.05
            # (the open flats by the camp stay open, and the mud flats bare)
            if abs(p[0]) < 120 and p[1] < -236 and w > 8:
                dens *= 0.15
            if p[0] < -120 and p[1] < -236 and f["dshore"][j, i] > 14:
                dens = 0.0
            if rng.random() > dens:
                continue
            kind = 2 if w < 7 and rng.random() < 0.6 else 1 if (w < 14 and rng.random() < 0.08) else 0
            trees.append((p[0], 0.0, p[1], kind, rng.uniform(0.75, 1.25), rng.uniform(0, 6.28), 0.0))
    return trees


def set_pieces(h, water, f, routes, rng):
    appr, shore, razor, power, summit = routes
    spheres, boxes, prop_boxes, posts, capsules, props, views, trees = [], [], [], [], [], [], [], []
    route_tree = cKDTree(np.vstack([r["pts"] for r in routes]))

    def gy(x, z):
        return float(sample(h, np.array([x, z], dtype=float))[0])

    def at(r, x, z):
        return int(np.argmin(np.linalg.norm(r["pts"] - np.array([x, z], dtype=float), axis=1)))

    def flag(x, z):
        props.append((x, z, rng.uniform(0, 6.28), PROP["FLAG"], None))

    def view(x, z, name, mark=(3.0, 2.5)):
        views.append((x, z, name))
        if mark:
            q = np.array([x + mark[0], z + mark[1]])
            if route_tree.query(q)[0] < 5.0:
                q = np.array([x - mark[0], z - mark[1]])
            flag(q[0], q[1])

    # Rockfall under the rimrock.
    face = f["face"]
    foot = face & (np.roll(h, 1, 0) + np.roll(h, -1, 0) + np.roll(h, 1, 1) + np.roll(h, -1, 1) > 4 * h + 5)
    jj, ii = np.nonzero(foot & (np.abs(X) < 370) & (np.abs(Z) < 370))
    pick = rng.permutation(len(jj))[:120]
    for q in pick:
        out = np.array([f["face_x"][jj[q], ii[q]], f["face_z"][jj[q], ii[q]]])
        c = np.array([X[jj[q], ii[q]], Z[jj[q], ii[q]]]) + out * rng.uniform(3, 12) + rng.normal(0, 1.5, 2)
        if route_tree.query(c)[0] < 8 or water[jj[q], ii[q]] > -999:
            continue
        rr = 0.5 + rng.random() ** 2 * 1.8
        spheres.append((c[0], gy(*c) - rr * 0.35, c[1], rr))

    # ---- Things to find, a whip flag by each.
    view(SUMMIT[0], SUMMIT[1], "KING DUNE", None)
    view(MEET[0] + 10, MEET[1] + 12, "SCAB ROCK")
    k = at(razor, -232, -32)
    view(razor["pts"][k][0], razor["pts"][k][1], "RAZORBACK", (-4.0, -3.0))
    view(BOWL[0], BOWL[1], "THE BOWL", (9.0, -7.0))
    view(ISLANDS[0][0] + 2, ISLANDS[0][1] + 5, "PELICAN ISLAND")
    view(MUD_BOG[0] + 32, MUD_BOG[1] + 6, "MUD FLATS")
    view(DRAG[1][0] + 12, DRAG[1][1], "SAND DRAGS")
    pond = f["ponds"][0]
    view(pond[0] - pond[2] - 5, pond[1] - 10, "CATTAIL POND")
    view(PLAY_BOWL[0], PLAY_BOWL[1], "PUNCH BOWL", (-7.0, 6.0))
    k = at(power, 14, -112)
    view(power["pts"][k][0] + 7, power["pts"][k][1], "THE ROLLERS")
    k = at(power, 42, 74)
    view(power["pts"][k][0] + 6, power["pts"][k][1], "THE RAMP")
    k = at(shore, 172, 30)
    view(shore["pts"][k][0], shore["pts"][k][1], "THE GRADE", (5.0, -4.0))
    view(314, -128, "HERON MARSH")
    view(296, 14, "RIMROCK", (-3.0, 3.0))

    # ---- The camp on the flats: trailers and their flags, a tent and its
    # fire, shade and a table, the notice board where the track leaves.
    for (cx, cz, yaw) in [(-46, -262, 1.9), (-64, -284, 2.5), (-50, -304, 0.5), (44, -270, -1.2), (62, -292, -0.4)]:
        props.append((cx, cz, yaw, PROP["CAMPER"], None))
        prop_boxes.append((cx, gy(cx, cz) + 1.35, cz, 1.25, 1.35, 3.0, yaw))
        flag(cx + 2.4 * math.cos(yaw) - 3.4 * math.sin(yaw), cz - 2.4 * math.sin(yaw) - 3.4 * math.cos(yaw))
    cp, cyaw = np.array([-28.0, -292.0]), 2.3
    props.append((cp[0], cp[1], cyaw, PROP["TENT"], None))
    for side in (-1, 1):
        q = cp + np.array([math.sin(cyaw), math.cos(cyaw)]) * 4.0 + np.array([math.cos(cyaw), -math.sin(cyaw)]) * side * 1.6
        dx, dz = math.sin(cyaw) * 0.9, math.cos(cyaw) * 0.9
        capsules.append((q[0] - dx, gy(q[0], q[1]) + 0.18, q[1] - dz, q[0] + dx, gy(q[0], q[1]) + 0.18, q[1] + dz, 0.18))
    # What the camp came out here to ride: quads by the trailers, a sand
    # rail by the second fire, where the east trailers' people sit.
    for (qx, qz, yaw) in [(-38, -270, 0.7), (-41.5, -273.5, 1.0), (52, -264, -2.0)]:
        props.append((qx, qz, yaw, PROP["QUAD"], None))
        prop_boxes.append((qx, gy(qx, qz) + 0.5, qz, 0.6, 0.5, 1.0, yaw))
    props.append((54, -283, 0.6, PROP["FIREPIT"], None))
    props.append((61, -277, -2.5, PROP["RAIL"], None))
    prop_boxes.append((61, gy(61, -277) + 0.75, -277, 1.0, 0.75, 1.8, -2.5))
    props.append((SPAWN[0] - 16, SPAWN[1] + 6, 1.2, PROP["RAMADA"], None))
    prop_boxes.append((SPAWN[0] - 16, gy(SPAWN[0] - 16, SPAWN[1] + 6) + 0.4, SPAWN[1] + 6, 0.9, 0.4, 0.4, 1.2))
    props.append((13, -238, 0.2, PROP["KIOSK"], None))
    props.append((FORK[0] - 7, FORK[1] + 2, 0.4, PROP["SIGN"], None))
    props.append((MEET[0] - 8, MEET[1] - 7, 0.8, PROP["SIGN"], None))
    props.append((DRAG[0][0] - 4, DRAG[0][1] + 11, 1.5, PROP["SIGN"], None))
    # Driftwood along the beach.
    for (cx, cz, ang, L, rr) in [(-20, -317, 0.3, 6.0, 0.22), (34, -316, 1.9, 5.0, 0.18), (120, -322, 1.2, 5.5, 0.2),
                                 (-112, -318, 2.2, 4.5, 0.18), (212, -310, 0.7, 6.5, 0.24)]:
        g = gy(cx, cz)
        dx, dz = math.sin(ang) * L / 2, math.cos(ang) * L / 2
        capsules.append((cx - dx, g + rr * 0.8, cz - dz, cx + dx, g + rr * 0.8, cz + dz, rr))

    # ---- The powerline: H-frames from an island in the lake straight
    # over the dunes and the bench, each stood where the sand lies lowest
    # near its place.
    u = np.array(PYLON_DIR) / np.linalg.norm(PYLON_DIR)
    yaw = math.atan2(u[0], u[1])

    def stand(s0, reach):
        """The best footing within reach of s0 metres along the line."""
        best, best_s = None, 1e9
        for off in np.arange(-reach, reach + 1, 4.0):
            q = np.array(PYLON_FROM) + u * (s0 + off)
            if abs(q[0]) > 372 or abs(q[1]) > 372:
                continue
            g = to_grid(q)
            i, j = int(round(g[0])), int(round(g[1]))
            if water[j, i] > -999 or route_tree.query(q)[0] < 9.0:
                continue
            sc = float(sample(f["lift"], q)[0]) + 6.0 * float(sample(f["slip"], q)[0] > 0) + abs(off) * 0.02
            if sc < best_s:
                best, best_s = s0 + off, sc
        return best

    along = [0.0] + [a_ for a_ in (stand(k * PYLON_GAP, 22.0) for k in range(1, 8)) if a_ is not None]

    def wire_y(s0, s1, t):
        """A wire's height t of the way from the frame at s0 to the one at s1 (src/dunes.c hangs it the same)."""
        a_, b_ = np.array(PYLON_FROM) + u * s0, np.array(PYLON_FROM) + u * s1
        return (gy(*a_) + 13.05) * (1 - t) + (gy(*b_) + 13.05) * t - 0.022 * (s1 - s0) * 4 * t * (1 - t)

    # Where a span would hang too near the sand (a dune between two
    # frames), another frame goes in half way along it.
    for _ in range(8):
        worst, at_s = 7.2, None
        for s0, s1 in zip(along[:-1], along[1:]):
            if s1 - s0 < 72:
                continue
            for t in np.linspace(0.1, 0.9, 17):
                q = np.array(PYLON_FROM) + u * (s0 + (s1 - s0) * t)
                clear = wire_y(s0, s1, t) - gy(*q)
                if clear < worst:
                    worst, at_s = clear, (s0 + s1) / 2
        if at_s is None:
            break
        s_new = stand(at_s, 8.0)
        if s_new is None:
            break
        along = sorted(along + [s_new])

    def clearance(s0, s1):
        return min(wire_y(s0, s1, t) - gy(*(np.array(PYLON_FROM) + u * (s0 + (s1 - s0) * t))) for t in np.linspace(0.06, 0.94, 23))

    def footing(sx):
        q = np.array(PYLON_FROM) + u * sx
        g = to_grid(q)
        i, j = int(round(g[0])), int(round(g[1]))
        return abs(q[0]) < 372 and abs(q[1]) < 372 and water[j, i] < -999 and route_tree.query(q)[0] > 9.0 and \
            float(sample(f["slip"], q)[0]) == 0

    # Then each frame is tried up and down the line between its
    # neighbours for the place that keeps both its spans highest off the
    # ground: on the crests, where a surveyor would have put them.
    for _ in range(2):
        for k in range(1, len(along) - 1):
            lo, hi = along[k - 1] + 44, along[k + 1] - 44
            cur = min(clearance(along[k - 1], along[k]), clearance(along[k], along[k + 1]))
            best, best_c = along[k], cur + 1.0
            for sx in np.arange(lo, hi + 0.1, 4.0):
                if not footing(sx):
                    continue
                c_ = min(clearance(along[k - 1], sx), clearance(sx, along[k + 1]))
                if c_ > best_c:
                    best, best_c = sx, c_
            along[k] = best
    for s0 in along:
        best = np.array(PYLON_FROM) + u * s0
        props.append((best[0], best[1], yaw, PROP["PYLON"], None))
        for side in (-1, 1):
            px, pz = best[0] + side * 2.6 * math.cos(yaw), best[1] - side * 2.6 * math.sin(yaw)
            posts.append((px, gy(px, pz) - 1.0, pz, px, gy(best[0], best[1]) + 15.0, pz, 0.36))
    low = min((wire_y(s0, s1, t) - gy(*(np.array(PYLON_FROM) + u * (s0 + (s1 - s0) * t))), s0 + (s1 - s0) * t)
              for s0, s1 in zip(along[:-1], along[1:]) for t in np.linspace(0.05, 0.95, 19))
    at_low = np.array(PYLON_FROM) + u * low[1]
    print(f"  {len(along)} frames on the powerline, its wires lowest {low[0]:.1f} m off the ground at {at_low[0]:.0f}, {at_low[1]:.0f}, "
          f"{route_tree.query(at_low)[0]:.0f} m from a route")

    # ---- The dune on the move again, and the olives it is burying: the
    # nearest to the crest up to their crowns in its slip face, the next
    # row knee deep, the rest still clear of it.
    pts, hs = f["crests"][len(RIDGES) + [d[:2] for d in DUNES].index(GROVE_DUNE)]
    for k in range(8, len(pts) - 8, 5):
        if hs[k] < 3.0:
            continue
        for row, (reach, sunk) in enumerate([(0.55, 2.6), (1.0, 1.2), (1.5, 0.0)]):
            if rng.random() < 0.3:
                continue
            q = pts[k] + WIND * (hs[k] / REPOSE * reach + 1.0) + rng.normal(0, 1.6, 2)
            if route_tree.query(q)[0] < 7.0:
                continue
            trees.append((q[0], 0.0, q[1], 0, rng.uniform(0.85, 1.2), rng.uniform(0, 6.28), sunk + rng.uniform(-0.3, 0.3)))
    mid = pts[len(pts) // 2] + WIND * (hs[len(pts) // 2] / REPOSE + 9.0)
    view(mid[0], mid[1], "BURIED GROVE")

    # ---- A truck that went into the bowl and never came out.
    wk = np.array([BOWL[0] - 9.0, BOWL[1] + 10.0])
    wyaw = 2.2
    prop_boxes.append((wk[0], gy(*wk) + 0.5, wk[1], 0.9, 0.9, 2.2, wyaw))
    props.append((wk[0], wk[1], wyaw, PROP["WRECK"], gy(*wk) - 0.42))
    return {"rocks": [], "spheres": spheres, "boxes": boxes, "prop_boxes": prop_boxes, "posts": posts,
            "capsules": capsules, "props": props, "views": views, "trees": trees}

# ---------------------------------------------------------------------------
# The extreme trail
#
# WITCH'S EYE: one line with the wind, over three dunes. It leaves the
# razorback where that drops off ridge A for the bowl, carries on along
# the crest, hooks round and goes over the brink: down A's slip face,
# across the trough (past its pond) on what that gave it, through a belt
# of soft sand and up the back of the big ridge beyond (the charge), over
# its brink and down into the witch's eye, a hole the wind has scoured at
# the foot of the face, deep and steep-sided, with nothing for it but to
# stay on the pedal and come out the far side; then the flat of the bench,
# under the powerline's wires, and Comp Hill, the king dune's back taken
# straight up the middle, through the headwall of a blowout, on whatever
# speed the flat gave; and along the king's crest to the top.
#
# Dune driving at its hardest is way on and commitment, not crawling. The
# numbers that decide it (tools/hostsim, every truck, low range and
# lockers): plain sand holds a truck on anything here, so a back is a
# climb; deep sand holds one on about a third of that, so the belt is a
# crawl for a truck that lost its speed, and the eye's far wall (45%) and
# Comp Hill's face (46% for 20 m) are not climbed at all: they are carried,
# the eye on what the drop into it gives, the face on about 20 mph off the
# flat. Each has firm ground before it to take a run from and a rest to be
# put back at. A slip face is only ever gone down, and nothing brakes on
# one: a truck goes over the brink square or it goes down sideways.
#
# The line passes half way between two of the powerline's frames, which
# is what fixes it; the CPU's driver only ever climbs it.

X_VIA = np.array([-56.4, 260.3])       # the line passes here: half way between two of the powerline's frames
X_WEST = 11.0                          # as far as the eye it runs this far north-west of that: clear of the trough's pond
X_EYE = (44.0, 24.0, 6.5, 0.74)        # the eye: metres past the big ridge's brink, half-length, depth, how narrow
X_FACE = (40, 20, 0.46)                # Comp Hill's face: how far up from the toe it starts, its length, its grade
X_SCOOP = 20.0                         # how far out the blowout it stands at the head of runs into the dune's back
X_BELT = 12                            # the belt of soft sand across the foot of the charge (m)


def witchs_eye(h, water, surf, f, routes, trees, rng):
    """Digs the eye, lays the trail and paints it. Returns the route for
    the header (levelkit.xt_full) and what stands along it: props as (x,
    z, yaw, kind, var), a viewpoint as (x, z, name)."""
    appr, shore, razor, power, summit = routes
    w, c = WIND, CREST

    def L(t, west=None):
        """The line, t metres along it from X_VIA: over the first two dunes
        X_WEST to the north-west of where it climbs the king, coming
        across on the flat between the eye and the hill."""
        if west is None:
            west = X_WEST * (1.0 - float(smoothstep(t_join[0], t_join[1], t)))
        return X_VIA + c * west + w * t

    def hit(crest, t_near, west):
        """Where the line crosses a crest as it lies on the ground: the
        distance along the line, and the crest's point."""
        rel = crest - X_VIA - c * west
        t = rel @ w
        k = int(np.argmin(np.abs(rel @ c) + 1e3 * (np.abs(t - t_near) > 45)))
        return float(t[k]), k
    cA, cB, cK = f["crests"][1][0], f["crests"][2][0], f["crests"][-1][0]
    (tA, kA), (tB, kB), (tK, kK) = hit(cA, -319.0, X_WEST), hit(cB, -205.0, X_WEST), hit(cK, 91.0, 0.0)
    t_join = (tB + X_EYE[0] + X_EYE[1] + 8.0, tB + X_EYE[0] + X_EYE[1] + 52.0)

    # The eye, at the foot of the big ridge's slip face, and its sand at rest.
    te, er, ed, en = X_EYE
    te += tB
    q = hollow(h, *L(te), er, ed, en, math.atan2(w[1], w[0]))
    eye = q > 0.02
    near = np.hypot(X - L(te)[0], Z - L(te)[1]) < er + 30
    h[:] = np.where(near, settle(h, near, iters=40), h)

    # The line: from the razorback along A's crest (a tyre's width back
    # from the brink, as the razorback runs), a hook down its back and
    # round to meet the brink square, then straight with the wind to the
    # king's crest, round on to that and along it to the top.
    i1 = int(np.argmin(np.linalg.norm(cA - np.array(RAZOR_CREST[2], float), axis=1)))
    k_in = xt_k(razor, *(cA[i1 - 12] - w * 2.2))
    XA = cA[kA]
    ctrl = [tuple(razor["pts"][k_in])] + [tuple(cA[j] - w * 2.2) for j in range(i1 + 2, kA - 44, 10)]
    R = 8.0
    cc = XA - w * 3.0 - c * R
    ctrl += [tuple(XA - c * 40 - w * 2.2), tuple(XA - c * 28 - w * 4.5), tuple(XA - c * 17 - w * 9.0), tuple(cc - w * R),
             tuple(cc + (c - w) * R * 0.7071), tuple(XA - w * 3.0)]
    R = 9.0
    ctrl += [tuple(L(t)) for t in np.arange(tA + 3.0, tK - R - 2.2 - 6.0, 10.0)]
    XK = cK[kK]
    cc = XK - w * (2.2 + R) - c * R
    ctrl += [tuple(XK - w * (2.2 + R)), tuple(cc + (c + w) * R * 0.7071)]
    k_sum = int(np.argmin(np.linalg.norm(cK - SUMMIT, axis=1)))
    # (It comes off the crest for the last of it and on to the king dune
    # trail's own top few metres, up the back: a truck that arrives along
    # the crest and turns round on it for the way down goes over the edge.)
    ctrl += [tuple(cK[j] - w * 2.2) for j in range(kK - int(R), k_sum + 30, -10)]
    R = 8.0
    cc = SUMMIT - w * 13.0 + c * R + w * R
    ctrl += [tuple(SUMMIT + c * 20 - w * 6.5), tuple(SUMMIT + c * R - w * 13.0), tuple(cc - (w + c) * R * 0.7071),
             tuple(SUMMIT - w * 5.0)]
    r = xt_route(h, [(p_[0], p_[1], None) for p_ in ctrl], "WITCH'S EYE", "WITCH EYE", 3.0)
    pts = r["pts"]
    n = len(pts)

    def at(t):
        """The trail's point where the line is t along (it is the line there)."""
        return xt_k(r, *L(t))
    k_turn = xt_k(r, *(XA - w * 3.0))                  # square to the brink of A
    k_a, k_b, k_k = at(tA), at(tB), at(tK - 11.0)      # A's brink, the big ridge's, the top of Comp Hill
    k_foot_a = at(tA + float(sample(f["lift"], XA)[0]) / REPOSE + 4.0)
    k_brink = at(tB - 9.0)
    k_eye0, k_eye, k_eye1 = at(te - er), at(te), at(te + er)
    k_line = at(te + er + 9.0)                         # level ground past the eye: where trucks line up
    # The hill starts where the king's back does.
    lift = sample(f["lift"], pts)
    k_toe = k_line + int(np.argmax(lift[k_line:k_k] > 0.6))

    # The charge is the big ridge's back as the wind left it: plain sand
    # that low range will pull a truck up from a standing start, behind a
    # belt of the deep stuff blown in off the trough across the foot of
    # it, which is nothing to a truck that kept what the first drop gave
    # it and a long job for one that braked.
    y = r["y"]
    k_low = k_foot_a + int(np.argmin(y[k_foot_a:k_b]))
    k_belt = k_low + int(np.argmax(np.diff(gaussian_filter1d(y[k_low:k_b], 2.0)) > 0.16))

    # Comp Hill. The king's back is a long even climb any truck will
    # crawl; what makes the hill is a blowout the wind has scooped out of
    # it half way up, whose headwall the line takes straight on: a face of
    # deep sand steeper than tyres will pull a truck up, to be carried on
    # what the flat below gave. Easy ground to the foot of it, the face,
    # and the back's own slope again from its lip to the crest.
    f0, fl, fg = X_FACE
    k_f0, k_f1, k_ht = k_toe + f0, k_toe + f0 + fl, k_k - 6
    y_f0 = y[k_toe] + 0.09 * f0
    hill = np.interp(np.arange(k_toe, k_ht + 1), [k_toe, k_f0, k_f1, k_ht], [y[k_toe], y_f0, y_f0 + fg * fl, y[k_ht]])
    top_grade = (y[k_ht] - (y_f0 + fg * fl)) / (k_ht - k_f1)
    assert 0.04 < top_grade < 0.3, f"Comp Hill's face leaves {top_grade * 100:.0f}% above it"
    y[k_toe:k_ht + 1] = hill
    y[k_toe - 10:k_f0 + 13] = gaussian_filter1d(y[k_toe - 26:k_f0 + 29], 5.0, mode="nearest")[16:-16]
    y[k_f1 - 8:k_ht + 5] = gaussian_filter1d(y[k_f1 - 20:k_ht + 17], 3.0, mode="nearest")[12:-12]
    # A hill everyone has a go at is as wide as a road is long, and a
    # blowout has no banks: the scoop runs out into the dune's back over
    # three times its own width.
    r["halfs"][k_toe - 4:k_ht] = 6.5
    d_, i_ = cKDTree(pts[k_toe - 8:k_ht + 4]).query(FLAT, distance_upper_bound=6.5 + X_SCOOP)
    ok_ = np.isfinite(d_)
    jj, ii = np.unravel_index(np.nonzero(ok_)[0], h.shape)
    h[jj, ii] += (y[k_toe - 8:k_ht + 4][i_[ok_]] - h[jj, ii]) * (1.0 - smoothstep(6.5, 6.5 + X_SCOOP, d_[ok_]))

    # The ground under it: only the strip the tyres have packed, the
    # sand beside it slumped back to rest, the strip trued up again.
    lay_route(h, r, 2.0)
    band = cKDTree(pts).query(FLAT, distance_upper_bound=30.0)[0]
    band = np.isfinite(band).reshape(N, N) & (f["dshore"] > 8)
    # (Not the other routes' ground: where the razorback comes down off
    # the crest beside the gate its ramp is cut steeper than sand lies,
    # and slumping it left trucks coming back up it to be winched.)
    others = cKDTree(np.vstack([q_["pts"] for q_ in routes])).query(FLAT, distance_upper_bound=14.0)[0]
    band &= ~np.isfinite(others).reshape(N, N)
    h[:] = np.where(band, settle(h, band, iters=40), h)
    lay_route(h, r, 1.5)
    xt_settle(h, razor, max(0, k_in - 30), k_in + 60)
    xt_settle(h, summit, len(summit["pts"]) - 22, len(summit["pts"]))
    r["y"] = sample(h, pts)

    # What it is made of. As on the other tracks over the dunes: deep
    # where the dune is, beaten down to plain sand on anything steep
    # (which is what makes a climb a climb and not a wall). Then the
    # trail's own: the eye deep from rim to rim, the flat packed hard,
    # and the middle of Comp Hill, its steepest part, left as deep as the
    # wind laid it.
    jj, ii, idx, d, lat = xt_band(r, float(np.max(r["halfs"])) + 0.8)
    on = d < r["halfs"][idx] + 0.6
    jj, ii, idx = jj[on], ii[on], idx[on]
    g = np.abs(np.gradient(gaussian_filter1d(r["y"], 2.0)))
    cur = surf[jj, ii]
    v = np.where((cur == S["HARDPACK"]) | (cur == S["GRASS"]), S["SAND"], cur)
    v = np.where((cur == S["DEEPSAND"]) & (g[idx] > 0.13), S["SAND"], v)
    surf[jj, ii] = np.where(water[jj, ii] > -999, cur, v)
    surf[eye] = S["DEEPSAND"]
    xt_surface(surf, r, k_eye1 + 3, k_toe + 6, S["HARDPACK"], extra=2.5)
    xt_surface(surf, r, k_toe + 6, k_f0 - 3, S["SAND"], extra=1.0)
    xt_surface(surf, r, k_f0 - 3, k_f1 + 3, S["DEEPSAND"], extra=4.0)
    xt_surface(surf, r, k_f1 + 3, k_ht, S["SAND"], extra=1.0)
    # And the belt across the foot of the charge.
    xt_surface(surf, r, k_belt, k_belt + X_BELT, S["DEEPSAND"], extra=3.0)

    # Nothing grows in it.
    tr = cKDTree(pts)
    trees[:] = [t_ for t_ in trees if tr.query((t_[0], t_[2]))[0] > 7.0]

    # What the CPU's driver knows about it: square to each brink and no
    # faster than a truck should leave one, flat out wherever a climb is
    # coming, and steady along the crests.
    pace = r["pace"]
    pace[:] = 4.5
    pace[:6] = 2.5
    pace[k_turn - 14:k_a + 2] = 3.0
    pace[k_a + 2:k_foot_a] = 9.0
    pace[k_foot_a:k_brink - 12] = 12.0
    pace[k_brink - 12:k_brink - 4] = 6.0
    pace[k_brink - 4:k_b + 2] = 3.0
    pace[k_b + 2:k_eye1 + 4] = 12.0
    pace[k_eye1 + 4:k_k - 26] = 14.0
    pace[k_k - 26:k_k - 8] = 8.0
    pace[k_k - 8:] = 4.5
    r["spots"] = [(5, "WITCH'S EYE", 1), (k_turn - 8, "THE HOOK", 0), (k_a + 2, "FIRST DROP", 0),
                  (k_foot_a + 12, "THE CHARGE", 0), (k_brink - 3, "THE BRINK", 1), (k_eye0 + 4, "THE EYE", 0),
                  (k_line, "THE LINE-UP", 1), (k_toe + 8, "COMP HILL", 0), (k_k + 6, "THE BLADE", 1)]

    # Whips: a pair either side of each brink, where to go over; one at
    # the line-up and one at the top of the hill to aim for.
    props = []
    for k in (k_a, k_b):
        for side in (-1, 1):
            q_ = pts[k] - w * 3.0 + c * side * 5.5
            props.append((q_[0], q_[1], rng.uniform(0, 6.28), PROP["FLAG"], 0))
    for k, off in ((k_line, 6.0), (k_k - 2, -6.5)):
        q_ = pts[k] + c * off
        props.append((q_[0], q_[1], rng.uniform(0, 6.28), PROP["FLAG"], 0))
    p_, tg_, nm_ = frame(r, 9)
    yaw0 = math.atan2(tg_[0], tg_[1])
    props.append(tuple(p_ - nm_ * 4.2) + (yaw0, PROP["PLATE"], 3))
    props.append(tuple(pts[14] - nm_ * 4.6) + (yaw0, PROP["SIGN"], 0))
    view = (L(te)[0] + c[0] * (er * en + 9.0), L(te)[1] + c[1] * (er * en + 9.0), "WITCH'S EYE")

    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    grade = np.diff(r["y"]) / np.maximum(seg, 1e-3)
    y = r["y"]
    print(f"  WITCH'S EYE {seg.sum():.0f} m from the razorback at ({pts[0][0]:.0f},{pts[0][1]:.0f}): the line crosses A's crest "
          f"at {tA:.0f} ({XA[0]:.0f},{XA[1]:.0f}), the big ridge's at {tB:.0f} ({cB[kB][0]:.0f},{cB[kB][1]:.0f}), the king's at "
          f"{tK:.0f} ({XK[0]:.0f},{XK[1]:.0f})")
    print(f"    first drop {y[k_a] - y[k_foot_a]:.1f} m, the charge {y[k_b] - y[k_foot_a:k_b].min():.1f} m at up to "
          f"{grade[k_foot_a:k_b].max() * 100:.0f}%, the drop into the eye {y[k_b] - y[k_eye]:.1f} m, out of it "
          f"{y[k_eye1] - y[k_eye]:.1f} m at up to {grade[k_eye:k_eye1 + 4].max() * 100:.0f}%, the flat {k_toe - k_eye1} m, "
          f"Comp Hill {y[k_k] - y[k_toe]:.1f} m in {k_k - k_toe} m, its face {y[k_f1] - y[k_f0]:.1f} m at up to "
          f"{grade[k_f0:k_f1].max() * 100:.0f}% (the back above it {top_grade * 100:.0f}%), the blade {n - k_k} m")
    full = xt_full(r, before=[(razor, 0, k_in)], after=[(summit, len(summit["pts"]) - 5, len(summit["pts"]))])
    return full, props, view


# ---------------------------------------------------------------------------
# The land beyond the map (outland.py): what the king dune looks out on.
#
# The basin is flat, and from 70 m up most of what there is to see is
# past the map's edge: the reservoir running on to the south and round
# the east side, full of drowned dunes; the dune field carrying on west;
# and on the bench to the north the irrigated circles the Columbia Basin
# Project turned the sagebrush into.

O_RIM = [(-1300, 300), (-1000, 262), (-760, 226), (-600, 206)] + RIM + [(600, -96), (760, -70), (920, -128), (1300, -150)]
# The reservoir's shore beyond the map, from the map's south-west corner
# round to its east side (the map's own edges close it).
O_LAKE = [(-384, -324), (-470, -330), (-600, -352), (-720, -404), (-836, -500), (-924, -640), (-950, -800),
          (-890, -936), (-700, -1004), (-400, -1036), (0, -1056), (400, -1030), (800, -1000), (1152, -964), (1340, -960),
          (1340, -560), (1040, -540), (868, -500), (744, -404), (700, -284), (712, -160), (676, -64), (592, -12),
          (484, -30), (384, -52), (384, -384), (-384, -384)]
# Drowned dunes out there: (x, z, half-length along the crest, half-width, height).
O_ISLANDS = [(-560, -470, 80, 40, 5.0), (-300, -520, 110, 46, 6.5), (-90, -470, 70, 38, 4.5), (150, -540, 120, 50, 7.0),
             (420, -470, 84, 40, 5.0), (620, -600, 110, 48, 6.0), (-700, -640, 90, 44, 5.5), (-440, -720, 130, 54, 7.0),
             (-120, -690, 100, 46, 6.0), (300, -760, 140, 56, 7.5), (640, -820, 110, 48, 6.0), (900, -700, 100, 46, 5.5),
             (20, -880, 130, 52, 6.5), (-600, -880, 100, 46, 5.5), (1040, -840, 90, 44, 5.0), (540, -250, 60, 34, 4.0),
             (600, -420, 70, 36, 4.5)]
# The circles: (colour, share of them).
FIELDS = [((92, 128, 60), 0.3), ((120, 150, 70), 0.2), ((200, 170, 98), 0.2), ((152, 126, 94), 0.12)]


def o_inside(poly):
    p = np.asarray(poly, dtype=float)
    m = np.zeros(ol.OX.shape, dtype=bool)
    for a, b in zip(p, np.roll(p, -1, axis=0)):
        if a[1] == b[1]:
            continue
        m ^= ((a[1] > ol.OZ) != (b[1] > ol.OZ)) & (ol.OX < (b[0] - a[0]) * (ol.OZ - a[1]) / (b[1] - a[1]) + a[0])
    return m


def build_outland(h, water, alb_ao):
    OX, OZ, OUT = ol.OX, ol.OZ, ol.OUT
    E = ol.extrude(h)
    p = OX * WIND[0] + OZ * WIND[1]
    q = OX * CREST[0] + OZ * CREST[1]
    n1 = fbm(OX, OZ, 70, 3, 701)
    n2 = fbm(OX, OZ, 240, 3, 702)
    low = 2.4 + 1.6 * (n2 - 0.5) * 2 + 0.6 * (n1 - 0.5)
    # The dune field carries on west and thins out: long swells across
    # the wind, which is all of a dune this grid can hold.
    west = smoothstep(-300, -480, OX) * smoothstep(-330, -250, OZ) * smoothstep(-1150, -900, OX)
    swell = 0.5 + 0.5 * np.sin(p * (2 * math.pi / 190.0) + 3.0 * (fbm(q, p * 0.3, 160, 2, 703) - 0.5) * 2)
    dunes = west * (5.0 + 9.0 * swell ** 2 * (0.5 + n2))
    P = low + dunes
    # The bench: rimrock in the east, a long ramp of sand in the west.
    dn = ol.side_dist(O_RIM, left=True) + 14 * (fbm(OX, OZ, 110, 3, 704) - 0.5) * 2
    bench = BENCH_Y + 2.0 + 3.0 * (n2 - 0.5) * 2 + 0.014 * np.clip(dn, 0, 900)
    hard = smoothstep(-120, 80, OX)
    up = ol.cliff(dn, bench - P, talus=0.3) * hard + (bench - P) * smoothstep(-140, 60, dn) * (1 - hard)
    P = P + up
    # The reservoir.
    lake = o_inside(O_LAKE)
    ring = np.array(O_LAKE + O_LAKE[:1], dtype=float)
    ds, _, _ = ol.line_dist(ring, smooth=1)
    bed = LAKE_Y - np.clip(ds * 0.05, 0.3, 2.6)
    P = np.where(lake, bed, np.maximum(P, LAKE_Y + 0.5 + 0.02 * np.clip(ds, 0, 200) * (dn < 0)))
    isl = np.zeros(P.shape)
    for k, (x, z, hl, hw, hh) in enumerate(O_ISLANDS):
        u = ((OX - x) * CREST[0] + (OZ - z) * CREST[1]) / hl
        v = ((OX - x) * WIND[0] + (OZ - z) * WIND[1]) / hw
        r2 = u * u + v * v + 0.3 * (fbm(OX, OZ, 60, 2, 710 + k) - 0.5)
        top = LAKE_Y - 0.8 + (hh + 0.8) * np.clip(1 - r2, 0, 1) ** 0.8
        isl = np.maximum(isl, np.clip(1 - r2, 0, 1) * (top > LAKE_Y))
        P = np.where(lake, np.maximum(P, np.where(r2 < 1, top, -99)), P)
    # Joined to the map: its border's ground carried out, giving way to
    # this over the first 170 m.
    w = smoothstep(0, 170, OUT)
    H = E * (1 - w) + P * w
    H[ol.INNER] = E[ol.INNER]
    H[ol.BORDER] = E[ol.BORDER]

    face, fx, fz = ol.faces(H, drop=9.0)
    nrm = ol.normals(H)
    nrm = np.where(face[..., None], np.dstack([fx * 0.94, np.full_like(H, 0.34), fz * 0.94]), nrm)
    # Colours: brush country, sand where the dunes run, basalt on the
    # walls, the bed under the water, and the fields.
    cheat = smoothstep(0.42, 0.66, fbm(OX, OZ, 130, 3, 720))[..., None]
    sagek = smoothstep(0.5, 0.7, fbm(OX, OZ, 90, 3, 721))[..., None] * 0.55
    alb = STEPPE * (1 - 0.6 * cheat) + CHEAT * 0.6 * cheat
    alb = alb * (1 - sagek) + SAGE * sagek
    sandy = np.clip(west * (0.35 + 0.65 * swell), 0, 1)[..., None]
    alb = alb * (1 - sandy) + (SAND * (0.94 + 0.12 * n1[..., None])) * sandy
    # Centre pivots: circles on a loose grid over the bench and the far
    # lowlands, each a crop, stubble or fallow.
    rng = np.random.default_rng(1952)
    farm = ((dn > 90) | (OZ < -1010) | (OX > 760)) & ~lake & (OUT > 60)
    R = 118.0
    for gz in np.arange(-1152 + 150, 1152, 268.0):
        for gx in np.arange(-1152 + 140, 1152, 268.0):
            cx, cz = gx + rng.uniform(-22, 22), gz + rng.uniform(-22, 22)
            pick = rng.random()
            acc = 0.0
            col = None
            for c_, share in FIELDS:
                acc += share
                if pick < acc:
                    col = np.array(c_, dtype=float)
                    break
            if col is None:
                continue
            d = np.hypot(OX - cx, OZ - cz)
            m = (d < R) & farm
            if m.sum() < 40:
                continue                 # cut by the rim or the water: no farmer would
            # (a wedge of it a shade different: half cut, or watered today)
            ang = np.arctan2(OZ - cz, OX - cx)
            wedge = ((ang - rng.uniform(-3, 3)) % (2 * math.pi)) < rng.uniform(0.5, 2.4)
            alb = np.where(m[..., None], col * np.where(wedge, 0.88, 1.0)[..., None], alb)
    # Rimrock.
    gz_, gx_ = np.gradient(H, ol.CELL)
    sl = np.hypot(gx_, gz_)
    tal = (smoothstep(0.2, 0.42, sl) * (dn > -90) * (dn < 40) * hard)[..., None]
    alb = alb * (1 - tal) + TALUS * 1.1 * tal
    alb = np.where(face[..., None], BASALT * 1.05, alb)
    # Islands: a rim of sand, willows and olives on top.
    shore_k = (smoothstep(60, 0, ds) * ~lake)[..., None] * (dn < 0)[..., None]
    alb = alb * (1 - 0.7 * shore_k) + SAND_DAMP * 0.7 * shore_k
    alb = np.where(lake[..., None], SAND_WET * 0.8, alb)
    ik = isl[..., None]
    isle = SAND * (1 - smoothstep(0.2, 0.55, isl)[..., None]) + np.array([126.0, 138.0, 92.0]) * smoothstep(0.2, 0.55, isl)[..., None]
    alb = np.where(ik > 0, isle, alb)
    alb *= (0.93 + 0.14 * n1)[..., None]
    w2 = smoothstep(0, 110, OUT)[..., None]
    alb = ol.extrude(alb_ao) * (1 - w2) + alb * w2
    hz = ol.horizon_map(H)
    fade = smoothstep(600, 768, OUT)
    # The water: the lake in bands the game can cull one stretch at a
    # time, north bank then south (or west then east), 24 m to a pair.
    ribbons = []
    for z0, z1 in ((-384, -556), (-556, -728), (-728, -900), (-900, -1064)):
        ribbons.append([(x, z0, x, z1, LAKE_Y) for x in np.arange(-984, 1345, 24.0)])
    for x0, x1 in ((384, 556), (556, 728)):
        ribbons.append([(x0, z, x1, z, LAKE_Y) for z in np.arange(-384, 13, 24.0)])
    return {"h": H, "alb": alb, "nrm": nrm, "fade": fade, "hz": hz, "ribbons": ribbons}


def report_surfaces(surf, routes):
    for r in routes:
        s = sample(surf.astype(float), r["pts"], order=0).astype(int)
        mix = ", ".join(f"{SURFACES[k].lower()} {100 * np.mean(s == k):.0f}%" for k in np.unique(s))
        print(f"    {r['name']:10s} {mix}")


def build(out, preview_path=None, raw=None):
    print("Moses Lake")
    rng = np.random.default_rng(1962)
    h, water, f, routes = build_ground()
    knobs(h, f)
    f["face"], f["face_x"], f["face_z"] = faces(h)
    f["face"] &= (f["lift"] < 1.0) & ((f["cliff"] > 2.5) | f["knob"] | (gaussian_filter(f["knob"].astype(float), 1.5) > 0.05))
    surf = surfaces(h, water, f, routes)
    trees = place_trees(h, water, surf, f, routes, rng)
    # The extreme trail, once everything that was here before it is placed
    # (so none of that moves for it).
    extreme, x_props, x_view = witchs_eye(h, water, surf, f, routes, trees, np.random.default_rng(1971))

    def gy(x, z):
        return float(sample(h, np.array([x, z]))[0])
    sp = set_pieces(h, water, f, routes, np.random.default_rng(1980))
    spheres, capsules, posts = sp["spheres"], sp["capsules"], sp["posts"]
    boxes = sp["boxes"] + sp["prop_boxes"]
    rock_boxes = len(sp["boxes"])
    stuff = [(s_[0], s_[2]) for s_ in spheres] + [(b[0], b[2]) for b in boxes] + [(q[0], q[2]) for q in posts] + \
            [(p_[0], p_[1]) for p_ in sp["props"]] + [((c[0] + c[3]) / 2, (c[2] + c[5]) / 2) for c in capsules]
    keep = cKDTree(np.array(stuff))
    trees = [(t[0], gy(t[0], t[2]) - t[6], t[2], t[3], t[4], t[5]) for t in trees if keep.query((t[0], t[2]))[0] > 4.5]
    trees += [(t[0], gy(t[0], t[2]) - t[6], t[2], t[3], t[4], t[5]) for t in sp["trees"]]
    # (the trail's own stretch is worn into the sand as the other tracks are)
    worn = {"pts": extreme["pts"][extreme["own0"]:extreme["own1"] + 1], "half": extreme["half"]}
    argb, rgb, alb_ao, nrm = colours(h, water, surf, f, routes + [worn])
    props = [(p[0], gy(p[0], p[1]) if p[4] is None else p[4], p[1], p[2], p[3]) for p in sp["props"]]
    props += [(p[0], gy(p[0], p[1]), p[1], p[2], p[3], p[4]) for p in x_props]
    views = [(v[0], gy(v[0], v[1]), v[1], v[2]) for v in sp["views"]]
    views.append((x_view[0], gy(x_view[0], x_view[1]), x_view[1], x_view[2]))
    props.append((x_view[0] + 3.0, gy(x_view[0] + 3.0, x_view[1] + 2.5), x_view[1] + 2.5, 1.0, PROP["FLAG"]))
    # The powerline's frames were stood without a thought for the trail:
    # it must pass well clear of every one.
    xt = cKDTree(extreme["pts"][extreme["own0"]:])
    for p in props:
        if p[4] == PROP["PYLON"]:
            assert xt.query((p[0], p[2]))[0] > 14.0, f"a powerline frame at {p[0]:.0f},{p[2]:.0f} stands in the extreme trail"
    routes = routes + [extreme]
    summit_y = gy(SUMMIT[0], SUMMIT[1])
    spawns = []
    for k in range(4):
        sx = SPAWN[0] + (k - 1.5) * 6.0
        sz = SPAWN[1] - abs(k - 1.5) * 2.0
        spawns.append((sx, gy(sx, sz), sz, 0.0))

    out.mkdir(parents=True, exist_ok=True)
    safe = recover_spots(h, water, routes, spheres, capsules, boxes, posts, trees)
    light = light_inputs(alb_ao, nrm, h, water, flags=vertex_flags(f["face"], safe))
    hz = horizon_map(h)
    blob = pack_level(h, water, surf, light, hz)
    (out / "level_dunes.lvl").write_bytes(blob)
    o = build_outland(h, water, alb_ao)
    oblob = ol.pack(o["h"], o["alb"], o["nrm"], o["fade"], o["hz"], o["ribbons"])
    (out / "level_dunes.out").write_bytes(oblob)
    if raw:
        raw.write_bytes(raw_level(h, argb, water, surf, light, hz))
    write_header(out / "level_dunes.h", "dunes", {
        "name": "MOSES LAKE", "blurb": "SAND DUNES ON THE COLUMBIA BASIN", "base_ft": 1046,
        "lake_y": LAKE_Y, "start_y": gy(SPAWN[0], SPAWN[1]), "summit": (SUMMIT[0], summit_y, SUMMIT[1]),
        "routes": routes, "spheres": spheres, "capsules": capsules, "boxes": boxes, "rock_boxes": rock_boxes,
        "posts": posts, "trees": trees, "props": props, "views": views, "spawns": spawns,
        "mists": [(-60, -340, 60), (130, -344, 60), (300, -150, 55), (f["ponds"][0][0], f["ponds"][0][1], 34), (-196, -276, 40)],
        "hawks": [(20, -236, 40, 40, 0.0, 1), (290, -110, 34, 36, 2.0, -1), (60, 190, 44, 30, 4.0, 1)],
        "herds": [(226, -84), (150, 110)],
        "rocks": [],
    })
    # The map: the water drawn in, as a park's would have it.
    wet = (water > h + 0.05)[..., None]
    minimap(np.where(wet, np.array([96.0, 150.0, 176.0]) * 0.8 + rgb * 0.2, rgb * 0.94), water, routes, [], out / "level_map_dunes.png")
    print(f"  heights {h.min():.1f}..{h.max():.1f} m, summit {summit_y:.1f} m, start {gy(SPAWN[0], SPAWN[1]):.1f} m, "
          f"{int((water > -999).sum())} wet vertices")
    report(h, routes)
    report_surfaces(surf, routes)
    print(f"  {len(spheres)} boulders, {len(boxes)} boxes, {len(trees)} trees, {len(views)} viewpoints, "
          f"level_dunes.lvl {len(blob)} bytes")
    if preview_path:
        preview_image(rgb, water, routes, spheres, capsules, boxes, [], views, preview_path)
        az, el = math.degrees(math.atan2(SUN[0], SUN[2])) % 360, math.degrees(math.asin(SUN[1]))
        orgb = ol.lit(o["alb"], o["nrm"], o["hz"], az, el, (0.62, 0.66, 0.72), (0.62, 0.577, 0.496))
        land = [ol.mesh(o["h"], orgb)] + [ol.ribbon_mesh(r, WATER_RGB) for r in o["ribbons"]]
        preview(h, rgb, water, preview_path.with_name("level-dunes-views.png"), land=land, spheres=spheres, boxes=boxes, trees=trees,
                tree_rgb=((132, 140, 112), (150, 150, 70), (120, 136, 70)))


if __name__ == "__main__":
    import sys
    build(Path(sys.argv[1]), Path(sys.argv[2]) / "level-dunes.png" if len(sys.argv) > 2 else None)
