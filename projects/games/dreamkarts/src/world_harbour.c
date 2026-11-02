/* Palm Harbour: the headland with a quay, moored boats and a harbour
   wall out to the lighthouse. Round the start a crowd doing the wave and
   Chao along the fence; a crane working the quay, and a ferry out on the
   bay going back and forth past the seafront. All of it is scenery: it
   runs on world_time, which the consoles online don't share, so nothing
   here may touch a kart. */
#include "world_int.h"

#define HB_CHAO     8
#define HB_CRANE_Y  12.0f      /* the jib's height */
#define FR_X        520.0f     /* the ferry's route: two lanes along the bay */
#define FR_Z        300.0f     /* joined by a half circle at each end */
#define FR_R        65.0f
#define FR_SPEED    9.0f       /* m/s */
#define FR_START    170.0f     /* metres into the route as a race loads */

/* A small lit mesh in its own space, drawn with r_load_model. */
typedef struct {
    RV v[176] __attribute__((aligned(32)));
    uint16_t idx[270];
    int nv, ni;
} HbMesh;

static Bystander hb_people[HB_CHAO];
static HbMesh hb_ferry, hb_jib;
static V3 hb_jib_at;
static float hb_t0;

static void mesh_quad(HbMesh *m, V3 a, V3 b, V3 c, V3 d, uint32_t col) {
    if(m->nv + 4 > 176 || m->ni + 6 > 270) return;
    V3 n = vnorm(vcross(vsub(b, a), vsub(c, a)));
    uint32_t lit = shade(col, n);
    const V3 p[4] = {a, b, c, d};
    for(int i = 0; i < 4; i++) m->v[m->nv + i] = (RV){p[i].x, p[i].y, p[i].z, 0, 0, lit};
    static const uint8_t tri[6] = {0, 1, 2, 2, 1, 3};
    for(int i = 0; i < 6; i++) m->idx[m->ni++] = (uint16_t)(m->nv + tri[i]);
    m->nv += 4;
}

/* A box without a bottom: c its centre, h its half sizes. */
static void mesh_box(HbMesh *m, V3 c, V3 h, uint32_t col) {
    float x0 = c.x - h.x, x1 = c.x + h.x, y0 = c.y - h.y, y1 = c.y + h.y, z0 = c.z - h.z, z1 = c.z + h.z;
    mesh_quad(m, v3(x0, y1, z1), v3(x1, y1, z1), v3(x0, y1, z0), v3(x1, y1, z0), col);
    mesh_quad(m, v3(x1, y1, z1), v3(x0, y1, z1), v3(x1, y0, z1), v3(x0, y0, z1), col);
    mesh_quad(m, v3(x0, y1, z0), v3(x1, y1, z0), v3(x0, y0, z0), v3(x1, y0, z0), col);
    mesh_quad(m, v3(x1, y1, z0), v3(x1, y1, z1), v3(x1, y0, z0), v3(x1, y0, z1), col);
    mesh_quad(m, v3(x0, y1, z1), v3(x0, y1, z0), v3(x0, y0, z1), v3(x0, y0, z0), col);
}

/* A car ferry ~60 m long, bow along +z, the waterline at y = 0: chunky so
   it reads from across the bay. */
static void fr_build(void) {
    HbMesh *m = &hb_ferry;
    uint32_t navy = argb(255, 30, 52, 110), red = argb(255, 214, 44, 38), white = argb(255, 246, 246, 240);
    uint32_t glass = argb(255, 52, 70, 92);
    m->nv = m->ni = 0;
    mesh_box(m, v3(0, 2.4f, -7.0f), v3(6.0f, 2.6f, 23.0f), navy);
    /* The bow, a wedge beyond the hull. */
    V3 pl = v3(-6.0f, 5.0f, 16.0f), pr = v3(6.0f, 5.0f, 16.0f), tip = v3(0, 5.6f, 31.0f), keel = v3(0, -0.2f, 26.0f);
    V3 fl = v3(-6.0f, -0.2f, 16.0f), fr = v3(6.0f, -0.2f, 16.0f);
    mesh_quad(m, pr, tip, fr, keel, navy);
    mesh_quad(m, tip, pl, keel, fl, navy);
    mesh_quad(m, pl, pr, tip, tip, red);
    mesh_box(m, v3(0, 4.7f, -7.0f), v3(6.08f, 0.45f, 23.05f), red);
    mesh_box(m, v3(0, 7.2f, -9.0f), v3(5.4f, 2.1f, 18.0f), white);
    mesh_box(m, v3(0, 7.8f, -9.0f), v3(5.5f, 0.6f, 18.05f), glass);
    mesh_box(m, v3(0, 10.5f, -12.0f), v3(4.6f, 1.2f, 12.0f), white);
    mesh_box(m, v3(0, 12.6f, -2.0f), v3(3.8f, 1.0f, 2.6f), white);
    mesh_box(m, v3(0, 12.8f, -2.0f), v3(3.9f, 0.4f, 2.7f), glass);
    mesh_box(m, v3(0, 13.8f, -15.0f), v3(1.9f, 2.6f, 2.5f), red);
    mesh_box(m, v3(0, 16.7f, -15.0f), v3(1.95f, 0.4f, 2.55f), argb(255, 24, 24, 28));
}

