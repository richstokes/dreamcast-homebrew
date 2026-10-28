/* Tsudoken Drift: a flat club circuit on the Kanto plain at golden hour:
   paddies to the horizon, Mt Fuji in the south-west with the sun setting
   beside it, cherry trees in blossom, tyre walls, a grandstand under a
   swept tiled roof along the start straight, race control and the pit
   garages opposite, torii over the road, stone lanterns at the bends, a
   pagoda beyond turn 1, teahouses, and a stream that runs under the jump
   into a koi pond in the infield, crossed by a red footbridge. The props
   are meshes baked by tools/build_tsudoken_props.py (Blender), instanced
   here. */
#include "tsudoken_props.h"
#include "world_int.h"

#define TK_LANTERNS   24
#define TK_PETALS     60
#define TK_STREAM_PTS 8
#define TK_WATER_Y    (-1.3f)
#define TK_CHANNEL_D  2.2f       /* stream depth */
#define TK_CHANNEL_HW 5.0f       /* flat bed half width; banks ease over TK_BANK */
#define TK_BANK       6.0f
#define TK_POND_R     12.0f
#define TK_TSUKUBA_DIR (38.0f * DK_PI / 180.0f)   /* Mt Tsukuba's twin peaks on the hills ring */
static const pvr_poly_hdr_t *tk_mat_hdr[TKM_COUNT];
static V3 tk_stream[TK_STREAM_PTS];     /* the stream's course, control points, the plain to the pond */
#define TK_PATH_PTS 72
static V3 tk_path[TK_PATH_PTS];         /* the course as a smooth curve through them */
static int tk_path_n;
static V3 tk_pond;
static V3 tk_lantern[TK_LANTERNS];      /* the fire boxes, for the glows */
static int tk_lantern_n;
/* The mountain's vertex colours, lit and hazed, one per strip vertex of
   the baked mesh (the positions come straight from the header each frame:
   the console is within a megabyte of its 16 MB, so no RV copy). */
static uint32_t tk_fuji_col[2000], tk_fuji_lo_col[640];
static float tk_fuji_scale;

static const pvr_poly_hdr_t *h_tkroad, *h_tkcurb, *h_verge, *h_paddy, *h_hoarding, *h_ishigaki, *h_kawara, *h_sakura,
    *h_sakurafar, *h_sakurabark, *h_petal, *h_tkcrowd, *h_plaster, *h_tkbanner, *h_pitwall, *h_stream;

/* The headers only Tsudoken draws with, and its baked props' materials. */
void tk_init(void) {
    h_tkroad = r_header(TEX_TKROAD, HF_OP);
    h_tkcurb = r_header(TEX_TKCURB, HF_OP);
    h_verge = r_header(TEX_VERGE, HF_OP);
    h_paddy = r_header(TEX_PADDY, HF_OP);
    h_hoarding = r_header(TEX_HOARDING, HF_OP);
    h_ishigaki = r_header(TEX_ISHIGAKI, HF_OP);
    h_kawara = r_header(TEX_KAWARA, HF_OP);
    h_sakura = r_header(TEX_SAKURA, HF_PT);
    h_sakurafar = r_header(TEX_SAKURAFAR, HF_PT);
    h_sakurabark = r_header(TEX_SAKURABARK, HF_OP);
    h_petal = r_header(TEX_PETAL, HF_TR);
    h_tkcrowd = r_header(TEX_TKCROWD, HF_OP);
    h_plaster = r_header(TEX_PLASTER, HF_OP);
    h_tkbanner = r_header(TEX_TKBANNER, HF_OP | HF_CLAMP);
    h_pitwall = r_header(TEX_PITWALL, HF_OP);
    h_stream = r_header(TEX_STREAM, HF_OP);
    /* The baked props' materials, in the header's TKM_* order. */
    tk_mat_hdr[TKM_FLAT] = h_col;
    tk_mat_hdr[TKM_KAWARA] = h_kawara;
    tk_mat_hdr[TKM_CROWD] = h_tkcrowd;
    tk_mat_hdr[TKM_ISHIGAKI] = h_ishigaki;
    tk_mat_hdr[TKM_SAKURA] = h_sakura;
    tk_mat_hdr[TKM_BARK] = h_sakurabark;
    tk_mat_hdr[TKM_PLASTER] = h_plaster;
    tk_mat_hdr[TKM_BANNER] = h_tkbanner;
    tk_mat_hdr[TKM_PITWALL] = h_pitwall;
}

/* The stream's channel and the pond, carved into the plain: depth 0 at
   TK_CHANNEL_HW + TK_BANK from the centreline, the full depth within
   TK_CHANNEL_HW. */
static float tk_water_cut(float x, float z) {
    V3 p = v3(x, 0, z);
    float d = 1e9f;
    for(int i = 0; i + 1 < tk_path_n; i++) d = fminf(d, seg_dist(p, tk_path[i], tk_path[i + 1]));
    float dp = sqrtf((x - tk_pond.x) * (x - tk_pond.x) + (z - tk_pond.z) * (z - tk_pond.z)) - (TK_POND_R - TK_CHANNEL_HW);
    d = fminf(d, dp);
    return -TK_CHANNEL_D * (1.0f - smooth01(TK_CHANNEL_HW, TK_CHANNEL_HW + TK_BANK, d));
}

/* The plain: flat round the circuit, rolling gently beyond 150 m, with
   the stream and the pond cut in. */
