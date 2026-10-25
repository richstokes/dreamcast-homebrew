/* Kart and Chao rendering from the generated rigid-part model.

   Each part gets its own matrix (kart pose * pivot * part rotation). The sun
   is rotated into part space once, so per-vertex lighting is one dot
   product. Rival karts recolour paint slots: slot 1 is the body paint, slot 2
   the trim, slots 3/4 the Chao's body and tip colours, blended per vertex. */
#include "models.h"
#include "render.h"
#include "prof.h"
#include "world.h"
#include "models_data.h"
#include "texture_assets.h"

static uint32_t paint[MAX_KARTS][DK_MODEL_VERT_COUNT];
/* Model vertices, shared by every kart: position and lighting group. */
static KV model_kv[DK_MODEL_VERT_COUNT] __attribute__((aligned(32)));
/* Vertices of a mesh that share a normal and a paint (argb, slot, tip)
   light alike, so lighting runs once per such group and each vertex takes
   its group's colour. A mesh's groups are mesh_g0[mesh] onwards; each
   model vertex holds its group within its mesh. Each kart's lit group
   colours are refreshed once per frame and shared by both split-screen
   views. Groups are found from the model data at boot (models_init); if
   lighting ever depends on more than the normal and the paint, that must
   join the group key. */
static uint16_t mesh_g0[DK_MODEL_MESH_COUNT], mesh_gn[DK_MODEL_MESH_COUNT];
static uint16_t group_vert[DK_MODEL_VERT_COUNT];        /* one vertex of each group */
static float group_nrm[DK_MODEL_VERT_COUNT][3];
static uint32_t group_paint[MAX_KARTS][DK_MODEL_VERT_COUNT];
static uint32_t group_lit[MAX_KARTS][DK_MODEL_VERT_COUNT] __attribute__((aligned(32)));
static int group_count;
/* A mesh's vertices that differ only in normal or paint share a position,
   which r_mesh_kv transforms once: each mesh's distinct positions are
   mesh_p0[mesh] onwards. */
static float model_pos[DK_MODEL_VERT_COUNT][4] __attribute__((aligned(32)));
static uint16_t mesh_p0[DK_MODEL_MESH_COUNT], mesh_np[DK_MODEL_MESH_COUNT];
static uint16_t vert_pos[DK_MODEL_VERT_COUNT];   /* a vertex's position within its mesh */
/* Each mesh's triangles as strips for r_mesh_kv (format in render.h), the
   winding of every source triangle kept. Worst case is a strip per
   triangle. */
#define STRIP_DATA_MAX (DK_MODEL_INDEX_COUNT / 3 * 7 + DK_MODEL_MESH_COUNT)
static uint16_t strip_data[STRIP_DATA_MAX];
static uint16_t mesh_strips[DK_MODEL_MESH_COUNT];   /* first word in strip_data */
static uint32_t lit_frame[MAX_KARTS][DK_MODEL_MESH_COUNT];
static uint32_t frame_id = 1;
/* Kart part matrices, computed once per frame and shared by both views. */
static float part_mats[MAX_KARTS][DK_PART_COUNT][16];
static V3 part_light[MAX_KARTS][DK_PART_COUNT];
static uint32_t mats_frame[MAX_KARTS];
static const pvr_poly_hdr_t *h_model, *h_shadow, *h_underglow;
static float ref_luma1 = 1.0f, ref_luma2 = 1.0f;
static int far_lod = 1;   /* lod 3 when the model has a far level */
static float normals[DK_MODEL_VERT_COUNT][3];
static V3 light_dir;

static float luma(uint32_t c) {
    return (0.30f * ((c >> 16) & 255) + 0.59f * ((c >> 8) & 255) + 0.11f * (c & 255)) / 255.0f;
}

/* Does triangle t (indices tri[t*3..]) have the cyclic order (a, b, c)? */
static bool tri_is(const uint16_t *tri, int t, unsigned a, unsigned b, unsigned c) {
    const uint16_t *p = tri + t * 3;
    for(int r = 0; r < 3; r++)
        if(p[r] == a && p[(r + 1) % 3] == b && p[(r + 2) % 3] == c) return true;
    return false;
}

