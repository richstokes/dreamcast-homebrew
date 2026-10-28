/* The scenery builder: a track's static geometry goes into the vertex
   pool as world-space strips grouped into chunks with bounding spheres,
   which world.c draws when in view. */
#include "world_int.h"

Chunk *cur_chunk;
int cur_strip_first;
const pvr_poly_hdr_t *strip_hdr;

void chunk_begin(int list, float lod_far) {
    if(chunk_n >= MAX_CHUNKS) {
        built_ok = false;
        cur_chunk = NULL;
        return;
    }
    cur_chunk = &chunks[chunk_n];
    cur_chunk->list = (uint8_t)list;
    cur_chunk->lod_far = lod_far;
    cur_chunk->first_strip = (uint16_t)strip_n;
    cur_chunk->nstrips = 0;
    cur_chunk->far = 0;
    cur_chunk->lod = 0;
    cur_strip_first = pool_n;
}

void chunk_end(void) {
    if(!cur_chunk) return;
    if(strip_n == cur_chunk->first_strip) {
        cur_chunk = NULL;
        return;
    }
    cur_chunk->nstrips = (uint16_t)(strip_n - cur_chunk->first_strip);
    int v0 = strips[cur_chunk->first_strip].first;
    V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for(int i = v0; i < pool_n; i++) {
        lo.x = fminf(lo.x, pool[i].x); hi.x = fmaxf(hi.x, pool[i].x);
        lo.y = fminf(lo.y, pool[i].y); hi.y = fmaxf(hi.y, pool[i].y);
        lo.z = fminf(lo.z, pool[i].z); hi.z = fmaxf(hi.z, pool[i].z);
    }
    cur_chunk->c = vscale(vadd(lo, hi), 0.5f);
    cur_chunk->r = vlen(vsub(hi, lo)) * 0.5f;
    chunk_n++;
    cur_chunk = NULL;
}

static void sv(float x, float y, float z, float u, float v, uint32_t c) {
    if(pool_n >= MAX_POOL) {
        built_ok = false;
        return;
    }
    pool[pool_n++] = (RV){x, y, z, u, v, c};
}

/* While set, every vertex built goes through this (Spiral Canyon's
   tunnels: dim inside, warm pools of torchlight). */
uint32_t (*svp_tint)(V3 p, uint32_t c);

void svp(V3 p, float u, float v, uint32_t c) {
    if(svp_tint) c = svp_tint(p, c);
    c = sunset_grade(c);
#ifdef DK_ROCK_TINT
    if(diag_on) c = diag_colour(p, c);
#endif
    sv(p.x, p.y, p.z, u, v, c);
}

/* svp, joining on to the strip so far with a degenerate pair (the last
   vertex again, this one twice) when it already has vertices: one strip
   for small separate pieces, each strip costing as much as ~4 vertices
   on the console. Nothing is back-face culled, so parity doesn't matter. */
void svj(V3 p, float u, float v, uint32_t c) {
    if(pool_n > cur_strip_first && pool_n < MAX_POOL) {
        RV last = pool[pool_n - 1];
        sv(last.x, last.y, last.z, last.u, last.v, last.c);
        svp(p, u, v, c);
    }
    svp(p, u, v, c);
}

/* A sphere round the strip's vertices for r_strip_bounded. World strips
   never move once built; geometry that changes at run time goes through
   r_strip (or would need its sphere recomputed). */
static Strip strip_bound(Strip st) {
    const RV *v = pool + st.first;
    V3 lo = v3(v[0].x, v[0].y, v[0].z), hi = lo;
    for(int i = 1; i < st.count; i++) {
        lo.x = fminf(lo.x, v[i].x); hi.x = fmaxf(hi.x, v[i].x);
        lo.y = fminf(lo.y, v[i].y); hi.y = fmaxf(hi.y, v[i].y);
        lo.z = fminf(lo.z, v[i].z); hi.z = fmaxf(hi.z, v[i].z);
    }
    st.c = vscale(vadd(lo, hi), 0.5f);
    float r2 = 0;
    for(int i = 0; i < st.count; i++) {
        V3 d = vsub(v3(v[i].x, v[i].y, v[i].z), st.c);
        r2 = fmaxf(r2, vdot(d, d));
    }
    st.r = sqrtf(r2) * 1.001f + 0.01f;
    return st;
}

/* Take a whole number of texture repeats off a strip's UVs so they start
   in [0, 1). The textures repeat, so the picture is the same, but the PVR
   interpolates texture coordinates with less precision than a float: the
   road's v runs to the lap length / 16 m (80 on Apple Valley, 540 on its
   curbs at 2.4 m a repeat), and at those values a texel's fraction is
   lost, so the texture swims by whole pixels on the polygons nearest the
   camera as it moves, worst on the grid just before the start line
   (s wraps there, so v is at its largest). Flycast samples in full float
   and never shows it. A strip within [0, 1] on both axes is left alone:
   the clamped textures (banners, the moon) rely on their edges. */
static void strip_wrap_uv(RV *v, int n) {
    float umin = v[0].u, umax = v[0].u, vmin = v[0].v, vmax = v[0].v;
    for(int i = 1; i < n; i++) {
        umin = fminf(umin, v[i].u); umax = fmaxf(umax, v[i].u);
        vmin = fminf(vmin, v[i].v); vmax = fmaxf(vmax, v[i].v);
    }
    float du = umin >= 0.0f && umax <= 1.0f ? 0.0f : floorf(umin);
    float dv = vmin >= 0.0f && vmax <= 1.0f ? 0.0f : floorf(vmin);
    if(du == 0.0f && dv == 0.0f) return;
    for(int i = 0; i < n; i++) {
        v[i].u -= du;
        v[i].v -= dv;
    }
}

