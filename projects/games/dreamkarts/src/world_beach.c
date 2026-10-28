/* Emerald Beach: the oval by the sea, its boardwalk and infield pond, the
   arch, lighthouse and boats out in the bay. */
#include "world_int.h"

static float shore_db[TRACK_SAMPLES];

/* Ground height beyond the outer wall: beach into the sea to the north,
   rising jungle hills to the south. db = distance beyond the wall. */
float terrain_height(V3 p, float db, float shore) {
    float north = smooth01(-30.0f, 30.0f, p.z);
    float beach = lerpf(0.0f, -0.75f, shore) - 0.02f * db - 0.0016f * db * db;
    float hills = fmaxf(0.0f, db - 16.0f) * 0.13f;
    float bumps = (sinf(p.x * 0.021f) + sinf(p.z * 0.017f + 1.3f) + sinf((p.x + p.z) * 0.009f)) *
                  clampf((db - 24.0f) / 60.0f, 0, 1) * 4.0f;
    return lerpf(hills + bumps, beach, north);
}

static void build_track_chunk(int c) {
    const int per = 8;
    const float ds = track.ds;
    int mid = (c * per + per / 2) % TRACK_SAMPLES;
    bool board = track.smp[mid].section == SEC_BOARDWALK;
    chunk_begin(HF_OP, 2000.0f);

    /* Road surface (asphalt with curbs, or the wider boardwalk). */
    strip_begin(board ? h_board : h_road);
    for(int j = 0; j <= per; j++) {
        const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
        float s = (float)(c * per + j) * ds, half = board ? 9.6f : 8.0f;
        uint32_t col = board ? argb(255, 255, 255, 255) : argb(255, 250, 250, 250);
        float v = s / (board ? 8.0f : ROAD_V_M);
        svp(vmad(t->p, t->r, -half), 0, v, col);
        svp(vmad(t->p, t->r, half), 1, v, col);
    }
    strip_end();

    if(!board) {
        for(int side = -1; side <= 1; side += 2) {
            strip_begin(h_curb);
            for(int j = 0; j <= per; j++) {
                const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
                float s = (float)(c * per + j) * ds;
                V3 a = vmad(t->p, t->r, side * 8.0f), b = vmad(t->p, t->r, side * 9.6f);
                a.y = b.y = 0.01f;
                if(side < 0) {
                    svp(b, 0, s / 2.4f, 0xffffffff);
                    svp(a, 1, s / 2.4f, 0xffffffff);
                }
                else {
                    svp(a, 1, s / 2.4f, 0xffffffff);
                    svp(b, 0, s / 2.4f, 0xffffffff);
                }
            }
            strip_end();
        }
    }

    /* Sand shoulders to the barriers. */
    for(int side = -1; side <= 1; side += 2) {
        strip_begin(board && side > 0 ? h_wood : h_sand);
        bool any = false;
        for(int j = 0; j <= per; j++) {
            const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
            float inner = board ? 9.6f : 9.6f;
            float outer = side < 0 ? t->wall_in : t->wall_out;
            if(outer <= inner + 0.05f) outer = inner + 0.05f;
            V3 a = vmad(t->p, t->r, side * inner), b = vmad(t->p, t->r, side * outer);
            uint32_t ca = argb(255, 238, 238, 238), cb = argb(255, 214, 214, 214);
            svp(a, a.x / 4.0f, a.z / 4.0f, ca);
            svp(b, b.x / 4.0f, b.z / 4.0f, cb);
            any = true;
        }
        if(any) strip_end();
    }

    /* Barriers: padded walls, or wooden rails along the boardwalk. */
    for(int side = -1; side <= 1; side += 2) {
        bool rail = board;
        strip_begin(rail ? h_wood : h_barrier);
        for(int j = 0; j <= per; j++) {
            const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
            float s = (float)(c * per + j) * ds;
            float w = side < 0 ? t->wall_in : t->wall_out;
            V3 b = vmad(t->p, t->r, side * w);
            float hgt = rail ? 0.85f : 1.0f;
            uint32_t col = shade(argb(255, 255, 255, 255), vscale(t->r, (float)-side));
            svp(vadd(b, v3(0, hgt, 0)), s / 4.0f, rail ? 0.0f : 0.0f, col);
            svp(b, s / 4.0f, rail ? 0.25f : 1.0f, color_scale(col, 0.8f));
        }
        strip_end();
        /* Barrier top cap so the walls read as solid from above. */
        strip_begin(h_col);
        for(int j = 0; j <= per; j++) {
            const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
            float w = side < 0 ? t->wall_in : t->wall_out;
            float hgt = rail ? 0.85f : 1.0f;
            V3 a = vmad(t->p, t->r, side * (w - 0.25f)), b = vmad(t->p, t->r, side * (w + 0.25f));
            a.y = b.y = hgt;
            uint32_t cap = rail ? argb(255, 190, 140, 90) : argb(255, 255, 212, 50);
            svp(a, 0, 0, cap);
            svp(b, 0, 0, cap);
        }
        strip_end();
    }
    chunk_end();

    /* Infield: from the inner wall to the oval's spine. */
    chunk_begin(HF_OP, 2000.0f);
    strip_begin(h_grass);
    for(int j = 0; j <= per; j++) {
        const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
        V3 a = vmad(t->p, t->r, -t->wall_in);
        V3 spine = v3(clampf(t->p.x, -85.0f, 85.0f), 0, 0);
        float dist = vlen(vsub(a, spine));
        V3 a2 = vlerp(a, spine, clampf(4.0f / fmaxf(dist, 1.0f), 0, 1));
        svp(a, a.x / 6.0f, a.z / 6.0f, argb(255, 196, 206, 196));
        svp(a2, a2.x / 6.0f, a2.z / 6.0f, argb(255, 240, 248, 240));
    }
    strip_end();
    strip_begin(h_grass);
    for(int j = 0; j <= per; j++) {
        const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
        V3 a = vmad(t->p, t->r, -t->wall_in);
        V3 spine = v3(clampf(t->p.x, -85.0f, 85.0f), 0, 0);
        float dist = vlen(vsub(a, spine));
        V3 a2 = vlerp(a, spine, clampf(4.0f / fmaxf(dist, 1.0f), 0, 1));
        svp(a2, a2.x / 6.0f, a2.z / 6.0f, argb(255, 240, 248, 240));
        svp(spine, spine.x / 6.0f, spine.z / 6.0f, argb(255, 255, 255, 255));
    }
    strip_end();
    chunk_end();

    /* Outside terrain bands. */
    static const float bands[] = {0.0f, 5.0f, 12.0f, 22.0f, 40.0f, 75.0f, 130.0f, 230.0f, 420.0f};
    const int nb = (int)(sizeof(bands) / sizeof(bands[0]));
    chunk_begin(HF_OP, 2000.0f);
    for(int b = 0; b + 1 < nb; b++) {
        int step = b >= 4 ? 2 : 1;
        if(b == 6) {
            /* The outermost bands form their own chunk. */
            chunk_end();
            chunk_begin(HF_OP, 2000.0f);
            if(cur_chunk) cur_chunk->far = 1;
        }
        /* Pick the band's material from its middle: grass on dry southern
           ground, sand on beaches and the sea floor. */
        const TrackSample *tm = &track.smp[mid];
        V3 pm = vmad(tm->p, tm->r, tm->wall_out + bands[b + 1]);
        pm.y = terrain_height(pm, bands[b + 1], tm->shore);
        bool grassy = b >= 2 && pm.z < 10.0f && pm.y > 0.4f;
        float uvs = grassy ? 6.0f : 4.0f;
        strip_begin(grassy ? h_grass : h_sand);
        for(int j = 0; j <= per; j += step) {
            const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
            for(int k = 0; k < 2; k++) {
                float db = bands[b + k];
                V3 p = vmad(t->p, t->r, t->wall_out + db);
                p.y = terrain_height(p, db, t->shore);
                svp(p, p.x / uvs, p.z / uvs, ground_color(p.y, db, grassy));
            }
        }
        strip_end();
    }
    chunk_end();
}

