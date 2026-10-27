"""Shared by the Blender prop bakers (tools/build_tsudoken_props.py,
tools/build_apple_valley_props.py): a bmesh builder in game coordinates, the
strip exporter, the preview renders and the header writer. Each prop also
carries a few solid boxes for the spectator's cameras to keep clear of.

Coordinates: the game is y-up; Blender gets (x, -z, y) so the scene reads the
right way round in the viewport, and the export maps it back. Positions are
exported in centimetres (or millimetres of a unit model the game scales up),
normals * 127, UVs in 1/256 and colours as bytes.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stripify import stripify  # noqa: E402

WHITE = (1.0, 1.0, 1.0, 1.0)


def b(x, y, z):
    """Game (y up) to Blender (z up)."""
    return Vector((x, -z, y))


def rgba(r, g, b_, a=255):
    return (r / 255.0, g / 255.0, b_ / 255.0, a / 255.0)


def mix(c0, c1, t):
    return tuple(c0[i] + (c1[i] - c0[i]) * t for i in range(4))


def scale_col(c, s):
    return (min(1.0, c[0] * s), min(1.0, c[1] * s), min(1.0, c[2] * s), c[3])


SOLIDS = {}   # each prop's Builder.solids, by name


class Builder:
    """A bmesh with a UV layer, a colour layer and the fixed material slots
    (a subclass sets MATERIALS, in the game's enum order)."""

    MATERIALS: list[str] = []

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UV")
        self.col = self.bm.loops.layers.color.new("Col")
        # Solid boxes the spectator's cameras keep clear of, (lo, hi) in
        # game metres; None means one box round the whole prop.
        self.solids = None

    def solid(self, lo, hi):
        self.solids = (self.solids or []) + [(lo, hi)]

    def bounds(self):
        """The prop's extents so far, (lo, hi) in game coordinates."""
        pts = [(v.co.x, v.co.z, -v.co.y) for v in self.bm.verts]
        return tuple(min(p[a] for p in pts) for a in range(3)), tuple(max(p[a] for p in pts) for a in range(3))

    # -- primitives (game coordinates) --------------------------------------

    def poly(self, pts, mat, uvs=None, cols=None, smooth=False):
        """A face over game-space points; uvs per point (or none), cols per
        point or one colour. Faces are built both ways round when the game
        draws them (no culling), so winding only sets the normal."""
        verts = [self.bm.verts.new(b(*p)) for p in pts]
        try:
            f = self.bm.faces.new(verts)
        except ValueError:
            return None
        f.material_index = self.MATERIALS.index(mat)
        f.smooth = smooth
        if cols is None:
            cols = [WHITE] * len(pts)
        elif isinstance(cols[0], (int, float)):
            cols = [cols] * len(pts)
        for i, loop in enumerate(f.loops):
            loop[self.uv].uv = uvs[i] if uvs else (0.0, 0.0)
            loop[self.col] = cols[i]
        return f

    def quad(self, a, b_, c, d, mat, uv=((0, 0), (1, 0), (1, 1), (0, 1)), col=WHITE, smooth=False):
        return self.poly([a, b_, c, d], mat, list(uv), col, smooth)

    def box(self, c, half, yaw, mat, col=WHITE, uv_scale=1.0, top=True, bottom=False, cols_by_face=None):
        """An upright box: centre, half extents (x, y, z), yaw about y."""
        ax = Vector((math.cos(yaw), 0, -math.sin(yaw))) * half[0]
        az = Vector((math.sin(yaw), 0, math.cos(yaw))) * half[2]
        ay = Vector((0, half[1], 0))
        cv = Vector(c)
        p = [cv + ax * (1 if i & 1 else -1) + ay * (1 if i & 2 else -1) + az * (1 if i & 4 else -1) for i in range(8)]
        ux, uy, uz = half[0] * 2 * uv_scale, half[1] * 2 * uv_scale, half[2] * 2 * uv_scale
        faces = {
            "front": ((0, 1, 3, 2), ux, uy),   # -z
            "back": ((5, 4, 6, 7), ux, uy),    # +z
            "left": ((4, 0, 2, 6), uz, uy),    # -x
            "right": ((1, 5, 7, 3), uz, uy),   # +x
        }
        if top:
            faces["top"] = ((2, 3, 7, 6), ux, uz)
        if bottom:
            faces["bottom"] = ((4, 5, 1, 0), ux, uz)
        for name, (idx, uw, vh) in faces.items():
            fc = (cols_by_face or {}).get(name, col)
            self.poly([tuple(p[i]) for i in idx], mat, [(0, vh), (uw, vh), (uw, 0), (0, 0)], fc)

    def tube(self, c0, c1, r0, r1, sides, mat, col0=WHITE, col1=None, u_rep=1.0, v_rep=1.0, smooth=True,
             phase=0.0, axis_up=None):
        """A tapered tube from centre c0 (radius r0) to c1 (radius r1)."""
        col1 = col1 or col0
        c0, c1 = Vector(c0), Vector(c1)
        axis = (c1 - c0).normalized()
        up = Vector(axis_up) if axis_up else (Vector((0, 0, 1)) if abs(axis.y) > 0.9 else Vector((0, 1, 0)))
        side = axis.cross(up).normalized()
        up = side.cross(axis).normalized()
        ring0, ring1 = [], []
        for i in range(sides):
            a = i / sides * math.tau + phase
            d = side * math.cos(a) + up * math.sin(a)
            ring0.append(c0 + d * r0)
            ring1.append(c1 + d * r1)
        for i in range(sides):
            j = (i + 1) % sides
            u0, u1 = i / sides * u_rep, (i + 1) / sides * u_rep
            self.poly([tuple(ring0[i]), tuple(ring0[j]), tuple(ring1[j]), tuple(ring1[i])], mat,
                      [(u0, v_rep), (u1, v_rep), (u1, 0), (u0, 0)], [col0, col0, col1, col1], smooth)
        return ring0, ring1

    def disc(self, c, r, sides, mat, col=WHITE, phase=0.0, flip=False, uv_scale=0.0):
        pts = []
        for i in range(sides):
            a = i / sides * math.tau + phase
            pts.append((c[0] + math.sin(a) * r, c[1], c[2] + math.cos(a) * r))
        if flip:
            pts.reverse()
        uvs = [((p[0] - c[0]) * uv_scale, (p[2] - c[2]) * uv_scale) for p in pts] if uv_scale else None
        return self.poly(pts, mat, uvs, col)

    def finish(self):
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=0.0005)
        self.bm.normal_update()
        me = bpy.data.meshes.new(self.name)
        for name in self.MATERIALS:
            me.materials.append(bpy.data.materials.get(name) or bpy.data.materials.new(name))
        self.bm.to_mesh(me)
        me.color_attributes.active_color = me.color_attributes["Col"]
        ob = bpy.data.objects.new(self.name, me)
        SOLIDS[self.name] = self.solids
        bpy.context.scene.collection.objects.link(ob)
        return ob


