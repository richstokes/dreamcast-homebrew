#include <kos.h>
#include <array>
#include <memory>
#include <cstdlib>
#include "Audio.h"
#include "performance_checks.h"
#include "kernels.h"
#include "../sprite_blit.h"
#include "../audio_math.h"
#include "nibblehelpers.h"
#include "graphics.h"
#include "host.h"
#include "../platform.h"

bool dc_frame_cache_checks(Host& host) {
    std::array<uint8_t, 8192> framebuffer{};
    uint8_t palette[16];
    for (int i = 0; i < 16; ++i) palette[i] = i;
    dc_reset_input();
    host.forceStretch(PixelPerfect);
    uint64_t first = dc_video_frames();
    host.drawFrame(framebuffer.data(), palette, 0);
    host.drawFrame(framebuffer.data(), palette, 0);
    bool passed = dc_video_frames() == first + 1;
    palette[0] = 1;
    host.drawFrame(framebuffer.data(), palette, 0);
    passed &= dc_video_frames() == first + 2;
    host.drawFrame(framebuffer.data(), palette, 129);
    passed &= dc_video_frames() == first + 3;
    host.forceStretch(StretchToFit);
    host.drawFrame(framebuffer.data(), palette, 129);
    passed &= dc_video_frames() == first + 4;
    framebuffer[3] = 0x23;
    host.drawFrame(framebuffer.data(), palette, 129);
    passed &= dc_video_frames() == first + 5;
    // A modal screen can replace the visible image without touching cart RAM.
    dc_reset_input();
    host.drawFrame(framebuffer.data(), palette, 129);
    passed &= dc_video_frames() == first + 6;
    host.forceStretch(PixelPerfect);
    dc_reset_input();
    printf("PICO8_TEST: %s FRAME_CACHE\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool pen_check() {
    auto ram = std::make_unique<PicoRam>(); ram->Reset();
    Graphics graphics("", ram.get());
    std::array<uint8_t, 8192> initial, expected;
    uint32_t seed = 123;
    auto random = [&] { seed = seed * 1664525u + 1013904223u; return uint8_t(seed >> 24); };
    for (int test = 0; test < 512; ++test) {
        for (auto& byte : initial) byte = random();
        auto& draw = ram->drawState;
        for (auto& color : draw.drawPaletteMap) color = random();
        draw.fillPattern[0] = random(); draw.fillPattern[1] = random();
        draw.fillPatternTransparencyBit = test & 1;
        ram->hwState.colorBitmask = random();
        graphics.camera(test % 5, test % 7);
        graphics.clip(test % 8, test % 9, 111, 113);
        int left = test % 39 - 9, right = left + test % 111;
        int y = test % 132 - 2; uint8_t color = random();
        memcpy(graphics.GetP8FrameBuffer(), initial.data(), initial.size());
        for (int x = left; x <= right; ++x) graphics.pset(x, y, color);
        memcpy(expected.data(), graphics.GetP8FrameBuffer(), expected.size());
        memcpy(graphics.GetP8FrameBuffer(), initial.data(), initial.size());
        graphics.rectfill(left, y, right, y, color);
        if (memcmp(expected.data(), graphics.GetP8FrameBuffer(), expected.size())) return false;
    }
    return true;
}

static bool blit_check() {
    std::array<uint8_t, 8192> source, expected, actual;
    uint8_t palette[16];
    DcSpriteBlitter blitter;
    uint32_t seed = 7;
    auto random = [&] { seed = seed * 1664525u + 1013904223u; return uint8_t(seed >> 24); };
    for (auto& value : source) value = random();
    for (int iteration = 0; iteration < 128; ++iteration) {
        for (auto& value : actual) value = random();
        expected = actual;
        for (auto& value : palette) value = random() & 31;
        int sx = (iteration % 8) * 8, sy = (iteration % 5) * 8;
        int dx = iteration % 21, dy = iteration % 33, w = 2 + 2 * (iteration % 16), h = 1 + iteration % 11;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            unsigned c = (source[(sy+y)*64+(sx+x)/2] >> (((sx+x)&1)*4)) & 15;
            c = palette[c];
            if (c >> 4) continue;
            auto& output = expected[(dy+y)*64+(dx+x)/2];
            int shift = ((dx+x)&1)*4;
            output = (output & ~(15 << shift)) | (c << shift);
        }
        if (!blitter.draw(source.data(), actual.data(), palette, sx, sy, dx, dy, w, h) || actual != expected) return false;
    }
    return !blitter.draw(actual.data(), actual.data(), palette, 0, 0, 0, 0, 8, 8);
}

