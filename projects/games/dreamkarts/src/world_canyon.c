/* Spiral Canyon: its road is a sky deck (world_skyway.c) laid over a
   valley, through tunnels in its rock. */
#include "world_int.h"
#include "canyon_rock.h"

/* A spring valley with a hill-zone flavour: rolling meadows with a lake
   in the infield, palms and bushes, spinning sunflowers and totem poles, and
   two slate towers the corkscrews climb as ledges cut round them (with
   waterfalls), a rock ridge carrying the summit road between them. The
   road is a warm cobbled path with grass verges, railed with strings of
   gold rings and flying through checkered loops; gold rings hang up the
   corkscrews. */
static const pvr_poly_hdr_t *h_meadow, *h_ehchecker, *h_sunflower, *h_totem, *h_bush, *h_waterfall;

/* The headers only Spiral Canyon draws with. */
void dr_init(void) {
    h_meadow = r_header(TEX_MEADOW, HF_OP);
    h_ehchecker = r_header(TEX_EHCHECKER, HF_OP);
    h_sunflower = r_header(TEX_SUNFLOWER, HF_PT);
    h_totem = r_header(TEX_TOTEM, HF_OP);
    h_bush = r_header(TEX_BUSH, HF_OP);
    h_waterfall = r_header(TEX_WATERFALL, HF_TR);
}

#define DR_TREES     84
#define DR_ROCKS     14     /* totem poles */
#define DR_PINWHEELS 6
#define DR_LAKE_X    (-22.0f)
#define DR_LAKE_Z    (-8.0f)
#define DR_LAKE_R    50.0f
#define DR_WATER_Y   (-0.8f)
typedef struct {
    V3 hub, axis;
    float size;
} Pinwheel;
static Pinwheel dr_pinwheel[DR_PINWHEELS];
static int dr_pinwheel_n;
static float dr_road_y[TRACK_SAMPLES];
static V3 dr_eye[2];           /* the rock towers' centres */
static float dr_eye_r[2];

/* Valley floor: rolling hills, climbing toward the edges, a lake basin in
   the infield, carved so the road always sits on top: within a bank's
   width of a barrier the ground drops just below the road (the low road),
   or to the valley floor under the corkscrews and the summit ridge. */
static float dr_hills(float x, float z) {
    float h = 2.4f * sinf(x * 0.021f + 0.4f) + 2.0f * sinf(z * 0.017f + 1.3f) + 2.8f * sinf((x + z) * 0.011f) +
              1.2f * sinf((x - z) * 0.031f + 0.7f);
    float far = sqrtf(x * x * 0.8f + z * z) - 230.0f;
    return h + 0.05f * fmaxf(0.0f, far);
}

/* The valley's height as a smooth function: rolling hills, the lake's
   basin, and kept under the road: within a few metres of the rails the
   grass stays a metre under the deck at its low edge (a banked turn's
   inside is lower than its centre), rising from there at 0.6 m a metre;
   under a road more than 3 m up (on the rock) it goes down to the valley
   floor. The ground mesh is built from this on a grid; dr_terrain reads
   the mesh itself. */
static float dr_terrain_raw(float x, float z) {
    float t = dr_hills(x, z);
    float dl = sqrtf((x - DR_LAKE_X) * (x - DR_LAKE_X) + (z - DR_LAKE_Z) * (z - DR_LAKE_Z));
    if(dl < 80.0f) t = fmaxf(t, 0.5f);
    t = lerpf(-3.2f, t, smooth01(34.0f, 58.0f, dl));
    if(x > -200.0f && x < 220.0f && z > -180.0f && z < 170.0f) {
        for(int i = 0; i < TRACK_SAMPLES; i++) {
            const TrackSample *a = &track.smp[i];
            float dx = x - a->p.x, dz = z - a->p.z;
            bool high = dr_road_y[i] > 3.0f;
            float low_edge = dr_road_y[i] - fabsf(a->bank) * SK_RAIL;
            float lim = high ? -1.2f : low_edge - 1.0f;
            if(lim >= t) continue;
            float e = sqrtf(dx * dx + dz * dz) - (high ? 30.0f : SK_RAIL + 4.0f);
            t = fminf(t, lim + 0.6f * fmaxf(0.0f, e));
        }
    }
    return t;
}

/* The ground mesh: a 17 m grid of dr_terrain_raw, each cell a road
   crosses pulled down so its flat triangles stay under the deck (between
   grid points they would otherwise bulge up through a narrow road).
   Triangles as the strips draw them: each cell split on its (i, j) to
   (i + 1, j + 1) diagonal. */
#define DR_GX 41
#define DR_GZ 36
#define DR_G0X (-340.0f)
#define DR_G0Z (-300.0f)
#define DR_GSTEP 17.0f
static float dr_gy[DR_GZ][DR_GX];
static bool dr_gy_ready;

float dr_terrain(float x, float z) {
    float fi = (x - DR_G0X) / DR_GSTEP, fj = (z - DR_G0Z) / DR_GSTEP;
    if(!dr_gy_ready || fi < 0 || fj < 0 || fi >= DR_GX - 1 || fj >= DR_GZ - 1) return dr_terrain_raw(x, z);
    int i = (int)fi, j = (int)fj;
    float u = fi - i, v = fj - j;
    float h00 = dr_gy[j][i], h10 = dr_gy[j][i + 1], h01 = dr_gy[j + 1][i], h11 = dr_gy[j + 1][i + 1];
    if(u >= v) return h00 + (h10 - h00) * u + (h11 - h10) * v;   /* (0,0) (1,0) (1,1) */
    return h00 + (h11 - h01) * u + (h01 - h00) * v;              /* (0,0) (1,1) (0,1) */
}

/* The deck's height at lap distance s and lateral d (banking and all). */
static float dr_deck_y(float s, float d) {
    return track_road_y(s, clampf(d, -SK_RAIL, SK_RAIL));
}

static void dr_ground_init(void) {
    dr_gy_ready = false;
    for(int j = 0; j < DR_GZ; j++)
        for(int i = 0; i < DR_GX; i++) dr_gy[j][i] = dr_terrain_raw(DR_G0X + i * DR_GSTEP, DR_G0Z + j * DR_GSTEP);
    /* Every point of every deck, out past the grass banks, holds the
       corners of its cell a metre under the deck there. */
    for(float s = 0; s < track.length; s += 1.5f) {
        for(float d = -(SK_RAIL + 4.0f); d <= SK_RAIL + 4.01f; d += 1.0f) {
            V3 p = track_point(s, d);
            float lim = dr_deck_y(s, d) - 1.0f - 0.25f * fmaxf(0.0f, fabsf(d) - SK_RAIL);
            float fi = (p.x - DR_G0X) / DR_GSTEP, fj = (p.z - DR_G0Z) / DR_GSTEP;
            int i = (int)floorf(fi), j = (int)floorf(fj);
            if(i < 0 || j < 0 || i + 1 >= DR_GX || j + 1 >= DR_GZ) continue;
            for(int q = 0; q < 4; q++) {
                float *h = &dr_gy[j + (q >> 1)][i + (q & 1)];
                if(*h > lim) *h = lim;
            }
        }
    }
    dr_gy_ready = true;
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    /* The check: no grass anywhere over any deck. */
    int over = 0;
    float worst = -1e9f, ws = 0, wd = 0;
    for(float s = 0; s < track.length; s += 0.5f)
        for(float d = -SK_RAIL; d <= SK_RAIL + 0.01f; d += 0.5f) {
            V3 p = track_point(s, d);
            float excess = dr_terrain(p.x, p.z) - (dr_deck_y(s, d) - 0.5f);
            if(excess > 0) over++;
            if(excess > worst) {
                worst = excess;
                ws = s;
                wd = d;
            }
        }
    printf("DK GRASS %d deck points with grass over them; closest at s %.1f d %.1f, %.2f m %s the deck's underside\n", over, (double)ws,
           (double)wd, (double)fabsf(worst), worst > 0 ? "over" : "under");
#endif
}

/* Grass colour: lighter on the ridges, deeper in the hollows. */
static uint32_t dr_grass_color(float y) {
    float t = clampf((y + 5.0f) / 14.0f, 0.0f, 1.0f);
    return color_lerp(argb(255, 196, 236, 110), argb(255, 246, 255, 170), t);
}

static void dr_build_ground(void) {
    float xs[DR_GX], zs[DR_GZ];
    const int nx = DR_GX, nz = DR_GZ;
    for(int i = 0; i < nx; i++) xs[i] = DR_G0X + i * DR_GSTEP;
    for(int j = 0; j < nz; j++) zs[j] = DR_G0Z + j * DR_GSTEP;
    float (*hy)[DR_GX] = dr_gy;
    for(int cz = 0; cz + 1 < nz; cz += 4) {
        for(int cx = 0; cx + 1 < nx; cx += 8) {
            chunk_begin(HF_OP, 2000.0f);
            bool far = true;
            for(int j = cz; j < cz + 4 && j + 1 < nz; j++) {
                float zm = (zs[j] + zs[j + 1]) * 0.5f;
                float xm = (xs[cx] + xs[cx + 8 < nx ? cx + 8 : nx - 1]) * 0.5f;
                if(fabsf(xm) < 260.0f && fabsf(zm) < 230.0f) far = false;
                strip_begin(h_meadow);
                for(int i = cx; i <= cx + 8 && i < nx; i++) {
                    for(int k = 1; k >= 0; k--) {
                        float x = xs[i], z = zs[j + k], y = hy[j + k][i];
                        uint32_t col = dr_grass_color(y);
                        if(i + 1 < nx) {
                            float gx = (hy[j + k][i + 1] - y) / (xs[i + 1] - x);
                            col = color_scale(col, clampf(1.0f + gx * 0.5f, 0.78f, 1.08f));
                        }
                        svp(v3(x, y, z), x / 9.0f, z / 9.0f, col);
                    }
                }
                strip_end();
            }
            if(cur_chunk) cur_chunk->far = far;
            chunk_end();
        }
    }
    /* Flat meadow out to the horizon beyond the grid. */
    chunk_begin(HF_OP, 4000.0f);
    const float G = 340.0f, E = 3000.0f, y = dr_terrain_raw(G, G);
    uint32_t col = dr_grass_color(y);
    quad(h_meadow, v3(-E, y, E), v3(E, y, E), v3(-E, y, G), v3(E, y, G), -E / 9, E / 9, E / 9, G / 9, col);
    quad(h_meadow, v3(-E, y, -G), v3(E, y, -G), v3(-E, y, -E), v3(E, y, -E), -E / 9, -G / 9, E / 9, -E / 9, col);
    quad(h_meadow, v3(-E, y, G), v3(-G, y, G), v3(-E, y, -G), v3(-G, y, -G), -E / 9, G / 9, -G / 9, -G / 9, col);
    quad(h_meadow, v3(G, y, G), v3(E, y, G), v3(G, y, -G), v3(E, y, -G), G / 9, G / 9, E / 9, -G / 9, col);
    chunk_end();
    /* The lake: a still disc of water. */
    chunk_begin(HF_OP, 1500.0f);
    strip_begin(h_water);
    const int sides = 16;
    uint32_t wc = argb(255, 70, 150, 235);
    for(int k = 0; k < sides; k++) {
        int i = (k & 1) ? (sides - (k + 1) / 2) % sides : k / 2;
        float a = (float)i / sides * DK_TAU;
        V3 p = v3(DR_LAKE_X + sinf(a) * DR_LAKE_R, DR_WATER_Y, DR_LAKE_Z + cosf(a) * DR_LAKE_R);
        svp(p, p.x / 26.0f, p.z / 26.0f, wc);
    }
    strip_end();
    chunk_end();
}

