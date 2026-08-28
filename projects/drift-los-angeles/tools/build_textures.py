#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Convert Drift Los Angeles's source atlases to PVR-ready RGB565 C arrays."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageOps


@dataclass(frozen=True)
class TextureSpec:
    name: str
    source: str
    quadrant: tuple[int, int] | None
    size: int | tuple[int, int]
    tileable: bool = False
    contrast: float = 1.0
    brightness: float = 1.0


SPECS = (
    TextureSpec("asphalt", "street-surface-atlas-v3.png", (0, 0), 256, True, 1.12, 1.12),
    TextureSpec("sidewalk", "street-surface-atlas-v3.png", (1, 0), 128, True, 1.10, 1.08),
    TextureSpec("pavers", "street-surface-atlas-v3.png", (0, 1), 128, True, 1.10, 1.08),
    TextureSpec("road_marks", "street-surface-atlas-v3.png", (1, 1), 128, False, 1.12, 1.08),
    TextureSpec("stucco", "city-material-atlas.png", (1, 0), 128, True, 1.02),
    TextureSpec("glass_facade", "city-material-atlas.png", (0, 1), 128, True, 1.12),
    TextureSpec("graffiti", "city-material-atlas.png", (1, 1), 128, True, 1.10),
    TextureSpec("car_paint", "vehicle-material-atlas.png", (0, 0), 128, True, 1.08, 1.52),
    TextureSpec("car_glass", "vehicle-material-atlas.png", (1, 0), 128, True, 1.10),
    TextureSpec("car_carbon", "vehicle-material-atlas.png", (0, 1), 128, True, 1.08),
    TextureSpec("car_lights", "vehicle-material-atlas.png", (1, 1), 128, False, 1.12),
    TextureSpec("storefront", "night-city-detail-atlas.png", (0, 0), 128, False, 1.12),
    TextureSpec("billboard", "night-city-detail-atlas.png", (1, 0), 128, False, 1.10),
    TextureSpec("lit_windows", "night-city-detail-atlas.png", (0, 1), 128, True, 1.14),
    TextureSpec("neon_facade", "night-city-detail-atlas.png", (1, 1), 128, False, 1.12),
    TextureSpec("district_coast", "district-material-atlas.png", (0, 0), 128, False, 1.10),
    TextureSpec("district_downtown", "district-material-atlas.png", (1, 0), 128, False, 1.12),
    TextureSpec("district_arts", "district-material-atlas.png", (0, 1), 128, False, 1.12),
    TextureSpec("district_neon", "district-material-atlas.png", (1, 1), 128, False, 1.14),
    TextureSpec("facade_downtown", "facade-atlas-v3.png", (0, 0), 256, False, 1.12, 1.12),
    TextureSpec("facade_coast", "facade-atlas-v3.png", (1, 0), 256, False, 1.08, 1.08),
    TextureSpec("facade_arts", "facade-atlas-v3.png", (0, 1), 256, False, 1.10, 1.08),
    TextureSpec("facade_neon", "facade-atlas-v3.png", (1, 1), 256, False, 1.14, 1.12),
    TextureSpec("facade_downtown_alt", "facade-variants-v5.png", (0, 0), 256, False, 1.12, 1.10),
    TextureSpec("facade_coast_alt", "facade-variants-v5.png", (1, 0), 256, False, 1.08, 1.08),
    TextureSpec("facade_arts_alt", "facade-variants-v5.png", (0, 1), 256, False, 1.10, 1.08),
    TextureSpec("facade_neon_alt", "facade-variants-v5.png", (1, 1), 256, False, 1.12, 1.10),
    TextureSpec("street_utility", "street-microdetail-v5.png", (0, 0), 128, False, 1.12, 1.06),
    TextureSpec("street_repair", "street-microdetail-v5.png", (1, 0), 128, False, 1.10, 1.04),
    TextureSpec("storefront_micro", "street-microdetail-v5.png", (0, 1), 128, False, 1.10, 1.06),
    TextureSpec("civic_micro", "street-microdetail-v5.png", (1, 1), 128, False, 1.10, 1.06),
    TextureSpec("sky_backdrop", "skyline-backdrop-v4.png", None, (512, 256), False, 1.08, 1.06),
    TextureSpec("title_art", "title-key-art-v2.png", None, (512, 256), False, 1.06),
)

