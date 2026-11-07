#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26"]
# ///
"""Procedurally build DreamKarts' go-kart and Chao-like driver meshes.

Usage:
    uv run tools/build_models.py                      # writes assets/generated/models.h
    uv run tools/build_models.py --preview-json X.json  # also dumps meshes for
                                                        # tools/preview_models_blender.py

Conventions (see the header comments for the runtime contract):
  * Meters; +X = kart's right, +Y = up, +Z = forward. y = 0 is the ground.
  * For every triangle (a, b, c), cross(b - a, c - a) points out of the surface.
  * Vertices are stored relative to their part's pivot.
  * Every vertex carries a paint slot (0 fixed, 1 kart primary, 2 kart accent,
    3 Chao body, 4 Chao tip) and a 0..255 blend weight toward the Chao tip colour.

Everything is built in kart space from a handful of primitives (ellipsoid,
tube/capsule/cone/torus, lathe/tyre, loft/box) and assigned to a part. Hard
surface primitives get angle-based auto-smoothing (split normals at creases);
organic ones get analytic smooth normals. Identical vertices are welded per mesh.
Output is deterministic.
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
import meshasm  # noqa: E402
from meshasm import cf, check_mesh, index_rows, mesh_json, mesh_rows, pack, vert_rows, weld, write_json  # noqa: E402
from meshkit import (Geo, Paint, _orient, box, ellipsoid, frame_from, geosphere, lathe,  # noqa: E402
                     lerp_rgb, loft, merge, prism, rect, rgb, rot_x, rot_y, smoothstep, surface_point, tube, unit)

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "assets" / "generated" / "models.h"

# --------------------------------------------------------------------------
# Parts, pivots and layout constants
# --------------------------------------------------------------------------

PART_NAMES = [
    "DK_PART_BODY", "DK_PART_WHEEL_FL", "DK_PART_WHEEL_FR", "DK_PART_WHEEL_RL",
    "DK_PART_WHEEL_RR", "DK_PART_STEER", "DK_PART_CHAO_BODY", "DK_PART_CHAO_HEAD",
    "DK_PART_EMOTE", "DK_PART_ARM_L", "DK_PART_ARM_R", "DK_PART_HAND_L", "DK_PART_HAND_R",
]
BODY, WFL, WFR, WRL, WRR, STEER, CBODY, CHEAD, EMOTE, ARM_L, ARM_R, HAND_L, HAND_R = range(len(PART_NAMES))

FRONT_R, FRONT_W, FRONT_X, FRONT_Z = 0.21, 0.17, 0.555, 0.625
REAR_R, REAR_W, REAR_X, REAR_Z = 0.25, 0.25, 0.575, -0.625

STEER_HUB = np.array([0.0, 0.61, 0.09])
_col = math.radians(32.0)                    # column angle above horizontal
STEER_AXIS = np.array([0.0, math.sin(_col), -math.cos(_col)])  # hub -> driver (up/back)
STEER_RIM_R = 0.15

HEAD_C = np.array([0.0, 1.09, -0.23])
HEAD_R = np.array([0.42, 0.36, 0.38])        # wider than tall; top at y = 1.45
TORSO_C = np.array([0.0, 0.50, -0.27])
TORSO_R = np.array([0.215, 0.24, 0.20])
EMOTE_C = np.array([0.0, 1.75, -0.20])
SHOULDERS = [np.array([s * 0.14, 0.60, -0.21]) for s in (-1.0, 1.0)]   # left, right

EXHAUST_TIPS = [np.array([s * 0.14, 0.38, -1.0]) for s in (-1.0, 1.0)]

PIVOTS = {
    BODY: np.zeros(3),
    WFL: np.array([-FRONT_X, FRONT_R, FRONT_Z]),
    WFR: np.array([FRONT_X, FRONT_R, FRONT_Z]),
    WRL: np.array([-REAR_X, REAR_R, REAR_Z]),
    WRR: np.array([REAR_X, REAR_R, REAR_Z]),
    STEER: STEER_HUB,
    CBODY: np.array([0.0, TORSO_C[1] - TORSO_R[1], TORSO_C[2]]),   # seat contact
    CHEAD: np.array([0.0, 0.73, -0.25]),                          # neck
    EMOTE: EMOTE_C,
    # Arms swing about the shoulder to wave and cheer; each hand grips the
    # wheel (following the steering) until its arm lets go.
    ARM_L: SHOULDERS[0],
    ARM_R: SHOULDERS[1],
}

# Mesh flags: bit0 unlit, bit1 drawn only while that arm is off the wheel.
FLAG_UNLIT = 1
FLAG_ARM_RAISED = 2

BUDGET_KART_LOD0 = 420
BUDGET_CHAO_LOD0 = 420
BUDGET_LOD1 = 330
BUDGET_LOD3 = 150
BUDGET_VERTS = 1640

# --------------------------------------------------------------------------
# Colours
# --------------------------------------------------------------------------


RED = Paint(rgb(0xE8, 0x16, 0x1E), 1)
WHITE = Paint(rgb(0xF6, 0xF6, 0xF2), 2)
YELLOW = Paint(rgb(0xFF, 0xC8, 0x12))
GUNMETAL = Paint(rgb(0x4A, 0x4E, 0x58))
DARK = Paint(rgb(0x30, 0x32, 0x38))
SEAT = Paint(rgb(0x26, 0x26, 0x2C))
TYRE = Paint(rgb(0x1E, 0x1E, 0x22))
GRIP = Paint(rgb(0x22, 0x22, 0x26))
PIPE_HOLE = Paint(rgb(0x10, 0x10, 0x12))
HUB_CAP = Paint(rgb(0xFF, 0xF4, 0xC0))

CHAO_BODY_RGB = rgb(0x98, 0xD6, 0xFF)   # light sky blue
CHAO_TIP_RGB = rgb(0xFF, 0xE2, 0x48)    # yellow
EYE = Paint(rgb(0x14, 0x12, 0x22))
EYE_SHINE = Paint(rgb(0xFF, 0xFF, 0xFF))
MOUTH = Paint(rgb(0x4A, 0x16, 0x26))
EMOTE_BALL = Paint(rgb(0xFF, 0xE4, 0x40))
EMOTE_BANG = Paint(rgb(0xFF, 0xD0, 0x1C))
EMOTE_HEART = Paint(rgb(0xFF, 0x4C, 0x7C))


def chao(t: float) -> Paint:
    """Chao skin: blend weight t (0 = body colour, 1 = tip colour)."""
    t = min(1.0, max(0.0, t))
    w = int(round(t * 255))
    return Paint(lerp_rgb(CHAO_BODY_RGB, CHAO_TIP_RGB, w / 255.0), 4 if w >= 128 else 3, w)


def chrome(n: np.ndarray) -> Paint:
    """Fake chrome: bright when facing up, darker underneath."""
    t = 0.5 + 0.5 * float(n[1])
    return Paint(lerp_rgb(rgb(0x6C, 0x72, 0x7E), rgb(0xF4, 0xF8, 0xFF), t ** 0.8))


# --------------------------------------------------------------------------
# Kart
# --------------------------------------------------------------------------


def tyre(radius: float, width: float, segs: int) -> Geo:
    """Right-hand LOD0 wheel in axle space (outer face on +X): flat inner face,
    faceted tread, sloped shoulder down to a domed yellow hub with a bright cap."""
    w = 0.5 * width
    profile = [
        (-w, 0.0),                      # inner face centre
        (-w, radius),                   # inner edge
        (w - 0.22 * width, radius),     # tread
        (w, radius * 0.64),             # outer shoulder / sidewall
        (w + 0.025, 0.0),               # hub cap
    ]

    # Black rubber everywhere except the hub fan (band 3), which blends from
    # the yellow rim (ring 3) to a pale cap (ring 4).
    return lathe(profile, segs, lambda k: HUB_CAP if k == 4 else YELLOW, 40.0,
                 face_paint=lambda i, j: None if i == 3 else TYRE, phase=math.pi / segs)


def nose_section(z, hw, yb, yt):
    h = yt - yb
    return [(hw, yb, z), (hw, yb + 0.48 * h, z), (0.80 * hw, yb + 0.86 * h, z),
            (0.30 * hw, yt, z), (-0.30 * hw, yt, z), (-0.80 * hw, yb + 0.86 * h, z),
            (-hw, yb + 0.48 * h, z), (-hw, yb, z)]


def pod_section(z, s=1.0, drop=0.0):
    """Right side pod outline at z, open underneath. s shrinks towards the centre."""
    cx, cy = 0.465, 0.17
    pts = [(0.625, 0.075), (0.66, 0.175), (0.605, 0.28), (0.37, 0.305), (0.30, 0.26), (0.30, 0.075)]
    return [(cx + (x - cx) * s, cy + (y - cy) * s - drop, z) for x, y in pts]


def build_kart_lod0() -> dict[int, list[tuple[Geo, int]]]:
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    body = parts[BODY]

    # Nose cone / front cowl with a white racing stripe; dark dash facing the driver.
    nose = loft([
        nose_section(0.975, 0.16, 0.13, 0.225),
        nose_section(0.86, 0.29, 0.085, 0.29),
        nose_section(0.24, 0.29, 0.08, 0.475),
    ], RED, 50.0, closed_loop=False, face_paint=lambda i, j: WHITE if j == 3 else None)
    nose.repaint(lambda c, n, p: DARK if n[2] < -0.9 else None)
    body.append((nose, 0))

    # Front bumper bar.
    body.append((tube([(-0.47, 0.15, 0.70), (-0.34, 0.15, 0.985), (0.34, 0.15, 0.985), (0.47, 0.15, 0.70)],
                      [0.03] * 4, 4, YELLOW, phase=math.pi / 4), 0))

    # Floor pan (top only; pods and nose hide its edges).
    floor = Geo()
    fl = [(0.30, 0.10, 0.70), (0.30, 0.10, -0.80), (-0.30, 0.10, -0.80), (-0.30, 0.10, 0.70)]
    floor.tri((fl[0], fl[1], fl[2]), None, DARK)
    floor.tri((fl[0], fl[2], fl[3]), None, DARK)
    _orient(floor, lambda c: (0, 1, 0))
    body.append((floor.flat(), 0))

    # Side pods (right, mirrored left), white stripe along the upper outer face.
    pod = loft([pod_section(0.415, 0.62, 0.015), pod_section(-0.375, 0.92)], RED, 40.0, closed_loop=False,
               face_paint=lambda i, j: WHITE if j == 1 else None)
    body.append((pod, 0))
    body.append((pod.mirrored_x(), 0))

    # Seat pan and a red-shelled backrest with a dark cushion facing the driver.
    body.append((box((0, 0.17, -0.27), (0.40, 0.18, 0.40), SEAT, bottom=False), 0))
    hw, h, b = 0.23, 0.50, 0.07
    back = prism([(hw, 0.0), (hw, h - b), (hw - b, h), (-hw + b, h), (-hw, h - b), (-hw, 0.0)],
                 -0.04, 0.04, RED, closed_loop=False)
    back.repaint(lambda c, n, p: SEAT if n[2] > 0.9 else None)
    back.xform(rot_x(math.radians(-14)), (0, 0.19, -0.49))
    body.append((back, 0))

    # Engine block (gunmetal, yellow top) and chrome exhausts with dark open ends.
    eng = box((0, 0.27, -0.71), (0.40, 0.30, 0.27), GUNMETAL, bottom=False)
    eng.repaint(lambda c, n, p: YELLOW if n[1] > 0.9 else None)
    body.append((eng, 0))
    for tip in EXHAUST_TIPS:
        start = np.array([tip[0] * 0.75, 0.31, -0.76])
        pipe = tube([start, tip], [0.04, 0.052], 5, GUNMETAL, cap1=PIPE_HOLE, phase=math.pi / 2)
        pipe.paint_corners(lambda p, n: chrome(n))
        pipe.repaint(lambda c, n, p: PIPE_HOLE if n[2] < -0.8 else None)
        body.append((pipe, 0))

    # Rear axle (visible below the engine from the chase camera).
    body.append((tube([(-0.47, REAR_R, REAR_Z), (0.47, REAR_R, REAR_Z)], [0.03, 0.03], 4, DARK,
                      phase=math.pi / 4), 0))

    # Steering column (body part, ends under the wheel hub).
    col0 = STEER_HUB - 0.42 * STEER_AXIS
    body.append((tube([col0, STEER_HUB - 0.01 * STEER_AXIS], [0.022, 0.022], 4, DARK, phase=math.pi / 4), 0))

    parts[STEER].append((steering_wheel(6, 3), 0))

    for part, r, w, x in ((WFL, FRONT_R, FRONT_W, -1), (WFR, FRONT_R, FRONT_W, 1),
                          (WRL, REAR_R, REAR_W, -1), (WRR, REAR_R, REAR_W, 1)):
        g = tyre(r, w, 8)
        if x < 0:
            g = g.mirrored_x()
        parts[part].append((g.xform(None, PIVOTS[part]), 0))
    return parts


def steering_wheel(segs: int, sides: int) -> Geo:
    fr = frame_from(STEER_AXIS, up=(0, 1, 0))       # local Y = steer axis
    path = []
    for k in range(segs):
        a = 2 * math.pi * k / segs
        path.append(fr @ np.array([STEER_RIM_R * math.cos(a), 0.0, STEER_RIM_R * math.sin(a)]) + STEER_HUB)
    rim = tube(path, [0.026] * segs, sides, GRIP, closed_path=True, up=STEER_AXIS, phase=math.pi / sides)
    # Cross spoke (open ends are buried in the rim).
    sw, st = 0.05, 0.016
    spoke = prism(rect(sw / 2, st / 2), -STEER_RIM_R + 0.01, STEER_RIM_R - 0.01, YELLOW,
                  cap0=False, cap1=False)
    # prism x (width) -> in-plane up/down, y (thin) -> steer axis, z (length) -> across.
    spoke.xform(np.column_stack([fr[:, 2], fr[:, 1], fr[:, 0]]), STEER_HUB)
    return merge(rim, spoke)


def tyre_lod1(radius: float, width: float, segs: int, inner: bool) -> Geo:
    """Distant wheel: one sloped band of rubber (tread + shoulder) ending in the
    same yellow-to-pale domed hub as LOD0, plus an optional flat inner face (rear
    wheels need it; the front ones' inner sides are hidden by the nose and pods)."""
    w = 0.5 * width
    profile = [(-w, radius), (w, radius * 0.76), (w + 0.025, 0.0)]
    g = lathe(profile, segs, lambda k: HUB_CAP if k == 2 else YELLOW, 40.0,
              face_paint=lambda i, j: TYRE if i == 0 else None, phase=math.pi / segs)
    if inner:
        ring = [np.array([-w, radius * math.cos(a), radius * math.sin(a)])
                for a in (2 * math.pi * j / segs + math.pi / segs for j in range(segs))]
        cap = Geo()
        for j in range(1, segs - 1):
            cap.tri((ring[0], ring[j], ring[j + 1]), None, TYRE)
        _orient(cap, lambda c: (-1.0, 0.0, 0.0))
        g.extend(cap.flat())
    return g


def nose_section_lod1(z, hw, yb, yt):
    h = yt - yb
    return [(hw, yb, z), (hw, yb + 0.55 * h, z), (0.30 * hw, yt, z),
            (-0.30 * hw, yt, z), (-hw, yb + 0.55 * h, z), (-hw, yb, z)]


def pod_section_lod1(z, s=1.0, drop=0.0):
    cx, cy = 0.465, 0.17
    pts = [(0.625, 0.075), (0.66, 0.175), (0.605, 0.28), (0.30, 0.29), (0.30, 0.075)]
    return [(cx + (x - cx) * s, cy + (y - cy) * s - drop, z) for x, y in pts]


def build_kart_lod1() -> dict[int, list[tuple[Geo, int]]]:
    """Same silhouette and colour layout as LOD0 with fewer sides/sections."""
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    body = parts[BODY]

    # Back cap (the dash) omitted: it faces the driver and is hidden behind the Chao.
    nose = loft([nose_section_lod1(0.975, 0.20, 0.11, 0.24), nose_section_lod1(0.24, 0.29, 0.08, 0.475)],
                RED, 50.0, closed_loop=False, cap1=False,
                face_paint=lambda i, j: WHITE if j == 2 else None)
    body.append((nose, 0))

    body.append((tube([(-0.47, 0.15, 0.70), (0.0, 0.15, 1.02), (0.47, 0.15, 0.70)],
                      [0.032] * 3, 3, YELLOW, phase=math.pi / 2), 0))

    floor = Geo()
    fl = [(0.30, 0.10, 0.70), (0.30, 0.10, -0.80), (-0.30, 0.10, -0.80), (-0.30, 0.10, 0.70)]
    floor.tri((fl[0], fl[1], fl[2]), None, DARK)
    floor.tri((fl[0], fl[2], fl[3]), None, DARK)
    _orient(floor, lambda c: (0, 1, 0))
    body.append((floor.flat(), 0))

    pod = loft([pod_section_lod1(0.415, 0.62, 0.015), pod_section_lod1(-0.375, 0.92)], RED, 40.0,
               closed_loop=False, cap0=False, face_paint=lambda i, j: WHITE if j == 1 else None)
    body.append((pod, 0))
    body.append((pod.mirrored_x(), 0))

    hw, h = 0.23, 0.50
    back = prism([(hw, 0.0), (hw, h), (-hw, h), (-hw, 0.0)], -0.04, 0.04, RED, closed_loop=False)
    back.repaint(lambda c, n, p: SEAT if n[2] > 0.9 else None)
    back.xform(rot_x(math.radians(-14)), (0, 0.19, -0.49))
    body.append((back, 0))

    eng = box((0, 0.27, -0.71), (0.40, 0.30, 0.27), GUNMETAL, bottom=False, cap1=False)  # front hidden
    eng.repaint(lambda c, n, p: YELLOW if n[1] > 0.9 else None)
    body.append((eng, 0))
    for tip in EXHAUST_TIPS:
        start = np.array([tip[0] * 0.75, 0.31, -0.76])
        pipe = tube([start, tip], [0.045, 0.055], 3, GUNMETAL, cap1=PIPE_HOLE, phase=math.pi / 2)
        pipe.paint_corners(lambda p, n: chrome(n))
        pipe.repaint(lambda c, n, p: PIPE_HOLE if n[2] < -0.8 else None)
        body.append((pipe, 0))
    body.append((tube([(-0.47, REAR_R, REAR_Z), (0.47, REAR_R, REAR_Z)], [0.035, 0.035], 3, DARK,
                      phase=math.pi / 2), 0))

    for part, r, w, x in ((WFL, FRONT_R, FRONT_W, -1), (WFR, FRONT_R, FRONT_W, 1),
                          (WRL, REAR_R, REAR_W, -1), (WRR, REAR_R, REAR_W, 1)):
        g = tyre_lod1(r, w, 8, inner=part in (WRL, WRR))
        if x < 0:
            g = g.mirrored_x()
        parts[part].append((g.xform(None, PIVOTS[part]), 0))
    return parts


# --------------------------------------------------------------------------
# Chao
# --------------------------------------------------------------------------


def hand_positions() -> list[np.ndarray]:
    """Rim points at "ten to two", nudged towards the driver."""
    plane_up = unit(np.cross(STEER_AXIS, (1.0, 0.0, 0.0)))
    if plane_up[1] < 0:
        plane_up = -plane_up
    a = math.radians(62.0)
    return [STEER_HUB + STEER_RIM_R * (math.sin(a) * np.array([s, 0.0, 0.0]) + math.cos(a) * plane_up)
            + 0.012 * STEER_AXIS for s in (-1.0, 1.0)]


PIVOTS[HAND_L], PIVOTS[HAND_R] = hand_positions()


def eye_frames():
    """(centre, frame, radii) for each eye; frame local Y = outward, local Z = down."""
    out = []
    for s in (-1.0, 1.0):
        p, n = surface_point(HEAD_C, HEAD_R, (s * 0.40, 0.03, 1.0))
        fr = frame_from(n, up=(0, 1, 0)) @ rot_y(s * math.radians(-12))
        out.append((s, p - 0.016 * n, fr, np.array([0.074, 0.036, 0.102])))
    return out


def head_point_path():
    """Centre line and radii of the teardrop point that curls back off the crown."""
    top = HEAD_C[1] + HEAD_R[1]
    path = [(0, top - 0.14, -0.31), (0, top + 0.02, -0.35), (0, top + 0.115, -0.43),
            (0, top + 0.145, -0.545), (0, top + 0.09, -0.655)]
    return path, [0.145, 0.125, 0.085, 0.045, 0.0]


def wing_frame(s: float):
    """(long axis, root, frame) of the wing on side s; frame local Y runs along the wing."""
    d = unit((s * 0.90, 0.40, -0.35))      # out, a little up, back
    root = np.array([s * 0.09, 0.63, -0.40])
    return d, root, frame_from(d, up=(0, 0, -1))   # flat side faces back


def build_chao_lod0() -> dict[int, list[tuple[Geo, int]]]:
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)

    # ---- Head: big squashed sphere, glossy eyes, smile, curling head point.
    # Poles on +/-Z: the silhouette seen from behind (chase camera) or in front is
    # the full 14-segment ring; 30-degree bands give a 12-gon from the side/top.
    to_z = np.column_stack([(1.0, 0, 0), (0, 0, 1.0), (0, -1.0, 0)])   # local Y -> +Z, local Z -> -Y
    head = ellipsoid(HEAD_C, (HEAD_R[0], HEAD_R[2], HEAD_R[1]), 14, [0, 30, 60, 90, 120, 150, 180],
                     chao(0.0), frame=to_z, phase=-math.pi / 2)
    parts[CHEAD].append((head, 0))

    shine = Geo()
    for s, eye_c, fr, eye_r in eye_frames():
        parts[CHEAD].append((ellipsoid(eye_c, eye_r, 8, [0, 55, 100], EYE, frame=fr), 0))
        # Highlights: a big unlit oval upper-outer, a tiny triangle lower-inner.
        for off, rr, k in (((s * 0.32, 0.45), (0.021, 0.029), 5), ((s * -0.30, -0.48), (0.012, 0.012), 3)):
            d = unit((off[0], 0.86, -off[1]))
            t = 1.0 / math.sqrt(float(np.sum((d / eye_r) ** 2)))
            q = eye_c + fr @ (d * t)
            qn = unit(fr @ (d * t / eye_r ** 2))
            qf = frame_from(qn, up=(0, 1, 0))
            pts = [q + 0.004 * qn + qf @ np.array([rr[0] * math.cos(a), 0.0, rr[1] * math.sin(a)])
                   for a in (2 * math.pi * i / k + math.pi / 2 for i in range(k))]
            for i in range(1, k - 1):
                shine.tri((pts[0], pts[i + 1], pts[i]), [qn] * 3, EYE_SHINE)
    _orient(shine, lambda c: c - HEAD_C)
    parts[CHEAD].append((shine, 1))

    # Mouth: small dark crescent smile.
    mc, mn = surface_point(HEAD_C, HEAD_R, (0.0, -0.36, 1.0))
    mf = frame_from(mn)
    mright, mup = mf[:, 0], np.cross(mn, mf[:, 0])
    if mup[1] < 0:
        mup = -mup

    def on_head(x, y):
        q, qn = surface_point(HEAD_C, HEAD_R, mc + mright * x + mup * y - HEAD_C)
        return q + 0.004 * qn, qn

    U, L = [], []
    for k in range(5):
        u = -1.0 + 2.0 * k / 4
        U.append(on_head(0.068 * u, -0.018 * (1 - u * u) + 0.014 * u * u))
        L.append(on_head(0.068 * u, -0.056 * (1 - u * u) + 0.014 * u * u))
    mouth = Geo()
    for k in range(4):
        quad = (U[k], U[k + 1], L[k + 1], L[k])
        for a, b, c in ((0, 1, 2), (0, 2, 3)):
            mouth.tri([quad[a][0], quad[b][0], quad[c][0]], [quad[a][1], quad[b][1], quad[c][1]], MOUTH)
    _orient(mouth, lambda c: c - HEAD_C)
    parts[CHEAD].append((mouth, 0))

    # Head point: a teardrop rising from the crown and curling back.
    pt_path, pt_r = head_point_path()
    point = tube(pt_path, pt_r, 6, lambda s: chao(smoothstep(0.2, 0.9, s)), up=(1, 0, 0), phase=math.pi / 6)
    parts[CHEAD].append((point, 0))

    # ---- Body: small round torso, stubby arms and feet, tiny wings.
    parts[CBODY].append((ellipsoid(TORSO_C, TORSO_R, 6, [0, 60, 120, 180], chao(0.0), phase=math.pi / 2), 0))

    for s, hp, shoulder, arm_part, hand_part in zip((-1.0, 1.0), hand_positions(), SHOULDERS,
                                                    (ARM_L, ARM_R), (HAND_L, HAND_R)):
        wrist = hp - 0.045 * unit(hp - shoulder)
        mid = 0.5 * (shoulder + wrist) + np.array([s * 0.035, -0.01, 0.0])
        arm = tube([shoulder, mid, wrist], [0.064, 0.056, 0.048], 5,
                   lambda t: chao(0.75 * smoothstep(0.35, 1.0, t)), up=(0, 1, 0))
        parts[arm_part].append((arm, 0))
        parts[hand_part].append((ellipsoid(hp, (0.066, 0.062, 0.066), 5, [0, 90, 180], chao(0.95),
                                           phase=math.pi / 2), 0))

        foot_c = np.array([s * 0.11, 0.30, -0.03])
        foot = ellipsoid(foot_c, (0.075, 0.065, 0.12), 5, [0, 90, 180],
                         lambda u: chao(smoothstep(-0.2, 0.9, float(u[2]))), frame=rot_y(s * 0.25),
                         phase=math.pi / 2)
        parts[CBODY].append((foot, 0))

        d, root, fr = wing_frame(s)
        wing = ellipsoid(root + d * 0.11, (0.095, 0.125, 0.03), 5, [0, 70, 125, 180],
                         lambda u: chao(smoothstep(0.0, 1.0, float(u[1]))), frame=fr, phase=math.pi / 2)
        parts[CBODY].append((wing, 0))
    return parts


