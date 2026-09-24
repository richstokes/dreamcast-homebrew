#pragma once

#include <string>
#include <cstdint>

//for 1 byte (8 bit) indexes, 128x64
//should be the equivalent of return y * 64 + (x / 2);
#define COMBINED_IDX(x, y) ((y) << 6) | ((x) >> 1)
#define IS_VALID_SPR_IDX(x, y) (y >= 0 && y < 128 && x >= 0 && x < 128)
//I think this should work if you cast the buffer to a uint32_t* pointer, but not tested
//for 4 byte (32 bit) inexes, 16x8
//should be the equivalent of return y * 16 + (x / 8);
//#define COMBINED_32_BIT_IDX(x, y) ((y) << 3) | ((x) >> 3)
//this may be hlpeful to optimize sprite blitting
//idea: get uint32_t from sprite buffer- should be 8 pixels
//split it up by bit shifting, and write to screen buffer as necessary

int getCombinedIdx(int x, int y);

int isValidSprIdx(int x, int y);

// Dreamcast: keep packed-pixel read/modify/write inside the rasterizer loop.
// GCC's normal inlining heuristic otherwise leaves calls per visible pixel.
#if defined(__DREAMCAST__)
#define P8_PIXEL_INLINE inline __attribute__((always_inline))
#else
#define P8_PIXEL_INLINE inline
#endif
P8_PIXEL_INLINE void setPixelNibble(int x, int y, uint8_t value, uint8_t* buffer) {
    auto& byte = buffer[COMBINED_IDX(x, y)];
    byte = (x & 1) ? (byte & 0x0f) | ((value & 15) << 4)
                   : (byte & 0xf0) | (value & 15);
}
P8_PIXEL_INLINE uint8_t getPixelNibble(int x, int y, const uint8_t* buffer) {
    auto byte = buffer[COMBINED_IDX(x, y)];
    return (x & 1) ? byte >> 4 : byte & 15;
}
#undef P8_PIXEL_INLINE
