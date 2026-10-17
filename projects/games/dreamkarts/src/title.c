/* Title screen: the beach in the background with the grid lined up, a
   rotating Sega-style sunburst, a bouncing per-letter logo, sparkles and a
   pulsing PRESS START. Then a slide-in menu for mode and engine class. */
#include "title.h"
#include "audio.h"
#include "hud.h"
#include "kart.h"
#include "render.h"
#include "texture_assets.h"
#include "font_metrics.h"

static const pvr_poly_hdr_t *h_logo, *h_flat, *h_star, *h_glow;
static float logo_in;        /* 0..1 drop-in animation */
static float menu_slide;
static int menu_row;
static float sparkle_t[8];
static float chao_call_t;

static const char *const cc_names[] = {"50cc", "100cc", "150cc"};
static float warn_t;

enum { ROW_PLAYERS, ROW_MODE, ROW_CC, ROW_COLOUR1, ROW_COLOUR2, ROW_START };

/* Menu rows: player 2's colour only appears in a two-player race. */
static int menu_rows(int *rows) {
    int n = 0;
    rows[n++] = ROW_PLAYERS;
    rows[n++] = ROW_MODE;
    rows[n++] = ROW_CC;
    rows[n++] = ROW_COLOUR1;
    if(game.num_players > 1) rows[n++] = ROW_COLOUR2;
    rows[n++] = ROW_START;
    return n;
}

static const char *mode_name(void) {
    if(game.mode == MODE_GP) return "GRAND PRIX";
    return game.num_players > 1 ? "VERSUS" : "TIME TRIAL";
}

/* Step a player's colour, skipping the other player's choice. */
static void step_colour(int player, int dir) {
    int c = game.pl[player].colour;
    do {
        c = (c + dir + COLOUR_COUNT) % COLOUR_COUNT;
    } while(game.num_players > 1 && c == game.pl[player ^ 1].colour);
    game.pl[player].colour = c;
    race_setup();
    audio_sfx(DK_SFX_CHAO, 150, player ? 190 : 66);
}

void title_init(void) {
    h_logo = r_header(TEX_LOGO, HF_TR | HF_NOFOG | HF_NOZTEST | HF_CLAMP);
    h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
    h_star = r_header(TEX_STAR, HF_TR | HF_ADD | HF_NOFOG | HF_NOZTEST);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG | HF_NOZTEST);
}

void title_enter(void) {
    logo_in = 0;
    menu_slide = 0;
    menu_row = 0;
    chao_call_t = 1.2f;
    for(int i = 0; i < 8; i++) sparkle_t[i] = -(float)i * 0.4f;
}

