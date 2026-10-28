/* Apple Valley: a kart club circuit in the Mojave high desert, after
   Apple Valley Speedway: tyre walls along a narrow road, run-off of packed
   dirt, the desert floor of sand and creosote out to a brown mountain in
   the south and the snow-capped range behind it, Joshua trees. Race
   control, the starter's stand and bleachers by the line, the clubhouse
   and the paddock's canopies, trailers and containers on the outside of
   the main straight. The props are baked by
   tools/build_apple_valley_props.py (Blender) and instanced here. */
#include "apple_valley_props.h"
#include "world_int.h"

#define AV_PEAK_DIR   (200.0f * DK_PI / 180.0f)   /* the brown mountain's bearing, 0 = north */
#define AV_PEAK_DIST  1300.0f
#define AV_PEAK_SCALE 230.0f                      /* its height, metres */
#define AV_PEAK_VERTS 1600                         /* the baked meshes' strip vertices (1579, 426) */
#define AV_PEAK_LO_VERTS 440
static const pvr_poly_hdr_t *av_mat_hdr[AVM_COUNT];
static uint32_t av_peak_col[AV_PEAK_VERTS], av_peak_lo_col[AV_PEAK_LO_VERTS];
static int av_trees, av_scrubs;
static const pvr_poly_hdr_t *h_avroad, *h_avcurb, *h_avdirt, *h_avdesert, *h_avtyres, *h_avmetal, *h_avcrowd,
    *h_avjoshua, *h_avjbark, *h_avscrub, *h_avbanner, *h_avstucco, *h_avrock, *h_avfar, *h_avtumble, *h_avdust,
    *h_avmtn, *h_avrange;

/* The headers only Apple Valley draws with, and its baked props'
   materials. */
void av_init(void) {
    h_avroad = r_header(TEX_AVROAD, HF_OP);
    h_avcurb = r_header(TEX_AVCURB, HF_OP);
    h_avdirt = r_header(TEX_AVDIRT, HF_OP);
    h_avdesert = r_header(TEX_AVDESERT, HF_OP);
    h_avtyres = r_header(TEX_AVTYRES, HF_OP);
    h_avmetal = r_header(TEX_AVMETAL, HF_OP);
    h_avcrowd = r_header(TEX_AVCROWD, HF_OP);
    h_avjoshua = r_header(TEX_AVJOSHUA, HF_PT);
    h_avjbark = r_header(TEX_AVJBARK, HF_OP);
    h_avscrub = r_header(TEX_AVSCRUB, HF_PT);
    h_avbanner = r_header(TEX_AVBANNER, HF_OP | HF_CLAMP);
    h_avstucco = r_header(TEX_AVSTUCCO, HF_OP);
    h_avrock = r_header(TEX_AVROCK, HF_OP);
    h_avfar = r_header(TEX_AVFAR, HF_PT);
    h_avtumble = r_header(TEX_AVTUMBLE, HF_PT);
    h_avdust = r_header(TEX_AVDUST, HF_TR);
    h_avmtn = r_header(TEX_AVMTN, HF_OP | HF_NOFOG);
    h_avrange = r_header(TEX_AVRANGE, HF_TR | HF_NOFOG);
    /* Apple Valley's baked props, in the header's AVM_* order. */
    av_mat_hdr[AVM_FLAT] = h_col;
    av_mat_hdr[AVM_METAL] = h_avmetal;
    av_mat_hdr[AVM_CROWD] = h_avcrowd;
    av_mat_hdr[AVM_JOSHUA] = h_avjoshua;
    av_mat_hdr[AVM_JBARK] = h_avjbark;
    av_mat_hdr[AVM_SCRUB] = h_avscrub;
    av_mat_hdr[AVM_BANNER] = h_avbanner;
    av_mat_hdr[AVM_STUCCO] = h_avstucco;
    av_mat_hdr[AVM_ROCK] = h_avrock;
}

/* The ground: a 10 m grid over the circuit (av_ground_init), bilinear
   between its nodes, and the open desert (av_far) beyond it. */
#define AV_GN 40
#define AV_GSTEP 10.0f
#define AV_G0 (-AV_GSTEP * (AV_GN - 1) * 0.5f)
static float av_gy[AV_GN][AV_GN];
static bool av_gy_ready;
static float av_pa, av_pb, av_pc;   /* the plane through the road's heights: y = a x + b z + c */

/* The open desert: the circuit's slope carried on and eased flat toward
   the horizon, swelling into low rises beyond 170 m. */
static float av_far(float x, float z) {
    float r = sqrtf(x * x + z * z);
    float tilt = lerpf(av_pa * x + av_pb * z + av_pc, av_pc, smooth01(250.0f, 410.0f, r));
    float roll = smooth01(170.0f, 420.0f, r) *
                 (4.0f + 3.0f * sinf(x * 0.011f + 0.7f) + 2.5f * sinf(z * 0.014f + 2.1f) + 1.5f * sinf((x - z) * 0.006f));
    return tilt + roll + F8_GRASS_Y;
}