def build_chao_lod1() -> dict[int, list[tuple[Geo, int]]]:
    """Same character as LOD0: round head with its curling point, oval eyes,
    arms reaching forward to where the (LOD0-only) wheel and hands are, wings."""
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    to_z = np.column_stack([(1.0, 0, 0), (0, 0, 1.0), (0, -1.0, 0)])
    parts[CHEAD].append((ellipsoid(HEAD_C, (HEAD_R[0], HEAD_R[2], HEAD_R[1]), 10, [0, 45, 90, 135, 180],
                                   chao(0.0), frame=to_z, phase=-math.pi / 2), 0))
    pt_path, pt_r = head_point_path()
    keep = [0, 2, 4]
    parts[CHEAD].append((tube([pt_path[i] for i in keep], [pt_r[i] for i in keep], 4,
                              lambda s: chao(smoothstep(0.2, 0.9, s)), up=(1, 0, 0), phase=math.pi / 4), 0))
    for s, eye_c, fr, eye_r in eye_frames():
        parts[CHEAD].append((ellipsoid(eye_c, eye_r, 4, [0, 95], EYE, frame=fr), 0))

    parts[CBODY].append((ellipsoid(TORSO_C, TORSO_R, 4, [0, 90, 180], chao(0.0), phase=math.pi / 4), 0))
    for s, hp, shoulder, arm_part in zip((-1.0, 1.0), hand_positions(), SHOULDERS, (ARM_L, ARM_R)):
        # Arm reaching to where LOD0's hand grips the wheel, ending in a blunt yellow fist.
        d = unit(hp - shoulder)
        arm = tube([shoulder, hp, hp + 0.05 * d], [0.062, 0.07, 0.0], 3,
                   lambda t: chao(0.95 * smoothstep(0.35, 0.85, t)), up=(0, 1, 0))
        parts[arm_part].append((arm, 0))
        wd, root, fr = wing_frame(s)
        parts[CBODY].append((ellipsoid(root + wd * 0.11, (0.095, 0.125, 0.03), 3, [0, 90, 180],
                                       lambda u: chao(smoothstep(0.0, 1.0, float(u[1]))), frame=fr,
                                       phase=math.pi / 2), 0))
    return parts


