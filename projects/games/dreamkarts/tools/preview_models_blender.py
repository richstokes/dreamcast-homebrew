"""Headless Blender preview renders of the DreamKarts models.

    uv run tools/build_models.py --preview-json /tmp/dk-models.json
    blender --background --factory-startup --python tools/preview_models_blender.py -- \
        --json /tmp/dk-models.json --out assets/generated/previews

Writes models-lod{0,1,3}-{chase,game,front34,side,rear34}.png, models-lod0-face.png,
models-emotes.png and side-by-side LOD comparisons (LOD0 | LOD1 | LOD3, left to
right / back to front) models-compare-{chase,side}.png plus
pixel-accurate models-compare-chase-{20,40}m.png and models-compare-side-20m.png.
Lighting imitates the runtime: one directional light plus ambient evaluated per
vertex from the exported (quantised) normals, Gouraud interpolated (Workbench
flat shading of baked vertex colours). Unlit meshes
(flags bit0) show their raw colour. Back faces are culled, so a wrongly wound
triangle shows up as a hole.

Model space is +X right, +Y up, +Z forward; Blender is +X right, +Y forward,
+Z up, so positions map (x, y, z) -> (x, z, y) and triangle winding is reversed.
"""

import argparse
import json
import math
import sys

import bpy
from mathutils import Vector

LIGHT_DIR = (0.30, 0.92, 0.10)    # towards the light, model space (overhead, a little right)
AMBIENT = 0.46
DIFFUSE = 0.62


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", type=int, nargs=2, default=(800, 600))
    return ap.parse_args(argv)


def norm(v):
    length = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / length for c in v)


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def to_bl(p):
    return (p[0], p[2], p[1])


def argb_to_rgb(argb):
    return ((argb >> 16) & 0xFF) / 255.0, ((argb >> 8) & 0xFF) / 255.0, (argb & 0xFF) / 255.0


def make_object(name, positions, faces, colours, collection):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([to_bl(p) for p in positions], [], faces)
    mesh.update()
    attr = mesh.color_attributes.new("Col", "FLOAT_COLOR", "POINT")
    flat = []
    for c in colours:
        flat += [srgb_to_linear(min(1.0, c[0])), srgb_to_linear(min(1.0, c[1])),
                 srgb_to_linear(min(1.0, c[2])), 1.0]
    attr.data.foreach_set("color", flat)
    mesh.color_attributes.active_color = attr
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def mesh_object(m, pivots, collection, offset=(0.0, 0.0, 0.0)):
    light = norm(LIGHT_DIR)
    piv = pivots[str(m["part"])]
    pos = [tuple(p[i] + piv[i] + offset[i] for i in range(3)) for p in m["pos"]]
    cols = []
    for n, argb in zip(m["nrm"], m["argb"]):
        r, g, b = argb_to_rgb(argb)
        if m["flags"] & 1:
            k = 1.0
        else:
            k = AMBIENT + DIFFUSE * max(0.0, sum(n[i] * light[i] for i in range(3)))
        cols.append((r * k, g * k, b * k))
    idx = m["idx"]
    faces = [(idx[i], idx[i + 2], idx[i + 1]) for i in range(0, len(idx), 3)]
    return make_object(f"p{m['part']}_l{m['lod']}_v{m['variant']}_f{m['flags']}", pos, faces, cols, collection)


def ground(collection, shadows=((0.0, 0.0),), extent=50):
    # 1 m checker so scale is readable, plus a soft blob shadow under each kart.
    pos, faces, cols = [], [], []
    for ix in range(-extent, extent):
        for iz in range(-extent, extent):
            base = len(pos)
            pos += [(ix, 0.0, iz), (ix, 0.0, iz + 1), (ix + 1, 0.0, iz + 1), (ix + 1, 0.0, iz)]
            shade = 0.42 if (ix + iz) % 2 == 0 else 0.47
            cols += [(shade * 0.92, shade, shade * 0.88)] * 4
            faces.append((base, base + 1, base + 2, base + 3))
    ring = 24
    for sx, sz in shadows:
        centre = len(pos)
        pos.append((sx, 0.003, sz - 0.02))
        cols.append((0.16, 0.17, 0.16))
        for k in range(ring):
            a = 2 * math.pi * k / ring
            pos.append((sx + 0.78 * math.cos(a), 0.003, sz - 0.02 + 1.12 * math.sin(a)))
            cols.append((0.40, 0.43, 0.38))
        for k in range(ring):
            faces.append((centre, centre + 1 + (k + 1) % ring, centre + 1 + k))
    # model-space positions; to_bl swaps, so reverse winding to face +Y(up)
    faces = [tuple(reversed(f)) for f in faces]
    return make_object("ground", pos, faces, cols, collection)


