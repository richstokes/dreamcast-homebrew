#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Build Gravity Wave's source atlases into PVR-ready 16-bit texture data."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image


@dataclass(frozen=True)
class TextureSpec:
    name: str
    source: str
    quadrant: tuple[int, int]
    size: int
    texture_format: str
    tileable: bool = False


SPECS = (
    TextureSpec("terrain_azure", "terrain-material-atlas.png", (0, 0), 128, "RGB565", True),
    TextureSpec("terrain_emerald", "terrain-material-atlas.png", (1, 0), 128, "RGB565", True),
    TextureSpec("terrain_violet", "terrain-material-atlas.png", (0, 1), 128, "RGB565", True),
    TextureSpec("terrain_ember", "terrain-material-atlas.png", (1, 1), 128, "RGB565", True),
    TextureSpec("hull_allied", "vehicle-material-atlas.png", (0, 0), 128, "RGB565", True),
    TextureSpec("hull_hostile", "vehicle-material-atlas.png", (1, 0), 128, "RGB565", True),
    TextureSpec("canopy_energy", "vehicle-material-atlas.png", (0, 1), 128, "RGB565", True),
    TextureSpec("ancient_machine", "vehicle-material-atlas.png", (1, 1), 128, "RGB565", True),
    TextureSpec("foliage", "billboard-sprite-atlas.png", (0, 0), 64, "ARGB4444"),
    TextureSpec("waterfall_mist", "billboard-sprite-atlas.png", (1, 0), 64, "ARGB4444"),
    TextureSpec("rift_energy", "billboard-sprite-atlas.png", (0, 1), 64, "ARGB4444"),
    TextureSpec("fire_smoke", "billboard-sprite-atlas.png", (1, 1), 64, "ARGB4444"),
)

BAYER_4X4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)


def crop_quadrant(image: Image.Image, quadrant: tuple[int, int]) -> Image.Image:
    """Crop one atlas quadrant while omitting its thin guide/seam line."""
    half_w = image.width // 2
    half_h = image.height // 2
    qx, qy = quadrant
    inset = max(2, min(image.width, image.height) // 256)
    left = qx * half_w + inset
    top = qy * half_h + inset
    right = (qx + 1) * half_w - inset
    bottom = (qy + 1) * half_h - inset
    return image.crop((left, top, right, bottom))


def make_tileable(image: Image.Image, feather: int = 12) -> Image.Image:
    """Cross-blend opposing edge texels so hardware wrapping has no hard seam."""
    result = image.copy()
    pixels = result.load()
    width, height = result.size
    channels = len(result.getbands())

    for y in range(height):
        original = [pixels[x, y] for x in range(width)]
        for distance in range(feather):
            amount = 1.0 - distance / float(feather)
            left = original[distance]
            right = original[width - 1 - distance]
            average = tuple((left[c] + right[c]) * 0.5 for c in range(channels))
            pixels[distance, y] = tuple(
                round(left[c] * (1.0 - amount) + average[c] * amount)
                for c in range(channels)
            )
            pixels[width - 1 - distance, y] = tuple(
                round(right[c] * (1.0 - amount) + average[c] * amount)
                for c in range(channels)
            )

    for x in range(width):
        original = [pixels[x, y] for y in range(height)]
        for distance in range(feather):
            amount = 1.0 - distance / float(feather)
            top = original[distance]
            bottom = original[height - 1 - distance]
            average = tuple((top[c] + bottom[c]) * 0.5 for c in range(channels))
            pixels[x, distance] = tuple(
                round(top[c] * (1.0 - amount) + average[c] * amount)
                for c in range(channels)
            )
            pixels[x, height - 1 - distance] = tuple(
                round(bottom[c] * (1.0 - amount) + average[c] * amount)
                for c in range(channels)
            )
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


def pack_argb4444(image: Image.Image) -> list[int]:
    rgba = image.convert("RGBA")
    packed: list[int] = []
    for y in range(rgba.height):
        for x in range(rgba.width):
            r, g, b, a = rgba.getpixel((x, y))
            if a < 10:
                a = 0
                r = g = b = 0
            packed.append(
                (quantize(a, 15, x, y) << 12)
                | (quantize(r, 15, x, y) << 8)
                | (quantize(g, 15, x, y) << 4)
                | quantize(b, 15, x, y)
            )
    return packed


def c_array(name: str, values: list[int]) -> str:
    lines = []
    for offset in range(0, len(values), 12):
        row = ", ".join(f"0x{value:04x}" for value in values[offset : offset + 12])
        lines.append(f"    {row},")
    return (
        f"static const uint16_t pixels_{name}[{len(values)}] "
        f"__attribute__((aligned(32))) = {{\n" + "\n".join(lines) + "\n};\n"
    )


def write_outputs(source_dir: Path, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    previews = output_dir / "previews"
    previews.mkdir(exist_ok=True)

    built: list[tuple[TextureSpec, list[int]]] = []
    for spec in SPECS:
        path = source_dir / spec.source
        with Image.open(path) as atlas:
            mode = "RGBA" if spec.texture_format == "ARGB4444" else "RGB"
            texture = crop_quadrant(atlas.convert(mode), spec.quadrant)
            texture = texture.resize((spec.size, spec.size), Image.Resampling.LANCZOS)
            if spec.tileable:
                texture = make_tileable(texture)
            texture.save(previews / f"{spec.name}.png", optimize=True)
            values = (
                pack_rgb565(texture)
                if spec.texture_format == "RGB565"
                else pack_argb4444(texture)
            )
            built.append((spec, values))

    header = """/* Generated by tools/build_textures.py. Do not edit by hand. */
#ifndef GRAVITY_WAVE_TEXTURE_ASSETS_H
#define GRAVITY_WAVE_TEXTURE_ASSETS_H

#include <stdint.h>

typedef enum {
    GRAVITY_WAVE_TEXTURE_RGB565 = 0,
    GRAVITY_WAVE_TEXTURE_ARGB4444 = 1
} gravity_wave_texture_format_t;

typedef struct {
    const uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    uint32_t byte_size;
    gravity_wave_texture_format_t format;
} gravity_wave_texture_asset_t;

typedef enum {
"""
    for index, (spec, _) in enumerate(built):
        header += f"    GRAVITY_WAVE_TEX_{spec.name.upper()} = {index},\n"
    header += f"    GRAVITY_WAVE_TEXTURE_COUNT = {len(built)}\n"
    header += "} gravity_wave_texture_id_t;\n\n"
    header += "extern const gravity_wave_texture_asset_t gravity_wave_texture_assets[GRAVITY_WAVE_TEXTURE_COUNT];\n\n"
    header += "#endif\n"

    source = """/* Generated by tools/build_textures.py. Do not edit by hand. */
#include "texture_assets.h"

"""
    for spec, values in built:
        source += c_array(spec.name, values) + "\n"
    source += "const gravity_wave_texture_asset_t gravity_wave_texture_assets[GRAVITY_WAVE_TEXTURE_COUNT] = {\n"
    for spec, values in built:
        source += (
            f"    {{ pixels_{spec.name}, {spec.size}, {spec.size}, "
            f"{len(values) * 2}u, GRAVITY_WAVE_TEXTURE_{spec.texture_format} }},\n"
        )
    source += "};\n"

    (output_dir / "texture_assets.h").write_text(header, encoding="utf-8")
    (output_dir / "texture_assets.c").write_text(source, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    write_outputs(args.source_dir.resolve(), args.output_dir.resolve())


if __name__ == "__main__":
    main()