float tk_terrain(float x, float z) {
    float r = sqrtf(x * x + z * z);
    float roll = smooth01(150.0f, 420.0f, r) *
                 (5.0f + 3.0f * sinf(x * 0.013f + 0.4f) + 2.5f * sinf(z * 0.011f + 1.3f) + 2.0f * sinf((x + z) * 0.007f));
    return roll + tk_water_cut(x, z) + F8_GRASS_Y;
}

static bool tk_clear(float x, float z, float margin) {
    if(sk_road_dist(v3(x, 0, z)) < 12.5f + margin) return false;
    if(tk_water_cut(x, z) < -0.01f) return false;
    return true;
}

static void tk_stream_init(void) {
    float cross = track.jump.cross;
    V3 c = track_point(cross, 0.0f);
    V3 t = track_tangent(cross), r = v3(t.z, 0, -t.x);
    /* The circuit runs clockwise, so the infield is on the right (+r) of
       the back straight and open country on the left. Out there the
       stream runs off to the plain, straight under the gap and meandering
       beyond; on the infield side it turns to the pond. */
    tk_stream[0] = vmad(vmad(c, r, -320.0f), t, -40.0f);
    tk_stream[1] = vmad(vmad(c, r, -230.0f), t, 22.0f);
    tk_stream[2] = vmad(vmad(c, r, -150.0f), t, -16.0f);
    tk_stream[3] = vmad(c, r, -70.0f);
    tk_stream[4] = vmad(c, r, -22.0f);
    tk_stream[5] = vmad(c, r, 22.0f);
    /* The pond sits in the infield between turn 5's straight and the back
       straight. */
    float leg = track_seg_s(8) + 28.0f;
    tk_pond = track_point(leg, 35.0f);
    tk_pond.y = 0;
    tk_stream[6] = vlerp(vmad(c, r, 22.0f), tk_pond, 0.5f);
    tk_stream[6] = vmad(tk_stream[6], t, 8.0f);
    tk_stream[7] = tk_pond;
    /* The course as a Catmull-Rom curve through the control points, a
       station every ~8 m: the channel and the water follow it, so the
       bends are rounded rather than kinked (a kink pinched the water
       ribbon where the stream turns off toward the pond). */
    tk_path_n = 0;
    for(int i = 0; i + 1 < TK_STREAM_PTS; i++) {
        V3 p0 = tk_stream[i > 0 ? i - 1 : i], p1 = tk_stream[i], p2 = tk_stream[i + 1],
           p3 = tk_stream[i + 2 < TK_STREAM_PTS ? i + 2 : i + 1];
        int steps = (int)(vlen(vsub(p2, p1)) / 8.0f) + 1;
        for(int k = 0; k < steps && tk_path_n < TK_PATH_PTS - 1; k++) {
            float u = (float)k / steps, u2 = u * u, u3 = u2 * u;
            V3 q = vscale(p1, 2.0f);
            q = vmad(q, vsub(p2, p0), u);
            q = vmad(q, vadd(vsub(vadd(vscale(p0, 2.0f), vscale(p2, 4.0f)), vscale(p1, 5.0f)), vscale(p3, -1.0f)), u2);
            q = vmad(q, vadd(vsub(vscale(p1, 3.0f), vscale(p2, 3.0f)), vsub(p3, p0)), u3);
            tk_path[tk_path_n++] = vscale(q, 0.5f);
        }
    }
    tk_path[tk_path_n++] = tk_stream[TK_STREAM_PTS - 1];
}

/* The ground: an 8 m grid over the circuit (fine enough for the stream's
   banks), coarser bands out to 400 m, then a flat plain to the horizon,
   all paddies; the stream bed and the pond floor darken to mud. */
#define TK_GN 40
#define TK_GSTEP 10.0f
static float tk_gy[TK_GN][TK_GN];

static uint32_t tk_ground_color(float y, float x, float z) {
    float cut = tk_water_cut(x, z);
    uint32_t c = argb(255, 255, 255, 255);
    if(cut < -0.05f) c = color_lerp(c, argb(255, 120, 96, 70), clampf(-cut / TK_CHANNEL_D, 0.0f, 1.0f));
    return c;
}

