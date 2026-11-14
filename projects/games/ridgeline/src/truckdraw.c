/* Ridgeline: drawing the trucks.

   Each model part (body, wheels, axle) at each level of detail is an
   engine mesh, lit per lighting group with the sun turned into the
   part's own space. A truck keeps its lit colours and is lit again every
   other frame (engine eng_lit_due), half the trucks on each, and at once
   when its lamps, paint, model or detail change: its light turns slowly,
   and lighting the four trucks' ~960 faces every frame was 0.7-0.8 ms on
   the console. The livery repaints the body's paint slots;
   lenses light up with the lights. Bodies too big for the renderer's mesh
   limit are split by triangles into several meshes at boot. */
#include "truckdraw.h"
#include "truck.h"
#include "terrain.h"
#include "world.h"
#include "lamps.h"
#include "glow.h"
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
static PartMeshes parts[RL_TRUCK_COUNT][RL_PART_COUNT][LODS];
_Static_assert(RL_TRUCK_COUNT == TRUCK_MODELS, "TRUCK_MODELS in rl.h");
static uint32_t ref_col[RL_TRUCK_COUNT][RL_PAINT_COUNT];
#endif
/* Each truck's lit colours (body, then the two wheels, then the axle),
   or, with no memory for those, one buffer lit for every truck in turn. */
