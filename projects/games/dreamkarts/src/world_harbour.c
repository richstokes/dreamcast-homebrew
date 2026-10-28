/* Palm Harbour: the headland with a quay, moored boats and a harbour
   wall out to the lighthouse. */
#include "world_int.h"

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

static void hb_place_props(void) {
    add_gantry();
    add_grandstand(-78.0f, -8.0f, -113.0f);

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
}