def render_pixelated(scene, cam, objects, path, scale=4, margin=0.25):
    """Render at the game's resolution, crop to the given objects and blow the
    crop up with nearest-neighbour sampling so individual pixels stay visible."""
    import numpy as np
    from bpy_extras.object_utils import world_to_camera_view

    bpy.context.view_layer.update()
    xs, ys = [], []
    for obj in objects:
        for v in obj.data.vertices:
            p = world_to_camera_view(scene, cam, obj.matrix_world @ v.co)
            xs.append(p.x), ys.append(p.y)
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    r = scene.render
    r.use_border, r.use_crop_to_border = True, True
    r.border_min_x, r.border_max_x = max(0.0, min(xs) - margin * w), min(1.0, max(xs) + margin * w)
    r.border_min_y, r.border_max_y = max(0.0, min(ys) - margin * h), min(1.0, max(ys) + margin * h)
    r.filepath = path
    bpy.ops.render.render(write_still=True)
    r.use_border = False
    img = bpy.data.images.load(path)
    iw, ih = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(ih, iw, 4)
    big = np.repeat(np.repeat(px, scale, axis=0), scale, axis=1)
    out = bpy.data.images.new("pix", iw * scale, ih * scale, alpha=True)
    out.pixels.foreach_set(big.ravel())
    out.filepath_raw = path
    out.file_format = "PNG"
    out.save()
    bpy.data.images.remove(img)
    bpy.data.images.remove(out)


def look_at(cam, eye, target, lens):
    eye_b, tgt_b = Vector(to_bl(eye)), Vector(to_bl(target))
    cam.location = eye_b
    cam.rotation_euler = (tgt_b - eye_b).to_track_quat("-Z", "Y").to_euler()
    cam.data.lens = lens


