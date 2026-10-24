#!/usr/bin/env python3
"""Build Spiral Canyon's rock as one baked mesh with Blender.

Run through Blender, not the host Python interpreter (``make canyon-rock``
does both steps):

    blender --background --python tools/build_canyon_rock.py -- \
        --dump captures/canyon-rock-dump.log --header assets/generated/canyon_rock.h \
        --blend captures/canyon_rock.blend

The dump comes from a ``-DDK_DUMP_ROCK`` build of the game: the rock's height
field and the valley floor on a 1 m grid, the tunnel bores and the open road's
clearance as profiles in world space, and the two towers. The height field
becomes one closed solid; the bores, the clearance and the towers are lofted
and cut from it with exact booleans, so every tunnel mouth, wing wall and
cutting comes out of the same surface and nothing is left to stitch. Faces
under the valley floor or under a road deck are dropped (they are never seen),
near-flat ones dissolved, and the rest triangulated, cut into strips and
written as a header the game draws as it is.

Coordinates: the game is y-up; Blender gets (x, -z, y), so the scene reads
the right way round in the viewport, and the export maps it back.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import bmesh
import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stripify import stripify  # noqa: E402
from mathutils import Vector
from mathutils.kdtree import KDTree

TILE = 64.0          # chunk size in metres (the game culls per chunk, and strips stop at a chunk's edge)
MAX_VERTS = 10000    # both rocks: what the world's vertex pool can spare (34000 less ~21500 for the rest)


def parse_dump(path: Path):
    grid = None
    H, G = {}, {}
    lofts = []   # (kind, [[(x, y, z), ...] per station])
    towers = []
    cur = None
    for raw in path.read_text(encoding="latin-1").splitlines():
        i = raw.find("DKR ")
        if i < 0:
            continue
        f = raw[i + 4:].split()
        tag = f[0]
        if tag == "GRID":
            grid = (float(f[1]), float(f[2]), float(f[3]), int(f[4]), int(f[5]))
        elif tag in ("H", "G"):
            (H if tag == "H" else G)[int(f[1])] = [int(v) for v in f[2:]]
        elif tag in ("BORE", "CLEAR"):
            cur = (tag, [])
            lofts.append(cur)
        elif tag == "P":
            v = [float(x) for x in f[1:]]
            cur[1].append([(v[k], v[k + 1], v[k + 2]) for k in range(0, len(v), 3)])
        elif tag == "TOWER":
            towers.append(tuple(float(x) for x in f[1:5]))
    if grid is None or not lofts:
        sys.exit(f"{path}: no DKR dump found (build with -DDK_DUMP_ROCK)")
    x0, z0, step, nx, nz = grid
    for name, d in (("H", H), ("G", G)):
        missing = [j for j in range(nz) if j not in d or len(d[j]) != nx]
        if missing:
            sys.exit(f"{path}: grid {name} rows missing or short: {missing[:5]}...")
    return grid, H, G, lofts, towers


def b(x, y, z):
    """Game (y up) to Blender (z up)."""
    return (x, -z, y)


def heightfield_solid(grid, H, G):
    x0, z0, step, nx, nz = grid
    bm = bmesh.new()
    top = []
    lo = 1e9
    for j in range(nz):
        row = []
        for i in range(nx):
            g = G[j][i] / 10.0
            h = H[j][i] / 10.0 if H[j][i] != -9999 else -1e9
            y = max(h, g - 2.0)        # no rock: just under the floor (cut away later)
            lo = min(lo, y)
            row.append(bm.verts.new(b(x0 + i * step, y, z0 + j * step)))
        top.append(row)
    base = lo - 10.0
    bot = [[bm.verts.new(b(x0 + i * step, base, z0 + j * step)) if i in (0, nx - 1) or j in (0, nz - 1) else None
            for i in range(nx)] for j in range(nz)]
    for j in range(nz - 1):
        for i in range(nx - 1):
            bm.faces.new((top[j][i], top[j][i + 1], top[j + 1][i + 1], top[j + 1][i]))
    # Walls round the edge and one flat bottom: a closed solid.
    ring = [(i, 0) for i in range(nx)] + [(nx - 1, j) for j in range(1, nz)] + \
           [(i, nz - 1) for i in range(nx - 2, -1, -1)] + [(0, j) for j in range(nz - 2, 0, -1)]
    for k in range(len(ring)):
        (ia, ja), (ib, jb) = ring[k], ring[(k + 1) % len(ring)]
        bm.faces.new((top[ja][ia], bot[ja][ia], bot[jb][ib], top[jb][ib]))
    bm.faces.new([bot[j][i] for i, j in reversed(ring)])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new("rock")
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new("CanyonRock", me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def road_distance(lofts):
    """Horizontal distance from a point (game x, z) to the road's edge."""
    pts = []
    for kind, stations in lofts:
        for prof in stations:
            l, r = Vector(prof[0]), Vector(prof[-1])
            c = (l + r) / 2
            pts.append((c.x, c.z, (Vector((r.x - l.x, r.z - l.z))).length / 2))
    kd = KDTree(len(pts))
    for k, (x, z, _) in enumerate(pts):
        kd.insert((x, z, 0.0), k)
    kd.balance()

    def dist(x, z):
        best = 1e9
        for co, k, d in kd.find_n((x, z, 0.0), 4):
            best = min(best, d - pts[k][2])
        return max(best, 0.0)
    return dist