float av_terrain(float x, float z) {
    float fx = (x - AV_G0) / AV_GSTEP, fz = (z - AV_G0) / AV_GSTEP;
    if(!av_gy_ready || fx < 0 || fz < 0 || fx > AV_GN - 1 || fz > AV_GN - 1) return av_far(x, z);
    int i = (int)fx, j = (int)fz;
    if(i > AV_GN - 2) i = AV_GN - 2;
    if(j > AV_GN - 2) j = AV_GN - 2;
    fx -= (float)i;
    fz -= (float)j;
    return lerpf(lerpf(av_gy[j][i], av_gy[j][i + 1], fx), lerpf(av_gy[j + 1][i], av_gy[j + 1][i + 1], fx), fz);
}

/* The grid follows the road on its slope: near it each node sits just
   under the lowest road within 20 m of the nearest one (so between two
   stretches at different heights it never rises over the lower), easing
   out over 15-60 m into the open desert; the edge nodes are the desert's
   own, so the grid meets the rings round it. */
static void av_ground_init(void) {
    av_gy_ready = false;
    /* Least squares plane through the road samples. */
    float sxx = 0, sxz = 0, szz = 0, sx = 0, sz = 0, sy = 0, sxy = 0, szy = 0, n = TRACK_SAMPLES;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        V3 p = track.smp[i].p;
        float y = track_height(track.smp[i].s);
        sxx += p.x * p.x; sxz += p.x * p.z; szz += p.z * p.z; sx += p.x; sz += p.z; sy += y; sxy += p.x * y; szy += p.z * y;
    }
    /* Centred, the normal equations split: the slope from a 2x2 solve. */
    float mx = sx / n, mz = sz / n, my = sy / n;
    float cxx = sxx / n - mx * mx, cxz = sxz / n - mx * mz, czz = szz / n - mz * mz;
    float cxy = sxy / n - mx * my, czy = szy / n - mz * my, det = cxx * czz - cxz * cxz;
    av_pa = (cxy * czz - czy * cxz) / det;
    av_pb = (czy * cxx - cxy * cxz) / det;
    av_pc = my - av_pa * mx - av_pb * mz;
    float d2[TRACK_SAMPLES];
    for(int j = 0; j < AV_GN; j++) {
        for(int i = 0; i < AV_GN; i++) {
            float x = AV_G0 + i * AV_GSTEP, z = AV_G0 + j * AV_GSTEP;
            if(i == 0 || j == 0 || i == AV_GN - 1 || j == AV_GN - 1) {
                av_gy[j][i] = av_far(x, z);
                continue;
            }
            float best = 1e18f;
            for(int k = 0; k < TRACK_SAMPLES; k++) {
                float dx = x - track.smp[k].p.x, dz = z - track.smp[k].p.z;
                d2[k] = dx * dx + dz * dz;
                best = fminf(best, d2[k]);
            }
            float dmin = sqrtf(best), reach = (dmin + 20.0f) * (dmin + 20.0f), low = 1e9f;
            for(int k = 0; k < TRACK_SAMPLES; k++)
                if(d2[k] < reach) low = fminf(low, track_height(track.smp[k].s));
            av_gy[j][i] = lerpf(low - 0.15f, av_far(x, z), smooth01(15.0f, 60.0f, dmin));
        }
    }
    av_gy_ready = true;
}

/* Clear of the road: margin metres beyond the barriers. */
static bool av_clear(float x, float z, float margin) {
    return sk_road_dist(v3(x, 0, z)) >= 10.0f + margin;
}

/* The ground: the 10 m grid over the circuit, coarser bands out to 415 m,
   then a flat plain to the horizon, all desert; the vertex colours drift
   between a paler and a duskier sand so the tile's repeat breaks up. */

static uint32_t av_ground_color(float x, float z) {
    float t = 0.5f + 0.25f * sinf(x * 0.021f + 0.4f) + 0.25f * sinf(z * 0.017f + 1.9f);
    return color_lerp(argb(255, 236, 226, 214), argb(255, 255, 252, 246), t);
}

/* One row of grid cells from row j0 to j1, columns i0 to i1 every step
   (the last column always included), lit by the slope along x. */