# --------------------------------------------------------------------------
# Emotes
# --------------------------------------------------------------------------


def heart_outline(k: int, size: float) -> list[np.ndarray]:
    pts = []
    for i in range(k):
        t = 2 * math.pi * i / k
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append(np.array([x, y + 2.5, 0.0]) * (size / 32.0))
    return pts


def puffy_heart(k: int, size: float, depth: float) -> Geo:
    """Pillow heart facing +/-Z (readable from behind and in front)."""
    outline = heart_outline(k, size)
    g = Geo()
    for sign in (1.0, -1.0):
        c = np.array([0.0, -0.05 * size, sign * depth])
        for i in range(k):
            a, b = outline[i], outline[(i + 1) % k]
            # outline runs clockwise seen from +Z (x = sin^3 t), so (c, b, a) faces +Z.
            g.tri((c, b, a) if sign > 0 else (c, a, b), None, EMOTE_HEART)
    _orient(g, lambda cen: np.array([0.0, 0.0, cen[2]]))
    return g.autosmooth(80.0)


def build_emotes(lod: int) -> dict[int, list[Geo]]:
    """Returns {variant: [geo, ...]} in emote-pivot space."""
    out: dict[int, list[Geo]] = {}
    if lod == 0:
        out[1] = [geosphere((0, 0, 0), (0.09, 0.09, 0.09), 2, EMOTE_BALL)]
        bar = tube([(0, 0.17, 0), (0, 0.0, 0)], [0.052, 0.03], 6, EMOTE_BANG, cap0=EMOTE_BANG, cap1=EMOTE_BANG)
        bar.autosmooth(50.0)
        dot = ellipsoid((0, -0.085, 0), (0.04, 0.04, 0.04), 5, [0, 90, 180], EMOTE_BANG)
        out[2] = [bar, dot]
        out[3] = [puffy_heart(12, 0.21, 0.05)]
    else:
        out[1] = [ellipsoid((0, 0, 0), (0.09, 0.09, 0.09), 6, [0, 90, 180], EMOTE_BALL)]
        bar = tube([(0, 0.17, 0), (0, 0.0, 0)], [0.055, 0.03], 3, EMOTE_BANG, cap0=EMOTE_BANG,
                   phase=math.pi / 2).autosmooth(50.0)
        dot = tube([(0, -0.12, 0), (0, -0.05, 0)], [0.05, 0.0], 3, EMOTE_BANG, cap0=EMOTE_BANG,
                   phase=math.pi / 2)
        out[2] = [bar, dot]
        out[3] = [puffy_heart(8, 0.21, 0.05)]
    return out


