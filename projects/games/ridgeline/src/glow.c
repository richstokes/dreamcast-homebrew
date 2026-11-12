/* Ridgeline: the campfire's and the lanterns' light (glow.h).

   The light on the ground is a fan of rings laid on the terrain about
   each place, additive, brightest under the light and gone by its reach;
   every vertex is weighted once (nothing here moves) and only scaled by
   the group's flicker each frame. A lantern is a soft glow round a hot
   point, held a few pixels wide from far off so the lookout shows from
   the valley. A lit window is a pane of the same warm light laid on the
   glass. Nothing is drawn by day. */
#include "glow.h"
#include "campfire.h"
#include "world.h"
#include "terrain.h"

Glow glows[GLOW_MAX];
int glow_n;
float glow_level[GLOW_GROUPS];
V3 glow_c[GLOW_GROUPS];
float glow_r[GLOW_GROUPS];
static float glow_t;
static const pvr_poly_hdr_t *h_add;

#define RINGS 10
#define SPOKES 16
#define LANTERN_FAR 420.0f
#define GROUND_K 0.55f       /* how much of a light shows on the ground and the grass */
static const float ring_t[RINGS] = {0, 0.035f, 0.075f, 0.13f, 0.2f, 0.3f, 0.425f, 0.575f, 0.75f, 1.0f};
typedef struct {
    V3 c;
    float reach;
    V3 p[RINGS][SPOKES];
    float k[RINGS][SPOKES];
} Fan;
static Fan fans[GLOW_GROUPS];

/* A trailer's lit windows: a quad each, just clear of the glass. */
#define PANES 16
typedef struct {
    V3 q[4];
    uint8_t group;
} Pane;
static Pane panes[PANES];
static int pane_n;

static void pane(const lvl_prop_t *p, int group, float side, float along, float y, float half_w, float half_h) {
    if(pane_n >= PANES) return;
    float sy = sinf(p->yaw), cy = cosf(p->yaw);
    V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
    Pane *pn = &panes[pane_n++];
    pn->group = (uint8_t)group;
    for(int k = 0; k < 4; k++) {
        float a = (k & 1 ? half_w : -half_w), h = (k & 2 ? -half_h : half_h);
        /* side 0: the window in the end wall, toward the hitch. */
        V3 at = side != 0 ? v3(r.x * side + f.x * (along + a), 0, r.z * side + f.z * (along + a))
                          : v3(f.x * along + r.x * a, 0, f.z * along + r.z * a);
        pn->q[k] = v3(p->x + at.x, p->y + y + h, p->z + at.z);
    }
}

/* The lookout's lanterns from its base: one under each corner of the
   roof, a bigger one hung under the deck. */
static const V3 tower_lanterns[5] = {{-2.25f, 7.72f, -2.25f}, {2.25f, 7.72f, -2.25f}, {2.25f, 7.72f, 2.25f},
                                     {-2.25f, 7.72f, 2.25f}, {0, 5.5f, 0}};

