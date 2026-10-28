/* Coral Crossing: the figure of eight with its jump, lagoons in both
   loops, on the headland Palm Harbour and Temple Hill share. */
#include "world_int.h"

/* Ground: a flat headland around the track, jungle hills rising to the
   south and a beach falling into the sea to the north. Palm Harbour shares
   it with its own plateau extent. */
float gnd_xw = 200.0f, gnd_zn = 84.0f, gnd_zs = 96.0f;

float f8_ground(float x, float z) {
    float dx = fmaxf(0.0f, fabsf(x) - gnd_xw);
    float dzn = fmaxf(0.0f, z - gnd_zn), dzs = fmaxf(0.0f, -z - gnd_zs);
    float dn = sqrtf(dx * dx + dzn * dzn), dsth = sqrtf(dx * dx + dzs * dzs);
    float beach = -0.012f * dn - 0.0005f * dn * dn;
    float bumps = (sinf(x * 0.021f) + sinf(z * 0.017f + 1.3f) + sinf((x + z) * 0.009f)) *
                  clampf((dsth - 20.0f) / 60.0f, 0, 1) * 4.0f;
    float hills = fmaxf(0.0f, dsth - 8.0f) * 0.14f + bumps;
    return lerpf(hills, beach, smooth01(-40.0f, 40.0f, z));
}

float f8_ground_y(float x, float z) {
    float h = f8_ground(x, z);
    return h > -0.01f && h < 0.01f ? F8_GRASS_Y : h + F8_GRASS_Y;
}

void f8_build_ground(void) {
    static const float zs[] = {-400, -340, -280, -230, -190, -150, -120, -96, -60, -20, 20, 60, 84, 92, 100,
                               108, 120, 140, 170, 210, 260};
    const int nz = (int)(sizeof(zs) / sizeof(zs[0]));
    const int nx = 24;          /* 40 m columns from x = -480 */
    for(int cz = 0; cz + 1 < nz; cz += 4) {
        for(int cx = 0; cx < nx; cx += 4) {
            chunk_begin(HF_OP, 2000.0f);
            bool far = true;
            for(int j = cz; j < cz + 4 && j + 1 < nz; j++) {
                float za = zs[j], zb = zs[j + 1];
                float xm = -480.0f + (cx + 2) * 40.0f, zm = (za + zb) * 0.5f;
                float hm = f8_ground(xm, zm);
                bool grassy = zm < 70.0f && hm > -0.1f;
                float uvs = grassy ? 6.0f : 4.0f;
                if(fabsf(xm) < 300.0f && zm > -200.0f && zm < 200.0f) far = false;
                strip_begin(grassy ? h_grass : h_sand);
                for(int i = cx; i <= cx + 4 && i <= nx; i++) {
                    float x = -480.0f + i * 40.0f;
                    for(int k = 1; k >= 0; k--) {
                        float z = k ? zb : za;
                        float y = f8_ground_y(x, z);
                        float db = fmaxf(0.0f, fmaxf(fabsf(x) - gnd_xw, -z - gnd_zs));
                        svp(v3(x, y, z), x / uvs, z / uvs, ground_color(y, db, grassy));
                    }
                }
                strip_end();
            }
            if(cur_chunk) cur_chunk->far = far;
            chunk_end();
        }
    }
}

/* Foam along the beach's waterline. */
void f8_foam(void) {
    for(float x = -460.0f; x <= 460.0f && foam_count < MAX_FOAM; x += 20.0f) {
        float z = 60.0f;
        while(z < 300.0f && f8_ground(x, z) >= WATER_Y) z += 1.0f;
        foam_p[foam_count] = v3(x, 0, z);
        foam_n[foam_count] = v3(0, 0, 1);
        foam_u[foam_count] = x;
        foam_on[foam_count] = z < 300.0f;
        foam_count++;
    }
}