# --------------------------------------------------------------------------
# Far LOD (lod 3): the cheapest model, used beyond ~40 m (1P) / ~25 m (split screen)
# --------------------------------------------------------------------------


def tyre_lod3(radius: float, width: float, segs: int) -> Geo:
    """Closed drum with a tapered outer sidewall framing a yellow hub. The tread
    keeps the tube's smooth normals (one shared vertex per corner) so the far
    wheel costs few vertices; the flat end caps are separate polygons."""
    w = 0.5 * width
    tread = tube([(-w, 0, 0), (w, 0, 0)], [radius, radius * 0.74], segs, TYRE, up=(0, 1, 0),
                 phase=math.pi / segs)
    caps = tube([(-w, 0, 0), (w, 0, 0)], [radius, radius * 0.74], segs, TYRE, cap0=TYRE, cap1=YELLOW,
                up=(0, 1, 0), phase=math.pi / segs)
    assert len(caps) == 2 * segs + 2 * (segs - 2)   # side quads first, then the two cap fans
    caps.P, caps.N, caps.C = caps.P[2 * segs:], caps.N[2 * segs:], caps.C[2 * segs:]   # drop its side quads
    return tread.extend(caps)


def build_kart_lod3() -> dict[int, list[tuple[Geo, int]]]:
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    body = parts[BODY]

    def sec(z, hw, yb, yt):
        return [(hw, yb, z), (0.0, yt, z), (-hw, yb, z)]

    nose = loft([sec(0.975, 0.17, 0.10, 0.25), sec(0.24, 0.29, 0.08, 0.49)], RED, 50.0, closed_loop=False,
                cap1=False)
    body.append((nose, 0))
    # Both side pods + floor as one slab, white sides like LOD0's pod stripes.
    pods = box((0, 0.18, 0.02), (1.30, 0.22, 0.78), RED, bottom=False)
    pods.repaint(lambda c, n, p: WHITE if n[1] < 0.9 and abs(n[0]) > 0.9 else None)
    body.append((pods, 0))
    # Backrest + engine as one block: red shell, yellow top, and a back face that
    # shades from the red backrest down to the gunmetal engine (free: same vertices).
    blk = box((0, 0.36, -0.66), (0.44, 0.50, 0.38), RED, bottom=False, cap1=False)  # front hidden by Chao
    blk.repaint(lambda c, n, p: YELLOW if n[1] > 0.9 else None)
    blk.repaint_corners(lambda p, n, old: (RED if p[1] > 0.4 else GUNMETAL) if n[2] < -0.9 else old)
    body.append((blk, 0))
    for part, r, w, x in ((WFL, FRONT_R, FRONT_W, -1), (WFR, FRONT_R, FRONT_W, 1),
                          (WRL, REAR_R, REAR_W, -1), (WRR, REAR_R, REAR_W, 1)):
        g = tyre_lod3(r, w, 6 if part in (WRL, WRR) else 5)
        if x < 0:
            g = g.mirrored_x()
        parts[part].append((g.xform(None, PIVOTS[part]), 0))
    return parts


