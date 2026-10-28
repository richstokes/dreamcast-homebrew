/* The road ribbon builder: the lap distances a chunk of road is built at,
   ribbons along them, and the road of the tracks on the ground (asphalt,
   curbs, shoulders, barriers and skirts, styled by f8m). The sky decks
   (world_skyway.c) build theirs from the same ribbons. */
#include "world_int.h"

/* Lap distances for chunk c's road: the regular samples plus the jump's
   profile breakpoints (so the lip and deck edges are exact) or a viaduct's
   ends, with extra samples where the road's height bends sharply. */
int f8_chunk_svals(int c, float *out) {
    const int per = 8;
    float s0 = (float)(c * per) * track.ds, s1 = (float)((c + 1) * per) * track.ds;
    int n = 0;
    for(int j = 0; j <= per; j++) out[n++] = (float)(c * per + j) * track.ds;
    if(track.has_jump || track.has_bridge) {
        const Jump *jp = &track.jump;
        float marks[6], at;
        int nm = 0;
        if(track.has_jump) {
            at = jp->cross;
            marks[nm++] = jp->ramp; marks[nm++] = jp->lip - 0.01f; marks[nm++] = jp->lip;
            marks[nm++] = jp->land; marks[nm++] = jp->land + 0.01f; marks[nm++] = jp->foot;
        }
        else {
            at = track.bridge_s;
            marks[nm++] = -track.bridge_half;
            marks[nm++] = track.bridge_half;
        }
        for(int m = 0; m < nm; m++) {
            float s = at + marks[m];
            while(s < s0) s += track.length;
            while(s >= s0 + track.length) s -= track.length;
            if(s <= s0 || s >= s1) continue;
            int k = n++;
            while(k > 0 && out[k - 1] > s) {
                out[k] = out[k - 1];
                k--;
            }
            out[k] = s;
        }
    }
    /* Through tight bends (Apple Valley's 14 m hairpins), halve intervals
       that turn more than 13 degrees, so the curbs and barriers stay round. */
    for(int pass = 0; pass < 2 && track.fine_bends; pass++) {
        for(int k = 0; k + 1 < n && n < MAX_SVALS; k++) {
            float a = out[k], b = out[k + 1], m = (a + b) * 0.5f;
            if(fabsf(track_curvature(m)) * (b - a) < 0.22f) continue;
            for(int q = n; q > k + 1; q--) out[q] = out[q - 1];
            out[k + 1] = m;
            n++;
            k++;
        }
    }
    /* Halve intervals whose midpoint height strays from a straight line
       (by 2 cm: the karts ride the curve, and a tyre's sidewall is less). */
    for(int pass = 0; pass < 3 && track.id != TRACK_FIGURE8; pass++) {
        for(int k = 0; k + 1 < n && n < MAX_SVALS; k++) {
            float a = out[k], b = out[k + 1], m = (a + b) * 0.5f;
            if(b - a < 0.8f) continue;
            if(fabsf(track_height(m) - 0.5f * (track_height(a) + track_height(b))) < 0.02f) continue;
            for(int q = n; q > k + 1; q--) out[q] = out[q - 1];
            out[k + 1] = m;
            n++;
            k++;
        }
    }
    return n;
}

/* Road-surface point at lap distance s, lateral d, raised by dy. */
V3 f8_pt(float s, float d, float dy) {
    V3 p = track_point(s, d);
    p.y = track_road_y(s, d) + dy;
    return p;
}

float f8_wall(float s) {
    int i = (int)(track_wrap(s) / track.ds) % TRACK_SAMPLES;
    int j = (i + 1) % TRACK_SAMPLES;
    float f = clampf((track_wrap(s) - track.smp[i].s) / track.ds, 0, 1);
    return lerpf(track.smp[i].wall_out, track.smp[j].wall_out, f);
}

/* One ribbon along the chunk's lap distances between lateral offsets d0
   and d1 (scaled by the barrier distance when flagged), broken over the
   jump gap. UVs either follow the track or the world plane. */
