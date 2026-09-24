#pragma once
#include "../video_convert.h"
#include <array>

inline bool dc_video_conversion_check() {
    std::array<uint8_t, 8192> source;
    alignas(32) std::array<uint16_t, 16384> actual;
    uint16_t colors[16];
    uint32_t seed = 0x76543210;
    for (auto& pixel : source) { seed = seed * 1664525u + 1013904223u; pixel = seed >> 24; }
    for (int i = 0; i < 16; ++i) colors[i] = uint16_t(i * 4111 + 31);
    for (uint8_t mode : {0,1,2,3,4,128,129,130,131,132,133,134,135,136,255}) {
        dc_convert_frame(actual.data(), source.data(), colors, mode);
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
            auto packed = source[sy * 64 + sx / 2];
            if (actual[y * 128 + x] != colors[(packed >> ((sx & 1) * 4)) & 15]) return false;
        }
    }
    return true;
}
