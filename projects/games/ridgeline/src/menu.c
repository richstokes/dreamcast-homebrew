/* Ridgeline menus: the title and main menu over a flyover of the mountain,
   picking an area, then a truck, its paint and company with the truck
   parked at the area's start, and the pause menu. */
#include "menu.h"
#include "records.h"
#include "tod.h"
#include "hud.h"
#include "engine/ui.h"
#include "truck.h"
#include "camera.h"
#include "terrain.h"
#include "fx.h"
#include "audio.h"
#include "demo.h"
#include "online.h"
#include "sound_assets.h"
#include "version.h"
#if __has_include("trucks.h")
#include "trucks.h"
#define HAVE_TRUCK_MODELS 1
#endif

static const pvr_poly_hdr_t *h_logo;
static int sel_row;
/* The truck select's page (menu_select_update). */
enum { PG_AREA, PG_TRUCK, PG_DRIVE };
static int sel_page;
static int area_pick;   /* the area page's choice, loaded on the way to the truck page */

/* The chrome's colours (engine/ui.h): smoke panels, amber plaques, cream
   lettering, white lettering on the amber (dark lettering blurs into the
   font's dark outline there). */
#define C(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
static const EngUiTheme theme = {
    .roles = {
        [UI_TITLE] = C(255, 255, 255, 255),   [UI_ITEM] = C(255, 244, 236, 216),     [UI_ITEM_SEL] = C(255, 255, 255, 255),
        [UI_LABEL] = C(255, 184, 176, 160),   [UI_VALUE] = C(255, 244, 236, 216),    [UI_VALUE_SEL] = C(255, 255, 255, 255),
        [UI_DIM] = C(255, 184, 176, 160),     [UI_TEXT] = C(255, 248, 240, 220),     [UI_HINT] = C(255, 244, 236, 216),
        [UI_ACCENT] = C(255, 232, 160, 48),   [UI_GOOD] = C(255, 144, 240, 176),     [UI_WARN] = C(255, 255, 144, 80),
    },
    .outline = C(255, 12, 16, 20),
    .shadow = C(96, 0, 0, 0),
    .panel_top = C(208, 52, 60, 68), .panel_bottom = C(208, 24, 32, 40),
    .well_top = C(255, 16, 20, 24), .well_bottom = C(255, 12, 16, 20),
    .plaque_top = C(255, 240, 176, 64), .plaque_bottom = C(255, 208, 128, 32), .plaque_glow = C(255, 252, 200, 96),
    .button_top = C(255, 84, 92, 100), .button_bottom = C(255, 48, 56, 64),
    .hint_top = C(224, 52, 60, 68), .hint_bottom = C(224, 24, 32, 40),
    .dim = C(128, 0, 0, 0),
};

void menu_init(void) {
    h_logo = r_header(TEX_LOGO, HF_TR | HF_NOFOG | HF_CLAMP);
    ui_init(hud_font(), &theme);
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
    static const char *const n[TRUCK_MODELS] = {"CASCADE 84", "KESTREL", "BOBCAT", "FFR", "TACO", "GOOSE", "ONE TEN",
                                                 "EAGLE"};
    return n[model % TRUCK_MODELS];
#endif
}

/* Start brings up the main menu under the logo; it stays up when coming
   back from the truck select or the sound test. */
enum { T_SINGLE, T_ONLINE, T_SOUND, T_CONTROLS, T_COUNT };
static bool title_menu, title_controls;
static int title_sel;
static float title_idle;

/* The controls screen: a read-only list over the flyover. Keep it in step
   with player_controls() in main.c and the README's table. A row with no
   action is a section heading. */
typedef struct {
    const char *key;     /* lettering, drawn before the glyph */
    const char *glyph;   /* a button glyph (engine/ui.h), or NULL */
    const char *does;
} ControlRow;

