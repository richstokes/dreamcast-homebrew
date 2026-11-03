/* Temple Hill: switchbacks up a jungle hill to a stepped temple, a stone
   viaduct over a sea inlet, columns, stone heads and braziers. */
#include "world_int.h"
#include "kart.h"

#define TH_MONKEYS 8
#define MONKEY_SCALE 1.8f
#define MONKEY_BOXES 17

typedef struct {
    V3 seat;
    float yaw, head, excite, seed;
} Monkey;

static Monkey th_monkeys[TH_MONKEYS];
static int th_monkey_n;
static const pvr_poly_hdr_t *h_monkey;

static void th_monkey(V3 seat, int i);

/* The ground: the headland with a jungle hill rising to the south and a
   sea inlet cut into it, then carved around the road so the road always
   sits on top: within a bank's width of a barrier the ground drops just
   below the road, and beyond it may rise again at a 0.6 slope (cuttings
   between the switchbacks) or fall away under an embankment ribbon. */
#define TH_BANK     8.0f       /* embankment width beyond the barrier */
#define TH_SINK     0.4f       /* ground below the road at the bank's foot */
static float th_road_y[TRACK_SAMPLES];
static V3 th_inlet[5];
V3 th_flame_p[TH_FLAMES];
int th_flame_n;

float th_terrain(float x, float z) {
    float t = f8_ground(x, z) + 20.0f * smooth01(15.0f, 110.0f, -z);
    /* The inlet: a channel 5 m deep, walls easing up over 10 to 26 m. */
    V3 p = v3(x, 0, z);
    float di = 1e9f;
    for(int i = 0; i + 1 < 5; i++) di = fminf(di, seg_dist(p, th_inlet[i], th_inlet[i + 1]));
    t = fminf(t, -5.0f + 5.4f * smooth01(10.0f, 26.0f, di));
    /* Carve around the road. */
    if(x > -190.0f && x < 190.0f && z > -180.0f && z < 130.0f) {
        for(int i = 0; i < TRACK_SAMPLES; i++) {
            const TrackSample *a = &track.smp[i];
            float dx = x - a->p.x, dz = z - a->p.z;
            float lim = th_road_y[i] - TH_SINK;
            if(lim >= t) continue;
            float e = sqrtf(dx * dx + dz * dz) - (a->wall_out + TH_BANK);
            t = fminf(t, lim + 0.6f * fmaxf(0.0f, e));
        }
    }
    return t;
}

static void th_build_ground(void) {
    float xs[64], zs[48];
    int nx = 0, nz = 0;
    for(float x = -480.0f; x < -150.0f; x += 40.0f) xs[nx++] = x;
    for(float x = -150.0f; x < 150.0f; x += 15.0f) xs[nx++] = x;
    for(float x = 150.0f; x <= 480.0f; x += 40.0f) xs[nx++] = x;
    for(float z = -400.0f; z < -170.0f; z += 46.0f) zs[nz++] = z;
    for(float z = -170.0f; z < 100.0f; z += 15.0f) zs[nz++] = z;
    const float zn[] = {100.0f, 108.0f, 120.0f, 140.0f, 170.0f, 210.0f, 260.0f};
    for(unsigned i = 0; i < sizeof(zn) / sizeof(zn[0]); i++) zs[nz++] = zn[i];
    static float hy[48][64];
    for(int j = 0; j < nz; j++)
        for(int i = 0; i < nx; i++) hy[j][i] = th_terrain(xs[i], zs[j]) + F8_GRASS_Y;
    for(int cz = 0; cz + 1 < nz; cz += 4) {
        /* Chunks 8 cells wide: long strips cost the console less per cell. */
        for(int cx = 0; cx + 1 < nx; cx += 8) {
            chunk_begin(HF_OP, 2000.0f);
            bool far = true;
            for(int j = cz; j < cz + 4 && j + 1 < nz; j++) {
                float zm = (zs[j] + zs[j + 1]) * 0.5f;
                float xm = (xs[cx] + xs[cx + 8 < nx ? cx + 8 : nx - 1]) * 0.5f;
                float hm = 0.25f * (hy[j][cx] + hy[j + 1][cx] + hy[j][cx + 1] + hy[j + 1][cx + 1]);
                bool grassy = zm < 70.0f && hm > -0.1f;
                float uvs = grassy ? 6.0f : 4.0f;
                if(fabsf(xm) < 300.0f && zm > -220.0f && zm < 200.0f) far = false;
                strip_begin(grassy ? h_grass : h_sand);
                for(int i = cx; i <= cx + 8 && i < nx; i++) {
                    for(int k = 1; k >= 0; k--) {
                        float x = xs[i], z = zs[j + k], y = hy[j + k][i];
                        uint32_t col = ground_color(y, 0.0f, grassy);
                        /* Shade slopes a touch so the hill reads. */
                        if(grassy && i + 1 < nx) {
                            float gx = (hy[j + k][i + 1] - y) / (xs[i + 1] - x);
                            col = color_scale(col, clampf(1.0f + gx * 0.5f, 0.8f, 1.08f));
                        }
                        svp(v3(x, y, z), x / uvs, z / uvs, col);
                    }
                }
                strip_end();
            }
            if(cur_chunk) cur_chunk->far = far;
            chunk_end();
        }
    }
}

