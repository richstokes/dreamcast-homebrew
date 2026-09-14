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


PROJECT_DIR = Path(__file__).resolve().parents[1]
MODEL_PATH = PROJECT_DIR / "model_data.h"
TEXTURE_TOOL_PATH = PROJECT_DIR / "tools" / "build_textures.py"
SOURCE_DIR = PROJECT_DIR / "assets" / "source"
HUD_BYTES = 512 * 256 * 2
VRAM_ART_BUDGET = 4 * 1024 * 1024


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


def parse_model() -> dict[str, Any]:
    source = MODEL_PATH.read_text(encoding="utf-8")
    vertex_source = source.split(
        "static const dla_mesh_vertex_t dla_car_vertices[] = {", 1
    )[1].split("};", 1)[0]
    face_source = source.split(
        "static const dla_mesh_face_t dla_car_faces[] = {", 1
    )[1].split("};", 1)[0]
    number = r"[-+ ]?(?:\d+(?:\.\d*)?|\.\d+)f?"
    vertex_pattern = re.compile(
        rf"\{{\s*({number}),\s*({number}),\s*({number}),\s*"
        rf"({number}),\s*({number}),\s*({number}),\s*({number}),\s*({number})\s*\}}"
    )
    face_pattern = re.compile(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}")
    vertices = [
        tuple(float(value.rstrip("f")) for value in match.groups())
        for match in vertex_pattern.finditer(vertex_source)
    ]
    faces = [tuple(int(value) for value in match.groups())
             for match in face_pattern.finditer(face_source)]
    if not vertices or not faces:
        raise RuntimeError("model_data.h did not contain a parseable car mesh")

    xs = [vertex[0] for vertex in vertices]
    ys = [vertex[1] for vertex in vertices]
    zs = [vertex[2] for vertex in vertices]
    material_faces = [0] * 8
    material_vertices: list[set[int]] = [set() for _ in range(8)]
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
    for vertex in vertices:
        normal_length = math.sqrt(sum(component * component for component in vertex[5:8]))
        if not 0.92 <= normal_length <= 1.08:
            invalid_normals += 1
    for a, b, c, material in faces:
        if max(a, b, c) >= len(vertices):
            raise RuntimeError("model face index exceeds exported vertex count")
        if material >= len(material_faces):
            raise RuntimeError(f"model uses unsupported material id {material}")
        material_faces[material] += 1
        material_vertices[material].update((a, b, c))
        if material == 0:
            union(a, b)
            union(b, c)

    width = max(xs) - min(xs)
    height = max(ys) - min(ys)
    length = max(zs) - min(zs)
    # Material zero is the continuous painted body shell.  Mirrors, fuel-door
    # trim and lamp bezels deliberately extend beyond it, so using the complete
    # detail mesh to judge vehicle proportions produces a false wide-car alarm.
    paint_components: dict[int, set[int]] = {}
    for index in material_vertices[0]:
        paint_components.setdefault(find(index), set()).add(index)
    body_indices = max(paint_components.values(), key=len)
    body = [vertices[index] for index in body_indices]
    body_width = max(vertex[0] for vertex in body) - min(vertex[0] for vertex in body)
    # The greenhouse is intentionally a separate glass component, so overall
    # height comes from the complete assembly while width/length come from the
    # continuous shell (excluding mirrors and trim).
    body_height = height
    body_length = max(vertex[2] for vertex in body) - min(vertex[2] for vertex in body)
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
        "material_triangles": material_faces[:5],
        "invalid_normals": invalid_normals,
    }


def audit_textures() -> dict[str, Any]:
    specs = load_texture_specs()
    total_bytes = HUD_BYTES
    source_files: dict[str, tuple[int, int]] = {}
    invalid_output_dimensions: list[str] = []
    missing_sources: list[str] = []
    for texture in specs:
        size = (texture.size, texture.size) if isinstance(texture.size, int) else texture.size
        total_bytes += size[0] * size[1] * 2
        if any(value <= 0 or value & (value - 1) for value in size):
            invalid_output_dimensions.append(f"{texture.name}:{size[0]}x{size[1]}")
        source_path = SOURCE_DIR / texture.source
        if not source_path.is_file():
            missing_sources.append(texture.source)
            continue
        if texture.source not in source_files:
            with Image.open(source_path) as image:
                source_files[texture.source] = image.size
    return {
        "texture_count": len(specs),
        "source_count": len(source_files),
        "source_dimensions": source_files,
        "pvr_bytes_including_hud": total_bytes,
        "pvr_mib_including_hud": round(total_bytes / (1024 * 1024), 3),
        "budget_mib": round(VRAM_ART_BUDGET / (1024 * 1024), 3),
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
    if model["vertices"] < 1500:
        failures.append("hero car has fewer than 1,500 vertices")
    if not 2600 <= model["triangles"] <= 6500:
        failures.append("hero car triangle count is outside the 2,600..6,500 target")
    if not 2.30 <= model["length_width_ratio"] <= 2.62:
        failures.append("hero car length/width ratio is outside sports-coupe bounds")
    if not 0.52 <= model["height_width_ratio"] <= 0.72:
        failures.append("hero car height/width ratio is outside low-coupe bounds")
    if any(count == 0 for count in model["material_triangles"]):
        failures.append("hero car does not exercise every renderer material")
    if model["invalid_normals"]:
        failures.append(f"hero car has {model['invalid_normals']} invalid normals")
    if textures["pvr_bytes_including_hud"] > VRAM_ART_BUDGET:
        failures.append("resident texture set exceeds the 4 MiB art budget")
    if textures["invalid_output_dimensions"]:
        failures.append("one or more PVR texture dimensions are not powers of two")
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
