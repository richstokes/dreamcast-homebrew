/* Night Meet: Free Roam's parking area, after Daikoku PA in Yokohama. A
   flat man-made island in the bay at night, a service road round a big
   lot of painted bays under sodium lamps, the expressway looping round
   the south and west sides on concrete pillars and an overpass crossing
   high over the north end, a block of service bays, a tower and a
   clubhouse along the south side with neon over them, trucks and
   containers parked at the east end, bleachers for whoever came to
   watch, cherry trees in planters along the quay, the city's towers on
   the mainland behind and a lit suspension bridge away across the water.

   Everything is borrowed: Apple Valley's light poles, bleachers,
   trailers, containers, tyre stacks and canopies, Tsudoken's pit garages,
   tower and cherry trees, Neon Skyway's towers, neon signs and running
   lights, Emerald Beach's start gantry. The lamps are real light: every
   vertex built while lot_tint is set is dimmed to the night and warmed by
   the lamps it is near, so the lot is pools of sodium light on dark
   asphalt, and the lamp heads glow each frame (lot_draw_tr).

   The expressway's deck and the overpass are road (track_road_y): the
   deck comes down to the ground at both ends, inside the ring road's
   barrier, which runs outside the deck along it, and the overpass is a
   hump bridge across the lot with its ramps on the lot. The lot's obstacles register
   themselves with roam.c as they are built, so the karts bounce off
   what they can see. */
#include "apple_valley_props.h"
#include "tsudoken_props.h"
#include "world_int.h"
#include "roam.h"
#include "models.h"

#define LOT_FLOOR_Y   (-0.03f)   /* just under the ring road, so the two never fight */
#define LOT_INNER_X   (LOT_HX - 9.5f)   /* the lot's edge: the ring's inner curb */
#define LOT_INNER_Z   (LOT_HZ - 9.5f)
#define LOT_FAR       700.0f     /* the ground runs out into the fog this far */
#define DECK_THICK    1.2f      /* the decks' slabs (their plan is in track.h: they are road) */
#define LOT_LAMPS     160

static const pvr_poly_hdr_t *h_floor, *h_apron, *h_concrete, *h_lotsigns, *h_lotcol;

/* What moves on the lot (lot_update): Chao standing about, baked from
   the drivers' model and bobbing on the spot, the crowd on the bleachers
   swaying, and a neon sign or two buzzing. */
#define LOT_PEOPLE 32
static Bystander people[LOT_PEOPLE];
static int people_n;
#define LOT_SWAY 2
#define SWAY_VERTS 96
#define SWAY_PHASES 32
typedef struct {
    int first, count;
    float base_y[SWAY_VERTS];
    uint8_t phase[SWAY_VERTS];   /* of SWAY_PHASES round the wave, so a frame takes 32 sines, not one a vertex */
} Sway;
static Sway sways[LOT_SWAY];
static int sway_n;
#define LOT_NEONS 4
typedef struct {
    int first;      /* the sign's four vertices in the pool */
    float period;   /* seconds between buzzes, 0 for a steady sign */
    float dim_t, next_t;
} Neon;
static Neon neons[LOT_NEONS];
static int neon_n;
static uint32_t lot_rng;   /* its own generator: the game's seeds QA fingerprints */
static float lot_frand(void) {
    lot_rng = lot_rng * 1664525u + 1013904223u;
    return (float)(lot_rng >> 8) / 16777216.0f;
}

/* The lamps: a sodium pool on the lot, a cooler white on the bridge. */
typedef struct {
    V3 p;
    float r;      /* reach of its pool, metres */
    uint8_t kind; /* 0 sodium, 1 white */
} Lamp;
static Lamp lamps[LOT_LAMPS];
static int lamp_n;

static void lamp(V3 p, float r, int kind) {
    if(lamp_n >= LOT_LAMPS) return;
    lamps[lamp_n++] = (Lamp){p, r, (uint8_t)kind};
}

/* Night: everything dimmed and cooled, then warmed within reach of a
   lamp (the pool falls off as the square of the distance). */
static uint32_t lot_tint(V3 p, uint32_t c) {
    float r = 0.30f, g = 0.32f, b = 0.46f;
    for(int i = 0; i < lamp_n; i++) {
        const Lamp *l = &lamps[i];
        V3 d = vsub(p, l->p);
        float d2 = vdot(d, d);
        if(d2 >= l->r * l->r) continue;
        float f = 1.0f - sqrtf(d2) / l->r;
        f *= f;
        if(l->kind) { r += f * 1.1f; g += f * 1.15f; b += f * 1.25f; }
        else { r += f * 1.5f; g += f * 1.08f; b += f * 0.55f; }
    }
    int cr = (int)(((c >> 16) & 255) * r), cg = (int)(((c >> 8) & 255) * g), cb = (int)((c & 255) * b);
    return argb((int)(c >> 24), cr > 255 ? 255 : cr, cg > 255 ? 255 : cg, cb > 255 ? 255 : cb);
}

void lot_init(void) {
    h_floor = r_header(TEX_AVROAD, HF_OP);
    h_apron = r_header(TEX_TKROAD, HF_OP);
    h_concrete = r_header(TEX_AVSTUCCO, HF_OP);
    h_lotsigns = r_header(TEX_SIGNS, HF_OP);
    h_lotcol = h_col;
}

uint32_t lot_sea_color(float x, float z) {
    float dist = sqrtf(x * x * 0.36f + z * z);
    float t = clampf((dist - 120.0f) / 400.0f, 0, 1);
    return color_lerp(argb(255, 52, 72, 128), argb(255, 22, 24, 66), t);
}

/* ------------------------------------------------------------------ */
/* Solids from the baked props' boxes (as world_tsudoken.c keeps them). */

static void prop_solids(const dk_prop_t *m, V3 p, float yaw, float scale) {
    float cs = cosf(yaw), sn = sinf(yaw), unit = (m->cm ? 0.01f : 0.001f) * scale;
    for(int i = 0; i < m->nsolids; i++) {
        const dk_prop_box_t *b = &m->solids[i];
        float cx = (b->x0 + b->x1) * 0.5f * unit, cz = (b->z0 + b->z1) * 0.5f * unit;
        V3 c = v3(p.x + cx * cs + cz * sn, p.y, p.z - cx * sn + cz * cs);
        roam_solid_box(c, (b->x1 - b->x0) * 0.5f * unit, (b->z1 - b->z0) * 0.5f * unit, yaw, p.y + b->y1 * unit);
    }
}

static void av_solid_prop(int which, V3 p, float yaw, float scale, float lod_far, bool far) {
    av_place_prop(which, p, yaw, scale, lod_far, far);
    prop_solids(av_prop_mesh(which), p, yaw, scale);
}

static void tk_solid_prop(int which, V3 p, float yaw, float scale, float lod_far, bool far) {
    tk_place_prop(which, p, yaw, scale, lod_far, far);
    prop_solids(tk_prop_mesh(which), p, yaw, scale);
}

/* A light pole (Apple Valley's, twin arms along its own x) with a lamp at
   each arm's end; yaw turns the arms. */
static void pole(V3 p, float yaw, float reach) {
    /* Its solid is the shaft (0.3 m at the foot), not the prop's baked
       box, which spans the whole arm 10 m up. */
    int before = chunk_n;
    av_place_prop(AVP_POLE, p, yaw, 1.0f, 420.0f, false);
    if(chunk_n == before + 1) {
        /* A split screen draws the baked pole (120 vertices, 21 strips)
           only within SPLIT_NEAR, and beyond it this stand-in in the
           prop's greys: the shaft as a four-sided prism, the arm a quad. */
        chunks[before].lod = 1;
        chunk_begin(HF_OP, 420.0f);
        if(cur_chunk) cur_chunk->lod = 2;
        uint32_t grey = argb(255, 112, 117, 126);
        cylinder(h_lotcol, p, 0.16f, 0.09f, 9.5f, 4, grey, grey, 1.0f, 1.0f);
        float cs = cosf(yaw), sn = sinf(yaw);
        V3 ax = v3(1.45f * cs, 0, -1.45f * sn), az = v3(0.18f * sn, 0, 0.18f * cs), c = vadd(p, v3(0, 9.55f, 0));
        quad(h_lotcol, vsub(vadd(c, az), ax), vadd(vadd(c, az), ax), vsub(vsub(c, az), ax), vadd(vsub(c, az), ax),
             0, 0, 1, 1, grey);
        chunk_end();
    }
    roam_solid_disc(p, 0.22f, 10.0f);
    float cs = cosf(yaw), sn = sinf(yaw);
    for(int side = -1; side <= 1; side += 2) {
        float lx = 1.3f * (float)side;
        lamp(v3(p.x + lx * cs, p.y + 9.6f, p.z - lx * sn), reach, 0);
    }
}

/* ------------------------------------------------------------------ */
/* The ground: the lot's floor, the island round it and the mainland. */

static void ground_rect(const pvr_poly_hdr_t *h, float x0, float x1, float z0, float z1, float cell, float lod,
                        int coarse, uint32_t col) {
    /* Rows of strips cell metres apart so the lamps' pools show on the
       vertices, in chunks a few cells square so the far ones are culled.
       With coarse, each chunk is followed by a copy of itself coarse
       cells a side, which a split screen draws beyond SPLIT_NEAR. */
    int nx = (int)ceilf((x1 - x0) / cell), nz = (int)ceilf((z1 - z0) / cell);
    if(nx < 1) nx = 1;
    if(nz < 1) nz = 1;
    const int per = 5;
    for(int cz = 0; cz < nz; cz += per) {
        for(int cx = 0; cx < nx; cx += per) {
            int ex = cx + per < nx ? cx + per : nx, ez = cz + per < nz ? cz + per : nz;
            for(int pass = 0; pass < (coarse ? 2 : 1); pass++) {
                /* The chunk's rows and columns: the cells, or coarse
                   divisions of the same rectangle. */
                int rows = pass ? coarse : ez - cz, cols = pass ? coarse : ex - cx;
                float cxa = x0 + (x1 - x0) * (float)cx / nx, cxb = x0 + (x1 - x0) * (float)ex / nx;
                float cza = z0 + (z1 - z0) * (float)cz / nz, czb = z0 + (z1 - z0) * (float)ez / nz;
                chunk_begin(HF_OP, lod);
                if(cur_chunk && coarse) cur_chunk->lod = (uint8_t)(pass + 1);
                for(int j = 0; j < rows; j++) {
                    float za = lerpf(cza, czb, (float)j / rows), zb = lerpf(cza, czb, (float)(j + 1) / rows);
                    strip_begin(h);
                    for(int i = 0; i <= cols; i++) {
                        float x = lerpf(cxa, cxb, (float)i / cols);
                        svp(v3(x, LOT_FLOOR_Y, zb), x / 16.0f, zb / 16.0f, col);
                        svp(v3(x, LOT_FLOOR_Y, za), x / 16.0f, za / 16.0f, col);
                    }
                    strip_end();
                }
                chunk_end();
            }
        }
    }
}

