/* Bay City Hills: a San Francisco street circuit. Victorian row houses in
   their painted colours step up and down the hills along every street;
   the waterfront has pier sheds, palms, fishing boats and a pier with sea
   lions off its head; the crooked street's brick switchbacks run between
   beds of hydrangeas; Coit Tower stands on Telegraph Hill and the city
   rolls away to the south. Across the bay: Alcatraz, the Golden Gate
   Bridge between its headlands with a bank of fog rolling in under it,
   Marin's hills behind. The props are baked by tools/build_sf_props.py
   (Blender) and instanced here; the houses are built here from the facade
   atlas. */
#include "sf_props.h"
#include "world_int.h"
#include "cablecar.h"
#include "audio.h"
#include "engine/sfx.h"

static const pvr_poly_hdr_t *sf_mat_hdr[SFM_COUNT];
static const pvr_poly_hdr_t *h_sfroad, *h_sfrails, *h_sfbrick, *h_sfcurb, *h_sfwalk, *h_sfrailing, *h_sfpier,
    *h_sfpierrail, *h_sffacade, *h_sffacade2, *h_sfbldg, *h_sfroof, *h_sfhedge, *h_sfshed, *h_sfsign, *h_sfbay, *h_sffog, *h_sfwake,
    *h_sfstreet, *h_sfgrass, *h_sfstall, *h_sfsigns;

/* Where the landmarks stand. */
#define SF_BRIDGE_X   (-1000.0f)
#define SF_BRIDGE_Z   450.0f
#define SF_BRIDGE_YAW (20.0f * DK_PI / 180.0f)
#define SF_COIT_X     45.0f
#define SF_COIT_Z     (-100.0f)
#define SF_SEABED     (-6.0f)
#define SF_SUN_DIR    (230.0f * DK_PI / 180.0f)

void sf_init(void) {
    h_sfroad = r_header(TEX_SFROAD, HF_OP | HF_SPEC);
    h_sfrails = r_header(TEX_SFRAILS, HF_OP);
    h_sfbrick = r_header(TEX_SFBRICK, HF_OP);
    h_sfcurb = r_header(TEX_SFCURB, HF_OP);
    h_sfwalk = r_header(TEX_SFWALK, HF_OP);
    h_sfrailing = r_header(TEX_SFRAILING, HF_OP);
    h_sfpier = r_header(TEX_SFPIER, HF_OP);
    h_sfpierrail = r_header(TEX_SFPIERRAIL, HF_OP);
    h_sffacade = r_header(TEX_SFFACADE, HF_OP);
    h_sffacade2 = r_header(TEX_SFFACADE2, HF_OP);
    h_sfbldg = r_header(TEX_SFBLDG, HF_OP);
    h_sfroof = r_header(TEX_SFROOF, HF_OP);
    h_sfhedge = r_header(TEX_SFHEDGE, HF_OP);
    h_sfshed = r_header(TEX_SFSHED, HF_OP);
    h_sfsign = r_header(TEX_SFSIGN, HF_OP | HF_CLAMP);
    h_sfbay = r_header(TEX_SFBAY, HF_OP | HF_SPEC);
    h_sffog = r_header(TEX_SFFOG, HF_TR);
    h_sfwake = r_header(TEX_NONE, HF_TR);
    h_sfstreet = r_header(TEX_SFSTREET, HF_OP);
    h_sfgrass = r_header(TEX_SFGRASS, HF_OP);
    h_sfstall = r_header(TEX_SFSTALL, HF_OP);
    h_sfsigns = r_header(TEX_SFSIGNS, HF_OP | HF_CLAMP);
    sf_mat_hdr[SFM_FLAT] = h_col;
    sf_mat_hdr[SFM_PALM] = r_header(TEX_SFPALM, HF_PT);
    sf_mat_hdr[SFM_PALMBARK] = r_header(TEX_SFPALMBARK, HF_OP);
    sf_mat_hdr[SFM_SIGN] = h_sfsign;
    sf_mat_hdr[SFM_PIER] = h_sfpier;
    sf_mat_hdr[SFM_BLDG] = h_sfbldg;
    sf_mat_hdr[SFM_CABLECAR] = r_header(TEX_SFCABLECAR, HF_OP);
}

const pvr_poly_hdr_t *sf_water_hdr(void) {
    return h_sfbay;
}

/* The bay: grey-green near the shore, deepening to a cold blue. */
uint32_t sf_sea_color(float x, float z) {
    float t = clampf((z - 90.0f) / 500.0f, 0.0f, 1.0f);
    return color_lerp(argb(255, 150, 176, 172), argb(255, 92, 128, 156), t);
}

/* ---- The land ---- */

/* The waterfront: the quay's edge, north of the road, by x. Further along
   the shore curves away: south-east past the Embarcadero, and west round
   the Marina toward the bridge. */
static float sf_shore(float x) {
    float z = lerpf(72.5f, 88.5f, smooth01(-120.0f, -140.0f, x));
    z -= 0.6f * fmaxf(0.0f, x - 150.0f) * smooth01(150.0f, 230.0f, x);
    z = lerpf(z, 30.0f, smooth01(-600.0f, -900.0f, x));
    return z;
}

/* The city's own hills away from the course: Russian Hill under the
   summit, Telegraph Hill, Nob Hill to the south and Pacific Heights to
   the west, the land rising slowly inland; flat along the waterfront. */
static float sf_bump(float x, float z, float cx, float cz, float r, float h) {
    float d2 = ((x - cx) * (x - cx) + (z - cz) * (z - cz)) / (r * r);
    return d2 < 1.0f ? h * (1.0f - d2) * (1.0f - d2) : 0.0f;
}

static float sf_hills(float x, float z) {
    float y = 2.0f + 0.03f * fmaxf(0.0f, 40.0f - z);
    y += sf_bump(x, z, -215.0f, -210.0f, 190.0f, 30.0f);
    y += sf_bump(x, z, 50.0f, -110.0f, 80.0f, 14.0f);
    y += sf_bump(x, z, -40.0f, -420.0f, 260.0f, 36.0f);
    y += sf_bump(x, z, -540.0f, -220.0f, 280.0f, 34.0f);
    y += sf_bump(x, z, 300.0f, -520.0f, 260.0f, 22.0f);
    float sh = sf_shore(x);
    return lerpf(1.0f, y, smooth01(sh - 6.0f, sh - 60.0f, z));
}

/* The grid over the course, 10 m a cell, its north rows pulled back to
   the shore; beyond it (or out in the bay) sf_far. */
#define SF_GN 46
#define SF_GSTEP 10.0f
#define SF_GX0 (-305.0f)
#define SF_GZ0 (-265.0f)
static float sf_gy[SF_GN][SF_GN];
static bool sf_gy_ready;

static float sf_far(float x, float z) {
    if(z > sf_shore(x) + 1.0f) return SF_SEABED;
    return sf_hills(x, z);
}

float sf_terrain(float x, float z) {
    if(z > sf_shore(x) + 0.5f) return SF_SEABED;
    float fx = (x - SF_GX0) / SF_GSTEP, fz = (z - SF_GZ0) / SF_GSTEP;
    if(!sf_gy_ready || fx < 0 || fz < 0 || fx > SF_GN - 1 || fz > SF_GN - 1) return sf_far(x, z);
    int i = (int)fx, j = (int)fz;
    if(i > SF_GN - 2) i = SF_GN - 2;
    if(j > SF_GN - 2) j = SF_GN - 2;
    fx -= (float)i;
    fz -= (float)j;
    return lerpf(lerpf(sf_gy[j][i], sf_gy[j][i + 1], fx), lerpf(sf_gy[j + 1][i], sf_gy[j + 1][i + 1], fx), fz);
}

/* Telegraph Hill's knoll, the tower on its top. */
static float sf_knoll(float x, float z) {
    float d = sqrtf((x - SF_COIT_X) * (x - SF_COIT_X) + (z - SF_COIT_Z) * (z - SF_COIT_Z));
    return 25.0f - 27.0f * smooth01(8.0f, 60.0f, d);   /* well under everything beyond 50 m */
}

/* Near the road each node sits just under the lowest stretch within 12 m
   of the nearest (between two switchbacks, the lower one: the upper
   stands on its retaining wall), easing out over 12-45 m to an
   interpolation of the whole lap's heights, which the city's hills take
   over from further out. */
static void sf_ground_init(void) {
    sf_gy_ready = false;
    float d2[TRACK_SAMPLES], hs[TRACK_SAMPLES];
    for(int k = 0; k < TRACK_SAMPLES; k++) hs[k] = track_height(track.smp[k].s);
    for(int j = 0; j < SF_GN; j++) {
        for(int i = 0; i < SF_GN; i++) {
            float x = SF_GX0 + i * SF_GSTEP, z = SF_GZ0 + j * SF_GSTEP;
            float best = 1e18f, wsum = 0, hsum = 0;
            for(int k = 0; k < TRACK_SAMPLES; k++) {
                float dx = x - track.smp[k].p.x, dz = z - track.smp[k].p.z;
                d2[k] = dx * dx + dz * dz;
                best = fminf(best, d2[k]);
                float w = 1.0f / ((d2[k] + 400.0f) * (d2[k] + 400.0f));
                wsum += w;
                hsum += w * hs[k];
            }
            /* Wide enough that round a hairpin stepping down the hill the
               node takes the lower end's height. */
            float dmin = sqrtf(best), reach = (dmin + 12.0f) * (dmin + 12.0f), low = 1e9f;
            for(int k = 0; k < TRACK_SAMPLES; k++)
                if(d2[k] < reach) low = fminf(low, hs[k]);
            float idw = hsum / wsum - 0.5f;
            float out = lerpf(idw, sf_hills(x, z), smooth01(60.0f, 200.0f, dmin));
            /* The waterfront's flat strip before the hill. */
            out = lerpf(1.0f, out, smooth01(15.0f, -30.0f, z));
            float y = lerpf(low - 0.3f, out, smooth01(12.0f, 45.0f, dmin));
            /* The knoll's bank starts two grid cells out from the road, so
               no cell between a node at the road and one on the bank
               reaches over the railing. */
            y = fmaxf(y, fminf(sf_knoll(x, z), low - 0.3f + 0.9f * fmaxf(0.0f, dmin - 20.0f)));
            if(i == 0 || j == 0 || i == SF_GN - 1 || j == SF_GN - 1) y = sf_far(x, z);
            sf_gy[j][i] = y;
        }
    }
    sf_gy_ready = true;
}

static uint32_t sf_ground_color(float x, float z) {
    float t = 0.5f + 0.25f * sinf(x * 0.019f + 0.4f) + 0.25f * sinf(z * 0.023f + 1.3f);
    return color_lerp(argb(255, 214, 214, 210), argb(255, 250, 248, 242), t);
}

/* The city's street grid: blocks SF_BPX by SF_BPZ metres apart, streets
   (sidewalks and all) 2 * SF_STREET wide between them; the ground's
   sfstreet texture is one block of it a tile. */
#define SF_BPX    47.0f
#define SF_BPZ    65.0f
#define SF_BX0    (-440.0f)
#define SF_BZ0    (-400.0f)
#define SF_STREET 4.5f

/* Pioneer Park on Telegraph Hill: lawn over the knoll round Coit Tower,
   out to SF_PARK_R or to the backs of the houses along the road, with a
   paved circle at the tower's foot and a path round the hilltop. The
   ground under it is sunk out of sight, so the lawn needn't hug it. */
#define SF_PARK_N 24
#define SF_PARK_R 36.0f
static float sf_park_rim[SF_PARK_N];

static void sf_park_init(void) {
    for(int k = 0; k < SF_PARK_N; k++) {
        float a = (float)k * (2.0f * DK_PI / SF_PARK_N), r = 4.0f;
        while(r < SF_PARK_R) {
            float x = SF_COIT_X + sinf(a) * (r + 1.0f), z = SF_COIT_Z + cosf(a) * (r + 1.0f);
            if(sk_road_dist(v3(x, 0, z)) < 21.5f || z > sf_shore(x) - 2.0f) break;
            r += 1.0f;
        }
        sf_park_rim[k] = r;
    }
    /* No spikes between the spokes the road cuts short. */
    for(int pass = 0; pass < 3; pass++)
        for(int k = 0; k < SF_PARK_N; k++) {
            float m = 0.5f * (sf_park_rim[(k + SF_PARK_N - 1) % SF_PARK_N] + sf_park_rim[(k + 1) % SF_PARK_N]);
            sf_park_rim[k] = fminf(sf_park_rim[k], m + 1.5f);
        }
}

/* How far out the park reaches in the direction of (x, z), and (in *d)
   how far (x, z) is from the tower. */
static float sf_park_reach(float x, float z, float *d) {
    float dx = x - SF_COIT_X, dz = z - SF_COIT_Z;
    *d = sqrtf(dx * dx + dz * dz);
    float a = atan2f(dx, dz);
    if(a < 0) a += 2.0f * DK_PI;
    float f = a * (SF_PARK_N / (2.0f * DK_PI));
    int k = (int)f;
    f -= (float)k;
    return lerpf(sf_park_rim[k % SF_PARK_N], sf_park_rim[(k + 1) % SF_PARK_N], f);
}

/* A node of the ground grid, pulled back onto the shore if it is out in
   the bay, and (the full grid's, step 1) sunk under the park when every
   cell it is in lies inside it. */
/* Not inlined: inlined into sf_ground_row it trips a GCC 15 SH-4 ICE. */
__attribute__((noinline)) static V3 sf_gnode(int i, int j, int step) {
    float x = SF_GX0 + i * SF_GSTEP, z = SF_GZ0 + j * SF_GSTEP, sh = sf_shore(x);
    if(z > sh) return v3(x, 1.0f, sh);
    float d, reach = sf_park_reach(x, z, &d);
    if(step == 1 && d + 1.42f * SF_GSTEP < reach - 1.0f) return v3(x, sf_gy[j][i] - 4.0f, z);
    return v3(x, sf_gy[j][i], z);
}

/* Height of the ground as drawn at (x, z) by the full grid (step 1) or
   the split screen's (step 2), its nodes unsunk: the strips of
   sf_build_ground, each cell split from its (i0, j0) node to (i1, j1). */
static float sf_ground_tri(float x, float z, int step) {
    float fi = (x - SF_GX0) / SF_GSTEP, fj = (z - SF_GZ0) / SF_GSTEP;
    if(fi < 0 || fj < 0 || fi >= SF_GN - 1 || fj >= SF_GN - 1) return sf_terrain(x, z);
    int ci = 9 * (int)(fi / 9.0f), cj = 5 * (int)(fj / 5.0f);
    if(ci > SF_GN - 10) ci = SF_GN - 10;
    if(cj > SF_GN - 6) cj = SF_GN - 6;
    int ci1 = ci + 9 < SF_GN - 1 ? ci + 9 : SF_GN - 1, cj1 = cj + 5 < SF_GN - 1 ? cj + 5 : SF_GN - 1;
    int i0 = ci + step * (int)((fi - (float)ci) / (float)step), j0 = cj + step * (int)((fj - (float)cj) / (float)step);
    if(i0 >= ci1) i0 = ci1 - 1;
    if(j0 >= cj1) j0 = cj1 - 1;
    int i1 = i0 + step < ci1 ? i0 + step : ci1, j1 = j0 + step < cj1 ? j0 + step : cj1;
    float fx = (fi - (float)i0) / (float)(i1 - i0), fz = (fj - (float)j0) / (float)(j1 - j0);
    float a = sf_gy[j0][i0], b = sf_gy[j0][i1], c = sf_gy[j1][i0], dd = sf_gy[j1][i1];
    if(fz > fx) return a + fx * (dd - c) + fz * (c - a);
    return a + fx * (b - a) + fz * (dd - b);
}

