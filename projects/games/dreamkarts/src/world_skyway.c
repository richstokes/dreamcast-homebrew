/* Neon Skyway (and Cyberstone, which shares it). Spiral Canyon's road is
   built on the same sky deck (sk_build_track_chunk). */
#include "world_int.h"

/* A neon road through a cyberpunk city at night, high above a sea of
   cloud: a dark deck under a glowing grid with running lights down its
   edges (cyan on the left, magenta on the right), glass rails carrying neon
   tubes round the turns, and no rails at all on the straights. Neon gates
   frame the line, the waves, the plunge and the infield, and dark towers
   rise out of the clouds all round, lit windows, neon crowns and signs. */
#define SK_RAIL_H   0.75f      /* neon tube height */
#define SK_TOWERS   30
#define SK_BEAMS    5
float sk_vscale;
static V3 sk_sparkle[SK_SPARKLES];
uint8_t sk_sparkle_kind[SK_SPARKLES];   /* 0 star, 1 canyon chip, 2 a big pulse, 3 none */
static V3 sk_beam[SK_BEAMS];     /* searchlights on the tallest towers' roofs */
int sk_beam_n;

static const uint32_t neon_cyan = 0xff3cdcff, neon_violet = 0xff9646ff, neon_magenta = 0xffff32c8,
                      neon_pink = 0xffff78e6, neon_blue = 0xff4678ff;

static const pvr_poly_hdr_t *h_skyline, *h_moon, *h_tower, *h_signs, *h_star_add;

/* The headers only the sky road's city draws with. */
void sk_init(void) {
    h_skyline = r_header(TEX_SKYLINE, HF_TR | HF_NOFOG);
    h_moon = r_header(TEX_MOON, HF_TR | HF_NOFOG | HF_CLAMP);
    h_tower = r_header(TEX_TOWER, HF_OP);
    h_signs = r_header(TEX_SIGNS, HF_OP);
    h_star_add = r_header(TEX_STAR, HF_TR | HF_ADD);
}

static const uint32_t canyon_cyan = 0xff8cf0ff,
                      canyon_blue = 0xff5a96ff;

/* Across the road: cyan on the left, through violet, to magenta on the
   right (the karts' underglow picks it up). */
uint32_t world_lane_color(float d) {
    float t = clampf((d + 9.6f) / 19.2f, 0.0f, 1.0f);
    if(world_canyon) return color_lerp(canyon_cyan, canyon_blue, t);
    return t < 0.5f ? color_lerp(neon_cyan, neon_violet, t * 2.0f) : color_lerp(neon_violet, neon_magenta, t * 2.0f - 1.0f);
}

/* Rail tubes: cyan shimmering to blue on the left, magenta to pink on the
   right, slowly along the lap; Spiral Canyon's are strings of orange beads,
   one bead a sample, so the colour alternates sample to sample. */
static uint32_t rail_color(float s, int side) {
    float w = 0.5f + 0.5f * sinf(s * (DK_TAU / 90.0f));
    if(world_canyon) return ((int)(track_wrap(s) / track.ds) & 1) ? 0xffffd63c : 0xffbe8c14;   /* gold rings */
    return side < 0 ? color_lerp(neon_cyan, neon_blue, w * 0.6f) : color_lerp(neon_magenta, neon_pink, w * 0.7f);
}

static bool sk_closed(float s, int side) {
    int i = (int)(track_wrap(s) / track.ds) % TRACK_SAMPLES;
    return !(track.smp[i].open & (side < 0 ? OPEN_LEFT : OPEN_RIGHT));
}

/* One rail run's piece: a ribbon at lateral side * d between heights y0
   and y1 above the road, or flat at height y0 between d and d1 when flat.
   Neon pieces take the rail colour with c0/c1's alpha. */
