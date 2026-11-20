/* Ridgeline: drawing the trucks.

   Each model part (body, wheels, axle) at each level of detail is an
   engine mesh, lit per lighting group with the sun turned into the
   part's own space. A truck keeps its lit colours and is lit again every
   other frame (engine eng_lit_due), half the trucks on each, and at once
   when its lamps, paint, model or detail change: its light turns slowly,
   and lighting the four trucks' ~960 faces every frame was 0.7-0.8 ms on
   the console. The livery repaints the body's paint slots;
   lenses light up with the lights. Bodies too big for the renderer's mesh
   limit are split by triangles into several meshes at boot.

   Damage (damage.h) shows here: scuffed faces of the body and crazed glass
   as its lit colours dulled, a smashed lamp as a dark lens with no glow or
   beam, and the pieces a knock takes off (each a mesh of its own in the
   body's space) drawn where they lie instead of on the truck. */
#include "truckdraw.h"
#include "truck.h"
#include "terrain.h"
#include "world.h"
#include "lamps.h"
#include "glow.h"
#include "damage.h"
#include "engine/mesh.h"
#include "engine/prof.h"
#include <malloc.h>
#if __has_include("trucks.h")
#include "trucks.h"
#define HAVE_TRUCK_MODELS 1
#endif

#define MAX_PARTS_SPLIT 4
#define LODS 3

typedef struct {
    EngMesh m[MAX_PARTS_SPLIT];
    int n;
} PartMeshes;

#ifdef HAVE_TRUCK_MODELS
static PartMeshes parts[RL_TRUCK_COUNT][RL_PART_PIECE0][LODS];
/* The pieces that come off, at the two levels of detail that have them. */
#define PIECE_LODS 2
static EngMesh piece_mesh[RL_TRUCK_COUNT][RL_PIECE_COUNT][PIECE_LODS];
_Static_assert(RL_TRUCK_COUNT == TRUCK_MODELS, "TRUCK_MODELS in rl.h");
/* The right-hand head and tail lenses get paint slots of their own as the
   body is built, so one side's can be smashed. */
enum { PAINT_HEAD_R = RL_PAINT_COUNT, PAINT_TAIL_R, PAINT_SLOTS };
_Static_assert(PAINT_SLOTS <= ENG_PAINT_SLOTS, "paint slots");
#define LENS_SMASHED 0xff26221e
static uint32_t ref_col[RL_TRUCK_COUNT][PAINT_SLOTS];
#endif
/* Each truck's lit colours (body, then the two wheels, then the axle),
   or, with no memory for those, one buffer lit for every truck in turn. */
#define LIT_MAX 2048
static uint32_t lit_buf[LIT_MAX] __attribute__((aligned(32)));
typedef struct {
    EngLitAge age;
    uint32_t *buf;
    uint16_t at_l, at_r, at_a;   /* where the wheels' and the axle's colours start */
#ifdef HAVE_TRUCK_MODELS
    uint16_t at_p[RL_PIECE_COUNT];   /* ...and each piece's */
#endif
} TruckLit;
static TruckLit truck_lit[MAX_TRUCKS];
static int lit_cap = LIT_MAX;
static uint32_t draw_frame;
static const pvr_poly_hdr_t *h_model, *h_shadow, *h_glow, *h_flat, *h_beam, *h_pool;

#ifdef HAVE_TRUCK_MODELS
/* Split a mesh's triangles into pieces of at most ~1000 vertices. */
static void build_part(PartMeshes *pm, const rl_mesh_t *src) {
    const rl_mvert_t *v = &rl_truck_verts[src->first_vert];
    const uint16_t *idx = &rl_truck_indices[src->first_index];
    int ntri = src->index_count / 3;
    static uint16_t remap[8192];
    static eng_mvert_t vb[1024];
    static uint16_t ib[6144];
    int t = 0;
    while(t < ntri && pm->n < MAX_PARTS_SPLIT) {
        memset(remap, 0xff, sizeof(uint16_t) * src->vert_count);
        int nv = 0, ni = 0;
        while(t < ntri) {
            int need = 0;
            for(int k = 0; k < 3; k++)
                if(remap[idx[t * 3 + k]] == 0xffff) need++;
            if(nv + need > 1000 || ni + 3 > 6144) break;
            for(int k = 0; k < 3; k++) {
                int o = idx[t * 3 + k];
                if(remap[o] == 0xffff) {
                    remap[o] = (uint16_t)nv;
                    memcpy(&vb[nv], &v[o], sizeof(eng_mvert_t));
                    if(src->part == RL_PART_BODY && vb[nv].x > 0) {
                        if(vb[nv].paint == RL_PAINT_HEADLIGHT) vb[nv].paint = PAINT_HEAD_R;
                        else if(vb[nv].paint == RL_PAINT_TAILLIGHT) vb[nv].paint = PAINT_TAIL_R;
                    }
                    nv++;
                }
                ib[ni++] = remap[o];
            }
            t++;
        }
        if(!eng_mesh_build(&pm->m[pm->n], vb, nv, ib, ni, NULL)) break;
        pm->n++;
    }
}

/* A mesh's rows of trucks.h's vertex and index tables, and every row before
   them, are finished with once it is built, and the tables are two thirds
   the size of what is built from them: each built mesh's arrays move into
   that room as far as it goes and the heap gets its own back (~90 KiB a
   truck, which is what stopped an eighth fitting). Truck by truck, so the
   heap never holds them all at once either. The engine allocated each
   array on its own (eng_mesh_free), so each moves on its own. Nothing may
   read a table's rows after this. */
static uint8_t *room_at[2], *room_end[2];

static void *room_move(const void *array, size_t bytes, uint32_t *moved) {
    size_t take = (bytes + 31) & ~(size_t)31;
    for(int r = 0; r < 2; r++) {
        if(!bytes || room_at[r] + take > room_end[r]) continue;
        void *to = room_at[r];
        memcpy(to, array, bytes);
        free((void *)array);
        room_at[r] += take;
        *moved += bytes;
        return to;
    }
    return (void *)array;
}