static void av_ground_row(int j0, int j1, int i0, int i1, int step) {
    strip_begin(h_avdesert);
    for(int i = i0;; i += step) {
        if(i > i1) i = i1;
        for(int k = 1; k >= 0; k--) {
            int j = k ? j1 : j0;
            float x = AV_G0 + i * AV_GSTEP, z = AV_G0 + j * AV_GSTEP, y = av_gy[j][i];
            uint32_t col = av_ground_color(x, z);
            if(i + 1 < AV_GN) {
                float gx = (av_gy[j][i + 1] - y) / AV_GSTEP;
                col = color_scale(col, clampf(1.0f + gx * 0.6f, 0.7f, 1.1f));
            }
            svp(v3(x, y, z), x / 24.0f, z / 24.0f, col);
        }
        if(i == i1) break;
    }
    strip_end();
}

/* Each 80 x 40 m piece of the grid, then its coarse copy at 20 m that a
   split screen draws beyond SPLIT_NEAR (a quarter of the vertices; the
   ground was the most a split screen sent at turn 14, where both views
   see nearly the whole circuit). */
static void av_build_ground(void) {
    for(int cz = 0; cz + 1 < AV_GN; cz += 4) {
        for(int cx = 0; cx + 1 < AV_GN; cx += 8) {
            int i1 = cx + 8 < AV_GN - 1 ? cx + 8 : AV_GN - 1, j1 = cz + 4 < AV_GN - 1 ? cz + 4 : AV_GN - 1;
            chunk_begin(HF_OP, 2000.0f);
            if(cur_chunk) cur_chunk->lod = 1;
            for(int j = cz; j < j1; j++) av_ground_row(j, j + 1, cx, i1, 1);
            chunk_end();
            chunk_begin(HF_OP, 2000.0f);
            if(cur_chunk) cur_chunk->lod = 2;
            for(int j = cz; j < j1; j += 2) av_ground_row(j, j + 2 < j1 ? j + 2 : j1, cx, i1, 2);
            chunk_end();
        }
    }
    /* Coarser rings of the rolling desert, then the flat horizon. */
    const float G = -AV_G0, E = 3000.0f;
    static const float rings[] = {0.0f, 40.0f, 90.0f, 150.0f, 220.0f};
    const int nr = (int)(sizeof(rings) / sizeof(rings[0]));
    for(int side = 0; side < 4; side++) {
        chunk_begin(HF_OP, 4000.0f);
        for(int b = 0; b + 1 < nr; b++) {
            strip_begin(h_avdesert);
            for(int k = 0; k <= 16; k++) {
                float t = -1.0f + 2.0f * (float)k / 16.0f;
                for(int q = 1; q >= 0; q--) {
                    float e = G + rings[b + q];
                    float x, z;
                    if(side == 0) { x = t * e; z = e; }
                    else if(side == 1) { x = t * e; z = -e; }
                    else if(side == 2) { x = e; z = t * (e - 0.001f); }
                    else { x = -e; z = t * (e - 0.001f); }
                    svp(v3(x, av_terrain(x, z), z), x / 24.0f, z / 24.0f, av_ground_color(x, z));
                }
            }
            strip_end();
        }
        chunk_end();
    }
    chunk_begin(HF_OP, 6000.0f);
    {
        const float R = G + rings[nr - 1], y = av_terrain(R, R);
        quad(h_avdesert, v3(-E, y, E), v3(E, y, E), v3(-E, y, R), v3(E, y, R), -E / 24, E / 24, E / 24, R / 24, 0xffffffff);
        quad(h_avdesert, v3(-E, y, -R), v3(E, y, -R), v3(-E, y, -E), v3(E, y, -E), -E / 24, -R / 24, E / 24, -E / 24, 0xffffffff);
        quad(h_avdesert, v3(-E, y, R), v3(-R, y, R), v3(-E, y, -R), v3(-R, y, -R), -E / 24, R / 24, -R / 24, -R / 24, 0xffffffff);
        quad(h_avdesert, v3(R, y, R), v3(E, y, R), v3(R, y, -R), v3(E, y, -R), R / 24, R / 24, E / 24, -R / 24, 0xffffffff);
    }
    chunk_end();
}

static void av_prop(int which, V3 p, float yaw, float scale, float lod_far, bool far) {
    prop_place(&av_props[which], av_mat_hdr, AVM_PT_MASK, p, yaw, scale, lod_far, far);
}

/* A prop on the ground beside the road at lap distance s, lateral d,
   turned to face the road (its front, -z, toward the centreline). */
static void av_prop_facing(int which, float s, float d, float lod_far, bool far) {
    V3 p = track_point(s, d), t = track_tangent(s);
    p.y = av_terrain(p.x, p.z);
    float h = atan2f(t.x, t.z);
    av_prop(which, p, d < 0 ? h - DK_PI * 0.5f : h + DK_PI * 0.5f, 1.0f, lod_far, far);
}

/* The start line: a chequered strip of flat quads, two rows of squares
   across the road, one strip. */
