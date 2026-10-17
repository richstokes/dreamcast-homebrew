/* Title screen: the track in the background with the grid lined up, a
   rotating Sega-style sunburst, a bouncing per-letter logo, sparkles and a
   pulsing PRESS START. Then a slide-in menu for mode and engine class, and
   a track select that flies over each course as it is picked. */
#include "title.h"
#include "audio.h"
#include "hud.h"
#include "kart.h"
#include "render.h"
#include "track.h"
#include "online.h"
#include "texture_assets.h"
#include "font_metrics.h"

static const pvr_poly_hdr_t *h_logo, *h_flat, *h_star, *h_glow;
static float logo_in;        /* 0..1 drop-in animation */
static float menu_slide;
static int menu_row;
static float sparkle_t[8];
static float chao_call_t;
static float tracks_slide;   /* track select panel slide-in */
static float track_swap;     /* card slide when the track changes, +-1 -> 0 */
static float fly_t;          /* time since the flyover camera last reset */

static void tracks_update(float dt);

/* Main menu: single player, two-player split screen or online. */
static int main_row;
static const char *const main_items[3] = {"SINGLE PLAYER", "TWO PLAYER", "ONLINE MULTIPLAYER"};

static void main_update(void) {
    menu_slide = fminf(1.0f, menu_slide + DK_DT * 4.0f);
    if(input_menu_up()) {
        main_row = (main_row + 2) % 3;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_menu_down()) {
        main_row = (main_row + 1) % 3;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(pad.pressed & BTN_B) {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        game_set_state(ST_TITLE);
        return;
    }
    if(!(pad.pressed & (BTN_A | BTN_START))) return;
#if defined(DK_AT_2P) && defined(DK_CAPTURE)
    main_row = 1;   /* scripted two-player captures and benchmarks */
#endif
    audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
    audio_sfx(DK_SFX_WHOOSH, 160, 128);
    if(main_row == 2) {
        online_enter();
        return;
    }
    game.num_players = main_row + 1;
    if(game.num_players > 1 && game.pl[1].colour == game.pl[0].colour)
        game.pl[1].colour = (game.pl[0].colour + 1) % COLOUR_COUNT;
    race_setup();
    game_set_state(ST_MENU);
    menu_row = 0;
}

static const char *const cc_names[] = {"50cc", "100cc", "150cc"};
static float warn_t;

enum { ROW_MODE, ROW_CC, ROW_COLOUR1, ROW_COLOUR2, ROW_START };

/* Menu rows: player 2's colour only appears in a two-player race. */
static int menu_rows(int *rows) {
    int n = 0;
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
            game_set_state(ST_MAIN);
            main_row = 0;
        }
        return;
    }
    if(game.state == ST_MAIN) {
        main_update();
        return;
    }
    if(game.state == ST_TRACKS) {
        tracks_update(dt);
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
        if(row == ROW_MODE) {
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
        game_set_state(ST_MAIN);
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
            audio_sfx(DK_SFX_WHOOSH, 160, 128);
            race_setup();
            game_set_state(ST_TRACKS);
            tracks_slide = 0;
            track_swap = 0;
            fly_t = 0;
        }
    }
}

static void tracks_update(float dt) {
    tracks_slide = fminf(1.0f, tracks_slide + dt * 3.5f);
    track_swap = approach(track_swap, 0.0f, dt * 4.0f);
    fly_t += dt;
    int dir = input_menu_left() ? -1 : input_menu_right() ? 1 : 0;
    if(dir) {
        game_load_track(game.track + dir);
        track_swap = (float)dir;
        fly_t = 0;
        audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
        audio_sfx(DK_SFX_WHOOSH, 140, dir < 0 ? 90 : 166);
    }
    if(pad.pressed & BTN_B) {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        game_set_state(ST_MENU);
        return;
    }
    if(pad.pressed & (BTN_A | BTN_START)) {
        audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
        audio_sfx(DK_SFX_CHAO_HAPPY, 200, 128);
        race_setup();
        game_set_state(ST_INTRO);
    }
}

