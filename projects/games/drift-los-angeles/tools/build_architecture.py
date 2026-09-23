#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Extract architectural modules from the project's original source paintings.

Upper walls contain only complete window floors; storefronts and cornices have
separate UV domains so entrances and roof lines never repeat up a tower.  Crop
bounds follow the source's masonry/mullion bands.  No image-generation step,
blurred seam crossfade, or new artwork is needed to reproduce these assets.

Run: uv run tools/build_architecture.py
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw


PROJECT_DIR = Path(__file__).resolve().parents[1]
DISTRICTS = ("downtown", "coast", "arts", "neon")


@dataclass(frozen=True)
class Module:
    atlas: str
    quadrant: tuple[int, int]
    # Bounds are in the source quadrant's 627x627 coordinate system.
    box: tuple[int, int, int, int]
    floors: int = 0
    copies_y: int = 1
    floor_rows: tuple[tuple[int, int], ...] = ()


UPPER = {
    # Four complete office floors; cut on steel mullions, away from lit rooms.
    ("downtown", "base"): Module("facade-atlas-v3.png", (0, 0), (46, 163, 588, 472), 4),
    ("downtown", "alt"): Module("facade-variants-v5.png", (0, 0), (5, 6, 622, 347), 3),
    # Three complete residential floors; neither roof silhouette nor entrance.
    ("coast", "base"): Module("facade-atlas-v3.png", (1, 0), (5, 194, 622, 543), 3),
    ("coast", "alt"): Module("facade-variants-v5.png", (1, 0), (5, 8, 622, 138), 1, 3),
    ("arts", "base"): Module("facade-atlas-v3.png", (0, 1), (5, 42, 622, 541), 3),
    ("arts", "alt"): Module("facade-variants-v5.png", (0, 1), (5, 8, 622, 266), 3,
                             floor_rows=((8, 135), (137, 265), (8, 135))),
    # Duplicate the two-window-floor module vertically.  Marquees stay below.
    ("neon", "base"): Module("facade-atlas-v3.png", (1, 1), (5, 94, 622, 354), 2, 2),
    ("neon", "alt"): Module("facade-variants-v5.png", (1, 1), (5, 9, 622, 280), 2, 2),
}

STOREFRONTS = {
    ("downtown", "base"): Module("night-city-detail-atlas.png", (0, 0), (5, 135, 622, 553)),
    ("downtown", "alt"): Module("facade-variants-v5.png", (0, 0), (5, 384, 622, 614)),
    ("coast", "base"): Module("district-material-atlas.png", (0, 0), (5, 345, 622, 592)),
    ("coast", "alt"): Module("facade-variants-v5.png", (1, 0), (5, 405, 622, 614)),
    ("arts", "base"): Module("district-material-atlas.png", (0, 1), (5, 220, 622, 606)),
    ("arts", "alt"): Module("facade-variants-v5.png", (0, 1), (5, 425, 622, 615)),
    ("neon", "base"): Module("night-city-detail-atlas.png", (1, 1), (5, 315, 622, 613)),
    ("neon", "alt"): Module("facade-variants-v5.png", (1, 1), (5, 421, 622, 615)),
}

CORNICES = {
    "downtown": Module("facade-atlas-v3.png", (0, 0), (46, 159, 588, 174)),
    "coast": Module("facade-atlas-v3.png", (1, 0), (5, 534, 622, 552)),
    "arts": Module("facade-atlas-v3.png", (0, 1), (5, 35, 622, 64)),
    "neon": Module("facade-atlas-v3.png", (1, 1), (5, 183, 622, 202)),
}


def read_crop(source_dir: Path, module: Module) -> Image.Image:
    with Image.open(source_dir / module.atlas) as source:
        source = source.convert("RGB")
        cell_w, cell_h = source.width // 2, source.height // 2
        qx, qy = module.quadrant
        left, top, right, bottom = module.box
        return source.crop((round((qx + left / 627) * cell_w),
                            round((qy + top / 627) * cell_h),
                            round((qx + right / 627) * cell_w),
                            round((qy + bottom / 627) * cell_h)))