/* Embankments: grass ribbons from each barrier's foot down to the carved
   ground, left open across the viaduct. */
void th_banks(const float *sv, int n) {
    for(int side = -1; side <= 1; side += 2) {
        bool open = false;
        for(int k = 0; k < n; k++) {
            float s = sv[k];
            bool skip = in_bridge(s) || (k + 1 < n && in_bridge((s + sv[k + 1]) * 0.5f) &&
                                         in_bridge(sv[k + 1]));
            if(skip) {
                if(open) strip_end();
                open = false;
                continue;
            }
            if(!open) {
                strip_begin(h_grass);
                open = true;
            }
            float w = f8_wall(s);
            V3 a = f8_pt(s, side * (w + 0.25f), 0.0f);
            V3 b = track_point(s, side * (w + TH_BANK));
            b.y = th_terrain(b.x, b.z) + F8_GRASS_Y - 0.15f;
            uint32_t ca = argb(255, 226, 232, 222), cb = argb(255, 246, 252, 240);
            if(side > 0) {
                svp(a, a.x / 6.0f, a.z / 6.0f, ca);
                svp(b, b.x / 6.0f, b.z / 6.0f, cb);
            }
            else {
                svp(b, b.x / 6.0f, b.z / 6.0f, cb);
                svp(a, a.x / 6.0f, a.z / 6.0f, ca);
            }
        }
        if(open) strip_end();
    }
}

/* The viaduct: three stone arches on two piers between abutments, with
   spandrel walls under the parapets and a vaulted underside. */
static void th_viaduct(void) {
    const float bs = track.bridge_s, half = track.bridge_half, base = -1.6f;
    const float edges[] = {-half, -10.0f, -7.0f, 7.0f, 10.0f, half};   /* opening, pier, opening, ... */
    chunk_begin(HF_OP, 2000.0f);
    float w = f8_wall(bs) + 0.25f;
    for(int o = 0; o < 3; o++) {
        float a = edges[o * 2], b = edges[o * 2 + 1];
        const int segs = 9;
        float sl[segs + 1], yl[segs + 1];
        for(int k = 0; k <= segs; k++) {
            float t = (float)k / segs;
            sl[k] = track_wrap(bs + lerpf(a, b, t));
            float crown = track_height(sl[k]) - 1.6f;
            yl[k] = base + (crown - base) * sqrtf(sinf(DK_PI * t));
        }
        /* Vault. */
        strip_begin(h_stone);
        for(int k = 0; k <= segs; k++) {
            V3 l = track_point(sl[k], -w), r = track_point(sl[k], w);
            l.y = r.y = yl[k];
            uint32_t c = color_scale(0xffffffff, 0.62f);
            svp(l, 0, sl[k] / 4.0f, c);
            svp(r, 2.0f * w / 4.0f, sl[k] / 4.0f, c);
        }
        strip_end();
        /* Spandrels, both faces of the bridge. */
        for(int side = -1; side <= 1; side += 2) {
            strip_begin(h_stone);
            for(int k = 0; k <= segs; k++) {
                V3 top = track_point(sl[k], side * w), bot = top;
                top.y = track_height(sl[k]);
                bot.y = yl[k];
                V3 r = vsub(track_point(sl[k], 1.0f), track_point(sl[k], 0.0f));
                uint32_t c = shade(argb(255, 255, 246, 230), vscale(r, (float)side));
                svp(top, sl[k] / 4.0f, top.y / 4.0f, c);
                svp(bot, sl[k] / 4.0f, bot.y / 4.0f, color_scale(c, 0.85f));
            }
            strip_end();
        }
    }
    /* Pier sides between the arches. */
    for(int pi = 0; pi < 2; pi++) {
        float a = track_wrap(bs + edges[1 + pi * 2]), b = track_wrap(bs + edges[2 + pi * 2]);
        for(int side = -1; side <= 1; side += 2) {
            V3 ta = track_point(a, side * w), tb = track_point(b, side * w);
            ta.y = track_height(a);
            tb.y = track_height(b);
            V3 ba = ta, bb = tb;
            ba.y = bb.y = base;
            quad(h_stone, ta, tb, ba, bb, 0, ta.y / 4.0f, 0.75f, base / 4.0f, argb(255, 255, 246, 230));
        }
        /* Cutwater: a pointed buttress on each face. */
        float m = track_wrap(bs + (edges[1 + pi * 2] + edges[2 + pi * 2]) * 0.5f);
        for(int side = -1; side <= 1; side += 2) {
            V3 c = track_point(m, side * (w + 1.2f));
            box(h_stone, v3(c.x, (base + track_height(m) - 1.0f) * 0.5f, c.z),
                v3(1.0f, (track_height(m) - 1.0f - base) * 0.5f, 1.2f), atan2f(track_tangent(m).x, track_tangent(m).z),
                argb(255, 236, 226, 206), 0.25f);
        }
    }
    /* Abutment faces closing the embankments at each end. */
    for(int e = -1; e <= 1; e += 2) {
        float se = track_wrap(bs + e * half);
        float h = track_height(se);
        strip_begin(h_stone);
        for(int k = -3; k <= 3; k++) {
            float d = k == 0 ? 0.0f : (float)(k < 0 ? -1 : 1) * (k * k == 1 ? w : k * k == 4 ? w + TH_BANK * 0.5f : w + TH_BANK);
            V3 top = track_point(se, (float)e * d);
            float y = fabsf(d) <= w + 0.01f ? h : lerpf(h, th_terrain(top.x, top.z) + F8_GRASS_Y, (fabsf(d) - w) / TH_BANK);
            top.y = y;
            V3 bot = top;
            bot.y = base;
            svp(top, d / 4.0f, y / 4.0f, argb(255, 236, 226, 206));
            svp(bot, d / 4.0f, base / 4.0f, argb(255, 200, 190, 170));
        }
        strip_end();
    }
    chunk_end();
}

