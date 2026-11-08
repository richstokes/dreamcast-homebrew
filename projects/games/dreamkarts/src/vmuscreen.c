/* DreamKarts on the VMU screens. In the menus every VMU shows the logo and
   a Chao driving past palms; in a race the VMU in each player's controller
   shows the countdown, then their place, lap and progress round the race,
   then where they finished. Drawn and sent through vmulcd.h. */
#include "vmuscreen.h"
#include "gp.h"
#include "kart.h"
#include "online.h"
#include "track.h"
#include "vmu_assets.h"
#include "vmulcd.h"
#include <string.h>

static float lcd_t;

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

static void sprite(int id, int x, int y) {
    vmulcd_bits(lcd_sprites[id].rows, lcd_sprites[id].h, x, y);
}

static int sprite_w(int id) {
    return lcd_sprites[id].w;
}

/* A strip of 3x3 checks scrolling left along rows y..y+5. */
static void checks(int y, float t) {
    int off = (int)(t * 12.0f) % 6;
    for(int x = -6; x < 48; x += 3)
        for(int row = 0; row < 2; row++)
            if((((x + 6) / 3) + row) % 2 == 0) vmulcd_fill(x - off, y + row * 3, x - off + 3, y + row * 3 + 3);
}

/* ------------------------------------------------------------------ */
/* Screens                                                             */

static void draw_logo_text(float t) {
    sprite(LCD_DREAM, (48 - sprite_w(LCD_DREAM)) / 2 - 1, 2);
    sprite(LCD_KARTS, (48 - sprite_w(LCD_KARTS)) / 2 + 1, 13);
    checks(25, t);
}

/* A Chao driving past palms, the road rushing by underneath. */
static void draw_drive(float t) {
    int scroll = (int)(t * 30.0f);
    for(int i = 0; i < 3; i++) sprite(LCD_PALM, (60 - (scroll / 2 + i * 22) % 66) - 9, 10);
    vmulcd_fill(0, 29, 48, 30);
    for(int x = 0; x < 48; x++)
        if((x + scroll) % 8 < 4) vmulcd_pset(x, 31);
    int bob = (int)(t * 8.0f) & 1;
    int kart = (int)(t * 10.0f) & 1 ? LCD_KART1 : LCD_KART0;
    vmulcd_bits_cut(lcd_sprites[kart].rows, lcd_sprites[kart].h, 11, 10 + bob);
    static const int lines[3][2] = {{15, 5}, {19, 7}, {23, 4}};
    for(int i = 0; i < 3; i++) {
        int s = ((int)(t * 20.0f) + i * 3) % 4;
        vmulcd_fill(4 - s, lines[i][0] + bob, 4 - s + lines[i][1], lines[i][0] + bob + 1);
    }
}

/* Logo and drive take turns. */
static void draw_attract(float t) {
    if((int)(t / 4.0f) & 1) draw_drive(t);
    else draw_logo_text(t);
}

static void draw_online(float t) {
    sprite(LCD_ONLINE, (48 - sprite_w(LCD_ONLINE)) / 2, 3);
    vmulcd_text_c(16, online_name());
    checks(25, t);
}

static void draw_big_centered(int id) {
    sprite(id, (48 - sprite_w(id)) / 2, (32 - lcd_sprites[id].h) / 2);
}

static void draw_countdown(void) {
    int which = (int)game.countdown;    /* as the HUD: 3, 2, 1, GO */
    if(game.countdown < 0) return;
    draw_big_centered(which < 3 ? LCD_BIG0 + 3 - which : LCD_GO);
}

static const char *ordinal(int place) {
    static const char *const s[] = {"TH", "ST", "ND", "RD", "TH", "TH", "TH"};
    return s[place >= 0 && place <= 6 ? place : 0];
}

static void draw_place(int place, int x, int y) {
    if(place < 1 || place > 9) return;
    sprite(LCD_BIG0 + place, x, y);
    vmulcd_text(x + sprite_w(LCD_BIG0 + place) + 2, y + 1, ordinal(place));
}

/* Share of the whole race this kart has driven, 0..1. */
static float race_done(const Kart *k) {
    if(k->finished) return 1.0f;
    float s = k->progress - (float)k->lap * track.length;
    return clampf(((float)(k->lap - 1) + s / track.length) / (float)game.laps, 0, 1);
}

/* The bottom strip: a little kart driving towards the chequered flag, or
   FINAL LAP flashing as the last lap starts. */
static void draw_progress(const Kart *k) {
    if(k->lap == game.laps && game.laps > 1 && game.race_time - k->lap_start < 2.5f) {
        if((int)(lcd_t * 4.0f) & 1) vmulcd_text_c(25, "FINAL LAP");
        return;
    }
    vmulcd_fill(0, 30, 41, 31);
    for(int x = 0; x < 41; x += 4) vmulcd_fill(x, 31, x + 2, 32);
    sprite(LCD_MINI, (int)(race_done(k) * 30.0f), 24);
    vmulcd_fill(42, 22, 43, 32);
    for(int j = 0; j < 3; j++)
        for(int i = 0; i < 2; i++)
            if((i + j) % 2 == 0) vmulcd_fill(43 + 2 * i, 22 + 2 * j, 45 + 2 * i, 24 + 2 * j);
}