static void av_start_line(void) {
    chunk_begin(HF_OP, 600.0f);
    strip_begin(h_col);
    const int across = 10;
    for(int row = 0; row < 2; row++) {
        for(int i = 0; i < across; i++) {
            float d0 = -AV_HALF + 2.0f * AV_HALF * (float)i / across, d1 = d0 + 2.0f * AV_HALF / across;
            float s0 = row ? 0.0f : -1.3f, s1 = s0 + 1.3f;
            uint32_t c = ((row + i) & 1) ? argb(255, 236, 236, 232) : argb(255, 26, 26, 30);
            V3 a = track_point(s0, d0), b = track_point(s0, d1), cc = track_point(s1, d0), dd = track_point(s1, d1);
            a.y = track_road_y(s0, d0) + 0.025f;
            b.y = track_road_y(s0, d1) + 0.025f;
            cc.y = track_road_y(s1, d0) + 0.025f;
            dd.y = track_road_y(s1, d1) + 0.025f;
            svj(a, 0, 0, c);
            svp(b, 0, 0, c);
            svp(cc, 0, 0, c);
            svp(dd, 0, 0, c);
        }
    }
    strip_end();
    chunk_end();
}

/* A Joshua tree or a creosote bush, if the spot is clear of the road. */
static void av_joshua(float x, float z, uint32_t seed) {
    if(!av_clear(x, z, 3.0f)) return;
    float scale = 0.9f + (float)((seed >> 8) & 7) * 0.06f;
    V3 p = v3(x, av_terrain(x, z), z);
    /* A split screen draws everything twice: it keeps one tree in three
       (each ~250 vertices), the far sprites standing in for the rest. */
    av_prop((seed & 1) ? AVP_JOSHUA_B : AVP_JOSHUA_A, p, (float)((seed >> 4) & 255) / 255.0f * DK_TAU, scale, 320.0f,
            av_trees % 3 != 0);
    av_trees++;
}

static void av_scrub(float x, float z, uint32_t seed) {
    if(!av_clear(x, z, 1.0f)) return;
    float scale = 0.7f + (float)((seed >> 8) & 7) * 0.08f;
    V3 p = v3(x, av_terrain(x, z), z);
    /* Small, but a strip each: a split screen leaves them all out. */
    av_prop(AVP_SCRUB, p, (float)((seed >> 4) & 255) / 255.0f * DK_TAU, scale, 140.0f, true);
    av_scrubs++;
}

/* The wind, from the south-west as it usually is out there: the
   tumbleweeds roll with it, the dust devils drift on it and the windsock
   streams along it. */
#define AV_WIND_DIR (45.0f * DK_PI / 180.0f)   /* the bearing it blows toward */
#define AV_TUMBLES  6
#define AV_DEVILS   2
static V3 av_sock_top;

static V3 av_wind(void) {
    return v3(sinf(AV_WIND_DIR), 0, cosf(AV_WIND_DIR));
}

/* Tumbleweeds blowing across the circuit, rolling and hopping, each down
   its own lane at its own pace; one grows in at the upwind end of a 300 m
   run and shrinks away at the far end before coming round again. */
void av_draw_tumbleweeds(void) {
    V3 w = av_wind(), q = v3(w.z, 0, -w.x);
    const float run = 300.0f, range = world_split ? 100.0f : 180.0f;
    r_hdr(h_avtumble);
    for(int i = 0; i < AV_TUMBLES; i += world_split ? 2 : 1) {
        uint32_t h = (uint32_t)i * 2654435761u + 7u;
        float speed = 4.0f + (float)(h & 7) * 0.4f;
        float lane = -120.0f + 48.0f * (float)i + (float)((h >> 4) & 15);
        float t = fmodf(world_time * speed + (float)((h >> 8) & 255), run + 60.0f);
        if(t > run) continue;   /* the gap before it comes round */
        float r = 0.5f * smooth01(0.0f, 15.0f, t) * smooth01(run, run - 15.0f, t);
        if(r < 0.05f) continue;
        V3 p = vadd(vscale(w, t - run * 0.5f), vscale(q, lane));
        V3 d = vsub(p, r_cam.pos);
        if(vdot(d, d) > range * range) continue;
        /* A hop every 7.5 m or so, high enough to clear the tyres mostly. */
        float hop = fabsf(sinf(t * 0.42f + (float)i)) * 1.3f;
        p.y = av_terrain(p.x, p.z) + 0.15f + r + hop;
        if(!r_sphere_visible(p, r)) continue;
        r_billboard(p, r, r, -t / 0.5f, 0, 0, 1, 1, 0xffffffff);
    }
}

/* Dust devils out in the desert: a column of dust puffs that spins up,
   drifts downwind and dies away, then forms again somewhere else. */
