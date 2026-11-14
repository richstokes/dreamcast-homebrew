#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "scipy>=1.11", "pillow>=10.1", "lz4>=4.3"]
# ///
"""Build Ridgeline's levels. This file is Cascade Ridge, a Pacific Northwest
mountain; --area moab builds Mesa Grande (level_moab.py), --area lowland
Loch Lowland (level_lowland.py), all the lot. They share levelkit.py.

A meadow to play in at the south end, a trailhead, and three routes up the
mountain that meet at an alpine lake before the last climb to the summit:

  logging road   gravel then dirt, gentle grades, long switchbacks (east)
  ridge line     steep rock, ledges and boulders (centre)
  creek bed      up the creek's valley: water, mud, logs, wet rock (west)

Routes are found by A* over the heightfield with a grade limit per route,
so switchbacks come out of the terrain, then carved into it. Obstacles
grow with the distance along each route. Everything is seeded, so the
output is reproducible.

World: +X east, +Y up, +Z north, metres. Writes to --out, per area:

    level_<area>.lvl      the grid, packed (levelkit.pack_level): heights,
                          the water's surface, surface ids, and what the
                          game needs to relight the ground as the day goes
                          by: per vertex its albedo (with the ambient
                          occlusion folded in), normal and how deep under
                          water it lies, and a horizon map (the skyline's
                          elevation in HZ_DIRS directions from every other
                          vertex) for hill shadows
    level_<area>.h        routes, obstacles, trees, rock formations, props,
                          spawn points and viewpoints (src/level.c
                          includes it; the types are src/level.h's)
    level_map_<area>.png  the minimap's picture (packed by
                          build_textures.py)

and a preview picture of each to the --preview folder.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np
from scipy.ndimage import gaussian_filter
from scipy.spatial import cKDTree

sys.path.insert(0, str(Path(__file__).resolve().parent))
from levelkit import *  # noqa: E402,F401,F403
from levelkit import _find_spot  # noqa: E402,F401

PEAK = np.array([30.0, 140.0])
TRAILHEAD = np.array([8.0, -204.0])
SPAWN = np.array([0.0, -278.0])

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


ROUTE_DEFS = [
    # name, waypoints after the fork, grade limit, half width
    ("LOGGING ROAD", [(100, -100), (150, -30), (110, 30)], 0.15, 3.4),
    ("RIDGE LINE", [(14, -78), (8, -30)], 0.40, 2.6),
    # The last waypoint sits on the shelf the route reaches the falls' pool
    # along, heading for the lake. At the foot of the falls (-70, 36) the
    # route had to turn back on itself to leave, and the two legs were
    # carved into a knot with a 3 m step in it.
    ("CREEK BED", [(-80, -118), (-116, -50), (-100, 0), (-78, 48)], 0.26, 3.0),
]
# A route listed here ends on the logging road at its point nearest this
# spot instead of at the high meadow, and follows it from there. The ridge
# line used to finish beside it, 2.5-7 m away and up to 10 m lower, and
# the two couldn't both be carved.
JOINS = {"RIDGE LINE": (95.0, 36.0)}
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
        end = HIGH_MEADOW
        if name in JOINS:
            road = np.vstack(next(sg[1] for sg in segs if sg[0] == "LOGGING ROAD"))
            end = road[int(np.argmin(np.linalg.norm(road - np.array(JOINS[name]), axis=1)))]
        pts = [FORK] + [np.array(w, dtype=float) for w in wps] + [end]
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
        r = {"name": name, "pts": pts, "gmax": gmax, "half": half}
        if name in JOINS:
            road = next(q for q in routes if q["name"] == "LOGGING ROAD")
            js = int(np.argmin(np.linalg.norm(road["pts"] - pts[-1], axis=1)))
            r["own"], r["join"] = len(pts) - 1, js
            r["pts"] = np.vstack([pts[:-1], road["pts"][js:]])
        routes.append(r)
    return routes


def place_obstacles(h, water, routes, rng):
    spheres, capsules, boxes, mud = [], [], [], []
    on_trail = set()
    # name -> list of (kind, spacing)
    for r in routes:
        pts, y, half = r["pts"], r["y"], r["half"]
        n = own(r)
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
                        off = off_line(r, k, off, half)
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
                        off = off_line(r, k, rng.uniform(-half - 1.5, half + 1.5), half)
                        c = p + sd * off + tg * rng.uniform(-3, 3)
                        rr = 0.3 + 0.55 * diff * rng.random() + 0.15 * rng.random()
                        if abs(off) < half:
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
                        c = p + sd * off_line(r, k, rng.uniform(-half - 1.0, half + 1.0), half) + tg * rng.uniform(-2, 2)
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


def settle_routes(h, water, routes):
    """Each route's driving band back to its own heights where another
    route's carving moved it more than 0.4 m (the ridge line, carved after
    the logging road and running beside it 6 m lower, once cut a 4 m hole
    in it; the creek bed's crossings, raised to keep the creek wadeable,
    stood 2 m proud of it). Later routes win where bands overlap: the
    logging road, the easy way up, goes last. Near the top the ridge line
    runs 2.5-7 m beside it and lower, so it gets two short rock steps
    there. Mud ruts (under 0.4 m) stay."""
    order = ["RIDGE LINE", "CREEK BED", "SUMMIT TRAIL", "FOREST ROAD", "LOGGING ROAD"]
    flat = np.stack([X.ravel(), Z.ravel()], axis=1)
    for name in order:
        r = next(q for q in routes if q["name"] == name)
        o = own(r)
        d, idx = cKDTree(r["pts"][:o]).query(flat, distance_upper_bound=float(np.max(r["halfs"][:o])))
        ok = np.isfinite(d)
        ok[ok] = d[ok] < r["halfs"][idx[ok]]
        jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
        y = r["y"][idx[ok]]
        fix = np.abs(h[jj, ii] - y) > 0.4
        h[jj[fix], ii[fix]] = y[fix]
        # Through water the route is a ford: no deeper than a wade.
        wet = fix & (water[jj, ii] > -999)
        water[jj[wet], ii[wet]] = np.minimum(water[jj[wet], ii[wet]], y[wet] + 0.45)


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
# The extreme trail

def widowmaker(h, water, surf, routes, trees, spheres, capsules, boxes, prop_boxes, rng):
    """WIDOWMAKER: the old spur that carries on where the logging road
    turns back at its far hairpin. Across the meadow into a tongue of big
    timber the loggers never reached (trunks to thread between, windfalls
    to climb over), up a stair of bedrock ledges, along the lip of a draw
    with the ground leaning away into it, through the bog in the bottom
    and up the bank beyond, through a rock garden, and then on a long
    slant straight up the bare rock of the mountain's north face through
    an old burn, across the summit trail and up the last of the rock to
    the lookout.

    Steps (ledges, windfalls) come on level ground with a run at them: a
    tyre climbs one on momentum, not from a standstill on a slope. The
    steep pitches are bare, and ask for grip.

    Returns the route for the header (levelkit.xt_full), props as (x, z,
    yaw, kind, y or None, var) and a mist."""
    log, summit = routes[1], routes[4]
    k_in = xt_k(log, 292.0, 152.0)
    pa, ya = log["pts"][k_in], float(log["y"][k_in])
    ctrl = [
        (pa[0], pa[1], None),
        # The old spur: over the meadow, into the timber.
        (283, 166, None), (268, 181, None), (250, 195, None), (232, 208, None), (214, 217, None), (198, 222, None),
        # The draw and its bog.
        (186, 225, None), (176, 226, None), (166, 225, None),
        # The bank.
        (154, 222, None), (140, 222, None), (126, 224, None),
        # The garden, the line weaving through it as it climbs.
        (116, 224, None), (107, 228, None), (98, 225, None), (89, 229, None), (79, 227, None), (70, 230, None),
        (61, 228, None),
        # The landing at the foot of the hill, and the hill: a slant up
        # the north face.
        (54, 227, None), (48, 225, None), (39, 222, None), (25, 216, None), (11, 206, None), (1, 194, None),
        (-5, 182, None), (-6, 171, None), (-5.5, 160, None),
        # Over the summit trail (between a ledge of its own and a boulder)
        # and up the crown to the lookout.
        (-2, 154, None), (6, 149.5, None), (16, 145, None), (PEAK[0] - 5.0, PEAK[1] + 1.5, None),
    ]
    r = xt_route(h, ctrl, "WIDOWMAKER", "WIDOW", 2.5)
    pts = r["pts"]
    n = len(pts)

    def at(x, z):
        return xt_k(r, x, z)

    def gnd(k):
        return float(np.mean(sample(h, pts[max(0, k - 2):k + 3])))
    k_tim = at(262, 186)                                   # where the timber begins
    k_bog0, k_bog, k_bog1 = at(187, 225), at(177, 226), at(167, 225)
    k_lad0, k_lad1 = at(160, 224), at(128, 224)
    k_gar0, k_gar1 = at(122, 224), at(60, 228)
    k_land = at(51, 226)
    k_hill0 = at(43, 222)
    # Where it crosses the summit trail, at the trail's own height.
    k_cross = int(np.argmin(cKDTree(summit["pts"]).query(pts[at(-6, 172):])[0])) + at(-6, 172)
    k_sum = int(cKDTree(summit["pts"]).query(pts[k_cross])[1])
    y_cross = float(summit["y"][k_sum])
    # Across the summit trail's bed it is the summit trail's ground, which
    # climbs along its own line: this trail takes those heights from one
    # edge of it to the other, so neither leaves a step in the other.
    BED = 6
    bed_y = [(k, float(summit["y"][int(cKDTree(summit["pts"]).query(pts[k])[1])])) for k in range(k_cross - BED, k_cross + BED + 1, 2)]
    # Heights: the ground's own over the meadow; in the timber the stairs,
    # three flights of a level tread, a ledge across it and a pitch; the
    # bog level in the bottom of its draw; the bank and the hill each one
    # pitch.
    y_bog = gnd(k_bog) - 1.2
    y_land = gnd(k_land) + 1.0
    y_top = float(sample(h, PEAK)[0])
    # The hill: one pitch of bare rock from the landing to the summit
    # trail. (A seep across it, wet rock too slick to hold a truck on that
    # grade, stopped every CPU driver that met it at less than a run.)
    hill_grade = (bed_y[0][1] - (y_land + 1.0)) / (k_cross - BED - k_hill0)
    marks = [(0, ya)] + [(k, gnd(k)) for k in range(14, k_tim - 6, 14)]
    TREAD, BLOCK, PITCH = 6, 3, 5
    k_st0 = k_tim + 2
    k_st1 = k_st0 + 3 * (TREAD + BLOCK + PITCH)
    k_side0, k_side1 = k_st1 + 3, k_bog0 - 9
    assert k_side1 - k_side0 > 12, "no room for the lean between the stairs and the draw"
    y0, y1 = gnd(k_st0), gnd(k_st1)
    steps = [0.2, 0.24, 0.28]
    rise = (y1 - y0 - sum(steps)) / 3.0                    # what each pitch climbs
    ledges = []
    yy, k = y0, k_st0
    for sp in steps:
        marks += [(k, yy), (k + TREAD, yy), (k + TREAD + 1, yy + sp), (k + TREAD + BLOCK, yy + sp)]
        ledges.append((k + TREAD, yy + sp))
        yy += sp + rise
        k += TREAD + BLOCK + PITCH
    marks += [(k_st1, y1), (k_side1, y_bog + 1.6), (k_bog0, y_bog), (k_bog1, y_bog + 0.1), (k_lad0, y_bog + 1.0),
              (k_lad1, 63.0), (k_gar0, 63.8), (k_gar1, y_land - 1.6), (k_land - 3, y_land - 0.2), (k_land + 3, y_land),
              (k_hill0, y_land + 1.0)] + bed_y + [(n - 5, y_top - 0.3), (n - 1, y_top)]
    xt_ramp(r, marks, 1.0)
    y = r["y"]
    # Wider where a truck lines up for the hill; the rest is a truck and a half.
    r["halfs"] = r["halfs"] + 1.6 * np.exp(-((np.arange(n) - k_land) / 7.0) ** 2)
    carve_route(h, pts[6:n - 5], y[6:n - 5], r["halfs"][6:n - 5])
    # The roads it leaves and crosses keep their own ground.
    xt_settle(h, log, max(0, k_in - 40), k_in + 40)
    xt_settle(h, summit, max(0, k_sum - 30), min(len(summit["pts"]), k_sum + 30))
    xt_settle(h, summit, len(summit["pts"]) - 30, len(summit["pts"]))

    flat = np.stack([X.ravel(), Z.ravel()], axis=1)
    others = np.vstack([q["pts"] for q in routes])
    off_roads = smoothstep(7.0, 13.0, cKDTree(others).query(flat)[0].reshape(N, N))

    # The draw: a gully that follows the fall line down across the trail,
    # as water would, a trickle in it, and where the trail crosses, ruts
    # full of water in black mud.
    hs = gaussian_filter(h, 2.5)
    gz, gx = np.gradient(hs, D)

    def run(p, sign, length):
        out = [np.array(p, dtype=float)]
        for _ in range(int(length / 2.0)):
            g = np.array([float(sample(gx, out[-1])[0]), float(sample(gz, out[-1])[0])])
            out.append(out[-1] + sign * g / (np.linalg.norm(g) + 1e-6) * 2.0)
        return out
    cb = pts[k_bog]
    gully = np.array(run(cb, 1.0, 44.0)[::-1] + run(cb, -1.0, 70.0)[1:])
    gd, gs, gp = line_dist(smooth_path(gully, 2))
    taper = smoothstep(0.0, 16.0, gs) * smoothstep(float(gs.max()), float(gs.max()) - 26.0, gs)
    W = 11.0
    prof = np.where(gd < W, np.cos(np.pi * gd / (2 * W)) ** 2, 0.0) * taper * off_roads
    td = cKDTree(pts).query(flat)[0].reshape(N, N)
    away = smoothstep(r["half"] + 0.5, r["half"] + 6.0, td)             # the trail's own bed is already down there
    h -= 2.1 * prof * away
    bed = (gd < 1.5) & (td > r["half"] + 2.0) & (prof > 0.45)
    water[bed] = np.maximum(water[bed], h[bed] + 0.16)
    surf[(gd < 3.2) & (prof > 0.3)] = S["WETROCK"]
    jj, ii, idx, d, lat = xt_band(r, r["half"] + 2.2, k_bog0 + 1, k_bog1 - 1)
    rut = smoothstep(r["half"] + 2.2, r["half"] - 0.6, d)
    h[jj, ii] -= 0.34 * rut
    surf[jj[rut > 0.15], ii[rut > 0.15]] = S["MUD"]
    pool = rut > 0.75
    water[jj[pool], ii[pool]] = np.maximum(water[jj[pool], ii[pool]], h[jj[pool], ii[pool]] + 0.2)

    # Surfaces along it: two ruts in the meadow's grass, forest duff worn
    # to dirt, rock where it is steep.
    xt_surface(surf, r, 6, k_bog0 + 1, S["DIRT"], extra=0.0)
    xt_surface(surf, r, k_st0 + 2, k_st1, S["ROCK"], extra=0.4)
    xt_surface(surf, r, k_bog1 - 1, k_hill0, S["DIRT"])
    xt_surface(surf, r, k_bog1 - 1, k_lad0 + 3, S["MUD"], extra=0.2)      # dragged out of the bog and up the bank's foot
    xt_surface(surf, r, k_hill0, n - 5, S["ROCK"], extra=1.6)

    # The lean: along the draw's lip the bed tilts into it, downhill to
    # the right of the way up.
    xt_camber(h, r, k_side0, k_side1, 0.2, ease=5.0)

    def across(k, L, rr, lift, skew=0.0):
        """A log or a root across the trail at point k."""
        p_, tg_, nm_ = frame(r, k)
        ang = math.atan2(nm_[0], nm_[1]) + skew
        e = np.array([math.sin(ang), math.cos(ang)]) * L / 2
        a, b = p_ - e, p_ + e
        # Lying on the trail's bed from bank to bank (the ground under
        # its ends may be a cut bank a metre higher).
        capsules.append((a[0], float(y[k]) + lift, a[1], b[0], float(y[k]) + lift, b[1], rr))
    # Windfalls on the spur, where there is a run at them.
    for k, rr in ((k_tim - 30, 0.18), (k_tim - 10, 0.22)):
        across(k, 8.0 + rng.uniform(0, 2), rr, rr * 0.1, rng.uniform(-0.3, 0.3))
    # Roots across the spur's level stretches: a judder, no more. (On a
    # pitch a root is a wall to a tyre that has no run at it.)
    for k in list(range(k_tim - 52, k_tim - 34, 5)) + list(range(k_tim - 24, k_tim - 13, 5)):
        rr = rng.uniform(0.04, 0.055)
        across(k, r["half"] * 2 + rng.uniform(0.5, 2.0), rr, rr * 0.2, rng.uniform(-0.4, 0.4))

    # The garden: boulders either side of a line that weaves, more of
    # them out in the trees.
    on_trail = []
    for k in range(k_gar0 + 2, k_gar1, 5):
        p_, tg_, nm_ = frame(r, k)
        for side in (-1, 1):
            rr = rng.uniform(0.7, 1.25)
            q = p_ + nm_ * side * (1.95 + rr + rng.uniform(0.0, 0.35)) + tg_ * rng.uniform(-1.2, 1.2)
            on_trail.append((q[0], float(sample(h, q)[0]) - rr * 0.35, q[1], rr))
        if k % 2:
            rr = rng.uniform(0.5, 1.5)
            q = p_ + nm_ * rng.choice([-1, 1]) * rng.uniform(5.0, 11.0) + tg_ * rng.uniform(-2.5, 2.5)
            on_trail.append((q[0], float(sample(h, q)[0]) - rr * 0.4, q[1], rr))
    # The stairs: a ledge of bedrock across each tread, taller each time,
    # its top the foot of the pitch beyond.
    for (k, y_led) in ledges:
        p_, tg_, nm_ = frame(r, k)
        yaw = math.atan2(tg_[0], tg_[1]) + rng.uniform(-0.12, 0.12)
        c = p_ + tg_ * (BLOCK * 0.5 + 0.2)
        boxes.append((c[0], y_led - 1.0, c[1], r["half"] + 1.6, 1.0, BLOCK * 0.5, yaw))
    # The lean: rocks on the bank above that keep a truck out on it.
    for k in range(k_side0 + 3, k_side1 - 2, 6):
        p_, tg_, nm_ = frame(r, k)
        rr = rng.uniform(0.6, 1.0)
        q = p_ - nm_ * (r["half"] + rr * 0.55)
        on_trail.append((q[0], float(sample(h, q)[0]) - rr * 0.3, q[1], rr))
    # The hill and the crown: boulders along their edges.
    for k in range(k_hill0 + 2, n - 12, 9):
        if abs(k - k_cross) < 9:
            continue
        p_, tg_, nm_ = frame(r, k)
        rr = rng.uniform(0.45, 1.0)
        q = p_ + nm_ * rng.choice([-1, 1]) * (r["half"] + 0.9 + rr + rng.uniform(0, 1.6))
        on_trail.append((q[0], float(sample(h, q)[0]) - rr * 0.4, q[1], rr))
    rtree = cKDTree(others)
    spheres += [q for q in on_trail if rtree.query((q[0], q[2]))[0] > 5.0 + q[3]]

    # Timber. Nothing grows in the trail. From where the spur leaves the
    # meadow the big trees stand close on both sides of it, and three
    # pairs of them leave a gap to thread. The north face burned: snags
    # there, and little else.
    tr = cKDTree(pts)
    keep = []
    for t in trees:
        d_, i_ = tr.query((t[0], t[2]))
        if d_ > (4.4 if i_ < k_gar1 else 13.0):
            keep.append(t)
    trees[:] = keep
    burn = np.array([14.0, 200.0])
    trees[:] = [t for t in trees if np.hypot((t[0] - burn[0]) / 40.0, (t[2] - burn[1]) / 48.0) > 1.0 or rng.random() < 0.15]

    def tree_at(q, kind, scale):
        if rtree.query(q)[0] < 7.0 or water[tuple(np.round(to_grid(q)[::-1]).astype(int))] > -999:
            return
        trees.append((q[0], float(sample(h, q)[0]), q[1], kind, scale, rng.uniform(0, 2 * math.pi)))
    squeeze = [k_tim - 20, k_side0 + 2, at(142, 222)]
    # (The scenery has room for about a hundred more trees than the
    # mountain had, the boot log's "FULL" when it runs out: the big ones
    # go where the trail is, and few behind them.)
    for k in range(k_tim - 44, k_gar1, 7):
        if k_bog0 - 2 < k < k_bog1 + 2 or any(abs(k - q) < 5 for q in squeeze):
            continue
        p_, tg_, nm_ = frame(r, k)
        side = 1 if (k // 7) % 2 else -1
        near = 3.9 if k < k_gar0 else 6.0                  # the garden's boulders have the trail's edge
        tree_at(p_ + nm_ * side * rng.uniform(near, near + 1.3) + tg_ * rng.uniform(-2, 2), 4, rng.uniform(1.25, 1.6))
        if rng.random() < 0.4:
            tree_at(p_ - nm_ * side * rng.uniform(near + 1.6, near + 5.0) + tg_ * rng.uniform(-3, 3), 4,
                    rng.uniform(1.1, 1.5))
    for k in squeeze:
        p_, tg_, nm_ = frame(r, k)
        for side in (-1, 1):
            tree_at(p_ + nm_ * side * 2.12, 4, 1.4)
    for _ in range(30):
        q = burn + rng.normal(0, 1.0, 2) * np.array([20.0, 24.0])
        if tr.query(q)[0] > r["half"] + 2.6:
            tree_at(q, 3, rng.uniform(0.8, 1.5))

    # What the CPU's driver knows about it: a run at each windfall and
    # ledge, steady across the lean, and way on up the hill.
    pace = r["pace"]
    pace[:] = 4.0
    pace[k_st0:k_st1] = 3.4
    pace[k_side0 - 2:k_side1 + 1] = 2.4
    pace[k_side1 + 1:k_lad1 + 4] = 3.6
    pace[k_gar0:k_gar1] = 2.8
    pace[k_land - 5:k_land + 4] = 2.6
    pace[k_hill0:k_cross - 8] = 4.4
    pace[k_cross - 6:k_cross + 5] = 3.4
    pace[k_cross + 5:] = 3.8
    pace[n - 10:] = 2.4
    r["spots"] = [(5, "WIDOWMAKER", 1), (k_st0, "THE STAIRS", 0), (k_side0, "THE LEAN", 0), (k_bog0 - 3, "THE BOG", 1),
                  (k_lad0, "THE BANK", 0), (k_gar0 + 2, "THE GARDEN", 1), (k_land, "LAST CHANCE", 1),
                  (k_hill0 + 3, "THE WIDOW", 0), (k_cross + 3, "THE CROWN", 0)]

    yaw0 = math.atan2(pts[8][0] - pts[0][0], pts[8][1] - pts[0][1])
    p_, tg_, nm_ = frame(r, 11)
    props = [tuple(p_ + nm_ * 3.6) + (yaw0, PROP["PLATE"], None, 3)]
    props.append(tuple(p_ - nm_ * 3.8) + (yaw0, PROP["SIGN"], None, 0))
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    grade = np.diff(y) / np.maximum(seg, 1e-3)
    print(f"  WIDOWMAKER {seg.sum():.0f} m from the logging road's far hairpin ({pa[0]:.0f},{pa[1]:.0f}) to the summit, "
          f"climbs {y[-1] - y.min():.1f} m, steepest {grade.max() * 100:.0f}%, the hill "
          f"{hill_grade * 100:.0f}% for {k_cross - BED - k_hill0} m, "
          f"landing {y_land:.1f}, bog {y_bog:.1f}, crosses the summit trail at ({pts[k_cross][0]:.0f},{pts[k_cross][1]:.0f})")
    full = xt_full(r, before=[(log, 0, k_in)])
    return full, props, (pts[k_bog][0], pts[k_bog][1], 34)


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
        o = own(r)
        tree = cKDTree(r["pts"][:o])
        d, idx = tree.query(flat, distance_upper_bound=float(np.max(r["halfs"][:o])) + 1.2)
        ok = np.isfinite(d)
        ok[ok] = d[ok] < r["halfs"][idx[ok]] + 1.2
        jj, ii = np.unravel_index(np.nonzero(ok)[0], h.shape)
        t = idx[ok] / o
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


def colours(h, water, surf):
    nrm = normals(h)
    lam = np.clip(np.einsum("jik,k->ji", nrm, SUN), 0, 1)
    sh = sun_shadow(h)
    ao = np.clip(1.0 + (h - gaussian_filter(h, 4)) * 0.06, 0.72, 1.08)
    n1 = fbm(X, Z, 9, 3, 101)
    n2 = fbm(X, Z, 40, 3, 102)
    alb = np.zeros((N, N, 3))
    for name, col in ALBEDO.items():
        alb[surf == S[name]] = col
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
    return argb.astype(np.uint32), rgb, alb * ao[..., None], nrm


def build(out, preview=None, raw=None):
    """Cascade Ridge into out: level_cascade.h, level_cascade.lvl and
    level_map_cascade.png."""
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
        if "join" in r:
            road = next(q for q in routes if q["name"] == "LOGGING ROAD")
            o, js = r["own"], r["join"]
            r["y"][o:] = road["y"][js:]
            r["halfs"][o:] = road["halfs"][js:]
            # At the road's height by the edge of its band, so the climb
            # meets the road level rather than as a step at its edge, and
            # eased into that over the 30 m before.
            s_ = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(r["pts"][:o + 1], axis=0), axis=1))])
            back = s_[o] - s_[:o]
            edge = float(road["halfs"][js]) + 1.5
            k_edge = int(np.argmax(back <= edge))
            r["y"][:o] += (road["y"][js] - r["y"][k_edge]) * np.clip(1 - (back - edge) / 30.0, 0, 1)
            r["y"][k_edge:o] = road["y"][js]
    for r in routes:
        o = own(r)
        carve_route(h, r["pts"][:o], r["y"][:o], r["halfs"][:o])
    spheres, capsules, boxes, mud = place_obstacles(h, water, routes, rng)
    mud_mask = apply_mud(h, mud)
    # The creek stays wadeable (a route carved down into its bed would
    # leave a hole deeper than a truck's air intake); the lake and the pond
    # keep their depth.
    still = (np.hypot(X - LAKE_C[0], Z - LAKE_C[1]) < LAKE_R + 4) | (np.hypot(X - POND_C[0], Z - POND_C[1]) < POND_R + 4)
    creek = (water > -999) & ~still
    h[creek] = np.maximum(h[creek], water[creek] - 0.5)
    water[water < h + 0.05] = -1000.0
    settle_routes(h, water, routes)
    water[water < h + 0.05] = -1000.0
    surf = surfaces(h, water, routes, mud_mask)
    trees = place_trees(h, water, routes, surf, rng, spheres, capsules, boxes)
    prop_boxes = []
    extra_props, extra_views, tracks = set_pieces(h, water, surf, routes, trees, spheres, capsules, boxes, prop_boxes,
                                                  np.random.default_rng(77))
    diffs = {"FOREST ROAD": 0, "LOGGING ROAD": 1, "CREEK BED": 2, "RIDGE LINE": 3, "SUMMIT TRAIL": 3}
    for r in routes:
        r["difficulty"] = diffs[r["name"]]
    extreme, x_props, x_mist = widowmaker(h, water, surf, routes, trees, spheres, capsules, boxes, prop_boxes,
                                          np.random.default_rng(1984))
    rock_boxes = len(boxes)
    boxes += prop_boxes
    water[water < h + 0.05] = -1000.0
    # Mirror Lake's west bank: the creek's channel was cut out through the
    # lake's lip and took the lip with it, which left the lake standing
    # metres over the creek bed trail like a wall. A sill of rock holds it
    # in now, the creek coming over a notch in it and down its face, and
    # what stood on the ground there stands on the sill.
    lake = (np.hypot(X - LAKE_C[0], Z - LAKE_C[1]) < LAKE_R + 2) & (water > lake_level - 0.01)
    lift, d_lake = bank_up(h, water, lake, lake_level, tracks=routes + [extreme])
    sill = lift > 0.05
    chan = (cKDTree(resample(CREEK, 1.0)).query(FLAT)[0].reshape(N, N) < 5.8) & sill
    notch = chan & (d_lake <= 4.5)
    h[notch] = lake_level - 0.25
    water[notch] = lake_level
    water[chan & ~notch] = h[chan & ~notch] + 0.3
    water[sill & ~chan] = -1000.0
    surf[lift > 0.4] = S["ROCK"]
    surf[chan] = S["WETROCK"]

    def up(x, z):
        return float(sample(lift, np.array([x, z]))[0])
    spheres = [(q[0], q[1] + up(q[0], q[2]), q[2]) + tuple(q[3:]) for q in spheres]
    boxes = [(q[0], q[1] + up(q[0], q[2]), q[2]) + tuple(q[3:]) for q in boxes]
    capsules = [(q[0], q[1] + up(q[0], q[2]), q[2], q[3], q[4] + up(q[3], q[5]), q[5]) + tuple(q[6:]) for q in capsules]
    for r in routes + [extreme]:
        assert not (sample(lift, r["pts"]) > 0.05).any(), f"{r['name']}: under the lake's sill"
    print(f"  the lake's sill: {int(sill.sum())} vertices raised, {lift.max():.1f} m at most, "
          f"{int((lake & (water < -999)).sum())} of the lake's given up to it")
    trees = [(t[0], float(sample(h, np.array([t[0], t[2]]))[0]), t[2], t[3], t[4], t[5]) for t in trees]
    settle_report(h, water)
    argb, rgb, alb_ao, nrm = colours(h, water, surf)

    def gy(x, z):
        return float(sample(h, np.array([x, z]))[0])
    # Props: the ranger cabin and a picnic table by the start, signs at the
    # trailhead and the fork, cairns at the viewpoints, the summit's tower.
    props = [(-28, gy(-28, -292), -292, 0.3, 0), (-14, gy(-14, -276), -276, 1.2, 2),
             (TRAILHEAD[0] + 5, gy(TRAILHEAD[0] + 5, TRAILHEAD[1]), TRAILHEAD[1], 0.0, 1),
             (FORK[0] - 6, gy(FORK[0] - 6, FORK[1]), FORK[1], 0.4, 1),
             (HIGH_MEADOW[0] + 6, gy(HIGH_MEADOW[0] + 6, HIGH_MEADOW[1]), HIGH_MEADOW[1], 0.8, 1)]
    boxes.append((-28.0, gy(-28, -292) + 1.6, -292.0, 4.2, 1.8, 3.2, 0.3))   # the cabin
    boxes.append((-14.0, gy(-14, -276) + 0.4, -276.0, 0.9, 0.4, 0.4, 1.2))    # the picnic table
    summit_y = gy(PEAK[0], PEAK[1])
    props.append((PEAK[0], summit_y, PEAK[1], 0.2, 4))
    for (px_, pz_, yaw_, kind_, y_) in extra_props:
        props.append((px_, gy(px_, pz_) if y_ is None else y_, pz_, yaw_, kind_))
    for (px_, pz_, yaw_, kind_, y_, var_) in x_props:
        props.append((px_, gy(px_, pz_) if y_ is None else y_, pz_, yaw_, kind_, var_))
    # The tower's legs: posts splayed from the deck's corners (1.8 m out)
    # to feet 2.7 m out, so a truck fits between them and can turn under
    # the deck. The trucks test posts against their whole hull (a box this
    # thin slipped between the hull's contact points, and its top was
    # ground 6 m up).
    posts = []
    for sx, sz in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        fx, fz = PEAK[0] + 2.7 * sx, PEAK[1] + 2.7 * sz
        posts.append((fx, gy(fx, fz) - 0.4, fz, PEAK[0] + 1.8 * sx, summit_y + 6.0, PEAK[1] + 1.8 * sz, 0.30))
    # Viewpoints along each route, high on it, where the view opens out.
    views = []
    names = {"LOGGING ROAD": "EAGLE BLUFF", "RIDGE LINE": "KNIFE EDGE", "CREEK BED": "CEDAR FALLS",
             "SUMMIT TRAIL": "SUMMIT", "FOREST ROAD": "FIR HOLLOW"}
    for r in routes:
        if r["name"] == "SUMMIT TRAIL":
            p = r["pts"][-1]
        elif r["name"] == "CREEK BED":
            # On the shelf above the long drop, in sight of the pool.
            p = r["pts"][int(np.argmin(np.linalg.norm(r["pts"] - np.array([-83.0, 60.0]), axis=1)))]
        else:
            k = int(own(r) * 0.82)
            p = r["pts"][k] + np.array([0.0, 0.0])
        views.append((p[0], gy(p[0], p[1]), p[1], names[r["name"]]))
    for v in views[:-1]:
        props.append((v[0] + 4.0, gy(v[0] + 4.0, v[2]), v[2], 0.0, 3))
    boxes += cairn_boxes(props)
    views.append((LAKE_C[0] + LAKE_R + 6, gy(LAKE_C[0] + LAKE_R + 6, LAKE_C[1]), LAKE_C[1], "MIRROR LAKE"))
    for (vx, vz, name) in extra_views:
        views.append((vx, gy(vx, vz), vz, name))
    spawns = []
    for k in range(4):
        sx = SPAWN[0] + (k - 1.5) * 6.0
        sz = SPAWN[1] - abs(k - 1.5) * 2.0
        spawns.append((sx, gy(sx, sz), sz, 0.0))

    out.mkdir(parents=True, exist_ok=True)
    safe = recover_spots(h, water, routes + [extreme], spheres, capsules, boxes, posts, trees)
    light = light_inputs(alb_ao, nrm, h, water, flags=vertex_flags(None, safe))
    hz = horizon_map(h)
    blob = pack_level(h, water, surf, light, hz)
    (out / "level_cascade.lvl").write_bytes(blob)
    if raw:
        raw.write_bytes(raw_level(h, argb, water, surf, light, hz))
    short = {"FOREST ROAD": "FOREST", "LOGGING ROAD": "LOGGING", "CREEK BED": "CREEK", "RIDGE LINE": "RIDGE",
             "SUMMIT TRAIL": "SUMMIT"}
    for r in routes:
        r["short"] = short[r["name"]]
    routes = routes + [extreme]
    write_header(out / "level_cascade.h", "cascade", {
        "name": "CASCADE RIDGE", "blurb": "A NORTHWEST MOUNTAIN IN AUTUMN", "base_ft": 2380,
        "lake_y": lake_level, "start_y": gy(SPAWN[0], SPAWN[1]), "summit": (PEAK[0], summit_y, PEAK[1]),
        "routes": routes, "spheres": spheres, "capsules": capsules, "boxes": boxes, "rock_boxes": rock_boxes,
        "posts": posts, "trees": trees, "props": props, "views": views, "spawns": spawns,
        # Where mist lies at dawn: the meadow and the pond, the east meadow,
        # the boulder field, up the creek's valley and on the lake.
        "mists": [(-40, -255, 55), (20, -228, 55), (140, -210, 50), (-130, -250, 45), (-70, -180, 40), (-105, -100, 40),
                  (-115, -20, 40), (-42, 66, 38), x_mist],
        # Hawks over the meadow, Eagle Bluff and above Cedar Falls.
        "hawks": [(10, -235, 34, 42, 0.0, 1), (118, 36, 28, 38, 2.0, -1), (-80, 84, 40, 46, 4.0, 1)],
        "herds": [(150, -196), (-150, -172)],
        "rocks": [],
    })
    minimap(rgb, water, routes, tracks, out / "level_map_cascade.png")
    if preview:
        preview_image(rgb, water, routes, spheres, capsules, boxes, trees, views, preview)
    print(f"heights {h.min():.1f}..{h.max():.1f} m, lake {lake_level:.1f} m, summit {summit_y:.1f} m")
    for r in routes:
        seg = np.linalg.norm(np.diff(r["pts"], axis=0), axis=1)
        grade = np.abs(np.diff(r["y"])) / np.maximum(seg, 1e-3)
        print(f"  {r['name']:13s} {seg.sum():6.0f} m, climbs {r['y'][-1] - r['y'][0]:6.1f} m, "
              f"grade p95 {np.percentile(grade, 95) * 100:4.0f}% max {grade.max() * 100:4.0f}%")
    print(f"  {len(spheres)} boulders, {len(capsules)} logs, {len(boxes)} boxes, {len(trees)} trees, "
          f"{len(views)} viewpoints, level_cascade.lvl {len(blob)} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--area", choices=["cascade", "moab", "dunes", "lowland", "all"], default="all")
    ap.add_argument("--preview", type=Path, default=None, help="a folder for each area's preview picture")
    ap.add_argument("--raw", type=Path, default=None, help="a folder for each area's grid unpacked (level_<area>.bin)")
    args = ap.parse_args()
    for d in (args.preview, args.raw):
        if d:
            d.mkdir(parents=True, exist_ok=True)

    def opt(d, name):
        return d / name if d else None
    if args.area in ("cascade", "all"):
        print("Cascade Ridge")
        build(args.out, opt(args.preview, "level-cascade.png"), opt(args.raw, "level_cascade.bin"))
    if args.area in ("moab", "all"):
        import level_moab
        level_moab.build(args.out, opt(args.preview, "level-moab.png"), opt(args.raw, "level_moab.bin"))
    if args.area in ("dunes", "all"):
        import level_dunes
        level_dunes.build(args.out, opt(args.preview, "level-dunes.png"), opt(args.raw, "level_dunes.bin"))
    if args.area in ("lowland", "all"):
        import level_lowland
        level_lowland.build(args.out, opt(args.preview, "level-lowland.png"), opt(args.raw, "level_lowland.bin"))


if __name__ == "__main__":
    main()