static void tk_build_ground(void) {
    const float g0 = -TK_GSTEP * (TK_GN - 1) * 0.5f;
    for(int j = 0; j < TK_GN; j++)
        for(int i = 0; i < TK_GN; i++) tk_gy[j][i] = tk_terrain(g0 + i * TK_GSTEP, g0 + j * TK_GSTEP);
    for(int cz = 0; cz + 1 < TK_GN; cz += 4) {
        for(int cx = 0; cx + 1 < TK_GN; cx += 8) {
            chunk_begin(HF_OP, 2000.0f);
            for(int j = cz; j < cz + 4 && j + 1 < TK_GN; j++) {
                strip_begin(h_paddy);
                for(int i = cx; i <= cx + 8 && i < TK_GN; i++) {
                    for(int k = 1; k >= 0; k--) {
                        float x = g0 + i * TK_GSTEP, z = g0 + (j + k) * TK_GSTEP, y = tk_gy[j + k][i];
                        uint32_t col = tk_ground_color(y, x, z);
                        if(i + 1 < TK_GN) {
                            float gx = (tk_gy[j + k][i + 1] - y) / TK_GSTEP;
                            col = color_scale(col, clampf(1.0f + gx * 0.6f, 0.7f, 1.1f));
                        }
                        svp(v3(x, y, z), x / 24.0f, z / 24.0f, col);
                    }
                }
                strip_end();
            }
            chunk_end();
        }
    }
    /* Coarser rings of the rolling plain, then the flat horizon. */
    const float G = -g0, E = 3000.0f;
    static const float rings[] = {0.0f, 40.0f, 90.0f, 150.0f, 220.0f};
    const int nr = (int)(sizeof(rings) / sizeof(rings[0]));
    /* Drawn in a split screen too (a few hundred vertices): the plain's
       edge at 195 m is well inside the split screen's fog. */
    for(int side = 0; side < 4; side++) {
        chunk_begin(HF_OP, 4000.0f);
        for(int b = 0; b + 1 < nr; b++) {
            strip_begin(h_paddy);
            for(int k = 0; k <= 16; k++) {
                float t = -1.0f + 2.0f * (float)k / 16.0f;
                for(int q = 1; q >= 0; q--) {
                    float e = G + rings[b + q];
                    float x, z;
                    if(side == 0) { x = t * e; z = e; }
                    else if(side == 1) { x = t * e; z = -e; }
                    else if(side == 2) { x = e; z = t * (e - 0.001f); }
                    else { x = -e; z = t * (e - 0.001f); }
                    float y = tk_terrain(x, z);
                    svp(v3(x, y, z), x / 24.0f, z / 24.0f, 0xffffffff);
                }
            }
            strip_end();
        }
        chunk_end();
    }
    chunk_begin(HF_OP, 6000.0f);
    {
        const float R = G + rings[nr - 1], y = tk_terrain(R, R);
        quad(h_paddy, v3(-E, y, E), v3(E, y, E), v3(-E, y, R), v3(E, y, R), -E / 24, E / 24, E / 24, R / 24, 0xffffffff);
        quad(h_paddy, v3(-E, y, -R), v3(E, y, -R), v3(-E, y, -E), v3(E, y, -E), -E / 24, -R / 24, E / 24, -E / 24, 0xffffffff);
        quad(h_paddy, v3(-E, y, R), v3(-R, y, R), v3(-E, y, -R), v3(-R, y, -R), -E / 24, R / 24, -R / 24, -R / 24, 0xffffffff);
        quad(h_paddy, v3(R, y, R), v3(E, y, R), v3(R, y, -R), v3(E, y, -R), R / 24, R / 24, E / 24, -R / 24, 0xffffffff);
    }
    chunk_end();
    /* The water: a ribbon down the stream and a disc on the pond. */
    chunk_begin(HF_OP, 1200.0f);
    /* The water ribbon: at each station the cross-section bisects the
       two segments meeting there (mitred, so the width holds round a
       bend). In pieces, so the chunks can be culled. */
    for(int i0 = 0; i0 + 1 < tk_path_n; i0 += 12) {
        strip_begin(h_stream);
        for(int i = i0; i <= i0 + 12 && i < tk_path_n; i++) {
            V3 a = tk_path[i > 0 ? i - 1 : i], b = tk_path[i], c2 = tk_path[i + 1 < tk_path_n ? i + 1 : i];
            V3 d = vnorm(vadd(vnorm(v3(b.x - a.x, 0, b.z - a.z)), vnorm(v3(c2.x - b.x, 0, c2.z - b.z))));
            V3 r = v3(d.z, 0, -d.x);
            float hw = TK_CHANNEL_HW + TK_BANK * 0.55f;
            V3 l = vmad(b, r, -hw), rr = vmad(b, r, hw);
            l.y = rr.y = TK_WATER_Y;
            svp(l, l.x / 12.0f, l.z / 12.0f, 0xffffffff);
            svp(rr, rr.x / 12.0f, rr.z / 12.0f, 0xffffffff);
        }
        strip_end();
    }
    strip_begin(h_stream);
    const int sides = 14;
    for(int k = 0; k < sides; k++) {
        int i = (k & 1) ? (sides - (k + 1) / 2) % sides : k / 2;
        float a = (float)i / sides * DK_TAU;
        float rr = TK_POND_R + TK_BANK * 0.55f;
        V3 p = v3(tk_pond.x + sinf(a) * rr, TK_WATER_Y, tk_pond.z + cosf(a) * rr);
        svp(p, p.x / 12.0f, p.z / 12.0f, 0xffffffff);
    }
    strip_end();
    chunk_end();
}

/* Tsudoken's solid scenery, for the spectator's cameras and sight lines
   (world_solid): the upright boxes each prop is baked with (round a
   building or lantern, a torii's posts and beams, a tree's crown). */
#define TK_SOLIDS 192
typedef struct {
    float x, z, cs, sn;     /* centre, and the yaw its prop was turned by */
    float hx, hz, y0, y1;   /* half extents across and along, and height */
} TkSolid;
static TkSolid tk_solid[TK_SOLIDS];
static int tk_solid_n;

/* A box over the model-unit extents x0..x1, y0..y1, z0..z1 of a prop at
   p turned as tk_prop turns it. */
static void tk_solid_add(V3 p, float cs, float sn, float unit, float x0, float x1, float y0, float y1, float z0, float z1) {
    if(tk_solid_n >= TK_SOLIDS) return;
    float cx = (x0 + x1) * 0.5f * unit, cz = (z0 + z1) * 0.5f * unit;
    tk_solid[tk_solid_n++] = (TkSolid){p.x + cx * cs + cz * sn, p.z - cx * sn + cz * cs, cs, sn, (x1 - x0) * 0.5f * unit,
                                       (z1 - z0) * 0.5f * unit, p.y + y0 * unit, p.y + y1 * unit};
}

/* The prop's baked solid boxes (tools/build_tsudoken_props.py). */
static void tk_prop_solids(const dk_prop_t *m, V3 p, float yaw, float scale) {
    float cs = cosf(yaw), sn = sinf(yaw), unit = (m->cm ? 0.01f : 0.001f) * scale;
    for(int i = 0; i < m->nsolids; i++) {
        const dk_prop_box_t *b = &m->solids[i];
        tk_solid_add(p, cs, sn, unit, b->x0, b->x1, b->y0, b->y1, b->z0, b->z1);
    }
}