static void meshes_into_tables(PartMeshes *pm, const rl_mesh_t *src, uint32_t *moved) {
    if(!room_at[0]) {
        room_at[0] = (uint8_t *)(((uintptr_t)rl_truck_verts + 31) & ~(uintptr_t)31);
        room_at[1] = (uint8_t *)(((uintptr_t)rl_truck_indices + 31) & ~(uintptr_t)31);
    }
    uint8_t *done[2] = {(uint8_t *)&rl_truck_verts[src->first_vert + src->vert_count],
                        (uint8_t *)&rl_truck_indices[src->first_index + src->index_count]};
    for(int r = 0; r < 2; r++)
        if(done[r] > room_end[r]) room_end[r] = done[r];
    for(int k = 0; k < pm->n; k++) {
        EngMesh *m = &pm->m[k];
        const uint16_t *s = m->km.strips;
        while(*s) s += 1 + 2 * *s;
        m->km.strips = room_move(m->km.strips, sizeof(uint16_t) * (size_t)(s + 1 - m->km.strips), moved);
        m->km.pos = room_move(m->km.pos, sizeof(float[4]) * m->km.npos, moved);
        m->km.idx = room_move(m->km.idx, sizeof(uint16_t) * m->km.ni, moved);
        m->km.v = room_move(m->km.v, sizeof(KV) * m->km.nv, moved);
        m->gvert = room_move(m->gvert, sizeof(uint16_t) * m->ngroups, moved);
        m->gargb = room_move(m->gargb, sizeof(uint32_t) * m->ngroups, moved);
        m->gpaint = room_move(m->gpaint, (size_t)m->ngroups, moved);
        m->gnrm = room_move(m->gnrm, sizeof(float[3]) * m->ngroups, moved);
    }
}
#endif

void truckdraw_init(void) {
    h_model = r_header(TEX_NONE, HF_OP);
    h_flat = h_model;
    h_shadow = r_header(TEX_SHADOW, HF_TR);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD);
    h_beam = r_header(TEX_SHAFT, HF_TR | HF_ADD | HF_NOFOG | HF_CLAMP);
    h_pool = r_header(TEX_BEAM, HF_TR | HF_ADD | HF_CLAMP);
#ifdef HAVE_TRUCK_MODELS
    _Static_assert(sizeof(rl_mvert_t) == sizeof(eng_mvert_t), "model vertex layout");
    for(int tr = 0; tr < RL_TRUCK_COUNT; tr++) {
        for(int s = 0; s < RL_PAINT_COUNT; s++) {
            /* The slot's reference colour: its first vertex's in the body. */
            ref_col[tr][s] = 0;
            for(int i = 0; i < RL_TRUCK_MESH_COUNT && !ref_col[tr][s]; i++) {
                const rl_mesh_t *m = &rl_truck_meshes[i];
                if(m->truck != tr || m->lod != 0) continue;
                for(int v = m->first_vert; v < m->first_vert + m->vert_count; v++)
                    if(rl_truck_verts[v].paint == s) {
                        ref_col[tr][s] = rl_truck_verts[v].argb;
                        break;
                    }
            }
        }
        ref_col[tr][PAINT_HEAD_R] = ref_col[tr][RL_PAINT_HEADLIGHT];
        ref_col[tr][PAINT_TAIL_R] = ref_col[tr][RL_PAINT_TAILLIGHT];
    }
    /* (The tables are read above: building overwrites them.) */
    int groups = 0, meshes = 0;
    uint32_t moved = 0;
    for(int i = 0; i < RL_TRUCK_MESH_COUNT; i++) {
        const rl_mesh_t *m = &rl_truck_meshes[i];
        if(m->lod >= LODS) continue;
        PartMeshes piece = {.n = 0}, *pm = &piece;
        if(m->part < RL_PART_PIECE0) pm = &parts[m->truck][m->part][m->lod];
        else if(m->lod >= PIECE_LODS) continue;
        build_part(pm, m);
        meshes_into_tables(pm, m, &moved);
        for(int k = 0; k < pm->n; k++) groups += pm->m[k].ngroups;
        meshes += pm->n;
        if(pm == &piece && piece.n) piece_mesh[m->truck][m->part - RL_PART_PIECE0][m->lod] = piece.m[0];
    }
    /* Room for the biggest truck's groups at full detail, per truck. */
    int most = 0;
    for(int tr = 0; tr < RL_TRUCK_COUNT; tr++)
        for(int lod = 0; lod < LODS; lod++) {
            int n = 0;
            for(int part = 0; part < RL_PART_PIECE0; part++)
                for(int k = 0; k < parts[tr][part][lod].n; k++) n += parts[tr][part][lod].m[k].ngroups;
            for(int p = 0; p < RL_PIECE_COUNT && lod < PIECE_LODS; p++) n += piece_mesh[tr][p][lod].ngroups;
            if(n > most) most = n;
        }
    if(most > LIT_MAX) most = LIT_MAX;
    uint32_t *all = memalign(32, sizeof(uint32_t) * most * MAX_TRUCKS);
    if(all) {
        lit_cap = most;
        for(int i = 0; i < MAX_TRUCKS; i++) truck_lit[i].buf = all + i * most;
    }
    printf("Ridgeline trucks: %d meshes, %d lighting groups, %d at most on a truck, %lu KiB of them in their tables' room\n",
           meshes, groups, most, (unsigned long)moved / 1024);
#endif
}

/* Column-major model matrix from axes and an origin. */
static void mat_axes(float *m, V3 x, V3 y, V3 z, V3 o) {
    m[0] = x.x; m[1] = x.y; m[2] = x.z; m[3] = 0;
    m[4] = y.x; m[5] = y.y; m[6] = y.z; m[7] = 0;
    m[8] = z.x; m[9] = z.y; m[10] = z.z; m[11] = 0;
    m[12] = o.x; m[13] = o.y; m[14] = o.z; m[15] = 1;
}

#ifdef HAVE_TRUCK_MODELS
/* Light a part's meshes into buf (consecutive group colours per mesh, at
   most cap of them); returns the words used. */
static int light_part(const PartMeshes *pm, const EngLight *l, uint32_t *buf, int cap) {
    int o = 0;
    for(int k = 0; k < pm->n; k++) {
        const EngMesh *m = &pm->m[k];
        if(o + m->ngroups > cap) break;
        eng_mesh_light(m, l, buf + o);
        o += m->ngroups;
    }
    return o;
}

/* The sun's glint on paint and glass: a Blinn highlight per lighting
   group (each a flat face), added on to its lit colour. H is the half
   vector in the part's space, k the sun's strength. A panel flares as it
   turns through the angle that sends the sun to the camera. */
static void sheen_part(const PartMeshes *pm, uint32_t *buf, V3 H, const float *k) {
    int o = 0;
    for(int m = 0; m < pm->n; m++) {
        const EngMesh *em = &pm->m[m];
        for(int g = 0; g < em->ngroups; g++) {
            int slot = em->gpaint[g];
            if(slot < RL_PAINT_BODY || slot > RL_PAINT_GLASS) continue;
            const float *n = em->gnrm[g];
            float d = n[0] * H.x + n[1] * H.y + n[2] * H.z;
            if(d < 0.8f) continue;
            float d2 = d * d, d4 = d2 * d2, d8 = d4 * d4, d16 = d8 * d8;
            float s = slot == RL_PAINT_GLASS ? d16 * d16 : d16;   /* glass sharper */
            uint32_t c = buf[o + g];
            int r = (int)((c >> 16) & 255) + (int)(k[0] * s), gg = (int)((c >> 8) & 255) + (int)(k[1] * s),
                b = (int)(c & 255) + (int)(k[2] * s);
            buf[o + g] = (c & 0xff000000u) | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(gg > 255 ? 255 : gg) << 8 |
                         (uint32_t)(b > 255 ? 255 : b);
        }
        o += em->ngroups;
    }
}

