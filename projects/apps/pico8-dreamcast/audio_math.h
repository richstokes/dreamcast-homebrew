#pragma once
#include <cmath>
#include <sh4zam/shz_scalar.h>

// SH4ZAM's truncation maps to SH-4 FTRC/FLOAT. Within this range subtraction
// gives the same fractional phase as fmodf(x, 1), with no libm division loop.
// Keep the fallback for infinities/NaNs and long-running, very large phases;
// shz_truncf itself requires a value representable by int32_t.
inline float dc_audio_phase(float phase) {
    if (phase > -8388608.f && phase < 8388608.f)
        return phase - shz_truncf(phase);
    return std::fmod(phase, 1.f);
}