static void sk_rail_ribbon(const float *sv, int k0, int k1, int side, float d, float d1, float y0, float y1,
                           const pvr_poly_hdr_t *h, uint32_t c0, uint32_t c1, bool neon, bool flat) {
    strip_begin(h);
    for(int k = k0; k <= k1; k++) {
        float s = sv[k];
        uint32_t a = c0, b = c1;
        if(neon) {
            uint32_t n = rail_color(s, side);
            a = color_alpha(n, (float)(c0 >> 24) / 255.0f);
            b = color_alpha(n, (float)(c1 >> 24) / 255.0f);
        }
        V3 p = f8_pt(s, side * d, y0), q = flat ? f8_pt(s, side * d1, y0) : f8_pt(s, side * d, y1);
        /* Wind toward the road on both sides. */
        if(side < 0) {
            svp(q, 0.5f, flat ? 0.0f : 1.0f, b);
            svp(p, 0.5f, 0.0f, a);
        }
        else {
            svp(p, 0.5f, 0.0f, a);
            svp(q, 0.5f, flat ? 0.0f : 1.0f, b);
        }
    }
    strip_end();
}

void sk_build_track_chunk(int c) {
    float sv[MAX_SVALS];
    int n = f8_chunk_svals(c, sv);
    chunk_begin(HF_OP, 2000.0f);
    if(cur_chunk && world_canyon) cur_chunk->lod = 1;   /* dr_build_deck_coarse follows straight after */
    /* The deck: grid and edge lights in one texture, the glowing side
       faces and the dark underside (Spiral Canyon: blue rock faces under
       grass verges, low rock walls under the strings of orange beads). */
    bool canyon = world_canyon;
    uint32_t face_lo = canyon ? argb(255, 118, 122, 146) : argb(255, 10, 30, 70);
    uint32_t face_l = canyon ? argb(255, 196, 200, 214) : argb(255, 60, 200, 255);
    uint32_t face_r = canyon ? argb(255, 196, 200, 214) : argb(255, 255, 50, 200);
    uint32_t face_ro = canyon ? argb(255, 118, 122, 146) : argb(255, 70, 10, 70);
    uint32_t ledge = canyon ? argb(255, 150, 214, 110) : argb(255, 16, 16, 40);
    uint32_t panel_lo = canyon ? argb(255, 156, 160, 180) : argb(255, 14, 14, 40);
    uint32_t panel_hi = canyon ? argb(255, 214, 218, 230) : argb(255, 40, 50, 110);
    uint32_t under = canyon ? argb(255, 104, 108, 130) : argb(255, 14, 12, 34);
    if(canyon) {
        /* Cobbles to 8 m, then the day tracks' red and white curbs out to
           the edge. */
        f8_road_ribbon(sv, n, &sk_road_hdr, 8.0f, sk_vscale);
        for(int side = -1; side <= 1; side += 2) {
            strip_begin(h_curb);
            for(int k = 0; k < n; k++) {
                V3 a = f8_pt(sv[k], side * 8.0f, 0.0f), b = f8_pt(sv[k], side * SK_EDGE, 0.0f);
                if(side < 0) {
                    svp(b, 0, sv[k] / 2.4f, 0xffffffff);
                    svp(a, 1, sv[k] / 2.4f, 0xffffffff);
                }
                else {
                    svp(a, 1, sv[k] / 2.4f, 0xffffffff);
                    svp(b, 0, sv[k] / 2.4f, 0xffffffff);
                }
            }
            strip_end();
        }
    }
    else
        f8_road_ribbon(sv, n, &sk_road_hdr, SK_EDGE, sk_vscale);
    f8_ribbon(sv, n, h_col, -SK_EDGE, -SK_EDGE, -SK_DEPTH, 0.0f, 0, 1, 1, true, face_lo, face_l);
    f8_ribbon(sv, n, h_col, SK_EDGE, SK_EDGE, 0.0f, -SK_DEPTH, 0, 1, 1, true, face_r, face_ro);
    /* Glass rails on the closed sides: a dark panel, a ledge out to it from
       the road edge and a neon tube along the top. */
    int runs[MAX_SVALS][3], nruns = 0;
    for(int side = -1; side <= 1; side += 2) {
        int k = 0;
        while(k < n) {
            if(!sk_closed(sv[k], side) || track_in_gap(sv[k])) {
                k++;
                continue;
            }
            int k0 = k;
            while(k + 1 < n && sk_closed(sv[k + 1], side) && !track_in_gap(sv[k + 1])) k++;
            if(k > k0 && canyon) {
                /* A grass ledge and the padded barrier the day tracks use. */
                sk_rail_ribbon(sv, k0, k, side, SK_EDGE, SK_RAIL, 0.0f, 0.0f, h_col, ledge, ledge, false, true);
                strip_begin(h_barrier);
                for(int q = k0; q <= k; q++) {
                    V3 p = f8_pt(sv[q], side * SK_RAIL, 0.95f), b = f8_pt(sv[q], side * SK_RAIL, -0.1f);
                    uint32_t c = 0xffffffff;
                    if(side < 0) {
                        svp(b, sv[q] / 4.0f, 1.0f, c);
                        svp(p, sv[q] / 4.0f, 0.0f, c);
                    }
                    else {
                        svp(p, sv[q] / 4.0f, 0.0f, c);
                        svp(b, sv[q] / 4.0f, 1.0f, c);
                    }
                }
                strip_end();
            }
            else if(k > k0) {
                sk_rail_ribbon(sv, k0, k, side, SK_EDGE, SK_RAIL, 0.0f, 0.0f, h_col, ledge, ledge, false, true);
                sk_rail_ribbon(sv, k0, k, side, SK_RAIL, 0, -SK_DEPTH, SK_RAIL_H, h_col, panel_lo, panel_hi, false,
                               false);
                sk_rail_ribbon(sv, k0, k, side, SK_RAIL - 0.12f, SK_RAIL + 0.12f, SK_RAIL_H, 0.0f, h_col, 0xffffffff,
                               0xffffffff, true, true);
                sk_rail_ribbon(sv, k0, k, side, SK_RAIL - 0.12f, 0, SK_RAIL_H - 0.16f, SK_RAIL_H, h_col, 0xffffffff,
                               0xffffffff, true, false);
                runs[nruns][0] = k0;
                runs[nruns][1] = k;
                runs[nruns][2] = side;
                nruns++;
            }
            k++;
        }
    }
    if(canyon) dr_skirts(sv, n);
    chunk_end();
    if(canyon) dr_build_deck_coarse(c);   /* its pair: straight after it */
    /* The deck's dark underside, only seen from below (left out of a split
       screen, like the glows). */
    chunk_begin(HF_OP, 2000.0f);
    if(cur_chunk) cur_chunk->far = 1;
    /* The canyon's ledges are rock underneath, seen from the road below. */
    if(canyon) {
        uint32_t rock_under = argb(255, 190, 194, 208);
#ifdef DK_ROCK_TINT
        rock_under = argb(255, 150, 70, 230);   /* a tint of its own: the leak test reads it as rock */
        rock_pass++;
#endif
        /* Out to the rails, not the deck edge: the rock the lid starts
           from is at the rail, and the slot between would show sky from
           the road below. */
        f8_ribbon(sv, n, h_islandrock, SK_RAIL + 0.45f, -(SK_RAIL + 0.45f), -SK_DEPTH, -SK_DEPTH, 0, 6.0f, 1, true, rock_under, rock_under);
#ifdef DK_ROCK_TINT
        rock_pass--;
#endif
    }
    else
        f8_ribbon(sv, n, h_col, SK_EDGE, -SK_EDGE, -SK_DEPTH, -SK_DEPTH, 0, 1, 1, true, under, under);
    chunk_end();
    /* The tubes' glow (additive). */
    if(nruns && !canyon) {
        chunk_begin(HF_TR, 700.0f);
        if(cur_chunk) cur_chunk->far = 1;
        uint32_t ga = canyon ? argb(70, 255, 255, 255) : argb(150, 255, 255, 255);
        for(int r = 0; r < nruns; r++)
            sk_rail_ribbon(sv, runs[r][0], runs[r][1], runs[r][2], SK_RAIL, 0, SK_RAIL_H - 0.55f, SK_RAIL_H + 0.65f,
                           h_dash_glow, ga, ga, true, false);
        chunk_end();
    }
}

