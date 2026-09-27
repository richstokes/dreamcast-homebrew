#!/usr/bin/env python3
"""Build and export Drift Los Angeles's C7 Corvette player car with Blender.

Run through Blender, not the host Python interpreter:

    blender --background --python tools/build_car_blender.py -- \
        --header model_data.h --preview-dir assets/generated/previews/blender-car

The body is one authored longitudinal loft whose cross-sections carry the
wheel openings, wheel wells, fender haunches, rocker and the cabin tub, so the
silhouette is designed rather than left to Boolean cuts.  The greenhouse is a
second loft sharing the body's shoulder, with the A-pillars, roof, B-pillar and
sail panels assigned per quad, so pillars are part of the surface instead of
floating sticks.  Trim, lamps and seams are snapped onto the finished surface.
Surface charts, split normals and local ambient occlusion are baked into the
export, so the game uses the same material boundaries as the Blender mesh.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector
from mathutils.bvhtree import BVHTree


PAINT = 0
GLASS = 1
CARBON = 2
LIGHTS = 3
METAL = 4
MAX_EXPORT_VERTICES = 4096

MATERIAL_IDS = {
    "DLA Paint": PAINT,
    "DLA Glass": GLASS,
    "DLA Carbon": CARBON,
    "DLA Lights": LIGHTS,
    "DLA Headlamp": LIGHTS,
    "DLA Lamp Lens": LIGHTS,
    "DLA Metal": METAL,
}

# Axles stay at the game's 3.10-unit wheelbase; body and staggered tires
# are scaled from C7 dimensions. Keep the preview and runtime wheel rigs aligned.
FRONT_AXLE_Y = 1.55
REAR_AXLE_Y = -1.55
FRONT_ARCH_Z = .400
REAR_ARCH_Z = .415
FRONT_ARCH_R = .430
REAR_ARCH_R = .445
FLOOR_Z = .115
TUB_Z = .46
TUB_HALF = .42
TUB_FRONT_Y = .58
TUB_REAR_Y = -1.55


def parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--preview-dir", type=Path)
    parser.add_argument("--blend", type=Path)
    return parser.parse_args(argv)


def clear_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for datablocks in (bpy.data.meshes, bpy.data.curves, bpy.data.materials,
                       bpy.data.cameras, bpy.data.lights):
        for datablock in list(datablocks):
            if datablock.users == 0:
                datablocks.remove(datablock)


def make_material(name: str, color: tuple[float, float, float, float],
                  metallic: float = 0.0, roughness: float = .45) -> bpy.types.Material:
    material = bpy.data.materials.new(name)
    material.diffuse_color = color
    material.metallic = metallic
    material.roughness = roughness
    return material


def apply_modifier(obj: bpy.types.Object, modifier: bpy.types.Modifier) -> None:
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    obj.select_set(False)


def smooth_mesh(obj: bpy.types.Object) -> None:
    for polygon in obj.data.polygons:
        polygon.use_smooth = True


def link_mesh(name: str, vertices: list[tuple[float, float, float]],
              faces: list[tuple[int, ...]], materials: list[bpy.types.Material],
              face_materials: list[int] | None = None,
              smooth: bool = True) -> bpy.types.Object:
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    for material in materials:
        mesh.materials.append(material)
    if face_materials is not None:
        for polygon, index in zip(mesh.polygons, face_materials):
            polygon.material_index = index
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    obj["export_kos"] = True
    bpy.context.collection.objects.link(obj)
    if smooth:
        smooth_mesh(obj)
    return obj


def orient_outward(obj: bpy.types.Object, axis_z: float) -> None:
    """Flip any face whose normal points toward the loft's long axis."""
    editable = bmesh.new()
    editable.from_mesh(obj.data)
    flipped = []
    for face in editable.faces:
        centre = face.calc_center_median()
        outward = Vector((centre.x, 0.0, centre.z - axis_z))
        if outward.length_squared > 1e-9 and face.normal.dot(outward) < 0.0:
            flipped.append(face)
    if flipped:
        bmesh.ops.reverse_faces(editable, faces=flipped)
    editable.to_mesh(obj.data)
    editable.free()
    obj.data.update()


def create_loft(name: str,
                rings_data: list[tuple[float, list[tuple[float, float]]]],
                materials: list[bpy.types.Material],
                quad_material,
                *, closed: bool, caps: tuple[int, int] | None = None) -> bpy.types.Object:
    """Loft ring profiles along y, assigning a material to every quad.

    ``quad_material(row, segment)`` names the material slot for the quad
    between ring ``row`` and ``row + 1`` on ring segment ``segment``.  Closed
    rings wrap around; open rings (the greenhouse) leave the last edge free.
    """
    vertices: list[tuple[float, float, float]] = []
    rings: list[list[int]] = []
    for y, profile in rings_data:
        ring: list[int] = []
        for x, z in profile:
            ring.append(len(vertices))
            vertices.append((x, y, z))
        rings.append(ring)
    ring_size = len(rings[0])
    if any(len(ring) != ring_size for ring in rings):
        raise RuntimeError(f"{name}: loft rings do not share a vertex count")
    faces: list[tuple[int, ...]] = []
    face_materials: list[int] = []
    segments = ring_size if closed else ring_size - 1
    for row, (current, following) in enumerate(zip(rings, rings[1:])):
        for segment in range(segments):
            nxt = (segment + 1) % ring_size
            quad = (current[segment], current[nxt], following[nxt], following[segment])
            if len(set(vertices[index] for index in quad)) < 3:
                continue
            faces.append(quad)
            face_materials.append(quad_material(row, segment))
    if caps is not None:
        faces.append(tuple(reversed(rings[0])))
        face_materials.append(caps[0])
        faces.append(tuple(rings[-1]))
        face_materials.append(caps[1])
    return link_mesh(name, vertices, faces, materials, face_materials)


# ---------------------------------------------------------------------------
# Body loft
# ---------------------------------------------------------------------------

# Greenhouse stations: (y, half-width at the sill, roof/glass top z).
GREENHOUSE_STATIONS = (
    ( .82, .815, 1.005),
    ( .54, .820, 1.142),
    ( .22, .825, 1.290),
    (-.10, .830, 1.397),
    (-.43, .835, 1.416),
    (-.76, .850, 1.401),
    (-.86, .860, 1.376),
    (-1.05, .875, 1.313),
    (-1.31, .875, 1.214),
    (-1.59, .860, 1.115),
    (-1.86, .820, 1.030),
    (-2.04, .770,  .990),
)
GREENHOUSE_SILL = .970


def greenhouse_half(y: float) -> float:
    stations = GREENHOUSE_STATIONS
    if y >= stations[0][0]:
        return stations[0][1]
    if y <= stations[-1][0]:
        return stations[-1][1]
    for (y0, h0, _), (y1, h1, _) in zip(stations, stations[1:]):
        if y1 <= y <= y0:
            t = (y0 - y) / (y0 - y1)
            return h0 + (h1 - h0) * t
    return stations[-1][1]


