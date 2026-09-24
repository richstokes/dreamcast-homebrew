"""Render the pedestrian preview OBJ files written by tools/build_pedestrians.py.

Run: blender --background --python tools/preview_pedestrians_blender.py -- \
        --input-dir <preview dir> --output <contact-sheet.png>

Every variant OBJ is imported with its pre-tinted atlas and rendered with the
Workbench engine in textured mode from a front three-quarter view and a rear
view, so the same geometry and texels the Dreamcast draws can be judged on the
host without an emulator capture.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(argv)


def clear_scene() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)


def point_camera(camera: bpy.types.Object, target: Vector) -> None:
    direction = target - camera.location
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def import_variants(directory: Path) -> int:
    count = 0
    for path in sorted(directory.glob("variant*.obj")):
        # The OBJ is authored y-up, z-forward (game space); Blender's importer
        # turns that into z-up with the models facing -Y.
        bpy.ops.wm.obj_import(filepath=str(path), forward_axis="NEGATIVE_Z", up_axis="Y")
        count += 1
    for material in bpy.data.materials:
        if not material.use_nodes:
            continue
        for node in material.node_tree.nodes:
            if node.type == "TEX_IMAGE" and node.image:
                node.image.colorspace_settings.name = "sRGB"
                node.interpolation = "Linear"
    return count


def render(output: Path, count: int) -> None:
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.studio_light = "paint.sl"
    scene.display.shading.color_type = "TEXTURE"
    scene.display.shading.show_shadows = True
    scene.display.shading.show_cavity = False
    scene.render.film_transparent = False
    scene.world = bpy.data.worlds.new("Preview world")
    scene.world.color = (.10, .11, .16)
    scene.render.image_settings.file_format = "PNG"
    scene.render.resolution_percentage = 100
    camera_data = bpy.data.cameras.new("Preview camera")
    camera_data.type = "ORTHO"
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera
    width = count * .9
    centre = Vector((width * .5 - .45, 0.0, .95))
    views = (
        ("front", Vector((centre.x - 2.0, -9.0, 2.4))),
        ("rear", Vector((centre.x + 2.0, 9.0, 2.4))),
    )
    frames = []
    for name, location in views:
        camera.location = location
        camera_data.ortho_scale = width + .8
        point_camera(camera, centre)
        scene.render.resolution_x = 1800
        scene.render.resolution_y = 620
        frame = output.with_name(f"{output.stem}-{name}.png")
        scene.render.filepath = str(frame)
        bpy.ops.render.render(write_still=True)
        frames.append(frame)
    # A close portrait of the first two figures checks the face and clothing texels.
    camera.location = Vector((.45, -4.0, 1.35))
    camera_data.ortho_scale = 2.2
    point_camera(camera, Vector((.45, 0.0, 1.05)))
    scene.render.resolution_x = 1200
    scene.render.resolution_y = 900
    frame = output.with_name(f"{output.stem}-closeup.png")
    scene.render.filepath = str(frame)
    bpy.ops.render.render(write_still=True)
    frames.append(frame)
    print("rendered: " + ", ".join(str(f) for f in frames))


def main() -> None:
    args = parse_args()
    clear_scene()
    count = import_variants(args.input_dir.resolve())
    if count == 0:
        raise SystemExit("no variant OBJ files found")
    render(args.output.resolve(), count)


if __name__ == "__main__":
    main()