/* An octagonal neon gate the road passes through: a magenta frame round
   a cyan one, with a soft additive halo. */
static void sk_gate(float s) {
    V3 c = track_point(s, 0.0f), r = vsub(track_point(s, 1.0f), c);
    c.y = track_road_y(s, 0.0f) + 2.0f;
    const int segs = 8;
    const float r0 = 12.6f;
    /* Corners on the diagonals, so the gate has flat sides and top. */
    const float off = DK_PI / 8.0f;
    static const struct { float ri, ro; uint32_t col; } bands[2] = {
        {12.6f, 13.1f, 0xff3cdcff}, {13.5f, 14.1f, 0xffff32c8}};
    chunk_begin(HF_OP, 1100.0f);
    for(int b = 0; b < 2; b++) {
        strip_begin(h_col);
        for(int i = 0; i <= segs; i++) {
            float a = (float)i / segs * DK_TAU + off;
            V3 dir = vadd(vscale(r, cosf(a)), v3(0, sinf(a), 0));
            svp(vmad(c, dir, bands[b].ro), 0, 0, bands[b].col);
            svp(vmad(c, dir, bands[b].ri), 0, 0, color_scale(bands[b].col, 0.8f));
        }
        strip_end();
    }
    chunk_end();
    chunk_begin(HF_TR, 1100.0f);
    if(cur_chunk) cur_chunk->far = 1;
    strip_begin(h_dash_glow);
    for(int i = 0; i <= segs; i++) {
        float a = (float)i / segs * DK_TAU + off;
        V3 dir = vadd(vscale(r, cosf(a)), v3(0, sinf(a), 0));
        uint32_t col = argb(120, 170, 110, 255);
        svp(vmad(c, dir, 14.1f + 2.6f), 0.5f, 0.0f, col);
        svp(vmad(c, dir, r0 - 2.6f), 0.5f, 1.0f, col);
    }
    strip_end();
    chunk_end();
}