def body_stations() -> list[dict]:
    """Cross-section stations from nose to tail.

    Each station gives the half width, hood/deck centre height, the hood
    valley at 40% width, the fender peak at 72%, the shoulder crease at 93%,
    the belt (widest point) and the rocker top.  Arch stations add the wheel
    opening lip height; cabin stations drop the centre into the tub.
    """
    def station(y, half, top, valley, peak, shoulder, belt, lower=.17, lip=None):
        return {"y": y, "half": half, "top": top, "valley": valley, "peak": peak,
                "shoulder": shoulder, "belt": belt, "lower": lower, "lip": lip}

    stations = [
        station(2.60,  .935, .615, .615, .638, .630, .450, .20),
        station(2.52,  .985, .686, .678, .722, .729, .575, .20),
        station(2.37, 1.023, .779, .758, .809, .824, .682, .20),
        station(2.20, 1.048, .829, .802, .878, .894, .740, .20),
        station(2.01, 1.060, .879, .845, .927, .950, .790, .20),
    ]
    # Nine stations keep the arch round without a subdivision modifier.
    for degrees in (0, 22.5, 45, 67.5, 90, 112.5, 135, 157.5, 180):
        angle = math.radians(degrees)
        t = math.sin(angle)
        y = FRONT_AXLE_Y + math.cos(angle) * FRONT_ARCH_R
        rearward = (FRONT_AXLE_Y + FRONT_ARCH_R - y) / (2 * FRONT_ARCH_R)
        stations.append(station(
            y, 1.060 + .012 * t, .886 + .084 * rearward, .859 + .093 * rearward,
            .948 + .023 * t + .023 * rearward,
            .962 + .016 * t + .009 * rearward, .805 + .015 * t,
            lip=FRONT_ARCH_Z + FRONT_ARCH_R * t))
    stations.extend([
        station(1.08, 1.042, .976, .953, .975, .973, .810, .21),
        station( .83, 1.000, .996, .984, .982, .980, .815, .21),
        station( .56,  .980, None, None, .980, .970, .810, .21),
        station( .00,  .972, None, None, .980, .977, .812, .21),
        station(-.45,  .990, None, None, .980, .990, .820, .21),
        station(-.85, 1.025, None, None, .990,1.018, .835, .21),
        station(-1.06,1.058, None, None,1.015,1.045, .849, .21),
    ])
    for degrees in (180, 157.5, 135, 112.5, 90, 67.5, 45, 22.5, 0):
        angle = math.radians(degrees)
        t = math.sin(angle)
        y = REAR_AXLE_Y + math.cos(angle) * REAR_ARCH_R
        cabin = y >= TUB_REAR_Y
        stations.append(station(
            y, 1.068 + .017 * t, None if cabin else .976, None if cabin else .979,
            1.025 + .016 * t, 1.044 + .015 * t, .840 + .015 * t,
            lip=REAR_ARCH_Z + REAR_ARCH_R * t))
    stations.extend([
        station(-2.01,1.067, .976, .981,1.025,1.040, .845, .21),
        station(-2.23,1.049, .974, .981,1.007,1.015, .828, .21),
        station(-2.43,1.024, .982, .987, .994, .985, .800, .20),
        station(-2.54, .998, .987, .990, .993, .980, .780, .20),
    ])
    # Each rear arch angle traverses tail-to-front; sort before lofting to
    # avoid overlapping faces and the creased spikes of the old wheel wells.
    return sorted(stations, key=lambda item: item["y"], reverse=True)


def body_profile(st: dict) -> list[tuple[float, float]]:
    """Left-side half profile from the top centre down to the floor centre."""
    h = st["half"]
    lower = st["lower"]
    belt = st["belt"]
    lip = st["lip"]
    tub = st["top"] is None
    if tub:
        top = (0.0, TUB_Z)
        valley = (-TUB_HALF, TUB_Z)
        peak = (-(greenhouse_half(st["y"]) - .005), GREENHOUSE_SILL - .010)
    else:
        top = (0.0, st["top"])
        valley = (-h * .40, st["valley"])
        peak = (-h * .72, st["peak"])
    shoulder = (-h * .93, st["shoulder"])
    if lip is None:
        # Belt at full width, a tucked scallop below it, a lower ledge that
        # comes back out to catch the light, then the inset rocker.
        side = [
            (-h, belt),
            (-h * .945, lower + (belt - lower) * .48),
            (-h * .997, lower + (belt - lower) * .20),
            (-h * .960, lower),
            (-h * .930, lower - .08),
            (-h * .800, FLOOR_Z),
        ]
    else:
        # The opening: shoulder, two side points sharing the fender lip, the
        # lip itself, then the well ceiling and its inner wall down to the floor.
        span = st["shoulder"] - lip
        side = [
            (-h, st["shoulder"] - span * .25),
            (-h * 1.002, lip + span * .42),
            (-h * 1.006, lip + min(.018, span * .20)),
            (-h * .995, lip),
            (-h * .660, lip + .012),
            (-h * .660, FLOOR_Z),
        ]
    return [top, valley, peak, shoulder, *side, (0.0, FLOOR_Z - .006)]


def body_ring(st: dict) -> list[tuple[float, float]]:
    left = body_profile(st)
    right = [(-x, z) for x, z in reversed(left[1:-1])]
    return left + right


def create_body(materials: dict[str, bpy.types.Material]) -> bpy.types.Object:
    paint = materials["DLA Paint"]
    carbon = materials["DLA Carbon"]
    stations = body_stations()
    rings = [(st["y"], body_ring(st)) for st in stations]
    slots = [paint, carbon]

    def quad_material(row: int, segment: int) -> int:
        # Segment j joins ring points j and j+1; 0..9 run down the left side,
        # 10..19 back up the right.  Mirror to the left-side index.
        left = segment if segment < 10 else 19 - segment
        a, b = stations[row], stations[row + 1]
        arch = a["lip"] is not None or b["lip"] is not None
        tub = a["top"] is None and b["top"] is None
        if left in (0, 1) and tub:
            return 1
        if left == 7 and arch:
            return 1
        if left in (8, 9):
            return 1
        return 0

    body = create_loft("Coupe body", rings, slots, quad_material,
                       closed=True, caps=(0, 0))
    # The fascia wraps around the corners. A planar cap made the first car
    # look like a box with stickers instead of the C7's swept nose and tail.
    for vertex in body.data.vertices:
        x, y, _z = vertex.co
        if y > 2.01:
            vertex.co.y -= .16 * (abs(x) / 1.06) ** 2 * (y - 2.01) / .59
        elif y < -2.01:
            vertex.co.y += .10 * (abs(x) / 1.07) ** 2 * (-y - 2.01) / .53
    editable = bmesh.new()
    editable.from_mesh(body.data)
    bmesh.ops.triangulate(editable, faces=[face for face in editable.faces
                                         if len(face.verts) > 4], ngon_method="BEAUTY")
    bmesh.ops.recalc_face_normals(editable, faces=editable.faces)
    editable.to_mesh(body.data)
    editable.free()
    body.data.update()
    return body