def density(rock, near_fn):
    near = far = 0
    for p in rock.data.polygons:
        c = p.center
        if near_fn(c.x, -c.y) < 12.0:
            near += len(p.vertices) - 2
        else:
            far += len(p.vertices) - 2
    return near, far


def weather(rock, voxel, iterations, tris, road_dist):
    """Remesh the height field's solid and smooth it: its 1 m grid turns
    every cliff and ledge end into a staircase, and the rock reads as rock
    once those are rounded off. Then cut it down to a Dreamcast budget.
    The road, the bores and the towers are cut afterwards, so they stay
    exact. (Most of the rock lies within 12 m of the road, so decimating
    harder away from it buys little.)"""
    bpy.context.view_layer.objects.active = rock
    for kind, opts in (("REMESH", dict(mode="VOXEL", voxel_size=voxel)),
                       ("SMOOTH", dict(factor=0.5, iterations=iterations))):
        mod = rock.modifiers.new(kind.lower(), kind)
        for k, v in opts.items():
            setattr(mod, k, v)
        bpy.ops.object.modifier_apply(modifier=mod.name)
    n = sum(len(p.vertices) - 2 for p in rock.data.polygons)
    mod = rock.modifiers.new("decimate", "DECIMATE")
    mod.decimate_type = "COLLAPSE"
    mod.ratio = min(1.0, tris / max(1, n))
    bpy.ops.object.modifier_apply(modifier=mod.name)
    near, far = density(rock, road_dist)
    print(f"canyon rock: remeshed {n} tris, decimated to {near + far} ({near} within 12 m of the road, {far} beyond)")