static void sf_ground_row(int j0, int j1, int i0, int i1, int step, const pvr_poly_hdr_t *hdr) {
    strip_begin(hdr);
    for(int i = i0;; i += step) {
        if(i > i1) i = i1;
        for(int k = 1; k >= 0; k--) {
            V3 p = sf_gnode(i, k ? j1 : j0, step);
            svp(p, (p.x - SF_BX0) / SF_BPX, (p.z - SF_BZ0) / SF_BPZ, sf_ground_color(p.x, p.z));
        }
        if(i == i1) break;
    }
    strip_end();
}

/* The park's surface at (x, z): over the ground as either grid draws it (a
   split screen may pair the park with either), by enough that the grid's
   creases stay under the park's own between its spokes, and flat round
   the tower's foot. */
static float sf_park_y(float x, float z, float plaza_y, float lift) {
    float y = fmaxf(sf_terrain(x, z), fmaxf(sf_ground_tri(x, z, 1), sf_ground_tri(x, z, 2))) + lift;
    float d = sqrtf((x - SF_COIT_X) * (x - SF_COIT_X) + (z - SF_COIT_Z) * (z - SF_COIT_Z));
    return lerpf(plaza_y, y, smooth01(7.0f, 10.5f, d));
}

static void sf_park(void) {
    /* Rings out from the tower: the paved circle, lawn, the path, lawn
       to the edge; a split screen's coarse copy beyond SPLIT_NEAR (lod 2)
       has no path and twelve spokes, standing higher so the ground's
       creases stay under its longer chords. */
    static const float rr1[] = {0.0f, 7.0f, 10.5f, 18.0f, 20.2f, 27.0f, SF_PARK_R};
    static const float rr2[] = {0.0f, 7.0f, 10.5f, 18.0f, 27.0f, SF_PARK_R};
    float plaza_y = -1e9f;
    for(int k = 0; k < 12; k++)
        for(float r = 0.0f; r <= 7.5f; r += 2.5f) {
            float a = (float)k * (DK_PI / 6.0f), x = SF_COIT_X + sinf(a) * r, z = SF_COIT_Z + cosf(a) * r;
            plaza_y = fmaxf(plaza_y, fmaxf(sf_terrain(x, z), fmaxf(sf_ground_tri(x, z, 1), sf_ground_tri(x, z, 2))));
        }
    for(int lod = 1; lod <= 2; lod++) {
        const float *rr = lod == 1 ? rr1 : rr2, lift = lod == 1 ? 0.5f : 1.0f;
        const int nr = lod == 1 ? (int)(sizeof(rr1) / sizeof(rr1[0])) : (int)(sizeof(rr2) / sizeof(rr2[0]));
        const int step = lod == 1 ? 1 : SF_PARK_N / 12;
        chunk_begin(HF_OP, 1500.0f);
        if(cur_chunk) cur_chunk->lod = (uint8_t)lod;
        /* The last band is a skirt down into the ground round the edge. */
        for(int b = 0; b < nr; b++) {
            bool paved = b == 0 || (lod == 1 && b == 3), skirt = b == nr - 1;
            strip_begin(paved ? h_sfwalk : h_sfgrass);
            for(int k = 0; k <= SF_PARK_N; k += step) {
                float a = (float)k * (2.0f * DK_PI / SF_PARK_N), rim = sf_park_rim[k % SF_PARK_N];
                for(int q = 1; q >= 0; q--) {
                    float r = skirt ? rim + 0.4f * (float)q : fminf(rr[b + q], rim);
                    float x = SF_COIT_X + sinf(a) * r, z = SF_COIT_Z + cosf(a) * r;
                    float y = sf_park_y(x, z, plaza_y + lift, lift) - (skirt && q ? 2.5f : 0.0f);
                    /* Lit by the slope, the knoll's shape showing. */
                    float gx = sf_terrain(x + 2.0f, z) - sf_terrain(x - 2.0f, z);
                    float gz = sf_terrain(x, z + 2.0f) - sf_terrain(x, z - 2.0f);
                    V3 n = r < 9.0f ? v3(0, 1, 0) : vnorm(v3(-gx, 4.0f, -gz));
                    uint32_t c = shade(paved ? argb(255, 236, 232, 222) : skirt ? argb(255, 200, 200, 200) : 0xffffffff, n);
                    svp(v3(x, y, z), x / 6.0f, z / 6.0f, c);
                }
            }
            strip_end();
        }
        chunk_end();
    }
}

/* The ground grid's strips, and where it shows between the houses (a
   house left out near the road, by a cross street or round the park):
   those rows get the plain street grid, lawns where the houses would be. */
static int sf_ground_s0, sf_ground_s1;
#define SF_GAPS 2400
static float sf_gap[SF_GAPS][2];
static int sf_gap_n;

static void sf_ground_gaps(void) {
    int plain = 0;
    for(int i = sf_ground_s0; i < sf_ground_s1; i++) {
        Strip *st = &strips[i];
        float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for(int k = 0; k < st->count; k++) {
            const RV *v = &pool[st->first + k];
            x0 = fminf(x0, v->x);
            x1 = fmaxf(x1, v->x);
            z0 = fminf(z0, v->z);
            z1 = fmaxf(z1, v->z);
        }
        for(int g = 0; g < sf_gap_n; g++)
            if(sf_gap[g][0] > x0 - 4.0f && sf_gap[g][0] < x1 + 4.0f && sf_gap[g][1] > z0 - 4.0f && sf_gap[g][1] < z1 + 4.0f) {
                st->hdr = h_sfstreet;
                plain++;
                break;
            }
    }
    /* The painted rows first, then the plain: one change of texture a
       chunk, not one a row (the order doesn't matter, opaque). */
    for(int c = 0; c < chunk_n; c++) {
        Chunk *ck = &chunks[c];
        if(ck->first_strip < sf_ground_s0 || ck->first_strip >= sf_ground_s1) continue;
        for(int k = ck->first_strip + 1; k < ck->first_strip + ck->nstrips; k++) {
            Strip t = strips[k];
            int q = k;
            for(; q > ck->first_strip && strips[q - 1].hdr != t.hdr && t.hdr == h_sfroof; q--) strips[q] = strips[q - 1];
            strips[q] = t;
        }
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DreamKarts bay city: %d gaps between the houses, %d of %d ground rows plain\n", sf_gap_n, plain,
           sf_ground_s1 - sf_ground_s0);
#endif
}

static void sf_build_ground(void) {
    sf_ground_s0 = strip_n;
    for(int cz = 0; cz + 1 < SF_GN; cz += 5) {
        for(int cx = 0; cx + 1 < SF_GN; cx += 9) {
            int i1 = cx + 9 < SF_GN - 1 ? cx + 9 : SF_GN - 1, j1 = cz + 5 < SF_GN - 1 ? cz + 5 : SF_GN - 1;
            /* Rows wholly out in the bay are only the shore, many times. */
            if(SF_GZ0 + cz * SF_GSTEP > 95.0f) continue;
            /* The city's roofs painted on (under the blocks, or for them
               where they are out of sight); sf_ground_gaps makes a row
               plain where it shows between houses. */
            const pvr_poly_hdr_t *hdr = h_sfroof;
            chunk_begin(HF_OP, 2000.0f);
            if(cur_chunk) cur_chunk->lod = 1;
            for(int j = cz; j < j1; j++) sf_ground_row(j, j + 1, cx, i1, 1, hdr);
            chunk_end();
            chunk_begin(HF_OP, 2000.0f);
            if(cur_chunk) cur_chunk->lod = 2;
            for(int j = cz; j < j1; j += 2) sf_ground_row(j, j + 2 < j1 ? j + 2 : j1, cx, i1, 2, hdr);
            chunk_end();
        }
    }
    sf_ground_s1 = strip_n;
    /* Coarser rings round the grid out to ~600 m, the land and the bay's
       floor; then the far city to the south, east and west. */
    const float gx0 = SF_GX0, gx1 = SF_GX0 + (SF_GN - 1) * SF_GSTEP, gz0 = SF_GZ0, gz1 = SF_GZ0 + (SF_GN - 1) * SF_GSTEP;
    static const float rings[] = {0.0f, 40.0f, 100.0f, 180.0f, 280.0f, 400.0f};
    const int nr = (int)(sizeof(rings) / sizeof(rings[0]));
    for(int side = 0; side < 4; side++) {
        chunk_begin(HF_OP, 4000.0f);
        for(int b = 0; b + 1 < nr; b++) {
            strip_begin(h_sfroof);
            for(int k = 0; k <= 20; k++) {
                float t = (float)k / 20.0f;
                for(int q = 1; q >= 0; q--) {
                    float e = rings[b + q], x, z;
                    if(side == 0) { x = lerpf(gx0 - e, gx1 + e, t); z = gz1 + e; }
                    else if(side == 1) { x = lerpf(gx0 - e, gx1 + e, t); z = gz0 - e; }
                    else if(side == 2) { x = gx1 + e; z = lerpf(gz0 - e + 0.01f, gz1 + e - 0.01f, t); }
                    else { x = gx0 - e; z = lerpf(gz0 - e + 0.01f, gz1 + e - 0.01f, t); }
                    float y = side == 0 ? SF_SEABED : sf_far(x, z);
                    if(side != 0 && z > sf_shore(x)) {
                        /* Pull a node in the bay back onto the shore. */
                        z = fmaxf(sf_shore(x), side == 1 ? z : -1e9f);
                        y = 1.0f;
                    }
                    svp(v3(x, y, z), (x - SF_BX0) / SF_BPX, (z - SF_BZ0) / SF_BPZ, sf_ground_color(x, z));
                }
            }
            strip_end();
        }
        chunk_end();
    }
    chunk_begin(HF_OP, 6000.0f);
    {
        const float R = rings[nr - 1], E = 3000.0f, y = 18.0f;
        float x0 = gx0 - R, x1 = gx1 + R, z0 = gz0 - R;
#define GU(x) (((x) - SF_BX0) / SF_BPX)
#define GV(z) (((z) - SF_BZ0) / SF_BPZ)
        quad(h_sfroof, v3(-E, y, z0), v3(E, y, z0), v3(-E, y, -E), v3(E, y, -E), GU(-E), GV(z0), GU(E), GV(-E), 0xffffffff);
        quad(h_sfroof, v3(-E, y, -60.0f), v3(x0, 12.0f, -60.0f), v3(-E, y, z0), v3(x0, y, z0), GU(-E), GV(-60.0f), GU(x0),
             GV(z0), 0xffffffff);
        quad(h_sfroof, v3(x1, 12.0f, -60.0f), v3(E, y, -60.0f), v3(x1, y, z0), v3(E, y, z0), GU(x1), GV(-60.0f), GU(E),
             GV(z0), 0xffffffff);
#undef GU
#undef GV
    }
    chunk_end();
}

/* The quay along the waterfront: a concrete wall down into the bay. */
static void sf_quay(void) {
    for(float x0 = SF_GX0 - 400.0f; x0 < SF_GX0 + (SF_GN - 1) * SF_GSTEP + 400.0f; x0 += 200.0f) {
        chunk_begin(HF_OP, 2500.0f);
        strip_begin(h_sfcurb);
        for(int k = 0; k <= 20; k++) {
            float x = x0 + 10.0f * (float)k, z = sf_shore(x);
            float top = 1.0f;
            uint32_t c = argb(255, 210, 206, 196);
            svp(v3(x, top, z + 0.02f), x / 2.4f, 0.0f, c);
            svp(v3(x, SF_SEABED, z + 0.02f), x / 2.4f, 2.0f, color_scale(c, 0.6f));
        }
        strip_end();
        chunk_end();
    }
}

/* The promenade along the bay: paving from the quay's edge in to the
   road's railing wherever land lies between them (the ground there
   would show the street grid's lawns). */
static float sf_prom_inner(float x, float zs) {
    if(sk_road_dist(v3(x, 0, zs - 0.3f)) < 10.4f) return zs;
    float lo = zs - 0.3f, hi = zs - 40.0f;
    for(float z = zs - 2.0f; z > zs - 40.0f; z -= 2.0f)
        if(sk_road_dist(v3(x, 0, z)) < 10.4f) {
            hi = z;
            break;
        }
        else lo = z;
    for(int k = 0; k < 3; k++) {
        float m = 0.5f * (lo + hi);
        if(sk_road_dist(v3(x, 0, m)) < 10.4f) hi = m;
        else lo = m;
    }
    return lo;
}

static void sf_promenade(void) {
    for(float x0 = -320.0f; x0 < 230.0f; x0 += 110.0f) {
        chunk_begin(HF_OP, 900.0f);
        strip_begin(h_sfwalk);
        bool open = false;
        for(float x = x0; x <= x0 + 110.01f; x += 4.0f) {
            float zs = sf_shore(x), zi = sf_prom_inner(x, zs);
            if(zs - zi < 1.0f) {
                if(open) {
                    strip_end();
                    strip_begin(h_sfwalk);
                }
                open = false;
                continue;
            }
            uint32_t c = shade(argb(255, 232, 228, 218), v3(0, 1, 0));
            for(int q = 0; q < 2; q++) {
                float z = q ? zs + 0.02f : zi;
                float y = fmaxf(1.0f, fmaxf(sf_terrain(x, z), fmaxf(sf_ground_tri(x, z, 1), sf_ground_tri(x, z, 2)))) + 0.06f;
                svp(v3(x, y, z), x / 6.0f, z / 6.0f, c);
            }
            open = true;
        }
        strip_end();
        chunk_end();
    }
}

/* ---- The road ---- */