/* A stepped temple with a shrine on top and a stair up its north face. */
static void th_temple(V3 c) {
    chunk_begin(HF_OP, 2000.0f);
    const int tiers = 5;
    float y = c.y - 2.0f, half = 18.0f;
    for(int t = 0; t < tiers; t++) {
        float th = t == 0 ? 5.0f : 3.0f;
        box(h_stone, v3(c.x, y + th * 0.5f, c.z), v3(half, th * 0.5f, half), 0, argb(255, 255, 248, 232), 0.22f);
        y += th;
        half -= 3.0f;
    }
    /* Shrine: walls, a dark doorway and a stone roof slab. */
    box(h_stone, v3(c.x, y + 2.2f, c.z), v3(4.2f, 2.2f, 4.2f), 0, argb(255, 255, 240, 214), 0.3f);
    quad(h_col, v3(c.x - 1.4f, y + 3.2f, c.z + 4.25f), v3(c.x + 1.4f, y + 3.2f, c.z + 4.25f), v3(c.x - 1.4f, y, c.z + 4.25f),
         v3(c.x + 1.4f, y, c.z + 4.25f), 0, 0, 1, 1, argb(255, 40, 30, 24));
    box(h_stone, v3(c.x, y + 4.8f, c.z), v3(5.4f, 0.4f, 5.4f), 0, argb(255, 230, 214, 180), 0.3f);
    th_flame_p[th_flame_n++] = v3(c.x - 4.6f, y + 1.5f, c.z + 5.4f);
    th_flame_p[th_flame_n++] = v3(c.x + 4.6f, y + 1.5f, c.z + 5.4f);
    /* The great stair, a ramp of steps on the north face. */
    float top_y = y;
    strip_begin(h_stone);
    for(int k = 0; k <= 1; k++) {
        float yy = k ? top_y : c.y - 2.0f;
        float zz = k ? c.z + 3.0f + 4.25f : c.z + 18.0f + 6.0f;
        svp(v3(c.x - 3.5f, yy, zz), 0, yy * 1.5f, argb(255, 220, 206, 176));
        svp(v3(c.x + 3.5f, yy, zz), 1.5f, yy * 1.5f, argb(255, 220, 206, 176));
    }
    strip_end();
    chunk_end();
}