def loft(name, stations):
    """A closed tube through the stations' profiles (each a closed polygon)."""
    bm = bmesh.new()
    rings = [[bm.verts.new(b(*p)) for p in st] for st in stations]
    n = len(rings[0])
    for a, c in zip(rings, rings[1:]):
        for k in range(n):
            bm.faces.new((a[k], a[(k + 1) % n], c[(k + 1) % n], c[k]))
    bm.faces.new(rings[0])
    bm.faces.new(list(reversed(rings[-1])))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def tower_prism(name, x, z, r, top):
    # The game's tower is a 16-sided cylinder (sin a, cos a); this is the
    # same polygon a few centimetres inside it, so the rock's cut face hides
    # behind the tower's wall rather than poking through its flats.
    st = []
    for y in (-40.0, top + 3.0):
        st.append([(x + math.sin(k / 16 * math.tau) * (r - 0.05), y, z + math.cos(k / 16 * math.tau) * (r - 0.05))
                   for k in range(16)])
    bm = bmesh.new()
    lo = [bm.verts.new(b(*p)) for p in st[0]]
    hi = [bm.verts.new(b(*p)) for p in st[1]]
    for k in range(16):
        bm.faces.new((lo[k], lo[(k + 1) % 16], hi[(k + 1) % 16], hi[k]))
    bm.faces.new(lo)
    bm.faces.new(list(reversed(hi)))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def cut(rock, cutter):
    mod = rock.modifiers.new("cut_" + cutter.name, "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.solver = "EXACT"
    mod.object = cutter
    bpy.context.view_layer.objects.active = rock
    bpy.ops.object.modifier_apply(modifier=mod.name)
    cutter.hide_set(True)
    cutter.hide_render = True


def floor_sampler(grid, G):
    x0, z0, step, nx, nz = grid

    def g(x, z):
        fi = min(max((x - x0) / step, 0.0), nx - 1.001)
        fj = min(max((z - z0) / step, 0.0), nz - 1.001)
        i, j = int(fi), int(fj)
        u, v = fi - i, fj - j
        a = G[j][i] * (1 - u) + G[j][i + 1] * u
        c = G[j + 1][i] * (1 - u) + G[j + 1][i + 1] * u
        return (a * (1 - v) + c * v) / 10.0
    return g


def deck_sampler(lofts):
    """Nearest road station: its centre, right vector, half width and deck
    height across (from the profile's floor corners, 0.6 m under the deck)."""
    st = []
    for kind, stations in lofts:
        for prof in stations:
            l, r = Vector(prof[0]), Vector(prof[-1])
            c = (l + r) / 2
            right = Vector((r.x - l.x, 0.0, r.z - l.z))
            half = right.length / 2
            st.append((c, right.normalized(), half, l.y + 0.6, r.y + 0.6))
    kd = KDTree(len(st))
    for k, s in enumerate(st):
        kd.insert((s[0].x, 0.0, s[0].z), k)
    kd.balance()

    def deck(x, z):
        _, k, _ = kd.find((x, 0.0, z))
        c, right, half, yl, yr = st[k]
        lat = (x - c.x) * right.x + (z - c.z) * right.z
        t = min(max((lat + half) / (2 * half), 0.0), 1.0)
        return lat, half, yl + (yr - yl) * t
    return deck


def prune(rock, gfloor, deck):
    """Drop faces nobody can see: under the valley floor, or flat under a
    road deck (the deck and its rock underside cover them)."""
    bm = bmesh.new()
    bm.from_mesh(rock.data)
    kill = []
    for f in bm.faces:
        pts = [(v.co.x, -v.co.y, v.co.z) for v in f.verts]   # game x, z, y
        if all(y < gfloor(x, z) - 0.15 for x, z, y in pts):
            kill.append(f)
            continue
        under = True
        for x, z, y in pts:
            lat, half, dy = deck(x, z)
            if abs(lat) > half - 0.3 or y > dy - 0.4 or y < dy - 1.2:
                under = False
                break
        if under:
            kill.append(f)
    bmesh.ops.delete(bm, geom=kill, context="FACES")
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context="VERTS")
    bm.to_mesh(rock.data)
    bm.free()


def simplify(rock, angle_deg):
    bm = bmesh.new()
    bm.from_mesh(rock.data)
    if angle_deg > 0:
        bmesh.ops.dissolve_limit(bm, angle_limit=math.radians(angle_deg), verts=bm.verts, edges=bm.edges)
    bmesh.ops.triangulate(bm, faces=bm.faces, quad_method="BEAUTY", ngon_method="BEAUTY")
    bm.to_mesh(rock.data)
    bm.free()


