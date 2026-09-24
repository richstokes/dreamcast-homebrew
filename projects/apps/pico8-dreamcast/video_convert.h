#pragma once
#include <cstdint>

// The output is 32-byte aligned in the host. Pack two RGB565 pixels per store
// on the little-endian SH-4, with no per-pixel color conversion or mode switch.
inline void dc_convert_frame(uint16_t* output, const uint8_t* framebuffer,
                             const uint16_t* colors, uint8_t mode) {
    if (mode == 0 || (mode > 3 && mode < 129) || mode > 135 || mode == 132) {
        uint32_t pairs[256];
        for (unsigned i = 0; i < 256; ++i)
            pairs[i] = uint32_t(colors[i & 15]) | (uint32_t(colors[i >> 4]) << 16);
        auto* packed = reinterpret_cast<uint32_t*>(output);
        for (int i = 0; i < 8192; ++i) packed[i] = pairs[framebuffer[i]];
        return;
    }
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        int sx = x, sy = y;
        switch (mode) {
            case 1: sx /= 2; break;
            case 2: sy /= 2; break;
            case 3: sx /= 2; sy /= 2; break;
            case 129: sx = 127-x; break;
            case 130: sy = 127-y; break;
            case 131: case 134: sx = 127-x; sy = 127-y; break;
            case 133: sx = y; sy = 127-x; break;
            case 135: sx = 127-y; sy = x; break;
        }
        unsigned pixel = framebuffer[sy * 64 + sx / 2];
        output[y * 128 + x] = colors[(pixel >> ((sx & 1) * 4)) & 15];
    }
}
