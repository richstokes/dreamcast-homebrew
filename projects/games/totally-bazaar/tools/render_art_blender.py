"""Render Totally Bazaar's goods icons and title logo with Blender (headless).

Run from the project directory:
    blender --background --factory-startup --python tools/render_art_blender.py \
        -- --output-dir assets/source/renders [--only camel,gold] [--samples 64]

Every object is built procedurally, so renders are reproducible from this
script alone. Output PNGs have transparent backgrounds and are composed into
card faces by tools/build_textures.py.
"""

import argparse
import math
import os
import sys

import bpy
import bmesh
from mathutils import Euler, Vector

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
FONT = "/Applications/Blender.app/Contents/Resources/5.2/datafiles/fonts/Inter.woff2"


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--output-dir", required=True)
    p.add_argument("--only", default="")
    p.add_argument("--samples", type=int, default=96)
    p.add_argument("--size", type=int, default=512)
    return p.parse_args(ARGS)


# ---------------------------------------------------------------- helpers

def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.use_denoising = True
    scene.render.film_transparent = True
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.cycles.seed = 7
    world = bpy.data.worlds.new("World")
    scene.world = world
    world.use_nodes = True
    nt = world.node_tree
    bg = nt.nodes["Background"]
    sky = nt.nodes.new("ShaderNodeTexGradient")
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    coord = nt.nodes.new("ShaderNodeTexCoord")
    mapping = nt.nodes.new("ShaderNodeMapping")
    mapping.inputs["Rotation"].default_value = (0, math.radians(-90), 0)
    nt.links.new(coord.outputs["Generated"], mapping.inputs["Vector"])
    nt.links.new(mapping.outputs["Vector"], sky.inputs["Vector"])
    nt.links.new(sky.outputs["Fac"], ramp.inputs["Fac"])
    ramp.color_ramp.elements[0].color = (0.12, 0.12, 0.16, 1)
    ramp.color_ramp.elements[1].color = (1.0, 0.97, 0.94, 1)
    nt.links.new(ramp.outputs["Color"], bg.inputs["Color"])
    bg.inputs["Strength"].default_value = 0.55
    return scene


def lin(color):
    """Convert an sRGB colour (as authored) to Blender's linear space."""
    def ch(v):
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return tuple(ch(v) for v in color[:3]) + tuple(color[3:])


def material(name, color, metallic=0.0, roughness=0.4, transmission=0.0,
             ior=1.45, coat=0.0, emission=None, subsurface=0.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*lin(color), 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Transmission Weight"].default_value = transmission
    bsdf.inputs["IOR"].default_value = ior
    bsdf.inputs["Coat Weight"].default_value = coat
    if subsurface:
        bsdf.inputs["Subsurface Weight"].default_value = subsurface
    if emission:
        bsdf.inputs["Emission Color"].default_value = (*emission[0], 1.0)
        bsdf.inputs["Emission Strength"].default_value = emission[1]
    return mat


def add_noise_bump(mat, scale=40.0, strength=0.25, detail=6.0):
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = scale
    noise.inputs["Detail"].default_value = detail
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = strength
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    return noise


def assign(obj, mat):
    obj.data.materials.clear()
    obj.data.materials.append(mat)


def smooth(obj, angle=40):
    for poly in obj.data.polygons:
        poly.use_smooth = True
    try:
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.shade_auto_smooth(angle=math.radians(angle))
    except Exception:
        pass


def bevel(obj, width=0.04, segments=3):
    mod = obj.modifiers.new("Bevel", "BEVEL")
    mod.width = width
    mod.segments = segments
    mod.limit_method = "ANGLE"
    return mod


def cube(size, loc, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc, rotation=rot)
    obj = bpy.context.object
    obj.scale = size
    bpy.ops.object.transform_apply(scale=True)
    return obj


def cylinder(radius, depth, loc, rot=(0, 0, 0), verts=64):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=radius,
                                        depth=depth, location=loc,
                                        rotation=rot)
    return bpy.context.object


def sphere(radius, loc, scale=(1, 1, 1), segs=48):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segs, ring_count=segs // 2,
                                         radius=radius, location=loc)
    obj = bpy.context.object
    obj.scale = scale
    bpy.ops.object.transform_apply(scale=True)
    smooth(obj, 180)
    return obj