def build_chao_lod3() -> dict[int, list[tuple[Geo, int]]]:
    parts: dict[int, list[tuple[Geo, int]]] = defaultdict(list)
    parts[CHEAD].append((geosphere(HEAD_C, HEAD_R, 2, chao(0.0)), 0))
    top = HEAD_C[1] + HEAD_R[1]
    pt = tube([(0, top - 0.08, -0.31), (0, top + 0.15, -0.53)], [0.12, 0.0], 3,
              lambda s: chao(s), phase=math.pi / 2, up=(1, 0, 0))
    parts[CHEAD].append((pt, 0))
    eyes = Geo()
    for s, eye_c, fr, eye_r in eye_frames():
        n = fr[:, 1]
        c = eye_c + 0.02 * n
        q = [c + fr @ np.array(v) for v in ((0.075, 0, 0), (0, 0, -0.10), (-0.075, 0, 0), (0, 0, 0.10))]
        eyes.tri((q[0], q[1], q[2]), [n] * 3, EYE)
        eyes.tri((q[0], q[2], q[3]), [n] * 3, EYE)
    _orient(eyes, lambda c: c - HEAD_C)
    parts[CHEAD].append((eyes, 0))
    parts[CBODY].append((ellipsoid(TORSO_C, TORSO_R, 3, [0, 90, 180], chao(0.0), phase=math.pi / 2), 0))
    # No arms while driving (the head hides them from behind); a raised arm
    # (wave, cheer) shows as a short three-sided stub with a yellow fist end.
    for hp, shoulder, arm_part in zip(hand_positions(), SHOULDERS, (ARM_L, ARM_R)):
        arm = tube([shoulder, hp], [0.06, 0.068], 3, lambda t: chao(0.6 * t), cap1=chao(0.95),
                   up=(0, 1, 0), phase=math.pi / 2)
        parts[arm_part].append((arm, FLAG_ARM_RAISED))
    return parts


