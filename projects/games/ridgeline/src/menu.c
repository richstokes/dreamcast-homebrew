/* Ridgeline menus: the title and main menu over a flyover of the mountain, picking a
   truck, its paint and company with the truck parked in the meadow, and
   the pause menu. */
#include "menu.h"
#include "hud.h"
#include "truck.h"
#include "camera.h"
#include "audio.h"
#include "demo.h"
#include "sound_assets.h"
#include "version.h"
#if __has_include("trucks.h")
#include "trucks.h"
#define HAVE_TRUCK_MODELS 1
#endif

static const pvr_poly_hdr_t *h_logo;
static int sel_row;

void menu_init(void) {
    h_logo = r_header(TEX_LOGO, HF_TR | HF_NOFOG | HF_CLAMP);
}

static const char *livery_name(int model, int lv) {
#ifdef HAVE_TRUCK_MODELS
    return rl_truck_info[model].livery_name[lv % RL_LIVERIES];
#else
    static const char *const n[4] = {"FACTORY", "FOREST", "SUNSET", "SLATE"};
    (void)model;
    return n[lv & 3];
#endif
}

const char *menu_truck_name(int model) {
#ifdef HAVE_TRUCK_MODELS
    return rl_truck_info[model].name;
#else
    return model ? "KESTREL" : "CASCADE 84";
#endif
}

/* Start brings up the main menu under the logo; it stays up when coming
   back from the truck select or the sound test. */
enum { T_SINGLE, T_ONLINE, T_SOUND, T_COUNT };
static bool title_menu;
static int title_sel;
static float title_idle;

void menu_title_update(float dt) {
    camera_flyover(game.time);
    /* Left alone with no menu up, the demo starts (demo.h). */
    bool touched = pad.pressed || pad2.pressed || fabsf(pad.stick_x) > 0.6f || fabsf(pad.stick_y) > 0.6f;
    title_idle = touched ? 0 : title_idle + dt;
    if(!title_menu) {
        if(title_idle >= DEMO_IDLE_SECONDS) {
            title_idle = 0;
            demo_start();
            return;
        }
        if(pad.pressed & (BTN_START | BTN_A)) {
            audio_sfx(RL_SFX_MENU_SELECT, 200);
            title_menu = true;
            title_sel = T_SINGLE;
        }
        return;
    }
    if(input_menu_up()) {
        title_sel = (title_sel + T_COUNT - 1) % T_COUNT;
        audio_sfx(RL_SFX_MENU_MOVE, 160);
    }
    if(input_menu_down()) {
        title_sel = (title_sel + 1) % T_COUNT;
        audio_sfx(RL_SFX_MENU_MOVE, 160);
    }
    if(pad.pressed & BTN_B) {
        audio_sfx(RL_SFX_MENU_BACK, 200);
        title_menu = false;
        return;
    }
    if(pad.pressed & (BTN_START | BTN_A)) {
        if(title_sel == T_SINGLE) {
            audio_sfx(RL_SFX_MENU_SELECT, 200);
            game_set_state(ST_SELECT);
            sel_row = 0;
        }
        else if(title_sel == T_SOUND) {
            audio_sfx(RL_SFX_MENU_SELECT, 200);
            game_set_state(ST_SOUND);
        }
        /* Online: not yet. */
    }
}

