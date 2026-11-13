"""Ridgeline's fourth area: Loch Lowland, a Highland pass over a loch.

After the Bealach na Ba in Wester Ross, and the hill tracks of the rest of
the Highlands. The land is what ice leaves: a long loch along the south,
flat ground at its head where the burn comes out of the hills, and behind
that a corrie cut back into a plateau, a U of steep walls with a level
floor, its head one long slope up to the pass. West of it a second, smaller
corrie hangs over the loch with a lochan in it. The plateau either side
is moss, bare rock and lochans; east of the pass a ridge climbs to the
top, which has a mast on it.

Each landform is a floor (the lowland, a corrie's floor, the glen north of
the pass) and the wall that rises from its edge to the plateau: a floor's
outline is drawn here with the kind of wall along each stretch of it (a
sandstone crag, a steep slope under a band of it, a long grass slope, the
even slope of a corrie's head, open moor), and the ground is whichever
wall is lowest at a point, or the plateau where none reaches it. Walls
from two floors meet in a ridge, as they do.

Three ways climb from the fork at the corrie's mouth to the pass:

  drove road   the old road: over the stone bridge, a long shelf up the
               corrie's east wall, hairpins up the head (east, easy)
  peat road    west through the pines, up into the hanging corrie, round
               its lochan through the peat, a rake up its back wall and
               over the moss along the crag's edge (west, harder)
  staircase    straight up the corrie: the burn's fords, the boulders, the
               rock bar, then ramps of bare rock up its head, pitch and
               tread (centre, hardest)

and the mast track goes on from the pass up the ridge to the top.

Every route is a line drawn here with its heights, each zigzag's legs
where the ground already stands at them; the ground is then cut and
filled to the line, so what the CPU drivers follow is what was designed.
Beyond the map the loch and the hills carry on (build_outland).
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from scipy.interpolate import RBFInterpolator
from scipy.ndimage import gaussian_filter, gaussian_filter1d, map_coordinates, maximum_filter, minimum_filter
from scipy.spatial import cKDTree

from levelkit import *  # noqa: F401,F403
import level_view
import outland as ol

# ---------------------------------------------------------------------------
# The plan (x east, z north, metres)

LOCH_Y = 1.0
SPAWN = np.array([0.0, -270.0])
TRAILHEAD = np.array([-2.0, -236.0])
FORK = np.array([-6.0, -200.0])
MEET = np.array([6.0, 215.0])           # the pass
SUMMIT = np.array([172.0, 277.0])
PASS_Y = 112.0

# Walls, by kind: 0 a sandstone crag, 1 a steep slope under a band of
# crag, 2 a long grass slope with a crag along its top (what a road is cut
# across), 3 one even slope from foot to top (a corrie's head, where the
# tracks zigzag), 4 open moor. A value between two is some of each.

# The foot of the hills along the loch's side: (x, z, wall). South of it
# is the lowland.
FOOT = [(-470, -240, 4), (-384, -240, 4), (-336, -234, 3.2), (-296, -226, 1.5), (-252, -222, 1.5), (-212, -220, 1.5),
        (-178, -216, 0.6), (-142, -210, 0), (-102, -206, 0), (-66, -203, 0), (-44, -204, 0.4), (0, -208, 1),
        (56, -204, 1), (72, -200, 1), (116, -205, 1), (166, -210, 1), (222, -218, 1.2), (282, -228, 2),
        (340, -238, 3.2), (384, -242, 4), (470, -246, 4)]
# The corrie's floor.
CORRIE = [(-46, -230, 0), (-47, -150, 0), (-51, -80, 0), (-53, -20, 0), (-52, 28, 0.5), (-45, 58, 2.4), (-28, 74, 3),
          (-6, 81, 3), (16, 82, 3), (38, 77, 3), (53, 64, 2.6), (61, 34, 2), (63, -10, 2), (61, -60, 2),
          (59, -130, 2), (61, -182, 2), (64, -230, 2)]
# The hanging corrie's: crag at its back and west, a slope a track can
# climb on the north-east.
HANGING = [(-262, -140, 2), (-300, -120, 0.6), (-314, -78, 0), (-302, -36, 0), (-270, -18, 0.3), (-238, -22, 2),
           (-216, -52, 2), (-212, -94, 2), (-226, -128, 2)]
HANGING_Y = 60.0
# The glen north of the pass.
NGLEN = [(-34, 229, 2), (-50, 278, 2.2), (-70, 338, 2.4), (-86, 470, 2.4), (40, 470, 2.4), (46, 338, 2.2),
         (42, 280, 2), (34, 230, 2), (6, 223, 2)]

# The plateau: heights the old surface passes through, before the ice.
TOPS = [
    # (over the lowland it only has to be out of the way)
    (-384, -384, 110), (0, -384, 110), (384, -384, 110), (0, -300, 110), (-200, -300, 110), (200, -300, 110),
    # The west hill.
    (-384, -236, 84), (-384, -120, 112), (-384, 0, 128), (-384, 160, 134), (-384, 384, 128),
    (-150, -150, 106), (-112, -112, 110), (-176, -30, 122), (-240, 10, 128), (-330, -40, 130), (-150, 60, 123),
    (-140, 130, 125), (-96, 188, 121), (-250, 150, 132), (-200, 280, 128), (-112, 310, 119), (-96, 384, 112),
    (-300, 384, 132),
    # The pass.
    (6, 213, 112), (6, 199, 112.4), (-26, 211, 114.5), (32, 221, 114.5),
    # The east hill: the ridge up from the pass to the top, and south from
    # there along the corrie's rim.
    (40, 229, 116), (80, 239, 124), (112, 247, 131), (140, 261, 141), (172, 277, 152), (230, 287, 146),
    (200, 345, 140), (120, 338, 128), (150, 384, 132), (270, 384, 120), (384, 384, 108), (384, 220, 114),
    (300, 210, 134), (214, 180, 144), (224, 80, 137), (226, -20, 129), (220, -120, 119), (156, -150, 106),
    (300, -60, 116), (384, 60, 102), (384, -100, 90), (384, -240, 72), (290, -190, 98),
]


# ---------------------------------------------------------------------------
# Floors and walls

def outline(ctrl, closed=True):
    """A floor's edge from its control points (x, z, wall): the line
    rounded off, a point every 0.75 m, the wall's kind carried along it."""
    p = chaikin(np.asarray(ctrl, dtype=float), 3, closed=closed)
    if closed:
        p = np.vstack([p, p[:1]])
    seg = np.linalg.norm(np.diff(p[:, :2], axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    t = np.arange(0, s[-1], 0.75)
    return np.stack([np.interp(t, s, p[:, k]) for k in range(3)], axis=1)


def floor_field(pts, closed=True):
    """For every vertex: metres outside a floor's edge (negative inside),
    and the wall's kind, the foot's position and how far along the edge
    it is at the nearest point of it. An open line's floor is the ground
    to its south."""
    d, idx = cKDTree(pts[:, :2]).query(FLAT)
    if closed:
        ins = inside(pts[:, :2]).ravel()
    else:
        tg = np.gradient(pts[:, :2], axis=0)
        rel = FLAT - pts[idx, :2]
        ins = tg[idx, 0] * rel[:, 1] - tg[idx, 1] * rel[:, 0] < 0      # to the right of a line drawn west to east
    return (np.where(ins, -d, d).reshape(N, N), pts[idx, 2].reshape(N, N),
            pts[idx, 0].reshape(N, N), pts[idx, 1].reshape(N, N), (idx * 0.75).reshape(N, N))


def wall_sheer(d, H):
    """Torridonian sandstone: scree banked against the foot, then one
    great wall of it to the rim. (Tiers of it with ledges between were
    truer and too fine: the game draws far ground on a 12 m grid and a
    24 m one, which made mush of them. One wall survives both.)"""
    a = 0.24 * H
    d1 = np.maximum(a / 0.62, 1e-3)
    apron = a * np.clip(d / d1, 0, 1) ** 1.25
    e = np.maximum(d - d1, 0)
    face = 0.7 * H * np.clip(e / 14.0, 0, 1)
    cap = 0.06 * H * np.clip((e - 14.0) / 14.0, 0, 1)
    return np.minimum(apron + face + cap, H)


def wall_steep(d, H):
    """Forty degrees of grass and scree, a band of crag along the rim."""
    W = np.maximum(H / 0.8, 1e-3)
    t = np.clip(d / W, 0, 1)
    return H * (0.76 * t + 0.24 * smoothstep(0.78, 0.86, t))


def wall_grassy(d, H):
    """A long slope of grass at twenty-five degrees, crag along its top."""
    low = 0.44 * H
    d1 = np.maximum(low / 0.46, 1e-3)
    e = d - d1
    up = 0.7 * e + 0.22 * H * smoothstep(10, 19, e)
    return np.minimum(np.where(d < d1, 0.46 * d, low + up), H)


def wall_ramp(d, H):
    W = np.maximum(H / 0.5, 1e-3)
    return H * np.clip(d / W, 0, 1) ** 1.06


def wall_moor(d, H):
    W = np.maximum(H / 0.30, 1e-3)
    t = np.clip(d / W, 0, 1)
    return H * (1 - (1 - t) ** 1.7)


def wall(d, H, kind):
    """The height gained d metres from a floor's edge, where the plateau
    stands H over it."""
    g = [wall_sheer(d, H), wall_steep(d, H), wall_grassy(d, H), wall_ramp(d, H), wall_moor(d, H)]
    out = np.zeros_like(d)
    for k in range(5):
        out += g[k] * np.clip(1 - np.abs(kind - k), 0, 1)
    return out


def tops():
    """The plateau's surface through TOPS: thin-plate, so it rolls."""
    p = np.array(TOPS, dtype=float)
    f = RBFInterpolator(p[:, :2], p[:, 2], kernel="thin_plate_spline", smoothing=40.0)
    return f(FLAT).reshape(N, N)


def lowland(x, z):
    """The flat ground at the loch's head: the burn's fan out of the
    corrie's mouth, falling gently to the shore."""
    fan = np.exp(-((x - 16) / 120.0) ** 2)
    return 2.2 + (5.2 + 3.4 * fan) * smoothstep(-298, -206, z)


CORRIE_Z = [-240, -205, -150, -72, -58, -44, 0, 40, 76, 120]
CORRIE_YS = [8.6, 10.6, 15.0, 24.0, 25.0, 33.5, 37.0, 41.5, 46.5, 48.0]


def corrie_floor(x, z):
    """Up the corrie: a rise from the mouth, a rock bar across it that the
    burn falls over, then the flats under the head wall."""
    return np.interp(z, CORRIE_Z, CORRIE_YS)


def nglen_floor(x, z):
    return np.interp(z, [215, 230, 270, 315, 384, 470], [PASS_Y, 110.6, 105.0, 97.0, 78.0, 58.0])


def rim_wobble(seed, big=7.0, small=2.4):
    """Metres to move a wall in or out by: bays and buttresses."""
    return big * 2 * (fbm(X, Z, 52, 3, seed) - 0.5) + small * (ridged(X, Z, 17, 2, seed + 7) - 0.5) * 2


def base_terrain():
    """The floors, their walls and the plateau, before the water and the
    routes. Returns the heights and the fields later steps need."""
    f = {}
    T = tops()
    f["top"] = T
    h = T.copy()
    regions = [
        ("low", outline(FOOT, closed=False), False, lowland, 601),
        ("corrie", outline(CORRIE), True, corrie_floor, 602),
        ("hanging", outline(HANGING), True, lambda x, z: np.full(np.shape(x), HANGING_Y), 603),
        ("nglen", outline(NGLEN), True, nglen_floor, 604),
    ]
    f["along"] = np.zeros((N, N))
    f["gully"] = np.zeros((N, N))
    dwin = np.full((N, N), 1e3)      # how far in from its foot the wall that stands at each vertex is
    for k, (name, pts, closed, floor, seed) in enumerate(regions):
        d, kind, fx, fz, along = floor_field(pts, closed)
        B = floor(fx, fz)
        H = np.maximum(T - B, 0)
        # Bays and buttresses, more of them the craggier the wall.
        wob = rim_wobble(seed) * (1.0 - 0.2 * np.clip(kind, 0, 4)) * smoothstep(0, 16, d)
        along = along + 1000.0 * k
        # A crag is buttresses with gullies between: its wall swells out
        # and back every twenty-odd metres along it. Round-nosed, and
        # standing out no more than a couple of metres: the ground is a
        # vertex every 3 m, and buttresses creased down the front and 6 m
        # proud came out as pleats of bare triangles. (Where they stand is
        # as it was: the routes' heights were set to this ground.)
        rib = 1.0 - (2.0 * vnoise(along / 12.0, np.full_like(along, 7.0), seed) - 1.0) ** 2
        wob += 4.2 * (rib - 0.62) * np.clip(1.0 - kind, 0, 1) * smoothstep(4, 20, d)
        g = wall(np.maximum(d + wob, 0), H, kind)
        # Gullies down it: every slope here is fluted by the water off it,
        # a line straight down every forty metres or so, a round-bottomed
        # scoop some eight metres across (a V two metres wide fell
        # between the vertices or on one, as it happened).
        line = 1.0 - (2.0 * vnoise(along / 21.0, np.full_like(along, 3.0), seed) - 1.0) ** 2
        left = np.clip((H - g) / np.maximum(H, 1.0), 0, 1)
        cut = line ** 2.5 * smoothstep(5, 24, d) * smoothstep(0.03, 0.3, left) * (0.35 + 0.65 * smoothstep(0.3, 1.2, kind))
        S = np.where(d <= 0, floor(X, Z), B + g - 2.6 * cut)
        lower = S < h
        f["along"] = np.where(lower & (d > 0), along, f["along"])
        dwin = np.where(lower, d, dwin)
        f["gully"] = np.where(lower, np.where(d > 0, cut, 0.0), f["gully"])
        h = np.minimum(h, S)
        f["d_" + name], f["k_" + name] = d, kind
    # The faces of the crags and the steep walls (not the ramps the routes
    # climb, nor the moor) are smoothed along themselves, 6 m either way at
    # the same distance in from the foot: what is left of the buttresses
    # is their swell, twenty-odd metres from one to the next, and a face
    # no longer zigzags from vertex to vertex, which the game drew as sharp
    # fins and notches of single triangles. Across itself it is as it
    # was, its rim and its foot as sharp.
    crag = np.zeros((N, N))
    for name in ("low", "corrie", "hanging", "nglen"):
        d, kind = f["d_" + name], f["k_" + name]
        crag = np.maximum(crag, smoothstep(2.7, 2.1, kind) * smoothstep(-2, 3, d) * smoothstep(60, 40, d))
    gz, gx = np.gradient(gaussian_filter(dwin, 1.0), D)
    gl = np.hypot(gx, gz) + 1e-6
    tx, tz = -gz / gl, gx / gl
    run = np.zeros((N, N))
    for k, w in ((-2, 1.0), (-1, 2.0), (0, 3.0), (1, 2.0), (2, 1.0)):
        run += w / 9.0 * map_coordinates(h, [(Z + tz * D * k - Z[0, 0]) / D, (X + tx * D * k - X[0, 0]) / D], order=1, mode="nearest")
    # (no vertex by more than 2.5 m: at a corner the line along one face
    # runs out across the next, and would round an arete off)
    h = h + np.clip(run - h, -2.5, 2.5) * CRAG_SMOOTH * gaussian_filter(crag, 1.0)
    # The plateau is not a table: knolls of bare rock and hollows of moss.
    high = smoothstep(4, 22, np.minimum.reduce([f["d_low"] - 60, f["d_corrie"] - 70, f["d_hanging"] - 40, f["d_nglen"] - 30]))
    f["high"] = high
    h += high * (2.6 * (fbm(X, Z, 44, 3, 611) - 0.5) * 2 + 1.1 * (ridged(X, Z, 19, 2, 612) - 0.5) * 2)
    f["gully"] *= 1 - high
    # Nor are the floors: what the ice dropped lies on them in low humps.
    floors = smoothstep(2, -10, np.minimum(f["d_low"], f["d_corrie"]))
    h += floors * smoothstep(-316, -300, Z) * 1.1 * (fbm(X, Z, 17, 2, 613) - 0.5) * 2
    return h, f




# ---------------------------------------------------------------------------

VIEWS = [
    ("shore-north", (0, 6, -312), (10, 70, 0)),
    ("shore-nw", (60, 7, -300), (-200, 80, -100)),
    ("shore-ne", (-80, 7, -300), (200, 80, -80)),
    ("fork", (-6, 14, -222), (10, 60, 0)),
    ("corrie-low", (-10, 22, -150), (10, 70, 120)),
    ("corrie-mid", (-10, 44, -30), (10, 90, 160)),
    ("east-wall", (-40, 50, -60), (160, 80, 20)),
    ("west-wall", (50, 56, -20), (-140, 90, 20)),
    ("head", (0, 54, 20), (20, 96, 190)),
    ("pass-south", (6, 118, 224), (20, 20, -300)),
    ("pass-north", (6, 118, 206), (-20, 80, 384)),
    ("summit-south", (172, 158, 287), (40, 20, -300)),
    ("summit-west", (180, 158, 277), (-200, 100, 120)),
    ("hanging", (-200, 16, -262), (-250, 60, -120)),
    ("lochan", (-236, 66, -136), (-290, 80, -50)),
    ("moss", (-170, 128, 60), (-60, 110, 190)),
    ("crag-edge", (-128, 128, 60), (60, 60, 0)),
    ("road-shelf", (150, 64, -60), (120, 84, 110)),
    ("hairpins", (124, 96, 110), (40, 104, 190)),
    ("stair", (20, 56, 64), (-40, 90, 170)),
    ("air-se", (330, 240, -330), (0, 60, 40)),
    ("air-sw", (-330, 240, -330), (0, 60, 40)),
    ("air-north", (20, 260, 384), (10, 60, 60)),
    ("base", (40, 12, -250), (-60, 2, -330)),
]


def preview(h, rgb, water, path, extra=None, land=(), **kw):
    ims = []
    for name, pos, tgt in VIEWS + (extra or []):
        p = list(pos)
        g = float(sample(h, np.array([p[0], p[2]]))[0])
        p[1] = max(p[1], g + 2.5)
        ims.append(level_view.render(h, rgb, p, tgt, water=water, extra=land, far=1500.0, haze=(206, 216, 222),
                                     fog=(140.0, 900.0), sky=((120, 150, 190), (214, 220, 224)), water_rgb=WATER_RGB, **kw))
    level_view.sheet(ims, 4, path)


# ---------------------------------------------------------------------------
# The loch, the burns and the lochans

# The loch's north shore, west to east: a bay where the hanging corrie's
# burn comes in, a headland, the bay at the start with its beach, the
# burn's delta.
SHORE = [(-470, -312), (-384, -314), (-330, -320), (-290, -316), (-256, -324), (-216, -340), (-180, -360),
         (-152, -366), (-130, -356), (-114, -334), (-82, -322), (-30, -319), (20, -321), (60, -325), (86, -334),
         (112, -330), (150, -322), (210, -320), (280, -324), (340, -328), (384, -326), (470, -326)]
KNOLL = (-152.0, -346.0, 20.0, 3.4)          # the headland's: x, z, radius, rise
CRANNOG = (-58.0, -354.0, 10.0)
CAUSEWAY = [(-66.0, -324.0), (-64.0, -338.0), (-58.0, -348.0)]
JETTY_SPANS = 3      # of the plank bridge's 14 m
CRAG_SMOOTH = 0.9    # how much of a crag's vertex-to-vertex zigzag along it is smoothed out
# Hummocks left by the ice on the flats east of the start, to drive over.
HUMMOCKS = [(128, -262, 13, 2.6), (150, -280, 11, 2.1), (166, -256, 15, 3.2), (188, -276, 12, 2.4), (144, -240, 10, 1.8),
            (204, -252, 13, 2.8), (178, -234, 9, 1.6), (222, -272, 10, 2.0)]
# The moraine across the corrie, which the burn has cut through.
MORAINE = [(-44, -114), (-20, -125), (8, -130), (36, -127), (56, -118)]

# Burns, each from its head down.
BURN = [(15, 126), (12, 110), (13, 96), (10, 84), (2, 68), (8, 52), (-2, 38), (0, 20), (6, 8), (2, -6), (10, -22),
        (6, -36), (8, -46), (8, -58), (16, -76), (6, -94), (14, -112), (10, -130), (20, -148), (16, -168), (22, -190),
        (36, -206), (30, -226), (46, -244), (50, -266), (70, -284), (72, -304), (86, -320), (90, -342)]
OUTFLOW = [(-276, -100), (-278, -122), (-280, -138), (-286, -168), (-290, -204), (-284, -240), (-274, -280),
           (-268, -312), (-266, -338)]
NBURN = [(-10, 286), (-16, 316), (-24, 350), (-30, 384)]
# Lochans: x, z, radius, depth.
LOCHAN_DUBH = (-274.0, -76.0, 25.0, 2.4)
LOCHAN_PASS = (-6.0, 266.0, 20.0, 1.8)
LOCHAN_CORRIE = (0.0, 20.0, 13.0, 1.3)
LOCHANS_MOSS = [(-204.0, 104.0, 13.0, 1.2), (-176.0, 152.0, 9.0, 0.9), (-230.0, 88.0, 8.0, 0.9)]


def shore_field():
    """Metres inland of the loch's shore (negative: out on the water)."""
    return floor_field(outline([(x, z, 0) for x, z in SHORE], closed=False), closed=False)[0]


def carve_loch(h, f):
    d = f["shore"] = shore_field()
    off = -d
    # The beach shelves into the water; the bed drops away a truck's
    # length out.
    bed = LOCH_Y - 0.5 * smoothstep(0, 8, off) - 1.9 * smoothstep(8, 22, off) - 2.4 * smoothstep(22, 70, off)
    land = LOCH_Y + 0.08 + 0.10 * np.maximum(d, 0) + 0.004 * np.maximum(d, 0) ** 2
    h = np.where(d > 0, np.minimum(h, land), np.minimum(h, bed))
    # The crannog: an islet a causeway of stones leads out to, just under
    # the water.
    cx, cz, cr = CRANNOG
    q = np.clip(1 - ((X - cx) ** 2 + (Z - cz) ** 2) / (cr * cr), 0, 1)
    h = np.maximum(h, np.where(q > 0, LOCH_Y - 0.5 + 2.1 * q ** 0.55, -1e9))
    dc, _, _ = line_dist(smooth_path(np.array(CAUSEWAY), 2))
    h = np.maximum(h, np.where((d < 2) & (dc < 12), LOCH_Y - 0.3 - 0.45 * np.maximum(dc - 2.4, 0), -1e9))
    f["crannog"] = q > 0.02
    water = np.where((d < 3) & (h < LOCH_Y - 0.03), LOCH_Y, -1000.0)
    return h, water


def burn_bed(h, line, incise):
    """A burn's line, a point a metre, and its bed: the ground along it,
    cut into, never rising downstream."""
    pts = resample(smooth_path(np.array(line, dtype=float), 3), 1.0)
    bed = gaussian_filter1d(sample(h, pts), 2.5, mode="nearest") - incise
    return pts, np.minimum.accumulate(bed)


def carve_burn(h, water, pts, bed, half, depth=0.35, bank=0.6, walled=None):
    """Its channel into the ground and its water. half: half its width,
    or that at every point. walled: a point where the banks stand nearly
    upright (a bridge's abutments)."""
    d, idx = cKDTree(pts).query(FLAT, distance_upper_bound=float(np.max(half)) + 16.0)
    ok = np.isfinite(d)
    jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
    d, idx = d[ok], idx[ok]
    half = half[idx] if np.ndim(half) else half
    slope = np.full(len(d), bank)
    if walled is not None:
        near = np.hypot(pts[idx, 0] - walled[0], pts[idx, 1] - walled[1])
        slope = bank + 1.4 * smoothstep(13, 7, near)
    target = bed[idx] + np.maximum(d - half, 0) * slope
    cur = h[jj, ii]
    h[jj, ii] = np.minimum(cur, target)
    wet = (d < half + 0.7) & (bed[idx] + depth > LOCH_Y + 0.1)
    water[jj[wet], ii[wet]] = np.maximum(water[jj[wet], ii[wet]], bed[idx[wet]] + depth)
    cut = np.zeros((N, N), dtype=bool)
    cut[jj, ii] = (d < half + 2.0) & (cur > target - 0.2)
    return cut


def lochan(h, water, spec, seed, level=None, closed=True):
    """A lochan in a hollow of its own: the ground round it levelled to a
    rim, the basin under it. Returns its water's level. closed: no burn
    runs out of it, so its rim should stand all the way round (rim_check)."""
    x, z, r, depth = spec
    c = np.array([x, z])
    if level is None:
        bench(h, c, r * 1.25, r * 0.9)
        level = float(sample(h, c)[0]) - 0.3
    d = np.hypot(X - x, Z - z)
    rr = r * (0.78 + 0.44 * fbm(X, Z, max(r * 0.8, 6.0), 2, seed))
    q = np.clip(1 - (d / rr) ** 2, 0, 1)
    basin = level + 0.25 - (depth + 0.25) * q ** 0.8
    h[:] = np.where(d < rr, np.minimum(h, basin), h)
    if closed:
        # Its rim holds it all the way round: where the ground beside it
        # stood lower than the water a lip of turf is raised, and nothing
        # past the basin is wet. (One drawn on the crag's edge had no rim
        # to the south: the game drew its water out over the drop.)
        lip = (d >= rr) & (d < rr + 5.0)
        h[:] = np.where(lip, np.maximum(h, level + 0.18 * smoothstep(rr + 5.0, rr + 2.0, d)), h)
        wet = (d < rr) & (h < level - 0.03)
        LOCHANS.append((x, z, r, level))
    else:
        wet = (d < rr + 3.0) & (h < level - 0.03)
    water[wet] = np.maximum(water[wet], level)
    return level


LOCHANS = []        # each closed one made: x, z, radius, level (rim_check)


def rim_check(h, water):
    """Any lochan whose rim the later ground work has cut below its water:
    dry ground beside a wet vertex of it that stands lower."""
    for (x, z, r, level) in LOCHANS:
        own = (np.hypot(X - x, Z - z) < r * 1.3) & (water > -999)
        beside = maximum_filter(own, 3) & (water < -999)
        low = np.where(beside, level - h, -1.0)
        if low.max() > 0.3:
            j, i = np.unravel_index(int(np.argmax(low)), low.shape)
            print(f"  LOCHAN at ({x:.0f}, {z:.0f}): its rim is {low.max():.2f} m under the water at ({X[j, i]:.0f}, {Z[j, i]:.0f})")


def mound(h, x, z, r, rise, squash=1.0, ang=0.0):
    dx, dz = X - x, Z - z
    ca, sa = math.cos(ang), math.sin(ang)
    u, v = dx * ca + dz * sa, (-dx * sa + dz * ca) / squash
    q = np.clip(1 - (u * u + v * v) / (r * r), 0, 1)
    h += rise * q ** 1.5
    return q > 0.02


# ---------------------------------------------------------------------------
# Routes: drawn here with their heights, the ground built to them

# (x, z, height or None for the ground as it lies)
R_APPROACH = [(0, -262, None), (-1, -250, None), (-2, -236, None), (-4, -218, None), (-6, -200, None)]
BRIDGE_AT = (21.0, -189.0)
R_DROVE = [
    (-6, -200, None), (4, -196, 11.9), (12, -192.6, 12.5), (21, -189, 12.5), (30, -185.4, 12.5), (38, -182, 12.9),
    (50, -172, 14.0),
    # North up the foot of the corrie's east wall.
    (60, -150, 16.6), (68, -116, 21.2), (68, -84, 25.4), (72, -64, 28.2), (84, -56, 30.4),
    # Back south, a leg higher.
    (92, -70, 32.6), (96, -92, 35.6), (108, -118, 39.8), (124, -134, 43.6), (138, -136, 46.4),
    # The long shelf north under the crag, the corrie's floor falling
    # away on the left, and round into its head.
    (134, -120, 48.8), (130, -100, 51.7), (134, -78, 54.9), (140, -56, 58.2), (128, -36, 61.5), (128, -12, 64.9),
    (130, 14, 68.6), (130, 40, 72.3), (130, 64, 75.7), (128, 86, 78.9), (118, 110, 82.6),
    # The hairpins: three legs up the head's slope, each where the ground
    # is at its height.
    (104, 126, 85.6), (92, 143, 88.6), (78, 155, 91.2), (64, 170, 94.1), (46, 178, 96.4), (32, 186, 97.4),
    (48, 189, 99.4), (64, 186, 101.5), (78, 182, 103.4), (88, 186, 104.6), (80, 196, 106.2), (62, 201, 108.6),
    (42, 206, 110.6), (22, 211, 111.6), (6, 215, None),
]
R_STAIR = [
    (-6, -200, None), (-10, -184, None), (-12, -160, None), (-8, -136, None), (0, -114, None), (11, -101, None),
    (24, -92, None), (30, -70, 24.6),
    # Through the rock bar by a notch the burn's old course left.
    (34, -52, 29.2), (30, -34, 33.8), (24, -18, None), (14, -8, None), (4, -1, None), (-8, 8, None), (-20, 24, None),
    (-24, 44, None), (-20, 62, None), (-14, 78, 46.6),
    # The staircase: ramps up the ledges of the corrie's head, each as
    # steep as the rock will hold a tyre.
    (-34, 88, 53.5), (-54, 99, 61.0), (-60, 107, 63.5), (-54, 114, 66.5), (-36, 130, 73.5), (-18, 148, 81.5),
    (-4, 164, 89.0), (-2, 173, 91.5), (-14, 180, 95.5), (-34, 188, 101.5), (-50, 196, 107.0), (-54, 204, 109.5),
    (-40, 210, 111.5), (-18, 214, 112.2), (6, 215, None),
]
R_PEAT = [
    (-6, -200, None), (-40, -208, None), (-84, -215, None), (-128, -221, None), (-166, -226, None), (-196, -222, 7.5),
    # Up the front of the hanging corrie: four legs.
    (-216, -215, 11.0), (-236, -208, 15.5), (-254, -201, 20.0), (-262, -194, 22.0), (-248, -188, 25.0),
    (-230, -183, 29.0), (-212, -178, 33.0), (-198, -170, 36.5), (-212, -162, 40.5), (-230, -154, 45.0),
    (-246, -148, 49.5), (-256, -141, 52.0), (-240, -136, 56.0), (-226, -128, 59.4),
    # Round the lochan through the peat.
    (-232, -108, None), (-236, -84, None), (-234, -60, None), (-238, -40, None), (-242, -26, 61.2),
    # The rake: south-east up the corrie's back wall, then all the way
    # back across it to the plateau.
    (-230, -21, 64.2), (-218, -22, 67.6), (-200, -28, 72.5), (-182, -38, 77.5), (-170, -50, 81.5), (-160, -54, 84.0),
    (-164, -38, 87.5), (-168, -17, 92.5), (-172, 1, 97.0), (-180, 17, 101.5), (-190, 32, 106.0), (-200, 43, 110.5),
    (-211, 51, 115.0), (-224, 58, 119.5),
    # Over the moss, along the crag's edge, down to the pass.
    (-212, 68, None), (-190, 74, None), (-166, 82, None), (-150, 104, None), (-138, 136, None), (-120, 170, None),
    (-94, 200, None), (-58, 221, None), (-24, 227, None), (6, 215, None),
]
R_SUMMIT = [(6, 215, None), (24, 221, None), (44, 228, None), (70, 235, None), (96, 241, None), (118, 249, None),
            (140, 261, None), (158, 270, None), (172, 277, None)]


def at(r, x, z):
    return int(np.argmin(np.linalg.norm(r["pts"] - np.array([x, z], dtype=float), axis=1)))


def ford(r, pts, bed):
    """Where route r crosses a burn it goes down into its bed and out."""
    d, idx = cKDTree(pts).query(r["pts"])
    on = np.nonzero(d < 1.2)[0]
    out = []
    while len(on):
        run = on[on < on[0] + 30]
        k = int(run[np.argmin(d[run])])
        w = np.cos(np.clip((np.arange(len(r["y"])) - k) / 11.0, -1, 1) * math.pi / 2) ** 2
        r["y"] = r["y"] * (1 - w) + (bed[idx[k]] + 0.05) * w
        out.append(k)
        on = on[on >= on[0] + 30]
    return out


def build_ground():
    h, f = base_terrain()
    # The headland's knoll, the hummocks on the flats, the moraine.
    kx, kz, kr, kh = KNOLL
    mound(h, kx, kz, kr, kh)
    f["hummock"] = np.zeros((N, N), dtype=bool)
    for (x, z, r, rise) in HUMMOCKS:
        f["hummock"] |= mound(h, x, z, r, rise, 0.75, 0.6)
    dm, _, _ = line_dist(smooth_path(np.array(MORAINE, dtype=float), 3))
    lump = 0.75 + 0.5 * fbm(X, Z, 13, 2, 631)
    h += 3.3 * np.exp(-(dm / 7.5) ** 2) * lump * (f["d_corrie"] < 4)
    f["moraine"] = dm < 10
    h, water = carve_loch(h, f)
    f["lochan_y"] = {"dubh": lochan(h, water, LOCHAN_DUBH, 641, level=HANGING_Y - 0.4, closed=False),
                     "pass": lochan(h, water, LOCHAN_PASS, 642, closed=False)}
    for k, spec in enumerate(LOCHANS_MOSS):
        lochan(h, water, spec, 650 + k)

    burn, bed = burn_bed(h, BURN, 1.0)
    f["burn"], f["burn_bed"] = burn, bed

    routes = [route_line(h, R_APPROACH, 0.12, "LOCHSIDE"), route_line(h, R_DROVE, 0.15, "DROVE ROAD"),
              route_line(h, R_STAIR, 0.42, "DEVIL'S STAIRCASE"), route_line(h, R_PEAT, 0.28, "PEAT ROAD"),
              route_line(h, R_SUMMIT, 0.32, "MAST TRACK")]
    appr, drove, stair, peat, summit = routes
    appr.update(half=3.6, difficulty=0, short="LOCHSIDE")
    drove.update(half=3.5, difficulty=1, short="DROVE RD")
    stair.update(half=3.0, difficulty=3, short="STAIRCASE")
    peat.update(half=3.0, difficulty=2, short="PEAT ROAD")
    summit.update(half=3.0, difficulty=3, short="MAST TRACK")
    f["fords"] = ford(stair, burn, bed)
    f["stair_k"] = at(stair, -14, 78)
    # The top's ledges: the ridge climbs in steps of pale rock.
    n = len(summit["pts"])
    f["summit_risers"] = list(range(40, n - 44, 28))
    terrace(summit, 14, n - 30, f["summit_risers"], spread=5.0, flat=0.75)
    # The staircase's ramps: the head wall's beds, each a short pitch of
    # bare rock and a tread behind it.
    k0, k1 = f["stair_k"], at(stair, -40, 210)
    f["stair_risers"] = list(range(k0 + 8, k1, 11))
    terrace(stair, k0, k1, f["stair_risers"], spread=2.4, flat=0.8)
    return h, water, f, routes


def finish_routes(h, routes, f):
    """Each route's turns, widths and heights, then the ground to it."""
    appr, drove, stair, peat, summit = routes
    for r in routes:
        r["turn"] = turniness(r["pts"])
        r["halfs"] = r["half"] * (1.0 + 0.9 * r["turn"])
        seg = np.linalg.norm(np.diff(r["pts"], axis=0), axis=1)
        y = r["y"]
        glim = r["gmax"] * (1.0 - 0.8 * r["turn"])
        for _ in range(4):
            for k in range(1, len(y)):
                lim = glim[k] * seg[k - 1]
                y[k] = np.clip(y[k], y[k - 1] - lim, y[k - 1] + lim)
            for k in range(len(y) - 2, -1, -1):
                lim = glim[k] * seg[k]
                y[k] = np.clip(y[k], y[k + 1] - lim, y[k + 1] + lim)
    # The bridge's deck is level: so is the road across it.
    kb = at(drove, *BRIDGE_AT)
    w = smoothstep(14.0, 8.5, np.abs(np.arange(len(drove["y"])) - kb).astype(float))
    drove["y"] = drove["y"] * (1 - w) + float(drove["y"][kb]) * w
    # Passing places on the road: it is a lane and a half wide between them.
    f["passing"] = [at(drove, 68, -116), at(drove, 108, -118), at(drove, 134, -78), at(drove, 130, 40), at(drove, 64, 186)]
    k = np.arange(len(drove["pts"]))
    for q in f["passing"]:
        drove["halfs"] += 1.9 * np.exp(-((k - q) / 7.0) ** 2)
    # And room to pass at the foot of the mast track: trucks coming off the
    # top met those setting out from the pass on a single track's width,
    # and both stood there until they were winched.
    summit["halfs"] += 1.7 * np.exp(-((np.arange(len(summit["halfs"])) - 10) / 6.0) ** 2)
    # Where routes meet they meet at one height.
    fork_y = appr["y"][-1]

    def pin(r, end, y_to, span=20.0):
        n = len(r["pts"])
        dist = np.arange(n, dtype=float) if end == 0 else np.arange(n - 1, -1, -1, dtype=float)
        r["y"] += (y_to - r["y"][0 if end == 0 else -1]) * np.clip(1 - dist / span, 0, 1)
    for r in (drove, stair, peat):
        pin(r, 0, fork_y)
        pin(r, -1, PASS_Y)
    pin(summit, 0, PASS_Y)
    # (every zigzag's legs were drawn where the ground stands at their
    # heights, so each is a bench cut and filled into the slope it climbs)
    # The pass itself is levelled: four tracks come into it, and the wedges
    # of ground left between them stood up as fins. (It is where everyone
    # stops to look back down the glen, besides.)
    d = np.hypot(X - MEET[0], Z - MEET[1])
    w = smoothstep(27.0, 15.0, d)
    h[:] = h * (1 - w) + (PASS_Y + (h - PASS_Y) * 0.1) * w
    f["pass"] = d < 13.0
    # A hairpin is a level place to turn: the ground inside its loop comes
    # to the road's height, or what is left between the two legs stands
    # there as a fin.
    for r in (drove, peat, stair):
        t = r["turn"]
        for k in range(8, len(t) - 8):
            if t[k] < 0.55 or t[k] < t[k - 8:k + 9].max():
                continue
            loop = r["pts"][max(0, k - 11):k + 12]
            c = loop.mean(axis=0)
            rad = float(np.linalg.norm(loop - c, axis=1).max()) + float(r["half"]) * 0.5
            d = np.hypot(X - c[0], Z - c[1])
            w = smoothstep(rad + 5.0, rad, d)
            h[:] = h * (1 - w) + (float(r["y"][k]) + (h - float(r["y"][k])) * 0.12) * w
    lay_route(h, stair, 4.5)
    lay_route(h, peat, 7.0)
    lay_route(h, summit, 5.0)
    lay_route(h, drove, 12.0)
    lay_route(h, appr, 6.0)
    smooth_banks(h, routes)


def smooth_banks(h, routes, reach=14.0):
    """The banks beside the routes, smoothed along the route: 6 m either
    way at the same distance out from it. A track cut across a steep wall
    at a slant to the grid had a bank of ribs, a vertex each, down its
    whole length. The running surface is not touched (nothing within 0.6 m
    of its edge), and no vertex moves more than 1.5 m."""
    best = np.full(N * N, 1e9)
    tx, tz, hw = np.zeros(N * N), np.zeros(N * N), np.zeros(N * N)
    for r in routes:
        pts = r["pts"]
        d, idx = cKDTree(pts).query(FLAT, distance_upper_bound=float(np.max(r["halfs"])) + reach)
        ok = np.isfinite(d) & (d < best)
        tg = np.gradient(pts, axis=0)
        tg /= np.linalg.norm(tg, axis=1)[:, None] + 1e-9
        best[ok], hw[ok] = d[ok], r["halfs"][idx[ok]]
        tx[ok], tz[ok] = tg[idx[ok], 0], tg[idx[ok], 1]
    w = np.where(best < 1e8, smoothstep(hw + 0.6, hw + 2.4, best) * smoothstep(hw + reach, hw + reach - 5.0, best), 0.0)
    w, tx, tz = w.reshape(N, N), tx.reshape(N, N), tz.reshape(N, N)
    run = np.zeros((N, N))
    for k, wk in ((-2, 1.0), (-1, 2.0), (0, 3.0), (1, 2.0), (2, 1.0)):
        run += wk / 9.0 * map_coordinates(h, [(Z + tz * D * k - Z[0, 0]) / D, (X + tx * D * k - X[0, 0]) / D], order=1, mode="nearest")
    h += np.clip(run - h, -1.5, 1.5) * 0.85 * w


def finish_water(h, water, f, routes):
    """The burns into the finished ground, the lochan the big one runs
    through, and every ford kept to a wade."""
    burn, bed = f["burn"], f["burn_bed"]
    # (a thread of white down the head, a burn you could wade below it)
    f["burn_cut"] = carve_burn(h, water, burn, bed, 1.0 + 1.3 * smoothstep(96, 60, burn[:, 1]), walled=BRIDGE_AT)
    k = int(np.argmin(np.linalg.norm(burn - np.array(LOCHAN_CORRIE[:2]), axis=1)))
    f["lochan_y"]["corrie"] = lochan(h, water, LOCHAN_CORRIE, 643, level=float(bed[k]) + 0.4, closed=False)
    for line, half in ((OUTFLOW, 1.2), (NBURN, 1.2)):
        pts, b = burn_bed(h, line, 0.7)
        f["burn_cut"] |= carve_burn(h, water, pts, b, half, depth=0.28)
    water[water < h + 0.05] = -1000.0
    for r in routes:
        d, idx = cKDTree(r["pts"]).query(FLAT, distance_upper_bound=float(np.max(r["halfs"])))
        ok = np.isfinite(d)
        ok[ok] = d[ok] < r["halfs"][idx[ok]]
        jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
        wet = water[jj, ii] > -999
        if r["name"] == "DROVE ROAD":
            continue        # it crosses by the bridge
        y = r["y"][idx[ok]]
        water[jj[wet], ii[wet]] = np.minimum(water[jj[wet], ii[wet]], np.maximum(h[jj[wet], ii[wet]], y[wet]) + 0.42)
    water[water < h + 0.05] = -1000.0


# Peat: x, z, radius, depth. The bog on the flats, the hags round the
# lochan and on the moss, the wet ground under the head wall.
PEAT = [(-112, -268, 14.0, 0.36), (-231, -112, 6.5, 0.16), (-236, -90, 7.5, 0.2), (-235, -66, 6.5, 0.18),
        (-200, 71, 7.0, 0.18), (-160, 88, 7.5, 0.2), (-142, 124, 6.5, 0.16), (-126, 160, 6.0, 0.14),
        (-27, 36, 6.0, 0.16), (-22, 54, 6.5, 0.16), (-190, 100, 10.0, 0.3), (-226, 124, 12.0, 0.3)]


# ---------------------------------------------------------------------------
# Surfaces and colours
#
# The game's surfaces, as this area uses them: GRASS is turf (the green
# ground by the loch and in the corries' floors), FOREST the heather moor
# that covers everything else, MUD peat, GRAVEL the road and the shingle.

def surfaces(h, water, f, routes, peat):
    sl = slope_deg(h)
    n = fbm(X, Z, 34, 3, 661)
    surf = np.full((N, N), S["FOREST"], dtype=np.uint8)
    turf = ((f["d_low"] <= 2) & (n > 0.36)) | ((f["d_corrie"] <= 3) & (n > 0.42)) | ((f["d_nglen"] <= 0) & (n > 0.5)) | \
           ((f["d_hanging"] <= 0) & (n > 0.5))
    surf[turf & (sl < 20)] = S["GRASS"]
    face = f["face"]
    # The hills are grass however steep, but for the crags themselves and
    # the scree fallen from them, which lies against the foot of each.
    below = gaussian_filter(face.astype(float), 2.0) > 0.1
    surf[below & (sl > 26) & (sl <= 48)] = S["SCREE"]
    surf[sl > 50] = S["ROCK"]
    surf[face] = S["ROCK"]
    # The plateau: pavements of bare rock; the top is frost-shattered
    # stone with the moss between.
    pave = fbm(X, Z, 24, 3, 662)
    fine = fbm(X, Z, 8, 2, 663)
    surf[(f["high"] > 0.5) & (pave > 0.62) & (sl < 30)] = S["ROCK"]
    stony = smoothstep(134, 150, h + 8 * (pave - 0.5))
    surf[(stony > 0.5) & (sl < 30) & (fine > 0.4)] = S["SCREE"]
    surf[(stony > 0.5) & (fine > 0.62)] = S["ROCK"]
    # The shore: shingle, sand in the bay by the start.
    d = f["shore"]
    surf[(d < 7) & (d > -40)] = S["GRAVEL"]
    surf[(d < 9) & (d > -14) & (np.abs(X + 20) < 62)] = S["SAND"]
    surf[f["crannog"]] = S["GRASS"]
    surf[f["burn_cut"] & (sl < 16)] = S["GRAVEL"]
    surf[f["burn_cut"] & (sl >= 16)] = S["WETROCK"]
    appr, drove, stair, peat_r, summit = routes
    for r in routes:
        dd, idx = cKDTree(r["pts"]).query(FLAT, distance_upper_bound=float(np.max(r["halfs"])) + 1.0)
        ok = np.isfinite(dd)
        ok[ok] = dd[ok] < r["halfs"][idx[ok]] + 0.8
        jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
        k = idx[ok]
        cur = surf[jj, ii]
        if r is appr or r is drove:
            v = np.full(len(jj), S["GRAVEL"])
        elif r is peat_r:
            v = np.where((cur == S["ROCK"]) | (cur == S["SCREE"]), cur, S["DIRT"])
        elif r is stair:
            # Turf and stones up the corrie, bare rock from the bar on
            # wherever it climbs.
            steep = np.abs(np.gradient(r["y"]))[k] > 0.14
            v = np.where(steep | (k > f["stair_k"]), S["ROCK"], np.where(cur == S["GRASS"], S["DIRT"], cur))
            v = np.where((cur == S["GRAVEL"]) | (cur == S["WETROCK"]), cur, v)
        else:
            # The mast track: broken stone, bare rock where it climbs.
            steep = np.abs(np.gradient(r["y"]))[k] > 0.17
            v = np.where(steep, S["ROCK"], np.where(cur == S["FOREST"], S["SCREE"], cur))
        surf[jj, ii] = v
    surf[f["pass"]] = S["GRAVEL"]
    surf[peat] = S["MUD"]
    return surf


# Torridonian sandstone: beds of it, purple-brown, each a little off the
# next. The top beds are quartzite, nearly white.
BEDS = [(-10, (120, 104, 102)), (14, (136, 116, 110)), (30, (114, 102, 102)), (46, (140, 120, 114)), (62, (118, 104, 104)),
        (78, (138, 120, 114)), (94, (118, 106, 106)), (110, (140, 124, 118)), (126, (140, 130, 124)), (140, (136, 130, 126)),
        (160, (140, 136, 132))]
WATER_RGB = (40, 56, 70)        # the loch: peat-dark


def bed_colour(y):
    ys = np.array([b[0] for b in BEDS], dtype=float)
    cs = np.array([b[1] for b in BEDS], dtype=float)
    return np.dstack([np.interp(y, ys, cs[:, k]) for k in range(3)])


def colours(h, water, surf, f, routes):
    """The ground's albedo: the moor a patchwork of deer grass gone
    russet, heather, bracken and moss; then lit as the game lights it, for
    the map and the previews."""
    nrm = normals(h)
    face, fx, fz = f["face"], f["face_x"], f["face_z"]
    wall_n = np.dstack([fx * 0.94, np.full_like(h, 0.34), fz * 0.94])
    nrm = np.where(face[..., None], wall_n, nrm)
    sl = slope_deg(h)
    n1 = fbm(X, Z, 9, 3, 101)
    n2 = fbm(X, Z, 40, 3, 102)
    m1 = fbm(X, Z, 64, 3, 671)
    m2 = fbm(X, Z, 23, 3, 672)
    m3 = fbm(X, Z, 120, 2, 673)

    def mix(a, b, t):
        return a * (1 - t[..., None]) + np.asarray(b, dtype=float) * t[..., None]
    # The moor: olive hill grass, with deer grass gone russet, heather
    # and (low down, where it drains) bracken through it.
    moor = np.zeros((N, N, 3)) + np.array([126.0, 140.0, 68.0])
    moor = mix(moor, (166, 132, 70), smoothstep(0.4, 0.62, m1) * 0.8)         # deer grass
    moor = mix(moor, (108, 94, 86), smoothstep(0.52, 0.66, m2) * 0.75)        # heather
    moor = mix(moor, (124, 98, 108), smoothstep(0.66, 0.78, m2) * smoothstep(0.4, 0.6, m1) * 0.6)   # still in flower
    low = smoothstep(84, 40, h)
    moor = mix(moor, (156, 104, 56), 0.7 * smoothstep(0.46, 0.32, m2) * smoothstep(9, 16, sl) * smoothstep(36, 26, sl) * low)   # bracken
    wet = smoothstep(5, 1.5, sl) * f["high"]
    moor = mix(moor, (168, 156, 100), wet * 0.55)                              # the moss's pale grasses
    moor = mix(moor, (140, 136, 104), smoothstep(100, 142, h) * 0.5)          # thin and wind-clipped up high
    # Turf: green by the loch, yellower up the corrie.
    turf = np.zeros((N, N, 3)) + np.array([104.0, 132.0, 60.0])
    turf = mix(turf, (132, 140, 66), smoothstep(0.4, 0.7, m1))
    turf = mix(turf, (138, 138, 70), smoothstep(6, 60, h) * 0.6)
    turf = mix(turf, (84, 112, 56), smoothstep(0.6, 0.72, m2) * 0.8)           # rushes
    turf = mix(turf, (156, 150, 86), smoothstep(0.4, 0.28, m2) * 0.6)          # bents gone to straw
    d_burn = cKDTree(f["burn"]).query(FLAT)[0].reshape(N, N)
    turf = mix(turf, (104, 146, 58), smoothstep(16, 5, d_burn) * 0.7)          # the burn's flush
    # Down the slopes: grass in the gullies, tongues of scree between
    # them under the crags.
    moor = mix(moor, (108, 140, 60), smoothstep(0.3, 0.8, f["gully"]) * 0.7)
    tongue = vnoise(f["along"] / 7.0, np.full_like(h, 5.0), 678)
    moor = mix(moor, (156, 146, 138), smoothstep(0.56, 0.76, tongue) * smoothstep(23, 31, sl) * 0.75)
    moor = mix(moor, (104, 118, 70), smoothstep(30, 44, sl) * 0.5)
    alb = moor.copy()
    alb[surf == S["GRASS"]] = turf[surf == S["GRASS"]]
    # Rock.
    warp = 4.0 * (fbm(X, Z, 70, 3, 674) - 0.5)
    bed = bed_colour(h + warp)
    # (beds a few metres thick, each weathered back a little from the one
    # under it: light and shade across a face in level bands)
    strata = 1.0 + 0.13 * np.sin((h + 3.0 * fbm(X, Z, 40, 2, 675)) * (2 * math.pi / 11.0))
    along = X * fz - Z * fx
    turn = np.round(np.arctan2(fz, fx) * 2.5)
    streak = 0.7 + 0.36 * vnoise(along / 2.8, turn, 676) ** 0.7
    streak *= 0.88 + 0.24 * vnoise(along / 24.0, turn, 677)
    rock = bed * (0.96 + 0.12 * (m2 - 0.5))[..., None]
    m = surf == S["ROCK"]
    alb[m] = (rock * 0.94)[m]
    alb[face] = (bed * (strata * streak)[..., None] * 0.94)[face]
    m = surf == S["SCREE"]
    alb[m] = (bed * 0.56 + np.array([132.0, 126.0, 118.0]) * 0.34)[m]
    m = surf == S["WETROCK"]
    alb[m] = (bed * 0.6)[m]
    # The top: grey stones with lichen on them, moss between.
    fine = fbm(X, Z, 8, 2, 663)
    top = smoothstep(132, 148, h)[..., None]
    stones = mix(np.zeros((N, N, 3)) + np.array([112.0, 110.0, 106.0]), (134, 130, 122), smoothstep(0.4, 0.7, n1))
    stones = mix(stones, (112, 118, 74), smoothstep(0.5, 0.36, fine) * 0.8)
    m = ((surf == S["SCREE"]) | (surf == S["ROCK"]) | (surf == S["FOREST"])) & ~face
    alb = np.where(m[..., None], alb * (1 - top) + stones * top, alb)
    alb[surf == S["GRAVEL"]] = (166, 158, 148)
    d = f["shore"]
    shingle = (surf == S["GRAVEL"]) & (d < 8)
    alb[shingle] = (150, 146, 140)
    alb[surf == S["SAND"]] = (206, 190, 150)
    alb[f["burn_cut"] & (surf == S["GRAVEL"])] = (104, 96, 88)
    alb[surf == S["DIRT"]] = (124, 98, 76)
    alb[surf == S["MUD"]] = (54, 40, 36)
    alb[(water > -999) & (d < 0)] = (90, 82, 72)      # the loch's bed
    soft = np.dstack([gaussian_filter(alb[..., c], 0.85) for c in range(3)])
    alb = np.where(face[..., None], alb, soft)
    alb *= (0.92 + 0.16 * n1)[..., None] * (0.95 + 0.1 * n2)[..., None] * (0.96 + 0.08 * m3)[..., None]
    ao = np.clip(1.0 + (h - gaussian_filter(h, 4)) * 0.05, 0.74, 1.08)
    ao = np.where(face, np.clip(ao, 0.82, 0.96), ao)
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


def place_trees(h, water, surf, f, routes, rng):
    """Scots pines (0) in a wood along the hill's foot west of the start,
    and here and there alone; birches (1) gone gold along the burns and
    the loch's east shore; rowans (2) where a burn or a wall gives one
    shelter."""
    sl = slope_deg(h)
    d_route = cKDTree(np.vstack([r["pts"] for r in routes])).query(FLAT)[0].reshape(N, N)
    d_burn = cKDTree(f["burn"]).query(FLAT)[0].reshape(N, N)
    stands = fbm(X, Z, 38, 3, 681)
    ok = (water < -999) & (surf != S["ROCK"]) & (surf != S["MUD"]) & (surf != S["GRAVEL"]) & (surf != S["SAND"]) & \
         (sl < 30) & (d_route > 7.5)
    # The pinewood.
    wood = smoothstep(60, 10, np.hypot((X + 150) / 1.6, (Z + 226) / 0.75)) * smoothstep(0.3, 0.5, stands)
    wood = np.maximum(wood, 0.5 * smoothstep(40, 10, np.hypot(X + 196, Z + 250)))
    # Birches: the lower burn, the east shore, the foot of the east hill.
    birch = smoothstep(16, 5, d_burn) * smoothstep(-80, -150, Z) * 0.8
    birch = np.maximum(birch, smoothstep(0.42, 0.6, stands) * smoothstep(90, 150, X) * smoothstep(-226, -252, Z) * smoothstep(-318, -300, Z))
    birch = np.maximum(birch, 0.5 * smoothstep(0.5, 0.64, stands) * smoothstep(70, 110, X) * smoothstep(-150, -206, Z) * (h < 40))
    trees = []
    for gz in np.arange(ORG + 6, -ORG - 6, 8.0):
        for gx in np.arange(ORG + 6, -ORG - 6, 8.0):
            p = np.array([gx + rng.uniform(-3.2, 3.2), gz + rng.uniform(-3.2, 3.2)])
            if np.hypot(*(p - SPAWN)) < 28 or not float(sample(ok.astype(float), p, order=0)[0]) > 0.5:
                continue
            roll = rng.random()
            if roll < float(sample(wood, p)[0]) * 0.85:
                trees.append((p[0], 0.0, p[1], 0, rng.uniform(0.8, 1.3), rng.uniform(0, 2 * math.pi)))
            elif roll < float(sample(birch, p)[0]) * 0.7:
                trees.append((p[0], 0.0, p[1], 1, rng.uniform(0.75, 1.25), rng.uniform(0, 2 * math.pi)))
    # The ones that stand alone: pines on the headland and the crannog,
    # by the lochans; rowans by the bridge, the bothy, the falls.
    for (x, z, kind, s_) in [(-140, -352, 0, 1.25), (-162, -340, 0, 1.0), (-54, -352, 0, 0.9), (-300, -96, 0, 1.1),
                             (-246, -54, 0, 0.95), (-296, -230, 0, 1.2), (-310, -250, 0, 1.0), (250, -246, 0, 1.15),
                             (30, -200, 2, 1.0), (10, -178, 2, 0.85), (-46, -282, 2, 1.1), (22, -48, 2, 0.9),
                             (24, 84, 2, 0.8), (-262, -150, 2, 0.9), (96, -322, 2, 0.9), (-62, -351, 2, 0.8),
                             (-38, 4, 2, 0.85), (58, -232, 1, 1.1), (74, -268, 1, 1.0), (-270, -300, 1, 1.0),
                             (-284, -262, 1, 1.15), (-28, 280, 2, 0.75)]:
        trees.append((float(x), 0.0, float(z), kind, s_, rng.uniform(0, 2 * math.pi)))
    return trees


# ---------------------------------------------------------------------------
# What stands on it

def standing_stone(x, z, y0, height, wide, thick, toward, seed):
    """A slab on end, its broad face turned toward (an angle), leaning a
    little as they all do by now."""
    lean = 0.05 * height
    dx, dz = math.sin(toward), math.cos(toward)
    prof = [(-0.25, 1.0), (0.3, 1.0), (0.7, 0.9), (0.93, 0.66), (1.0, 0.22)]
    rings = []
    for t, k in prof:
        off = lean * t * t
        rings.append((x + dx * off, y0 + height * t, z + dz * off, wide * k, thick * (0.7 + 0.3 * k)))
    return {"rings": rings, "sides": 6, "flags": ("SMOOTH",), "seed": seed}


def set_pieces(h, water, f, routes, trees, rng):
    """The bridge, the bothy and its walls, the stones, the obstacles on
    each route and the things to find. Returns a dict of the level's lists."""
    appr, drove, stair, peat, summit = routes
    rocks, spheres, boxes, prop_boxes, posts, capsules, props, views, on_trail = [], [], [], [], [], [], [], [], set()

    def gy(x, z):
        return float(sample(h, np.array([x, z], dtype=float))[0])

    def post(x, z, top, r):
        posts.append((x, gy(x, z) - 1.0, z, x, top, z, r))

    def dyke(a, b, height=0.95):
        """A dry stone wall from a to b, in lengths that follow the ground."""
        a, b = np.asarray(a, float), np.asarray(b, float)
        n = max(1, int(math.ceil(np.linalg.norm(b - a) / 11.0)))
        for i in range(n):
            p0, p1 = a + (b - a) * i / n, a + (b - a) * (i + 1) / n
            c = (p0 + p1) / 2
            lo, hi = min(gy(*p0), gy(*p1), gy(*c)), max(gy(*p0), gy(*p1), gy(*c))
            top = hi + height
            boxes.append((c[0], (top + lo - 0.5) / 2, c[1], 0.42, (top - lo + 0.5) / 2, float(np.linalg.norm(p1 - p0)) / 2 + 0.2,
                          math.atan2(p1[0] - p0[0], p1[1] - p0[1])))

    # ---- The stone bridge the drove road crosses the burn by.
    k = at(drove, *BRIDGE_AT)
    p, tg, nm = frame(drove, k)
    yaw = math.atan2(tg[0], tg[1])
    y0 = float(drove["y"][k]) + 0.06
    prop_boxes.append((p[0], y0 - 0.3, p[1], BRIDGE_HX, 0.3, BRIDGE_HZ, yaw))
    for side in (-1, 1):
        e = p + nm * side * (BRIDGE_HX - 0.25)
        a, b = e - tg * BRIDGE_HZ, e + tg * BRIDGE_HZ
        capsules.append((a[0], y0 + 0.42, a[1], b[0], y0 + 0.42, b[1], 0.25))
    props.append((p[0], p[1], yaw, PROP["STONEBRIDGE"], y0))
    f["bridge_capsules"] = 2
    views.append((p[0] - tg[0] * 13 + nm[0] * 7, p[1] - tg[1] * 13 + nm[1] * 7, "STONE BRIDGE"))

    # ---- The start: the bothy, its table, the fank beside it, the jetty.
    bx, bz, byaw = -36.0, -288.0, 0.25
    props.append((bx, bz, byaw, PROP["CABIN"], None))
    prop_boxes.append((bx, gy(bx, bz) + 1.6, bz, 4.2, 1.8, 3.2, byaw))
    props.append((-20.0, -272.0, 1.2, PROP["TABLE"], None))
    prop_boxes.append((-20.0, gy(-20, -272) + 0.4, -272.0, 0.9, 0.4, 0.4, 1.2))
    # The fank: a sheepfold of dry stone, one side fallen in.
    fx0, fz0 = -84.0, -296.0
    for a, b in (((0, 0), (20, 0)), ((20, 0), (20, 13)), ((20, 13), (12, 13)), ((4, 13), (0, 13)), ((0, 13), (0, 0)),
                 ((9, 0), (9, 7))):
        dyke((fx0 + a[0], fz0 + a[1]), (fx0 + b[0], fz0 + b[1]))
    # Dykes down from the hill to the shore either side of the in-bye.
    dyke((-98, -232), (-102, -262))
    dyke((-104, -278), (-106, -296), 0.8)
    dyke((104, -222), (110, -252))
    dyke((113, -266), (118, -296), 0.8)
    # The jetty, 42 m out into the loch (three spans of the plank bridge,
    # end to end, on raked piles), and the boat pulled up beside it.
    jx, jz, jy = 30.0, -327.5, LOCH_Y + 0.8
    d = np.hypot(X - jx, Z - (jz + BRIDGE_HZ + 2.0))
    h[:] = np.maximum(h, np.where(d < 9, jy + 0.04 - 0.14 * np.maximum(d - 2.6, 0), -1e9))
    for span in range(JETTY_SPANS):
        cz = jz - 2 * BRIDGE_HZ * span
        prop_boxes.append((jx, jy - 0.15, cz, BRIDGE_HX, 0.15, BRIDGE_HZ, 0.0))
        for side in (-1, 1):
            capsules.append((jx + side * (BRIDGE_HX - 0.15), jy + 0.3, cz - BRIDGE_HZ, jx + side * (BRIDGE_HX - 0.15), jy + 0.3,
                             cz + BRIDGE_HZ, 0.14))
            # (a pile under each side of its seaward end, leaning out: the
            # game cannot draw a log that stands dead upright)
            pz = cz - BRIDGE_HZ + 0.4
            capsules.append((jx + side * (BRIDGE_HX + 0.25), gy(jx + side * BRIDGE_HX, pz) - 0.4, pz,
                             jx + side * (BRIDGE_HX - 0.5), jy - 0.32, pz, 0.17))
        props.append((jx, cz, 0.0, PROP["BRIDGE"], jy))
    views.append((jx, jz + BRIDGE_HZ + 3.0, "OLD JETTY"))
    props.append((43.0, -318.0, 2.2, PROP["BOAT"], None))
    prop_boxes.append((43.0, gy(43, -318) + 0.3, -318.0, 0.8, 0.4, 2.0, 2.2))
    props.append((TRAILHEAD[0] + 6.5, TRAILHEAD[1], 0.2, PROP["KIOSK"], None))
    props.append((FORK[0] - 8, FORK[1] - 3, 0.5, PROP["SIGN"], None))
    props.append((MEET[0] + 10, MEET[1] + 7, 0.8, PROP["SIGN"], None))

    # ---- The standing stones on the headland's knoll.
    kx, kz = KNOLL[0], KNOLL[1]
    for i in range(7):
        a = 2 * math.pi * i / 7 + 0.3
        sx, sz = kx + math.sin(a) * 7.0, kz + math.cos(a) * 7.0
        hgt = rng.uniform(2.2, 3.6)
        rocks.append(standing_stone(sx, sz, gy(sx, sz), hgt, rng.uniform(0.55, 0.8), rng.uniform(0.2, 0.3), a + math.pi, 700 + i))
        post(sx, sz, gy(sx, sz) + hgt, 0.55)
    views.append((kx, kz, "STANDING STONES"))
    # ---- The crannog: what is left of a wall on it.
    cx, cz = CRANNOG[0], CRANNOG[1]
    dyke((cx - 5, cz + 3), (cx + 3, cz + 4.5), 1.2)
    dyke((cx + 3, cz + 4.5), (cx + 5, cz - 2), 0.7)
    views.append((cx - 1, cz - 1, "THE CRANNOG"))
    views.append((PEAT[0][0] + 16, PEAT[0][1] + 6, "BLACK MOSS"))
    views.append((HUMMOCKS[2][0], HUMMOCKS[2][1], "THE HUMMOCKS"))
    # Driftwood on the beach, a pine down in the wood.
    for (lx, lz, ang, L, rr) in [(-52, -321, 1.4, 5.5, 0.2), (2, -322, 1.8, 4.5, 0.16), (-120, -236, 0.5, 8.0, 0.3),
                                 (-176, -244, 2.2, 7.0, 0.26)]:
        g = gy(lx, lz)
        dx, dz = math.sin(ang) * L / 2, math.cos(ang) * L / 2
        capsules.append((lx - dx, g + rr * 0.8, lz - dz, lx + dx, g + rr * 0.8, lz + dz, rr))

    # ---- Up the hill.
    # A shieling's ruin on the corrie's flats.
    sx, sz, syaw = -40.0, -6.0, 0.5
    props.append((sx, sz, syaw, PROP["RUIN"], None))
    prop_boxes.append((sx, gy(sx, sz) + 0.7, sz, 3.0, 0.9, 2.2, syaw))
    views.append((sx + 8, sz - 5, "THE SHIELING"))
    views.append((0.0, 70.0, "GREY MARE'S TAIL"))
    views.append((LOCHAN_DUBH[0] + 32, LOCHAN_DUBH[1] - 6, "LOCHAN DUBH"))
    k = at(drove, 88, 186)
    views.append((drove["pts"][k][0], drove["pts"][k][1], "THE HAIRPINS"))
    # The crag's edge: back from where the plateau ends over the corrie.
    ex, ez = -156.0, 70.0
    y_here = gy(ex, ez)
    while gy(ex + 3.0, ez) > y_here - 5.0 and ex < -60:
        ex += 1.5
    views.append((ex - 5.0, ez, "THE CRAG"))
    views.append((MEET[0], MEET[1], "THE BEALACH"))
    # A camp by the lochan north of the pass.
    cp, cyaw = np.array([27.0, 262.0]), -1.45
    props.append((cp[0], cp[1], cyaw, PROP["TENT"], None))
    for side in (-1, 1):
        q = cp + np.array([math.sin(cyaw), math.cos(cyaw)]) * 4.0 + np.array([math.cos(cyaw), -math.sin(cyaw)]) * side * 1.6
        dx, dz = math.sin(cyaw) * 0.9, math.cos(cyaw) * 0.9
        capsules.append((q[0] - dx, gy(q[0], q[1]) + 0.18, q[1] - dz, q[0] + dx, gy(q[0], q[1]) + 0.18, q[1] + dz, 0.18))
    views.append((cp[0], cp[1], "LOCHAN CAMP"))
    # An old truck where the peat cutters left it, out on the moss.
    wk, wyaw = np.array([-218.0, 86.0]), 2.3
    prop_boxes.append((wk[0], gy(*wk) + 0.9, wk[1], 0.9, 0.9, 2.2, wyaw))
    props.append((wk[0], wk[1], wyaw, PROP["WRECK"], None))
    views.append((wk[0], wk[1], "OLD TIMER"))
    # The top: the mast and its hut, the trig point.
    mx, mz = SUMMIT[0] + 11.0, SUMMIT[1] + 7.0
    my = gy(mx, mz)
    props.append((mx, mz, 0.0, PROP["MAST"], None))
    for sx_, sz_ in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        posts.append((mx + 1.7 * sx_, gy(mx + 1.7 * sx_, mz + 1.7 * sz_) - 0.4, mz + 1.7 * sz_, mx + 0.35 * sx_, my + 17.0,
                      mz + 0.35 * sz_, 0.2))
    prop_boxes.append((mx + 6.5, gy(mx + 6.5, mz - 4.0) + 1.1, mz - 4.0, 1.7, 1.3, 1.3, 0.0))
    # Tors: what frost has left standing of the top bed, and its litter.
    for i, (tx, tz, big) in enumerate([(-14, 14, 1.0), (16, -16, 0.8), (-26, -27, 0.7), (30, 18, 0.9)]):
        x, z = SUMMIT[0] + tx, SUMMIT[1] + tz
        y0 = gy(x, z)
        rocks.append({"rings": [(x, y0 - 0.6, z, 2.6 * big, 2.1 * big), (x + 0.2, y0 + 1.1 * big, z, 2.8 * big, 2.2 * big),
                                (x - 0.2, y0 + 1.9 * big, z + 0.3, 2.0 * big, 1.8 * big), (x + 0.3, y0 + 2.9 * big, z, 2.3 * big, 1.6 * big),
                                (x, y0 + 3.7 * big, z, 1.3 * big, 1.1 * big), (x, y0 + 4.0 * big, z, 0.3, 0.3)],
                      "sides": 8, "flags": (), "seed": 720 + i})
        post(x, z, y0 + 3.6 * big, 2.0 * big)
    props.append((SUMMIT[0] - 5.0, SUMMIT[1] + 5.0, 0.0, PROP["TRIG"], None))
    post(SUMMIT[0] - 5.0, SUMMIT[1] + 5.0, gy(SUMMIT[0] - 5.0, SUMMIT[1] + 5.0) + 1.2, 0.45)
    views.append((SUMMIT[0], SUMMIT[1], "THE SUMMIT"))

    # Each way's rating on a post a little way up it from the fork (and
    # the mast track's from the pass): a green circle, a blue square, a
    # black diamond.
    for r, var, k in ((drove, 0, 60), (peat, 1, 26), (stair, 2, 24), (summit, 1, 24)):
        p, tg, nm = frame(r, k)
        # (well off the line: the old pickup comes round the pass wide)
        q = p - nm * (float(r["halfs"][k]) + 2.6)
        props.append((q[0], q[1], math.atan2(-tg[0], -tg[1]), PROP["PLATE"], None, var))
        posts.append((q[0], gy(*q) - 0.5, q[1], q[0], gy(*q) + 1.5, q[1], 0.12))
    # Passing place signs, and the poles of the line over the pass.
    for q in f["passing"][::2]:
        p, tg, nm = frame(drove, q)
        c = p + nm * (float(drove["halfs"][q]) + 1.2)
        props.append((c[0], c[1], math.atan2(tg[0], tg[1]) + math.pi / 2, PROP["SIGN"], None))
    n = len(drove["pts"])
    k, last = at(drove, 50, -172), None
    while k < n - 30 and sum(1 for q in props if q[3] == PROP["POLE"]) < 10:
        p, tg, nm = frame(drove, k)
        if drove["turn"][k] < 0.25:
            # On the side the hill is on.
            side = 1.0 if gy(*(p + nm * 8)) > gy(*(p - nm * 8)) else -1.0
            c = p + nm * side * (float(drove["halfs"][k]) + 3.0)
            if last is None or np.linalg.norm(c - last) > 60:
                props.append((c[0], c[1], math.atan2(tg[0], tg[1]), PROP["POLE"], None))
                post(c[0], c[1], gy(*c) + 6.5, 0.2)
                last = c
        k += 6

    # ---- Obstacles, harder the further up each route.
    def boulder(c, rr, sink=0.4, trail=False):
        if trail:
            on_trail.add(len(spheres))
        spheres.append((c[0], gy(*c) - rr * sink, c[1], rr))

    def beside(r, k, rr, gap=0.8):
        p, tg, nm = frame(r, k)
        half = float(r["halfs"][k])
        boulder(p + nm * rng.choice([-1, 1]) * (half + gap + rr + rng.random() * 2.0), rr)

    # The drove road: what has rolled down beside it, nothing on it.
    n = len(drove["pts"])
    k = 70
    while k < n - 20:
        beside(drove, k, 0.4 + rng.random() * 0.8, 1.2)
        k += int(16 + rng.random() * 18)
    # The peat road: stones along it, a pair of rock steps on the rake.
    n = len(peat["pts"])
    k = 200
    while k < n - 20:
        beside(peat, k, 0.35 + rng.random() * 0.7)
        k += int(13 + rng.random() * 14)
    for (x, z, step) in ((-200, -28, 0.14), (-170, -8, 0.17), (-195, 38, 0.2)):
        kk = at(peat, x, z)
        if float(np.max(peat["turn"][kk - 6:kk + 8])) < 0.22:
            boxes.append(ledge(peat, kk, step, float(peat["halfs"][kk]) + 1.2, rng))
    # The staircase: boulders up the corrie's floor to thread between,
    # steps in the rock bar's notch, a flight of them on every ramp.
    k0, k1 = at(stair, -12, -160), f["stair_k"]
    k = k0
    while k < k1 - 6:
        diff = (k - k0) / max(1, k1 - k0)
        p, tg, nm = frame(stair, k)
        half = float(stair["halfs"][k])
        if water[tuple(np.round(to_grid(p)[::-1]).astype(int))] < -999:
            for _ in range(1 + int(diff * 2.4)):
                off = rng.uniform(-half - 4.0, half + 4.0)
                rr = 0.35 + 0.7 * diff * rng.random() + 0.25 * rng.random()
                if abs(off) < rr + 2.0:
                    off = math.copysign(rr + 2.0, off)       # always a gap a truck fits down the middle
                boulder(p + nm * off + tg * rng.uniform(-3, 3), rr, 0.45, trail=abs(off) < half + rr)
        k += int(12 + rng.random() * 9)
    last = -100
    n = len(stair["pts"])
    for kk in range(at(stair, 30, -70) + 4, n - 16, 3):
        grade = float(np.max(np.abs(np.diff(stair["y"][kk - 5:kk + 9]))))
        on_rock = kk > k1 or kk < at(stair, 30, -34)
        # (on the treads, never the pitches: a step on the steepest of it
        # stops a long truck dead)
        if not on_rock or grade < 0.08 or grade > 0.24 or kk - last < 13 or float(np.max(stair["turn"][kk - 7:kk + 9])) > 0.2:
            continue
        diff = kk / n
        boxes.append(ledge(stair, kk, 0.08 + 0.1 * diff + 0.03 * rng.random(), float(stair["halfs"][kk]) + 1.6, rng))
        last = kk
    # The mast track: loose stone to its sides.
    n = len(summit["pts"])
    k = 30
    while k < n - 14:
        p, tg, nm = frame(summit, k)
        half = float(summit["halfs"][k])
        rr = 0.3 + 0.5 * rng.random()
        boulder(p + nm * rng.choice([-1, 1]) * (half + 0.3 + rr + rng.random() * 1.5) + tg * rng.uniform(-2, 2), rr, 0.45)
        k += int(10 + rng.random() * 9)
    # The boulder field under the head wall, and blocks at the foot of
    # every crag.
    d_route = cKDTree(np.vstack([r["pts"] for r in routes]))
    for _ in range(34):
        c = np.array([-24.0, 56.0]) + rng.normal(0, 12.0, 2)
        if d_route.query(c)[0] > 5.5 and water[tuple(np.round(to_grid(c)[::-1]).astype(int))] < -999:
            boulder(c, 0.5 + rng.random() ** 2 * 1.6, 0.35)
    face = f["face"]
    foot = face & (np.roll(h, 1, 0) + np.roll(h, -1, 0) + np.roll(h, 1, 1) + np.roll(h, -1, 1) > 4 * h + 6)
    jj, ii = np.nonzero(foot & (np.abs(X) < 370) & (np.abs(Z) < 370))
    for q in rng.permutation(len(jj))[:170]:
        c = np.array([X[jj[q], ii[q]], Z[jj[q], ii[q]]]) + np.array([f["face_x"][jj[q], ii[q]], f["face_z"][jj[q], ii[q]]]) * rng.uniform(4, 18)
        if abs(c[0]) > 372 or abs(c[1]) > 372 or d_route.query(c)[0] < 9 or water[tuple(np.round(to_grid(c)[::-1]).astype(int))] > -999:
            continue
        boulder(c + rng.normal(0, 1.5, 2), 0.6 + rng.random() ** 2 * 2.0, 0.35)
    for _ in range(70):
        c = SUMMIT + rng.normal(0, 26.0, 2)
        if d_route.query(c)[0] > 5.0 and abs(c[0]) < 372 and abs(c[1]) < 372 and np.hypot(*(c - SUMMIT)) > 5.0:
            boulder(c, 0.25 + rng.random() ** 2 * 0.9, 0.4)
    # Erratics the ice left on the flats.
    for (x, z, rr) in [(62, -262, 2.3), (-140, -290, 1.7), (238, -258, 2.0), (-226, -262, 1.5), (96, -238, 1.2),
                       (-60, -246, 1.3), (176, -302, 1.6), (-30, -60, 1.9), (38, 30, 1.6), (-300, -278, 1.8)]:
        boulder(np.array([float(x), float(z)]), rr, 0.3)
    for _ in range(60):
        c = np.array([rng.uniform(-44, 54), rng.uniform(-190, 70)])
        if d_route.query(c)[0] > 7.0 and water[tuple(np.round(to_grid(c)[::-1]).astype(int))] < -999:
            boulder(c, 0.35 + rng.random() ** 2.5 * 1.5, 0.35)
    return {"rocks": rocks, "spheres": clear_roads(spheres, routes, on_trail), "boxes": boxes, "prop_boxes": prop_boxes,
            "posts": posts, "capsules": capsules, "props": props, "views": views}


# ---------------------------------------------------------------------------
# The extreme trail: THE BLACK RAKE
#
# A stalker's path at its worst, up the whole of the corrie's east wall,
# which no road dares more than the foot of. It leaves the Devil's
# Staircase on the corrie's floor above the rock bar and takes the grass
# of the wall on a long rising slant (THE BRAE), the ground leaning out
# from under it, to the drove road's shelf at the old road-menders' quarry.
# From the back of the quarry the rake itself goes south: a ledge across
# the great crag, sandstone standing over it and nothing under it but the
# corrie, the loch ahead all the way (THE BAD STEP where the wall is
# sheerest, a bay to breathe in at EAGLE'S LEDGE). It comes out on the
# hill's shoulder at CARN DUBH, turns north along the plateau's rim and
# crosses the moss to the top: through the peat hags, over a lochan by a
# causeway that lies just under the water (the rest of it is over any
# truck's intake), up wet slabs that lean, through the frost-broken
# stones and over two ledges of the top bed, and down on to the mast
# track for the last of the climb.
#
# The rake's line is found, not drawn: at each step south it is put
# where the wall already stands at the height it has climbed to, so the
# ledge is cut into the face it crosses and not through it.
#
# It is laid over the finished hill (build): everything else on the hill
# is where it was before there was a trail, to the centimetre, and the
# three ways up drive as they did. What it took to get there: the routes
# it touches get their own ground back from a copy, not from their
# heights (the burn's ford is cut into the staircase after it is laid);
# only its banks on the wall are smoothed; and it comes on to the mast
# track well short of the top, so a CPU driver that arrives by it is
# standing on the track it goes home by.

RAKE_HALF = 2.8
RAKE_GRADE = 0.345
LOCHAN_RAKE = (217.0, 122.0, 19.0, 1.7)         # the lochan the causeway crosses: x, z, radius, depth
CAUSEWAY_RAKE = [(213.0, 98.0), (211.0, 112.0), (214.0, 122.0), (223.0, 131.0), (224.0, 146.0)]
CAUSEWAY_DEEP = 0.3                             # how far under the water it lies
POOLS_RAKE = [(229.0, 33.0, 7.5, 0.8), (195.0, 61.0, 6.5, 0.7)]
QUARRY = (140.0, 81.0, 11.5, 9.5)               # the bay at the road's bend: x, z, half its width and its depth
QUARRY_FACE, QUARRY_FOOT, QUARRY_SPOIL = 2.4, 0.8, 2.0   # its back's pitch, the pitch of the spoil at its foot, how far out that reaches
HAGS = [(214.0, 22.0, 8.0, 0.3), (212.0, 38.0, 8.5, 0.32), (209.0, 54.0, 8.0, 0.3), (210.0, 70.0, 7.0, 0.26)]


RAKE_WALL = 1.5      # the wall over the ledge, metres up a metre in from its inner edge
RAKE_DROP = 1.3      # the ground under it, metres down a metre out from its outer edge
RAKE_PLANE = 10.0    # how far out from the ledge's edges those go


def rake_planes(h, r, k0, k1):
    """The wall over the rake's ledge and the ground under it as planes
    from the ledge's two edges, the ledge between them.

    A bench let into a crag with a sheer back and a sheer front is an
    edge the ground's grid cannot draw: the ledge runs at a slant to it,
    so along each edge every other vertex fell on the ledge and the next
    on the wall, metres higher, and the game drew a saw of jutting
    triangles down the length of the rake, ribs standing over them. A
    plane comes out as a plane however the grid lies across it. So the
    wall is laid back to RAKE_WALL from the ledge's inner edge wherever it
    stood steeper than that (above, it is the crag as it was), and the
    ground under the outer edge filled out to RAKE_DROP: a rake with a
    foot to its wall and a shoulder to its drop, as the real ones have,
    where it was a plank in a cliff. Eased in over the rake's ends."""
    pts, y = r["pts"], r["y"]
    half = float(np.max(r["halfs"][k0:k1]))
    jj, ii, idx, d, lat = xt_band(r, half + RAKE_PLANE, k0, k1)
    hw, yb = r["halfs"][idx], y[idx]
    off = d - hw
    # (a truck climbing the rake goes south: the wall is on its left)
    ends = np.clip(np.minimum(idx - k0, k1 - 1 - idx) / 10.0, 0, 1)
    fade = ends * smoothstep(RAKE_PLANE, RAKE_PLANE - 3.0, off)
    cur = h[jj, ii]
    wall = (lat < 0) & (off > 0)
    drop = (lat > 0) & (off > 0)
    to = np.where(wall, np.minimum(cur, yb + RAKE_WALL * off), np.where(drop, np.maximum(cur, yb - RAKE_DROP * off), cur))
    h[jj, ii] = cur + (to - cur) * fade


def black_rake(h, water, f, routes):
    """The trail's line and its ground. Returns what the later steps need:
    the route (levelkit.xt_route), its places as point indices, and the
    peat to lay with the rest."""
    appr, drove, stair, peat, summit = routes
    x = {}
    h_was = h.copy()
    k_in = at(stair, 27, -26)
    p_in = stair["pts"][k_in]
    k_road = at(drove, 129, 80)
    p_road = drove["pts"][k_road]
    y_road = float(drove["y"][k_road])
    hs = gaussian_filter(h, 0.8)

    # The rake: south from the quarry's back, each point where the wall
    # stands at the height the ledge has reached.
    z0, x0, y0 = 70.0, 146.0, y_road + 1.2
    zs = np.arange(z0, -65.0, -2.0)
    xs_ = np.full(len(zs), x0)
    cand = np.arange(x0, 216.0, 0.5)
    for _ in range(5):
        seg = np.hypot(np.diff(xs_), np.diff(zs))
        yt = y0 + RAKE_GRADE * np.concatenate([[0], np.cumsum(seg)])
        for i, z in enumerate(zs):
            hh = sample(hs, np.stack([cand, np.full(len(cand), z)], axis=1))
            j = int(np.argmax(hh >= yt[i])) if np.any(hh >= yt[i]) else len(cand) - 1
            xs_[i] = cand[j]
        xs_ = gaussian_filter1d(np.maximum.accumulate(xs_), 5.0, mode="nearest")
        xs_[0] = x0
    rake = [(float(a), float(b), None) for a, b in zip(xs_[::4], zs[::4])]
    x_top, z_top = rake[-1][0], rake[-1][1]
    # Where it comes on to the mast track, 26 m short of the top, and
    # where its own stretch ends on it.
    j0, j1 = len(summit["pts"]) - 26, len(summit["pts"]) - 19
    pj0, pj1 = summit["pts"][j0], summit["pts"][j1]
    x["j1"] = j1

    ctrl = [
        (p_in[0], p_in[1], None), (34, -19, None), (46, -11, None), (58, -4, None),
        # The brae: one slant up the wall's grass to the road.
        (70, 9, None), (84, 26, None), (98, 44, None), (111, 60, None), (121, 71, None), (p_road[0], p_road[1], None),
        # Round the quarry's floor and out of the back of it.
        (135.5, 86.5, None), (142.5, 87.5, None), (147.5, 83, None), (148, 76.5, None),
    ] + rake + [
        # Out on the shoulder, and round to the north.
        (x_top + 8, z_top - 6, None), (x_top + 17, z_top - 4, None), (x_top + 22, z_top + 6, None),
        (x_top + 21, z_top + 20, None), (220, -18, None),
        # The moss: the hags, the causeway, the slabs.
        (215, 4, None), (214, 22, None), (212, 38, None), (209, 54, None), (210, 70, None), (212, 84, None),
    ] + [(a, b, None) for a, b in CAUSEWAY_RAKE] + [
        (220, 160, None), (213, 174, None), (205, 188, None), (198, 202, None),
        # The stones, and the top.
        (190, 212, None), (181, 221, None), (174, 231, None), (166, 240, None), (159, 249, None), (154, 257, None),
        (151.3, 262.5, None),
        # Down on to the mast track for the last of it (a CPU driver that
        # has arrived this way then stands on the track it goes home by).
        (pj0[0], pj0[1], None), (pj1[0], pj1[1], None),
    ]
    r = xt_route(h, ctrl, "THE BLACK RAKE", "BLK RAKE", RAKE_HALF)
    pts = r["pts"]
    n = len(pts)

    def k_at(px, pz):
        return xt_k(r, px, pz)

    def gnd(k):
        return float(np.mean(sample(h, pts[max(0, k - 2):k + 3])))
    k_edge = k_at(60, -3)                                   # where the floor ends and the wall begins
    k_lean0, k_lean1 = k_at(82, 23), k_at(104, 52)
    k_road_x = int(np.argmin(np.linalg.norm(pts[:k_at(148, 76.5)] - p_road, axis=1)))
    k_q0, k_q1 = k_at(142.5, 87.5), k_at(148, 76.5)        # on the quarry's floor
    k_rake0 = k_at(x0, z0)
    k_top = k_at(x_top, z_top)
    k_bad = k_at(float(np.interp(36.0, zs[::-1], xs_[::-1])), 36.0)
    k_ledge = k_at(float(np.interp(8.0, zs[::-1], xs_[::-1])), 8.0)
    k_carn = k_at(x_top + 20, z_top + 2)
    k_hag0, k_hag1 = k_at(214, 14), k_at(210, 76)
    k_ford0, k_ford1 = k_at(*CAUSEWAY_RAKE[0]), k_at(*CAUSEWAY_RAKE[-1])
    k_slab0, k_slab1 = k_ford1 + 8, k_at(203, 192)
    k_st0, k_st1 = k_at(196, 205), k_at(182, 220)
    x.update(k_in=k_in, k_road=k_road, k_edge=k_edge, k_lean=(k_lean0, k_lean1), k_road_x=k_road_x, k_q=(k_q0, k_q1),
             k_rake0=k_rake0, k_top=k_top, k_bad=k_bad, k_ledge=k_ledge, k_carn=k_carn, k_hag=(k_hag0, k_hag1),
             k_ford=(k_ford0, k_ford1), k_slab=(k_slab0, k_slab1), k_st=(k_st0, k_st1))

    # The lochan on the moss first (its level sets the causeway's), in a
    # hollow of its own; then the causeway raised across its bed.
    level = lochan(h, water, LOCHAN_RAKE, 655)
    x["lochan_y"] = level
    cw = resample(smooth_path(np.array(CAUSEWAY_RAKE, dtype=float), 3), 1.0)
    dc = cKDTree(cw).query(FLAT)[0].reshape(N, N)
    dl = np.hypot(X - LOCHAN_RAKE[0], Z - LOCHAN_RAKE[1])
    bar = level - CAUSEWAY_DEEP - 0.55 * np.maximum(dc - 3.3, 0)
    h[:] = np.where(dl < LOCHAN_RAKE[2] * 1.3, np.maximum(h, bar), h)
    wet = (dl < LOCHAN_RAKE[2] * 1.25) & (h < level - 0.03) & (dc < 6.0)
    water[wet] = np.maximum(water[wet], level)
    # And two of the moss's black pools by the hags, to pass between.
    for k, spec in enumerate(POOLS_RAKE):
        lochan(h, water, spec, 656 + k)

    # Heights. Where it meets the staircase and where it crosses the road
    # it is their ground; the brae is one grade from the floor's edge to
    # the road; the quarry's floor is the road's height; the rake one grade
    # with a level bay in the middle of it; the moss as it lies, the hags
    # sunk into it, the causeway level; the summit's stones in two steps.
    BED = 6
    road_tree = cKDTree(drove["pts"])
    marks = [(0, float(stair["y"][k_in])), (k_edge, gnd(k_edge))]
    marks += [(k, float(drove["y"][int(road_tree.query(pts[k])[1])])) for k in range(k_road_x - BED, k_road_x + BED + 1, 3)]
    y_q = y_road + 0.9
    y_rake0 = y_q + 0.3
    y_top = gnd(k_top + 4)
    LEDGE = 4                                               # half the bay's length
    g = (y_top - y_rake0) / (k_top - k_rake0 - 2 * LEDGE)
    y_ledge = y_rake0 + g * (k_ledge - LEDGE - k_rake0)
    marks += [(k_q0, y_q), (k_q1, y_q + 0.1), (k_rake0, y_rake0), (k_ledge - LEDGE, y_ledge), (k_ledge + LEDGE, y_ledge + 0.15),
              (k_top, y_top), (k_carn, gnd(k_carn))]
    marks += [(k, gnd(k)) for k in range(k_carn + 12, k_hag0 - 4, 12)]
    y_h0, y_h1 = gnd(k_hag0), gnd(k_hag1)
    marks += [(k_hag0, y_h0), (k_hag0 + 5, y_h0 - 0.55), (k_hag1 - 5, y_h1 - 0.55), (k_hag1, y_h1),
              (k_ford0 - 5, level + 0.5), (k_ford0, level - CAUSEWAY_DEEP), (k_ford1, level - CAUSEWAY_DEEP),
              (k_ford1 + 5, level + 0.6), (k_slab1, gnd(k_slab1))]
    # The stones: level ground to thread them on, then two ledges of the
    # top bed, each across a level tread with a pitch behind it.
    TREAD, BLOCK = 6, 3
    k_j0 = k_at(pj0[0], pj0[1])
    y_j0, y_j1 = float(summit["y"][j0]), float(summit["y"][j1])
    y_s0 = gnd(k_st0)
    k_l1 = k_st1 + 2
    k_l2 = k_l1 + TREAD + BLOCK + 6
    rise = 0.5                                              # what the pitch between them climbs
    y_a = gnd(k_st1)
    marks += [(k_st0, y_s0), (k_st1, y_a), (k_l1 + TREAD, y_a), (k_l1 + TREAD + 1, y_a + 0.2), (k_l1 + TREAD + BLOCK, y_a + 0.2),
              (k_l2, y_a + 0.2 + rise), (k_l2 + TREAD, y_a + 0.2 + rise), (k_l2 + TREAD + 1, y_a + 0.44 + rise),
              (k_l2 + TREAD + BLOCK + 2, y_a + 0.44 + rise), (k_j0 - 3, y_j0 + 0.1), (k_j0, y_j0), (n - 1, y_j1)]
    assert k_l2 + TREAD + BLOCK < k_j0 - 9, "no room for the top's ledges"
    x["ledges"] = [(k_l1 + TREAD, y_a + 0.2), (k_l2 + TREAD, y_a + 0.44 + rise)]
    marks.sort()
    xt_ramp(r, marks, 1.0)
    y = r["y"]
    x["grades"] = (float((y[k_road_x - BED] - y[k_edge]) / (k_road_x - BED - k_edge)), float(g))

    # Widths: room to turn on the quarry's floor and on the shoulder, a
    # bay at the ledge; the rake itself no wider than it must be.
    kk = np.arange(n)
    r["halfs"] = np.full(n, RAKE_HALF) + 0.5 * r["turn"]
    # (A ledge 0.3 m narrower fell between the ground's vertices, 3 m
    # apart, with no level bed left on it in places: every CPU truck went
    # over the edge. The bad step is made by a block and a lean instead.)
    r["halfs"] += 1.7 * np.exp(-((kk - k_ledge) / 5.0) ** 2) + 1.2 * np.exp(-((kk - k_carn) / 8.0) ** 2)

    # The ground to it: a narrow bench on the wall (a cut would take the
    # crag back to a bank), an easier one on the moss.
    def lay(k0, k1, shoulder):
        lay_route(h, {"pts": pts[k0:k1], "y": y[k0:k1], "halfs": r["halfs"][k0:k1]}, shoulder)
    lay(6, k_edge + 2, 3.0)
    lay(k_edge, k_road_x - 2, 5.0)
    lay(k_rake0 - 2, k_top + 2, 3.0)
    lay(k_top, k_ford0 - 4, 4.0)
    # The slabs leave the lochan up through the bank it lies under, four
    # metres of it: a gully with its sides laid back, not a slot.
    lay(k_slab0 - 2, k_slab1 + 2, 9.0)
    lay(k_ford1 + 4, n, 3.0)
    # The quarry: a floor at the road's height let into the slope east of
    # it, its back a face.
    qx, qz, qa, qb = QUARRY
    e = np.hypot((X - qx) / qa, (Z - qz) / qb)
    floor = y_q + 0.02 * (X - qx)
    # (the face stands on a foot of spoil, QUARRY_FOOT up a metre at the
    # floor and the face's own pitch a few metres out: where face met floor
    # square, the grid drew the corner as a row of teeth, as on the rake)
    out = np.maximum(e - 1.0, 0) * qb
    cut = floor + QUARRY_FACE * out - (QUARRY_FACE - QUARRY_FOOT) * QUARRY_SPOIL * (1 - np.exp(-out / QUARRY_SPOIL))
    east = X > p_road[0] + 1.0
    h[:] = np.where(east & (cut < h), cut, h)
    x["quarry"] = east & (e < 1.25)
    lay(k_q0 - 6, k_rake0 + 6, 2.5)
    # Its banks on the wall smoothed along it, as the other tracks' are
    # (not by the staircase or on the top: what stands there stays as it
    # was, to the centimetre, and so does what the CPU makes of it).
    smooth_banks(h, [{"pts": pts[a:b], "halfs": r["halfs"][a:b]} for a, b in ((k_edge + 8, k_road_x - 12), (k_rake0 + 4, k_top + 6))])
    lay(k_rake0 - 2, k_top + 2, 2.2)
    rake_planes(h, r, k_rake0 + 2, k_top + 2)
    # The staircase, the road and the mast track keep their own ground, as
    # it was (the burn's ford through the first, the others' lumps and all).
    for q, k0, k1 in ((stair, max(0, k_in - 40), k_in + 40), (drove, k_road - 40, k_road + 40),
                      (summit, len(summit["pts"]) - 45, len(summit["pts"]))):
        d, idx = cKDTree(q["pts"][k0:k1]).query(FLAT, distance_upper_bound=float(np.max(q["halfs"][k0:k1])))
        ok = np.isfinite(d)
        ok[ok] = d[ok] < q["halfs"][k0:k1][idx[ok]]
        h.ravel()[ok] = h_was.ravel()[ok]
    # The brae leans: its bed tilts out, downhill to the left of the way up.
    # So does the bad step, a little, the other way (the drop is on the
    # right of a truck going south), and the wet slabs on the top.
    xt_camber(h, r, k_lean0, k_lean1, -0.19, ease=8.0)
    xt_camber(h, r, k_bad - 2, k_bad + 13, 0.07, ease=5.0)
    xt_camber(h, r, k_slab0 + 4, k_slab1 - 6, -0.11, ease=7.0)
    # The hags: the track sunk between banks of peat that stand over it.
    jj, ii, idx, d, lat = xt_band(r, 9.0, k_hag0, k_hag1)
    along = np.minimum(idx - k_hag0, k_hag1 - 1 - idx)
    bank = np.exp(-((d - 5.2) / 1.7) ** 2) * np.clip(along / 6.0, 0, 1)
    lump = 0.5 + fbm(X, Z, 5.0, 2, 667)[jj, ii]
    h[jj, ii] += 0.7 * bank * lump
    x["r"] = r
    x["peat"] = list(HAGS)
    x["bed"] = cKDTree(pts).query(FLAT)[0].reshape(N, N)
    return x


def black_rake_surfaces(h, surf, f, x):
    """Its surfaces: a path worn to earth on the brae and the moss, bare
    rock on the rake's ledge, the slabs wet, peat in the hags."""
    r = x["r"]
    n = len(r["pts"])
    k_slab0, k_slab1 = x["k_slab"]
    k_st0, _ = x["k_st"]
    xt_surface(surf, r, 4, x["k_road_x"] - 5, S["DIRT"], extra=0.2)
    surf[x["quarry"]] = S["GRAVEL"]
    # The rake is what its name says, a ledge the grass has, slanting
    # green across the dark of the crag: turf along its lip, the path worn
    # to earth under the wall. (All turf, the heaviest truck slid off it.)
    xt_surface(surf, r, x["k_rake0"], x["k_top"], S["ROCK"], extra=0.4)
    jj, ii, idx, d, lat = xt_band(r, RAKE_HALF + 0.6, x["k_rake0"] + 10, x["k_top"] - 2)
    lip = lat > 0.9
    surf[jj[lip], ii[lip]] = S["GRASS"]
    xt_surface(surf, r, x["k_top"], x["k_hag"][0] + 2, S["DIRT"], extra=0.0)
    xt_surface(surf, r, x["k_hag"][1] - 2, x["k_ford"][0] - 4, S["DIRT"], extra=0.0)
    xt_surface(surf, r, x["k_ford"][1] + 4, k_slab0, S["DIRT"], extra=0.0)
    xt_surface(surf, r, k_slab0, k_slab1, S["WETROCK"], extra=3.0)
    xt_surface(surf, r, k_slab1, k_st0, S["ROCK"], extra=1.0)
    xt_surface(surf, r, k_st0, n - 2, S["SCREE"], extra=0.3)
    # The causeway: stones.
    cw = resample(smooth_path(np.array(CAUSEWAY_RAKE, dtype=float), 3), 1.0)
    dc = cKDTree(cw).query(FLAT)[0].reshape(N, N)
    dl = np.hypot(X - LOCHAN_RAKE[0], Z - LOCHAN_RAKE[1])
    surf[(dc < 4.0) & (dl < LOCHAN_RAKE[2] * 1.3)] = S["GRAVEL"]


def black_rake_pieces(h, h0, water, f, x, sp, trees, routes, rng):
    """What stands along it, what the CPU's driver knows of it and its
    places. Adds to set_pieces' lists (sp) and to the trees; returns the
    route for the header (levelkit.xt_full) and an eagle for the crag.
    h0: the ground before the trail was laid."""
    appr, drove, stair, peat, summit = routes
    r = x["r"]
    pts, y, n = r["pts"], r["y"], len(r["pts"])
    k_lean0, k_lean1 = x["k_lean"]
    k_q0, k_q1 = x["k_q"]
    k_hag0, k_hag1 = x["k_hag"]
    k_ford0, k_ford1 = x["k_ford"]
    k_slab0, k_slab1 = x["k_slab"]
    k_st0, k_st1 = x["k_st"]
    k_edge, k_rake0, k_top, k_bad, k_ledge, k_carn = (x[q] for q in ("k_edge", "k_rake0", "k_top", "k_bad", "k_ledge", "k_carn"))

    def gy(px, pz):
        return float(sample(h, np.array([px, pz], dtype=float))[0])
    # Nothing of the hill's own litter lies in the trail (the blocks under
    # the crags, the stones about the top), past where it leaves the
    # staircase with the staircase's own.
    line = cKDTree(pts[10:])
    keep = []
    for s_ in sp["spheres"]:
        d_, i_ = line.query((s_[0], s_[2]))
        if d_ > float(r["halfs"][10 + i_]) + s_[3] + 1.3:
            # (one beside it goes up or down with the bank it lies on)
            moved = float(sample(h - h0, np.array([s_[0], s_[2]]))[0])
            keep.append((s_[0], s_[1] + moved, s_[2], s_[3]))
    sp["spheres"] = keep
    trees[:] = [t for t in trees if line.query((t[0], t[2]))[0] > RAKE_HALF + 3.0]
    spheres, boxes, props, posts, views = sp["spheres"], sp["boxes"], sp["props"], sp["posts"], sp["views"]

    def stone(c, rr, sink=0.4):
        spheres.append((c[0], gy(*c) - rr * sink, c[1], rr))

    # The squeeze: a line of the ice's erratics across the corrie's floor,
    # the path through the one gap in it.
    p, tg, nm = frame(r, k_edge - 9)
    stone(p + nm * (1.72 + 1.35), 1.35, 0.3)
    stone(p - nm * (1.72 + 1.1) + tg * 1.0, 1.1, 0.3)
    for off, along, rr in ((6.3, -0.6, 0.95), (8.9, 0.9, 1.2), (11.6, -0.3, 0.8), (-5.5, 0.2, 0.9), (-8.0, 1.4, 1.25), (-10.9, 0.4, 0.75)):
        stone(p + nm * off + tg * along, rr, 0.32)
    # The brae: stones on the bank above that keep a truck out on the lean.
    for k in range(k_lean0 + 3, k_lean1 - 2, 7):
        p, tg, nm = frame(r, k)
        rr = rng.uniform(0.55, 0.95)
        stone(p + nm * (float(r["halfs"][k]) + rr * 0.6), rr, 0.3)
    # Blocks off the crag in the back of the ledge's bay, and one on the
    # rake's inside edge at the bad step, to be passed on the outside.
    p, tg, nm = frame(r, k_ledge)
    for off, rr in ((-2.5, 0.9), (1.5, 0.6), (4.0, 1.2)):
        stone(p - nm * (float(r["halfs"][k_ledge]) + rr * 0.5) + tg * off, rr, 0.35)
    p, tg, nm = frame(r, k_bad + 5)
    stone(p - nm * (float(r["halfs"][k_bad + 5]) + 1.0 * 0.6), 1.0, 0.35)
    # Erratics the ice left out on the moss.
    for (ex, ez, rr) in ((226.0, -4.0, 1.7), (200.0, 90.0, 1.3), (234.0, 152.0, 2.0), (190.0, 172.0, 1.4), (228.0, 76.0, 1.1)):
        if line.query((ex, ez))[0] > RAKE_HALF + rr + 4.0:
            stone(np.array([ex, ez]), rr, 0.3)
    # The stones of the top: pairs of them either side of a line that
    # weaves, as the frost left them.
    for k in range(k_st0 + 3, k_st1 - 1, 6):
        p, tg, nm = frame(r, k)
        for side in (-1, 1):
            rr = rng.uniform(0.6, 1.15)
            stone(p + nm * side * (1.95 + rr + rng.uniform(0.0, 0.3)) + tg * rng.uniform(-1.0, 1.0), rr)
            # (and the field of them it is the way through)
            rr = rng.uniform(0.5, 1.3)
            stone(p + nm * side * rng.uniform(5.4, 10.5) + tg * rng.uniform(-2.5, 2.5), rr)
    # The top bed's two ledges, each across its level tread and no wider
    # (the hill falls away to the side of it, and a ledge standing out
    # over open ground is a plank), a boulder bedded at either end.
    for n_, (k, y_top) in enumerate(x["ledges"]):
        p, tg, nm = frame(r, k)
        c = p + tg * 1.7
        half = RAKE_HALF + 0.5
        boxes.append((c[0], y_top - 1.0, c[1], half, 1.0, 1.5, math.atan2(tg[0], tg[1]) + rng.uniform(-0.1, 0.1)))
        for side, rr, along in ((-1, 1.25, 0.9), (1, 1.0, 1.9)) if n_ == 0 else ((-1, 1.05, 2.0), (1, 1.3, 1.1)):
            q = p + nm * side * (half + rr * 0.55) + tg * along
            spheres.append((q[0], y_top - rr * 0.3, q[1], rr))

    # Its rating at the gate (two black diamonds) and a fingerpost; a post
    # where it leaves the road by the quarry.
    p, tg, nm = frame(r, 12)
    q = p + nm * (RAKE_HALF + 2.4)
    props.append((q[0], q[1], math.atan2(-tg[0], -tg[1]), PROP["PLATE"], None, 3))
    posts.append((q[0], gy(*q) - 0.5, q[1], q[0], gy(*q) + 1.5, q[1], 0.12))
    q = p - nm * (RAKE_HALF + 3.0)
    props.append((q[0], q[1], math.atan2(tg[0], tg[1]) + 0.3, PROP["SIGN"], None))
    p, tg, nm = frame(r, k_q1 + 3)
    q = p + nm * (RAKE_HALF + 1.6)
    props.append((q[0], q[1], math.atan2(-tg[0], -tg[1]), PROP["PLATE"], None, 3))
    posts.append((q[0], gy(*q) - 0.5, q[1], q[0], gy(*q) + 1.5, q[1], 0.12))
    # Carn Dubh: the cairn on the shoulder, where the loch is all below.
    p, tg, nm = frame(r, k_carn)
    c = p + nm * (float(r["halfs"][k_carn]) + 4.0)
    props.append((c[0], c[1], 0.0, PROP["CAIRN"], None))
    views.append((c[0] - nm[0] * 3.0, c[1] - nm[1] * 3.0, "CARN DUBH"))
    # The causeway's stones: the tops of them break the water along both
    # its edges, and are all there is to steer by (from a truck's seat the
    # water is the sky's colour, shallow or deep).
    level = x["lochan_y"]
    for i, k in enumerate(range(k_ford0 + 2, k_ford1 - 1, 3)):
        p, tg, nm = frame(r, k)
        c = p + nm * (1 if i % 2 else -1) * 3.8
        if water[tuple(np.round(to_grid(c)[::-1]).astype(int))] > -999 and gy(*c) < level - 0.2:
            rr = rng.uniform(0.38, 0.58)
            spheres.append((c[0], level + rng.uniform(0.14, 0.28) - rr, c[1], rr))
    # A cairn at each end of the causeway, on the side the line bends to.
    for k, side in ((k_ford0 - 4, -1), (k_ford1 + 4, 1)):
        p, tg, nm = frame(r, k)
        c = p + nm * side * (RAKE_HALF + 2.6)
        props.append((c[0], c[1], 0.0, PROP["CAIRN"], None))
    # A rowan by the quarry and one out of the crag at the ledge; a pine
    # the wind has had its way with by the cairn.
    p, tg, nm = frame(r, k_ledge)
    q = p - nm * (float(r["halfs"][k_ledge]) + 1.1) - tg * 5.5
    trees.append((q[0], 0.0, q[1], 2, 0.8, rng.uniform(0, 2 * math.pi)))
    trees.append((137.0, 0.0, 96.5, 2, 0.95, rng.uniform(0, 2 * math.pi)))
    p, tg, nm = frame(r, k_carn)
    q = p + nm * (float(r["halfs"][k_carn]) + 7.5) + tg * 5.0
    trees.append((q[0], 0.0, q[1], 0, 0.85, rng.uniform(0, 2 * math.pi)))

    # What the CPU's driver knows of it: easy between the erratics and on
    # the lean, slow on the ledge, a run at the peat, the causeway at a
    # walk (the line is all there is to stand on), a run at each ledge.
    pace = r["pace"]
    pace[:] = 3.4
    pace[k_edge - 14:k_edge - 2] = 2.6
    pace[k_lean0 - 2:k_lean1 + 2] = 2.6
    pace[x["k_road_x"] - 8:k_rake0 + 2] = 2.4
    pace[k_rake0 + 2:k_top] = 3.2
    pace[k_ledge - 6:k_ledge + 6] = 2.6
    pace[k_top:k_carn + 10] = 2.8
    pace[k_carn + 10:k_hag0 - 8] = 4.0
    pace[k_hag0 - 8:k_hag1] = 5.0
    pace[k_hag1:k_ford0 - 8] = 3.4
    pace[k_ford0 - 8:k_ford1 + 4] = 2.4
    pace[k_ford1 + 4:k_st0] = 3.4
    pace[k_st0:k_st1] = 2.6
    pace[k_st1:] = 3.4
    pace[n - 8:] = 2.4
    r["spots"] = [(5, "THE BLACK RAKE", 1), (k_edge + 4, "THE BRAE", 0), (k_q0 + 2, "THE QUARRY", 1),
                  (k_bad - 8, "THE BAD STEP", 0), (k_ledge, "EAGLE'S LEDGE", 1), (k_carn, "CARN DUBH", 1),
                  (k_hag0 - 2, "THE HAGS", 0), (k_ford0 - 10, "THE CAUSEWAY", 1), (k_slab0, "THE SLABS", 0),
                  (k_st0 - 3, "THE STONES", 1)]
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    grade = np.diff(y) / np.maximum(seg, 1e-3)
    print(f"  THE BLACK RAKE {seg.sum():.0f} m from the staircase at ({pts[0][0]:.0f},{pts[0][1]:.0f}) to the summit, climbs "
          f"{y[-1] - y[0]:.1f} m, steepest {grade.max() * 100:.0f}%; the brae {x['grades'][0] * 100:.0f}%, the rake "
          f"{x['grades'][1] * 100:.0f}% for {k_top - k_rake0} m from ({pts[k_rake0][0]:.0f},{pts[k_rake0][1]:.0f}) to "
          f"({pts[k_top][0]:.0f},{pts[k_top][1]:.0f}); the lochan at {x['lochan_y']:.1f} m, its causeway {CAUSEWAY_DEEP:.2f} m under")
    full = xt_full(r, before=[(stair, 0, x["k_in"])], after=[(summit, x["j1"] + 1, len(summit["pts"]))])
    return full, (154.0, 12.0, 30.0, 30.0, 3.0, 1)


# ---------------------------------------------------------------------------
# The land beyond the map (outland.py): what the pass looks out on.
#
# The loch carries on: the map has only its north shore, and the far one
# is three hundred metres out, hills behind it. West it bends away
# south-west between them; east it narrows to its head. North of the pass
# the glen runs down to the sea, a sound with islands in it.

# The loch's shores outside the map, west to east (the map's own shore
# lies between the two halves of the north one).
O_SHORE_NW = [(-1160, -660), (-900, -480), (-640, -356), (-480, -318), (-384, -314)]
O_SHORE_NE = [(384, -326), (480, -330), (700, -366), (950, -452), (1070, -500)]
O_SHORE_S = [(1070, -500), (950, -570), (700, -628), (384, -662), (0, -672), (-384, -680), (-640, -690), (-900, -760),
             (-1160, -900)]
# Its water, as lanes across it at stations along x: the north and south
# edges of what they cover (more than the loch: the land hides the rest).
# South of the map they start 30 m inside it: the map's far water is on a
# 6 m and a 12 m grid that stop short of its edge, and the bed showed between.
# On the map's own edge the north one is the edge exactly.
O_LOCH_X = [-1152, -900, -640, -480, -384.01, -384, 384, 384.01, 480, 700, 950, 1080]
O_LOCH_N = [-640, -450, -330, -296, -296, -354, -354, -306, -306, -340, -430, -490]
O_LOCH_S = [-920, -790, -716, -706, -704, -704, -690, -690, -688, -652, -596, -510]
# The glen north of the pass, on down to the sea.
O_NGLEN = [(-30, 384), (-52, 470), (-70, 600), (-40, 740), (-10, 860)]
SEA_Y = 0.6
# The upland: heights the hills beyond pass through.
O_TOPS = [
    (-1150, -1150, 150), (-700, -980, 215), (-250, -1040, 250), (220, -980, 200), (660, -1040, 235), (1150, -1100, 170),
    (-980, -800, 130), (-400, -800, 150), (40, -790, 120), (480, -790, 140), (980, -760, 150),
    (-560, -250, 136), (-760, -160, 180), (-1040, -60, 215), (-640, 210, 185), (-1020, 420, 160), (-520, 480, 140),
    (540, -240, 124), (740, -120, 170), (1030, 40, 205), (620, 230, 180), (1020, 400, 160), (520, 500, 150),
    (-460, 720, 96), (-180, 640, 104), (160, 640, 110), (460, 720, 100), (-900, 760, 80), (900, 760, 90),
    (-1150, 1150, 30), (0, 1150, 20), (1150, 1150, 30), (-600, 1000, 30), (600, 1000, 30), (1150, -400, 160), (-1150, 200, 190),
    (1150, 300, 180), (-1150, -500, 170),
]
# Hills that stand out of the rest, for the skyline: x, z, radius, height added.
O_PEAKS = [(-470, -990, 190, 120), (120, -1060, 170, 90), (560, -960, 180, 110), (-900, -40, 200, 90), (940, 120, 190, 100),
           (-760, 520, 170, 70), (700, 520, 170, 80)]
# Islands out in the sound: x, z, radius, height.
O_ISLES = [(-380, 1010, 170, 70), (260, 1060, 210, 96), (760, 980, 130, 54), (-860, 1060, 150, 60)]


def o_sea_edge(x):
    """The sound's near shore: the z it lies at, by x."""
    return 850.0 + 46.0 * np.sin(x / 250.0) + 30.0 * np.sin(x / 97.0 + 1.3)


def o_poly_dist(poly):
    """Signed distance from the ring's vertices to a closed outline: positive inside."""
    p = np.asarray(poly, dtype=float)
    pts = resample(np.vstack([p, p[:1]]), 3.0)
    d = cKDTree(pts).query(ol.OFLAT)[0].reshape(ol.ON, ol.ON)
    m = np.zeros((ol.ON, ol.ON), dtype=bool)
    for a, b in zip(p, np.roll(p, -1, axis=0)):
        if a[1] == b[1]:
            continue
        m ^= ((a[1] > ol.OZ) != (b[1] > ol.OZ)) & (ol.OX < (b[0] - a[0]) * (ol.OZ - a[1]) / (b[1] - a[1]) + a[0])
    return np.where(m, d, -d)


def lanes(xs, north, south, y, n):
    """A sheet of water as n lanes side by side (a river each to the
    game), a pair of bank points every 24 m of x."""
    st = np.arange(xs[0], xs[-1] + 1.0, ol.CELL)
    st = np.unique(np.concatenate([st, [q for q in xs if abs(abs(q) - 384) < 1]]))
    zn, zs = np.interp(st, xs, north), np.interp(st, xs, south)
    out = []
    for k in range(n):
        a, b = zn + (zs - zn) * k / n, zn + (zs - zn) * (k + 1) / n
        out.append([(float(x), float(za), float(x), float(zb), y) for x, za, zb in zip(st, a, b)])
    return out


def build_outland(h, water, alb_ao):
    """The ring's heights, albedo, normals, haze, horizon map and water,
    joined to the finished map."""
    OX, OZ, OUT = ol.OX, ol.OZ, ol.OUT
    E = ol.extrude(h)
    # The hills.
    tp = np.array(O_TOPS, dtype=float)
    P = RBFInterpolator(tp[:, :2], tp[:, 2], kernel="thin_plate_spline", smoothing=2000.0)(ol.OFLAT).reshape(ol.ON, ol.ON)
    P = np.maximum(P, 12.0)
    for (x, z, r, top) in O_PEAKS:
        P += top * np.exp(-((OX - x) ** 2 + (OZ - z) ** 2) / (r * r)) ** 0.8
    P += 16.0 * (fbm(OX, OZ, 330, 3, 701) - 0.5) * 2 + 9.0 * (ridged(OX, OZ, 150, 3, 702) - 0.5) * 2
    # The loch: its bed, and the shores rising from it to the hills.
    shore = O_SHORE_NW + [(x, z) for x, z in SHORE if -384 < x < 384] + O_SHORE_NE + O_SHORE_S[1:]
    dl = o_poly_dist(shore)
    bed = LOCH_Y - 0.5 - 3.4 * smoothstep(0, 60, dl)
    rise = LOCH_Y + 0.6 + 0.42 * np.maximum(-dl, 0) + 0.0012 * np.maximum(-dl, 0) ** 2
    P = np.where(dl > 0, bed, np.minimum(P, rise))
    # The glen to the sea, and the sea.
    dg, sg, _ = ol.line_dist(O_NGLEN)
    floor = 78.0 - (78.0 - 4.0) * smoothstep(0, 470, sg)
    P = np.minimum(P, floor + 0.5 * np.maximum(dg - 30, 0) + 0.0008 * np.maximum(dg - 30, 0) ** 2)
    ds = OZ - o_sea_edge(OX)                    # metres out into the sound
    P = np.where(ds > 0, SEA_Y - 0.6 - 3.0 * smoothstep(0, 80, ds), np.minimum(P, SEA_Y + 0.8 + 0.3 * np.maximum(-ds, 0)))
    isle = np.zeros_like(P)
    for (x, z, r, top) in O_ISLES:
        q = np.clip(1 - ((OX - x) ** 2 + ((OZ - z) / 0.7) ** 2) / (r * r), 0, 1)
        isle = np.maximum(isle, top * q ** 0.8 * (0.8 + 0.4 * fbm(OX, OZ, 90, 2, 703)))
    P = np.where(isle > 2.5, np.maximum(P, isle), P)
    # Joined to the map: its border's ground carried out, giving way to
    # this over the first 170 m (the loch's bed is the map's already).
    w = smoothstep(0, 170, OUT)
    H = E * (1 - w) + P * w
    H = np.where((dl > 0) & (OUT < 170), np.minimum(H, bed * w + E * (1 - w)), H)
    H[ol.INNER] = E[ol.INNER]
    H[ol.BORDER] = E[ol.BORDER]

    # Colours: the map's own way with them.
    # (only real cliffs: on this grid a hill at thirty degrees would count
    # as a wall's foot, and every hill here is steeper than that)
    face, fx, fz = ol.faces(H, drop=30.0)
    nrm = ol.normals(H)
    nrm = np.where(face[..., None], np.dstack([fx * 0.94, np.full_like(H, 0.34), fz * 0.94]), nrm)
    gz, gx = np.gradient(H, ol.CELL)
    sl = np.hypot(gx, gz)
    m1 = fbm(OX, OZ, 64, 3, 671)
    m2 = fbm(OX, OZ, 23, 3, 672)
    m3 = fbm(OX, OZ, 240, 3, 704)

    def mix(a, b, t):
        return a * (1 - t[..., None]) + np.asarray(b, dtype=float) * t[..., None]
    alb = np.zeros(H.shape + (3,)) + np.array([96.0, 116.0, 56.0])
    alb = mix(alb, (148, 124, 68), smoothstep(0.42, 0.66, m3) * 0.6)
    alb = mix(alb, (100, 92, 82), smoothstep(0.5, 0.66, m1) * 0.6)
    alb = mix(alb, (96, 124, 56), smoothstep(40, 6, H) * 0.6)                  # greener by the water
    alb = mix(alb, (126, 126, 96), smoothstep(120, 190, H) * 0.6)
    bed_c = bed_colour(H)
    alb = mix(alb, bed_c * 0.6 + np.array([120.0, 114.0, 108.0]) * 0.3, smoothstep(0.6, 0.9, sl))
    alb = mix(alb, (132, 128, 122), smoothstep(200, 250, H) * 0.6)             # the tops: stone
    alb = np.where(face[..., None], bed_c * 0.78, alb)
    wet = (dl > 0) | ((ds > 0) & (isle < 2.5))
    alb = np.where(wet[..., None], np.array([90.0, 82.0, 72.0]), alb)
    alb *= (0.92 + 0.16 * m2)[..., None]
    # The map's own colours at its edge, and for 100 m out.
    w2 = smoothstep(0, 110, OUT)[..., None]
    alb = ol.extrude(alb_ao) * (1 - w2) + alb * w2
    hz = ol.horizon_map(H)
    fade = smoothstep(600, 768, OUT)
    xs = np.arange(-1152.0, 1152.1, ol.CELL)
    ribbons = lanes(O_LOCH_X, O_LOCH_N, O_LOCH_S, LOCH_Y, 3)
    ribbons += lanes([-1152.0, 1152.0], [1152.0, 1152.0], [float(o_sea_edge(xs).min()) - 30.0] * 2, SEA_Y, 2)
    return {"h": H, "alb": alb, "nrm": nrm, "fade": fade, "hz": hz, "ribbons": ribbons}


def route_fit(natural, name, ctrl, miss=4.0):
    """The control points of a route drawn more than miss metres off the
    ground as it lay before any route was cut: where its bench will be a
    cutting or a bank that tall. (A hairpin's own turn always is, a
    little.)"""
    off = [(x, z, y, float(sample(natural, np.array([x, z], dtype=float))[0])) for x, z, y in ctrl if y is not None]
    off = [q for q in off if abs(q[3] - q[2]) > miss]
    if off:
        print(f"  {name}: drawn off the ground at " + ", ".join(f"({x:.0f},{z:.0f}) {y:.1f} for {g:.1f}" for x, z, y, g in off))


def build(out, preview_path=None, raw=None):
    print("Loch Lowland")
    rng = np.random.default_rng(1746)
    h, water, f, routes = build_ground()
    natural = h.copy()
    finish_routes(h, routes, f)
    finish_water(h, water, f, routes)
    peat = apply_mud(h, PEAT)
    water[water < h + 0.05] = -1000.0
    f["face"], f["face_x"], f["face_z"] = faces(h)
    surf = surfaces(h, water, f, routes, peat)
    trees = place_trees(h, water, surf, f, routes, rng)

    def gy(x, z):
        return float(sample(h, np.array([x, z]))[0])
    sp = set_pieces(h, water, f, routes, trees, np.random.default_rng(77))
    # The extreme trail is laid over the finished hill, so that the hill
    # is as it was everywhere else: its trees and its stones are dealt
    # from one seeded pack each, and a face or a surface changed before
    # they were would move every one of them.
    h0, face0 = h.copy(), f["face"].copy()
    xr = black_rake(h, water, f, routes)
    hags = apply_mud(h, xr["peat"])
    by_lochan = np.zeros((N, N), dtype=bool)
    for (lx, lz, lr, _) in [LOCHAN_RAKE] + POOLS_RAKE:
        by_lochan |= np.hypot(X - lx, Z - lz) < lr * 1.6
    water[by_lochan & (water < h + 0.05)] = -1000.0
    rim_check(h, water)
    f["face"], f["face_x"], f["face_z"] = faces(h)
    # The rake's ledge is ground a truck stands on: lit and coloured as
    # ground, not as the wall it crosses. What it has cut is rock.
    f["face"] &= xr["bed"] > RAKE_HALF + 0.3
    surf[f["face"] & ~face0] = S["ROCK"]
    surf[hags] = S["MUD"]
    black_rake_surfaces(h, surf, f, xr)
    extreme, eagle = black_rake_pieces(h, h0, water, f, xr, sp, trees, routes, np.random.default_rng(1689))
    rocks, spheres, capsules, posts = sp["rocks"], sp["spheres"], sp["capsules"], sp["posts"]
    boxes = sp["boxes"] + sp["prop_boxes"]
    rock_boxes = len(sp["boxes"])
    # Nothing grows through a rock or a prop.
    keep = cKDTree(np.array([(s_[0], s_[2]) for s_ in spheres] + [(b[0], b[2]) for b in boxes] + [(q[0], q[2]) for q in posts] +
                            [(p[0], p[1]) for p in sp["props"]] + [((c[0] + c[3]) / 2, (c[2] + c[5]) / 2) for c in capsules]))
    trees = [(t[0], gy(t[0], t[2]), t[2], t[3], t[4], t[5]) for t in trees if keep.query((t[0], t[2]))[0] > 4.5]
    argb, rgb, alb_ao, nrm = colours(h, water, surf, f, routes)
    props = [(p[0], gy(p[0], p[1]) if p[4] is None else p[4], p[1], p[2], p[3], p[5] if len(p) > 5 else 0) for p in sp["props"]]
    views = [(v[0], gy(v[0], v[1]), v[1], v[2]) for v in sp["views"]]
    # A cairn by some of the things to find, 5 m off: to the north-east
    # unless that is within 4.5 m of a route's line (they stop a truck).
    lines = cKDTree(np.vstack([r["pts"] for r in routes]))

    def cairn_by(x, z):
        for ox, oz in ((4.0, 3.0), (-4.0, 3.0), (4.0, -3.0), (-4.0, -3.0), (0.0, 7.0), (0.0, -7.0), (7.0, 0.0), (-7.0, 0.0)):
            if lines.query((x + ox, z + oz))[0] > 4.5:
                return x + ox, z + oz
        return x + 4.0, z + 3.0
    for v in views:
        if v[3] in ("BLACK MOSS", "THE HUMMOCKS", "LOCHAN DUBH", "GREY MARE'S TAIL", "THE HAIRPINS", "THE CRAG", "THE BEALACH"):
            cx, cz = cairn_by(v[0], v[2])
            props.append((cx, gy(cx, cz), cz, 0.0, PROP["CAIRN"], 0))
    boxes += cairn_boxes(props)
    summit_y = gy(SUMMIT[0], SUMMIT[1])
    spawns = []
    for k in range(4):
        sx = SPAWN[0] + (k - 1.5) * 6.0
        sz = SPAWN[1] - abs(k - 1.5) * 2.0
        spawns.append((sx, gy(sx, sz), sz, 0.0))

    out.mkdir(parents=True, exist_ok=True)
    # The game's coarser grids take a vertex's colour from the nearest
    # flagged face, which keeps a great crag a wall from far off; flag the
    # small ones too and a hillside with a band of crag across it turns
    # to rock from top to bottom.
    great = f["face"] & (maximum_filter(h, 5) - minimum_filter(h, 5) > 21.0)
    light = light_inputs(alb_ao, nrm, h, water, flags=great.astype(np.uint8))
    hz = horizon_map(h)
    blob = pack_level(h, water, surf, light, hz)
    (out / "level_lowland.lvl").write_bytes(blob)
    o = build_outland(h, water, alb_ao)
    (out / "level_lowland.out").write_bytes(ol.pack(o["h"], o["alb"], o["nrm"], o["fade"], o["hz"], o["ribbons"]))
    if raw:
        raw.write_bytes(raw_level(h, argb, water, surf, light, hz))
    ly = f["lochan_y"]
    write_header(out / "level_lowland.h", "lowland", {
        "name": "LOCH LOWLAND", "blurb": "A HIGHLAND PASS OVER THE LOCH", "base_ft": 1687,
        "lake_y": LOCH_Y, "start_y": gy(SPAWN[0], SPAWN[1]), "summit": (SUMMIT[0], summit_y, SUMMIT[1]),
        "routes": routes + [extreme], "spheres": spheres, "capsules": capsules, "boxes": boxes, "rock_boxes": rock_boxes,
        "posts": posts, "trees": trees, "props": props, "views": views, "spawns": spawns,
        # Mist: along the loch's shore, on each lochan, over the bog.
        # (the last three lie out on the loch beyond the map's edge)
        "mists": [(-210, -352, 66), (-70, -350, 66), (70, -350, 66), (210, -348, 66), (LOCHAN_DUBH[0], LOCHAN_DUBH[1], 42),
                  (LOCHAN_CORRIE[0], LOCHAN_CORRIE[1], 36), (LOCHAN_PASS[0], LOCHAN_PASS[1], 38), (PEAT[0][0], PEAT[0][1], 34),
                  (LOCHANS_MOSS[0][0], LOCHANS_MOSS[0][1], 30), (-190, -500, 120), (40, -520, 120), (270, -490, 120)],
        # Eagles over the corrie, the loch's shore, the moss and the top.
        # (and one off the great crag, level with the rake's ledge)
        "hawks": [(6, -40, 50, 70, 0.0, 1), (40, -280, 44, 50, 2.0, -1), (-190, 90, 40, 44, 4.0, 1), (150, 230, 44, 40, 1.0, -1),
                  eagle],
        # Red deer among the hummocks and up the corrie; then (deer.c) the
        # cattle on the in-bye by the fank, the sheep at the corrie's mouth.
        "herds": [(170, -246), (-20, 22), (-62, -262), (48, -226)],
        "rocks": rocks,
    })
    # The map shows the water as a map does.
    mrgb = np.where((water > -999)[..., None], np.array([92.0, 124.0, 148.0]), rgb * 1.08)
    minimap(mrgb, water, routes + [extreme], [], out / "level_map_lowland.png")
    wet = int((water > -999).sum())
    print(f"  heights {h.min():.1f}..{h.max():.1f} m, pass {gy(MEET[0], MEET[1]):.1f} m, summit {summit_y:.1f} m, "
          f"start {gy(SPAWN[0], SPAWN[1]):.1f} m, {wet} wet vertices")
    report(h, routes)
    for name, ctrl in (("DROVE ROAD", R_DROVE), ("DEVIL'S STAIRCASE", R_STAIR), ("PEAT ROAD", R_PEAT)):
        route_fit(natural, name, ctrl)
    print(f"  {len(spheres)} boulders, {len(boxes)} boxes ({rock_boxes} of rock), {len(trees)} trees, {len(props)} props, "
          f"{len(rocks)} formations, {len(views)} viewpoints, level_lowland.lvl {len(blob)} bytes")
    if preview_path:
        preview_image(rgb, water, routes + [extreme], spheres, capsules, boxes, trees, views, preview_path)
        az, el = math.degrees(math.atan2(SUN[0], SUN[2])) % 360, math.degrees(math.asin(SUN[1]))
        orgb = ol.lit(o["alb"], o["nrm"], o["hz"], az, el, (0.62, 0.66, 0.72), (0.62, 0.577, 0.496))
        land = [ol.mesh(o["h"], orgb)] + [ol.ribbon_mesh(r, WATER_RGB) for r in o["ribbons"]]
        preview(h, rgb, water, preview_path.with_name("level-lowland-views.png"), land=land, rocks=rocks, spheres=spheres, boxes=boxes,
                trees=trees, tree_rgb=((52, 84, 62), (206, 168, 60), (190, 84, 44)), rock_rgb=(140, 122, 116))


if __name__ == "__main__":
    import sys
    build(Path(sys.argv[1]), Path(sys.argv[2]) / "level-lowland.png" if len(sys.argv) > 2 else None)