/* The jump's lip and the deck's front edge: glowing faces across the road,
   cyan to magenta. */
static void sk_jump_face(float s, float dir) {
    float h = track_height(s - dir * 0.02f);
    const int stripes = 8;
    for(int i = 0; i < stripes; i++) {
        float d0 = -SK_EDGE + 2.0f * SK_EDGE * (float)i / stripes, d1 = -SK_EDGE + 2.0f * SK_EDGE * (float)(i + 1) / stripes;
        V3 a = track_point(s, d0 * dir), b = track_point(s, d1 * dir);
        a.y = b.y = h;
        V3 ab = a, bb = b;
        ab.y = bb.y = h - SK_DEPTH - 0.4f;
        uint32_t col = (i & 1) ? argb(255, 14, 12, 34) : world_lane_color((d0 + d1) * 0.5f * dir);
        strip_begin(h_col);
        svp(a, 0, 0, col); svp(b, 0, 0, col); svp(ab, 0, 0, color_scale(col, 0.6f)); svp(bb, 0, 0, color_scale(col, 0.6f));
        strip_end();
    }
}

/* A city tower rising out of the clouds: dark glass with lit windows, a
   neon crown round the roof, a neon strip down one corner and a sign on
   the face toward the road. */
static void sk_tower(V3 base, float hx, float hz, float top, float yaw, uint32_t neon, int sign, V3 face_to) {
    chunk_begin(HF_OP, 2000.0f);
    float bottom = CLOUD_Y - 8.0f, hh = (top - bottom) * 0.5f;
    box(h_tower, v3(base.x, bottom + hh, base.z), v3(hx, hh, hz), yaw, 0xffffffff, 1.0f / 24.0f);
    V3 ax = heading_right(yaw), az = heading_fwd(yaw);
    /* Crown: a band of neon just proud of each face, under the roofline. */
    V3 corner[4] = {vadd(vscale(ax, -hx - 0.2f), vscale(az, -hz - 0.2f)), vadd(vscale(ax, hx + 0.2f), vscale(az, -hz - 0.2f)),
                    vadd(vscale(ax, hx + 0.2f), vscale(az, hz + 0.2f)), vadd(vscale(ax, -hx - 0.2f), vscale(az, hz + 0.2f))};
    strip_begin(h_col);
    for(int i = 0; i <= 4; i++) {
        V3 p = vadd(base, corner[i % 4]);
        svp(v3(p.x, top - 0.6f, p.z), 0, 0, neon);
        svp(v3(p.x, top - 1.6f, p.z), 0, 0, color_scale(neon, 0.7f));
    }
    strip_end();
    /* A neon strip down one corner. */
    V3 k = vadd(base, corner[(int)(fabsf(yaw) * 7.0f) & 3]);
    V3 kn = vnorm(v3(k.x - base.x, 0, k.z - base.z));
    V3 kt = v3(kn.z, 0, -kn.x);
    strip_begin(h_col);
    svp(vadd(vmad(k, kt, -0.5f), v3(0, top - 2.0f, 0)), 0, 0, neon);
    svp(vadd(vmad(k, kt, 0.5f), v3(0, top - 2.0f, 0)), 0, 0, neon);
    svp(vadd(vmad(k, kt, -0.5f), v3(0, top - 45.0f, 0)), 0, 0, color_scale(neon, 0.5f));
    svp(vadd(vmad(k, kt, 0.5f), v3(0, top - 45.0f, 0)), 0, 0, color_scale(neon, 0.5f));
    strip_end();
    /* The sign, on whichever face looks most toward the road. */
    if(sign >= 0) {
        V3 to = vnorm(v3(face_to.x - base.x, 0, face_to.z - base.z));
        V3 n = az;
        float hw = hx;
        if(fabsf(vdot(to, ax)) > fabsf(vdot(to, az))) {
            n = vdot(to, ax) > 0 ? ax : vscale(ax, -1);
            hw = hz;
        }
        else if(vdot(to, az) < 0) n = vscale(az, -1);
        float depth = (n.x == ax.x && n.z == ax.z) || (n.x == -ax.x && n.z == -ax.z) ? hx : hz;
        V3 c = vmad(base, n, depth + 0.35f);
        V3 right = v3(-n.z, 0, n.x);   /* viewer's right, looking at the face */
        float w = fminf(hw * 0.85f, 14.0f), h = w * 0.25f, y = top - 6.0f - h;
        float v0 = (float)sign * 0.25f, v1 = v0 + 0.25f;
        strip_begin(h_signs);
        svp(vadd(vmad(c, right, -w), v3(0, y + h, 0)), 0, v0, 0xffffffff);
        svp(vadd(vmad(c, right, w), v3(0, y + h, 0)), 1, v0, 0xffffffff);
        svp(vadd(vmad(c, right, -w), v3(0, y - h, 0)), 0, v1, 0xffffffff);
        svp(vadd(vmad(c, right, w), v3(0, y - h, 0)), 1, v1, 0xffffffff);
        strip_end();
    }
    chunk_end();
}

