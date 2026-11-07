"""Pack uncompressed 16-bit PowerVR textures for direct ``pvr_txr_load``.

The result is a list of uint16 words, already twiddled and padded to a 32-byte
transfer length. Emit it as an aligned(32) C uint16_t array; SH-4's little-endian
storage is the expected byte order. The list itself has no memory-alignment
guarantee. Allocate/copy ``len(words) * 2`` bytes, set PVR_TXRFMT_TWIDDLED and the
matching RGB565/ARGB4444 format, and set context.txr.mipmap for mipmapped data.
Do not pass the result through pvr_txr_load_ex, which would twiddle it again.

Layout follows the checked-out KOS utils/pvrtex/pvr_texture.c MipMapOffset and
MakeTwiddled16, and pvr_texture_encoder.c pteCombineABGRData: three leading
copies of the 1x1 texel, then levels 1x1, 2x2, ..., full size. Within each level,
y occupies the low Morton bit, x the next bit. Rectangular non-mip textures
continue along the longer axis after interleaving the shared coordinate bits.
Flycast 2.7 core/rend/TexCache.cpp OtherMipPoint and texconv.cpp twiddle_slow
independently agree with this layout. Padding is never inserted between levels.
"""

from __future__ import annotations

from PIL import Image


_FORMATS = {"RGB565", "ARGB4444"}
_BAYER = ((0, 8, 2, 10), (12, 4, 14, 6),
          (3, 11, 1, 9), (15, 7, 13, 5))
_SRGB_TO_LINEAR = tuple(
    (v / 255.0 / 12.92) if v <= 10 else ((v / 255.0 + 0.055) / 1.055) ** 2.4
    for v in range(256)
)


def _pixels(image: Image.Image):
    # get_flattened_data was introduced after Pillow10, our minimum version.
    return image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()


def _check_size(width: int, height: int, mipmap: bool) -> None:
    for dimension in (width, height):
        if not isinstance(dimension, int) or not 8 <= dimension <= 1024 or dimension & (dimension - 1):
            raise ValueError("PowerVR dimensions must be powers of two from 8 to 1024")
    if mipmap and width != height:
        raise ValueError("PowerVR mipmapped textures must be square")


