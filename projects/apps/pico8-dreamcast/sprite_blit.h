#pragma once
#include <cstdint>
#include <cstring>

class DcSpriteBlitter {
    uint8_t previous[16]{}, mapped[256]{}, keep[256]{};
    bool valid = false;
public:
    // Already camera-adjusted/clipped, unflipped, with color mask 0xff.
    // Other cases retain the upstream renderer, including overlapping mappings.
    bool draw(const uint8_t* source, uint8_t* destination, const uint8_t* palette,
              int sx, int sy, int dx, int dy, int width, int height) {
        auto src = uintptr_t(source), dst = uintptr_t(destination);
        if ((src < dst + 8192 && dst < src + 8192) || (sx & 1) || (width & 1) ||
            sx < 0 || sy < 0 || dx < 0 || dy < 0 || width <= 0 || height <= 0 ||
            sx + width > 128 || sy + height > 128 || dx + width > 128 || dy + height > 128)
            return false;
        if (!valid || memcmp(previous, palette, 16)) {
            memcpy(previous, palette, 16); valid = true;
            for (unsigned i = 0; i < 256; ++i) {
                auto lo = palette[i & 15], hi = palette[i >> 4];
                keep[i] = (lo >> 4 ? 15 : 0) | (hi >> 4 ? 240 : 0);
                mapped[i] = (lo >> 4 ? 0 : lo) | (hi >> 4 ? 0 : hi << 4);
            }
        }
        for (int y = 0; y < height; ++y) {
            const uint8_t* in = source + (sy+y) * 64 + sx/2;
            uint8_t* out = destination + (dy+y) * 64 + dx/2;
            if (!(dx & 1)) {
                for (int x = 0; x < width/2; ++x) {
                    unsigned pair = in[x];
                    out[x] = (out[x] & keep[pair]) | mapped[pair];
                }
            } else {
                for (int x = 0; x < width/2; ++x) {
                    unsigned pair = in[x], mask = keep[pair], value = mapped[pair];
                    out[x] = (out[x] & (15 | (mask << 4))) | (value << 4);
                    out[x+1] = (out[x+1] & (240 | (mask >> 4))) | (value >> 4);
                }
            }
        }
        return true;
    }
};