def light(kind, loc, energy, color=(1, 1, 1), size=2.0, target=(0, 0, 0.6)):
    data = bpy.data.lights.new(kind + str(len(bpy.data.lights)), kind)
    data.energy = energy
    data.color = color
    if kind == "AREA":
        data.size = size
    obj = bpy.data.objects.new(data.name, data)
    bpy.context.collection.objects.link(obj)
    obj.location = loc
    direction = Vector(target) - Vector(loc)
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    return obj


def studio(key=900.0, warm=(1.0, 0.9, 0.78), rim=(0.7, 0.8, 1.0)):
    light("AREA", (3.5, -4.0, 5.0), key, warm, 3.0)
    light("AREA", (-4.5, -2.0, 2.5), key * 0.35, (1.0, 0.75, 0.9), 4.0)
    light("AREA", (-1.0, 5.0, 4.0), key * 0.6, rim, 2.0)


def camera(loc, target, ortho=None, lens=60):
    data = bpy.data.cameras.new("Camera")
    if ortho:
        data.type = "ORTHO"
        data.ortho_scale = ortho
    else:
        data.lens = lens
    cam = bpy.data.objects.new("Camera", data)
    bpy.context.collection.objects.link(cam)
    cam.location = loc
    direction = Vector(target) - Vector(cam.location)
    cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.camera = cam
    return cam


def frame_objects(cam, objs, margin=1.12):
    """Fit an orthographic camera to the objects' bounding box."""
    scene = bpy.context.scene
    bpy.context.view_layer.update()
    inv = cam.matrix_world.inverted()
    xs, ys = [], []
    for obj in objs:
        if obj.type not in {"MESH", "CURVE", "FONT", "META"}:
            continue
        deps = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        for corner in deps.bound_box:
            p = inv @ (obj.matrix_world @ Vector(corner))
            xs.append(p.x)
            ys.append(p.y)
    cx = (min(xs) + max(xs)) / 2
    cy = (min(ys) + max(ys)) / 2
    w = max(xs) - min(xs)
    h = max(ys) - min(ys)
    aspect = scene.render.resolution_x / scene.render.resolution_y
    cam.data.ortho_scale = max(w, h * aspect) * margin
    cam.data.shift_x = cx / cam.data.ortho_scale
    cam.data.shift_y = cy / cam.data.ortho_scale


def render(path, size_x, size_y, samples):
    scene = bpy.context.scene
    scene.render.resolution_x = size_x
    scene.render.resolution_y = size_y
    scene.render.resolution_percentage = 100
    scene.cycles.samples = samples
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("rendered", path)


def ortho_view(objs, elev=24, azim=-35, margin=1.1):
    r = 20.0
    el = math.radians(elev)
    az = math.radians(azim)
    center = Vector((0, 0, 0))
    count = 0
    for o in objs:
        if o.type in {"MESH", "META", "FONT", "CURVE"}:
            center += o.matrix_world.translation
            count += 1
    loc = (r * math.cos(el) * math.sin(az), -r * math.cos(el) * math.cos(az),
           r * math.sin(el))
    cam = camera(loc, (0, 0, 0.5), ortho=4.0)
    frame_objects(cam, objs, margin)
    return cam


# ---------------------------------------------------------------- goods