BAYER_4X4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)


def crop_quadrant(image: Image.Image, quadrant: tuple[int, int]) -> Image.Image:
    half_w = image.width // 2
    half_h = image.height // 2
    qx, qy = quadrant
    inset = max(2, min(image.width, image.height) // 256)
    return image.crop((
        qx * half_w + inset,
        qy * half_h + inset,
        (qx + 1) * half_w - inset,
        (qy + 1) * half_h - inset,
    ))


def make_tileable(image: Image.Image, feather: int = 10) -> Image.Image:
    result = image.copy()
    pixels = result.load()
    width, height = result.size
    for y in range(height):
        row = [pixels[x, y] for x in range(width)]
        for distance in range(feather):
            blend = 1.0 - distance / float(feather)
            left = row[distance]
            right = row[width - 1 - distance]
            average = tuple((left[c] + right[c]) * 0.5 for c in range(3))
            pixels[distance, y] = tuple(round(left[c] * (1.0 - blend) + average[c] * blend) for c in range(3))
            pixels[width - 1 - distance, y] = tuple(round(right[c] * (1.0 - blend) + average[c] * blend) for c in range(3))
    for x in range(width):
        column = [pixels[x, y] for y in range(height)]
        for distance in range(feather):
            blend = 1.0 - distance / float(feather)
            top = column[distance]
            bottom = column[height - 1 - distance]
            average = tuple((top[c] + bottom[c]) * 0.5 for c in range(3))
            pixels[x, distance] = tuple(round(top[c] * (1.0 - blend) + average[c] * blend) for c in range(3))
            pixels[x, height - 1 - distance] = tuple(round(bottom[c] * (1.0 - blend) + average[c] * blend) for c in range(3))
    return result


def make_pearl_white(image: Image.Image) -> Image.Image:
    """Retain the source reflection structure but remap blue paint to pearl."""
    luminance = ImageEnhance.Contrast(ImageOps.grayscale(image)).enhance(0.72)
    pixels = []
    for value in luminance.get_flattened_data():
        pearl = 176 + value * 68 // 255
        pixels.append((min(255, pearl + 5), min(255, pearl + 6),
                       min(255, pearl + 10)))
    result = Image.new("RGB", image.size)
    result.putdata(pixels)
    return result


def make_vehicle_lights(image: Image.Image) -> Image.Image:
    """Build compact tail and projector cells for the low-resolution car."""
    result = image.copy()
    draw = ImageDraw.Draw(result)
    draw.rectangle((0, 0, 63, 63), fill=(24, 2, 5))
    draw.polygon(((3, 27), (9, 10), (53, 8), (61, 25),
                  (51, 55), (11, 53)), fill=(112, 4, 10))
    draw.polygon(((8, 28), (14, 16), (49, 15), (56, 27),
                  (46, 48), (16, 47)), fill=(229, 12, 22))
    draw.polygon(((14, 29), (20, 22), (44, 21), (50, 29),
                  (41, 40), (21, 40)), fill=(65, 3, 8))
    draw.line(((14, 18), (48, 17), (54, 27)), fill=(255, 78, 72), width=3)
    draw.line(((18, 45), (44, 45)), fill=(153, 4, 12), width=2)

    # A dedicated top-right headlamp cell prevents the source atlas' large
    # diagonal red/blue split from reading as broken bodywork when mapped onto
    # a small Dreamcast-era polygon.  It carries a smoked lens, white DRL and
    # cold projector highlight as one coherent lamp assembly.
    draw.rectangle((64, 0, 127, 63), fill=(3, 7, 17))
    draw.polygon(((68, 55), (72, 18), (91, 7), (116, 10), (124, 27),
                  (115, 53)), fill=(8, 18, 38))
    draw.line(((70, 53), (76, 21), (93, 11), (115, 14), (121, 27)),
              fill=(176, 207, 229), width=3)
    draw.line(((73, 50), (80, 25), (95, 16)), fill=(62, 112, 162), width=1)
    draw.ellipse((93, 21, 114, 43), fill=(8, 21, 39),
                 outline=(142, 179, 202), width=2)
    draw.ellipse((100, 28, 108, 36), fill=(2, 7, 15))
    draw.ellipse((101, 27, 104, 30), fill=(201, 230, 244))
    draw.polygon(((80, 48), (112, 48), (116, 52), (78, 53)),
                 fill=(91, 139, 174))
    return result


def quantize(value: int, levels: int, x: int, y: int) -> int:
    normalized = value * levels / 255.0
    threshold = (BAYER_4X4[y & 3][x & 3] - 7.5) / 16.0
    return max(0, min(levels, int(normalized + 0.5 + threshold)))


def pack_rgb565(image: Image.Image) -> list[int]:
    rgb = image.convert("RGB")
    return [
        (quantize(r, 31, x, y) << 11)
        | (quantize(g, 63, x, y) << 5)
        | quantize(b, 31, x, y)
        for y in range(rgb.height)
        for x in range(rgb.width)
        for r, g, b in (rgb.getpixel((x, y)),)
    ]


def c_array(name: str, values: list[int]) -> str:
    rows = []
    for offset in range(0, len(values), 12):
        rows.append("    " + ", ".join(f"0x{v:04x}" for v in values[offset:offset + 12]) + ",")
    return (
        f"static const uint16_t pixels_{name}[{len(values)}] "
        f"__attribute__((aligned(32))) = {{\n" + "\n".join(rows) + "\n};\n"
    )


def build(source_dir: Path, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    previews = output_dir / "previews"
    previews.mkdir(exist_ok=True)
    built: list[tuple[TextureSpec, list[int]]] = []

    for spec in SPECS:
        with Image.open(source_dir / spec.source) as atlas:
            source = atlas.convert("RGB")
            texture = crop_quadrant(source, spec.quadrant) if spec.quadrant else source
            size = (spec.size, spec.size) if isinstance(spec.size, int) else spec.size
            texture = ImageOps.fit(
                texture, size, Image.Resampling.LANCZOS, centering=(0.5, 0.48)
            )
            if spec.name == "car_paint":
                texture = make_pearl_white(texture)
            else:
                texture = ImageEnhance.Contrast(texture).enhance(spec.contrast)
                texture = ImageEnhance.Brightness(texture).enhance(spec.brightness)
                if spec.name == "car_lights":
                    texture = make_vehicle_lights(texture)
            if spec.tileable:
                texture = make_tileable(texture)
            texture.save(previews / f"{spec.name}.png", optimize=True)
            built.append((spec, pack_rgb565(texture)))

    header = """/* Generated by tools/build_textures.py. Do not edit. */
#ifndef DRIFT_LA_TEXTURE_ASSETS_H
#define DRIFT_LA_TEXTURE_ASSETS_H

#include <stdint.h>

typedef struct {
    const uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    uint32_t byte_size;
} dla_texture_asset_t;

typedef enum {
"""
    for index, (spec, _) in enumerate(built):
        header += f"    DLA_TEX_{spec.name.upper()} = {index},\n"
    header += f"    DLA_TEXTURE_COUNT = {len(built)}\n}} dla_texture_id_t;\n\n"
    header += "extern const dla_texture_asset_t dla_texture_assets[DLA_TEXTURE_COUNT];\n\n#endif\n"

    source = '/* Generated by tools/build_textures.py. Do not edit. */\n#include "texture_assets.h"\n\n'
    for spec, values in built:
        source += c_array(spec.name, values) + "\n"
    source += "const dla_texture_asset_t dla_texture_assets[DLA_TEXTURE_COUNT] = {\n"
    for spec, values in built:
        size = (spec.size, spec.size) if isinstance(spec.size, int) else spec.size
        source += f"    {{ pixels_{spec.name}, {size[0]}, {size[1]}, {len(values) * 2}u }},\n"
    source += "};\n"

    (output_dir / "texture_assets.h").write_text(header, encoding="utf-8")
    (output_dir / "texture_assets.c").write_text(source, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    build(args.source_dir.resolve(), args.output_dir.resolve())


if __name__ == "__main__":
    main()
