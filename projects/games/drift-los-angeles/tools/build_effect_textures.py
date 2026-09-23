#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Deterministic alpha art for Drift Los Angeles smoke, shadows, lights and palms.

Run ``uv run tools/build_effect_textures.py --output-dir assets/source``.
Smoke is a 2x2 atlas of independently shaped 128px white smoke wisps, with
transparent gutters. Shadow UV v follows the vehicle's length. The palm is a
single 128x256 frond: stem at bottom center, tip at top, for an alpha-tested
camera-independent world quad. The headlight ground cookie has its near edge
at v=1, far edge at v=0. These images are straight-alpha RGBA sources.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import random

from PIL import Image, ImageDraw, ImageFilter


def pixels(image: Image.Image):
    return image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()


def smoothstep(a: float, b: float, value: float) -> float:
    t = max(0.0, min(1.0, (value - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


def noise_plane(size: int, seed: int) -> list[float]:
    """Low-frequency turbulence, deliberately avoiding single-pixel grain."""
    rng = random.Random(seed)
    output = [0.0] * (size * size)
    for grid, weight in ((4, .50), (8, .29), (16, .15), (32, .06)):
        noise = Image.new("L", (grid, grid))
        noise.putdata([rng.randrange(256) for _ in range(grid * grid)])
        noise = noise.resize((size, size), Image.Resampling.BICUBIC)
        for i, value in enumerate(pixels(noise)):
            output[i] += weight * (value / 127.5 - 1.0)
    return output


def smoke_tile(seed: int, style: int) -> Image.Image:
    size = 256
    rng = random.Random(seed)
    noise = noise_plane(size, seed)
    warp = noise_plane(size, seed + 173)
    # Bent streams of broad lobes give each atlas cell a different silhouette.
    lobes = []
    for i in range(9):
        t = i / 8.0
        angle = -1.2 + t * 3.5 + style * .57
        radius = .10 + .35 * t
        x = math.cos(angle) * radius * (1.0 if style & 1 else .78)
        y = math.sin(angle) * radius * (.80 if style & 1 else 1.0)
        lobes.append((x + rng.uniform(-.12, .12), y + rng.uniform(-.12, .12),
                      rng.uniform(.15, .29), rng.uniform(.15, .25),
                      rng.uniform(.35, .65)))
    opacity = []
    for y in range(size):
        py = (y + .5) / size * 2.0 - 1.0
        for x in range(size):
            px = (x + .5) / size * 2.0 - 1.0
            index = y * size + x
            wx = px + noise[index] * .14
            wy = py + warp[index] * .13
            density = sum(weight * math.exp(-1.7 * (((wx - cx) / sx) ** 2 + ((wy - cy) / sy) ** 2))
                          for cx, cy, sx, sy, weight in lobes)
            # Smooth turbulent pockets inside the cloud, not a dark outline.
            turbulence = max(.20, .76 + noise[index] * .60 + warp[index] * .17)
            density = (1.0 - math.exp(-density * 1.6)) * turbulence
            radial_fade = 1.0 - smoothstep(.64, .94, math.hypot(px, py))
            gutter = smoothstep(.03, .13, 1.0 - max(abs(px), abs(py)))
            opacity.append(round(min(.80, density * radial_fade * gutter) * 255.0))
    alpha = Image.new("L", (size, size))
    alpha.putdata(opacity)
    alpha = alpha.filter(ImageFilter.GaussianBlur(1.1))
    result = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    result.putalpha(alpha)
    return result.resize((128, 128), Image.Resampling.LANCZOS)


def make_smoke() -> Image.Image:
    atlas = Image.new("RGBA", (256, 256), (255, 255, 255, 0))
    for i in range(4):
        atlas.paste(smoke_tile(17303 + i * 7919, i), ((i & 1) * 128, (i >> 1) * 128))
    # Preserve white RGB even in fully transparent texels: bilinear samples
    # must not acquire a dark halo where the sprite fades to zero alpha.
    atlas.putdata([(255, 255, 255, a) for _, _, _, a in pixels(atlas)])
    return atlas


def make_shadow() -> Image.Image:
    scale = 4
    size = 128 * scale
    shadow = Image.new("L", (size, size))
    draw = ImageDraw.Draw(shadow)
    def box(coords):
        return tuple(round(v * scale) for v in coords)
    draw.ellipse(box((27, 9, 101, 120)), fill=73)
    shadow = shadow.filter(ImageFilter.GaussianBlur(7 * scale))
    chassis = Image.new("L", (size, size))
    draw = ImageDraw.Draw(chassis)
    draw.rounded_rectangle(box((37, 24, 91, 105)), radius=14 * scale, fill=91)
    draw.ellipse(box((42, 32, 86, 100)), fill=113)
    chassis = chassis.filter(ImageFilter.GaussianBlur(4 * scale))
    from PIL import ImageChops
    shadow = ImageChops.screen(shadow, chassis)
    contacts = Image.new("L", (size, size))
    draw = ImageDraw.Draw(contacts)
    # Runtime quad is x+-1.45m, z+-3.20m; tires are x+-0.95m, z+-1.55m.
    for x in (128 * (.5 - .95 / 2.9), 128 * (.5 + .95 / 2.9)):
        for y in (128 * (.5 - 1.55 / 6.4), 128 * (.5 + 1.55 / 6.4)):
            draw.ellipse(box((x - 7.0, y - 12, x + 7.0, y + 12)), fill=131)
            draw.ellipse(box((x - 3.5, y - 8, x + 3.5, y + 8)), fill=155)
    contacts = contacts.filter(ImageFilter.GaussianBlur(2.1 * scale))
    shadow = ImageChops.screen(shadow, contacts)
    shadow = shadow.resize((128, 128), Image.Resampling.LANCZOS)
    # An explicit soft perimeter prevents clipped rectangles on bright paving.
    shadow.putdata([
        round(a * smoothstep(1.0, 11.0, min(x, y, 127 - x, 127 - y)))
        for y in range(128) for x in range(128)
        for a in (shadow.getpixel((x, y)),)
    ])
    result = Image.new("RGBA", (128, 128), (4, 7, 12, 0))
    result.putalpha(shadow)
    return result


def bezier(p0, p1, p2, t):
    return ((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
            (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1])


def make_palm() -> Image.Image:
    scale = 4
    image = Image.new("RGBA", (128 * scale, 256 * scale), (42, 77, 43, 0))
    draw = ImageDraw.Draw(image)
    rng = random.Random(41791)
    def rib(t):
        return bezier((64, 245), (75, 118), (59, 8), t)
    def points(coords):
        return [(round(x * scale), round(y * scale)) for x, y in coords]
    # Narrow curved leaflets alternate slightly down the rachis, with varied
    # tips and twists. Their opaque interiors survive a 0.5 alpha-test cutoff.
    for i in range(32):
        t = .105 + i * .0266
        for side in (-1, 1):
            leaf_t = t + (.008 if side > 0 else 0)
            root = rib(leaf_t)
            length = (9 + 44 * math.sin(math.pi * leaf_t) ** .72) * rng.uniform(.88, 1.07)
            tip = (max(5, min(123, root[0] + side * length)),
                   root[1] + 8 + length * (.36 - leaf_t * .22) + rng.uniform(-4, 4))
            bend = (root[0] + side * length * .67,
                    root[1] - 5 + length * .025)
            center = [bezier(root, bend, tip, j / 8) for j in range(9)]
            width = (1.2 + 1.9 * math.sin(math.pi * leaf_t)) * rng.uniform(.85, 1.08)
            left, right = [], []
            for j, (x, y) in enumerate(center):
                spread = width * math.sin(math.pi * (j / 8) ** .70) * .55
                left.append((x, y - spread))
                right.append((x, y + spread))
            lit = rng.uniform(.82, 1.13)
            base = (69, 102, 50) if side < 0 else (40, 78, 44)
            color = (*[min(255, round(c * lit)) for c in base], 255)
            draw.polygon(points(left + right[::-1]), fill=color)
            highlight = (round(96 * lit), round(123 * lit), round(61 * lit), 255)
            draw.line(points(center[:7]), fill=highlight if side < 0 else (62, 94, 49, 255),
                      width=scale // 2)
    stalk = [rib(i / 80) for i in range(81)]
    for i in range(80):
        width = max(1, round((2.7 - i / 80 * 2.0) * scale))
        draw.line(points(stalk[i:i + 2]), fill=(93, 112, 58, 255), width=width)
    draw.line(points([(x - .5, y) for x, y in stalk]), fill=(134, 141, 73, 255), width=scale // 2)
    return image.resize((128, 256), Image.Resampling.LANCZOS)


def make_light() -> Image.Image:
    """Soft paired headlamp spill, mapped onto a tapered ground quad.

    Keep the illumination smooth and low-frequency: directional falloff is
    supplied by both this cookie and the near-to-far widening of the quad.
    All four borders are transparent, so neither triangle exposes an edge.
    """
    size = 128
    peak_v = 1.5 / 1.9
    normalization = peak_v ** 1.5 * (1.0 - peak_v) ** .4
    opacity = []
    for y in range(size):
        v = y / (size - 1)
        longitudinal = v ** 1.5 * (1.0 - v) ** .4 / normalization
        longitudinal *= smoothstep(0.0, .10, v) * (1.0 - smoothstep(.90, 1.0, v))
        for x in range(size):
            u = x / (size - 1) * 2.0 - 1.0
            # Two barely distinct near pools merge into one broad far beam.
            separation = .30 * smoothstep(.34, .83, v)
            beam = (math.exp(-3.9 * (u - separation) ** 2)
                    + math.exp(-3.9 * (u + separation) ** 2)) * .5
            beam /= math.exp(-3.9 * separation ** 2)
            lateral = beam * (1.0 - smoothstep(.66, 1.0, abs(u)))
            opacity.append(round(224 * longitudinal * lateral))
    # Normalize the analytic field without changing its smooth falloff.
    maximum = max(opacity)
    opacity = [round(a * 224 / maximum) for a in opacity]
    alpha = Image.new("L", (size, size))
    alpha.putdata(opacity)
    result = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    result.putalpha(alpha)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path("assets/source"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for filename, image in (("effect-smoke.png", make_smoke()),
                            ("effect-shadow.png", make_shadow()),
                            ("effect-palm.png", make_palm()),
                            ("effect-light.png", make_light())):
        path = args.output_dir / filename
        image.save(path, optimize=True)
        alpha = image.getchannel("A")
        print(f"{path}: {image.width}x{image.height} RGBA, alpha {alpha.getextrema()}")


if __name__ == "__main__":
    main()