static const ControlRow controls[] = {
    {"DRIVING", NULL, NULL},
    {"STICK", NULL, "STEER"},
    {"R TRIGGER", NULL, "THROTTLE"},
    {"L TRIGGER", NULL, "BRAKE, HOLD AT A STOP TO REVERSE"},
    {"D-PAD LEFT/RIGHT", NULL, "DRIVE MODE: 2WD, 4WD, LOCK"},
    {"HOLD D-PAD LEFT", NULL, "TRACTION CONTROL OFF/ON (KESTREL, TACO)"},
    {"D-PAD UP", NULL, "HORN"},
    {"D-PAD DOWN", NULL, "LIGHTS: OFF, DIPPED, FULL BEAM"},
    {"", "A", "HANDBRAKE"},
    {"", "B", "CAMERA: CHASE, FAR CHASE, BUMPER"},
    {"", "X", "MAP: CORNER, LARGE, OFF"},
    {"HOLD", "Y", "RECOVER TO THE NEAREST TRAIL"},
    {"", "START", "PAUSE MENU"},
    {"PAUSED", NULL, NULL},
    {"", "Y", "PHOTO MODE"},
    {"PHOTO MODE", NULL, "STICK: TURN, R/L: ZOOM, D-PAD: CLOCK"},
};
#define CONTROL_ROWS (int)(sizeof(controls) / sizeof(controls[0]))

static void controls_draw(void) {
    const float scale = 0.5f, kx = 64, dx = 252, step = 21;
    ui_heading(320, 40, "CONTROLS");
    ui_panel(44, 72, 596, 82 + CONTROL_ROWS * step + 8);
    float y = 92;
    for(int i = 0; i < CONTROL_ROWS; i++, y += step) {
        const ControlRow *r = &controls[i];
        if(!r->does) {
            hud_text_c(kx, y, 0.56f, ALIGN_LEFT, ui_col(UI_ACCENT), r->key);
            continue;
        }
        float x = kx;
        if(r->key[0]) {
            hud_text_c(x, y, scale, ALIGN_LEFT, ui_col(UI_LABEL), r->key);
            x += text_width(hud_font(), r->key, scale) + 8;
        }
        if(r->glyph) ui_glyph(x, y, r->glyph, true);
        hud_text_c(dx, y, scale, ALIGN_LEFT, ui_col(UI_TEXT), r->does);
    }
    ui_hint("B: BACK");
}

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
    if(title_controls) {
        if(pad.pressed & (BTN_B | BTN_A | BTN_START)) {
            audio_sfx(RL_SFX_MENU_BACK, 200);
            title_controls = false;
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
        }
        else if(title_sel == T_SOUND) {
            audio_sfx(RL_SFX_MENU_SELECT, 200);
            game_set_state(ST_SOUND);
        }
        else if(title_sel == T_CONTROLS) {
            audio_sfx(RL_SFX_MENU_SELECT, 200);
            title_controls = true;
        }
        else {
            audio_sfx(RL_SFX_MENU_SELECT, 200);
            online_enter();
        }
    }
}

