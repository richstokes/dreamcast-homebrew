/* Engine: a low-poly bird for scenery life (DreamKarts' gulls and parrots,
   Ridgeline's jays, ravens and geese). Flat vertex colours, opaque list.

   The file that draws birds defines ENG_BIRD_IMPL and includes this header
   to hold eng_bird_draw as a static function, so GCC can inline it into its
   callers as it did before the move. */
#ifndef ENG_BIRD_H
#define ENG_BIRD_H

#include "render.h"

/* Palette slots for eng_bird_draw: legs take the beak's colour. */
enum { BIRD_BODY, BIRD_SHADE, BIRD_WING, BIRD_TIP, BIRD_BEAK, BIRD_COLS };

#endif

#if defined(ENG_BIRD_IMPL) && !defined(ENG_BIRD_IMPL_DONE)
#define ENG_BIRD_IMPL_DONE
/* One bird in its own frame (+z forward, +y up): body, crown, beak, tail,
   two wings of two panels each and, standing, two legs, coloured from pal
   (body, shade, wing, tip, beak). open 0 folds the wings along the back, 1
   spreads them; flap raises (+) or lowers (-) them. At scale 1 the body is
   0.8 m nose to tail and the wings span 1.24 m. Leaves the model matrix
   loaded: call r_load_vp() after the last bird. */
static void eng_bird_draw(V3 pos, float yaw, float pitch, float roll, float open, float flap, float scale,
                          const uint32_t *pal, bool legs) {
    /* Body 0-5 (head, tail end, left, right, back, belly), beak 6-8,
       tail 9-11, then each wing: root front, root back, elbow front, elbow
       back, tip. */
    static const uint16_t idx[] = {
        0, 2, 4, 0, 4, 3, 0, 3, 5, 0, 5, 2, 1, 4, 2, 1, 3, 4, 1, 5, 3, 1, 2, 5,
        6, 7, 8, 9, 10, 11,
        12, 13, 14, 13, 15, 14, 14, 15, 16,
        17, 18, 19, 18, 20, 19, 19, 20, 21,
        22, 0, 2, 22, 3, 0, 22, 4, 3, 22, 2, 4,      /* crown */
        23, 24, 25, 26, 27, 28,                      /* legs */
    };
    const uint32_t body = pal[0], shade = pal[1], wing = pal[2], tip = pal[3], beak = pal[4];
    RV v[29] __attribute__((aligned(32)));
    int n = 0;
#define P(x, y, z, c) (v[n++] = (RV){(x), (y), (z), 0, 0, (c)})
    P(0, 0.07f, 0.30f, body);
    P(0, 0.02f, -0.26f, shade);
    P(-0.08f, 0, 0, body);
    P(0.08f, 0, 0, body);
    P(0, 0.10f, 0.02f, body);
    P(0, -0.07f, 0, shade);
    P(-0.015f, 0.06f, 0.30f, beak);
    P(0.015f, 0.06f, 0.30f, beak);
    P(0, 0.05f, 0.42f, beak);
    P(-0.07f, 0.03f, -0.22f, shade);
    P(0.07f, 0.03f, -0.22f, shade);
    P(0, 0.02f, -0.38f, body);
    /* Spread, the elbow sits 0.3 out and the tip 0.32 beyond, the outer
       panel bending further than the inner; folded, both lie along the
       back towards the tail. */
    float c0 = cosf(flap), s0 = sinf(flap), c1 = cosf(flap * 1.5f), s1 = sinf(flap * 1.5f);
    float ex = lerpf(0.09f, 0.30f * c0, open), ey = lerpf(0.07f, 0.30f * s0 + 0.04f, open);
    float tx = lerpf(0.06f, 0.30f * c0 + 0.32f * c1, open), ty = lerpf(0.07f, ey + 0.32f * s1, open);
    float ez = lerpf(-0.10f, 0.04f, open), tz = lerpf(-0.36f, -0.14f, open);
    for(int sd = -1; sd <= 1; sd += 2) {
        P(sd * 0.06f, 0.05f, 0.10f, wing);
        P(sd * 0.06f, 0.05f, -0.10f, wing);
        P(sd * ex, ey, ez, wing);
        P(sd * ex, ey, ez - 0.14f, wing);
        P(sd * tx, ty, tz, tip);
    }
    P(0, 0.16f, 0.24f, body);
    for(int sd = -1; sd <= 1; sd += 2) {
        P(sd * 0.035f, -0.04f, 0.04f, beak);
        P(sd * 0.035f, -0.04f, -0.02f, beak);
        P(sd * 0.035f, -0.17f, 0.02f, beak);
    }
#undef P
    float m[16];
    m4_translate_rotate(m, pos, yaw, pitch, roll);
    m4_scale_apply(m, scale, scale, scale);
    r_load_model(m);
    int ni = (int)(sizeof(idx) / sizeof(idx[0]));
    r_mesh(v, n, idx, legs ? ni : ni - 6, false);
}
#endif