/* A carved stone head on a plinth, facing yaw. */
static void th_head(V3 base, float yaw, float size) {
    chunk_begin(HF_OP, 260.0f);
    V3 f = heading_fwd(yaw);
    uint32_t stone = argb(255, 214, 204, 186);
    box(h_stone, vadd(base, v3(0, 0.6f * size, 0)), vscale(v3(1.4f, 0.6f, 1.4f), size), yaw, stone, 0.4f);
    box(h_rock, vadd(base, v3(0, 3.0f * size, 0)), vscale(v3(1.0f, 1.8f, 0.9f), size), yaw, stone, 0.35f);
    /* Brow, nose and mouth stand proud of the face. */
    V3 face = vmad(base, f, 0.95f * size);
    box(h_rock, vadd(face, v3(0, 3.9f * size, 0)), vscale(v3(1.05f, 0.22f, 0.25f), size), yaw, color_scale(stone, 0.9f), 0.5f);
    box(h_rock, vadd(vmad(face, f, 0.2f * size), v3(0, 3.1f * size, 0)), vscale(v3(0.22f, 0.65f, 0.3f), size), yaw, stone, 0.5f);
    box(h_col, vadd(face, v3(0, 2.1f * size, 0)), vscale(v3(0.55f, 0.1f, 0.12f), size), yaw, argb(255, 60, 48, 40), 1);
    for(int e = -1; e <= 1; e += 2)
        box(h_col, vadd(vmad(face, heading_right(yaw), 0.42f * size * e), v3(0, 3.55f * size, 0)),
            vscale(v3(0.2f, 0.14f, 0.1f), size), yaw, argb(255, 40, 34, 30), 1);
    chunk_end();
}

/* An ancient column: plinth, fluted shaft (maybe broken off) and capital. */
static void th_column(V3 base, float height, bool broken) {
    chunk_begin(HF_OP, 220.0f);
    uint32_t col = argb(255, 246, 236, 214);
    box(h_stone, vadd(base, v3(0, 0.35f, 0)), v3(1.2f, 0.35f, 1.2f), 0, col, 0.4f);
    cylinder(h_stone, vadd(base, v3(0, 0.7f, 0)), 0.85f, 0.75f, height, 8, color_scale(col, 0.85f), col, 2.0f, height / 3.0f);
    if(!broken)
        box(h_stone, vadd(base, v3(0, 0.7f + height + 0.3f, 0)), v3(1.15f, 0.3f, 1.15f), 0, col, 0.4f);
    chunk_end();
}

/* A brazier: a stone pedestal with a fire bowl; the flame is drawn live. */
static void th_brazier(V3 base) {
    chunk_begin(HF_OP, 180.0f);
    box(h_stone, vadd(base, v3(0, 0.8f, 0)), v3(0.45f, 0.8f, 0.45f), 0, argb(255, 236, 226, 206), 0.5f);
    cylinder(h_col, vadd(base, v3(0, 1.6f, 0)), 0.35f, 0.8f, 0.5f, 8, argb(255, 90, 70, 50), argb(255, 130, 100, 60), 1, 1);
    chunk_end();
    if(th_flame_n < TH_FLAMES) th_flame_p[th_flame_n++] = vadd(base, v3(0, 2.4f, 0));
}

/* A tumbled ruin wall: a few offset courses of blocks. */
static void th_ruin(V3 c, float yaw, uint32_t seed) {
    chunk_begin(HF_OP, 170.0f);
    V3 ax = heading_right(yaw);
    for(int row = 0; row < 3; row++) {
        for(int i = 0; i < 4 - row; i++) {
            seed = seed * 1103515245u + 12345u;
            if(row > 0 && ((seed >> 16) & 3) == 0) continue;
            V3 p = vmad(c, ax, ((float)i - (3 - row) * 0.5f) * 2.1f + (row & 1) * 0.5f);
            p.y = c.y + 0.5f + row * 1.0f;
            box(h_stone, p, v3(1.0f, 0.5f, 0.7f), yaw + 0.05f * (float)((seed >> 20) % 5 - 2), argb(255, 230, 220, 200), 0.4f);
        }
    }
    chunk_end();
}

static V3 th_ground(float x, float z) {
    return v3(x, th_terrain(x, z) + F8_GRASS_Y, z);
}