/* Which layout segment lap distance s is in. */
static int sf_seg_at(float s) {
    int best = 0;
    float bd = 1e9f;
    for(int i = 0; i < track_seg_count(); i++) {
        float d = track_wrap(s - track_seg_s(i));
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static bool sf_on_pier(int seg) {
    return seg >= SF_PIER_SEG && seg <= SF_PIER_END;
}

static bool sf_in_crook(int seg) {
    return seg >= SF_CROOK_SEG && seg <= SF_CROOK_END;
}

/* Each chunk's road in its district's materials: the pier's planks and
   timber rail, Hyde Street's cable car tracks, the crooked street's
   bricks between flowerbeds, and the city's asphalt and sidewalks. */
static int sf_chunk_seg(int c) {
    return sf_seg_at(((float)(c * 8) + 4.0f) * track.ds);
}

/* The kerb: a thin pale line of concrete where the asphalt meets the
   sidewalk (the sidewalks start at the road's edge, over the curb strip
   the karts still feel: a 1.3 m strip of concrete there read as a second
   sidewalk, its joints fanning round the bends). */
static void sf_kerbs(const float *sv, int n) {
    const float hw = SF_HALF;
    f8_ribbon(sv, n, h_col, -(hw + 0.16f), -(hw - 0.02f), 0.015f, 0.015f, 0, 1, 1, true, argb(255, 196, 194, 188),
              argb(255, 226, 224, 218));
    f8_ribbon(sv, n, h_col, hw - 0.02f, hw + 0.16f, 0.015f, 0.015f, 0, 1, 1, true, argb(255, 226, 224, 218),
              argb(255, 196, 194, 188));
}

static void sf_road_style(int c) {
    int seg = sf_chunk_seg(c);
    f8m.road = h_sfroad;
    f8m.curb = h_sfcurb;
    f8m.shoulder = h_sfwalk;
    f8m.barrier = h_sfrailing;
    f8m.skirt = h_sfcurb;
    f8m.cap = argb(255, 40, 78, 58);
    f8m.wall_h = 1.0f;
    f8m.extra = sf_kerbs;   /* and the houses stand on the ground beyond the railings */
    f8m.verge = h_sfwalk;
    if(sf_on_pier(seg)) {
        f8m.road = f8m.curb = f8m.shoulder = h_sfpier;
        f8m.barrier = f8m.skirt = h_sfpierrail;
        f8m.cap = argb(255, 236, 234, 226);
        f8m.wall_h = SF_PIER_RAIL_H;   /* karts stop at it all the same */
        f8m.extra = NULL;
    }
    else if(seg == SF_HYDE_SEG) f8m.road = h_sfrails;
    else if(sf_in_crook(seg)) {
        f8m.road = h_sfbrick;
        f8m.shoulder = h_sfhedge;
    }
}

/* The cable car tracks over the whole of the car's run (cablecar.c),
   from just past turn 1 to where it waits at the top: the chunks given
   the rails texture end where Hyde Street's do, so the rest is laid over
   the asphalt as a band of the same texture, the road's u across it, on
   the road mesh's own stations so it lies flat on it. */
static void sf_hyde_rails(void) {
    float s0 = track_seg_s(SF_HYDE_SEG) + 2.0f, s1 = track_seg_s(SF_HYDE_SEG) + 213.0f;
    const float w = 2.85f;   /* both trackways, out to their concrete's edge */
    for(int c = 0; c < TRACK_SAMPLES / 8; c++) {
        if(sf_chunk_seg(c) == SF_HYDE_SEG) continue;
        float sv[MAX_SVALS];
        int n = f8_chunk_svals(c, sv);
        if(sv[n - 1] <= s0 || sv[0] >= s1) continue;
        chunk_begin(HF_OP, 2000.0f);
        strip_begin(h_sfrails);
        for(int k = 0; k < n; k++) {
            float s = clampf(sv[k], s0, s1);
            if(k > 0 && s == clampf(sv[k - 1], s0, s1)) continue;
            svp(f8_pt(s, -w, 0.03f), (6.5f - w) / 13.0f, s / ROAD_V_M, 0xfffafafa);
            svp(f8_pt(s, w, 0.03f), (6.5f + w) / 13.0f, s / ROAD_V_M, 0xfffafafa);
        }
        strip_end();
        chunk_end();
    }
}

/* The start line: two rows of chequers across the road. */
static void sf_start_line(void) {
    chunk_begin(HF_OP, 600.0f);
    strip_begin(h_col);
    const int across = 10;
    for(int row = 0; row < 2; row++) {
        for(int i = 0; i < across; i++) {
            float d0 = -SF_HALF + 2.0f * SF_HALF * (float)i / across, d1 = d0 + 2.0f * SF_HALF / across;
            float s0 = row ? 0.0f : -1.3f, s1 = s0 + 1.3f;
            uint32_t c = ((row + i) & 1) ? argb(255, 236, 236, 232) : argb(255, 26, 26, 30);
            V3 a = f8_pt(s0, d0, 0.025f), b = f8_pt(s0, d1, 0.025f), cc = f8_pt(s1, d0, 0.025f), dd = f8_pt(s1, d1, 0.025f);
            svj(a, 0, 0, c);
            svp(b, 0, 0, c);
            svp(cc, 0, 0, c);
            svp(dd, 0, 0, c);
        }
    }
    strip_end();
    chunk_end();
}

/* ---- Buildings ---- */

/* Clear of every stretch of road by margin metres past its barriers, and
   on dry land. */
static bool sf_clear(float x, float z, float margin) {
    if(z > sf_shore(x) - 1.0f) return false;
    return sk_road_dist(v3(x, 0, z)) >= 10.0f + margin;
}

/* One house's front (left and right corners at street level, the left
   as seen from the street), how deep and tall it is, its colours. */
typedef struct {
    V3 l, r, n;          /* front corners, outward normal (away from the street) */
    float street, base;  /* street level at the facade, the foot of its plinth */
    float h, d;
    int kind;            /* 0-7 a Victorian (its facade), 8 a pier shed */
    int sign;            /* a shed's pier number (sfsigns' row) */
    uint32_t tint;
} House;

/* Hyde Street's two flat crossings are intersections: a gap in the
   houses each side and a short cross street running off, level from the
   railing then following the ground; the upper crossing's east street
   has a cable car line, a car waiting on it to cross. */
#define SF_CROSS_HALF 8.0f   /* half the gap along Hyde Street: the street and its sidewalks */
#define SF_CROSS_LEN  50.0f  /* how far the cross streets run from Hyde Street's centreline */
static float sf_cross_s[2];

/* Whether lap distance s is at a crossing, within pad metres of it. */
static bool sf_at_cross(float s, float pad) {
    for(int k = 0; k < 2; k++)
        if(fabsf(track_delta(sf_cross_s[k], s)) < SF_CROSS_HALF + pad) return true;
    return false;
}

/* Whether (x, z) is within m metres of a cross street's centreline. */
static bool sf_near_cross(float x, float z, float m) {
    for(int k = 0; k < 2; k++) {
        V3 a = track_point(sf_cross_s[k], -SF_CROSS_LEN), b = track_point(sf_cross_s[k], SF_CROSS_LEN);
        if(seg_dist(v3(x, 0, z), a, b) < m) return true;
    }
    return false;
}

/* Every building as a solid for the spectator's cameras: a footprint
   (corner, two edges) and its height range. */
typedef struct {
    float x, z, ax, az, bx, bz, y0, y1;
} SfSolid;
#define SF_SOLIDS 1000
static SfSolid sf_solid[SF_SOLIDS];
static int sf_solid_n;

static void sf_add_solid(V3 o, V3 a, V3 b, float y0, float y1) {
    if(sf_solid_n >= SF_SOLIDS) return;
    sf_solid[sf_solid_n++] = (SfSolid){o.x, o.z, a.x, a.z, b.x, b.z, y0, y1};
}

bool sf_in_solid(V3 p, float margin) {
    for(int i = 0; i < sf_solid_n; i++) {
        const SfSolid *q = &sf_solid[i];
        if(p.y < q->y0 - margin || p.y > q->y1 + margin) continue;
        float dx = p.x - q->x, dz = p.z - q->z;
        float la = q->ax * q->ax + q->az * q->az, lb = q->bx * q->bx + q->bz * q->bz;
        float ta = (dx * q->ax + dz * q->az) / la, tb = (dx * q->bx + dz * q->bz) / lb;
        float ma = margin / sqrtf(la), mb = margin / sqrtf(lb);
        if(ta > -ma && ta < 1.0f + ma && tb > -mb && tb < 1.0f + mb) return true;
    }
    return false;
}

#define SF_HOUSE_MAX 24
static House sf_row[SF_HOUSE_MAX];
static int sf_row_n, sf_houses_n, sf_sheds_n;

/* House kinds: 0-7 the Victorians (sffacade's cells), SF_SHED a pier
   shed, SF_TOWN on North Beach's shops (four, with awnings) and flats
   (sffacade2's cells). */
#define SF_SHED 8
#define SF_TOWN 9
static const uint32_t sf_body2[8] = {
    0xffe8dcc4, 0xffa8483a, 0xffeed696, 0xff96aa8c, 0xffb0bcca, 0xffeceae2, 0xff964634, 0xffdec49c,
};

static const uint32_t sf_body[8] = {
    0xff80acd2, 0xfff0d678, 0xffec9eb0, 0xff98c6a0, 0xffbca2d8, 0xfff2e8d4, 0xfff2ac7a, 0xff80bec0,
};

static V3 sf_up(V3 p, float dy) {
    return v3(p.x, p.y + dy, p.z);
}

/* Emit a quad into the open strip: a-b along the top, c-d along the
   bottom. */
static void sf_quadj(V3 a, V3 b, V3 c, V3 d, float u0, float v0, float u1, float v1, uint32_t col) {
    svj(a, u0, v0, col);
    svp(b, u1, v0, col);
    svp(c, u0, v1, col);
    svp(d, u1, v1, col);
}

/* Whether house i's left (side -1) or right side is against the next
   house in the row, which stands at least as tall there. Houses are in
   the row in the order built, along the street; which way that runs
   from left to right depends on the street's side, so check both
   neighbours' near corners. */
static bool sf_hidden_side(int i, int side) {
    const House *h = &sf_row[i];
    V3 p = side < 0 ? h->l : h->r;
    float top = h->street + h->h;
    for(int k = i - 1; k <= i + 1; k += 2) {
        if(k < 0 || k >= sf_row_n) continue;
        const House *o = &sf_row[k];
        V3 q = side < 0 ? o->r : o->l;
        float dx = q.x - p.x, dz = q.z - p.z;
        if(dx * dx + dz * dz < 0.6f * 0.6f && o->street + o->h >= top - 0.1f) return true;
    }
    return false;
}

/* The houses' backs, only drawn in the track select's flyover, which
   looks down on them: from the road they barely show, and sent in the
   fronts' strips (transformed whenever a front is in view) they cost the
   race ~0.2 ms a frame on the console. */
typedef struct {
    const pvr_poly_hdr_t *hdr;
    V3 a, b, c, d;
    float u0, v0, u1, v1;
    uint32_t col;
} SfBack;
static SfBack sf_back[SF_HOUSE_MAX * 2];
static int sf_back_n;

static void sf_back_add(const pvr_poly_hdr_t *hdr, V3 a, V3 b, V3 c, V3 d, float u0, float v0, float u1, float v1,
                        uint32_t col) {
    if(sf_back_n < SF_HOUSE_MAX * 2) sf_back[sf_back_n++] = (SfBack){hdr, a, b, c, d, u0, v0, u1, v1, col};
}

/* The houses in sf_row, as two strips: the textured fronts (a facade, its
   bay window standing out from it), then everything in flat colours
   (plinth, cornice, sides, roof). */
static void sf_flush_row(void) {
    if(sf_row_n == 0) return;
    chunk_begin(HF_OP, 320.0f);
    /* Passes by texture: the Victorians, the sheds, flat colours, and
       North Beach's shops and flats. */
    for(int pass = 0; pass < 5; pass++) {
        static const int8_t first[5] = {0, SF_SHED, 0, SF_TOWN, SF_SHED}, last[5] = {SF_SHED - 1, SF_SHED, 127, SF_TOWN + 7, SF_SHED};
        const pvr_poly_hdr_t *hdr = pass == 0   ? h_sffacade
                                    : pass == 1 ? h_sfshed
                                    : pass == 2 ? h_col
                                    : pass == 3 ? h_sffacade2
                                                : h_sfsigns;
        bool any = false;
        for(int i = 0; i < sf_row_n; i++) any |= sf_row[i].kind >= first[pass] && sf_row[i].kind <= last[pass];
        if(!any) continue;
        strip_begin(hdr);
        for(int i = 0; i < sf_row_n; i++) {
            const House *h = &sf_row[i];
            V3 l = sf_up(h->l, h->street - h->l.y), r = sf_up(h->r, h->street - h->r.y);
            V3 lt = sf_up(l, h->h), rt = sf_up(r, h->h);
            V3 n = h->n, along = vsub(r, l);
            float w = sqrtf(along.x * along.x + along.z * along.z);
            V3 t = vscale(along, 1.0f / w);
            uint32_t lit = shade(0xffffffff, vscale(n, -1.0f));
            bool vic = h->kind < SF_SHED, town = h->kind >= SF_TOWN, faced = vic || town;
            if((pass == 0 && vic) || (pass == 3 && town)) {
                int cell = town ? h->kind - SF_TOWN : h->kind;
                float u0 = (float)(cell & 3) * 0.25f, u1 = u0 + 0.25f, v0 = (float)(cell >> 2) * 0.5f, v1 = v0 + 0.5f;
                sf_quadj(lt, rt, l, r, u0 + 0.002f, v0 + 0.002f, u1 - 0.002f, v1 - 0.002f, lit);
                if(town && cell < 4) {
                    /* A shop's awning, 1.3 m out over the sidewalk, its
                       stripes the valance's painted on the front. */
                    float hk = h->h / 12.0f, vv0 = v1 - 0.5f * 3.35f / 12.0f, vv1 = v1 - 0.5f * 2.95f / 12.0f;
                    V3 out = vscale(n, -1.3f);
                    V3 a0 = sf_up(l, 3.6f * hk), a1 = sf_up(r, 3.6f * hk);
                    V3 b0 = vadd(sf_up(l, 3.0f * hk), out), b1 = vadd(sf_up(r, 3.0f * hk), out);
                    sf_quadj(a0, a1, b0, b1, u0 + 0.002f, vv0, u1 - 0.002f, vv1, shade(0xffffffff, vadd(vscale(n, -0.6f), v3(0, 1, 0))));
                    sf_quadj(b0, b1, vadd(b0, v3(0, -0.35f, 0)), vadd(b1, v3(0, -0.35f, 0)), u0 + 0.002f, vv0, u1 - 0.002f, vv1, lit);
                }
            }
            if(pass == 0 && vic) {
                float u0 = (float)(h->kind & 3) * 0.25f, v1 = (float)(h->kind >> 2) * 0.5f + 0.5f;
                /* The bay: its front panel 0.7 m out over the left half's
                   windows, the slanted sides back to the wall. */
                float k = w / 7.0f, hk = h->h / 12.0f;
                float bx0 = 0.45f * k, bx1 = 3.55f * k, by0 = 3.2f * hk, by1 = 9.75f * hk;
                V3 out = vscale(n, -0.7f);
                V3 p0 = vmad(l, t, bx0 + 0.45f * k), p1 = vmad(l, t, bx1 - 0.45f * k);
                V3 q0 = vadd(p0, out), q1 = vadd(p1, out);
                float ua = u0 + 0.25f * (bx0 / w), ub = u0 + 0.25f * (bx1 / w);
                float ua2 = u0 + 0.25f * ((bx0 + 0.45f * k) / w), ub2 = u0 + 0.25f * ((bx1 - 0.45f * k) / w);
                float va = v1 - 0.5f * (by1 / h->h), vb = v1 - 0.5f * (by0 / h->h);
                uint32_t front = shade(0xffffffff, vscale(n, -1.0f));
                sf_quadj(sf_up(q0, by1), sf_up(q1, by1), sf_up(q0, by0), sf_up(q1, by0), ua2, va, ub2, vb, front);
                V3 e0 = vmad(l, t, bx0), e1 = vmad(l, t, bx1);
                uint32_t side_l = shade(0xffe8e8e8, vsub(vscale(t, -1.0f), n)), side_r = shade(0xffe8e8e8, vsub(t, n));
                sf_quadj(sf_up(e0, by1), sf_up(q0, by1), sf_up(e0, by0), sf_up(q0, by0), ua, va, ua2, vb, side_l);
                sf_quadj(sf_up(q1, by1), sf_up(e1, by1), sf_up(q1, by0), sf_up(e1, by0), ub2, va, ub, vb, side_r);
            }
            if((pass == 0 && vic) || (pass == 3 && town)) {
                /* A side left showing (at a corner, or above a lower
                   neighbour on a hill) gets a front of its own: a
                   Victorian's its own house front, a shop's the flats
                   over the next shop along. */
                int cell = town ? h->kind - SF_TOWN : h->kind;
                if(town && cell < 4) cell = 4 + (cell & 3);
                float u0 = (float)(cell & 3) * 0.25f, u1 = u0 + 0.25f, v0 = (float)(cell >> 2) * 0.5f, v1 = v0 + 0.5f;
                V3 bl = vadd(l, vscale(n, h->d)), br = vadd(r, vscale(n, h->d));
                if(!sf_hidden_side(i, -1))
                    sf_quadj(sf_up(bl, h->h), lt, bl, l, u0 + 0.002f, v0 + 0.002f, u1 - 0.002f, v1 - 0.002f,
                             shade(0xffffffff, vscale(t, -1.0f)));
                if(!sf_hidden_side(i, 1))
                    sf_quadj(rt, sf_up(br, h->h), r, br, u0 + 0.002f, v0 + 0.002f, u1 - 0.002f, v1 - 0.002f,
                             shade(0xffffffff, t));
                /* The back, where the ground falls away behind: its own
                   front again. */
                V3 bmid = vlerp(bl, br, 0.5f);
                if(sf_terrain(bmid.x, bmid.z) < h->street - 0.5f)
                    sf_back_add(town ? h_sffacade2 : h_sffacade, sf_up(br, h->h), sf_up(bl, h->h), br, bl, u0 + 0.002f,
                                v0 + 0.002f, u1 - 0.002f, v1 - 0.002f, shade(0xffffffff, n));
            }
            else if(pass == 1 && h->kind == SF_SHED) {
                float reps = fmaxf(1.0f, floorf(w / 8.0f + 0.5f));
                sf_quadj(lt, rt, l, r, 0.0f, 0.0f, reps, 1.0f, color_scale(h->tint, (float)((lit >> 8) & 255) / 255.0f));
                /* The sides too, where they show: the warehouse's bays
                   run down its flank (they were blank white walls). */
                V3 bl = vadd(l, vscale(n, h->d)), br = vadd(r, vscale(n, h->d));
                float sreps = fmaxf(1.0f, floorf(h->d / 8.0f + 0.5f));
                uint32_t sl = shade(h->tint, vscale(t, -1.0f)), sr = shade(h->tint, t);
                if(!sf_hidden_side(i, -1)) sf_quadj(sf_up(bl, h->h), lt, bl, l, 0.0f, 0.0f, sreps, 1.0f, sl);
                if(!sf_hidden_side(i, 1)) sf_quadj(rt, sf_up(br, h->h), r, br, 0.0f, 0.0f, sreps, 1.0f, sr);
            }
            else if(pass == 4 && h->kind == SF_SHED) {
                /* Its pier number on the sign band over the doors. */
                float sw = fminf(5.2f, w - 1.5f), hk = h->h / 8.0f;
                if(sw < 2.0f) continue;
                V3 mid = vmad(vlerp(l, r, 0.5f), n, -0.04f);
                V3 sa = vmad(mid, t, -0.5f * sw), sb = vmad(mid, t, 0.5f * sw);
                float v0 = 0.125f * (float)(h->sign % 6) + 0.004f, v1 = v0 + 0.117f;
                sf_quadj(sf_up(sa, h->h - 1.45f * hk), sf_up(sb, h->h - 1.45f * hk), sf_up(sa, h->h - 2.25f * hk),
                         sf_up(sb, h->h - 2.25f * hk), 0.004f, v0, 0.996f, v1, lit);
            }
            else if(pass == 2) {
                uint32_t body = vic ? sf_body[h->kind] : town ? sf_body2[h->kind - SF_TOWN] : h->tint;
                V3 lb = v3(l.x, h->base, l.z), rb = v3(r.x, h->base, r.z);
                V3 bl = vadd(l, vscale(n, h->d)), br = vadd(r, vscale(n, h->d));
                V3 blb = v3(bl.x, h->base, bl.z), brb = v3(br.x, h->base, br.z);
                V3 blt = sf_up(bl, h->h), brt = sf_up(br, h->h);
                /* The plinth below the street and the sides (the backs
                   face away from every street). */
                uint32_t dim = color_scale(body, 0.75f);
                /* Below the railing's top there is nothing to see; a side
                   against a neighbour at least as tall is hidden too. */
                if(h->street - fminf(h->l.y, h->r.y) > 0.8f) sf_quadj(l, r, lb, rb, 0, 0, 1, 1, shade(dim, vscale(n, -1.0f)));
                /* A Victorian's sides above the street are in its facade
                   pass; here only their plinths. */
                bool shed = h->kind == SF_SHED;
                V3 ls = faced || shed ? l : lt, bls = faced || shed ? bl : blt, rs = faced || shed ? r : rt,
                   brs = faced || shed ? br : brt;
                if(!sf_hidden_side(i, -1)) sf_quadj(ls, bls, lb, blb, 0, 0, 1, 1, shade(body, vscale(t, -1.0f)));
                if(!sf_hidden_side(i, 1)) sf_quadj(brs, rs, brb, rb, 0, 0, 1, 1, shade(body, t));
                /* The back: below the street on a hill (a faced house's
                   back above it is in its facade pass), all of a shed's. */
                V3 bm = vlerp(bl, br, 0.5f);
                if(!faced) sf_back_add(h_col, brt, blt, brb, blb, 0, 0, 1, 1, shade(body, n));
                else if(h->street - sf_terrain(bm.x, bm.z) > 0.3f) sf_back_add(h_col, br, bl, brb, blb, 0, 0, 1, 1, shade(dim, n));
                /* A flat roof behind a parapet: tar, gravel, the odd
                   pale or terracotta one. */
                static const uint32_t roofs[6] = {0xff7c7a76, 0xff96928a, 0xffaeaa9e, 0xff8a8682, 0xffa69888, 0xffc4c0b4};
                uint32_t rh = (uint32_t)(int)(h->l.x * 3.0f + 4000.0f) * 2654435761u ^ (uint32_t)(int)(h->l.z * 7.0f + 4000.0f);
                uint32_t roof = faced ? roofs[(rh >> 9) % 6u] : argb(255, 150, 146, 140);
                sf_quadj(lt, rt, blt, brt, 0, 0, 1, 1, shade(roof, v3(0, 1, 0)));
                if(vic) {
                    /* The cornice: a white lip along the top. */
                    V3 o = vscale(n, -0.45f);
                    V3 c0 = vadd(sf_up(l, h->h - 1.9f * h->h / 12.0f), o), c1 = vadd(sf_up(r, h->h - 1.9f * h->h / 12.0f), o);
                    V3 c2 = vadd(sf_up(l, h->h - 1.2f * h->h / 12.0f), o), c3 = vadd(sf_up(r, h->h - 1.2f * h->h / 12.0f), o);
                    uint32_t trim = argb(255, 246, 244, 236);
                    sf_quadj(c2, c3, c0, c1, 0, 0, 1, 1, shade(trim, vscale(n, -1.0f)));
                }
            }
        }
        strip_end();
    }
    chunk_end();
    if(sf_back_n) {
        chunk_begin(HF_OP, 400.0f);
        if(cur_chunk) cur_chunk->lod = 4;
        const pvr_poly_hdr_t *hdrs[3] = {h_sffacade, h_sffacade2, h_col};
        for(int k = 0; k < 3; k++) {
            strip_begin(hdrs[k]);
            for(int i = 0; i < sf_back_n; i++) {
                const SfBack *q = &sf_back[i];
                if(q->hdr == hdrs[k]) sf_quadj(q->a, q->b, q->c, q->d, q->u0, q->v0, q->u1, q->v1, q->col);
            }
            strip_end();
        }
        chunk_end();
        sf_back_n = 0;
    }
    sf_row_n = 0;
}

/* Row houses along one side of the road, front to the railing: each a
   house's width along the street, kept if its footprint is clear of
   every other stretch and the bay; the waterfront gets pier sheds. */
static void sf_street(int side) {
    float s = 0;
    uint32_t seed = side > 0 ? 0x5f5f1u : 0xbadc0deu;
    int last_kind = -1;
    while(s < track.length) {
        seed = seed * 1664525u + 1013904223u;
        int seg = sf_seg_at(s);
        bool water = seg <= 11 || seg >= 31;   /* the waterfront: sheds on the land side, nothing on the bay's */
        float w = water ? 9.0f + (float)((seed >> 9) & 3) : 6.4f + (float)((seed >> 9) & 7) * 0.18f;
        float front = 10.6f;
        /* Where the road bends toward this side, step along further. */
        float curv = track_curvature(s) * (float)side;
        float k = curv > 0 ? 1.0f - fminf(front * curv, 0.9f) : 1.0f + fminf(-curv * front, 1.5f);
        float ds = w / k;
        float sa = s, sb = s + ds;
        s += ds + (water ? 1.5f : 0.05f);
        if(sb > track.length) break;   /* past the line it would stand in the first one */
        if(seg == SF_HYDE_SEG && (sf_at_cross(sa, 0.0f) || sf_at_cross(sb, 0.0f) || sf_at_cross((sa + sb) * 0.5f, 0.0f)))
            continue;
        if(sf_on_pier(seg) || sf_in_crook(seg) || fabsf(track_curvature(sa + ds * 0.5f)) > 1.0f / 22.0f) continue;
        /* Left and right as seen from the street. */
        V3 l = track_point(sa, side * front), r = track_point(sb, side * front);
        if(side > 0) {
            V3 t = l;
            l = r;
            r = t;
        }
        V3 along = vsub(r, l);
        float len = sqrtf(along.x * along.x + along.z * along.z);
        if(len < 4.0f) continue;
        V3 n = vnorm(v3(-along.z, 0, along.x));   /* away from the road */
        V3 mid = vmad(vlerp(l, r, 0.5f), n, 0.1f);
        if(vdot(vsub(mid, track_point((sa + sb) * 0.5f, 0)), n) < 0) n = vscale(n, -1.0f);
        float depth = water ? 16.0f : 11.0f;
        V3 bl = vmad(l, n, depth), br = vmad(r, n, depth), bm = vmad(mid, n, depth * 0.5f);
        if(!sf_clear(l.x, l.z, 0.2f) || !sf_clear(r.x, r.z, 0.2f) || !sf_clear(bl.x, bl.z, 0.4f) ||
           !sf_clear(br.x, br.z, 0.4f) || !sf_clear(bm.x, bm.z, 0.4f))
            continue;
        if(water && side > 0) continue;
        House *h = &sf_row[sf_row_n];
        h->l = l;
        h->r = r;
        h->n = n;
        float ya = track_road_y(sa, 0), yb = track_road_y(sb, 0);
        /* Faced houses stand clear of the 1 m railing in front, so their
           shopfronts and doors show over it (half hidden behind it on a
           slope, they looked sunk). */
        h->street = fmaxf(ya, yb) + (water ? -0.2f : 0.6f);
        float g = fminf(fminf(sf_terrain(bl.x, bl.z), sf_terrain(br.x, br.z)), fminf(ya, yb));
        h->base = g - 1.5f;
        h->d = depth;
        /* From the crooked street's foot to the waterfront, North Beach:
           mostly shops and plainer flats, the odd Victorian among them. */
        bool beach = seg > SF_CROOK_END && seg <= SF_PLUNGE_SEG;
        h->kind = water ? SF_SHED : beach && ((seed >> 20) & 3u) ? SF_TOWN + (int)((seed >> 16) % 8u) : (int)((seed >> 16) % 8u);
        /* Never the same front twice in a row (two bakeries side by side). */
        if(h->kind == last_kind && !water) h->kind = h->kind >= SF_TOWN ? SF_TOWN + (h->kind - SF_TOWN + 1) % 8 : (h->kind + 1) % 8;
        h->h = water ? 9.5f + (float)((seed >> 4) & 3) * 0.6f : 10.2f + (float)((seed >> 5) & 7) * 0.25f;
        static const uint32_t shed[4] = {0xfff0e6d6, 0xffd8e4e8, 0xffe6d2c0, 0xffdce6d0};
        h->tint = shed[(seed >> 12) & 3];
        /* The back of a house cut into the hill: no higher than the
           ground behind it plus its own height. */
        if(sf_terrain(bm.x, bm.z) > h->street + h->h - 1.0f) continue;
        last_kind = h->kind;
        h->sign = sf_sheds_n;
        if(water) sf_sheds_n++;
        else sf_houses_n++;
        sf_add_solid(l, vsub(r, l), vscale(n, depth), h->base, h->street + h->h);
        if(++sf_row_n == 6) sf_flush_row();
    }
    sf_flush_row();
}

/* The city behind the streets: the street grid's blocks, each a ring of
   row houses round its backyards (the ground's lawn and trees show
   through the middle). Going round, the houses are one facade cell each,
   in runs of three to five under one flat roof; each run stands on its
   highest ground, so the roofline steps down the hills. A house too near
   the road, a street house, the park, a cross street or the bay is left
   out, the run's end walled off. Far detail, so a split screen leaves
   them out. */
#define SF_LOT_D   10.0f   /* how deep the houses are */
#define SF_RING    40      /* most houses round a block */

#define SF_WHITE_UV 0.006f   /* the facade atlases' near-white top left corner */

typedef struct {
    V3 o0, o1, i0, i1;     /* outer (street) and inner (yard) corners */
    int run;
    bool ok, corner;       /* kept; starts a side of the block */
} SfLot;

typedef struct {
    float top, h, obot, ibot, v0;
    uint32_t roof;
} SfRun;

/* How far (x, z) is from the crooked street; within SF_LADIES_R of it
   the painted ladies stand instead of the street grid's blocks. */
#define SF_LADIES_R 55.0f
static float sf_crook_dist(float x, float z) {
    float s0 = track_seg_s(SF_CROOK_SEG), s1 = track_seg_s(SF_CROOK_END + 1), best = 1e9f;
    for(float s = s0; s <= s1; s += 4.0f) {
        V3 p = track_point(s, 0.0f);
        best = fminf(best, (p.x - x) * (p.x - x) + (p.z - z) * (p.z - z));
    }
    return sqrtf(best);
}

/* Whether a house may stand over (x, z); if not, and nothing else covers
   the ground there (the road and its sidewalks, a street house, the park,
   the bay), it goes down as a gap for sf_ground_gaps. */
static bool sf_lot_ok(float x, float z, bool near_road, bool note) {
    if(z > sf_shore(x) - 4.0f) return false;
    bool ok = true, covered = false;
    if(near_road) {
        float rd = sk_road_dist(v3(x, 0, z));
        ok = rd >= 14.0f;
        covered = rd < 10.5f;
    }
    float d, reach = sf_park_reach(x, z, &d);
    if(d < reach + 3.0f) {
        ok = false;
        covered |= d < reach;
    }
    if(sf_near_cross(x, z, 10.0f)) ok = false;
    if(ok && sf_crook_dist(x, z) < SF_LADIES_R) {
        ok = false;
        covered = true;
    }
    if(sf_in_solid(v3(x, sf_terrain(x, z) + 3.0f, z), 0.5f)) {
        ok = false;
        covered |= sf_in_solid(v3(x, sf_terrain(x, z) + 3.0f, z), 0.0f);
    }
    if(note && !ok && !covered && sf_gap_n < SF_GAPS) {
        sf_gap[sf_gap_n][0] = x;
        sf_gap[sf_gap_n++][1] = z;
    }
    return ok;
}

/* One ring of a block: mode 0 the fronts, 1 the backs round the yard (both
   in the facade atlas), 2 the roofs (flat colours). A post is emitted at
   the start of each stretch of kept houses, at every run boundary (twice:
   the step), at the block's corners and at each stretch's end. */
static void sf_block_ring(const SfLot *lot, int n, const SfRun *run, int mode, V3 mid) {
    int s0 = 0;
    bool all = true;
    for(int g = 0; g < n; g++)
        if(!lot[g].ok) {
            all = false;
            s0 = (g + 1) % n;
        }
    bool open = false;
    int first = 0;
    for(int k = 0; k < n; k++) {
        int g = (s0 + k) % n;
        const SfLot *L = &lot[g];
        if(!L->ok) continue;
        const SfRun *R = &run[L->run];
        int prev = (g + n - 1) % n, next = (g + 1) % n;
        bool start = !open, step = open && lot[prev].run != L->run;
        bool end = !lot[next].ok || (all && k == n - 1);
        /* Round the whole ring, the last post goes in again at the first
           run's top, closing the step there. */
        bool close = all && k == n - 1 && lot[s0].run != L->run;
        for(int e = 0; e < 3; e++) {
            /* e 0: the house's start post (a stretch's first, or a step:
               the run before ended at its own top), 1: its end post, 2:
               the closing step. */
            bool need = e == 0 ? start || step : e == 1 ? end || lot[next].corner || lot[next].run != L->run : close;
            if(!need) continue;
            const SfRun *was = e == 0 ? &run[lot[prev].run] : R;   /* the run before a step */
            if(e == 2) R = &run[lot[s0].run];
            V3 o = e ? L->o1 : L->o0, in = e ? L->i1 : L->i0;
            float u = 0.25f * (float)(k + (e ? 1 : 0));
            bool join = e == 0 && start;
            V3 nrm = vnorm(v3(o.x - mid.x, 0, o.z - mid.z));
            if(!(e ? lot[next].corner : L->corner)) {
                /* Mid-side: the side's own normal. */
                V3 t = vsub(L->o1, L->o0);
                nrm = vnorm(v3(t.z, 0, -t.x));
                if(vdot(nrm, vsub(o, mid)) < 0) nrm = vscale(nrm, -1.0f);
            }
            if(mode == 0) {
                float vb = R->v0 + 0.5f * fminf((R->top - R->obot) / R->h, 1.0f) - 0.002f;
                uint32_t c = shade(0xffffffff, nrm);
                if(join) svj(v3(o.x, R->top, o.z), u, R->v0 + 0.002f, c);
                else svp(v3(o.x, R->top, o.z), u, R->v0 + 0.002f, c);
                svp(v3(o.x, R->obot, o.z), u, vb, c);
            }
            else if(mode == 1) {
                float vb = R->v0 + 0.5f * fminf((R->top - R->ibot) / R->h, 1.0f) - 0.002f;
                uint32_t c = color_scale(shade(0xffffffff, vscale(nrm, -1.0f)), 0.8f);
                if(join) svj(v3(in.x, R->top, in.z), u, R->v0 + 0.002f, c);
                else svp(v3(in.x, R->top, in.z), u, R->v0 + 0.002f, c);
                svp(v3(in.x, R->ibot, in.z), u, vb, c);
            }
            else {
                /* Flat colours from the atlas's white corner texel, so the
                   roofs share the walls' strip. */
                if(!join && e != 1) {
                    /* A step: the taller run's end wall, plain stucco,
                       facing down the roofline. */
                    V3 t = vnorm(vsub(L->o1, L->o0));
                    uint32_t w = shade(0xffd8d4ca, R->top > was->top ? vscale(t, -1.0f) : t);
                    svp(v3(o.x, was->top, o.z), SF_WHITE_UV, SF_WHITE_UV, w);
                    svp(v3(in.x, was->top, in.z), SF_WHITE_UV, SF_WHITE_UV, w);
                    svp(v3(o.x, R->top, o.z), SF_WHITE_UV, SF_WHITE_UV, w);
                    svp(v3(in.x, R->top, in.z), SF_WHITE_UV, SF_WHITE_UV, w);
                }
                uint32_t c = shade(R->roof, v3(0, 1, 0));
                if(join) svj(v3(o.x, R->top, o.z), SF_WHITE_UV, SF_WHITE_UV, c);
                else svp(v3(o.x, R->top, o.z), SF_WHITE_UV, SF_WHITE_UV, c);
                svp(v3(in.x, R->top, in.z), SF_WHITE_UV, SF_WHITE_UV, c);
            }
        }
        if(start) first = g;
        open = !end;
        if(end && mode == 0 && !all) {
            /* Wall off both ends of the stretch: a house front on each. */
            for(int w = 0; w < 2; w++) {
                const SfLot *E = w ? L : &lot[first];
                const SfRun *ER = &run[E->run];
                V3 o = w ? E->o1 : E->o0, in = w ? E->i1 : E->i0;
                float bot = fminf(ER->obot, ER->ibot);
                V3 t = vsub(in, o);
                V3 nrm = vnorm(w ? v3(-t.z, 0, t.x) : v3(t.z, 0, -t.x));
                if(vdot(nrm, vsub(vlerp(o, in, 0.5f), vlerp(E->o0, E->i1, 0.5f))) < 0) nrm = vscale(nrm, -1.0f);
                float u0 = 0.25f * (float)(g & 3);
                sf_quadj(v3(o.x, ER->top, o.z), v3(in.x, ER->top, in.z), v3(o.x, bot, o.z), v3(in.x, bot, in.z), u0,
                         ER->v0 + 0.002f, u0 + 0.25f * SF_LOT_D / 7.0f, ER->v0 + 0.498f, shade(0xffffffff, nrm));
            }
        }
    }
}

/* Round the crooked street the painted ladies close behind the terraces,
   as the street grid's blocks would stand back from the switchbacks:
   small blocks of Victorians on a 17 m grid, each a 14 m square with two
   house fronts a side, standing on its highest corner. */
static int sf_ladies(void) {
    float s0 = track_seg_s(SF_CROOK_SEG), s1 = track_seg_s(SF_CROOK_END + 1);
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for(float s = s0; s <= s1; s += 4.0f) {
        V3 p = track_point(s, 0.0f);
        x0 = fminf(x0, p.x);
        x1 = fmaxf(x1, p.x);
        z0 = fminf(z0, p.z);
        z1 = fmaxf(z1, p.z);
    }
    const float hw = 7.0f, pitch = 17.0f;
    struct {
        float x, z, lo, hi;
        uint32_t hsh;
        int group;
    } lady[64];
    int n = 0;
    int gi = 0, gj = 0;
    for(float z = z0 - 60.0f; z < z1 + 60.0f; z += pitch, gj++) {
        gi = 0;
        for(float x = x0 - 60.0f; x < x1 + 60.0f && n < 64; x += pitch, gi++) {
            if(sf_crook_dist(x, z) > SF_LADIES_R + 6.0f || sk_road_dist(v3(x, 0, z)) < 24.0f || z > sf_shore(x) - 14.0f) continue;
            if(sf_near_cross(x, z, 16.0f)) continue;
            bool clear = true;
            float lo = 1e9f, hi = -1e9f;
            for(int i = 0; i < 4 && clear; i++) {
                float cx = x + ((i & 1) ? hw : -hw), cz = z + ((i & 2) ? hw : -hw), g = sf_terrain(cx, cz);
                lo = fminf(lo, g);
                hi = fmaxf(hi, g);
                clear = !sf_in_solid(v3(cx, g + 3.0f, cz), 1.0f);
            }
            if(!clear || sf_in_solid(v3(x, sf_terrain(x, z) + 3.0f, z), 1.0f)) continue;
            uint32_t hsh = (uint32_t)(int)(x * 7.0f + 5000.0f) * 2654435761u ^ (uint32_t)(int)(z * 13.0f + 5000.0f) * 40503u;
            lady[n].x = x;
            lady[n].z = z;
            lady[n].lo = lo;
            lady[n].hi = hi;
            lady[n].group = (gj / 2) * 64 + gi / 2;
            lady[n++].hsh = hsh;
        }
    }
    /* Not far detail: no street houses line the switchbacks, so in a
       split screen (which leaves far detail out) they are all there is.
       Two by two a chunk, drawn from 240 m (a split screen's ~110 m), so
       a split screen only has them at the crooked street. */
    for(int g0 = 0; g0 < n; g0++) {
        bool first = true;
        for(int q = 0; q < g0; q++) first &= lady[q].group != lady[g0].group;
        if(!first) continue;
        chunk_begin(HF_OP, 240.0f);
        strip_begin(h_sffacade);
        for(int q = g0; q < n; q++) {
            if(lady[q].group != lady[g0].group) continue;
            float x = lady[q].x, z = lady[q].z, lo = lady[q].lo, hi = lady[q].hi;
            uint32_t hsh = lady[q].hsh;
            V3 c[4] = {v3(x - hw, 0, z - hw), v3(x + hw, 0, z - hw), v3(x + hw, 0, z + hw), v3(x - hw, 0, z + hw)};
            float h = 9.5f + (float)((hsh >> 13) & 7) * 0.4f, top = hi + h, base = lo - 1.0f;
            sf_add_solid(c[0], v3(2.0f * hw, 0, 0), v3(0, 0, 2.0f * hw), base, top);
            /* The four sides as one ring, two house fronts a side: u runs
               on round it (the atlas repeats), each corner lit by its two
               faces; below the facade's foot (the high corner) the low
               side's storeys stretch down to the ground. Then the roof,
               in the blocks' colours, from the atlas's white corner. */
            float u = (float)((hsh >> 7) & 1) * 0.5f, v0 = (float)((hsh >> 8) & 1) * 0.5f;
            float vb = v0 + 0.5f * fminf(1.0f + (hi - lo) / h, 1.24f);
            static const V3 corner_n[4] = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}};
            for(int k = 0; k <= 4; k++) {
                int i = k & 3;
                uint32_t col = shade(0xffffffff, corner_n[i]);
                if(k == 0) svj(v3(c[i].x, top, c[i].z), u, v0 + 0.002f, col);
                else svp(v3(c[i].x, top, c[i].z), u, v0 + 0.002f, col);
                svp(v3(c[i].x, base, c[i].z), u, vb, col);
                u += 0.5f;
            }
            static const uint32_t roofs[4] = {0xff86847f, 0xff9c9890, 0xffaa9c88, 0xffb4b0a6};
            uint32_t rc = shade(roofs[(hsh >> 17) & 3u], v3(0, 1, 0));
            sf_quadj(v3(c[3].x, top, c[3].z), v3(c[2].x, top, c[2].z), v3(c[0].x, top, c[0].z), v3(c[1].x, top, c[1].z),
                     SF_WHITE_UV, SF_WHITE_UV, SF_WHITE_UV, SF_WHITE_UV, rc);
        }
        strip_end();
        chunk_end();
    }
    return n;
}

