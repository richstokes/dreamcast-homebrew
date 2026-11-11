"""Ridgeline's second area: Mesa Grande, red rock canyon country.

The land is laid out by hand, not found in noise: a river under a wall
along the south edge, sand flats to play on, then three tiers of sandstone
stepping up to the north (a cliff over a talus slope each, a bench on
top), the last a tilted mesa whose far edge drops sheer into a basin of
towers. Three ways climb the first two tiers from the fork on the flats
and meet under an arch on the high bench:

  rim road   a graded dirt road: switchbacks up the talus where each
             cliff has fallen in, a squeeze under a leaning slab (east)
  the fins   slickrock: straight up the spines of sandstone fins, steep
             and bare, with the grip to do it (centre)
  canyon     up the wash into a canyon: sand, boulders and rock ledges
             that get taller toward its head (west)

and one trail goes on from the arch: a shelf across the mesa's cliff,
then ledges over its tilted top to the overlook.

Every route is a line drawn here with its heights; the ground is built
to it (a fin raised under the line, a canyon cut along it, a road bench
cut and filled), so what the CPU drivers follow is what was designed.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from scipy.ndimage import gaussian_filter, gaussian_filter1d
from scipy.spatial import cKDTree

from levelkit import *  # noqa: F401,F403
import level_view
import outland as ol

# ---------------------------------------------------------------------------
# The plan (x east, z north, metres)

SPAWN = np.array([0.0, -276.0])
TRAILHEAD = np.array([4.0, -238.0])
FORK = np.array([8.0, -198.0])
MEET = np.array([8.0, 68.0])           # under the arch on the high bench
SUMMIT = np.array([24.0, 207.0])

FLATS_Y = 5.0
TIER = (30.0, 34.0)                    # the first two cliffs' rise
MESA_RISE = (30.0, 55.0)               # the mesa's south rim, and its north edge, over the high bench
MESA_STEP = 2.2                        # the height of each ledge its top climbs in
BASIN_Y = 14.0

# Each tier is a plateau: everything inside its outline stands a cliff
# higher. Outlines run counter-clockwise and off the map where the plateau
# does (west, and north where the great cliff cuts them all). The rims
# swing in and out: a bay where the fins climb, a point beside it, a
# re-entrant the canyon comes out of, a bay where the wall has fallen in.
TIER1 = [(-470, -128), (-384, -131), (-322, -150), (-268, -166), (-222, -150), (-180, -134), (-142, -150),
         (-100, -172), (-58, -158), (-24, -128), (6, -106), (40, -100), (72, -112), (98, -140), (120, -166),
         (154, -170), (186, -146), (220, -136), (250, -142), (280, -160), (306, -148), (318, -108), (304, -58),
         (316, -6), (302, 64), (312, 136), (300, 260), (300, 470), (-470, 470)]
TIER2 = [(-470, -18), (-384, -22), (-320, -36), (-264, -8), (-214, -30), (-170, -36), (-124, -14), (-84, 4),
         (-50, -8), (-28, 14), (-12, 40), (8, 58), (30, 52), (54, 30), (82, 2), (104, -22), (136, -28), (166, -10),
         (196, -2), (230, -12), (254, 10), (264, 58), (254, 120), (264, 190), (256, 260), (256, 470), (-470, 470)]
MESA = [(-140, 116), (-104, 96), (-62, 104), (-24, 92), (28, 98), (72, 86), (118, 94), (160, 90), (196, 110),
        (214, 146), (220, 200), (206, 262), (206, 470), (-150, 470), (-164, 190), (-152, 140)]
# The upland along the west side, a wall over everything: east of this
# line is the drive.
WEST = [(-318, -470), (-326, -384), (-312, -300), (-334, -224), (-316, -150), (-338, -72), (-312, 0), (-330, 84),
        (-306, 170), (-320, 250), (-310, 470)]
# The great cliff along the north: north of this line the land is gone.
# The point in the middle is the overlook.
NORTH = [(-470, 224), (-384, 220), (-300, 228), (-210, 214), (-120, 222), (-50, 210), (-4, 212), (24, 222),
         (50, 211), (110, 216), (190, 208), (270, 218), (384, 212), (470, 216)]
# The river along the south edge; its far bank is a wall straight out of it.
RIVER = [(-470, -344), (-384, -340), (-250, -346), (-120, -336), (0, -342), (120, -334), (250, -344), (384, -338),
         (470, -340)]


def chaikin(poly, iters=3, closed=False):
    p = np.asarray(poly, dtype=float)
    for _ in range(iters):
        q = []
        n = len(p)
        for i in range(n if closed else n - 1):
            a, b = p[i], p[(i + 1) % n]
            q.append(a * 0.75 + b * 0.25)
            q.append(a * 0.25 + b * 0.75)
        if not closed:
            q = [p[0]] + q + [p[-1]]
        p = np.array(q)
    return p


FLAT = np.stack([X.ravel(), Z.ravel()], axis=1)


def line_dist(poly, step=0.75):
    """Distance from every vertex to a line, and how far along it (m)."""
    pts = resample(np.asarray(poly, dtype=float), step)
    d, idx = cKDTree(pts).query(FLAT)
    return d.reshape(N, N), (idx * step).reshape(N, N), pts


def inside(poly):
    """Even-odd: which vertices lie inside a closed outline."""
    p = np.asarray(poly, dtype=float)
    x, z = X, Z
    m = np.zeros((N, N), dtype=bool)
    for a, b in zip(p, np.roll(p, -1, axis=0)):
        if a[1] == b[1]:
            continue
        cross = ((a[1] > z) != (b[1] > z)) & (x < (b[0] - a[0]) * (z - a[1]) / (b[1] - a[1]) + a[0])
        m ^= cross
    return m


def plateau_dist(outline, round_off=3):
    """Signed distance to a plateau's rim: positive inside."""
    p = chaikin(outline, round_off, closed=True)
    d, _, _ = line_dist(np.vstack([p, p[:1]]))
    return np.where(inside(p), d, -d)


def side_dist(line, left=True):
    """Signed distance to an open line: positive to its left (looking
    along it), or its right."""
    p = chaikin(line, 3)
    pts = resample(p, 0.75)
    d, idx = cKDTree(pts).query(FLAT)
    tg = np.gradient(pts, axis=0)
    rel = FLAT - pts[idx]
    cr = tg[idx, 0] * rel[:, 1] - tg[idx, 1] * rel[:, 0]
    s = np.where(cr > 0, d, -d).reshape(N, N)
    return s if left else -s


def scarp(d, rise, talus=0.3, slope=0.64, face=6.4):
    """The height gained crossing a rim (d metres inside it): a talus
    slope piled against the foot of the cliff up to talus of the rise,
    steepening toward the wall, then the face."""
    rise = np.broadcast_to(np.asarray(rise, dtype=float), d.shape)
    tw = np.maximum(talus * rise / slope, 1e-3)
    t = np.clip((d + tw) / tw, 0, 1)
    y = talus * rise * t ** 1.3
    return y + (1 - talus) * rise * np.clip(d / face, 0, 1)


def rim_wobble(seed, big=7.0, small=2.2):
    """Metres to shift a rim by: alcoves and promontories, and the
    buttresses along a face."""
    return big * 2 * (fbm(X, Z, 48, 3, seed) - 0.5) + small * (ridged(X, Z, 15, 2, seed + 7) - 0.5) * 2


# ---------------------------------------------------------------------------
# The ground

def base_terrain():
    """The tiers, the walls and the lowlands, before anything is built on
    them. Returns the heights and the fields later steps need."""
    f = {}
    # The flats tilt gently up from the river to the foot of the cliffs.
    h = FLATS_Y + 4.5 * smoothstep(-330, -170, Z) + 0.9 * (fbm(X, Z, 60, 3, 301) - 0.5)
    f["flats"] = h.copy()
    d1 = plateau_dist(TIER1) + rim_wobble(311)
    d2 = plateau_dist(TIER2) + rim_wobble(312)
    d3 = plateau_dist(MESA) + rim_wobble(313, 5.0, 2.0)
    f["d1"], f["d2"], f["d3"] = d1, d2, d3
    h = h + scarp(d1, TIER[0], talus=0.34) + scarp(d2, TIER[1], talus=0.3)
    # The benches are not dead level: they dip toward their rims and roll.
    h += smoothstep(0, 60, d1) * (1.5 + 1.2 * (fbm(X, Z, 70, 3, 321) - 0.5) * 2)
    h += smoothstep(0, 60, d2) * (2.0 + 1.4 * (fbm(X, Z, 70, 3, 322) - 0.5) * 2)
    # The mesa: a slab tilted up to the north, where it is highest at its
    # very edge.
    tilt = MESA_RISE[0] + (MESA_RISE[1] - MESA_RISE[0]) * smoothstep(96, 208, Z)
    # Its top is bedded rock: it climbs in ledges, each bed's edge a step
    # a couple of metres high wandering across it, rather than as a ramp.
    bedding = tilt + 2.6 * (fbm(X, Z, 55, 3, 324) - 0.5) * 2
    t = bedding / MESA_STEP
    stepped = (np.floor(t) + smoothstep(0.8, 1.0, t - np.floor(t))) * MESA_STEP
    tilt = tilt + (stepped - tilt) * smoothstep(8, 20, d3)
    h = h + scarp(d3, tilt, talus=0.24)
    f["bench_y"] = h.copy()
    # The west upland: three cliffs stepped back, over the lot.
    dw = side_dist(WEST, left=True) + rim_wobble(314, 9.0, 2.5)
    f["dw"] = dw
    up = scarp(dw, 44, talus=0.4) + scarp(dw - 34, 46, talus=0.2) + scarp(dw - 70, 40, talus=0.15)
    up += smoothstep(70, 120, dw) * 3 * (fbm(X, Z, 60, 3, 323) - 0.3)
    h = np.maximum(h, f["flats"] + up)
    # The river's far bank.
    ds = side_dist(RIVER, left=False) - 13.0 + rim_wobble(315, 2.0, 1.2)
    f["ds"] = ds
    # Its top steps and sags: a long wall is never one height.
    top = 70 + 26 * fbm(X, Z * 0.2, 90, 3, 317) + 8 * np.round(2 * fbm(X, Z * 0.2, 150, 2, 318))
    f["south"] = scarp(ds, top, talus=0.0, face=6.4)
    # The great cliff: everything north of the line is cut away down to
    # the basin, its foot buried in talus.
    # (steadier at the overlook's point, so it stays a point)
    calm = 1.0 - np.exp(-(((X - 24) / 60.0) ** 2))
    dn = side_dist(NORTH, left=False) + rim_wobble(316, 6.0, 2.5) * calm
    f["dn"] = dn
    basin = BASIN_Y + 5 * (fbm(X, Z, 80, 3, 331) - 0.5) + 0.02 * np.clip(Z - 220, 0, 200)
    h = np.minimum(h, basin + scarp(dn, 190, talus=0.2, slope=0.7, face=7.0))
    f["basin"] = dn < -2
    return h, f