def create_greenhouse(materials: dict[str, bpy.types.Material]) -> bpy.types.Object:
    paint = materials["DLA Paint"]
    glass = materials["DLA Glass"]
    carbon = materials["DLA Carbon"]
    rings = []
    for y, half, top in GREENHOUSE_STATIONS:
        sill = GREENHOUSE_SILL
        rise = top - sill
        glass_edge = .780 - max(0.0, -y - .86) * .080
        left = [
            (-half, sill),
            (-(half - .012), sill + min(.032, rise * .25)),
            (-half * (glass_edge + .030), top - min(.032, rise * .40)),
            (-half * glass_edge, top - min(.009, rise * .12)),
            (-half * .430, top - min(.003, rise * .04)),
        ]
        ring = left + [(0.0, top)] + [(-x, z) for x, z in reversed(left)]
        rings.append((y, ring))
    slots = [paint, glass, carbon]
    # Per row: material for segments 0..4 from the sill to the roof centre.
    windshield = (0, 1, 0, 1, 1)
    roof = (0, 1, 0, 2, 2)
    b_pillar = (0, 2, 2, 0, 0)
    sail = (0, 1, 0, 1, 1)
    hatch = (0, 0, 0, 1, 1)
    row_specs = [windshield, windshield, windshield, roof, roof, b_pillar,
                 sail, hatch, hatch, hatch, hatch]

    def quad_material(row: int, segment: int) -> int:
        left = segment if segment < 5 else 9 - segment
        return row_specs[row][left]

    greenhouse = create_loft("Greenhouse", rings, slots, quad_material, closed=False)
    orient_outward(greenhouse, .70)
    return greenhouse


# ---------------------------------------------------------------------------
# Details
# ---------------------------------------------------------------------------

def surface_tree(obj: bpy.types.Object) -> BVHTree:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    return BVHTree.FromObject(obj, depsgraph)


def create_polygon(name: str,
                   points: tuple[tuple[float, float, float], ...],
                   material: bpy.types.Material,
                   thickness: float = 0.0,
                   bevel_width: float = 0.0) -> bpy.types.Object:
    obj = link_mesh(name, list(points), [tuple(range(len(points)))], [material],
                    smooth=False)
    if thickness <= 0.0:
        # Single-sided trim is backface culled in the game, so its normal
        # must leave the car: flip it if it points toward the body centre.
        polygon = obj.data.polygons[0]
        centre = Vector(polygon.center)
        outward = centre - Vector((0.0, centre.y * .35, .55))
        if polygon.normal.dot(outward) < 0.0:
            obj.data.flip_normals()
            obj.data.update()
    if thickness > 0.0:
        solidify = obj.modifiers.new("Inset depth", "SOLIDIFY")
        solidify.thickness = thickness
        solidify.offset = 0.0
        apply_modifier(obj, solidify)
    if bevel_width > 0.0:
        bevel = obj.modifiers.new("Edge radius", "BEVEL")
        bevel.width = bevel_width
        bevel.segments = 1
        apply_modifier(obj, bevel)
    smooth_mesh(obj)
    return obj


def mirror_points_x(points):
    return tuple((-x, y, z) for x, y, z in reversed(points))


def create_surface_patch(name: str, points, material: bpy.types.Material,
                         tree: BVHTree, offset: float, cuts: int = 2) -> bpy.types.Object:
    """Tessellate trim before projection so its interior follows curved panels.

    Projecting only the outline leaves hood vents and door seams buried in
    convex bodywork. Small flat details can skip subdivision entirely.
    """
    obj=link_mesh(name,list(points),[tuple(range(len(points)))],[material])
    editable=bmesh.new()
    editable.from_mesh(obj.data)
    bmesh.ops.triangulate(editable,faces=list(editable.faces))
    if cuts:
        bmesh.ops.subdivide_edges(editable,edges=list(editable.edges),cuts=cuts,use_grid_fill=True)
    for vertex in editable.verts:
        location,normal,_index,_distance=tree.find_nearest(vertex.co)
        if location is not None:
            vertex.co=location+normal*offset
    editable.normal_update()
    for face in editable.faces:
        _location,normal,_index,_distance=tree.find_nearest(face.calc_center_median())
        if normal is not None and face.normal.dot(normal)<0:
            face.normal_flip()
    editable.to_mesh(obj.data)
    editable.free()
    obj.data.update()
    return obj


def create_fascia_patch(name, points, material, body, front, offset):
    """Clip an X/Z decal to each cap triangle before lifting it off the skin.

    The bumper wraps in Y. Projecting a coarse decal onto it lets triangles
    bridge that curvature and sink into the paint. Matching the cap's actual
    topology gives exact contact without piling on tessellation.
    """
    outline=bmesh.new()
    vertices=[outline.verts.new(point) for point in points]
    outline.faces.new(vertices)
    outline.normal_update()
    bmesh.ops.triangulate(outline, faces=list(outline.faces), ngon_method="BEAUTY")
    pieces=[[Vector((v.co.x,v.co.z)) for v in face.verts] for face in outline.faces]
    outline.free()
    size=20
    last=len(body.data.vertices)-size
    caps=[face for face in body.data.polygons
          if all(index<size if front else index>=last for index in face.vertices)]
    vertices=[]
    faces=[]
    def cross(a,b):
        return a.x*b.y-a.y*b.x
    for face in caps:
        tri=[body.data.vertices[index].co for index in face.vertices]
        normal=(tri[1]-tri[0]).cross(tri[2]-tri[0])
        if abs(normal.y)<1e-9:
            continue
        projected=[Vector((v.x,v.z)) for v in tri]
        if cross(projected[1]-projected[0],projected[2]-projected[0])<0:
            projected.reverse()
        for piece in pieces:
            clipped=piece
            for a,b in zip(projected,projected[1:]+projected[:1]):
                result=[]
                for p,q in zip(clipped,clipped[1:]+clipped[:1]):
                    dp=cross(b-a,p-a)
                    dq=cross(b-a,q-a)
                    if dp>=-1e-8:
                        result.append(p)
                    if (dp>=0)!=(dq>=0):
                        result.append(p+(q-p)*(dp/(dp-dq)))
                clipped=result
                if len(clipped)<3:
                    break
            if len(clipped)<3:
                continue
            start=len(vertices)
            for v in clipped:
                y=tri[0].y-(normal.x*(v.x-tri[0].x)+normal.z*(v.y-tri[0].z))/normal.y
                vertices.append((v.x,y+(offset if front else -offset),v.y))
            faces.append(tuple(range(start,len(vertices))))
    obj=link_mesh(name,vertices,faces,[material],smooth=False)
    for face in obj.data.polygons:
        if face.normal.y*(1 if front else -1)<0:
            face.flip()
    obj.data.update()
    return obj


def create_prism(name: str, base: tuple[tuple[float, float, float], ...],
                 direction: tuple[float, float, float],
                 material: bpy.types.Material) -> bpy.types.Object:
    """Extrude a flat polygon along a direction into a closed prism."""
    count = len(base)
    top = [(x + direction[0], y + direction[1], z + direction[2]) for x, y, z in base]
    vertices = list(base) + top
    faces = [tuple(reversed(range(count))), tuple(range(count, 2 * count))]
    for index in range(count):
        nxt = (index + 1) % count
        faces.append((index, nxt, count + nxt, count + index))
    obj = link_mesh(name, vertices, faces, [material])
    editable = bmesh.new()
    editable.from_mesh(obj.data)
    bmesh.ops.recalc_face_normals(editable, faces=editable.faces)
    editable.to_mesh(obj.data)
    editable.free()
    obj.data.update()
    return obj


