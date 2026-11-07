/* Engine: sky dome and lens flare (sky.h). */
#include "sky.h"
#include <malloc.h>
#include <stdio.h>

bool sky_init(SkyDome *s, const float *elev, int rings, int segs, float radius, const pvr_poly_hdr_t *hdr) {
    int n = rings * (segs + 1);
    s->rings = rings;
    s->segs = segs;
    s->radius = radius;
    s->hdr = hdr;
    s->dir = malloc(sizeof(V3) * n);
    s->col = malloc(sizeof(uint32_t) * n);
    if(!s->dir || !s->col) {
        printf(ENG_NAME " sky: out of memory\n");
        free(s->dir);
        free(s->col);
        s->dir = NULL;
        s->col = NULL;
        return false;
    }
    for(int e = 0; e < rings; e++) {
        float ce = cosf(elev[e]), se = sinf(elev[e]);
        for(int i = 0; i <= segs; i++) {
            float a = (float)i / segs * ENG_TAU;
            *sky_dir(s, e, i) = v3(sinf(a) * ce, se, cosf(a) * ce);
            s->col[e * (segs + 1) + i] = 0xffffffff;
        }
    }
    return true;
}

void sky_ring_color(SkyDome *s, int ring, uint32_t c) {
    for(int i = 0; i <= s->segs; i++) s->col[ring * (s->segs + 1) + i] = c;
}

void sky_set_color(SkyDome *s, int ring, int seg, uint32_t c) {
    s->col[ring * (s->segs + 1) + seg] = c;
}

void sky_draw(const SkyDome *s) {
    if(!s->dir) return;
    V3 c = r_cam.pos;
    float R = s->radius;
    r_hdr(s->hdr);
    for(int e = 0; e + 1 < s->rings; e++) {
        RV v[2 * 64];
        int n = 0;
        for(int i = 0; i <= s->segs && n + 2 <= 2 * 64; i++)
            for(int k = 1; k >= 0; k--) {
                int at = (e + k) * (s->segs + 1) + i;
                V3 d = s->dir[at];
                v[n++] = (RV){c.x + d.x * R, c.y + d.y * R, c.z + d.z * R, 0, 0, s->col[at]};
            }
        r_strip(v, n);
    }
}

bool sky_sun_screen(V3 sun_dir, float dist, float *sx, float *sy) {
    V3 sp = vmad(r_cam.pos, sun_dir, dist);
    float w;
    if(!r_project(sp, sx, sy, &w)) return false;
    return *sx >= -40 && *sx <= 680 && *sy >= -40 && *sy <= 520;
}

void sky_flare(float sx, float sy, const pvr_poly_hdr_t *hdr, const SkyGhost *g, int n, float alpha, float z) {
    float dx = 320.0f - sx, dy = 240.0f - sy;
    r_hdr(hdr);
    for(int i = 0; i < n; i++) {
        float x = sx + dx * g[i].t, y = sy + dy * g[i].t, s = g[i].size;
        r_quad2d(x - s, y - s, x + s, y + s, g[i].u0, 0, g[i].u1, 1, color_alpha(g[i].col, alpha), z);
    }
}
