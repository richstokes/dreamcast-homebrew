/* Engine: a gradient sky dome and a lens flare.

   The dome is rings of directions at rising elevations, segs round, drawn
   as strips about the camera at a fixed radius so it never moves; each
   vertex has its own colour (a ring at a time for a plain gradient, one
   by one for a glow toward the sun). The flare's ghosts sit along the
   line from the sun through the screen centre. */
#ifndef ENG_SKY_H
#define ENG_SKY_H

#include "render.h"

typedef struct {
    int rings, segs;
    V3 *dir;                  /* [rings][segs + 1] unit directions */
    uint32_t *col;            /* the same, a colour each */
    float radius;
    const pvr_poly_hdr_t *hdr;
} SkyDome;

/* rings directions at elevations elev[] (radians, rising), segs round,
   drawn at radius with hdr. Allocates; false if out of memory. */
bool sky_init(SkyDome *s, const float *elev, int rings, int segs, float radius, const pvr_poly_hdr_t *hdr);
static inline V3 *sky_dir(const SkyDome *s, int ring, int seg) {
    return &s->dir[ring * (s->segs + 1) + seg];
}
void sky_ring_color(SkyDome *s, int ring, uint32_t c);
void sky_set_color(SkyDome *s, int ring, int seg, uint32_t c);
/* The dome about r_cam.pos: a strip per pair of rings. */
void sky_draw(const SkyDome *s);

/* Where the sun (a direction, dist away) falls on the screen; false when
   behind the camera or more than 40 px off it. */
bool sky_sun_screen(V3 sun_dir, float dist, float *sx, float *sy);
/* A flare ghost: t of the way from the sun to the screen centre (1 at the
   centre, 2 mirrored beyond it), half-size in pixels, colour, and the
   texture columns it uses. */
typedef struct { float t, size; uint32_t col; float u0, u1; } SkyGhost;
/* n ghosts from the sun at (sx, sy), colours' alpha scaled by alpha, at
   overlay depth z. */
void sky_flare(float sx, float sy, const pvr_poly_hdr_t *hdr, const SkyGhost *g, int n, float alpha, float z);

#endif