def build_emotes_lod3() -> dict[int, list[Geo]]:
    out: dict[int, list[Geo]] = {}
    out[1] = [ellipsoid((0, 0, 0), (0.09, 0.09, 0.09), 4, [0, 90, 180], EMOTE_BALL)]
    bar = tube([(0, 0.17, 0), (0, 0.0, 0)], [0.05, 0.0], 3, EMOTE_BANG, cap0=EMOTE_BANG)
    dot = tube([(0, -0.12, 0), (0, -0.05, 0)], [0.05, 0.0], 3, EMOTE_BANG, cap0=EMOTE_BANG)
    out[2] = [bar, dot]
    g = Geo()
    o = heart_outline(12, 0.21)
    pts = [o[6], o[8], o[10], o[0], o[2], o[4]]   # tip, left side, left lobe, cusp, right lobe, right side
    for sign in (1.0, -1.0):
        z = np.array([0, 0, sign * 0.025])
        for i in range(1, 5):
            g.tri((pts[0] + z, pts[i] + z, pts[i + 1] + z), None, EMOTE_HEART)
    _orient(g, lambda cen: np.array([0, 0, cen[2]]))
    out[3] = [g.flat()]
    return out


# --------------------------------------------------------------------------
# Assembly, welding, checks, output
# --------------------------------------------------------------------------