def create_ring(name: str, centre: tuple[float, float, float], axis: str,
                outer: float, inner: float, depth: float, segments: int,
                material: bpy.types.Material) -> bpy.types.Object:
    """A short tube with a flat annular face: exhaust tips and the fuel door."""
    vertices = []
    faces = []
    for step, radius in ((0.0, outer), (0.0, inner), (-depth, outer)):
        for index in range(segments):
            angle = index * math.tau / segments
            u, v = math.cos(angle) * radius, math.sin(angle) * radius
            if axis == "y":
                vertices.append((centre[0] + u, centre[1] + step, centre[2] + v))
            else:
                vertices.append((centre[0] + step, centre[1] + u, centre[2] + v))
    for index in range(segments):
        nxt = (index + 1) % segments
        faces.append((index, nxt, segments + nxt, segments + index))
        faces.append((2 * segments + index, 2 * segments + nxt, nxt, index))
    obj = link_mesh(name, vertices, faces, [material])
    editable = bmesh.new()
    editable.from_mesh(obj.data)
    bmesh.ops.recalc_face_normals(editable, faces=editable.faces)
    editable.to_mesh(obj.data)
    editable.free()
    obj.data.update()
    return obj


def add_surface_details(materials: dict[str, bpy.types.Material],
                        body: bpy.types.Object) -> None:
    paint = materials["DLA Paint"]
    carbon = materials["DLA Carbon"]
    lights = materials["DLA Lights"]
    metal = materials["DLA Metal"]
    headlamp = materials["DLA Headlamp"]
    tree = surface_tree(body)

    def patch(name, points, material, offset=.008, cuts=1):
        if min(p[1] for p in points)>2.5 and max(p[2] for p in points)<.61:
            return create_fascia_patch(name,points,material,body,True,offset)
        if max(p[1] for p in points)<-2.57:
            return create_fascia_patch(name,points,material,body,False,offset)
        return create_surface_patch(name, points, material, tree, offset, cuts)

    def pair(name, points, material, offset=.008, cuts=1):
        return [patch(name + side, polygon, material, offset, cuts)
                for side, polygon in ((" L", points), (" R", mirror_points_x(points)))]

    # Broad swept housings, with the DRL following the inboard edge and a
    # discrete projector in the wide end. These are not slit-shaped lamps.
    housing = ((-.620,2.460,.715),(-.907,2.413,.763),
               (-1.014,2.004,.947),(-.944,1.940,.961),
               (-.810,2.217,.845),(-.598,2.423,.740))
    pair("Headlamp recess", housing, carbon, .009, 2)
    lens = ((-.650,2.446,.720),(-.891,2.400,.770),
            (-.991,2.029,.944),(-.947,1.992,.950),
            (-.828,2.233,.843),(-.629,2.414,.742))
    pair("Headlamp lens", lens, materials["DLA Lamp Lens"], .015, 2)
    led = ((-.615,2.441,.730),(-.815,2.225,.852),(-.944,1.968,.962),
           (-.962,1.981,.962),(-.838,2.245,.851),(-.627,2.462,.730))
    pair("Headlamp DRL", led, headlamp, .023, 2)
    for side in (-1.0, 1.0):
        projector = tuple((side*(.895+.040*math.cos(a)),2.304+.054*math.sin(a),.841)
                          for a in (i*math.tau/10 for i in range(10)))
        patch("Headlamp projector", projector, headlamp, .024, 0)

    grille = ((-.666,2.615,.496),(.666,2.615,.496),(.742,2.615,.435),
              (.680,2.615,.246),(.575,2.615,.224),(-.575,2.615,.224),
              (-.680,2.615,.246),(-.742,2.615,.435))
    patch("Front grille", grille, carbon, .012, 2)
    patch("Grille crossbar", ((-.711,2.625,.360),(.711,2.625,.360),
                              (.705,2.625,.350),(-.705,2.625,.350)), metal, .022)
    for x in (-.50,-.25,0,.25,.50):
        patch("Grille upright", ((x-.006,2.62,.47),(x+.006,2.62,.47),
                                  (x+.006,2.62,.25),(x-.006,2.62,.25)), metal, .020, 0)
    create_prism("Front splitter", ((-.90,2.478,.138),(-.76,2.598,.138),
                 (0,2.660,.138),(.76,2.598,.138),(.90,2.478,.138),
                 (1.00,2.325,.138),(-1.00,2.325,.138)), (0,0,.029), carbon)
    patch("Hood extractor", ((-.240,1.985,.943),(.240,1.985,.943),
                             (.325,1.545,.950),(-.325,1.545,.950)), carbon, .009, 2)
    for y in (1.63,1.72,1.81,1.90):
        half=.325-(y-1.545)*(.085/.440)
        patch("Hood vent louver", ((-half,y,.96),(half,y,.96),
               (half-.006,y+.012,.96),(-half+.006,y+.012,.96)), metal, .020)
    pair("Hood crease", ((-.42,2.28,.85),(-.535,1.02,1.00),
                          (-.528,1.02,1.00),(-.414,2.28,.85)), paint, .005, 2)
    patch("Front badge red", ((-.008,2.54,.729),(-.092,2.50,.752),
                              (-.015,2.57,.710),(.004,2.56,.716)), lights, .016, 0)
    patch("Front badge metal", ((.008,2.54,.729),(.092,2.50,.752),
                                (.015,2.57,.710),(-.004,2.56,.716)), metal, .016, 0)

    # The characteristic vent follows the back of the front wheel arch,
    # opening out along the beltline above the sculpted door scallop.
    extractor = ((-1.075,1.085,.911),(-1.042,.552,.907),
                 (-1.034,.817,.760),(-1.058,1.012,.630))
    pair("Fender extractor", extractor, carbon, .009, 2)
    pair("Fender vent blade", ((-1.070,1.050,.867),(-1.036,.671,.864),
                              (-1.039,.715,.846),(-1.063,1.037,.846)), metal, .019)
    pair("Side marker", ((-1.040,2.300,.710),(-1.065,2.115,.697),
                          (-1.066,2.114,.680),(-1.039,2.296,.692)), lights, .013, 0)
    pair("Door handle", ((-1.04,-.49,.932),(-1.04,-.71,.932),
                          (-1.04,-.71,.911),(-1.04,-.49,.911)), carbon, .009, 0)
    pair("Front door seam", ((-1.05,.51,.946),(-1.05,.48,.26),
                              (-1.05,.475,.26),(-1.05,.505,.946)), carbon, .004, 2)
    pair("Rear door seam", ((-1.06,-.88,.950),(-1.06,-.94,.26),
                             (-1.06,-.945,.26),(-1.06,-.885,.95)), carbon, .004, 2)
    scoop = ((-.885,-.937,1.023),(-1.024,-1.082,1.054),
              (-1.051,-1.280,1.060),(-.901,-1.167,1.025))
    pair("Quarter cooling scoop", scoop, carbon, .012)
    location, normal, _i, _d = tree.find_nearest(Vector((-1.2,-2.12,.85)))
    create_ring("Fuel door outline", (location.x + normal.x*.005, location.y, location.z),
                "x", .066, .062, .003, 12, carbon)
    for side in (-1.0, 1.0):
        create_prism("Side skirt", ((side*.961,1.09,.143),(side*1.014,1.09,.143),
                     (side*1.047,-1.07,.143),(side*.977,-1.07,.143)), (0,0,.039), carbon)
        x = side * 1.05
        head = ((x-.022*side,.41,1.010),(x+.092*side,.41,.998),(x+.165*side,.41,1.030),
                (x+.160*side,.41,1.065),(x+.082*side,.41,1.088),(x-.020*side,.41,1.079))
        create_prism("Door mirror head", head, (0,.15,0), paint)
        glass = tuple((px,py-.005,pz) for px,py,pz in head)
        create_polygon("Door mirror glass", glass, materials["DLA Glass"])
        stalk = ((x-.100*side,.45,.972),(x-.100*side,.52,.972),
                 (x+.018*side,.51,1.021),(x+.018*side,.46,1.021))
        create_prism("Door mirror stalk", stalk, (0,0,.022), carbon)

    # Both lights sit in one angular black surround; its outer corner feeds
    # into the tall extractor underneath, as on the C7 production fascia.
    surround = ((-.990,-2.58,.949),(-.248,-2.58,.921),(-.206,-2.58,.858),
                (-.274,-2.58,.682),(-.729,-2.58,.664),(-.874,-2.58,.507),
                (-1.002,-2.58,.570),(-1.020,-2.58,.801))
    pair("Rear lamp and extractor surround", surround, carbon, .012, 2)
    outer = ((-.960,-2.59,.902),(-.686,-2.59,.891),(-.638,-2.59,.839),
             (-.707,-2.59,.735),(-.900,-2.59,.743),(-.981,-2.59,.803))
    inner = ((-.608,-2.59,.883),(-.324,-2.59,.873),(-.281,-2.59,.828),
             (-.345,-2.59,.724),(-.549,-2.59,.733),(-.626,-2.59,.789))
    for points in (outer,inner,mirror_points_x(outer),mirror_points_x(inner)):
        cx=sum(p[0] for p in points)/len(points)
        cz=sum(p[2] for p in points)/len(points)
        inset=tuple((cx+(x-cx)*.79,y,cz+(z-cz)*.69) for x,y,z in points)
        projected=[]
        for point in (*points,*inset):
            location,normal,_index,_distance=tree.find_nearest(Vector(point))
            projected.append(tuple(location+normal*.026))
        count=len(points)
        lamp=link_mesh("Tail lamp signature",projected,
                      [(i,(i+1)%count,(i+1)%count+count,i+count) for i in range(count)],
                      [lights],smooth=False)
        lamp["brake_lamp"]=True
        for polygon in lamp.data.polygons:
            if polygon.normal.y>0:
                polygon.flip()
        lamp.data.update()
        patch("Tail lamp dark lens",inset,materials["DLA Lamp Lens"],.022,0)
    for z in (.575,.615):
        pair("Rear extractor blade", ((-.963,-2.59,z+.015),(-.844,-2.59,z+.015),
                                       (-.827,-2.59,z),(-.958,-2.59,z)), metal, .024)
    diffuser = ((-.932,-2.58,.505),(-.640,-2.58,.466),(-.460,-2.58,.600),
                (.460,-2.58,.600),(.640,-2.58,.466),(.932,-2.58,.505),
                (.805,-2.58,.163),(.400,-2.58,.144),(-.400,-2.58,.144),(-.805,-2.58,.163))
    patch("Rear diffuser",diffuser,carbon,.015,2)
    patch("Plate", ((-.248,-2.59,.571),(.248,-2.59,.571),
                     (.242,-2.59,.401),(-.242,-2.59,.401)), metal, .031)
    # A small raised C7 plate mark stays readable without another texture.
    for points in (((-.097,-2.60,.527),(-.015,-2.60,.527),(-.015,-2.60,.511),
                    (-.077,-2.60,.511),(-.077,-2.60,.456),(-.015,-2.60,.456),
                    (-.015,-2.60,.440),(-.097,-2.60,.440)),
                   ((.010,-2.60,.527),(.099,-2.60,.527),(.099,-2.60,.510),
                    (.049,-2.60,.440),(.025,-2.60,.440),(.075,-2.60,.510),(.010,-2.60,.510))):
        patch("Plate C7",points,carbon,.037,0)
    for x in (-.66,-.47,.47,.66):
        create_prism("Diffuser fin",((x,-2.57,.135),(x,-2.23,.135),
                     (x,-2.23,.20),(x,-2.57,.32)),(.016,0,0),carbon)
    pair("Rear reflector", ((-.871,-2.60,.463),(-.624,-2.60,.426),
                            (-.631,-2.60,.403),(-.860,-2.60,.441)), lights, .032)
    patch("Rear badge red", ((-.016,-2.58,.916),(-.101,-2.58,.944),
                             (-.023,-2.58,.876),(0,-2.58,.890)), lights,.025,0)
    patch("Rear badge metal", ((.016,-2.58,.916),(.101,-2.58,.944),
                               (.023,-2.58,.876),(0,-2.58,.890)), metal,.025,0)
    for center_x in (-.270,-.090,.090,.270):
        create_ring("Exhaust tip", (center_x,-2.592,.235), "y", .076, .060, -.11, 12, metal)
        create_polygon("Exhaust bore", tuple(
            (center_x+math.cos(a)*.060,-2.577,.235+math.sin(a)*.060)
            for a in (i*math.tau/12 for i in range(12))), carbon)

    # Low contoured Z51 lip: rises at the outboard ends, wraps with the tail.
    spoiler=[]
    for x in (-1.012,-.82,-.46,0,.46,.82,1.012):
        back=-2.575+.10*(abs(x)/1.07)**2
        z=1.010+.037*(abs(x)/1.012)**3
        spoiler.extend(((x,back+.12,z-.031),(x,back,z)))
    faces=[(i*2,i*2+1,i*2+3,i*2+2) for i in range(6)]
    lip=link_mesh("Spoiler lip",spoiler,faces,[carbon])
    solidify=lip.modifiers.new("Spoiler thickness","SOLIDIFY")
    solidify.thickness=.024
    apply_modifier(lip,solidify)
    stop=create_polygon("Center stop lamp", ((-.244,-2.581,1.005),(.244,-2.581,1.005),
                                             (.244,-2.581,.990),(-.244,-2.581,.990)), lights)
    stop["brake_lamp"]=True