static void tk_prop(int which, V3 p, float yaw, float scale, float lod_far, bool far) {
    tk_prop_solids(&dk_props[which], p, yaw, scale);
    prop_place(&dk_props[which], tk_mat_hdr, TKM_PT_MASK, p, yaw, scale, lod_far, far);
}

/* A torii over the road at lap distance s: the gate itself, and at the
   line the course banner hung between its pillars. */
static void tk_torii(float s, float scale, bool banner) {
    V3 c = track_point(s, 0.0f);
    V3 t = track_tangent(s);
    float yaw = atan2f(t.x, t.z);
    c.y = F8_GRASS_Y;
    tk_prop(TKP_TORII, c, yaw, scale, 1400.0f, false);
    if(banner) {
        V3 r = v3(t.z, 0, -t.x);
        float hw = 2.9f * scale, y1 = 4.1f * scale, y0 = y1 - 1.45f * scale;   /* under the tie beam, between the pillars */
        V3 l = vmad(c, r, -hw), rr = vmad(c, r, hw);
        chunk_begin(HF_OP, 1400.0f);
        for(int f = -1; f <= 1; f += 2) {
            V3 o = vscale(t, 0.12f * f);
            V3 a = vadd(vadd(f < 0 ? l : rr, o), v3(0, y1, 0)), b2 = vadd(vadd(f < 0 ? rr : l, o), v3(0, y1, 0));
            V3 cc = vadd(vadd(f < 0 ? l : rr, o), v3(0, y0, 0)), d = vadd(vadd(f < 0 ? rr : l, o), v3(0, y0, 0));
            strip_begin(h_tkbanner);
            svp(a, 0, 0, 0xffffffff); svp(b2, 1, 0, 0xffffffff);
            svp(cc, 0, 0.5f, 0xffffffff); svp(d, 1, 0.5f, 0xffffffff);
            strip_end();
        }
        chunk_end();
    }
    if(world_gates < WORLD_MAX_GATES) world_gate_s[world_gates++] = track_wrap(s);
}

static void tk_lantern_at(float s, float d) {
    if(tk_lantern_n >= TK_LANTERNS) return;
    V3 p = track_point(s, d);
    if(!tk_clear(p.x, p.z, -11.0f)) return;
    p.y = tk_terrain(p.x, p.z);
    V3 t = track_tangent(s);
    tk_prop(TKP_LANTERN, p, atan2f(t.x, t.z), 1.0f, 240.0f, false);
    tk_lantern[tk_lantern_n++] = vadd(p, v3(0, 1.92f, 0));
}

/* A cherry tree, if the spot is clear of the road and the water. */
static int tk_trees;
static void tk_sakura(float x, float z, uint32_t seed) {
    if(!tk_clear(x, z, 3.0f)) return;
    float scale = 1.5f + (float)((seed >> 8) & 7) * 0.08f;
    V3 p = v3(x, tk_terrain(x, z), z);
    /* A split screen draws everything twice: it leaves out every third
       tree and sees the rest less far. */
    tk_prop((seed & 1) ? TKP_SAKURA_B : TKP_SAKURA_A, p, (float)((seed >> 4) & 255) / 255.0f * DK_TAU, scale, 300.0f,
            tk_trees % 3 == 2);
    tk_trees++;
}

