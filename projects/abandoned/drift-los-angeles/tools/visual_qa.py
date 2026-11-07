#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Audit Drift Los Angeles art assets, hero-car geometry, and QA captures.

The checks are intentionally platform-facing rather than aesthetic theatre:
they protect the Dreamcast VRAM budget, the sports-car silhouette, material
coverage, readable 480p contrast, street-level detail, and visual separation
between district captures.  Subjective review still decides whether a frame is
good; this script makes it difficult to regress unnoticed between reviews.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from statistics import mean, pstdev
from typing import Any

from PIL import Image, ImageDraw, ImageFilter, ImageOps, ImageStat
from pvr_texture import texture_byte_size


PROJECT_DIR = Path(__file__).resolve().parents[1]
MODEL_PATH = PROJECT_DIR / "model_data.h"
TEXTURE_TOOL_PATH = PROJECT_DIR / "tools" / "build_textures.py"
SOURCE_DIR = PROJECT_DIR / "assets" / "source"
HUD_BYTES = 512 * 256 * 2
HARDWARE_VRAM_BYTES = 8 * 1024 * 1024
MAX_CAR_MESH_VERTICES = 4096
MAX_CAR_MESH_FACES = 4096


@dataclass
class CaptureMetrics:
    path: str
    width: int
    height: int
    luminance_mean: float
    luminance_stddev: float
    saturation_mean: float
    edge_energy: float
    street_edge_energy: float
    hero_readability_fraction: float
    crushed_black_fraction: float
    clipped_highlight_fraction: float
    palette_bins: int
    rgb_histogram: list[float]