# ---------------------------------------------------------------------------
# Export

def export(ob, f, prefix, cm, label):
    me = ob.data
    me.calc_loop_triangles()
    uv_layer = me.uv_layers["UV"].data
    col_layer = me.color_attributes["Col"].data
    key_to_out = {}
    verts = []
    per_mat = {}
    def out_vert(poly, li):
        vi = me.loops[li].vertex_index
        co = me.vertices[vi].co
        n = me.vertices[vi].normal if poly.use_smooth else poly.normal
        u, v = uv_layer[li].uv
        c = col_layer[li].color
        x, y, z = co.x, co.z, -co.y
        nx, ny, nz = n.x, n.z, -n.y
        if cm:
            pos = (round(x * 100), round(y * 100), round(z * 100))
        else:
            pos = (round(x * 1000), round(y * 1000), round(z * 1000))
        key = (pos, (round(nx * 127), round(ny * 127), round(nz * 127)), (round(u * 256), round(v * 256)),
               tuple(round(min(1.0, max(0.0, c[k])) * 255) for k in range(4)), poly.material_index)
        if key not in key_to_out:
            key_to_out[key] = len(verts)
            verts.append(key)
        return key_to_out[key]
    for t in me.loop_triangles:
        poly = me.polygons[t.polygon_index]
        tri = tuple(out_vert(poly, li) for li in t.loops)
        per_mat.setdefault(poly.material_index, []).append(tri)
    for pos, _, uv, _, _ in verts:
        if max(abs(v) for v in pos) > 32767 or max(abs(v) for v in uv) > 32767:
            sys.exit(f"{prefix}: a position or UV out of int16 range")
    index, strip_len, strip_mat = [], [], []
    for mat in sorted(per_mat):
        for seq, _ in stripify(per_mat[mat], True):
            # Long strips are cut so the game's submit buffer always fits;
            # each piece repeats the two vertices before it.
            at = 0
            while at < len(seq):
                piece = seq[at:at + 240]
                if len(piece) < 3:
                    break
                strip_len.append(len(piece))
                strip_mat.append(mat)
                index.extend(piece)
                at += 238
    # Solid boxes in the vertices' units: the prop's own, or one round it.
    per_m = 100 if cm else 1000
    solids = SOLIDS[ob.name]
    if solids is None:
        solids = [tuple(tuple(fn(v[0][a] for v in verts) / per_m for a in range(3)) for fn in (min, max))]
    boxes = [tuple(round(lo[a] * per_m) for a in range(3)) + tuple(round(hi[a] * per_m) for a in range(3)) for lo, hi in solids]
    for bx in boxes:
        if max(abs(v) for v in bx) > 32767 or any(bx[a] > bx[a + 3] for a in range(3)):
            sys.exit(f"{prefix}: a solid box out of int16 range or inside out")
    tris = sum(len(v) for v in per_mat.values())
    print(f"{label} {prefix}: {len(verts)} verts, {tris} tris, {len(strip_len)} strips "
          f"({len(index) / max(1, len(strip_len)):.1f} verts each), {len(index)} strip verts")
    P = prefix
    f.write(f"/* {prefix}: {len(verts)} vertices, {tris} triangles */\n")
    f.write(f"static const dk_prop_vert_t {P}_verts[{len(verts)}] = {{\n")
    for pos, n, uv, c, mat in verts:
        f.write(f"{{{pos[0]},{pos[1]},{pos[2]},{n[0]},{n[1]},{n[2]},{mat},{uv[0]},{uv[1]},{c[0]},{c[1]},{c[2]},{c[3]}}},\n")
    f.write(f"}};\nstatic const uint16_t {P}_index[{len(index)}] = {{\n")
    for k in range(0, len(index), 24):
        f.write(",".join(str(v) for v in index[k:k + 24]) + ",\n")
    f.write(f"}};\nstatic const uint8_t {P}_strip_len[{len(strip_len)}] = {{\n")
    for k in range(0, len(strip_len), 32):
        f.write(",".join(str(v) for v in strip_len[k:k + 32]) + ",\n")
    f.write(f"}};\nstatic const uint8_t {P}_strip_mat[{len(strip_mat)}] = {{\n")
    for k in range(0, len(strip_mat), 32):
        f.write(",".join(str(v) for v in strip_mat[k:k + 32]) + ",\n")
    f.write("};\n")
    if boxes:
        f.write(f"static const dk_prop_box_t {P}_solids[{len(boxes)}] = {{\n")
        for bx in boxes:
            f.write("{" + ",".join(str(v) for v in bx) + "},\n")
        f.write("};\n")
    f.write("\n")
    solids_ref = f"{P}_solids" if boxes else "0"
    return len(index), (f"{{{P}_verts, {P}_index, {P}_strip_len, {P}_strip_mat, {solids_ref}, {len(verts)}, {len(strip_len)}, "
                        f"{len(boxes)}, {int(cm)}}}")