static void draw_lap(const Kart *k, int y) {
    char buf[16];
    int lap = k->lap < 1 ? 1 : (k->lap > game.laps ? game.laps : k->lap);
    snprintf(buf, sizeof(buf), "%d/%d", lap, game.laps);
    vmulcd_text(48 - vmulcd_text_w("LAP"), y, "LAP");
    vmulcd_text(48 - vmulcd_text_w(buf), y + 7, buf);
}

static void format_time(char *buf, size_t size, float t) {
    int cs = (int)(clampf(t, 0, 5999.0f) * 100.0f + 0.5f);
    snprintf(buf, size, "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

static void draw_race(const Kart *k) {
    draw_place(k->place, 1, 1);
    draw_lap(k, 2);
    draw_progress(k);
}

/* Time trial on your own: no place, so the clock instead. */
static void draw_time_trial(const Kart *k) {
    char buf[32], t[16];
    int lap = k->lap < 1 ? 1 : (k->lap > game.laps ? game.laps : k->lap);
    snprintf(buf, sizeof(buf), "LAP %d/%d", lap, game.laps);
    vmulcd_text(1, 2, buf);
    format_time(t, sizeof(t), game.race_time - k->lap_start);   /* this lap, like the HUD */
    vmulcd_text(1, 10, t);
    if(k->best_lap > 0) {
        format_time(t, sizeof(t), k->best_lap);
        snprintf(buf, sizeof(buf), "BEST %s", t);
        vmulcd_text(1, 17, buf);
    }
    draw_progress(k);
}

static void draw_finished(const Kart *k) {
    if(game.mode == MODE_TT && game.num_players == 1) {
        char t[16];
        vmulcd_text_c(2, "FINISH!");
        format_time(t, sizeof(t), k->finish_time);
        vmulcd_text_c(11, t);
        if(k->best_lap > 0) {
            char buf[32];
            format_time(t, sizeof(t), k->best_lap);
            snprintf(buf, sizeof(buf), "BEST %s", t);
            vmulcd_text_c(19, buf);
        }
        checks(26, lcd_t);
        return;
    }
    if(gp.active && game.state == ST_RESULTS && gp.page == 1) {
        /* The Grand Prix standings page: this player's standing. */
        int s = gp_standing(k - karts);
        draw_place(s, 2, 1);
        sprite(s <= 3 ? LCD_TROPHY : LCD_FLAG, 32, 3);
        vmulcd_text_c(26, gp_over() ? (s == 1 ? "CHAMPION!" : "GRAND PRIX") : "STANDINGS");
        return;
    }
    draw_place(k->place, 2, 1);
    sprite(k->place <= 3 ? LCD_TROPHY : LCD_FLAG, 32, 3);
    if((int)(lcd_t * 2.0f) & 1 || game.state == ST_RESULTS) vmulcd_text_c(26, "FINISH!");
}

/* Free Roam: where you are on the lot, and how fast. */
static void draw_roam(const Kart *k) {
    char buf[24];
    vmulcd_text_c(2, "FREE ROAM");
    snprintf(buf, sizeof(buf), "%d MPH", (int)(fabsf(k->speed) * 2.23694f + 0.5f));
    vmulcd_text_c(11, buf);
    vmulcd_text_c(19, "NIGHT MEET");
    checks(26, lcd_t);
}

static void draw_pause(void) {
    sprite(LCD_PAUSE, (48 - sprite_w(LCD_PAUSE)) / 2, 4);
    vmulcd_text_c(24, "PAUSED");
}

/* The player whose controller is on this port, -1 for nobody. */
static int owner(int port) {
    int o = -1;
    for(int p = 0; p < game.num_players; p++)
        if(input_pad(p)->port == port) o = p;
    return o;
}

/* The screen for the VMU in this controller. */
static void compose(int port) {
    int player = owner(port);
    const Kart *k = player >= 0 ? &karts[game.pl[player].kart] : NULL;
    bool racing = game.state == ST_COUNTDOWN || game.state == ST_RACE || game.state == ST_RESULTS;
    if(racing && game.paused) draw_pause();
    else if(game.state == ST_COUNTDOWN || (game.state == ST_RACE && game.race_time < 0.8f)) draw_countdown();
    else if(racing && k && (k->finished || game.state == ST_RESULTS)) draw_finished(k);
    else if(game.state == ST_RACE && k) {
        if(track_is_roam()) draw_roam(k);
        else if(game.mode != MODE_TT || game.num_players > 1) draw_race(k);
        else draw_time_trial(k);
    }
    else if(racing || game.state == ST_INTRO) draw_drive(lcd_t);
    else if(game.state == ST_ONLINE) draw_online(lcd_t);
    else draw_attract(lcd_t);
}

/* Idle animations go out 4 times a second, race screens 10 and only the
   countdown 20 (vmulcd.h). */
static float gap(int port) {
    return game.state == ST_COUNTDOWN ? 0.05f : owner(port) >= 0 && game.state == ST_RACE ? 0.1f : 0.25f;
}

void vmuscreen_update(float dt) {
    lcd_t += dt;
    vmulcd_update(dt, gap, compose);
}