/* Damage on the body's paint and glass: each lighting group (a flat face)
   dulled toward bare, dusty metal by how scuffed the faces of the body it
   looks out of are, some more than others so a panel reads as scraped
   rather than repainted; glass goes milky instead. */
static void scuff_part(const PartMeshes *pm, uint32_t *buf, const uint8_t *face) {
    static const float deep[DMG_LEVELS] = {0, 0.22f, 0.4f, 0.58f};
    float fl = deep[face[DMG_FRONT]], fr = deep[face[DMG_REAR]], sl = deep[face[DMG_LEFT]], sr = deep[face[DMG_RIGHT]],
          tp = deep[face[DMG_TOP]];
    int o = 0;
    for(int m = 0; m < pm->n; m++) {
        const EngMesh *em = &pm->m[m];
        for(int g = 0; g < em->ngroups; g++) {
            int slot = em->gpaint[g];
            if(slot < RL_PAINT_BODY || slot > RL_PAINT_GLASS) continue;
            const float *n = em->gnrm[g];
            float a = n[0] * n[0] * (n[0] > 0 ? sr : sl) + n[2] * n[2] * (n[2] > 0 ? fl : fr) + (n[1] > 0 ? n[1] * n[1] * tp : 0);
            if(a < 0.02f) continue;
            a *= 0.7f + 0.2f * (float)((g * 7 + m * 3) & 3);
            uint32_t c = buf[o + g];
            float r = (float)((c >> 16) & 255), gg = (float)((c >> 8) & 255), b = (float)(c & 255);
            float lum = 0.3f * r + 0.6f * gg + 0.1f * b;
            float tr, tg, tb;
            if(slot == RL_PAINT_GLASS) tr = tg = tb = 0.55f * lum + 70.0f, a *= 1.3f;
            else tr = 0.52f * lum + 10.0f, tg = 0.48f * lum + 9.0f, tb = 0.44f * lum + 8.0f;
            if(a > 0.8f) a = 0.8f;
            buf[o + g] = (c & 0xff000000u) | (uint32_t)(r + (tr - r) * a) << 16 | (uint32_t)(gg + (tg - gg) * a) << 8 |
                         (uint32_t)(b + (tb - b) * a);
        }
        o += em->ngroups;
    }
}

static void draw_part(const PartMeshes *pm, const float *mat, const uint32_t *lit) {
    r_load_model(mat);
    r_hdr(h_model);
    int o = 0;
    for(int k = 0; k < pm->n; k++) {
        r_mesh_kv(&pm->m[k].km, lit + o);
        o += pm->m[k].ngroups;
    }
}
#endif

/* The headlights on the ground: one beam from the nose (flat, fh ahead
   and rh across) to where it lands, the beam texture (a cone from above,
   brightest a third of the way out: tools/build_textures.py's tex_beam)
   laid over the ground's own triangles from start to end ahead, w across
   each way at its far end. The beam's axis climbs slope a metre: ground
   well above or below it is out of the light. */
#define BEAM_TEX_NEAR 0.35f             /* its width at the truck, of that at the far end */
#define BEAM_TEX_U (15.0f / 32.0f)      /* u from the middle to the cone's edge at the far end */
#define BEAM_TEX_V0 (1.0f / 64.0f)      /* v at the start, */
#define BEAM_TEX_DV (62.0f / 64.0f)     /* and from there to the end */
static void beam_pool(V3 nose, V3 fh, V3 rh, float slope, float start, float end, float w, uint32_t col, float alpha) {
    V3 mid = vmad(nose, fh, (start + end) * 0.5f);
    if(!r_sphere_visible(mid, end - start + w)) return;
    float ku = BEAM_TEX_U / w, kv = BEAM_TEX_DV / (end - start);
    float across = rh.x * nose.x + rh.z * nose.z, along = fh.x * nose.x + fh.z * nose.z;
    /* Whole within 1.5 m of the axis and 0.15 m more a metre out. */
    const float spread = 0.15f;
    TerDecal d = {.u = {0.5f - ku * across, ku * rh.x, ku * rh.z},
                  .v = {BEAM_TEX_V0 - kv * (along + start), kv * fh.x, kv * fh.z},
                  .axis = {nose.y - slope * along, slope * fh.x, slope * fh.z},
                  .reach = {1.5f - spread * along, spread * fh.x, spread * fh.z},
                  .lift = 0.06f,
                  .col = color_alpha(col, alpha)};
    for(int k = 0; k < 4; k++) {
        float a = k < 2 ? start : end, l = (k == 0 || k == 3 ? -w : w) * (k < 2 ? BEAM_TEX_NEAR : 1.0f);
        d.quad[k][0] = nose.x + fh.x * a + rh.x * l;
        d.quad[k][1] = nose.z + fh.z * a + rh.z * l;
    }
    r_hdr(h_pool);
    terrain_decal(&d);
}

/* The same from a deck or a rock's top, which are not the terrain's
   triangles: a strip of rows laid on whatever the truck could stand on
   (each row sampled, so a slope bends it rather than kinking it), the
   glow texture across for soft sides. It stops at the deck's sides and
   end: a point sampled past them would find the water or the loch's bed
   and pull the strip down into a lit wall. */
#define BEAM_ROWS 13
#define BEAM_COLS 5
#define BEAM_MID (BEAM_COLS / 2)
#define BEAM_EDGE_STEPS 4
/* What a point of the patch lies on, and whether that carries on from the
   last point (href): anything standing up off the terrain does, ground or
   water only if level with it. */
static bool patch_ground(float x, float z, float ymax, float href, float *h) {
    float g = ground_below(x, z, ymax, NULL, NULL), under = fmaxf(terrain_height(x, z), water_at(x, z));
    *h = fmaxf(g, under);
    return g > under + 0.05f || fabsf(*h - href) < 0.3f;
}
/* Where the patch runs out between p (on it, at height *h) and q (off
   it): a few halvings. Leaves the height there in *h. */