void menu_title_draw(void) {
    float k = clampf(game.time / 1.2f, 0, 1);
    r_hdr(h_logo);
    float w = 256 * (0.9f + 0.1f * k), h = 64 * (0.9f + 0.1f * k);
    r_quad2d(320 - w, 150 - h, 320 + w, 150 + h, 0, 0, 1, 1, color_alpha(0xffffffff, k), 12.0f);
    hud_text_c(320, 214, 0.72f, ALIGN_CENTER, color_alpha(0xfff8f0dc, k), "CASCADE RIDGE  -  A TRAIL TO THE SUMMIT");
    if(title_menu) {
        static const char *const items[T_COUNT] = {"SINGLE PLAYER", "ONLINE", "SOUND TEST"};
        hud_panel(200, 278, 440, 404, color_alpha(0xb0182028, k));
        for(int i = 0; i < T_COUNT; i++) {
            float y = 304 + i * 37;
            bool on = i == title_sel, off = i == T_ONLINE;
            if(on) hud_panel(206, y - 15, 434, y + 15, off ? 0xc0706860 : 0xd0e8a030);
            uint32_t c = off ? (on ? 0xffc8c0b0 : 0xff807868) : on ? 0xff201810 : 0xfff4ecd8;
            hud_text_c(320, y, 0.72f, ALIGN_CENTER, color_alpha(c, k), items[i]);
        }
        if(title_sel == T_ONLINE)
            hud_text_c(320, 426, 0.5f, ALIGN_CENTER, color_alpha(0xffb8b0a0, k), "COMING SOON");
    }
    else if(fmodf(game.time, 1.2f) < 0.8f)
        hud_text_c(320, 380, 0.9f, ALIGN_CENTER, color_alpha(0xffffffff, k), "PRESS START");
    hud_text_c(624, 462, 0.42f, ALIGN_RIGHT, 0xa0ffffff, RL_GIT_SHA);
}

/* The truck parked in the meadow, the camera circling it. */
static void select_scene(float dt) {
    Truck *t = &trucks[0];
    TruckInput none = {0};
    t->in = none;
    t->lights = fmodf(game.time, 8.0f) > 4.0f;
    truck_update(t, dt);
    float a = game.time * 0.25f + 0.9f;
    V3 c = t->pos;
    V3 p = v3(c.x + sinf(a) * 7.5f, c.y + 1.4f, c.z + cosf(a) * 7.5f);
    camera_set(p, vadd(c, v3(0, 0.1f, 0)), 1.1f);
}

void menu_select_update(float dt) {
    if(input_menu_up()) {
        sel_row = (sel_row + 3) % 4;
        audio_sfx(RL_SFX_MENU_MOVE, 160);
    }
    if(input_menu_down()) {
        sel_row = (sel_row + 1) % 4;
        audio_sfx(RL_SFX_MENU_MOVE, 160);
    }
    int d = input_menu_right() ? 1 : input_menu_left() ? -1 : 0;
    if(d) {
        audio_sfx(RL_SFX_MENU_MOVE, 160);
        if(sel_row == 0) {
            game.truck_model ^= 1;
            trucks[0].model = game.truck_model;
            trucks[0].livery = game.livery;
            truck_place(&trucks[0], trucks[0].pos, 0);
        }
        else if(sel_row == 1) {
            game.livery = (game.livery + 4 + d) % 4;
            trucks[0].livery = game.livery;
        }
        else if(sel_row == 2) game.companions = (game.companions + 4 + d) % 4;
    }
    if(pad.pressed & BTN_B) {
        audio_sfx(RL_SFX_MENU_BACK, 200);
        game_set_state(ST_TITLE);
        return;
    }
    if((pad.pressed & BTN_START) || ((pad.pressed & BTN_A) && sel_row == 3)) {
        audio_sfx(RL_SFX_MENU_SELECT, 220);
        game_start_drive();
        return;
    }
    select_scene(dt);
}

void menu_select_draw(void) {
    static const char *const company[4] = {"SOLO", "1 CPU TRUCK", "2 CPU TRUCKS", "3 CPU TRUCKS"};
    const char *rows[4][2] = {
        {"TRUCK", menu_truck_name(game.truck_model)},
        {"PAINT", livery_name(game.truck_model, game.livery)},
        {"COMPANY", company[game.companions]},
        {"", "DRIVE!"},
    };
    hud_panel(40, 300, 430, 452, 0xb0182028);
    for(int r = 0; r < 4; r++) {
        float y = 324 + r * 36;
        bool on = r == sel_row;
        if(on) hud_panel(44, y - 15, 426, y + 15, 0xd0e8a030);
        uint32_t c = on ? 0xff201810 : 0xfff4ecd8;
        hud_text_c(58, y, 0.66f, ALIGN_LEFT, on ? 0xff403020 : 0xffb8b0a0, rows[r][0]);
        if(r < 3) {
            hud_text_c(297, y, 0.64f, ALIGN_CENTER, c, rows[r][1]);
            hud_text_c(190, y, 0.72f, ALIGN_CENTER, c, "<");
            hud_text_c(408, y, 0.72f, ALIGN_CENTER, c, ">");
        }
        else hud_text_c(235, y, 0.8f, ALIGN_CENTER, c, rows[r][1]);
    }
    hud_text_c(320, 40, 0.8f, ALIGN_CENTER, 0xfff8f0dc, "CHOOSE YOUR TRUCK");
}