void av_draw_dust_devils(void) {
    V3 w = av_wind();
    r_hdr(h_avdust);
    for(int i = 0; i < AV_DEVILS; i += world_split ? 2 : 1) {
        float life = 38.0f + 9.0f * (float)i, age = world_time + 17.0f * (float)i;
        float ph = fmodf(age, life) / life;
        if(ph > 0.7f) continue;
        float k = sinf(ph / 0.7f * DK_PI);
        float a = (i ? 2.4f : 4.3f) + 0.9f * floorf(age / life);   /* a new spot each time */
        V3 base = vadd(v3(sinf(a) * 380.0f, 0, cosf(a) * 380.0f), vscale(w, ph * 70.0f));
        base.y = av_terrain(base.x, base.z);
        if(!r_sphere_visible(vadd(base, v3(0, 20.0f, 0)), 30.0f)) continue;
        for(int j = 0; j < 7; j++) {
            float f = (float)j / 6.0f, sway = 1.5f * f;
            V3 c = vadd(base, v3(sinf(world_time * 1.7f + (float)j) * sway, 2.0f + f * 44.0f * k,
                                 cosf(world_time * 1.3f + (float)j) * sway));
            float wd = (2.5f + f * 10.0f) * (0.5f + 0.5f * k);
            r_billboard(c, wd, wd * 0.9f, world_time * 2.5f + (float)j, 0, 0, 1, 1,
                        color_alpha(argb(255, 184, 152, 114), 0.6f * k * (1.0f - 0.7f * f)));
        }
    }
}

/* The windsock by race control: four striped segments streaming downwind
   from the top of its pole, sagging in the lulls and flicking in the
   gusts. */
void av_draw_windsock(void) {
    V3 top = av_sock_top;
    V3 d0 = vsub(top, r_cam.pos);
    if(vdot(d0, d0) > 250.0f * 250.0f || !r_sphere_visible(top, 3.0f)) return;
    float gust = 0.65f + 0.25f * sinf(world_time * 0.37f) + 0.1f * sinf(world_time * 1.9f);
    float droop = (1.0f - gust) * 1.1f;
    float yaw = AV_WIND_DIR + 0.12f * sinf(world_time * 2.3f);
    V3 dir = v3(sinf(yaw) * cosf(droop), -sinf(droop), cosf(yaw) * cosf(droop));
    V3 side = vnorm(vcross(dir, v3(0, 1, 0))), up = vcross(side, dir);
    enum { SIDES = 6, SEGS = 4 };
    V3 ring[SEGS + 1][SIDES + 1];
    for(int k = 0; k <= SEGS; k++) {
        float r = lerpf(0.34f, 0.18f, (float)k / SEGS);
        V3 c = vadd(vadd(top, vscale(dir, 0.6f * (float)k)),
                    vscale(up, 0.05f * (float)k * sinf(world_time * 9.0f - (float)k)));
        for(int j = 0; j <= SIDES; j++) {
            float a = (float)j / SIDES * DK_TAU;
            ring[k][j] = vadd(c, vadd(vscale(side, cosf(a) * r), vscale(up, sinf(a) * r)));
        }
    }
    r_hdr(h_col);
    for(int k = 0; k < SEGS; k++) {
        uint32_t col = (k & 1) ? argb(255, 240, 240, 236) : argb(255, 255, 104, 28);
        RV v[2 * (SIDES + 1)];
        for(int j = 0; j <= SIDES; j++) {
            /* Lit from above: the underside a shade darker. */
            uint32_t c = color_scale(col, 0.78f + 0.22f * sinf((float)j / SIDES * DK_TAU));
            v[2 * j] = (RV){ring[k][j].x, ring[k][j].y, ring[k][j].z, 0, 0, c};
            v[2 * j + 1] = (RV){ring[k + 1][j].x, ring[k + 1][j].y, ring[k + 1][j].z, 0, 0, c};
        }
        r_strip(v, 2 * (SIDES + 1));
    }
}