static V3 patch_edge(V3 p, V3 q, float ymax, float *h) {
    for(int k = 0; k < BEAM_EDGE_STEPS; k++) {
        V3 m = vscale(vadd(p, q), 0.5f);
        float hm;
        if(patch_ground(m.x, m.z, ymax, *h, &hm)) p = m, *h = hm;
        else q = m;
    }
    return p;
}
static void beam_patch(V3 nose, V3 fh, V3 rh, float ymax, float start, float end, float w0, float w1, uint32_t col,
                       float alpha) {
    RV v[BEAM_ROWS][BEAM_COLS];
    bool dup[BEAM_ROWS][BEAM_COLS];   /* a copy of the point inside it: the deck's side came first */
    int rows = 0;
    float hmid = 0;
    V3 mprev = nose;
    for(int k = 0; k < BEAM_ROWS; k++) {
        float t = (float)k / (BEAM_ROWS - 1);
        V3 m = vmad(nose, fh, start + (end - start) * t);
        float h;
        bool last = false;
        if(!patch_ground(m.x, m.z, ymax, hmid, &h) && k > 0) {
            /* Off the end: this row is the last, where the deck stops. */
            h = hmid;
            m = patch_edge(mprev, m, ymax, &h);
            t = (fh.x * (m.x - nose.x) + fh.z * (m.z - nose.z) - start) / (end - start);
            last = true;
        }
        hmid = h;
        mprev = m;
        float w = w0 + (w1 - w0) * t;
        float a = alpha * (t < 0.3f ? 0.55f + 0.45f * t / 0.3f : 1.0f - (t - 0.3f) / 0.7f);
        uint32_t c = color_alpha(col, a);
        /* The middle of the glow across it: a broad pool with soft edges
           rather than a bright stripe down the centre. */
        m.y = h + 0.08f;
        v[k][BEAM_MID] = (RV){m.x, m.y, m.z, 0.5f, 0.5f, c};
        dup[k][BEAM_MID] = false;
        for(int side = -1; side <= 1; side += 2) {
            V3 p = m;
            float hp = h;
            bool off = false;
            for(int i = BEAM_MID + side; i >= 0 && i < BEAM_COLS; i += side) {
                dup[k][i] = off;
                if(!off) {
                    V3 q = vmad(m, rh, (float)(i - BEAM_MID) / BEAM_MID * w);
                    float hq;
                    if(patch_ground(q.x, q.z, ymax, hp, &hq)) p = q, hp = hq;
                    else p = patch_edge(p, q, ymax, &hp), off = true;
                }
                float o = rh.x * (p.x - m.x) + rh.z * (p.z - m.z);
                v[k][i] = (RV){p.x, hp + 0.08f, p.z, 0.5f + 0.32f * o / w, 0.5f, c};
            }
        }
        rows = k + 1;
        if(last) break;
    }
    /* A strip down each pair of columns, broken where the pair has closed
       up to nothing past the deck's side. */
    for(int i = 0; i + 1 < BEAM_COLS; i++) {
        RV st[2 * BEAM_ROWS];
        int n = 0;
        for(int k = 0; k <= rows; k++) {
            if(k < rows && !dup[k][i] && !dup[k][i + 1]) {
                st[n++] = v[k][i];
                st[n++] = v[k][i + 1];
                continue;
            }
            if(n >= 4) r_strip(st, n);
            n = 0;
        }
    }
}

/* A headlight's beam through the air: a soft cone along it, turned about
   its axis to face the camera, brightest at the lamp and gone by its end
   (the shaft texture across it: lit air, thickest through the middle).
   Seen along its length it would fold into a smear, so it fades out
   there. It never goes into the ground, which would cut a hard line
   across it wherever the two met: it ends, dark, where its middle comes
   down, and an edge that would go under before that is lifted to lie on
   the ground, where the texture is dark anyway. */
#define AIR_N 5         /* points along it */
#define AIR_MARCH 12    /* looks at the ground under its middle */
/* What the cone comes down on: the terrain, or on a deck (ymax the
   height it stands on) the deck, or the water off its side. */
static float air_floor(float x, float z, float ymax) {
    if(ymax <= -1000.0f) return terrain_height(x, z);
    return fmaxf(ground_below(x, z, ymax, NULL, NULL), water_at(x, z));
}
static void beam_air(V3 lamp, V3 dir, float len, float w1, uint32_t col, float alpha, float deck) {
    static const float at[AIR_N] = {0, 0.12f, 0.3f, 0.55f, 1.0f}, lit[AIR_N] = {1.0f, 0.85f, 0.55f, 0.25f, 0};
    const float w0 = 0.18f, clear = 0.08f;
    V3 mid = vmad(lamp, dir, len * 0.5f);
    V3 to = vsub(r_cam.pos, mid);
    float tl = vlen(to);
    if(tl < 0.01f) return;
    V3 side = vcross(dir, vscale(to, 1.0f / tl));
    float sl = vlen(side);
    alpha *= clampf((sl - 0.4f) * (1.0f / 0.4f), 0, 1);
    if(alpha < 0.01f) return;
    side = vscale(side, 1.0f / sl);
    /* How far it gets before the ground. */
    float reach = len, up0 = lamp.y - air_floor(lamp.x, lamp.z, deck) - clear;
    if(up0 <= 0) return;
    for(int m = 1; m <= AIR_MARCH; m++) {
        V3 p = vmad(lamp, dir, len * m * (1.0f / AIR_MARCH));
        float up = p.y - air_floor(p.x, p.z, deck) - clear;
        if(up <= 0) {
            reach = len * (m - 1 + up0 / (up0 - up)) * (1.0f / AIR_MARCH);
            break;
        }
        up0 = up;
    }
    if(reach < 1.0f) return;
    RV a[2 * AIR_N], b[2 * AIR_N];
    for(int k = 0; k < AIR_N; k++) {
        float d = reach * at[k], w = w0 + (w1 - w0) * d / len;
        V3 p = vmad(lamp, dir, d);
        uint32_t c = color_alpha(col, alpha * lit[k]);
        for(int e = 0; e < 3; e++) {
            V3 q = vmad(p, side, (e - 1) * w);
            if(e != 1) q.y = fmaxf(q.y, air_floor(q.x, q.z, deck) + clear * 0.5f);
            RV v = {q.x, q.y, q.z, e * 0.5f, 0.5f, c};
            if(e < 2) a[2 * k + e] = v;
            if(e > 0) b[2 * k + e - 1] = v;
        }
    }
    r_strip(a, 2 * AIR_N);
    r_strip(b, 2 * AIR_N);
}

/* Where a beam from the headlights, pitched down a little from the nose,
   meets the ground: the distance along the ground, or max if it goes
   over a crest into the dark. */
static float beam_landing(const Truck *t, V3 from, float pitch, float max, bool *hit) {
    V3 F = t->ax[2], U = t->ax[1];
    V3 dir = vsub(vscale(F, cosf(pitch)), vscale(U, sinf(pitch)));
    *hit = false;
    for(float d = 2.0f; d <= max; d += 1.5f) {
        V3 q = vmad(from, dir, d);
        if(terrain_height(q.x, q.z) >= q.y) {
            *hit = true;
            return d * cosf(pitch);
        }
    }
    return max;
}

