/* Title screen: the track in the background with the grid lined up, a
   rotating Sega-style sunburst, a bouncing per-letter logo, sparkles and a
   pulsing PRESS START. Then a slide-in menu for mode and engine class, and
   a track select that flies over each course as it is picked. Two players
   pick their karts side by side on a split-screen kart select, each with
   their own pad. The main menu's Settings page holds the sound test, the
   online name, the music switch and the Customize screen, where the player
   paints a kart of their own and watches it change. */
#include "title.h"
#include "audio.h"
#include "hud.h"
#include "kart.h"
#include "models.h"
#include "render.h"
#include "save.h"
#include "track.h"
#include "online.h"
#include "gp.h"
#include "texture_assets.h"
#include "font_metrics.h"
#include "version.h"
#include "sound_assets.h"
#include <ctype.h>

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
static void karts_enter(void);
static void karts_update(float dt);
static void tracks_enter(void);

/* Main menu: single player, two-player split screen, online or the
   settings. */
enum { MAIN_ITEMS = 4, MAIN_ONLINE = 2, MAIN_SETTINGS = 3 };
static int main_row;
static const char *const main_items[MAIN_ITEMS] = {"SINGLE PLAYER", "TWO PLAYER", "ONLINE MULTIPLAYER",
                                                   "SETTINGS"};

/* Settings: a page of the main menu (the backdrop stays put) opening the
   sound test, the online name entry, the music switch and the Customize
   screen. */
enum { SET_SOUND, SET_NAME, SET_MUSIC, SET_CUSTOM, SET_ROWS };
static bool settings_open;
static float settings_slide;
static int settings_row;

/* Sound test: pick and play any song or effect, and hear the final-lap
   speed-up. */
enum { SND_MUSIC, SND_SFX, SND_SPEED, SND_ROWS };
static bool sound_open;
static float sound_slide;
static int sound_row, sound_song, sound_sfx;
static float sfx_flash;       /* highlight pulse when an effect is played */

/* Customize: the player's own kart, three palette picks (body paint, trim,
   Chao) painted onto the kart in the background as they change. Saved to
   the VMU, it joins the colour choices as CUSTOM. */
enum { CUS_BODY, CUS_TRIM, CUS_CHAO, CUS_SAVE, CUS_ROWS };
static bool custom_open;
static float custom_slide;
static int custom_row;
static KartCustom edit;
static int custom_prev;       /* player 1's colour before editing, for a cancel */

/* Colour choices: the presets, and the custom kart once one is saved. */
static int colour_options(void) {
    return COLOUR_COUNT + (save.custom_on ? 1 : 0);
}

static void sound_enter(void) {
    sound_open = true;
    sound_slide = 0;
    sound_row = SND_MUSIC;
    sound_song = audio_music_now(NULL, NULL);
    sfx_flash = 0;
}