/* The corkscrews pass over their own first quarter turn: there the lower
   road runs through a tunnel carved in one mass of rock that rises from
   the valley floor to the underside of the ledge above, so the ledge
   needs no skirt of its own there. */
/* ---- Rock ----
   Each corkscrew climbs a mountain: a surface of revolution about its
   tower whose height at any angle is the deck of the highest road there
   (a 450-degree helix wraps the tower, so every angle has one), sloping
   down to the valley outside the rail. The summit road crosses between
   the two on a ridge mound built the same way along it. dr_rock_y gives
   the rock's height anywhere, with the roads' cuttings carved out; a road
   sample the rock buries across its whole width runs through a tunnel.
   That height function and the tunnel bores are dumped by a
   -DDK_DUMP_ROCK build and baked into one mesh by
   tools/build_canyon_rock.py (make canyon-rock), which cuts the bores and
   the road out of it with booleans; dr_build_rock draws the result. */
#define DR_DECK   0.75f    /* rock stays this far under the road surface */
#define DR_SLOPE  1.25f    /* the mountainside falls this much per metre out */
#define DR_TUNNEL_WALL  4.5f   /* the arch springs from here */
#define DR_TUNNEL_CROWN 7.4f
#define DR_TUNNEL_D     (SK_RAIL + 0.1f)
#define DR_COVER  8.0f     /* rock this far over a road buries it; less, it cuts a cutting */

typedef struct {
    V3 eye, v0;      /* the tower's centre; unit radial at the helix start */
    float R, s0, len, dir, top;
} Mountain;
static Mountain dr_mtn[2];
static int dr_mtn_n;
static float dr_ridge_s0, dr_ridge_s1;   /* the summit road's extent */

/* Angle of (x, z) round the mountain, in the helix's turn direction from
   its start, in [0, 2pi), and the distance out. */
static float dr_mtn_angle(const Mountain *m, float x, float z, float *r) {
    float wx = x - m->eye.x, wz = z - m->eye.z;
    *r = sqrtf(wx * wx + wz * wz);
    float cw = atan2f(m->v0.z * wx - m->v0.x * wz, m->v0.x * wx + m->v0.z * wz);
    float a = m->dir > 0 ? cw : -cw;
    if(a < 0) a += DK_TAU;
    return a;
}

/* The lap distance of the surface road at angle a: where the helix is
   stacked over its own start, whichever lap is higher (the second lap of
   a climbing corkscrew, the first of a falling one); the lower runs
   through a tunnel beneath. */
static float dr_mtn_s(const Mountain *m, float a) {
    float extra = m->len / m->R - DK_TAU;   /* the stacked quarter, radians */
    float s = m->s0 + m->R * a;
    if(a < extra) {
        float s2 = s + m->R * DK_TAU;
        if(track_height(s2) > track_height(s)) s = s2;
    }
    return track_wrap(s);
}

/* The mountain's height at distance r out at angle a, or -1e9 beyond it. */
static float dr_mtn_profile(const Mountain *m, float a, float r) {
    if(r < m->R - SK_RAIL - 0.3f) return m->top;
    float s = dr_mtn_s(m, a);
    float d = (m->dir > 0 ? m->R - r : r - m->R);   /* lateral, + to the right */
    if(r <= m->R + SK_RAIL) return track_road_y(s, d) - DR_DECK;
    float edge = track_road_y(s, m->dir > 0 ? -SK_RAIL : SK_RAIL) - DR_DECK;
    float h = edge - DR_SLOPE * (r - m->R - SK_RAIL);
    return h < -40.0f ? -1e9f : h;
}

static float dr_mtn_y(const Mountain *m, float x, float z) {
    float r;
    float a = dr_mtn_angle(m, x, z, &r);
    return dr_mtn_profile(m, a, r);
}

/* The ridge mound under the summit road: the deck within the rails,
   sloping away beyond, between the two mountains. */
static float dr_ridge_y(float x, float z) {
    V3 A = track_point(dr_ridge_s0, 0), B = track_point(dr_ridge_s1, 0);
    float ax = B.x - A.x, az = B.z - A.z, L2 = ax * ax + az * az;
    float t = ((x - A.x) * ax + (z - A.z) * az) / L2;
    if(t < -0.1f || t > 1.1f) return -1e9f;
    float s = dr_ridge_s0 + clampf(t, 0, 1) * (dr_ridge_s1 - dr_ridge_s0);
    V3 tg = track_tangent(s);
    float d = (x - A.x) * tg.z - (z - A.z) * tg.x;   /* dot with right = (t.z, -t.x) */
    float ad = fabsf(d);
    if(ad <= SK_RAIL) return track_road_y(s, d) - DR_DECK;
    float edge = track_road_y(s, d > 0 ? SK_RAIL : -SK_RAIL) - DR_DECK;
    float h = edge - DR_SLOPE * (ad - SK_RAIL);
    return h < -40.0f ? -1e9f : h;
}

/* The bare rock (mountains and ridge) at (x, z), or -1e9 where none. */
static float dr_bare_rock_y(float x, float z) {
    float h = dr_ridge_y(x, z);
    for(int i = 0; i < dr_mtn_n; i++) h = fmaxf(h, dr_mtn_y(&dr_mtn[i], x, z));
    return h;
}

/* Every road sample is classed once against the bare rock: buried (a
   full tunnel's height of rock over it, so it runs through a tunnel),
   cutting (rock over or about it but not that much, so it cuts a cutting
   whose floor is its deck and whose walls climb at the mountainside's
   slope), or clear. The rock anywhere then follows the nearest sample's
   class, so a cutting runs right up to a tunnel's portal face and no road
   is left half buried. */
enum { ROCK_CLEAR, ROCK_CUTTING, ROCK_BURIED };
static uint8_t dr_class[TRACK_SAMPLES];

static float dr_cover[TRACK_SAMPLES];   /* bare rock over the road at each sample */
static float dr_crown(float s);

/* The nearest road sample to (x, z) whose deck is at height y's level
   (y from 2 m under it to 8.5 m over: its tunnel's bore and a little),
   or -1 if none is; and, along the chord to whichever neighbour the
   point projects onto, how far toward it (0..1) and the distance out from
   that chord. The road passes over both tunnels, nearer in plan to
   their edges than their own centrelines are. */
static int dr_nearest(float x, float y, float z, float *t, float *dist) {
    int best = -1;
    float bd = 1e30f;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        if(y > dr_road_y[i] + 8.5f || y < dr_road_y[i] - 2.0f) continue;
        float dx = x - track.smp[i].p.x, dz = z - track.smp[i].p.z, d2 = dx * dx + dz * dz;
        if(d2 < bd) {
            bd = d2;
            best = i;
        }
    }
    if(best < 0) return -1;
    V3 a = track.smp[best].p, b = track.smp[(best + 1) % TRACK_SAMPLES].p;
    float ax = b.x - a.x, az = b.z - a.z;
    float u = ((x - a.x) * ax + (z - a.z) * az) / (ax * ax + az * az);
    if(u < 0.0f) {
        /* Behind this sample: the chord from the one before. */
        b = a;
        a = track.smp[(best + TRACK_SAMPLES - 1) % TRACK_SAMPLES].p;
        ax = b.x - a.x;
        az = b.z - a.z;
        u = ((x - a.x) * ax + (z - a.z) * az) / (ax * ax + az * az);
        best = (best + TRACK_SAMPLES - 1) % TRACK_SAMPLES;
    }
    u = clampf(u, 0.0f, 1.0f);
    float px = a.x + ax * u - x, pz = a.z + az * u - z;
    *t = u;
    *dist = sqrtf(px * px + pz * pz);
    return best;
}

static float dr_mouth_dist[TRACK_SAMPLES];   /* a buried sample's distance in from the nearest mouth */

/* The rock's height at (x, z), or -1e9 where there is none: the bare rock
   with every nearby road's cutting carved out of it, and how far (x, z)
   is from the nearest road's centreline. Where rock stands over a road
   but not deep enough to bury it, the cut takes it down to the deck
   across the road and lets it rise again as a bank beyond the rail.
   Toward a tunnel the cut lifts off as the cover thickens, within the
   last metre of cover over the road (the portal's headwall) and over the
   last ten out on the mountainside, so the mouth has banked wing walls
   that join the slope either side. Each road's height and cover are read
   between its two nearest samples, so the cut is continuous along it.
   Every road within reach cuts, not just the nearest: under a ledge the
   road above is nearest, but it is the cutting beneath that shapes the
   bank up to the ledge. */