def mip_level_offset(size: int) -> int:
    """Byte offset of a square level (1..1024) within a 16-bit mip chain."""
    if not isinstance(size, int) or not 1 <= size <= 1024 or size & (size - 1):
        raise ValueError("Mip level size must be a power of two from 1 to 1024")
    return 2 * (3 + (size * size - 1) // 3)


def texture_byte_size(width: int, height: int, *, mipmap: bool = False) -> int:
    """Exact allocation/upload size, including required leading/tail padding."""
    _check_size(width, height, mipmap)
    payload = mip_level_offset(width) + width * width * 2 if mipmap else width * height * 2
    return (payload + 31) & ~31


def _spread_bits(value: int) -> int:
    result = 0
    for bit in range(value.bit_length()):
        result |= ((value >> bit) & 1) << (bit * 2)
    return result


def _twiddle(words: list[int], width: int, height: int) -> list[int]:
    edge = min(width, height)
    mask = edge - 1
    spread = [_spread_bits(i) for i in range(edge)]
    result = [0] * (width * height)
    for y in range(height):
        for x in range(width):
            # Equivalent to KOS TWIDOUT(x & mask, y & mask) plus square block.
            block = ((x // edge) + (y // edge)) * edge * edge
            offset = block + (spread[x & mask] << 1) + spread[y & mask]
            result[offset] = words[y * width + x]
    return result


def _quantize(value: int, maximum: int, x: int, y: int, dither: bool) -> int:
    noise = (_BAYER[y & 3][x & 3] - 7.5) / 16.0 if dither else 0.0
    return max(0, min(maximum, int(value * maximum / 255.0 + 0.5 + noise)))


def _pack_level(image: Image.Image, pixel_format: str, dither: bool) -> list[int]:
    rgba = image.convert("RGBA")
    words = []
    for index, (r, g, b, a) in enumerate(_pixels(rgba)):
        x, y = index % image.width, index // image.width
        if pixel_format == "RGB565":
            word = (_quantize(r, 31, x, y, dither) << 11
                    | _quantize(g, 63, x, y, dither) << 5
                    | _quantize(b, 31, x, y, dither))
        else:
            # Alpha is coverage; patterned alpha produces noisy particle edges.
            word = (_quantize(a, 15, x, y, False) << 12
                    | _quantize(r, 15, x, y, dither) << 8
                    | _quantize(g, 15, x, y, dither) << 4
                    | _quantize(b, 15, x, y, dither))
        words.append(word)
    return _twiddle(words, image.width, image.height)


def _pad_plane(plane: Image.Image, wrap: bool) -> Image.Image:
    """Extend by one image on each side for phase-correct area resampling."""
    width, height = plane.size
    result = Image.new("F", (width * 3, height * 3))
    if wrap:
        for y in range(3):
            for x in range(3):
                result.paste(plane, (x * width, y * height))
    else:
        result.paste(plane, (width, height))
        result.paste(plane.crop((0, 0, 1, height)).resize((width, height), Image.Resampling.NEAREST),
                     (0, height))
        result.paste(plane.crop((width - 1, 0, width, height)).resize((width, height), Image.Resampling.NEAREST),
                     (2 * width, height))
        result.paste(result.crop((0, height, width * 3, height + 1)).resize((width * 3, height), Image.Resampling.NEAREST),
                     (0, 0))
        result.paste(result.crop((0, 2 * height - 1, width * 3, 2 * height)).resize((width * 3, height), Image.Resampling.NEAREST),
                     (0, 2 * height))
    return result


def _linear_to_srgb(value: float) -> int:
    value = max(0.0, min(1.0, value))
    encoded = value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055
    return max(0, min(255, int(encoded * 255.0 + 0.5)))


def _mip_images(image: Image.Image, wrap: bool) -> list[Image.Image]:
    """Generate ascending levels with linear-light, premultiplied filtering.

    Area filtering suppresses distant facade/road noise without sharpening
    halos. Every level is derived from the full-resolution source. KOS's PVR
    phase correction is shift=-0.5 + level/base/2 output pixels, equivalent to
    a source-box offset of 0.5 - base/level/2. This keeps mip texel centers
    aligned with the hardware instead of making details crawl between levels.
    """
    base = image.convert("RGBA")
    pixels = list(_pixels(base))
    alpha = [pixel[3] / 255.0 for pixel in pixels]
    planes = []
    for channel in range(4):
        plane = Image.new("F", base.size)
        plane.putdata(alpha if channel == 3 else [
            _SRGB_TO_LINEAR[pixel[channel]] * a for pixel, a in zip(pixels, alpha)
        ])
        planes.append(_pad_plane(plane, wrap))
    levels = []
    size = 1
    while size < base.width:
        offset = base.width + 0.5 - base.width / size / 2.0
        box = (offset, offset, offset + base.width, offset + base.height)
        channels = [list(_pixels(plane.resize((size, size), Image.Resampling.BOX, box=box)))
                    for plane in planes]
        mip = Image.new("RGBA", (size, size))
        mip.putdata([
            (*(_linear_to_srgb(c / a) if a > 1.0e-8 else 0 for c in (r, g, b)),
             max(0, min(255, int(a * 255.0 + 0.5))))
            for r, g, b, a in zip(*channels)
        ])
        levels.append(mip)
        size *= 2
    levels.append(base)
    return levels


def pack_texture(image: Image.Image, pixel_format: str = "RGB565", *,
                 mipmap: bool = False, dither: bool = True,
                 wrap: bool = False) -> list[int]:
    """Return twiddled uint16 words including complete mips and DMA padding.

    Dimensions must be powers of two from 8 through 1024; mipmaps require a
    square image. ``wrap`` controls mip filtering at edges (use for tileable
    materials); it does not change runtime UV clamp flags. RGB565 ignores alpha.
    ARGB4444 stores straight alpha, with premultiplied filtering during mip
    generation to prevent invisible RGB from bleeding into particle edges.
    """
    pixel_format = pixel_format.upper()
    if pixel_format not in _FORMATS:
        raise ValueError(f"Unsupported PowerVR pixel format: {pixel_format}")
    _check_size(image.width, image.height, mipmap)
    # Opaque RGB565 must not let source alpha alter the filtering result.
    source = image.convert("RGB") if pixel_format == "RGB565" else image.convert("RGBA")
    levels = _mip_images(source, wrap) if mipmap else [source]
    words = []
    for level in levels:
        packed = _pack_level(level, pixel_format, dither)
        if mipmap and level.width == 1:
            words.extend([packed[0]] * 3)
        words.extend(packed)
    words.extend([0] * (texture_byte_size(image.width, image.height, mipmap=mipmap) // 2 - len(words)))
    return words