def gem_mesh(name, radius=1.0, sides=16):
    """A brilliant-cut gem: table, crown and pointed pavilion."""
    bm = bmesh.new()
    table_r = radius * 0.55
    crown_h = radius * 0.32
    pavilion_h = radius * 0.95
    top = [bm.verts.new((table_r * math.cos(a), table_r * math.sin(a), crown_h))
           for a in (2 * math.pi * i / sides for i in range(sides))]
    girdle = [bm.verts.new((radius * math.cos(a), radius * math.sin(a), 0))
              for a in (2 * math.pi * (i + 0.5) / sides for i in range(sides))]
    girdle2 = [bm.verts.new((radius * math.cos(a), radius * math.sin(a), -0.04 * radius))
               for a in (2 * math.pi * (i + 0.5) / sides for i in range(sides))]
    tip = bm.verts.new((0, 0, -pavilion_h))
    bm.faces.new(top)
    for i in range(sides):
        j = (i + 1) % sides
        bm.faces.new((top[i], girdle[i], top[j]))
        bm.faces.new((girdle[i], girdle[j], top[j]))
        bm.faces.new((girdle[i], girdle2[i], girdle2[j], girdle[j]))
        bm.faces.new((girdle2[j], girdle2[i], tip))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def build_diamond():
    ruby = material("Ruby", (0.95, 0.06, 0.16), roughness=0.0,
                    transmission=1.0, ior=1.9)
    pale = material("Diamond", (0.9, 0.95, 1.0), roughness=0.0,
                    transmission=1.0, ior=2.4)
    gold = material("Gold", (1.0, 0.72, 0.25), metallic=1.0, roughness=0.18)
    objs = []
    g = gem_mesh("GemA", 1.0)
    g.location = (0, 0, 1.1)
    g.rotation_euler = (math.radians(-12), math.radians(8), 0.2)
    assign(g, ruby)
    objs.append(g)
    g2 = gem_mesh("GemB", 0.62)
    g2.location = (1.15, -0.55, 0.45)
    g2.rotation_euler = (math.radians(70), 0.3, 0.6)
    assign(g2, pale)
    objs.append(g2)
    g3 = gem_mesh("GemC", 0.5)
    g3.location = (-1.05, -0.35, 0.38)
    g3.rotation_euler = (math.radians(-65), 0.1, -0.4)
    assign(g3, ruby)
    objs.append(g3)
    # Gold ring setting under the main gem.
    bpy.ops.mesh.primitive_torus_add(major_radius=1.02, minor_radius=0.07,
                                     location=(0, 0, 1.1),
                                     rotation=g.rotation_euler)
    ring = bpy.context.object
    smooth(ring, 180)
    assign(ring, gold)
    objs.append(ring)
    studio(1200)
    light("POINT", (0, -2.5, 3.5), 300, (1, 0.9, 0.9))
    return objs, (22, -20)


def build_gold():
    gold = material("Gold", (1.0, 0.66, 0.18), metallic=1.0, roughness=0.16)
    objs = []

    def ingot(loc, rz):
        bm = bmesh.new()
        lb, wb, lt, wt, h = 1.0, 0.5, 0.78, 0.32, 0.36
        pts = [(-lb, -wb, 0), (lb, -wb, 0), (lb, wb, 0), (-lb, wb, 0),
               (-lt, -wt, h), (lt, -wt, h), (lt, wt, h), (-lt, wt, h)]
        v = [bm.verts.new(p) for p in pts]
        for f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5),
                  (2, 3, 7, 6), (3, 0, 4, 7)):
            bm.faces.new([v[i] for i in f])
        mesh = bpy.data.meshes.new("Ingot")
        bm.to_mesh(mesh)
        obj = bpy.data.objects.new("Ingot", mesh)
        bpy.context.collection.objects.link(obj)
        obj.location = loc
        obj.rotation_euler = (0, 0, rz)
        bevel(obj, 0.05, 3)
        assign(obj, gold)
        return obj

    objs.append(ingot((0, -0.55, 0), 0.0))
    objs.append(ingot((0, 0.55, 0), 0.0))
    objs.append(ingot((0, 0, 0.37), math.radians(90)))
    objs.append(ingot((0.1, 0.0, 0.74), math.radians(8)))
    studio(1000)
    return objs, (26, -38)


def build_silver():
    silver = material("Silver", (0.8, 0.86, 0.95), metallic=1.0,
                      roughness=0.12)
    objs = []
    for stack, (x, y, n) in enumerate(((0.0, 0.0, 7), (1.05, -0.35, 4),
                                       (-1.0, -0.25, 5))):
        for i in range(n):
            c = cylinder(0.55, 0.12, (x + 0.02 * math.sin(i * 1.7 + stack),
                                      y + 0.02 * math.cos(i * 2.1),
                                      0.06 + i * 0.125))
            bevel(c, 0.025, 2)
            smooth(c, 50)
            assign(c, silver)
            objs.append(c)
    # A loose coin leaning in front.
    c = cylinder(0.5, 0.1, (0.15, -1.1, 0.42), rot=(math.radians(70), 0, 0.2))
    bevel(c, 0.025, 2)
    smooth(c, 50)
    assign(c, silver)
    objs.append(c)
    studio(1000, rim=(0.6, 0.75, 1.0))
    return objs, (24, -25)