/* Where the ferry is: down the near lane heading west, round the half
   circle, back up the far lane heading east, and round again. */
static void fr_pose(V3 *p, float *yaw) {
    float straight = 2.0f * FR_X, arc = DK_PI * FR_R;
    float s = fmodf((world_time - hb_t0) * FR_SPEED + FR_START, 2.0f * (straight + arc));
    float bob = 0.12f * sinf(world_time * 0.8f);
    if(s < straight) {
        *p = v3(FR_X - s, bob, FR_Z);
        *yaw = -DK_PI * 0.5f;
    }
    else if((s -= straight) < arc) {
        float a = s / FR_R;
        *p = v3(-FR_X - FR_R * sinf(a), bob, FR_Z + FR_R - FR_R * cosf(a));
        *yaw = -DK_PI * 0.5f + a;
    }
    else if((s -= arc) < straight) {
        *p = v3(-FR_X + s, bob, FR_Z + 2.0f * FR_R);
        *yaw = DK_PI * 0.5f;
    }
    else {
        float a = (s - straight) / FR_R;
        *p = v3(FR_X + FR_R * sinf(a), bob, FR_Z + FR_R + FR_R * cosf(a));
        *yaw = DK_PI * 0.5f + a;
    }
    p->y += WATER_Y;
}

/* The crane's jib, pivoting on its tower: a long arm over +z, a short
   counterweighted one behind, a crate hanging on its cable. */
static void jib_build(void) {
    HbMesh *m = &hb_jib;
    uint32_t yellow = argb(255, 246, 190, 30), grey = argb(255, 70, 72, 80);
    m->nv = m->ni = 0;
    mesh_box(m, v3(0, 0.3f, 5.0f), v3(0.4f, 0.35f, 11.0f), yellow);
    mesh_box(m, v3(0, 0.2f, -5.0f), v3(1.0f, 0.8f, 1.0f), grey);
    mesh_box(m, v3(0, 1.0f, 0.5f), v3(0.8f, 0.9f, 1.0f), yellow);
    mesh_box(m, v3(0, -3.6f, 13.5f), v3(0.05f, 3.7f, 0.05f), grey);
    mesh_box(m, v3(0, -8.0f, 13.5f), v3(1.25f, 0.9f, 1.8f), argb(255, 47, 111, 216));
}

void hb_init(void) {
    fr_build();
    jib_build();
}

void hb_update(void) {
    grandstand_update();
    bystanders_update(hb_people, HB_CHAO, 0.04f);
}

void hb_draw_op(void) {
    float m[16];
    V3 p;
    float yaw;
    fr_pose(&p, &yaw);
    V3 d = vsub(p, r_cam.pos);
    float range = world_split ? 500.0f : 1500.0f;
    if(vdot(d, d) < range * range && r_sphere_visible(vadd(p, v3(0, 8.0f, 0)), 42.0f)) {
        m4_translate_rotate(m, p, yaw, 0.006f * sinf(world_time * 0.6f), 0.012f * sinf(world_time * 0.47f));
        r_load_model(m);
        r_hdr(h_col);
        r_mesh(hb_ferry.v, hb_ferry.nv, hb_ferry.idx, hb_ferry.ni, false);
    }
    d = vsub(hb_jib_at, r_cam.pos);
    range = world_split ? 150.0f : 350.0f;
    if(vdot(d, d) < range * range && r_sphere_visible(hb_jib_at, 18.0f)) {
        /* From over the crates out to the boats and back, easing at each end. */
        m4_translate_rotate(m, hb_jib_at, 0.9f * sinf(world_time * 0.21f) - 0.2f, 0, 0);
        r_load_model(m);
        r_hdr(h_col);
        r_mesh(hb_jib.v, hb_jib.nv, hb_jib.idx, hb_jib.ni, false);
    }
    r_load_vp();
}