def add_interior(materials: dict[str, bpy.types.Material]) -> None:
    """Cabin furniture inside the tub, seen through the greenhouse."""
    carbon = materials["DLA Carbon"]
    metal = materials["DLA Metal"]
    create_polygon("Dashboard", ((-.62,.66,.79),(.62,.66,.79),(.58,.34,.83),(-.58,.34,.83)), carbon, .10)
    create_polygon("Dash fascia", ((-.58,.34,.83),(.58,.34,.83),(.56,.32,.58),(-.56,.32,.58)), carbon, .04)
    create_polygon("Binnacle", ((-.52,.36,.86),(-.20,.36,.86),(-.20,.24,.90),(-.52,.24,.90)), carbon, .05)
    create_polygon("Centre console", ((-.12,.32,.60),(.12,.32,.60),(.12,-.60,.56),(-.12,-.60,.56)), carbon, .08)
    rim = tuple((-.36+.17*math.cos(a), .12, .80+.17*math.sin(a))
                for a in (i*math.pi/4 for i in range(8)))
    create_polygon("Steering wheel", rim, metal, .03)
    create_polygon("Steering hub", ((-.42,.14,.86),(-.30,.14,.86),(-.30,.14,.74),(-.42,.14,.74)), carbon, .04)
    for side, label in ((-1.0,"Driver"),(1.0,"Passenger")):
        x = side*.36
        create_polygon(f"{label} seat base", ((x-.24,-.12,.54),(x+.24,-.12,.54),(x+.24,-.62,.56),(x-.24,-.62,.56)), carbon, .12)
        create_polygon(f"{label} seat back", ((x-.24,-.60,.54),(x+.24,-.60,.54),(x+.22,-.74,1.02),(x-.22,-.74,1.02)), carbon, .10)
        create_polygon(f"{label} headrest", ((x-.13,-.72,1.04),(x+.13,-.72,1.04),(x+.13,-.78,1.20),(x-.13,-.78,1.20)), carbon, .08)