def box_uvs(me, size):
    """Box projection in world metres: the dominant axis of the face's
    normal picks the plane. Faces on different planes need their own copies
    of the vertices they share, which breaks the strips there; choosing by
    each vertex's own normal keeps one UV a vertex and longer strips, but on
    the steep cut walls the averaged normals pick the top plane and the
    texture streaks down them."""
    uv = []
    for poly in me.polygons:
        for li in poly.loop_indices:
            vi = me.loops[li].vertex_index
            n = poly.normal   # Blender axes: x, y (= -game z), z (= game y)
            ax = max(range(3), key=lambda k: abs(n[k]) * (1.9 if k == 2 else 1.0))
            co = me.vertices[vi].co
            if ax == 2:
                uv.append((co.x / size, co.y / size))
            elif ax == 0:
                uv.append((co.y / size, co.z / size))
            else:
                uv.append((co.x / size, co.z / size))
    return uv


def export(rock, f, prefix: str, tex_size: float, tile: float, swaps: bool):
    me = rock.data
    me.calc_loop_triangles()
    uv = box_uvs(me, tex_size)
    # One export vertex per distinct (vertex, uv).
    key_to_out = {}
    verts, normals, uvs = [], [], []
    def out_vert(li):
        vi = me.loops[li].vertex_index
        u, v = uv[li]
        key = (vi, round(u * 256), round(v * 256))
        if key not in key_to_out:
            key_to_out[key] = len(verts)
            co, nm = me.vertices[vi].co, me.vertices[vi].normal
            verts.append((co.x, co.z, -co.y))           # back to game x, y, z
            normals.append((nm.x, nm.z, -nm.y))
            uvs.append(key[1:])
        return key_to_out[key]
    tris = [tuple(out_vert(li) for li in t.loops) for t in me.loop_triangles]
    if max(max(abs(u), abs(v)) for u, v in uvs) > 32767:
        sys.exit("canyon rock: a UV out of int16 range")
    # Chunks: triangles by the tile their centre lies in.
    tiles = {}
    for t in tris:
        cx = sum(verts[i][0] for i in t) / 3
        cz = sum(verts[i][2] for i in t) / 3
        tiles.setdefault((math.floor(cx / tile), math.floor(cz / tile)), []).append(t)
    chunks = []
    out_idx = []
    strip_lens = []
    for key in sorted(tiles):
        strips = stripify(tiles[key], swaps)
        chunks.append((len(strip_lens), len(strips)))
        for seq, _ in strips:
            strip_lens.append(len(seq))
            out_idx.extend(seq)
    pool = sum(strip_lens)
    # The game draws every strip in view (culling strips that face away was
    # tried: they mix slopes, so it skipped only 7-19% and cost more strips).
    # The console's cost to draw it all, from hardware timings: 0.41 us a
    # strip vertex, 1.6 us a strip.
    cost = 0.00041 * pool + 0.0016 * len(strip_lens)
    print(f"canyon rock {prefix}: {len(verts)} verts, {len(tris)} tris, {len(strip_lens)} strips "
          f"({pool / max(1, len(strip_lens)):.1f} verts each), {len(chunks)} chunks, {pool} pool verts, "
          f"~{cost:.2f} ms a view on the console")
    if max(strip_lens) > 255:
        sys.exit("canyon rock: a strip over 255 vertices; split it")
    P, N = prefix, prefix.upper()
    f.write(f"#define {N}_VERTS {len(verts)}\n#define {N}_INDICES {len(out_idx)}\n")
    f.write(f"#define {N}_STRIPS {len(strip_lens)}\n#define {N}_CHUNKS {len(chunks)}\n")
    f.write(f"static const dk_rock_vert_t {P}_verts[{N}_VERTS] = {{\n")
    for (x, y, z), (nx, ny, nz), (u, v) in zip(verts, normals, uvs):
        f.write(f"{{{round(x * 100)},{round(y * 100)},{round(z * 100)},"
                f"{round(nx * 127)},{round(ny * 127)},{round(nz * 127)},0,{u},{v}}},\n")
    f.write(f"}};\nstatic const uint16_t {P}_index[{N}_INDICES] = {{\n")
    for k in range(0, len(out_idx), 24):
        f.write(",".join(str(v) for v in out_idx[k:k + 24]) + ",\n")
    f.write(f"}};\nstatic const uint8_t {P}_strip_len[{N}_STRIPS] = {{\n")
    for k in range(0, len(strip_lens), 32):
        f.write(",".join(str(v) for v in strip_lens[k:k + 32]) + ",\n")
    f.write(f"}};\n/* first strip, strip count */\nstatic const uint16_t {P}_chunk[{N}_CHUNKS][2] = {{\n")
    for c0, n in chunks:
        f.write(f"{{{c0},{n}}},\n")
    f.write("};\n")
    return pool


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", type=Path, required=True)
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--blend", type=Path)
    ap.add_argument("--angle", type=float, default=3.0, help="planar dissolve limit after the cuts, degrees")
    ap.add_argument("--voxel", type=float, default=0.5, help="remesh voxel size, metres")
    ap.add_argument("--smooth", type=int, default=12, help="smoothing iterations")
    ap.add_argument("--tris", type=int, default=3400, help="triangle target before the cuts")
    ap.add_argument("--split-tris", type=int, default=1200, help="the same for the split-screen rock")
    ap.add_argument("--split-clear-step", type=int, default=2, help="split-screen rock: use every nth clearance station")
    ap.add_argument("--tex", type=float, default=6.0, help="metres per texture repeat")
    ap.add_argument("--tile", type=float, default=64.0, help="chunk size, metres")
    ap.add_argument("--swaps", type=int, default=1, help="let strips continue through a repeated vertex")
    a = ap.parse_args(argv)

    grid, H, G, lofts, towers = parse_dump(a.dump)
    total = 0
    with a.header.open("w") as f:
        f.write("/* Generated by tools/build_canyon_rock.py (Blender). Do not edit. */\n")
        f.write("#ifndef DK_CANYON_ROCK_H\n#define DK_CANYON_ROCK_H\n#include <stdint.h>\n")
        f.write("/* Position in centimetres, normal * 127, texture u v in 1/256. The full\n"
                "   rock (dk_rock) and a coarser one a split screen draws (dk_rock_split). */\n")
        f.write("typedef struct { int16_t x, y, z; int8_t nx, ny, nz, pad; int16_t u, v; } dk_rock_vert_t;\n")
        for prefix, tris in (("dk_rock", a.tris), ("dk_rock_split", a.split_tris)):
            bpy.ops.wm.read_factory_settings(use_empty=True)
            rock = heightfield_solid(grid, H, G)
            weather(rock, a.voxel, a.smooth, tris, road_distance(lofts))
            for n, (kind, stations) in enumerate(lofts):
                # The split-screen rock cuts the open road's clearance at
                # every other station (6 m): fewer faces along the
                # cuttings, and on the tightest bend (22 m) the chords stay
                # 0.2 m clear of the barriers.
                if kind == "CLEAR" and prefix == "dk_rock_split" and a.split_clear_step > 1:
                    stations = stations[::a.split_clear_step] + ([stations[-1]] if (len(stations) - 1) % a.split_clear_step else [])
                cut(rock, loft(f"{kind.title()}{n}", stations))
            for n, (x, z, r, top) in enumerate(towers):
                cut(rock, tower_prism(f"Tower{n}", x, z, r, top))
            prune(rock, floor_sampler(grid, G), deck_sampler(lofts))
            simplify(rock, a.angle)
            if a.blend and prefix == "dk_rock":
                a.blend.parent.mkdir(parents=True, exist_ok=True)
                bpy.ops.wm.save_as_mainfile(filepath=str(a.blend.resolve()))
            total += export(rock, f, prefix, a.tex, a.tile, a.swaps)
        f.write("#endif\n")
    if total > MAX_VERTS:
        sys.exit(f"canyon rock: {total} strip vertices in all, over the {MAX_VERTS} budget")

main()