def main():
    args = parse_args()
    with open(args.json) as f:
        data = json.load(f)

    scene = bpy.context.scene
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)

    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "FLAT"
    scene.display.shading.color_type = "VERTEX"
    scene.display.shading.show_backface_culling = True
    scene.display.render_aa = "16"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    if scene.world is None:
        scene.world = bpy.data.worlds.new("World")
    scene.world.color = (srgb_to_linear(0.62), srgb_to_linear(0.80), srgb_to_linear(0.95))
    scene.render.resolution_x, scene.render.resolution_y = args.size
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"

    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.data.clip_start = 0.05

    pivots = data["pivots"]
    colls = {}
    # LOD0 vs LOD1 comparisons: side by side across X (chase) or along Z (side).
    lods = sorted({m["lod"] for m in data["meshes"]})
    cmp_x = {lod: ((i - (len(lods) - 1) / 2) * 1.9, 0.0, 0.0) for i, lod in enumerate(lods)}
    cmp_z = {lod: (0.0, 0.0, (i - (len(lods) - 1) / 2) * 2.7) for i, lod in enumerate(lods)}
    keys = ["emotes", "ground", "ground_x", "ground_z"]
    keys += [f"{k}{lod}" for lod in lods for k in ("lod", "cmpx", "cmpz")]
    for key in keys:
        c = bpy.data.collections.new(key)
        scene.collection.children.link(c)
        colls[key] = c
    ground(colls["ground"])
    ground(colls["ground_x"], [(o[0], o[2]) for o in cmp_x.values()])
    ground(colls["ground_z"], [(o[0], o[2]) for o in cmp_z.values()])
    for m in data["meshes"]:
        if m["variant"] in (0, 1):
            mesh_object(m, pivots, colls[f"lod{m['lod']}"])
            mesh_object(m, pivots, colls[f"cmpx{m['lod']}"], cmp_x[m["lod"]])
            mesh_object(m, pivots, colls[f"cmpz{m['lod']}"], cmp_z[m["lod"]])
        if m["part"] == len(data["parts"]) - 1:
            # Emote variants side by side, one row per LOD (LOD0 on top) in front of the camera.
            off = ((m["variant"] - 2) * 0.42, -0.30 - 0.36 * lods.index(m["lod"]), 0.9)
            mesh_object(m, pivots, colls["emotes"], off)

    views = {
        "chase": ((0.0, 2.4, -5.5), (0.0, 0.72, 0.0), 58.0),
        "game": ((0.0, 2.4, -5.5), (0.0, 0.9, 2.0), 22.0),
        "front34": ((2.7, 1.55, 3.3), (0.0, 0.68, 0.0), 52.0),
        "side": ((5.2, 0.85, 0.0), (0.0, 0.78, 0.0), 58.0),
        "rear34": ((-2.9, 1.8, -3.3), (0.0, 0.62, 0.0), 52.0),
    }

    def show(*names):
        for k, c in colls.items():
            c.hide_render = k not in names

    out = args.out.rstrip("/")
    for lod in (f"lod{lod}" for lod in lods):
        for view, (eye, tgt, lens) in views.items():
            show(lod, "ground")
            look_at(cam, eye, tgt, lens)
            scene.render.filepath = f"{out}/models-{lod}-{view}.png"
            bpy.ops.render.render(write_still=True)
    show("lod0", "ground")
    look_at(cam, (0.75, 1.28, 2.0), (0.0, 1.12, -0.2), 62.0)
    scene.render.filepath = f"{out}/models-lod0-face.png"
    bpy.ops.render.render(write_still=True)
    show("emotes")
    look_at(cam, (0.3, 1.25, 3.1), (0.0, 1.08, 0.7), 40.0)
    scene.render.filepath = f"{out}/models-emotes.png"
    bpy.ops.render.render(write_still=True)

    # Every LOD side by side (LOD0 on the left), close up at full resolution...
    cmpx = [f"cmpx{lod}" for lod in lods]
    cmpz = [f"cmpz{lod}" for lod in lods]
    show(*cmpx, "ground_x")
    look_at(cam, (0.0, 2.4, -7.0 - 2.0 * (len(lods) - 2)), (0.0, 0.7, 0.0), 40.0)
    scene.render.filepath = f"{out}/models-compare-chase.png"
    bpy.ops.render.render(write_still=True)
    show(*cmpz, "ground_z")
    look_at(cam, (7.5 + 3.0 * (len(lods) - 2), 1.0, 0.0), (0.0, 0.75, 0.0), 40.0)
    scene.render.filepath = f"{out}/models-compare-side.png"
    bpy.ops.render.render(write_still=True)
    # ...and at the distances LOD1 is used: 640x480, ~69 degree horizontal FOV,
    # cropped and enlarged 4x with nearest-neighbour sampling.
    size = (scene.render.resolution_x, scene.render.resolution_y)
    scene.render.resolution_x, scene.render.resolution_y = 640, 480
    scene.display.render_aa = "8"
    for dist in (20, 40):
        show(*cmpx, "ground_x")
        look_at(cam, (0.0, 2.4, -float(dist)), (0.0, 0.7, 0.0), 25.0)
        render_pixelated(scene, cam, [o for k in cmpx for o in colls[k].objects],
                         f"{out}/models-compare-chase-{dist}m.png")
    show(*cmpz, "ground_z")
    look_at(cam, (20.0, 1.6, 0.0), (0.0, 0.7, 0.0), 25.0)
    render_pixelated(scene, cam, [o for k in cmpz for o in colls[k].objects], f"{out}/models-compare-side-20m.png")
    scene.render.resolution_x, scene.render.resolution_y = size


main()
