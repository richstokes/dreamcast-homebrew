"""Headless Blender preview renders of Ridgeline's trucks.

    uv run tools/build_trucks.py --preview-json /tmp/dl-trucks.json
    blender --background --factory-startup --python tools/preview_trucks_blender.py -- \
        --json /tmp/dl-trucks.json --out captures/truck-previews

Writes <truck>-lod0-{front34,rear34,side,top,front,rear,low,wheel}.png,
<truck>-lod{1,2}-{front34,rear34}.png,
<truck>-liveries.png (the four liveries side by side), <truck>-lods-{20,45,80}m.png
(LOD0 | LOD1 | LOD2 at game resolution, nearest-neighbour enlarged) and a
contact sheet trucks-sheet.png.

Lighting imitates the runtime: one directional light plus ambient evaluated per
vertex from the exported (quantised) normals, Gouraud interpolated (Workbench
flat shading of baked vertex colours). Back faces are culled, so a wrongly wound
triangle shows up as a hole. Wheels sit at wheel_pos (left mesh on the left),
axles are scaled between the wheel centres like the game does.

Truck space is +X right, +Y up, +Z forward; Blender is +X right, +Y forward,
+Z up, so positions map (x, y, z) -> (x, z, y) and triangle winding is reversed.
"""

import json
import sys
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
import blender_preview  # noqa: E402
from blender_preview import argb_to_rgb, look_at, make_object, mesh_faces, norm, parse_args, setup_scene  # noqa: E402

LIGHT_DIR = (0.45, 0.80, 0.40)    # towards the sun, truck space
AMBIENT = 0.50
DIFFUSE = 0.58
BODY, WHEEL_L, WHEEL_R, AXLE = range(4)


def luma(rgb):
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def livery_colour(argb, slot, livery, ref):
    """Slots 1-3 take the livery colour scaled by the vertex's brightness relative
    to livery 0's reference colour for that slot (how the game recolours)."""
    rgb = argb_to_rgb(argb)
    if livery is None or slot not in (1, 2, 3):
        return rgb
    new = argb_to_rgb(livery[slot - 1])
    k = luma(rgb) / max(1e-3, luma(argb_to_rgb(ref[slot - 1])))
    return tuple(min(1.0, c * k) for c in new)


def mesh_object(m, collection, offset, scale_x=1.0, livery=None, ref=None):
    light = norm(LIGHT_DIR)
    pos = [(p[0] * scale_x + offset[0], p[1] + offset[1], p[2] + offset[2]) for p in m["pos"]]
    cols = []
    for n, argb, slot in zip(m["nrm"], m["argb"], m["slot"]):
        r, g, b = livery_colour(argb, slot, livery, ref)
        k = AMBIENT + DIFFUSE * max(0.0, sum(n[i] * light[i] for i in range(3)))
        if slot in (5, 7):
            k = max(k, 0.95)      # lenses read as lit glass
        cols.append((r * k, g * k, b * k))
    return make_object(f"t{m['truck']}_p{m['part']}_l{m['lod']}", pos, mesh_faces(m), cols, collection)


def add_truck(data, truck, lod, collection, origin=(0.0, 0.0, 0.0), livery=None):
    info = data["trucks"][truck]
    ref = info["liveries"][0][1:]
    meshes = {m["part"]: m for m in data["meshes"] if m["truck"] == truck and m["lod"] == lod}
    objs = []
    o = origin
    objs.append(mesh_object(meshes[BODY], collection, o, livery=livery, ref=ref))
    wp = info["wheel_pos"]
    for i, p in enumerate(wp):
        part = WHEEL_L if p[0] < 0 else WHEEL_R
        objs.append(mesh_object(meshes[part], collection, (p[0] + o[0], p[1] + o[1], p[2] + o[2]),
                                livery=livery, ref=ref))
    if AXLE in meshes:
        for a, b in ((wp[0], wp[1]), (wp[2], wp[3])):
            mid = [(a[k] + b[k]) / 2 + o[k] for k in range(3)]
            objs.append(mesh_object(meshes[AXLE], collection, mid, scale_x=abs(b[0] - a[0]), livery=livery,
                                    ref=ref))
    return objs


def ground(collection, centres=((0.0, 0.0),)):
    # sandy checker with a truck-sized blob shadow under each centre
    return blender_preview.ground(collection, centres, extent=40, shades=(0.50, 0.55), tint=(0.95, 0.92, 0.80),
                                  blob=(1.15, 2.45), blob_z=0.0, blob_lift=0.004, blob_ring=28,
                                  blob_colours=((0.20, 0.20, 0.18), (0.46, 0.44, 0.38)))


def load_px(path):
    img = bpy.data.images.load(path)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
    bpy.data.images.remove(img)
    return px


def save_px(px, path):
    h, w = px.shape[:2]
    out = bpy.data.images.new("sheet", w, h, alpha=True)
    out.pixels.foreach_set(px.ravel())
    out.filepath_raw = path
    out.file_format = "PNG"
    out.save()
    bpy.data.images.remove(out)


def pixelate(path, scale):
    px = load_px(path)
    big = np.repeat(np.repeat(px, scale, axis=0), scale, axis=1)
    save_px(big, path)