/* A soft shadow on the ground: a 3 x 3 grid following the terrain. */
/* Only ground below ymax: a vertex that found the cabin's roof beside the
   truck would pull the shadow up the wall. */
static void ground_quad(V3 c, V3 ax, V3 az, float ymax, float lift, uint32_t col) {
    RV v[3][3];
    for(int j = 0; j < 3; j++)
        for(int i = 0; i < 3; i++) {
            V3 p = vadd(c, vadd(vscale(ax, (i - 1.0f)), vscale(az, (j - 1.0f))));
            p.y = ground_below(p.x, p.z, ymax, NULL, NULL) + lift;
            v[j][i] = (RV){p.x, p.y, p.z, i * 0.5f, j * 0.5f, col};
        }
    for(int j = 0; j < 2; j++) {
        RV s[6] = {v[j + 1][0], v[j][0], v[j + 1][1], v[j][1], v[j + 1][2], v[j][2]};
        r_strip(s, 6);
    }
}

#ifdef HAVE_TRUCK_MODELS
/* The pieces knocked off (damage.c), where they lie or tumble: each lit
   as it is turned, every other frame while it moves, once more when it
   comes to rest and every couple of seconds after. */
#define DEBRIS_LIT 64
static void debris_draw(void) {
    static uint32_t lit[DMG_DEBRIS][DEBRIS_LIT];
    static uint32_t key[DMG_DEBRIS];
    for(int i = 0; i < DMG_DEBRIS; i++) {
        const Debris *d = damage_debris(i);
        if(!d->on) {
            key[i] = 0;
            continue;
        }
        /* A wheel is the truck's own wheel mesh, about its hub; a piece
           is in the body's space. */
        bool wheel = d->piece >= DMG_WHEEL0;
        int wpart = ((d->piece - DMG_WHEEL0) & 1) ? RL_PART_WHEEL_R : RL_PART_WHEEL_L;
        V3 mid = d->pos;
        if(!wheel) {
            const float *c = rl_truck_info[d->model].piece[d->piece][0];
            mid = vadd(d->pos, vadd(vadd(vscale(d->ax[0], c[0]), vscale(d->ax[1], c[1])), vscale(d->ax[2], c[2])));
        }
        float dist = vlen(vsub(mid, r_cam.pos));
        if(dist > 120.0f || !r_sphere_visible(mid, 1.6f)) continue;
        const EngMesh *em = wheel ? &parts[d->model][wpart][0].m[0] : &piece_mesh[d->model][d->piece][0];
        int lod = 0;
        if(dist > 25.0f || !em->ngroups || em->ngroups > DEBRIS_LIT) {
            const EngMesh *far = wheel ? &parts[d->model][wpart][1].m[0] : &piece_mesh[d->model][d->piece][1];
            if(far->ngroups) em = far, lod = 1;
        }
        if(!em->ngroups || em->ngroups > DEBRIS_LIT) continue;
        uint32_t k = 1u | (uint32_t)lod << 1 | (uint32_t)d->model << 2 | (uint32_t)d->piece << 6 | (uint32_t)d->livery << 10 |
                     (uint32_t)d->truck << 12;   /* (four bits of piece: fourteen of them) */
        bool rested = key[i] & 0x80000000u;
        /* (and now and then where it lies, as the day goes on) */
        if((key[i] & 0x7fffffffu) != k || (d->moving && ((draw_frame + i) & 1)) || (!d->moving && !rested) ||
           ((draw_frame + i * 16) & 127) == 0) {
            EngLight l;
            l.L = v3(vdot(sun_dir, d->ax[0]), vdot(sun_dir, d->ax[1]), vdot(sun_dir, d->ax[2]));
            for(int j = 0; j < 3; j++) {
                l.sun[j] = pal->sun_k[j] * 1.1f;
                l.sky[j] = pal->sky_k[j];
                l.ambient[j] = pal->amb_k[j] * 1.2f;
            }
            uint32_t paint[PAINT_SLOTS] = {0};
            const rl_truck_info_t *info = &rl_truck_info[d->model];
            int lv = d->livery % RL_LIVERIES;
            if(lv) {
                paint[RL_PAINT_BODY] = info->livery[lv][0];
                paint[RL_PAINT_ACCENT] = info->livery[lv][1];
                paint[RL_PAINT_TRIM] = info->livery[lv][2];
            }
            l.paint = paint;
            l.ref = ref_col[d->model];
            l.unlit_mask = 0;
            eng_mesh_light(em, &l, lit[i]);
            key[i] = k | (d->moving ? 0 : 0x80000000u);   /* (top bit: lit where it came to rest) */
        }
        float mat[16];
        mat_axes(mat, d->ax[0], d->ax[1], d->ax[2], d->pos);
        r_load_model(mat);
        r_hdr(h_model);
        r_mesh_kv(&em->km, lit[i]);
    }
}
#endif

#define TAIL_REVERSE 0xfffff6e4 /* the tail lamps in reverse gear */

/* The hazard lights: on by themselves once a truck has been upside down
   for two seconds, amber at its four corners, flashing (lit first) until
   it is back over. */
#define HAZARD_AFTER 2.0f
#define HAZARD_PERIOD 0.7f
#define HAZARD_AMBER 0xffffa820
static bool hazards_lit(const Truck *t) {
    float on = t->flip_t - HAZARD_AFTER;
    return on >= 0 && fmodf(on, HAZARD_PERIOD) < 0.5f * HAZARD_PERIOD;
}

/* Full detail is over three times a truck's vertices at the next level
   (~3,400 against ~1,000) and ~1.8 ms of a console's frame, so besides
   the player's only one other truck has it at a time: the nearest in
   front of the camera, kept until another is a metre nearer so two
   beside each other do not trade it back and forth. Where trucks gather
   (a summit, a meeting online) three more at full detail was most of
   what took the frame over. Photo mode gives it to all that are close. */
static int full_other(void) {
    static int held = -1;
    int best = -1;
    float best_d = 9.0f, held_d = 1e9f;
    for(int ti = 1; ti < game.ntrucks; ti++) {
        const Truck *t = &trucks[ti];
        if(t->absent) continue;
        V3 d = vsub(t->pos, r_cam.pos);
        if(vdot(d, r_cam.fwd) < 1.0f) continue;
        float dist = vlen(d);
        if(ti == held) held_d = dist;
        if(dist < best_d) {
            best = ti;
            best_d = dist;
        }
    }
    if(held_d < 9.0f && held_d < best_d + 1.0f) best = held;
    held = best;
    return best;
}