/* Greedy strip from triangle t started at rotation rot into out (vertex
   indices), taking unused triangles that continue it with the right
   winding; marks taken ones with stamp. Returns the strip's vertex count. */
static int grow_strip(const uint16_t *tri, int ntri, uint16_t *used, uint16_t stamp, int t, int rot,
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
        for(int u = 0; u < ntri && found < 0; u++) {
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

/* Strip each mesh's triangle list (once, at boot), trying the three
   rotations of each starting triangle and keeping the longest. */
static void build_strips(void) {
    static uint16_t used[DK_MODEL_INDEX_COUNT / 3], strip[R_MESH_STRIP_TRIS + 2];
    int o = 0, strips = 0, nsv = 0;
    for(int mi = 0; mi < DK_MODEL_MESH_COUNT; mi++) {
        const dk_mesh_t *m = &dk_model_meshes[mi];
        const uint16_t *tri = &dk_model_indices[m->first_index];
        int ntri = m->index_count / 3;
        mesh_strips[mi] = (uint16_t)o;
        memset(used, 0, sizeof(used));
        for(int t = 0; t < ntri; t++) {
            if(used[t]) continue;
            int best = 0, best_n = 0;
            for(int rot = 0; rot < 3; rot++) {
                /* Trial runs mark with stamp 2 + rot and are undone. */
                int n = grow_strip(tri, ntri, used, (uint16_t)(2 + rot), t, rot, strip);
                for(int u = 0; u < ntri; u++)
                    if(used[u] == 2 + rot) used[u] = 0;
                if(n > best_n) {
                    best_n = n;
                    best = rot;
                }
            }
            int n = grow_strip(tri, ntri, used, 1, t, best, strip);
#ifdef DK_XF_CHECK
            /* Every strip triangle must be an unclaimed source triangle,
               wound the same way. */
            extern uint32_t xf_checked, xf_mismatch;
            static uint8_t claimed[DK_MODEL_INDEX_COUNT / 3];
            if(t == 0) memset(claimed, 0, sizeof(claimed));
            for(int k = 0; k + 2 < n; k++) {
                unsigned a = strip[k + (k & 1)], b = strip[k + 1 - (k & 1)], c = strip[k + 2];
                int u = 0;
                while(u < ntri && (claimed[u] || !tri_is(tri, u, a, b, c))) u++;
                if(u < ntri) claimed[u] = 1;
                else xf_mismatch++;
                xf_checked++;
            }
#endif
            strip_data[o++] = (uint16_t)n;
            for(int k = 0; k < n; k++) {
                int v = m->first_vert + strip[k];
                strip_data[o++] = (uint16_t)(vert_pos[v] * 16);
                strip_data[o++] = (uint16_t)(model_kv[v].group * 4);
            }
            strips++;
            nsv += n;
        }
        strip_data[o++] = 0;
    }
    printf("DreamKarts models: %d strips, %d strip vertices for %d triangles\n", strips, nsv,
           DK_MODEL_INDEX_COUNT / 3);
}

/* Each mesh's distinct positions, and each vertex's among them. */
static void find_positions(void) {
    int np = 0;
    for(int mi = 0; mi < DK_MODEL_MESH_COUNT; mi++) {
        const dk_mesh_t *m = &dk_model_meshes[mi];
        mesh_p0[mi] = (uint16_t)np;
        for(int i = m->first_vert; i < m->first_vert + m->vert_count; i++) {
            const KV *v = &model_kv[i];
            int p = mesh_p0[mi];
            while(p < np && !(model_pos[p][0] == v->x && model_pos[p][1] == v->y && model_pos[p][2] == v->z)) p++;
            if(p == np) {
                model_pos[np][0] = v->x;
                model_pos[np][1] = v->y;
                model_pos[np][2] = v->z;
                model_pos[np][3] = 0;
                np++;
            }
            vert_pos[i] = (uint16_t)(p - mesh_p0[mi]);
        }
        mesh_np[mi] = (uint16_t)(np - mesh_p0[mi]);
    }
    printf("DreamKarts models: %d distinct positions for %d vertices\n", np, DK_MODEL_VERT_COUNT);
}

void models_init(void) {
    h_model = r_header(TEX_NONE, HF_OP);
    h_shadow = r_header(TEX_SHADOW, HF_TR);
    h_underglow = r_header(TEX_GLOW, HF_TR | HF_ADD);
    /* Reference brightness of the default body and trim paints. */
    float sum[2] = {0, 0};
    int n[2] = {0, 0};
    for(int i = 0; i < DK_MODEL_VERT_COUNT; i++) {
        int p = dk_model_verts[i].paint;
        if(p == 1 || p == 2) {
            sum[p - 1] += luma(dk_model_verts[i].argb);
            n[p - 1]++;
        }
    }
    if(n[0]) ref_luma1 = fmaxf(sum[0] / n[0], 0.05f);
    if(n[1]) ref_luma2 = fmaxf(sum[1] / n[1], 0.05f);
    for(int i = 0; i < DK_MODEL_VERT_COUNT; i++) {
        model_kv[i].x = dk_model_verts[i].x;
        model_kv[i].y = dk_model_verts[i].y;
        model_kv[i].z = dk_model_verts[i].z;
    }
    for(int i = 0; i < DK_MODEL_VERT_COUNT; i++) {
        normals[i][0] = dk_model_verts[i].nx * (1.0f / 127.0f);
        normals[i][1] = dk_model_verts[i].ny * (1.0f / 127.0f);
        normals[i][2] = dk_model_verts[i].nz * (1.0f / 127.0f);
    }
    group_count = 0;
    for(int mi = 0; mi < DK_MODEL_MESH_COUNT; mi++) {
        const dk_mesh_t *m = &dk_model_meshes[mi];
        mesh_g0[mi] = (uint16_t)group_count;
        for(int i = m->first_vert; i < m->first_vert + m->vert_count; i++) {
            const dk_mvert_t *v = &dk_model_verts[i];
            int g = mesh_g0[mi];
            for(; g < group_count; g++) {
                const dk_mvert_t *w = &dk_model_verts[group_vert[g]];
                if(v->nx == w->nx && v->ny == w->ny && v->nz == w->nz && v->argb == w->argb &&
                   v->paint == w->paint && v->tip == w->tip) break;
            }
            if(g == group_count) {
                group_vert[g] = (uint16_t)i;
                memcpy(group_nrm[g], normals[i], sizeof(group_nrm[g]));
                group_count++;
            }
            model_kv[i].group = (uint16_t)(g - mesh_g0[mi]);
        }
        mesh_gn[mi] = (uint16_t)(group_count - mesh_g0[mi]);
    }
    find_positions();
    build_strips();
    /* Key light from above, slightly in front of the karts' usual view. */
    light_dir = vnorm(v3(0.35f, 0.85f, 0.40f));
    for(int i = 0; i < DK_MODEL_MESH_COUNT; i++)
        if(dk_model_meshes[i].lod == 3) far_lod = 3;
    int meshes_max = 0;
    for(int i = 0; i < DK_MODEL_MESH_COUNT; i++)
        if(dk_model_meshes[i].vert_count > meshes_max) meshes_max = dk_model_meshes[i].vert_count;
    printf("DreamKarts models: %d verts (%d lit alike groups), %d tris, %d meshes (largest %d verts)\n",
           DK_MODEL_VERT_COUNT, group_count, DK_MODEL_INDEX_COUNT / 3, DK_MODEL_MESH_COUNT, meshes_max);
}

void models_paint_kart(int index) {
    const Kart *k = &karts[index];
    for(int i = 0; i < DK_MODEL_VERT_COUNT; i++) {
        const dk_mvert_t *v = &dk_model_verts[i];
        uint32_t c = v->argb;
        if(v->paint == 1 && k->paint1) {
            c = color_scale(k->paint1, luma(v->argb) / ref_luma1);
        }
        else if(v->paint == 2 && k->paint2) {
            c = color_scale(k->paint2, luma(v->argb) / ref_luma2);
        }
        else if((v->paint == 3 || v->paint == 4) && k->chao) {
            c = color_lerp(k->chao, k->chao_tip, (float)v->tip / 255.0f);
        }
        /* The track's light tint (moonlight at night) is baked in here. */
        unsigned r = (((c >> 16) & 255) * world_light[0]) >> 8, g = (((c >> 8) & 255) * world_light[1]) >> 8,
                 b = ((c & 255) * world_light[2]) >> 8;
        paint[index][i] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
    for(int g = 0; g < group_count; g++) group_paint[index][g] = paint[index][group_vert[g]];
}

static float smooth01(float x) {
    x = clampf(x, 0, 1);
    return x * x * (3.0f - 2.0f * x);
}

/* Emote rhythm: emote_bob advances 3.2 a second, so this is 2.5 waves a
   second. */
static float emote_phase(const Kart *k) {
    return k->emote_bob * (DK_TAU * 2.5f / 3.2f);
}

/* How far the Chao bounces up in its seat while cheering (arms flap up). */
static float cheer_lift(const Kart *k) {
    if(k->cheer_t <= 0) return 0;
    return 0.045f * smooth01(k->cheer_t * 3.0f) * (0.5f + 0.5f * sinf(emote_phase(k) * 1.1f));
}

/* Chao body sway and bob about its seat; the arms hang off it. */
static void chao_body_local(float *m, const Kart *k, float lift) {
    const float *bp = dk_part_pivot[DK_PART_CHAO_BODY];
    m4_rot_z(m, -k->head_tilt * 0.35f);
    m[12] = bp[0];
    m[13] = bp[1] + 0.02f * sinf(k->emote_bob * 2.0f) + lift;
    m[14] = bp[2];
}

/* An arm (side 0 left, 1 right) raised by arm_up: the resting arm, shoulder
   to the hand on the wheel, swings toward a pose out to the side, theta
   above horizontal, and stretches (cartoon-style) so the hand clears the
   big head; the hand grows a little so it reads from the chase camera.
   A wave sweeps one arm up and down; a cheer flaps both arms. With
   hand set, this is the matrix for the hand at the end of the arm. */
static void arm_local(float *out, const Kart *k, int side, bool hand, float lift) {
    const float *sp = dk_part_pivot[DK_PART_ARM_L + side], *hp = dk_part_pivot[DK_PART_HAND_L + side];
    const float *bp = dk_part_pivot[DK_PART_CHAO_BODY];
    float u = smooth01(k->arm_up[side]);
    V3 rest = v3(hp[0] - sp[0], hp[1] - sp[1], hp[2] - sp[2]);
    V3 a = vnorm(rest);
    float m[16], body[16];
    m4_identity(m);
    float stretch = 1.0f + 0.5f * u;
    if(u > 0) {
        float ph = emote_phase(k);
        float th = k->cheer_t > 0 ? 0.45f + 0.45f * sinf(ph * 1.1f) : 0.70f + 0.42f * sinf(ph);
        V3 b = vnorm(v3(side ? cosf(th) : -cosf(th), sinf(th), 0.05f));
        float ang = acosf(clampf(vdot(a, b), -1.0f, 1.0f));
        m4_rot_axis(m, vnorm(vcross(a, b)), ang * u);
    }
    float t[16];
    m4_identity(t);
    if(hand) {
        t[0] = t[5] = t[10] = 1.0f + 0.35f * u;
        t[12] = rest.x * stretch;
        t[13] = rest.y * stretch;
        t[14] = rest.z * stretch;
    }
    else {
        /* Stretch along the resting arm: I + (stretch - 1) a a^T. */
        float av[3] = {a.x, a.y, a.z};
        for(int c = 0; c < 3; c++)
            for(int r = 0; r < 3; r++) t[c * 4 + r] += (stretch - 1.0f) * av[r] * av[c];
    }
    m4_mul(m, m, t);
    m[12] += sp[0] - bp[0];
    m[13] += sp[1] - bp[1];
    m[14] += sp[2] - bp[2];
    chao_body_local(body, k, lift);
    m4_mul(out, body, m);
}

static void part_matrix(float *out, const float *kart, int part, const Kart *k) {
    float local[16], rot[16];
    m4_identity(local);
    const float *pv = dk_part_pivot[part];
    float lift = cheer_lift(k);
    switch(part) {
        case DK_PART_WHEEL_FL:
        case DK_PART_WHEEL_FR: {
            float steer[16];
            m4_rot_y(steer, k->steer_vis * 0.55f);
            m4_rot_x(rot, -k->wheel_spin);
            m4_mul(local, steer, rot);
            break;
        }
        case DK_PART_WHEEL_RL:
        case DK_PART_WHEEL_RR:
            m4_rot_x(local, -k->wheel_spin);
            break;
        case DK_PART_STEER:
            m4_rot_axis(local, v3(dk_steer_axis[0], dk_steer_axis[1], dk_steer_axis[2]), -k->steer_vis * 1.4f);
            break;
        case DK_PART_CHAO_BODY:
            chao_body_local(out, k, lift);
            m4_mul(out, kart, out);
            return;
        case DK_PART_CHAO_HEAD: {
            /* A cheering Chao throws its head back; a waving one leans
               away from its raised arm. */
            float nod[16];
            float lean = 0.22f * (smooth01(k->arm_up[1]) - smooth01(k->arm_up[0]));
            m4_rot_z(local, -k->head_tilt + (k->cheer_t > 0 ? 0.0f : lean));
            m4_rot_x(nod, 0.06f * sinf(k->emote_bob * 1.3f) - k->pitch * 1.5f + 0.2f * smooth01(k->cheer_t * 3.0f));
            m4_mul(local, local, nod);
            local[13] += lift;
            break;
        }
        case DK_PART_EMOTE:
            m4_rot_y(local, k->emote_bob * 0.8f);
            local[13] += 0.05f * sinf(k->emote_bob * 2.2f) + lift;
            break;
        case DK_PART_ARM_L:
        case DK_PART_ARM_R:
            arm_local(out, k, part - DK_PART_ARM_L, false, lift);
            m4_mul(out, kart, out);
            return;
        case DK_PART_HAND_L:
        case DK_PART_HAND_R: {
            int side = part - DK_PART_HAND_L;
            if(k->arm_up[side] > 0.0f) {
                arm_local(out, k, side, true, lift);
                m4_mul(out, kart, out);
                return;
            }
            /* On the wheel: turn with it about the steering column. */
            const float *st = dk_part_pivot[DK_PART_STEER];
            m4_rot_axis(local, v3(dk_steer_axis[0], dk_steer_axis[1], dk_steer_axis[2]), -k->steer_vis * 1.4f);
            float off[16];
            m4_identity(off);
            off[12] = pv[0] - st[0];
            off[13] = pv[1] - st[1];
            off[14] = pv[2] - st[2];
            m4_mul(local, local, off);
            pv = st;
            break;
        }
        default:
            break;
    }
    local[12] += pv[0];
    local[13] += pv[1];
    local[14] += pv[2];
    m4_mul(out, kart, local);
}

/* The pose from the physics. The chassis (wheels) sits on the road:
   banking, slope and squash; the body on top of it also leans in corners
   and pitches under power, on its springs, so a lean never buries the
   inner wheels. */
#define TRICK_PIVOT 0.5f   /* height in the body the barrel roll turns about */
static void kart_pose(float *m, const Kart *k, bool body) {
    float sq = k->squash;
    m4_translate_rotate(m, k->pos, k->heading + k->vis_yaw, k->pitch + (body ? k->nose : 0.0f), k->roll + (body ? k->lean : 0.0f));
    if(k->trick_a != 0) {
        /* The barrel roll turns the whole kart about its nose axis through
           a point up in the body, so the wheels sweep round it. */
        float rz[16];
        m4_rot_z(rz, k->trick_a);
        rz[12] = -rz[4] * TRICK_PIVOT;
        rz[13] = TRICK_PIVOT - rz[5] * TRICK_PIVOT;
        m4_mul(m, m, rz);
    }
    m4_scale_apply(m, 1.0f - sq * 0.45f, 1.0f + sq, 1.0f - sq * 0.45f);
}

/* Height of the road as drawn under world point p, near kart k. */
static float surface_under(const Kart *k, V3 p) {
    float dx = p.x - k->pos.x, dz = p.z - k->pos.z;
    return world_surface_y(k->tp.s + dx * k->tp.tangent.x + dz * k->tp.tangent.z,
                           k->tp.d + dx * k->tp.right.x + dz * k->tp.right.z);
}

/* Is the kart over road that is drawn (not off an open edge or over a
   jump's gap, where it is falling or flying anyway)? */
static bool over_road(const Kart *k) {
    return fabsf(k->tp.d) < 20.0f && !track_off_edge(&k->tp);
}

/* How far the lowest tyre's contact point in pose m sits under the drawn
   road (negative: that much clear of it). The foot of each tyre is at its
   pivot's x, z and y = 0 in kart space. */
static float tyre_sink(const float *m, const Kart *k) {
    float worst = -1e9f;
    for(int w = 0; w < 4; w++) {
        const float *p = dk_part_pivot[DK_PART_WHEEL_FL + w];
        V3 foot = v3(m[0] * p[0] + m[8] * p[2] + m[12], m[1] * p[0] + m[9] * p[2] + m[13], m[2] * p[0] + m[10] * p[2] + m[14]);
        worst = fmaxf(worst, surface_under(k, foot) - foot.y);
    }
    return worst;
}

/* The body's (or with chassis the wheels') world matrix. */
static void kart_matrix(float *m, const Kart *k, bool chassis) {
    kart_pose(m, k, !chassis);
}

float models_kart_sink(const Kart *k) {
    if(!over_road(k)) return -1e9f;
    float m[16];
    kart_pose(m, k, false);
    return tyre_sink(m, k);
}

static int emote_variant(const Kart *k) {
    if(k->finished && k->place == 1) return 3;
    if(k->boost_t > 0) return 2;
    return 1;
}

static void update_kart_mats(int index) {
    if(mats_frame[index] == frame_id) return;
    mats_frame[index] = frame_id;
    const Kart *k = &karts[index];
    float km[16], kc[16];
    kart_matrix(km, k, false);
    kart_matrix(kc, k, true);
    for(int part = 0; part < DK_PART_COUNT; part++) {
        float *pm = part_mats[index][part];
        bool wheel = part >= DK_PART_WHEEL_FL && part <= DK_PART_WHEEL_RR;
        part_matrix(pm, wheel ? kc : km, part, k);
        /* Sun into part space (columns are the part axes). */
        V3 cx = v3(pm[0], pm[1], pm[2]), cy = v3(pm[4], pm[5], pm[6]), cz = v3(pm[8], pm[9], pm[10]);
        part_light[index][part] = v3(vdot(vnorm(cx), light_dir), vdot(vnorm(cy), light_dir), vdot(vnorm(cz), light_dir));
    }
}

/* lod 2 = split-screen "own kart": full-detail body and Chao, the simple
   wheels, and no steering wheel (hidden behind the Chao from the chase
   camera). */
static bool mesh_in_lod(const dk_mesh_t *m, int lod) {
    if(lod != 2) return m->lod == lod;
    switch(m->part) {
        case DK_PART_WHEEL_FL: case DK_PART_WHEEL_FR: case DK_PART_WHEEL_RL: case DK_PART_WHEEL_RR:
            return m->lod == 1;
        case DK_PART_STEER:
            return false;
        default:
            return m->lod == 0;
    }
}

static void draw_kart_lod(int index, int lod) {
    const Kart *k = &karts[index];
    PROF_BEGIN(pm);
    update_kart_mats(index);
    PROF_LAP(PS_MATS, pm);
    int variant = emote_variant(k);
    int last_part = -1;
    V3 lp = light_dir;
    r_hdr(h_model);
    for(int mi = 0; mi < DK_MODEL_MESH_COUNT; mi++) {
        const dk_mesh_t *m = &dk_model_meshes[mi];
        if(!mesh_in_lod(m, lod)) continue;
        if(m->variant != 0 && m->variant != variant) continue;
        /* The far model's arms only show raised. */
        if((m->flags & 2) && (m->part == DK_PART_ARM_L || m->part == DK_PART_ARM_R) &&
           k->arm_up[m->part - DK_PART_ARM_L] <= 0.0f) continue;
        if(m->part != last_part) {
            r_load_model(part_mats[index][m->part]);
            lp = part_light[index][m->part];
            last_part = m->part;
        }
        int nv = m->vert_count, g0 = mesh_g0[mi], ng = mesh_gn[mi];
        uint32_t *lit = &group_lit[index][g0];
        if(lit_frame[index][mi] != frame_id) {
            PROF_RESTART(pm);
            lit_frame[index][mi] = frame_id;
            bool unlit = m->flags & 1;
            /* Lighting in 8.8 fixed point: one float->int per group. */
            float lx = lp.x * 158.7f, ly = lp.y * 158.7f + 25.6f, lz = lp.z * 158.7f;
            const uint32_t *col = &group_paint[index][g0];
            const float (*nrm)[3] = &group_nrm[g0];
            for(int i = 0; i < ng; i++) {
                uint32_t c = col[i];
                if(unlit) {
                    lit[i] = c;
                    continue;
                }
                float d = nrm[i][0] * lx + nrm[i][1] * ly + nrm[i][2] * lz;
                int li = 133 + (d > 0 ? (int)d : 0);
                unsigned r = (((c >> 16) & 255) * li) >> 8, g = (((c >> 8) & 255) * li) >> 8, b = ((c & 255) * li) >> 8;
                lit[i] = 0xff000000u | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
            }
            PROF_LAP(PS_LIGHT, pm);
            PROF_COUNT(PN_LVERTS, ng);
        }
        const KMesh km = {&model_kv[m->first_vert], nv, &dk_model_indices[m->first_index], m->index_count,
                          &model_pos[mesh_p0[mi]], mesh_np[mi], &strip_data[mesh_strips[mi]]};
        r_mesh_kv(&km, lit);
    }
}

void models_draw_kart(int index) {
    const Kart *k = &karts[index];
    V3 c = vadd(k->pos, v3(0, 0.8f, 0));
    if(!r_sphere_visible(c, 1.8f)) return;
    float d = vlen(vsub(c, r_cam.pos));
    draw_kart_lod(index, d > 38.0f ? 1 : 0);
}

/* Kart detail is chosen by distance from each view's camera, with
   hysteresis so karts don't flick between models at a threshold:
     lod 0  full detail
     lod 2  "own kart" in split screen: full body and Chao, simple wheels,
            no steering wheel (hidden behind the Chao from the chase camera)
     lod 1  330-triangle model built to match lod 0 from the chase camera
     lod 3  ~150-triangle far model
   Single view: lod 0 within ~16 m, lod 1 to ~42 m, lod 3 beyond. Split
   screen draws every kart twice: each view's own kart uses lod 2 and every
   other kart lod 3 (measured on hardware: 10 dropped frames a race, versus
   ~140 with lod 1 for nearby rivals). */
static const float lod_in[2][2] = {{16.0f, 42.0f}, {0.0f, 0.0f}};   /* [split][near/mid] */
static const float lod_out[2][2] = {{19.0f, 46.0f}, {0.0f, 0.0f}};
static uint8_t lod_state[MAX_PLAYERS][MAX_KARTS];   /* 0 near, 1 mid, 2 far */

void models_begin_frame(void) {
    frame_id++;
}

void models_draw_karts(int view_kart) {
    int view = 0;
    for(int p = 0; p < game.num_players; p++)
        if(game.pl[p].kart == view_kart) view = p;
    /* The kart select shows each player's kart close up: full detail. */
    int sp = world_split && game.state != ST_KARTS ? 1 : 0;
    /* A zoomed-in (TV) camera sees a far kart as if it were near. */
    float zoom = r_cam.fov < 1.2f ? r_cam.fov / 1.3f : 1.0f;
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->gone) continue;
        V3 d = vsub(k->pos, r_cam.pos);
        float dist = sqrtf(vdot(d, d)) * zoom;
        uint8_t *st = &lod_state[view][i];
        /* Step out past the outer edge, step in past the inner edge. */
        while(*st < 2 && dist > lod_out[sp][*st]) (*st)++;
        while(*st > 0 && dist < lod_in[sp][*st - 1]) (*st)--;
        V3 c = vadd(k->pos, v3(0, 0.8f, 0));
        if(!r_sphere_visible(c, 1.8f)) continue;
        int lod = *st == 0 ? 0 : *st == 1 ? 1 : far_lod;
        if(i == view_kart) lod = sp ? 2 : 0;
        draw_kart_lod(i, lod);
    }
}

/* A quad on the road under kart k, centred at c (x, z), spanning +-f and
   +-r, each corner resting on the road as drawn there, up metres above
   it, so it follows the deck's triangles rather than cutting through them
   (or hanging in the air) where the road twists or banks. */
static void road_quad(const Kart *k, V3 c, V3 f, V3 r, float up, uint32_t col) {
    RV q[4];
    for(int i = 0; i < 4; i++) {
        float sf = i < 2 ? 1.0f : -1.0f, sr = (i & 1) ? 1.0f : -1.0f;
        V3 p = v3(c.x + sf * f.x + sr * r.x, 0, c.z + sf * f.z + sr * r.z);
        p.y = surface_under(k, p) + up;
        q[i] = (RV){p.x, p.y, p.z, (float)(i & 1), i < 2 ? 0.0f : 1.0f, col};
    }
    r_strip(q, 4);
}

void models_draw_shadows(void) {
    r_load_vp();
    r_hdr(h_shadow);
    V3 sun_off = v3(world_sun_dir.x, 0, world_sun_dir.z);
    float sl = sqrtf(vdot(sun_off, sun_off));
    sun_off = sl > 0.01f ? vscale(sun_off, 0.12f / sl) : v3(0, 0, 0);
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->gone || !r_sphere_visible(k->pos, 2.0f)) continue;
        /* Cast onto the road below: the floor under a jump's gap is y = 0
           (in the sky there is nothing there). */
        if(!over_road(k)) continue;
        float h = fmaxf(k->pos.y - world_surface_y(k->tp.s, k->tp.d), 0.0f);
        if(h > 12.0f) continue;
        float a = clampf(0.80f - h * 0.40f, 0.15f, 0.80f);
        float s = 1.0f + h * 0.25f;
        V3 f = vscale(heading_fwd(k->heading + k->vis_yaw), 1.2f * s), r = vscale(heading_right(k->heading + k->vis_yaw), 0.88f * s);
        /* The kart-shaped shadow leans a touch away from the sun; any more
           and it swings visibly round the kart as it turns. */
        road_quad(k, v3(k->pos.x - sun_off.x, 0, k->pos.z - sun_off.z), f, r, 0.04f, color_alpha(0xff000000, a));
    }
    if(!world_night) return;
    /* Neon underglow: the road under each kart lights its belly, cyan to magenta across. */
    r_hdr(h_underglow);
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->gone || !over_road(k) || !r_sphere_visible(k->pos, 3.0f)) continue;
        float h = fmaxf(k->pos.y - world_surface_y(k->tp.s, k->tp.d), 0.0f);
        if(h > 4.0f) continue;
        float a = 0.55f * (1.0f - h * 0.25f);
        V3 f = vscale(heading_fwd(k->heading + k->vis_yaw), 2.3f), r = vscale(heading_right(k->heading + k->vis_yaw), 1.8f);
        road_quad(k, k->pos, f, r, 0.06f, color_alpha(world_lane_color(k->tp.d), a));
    }
}

static V3 kart_point(const Kart *k, V3 local, bool chassis) {
    float m[16];
    kart_matrix(m, k, chassis);
    return v3(m[0] * local.x + m[4] * local.y + m[8] * local.z + m[12],
              m[1] * local.x + m[5] * local.y + m[9] * local.z + m[13],
              m[2] * local.x + m[6] * local.y + m[10] * local.z + m[14]);
}

V3 models_exhaust_world(const Kart *k, int which) {
    const float *e = dk_exhaust_pos[which & 1];
    return kart_point(k, v3(e[0], e[1], e[2]), false);
}

V3 models_kart_point_world(const Kart *k, V3 local) {
    return kart_point(k, local, false);
}

V3 models_wheel_world(const Kart *k, int wheel) {
    const float *p = dk_part_pivot[DK_PART_WHEEL_FL + wheel];
    return kart_point(k, v3(p[0], 0.02f, p[2]), true);
}