static void tk_place_props(void) {
    tk_solid_n = 0;
    tk_lantern_n = 0;
    tk_trees = 0;
    world_gates = 0;
    const TrackSample *t0 = &track.smp[0];
    float h0 = atan2f(t0->t.x, t0->t.z);

    /* The line: a great torii with the banner, the chequered strip. */
    tk_torii(0.0f, 3.1f, true);
    chunk_begin(HF_OP, 600.0f);
    strip_begin(h_checker);
    for(int k = 0; k <= 1; k++) {
        float s = (k ? 1.2f : -1.2f);
        V3 a = track_point(s, -8.0f), b = track_point(s, 8.0f);
        a.y = b.y = 0.025f;
        svp(a, 0, s / 1.2f, 0xffffffff);
        svp(b, 8, s / 1.2f, 0xffffffff);
    }
    strip_end();
    chunk_end();

    /* The grandstand along the outside of the start straight, five
       modules facing the road; race control and the pits opposite. */
    for(int i = 0; i < 5; i++) {
        float s = -27.0f + 24.0f * (float)i;
        V3 p = track_point(s, -17.0f);
        p.y = F8_GRASS_Y;
        tk_prop(TKP_STAND, p, h0 - DK_PI * 0.5f, 1.0f, 1600.0f, false);
        tk_prop(TKP_CHOCHIN, p, h0 - DK_PI * 0.5f, 1.0f, 500.0f, true);
    }
    /* The infield here is only ~9 m wide between the two barriers (turn
       1's exit runs back down beside the start straight), so race control
       and the pits are slim and sit right behind the tyre wall. */
    {
        V3 p = track_point(-4.0f, 16.4f);
        p.y = F8_GRASS_Y;
        tk_prop(TKP_TOWER, p, h0 + DK_PI * 0.5f, 1.0f, 1600.0f, false);
    }
    for(int i = 0; i < 3; i++) {
        V3 p = track_point(22.0f + 24.0f * (float)i, 14.2f);
        p.y = F8_GRASS_Y;
        tk_prop(TKP_PITS, p, h0 + DK_PI * 0.5f, 1.0f, 1400.0f, false);
    }

    /* Torii over the road: out of the hairpin at turn 4, and framing the
       run at the jump. */
    tk_torii(track_seg_s(8) + 26.0f, 2.4f, false);
    tk_torii(track_wrap(track.jump.cross - 76.0f), 2.4f, false);

    /* The pagoda beyond turn 1, teahouses outside turns 8 and 9 and by
       the stream, the footbridge over the stream east of the road. */
    {
        V3 p = track_point(track_seg_s(1) + 30.0f, -58.0f);
        p.y = tk_terrain(p.x, p.z);
        tk_prop(TKP_PAGODA, p, h0 + 0.3f, 1.0f, 2000.0f, true);
    }
    {
        float s8 = track_seg_s(15) + 30.0f;
        V3 p = track_point(s8, -34.0f), q = track_point(s8, 0.0f);
        p.y = tk_terrain(p.x, p.z);
        tk_prop(TKP_TEAHOUSE, p, atan2f(q.x - p.x, q.z - p.z), 1.0f, 900.0f, true);
    }
    {
        float s9 = track_seg_s(17) + 75.0f;
        V3 p = track_point(s9, -40.0f), q = track_point(s9, 0.0f);
        p.y = tk_terrain(p.x, p.z);
        tk_prop(TKP_TEAHOUSE, p, atan2f(q.x - p.x, q.z - p.z), 1.0f, 900.0f, true);
    }
    {
        /* Out on the open side of the back straight (left, -r). */
        V3 c = track_point(track.jump.cross, 0.0f), t = track_tangent(track.jump.cross), r = v3(t.z, 0, -t.x);
        V3 p = vmad(c, r, -48.0f);
        p.y = F8_GRASS_Y;
        tk_prop(TKP_BRIDGE, p, atan2f(t.x, t.z) + DK_PI * 0.5f, 1.0f, 900.0f, true);
        V3 th = vmad(vmad(c, r, -52.0f), t, 26.0f), q = vmad(c, r, -48.0f);
        th.y = tk_terrain(th.x, th.z);
        tk_prop(TKP_TEAHOUSE, th, atan2f(q.x - th.x, q.z - th.z), 1.0f, 900.0f, true);
    }

#ifdef DK_CHUNK_STATS
    ck_first[CK_PROPS] = chunk_n;
    ck_first[CK_TORCH] = ck_first[CK_N] = 100000;   /* unused kinds */
#endif
    /* Stone lanterns: three round the outside of every bend, and a pair
       by the line. */
    for(int i = 0; i < track_seg_count(); i++) {
        float r = track_seg_radius(i), deg = track_seg_deg(i);
        if(r <= 0) continue;
        float s0 = track_seg_s(i), len = r * fabsf(deg) * (DK_PI / 180.0f);
        float side = deg > 0 ? -1.0f : 1.0f;   /* the outside of a right turn is the left */
        for(int k = 0; k < 2; k++) tk_lantern_at(track_wrap(s0 + len * (0.25f + 0.5f * k)), side * 15.6f);
    }
    tk_lantern_at(-12.0f, 15.6f);
    tk_lantern_at(12.0f, 15.6f);

    /* Cherry trees: an avenue down both sides of the back straight (clear
       of the jump), a line behind the grandstand, groves round the pagoda,
       the pond and the teahouses, and a scatter beyond. */
    uint32_t seed = 0x5a1cu;
    float back = track_seg_s(TK_BACK_SEG), back_len = track_seg_s(TK_BACK_SEG + 1) - back;
    for(float u = 10.0f; u < back_len - 10.0f; u += 17.0f) {
        float s = back + u;
        if(fabsf(track_delta(track.jump.cross, s)) < 44.0f) continue;
        for(int side = -1; side <= 1; side += 2) {
            seed = seed * 1664525u + 1013904223u;
            V3 p = track_point(s, side * 20.5f);
            tk_sakura(p.x, p.z, seed);
        }
    }
    for(float s = -36.0f; s < 95.0f; s += 19.0f) {
        seed = seed * 1664525u + 1013904223u;
        V3 p = track_point(s, -38.0f + (float)((seed >> 12) & 3));
        tk_sakura(p.x, p.z, seed);
    }
    for(int i = 0; i < 8; i++) {
        seed = seed * 1664525u + 1013904223u;
        float a = (float)i / 8.0f * DK_TAU;
        V3 p = track_point(track_seg_s(1) + 30.0f, -58.0f);
        tk_sakura(p.x + sinf(a) * 20.0f, p.z + cosf(a) * 20.0f, seed);
    }
    for(int i = 0; i < 6; i++) {
        seed = seed * 1664525u + 1013904223u;
        float a = (float)i / 6.0f * DK_TAU + 0.3f;
        tk_sakura(tk_pond.x + sinf(a) * (TK_POND_R + 9.0f), tk_pond.z + cosf(a) * (TK_POND_R + 9.0f), seed);
    }
    for(int tries = 0; tries < 400 && tk_trees < 58; tries++) {
        seed = seed * 1664525u + 1013904223u;
        float x = -260.0f + (float)((seed >> 8) & 1023) / 1023.0f * 520.0f;
        float z = -240.0f + (float)((seed >> 18) & 1023) / 1023.0f * 480.0f;
        if(sk_road_dist(v3(x, 0, z)) < 36.0f) continue;
        tk_sakura(x, z, seed);
    }
    /* Far trees as sprites out on the plain. */
    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t h = (uint32_t)i * 2654435761u + 77u;
        float a = (float)(h & 1023) / 1023.0f * DK_TAU, d = 230.0f + (float)((h >> 10) & 255);
        float x = sinf(a) * d, z = cosf(a) * d;
        far_sprites[far_sprite_n] = v3(x, tk_terrain(x, z), z);
        far_sprite_h[far_sprite_n] = 9.0f + (float)((h >> 18) & 7);
        far_sprite_n++;
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DreamKarts tsudoken props: %d cherry trees, %d lanterns, %d torii\n", tk_trees, tk_lantern_n, world_gates);
#endif
}

