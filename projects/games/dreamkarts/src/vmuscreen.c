/* DreamKarts on the VMU screens. In the menus every VMU shows the logo and
   a Chao driving past palms; in a race the VMU in each player's controller
   shows the countdown, then their place, lap and progress round the race,
   then where they finished.

   Screens are drawn into a 48x32 1-bit buffer each frame and only sent when
   they change, at most 20 times a second per VMU. vmu_draw_lcd only queues
   a maple frame and never waits: if the card is busy (a save in progress)
   the screen just tries again next frame. */
#include "vmuscreen.h"
#include "kart.h"
#include "online.h"
#include "track.h"
#include "vmu_assets.h"
#include <dc/maple/vmu.h>
#include <string.h>

#define SLOTS (MAPLE_PORT_COUNT * 2)    /* a VMU in either slot of each controller */

typedef struct {
    maple_device_t *dev;
    uint32_t sent[48];
    bool valid;
    float since;
} Slot;

static Slot slots[SLOTS];
static uint32_t fb_words[48];          /* 32 rows of 6 bytes; must be 4-byte aligned */
static uint8_t *const fb = (uint8_t *)fb_words;
static float lcd_t;

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

static void pset(int x, int y) {
    if(x >= 0 && x < 48 && y >= 0 && y < 32) fb[y * 6 + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

static void fill(int x0, int y0, int x1, int y1) {
    for(int y = y0; y < y1; y++)
        for(int x = x0; x < x1; x++) pset(x, y);
}

static void blank(int x0, int y0, int x1, int y1) {
    for(int y = y0 < 0 ? 0 : y0; y < y1 && y < 32; y++)
        for(int x = x0 < 0 ? 0 : x0; x < x1 && x < 48; x++) fb[y * 6 + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
}

static void sprite(int id, int x, int y) {
    const LcdSprite *s = &lcd_sprites[id];
    for(int j = 0; j < s->h; j++) {
        uint64_t r = s->rows[j];
        for(int i = 0; r; i++, r <<= 1)
            if(r & (1ull << 63)) pset(x + i, y + j);
    }
}

/* As sprite(), on a cleared silhouette so it stands out from what's behind. */
static void sprite_cut(int id, int x, int y) {
    const LcdSprite *s = &lcd_sprites[id];
    for(int j = -1; j <= s->h; j++) {
        uint64_t r = 0;
        for(int k = j - 1; k <= j + 1; k++)
            if(k >= 0 && k < s->h) r |= s->rows[k];
        r |= r >> 1 | r << 1;
        for(int i = 0; r; i++, r <<= 1)
            if(r & (1ull << 63)) blank(x + i, y + j, x + i + 1, y + j + 1);
    }
    sprite(id, x, y);
}

static int sprite_w(int id) {
    return lcd_sprites[id].w;
}

static int text_w(const char *s) {
    int w = 0;
    for(; *s; s++) {
        int c = *s - 32;
        if(c >= 0 && c < 59) w += lcd_font_w[c] + 1;
    }
    return w > 0 ? w - 1 : 0;
}

static void text(int x, int y, const char *s) {
    for(; *s; s++) {
        int c = *s - 32;
        if(c < 0 || c >= 59) continue;
        for(int j = 0; j < 5; j++)
            for(int i = 0; i < lcd_font_w[c]; i++)
                if(lcd_font[c][j] & (0x80 >> i)) pset(x + i, y + j);
        x += lcd_font_w[c] + 1;
    }
}

static void text_c(int y, const char *s) {
    text((48 - text_w(s)) / 2, y, s);
}

/* A strip of 3x3 checks scrolling left along rows y..y+5. */
static void checks(int y, float t) {
    int off = (int)(t * 12.0f) % 6;
    for(int x = -6; x < 48; x += 3)
        for(int row = 0; row < 2; row++)
            if((((x + 6) / 3) + row) % 2 == 0) fill(x - off, y + row * 3, x - off + 3, y + row * 3 + 3);
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
    fill(0, 29, 48, 30);
    for(int x = 0; x < 48; x++)
        if((x + scroll) % 8 < 4) pset(x, 31);
    int bob = (int)(t * 8.0f) & 1;
    sprite_cut((int)(t * 10.0f) & 1 ? LCD_KART1 : LCD_KART0, 11, 10 + bob);
    static const int lines[3][2] = {{15, 5}, {19, 7}, {23, 4}};
    for(int i = 0; i < 3; i++) {
        int s = ((int)(t * 20.0f) + i * 3) % 4;
        fill(4 - s, lines[i][0] + bob, 4 - s + lines[i][1], lines[i][0] + bob + 1);
    }
}

/* Logo and drive take turns. */
static void draw_attract(float t) {
    if((int)(t / 4.0f) & 1) draw_drive(t);
    else draw_logo_text(t);
}

static void draw_online(float t) {
    sprite(LCD_ONLINE, (48 - sprite_w(LCD_ONLINE)) / 2, 3);
    text_c(16, online_name());
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
    text(x + sprite_w(LCD_BIG0 + place) + 2, y + 1, ordinal(place));
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
        if((int)(lcd_t * 4.0f) & 1) text_c(25, "FINAL LAP");
        return;
    }
    fill(0, 30, 41, 31);
    for(int x = 0; x < 41; x += 4) fill(x, 31, x + 2, 32);
    sprite(LCD_MINI, (int)(race_done(k) * 30.0f), 24);
    fill(42, 22, 43, 32);
    for(int j = 0; j < 3; j++)
        for(int i = 0; i < 2; i++)
            if((i + j) % 2 == 0) fill(43 + 2 * i, 22 + 2 * j, 45 + 2 * i, 24 + 2 * j);
}

static void draw_lap(const Kart *k, int y) {
    char buf[16];
    int lap = k->lap < 1 ? 1 : (k->lap > game.laps ? game.laps : k->lap);
    snprintf(buf, sizeof(buf), "%d/%d", lap, game.laps);
    text(48 - text_w("LAP"), y, "LAP");
    text(48 - text_w(buf), y + 7, buf);
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
    text(1, 2, buf);
    format_time(t, sizeof(t), game.race_time);
    text(1, 10, t);
    if(k->best_lap > 0) {
        format_time(t, sizeof(t), k->best_lap);
        snprintf(buf, sizeof(buf), "BEST %s", t);
        text(1, 17, buf);
    }
    draw_progress(k);
}

static void draw_finished(const Kart *k) {
    if(game.mode == MODE_TT && game.num_players == 1) {
        char t[16];
        text_c(2, "FINISH!");
        format_time(t, sizeof(t), k->finish_time);
        text_c(11, t);
        if(k->best_lap > 0) {
            char buf[32];
            format_time(t, sizeof(t), k->best_lap);
            snprintf(buf, sizeof(buf), "BEST %s", t);
            text_c(19, buf);
        }
        checks(26, lcd_t);
        return;
    }
    draw_place(k->place, 2, 1);
    sprite(k->place <= 3 ? LCD_TROPHY : LCD_FLAG, 32, 3);
    if((int)(lcd_t * 2.0f) & 1 || game.state == ST_RESULTS) text_c(26, "FINISH!");
}

static void draw_pause(void) {
    sprite(LCD_PAUSE, (48 - sprite_w(LCD_PAUSE)) / 2, 4);
    text_c(24, "PAUSED");
}

/* The screen for the VMU in this player's controller (-1: nobody's). */
static void compose(int player) {
    memset(fb_words, 0, sizeof(fb_words));
    const Kart *k = player >= 0 ? &karts[game.pl[player].kart] : NULL;
    bool racing = game.state == ST_COUNTDOWN || game.state == ST_RACE || game.state == ST_RESULTS;
    if(racing && game.paused) draw_pause();
    else if(game.state == ST_COUNTDOWN || (game.state == ST_RACE && game.race_time < 0.8f)) draw_countdown();
    else if(racing && k && (k->finished || game.state == ST_RESULTS)) draw_finished(k);
    else if(game.state == ST_RACE && k) {
        if(game.mode == MODE_GP || game.num_players > 1) draw_race(k);
        else draw_time_trial(k);
    }
    else if(racing || game.state == ST_INTRO) draw_drive(lcd_t);
    else if(game.state == ST_ONLINE) draw_online(lcd_t);
    else draw_attract(lcd_t);
}

/* ------------------------------------------------------------------ */

void vmuscreen_update(float dt) {
    lcd_t += dt;
    for(int s = 0; s < SLOTS; s++) slots[s].since += dt;
    maple_device_t *dev;
    for(int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_LCD)); i++) {
        int s = dev->port * 2 + dev->unit - 1;
        if(s < 0 || s >= SLOTS) continue;
        Slot *sl = &slots[s];
        if(sl->dev != dev) {
            sl->dev = dev;
            sl->valid = false;
        }
        if(sl->since < 0.05f) continue;
        int owner = -1;
        for(int p = 0; p < game.num_players; p++)
            if(input_pad(p)->port == dev->port) owner = p;
        compose(owner);
        if(sl->valid && memcmp(sl->sent, fb_words, sizeof(fb_words)) == 0) continue;
        if(vmu_draw_lcd_rotated(dev, fb_words) == MAPLE_EOK) {
            memcpy(sl->sent, fb_words, sizeof(fb_words));
            sl->valid = true;
            sl->since = 0;
        }
    }
}
