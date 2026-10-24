#!/usr/bin/env python3
"""Model Spiral Canyon's red biplane in Blender and bake it for the game.

Run through Blender, not the host Python interpreter (``make biplane`` does
it):

    blender --background --python tools/build_biplane.py -- \
        --header assets/generated/biplane_mesh.h \
        --blend captures/biplane.blend --preview captures/biplane

A cartoon two-seat biplane in the spirit of the one a certain fox flies:
a red fuselage with a silver cowl and red spinner, staggered red wings on
cream struts, a red tail with a cream rudder, fixed gear and an orange pilot
with pointy ears. The propeller is its own mesh so the game can spin it
about the hub.

Coordinates: the game is y-up with the nose along +z and +x to the right;
Blender gets (x, -z, y) so the scene reads the right way round, and the
export maps it back. Positions go out in millimetres, normals * 127 and
colours as bytes; the game lights the vertices once by their normals.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector

RED = (0.84, 0.10, 0.08, 1.0)
RED_DARK = (0.62, 0.07, 0.06, 1.0)
CREAM = (0.96, 0.90, 0.74, 1.0)
SILVER = (0.78, 0.80, 0.84, 1.0)
GUNMETAL = (0.22, 0.23, 0.26, 1.0)
TYRE = (0.10, 0.10, 0.11, 1.0)
WOOD = (0.36, 0.20, 0.10, 1.0)
YELLOW = (1.0, 0.82, 0.12, 1.0)
GLASS = (0.62, 0.80, 0.92, 1.0)
FUR = (1.0, 0.60, 0.12, 1.0)
WHITE = (1.0, 1.0, 1.0, 1.0)

HUB = (0.0, 0.0, 2.42)   # propeller hub, game space


def b(p):
    """Game (y up) to Blender (z up)."""
    return Vector((p[0], -p[2], p[1]))


class Builder:
    """A bmesh with a colour layer; faces are flat unless asked."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.col = self.bm.loops.layers.color.new("Col")

    def poly(self, pts, col, smooth=False):
        verts = [self.bm.verts.new(b(p)) for p in pts]
        try:
            f = self.bm.faces.new(verts)
        except ValueError:
            return None
        f.smooth = smooth
        for loop in f.loops:
            loop[self.col] = col
        return f

    def prism(self, outline, y0, y1, col, col_side=None, axis="y"):
        """An outline (a convex polygon, anticlockwise seen from +axis)
        extruded from y0 to y1 along the axis: 'y' outlines are (x, z),
        'x' outlines are (z, y)."""
        def p(o, h):
            if axis == "y":
                return (o[0], h, o[1])
            return (h, o[1], o[0])
        n = len(outline)
        top = [p(o, y1) for o in outline]
        bot = [p(o, y0) for o in outline]
        if axis == "y":
            self.poly(list(reversed(top)), col)
            self.poly(bot, col)
        else:
            self.poly(top, col)
            self.poly(list(reversed(bot)), col)
        side = col_side or col
        for i in range(n):
            j = (i + 1) % n
            if axis == "y":
                self.poly([bot[i], bot[j], top[j], top[i]][::-1], side)
            else:
                self.poly([bot[i], bot[j], top[j], top[i]], side)

    def tube(self, c0, c1, r0, r1, sides, col, smooth=False, caps=False, phase=0.0):
        c0, c1 = Vector(c0), Vector(c1)
        axis = (c1 - c0).normalized()
        up = Vector((0, 0, 1)) if abs(axis.y) > 0.9 else Vector((0, 1, 0))
        side = axis.cross(up).normalized()
        up = side.cross(axis).normalized()
        ring0, ring1 = [], []
        for i in range(sides):
            a = i / sides * math.tau + phase
            d = side * math.cos(a) + up * math.sin(a)
            ring0.append(tuple(c0 + d * r0))
            ring1.append(tuple(c1 + d * r1))
        for i in range(sides):
            j = (i + 1) % sides
            self.poly([ring0[i], ring0[j], ring1[j], ring1[i]], col, smooth)
        if caps:
            if r0 > 0.001:
                self.poly(list(reversed(ring0)), col)
            if r1 > 0.001:
                self.poly(ring1, col)

    def sphere(self, c, r, segs, rings, col, squash=(1.0, 1.0, 1.0), col_fn=None):
        """A UV sphere, smooth; col_fn(direction) may colour each face."""
        def pt(i, k):
            th = k / rings * math.pi
            ph = i / segs * math.tau
            d = Vector((math.sin(th) * math.sin(ph), math.cos(th), math.sin(th) * math.cos(ph)))
            return (c[0] + d.x * r * squash[0], c[1] + d.y * r * squash[1], c[2] + d.z * r * squash[2]), d
        for k in range(rings):
            for i in range(segs):
                j = (i + 1) % segs
                quad = [pt(i, k), pt(j, k), pt(j, k + 1), pt(i, k + 1)]
                pts = [q[0] for q in quad]
                if k == 0:
                    pts = [pts[0], pts[2], pts[3]]
                elif k == rings - 1:
                    pts = [pts[0], pts[1], pts[2]]
                mid = sum((q[1] for q in quad), Vector()) / 4
                self.poly(pts[::-1], col_fn(mid) if col_fn else col, smooth=True)

    def finish(self):
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=0.0005)
        bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces)
        self.bm.normal_update()
        me = bpy.data.meshes.new(self.name)
        self.bm.to_mesh(me)
        me.color_attributes.active_color = me.color_attributes["Col"]
        ob = bpy.data.objects.new(self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        return ob


# ---------------------------------------------------------------------------

def rounded_wing(half_span, z_lead, z_trail, round_r=0.32):
    """A wing planform (x, z) with rounded tips, anticlockwise from +y."""
    pts = []
    zc = (z_lead + z_trail) / 2
    hc = (z_lead - z_trail) / 2
    # Right tip: a half ellipse from the trailing edge round to the leading.
    for k in range(6):
        a = -math.pi / 2 + k / 5 * math.pi
        pts.append((half_span - round_r + round_r * math.cos(a), zc + hc * math.sin(a)))
    for k in range(6):
        a = math.pi / 2 + k / 5 * math.pi
        pts.append((-half_span + round_r + round_r * math.cos(a), zc + hc * math.sin(a)))
    return pts


def build_body():
    bd = Builder("biplane")
    # Fuselage: lofted rings (z, centre y, half width, half height), the
    # cowl silver from the firewall forward.
    secs = [(-3.45, 0.34, 0.05, 0.07), (-2.7, 0.27, 0.22, 0.27), (-1.6, 0.15, 0.40, 0.45),
            (-0.5, 0.06, 0.54, 0.57), (0.6, 0.02, 0.60, 0.62), (1.45, 0.0, 0.62, 0.62),
            (2.05, 0.0, 0.60, 0.60), (2.22, 0.0, 0.48, 0.48)]
    sides = 12
    rings = []
    for z, cy, rx, ry in secs:
        rings.append([(math.sin(i / sides * math.tau) * rx, cy + math.cos(i / sides * math.tau) * ry, z)
                      for i in range(sides)])
    for s in range(len(secs) - 1):
        col = SILVER if secs[s][0] >= 1.45 else RED
        for i in range(sides):
            j = (i + 1) % sides
            c = col
            # A cream stripe along each flank, the belly a shade darker.
            if col is RED and i in (3, 9) and secs[s][0] < 1.0:
                c = CREAM
            elif col is RED and 4 <= i <= 7:
                c = RED_DARK
            bd.poly([rings[s][j], rings[s][i], rings[s + 1][i], rings[s + 1][j]], c, smooth=True)
    bd.poly(rings[0], RED)
    # The engine face behind the propeller and the red spinner.
    bd.poly(list(reversed(rings[-1])), GUNMETAL)
    bd.tube((0, 0, 2.20), (0, 0, HUB[2] + 0.42), 0.24, 0.0, 10, RED, smooth=True)
    # Exhaust stubs either side of the cowl.
    for sx in (-1, 1):
        bd.tube((sx * 0.56, -0.20, 1.65), (sx * 0.70, -0.30, 1.25), 0.07, 0.07, 5, GUNMETAL, caps=True)

    # Cockpit: a dark coaming ring, a windscreen and the pilot.
    bd.prism([(0.30, -0.95), (0.36, -0.55), (0.30, -0.15), (-0.30, -0.15), (-0.36, -0.55), (-0.30, -0.95)],
             0.56, 0.64, GUNMETAL)
    bd.poly([(-0.28, 0.62, -0.05), (0.28, 0.62, -0.05), (0.24, 0.92, -0.20), (-0.24, 0.92, -0.20)], GLASS)
    head = (0.0, 1.02, -0.62)

    def fur(d):
        # A white muzzle low on the front of the face, dark goggles above it.
        if d.z > 0.55 and d.y < 0.05:
            return WHITE
        if d.z > 0.45 and 0.15 < d.y < 0.5:
            return GUNMETAL
        return FUR
    bd.sphere(head, 0.36, 10, 6, FUR, col_fn=fur)
    for sx in (-1, 1):
        bd.tube((sx * 0.16, 1.20, -0.66), (sx * 0.40, 1.68, -0.74), 0.15, 0.0, 4, FUR, phase=math.pi / 4)
    bd.sphere((0.0, 0.70, -0.62), 0.30, 8, 4, FUR, squash=(1.0, 0.6, 0.9))   # shoulders

    # Wings: the upper one staggered forward over the cockpit.
    up_y, lo_y, t = 1.50, -0.32, 0.075
    bd.prism(rounded_wing(4.0, 1.45, 0.10), up_y - t, up_y + t, RED)
    bd.prism(rounded_wing(3.6, 1.20, -0.05), lo_y - t, lo_y + t, RED)
    # Interplane struts (an N each side) and the cabane struts.
    for sx in (-1, 1):
        x = sx * 2.75
        bd.tube((x, lo_y, 1.0), (x, up_y, 1.22), 0.05, 0.05, 4, CREAM, smooth=True)
        bd.tube((x, lo_y, 0.15), (x, up_y, 0.38), 0.05, 0.05, 4, CREAM, smooth=True)
        bd.tube((x, lo_y, 0.15), (x, up_y, 1.22), 0.035, 0.035, 4, CREAM, smooth=True)
        for z in (0.45, 1.15):
            bd.tube((sx * 0.30, 0.45, z), (sx * 0.55, up_y, z + 0.08), 0.04, 0.04, 4, CREAM, smooth=True)

    # Tail: stabiliser, fin and a cream rudder.
    bd.prism([(1.35, -3.30), (1.45, -2.95), (0.40, -2.35), (-0.40, -2.35), (-1.45, -2.95), (-1.35, -3.30)],
             0.26, 0.32, RED)
    bd.prism([(-3.05, 0.36), (-2.35, 0.36), (-2.95, 1.10), (-3.12, 1.18)], -0.04, 0.04, RED, axis="x")
    bd.prism([(-3.55, 0.30), (-3.05, 0.30), (-3.12, 1.18), (-3.40, 1.22), (-3.58, 1.00)], -0.035, 0.035, CREAM,
             axis="x")

    # Landing gear: V legs to the axle, two wheels and a tail skid.
    ax_y, ax_z = -1.22, 1.05
    for sx in (-1, 1):
        foot = (sx * 0.80, ax_y, ax_z)
        bd.tube((sx * 0.28, -0.40, 1.35), foot, 0.045, 0.045, 4, GUNMETAL, smooth=True)
        bd.tube((sx * 0.28, -0.40, 0.55), foot, 0.045, 0.045, 4, GUNMETAL, smooth=True)
        bd.tube((sx * 0.72, ax_y, ax_z), (sx * 0.98, ax_y, ax_z), 0.36, 0.36, 10, TYRE, smooth=True, caps=True)
        # Silver hubs just proud of each side wall.
        for x in (0.69, 1.01):
            hub = []
            for i in range(6):
                a = i / 6 * math.tau
                hub.append((sx * x, ax_y + math.cos(a) * 0.17, ax_z + math.sin(a) * 0.17))
            bd.poly(hub if (x > 0.8) == (sx > 0) else list(reversed(hub)), SILVER)
    bd.tube((0, 0.22, -3.1), (0, -0.12, -3.38), 0.04, 0.03, 4, GUNMETAL)
    return bd.finish()


def build_prop():
    """Two wooden blades with yellow tips about the hub (local origin),
    their length along y, twisted, faces towards +z."""
    bd = Builder("biplane_prop")
    for sgn in (-1, 1):
        stations = [(0.16, 0.10, 0.50), (0.55, 0.16, 0.35), (0.95, 0.14, 0.22), (1.05, 0.13, 0.20),
                    (1.25, 0.08, 0.15)]
        prev = None
        for k, (r, w, twist) in enumerate(stations):
            ca, sa = math.cos(twist), math.sin(twist)
            # Chord across x, tilted about the blade axis; 0.03 thick.
            pts = [(-w * ca - 0.03 * sa, r * sgn, -w * sa + 0.03 * ca), (w * ca - 0.03 * sa, r * sgn, w * sa + 0.03 * ca),
                   (w * ca + 0.03 * sa, r * sgn, w * sa - 0.03 * ca), (-w * ca + 0.03 * sa, r * sgn, -w * sa - 0.03 * ca)]
            if prev:
                col = YELLOW if r > 1.0 else WOOD
                for i in range(4):
                    j = (i + 1) % 4
                    q = [prev[i], prev[j], pts[j], pts[i]]
                    bd.poly(q if sgn > 0 else q[::-1], col)
            prev = pts
        bd.poly(prev if sgn > 0 else prev[::-1], YELLOW)
    return bd.finish()


# ---------------------------------------------------------------------------
# Export

def export(ob, f, prefix):
    me = ob.data
    me.calc_loop_triangles()
    col_layer = me.color_attributes["Col"].data
    key_to_out = {}
    verts = []
    tris = []

    def out_vert(poly, li):
        vi = me.loops[li].vertex_index
        co = me.vertices[vi].co
        n = me.vertices[vi].normal if poly.use_smooth else poly.normal
        c = col_layer[li].color
        key = ((round(co.x * 1000), round(co.z * 1000), round(-co.y * 1000)),
               (round(n.x * 127), round(n.z * 127), round(-n.y * 127)),
               tuple(round(min(1.0, max(0.0, c[k])) * 255) for k in range(3)))
        if key not in key_to_out:
            key_to_out[key] = len(verts)
            verts.append(key)
        return key_to_out[key]

    for t in me.loop_triangles:
        poly = me.polygons[t.polygon_index]
        tris.append(tuple(out_vert(poly, li) for li in t.loops))
    if len(verts) > 1024:
        sys.exit(f"{prefix}: {len(verts)} vertices, over the game's 1024 per mesh")
    print(f"biplane {prefix}: {len(verts)} verts, {len(tris)} tris")
    f.write(f"/* {prefix}: {len(verts)} vertices, {len(tris)} triangles */\n")
    f.write(f"static const dk_plane_vert_t {prefix}_verts[{len(verts)}] = {{\n")
    for pos, n, c in verts:
        f.write(f"{{{pos[0]},{pos[1]},{pos[2]},{n[0]},{n[1]},{n[2]},{c[0]},{c[1]},{c[2]}}},\n")
    f.write(f"}};\nstatic const uint16_t {prefix}_index[{len(tris) * 3}] = {{\n")
    flat = [i for t in tris for i in t]
    for k in range(0, len(flat), 24):
        f.write(",".join(str(v) for v in flat[k:k + 24]) + ",\n")
    f.write("};\n\n")


def preview(objects, out_dir: Path):
    """Workbench renders from a few angles, vertex colours on."""
    out_dir.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "VERTEX"
    scene.render.resolution_x, scene.render.resolution_y = 640, 480
    world = bpy.data.worlds.new("w")
    world.color = (0.55, 0.62, 0.72)
    scene.world = world
    cam_data = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    c = Vector((0, 0.3, 0.2))
    for name, d in (("three-quarter", (1.0, -1.3, 0.7)), ("side", (1.0, 0.0, 0.05)),
                    ("front", (0.05, -1.0, 0.15)), ("above-behind", (-0.4, 1.2, 0.9)),
                    ("below", (0.6, -0.6, -0.8))):
        cam.location = c + Vector(d).normalized() * 13.0
        cam.rotation_euler = (c - cam.location).to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = str(out_dir / f"{name}.png")
        bpy.ops.render.render(write_still=True)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--blend", type=Path)
    ap.add_argument("--preview", type=Path)
    a = ap.parse_args(argv)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    body = build_body()
    prop = build_prop()
    prop.location = b(HUB)
    if a.blend:
        a.blend.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(a.blend.resolve()))
    if a.preview:
        preview([body, prop], a.preview)
    a.header.parent.mkdir(parents=True, exist_ok=True)
    with a.header.open("w") as f:
        f.write("/* Generated by tools/build_biplane.py (Blender). Do not edit. */\n")
        f.write("#ifndef DK_BIPLANE_MESH_H\n#define DK_BIPLANE_MESH_H\n#include <stdint.h>\n")
        f.write("/* Position in millimetres (nose +z, up +y, right +x), normal * 127, colour. */\n")
        f.write("typedef struct { int16_t x, y, z; int8_t nx, ny, nz; uint8_t r, g, b; } dk_plane_vert_t;\n")
        f.write("/* The propeller spins about +z through its hub (the prop mesh's origin). */\n")
        f.write(f"#define DK_PLANE_HUB_Z_MM {round(HUB[2] * 1000)}\n\n")
        export(body, f, "dk_plane_body")
        export(prop, f, "dk_plane_prop")
        f.write("#endif\n")


main()