/* North Beach's flats east of the crooked street (their plainer fronts,
   sffacade2's lower row), else the Victorians; only the Victorians within
   sight of the crooked street. */
static bool sf_block_town(int bi, int bj) {
    float x = SF_BX0 + ((float)bi + 0.5f) * SF_BPX, z = SF_BZ0 + ((float)bj + 0.5f) * SF_BPZ;
    uint32_t hsh = (uint32_t)(bi + 7) * 2654435761u ^ (uint32_t)(bj + 13) * 40503u;
    return x > -90.0f && ((hsh >> 5) & 3u) && sf_crook_dist(x, z) > 120.0f;
}

/* Yard fences: where a block's house is left out (by the road, a cross
   street), a fence along the back of the empty lot closes the yard off
   from the street (it showed through the gap). Drawn by sf_city_fill
   after each chunk's blocks. */
#define SF_FENCES 96
static V3 sf_fence[SF_FENCES][2];
static int sf_fence_n;

static bool sf_fence_ok(V3 p) {
    float d;
    return sk_road_dist(p) > 11.0f && p.z < sf_shore(p.x) - 4.0f && !sf_near_cross(p.x, p.z, 7.0f) &&
           sf_park_reach(p.x, p.z, &d) < d - 1.0f && sf_crook_dist(p.x, p.z) > SF_LADIES_R;
}