static void build_ground(void) {
    uint32_t lot_col = argb(255, 235, 235, 240), apron_col = argb(255, 200, 200, 205);
    /* The lot itself, 8 m cells for the light pools. */
    ground_rect(h_floor, -LOT_INNER_X, LOT_INNER_X, -LOT_INNER_Z, LOT_INNER_Z, 8.0f, 1200.0f, 2, lot_col);
    /* The island round the ring road: to the quay in the north, the
       shore east and west, the mainland south. */
    ground_rect(h_apron, -LOT_ISLAND_X, LOT_ISLAND_X, LOT_INNER_Z, LOT_QUAY_Z, 16.0f, 1500.0f, 2, apron_col);
    ground_rect(h_apron, -LOT_ISLAND_X, -LOT_INNER_X, -LOT_INNER_Z, LOT_INNER_Z, 16.0f, 1500.0f, 2, apron_col);
    ground_rect(h_apron, LOT_INNER_X, LOT_ISLAND_X, -LOT_INNER_Z, LOT_INNER_Z, 16.0f, 1500.0f, 2, apron_col);
    ground_rect(h_apron, -LOT_ISLAND_X, LOT_ISLAND_X, -LOT_ISLAND_X, -LOT_INNER_Z, 24.0f, 1500.0f, 2, apron_col);
    /* The mainland out into the fog, coarse. */
    uint32_t land = argb(255, 150, 150, 160);
    ground_rect(h_apron, -LOT_FAR, -LOT_ISLAND_X, -LOT_FAR, LOT_SHORE_Z, 120.0f, 6000.0f, 0, land);
    ground_rect(h_apron, LOT_ISLAND_X, LOT_FAR, -LOT_FAR, LOT_SHORE_Z, 120.0f, 6000.0f, 0, land);
    ground_rect(h_apron, -LOT_ISLAND_X, LOT_ISLAND_X, -LOT_FAR, -LOT_ISLAND_X, 120.0f, 6000.0f, 0, land);
    /* Quay walls where the island meets the water, a parapet along the top. */
    uint32_t q = argb(255, 170, 172, 180);
    chunk_begin(HF_OP, 2500.0f);
    box(h_concrete, v3(0, -0.4f, LOT_QUAY_Z + 0.4f), v3(LOT_ISLAND_X + 0.8f, 1.1f, 0.4f), 0, q, 0.25f);
    chunk_end();
    for(int side = -1; side <= 1; side += 2) {
        chunk_begin(HF_OP, 2500.0f);
        float zc = (LOT_QUAY_Z + LOT_SHORE_Z) * 0.5f, hz = (LOT_QUAY_Z - LOT_SHORE_Z) * 0.5f;
        box(h_concrete, v3(side * (LOT_ISLAND_X + 0.4f), -0.4f, zc), v3(0.4f, 1.1f, hz), 0, q, 0.25f);
        chunk_end();
    }
}

/* Painted bays: a white line every LOT_BAY_W along each double row, and
   the row's centre line. */
static void build_bays(void) {
    uint32_t white = argb(255, 235, 235, 230);
    const float lw = 0.07f, y = LOT_FLOOR_Y + 0.012f;
    for(int r = 0; r < LOT_ROWS; r++) {
        float zc = lot_row_z[r], z0 = zc - LOT_ROW_HALF, z1 = zc + LOT_ROW_HALF;
        int n = (int)(2.0f * LOT_ROW_HX / LOT_BAY_W + 0.5f);
        for(int i0 = 0; i0 <= n; i0 += 20) {
            chunk_begin(HF_OP, 170.0f);
            if(cur_chunk) cur_chunk->lod = 1;   /* split screen: only near, the lines are under a pixel beyond */
            strip_begin(h_lotcol);
            for(int i = i0; i < i0 + 20 && i <= n; i++) {
                float x = -LOT_ROW_HX + i * LOT_BAY_W;
                svj(v3(x - lw, y, z1), 0, 0, white);
                svp(v3(x + lw, y, z1), 0, 0, white);
                svp(v3(x - lw, y, z0), 0, 0, white);
                svp(v3(x + lw, y, z0), 0, 0, white);
            }
            /* The centre line of this stretch. */
            float xa = -LOT_ROW_HX + i0 * LOT_BAY_W, xb = fminf(xa + 20 * LOT_BAY_W, LOT_ROW_HX);
            svj(v3(xa, y, zc + lw), 0, 0, white);
            svp(v3(xb, y, zc + lw), 0, 0, white);
            svp(v3(xa, y, zc - lw), 0, 0, white);
            svp(v3(xb, y, zc - lw), 0, 0, white);
            strip_end();
            chunk_end();
        }
    }
}

/* ------------------------------------------------------------------ */
/* The ring road: asphalt, curbs both sides, a shoulder and a concrete
   barrier on the outside; the lot's floor meets the inner curb. */

/* Every other station, keeping the first and the last: the coarse
   copies a split screen draws beyond SPLIT_NEAR. */
static int coarse_svals(const float *sv, int n, float *out) {
    int m = 0;
    for(int k = 0; k < n; k += 2) out[m++] = sv[k];
    if(n > 1 && out[m - 1] != sv[n - 1]) out[m++] = sv[n - 1];
    return m;
}

/* The on-ramps: a lane leaving the ring road's outer edge. From 30 m
   before a foot the asphalt widens out to the lane (LOT_RAMP_D1), with
   a white line along the widening edge that the lane's outer line takes
   over at the foot. Past the foot the widened asphalt is the gore under
   the lane until the lane has lifted clear (0.5 m, where roam.c puts up
   its walls); then the road's own edge and curb return beside the
   retaining wall, and beyond the abutment the ground is parking under
   the deck. One edge line at a time: the ground's own line along the
   ramp read as a double line splitting from the lane's. */
