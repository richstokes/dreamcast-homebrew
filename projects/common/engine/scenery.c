/* Engine: static scenery (scenery.h). */
#include "scenery.h"
#include "prof.h"
#include <malloc.h>
#include <stdio.h>
#include <string.h>

bool sc_init(Scenery *s, uint32_t max_verts, uint32_t max_strips, uint32_t max_chunks) {
    memset(s, 0, sizeof(*s));
    s->pool = memalign(32, sizeof(RV) * max_verts);
    s->strips = malloc(sizeof(ScStrip) * max_strips);
    s->chunks = malloc(sizeof(ScChunk) * max_chunks);
    if(!s->pool || !s->strips || !s->chunks) {
        printf(ENG_NAME " scenery: out of memory for %lu vertices\n", (unsigned long)max_verts);
        sc_free(s);
        return false;
    }
    s->pool_max = max_verts;
    s->strip_max = max_strips;
    s->chunk_max = max_chunks;
    return true;
}

void sc_free(Scenery *s) {
    free(s->pool);
    free(s->strips);
    free(s->chunks);
    memset(s, 0, sizeof(*s));
}

void sc_clear(Scenery *s) {
    s->pool_n = s->strip_n = s->chunk_n = 0;
    s->full = false;
    s->cur = NULL;
}

void sc_chunk_begin(Scenery *s, int list, float near_m, float far_m, int tag) {
    if(s->chunk_n >= s->chunk_max) {
        s->full = true;
        s->cur = NULL;
        return;
    }
    ScChunk *c = &s->chunks[s->chunk_n];
    c->list = (uint8_t)list;
    c->tag = (uint8_t)tag;
    c->near2 = near_m * near_m;
    c->far2 = far_m * far_m;
    c->first_strip = s->strip_n;
    c->nstrips = 0;
    s->cur = c;
    s->lc_set = false;
}

void sc_chunk_lod_point(Scenery *s, V3 p) {
    if(!s->cur) return;
    s->cur->lc = p;
    s->lc_set = true;
}

void sc_chunk_end(Scenery *s) {
    ScChunk *c = s->cur;
    if(!c) return;
    s->cur = NULL;
    if(s->strip_n == c->first_strip) return;
    c->nstrips = (uint16_t)(s->strip_n - c->first_strip);
    /* A sphere round the strips' spheres. */
    V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for(uint32_t i = c->first_strip; i < s->strip_n; i++) {
        const ScStrip *t = &s->strips[i];
        lo.x = fminf(lo.x, t->c.x - t->r); hi.x = fmaxf(hi.x, t->c.x + t->r);
        lo.y = fminf(lo.y, t->c.y - t->r); hi.y = fmaxf(hi.y, t->c.y + t->r);
        lo.z = fminf(lo.z, t->c.z - t->r); hi.z = fmaxf(hi.z, t->c.z + t->r);
    }
    c->c = vscale(vadd(lo, hi), 0.5f);
    float r = 0;
    for(uint32_t i = c->first_strip; i < s->strip_n; i++) {
        const ScStrip *t = &s->strips[i];
        r = fmaxf(r, vlen(vsub(t->c, c->c)) + t->r);
    }
    c->r = r;
    if(!s->lc_set) c->lc = c->c;
    s->chunk_n++;
}

void sc_strip_begin(Scenery *s, const pvr_poly_hdr_t *hdr) {
    s->hdr = hdr;
    s->strip_first = s->pool_n;
}

void sc_vert(Scenery *s, V3 p, float u, float v, uint32_t c) {
    if(s->pool_n >= s->pool_max) {
        s->full = true;
        return;
    }
    s->pool[s->pool_n++] = (RV){p.x, p.y, p.z, u, v, c};
}

void sc_join(Scenery *s, V3 p, float u, float v, uint32_t c) {
    if(s->pool_n > s->strip_first && s->pool_n < s->pool_max) {
        RV last = s->pool[s->pool_n - 1];
        s->pool[s->pool_n++] = last;
        sc_vert(s, p, u, v, c);
    }
    sc_vert(s, p, u, v, c);
}

/* The renderer takes strips of up to 512 vertices. */
#define SC_MAX_STRIP 510

void sc_strip_end(Scenery *s) {
    uint32_t first = s->strip_first, n = s->pool_n - first;
    s->strip_first = s->pool_n;
    if(n < 3 || !s->cur) return;
    /* Long strips go in pieces, each repeating the two vertices before it
       (an even count, so the winding never flips, not that it matters with
       culling off). */
    uint32_t at = first;
    while(at + 2 < first + n) {
        uint32_t cnt = first + n - at;
        if(cnt > SC_MAX_STRIP) cnt = SC_MAX_STRIP;
        if(s->strip_n >= s->strip_max) {
            s->full = true;
            return;
        }
        ScStrip *t = &s->strips[s->strip_n++];
        t->hdr = s->hdr;
        t->first = at;
        t->count = (uint16_t)cnt;
        V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
        for(uint32_t i = at; i < at + cnt; i++) {
            const RV *q = &s->pool[i];
            lo.x = fminf(lo.x, q->x); hi.x = fmaxf(hi.x, q->x);
            lo.y = fminf(lo.y, q->y); hi.y = fmaxf(hi.y, q->y);
            lo.z = fminf(lo.z, q->z); hi.z = fmaxf(hi.z, q->z);
        }
        t->c = vscale(vadd(lo, hi), 0.5f);
        t->r = vlen(vsub(hi, lo)) * 0.5f + 0.01f;
        if(at + cnt >= first + n) break;
        at += cnt - 2;
    }
}

int sc_draw(const Scenery *s, int list, uint32_t tag_mask) {
    V3 cam = r_cam.pos;
    int sent = 0;
    const pvr_poly_hdr_t *last = NULL;
    for(uint32_t i = 0; i < s->chunk_n; i++) {
        const ScChunk *c = &s->chunks[i];
        if(c->list != list || !(tag_mask & (1u << c->tag))) continue;
        V3 d = vsub(c->lc, cam);
        float d2 = vdot(d, d);
        if(d2 < c->near2 || d2 >= c->far2) continue;
        if(!r_sphere_visible(c->c, c->r)) continue;
        const ScStrip *t = &s->strips[c->first_strip];
        for(int k = 0; k < c->nstrips; k++, t++) {
            if(t->hdr != last) {
                r_hdr(t->hdr);
                last = t->hdr;
            }
            r_strip_bounded(s->pool + t->first, t->count, t->c, t->r);
            sent += t->count;
        }
    }
    return sent;
}

int sc_draw_chunk(const Scenery *s, uint32_t index) {
    if(index >= s->chunk_n) return 0;
    const ScChunk *c = &s->chunks[index];
    V3 d = vsub(c->lc, r_cam.pos);
    float d2 = vdot(d, d);
    if(d2 < c->near2 || d2 >= c->far2 || !r_sphere_visible(c->c, c->r)) return 0;
    int sent = 0;
    const ScStrip *t = &s->strips[c->first_strip];
    for(int k = 0; k < c->nstrips; k++, t++) {
        r_hdr(t->hdr);
        r_strip_bounded(s->pool + t->first, t->count, t->c, t->r);
        sent += t->count;
    }
    return sent;
}