static void th_place_props(void) {
    add_gantry();
    add_grandstand(-30.0f, 40.0f, 46.5f, true);
    th_viaduct();
    th_temple(th_ground(-4.0f, -152.0f));

    /* Braziers at the start line and both ends of the viaduct. */
    const TrackSample *t0 = &track.smp[0];
    for(int side = -1; side <= 1; side += 2) {
        V3 p = vmad(vmad(t0->p, t0->r, side * 14.5f), t0->t, 6.0f);
        th_brazier(th_ground(p.x, p.z));
        for(int e = -1; e <= 1; e += 2) {
            float se = track.bridge_s + e * (track.bridge_half + 4.0f);
            V3 q = track_point(se, side * 14.0f);
            q.y = th_terrain(q.x, q.z) + F8_GRASS_Y;
            q.y = fmaxf(q.y, track_height(se) - 0.2f);
            th_brazier(q);
        }
    }

    /* A colonnade along the summit ridge, some columns fallen short. They
       stand at the ridge's level, not on the hump, which only lifts the
       road: one raised with it shows its plinth floating over the barrier. */
    float ridge = track_seg_s(8);
    float ridge_y = track_height(ridge + 6.0f) - 0.2f;
    for(int i = 0; i < 9; i++) {
        float s = ridge + 6.0f + i * 14.0f;
        V3 p = track_point(s, 17.5f);
        p.y = ridge_y;
        bool broken = (i % 3) == 1;
        float h = broken ? 2.0f + (float)(i % 2) * 1.5f : 6.5f;
        th_column(p, h, broken);
        if(broken) th_monkey(vadd(p, v3(0, 0.7f + h, 0)), 3 + i);
    }
    /* Stone heads watch the hairpins and the summit. */
    for(int hp = 0; hp < 2; hp++) {
        float s = track_seg_s(5 + hp * 2) + 26.7f;          /* hairpin apex */
        float d = hp == 0 ? -20.0f : 20.0f;                  /* outside of the bend */
        V3 p = track_point(s, d);
        V3 toward = vsub(track_point(s, 0), p);
        th_head(th_ground(p.x, p.z), atan2f(toward.x, toward.z), 1.6f);
        th_monkey(vadd(th_ground(p.x, p.z), v3(0, 4.8f * 1.6f, 0)), hp);
        th_brazier(th_ground(track_point(s + 9.0f, d).x, track_point(s + 9.0f, d).z));
    }
    {
        float s = ridge + 95.0f;
        V3 p = track_point(s, -19.0f);
        V3 toward = vsub(track_point(s, 0), p);
        V3 g = th_ground(p.x, p.z);
        g.y = fmaxf(g.y, track_height(s) - 0.5f);
        th_head(g, atan2f(toward.x, toward.z), 2.0f);
        th_monkey(vadd(g, v3(0, 4.8f * 2.0f, 0)), 2);
    }
    /* Tumbled walls on the terraces and in the infield. */
    const float ruins[][3] = {{-40, -2, 0.3f}, {20, 12, -0.5f}, {-100, -40, 1.2f}, {70, -60, 0.2f}, {-60, -120, 0.9f}};
    for(int i = 0; i < 5; i++) {
        V3 p = th_ground(ruins[i][0], ruins[i][1]);
        if(!hb_clear(p, 2.0f)) continue;
        th_ruin(p, ruins[i][2], (uint32_t)(i * 17 + 3));
        /* On the end block of the bottom course, clear of the one above. */
        if(i < 2) th_monkey(vadd(vmad(p, heading_right(ruins[i][2]), -3.6f), v3(0, 1.0f, 0)), 12 + i);
    }
    for(int i = 0; i < 6; i++) {
        float a = (float)i / 6.0f * DK_TAU;
        V3 p = th_ground(-20.0f + cosf(a) * 26.0f, 8.0f + sinf(a) * 16.0f);
        if(hb_clear(p, 2.0f)) th_column(p, (i & 1) ? 3.0f : 5.5f, i & 1);
    }

    /* Jungle palms, kept behind the barriers and off the inlet. */
    uint32_t r = 0x7e3au;
    for(int tries = 0; tries < 500 && palm_n < 42; tries++) {
        r = r * 1664525u + 1013904223u;
        float x = -160.0f + (float)((r >> 8) & 1023) / 1023.0f * 320.0f;
        float z = -175.0f + (float)((r >> 18) & 1023) / 1023.0f * 268.0f;
        V3 p = th_ground(x, z);
        if(p.y < WATER_Y + 0.2f || !hb_clear(p, 3.0f)) continue;
        if(z > 28.0f && z < 50.0f && x > -36.0f && x < 46.0f) continue;     /* grandstand */
        if(fabsf(x + 4.0f) < 28.0f && z < -128.0f) continue;                /* temple */
        bool crowded = false;
        for(int i = 0; i < palm_n; i++) {
            V3 d = vsub(palms[i].base, p);
            if(d.x * d.x + d.z * d.z < 11.0f * 11.0f) crowded = true;
        }
        if(crowded) continue;
        add_palm(p, 9.0f + (float)((r >> 4) % 4) * 1.3f, (float)(r % 628) * 0.01f, 0.2f, r >> 12);
    }

    /* The beach and the sea. */
    add_umbrella(th_ground(-40.0f, 90.0f), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(th_ground(10.0f, 92.0f), argb(255, 40, 200, 160), 0xffffffff);
    add_umbrella(th_ground(60.0f, 89.0f), argb(255, 255, 150, 30), 0xffffffff);
    add_rock(v3(-140, 0, 200), 12.0f, 14.0f, 9, true);
    add_rock(v3(60, 0, 250), 15.0f, 20.0f, 5, true);
    add_rock(v3(250, 0, 140), 10.0f, 10.0f, 17, true);
    for(int i = 0; i < 4; i++) {
        V3 p = vlerp(th_inlet[1], th_inlet[3], (float)i / 3.0f);
        add_rock(vadd(p, v3((i & 1) ? 24.0f : -24.0f, 0, (i & 1) ? -6.0f : 6.0f)), 4.0f, 2.5f, (uint32_t)(i * 7 + 1), false);
    }
    add_boat(v3(-60, WATER_Y, 300), 0.8f, argb(255, 30, 60, 140));
    add_boat(v3(160, WATER_Y, 420), -0.4f, argb(255, 200, 40, 40));

    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        float x = -420.0f + (float)(h & 1023) / 1023.0f * 840.0f;
        float z = -190.0f - (float)((h >> 10) & 255) / 255.0f * 180.0f;
        far_sprites[far_sprite_n] = th_ground(x, z);
        far_sprite_h[far_sprite_n] = 9.0f + (float)((h >> 18) & 7);
        far_sprite_n++;
    }
    f8_foam();
}