PAINT_SLOTS = 5   # fixed, kart primary, kart accent, Chao body, Chao tip


@dataclass
class Mesh(meshasm.Mesh):
    part: int
    lod: int
    variant: int
    flags: int


def build_mesh(part: int, lod: int, variant: int, flags: int, geos: list[Geo]) -> Mesh:
    name = f"{PART_NAMES[part]} lod{lod} var{variant}"
    verts, index = weld(geos, name, pivot=PIVOTS[part])
    return Mesh(name, verts, index, part, lod, variant, flags)


def assemble() -> list[Mesh]:
    groups: dict[tuple[int, int, int, int], list[Geo]] = defaultdict(list)

    for lod, kart, chao_, emotes in ((0, build_kart_lod0(), build_chao_lod0(), build_emotes(0)),
                                     (1, build_kart_lod1(), build_chao_lod1(), build_emotes(1)),
                                     (3, build_kart_lod3(), build_chao_lod3(), build_emotes_lod3())):
        for src in (kart, chao_):
            for part, items in src.items():
                for g, flags in items:
                    groups[(part, lod, 0, flags)].append(g)
        for variant, geos in emotes.items():
            for g in geos:
                groups[(EMOTE, lod, variant, 0)].append(g.copy().xform(None, EMOTE_C))

    meshes = []
    for (part, lod, variant, flags) in sorted(groups, key=lambda k: (k[1], k[0], k[2], k[3])):
        m = build_mesh(part, lod, variant, flags, groups[(part, lod, variant, flags)])
        check_mesh(m, PAINT_SLOTS)
        meshes.append(m)
    return meshes


def tri_count(meshes, lod, parts) -> int:
    """Triangles drawn for the given parts at a LOD while driving, taking the
    largest emote variant (raised-arm-only meshes are reported separately)."""
    fixed = sum(len(m.indices) // 3 for m in meshes if m.lod == lod and m.part in parts and m.variant == 0
                and not m.flags & FLAG_ARM_RAISED)
    var = defaultdict(int)
    for m in meshes:
        if m.lod == lod and m.part in parts and m.variant != 0:
            var[m.variant] += len(m.indices) // 3
    return fixed + (max(var.values()) if var else 0)


KART_PARTS = {BODY, WFL, WFR, WRL, WRR, STEER}
CHAO_PARTS = {CBODY, CHEAD, EMOTE, ARM_L, ARM_R, HAND_L, HAND_R}


