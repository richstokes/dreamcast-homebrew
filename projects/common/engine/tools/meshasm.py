"""Assembly of meshkit geometry into the games' generated model headers.

Shared by DreamKarts' tools/build_models.py and Ridgeline's tools/build_trucks.py:
welding triangle soups into indexed vertices (with optional normal clustering so
the engine lights fewer groups), the sanity checks, per-mesh stats, and the C
header and preview JSON emitters. Each game keeps a Mesh subclass with its
identity fields, its vertex struct text and whatever else its header holds.

A welded vertex is (pos[3], nrm_i8[3], slot, argb, tip): normal * 127, the
paint slot, the colour and the 0..255 tip blend (always 0 when a game does not
paint tips). Paint slot 0 is the fixed colour in every game.
"""

from __future__ import annotations

import json
import math
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from meshkit import Geo, _pkey, unit  # noqa: E402


@dataclass
class Mesh:
    name: str      # for messages, e.g. "RL_PART_BODY lod0"
    verts: list    # (pos[3], nrm_i8[3], slot, argb, tip)
    indices: list  # triangles, relative to this mesh's first vertex


def q8(n) -> tuple:
    return tuple(int(max(-127, min(127, round(float(x) * 127.0)))) for x in n)


def cluster_normals(geos: Iterable[Geo], tol_deg: float) -> dict:
    """Per paint colour, merge vertex normals within tol_deg of each other (the
    game lights each (normal, slot, colour) group once). Greedy over sorted
    normals, then one pass moving each centre to its members' mean. Dark fixed
    colours take a wider tolerance since the shading hardly shows on them.
    Returns {(slot, argb, q8 normal): q8 normal}."""
    by_col: dict[tuple, set] = defaultdict(set)
    for g in geos:
        for t in range(len(g.P)):
            for k in range(3):
                c = g.C[t][k]
                by_col[(c.slot, c.argb)].add(q8(g.N[t][k]))
    out = {}
    for col in sorted(by_col):
        r, gg, b = (col[1] >> 16) & 0xFF, (col[1] >> 8) & 0xFF, col[1] & 0xFF
        dark = (0.299 * r + 0.587 * gg + 0.114 * b) / 255.0 < 0.25 and col[0] == 0
        cos_t = math.cos(math.radians(tol_deg * (1.8 if dark else 1.0)))
        ns = sorted(by_col[col])
        vec = {n: unit(np.array(n, dtype=float)) for n in ns}
        centres: list[np.ndarray] = []
        assign = {}
        for n in ns:
            best, best_d = None, cos_t
            for ci, c in enumerate(centres):
                d = float(np.dot(vec[n], c))
                if d >= best_d:
                    best, best_d = ci, d
            if best is None:
                centres.append(vec[n])
                best = len(centres) - 1
            assign[n] = best
        sums = [np.zeros(3) for _ in centres]
        for n, ci in assign.items():
            sums[ci] += vec[n]
        cq = [q8(unit(s)) for s in sums]
        for n, ci in assign.items():
            out[col + (n,)] = cq[ci]
    return out


def weld(geos: Iterable[Geo], name: str, *, pivot=None, digits: int = 6, normal_tol: float | None = None,
         drop_collapsed: bool = False) -> tuple[list, list]:
    """Index the triangle soups, sharing corners with equal position (to 1e-5),
    quantised normal and paint. Positions are taken relative to pivot and
    rounded to the given decimals. normal_tol clusters normals first (see
    cluster_normals). drop_collapsed removes triangles the weld folded onto
    two corners instead of leaving them for check_mesh."""
    geos = list(geos)
    snap = cluster_normals(geos, normal_tol) if normal_tol is not None else None
    verts, index, lookup = [], [], {}
    for g in geos:
        for t in range(len(g.P)):
            assert g.N[t] is not None, f"{name}: triangle without normals"
            for k in range(3):
                p = g.P[t][k] if pivot is None else g.P[t][k] - pivot
                c = g.C[t][k]
                ni = q8(g.N[t][k])
                if snap is not None:
                    ni = snap[(c.slot, c.argb, ni)]
                key = (_pkey(p), ni, c.argb, c.slot, c.tip)
                if key not in lookup:
                    lookup[key] = len(verts)
                    verts.append(([round(float(x), digits) + 0.0 for x in p], ni, c.slot, c.argb, c.tip))
                index.append(lookup[key])
    if drop_collapsed:
        tris = [index[i:i + 3] for i in range(0, len(index), 3)]
        index = [i for t in tris if len(set(t)) == 3 for i in t]
    return verts, index