static float ramp_smooth(float x) {
    x = clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

/* The ground asphalt's outer edge at s. */
static float ground_edge(float s) {
    float s0 = track_seg_s(6), s1 = track_seg_s(2);
    if(lot_in_deck(s)) return lerpf(LOT_RAMP_D1, 8.0f, ramp_smooth(lot_deck_y(s) / 0.5f));
    float before = fminf(track_wrap(s0 - s), track_wrap(s - s1));
    if(before < 30.0f) return lerpf(8.0f, LOT_RAMP_D1, ramp_smooth(1.0f - before / 30.0f));
    return 8.0f;
}

/* The barrier: a concrete Jersey profile (a battered face each side up
   to a flat top, three faces: the whole straight's worth is in view from
   the deck, so every face costs) following lot_wall_d. The coarse copy
   drops the far face. */
static void jersey_run(const float *sv, int n, bool full) {
    static const float prof[4][2] = {{-0.36f, 0.0f}, {-0.12f, 0.92f}, {0.12f, 0.92f}, {0.36f, 0.0f}};
    uint32_t conc = argb(255, 205, 205, 212);
    int np = full ? 4 : 3;
    for(int j = 0; j + 1 < np; j++) {
        const float *p0 = prof[j], *p1 = prof[j + 1];
        /* The face's normal in the profile plane: lateral, up. */
        float dx = p1[0] - p0[0], dy = p1[1] - p0[1], len = sqrtf(dx * dx + dy * dy);
        float nl = len > 0 ? -dy / len : 0, nu = len > 0 ? dx / len : 1;
        strip_begin(h_concrete);
        for(int k = 0; k < n; k++) {
            float s = sv[k], d = lot_wall_d(s);
            V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
            uint32_t col = shade(conc, v3(r.x * nl, nu, r.z * nl));
            V3 a = f8_pt(s, d + p0[0], 0.0f), b = f8_pt(s, d + p1[0], 0.0f);
            a.y = p0[1];
            b.y = p1[1];
            svp(a, s * 0.25f, p0[1], col);
            svp(b, s * 0.25f, p1[1], col);
        }
        strip_end();
    }
}

static void ring_pass(const float *sv, int n, bool full) {
    uint32_t road = argb(255, 250, 250, 250), white = argb(255, 235, 235, 230);
    /* The road's own 16 m, its edge lines in the texture: the outer one
       runs straight on past a ramp's taper as the through lane's line. */
    strip_begin(h_road);
    for(int k = 0; k < n; k++) {
        float s = sv[k];
        V3 a = f8_pt(s, -8.0f, 0.0f), b = f8_pt(s, 8.0f, 0.0f);
        svp(a, 0.0f, s / ROAD_V_M, road);
        svp(b, 1.0f, s / ROAD_V_M, road);
    }
    strip_end();
    /* The taper and gore out to a ramp, from the texture's plain band
       (stretching the road's texture over it wrapped its edge line a
       metre outside the road's). A zero-width station each end closes
       the run. */
    bool open = false;
    for(int k = 0; k < n; k++) {
        float s = sv[k], e = ground_edge(s);
        bool near = e > 8.05f || (k > 0 && ground_edge(sv[k - 1]) > 8.05f) || (k + 1 < n && ground_edge(sv[k + 1]) > 8.05f);
        if(!near) {
            if(open) strip_end();
            open = false;
            continue;
        }
        if(!open) {
            strip_begin(h_road);
            open = true;
        }
        V3 a = f8_pt(s, 8.0f, 0.0f), b = track_point(s, e);
        b.y = a.y;
        svp(a, 0.06f, s / ROAD_V_M, road);
        svp(b, 0.06f + (e - 8.0f) / 16.0f, s / ROAD_V_M, road);
    }
    if(open) strip_end();
    if(full) {
        f8_ribbon(sv, n, h_curb, -9.6f, -8.0f, 0.01f, 0.01f, 0, 0.0f, 2.4f, false, 0xffffffff, 0xffffffff);
        /* The outer curb where the edge is the road's; a white line along
           it where the asphalt opens out to a ramp (the approach only:
           past the foot the lane's own lines take over). */
        for(int pass = 0; pass < 2; pass++) {
            open = false;
            for(int k = 0; k < n; k++) {
                bool wide = ground_edge(sv[k]) > 8.3f;
                bool want = pass == 0 ? !wide : (wide && !lot_in_deck(sv[k]));
                if(!want) {
                    if(open) strip_end();
                    open = false;
                    continue;
                }
                if(!open) {
                    strip_begin(pass == 0 ? h_curb : h_lotcol);
                    open = true;
                }
                float s = sv[k];
                if(pass == 0) {
                    svp(f8_pt(s, 8.0f, 0.01f), 1.0f, s / 2.4f, 0xffffffff);
                    svp(f8_pt(s, 9.6f, 0.01f), 0.0f, s / 2.4f, 0xffffffff);
                }
                else {
                    float e = ground_edge(s);
                    V3 p = track_point(s, e - 0.55f), q = track_point(s, e - 0.4f);
                    p.y = q.y = 0.012f;
                    svp(p, 0, 0, white);
                    svp(q, 0, 0, white);
                }
            }
            if(open) strip_end();
        }
    }
    /* The ground from the road out to the barrier: the shoulder, or the
       lot's floor where it is parking under the deck. */
    float mid = sv[n / 2];
    bool park = lot_in_deck(mid) && !lot_deck_banked(mid);
    strip_begin(park ? h_floor : h_apron);
    for(int k = 0; k < n; k++) {
        float s = sv[k], w = lot_wall_d(s), e = ground_edge(s);
        if(e <= 8.3f) e = 9.6f;
        V3 a = track_point(s, e), b = track_point(s, w + 0.4f);
        a.y = b.y = park ? LOT_FLOOR_Y + 0.01f : 0.0f;
        svp(a, a.x / 16.0f, a.z / 16.0f, road);
        svp(b, b.x / 16.0f, b.z / 16.0f, road);
    }
    strip_end();
    jersey_run(sv, n, full);
}

static void build_ring_chunk(int c) {
    float sv[MAX_SVALS], cv[MAX_SVALS];
    int n = f8_chunk_svals(c, sv), nc = coarse_svals(sv, n, cv);
    chunk_begin(HF_OP, 2000.0f);
    if(cur_chunk) cur_chunk->lod = 1;
    ring_pass(sv, n, true);
    chunk_end();
    /* The coarse copy a split screen draws beyond SPLIT_NEAR: half the
       stations, no curbs or lines. */
    chunk_begin(HF_OP, 2000.0f);
    if(cur_chunk) cur_chunk->lod = 2;
    ring_pass(cv, nc, false);
    chunk_end();
}

/* ------------------------------------------------------------------ */
/* The expressway: a deck round the south and west sides outside the
   ring road, up on round pillars, with the ground beneath it parking;
   at each end a single-lane ramp leaving the road's edge on an earth
   embankment, widening into the deck as it climbs. The overpass across
   the north end of the lot. */

/* Where the deck's lane is at s: u across it (0 its inner edge, 1 its
   outer) plus off metres. */
static float deck_d(float s, float u, float off) {
    return lerpf(lot_deck_d0(s), lot_deck_d1(s), u) + off;
}

/* A ribbon along the deck between (u0, off0) and (u1, off1) across the
   lane, at dy0, dy1 above its height (vertical faces have the same
   lateral at both). */
static void deck_rib(const float *sv, int n, const pvr_poly_hdr_t *h, float u0, float off0, float u1, float off1,
                     float dy0, float dy1, float tu0, float tu1, float vscale, uint32_t c0, uint32_t c1) {
    strip_begin(h);
    for(int k = 0; k < n; k++) {
        float s = sv[k], y = lot_deck_y(s);
        V3 a = track_point(s, deck_d(s, u0, off0)), b = track_point(s, deck_d(s, u1, off1));
        /* The slab's underside stays just under the ground at the feet. */
        a.y = fmaxf(y + dy0, LOT_FLOOR_Y - 0.02f);
        b.y = fmaxf(y + dy1, LOT_FLOOR_Y - 0.02f);
        svp(a, tu0, s / vscale, c0);
        svp(b, tu1, s / vscale, c1);
    }
    strip_end();
}

/* The inner parapet (face and top) in runs that stop at the gaps, where
   a hazard-striped band marks the open edge. */
static void deck_rib_inner(const float *sv, int n, uint32_t lip, uint32_t face) {
    for(int pass = 0; pass < 2; pass++) {
        bool open = false;
        for(int k = 0; k < n; k++) {
            float s = sv[k], y = lot_deck_y(s), d0 = lot_deck_d0(s);
            bool gap = k + 1 < n ? lot_deck_gap((s + sv[k + 1]) * 0.5f) : lot_deck_gap(s - 0.5f);
            bool was = k > 0 && !lot_deck_gap((sv[k - 1] + s) * 0.5f);
            if(gap && !was) {
                if(open) strip_end();
                open = false;
                continue;
            }
            if(!open) {
                strip_begin(pass ? h_lotcol : h_concrete);
                open = true;
            }
            if(pass == 0) {
                V3 a = track_point(s, d0), b = a;
                a.y = y + 1.0f;
                b.y = y;
                svp(a, s / 8.0f, 0.0f, lip);
                svp(b, s / 8.0f, 0.3f, face);
            }
            else {
                V3 a = track_point(s, d0 - 0.2f), b = track_point(s, d0 + 0.4f);
                a.y = b.y = y + 1.0f;
                svp(a, 0, 0, lip);
                svp(b, 0, 0, lip);
            }
            if(gap) {
                strip_end();
                open = false;
            }
        }
        if(open) strip_end();
    }
    /* The stripes along each gap. */
    bool open = false;
    for(int k = 0; k < n; k++) {
        float s = sv[k], d0 = lot_deck_d0(s);
        if(!lot_deck_gap(s)) {
            if(open) strip_end();
            open = false;
            continue;
        }
        if(!open) {
            strip_begin(h_checker);
            open = true;
        }
        V3 a = track_point(s, d0 - 0.1f), b = track_point(s, d0 + 1.1f);
        a.y = b.y = lot_deck_y(s) + 0.02f;
        svp(a, 0, s / 1.2f, 0xffffffff);
        svp(b, 1, s / 1.2f, 0xffffffff);
    }
    if(open) strip_end();
}

/* The deck's foot between stations a and b (one is on the deck, the
   other not): whichever end of the deck lies between them. */
static float deck_foot_between(float a, float b) {
    return lot_in_deck(a) ? (lot_in_deck(b) ? a : track_seg_s(2)) : track_seg_s(6);
}

/* The ramp's embankment over the stations given (all on it): a concrete
   retaining wall on the road side, straight down from the lane's edge,
   and an earth bank on the other, LOT_RAMP_BATTER out per metre down
   (u across the bank from the apron texture's plain band: its lines
   would otherwise cross the bank, see over_banks). */
static void ramp_sides(const float *sv, int n) {
    uint32_t wall = argb(255, 178, 178, 186), top = argb(255, 150, 142, 128), foot = argb(255, 112, 106, 96);
    strip_begin(h_concrete);
    for(int k = 0; k < n; k++) {
        float s = sv[k], y = lot_deck_y(s);
        V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
        uint32_t col = shade(wall, vscale(r, -1.0f));
        V3 a = track_point(s, lot_deck_d0(s)), b = a;
        a.y = y - 0.02f;
        b.y = LOT_FLOOR_Y - 0.02f;
        svp(a, s * 0.25f, 0.0f, col);
        svp(b, s * 0.25f, y / 4.0f, color_scale(col, 0.85f));
    }
    strip_end();
    strip_begin(h_apron);
    for(int k = 0; k < n; k++) {
        float s = sv[k], y = lot_deck_y(s), d1 = lot_deck_d1(s);
        V3 a = track_point(s, d1), b = track_point(s, d1 + LOT_RAMP_BATTER * y);
        a.y = y - 0.02f;
        b.y = LOT_FLOOR_Y - 0.02f;
        svp(b, 0.28f, s / 16.0f, foot);
        svp(a, 0.08f, s / 16.0f, top);
    }
    strip_end();
}

/* The embankment's end wall under the deck where the span begins, at
   station s, facing along the deck toward ahead (+1) or back (-1). */
static void ramp_abutment(float s, float ahead) {
    float y = lot_deck_y(s), d0 = lot_deck_d0(s), d1 = lot_deck_d1(s);
    uint32_t face = argb(255, 160, 160, 170);
    V3 t = track_tangent(s);
    V3 a = track_point(s, d0), b = track_point(s, d1 + LOT_RAMP_BATTER * y);
    a.y = b.y = LOT_FLOOR_Y - 0.02f;
    V3 c = track_point(s, d0), d = track_point(s, d1);
    c.y = d.y = y - DECK_THICK + 0.05f;
    uint32_t col = shade(face, vscale(t, ahead));
    strip_begin(h_concrete);
    svp(c, 0, 0, col); svp(d, 2.0f, 0, col);
    svp(a, 0, 1.0f, col); svp(b, 2.0f, 1.0f, col);
    strip_end();
}

static void build_deck_chunk(int c) {
    float sv[MAX_SVALS], dv[MAX_SVALS + 2], pv[MAX_SVALS + 2];
    int n = f8_chunk_svals(c, sv), nd = 0, np = 0;
    /* The slab's stations: those on the deck, plus its exact foot where
       a chunk spans one, so the lane begins right there at the ground. */
    for(int k = 0; k < n; k++) {
        bool in = lot_in_deck(sv[k]);
        if(in && nd == 0 && k > 0) dv[nd++] = deck_foot_between(sv[k - 1], sv[k]);
        if(in) dv[nd++] = sv[k];
        else if(nd > 0 && k > 0 && lot_in_deck(sv[k - 1])) dv[nd++] = deck_foot_between(sv[k - 1], sv[k]);
    }
    if(nd < 2) return;
    /* The parapets' stations: where the deck has lifted off the ground,
       as its walls have (roam.c). */
    for(int k = 0; k < nd; k++)
        if(lot_deck_y(dv[k]) >= 0.5f) pv[np++] = dv[k];
    uint32_t road = argb(255, 250, 250, 250), under = argb(255, 120, 120, 132), face = argb(255, 175, 175, 184),
             lip = argb(255, 215, 215, 220), white = argb(255, 235, 235, 230);
    float cdv[MAX_SVALS + 2], cpv[MAX_SVALS + 2];
    int ncd = coarse_svals(dv, nd, cdv), ncp = coarse_svals(pv, np, cpv);
    for(int pass = 0; pass < 2; pass++) {
        bool full = pass == 0;
        const float *v = full ? dv : cdv, *pp = full ? pv : cpv;
        int nv = full ? nd : ncd, npp = full ? np : ncp;
        chunk_begin(HF_OP, 2200.0f);
        if(cur_chunk) cur_chunk->lod = (uint8_t)(pass + 1);
        /* The lane, a touch above the ground asphalt it leaves at the foot. */
        deck_rib(v, nv, h_road, 0, 0, 1, 0, 0.015f, 0.015f, 0.0f, 1.0f, ROAD_V_M, road, road);
        if(full) {
            deck_rib(v, nv, h_lotcol, 0, 0.4f, 0, 0.55f, 0.027f, 0.027f, 0, 0, 1, white, white);
            deck_rib(v, nv, h_lotcol, 1, -0.55f, 1, -0.4f, 0.027f, 0.027f, 0, 0, 1, white, white);
        }
        /* The embankment's stations and the span's, each run taking one
           station of the other so they meet inside the embankment, an
           abutment wall where they meet. */
        for(int k0 = 0; k0 < nv;) {
            bool emb = lot_deck_banked(v[k0]);
            int k1 = k0;
            while(k1 + 1 < nv && lot_deck_banked(v[k1 + 1]) == emb) k1++;
            int from = k0 > 0 ? k0 - 1 : k0, to = k1 + 1 < nv ? k1 + 1 : k1;
            if(emb) {
                ramp_sides(v + k0, k1 - k0 + 1);
                if(k0 > 0) ramp_abutment(v[k0], -1.0f);
                if(k1 + 1 < nv) ramp_abutment(v[k1], 1.0f);
            }
            else {
                int cnt = to - from + 1;
                deck_rib(v + from, cnt, h_concrete, 1, 0, 0, 0, -DECK_THICK, -DECK_THICK, 0.0f, 2.0f, 8.0f, under, under);
                if(full) {
                    deck_rib(v + from, cnt, h_concrete, 0, 0, 0, 0, 0.0f, -DECK_THICK, 0.0f, 0.3f, 8.0f, face, face);
                    deck_rib(v + from, cnt, h_concrete, 1, 0, 1, 0, -DECK_THICK, 0.0f, 0.0f, 0.3f, 8.0f, face, face);
                }
            }
            k0 = k1 + 1;
        }
        if(npp >= 2) {
            deck_rib_inner(pv, np, lip, face);   /* toward the lot, open at the gaps: every station, so the gaps stay put */
            deck_rib(pp, npp, h_concrete, 1, 0, 1, 0, 0.0f, 1.0f, 0.0f, 0.3f, 8.0f, face, lip);   /* outer parapet */
            if(full) deck_rib(pp, npp, h_lotcol, 1, -0.4f, 1, 0.2f, 1.0f, 1.0f, 0, 0, 1, lip, lip);
            /* Running lights along the outer parapet: Neon Skyway's edge
               strip, its header swapped through the phases each frame
               (world_update). */
            svp_tint = NULL;
            deck_rib(pp, npp, &sk_road_hdr, 1, -0.35f, 1, 0.15f, 1.02f, 1.02f, 0.92f, 0.985f, 8.0f, 0xffffffff, 0xffffffff);
            svp_tint = lot_tint;
        }
        chunk_end();
    }
}

/* Parking under the deck: painted bays either side of the pillars'
   line, LOT_BAY_W along the road, where the deck is on its span. In
   short chunks with a short range: from up on the deck the whole span
   is in view, and these lines are under a pixel past 100 m. */
static void build_deck_bays(void) {
    uint32_t white = argb(255, 235, 235, 230);
    const float lw = 0.07f, y = LOT_FLOOR_Y + 0.022f;
    float s0 = track_seg_s(6), len = track.length - s0 + track_seg_s(2);
    for(float from = LOT_RAMP_EMB + 6.0f; from < len - LOT_RAMP_EMB - 6.0f; from += 10.0f * LOT_BAY_W) {
        float to = fminf(from + 10.0f * LOT_BAY_W, len - LOT_RAMP_EMB - 6.0f);
        chunk_begin(HF_OP, 110.0f);
        if(cur_chunk) cur_chunk->lod = 1;   /* split screen: only near, the lines are under a pixel beyond */
        strip_begin(h_lotcol);
        for(int row = 0; row < 2; row++) {
            float da = row ? 28.0f : 19.0f, db = da + 5.0f;
            for(float at = from; at <= to + 0.1f; at += LOT_BAY_W) {
                float s = track_wrap(s0 + at);
                V3 a = track_point(s - lw, da), b = track_point(s + lw, da), c = track_point(s - lw, db), d = track_point(s + lw, db);
                a.y = b.y = c.y = d.y = y;
                svj(a, 0, 0, white); svp(b, 0, 0, white); svp(c, 0, 0, white); svp(d, 0, 0, white);
            }
            /* The row's outer line. */
            V3 a = track_point(track_wrap(s0 + from), row ? db : da), b = track_point(track_wrap(s0 + to), row ? db : da);
            V3 c = track_point(track_wrap(s0 + from), (row ? db : da) + (row ? -2 * lw : 2 * lw)),
               d = track_point(track_wrap(s0 + to), (row ? db : da) + (row ? -2 * lw : 2 * lw));
            a.y = b.y = c.y = d.y = y;
            svj(a, 0, 0, white); svp(b, 0, 0, white); svp(c, 0, 0, white); svp(d, 0, 0, white);
        }
        strip_end();
        chunk_end();
    }
}

/* Lamps on the deck's outer parapet every 48 m, lighting the deck:
   registered before the deck is built so their pools bake onto it. */
static void build_deck_lamps(void) {
    float deck_s0 = track_seg_s(6), deck_s1 = track_seg_s(2), deck_len = track.length - deck_s0 + deck_s1;
    for(float s = deck_s0 + 24.0f; s < deck_s0 + deck_len - 24.0f; s += 48.0f) {
        float y = lot_deck_y(s);
        if(y < 6.0f) continue;
        V3 p = track_point(s, lot_deck_d1(s) - 0.6f);
        p.y = y + 1.0f;
        chunk_begin(HF_OP, 500.0f);
        box(h_lotcol, vadd(p, v3(0, 2.6f, 0)), v3(0.1f, 2.6f, 0.1f), 0, argb(255, 120, 120, 130), 1);
        chunk_end();
        lamp(vadd(p, v3(0, 5.4f, 0)), 18.0f, 0);
    }
}

/* The pillars stand every 24 m along the span, under the deck's middle. */
static bool deck_pillar_at(float s) {
    return lot_in_deck(s) && !lot_deck_banked(s) && lot_deck_y(s) >= 3.0f;
}

/* Lamps on the pillars, lighting the parking under the deck: before the
   ground, so their pools bake onto it (the fixtures go up with the
   pillars). */
static void build_deck_underlamps(void) {
    float deck_s0 = track_seg_s(6), deck_s1 = track_seg_s(2), deck_len = track.length - deck_s0 + deck_s1;
    for(float s = deck_s0 + 12.0f; s < deck_s0 + deck_len - 12.0f; s += 24.0f) {
        if(!deck_pillar_at(s)) continue;
        V3 p = track_point(s, (LOT_DECK_D0 + LOT_DECK_D1) * 0.5f - 1.7f);
        p.y = 4.6f;
        lamp(p, 15.0f, 0);
    }
}

static void build_deck_pillars(void) {
    uint32_t conc = argb(255, 165, 165, 175);
    float deck_s0 = track_seg_s(6), deck_s1 = track_seg_s(2), deck_len = track.length - deck_s0 + deck_s1;
    for(float s = deck_s0 + 12.0f; s < deck_s0 + deck_len - 12.0f; s += 24.0f) {
        if(!deck_pillar_at(s)) continue;
        float y = lot_deck_y(s);
        V3 p = track_point(s, (LOT_DECK_D0 + LOT_DECK_D1) * 0.5f);
        p.y = LOT_FLOOR_Y;
        chunk_begin(HF_OP, 900.0f);
        cylinder(h_concrete, p, 1.4f, 1.4f, y - DECK_THICK + 0.05f, 8, color_scale(conc, 0.8f), conc, 2.0f, (y - 1.0f) / 4.0f);
        chunk_end();
        roam_solid_disc(p, 1.4f, y - DECK_THICK);   /* for karts under the deck; those on it are above its top */
    }
}

/* The overpass: a hump bridge over the lot's north end, flat across the
   middle and ramping down to the lot at both ends (lot_over_y). Each
   approach is on an earth embankment, banked LOT_RAMP_BATTER out per
   metre of height on both sides like the expressway ramps', for
   LOT_OVER_EMB from the foot, then the slab spans to the flat on
   pillars; drivable, with parapets either side from where it has lifted
   off the ground. */
static void over_rib(const float *xv, int n, const pvr_poly_hdr_t *h, float z0, float z1, float dy0, float dy1,
                     bool world_uv, uint32_t c0, uint32_t c1) {
    strip_begin(h);
    for(int k = 0; k < n; k++) {
        float x = xv[k], y = lot_over_y(x);
        V3 a = v3(x, y + dy0, z0), b = v3(x, y + dy1, z1);
        if(world_uv) {
            svp(a, x / 16.0f, z0 / 16.0f, c0);
            svp(b, x / 16.0f, z1 / 16.0f, c1);
        }
        else {
            svp(a, x / 8.0f, 0.0f, c0);
            svp(b, x / 8.0f, 0.3f, c1);
        }
    }
    strip_end();
}

/* The embankment's banks over the stations given (all on it), from the
   slab's edges down to the ground. The apron texture is a road with
   lines along v: these banks run along x, so they take u across the
   bank from the texture's plain band and v along it, or the lines
   would cross them every 16 m. */
static void over_banks(const float *xv, int n, float z0, float z1) {
    uint32_t top = argb(255, 150, 142, 128), foot = argb(255, 112, 106, 96);
    for(int side = 0; side < 2; side++) {
        float ze = side ? z1 : z0, out = side ? 1.0f : -1.0f;
        strip_begin(h_apron);
        for(int k = 0; k < n; k++) {
            float x = xv[k], y = lot_over_y(x);
            V3 a = v3(x, y - 0.02f, ze), b = v3(x, LOT_FLOOR_Y - 0.02f, ze + out * LOT_RAMP_BATTER * y);
            if(side) {
                svp(a, 0.08f, x / 16.0f, top);
                svp(b, 0.28f, x / 16.0f, foot);
            }
            else {
                svp(b, 0.28f, x / 16.0f, foot);
                svp(a, 0.08f, x / 16.0f, top);
            }
        }
        strip_end();
    }
}

/* The embankment's end wall under the slab where the span begins, at
   station x, facing along the overpass toward +x (1) or -x (-1). */
static void over_abutment(float x, float ahead, float z0, float z1) {
    float y = lot_over_y(x), w = LOT_RAMP_BATTER * y;
    uint32_t col = shade(argb(255, 160, 160, 170), v3(ahead, 0, 0));
    V3 a = v3(x, LOT_FLOOR_Y - 0.02f, z0 - w), b = v3(x, LOT_FLOOR_Y - 0.02f, z1 + w);
    V3 c = v3(x, y - DECK_THICK + 0.05f, z0), d = v3(x, y - DECK_THICK + 0.05f, z1);
    strip_begin(h_concrete);
    if(ahead > 0) { svp(d, 2.0f, 0, col); svp(c, 0, 0, col); svp(b, 2.0f, 1.0f, col); svp(a, 0, 1.0f, col); }
    else { svp(c, 0, 0, col); svp(d, 2.0f, 0, col); svp(a, 0, 1.0f, col); svp(b, 2.0f, 1.0f, col); }
    strip_end();
}

/* Where the overpass has lifted LOT_OVER_LIFT off the ground, |x| at
   each foot, found once. */
#define LOT_OVER_LIFT 0.5f
static float over_lift_a(void) {
    static float a = 0.0f;
    if(a == 0.0f) {
        float lo = LOT_OVER_FOOT - LOT_OVER_EMB, hi = LOT_OVER_FOOT;
        for(int i = 0; i < 24; i++) {
            float mid = (lo + hi) * 0.5f;
            if(lot_over_y(mid) >= LOT_OVER_LIFT) lo = mid; else hi = mid;
        }
        a = lo;
    }
    return a;
}

/* The parapets' stations among those given: the ones where the deck is
   up, plus the exact lift point where a pair straddles it (on the
   embankment only when banked_only). */
static int over_parapet_svals(const float *xv, int n, float *pv, bool banked_only) {
    int np = 0;
    for(int k = 0; k < n; k++) {
        bool up = lot_over_y(xv[k]) >= LOT_OVER_LIFT, ok = up && (!banked_only || lot_over_banked(xv[k]));
        if(k > 0 && up != (lot_over_y(xv[k - 1]) >= LOT_OVER_LIFT)) {
            float lift = xv[k] < 0 ? -over_lift_a() : over_lift_a();
            if(!banked_only || lot_over_banked(lift)) pv[np++] = lift;
        }
        if(ok) pv[np++] = xv[k];
    }
    return np;
}

/* One piece of the overpass over the stations given: the deck, then its
   embankment's and its span's stations in runs, each run taking one
   station of the other so they meet inside the embankment with the
   abutment wall where they meet (as the expressway's chunks do), and
   the parapets where the deck has lifted off the ground. */
static void over_piece(const float *xv, int n, bool full) {
    uint32_t top = argb(255, 235, 235, 240), under = argb(255, 120, 120, 132), face = argb(255, 175, 175, 184),
             lip = argb(255, 215, 215, 220);
    const float z0 = LOT_OVER_Z - LOT_OVER_HW, z1 = LOT_OVER_Z + LOT_OVER_HW;
    over_rib(xv, n, h_floor, z1, z0, 0.0f, 0.0f, true, top, top);
    for(int k0 = 0; k0 < n;) {
        bool emb = lot_over_banked(xv[k0]);
        int k1 = k0;
        while(k1 + 1 < n && lot_over_banked(xv[k1 + 1]) == emb) k1++;
        if(emb) {
            over_banks(xv + k0, k1 - k0 + 1, z0, z1);
            if(k0 > 0) over_abutment(xv[k0], -1.0f, z0, z1);
            if(k1 + 1 < n) over_abutment(xv[k1], 1.0f, z0, z1);
        }
        else {
            int from = k0 > 0 ? k0 - 1 : k0, to = k1 + 1 < n ? k1 + 1 : k1, cnt = to - from + 1;
            over_rib(xv + from, cnt, h_concrete, z0, z1, -DECK_THICK, -DECK_THICK, false, under, under);
            over_rib(xv + from, cnt, h_concrete, z0, z0, 1.0f, -DECK_THICK, false, lip, face);
            over_rib(xv + from, cnt, h_concrete, z1, z1, -DECK_THICK, 1.0f, false, face, lip);
        }
        k0 = k1 + 1;
    }
    /* The parapets, from where the deck has lifted half a metre as its
       walls have (roam.c), that point put in as a station so they start
       there and not at the next station along: on the span the slab's
       faces above carry on as the parapet's; on the embankment they
       need their own. */
    float pv[16];
    int np = over_parapet_svals(xv, n, pv, true);
    if(np >= 2) {
        over_rib(pv, np, h_concrete, z0, z0, 1.0f, -0.02f, false, lip, face);
        over_rib(pv, np, h_concrete, z1, z1, -0.02f, 1.0f, false, face, lip);
    }
    if(full) {
        np = over_parapet_svals(xv, n, pv, false);
        if(np >= 2) {
            over_rib(pv, np, h_lotcol, z0 - 0.2f, z0 + 0.4f, 1.0f, 1.0f, false, lip, lip);
            over_rib(pv, np, h_lotcol, z1 - 0.4f, z1 + 0.2f, 1.0f, 1.0f, false, lip, lip);
        }
    }
}

static void build_overpass(void) {
    const float z1 = LOT_OVER_Z + LOT_OVER_HW;
    /* Lamps along the north parapet, first so their pools bake onto it. */
    for(float x = -120.0f; x <= 120.0f; x += 40.0f) {
        V3 p = v3(x, lot_over_y(x) + 1.0f, z1 - 0.6f);
        chunk_begin(HF_OP, 500.0f);
        box(h_lotcol, vadd(p, v3(0, 2.6f, 0)), v3(0.1f, 2.6f, 0.1f), 0, argb(255, 120, 120, 130), 1);
        chunk_end();
        lamp(vadd(p, v3(0, 5.4f, 0)), 18.0f, 0);
    }
    /* In 30 m pieces of four stations, so the far ones are culled and the
       ramps' curves and the lamps' pools show; the embankment's end falls
       on a station. */
    for(float x = -LOT_OVER_FOOT; x < LOT_OVER_FOOT - 0.1f; x += 30.0f) {
        float xa = x, xb = fminf(x + 30.0f, LOT_OVER_FOOT), xv[5];
        for(int k = 0; k <= 4; k++) xv[k] = lerpf(xa, xb, (float)k / 4.0f);
        chunk_begin(HF_OP, 2200.0f);
        if(cur_chunk) cur_chunk->lod = 1;
        over_piece(xv, 5, true);
        chunk_end();
        /* The coarse copy for a split screen: two steps, no parapet tops. */
        float cv[3] = {xv[0], xv[2], xv[4]};
        chunk_begin(HF_OP, 2200.0f);
        if(cur_chunk) cur_chunk->lod = 2;
        over_piece(cv, 3, false);
        chunk_end();
    }
    /* Pillars under the span, from the abutments in; nothing in the
       lot's aisles beyond them. */
    uint32_t conc = argb(255, 165, 165, 175);
    for(float x = -80.0f; x <= 80.0f; x += 40.0f) {
        V3 p = v3(x, LOT_FLOOR_Y, LOT_OVER_Z);
        float h = lot_over_y(x) - DECK_THICK;
        chunk_begin(HF_OP, 900.0f);
        cylinder(h_concrete, p, 1.4f, 1.4f, h + 0.05f, 8, color_scale(conc, 0.8f), conc, 2.0f, (h - 1.0f) / 4.0f);
        chunk_end();
        roam_solid_disc(p, 1.4f, h);   /* up to the slab: a kart on the deck is above it */
    }
}

/* ------------------------------------------------------------------ */
/* What is parked and built on the lot. */

/* A neon sign (Neon Skyway's panels, row 0-3) on a wall facing north at
   z, centred on x, w wide and h tall with its foot at y. */
static void neon_sign(float x, float y, float z, float w, float h, int row, float flicker) {
    float v0 = (float)row * 0.25f, v1 = v0 + 0.25f;
    svp_tint = NULL;
    chunk_begin(HF_OP, 1400.0f);
    /* Read from the lot, facing south: the viewer's left is east. */
    strip_begin(h_lotsigns);
    svp(v3(x + w * 0.5f, y + h, z), 0, v0, 0xffffffff);
    svp(v3(x - w * 0.5f, y + h, z), 1, v0, 0xffffffff);
    svp(v3(x + w * 0.5f, y, z), 0, v1, 0xffffffff);
    svp(v3(x - w * 0.5f, y, z), 1, v1, 0xffffffff);
    strip_end();
    if(neon_n < LOT_NEONS && cur_chunk) {
        Neon *n = &neons[neon_n++];
        n->first = strips[strip_n - 1].first;
        n->period = flicker;
        n->dim_t = 0;
        n->next_t = flicker * (0.5f + lot_frand());
    }
    chunk_end();
    svp_tint = lot_tint;
}

/* A Chao standing at p facing yaw, in kart_palette colour pal, head
   turned a little. It bobs on the spot (lot_update). */
static void person(V3 p, float yaw, int pal) {
    if(people_n >= LOT_PEOPLE) return;
    float head = (lot_frand() - 0.5f) * 0.9f;
    yaw += (lot_frand() - 0.5f) * 0.4f;
    float phase = lot_frand() * DK_TAU;
    add_chao(&people[people_n++], p, yaw, head, pal, phase);
    roam_solid_disc(p, 0.35f, 1.0f);
}

/* The crowd baked onto the bleachers just placed (its strip is the
   prop's last, the crowd material coming after the frame's) sways. */
static void sway_last_prop(void) {
    if(sway_n >= LOT_SWAY || chunk_n == 0) return;
    const Chunk *c = &chunks[chunk_n - 1];
    const Strip *st = &strips[c->first_strip + c->nstrips - 1];
    Sway *sw = &sways[sway_n++];
    sw->first = st->first;
    sw->count = st->count < SWAY_VERTS ? st->count : SWAY_VERTS;
    for(int i = 0; i < sw->count; i++) {
        const RV *v = &pool[sw->first + i];
        sw->base_y[i] = v->y;
        sw->phase[i] = (uint8_t)((int)((v->x + v->z) * 1.7f * (SWAY_PHASES / DK_TAU)) & (SWAY_PHASES - 1));
    }
}

/* A food truck with its hatch to the lot (north), a striped awning over
   the counter, a lamp under it and a queue along its side. */
static void build_food_truck(V3 p) {
    uint32_t body = argb(255, 240, 235, 220), cab = argb(255, 235, 240, 245), dark = argb(255, 30, 30, 36);
    uint32_t red = argb(255, 230, 60, 50), white = argb(255, 250, 250, 250), hatch = argb(255, 255, 236, 185);
    lamp(v3(p.x, 2.9f, p.z + 2.0f), 13.0f, 0);
    /* Twice: in full, then a split screen's coarse copy beyond SPLIT_NEAR
       (the body, the cab, the awning in two and the lit hatch). */
    for(int pass = 0; pass < 2; pass++) {
        bool full = pass == 0;
        chunk_begin(HF_OP, 700.0f);
        if(cur_chunk) cur_chunk->lod = (uint8_t)(pass + 1);
        box(h_lotcol, vadd(p, v3(0, 1.45f, 0)), v3(2.6f, 1.25f, 1.1f), 0, body, 1);
        box(h_lotcol, vadd(p, v3(3.4f, 1.05f, 0)), v3(0.9f, 0.85f, 1.0f), 0, cab, 1);
        if(full) {
            box(h_lotcol, vadd(p, v3(4.1f, 1.5f, 0)), v3(0.25f, 0.4f, 0.9f), 0, argb(255, 60, 70, 100), 1);   /* windscreen */
            for(int i = 0; i < 4; i++)
                box(h_lotcol, vadd(p, v3(i < 2 ? -1.7f : 3.1f, 0.33f, (i & 1) ? 0.95f : -0.95f)), v3(0.36f, 0.36f, 0.16f), 0,
                    dark, 1);
            /* The counter and its shelf, a menu board by the queue. */
            box(h_lotcol, vadd(p, v3(0, 1.1f, 1.2f)), v3(1.9f, 0.08f, 0.22f), 0, argb(255, 120, 90, 60), 1);
            box(h_lotcol, vadd(p, v3(-3.3f, 0.6f, 2.0f)), v3(0.4f, 0.6f, 0.05f), 0, dark, 1);
        }
        /* Stripes on the awning, sloping out from the roof line. */
        int stripes = full ? 6 : 2;
        for(int i = 0; i < stripes; i++) {
            float xa = p.x - 2.4f + 4.8f * (float)i / (float)stripes, xb = p.x - 2.4f + 4.8f * (float)(i + 1) / (float)stripes;
            quad(h_lotcol, v3(xa, p.y + 2.75f, p.z + 1.1f), v3(xb, p.y + 2.75f, p.z + 1.1f), v3(xa, p.y + 2.3f, p.z + 2.5f),
                 v3(xb, p.y + 2.3f, p.z + 2.5f), 0, 0, 1, 1, (i & 1) ? white : red);
        }
        /* The lit hatch and the menu board's face, their own light. */
        svp_tint = NULL;
        quad(h_lotcol, v3(p.x - 1.7f, p.y + 2.4f, p.z + 1.12f), v3(p.x + 1.7f, p.y + 2.4f, p.z + 1.12f),
             v3(p.x - 1.7f, p.y + 1.2f, p.z + 1.12f), v3(p.x + 1.7f, p.y + 1.2f, p.z + 1.12f), 0, 0, 1, 1, hatch);
        if(full)
            quad(h_lotcol, v3(p.x - 3.6f, p.y + 1.1f, p.z + 2.06f), v3(p.x - 3.0f, p.y + 1.1f, p.z + 2.06f),
                 v3(p.x - 3.6f, p.y + 0.2f, p.z + 2.06f), v3(p.x - 3.0f, p.y + 0.2f, p.z + 2.06f), 0, 0, 1, 1,
                 argb(255, 120, 130, 110));
        svp_tint = lot_tint;
        chunk_end();
    }
    roam_solid_box(vadd(p, v3(0.8f, 0, 0)), 3.6f, 1.2f, 0, 2.8f);
    /* The queue, from the counter back along the truck, facing it. */
    static const int pals[6] = {3, 9, 6, 1, 11, 8};
    for(int i = 0; i < 6; i++)
        person(vadd(p, v3(-0.8f - 0.9f * (float)i, 0, 2.1f + 0.25f * (lot_frand() - 0.5f))), DK_PI, pals[i]);
}

static void vending_machine(V3 p, uint32_t body, uint32_t face) {
    svp_tint = NULL;
    chunk_begin(HF_OP, 220.0f);
    box(h_lotcol, vadd(p, v3(0, 0.92f, 0)), v3(0.45f, 0.92f, 0.4f), 0, body, 1);
    /* The lit front, proud of the body, facing north. */
    quad(h_lotcol, vadd(p, v3(-0.38f, 1.7f, 0.42f)), vadd(p, v3(0.38f, 1.7f, 0.42f)), vadd(p, v3(-0.38f, 0.3f, 0.42f)),
         vadd(p, v3(0.38f, 0.3f, 0.42f)), 0, 0, 1, 1, face);
    chunk_end();
    svp_tint = lot_tint;
    roam_solid_box(p, 0.45f, 0.4f, 0, 1.9f);
}

static void planter_tree(V3 p, uint32_t seed) {
    uint32_t conc = argb(255, 170, 170, 180);
    chunk_begin(HF_OP, 400.0f);
    box(h_concrete, vadd(p, v3(0, 0.3f, 0)), v3(1.4f, 0.3f, 1.4f), 0, conc, 0.3f);
    quad(h_grass, vadd(p, v3(-1.2f, 0.62f, 1.2f)), vadd(p, v3(1.2f, 0.62f, 1.2f)), vadd(p, v3(-1.2f, 0.62f, -1.2f)),
         vadd(p, v3(1.2f, 0.62f, -1.2f)), 0, 0, 1, 1, argb(255, 160, 170, 140));
    chunk_end();
    roam_solid_box(p, 1.4f, 1.4f, 0, 1.0f);
    float yaw = (float)(seed & 255) / 255.0f * DK_TAU;
    tk_place_prop((seed & 1) ? TKP_SAKURA_B : TKP_SAKURA_A, vadd(p, v3(0, 0.6f, 0)), yaw, 1.3f, 320.0f, (seed >> 1) & 1);
}

static void build_props(void) {
    /* Lamp poles down every row's centre line, arms across the row. */
    for(int r = 0; r < LOT_ROWS; r++)
        for(float x = -LOT_ROW_HX; x <= LOT_ROW_HX + 0.1f; x += LOT_POLE_STEP)
            pole(v3(x, LOT_FLOOR_Y, lot_row_z[r]), DK_PI * 0.5f, 30.0f);
    /* And round the ring road, just beyond the barrier, arms across it:
       only where the barrier is beside the road (along the deck it runs
       outside it, and the deck's own lamps light the road there). */
    for(float s = 20.0f; s < track.length - 10.0f; s += 46.0f) {
        if(lot_wall_d(s) > LOT_WALL + 0.1f) continue;
        V3 p = track_point(s, LOT_WALL + 1.2f);
        V3 t = track_tangent(s);
        p.y = LOT_FLOOR_Y;
        pole(p, atan2f(t.x, t.z), 26.0f);
    }
    /* And at the on-ramps, beside the barrier as it follows the lane
       out, so the slip road and the embankment are lit. */
    for(int f = 0; f < 2; f++) {
        float foot = track_seg_s(f ? 2 : 6);
        for(int i = 0; i < 2; i++) {
            float s = foot + (f ? 1.0f : -1.0f) * (i ? -24.0f : 6.0f);
            V3 p = track_point(s, lot_wall_d(s) + 1.2f), t = track_tangent(s);
            p.y = LOT_FLOOR_Y;
            pole(p, atan2f(t.x, t.z), 30.0f);
        }
    }

    /* The south side: the service bays, the tower and the clubhouse,
       their fronts to the lot, neon over them. */
    const float bz = -70.0f;
    tk_solid_prop(TKP_PITS, v3(-48.0f, LOT_FLOOR_Y, bz), DK_PI, 1.0f, 1600.0f, false);
    tk_solid_prop(TKP_PITS, v3(-23.4f, LOT_FLOOR_Y, bz), DK_PI, 1.0f, 1600.0f, false);
    /* The tower's stone plinth is modelled below its floor with a cap at
       floor level: sunk a hand so the cap lies under the lot's floor and
       the two never fight (it showed as a dark square blinking at the
       foot of the tower as the camera moved). */
    tk_solid_prop(TKP_TOWER, v3(0.0f, LOT_FLOOR_Y - 0.1f, bz - 2.0f), DK_PI, 1.0f, 1800.0f, false);
    av_solid_prop(AVP_CLUBHOUSE, v3(28.0f, LOT_FLOOR_Y, bz + 2.0f), DK_PI, 1.0f, 1400.0f, false);
    neon_sign(-36.0f, 5.4f, bz + 3.7f, 16.0f, 4.0f, 2, 3.5f);    /* NEON 24H over the bays, buzzing */
    neon_sign(0.0f, 9.5f, bz - 2.0f + 4.5f, 10.0f, 2.5f, 0, 0.0f); /* DREAMKARTS on the tower */
    neon_sign(28.0f, 3.3f, bz + 2.0f + 6.4f, 11.0f, 2.6f, 1, 16.0f); /* ARCADE on the clubhouse, over its old banner */
    /* Vending machines in a row under the bays' lights. */
    static const uint32_t bodies[5] = {0xfff0f0f4, 0xff3060d0, 0xffd03030, 0xfff0f0f4, 0xff30a060};
    static const uint32_t faces[5] = {0xffffffe0, 0xffa0d0ff, 0xffffc0b0, 0xffe0ffe0, 0xffd0ffd0};
    for(int i = 0; i < 5; i++) vending_machine(v3(-30.0f + 3.0f * (float)i, LOT_FLOOR_Y, bz + 6.5f), bodies[i], faces[i]);
    person(v3(-27.2f, LOT_FLOOR_Y, bz + 8.6f), DK_PI, 5);
    person(v3(-21.6f, LOT_FLOOR_Y, bz + 8.3f), DK_PI, 10);
    /* Market stalls by the clubhouse, tyre stacks marking their pitch. */
    static const int stalls[3] = {AVP_CANOPY_R, AVP_CANOPY_B, AVP_CANOPY_Y};
    for(int i = 0; i < 3; i++) av_solid_prop(stalls[i], v3(48.0f + 5.0f * (float)i, LOT_FLOOR_Y, bz + 12.0f), 0, 1.0f, 500.0f, false);
    for(int i = 0; i < 4; i++) {
        V3 p = v3(45.0f + 6.0f * (float)i, LOT_FLOOR_Y, bz + 16.0f);
        av_place_prop(AVP_TYRES, p, (float)i * 0.7f, 1.0f, 300.0f, false);
        roam_solid_disc(p, 0.45f, 1.1f);
    }
    person(v3(50.0f, LOT_FLOOR_Y, bz + 14.6f), DK_PI, 2);
    person(v3(54.6f, LOT_FLOOR_Y, bz + 14.2f), DK_PI, 7);
    person(v3(58.2f, LOT_FLOOR_Y, bz + 14.9f), DK_PI, 12);
    build_food_truck(v3(78.0f, LOT_FLOOR_Y, bz + 10.0f));

    /* The east end: trucks and containers, and bleachers for the crowd. */
    av_solid_prop(AVP_CONTAINER_R, v3(118.0f, LOT_FLOOR_Y, -46.0f), 0, 1.0f, 900.0f, false);
    av_solid_prop(AVP_CONTAINER_T, v3(118.0f, LOT_FLOOR_Y, -52.0f), 0, 1.0f, 900.0f, false);
    for(int i = 0; i < 3; i++) av_solid_prop(AVP_TRAILER, v3(124.0f, LOT_FLOOR_Y, -20.0f + 8.0f * (float)i), DK_PI * 0.5f, 1.0f, 800.0f, false);
    for(int i = 0; i < 2; i++) {
        av_solid_prop(AVP_STAND, v3(130.0f, LOT_FLOOR_Y, 8.0f + 15.0f * (float)i), DK_PI * 0.5f, 1.0f, 1200.0f, false);
        sway_last_prop();
    }
    /* Watching the ring road from the curb by the bleachers. */
    person(v3(138.4f, LOT_FLOOR_Y, 2.0f), DK_PI * 0.5f, 0);
    person(v3(138.6f, LOT_FLOOR_Y, 4.6f), DK_PI * 0.5f, 4);
    person(v3(138.3f, LOT_FLOOR_Y, 27.5f), DK_PI * 0.5f, 9);
    person(v3(138.7f, LOT_FLOOR_Y, 30.3f), DK_PI * 0.5f, 13);

    /* Cherry trees in planters along the quay side, between the overpass
       and the ring. */
    uint32_t seed = 0xda1cu;
    for(float x = -100.0f; x <= 100.0f; x += 20.0f) {
        seed = seed * 1664525u + 1013904223u;
        planter_tree(v3(x, LOT_FLOOR_Y, 78.0f), seed >> 8);
    }
    /* And a few at the west end, by the way in. */
    for(float z = -50.0f; z <= 50.0f; z += 25.0f) {
        seed = seed * 1664525u + 1013904223u;
        planter_tree(v3(-128.0f, LOT_FLOOR_Y, z), seed >> 8);
    }

    /* The start gantry over the ring road at the line, as the meet point. */
    add_gantry();
}

/* ------------------------------------------------------------------ */
/* Beyond the island: the city on the mainland and the bridge over the bay. */

static void build_city(void) {
    static const uint32_t crowns[4] = {0xff3cdcff, 0xffff32c8, 0xff9646ff, 0xffff78e6};
    uint32_t r = 0x7a1fu;
    V3 lot_c = v3(0, 0, 0);
    svp_tint = NULL;   /* the towers' windows are their own light */
    int n = 0;
    for(int tries = 0; tries < 300 && n < 26; tries++) {
        r = r * 1664525u + 1013904223u;
        float x = -620.0f + (float)((r >> 8) & 1023) / 1023.0f * 1240.0f;
        float z = -640.0f + (float)((r >> 18) & 1023) / 1023.0f * 560.0f;   /* -640 .. -80 */
        /* Behind the island only: south of it, or on the shore either side. */
        if(z > -260.0f && fabsf(x) < 300.0f) continue;
        float hs = 9.0f + (float)((r >> 4) & 15) * 0.7f;
        float top = 30.0f + (float)((r >> 12) & 63) * 1.4f;
        r = r * 1664525u + 1013904223u;
        float yaw = (float)((r >> 8) & 255) / 255.0f * 1.5f;
        sk_tower(v3(x, 0, z), hs, hs * (0.7f + 0.3f * (float)((r >> 16) & 3) / 3.0f), top, yaw, crowns[n & 3],
                 (n % 3 == 0) ? (n / 3) % 4 : -1, lot_c);
        n++;
    }
    svp_tint = lot_tint;
}

/* The bay bridge: a lit suspension bridge from the shore east of the
   island out over the water, seen through the fog as a string of lights. */
#define BRIDGE_LEN   760.0f
#define BRIDGE_Y     32.0f
#define BRIDGE_APPROACH 600.0f   /* the viaduct curving away from it, out into the fog */
static V3 bridge_a, bridge_dir;
static void build_bridge(void) {
    bridge_a = v3(300.0f, 0, -90.0f);
    bridge_dir = vnorm(v3(0.72f, 0, 0.69f));
    float yaw = atan2f(bridge_dir.x, bridge_dir.z);
    uint32_t steel = argb(255, 120, 130, 150), cable = argb(255, 200, 210, 230);
    svp_tint = NULL;
    /* The approach: a viaduct curving away from the bridge's end to the
       south-east and gently down, on pillars, running BRIDGE_APPROACH out
       so it is lost in the fog rather than seen to stop. Stations are
       walked back from the deck's end, turning steadily left (as seen
       going back), and the slab is ribbons: the top, the underside and
       both faces. */
    {
        const int steps = 24;
        const float step = BRIDGE_APPROACH / (float)steps, turn = step / 380.0f;   /* radius 380 m */
        V3 pos[steps + 1], rgt[steps + 1];
        float ys[steps + 1];
        V3 c = bridge_a, d = vscale(bridge_dir, -1.0f);
        for(int k = 0; k <= steps; k++) {
            float u = (float)k * step;
            pos[k] = c;
            rgt[k] = v3(d.z, 0, -d.x);
            ys[k] = lerpf(BRIDGE_Y, 9.0f, smooth01(60.0f, BRIDGE_APPROACH, u));
            V3 left = v3(-d.z, 0, d.x);
            d = vnorm(vmad(d, left, turn));
            c = vmad(c, d, step);
        }
        float hw = 9.0f;
        uint32_t ct = shade(steel, v3(0, 1, 0)), cs = color_scale(steel, 0.8f);
        for(int seg = 0; seg < steps; seg += 4) {
            chunk_begin(HF_OP, 6000.0f);
            for(int face = 0; face < 4; face++) {
                strip_begin(h_concrete);
                for(int k = seg; k <= seg + 4 && k <= steps; k++) {
                    V3 l = vmad(pos[k], rgt[k], -hw), r = vmad(pos[k], rgt[k], hw);
                    float top = ys[k] + 1.6f, bot = ys[k] - 1.6f, v = (float)k * step * 0.02f;
                    switch(face) {
                        case 0: svp(v3(l.x, top, l.z), 0, v, ct); svp(v3(r.x, top, r.z), 1, v, ct); break;
                        case 1: svp(v3(r.x, bot, r.z), 0, v, cs); svp(v3(l.x, bot, l.z), 1, v, cs); break;
                        case 2: svp(v3(l.x, top, l.z), 0, v, cs); svp(v3(l.x, bot, l.z), 0.3f, v, cs); break;
                        default: svp(v3(r.x, bot, r.z), 0, v, cs); svp(v3(r.x, top, r.z), 0.3f, v, cs); break;
                    }
                }
                strip_end();
            }
            chunk_end();
        }
        /* Pillars every other station, lights every third. */
        for(int k = 1; k <= steps; k++) {
            if(k % 2 == 0) {
                chunk_begin(HF_OP, 6000.0f);
                cylinder(h_concrete, v3(pos[k].x, LOT_FLOOR_Y, pos[k].z), 2.0f, 2.0f, ys[k] - 1.6f, 8, color_scale(steel, 0.7f),
                         steel, 2.0f, 4.0f);
                chunk_end();
            }
            if(k % 3 == 1) lamp(v3(pos[k].x, ys[k] + 6.0f, pos[k].z), 1.0f, 1);
        }
    }
    /* The deck in pieces. */
    for(float u = 0; u < BRIDGE_LEN; u += 95.0f) {
        float ub = fminf(u + 95.0f, BRIDGE_LEN);
        V3 c = vmad(bridge_a, bridge_dir, (u + ub) * 0.5f);
        chunk_begin(HF_OP, 6000.0f);
        box(h_concrete, v3(c.x, BRIDGE_Y, c.z), v3(9.0f, 1.6f, (ub - u) * 0.5f), yaw, steel, 0.05f);
        chunk_end();
    }
    /* Two towers, the main cables between and over them, and hangers. */
    float tw[2] = {BRIDGE_LEN * 0.28f, BRIDGE_LEN * 0.72f};
    const float top = BRIDGE_Y + 85.0f;
    for(int t = 0; t < 2; t++) {
        V3 c = vmad(bridge_a, bridge_dir, tw[t]);
        chunk_begin(HF_OP, 6000.0f);
        for(int side = -1; side <= 1; side += 2) {
            V3 leg = vmad(c, heading_right(yaw), side * 7.5f);
            box(h_concrete, v3(leg.x, (top + 2.0f) * 0.5f - 1.0f, leg.z), v3(2.2f, (top + 2.0f) * 0.5f + 1.0f, 2.2f), yaw, steel, 0.05f);
        }
        box(h_concrete, v3(c.x, top - 3.0f, c.z), v3(9.0f, 1.5f, 1.5f), yaw, steel, 0.05f);
        chunk_end();
        lamp(v3(c.x, top + 1.0f, c.z), 1.0f, 1);
    }
    chunk_begin(HF_OP, 6000.0f);
    for(int side = -1; side <= 1; side += 2) {
        V3 off = vscale(heading_right(yaw), side * 7.5f);
        /* The main cable: a sag between the towers, a drop to each anchorage. */
        strip_begin(h_lotcol);
        for(int k = 0; k <= 24; k++) {
            float u = BRIDGE_LEN * (float)k / 24.0f, y;
            if(u < tw[0]) y = lerpf(BRIDGE_Y + 3.0f, top, u / tw[0]);
            else if(u > tw[1]) y = lerpf(top, BRIDGE_Y + 3.0f, (u - tw[1]) / (BRIDGE_LEN - tw[1]));
            else {
                float f = (u - tw[0]) / (tw[1] - tw[0]);
                y = top - (top - BRIDGE_Y - 8.0f) * 4.0f * f * (1.0f - f);
            }
            V3 p = vadd(vmad(bridge_a, bridge_dir, u), off);
            svp(v3(p.x, y + 0.6f, p.z), 0, 0, cable);
            svp(v3(p.x, y - 0.6f, p.z), 0, 0, cable);
        }
        strip_end();
        /* Hangers every 38 m between the towers. */
        for(float u = tw[0] + 38.0f; u < tw[1] - 1.0f; u += 38.0f) {
            float f = (u - tw[0]) / (tw[1] - tw[0]);
            float y = top - (top - BRIDGE_Y - 8.0f) * 4.0f * f * (1.0f - f);
            V3 p = vadd(vmad(bridge_a, bridge_dir, u), off);
            strip_begin(h_lotcol);
            svp(v3(p.x - 0.25f, y, p.z), 0, 0, cable);
            svp(v3(p.x + 0.25f, y, p.z), 0, 0, cable);
            svp(v3(p.x - 0.25f, BRIDGE_Y + 1.6f, p.z), 0, 0, cable);
            svp(v3(p.x + 0.25f, BRIDGE_Y + 1.6f, p.z), 0, 0, cable);
            strip_end();
        }
    }
    chunk_end();
    /* Lights along the deck. */
    for(float u = 20.0f; u < BRIDGE_LEN; u += 40.0f) {
        V3 p = vmad(bridge_a, bridge_dir, u);
        lamp(v3(p.x, BRIDGE_Y + 6.0f, p.z), 1.0f, 1);
    }
    svp_tint = lot_tint;
}

/* ------------------------------------------------------------------ */

void build_lot(void) {
    roam_solids_clear();
    lamp_n = 0;
    people_n = sway_n = neon_n = 0;
    lot_rng = 0x5eedu;
    sk_road_hdr = *h_skyroad[0];
    /* The island's edges: the quays, and a line across the mainland
       short of the fog, which the ramps' gaps in the ring's barrier
       would otherwise let a kart drive out to. */
    roam_solid_box(v3(0, 0, LOT_QUAY_Z + 1.0f), LOT_ISLAND_X + 2.0f, 1.0f, 0, 60.0f);
    roam_solid_box(v3(0, 0, LOT_SOUTH_Z - 1.0f), LOT_ISLAND_X + 2.0f, 1.0f, 0, 60.0f);
    for(int side = -1; side <= 1; side += 2)
        roam_solid_box(v3(side * (LOT_ISLAND_X + 1.0f), 0, (LOT_QUAY_Z + LOT_SOUTH_Z) * 0.5f), 1.0f,
                       (LOT_QUAY_Z - LOT_SOUTH_Z) * 0.5f + 1.0f, 0, 60.0f);
    sk_beam_n = 0;   /* no searchlights here (a Skyway build before leaves its own) */
    /* The poles go in first (build_props places them before anything
       else), so everything built after is lit by their lamps; the ground
       and road come after the props for the same reason. */
    svp_tint = lot_tint;
#ifdef DK_CHUNK_STATS
    static const char *const names[CK_N] = {"props", "ground", "ring", "decks", "skyline"};
    ck_names = names;
    ck_first[0] = chunk_n;
#endif
    build_props();
    build_deck_underlamps();
#ifdef DK_CHUNK_STATS
    ck_first[1] = chunk_n;
#endif
    build_ground();
    build_bays();
    build_deck_bays();
#ifdef DK_CHUNK_STATS
    ck_first[2] = chunk_n;
#endif
    for(int c = 0; c < TRACK_SAMPLES / 8; c++) build_ring_chunk(c);
#ifdef DK_CHUNK_STATS
    ck_first[3] = chunk_n;
#endif
    build_deck_lamps();
    for(int c = 0; c < TRACK_SAMPLES / 8; c++) build_deck_chunk(c);
    build_deck_pillars();
    build_overpass();
#ifdef DK_CHUNK_STATS
    ck_first[4] = chunk_n;
#endif
    build_city();
    build_bridge();
#ifdef DK_CHUNK_STATS
    ck_first[CK_N] = chunk_n;
#endif
    svp_tint = NULL;
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DreamKarts lot: %d lamps, %d bystanders\n", lamp_n, people_n);
#endif
}

/* Each frame: the buzzing signs and the swaying crowd, written straight
   into the pool the chunks are drawn from. */
void lot_update(float dt) {
    for(int i = 0; i < neon_n; i++) {
        Neon *n = &neons[i];
        if(n->period <= 0) continue;
        uint32_t col = 0xffffffff;
        if(n->dim_t > 0) {
            n->dim_t -= dt;
            /* Out, with a sputter of light partway through. */
            col = fmodf(n->dim_t, 0.11f) < 0.03f ? argb(255, 190, 180, 210) : argb(255, 45, 40, 70);
            if(n->dim_t <= 0) n->next_t = n->period * (0.4f + 1.2f * lot_frand());
        }
        else if((n->next_t -= dt) <= 0) {
            n->dim_t = 0.12f + 0.5f * lot_frand();
        }
        for(int v = 0; v < 4; v++) pool[n->first + v].c = col;
    }
    bystanders_update(people, people_n, 0.02f);
    if(sway_n) {
        float wave[SWAY_PHASES];
        for(int i = 0; i < SWAY_PHASES; i++) wave[i] = 0.07f * sinf(world_time * 3.6f + (float)i * (DK_TAU / SWAY_PHASES));
        for(int i = 0; i < sway_n; i++) {
            const Sway *sw = &sways[i];
            for(int v = 0; v < sw->count; v++) pool[sw->first + v].y = sw->base_y[v] + wave[sw->phase[v]];
        }
    }
}

/* Put the first buzzing sign out for a while: the view tour's shot of it. */
void lot_neon_buzz(float seconds) {
    for(int i = 0; i < neon_n; i++)
        if(neons[i].period > 0) {
            neons[i].dim_t = seconds;
            return;
        }
}

/* The lamp heads glow; the bridge's lights are white. */
void lot_draw_tr(void) {
    float range = world_split ? 180.0f : 320.0f;
    r_hdr(h_glow_add);
    for(int i = 0; i < lamp_n; i++) {
        const Lamp *l = &lamps[i];
        V3 d = vsub(l->p, r_cam.pos);
        float d2 = vdot(d, d);
        if(l->kind == 0 && d2 > range * range) continue;
        if(!r_sphere_visible(l->p, 3.0f)) continue;
        if(l->kind) {
            r_billboard(l->p, 5.0f, 5.0f, 0, 0, 0, 1, 1, argb(200, 230, 240, 255));
        }
        else {
            float a = 0.55f + 0.05f * sinf(world_time * 7.0f + (float)i);
            r_billboard(l->p, 2.6f, 2.6f, 0, 0, 0, 1, 1, color_alpha(argb(255, 255, 190, 110), a));
        }
    }
}
