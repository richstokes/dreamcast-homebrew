#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Hardware-layout regression tests: uv run tools/test_pvr_texture.py."""

import unittest

from PIL import Image

from pvr_texture import mip_level_offset, pack_texture, texture_byte_size


# Source row-major indices in KOS/Flycast y-low, x-high Morton order for 8x8.
# Keeping this fixture explicit catches transposition and inverted axis order.
MORTON_8 = (
    0, 8, 1, 9, 16, 24, 17, 25, 2, 10, 3, 11, 18, 26, 19, 27,
    32, 40, 33, 41, 48, 56, 49, 57, 34, 42, 35, 43, 50, 58, 51, 59,
    4, 12, 5, 13, 20, 28, 21, 29, 6, 14, 7, 15, 22, 30, 23, 31,
    36, 44, 37, 45, 52, 60, 53, 61, 38, 46, 39, 47, 54, 62, 55, 63,
)


class PowerVRTextureTests(unittest.TestCase):
    def test_hardware_mip_offsets(self):
        # KOS MipMapOffset table, also Flycast OtherMipPoint * sizeof(uint16).
        offsets = (0x6, 0x8, 0x10, 0x30, 0xB0, 0x2B0,
                   0xAB0, 0x2AB0, 0xAAB0, 0x2AAB0, 0xAAAB0)
        self.assertEqual(tuple(mip_level_offset(1 << i) for i in range(11)), offsets)
        self.assertEqual(texture_byte_size(256, 256, mipmap=True), 174784)
        self.assertEqual(texture_byte_size(512, 256), 262144)

    def test_rgb565_bit_fields_and_morton_order(self):
        image = Image.new("RGB", (8, 8))
        image.putdata([(0, round(i * 255 / 63), 0) for i in range(64)])
        self.assertEqual(pack_texture(image, dither=False), [i << 5 for i in MORTON_8])
        for color, expected in (((255, 0, 0), 0xF800), ((0, 255, 0), 0x07E0),
                                ((0, 0, 255), 0x001F), ((255, 255, 255), 0xFFFF)):
            self.assertEqual(set(pack_texture(Image.new("RGB", (8, 8), color))), {expected})

    def test_rectangular_twiddling_continues_in_square_blocks(self):
        image = Image.new("RGB", (16, 8))
        for y in range(8):
            for x in range(16):
                image.putpixel((x, y), (255 if x >= 8 else 0,
                                       round((y * 8 + x % 8) * 255 / 63), 0))
        self.assertEqual(pack_texture(image, dither=False),
                         [i << 5 for i in MORTON_8] + [0xF800 | i << 5 for i in MORTON_8])
        tall = Image.new("RGB", (8, 16))
        tall.paste(image.crop((0, 0, 8, 8)), (0, 0))
        tall.paste(image.crop((8, 0, 16, 8)), (0, 8))
        self.assertEqual(pack_texture(tall, dither=False), pack_texture(image, dither=False))

    def test_argb4444_channels_and_undithered_alpha(self):
        image = Image.new("RGBA", (8, 8), (17, 34, 51, 136))
        self.assertEqual(set(pack_texture(image, "ARGB4444", dither=False)), {0x8123})
        self.assertEqual({word >> 12 for word in pack_texture(image, "ARGB4444")}, {8})

    def test_mip_payload_has_leading_texels_and_tail_only_padding(self):
        image = Image.new("RGB", (8, 8), (255, 0, 0))
        words = pack_texture(image, mipmap=True, dither=False)
        # Three pad texels + 1 + 4 + 16 + 64 pixels = 88 words, rounded to96.
        self.assertEqual(len(words), 96)
        self.assertEqual(words[:88], [0xF800] * 88)
        self.assertEqual(words[88:], [0] * 8)
        # Largest level is unmodified, at the hardware's byte offset48.
        self.assertEqual(words[24:88], pack_texture(image, dither=False))

    def test_mips_filter_in_linear_light_and_preserve_alpha_color(self):
        checker = Image.new("RGB", (8, 8))
        checker.putdata([(255, 255, 255) if (x + y) & 1 else (0, 0, 0)
                         for y in range(8) for x in range(8)])
        words = pack_texture(checker, mipmap=True, wrap=True, dither=False)
        # The 1x1 average is sRGB188, not gamma-space128 (roughly RGB565 0x8410).
        self.assertEqual(words[3], 0xBDD7)
        alpha = Image.new("RGBA", (8, 8))
        alpha.putdata([(255, 0, 0, 0) if (x + y) & 1 else (0, 0, 255, 255)
                       for y in range(8) for x in range(8)])
        self.assertEqual(pack_texture(alpha, "ARGB4444", mipmap=True,
                                      wrap=True, dither=False)[3], 0x800F)

    def test_wrapped_mips_are_independent_of_tile_translation(self):
        # Any complete 8x8 tile has the same 1x1 average, regardless of seam.
        from PIL import ImageChops
        image = Image.new("RGB", (8, 8))
        image.paste((255, 0, 0), (0, 0, 2, 8))
        average = pack_texture(image, mipmap=True, wrap=True, dither=False)[3]
        for shift in range(1, 8):
            self.assertEqual(pack_texture(ImageChops.offset(image, shift, shift),
                                          mipmap=True, wrap=True, dither=False)[3], average)

    def test_predictor_matches_payload(self):
        for size in (8, 16, 64, 128, 256):
            for mipmap in (False, True):
                with self.subTest(size=size, mipmap=mipmap):
                    words = pack_texture(Image.new("RGB", (size, size)), mipmap=mipmap)
                    self.assertEqual(len(words) * 2, texture_byte_size(size, size, mipmap=mipmap))
                    self.assertEqual(len(words) * 2 % 32, 0)

    def test_rejects_invalid_hardware_layouts(self):
        for width, height, mipmap in ((7, 8, False), (12, 8, False), (2048, 8, False),
                                       (8, 16, True), (0, 8, False)):
            with self.subTest(width=width, height=height, mipmap=mipmap):
                with self.assertRaises(ValueError):
                    texture_byte_size(width, height, mipmap=mipmap)
        with self.assertRaises(ValueError):
            pack_texture(Image.new("RGB", (8, 8)), "RGBA8888")


if __name__ == "__main__":
    unittest.main()