void build_temple(void) {
    f8m_default();
    gnd_xw = 170.0f;
    gnd_zn = 84.0f;
    gnd_zs = 125.0f;
    th_flame_n = 0;
    th_monkey_n = 0;
    for(int i = 0; i < TRACK_SAMPLES; i++) th_road_y[i] = track_height(track.smp[i].s);
    /* The inlet runs east under the viaduct, then north out to the sea. */
    V3 b = track_point(track.bridge_s, 0);
    th_inlet[0] = v3(b.x - 34.0f, 0, b.z);
    th_inlet[1] = v3(b.x + 10.0f, 0, b.z);
    th_inlet[2] = v3(b.x + 44.0f, 0, b.z + 6.0f);
    th_inlet[3] = v3(b.x + 86.0f, 0, b.z + 58.0f);
    th_inlet[4] = v3(b.x + 92.0f, 0, b.z + 190.0f);
    th_build_ground();
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        f8_build_track_chunk(c);
    th_place_props();
}

void th_draw_flames(void) {
    if(!th_flame_n) return;
    /* Spiral Canyon's split screen draws its torch flames half as far and
       without the glow (it draws everything twice); Temple Hill's braziers
       stay as they are. */
    bool lite = world_split && world_canyon;
    float range = lite ? 130.0f : 260.0f;
    r_hdr(h_flame);
    for(int i = 0; i < th_flame_n; i++) {
        V3 p = th_flame_p[i];
        V3 d = vsub(p, r_cam.pos);
        if(vdot(d, d) > range * range || !r_sphere_visible(p, 2.0f)) continue;
        float f = sinf(world_time * 13.0f + i * 2.1f) * 0.5f + sinf(world_time * 7.3f + i) * 0.5f;
        float h = 1.9f + 0.3f * f, w = 1.25f + 0.12f * f;
        r_billboard_y(vsub(p, v3(0, 0.75f, 0)), w, h, 0, 0, 1, 1, argb(255, 255, 200, 120));
    }
    if(lite) return;
    r_hdr(h_glow_add);
    for(int i = 0; i < th_flame_n; i++) {
        V3 p = th_flame_p[i];
        V3 d = vsub(p, r_cam.pos);
        if(vdot(d, d) > range * range || !r_sphere_visible(p, 3.0f)) continue;
        float a = 0.28f + 0.06f * sinf(world_time * 11.0f + i * 1.7f);
        r_billboard(p, 4.5f, 4.5f, 0, 0, 0, 1, 1, color_alpha(argb(255, 255, 150, 60), a));
    }
}

/* Monkeys on the ruins: one on each stone head, on the broken columns of
   the summit colonnade and on two of the tumbled walls. Each sits facing
   the road, tail swaying, now and then scratching its head; it watches the
   nearest kart go by and bounces with its arms up when one comes close. */