def build_cloth():
    colors = [(0.55, 0.12, 0.75), (0.95, 0.2, 0.55), (0.1, 0.65, 0.75)]
    accents = [(1.0, 0.75, 0.2), (1.0, 0.9, 0.4), (1.0, 0.6, 0.85)]
    objs = []
    placements = [((-0.62, 0, 0.5), 0), ((0.62, 0, 0.5), 1), ((0, 0, 1.36), 2)]
    for loc, idx in placements:
        mat = material("Cloth%d" % idx, colors[idx], roughness=0.65)
        nt = mat.node_tree
        bsdf = nt.nodes["Principled BSDF"]
        bsdf.inputs["Sheen Weight"].default_value = 0.3
        wave = nt.nodes.new("ShaderNodeTexWave")
        wave.inputs["Scale"].default_value = 3.0
        wave.inputs["Distortion"].default_value = 0.0
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        mix.inputs[6].default_value = (*lin(colors[idx]), 1)
        mix.inputs[7].default_value = (*lin(accents[idx]), 1)
        ramp = nt.nodes.new("ShaderNodeValToRGB")
        ramp.color_ramp.elements[0].position = 0.82
        ramp.color_ramp.elements[1].position = 0.86
        nt.links.new(wave.outputs["Fac"], ramp.inputs["Fac"])
        nt.links.new(ramp.outputs["Color"], mix.inputs[0])
        nt.links.new(mix.outputs[2], bsdf.inputs["Base Color"])
        bolt = cylinder(0.5, 2.2, loc, rot=(0, math.radians(90), 0))
        bolt.rotation_euler = (math.radians(90), 0, math.radians(90))
        bevel(bolt, 0.08, 4)
        smooth(bolt, 50)
        assign(bolt, mat)
        objs.append(bolt)
        # A wooden core at the end of each bolt.
        core = cylinder(0.12, 2.36, loc, rot=(math.radians(90), 0, math.radians(90)))
        assign(core, material("Wood", (0.45, 0.25, 0.1), roughness=0.5))
        objs.append(core)
    # A loose draped tail of the top bolt.
    bpy.ops.mesh.primitive_plane_add(size=1, location=(0.0, -0.85, 0.62))
    tail = bpy.context.object
    tail.scale = (0.95, 0.9, 1)
    bpy.ops.object.transform_apply(scale=True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.subdivide(number_cuts=12)
    bpy.ops.object.mode_set(mode="OBJECT")
    for v in tail.data.vertices:
        t = (v.co.y + 0.45) / 0.9
        v.co.z += t * 0.85 + 0.06 * math.sin(v.co.x * 9)
        v.co.y -= (1 - t) * 0.15
    tail.modifiers.new("Solid", "SOLIDIFY").thickness = 0.03
    smooth(tail, 180)
    assign(tail, bpy.data.materials["Cloth2"])
    objs.append(tail)
    studio(950)
    return objs, (20, -30)


def build_spice():
    brass = material("Brass", (0.95, 0.62, 0.25), metallic=1.0, roughness=0.25)
    objs = []

    def bowl(loc, r, powder_color, heap):
        bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32,
                                             radius=r, location=loc)
        b = bpy.context.object
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="DESELECT")
        bpy.ops.object.mode_set(mode="OBJECT")
        for v in b.data.vertices:
            v.select = v.co.z > r * 0.25
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.delete(type="VERT")
        bpy.ops.object.mode_set(mode="OBJECT")
        b.modifiers.new("Solid", "SOLIDIFY").thickness = 0.06
        smooth(b, 180)
        assign(b, brass)
        objs.append(b)
        # Powder heap: a displaced cone.
        bpy.ops.mesh.primitive_cone_add(vertices=64, radius1=r * 0.93,
                                        depth=heap, location=(loc[0], loc[1], loc[2] + r * 0.25 + heap / 2 - 0.05))
        cone = bpy.context.object
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.subdivide(number_cuts=6)
        bpy.ops.object.mode_set(mode="OBJECT")
        for v in cone.data.vertices:
            if v.co.z > -heap / 2 + 0.01:
                f = (v.co.z + heap / 2) / heap
                v.co.x *= 1 + 0.25 * (1 - f) * f
                v.co.y *= 1 + 0.25 * (1 - f) * f
        smooth(cone, 180)
        mat = material("Powder", powder_color, roughness=0.9)
        add_noise_bump(mat, 90, 0.5)
        assign(cone, mat)
        objs.append(cone)

    bowl((0, 0.2, 0.55), 0.95, (0.9, 0.12, 0.02), 0.85)
    bowl((1.25, -0.75, 0.3), 0.55, (1.0, 0.6, 0.0), 0.45)
    bowl((-1.2, -0.7, 0.3), 0.55, (0.25, 0.5, 0.05), 0.4)
    # Cinnamon sticks.
    for i, (x, rz) in enumerate(((0.1, 0.4), (0.3, 0.25))):
        stick = cylinder(0.07, 1.3, (x, -1.25, 0.07 + i * 0.12),
                         rot=(0, math.radians(90), rz), verts=24)
        assign(stick, material("Cinnamon", (0.45, 0.2, 0.08), roughness=0.8))
        objs.append(stick)
    studio(1000)
    return objs, (28, -20)