def carve_river(h, south):
    """The river along the south edge: a channel too deep to wade except
    over the bar off the beach. Returns the water's surface."""
    d, s, pts = line_dist(chaikin(RIVER, 3))
    half = 11.0
    bed = -1.6 - 1.8 * np.clip(1 - (d / half) ** 2, 0, 1)
    bank = smoothstep(half + 12, half, d)
    # The surface falls a metre and a half across the map, so it flows east.
    level = 1.6 - 1.5 * (X + 384) / 768
    hr = level + bed
    h2 = np.where(d < half + 12, np.minimum(h, hr * bank + h * (1 - bank)), h)
    # The beach by the start: a sand bar shelving in, shallow enough to
    # drive through half way over.
    bx = np.exp(-((X - 6) / 46.0) ** 2)
    shelf = level - 0.25 - 0.045 * np.clip(-(Z + 322), 0, 30)
    h2 = np.where((Z > -346) & (Z < -300), h2 * (1 - bx) + np.minimum(h2 + 2.0, np.maximum(shelf, h2)) * bx, h2)
    water = np.where(h2 < level - 0.05, level, -1000.0)
    water[Z > -300] = -1000.0
    # The far bank's wall stands straight out of the water.
    h2 = np.maximum(h2, level - 2.0 + south)
    water[h2 > level] = -1000.0
    return h2, water


# ---------------------------------------------------------------------------
# Routes: drawn here with their heights, the ground built to them

# (x, z, height or None for the ground as it lies). The line is rounded
# off between the points.
R_APPROACH = [(0, -262, None), (2, -246, None), (4, -230, None), (7, -214, None), (8, -198, None)]
R_RIM = [
    (8, -198, None), (44, -204, None), (96, -208, None), (150, -200, None), (178, -192, 10.5),
    # The first dugway: three legs up the talus where the wall has fallen in.
    (214, -190, 15.0), (246, -186, 19.2), (257, -181, 20.4), (258, -174, 21.0), (249, -170, 22.0),
    (220, -171, 25.8), (192, -170, 29.4), (181, -166, 30.6), (180, -159, 31.2), (189, -155, 32.2),
    (216, -156, 35.6), (238, -152, 38.6), (246, -144, 40.2), (240, -132, None),
    # Along the first bench to the foot of the second.
    (228, -108, None), (222, -82, None), (218, -62, 43.4),
    # The second dugway: four legs.
    (196, -58, 46.2), (176, -54, 48.8), (166, -50, 50.0), (165, -43, 50.6), (174, -39, 51.6),
    (198, -40, 54.8), (222, -38, 58.0), (232, -34, 59.2), (233, -27, 59.8), (224, -23, 60.8),
    (200, -24, 64.0), (180, -22, 66.8), (170, -18, 68.0), (169, -11, 68.6), (178, -7, 69.6),
    (198, -6, 72.4), (214, 0, 75.0), (222, 12, 77.2), (222, 28, None),
    # West along the high bench to the arch.
    (204, 44, None), (168, 50, None), (128, 58, None), (92, 66, None), (56, 74, None), (28, 76, None), (8, 68, None),
]
R_FINS = [
    (8, -198, None), (12, -190, None), (16, -180, 11.0), (20, -164, 16.5), (24, -148, 22.5), (27, -134, 27.5),
    (30, -120, 33.0), (31, -108, 37.5), (28, -96, 40.5), (30, -84, None),
    # Across the bench.
    (40, -74, None), (54, -66, None), (66, -58, 43.0),
    # The second fin, up into the bay under the arch.
    (64, -44, 48.0), (56, -28, 54.5), (46, -12, 60.5), (36, 4, 66.0), (26, 20, 71.0), (16, 36, 75.0), (10, 52, 77.0),
    (8, 68, None),
]
R_CANYON = [
    (8, -198, None), (-28, -204, None), (-76, -206, None), (-128, -200, None), (-168, -176, 9.4),
    (-186, -150, 10.2), (-184, -126, 11.5), (-204, -98, 15.0), (-172, -72, 19.0), (-198, -44, 25.0), (-176, -18, 31.0),
    (-196, 8, 38.0), (-164, 28, 45.0), (-188, 52, 52.0), (-152, 68, 59.0), (-120, 52, 65.0), (-94, 64, 71.0),
    (-68, 62, 76.5), (-30, 70, None), (8, 68, None),
]
R_SUMMIT = [
    (8, 68, None), (-14, 76, None), (-30, 86, 80.0),
    # The shelf: east across the mesa's south cliff.
    (-22, 96, 82.0), (4, 102, 87.0), (28, 104, 92.0), (52, 98, 97.0), (72, 92, 101.5), (96, 96, 105.5),
    (110, 106, None),
    # Over the top, up its ledges one after another, to the point.
    (116, 126, None), (104, 146, None), (80, 158, None), (56, 158, None), (40, 172, None), (30, 190, None),
    (24, 207, None),
]


def route_line(h, ctrl, gmax, name):
    """A route from its control points: the line rounded off, a point a
    metre, and its heights (fixed where given, the ground's elsewhere),
    smoothed."""
    c = np.array([(x, z) for x, z, _ in ctrl], dtype=float)
    pts = resample(smooth_path(c, 3), 1.0)
    tree = cKDTree(pts)
    at = np.array([tree.query(q)[1] for q in c])
    ys = np.array([float(sample(h, q)[0]) if y is None else y for (_, _, y), q in zip(ctrl, c)])
    y = np.interp(np.arange(len(pts)), at, ys)
    y = gaussian_filter1d(y, 3.0, mode="nearest")
    # Between two points left to the ground the line follows it, steps
    # and all (only rounded enough to drive).
    ground = gaussian_filter1d(sample(h, pts), 1.2, mode="nearest")
    free = np.zeros(len(pts))
    for k in range(len(ctrl) - 1):
        if ctrl[k][2] is None and ctrl[k + 1][2] is None:
            free[at[k]:at[k + 1] + 1] = 1.0
    free = gaussian_filter1d(free, 2.0, mode="nearest")
    y = y * (1 - free) + ground * free
    return {"name": name, "pts": pts, "y": y, "gmax": gmax}


def terrace(r, s0, s1, risers, spread=6.0, flat=0.9):
    """Redistribute the climb between points s0 and s1 of route r into
    risers (points along it) with easier ground between: a canyon floor's
    pour-offs."""
    y = r["y"]
    k = np.arange(s0, s1)
    w = np.full(len(k), flat)
    for q in risers:
        w += np.exp(-((k - q) / spread) ** 2)
    c = np.concatenate([[0], np.cumsum(w)])[:-1] / w.sum()
    y[s0:s1] = y[s0] + (y[s1] - y[s0]) * c


def raise_fin(h, pts, y, half, crown=1.6, side=2.0, mask=None):
    """A sandstone fin under a line: a rounded crest half wide at the
    line's heights, falling away steeply either side. Returns how near the
    crest each vertex is (1 on it, 0 off the fin)."""
    tree = cKDTree(pts)
    d, idx = tree.query(FLAT, distance_upper_bound=60.0)
    ok = np.isfinite(d)
    jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
    d = d[ok]
    yc = y[idx[ok]]
    w = half[idx[ok]] if np.ndim(half) else np.full(len(d), half)
    over = np.maximum(d - w, 0)
    prof = yc - crown * np.minimum(d / w, 1.0) ** 2 - side * over - 0.03 * over ** 2
    cur = h[jj, ii]
    on = prof > cur
    h[jj, ii] = np.maximum(cur, prof)
    slick = np.zeros((N, N))
    slick[jj[on], ii[on]] = 1.0
    return slick


def cut_canyon(h, pts, y, half, wall=4.5, lip=0.0):
    """A canyon along a line: a floor half wide at the line's heights,
    walls rising steeply from it to whatever ground was there."""
    tree = cKDTree(pts)
    d, idx = tree.query(FLAT, distance_upper_bound=70.0)
    ok = np.isfinite(d)
    jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
    d = d[ok]
    yc = y[idx[ok]]
    w = half[idx[ok]] if np.ndim(half) else np.full(len(d), half)
    e = np.maximum(d - w, 0)
    prof = yc + wall * e + lip * np.minimum(e, 3.0)
    cur = h[jj, ii]
    h[jj, ii] = np.minimum(cur, prof)
    cut = np.zeros((N, N))
    cut[jj, ii] = np.clip((cur - prof) / 3.0, 0, 1) * (d < w + 1.5)
    return cut


def talus_fill(h, a, b, top, grade=0.56, inside=None):
    """Where a cliff has fallen in: debris piled to its top along the rim
    from a to b, fanning out and down from there."""
    a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    e = b - a
    t = np.clip(((X - a[0]) * e[0] + (Z - a[1]) * e[1]) / (e @ e), 0, 1)
    d = np.hypot(X - (a[0] + e[0] * t), Z - (a[1] + e[1] * t))
    # Lumpy, as rockfall is, and steeper toward the wall.
    d = d * (1.0 + 0.16 * (fbm(X, Z, 22, 3, 341) - 0.5) * 2)
    fill = top - grade * d - 0.0016 * d * d
    if inside is not None:
        fill = np.where(inside > 10, -1e9, fill)
    np.maximum(h, fill, out=h)
    return fill > h - 0.01


def dome(h, x, z, r, rise, squash=1.0, ang=0.0):
    """A slickrock dome: a low smooth swell."""
    dx, dz = X - x, Z - z
    ca, sa = math.cos(ang), math.sin(ang)
    u, v = dx * ca + dz * sa, (-dx * sa + dz * ca) / squash
    q = np.clip(1 - (u * u + v * v) / (r * r), 0, 1)
    h += rise * q ** 1.4
    return q > 0.02


def pothole(h, x, z, r, depth):
    q = np.clip(1 - ((X - x) ** 2 + (Z - z) ** 2) / (r * r), 0, 1)
    h -= depth * q ** 0.75