static void sk_place_city(void) {
    static const uint32_t crowns[4] = {0xff3cdcff, 0xffff32c8, 0xff9646ff, 0xffff78e6};
    V3 placed[SK_TOWERS];
    float size[SK_TOWERS], tops[SK_TOWERS];
    int n = 0;
    /* Cyberstone is wider east to west than Neon Skyway. */
    bool cs = track.id == TRACK_CYBERSTONE;
    float ex = cs ? 270.0f : 250.0f, ez = cs ? 200.0f : 220.0f;
    uint32_t r = cs ? 0x5717u : 0xc17eu;
    for(int tries = 0; tries < 600 && n < SK_TOWERS; tries++) {
        r = r * 1664525u + 1013904223u;
        float x = -ex + (float)((r >> 8) & 1023) / 1023.0f * 2.0f * ex;
        float z = -ez + (float)((r >> 18) & 1023) / 1023.0f * 2.0f * ez;
        float hs = 8.0f + (float)((r >> 4) & 15) * 0.6f;
        V3 p = v3(x, 0, z);
        float road = sk_road_dist(p);
        if(road < hs * 1.45f + 22.0f) continue;
        bool crowded = false;
        for(int i = 0; i < n; i++) {
            float dx = placed[i].x - x, dz = placed[i].z - z;
            if(dx * dx + dz * dz < (hs + size[i] + 14.0f) * (hs + size[i] + 14.0f)) crowded = true;
        }
        if(crowded) continue;
        /* Towers near the road stay low enough to see past; further out the
           skyline climbs. */
        float top = 10.0f + clampf((road - 30.0f) / 90.0f, 0.0f, 1.0f) * 70.0f + (float)((r >> 12) & 31);
        placed[n] = p;
        size[n] = hs;
        tops[n] = top;
        r = r * 1664525u + 1013904223u;
        float yaw = (float)((r >> 8) & 255) / 255.0f * 1.5f;
        TrackPos tp;
        track_locate(p, -1, &tp);
        sk_tower(p, hs, hs * (0.7f + 0.3f * (float)((r >> 16) & 3) / 3.0f), top, yaw, crowns[n & 3],
                 road < 110.0f ? (n % 4) : -1, tp.center);
        n++;
    }
    /* Searchlights from the tallest. */
    sk_beam_n = 0;
    for(int k = 0; k < SK_BEAMS; k++) {
        int best = -1;
        for(int i = 0; i < n; i++) {
            bool used = false;
            for(int j = 0; j < sk_beam_n; j++)
                if(sk_beam[j].x == placed[i].x && sk_beam[j].z == placed[i].z) used = true;
            if(!used && (best < 0 || tops[i] > tops[best])) best = i;
        }
        if(best < 0) break;
        sk_beam[sk_beam_n++] = v3(placed[best].x, tops[best], placed[best].z);
    }
}