def build_leather():
    hide = material("Leather", (0.55, 0.28, 0.12), roughness=0.55, coat=0.2)
    nt = hide.node_tree
    noise = add_noise_bump(hide, 25, 0.35)
    # Darker mottling.
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (*lin((0.36, 0.17, 0.07)), 1)
    ramp.color_ramp.elements[1].color = (*lin((0.66, 0.36, 0.16)), 1)
    noise2 = nt.nodes.new("ShaderNodeTexNoise")
    noise2.inputs["Scale"].default_value = 4.0
    nt.links.new(noise2.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], nt.nodes["Principled BSDF"].inputs["Base Color"])
    # Hide outline: a stretched pelt with four leg flaps.
    pts = []
    n = 72
    for i in range(n):
        a = 2 * math.pi * i / n
        r = 1.0 + 0.32 * max(0.0, math.cos(4 * (a - math.pi / 4))) ** 6 \
            + 0.06 * math.sin(7 * a)
        pts.append((r * 1.25 * math.cos(a), r * 0.95 * math.sin(a)))
    bm = bmesh.new()
    verts = [bm.verts.new((x, y, 0)) for x, y in pts]
    bm.faces.new(verts)
    mesh = bpy.data.meshes.new("Hide")
    bm.to_mesh(mesh)
    pelt = bpy.data.objects.new("Hide", mesh)
    bpy.context.collection.objects.link(pelt)
    bpy.context.view_layer.objects.active = pelt
    pelt.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.poke()
    bpy.ops.mesh.subdivide(number_cuts=3)
    bpy.ops.object.mode_set(mode="OBJECT")
    for v in pelt.data.vertices:
        d = math.hypot(v.co.x / 1.25, v.co.y / 0.95)
        v.co.z = 0.25 * math.cos(min(d, 1.4) * 1.1) + 0.05 * math.sin(v.co.x * 5)
    pelt.modifiers.new("Solid", "SOLIDIFY").thickness = 0.05
    pelt.rotation_euler = (math.radians(62), 0, math.radians(6))
    pelt.location = (0, 0.4, 1.1)
    smooth(pelt, 180)
    assign(pelt, hide)
    # A rolled satchel strap and buckle in front.
    bpy.ops.mesh.primitive_torus_add(major_radius=0.55, minor_radius=0.11,
                                     location=(0.6, -0.9, 0.25),
                                     rotation=(math.radians(80), 0, 0.4))
    roll = bpy.context.object
    smooth(roll, 180)
    assign(roll, material("Strap", (0.35, 0.16, 0.06), roughness=0.45, coat=0.3))
    buckle = cube((0.28, 0.06, 0.22), (0.25, -1.3, 0.3), (math.radians(80), 0, 0.4))
    assign(buckle, material("Buckle", (1.0, 0.75, 0.3), metallic=1.0, roughness=0.2))
    bevel(buckle, 0.02, 2)
    studio(1000)
    return [pelt, roll, buckle], (30, -15)