void f8_ribbon(const float *sv, int n, const pvr_poly_hdr_t *h, float d0, float d1, float dy0, float dy1,
               int flags, float uscale, float vscale, bool world_uv, uint32_t c0, uint32_t c1) {
    bool open = false;
    for(int k = 0; k < n; k++) {
        float s = sv[k];
        if(track_in_gap(s)) {
            if(open) strip_end();
            open = false;
            continue;
        }
        if(!open) {
            strip_begin(h);
            open = true;
        }
        float w = f8_wall(s);
        V3 pa = f8_pt(s, (flags & RB_WALL0) ? d0 * w : d0, dy0);
        V3 pb = f8_pt(s, (flags & RB_WALL1) ? d1 * w : d1, dy1);
        if(world_uv) {
            svp(pa, pa.x / uscale, pa.z / uscale, c0);
            svp(pb, pb.x / uscale, pb.z / uscale, c1);
        }
        else {
            svp(pa, 0, s / vscale, c0);
            svp(pb, uscale, s / vscale, c1);
        }
    }
    if(open) strip_end();
}

/* How many pieces the sky decks' road quad from sa to sb is drawn in. The
   karts ride track_road_y, and the ribbon's flat triangles stray from it
   by a quarter of the quad's twist (its bank change times the road's
   width) where a bank eases in or out: 10 to 15 cm, enough to bury or
   float the tyres. Drawn in m pieces that stray is a quarter of the twist
   over m; held to a centimetre, well inside a tyre's sidewall. */
int road_pieces(float sa, float sb) {
    if(!track.in_sky) return 1;
    float half = world_canyon ? 8.0f : SK_EDGE;
    float stray = fabsf(track_bank(sb) - track_bank(sa)) * half * 0.5f;
    int m = (int)ceilf(stray / 0.01f);
    return m < 1 ? 1 : m > 12 ? 12 : m;
}

/* A sky deck's road: f8_ribbon's quads from -half to half, each drawn in
   road_pieces pieces whose vertices lie on the quad's own straight edges,
   so the curbs and faces beside it, drawn from the same samples, still
   meet it exactly; only the inside of each quad follows its twist. */
void f8_road_ribbon(const float *sv, int n, const pvr_poly_hdr_t *h, float half, float vscale) {
    int last = -1;   /* the previous sample, when it is in this strip */
    for(int k = 0; k < n; k++) {
        float s = sv[k];
        if(track_in_gap(s)) {
            if(last >= 0) strip_end();
            last = -1;
            continue;
        }
        if(last < 0) strip_begin(h);
        V3 a1 = f8_pt(s, -half, 0.0f), b1 = f8_pt(s, half, 0.0f);
        if(last >= 0) {
            float s0 = sv[last];
            int m = road_pieces(s0, s);
            V3 a0 = f8_pt(s0, -half, 0.0f), b0 = f8_pt(s0, half, 0.0f);
            for(int j = 1; j < m; j++) {
                float t = (float)j / (float)m, sj = lerpf(s0, s, t);
                svp(vlerp(a0, a1, t), 0, sj / vscale, 0xffffffff);
                svp(vlerp(b0, b1, t), 1, sj / vscale, 0xffffffff);
            }
        }
        svp(a1, 0, s / vscale, 0xffffffff);
        svp(b1, 1, s / vscale, 0xffffffff);
        last = k;
    }
    if(last >= 0) strip_end();
}

bool in_bridge(float s) {
    return track.has_bridge && fabsf(track_delta(track.bridge_s, s)) < track.bridge_half - 0.01f;
}

RoadStyle f8m;

/* The barrier texture's u per metre of lap: backwards on the right wall,
   which faces the other way, so lettering on it reads the right way round. */
static float f8m_barrier_u(int side) {
    return (side > 0 ? -1.0f : 1.0f) / f8m.barrier_u;
}

void f8m_default(void) {
    f8m.road = h_road;
    f8m.curb = h_curb;
    f8m.shoulder = h_sand;
    f8m.barrier = h_barrier;
    f8m.skirt = h_wood;
    f8m.cap = argb(255, 255, 212, 50);
    f8m.stripe_a = argb(255, 40, 40, 44);
    f8m.stripe_b = argb(255, 255, 206, 30);
    f8m.shoulder_uv = 4.0f;
    f8m.wall_h = 1.0f;
    f8m.barrier_u = 4.0f;
    f8m.split_lod = false;
    f8m.extra = NULL;
    f8m.half = 8.0f;
    f8m.edge = 9.6f;
    f8m.verge = NULL;
    f8m.ground = NULL;
    f8m.skirt_ground = false;
}