static void av_place_props(void) {
    av_trees = av_scrubs = 0;
    world_gates = 0;
    av_start_line();

    /* Race control and the starter's stand on the outside of the line,
       bleachers before them; light poles down the straight. */
    av_prop_facing(AVP_TOWER, 8.0f, -15.5f, 1600.0f, false);
    av_prop_facing(AVP_FLAGSTAND, -1.5f, -12.2f, 900.0f, true);
    for(int i = 0; i < 3; i++) av_prop_facing(AVP_STAND, -64.0f + 15.0f * (float)i, -15.0f, 1200.0f, false);
    for(int i = 0; i < 4; i++) av_prop_facing(AVP_POLE, -90.0f + 55.0f * (float)i, -12.0f, 1200.0f, false);

    /* The clubhouse past race control, a pair of containers, and the
       paddock: two rows of canopies with the trailers behind. */
    av_prop_facing(AVP_CLUBHOUSE, 36.0f, -30.0f, 1400.0f, false);
    av_prop_facing(AVP_CONTAINER_R, 60.0f, -24.0f, 900.0f, false);
    av_prop_facing(AVP_CONTAINER_T, 60.0f, -31.0f, 900.0f, true);
    for(int i = 0; i < 4; i++) {
        static const int col[4] = {AVP_CANOPY_R, AVP_CANOPY_B, AVP_CANOPY_Y, AVP_CANOPY_B};
        av_prop_facing(col[i], 72.0f + 9.0f * (float)i, -16.5f, 700.0f, false);
        av_prop_facing(col[3 - i], 72.0f + 9.0f * (float)i, -27.0f, 700.0f, true);
        if(i < 3) av_prop_facing(AVP_TRAILER, 76.0f + 10.0f * (float)i, -35.0f, 700.0f, true);
    }
    for(int i = 0; i < 4; i++) av_prop_facing(AVP_FENCE, 2.0f + 13.0f * (float)i, -12.6f, 600.0f, true);
    /* The windsock's pole by race control (av_draw_windsock flies it). */
    {
        V3 p = track_point(24.0f, -12.4f);
        p.y = av_terrain(p.x, p.z);
        chunk_begin(HF_OP, 600.0f);
        cylinder(h_col, p, 0.07f, 0.05f, 6.2f, 5, argb(255, 176, 180, 186), argb(255, 196, 200, 206), 1, 1);
        chunk_end();
        av_sock_top = vadd(p, v3(0, 6.1f, 0));
    }

#ifdef DK_CHUNK_STATS
    ck_first[CK_PROPS] = chunk_n;
    ck_first[CK_TORCH] = ck_first[CK_N] = 100000;   /* unused kinds */
#endif
    /* Tyre stacks on the outside of the hairpins (2, 4, 9, 13 and 14). */
    static const int hairpins[5] = {3, 7, 17, 23, 25};
    for(int k = 0; k < 5; k++) {
        int i = hairpins[k];
        float s0 = track_seg_s(i), len = track_seg_radius(i) * fabsf(track_seg_deg(i)) * (DK_PI / 180.0f);
        float side = track_seg_deg(i) > 0 ? -1.0f : 1.0f;   /* the outside of a right turn is the left */
        for(int q = 0; q < 2; q++) {
            V3 p = track_point(s0 + len * (0.3f + 0.4f * q), side * 11.4f);
            if(!av_clear(p.x, p.z, 0.6f)) continue;
            p.y = av_terrain(p.x, p.z);
            av_prop(AVP_TYRES, p, (float)q, 1.0f, 300.0f, false);
        }
    }

    /* Boulder piles, Joshua trees and creosote scattered over the desert,
       in the infield's gaps and out to the plain. */
    uint32_t seed = 0x51a7u;
    int rocks = 0;
    for(int tries = 0; tries < 200 && rocks < 4; tries++) {
        seed = seed * 1664525u + 1013904223u;
        float x = -150.0f + (float)((seed >> 8) & 1023) / 1023.0f * 300.0f;
        float z = -90.0f + (float)((seed >> 18) & 1023) / 1023.0f * 180.0f;
        if(!av_clear(x, z, 6.0f)) continue;
        av_prop(AVP_ROCKS, v3(x, av_terrain(x, z), z), (float)(seed & 255) / 40.0f, 1.0f + (float)((seed >> 3) & 3) * 0.3f,
                500.0f, false);
        rocks++;
    }
    /* A dozen near the road, a few metres past the tyres, so they pass
       close by; the rest anywhere. */
    for(int i = 0; i < 24 && av_trees < 12; i++) {
        seed = seed * 1664525u + 1013904223u;
        float s = track.length * ((float)i + 0.3f) / 24.0f, d = (seed & 1 ? 1.0f : -1.0f) * (15.0f + (float)((seed >> 9) & 7));
        V3 p = track_point(s, d);
        av_joshua(p.x, p.z, seed);
    }
    for(int tries = 0; tries < 600 && av_trees < 30; tries++) {
        seed = seed * 1664525u + 1013904223u;
        float x = -260.0f + (float)((seed >> 8) & 1023) / 1023.0f * 520.0f;
        float z = -200.0f + (float)((seed >> 18) & 1023) / 1023.0f * 400.0f;
        av_joshua(x, z, seed);
    }
    for(int i = 0; i < 90 && av_scrubs < 60; i++) {
        seed = seed * 1664525u + 1013904223u;
        float s = track.length * (float)i / 90.0f, d = (seed & 1 ? 1.0f : -1.0f) * (12.0f + (float)((seed >> 9) & 15));
        V3 p = track_point(s, d);
        av_scrub(p.x, p.z, seed);
    }
    for(int tries = 0; tries < 900 && av_scrubs < 150; tries++) {
        seed = seed * 1664525u + 1013904223u;
        float x = -200.0f + (float)((seed >> 8) & 1023) / 1023.0f * 400.0f;
        float z = -150.0f + (float)((seed >> 18) & 1023) / 1023.0f * 300.0f;
        av_scrub(x, z, seed);
    }
    /* Far Joshua trees as sprites out on the plain. */
    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t h = (uint32_t)i * 2654435761u + 131u;
        float a = (float)(h & 1023) / 1023.0f * DK_TAU, d = 240.0f + (float)((h >> 10) & 255);
        float x = sinf(a) * d, z = cosf(a) * d;
        far_sprites[far_sprite_n] = v3(x, av_terrain(x, z), z);
        far_sprite_h[far_sprite_n] = 5.0f + (float)((h >> 18) & 7) * 0.5f;
        far_sprite_n++;
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DreamKarts apple valley props: %d joshua trees, %d creosote, %d rock piles\n", av_trees, av_scrubs, rocks);
#endif
}