static float dr_rock_at(float x, float z, float *dist) {
    float h = dr_bare_rock_y(x, z);
    *dist = 1e9f;
    /* Each pass of the road near the point cuts once, from the chord of
       that pass the point is nearest to: a run of consecutive chords
       within reach is one pass, and only its nearest chord's cut
       applies. (Letting every chord cut, clamped to its ends, drags a
       mouth's thin cover down the road into the tunnel.) */
    float run_d = 1e9f, run_cut = 1e9f, run_floor = -1e9f, floor = -1e9f;
    bool in_run = false;
    for(int n = 0; n <= TRACK_SAMPLES; n++) {
        int i = n % TRACK_SAMPLES, k = (i + 1) % TRACK_SAMPLES;
        V3 a = track.smp[i].p, b = track.smp[k].p;
        float ax = b.x - a.x, az = b.z - a.z;
        float u = ((x - a.x) * ax + (z - a.z) * az) / (ax * ax + az * az);
        float uc = clampf(u, 0.0f, 1.0f);
        float px = a.x + ax * uc - x, pz = a.z + az * uc - z, d2 = px * px + pz * pz;
        bool near = d2 < 30.0f * 30.0f && u > -0.5f && u < 1.5f && n < TRACK_SAMPLES;
        if(near) {
            float d = sqrtf(d2);
            if(d < *dist) *dist = d;
            if(d < run_d) {
                run_d = d;
                run_cut = 1e9f;
                /* The cover along the chord. Across a tunnel mouth it is
                   the open sample's all the way to the buried one: the
                   portal stands at that sample, and a blend would carry
                   the roof's cover metres past it as a slab of rock over
                   the open road, where no mesh is (rightly) built. */
                bool bi = dr_class[i] == ROCK_BURIED, bk = dr_class[k] == ROCK_BURIED;
                float cover = bi == bk ? lerpf(dr_cover[i], dr_cover[k], uc) : bi ? (uc < 0.001f ? dr_cover[i] : dr_cover[k])
                                                                               : (uc > 0.999f ? dr_cover[k] : dr_cover[i]);
                /* Beside and over a mouth the rock stands at least a metre
                   above the arch's crown at the portal, falling away 1.6 m
                   a metre beyond it: otherwise a mouth whose open side is
                   only just under the tunnel threshold would leave the
                   tunnel's last metres standing proud of the rock. */
                float head = -1e9f;
                if(bi != bk) {
                    float clen = sqrtf(ax * ax + az * az);
                    float beyond = (bi ? uc : 1.0f - uc) * clen;
                    int portal = bi ? i : k;
                    head = dr_road_y[portal] + dr_crown(track.smp[portal].s) + 1.0f - 1.6f * beyond;
                }
                /* Every road cuts, whatever its cover (the cover is the
                   thinnest across the road; rock can still stand over
                   the other half of a "clear" sample): where the rock is
                   already below the cut the cut changes nothing. */
                /* The deck's height across the road, banking and all
                   (on the low side of a banked turn the centre's height
                   would stand above the deck). */
                V3 rr = vlerp(track.smp[i].r, track.smp[k].r, uc);
                float lat = clampf((x - a.x - ax * uc) * rr.x + (z - a.z - az * uc) * rr.z, -SK_RAIL, SK_RAIL);
                float road = lerpf(dr_road_y[i], dr_road_y[k], uc) + lerpf(track.smp[i].bank, track.smp[k].bank, uc) * lat;
                /* Under the road itself the rock stays at the deck's
                   underside: no other road's cutting undercuts a ledge. */
                run_floor = d <= SK_RAIL + 0.5f ? road - DR_DECK : -1e9f;
                float out = fmaxf(0.0f, d - SK_RAIL - 0.5f);
                float w = fminf(1.0f, out / 3.0f);
                float rise = lerpf(1.0f, 10.0f, w);
                float lift = fmaxf(0.0f, cover - (DR_COVER - rise)) * (DR_COVER + DR_DECK + 1.0f) / rise;
                run_cut = fmaxf(road - DR_DECK + DR_SLOPE * out + lift, head);
            }
            in_run = true;
        }
        else if(in_run) {
            h = fminf(h, run_cut);
            floor = fmaxf(floor, run_floor);
            in_run = false;
            run_d = 1e9f;
        }
    }
    /* Never above the bare rock: the floor only keeps rock that is there. */
    float bare = dr_bare_rock_y(x, z);
    return fmaxf(h, fminf(floor, bare));
}

static float dr_rock_y(float x, float z) {
    float dist;
    return dr_rock_at(x, z, &dist);
}

static void dr_classify(void) {
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        /* The cover is the thinnest across the road: a ledge along a
           hillside, rock over its inner half and air beyond its outer
           rail, is a cutting, not a tunnel with one wall in the open. */
        float rock = 1e9f;
        for(int side = -1; side <= 1; side++) {
            V3 p = track_point(track.smp[i].s, (float)side * SK_RAIL);
            rock = fminf(rock, dr_bare_rock_y(p.x, p.z));
        }
        dr_cover[i] = rock < -1e8f ? -50.0f : rock - dr_road_y[i];
        if(rock > dr_road_y[i] + DR_COVER) dr_class[i] = ROCK_BURIED;
        else if(rock > dr_road_y[i] - DR_DECK + 0.2f) dr_class[i] = ROCK_CUTTING;
        else dr_class[i] = ROCK_CLEAR;
    }
    /* How far into its tunnel each buried sample is, for the daylight
       spilling in at the mouths. */
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        dr_mouth_dist[i] = 0;
        if(dr_class[i] != ROCK_BURIED) continue;
        int k = 1;
        while(k < TRACK_SAMPLES / 2 && dr_class[(i + k) % TRACK_SAMPLES] == ROCK_BURIED &&
              dr_class[(i + TRACK_SAMPLES - k) % TRACK_SAMPLES] == ROCK_BURIED)
            k++;
        dr_mouth_dist[i] = (float)k * track.ds;
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    /* The runs, for checking where the tunnels and cuttings fall. */
    for(int i = 0; i < TRACK_SAMPLES;) {
        int j = i;
        while(j + 1 < TRACK_SAMPLES && dr_class[j + 1] == dr_class[i]) j++;
        if(dr_class[i] != ROCK_CLEAR)
            printf("DK ROCK %s s %.0f to %.0f\n", dr_class[i] == ROCK_BURIED ? "tunnel" : "cutting",
                   (double)track.smp[i].s, (double)track.smp[j].s);
        i = j + 1;
    }
#endif
}

/* True where the rock buries the road at lap distance s. */
static bool dr_buried(float s) {
    int i = (int)(track_wrap(s) / track.ds + 0.5f) % TRACK_SAMPLES;
    return dr_class[i] == ROCK_BURIED;
}

/* The tunnel's crown over the road at s: the full arch where the rock is
   deep, lower where it thins toward a mouth. */
static float dr_crown(float s) {
    V3 p = track_point(s, 0);
    /* From the bare rock: next to a cutting the cut rock sits at road
       level, which would pull the arch down through the road. */
    float cover = dr_bare_rock_y(p.x, p.z) - track_height(s);
    return fmaxf(DR_TUNNEL_WALL + 0.5f, fminf(DR_TUNNEL_CROWN, cover - 0.4f));
}

#ifdef DK_DUMP_ROCK
static float dr_arch_h(float s, float t) {
    return lerpf(DR_TUNNEL_WALL, dr_crown(s), sinf(t * DK_PI));
}
#endif

/* The road's own banks and tunnels, per chunk: a grass bank from each
   rail's foot to the ground where the road runs at ground level, and a
   tunnel wherever the rock buries it. */
void dr_skirts(const float *sv, int n) {
    for(int side = -1; side <= 1; side += 2) {
        bool open = false;
        for(int k = 0; k < n; k++) {
            float s = sv[k];
            V3 a = f8_pt(s, side * SK_RAIL, -SK_DEPTH);
            V3 b = track_point(s, side * (SK_RAIL + 3.5f));
            b.y = dr_terrain(b.x, b.z) - 0.3f;
            /* Only an embankment: a road more than 3 m up is carried by
               rock (or bridges a cutting on its own rock underside). */
            bool on_rock = dr_rock_y(a.x, a.z) > a.y - 1.5f;
            if(on_rock || b.y > a.y - 0.3f || a.y - b.y > 3.0f) {
                if(open) strip_end();
                open = false;
                continue;
            }
            if(!open) {
                strip_begin(h_meadow);
                open = true;
            }
            uint32_t ca = argb(255, 236, 246, 232), cb = 0xffffffff;
            if(side > 0) {
                svp(a, a.x / 9.0f, a.z / 9.0f, ca);
                svp(b, b.x / 9.0f, b.z / 9.0f, cb);
            }
            else {
                svp(b, b.x / 9.0f, b.z / 9.0f, cb);
                svp(a, a.x / 9.0f, a.z / 9.0f, ca);
            }
        }
        if(open) strip_end();
    }
}

/* ---- Tiki torches ----
   Along the road every 44 m (12 m in the tunnels), on alternate sides
   but always the outside of a corkscrew (the inside is the tower): a
   bamboo pole with a thatched head on the verge, or a bracketed torch on
   a tunnel wall. Their flames are the braziers' (th_draw_flames), and
   they are real light sources for the tunnels: everything built while
   svp_tint is dr_tint is dimmed inside a tunnel, with daylight spilling
   in at the mouths, and warmed by the torches it is near. */
#define DR_TORCHES 72
#define DR_TORCH_R 15.0f       /* a torch lights the rock this far */
typedef struct {
    V3 base, flame;
    float yaw, side, s_at;
    bool wall;
} Torch;
static Torch dr_torch[DR_TORCHES];
static int dr_torch_n;

static int dr_seg_at(float s) {
    int seg = 0;
    for(int i = 1; i < 16; i++)
        if(track_seg_s(i) <= s) seg = i;
    return seg;
}

/* Where a torch pole stands just beyond the rail: on the rock when the
   road is carried by rock, else on the grass bank down from the deck. */
static float dr_verge_y(float s, float d) {
    V3 p = track_point(s, d);
    float rock = dr_rock_y(p.x, p.z);
    float rail = track_road_y(s, d > 0 ? SK_RAIL : -SK_RAIL) - SK_DEPTH;
    if(rock > rail - 1.5f) return fmaxf(rock, track_road_y(s, d) - 1.5f);
    float ground = dr_terrain(p.x, p.z) - 0.3f;
    if(rail - ground > 3.0f) return rail;
    return lerpf(rail, ground, (fabsf(d) - SK_RAIL) / 3.5f);
}

/* The nearest road chord to (x, z) more than 40 m along the lap from lap
   distance s (another pass of the road, not this one round a bend), and
   its distance. Its sample's
   height is the other road's deck. */
static int dr_nearest_other_any(float x, float z, float s, float *dist) {
    int best = -1;
    float bd = 1e30f;
    for(int k = 0; k < TRACK_SAMPLES; k++) {
        if(fabsf(track_delta(track.smp[k].s, s)) < 40.0f) continue;
        V3 a = track.smp[k].p, b = track.smp[(k + 1) % TRACK_SAMPLES].p;
        float ax = b.x - a.x, az = b.z - a.z;
        float u = clampf(((x - a.x) * ax + (z - a.z) * az) / (ax * ax + az * az), 0.0f, 1.0f);
        float px = a.x + ax * u - x, pz = a.z + az * u - z, d2 = px * px + pz * pz;
        if(d2 < bd) {
            bd = d2;
            best = k;
        }
    }
    *dist = sqrtf(bd);
    return best;
}