static void sf_fences_draw(void) {
    if(!sf_fence_n) return;
    strip_begin(h_sfrailing);
    for(int i = 0; i < sf_fence_n; i++) {
        V3 a = sf_fence[i][0], b = sf_fence[i][1];
        float ga = sf_terrain(a.x, a.z), gb = sf_terrain(b.x, b.z);
        float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
        uint32_t c = shade(0xffe0e0e0, v3(b.z - a.z, 0, a.x - b.x));
        sf_quadj(v3(a.x, ga + 2.2f, a.z), v3(b.x, gb + 2.2f, b.z), v3(a.x, ga - 0.3f, a.z), v3(b.x, gb - 0.3f, b.z),
                 0.0f, 0.0f, len / 4.0f, 0.98f, c);
    }
    strip_end();
    sf_fence_n = 0;
}

/* One block into the open strip (the facade atlas town picks). */
static int sf_block(int bi, int bj, bool near_road, bool town) {
    float x0 = SF_BX0 + (float)bi * SF_BPX + SF_STREET, x1 = x0 + SF_BPX - 2.0f * SF_STREET;
    float z0 = SF_BZ0 + (float)bj * SF_BPZ + SF_STREET, z1 = z0 + SF_BPZ - 2.0f * SF_STREET;
    const float dd = SF_LOT_D;
    V3 C[4] = {v3(x0, 0, z0), v3(x1, 0, z0), v3(x1, 0, z1), v3(x0, 0, z1)};
    V3 D[4] = {v3(x0 + dd, 0, z0 + dd), v3(x1 - dd, 0, z0 + dd), v3(x1 - dd, 0, z1 - dd), v3(x0 + dd, 0, z1 - dd)};
    V3 mid = v3((x0 + x1) * 0.5f, 0, (z0 + z1) * 0.5f);
    uint32_t hsh = (uint32_t)(bi + 7) * 2654435761u ^ (uint32_t)(bj + 13) * 40503u;
    SfLot lot[SF_RING];
    SfRun run[SF_RING];
    int n = 0, nrun = 0, left = 0, kept = 0;
    for(int k = 0; k < 4; k++) {
        V3 a = C[k], b = C[(k + 1) & 3], ia = D[k], ib = D[(k + 1) & 3];
        float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
        int m = (int)(len / 7.2f + 0.5f);
        for(int i = 0; i < m && n < SF_RING; i++) {
            SfLot *L = &lot[n++];
            float t0 = (float)i / (float)m, t1 = (float)(i + 1) / (float)m;
            L->o0 = vlerp(a, b, t0);
            L->o1 = vlerp(a, b, t1);
            L->i0 = vlerp(ia, ib, t0);
            L->i1 = vlerp(ia, ib, t1);
            L->corner = i == 0;
            V3 c = vlerp(vlerp(L->o0, L->o1, 0.5f), vlerp(L->i0, L->i1, 0.5f), 0.5f);
            /* The middle always tested, so each gap is noted (by its middle:
               a row of the ground goes plain only where a lot left out
               really shows). */
            bool ok4 = sf_lot_ok(c.x, c.z, near_road, true);
            L->ok = ok4 && sf_lot_ok(L->o0.x, L->o0.z, near_road, false) && sf_lot_ok(L->o1.x, L->o1.z, near_road, false) &&
                    sf_lot_ok(L->i0.x, L->i0.z, near_road, false) && sf_lot_ok(L->i1.x, L->i1.z, near_road, false);
            kept += L->ok;
            /* Runs of three to five houses. */
            if(left == 0) {
                hsh = hsh * 1664525u + 1013904223u;
                left = 3 + (int)((hsh >> 11) % 3u);
                nrun++;
            }
            L->run = nrun - 1;
            left--;
        }
    }
    if(kept == 0) return 0;
    for(int g = 0; g < n && sf_fence_n < SF_FENCES; g++)
        if(!lot[g].ok && sf_fence_ok(lot[g].i0) && sf_fence_ok(lot[g].i1)) {
            sf_fence[sf_fence_n][0] = lot[g].i0;
            sf_fence[sf_fence_n++][1] = lot[g].i1;
        }
    static const uint32_t roofs[8] = {0xff86847f, 0xff9c9890, 0xffb4b0a6, 0xffc8c4b8, 0xff8e8a88,
                                      0xffaa9c88, 0xffa4705c, 0xffd6d2c8};
    for(int r = 0; r < nrun; r++) {
        SfRun *R = &run[r];
        float hi = -1e9f, olo = 1e9f, ilo = 1e9f;
        for(int g = 0; g < n; g++) {
            if(lot[g].run != r) continue;
            const SfLot *L = &lot[g];
            V3 om = vlerp(L->o0, L->o1, 0.5f), im = vlerp(L->i0, L->i1, 0.5f);
            float go[3] = {sf_terrain(L->o0.x, L->o0.z), sf_terrain(om.x, om.z), sf_terrain(L->o1.x, L->o1.z)};
            float gi[3] = {sf_terrain(L->i0.x, L->i0.z), sf_terrain(im.x, im.z), sf_terrain(L->i1.x, L->i1.z)};
            for(int q = 0; q < 3; q++) {
                hi = fmaxf(hi, fmaxf(go[q], gi[q]));
                olo = fminf(olo, go[q]);
                ilo = fminf(ilo, gi[q]);
            }
        }
        hsh = hsh * 1664525u + 1013904223u;
        R->h = town ? 10.0f + (float)((hsh >> 8) & 7) * 0.55f : 9.6f + (float)((hsh >> 8) & 7) * 0.4f;
        R->top = hi + R->h;
        R->obot = olo - 1.2f;
        R->ibot = ilo - 1.2f;
        R->v0 = town || ((hsh >> 14) & 1u) ? 0.5f : 0.0f;
        R->roof = roofs[(hsh >> 17) & 7u];
    }
    sf_block_ring(lot, n, run, 0, mid);
    sf_block_ring(lot, n, run, 1, mid);
    sf_block_ring(lot, n, run, 2, mid);
    /* Solids for the spectator's cameras: each side's kept houses. */
    for(int k = 0; k < 4; k++) {
        float lo = 1e9f, top = -1e9f;
        int g0 = -1, g1 = -1;
        for(int g = 0; g < n; g++) {
            int side = 0;
            for(int q = 1; q <= g; q++) side += lot[q].corner;
            if(side != k || !lot[g].ok) continue;
            if(g0 < 0) g0 = g;
            g1 = g;
            lo = fminf(lo, fminf(run[lot[g].run].obot, run[lot[g].run].ibot));
            top = fmaxf(top, run[lot[g].run].top);
        }
        if(g0 < 0) continue;
        V3 a = lot[g0].o0, b = lot[g1].o1;
        V3 in = vsub(lot[g0].i0, lot[g0].o0);
        V3 t = vnorm(vsub(b, a));
        float depth = fabsf(in.x * t.z - in.z * t.x);
        V3 nrm = v3(-t.z, 0, t.x);
        if(vdot(nrm, vsub(mid, a)) < 0) nrm = vscale(nrm, -1.0f);
        sf_add_solid(a, vsub(b, a), vscale(nrm, depth), lo, top);
    }
    return kept;
}