# Fins beside the route's: (points with heights, half width). Scenery, and
# somewhere to play.
FINS_DECOR = [
    # In the first tier's bay, either side of the route's.
    ([(-14, -178, 9.5), (-10, -160, 17), (-6, -142, 26), (-4, -126, 34), (-6, -110, 41)], 6.5),
    ([(48, -178, 9.5), (52, -162, 16), (56, -146, 24), (58, -130, 32), (56, -114, 40)], 6.0),
    ([(-44, -172, 9.5), (-40, -156, 18), (-36, -140, 27), (-32, -126, 36)], 5.5),
    ([(78, -170, 9.5), (82, -154, 18), (86, -140, 27), (86, -126, 37)], 5.5),
    # In the second's.
    ([(34, -56, 42), (24, -38, 50), (12, -20, 59), (0, -2, 67), (-10, 16, 75)], 6.5),
    ([(92, -42, 42), (84, -26, 51), (74, -10, 60), (64, 8, 69), (54, 24, 76)], 6.0),
]
# The baby fin on the flats: up one end, along the top and down the other.
BABY_FIN = ([(84, -262, 6.5), (96, -252, 11.5), (110, -242, 14.5), (124, -234, 13.5), (136, -226, 8.0)], 5.0)

# Slickrock domes: x, z, radius, rise, squash, turn.
DOMES = [
    # On the flats: the hot tubs' pavement and outliers.
    (-112, -238, 30, 3.4, 0.8, 0.3), (-84, -226, 16, 2.2, 1.0, 0), (-136, -252, 14, 1.8, 1.0, 0),
    (150, -246, 20, 3.0, 0.7, 0.9), (-222, -230, 26, 5.0, 0.7, 1.2), (240, -262, 22, 4.0, 0.8, 0.4),
    # The first bench: one out on the point, the rest between the rims.
    (-98, -148, 22, 5.0, 0.8, 0.4), (112, -92, 26, 6.0, 0.7, 0.5), (142, -64, 20, 4.5, 0.9, 0),
    (-72, -88, 28, 6.5, 0.7, -0.4), (150, -122, 20, 4.5, 0.8, 0.2), (-112, -52, 18, 4.0, 1.0, 0),
    (-36, -62, 16, 3.0, 0.9, 0.3), (274, -92, 18, 4.0, 0.8, 1.0), (-256, -96, 24, 6.0, 0.7, 0.6),
    (-262, -40, 18, 4.0, 0.9, 0),
    # The high bench.
    (-44, 44, 20, 4.5, 0.8, 0.4), (112, 30, 22, 5.5, 0.7, -0.3), (150, 74, 20, 5.0, 0.9, 0), (-232, 44, 26, 7.0, 0.7, 0.2),
    (-216, 124, 30, 8.0, 0.7, 1.1), (234, 96, 18, 4.5, 0.8, 0.5), (230, 160, 20, 5.0, 0.8, 0), (-258, 150, 20, 5.0, 0.8, 0),
    # The mesa top: low swells the trail's ledges step over.
    (132, 146, 26, 4.0, 0.7, 0.6), (-40, 150, 30, 5.0, 0.7, -0.5), (156, 190, 20, 3.5, 0.9, 0), (-100, 180, 24, 4.0, 0.8, 0.4),
    (0, 132, 18, 2.5, 0.9, 0),
]
# Potholes: x, z, radius, depth, water depth (0 dry).
POTHOLES = [(-118, -240, 6.5, 2.2, 0.0), (-100, -232, 5.0, 1.6, 0.45), (-122, -226, 4.0, 1.2, 0.0),
            (112, 30, 6.0, 1.8, 0.5), (-44, 44, 4.5, 1.4, 0.0), (132, 146, 5.0, 1.5, 0.4)]
SAND_HILL = (176, -292, 30, 8.0)


def build_ground():
    h, f = base_terrain()
    slick = np.zeros((N, N))
    # Where each cliff has fallen in, for the rim road's switchbacks.
    f["talus"] = talus_fill(h, (200, -141), (250, -141), float(sample(h, np.array([226.0, -124.0]))[0]) - 0.3, 0.58, f["d1"])
    f["talus"] |= talus_fill(h, (180, -6), (226, -6), float(sample(h, np.array([204.0, 14.0]))[0]) - 0.3, 0.55, f["d2"])

    routes = [route_line(h, R_APPROACH, 0.12, "SAND FLATS ROAD"), route_line(h, R_RIM, 0.15, "RIM ROAD"),
              route_line(h, R_CANYON, 0.36, "BIGHORN CANYON"), route_line(h, R_FINS, 0.48, "THE FINS"),
              route_line(h, R_SUMMIT, 0.34, "SUMMIT TRAIL")]
    appr, rim, canyon, fins, summit = routes
    appr.update(half=3.6, difficulty=0, short="FLATS")
    rim.update(half=3.4, difficulty=1, short="RIM ROAD")
    canyon.update(half=3.2, difficulty=3, short="CANYON")
    fins.update(half=3.4, difficulty=2, short="FINS")
    summit.update(half=3.0, difficulty=3, short="SUMMIT")

    # The fins: the route's two, each raised under its line, and the ones
    # beside them. The crest is widest where it tops out.
    n = len(fins["pts"])
    k = np.arange(n)
    # Where the route is on a fin: from the foot of the first to the bench,
    # and from the dome on the bench to the high bench.
    fin_half = 5.0 + 0.7 * np.sin(k * 0.05)

    def at(x, z):
        return int(np.argmin(np.linalg.norm(fins["pts"] - np.array([x, z], dtype=float), axis=1)))
    f["fin_k"] = [(at(14, -186), at(29, -90)), (at(66, -56), at(10, 54))]
    for a, b in f["fin_k"]:
        slick = np.maximum(slick, raise_fin(h, fins["pts"][a:b], fins["y"][a:b], fin_half[a:b]))
    for ctrl, half in FINS_DECOR + [BABY_FIN]:
        c = np.array(ctrl, dtype=float)
        pts = resample(smooth_path(c[:, :2], 3), 1.0)
        at = np.array([cKDTree(pts).query(q)[1] for q in c[:, :2]])
        y = gaussian_filter1d(np.interp(np.arange(len(pts)), at, c[:, 2]), 3.0, mode="nearest")
        slick = np.maximum(slick, raise_fin(h, pts, y, half))

    for (x, z, r, rise, sq, ang) in DOMES:
        slick = np.maximum(slick, dome(h, x, z, r, rise, sq, ang).astype(float))
    # The sand hill by the river: a dune to climb.
    sx, sz, sr, sh = SAND_HILL
    q = np.clip(1 - ((X - sx) ** 2 / (sr * sr) + (Z - sz) ** 2 / (sr * 0.7) ** 2), 0, 1)
    h += sh * q ** 1.2
    f["dune"] = q > 0.02

    # The canyon: the wash across the flats, then the cut through both tiers.
    n = len(canyon["pts"])
    k = np.arange(n)
    mouth = int(np.argmin(np.linalg.norm(canyon["pts"] - np.array([-184.0, -134.0]), axis=1)))
    head = int(np.argmin(np.linalg.norm(canyon["pts"] - np.array([-68.0, 62.0]), axis=1)))
    park = int(np.argmin(np.linalg.norm(canyon["pts"] - np.array([-198.0, -44.0]), axis=1)))
    f["canyon_k"] = (mouth, park, head)
    # The floor climbs in pour-offs: more of them, closer, toward the head.
    risers = [mouth + 55, mouth + 105, mouth + 150] + list(range(park + 28, head - 8, 34))
    terrace(canyon, mouth + 20, head - 4, risers)
    f["risers"] = risers
    half = 9.0 + 3.0 * np.sin(k * 0.045) + 7.0 * np.exp(-((k - park) / 22.0) ** 2) - 2.5 * smoothstep(park + 30, head, k)
    cut = cut_canyon(h, canyon["pts"][mouth - 12:head + 6], canyon["y"][mouth - 12:head + 6], half[mouth - 12:head + 6])
    f["canyon"] = cut
    # The wash: a shallow sandy channel from the canyon's mouth across the
    # flats to the river, which the canyon route follows up from the fork.
    wash = np.array([(-182, -130), (-190, -152), (-176, -178), (-140, -200), (-100, -214), (-72, -232), (-66, -262),
                     (-52, -292), (-40, -322), (-34, -340)], dtype=float)
    wp = resample(smooth_path(wash, 3), 1.0)
    wy = gaussian_filter1d(sample(h, wp), 8.0, mode="nearest") - 1.3
    wy = np.minimum.accumulate(wy)
    f["wash"] = cut_canyon(h, wp, wy, 4.5, wall=0.55) > 0.05
    f["wash_pts"] = wp

    for (x, z, r, depth, _) in POTHOLES:
        pothole(h, x, z, r, depth)
    f["slick"] = slick
    return h, f, routes


# ---------------------------------------------------------------------------

VIEWS = [
    ("flats-north", (0, 9, -300), (10, 60, 0)),
    ("flats-east", (-60, 8, -250), (250, 30, -160)),
    ("flats-west", (60, 8, -240), (-300, 50, -150)),
    ("fork", (8, 12, -215), (20, 50, -100)),
    ("bench1", (60, 52, -120), (40, 80, 20)),
    ("bench1-east", (120, 52, -100), (300, 40, -60)),
    ("high-bench", (-40, 86, 30), (30, 110, 150)),
    ("summit-north", (24, 144, 200), (40, 60, 330)),
    ("summit-south", (24, 146, 214), (0, 20, -280)),
    ("canyon-mouth", (-150, 13, -176), (-200, 30, -100)),
    ("canyon-park", (-190, 30, -60), (-180, 60, 10)),
    ("canyon-upper", (-180, 46, 20), (-160, 70, 70)),
    ("dugway1", (150, 14, -215), (230, 40, -140)),
    ("fin-a", (-10, 14, -210), (30, 40, -100)),
    ("fin-b", (90, 50, -80), (20, 80, 50)),
    ("shelf", (-60, 84, 70), (60, 104, 100)),
    ("air-se", (330, 200, -330), (0, 60, 40)),
    ("air-sw", (-330, 210, -330), (20, 60, 60)),
    ("air-north", (40, 230, 380), (20, 80, 100)),
    ("overlook-out", (24, 136.5, 198), (24, 96, 505)),
]