void title_update(float dt) {
    logo_in = fminf(1.0f, logo_in + dt * 1.3f);
    if(chao_call_t > 0) {
        chao_call_t -= dt;
        if(chao_call_t <= 0) audio_sfx(DK_SFX_CHAO, 200, 128);
    }
    if(game.state == ST_TITLE) {
        menu_slide = fmaxf(0.0f, menu_slide - dt * 4.0f);
        if(game.time > 0.6f && ((pad.pressed | pad2.pressed) & (BTN_START | BTN_A))) {
            audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
            audio_sfx(DK_SFX_WHOOSH, 160, 128);
            game_set_state(ST_MENU);
            menu_row = 0;
        }
        return;
    }
    /* ST_MENU */
    menu_slide = fminf(1.0f, menu_slide + dt * 4.0f);
    if(warn_t > 0) warn_t -= dt;
    int rows[8], n = menu_rows(rows);
    if(menu_row >= n) menu_row = n - 1;
    if(input_menu_up()) {
        menu_row = (menu_row + n - 1) % n;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_menu_down()) {
        menu_row = (menu_row + 1) % n;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    int row = rows[menu_row];
    int dir = input_menu_left() ? -1 : input_menu_right() ? 1 : 0;
    if(dir) {
        if(row == ROW_PLAYERS) {
            game.num_players = game.num_players == 1 ? 2 : 1;
            if(game.pl[1].colour == game.pl[0].colour) game.pl[1].colour = (game.pl[0].colour + 1) % COLOUR_COUNT;
            race_setup();
        }
        else if(row == ROW_MODE) {
            game.mode = game.mode == MODE_GP ? MODE_TT : MODE_GP;
            race_setup();
        }
        else if(row == ROW_CC) {
            game.cc = (game.cc + dir + CC_COUNT) % CC_COUNT;
        }
        else if(row == ROW_COLOUR1 || row == ROW_COLOUR2) {
            step_colour(row == ROW_COLOUR2, dir);
        }
        if(row != ROW_START) audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
    }
    /* Player 2 picks their own colour with their pad at any time. */
    if(game.num_players > 1) {
        int dir2 = input_left(1) ? -1 : input_right(1) ? 1 : 0;
        if(dir2) step_colour(1, dir2);
    }
    if(pad.pressed & BTN_B) {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        game_set_state(ST_TITLE);
        return;
    }
    if(pad.pressed & (BTN_A | BTN_START)) {
        if(row != ROW_START && !(pad.pressed & BTN_START)) {
            menu_row++;
            audio_sfx(DK_SFX_MENU_MOVE, 200, 128);
        }
#ifndef DK_AUTOTEST
        else if(game.num_players > 1 && !pad2.connected) {
            warn_t = 2.0f;
            audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        }
#endif
        else {
            audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
            audio_sfx(DK_SFX_CHAO_HAPPY, 200, 128);
            race_setup();
            game_set_state(ST_INTRO);
        }
    }
}

void title_camera(Camera *cam, float dt) {
    (void)dt;
    const Kart *p = &karts[game.player];
    float a = game.title_t * 0.22f + 2.3f;
    float r = 8.5f + 1.5f * sinf(game.title_t * 0.13f);
    V3 target = vadd(p->pos, v3(0, 1.0f, 0));
    if(game.state == ST_MENU) {
        /* Close-up showing off the driver, framed to the left of the menu. */
        a = game.title_t * 0.35f;
        r = 5.0f;
    }
    cam->pos = vadd(target, v3(sinf(a) * r, 2.4f + 0.8f * sinf(game.title_t * 0.17f), cosf(a) * r));
    cam->fwd = vnorm(vsub(target, cam->pos));
    if(game.state == ST_MENU) {
        /* Shift the view so the kart sits left of centre. */
        V3 right = vnorm(vcross(v3(0, 1, 0), cam->fwd));
        cam->fwd = vnorm(vmad(cam->fwd, right, 0.28f));
    }
    cam->right = vnorm(vcross(v3(0, 1, 0), cam->fwd));
    cam->up = vcross(cam->fwd, cam->right);
    cam->fov = 1.20f;
}

static void draw_sunburst(float cx, float cy, float radius, float spin, float alpha) {
    r_hdr(h_flat);
    const int rays = 16;
    for(int i = 0; i < rays; i++) {
        float a0 = spin + (float)i / rays * DK_TAU, a1 = a0 + DK_TAU / rays * 0.5f;
        uint32_t inner = color_alpha((i & 1) ? argb(255, 255, 240, 120) : argb(255, 255, 190, 60), alpha * 0.85f);
        uint32_t outer = color_alpha((i & 1) ? argb(255, 255, 200, 60) : argb(255, 255, 150, 30), 0.0f);
        r_tri2d(cx, cy, cx + cosf(a0) * radius, cy + sinf(a0) * radius * 0.62f,
                cx + cosf(a1) * radius, cy + sinf(a1) * radius * 0.62f, inner, outer, outer, 10.0f);
    }
}

static void draw_logo(float cx, float cy, float scale) {
    static const char word[] = "DreamKarts";
    float total = 0;
    for(const char *c = word; *c; c++) {
        const char *at = strchr(LOGO_LETTERS, *c);
        total += (float)logo_advance[at - LOGO_LETTERS] * scale;
    }
    float x = cx - total * 0.5f;
    r_hdr(h_logo);
    int i = 0;
    for(const char *c = word; *c; c++, i++) {
        int idx = (int)(strchr(LOGO_LETTERS, *c) - LOGO_LETTERS);
        float adv = (float)logo_advance[idx] * scale;
        /* Letters drop in one after another, then ride a gentle wave. */
        float t = clampf(logo_in * 2.2f - i * 0.11f, 0, 1);
        float drop = (1.0f - t) * (1.0f - t) * -260.0f;
        float settle = t < 1 ? sinf(t * DK_PI) * 0.25f : 0;
        float wave = sinf(game.title_t * 3.0f - i * 0.55f) * 7.0f * scale;
        float s = scale * (1.0f + settle + 0.04f * sinf(game.title_t * 3.0f - i * 0.55f + 1.2f));
        float u0 = (float)(idx % 4) * 0.25f, v0 = (float)(idx / 4) * 0.5f;
        float lx = x + adv * 0.5f, ly = cy + drop + wave;
        float half = LOGO_CELL * 0.5f * s;
        r_quad2d(lx - half, ly - half, lx + half, ly + half, u0, v0, u0 + 0.25f, v0 + 0.5f,
                 color_alpha(0xffffffff, t), 12.0f + i * 0.01f);
        x += adv;
    }
}

static void draw_sparkles(float dt_unused) {
    (void)dt_unused;
    r_hdr(h_star);
    for(int i = 0; i < 8; i++) {
        float t = fmodf(game.title_t * 0.9f + i * 0.37f, 2.2f);
        if(t > 0.9f) continue;
        int seed = (int)((game.title_t * 0.9f + i * 0.37f) / 2.2f) * 31 + i * 17;
        float x = 140 + (float)((seed * 97) % 360);
        float y = 70 + (float)((seed * 53) % 110);
        float s = sinf(t / 0.9f * DK_PI) * 22.0f;
        r_quad2d(x - s, y - s, x + s, y + s, 0, 0, 1, 1, 0xffffffff, 13.0f);
    }
}

static void colour_swatch(float x, float y, int colour) {
    const KartColour *c = &kart_colours[colour];
    r_hdr(h_flat);
    uint32_t kart = c->paint1 ? c->paint1 : 0xffe02828;
    r_quad2d(x - 1, y - 1, x + 33, y + 17, 0, 0, 1, 1, 0xff0c1430, 16.4f);
    r_quad2d(x, y, x + 16, y + 16, 0, 0, 1, 1, kart | 0xff000000, 16.5f);
    r_quad2d(x + 16, y, x + 32, y + 16, 0, 0, 1, 1, c->ui | 0xff000000, 16.5f);
}

static void draw_menu(void) {
    float slide = menu_slide * menu_slide * (3 - 2 * menu_slide);
    float x0 = 640 - 300 * slide, x1 = x0 + 280;
    int rows[8], n = menu_rows(rows);
    const float pitch = 50.0f;
    float top = 112, y_first = 166;
    float bottom = y_first + n * pitch + 6;
    hud_panel(x0, top, x1, bottom, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    hud_text((x0 + x1) * 0.5f, top + 6, 0.9f, ALIGN_CENTER, argb(255, 255, 220, 60), "RACE SETUP");
    for(int i = 0; i < n; i++) {
        int row = rows[i];
        bool sel = i == menu_row;
        float y = y_first + i * pitch;
        if(sel) {
            r_hdr(h_flat);
            float glow = 0.18f + 0.1f * sinf(game.title_t * 7);
            r_quad2d(x0 + 10, y - 6, x1 - 10, y + 42, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
        }
        uint32_t col = sel ? argb(255, 255, 255, 150) : argb(255, 200, 220, 255);
        uint32_t label = argb(255, 150, 200, 255);
        const char *value = NULL;
        switch(row) {
            case ROW_PLAYERS:
                hud_text(x0 + 20, y - 6, 0.5f, ALIGN_LEFT, label, "PLAYERS");
                value = game.num_players > 1 ? "2P SPLIT" : "1P";
                break;
            case ROW_MODE:
                hud_text(x0 + 20, y - 6, 0.5f, ALIGN_LEFT, label, "MODE");
                value = mode_name();
                break;
            case ROW_CC:
                hud_text(x0 + 20, y - 6, 0.5f, ALIGN_LEFT, label, "ENGINE CLASS");
                value = cc_names[game.cc];
                for(int p = 0; p < 3; p++) {
                    r_hdr(h_flat);
                    uint32_t pc = p <= game.cc ? argb(255, 255, 200, 40) : argb(120, 255, 255, 255);
                    r_quad2d(x1 - 60 + p * 14, y + 30 - p * 4, x1 - 51 + p * 14, y + 36, 0, 0, 1, 1, pc, 16.5f);
                }
                break;
            case ROW_COLOUR1:
            case ROW_COLOUR2: {
                int pl = row == ROW_COLOUR2;
                hud_text(x0 + 20, y - 6, 0.5f, ALIGN_LEFT, label,
                         game.num_players > 1 ? (pl ? "P2 COLOUR" : "P1 COLOUR") : "COLOUR");
                value = kart_colours[game.pl[pl].colour].name;
                colour_swatch(x1 - 58, y - 4, game.pl[pl].colour);
                break;
            }
            default: {
                float s = sel ? 1.1f + 0.06f * sinf(game.title_t * 8) : 0.95f;
                hud_text((x0 + x1) * 0.5f, y, s, ALIGN_CENTER, sel ? argb(255, 255, 230, 60) : col, "START!");
                break;
            }
        }
        if(value) hud_text((x0 + x1) * 0.5f, y + 10, 0.75f, ALIGN_CENTER, col, value);
        if(sel && row != ROW_START) {
            float bob = 3.0f * sinf(game.title_t * 9);
            hud_text(x0 + 22 - bob, y + 10, 0.75f, ALIGN_LEFT, col, "<");
            hud_text(x1 - 22 + bob, y + 10, 0.75f, ALIGN_RIGHT, col, ">");
        }
    }
    if(game.num_players > 1)
        hud_text(24, 404, 0.45f, ALIGN_LEFT, argb(230, 255, 255, 255), "P2: LEFT/RIGHT = COLOUR");
    hud_text(24, 428, 0.5f, ALIGN_LEFT, argb(230, 255, 255, 255), "A: OK   B: BACK");
    if(warn_t > 0 && ((int)(game.title_t * 6) & 1))
        hud_text(320, 72, 0.7f, ALIGN_CENTER, 0xffff8080, "CONNECT CONTROLLER 2");
}

void title_draw(void) {
    if(game.state == ST_TITLE) {
        float burst_a = clampf(logo_in * 1.5f, 0, 1);
        draw_sunburst(320, 118, 520, game.title_t * 0.25f, burst_a * 0.55f);
        r_hdr(h_glow);
        r_quad2d(320 - 260, 118 - 100, 320 + 260, 118 + 100, 0, 0, 1, 1, color_alpha(argb(255, 255, 230, 160), 0.35f * burst_a), 11.0f);
        draw_logo(320, 118, 0.66f);
        draw_sparkles(0);
        if(logo_in >= 1.0f) {
            float pulse = 0.5f + 0.5f * sinf(game.title_t * 4.5f);
            float s = 1.0f + 0.06f * pulse;
            hud_text(320, 352, s, ALIGN_CENTER, color_lerp(argb(255, 255, 255, 255), argb(255, 255, 210, 60), pulse),
                     "PRESS START");
        }
        hud_text(320, 196, 0.62f, ALIGN_CENTER, argb(255, 255, 255, 255), "EMERALD BEACH GRAND PRIX");
        hud_text(320, 440, 0.45f, ALIGN_CENTER, argb(200, 230, 240, 255), "github.com/richstokes - 2026");
    }
    else {
        draw_logo(180, 62, 0.36f);
        draw_menu();
    }
}