void glow_place(void) {
    glow_n = pane_n = 0;
    V3 f;
    campfire_place();
    for(int k = 0; k < CAMPFIRES; k++)
        if(campfire_lit(k, &f))
            glows[glow_n++] = (Glow){v3(f.x, f.y + 0.5f, f.z), 20.0f, 5.0f, 1.0f, (uint8_t)(GLOW_FIRE + k), false, 0, 0, false};
    /* The first lookout, cabin and picnic table are the ones lit. */
    bool tower = false, cabin = false, table = false;
    int campers = 0, camps = 0;
    for(int i = 0; i < LVL_PROPS; i++) {
        const lvl_prop_t *p = &lvl_props[i];
        float sy = sinf(p->yaw), cy = cosf(p->yaw);
        if(p->kind == PROP_TOWER && !tower) {
            tower = true;
            for(int k = 0; k < 5; k++) {
                V3 q = v3(p->x + tower_lanterns[k].x, p->y + tower_lanterns[k].y, p->z + tower_lanterns[k].z);
                glows[glow_n++] = k < 4 ? (Glow){q, 9.0f, 2.2f, 0.55f, GLOW_TOWER, true, 0.09f, 0.42f, false}
                                        : (Glow){q, 16.0f, 4.5f, 1.3f, GLOW_TOWER, true, 0.13f, 0.35f, false};
            }
        }
        else if(p->kind == PROP_CABIN && !cabin) {
            /* Under the eave on the side the trucks start from, toward
               the north end of the wall. */
            cabin = true;
            V3 q = v3(p->x + cy * 4.5f + sy * 1.4f, p->y + 2.45f, p->z - sy * 4.5f + cy * 1.4f);
            glows[glow_n++] = (Glow){q, 14.0f, 4.0f, 1.1f, GLOW_CABIN, true, 0.12f, 0.33f, false};
        }
        else if(p->kind == PROP_RAMADA && !table) {
            /* Hung under the shade's roof, over one end of the table. */
            table = true;
            V3 q = v3(p->x + cy * 0.9f, p->y + 2.2f, p->z - sy * 0.9f);
            glows[glow_n++] = (Glow){q, 12.0f, 3.2f, 0.95f, GLOW_TABLE, true, 0.11f, 0.22f, false};
        }
        else if(p->kind == PROP_CAMPER) {
            /* Someone is in three trailers of every five. A lantern hung
               under the awning, over the door; light in the long window
               and the one by the hitch, and in the small one for some. */
            int k = campers++ % 5;
            if(k == 1 || k == 4 || camps >= GLOW_CAMPS) continue;
            int g = GLOW_CAMP + camps++;
            V3 q = v3(p->x + cy * 2.5f + sy * 0.3f, p->y + 1.84f, p->z - sy * 2.5f + cy * 0.3f);
            glows[glow_n++] = (Glow){q, 13.0f, 3.6f, 1.0f, (uint8_t)g, true, 0.11f, 0.24f, false};
            for(int side = -1; side <= 1; side += 2) {
                pane(p, g, side * 1.2f, 1.2f, 2.0f, 0.58f, 0.23f);
                if(k != 2) pane(p, g, side * 1.2f, -1.5f, 2.0f, 0.36f, 0.23f);
            }
            pane(p, g, 0, 2.95f, 2.0f, 0.66f, 0.23f);
        }
        else if(p->kind == PROP_TABLE && !table) {
            /* Stood on one end of the table. */
            table = true;
            V3 q = v3(p->x + cy * 0.5f, p->y + 0.8f + 0.13f, p->z - sy * 0.5f);
            glows[glow_n++] = (Glow){q, 9.0f, 2.4f, 0.75f, GLOW_TABLE, true, 0.1f, 0, false};
        }
    }
    /* Where the top has no lookout, a lantern on a post marks it. */
    if(!tower) {
        V3 s = v3(LEVEL_SUMMIT_X - 6.0f, 0, LEVEL_SUMMIT_Z - 8.0f);
        s.y = terrain_height(s.x, s.z) + 2.3f;
        glows[glow_n++] = (Glow){s, 16.0f, 4.5f, 1.2f, GLOW_TOWER, true, 0.13f, 0.3f, true};
    }
    /* A post beside the east end of the start grid. */
    const float *sp = lvl_spawns[LVL_SPAWNS - 1];
    V3 q = v3(sp[0] + 4.6f, 0, sp[2] + 1.5f);
    q.y = terrain_height(q.x, q.z) + 2.3f;
    glows[glow_n++] = (Glow){q, 14.0f, 4.0f, 1.0f, GLOW_POST, true, 0.12f, 0.3f, true};
    for(int g = 0; g < GLOW_GROUPS; g++) {
        V3 c = v3(0, 0, 0);
        int n = 0;
        for(int i = 0; i < glow_n; i++)
            if(glows[i].group == g) {
                c = vadd(c, glows[i].p);
                n++;
            }
        glow_c[g] = n ? vscale(c, 1.0f / n) : c;
        glow_r[g] = 0;
        for(int i = 0; i < glow_n; i++)
            if(glows[i].group == g) glow_r[g] = fmaxf(glow_r[g], vlen(vsub(glows[i].p, glow_c[g])) + glows[i].reach);
    }
}