static void th_monkey(V3 seat, int i) {
    if(th_monkey_n >= TH_MONKEYS) return;
    float best = 1e9f;
    V3 to = v3(0, 0, 1);
    for(int k = 0; k < TRACK_SAMPLES; k++) {
        V3 d = vsub(track.smp[k].p, seat);
        float q = d.x * d.x + d.z * d.z;
        if(q < best) {
            best = q;
            to = d;
        }
    }
    th_monkeys[th_monkey_n++] = (Monkey){seat, atan2f(to.x, to.z), 0, 0, (float)i * 1.37f};
}

void th_init(void) {
    h_monkey = r_header(TEX_NONE, HF_OP);
}

void th_update(float dt) {
    for(int i = 0; i < th_monkey_n; i++) {
        Monkey *m = &th_monkeys[i];
        float best = 60.0f * 60.0f, look = m->yaw + 0.6f * sinf(world_time * 0.5f + m->seed);
        for(int k = 0; k < MAX_KARTS; k++) {
            V3 d = vsub(karts[k].pos, m->seat);
            float q = d.x * d.x + d.z * d.z;
            if(q < best) {
                best = q;
                look = atan2f(d.x, d.z);
            }
        }
        /* Turn the head (not past the shoulders) towards it. */
        float rel = clampf(wrap_angle(look - m->yaw), -1.3f, 1.3f);
        m->head += (rel - m->head) * fminf(dt * 6.0f, 1.0f);
        float want = best < 24.0f * 24.0f ? 1.0f : 0.0f;
        m->excite += (want - m->excite) * fminf(dt * (want > m->excite ? 6.0f : 1.5f), 1.0f);
    }
}

/* A box of the monkey: pivot and its rotations (rx about x, then ry about
   y), offset and half size in the pivot's frame. Faces shaded by facing. */
static int mk_box(RV *v, int n, V3 pivot, float rx, float ry, V3 off, V3 half, uint32_t col) {
    static const int8_t fc[6][4][3] = {
        {{-1, 1, -1}, {1, 1, -1}, {-1, 1, 1}, {1, 1, 1}},     {{-1, -1, 1}, {1, -1, 1}, {-1, -1, -1}, {1, -1, -1}},
        {{-1, 1, 1}, {1, 1, 1}, {-1, -1, 1}, {1, -1, 1}},     {{1, 1, -1}, {-1, 1, -1}, {1, -1, -1}, {-1, -1, -1}},
        {{-1, 1, -1}, {-1, 1, 1}, {-1, -1, -1}, {-1, -1, 1}}, {{1, 1, 1}, {1, 1, -1}, {1, -1, 1}, {1, -1, -1}},
    };
    static const float shade[6] = {1.0f, 0.6f, 0.9f, 0.75f, 0.8f, 0.8f};
    float cx = cosf(rx), sx = sinf(rx), cy = cosf(ry), sy = sinf(ry);
    for(int f = 0; f < 6; f++) {
        uint32_t c = color_scale(col, shade[f]);
        for(int k = 0; k < 4; k++) {
            float x = off.x + fc[f][k][0] * half.x, y = off.y + fc[f][k][1] * half.y, z = off.z + fc[f][k][2] * half.z;
            float y1 = y * cx - z * sx, z1 = y * sx + z * cx;
            float x2 = x * cy + z1 * sy, z2 = -x * sy + z1 * cy;
            v[n++] = (RV){pivot.x + x2, pivot.y + y1, pivot.z + z2, 0, 0, c};
        }
    }
    return n;
}

/* Detail 0, the whole monkey; 1, just its body, head, face, arms and
   tail, about a third of the vertices; 2, far off, body, head and face.
   Drawn whole out to 140 m they cost the console ~0.5 ms a frame on
   Temple Hill. */