static void sf_city_fill(void) {
    int blocks = 0, houses = 0;
    sf_gap_n = 0;
    /* Two by two blocks a chunk, a strip for each facade atlas in it. */
    for(int gj = 0; SF_BZ0 + (float)(2 * gj) * SF_BPZ < 80.0f; gj++) {
        for(int gi = 0; SF_BX0 + (float)(2 * gi) * SF_BPX < 250.0f; gi++) {
            float rd[4];
            bool any = false;
            for(int q = 0; q < 4; q++) {
                int bi = 2 * gi + (q & 1), bj = 2 * gj + (q >> 1);
                V3 c = v3(SF_BX0 + ((float)bi + 0.5f) * SF_BPX, 0, SF_BZ0 + ((float)bj + 0.5f) * SF_BPZ);
                rd[q] = c.z > sf_shore(c.x) + 20.0f ? 1e9f : sk_road_dist(c);
                any |= rd[q] <= 165.0f;
            }
            if(!any) continue;
            chunk_begin(HF_OP, 240.0f);
            if(cur_chunk) cur_chunk->far = 1;
            for(int town = 0; town < 2; town++) {
                strip_begin(town ? h_sffacade2 : h_sffacade);
                for(int q = 0; q < 4; q++) {
                    int bi = 2 * gi + (q & 1), bj = 2 * gj + (q >> 1);
                    if(rd[q] > 165.0f || sf_block_town(bi, bj) != (town != 0)) continue;
                    int k = sf_block(bi, bj, rd[q] < 14.0f + 0.5f * sqrtf(SF_BPX * SF_BPX + SF_BPZ * SF_BPZ) + 2.0f, town);
                    blocks += k > 0;
                    houses += k;
                }
                strip_end();
            }
            sf_fences_draw();
            chunk_end();
        }
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DreamKarts bay city: %d row houses, %d sheds, %d blocks behind (%d houses); park rim %.0f-%.0f m\n",
           sf_houses_n, sf_sheds_n, blocks, houses, (double)sf_park_rim[0], (double)sf_park_rim[SF_PARK_N / 2]);
#endif
    int ladies = sf_ladies();
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DreamKarts bay city: %d painted ladies round the crooked street\n", ladies);
#else
    (void)ladies;
#endif
    sf_ground_gaps();
}

/* One cross street off Hyde Street at lap distance sc on one side:
   asphalt and sidewalks out from the railing, flat at the crossing's
   height for its first few metres and then easing onto the ground; with
   a car, its cable car track and the car waiting 22 m out. */
static void sf_prop(int which, V3 p, float yaw, float scale, float lod_far, bool far);

static void sf_cross_street(float sc, int side, bool car) {
    V3 t = track_tangent(sc);
    float yp = track_road_y(sc, 0.0f);
    enum { N = 9 };
    V3 c[N + 1];
    for(int j = 0; j <= N; j++) {
        float d = 10.2f + (SF_CROSS_LEN - 10.2f) * (float)j / N;
        c[j] = track_point(sc, (float)side * d);
        c[j].y = lerpf(yp, sf_terrain(c[j].x, c[j].z) + 0.05f, smooth01(14.0f, 30.0f, d)) + 0.02f;
    }
    chunk_begin(HF_OP, 420.0f);
    /* Asphalt across 13 m (the road texture's width), sidewalks either side. */
    strip_begin(h_sfroad);
    for(int j = 0; j <= N; j++) {
        float v = (float)j * (SF_CROSS_LEN - 10.2f) / N / 16.0f;
        svp(vmad(c[j], t, -6.5f), 0.0f, v, 0xfff0f0f0);
        svp(vmad(c[j], t, 6.5f), 1.0f, v, 0xfff0f0f0);
    }
    strip_end();
    for(int e = -1; e <= 1; e += 2) {
        strip_begin(h_sfwalk);
        for(int j = 0; j <= N; j++) {
            V3 a = vmad(c[j], t, 6.5f * (float)e), b = vmad(c[j], t, 8.0f * (float)e);
            a.y += 0.12f;
            b.y += 0.12f;
            svp(a, a.x / 6.0f, a.z / 6.0f, 0xffe8e8e8);
            svp(b, b.x / 6.0f, b.z / 6.0f, 0xffe8e8e8);
        }
        strip_end();
    }
    if(car) {
        /* The trackway, the two rails and the cable's slot. */
        static const float w[4][2] = {{-0.95f, 0.95f}, {-0.585f, -0.485f}, {0.485f, 0.585f}, {-0.03f, 0.03f}};
        static const uint32_t col[4] = {0xffa09e98, 0xffced2d8, 0xffced2d8, 0xff1e1e20};
        for(int q = 0; q < 4; q++) {
            strip_begin(h_col);
            for(int j = 0; j <= N; j++) {
                float lift = q ? 0.04f : 0.03f;
                svp(vadd(vmad(c[j], t, w[q][0]), v3(0, lift, 0)), 0, 0, col[q]);
                svp(vadd(vmad(c[j], t, w[q][1]), v3(0, lift, 0)), 0, 0, col[q]);
            }
            strip_end();
        }
    }
    chunk_end();
    if(car) {
        float f = (22.0f - 10.2f) / (SF_CROSS_LEN - 10.2f) * N;
        int j = (int)f;
        V3 p = vlerp(c[j], c[j + 1], f - (float)j);
        V3 dir = vsub(c[N], c[0]);
        /* Drawn within 200 m: from the start straight, 240 m off, it was
           1,300 vertices (~0.7 ms on the console) for a speck. */
        sf_prop(SFP_CABLECAR, vadd(p, v3(0, 0.05f, 0)), atan2f(dir.x, dir.z), 1.0f, 200.0f, true);
    }
}

/* ---- The pier ---- */

/* Between the pier's two lanes: planks over the gap and a row of little
   kiosks down the middle; under the deck, pilings in the bay. */
static void sf_pier(void) {
    float s1 = track_seg_s(SF_PIER_SEG + 1), s2 = track_seg_s(SF_PIER_SEG + 3);
    float len = track_seg_s(SF_PIER_SEG + 2) - s1;
    V3 a0 = track_point(s1, -10.0f), a1 = track_point(s1 + len, -10.0f);
    V3 b0 = track_point(s2 + len, -10.0f), b1 = track_point(s2, -10.0f);
    chunk_begin(HF_OP, 700.0f);
    float y = SF_QUAY_Y;
    quad(h_sfpier, v3(a1.x, y, a1.z), v3(b1.x, y, b1.z), v3(a0.x, y, a0.z), v3(b0.x, y, b0.z), 0, a1.z / 16.0f, 0.4f,
         a0.z / 16.0f, 0xffe8e8e8);
    chunk_end();
    /* The stalls and benches on it, nearer: the deck shows across the bay. */
    chunk_begin(HF_OP, 260.0f);
    /* The market stalls down the middle, each with its front (sfstall:
       crab, chowder, sourdough, gifts) on both long sides, facing a lane. */
    static const uint32_t kiosk[4] = {0xffd64838, 0xff3c74be, 0xffe2a828, 0xff469660};
    V3 sc[4];
    for(int i = 0; i < 4; i++) {
        float t = (0.15f + 0.22f * (float)i);
        sc[i] = vlerp(vlerp(a0, b0, 0.5f), vlerp(a1, b1, 0.5f), t);
        box(h_col, v3(sc[i].x, y + 1.5f, sc[i].z), v3(1.5f, 1.5f, 3.0f), 0, 0xfff4f0e8, 1.0f);
        box(h_col, v3(sc[i].x, y + 3.2f, sc[i].z), v3(2.0f, 0.2f, 3.4f), 0, kiosk[i], 1.0f);
    }
    strip_begin(h_sfstall);
    for(int i = 0; i < 4; i++) {
        float v0 = 0.25f * (float)i + 0.003f, v1 = v0 + 0.244f;
        for(int side = -1; side <= 1; side += 2) {
            /* Facing +x, the viewer's right is +z; facing -x, it is -z. */
            float x = sc[i].x + (float)side * 1.52f, za = sc[i].z - (float)side * 3.0f, zb = sc[i].z + (float)side * 3.0f;
            sf_quadj(v3(x, y + 3.0f, za), v3(x, y + 3.0f, zb), v3(x, y, za), v3(x, y, zb), 0.003f, v0, 0.997f, v1,
                     shade(0xffffffff, v3((float)side, 0, 0)));
        }
    }
    strip_end();
    /* Park benches back to back between the stalls, one facing each
       lane: a green slatted seat and back on dark iron ends. */
    V3 dir = vnorm(vsub(vlerp(a1, b1, 0.5f), vlerp(a0, b0, 0.5f))), across = v3(dir.z, 0, -dir.x);
    strip_begin(h_col);
    for(int i = 0; i < 3; i++) {
        V3 c = vlerp(sc[i], sc[i + 1], 0.5f);
        for(int side = -1; side <= 1; side += 2) {
            V3 n = vscale(across, (float)side), m = vmad(c, n, 0.35f);
            V3 l = vmad(m, dir, -0.9f), r = vmad(m, dir, 0.9f);
            V3 lf = vmad(l, n, 0.45f), rf = vmad(r, n, 0.45f);
            uint32_t seat = shade(argb(255, 58, 96, 66), v3(0, 1, 0)), back = shade(argb(255, 52, 86, 60), n);
            uint32_t iron = shade(argb(255, 40, 44, 42), n);
            sf_quadj(sf_up(l, y + 0.47f), sf_up(r, y + 0.47f), sf_up(lf, y + 0.45f), sf_up(rf, y + 0.45f), 0, 0, 0, 0, seat);
            sf_quadj(sf_up(lf, y + 0.45f), sf_up(rf, y + 0.45f), sf_up(lf, y), sf_up(rf, y), 0, 0, 0, 0, iron);
            sf_quadj(sf_up(l, y + 1.0f), sf_up(r, y + 1.0f), sf_up(l, y + 0.47f), sf_up(r, y + 0.47f), 0, 0, 0, 0, back);
        }
    }
    strip_end();
    chunk_end();
}

/* ---- The crooked street's beds ---- */

/* Hydrangea hedges along the inside of each railing down the switchbacks,
   rounded on top, and a mound of them filling each hairpin's inside. */
static void sf_beds(void) {
    for(int g = SF_CROOK_SEG; g <= SF_CROOK_END; g++) {
        float s0 = track_seg_s(g), s1 = track_seg_s(g + 1);
        float len = track_wrap(s1 - s0);
        chunk_begin(HF_OP, 400.0f);
        for(int side = -1; side <= 1; side += 2) {
            strip_begin(h_sfhedge);
            int n = (int)(len / 3.0f) + 1;
            for(int k = 0; k <= n; k++) {
                float s = s0 + len * (float)k / (float)n;
                V3 a = f8_pt(s, side * 9.2f, 0.0f), b = f8_pt(s, side * 9.9f, 0.0f);
                a.y += 0.05f;
                b.y += 1.2f;
                uint32_t ca = argb(255, 200, 200, 200), cb = 0xffffffff;
                if(side > 0) {
                    svp(a, s / 3.0f, 0.0f, ca);
                    svp(b, s / 3.0f, 0.4f, cb);
                }
                else {
                    svp(b, s / 3.0f, 0.4f, cb);
                    svp(a, s / 3.0f, 0.0f, ca);
                }
            }
            strip_end();
        }
        chunk_end();
    }
}

/* The terraces behind the crooked street's railings: from each railing's
   top a bed of hydrangeas slopes out to meet the next switchback's,
   halfway between them at the height between their two roads, so no gap
   between the legs is left open to see the ground and the retaining
   walls far below. Inside a hairpin the beds rise to a mound at its
   centre; where there is no leg beside, they come down to the ground. */
#define SF_TERRACE_OUT 12.5f   /* half the 25 m between legs */