void sk_place_props(void) {
    /* Start line. */
    chunk_begin(HF_OP, 600.0f);
    strip_begin(h_checker);
    for(int k = 0; k <= 1; k++) {
        float s = (k ? 1.2f : -1.2f);
        V3 a = f8_pt(s, -8.0f, 0.03f), b = f8_pt(s, 8.0f, 0.03f);
        svp(a, 0, s / 1.2f, 0xffffffff);
        svp(b, 8, s / 1.2f, 0xffffffff);
    }
    strip_end();
    chunk_end();
    if(track.has_jump) {
        chunk_begin(HF_OP, 900.0f);
        sk_jump_face(track_wrap(track.jump.cross + track.jump.lip), 1.0f);
        sk_jump_face(track_wrap(track.jump.cross + track.jump.land), -1.0f);
        chunk_end();
    }

    float gates[WORLD_MAX_GATES];
    int ngates = 0;
    if(track.id == TRACK_CANYON) {
        /* Hoops: the line, over the summit hump (the jump flies through
           it) and the long way home before the chicane. */
        gates[ngates++] = 0.0f;
        gates[ngates++] = track_seg_s(4) + 28.0f;
        gates[ngates++] = track_seg_s(8) + 40.0f;
    }
    else if(track.id == TRACK_CYBERSTONE) {
        /* Gates: the line, out of the first chicane, on the crest before
           the drop, up the National straight and between its rollers. */
        gates[ngates++] = 0.0f;
        gates[ngates++] = track_seg_s(4) + 50.0f;
        gates[ngates++] = track_seg_s(6) + 26.0f;
        gates[ngates++] = track_seg_s(10) + 50.0f;
        gates[ngates++] = track_seg_s(14) + 64.0f;
    }
    else {
        /* Gates: the line, the main straight, the waves' troughs, over the
           crest into the plunge and the bottom of the infield dip. */
        float waves = track_seg_s(2) + 18.0f;
        gates[ngates++] = 0.0f;
        gates[ngates++] = 70.0f;
        gates[ngates++] = waves + 26.0f;
        gates[ngates++] = waves + 52.0f;
        gates[ngates++] = track_seg_s(4) + 47.0f;
        gates[ngates++] = track_seg_s(6) + 45.0f;
    }
    world_gates = 0;
    for(int i = 0; i < ngates; i++) {
        if(world_canyon) dr_hoop(track_wrap(gates[i]));
        else sk_gate(track_wrap(gates[i]));
        world_gate_s[world_gates++] = track_wrap(gates[i]);
    }

    if(world_canyon) {
        dr_place_props();
        return;
    }
    sk_place_city();

    /* Sparkles hanging in the sky around the course, clear of the road. */
    uint32_t r = 0x5a17u;
    for(int i = 0; i < SK_SPARKLES; i++) {
        V3 p;
        for(int tries = 0; tries < 20; tries++) {
            r = r * 1664525u + 1013904223u;
            float s = (float)((r >> 8) & 1023) / 1023.0f * track.length;
            float side = (r & 1) ? 1.0f : -1.0f;
            float d = side * (24.0f + (float)((r >> 18) & 255) / 255.0f * 70.0f);
            p = track_point(s, d);
            p.y = track_height(s) - 12.0f + (float)((r >> 4) & 63) * 0.6f;
            if(sk_road_dist(p) > 22.0f) break;
        }
        sk_sparkle_kind[i] = 0;
        sk_sparkle[i] = p;
    }
}

