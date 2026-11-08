/* Host stand-ins for the renderer calls terrain.c makes while building its
   scenery, which the physics harness never draws. */
#include "rl.h"
#include "engine/scenery.h"
#include "world.h"
#include <stdlib.h>

Camera r_cam;
Game game;
RenderStats rstats;
static pvr_poly_hdr_t dummy;
const pvr_poly_hdr_t *r_header(int tex, int flags) { (void)tex; (void)flags; return &dummy; }
void r_hdr(const pvr_poly_hdr_t *h) { (void)h; }
bool r_sphere_visible(V3 c, float r) { (void)c; (void)r; return true; }
void r_strip_bounded(const RV *v, int n, V3 c, float r) { (void)v; (void)n; (void)c; (void)r; }
void sc_chunk_begin(Scenery *s, int l, float a, float b, int t) { (void)s; (void)l; (void)a; (void)b; (void)t; }
void sc_chunk_end(Scenery *s) { (void)s; }
void sc_chunk_lod_point(Scenery *s, V3 p) { (void)s; (void)p; }
void sc_strip_begin(Scenery *s, const pvr_poly_hdr_t *h) { (void)s; (void)h; }
void sc_vert(Scenery *s, V3 p, float u, float v, uint32_t c) { (void)s; (void)p; (void)u; (void)v; (void)c; }
void sc_join(Scenery *s, V3 p, float u, float v, uint32_t c) { (void)s; (void)p; (void)u; (void)v; (void)c; }
void sc_strip_end(Scenery *s) { (void)s; }

uint8_t *rl_level_bin_buf;
static const Palette no_pal = {.water_k = 1.0f};
const Palette *pal = &no_pal;
int sc_draw_chunk(const Scenery *s, uint32_t i) { (void)s; (void)i; return 0; }