/* The brown mountain: the baked unit mountain lit and hazed once, then
   drawn each frame at a fixed bearing from the camera beyond the fog, as
   Tsudoken's Fuji is (in the opaque list, so its back faces never paint
   over the front). */
static void av_peak_bake(const dk_prop_t *m, uint32_t *out, int out_max) {
    int at = 0;
    for(int s = 0; s < m->nstrips; s++) {
        int n = m->strip_len[s];
        if(at + n > out_max) break;
        for(int q = 0; q < n; q++) {
            const dk_prop_vert_t *v = &m->verts[m->index[at + q]];
            V3 nn = vnorm(v3(v->nx, v->ny, v->nz));
            float l = 0.78f + 0.34f * fmaxf(0.0f, vdot(nn, world_sun_dir));
            /* The desert air is clear: a light haze, thickest at the foot. */
            float hz = clampf(v->y / 1000.0f, 0.0f, 1.0f);
            out[at + q] = color_lerp(color_scale(argb(255, v->r, v->g, v->b), l), argb(255, 206, 220, 232),
                                     lerpf(0.42f, 0.18f, hz));
        }
        at += n;
    }
}

void av_draw_peak(void) {
    V3 c = r_cam.pos;
    V3 base = v3(c.x + sinf(AV_PEAK_DIR) * AV_PEAK_DIST, -8.0f, c.z + cosf(AV_PEAK_DIR) * AV_PEAK_DIST);
    if(!r_sphere_visible(vadd(base, v3(0, AV_PEAK_SCALE * 0.5f, 0)), AV_PEAK_SCALE * 1.6f)) return;
    r_hdr(h_avmtn);   /* opaque, no fog, the rock texture over the colours */
    const dk_prop_t *m = &av_props[world_split ? AVP_PEAK_LO : AVP_PEAK];
    const uint32_t *col = world_split ? av_peak_lo_col : av_peak_col;
    int col_max = world_split ? AV_PEAK_LO_VERTS : AV_PEAK_VERTS;
    float k = 0.001f * AV_PEAK_SCALE;
    RV v[256];
    int at = 0;
    for(int s = 0; s < m->nstrips; s++) {
        int n = m->strip_len[s];
        if(at + n > col_max) break;
        for(int q = 0; q < n; q++) {
            const dk_prop_vert_t *p = &m->verts[m->index[at + q]];
            /* Twice as wide as it is deep: a long massif, side on. */
            v[q] = (RV){base.x + p->x * k * 1.5f, base.y + p->y * k, base.z + p->z * k, p->u / 256.0f, p->v / 256.0f,
                        col[at + q]};
        }
        r_strip(v, n);
        at += n;
    }
}

/* The snowy range's ridge at bearing a: its height, the snow line, and
   how far into the south it is (1 due south, 0 beyond ~70 degrees off),
   where the peaks rise and carry snow. Three octaves of sharp crests
   (1 - |sin|, cusped at the top) make jagged peaks rather than bumps,
   and the higher a peak the further down its snow comes. */
static float av_range_h(float a, float *snow, float *south) {
    *south = clampf(1.0f - fabsf(wrap_angle(a - DK_PI)) / 1.25f, 0.0f, 1.0f);
    float c1 = 1.0f - fabsf(sinf(a * 5.0f + 0.5f)), c2 = 1.0f - fabsf(sinf(a * 13.0f + 1.7f)),
          c3 = 1.0f - fabsf(sinf(a * 29.0f + 0.3f));
    float pk = 0.6f * c1 * sqrtf(c1) + 0.3f * c2 * sqrtf(c2) + 0.1f * c3 * c3;
    float h = 14.0f + *south * (55.0f + 85.0f * pk);
    *snow = fmaxf(h - *south * (10.0f + 40.0f * pk), 4.0f);
    return h;
}

