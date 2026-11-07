/* Engine: rigid meshes (mesh.h). The strip builder follows DreamKarts'
   models.c, which proved the layout r_mesh_kv's assembly expects. */
#include "mesh.h"
#include <malloc.h>
#include <stdio.h>
#include <string.h>

/* Does triangle t (indices tri[t*3..]) have the cyclic order (a, b, c)? */
static bool tri_is(const uint16_t *tri, int t, unsigned a, unsigned b, unsigned c) {
    const uint16_t *p = tri + t * 3;
    for(int r = 0; r < 3; r++)
        if(p[r] == a && p[(r + 1) % 3] == b && p[(r + 2) % 3] == c) return true;
    return false;
}

/* Per vertex, the triangles that use it: for finding a strip's next
   triangle without scanning the whole mesh. */
typedef struct {
    uint16_t *first, *count, *list;
} VertTris;

/* Greedy strip from triangle t at rotation rot into out (vertex indices),
   taking unused triangles that continue it with the right winding; marks
   those with stamp. Returns the strip's vertex count. */
static int grow_strip(const uint16_t *tri, const VertTris *vt, uint16_t *used, uint16_t stamp, int t, int rot,
                      uint16_t *out) {
    const uint16_t *p = tri + t * 3;
    int n = 0;
    for(int k = 0; k < 3; k++) out[n++] = p[(rot + k) % 3];
    used[t] = stamp;
    while(n - 2 < R_MESH_STRIP_TRIS) {
        /* Next triangle k = n - 2: (q, r, x) wound as (q, r, x) for even
           k, (r, q, x) for odd. */
        unsigned q = out[n - 2], r = out[n - 1];
        bool odd = (n - 2) & 1;
        int found = -1;
        unsigned x = 0;
        for(int s = 0; s < vt->count[q] && found < 0; s++) {
            int u = vt->list[vt->first[q] + s];
            if(used[u]) continue;
            for(int k = 0; k < 3; k++) {
                unsigned c = tri[u * 3 + k];
                if(c == q || c == r) continue;
                if(odd ? tri_is(tri, u, r, q, c) : tri_is(tri, u, q, r, c)) {
                    found = u;
                    x = c;
                    break;
                }
            }
        }
        if(found < 0) break;
        used[found] = stamp;
        out[n++] = (uint16_t)x;
    }
    return n;
}