static float sf_terrace_y(V3 p, float s, float y_wall) {
    float best = 1e9f, y = 0;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        if(fabsf(track_delta(track.smp[i].s, s)) < 30.0f) continue;   /* this stretch itself */
        float dx = p.x - track.smp[i].p.x, dz = p.z - track.smp[i].p.z, d2 = dx * dx + dz * dz;
        if(d2 < best) {
            best = d2;
            y = track_height(track.smp[i].s);
        }
    }
    if(best < 15.0f * 15.0f) return 0.5f * (y_wall + y + f8m.wall_h);   /* a leg beside: meet its bank halfway */
    return fminf(y_wall, sf_terrain(p.x, p.z) + 0.3f);
}

static void sf_terraces(void) {
    float s0 = track_seg_s(SF_CROOK_SEG) - 6.0f, s1 = track_seg_s(SF_CROOK_END + 1) + 6.0f;
    int n = (int)((s1 - s0) / 2.5f);
    for(int side = -1; side <= 1; side += 2) {
        for(int c0 = 0; c0 < n; c0 += 16) {
            chunk_begin(HF_OP, 450.0f);
            strip_begin(h_sfhedge);
            for(int k = c0; k <= c0 + 16 && k <= n; k++) {
                float s = s0 + (s1 - s0) * (float)k / (float)n;
                /* Inside a bend, out no further than its centre. */
                float curv = track_curvature(s) * (float)side, out = SF_TERRACE_OUT;
                if(curv < 0) out = fminf(out, 1.0f / -curv);
                float wall = f8_wall(s) + 0.2f;
                V3 a = f8_pt(s, (float)side * wall, f8m.wall_h);
                V3 b = track_point(s, (float)side * fmaxf(out, wall + 0.5f));
                b.y = sf_terrace_y(b, s, a.y - f8m.wall_h);
                uint32_t ca = 0xffffffff, cb = argb(255, 214, 214, 214);
                if(side > 0) {
                    svp(a, a.x / 4.0f, a.z / 4.0f, ca);
                    svp(b, b.x / 4.0f, b.z / 4.0f, cb);
                }
                else {
                    svp(b, b.x / 4.0f, b.z / 4.0f, cb);
                    svp(a, a.x / 4.0f, a.z / 4.0f, ca);
                }
            }
            strip_end();
            chunk_end();
        }
    }
}

/* ---- Props ---- */

static void sf_prop(int which, V3 p, float yaw, float scale, float lod_far, bool far) {
    prop_place(&sf_props[which], sf_mat_hdr, SFM_PT_MASK, p, yaw, scale, lod_far, far);
}

static float sf_heading(float s) {
    V3 t = track_tangent(s);
    return atan2f(t.x, t.z);
}

/* The downtown towers south-east of Telegraph Hill, round the pyramid. */
static void sf_downtown(void) {
    static const float t[][4] = {
        {300.0f, -330.0f, 70.0f, 14.0f}, {330.0f, -380.0f, 88.0f, 16.0f}, {240.0f, -390.0f, 60.0f, 13.0f},
        {350.0f, -300.0f, 52.0f, 12.0f}, {280.0f, -440.0f, 76.0f, 15.0f}, {380.0f, -420.0f, 64.0f, 14.0f},
        {210.0f, -330.0f, 44.0f, 12.0f},
    };
    chunk_begin(HF_OP, 3000.0f);
    if(cur_chunk) cur_chunk->far = 1;
    for(unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        float gy = sf_far(t[i][0], t[i][1]);
        box(h_sfbldg, v3(t[i][0], gy + t[i][2] * 0.5f - 3.0f, t[i][1]), v3(t[i][3], t[i][2] * 0.5f + 3.0f, t[i][3]),
            0.3f * (float)(i & 1), i & 1 ? 0xffd0dae4 : 0xffe8e2d6, 1.0f / 8.0f);
    }
    chunk_end();
    sf_prop(SFP_PYRAMID, v3(265.0f, sf_far(265.0f, -355.0f) - 2.0f, -355.0f), 0.3f, 1.0f, 3000.0f, true);
}

static void sf_place_props(void) {
    world_gates = 0;
    sf_start_line();

    /* The far scenery: the bridge and its headlands, Alcatraz. */
    sf_prop(SFP_BRIDGE, v3(SF_BRIDGE_X, 0.0f, SF_BRIDGE_Z), SF_BRIDGE_YAW, 30.0f, 6000.0f, false);
    sf_prop(SFP_ALCATRAZ, v3(-150.0f, 0.0f, 720.0f), 0.55f, 10.0f, 4000.0f, false);
    /* Coit Tower in Pioneer Park, a ring of cypresses round the hilltop
       outside the path where the lawn reaches far enough. */
    sf_prop(SFP_COIT, v3(SF_COIT_X, sf_terrain(SF_COIT_X, SF_COIT_Z) - 0.5f, SF_COIT_Z), 0.0f, 1.0f, 3000.0f, false);
    for(int i = 0; i < 8; i++) {
        float a = (float)i * (2.0f * DK_PI / 8.0f) + 0.2f * sinf((float)i * 2.7f), r = 25.0f + 3.0f * sinf((float)i * 1.9f);
        float x = SF_COIT_X + sinf(a) * r, z = SF_COIT_Z + cosf(a) * r, d;
        /* On the lawn (the crown 3.5 m across) and clear of the houses,
           whose fronts it would poke through. */
        if(sf_park_reach(x, z, &d) < r + 3.0f || sf_in_solid(v3(x, sf_terrain(x, z) + 3.0f, z), 3.5f)) continue;
        sf_prop(SFP_CYPRESS, v3(x, sf_terrain(x, z) - 0.2f, z), (float)i * 1.3f, 0.9f + 0.15f * (float)(i % 3), 600.0f,
                true);
    }
    sf_downtown();

    /* The waterfront: the wharf's sign over the road, palms along the
       Embarcadero, boats at their moorings and sea lions off the pier. */
    float wharf = track_seg_s(6);
    {
        float s = wharf + 22.0f;
        V3 p = track_point(s, 0.0f);
        p.y = track_road_y(s, 0.0f);
        sf_prop(SFP_SIGN_ARCH, p, sf_heading(s), 1.0f, 900.0f, false);
    }
    for(int i = 0; i < 7; i++) {
        float s = track_wrap(-30.0f + 22.0f * (float)i);
        int seg = sf_seg_at(s);
        if(sf_on_pier(seg)) continue;
        V3 p = track_point(s, 11.0f);   /* on the quay's edge */
        if(sk_road_dist(p) < 10.6f) continue;
        sf_prop(SFP_PALM, v3(p.x, sf_terrain(p.x, p.z), p.z), (float)i, 1.0f, 500.0f, true);
    }
    static const float boats[][3] = {{-104.0f, 80.0f, 1.57f}, {-160.0f, 97.0f, -1.5f}, {-186.0f, 99.0f, -1.62f},
                                     {-20.0f, 84.0f, 1.6f}, {10.0f, 82.0f, -1.55f}};
    for(unsigned i = 0; i < sizeof(boats) / sizeof(boats[0]); i++)
        sf_prop(SFP_BOAT, v3(boats[i][0], WATER_Y - 0.2f, boats[i][1]), boats[i][2], 1.0f, 170.0f, true);
    /* Beside each of the pier's lanes and off its head, ~15 m out from its
       edge, where they show over it; drawn within 110 m (the detailed herd
       is ~2,000 vertices a raft, a speck from the start line). */
    static const float rafts[][3] = {{-28.0f, 104.0f, 0.1f}, {-66.0f, 157.0f, 1.65f}, {-104.0f, 108.0f, -0.12f}};
    for(unsigned i = 0; i < sizeof(rafts) / sizeof(rafts[0]); i++)
        sf_prop(i == 1 ? SFP_SEALIONS_B : SFP_SEALIONS, v3(rafts[i][0], WATER_Y - 0.25f, rafts[i][1]), rafts[i][2], 1.0f,
                110.0f, true);   /* raft B is the smaller one, off the head */

    /* Lamps along the pier's sea-side rail, out and back. */
    for(int lane = 0; lane < 2; lane++) {
        float s0 = track_seg_s(SF_PIER_SEG + 1 + 2 * lane), len = track_seg_s(SF_PIER_SEG + 2 + 2 * lane) - s0;
        for(float u = 8.0f; u < len - 4.0f; u += 22.0f) {
            float s = s0 + u;
            V3 p = track_point(s, f8_wall(s) + 0.1f);
            p.y = track_road_y(s, 0.0f);
            sf_prop(SFP_LAMP, p, sf_heading(s) + DK_PI * 0.5f, 1.0f, 90.0f, true);
        }
    }

    /* At the top of the crooked street, a lamp each side and a street
       sign on the corner: LOMBARD ST facing the karts coming up to it,
       HYDE ST along the street they are on. */
    {
        float s0 = track_seg_s(SF_CROOK_SEG);
        for(int side = -1; side <= 1; side += 2) {
            float s = s0 - 9.0f;
            V3 p = track_point(s, side * 10.35f);
            p.y = track_road_y(s, side * 9.0f);
            sf_prop(SFP_LAMP, p, sf_heading(s) + (side < 0 ? -DK_PI * 0.5f : DK_PI * 0.5f), 1.0f, 140.0f, true);
        }
        float s = s0 - 3.0f;
        V3 base = track_point(s, 10.2f), t = track_tangent(s), r = v3(t.z, 0, -t.x);
        float y = track_road_y(s, 9.0f);
        chunk_begin(HF_OP, 160.0f);
        if(cur_chunk) cur_chunk->far = 1;
        box(h_col, v3(base.x, y + 1.6f, base.z), v3(0.05f, 1.6f, 0.05f), sf_heading(s), argb(255, 40, 70, 52), 1.0f);
        strip_begin(h_sfsigns);
        for(int blade = 0; blade < 2; blade++) {
            /* Each blade back to back, so it reads from either side. */
            V3 dir = blade ? t : vscale(r, -1.0f), face = blade ? r : t;
            float y0 = y + (blade ? 2.72f : 3.0f), row = blade ? 7.0f : 6.0f;
            float v0 = 0.125f * row + 0.004f, v1 = v0 + 0.117f;
            V3 a = vmad(v3(base.x, 0, base.z), dir, blade ? -0.75f : -0.1f), b = vmad(a, dir, 1.5f);
            for(int back = 0; back < 2; back++) {
                V3 o = vscale(face, back ? 0.012f : -0.012f);
                V3 pa = vadd(back ? a : b, o), pb = vadd(back ? b : a, o);
                sf_quadj(v3(pa.x, y0 + 0.24f, pa.z), v3(pb.x, y0 + 0.24f, pb.z), v3(pa.x, y0, pa.z), v3(pb.x, y0, pb.z),
                         0.004f, v0, 0.996f, v1, shade(0xffffffff, vscale(face, back ? 1.0f : -1.0f)));
            }
        }
        strip_end();
        chunk_end();
    }

    /* The Embarcadero's run-in from the S to the line: palms behind the
       railings and benches between them, short of the start straight
       (its view from the grid is the lap's busiest). */
    for(int pass = 0; pass < 2; pass++) {
        if(pass) {
            chunk_begin(HF_OP, 200.0f);
            if(cur_chunk) cur_chunk->far = 1;
            strip_begin(h_col);
        }
        int k = 0;
        for(float s = track_seg_s(31) + 4.0f; s < track_seg_s(35) + 8.0f; s += 13.0f, k++) {
            int side = (k & 1) ? 1 : -1;
            V3 p = track_point(s, side * 11.2f);
            if(!sf_clear(p.x, p.z, 0.5f) || sf_in_solid(v3(p.x, sf_terrain(p.x, p.z) + 2.0f, p.z), 1.5f)) continue;
            if((k % 3 != 2) != (pass == 0)) continue;
            if(pass == 0) {   /* palms first: each is a chunk of its own */
                sf_prop(SFP_PALM, v3(p.x, sf_terrain(p.x, p.z), p.z), (float)k * 1.7f, 0.9f + 0.1f * (float)(k % 2), 250.0f,
                        true);
                continue;
            }
            /* A bench facing the road. */
            V3 t = track_tangent(s), n = vscale(v3(t.z, 0, -t.x), (float)side);   /* away from the road */
            float g = sf_terrain(p.x, p.z);
            V3 m = v3(p.x, g, p.z), l = vmad(m, t, -0.9f), r = vmad(m, t, 0.9f), lf = vmad(l, n, -0.45f), rf = vmad(r, n, -0.45f);
            uint32_t seat = shade(argb(255, 58, 96, 66), v3(0, 1, 0)), back = shade(argb(255, 52, 86, 60), vscale(n, -1.0f));
            uint32_t iron = shade(argb(255, 40, 44, 42), vscale(n, -1.0f));
            sf_quadj(sf_up(l, 0.47f), sf_up(r, 0.47f), sf_up(lf, 0.45f), sf_up(rf, 0.45f), 0, 0, 0, 0, seat);
            sf_quadj(sf_up(lf, 0.45f), sf_up(rf, 0.45f), lf, rf, 0, 0, 0, 0, iron);
            sf_quadj(sf_up(l, 1.0f), sf_up(r, 1.0f), sf_up(l, 0.47f), sf_up(r, 0.47f), 0, 0, 0, 0, back);
        }
        if(pass) {
            strip_end();
            chunk_end();
        }
    }

    /* Street lamps every 40 m, on one sidewalk then the other, off the
       pier and the crooked street. */
    int alt = 0;
    for(float s = 10.0f; s < track.length; s += 40.0f) {
        int seg = sf_seg_at(s), side = (alt++ & 1) ? 1 : -1;
        if(sf_on_pier(seg) || sf_in_crook(seg)) continue;
        V3 p = track_point(s, side * 10.35f);
        if(p.z > sf_shore(p.x) - 2.0f) continue;
        p.y = track_road_y(s, side * 9.0f);
        float yaw = sf_heading(s) + (side < 0 ? -DK_PI * 0.5f : DK_PI * 0.5f);   /* the arm over the road */
        sf_prop(SFP_LAMP, p, yaw, 1.0f, 140.0f, true);
    }
}

/* ---- Sound ---- */

/* Volume (0-255) and pan of a sound at p as player 1's camera hears it,
   loud as `vol` within `near` metres and falling off beyond. */
static void sf_heard(V3 p, float vol, float near, int *v, int *pan) {
    float d = sfx_listen(p, &game.pl[0].cam, 90.0f, pan);
    *v = (int)(vol * clampf(near / fmaxf(d, near), 0, 1));
}

/* The cable car's bell, two clangs as it sets off and every few seconds
   while it runs within earshot; and a foghorn out on the bay, under the
   bridge, every minute or so. Off the race clock and eng_rand, so the
   race and its replay are untouched. */
void world_sf_audio(bool on, float dt) {
    static float bell_t, horn_t = 18.0f;
    static int last_dir;
    if(!on || track.id != TRACK_SF) {
        last_dir = 0;
        return;
    }
    float cs, cd;
    int dir, v, pan;
    cablecar_pose(&cs, &cd, &dir);
    V3 p = track_point(cs, cd);
    p.y = track_road_y(cs, cd) + 2.5f;
    sf_heard(p, 230.0f, 14.0f, &v, &pan);
    bell_t -= dt;
    if((dir != 0 && last_dir == 0) || (dir != 0 && bell_t <= 0)) {
        if(v > 24) audio_sfx(DK_SFX_CC_BELL, v, pan);
        bell_t = 4.0f + 1.5f * (0.5f + 0.5f * sinf(world_time * 3.7f));
    }
    last_dir = dir;
    horn_t -= dt;
    if(horn_t <= 0) {
        sf_heard(v3(SF_BRIDGE_X * 0.5f, 6.0f, SF_BRIDGE_Z * 0.6f), 130.0f, 1e9f, &v, &pan);
        audio_sfx(DK_SFX_FOGHORN, v, pan);
        horn_t = 50.0f + 40.0f * (0.5f + 0.5f * sinf(world_time * 1.3f));
    }
}