static void f8_place_props(void) {
    const float cx = 89.34f;   /* loop centres at (+-cx, 0) */
    chunk_begin(HF_OP, 900.0f);
    f8_jump_face(track_wrap(track.jump.cross + track.jump.lip), 1.0f);
    f8_jump_face(track_wrap(track.jump.cross + track.jump.land), -1.0f);
    chunk_end();
    add_gantry();
    add_grandstand(-15.0f, 45.0f, -62.0f);

    /* Lagoons in both loops. */
    for(int side = -1; side <= 1; side += 2) {
        V3 c = v3(side * cx, F8_GRASS_Y, 0);
        add_pond(c, 21.0f, 15.0f);
        add_umbrella(v3(side * cx - 14.0f, 0, 19.0f), argb(255, 230, 40, 40), 0xffffffff);
        add_umbrella(v3(side * cx + 16.0f, 0, -18.0f), argb(255, 40, 120, 230), 0xffffffff);
        add_umbrella(v3(side * cx + 4.0f, 0, 22.0f), argb(255, 255, 210, 40), argb(255, 60, 190, 90));
        for(int i = 0; i < 6; i++) {
            float a = (float)i / 6.0f * DK_TAU + 0.4f * side;
            V3 p = v3(c.x + cosf(a) * 30.0f, 0, sinf(a) * 25.0f);
            add_palm(p, 8.5f + (float)(i % 3) * 1.5f, (float)i * 1.7f, 0.18f, (uint32_t)(i * 13 + 5 + side));
        }
    }
    add_sign(v3(0, 0, 48.0f), 0.0f);

    /* Between the legs and around the outside. */
    const V3 palms_in[] = {
        {0, 0, 40}, {-14, 0, 62}, {16, 0, 66}, {0, 0, -38}, {-18, 0, -48},
        {-70, 0, -86}, {60, 0, -92}, {-140, 0, -80}, {150, 0, -78}, {-160, 0, -60},
        {170, 0, -50}, {-90, 0, -92}, {90, 0, -100}, {0, 0, -100}, {-180, 0, 10}, {182, 0, -6}
    };
    for(unsigned i = 0; i < sizeof(palms_in) / sizeof(palms_in[0]); i++) {
        V3 p = palms_in[i];
        p.y = f8_ground_y(p.x, p.z);
        add_palm(p, 9.5f + (float)(i % 4) * 1.2f, (float)i * 2.3f, 0.22f, i * 7 + 3);
    }
    add_hut(v3(-120, 0, -98), 0.3f);
    add_hut(v3(-100, 0, -104), -0.2f);
    add_hut(v3(118, 0, -96), 0.6f);

    /* North beach and the sea. */
    const float beach_x[] = {-160, -110, -60, 70, 120, 170};
    for(int i = 0; i < 6; i++) {
        V3 p = v3(beach_x[i], 0, 90.0f + (float)(i % 2) * 4.0f);
        p.y = f8_ground_y(p.x, p.z);
        add_palm(p, 9.5f + (float)i, (float)i * 1.1f + 2.0f, 0.35f, i * 5 + 11);
    }
    add_umbrella(v3(-130, f8_ground_y(-130, 94), 94), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(v3(140, f8_ground_y(140, 94), 94), argb(255, 40, 200, 160), 0xffffffff);
    add_arch(v3(-40, 0, 185), 0.15f);
    add_rock(v3(110, 0, 250), 14.0f, 18.0f, 5, true);
    add_rock(v3(-210, 0, 170), 10.0f, 12.0f, 9, true);
    add_rock(v3(290, 0, 120), 16.0f, 22.0f, 17, true);
    add_lighthouse(v3(230, 0, 230));
    add_boat(v3(-170, WATER_Y, 350), 0.8f, argb(255, 30, 60, 140));
    add_boat(v3(160, WATER_Y, 500), -0.4f, argb(255, 200, 40, 40));
    add_pier(30.0f, 94.0f);
    chunk_begin(HF_OP, 500.0f);
    for(int i = 0; i < 9; i++) {
        V3 b = v3(-80.0f + i * 20.0f, WATER_Y - 0.2f, 130.0f);
        uint32_t col = (i & 1) ? argb(255, 250, 250, 250) : argb(255, 230, 50, 40);
        cylinder(h_col, b, 0.45f, 0.35f, 0.9f, 6, col, col, 1, 1);
    }
    chunk_end();

    /* Distant palm silhouettes on the southern hills. */
    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t r = (uint32_t)i * 2654435761u;
        float x = -420.0f + (float)(r & 1023) / 1023.0f * 840.0f;
        float z = -130.0f - (float)((r >> 10) & 255) / 255.0f * 230.0f;
        V3 p = v3(x, 0, z);
        p.y = f8_ground_y(x, z);
        far_sprites[far_sprite_n] = p;
        far_sprite_h[far_sprite_n] = 9.0f + (float)((r >> 18) & 7);
        far_sprite_n++;
    }

    f8_foam();
}

void build_figure8(void) {
    f8m_default();
    gnd_xw = 200.0f;
    gnd_zn = 84.0f;
    gnd_zs = 96.0f;
    f8_build_ground();
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        f8_build_track_chunk(c);
    f8_place_props();
}