#define LIT_MAX 2048
static uint32_t lit_buf[LIT_MAX] __attribute__((aligned(32)));
typedef struct {
    EngLitAge age;
    uint32_t *buf;
    uint16_t at_l, at_r, at_a;   /* where the wheels' and the axle's colours start */
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
    for(int tr = 0; tr < RL_TRUCK_COUNT; tr++)
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
    /* (The tables are read above: building overwrites them.) */
    int groups = 0, meshes = 0;
    uint32_t moved = 0;
    for(int i = 0; i < RL_TRUCK_MESH_COUNT; i++) {
        const rl_mesh_t *m = &rl_truck_meshes[i];
        if(m->lod >= LODS) continue;
        PartMeshes *pm = &parts[m->truck][m->part][m->lod];
        build_part(pm, m);
        meshes_into_tables(pm, m, &moved);
        for(int k = 0; k < pm->n; k++) groups += pm->m[k].ngroups;
        meshes += pm->n;
    }
    /* Room for the biggest truck's groups at full detail, per truck. */
    int most = 0;
    for(int tr = 0; tr < RL_TRUCK_COUNT; tr++)
        for(int lod = 0; lod < LODS; lod++) {
            int n = 0;
            for(int part = 0; part < RL_PART_COUNT; part++)
                for(int k = 0; k < parts[tr][part][lod].n; k++) n += parts[tr][part][lod].m[k].ngroups;
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
   glow texture across for soft sides. */
#define BEAM_ROWS 9
#define BEAM_COLS 3
static void beam_patch(V3 nose, V3 fh, V3 rh, float ymax, float start, float end, float w0, float w1, uint32_t col,
                       float alpha) {
    RV v[BEAM_ROWS][BEAM_COLS];
    for(int k = 0; k < BEAM_ROWS; k++) {
        float t = (float)k / (BEAM_ROWS - 1);
        float d = start + (end - start) * t;
        float w = w0 + (w1 - w0) * t;
        float a = alpha * (t < 0.3f ? 0.55f + 0.45f * t / 0.3f : 1.0f - (t - 0.3f) / 0.7f);
        uint32_t c = color_alpha(col, a);
        V3 m = vmad(nose, fh, d);
        for(int i = 0; i < BEAM_COLS; i++) {
            float u = (float)i / (BEAM_COLS - 1);
            V3 q = vmad(m, rh, (2.0f * u - 1.0f) * w);
            q.y = ground_below(q.x, q.z, ymax, NULL, NULL) + 0.08f;
            /* The middle of the glow across it: a broad pool with soft
               edges rather than a bright stripe down the centre. */
            v[k][i] = (RV){q.x, q.y, q.z, 0.18f + 0.64f * u, 0.5f, c};
        }
    }
    /* A strip down each pair of columns. */
    for(int i = 0; i + 1 < BEAM_COLS; i++) {
        RV st[2 * BEAM_ROWS];
        for(int k = 0; k < BEAM_ROWS; k++) {
            st[2 * k] = v[k][i];
            st[2 * k + 1] = v[k][i + 1];
        }
        r_strip(st, 2 * BEAM_ROWS);
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
static void beam_air(V3 lamp, V3 dir, float len, float w1, uint32_t col, float alpha) {
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
    float reach = len, up0 = lamp.y - terrain_height(lamp.x, lamp.z) - clear;
    if(up0 <= 0) return;
    for(int m = 1; m <= AIR_MARCH; m++) {
        V3 p = vmad(lamp, dir, len * m * (1.0f / AIR_MARCH));
        float up = p.y - terrain_height(p.x, p.z) - clear;
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
            if(e != 1) q.y = fmaxf(q.y, terrain_height(q.x, q.z) + clear * 0.5f);
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

#define TAIL_REVERSE 0xfffff6e4 /* the tail lamps in reverse gear */

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
        uint32_t key = (uint32_t)t->model | (uint32_t)lod << 8 | (uint32_t)(t->livery % RL_LIVERIES) << 12 |
                       (uint32_t)t->lights << 16 | (t->brake > 0.2f ? 1u << 20 : 0) | (t->gear < 0 ? 1u << 21 : 0);
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
            uint32_t paint[RL_PAINT_COUNT] = {0};
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
               of their own for it), bright red on the brakes. */
            uint32_t tail = t->gear < 0 ? TAIL_REVERSE : t->brake > 0.2f ? 0xffff3020 : 0;
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
            /* Wheels: lit once for all four with the body's sun: a tyre is
               dark and round, and nobody sees its light turn with the spin. */
            EngLight lw = l;
            lw.unlit_mask = 0;
            int used_l = light_part(&parts[t->model][RL_PART_WHEEL_L][lod], &lw, lit_b + used, lit_cap - used);
            int used_r = light_part(&parts[t->model][RL_PART_WHEEL_R][lod], &lw, lit_b + used + used_l, lit_cap - used - used_l);
            light_part(&parts[t->model][RL_PART_AXLE][lod], &lw, lit_b + used + used_l + used_r, lit_cap - used - used_l - used_r);
            tl->at_l = (uint16_t)used;
            tl->at_r = (uint16_t)(used + used_l);
            tl->at_a = (uint16_t)(used + used_l + used_r);
        }
        const uint32_t *lit_l = lit_b + tl->at_l, *lit_r = lit_b + tl->at_r, *lit_a = lit_b + tl->at_a;
        draw_part(&parts[t->model][RL_PART_BODY][lod], mat, lit_b);
        /* Wheels: steered, spinning, where the suspension puts them. */
        for(int i = 0; i < 4; i++) {
            const Wheel *w = &t->wh[i];
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
        if(!t->lights) continue;
        bool full = t->lights == LIGHTS_FULL;
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
        for(int i = 0; i < 2; i++) {
            V3 p = truck_to_world(t, v3(info->headlight[i][0], info->headlight[i][1], info->headlight[i][2]));
            beam_air(vmad(p, F, 0.1f), bdir, blen, full ? 2.2f : 1.6f, full ? 0xfffff8ec : 0xfffff0d0, air_a);
        }
        r_hdr(h_glow);
        for(int i = 0; i < 2; i++) {
            V3 p = truck_to_world(t, v3(info->headlight[i][0], info->headlight[i][1], info->headlight[i][2]));
            /* Brighter and bigger in the dark (full beam a little more,
               short of a glare that hides the truck). */
            float sz = info->headlight_r * (2.2f + 4.5f * facing * facing) * (1.0f + 0.4f * pal->dark) * (full ? 1.2f : 1.0f);
            r_billboard(vmad(p, F, 0.05f), sz, sz, 0, 0, 0, 1, 1,
                        color_alpha(full ? 0xffffffff : 0xfffff0c8, (0.45f + 0.4f * facing) * (1.0f + 0.5f * pal->dark)));
            V3 tl = truck_to_world(t, v3(info->taillight[i][0], info->taillight[i][1], info->taillight[i][2]));
            r_billboard(tl, 0.35f, 0.35f, 0, 0, 0, 1, 1, color_alpha(t->gear < 0 ? TAIL_REVERSE : 0xffff2010, 0.6f));
        }
        if(full)
            for(int i = 0; i < info->n_aux && i < 4; i++) {
                V3 p = truck_to_world(t, v3(info->aux_light[i][0], info->aux_light[i][1], info->aux_light[i][2]));
                float sz = (0.25f + 0.9f * facing * facing) * (1.0f + 0.5f * pal->dark);
                r_billboard(vmad(p, F, 0.05f), sz, sz, 0, 0, 0, 1, 1, color_alpha(0xffffffff, 0.4f + 0.5f * facing));
            }
#endif
        /* The beam on the ground, from the nose to where it lands. Over
           a crest it lands nowhere and only a faint trace is left. */
        float a = (full ? 0.34f + 0.5f * pal->dark : 0.3f + 0.45f * pal->dark) * (hit ? 1.0f : 0.45f);
        uint32_t col = full ? 0xfffff4d8 : 0xffffe8b0;
        /* (a deck's or a tall rock's top is not the terrain the pool is laid on) */
        if(g > terrain_height(c.x, c.z) + 1.5f) {
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