/* ---- Per frame ---- */

/* The cable car on Hyde Street (cablecar.c moves it): its baked mesh lit
   once as it faces up the street, then each frame pitched to the slope
   under its two trucks and drawn in joined strips. */
#define SF_CC_VERTS 1500
static uint32_t sf_cc_col[SF_CC_VERTS];
static float sf_cc_yaw;

static void sf_cablecar_bake(void) {
    const dk_prop_t *m = &sf_props[SFP_CABLECAR];
    sf_cc_yaw = sf_heading(track_seg_s(SF_HYDE_SEG) + 60.0f);
    float cs = cosf(sf_cc_yaw), sn = sinf(sf_cc_yaw);
    int at = 0;
    for(int s = 0; s < m->nstrips; s++)
        for(int q = 0; q < m->strip_len[s] && at < SF_CC_VERTS; q++, at++) {
            const dk_prop_vert_t *v = &m->verts[m->index[at]];
            V3 n = v3(v->nx * cs + v->nz * sn, v->ny, -v->nx * sn + v->nz * cs);
            sf_cc_col[at] = shade(argb(255, v->r, v->g, v->b), n);
        }
}

void sf_draw_cablecar(void) {
    if(!cablecar_on()) return;
    float s, d;
    int dir;
    cablecar_pose(&s, &d, &dir);
    V3 p = track_point(s, d);
    float yf = track_road_y(s + 3.0f, d), yb = track_road_y(s - 3.0f, d);
    p.y = 0.5f * (yf + yb);
    V3 dc = vsub(p, r_cam.pos);
    float range = world_split ? 160.0f : 320.0f;
    if(vdot(dc, dc) > range * range || !r_sphere_visible(vadd(p, v3(0, 2.0f, 0)), 6.0f)) return;
    float a = atan2f(yf - yb, 6.0f), ca = cosf(a), sa = sinf(a);
    float cs = cosf(sf_cc_yaw), sn = sinf(sf_cc_yaw);
    const dk_prop_t *m = &sf_props[SFP_CABLECAR];
    RV buf[240];
    int n = 0, at = 0, mat = -1;
    for(int st = 0; st < m->nstrips; st++) {
        int len = m->strip_len[st];
        if(at + len > SF_CC_VERTS) break;
        /* A new material, or no room left: send what is joined so far. */
        if(n && (m->strip_mat[st] != mat || n + len + 2 > 240)) {
            r_strip(buf, n);
            n = 0;
        }
        if(m->strip_mat[st] != mat) {
            mat = m->strip_mat[st];
            r_hdr(sf_mat_hdr[mat]);
        }
        for(int q = 0; q < len; q++) {
            const dk_prop_vert_t *v = &m->verts[m->index[at + q]];
            float x = v->x * 0.01f, y = v->y * 0.01f, z = v->z * 0.01f;
            float y2 = y * ca + z * sa, z2 = z * ca - y * sa;   /* nose up the hill */
            RV r = {p.x + x * cs + z2 * sn, p.y + y2, p.z - x * sn + z2 * cs, v->u / 256.0f, v->v / 256.0f, sf_cc_col[at + q]};
            if(q == 0 && n) {
                buf[n] = buf[n - 1];   /* join with a degenerate pair */
                buf[n + 1] = r;
                n += 2;
            }
            buf[n++] = r;
        }
        at += len;
    }
    if(n >= 3) r_strip(buf, n);
}

/* The hills round the bay: Marin's high and green-gold behind the bridge
   to the north-west and north, Angel Island and Tiburon lower to the
   north-east, the East Bay hills far off in the east, and the city's own
   rooftops low along the south and west. A ring round the camera, beyond
   the bridge. */
static float sf_range_h(float a) {
    float north = clampf(1.0f - fabsf(wrap_angle(a + 0.45f)) / 1.3f, 0.0f, 1.0f);
    float east = clampf(1.0f - fabsf(wrap_angle(a - 1.45f)) / 0.7f, 0.0f, 1.0f);
    float h = 10.0f + north * (60.0f + 70.0f * fabsf(sinf(a * 5.0f + 0.4f)) + 30.0f * sinf(a * 11.0f));
    h += east * (30.0f + 14.0f * sinf(a * 9.0f + 1.0f));
    return h;
}

void sf_draw_ranges(void) {
    V3 c = r_cam.pos;
    enum { SEGS = 96, PIECE = 12 };
    RV v[2 * (PIECE + 1)];
    const uint32_t haze = argb(255, 196, 212, 224);
    const float R = 1420.0f;
    r_hdr(h_hills);
    for(int p0 = 0; p0 < SEGS; p0 += PIECE) {
        float am = (p0 + PIECE * 0.5f) / SEGS * DK_TAU;
        V3 mid = v3(c.x + sinf(am) * R, 60.0f, c.z + cosf(am) * R);
        if(!r_sphere_visible(mid, R * sinf(DK_PI * PIECE / SEGS) + 120.0f)) continue;
        int n = 0;
        for(int i = p0; i <= p0 + PIECE; i++) {
            float a = (float)i / SEGS * DK_TAU, h = sf_range_h(a);
            float x = c.x + sinf(a) * R, z = c.z + cosf(a) * R;
            float green = clampf(h / 120.0f, 0.0f, 1.0f);
            uint32_t top = color_lerp(argb(255, 168, 172, 168), argb(255, 132, 140, 98), green);
            v[n++] = (RV){x, h, z, 0, 0, color_lerp(top, haze, 0.35f)};
            v[n++] = (RV){x, -6.0f, z, 0, 0, haze};
        }
        r_strip(v, n);
    }
}

/* The ferry: round and round a long loop across the bay in front of
   Alcatraz, east along the near lane and back west along the far one,
   at a commuter catamaran's pace. Scenery on world_time, so it may be
   anywhere on its loop (and differ between consoles online). */
#define SF_FERRY_Z0   200.0f     /* the near lane, eastbound */
#define SF_FERRY_Z1   360.0f     /* the far lane, westbound */
#define SF_FERRY_XW   (-620.0f)  /* where it turns at each end */
#define SF_FERRY_XE   520.0f
#define SF_FERRY_V    11.0f      /* m/s, about 21 knots */
#define SF_WAKE_N     10

/* Where the ferry is u metres round its loop, and its heading. */
static V3 sf_ferry_at(float u, float *heading) {
    const float R = (SF_FERRY_Z1 - SF_FERRY_Z0) * 0.5f, A = SF_FERRY_XE - SF_FERRY_XW, T = DK_PI * R;
    u = fmodf(u, 2.0f * (A + T));
    if(u < 0) u += 2.0f * (A + T);
    if(u < A) {
        *heading = DK_PI * 0.5f;
        return v3(SF_FERRY_XW + u, 0, SF_FERRY_Z0);
    }
    u -= A;
    if(u < T) {
        float a = u / R;
        *heading = DK_PI * 0.5f - a;
        return v3(SF_FERRY_XE + R * sinf(a), 0, SF_FERRY_Z0 + R - R * cosf(a));
    }
    u -= T;
    if(u < A) {
        *heading = -DK_PI * 0.5f;
        return v3(SF_FERRY_XE - u, 0, SF_FERRY_Z1);
    }
    u -= A;
    float a = u / R;
    *heading = -DK_PI * 0.5f - a;
    return v3(SF_FERRY_XW - R * sinf(a), 0, SF_FERRY_Z1 - R + R * cosf(a));
}

static float sf_ferry_u(void) {
    return world_time * SF_FERRY_V + 900.0f;   /* a little way along the near lane at the start */
}

/* The ferry's baked mesh, lit each frame for its heading, riding a slow
   swell. */
void sf_draw_ferry(void) {
    if(world_split) return;   /* a split screen's CPU time goes on the hazards and the road */
    float h;
    V3 p = sf_ferry_at(sf_ferry_u(), &h);
    V3 dc = vsub(p, r_cam.pos);
    if(vdot(dc, dc) > 1300.0f * 1300.0f || !r_sphere_visible(vadd(p, v3(0, 5.0f, 0)), 24.0f)) return;
    const dk_prop_t *m = &sf_props[SFP_FERRY];
    float cs = cosf(h), sn = sinf(h);
    float pitch = 0.012f * sinf(world_time * 0.7f), roll = 0.018f * sinf(world_time * 0.53f + 1.0f);
    p.y = WATER_Y - 0.3f + 0.12f * sinf(world_time * 0.9f);
    /* The sun in the ferry's own frame, for lighting its normals. */
    V3 L = world_sun_dir, Ll = v3(L.x * cs - L.z * sn, L.y, L.x * sn + L.z * cs);
    r_hdr(h_col);
    RV buf[240];
    int n = 0, at = 0;
    for(int st = 0; st < m->nstrips; st++) {
        int len = m->strip_len[st];
        if(n && n + len + 2 > 240) {
            r_strip(buf, n);
            n = 0;
        }
        for(int q = 0; q < len; q++) {
            const dk_prop_vert_t *v = &m->verts[m->index[at + q]];
            float x = v->x * 0.01f, y = v->y * 0.01f, z = v->z * 0.01f;
            y += z * pitch + x * roll;
            float l = 0.66f + 0.42f * fmaxf(0.0f, (v->nx * Ll.x + v->ny * Ll.y + v->nz * Ll.z) * (1.0f / 127.0f));
            RV r = {p.x + x * cs + z * sn, p.y + y, p.z - x * sn + z * cs, 0, 0, color_scale(argb(255, v->r, v->g, v->b), l)};
            if(q == 0 && n) {
                buf[n] = buf[n - 1];
                buf[n + 1] = r;
                n += 2;
            }
            buf[n++] = r;
        }
        at += len;
    }
    if(n >= 3) r_strip(buf, n);
}

/* Its wake: a fading band of churned water astern along the way it has
   come, spreading as it goes, and the bow wave either side. */
void sf_draw_ferry_wake(void) {
    if(world_split) return;
    float u = sf_ferry_u(), h;
    V3 p = sf_ferry_at(u, &h);
    V3 dc = vsub(p, r_cam.pos);
    if(vdot(dc, dc) > 1300.0f * 1300.0f || !r_sphere_visible(p, 160.0f)) return;
    RV v[2 * (SF_WAKE_N + 1)];
    for(int k = 0; k <= SF_WAKE_N; k++) {
        float back = 16.0f + 14.0f * (float)k, hk;
        V3 c = sf_ferry_at(u - back, &hk);
        V3 side = v3(cosf(hk), 0, -sinf(hk));
        float w = 5.5f + 0.9f * (float)k, a = 150.0f * (1.0f - (float)k / SF_WAKE_N);
        uint32_t col = argb((int)a, 236, 244, 246);
        V3 l = vmad(c, side, -w), r = vmad(c, side, w);
        l.y = r.y = WATER_Y + 0.05f;
        v[2 * k] = (RV){l.x, l.y, l.z, 0, 0, col};
        v[2 * k + 1] = (RV){r.x, r.y, r.z, 0, 0, col};
    }
    r_hdr(h_sfwake);
    r_strip(v, 2 * (SF_WAKE_N + 1));
}

/* The fog bank coming in through the Golden Gate: wide soft heaps over
   the water at the bridge's feet, drifting in from the ocean and thinning
   out into the bay. */
#define SF_FOG_N 9
void sf_draw_fog(void) {
    V3 b = v3(SF_BRIDGE_X, 0, SF_BRIDGE_Z);
    V3 along = v3(sinf(SF_BRIDGE_YAW), 0, cosf(SF_BRIDGE_YAW));
    V3 across = v3(cosf(SF_BRIDGE_YAW), 0, -sinf(SF_BRIDGE_YAW));   /* out to the ocean is -across */
    if(!r_sphere_visible(b, 700.0f)) return;
    r_hdr(h_sffog);
    for(int i = 0; i < SF_FOG_N; i += world_split ? 2 : 1) {
        uint32_t hsh = (uint32_t)i * 2654435761u + 77u;
        float run = 900.0f, speed = 6.0f + (float)(hsh & 7) * 0.5f;
        float t = fmodf(world_time * speed + (float)((hsh >> 4) & 1023), run);
        float off = -420.0f + t;   /* from the ocean side in through the strait */
        float lat = -320.0f + 640.0f * (float)i / (SF_FOG_N - 1) + (float)((hsh >> 12) & 63) - 32.0f;
        V3 p = vadd(b, vadd(vscale(across, off), vscale(along, lat)));
        float fade = smooth01(0.0f, 120.0f, t) * smooth01(run, run - 300.0f, t);
        float w = 340.0f + (float)((hsh >> 18) & 63) * 3.0f, h = 55.0f + (float)((hsh >> 22) & 15) * 2.0f;
        p.y = h * 0.42f;
        if(!r_sphere_visible(p, w)) continue;
        r_billboard_y(v3(p.x, -4.0f, p.z), w, h, 0, 0, 1, 1, argb((int)(170.0f * fade), 255, 255, 255));
    }
}

/* ---- The build ---- */

void build_sf(void) {
    f8m_default();
    h_sheen = h_sfroad;
    f8m.wall_h = 1.0f;
    f8m.barrier_u = 4.0f;
    f8m.shoulder_uv = 6.0f;
    f8m.half = SF_HALF;
    f8m.edge = SF_HALF;   /* no curb strip drawn: the sidewalk (sf_kerbs) */
    f8m.ground = sf_terrain;
    f8m.skirt_ground = true;
    sf_houses_n = sf_sheds_n = sf_row_n = sf_solid_n = sf_back_n = 0;
    sf_cross_s[0] = track_seg_s(SF_HYDE_SEG) + 65.5f;
    sf_cross_s[1] = sf_cross_s[0] + 59.0f;
    sf_ground_init();
    sf_park_init();
#ifdef DK_CHUNK_STATS
    ck_first[CK_GROUND] = chunk_n;   /* the kinds here: ground, road ("deck"), houses ("rock"), props */
#endif
    sf_build_ground();
    sf_park();
    sf_quay();
    sf_promenade();
#ifdef DK_CHUNK_STATS
    ck_first[CK_DECK] = chunk_n;
#endif
    for(int c = 0; c < TRACK_SAMPLES / 8; c++) {
        sf_road_style(c);
        f8m.split_lod = true;
        f8_build_track_chunk(c);
        f8_build_deck_coarse(c);
    }
    f8m.split_lod = false;
    f8m.wall_h = 1.0f;
    sf_hyde_rails();
    sf_pier();
    sf_beds();
    sf_terraces();
    for(int k = 0; k < 2; k++)
        for(int side = -1; side <= 1; side += 2) sf_cross_street(sf_cross_s[k], side, k == 1 && side < 0);
#ifdef DK_CHUNK_STATS
    ck_first[CK_ROCK] = chunk_n;
#endif
    sf_street(-1);
    sf_street(1);
    sf_city_fill();
#ifdef DK_CHUNK_STATS
    ck_first[CK_PROPS] = chunk_n;
    ck_first[CK_TORCH] = ck_first[CK_N] = 100000;
#endif
    sf_place_props();
    sf_cablecar_bake();
}