static void dr_place_torches(void) {
    dr_torch_n = 0;
    const float hoops[3] = {0.0f, track_seg_s(4) + 28.0f, track_seg_s(8) + 40.0f};
    float side = 1.0f, out_side = 1.0f;
    int outdoor = 0;
    for(float s = 16.0f; s < track.length - 10.0f && dr_torch_n < DR_TORCHES;) {
        bool tunnel = dr_buried(s);
        int seg = dr_seg_at(s);
        if(seg == 3) side = -1.0f;
        else if(seg == 5) side = 1.0f;
        else side = -side;
        bool near_hoop = false;
        for(int h = 0; h < 3; h++)
            if(fabsf(track_delta(hoops[h], s)) < 12.0f) near_hoop = true;
        /* Outside, only every other one (44 m apart), still on
           alternating sides away from the corkscrews. */
        bool keep = !near_hoop;
        float sd = side;
        if(keep && !tunnel) {
            keep = (outdoor++ & 1) == 0;
            if(keep && seg != 3 && seg != 5) sd = out_side = -out_side;
        }
        if(keep) {
            Torch *t = &dr_torch[dr_torch_n++];
            V3 tg = track_tangent(s);
            t->yaw = atan2f(tg.x, tg.z);
            t->side = sd;
            t->s_at = s;
            t->wall = tunnel;
            if(tunnel) {
                t->base = f8_pt(s, sd * 9.25f, 2.05f);
                t->flame = f8_pt(s, sd * 9.25f, 2.95f);
            }
            else {
                t->base = track_point(s, sd * 10.6f);
                t->base.y = dr_verge_y(s, sd * 10.6f);
                t->flame = vadd(t->base, v3(0, 2.85f, 0));
            }
            /* Not on another pass of the road: beside the descent the
               summit runs within a lane of the verge at the same height. */
            float od;
            int other = dr_nearest_other_any(t->base.x, t->base.z, s, &od);
            if(other >= 0 && od < SK_RAIL + 1.5f && fabsf(dr_road_y[other] - t->base.y) < 4.0f) {
                dr_torch_n--;
#if defined(DK_AUTOTEST) && !defined(DK_HW)
                printf("DreamKarts torches: none at s %.0f, it would stand on the road at s %.0f\n", (double)s, (double)track.smp[other].s);
#endif
            }
        }
        s += tunnel ? 12.0f : 22.0f;
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    int walls = 0;
    for(int i = 0; i < dr_torch_n; i++) walls += dr_torch[i].wall;
    printf("DreamKarts torches: %d in the tunnels, %d outside\n", walls, dr_torch_n - walls);
#endif
}

static uint32_t dr_tint(V3 p, uint32_t c) {
    float t, dist;
    int j = dr_nearest(p.x, p.y, p.z, &t, &dist);
    bool inside = j >= 0 && dr_class[j] == ROCK_BURIED && dist < SK_RAIL + 2.5f;
    float amb = 1.0f, k = 0.3f;
    if(inside) {
        float spill = clampf(1.0f - dr_mouth_dist[j] / 10.0f, 0.0f, 1.0f);
        amb = 0.36f + 0.64f * spill;
        k = 1.0f;
    }
    float glow = 0;
    for(int i = 0; i < dr_torch_n; i++) {
        V3 d = vsub(p, dr_torch[i].flame);
        float d2 = vdot(d, d);
        if(d2 >= DR_TORCH_R * DR_TORCH_R) continue;
        float f = 1.0f - sqrtf(d2) / DR_TORCH_R;
        glow += f * f;
    }
    glow = fminf(glow * k * 1.6f, 1.3f);
    float r = amb + glow, g = amb + glow * 0.72f, b = amb + glow * 0.42f;
    int cr = (int)(((c >> 16) & 255) * r), cg = (int)(((c >> 8) & 255) * g), cb = (int)((c & 255) * b);
    return argb((int)(c >> 24), cr > 255 ? 255 : cr, cg > 255 ? 255 : cg, cb > 255 ? 255 : cb);
}

static void dr_build_torches(void) {
    uint32_t wood_lo = argb(255, 110, 72, 36), wood_hi = argb(255, 150, 104, 56), straw = argb(255, 220, 190, 110);
    /* Four torches to a chunk (they run in order along the road), drawn
       to 160 m: a 2 m pole is a few pixels by then, and its flame still
       shows. Square poles; the heads keep six sides. */
    for(int i = 0; i < dr_torch_n; i++) {
        const Torch *t = &dr_torch[i];
        if(i % 4 == 0) chunk_begin(HF_OP, 160.0f);
        if(t->wall) {
            /* A bracket out from the wall, the torch standing on it. */
            V3 bc = vmad(t->base, heading_right(t->yaw), t->side * 0.2f);
            box(h_bark, bc, v3(0.4f, 0.06f, 0.06f), t->yaw, wood_lo, 1.0f);
            cylinder(h_bark, vadd(t->base, v3(0, 0.05f, 0)), 0.1f, 0.08f, 0.5f, 4, wood_lo, wood_hi, 1, 1);
            cylinder(h_thatch, vadd(t->base, v3(0, 0.55f, 0)), 0.09f, 0.3f, 0.35f, 6, wood_lo, straw, 1, 1);
        }
        else {
            cylinder(h_bark, t->base, 0.17f, 0.13f, 2.3f, 4, wood_lo, wood_hi, 1, 2);
            cylinder(h_thatch, vadd(t->base, v3(0, 2.3f, 0)), 0.13f, 0.38f, 0.45f, 6, wood_lo, straw, 1, 1);
        }
        if(i % 4 == 3 || i + 1 == dr_torch_n) chunk_end();
        if(th_flame_n < TH_FLAMES) th_flame_p[th_flame_n++] = t->flame;
    }
}

/* A hoop the road passes through: a ring of orange beads (24 segments,
   light and dark by turns, the bead's belly a shade brighter) with a soft
   halo. */
void dr_hoop(float s) {
    V3 c = track_point(s, 0.0f), r = vsub(track_point(s, 1.0f), c);
    c.y = track_road_y(s, 0.0f) + 2.0f;
    const int segs = 12;
    const float ri = 12.6f, ro = 14.1f;
    chunk_begin(HF_OP, 1100.0f);
    /* A checkered loop over the road, as in the old hill zones: its face
       and a rim. It runs from a little below the verge either side up and
       over, so its feet stand in the ground rather than hang in the air
       (the rest of a full ring would hang down into the rock, and a
       tunnel, beneath). */
    V3 fwd = vscale(vnorm(vcross(r, v3(0, 1, 0))), 1.2f);
    strip_begin(h_ehchecker);
    for(int i = 0; i <= segs; i++) {
        float a = lerpf(-0.26f, DK_PI + 0.26f, (float)i / segs);
        V3 dir = vadd(vscale(r, cosf(a)), v3(0, sinf(a), 0));
        svp(vmad(c, dir, ro + 1.2f), (float)i * 0.5f, 0, 0xffffffff);
        svp(vmad(c, dir, ri), (float)i * 0.5f, 1, argb(255, 200, 200, 200));
    }
    strip_end();
    strip_begin(h_ehchecker);
    for(int i = 0; i <= segs; i++) {
        float a = lerpf(-0.26f, DK_PI + 0.26f, (float)i / segs);
        V3 dir = vadd(vscale(r, cosf(a)), v3(0, sinf(a), 0));
        svp(vadd(vmad(c, dir, ro + 1.2f), fwd), (float)i * 0.5f, 0, argb(255, 220, 220, 220));
        svp(vsub(vmad(c, dir, ro + 1.2f), fwd), (float)i * 0.5f, 0.5f, argb(255, 220, 220, 220));
    }
    strip_end();
    chunk_end();
}

static void dr_add_pinwheel(V3 base, uint32_t h) {
    if(dr_pinwheel_n >= DR_PINWHEELS) return;
    float yaw = (float)((h >> 8) & 255) / 255.0f * DK_TAU;
    V3 axis = heading_fwd(yaw);
    /* A sunflower: a green stem, the head turns to the camera and spins. */
    cylinder(h_col, base, 0.16f, 0.12f, 4.2f, 5, argb(255, 60, 140, 40), argb(255, 90, 180, 60), 1, 1);
    dr_pinwheel[dr_pinwheel_n++] = (Pinwheel){vadd(base, v3(0, 4.4f, 0)), axis, 1.5f};
}

/* A meadow disc (the rim polygon as one zigzag strip). */
static void dr_meadow_disc(V3 c, float r, int sides) {
    strip_begin(h_meadow);
    uint32_t top = shade(0xffffffff, v3(0, 1, 0));
    for(int k = 0; k < sides; k++) {
        int i = (k & 1) ? (sides - (k + 1) / 2) % sides : k / 2;
        float a = (float)i / sides * DK_TAU;
        V3 p = vadd(c, v3(sinf(a) * r, 0, cosf(a) * r));
        svp(p, p.x / 9.0f, p.z / 9.0f, top);
    }
    strip_end();
}

/* A rock tower in the eye of a corkscrew: a cylinder of blue rock from
   the valley floor to just over the summit (straight-sided, so it never
   leans into the ledge), a meadow on top with trees and a pinwheel. */
#define DR_FALLS 2
/* A waterfall down a tunnel's mountain flank: its lip at the rock's top
   edge, a knee where the flank's facets meet, its foot on the valley
   floor; t runs along the flank, n points out of it. */
typedef struct {
    V3 lip, knee, foot, t, n;
} Waterfall;
static Waterfall dr_fall[DR_FALLS];
static int dr_fall_n;

/* The waterfalls, each frame: the water scrolls down the sheet, widening
   and thinning as it falls, and a mist cloud breathes at the foot. */
void dr_draw_waterfalls(void) {
    if(!world_canyon) return;
    for(int i = 0; i < dr_fall_n; i++) {
        const Waterfall *f = &dr_fall[i];
        if(!r_sphere_visible(f->knee, vlen(vsub(f->lip, f->foot)) * 0.6f + 4.0f)) continue;
        float h = f->lip.y - f->foot.y, reps = h / 5.0f;
        float scroll = fmodf(world_time * 1.6f + (float)i * 0.37f, 1.0f);
        r_hdr(h_waterfall);
        const int segs = 6;
        RV v[2 * (6 + 1)];
        int n = 0;
        for(int k = 0; k <= segs; k++) {
            float u = (float)k / segs;
            /* Down the two facets, a little out from the rock. */
            V3 p = u < 0.5f ? vlerp(f->lip, f->knee, u * 2.0f) : vlerp(f->knee, f->foot, u * 2.0f - 1.0f);
            p = vmad(p, f->n, 0.5f);
            p.y += 0.3f;
            float w = 1.6f + 1.6f * u;
            float a = u < 0.1f ? u * 10.0f : 1.0f - 0.2f * u;
            uint32_t c = color_alpha(0xffffffff, a);
            V3 l = vmad(p, f->t, -w), rr = vmad(p, f->t, w);
            v[n++] = (RV){l.x, l.y, l.z, 0, u * reps - scroll, c};
            v[n++] = (RV){rr.x, rr.y, rr.z, 1, u * reps - scroll, c};
        }
        r_strip(v, n);
        /* Mist at the foot, breathing. */
        r_hdr(h_glow_add);
        for(int m = 0; m < 3; m++) {
            float ph = world_time * (1.1f + 0.2f * (float)m) + (float)(m * 2 + i);
            float size = 3.2f + 1.2f * sinf(ph) + (float)m;
            V3 c = vmad(f->foot, f->n, 1.5f + (float)m * 1.2f);
            c = vmad(c, f->t, sinf(ph * 0.7f) * 1.5f);
            c.y += 1.0f + 0.6f * (float)m;
            r_billboard(c, size, size * 0.7f, ph * 0.2f, 0, 0, 1, 1, argb(70, 210, 235, 255));
        }
    }
}

/* A waterfall down a mountain's outer slope at helix angle a: from the
   rock's edge outside the rail to where the slope meets the valley, and
   a pool there. */
static void dr_place_fall(const Mountain *m, float a) {
    if(dr_fall_n >= DR_FALLS) return;
    float cw = m->dir > 0 ? a : -a;
    V3 dir = v3(m->v0.x * cosf(cw) + m->v0.z * sinf(cw), 0, -m->v0.x * sinf(cw) + m->v0.z * cosf(cw));
    float r0 = m->R + SK_RAIL + 0.3f;
    float edge = dr_mtn_profile(m, a, r0);
    V3 foot = vmad(m->eye, dir, r0);
    float ground = dr_terrain(foot.x, foot.z) - 0.3f;
    float r1 = r0 + fmaxf(edge - ground, 1.0f) / DR_SLOPE;
    Waterfall *f = &dr_fall[dr_fall_n++];
    f->lip = vmad(m->eye, dir, r0);
    f->lip.y = edge;
    f->foot = vmad(m->eye, dir, r1);
    f->foot.y = ground;
    f->knee = vlerp(f->lip, f->foot, 0.5f);
    f->n = dir;
    f->t = v3(dir.z, 0, -dir.x);
    chunk_begin(HF_OP, 1500.0f);
    strip_begin(h_water);
    const int sides = 10;
    for(int k = 0; k < sides; k++) {
        int i = (k & 1) ? (sides - (k + 1) / 2) % sides : k / 2;
        float ang = (float)i / sides * DK_TAU;
        V3 q = vadd(f->foot, v3(sinf(ang) * 5.0f, 0.1f, cosf(ang) * 5.0f));
        q = vmad(q, dir, 3.0f);
        svp(q, q.x / 9.0f, q.z / 9.0f, argb(255, 120, 190, 240));
    }
    strip_end();
    chunk_end();
}

static void dr_tower(V3 eye, float r, float top, uint32_t seed) {
    chunk_begin(HF_OP, 2000.0f);
    float bottom = dr_terrain(eye.x, eye.z) - 2.0f;
#ifdef DK_ROCK_TINT
    rock_pass++;   /* the tower and its top are rock to the leak test */
    cylinder(h_islandrock, v3(eye.x, bottom, eye.z), r, r, top - bottom, 16, argb(255, 230, 220, 60), argb(255, 255, 240, 90), 12.0f,
             (top - bottom) / 4.5f);
#else
    cylinder(h_islandrock, v3(eye.x, bottom, eye.z), r, r, top - bottom, 16, argb(255, 184, 188, 204),
             argb(255, 238, 240, 248), 12.0f, (top - bottom) / 4.5f);
#endif
    chunk_end();
    chunk_begin(HF_OP, 2000.0f);
    V3 c = v3(eye.x, top, eye.z);
    dr_meadow_disc(c, r, 16);
    uint32_t h = seed;
    dr_add_pinwheel(c, h);
    chunk_end();
    for(int t = 0; t < 3; t++) {
        h = h * 1664525u + 1013904223u;
        float a = (float)((h >> 8) & 255) / 255.0f * DK_TAU, d = r * (0.2f + 0.5f * (float)((h >> 16) & 255) / 255.0f);
        add_palm(vadd(c, v3(sinf(a) * d, 0, cosf(a) * d)), 6.0f + (float)((h >> 24) & 3), a, 0.15f, h >> 3);
    }
#ifdef DK_ROCK_TINT
    rock_pass--;
#endif
}

/* A totem pole: three or four carved and painted blocks, each with one
   of the four patterns of the totem texture (its 2 by 2 cells), turned a
   touch and stacked, a cap on top and a pair of spread wings, red over
   black with cream tips, across the road-facing side (fwd). */
static void dr_totem_block(V3 c, float half, float hh, float yaw, int cell, uint32_t tint, bool cap) {
    V3 ax = vscale(heading_right(yaw), half), az = vscale(heading_fwd(yaw), half);
    /* Half a texel in from the cell's edges: the cells sit side by side. */
    const float in = 0.5f / 64.0f;
    float u0 = (float)(cell & 1) * 0.5f + in, u1 = u0 + 0.5f - 2.0f * in;
    float v0 = (float)(cell >> 1) * 0.5f + in, v1 = v0 + 0.5f - 2.0f * in;
    V3 corner[4] = {vadd(vsub(c, ax), az), vadd(vadd(c, ax), az), vsub(vadd(c, ax), az), vsub(vsub(c, ax), az)};
    /* The four sides wrap round as one run of the caller's strip, the
       pattern mirrored on every other side (the patterns are symmetric),
       each corner lit by its outward direction. */
    for(int i = 0; i <= 4; i++) {
        V3 p = corner[i & 3];
        uint32_t col = shade(tint, vsub(p, c));
        float u = (i & 1) ? u1 : u0;
        if(i == 0) svj(vadd(p, v3(0, hh, 0)), u, v0, col);
        else svp(vadd(p, v3(0, hh, 0)), u, v0, col);
        svp(vsub(p, v3(0, hh, 0)), u, v1, col);
    }
    if(cap) {
        uint32_t col = shade(tint, v3(0, 1, 0));
        float cu = 0.25f, cv = 0.06f;   /* the carved rim's dark wood */
        svj(vadd(corner[0], v3(0, hh, 0)), cu, cv, col);
        svp(vadd(corner[1], v3(0, hh, 0)), cu, cv, col);
        svp(vadd(corner[3], v3(0, hh, 0)), cu, cv, col);
        svp(vadd(corner[2], v3(0, hh, 0)), cu, cv, col);
    }
}

static void dr_totem(V3 base, V3 fwd, uint32_t seed) {
    static const uint32_t tints[3] = {0xffffffff, 0xfff0e0d0, 0xffe0d8c8};
    float yaw = atan2f(fwd.x, fwd.z);
    int blocks = 3 + (int)(seed & 1);
    const float half = 0.75f, hh = 0.8f;
    chunk_begin(HF_OP, 350.0f);
    float y = base.y - 0.3f;
    int cell = (int)((seed >> 4) & 3);
    strip_begin(h_totem);
    for(int k = 0; k < blocks; k++) {
        seed = seed * 1664525u + 1013904223u;
        float turn = ((float)((seed >> 8) & 255) / 255.0f - 0.5f) * 0.2f;
        float hx = half * (k == blocks - 1 ? 1.1f : 1.0f);
        dr_totem_block(v3(base.x, y + hh, base.z), hx, hh, yaw + turn, (cell + k) & 3, tints[(seed >> 16) % 3], k == blocks - 1);
        y += 2.0f * hh;
    }
    strip_end();
    /* The wings, spread from the top block: a black inner band, red
       feathers out to cream tips. */
    V3 right = heading_right(yaw), nf = fwd;
    float wy = y - hh * 1.1f;
    strip_begin(h_col);
    for(int side = -1; side <= 1; side += 2) {
        V3 o = vscale(right, (float)side);
        V3 root_lo = vadd(v3(base.x, wy, base.z), vscale(o, half));
        V3 root_hi = vadd(root_lo, v3(0, 1.0f, 0));
        V3 mid_lo = vadd(root_lo, vadd(vscale(o, 0.7f), v3(0, 0.2f, 0)));
        V3 mid_hi = vadd(root_lo, vadd(vscale(o, 0.7f), v3(0, 1.35f, 0)));
        V3 tip_lo = vadd(root_lo, vadd(vscale(o, 1.9f), v3(0, 0.6f, 0)));
        V3 tip_hi = vadd(root_lo, vadd(vscale(o, 2.1f), v3(0, 1.9f, 0)));
        uint32_t black = shade(argb(255, 40, 32, 28), nf), red = shade(argb(255, 200, 56, 36), nf),
                 cream = shade(argb(255, 240, 224, 186), nf);
        svj(root_hi, 0, 0, black);
        svp(root_lo, 0, 0, black);
        svp(mid_hi, 0, 0, red);
        svp(mid_lo, 0, 0, black);
        svp(tip_hi, 0, 0, cream);
        svp(tip_lo, 0, 0, red);
    }
    strip_end();
    chunk_end();
}

/* A bush: a lumpy dome of leaves, a little sunk into the grass. */
static void dr_bush(V3 c, float r, float h, uint32_t seed) {
    enum { SEG = 7 };
    static const float elev[3] = {0.0f, 0.55f, 0.55f};   /* fractions of a quarter turn: a band and a cap */
    float jit[SEG];
    for(int i = 0; i < SEG; i++) {
        seed = seed * 1664525u + 1013904223u;
        jit[i] = 0.85f + 0.3f * (float)((seed >> 8) & 255) / 255.0f;
    }
    V3 ring[3][SEG + 1];
    for(int e = 0; e < 3; e++)
        for(int i = 0; i <= SEG; i++) {
            float a = (float)i / SEG * DK_TAU, el = elev[e] * DK_PI * 0.5f;
            float rr = r * cosf(el) * jit[i % SEG];
            ring[e][i] = v3(c.x + sinf(a) * rr, c.y - 0.3f + h * sinf(el), c.z + cosf(a) * rr);
        }
    V3 apex = v3(c.x, c.y - 0.3f + h, c.z);
    uint32_t base_col = argb(255, 255, 255, 255);
    /* The band, then the cap joined on: one strip. */
    strip_begin(h_bush);
    for(int e = 0; e < 3; e += 2) {
        for(int i = 0; i <= SEG; i++) {
            float a = (float)i / SEG * DK_TAU;
            V3 n0 = v3(sinf(a), elev[e] * 1.4f, cosf(a));
            if(e == 0) {
                V3 n1 = v3(sinf(a), elev[e + 1] * 1.4f, cosf(a));
                svp(ring[e + 1][i], (float)i / SEG * 3.0f, (e + 1) * 0.6f, shade(base_col, n1));
                svp(ring[e][i], (float)i / SEG * 3.0f, e * 0.6f, shade(base_col, n0));
            }
            else {
                if(i == 0) svj(apex, 0.0f, 1.8f, shade(base_col, v3(0, 1, 0)));
                else svp(apex, (float)i / SEG * 3.0f, 1.8f, shade(base_col, v3(0, 1, 0)));
                svp(ring[e][i], (float)i / SEG * 3.0f, 1.2f, shade(base_col, n0));
            }
        }
    }
    strip_end();
}

/* True when (x, z) is clear of the road, the lake and the towers by the
   given margin. */
static bool dr_clear(float x, float z, float margin) {
    if(sk_road_dist(v3(x, 0, z)) < margin + 12.0f) return false;
    float dl = sqrtf((x - DR_LAKE_X) * (x - DR_LAKE_X) + (z - DR_LAKE_Z) * (z - DR_LAKE_Z));
    if(dl < DR_LAKE_R + 6.0f + margin) return false;
    for(int e = 0; e < 2; e++) {
        float dx = x - dr_eye[e].x, dz = z - dr_eye[e].z;
        if(sqrtf(dx * dx + dz * dz) < 40.0f + margin) return false;
    }
    return true;
}

void dr_place_props(void) {
    dr_pinwheel_n = 0;
    dr_fall_n = 0;
    dr_tower(dr_eye[0], dr_eye_r[0], 20.8f, 0x1234u);
    dr_tower(dr_eye[1], dr_eye_r[1], 20.8f, 0x5678u);
    /* A waterfall down each mountain's outer slope: the first's faces the
       start straight (south-west), the second's the long way home
       (north-west, clear of the exit turn's cutting on the north face). */
    dr_place_fall(&dr_mtn[0], 5.2f);
    dr_place_fall(&dr_mtn[1], 3.93f);
    /* Nothing hangs in the air: the sparkle slots go unused. */
    int n = 0;
    for(int i = n; i < SK_SPARKLES; i++) sk_sparkle_kind[i] = 3;
    sk_beam_n = 0;
    /* Palms in clumps over the hills with bushes at their feet, totem
       poles, and sunflowers on the higher tops. */
    uint32_t r = 0x7d1eu;
    int trees = 0, rocks = 0, bushes = 0;
    for(int tries = 0; tries < 1400 && (trees < DR_TREES || rocks < DR_ROCKS); tries++) {
        r = r * 1664525u + 1013904223u;
        /* Half the tries stay near the course, so the roadsides fill. */
        float ex = (tries & 1) ? 300.0f : 150.0f, ez = (tries & 1) ? 260.0f : 130.0f;
        float x = -ex + (float)((r >> 8) & 1023) / 1023.0f * 2.0f * ex;
        float z = -ez + (float)((r >> 18) & 1023) / 1023.0f * 2.0f * ez;
        if(!dr_clear(x, z, 4.0f)) continue;
        float y = dr_terrain(x, z);
        bool rock = (r & 7) == 0 && rocks < DR_ROCKS;
        if(!rock && trees >= DR_TREES) continue;
        bool far = fabsf(x) > 200.0f || fabsf(z) > 180.0f;
        chunk_begin(HF_OP, 1400.0f);
        if(cur_chunk) cur_chunk->far = far;
        if(rock) {
            /* A totem pole, facing the nearest road. */
            chunk_end();
            TrackPos tp;
            track_locate(v3(x, y, z), -1, &tp);
            V3 to = vsub(tp.center, v3(x, 0, z));
            to.y = 0;
            dr_totem(v3(x, y, z), vnorm(to), r >> 3);
            chunk_begin(HF_OP, 1400.0f);
            rocks++;
        }
        else {
            int clump = 1 + (int)((r >> 5) & 3);
            chunk_end();
            for(int t = 0; t < clump && trees < DR_TREES; t++, trees++) {
                r = r * 1664525u + 1013904223u;
                float a = (float)((r >> 8) & 255) / 255.0f * DK_TAU, d = (float)(t) * 4.5f;
                float tx = x + sinf(a) * d, tz = z + cosf(a) * d;
                add_palm(v3(tx, dr_terrain(tx, tz) - 0.2f, tz), 7.0f + (float)((r >> 24) & 3) * 1.2f, a, 0.18f, r >> 5);
            }
            /* Bushes round the clump's foot, kept off the road and out of
               the lake. */
            chunk_begin(HF_OP, 160.0f);
            if(cur_chunk) cur_chunk->far = far;
            int nb = 1 + (int)((r >> 11) & 1);
            for(int b = 0; b < nb; b++) {
                r = r * 1664525u + 1013904223u;
                float a = (float)((r >> 8) & 255) / 255.0f * DK_TAU, d = 2.5f + (float)((r >> 16) & 7) * 0.5f;
                float bx = x + sinf(a) * d, bz = z + cosf(a) * d;
                float br = 1.4f + (float)((r >> 20) & 7) * 0.18f;
                float dl = sqrtf((bx - DR_LAKE_X) * (bx - DR_LAKE_X) + (bz - DR_LAKE_Z) * (bz - DR_LAKE_Z));
                if(sk_road_dist(v3(bx, 0, bz)) < SK_RAIL + br + 2.0f || dl < DR_LAKE_R + br + 2.0f) continue;
                dr_bush(v3(bx, dr_terrain(bx, bz), bz), br, br * (0.75f + (float)((r >> 24) & 3) * 0.08f), r >> 3);
                bushes++;
            }
            chunk_end();
            chunk_begin(HF_OP, 1400.0f);
            if(cur_chunk) cur_chunk->far = far;
            if(y > 4.0f && !far && (r & 0x30) == 0) dr_add_pinwheel(v3(x + 6.0f, dr_terrain(x + 6.0f, z), z), r);
        }
        chunk_end();
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DreamKarts canyon props: %d palms, %d totem poles, %d bushes\n", trees, rocks, bushes);
#endif
}

/* The canyon's punch-through props: the sunflower heads turning about
   their axes, one textured quad each. */
void dr_draw_pt(void) {
    if(!world_canyon) return;
    /* Sunflower heads, facing the camera and spinning. */
    r_hdr(h_sunflower);
    for(int i = 0; i < dr_pinwheel_n; i++) {
        const Pinwheel *w = &dr_pinwheel[i];
        if(!r_sphere_visible(w->hub, w->size * 1.5f)) continue;
        V3 d = vsub(w->hub, r_cam.pos);
        float lim = world_split ? 220.0f : 420.0f;
        if(vdot(d, d) > lim * lim) continue;
        float t = world_time * (1.2f + 0.3f * (float)i) + (float)i;
        /* Half a metre toward the camera, so the head always covers the
           top of its stem (which ends just under the hub). */
        V3 c = vmad(w->hub, vnorm(vscale(d, -1.0f)), 0.5f);
        r_billboard(c, w->size * 2.0f, w->size * 2.0f, t, 0, 0, 1, 1, sunset_grade(0xffffffff));
    }
}

/* The rock: one mesh baked by tools/build_canyon_rock.py (Blender) from
   the same height function, with the tunnels and the road cut out of it,
   drawn as it comes in strips, chunk by chunk. Colour comes from the
   normal: lit by the sun, a shade darker on steep faces. The texture is
   box-projected per face in the bake. */
/* One of the baked rocks: its strips in its chunks. split: 0 the full
   rock (a split screen skips it), 1 the coarse one only a split screen
   draws. */
typedef struct {
    const dk_rock_vert_t *verts;
    const uint16_t *index;
    const uint8_t *strip_len;
    const uint16_t (*chunk)[2];
    int nchunks;
} RockMesh;

static void dr_build_rock_mesh(const RockMesh *m, int split) {
    int at = 0;
    for(int c = 0; c < m->nchunks; c++) {
        chunk_begin(HF_OP, 2000.0f);
        if(cur_chunk) {
            if(split) cur_chunk->lod = 3;
            else cur_chunk->far = 1;
        }
        for(int k = 0; k < m->chunk[c][1]; k++) {
            int n = m->strip_len[m->chunk[c][0] + k];
            strip_begin(h_islandrock);
            for(int q = 0; q < n; q++) {
                const dk_rock_vert_t *v = &m->verts[m->index[at + q]];
                V3 p = v3(v->x * 0.01f, v->y * 0.01f, v->z * 0.01f);
                V3 nn = v3(v->nx / 127.0f, v->ny / 127.0f, v->nz / 127.0f);
                float steep = clampf((0.85f - fabsf(nn.y)) / 0.6f, 0.0f, 1.0f);
                uint32_t col = shade(color_lerp(argb(255, 226, 228, 238), argb(255, 168, 172, 190), steep), nn);
#ifdef DK_ROCK_TINT
                col = argb(255, 255, 90, 90);
#endif
                svp(p, v->u / 256.0f, v->v / 256.0f, col);
            }
            strip_end();
            at += n;
        }
        chunk_end();
    }
}

static void dr_build_rock(void) {
#ifdef DK_ROCK_TINT
    rock_pass++;
#endif
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    int pool0 = pool_n, strips0 = strip_n;
#endif
    static const RockMesh full = {dk_rock_verts, dk_rock_index, dk_rock_strip_len, dk_rock_chunk, DK_ROCK_CHUNKS};
    static const RockMesh split = {dk_rock_split_verts, dk_rock_split_index, dk_rock_split_strip_len, dk_rock_split_chunk,
                                   DK_ROCK_SPLIT_CHUNKS};
    dr_build_rock_mesh(&full, 0);
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    int pool1 = pool_n, strips1 = strip_n;
#endif
    dr_build_rock_mesh(&split, 1);
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DreamKarts rock: %d vertices, %d strips; split screen %d, %d (world before it: %d, %d)\n", pool1 - pool0,
           strips1 - strips0, pool_n - pool1, strip_n - strips1, pool0, strips0);
#endif
#ifdef DK_ROCK_TINT
    rock_pass--;
#endif
}

#ifdef DK_ROCK_TINT
/* The leak test's oracle: the rock height function itself as a plain
   2.5 m heightfield, white, in chunks of its own after everything else.
   Drawn alone (world_diag_mode(1)) it shows what the rock meshes should
   cover; a shot pair from the same camera, real against reference,
   gives tools/leakcheck.py the holes. */
static void dr_build_reference(void) {
    const float step = 2.5f;
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        x0 = fminf(x0, track.smp[i].p.x); x1 = fmaxf(x1, track.smp[i].p.x);
        z0 = fminf(z0, track.smp[i].p.z); z1 = fmaxf(z1, track.smp[i].p.z);
    }
    x0 -= 60.0f; x1 += 60.0f; z0 -= 60.0f; z1 += 60.0f;
    diag_ref_first = chunk_n;
    rock_pass++;
    for(float z = z0; z < z1; z += step * 4.0f) {
        chunk_begin(HF_OP, 4000.0f);
        for(float zr = z; zr < z + step * 4.0f && zr < z1; zr += step) {
            bool open = false;
            for(float x = x0; x <= x1; x += step) {
                V3 p[2];
                bool ok = true;
                for(int q = 0; q < 2; q++) {
                    float zz = zr + q * step;
                    float h = dr_rock_y(x, zz), g = dr_terrain(x, zz);
                    if(h < g - 0.3f) ok = false;
                    p[q] = v3(x, h, zz);
                }
                if(!ok) {
                    if(open) strip_end();
                    open = false;
                    continue;
                }
                if(!open) {
                    strip_begin(h_col);
                    open = true;
                }
                svp(p[0], 0, 0, 0xffffffff);
                svp(p[1], 0, 0, 0xffffffff);
            }
            if(open) strip_end();
        }
        chunk_end();
    }
    rock_pass--;
    printf("DK REF rock reference: chunks %d to %d, pool %d of %d\n", diag_ref_first, chunk_n - 1, pool_n, MAX_POOL);
}
#endif