void build_skyway(void) {
    sk_road_hdr = *h_skyroad[0];   /* the deck's header, before any update */
    sk_vscale = track.length / roundf(track.length / 8.0f);
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        sk_build_track_chunk(c);
    sk_place_props();
}

/* Night: the far city on the horizon, rising through the cloud sea, a
   crescent moon, now and then a shooting star, and searchlights sweeping
   up from the tallest towers. */
void draw_night_distant(void) {
    V3 c = r_cam.pos;
    r_hdr(h_skyline);
    const int segs = 24;
    const float R = 1350.0f;
    RV v[2 * 25];
    int n = 0;
    for(int i = 0; i <= segs; i++) {
        float x = c.x + ring_dir[i].x * R, z = c.z + ring_dir[i].z * R;
        v[n++] = (RV){x, 70.0f, z, (float)i / segs * 6.0f, 0.0f, 0xffffffff};
        v[n++] = (RV){x, CLOUD_Y - 40.0f, z, (float)i / segs * 6.0f, 1.0f, 0xffffffff};
    }
    r_strip(v, n);

    V3 mp = vmad(c, world_sun_dir, 1300.0f);
    if(r_sphere_visible(mp, 220.0f)) {
        r_hdr(h_glow_add);
        r_billboard(mp, 460.0f, 460.0f, 0, 0, 0, 1, 1, argb(60, 255, 230, 170));
        r_hdr(h_moon);
        r_billboard(mp, 150.0f, 150.0f, 0, 0, 0, 1, 1, 0xffffffff);
    }
    /* A shooting star every few seconds, somewhere new each time. */
    const float every = 3.7f;
    int idx = (int)(world_time / every);
    float t = (world_time - (float)idx * every) / 0.7f;
    if(t < 1.0f) {
        uint32_t h = (uint32_t)idx * 2654435761u;
        float az = (float)(h & 1023) / 1023.0f * DK_TAU, el = 0.35f + (float)((h >> 10) & 255) / 255.0f * 0.5f;
        V3 d0 = v3(sinf(az) * cosf(el), sinf(el), cosf(az) * cosf(el));
        V3 dir = vnorm(v3(cosf(az) * ((h & 1024) ? 1.0f : -1.0f), -0.45f, -sinf(az) * ((h & 1024) ? 1.0f : -1.0f)));
        V3 head = vadd(c, vscale(vmad(d0, dir, t * 0.28f), 1250.0f));
        V3 tail = vadd(c, vscale(vmad(d0, dir, fmaxf(t - 0.25f, 0.0f) * 0.28f), 1250.0f));
        float a = sinf(t * DK_PI);
        r_hdr(h_glow_add);
        r_streak(head, tail, 7.0f, argb((int)(255 * a), 255, 255, 255), argb(0, 160, 200, 255));
    }
}