def load_texture_specs() -> tuple[Any, ...]:
    spec = importlib.util.spec_from_file_location("dla_build_textures", TEXTURE_TOOL_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load build_textures.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return tuple(module.SPECS)


def parse_model(path: Path = MODEL_PATH) -> dict[str, Any]:
    source = path.read_text(encoding="utf-8")
    vertex_source = source.split(
        "static const dla_mesh_vertex_t dla_car_vertices[] = {", 1
    )[1].split("};", 1)[0]
    face_source = source.split(
        "static const dla_mesh_face_t dla_car_faces[] = {", 1
    )[1].split("};", 1)[0]
    vertices = [
        tuple(float(value.strip().removesuffix("f")) for value in row.split(","))
        for row in re.findall(r"\{([^{}]+)\}", vertex_source)
    ]
    faces = [tuple(int(value.strip()) for value in row.split(","))
             for row in re.findall(r"\{([^{}]+)\}", face_source)]
    if not vertices or not faces:
        raise RuntimeError("model_data.h did not contain a parseable car mesh")
    if len({len(vertex) for vertex in vertices}) != 1 or len(vertices[0]) not in (8, 9):
        raise RuntimeError("expected xyz, uv, normal and optional AO in each vertex")
    if any(len(face) != 4 for face in faces):
        raise RuntimeError("expected three vertex indices and a material in each face")
    if any(not math.isfinite(value) for vertex in vertices for value in vertex):
        raise RuntimeError("model contains a non-finite vertex attribute")

    xs = [vertex[0] for vertex in vertices]
    ys = [vertex[1] for vertex in vertices]
    zs = [vertex[2] for vertex in vertices]
    material_faces = [0] * 5
    material_vertices: list[set[int]] = [set() for _ in range(5)]
    collapsed_uv_faces = [0] * 5
    parents = list(range(len(vertices)))

    def find(index: int) -> int:
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    def union(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parents[right_root] = left_root

    invalid_normals = 0
    invalid_ao = 0
    for vertex in vertices:
        normal_length = math.sqrt(sum(component * component for component in vertex[5:8]))
        if not 0.92 <= normal_length <= 1.08:
            invalid_normals += 1
        if len(vertex) == 9 and not 0.0 <= vertex[8] <= 1.0:
            invalid_ao += 1
    for a, b, c, material in faces:
        if min(a, b, c) < 0 or max(a, b, c) >= len(vertices):
            raise RuntimeError("model face index exceeds exported vertex count")
        if material < 0 or material >= len(material_faces):
            raise RuntimeError(f"model uses unsupported material id {material}")
        material_faces[material] += 1
        material_vertices[material].update((a, b, c))
        ua, va = vertices[a][3:5]
        ub, vb = vertices[b][3:5]
        uc, vc = vertices[c][3:5]
        if abs((ub - ua) * (vc - va) - (uc - ua) * (vb - va)) < 1e-10:
            collapsed_uv_faces[material] += 1
        if material == 0:
            union(a, b)
            union(b, c)

    # Carbon intentionally tiles its weave; metal is untextured. Paint,
    # glass and lamps each address one atlas and must remain within it.
    atlas_vertices = set().union(*(material_vertices[index] for index in (0, 1, 3)))
    invalid_uvs = sum(any(not -0.00001 <= value <= 1.00001
                          for value in vertices[index][3:5])
                      for index in atlas_vertices)
    shared_material_vertices = sum(
        sum(index in indices for indices in material_vertices) > 1
        for index in range(len(vertices))
    )

    # UV seams and hard normals intentionally duplicate a geometric vertex.
    # Weld only the painted shell for the connectivity/proportions check;
    # keep the original split count when checking the renderer scratch limit.
    positions: dict[tuple[float, ...], int] = {}
    for index in material_vertices[0]:
        position = tuple(round(value, 5) for value in vertices[index][:3])
        if position in positions:
            union(index, positions[position])
        else:
            positions[position] = index

    width = max(xs) - min(xs)
    height = max(ys) - min(ys)
    length = max(zs) - min(zs)
    # Material zero is the continuous painted body shell.  Mirrors, fuel-door
    # trim and lamp bezels deliberately extend beyond it, so using the complete
    # detail mesh to judge vehicle proportions produces a false wide-car alarm.
    paint_components: dict[int, set[int]] = {}
    for index in material_vertices[0]:
        paint_components.setdefault(find(index), set()).add(index)
    if not paint_components:
        raise RuntimeError("model has no painted body shell")
    body_indices = max(paint_components.values(), key=lambda indices: len({
        vertices[index][:3] for index in indices
    }))
    body = [vertices[index] for index in body_indices]
    body_width = max(vertex[0] for vertex in body) - min(vertex[0] for vertex in body)
    # The greenhouse is intentionally a separate glass component, so overall
    # height comes from the complete assembly while width/length come from the
    # continuous shell (excluding mirrors and trim).
    body_height = height
    body_length = max(vertex[2] for vertex in body) - min(vertex[2] for vertex in body)
    if body_width <= 0.0:
        raise RuntimeError("painted body shell has zero width")
    range_match = re.search(
        r"dla_car_material_ranges\s*\[[^]]*\]\s*=\s*\{(.*?)\};", source, re.S
    )
    invalid_material_ranges = False
    if range_match:
        ranges = [tuple(map(int, values)) for values in re.findall(
            r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", range_match.group(1)
        )]
        cursor = 0
        invalid_material_ranges = len(ranges) != 5
        for material, (first, count) in enumerate(ranges):
            if first != cursor or first + count > len(faces):
                invalid_material_ranges = True
            if any(face[3] != material for face in faces[first:first + count]):
                invalid_material_ranges = True
            cursor = first + count
        invalid_material_ranges |= cursor != len(faces)
    return {
        "vertices": len(vertices),
        "triangles": len(faces),
        "width": round(width, 4),
        "height": round(height, 4),
        "length": round(length, 4),
        "body_width": round(body_width, 4),
        "body_height": round(body_height, 4),
        "body_length": round(body_length, 4),
        "length_width_ratio": round(body_length / body_width, 4),
        "height_width_ratio": round(body_height / body_width, 4),
        "renderer_vertex_capacity": MAX_CAR_MESH_VERTICES,
        "renderer_face_capacity": MAX_CAR_MESH_FACES,
        "material_triangles": material_faces,
        "shared_material_vertices": shared_material_vertices,
        "invalid_normals": invalid_normals,
        "invalid_uvs": invalid_uvs,
        "collapsed_uv_triangles_by_material": collapsed_uv_faces,
        "invalid_ao": invalid_ao,
        "ao_range": [min(vertex[8] if len(vertex) == 9 else 1.0 for vertex in vertices),
                     max(vertex[8] if len(vertex) == 9 else 1.0 for vertex in vertices)],
        "has_material_ranges": range_match is not None,
        "invalid_material_ranges": invalid_material_ranges,
    }


def audit_textures() -> dict[str, Any]:
    specs = load_texture_specs()
    total_bytes = HUD_BYTES
    source_files: dict[str, tuple[int, int]] = {}
    invalid_output_dimensions: list[str] = []
    missing_sources: list[str] = []
    mipmapped_textures: list[str] = []
    alpha_textures: list[str] = []
    for texture in specs:
        size = (texture.size, texture.size) if isinstance(texture.size, int) else texture.size
        mipmap = getattr(texture, "mipmap", False)
        if mipmap:
            mipmapped_textures.append(texture.name)
        if getattr(texture, "alpha", False):
            alpha_textures.append(texture.name)
        try:
            total_bytes += texture_byte_size(*size, mipmap=mipmap)
        except ValueError as error:
            invalid_output_dimensions.append(f"{texture.name}:{size[0]}x{size[1]}: {error}")
        source_path = SOURCE_DIR / texture.source
        if not source_path.is_file():
            missing_sources.append(texture.source)
            continue
        if texture.source not in source_files:
            with Image.open(source_path) as image:
                source_files[texture.source] = image.size
    return {
        "texture_count": len(specs),
        "mipmapped_textures": mipmapped_textures,
        "alpha_textures": alpha_textures,
        "source_count": len(source_files),
        "source_dimensions": source_files,
        "pvr_bytes_including_hud": total_bytes,
        "pvr_mib_including_hud": round(total_bytes / (1024 * 1024), 3),
        "hardware_vram_bytes": HARDWARE_VRAM_BYTES,
        "allocation_validation": (
            "Static bytes are an inventory, not an available-texture budget. "
            "Framebuffers, PVR vertex buffers and tile bins share the 8 MiB VRAM. "
            "Confirm successful runtime allocations and free VRAM with "
            "analyze_render_log.py after the full scene has initialized."
        ),
        "invalid_output_dimensions": invalid_output_dimensions,
        "missing_sources": sorted(set(missing_sources)),
    }


def channel_histogram(image: Image.Image, bins: int = 16) -> list[float]:
    small = ImageOps.fit(image.convert("RGB"), (96, 72), Image.Resampling.BILINEAR)
    counts = [0] * (bins * 3)
    pixels = list(small.get_flattened_data())
    for red, green, blue in pixels:
        counts[(red * bins) // 256] += 1
        counts[bins + (green * bins) // 256] += 1
        counts[bins * 2 + (blue * bins) // 256] += 1
    scale = 1.0 / max(1, len(pixels))
    return [round(value * scale, 7) for value in counts]


def capture_metrics(path: Path) -> CaptureMetrics:
    with Image.open(path) as opened:
        image = opened.convert("RGB")
    width, height = image.size
    analysis = ImageOps.fit(image, (320, 240), Image.Resampling.LANCZOS)
    gray = ImageOps.grayscale(analysis)
    gray_values = list(gray.get_flattened_data())
    hsv = analysis.convert("HSV")
    saturation = [pixel[1] / 255.0 for pixel in hsv.get_flattened_data()]
    edges = gray.filter(ImageFilter.FIND_EDGES)
    street = gray.crop((0, 118, 320, 240)).filter(ImageFilter.FIND_EDGES)
    hero_rgb = analysis.crop((70, 132, 250, 240))
    hero_gray = ImageOps.grayscale(hero_rgb)
    hero_hsv = hero_rgb.convert("HSV")
    hero_luminance = list(hero_gray.get_flattened_data())
    hero_saturation = [pixel[1] / 255.0 for pixel in hero_hsv.get_flattened_data()]
    hero_fraction = sum(
        luminance > 132 and saturation < .34
        for luminance, saturation in zip(hero_luminance, hero_saturation)
    ) / len(hero_luminance)
    edge_energy = ImageStat.Stat(edges).mean[0] / 255.0
    street_edge_energy = ImageStat.Stat(street).mean[0] / 255.0
    quantized = analysis.quantize(colors=64, method=Image.Quantize.MEDIANCUT)
    palette_bins = sum(1 for count in quantized.getcolors(maxcolors=64) or [] if count)
    return CaptureMetrics(
        path=str(path),
        width=width,
        height=height,
        luminance_mean=round(mean(gray_values), 3),
        luminance_stddev=round(pstdev(gray_values), 3),
        saturation_mean=round(mean(saturation), 4),
        edge_energy=round(edge_energy, 5),
        street_edge_energy=round(street_edge_energy, 5),
        hero_readability_fraction=round(hero_fraction, 5),
        crushed_black_fraction=round(
            sum(value < 8 for value in gray_values) / len(gray_values), 5
        ),
        clipped_highlight_fraction=round(
            sum(value > 247 for value in gray_values) / len(gray_values), 5
        ),
        palette_bins=palette_bins,
        rgb_histogram=channel_histogram(analysis),
    )


def histogram_distance(left: CaptureMetrics, right: CaptureMetrics) -> float:
    # Each RGB channel sums to one, so six is the maximum L1 separation.
    return sum(abs(a - b) for a, b in zip(left.rgb_histogram, right.rgb_histogram)) / 6.0


def make_contact_sheet(captures: list[CaptureMetrics], output: Path) -> None:
    if not captures:
        return
    cell_w, cell_h = 480, 390
    columns = 2
    rows = (len(captures) + columns - 1) // columns
    sheet = Image.new("RGB", (cell_w * columns, cell_h * rows), (7, 10, 20))
    draw = ImageDraw.Draw(sheet)
    for index, capture in enumerate(captures):
        with Image.open(capture.path) as opened:
            frame = ImageOps.fit(opened.convert("RGB"), (cell_w, 360), Image.Resampling.LANCZOS)
        x = (index % columns) * cell_w
        y = (index // columns) * cell_h
        sheet.paste(frame, (x, y))
        label = (
            f"{Path(capture.path).stem}  L {capture.luminance_mean:.0f}  "
            f"C {capture.luminance_stddev:.0f}  E {capture.street_edge_energy:.3f}  "
            f"H {capture.hero_readability_fraction:.2f}"
        )
        draw.rectangle((x, y + 360, x + cell_w, y + cell_h), fill=(7, 10, 20))
        draw.text((x + 8, y + 368), label, fill=(226, 236, 255))
    output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(output, optimize=True)


def evaluate(report: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    model = report["model"]
    textures = report["textures"]
    if not 1 <= model["vertices"] <= MAX_CAR_MESH_VERTICES:
        failures.append("hero car exceeds the renderer's 4,096-vertex scratch capacity")
    if not 1 <= model["triangles"] <= MAX_CAR_MESH_FACES:
        failures.append("hero car exceeds the renderer's 4,096-face visibility capacity")
    if not 2.30 <= model["length_width_ratio"] <= 2.62:
        failures.append("hero car length/width ratio is outside sports-coupe bounds")
    if not 0.52 <= model["height_width_ratio"] <= 0.72:
        failures.append("hero car height/width ratio is outside low-coupe bounds")
    if any(count == 0 for count in model["material_triangles"]):
        failures.append("hero car does not exercise every renderer material")
    if model["shared_material_vertices"]:
        failures.append(f"hero car shares {model['shared_material_vertices']} vertices between materials; cached shading requires material splits")
    if model["invalid_normals"]:
        failures.append(f"hero car has {model['invalid_normals']} invalid normals")
    if model["invalid_uvs"]:
        failures.append(f"hero car has {model['invalid_uvs']} UVs outside its material atlas")
    collapsed_textured = sum(model["collapsed_uv_triangles_by_material"][:4])
    if collapsed_textured:
        failures.append(f"hero car has {collapsed_textured} textured triangles with collapsed UVs")
    if model["invalid_ao"]:
        failures.append(f"hero car has {model['invalid_ao']} AO values outside 0..1")
    if model["invalid_material_ranges"]:
        failures.append("hero car material ranges do not cover the matching faces exactly")
    if textures["pvr_bytes_including_hud"] >= HARDWARE_VRAM_BYTES:
        failures.append("textures alone exhaust the Dreamcast's 8 MiB VRAM")
    if textures["invalid_output_dimensions"]:
        failures.append("one or more texture dimensions violate PVR size/mipmap requirements")
    if textures["missing_sources"]:
        failures.append("one or more texture source files are missing")

    captures = report["captures"]
    for capture in captures:
        name = Path(capture["path"]).name
        if not 42.0 <= capture["luminance_mean"] <= 185.0:
            failures.append(f"{name}: mean luminance is outside the readable dusk range")
        if capture["luminance_stddev"] < 34.0:
            failures.append(f"{name}: insufficient scene contrast")
        if capture["saturation_mean"] < 0.20:
            failures.append(f"{name}: color separation is too weak")
        if capture["street_edge_energy"] < 0.085:
            failures.append(f"{name}: lower-frame street detail is too sparse")
        if not 0.055 <= capture["hero_readability_fraction"] <= 0.34:
            failures.append(f"{name}: pearl hero-car presence is outside the composition target")
        if capture["crushed_black_fraction"] > 0.22:
            failures.append(f"{name}: too much of the frame is crushed to black")
        if capture["clipped_highlight_fraction"] > 0.10:
            failures.append(f"{name}: too much of the frame is clipped white")

    if len(captures) >= 4:
        distances = report["district_histogram_distances"]
        if min(distances.values()) < 0.085:
            failures.append("at least two district captures are not visually separated enough")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("screenshots", nargs="*", type=Path)
    parser.add_argument("--contact-sheet", type=Path)
    parser.add_argument("--json", dest="json_path", type=Path)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    captures = [capture_metrics(path.resolve()) for path in args.screenshots]
    distances: dict[str, float] = {}
    for left_index, left in enumerate(captures):
        for right in captures[left_index + 1:]:
            key = f"{Path(left.path).stem}__{Path(right.path).stem}"
            distances[key] = round(histogram_distance(left, right), 5)
    report: dict[str, Any] = {
        "model": parse_model(),
        "textures": audit_textures(),
        "captures": [asdict(capture) for capture in captures],
        "district_histogram_distances": distances,
    }
    failures = evaluate(report)
    report["failures"] = failures

    if args.contact_sheet:
        make_contact_sheet(captures, args.contact_sheet.resolve())
    if args.json_path:
        args.json_path.resolve().parent.mkdir(parents=True, exist_ok=True)
        args.json_path.resolve().write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(json.dumps(report, indent=2))
    if failures:
        print("\nVisual QA failures:")
        for failure in failures:
            print(f"  - {failure}")
    else:
        print("\nVisual QA: all configured gates passed.")
    if args.strict and failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