#ifdef DK_DUMP_ROCK
/* The rock's source data for tools/build_canyon_rock.py (Blender): the
   height field and the valley floor on a 1 m grid over the rock, the
   tunnel bores and the road's clearance as profiles in world space
   (lofted there and cut from the rock), and the towers. Heights are
   printed in decimetres. Coordinates are the game's (y up). */
static void dr_dump_rock(void) {
    const float step = 1.0f;
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for(float z = -200.0f; z <= 200.0f; z += 4.0f)
        for(float x = -200.0f; x <= 260.0f; x += 4.0f)
            if(dr_bare_rock_y(x, z) > dr_terrain(x, z) - 0.5f) {
                x0 = fminf(x0, x); x1 = fmaxf(x1, x);
                z0 = fminf(z0, z); z1 = fmaxf(z1, z);
            }
    x0 = floorf(x0 - 8.0f); z0 = floorf(z0 - 8.0f);
    int nx = (int)((x1 + 8.0f - x0) / step) + 1, nz = (int)((z1 + 8.0f - z0) / step) + 1;
    printf("DKR GRID %.1f %.1f %.2f %d %d\n", (double)x0, (double)z0, (double)step, nx, nz);
    for(int pass = 0; pass < 2; pass++)
        for(int j = 0; j < nz; j++) {
            printf("DKR %c %d", pass ? 'G' : 'H', j);
            for(int i = 0; i < nx; i++) {
                float x = x0 + i * step, z = z0 + j * step;
                /* Inside a tower (and just outside its wall) the rock
                   carries on at the ledge's height, read just outside:
                   the tower's own cylinder and top stand there, and a
                   plateau smoothed out past its wall would show. */
                float sx = x, sz = z;
                for(int e = 0; e < 2; e++) {
                    float dx = x - dr_eye[e].x, dz = z - dr_eye[e].z, r = sqrtf(dx * dx + dz * dz), rr = dr_eye_r[e] + 1.5f;
                    if(r < rr) {
                        float k = r > 0.01f ? rr / r : 0.0f;
                        sx = dr_eye[e].x + dx * k;
                        sz = dr_eye[e].z + dz * k;
                    }
                }
                float v = pass ? dr_terrain(x, z) : dr_rock_y(sx, sz);
                printf(" %d", v < -1e8f ? -9999 : (int)lroundf(v * 10.0f));
            }
            printf("\n");
        }
    /* Bores: every buried run, a metre past each portal. Clearance: the
       open road, rail to rail, from just above the rock under the deck to
       8 m up, so nothing stands in the road. */
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        int prev = (i + TRACK_SAMPLES - 1) % TRACK_SAMPLES;
        if(dr_class[i] != ROCK_BURIED || dr_class[prev] == ROCK_BURIED) continue;
        int j = i;
        while(dr_class[(j + 1) % TRACK_SAMPLES] == ROCK_BURIED) j = (j + 1) % TRACK_SAMPLES;
        float sa = track.smp[i].s - 1.0f, sb = track.smp[j].s + 1.0f;
        printf("DKR BORE %.1f %.1f\n", (double)sa, (double)sb);
        int n = (int)ceilf((sb - sa) / 4.0f);
        for(int k = 0; k <= n; k++) {
            float s = track_wrap(lerpf(sa, sb, (float)k / n));
            printf("DKR P");
            const int across = 5;
            for(int q = 0; q <= across + 3; q++) {
                float d, dy;
                if(q == 0) { d = -DR_TUNNEL_D; dy = -0.6f; }
                else if(q == across + 3) { d = DR_TUNNEL_D; dy = -0.6f; }
                else {
                    float t = (float)(q - 1) / (across + 1);
                    d = lerpf(-DR_TUNNEL_D, DR_TUNNEL_D, t);
                    dy = dr_arch_h(s, t);
                }
                V3 p = f8_pt(s, d, dy);
                printf(" %.2f %.2f %.2f", (double)p.x, (double)p.y, (double)p.z);
            }
            printf("\n");
        }
        /* The open road from this tunnel's end to the next one's start. */
        int nb = (j + 1) % TRACK_SAMPLES;
        while(dr_class[nb] != ROCK_BURIED) nb = (nb + 1) % TRACK_SAMPLES;
        float ca = track.smp[j].s, cb = track.smp[nb].s;
        if(cb <= ca) cb += track.length;
        printf("DKR CLEAR %.1f %.1f\n", (double)ca, (double)cb);
        int m = (int)ceilf((cb - ca) / 3.0f);
        for(int k = 0; k <= m; k++) {
            float s = track_wrap(lerpf(ca, cb, (float)k / m));
            /* Out past the barriers, so no rock face stands in a panel. */
            const float w = SK_RAIL + 0.45f;
            V3 p[4] = {f8_pt(s, -w, -0.6f), f8_pt(s, -w, 8.0f), f8_pt(s, w, 8.0f), f8_pt(s, w, -0.6f)};
            printf("DKR P");
            for(int q = 0; q < 4; q++) printf(" %.2f %.2f %.2f", (double)p[q].x, (double)p[q].y, (double)p[q].z);
            printf("\n");
        }
    }
    for(int e = 0; e < 2; e++)
        printf("DKR TOWER %.3f %.3f %.3f %.3f\n", (double)dr_eye[e].x, (double)dr_eye[e].z, (double)dr_eye_r[e], 20.8);
    printf("DKR END\n");
}
#endif