static void th_draw_monkey(const Monkey *m, int detail, float scale) {
    bool lite = detail > 0, far = detail > 1;
    static RV v[MONKEY_BOXES * 24] __attribute__((aligned(32)));
    static uint16_t idx[MONKEY_BOXES * 36];
    static bool idx_built;
    if(!idx_built) {
        for(int q = 0; q < MONKEY_BOXES * 6; q++) {
            uint16_t a = (uint16_t)(q * 4);
            uint16_t t[6] = {a, (uint16_t)(a + 1), (uint16_t)(a + 2), (uint16_t)(a + 1), (uint16_t)(a + 3), (uint16_t)(a + 2)};
            for(int k = 0; k < 6; k++) idx[q * 6 + k] = t[k];
        }
        idx_built = true;
    }
    const uint32_t fur = argb(255, 112, 70, 38), skin = argb(255, 222, 184, 140), eye = argb(255, 30, 22, 18);
    float t = world_time + m->seed, ex = m->excite;
    /* Idle: a scratch of the head with the right arm every few seconds.
       Excited: both arms up, waving out of step. */
    float scratch = fmaxf(0.0f, sinf(t * 0.7f) - 0.8f) * 5.0f;
    float arm_l = lerpf(0.35f, 2.7f + 0.3f * sinf(t * 12.0f), ex);
    float arm_r = lerpf(0.35f + 2.0f * scratch, 2.7f + 0.3f * sinf(t * 12.0f + 2.0f), ex);
    float hop = 0.12f * ex * fabsf(sinf(t * 9.0f));
    float tail = 0.5f * sinf(t * 1.6f);
    int n = 0;
    V3 o = v3(0, hop, 0);
    n = mk_box(v, n, o, 0, 0, v3(0, 0.38f, 0), v3(0.17f, 0.2f, 0.13f), fur);
    if(!lite) n = mk_box(v, n, o, 0, 0, v3(0, 0.35f, 0.11f), v3(0.11f, 0.14f, 0.03f), skin);
    for(int s = -1; s <= 1; s += 2) {
        if(!lite) n = mk_box(v, n, o, 0, 0, v3(s * 0.11f, 0.08f, 0.1f), v3(0.07f, 0.07f, 0.14f), fur);
        V3 sh = vadd(o, v3(s * 0.21f, 0.52f, 0));
        float a = s < 0 ? arm_l : arm_r;
        if(!far) n = mk_box(v, n, sh, -a, 0, v3(0, -0.2f, 0), v3(0.05f, 0.2f, 0.05f), fur);
        if(!lite) n = mk_box(v, n, sh, -a, 0, v3(0, -0.37f, 0), v3(0.05f, 0.05f, 0.05f), skin);
    }
    V3 neck = vadd(o, v3(0, 0.6f, 0.02f));
    float hy = m->head;
    n = mk_box(v, n, neck, 0, hy, v3(0, 0.15f, 0), v3(0.15f, 0.14f, 0.13f), fur);
    n = mk_box(v, n, neck, 0, hy, v3(0, 0.12f, 0.12f), v3(0.1f, 0.09f, 0.03f), skin);
    if(!lite) n = mk_box(v, n, neck, 0, hy, v3(0, 0.07f, 0.155f), v3(0.06f, 0.04f, 0.03f), color_scale(skin, 1.08f));
    for(int s = -1; s <= 1 && !lite; s += 2) {
        n = mk_box(v, n, neck, 0, hy, v3(s * 0.045f, 0.17f, 0.152f), v3(0.02f, 0.02f, 0.01f), eye);
        n = mk_box(v, n, neck, 0, hy, v3(s * 0.17f, 0.17f, 0), v3(0.035f, 0.05f, 0.02f), skin);
    }
    V3 root = vadd(o, v3(0, 0.22f, -0.12f));
    if(!far) n = mk_box(v, n, root, 0, tail, v3(0, 0, -0.15f), v3(0.03f, 0.03f, 0.15f), fur);
    if(!lite) n = mk_box(v, n, root, 0, tail, v3(0, 0.15f, -0.3f), v3(0.03f, 0.15f, 0.03f), fur);
    float mt[16];
    m4_translate_rotate(mt, m->seat, m->yaw, 0, 0);
    m4_scale_apply(mt, scale, scale, scale);
    r_load_model(mt);
    r_mesh(v, n, idx, n / 4 * 6, false);
}

void th_draw_op(void) {
    if(!th_monkey_n) return;
    /* Out to about where the heads and columns they sit on are drawn,
       growing in over the last 30 m so they never pop into view. */
    float range = world_split ? 120.0f : 200.0f, near = world_split ? 25.0f : 35.0f, lite = world_split ? 60.0f : 80.0f;
    r_hdr(h_monkey);
    for(int i = 0; i < th_monkey_n; i++) {
        const Monkey *m = &th_monkeys[i];
        V3 d = vsub(m->seat, r_cam.pos);
        float q = vdot(d, d);
        if(q > range * range || !r_sphere_visible(vadd(m->seat, v3(0, 0.8f, 0)), 1.5f)) continue;
        float dist = sqrtf(q), grow = clampf((range - dist) / 30.0f, 0, 1);
        th_draw_monkey(m, dist < near ? 0 : dist < lite ? 1 : 2, MONKEY_SCALE * grow);
    }
    r_load_vp();
}
