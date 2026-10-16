/* Fast SH-4 math for per-frame animation and drawing, via SH4ZAM
 * (https://sh4zam.com): FSCA sine/cosine pairs and approximate exp/pow. */
#ifndef BAZAAR_FASTMATH_H
#define BAZAAR_FASTMATH_H

#include <sh4zam/shz_scalar.h>
#include <sh4zam/shz_trig.h>

static inline void fm_sincos(float radians, float *s, float *c) {
    shz_sincos_t sc = shz_sincosf(radians);
    *s = sc.sin;
    *c = sc.cos;
}

#define fm_sinf(x) shz_sinf(x)
#define fm_cosf(x) shz_cosf(x)
#define fm_expf(x) shz_expf(x)
#define fm_powf(x, p) shz_powf((x), (p))

#endif