/* Sparkles hanging round the sky road, twinkling. */
void draw_sparkles(void) {
    r_hdr(h_star_add);
    float range = world_split ? 220.0f : 340.0f;
    for(int i = 0; i < SK_SPARKLES; i += world_split ? 2 : 1) {
        V3 p = sk_sparkle[i];
        V3 d = vsub(p, r_cam.pos);
        float tw = 0.55f + 0.45f * sinf(world_time * (2.0f + (float)(i % 5) * 0.6f) + (float)i * 1.9f);
        if(sk_sparkle_kind[i] == 2) {
            /* A spire's tip: a big slow pulse, seen from anywhere. */
            if(!r_sphere_visible(p, 12.0f)) continue;
            float pulse = 0.8f + 0.2f * sinf(world_time * 1.3f + (float)i);
            r_billboard(p, 14.0f * pulse, 14.0f * pulse, world_time * 0.2f, 0, 0, 1, 1, argb(220, 255, 240, 220));
            continue;
        }
        if(sk_sparkle_kind[i] == 1 || sk_sparkle_kind[i] == 3) continue;   /* chips: dr_draw_pt; 3: none */
        if(vdot(d, d) > range * range || !r_sphere_visible(p, 5.0f)) continue;
        float size = 4.0f + (float)(i % 4) * 1.5f;
        r_billboard(p, size * tw, size * tw, world_time * 0.3f + (float)i, 0, 0, 1, 1,
                    color_alpha(world_lane_color((float)(i % 7) * 3.2f - 9.6f), 0.8f * tw));
    }
    /* Searchlights (not in a split screen): long soft beams turning slowly over the city, each
       facing the camera about its own axis. */
    r_hdr(h_glow_add);
    for(int i = 0; i < (world_split ? 0 : sk_beam_n); i++) {
        float a = world_time * (0.25f + 0.04f * (float)i) + (float)i * 2.1f;
        V3 base = sk_beam[i];
        V3 dir = vnorm(v3(sinf(a) * 0.45f, 1.0f, cosf(a) * 0.45f));
        V3 tip = vmad(base, dir, 420.0f);
        V3 side0 = vnorm(vcross(dir, vsub(r_cam.pos, base))), side1 = vnorm(vcross(dir, vsub(r_cam.pos, tip)));
        uint32_t c0 = (i & 1) ? argb(70, 120, 200, 255) : argb(70, 210, 120, 255), c1 = color_alpha(c0, 0.0f);
        RV q[4] = {
            {base.x - side0.x * 2.0f, base.y - side0.y * 2.0f, base.z - side0.z * 2.0f, 0, 0.5f, c0},
            {base.x + side0.x * 2.0f, base.y + side0.y * 2.0f, base.z + side0.z * 2.0f, 1, 0.5f, c0},
            {tip.x - side1.x * 26.0f, tip.y - side1.y * 26.0f, tip.z - side1.z * 26.0f, 0, 0.5f, c1},
            {tip.x + side1.x * 26.0f, tip.y + side1.y * 26.0f, tip.z + side1.z * 26.0f, 1, 0.5f, c1},
        };
        r_strip(q, 4);
    }
}