def build_camel():
    fur = material("Camel", (0.88, 0.58, 0.28), roughness=0.7, subsurface=0.05)
    add_noise_bump(fur, 60, 0.1)
    dark = material("Dark", (0.05, 0.03, 0.02), roughness=0.25, coat=0.5)
    blanket = material("Blanket", (0.9, 0.06, 0.25), roughness=0.55)
    trim = material("Trim", (1.0, 0.75, 0.2), metallic=1.0, roughness=0.25)

    parts = []

    def blob(loc, scale, rot=(0, 0, 0)):
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16,
                                             radius=1.0, location=loc,
                                             rotation=rot)
        o = bpy.context.object
        o.scale = scale
        parts.append(o)
        return o

    def limb(a, b, r):
        a = Vector(a)
        b = Vector(b)
        d = b - a
        bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=r,
                                            depth=d.length,
                                            location=(a + b) / 2)
        o = bpy.context.object
        o.rotation_euler = d.to_track_quat("Z", "Y").to_euler()
        parts.append(o)
        blob(tuple(b), (r * 1.15, r * 1.15, r * 1.15))
        return o

    blob((0, 0, 1.5), (1.15, 0.58, 0.55))            # body
    blob((-0.05, 0, 1.9), (0.72, 0.5, 0.62))         # hump
    blob((-0.95, 0, 1.55), (0.5, 0.5, 0.5))          # rump
    blob((0.95, 0, 1.45), (0.45, 0.45, 0.48))        # chest
    limb((1.05, 0, 1.55), (1.45, 0, 2.15), 0.22)     # lower neck
    limb((1.45, 0, 2.15), (1.62, 0, 2.6), 0.18)      # upper neck
    blob((1.85, 0, 2.66), (0.42, 0.21, 0.2), (0, math.radians(12), 0))  # head
    blob((2.18, 0, 2.56), (0.2, 0.16, 0.16))         # muzzle
    for x, y in ((0.75, 0.3), (0.95, -0.3), (-0.85, 0.3), (-0.65, -0.3)):
        limb((x, y, 1.25), (x + 0.03, y, 0.62), 0.15)   # thigh
        limb((x + 0.03, y, 0.62), (x + 0.06, y, 0.1), 0.09)  # shin
        blob((x + 0.12, y, 0.07), (0.17, 0.13, 0.07))   # foot
    limb((-1.38, 0, 1.6), (-1.55, 0, 1.0), 0.05)     # tail

    for o in bpy.context.selected_objects:
        o.select_set(False)
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    body = bpy.context.object
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    rem = body.modifiers.new("Remesh", "REMESH")
    rem.mode = "VOXEL"
    rem.voxel_size = 0.035
    bpy.ops.object.modifier_apply(modifier="Remesh")
    sm = body.modifiers.new("Smooth", "CORRECTIVE_SMOOTH")
    sm.factor = 0.8
    sm.iterations = 12
    sm.use_only_smooth = True
    smooth(body, 180)
    assign(body, fur)
    objs = [body]

    for y in (0.15, -0.15):
        eye = sphere(0.055, (2.02, y, 2.76))
        assign(eye, dark)
        objs.append(eye)
        ear = sphere(0.08, (1.62, y * 1.15, 2.86), (0.6, 0.6, 1.5))
        assign(ear, fur)
        objs.append(ear)
    tuft = sphere(0.09, (-1.56, 0, 0.98), (0.8, 0.8, 1.8))
    assign(tuft, dark)
    objs.append(tuft)

    # Saddle blanket: a shell over the hump that hangs down both flanks.
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=1.0,
                                         location=(-0.05, 0, 1.78))
    cloth = bpy.context.object
    cloth.scale = (0.8, 0.64, 0.82)
    bpy.ops.object.transform_apply(scale=True)
    bm = bmesh.new()
    bm.from_mesh(cloth.data)
    kill = [v for v in bm.verts if abs(v.co.x) > 0.5 or v.co.z < -0.38]
    bmesh.ops.delete(bm, geom=kill, context="VERTS")
    bm.to_mesh(cloth.data)
    bm.free()
    cloth.modifiers.new("Solid", "SOLIDIFY").thickness = 0.05
    smooth(cloth, 180)
    assign(cloth, blanket)
    objs.append(cloth)
    # Gold tassels along the blanket hem.
    for x in (-0.42, -0.21, 0.0, 0.21, 0.42):
        for y in (0.6, -0.6):
            t = sphere(0.06, (x - 0.05, y, 1.42))
            assign(t, trim)
            objs.append(t)
    studio(1100)
    return objs, (12, -10)