def preview(h, rgb, water, path, extra=None, land=(), **kw):
    """The views, with the land beyond the map (triangles and colours) if
    there is any yet."""
    ims = []
    for name, pos, tgt in VIEWS + (extra or []):
        p = list(pos)
        g = float(sample(h, np.array([p[0], p[2]]))[0])
        p[1] = max(p[1], g + 2.5)
        ims.append(level_view.render(h, rgb, p, tgt, water=water, extra=land, far=1500.0, **kw))
    level_view.sheet(ims, 4, path)


def finish_routes(h, routes):
    """Each route's turns, widths and carving into the ground."""
    for r in routes:
        r["turn"] = turniness(r["pts"])
        r["halfs"] = r["half"] * (1.0 + 0.9 * r["turn"])
        seg = np.linalg.norm(np.diff(r["pts"], axis=0), axis=1)
        # Held to the grade, easing round each hairpin.
        y = r["y"]
        glim = r["gmax"] * (1.0 - 0.8 * r["turn"])
        for _ in range(4):
            for k in range(1, len(y)):
                lim = glim[k] * seg[k - 1]
                y[k] = np.clip(y[k], y[k - 1] - lim, y[k - 1] + lim)
            for k in range(len(y) - 2, -1, -1):
                lim = glim[k] * seg[k]
                y[k] = np.clip(y[k], y[k + 1] - lim, y[k + 1] + lim)
    # Where routes meet they meet at one height.
    appr, rim, canyon, fins, summit = routes
    fork_y = appr["y"][-1]
    meet_y = float(np.mean([r["y"][-1] for r in (rim, canyon, fins)]))

    def pin(r, end, y_to, span=20.0):
        n = len(r["pts"])
        dist = np.arange(n, dtype=float) if end == 0 else np.arange(n - 1, -1, -1, dtype=float)
        r["y"] += (y_to - r["y"][0 if end == 0 else -1]) * np.clip(1 - dist / span, 0, 1)
    for r in (rim, canyon, fins):
        pin(r, 0, fork_y)
        pin(r, -1, meet_y)
    pin(summit, 0, meet_y)
    # A road is cut and filled into the slope it crosses; a fin's crest and
    # a canyon's floor were built to their lines already, and only the
    # strip the tyres use is trued up (a cut there would take the canyon's
    # walls back to banks).
    lay_route(h, canyon, 2.5)
    lay_route(h, fins, 3.0)
    lay_route(h, summit, 5.0)
    lay_route(h, rim, 12.0)
    lay_route(h, appr, 6.0)


def lay_route(h, r, shoulder_max):
    pts, y, half = r["pts"], r["y"], r["halfs"]
    d, idx = cKDTree(pts).query(FLAT, distance_upper_bound=float(np.max(half)) + shoulder_max + 3.0)
    ok = np.isfinite(d)
    jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
    d = d[ok]
    yt = y[idx[ok]]
    hw = half[idx[ok]]
    diff = h[jj, ii] - yt
    shoulder = np.minimum(2.0 + np.abs(diff) * 0.9, shoulder_max)
    h[jj, ii] = yt + diff * smoothstep(hw, hw + shoulder, d)


def report(h, routes):
    for r in routes:
        seg = np.linalg.norm(np.diff(r["pts"], axis=0), axis=1)
        grade = np.abs(np.diff(r["y"])) / np.maximum(seg, 1e-3)
        off = np.abs(sample(h, r["pts"]) - r["y"])
        print(f"  {r['name']:15s} {seg.sum():6.0f} m, climbs {r['y'][-1] - r['y'][0]:6.1f} m, "
              f"grade p95 {np.percentile(grade, 95) * 100:4.0f}% max {grade.max() * 100:4.0f}%, ground off by {off.max():.2f} m")


# ---------------------------------------------------------------------------
# Surfaces and colours

# The rock's colour by height: the beds lie level across the whole area,
# dark red at the river, paler with every tier, cream at the top. Steps
# between beds are small; the climb through them is what shows.
BEDS = [(-10, (156, 70, 48)), (10, (176, 76, 42)), (38, (184, 80, 44)), (44, (166, 72, 48)), (52, (206, 102, 56)),
        (78, (216, 116, 66)), (86, (208, 108, 62)), (104, (220, 124, 72)), (128, (226, 134, 80)),
        (140, (230, 142, 88)), (160, (232, 146, 92))]


def bed_colour(y):
    ys = np.array([b[0] for b in BEDS], dtype=float)
    cs = np.array([b[1] for b in BEDS], dtype=float)
    return np.dstack([np.interp(y, ys, cs[:, k]) for k in range(3)])