void truckdraw_draw_op(void) {
    PROF_BEGIN(pt);
    draw_frame++;
    int full = game.photo ? -1 : full_other();
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        if(t->absent) continue;
        V3 d = vsub(t->pos, r_cam.pos);
        float dist = vlen(d);
        if(!r_sphere_visible(t->pos, 3.5f)) continue;
        /* Full detail for the player's truck and anything very close. */
        int lod = (ti == 0 && dist < 18.0f) || (game.photo ? dist < 9.0f : ti == full) ? 0 : dist < 50.0f ? 1 : 2;
        V3 R = t->ax[0], U = t->ax[1], F = t->ax[2];
        float mat[16];
        V3 origin = vsub(vsub(t->pos, vscale(U, t->spec.cog_y)), vscale(F, t->spec.cog_z));
        mat_axes(mat, R, U, F, origin);
#ifdef HAVE_TRUCK_MODELS
        TruckLit *tl = &truck_lit[ti];
        uint32_t *lit_b = tl->buf ? tl->buf : lit_buf;
        bool hazard = hazards_lit(t);
        uint32_t key = (uint32_t)t->model | (uint32_t)lod << 8 | (uint32_t)(t->livery % RL_LIVERIES) << 12 |
                       (uint32_t)t->lights << 16 | (t->brake > 0.2f ? 1u << 20 : 0) | (t->gear < 0 ? 1u << 21 : 0) |
                       (hazard ? 1u << 22 : 0) | (uint32_t)damage_stamp(ti) << 23;
        const DamageLook *dl = damage_look(ti);
        if(!tl->buf || eng_lit_due(&tl->age, key, draw_frame, (unsigned)ti)) {
            EngLight l;
            l.L = v3(vdot(sun_dir, R), vdot(sun_dir, U), vdot(sun_dir, F));
            for(int i = 0; i < 3; i++) {
                l.sun[i] = pal->sun_k[i] * 1.1f;
                l.sky[i] = pal->sky_k[i];
                l.ambient[i] = pal->amb_k[i] * 1.2f;
            }
            /* By the campfire or the lookout's lanterns after dark: their
               orange on the side toward them.
               One light direction is all a model has, so it swings from the
               moon to them as they take over. */
            V3 fire_from;
            float fire_k = glow_light(t->pos, &fire_from) * 1.5f;
            if(fire_k > 0.01f) {
                float moon = (l.sun[0] + l.sun[1] + l.sun[2]) * (1.0f / 3.0f);
                V3 L = vnorm(vlerp(sun_dir, fire_from, fire_k / (fire_k + moon + 0.01f)));
                l.L = v3(vdot(L, R), vdot(L, U), vdot(L, F));
                l.sun[0] += fire_k;
                l.sun[1] += fire_k * 0.55f;
                l.sun[2] += fire_k * 0.2f;
                l.ambient[0] += fire_k * 0.12f;
                l.ambient[1] += fire_k * 0.06f;
            }
            /* In another truck's headlights: their warm white on the side
               toward them, swinging the light round the same way. */
            V3 beam_from;
            float beam_k = lamps_light(t->pos, LAMPS_ALL, ti, &beam_from) * 0.65f;
            if(beam_k > 0.01f) {
                V3 Lw = vadd(vadd(vscale(R, l.L.x), vscale(U, l.L.y)), vscale(F, l.L.z));
                float rest = (l.sun[0] + l.sun[1] + l.sun[2]) * (1.0f / 3.0f);
                V3 L = vnorm(vlerp(Lw, beam_from, beam_k / (beam_k + rest + 0.01f)));
                l.L = v3(vdot(L, R), vdot(L, U), vdot(L, F));
                l.sun[0] += beam_k;
                l.sun[1] += beam_k * 0.95f;
                l.sun[2] += beam_k * 0.8f;
                for(int i = 0; i < 3; i++) l.ambient[i] += beam_k * 0.06f;
            }
            uint32_t paint[PAINT_SLOTS] = {0};
            const rl_truck_info_t *info = &rl_truck_info[t->model];
            int lv = t->livery % RL_LIVERIES;
            /* Livery 0 is the model's own colours (the stripes' three tones). */
            if(lv) {
                paint[RL_PAINT_BODY] = info->livery[lv][0];
                paint[RL_PAINT_ACCENT] = info->livery[lv][1];
                paint[RL_PAINT_TRIM] = info->livery[lv][2];
            }
            uint32_t unlit = 0;
            /* The tail lenses: white in reverse (the models have no lamp
               of their own for it), bright red on the brakes, amber on a
               flash of the hazards. */
            uint32_t tail = hazard ? HAZARD_AMBER : t->gear < 0 ? TAIL_REVERSE : t->brake > 0.2f ? 0xffff3020 : 0;
            if(t->lights) {
                /* Full beam: whiter lenses, and the roof lights with them. */
                bool full = t->lights == LIGHTS_FULL;
                paint[RL_PAINT_HEADLIGHT] = full ? 0xffffffff : 0xfffff4d8;
                paint[RL_PAINT_TAILLIGHT] = tail ? tail : 0xffd81818;
                unlit = (1u << RL_PAINT_HEADLIGHT) | (1u << RL_PAINT_TAILLIGHT);
                if(full) {
                    paint[RL_PAINT_AUX] = 0xfffff8e8;
                    unlit |= 1u << RL_PAINT_AUX;
                }
            }
            else if(tail) {
                paint[RL_PAINT_TAILLIGHT] = tail;
                unlit = 1u << RL_PAINT_TAILLIGHT;
            }
            /* The right-hand lenses as the left, and either dark if smashed. */
            paint[PAINT_HEAD_R] = paint[RL_PAINT_HEADLIGHT];
            paint[PAINT_TAIL_R] = paint[RL_PAINT_TAILLIGHT];
            if(unlit & (1u << RL_PAINT_HEADLIGHT)) unlit |= 1u << PAINT_HEAD_R;
            if(unlit & (1u << RL_PAINT_TAILLIGHT)) unlit |= 1u << PAINT_TAIL_R;
            static const uint8_t lens_slot[4] = {RL_PAINT_HEADLIGHT, PAINT_HEAD_R, RL_PAINT_TAILLIGHT, PAINT_TAIL_R};
            for(int k = 0; k < 4; k++)
                if(dl->lamps & (1u << k)) {
                    paint[lens_slot[k]] = LENS_SMASHED;
                    unlit &= ~(1u << lens_slot[k]);
                }
            l.paint = paint;
            l.ref = ref_col[t->model];
            l.unlit_mask = unlit;
            int used = light_part(&parts[t->model][RL_PART_BODY][lod], &l, lit_b, lit_cap);
            if(lod < 2) {
                /* The glint: the sun's colour, as strong as its light. */
                V3 to = vsub(r_cam.pos, t->pos);
                V3 V = vnorm(v3(vdot(to, R), vdot(to, U), vdot(to, F)));
                V3 H = vnorm(vadd(l.L, V));
                float k[3];
                for(int i = 0; i < 3; i++) k[i] = 150.0f * pal->sun_k[i] * (((pal->sun_col >> (16 - 8 * i)) & 255) * (1.0f / 255));
                if(k[0] + k[1] + k[2] > 6.0f) sheen_part(&parts[t->model][RL_PART_BODY][lod], lit_b, H, k);
            }
            if(dl->face[0] | dl->face[1] | dl->face[2] | dl->face[3] | dl->face[4])
                scuff_part(&parts[t->model][RL_PART_BODY][lod], lit_b, dl->face);
            /* Wheels: lit once for all four with the body's sun: a tyre is
               dark and round, and nobody sees its light turn with the spin. */
            EngLight lw = l;
            lw.unlit_mask = 0;
            int used_l = light_part(&parts[t->model][RL_PART_WHEEL_L][lod], &lw, lit_b + used, lit_cap - used);
            int used_r = light_part(&parts[t->model][RL_PART_WHEEL_R][lod], &lw, lit_b + used + used_l, lit_cap - used - used_l);
            int at = used + used_l + used_r;
            at += light_part(&parts[t->model][RL_PART_AXLE][lod], &lw, lit_b + at, lit_cap - at);
            tl->at_l = (uint16_t)used;
            tl->at_r = (uint16_t)(used + used_l);
            tl->at_a = (uint16_t)(used + used_l + used_r);
            /* The pieces still on it, with the body's light and lamps. */
            for(int p = 0; p < RL_PIECE_COUNT && lod < PIECE_LODS; p++) {
                const EngMesh *em = &piece_mesh[t->model][p][lod];
                tl->at_p[p] = (uint16_t)at;
                if(!em->ngroups || (dl->pieces & (1u << p)) || at + em->ngroups > lit_cap) continue;
                eng_mesh_light(em, &l, lit_b + at);
                at += em->ngroups;
            }
        }
        const uint32_t *lit_l = lit_b + tl->at_l, *lit_r = lit_b + tl->at_r, *lit_a = lit_b + tl->at_a;
        draw_part(&parts[t->model][RL_PART_BODY][lod], mat, lit_b);
        for(int p = 0; p < RL_PIECE_COUNT && lod < PIECE_LODS; p++) {
            const EngMesh *em = &piece_mesh[t->model][p][lod];
            if(em->ngroups && !(dl->pieces & (1u << p)) && tl->at_p[p] + em->ngroups <= lit_cap)
                r_mesh_kv(&em->km, lit_b + tl->at_p[p]);
        }
        /* Wheels: steered, spinning, where the suspension puts them. */
        for(int i = 0; i < 4; i++) {
            const Wheel *w = &t->wh[i];
            if(t->wheel_off & (1u << i)) continue;   /* knocked off (damage.c): it lies where it fell */
            float cs = cosf(w->steer), sn = sinf(w->steer);
            V3 f1 = vadd(vscale(F, cs), vscale(R, sn)), r1 = vsub(vscale(R, cs), vscale(F, sn));
            float cp = cosf(w->spin), sp = sinf(w->spin);
            V3 u2 = vadd(vscale(U, cp), vscale(f1, sp)), f2 = vsub(vscale(f1, cp), vscale(U, sp));
            mat_axes(mat, r1, u2, f2, w->centre);
            draw_part(&parts[t->model][(i & 1) ? RL_PART_WHEEL_R : RL_PART_WHEEL_L][lod], mat, (i & 1) ? lit_r : lit_l);
        }
        /* Axles: a beam between each pair of wheel centres. */
        if(lod < 2) {
            /* The Kestrel's, the Taco's and the Eagle's front suspension is
               independent: one beam. */
            bool ifs = t->model == RL_TRUCK_KESTREL || t->model == RL_TRUCK_TACO || t->model == RL_TRUCK_EAGLE;
            for(int a = ifs ? 1 : 0; a < 2; a++) {
                V3 cl = t->wh[a * 2].centre, cr = t->wh[a * 2 + 1].centre;
                V3 x = vsub(cr, cl);
                V3 xn = vnorm(x);
                V3 z = vnorm(vcross(xn, U));
                V3 y = vcross(z, xn);
                mat_axes(mat, x, y, z, vscale(vadd(cl, cr), 0.5f));
                draw_part(&parts[t->model][RL_PART_AXLE][lod], mat, lit_a);
            }
        }
#else
        /* No models yet: a box for the body. */
        (void)lod;
        r_hdr(h_flat);
        const TruckSpec *s = &t->spec;
        V3 c[8];
        for(int k = 0; k < 8; k++)
            c[k] = truck_to_world(t, v3((k & 1) ? s->body_hi[0] : s->body_lo[0], (k & 2) ? s->body_hi[1] : s->body_lo[1],
                                        (k & 4) ? s->body_hi[2] : s->body_lo[2]));
        static const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 4, 5}, {2, 3, 6, 7}, {0, 2, 4, 6}, {1, 3, 5, 7}};
        for(int f = 0; f < 6; f++) {
            static const uint32_t box_col[TRUCK_MODELS] = {0xffc8202a, 0xffe0601c, 0xff6a6862, 0xffc81a1e, 0xff2a8eb6,
                                                            0xfff2be18, 0xff367868, 0xffeceeec};
            uint32_t col = color_scale(box_col[t->model], 0.6f + 0.08f * f);
            RV q[4];
            for(int k = 0; k < 4; k++) q[k] = (RV){c[faces[f][k]].x, c[faces[f][k]].y, c[faces[f][k]].z, 0, 0, col};
            r_strip(q, 4);
        }
        for(int i = 0; i < 4; i++) {
            V3 wc = t->wh[i].centre;
            r_billboard(wc, s->radius, s->radius, 0, 0, 0, 1, 1, 0xff202020);
        }
