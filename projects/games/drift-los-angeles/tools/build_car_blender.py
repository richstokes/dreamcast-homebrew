#!/usr/bin/env python3
"""Build and export Drift Los Angeles's reference-profiled sports coupe with Blender.

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
    "DLA Metal": METAL,
}

# Chassis constants shared with the game: axle positions, tyre radii and the
# floor height are fixed by the physics and wheel rigs in drift-los-angeles.c.
FRONT_AXLE_Y = 1.55
REAR_AXLE_Y = -1.55
FRONT_ARCH_Z = .445
REAR_ARCH_Z = .465
FRONT_ARCH_R = .47
REAR_ARCH_R = .49
FLOOR_Z = .064
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
    ( .72, .735,  .858),
    ( .50, .700, 1.060),
    ( .24, .660, 1.265),
    (-.02, .635, 1.375),
    (-.36, .635, 1.400),
    (-.68, .650, 1.370),
    (-.80, .660, 1.340),
    (-.96, .680, 1.255),
    (-1.20, .715, 1.090),
    (-1.42, .740,  .960),
    (-1.62, .755,  .885),
    (-1.78, .760,  .846),
)
GREENHOUSE_SILL = .822


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
        station(2.72,  .880, .515, .508, .500, .455, .340, .16),
        station(2.62,  .930, .600, .590, .585, .548, .430, .165),
        station(2.45,  .990, .665, .655, .672, .665, .520),
        station(2.25, 1.030, .725, .705, .775, .805, .585),
        station(2.06, 1.070, .765, .735, .845, .905, .640),
    ]
    # Front arch: seven stations over the opening, densest at the top.
    for degrees in (0, 30, 60, 90, 120, 150, 180):
        angle = math.radians(degrees)
        t = math.sin(angle)
        y = FRONT_AXLE_Y + math.cos(angle) * FRONT_ARCH_R
        stations.append(station(
            y, 1.085 + .015 * t, .78 + .025 * t, .745 + .02 * t,
            .86 + .04 * t, .93 + .04 * t, .66 + .03 * t,
            lip=FRONT_ARCH_Z + FRONT_ARCH_R * t))
    stations.extend([
        station(1.05, 1.055, .800, .780, .845, .875, .650),
        station( .72, 1.030, .800, .790, .815, .835, .630),
        station( .40, 1.025, None, None, .812, .825, .620),
        station( .00, 1.030, None, None, .812, .825, .620),
        station(-.45, 1.035, None, None, .814, .830, .625),
        station(-.85, 1.050, None, None, .818, .845, .640),
        station(-1.04, 1.075, None, None, .822, .880, .660),
    ])
    for degrees in (180, 150, 120, 90, 60, 30, 0):
        angle = math.radians(degrees)
        t = math.sin(angle)
        y = REAR_AXLE_Y + math.cos(angle) * REAR_ARCH_R
        cabin = y >= TUB_REAR_Y
        stations.append(station(
            y, 1.10 + .04 * t, None if cabin else .815, None if cabin else .80,
            .83 + .01 * t, .95 + .05 * t, .68 + .03 * t,
            lip=REAR_ARCH_Z + REAR_ARCH_R * t))
    stations.extend([
        station(-2.06, 1.090, .815, .805, .855, .925, .670),
        station(-2.25, 1.060, .815, .810, .840, .885, .655),
        station(-2.42, 1.030, .825, .822, .842, .870, .650, .16),
        station(-2.55,  .990, .852, .850, .858, .862, .655, .15),
        station(-2.60,  .950, .872, .870, .872, .860, .655, .15),
    ])
    return stations


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
        peak = (-(greenhouse_half(st["y"]) - .005), .812)
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
            (-h * .972, lower + (belt - lower) * .48),
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
            (-h, st["shoulder"] - span * .30),
            (-h, st["shoulder"] - span * .62),
            (-h, lip + .03),
            (-h * .990, lip),
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
    editable = bmesh.new()
    editable.from_mesh(body.data)
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
        left = [
            (-half, sill),
            (-(half - .030), sill + min(.040, rise * .5)),
            (-half * .930, sill + rise * .52),
            (-half * .840, top - min(.035, rise * .4)),
            (-half * .470, top - min(.004, rise * .05)),
        ]
        ring = left + [(0.0, top)] + [(-x, z) for x, z in reversed(left)]
        rings.append((y, ring))
    slots = [paint, glass, carbon]
    # Per row: material for segments 0..4 from the sill to the roof centre.
    windshield = (0, 0, 1, 1, 1)
    roof = (0, 1, 1, 0, 0)
    b_pillar = (0, 2, 2, 0, 0)
    sail = (0, 0, 0, 0, 0)
    hatch = (0, 0, 1, 1, 1)
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


def snap_points(points, tree: BVHTree, offset: float):
    """Project authored points onto the nearest body surface plus an offset."""
    snapped = []
    for point in points:
        location, normal, _index, _distance = tree.find_nearest(Vector(point))
        if location is None:
            snapped.append(tuple(point))
            continue
        moved = location + normal * offset
        snapped.append((moved.x, moved.y, moved.z))
    return tuple(snapped)


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
    tree = surface_tree(body)

    def snapped(points, offset):
        return snap_points(points, tree, offset)

    # Slit lamps that straddle the fender's shoulder crease from the nose
    # corner back toward the wheel, snapped to the new surface.
    left_headlamp_housing = ((-.720,2.690,.520),(-.900,2.520,.610),
                             (-.985,2.240,.780),(-1.000,2.080,.880),
                             (-.900,2.080,.960),(-.855,2.240,.900),
                             (-.760,2.520,.720),(-.600,2.690,.610))
    left_headlamp = ((-.740,2.665,.540),(-.905,2.510,.625),
                     (-.975,2.260,.785),(-.980,2.120,.875),
                     (-.910,2.120,.935),(-.870,2.260,.880),
                     (-.775,2.510,.705),(-.630,2.665,.600))
    for points in (left_headlamp_housing, mirror_points_x(left_headlamp_housing)):
        create_polygon("Headlamp recess", snapped(points, .006), carbon)
    for points in (left_headlamp, mirror_points_x(left_headlamp)):
        create_polygon("Headlamp lens", snapped(points, .012), lights)

    # Nose: grille, crossbar, brake ducts and a splitter with real thickness.
    create_polygon("Front grille", ((-.600,2.728,.430),(.600,2.728,.430),
                                    (.760,2.728,.315),(.655,2.728,.130),
                                    (-.655,2.728,.130),(-.760,2.728,.315)), carbon)
    create_polygon("Grille crossbar", ((-.690,2.736,.282),(.690,2.736,.282),
                                       (.665,2.736,.252),(-.665,2.736,.252)), metal)
    left_front_duct = ((-.815,2.728,.355),(-.960,2.728,.325),
                       (-.920,2.728,.150),(-.790,2.728,.185))
    create_polygon("Left front duct", left_front_duct, carbon)
    create_polygon("Right front duct", mirror_points_x(left_front_duct), carbon)
    create_prism("Front splitter", ((-.905,2.780,.050),(.905,2.780,.050),
                                    (.840,2.560,.050),(-.840,2.560,.050)),
                 (0.0,0.0,.022), carbon)
    create_polygon("Hood extractor", snapped(((-.235,1.38,.84),(.235,1.38,.84),
                                              (.285,1.06,.84),(-.285,1.06,.84)), .006),
                   carbon)
    create_polygon("Front badge red", ((-.012,2.742,.548),(-.115,2.742,.574),
                                       (-.020,2.742,.512),(.005,2.742,.522)), lights)
    create_polygon("Front badge metal", ((.012,2.742,.548),(.115,2.742,.574),
                                         (.020,2.742,.512),(-.005,2.742,.522)), metal)

    # Flank: fender extractor, side marker, door handle, seams and fuel door.
    left_extractor = ((-1.055,1.10,.75),(-1.065,.98,.60),(-1.065,.84,.42),
                      (-1.060,.90,.40),(-1.060,1.03,.58),(-1.055,1.12,.72))
    left_marker = ((-1.05,2.24,.56),(-1.06,2.16,.55),(-1.06,2.16,.40),(-1.05,2.24,.41))
    left_handle = ((-1.04,-.47,.735),(-1.04,-.69,.735),(-1.04,-.69,.712),(-1.04,-.47,.712))
    left_front_seam = ((-1.04,.52,.80),(-1.05,.50,.22),(-1.05,.485,.22),(-1.04,.505,.80))
    left_rear_seam = ((-1.05,-.90,.80),(-1.06,-.93,.22),(-1.06,-.945,.22),(-1.05,-.915,.80))
    for label, points, material, offset in (
        ("Fender extractor", left_extractor, carbon, .008),
        ("Side marker", left_marker, lights, .008),
        ("Door handle", left_handle, carbon, .008),
        ("Front door seam", left_front_seam, carbon, .004),
        ("Rear door seam", left_rear_seam, carbon, .004),
    ):
        create_polygon("Left " + label, snapped(points, offset), material)
        create_polygon("Right " + label, snapped(mirror_points_x(points), offset), material)
    for side in (-1.0, 1.0):
        location, normal, _i, _d = tree.find_nearest(Vector((side*1.2,-1.12,.80)))
        create_ring("Fuel door outline", (location.x + normal.x*.004, location.y, location.z),
                    "x", .075, .062, .004 if side < 0 else -.004, 10, carbon)

    # Door mirrors: a hexagonal head on a short stalk.
    for side in (-1.0, 1.0):
        x = side * 1.09
        head = ((x-.035*side,.40,.845),(x+.075*side,.40,.835),(x+.135*side,.40,.86),
                (x+.135*side,.40,.90),(x+.075*side,.40,.925),(x-.035*side,.40,.915))
        create_prism("Door mirror head", head, (0.0,.10,0.0), paint)
        stalk = ((x-.10*side,.44,.815),(x-.10*side,.50,.815),(x-.02*side,.49,.845),(x-.02*side,.44,.845))
        create_prism("Door mirror stalk", stalk, (0.0,0.0,.022), carbon)

    # Rear fascia: one dark lamp panel with four angular lenses, a plate
    # recess, a diffuser and quad exhausts, all sitting on the tail cap.
    create_polygon("Tail lamp panel", ((-1.02,-2.606,.855),(1.02,-2.606,.855),
                                       (1.04,-2.606,.700),(.94,-2.606,.545),
                                       (-.94,-2.606,.545),(-1.04,-2.606,.700)), carbon)
    left_outer_lamp = ((-.960,-2.614,.812),(-.640,-2.614,.798),
                       (-.595,-2.614,.725),(-.640,-2.614,.615),
                       (-.870,-2.614,.610),(-.975,-2.614,.688))
    left_inner_lamp = ((-.585,-2.614,.795),(-.285,-2.614,.772),
                       (-.235,-2.614,.697),(-.325,-2.614,.607),
                       (-.530,-2.614,.617),(-.615,-2.614,.697))
    for points in (left_outer_lamp, left_inner_lamp,
                   mirror_points_x(left_outer_lamp), mirror_points_x(left_inner_lamp)):
        create_polygon("Tail lamp", points, lights)
    create_polygon("Plate recess", ((-.355,-2.612,.540),(.355,-2.612,.540),
                                    (.320,-2.612,.425),(-.320,-2.612,.425)), carbon)
    create_polygon("Plate", ((-.282,-2.618,.522),(.282,-2.618,.522),
                             (.262,-2.618,.447),(-.262,-2.618,.447)), metal)
    create_prism("Rear diffuser", ((-.930,-2.604,.430),(.930,-2.604,.430),
                                   (.740,-2.596,.060),(-.740,-2.596,.060)),
                 (0.0,-.014,0.0), carbon)
    left_lower_reflector = ((-.905,-2.622,.395),(-.600,-2.622,.395),
                            (-.615,-2.622,.360),(-.890,-2.622,.360))
    create_polygon("Tail lamp lower reflector", left_lower_reflector, lights)
    create_polygon("Tail lamp lower reflector right",
                   mirror_points_x(left_lower_reflector), lights)
    create_polygon("Rear badge red", ((-.025,-2.620,.843),(-.150,-2.620,.870),
                                      (-.035,-2.620,.808),(0.0,-2.620,.820)), lights)
    create_polygon("Rear badge metal", ((.025,-2.620,.843),(.150,-2.620,.870),
                                        (.035,-2.620,.808),(0.0,-2.620,.820)), metal)
    for center_x in (-.285,-.095,.095,.285):
        create_ring("Exhaust tip", (center_x,-2.628,.17), "y", .068, .050, .045, 8, metal)
        create_polygon("Exhaust bore", tuple(
            (center_x+math.cos(a)*.050,-2.629,.17+math.sin(a)*.050)
            for a in (i*math.tau/8 for i in range(8))), carbon)

    # Ducktail lip with a centre stop lamp.
    create_prism("Spoiler lip", ((-.880,-2.05,.868),(.880,-2.05,.868),
                                 (.960,-2.25,.905),(-.960,-2.25,.905)),
                 (0.0,0.0,.012), carbon)
    create_polygon("Center stop lamp", ((-.315,-2.255,.912),(.315,-2.255,.912),
                                        (.292,-2.262,.892),(-.292,-2.262,.892)), lights)


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
    tire_material = make_material("QA Tire", (.018,.020,.025,1.0), roughness=.80)
    rim_material = make_material("QA Rim", (.30,.33,.38,1.0), metallic=.70, roughness=.25)
    for axle_y, radius, width, x_center in ((1.55,.43,.30,.95),(-1.55,.45,.32,.97)):
        for side in (-1.0,1.0):
            bpy.ops.mesh.primitive_cylinder_add(
                vertices=32,radius=radius,depth=width,location=(side*x_center,axle_y,radius+.015),
                rotation=(0.0,math.pi*.5,0.0))
            tire=bpy.context.object
            tire.name="QA Tire"
            tire["export_kos"]=False
            tire.data.materials.append(tire_material)
            smooth_mesh(tire)
            bpy.ops.mesh.primitive_cylinder_add(
                vertices=24,radius=radius*.39,depth=width+.014,
                location=(side*x_center,axle_y,radius+.015),rotation=(0.0,math.pi*.5,0.0))
            rim=bpy.context.object
            rim.name="QA Wheel hub"
            rim["export_kos"]=False
            rim.data.materials.append(rim_material)
            smooth_mesh(rim)
            outer_x=side*(x_center+(width*.5+.014))
            for spoke in range(5):
                angle=spoke*math.tau/5.0
                radial=radius*.43
                bpy.ops.mesh.primitive_cube_add(
                    location=(outer_x,axle_y+math.sin(angle)*radial,
                              radius+.015+math.cos(angle)*radial),
                    scale=(.022,.045,radius*.31),rotation=(angle,0.0,0.0))
                spoke_obj=bpy.context.object
                spoke_obj.name="QA Wheel spoke"
                spoke_obj["export_kos"]=False
                spoke_obj.data.materials.append(rim_material)
                spoke_bevel=spoke_obj.modifiers.new("Spoke edge radius","BEVEL")
                spoke_bevel.width=.012
                spoke_bevel.segments=1
                apply_modifier(spoke_obj,spoke_bevel)


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
            else:
                u = (point[axes[0]] - bounds[0][0]) / max(bounds[0][1] - bounds[0][0], 1e-6)
                v = 1.0 - (point[axes[1]] - bounds[1][0]) / max(bounds[1][1] - bounds[1][0], 1e-6)
                if material == LIGHTS:
                    if name.startswith("headlamp lens"):
                        u, v = .535 + u * .430, .035 + v * .430
                    elif name.startswith("tail lamp"):
                        u, v = .035 + u * .440, .035 + v * .440
                    elif name.endswith("side marker"):
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


def collect_export_mesh() -> tuple[list[tuple[float, ...]], list[tuple[int, int, int, int]]]:
    for obj in bpy.context.scene.objects:
        if obj.type == "MESH" and bool(obj.get("export_kos", False)):
            author_surface_charts(obj)
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    occlusion_tree = build_occlusion_tree()
    vertices: list[tuple[float, ...]] = []
    faces: list[tuple[int, int, int, int]] = []
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
        print(f"  export {obj.name}: {len(vertices)-object_vertex_start} vertices, "
              f"{len(faces)-object_face_start} triangles")
        evaluated.to_mesh_clear()
    used_indices = sorted({index for face in faces for index in face[:3]})
    remap = {previous: current for current, previous in enumerate(used_indices)}
    vertices = [vertices[index] for index in used_indices]
    faces = [(remap[a], remap[b], remap[c], material) for a, b, c, material in faces]
    if len(vertices) > MAX_EXPORT_VERTICES:
        raise RuntimeError(f"mesh has {len(vertices)} vertices; renderer limit is {MAX_EXPORT_VERTICES}")
    faces.sort(key=lambda face: face[3])
    return vertices,faces


def write_header(path: Path, vertices: list[tuple[float, ...]],
                 faces: list[tuple[int, int, int, int]]) -> None:
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
        f"/* Blender reference-profiled coupe: {len(vertices)} vertices, {len(faces)} triangles. */",
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
        ("front-three-quarter",(-6.5,8.0,1.8),(0.0,.15,.61),4.35),
        ("rear-three-quarter",(6.5,-8.0,1.7),(0.0,-.10,.61),4.35),
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
        "DLA Metal":make_material("DLA Metal",(.34,.38,.45,1.0),metallic=.82,roughness=.20),
    }
    body=create_body(materials)
    create_greenhouse(materials)
    add_surface_details(materials,body)
    add_interior(materials)
    add_qa_wheels(materials)
    vertices,faces=collect_export_mesh()
    write_header(args.header.resolve(),vertices,faces)
    if args.preview_dir:
        render_previews(args.preview_dir.resolve())
    if args.blend:
        args.blend.resolve().parent.mkdir(parents=True,exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.blend.resolve()))
    print(f"DLA Blender car: {len(vertices)} vertices, {len(faces)} triangles -> {args.header}")


if __name__=="__main__":
    main()
