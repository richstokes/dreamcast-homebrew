"""Shared by the headless Blender preview renders of the games' models
(DreamKarts' tools/preview_models_blender.py, Ridgeline's
tools/preview_trucks_blender.py): the Workbench scene with vertex colours and
back-face culling, a mesh from a build_*.py preview JSON mesh, the checker
ground with blob shadows and a look-at camera. Runs inside Blender's Python.

Model space is +X right, +Y up, +Z forward; Blender is +X right, +Y forward,
+Z up, so to_bl maps (x, y, z) -> (x, z, y) and callers reverse triangle winding.
"""

import argparse
import math
import sys

import bpy
from mathutils import Vector


def parse_args(size=(800, 600)):
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", type=int, nargs=2, default=size)
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


def setup_scene(sky):
    """Empty the scene, set up flat vertex-colour Workbench rendering to PNG with
    the given sRGB sky colour, and return (scene, camera)."""
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
    scene.world.color = tuple(srgb_to_linear(c) for c in sky)
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.data.clip_start = 0.05
    return scene, cam


def make_object(name, positions, faces, colours, collection):
    """A mesh object from model-space positions, faces (already wound for
    Blender) and per-vertex sRGB colours, linked into the collection."""
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


def mesh_faces(m):
    """Triangles of a preview JSON mesh, rewound for Blender."""
    idx = m["idx"]
    return [(idx[i], idx[i + 2], idx[i + 1]) for i in range(0, len(idx), 3)]


def ground(collection, centres=((0.0, 0.0),), *, extent=50, shades=(0.42, 0.47), tint=(0.92, 1.0, 0.88),
           blob=(0.78, 1.12), blob_z=-0.02, blob_lift=0.003, blob_ring=24,
           blob_colours=((0.16, 0.17, 0.16), (0.40, 0.43, 0.38))):
    """1 m checker so scale is readable (alternating shades times the tint),
    plus a soft elliptical blob shadow (x, z radii) under each centre."""
    pos, faces, cols = [], [], []
    for ix in range(-extent, extent):
        for iz in range(-extent, extent):
            base = len(pos)
            pos += [(ix, 0.0, iz), (ix, 0.0, iz + 1), (ix + 1, 0.0, iz + 1), (ix + 1, 0.0, iz)]
            shade = shades[0] if (ix + iz) % 2 == 0 else shades[1]
            cols += [(shade * tint[0], shade * tint[1], shade * tint[2])] * 4
            faces.append((base, base + 1, base + 2, base + 3))
    for cx, cz in centres:
        centre = len(pos)
        pos.append((cx, blob_lift, cz + blob_z))
        cols.append(blob_colours[0])
        for k in range(blob_ring):
            a = 2 * math.pi * k / blob_ring
            pos.append((cx + blob[0] * math.cos(a), blob_lift, cz + blob_z + blob[1] * math.sin(a)))
            cols.append(blob_colours[1])
        for k in range(blob_ring):
            faces.append((centre, centre + 1 + (k + 1) % blob_ring, centre + 1 + k))
    # model-space positions; to_bl swaps, so reverse winding to face +Y (up)
    faces = [tuple(reversed(f)) for f in faces]
    return make_object("ground", pos, faces, cols, collection)


def look_at(cam, eye, target, lens):
    eye_b, tgt_b = Vector(to_bl(eye)), Vector(to_bl(target))
    cam.location = eye_b
    cam.rotation_euler = (tgt_b - eye_b).to_track_quat("-Z", "Y").to_euler()
    cam.data.lens = lens