bool eng_mesh_build(EngMesh *m, const eng_mvert_t *v, int nv, const uint16_t *idx, int ni) {
    memset(m, 0, sizeof(*m));
    int ntri = ni / 3;
    if(nv < 3 || ntri < 1 || nv > 1024) return false;
    KV *kv = memalign(32, sizeof(KV) * nv);
    float (*pos)[4] = memalign(32, sizeof(float) * 4 * nv);
    uint16_t *vpos = malloc(sizeof(uint16_t) * nv);
    uint16_t *gvert = malloc(sizeof(uint16_t) * nv);
    /* Worst case: a strip per triangle, 1 + 3 * 2 words each, and the end. */
    uint16_t *strips = malloc(sizeof(uint16_t) * (ntri * 7 + 1));
    uint16_t *used = calloc(ntri, sizeof(uint16_t));
    VertTris vt = {calloc(nv, sizeof(uint16_t)), calloc(nv, sizeof(uint16_t)), malloc(sizeof(uint16_t) * ni)};
    uint16_t *idx_copy = malloc(sizeof(uint16_t) * ni);
    if(!kv || !pos || !vpos || !gvert || !strips || !used || !vt.first || !vt.count || !vt.list || !idx_copy) {
        printf(ENG_NAME " mesh: out of memory\n");
        return false;
    }
    memcpy(idx_copy, idx, sizeof(uint16_t) * ni);

    /* Lighting groups: normal, paint and colour alike. */
    int ng = 0;
    float r2 = 0;
    for(int i = 0; i < nv; i++) {
        int g = 0;
        for(; g < ng; g++) {
            const eng_mvert_t *w = &v[gvert[g]];
            if(v[i].nx == w->nx && v[i].ny == w->ny && v[i].nz == w->nz && v[i].paint == w->paint && v[i].argb == w->argb)
                break;
        }
        if(g == ng) gvert[ng++] = (uint16_t)i;
        kv[i] = (KV){v[i].x, v[i].y, v[i].z, (uint16_t)g, 0};
        float d2 = v[i].x * v[i].x + v[i].y * v[i].y + v[i].z * v[i].z;
        if(d2 > r2) r2 = d2;
    }
    m->ngroups = ng;
    m->gnrm = malloc(sizeof(float) * 3 * ng);
    m->gpaint = malloc(ng);
    m->gargb = malloc(sizeof(uint32_t) * ng);
    for(int g = 0; g < ng; g++) {
        const eng_mvert_t *w = &v[gvert[g]];
        V3 n = vnorm(v3(w->nx, w->ny, w->nz));
        m->gnrm[g][0] = n.x;
        m->gnrm[g][1] = n.y;
        m->gnrm[g][2] = n.z;
        m->gpaint[g] = w->paint;
        m->gargb[g] = w->argb;
    }
    m->radius = sqrtf(r2);

    /* Distinct positions. */
    int np = 0;
    for(int i = 0; i < nv; i++) {
        int p = 0;
        while(p < np && !(pos[p][0] == v[i].x && pos[p][1] == v[i].y && pos[p][2] == v[i].z)) p++;
        if(p == np) {
            pos[np][0] = v[i].x;
            pos[np][1] = v[i].y;
            pos[np][2] = v[i].z;
            pos[np][3] = 0;
            np++;
        }
        vpos[i] = (uint16_t)p;
    }

    /* Triangles by vertex. */
    for(int i = 0; i < ni; i++) vt.count[idx[i]]++;
    for(int i = 1; i < nv; i++) vt.first[i] = vt.first[i - 1] + vt.count[i - 1];
    memset(vt.count, 0, sizeof(uint16_t) * nv);
    for(int t = 0; t < ntri; t++)
        for(int k = 0; k < 3; k++) {
            unsigned a = idx[t * 3 + k];
            vt.list[vt.first[a] + vt.count[a]++] = (uint16_t)t;
        }

    /* Strips: from each unused triangle, the longest of its three rotations. */
    static uint16_t strip[R_MESH_STRIP_TRIS + 2];
    int o = 0;
    for(int t = 0; t < ntri; t++) {
        if(used[t]) continue;
        int best = 0, best_n = 0;
        for(int rot = 0; rot < 3; rot++) {
            int n = grow_strip(idx, &vt, used, (uint16_t)(2 + rot), t, rot, strip);
            for(int s = 0; s < n; s++)
                for(int q = 0; q < vt.count[strip[s]]; q++) {
                    int u = vt.list[vt.first[strip[s]] + q];
                    if(used[u] == 2 + rot) used[u] = 0;
                }
            if(n > best_n) {
                best_n = n;
                best = rot;
            }
        }
        int n = grow_strip(idx, &vt, used, 1, t, best, strip);
        strips[o++] = (uint16_t)n;
        for(int k = 0; k < n; k++) {
            strips[o++] = (uint16_t)(vpos[strip[k]] * 16);
            strips[o++] = (uint16_t)(kv[strip[k]].group * 4);
        }
    }
    strips[o++] = 0;
    free(used);
    free(vt.first);
    free(vt.count);
    free(vt.list);
    free(vpos);
    free(gvert);
    m->km = (KMesh){kv, nv, idx_copy, ni, (const float (*)[4])pos, np, strips};
    return true;
}

void eng_mesh_free(EngMesh *m) {
    free((void *)m->km.v);
    free((void *)m->km.idx);
    free((void *)m->km.pos);
    free((void *)m->km.strips);
    free(m->gnrm);
    free(m->gpaint);
    free(m->gargb);
    memset(m, 0, sizeof(*m));
}

static inline int ch(uint32_t c, int s) {
    return (int)((c >> s) & 255);
}

void eng_mesh_light(const EngMesh *m, const EngLight *l, uint32_t *out) {
    for(int g = 0; g < m->ngroups; g++) {
        const float *n = m->gnrm[g];
        int slot = m->gpaint[g];
        uint32_t c = m->gargb[g];
        if(l->paint && l->paint[slot]) {
            /* The livery's colour at the vertex's brightness against the
               slot's reference, so baked shading within a slot survives. */
            uint32_t ref = l->ref ? l->ref[slot] : c;
            float lr = (float)(ch(ref, 16) + ch(ref, 8) + ch(ref, 0)) + 1.0f;
            float lv = (float)(ch(c, 16) + ch(c, 8) + ch(c, 0)) + 1.0f;
            c = color_scale(l->paint[slot], fminf(lv / lr, 1.6f));
        }
        if(l->unlit_mask & (1u << slot)) {
            out[g] = c | 0xff000000u;
            continue;
        }
        float d = n[0] * l->L.x + n[1] * l->L.y + n[2] * l->L.z;
        if(d < 0) d = 0;
        float up = 0.5f + 0.5f * n[1];
        float k[3];
        for(int i = 0; i < 3; i++) k[i] = l->ambient[i] + l->sky[i] * up + l->sun[i] * d;
        int r = (int)(ch(c, 16) * k[0]), gg = (int)(ch(c, 8) * k[1]), b = (int)(ch(c, 0) * k[2]);
        out[g] = 0xff000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(gg > 255 ? 255 : gg) << 8 |
                 (uint32_t)(b > 255 ? 255 : b);
    }
}