/* True when p (on the ground plane) is at least margin behind the nearest
   barrier. */
bool hb_clear(V3 p, float margin) {
    TrackPos tp;
    track_locate(v3(p.x, 0, p.z), -1, &tp);
    V3 d = vsub(v3(p.x, 0, p.z), tp.center);
    return vlen(d) > track_wall_out_at(&tp) + margin;
}

static V3 hb_ground(float x, float z) {
    return v3(x, f8_ground_y(x, z), z);
}

/* A stack of shipping crates on the quay. */
static void hb_crates(V3 c, float yaw, uint32_t seed) {
    static const uint32_t cols[] = {0xffd83c30, 0xff2f6fd8, 0xfff0b020, 0xff38a050, 0xffe8e8e0};
    chunk_begin(HF_OP, 500.0f);
    V3 ax = heading_right(yaw);
    for(int i = 0; i < 4; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t col = cols[(seed >> 16) % 5];
        V3 p = i < 3 ? vmad(c, ax, (float)(i - 1) * 2.7f) : vadd(c, v3(0, 2.5f, 0));
        float h = 1.25f;
        box(h_wood, vadd(p, v3(0, h, 0)), v3(1.25f, h, 2.6f), yaw + 0.04f * (float)(i - 1), col, 0.4f);
    }
    chunk_end();
}

/* The harbour wall: a curve of boulders out to the lighthouse. */
static void hb_breakwater(void) {
    for(int i = 0; i < 9; i++) {
        float t = (float)i / 8.0f;
        V3 p = v3(lerpf(118.0f, 205.0f, t), 0, 96.0f + 125.0f * t * t);
        add_rock(p, 6.0f + (float)(i % 3), 2.4f + (float)((i * 5) % 3) * 0.8f, (uint32_t)(i * 31 + 7), false);
    }
}

/* Chao on a low deck behind the infield fence by the line, high enough
   to see over it (and to be seen from the road). */
static void hb_fence_chao(void) {
    const float h = 0.75f;
    V3 a = track_point(track_wrap(-12.0f), -15.4f), b = track_point(track_wrap(28.0f), -15.4f);
    chunk_begin(HF_OP, 500.0f);
    box(h_wood, v3((a.x + b.x) * 0.5f, h * 0.5f, (a.z + b.z) * 0.5f), v3(vlen(vsub(b, a)) * 0.5f, h * 0.5f, 1.4f), 0,
        argb(255, 190, 150, 110), 0.4f);
    chunk_end();
    for(int i = 0; i < HB_CHAO; i++) {
        V3 at = track_point(track_wrap(-9.0f + (float)i * 5.0f), -15.0f - (float)(i & 1) * 0.8f);
        add_chao(&hb_people[i], v3(at.x, h, at.z), DK_PI + 0.3f - (float)(i % 3) * 0.3f,
                 (float)(i % 4) * 0.25f - 0.4f, i * 3, (float)i * 1.9f);
    }
}

/* The quay crane, its jib drawn apart so it can swing. */
static void hb_crane(V3 c) {
    uint32_t yellow = argb(255, 246, 190, 30);
    chunk_begin(HF_OP, 500.0f);
    box(h_col, vadd(c, v3(0, 0.4f, 0)), v3(2.2f, 0.4f, 2.2f), 0, argb(255, 170, 170, 165), 1);
    box(h_col, vadd(c, v3(0, HB_CRANE_Y * 0.5f, 0)), v3(0.55f, HB_CRANE_Y * 0.5f, 0.55f), 0, yellow, 1);
    chunk_end();
    hb_jib_at = vadd(c, v3(0, HB_CRANE_Y, 0));
}