/* The horizon: the snow-capped San Bernardinos along the south, ridges
   of bare brown desert hills all round in front of them. */
void av_draw_ranges(void) {
    V3 c = r_cam.pos;
    enum { SEGS = 192, PIECE = 16 };
    RV v[2 * (PIECE + 1)];
    const uint32_t haze = argb(255, 206, 220, 232);
    for(int layer = 0; layer < 2; layer++) {
        float R = 1420.0f - 80.0f * layer;
        /* The snowy range carries the couloir texture, 14 times round. */
        r_hdr(layer == 0 ? h_avrange : h_hills);
        for(int p0 = 0; p0 < SEGS; p0 += PIECE) {
            float am = (p0 + PIECE * 0.5f) / SEGS * DK_TAU;
            V3 mid = v3(c.x + sinf(am) * R, 60.0f, c.z + cosf(am) * R);
            if(!r_sphere_visible(mid, R * sinf(DK_PI * PIECE / SEGS) + 120.0f)) continue;
            if(layer == 0) {
                /* The snowy range: one strip from the ridge to the snow
                   line, one from there down into the haze. */
                for(int band = 0; band < 2; band++) {
                    int n = 0;
                    for(int i = p0; i <= p0 + PIECE; i++) {
                        float a = (float)i / SEGS * DK_TAU, snow, south;
                        float h = av_range_h(a, &snow, &south);
                        float x = c.x + sinf(a) * R, z = c.z + cosf(a) * R, u = (float)i / SEGS * 14.0f;
                        float vs = (h - snow) / fmaxf(h + 6.0f, 1.0f);   /* v: 0 at the ridge, 1 at the foot */
                        uint32_t rock = color_lerp(argb(255, 160, 168, 186), argb(255, 104, 112, 146), south);
                        if(band == 0) {
                            v[n++] = (RV){x, h, z, u, 0, color_lerp(argb(255, 168, 172, 186), argb(255, 246, 248, 252), south)};
                            v[n++] = (RV){x, snow, z, u, vs, rock};
                        }
                        else {
                            v[n++] = (RV){x, snow, z, u, vs, rock};
                            v[n++] = (RV){x, -6.0f, z, u, 1.0f, haze};
                        }
                    }
                    r_strip(v, n);
                }
            }
            else {
                int n = 0;
                for(int i = p0; i <= p0 + PIECE; i++) {
                    float a = (float)i / SEGS * DK_TAU;
                    float h = 9.0f + 5.0f * sinf(a * 4.0f + 1.1f) + 4.0f * fabsf(sinf(a * 11.0f + 0.3f)) + 2.5f * sinf(a * 19.0f);
                    float x = c.x + sinf(a) * R, z = c.z + cosf(a) * R;
                    v[n++] = (RV){x, h, z, 0, 0, argb(255, 178, 150, 124)};
                    v[n++] = (RV){x, -6.0f, z, 0, 0, haze};
                }
                r_strip(v, n);
            }
        }
    }
}

void build_apple(void) {
    f8m_default();
    f8m.road = h_avroad;
    f8m.curb = h_avcurb;
    f8m.shoulder = h_avdirt;
    f8m.barrier = h_avtyres;
    f8m.skirt = h_avrock;
    f8m.cap = argb(255, 34, 34, 38);   /* the top tyre */
    f8m.wall_h = 1.1f;
    f8m.barrier_u = 8.0f;
    f8m.shoulder_uv = 6.0f;
    f8m.half = AV_HALF;
    f8m.edge = AV_HALF + 1.3f;
    f8m.extra = f8_verges;
    f8m.verge = h_avdirt;
    f8m.ground = av_terrain;
    f8m.skirt_ground = true;
    h_far_sprite = h_avfar;
    av_ground_init();
#ifdef DK_CHUNK_STATS
    ck_first[CK_GROUND] = chunk_n;   /* the kinds here: ground, road ("deck"), buildings ("rock"), trees+scrub ("props") */
#endif
    av_build_ground();
#ifdef DK_CHUNK_STATS
    ck_first[CK_DECK] = chunk_n;
#endif
    /* Flat all the way round: every chunk has a coarse copy for a split
       screen. */
    for(int c = 0; c < TRACK_SAMPLES / 8; c++) {
        f8m.split_lod = true;
        f8_build_track_chunk(c);
        f8_build_deck_coarse(c);
    }
    f8m.split_lod = false;
#ifdef DK_CHUNK_STATS
    ck_first[CK_ROCK] = chunk_n;
#endif
    av_place_props();
    av_peak_bake(&av_props[AVP_PEAK], av_peak_col, AV_PEAK_VERTS);
    av_peak_bake(&av_props[AVP_PEAK_LO], av_peak_lo_col, AV_PEAK_LO_VERTS);
}