/* Mt Fuji: the baked unit mountain lit and scaled, kept about the origin;
   drawn each frame at a fixed bearing from the camera, so it never comes
   nearer, beyond the fog. It goes in the opaque list: nothing culls its
   back faces, and in the translucent list (drawn in submission order,
   autosort off) they painted dark wedges over the front. */
static void tk_fuji_bake(const dk_prop_t *m, uint32_t *out, int out_max) {
    int at = 0;
    for(int s = 0; s < m->nstrips; s++) {
        int n = m->strip_len[s];
        if(at + n > out_max) break;
        for(int q = 0; q < n; q++) {
            const dk_prop_vert_t *v = &m->verts[m->index[at + q]];
            V3 nn = vnorm(v3(v->nx, v->ny, v->nz));
            /* Lit softly from the sun's side, the haze keeping it flat. */
            float l = 0.80f + 0.26f * fmaxf(0.0f, vdot(nn, world_sun_dir));
            /* Atmospheric perspective: the foot all but dissolves into the
               horizon's haze, the summit stands clearer. */
            uint32_t haze = TK_AFTERNOON ? argb(255, 189, 230, 248) : argb(255, 243, 184, 150);
            float hz = clampf(v->y / 1000.0f, 0.0f, 1.0f);
            float mix = TK_AFTERNOON ? lerpf(0.78f, 0.38f, hz) : lerpf(0.62f, 0.22f, hz);
            out[at + q] = color_lerp(sunset_grade(color_scale(argb(255, v->r, v->g, v->b), l)), haze, mix);
        }
        at += n;
    }
}

static void tk_fuji_init(void) {
    tk_fuji_scale = 290.0f;
    tk_fuji_bake(&dk_props[TKP_FUJI], tk_fuji_col, 2000);
    tk_fuji_bake(&dk_props[TKP_FUJI_LO], tk_fuji_lo_col, 640);
}

void tk_draw_fuji(void) {
    V3 c = r_cam.pos;
    V3 base = v3(c.x + sinf(TK_FUJI_DIR) * 1420.0f, -14.0f, c.z + cosf(TK_FUJI_DIR) * 1420.0f);
    if(!r_sphere_visible(vadd(base, v3(0, tk_fuji_scale * 0.5f, 0)), tk_fuji_scale * 2.4f)) return;
    r_hdr(h_sky);   /* opaque, no fog */
    const dk_prop_t *m = &dk_props[world_split ? TKP_FUJI_LO : TKP_FUJI];
    const uint32_t *col = world_split ? tk_fuji_lo_col : tk_fuji_col;
    int col_max = world_split ? 640 : 2000;
    float k = 0.001f * tk_fuji_scale;
    RV v[256];
    int at = 0;
    for(int s = 0; s < m->nstrips; s++) {
        int n = m->strip_len[s];
        if(at + n > col_max) break;
        for(int q = 0; q < n; q++) {
            const dk_prop_vert_t *p = &m->verts[m->index[at + q]];
            v[q] = (RV){base.x + p->x * k, base.y + p->y * k, base.z + p->z * k, 0, 0, col[at + q]};
        }
        r_strip(v, n);
        at += n;
    }
}

/* A band of haze across Fuji's lower slopes: one gradient strip at the
   mountain's distance, the horizon's colour, solid at its foot and gone
   two fifths of the way up, so the mountain rises out of mist rather
   than standing cut out against the sky. */
void tk_draw_mist(void) {
    V3 c = r_cam.pos;
    V3 d = v3(sinf(TK_FUJI_DIR), 0, cosf(TK_FUJI_DIR)), r = v3(d.z, 0, -d.x);
    V3 base = v3(c.x + d.x * 1400.0f, -14.0f, c.z + d.z * 1400.0f);
    float hw = tk_fuji_scale * 2.6f, top = tk_fuji_scale * 0.42f;
    if(!r_sphere_visible(vadd(base, v3(0, top * 0.5f, 0)), hw)) return;
    uint32_t haze = TK_AFTERNOON ? argb(255, 189, 230, 248) : argb(255, 243, 184, 150);
    r_hdr(h_hills);
    RV v[8];
    int n = 0;
    for(int i = 0; i <= 3; i++) {
        float t = (float)i / 3.0f;
        V3 p = vmad(base, r, lerpf(-hw, hw, t));
        /* Thinner toward the ends, so the band has no hard sides. */
        float side = 1.0f - 0.6f * fabsf(t - 0.5f) * 2.0f;
        v[n++] = (RV){p.x, p.y + top, p.z, 0, 0, color_alpha(haze, 0.0f)};
        v[n++] = (RV){p.x, p.y, p.z, 0, 0, color_alpha(haze, 0.85f * side)};
    }
    r_strip(v, n);
}

/* The horizon: two ranges of low hills all round in the evening haze,
   Mt Tsukuba's twin peaks rising in the north-east. */