static void sound_update(void) {
    sound_slide = fminf(1.0f, sound_slide + DK_DT * 4.0f);
    if(sfx_flash > 0) sfx_flash -= DK_DT;
    if(input_menu_up()) {
        sound_row = (sound_row + SND_ROWS - 1) % SND_ROWS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_menu_down()) {
        sound_row = (sound_row + 1) % SND_ROWS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    int dir = input_menu_left() ? -1 : input_menu_right() ? 1 : 0;
    if(dir) {
        if(sound_row == SND_MUSIC) {
            sound_song = (sound_song + dir + DK_MUSIC_SONG_COUNT) % DK_MUSIC_SONG_COUNT;
            audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
        }
        else if(sound_row == SND_SFX) {
            sound_sfx = (sound_sfx + dir + DK_SFX_COUNT) % DK_SFX_COUNT;
            audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
        }
        else {
            audio_music_rate(audio_music_rate_now() > 1.0f ? 1.0f : 1.06f);
            audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
        }
    }
    if(pad.pressed & BTN_B) {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        audio_music_rate(1.0f);
        audio_music_enable(save.music);   /* a song played here with the music off stops */
        sound_open = false;
        settings_slide = 0;
        return;
    }
    if(pad.pressed & (BTN_A | BTN_START)) {
        if(sound_row == SND_MUSIC) {
            audio_music_enable(true);
            audio_music_play(sound_song);
        }
        else if(sound_row == SND_SFX) {
            audio_sfx(sound_sfx, 255, 128);
            sfx_flash = 0.35f;
        }
        else {
            audio_music_rate(audio_music_rate_now() > 1.0f ? 1.0f : 1.06f);
            audio_sfx(DK_SFX_MENU_SELECT, 200, 128);
        }
    }
}

static void music_toggle(void) {
    save.music = !save.music;
    audio_music_enable(save.music);
    save_store();
    audio_sfx(DK_SFX_MENU_MOVE, 200, 128);
}

static void custom_apply(void) {
    kart_apply_look(&karts[game.player], COLOUR_CUSTOM, &edit);
    models_paint_kart(game.player);
}

static void custom_enter(void) {
    custom_open = true;
    custom_slide = 0;
    custom_row = CUS_BODY;
    edit = save.custom_on ? save.custom : kart_custom_default;
    custom_prev = game.pl[0].colour;
    custom_apply();
}

static uint8_t *custom_pick(int row) {
    return row == CUS_BODY ? &edit.body : row == CUS_TRIM ? &edit.trim : &edit.chao;
}

static void custom_close(void) {
    custom_open = false;
    settings_slide = 0;
}

static void custom_update(void) {
    custom_slide = fminf(1.0f, custom_slide + DK_DT * 4.0f);
    if(input_menu_up()) {
        custom_row = (custom_row + CUS_ROWS - 1) % CUS_ROWS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_menu_down()) {
        custom_row = (custom_row + 1) % CUS_ROWS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    int dir = input_menu_left() ? -1 : input_menu_right() ? 1 : 0;
    if(dir && custom_row != CUS_SAVE) {
        uint8_t *pick = custom_pick(custom_row);
        *pick = (uint8_t)((*pick + dir + PALETTE_COUNT) % PALETTE_COUNT);
        custom_apply();
        audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
        if(custom_row == CUS_CHAO) audio_chao(CHAO_HI, kart_voice(COLOUR_CUSTOM, &edit), 150, 128);
    }
    if(pad.pressed & BTN_B) {
        /* Cancel: the kart goes back to the colour it had. */
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        kart_apply_colour(&karts[game.player], custom_prev);
        models_paint_kart(game.player);
        custom_close();
        return;
    }
    if(!(pad.pressed & (BTN_A | BTN_START))) return;
    if(custom_row != CUS_SAVE && !(pad.pressed & BTN_START)) {
        custom_row++;
        audio_sfx(DK_SFX_MENU_MOVE, 200, 128);
        return;
    }
    /* Save: the custom kart is player 1's choice from here on. */
    save.custom = edit;
    save.custom_on = true;
    save_store();
    game.pl[0].colour = COLOUR_CUSTOM;
    if(game.pl[1].colour == COLOUR_CUSTOM) game.pl[1].colour = 0;
    race_setup();
    audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
    audio_chao(CHAO_HI, kart_voice(COLOUR_CUSTOM, &edit), 200, 128);
    custom_close();
}

static void settings_enter(void) {
    settings_open = true;
    settings_slide = 0;
    settings_row = SET_SOUND;
}

static void settings_update(void) {
    settings_slide = fminf(1.0f, settings_slide + DK_DT * 4.0f);
    if(input_menu_up()) {
        settings_row = (settings_row + SET_ROWS - 1) % SET_ROWS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_menu_down()) {
        settings_row = (settings_row + 1) % SET_ROWS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(settings_row == SET_MUSIC && (input_menu_left() || input_menu_right())) {
        music_toggle();
        return;
    }
    if(pad.pressed & BTN_B) {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        settings_open = false;
        menu_slide = 0;
        return;
    }
    if(!(pad.pressed & (BTN_A | BTN_START))) return;
    if(settings_row == SET_MUSIC) {
        music_toggle();
        return;
    }
    audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
    if(settings_row == SET_SOUND) sound_enter();
    else if(settings_row == SET_NAME) online_name_enter();
    else custom_enter();
}

void title_settings_enter(void) {
    game_set_state(ST_MAIN);
    main_row = MAIN_SETTINGS;
    menu_slide = 1;
    sound_open = custom_open = false;
    settings_enter();
    settings_row = SET_NAME;
}

static void main_update(void) {
    if(custom_open) {
        custom_update();
        return;
    }
    if(sound_open) {
        sound_update();
        return;
    }
    if(settings_open) {
        settings_update();
        return;
    }
    menu_slide = fminf(1.0f, menu_slide + DK_DT * 4.0f);
    if(input_menu_up()) {
        main_row = (main_row + MAIN_ITEMS - 1) % MAIN_ITEMS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_menu_down()) {
        main_row = (main_row + 1) % MAIN_ITEMS;
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
    if(main_row == MAIN_ONLINE) {
        online_enter();
        return;
    }
    if(main_row == MAIN_SETTINGS) {
        settings_enter();
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

static const int laps_options[3] = {3, 5, 10};

enum { ROW_MODE, ROW_CC, ROW_LAPS, ROW_ITEMS, ROW_COLOUR, ROW_START };

/* A solo time trial never has power-ups: lap times stay comparable. */
static bool items_locked(void) {
    return game.mode == MODE_TT && game.num_players == 1;
}

/* Menu rows: two players pick their colours on the kart select instead. */
static int menu_rows(int *rows) {
    int n = 0;
    rows[n++] = ROW_MODE;
    rows[n++] = ROW_CC;
    rows[n++] = ROW_LAPS;
    rows[n++] = ROW_ITEMS;
    if(game.num_players == 1) rows[n++] = ROW_COLOUR;
    rows[n++] = ROW_START;
    return n;
}

static const char *mode_name(void) {
    if(game.mode == MODE_GP) return "GRAND PRIX";
    if(game.mode == MODE_RACE) return "SINGLE RACE";
    return game.num_players > 1 ? "VERSUS" : "TIME TRIAL";
}

static const char *mode_blurb(void) {
    if(game.mode == MODE_GP) return "ALL SEVEN COURSES, POINTS EVERY RACE";
    if(game.mode == MODE_RACE) return "ONE RACE ON A COURSE YOU PICK";
    return game.num_players > 1 ? "JUST THE TWO OF YOU" : "ALONE AGAINST THE CLOCK";
}

/* After the setup (and the kart select with two players): a Grand Prix
   starts on its first course, anything else picks a course. */
static void setup_done(void) {
    if(game.mode == MODE_GP) gp_start();
    else tracks_enter();
}

/* Step a player's colour, skipping the other player's choice. The grid is
   set up again (rivals take the colours left over), so a wave the other
   player's kart is in the middle of carries over. */
static void step_colour(int player, int dir) {
    int c = game.pl[player].colour, opts = colour_options();
    do {
        c = (c + dir + opts) % opts;
    } while(game.num_players > 1 && c == game.pl[player ^ 1].colour);
    game.pl[player].colour = c;
    float wave[MAX_PLAYERS];
    for(int p = 0; p < game.num_players; p++) wave[p] = karts[game.pl[p].kart].wave_t;
    race_setup();
    for(int p = 0; p < game.num_players; p++)
        if(p != player) karts[game.pl[p].kart].wave_t = wave[p];
    audio_chao(CHAO_HI, kart_voice(c, NULL), 150, game.num_players == 1 ? 128 : player ? 190 : 66);
}

#ifdef DK_AT_SETTINGS
/* Settings autotest (capture build, on an isolated VMU): walks the Settings
   page as a player would, a press every 0.4 s: music off, a custom kart
   painted LIME / RED / BLACK and saved, the race setup's COLOUR row on
   CUSTOM and stepping either side of it, then the online name saved from
   its row. A press is repeated until its target shows (a screenshot stalls
   Flycast, and a tap held over a multi-step frame fires once per step),
   checks run along the way, "DK SHOT" lines are for capture.py, and
   "DK AUTOTEST: PASS" comes once the card has had time to take the write.
   With DK_AT_SETTINGS_WANT (a second boot on the same card) it instead
   checks the saved settings came back. */
typedef struct {
    uint32_t btn;
    const char *shot;
    int until;   /* press until this condition holds (0: press once) */
    int check;   /* this condition must hold now */
} AtStep;

enum {
    C_MAIN = 1, C_ROW_SETTINGS, C_SETTINGS, C_ROW_MUSIC, C_MUSIC_OFF, C_ROW_CUSTOM, C_CUSTOM, C_BODY, C_ROW_TRIM,
    C_TRIM, C_ROW_CHAO, C_CHAO, C_ROW_SAVE, C_SAVED, C_ROW_SINGLE, C_MENU, C_ROW_COLOUR, C_LIME, C_IS_CUSTOM,
    C_ROW_NAME, C_NAME_SCREEN, C_BACK_ON_NAME, C_SAVED_LOOK
};

static bool at_cond(int c) {
    const Kart *k = &karts[game.player];
    switch(c) {
        case C_MAIN: return game.state == ST_MAIN && !settings_open && !custom_open && !sound_open;
        case C_ROW_SETTINGS: return main_row == MAIN_SETTINGS;
        case C_SETTINGS: return settings_open;
        case C_ROW_MUSIC: return settings_row == SET_MUSIC;
        case C_MUSIC_OFF: return !save.music && !audio_music_enabled();
        case C_ROW_CUSTOM: return settings_row == SET_CUSTOM;
        case C_CUSTOM: return custom_open;
        case C_BODY: return edit.body == 3;
        case C_ROW_TRIM: return custom_row == CUS_TRIM;
        case C_TRIM: return edit.trim == 0;
        case C_ROW_CHAO: return custom_row == CUS_CHAO;
        case C_CHAO: return edit.chao == 15;
        case C_ROW_SAVE: return custom_row == CUS_SAVE;
        case C_SAVED: return !custom_open && save.custom_on;
        case C_ROW_SINGLE: return main_row == 0;
        case C_MENU: return game.state == ST_MENU;
        case C_ROW_COLOUR: return menu_row == 4;
        case C_LIME: return game.pl[0].colour == COLOUR_COUNT - 1;
        case C_IS_CUSTOM: return game.pl[0].colour == COLOUR_CUSTOM;
        case C_ROW_NAME: return settings_row == SET_NAME;
        case C_NAME_SCREEN: return game.state == ST_ONLINE && !game.online;
        case C_BACK_ON_NAME: return game.state == ST_MAIN && settings_open && settings_row == SET_NAME;
        case C_SAVED_LOOK:
            return save.custom.body == 3 && save.custom.trim == 0 && save.custom.chao == 15 &&
                   game.pl[0].colour == COLOUR_CUSTOM && k->paint1 == kart_palette[3].rgb &&
                   k->paint2 == kart_palette[0].rgb && k->chao == kart_palette[15].rgb;
        default: return true;
    }
}

#define PAUSE {0, NULL, 0, 0}, {0, NULL, 0, 0}
static const AtStep at_steps[] = {
    {BTN_START, NULL, C_MAIN, 0}, {BTN_DOWN, NULL, C_ROW_SETTINGS, 0}, {0, "settings-main", 0, 0}, PAUSE,
    {BTN_A, NULL, C_SETTINGS, 0}, {0, "settings", 0, 0}, PAUSE,
    {BTN_DOWN, NULL, C_ROW_MUSIC, 0}, {BTN_RIGHT, NULL, C_MUSIC_OFF, 0}, {0, "settings-music-off", 0, C_MUSIC_OFF}, PAUSE,
    {BTN_DOWN, NULL, C_ROW_CUSTOM, 0}, {BTN_A, NULL, C_CUSTOM, 0}, {0, "customize", 0, 0}, PAUSE,
    {BTN_RIGHT, NULL, C_BODY, 0}, {BTN_DOWN, NULL, C_ROW_TRIM, 0}, {BTN_RIGHT, NULL, C_TRIM, 0},
    {BTN_DOWN, NULL, C_ROW_CHAO, 0}, {BTN_RIGHT, NULL, C_CHAO, 0}, {0, "customize-picked", 0, 0}, PAUSE,
    {BTN_DOWN, NULL, C_ROW_SAVE, 0}, {BTN_A, NULL, C_SAVED, 0}, {0, "settings-saved", 0, C_SAVED_LOOK}, PAUSE,
    {BTN_B, NULL, C_MAIN, 0}, {BTN_UP, NULL, C_ROW_SINGLE, 0}, {BTN_A, NULL, C_MENU, 0},
    {0, "menu-custom", 0, C_IS_CUSTOM}, PAUSE,
    {BTN_UP, NULL, C_ROW_COLOUR, 0}, {BTN_LEFT, NULL, C_LIME, 0}, {BTN_RIGHT, NULL, C_IS_CUSTOM, 0},
    {BTN_B, NULL, C_MAIN, 0}, {BTN_DOWN, NULL, C_ROW_SETTINGS, 0}, {BTN_A, NULL, C_SETTINGS, 0},
    {BTN_DOWN, NULL, C_ROW_NAME, 0}, PAUSE, {BTN_A, NULL, C_NAME_SCREEN, 0}, {0, "settings-name", 0, 0}, PAUSE,
    {BTN_START, NULL, C_BACK_ON_NAME, 0}, {0, "settings-after-name", 0, C_BACK_ON_NAME}, PAUSE,
};

static void settings_autotest(float dt) {
    static float t;
    static int step, tries;
    static bool ok = true, done;
    if(done) return;
    t += dt;
#ifdef DK_AT_SETTINGS_WANT
    if(t > 3.0f) {
        ok = !save.music && !audio_music_enabled() && save.custom_on && save.custom.body == 3 && save.custom.trim == 0 &&
             save.custom.chao == 15 && strcmp(save.name, "CHAO") == 0;
        printf("DK SETTINGS: loaded music %d custom %d (%d %d %d) name %s\nDK AUTOTEST: %s\n", save.music,
               save.custom_on, save.custom.body, save.custom.trim, save.custom.chao, save.name, ok ? "PASS" : "FAIL");
        done = true;
    }
    return;
#endif
    if(step >= (int)(sizeof(at_steps) / sizeof(at_steps[0]))) {
        if(t > 3.0f) {   /* the card's write */
            printf("DK SETTINGS: saved music %d custom %d (%d %d %d) name %s\nDK AUTOTEST: %s\n", save.music,
                   save.custom_on, save.custom.body, save.custom.trim, save.custom.chao, save.name, ok ? "PASS" : "FAIL");
            done = true;
        }
        return;
    }
    if(t < 0.4f) return;
    t = 0;
    const AtStep *s = &at_steps[step];
    if(s->until && (at_cond(s->until) || tries >= 40)) {
        if(tries >= 40) {
            printf("DK SETTINGS: step %d never reached condition %d (state %d)\n", step, s->until, game.state);
            ok = false;
        }
        step++;
        tries = 0;
        return;
    }
    if(s->btn & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)) input_film_tap(0, s->btn);
    else if(s->btn) pad.pressed |= s->btn;
    if(s->shot) printf("DK SHOT %s\n", s->shot);
    if(s->check && !at_cond(s->check)) {
        printf("DK SETTINGS: check %d failed at step %d (state %d colour %d music %d custom %d %d %d %d)\n", s->check,
               step, game.state, game.pl[0].colour, save.music, save.custom_on, save.custom.body, save.custom.trim,
               save.custom.chao);
        ok = false;
    }
    if(s->until) tries++;
    else step++;
}

/* Ticked by the capture script every frame, whatever the state (the name
   screen is not a title state). */
void title_settings_autotest(float dt) {
    settings_autotest(dt);
}
#endif

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
        if(chao_call_t <= 0) audio_chao(CHAO_HI, kart_voice(game.pl[0].colour, NULL), 200, 128);
    }
    if(game.state == ST_TITLE) {
        menu_slide = fmaxf(0.0f, menu_slide - dt * 4.0f);
        /* Hidden: the triggers cycle the background track. */
        uint32_t trig = (pad.pressed | pad2.pressed) & (BTN_L | BTN_R);
        if(trig && trig != (BTN_L | BTN_R)) {
            int dir = trig == BTN_L ? -1 : 1;
            game_load_track(game.track + dir);
            audio_sfx(DK_SFX_WHOOSH, 140, dir < 0 ? 90 : 166);
        }
        if(game.time > 0.6f &&((pad.pressed | pad2.pressed) & (BTN_START | BTN_A))) {
            audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
            audio_sfx(DK_SFX_WHOOSH, 160, 128);
            game_set_state(ST_MAIN);
            main_row = 0;
            sound_open = settings_open = custom_open = false;
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
    if(game.state == ST_KARTS) {
        karts_update(dt);
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
            game.mode = (game.mode + dir + MODE_COUNT) % MODE_COUNT;
            race_setup();
        }
        else if(row == ROW_CC) {
            game.cc = (game.cc + dir + CC_COUNT) % CC_COUNT;
        }
        else if(row == ROW_LAPS) {
            int i = game.setup_laps == 10 ? 2 : game.setup_laps == 5 ? 1 : 0;
            game.laps = game.setup_laps = laps_options[(i + dir + 3) % 3];
        }
        else if(row == ROW_ITEMS) {
            if(!items_locked()) {
                game.setup_items = !game.setup_items;
                race_setup();
            }
        }
        else if(row == ROW_COLOUR) {
            step_colour(0, dir);
        }
        if(row != ROW_START) audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
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
#if !defined(DK_AUTOTEST) && !defined(DK_FILM)
        else if(game.num_players > 1 && !pad2.connected) {
            warn_t = 2.0f;
            audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        }
#endif
        else {
            audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
            audio_sfx(DK_SFX_WHOOSH, 160, 128);
            race_setup();
            if(game.num_players > 1) karts_enter();
            else setup_done();
        }
    }
}

static void tracks_enter(void) {
    game_set_state(ST_TRACKS);
    tracks_slide = 0;
    track_swap = 0;
    fly_t = 0;
}

void title_tracks_enter(void) {
    tracks_enter();
}

/* Two-player kart select: each half of the split screen shows that
   player's kart, and each pad steps through the colours and readies up on
   its own. The track select follows once both are ready. */
static bool kart_ready[MAX_PLAYERS];
static float karts_slide, karts_go_t, ready_pop[MAX_PLAYERS];

static void karts_enter(void) {
    game_set_state(ST_KARTS);
    karts_slide = 0;
    karts_go_t = 0;
    for(int p = 0; p < MAX_PLAYERS; p++) kart_ready[p] = false, ready_pop[p] = 0;
}

static void karts_update(float dt) {
    karts_slide = fminf(1.0f, karts_slide + dt * 4.0f);
    for(int p = 0; p < 2; p++) {
        const Pad *pd = input_pad(p);
        if(ready_pop[p] > 0) ready_pop[p] = fmaxf(0.0f, ready_pop[p] - dt * 3.0f);
        int dir = input_left(p) ? -1 : input_right(p) ? 1 : 0;
        if(dir && !kart_ready[p]) {
            step_colour(p, dir);
            audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
        }
        if((pd->pressed & (BTN_A | BTN_START)) && !kart_ready[p]) {
            kart_ready[p] = true;
            ready_pop[p] = 1.0f;
            kart_wave(&karts[game.pl[p].kart]);
            audio_sfx(DK_SFX_MENU_SELECT, 230, p ? 190 : 66);
        }
        else if(pd->pressed & BTN_B) {
            if(kart_ready[p]) {
                kart_ready[p] = false;
                audio_sfx(DK_SFX_MENU_BACK, 200, p ? 190 : 66);
            }
            else if(p == 0) {
                audio_sfx(DK_SFX_MENU_BACK, 200, 128);
                game_set_state(ST_MENU);
                menu_slide = 0;
                return;
            }
        }
    }
    /* A short beat with both ready, so a late B can still back out. */
    karts_go_t = kart_ready[0] && kart_ready[1] ? karts_go_t + dt : 0;
    if(karts_go_t > 0.7f) {
        audio_sfx(DK_SFX_WHOOSH, 160, 128);
        setup_done();
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
        if(game.num_players > 1) karts_enter();
        else game_set_state(ST_MENU);
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
    bool closeup = game.state == ST_MENU || (game.state == ST_MAIN && custom_open);
    if(closeup) {
        /* Close-up showing off the driver, framed to the left of the menu. */
        a = game.title_t * 0.35f;
        r = 5.0f;
    }
    cam->pos = vadd(target, v3(sinf(a) * r, 2.4f + 0.8f * sinf(game.title_t * 0.17f), cosf(a) * r));
    cam->fwd = vnorm(vsub(target, cam->pos));
    if(closeup) {
        /* Shift the view so the kart sits left of centre. */
        V3 right = vnorm(vcross(v3(0, 1, 0), cam->fwd));
        cam->fwd = vnorm(vmad(cam->fwd, right, 0.28f));
    }
    cam->right = vnorm(vcross(v3(0, 1, 0), cam->fwd));
    cam->up = vcross(cam->fwd, cam->right);
    cam->fov = 1.20f;
}

/* Kart select: each player's view orbits their own kart, starting on
   opposite sides, with the kart left of centre clear of the panel. */
void title_kart_cameras(void) {
    for(int p = 0; p < game.num_players; p++) {
        Camera *cam = &game.pl[p].cam;
        V3 target = vadd(karts[game.pl[p].kart].pos, v3(0, 0.75f, 0));
        float a = game.title_t * 0.35f + (float)p * DK_PI;
        float r = 5.2f;
        cam->pos = vadd(target, v3(sinf(a) * r, 1.6f + 0.4f * sinf(game.title_t * 0.17f + p), cosf(a) * r));
        cam->fwd = vnorm(vsub(target, cam->pos));
        V3 right = vnorm(vcross(v3(0, 1, 0), cam->fwd));
        cam->fwd = vnorm(vmad(cam->fwd, right, 0.30f));
        cam->right = vnorm(vcross(v3(0, 1, 0), cam->fwd));
        cam->up = vcross(cam->fwd, cam->right);
        cam->fov = 1.20f;
        cam->shake = 0;
    }
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
    KartColour c = kart_colour_look(colour, NULL);
    r_hdr(h_flat);
    uint32_t kart = c.paint1 ? c.paint1 : 0xffe02828;
    r_quad2d(x - 1, y - 1, x + 33, y + 17, 0, 0, 1, 1, 0xff0c1430, 16.4f);
    r_quad2d(x, y, x + 16, y + 16, 0, 0, 1, 1, kart | 0xff000000, 16.5f);
    r_quad2d(x + 16, y, x + 32, y + 16, 0, 0, 1, 1, c.ui | 0xff000000, 16.5f);
}

static void draw_main(void) {
    float slide = menu_slide * menu_slide * (3 - 2 * menu_slide);
    float y0 = 196 + (1 - slide) * 300;
    hud_panel(150, y0, 490, y0 + 214, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    for(int i = 0; i < MAIN_ITEMS; i++) {
        bool sel = i == main_row;
        float y = y0 + 18 + i * 46;
        if(sel) {
            r_hdr(h_flat);
            float glow = 0.18f + 0.1f * sinf(game.title_t * 7);
            r_quad2d(166, y - 6, 474, y + 42, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
        }
        float s = sel ? 0.86f + 0.04f * sinf(game.title_t * 8) : 0.8f;
        hud_text(320, y + 2, s, ALIGN_CENTER, sel ? argb(255, 255, 230, 60) : argb(255, 200, 220, 255), main_items[i]);
    }
    static const char *const blurbs[MAIN_ITEMS] = {"GRAND PRIX, SINGLE RACE OR TIME TRIAL", "SPLIT SCREEN WITH A FRIEND",
                                                   "UP TO SIX PLAYERS (BROADBAND ADAPTER OR MODEM)",
                                                   "SOUND TEST, ONLINE NAME, MUSIC AND YOUR OWN KART"};
    hud_text(320, 440, 0.45f, ALIGN_CENTER, argb(220, 230, 240, 255), blurbs[main_row]);
}

static void draw_settings(void) {
    float slide = settings_slide * settings_slide * (3 - 2 * settings_slide);
    float y0 = 196 + (1 - slide) * 300;
    hud_panel(150, y0, 490, y0 + 214, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    hud_text(320, y0 + 4, 0.7f, ALIGN_CENTER, argb(255, 255, 220, 60), "SETTINGS");
    static const char *const labels[SET_ROWS] = {"SOUND TEST", "ONLINE NAME", "MUSIC", "CUSTOMIZE KART"};
    char buf[48];
    for(int i = 0; i < SET_ROWS; i++) {
        bool sel = i == settings_row;
        float y = y0 + 42 + i * 42;
        if(sel) {
            r_hdr(h_flat);
            float glow = 0.18f + 0.1f * sinf(game.title_t * 7);
            r_quad2d(166, y - 5, 474, y + 35, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
        }
        uint32_t col = sel ? argb(255, 255, 230, 60) : argb(255, 200, 220, 255);
        float s = sel ? 0.72f + 0.03f * sinf(game.title_t * 8) : 0.68f;
        if(i == SET_MUSIC) {
            hud_text(184, y + 2, s, ALIGN_LEFT, col, labels[i]);
            snprintf(buf, sizeof(buf), "%s%s%s", sel ? "< " : "", save.music ? "ON" : "OFF", sel ? " >" : "");
            hud_text(456, y + 2, s, ALIGN_RIGHT, col, buf);
        }
        else {
            hud_text(320, y + 2, s, ALIGN_CENTER, col, labels[i]);
        }
    }
    const char *blurb = "LISTEN TO THE MUSIC AND SOUND EFFECTS";
    if(settings_row == SET_NAME) {
        if(save.name[0]) snprintf(buf, sizeof(buf), "THE NAME OTHER PLAYERS SEE ONLINE: %s", save.name);
        else snprintf(buf, sizeof(buf), "THE NAME OTHER PLAYERS SEE ONLINE");
        blurb = buf;
    }
    else if(settings_row == SET_MUSIC) blurb = "MUSIC IN THE MENUS AND RACES, SAVED TO THE VMU";
    else if(settings_row == SET_CUSTOM) blurb = save.custom_on ? "REPAINT YOUR OWN KART" : "PAINT A KART OF YOUR OWN";
    hud_text(320, 440, 0.45f, ALIGN_CENTER, argb(220, 230, 240, 255), blurb);
}

/* The Customize panel sits right of the kart like the race setup, each
   colour row showing the whole palette with the pick framed. */
static void draw_custom(void) {
    float slide = custom_slide * custom_slide * (3 - 2 * custom_slide);
    float x0 = 640 - 300 * slide, x1 = x0 + 280, cx = (x0 + x1) * 0.5f;
    const float top = 84, y_first = top + 54, pitch = 74;
    float y_save = y_first + 3 * pitch + 2;
    hud_panel(x0, top, x1, y_save + 52, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    hud_text(cx, top + 6, 0.9f, ALIGN_CENTER, argb(255, 255, 220, 60), "CUSTOMIZE");
    static const char *const labels[3] = {"KART BODY", "KART TRIM", "CHAO"};
    float glow = 0.18f + 0.1f * sinf(game.title_t * 7);
    float bob = 3.0f * sinf(game.title_t * 9);
    for(int i = 0; i < 3; i++) {
        bool sel = i == custom_row;
        float y = y_first + i * pitch;
        if(sel) {
            r_hdr(h_flat);
            r_quad2d(x0 + 10, y - 6, x1 - 10, y + pitch - 10, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
        }
        uint32_t col = sel ? argb(255, 255, 255, 150) : argb(255, 200, 220, 255);
        int pick = *custom_pick(i);
        hud_text(x0 + 20, y - 6, 0.5f, ALIGN_LEFT, argb(255, 150, 200, 255), labels[i]);
        hud_text(x1 - 20, y - 6, 0.5f, ALIGN_RIGHT, col, kart_palette[pick].name);
        /* The palette strip: 16 cells, the pick framed in white. */
        float sx = x0 + 30, sy = y + 22, cell = 12, gap = 2;
        r_hdr(h_flat);
        r_quad2d(sx - 3, sy - 3, sx + PALETTE_COUNT * (cell + gap) + 1, sy + cell + 3, 0, 0, 1, 1, 0xff0c1430, 16.3f);
        for(int c = 0; c < PALETTE_COUNT; c++) {
            float x = sx + c * (cell + gap);
            if(c == pick) r_quad2d(x - 2, sy - 2, x + cell + 2, sy + cell + 2, 0, 0, 1, 1, 0xfffffff0, 16.4f);
            r_quad2d(x, sy, x + cell, sy + cell, 0, 0, 1, 1, kart_palette[c].rgb, 16.5f);
        }
        if(sel) {
            hud_text(x0 + 16 - bob, sy - 10, 0.6f, ALIGN_CENTER, col, "<");
            hud_text(x1 - 16 + bob, sy - 10, 0.6f, ALIGN_CENTER, col, ">");
        }
    }
    bool sel = custom_row == CUS_SAVE;
    if(sel) {
        r_hdr(h_flat);
        r_quad2d(x0 + 10, y_save - 6, x1 - 10, y_save + 40, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
    }
    float s = sel ? 1.1f + 0.06f * sinf(game.title_t * 8) : 0.95f;
    hud_text(cx, y_save, s, ALIGN_CENTER, sel ? argb(255, 255, 230, 60) : argb(255, 200, 220, 255), "SAVE");
    hud_text(24, 428, 0.5f, ALIGN_LEFT, argb(230, 255, 255, 255),
             sel ? "A: SAVE   B: CANCEL" : "LEFT/RIGHT: COLOUR   A: NEXT   B: CANCEL");
    hud_text(176, 100, 0.4f, ALIGN_CENTER, argb(220, 230, 240, 255), "CUSTOMIZE YOUR LOOK!");
}

static void upper_copy(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for(; src[i] && i + 1 < n; i++) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = 0;
}

static void draw_sound(void) {
    float slide = sound_slide * sound_slide * (3 - 2 * sound_slide);
    float off = (1 - slide) * 320;
    float x0 = 70, x1 = 570, y0 = 112 + off, y1 = 420 + off;
    hud_panel(x0, y0, x1, y1, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    hud_text(320, y0 + 6, 0.9f, ALIGN_CENTER, argb(255, 255, 220, 60), "SOUND TEST");

    /* Now playing: song, position and an equaliser that pulses on the beat. */
    float secs, len;
    int song = audio_music_now(&secs, &len);
    bool live = audio_music_playing() && audio_music_enabled();
    float cy0 = y0 + 50, cy1 = y0 + 150;
    hud_panel(x0 + 14, cy0, x1 - 14, cy1, argb(200, 8, 24, 70), argb(200, 8, 24, 70), 15.5f);
    char buf[64], name[40];
    upper_copy(name, sizeof(name), dk_music_song_title[song]);
    hud_text(x0 + 30, cy0 + 4, 0.42f, ALIGN_LEFT, argb(255, 150, 200, 255), live ? "NOW PLAYING" : "MUSIC OFF");
    hud_text(x0 + 30, cy0 + 22, 0.75f, ALIGN_LEFT, argb(255, 255, 235, 120), name);
    int rate_pct = (int)(audio_music_rate_now() * 100.0f + 0.5f);
    snprintf(buf, sizeof(buf), "TRACK %d/%d   %d BPM%s", song + 1, DK_MUSIC_SONG_COUNT,
             (int)(dk_music_song_bpm[song] * audio_music_rate_now() + 0.5f), rate_pct > 100 ? "  (FINAL LAP)" : "");
    hud_text(x0 + 30, cy0 + 54, 0.42f, ALIGN_LEFT, argb(255, 200, 225, 255), buf);
    int s = (int)secs, l = (int)(len + 0.5f);
    snprintf(buf, sizeof(buf), "%d:%02d / %d:%02d", s / 60, s % 60, l / 60, l % 60);
    hud_text(x0 + 30, cy0 + 72, 0.42f, ALIGN_LEFT, 0xffffffff, buf);
    float px0 = x0 + 150, px1 = x0 + 300, py = cy0 + 82;
    r_hdr(h_flat);
    r_quad2d(px0, py, px1, py + 6, 0, 0, 1, 1, argb(255, 40, 50, 80), 16.0f);
    r_quad2d(px0, py, px0 + (px1 - px0) * clampf(secs / len, 0, 1), py + 6, 0, 0, 1, 1,
             argb(255, 255, 200, 50), 16.1f);
    float beat = secs * dk_music_song_bpm[song] / 60.0f;
    float kick = live ? expf(-(beat - floorf(beat)) * 5.0f) : 0;
    const int bars = 12;
    float bx0 = x0 + 318, bw = 12, base = cy1 - 12;
    for(int i = 0; i < bars; i++) {
        float wob = 0.5f + 0.5f * sinf(game.title_t * (5.0f + i * 0.7f) + i * 1.9f);
        float shape = 1.0f - fabsf((float)i - (bars - 1) * 0.5f) / bars;
        float h = live ? (10.0f + 62.0f * shape * (0.35f + 0.4f * kick + 0.25f * wob)) : 4.0f;
        float x = bx0 + i * (bw + 3);
        uint32_t top = color_lerp(argb(255, 80, 230, 120), argb(255, 255, 90, 60), clampf(h / 72.0f, 0, 1));
        r_quad2d_grad(x, base - h, x + bw, base, top, argb(255, 40, 140, 220), 16.2f);
    }

    /* Rows. */
    static const char *const labels[SND_ROWS] = {"MUSIC", "SOUND EFFECT", "SPEED"};
    for(int i = 0; i < SND_ROWS; i++) {
        bool sel = i == sound_row;
        float y = y0 + 164 + i * 46;
        if(sel) {
            float glow = 0.18f + 0.1f * sinf(game.title_t * 7) + (i == SND_SFX ? sfx_flash : 0);
            r_hdr(h_flat);
            r_quad2d(x0 + 14, y - 4, x1 - 14, y + 40, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
        }
        uint32_t col = sel ? argb(255, 255, 255, 150) : argb(255, 200, 220, 255);
        hud_text(x0 + 30, y + 8, 0.48f, ALIGN_LEFT, argb(255, 150, 200, 255), labels[i]);
        if(i == SND_MUSIC) {
            upper_copy(name, sizeof(name), dk_music_song_title[sound_song]);
            snprintf(buf, sizeof(buf), "%s", name);
        }
        else if(i == SND_SFX) {
            snprintf(buf, sizeof(buf), "%02d %s", sound_sfx + 1, dk_sfx_label[sound_sfx]);
        }
        else {
            snprintf(buf, sizeof(buf), "%s", audio_music_rate_now() > 1.0f ? "FINAL LAP +6%" : "NORMAL");
        }
        float vx = x0 + 352;
        hud_text(vx, y + 5, 0.56f, ALIGN_CENTER, col, buf);
        if(sel) {
            float bob = 3.0f * sinf(game.title_t * 9);
            hud_text(vx - 134 - bob, y + 5, 0.56f, ALIGN_CENTER, col, "<");
            hud_text(vx + 134 + bob, y + 5, 0.56f, ALIGN_CENTER, col, ">");
        }
    }
    hud_text(320, 440, 0.45f, ALIGN_CENTER, argb(230, 255, 255, 255),
             sound_row == SND_SPEED ? "LEFT/RIGHT: SPEED   B: BACK" : "LEFT/RIGHT: CHOOSE   A: PLAY   B: BACK");
}

static void draw_menu(void) {
    float slide = menu_slide * menu_slide * (3 - 2 * menu_slide);
    float x0 = 640 - 300 * slide, x1 = x0 + 280;
    int rows[8], n = menu_rows(rows);
    /* Six rows (one player's colour too) close up to stay inside the screen. */
    const float pitch = n > 5 ? 46.0f : 50.0f;
    float top = n > 5 ? 96 : 112, y_first = top + 54;
    char laps_buf[8];
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
            r_quad2d(x0 + 10, y - 6, x1 - 10, y + pitch - 8, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
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
            case ROW_LAPS:
                hud_text(x0 + 20, y - 6, 0.5f, ALIGN_LEFT, label, "LAPS");
                snprintf(laps_buf, sizeof(laps_buf), "%d", game.setup_laps);
                value = laps_buf;
                break;
            case ROW_ITEMS:
                hud_text(x0 + 20, y - 6, 0.5f, ALIGN_LEFT, label, "ITEMS");
                value = items_locked() ? "OFF IN TIME TRIAL" : game.setup_items ? "ON" : "OFF";
                if(items_locked()) col = argb(255, 160, 180, 220);
                break;
            case ROW_COLOUR:
                hud_text(x0 + 20, y - 6, 0.5f, ALIGN_LEFT, label, "COLOUR");
                value = kart_colour_look(game.pl[0].colour, NULL).name;
                colour_swatch(x1 - 58, y - 4, game.pl[0].colour);
                break;
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
    hud_text(24, 428, 0.5f, ALIGN_LEFT, argb(230, 255, 255, 255), "A: OK   B: BACK");
    hud_text(176, 100, 0.4f, ALIGN_CENTER, argb(220, 230, 240, 255), mode_blurb());
    if(warn_t > 0 && ((int)(game.title_t * 6) & 1))
        hud_text(320, 72, 0.7f, ALIGN_CENTER, 0xffff8080, "CONNECT CONTROLLER 2");
}

static void draw_karts(void) {
    float slide = karts_slide * karts_slide * (3 - 2 * karts_slide);
    /* Divider between the views, as in the race. */
    r_hdr(h_flat);
    r_quad2d_grad(0, 224, 640, 256, 0xff1c3a8a, 0xff0c1a48, 17.0f);
    r_quad2d(0, 224, 640, 226, 0, 0, 1, 1, 0xfff0f0ff, 17.05f);
    r_quad2d(0, 254, 640, 256, 0, 0, 1, 1, 0xfff0f0ff, 17.05f);
    hud_text(320, 223, 0.8f, ALIGN_CENTER, argb(255, 255, 220, 60), "SELECT KART");
    hud_text(20, 230, 0.42f, ALIGN_LEFT, argb(230, 255, 255, 255), "B: BACK");
    for(int p = 0; p < 2; p++) {
        float y0 = p ? 256.0f : 0.0f;
        float x0 = 640 - 268 * slide, x1 = x0 + 248, top = y0 + 22, bottom = y0 + 202;
        int c = game.pl[p].colour;
        bool ready = kart_ready[p];
        hud_panel(x0, top, x1, bottom, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
        float cx = (x0 + x1) * 0.5f;
        hud_text(cx, top + 6, 0.75f, ALIGN_CENTER, argb(255, 255, 220, 60), p ? "PLAYER 2" : "PLAYER 1");
        uint32_t col = ready ? argb(255, 200, 220, 255) : argb(255, 255, 255, 150);
        hud_text(cx, top + 50, 0.75f, ALIGN_CENTER, col, kart_colour_look(c, NULL).name);
        if(!ready) {
            float bob = 3.0f * sinf(game.title_t * 9);
            hud_text(x0 + 18 - bob, top + 50, 0.75f, ALIGN_LEFT, col, "<");
            hud_text(x1 - 18 + bob, top + 50, 0.75f, ALIGN_RIGHT, col, ">");
        }
        colour_swatch(cx - 16, top + 94, c);
        if(ready) {
            float s = 1.0f + 0.5f * ready_pop[p] * ready_pop[p];
            hud_text(cx, top + 124, s, ALIGN_CENTER, argb(255, 120, 255, 120), "READY!");
        }
        else if(p == 1 && !pad2.connected) {
#if !defined(DK_AUTOTEST) && !defined(DK_FILM)
            if((int)(game.title_t * 4) & 1)
                hud_text(cx, top + 130, 0.5f, ALIGN_CENTER, 0xffff8080, "CONNECT CONTROLLER 2");
#endif
        }
        else {
            float pulse = 0.5f + 0.5f * sinf(game.title_t * 5.0f);
            hud_text(cx, top + 130, 0.5f, ALIGN_CENTER,
                     color_lerp(argb(255, 255, 255, 255), argb(255, 255, 210, 60), pulse), "A: READY");
        }
        if(ready && !kart_ready[p ^ 1])
            hud_text(20, y0 + 180, 0.45f, ALIGN_LEFT, argb(230, 255, 255, 255),
                     p ? "WAITING FOR PLAYER 1" : "WAITING FOR PLAYER 2");
    }
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
        hud_text(320, 440, 0.45f, ALIGN_CENTER, argb(200, 230, 240, 255), "github.com/richstokes");
        /* Build id for bug reports: the commit this was built from. */
        hud_text(604, 446, 0.32f, ALIGN_RIGHT, argb(150, 230, 240, 255), DK_GIT_SHA);
    }
    else if(game.state == ST_TRACKS) {
        draw_tracks();
    }
    else if(game.state == ST_KARTS) {
        draw_karts();
    }
    else if(game.state == ST_MAIN && custom_open) {
        draw_logo(180, 62, 0.36f);
        draw_custom();
    }
    else if(game.state == ST_MAIN && sound_open) {
        draw_sunburst(320, 60, 420, game.title_t * 0.25f, 0.3f);
        draw_logo(320, 60, 0.36f);
        draw_sound();
    }
    else if(game.state == ST_MAIN) {
        draw_sunburst(320, 112, 420, game.title_t * 0.25f, 0.4f);
        draw_logo(320, 112, 0.5f);
        if(settings_open) draw_settings();
        else draw_main();
    }
    else {
        draw_logo(180, 62, 0.36f);
        draw_menu();
    }
}