bool dc_kernel_checks() {
    bool passed = dc_video_conversion_check();
    printf("PICO8_KERNEL: %s VIDEO_CONVERSION\n", passed ? "PASS" : "FAIL");
    bool pixels = true;
    std::array<uint8_t, 8192> packed_pixels;
    for (int x = 0; x < 128; ++x) for (int value = 0; value < 256; ++value) {
        packed_pixels[64*31+x/2] = 0xa5;
        setPixelNibble(x, 31, value, packed_pixels.data());
        pixels &= getPixelNibble(x, 31, packed_pixels.data()) == (value & 15);
        pixels &= getPixelNibble(x ^ 1, 31, packed_pixels.data()) == ((x & 1) ? 5 : 10);
    }
    passed &= pixels;
    printf("PICO8_KERNEL: %s PACKED_PIXELS\n", pixels ? "PASS" : "FAIL");
    bool blit = blit_check(); passed &= blit;
    printf("PICO8_KERNEL: %s SPRITE_BLIT\n", blit ? "PASS" : "FAIL");
    bool pen = pen_check(); passed &= pen;
    printf("PICO8_KERNEL: %s PEN_SPANS\n", pen ? "PASS" : "FAIL");
    bool phase = true;
    for (int i = -10000; i < 10000; ++i) {
        float x = i * 0.13371337f;
        phase &= dc_audio_phase(x) == std::fmod(x, 1.f);
    }
    for (float x : {8388608.f, -8388608.f, 1e30f, -1e30f}) phase &= dc_audio_phase(x) == std::fmod(x, 1.f);
    passed &= phase;
    printf("PICO8_KERNEL: %s SH4ZAM_PHASE\n", phase ? "PASS" : "FAIL");
    // Recorded from the unmodified audio algorithms on SH-4/GCC 15.2.0.
    constexpr uint32_t expected_pcm[] = {0xd6bf247e,0xc4a9a651,0xdb7df3cc,0x2980befb,
        0xb5248a71,0x1a6f8296,0x75a028c8,0x06712677,0xdcc633bd,0x6a56749e,0x8785dc72,0xa564eb4f};
    // Fixed sample counts, deterministic noise, every waveform/effect, loop
    // boundaries, filters, half rate, music, custom instruments, and live edits.
    for (int test = 0; test < 12; ++test) {
        auto ram = std::make_unique<PicoRam>(); ram->Reset();
        Audio synth(ram.get());
        srand(12345);
        for (int s = 0; s < 8; ++s) {
            auto& sound = ram->sfx[s];
            sound.speed = 1 + (test > 7 ? s % 3 : 0);
            sound.loopRangeStart = test > 7 ? 2 : 0;
            sound.loopRangeEnd = test > 7 ? 7 : 0;
            sound.filters = test > 7 ? (s * 31) : 0;
            for (int n = 0; n < 32; ++n) {
                sound.notes[n].setKey((n * 7 + s * 13) % 64);
                sound.notes[n].setVolume(n < 24 ? 7 : 0);
                sound.notes[n].setWaveform(test < 8 ? test : s);
                sound.notes[n].setEffect(n % 8);
            }
        }
        if (test == 9) {
            ram->songs[0].data[0] = 0x80;
            ram->songs[0].data[1] = 0x81;
            ram->songs[0].data[2] = 2;
            ram->songs[0].data[3] = 3;
            synth.api_music(0, 100, 0);
        } else {
            if (test == 10) for (auto& n : ram->sfx[7].notes) {
                n.setWaveform(1); n.setCustom(1);
            }
            for (int ch = 0; ch < 4; ++ch) synth.api_sfx(test == 10 ? 7 : ch, ch, 0, 0);
        }
        if (test >= 8) { ram->hwState.half_rate = 0xa5; ram->hwState.reverb = 0x21; ram->hwState.lowpass = 0x84; }
        alignas(32) int16_t samples[512];
        uint32_t hash = 2166136261u;
        uint64_t start = timer_us_gettime64();
        for (int block = 0; block < 24; ++block) {
            if (test == 11 && block == 12) {
                ram->sfx[0].speed = 7;
                ram->sfx[0].notes[31].setVolume(7);
                ram->sfx[1].loopRangeEnd = 0;
                ram->hwState.half_rate = 0;
            }
            synth.FillMonoAudioBuffer(samples, 0, 512);
            for (auto sample : samples) hash = (hash ^ uint16_t(sample)) * 16777619u;
        }
        printf("PICO8_KERNEL: AUDIO case=%d hash=%08lx us=%llu\n", test,
            (unsigned long)hash, (unsigned long long)(timer_us_gettime64() - start));
        passed &= hash == expected_pcm[test];
    }
    printf("PICO8_TEST: %s KERNELS\n", passed ? "PASS" : "FAIL");
    return passed;
}
