#pragma once
#include <cstdint>

// A clipped horizontal pen span. A fill pattern repeats every four pixels;
// resolve its palette/transparency/write mask once, then write two at a time.
inline void dc_pen_span(uint8_t* screen, int left, int right, int y,
                        uint8_t color, uint16_t pattern, bool transparent,
                        uint8_t color_mask, const uint8_t* palette) {
    uint8_t keep[4], put[4];
    const unsigned write = color_mask & 15, read = color_mask >> 4;
    for (int x = 0; x < 4; ++x) {
        bool alternate = (pattern >> (15 - x - 4 * (y & 3))) & 1;
        keep[x] = alternate && transparent ? 15 : (~write & 15);
        put[x] = alternate && transparent ? 0 :
            (palette[alternate ? color >> 4 : color & 15] & write & read);
    }
    auto pixel = [&](int x) {
        auto& byte = screen[y * 64 + x / 2];
        unsigned shift = (x & 1) * 4;
        byte = (byte & ((keep[x & 3] << shift) | (15 << (4 - shift)))) |
               (put[x & 3] << shift);
    };
    if (left & 1) pixel(left++);
    if (left <= right && !(right & 1)) pixel(right--);
    uint8_t keep_pair[] = {uint8_t(keep[0] | (keep[1] << 4)), uint8_t(keep[2] | (keep[3] << 4))};
    uint8_t put_pair[] = {uint8_t(put[0] | (put[1] << 4)), uint8_t(put[2] | (put[3] << 4))};
    auto* row = screen + y * 64;
    for (int x = left; x <= right; x += 2) {
        auto& byte = row[x / 2];
        unsigned pair = (x >> 1) & 1;
        byte = (byte & keep_pair[pair]) | put_pair[pair];
    }
}