void menu_title_draw(void) {
    if(title_menu && title_controls) {
        controls_draw();
        return;
    }
    float k = clampf(game.time / 1.2f, 0, 1);
    r_hdr(h_logo);
    float w = 256 * (0.9f + 0.1f * k), h = 64 * (0.9f + 0.1f * k);
    r_quad2d(320 - w, 96 - h, 320 + w, 96 + h, 0, 0, 1, 1, color_alpha(0xffffffff, k), 12.0f);
    if(title_menu) {
        static const char *const items[T_COUNT] = {"SINGLE PLAYER", "ONLINE", "SOUND TEST", "CONTROLS"};
        ui_panel(200, 252, 440, 415);
        for(int i = 0; i < T_COUNT; i++) {
            float y = 278 + i * 37;
            bool on = i == title_sel;
            ui_row(206, y - 15, 434, y + 15, on);
            hud_text_c(320, y, 0.72f, ALIGN_CENTER, on ? ui_col(UI_ITEM_SEL) : ui_col(UI_ITEM), items[i]);
        }
        ui_hint("A: OK   B: BACK");
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
    t->lights = (uint8_t)(fmodf(game.time, 12.0f) / 4.0f);   /* off, dipped, full beam in turn */
    truck_update(t, dt);
    float a = game.time * 0.25f + 0.9f;
    V3 c = t->pos;
    camera_set(orbit_point(c, a, 7.5f, 1.4f), vadd(c, v3(0, 0.1f, 0)), 1.1f);
}

/* Three pages: the area, with its map and ways up; the truck and its
   paint, so the truck stays in view; then the company and the time to set
   off with the best times. Online it is one page, the truck, its paint
   and the lobby to join: the area and the company come with the lobby,
   and the lobby keeps the time of day. */
#define SEL_LAST (sel_page == PG_AREA ? 1 : 2)   /* a page is its rows and a button */

static void select_back(void) {
    audio_sfx(RL_SFX_MENU_BACK, 200);
    if(game.online) online_back_to_browser();
    else if(sel_page == PG_AREA) game_set_state(ST_TITLE);
    else {
        sel_page--;
        sel_row = 0;
        area_pick = game.area;
    }
}

void menu_select_begin(void) {
    sel_row = 0;
    sel_page = PG_AREA;
    area_pick = game.area;
}

void menu_select_update(float dt) {
    if(game.online && online_joining()) {
        if(pad.pressed & BTN_B) {
            audio_sfx(RL_SFX_MENU_BACK, 200);
            online_back_to_browser();
            return;
        }
        select_scene(dt);
        return;
    }
    if(game.online) sel_page = PG_TRUCK;
    int rows = SEL_LAST + 1;
    if(sel_row >= rows) sel_row = 0;
    if(sel_page == PG_AREA) {
        int da = input_menu_right() ? 1 : input_menu_left() ? -1 : 0;
        if(da && sel_row == 0) {
            audio_sfx(RL_SFX_MENU_MOVE, 160);
            area_pick = (area_pick + AREA_COUNT + da) % AREA_COUNT;
        }
        if(input_menu_up() || input_menu_down()) {
            sel_row ^= 1;
            audio_sfx(RL_SFX_MENU_MOVE, 160);
        }
        if(pad.pressed & BTN_B) {
            select_back();
            return;
        }
        if(pad.pressed & (BTN_A | BTN_START)) {
            audio_sfx(RL_SFX_MENU_SELECT, 200);
            game_load_area(area_pick);
            sel_page = PG_TRUCK;
            sel_row = 0;
        }
        select_scene(dt);
        return;
    }
    if(input_menu_up()) {
        sel_row = (sel_row + rows - 1) % rows;
        audio_sfx(RL_SFX_MENU_MOVE, 160);
    }
    if(input_menu_down()) {
        sel_row = (sel_row + 1) % rows;
        audio_sfx(RL_SFX_MENU_MOVE, 160);
    }
    int d = input_menu_right() ? 1 : input_menu_left() ? -1 : 0;
    if(d && sel_row < SEL_LAST) {
        audio_sfx(RL_SFX_MENU_MOVE, 160);
        int item = (sel_page - PG_TRUCK) * 2 + sel_row;
        if(item == 0) {
            game.truck_model = (game.truck_model + TRUCK_MODELS + d) % TRUCK_MODELS;
            trucks[0].model = game.truck_model;
            trucks[0].livery = game.livery;
            truck_place(&trucks[0], trucks[0].pos, 0);
        }
        else if(item == 1) {
            game.livery = (game.livery + 4 + d) % 4;
            trucks[0].livery = game.livery;
        }
        else if(item == 2) game.companions = (game.companions + 4 + d) % 4;
        else {
            game.start_tod = (game.start_tod + TOD_COUNT + d) % TOD_COUNT;
            tod_set_hour(tod_hours[game.start_tod]);
        }
    }
    if(pad.pressed & BTN_B) {
        select_back();
        return;
    }
    /* A on the truck page goes on to the drive page; Start drives (or
       joins) from either. */
    if(!game.online && sel_page == PG_TRUCK && (pad.pressed & BTN_A) && !(pad.pressed & BTN_START)) {
        audio_sfx(RL_SFX_MENU_SELECT, 200);
        sel_page = PG_DRIVE;
        sel_row = 0;
    }
    else if((pad.pressed & BTN_START) || ((pad.pressed & BTN_A) && sel_row == SEL_LAST)) {
        audio_sfx(RL_SFX_MENU_SELECT, 220);
        if(game.online) online_join();
        else {
            game_start_drive();
            return;
        }
    }
    select_scene(dt);
}

/* Two rows with arrows and a button under them, in a panel. */
static void select_rows(float x0, float x1, float y0, float y1, const char *const rows[3][2]) {
    float lx = x0 + 18, vx = x1 - 133, step = (y1 - y0 - 40) * 0.5f;
    ui_panel(x0, y0, x1, y1);
    for(int r = 0; r < 3; r++) {
        float y = y0 + 20 + r * step;
        bool on = r == sel_row;
        if(r < 2) {
            ui_row(x0 + 4, y - 15, x1 - 4, y + 15, on);
            uint32_t c = ui_col(on ? UI_VALUE_SEL : UI_VALUE);
            hud_text_c(lx, y, 0.66f, ALIGN_LEFT, ui_col(on ? UI_ITEM_SEL : UI_LABEL), rows[r][0]);
            hud_text_c(vx, y, 0.64f, ALIGN_CENTER, c, rows[r][1]);
            if(on) ui_arrows(vx - 107, vx + 111, y, 0.72f, c);
        }
        else ui_button((x0 + x1) * 0.5f, y, game.online ? 240 : 200, rows[r][1], on);
    }
}

/* An area's card, its top at y0: its map, what it is and its ways up. */
void menu_area_card(int area, float y0) {
    const RlLevel *a = rl_levels[area];
    ui_panel(70, y0, 570, y0 + 232);
    r_hdr(hud_map_hdr(area));
    r_quad2d(86, y0 + 16, 286, y0 + 216, 0, 0, 1, 1, 0xffffffff, 12.0f);
    const float tx = 304;
    hud_text_c(tx, y0 + 30, 0.8f, ALIGN_LEFT, ui_col(UI_TITLE), a->name);
    hud_text_c(tx, y0 + 56, 0.42f, ALIGN_LEFT, ui_col(UI_LABEL), a->blurb);
    char buf[40];
    snprintf(buf, sizeof(buf), "SUMMIT %d FT", (int)((float)a->base_ft + a->summit_y * M_TO_FT));
    hud_text_c(tx, y0 + 86, 0.5f, ALIGN_LEFT, ui_col(UI_TEXT), buf);
    hud_text_c(tx, y0 + 116, 0.46f, ALIGN_LEFT, ui_col(UI_ACCENT), "WAYS UP");
    /* Easiest first. */
    static const char *const grade[3] = {"EASY", "HARDER", "HARDEST"};
    static const int role[3] = {UI_GOOD, UI_ACCENT, UI_WARN};
    float y = y0 + 140;
    for(int d = 1; d <= 3; d++)
        for(int r = 1; r <= 3; r++) {
            if(a->routes[r].difficulty != d) continue;
            hud_text_c(tx, y, 0.52f, ALIGN_LEFT, ui_col(UI_TEXT), a->routes[r].name);
            hud_text_c(554, y, 0.46f, ALIGN_RIGHT, ui_col(role[d - 1]), grade[d - 1]);
            y += 24;
        }
}

/* The area page: the picked area's card over one row to change it and the
   button. */
static void area_draw(void) {
    const RlLevel *a = rl_levels[area_pick];
    ui_heading(320, 40, "CHOOSE YOUR AREA");
    menu_area_card(area_pick, 74);
    ui_panel(140, 330, 500, 432);
    bool on = sel_row == 0;
    uint32_t c = ui_col(on ? UI_VALUE_SEL : UI_VALUE);
    ui_row(144, 341, 496, 371, on);
    hud_text_c(158, 356, 0.66f, ALIGN_LEFT, ui_col(on ? UI_ITEM_SEL : UI_LABEL), "AREA");
    hud_text_c(354, 356, 0.6f, ALIGN_CENTER, c, a->name);
    if(on) ui_arrows(354 - 122, 354 + 126, 356, 0.72f, c);
    ui_button(320, 402, 200, "NEXT", !on);
    ui_hint("LEFT/RIGHT: CHANGE   A: NEXT   B: BACK");
}

void menu_select_draw(void) {
    static const char *const company[4] = {"SOLO", "1 CPU TRUCK", "2 CPU TRUCKS", "3 CPU TRUCKS"};
    if(sel_page == PG_AREA && !game.online) {
        area_draw();
        return;
    }
    if(sel_page == PG_TRUCK) {
        char join[24];
        snprintf(join, sizeof(join), "JOIN LOBBY %d", online_lobby_pick() + 1);
        const char *const rows[3][2] = {
            {"TRUCK", menu_truck_name(game.truck_model)},
            {"PAINT", livery_name(game.truck_model, game.livery)},
            {"", game.online ? join : "NEXT"},
        };
        select_rows(140, 500, 318, 432, rows);
        ui_heading(320, 40, "CHOOSE YOUR TRUCK");
        if(game.online && online_joining()) ui_hint("JOINING...   B: CANCEL");
        else if(game.online) ui_hint("LEFT/RIGHT: CHANGE   A: JOIN   B: BACK");
        else ui_hint("LEFT/RIGHT: CHANGE   A: NEXT   B: BACK");
        return;
    }
    const char *const rows[3][2] = {
        {"COMPANY", company[game.companions]},
        {"START AT", tod_names[game.start_tod]},
        {"", "DRIVE!"},
    };
    select_rows(40, 430, 312, 432, rows);
    /* This truck's best times to the summit, by the way up. */
    ui_panel(446, 312, 610, 432);
    hud_text_c(528, 332, 0.5f, ALIGN_CENTER, ui_col(UI_LABEL), "BEST TIMES");
    for(int w = 0; w < WAY_COUNT; w++) {
        float y = 356 + w * 22;
        char t[16] = "--:--";
        uint32_t b = records_best(game.truck_model, w);
        if(b) records_format(b, t, sizeof(t));
        hud_text_c(458, y, 0.5f, ALIGN_LEFT, ui_col(UI_LABEL), records_way_short(w));
        hud_text_c(598, y, 0.5f, ALIGN_RIGHT, ui_col(b ? UI_VALUE : UI_LABEL), t);
    }
    ui_heading(320, 40, "PLAN YOUR DRIVE");
    ui_hint("LEFT/RIGHT: CHANGE   A: DRIVE   B: BACK");
}

/* ---- Pause ---- */

enum { P_CARRY_ON, P_RECOVER, P_VIEW, P_FPS, P_HUD, P_QUIT, P_COUNT };

/* The viewpoint item is there once one has been found; left and right step
   through the ones found. */
static bool pause_item_on(int i) {
    return i != P_VIEW || game.views_found;
}

static void view_step(int d) {
    for(int k = 1; k <= LVL_VIEWS; k++) {
        int v = (game.view_sel + d * k + LVL_VIEWS * k) % LVL_VIEWS;
        if(game.views_found & (1u << v)) {
            game.view_sel = v;
            return;
        }
    }
}

void menu_pause_update(float dt) {
    if(game.photo) {
        /* Photo mode: the stick orbits, B, Y or Start goes back. */
        if(game.cam_scripted) return;
        game.photo_t += dt;
        game.photo_time_t += dt;
        if(pad.pressed & (BTN_B | BTN_Y | BTN_START)) {
            game.photo = false;
            audio_sfx(RL_SFX_MENU_BACK, 160);
            return;
        }
        /* The D-pad moves the clock: a tap a quarter hour, held it runs. */
        static float held;
        int dir = (pad.held & BTN_RIGHT) ? 1 : (pad.held & BTN_LEFT) ? -1 : 0;
        float step = 0;
        if(pad.pressed & (BTN_LEFT | BTN_RIGHT)) step = 0.25f;
        held = dir ? held + dt : 0;
        if(held > 0.4f) step = 2.0f * dt;
        if(dir && step > 0 && !game.online) {   /* online the lobby keeps the clock */
            tod_set_hour(tod.hour + dir * step);
            game.photo_time_t = 0;
        }
        camera_photo(&trucks[0], dt);
        return;
    }
    if(input_menu_up())
        do game.pause_sel = (game.pause_sel + P_COUNT - 1) % P_COUNT;
        while(!pause_item_on(game.pause_sel));
    if(input_menu_down())
        do game.pause_sel = (game.pause_sel + 1) % P_COUNT;
        while(!pause_item_on(game.pause_sel));
    if(game.pause_sel == P_VIEW && (input_menu_left() || input_menu_right())) {
        view_step(input_menu_right() ? 1 : -1);
        audio_sfx(RL_SFX_MENU_SELECT, 120);
    }
    if(pad.pressed & (BTN_START | BTN_B)) {
        game.paused = false;
        return;
    }
    if(pad.pressed & BTN_Y) {
        game.photo = true;
        game.photo_t = 0;
        game.photo_time_t = 10.0f;
        camera_photo_begin(&trucks[0]);
        audio_sfx(RL_SFX_MENU_SELECT, 160);
        return;
    }
    if(pad.pressed & BTN_A) {
        audio_sfx(RL_SFX_MENU_SELECT, 200);
        switch(game.pause_sel) {
            case P_CARRY_ON: game.paused = false; break;
            case P_RECOVER: truck_recover(&trucks[0]); camera_reset(); game.paused = false; break;
            case P_VIEW: {
                const lvl_view_t *v = &lvl_views[game.view_sel];
                char buf[48];
                truck_recover_at(&trucks[0], v3(v->x, v->y, v->z));
                records_jumped();
                camera_reset();
                fx_clear();
                game.paused = false;
                snprintf(buf, sizeof(buf), "BACK AT %s", v->name);
                hud_message(buf, 0xff90f0b0, 2.0f);
                break;
            }
            case P_FPS: game.show_fps = !game.show_fps; break;
            case P_HUD: game.hud_hidden = !game.hud_hidden; break;
            case P_QUIT:
                if(game.online) online_leave_lobby();
                else {
                    game.paused = false;
                    audio_drive_stop();
                    game_set_state(ST_TITLE);
                }
                break;
        }
    }
}

void menu_pause_draw(void) {
    if(game.photo) {
        /* Nothing on screen but a hint that fades after a moment. */
        float a = clampf(2.5f - game.photo_t, 0, 1);
        if(a > 0 && !game.cam_scripted)
            hud_text_c(320, 452, 0.55f, ALIGN_CENTER, color_alpha(0xfff8f0dc, a),
                       game.online ? "PHOTO MODE   STICK: TURN   R/L: ZOOM   B: BACK"
                                   : "PHOTO MODE   STICK: TURN   R/L: ZOOM   D-PAD: TIME   B: BACK");
        /* The clock while it is being moved. */
        float ta = clampf(2.0f - game.photo_time_t, 0, 1);
        if(ta > 0 && !game.cam_scripted) {
            char t[8];
            tod_format(tod.hour, t, sizeof(t));
            hud_text_c(320, 30, 0.9f, ALIGN_CENTER, color_alpha(0xfff8f0dc, ta), t);
        }
        return;
    }
    char view[32] = "";
    if(game.views_found) snprintf(view, sizeof(view), "TO %s", lvl_views[game.view_sel].name);
    const char *items[P_COUNT] = {"CARRY ON", "RECOVER TRUCK", view, game.show_fps ? "FPS COUNTER: ON" : "FPS COUNTER: OFF",
                                  game.hud_hidden ? "HUD: OFF" : "HUD: ON", game.online ? "LEAVE LOBBY" : "QUIT TO TITLE"};
    ui_dim();
    int n = 0;
    for(int i = 0; i < P_COUNT; i++) n += pause_item_on(i);
    ui_panel(190, 150, 450, 204 + n * 32);
    char title[24], t[8];
    tod_format(tod.hour, t, sizeof(t));
    /* Online nothing stops: it is a menu over the mountain. */
    snprintf(title, sizeof(title), "%s   %s", game.online ? "MENU" : "PAUSED", t);
    ui_title(190, 150, 450, title);
    float y = 204;
    for(int i = 0; i < P_COUNT; i++) {
        if(!pause_item_on(i)) continue;
        bool on = i == game.pause_sel;
        uint32_t c = ui_col(on ? UI_ITEM_SEL : UI_ITEM);
        ui_row(202, y - 14, 438, y + 14, on);
        hud_text_c(320, y, 0.7f, ALIGN_CENTER, c, items[i]);
        if(on && i == P_VIEW && __builtin_popcount(game.views_found) > 1) ui_arrows(214, 426, y, 0.72f, c);
        y += 32;
    }
    ui_hint("A: OK   Y: PHOTO MODE   B: BACK");
}
