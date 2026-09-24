#!/usr/bin/env python3
"""Build and export Drift Los Angeles's reference-profiled sports coupe with Blender.

Run through Blender, not the host Python interpreter:

    blender --background --python tools/build_car_blender.py -- \
        --header model_data.h --preview-dir assets/generated/previews/blender-car

The model uses an authored longitudinal loft and Boolean wheel openings.
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


def create_loft(name: str,
                rings_data: tuple[tuple[float, tuple[tuple[float, float], ...]], ...],
                material: bpy.types.Material,
                *, smooth: bool = True) -> bpy.types.Object:
    """Create a closed longitudinal loft without subdivision.

    The old car was one Catmull-Clark tube.  That made the hood, roof, fenders,
    and sills share the same swollen curvature.  This helper preserves the
    authored C7 shoulder and hood break lines exactly.
    """
    vertices: list[tuple[float, float, float]] = []
    rings: list[list[int]] = []
    for y, profile in rings_data:
        ring: list[int] = []
        for x, z in profile:
            ring.append(len(vertices))
            vertices.append((x, y, z))
        rings.append(ring)
    faces: list[tuple[int, ...]] = []
    ring_size = len(rings[0])
    if any(len(ring) != ring_size for ring in rings):
        raise RuntimeError(f"{name}: loft rings do not share a vertex count")
    for current, following in zip(rings, rings[1:]):
        for edge in range(ring_size):
            nxt = (edge + 1) % ring_size
            faces.append((current[edge], current[nxt], following[nxt], following[edge]))
    faces.append(tuple(reversed(rings[0])))
    faces.append(tuple(rings[-1]))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.materials.append(material)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    obj["export_kos"] = True
    bpy.context.collection.objects.link(obj)
    if smooth:
        smooth_mesh(obj)
    return obj


def lower_body_profile(half: float, center_top: float, hood_edge: float,
                       shoulder: float, belt: float, lower: float,
                       bottom: float) -> tuple[tuple[float, float], ...]:
    """Hard-edged lower-body section, clockwise from the hood center."""
    left = (
        (0.0, center_top),
        (-half * .42, center_top - .006),
        (-half * .77, hood_edge),
        (-half * .975, shoulder),
        (-half, belt),
        (-half * .992, lower),
        (-half * .82, bottom),
        (0.0, bottom - .006),
    )
    return left + tuple((-x, z) for x, z in reversed(left[1:-1]))


def create_canopy(glass: bpy.types.Material,
                  paint: bpy.types.Material,
                  carbon: bpy.types.Material) -> bpy.types.Object:
    """Build a separate cab-back greenhouse with explicit roof and pillars."""
    canopy_sections = (
        ( .72, .710, .845, .825),
        ( .50, .665, 1.055, .825),
        ( .22, .620, 1.270, .822),
        (-.04, .595, 1.365, .820),
        (-.38, .600, 1.390, .820),
        (-.67, .625, 1.355, .822),
        (-.94, .660, 1.245, .828),
        (-1.17, .705, 1.095, .838),
        (-1.34, .750, .950, .848),
        (-1.42, .770, .885, .852),
    )
    rings = []
    for y, half, top, bottom in canopy_sections:
        profile = (
            (0.0, top),
            (-half * .55, top - .004),
            (-half * .90, top - .055),
            (-half, bottom + .105),
            (-half * 1.015, bottom),
            (half * 1.015, bottom),
            (half, bottom + .105),
            (half * .90, top - .055),
            (half * .55, top - .004),
        )
        rings.append((y, profile))
    canopy = create_loft("Continuous glass canopy", tuple(rings), glass)

    # The painted roof panel is deliberately narrow.  Most of the greenhouse
    # remains dark, while the rails and pillars define the C7's fastback shape.
    create_grid_surface("Painted roof panel", (
        ( .02, .420, 1.374, .006),
        (-.34, .445, 1.398, .006),
        (-.67, .465, 1.363, .004),
    ), paint)

    left_a = ((-.718,.704,.825),(-.648,.548,1.040),
              (-.576,.010,1.354),(-.535,.000,1.365),
              (-.606,.560,1.035),(-.680,.716,.824))
    left_b = ((-.626,-.455,1.372),(-.682,-.515,.824),
              (-.720,-.575,.826),(-.668,-.520,1.371))
    left_c = ((-.630,-.680,1.344),(-.724,-1.332,.850),
              (-.765,-1.386,.850),(-.674,-.705,1.349))
    left_rail = ((-.535,.010,1.370),(-.628,-.690,1.350),
                 (-.659,-.700,1.337),(-.565,.005,1.356))
    for label, points, mat in (
        ("A pillar", left_a, paint),
        ("B pillar", left_b, carbon),
        ("C pillar", left_c, paint),
        ("Roof rail", left_rail, paint),
    ):
        create_polygon("Left " + label, points, mat, .008)
        create_polygon("Right " + label, mirror_points_x(points), mat, .008)
    return canopy


def create_body(material: bpy.types.Material, glass: bpy.types.Material,
                carbon: bpy.types.Material) -> bpy.types.Object:
    # y is longitudinal (front positive), z is vertical.  The 5.30:3.10:2.18
    # length/wheelbase/width relationship follows the supplied C7 elevations.
    # The roof is not part of these sections; it is a separate angular canopy.
    stations = (
        ( 2.72,.880,.515,.475,.435,.325,.105,.064),
        ( 2.59,.955,.625,.585,.535,.430,.115,.064),
        ( 2.40,1.000,.660,.680,.690,.515,.125,.064),
        ( 2.18,1.025,.720,.760,.790,.575,.135,.064),
        ( 1.92,1.045,.760,.820,.860,.615,.145,.064),
        ( 1.68,1.060,.790,.870,.900,.645,.150,.064),
        ( 1.52,1.065,.800,.885,.915,.655,.150,.064),
        ( 1.34,1.060,.792,.875,.905,.642,.145,.064),
        ( 1.12,1.045,.785,.850,.885,.625,.140,.064),
        (  .90,1.025,.800,.830,.865,.610,.135,.064),
        (  .70,1.010,.820,.835,.850,.602,.132,.064),
        (  .42,1.010,.810,.810,.825,.607,.132,.064),
        (  .08,1.020,.800,.802,.815,.615,.135,.064),
        ( -.32,1.030,.800,.810,.820,.625,.140,.064),
        ( -.72,1.050,.800,.830,.850,.645,.145,.064),
        (-1.06,1.070,.800,.870,.900,.670,.150,.064),
        (-1.34,1.090,.810,.900,.925,.695,.150,.064),
        (-1.55,1.100,.820,.920,.945,.705,.150,.064),
        (-1.76,1.090,.820,.900,.925,.690,.145,.064),
        (-1.98,1.070,.800,.860,.885,.660,.135,.064),
        (-2.18,1.045,.800,.840,.860,.640,.125,.064),
        (-2.37,1.020,.805,.840,.855,.650,.115,.064),
        (-2.52,1.000,.830,.855,.860,.675,.105,.064),
        (-2.58,.985,.850,.860,.840,.680,.100,.064),
    )
    body_rings = tuple(
        (y, lower_body_profile(half, center, edge, shoulder, belt, lower, bottom))
        for y, half, center, edge, shoulder, belt, lower, bottom in stations
    )
    body = create_loft("C7 lower body", body_rings, material)

    # Shallow side-only wheel openings keep the center chassis intact.
    for axle_y, center_z, cut_radius, lateral in (
        (1.55, .445, .482, 1.035),
        (-1.55, .465, .502, 1.065),
    ):
        for side in (-1.0, 1.0):
            bpy.ops.mesh.primitive_cylinder_add(
                vertices=32,
                radius=cut_radius,
                depth=.28,
                end_fill_type="NGON",
                location=(side*(lateral + .045), axle_y, center_z),
                rotation=(0.0, math.pi*.5, 0.0),
            )
            cutter = bpy.context.object
            cutter.name = "Wheel opening cutter"
            boolean = body.modifiers.new("Wheel opening", "BOOLEAN")
            boolean.operation = "DIFFERENCE"
            boolean.solver = "EXACT"
            boolean.object = cutter
            apply_modifier(body, boolean)
            bpy.data.objects.remove(cutter, do_unlink=True)
    smooth_mesh(body)
    create_canopy(glass, material, carbon)
    return body


def create_grid_surface(name: str,
                        sections: tuple[tuple[float, float, float, float], ...],
                        material: bpy.types.Material) -> bpy.types.Object:
    across = (-1.0, -.68, -.34, 0.0, .34, .68, 1.0)
    vertices: list[tuple[float, float, float]] = []
    for y, half_width, edge_z, crown in sections:
        for factor in across:
            crown_amount = 1.0-factor*factor
            vertices.append((factor*half_width, y, edge_z+crown*crown_amount))
    faces: list[tuple[int, int, int, int]] = []
    width = len(across)
    for row in range(len(sections)-1):
        for column in range(width-1):
            a = row*width+column
            faces.append((a, a+1, a+1+width, a+width))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.materials.append(material)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    obj["export_kos"] = True
    bpy.context.collection.objects.link(obj)
    smooth_mesh(obj)
    return obj


def create_polygon(name: str,
                   points: tuple[tuple[float, float, float], ...],
                   material: bpy.types.Material,
                   thickness: float = 0.0,
                   bevel_width: float = 0.0) -> bpy.types.Object:
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(points, [], [tuple(range(len(points)))])
    mesh.materials.append(material)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    obj["export_kos"] = True
    bpy.context.collection.objects.link(obj)
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


def mirror_points_x(points: tuple[tuple[float, float, float], ...]) -> tuple[tuple[float, float, float], ...]:
    return tuple((-x, y, z) for x, y, z in reversed(points))


def create_arch_lip(name: str, side: float, axle_y: float, center_z: float,
                    radius: float, lateral: float,
                    material: bpy.types.Material) -> None:
    curve=bpy.data.curves.new(name,"CURVE")
    curve.dimensions="3D"
    curve.resolution_u=1
    curve.bevel_depth=.010
    curve.bevel_resolution=0
    spline=curve.splines.new("POLY")
    segments=12
    spline.points.add(segments)
    for index in range(segments+1):
        angle=index*math.pi/segments
        spline.points[index].co=(side*lateral,
                                 axle_y+math.cos(angle)*radius,
                                 center_z+math.sin(angle)*radius,1.0)
    obj=bpy.data.objects.new(name,curve)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(material)
    bpy.context.view_layer.objects.active=obj
    obj.select_set(True)
    bpy.ops.object.convert(target="MESH")
    obj=bpy.context.object
    obj["export_kos"]=True
    smooth_mesh(obj)


def add_surface_details(materials: dict[str, bpy.types.Material]) -> None:
    paint = materials["DLA Paint"]
    glass = materials["DLA Glass"]
    carbon = materials["DLA Carbon"]
    lights = materials["DLA Lights"]
    metal = materials["DLA Metal"]

    # Long, swept C7 lamps.  The lens nearly fills the shallow recess so the
    # black housing reads as a narrow bezel rather than an enormous arrow.
    left_headlamp_housing = ((-.525,2.045,.825),(-.745,1.980,.775),
                             (-.925,2.430,.675),(-.965,2.705,.565),
                             (-.640,2.742,.598),(-.555,2.610,.705))
    left_headlamp = ((-.575,2.110,.800),(-.705,2.065,.770),
                     (-.855,2.455,.680),(-.895,2.688,.595),
                     (-.675,2.710,.620),(-.595,2.590,.706))
    for points in (left_headlamp_housing, mirror_points_x(left_headlamp_housing)):
        create_polygon("Headlamp recess", points, carbon, .003)
    for points in (left_headlamp, mirror_points_x(left_headlamp)):
        create_polygon("Headlamp lens", points, lights)
    create_polygon("Front grille", ((-.610,2.748,.430),(.610,2.748,.430),
                                     (.765,2.748,.315),(.660,2.748,.135),
                                     (-.660,2.748,.135),(-.765,2.748,.315)),
                   carbon, .010, .004)
    create_polygon("Grille crossbar", ((-.690,2.760,.277),(.690,2.760,.277),
                                        (.665,2.760,.250),(-.665,2.760,.250)), metal, .004)
    left_front_duct = ((-.815,2.748,.355),(-.965,2.748,.325),
                       (-.925,2.748,.155),(-.790,2.748,.190))
    create_polygon("Left front duct", left_front_duct, carbon, .010)
    create_polygon("Right front duct", mirror_points_x(left_front_duct), carbon, .010)
    create_polygon("Front splitter", ((-.890,2.752,.078),(.890,2.752,.078),
                                       (.835,2.625,.054),(-.835,2.625,.054)), carbon, .006)
    create_polygon("Hood extractor", ((-.245,1.360,.818),(.245,1.360,.818),
                                      (.295,1.055,.818),(-.295,1.055,.818)), carbon, .005)
    create_polygon("Front badge red", ((-.012,2.760,.545),(-.115,2.760,.570),
                                       (-.020,2.760,.510),(.005,2.760,.520)), lights)
    create_polygon("Front badge metal", ((.012,2.760,.545),(.115,2.760,.570),
                                         (.020,2.760,.510),(-.005,2.760,.520)), metal)

    # Side extractor, rocker and compact mirrors.
    left_extractor = ((-1.045,1.100,.650),(-1.052,.930,.585),
                      (-1.052,.690,.365),(-1.045,.640,.455))
    left_rocker = ((-1.018,.720,.205),(-1.045,-1.030,.205),
                   (-1.015,-.890,.086),(-.995,.610,.086))
    for label, points in (("Fender extractor",left_extractor),("Rocker",left_rocker)):
        create_polygon("Left "+label, points, carbon, .008, .004)
        create_polygon("Right "+label, mirror_points_x(points), carbon, .008, .004)
    left_marker = ((-1.032,2.225,.555),(-1.040,2.145,.545),
                   (-1.040,2.145,.390),(-1.032,2.225,.402))
    create_polygon("Left side marker",left_marker,lights,.002)
    create_polygon("Right side marker",mirror_points_x(left_marker),lights,.002)
    left_handle = ((-1.030,-.475,.735),(-1.035,-.690,.735),
                   (-1.035,-.690,.714),(-1.030,-.475,.714))
    for label,points in (("Door handle",left_handle),):
        create_polygon("Left "+label,points,carbon,.004)
        create_polygon("Right "+label,mirror_points_x(points),carbon,.004)

    # Fine panel gaps prevent the large door skin from reading as one blank
    # slab at 480p.  They sit proud by only a few millimetres.
    left_front_door_seam = ((-1.034,.520,.805),(-1.041,.500,.220),
                            (-1.041,.482,.220),(-1.034,.500,.805))
    left_rear_door_seam = ((-1.045,-.900,.805),(-1.052,-.930,.220),
                           (-1.052,-.950,.220),(-1.045,-.922,.805))
    left_lower_door_crease = ((-1.040,.430,.245),(-1.050,-.855,.245),
                              (-1.050,-.835,.226),(-1.040,.410,.226))
    for label, points in (
        ("Front door seam", left_front_door_seam),
        ("Rear door seam", left_rear_door_seam),
        ("Lower door crease", left_lower_door_crease),
    ):
        create_polygon("Left " + label, points, carbon)
        create_polygon("Right " + label, mirror_points_x(points), carbon)

    bpy.ops.mesh.primitive_torus_add(
        major_radius=.075, minor_radius=.006, major_segments=12,
        minor_segments=3, location=(-1.083,-1.115,.805),
        rotation=(0.0,math.pi*.5,0.0))
    fuel_door=bpy.context.object
    fuel_door.name="Fuel door outline"
    fuel_door["export_kos"]=True
    fuel_door.data.materials.append(carbon)
    for side in (-1.0,1.0):
        bpy.ops.mesh.primitive_cube_add(location=(side*1.055,.465,.875),scale=(.135,.095,.052))
        mirror = bpy.context.object
        mirror.name = "Door mirror"
        mirror["export_kos"] = True
        mirror.data.materials.append(paint)
        bevel = mirror.modifiers.new("Mirror edge radius","BEVEL")
        bevel.width=.025
        bevel.segments=1
        apply_modifier(mirror,bevel)
        smooth_mesh(mirror)

    # Rear fascia: two coherent black basins, each carrying two angular C7
    # lamp cells.  The four lenses are deliberately wide and horizontal.
    left_lamp_basin = ((-1.000,-2.592,.850),(-.205,-2.596,.815),
                       (-.180,-2.600,.680),(-.315,-2.602,.550),
                       (-.900,-2.598,.560),(-1.015,-2.592,.680))
    create_polygon("Left tail lamp basin", left_lamp_basin, carbon, .010)
    create_polygon("Right tail lamp basin", mirror_points_x(left_lamp_basin), carbon, .010)
    left_outer_lamp = ((-.955,-2.608,.808),(-.635,-2.611,.795),
                       (-.590,-2.613,.725),(-.635,-2.614,.615),
                       (-.865,-2.612,.610),(-.970,-2.608,.685))
    left_inner_lamp = ((-.585,-2.613,.793),(-.285,-2.616,.770),
                       (-.235,-2.617,.695),(-.325,-2.618,.605),
                       (-.530,-2.616,.615),(-.615,-2.613,.695))
    tail_lamps=(left_outer_lamp,left_inner_lamp,
                mirror_points_x(left_outer_lamp),mirror_points_x(left_inner_lamp))
    for points in tail_lamps:
        create_polygon("Tail lamp", points, lights, .002)

    # The real C7 rear is not a white rectangle around four lamps: deep black
    # corner vents pull the fascia inward and visually separate the lamps from
    # the lower bumper.  These wedges remain readable in the normal chase view
    # and eliminate the old slab-sided silhouette.
    left_rear_corner_vent = ((-1.055,-2.604,.770),(-.970,-2.610,.590),
                             (-.935,-2.611,.485),(-1.040,-2.606,.420),
                             (-1.090,-2.601,.560))
    create_polygon("Left rear corner vent", left_rear_corner_vent, carbon, .008)
    create_polygon("Right rear corner vent", mirror_points_x(left_rear_corner_vent),
                   carbon, .008)

    # Restrained lower black fascia and center exhaust group.
    create_polygon("Rear diffuser", ((-.955,-2.598,.455),(.955,-2.598,.455),
                                      (.760,-2.590,.060),(-.760,-2.590,.060)), carbon, .016, .004)
    create_polygon("Plate recess", ((-.355,-2.613,.565),(.355,-2.613,.565),
                                    (.320,-2.617,.440),(-.320,-2.617,.440)), carbon, .008, .003)
    create_polygon("Plate", ((-.282,-2.624,.537),(.282,-2.624,.537),
                             (.262,-2.626,.462),(-.262,-2.626,.462)), metal, .004)
    create_polygon("Spoiler lip", ((-.875,-2.015,.842),(.875,-2.015,.842),
                                   (.955,-2.205,.872),(-.955,-2.205,.872)), carbon, .010, .003)
    create_polygon("Center stop lamp", ((-.315,-2.218,.868),(.315,-2.218,.868),
                                        (.292,-2.224,.849),(-.292,-2.224,.849)), lights, .004)
    left_lower_reflector = ((-.910,-2.612,.405),(-.590,-2.615,.405),
                            (-.610,-2.616,.365),(-.890,-2.613,.365))
    create_polygon("Tail lamp lower reflector",left_lower_reflector,lights,.002)
    create_polygon("Tail lamp lower reflector right",
                   mirror_points_x(left_lower_reflector),lights,.002)

    # Tiny crossed-color rear emblem at the visual center of the fascia.
    create_polygon("Rear badge red", ((-.025,-2.626,.842),(-.150,-2.626,.870),
                                      (-.035,-2.626,.807),(0.0,-2.626,.819)), lights)
    create_polygon("Rear badge metal", ((.025,-2.626,.842),(.150,-2.626,.870),
                                        (.035,-2.626,.807),(0.0,-2.626,.819)), metal)

    for center_x in (-.285,-.095,.095,.285):
        bpy.ops.mesh.primitive_torus_add(
            major_radius=.067, minor_radius=.014, major_segments=10,
            minor_segments=3, location=(center_x,-2.625,.17),
            rotation=(math.pi*.5,0.0,0.0))
        tip = bpy.context.object
        tip.name = "Exhaust tip"
        tip["export_kos"] = True
        tip.data.materials.append(metal)
        smooth_mesh(tip)
        bpy.ops.mesh.primitive_cylinder_add(
            vertices=12,radius=.051,depth=.018,location=(center_x,-2.628,.17),
            rotation=(math.pi*.5,0.0,0.0))
        bore=bpy.context.object
        bore.name="Exhaust bore"
        bore["export_kos"]=True
        bore.data.materials.append(carbon)


def add_interior(materials: dict[str, bpy.types.Material]) -> None:
    """Cabin furniture seen through the translucent greenhouse: floor, dash,
    two bucket seats with headrests, a steering wheel and a console."""
    carbon = materials["DLA Carbon"]
    metal = materials["DLA Metal"]
    # Floor and rear deck close the cabin so nothing behind the car shows through.
    create_polygon("Cabin floor", ((-.62,.70,.40),(.62,.70,.40),(.62,-1.40,.40),(-.62,-1.40,.40)), carbon, .02)
    create_polygon("Rear deck", ((-.60,-1.00,.80),(.60,-1.00,.80),(.62,-1.40,.84),(-.62,-1.40,.84)), carbon, .03)
    # Dashboard: a thick wedge under the windshield base with a raised binnacle.
    create_polygon("Dashboard", ((-.64,.72,.78),(.64,.72,.78),(.60,.30,.86),(-.60,.30,.86)), carbon, .16)
    create_polygon("Dash fascia", ((-.60,.30,.86),(.60,.30,.86),(.58,.28,.60),(-.58,.28,.60)), carbon, .04)
    create_polygon("Binnacle", ((-.52,.32,.90),(-.20,.32,.90),(-.20,.20,.94),(-.52,.20,.94)), carbon, .06)
    create_polygon("Centre console", ((-.12,.30,.62),(.12,.30,.62),(.12,-.60,.58),(-.12,-.60,.58)), carbon, .10)
    # Steering wheel: an eight-sided rim disc and a hub on a short column.
    rim = tuple((-.36+.17*math.cos(a), .12-.02*math.sin(a)*0.0, .80+.17*math.sin(a))
                for a in (i*math.pi/4 for i in range(8)))
    create_polygon("Steering wheel", rim, metal, .03)
    create_polygon("Steering hub", ((-.42,.14,.86),(-.30,.14,.86),(-.30,.14,.74),(-.42,.14,.74)), carbon, .04)
    for side, label in ((-1.0,"Driver"),(1.0,"Passenger")):
        x = side*.36
        create_polygon(f"{label} seat base", ((x-.24,-.12,.50),(x+.24,-.12,.50),(x+.24,-.62,.52),(x-.24,-.62,.52)), carbon, .14)
        create_polygon(f"{label} seat back", ((x-.24,-.60,.52),(x+.24,-.60,.52),(x+.22,-.74,1.02),(x-.22,-.74,1.02)), carbon, .12)
        create_polygon(f"{label} headrest", ((x-.13,-.72,1.04),(x+.13,-.72,1.04),(x+.13,-.78,1.20),(x-.13,-.78,1.20)), carbon, .10)
        create_polygon(f"{label} bolster", ((x-.28,-.14,.58),(x+.28,-.14,.58),(x+.28,-.30,.58),(x-.28,-.30,.58)), carbon, .06)


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

    The body has separate hood/roof, side and fascia charts.  Details use their
    own surface-aligned charts, while lamps keep their dedicated atlas cells.
    These materials are reusable surfaces, rather than a unique painted car
    atlas: overlapping charts are intentional, collapsed triangles are not.
    """
    mesh = obj.data
    # Boolean arch cuts leave a few non-planar n-gons.  Give each of their
    # triangles its own corner UVs before choosing a surface chart.  Paper-thin
    # solidify side faces with no measurable area cannot contribute pixels.
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
            # Preserve a consistent scale between matching body panels.  The
            # side chart includes height, unlike the former top-down projection.
            if material in (PAINT, GLASS):
                if axes == (0, 1):
                    u, v = .5 + point.x / 2.5, (2.82 - point.y) / 5.55
                elif axes == (1, 2):
                    u, v = (point.y + 2.73) / 5.55, (1.43 - point.z) / 1.43
                else:
                    u, v = .5 + point.x / 2.5, (1.43 - point.z) / 1.43
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
    create_body(materials["DLA Paint"],materials["DLA Glass"],materials["DLA Carbon"])
    add_surface_details(materials)
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