def add_qa_wheels(materials: dict[str, bpy.types.Material]) -> None:
    """Preview the same staggered tires and forged spokes used by the rig."""
    tire_material = make_material("QA Tire", (.018,.020,.025,1), roughness=.80)
    rim_material = make_material("QA Rim", (.58,.60,.64,1), metallic=.70, roughness=.25)
    rotor_material = make_material("QA Rotor", (.16,.18,.20,1), metallic=.70)
    caliper_material = make_material("QA Caliper", (.66,.018,.010,1), metallic=.20)

    def preview(obj):
        obj["export_kos"]=False
        return obj

    for axle_y,radius,width,x_center in ((1.55,.385,.28,.916),(-1.55,.400,.32,.900)):
        for side in (-1.0,1.0):
            cx=side*x_center
            cz=radius+.015
            half=width*.5
            segments=20
            vertices=[]
            faces=[]
            profile=((-half,.805),(-half,.94),(-half+.024,1),
                     (half-.024,1),(half,.94),(half,.805))
            for x,r in profile:
                for i in range(segments):
                    a=i*math.tau/segments
                    vertices.append((cx+x,axle_y+math.sin(a)*radius*r,cz+math.cos(a)*radius*r))
            for j in range(6):
                for i in range(segments):
                    n=(i+1)%segments
                    k=(j+1)%6
                    faces.append((j*segments+i,k*segments+i,k*segments+n,j*segments+n))
            tire=preview(link_mesh("QA Tire",vertices,faces,[tire_material]))
            editable=bmesh.new()
            editable.from_mesh(tire.data)
            bmesh.ops.recalc_face_normals(editable,faces=editable.faces)
            editable.to_mesh(tire.data)
            editable.free()
            face=cx+side*(half+.007)
            preview(create_ring("QA Rim lip",(face,axle_y,cz),"x",
                                radius*.825,radius*.790,side*.02,20,rim_material))
            disc=tuple((cx+side*(half-.050),axle_y+math.sin(a)*radius*.635,
                        cz+math.cos(a)*radius*.635) for a in (i*math.tau/20 for i in range(20)))
            preview(create_polygon("QA Rotor",disc,rotor_material))
            for spoke in range(5):
                angle=spoke*math.tau/5
                vertices=[]
                for r,w,d in ((.13,.38,-.014),(.43,.155,.021),(.805,.100,.005)):
                    for k in range(3):
                        a=angle+(k-1)*w
                        vertices.append((face+side*(d+(.012 if k==1 else 0)),
                                         axle_y+math.sin(a)*radius*r,cz+math.cos(a)*radius*r))
                faces=[(j*3+k,(j+1)*3+k,(j+1)*3+k+1,j*3+k+1)
                       for j in range(2) for k in range(2)]
                obj=preview(link_mesh("QA Wheel spoke",vertices,faces,[rim_material],smooth=False))
                for polygon in obj.data.polygons:
                    if polygon.normal.x*side<0:
                        polygon.flip()
                obj.data.update()
            preview(create_polygon("QA Hub",tuple(
                (face+side*.014,axle_y+math.sin(a)*radius*.16,cz+math.cos(a)*radius*.16)
                for a in (i*math.tau/10 for i in range(10))),rim_material))
            x=cx+side*(half-.023)
            preview(create_polygon("QA Caliper",((x,axle_y-radius*.61,cz-.070),
                (x,axle_y-radius*.61,cz+.090),(x,axle_y-radius*.35,cz+.115),
                (x,axle_y-radius*.35,cz-.090)),caliper_material))


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------

def material_id_for(obj: bpy.types.Object, mesh: bpy.types.Mesh,
                    polygon: bpy.types.MeshPolygon) -> int:
    if len(mesh.materials) == 0:
        raise RuntimeError(f"{obj.name} has no export material")
    name = mesh.materials[min(polygon.material_index,len(mesh.materials)-1)].name
    if name not in MATERIAL_IDS:
        raise RuntimeError(f"{obj.name} uses unknown export material {name!r}")
    return MATERIAL_IDS[name]


def surface_axes(normal: Vector) -> tuple[int, int]:
    """Choose a chart plane that cannot collapse this polygon's UV area."""
    dominant = max(range(3), key=lambda axis: abs(normal[axis]))
    # Top: width/length; side: length/height; fascia: width/height.
    return ((1, 2), (0, 2), (0, 1))[dominant]