/* How far the road stands above its skirt's foot at lap distance s: above
   the grass (a ramp or a deck), or with f8m.skirt_ground above the ground
   just outside that side's barrier. */
static float f8_skirt_h(float s, int side) {
    if(!f8m.skirt_ground) return track_height(s);
    V3 top = f8_pt(s, side * (f8_wall(s) + 0.25f), 0.0f);
    return top.y - f8m.ground(top.x, top.z);
}

void f8_build_track_chunk(int c) {
    float sv[MAX_SVALS];
    int n = f8_chunk_svals(c, sv);
    chunk_begin(HF_OP, 2000.0f);
    if(cur_chunk && f8m.split_lod) cur_chunk->lod = 1;
    /* Asphalt and curbs. */
    float hw = f8m.half, ew = f8m.edge;
    f8_ribbon(sv, n, f8m.road, -hw, hw, 0.0f, 0.0f, 0, 1.0f, ROAD_V_M, false, argb(255, 250, 250, 250),
              argb(255, 250, 250, 250));
    f8_ribbon(sv, n, f8m.curb, -ew, -hw, 0.01f, 0.01f, 0, 0.0f, 2.4f, false, 0xffffffff, 0xffffffff);
    f8_ribbon(sv, n, f8m.curb, hw, ew, 0.01f, 0.01f, 0, 1.0f, 2.4f, false, 0xffffffff, 0xffffffff);
    /* Shoulders (sand, or grass verges) to the barriers. */
    f8_ribbon(sv, n, f8m.shoulder, -ew, -1.0f, 0.0f, 0.0f, RB_WALL1, f8m.shoulder_uv, 0, true, argb(255, 238, 238, 238),
              argb(255, 214, 214, 214));
    f8_ribbon(sv, n, f8m.shoulder, 1.0f, ew, 0.0f, 0.0f, RB_WALL0, f8m.shoulder_uv, 0, true, argb(255, 214, 214, 214),
              argb(255, 238, 238, 238));
    /* Barriers with a top cap (stone parapets across a viaduct), then the
       ramp's wooden side skirts or a hill's embankments. */
    for(int side = -1; side <= 1; side += 2) {
        bool open = false;
        int run_bridge = -1;
        for(int k = 0; k < n; k++) {
            float s = sv[k];
            if(track_in_gap(s)) {
                if(open) strip_end();
                open = false;
                continue;
            }
            /* The interval after this sample decides the material; a run
               ends on the shared sample so the wall stays continuous. */
            int br = k + 1 < n ? in_bridge((s + sv[k + 1]) * 0.5f) : run_bridge;
            if(open && br != run_bridge) {
                V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
                uint32_t col = shade(argb(255, 255, 255, 255), vscale(r, (float)-side));
                V3 b = f8_pt(s, side * f8_wall(s), 0.0f);
                if(side < 0) {
                    svp(vadd(b, v3(0, f8m.wall_h, 0)), s * f8m_barrier_u(side), 0.0f, col);
                    svp(b, s * f8m_barrier_u(side), 1.0f, color_scale(col, 0.8f));
                }
                else {
                    svp(b, s * f8m_barrier_u(side), 1.0f, color_scale(col, 0.8f));
                    svp(vadd(b, v3(0, f8m.wall_h, 0)), s * f8m_barrier_u(side), 0.0f, col);
                }
                strip_end();
                open = false;
            }
            if(!open) {
                strip_begin(br > 0 ? h_stone : f8m.barrier);
                run_bridge = br;
                open = true;
            }
            V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
            uint32_t col = shade(argb(255, 255, 255, 255), vscale(r, (float)-side));
            V3 b = f8_pt(s, side * f8_wall(s), 0.0f);
            /* Wind the faces toward the road on both sides. */
            if(side < 0) {
                svp(vadd(b, v3(0, f8m.wall_h, 0)), s * f8m_barrier_u(side), 0.0f, col);
                svp(b, s * f8m_barrier_u(side), 1.0f, color_scale(col, 0.8f));
            }
            else {
                svp(b, s * f8m_barrier_u(side), 1.0f, color_scale(col, 0.8f));
                svp(vadd(b, v3(0, f8m.wall_h, 0)), s * f8m_barrier_u(side), 0.0f, col);
            }
        }
        if(open) strip_end();
        /* Top cap, +-0.25 m about the barrier line (2% of a 12.5 m wall). */
        uint32_t cap = track.has_bridge ? argb(255, 214, 190, 140) : f8m.cap;
        f8_ribbon(sv, n, h_col, side * 0.98f, side * 1.02f, f8m.wall_h, f8m.wall_h, RB_WALL0 | RB_WALL1, 1, 1, true, cap, cap);
    }
    if(track.id == TRACK_TEMPLE) {
        th_banks(sv, n);
        chunk_end();
        return;
    }
    /* Skirts: from the barrier foot on the raised deck down to the grass
       (or the ground, where a road on a slope stands well above it). */
    const float lim = f8m.skirt_ground ? 0.35f : 0.001f;
    for(int side = -1; side <= 1; side += 2) {
        bool open = false;
        for(int k = 0; k < n; k++) {
            float s = sv[k];
            bool raised = f8_skirt_h(s, side) > lim ||
                          (k + 1 < n && f8_skirt_h(sv[k + 1], side) > lim) ||
                          (k > 0 && f8_skirt_h(sv[k - 1], side) > lim);
            if(track_in_gap(s) || !raised) {
                if(open) strip_end();
                open = false;
                continue;
            }
            if(!open) {
                strip_begin(f8m.skirt);
                open = true;
            }
            float h = track_height(s);
            V3 top = f8_pt(s, side * (f8_wall(s) + 0.25f), 0.0f), bot = top;
            bot.y = F8_GRASS_Y;
            if(f8m.skirt_ground) {
                bot.y = f8m.ground(bot.x, bot.z) - 0.05f;
                h = top.y - bot.y;
            }
            V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
            uint32_t col = shade(argb(255, 255, 240, 220), vscale(r, (float)side));
            if(side > 0) {
                svp(top, s / 3.0f, 0, col);
                svp(bot, s / 3.0f, h / 3.0f, color_scale(col, 0.75f));
            }
            else {
                svp(bot, s / 3.0f, h / 3.0f, color_scale(col, 0.75f));
                svp(top, s / 3.0f, 0, col);
            }
        }
        if(open) strip_end();
    }
    if(f8m.extra) f8m.extra(sv, n);
    chunk_end();
}