void build_canyon(void) {
    for(int i = 0; i < TRACK_SAMPLES; i++) dr_road_y[i] = track_height(track.smp[i].s);
    /* The towers stand in the eyes of the corkscrews (the first turns
       right, so its eye is on the right; the second turns left), just
       inside the rails. */
    dr_eye[0] = track_point(track_seg_s(3), 24.0f);
    dr_eye[1] = track_point(track_seg_s(5), -22.0f);
    dr_eye_r[0] = 24.0f - SK_RAIL - 0.3f;
    dr_eye_r[1] = 22.0f - SK_RAIL - 0.3f;
    /* The mountains and the ridge, as height functions first. */
    dr_mtn_n = 2;
    for(int i = 0; i < 2; i++) {
        Mountain *m = &dr_mtn[i];
        int seg = i ? 5 : 3;
        m->eye = dr_eye[i];
        m->R = i ? 22.0f : 24.0f;
        m->s0 = track_seg_s(seg);
        m->len = track_seg_s(seg + 1) - track_seg_s(seg);
        m->dir = i ? -1.0f : 1.0f;
        m->top = 20.8f;
        V3 p0 = track_point(m->s0, 0);
        m->v0 = vnorm(v3(p0.x - m->eye.x, 0, p0.z - m->eye.z));
    }
    dr_ridge_s0 = track_seg_s(4);
    dr_ridge_s1 = track_seg_s(5);
    dr_classify();
    dr_ground_init();
#ifdef DK_DUMP_ROCK
    dr_dump_rock();
#endif
    dr_place_torches();
#ifdef DK_ROCK_TINT
    diag_on = true;
#endif
#ifdef DK_CHUNK_STATS
    ck_first[CK_GROUND] = chunk_n;
#endif
    dr_build_ground();
#ifdef DK_CHUNK_STATS
    ck_first[CK_DECK] = chunk_n;
#endif
    sk_road_hdr = *h_canyonroad;   /* the deck's header, before any update */
    sk_vscale = track.length / roundf(track.length / 8.0f);
#ifdef DK_ROCK_TINT
    (void)dr_tint;   /* the leak test reads pure tints: no torchlight */
#else
    svp_tint = dr_tint;   /* the tunnels: dim, torchlit */
#endif
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        sk_build_track_chunk(c);
#ifdef DK_CHUNK_STATS
    ck_first[CK_ROCK] = chunk_n;
#endif
    dr_build_rock();
    svp_tint = NULL;
#ifdef DK_CHUNK_STATS
    ck_first[CK_PROPS] = chunk_n;
#endif
    sk_place_props();
#ifdef DK_CHUNK_STATS
    ck_first[CK_TORCH] = chunk_n;
#endif
    dr_build_torches();
#ifdef DK_CHUNK_STATS
    ck_first[CK_N] = chunk_n;
#endif
#ifdef DK_ROCK_TINT
    diag_on = false;
    dr_build_reference();
#endif
}

