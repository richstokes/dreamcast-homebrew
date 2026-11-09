/* Ridgeline: drawing the trucks.

   Each model part (body, wheels, axle) at each level of detail is an
   engine mesh, lit once a frame per lighting group with the sun turned
   into the part's own space. The livery repaints the body's paint slots;
   lenses light up with the lights. Bodies too big for the renderer's mesh
   limit are split by triangles into several meshes at boot. */
#include "truckdraw.h"
#include "truck.h"
#include "terrain.h"
#include "world.h"
#include "engine/mesh.h"
#include "engine/prof.h"
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
static uint32_t lit_buf[2048] __attribute__((aligned(32)));
static const pvr_poly_hdr_t *h_model, *h_shadow, *h_glow, *h_flat, *h_beam;

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
#endif

void truckdraw_init(void) {
    h_model = r_header(TEX_NONE, HF_OP);
    h_flat = h_model;
    h_shadow = r_header(TEX_SHADOW, HF_TR);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD);
    h_beam = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
#ifdef HAVE_TRUCK_MODELS
    _Static_assert(sizeof(rl_mvert_t) == sizeof(eng_mvert_t), "model vertex layout");
    int groups = 0, meshes = 0;
    for(int i = 0; i < RL_TRUCK_MESH_COUNT; i++) {
        const rl_mesh_t *m = &rl_truck_meshes[i];
        if(m->lod >= LODS) continue;
        PartMeshes *pm = &parts[m->truck][m->part][m->lod];
        build_part(pm, m);
        for(int k = 0; k < pm->n; k++) groups += pm->m[k].ngroups;
        meshes += pm->n;
    }
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
    printf("Ridgeline trucks: %d meshes, %d lighting groups\n", meshes, groups);
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
/* Light a part's meshes into buf (consecutive group colours per mesh);
   returns the words used. */