def author_surface_charts(obj: bpy.types.Object) -> None:
    """Store actual corner UVs and hard edges on the reproducible source mesh.

    Paint and glass sample one shared environment-gradient texture: side and
    fascia charts map height onto the gradient, while upward-facing panels
    sample its bright sky band.  Details use their own surface-aligned charts,
    while lamps keep their dedicated atlas cells.
    """
    mesh = obj.data
    editable = bmesh.new()
    editable.from_mesh(mesh)
    ngons = [face for face in editable.faces if len(face.verts) > 4]
    if ngons:
        bmesh.ops.triangulate(editable, faces=ngons, ngon_method="BEAUTY")
    empty_faces = [face for face in editable.faces if face.calc_area() < 1e-8]
    if empty_faces:
        bmesh.ops.delete(editable, geom=empty_faces, context="FACES_ONLY")
    editable.to_mesh(mesh)
    editable.free()
    mesh.update()
    normal_matrix = obj.matrix_world.to_3x3().inverted().transposed()
    positions = [obj.matrix_world @ vertex.co for vertex in mesh.vertices]
    uv_layer = mesh.uv_layers.get("DLA Surface") or mesh.uv_layers.new(name="DLA Surface")
    mesh.uv_layers.active = uv_layer
    adjacent: dict[int, list[Vector]] = {}
    for polygon in mesh.polygons:
        for loop_index in polygon.loop_indices:
            edge_index = mesh.loops[loop_index].edge_index
            adjacent.setdefault(edge_index, []).append(polygon.normal.copy())
    crease_cosine = math.cos(math.radians(42.0))
    for edge in mesh.edges:
        normals = adjacent.get(edge.index, [])
        edge.use_edge_sharp = (len(normals) == 2 and
                               normals[0].dot(normals[1]) < crease_cosine)

    for polygon in mesh.polygons:
        normal = (normal_matrix @ polygon.normal).normalized()
        axes = surface_axes(normal)
        material = material_id_for(obj, mesh, polygon)
        bounds = [(min(point[axis] for point in positions),
                   max(point[axis] for point in positions)) for axis in axes]
        name = obj.name.lower()
        for loop_index in polygon.loop_indices:
            point = positions[mesh.loops[loop_index].vertex_index]
            if material in (PAINT, GLASS):
                if axes == (0, 1):
                    # Upward panels: the sky band across the top of the gradient.
                    u, v = .5 + point.x / 2.5, .035 + .09 * (2.82 - point.y) / 5.55
                elif axes == (1, 2):
                    u, v = (point.y + 2.73) / 5.55, .04 + .94 * (1.43 - point.z) / 1.43
                else:
                    u, v = .5 + point.x / 2.5, .04 + .94 * (1.43 - point.z) / 1.43
            elif material == CARBON:
                # Repeated weave keeps trim density independent of part size.
                u, v = point[axes[0]] * 1.6, -point[axes[1]] * 1.6
                if obj.name != "Greenhouse" and obj.name != "Coupe body":
                    # A tiny but non-collapsed footprint in the matte swatch.
                    u = .952 + .020 * (point[axes[0]] - bounds[0][0]) / max(bounds[0][1] - bounds[0][0], 1e-6)
                    v = .952 + .020 * (point[axes[1]] - bounds[1][0]) / max(bounds[1][1] - bounds[1][0], 1e-6)
            else:
                u = (point[axes[0]] - bounds[0][0]) / max(bounds[0][1] - bounds[0][0], 1e-6)
                v = 1.0 - (point[axes[1]] - bounds[1][0]) / max(bounds[1][1] - bounds[1][0], 1e-6)
                if material == LIGHTS:
                    if name.startswith(("headlamp drl", "headlamp projector")):
                        u, v = .120 + u * .010, .620 + v * .010
                    elif name.startswith("tail lamp signature") or name == "center stop lamp":
                        u, v = .370 + u * .010, .620 + v * .010
                    elif name.startswith("headlamp lens"):
                        u, v = .620 + u * .010, .620 + v * .010
                    elif name.startswith("tail lamp dark lens"):
                        u, v = .870 + u * .010, .620 + v * .010
                    elif name.startswith("tail lamp"):
                        u, v = .035 + u * .440, .035 + v * .440
                    elif name.startswith("side marker"):
                        u, v = .80 + u * .04, .84 + v * .04
                    else:
                        # Small red trim samples a compact red patch with a
                        # finite footprint, rather than a zero-area UV point.
                        u, v = .145 + u * .018, .145 + v * .018
            uv_layer.data[loop_index].uv = (u, v)
    mesh.update()


def build_occlusion_tree() -> BVHTree:
    """Include the preview wheels as occluders, but never as exported geometry."""
    positions: list[Vector] = []
    triangles: list[tuple[int, int, int]] = []
    depsgraph = bpy.context.evaluated_depsgraph_get()
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        mesh.calc_loop_triangles()
        base = len(positions)
        positions.extend(obj.matrix_world @ vertex.co for vertex in mesh.vertices)
        triangles.extend(tuple(base + index for index in triangle.vertices)
                         for triangle in mesh.loop_triangles)
        evaluated.to_mesh_clear()
    return BVHTree.FromPolygons(positions, triangles, all_triangles=True)


def bake_occlusion(tree: BVHTree, position: Vector, normal: Vector) -> float:
    """Bake soft, short-range cavity shading with a cosine-weighted hemisphere."""
    helper = Vector((0.0, 0.0, 1.0)) if abs(normal.z) < .85 else Vector((0.0, 1.0, 0.0))
    tangent = normal.cross(helper).normalized()
    bitangent = normal.cross(tangent)
    origin = position + normal * .009
    occlusion = 0.0
    sample_count = 32
    radius = .48
    for index in range(sample_count):
        radius_squared = (index + .5) / sample_count
        angle = index * 2.399963229728653
        radial = math.sqrt(radius_squared)
        direction = (tangent * (math.cos(angle) * radial) +
                     bitangent * (math.sin(angle) * radial) +
                     normal * math.sqrt(1.0 - radius_squared))
        _hit, _normal, _face, distance = tree.ray_cast(origin, direction, radius)
        if distance is not None:
            occlusion += 1.0 - (distance / radius) ** 2
    return max(.62, 1.0 - .52 * occlusion / sample_count)


def collect_export_mesh() -> tuple[list[tuple[float, ...]], list[tuple[int, int, int, int]], list[int]]:
    for obj in bpy.context.scene.objects:
        if obj.type == "MESH" and bool(obj.get("export_kos", False)):
            author_surface_charts(obj)
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    occlusion_tree = build_occlusion_tree()
    vertices: list[tuple[float, ...]] = []
    faces: list[tuple[int, int, int, int]] = []
    brake_triangles: set[tuple[int, int, int, int]] = set()
    vertex_indices: dict[tuple[float, ...], int] = {}
    occlusion_cache: dict[tuple[float, ...], float] = {}
    for obj in sorted(bpy.context.scene.objects,key=lambda item:item.name):
        if obj.type != "MESH" or not bool(obj.get("export_kos",False)):
            continue
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        mesh.calc_loop_triangles()
        normal_matrix = obj.matrix_world.to_3x3().inverted().transposed()
        object_vertex_start=len(vertices)
        object_face_start=len(faces)
        uv_layer = mesh.uv_layers.active
        if uv_layer is None:
            raise RuntimeError(f"{obj.name}: missing authored UV layer")
        for triangle in mesh.loop_triangles:
            if triangle.area < 1e-8:
                continue
            material = material_id_for(obj,mesh,mesh.polygons[triangle.polygon_index])
            indices = []
            for loop_index in triangle.loops:
                loop = mesh.loops[loop_index]
                position = obj.matrix_world @ mesh.vertices[loop.vertex_index].co
                normal = (normal_matrix @ mesh.corner_normals[loop_index].vector).normalized()
                u, v = uv_layer.data[loop_index].uv
                ao_key = tuple(round(value, 6) for value in (*position, *normal))
                if material == LIGHTS:
                    ao = 1.0
                else:
                    if ao_key not in occlusion_cache:
                        occlusion_cache[ao_key] = bake_occlusion(occlusion_tree, position, normal)
                    ao = occlusion_cache[ao_key]
                # Blender x/y/z -> game lateral/vertical/longitudinal.  Material
                # is part of the dedup key so cached runtime shading is unique.
                exported = (position.x, position.z, position.y, u, v,
                            normal.x, normal.z, normal.y, ao)
                key = (material, *(round(value, 5) for value in exported))
                if key not in vertex_indices:
                    vertex_indices[key] = len(vertices)
                    vertices.append(exported)
                indices.append(vertex_indices[key])
            a,b,c = indices
            rounded_positions = [Vector(tuple(round(value, 5) for value in vertices[index][:3]))
                                 for index in indices]
            edge_a = rounded_positions[1] - rounded_positions[0]
            edge_b = rounded_positions[2] - rounded_positions[0]
            if edge_a.cross(edge_b).length_squared < 1e-16:
                continue
            faces.append((a,b,c,material))
            if obj.get("brake_lamp", False):
                brake_triangles.add((a,b,c,material))
        print(f"  export {obj.name}: {len(vertices)-object_vertex_start} vertices, "
              f"{len(faces)-object_face_start} triangles")
        evaluated.to_mesh_clear()
    used_indices = sorted({index for face in faces for index in face[:3]})
    remap = {previous: current for current, previous in enumerate(used_indices)}
    vertices = [vertices[index] for index in used_indices]
    faces = [(remap[a], remap[b], remap[c], material) for a, b, c, material in faces]
    brake_triangles = {(remap[a],remap[b],remap[c],material)
                       for a,b,c,material in brake_triangles}
    if len(vertices) > MAX_EXPORT_VERTICES:
        raise RuntimeError(f"mesh has {len(vertices)} vertices; renderer limit is {MAX_EXPORT_VERTICES}")
    faces.sort(key=lambda face: face[3])
    return vertices,faces,[index for index,face in enumerate(faces) if face in brake_triangles]