/* Spiral Canyon's road as a split screen draws it beyond SPLIT_NEAR: every
   other station (the chunk's ends kept, so it meets its neighbours), the
   cobbles, the curbs out to the barrier (over the thin grass ledge), the
   barriers and the grass banks; five strips where the full road has nine.
   It follows sk_build_track_chunk's. */
void dr_build_deck_coarse(int c) {
    float full[MAX_SVALS], sv[MAX_SVALS];
    int nf = f8_chunk_svals(c, full), n = 0;
    for(int k = 0; k < nf; k += 2) sv[n++] = full[k];
    if(sv[n - 1] != full[nf - 1]) sv[n++] = full[nf - 1];
    chunk_begin(HF_OP, 2000.0f);
    if(cur_chunk) cur_chunk->lod = 2;
    f8_road_ribbon(sv, n, &sk_road_hdr, 8.0f, sk_vscale);
    for(int side = -1; side <= 1; side += 2) {
        strip_begin(h_curb);
        for(int k = 0; k < n; k++) {
            V3 a = f8_pt(sv[k], side * 8.0f, 0.0f), b = f8_pt(sv[k], side * SK_RAIL, 0.0f);
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
        strip_begin(h_barrier);
        for(int k = 0; k < n; k++) {
            V3 p = f8_pt(sv[k], side * SK_RAIL, 0.95f), b = f8_pt(sv[k], side * SK_RAIL, -0.1f);
            if(side < 0) {
                svp(b, sv[k] / 4.0f, 1.0f, 0xffffffff);
                svp(p, sv[k] / 4.0f, 0.0f, 0xffffffff);
            }
            else {
                svp(p, sv[k] / 4.0f, 0.0f, 0xffffffff);
                svp(b, sv[k] / 4.0f, 1.0f, 0xffffffff);
            }
        }
        strip_end();
    }
    dr_skirts(sv, n);
    chunk_end();
}

#ifdef DK_ROCK_TINT
/* The leak test's colouring: rock surfaces keep their tints (rock_pass
   is set while they are built, and the road deck's rock underside counts);
   everything else is dark grey and the sky black, so a shot pair, real
   against the reference rock, tells rock from not-rock. */
int rock_pass;
bool diag_on;
int diag_mode;        /* 0: the real world; 1: only the reference rock */
int diag_ref_first;   /* the reference rock's first chunk */

void world_diag_mode(int mode) {
    diag_mode = mode;
}
uint32_t diag_colour(V3 p, uint32_t c) {
    if(rock_pass) return c;
    /* Anything under the rock's surface (the valley floor runs on under
       the mountains, the decks through the tunnels) is inside the solid:
       teal, a tint, so the leak test reads it as rock, not a way out. */
    return dr_rock_y(p.x, p.z) > p.y + 0.5f ? argb(255, 40, 170, 170) : 0xff404040;
}
#endif

/* The leak test's camera spots: a point inside Spiral Canyon's rock at
   lap distance s and lateral d, or false where there is none. Over a
   tunnel it is just above the arch's crown; elsewhere two metres under
   the rock's surface where the rock stands well clear of the ground,
   never inside a tower, a tunnel's bore or a road's deck. */
#ifdef DK_ROCK_TINT
static float world_rock_mesh_top(float x, float z, int *count, float *lowest);
#endif

bool world_leak_point(float s, float d, V3 *out) {
    if(!world_canyon) return false;
    V3 p = track_point(s, d);
    float ground = dr_terrain(p.x, p.z), nearest, rock = dr_rock_at(p.x, p.z, &nearest), h;
    if(rock < -1e8f) return false;
    /* Over another road's deck (the summit's flank over the corkscrew
       below it, say) the deck is the surface, not rock: skipped. */
    if(fabsf(d) >= 9.8f && nearest < 9.8f) return false;
    if(fabsf(d) < 9.8f) {
        if(!dr_buried(s)) return false;
        h = track_road_y(s, 0.0f) + dr_crown(s) + 0.6f;
        if(h > rock - 0.4f) return false;
    }
    else {
        if(rock < ground + 4.0f) return false;
        for(int e = 0; e < dr_mtn_n; e++) {
            float dx = p.x - dr_mtn[e].eye.x, dz = p.z - dr_mtn[e].eye.z;
            if(sqrtf(dx * dx + dz * dz) < dr_mtn[e].R - SK_RAIL) return false;
        }
        h = rock - 2.0f;
        if(h < ground + 0.5f) return false;
    }
    /* Not in any tunnel's bore, nor in a road's deck. */
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        int k = (i + 1) % TRACK_SAMPLES;
        V3 a = track.smp[i].p, b = track.smp[k].p;
        float ax = b.x - a.x, az = b.z - a.z;
        float u = clampf(((p.x - a.x) * ax + (p.z - a.z) * az) / (ax * ax + az * az), 0.0f, 1.0f);
        float px = a.x + ax * u - p.x, pz = a.z + az * u - p.z;
        if(px * px + pz * pz > 9.8f * 9.8f) continue;
        float road = lerpf(dr_road_y[i], dr_road_y[k], u);
        bool mine = fabsf(d) < 9.8f && fabsf(track_delta(track.smp[i].s, s)) < 6.0f;
        if(!mine && dr_class[i] == ROCK_BURIED && h > road - 1.0f && h < road + 7.6f) return false;
        if(fabsf(h - road) < 1.5f) return false;
    }
#ifdef DK_ROCK_TINT
    {
        /* Not within 16 m of a tunnel mouth: the reference heightfield's
           2.5 m grid smears the headwall, and the wing walls are steeper
           than a camera can be kept under, so the reference is not to be
           trusted there. */
        for(int i = 0; i < TRACK_SAMPLES; i++) {
            int k = (i + 1) % TRACK_SAMPLES;
            if((dr_class[i] == ROCK_BURIED) == (dr_class[k] == ROCK_BURIED)) continue;
            V3 m = track.smp[k].p;
            float dx = p.x - m.x, dz = p.z - m.z;
            if(dx * dx + dz * dz < 16.0f * 16.0f) return false;
        }
        /* The meshes follow the height function on rows metres apart, so
           where it is steep they run under it: the camera goes 1.5 m under
           the lowest real rock vertex near it, else it would look out from
           inside a slope's face. A spot with no rock mesh near it keeps
           the function's height, and is logged: a hole to go and see. */
        int n;
        float low;
        float top = world_rock_mesh_top(p.x, p.z, &n, &low);
        if(fabsf(d) >= 9.8f && n > 0 && low - 1.5f < h) {
            h = low - 1.5f;
            if(h < ground + 0.5f) return false;
        }
        printf("DK LEAKPT s %.0f d %.1f at (%.0f, %.0f) cam %.1f rock %.1f ground %.1f mesh %.1f..%.1f (%d verts)%s\n", (double)s, (double)d,
               (double)p.x, (double)p.z, (double)h, (double)rock, (double)ground, (double)low, (double)top, n, n == 0 ? "  <-- NO ROCK MESH NEAR" : "");
    }
#endif
    *out = v3(p.x, h, p.z);
    return true;
}