def build_logo():
    gold = material("LogoGold", (1.0, 0.68, 0.16), metallic=1.0, roughness=0.2)
    side = material("LogoSide", (0.75, 0.12, 0.35), metallic=0.3, roughness=0.3,
                    coat=1.0)
    font = bpy.data.fonts.load(FONT) if os.path.exists(FONT) else None
    objs = []

    def text(body, loc, size, extrude):
        bpy.ops.object.text_add(location=loc)
        t = bpy.context.object
        t.data.body = body
        if font:
            t.data.font = font
        t.data.size = size
        t.data.extrude = extrude
        t.data.bevel_depth = 0.035
        t.data.bevel_resolution = 4
        t.data.align_x = "CENTER"
        t.data.align_y = "CENTER"
        t.data.space_character = 1.05
        t.data.offset = 0.045  # thicken Inter's strokes into a display weight
        t.data.materials.append(gold)
        t.data.materials.append(side)
        t.rotation_euler = (math.radians(90), 0, 0)
        return t

    title = text("BAZAAR", (0, 0, -0.3), 1.45, 0.22)
    # Inter is variable; ask for the boldest instance if Blender exposes it.
    try:
        title.data.font_bold = font
        for ch in title.data.body_format:
            ch.use_bold = True
    except Exception:
        pass
    sub = text("TOTALLY", (0, 0, 0.82), 0.8, 0.14)
    objs += [title, sub]
    # Convert to mesh so the side material can go on extruded walls.
    for obj in (title, sub):
        bpy.context.view_layer.objects.active = obj
        for o in bpy.context.selected_objects:
            o.select_set(False)
        obj.select_set(True)
        bpy.ops.object.convert(target="MESH")
        mesh = obj.data
        for poly in mesh.polygons:
            n = poly.normal
            poly.material_index = 0 if abs(n.z) > 0.6 else 1
    light("AREA", (2.5, -6.0, 3.0), 1500, (1.0, 0.92, 0.8), 3.0, (0, 0, 0))
    light("AREA", (-4.0, -3.0, -2.0), 700, (1.0, 0.5, 0.8), 4.0, (0, 0, 0))
    light("AREA", (0.0, 4.0, 3.0), 900, (0.6, 0.8, 1.0), 3.0, (0, 0, 0))
    return objs, None


BUILDERS = {
    "diamond": build_diamond,
    "gold": build_gold,
    "silver": build_silver,
    "cloth": build_cloth,
    "spice": build_spice,
    "leather": build_leather,
    "camel": build_camel,
    "logo": build_logo,
}


def main():
    args = parse_args()
    out = os.path.abspath(args.output_dir)
    os.makedirs(out, exist_ok=True)
    only = [s for s in args.only.split(",") if s]
    for name, builder in BUILDERS.items():
        if only and name not in only:
            continue
        reset_scene()
        objs, view = builder()
        if name == "logo":
            cam = camera((0.6, -12, 1.2), (0, 0, 0), ortho=6)
            frame_objects(cam, objs, 1.08)
            render(os.path.join(out, name + ".png"), args.size,
                   args.size // 2, args.samples)
        else:
            if name == "camel":
                cam = camera((7, -13, 4.5), (0.2, 0, 1.4), lens=70)
                cam.data.type = "ORTHO"
                frame_objects(cam, objs, 1.06)
            else:
                ortho_view(objs, *view)
            render(os.path.join(out, name + ".png"), args.size, args.size,
                   args.samples)


main()