void strip_end(void) {
    int n = pool_n - cur_strip_first;
    if(!cur_chunk || n < 3 || strip_n >= MAX_STRIPS) {
        if(strip_n >= MAX_STRIPS) built_ok = false;
        pool_n = cur_strip_first;
        return;
    }
    strip_wrap_uv(pool + cur_strip_first, n);
    /* Long strips are split so r_strip's buffer always fits. */
    int first = cur_strip_first;
    while(n >= 3) {
        int take = n > 240 ? 240 : n;
        strips[strip_n++] = strip_bound((Strip){strip_hdr, (uint16_t)first, (uint16_t)take, v3(0, 0, 0), 0});
        if(take == n) break;
        first += take - 2;
        n -= take - 2;
        if(strip_n >= MAX_STRIPS) {
            built_ok = false;
            break;
        }
    }
}

uint32_t shade(uint32_t c, V3 n) {
    float l = 0.66f + 0.42f * fmaxf(0.0f, vdot(vnorm(n), world_sun_dir));
    return color_scale(c, l);
}

/* A 4-vertex strip (a b c d: a-b top edge, c-d bottom edge). */
void quad(const pvr_poly_hdr_t *h, V3 a, V3 b, V3 c, V3 d, float u0, float v0, float u1, float v1,
          uint32_t col) {
    V3 n = vcross(vsub(b, a), vsub(c, a));
    uint32_t s = shade(col, vscale(n, -1.0f));
    if(vdot(n, n) > 0 && vnorm(n).y > 0.0f) s = shade(col, n);
    strip_begin(h);
    svp(a, u0, v0, s);
    svp(b, u1, v0, s);
    svp(c, u0, v1, s);
    svp(d, u1, v1, s);
    strip_end();
}

/* Upright box without a bottom; yaw about Y. */
void box(const pvr_poly_hdr_t *h, V3 c, V3 half, float yaw, uint32_t col, float uv_scale) {
    V3 ax = vscale(heading_right(yaw), half.x), az = vscale(heading_fwd(yaw), half.z), ay = v3(0, half.y, 0);
    V3 p[8];
    for(int i = 0; i < 8; i++)
        p[i] = vadd(c, vadd(vscale(ax, (i & 1) ? 1 : -1), vadd(vscale(ay, (i & 2) ? 1 : -1), vscale(az, (i & 4) ? 1 : -1))));
    float ux = half.x * 2 * uv_scale, uy = half.y * 2 * uv_scale, uz = half.z * 2 * uv_scale;
    /* top */
    strip_begin(h);
    uint32_t top = shade(col, v3(0, 1, 0));
    svp(p[6], 0, 0, top); svp(p[7], ux, 0, top); svp(p[2], 0, uz, top); svp(p[3], ux, uz, top);
    strip_end();
    const int faces[4][4] = {{2, 3, 0, 1}, {7, 6, 5, 4}, {6, 2, 4, 0}, {3, 7, 1, 5}};
    const V3 normals[4] = {vscale(az, -1), az, vscale(ax, -1), ax};
    for(int f = 0; f < 4; f++) {
        float uw = f < 2 ? ux : uz;
        uint32_t s = shade(col, normals[f]);
        strip_begin(h);
        svp(p[faces[f][0]], 0, 0, s); svp(p[faces[f][1]], uw, 0, s);
        svp(p[faces[f][2]], 0, uy, s); svp(p[faces[f][3]], uw, uy, s);
        strip_end();
    }
}

/* Tapered cylinder as one strip; colour may vary per ring via cb. */
void cylinder(const pvr_poly_hdr_t *h, V3 base, float r0, float r1, float height, int sides,
              uint32_t col0, uint32_t col1, float u_rep, float v_rep) {
    strip_begin(h);
    for(int i = 0; i <= sides; i++) {
        float a = (float)i / sides * DK_TAU;
        V3 n = v3(sinf(a), 0, cosf(a));
        V3 b = vmad(base, n, r0), t = vadd(vmad(base, n, r1), v3(0, height, 0));
        float u = (float)i / sides * u_rep;
        svp(t, u, 0, shade(col1, n));
        svp(b, u, v_rep, shade(col0, n));
    }
    strip_end();
}

void cone(const pvr_poly_hdr_t *h, V3 apex, float radius, float drop, int sides, uint32_t ca, uint32_t cb) {
    for(int i = 0; i < sides; i++) {
        float a0 = (float)i / sides * DK_TAU, a1 = (float)(i + 1) / sides * DK_TAU;
        V3 p0 = vadd(apex, v3(sinf(a0) * radius, -drop, cosf(a0) * radius));
        V3 p1 = vadd(apex, v3(sinf(a1) * radius, -drop, cosf(a1) * radius));
        uint32_t c = (i & 1) ? cb : ca;
        V3 n = vcross(vsub(p1, apex), vsub(p0, apex));
        if(n.y < 0) n = vscale(n, -1);
        c = shade(c, n);
        strip_begin(h);
        svp(apex, 0.5f, 0, c);
        svp(p0, 0, 1, c);
        svp(p1, 1, 1, c);
        strip_end();
    }
}

uint32_t ground_color(float h, float db, bool grassy) {
    if(h < WATER_Y) return argb(255, 200, 196, 170);           /* wet sand under water */
    if(grassy) return argb(255, 255, 255, 255);
    float wet = clampf((h - WATER_Y) / 0.25f, 0, 1);
    return color_lerp(argb(255, 196, 184, 150), argb(255, 255, 255, 255), wet);
}