def preview(objects, out_dir: Path):
    """A Workbench render of each prop from a three-quarter view, vertex
    colours on, so the models can be looked at without the game."""
    out_dir.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "VERTEX"
    scene.render.resolution_x, scene.render.resolution_y = 640, 480
    scene.render.film_transparent = False
    cam_data = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    for ob in objects:
        for o in objects:
            o.hide_render = o is not ob
        world = [ob.matrix_world @ v.co for v in ob.data.vertices]
        xs = [v.x for v in world]
        ys = [v.y for v in world]
        zs = [v.z for v in world]
        c = Vector(((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2))
        size = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
        d = Vector((1.0, -1.4, 0.9)).normalized() * size * 1.9
        cam.location = c + d
        direction = c - cam.location
        cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
        cam_data.clip_end = size * 10 + 10
        scene.render.filepath = str(out_dir / f"{ob.name}.png")
        bpy.ops.render.render(write_still=True)
    for o in objects:
        o.hide_render = False


def bake(props, materials, pt_materials, header: Path, blend: Path | None, preview_dir: Path | None, *,
         script, guard, label, unit_note, mat_prefix, prop_prefix, table):
    """Save the scene, render the previews and write the header: each
    prop's strips, the material and prop enums and the table. props is a
    list of (prefix, object, cm)."""
    x = 0.0
    for _, ob, _ in props:
        ob.location.x = x
        x += max(v.co.x for v in ob.data.vertices) - min(v.co.x for v in ob.data.vertices) + 6.0
    if blend:
        blend.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(blend.resolve()))
    if preview_dir:
        preview([ob for _, ob, _ in props], preview_dir)
    for _, ob, _ in props:
        ob.location.x = 0.0
    total = 0
    rows = []
    header.parent.mkdir(parents=True, exist_ok=True)
    M = mat_prefix
    with header.open("w") as f:
        f.write(f"/* Generated by {script} (Blender). Do not edit. */\n")
        f.write(f"#ifndef {guard}\n#define {guard}\n#include <stdint.h>\n")
        f.write(f"/* Position in centimetres ({unit_note}),\n"
                f"   normal * 127, material ({M}_*), texture u v in 1/256, colour. */\n")
        f.write("#ifndef DK_PROP_TYPES\n#define DK_PROP_TYPES\n")
        f.write("typedef struct { int16_t x, y, z; int8_t nx, ny, nz; uint8_t mat; int16_t u, v; uint8_t r, g, b, a; } dk_prop_vert_t;\n")
        f.write("/* A solid box in the vertices' units, before the prop is turned: the\n"
                "   spectator's cameras keep clear of them and their sight lines. */\n")
        f.write("typedef struct { int16_t x0, y0, z0, x1, y1, z1; } dk_prop_box_t;\n")
        f.write("typedef struct { const dk_prop_vert_t *verts; const uint16_t *index; const uint8_t *strip_len, *strip_mat;\n"
                "                 const dk_prop_box_t *solids; uint16_t nverts, nstrips; uint8_t nsolids, cm; } dk_prop_t;\n")
        f.write("#endif\n")
        f.write("enum { " + ", ".join(f"{M}_{m.upper()}" for m in materials) + f", {M}_COUNT }};\n")
        f.write("/* The materials a punch-through (alpha tested) list draws: " + ", ".join(sorted(pt_materials)) + ". */\n")
        f.write(f"#define {M}_PT_MASK " + " | ".join(f"(1u << {M}_{m.upper()})" for m in sorted(pt_materials)) + "\n\n")
        for prefix, ob, cm in props:
            n, row = export(ob, f, prefix, cm, label)
            total += n
            rows.append((prefix, row))
        f.write("enum { " + ", ".join(p.upper() for p, _ in rows) + f", {prop_prefix}_COUNT }};\n")
        f.write(f"static const dk_prop_t {table}[{prop_prefix}_COUNT] = {{\n")
        for prefix, row in rows:
            f.write(f"    {row},   /* {prefix} */\n")
        f.write("};\n#endif\n")
    print(f"{label}s: {total} strip vertices in all, header {header}")