void tk_draw_hills(void) {
    V3 c = r_cam.pos;
    enum { SEGS = 96, PIECE = 8 };
    RV v[2 * (PIECE + 1)];
    r_hdr(h_hills);
    /* Barely darker than the haze they stand in: the plain runs out to
       them fogged to the horizon's peach, so anything bolder reads as a
       wall. The foot is the fog colour exactly. */
    for(int layer = 0; layer < 2; layer++) {
        float R = 1360.0f - 50.0f * layer;
        uint32_t ridge = layer ? argb(255, 186, 136, 156) : argb(255, 214, 158, 158);
        uint32_t foot = argb(255, 243, 184, 150);
        if(TK_AFTERNOON) {
            ridge = layer ? argb(255, 128, 160, 184) : argb(255, 158, 190, 210);
            foot = argb(255, 189, 230, 248);
        }
        for(int p0 = 0; p0 < SEGS; p0 += PIECE) {
            float am = (p0 + PIECE * 0.5f) / SEGS * DK_TAU;
            V3 mid = v3(c.x + sinf(am) * R, 30.0f, c.z + cosf(am) * R);
            if(!r_sphere_visible(mid, R * sinf(DK_PI * PIECE / SEGS) + 80.0f)) continue;
            int n = 0;
            for(int i = p0; i <= p0 + PIECE; i++) {
                float a = (float)i / SEGS * DK_TAU;
                float h = 16.0f - 5.0f * layer + 6.0f * sinf(a * 3.0f + layer * 1.7f) + 3.5f * sinf(a * 7.0f + layer * 0.9f) +
                          2.0f * sinf(a * 13.0f + layer * 2.3f);
                /* Mt Tsukuba: two peaks a few degrees apart. */
                float da = wrap_angle(a - TK_TSUKUBA_DIR);
                if(layer == 1) h += 70.0f * expf(-da * da * 320.0f) + 60.0f * expf(-(da - 0.06f) * (da - 0.06f) * 420.0f);
                float x = c.x + sinf(a) * R, z = c.z + cosf(a) * R;
                v[n++] = (RV){x, h, z, 0, 0, ridge};
                v[n++] = (RV){x, -6.0f, z, 0, 0, foot};
            }
            r_strip(v, n);
        }
    }
}

/* Cherry petals drifting past the camera on the breeze: a fixed set of
   positions in a box that follows the camera, each falling and swaying.
   A petal is never culled while it can be seen: it wraps round the box
   only once it has faded out over TK_PETAL_BAND metres of each face
   (about five seconds at the wind's speed), and fades the same way into
   the ground and into the camera. */
