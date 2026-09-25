#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Convert Drift Los Angeles's source atlases to PVR-ready RGB565/ARGB4444 C arrays."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageOps, ImageFilter
from pvr_texture import pack_texture


@dataclass(frozen=True)
class TextureSpec:
    name: str
    source: str
    quadrant: tuple[int, int] | None
    size: int | tuple[int, int]
    tileable: bool = False
    contrast: float = 1.0
    brightness: float = 1.0
    alpha: bool = False
    mipmap: bool = False


SPECS = (
    TextureSpec("asphalt", "street-surface-atlas-v3.png", (0, 0), 256, True, .70, 1.02, mipmap=True),
    TextureSpec("sidewalk", "street-surface-atlas-v3.png", (1, 0), 128, True, 1.10, 1.08, mipmap=True),
    TextureSpec("pavers", "street-surface-atlas-v3.png", (0, 1), 128, True, 1.10, 1.08, mipmap=True),
    TextureSpec("road_marks", "street-surface-atlas-v3.png", (1, 1), 128, False, 1.12, 1.08),
    TextureSpec("stucco", "city-material-atlas.png", (1, 0), 128, True, 1.02, mipmap=True),
    TextureSpec("glass_facade", "city-material-atlas.png", (0, 1), 128, True, 1.12, mipmap=True),
    TextureSpec("graffiti", "city-material-atlas.png", (1, 1), 128, True, 1.10),
    TextureSpec("car_paint", "vehicle-material-atlas.png", (0, 0), 128, True, 1.08, 1.52),
    TextureSpec("car_glass", "vehicle-material-atlas.png", (1, 0), 128, True, 1.10),
    TextureSpec("car_carbon", "vehicle-material-atlas.png", (0, 1), 128, True, 1.08),
    TextureSpec("car_lights", "vehicle-material-atlas.png", (1, 1), 128, False, 1.12),
    TextureSpec("car_reflection", "skyline-backdrop-v4.png", None, 128),
    TextureSpec("storefront", "night-city-detail-atlas.png", (0, 0), 128, False, 1.12),
    TextureSpec("billboard", "night-city-detail-atlas.png", (1, 0), 128, False, 1.10),
    TextureSpec("lit_windows", "night-city-detail-atlas.png", (0, 1), 128, True, 1.14, mipmap=True),
    TextureSpec("neon_facade", "night-city-detail-atlas.png", (1, 1), 128, False, 1.12),
    TextureSpec("district_coast", "district-material-atlas.png", (0, 0), 128, False, 1.10),
    TextureSpec("district_downtown", "district-material-atlas.png", (1, 0), 128, False, 1.12),
    TextureSpec("district_arts", "district-material-atlas.png", (0, 1), 128, False, 1.12),
    TextureSpec("district_neon", "district-material-atlas.png", (1, 1), 128, False, 1.14),
    TextureSpec("facade_downtown", "facade-upper-downtown-base.png", None, 256, True, .95, mipmap=True),
    TextureSpec("facade_coast", "facade-upper-coast-base.png", None, 256, True, .95, mipmap=True),
    TextureSpec("facade_arts", "facade-upper-arts-base.png", None, 256, True, .95, mipmap=True),
    TextureSpec("facade_neon", "facade-upper-neon-base.png", None, 256, True, .95, mipmap=True),
    TextureSpec("facade_downtown_alt", "facade-upper-downtown-alt.png", None, 256, True, .95, mipmap=True),
    TextureSpec("facade_coast_alt", "facade-upper-coast-alt.png", None, 256, True, .95, mipmap=True),
    TextureSpec("facade_arts_alt", "facade-upper-arts-alt.png", None, 256, True, .95, mipmap=True),
    TextureSpec("facade_neon_alt", "facade-upper-neon-alt.png", None, 256, True, .95, mipmap=True),
    TextureSpec("street_utility", "street-microdetail-v5.png", (0, 0), 128, False, 1.12, 1.06),
    TextureSpec("street_repair", "street-microdetail-v5.png", (1, 0), 128, False, 1.10, 1.04),
    TextureSpec("storefront_micro", "street-microdetail-v5.png", (0, 1), 128, False, 1.10, 1.06),
    TextureSpec("civic_micro", "street-microdetail-v5.png", (1, 1), 128, False, 1.10, 1.06),
    TextureSpec("facade_storefront_downtown", "facade-storefront-downtown-base.png", None, (256, 128)),
    TextureSpec("facade_storefront_coast", "facade-storefront-coast-base.png", None, (256, 128)),
    TextureSpec("facade_storefront_arts", "facade-storefront-arts-base.png", None, (256, 128)),
    TextureSpec("facade_storefront_neon", "facade-storefront-neon-base.png", None, (256, 128)),
    TextureSpec("facade_storefront_downtown_alt", "facade-storefront-downtown-alt.png", None, (256, 128)),
    TextureSpec("facade_storefront_coast_alt", "facade-storefront-coast-alt.png", None, (256, 128)),
    TextureSpec("facade_storefront_arts_alt", "facade-storefront-arts-alt.png", None, (256, 128)),
    TextureSpec("facade_storefront_neon_alt", "facade-storefront-neon-alt.png", None, (256, 128)),
    TextureSpec("facade_cornice_downtown", "facade-cornice-downtown.png", None, (256, 16)),
    TextureSpec("facade_cornice_coast", "facade-cornice-coast.png", None, (256, 16)),
    TextureSpec("facade_cornice_arts", "facade-cornice-arts.png", None, (256, 16)),
    TextureSpec("facade_cornice_neon", "facade-cornice-neon.png", None, (256, 16)),
    TextureSpec("sky_backdrop", "skyline-backdrop-v4.png", None, (512, 256), False, 1.08, 1.06),
    TextureSpec("effect_light", "effect-light.png", None, 128, alpha=True),
    TextureSpec("effect_palm", "effect-palm.png", None, 128, alpha=True, mipmap=True),
    TextureSpec("effect_shadow", "effect-shadow.png", None, 128, alpha=True),
    TextureSpec("effect_smoke", "effect-smoke.png", None, 256, alpha=True),
    TextureSpec("pedestrian", "pedestrian-atlas.png", None, 256, False, 1.0, 1.0, mipmap=True),
    TextureSpec("vehicle", "vehicle-atlas.png", None, 256, False, 1.0, 1.0, mipmap=True),
    TextureSpec("prop", "prop-atlas.png", None, 256, False, 1.0, 1.0, mipmap=True),
    TextureSpec("title_art", "title-key-art-v2.png", None, (512, 256), False, 1.06),
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
        pearl = 180 + value * 38 // 255
        pixels.append((min(255, pearl + 5), min(255, pearl + 6),
                       min(255, pearl + 10)))
    result = Image.new("RGB", image.size)
    result.putdata(pixels)
    return result


def make_reflection(image: Image.Image) -> Image.Image:
    """Broad environment features, sampled by reflected world-space normals."""
    sky = ImageOps.fit(image, (128, 76)).filter(ImageFilter.GaussianBlur(3))
    result = Image.new("RGB", (128, 128), (20, 24, 34))
    result.paste(sky, (0, 0))
    draw = ImageDraw.Draw(result)
    for y in range(76, 128):
        t = (y - 76) / 52
        draw.line((0, y, 127, y), fill=(int(47-29*t), int(43-23*t), int(60-30*t)))
    # Reflected windows form broad ribbons, never baked into the base glass.
    for x in (9, 30, 67, 105):
        draw.rectangle((x, 48, x+3, 77), fill=(123, 139, 166))
        draw.rectangle((x+4, 64, x+11, 70), fill=(156, 108, 74))
    return ImageEnhance.Color(result.filter(ImageFilter.GaussianBlur(2))).enhance(.45)


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
            source = atlas.convert("RGBA" if spec.alpha else "RGB")
            texture = crop_quadrant(source, spec.quadrant) if spec.quadrant else source
            size = (spec.size, spec.size) if isinstance(spec.size, int) else spec.size
            texture = (texture.resize(size, Image.Resampling.LANCZOS)
                       if spec.name == "effect_palm" else ImageOps.fit(
                           texture, size, Image.Resampling.LANCZOS, centering=(0.5, 0.48)))
            if spec.name == "car_paint":
                texture = make_pearl_white(texture)
            elif spec.name == "car_glass":
                texture = Image.new("RGB", size, (34, 47, 64))
            elif spec.name == "car_reflection":
                texture = make_reflection(texture)
            else:
                texture = ImageEnhance.Contrast(texture).enhance(spec.contrast)
                texture = ImageEnhance.Brightness(texture).enhance(spec.brightness)
                if spec.name == "car_lights":
                    texture = make_vehicle_lights(texture)
            if spec.tileable and not spec.name.startswith("facade_"):
                texture = make_tileable(texture)
            texture.save(previews / f"{spec.name}.png", optimize=True)
            built.append((spec, pack_texture(texture, "ARGB4444" if spec.alpha else "RGB565",
                mipmap=spec.mipmap, wrap=spec.tileable)))

    header = """/* Generated by tools/build_textures.py. Do not edit. */
#ifndef DRIFT_LA_TEXTURE_ASSETS_H
#define DRIFT_LA_TEXTURE_ASSETS_H

#include <stdint.h>

typedef struct {
    const uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    uint32_t byte_size;
    uint8_t alpha;
    uint8_t mipmap;
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
        source += f"    {{ pixels_{spec.name}, {size[0]}, {size[1]}, {len(values) * 2}u, {int(spec.alpha)}, {int(spec.mipmap)} }},\n"
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