#endif
    }
#ifdef HAVE_TRUCK_MODELS
    debris_draw();
#endif
    PROF_LAP(PS_TRUCKS, pt);
}

void truckdraw_draw_tr(void) {
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        if(t->absent || vlen(vsub(t->pos, r_cam.pos)) > 120.0f) continue;
        const TruckSpec *s = &t->spec;
        V3 F = t->ax[2];
        V3 fh = vnorm(v3(F.x, 0, F.z)), rh = v3(fh.z, 0, -fh.x);
        /* The shadow, darker the closer the body sits to the ground. */
        V3 c = t->pos;
        float g = ground_below(c.x, c.z, c.y, NULL, NULL);
        float k = clampf(1.0f - (c.y - g - s->cog_y) / 3.0f, 0, 1);
        if(k > 0) {
            r_hdr(h_shadow);
            float hx = (s->body_hi[0] - s->body_lo[0]) * 0.62f, hz = (s->body_hi[2] - s->body_lo[2]) * 0.56f;
            V3 sc = vadd(v3(c.x, 0, c.z), vscale(v3(-sun_dir.x, 0, -sun_dir.z), 0.6f));
            ground_quad(sc, vscale(rh, hx), vscale(fh, hz), c.y, 0.06f, color_alpha(0xff000000, 0.55f * k));
        }
#ifdef HAVE_TRUCK_MODELS
        if(hazards_lit(t)) {
            /* A flash at each corner: outboard of the headlights, where a
               truck's indicators are, and on the tail lenses. */
            const rl_truck_info_t *hi = &rl_truck_info[t->model];
            float sz = 0.45f * (1.0f + 0.5f * pal->dark);
            uint32_t col = color_alpha(HAZARD_AMBER, 0.85f);
            r_hdr(h_glow);
            int out_mask = damage_look(ti)->lamps;   /* a smashed lamp flashes nothing */
            for(int i = 0; i < 2; i++) {
                float out = hi->headlight[i][0] < 0 ? -hi->headlight_r : hi->headlight_r;
                V3 hp = truck_to_world(t, v3(hi->headlight[i][0] + out, hi->headlight[i][1], hi->headlight[i][2]));
                if(!(out_mask & (DMG_HEAD_L << i))) r_billboard(vmad(hp, F, 0.05f), sz, sz, 0, 0, 0, 1, 1, col);
                V3 tp = truck_to_world(t, v3(hi->taillight[i][0], hi->taillight[i][1], hi->taillight[i][2]));
                if(!(out_mask & (DMG_TAIL_L << i))) r_billboard(tp, sz, sz, 0, 0, 0, 1, 1, col);
            }
        }
#endif
        if(!t->lights) continue;
        bool full = t->lights == LIGHTS_FULL;
        /* Up on a deck or a tall rock, or a jetty's planks over the water */
        float th = terrain_height(c.x, c.z), wy = water_at(c.x, c.z);
        bool on_deck = g > th + 1.5f || (wy > th && g > wy + 0.3f);
        /* Smashed lamps: no glow and no beam from that one, and less light
           on the ground (none with both gone). */
        const DamageLook *dl = damage_look(ti);
        float beam_k = damage_beam(ti);
        /* Where the beam lands: dipped is pitched down and short, full
           beam flatter and far. */
        V3 nose = vmad(c, F, s->body_hi[2]);
        nose.y += 0.4f;
        bool hit;
        float pitch = (full ? 2.5f : 6.0f) * ENG_PI / 180.0f;
        float land = beam_landing(t, nose, pitch, full ? 40.0f : 18.0f, &hit);
        r_hdr(h_glow);
#ifdef HAVE_TRUCK_MODELS
        const rl_truck_info_t *info = &rl_truck_info[t->model];
        V3 to_cam = vnorm(vsub(r_cam.pos, t->pos));
        float facing = clampf(vdot(to_cam, F), 0, 1);
        /* The beams through the air: faint by day, carrying at night and
           in the summit's cloud. */
        V3 bdir = vsub(vscale(F, cosf(pitch)), vscale(t->ax[1], sinf(pitch)));
        float blen = full ? 22.0f : 13.0f;
        float air_a = (0.03f + 0.18f * pal->dark + 0.18f * world_mist()) * (full ? 1.0f : 0.8f);
        r_hdr(h_beam);
        float deck = on_deck ? c.y : -1e9f;
        for(int i = 0; i < 2; i++) {
            if(dl->lamps & (DMG_HEAD_L << i)) continue;
            V3 p = truck_to_world(t, v3(info->headlight[i][0], info->headlight[i][1], info->headlight[i][2]));
            beam_air(vmad(p, F, 0.1f), bdir, blen, full ? 2.2f : 1.6f, full ? 0xfffff8ec : 0xfffff0d0, air_a, deck);
        }
        r_hdr(h_glow);
        for(int i = 0; i < 2; i++) {
            V3 p = truck_to_world(t, v3(info->headlight[i][0], info->headlight[i][1], info->headlight[i][2]));
            /* Brighter and bigger in the dark (full beam a little more,
               short of a glare that hides the truck). */
            float sz = info->headlight_r * (2.2f + 4.5f * facing * facing) * (1.0f + 0.4f * pal->dark) * (full ? 1.2f : 1.0f);
            if(!(dl->lamps & (DMG_HEAD_L << i)))
                r_billboard(vmad(p, F, 0.05f), sz, sz, 0, 0, 0, 1, 1,
                            color_alpha(full ? 0xffffffff : 0xfffff0c8, (0.45f + 0.4f * facing) * (1.0f + 0.5f * pal->dark)));
            V3 tl = truck_to_world(t, v3(info->taillight[i][0], info->taillight[i][1], info->taillight[i][2]));
            if(!(dl->lamps & (DMG_TAIL_L << i)))
                r_billboard(tl, 0.35f, 0.35f, 0, 0, 0, 1, 1, color_alpha(t->gear < 0 ? TAIL_REVERSE : 0xffff2010, 0.6f));
        }
        /* (the aux lights go with the piece they are on) */
        if(full && !(info->aux_piece >= 0 && (dl->pieces & (1u << (info->aux_piece - RL_PART_PIECE0)))))
            for(int i = 0; i < info->n_aux && i < 4; i++) {
                V3 p = truck_to_world(t, v3(info->aux_light[i][0], info->aux_light[i][1], info->aux_light[i][2]));
                float sz = (0.25f + 0.9f * facing * facing) * (1.0f + 0.5f * pal->dark);
                r_billboard(vmad(p, F, 0.05f), sz, sz, 0, 0, 0, 1, 1, color_alpha(0xffffffff, 0.4f + 0.5f * facing));
            }
#endif
        /* The beam on the ground, from the nose to where it lands. Over
           a crest it lands nowhere and only a faint trace is left. */
        if(beam_k <= 0) continue;
        float a = (full ? 0.34f + 0.5f * pal->dark : 0.3f + 0.45f * pal->dark) * (hit ? 1.0f : 0.45f) * beam_k;
        uint32_t col = full ? 0xfffff4d8 : 0xffffe8b0;
        /* (a deck's or a tall rock's top is not the terrain the pool is laid on) */
        if(on_deck) {
            r_hdr(h_glow);
            beam_patch(v3(nose.x, 0, nose.z), fh, rh, nose.y + 1.5f, 1.0f, land * 1.25f, 1.6f, full ? 7.0f : 4.5f, col, a);
        }
        else {
            V3 bd = vsub(vscale(F, cosf(pitch)), vscale(t->ax[1], sinf(pitch)));
            float slope = clampf(bd.y / sqrtf(fmaxf(1.0f - bd.y * bd.y, 0.01f)), -1.0f, 1.0f);
            beam_pool(nose, fh, rh, slope, 0.4f, land * 1.25f, full ? 8.0f : 5.2f, col, a);
        }
    }
}