#define TK_PETAL_BOX  52.0f
#define TK_PETAL_BAND 10.0f
static inline float tk_petal_fade(float d, float band) {
    float t = clampf(d / band, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void tk_draw_petals(void) {
    if(world_split) return;
    const float box = TK_PETAL_BOX;
    V3 c = r_cam.pos;
    V3 wind = v3(0.9f, -0.55f, 0.35f);
    r_hdr(h_petal);
    for(int i = 0; i < TK_PETALS; i++) {
        uint32_t h = (uint32_t)i * 2654435761u + 913u;
        float ox = (float)(h & 1023) / 1023.0f * box, oy = (float)((h >> 10) & 1023) / 1023.0f * box,
              oz = (float)((h >> 20) & 1023) / 1023.0f * box;
        float t = world_time * (0.8f + 0.3f * (float)(i & 3));
        float x = ox + wind.x * t * 1.6f + 1.4f * sinf(t * 1.1f + i), y = oy + wind.y * t * 1.6f + 0.5f * sinf(t * 2.3f + i * 0.7f),
              z = oz + wind.z * t * 1.6f + 1.2f * cosf(t * 0.9f + i * 1.3f);
        /* Wrap into the box about the camera, two thirds above eye level. */
        float rx = fmodf(fmodf(x - c.x + box * 0.5f, box) + box, box);
        float ry = fmodf(fmodf(y - c.y + box * 0.3f, box) + box, box);
        float rz = fmodf(fmodf(z - c.z + box * 0.5f, box) + box, box);
        /* Fade out towards every face of the box, so the wrap to the
           opposite face happens at zero alpha. */
        float a = tk_petal_fade(fminf(rx, box - rx), TK_PETAL_BAND)
                * tk_petal_fade(fminf(ry, box - ry), TK_PETAL_BAND)
                * tk_petal_fade(fminf(rz, box - rz), TK_PETAL_BAND);
        if(a < 0.02f) continue;
        V3 p = v3(c.x - box * 0.5f + rx, c.y - box * 0.3f + ry, c.z - box * 0.5f + rz);
        float above = p.y - world_ground_y(p.x, p.z) - 0.3f;
        if(above <= 0.0f) continue;
        a *= tk_petal_fade(above, 2.5f);       /* settles into the ground */
        V3 d = vsub(p, c);
        float dd = vdot(d, d);
        if(dd < 2.0f || !r_sphere_visible(p, 0.5f)) continue;
        a *= tk_petal_fade(sqrtf(dd) - 1.4f, 2.0f);   /* passes through the camera */
        if(a < 0.02f) continue;
        r_billboard(p, 0.42f, 0.42f, t * 2.5f + (float)i, 0, 0, 1, 1, color_alpha(sunset_grade(argb(230, 255, 230, 240)), 0.9f * a));
    }
}

/* The lanterns' fire boxes glow once the sun is low. */
void tk_draw_glows(void) {
    if(TK_AFTERNOON) return;   /* unlit by day */
    float range = world_split ? 120.0f : 220.0f;
    r_hdr(h_glow_add);
    for(int i = 0; i < tk_lantern_n; i++) {
        V3 p = tk_lantern[i];
        V3 d = vsub(p, r_cam.pos);
        if(vdot(d, d) > range * range || !r_sphere_visible(p, 2.0f)) continue;
        float a = 0.30f + 0.06f * sinf(world_time * 9.0f + i * 1.7f);
        r_billboard(p, 2.6f, 2.6f, 0, 0, 0, 1, 1, color_alpha(argb(255, 255, 170, 70), a));
    }
}

void build_tsudoken(void) {
    f8m_default();
    f8m.road = h_tkroad;
    f8m.curb = h_tkcurb;
    f8m.shoulder = h_verge;
    f8m.barrier = h_hoarding;
    f8m.skirt = h_ishigaki;
    f8m.cap = argb(255, 40, 34, 96);   /* the boards' top edge */
    f8m.wall_h = 1.3f;                 /* Armco with a band of boards above it */
    f8m.barrier_u = 8.0f;              /* two boards a repeat */
    f8m.stripe_a = argb(255, 230, 230, 224);
    f8m.stripe_b = argb(255, 214, 44, 36);
    f8m.shoulder_uv = 6.0f;
    f8m.extra = f8_verges;
    f8m.verge = h_verge;
    f8m.ground = tk_terrain;
    h_far_sprite = h_sakurafar;
    tk_stream_init();
#ifdef DK_CHUNK_STATS
    ck_first[CK_GROUND] = chunk_n;   /* the kinds here: ground, road ("deck"), buildings ("rock"), trees+lanterns ("props") */
#endif
    tk_build_ground();
#ifdef DK_CHUNK_STATS
    ck_first[CK_DECK] = chunk_n;
#endif
    for(int c = 0; c < TRACK_SAMPLES / 8; c++) {
        /* The ramp's chunks (any raised road, or the gap) keep the full
           road at every distance: the coarse copy has no skirts. */
        float sv[MAX_SVALS];
        int n = f8_chunk_svals(c, sv);
        bool ramp = false;
        for(int k = 0; k < n; k++)
            if(track_height(sv[k]) > 0.001f || track_in_gap(sv[k])) ramp = true;
        f8m.split_lod = !ramp;
        f8_build_track_chunk(c);
        if(!ramp) f8_build_deck_coarse(c);
    }
    f8m.split_lod = false;
    chunk_begin(HF_OP, 900.0f);
    f8_jump_face(track_wrap(track.jump.cross + track.jump.lip), 1.0f);
    f8_jump_face(track_wrap(track.jump.cross + track.jump.land), -1.0f);
    chunk_end();
#ifdef DK_CHUNK_STATS
    ck_first[CK_ROCK] = chunk_n;
#endif
    tk_place_props();
    tk_fuji_init();
}

/* Tsudoken: p within margin metres of one of the solid boxes. */
static bool tk_in_box(V3 p, float margin) {
    for(int i = 0; i < tk_solid_n; i++) {
        const TkSolid *b = &tk_solid[i];
        if(p.y < b->y0 - margin || p.y > b->y1 + margin) continue;
        float dx = p.x - b->x, dz = p.z - b->z;
        if(fabsf(dx * b->cs - dz * b->sn) < b->hx + margin && fabsf(dx * b->sn + dz * b->cs) < b->hz + margin) return true;
    }
    return false;
}

/* Tsudoken: p in the ground, under the raised road (the ramp up to the
   jump and its stone walls) or in a barrier, or less than margin metres
   above them. hint carries the nearest road sample along a sight line. */
static bool tk_in_ground(V3 p, float margin, int *hint) {
    if(p.y < tk_terrain(p.x, p.z) + margin) return true;
    TrackPos tp;
    track_locate(p, *hint, &tp);
    *hint = tp.idx;
    if(track_in_gap(tp.s)) return false;
    float wall = tp.d > 0 ? track_wall_out_at(&tp) : track_wall_in_at(&tp), ad = fabsf(tp.d);
    if(ad > wall + 0.3f + margin) return false;
    float half = track.smp[tp.idx].half, road = track_road_y(tp.s, clampf(tp.d, -half, half));
    return p.y < road + margin || (ad > wall - 0.3f - margin && p.y < road + f8m.wall_h + margin);
}

/* world_solid on Tsudoken. */
bool tk_in_solid(V3 p, float margin) {
    int hint = -1;
    return tk_in_box(p, margin) || tk_in_ground(p, margin, &hint);
}

/* world_sight_clear on Tsudoken. */
bool tk_sight_clear(V3 a, V3 b) {
    /* Each box against the segment in its own frame (slabs). */
    for(int i = 0; i < tk_solid_n; i++) {
        const TkSolid *o = &tk_solid[i];
        float ax = a.x - o->x, az = a.z - o->z, bx = b.x - o->x, bz = b.z - o->z;
        float p0[3] = {ax * o->cs - az * o->sn, a.y, ax * o->sn + az * o->cs};
        float p1[3] = {bx * o->cs - bz * o->sn, b.y, bx * o->sn + bz * o->cs};
        float lo[3] = {-o->hx, o->y0, -o->hz}, hi[3] = {o->hx, o->y1, o->hz};
        float t0 = 0.0f, t1 = 1.0f;
        for(int k = 0; k < 3 && t0 <= t1; k++) {
            float d = p1[k] - p0[k];
            if(fabsf(d) < 1e-6f) {
                if(p0[k] < lo[k] || p0[k] > hi[k]) t1 = -1.0f;
                continue;
            }
            float u = (lo[k] - p0[k]) / d, v = (hi[k] - p0[k]) / d;
            t0 = fmaxf(t0, fminf(u, v));
            t1 = fminf(t1, fmaxf(u, v));
        }
        if(t0 <= t1) return false;
    }
    int steps = (int)vlen(vsub(b, a)), hint = -1;
    for(int i = 1; i < steps; i++)
        if(tk_in_ground(vlerp(a, b, (float)i / steps), 0.3f, &hint)) return false;
    return true;
}