static void place_props(void) {
    /* Infield. */
    add_pond(v3(0, 0, 0), 34.0f, 13.0f);
    add_sign(v3(-58.0f, 0, 0), 0.0f);
    add_sign(v3(58.0f, 0, 0), 0.0f);
    const V3 infield_palms[] = {
        {-30, 0, 15}, {-12, 0, 17}, {10, 0, 16}, {30, 0, -15}, {12, 0, -17}, {-14, 0, -16},
        {-40, 0, -6}, {40, 0, 6}, {-95, 0, 12}, {95, 0, -12}, {-110, 0, -6}, {110, 0, 8}
    };
    for(unsigned i = 0; i < sizeof(infield_palms) / sizeof(infield_palms[0]); i++)
        add_palm(infield_palms[i], 8.5f + (float)(i % 3) * 1.5f, (float)i * 1.7f, 0.18f, i * 13 + 5);
    add_umbrella(v3(-22, 0, -8), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(v3(0, 0, 9), argb(255, 40, 120, 230), 0xffffffff);
    add_umbrella(v3(22, 0, -6), argb(255, 255, 210, 40), argb(255, 60, 190, 90));
    add_umbrella(v3(-70, 0, -18), argb(255, 255, 120, 180), 0xffffffff);
    add_umbrella(v3(70, 0, 18), argb(255, 255, 150, 30), 0xffffffff);

    /* South side: grandstand, huts, palms. */
    add_grandstand(-52.0f, 26.0f, -82.0f);
    add_gantry();
    add_hut(v3(-120, 0, -98), 0.3f);
    add_hut(v3(-100, 0, -104), -0.2f);
    add_hut(v3(118, 0, -96), 0.6f);
    const V3 south_palms[] = {
        {-70, 0, -86}, {44, 0, -84}, {60, 0, -92}, {-140, 0, -80}, {150, 0, -78}, {-160, 0, -60},
        {170, 0, -50}, {-90, 0, -92}, {90, 0, -100}, {0, 0, -104}
    };
    for(unsigned i = 0; i < sizeof(south_palms) / sizeof(south_palms[0]); i++)
        add_palm(south_palms[i], 10.0f + (float)(i % 4) * 1.2f, (float)i * 2.3f, 0.22f, i * 7 + 3);

    /* North beaches at the turn ends. */
    const V3 beach_palms[] = {
        {150, 0, 50}, {165, 0, 30}, {-150, 0, 48}, {-166, 0, 26}, {120, 0, 72}, {-120, 0, 72}
    };
    for(unsigned i = 0; i < sizeof(beach_palms) / sizeof(beach_palms[0]); i++) {
        V3 p = beach_palms[i];
        TrackPos tp;
        track_locate(p, -1, &tp);
        float wall = track_wall_out_at(&tp);
        p = vmad(tp.center, tp.right, wall + 4.0f);
        p.y = terrain_height(p, 4.0f, track.smp[tp.idx].shore);
        if(p.y > WATER_Y + 0.1f)
            add_palm(p, 9.5f + (float)i, (float)i * 1.1f + 2.0f, 0.35f, i * 5 + 11);
    }
    add_umbrella(v3(160, -0.05f, 40), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(v3(-158, -0.05f, 40), argb(255, 40, 200, 160), 0xffffffff);

    /* Sea features. */
    add_arch(v3(-40, 0, 165), 0.15f);
    add_rock(v3(110, 0, 230), 14.0f, 18.0f, 5, true);
    add_rock(v3(-210, 0, 150), 10.0f, 12.0f, 9, true);
    add_rock(v3(290, 0, 60), 16.0f, 22.0f, 17, true);
    add_rock(v3(60, 0, 140), 5.0f, 4.0f, 21, false);
    add_lighthouse(v3(230, 0, 210));
    add_boat(v3(-170, WATER_Y, 330), 0.8f, argb(255, 30, 60, 140));
    add_boat(v3(160, WATER_Y, 480), -0.4f, argb(255, 200, 40, 40));
    add_boat(v3(420, WATER_Y, 260), 1.9f, argb(255, 250, 250, 250));
    add_pier(32.0f, 66.0f);
    chunk_begin(HF_OP, 500.0f);
    for(int i = 0; i < 9; i++) {
        V3 b = v3(-80.0f + i * 20.0f, WATER_Y - 0.2f, 104.0f);
        cylinder(h_col, b, 0.45f, 0.35f, 0.9f, 6, (i & 1) ? argb(255, 250, 250, 250) : argb(255, 230, 50, 40),
                 (i & 1) ? argb(255, 250, 250, 250) : argb(255, 230, 50, 40), 1, 1);
    }
    chunk_end();

    /* Distant palm silhouettes on the hills. */
    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t r = (uint32_t)i * 2654435761u;
        float t = (float)(r & 1023) / 1023.0f;
        float db = 30.0f + (float)((r >> 10) & 255) / 255.0f * 150.0f;
        float s = track.length * (0.55f + 0.9f * t);
        V3 p = track_point(s, 21.0f + db);
        if(p.z > 5.0f) continue;
        p.y = terrain_height(p, db, 0.0f);
        far_sprites[far_sprite_n] = p;
        far_sprite_h[far_sprite_n] = 9.0f + (float)((r >> 18) & 7);
        far_sprite_n++;
    }
}

void build_beach(void) {
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        build_track_chunk(c);
    /* Waterline distance beyond the outer wall, for the foam strip. */
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        const TrackSample *t = &track.smp[i];
        float db = 0;
        for(int k = 0; k < 80; k++) {
            V3 p = vmad(t->p, t->r, t->wall_out + db);
            if(terrain_height(p, db, t->shore) < WATER_Y) break;
            db += 0.25f;
        }
        shore_db[i] = db;
    }
    for(int i = 0; i <= TRACK_SAMPLES && foam_count < MAX_FOAM; i += 2) {
        const TrackSample *t = &track.smp[i % TRACK_SAMPLES];
        foam_p[foam_count] = vmad(t->p, t->r, t->wall_out + shore_db[i % TRACK_SAMPLES]);
        foam_n[foam_count] = t->r;
        foam_u[foam_count] = t->s;
        foam_on[foam_count] = t->p.z > 8.0f;
        foam_count++;
    }
    place_props();
}