static int light_part(const PartMeshes *pm, const EngLight *l, uint32_t *buf) {
    int o = 0;
    for(int k = 0; k < pm->n; k++) {
        const EngMesh *m = &pm->m[k];
        if(o + m->ngroups > 2048) break;
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

/* The headlights on the ground: one beam from the nose to where it lands,
   a cone footprint widening with distance, drawn as a strip of rows laid
   on the terrain (each row sampled, so a slope bends it rather than
   kinking it), the glow texture across for soft sides. Brightest a third
   of the way out, gone a little past the landing. */
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

/* A headlight's beam through the air: a soft wedge along it, turned about
   its axis to face the camera, brightest just out from the lamp and gone
   by its end (the glow texture across it gives the soft sides). Seen
   along its length it would fold into a smear, so it fades out there. */
static void beam_air(V3 lamp, V3 dir, float len, float w1, uint32_t col, float alpha) {
    V3 end = vmad(lamp, dir, len);
    V3 mid = vscale(vadd(lamp, end), 0.5f);
    V3 to = vsub(r_cam.pos, mid);
    float tl = vlen(to);
    if(tl < 0.01f) return;
    V3 side = vcross(dir, vscale(to, 1.0f / tl));
    float sl = vlen(side);
    alpha *= clampf((sl - 0.4f) * (1.0f / 0.4f), 0, 1);
    if(alpha < 0.01f) return;
    side = vscale(side, 1.0f / sl);
    V3 q = vmad(lamp, dir, len * 0.25f);
    float wq = 0.1f + (w1 - 0.1f) * 0.25f;
    uint32_t c0 = color_alpha(col, alpha * 0.5f), c1 = color_alpha(col, alpha), c2 = col & 0xffffffu;
    RV v[6] = {{lamp.x - side.x * 0.1f, lamp.y - side.y * 0.1f, lamp.z - side.z * 0.1f, 0, 0.5f, c0},
               {lamp.x + side.x * 0.1f, lamp.y + side.y * 0.1f, lamp.z + side.z * 0.1f, 1, 0.5f, c0},
               {q.x - side.x * wq, q.y - side.y * wq, q.z - side.z * wq, 0, 0.5f, c1},
               {q.x + side.x * wq, q.y + side.y * wq, q.z + side.z * wq, 1, 0.5f, c1},
               {end.x - side.x * w1, end.y - side.y * w1, end.z - side.z * w1, 0, 0.5f, c2},
               {end.x + side.x * w1, end.y + side.y * w1, end.z + side.z * w1, 1, 0.5f, c2}};
    r_strip(v, 6);
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

void truckdraw_draw_op(void) {
    PROF_BEGIN(pt);
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        if(t->absent) continue;
        V3 d = vsub(t->pos, r_cam.pos);
        float dist = vlen(d);
        if(!r_sphere_visible(t->pos, 3.5f)) continue;
        /* Full detail for the player's truck and anything very close. */
        int lod = (ti == 0 && dist < 18.0f) || dist < 9.0f ? 0 : dist < 50.0f ? 1 : 2;
        V3 R = t->ax[0], U = t->ax[1], F = t->ax[2];
        float mat[16];
        V3 origin = vsub(vsub(t->pos, vscale(U, t->spec.cog_y)), vscale(F, t->spec.cog_z));
        mat_axes(mat, R, U, F, origin);
#ifdef HAVE_TRUCK_MODELS
        EngLight l;
        l.L = v3(vdot(sun_dir, R), vdot(sun_dir, U), vdot(sun_dir, F));
        for(int i = 0; i < 3; i++) {
            l.sun[i] = pal->sun_k[i] * 1.1f;
            l.sky[i] = pal->sky_k[i];
            l.ambient[i] = pal->amb_k[i] * 1.2f;
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
        if(t->lights) {
            /* Full beam: whiter lenses, and the roof lights with them. */
            bool full = t->lights == LIGHTS_FULL;
            paint[RL_PAINT_HEADLIGHT] = full ? 0xffffffff : 0xfffff4d8;
            paint[RL_PAINT_TAILLIGHT] = t->brake > 0.2f ? 0xffff3020 : 0xffd81818;
            unlit = (1u << RL_PAINT_HEADLIGHT) | (1u << RL_PAINT_TAILLIGHT);
            if(full) {
                paint[RL_PAINT_AUX] = 0xfffff8e8;
                unlit |= 1u << RL_PAINT_AUX;
            }
        }
        else if(t->brake > 0.2f) {
            paint[RL_PAINT_TAILLIGHT] = 0xffff3020;
            unlit = 1u << RL_PAINT_TAILLIGHT;
        }
        l.paint = paint;
        l.ref = ref_col[t->model];
        l.unlit_mask = unlit;
        int used = light_part(&parts[t->model][RL_PART_BODY][lod], &l, lit_buf);
        if(lod < 2) {
            /* The glint: the sun's colour, as strong as its light. */
            V3 to = vsub(r_cam.pos, t->pos);
            V3 V = vnorm(v3(vdot(to, R), vdot(to, U), vdot(to, F)));
            V3 H = vnorm(vadd(l.L, V));
            float k[3];
            for(int i = 0; i < 3; i++) k[i] = 150.0f * pal->sun_k[i] * (((pal->sun_col >> (16 - 8 * i)) & 255) * (1.0f / 255));
            if(k[0] + k[1] + k[2] > 6.0f) sheen_part(&parts[t->model][RL_PART_BODY][lod], lit_buf, H, k);
        }
        draw_part(&parts[t->model][RL_PART_BODY][lod], mat, lit_buf);
        /* Wheels: steered, spinning, where the suspension puts them. Lit
           once for all four with the body's sun: a tyre is dark and round,
           and nobody sees its light turn with the spin. */
        EngLight lw = l;
        lw.unlit_mask = 0;
        uint32_t *lit_l = lit_buf + used;
        int used_l = light_part(&parts[t->model][RL_PART_WHEEL_L][lod], &lw, lit_l);
        uint32_t *lit_r = lit_l + used_l;
        int used_r = light_part(&parts[t->model][RL_PART_WHEEL_R][lod], &lw, lit_r);
        uint32_t *lit_a = lit_r + used_r;
        light_part(&parts[t->model][RL_PART_AXLE][lod], &lw, lit_a);
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
            /* The Kestrel's front suspension is independent: one beam. */
            for(int a = (t->model == RL_TRUCK_KESTREL ? 1 : 0); a < 2; a++) {
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
            static const uint32_t box_col[TRUCK_MODELS] = {0xffc8202a, 0xffe0601c, 0xff6a6862, 0xffc81a1e};
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
        float blen = fminf(full ? 26.0f : 15.0f, land / cosf(pitch) + 1.0f);
        float air_a = (0.05f + 0.3f * pal->dark + 0.3f * world_mist()) * (full ? 1.0f : 0.8f);
        r_hdr(h_beam);
        for(int i = 0; i < 2; i++) {
            V3 p = truck_to_world(t, v3(info->headlight[i][0], info->headlight[i][1], info->headlight[i][2]));
            beam_air(vmad(p, F, 0.1f), bdir, blen, full ? 2.6f : 1.8f, full ? 0xfffff8ec : 0xfffff0d0, air_a);
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
            r_billboard(tl, 0.35f, 0.35f, 0, 0, 0, 1, 1, color_alpha(0xffff2010, 0.6f));
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
        V3 nose_h = v3(nose.x, 0, nose.z);
        beam_patch(nose_h, fh, rh, nose.y + 1.5f, 1.0f, land * 1.25f, 1.6f, full ? 7.0f : 4.5f, full ? 0xfffff4d8 : 0xffffe8b0, a);
    }
}