bool world_in_rock(V3 p, float margin) {
    return world_canyon && dr_in_rock(p, margin);
}

/* Spiral Canyon: p in the rock, or less than margin metres above it,
   and not in a tunnel's bore. */
bool dr_in_rock(V3 p, float margin) {
    /* The rock never stands above the bare rock: most points stop here. */
    if(p.y > dr_bare_rock_y(p.x, p.z) + margin) return false;
    float dist, rock = dr_rock_at(p.x, p.z, &dist);
    if(p.y > rock + margin) return false;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        int k = (i + 1) % TRACK_SAMPLES;
        if(dr_class[i] != ROCK_BURIED && dr_class[k] != ROCK_BURIED) continue;
        V3 a = track.smp[i].p, b = track.smp[k].p;
        float ax = b.x - a.x, az = b.z - a.z;
        float u = clampf(((p.x - a.x) * ax + (p.z - a.z) * az) / (ax * ax + az * az), 0.0f, 1.0f);
        float px = a.x + ax * u - p.x, pz = a.z + az * u - p.z, out = sqrtf(px * px + pz * pz);
        if(out > DR_TUNNEL_D - margin) continue;
        float road = lerpf(dr_road_y[i], dr_road_y[k], u);
        float arch = lerpf(DR_TUNNEL_WALL, dr_crown(track.smp[i].s), cosf(out / DR_TUNNEL_D * DK_PI * 0.5f));
        if(p.y > road - 0.5f && p.y < road + arch - margin) return false;
    }
    return true;
}

#ifdef DK_ROCK_TINT
/* For the leak test: the highest and lowest real rock vertices within
   3 m of (x, z) horizontally, and how many there are. */
static float world_rock_mesh_top(float x, float z, int *count, float *lowest) {
    float top = -1e9f, low = 1e9f;
    int n = 0;
    int end = chunks[diag_ref_first].first_strip < strip_n ? strips[chunks[diag_ref_first].first_strip].first : pool_n;
    for(int i = 0; i < end; i++) {
        const RV *v = &pool[i];
        int r = (v->c >> 16) & 255, g = (v->c >> 8) & 255, b = v->c & 255;
        int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if(hi - lo < 40) continue;   /* not a rock tint */
        float dx = v->x - x, dz = v->z - z;
        if(dx * dx + dz * dz > 3.0f * 3.0f) continue;
        n++;
        if(v->y > top) top = v->y;
        if(v->y < low) low = v->y;
    }
    *count = n;
    *lowest = low;
    return top;
}
#endif

/* Spiral Canyon's horizon: three ranges of rolling hills all round, far
   and green to near and violet, each a strip of vertex colours from a
   lit ridge down to a hazed foot. Geometry, not a texture: a texture
   ring this far off magnifies its texel steps into a jagged ridge and
   its 4-bit gradient into bands, where a Gouraud edge stays a line. */
void draw_canyon_hills(void) {
    V3 c = r_cam.pos;
    enum { SEGS = 96, PIECE = 8 };
    RV v[2 * (PIECE + 1)];
    r_hdr(h_hills);
    for(int layer = world_split ? 1 : 0; layer < 3; layer++) {   /* a split screen leaves out the farthest */
        float R = 1350.0f - 40.0f * layer;   /* nearer ranges sort in front */
        float t = (float)layer / 2.0f;
        uint32_t ridge = color_lerp(argb(255, 150, 222, 126), argb(255, 112, 148, 158), t);
        uint32_t foot = color_lerp(argb(255, 176, 216, 232), argb(255, 150, 176, 214), t);
        if(world_sunset) {
            /* Purple silhouettes against the sunset, hazier further off. */
            ridge = color_lerp(argb(255, 58, 30, 92), argb(255, 132, 62, 128), t);
            foot = color_lerp(argb(255, 176, 84, 120), argb(255, 226, 120, 128), t);
        }
        /* In pieces, each drawn only when on screen: most of the ring is
           behind the camera, and a strip that crosses the view takes the
           clipping path. */
        for(int p0 = 0; p0 < SEGS; p0 += PIECE) {
            float am = (p0 + PIECE * 0.5f) / SEGS * DK_TAU;
            V3 mid = v3(c.x + sinf(am) * R, 35.0f, c.z + cosf(am) * R);
            if(!r_sphere_visible(mid, R * sinf(DK_PI * PIECE / SEGS) + 45.0f)) continue;
            int n = 0;
            for(int i = p0; i <= p0 + PIECE; i++) {
                float a = (float)i / SEGS * DK_TAU;
                float h = 62.0f - 15.0f * layer;
                h += 11.0f * sinf(a * 3.0f + (float)layer * 1.7f) + 6.0f * sinf(a * 7.0f + (float)layer * 0.9f) +
                     3.0f * sinf(a * 13.0f + (float)layer * 2.3f);
                float x = c.x + sinf(a) * R, z = c.z + cosf(a) * R;
                v[n++] = (RV){x, h, z, 0, 0, ridge};
                v[n++] = (RV){x, 4.0f, z, 0, 0, foot};
            }
            r_strip(v, n);
        }
    }
}