def faces(h, drop=4.6):
    """Which vertices are on a cliff's face: one a cell from a neighbour
    far above or below it. Taken with the rim and the foot, so a face is
    coloured and lit as a wall from top to bottom (the ground's colours
    run smoothly between vertices, and a rim shaded as the top it joins
    turns a cliff into a slope). Returns the mask and each vertex's
    direction out of the wall."""
    big = np.zeros_like(h)
    for dj, di in ((0, 1), (1, 0), (0, -1), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
        nb = np.roll(np.roll(h, dj, axis=0), di, axis=1)
        big = np.maximum(big, np.abs(nb - h) / math.hypot(dj, di))
    face = big > drop
    # And on the 6 m grid the game draws further off (every other vertex):
    # the vertices a cliff falls between there.
    e = h[::2, ::2]
    big2 = np.zeros_like(e)
    for dj, di in ((0, 1), (1, 0), (0, -1), (-1, 0)):
        big2 = np.maximum(big2, np.abs(np.roll(np.roll(e, dj, axis=0), di, axis=1) - e) / 2.0)
    face[::2, ::2] |= big2 > drop
    face[0, :] = face[-1, :] = face[:, 0] = face[:, -1] = False
    gz, gx = np.gradient(gaussian_filter(h, 0.6), D)
    g = np.hypot(gx, gz) + 1e-6
    return face, -gx / g, -gz / g


def surfaces(h, water, f, routes):
    sl = slope_deg(h)
    slick = gaussian_filter(f["slick"], 0.8) > 0.4
    # Scrub: hard red ground with brush on it (the game's GRASS, for how
    # it drives and for what grows).
    surf = np.full((N, N), S["GRASS"], dtype=np.uint8)
    # Sand lies where the wind and the washes leave it: in pockets on the
    # benches, between the fins, along the river.
    pockets = fbm(X, Z, 46, 3, 351)
    surf[(pockets > 0.6) & (sl < 9)] = S["SAND"]
    surf[f["dune"] | f["wash"]] = S["SAND"]
    surf[(Z < -306) & (sl < 12)] = S["SAND"]
    # Bare slickrock: the fins and domes, pavement on the high ground.
    pave = fbm(X, Z, 60, 3, 352)
    surf[(f["d2"] > 6) & (pave > 0.52) & (sl < 14)] = S["SLICKROCK"]
    surf[(f["d3"] > 4) & (pave > 0.36)] = S["SLICKROCK"]
    surf[slick] = S["SLICKROCK"]
    surf[(sl > 24) & ~slick] = S["SCREE"]
    surf[f["talus"] & (sl > 12)] = S["SCREE"]
    surf[sl > 43] = S["ROCK"]
    surf[slick & (sl > 52)] = S["ROCK"]
    surf[f["face"]] = S["ROCK"]
    surf[(f["canyon"] > 0.05) & (sl < 16)] = S["SAND"]
    appr, rim, canyon, fins, summit = routes
    for r in routes:
        d, idx = cKDTree(r["pts"]).query(FLAT, distance_upper_bound=float(np.max(r["halfs"])) + 1.0)
        ok = np.isfinite(d)
        ok[ok] = d[ok] < r["halfs"][idx[ok]] + 0.8
        jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
        k = idx[ok]
        cur = surf[jj, ii]
        if r is appr:
            v = np.full(len(jj), S["DIRT"])
        elif r is rim:
            # Graded dirt, gravel where it is cut into the talus.
            v = np.where(cur == S["SCREE"], S["GRAVEL"], np.where(cur == S["SLICKROCK"], S["SLICKROCK"], S["DIRT"]))
            v = np.where(cur == S["ROCK"], S["GRAVEL"], v)
        elif r is fins:
            v = np.where((cur == S["SLICKROCK"]) | (cur == S["ROCK"]), S["SLICKROCK"], np.where(cur == S["GRASS"], S["DIRT"], cur))
        elif r is canyon:
            # Sand between the pour-offs, rock at each.
            mouth, park, head = f["canyon_k"]
            riser = np.zeros(len(r["pts"]), dtype=bool)
            for q in f["risers"]:
                riser[max(0, q - 17):q + 13] = True      # rock under the ledges and the run up to them
            v = np.where(riser[k], S["ROCK"], np.where(k > mouth - 20, S["SAND"], S["DIRT"]))
            v = np.where(k > head + 4, np.where(cur == S["GRASS"], S["DIRT"], cur), v)
        else:
            v = np.where(cur == S["SAND"], cur, np.where(cur == S["GRASS"], S["DIRT"], S["SLICKROCK"]))
            v = np.where((cur == S["SCREE"]) | (cur == S["ROCK"]), S["ROCK"], v)
        surf[jj, ii] = v
    for (x, z, r_, depth, wd) in POTHOLES:
        if wd > 0:
            surf[np.hypot(X - x, Z - z) < r_ * 0.8] = S["WETROCK"]
    return surf


def colours(h, water, surf, f, routes):
    """The ground's albedo by surface and bed, with the tyre marks up the
    slickrock; then lit as the game lights it, for the map and previews."""
    nrm = normals(h)
    face, fx, fz = f["face"], f["face_x"], f["face_z"]
    # A wall catches the light as a wall: its normal lies nearly flat.
    wall = np.dstack([fx * 0.94, np.full_like(h, 0.34), fz * 0.94])
    nrm = np.where(face[..., None], wall, nrm)
    sl = slope_deg(h)
    n1 = fbm(X, Z, 9, 3, 101)
    n2 = fbm(X, Z, 40, 3, 102)
    warp = 5.0 * (fbm(X, Z, 70, 3, 361) - 0.5)
    bed = bed_colour(h + warp)
    alb = np.zeros((N, N, 3))
    dirt = bed * 0.4 + np.array([198.0, 102.0, 60.0]) * 0.6
    sand = np.array([232.0, 150.0, 92.0])
    alb[:] = dirt
    alb[surf == S["DIRT"]] = (dirt * 0.25 + np.array([240.0, 172.0, 124.0]) * 0.75)[surf == S["DIRT"]]
    alb[surf == S["SAND"]] = sand
    alb[f["wash"]] = (238, 172, 116)
    alb[surf == S["GRAVEL"]] = (232, 164, 120)
    # Slickrock: the bed's colour bleached toward salmon, with faint
    # contour lines where its cross-beds weather out.
    lines = 1.0 + 0.04 * np.sin((h + 2.5 * fbm(X, Z, 30, 2, 362)) * (2 * math.pi / 3.4))
    # (paler the higher: the top beds are the bleached ones)
    high = smoothstep(40, 115, h)[..., None]
    pale = np.array([240.0, 152.0, 96.0]) * (1 - high) + np.array([246.0, 190.0, 140.0]) * high
    sr = (bed * 0.4 + pale * 0.6) * lines[..., None]
    # (the fins' and domes' sides, and the mesa's ledges, are slickrock
    # however steep: only the tiers' cliffs are the dark walls)
    slick = (gaussian_filter(f["slick"], 0.8) > 0.4) | ((f["d3"] > 5) & (f["dn"] > 3))
    m = (surf == S["SLICKROCK"]) | (surf == S["WETROCK"])
    alb[m] = sr[m]
    # Cliffs: the bed, darker, streaked down the face with varnish.
    # The streaks run down the face: one value for a column of it, taken
    # from how far along the wall the vertex is.
    along = X * fz - Z * fx
    streak = 0.66 + 0.4 * vnoise(along / 2.6, np.round(np.arctan2(fz, fx) * 2.5), 363) ** 0.7
    streak *= 0.86 + 0.28 * vnoise(along / 21.0, np.round(np.arctan2(fz, fx) * 2.5), 364)   # and in panels, bay to buttress
    streak = np.where(face, streak, 0.80 + 0.26 * fbm(X, Z, 7, 2, 363))
    m = (surf == S["ROCK"]) & ~slick
    alb[m] = (bed * streak[..., None] * 0.9)[m]
    m = (surf == S["ROCK"]) & slick
    alb[m] = (sr * (0.9 + 0.1 * streak[..., None]))[m]
    # Talus: the bed greyed and dulled.
    m = surf == S["SCREE"]
    alb[m] = (bed * 0.78 + np.array([150.0, 104.0, 90.0]) * 0.22)[m]
    soft = np.dstack([gaussian_filter(alb[..., c], 0.85) for c in range(3)])
    alb = np.where(face[..., None], alb, soft)    # a face keeps its edge
    # Green by the river: the only damp ground there is.
    damp = np.maximum(smoothstep(-296, -322, Z), smoothstep(34, 12, f["basin_river"]) * (Z > 236)) * (sl < 10)
    alb = alb * (1 - 0.3 * damp[..., None]) + np.array([150.0, 138.0, 86.0]) * 0.3 * damp[..., None]
    alb *= (0.92 + 0.16 * n1)[..., None] * (0.96 + 0.08 * n2)[..., None]
    # Rubber on the slickrock where everyone drives: a soft darker band
    # up the fins (the grid is too coarse for a pair of tyre marks, and a
    # hard line across it comes out as a zigzag).
    wear = np.zeros((N, N))
    for r in routes:
        d, idx = cKDTree(r["pts"]).query(FLAT, distance_upper_bound=7.0)
        ok = np.isfinite(d).reshape(N, N)
        wear = np.maximum(wear, np.where(ok, np.clip(1 - d.reshape(N, N) / 5.5, 0, 1), 0.0))
    wear = gaussian_filter(wear, 1.0) * ((surf == S["SLICKROCK"]) | (surf == S["ROCK"]))
    alb = alb * (1 - 0.2 * wear[..., None]) + np.array([84.0, 62.0, 54.0]) * 0.2 * wear[..., None]
    ao = np.clip(1.0 + (h - gaussian_filter(h, 4)) * 0.05, 0.74, 1.08)
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
    q = rgb.astype(np.uint32)
    argb = (0xFF << 24) | (q[..., 0] << 16) | (q[..., 1] << 8) | q[..., 2]
    return argb.astype(np.uint32), rgb, alb * ao[..., None], nrm


WATER_RGB = (104, 112, 78)      # the river: silt green


def place_trees(h, water, surf, f, routes, rng):
    """Junipers (0) and pinyons (2) where there is soil on the benches and
    the mesa, in loose stands; cottonwoods (1) where there is water: the
    river's bank, the wash, the canyon's park."""
    sl = slope_deg(h)
    d_route = cKDTree(np.vstack([r["pts"] for r in routes])).query(FLAT)[0].reshape(N, N)
    stands = fbm(X, Z, 55, 3, 371)
    soil = (surf == S["GRASS"]) | (surf == S["SAND"])
    dens = smoothstep(0.5, 0.66, stands) * 0.55 + 0.03
    dens *= soil * (sl < 16) * smoothstep(0, 1, (d_route - 6.0) / 6.0)
    dens *= np.where(f["d1"] > 4, 1.0, 0.12)        # few on the flats
    dens[f["wash"] | f["dune"] | (water > -999) | (Z < -300)] = 0
    dens[f["dn"] < 6] *= 0.3
    trees = []
    for gz in np.arange(ORG + 6, -ORG - 6, 9.0):
        for gx in np.arange(ORG + 6, -ORG - 6, 9.0):
            p = np.array([gx + rng.uniform(-3.5, 3.5), gz + rng.uniform(-3.5, 3.5)])
            if rng.random() > float(sample(dens, p)[0]):
                continue
            if np.hypot(*(p - SPAWN)) < 30:
                continue
            kind = 2 if rng.random() < 0.3 else 0
            trees.append((p[0], 0.0, p[1], kind, rng.uniform(0.75, 1.3), rng.uniform(0, 2 * math.pi)))
    # Cottonwoods: along the river's north bank, the lower wash, the park.
    wp = f["wash_pts"]
    spots = [(x, -318 + rng.uniform(-4, 6)) for x in np.arange(-300, 330, 17.0) if abs(x - 6) > 44]
    spots += [tuple(wp[k] + rng.uniform(-9, 9, 2)) for k in range(len(wp) - 80, len(wp) - 20, 14)]
    spots += [(-196 + rng.uniform(-10, 10), -46 + rng.uniform(-10, 10)) for _ in range(5)]
    for (x, z) in spots:
        p = np.array([x + rng.uniform(-5, 5), z])
        g = to_grid(p)
        i, j = int(round(g[0])), int(round(g[1]))
        if water[j, i] > -999 or d_route[j, i] < 7 or sl[j, i] > 14:
            continue
        trees.append((p[0], 0.0, p[1], 1, rng.uniform(0.9, 1.4), rng.uniform(0, 2 * math.pi)))
    return trees


# ---------------------------------------------------------------------------
# Rock formations: each a skin lofted over rings along a spine (level.h)

def arch(a, b, ya, yb, top, thick, depth, seed, n=13, flags=()):
    """An arch from foot a to foot b (x, z), each rooted in its own
    ground (ya, yb), its crown at top: thick in the span's plane, depth
    through it, stouter at the feet."""
    a, b = np.asarray(a, float), np.asarray(b, float)
    mid, half = (a + b) / 2, (b - a) / 2
    rings = []
    for i in range(n):
        th = math.pi * i / (n - 1)
        th = th + 0.22 * math.sin(th) ** 2      # the crown leans to one side
        p = mid - half * math.cos(th)
        foot = abs(math.cos(th)) ** 2.5
        t = (1 - math.cos(th)) / 2
        base = ya + (yb - ya) * t - 4.0 * foot
        # One leg stouter than the other, the crown a flattened bar.
        stout = 1.0 + 0.25 * max(0.0, math.cos(th))
        rings.append((p[0], base + (top - base) * math.sin(th) ** 0.6, p[1], depth * (1 + 0.25 * foot) * stout,
                      thick * (1 + 0.55 * foot) * stout))
    return {"rings": rings, "sides": 8, "flags": flags, "seed": seed}


def hoodoo(x, z, y0, height, r, seed):
    """A goblin: a soft stem under a harder, paler cap."""
    prof = [(-0.15, 1.0), (0.25, 0.72), (0.52, 0.6), (0.66, 1.25), (0.8, 1.45), (0.93, 1.0), (1.0, 0.25)]
    return {"rings": [(x, y0 + height * t, z, r * k, r * k) for t, k in prof], "sides": 7, "flags": ("CAP",), "seed": seed}


def slab(a, b, half_w, half_t, seed, n=6, flags=("SMOOTH",)):
    """A slab of rock from a to b (x, y, z): half_w wide, half_t thick,
    thinning to its ends."""
    a, b = np.asarray(a, float), np.asarray(b, float)
    rings = []
    for i in range(n):
        t = i / (n - 1)
        p = a + (b - a) * t
        k = 0.45 if i in (0, n - 1) else 1.0
        rings.append((p[0], p[1], p[2], half_w * (0.9 + 0.1 * k), half_t * k))
    return {"rings": rings, "sides": 8, "flags": flags, "seed": seed}


# The basin under the great cliff, what the overlook looks down on: a
# river in a gooseneck round a stepped neck of rock that points back at
# the overlook, a couple of buttes left standing either side.
GOOSENECK = [(176, 470), (152, 384), (130, 322), (106, 282), (62, 262), (10, 257), (-40, 265), (-78, 290), (-98, 330),
             (-106, 384), (-112, 470)]
NECK = [(-54, 470), (-58, 332), (-42, 298), (-10, 281), (28, 278), (64, 288), (88, 316), (100, 360), (106, 470)]
# One mesa in the basin's north-east corner, long and off to the side
# (two buttes either side of the neck made a face of the map).
BUTTES = [
    ([(232, 316), (290, 300), (346, 312), (352, 344), (300, 356), (244, 348)], 44),
]


def carve_basin(h, water):
    """The basin's river, neck and buttes, once the great cliff is cut."""
    floor = h.copy()
    d = plateau_dist(NECK) + rim_wobble(383, 3.0, 1.5)
    # It sinks to a low saddle where it joins the country to the north (and
    # where the map ends: a cliff running off the edge would run on for ever).
    low = smoothstep(378, 336, Z)
    np.maximum(h, np.where(d > -30, floor + (scarp(d, 24, talus=0.34) + scarp(d - 20, 20, talus=0.2)) * low, -1e9), out=h)
    for k, (outline, height) in enumerate(BUTTES):
        d = plateau_dist(outline, 1) + 1.2 * (ridged(X, Z, 9, 2, 384 + k) - 0.5)
        c = np.mean(np.array(outline, dtype=float), axis=0)
        base = float(sample(floor, c)[0])
        np.maximum(h, np.where(d > -40, base - 1 + scarp(d, height, talus=0.34, slope=0.66, face=5.0), -1e9), out=h)
    # The river: cut a little into the floor, falling west to east.
    dr, sr, _ = line_dist(chaikin(GOOSENECK, 3))
    half = 9.0
    level = BASIN_Y - 3.0 + 1.0 * sr / max(1.0, float(sr.max()))
    bed = level - 0.8 - 1.4 * np.clip(1 - (dr / half) ** 2, 0, 1)
    bank = smoothstep(half + 16, half, dr)
    near = (dr < half + 16) & (Z > 236)
    h[near] = np.minimum(h, bed * bank + h * (1 - bank))[near]
    wet = near & (h < level - 0.05)
    water[wet] = level[wet]
    return dr


LEDGE_C = (0.12, 0.12)     # the canyon's ledges: the first one's step, and how much taller by the head


def set_pieces(h, water, f, routes, rng):
    """The rock formations, the obstacles on each route, the playground on
    the flats and the things to find. Returns a dict of the level's lists."""
    appr, rim, canyon, fins, summit = routes
    rocks, spheres, boxes, prop_boxes, posts, capsules, props, views, on_trail = [], [], [], [], [], [], [], [], set()

    def gy(x, z):
        return float(sample(h, np.array([x, z], dtype=float))[0])

    def at(r, x, z):
        return int(np.argmin(np.linalg.norm(r["pts"] - np.array([x, z], dtype=float), axis=1)))

    def post(x, z, top, r):
        posts.append((x, gy(x, z) - 1.0, z, x, top, z, r))

    # The arch over the head of the second fin, where the ways up meet.
    # Its feet stand on the bench either side of the notch the fin comes
    # up through: back from the rim until the ground is the bench's.
    my = gy(MEET[0], MEET[1])
    feet = []
    for side in (-1.0, 1.0):
        q = np.array([MEET[0] + side * 15.0, MEET[1] - 6.0])
        while gy(*q) < my - 1.5 and abs(q[0] - MEET[0]) < 30:
            q = q + np.array([side * 1.0, 0.4])
        feet.append(q + np.array([side * 1.5, 0.0]))
    a, b = feet
    rocks.append(arch(a, b, gy(*a), gy(*b), my + 14.5, 1.6, 3.0, 201))
    for q in (a, b):
        post(q[0], q[1], gy(*q) + 6.0, 2.6)
    views.append((MEET[0], MEET[1], "WINDOW ARCH"))
    # The overlook's prow: a slab out over the drop, to park on.
    sy = gy(SUMMIT[0], SUMMIT[1])
    edge = SUMMIT[1]
    while gy(SUMMIT[0], edge + 1.0) > sy - 4.0:      # where the ground falls away
        edge += 1.0
    tip = np.array([SUMMIT[0] + 0.6, edge + 8.5])
    root = np.array([SUMMIT[0], edge - 8.0])
    rocks.append(slab((root[0], sy - 0.75, root[1]), (tip[0], sy - 0.55, tip[1]), 3.6, 0.8, 202, n=7))
    c = (root + tip) / 2
    prop_boxes.append((c[0], sy - 0.75, c[1], 3.3, 0.78, float(np.linalg.norm(tip - root)) / 2 - 0.6,
                       math.atan2(tip[0] - root[0], tip[1] - root[1])))
    views.append((SUMMIT[0], SUMMIT[1], "THE DIVING BOARD"))
    props.append((root[0] + 6.0, root[1] + 2.0, 0.0, PROP["CAIRN"], None))
    # The top hairpin of the rim road's first climb, the flats laid out below.
    k = at(rim, 252, -160)
    views.append((rim["pts"][k][0], rim["pts"][k][1], "THE DUGWAY"))
    # Goblins on the flats' west side.
    gx, gz = -262.0, -270.0
    for k in range(9):
        q = np.array([gx, gz]) + rng.normal(0, 9.0, 2)
        hgt = rng.uniform(2.4, 5.0)
        rocks.append(hoodoo(q[0], q[1], gy(*q), hgt, hgt * rng.uniform(0.2, 0.28), 300 + k))
        post(q[0], q[1], gy(*q) + hgt, hgt * 0.2)
    views.append((gx + 12, gz + 6, "GOBLIN GARDEN"))
    # A balanced rock out on the first bench's point.
    bx, bz = -116.0, -160.0
    by = gy(bx, bz)
    rocks.append({"rings": [(bx, by - 1, bz, 1.9, 1.9), (bx, by + 2, bz, 1.3, 1.3), (bx, by + 5.2, bz, 0.9, 0.9), (bx, by + 6.4, bz, 0.7, 0.7)],
                  "sides": 7, "flags": ("DARK",), "seed": 401})
    rocks.append({"rings": [(bx + 0.2, by + 6.2, bz, 0.5, 0.5), (bx + 0.3, by + 7.2, bz, 2.3, 2.0), (bx + 0.3, by + 8.8, bz, 2.6, 2.3),
                            (bx + 0.2, by + 10.2, bz, 1.8, 1.6), (bx + 0.2, by + 10.9, bz, 0.4, 0.4)],
                  "sides": 8, "flags": (), "seed": 402})
    post(bx, bz, by + 6.0, 1.5)
    views.append((bx + 9, bz + 6, "BALANCED ROCK"))

    # ---- Obstacles, harder the further up each route.
    def boulder(c, rr, sink=0.4, trail=False):
        if trail:
            on_trail.add(len(spheres))
        spheres.append((c[0], gy(*c) - rr * sink, c[1], rr))

    def beside(r, k, rr, gap=0.8):
        p, tg, nm = frame(r, k)
        half = float(r["halfs"][k])
        boulder(p + nm * rng.choice([-1, 1]) * (half + gap + rr + rng.random() * 2.0), rr)

    # The rim road: rockfall beside it, nothing on it.
    n = len(rim["pts"])
    k = 180
    while k < n - 20:
        beside(rim, k, 0.4 + rng.random() * 0.9)
        k += int(12 + rng.random() * 16)
    # The canyon: boulders to thread between on the sand, and a flight of
    # ledges at every pour-off, taller toward the head.
    mouth, park, head = f["canyon_k"]
    last_ledge = -100
    for q in f["risers"]:
        diff = (q - mouth) / max(1, head - mouth)
        # One where the floor steepens into the pour-off and one where it
        # eases out of it: a step on the steepest of it stops a truck dead.
        for kk in (q - 9, q + 7):
            grade = abs(canyon["y"][kk + 4] - canyon["y"][kk]) / 4.0
            # (and never on a bend: a truck meets a step across one at an
            # angle and is turned along it into the wall)
            if grade < 0.24 and kk - last_ledge >= 11 and float(np.max(canyon["turn"][kk - 6:kk + 8])) < 0.22:
                boxes.append(ledge(canyon, kk, LEDGE_C[0] + LEDGE_C[1] * diff + 0.03 * rng.random(), float(canyon["halfs"][kk]) + 5.0, rng))
                last_ledge = kk
    k = mouth
    while k < head:
        diff = (k - mouth) / max(1, head - mouth)
        if min(abs(k - q) for q in f["risers"]) > 12:
            p, tg, nm = frame(canyon, k)
            half = float(canyon["halfs"][k])
            for _ in range(1 + int(diff * 2.2)):
                off = rng.uniform(-half - 3.5, half + 3.5)
                rr = 0.4 + 0.7 * diff * rng.random() + 0.25 * rng.random()
                if abs(off) < rr + 2.0:
                    off = math.copysign(rr + 2.0, off)      # always a gap a truck fits down the middle
                c = p + nm * off + tg * rng.uniform(-3, 3)
                boulder(c, rr, 0.45, trail=abs(off) < half + rr)
        k += int(13 + rng.random() * 9)
    # The summit trail: rockfall on the shelf and loose rock over the top
    # (its ledges are the mesa's own, in the ground).
    n = len(summit["pts"])
    k = 60
    while k < n - 14:
        diff = k / n
        p, tg, nm = frame(summit, k)
        half = float(summit["halfs"][k])
        rr = 0.3 + 0.4 * rng.random() * diff + 0.15
        off = off_line(summit, k, rng.uniform(-half - 1.0, half + 1.0), half)
        if abs(off) < rr + 1.9:
            off = math.copysign(rr + 1.9, off if off else 1.0)
        boulder(p + nm * off, rr, 0.45, trail=True)
        k += int(13 + rng.random() * 10)
    # Rockfall under the walls: boulders at the foot of every cliff.
    face = f["face"]
    foot = face & (np.roll(h, 1, 0) + np.roll(h, -1, 0) + np.roll(h, 1, 1) + np.roll(h, -1, 1) > 4 * h + 6)
    jj, ii = np.nonzero(foot & (np.abs(X) < 370) & (np.abs(Z) < 370))
    pick = rng.permutation(len(jj))[:150]
    d_route = cKDTree(np.vstack([r["pts"] for r in routes]))
    for q in pick:
        c = np.array([X[jj[q], ii[q]], Z[jj[q], ii[q]]]) + np.array([-f["face_x"][jj[q], ii[q]], -f["face_z"][jj[q], ii[q]]]) * -rng.uniform(4, 14)
        if d_route.query(c)[0] < 9 or water[jj[q], ii[q]] > -999:
            continue
        boulder(c + rng.normal(0, 1.5, 2), 0.6 + rng.random() ** 2 * 2.2, 0.35)

    # ---- The flats: somewhere to play before the climb.
    # Driftwood on the beach, a stray log by the wash.
    for (cx, cz, ang, L, rr) in [(-22, -322, 0.3, 6.0, 0.22), (30, -324, 1.9, 5.0, 0.18), (-84, -246, 1.1, 5.5, 0.2)]:
        g = gy(cx, cz)
        dx, dz = math.sin(ang) * L / 2, math.cos(ang) * L / 2
        capsules.append((cx - dx, g + rr * 0.8, cz - dz, cx + dx, g + rr * 0.8, cz + dz, rr))
    views.append((-108.0, -236.0, "HOT TUBS"))
    views.append((6.0, -318.0, "RIVER BEACH"))
    views.append((SAND_HILL[0], SAND_HILL[1], "SAND HILL"))
    k = at(fins, 29, -92)
    views.append((fins["pts"][k][0], fins["pts"][k][1], "THE SPINE"))
    k = at(canyon, -198, -46)
    views.append((canyon["pts"][k][0] + 8, canyon["pts"][k][1], "COTTONWOOD PARK"))
    # A camp under the cottonwoods by the river.
    cp = np.array([-78.0, -300.0])
    cyaw = 2.4
    props.append((cp[0], cp[1], cyaw, PROP["TENT"], None))
    # Driftwood to sit on, either side of the fire (campfire.c lights it
    # 4 m out from the tent's door).
    for side in (-1, 1):
        q = cp + np.array([math.sin(cyaw), math.cos(cyaw)]) * 4.0 + np.array([math.cos(cyaw), -math.sin(cyaw)]) * side * 1.6
        dx, dz = math.sin(cyaw) * 0.9, math.cos(cyaw) * 0.9
        capsules.append((q[0] - dx, gy(q[0], q[1]) + 0.18, q[1] - dz, q[0] + dx, gy(q[0], q[1]) + 0.18, q[1] + dz, 0.18))
    views.append((cp[0], cp[1], "RIVER CAMP"))
    # The trailhead: a notice board by the start, a table in the shade,
    # signs at the fork and under the arch.
    props.append((SPAWN[0] - 16, SPAWN[1] + 4, 1.2, PROP["RAMADA"], None))
    prop_boxes.append((SPAWN[0] - 16, gy(SPAWN[0] - 16, SPAWN[1] + 4) + 0.4, SPAWN[1] + 4, 0.9, 0.4, 0.4, 1.2))
    props.append((TRAILHEAD[0] + 6, TRAILHEAD[1], 0.2, PROP["KIOSK"], None))
    props.append((FORK[0] - 7, FORK[1] + 2, 0.4, PROP["SIGN"], None))
    props.append((MEET[0] + 9, MEET[1] + 9, 0.8, PROP["SIGN"], None))
    # An old truck left where the mine road gave out, on the first bench.
    wk = np.array([-246.0, -66.0])
    wyaw = 0.7
    prop_boxes.append((wk[0], gy(*wk) + 0.9, wk[1], 0.9, 0.9, 2.2, wyaw))
    props.append((wk[0], wk[1], wyaw, PROP["WRECK"], None))
    views.append((wk[0], wk[1], "OLD TIMER"))
    return {"rocks": rocks, "spheres": clear_roads(spheres, routes, on_trail), "boxes": boxes, "prop_boxes": prop_boxes,
            "posts": posts, "capsules": capsules, "props": props, "views": views}


# ---------------------------------------------------------------------------
# The land beyond the map (outland.py): what the overlook looks out on.
#
# The same country carried on: the upland fills the west, the plateau
# over the river the south, and north and east the low desert runs out
# past mesas and buttes into the haze. The basin's river comes in round
# the upland's foot, and leaves the map's gooseneck for a second one right
# under the overlook; the river at the start comes out of a canyon between
# upland and plateau and leaves along the plateau's foot.

# The great cliff, west of the map: it swings away north-west and the
# basin opens out behind the upland.
O_NORTH = [(-1250, 1010), (-1100, 800), (-990, 640), (-900, 500), (-800, 380), (-690, 290), (-580, 244),
           (-470, 224)] + NORTH[1:] + [(600, 216), (1300, 216)]
O_WEST = [(-540, -1300), (-480, -1000), (-400, -760), (-340, -570)] + WEST + [(-312, 1300)]
# The plateau over the river: its rim follows the far bank, then turns
# away south-east and lets the low desert in.
O_SOUTH = [(-1300, -470), (-1152, -452), (-1040, -412), (-950, -404), (-860, -440), (-770, -426), (-690, -370),
           (-590, -348), (-470, -357), (-384, -353), (384, -351), (470, -354), (590, -368), (700, -412), (800, -486),
           (880, -590), (940, -720), (980, -880), (1000, -1040), (1010, -1300)]
# The rivers, each from where it crosses the map's edge: the basin's
# upstream and down, the one at the start upstream and down.
O_RIVERS = [
    ("basin-up", [(-106, 384), (-118, 432), (-160, 476), (-226, 500), (-300, 492), (-362, 462), (-430, 462), (-500, 500),
                  (-570, 560), (-650, 630), (-750, 676), (-860, 700), (-960, 770), (-1020, 880), (-1050, 1000),
                  (-1070, 1200)], 0.35),
    # (out of the map's gooseneck and straight into another, round a rincon
    # right under the overlook)
    ("basin-down", [(152, 384), (170, 430), (160, 484), (110, 512), (50, 516), (0, 540), (-24, 590), (-6, 646), (50, 686),
                    (130, 700), (210, 676), (270, 624), (320, 572), (380, 540), (440, 490), (500, 446), (570, 440),
                    (628, 488), (648, 566), (612, 644), (540, 690), (490, 760), (500, 846), (580, 908), (700, 930),
                    (830, 900), (950, 920), (1060, 990), (1200, 1040)], -0.3),
    ("start-up", [(-384, -340), (-470, -344), (-590, -334), (-690, -356), (-770, -412), (-860, -426), (-950, -390),
                  (-1040, -398), (-1152, -438), (-1300, -456)], 0.5),
    ("start-down", [(384, -338), (470, -340), (590, -354), (700, -398), (800, -470), (880, -574), (940, -704),
                    (1010, -830), (1110, -910), (1300, -960)], -0.25),
]
# Mesas and buttes standing on the low desert: centre, half-lengths, the
# turn of the long axis (degrees), rise, and a second tier's rise (0: none).
O_MESAS = [
    # North, in front of the overlook: the rincon in the gooseneck, mesas
    # either side of it and beyond, another rincon downstream.
    (95, 604, 84, 54, 10, 46, 0), (-176, 656, 116, 74, -14, 64, 0), (-384, 640, 124, 84, -20, 60, 28),
    (-330, 820, 84, 70, 0, 76, 0), (334, 776, 110, 70, 20, 58, 0), (556, 572, 66, 58, 0, 48, 0),
    (-60, 850, 96, 70, 30, 66, 0),
    # East, across the low desert from the flats.
    (640, 64, 136, 84, 16, 54, 28), (520, -150, 84, 70, 0, 58, 0), (806, -190, 104, 72, -18, 66, 0),
    (704, 276, 78, 66, 0, 72, 0), (560, 300, 66, 58, 0, 46, 0), (790, 410, 96, 66, -30, 60, 0),
    # Far out, where they are outlines in the haze under the skyline.
    (-640, 1010, 190, 100, -12, 96, 0), (250, 1070, 170, 84, 6, 84, 0), (600, 1010, 150, 84, 18, 92, 0),
    (1010, 560, 110, 150, 10, 92, 0), (1050, 60, 120, 170, -8, 108, 0), (1040, -430, 100, 130, 14, 84, 0),
]
# And on the upland: a higher tier here and there.
O_CAPS = [(-610, -30, 150, 110, 12, 44), (-560, -230, 90, 60, 40, 36)]


def o_blob(cx, cz, rx, rz, ang, seed):
    """Metres inside a mesa's rim: a squared-off ellipse whose edge wanders."""
    a = math.radians(ang)
    u = (ol.OX - cx) * math.cos(a) + (ol.OZ - cz) * math.sin(a)
    v = -(ol.OX - cx) * math.sin(a) + (ol.OZ - cz) * math.cos(a)
    q = (np.abs(u / rx) ** 2.6 + np.abs(v / rz) ** 2.6) ** (1 / 2.6)
    r = min(rx, rz)
    return (1 - q) * r + 0.3 * r * (fbm(ol.OX, ol.OZ, 0.8 * r + 50, 3, seed) - 0.5) * 2


def build_outland(h, water, alb_ao):
    """The ring's heights, albedo, normals, haze, horizon map and rivers,
    joined to the finished map."""
    OX, OZ, OUT = ol.OX, ol.OZ, ol.OUT
    E = ol.extrude(h)
    # The low desert: higher in the basin than in the east, rolling.
    low = 6 + 9 * smoothstep(120, 400, OZ) + 2.5 * (fbm(OX, OZ, 180, 3, 501) - 0.5) * 2 + 1.0 * (fbm(OX, OZ, 60, 2, 502) - 0.5) * 2
    P = low.copy()
    for k, (cx, cz, rx, rz, a, rise, rise2) in enumerate(O_MESAS):
        d = o_blob(cx, cz, rx, rz, a, 510 + k)
        y = low + ol.cliff(d, rise)
        if rise2:
            y += ol.cliff(d - 44, rise2, talus=0.0)
        y += smoothstep(0, 40, d) * 2.0 * (fbm(OX, OZ, 70, 2, 530 + k) - 0.5) * 2
        P = np.maximum(P, np.where(d > -90, y, -1e9))
    # The plateau over the river.
    ds = ol.side_dist(O_SOUTH, left=False) + 14 * (fbm(OX, OZ, 110, 3, 505) - 0.5) * 2
    top = 78 + 22 * fbm(OX, OZ, 220, 3, 506) + 6 * np.round(2 * fbm(OX, OZ, 260, 2, 507))
    south = low + ol.cliff(ds, top, talus=0.12, slope=0.6)
    # The upland: the map's three cliffs stepped back, then rolling top.
    dw = ol.side_dist(O_WEST, left=True) + 12 * (fbm(OX, OZ, 120, 3, 508) - 0.5) * 2
    up = low + ol.cliff(dw, 44, talus=0.4, slope=0.64) + ol.cliff(dw - 40, 46, talus=0.2, slope=0.64) + ol.cliff(dw - 84, 40, talus=0.15, slope=0.64)
    up += smoothstep(70, 200, dw) * 9 * (fbm(OX, OZ, 240, 3, 509) - 0.4)
    for k, (cx, cz, rx, rz, a, rise) in enumerate(O_CAPS):
        d = o_blob(cx, cz, rx, rz, a, 560 + k)
        cap = ol.cliff(d, rise)
        up = up + np.where(dw > 100, cap, 0)
    P = np.maximum(P, south)
    # (the great cliff cuts the upland off as it does on the map)
    dn = ol.side_dist(O_NORTH, left=False) + 16 * (fbm(OX, OZ, 130, 3, 511) - 0.5) * 2
    up = np.minimum(up, low + ol.cliff(dn, 190, talus=0.2, slope=0.7))
    P = np.maximum(P, up)
    # Joined to the map: its border's ground carried out, giving way to
    # this over the first 170 m.
    w = smoothstep(0, 170, OUT)
    H = E * (1 - w) + P * w
    # The rivers: a wash cut for each, wide enough that this grid always
    # has its floor under the water, and a canyon where it crosses high
    # ground (the same cut, deeper).
    ribbons, river_d = [], np.full(H.shape, 1e9)
    for name, line, fall in O_RIVERS:
        d, s, _ = ol.line_dist(line)
        x0, z0 = line[0]
        # (the water's level where the map's river meets its edge)
        m = (water > -999) & (np.hypot(X - x0, Z - z0) < 30)
        y0 = float(water[m].max())
        level = y0 + fall * s / 100.0
        bed = level - 2.4
        cut = bed + (H - bed) * smoothstep(40, 86, d)
        H = np.where(d < 86, np.minimum(H, cut), H)
        river_d = np.minimum(river_d, d)
        # (no wider than the map's own channel where it takes over from it)
        ribbons.append(ol.ribbon(line, lambda t: 7.0 + 7.0 * float(smoothstep(0, 200, t)), lambda t, y0=y0, fall=fall: y0 + fall * t / 100.0))
    # On the map's edge the ground is the map's, but for where a river
    # leaves it: this grid's vertices there miss the channel as often as
    # not, and would stand in the water.
    H[ol.INNER] = E[ol.INNER]
    H[ol.BORDER] = np.where(river_d < 86, np.minimum(E, H), E)[ol.BORDER]

    # Colours: the map's own way with them, by slope and height.
    face, fx, fz = ol.faces(H)
    nrm = ol.normals(H)
    nrm = np.where(face[..., None], np.dstack([fx * 0.94, np.full_like(H, 0.34), fz * 0.94]), nrm)
    gz, gx = np.gradient(H, ol.CELL)
    sl = np.hypot(gx, gz)
    bed_c = bed_colour(H + 5.0 * (fbm(OX, OZ, 70, 3, 361) - 0.5))
    n1 = fbm(OX, OZ, 60, 3, 520)
    n2 = fbm(OX, OZ, 200, 3, 521)
    dirt = bed_c * 0.4 + np.array([198.0, 102.0, 60.0]) * 0.6
    sand = np.array([232.0, 150.0, 92.0])
    sandy = smoothstep(0.5, 0.66, n2)[..., None]
    alb = dirt * (1 - sandy) + sand * sandy
    # High ground is slickrock, paler the higher, with pinyon and juniper
    # in dark drifts across it.
    high = smoothstep(40, 115, H)[..., None]
    pale = np.array([240.0, 152.0, 96.0]) * (1 - high) + np.array([246.0, 190.0, 140.0]) * high
    sr = bed_c * 0.4 + pale * 0.6
    woods = (smoothstep(0.52, 0.64, fbm(OX, OZ, 90, 3, 522)) * 0.34)[..., None]
    sr = sr * (1 - woods) + np.array([104.0, 108.0, 70.0]) * woods
    topk = (smoothstep(36, 56, H - low) * smoothstep(0.28, 0.14, sl))[..., None]
    alb = alb * (1 - topk) + sr * topk
    # Talus, then the walls, streaked.
    tal = (smoothstep(0.16, 0.34, sl) * (1 - topk[..., 0]))[..., None]
    alb = alb * (1 - tal) + (bed_c * 0.82 + np.array([150.0, 96.0, 78.0]) * 0.18) * tal
    streak = 0.72 + 0.3 * fbm(OX * fz - OZ * fx, np.round(np.arctan2(fz, fx) * 2.5), 9, 2, 363)
    alb = np.where(face[..., None], bed_c * streak[..., None] * 0.72, alb)
    # The washes: sand bars, and the only green there is.
    bar = smoothstep(62, 30, river_d)[..., None] * (H < low + 12)[..., None]
    green = 0.5 + 0.5 * np.sin(OX * 0.045 + 3 * n1) * np.cos(OZ * 0.05)
    alb = alb * (1 - bar) + (np.array([236.0, 186.0, 132.0]) * (1 - 0.6 * green[..., None]) + np.array([118.0, 128.0, 74.0]) * 0.6 * green[..., None]) * bar
    alb *= (0.9 + 0.2 * n1)[..., None]
    # The map's own colours at its edge, and for 100 m out.
    w2 = smoothstep(0, 110, OUT)[..., None]
    alb = ol.extrude(alb_ao) * (1 - w2) + alb * w2
    hz = ol.horizon_map(H)
    fade = smoothstep(600, 768, OUT)
    return {"h": H, "alb": alb, "nrm": nrm, "fade": fade, "hz": hz, "ribbons": ribbons}


def build(out, preview_path=None, raw=None):
    print("Mesa Grande")
    rng = np.random.default_rng(1869)
    h, f, routes = build_ground()
    finish_routes(h, routes)
    h, water = carve_river(h, f["south"])
    f["basin_river"] = carve_basin(h, water)
    for (x, z, r_, depth, wd) in POTHOLES:
        if wd > 0:
            m = np.hypot(X - x, Z - z) < r_
            level = float(h[m].min()) + wd
            water[m & (h < level - 0.03)] = level
    f["face"], f["face_x"], f["face_z"] = faces(h)
    # The mesa's ledges are faces too, small as they are: without their
    # own colour and light its top is a dune.
    f["face"] |= faces(h, 1.2)[0] & (f["d3"] > 5) & (f["dn"] > 3)
    surf = surfaces(h, water, f, routes)
    trees = place_trees(h, water, surf, f, routes, rng)

    def gy(x, z):
        return float(sample(h, np.array([x, z]))[0])
    sp = set_pieces(h, water, f, routes, np.random.default_rng(77))
    rocks, spheres, capsules, posts = sp["rocks"], sp["spheres"], sp["capsules"], sp["posts"]
    boxes = sp["boxes"] + sp["prop_boxes"]
    rock_boxes = len(sp["boxes"])
    # Nothing grows through a rock or a prop.
    keep = cKDTree(np.array([(s_[0], s_[2]) for s_ in spheres] + [(b[0], b[2]) for b in boxes] + [(q[0], q[2]) for q in posts] +
                            [(p[0], p[1]) for p in sp["props"]] + [((c[0] + c[3]) / 2, (c[2] + c[5]) / 2) for c in capsules]))
    trees = [(t[0], gy(t[0], t[2]), t[2], t[3], t[4], t[5]) for t in trees if keep.query((t[0], t[2]))[0] > 4.5]
    argb, rgb, alb_ao, nrm = colours(h, water, surf, f, routes)
    props = [(p[0], gy(p[0], p[1]) if p[4] is None else p[4], p[1], p[2], p[3]) for p in sp["props"]]
    views = [(v[0], gy(v[0], v[1]), v[1], v[2]) for v in sp["views"]]
    # A cairn by each thing to find, but the summit and the arch.
    for v in views:
        if v[3] not in ("THE DIVING BOARD", "WINDOW ARCH", "RIVER CAMP", "OLD TIMER"):
            props.append((v[0] + 4.0, gy(v[0] + 4.0, v[2] + 3.0), v[2] + 3.0, 0.0, PROP["CAIRN"]))
    summit_y = gy(SUMMIT[0], SUMMIT[1])
    spawns = []
    for k in range(4):
        sx = SPAWN[0] + (k - 1.5) * 6.0
        sz = SPAWN[1] - abs(k - 1.5) * 2.0
        spawns.append((sx, gy(sx, sz), sz, 0.0))

    out.mkdir(parents=True, exist_ok=True)
    light = light_inputs(alb_ao, nrm, h, water, flags=f["face"].astype(np.uint8))
    hz = horizon_map(h)
    blob = pack_level(h, water, surf, light, hz)
    (out / "level_moab.lvl").write_bytes(blob)
    o = build_outland(h, water, alb_ao)
    oblob = ol.pack(o["h"], o["alb"], o["nrm"], o["fade"], o["hz"], o["ribbons"])
    (out / "level_moab.out").write_bytes(oblob)
    if raw:
        raw.write_bytes(raw_level(h, argb, water, surf, light, hz))
    write_header(out / "level_moab.h", "moab", {
        "name": "MESA GRANDE", "blurb": "RED ROCK CANYON COUNTRY", "base_ft": 3960,
        "lake_y": 0.0, "start_y": gy(SPAWN[0], SPAWN[1]), "summit": (SUMMIT[0], summit_y, SUMMIT[1]),
        "routes": routes, "spheres": spheres, "capsules": capsules, "boxes": boxes, "rock_boxes": rock_boxes,
        "posts": posts, "trees": trees, "props": props, "views": views, "spawns": spawns,
        "mists": [(0, -338, 60), (-170, -338, 60), (180, -336, 60)],
        # Hawks over the flats, the canyon, the mesa and the gooseneck.
        "hawks": [(20, -230, 40, 46, 0.0, 1), (-180, -20, 30, 60, 2.0, -1), (30, 150, 44, 40, 4.0, 1), (30, 300, 50, 70, 1.0, -1)],
        "herds": [(230, -236), (-250, -250)],
        "rocks": rocks,
    })
    minimap(rgb, water, routes, [], out / "level_map_moab.png")
    print(f"  heights {h.min():.1f}..{h.max():.1f} m, summit {summit_y:.1f} m, start {gy(SPAWN[0], SPAWN[1]):.1f} m")
    report(h, routes)
    print(f"  {len(spheres)} boulders, {len(boxes)} boxes, {len(trees)} trees, {len(rocks)} formations, "
          f"{len(views)} viewpoints, level_moab.lvl {len(blob)} bytes")
    if preview_path:
        preview_image(rgb, water, routes, spheres, capsules, boxes, [], views, preview_path)
        # The land beyond, lit as colours() lights the map.
        az, el = math.degrees(math.atan2(SUN[0], SUN[2])) % 360, math.degrees(math.asin(SUN[1]))
        orgb = ol.lit(o["alb"], o["nrm"], o["hz"], az, el, (0.62, 0.66, 0.72), (0.62, 0.577, 0.496))
        land = [ol.mesh(o["h"], orgb)] + [ol.ribbon_mesh(r, WATER_RGB) for r in o["ribbons"]]
        preview(h, rgb, water, preview_path.with_name("level-moab-views.png"), land=land, rocks=rocks, spheres=spheres,
                boxes=boxes, trees=trees, tree_rgb=((74, 96, 58), (122, 150, 62), (64, 88, 60)))


if __name__ == "__main__":
    import sys
    build(Path(sys.argv[1]), Path(sys.argv[2]) / "level-moab.png" if len(sys.argv) > 2 else None)
