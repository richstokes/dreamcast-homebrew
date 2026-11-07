#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Author Drift Los Angeles's traffic and parked cars: atlas, meshes and previews.

Run ``uv run tools/build_vehicles.py --atlas assets/source/vehicle-atlas.png
--header vehicle_data.h``.  Five body types (sedan, hatchback, SUV, van and
pickup) are lofted from cross-section stations along the car's length so the
hood, windshield, roof and tail all read as real surfaces instead of stacked
boxes.  The paintwork regions of the 256x256 atlas are neutral and tinted per
vehicle at draw time; glass, lamps, grille, bumpers and wheels are painted in
place.  Each body is exported at three detail levels in the shared rig format
so the game can spin the wheels and pick a mesh by distance.
"""

from __future__ import annotations

import argparse
import math
import random
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw
from rig_export import Part, fix_winding, normalize, write_obj, write_rig_header

ATLAS = 256
INSET = 1.5 / ATLAS

REGIONS = {
    "paint": (0, 0, 64, 64),
    "side": (64, 0, 128, 64),
    "roof": (192, 0, 64, 64),
    "glass": (0, 64, 64, 64),
    "front": (64, 64, 128, 64),
    "tread": (192, 64, 64, 32),
    "trim": (192, 96, 64, 32),
    "wheel": (0, 128, 64, 64),
    "rear": (64, 128, 128, 64),
    "taxi": (192, 128, 64, 32),
    "bed": (192, 160, 64, 32),
}

KINDS = ["sedan", "hatchback", "suv", "van", "pickup"]
PART_ENUM = ["BODY", "GLASS", "FRONT", "REAR", "TRIM", "WHEEL_FL", "WHEEL_FR",
             "WHEEL_RL", "WHEEL_RR", "TAXI_SIGN"]
SLOT_ENUM = ["PAINT", "GLASS", "FRONT", "REAR", "TRIM", "WHEEL", "ACCENT"]


# ---------------------------------------------------------------------------
# Atlas painting
# ---------------------------------------------------------------------------
def clamp8(value: float) -> int:
    return max(0, min(255, int(round(value))))


def scale_rgb(color, factor):
    return tuple(clamp8(c * factor) for c in color)


def gradient_fill(img: Image.Image, box, top, bottom, streak: float = 0.0, seed: int = 0) -> None:
    x0, y0, w, h = box
    pixels = img.load()
    rng = random.Random(seed)
    for y in range(h):
        t = y / max(1, h - 1)
        base = tuple(top[c] + (bottom[c] - top[c]) * t for c in range(3))
        for x in range(w):
            factor = 1.0 + (rng.random() - .5) * streak
            pixels[x0 + x, y0 + y] = scale_rgb(base, factor)


def paint_atlas() -> Image.Image:
    img = Image.new("RGB", (ATLAS, ATLAS), (110, 110, 110))
    draw = ImageDraw.Draw(img)
    # Paint: a sky-to-ground reflection gradient the tint multiplies.
    gradient_fill(img, REGIONS["paint"], (246, 248, 252), (196, 198, 204), .02, 1)
    x0, y0, w, h = REGIONS["paint"]
    draw.line((x0, y0 + 22, x0 + 63, y0 + 22), fill=(255, 255, 255))
    # Side: doors, handles, sill and a horizontal highlight.  u runs nose to tail.
    gradient_fill(img, REGIONS["side"], (244, 246, 250), (186, 188, 194), .02, 2)
    x0, y0, w, h = REGIONS["side"]
    for seam in (x0 + 34, x0 + 74, x0 + 104):
        draw.line((seam, y0 + 4, seam, y0 + 56), fill=(150, 152, 158))
    for handle in (x0 + 52, x0 + 90):
        draw.rounded_rectangle((handle, y0 + 24, handle + 10, y0 + 28), 2, fill=(140, 142, 148))
    draw.line((x0, y0 + 18, x0 + 127, y0 + 18), fill=(255, 255, 255), width=2)
    draw.rectangle((x0, y0 + 56, x0 + 127, y0 + 63), fill=(120, 122, 128))
    draw.rectangle((x0, y0 + 54, x0 + 127, y0 + 55), fill=(200, 202, 206))
    # Roof and hood: plain paint with a soft centre highlight.
    gradient_fill(img, REGIONS["roof"], (248, 250, 254), (226, 228, 234), .015, 3)
    # Glass: dark tint with a sky streak; not tinted per vehicle.
    gradient_fill(img, REGIONS["glass"], (96, 116, 148), (26, 34, 50), .03, 4)
    x0, y0, w, h = REGIONS["glass"]
    draw.rectangle((x0, y0 + 8, x0 + 63, y0 + 16), fill=(150, 170, 200))
    draw.rectangle((x0, y0 + 17, x0 + 63, y0 + 19), fill=(110, 128, 160))
    draw.rectangle((x0, y0, x0 + 63, y0 + 2), fill=(20, 26, 40))
    # Front face: bumper, lower grille, headlamps, grille bars, badge.
    x0, y0, w, h = REGIONS["front"]
    gradient_fill(img, REGIONS["front"], (58, 60, 66), (34, 36, 42), .04, 5)
    draw.rectangle((x0 + 30, y0 + 44, x0 + 97, y0 + 58), fill=(14, 15, 18))
    for bar in range(46, 58, 4):
        draw.line((x0 + 32, y0 + bar, x0 + 95, y0 + bar), fill=(40, 42, 48))
    draw.rectangle((x0 + 40, y0 + 16, x0 + 87, y0 + 34), fill=(18, 19, 24))
    for bar in range(19, 34, 4):
        draw.line((x0 + 42, y0 + bar, x0 + 85, y0 + bar), fill=(150, 154, 162))
    draw.ellipse((x0 + 60, y0 + 21, x0 + 67, y0 + 28), fill=(200, 204, 212))
    for lamp_x in (x0 + 6, x0 + 96):
        draw.rounded_rectangle((lamp_x, y0 + 14, lamp_x + 26, y0 + 34), 4, fill=(120, 150, 190))
        draw.ellipse((lamp_x + 3, y0 + 16, lamp_x + 23, y0 + 32), fill=(228, 240, 255))
        draw.ellipse((lamp_x + 8, y0 + 19, lamp_x + 18, y0 + 29), fill=(255, 255, 255))
        draw.line((lamp_x + 2, y0 + 36, lamp_x + 24, y0 + 36), fill=(240, 180, 90), width=2)
    draw.rectangle((x0, y0 + 60, x0 + 127, y0 + 63), fill=(20, 21, 24))
    # Rear face: tail lamp bar, plate, bumper, reflectors, exhaust.
    x0, y0, w, h = REGIONS["rear"]
    gradient_fill(img, REGIONS["rear"], (52, 54, 60), (30, 32, 38), .04, 6)
    for lamp_x in (x0 + 4, x0 + 92):
        draw.rounded_rectangle((lamp_x, y0 + 10, lamp_x + 32, y0 + 26), 3, fill=(150, 10, 12))
        draw.rectangle((lamp_x + 2, y0 + 12, lamp_x + 30, y0 + 18), fill=(238, 40, 40))
        draw.rectangle((lamp_x + 2, y0 + 20, lamp_x + 14, y0 + 24), fill=(255, 190, 120))
        draw.rectangle((lamp_x + 16, y0 + 20, lamp_x + 30, y0 + 24), fill=(255, 120, 110))
    draw.rectangle((x0 + 48, y0 + 30, x0 + 79, y0 + 44), fill=(214, 216, 200))
    draw.rectangle((x0 + 50, y0 + 32, x0 + 77, y0 + 42), outline=(90, 92, 84))
    for glyph in range(6):
        draw.rectangle((x0 + 53 + glyph * 4, y0 + 34, x0 + 54 + glyph * 4, y0 + 40), fill=(60, 62, 56))
    draw.rectangle((x0 + 4, y0 + 52, x0 + 14, y0 + 56), fill=(200, 60, 40))
    draw.rectangle((x0 + 113, y0 + 52, x0 + 123, y0 + 56), fill=(200, 60, 40))
    draw.ellipse((x0 + 24, y0 + 54, x0 + 32, y0 + 61), fill=(90, 92, 96), outline=(160, 162, 168))
    draw.rectangle((x0, y0 + 60, x0 + 127, y0 + 63), fill=(18, 19, 22))
    # Wheel: tyre ring, five-spoke rim, hub cap.
    x0, y0, w, h = REGIONS["wheel"]
    draw.rectangle((x0, y0, x0 + 63, y0 + 63), fill=(22, 23, 26))
    draw.ellipse((x0 + 1, y0 + 1, x0 + 62, y0 + 62), fill=(34, 35, 40))
    draw.ellipse((x0 + 4, y0 + 4, x0 + 59, y0 + 59), outline=(58, 60, 66), width=2)
    draw.ellipse((x0 + 12, y0 + 12, x0 + 51, y0 + 51), fill=(70, 72, 78))
    cx, cy = x0 + 32, y0 + 32
    for spoke in range(5):
        angle = -math.pi / 2 + spoke * 2 * math.pi / 5
        draw.line((cx, cy, cx + math.cos(angle) * 19, cy + math.sin(angle) * 19), fill=(178, 182, 190), width=6)
        draw.line((cx, cy, cx + math.cos(angle) * 19, cy + math.sin(angle) * 19), fill=(214, 218, 226), width=2)
    draw.ellipse((cx - 6, cy - 6, cx + 6, cy + 6), fill=(196, 200, 208))
    draw.ellipse((cx - 2, cy - 2, cx + 2, cy + 2), fill=(90, 92, 98))
    # Tread and trim.
    gradient_fill(img, REGIONS["tread"], (32, 33, 36), (24, 25, 28), .08, 7)
    x0, y0, w, h = REGIONS["tread"]
    for groove in range(0, 64, 6):
        draw.line((x0 + groove, y0, x0 + groove, y0 + 31), fill=(16, 17, 19))
    gradient_fill(img, REGIONS["trim"], (56, 58, 64), (30, 32, 36), .03, 8)
    # Taxi roof sign and pickup bed.
    x0, y0, w, h = REGIONS["taxi"]
    draw.rectangle((x0, y0, x0 + 63, y0 + 31), fill=(250, 200, 30))
    draw.rectangle((x0 + 4, y0 + 4, x0 + 59, y0 + 27), outline=(40, 30, 10), width=2)
    draw.text((x0 + 14, y0 + 8), "TAXI", fill=(30, 22, 8))
    gradient_fill(img, REGIONS["bed"], (44, 44, 46), (22, 22, 24), .06, 9)
    x0, y0, w, h = REGIONS["bed"]
    for rib in range(6, 64, 10):
        draw.line((x0 + rib, y0, x0 + rib, y0 + 31), fill=(58, 58, 60))
    return img


# ---------------------------------------------------------------------------
# Body lofting
# ---------------------------------------------------------------------------
@dataclass
class Station:
    z: float
    sill: float        # underside height
    belt: float        # top of the lower body
    hw: float          # half width at the belt line
    top: float         # roof or hood height at this station
    cabin_hw: float    # half width of the greenhouse (roof edge)
    cabin: bool        # this station lies inside the greenhouse


@dataclass
class Body:
    kind: str
    stations: list[Station]
    wheel_z: tuple[float, float]
    wheel_x: float
    wheel_r: float
    glass_pairs: set[int]   # station pairs whose upper bands are windshield/backlight glass
    length: float
    taxi_sign_y: float


def sedan() -> Body:
    return Body("sedan", [
        Station(2.18, .30, .56, .78, .64, .60, False),
        Station(1.86, .28, .74, .86, .78, .70, False),
        Station(.92, .28, .88, .90, .92, .72, True),
        Station(.30, .28, .90, .90, 1.38, .68, True),
        Station(-.72, .28, .90, .90, 1.38, .68, True),
        Station(-1.42, .28, .92, .88, 1.00, .70, True),
        Station(-2.18, .30, .84, .80, .88, .62, False),
    ], (1.32, -1.32), .80, .33, {2, 4}, 4.36, 1.44)


def hatchback() -> Body:
    return Body("hatchback", [
        Station(1.98, .30, .56, .76, .64, .58, False),
        Station(1.66, .28, .74, .84, .80, .68, False),
        Station(.80, .28, .88, .88, .94, .70, True),
        Station(.20, .28, .90, .88, 1.42, .66, True),
        Station(-.90, .28, .90, .88, 1.42, .66, True),
        Station(-1.62, .28, .92, .86, 1.18, .68, True),
        Station(-1.98, .30, .90, .78, .96, .64, False),
    ], (1.22, -1.22), .78, .32, {2, 4, 5}, 3.96, 1.48)


def suv() -> Body:
    return Body("suv", [
        Station(2.30, .40, .70, .82, .82, .66, False),
        Station(1.98, .38, .92, .92, .98, .74, False),
        Station(1.00, .38, 1.02, .94, 1.10, .76, True),
        Station(.40, .38, 1.04, .94, 1.72, .72, True),
        Station(-1.10, .38, 1.04, .94, 1.72, .72, True),
        Station(-1.90, .38, 1.04, .92, 1.62, .72, True),
        Station(-2.30, .40, 1.00, .84, 1.20, .66, False),
    ], (1.42, -1.42), .84, .37, {2, 5}, 4.60, 1.78)


def van() -> Body:
    return Body("van", [
        Station(2.35, .36, .66, .84, .74, .70, False),
        Station(2.05, .34, .90, .92, 1.02, .78, False),
        Station(1.45, .34, .98, .94, 1.30, .80, True),
        Station(.85, .34, 1.00, .94, 1.92, .80, True),
        Station(-1.30, .34, 1.00, .94, 1.94, .80, True),
        Station(-2.10, .34, 1.00, .94, 1.90, .80, True),
        Station(-2.35, .36, .96, .86, 1.60, .74, False),
    ], (1.55, -1.55), .84, .35, {2, 5}, 4.70, 1.98)


def pickup() -> Body:
    return Body("pickup", [
        Station(2.45, .38, .66, .84, .78, .68, False),
        Station(2.10, .36, .90, .94, .96, .76, False),
        Station(1.05, .36, 1.00, .96, 1.06, .78, True),
        Station(.45, .36, 1.02, .96, 1.70, .74, True),
        Station(-.40, .36, 1.02, .96, 1.70, .74, True),
        Station(-.70, .36, 1.02, .96, 1.04, .78, False),
        Station(-2.45, .38, 1.00, .90, 1.02, .76, False),
    ], (1.55, -1.45), .86, .37, {2, 4}, 4.90, 1.76)


BODIES = {"sedan": sedan, "hatchback": hatchback, "suv": suv, "van": van, "pickup": pickup}


def region_uv(region: str, u: float, v: float) -> tuple[float, float]:
    x0, y0, w, h = REGIONS[region]
    u = min(max(u, 0.0), 1.0)
    v = min(max(v, 0.0), 1.0)
    return ((x0 + INSET * ATLAS + u * (w - 2 * INSET * ATLAS)) / ATLAS,
            (y0 + INSET * ATLAS + v * (h - 2 * INSET * ATLAS)) / ATLAS)


def section_points(st: Station, detail: int) -> list[tuple[float, float]]:
    """Right-hand side profile from the underside up to the roof centre line."""
    if detail == 2:
        return [(st.hw * .92, st.sill), (st.hw, st.belt), (st.cabin_hw * .9, st.top)]
    if detail == 1:
        return [(st.hw * .92, st.sill), (st.hw, st.belt - .04), (st.cabin_hw, st.top - .05),
                (st.cabin_hw * .84, st.top)]
    return [(st.hw * .92, st.sill), (st.hw * .98, st.sill + .10), (st.hw, st.belt - .05),
            (st.cabin_hw + .05, st.belt + .02), (st.cabin_hw, st.top - .06), (st.cabin_hw * .84, st.top)]


def band_material(body: Body, pair: int, band: int, bands: int, detail: int) -> str:
    """Material of the loft band between profile points band and band+1."""
    a, b = body.stations[pair], body.stations[pair + 1]
    if detail == 2:
        return "GLASS" if band == 1 and (pair in body.glass_pairs or (a.cabin and b.cabin)) else "PAINT"
    window_band = 2 if detail == 1 else 3
    if band == 0:
        return "TRIM"
    if band == window_band:
        if a.cabin and b.cabin:
            return "GLASS"
    if band >= window_band and pair in body.glass_pairs:
        return "GLASS"
    return "PAINT"


def build_vehicle(body: Body, detail: int) -> list[Part]:
    parts: dict[str, Part] = {}
    order = ["body", "glass", "front", "rear", "trim", "wheel_fl", "wheel_fr", "wheel_rl", "wheel_rr", "taxi_sign"]
    slots = {"body": "paint", "glass": "glass", "front": "front", "rear": "rear", "trim": "trim",
             "wheel_fl": "wheel", "wheel_fr": "wheel", "wheel_rl": "wheel", "wheel_rr": "wheel",
             "taxi_sign": "accent"}
    regions = {"body": "side", "glass": "glass", "front": "front", "rear": "rear", "trim": "trim",
               "wheel_fl": "wheel", "wheel_fr": "wheel", "wheel_rl": "wheel", "wheel_rr": "wheel",
               "taxi_sign": "taxi"}
    for name in order:
        if name.startswith("wheel"):
            side = -1.0 if name.endswith("l") else 1.0
            z = body.wheel_z[0] if name[6] == "f" else body.wheel_z[1]
            pivot = (side * body.wheel_x, body.wheel_r, z)
            parent = 0
        else:
            pivot = (0.0, 0.0, 0.0)
            parent = -1 if name == "body" else 0
        parts[name] = Part(name, parent, pivot, regions[name], slots[name])

    stations = body.stations
    if detail == 1:
        stations = [stations[0], stations[2], stations[3], stations[4], stations[5], stations[6]]
        glass_pairs = {1 if 2 in body.glass_pairs else -1, 3 if 4 in body.glass_pairs else -1,
                       4 if 5 in body.glass_pairs else -1}
        body = Body(body.kind, stations, body.wheel_z, body.wheel_x, body.wheel_r,
                    {g for g in glass_pairs if g >= 0}, body.length, body.taxi_sign_y)
    elif detail == 2:
        stations = [stations[0], stations[2], stations[3], stations[4], stations[6]]
        body = Body(body.kind, stations, body.wheel_z, body.wheel_x, body.wheel_r,
                    {1, 3}, body.length, body.taxi_sign_y)
    profiles = [section_points(st, detail) for st in stations]
    bands = len(profiles[0]) - 1
    front_z = stations[0].z
    length = stations[0].z - stations[-1].z

    def side_normal(profile, index, sign):
        prev_p = profile[max(0, index - 1)]
        next_p = profile[min(len(profile) - 1, index + 1)]
        tx, ty = next_p[0] - prev_p[0], next_p[1] - prev_p[1]
        n = normalize((ty * sign, -tx, 0.0)) if (tx or ty) else (sign, 0.0, 0.0)
        # Profiles run outward-up, so the outward normal is (dy, -dx) on the right.
        if n[0] * sign < 0.0 or (abs(n[0]) < 1e-3 and n[1] < 0.0):
            n = (-n[0], -n[1], n[2])
        return n

    def station_slope(station_index: int, point_index: int) -> float:
        """Height change per unit length toward the nose, from neighbouring stations."""
        prev_i = max(0, station_index - 1)
        next_i = min(len(stations) - 1, station_index + 1)
        dy = profiles[prev_i][point_index][1] - profiles[next_i][point_index][1]
        dz = stations[prev_i].z - stations[next_i].z
        return dy / dz if abs(dz) > 1e-4 else 0.0

    def emit_ring_vertex(part: Part, station_index: int, point_index: int, sign: float,
                         region: str, u: float, v: float, slope: float) -> int:
        px, py = profiles[station_index][point_index]
        n = side_normal(profiles[station_index], point_index, sign)
        # Normals depend only on the station and profile point so that the
        # vertex can be shared by every band and pair that touches it.
        rise = station_slope(station_index, point_index)
        n = normalize((n[0], n[1] + max(0.0, rise) * .35, rise * .55))
        del slope
        return part.add_vertex((px * sign, py, stations[station_index].z), region_uv(region, u, v), n)

    # Side and top bands between consecutive stations, mirrored left/right.
    for pair in range(len(stations) - 1):
        z_a, z_b = stations[pair].z, stations[pair + 1].z
        u_a, u_b = (front_z - z_a) / length, (front_z - z_b) / length
        for band in range(bands):
            material = band_material(body, pair, band, bands, detail)
            part = parts["body"] if material == "PAINT" else parts[material.lower()]
            region = {"PAINT": "side", "GLASS": "glass", "TRIM": "trim"}[material]
            slope_a = profiles[pair][band + 1][1] - profiles[pair + 1][band + 1][1]
            for sign in (1.0, -1.0):
                v0 = band / bands
                v1 = (band + 1) / bands
                if material == "GLASS":
                    v0, v1 = .15 + .7 * (bands - band - 1) / bands, .15 + .7 * (bands - band) / bands
                base = len(part.vertices)
                emit_ring_vertex(part, pair, band, sign, region, u_a, 1.0 - v0, slope_a)
                emit_ring_vertex(part, pair, band + 1, sign, region, u_a, 1.0 - v1, slope_a)
                emit_ring_vertex(part, pair + 1, band, sign, region, u_b, 1.0 - v0, slope_a)
                emit_ring_vertex(part, pair + 1, band + 1, sign, region, u_b, 1.0 - v1, slope_a)
                part.faces.append((base, base + 1, base + 3))
                part.faces.append((base, base + 3, base + 2))
        # Top strip between the two roof-edge points.
        top_material = "GLASS" if pair in body.glass_pairs else "PAINT"
        part = parts["body"] if top_material == "PAINT" else parts["glass"]
        region = "roof" if top_material == "PAINT" else "glass"
        rise = profiles[pair][-1][1] - profiles[pair + 1][-1][1]
        n = normalize((0.0, 1.0, rise / max(1e-3, z_a - z_b) * .9))
        base = len(part.vertices)
        for (si, u) in ((pair, u_a), (pair + 1, u_b)):
            px, py = profiles[si][-1]
            vv = .5 + (u - .5) * .9 if region == "roof" else u
            part.add_vertex((px, py, stations[si].z), region_uv(region, .1 if region == "roof" else .2, vv), n)
            part.add_vertex((-px, py, stations[si].z), region_uv(region, .9 if region == "roof" else .8, vv), n)
        part.faces.append((base, base + 2, base + 3))
        part.faces.append((base, base + 3, base + 1))
        # Underside strip.
        part = parts["trim"]
        base = len(part.vertices)
        for si in (pair, pair + 1):
            px, py = profiles[si][0]
            part.add_vertex((px, py, stations[si].z), region_uv("trim", .1, .5), (0.0, -1.0, 0.0))
            part.add_vertex((-px, py, stations[si].z), region_uv("trim", .9, .5), (0.0, -1.0, 0.0))
        part.faces.append((base, base + 3, base + 2))
        part.faces.append((base, base + 1, base + 3))

    # Front and rear caps: fans over the full profile mapped to the face regions.
    for (si, region, normal_z) in ((0, "front", 1.0), (len(stations) - 1, "rear", -1.0)):
        part = parts[region]
        profile = profiles[si]
        st = stations[si]
        ring = [(px, py) for (px, py) in profile] + [(-px, py) for (px, py) in reversed(profile)]
        y_min, y_max = st.sill, st.top
        indices = []
        for (px, py) in ring:
            u = .5 + px / (2.0 * st.hw) * .96 * normal_z
            v = 1.0 - (py - y_min) / max(1e-3, y_max - y_min)
            indices.append(part.add_vertex((px, py, st.z), region_uv(region, u, v), (0.0, .0, normal_z)))
        centre = part.add_vertex((0.0, (y_min + y_max) * .5, st.z), region_uv(region, .5, .5), (0.0, 0.0, normal_z))
        for i in range(len(ring)):
            part.faces.append((centre, indices[i], indices[(i + 1) % len(ring)]))

    # Mirrors and a roof sign for taxis; a bed liner for pickups.
    if detail == 0:
        cabin_start = next(s for s in stations if s.cabin)
        for sign in (-1.0, 1.0):
            part = parts["trim"]
            mx, my, mz = sign * (cabin_start.cabin_hw + .12), cabin_start.belt + .13, cabin_start.z - .06
            add_box(part, (mx - .08, my - .05, mz - .09), (mx + .08, my + .05, mz + .05), "trim")
            add_box(part, (sign * cabin_start.cabin_hw, my - .02, mz - .03),
                    (mx, my + .02, mz + .01), "trim")
    part = parts["taxi_sign"]
    add_box(part, (-.26, body.taxi_sign_y - .02, -.16), (.26, body.taxi_sign_y + .12, .16), "taxi")
    if body.kind == "pickup":
        part = parts["trim"]
        add_box(part, (-.80, .96, -2.30), (.80, 1.02, -.75), "bed")

    # Wheels: outer rim disc, tread, inner disc. The far mesh has none; its
    # contact shadow covers the gap at that range.
    segments = 8 if detail == 0 else 6
    for name in ("wheel_fl", "wheel_fr", "wheel_rl", "wheel_rr") if detail < 2 else ():
        part = parts[name]
        side = -1.0 if name.endswith("l") else 1.0
        r = body.wheel_r
        half_w = .11
        outer = []
        inner = []
        for i in range(segments):
            a = 2.0 * math.pi * i / segments
            cy, cz = math.sin(a) * r, math.cos(a) * r
            u, v = .5 + math.cos(a) * .48 * side, .5 - math.sin(a) * .48
            outer.append(part.add_vertex((side * half_w, cy, cz), region_uv("wheel", u, v), (side, 0.0, 0.0)))
            inner.append(part.add_vertex((-side * half_w, cy, cz), region_uv("wheel", u, v), (-side, 0.0, 0.0)))
        oc = part.add_vertex((side * half_w, 0.0, 0.0), region_uv("wheel", .5, .5), (side, 0.0, 0.0))
        for i in range(segments):
            part.faces.append((oc, outer[i], outer[(i + 1) % segments]))
        if detail < 2:
            ic = part.add_vertex((-side * half_w, 0.0, 0.0), region_uv("wheel", .5, .5), (-side, 0.0, 0.0))
            for i in range(segments):
                part.faces.append((ic, inner[(i + 1) % segments], inner[i]))
        for i in range(segments):
            a0 = 2.0 * math.pi * i / segments
            a1 = 2.0 * math.pi * (i + 1) / segments
            n0 = (0.0, math.sin(a0), math.cos(a0))
            n1 = (0.0, math.sin(a1), math.cos(a1))
            base = len(part.vertices)
            for (a, n) in ((a0, n0), (a1, n1)):
                cy, cz = math.sin(a) * r, math.cos(a) * r
                uu = (i + (0 if a == a0 else 1)) / segments
                part.add_vertex((side * half_w, cy, cz), region_uv("tread", uu, .1), n)
                part.add_vertex((-side * half_w, cy, cz), region_uv("tread", uu, .9), n)
            part.faces.append((base, base + 2, base + 3))
            part.faces.append((base, base + 3, base + 1))

    result = [parts[name] for name in order]
    for part in result:
        fix_winding(part)
    return result


def add_box(part: Part, lo, hi, region: str) -> None:
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    corners = {
        "lbb": (x0, y0, z0), "rbb": (x1, y0, z0), "lbf": (x0, y0, z1), "rbf": (x1, y0, z1),
        "ltb": (x0, y1, z0), "rtb": (x1, y1, z0), "ltf": (x0, y1, z1), "rtf": (x1, y1, z1),
    }
    faces = (
        (("ltb", "rtb", "rtf", "ltf"), (0.0, 1.0, 0.0)),
        (("lbf", "rbf", "rbb", "lbb"), (0.0, -1.0, 0.0)),
        (("ltf", "rtf", "rbf", "lbf"), (0.0, 0.0, 1.0)),
        (("rtb", "ltb", "lbb", "rbb"), (0.0, 0.0, -1.0)),
        (("ltb", "ltf", "lbf", "lbb"), (-1.0, 0.0, 0.0)),
        (("rtf", "rtb", "rbb", "rbf"), (1.0, 0.0, 0.0)),
    )
    for names, normal in faces:
        base = len(part.vertices)
        for name, (u, v) in zip(names, ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0))):
            c = corners[name]
            part.add_vertex((c[0] - part.pivot[0], c[1] - part.pivot[1], c[2] - part.pivot[2]),
                            region_uv(region, u, v), normal)
        part.faces.append((base, base + 1, base + 2))
        part.faces.append((base, base + 2, base + 3))


# ---------------------------------------------------------------------------
# Preview
# ---------------------------------------------------------------------------
PAINTS = [(.62, .08, .07), (.06, .22, .52), (.66, .68, .72), (.05, .06, .08), (.50, .30, .06),
          (.08, .34, .24), (.90, .90, .92), (.94, .66, .10)]


def write_preview(directory: Path, atlas: Image.Image, meshes: dict[str, list[Part]]) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    column = 0
    layout = [("sedan", 0), ("hatchback", 0), ("suv", 0), ("van", 0), ("pickup", 0), ("sedan", 1), ("suv", 2)]
    for kind, detail in layout:
        parts = meshes[f"dla_vehicle_{kind}_lod{detail}"]
        paint = PAINTS[column % len(PAINTS)]
        taxi = column == 5
        if taxi:
            paint = (.94, .70, .08)
        tinted = atlas.copy()
        pixels = tinted.load()
        for region in ("paint", "side", "roof"):
            x0, y0, w, h = REGIONS[region]
            for y in range(y0, y0 + h):
                for x in range(x0, x0 + w):
                    p = pixels[x, y]
                    pixels[x, y] = (clamp8(p[0] * paint[0]), clamp8(p[1] * paint[1]), clamp8(p[2] * paint[2]))
        tinted.save(directory / f"variant{column}.png")
        yaw = -.55 + column * .12
        c, s = math.cos(yaw), math.sin(yaw)
        world = []
        spin = column * .7
        for part in parts:
            for v in part.vertices:
                x, y, z = v[0], v[1], v[2]
                if part.name.startswith("wheel"):
                    y, z = y * math.cos(spin) + z * math.sin(spin), -y * math.sin(spin) + z * math.cos(spin)
                x += part.pivot[0]
                y += part.pivot[1]
                z += part.pivot[2]
                world.append((x * c + z * s, y, -x * s + z * c))
        skip = [part.name == "taxi_sign" and not taxi for part in parts]
        write_obj(directory / f"variant{column}.obj", f"variant{column}", world, parts,
                  [(0.0, 0.0)] * len(parts), skip, x_offset=column * 2.6)
        column += 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--atlas", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    atlas = paint_atlas()
    args.atlas.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(args.atlas, optimize=True)
    meshes: dict[str, list[Part]] = {}
    for kind in KINDS:
        for detail in range(3):
            parts = build_vehicle(BODIES[kind](), detail)
            meshes[f"dla_vehicle_{kind}_lod{detail}"] = parts
            print(f"{kind} LOD{detail}: {sum(len(p.vertices) for p in parts)} vertices, "
                  f"{sum(len(p.faces) for p in parts)} triangles")
    write_rig_header(
        args.header, "DRIFT_LA_VEHICLE_DATA_H", "tools/build_vehicles.py",
        "DLA_VEH_", PART_ENUM, "DLA_VEH_SLOT_", SLOT_ENUM, meshes,
        ["enum { " + ", ".join(f"DLA_VEHICLE_{kind.upper()}" for kind in KINDS) + ", DLA_VEHICLE_KIND_COUNT };"],
        "dla_vehicle_lods", "[DLA_VEHICLE_KIND_COUNT][3]",
        [[f"dla_vehicle_{kind}_lod{detail}" for detail in range(3)] for kind in KINDS])
    if args.preview_dir:
        write_preview(args.preview_dir, atlas, meshes)


if __name__ == "__main__":
    main()