def main():
    args = parse_args(size=(960, 720))
    with open(args.json) as f:
        data = json.load(f)

    scene, cam = setup_scene(sky=(0.64, 0.80, 0.93))

    colls = {}

    def coll(key):
        if key not in colls:
            c = bpy.data.collections.new(key)
            scene.collection.children.link(c)
            colls[key] = c
        return colls[key]

    def show(*names):
        for k, c in colls.items():
            c.hide_render = k not in names

    ntrucks = len(data["trucks"])
    lods = sorted({m["lod"] for m in data["meshes"]})
    ground(coll("ground"))
    for t in range(ntrucks):
        for lod in lods:
            add_truck(data, t, lod, coll(f"t{t}l{lod}"))
        # LODs side by side along X for the distance strips
        for i, lod in enumerate(lods):
            add_truck(data, t, lod, coll(f"t{t}cmp"), origin=((i - 1) * 2.6, 0.0, 0.0))
        for i, lv in enumerate(data["trucks"][t]["liveries"]):
            add_truck(data, t, 0, coll(f"t{t}liv"), origin=((i - 1.5) * 2.5, 0.0, 0.0), livery=lv[1:])
    ground(coll("ground_cmp"), centres=[((i - 1) * 2.6, 0.0) for i in range(len(lods))])
    ground(coll("ground_liv"), centres=[((i - 1.5) * 2.5, 0.0) for i in range(4)])

    views = {
        "front34": ((4.6, 2.3, 6.4), (0.0, 1.0, 0.15), 40.0),
        "rear34": ((-4.8, 2.6, -6.2), (0.0, 1.0, -0.2), 40.0),
        "side": ((8.8, 1.2, 0.0), (0.0, 1.0, 0.0), 40.0),
        "top": ((3.2, 7.6, 4.2), (0.0, 0.7, -0.1), 40.0),
        "front": ((0.0, 1.35, 9.0), (0.0, 1.05, 0.0), 45.0),
        "low": ((3.0, 0.55, 5.4), (0.0, 0.9, 0.3), 32.0),
        "wheel": ((2.9, 0.95, 2.9), (0.8, 0.55, 1.25), 50.0),
        "rear": ((0.0, 1.5, -8.5), (0.0, 1.0, 0.0), 45.0),
    }
    out = args.out.rstrip("/")
    scene.render.resolution_x, scene.render.resolution_y = args.size
    written = {}
    for t in range(ntrucks):
        slug = data["trucks"][t]["name"].split()[0].lower()
        for lod in lods:
            for view, (eye, tgt, lens) in views.items():
                if lod > 0 and view not in ("front34", "rear34"):
                    continue
                show(f"t{t}l{lod}", "ground")
                look_at(cam, eye, tgt, lens)
                path = f"{out}/{slug}-lod{lod}-{view}.png"
                scene.render.filepath = path
                bpy.ops.render.render(write_still=True)
                written[(t, lod, view)] = path
        show(f"t{t}liv", "ground_liv")
        look_at(cam, (5.0, 4.2, 11.0), (0.0, 0.8, 0.0), 30.0)
        scene.render.resolution_x, scene.render.resolution_y = 1600, 700
        path = f"{out}/{slug}-liveries.png"
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        written[(t, "liv")] = path
        # game resolution (640x480, ~70 degree horizontal FOV) at LOD distances
        scene.render.resolution_x, scene.render.resolution_y = 640, 480
        scene.display.render_aa = "8"
        for dist in (20, 45, 80):
            show(f"t{t}cmp", "ground_cmp")
            look_at(cam, (0.35 * dist, 0.18 * dist + 1.5, 0.9 * dist), (0.0, 0.9, 0.0), 22.4)
            r = scene.render
            # crop to the middle and blow up so pixels stay visible
            frac = min(0.5, 8.5 / dist)
            r.use_border, r.use_crop_to_border = True, True
            r.border_min_x, r.border_max_x = 0.5 - frac * 0.75, 0.5 + frac * 0.75
            r.border_min_y, r.border_max_y = 0.5 - frac * 0.5, 0.5 + frac * 0.5
            path = f"{out}/{slug}-lods-{dist}m.png"
            r.filepath = path
            bpy.ops.render.render(write_still=True)
            r.use_border = False
            pixelate(path, max(1, int(round(0.5 / frac))) * 2)
        scene.display.render_aa = "16"
        scene.render.resolution_x, scene.render.resolution_y = args.size

    # contact sheet: per truck one row of LOD0 front34 / rear34 / side / top and one of LOD1, LOD2, liveries
    tile_w, tile_h = 480, 360
    rows = []
    for t in range(ntrucks):
        keys = [(t, 0, "front34"), (t, 0, "rear34"), (t, 0, "side"), (t, 0, "top"),
                (t, 0, "low"), (t, 1, "front34"), (t, 2, "front34"), (t, 0, "front")]
        tiles = []
        for k in keys:
            px = load_px(written[k])
            h, w = px.shape[:2]
            ys = (np.arange(tile_h) * h / tile_h).astype(int)
            xs = (np.arange(tile_w) * w / tile_w).astype(int)
            tiles.append(px[ys][:, xs])
        rows.append(np.concatenate(tiles[:4], axis=1))
        rows.append(np.concatenate(tiles[4:], axis=1))
    # rows are in reading order; Blender pixel rows run bottom-up, so stack reversed
    sheet = np.concatenate(rows[::-1], axis=0)
    save_px(sheet, f"{out}/trucks-sheet.png")


main()