def check_mesh(m: Mesh, slot_count: int) -> None:
    """Triangles are proper, wound to agree with their normals, and every
    vertex's slot, tip and alpha are in range."""
    assert len(m.indices) % 3 == 0
    assert len(m.verts) < 65536 and max(m.indices) < 65536
    P = np.array([v[0] for v in m.verts])
    Nq = np.array([v[1] for v in m.verts], dtype=float) / 127.0
    for t in range(0, len(m.indices), 3):
        a, b, c = m.indices[t:t + 3]
        assert len({a, b, c}) == 3, f"{m.name}: repeated index in triangle {t // 3}"
        fn = np.cross(P[b] - P[a], P[c] - P[a])
        area = 0.5 * float(np.linalg.norm(fn))
        assert area > 1e-7, f"{m.name}: degenerate triangle {t // 3} (area {area:g})"
        mean_n = Nq[a] + Nq[b] + Nq[c]
        assert float(np.dot(fn, mean_n)) > 0, f"{m.name}: triangle {t // 3} winding disagrees with its normals"
    for v in m.verts:
        assert 0 <= v[2] < slot_count, f"{m.name}: paint slot {v[2]}"
        assert 0 <= v[4] <= 255
        assert (v[3] >> 24) == 0xFF


def position_count(m: Mesh) -> int:
    return len({tuple(v[0]) for v in m.verts})


def light_group_count(m: Mesh) -> int:
    """Distinct (normal, slot, colour) combinations: what the engine lights."""
    return len({(v[1], v[2], v[3]) for v in m.verts})


# --------------------------------------------------------------------------
# Emit
# --------------------------------------------------------------------------


def cf(x: float, sig: int = 6) -> str:
    """A C float literal with sig significant digits."""
    s = f"{x:.{sig}g}"
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def cf3(p, sig: int = 6) -> str:
    return "{ " + ", ".join(cf(float(x), sig) for x in p) + " }"


def pack(meshes: list) -> tuple[list, list, list]:
    """Concatenate the meshes' vertices and indices. Returns (verts, indices,
    descs) with descs = (first_vert, vert_count, first_index, index_count, mesh)."""
    verts, indices, descs = [], [], []
    for m in meshes:
        descs.append((len(verts), len(m.verts), len(indices), len(m.indices), m))
        verts += m.verts
        indices += m.indices
    return verts, indices, descs


def vert_rows(verts: list, sig: int = 6, tip: bool = False) -> list[str]:
    """One initialiser per vertex: x, y, z, nx, ny, nz, slot, argb and, with
    tip, the tip weight plus a 3-byte pad."""
    rows = []
    for p, n, slot, argb, tp in verts:
        row = f"    {{ {cf(p[0], sig)}, {cf(p[1], sig)}, {cf(p[2], sig)}, {n[0]}, {n[1]}, {n[2]}, {slot}, 0x{argb:08X}u"
        if tip:
            row += f", {tp}, {{ 0, 0, 0 }}"
        rows.append(row + " },")
    return rows


def index_rows(indices: list, per_line: int) -> list[str]:
    return ["    " + ", ".join(str(x) for x in indices[i:i + per_line]) + "," for i in range(0, len(indices), per_line)]


def mesh_rows(descs: list, fields: Callable[[Mesh], str]) -> list[str]:
    """One initialiser per mesh: the four pack() offsets then fields(mesh)."""
    return [f"    {{ {d[0]}, {d[1]}, {d[2]}, {d[3]}, {fields(d[4])} }}," for d in descs]


def write_header(path: Path, generator: str, guard: str, lines: list[str]) -> None:
    out = [f"/* Generated by {generator}. Do not edit. */", f"#ifndef {guard}", f"#define {guard}",
           "#include <stdint.h>", *lines, "#endif"]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out) + "\n")


def mesh_json(m: Mesh) -> dict:
    """The mesh as the Blender preview scripts read it; add the game's identity keys."""
    return {
        "pos": [v[0] for v in m.verts],
        "nrm": [[c / 127.0 for c in v[1]] for v in m.verts],
        "slot": [v[2] for v in m.verts],
        "argb": [v[3] for v in m.verts],
        "idx": m.indices,
    }


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data))