void glow_init(void) {
    h_add = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
    for(int g = 0; g < GLOW_GROUPS; g++) {
        Fan *fan = &fans[g];
        fan->c = glow_c[g];
        fan->reach = 0;
        for(int i = 0; i < glow_n; i++)
            if(glows[i].group == g) fan->reach = fmaxf(fan->reach, glows[i].reach);
        for(int r = 0; r < RINGS; r++)
            for(int s = 0; s < SPOKES; s++) {
                float a = ENG_TAU * (s + 0.5f * (r & 1)) / SPOKES, rad = ring_t[r] * fan->reach;
                V3 n, p = v3(fan->c.x + sinf(a) * rad, 0, fan->c.z + cosf(a) * rad);
                p.y = terrain_height_n(p.x, p.z, &n);
                float sum = 0;
                for(int i = 0; i < glow_n && r < RINGS - 1; i++) {
                    const Glow *gl = &glows[i];
                    if(gl->group != g) continue;
                    V3 to = vsub(gl->p, p);
                    /* Facing a point a little above the light, so flat
                       ground further out still catches some of it. */
                    V3 up = v3(to.x, to.y + 1.7f, to.z);
                    sum += glow_falloff(gl, vdot(to, to)) * clampf(0.35f + 0.65f * vdot(n, vnorm(up)), 0, 1);
                }
                fan->k[r][s] = sum;
                /* Lifted clear of the ground, more where the fan is
                   coarse (and by then nearly nothing). */
                p.y += 0.06f + 0.03f * rad;
                fan->p[r][s] = p;
            }
    }
}

float glow_light(V3 p, V3 *from) {
    float sum = 0;
    V3 dir = v3(0, 0, 0);
    for(int i = 0; i < glow_n; i++) {
        const Glow *g = &glows[i];
        float lv = glow_level[g->group];
        if(lv <= 0) continue;
        V3 d = vsub(g->p, p);
        float d2 = vdot(d, d);
        if(d2 >= g->reach * g->reach) continue;
        float w = lv * glow_falloff(g, d2);
        sum += w;
        dir = vmad(dir, d, w / sqrtf(fmaxf(d2, 1e-4f)));
    }
    if(from && sum > 0) *from = vnorm(dir);
    return sum;
}

bool glow_near(V3 p, float r) {
    for(int g = 0; g < GLOW_GROUPS; g++) {
        if(glow_level[g] < 0.02f) continue;
        float dx = p.x - glow_c[g].x, dz = p.z - glow_c[g].z, reach = glow_r[g] + r;
        if(dx * dx + dz * dz < reach * reach) return true;
    }
    return false;
}

uint32_t glow_tint(uint32_t col, V3 p) {
    float w = glow_light(p, NULL) * GROUND_K;
    if(w < 0.01f) return col;
    if(w > 1.0f) w = 1.0f;
    int r = (int)((col >> 16) & 255) + (int)(255.0f * w), g = (int)((col >> 8) & 255) + (int)(140.0f * w),
        b = (int)(col & 255) + (int)(52.0f * w);
    return (col & 0xff000000u) | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 |
           (uint32_t)(b > 255 ? 255 : b);
}

void glow_update(float dt) {
    if(game_frozen()) return;
    glow_t += dt;
    /* The fire sets its own level (campfire.c); a lantern burns steadier,
       each place's a little out of step with the others. */
    float dark = clampf(pal->dark * 1.15f, 0, 1);
    for(int g = GLOW_TOWER; g < GLOW_GROUPS; g++) {
        float t = glow_t + g * 2.3f;
        glow_level[g] = dark * (0.95f + 0.04f * sinf(t * 6.1f) * sinf(t * 1.7f) + 0.02f * sinf(t * 17.0f));
    }
}