/* The lip and the landing deck's front: solid faces over the gap with a
   hazard-striped top band. */
void f8_jump_face(float s, float dir) {
    float h = track_height(s - dir * 0.02f);
    float w = f8_wall(s) + 0.25f;
    V3 t = track_tangent(s);
    uint32_t col = shade(argb(255, 255, 240, 220), vscale(t, dir));
    V3 l = f8_pt(s, -w * dir, 0), r = f8_pt(s, w * dir, 0);
    l.y = r.y = h;
    V3 lb = l, rb = r;
    lb.y = rb.y = F8_GRASS_Y;
    float band = 0.7f;
    quad(f8m.skirt, vsub(l, v3(0, band, 0)), vsub(r, v3(0, band, 0)), lb, rb, 0, 0, 2.0f * w / 3.0f, h / 3.0f, col);
    const int stripes = 10;
    for(int i = 0; i < stripes; i++) {
        V3 a = vlerp(l, r, (float)i / stripes), b = vlerp(l, r, (float)(i + 1) / stripes);
        uint32_t sc = (i & 1) ? f8m.stripe_a : f8m.stripe_b;
        quad(h_col, a, b, vsub(a, v3(0, band, 0)), vsub(b, v3(0, band, 0)), 0, 0, 1, 1, sc);
    }
}

/* Verges from each barrier's foot out over the ground (f8m.verge, Tsudoken's
   grass over the paddies, Apple Valley's run-off dirt over the desert), at
   the ground's height (so they stay down where a ramp climbs); built into
   each road chunk through f8m.extra. */