def match_gutter(image: Image.Image, *, horizontal: bool, vertical: bool) -> Image.Image:
    """Match only the outer texel, avoiding the old wide ghosted-edge blends.

    Crops land on architectural bands.  A shared boundary texel suppresses the
    remaining paint/color jump while keeping all window and mullion edges sharp.
    """
    image = image.copy()
    pixels = image.load()
    if horizontal:
        for y in range(image.height):
            average = tuple((a + b) // 2 for a, b in zip(pixels[0, y], pixels[image.width - 1, y]))
            pixels[0, y] = pixels[image.width - 1, y] = average
    if vertical:
        for x in range(image.width):
            average = tuple((a + b) // 2 for a, b in zip(pixels[x, 0], pixels[x, image.height - 1]))
            pixels[x, 0] = pixels[x, image.height - 1] = average
    return image


def make_upper(source_dir: Path, module: Module) -> Image.Image:
    if module.floor_rows:
        # The Arts painting's third floor has a tall white-framed aperture that
        # reads as an entrance at 480p.  Reassemble three unmistakable window
        # rows instead, retaining both of the source's upper-floor variants.
        result = Image.new("RGB", (512, 512))
        left, _top, right, _bottom = module.box
        for index, (top, bottom) in enumerate(module.floor_rows):
            row = Module(module.atlas, module.quadrant, (left, top, right, bottom))
            y0 = round(index * 512 / len(module.floor_rows))
            y1 = round((index + 1) * 512 / len(module.floor_rows))
            panel = read_crop(source_dir, row).resize((512, y1 - y0), Image.Resampling.LANCZOS)
            panel = match_gutter(panel, horizontal=True, vertical=True)
            result.paste(panel, (0, y0))
        return match_gutter(result, horizontal=True, vertical=True)
    crop = read_crop(source_dir, module)
    panel = match_gutter(crop, horizontal=True, vertical=True)
    result = Image.new("RGB", (panel.width, panel.height * module.copies_y))
    for copy in range(module.copies_y):
        result.paste(panel, (0, copy * panel.height))
    result = result.resize((512, 512), Image.Resampling.LANCZOS)
    return match_gutter(result, horizontal=True, vertical=True)


def build(source_dir: Path, output_dir: Path, preview: Path | None) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    preview_rows: list[tuple[str, Image.Image, Image.Image]] = []
    for district in DISTRICTS:
        for variant in ("base", "alt"):
            key = district, variant
            upper = make_upper(source_dir, UPPER[key])
            storefront = read_crop(source_dir, STOREFRONTS[key]).resize((512, 256), Image.Resampling.LANCZOS)
            storefront = match_gutter(storefront, horizontal=True, vertical=False)
            for kind, image in (("upper", upper), ("storefront", storefront)):
                image.save(output_dir / f"facade-{kind}-{district}-{variant}.png", optimize=True)
            preview_rows.append((f"{district} / {variant}: {UPPER[key].floors * UPPER[key].copies_y} floors per 12m", upper, storefront))
        cornice = read_crop(source_dir, CORNICES[district]).resize((512, 32), Image.Resampling.LANCZOS)
        cornice = match_gutter(cornice, horizontal=True, vertical=False)
        cornice.save(output_dir / f"facade-cornice-{district}.png", optimize=True)
    if preview:
        preview.parent.mkdir(parents=True, exist_ok=True)
        sheet = Image.new("RGB", (1024, len(preview_rows) * 224), (12, 17, 27))
        draw = ImageDraw.Draw(sheet)
        for index, (label, upper, storefront) in enumerate(preview_rows):
            y = index * 224
            draw.text((8, y + 4), label, fill=(230, 234, 242))
            # Two adjacent copies make module boundaries obvious during review.
            tile = upper.resize((192, 192), Image.Resampling.LANCZOS)
            sheet.paste(tile, (8, y + 24))
            sheet.paste(tile, (200, y + 24))
            sheet.paste(storefront.resize((384, 192), Image.Resampling.LANCZOS), (416, y + 24))
        sheet.save(preview, optimize=True)
    print(f"Built {len(UPPER)} upper walls, {len(STOREFRONTS)} storefronts and {len(CORNICES)} cornices in {output_dir}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=PROJECT_DIR / "assets" / "source")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    build(args.source_dir, args.output_dir or args.source_dir, args.preview)


if __name__ == "__main__":
    main()
