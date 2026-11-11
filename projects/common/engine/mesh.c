/* Engine: rigid meshes (mesh.h). The strip builder came from DreamKarts'
   models.c, which proved the layout r_mesh_kv's assembly expects. */
#include "mesh.h"
#include <malloc.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <string.h>

/* Does triangle t (indices tri[t*3..]) have the cyclic order (a, b, c)? */
static bool tri_is(const uint16_t *tri, int t, unsigned a, unsigned b, unsigned c) {
    const uint16_t *p = tri + t * 3;
    for(int r = 0; r < 3; r++)
        if(p[r] == a && p[(r + 1) % 3] == b && p[(r + 2) % 3] == c) return true;
    return false;
}

/* Per vertex, the triangles that use it (in index order): for finding a
   strip's next triangle without scanning the whole mesh. */
typedef struct {
    uint16_t *first, *count, *list;
} VertTris;

/* Greedy strip from triangle t at rotation rot into out (vertex indices),
   taking the lowest-numbered unused triangle that continues it with the
   right winding; marks those with stamp. Returns the strip's vertex count. */
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

#ifdef ENG_XF_CHECK
/* Every strip triangle must be an unclaimed source triangle wound the same
   way (render.c counts the misses with the vertex paths'). */
extern uint32_t xf_checked, xf_mismatch;
static void check_strip(const uint16_t *tri, int ntri, uint8_t *claimed, const uint16_t *strip, int n) {
    for(int k = 0; k + 2 < n; k++) {
        unsigned a = strip[k + (k & 1)], b = strip[k + 1 - (k & 1)], c = strip[k + 2];
        int u = 0;
        while(u < ntri && (claimed[u] || !tri_is(tri, u, a, b, c))) u++;
        if(u < ntri) claimed[u] = 1;
        else xf_mismatch++;
        xf_checked++;
    }
}
#endif