static void sprite(V3 p, float s, uint32_t col) {
    V3 rx = vscale(r_cam.right, s), ry = vscale(r_cam.up, s);
    RV q[4] = {{p.x - rx.x + ry.x, p.y - rx.y + ry.y, p.z - rx.z + ry.z, 0, 0, col},
               {p.x + rx.x + ry.x, p.y + rx.y + ry.y, p.z + rx.z + ry.z, 1, 0, col},
               {p.x - rx.x - ry.x, p.y - rx.y - ry.y, p.z - rx.z - ry.z, 0, 1, col},
               {p.x + rx.x - ry.x, p.y + rx.y - ry.y, p.z + rx.z - ry.z, 1, 1, col}};
    r_strip(q, 4);
}

void glow_draw_tr(void) {
    bool hdr = false;
    for(int g = 0; g < GLOW_GROUPS; g++) {
        float lv = glow_level[g];
        if(lv < 0.02f) continue;
        V3 dc = vsub(glow_c[g], r_cam.pos);
        float dist = vlen(dc);
        if(dist > LANTERN_FAR || !r_sphere_visible(glow_c[g], glow_r[g])) continue;
        /* The fog takes it with everything else (this list isn't fogged). */
        float fog = clampf(1.0f - dist / world_fog_far(), 0, 1);
        if(fog <= 0) continue;
        if(!hdr) {
            r_load_vp();
            r_hdr(h_add);
            hdr = true;
        }
        if(dist < 140.0f) {
            const Fan *fan = &fans[g];
            static const uint32_t warm = 0xffff8c34;
            uint32_t col[RINGS][SPOKES];
            for(int r = 0; r < RINGS; r++)
                for(int s = 0; s < SPOKES; s++) col[r][s] = color_alpha(warm, clampf(fan->k[r][s] * lv * GROUND_K * fog, 0, 1));
            for(int r = 0; r + 1 < RINGS; r++) {
                RV st[2 * (SPOKES + 1)];
                for(int s = 0; s <= SPOKES; s++) {
                    int m = s % SPOKES;
                    V3 a = fan->p[r][m], b = fan->p[r + 1][m];
                    st[2 * s] = (RV){a.x, a.y, a.z, 0.5f, 0.5f, col[r][m]};
                    st[2 * s + 1] = (RV){b.x, b.y, b.z, 0.5f, 0.5f, col[r + 1][m]};
                }
                r_strip_bounded(st, 2 * (SPOKES + 1), fan->c, fan->reach + 6.0f);
            }
        }
        for(int i = 0; i < glow_n; i++) {
            const Glow *gl = &glows[i];
            if(gl->group != g || !gl->lantern) continue;
            float d = vlen(vsub(gl->p, r_cam.pos));
            sprite(gl->p, fmaxf(0.75f * gl->k + 0.25f, d * 0.009f), color_alpha(0xffff8a30, 0.3f * lv * fog));
            sprite(gl->p, fmaxf(0.16f, d * 0.0035f), color_alpha(0xffffe2a0, 0.9f * lv * fog));
        }
        if(g >= GLOW_CAMP && dist < 260.0f) {
            /* (the middle of the glow's texture: all of the colour) */
            uint32_t col = color_alpha(0xffffc470, 0.85f * lv * fog);
            for(int i = 0; i < pane_n; i++) {
                const Pane *pn = &panes[i];
                if(pn->group != g) continue;
                RV q[4];
                for(int k = 0; k < 4; k++) q[k] = (RV){pn->q[k].x, pn->q[k].y, pn->q[k].z, 0.5f, 0.5f, col};
                r_strip(q, 4);
            }
        }
    }
}