void title_camera(Camera *cam, float dt) {
    (void)dt;
    if(game.state == ST_TRACKS) {
        /* High flyover circling the course, swooping in after each pick. */
        float a = game.title_t * 0.07f;
        float in = clampf(fly_t * 0.8f, 0, 1);
        in = in * in * (3 - 2 * in);
        float r = lerpf(260.0f, 190.0f, in), h = lerpf(130.0f, 92.0f, in);
        cam->pos = v3(sinf(a) * r, h, cosf(a) * r * 0.8f);
        V3 target = v3(0, 0, 0);
        cam->fwd = vnorm(vsub(target, cam->pos));
        cam->right = vnorm(vcross(v3(0, 1, 0), cam->fwd));
        cam->up = vcross(cam->fwd, cam->right);
        /* Tilt down a little so the course sits above the info panel. */
        cam->fwd = vnorm(vmad(cam->fwd, cam->up, -0.16f));
        cam->up = vcross(cam->fwd, cam->right);
        cam->fov = 1.10f;
        return;
    }
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

static const char *const loading_tips[] = {
    "TIP: HOLD R AS YOU TURN IN TO POWERSLIDE",
    "TIP: RELEASE A DRIFT ON PURPLE SPARKS FOR THE BIGGEST BOOST",
    "TIP: PUSH AWAY FROM THE TURN TO WIDEN A SLIDE",
    "TIP: PRESS A AS THE 2 FADES FOR A ROCKET START",
    "TIP: HOLD R WITHOUT STEERING TO CHARGE A JUMP",
    "TIP: BOOSTS CUT STRAIGHT THROUGH SAND",
};

void title_draw_loading(float progress, const char *what, float t) {
    static int tip = -1;
    if(tip < 0) tip = (int)(dk_rand() % (sizeof(loading_tips) / sizeof(loading_tips[0])));
    /* Boot steps are too far apart to animate the letters dropping in, so
       the logo sits settled; the title screen drops it in afterwards. */
    game.title_t = t;
    logo_in = 1.0f;

    /* Sky-blue backdrop under a slowly turning sunburst and the logo. */
    r_hdr(h_flat);
    r_quad2d_grad(0, 0, 640, 480, argb(255, 60, 150, 240), argb(255, 12, 40, 130), 1.0f);
    draw_sunburst(320, 170, 560, t * 0.25f, 0.32f);
    r_hdr(h_glow);
    r_quad2d(320 - 250, 170 - 95, 320 + 250, 170 + 95, 0, 0, 1, 1, color_alpha(argb(255, 255, 230, 160), 0.3f), 11.0f);
    draw_logo(320, 170, 0.62f);

    /* Progress bar: a little racetrack with a chequered finish and a
       glowing marker riding the leading edge. */
    float x0 = 130, x1 = 510, y0 = 318, y1 = 336;
    float fill = x0 + (x1 - x0) * clampf(progress, 0, 1);
    r_hdr(h_flat);
    r_quad2d(x0 - 4, y0 - 4, x1 + 18, y1 + 4, 0, 0, 1, 1, argb(230, 8, 20, 60), 14.0f);
    r_quad2d_grad(x0, y0, x1, y1, argb(255, 70, 74, 84), argb(255, 44, 46, 54), 14.1f);
    if(fill > x0) r_quad2d_grad(x0, y0, fill, y1, argb(255, 255, 226, 70), argb(255, 255, 140, 30), 14.2f);
    for(int r = 0; r < 3; r++)
        for(int c = 0; c < 2; c++) {
            float cx = x1 + 2 + c * 7, cy = y0 + r * 6;
            r_quad2d(cx, cy, cx + 7, cy + 6, 0, 0, 1, 1, ((r + c) & 1) ? 0xff101010 : 0xfff4f4f4, 14.3f);
        }
    float pulse = 0.75f + 0.25f * sinf(t * 9.0f);
    r_hdr(h_glow);
    r_quad2d(fill - 16, (y0 + y1) * 0.5f - 16, fill + 16, (y0 + y1) * 0.5f + 16, 0, 0, 1, 1,
             color_alpha(argb(255, 255, 250, 210), pulse), 14.4f);

    hud_text(320, 346, 0.55f, ALIGN_CENTER, 0xffffffff, what);
    hud_text(320, 420, 0.42f, ALIGN_CENTER, argb(220, 200, 225, 255), loading_tips[tip]);
}

static void colour_swatch(float x, float y, int colour) {
    const KartColour *c = &kart_colours[colour];
    r_hdr(h_flat);
    uint32_t kart = c->paint1 ? c->paint1 : 0xffe02828;
    r_quad2d(x - 1, y - 1, x + 33, y + 17, 0, 0, 1, 1, 0xff0c1430, 16.4f);
    r_quad2d(x, y, x + 16, y + 16, 0, 0, 1, 1, kart | 0xff000000, 16.5f);
    r_quad2d(x + 16, y, x + 32, y + 16, 0, 0, 1, 1, c->ui | 0xff000000, 16.5f);
}

static void draw_main(void) {
    float slide = menu_slide * menu_slide * (3 - 2 * menu_slide);
    float y0 = 196 + (1 - slide) * 300;
    hud_panel(150, y0, 490, y0 + 200, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    for(int i = 0; i < 3; i++) {
        bool sel = i == main_row;
        float y = y0 + 22 + i * 58;
        if(sel) {
            r_hdr(h_flat);
            float glow = 0.18f + 0.1f * sinf(game.title_t * 7);
            r_quad2d(166, y - 6, 474, y + 42, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
        }
        float s = sel ? 0.86f + 0.04f * sinf(game.title_t * 8) : 0.8f;
        hud_text(320, y + 2, s, ALIGN_CENTER, sel ? argb(255, 255, 230, 60) : argb(255, 200, 220, 255), main_items[i]);
    }
    static const char *const blurbs[3] = {"RACE FIVE CPU CHAO", "SPLIT SCREEN WITH A FRIEND",
                                          "UP TO SIX PLAYERS (BROADBAND OR DREAMPI)"};
    hud_text(320, 440, 0.45f, ALIGN_CENTER, argb(220, 230, 240, 255), blurbs[main_row]);
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
                hud_text((x0 + x1) * 0.5f, y, s, ALIGN_CENTER, sel ? argb(255, 255, 230, 60) : col, "NEXT");
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

static void draw_tracks(void) {
    float slide = tracks_slide * tracks_slide * (3 - 2 * tracks_slide);
    float off = (1.0f - slide) * 220.0f;
    /* Header. */
    hud_panel(190, 18 - off * 0.4f, 450, 66 - off * 0.4f, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    hud_text(320, 22 - off * 0.4f, 0.95f, ALIGN_CENTER, argb(255, 255, 220, 60), "SELECT TRACK");

    /* Info card: map on the left, name and details on the right. */
    float swap = track_swap * track_swap * track_swap * 260.0f;
    float y0 = 296 + off, y1 = 428 + off;
    hud_panel(52, y0, 588, y1, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    float mx = 160 + swap, my = (y0 + y1) * 0.5f;
    hud_panel(mx - 98, y0 + 10, mx + 98, y1 - 10, argb(200, 8, 24, 70), argb(200, 8, 24, 70), 15.5f);
    hud_draw_track_map(mx, my, 0.56f, 16.0f, false);

    float tx = 412 + swap;
    float pulse = 1.0f + 0.03f * sinf(game.title_t * 5.0f);
    hud_text(tx, y0 + 6, 0.95f * pulse, ALIGN_CENTER, argb(255, 255, 226, 70), track.name);
    hud_text(tx, y0 + 46, 0.55f, ALIGN_CENTER, argb(255, 200, 225, 255), track.blurb);
    char buf[48];
    snprintf(buf, sizeof(buf), "LAP %d m  -  %d LAPS", (int)(track.length + 0.5f), game.laps);
    hud_text(tx, y0 + 74, 0.5f, ALIGN_CENTER, 0xffffffff, buf);
    /* Page dots. */
    r_hdr(h_flat);
    for(int i = 0; i < TRACK_COUNT; i++) {
        float dx = tx + ((float)i - (TRACK_COUNT - 1) * 0.5f) * 18.0f;
        uint32_t c = i == game.track ? argb(255, 255, 220, 60) : argb(140, 255, 255, 255);
        float r = i == game.track ? 5.0f : 3.5f;
        r_quad2d(dx - r, y1 - 20 - r, dx + r, y1 - 20 + r, 0, 0, 1, 1, c, 16.5f);
    }
    float bob = 4.0f * sinf(game.title_t * 7);
    hud_text(36 - bob, my - 22, 1.3f, ALIGN_CENTER, argb(255, 255, 255, 150), "<");
    hud_text(604 + bob, my - 22, 1.3f, ALIGN_CENTER, argb(255, 255, 255, 150), ">");
    hud_text(320, 440, 0.5f, ALIGN_CENTER, argb(230, 255, 255, 255), "LEFT/RIGHT: TRACK   A: RACE!   B: BACK");
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
        char sub[48];
        snprintf(sub, sizeof(sub), "%s GRAND PRIX", track.name);
        hud_text(320, 196, 0.62f, ALIGN_CENTER, argb(255, 255, 255, 255), sub);
        hud_text(320, 440, 0.45f, ALIGN_CENTER, argb(200, 230, 240, 255), "github.com/richstokes - 2026");
    }
    else if(game.state == ST_TRACKS) {
        draw_tracks();
    }
    else if(game.state == ST_MAIN) {
        draw_sunburst(320, 112, 420, game.title_t * 0.25f, 0.4f);
        draw_logo(320, 112, 0.5f);
        draw_main();
    }
    else {
        draw_logo(180, 62, 0.36f);
        draw_menu();
    }
}