def write_header(path: Path, vertices: list[tuple[float, ...]],
                 faces: list[tuple[int, int, int, int]], brake_faces: list[int]) -> None:
    lines = [
        "/* Generated by tools/build_car_blender.py. Do not edit by hand. */",
        "#ifndef DRIFT_LA_MODEL_DATA_H",
        "#define DRIFT_LA_MODEL_DATA_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct { float x, y, z; float u, v; float nx, ny, nz; float ao; } dla_mesh_vertex_t;",
        "typedef struct { uint16_t a, b, c; uint8_t material; } dla_mesh_face_t;",
        "typedef struct { uint16_t first_face, face_count; } dla_mesh_material_range_t;",
        "typedef struct {",
        "    const dla_mesh_vertex_t *vertices;",
        "    const dla_mesh_face_t *faces;",
        "    uint16_t vertex_count, face_count;",
        "    float radius;",
        "} dla_mesh_t;",
        "",
        "#define DLA_COUNT_OF(a) ((uint16_t)(sizeof(a) / sizeof((a)[0])))",
        "enum { DLA_MAT_PAINT, DLA_MAT_GLASS, DLA_MAT_CARBON, DLA_MAT_LIGHTS, DLA_MAT_METAL };",
        "",
        f"/* Blender C7 Corvette: {len(vertices)} vertices, {len(faces)} triangles. */",
        "static const dla_mesh_vertex_t dla_car_vertices[] = {",
    ]
    for vertex in vertices:
        x,y,z,u,v,nx,ny,nz,ao=vertex
        lines.append(f"    {{ {x: .5f}f, {y: .5f}f, {z: .5f}f, {u:.5f}f, {v:.5f}f,"
                     f" {nx: .5f}f, {ny: .5f}f, {nz: .5f}f, {ao:.5f}f }},")
    lines.extend(("};","","static const dla_mesh_face_t dla_car_faces[] = {"))
    for a,b,c,material in faces:
        lines.append(f"    {{ {a:4d}, {b:4d}, {c:4d}, {material} }},")
    lines.extend(("};", "", "/* Contiguous material batches; every vertex belongs to one material. */",
                  "static const dla_mesh_material_range_t dla_car_material_ranges[5] = {"))
    first_face = 0
    for material in range(5):
        count = sum(face[3] == material for face in faces)
        lines.append(f"    {{ {first_face}, {count} }},")
        first_face += count
    lines.extend((
        "};", "", "/* Authored lamp rings and center stop lamp; shared by the brake overlay. */",
        "static const uint16_t dla_car_brake_faces[] = {",
        "    " + ", ".join(str(index) for index in brake_faces),
        "};","","static const dla_mesh_t dla_car_mesh = {",
        "    dla_car_vertices, dla_car_faces,",
        "    DLA_COUNT_OF(dla_car_vertices), DLA_COUNT_OF(dla_car_faces), 3.2f",
        "};","","#endif","",
    ))
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text("\n".join(lines),encoding="utf-8")


def point_camera(camera: bpy.types.Object, target: tuple[float,float,float]) -> None:
    direction=Vector(target)-camera.location
    camera.rotation_euler=direction.to_track_quat("-Z","Y").to_euler()


def render_previews(directory: Path) -> None:
    directory.mkdir(parents=True,exist_ok=True)
    scene=bpy.context.scene
    scene.render.engine="BLENDER_WORKBENCH"
    scene.display.shading.light="STUDIO"
    scene.display.shading.studio_light="paint.sl"
    scene.display.shading.color_type="MATERIAL"
    scene.display.shading.show_shadows=True
    scene.display.shading.show_cavity=True
    scene.display.shading.cavity_type="BOTH"
    scene.render.film_transparent=True
    scene.render.resolution_x=800
    scene.render.resolution_y=500
    scene.render.resolution_percentage=100
    scene.render.image_settings.file_format="PNG"
    camera_data=bpy.data.cameras.new("Orthographic QA camera")
    camera_data.type="ORTHO"
    camera=bpy.data.objects.new("Orthographic QA camera",camera_data)
    bpy.context.collection.objects.link(camera)
    scene.camera=camera
    views=(
        ("front",(0.0,8.0,.66),(0.0,.10,.62),3.05),
        ("rear",(0.0,-8.0,.66),(0.0,-.10,.62),3.05),
        ("side",(-8.0,0.0,.67),(0.0,.15,.64),6.00),
        ("front-three-quarter",(-6.5,8.0,3.0),(0.0,.15,.68),6.10),
        ("rear-three-quarter",(6.5,-8.0,2.7),(0.0,-.10,.68),6.10),
    )
    for name,location,target,scale in views:
        camera.location=location
        camera_data.ortho_scale=scale
        point_camera(camera,target)
        scene.render.filepath=str((directory/f"{name}.png").resolve())
        bpy.ops.render.render(write_still=True)


def main() -> None:
    args=parse_args()
    clear_scene()
    materials={
        "DLA Paint":make_material("DLA Paint",(.72,.75,.81,1.0),metallic=.18,roughness=.30),
        "DLA Glass":make_material("DLA Glass",(.025,.045,.105,1.0),metallic=.20,roughness=.18),
        "DLA Carbon":make_material("DLA Carbon",(.012,.016,.024,1.0),metallic=.08,roughness=.36),
        "DLA Lights":make_material("DLA Lights",(.80,.025,.018,1.0),metallic=.08,roughness=.20),
        "DLA Headlamp":make_material("DLA Headlamp",(.72,.86,1.0,1.0),metallic=.08,roughness=.20),
        "DLA Lamp Lens":make_material("DLA Lamp Lens",(.015,.030,.055,1.0),metallic=.08,roughness=.20),
        "DLA Metal":make_material("DLA Metal",(.34,.38,.45,1.0),metallic=.82,roughness=.20),
    }
    body=create_body(materials)
    create_greenhouse(materials)
    add_surface_details(materials,body)
    add_interior(materials)
    add_qa_wheels(materials)
    vertices,faces,brake_faces=collect_export_mesh()
    write_header(args.header.resolve(),vertices,faces,brake_faces)
    if args.preview_dir:
        render_previews(args.preview_dir.resolve())
    if args.blend:
        args.blend.resolve().parent.mkdir(parents=True,exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.blend.resolve()))
    print(f"DLA Blender car: {len(vertices)} vertices, {len(faces)} triangles -> {args.header}")


if __name__=="__main__":
    main()