void f8_verges(const float *sv, int n) {
    for(int side = -1; side <= 1; side += 2) {
        strip_begin(f8m.verge);
        for(int k = 0; k < n; k++) {
            float s = sv[k], w = f8_wall(s);
            V3 a = track_point(s, side * (w + 0.3f)), b = track_point(s, side * (w + 7.0f));
            a.y = f8m.ground(a.x, a.z) + 0.03f;
            b.y = f8m.ground(b.x, b.z) + 0.02f;
            uint32_t ca = argb(255, 236, 240, 226), cb = argb(255, 210, 220, 196);
            if(side > 0) {
                svp(a, a.x / 6.0f, a.z / 6.0f, ca);
                svp(b, b.x / 6.0f, b.z / 6.0f, cb);
            }
            else {
                svp(b, b.x / 6.0f, b.z / 6.0f, cb);
                svp(a, a.x / 6.0f, a.z / 6.0f, ca);
            }
        }
        strip_end();
    }
}

/* The road as a split screen draws it beyond SPLIT_NEAR (its pair, built
   straight after the full chunk): every other station, the asphalt, one
   verge ribbon a side from the road's edge over the curb and shoulder
   out onto the ground, and the barrier; no caps or skirts. Five strips
   where the full road has thirteen. On a flat circuit nearly the whole
   lap is in view, so the road was most of a split screen's vertices. */
void f8_build_deck_coarse(int c) {
    float full[MAX_SVALS], sv[MAX_SVALS];
    int n = f8_chunk_svals(c, full), m = 0;
    for(int k = 0; k < n; k++)
        if((k & 1) == 0 || k == n - 1) sv[m++] = full[k];
    chunk_begin(HF_OP, 2000.0f);
    if(cur_chunk) cur_chunk->lod = 2;
    f8_ribbon(sv, m, f8m.road, -f8m.half, f8m.half, 0.0f, 0.0f, 0, 1.0f, ROAD_V_M, false, argb(255, 250, 250, 250),
              argb(255, 250, 250, 250));
    for(int side = -1; side <= 1; side += 2) {
        strip_begin(f8m.verge);
        for(int k = 0; k < m; k++) {
            float s = sv[k], w = f8_wall(s);
            V3 a = f8_pt(s, side * f8m.half, 0.01f), b = track_point(s, side * (w + 7.0f));
            b.y = f8m.ground(b.x, b.z) + 0.02f;
            uint32_t ca = argb(255, 232, 236, 222), cb = argb(255, 210, 220, 196);
            if(side > 0) {
                svp(a, a.x / 6.0f, a.z / 6.0f, ca);
                svp(b, b.x / 6.0f, b.z / 6.0f, cb);
            }
            else {
                svp(b, b.x / 6.0f, b.z / 6.0f, cb);
                svp(a, a.x / 6.0f, a.z / 6.0f, ca);
            }
        }
        strip_end();
        strip_begin(f8m.barrier);
        for(int k = 0; k < m; k++) {
            float s = sv[k];
            V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
            uint32_t col = shade(argb(255, 255, 255, 255), vscale(r, (float)-side));
            V3 b = f8_pt(s, side * f8_wall(s), 0.0f);
            if(side < 0) {
                svp(vadd(b, v3(0, f8m.wall_h, 0)), s * f8m_barrier_u(side), 0.0f, col);
                svp(b, s * f8m_barrier_u(side), 1.0f, color_scale(col, 0.8f));
            }
            else {
                svp(b, s * f8m_barrier_u(side), 1.0f, color_scale(col, 0.8f));
                svp(vadd(b, v3(0, f8m.wall_h, 0)), s * f8m_barrier_u(side), 0.0f, col);
            }
        }
        strip_end();
    }
    chunk_end();
}

/* Horizontal distance from p to the nearest stretch of road. */
float sk_road_dist(V3 p) {
    float best = 1e9f;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        float dx = p.x - track.smp[i].p.x, dz = p.z - track.smp[i].p.z;
        best = fminf(best, dx * dx + dz * dz);
    }
    return sqrtf(best);
}