bool eng_mesh_build(EngMesh *m, const eng_mvert_t *v, int nv, const uint16_t *idx, int ni, const uint8_t *key) {
    memset(m, 0, sizeof(*m));
    int ntri = ni / 3;
    if(nv < 3 || ntri < 1 || nv > 1024) return false;
    /* Scratch for the build, freed before returning. */
    uint16_t *vpos = malloc(sizeof(uint16_t) * nv);
    uint16_t *used = calloc(ntri, sizeof(uint16_t));
    VertTris vt = {calloc(nv, sizeof(uint16_t)), calloc(nv, sizeof(uint16_t)), malloc(sizeof(uint16_t) * ni)};
#ifdef ENG_XF_CHECK
    uint8_t *claimed = calloc(ntri, 1);
#else
    uint8_t *claimed = NULL;
#endif
    /* The mesh's own. */
    KV *kv = memalign(32, sizeof(KV) * nv);
    float (*pos)[4] = memalign(32, sizeof(float) * 4 * nv);
    /* Worst case: a strip per triangle, 1 + 3 * 2 words each, and the end;
       shrunk to what was used below. */
    uint16_t *strips = malloc(sizeof(uint16_t) * (ntri * 7 + 1));
    uint16_t *idx_copy = malloc(sizeof(uint16_t) * ni);
    m->gnrm = malloc(sizeof(float) * 3 * nv);
    m->gpaint = malloc(nv);
    m->gargb = malloc(sizeof(uint32_t) * nv);
    m->gvert = malloc(sizeof(uint16_t) * nv);
    m->gcolf = NULL;
    m->nslots = 0;
    bool ok = vpos && used && vt.first && vt.count && vt.list && kv && pos && strips && idx_copy && m->gnrm &&
              m->gpaint && m->gargb && m->gvert;
#ifdef ENG_XF_CHECK
    ok = ok && claimed;
#endif
    if(!ok) {
        printf(ENG_NAME " mesh: out of memory\n");
        free(kv);
        free(pos);
        free(strips);
        free(idx_copy);
        free(m->gnrm);
        free(m->gpaint);
        free(m->gargb);
        free(m->gvert);
        memset(m, 0, sizeof(*m));
        goto scratch;
    }
    memcpy(idx_copy, idx, sizeof(uint16_t) * ni);

    /* Lighting groups: normal, paint, colour (and key) alike. */
    int ng = 0;
    float r2 = 0;
    for(int i = 0; i < nv; i++) {
        int g = 0;
        for(; g < ng; g++) {
            int j = m->gvert[g];
            const eng_mvert_t *w = &v[j];
            if(v[i].nx == w->nx && v[i].ny == w->ny && v[i].nz == w->nz && v[i].paint == w->paint &&
               v[i].argb == w->argb && (!key || key[i] == key[j]))
                break;
        }
        if(g == ng) m->gvert[ng++] = (uint16_t)i;
        kv[i] = (KV){v[i].x, v[i].y, v[i].z, (uint16_t)g, 0};
        float d2 = v[i].x * v[i].x + v[i].y * v[i].y + v[i].z * v[i].z;
        if(d2 > r2) r2 = d2;
    }
    m->ngroups = ng;
    /* The group arrays were sized for a group per vertex; keep what is used. */
    if(ng < nv) {
        void *q;
        if((q = realloc(m->gnrm, sizeof(float) * 3 * ng))) m->gnrm = q;
        if((q = realloc(m->gpaint, ng))) m->gpaint = q;
        if((q = realloc(m->gargb, sizeof(uint32_t) * ng))) m->gargb = q;
        if((q = realloc(m->gvert, sizeof(uint16_t) * ng))) m->gvert = q;
    }
    for(int g = 0; g < ng; g++) {
        const eng_mvert_t *w = &v[m->gvert[g]];
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

    /* Sized for a position per vertex: keep what is used (in place, so the
       alignment stands). */
    if(np < nv) {
        void *q = realloc(pos, sizeof(float) * 4 * np);
        if(q) pos = q;
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

    /* Strips: from each unused triangle, the longest of its three rotations
       (trial runs mark with stamp 2 + rot and are undone). */
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
#ifdef ENG_XF_CHECK
        check_strip(idx, ntri, claimed, strip, n);
#endif
        strips[o++] = (uint16_t)n;
        for(int k = 0; k < n; k++) {
            strips[o++] = (uint16_t)(vpos[strip[k]] * 16);
            strips[o++] = (uint16_t)(kv[strip[k]].group * 4);
        }
    }
    strips[o++] = 0;
    uint16_t *fit = realloc(strips, sizeof(uint16_t) * o);
    if(fit) strips = fit;
    m->km = (KMesh){kv, nv, idx_copy, ni, (const float (*)[4])pos, np, strips};
scratch:
    free(vpos);
    free(used);
    free(vt.first);
    free(vt.count);
    free(vt.list);
    free(claimed);
    return ok;
}

void eng_mesh_free(EngMesh *m) {
    free((void *)m->km.v);
    free((void *)m->km.idx);
    free((void *)m->km.pos);
    free((void *)m->km.strips);
    free(m->gnrm);
    free(m->gpaint);
    free(m->gargb);
    free(m->gvert);
    free(m->gcolf);
    memset(m, 0, sizeof(*m));
}

void eng_mesh_stats(const EngMesh *m, int *strips, int *strip_verts) {
    int ns = 0, nv = 0;
    for(const uint16_t *s = m->km.strips; s && *s; s += 1 + 2 * *s) {
        ns++;
        nv += *s;
    }
    if(strips) *strips = ns;
    if(strip_verts) *strip_verts = nv;
}

static inline int ch(uint32_t c, int s) {
    return (int)((c >> s) & 255);
}

/* A mesh's groups as eng_mesh_light wants them: colour and brightness
   (the channels' sum + 1, what a repaint scales by) as floats, so a frame
   converts nothing per group but the result. */
static bool light_cache(EngMesh *m) {
    if(m->gcolf) return true;
    float (*c)[4] = malloc(sizeof(float) * 4 * (m->ngroups ? m->ngroups : 1));
    if(!c) return false;
    int top = 0;
    for(int g = 0; g < m->ngroups; g++) {
        uint32_t a = m->gargb[g];
        c[g][0] = (float)ch(a, 16);
        c[g][1] = (float)ch(a, 8);
        c[g][2] = (float)ch(a, 0);
        c[g][3] = c[g][0] + c[g][1] + c[g][2] + 1.0f;
        if(m->gpaint[g] > top) top = m->gpaint[g];
    }
    m->nslots = top + 1 > ENG_PAINT_SLOTS ? ENG_PAINT_SLOTS : top + 1;
    m->gcolf = c;
    return true;
}

void eng_mesh_light(const EngMesh *m, const EngLight *l, uint32_t *out) {
    if(!light_cache((EngMesh *)m)) {
        /* No memory for the cache: unlit rather than nothing. */
        for(int g = 0; g < m->ngroups; g++) out[g] = m->gargb[g] | 0xff000000u;
        return;
    }
    /* The repaints as floats, each with one over its reference's
       brightness: the livery's colour at the group's brightness against
       its slot's reference, so baked shading within a slot survives. */
    float pf[ENG_PAINT_SLOTS][4];
    uint32_t painted = 0;
    for(int s = 0; s < m->nslots; s++) {
        uint32_t p = l->paint ? l->paint[s] : 0;
        if(!p) continue;
        painted |= 1u << s;
        pf[s][0] = (float)ch(p, 16);
        pf[s][1] = (float)ch(p, 8);
        pf[s][2] = (float)ch(p, 0);
        /* (no reference: each group against itself, as it is) */
        uint32_t ref = l->ref ? l->ref[s] : 0;
        pf[s][3] = l->ref ? 1.0f / ((float)(ch(ref, 16) + ch(ref, 8) + ch(ref, 0)) + 1.0f) : 0.0f;
    }
    /* (d, up, 1, 0) through this gives each channel's light. */
    float mtx[16] = {l->sun[0], l->sun[1], l->sun[2], 0, l->sky[0], l->sky[1], l->sky[2], 0,
                     l->ambient[0], l->ambient[1], l->ambient[2], 0, 0, 0, 0, 0};
    shz_xmtrx_load_unaligned_4x4(mtx);
    const float Lx = l->L.x, Ly = l->L.y, Lz = l->L.z;
    const float (*gc)[4] = m->gcolf;
    uint32_t unlit = l->unlit_mask;
    for(int g = 0; g < m->ngroups; g++) {
        const float *n = m->gnrm[g];
        unsigned slot = m->gpaint[g], bit = slot < ENG_PAINT_SLOTS ? 1u << slot : 0;
        float br = gc[g][0], bg = gc[g][1], bb = gc[g][2];
        if(painted & bit) {
            float q = pf[slot][3] != 0.0f ? fminf(gc[g][3] * pf[slot][3], 1.6f) : 1.0f;
            br = fminf(pf[slot][0] * q, 255.0f);
            bg = fminf(pf[slot][1] * q, 255.0f);
            bb = fminf(pf[slot][2] * q, 255.0f);
        }
        if(unlit & bit) {
            out[g] = painted & bit ? 0xff000000u | (uint32_t)(int)br << 16 | (uint32_t)(int)bg << 8 | (uint32_t)(int)bb
                                   : m->gargb[g] | 0xff000000u;
            continue;
        }
        float d = n[0] * Lx + n[1] * Ly + n[2] * Lz;
        if(d < 0) d = 0;
        shz_vec4_t k = shz_xmtrx_transform_vec4(shz_vec4_init(d, 0.5f + 0.5f * n[1], 1.0f, 0.0f));
        int r = (int)(br * k.x), gg = (int)(bg * k.y), b = (int)(bb * k.z);
        out[g] = 0xff000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(gg > 255 ? 255 : gg) << 8 |
                 (uint32_t)(b > 255 ? 255 : b);
    }
}