static void hb_place_props(void) {
    add_gantry();
    add_grandstand(-78.0f, -8.0f, -113.0f, true);
    hb_fence_chao();

    /* Infield: the big field inside turn 1, the pocket between the hairpins
       and the seafront lawn. */
    add_pond(v3(-12.0f, F8_GRASS_Y, -62.0f), 26.0f, 12.0f);
    add_sign(v3(22.0f, 0, -64.0f), 0.0f);
    add_sign(v3(-40.0f, 0, 40.0f), 0.0f);
    add_umbrella(v3(-32.0f, 0, -54.0f), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(v3(8.0f, 0, -72.0f), argb(255, 40, 120, 230), 0xffffffff);
    add_umbrella(v3(-58.0f, 0, 36.0f), argb(255, 255, 210, 40), argb(255, 60, 190, 90));
    add_umbrella(v3(-14.0f, 0, 44.0f), argb(255, 255, 120, 180), 0xffffffff);
    add_hut(v3(30.0f, 0, -14.0f), 0.4f);
    add_hut(v3(28.0f, 0, 10.0f), -0.3f);
    add_hut(v3(-120, 0, -118), 0.3f);
    add_hut(v3(-100, 0, -124), -0.2f);
    add_hut(v3(112, 0, -112), 0.6f);

    /* Palms scattered over the headland, kept behind the barriers. */
    uint32_t r = 0x50a1u;
    for(int tries = 0; tries < 400 && palm_n < 40; tries++) {
        r = r * 1664525u + 1013904223u;
        float x = -150.0f + (float)((r >> 8) & 1023) / 1023.0f * 300.0f;
        float z = -135.0f + (float)((r >> 18) & 1023) / 1023.0f * 228.0f;
        V3 p = hb_ground(x, z);
        if(p.y < WATER_Y + 0.15f || !hb_clear(p, 3.0f)) continue;
        /* Keep the grandstand, the pier and the props above clear. */
        if(z < -108.0f && x > -84.0f && x < -2.0f) continue;
        if(fabsf(x - 70.0f) < 8.0f && z > 84.0f) continue;
        bool crowded = false;
        for(int i = 0; i < palm_n; i++) {
            V3 d = vsub(palms[i].base, p);
            if(d.x * d.x + d.z * d.z < 12.0f * 12.0f) crowded = true;
        }
        if(crowded) continue;
        add_palm(p, 9.0f + (float)((r >> 4) % 4) * 1.2f, (float)(r % 628) * 0.01f, 0.18f + (z > 70.0f ? 0.15f : 0.0f),
                 r >> 12);
    }

    /* The quay: the pier, moored boats, crates and the harbour wall. */
    add_pier(70.0f, 92.0f);
    hb_crates(hb_ground(48.0f, 88.0f), 0.1f, 3);
    hb_crane(hb_ground(58.0f, 96.0f));
    hb_crates(hb_ground(-104.0f, 90.0f), -0.2f, 11);
    add_boat(v3(80.0f, WATER_Y, 122.0f), 0.0f, argb(255, 200, 40, 40));
    add_boat(v3(60.0f, WATER_Y, 128.0f), 0.1f, argb(255, 250, 250, 250));
    add_boat(v3(92.0f, WATER_Y, 146.0f), -0.15f, argb(255, 30, 60, 140));
    add_boat(v3(-170, WATER_Y, 350), 0.8f, argb(255, 30, 60, 140));
    add_boat(v3(160, WATER_Y, 500), -0.4f, argb(255, 250, 200, 40));
    hb_breakwater();
    add_lighthouse(v3(214.0f, 0, 236.0f));
    add_umbrella(hb_ground(-60.0f, 92.0f), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(hb_ground(-20.0f, 94.0f), argb(255, 40, 200, 160), 0xffffffff);
    add_umbrella(hb_ground(20.0f, 91.0f), argb(255, 255, 150, 30), 0xffffffff);
    add_arch(v3(-90, 0, 190), 0.25f);
    add_rock(v3(-210, 0, 170), 10.0f, 12.0f, 9, true);
    add_rock(v3(10, 0, 260), 14.0f, 18.0f, 5, true);
    chunk_begin(HF_OP, 500.0f);
    for(int i = 0; i < 9; i++) {
        V3 b = v3(-110.0f + i * 20.0f, WATER_Y - 0.2f, 134.0f);
        uint32_t col = (i & 1) ? argb(255, 250, 250, 250) : argb(255, 230, 50, 40);
        cylinder(h_col, b, 0.45f, 0.35f, 0.9f, 6, col, col, 1, 1);
    }
    chunk_end();

    /* Distant palm silhouettes on the southern hills. */
    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        float x = -420.0f + (float)(h & 1023) / 1023.0f * 840.0f;
        float z = -165.0f - (float)((h >> 10) & 255) / 255.0f * 200.0f;
        far_sprites[far_sprite_n] = hb_ground(x, z);
        far_sprite_h[far_sprite_n] = 9.0f + (float)((h >> 18) & 7);
        far_sprite_n++;
    }
    f8_foam();
}

void build_harbour(void) {
    f8m_default();
    gnd_xw = 160.0f;
    gnd_zn = 84.0f;
    gnd_zs = 135.0f;
    f8_build_ground();
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        f8_build_track_chunk(c);
    hb_place_props();
    hb_t0 = world_time;
    grandstand_update();
}
