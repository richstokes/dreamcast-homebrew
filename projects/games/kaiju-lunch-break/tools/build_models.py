#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26"]
# ///
"""Build Kaiju Lunch Break's models: the kaiju's rigid segments, the
vehicles and the rooftop props, from the engine's meshkit primitives.

The kaiju is a chain of rigid parts (torso, head, jaw, arms, legs, tail)
that src/kaiju.c poses every frame, so each part is modelled about its
joint. Every part also gets an ink shell: the same surface pushed out along
its smoothed normals and turned inside out, which drawn black behind the
part leaves the comic outline. Writes assets/generated/models.h.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
from meshasm import Mesh, check_mesh, index_rows, ink_shell, pack, vert_rows, weld, write_header  # noqa: E402
from meshkit import Geo, Paint, box, ellipsoid, geosphere, prism, rgb, rot_x, rot_y, rot_z, tube  # noqa: E402

GEN = Path(__file__).resolve().parent.parent / "assets" / "generated"

# Paint slots: 0 fixed colour, 1 the instance's own paint (cars, debris),
# 2 the kaiju's back plates (they glow as the breath charges).
BODY = Paint(rgb(70, 198, 150))
BELLY = Paint(rgb(252, 238, 184))
PLATE = Paint(rgb(255, 150, 60), 2)
BONE = Paint(rgb(255, 250, 240))
MOUTH = Paint(rgb(236, 96, 124))
DARK = Paint(rgb(30, 104, 96))
INK = Paint(rgb(22, 18, 34))
WHITE = Paint(rgb(255, 255, 255))
GLASS = Paint(rgb(150, 214, 240))
OWN = Paint(rgb(255, 255, 255), 1)
OLIVE = Paint(rgb(112, 138, 84))
OLIVE_D = Paint(rgb(78, 100, 66))
STEEL = Paint(rgb(190, 196, 210))

# The kaiju's joints, in metres (src/kaiju.c reads these from the header).
THIGH_LEN, SHIN_LEN = 8.5, 7.5
ANKLE_H = 1.9
HIP_Y, HIP_X = 17.0, 6.2
NECK = (0.0, 17.0, 4.6)          # on the torso
JAW = (0.0, 0.5, 3.4)            # on the head
MOUTH_POS = (0.0, 0.8, 11.0)     # on the head: where the breath leaves
SHOULDER = (8.2, 11.0, 3.6)      # on the torso (x mirrored for the left)
UPPER_ARM_LEN, FOREARM_LEN = 5.5, 5.0
TAIL_ROOT = (0.0, -1.5, -6.5)    # on the torso
TAIL_LEN = (7.0, 6.6, 6.2, 5.8, 5.6)
TAIL_R = (6.2, 5.0, 3.9, 2.9, 1.9, 0.5)


def cone(a, b, r, paint, sides=4):
    return tube([a, b], [r, 0.0], sides, paint, cap0=paint)


def plate(base, size, lean):
    """A back spike: a four-sided pyramid on base, leaning back, so it
    reads as a triangle from behind as well as from the side."""
    s = size
    tip = np.array(base) + np.array([0.0, math.cos(lean), math.sin(lean)]) * 1.5 * s
    return tube([base, tip], [0.62 * s, 0.0], 4, PLATE, cap0=PLATE, phase=math.pi / 4)


def belly_paint(u):
    return BELLY if u[2] > 0.42 and u[1] < 0.62 else BODY


def torso():
    lean = rot_x(0.14)
    g = ellipsoid((0, 5.6, 0.6), (8.6, 10.4, 8.2), 12, (0, 26, 52, 78, 104, 130, 156, 180), belly_paint, frame=lean)
    g.extend(ellipsoid((0, -0.8, -1.2), (8.0, 6.0, 8.6), 10, (0, 40, 80, 120, 180), lambda u: BELLY if u[2] > 0.6 and u[1] < 0.2 else BODY))
    g.extend(ellipsoid((0, 11.8, 2.4), (7.2, 5.4, 6.2), 10, (0, 40, 80, 120, 180), lambda u: BELLY if u[2] > 0.5 and u[1] < 0.3 else BODY))
    g.extend(tube([(0, 13.0, 3.2), (0, 17.4, 4.8)], [4.6, 4.0], 9, BODY))
    g.extend(plate((0, 15.2, -1.6), 3.6, -0.25))
    g.extend(plate((0, 11.0, -5.2), 5.0, -0.5))
    g.extend(plate((0, 5.2, -7.6), 5.4, -0.75))
    g.extend(plate((0, 0.0, -8.8), 4.4, -1.0))
    return g


def head():
    g = ellipsoid((0, 3.0, 2.4), (6.3, 5.6, 6.8), 12, (0, 30, 60, 90, 120, 150, 180), BODY)
    g.extend(ellipsoid((0, 2.0, 8.0), (4.8, 3.2, 5.4), 10, (0, 36, 72, 108, 144, 180), lambda u: MOUTH if u[1] < -0.8 else BODY))
    for sx in (1, -1):
        g.extend(geosphere((sx * 3.5, 5.7, 6.4), (1.9, 2.1, 1.7), 2, WHITE))
        g.extend(geosphere((sx * 3.7, 5.5, 7.9), (0.8, 1.0, 0.5), 1, INK))
        g.extend(box((sx * 3.5, 8.0, 6.8), (4.2, 1.0, 2.0), DARK, frame=rot_z(-sx * 0.42) @ rot_x(0.2)))
        g.extend(cone((sx * 3.8, 7.2, -0.6), (sx * 5.6, 11.4, -3.0), 1.4, PLATE, 5))
        g.extend(geosphere((sx * 1.6, 3.9, 12.7), (0.5, 0.4, 0.4), 1, DARK))
        for z, x in ((7.0, 3.6), (9.4, 3.5), (11.6, 2.5)):
            g.extend(cone((sx * x, -0.2, z), (sx * x, -2.3, z + 0.2), 0.75, BONE))
    g.extend(cone((0.9, -0.2, 12.8), (0.9, -2.0, 13.0), 0.7, BONE))
    g.extend(cone((-0.9, -0.2, 12.8), (-0.9, -2.0, 13.0), 0.7, BONE))
    g.extend(plate((0, 8.2, 1.0), 2.2, -0.2))
    return g


def jaw():
    g = ellipsoid((0, -1.4, 4.4), (4.4, 2.0, 5.8), 10, (0, 36, 72, 108, 144, 180), lambda u: MOUTH if u[1] > 0.55 else BELLY if u[1] < -0.2 else BODY)
    for sx in (1, -1):
        for z, x in ((6.2, 3.2), (8.4, 2.6)):
            g.extend(cone((sx * x, -0.6, z), (sx * x, 1.5, z), 0.7, BONE))
    return g


def upper_arm():
    g = ellipsoid((0, 0, 0), (2.9, 2.9, 2.9), 8, (0, 45, 90, 135, 180), BODY)
    g.extend(tube([(0, 0.0, 0), (0, -2.8, 0.2), (0, -UPPER_ARM_LEN, 0)], [2.4, 2.3, 1.9], 7, BODY, cap1=BODY))
    return g


def forearm():
    g = tube([(0, 0.4, 0), (0, -2.4, 0.3), (0, -FOREARM_LEN, 0.6)], [2.0, 1.9, 1.7], 7, BODY, cap0=BODY, cap1=BODY)
    for dx in (-1.1, 0.0, 1.1):
        g.extend(cone((dx, -FOREARM_LEN + 0.3, 0.9), (dx * 1.5, -FOREARM_LEN - 3.0, 2.6), 0.75, BONE))
    return g


def thigh():
    g = ellipsoid((0, 0, 0), (5.6, 5.4, 5.8), 9, (0, 40, 80, 120, 180), BODY)
    g.extend(tube([(0, 0.0, 0), (0, -4.0, 0.6), (0, -THIGH_LEN, 0)], [5.2, 4.8, 3.7], 9, BODY, cap1=BODY))
    return g


def shin():
    g = ellipsoid((0, 0, 0), (3.9, 3.9, 3.9), 8, (0, 45, 90, 135, 180), BODY)
    g.extend(tube([(0, 0.0, 0), (0, -3.6, -0.5), (0, -SHIN_LEN, 0)], [3.7, 3.2, 2.6], 8, BODY, cap1=BODY))
    return g


def foot():
    g = ellipsoid((0, -0.2, 2.2), (3.6, 1.7, 5.2), 10, (0, 40, 80, 120, 180), lambda u: BELLY if u[1] < -0.75 else BODY)
    g.extend(ellipsoid((0, 0.0, 0.0), (2.9, 2.2, 2.9), 8, (0, 45, 90, 135, 180), BODY))
    for dx in (-2.0, 0.0, 2.0):
        g.extend(cone((dx, -0.9, 6.4), (dx * 1.25, -1.6, 9.4), 0.95, BONE, 5))
    return g


def tail(i):
    L, r0, r1 = TAIL_LEN[i], TAIL_R[i], TAIL_R[i + 1]
    g = tube([(0, 0, 0.8), (0, 0, -L * 0.5), (0, 0, -L)], [r0 * 1.03, (r0 + r1) * 0.5, r1], 9, BODY, cap0=BODY, cap1=BODY)
    g.repaint_corners(lambda p, n, old: BELLY if n[1] < -0.72 else old)
    if i < 4:
        g.extend(plate((0, (r0 + r1) * 0.5 - 0.3, -L * 0.3), 3.4 - i * 0.6, -0.9).xform(rot_y(math.pi)))
    return g


# ---------------------------------------------------------------------------
# Vehicles and props: flat shaded, drawn 1.5 times life size so they read
# from the kaiju's shoulder.

def car():
    g = box((0, 1.25, 0), (3.0, 1.2, 6.8), OWN)
    g.extend(box((0, 2.3, -0.3), (2.6, 1.0, 3.4), GLASS))
    g.extend(box((0, 0.5, 0), (3.1, 0.6, 5.6), INK))
    return g


def bus():
    g = box((0, 2.9, 0), (4.0, 4.0, 16.0), OWN, bevel=0.4)
    g.extend(box((0, 3.6, 0), (4.1, 1.3, 14.4), GLASS))
    g.extend(box((0, 0.6, 0), (4.1, 0.8, 13.0), INK))
    return g


def tank():
    g = box((0, 1.9, 0), (5.2, 1.6, 10.0), OLIVE, bevel=0.4)
    for sx in (1, -1):
        g.extend(box((sx * 2.6, 1.0, 0), (1.6, 2.0, 10.6), OLIVE_D, bevel=0.5))
    return g


def turret():
    g = box((0, 0.8, -0.4), (3.8, 1.5, 4.8), OLIVE, bevel=0.5)
    g.extend(tube([(0, 0.9, 1.8), (0, 0.9, 9.4)], [0.45, 0.45], 6, OLIVE_D, cap1=INK))
    return g


def heli():
    g = ellipsoid((0, 0, 0.6), (2.4, 2.3, 4.6), 7, (0, 45, 90, 135, 180), lambda u: GLASS if u[2] > 0.25 and u[1] > -0.3 else OWN, frame=rot_x(math.pi / 2))
    g.extend(tube([(0, 0.6, -3.2), (0, 1.3, -10.6)], [0.9, 0.4], 5, OWN, cap1=OWN))
    g.extend(box((0, 2.3, -10.4), (0.3, 2.6, 1.6), OWN))
    for sx in (1, -1):
        g.extend(box((sx * 1.7, -2.7, 0.4), (0.3, 0.3, 6.4), INK))
        g.extend(box((sx * 1.7, -2.1, 0.4), (0.25, 1.0, 0.25), INK))
    g.extend(tube([(0, 2.0, 0.4), (0, 3.0, 0.4)], [0.35, 0.35], 4, INK))
    return g


def rotor():
    g = box((0, 0, 0), (17.0, 0.16, 1.0), INK)
    g.extend(box((0, 0, 0), (1.0, 0.16, 17.0), INK))
    return g


def jet():
    g = tube([(0, 0, -8), (0, 0, 5), (0, 0, 10)], [1.3, 1.3, 0.0], 6, STEEL, cap0=INK)
    wing = prism([(0.0, 0.0), (9.0, -7.0), (9.0, -9.5), (0.0, -8.0)], -0.15, 0.15, STEEL, smooth_deg=20.0)
    wing.xform(rot_x(-math.pi / 2))
    g.extend(wing.copy().xform(None, (0.8, 0, 0)))
    g.extend(wing.mirrored_x().xform(None, (-0.8, 0, 0)))
    g.extend(box((0, 2.2, -6.8), (0.25, 3.2, 2.6), STEEL))
    g.extend(geosphere((0, 1.0, 3.6), (0.8, 0.8, 2.2), 1, GLASS))
    return g


def cube():
    return box((0, 0, 0), (1.0, 1.0, 1.0), OWN)


def donut():
    n, R, r = 14, 5.2, 2.3
    path = [(math.cos(i * 2 * math.pi / n) * R, R + r + math.sin(i * 2 * math.pi / n) * R, 0.0) for i in range(n)]
    g = tube(path, [r] * n, 8, Paint(rgb(236, 182, 112)), closed_path=True, up=(0, 0, 1))
    g.repaint_corners(lambda p, n_, old: Paint(rgb(250, 130, 180)) if n_[2] > 0.15 else old)
    for k in range(9):
        a = k * 2 * math.pi / 9 + 0.3
        c = (math.cos(a) * R, R + r + math.sin(a) * R, r * 0.95)
        g.extend(box(c, (0.5, 1.4, 0.35), (WHITE, Paint(rgb(110, 210, 240)), Paint(rgb(255, 226, 110)))[k % 3], frame=rot_z(a * 2.3)))
    g.extend(box((0, 0.4, 0), (1.2, 1.0, 1.2), INK))
    return g


def water_tank():
    wood, top = Paint(rgb(196, 138, 96)), Paint(rgb(170, 72, 64))
    g = tube([(0, 2.4, 0), (0, 6.8, 0)], [2.8, 2.8], 8, wood, cap0=wood)
    g.extend(tube([(0, 6.8, 0), (0, 8.8, 0)], [3.2, 0.0], 8, top, cap0=top))
    for sx in (1, -1):
        for sz in (1, -1):
            g.extend(box((sx * 1.7, 1.2, sz * 1.7), (0.4, 2.4, 0.4), INK))
    return g


def missile():
    g = tube([(0, 0, -2.0), (0, 0, 1.2), (0, 0, 2.4)], [0.5, 0.5, 0.0], 5, WHITE, cap0=Paint(rgb(255, 150, 60)))
    g.repaint_corners(lambda p, n, old: Paint(rgb(232, 64, 64)) if p[2] > 1.1 else old)
    return g


# ---------------------------------------------------------------------------

def main():
    cel = [("TORSO", torso()), ("HEAD", head()), ("JAW", jaw()),
           ("UPPER_ARM", upper_arm()), ("FOREARM", forearm()),
           ("THIGH", thigh()), ("SHIN", shin()), ("FOOT", foot())]
    cel += [(f"TAIL{i}", tail(i)) for i in range(5)]
    flat = [("CAR", car(), 0.0), ("BUS", bus(), 0.0), ("TANK", tank(), 0.22), ("TURRET", turret(), 0.0),
            ("HELI", heli(), 0.2), ("ROTOR", rotor(), 0.0), ("JET", jet(), 0.25), ("CUBE", cube(), 0.0),
            ("DONUT", donut(), 0.3), ("WTANK", water_tank(), 0.22), ("MISSILE", missile(), 0.0)]
    meshes = []
    for name, g in cel:
        v, i = weld([g], name, drop_collapsed=True)
        m = Mesh(name, v, i)
        check_mesh(m, 3)
        meshes += [m, ink_shell(m, 0.34, INK.argb)]
    for name, g, ink in flat:
        v, i = weld([g.flat()], name, normal_tol=12.0, drop_collapsed=True)
        m = Mesh(name, v, i)
        check_mesh(m, 3)
        meshes.append(m)
        if ink:
            meshes.append(ink_shell(m, ink, INK.argb))
    verts, indices, descs = pack(meshes)
    for d in descs:
        print(f"  {d[4].name:14s} {d[1]:4d} verts {d[3] // 3:4d} tris")
        assert d[1] <= 1024, d[4].name
    print(f"{len(verts)} verts, {len(indices) // 3} tris")

    def c3(name, p):
        return [f"#define KJ_{name}_X {p[0]:.3f}f", f"#define KJ_{name}_Y {p[1]:.3f}f", f"#define KJ_{name}_Z {p[2]:.3f}f"]
    lines = ['#include "engine/mesh.h"', "",
             "/* The kaiju's joints, metres. */",
             f"#define KJ_THIGH_LEN {THIGH_LEN:.3f}f", f"#define KJ_SHIN_LEN {SHIN_LEN:.3f}f",
             f"#define KJ_ANKLE_H {ANKLE_H:.3f}f", f"#define KJ_HIP_Y {HIP_Y:.3f}f", f"#define KJ_HIP_X {HIP_X:.3f}f",
             f"#define KJ_UPPER_ARM_LEN {UPPER_ARM_LEN:.3f}f", f"#define KJ_FOREARM_LEN {FOREARM_LEN:.3f}f",
             *c3("NECK", NECK), *c3("JAW", JAW), *c3("MOUTH", MOUTH_POS), *c3("SHOULDER", SHOULDER), *c3("TAIL_ROOT", TAIL_ROOT),
             "#define KJ_TAIL_SEGS 5",
             "static const float kj_tail_len[KJ_TAIL_SEGS] __attribute__((unused)) = { " + ", ".join(f"{x:.3f}f" for x in TAIL_LEN) + " };", "",
             "enum {"]
    lines += [f"    KM_{d[4].name}," for d in descs]
    lines += ["    KM_COUNT", "};", "",
              "typedef struct { uint32_t v0, i0; uint16_t nv, ni; } kl_mesh_desc_t;",
              "static const kl_mesh_desc_t kl_meshes[KM_COUNT] __attribute__((unused)) = {"]
    lines += [f"    {{ {d[0]}, {d[2]}, {d[1]}, {d[3]} }},  /* {d[4].name} */" for d in descs]
    lines += ["};", "", "/* The data itself, for the one file that builds the meshes (src/cel.c). */", "#ifdef KL_MODELS_DATA",
              f"static const eng_mvert_t kl_mverts[{len(verts)}] = {{", *vert_rows(verts, 5), "};", "",
              f"static const uint16_t kl_midx[{len(indices)}] = {{", *index_rows(indices, 24), "};", "#endif"]
    write_header(GEN / "models.h", "tools/build_models.py", "KL_MODELS_H", lines)
    print(f"wrote {GEN / 'models.h'}")


if __name__ == "__main__":
    main()