def report(meshes: list[Mesh]) -> list[str]:
    lines = []
    for lod in (0, 1, 3):
        for part in range(len(PART_NAMES)):
            for m in meshes:
                if m.lod == lod and m.part == part:
                    lines.append(f"  lod{lod} {PART_NAMES[part]:<20} var{m.variant} flags{m.flags}: "
                                 f"{len(m.indices) // 3:4d} tris {len(m.verts):4d} verts")
    k0 = tri_count(meshes, 0, KART_PARTS)
    c0 = tri_count(meshes, 0, CHAO_PARTS)
    l1 = tri_count(meshes, 1, KART_PARTS | CHAO_PARTS)
    l3 = tri_count(meshes, 3, KART_PARTS | CHAO_PARTS)
    l3_arms = sum(len(m.indices) // 3 for m in meshes if m.lod == 3 and m.flags & FLAG_ARM_RAISED)
    nv = sum(len(m.verts) for m in meshes)
    ni = sum(len(m.indices) for m in meshes)
    lines.append(f"  LOD0 kart {k0} tris (budget {BUDGET_KART_LOD0}), Chao incl. hands + largest emote "
                 f"{c0} tris (budget {BUDGET_CHAO_LOD0})")
    lines.append(f"  LOD1 kart+Chao incl. largest emote {l1} tris (budget {BUDGET_LOD1})")
    lines.append(f"  LOD3 kart+Chao incl. largest emote {l3} tris (budget {BUDGET_LOD3}), "
                 f"+{l3_arms} with both arms raised")
    lines.append(f"  total {nv} verts (budget {BUDGET_VERTS}), {ni} indices, {len(meshes)} meshes")
    print("\n".join(lines))
    errors = []
    if k0 > BUDGET_KART_LOD0:
        errors.append("kart LOD0 over budget")
    if c0 > BUDGET_CHAO_LOD0:
        errors.append("Chao LOD0 over budget")
    if l1 > BUDGET_LOD1:
        errors.append("LOD1 over budget")
    if l3 > BUDGET_LOD3:
        errors.append("LOD3 over budget")
    if nv > BUDGET_VERTS:
        errors.append("vertex budget exceeded")
    return errors


def write_header(meshes: list[Mesh], path: Path) -> None:
    verts, indices, descs = pack(meshes)
    assert len(verts) < 65536 and len(indices) < 65536

    out = [
        "typedef struct { float x, y, z; int8_t nx, ny, nz; uint8_t paint; uint32_t argb; uint8_t tip; "
        "uint8_t pad[3]; } dk_mvert_t; /* normal*127; tip = 0..255 blend toward tip colour for paint 3/4 */",
        "typedef struct { uint16_t first_vert, vert_count, first_index, index_count; uint8_t part, lod, "
        "variant, flags; } dk_mesh_t; /* flags bit0 = unlit (emissive/eye highlights), bit1 = drawn only while that arm is raised */",
        "enum { " + ", ".join(PART_NAMES) + ", DK_PART_COUNT };",
        "/* Part pivots in kart space (m): world = kart * translate(pivot) * part_rotation * vertex. */",
        "static const float dk_part_pivot[DK_PART_COUNT][3] = {",
    ]
    for i, name in enumerate(PART_NAMES):
        p = PIVOTS[i]
        out.append(f"    {{ {cf(p[0])}, {cf(p[1])}, {cf(p[2])} }}, /* {name} */")
    out.append("};")
    a = STEER_AXIS
    out.append("/* Steering column axis (unit), pointing from the wheel hub up and back towards the driver. */")
    out.append(f"static const float dk_steer_axis[3] = {{ {cf(a[0])}, {cf(a[1])}, {cf(a[2])} }};")
    out.append("static const float dk_exhaust_pos[2][3] = {  /* exhaust pipe tips, kart space (left, right) */")
    for t in EXHAUST_TIPS:
        out.append(f"    {{ {cf(t[0])}, {cf(t[1])}, {cf(t[2])} }},")
    out.append("};")
    out.append(f"static const float dk_wheel_radius[4] = {{ {cf(FRONT_R)}, {cf(FRONT_R)}, {cf(REAR_R)}, "
               f"{cf(REAR_R)} }};    /* FL, FR, RL, RR */")
    out.append(f"#define DK_MODEL_VERT_COUNT {len(verts)}")
    out.append(f"#define DK_MODEL_INDEX_COUNT {len(indices)}")
    out.append(f"#define DK_MODEL_MESH_COUNT {len(descs)}")
    out.append("static const dk_mvert_t dk_model_verts[DK_MODEL_VERT_COUNT] = {")
    out += vert_rows(verts, tip=True)
    out.append("};")
    out.append("static const uint16_t dk_model_indices[DK_MODEL_INDEX_COUNT] = {  "
               "/* indices are relative to the mesh's first_vert */")
    out += index_rows(indices, 18)
    out.append("};")
    out.append("static const dk_mesh_t dk_model_meshes[DK_MODEL_MESH_COUNT] = {")
    out += mesh_rows(descs, lambda m: f"{PART_NAMES[m.part]}, {m.lod}, {m.variant}, {m.flags}")
    out.append("};")
    meshasm.write_header(path, "tools/build_models.py", "DK_MODELS_H", out)


def write_preview_json(meshes: list[Mesh], path: Path) -> None:
    write_json(path, {
        "parts": PART_NAMES,
        "pivots": {i: [float(x) for x in PIVOTS[i]] for i in range(len(PART_NAMES))},
        "exhaust": [[float(x) for x in t] for t in EXHAUST_TIPS],
        "meshes": [{"part": m.part, "lod": m.lod, "variant": m.variant, "flags": m.flags, **mesh_json(m)}
                   for m in meshes],
    })


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=HEADER)
    ap.add_argument("--preview-json", type=Path, help="also dump meshes for preview_models_blender.py")
    args = ap.parse_args()
    meshes = assemble()
    errors = report(meshes)
    if args.preview_json:
        write_preview_json(meshes, args.preview_json)
        print(f"wrote {args.preview_json}")
    if errors:
        raise SystemExit("error: " + "; ".join(errors) + f" (not writing {args.out})")
    write_header(meshes, args.out)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