/* ---- Pause ---- */

enum { P_CARRY_ON, P_RECOVER, P_FPS, P_HUD, P_QUIT, P_COUNT };

void menu_pause_update(float dt) {
    if(game.photo) {
        /* Photo mode: the stick orbits, B, Y or Start goes back. */
        if(game.cam_scripted) return;
        game.photo_t += dt;
        if(pad.pressed & (BTN_B | BTN_Y | BTN_START)) {
            game.photo = false;
            audio_sfx(RL_SFX_MENU_BACK, 160);
            return;
        }
        camera_photo(&trucks[0], dt);
        return;
    }
    if(input_menu_up()) game.pause_sel = (game.pause_sel + P_COUNT - 1) % P_COUNT;
    if(input_menu_down()) game.pause_sel = (game.pause_sel + 1) % P_COUNT;
    if(pad.pressed & (BTN_START | BTN_B)) {
        game.paused = false;
        return;
    }
    if(pad.pressed & BTN_Y) {
        game.photo = true;
        game.photo_t = 0;
        camera_photo_begin(&trucks[0]);
        audio_sfx(RL_SFX_MENU_SELECT, 160);
        return;
    }
    if(pad.pressed & BTN_A) {
        audio_sfx(RL_SFX_MENU_SELECT, 200);
        switch(game.pause_sel) {
            case P_CARRY_ON: game.paused = false; break;
            case P_RECOVER: truck_recover(&trucks[0]); camera_reset(); game.paused = false; break;
            case P_FPS: game.show_fps = !game.show_fps; break;
            case P_HUD: game.hud_hidden = !game.hud_hidden; break;
            case P_QUIT: game.paused = false; audio_drive_stop(); game_set_state(ST_TITLE); break;
        }
    }
}

void menu_pause_draw(void) {
    if(game.photo) {
        /* Nothing on screen but a hint that fades after a moment. */
        float a = clampf(2.5f - game.photo_t, 0, 1);
        if(a > 0 && !game.cam_scripted)
            hud_text_c(320, 452, 0.55f, ALIGN_CENTER, color_alpha(0xfff8f0dc, a),
                       "PHOTO MODE   STICK: TURN   R/L: ZOOM   B: BACK");
        return;
    }
    const char *items[P_COUNT] = {"CARRY ON", "RECOVER TRUCK", game.show_fps ? "FPS COUNTER: ON" : "FPS COUNTER: OFF",
                                  game.hud_hidden ? "HUD: OFF" : "HUD: ON", "QUIT TO TITLE"};
    hud_panel(190, 134, 450, 380, 0xc0182028);
    hud_text_c(320, 160, 0.9f, ALIGN_CENTER, 0xfff8f0dc, "PAUSED");
    for(int i = 0; i < P_COUNT; i++) {
        float y = 204 + i * 32;
        bool on = i == game.pause_sel;
        if(on) hud_panel(202, y - 14, 438, y + 14, 0xd0e8a030);
        hud_text_c(320, y, 0.7f, ALIGN_CENTER, on ? 0xff201810 : 0xfff4ecd8, items[i]);
    }
    hud_text_c(320, 362, 0.55f, ALIGN_CENTER, 0xffd8d0c0, "Y: PHOTO MODE");
}
