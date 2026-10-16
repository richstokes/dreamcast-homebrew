/* Totally Bazaar - the Jaipur trading card game for Sega Dreamcast, 2-4 players.
 *
 * Flow: title -> setup -> rounds of turns (hot-seat with privacy screens
 * between human players, CPU merchants optional) -> round scoring -> seals ->
 * winner. All rules live in rules.c; this file is presentation and input. */
#include <kos.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "audio.h"
#include "gfx.h"
#include "rules.h"
#include "selftest.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

#define PI 3.14159265f
#ifndef BAZAAR_AUTOPLAY_GAMES
#define BAZAAR_AUTOPLAY_GAMES 3
#endif

/* ------------------------------------------------------------------ */
/* Seats                                                               */

typedef enum { SEAT_HUMAN = 0, SEAT_CPU_EASY, SEAT_CPU_HARD, SEAT_KINDS } seat_kind_t;

static const char *const seat_kind_names[SEAT_KINDS] = { "Human", "CPU Easy", "CPU Hard" };
static const char *const cpu_names[MAX_PLAYERS] = { "Raja", "Rani", "Vizier", "Nawab" };
static const uint32_t seat_colors[MAX_PLAYERS] = {
    0xffe83848, 0xff3a78f0, 0xff34b854, 0xfff4c020
};

static int setup_players = 2;
static seat_kind_t seat_kind[MAX_PLAYERS] = { SEAT_HUMAN, SEAT_CPU_HARD, SEAT_CPU_HARD, SEAT_CPU_HARD };
static char seat_name[MAX_PLAYERS][16];

/* ------------------------------------------------------------------ */
/* Input                                                               */

enum {
    BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_A = 1 << 4, BTN_B = 1 << 5, BTN_X = 1 << 6, BTN_Y = 1 << 7,
    BTN_START = 1 << 8, BTN_L = 1 << 9, BTN_R = 1 << 10
};
#define BTN_DIRS (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)

static uint32_t in_held, in_pressed, in_prev;
static float repeat_timer;
static int pads_connected;

static void input_poll(float dt) {
    uint32_t held = 0;
    int i, pads = 0;
    for(i = 0; i < 4; ++i) {
        maple_device_t *dev = maple_enum_type(i, MAPLE_FUNC_CONTROLLER);
        cont_state_t *st;
        if(!dev)
            break;
        st = (cont_state_t *)maple_dev_status(dev);
        if(!st)
            continue;
        pads++;
        if(st->buttons & CONT_DPAD_UP) held |= BTN_UP;
        if(st->buttons & CONT_DPAD_DOWN) held |= BTN_DOWN;
        if(st->buttons & CONT_DPAD_LEFT) held |= BTN_LEFT;
        if(st->buttons & CONT_DPAD_RIGHT) held |= BTN_RIGHT;
        if(st->buttons & CONT_A) held |= BTN_A;
        if(st->buttons & CONT_B) held |= BTN_B;
        if(st->buttons & CONT_X) held |= BTN_X;
        if(st->buttons & CONT_Y) held |= BTN_Y;
        if(st->buttons & CONT_START) held |= BTN_START;
        if(st->ltrig > 140) held |= BTN_L;
        if(st->rtrig > 140) held |= BTN_R;
        if(st->joyy < -64) held |= BTN_UP;
        if(st->joyy > 64) held |= BTN_DOWN;
        if(st->joyx < -64) held |= BTN_LEFT;
        if(st->joyx > 64) held |= BTN_RIGHT;
    }
    pads_connected = pads;
    in_pressed = held & ~in_prev;
    /* Auto-repeat directions while held. */
    if(held & BTN_DIRS & in_prev) {
        repeat_timer -= dt;
        if(repeat_timer <= 0.0f) {
            in_pressed |= held & BTN_DIRS;
            repeat_timer = 0.085f;
        }
    }
    else {
        repeat_timer = 0.32f;
    }
    in_prev = held;
    in_held = held;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float approach(float cur, float target, float rate, float dt) {
    return target + (cur - target) * expf(-rate * dt);
}

static float ease_out_back(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    t = clampf(t, 0.0f, 1.0f);
    return 1.0f + c3 * powf(t - 1.0f, 3.0f) + c1 * powf(t - 1.0f, 2.0f);
}

static float frand(void) {
    return (float)(rand() & 0xffff) / 65535.0f;
}

static float time_now;   /* seconds since boot, for animation */

/* ------------------------------------------------------------------ */
/* Game session                                                        */

typedef enum {
    ST_TITLE, ST_SETUP, ST_RULES, ST_DEAL, ST_PASS, ST_TURN_HUMAN,
    ST_TURN_CPU, ST_ANIMATE, ST_ROUND_END, ST_GAME_OVER
} state_t;

static state_t state = ST_TITLE;
static state_t rules_return = ST_TITLE;
static float state_time;
static game_t game;
static int viewer;
static bool paused;
static int pause_cursor;
static bool demo_mode;
static float idle_time;
static uint32_t ai_rng = 0x51f15eedu;
static char action_log[3][64];
static float cpu_timer;
static move_t cpu_move;
static bool cpu_has_move;
static int saved_players = 2;

static void set_state(state_t s) {
    state = s;
    state_time = 0.0f;
}

static int human_count(void) {
    int p, n = 0;
    for(p = 0; p < game.num_players; ++p)
        if(seat_kind[p] == SEAT_HUMAN && !demo_mode)
            n++;
    return n;
}

static bool is_human(int p) {
    return !demo_mode && seat_kind[p] == SEAT_HUMAN;
}

static void log_action(const char *text) {
    memmove(action_log[1], action_log[0], sizeof(action_log[0]) * 2);
    snprintf(action_log[0], sizeof(action_log[0]), "%s", text);
    printf("BAZAAR: %s\n", text);
}

/* ------------------------------------------------------------------ */
/* Effects: particles, floating text, token flights, banners           */

#define MAX_PARTICLES 320
typedef struct {
    float x, y, vx, vy, life, max, size, rot, vr, gravity;
    uint32_t color;
    uint8_t kind;  /* 0 star, 1 glow, 2 confetti */
} particle_t;
static particle_t particles[MAX_PARTICLES];

static void spawn_particle(int kind, float x, float y, float vx, float vy,
                           float life, float size, uint32_t color, float gravity) {
    int i;
    for(i = 0; i < MAX_PARTICLES; ++i) {
        particle_t *p = &particles[i];
        if(p->life > 0.0f)
            continue;
        p->kind = (uint8_t)kind;
        p->x = x;
        p->y = y;
        p->vx = vx;
        p->vy = vy;
        p->life = p->max = life;
        p->size = size;
        p->rot = frand() * PI;
        p->vr = (frand() - 0.5f) * 8.0f;
        p->color = color;
        p->gravity = gravity;
        return;
    }
}

static void sparkle_burst(float x, float y, int n, uint32_t color) {
    int i;
    for(i = 0; i < n; ++i) {
        float a = frand() * 2.0f * PI, s = 40.0f + frand() * 140.0f;
        spawn_particle(0, x, y, cosf(a) * s, sinf(a) * s - 40.0f,
                       0.5f + frand() * 0.5f, 10.0f + frand() * 14.0f, color, 200.0f);
    }
}

static void confetti_burst(float x, float y, int n) {
    static const uint32_t cols[] = { 0xffe83848, 0xff3a78f0, 0xff34b854,
                                     0xfff4c020, 0xffff70d0, 0xff40e0f0 };
    int i;
    for(i = 0; i < n; ++i) {
        float a = -PI * 0.5f + (frand() - 0.5f) * 2.2f, s = 120.0f + frand() * 300.0f;
        spawn_particle(2, x, y, cosf(a) * s, sinf(a) * s, 2.0f + frand() * 1.5f,
                       5.0f + frand() * 5.0f, cols[rand() % 6], 260.0f);
    }
}

static void update_particles(float dt) {
    int i;
    for(i = 0; i < MAX_PARTICLES; ++i) {
        particle_t *p = &particles[i];
        if(p->life <= 0.0f)
            continue;
        p->life -= dt;
        p->vy += p->gravity * dt;
        if(p->kind == 2) {
            p->vx *= expf(-1.5f * dt);
            p->vy = fminf(p->vy, 90.0f);
        }
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->rot += p->vr * dt;
    }
}

static void draw_particles(void) {
    int i;
    for(i = 0; i < MAX_PARTICLES; ++i) {
        particle_t *p = &particles[i];
        float a;
        if(p->life <= 0.0f || p->kind == 2)
            continue;
        a = clampf(p->life / p->max * 1.5f, 0.0f, 1.0f);
        gfx_set_additive(true);
        gfx_cell(p->kind == 0 ? JP_ATLAS_STAR : JP_ATLAS_GLOW, p->x, p->y,
                 p->size * (0.5f + 0.5f * a), p->rot, color_alpha(p->color, a));
        gfx_set_additive(false);
    }
    for(i = 0; i < MAX_PARTICLES; ++i) {
        particle_t *p = &particles[i];
        if(p->life <= 0.0f || p->kind != 2)
            continue;
        {
            float w = p->size * fabsf(cosf(p->rot * 2.0f)) + 1.0f;
            float a = clampf(p->life, 0.0f, 1.0f);
            float s = sinf(p->rot), c = cosf(p->rot);
            float hw = w * 0.5f, hh = p->size * 0.35f;
            float xy[8] = {
                p->x - hw * c + hh * s, p->y - hw * s - hh * c,
                p->x + hw * c + hh * s, p->y + hw * s - hh * c,
                p->x - hw * c - hh * s, p->y - hw * s + hh * c,
                p->x + hw * c - hh * s, p->y + hw * s + hh * c
            };
            uint32_t col = color_alpha(p->color, a);
            uint32_t cs[4] = { col, col, col, col };
            gfx_quad(-1, xy, NULL, cs);
        }
    }
}

#define MAX_FLOATERS 24
typedef struct {
    float x, y, life, max, delay;
    float scale;
    uint32_t color;
    char text[32];
} floater_t;
static floater_t floaters[MAX_FLOATERS];

static void spawn_floater(float x, float y, float delay, float scale, uint32_t color,
                          const char *text) {
    int i;
    for(i = 0; i < MAX_FLOATERS; ++i) {
        floater_t *f = &floaters[i];
        if(f->life > 0.0f)
            continue;
        f->x = x;
        f->y = y;
        f->life = f->max = 1.4f;
        f->delay = delay;
        f->scale = scale;
        f->color = color;
        snprintf(f->text, sizeof(f->text), "%s", text);
        return;
    }
}

static void update_floaters(float dt) {
    int i;
    for(i = 0; i < MAX_FLOATERS; ++i) {
        floater_t *f = &floaters[i];
        if(f->life <= 0.0f)
            continue;
        if(f->delay > 0.0f) {
            f->delay -= dt;
            continue;
        }
        f->life -= dt;
        f->y -= 34.0f * dt;
    }
}

static void draw_floaters(void) {
    int i;
    for(i = 0; i < MAX_FLOATERS; ++i) {
        floater_t *f = &floaters[i];
        float t, pop;
        if(f->life <= 0.0f || f->delay > 0.0f)
            continue;
        t = 1.0f - f->life / f->max;
        pop = ease_out_back(clampf(t * 5.0f, 0.0f, 1.0f));
        gfx_text(FONT_BIG, f->x, f->y, f->scale * pop,
                 color_alpha(f->color, clampf(f->life * 2.0f, 0.0f, 1.0f)),
                 ALIGN_CENTER, f->text);
    }
}

#define MAX_FLIGHTS 24
typedef struct {
    int cell;
    int value;
    float x0, y0, x1, y1;
    float t, delay, dur;
    bool active;
    float pitch;
} flight_t;
static flight_t flights[MAX_FLIGHTS];

static void spawn_flight(int cell, int value, float x0, float y0, float x1,
                         float y1, float delay) {
    int i;
    for(i = 0; i < MAX_FLIGHTS; ++i) {
        flight_t *f = &flights[i];
        if(f->active)
            continue;
        f->active = true;
        f->cell = cell;
        f->value = value;
        f->x0 = x0;
        f->y0 = y0;
        f->x1 = x1;
        f->y1 = y1;
        f->t = 0.0f;
        f->delay = delay;
        f->dur = 0.55f;
        f->pitch = 0.9f + 0.05f * (float)value;
        return;
    }
}

static int active_flights(void) {
    int i, n = 0;
    for(i = 0; i < MAX_FLIGHTS; ++i)
        n += flights[i].active;
    return n;
}

static void update_flights(float dt) {
    int i;
    for(i = 0; i < MAX_FLIGHTS; ++i) {
        flight_t *f = &flights[i];
        if(!f->active)
            continue;
        if(f->delay > 0.0f) {
            f->delay -= dt;
            if(f->delay <= 0.0f)
                audio_play(JP_SFX_WHOOSH, 70, 128);
            continue;
        }
        f->t += dt / f->dur;
        if(f->t >= 1.0f) {
            f->active = false;
            audio_play_pitched(f->cell >= JP_ATLAS_BONUS3 && f->cell <= JP_ATLAS_BONUS5 ?
                               JP_SFX_BONUS : JP_SFX_COIN, 200, 128, f->pitch);
            sparkle_burst(f->x1, f->y1, 10, 0xffffe080);
        }
    }
}

static void draw_coin_value(float x, float y, float size, int value) {
    if(value > 0)
        gfx_textf(FONT_SMALL, x, y - size * 0.02f, size / 40.0f, 0xffffffff,
                  ALIGN_CENTER, "%d", value);
}

static void draw_flights(void) {
    int i;
    for(i = 0; i < MAX_FLIGHTS; ++i) {
        flight_t *f = &flights[i];
        float t, x, y, s;
        if(!f->active || f->delay > 0.0f)
            continue;
        t = f->t;
        t = t * t * (3.0f - 2.0f * t);
        x = f->x0 + (f->x1 - f->x0) * t;
        y = f->y0 + (f->y1 - f->y0) * t - sinf(t * PI) * 60.0f;
        s = 34.0f + sinf(t * PI) * 22.0f;
        gfx_set_additive(true);
        gfx_cell(JP_ATLAS_GLOW, x, y, s * 1.8f, 0.0f, 0x80ffd070);
        gfx_set_additive(false);
        gfx_cell(f->cell, x, y, s, sinf(t * PI * 2.0f) * 0.4f, 0xffffffff);
        if(f->cell < JP_ATLAS_BONUS3 || f->cell == JP_ATLAS_CAMEL_TOKEN)
            draw_coin_value(x, y, s, f->value);
    }
}

typedef struct {
    char text[40];
    char sub[64];
    uint32_t color;
    float t, dur;
} banner_t;
static banner_t banner;

static void show_banner(const char *text, const char *sub, uint32_t color, float dur) {
    snprintf(banner.text, sizeof(banner.text), "%s", text);
    snprintf(banner.sub, sizeof(banner.sub), "%s", sub ? sub : "");
    banner.color = color;
    banner.t = 0.0f;
    banner.dur = dur;
}

static void draw_banner(void) {
    float t, in, out, a, y;
    if(banner.dur <= 0.0f || banner.t >= banner.dur)
        return;
    t = banner.t;
    in = ease_out_back(clampf(t * 3.0f, 0.0f, 1.0f));
    out = clampf((banner.dur - t) * 4.0f, 0.0f, 1.0f);
    a = out;
    y = 214.0f;
    gfx_rect_grad(0, y - 34.0f * in, SCREEN_W, 68.0f * in + (banner.sub[0] ? 24.0f * in : 0.0f),
                  color_alpha(0xc0200828, a), color_alpha(0xe0401040, a));
    gfx_rect(0, y - 34.0f * in, SCREEN_W, 3, color_alpha(banner.color, a));
    gfx_rect(0, y + 34.0f * in + (banner.sub[0] ? 24.0f * in : 0.0f) - 3, SCREEN_W, 3,
             color_alpha(banner.color, a));
    gfx_text(FONT_BIG, 320.0f + (1.0f - in) * -200.0f, y - 26.0f, 1.0f,
             color_alpha(banner.color, a), ALIGN_CENTER, banner.text);
    if(banner.sub[0])
        gfx_text(FONT_SMALL, 320.0f, y + 22.0f, 1.0f, color_alpha(0xffffffff, a),
                 ALIGN_CENTER, banner.sub);
}

/* ------------------------------------------------------------------ */
/* Card visuals and layout                                             */

#define CW 72.0f
#define CH 101.0f
#define CARD_V1 (180.0f / 256.0f)

#define DECK_X 60.0f
#define DECK_Y 166.0f
#define MARKET_X0 172.0f
#define MARKET_DX 82.0f
#define MARKET_Y 166.0f
#define DISCARD_X 60.0f
#define DISCARD_Y 262.0f
#define HAND_X0 150.0f
#define HAND_DX 74.0f
#define HAND_Y 358.0f
#define HERD_X 60.0f
#define HERD_Y 358.0f
#define PANEL_Y 24.0f
#define PANEL_H 72.0f
#define PANEL_W 196.0f

enum { ZONE_DECK = 0, ZONE_MARKET, ZONE_HAND, ZONE_HERD, ZONE_DISCARD };

typedef struct {
    float x, y, rot, scale, flip, layer;
    float tx, ty, trot, tscale, tflip, tlayer;
    float delay;
    float travel;      /* distance at departure, for the lift arc */
    int zone, owner, index;
    bool moving, visible, placed, sounded;
    float glow, tglow;
} card_vis_t;

static card_vis_t cv[CARD_COUNT];
static float stagger_clock;
static float stagger_step = 0.05f;
static bool collecting;          /* every card heads to the deck */
static float last_card_sound;

/* Human selection state. */
static bool sel_card[CARD_COUNT];
static int herd_sel;
static int cursor_row;           /* 0 market, 1 hand */
static int cursor_col;
static uint8_t cpu_preview_mask; /* market slots a CPU is about to take */
static float hint_flash;
static char hint_error[96];

static int sorted_hand[HAND_LIMIT];
static int sorted_count;

static void sort_hand(int player) {
    const player_t *pl = &game.players[player];
    int i, j;
    sorted_count = pl->hand_count;
    for(i = 0; i < sorted_count; ++i)
        sorted_hand[i] = pl->hand[i];
    for(i = 1; i < sorted_count; ++i) {
        int v = sorted_hand[i];
        int key = game.card_type[v] * 64 + v;
        for(j = i - 1; j >= 0 &&
            game.card_type[sorted_hand[j]] * 64 + sorted_hand[j] > key; --j)
            sorted_hand[j + 1] = sorted_hand[j];
        sorted_hand[j + 1] = v;
    }
}

static int opponent_slot(int p) {
    int n = game.num_players;
    return (p - viewer - 1 + n) % n;
}

static float panel_x(int slot) {
    int n = game.num_players - 1;
    float total = n * PANEL_W + (n - 1) * 10.0f;
    return (SCREEN_W - total) * 0.5f + slot * (PANEL_W + 10.0f);
}

static bool hands_hidden(void) {
    if(demo_mode)
        return true;
    if(state == ST_ROUND_END || state == ST_GAME_OVER)
        return false;
    if(human_count() >= 2)
        return !(state == ST_TURN_HUMAN && game.current == viewer && !paused);
    return false;
}

static void set_target(int id, int zone, int owner, int index, float x, float y,
                       float rot, float scale, float flip, float layer, bool visible) {
    card_vis_t *c = &cv[id];
    if(!c->placed) {
        c->x = x;
        c->y = y;
        c->rot = rot;
        c->scale = scale;
        c->flip = flip;
        c->layer = layer;
        c->placed = true;
        c->zone = zone;
        c->owner = owner;
        c->index = index;
    }
    if(c->zone != zone || c->owner != owner) {
        c->moving = true;
        c->sounded = false;
        c->delay = stagger_clock;
        stagger_clock += stagger_step;
        c->travel = hypotf(x - c->x, y - c->y);
        c->zone = zone;
        c->owner = owner;
    }
    c->index = index;
    c->tglow = 0.0f;
    c->tx = x;
    c->ty = y;
    c->trot = rot;
    c->tscale = scale;
    c->tflip = flip;
    c->tlayer = layer;
    c->visible = visible;
}

static void layout_cards(void) {
    int i, p, s;
    bool hidden = hands_hidden();
    float lift;

    stagger_clock = 0.0f;
    if(collecting) {
        for(i = 0; i < CARD_COUNT; ++i)
            set_target(i, ZONE_DECK, -1, 0, DECK_X - (i % 8) * 0.3f, DECK_Y - (i % 8) * 0.4f,
                       0.0f, 1.0f, 1.0f, 1.0f + i * 0.001f, true);
        return;
    }
    for(i = 0; i < game.deck_count; ++i) {
        int id = game.deck[i];
        int k = i - (game.deck_count - 8);
        float off = (float)(k > 0 ? k : 0);
        set_target(id, ZONE_DECK, -1, i, DECK_X - off * 0.4f, DECK_Y - off * 0.6f,
                   0.0f, 1.0f, 1.0f, 1.0f + i * 0.001f, k > -2);
    }
    for(i = 0; i < game.discard_count; ++i) {
        int id = game.discard[i];
        float rot = (float)((id * 37) % 11 - 5) * 0.035f;
        set_target(id, ZONE_DISCARD, -1, i, DISCARD_X + (float)((id * 13) % 5 - 2),
                   DISCARD_Y, rot, 0.72f, 0.0f, 2.0f + i * 0.001f,
                   i >= game.discard_count - 4);
    }
    for(s = 0; s < MARKET_SIZE; ++s) {
        int id = game.market[s];
        if(id == NO_CARD)
            continue;
        lift = 0.0f;
        if(sel_card[id] || (cpu_preview_mask & (1u << s)))
            lift = 14.0f;
        set_target(id, ZONE_MARKET, -1, s, MARKET_X0 + s * MARKET_DX, MARKET_Y - lift,
                   0.0f, 1.0f, 0.0f, 3.0f + (lift > 0 ? 0.5f : 0.0f), true);
        cv[id].tglow = lift > 0.0f ? 1.0f : 0.0f;
    }
    for(p = 0; p < game.num_players; ++p) {
        const player_t *pl = &game.players[p];
        if(p == viewer) {
            sort_hand(p);
            for(i = 0; i < sorted_count; ++i) {
                int id = sorted_hand[i];
                lift = sel_card[id] && !hidden ? 20.0f : 0.0f;
                set_target(id, ZONE_HAND, p, i, HAND_X0 + i * HAND_DX, HAND_Y - lift,
                           0.0f, 1.0f, hidden ? 1.0f : 0.0f, 6.0f + i * 0.01f, true);
                cv[id].tglow = lift > 0.0f ? 1.0f : 0.0f;
            }
            for(i = 0; i < pl->herd_count; ++i) {
                int id = pl->herd[i];
                bool picked = i >= pl->herd_count - herd_sel;
                set_target(id, ZONE_HERD, p, i, HERD_X + (picked ? 6.0f : 0.0f),
                           HERD_Y - i * 3.0f - (picked ? 12.0f : 0.0f), 0.0f, 1.0f, 0.0f,
                           5.0f + i * 0.01f, i >= pl->herd_count - 6);
                cv[id].tglow = picked ? 1.0f : 0.0f;
            }
        }
        else {
            float px = panel_x(opponent_slot(p));
            for(i = 0; i < pl->hand_count; ++i)
                set_target(pl->hand[i], ZONE_HAND, p, i, px + 22.0f + i * 11.0f,
                           PANEL_Y + 50.0f, (i - 3) * 0.05f, 0.3f, 1.0f, 4.0f + i * 0.01f,
                           true);
            /* Herds show as an icon and count; cards only appear in flight. */
            for(i = 0; i < pl->herd_count; ++i)
                set_target(pl->herd[i], ZONE_HERD, p, i, px + 130.0f, PANEL_Y + 44.0f,
                           0.0f, 0.22f, 0.0f, 4.5f + i * 0.01f, false);
        }
    }
}

static void update_cards(float dt) {
    int i;
    for(i = 0; i < CARD_COUNT; ++i) {
        card_vis_t *c = &cv[i];
        float d;
        if(c->delay > 0.0f) {
            c->delay -= dt;
            continue;
        }
        if(c->moving && !c->sounded) {
            c->sounded = true;
            if(time_now - last_card_sound > 0.035f) {
                audio_play_pitched(JP_SFX_CARD, 150, (int)clampf(c->tx / 2.5f, 0, 255),
                                   0.9f + frand() * 0.2f);
                last_card_sound = time_now;
            }
        }
        c->x = approach(c->x, c->tx, 11.0f, dt);
        c->y = approach(c->y, c->ty, 11.0f, dt);
        c->rot = approach(c->rot, c->trot, 10.0f, dt);
        c->scale = approach(c->scale, c->tscale, 10.0f, dt);
        c->flip = approach(c->flip, c->tflip, 9.0f, dt);
        c->glow = approach(c->glow, c->tglow, 12.0f, dt);
        d = fabsf(c->x - c->tx) + fabsf(c->y - c->ty);
        if(c->moving && d < 1.0f && fabsf(c->flip - c->tflip) < 0.02f)
            c->moving = false;
        c->layer = c->moving ? 20.0f + c->tlayer * 0.01f : c->tlayer;
    }
}

static int cards_moving(void) {
    int i, n = 0;
    for(i = 0; i < CARD_COUNT; ++i)
        if(cv[i].moving)
            n++;
    return n;
}

static void draw_card_at(int type, float x, float y, float scale, float rot,
                         float flip, uint32_t tint, bool shadow) {
    float angle = flip * PI;
    float cw = cosf(angle);
    float w = CW * scale * fabsf(cw);
    float h = CH * scale;
    int tex = cw >= 0.0f ? JP_TEXID_CARD_DIAMOND + type : JP_TEXID_CARD_BACK;
    float skew = sinf(angle) * 0.08f * h;
    float s = sinf(rot), c = cosf(rot);
    float lx[4] = { -w * 0.5f, w * 0.5f, -w * 0.5f, w * 0.5f };
    float ly[4] = { -h * 0.5f - skew, -h * 0.5f + skew, h * 0.5f + skew * 0.3f,
                    h * 0.5f - skew * 0.3f };
    float xy[8];
    float uv[8] = { 0, 0, 1, 0, 0, CARD_V1, 1, CARD_V1 };
    uint32_t col[4] = { tint, tint, tint, tint };
    int i;
    if(w < 0.5f)
        return;
    if(shadow) {
        uint32_t sc = 0x58000000;
        uint32_t scs[4] = { sc, sc, sc, sc };
        float off = 3.0f + scale * 3.0f;
        for(i = 0; i < 4; ++i) {
            xy[i * 2] = x + off * 0.6f + lx[i] * c - ly[i] * s;
            xy[i * 2 + 1] = y + off + lx[i] * s + ly[i] * c;
        }
        gfx_quad(tex, xy, uv, scs);
    }
    for(i = 0; i < 4; ++i) {
        xy[i * 2] = x + lx[i] * c - ly[i] * s;
        xy[i * 2 + 1] = y + lx[i] * s + ly[i] * c;
    }
    gfx_quad(tex, xy, uv, col);
}

static void draw_cards(void) {
    int order[CARD_COUNT];
    int n = 0, i, j;
    for(i = 0; i < CARD_COUNT; ++i)
        if(cv[i].placed && (cv[i].visible || cv[i].moving))
            order[n++] = i;
    for(i = 1; i < n; ++i) {
        int v = order[i];
        for(j = i - 1; j >= 0 && cv[order[j]].layer > cv[v].layer; --j)
            order[j + 1] = order[j];
        order[j + 1] = v;
    }
    for(i = 0; i < n; ++i) {
        card_vis_t *c = &cv[order[i]];
        float lift = 0.0f;
        if(c->moving && c->travel > 40.0f) {
            float rem = (fabsf(c->x - c->tx) + fabsf(c->y - c->ty)) / (c->travel + 1.0f);
            lift = sinf(clampf(rem, 0.0f, 1.0f) * PI) * 0.14f;
        }
        if(c->glow > 0.02f) {
            float pulse = 0.75f + 0.25f * sinf(time_now * 7.0f);
            gfx_set_additive(true);
            gfx_panel(c->x - CW * c->scale * 0.5f - 7, c->y - CH * c->scale * 0.5f - 7,
                      CW * c->scale + 14, CH * c->scale + 14, 14,
                      color_alpha(0xffffd040, c->glow * pulse));
            gfx_set_additive(false);
        }
        draw_card_at(game.card_type[order[i]], c->x, c->y, c->scale * (1.0f + lift),
                     c->rot, c->flip, 0xffffffff, c->scale > 0.5f || lift > 0.01f);
    }
}

/* Visual positions used by HUD elements. */
static void score_anchor(int p, float *x, float *y) {
    if(p == viewer) {
        *x = 150.0f;
        *y = 446.0f;
    }
    else {
        *x = panel_x(opponent_slot(p)) + 172.0f;
        *y = PANEL_Y + 47.0f;
    }
}

static void bazaar_pos(int item, float *x, float *y) {
    /* Items 0-5 goods, 6-8 bonus 3/4/5, 9 camel token. */
    int col = item < 5 ? 0 : 1;
    int row = item < 5 ? item : item - 5;
    *x = 574.0f + col * 40.0f;
    *y = 128.0f + row * 36.0f;
}

/* ------------------------------------------------------------------ */
/* Move building from the selection                                     */

typedef enum { BUILD_NOTHING, BUILD_VALID, BUILD_INCOMPLETE, BUILD_INVALID } build_t;

static void clear_selection(void) {
    memset(sel_card, 0, sizeof(sel_card));
    herd_sel = 0;
}

static build_t build_move(move_t *m, char *desc, int len) {
    const player_t *pl = &game.players[game.current];
    uint8_t hand_types[GOOD_TYPES];
    int mg = 0, mc = 0, hs = 0, types = 0, last_type = -1;
    int s, i, t;
    move_error_t e;

    memset(m, 0, sizeof(*m));
    memset(hand_types, 0, sizeof(hand_types));
    for(s = 0; s < MARKET_SIZE; ++s) {
        int id = game.market[s];
        if(id == NO_CARD || !sel_card[id])
            continue;
        if(game.card_type[id] == GOOD_CAMEL)
            mc++;
        else {
            mg++;
            m->market_mask |= (uint8_t)(1u << s);
        }
    }
    for(i = 0; i < pl->hand_count; ++i) {
        int id = pl->hand[i];
        if(!sel_card[id])
            continue;
        t = game.card_type[id];
        if(!hand_types[t])
            types++;
        hand_types[t]++;
        last_type = t;
        hs++;
    }

    if(mg + mc + hs + herd_sel == 0) {
        snprintf(desc, (size_t)len, "Take from the market or sell from your hand");
        return BUILD_NOTHING;
    }
    if(mc > 0) {
        if(mg || hs || herd_sel) {
            snprintf(desc, (size_t)len, "Camels are taken on their own");
            return BUILD_INVALID;
        }
        m->type = MOVE_TAKE_CAMELS;
        snprintf(desc, (size_t)len, "Take %d camel%s", mc, mc > 1 ? "s" : "");
        return BUILD_VALID;
    }
    if(mg == 0) {
        if(herd_sel > 0 && hs == 0) {
            snprintf(desc, (size_t)len, "Pick market goods to swap camels for");
            return BUILD_INCOMPLETE;
        }
        if(herd_sel > 0) {
            snprintf(desc, (size_t)len, "Pick market goods for your exchange");
            return BUILD_INCOMPLETE;
        }
        m->type = MOVE_SELL;
        if(types > 1) {
            snprintf(desc, (size_t)len, "Sell one type - or pick market goods");
            return BUILD_INCOMPLETE;
        }
        m->sell_good = (uint8_t)last_type;
        m->sell_count = (uint8_t)hs;
        e = jp_validate(&game, m);
        if(e != MOVE_OK) {
            snprintf(desc, (size_t)len, "%s", jp_error_text(e));
            return BUILD_INCOMPLETE;
        }
        {
            char vals[40] = "";
            int k, pos = 0, avail = game.goods_token_count[last_type];
            for(k = 0; k < hs && k < avail && pos < 30; ++k)
                pos += snprintf(vals + pos, sizeof(vals) - (size_t)pos, "%s%d",
                                k ? "+" : "", jp_token_peek(&game, last_type, k));
            if(avail == 0)
                snprintf(vals, sizeof(vals), "no tokens left");
            snprintf(desc, (size_t)len, "Sell %d %s: %s%s", hs,
                     hs > 1 ? jp_good_names[last_type] : jp_good_singular[last_type],
                     vals, hs >= 3 ? " +bonus" : "");
        }
        return BUILD_VALID;
    }
    if(hs == 0 && herd_sel == 0) {
        if(mg == 1) {
            m->type = MOVE_TAKE_ONE;
            e = jp_validate(&game, m);
            if(e != MOVE_OK) {
                snprintf(desc, (size_t)len, "%s", jp_error_text(e));
                return BUILD_INVALID;
            }
            for(s = 0; s < MARKET_SIZE; ++s)
                if(m->market_mask & (1u << s))
                    snprintf(desc, (size_t)len, "Take 1 %s",
                             jp_good_singular[game.card_type[game.market[s]]]);
            return BUILD_VALID;
        }
        snprintf(desc, (size_t)len, "Exchange: give %d cards from hand or herd", mg);
        return BUILD_INCOMPLETE;
    }
    m->type = MOVE_EXCHANGE;
    for(t = 0; t < TRADE_GOODS; ++t)
        m->give[t] = hand_types[t];
    m->give_camels = (uint8_t)herd_sel;
    if(mg < 2) {
        snprintf(desc, (size_t)len, "Exchanges take at least 2 goods");
        return BUILD_INCOMPLETE;
    }
    if(hs + herd_sel < mg) {
        snprintf(desc, (size_t)len, "Exchange: give %d more card%s", mg - hs - herd_sel,
                 mg - hs - herd_sel > 1 ? "s" : "");
        return BUILD_INCOMPLETE;
    }
    if(hs + herd_sel > mg) {
        snprintf(desc, (size_t)len, "Give exactly %d - you picked %d", mg, hs + herd_sel);
        return BUILD_INVALID;
    }
    e = jp_validate(&game, m);
    if(e != MOVE_OK) {
        snprintf(desc, (size_t)len, "%s", jp_error_text(e));
        return BUILD_INVALID;
    }
    snprintf(desc, (size_t)len, "Exchange %d for %d", mg, mg);
    return BUILD_VALID;
}

/* Turns a move into a selection (for the hint button). */
static void select_move(const move_t *m) {
    const player_t *pl = &game.players[game.current];
    int s, i, t;
    clear_selection();
    if(m->type == MOVE_TAKE_CAMELS) {
        for(s = 0; s < MARKET_SIZE; ++s)
            if(game.market[s] != NO_CARD && game.card_type[game.market[s]] == GOOD_CAMEL)
                sel_card[game.market[s]] = true;
        return;
    }
    for(s = 0; s < MARKET_SIZE; ++s)
        if(m->market_mask & (1u << s))
            sel_card[game.market[s]] = true;
    if(m->type == MOVE_SELL) {
        int left = m->sell_count;
        for(i = 0; i < pl->hand_count && left; ++i)
            if(game.card_type[pl->hand[i]] == m->sell_good) {
                sel_card[pl->hand[i]] = true;
                left--;
            }
    }
    if(m->type == MOVE_EXCHANGE) {
        for(t = 0; t < TRADE_GOODS; ++t) {
            int left = m->give[t];
            for(i = 0; i < pl->hand_count && left; ++i)
                if(game.card_type[pl->hand[i]] == t) {
                    sel_card[pl->hand[i]] = true;
                    left--;
                }
        }
        herd_sel = m->give_camels;
    }
}

/* ------------------------------------------------------------------ */
/* Applying moves                                                       */

static void describe_move(const move_result_t *r, char *out, int len) {
    const move_t *m = &r->move;
    const char *who = seat_name[r->player];
    int s, n = 0, t;
    switch(m->type) {
    case MOVE_TAKE_ONE:
        for(s = 0; s < MARKET_SIZE; ++s)
            if(m->market_mask & (1u << s))
                n = s;
        snprintf(out, (size_t)len, "%s took 1 %s", who,
                 jp_good_singular[game.card_type[game.players[r->player].hand[
                     game.players[r->player].hand_count - 1]]]);
        (void)n;
        break;
    case MOVE_TAKE_CAMELS:
        for(s = 0; s < MARKET_SIZE; ++s)
            n += (m->market_mask >> s) & 1;
        snprintf(out, (size_t)len, "%s took %d camel%s", who, n, n > 1 ? "s" : "");
        break;
    case MOVE_EXCHANGE:
        for(s = 0; s < MARKET_SIZE; ++s)
            n += (m->market_mask >> s) & 1;
        t = m->give_camels;
        snprintf(out, (size_t)len, "%s exchanged %d card%s%s", who, n, n > 1 ? "s" : "",
                 t ? " using camels" : "");
        break;
    default:
        snprintf(out, (size_t)len, "%s sold %d %s for %d", who, m->sell_count,
                 m->sell_count > 1 ? jp_good_names[m->sell_good] :
                 jp_good_singular[m->sell_good], r->points);
        break;
    }
}

static void begin_round_end(void);

static void perform_move(const move_t *m) {
    move_result_t r;
    char text[64];
    int p = game.current;
    int i;
    float sx, sy, bx, by;

    if(!jp_apply(&game, m, &r)) {
        audio_play(JP_SFX_ERROR, 200, 128);
        return;
    }
    clear_selection();
    cpu_preview_mask = 0;
    describe_move(&r, text, sizeof(text));
    log_action(text);

    switch(r.move.type) {
    case MOVE_TAKE_CAMELS:
        audio_play(JP_SFX_CAMEL, 170, 128);
        break;
    case MOVE_SELL:
        score_anchor(p, &sx, &sy);
        bazaar_pos(r.move.sell_good, &bx, &by);
        for(i = 0; i < r.tokens_taken; ++i)
            spawn_flight(JP_ATLAS_TOKEN0 + r.move.sell_good, r.token_values[i], bx, by,
                         sx, sy, 0.25f + i * 0.16f);
        if(r.bonus_value) {
            bazaar_pos(6 + r.bonus_size - 3, &bx, &by);
            spawn_flight(JP_ATLAS_BONUS3 + r.bonus_size - 3, r.bonus_value, bx, by, sx, sy,
                         0.3f + r.tokens_taken * 0.16f);
        }
        if(r.points > 0) {
            char pts[24];
            /* Bonus values are secret: others only see the token haul. */
            if(p == viewer && !hands_hidden())
                snprintf(pts, sizeof(pts), "+%d", r.points);
            else
                snprintf(pts, sizeof(pts), "+%d", r.points - r.bonus_value);
            spawn_floater(sx, sy - 40.0f, 0.35f + r.tokens_taken * 0.16f, 0.9f,
                          0xffffe060, pts);
        }
        if(r.bonus_value)
            show_banner(r.bonus_size >= 5 ? "MEGA SALE!" : "BIG SALE!", text, 0xffffd040, 1.6f);
        break;
    default:
        audio_play(JP_SFX_CONFIRM, 150, 128);
        break;
    }
    set_state(ST_ANIMATE);
}

/* ------------------------------------------------------------------ */
/* Round / game flow                                                    */

static void start_turn(void);

static void deal_round(void) {
    int i;
    collecting = false;
    clear_selection();
    stagger_step = 0.045f;
    for(i = 0; i < CARD_COUNT; ++i) {
        cv[i].zone = ZONE_DECK;
        cv[i].owner = -1;
    }
    /* First human in turn order sits at the bottom when nobody else does. */
    if(!is_human(viewer) || human_count() < 2) {
        int p;
        for(p = 0; p < game.num_players; ++p) {
            int q = (game.current + p) % game.num_players;
            if(is_human(q)) {
                viewer = q;
                break;
            }
        }
    }
    audio_play(JP_SFX_SHUFFLE, 200, 128);
    set_state(ST_DEAL);
    {
        char sub[48];
        snprintf(sub, sizeof(sub), "%s starts", seat_name[game.current]);
        char title[32];
        snprintf(title, sizeof(title), "ROUND %d", game.round_number);
        show_banner(title, sub, 0xffffd040, 2.0f);
    }
    printf("BAZAAR: round %d begins, %s starts.\n", game.round_number,
           seat_name[game.current]);
}

static void new_game(void) {
    int p, h = 0, c = 0;
    uint32_t seed = (uint32_t)timer_us_gettime64() ^ 0x2026u;
    for(p = 0; p < setup_players; ++p) {
        if(seat_kind[p] == SEAT_HUMAN && !demo_mode)
            snprintf(seat_name[p], sizeof(seat_name[p]), "Player %d", (++h) & 7);
        else
            snprintf(seat_name[p], sizeof(seat_name[p]), "%s", cpu_names[c++]);
    }
    jp_new_game(&game, setup_players, seed, -1);
    memset(cv, 0, sizeof(cv));
    memset(action_log, 0, sizeof(action_log));
    viewer = 0;
    for(p = 0; p < setup_players; ++p)
        if(is_human(p)) {
            viewer = p;
            break;
        }
    for(p = 0; p < CARD_COUNT; ++p) {
        cv[p].x = cv[p].tx = DECK_X;
        cv[p].y = cv[p].ty = DECK_Y;
        cv[p].scale = cv[p].tscale = 1.0f;
        cv[p].flip = cv[p].tflip = 1.0f;
        cv[p].placed = true;
        cv[p].zone = ZONE_DECK;
        cv[p].owner = -1;
    }
    printf("BAZAAR: new game, %d players.\n", setup_players);
    deal_round();
}

static void start_turn(void) {
    int p = game.current;
    stagger_step = 0.05f;
    cpu_has_move = false;
    cpu_preview_mask = 0;
    clear_selection();
    cursor_row = 0;
    cursor_col = 0;
    hint_error[0] = 0;
    if(is_human(p)) {
        if(human_count() >= 2) {
            set_state(ST_PASS);
            audio_play(JP_SFX_TURN, 150, 128);
            return;
        }
        viewer = p;
        set_state(ST_TURN_HUMAN);
        audio_play(JP_SFX_TURN, 120, 128);
        show_banner("YOUR TURN", NULL, seat_colors[p], 1.0f);
    }
    else {
        set_state(ST_TURN_CPU);
    }
}

static void begin_round_end(void) {
    set_state(ST_ROUND_END);
    audio_play(JP_SFX_FANFARE, 220, 128);
    {
        int p;
        for(p = 0; p < game.num_players; ++p)
            printf("BAZAAR: round %d %s scored %d (seals %d)%s\n", game.round_number,
                   seat_name[p], game.players[p].round_score, game.players[p].seals,
                   (game.round_winners >> p) & 1 ? " WINS SEAL" : "");
    }
}

/* ------------------------------------------------------------------ */
/* Drawing: table HUD                                                   */

static void draw_button(int which, float x, float y, float size) {
    /* which: 0 A, 1 B, 2 X, 3 Y, 4 START */
    if(which == 4)
        gfx_atlas_rect(192, 192, 64, 32, x - size, y - size * 0.5f, size * 2, size, 0xffffffff);
    else
        gfx_atlas_rect(128 + (which % 2) * 32, 192 + (which / 2) * 32, 32, 32,
                       x - size * 0.5f, y - size * 0.5f, size, size, 0xffffffff);
}

static float draw_prompt(float x, float y, int button, const char *label, uint32_t color) {
    float w;
    draw_button(button, x + (button == 4 ? 22.0f : 11.0f), y + 11.0f, 22.0f);
    x += button == 4 ? 48.0f : 26.0f;
    w = gfx_text(FONT_SMALL, x, y - 1.0f, 0.8f, color, ALIGN_LEFT, label);
    return x + w + 14.0f;
}

static void draw_seals(float x, float y, int seals, float size) {
    int i;
    for(i = 0; i < SEALS_TO_WIN || i < seals; ++i)
        gfx_cell(JP_ATLAS_SEAL, x + i * (size + 2.0f), y, size, 0.0f,
                 i < seals ? 0xffffffff : 0x40ffffff);
}

static void draw_opponent_panels(void) {
    int p;
    for(p = 0; p < game.num_players; ++p) {
        const player_t *pl;
        float px, glow;
        bool active;
        if(p == viewer)
            continue;
        pl = &game.players[p];
        px = panel_x(opponent_slot(p));
        active = game.current == p && state != ST_ROUND_END && state != ST_GAME_OVER;
        glow = active ? 0.6f + 0.4f * sinf(time_now * 5.0f) : 0.0f;
        if(active) {
            gfx_set_additive(true);
            gfx_panel(px - 6, PANEL_Y - 6, PANEL_W + 12, PANEL_H + 12, 18,
                      color_alpha(seat_colors[p], glow));
            gfx_set_additive(false);
        }
        gfx_panel(px, PANEL_Y, PANEL_W, PANEL_H, 14, 0xe0201030);
        gfx_panel(px + 3, PANEL_Y + 3, PANEL_W - 6, 22, 9, color_alpha(seat_colors[p], 0.85f));
        gfx_text(FONT_SMALL, px + 12, PANEL_Y + 2, 0.8f, 0xffffffff, ALIGN_LEFT, seat_name[p]);
        if(seat_kind[p] != SEAT_HUMAN || demo_mode)
            gfx_text(FONT_SMALL, px + 16 + gfx_text_width(FONT_SMALL, 0.8f, seat_name[p]) + 6,
                     PANEL_Y + 7, 0.5f, 0xd0ffffff, ALIGN_LEFT,
                     seat_kind[p] == SEAT_CPU_EASY ? "CPU EASY" : "CPU HARD");
        draw_seals(px + PANEL_W - 38, PANEL_Y + 14, pl->seals, 14.0f);
        if(pl->hand_count == 0)
            gfx_text(FONT_SMALL, px + 16, PANEL_Y + 40, 0.6f, 0xa0ffffff, ALIGN_LEFT, "no cards");
        gfx_atlas_rect(192, 224, 32, 32, px + 116, PANEL_Y + 28, 28, 28, 0xffffffff);
        /* Tokens: coin count and bonus count, values stay secret. */
        gfx_atlas_rect(224, 224, 32, 32, px + 152, PANEL_Y + 30, 18, 18, 0xffffffff);
        gfx_textf(FONT_SMALL, px + 174, PANEL_Y + 28, 0.7f, 0xffffffff, ALIGN_LEFT, "%d",
                  pl->goods_token_count);
        gfx_cell(JP_ATLAS_BONUS3, px + 161, PANEL_Y + 60, 20, 0.0f, 0xffffffff);
        gfx_textf(FONT_SMALL, px + 174, PANEL_Y + 49, 0.7f, 0xffffffff, ALIGN_LEFT, "%d",
                  pl->bonus_token_count);
    }
}

/* Counters that must sit on top of the cards. */
static void draw_counts_over_cards(void) {
    int p;
    gfx_panel(DECK_X + 14, DECK_Y - 62, 34, 22, 10, 0xf0e8306a);
    gfx_textf(FONT_SMALL, DECK_X + 31, DECK_Y - 63, 0.72f, 0xffffffff, ALIGN_CENTER, "%d",
              game.deck_count);
    for(p = 0; p < game.num_players; ++p) {
        const player_t *pl = &game.players[p];
        float px;
        if(p == viewer)
            continue;
        px = panel_x(opponent_slot(p));
        if(pl->hand_count)
            gfx_textf(FONT_SMALL, px + 22.0f + (pl->hand_count - 1) * 11.0f + 18.0f,
                      PANEL_Y + 44, 0.65f, 0xffffffff, ALIGN_LEFT, "%d", pl->hand_count);
        gfx_textf(FONT_SMALL, px + 130, PANEL_Y + 50, 0.65f, 0xffffffff, ALIGN_CENTER, "x%d",
                  pl->herd_count);
    }
}


static void draw_bazaar(void) {
    int item;
    gfx_panel(550, 100, 86, 196, 14, 0xc0200828);
    gfx_text(FONT_SMALL, 593, 101, 0.62f, 0xffffd070, ALIGN_CENTER, "BAZAAR");
    for(item = 0; item < 10; ++item) {
        float x, y;
        int count, value, cell, k;
        bazaar_pos(item, &x, &y);
        y += 8.0f;
        if(item < 6) {
            count = game.goods_token_count[item];
            value = jp_token_peek(&game, item, 0);
            cell = JP_ATLAS_TOKEN0 + item;
        }
        else if(item < 9) {
            count = game.bonus_token_count[item - 6];
            value = 0;
            cell = JP_ATLAS_BONUS3 + item - 6;
        }
        else {
            count = game.round_over ? 0 : 1;
            value = CAMEL_BONUS;
            cell = JP_ATLAS_CAMEL_TOKEN;
        }
        if(count == 0) {
            gfx_cell(cell, x, y, 30, 0.0f, 0x50ffffff);
            continue;
        }
        for(k = count > 4 ? 3 : count - 1; k > 0; --k)
            gfx_cell(cell, x, y + k * 2.0f, 32, 0.0f, 0xff806040);
        gfx_cell(cell, x, y, 32, 0.0f, 0xffffffff);
        if(item < 6 || item == 9)
            draw_coin_value(x, y, 32, value);
        if(item < 9)
            gfx_textf(FONT_SMALL, x + 15, y + 4, 0.55f, 0xffffe0a0, ALIGN_LEFT, "%d", count);
    }
}

static void draw_market_area(void) {
    gfx_panel(126, 104, 416, 126, 16, 0x60100418);
    gfx_text(FONT_SMALL, 334, 100, 0.62f, 0xffffd070, ALIGN_CENTER, "MARKET");
    if(game.discard_count == 0) {
        gfx_panel(DISCARD_X - 26, DISCARD_Y - 36, 52, 72, 10, 0x40000000);
        gfx_text(FONT_SMALL, DISCARD_X, DISCARD_Y - 8, 0.5f, 0x90ffffff, ALIGN_CENTER, "DISCARD");
    }
}

static void draw_hand_area(void) {
    const player_t *pl = &game.players[viewer];
    bool hidden = hands_hidden();
    gfx_panel(106, 296, 524, 120, 16, 0x60100418);
    if(pl->herd_count == 0)
        gfx_panel(HERD_X - 30, HERD_Y - 44, 60, 88, 10, 0x40000000);
    gfx_textf(FONT_SMALL, HERD_X, HERD_Y + 50, 0.62f, 0xffffffff, ALIGN_CENTER,
              herd_sel ? "HERD %d/%d" : "HERD %d", herd_sel ? herd_sel : pl->herd_count,
              pl->herd_count);
    (void)hidden;
}

static void draw_bottom_bar(void) {
    const player_t *pl = &game.players[viewer];
    float x;
    gfx_rect_grad(0, 424, SCREEN_W, 56, 0xc0200828, 0xf0100410);
    gfx_rect(0, 424, SCREEN_W, 2, color_alpha(seat_colors[viewer], 0.9f));
    gfx_panel(16, 432, 118, 26, 10, color_alpha(seat_colors[viewer], 0.9f));
    gfx_text(FONT_SMALL, 75, 433, 0.8f, 0xffffffff, ALIGN_CENTER, seat_name[viewer]);
    draw_seals(28, 466, pl->seals, 14.0f);
    if(!hands_hidden()) {
        gfx_atlas_rect(224, 224, 32, 32, 140, 432, 22, 22, 0xffffffff);
        gfx_textf(FONT_SMALL, 166, 430, 0.9f, 0xffffe060, ALIGN_LEFT, "%d",
                  jp_player_score(&game, viewer));
        gfx_textf(FONT_SMALL, 140, 456, 0.55f, 0xc0ffffff, ALIGN_LEFT, "%d tokens, %d bonus",
                  pl->goods_token_count, pl->bonus_token_count);
    }
    x = 252.0f;
    if(state == ST_TURN_HUMAN && !paused) {
        x = draw_prompt(x, 434, 0, "Pick", 0xffffffff);
        x = draw_prompt(x, 434, 2, "Trade", 0xffffffff);
        x = draw_prompt(x, 434, 1, "Clear", 0xffffffff);
        x = draw_prompt(x, 434, 3, "Hint", 0xffffffff);
        gfx_text(FONT_SMALL, 252, 456, 0.55f, 0xb0ffffff, ALIGN_LEFT,
                 "R trigger: pick all of a type   START: menu");
    }
    else if(state == ST_TURN_CPU || state == ST_ANIMATE) {
        gfx_text(FONT_SMALL, 252, 440, 0.7f, 0xc0ffffff, ALIGN_LEFT, action_log[0]);
    }
}

static void draw_hint_bar(void) {
    move_t m;
    char desc[100];
    build_t b;
    uint32_t col;
    float w;
    if(state != ST_TURN_HUMAN || paused)
        return;
    b = build_move(&m, desc, sizeof(desc));
    if(hint_flash > 0.0f && hint_error[0]) {
        snprintf(desc, sizeof(desc), "%s", hint_error);
        col = 0xffff6060;
    }
    else if(b == BUILD_VALID)
        col = 0xff80ff80;
    else if(b == BUILD_INVALID)
        col = 0xffffa040;
    else
        col = 0xffffffff;
    w = gfx_text_width(FONT_SMALL, 0.85f, desc) + (b == BUILD_VALID ? 34.0f : 0.0f);
    gfx_panel(334 - w * 0.5f - 16, 236, w + 32, 30, 12, 0xd0200828);
    if(b == BUILD_VALID && !(hint_flash > 0.0f && hint_error[0])) {
        float pulse = 1.0f + 0.08f * sinf(time_now * 8.0f);
        draw_button(2, 334 - w * 0.5f + 10, 251, 22 * pulse);
        gfx_text(FONT_SMALL, 334 - w * 0.5f + 28, 238, 0.85f, col, ALIGN_LEFT, desc);
    }
    else {
        gfx_text(FONT_SMALL, 334, 238, 0.85f, col, ALIGN_CENTER, desc);
    }
}

static void cursor_target(float *x, float *y, float *w, float *h) {
    if(cursor_row == 0) {
        int id = game.market[cursor_col];
        *x = MARKET_X0 + cursor_col * MARKET_DX;
        *y = id != NO_CARD ? cv[id].y : MARKET_Y;
        *w = CW;
        *h = CH;
    }
    else if(cursor_col == 0) {
        const player_t *pl = &game.players[viewer];
        *x = HERD_X;
        *y = pl->herd_count ? cv[pl->herd[pl->herd_count - 1]].y : HERD_Y;
        *w = CW;
        *h = CH;
    }
    else {
        int id = sorted_hand[cursor_col - 1];
        *x = cv[id].x;
        *y = cv[id].y;
        *w = CW;
        *h = CH;
    }
}

static float cursor_x, cursor_y;

static void draw_cursor(float dt) {
    float x, y, w, h, bob, pulse;
    if(state != ST_TURN_HUMAN || paused)
        return;
    cursor_target(&x, &y, &w, &h);
    cursor_x = approach(cursor_x, x, 18.0f, dt);
    cursor_y = approach(cursor_y, y, 18.0f, dt);
    pulse = 0.6f + 0.4f * sinf(time_now * 6.0f);
    gfx_set_additive(true);
    gfx_panel(cursor_x - w * 0.5f - 5, cursor_y - h * 0.5f - 5, w + 10, h + 10, 12,
              color_alpha(0xff40e0ff, pulse * 0.8f));
    gfx_set_additive(false);
    bob = sinf(time_now * 6.0f) * 4.0f;
    {
        float ax = cursor_x, ay = cursor_y - h * 0.5f - 14.0f + bob;
        float xy[8] = { ax - 10, ay - 10, ax + 10, ay - 10, ax, ay + 4, ax, ay + 4 };
        uint32_t c[4] = { 0xff40e0ff, 0xff40e0ff, 0xffffffff, 0xffffffff };
        float xs[8] = { ax - 13, ay - 12, ax + 13, ay - 12, ax, ay + 7, ax, ay + 7 };
        uint32_t k[4] = { 0xff102040, 0xff102040, 0xff102040, 0xff102040 };
        gfx_quad(-1, xs, NULL, k);
        gfx_quad(-1, xy, NULL, c);
    }
    /* Name the hovered card type. */
    {
        int type = -1;
        if(cursor_row == 0 && game.market[cursor_col] != NO_CARD)
            type = game.card_type[game.market[cursor_col]];
        else if(cursor_row == 1 && cursor_col > 0)
            type = game.card_type[sorted_hand[cursor_col - 1]];
        else if(cursor_row == 1)
            type = GOOD_CAMEL;
        if(type >= 0) {
            const char *label = cursor_row == 1 && cursor_col == 0 ? "Herd: A adds a camel" :
                                jp_good_singular[type];
            gfx_text(FONT_SMALL, cursor_x, cursor_y + h * 0.5f + (cursor_row ? -2.0f : 4.0f),
                     0.6f, 0xffffffff, ALIGN_CENTER, cursor_row == 0 ? label : "");
            if(cursor_row == 1 && cursor_col == 0)
                gfx_text(FONT_SMALL, cursor_x, cursor_y - h * 0.5f - 44, 0.55f, 0xffffffff,
                         ALIGN_CENTER, label);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Overlays                                                             */

static void dim(float a) {
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, color_alpha(0xff080010, a));
}

static void draw_pass_screen(void) {
    int p = game.current;
    float t = ease_out_back(clampf(state_time * 2.5f, 0.0f, 1.0f));
    int i;
    dim(0.7f);
    gfx_panel(120, 120 + (1.0f - t) * 60.0f, 400, 230, 22, 0xf0281038);
    gfx_panel(120, 120 + (1.0f - t) * 60.0f, 400, 58, 22, color_alpha(seat_colors[p], 0.95f));
    gfx_text(FONT_SMALL, 320, 128 + (1.0f - t) * 60.0f, 0.8f, 0xffffffff, ALIGN_CENTER,
             "PASS THE CONTROLLER TO");
    gfx_text(FONT_BIG, 320, 186 + (1.0f - t) * 60.0f, 1.1f, 0xffffffff, ALIGN_CENTER,
             seat_name[p]);
    for(i = 0; i < 3; ++i)
        if(action_log[i][0])
            gfx_text(FONT_SMALL, 320, 246 + i * 20.0f + (1.0f - t) * 60.0f, 0.65f,
                     i == 0 ? 0xffffe080 : 0xa0ffffff, ALIGN_CENTER, action_log[i]);
    if(fmodf(time_now, 1.0f) < 0.7f) {
        draw_button(0, 238, 322, 26);
        gfx_text(FONT_SMALL, 256, 310, 0.85f, 0xffffffff, ALIGN_LEFT, "when ready - no peeking!");
    }
}

static const char *const pause_items[] = { "Resume", "How to Play", "Quit to Title" };

static void draw_pause(void) {
    int i;
    dim(0.65f);
    gfx_panel(190, 140, 260, 200, 20, 0xf0281038);
    gfx_text(FONT_BIG, 320, 150, 0.9f, 0xffffd040, ALIGN_CENTER, "PAUSED");
    for(i = 0; i < 3; ++i) {
        bool on = pause_cursor == i;
        if(on)
            gfx_panel(210, 210 + i * 40.0f, 220, 34, 12, 0xff8030c0);
        gfx_text(FONT_SMALL, 320, 213 + i * 40.0f, 1.0f, on ? 0xffffffff : 0xb0ffffff,
                 ALIGN_CENTER, pause_items[i]);
    }
}

static void draw_round_end(void) {
    int p, rows = game.num_players;
    float t = state_time;
    float y0 = 112.0f;
    dim(0.72f);
    gfx_panel(56, 58, 528, 372, 22, 0xf0281038);
    gfx_textf(FONT_BIG, 320, 66, 0.9f, 0xffffd040, ALIGN_CENTER, "ROUND %d RESULTS",
              game.round_number);
    gfx_text(FONT_SMALL, 230, y0 - 4, 0.6f, 0xc0ffffff, ALIGN_CENTER, "GOODS");
    gfx_text(FONT_SMALL, 300, y0 - 4, 0.6f, 0xc0ffffff, ALIGN_CENTER, "BONUS");
    gfx_text(FONT_SMALL, 370, y0 - 4, 0.6f, 0xc0ffffff, ALIGN_CENTER, "CAMELS");
    gfx_text(FONT_SMALL, 450, y0 - 4, 0.6f, 0xc0ffffff, ALIGN_CENTER, "TOTAL");
    gfx_text(FONT_SMALL, 530, y0 - 4, 0.6f, 0xc0ffffff, ALIGN_CENTER, "SEALS");
    for(p = 0; p < rows; ++p) {
        const player_t *pl = &game.players[p];
        float y = y0 + 22.0f + p * 56.0f;
        float rt = clampf((t - 0.3f - p * 0.35f) * 1.5f, 0.0f, 1.0f);
        int goods = 0, bonus = 0, i;
        bool won = (game.round_winners >> p) & 1;
        for(i = 0; i < pl->goods_token_count; ++i)
            goods += pl->goods_tokens[i];
        for(i = 0; i < pl->bonus_token_count; ++i)
            bonus += pl->bonus_tokens[i];
        if(won && t > 2.2f) {
            gfx_set_additive(true);
            gfx_panel(68, y - 4, 504, 50, 14,
                      color_alpha(0xffffd040, 0.5f + 0.3f * sinf(time_now * 6.0f)));
            gfx_set_additive(false);
        }
        gfx_panel(72, y, 496, 42, 12, 0xff3a1850);
        gfx_panel(76, y + 4, 110, 34, 10, color_alpha(seat_colors[p], 0.9f));
        gfx_text(FONT_SMALL, 131, y + 7, 0.8f, 0xffffffff, ALIGN_CENTER, seat_name[p]);
        gfx_textf(FONT_SMALL, 230, y + 6, 1.0f, 0xffffffff, ALIGN_CENTER, "%d",
                  (int)(goods * rt));
        gfx_textf(FONT_SMALL, 300, y + 6, 1.0f, 0xffffffff, ALIGN_CENTER, "%d",
                  (int)(bonus * rt));
        gfx_textf(FONT_SMALL, 370, y + 6, 1.0f, pl->camel_token ? 0xff80ffff : 0x80ffffff,
                  ALIGN_CENTER, pl->camel_token ? "+5" : "-");
        gfx_textf(FONT_BIG, 450, y - 3, 0.75f, 0xffffe060, ALIGN_CENTER, "%d",
                  (int)(pl->round_score * rt));
        draw_seals(512, y + 21, pl->seals - (won && t < 2.2f ? 1 : 0), 18.0f);
    }
    {
        float y = y0 + 30.0f + rows * 56.0f;
        if(game.camel_token_winner >= 0)
            gfx_textf(FONT_SMALL, 320, y, 0.7f, 0xff80ffff, ALIGN_CENTER,
                      "Camel token: %s has the biggest herd (%d)",
                      seat_name[game.camel_token_winner],
                      game.players[game.camel_token_winner].herd_count);
        else
            gfx_text(FONT_SMALL, 320, y, 0.7f, 0xa0ffffff, ALIGN_CENTER,
                     "Camel token: herds tied - nobody scores it");
        if(t > 2.4f) {
            int winners = 0;
            for(p = 0; p < rows; ++p)
                winners += (game.round_winners >> p) & 1;
            for(p = 0; p < rows; ++p)
                if((game.round_winners >> p) & 1) {
                    gfx_textf(FONT_SMALL, 320, y + 26, 0.85f, 0xffffd040, ALIGN_CENTER,
                              winners > 1 ? "Tie! Each leader earns a Seal of Excellence" :
                              "%s earns a Seal of Excellence!", seat_name[p]);
                    break;
                }
            if(fmodf(time_now, 1.0f) < 0.75f) {
                draw_button(0, 262, 398, 24);
                gfx_text(FONT_SMALL, 280, 386, 0.8f, 0xffffffff, ALIGN_LEFT,
                         game.game_over ? "Final results" : "Next round");
            }
        }
    }
}

static void draw_game_over(void) {
    int p = game.game_winner;
    float t = ease_out_back(clampf(state_time * 1.5f, 0.0f, 1.0f));
    dim(0.55f);
    gfx_set_additive(true);
    gfx_cell(JP_ATLAS_GLOW, 320, 200, 520 * t, 0, color_alpha(seat_colors[p], 0.6f));
    gfx_set_additive(false);
    gfx_cell(JP_ATLAS_SEAL, 320, 150, 120 * t, sinf(time_now * 2.0f) * 0.15f, 0xffffffff);
    gfx_text(FONT_BIG, 320, 226, 1.0f * t, 0xffffd040, ALIGN_CENTER, "MASTER MERCHANT");
    gfx_text(FONT_BIG, 320, 284, 1.0f * t, seat_colors[p] | 0xff000000, ALIGN_CENTER,
             seat_name[p]);
    gfx_textf(FONT_SMALL, 320, 338, 0.85f, 0xffffffff, ALIGN_CENTER,
              "wins with %d Seals of Excellence in %d rounds", game.players[p].seals,
              game.round_number);
    if(state_time > 2.0f && fmodf(time_now, 1.0f) < 0.75f) {
        draw_button(4, 266, 398, 22);
        gfx_text(FONT_SMALL, 296, 386, 0.8f, 0xffffffff, ALIGN_LEFT, "Back to title");
    }
}

/* ------------------------------------------------------------------ */
/* Title, setup and rules screens                                       */

static int title_cursor;
static const char *const title_items[] = { "Start Game", "How to Play" };

static void draw_title_cards(void) {
    int pass, i;
    /* A carousel of spinning goods cards; back half first. */
    for(pass = 0; pass < 2; ++pass) {
        for(i = 0; i < 7; ++i) {
            float a = time_now * 0.45f + i * (2.0f * PI / 7.0f);
            float x = 320.0f + sinf(a) * 236.0f;
            float y = 356.0f + cosf(a) * 34.0f;
            float depth = (cosf(a) + 1.0f) * 0.5f;
            float spin = time_now * 0.35f + i * 0.29f;
            uint32_t tint = color_lerp(0xff7060a0, 0xffffffff, depth);
            if((depth >= 0.5f) != (pass == 1))
                continue;
            draw_card_at(i, x, y, 0.62f + depth * 0.45f, sinf(a) * 0.12f, spin, tint,
                         pass == 1);
        }
    }
}

static void draw_logo(float y) {
    float bob = sinf(time_now * 1.6f) * 5.0f;
    float scale = 1.0f + sinf(time_now * 2.2f) * 0.015f;
    float w = 400.0f * scale, h = 200.0f * scale;
    gfx_sprite(JP_TEXID_LOGO, 320 - w * 0.5f + 6, y + bob - h * 0.5f + 8, w, h, 0, 0, 1, 1,
               0x70000000);
    gfx_sprite(JP_TEXID_LOGO, 320 - w * 0.5f, y + bob - h * 0.5f, w, h, 0, 0, 1, 1,
               0xffffffff);
    {
        /* A glint sweeping across the logo. */
        float gx = fmodf(time_now * 260.0f, 1400.0f) - 300.0f;
        if(gx < 640.0f) {
            gfx_set_additive(true);
            gfx_cell(JP_ATLAS_STAR, gx, y + bob - 40, 70, time_now, 0xc0ffffff);
            gfx_set_additive(false);
        }
    }
}

static void draw_title(void) {
    int i;
    draw_title_cards();
    draw_logo(102.0f);
    if(state_time < 0.3f)
        dim(1.0f - state_time / 0.3f);
    gfx_text(FONT_SMALL, 320, 204, 0.75f, 0xffffe0b0, ALIGN_CENTER,
             "The merchant card game of Jaipur  -  2 to 4 players");
    for(i = 0; i < 2; ++i) {
        bool on = title_cursor == i;
        float y = 234.0f + i * 40.0f;
        if(on) {
            float p = 0.8f + 0.2f * sinf(time_now * 6.0f);
            gfx_panel(220, y, 200, 34, 14, color_alpha(0xffe8306a, p));
        }
        else
            gfx_panel(220, y, 200, 34, 14, 0x80200828);
        gfx_text(FONT_SMALL, 320, y + 3, 1.0f, on ? 0xffffffff : 0xc0ffffff, ALIGN_CENTER,
                 title_items[i]);
    }
    gfx_text(FONT_SMALL, 320, 456, 0.55f, 0xa0ffffff, ALIGN_CENTER,
             "Totally Bazaar - Dreamcast homebrew  -  (A) / START to choose");
}

static int setup_cursor;

static void draw_setup(void) {
    int i, rows = 1 + setup_players + 1;
    dim(0.45f);
    gfx_panel(110, 70, 420, 360, 22, 0xf0281038);
    gfx_text(FONT_BIG, 320, 78, 0.9f, 0xffffd040, ALIGN_CENTER, "NEW GAME");
    for(i = 0; i < rows; ++i) {
        float y = 140.0f + i * 44.0f;
        bool on = setup_cursor == i;
        char label[40];
        uint32_t base = on ? 0xff8030c0 : 0xff3a1850;
        if(i == rows - 1) {
            y += 12.0f;
            gfx_panel(230, y, 180, 38, 14, on ? color_alpha(0xffe8306a, 0.8f + 0.2f *
                                                            sinf(time_now * 6.0f))
                                                : 0xff3a1850);
            gfx_text(FONT_SMALL, 320, y + 5, 1.0f, 0xffffffff, ALIGN_CENTER, "Start!");
            continue;
        }
        gfx_panel(140, y, 360, 36, 12, base);
        if(i == 0) {
            gfx_text(FONT_SMALL, 160, y + 4, 0.9f, 0xffffffff, ALIGN_LEFT, "Players");
            snprintf(label, sizeof(label), "%d", setup_players);
        }
        else {
            int p = i - 1;
            gfx_panel(146, y + 6, 24, 24, 8, seat_colors[p]);
            gfx_textf(FONT_SMALL, 180, y + 4, 0.9f, 0xffffffff, ALIGN_LEFT, "Seat %d", p + 1);
            snprintf(label, sizeof(label), "%s", seat_kind_names[seat_kind[p]]);
        }
        gfx_text(FONT_SMALL, 410, y + 4, 0.9f, on ? 0xffffffff : 0xffffe0a0, ALIGN_CENTER, label);
        if(on) {
            gfx_text(FONT_SMALL, 330, y + 4, 0.9f, 0xffffffff, ALIGN_CENTER, "<");
            gfx_text(FONT_SMALL, 490, y + 4, 0.9f, 0xffffffff, ALIGN_CENTER, ">");
        }
    }
    gfx_text(FONT_SMALL, 320, 396, 0.6f, 0xc0ffffff, ALIGN_CENTER,
             "Left/Right change   (A) start   (B) back");
    gfx_text(FONT_SMALL, 320, 412, 0.55f, 0x90ffffff, ALIGN_CENTER,
             "Humans share the screen; hands hide between turns.");
}

static int rules_page;
#define RULES_PAGES 5
static const char *const rules_text[RULES_PAGES][11] = {
    { "THE GOAL",
      "You are a merchant of Jaipur. Trade goods in the market",
      "and sell them for rupee tokens. Win two rounds to earn two",
      "Seals of Excellence and become the Maharaja's merchant.",
      "",
      "Cards: Diamonds, Gold, Silver (precious), Cloth, Spice,",
      "Leather and Camels. Camels live in your herd, not your hand.",
      "",
      "Each turn you must either TAKE cards or SELL cards.",
      "Hand limit: 7 goods cards (camels don't count).",
      "" },
    { "TAKE CARDS (pick one)",
      "TAKE ONE: one goods card from the market. The deck refills",
      "  the empty slot.",
      "TAKE CAMELS: all camels in the market at once.",
      "EXCHANGE: take 2+ goods from the market and give back the",
      "  same number of cards from your hand and/or herd.",
      "  You can't take and give the same kind of goods.",
      "",
      "Tip: camels are great exchange currency, but taking them",
      "reveals new cards for your rivals.",
      "" },
    { "SELL CARDS",
      "Discard any number of ONE type of goods from your hand and",
      "take that many tokens from its stack, highest first.",
      "Diamonds, Gold and Silver must be sold 2 or more at a time.",
      "",
      "Sell 3 cards: bonus token worth 1-3 rupees.",
      "Sell 4 cards: bonus token worth 4-6.",
      "Sell 5+:      bonus token worth 8-10.",
      "Bonus values stay secret until the round ends.",
      "",
      "" },
    { "END OF ROUND",
      "A round ends at once when three goods token stacks are",
      "empty, or when the deck runs out and the market can't refill.",
      "",
      "The biggest herd earns the camel token (+5). Ties: nobody.",
      "Most rupees wins a Seal of Excellence. Ties go to most",
      "bonus tokens, then most goods tokens.",
      "The round's lowest scorer starts the next one.",
      "First to 2 seals wins (3-4 players: same rules, more rivals!)",
      "" , "" },
    { "CONTROLS",
      "D-Pad / stick   move the cursor between market and hand",
      "(A)  pick a card (on your herd: add a camel)",
      "(X)  make the trade shown in the hint bar",
      "(B)  clear your picks       (Y)  ask for a hint",
      "R trigger  pick every card of the hovered type",
      "START  pause menu",
      "",
      "Hot-seat: pass the pad when asked; hands stay face down",
      "until the next merchant presses (A).",
      "" }
};

static void draw_rules(void) {
    int i;
    dim(0.6f);
    gfx_panel(40, 40, 560, 400, 22, 0xf0281038);
    gfx_text(FONT_BIG, 320, 50, 0.8f, 0xffffd040, ALIGN_CENTER, rules_text[rules_page][0]);
    for(i = 1; i < 11; ++i)
        if(rules_text[rules_page][i] && rules_text[rules_page][i][0])
            gfx_text(FONT_SMALL, 66, 98 + (i - 1) * 28.0f, 0.78f, 0xffffffff, ALIGN_LEFT,
                     rules_text[rules_page][i]);
    if(rules_page == 2) {
        int g;
        for(g = 0; g < 6; ++g)
            gfx_cell(JP_ATLAS_TOKEN0 + g, 360 + g * 38.0f, 365, 34, 0, 0xffffffff);
    }
    if(rules_page == 0) {
        int g;
        for(g = 0; g < 7; ++g)
            draw_card_at(g, 110 + g * 70.0f, 376, 0.5f, (g - 3) * 0.04f, 0.0f, 0xffffffff, true);
    }
    gfx_textf(FONT_SMALL, 320, 408, 0.65f, 0xc0ffffff, ALIGN_CENTER,
              "<  page %d of %d  >      (B) back", rules_page + 1, RULES_PAGES);
}

/* ------------------------------------------------------------------ */
/* Per-state update                                                     */

static void move_cursor_row(int row) {
    float x, y, w, h;
    int best = 0, i;
    float bestd = 1e9f;
    cursor_target(&x, &y, &w, &h);
    if(row == 0) {
        for(i = 0; i < MARKET_SIZE; ++i) {
            float d = fabsf(MARKET_X0 + i * MARKET_DX - x);
            if(d < bestd) {
                bestd = d;
                best = i;
            }
        }
    }
    else {
        const player_t *pl = &game.players[viewer];
        for(i = 0; i <= sorted_count; ++i) {
            float cx = i == 0 ? HERD_X : HAND_X0 + (i - 1) * HAND_DX;
            float d;
            if(i == 0 && pl->herd_count == 0)
                continue;
            d = fabsf(cx - x);
            if(d < bestd) {
                bestd = d;
                best = i;
            }
        }
        if(bestd > 1e8f)
            return;  /* nothing to point at in the hand row */
    }
    cursor_row = row;
    cursor_col = best;
}

static void flash_error(const char *msg) {
    snprintf(hint_error, sizeof(hint_error), "%s", msg);
    hint_flash = 1.6f;
    audio_play(JP_SFX_ERROR, 180, 128);
}

static void toggle_pick(void) {
    const player_t *pl = &game.players[viewer];
    int s, i;
    if(cursor_row == 0) {
        int id = game.market[cursor_col];
        if(id == NO_CARD)
            return;
        if(game.card_type[id] == GOOD_CAMEL) {
            bool on = !sel_card[id];
            clear_selection();
            for(s = 0; s < MARKET_SIZE; ++s)
                if(game.market[s] != NO_CARD && game.card_type[game.market[s]] == GOOD_CAMEL)
                    sel_card[game.market[s]] = on;
            audio_play(on ? JP_SFX_CAMEL : JP_SFX_DESELECT, on ? 120 : 160, 128);
            return;
        }
        /* Picking goods drops any camel pick. */
        for(s = 0; s < MARKET_SIZE; ++s)
            if(game.market[s] != NO_CARD && game.card_type[game.market[s]] == GOOD_CAMEL)
                sel_card[game.market[s]] = false;
        sel_card[id] = !sel_card[id];
        audio_play(sel_card[id] ? JP_SFX_SELECT : JP_SFX_DESELECT, 170, 128);
        return;
    }
    for(s = 0; s < MARKET_SIZE; ++s)
        if(game.market[s] != NO_CARD && game.card_type[game.market[s]] == GOOD_CAMEL)
            sel_card[game.market[s]] = false;
    if(cursor_col == 0) {
        if(pl->herd_count == 0)
            return;
        herd_sel = herd_sel >= pl->herd_count ? 0 : herd_sel + 1;
        audio_play(herd_sel ? JP_SFX_SELECT : JP_SFX_DESELECT, 170, 128);
        return;
    }
    i = sorted_hand[cursor_col - 1];
    sel_card[i] = !sel_card[i];
    audio_play(sel_card[i] ? JP_SFX_SELECT : JP_SFX_DESELECT, 170, 128);
}

static void pick_all_of_type(void) {
    const player_t *pl = &game.players[viewer];
    int type, i;
    bool any_off = false;
    if(cursor_row != 1 || cursor_col == 0) {
        if(cursor_row == 1 && pl->herd_count) {
            herd_sel = herd_sel == pl->herd_count ? 0 : pl->herd_count;
            audio_play(JP_SFX_SELECT, 170, 128);
        }
        return;
    }
    type = game.card_type[sorted_hand[cursor_col - 1]];
    for(i = 0; i < pl->hand_count; ++i)
        if(game.card_type[pl->hand[i]] == type && !sel_card[pl->hand[i]])
            any_off = true;
    for(i = 0; i < pl->hand_count; ++i)
        if(game.card_type[pl->hand[i]] == type)
            sel_card[pl->hand[i]] = any_off;
    audio_play(any_off ? JP_SFX_SELECT : JP_SFX_DESELECT, 170, 128);
}

static void update_human_turn(void) {
    const player_t *pl = &game.players[viewer];
    int row_len;

    sort_hand(viewer);
    if(cursor_row == 1) {
        if(cursor_col > sorted_count)
            cursor_col = sorted_count;
        if(cursor_col == 0 && pl->herd_count == 0 && sorted_count > 0)
            cursor_col = 1;
        if(sorted_count == 0 && pl->herd_count == 0)
            cursor_row = 0;
    }
    if(in_pressed & BTN_LEFT) {
        if(cursor_row == 0)
            cursor_col = (cursor_col + MARKET_SIZE - 1) % MARKET_SIZE;
        else {
            int lo = pl->herd_count ? 0 : 1;
            row_len = sorted_count + 1;
            cursor_col = cursor_col - 1 < lo ? row_len - 1 : cursor_col - 1;
        }
        audio_play(JP_SFX_MOVE, 120, 128);
    }
    if(in_pressed & BTN_RIGHT) {
        if(cursor_row == 0)
            cursor_col = (cursor_col + 1) % MARKET_SIZE;
        else {
            int lo = pl->herd_count ? 0 : 1;
            row_len = sorted_count + 1;
            cursor_col = cursor_col + 1 >= row_len ? lo : cursor_col + 1;
        }
        audio_play(JP_SFX_MOVE, 120, 128);
    }
    if((in_pressed & BTN_DOWN) && cursor_row == 0) {
        move_cursor_row(1);
        audio_play(JP_SFX_MOVE, 120, 128);
    }
    if((in_pressed & BTN_UP) && cursor_row == 1) {
        move_cursor_row(0);
        audio_play(JP_SFX_MOVE, 120, 128);
    }
    if(in_pressed & BTN_A)
        toggle_pick();
    if(in_pressed & BTN_R)
        pick_all_of_type();
    if(in_pressed & (BTN_B | BTN_L)) {
        clear_selection();
        audio_play(JP_SFX_DESELECT, 150, 128);
    }
    if(in_pressed & BTN_Y) {
        move_t m;
        if(ai_choose_move(&game, AI_HARD, &ai_rng, &m)) {
            select_move(&m);
            audio_play(JP_SFX_BONUS, 120, 128);
        }
    }
    if(in_pressed & BTN_X) {
        move_t m;
        char desc[80];
        build_t b = build_move(&m, desc, sizeof(desc));
        if(b == BUILD_VALID)
            perform_move(&m);
        else
            flash_error(b == BUILD_NOTHING ? "Pick some cards first" : desc);
    }
}


static void update_cpu_turn(float dt) {
    float think = demo_mode ? 0.35f : 0.8f;
    if(!cpu_has_move && state_time > think) {
        ai_level_t lvl = seat_kind[game.current] == SEAT_CPU_EASY ? AI_EASY : AI_HARD;
        cpu_has_move = ai_choose_move(&game, lvl, &ai_rng, &cpu_move);
        cpu_timer = 0.0f;
        if(cpu_has_move && cpu_move.type != MOVE_SELL) {
            int s;
            cpu_preview_mask = cpu_move.market_mask;
            if(cpu_move.type == MOVE_TAKE_CAMELS)
                for(s = 0; s < MARKET_SIZE; ++s)
                    if(game.market[s] != NO_CARD && game.card_type[game.market[s]] == GOOD_CAMEL)
                        cpu_preview_mask |= (uint8_t)(1u << s);
            audio_play(JP_SFX_SELECT, 130, 128);
        }
    }
    if(cpu_has_move) {
        cpu_timer += dt;
        if(cpu_timer > (demo_mode ? 0.25f : 0.6f)) {
            cpu_has_move = false;
            perform_move(&cpu_move);
        }
    }
}

static void finish_round_end(void) {
    if(game.game_over) {
        set_state(ST_GAME_OVER);
        audio_play(JP_SFX_VICTORY, 240, 128);
        confetti_burst(320, 200, 120);
        printf("BAZAAR: game over, %s wins with %d seals after %d rounds.\n",
               seat_name[game.game_winner], game.players[game.game_winner].seals,
               game.round_number);
    }
    else {
        collecting = true;
        jp_next_round(&game);
        set_state(ST_DEAL);
        state_time = -0.9f;   /* let the cards gather first */
        audio_play(JP_SFX_SHUFFLE, 200, 128);
    }
}

#ifdef BAZAAR_AUTOPLAY
static int demo_games;
#endif

static void update_game(float dt) {
    if(paused) {
        if(in_pressed & BTN_UP)
            pause_cursor = (pause_cursor + 2) % 3;
        if(in_pressed & BTN_DOWN)
            pause_cursor = (pause_cursor + 1) % 3;
        if(in_pressed & (BTN_UP | BTN_DOWN))
            audio_play(JP_SFX_MOVE, 120, 128);
        if(in_pressed & (BTN_START | BTN_B)) {
            paused = false;
            audio_play(JP_SFX_DESELECT, 150, 128);
        }
        else if(in_pressed & BTN_A) {
            audio_play(JP_SFX_CONFIRM, 160, 128);
            if(pause_cursor == 0)
                paused = false;
            else if(pause_cursor == 1) {
                rules_return = state;
                rules_page = 0;
                paused = false;
                set_state(ST_RULES);
                state_time = 0.0f;
            }
            else {
                paused = false;
                set_state(ST_TITLE);
                audio_music_target(200);
            }
        }
        return;
    }
    if((in_pressed & BTN_START) && state != ST_GAME_OVER && state != ST_ROUND_END &&
       !demo_mode) {
        paused = true;
        pause_cursor = 0;
        audio_play(JP_SFX_SELECT, 150, 128);
        return;
    }
    if(demo_mode && (in_pressed & (BTN_A | BTN_START | BTN_B))) {
#ifndef BAZAAR_AUTOPLAY
        demo_mode = false;
        setup_players = saved_players;
        set_state(ST_TITLE);
        return;
#endif
    }

    switch(state) {
    case ST_DEAL:
        if(collecting && state_time >= 0.0f) {
            /* Gathered: deal the new round. */
            deal_round();
        }
        if(!collecting && state_time > 0.6f && cards_moving() == 0)
            start_turn();
        break;
    case ST_PASS:
        if(in_pressed & BTN_A) {
            viewer = game.current;
            audio_play(JP_SFX_CONFIRM, 180, 128);
            set_state(ST_TURN_HUMAN);
            show_banner("YOUR TURN", seat_name[viewer], seat_colors[viewer], 1.0f);
        }
        break;
    case ST_TURN_HUMAN:
        update_human_turn();
        break;
    case ST_TURN_CPU:
        update_cpu_turn(dt);
        break;
    case ST_ANIMATE:
        if(state_time > 0.45f && cards_moving() == 0 && active_flights() == 0) {
            if(game.round_over)
                begin_round_end();
            else
                start_turn();
        }
        break;
    case ST_ROUND_END:
        if(state_time > 2.2f && state_time - dt <= 2.2f) {
            audio_play(JP_SFX_BONUS, 220, 128);
            sparkle_burst(512, 160, 30, 0xffffd040);
        }
        if((state_time > 2.6f && (in_pressed & (BTN_A | BTN_START))) ||
           (demo_mode && state_time > 4.0f))
            finish_round_end();
        break;
    case ST_GAME_OVER:
        if(fmodf(state_time, 0.9f) < dt)
            confetti_burst(80 + frand() * 480, 120 + frand() * 120, 30);
        if((state_time > 2.0f && (in_pressed & (BTN_A | BTN_START))) ||
           (demo_mode && state_time > 6.0f)) {
#ifdef BAZAAR_AUTOPLAY
            demo_games++;
            printf("BAZAAR AUTOPLAY: game %d complete.\n", demo_games);
            if(demo_games >= BAZAAR_AUTOPLAY_GAMES)
                printf("BAZAAR AUTOPLAY: ALL GAMES COMPLETE.\n");
            new_game();
#else
            set_state(ST_TITLE);
            demo_mode = false;
            setup_players = saved_players;
#endif
        }
        break;
    default:
        break;
    }
}

static void update_menus(void) {
    if(state == ST_TITLE) {
        if(in_pressed & (BTN_UP | BTN_DOWN)) {
            title_cursor ^= 1;
            audio_play(JP_SFX_MOVE, 120, 128);
            idle_time = 0.0f;
        }
        if(in_pressed & (BTN_A | BTN_START)) {
            audio_play(JP_SFX_CONFIRM, 180, 128);
            idle_time = 0.0f;
            if(title_cursor == 0) {
                set_state(ST_SETUP);
                setup_cursor = 1 + setup_players;
            }
            else {
                rules_return = ST_TITLE;
                rules_page = 0;
                set_state(ST_RULES);
            }
        }
        if(in_held)
            idle_time = 0.0f;
        if(idle_time > 30.0f) {
            /* Attract mode: four CPU merchants play while nobody's around. */
            idle_time = 0.0f;
            demo_mode = true;
            saved_players = setup_players;
            setup_players = 4;
            new_game();
        }
    }
    else if(state == ST_SETUP) {
        int rows = 1 + setup_players + 1;
        if(in_pressed & BTN_UP)
            setup_cursor = (setup_cursor + rows - 1) % rows;
        if(in_pressed & BTN_DOWN)
            setup_cursor = (setup_cursor + 1) % rows;
        if(in_pressed & (BTN_UP | BTN_DOWN))
            audio_play(JP_SFX_MOVE, 120, 128);
        if(in_pressed & (BTN_LEFT | BTN_RIGHT)) {
            int d = (in_pressed & BTN_RIGHT) ? 1 : -1;
            if(setup_cursor == 0) {
                setup_players += d;
                if(setup_players < MIN_PLAYERS)
                    setup_players = MAX_PLAYERS;
                if(setup_players > MAX_PLAYERS)
                    setup_players = MIN_PLAYERS;
            }
            else if(setup_cursor <= setup_players) {
                int p = setup_cursor - 1;
                seat_kind[p] = (seat_kind_t)((seat_kind[p] + SEAT_KINDS + d) % SEAT_KINDS);
            }
            audio_play(JP_SFX_SELECT, 140, 128);
        }
        if((in_pressed & (BTN_A | BTN_START)) && (setup_cursor == rows - 1 ||
                                                 (in_pressed & BTN_START))) {
            audio_play(JP_SFX_CONFIRM, 200, 128);
            demo_mode = false;
            new_game();
        }
        if(in_pressed & BTN_B) {
            audio_play(JP_SFX_DESELECT, 150, 128);
            set_state(ST_TITLE);
        }
    }
    else if(state == ST_RULES) {
        if(in_pressed & BTN_RIGHT)
            rules_page = (rules_page + 1) % RULES_PAGES;
        if(in_pressed & BTN_LEFT)
            rules_page = (rules_page + RULES_PAGES - 1) % RULES_PAGES;
        if(in_pressed & (BTN_LEFT | BTN_RIGHT))
            audio_play(JP_SFX_CARD, 160, 128);
        if(in_pressed & (BTN_B | BTN_START | BTN_A)) {
            if((in_pressed & BTN_A) && rules_page < RULES_PAGES - 1) {
                rules_page++;
                audio_play(JP_SFX_CARD, 160, 128);
            }
            else {
                audio_play(JP_SFX_DESELECT, 150, 128);
                set_state(rules_return);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* UI test driver: plays the human seats through the real controls.     */
#ifdef BAZAAR_UITEST
#ifndef BAZAAR_UITEST_GAMES
#define BAZAAR_UITEST_GAMES 2
#endif
#ifndef UT_SPEED
#define UT_SPEED 2.0f
#endif
#ifndef UT_STEP
#define UT_STEP 0.07f
#endif
#ifndef UT_HOLD
#define UT_HOLD 0.0f
#endif
static bool ut_planned;
static bool ut_target[CARD_COUNT];
static int ut_herd;
static move_t ut_move;
static float ut_wait;
static int ut_games, ut_moves, ut_hints, ut_pauses;
static bool ut_did_rules;

static void ut_fail(const char *why) {
    printf("BAZAAR UITEST: FAIL - %s (state %d, moves %d)\n", why, state, ut_moves);
}

static void ut_plan(void) {
    move_t m;
    const player_t *pl = &game.players[game.current];
    int s, i, t;
    ai_choose_move(&game, (ut_moves % 3) ? AI_EASY : AI_HARD, &ai_rng, &m);
    ut_move = m;
    memset(ut_target, 0, sizeof(ut_target));
    ut_herd = 0;
    if(m.type == MOVE_TAKE_CAMELS) {
        for(s = 0; s < MARKET_SIZE; ++s)
            if(game.market[s] != NO_CARD && game.card_type[game.market[s]] == GOOD_CAMEL)
                ut_target[game.market[s]] = true;
    }
    for(s = 0; s < MARKET_SIZE; ++s)
        if(m.type != MOVE_TAKE_CAMELS && (m.market_mask & (1u << s)))
            ut_target[game.market[s]] = true;
    sort_hand(game.current);
    for(t = 0; t < TRADE_GOODS; ++t) {
        int want = m.type == MOVE_SELL ? (t == m.sell_good ? m.sell_count : 0) :
                   m.type == MOVE_EXCHANGE ? m.give[t] : 0;
        for(i = 0; i < sorted_count && want; ++i)
            if(game.card_type[sorted_hand[i]] == t) {
                ut_target[sorted_hand[i]] = true;
                want--;
            }
    }
    if(m.type == MOVE_EXCHANGE)
        ut_herd = m.give_camels;
    (void)pl;
    ut_planned = true;
}

/* Returns the synthetic buttons to press this frame. */
static uint32_t ut_buttons(float dt) {
    int s, i;
    ut_wait -= dt;
    if(ut_wait > 0.0f)
        return 0;
    ut_wait = UT_STEP;
    if((state == ST_TITLE || state == ST_SETUP || state == ST_PASS || state == ST_RULES) &&
       state_time < UT_HOLD)
        return 0;
    switch(state) {
    case ST_TITLE:
        if(ut_games >= BAZAAR_UITEST_GAMES) {
            printf("BAZAAR UITEST: ALL PASS (%d games, %d human moves, %d hints, %d pauses).\n",
                   ut_games, ut_moves, ut_hints, ut_pauses);
            ut_wait = 1e9f;
            return 0;
        }
        if(!ut_did_rules) {
            ut_did_rules = true;
            title_cursor = 1;
            return BTN_A;           /* open How to Play */
        }
        title_cursor = 0;
        return BTN_A;
    case ST_RULES:
        if(rules_page < RULES_PAGES - 1)
            return BTN_RIGHT;
        return BTN_B;
    case ST_SETUP:
        /* Game 1: two humans and a CPU. Game 2: four seats, three humans. */
        if(setup_cursor != 0 && setup_players != (ut_games == 0 ? 3 : 4)) {
            setup_cursor = 0;
            return 0;
        }
        if(setup_players != (ut_games == 0 ? 3 : 4))
            return BTN_RIGHT;
        seat_kind[0] = SEAT_HUMAN;
        seat_kind[1] = SEAT_HUMAN;
        seat_kind[2] = ut_games == 0 ? SEAT_CPU_HARD : SEAT_HUMAN;
        seat_kind[3] = SEAT_CPU_EASY;
        return BTN_START;
    case ST_PASS:
        return state_time > 0.3f ? BTN_A : 0;
    case ST_TURN_HUMAN: {
        const player_t *pl = &game.players[viewer];
        if(paused) {
            ut_pauses++;
            return BTN_B;
        }
        if(viewer != game.current) {
            ut_fail("human turn shown for the wrong seat");
            return 0;
        }
        if(state_time < 0.3f)
            return 0;
        if(ut_moves % 7 == 3 && ut_pauses <= ut_moves / 7 && !ut_planned) {
            return BTN_START;
        }
        if(!ut_planned) {
            if(ut_moves % 5 == 1 && ut_hints <= ut_moves / 5) {
                ut_hints++;
                return BTN_Y;
            }
            clear_selection();
            ut_plan();
        }
        /* Market picks first. */
        for(s = 0; s < MARKET_SIZE; ++s) {
            int id = game.market[s];
            if(id == NO_CARD || sel_card[id] == ut_target[id])
                continue;
            if(cursor_row != 0)
                return BTN_UP;
            if(cursor_col != s)
                return ((s - cursor_col + MARKET_SIZE) % MARKET_SIZE) <= 2 ? BTN_RIGHT : BTN_LEFT;
            return BTN_A;
        }
        sort_hand(viewer);
        for(i = 0; i < sorted_count; ++i) {
            int id = sorted_hand[i];
            if(sel_card[id] == ut_target[id])
                continue;
            if(cursor_row != 1)
                return BTN_DOWN;
            if(cursor_col != i + 1)
                return cursor_col < i + 1 ? BTN_RIGHT : BTN_LEFT;
            return BTN_A;
        }
        if(herd_sel != ut_herd) {
            if(cursor_row != 1)
                return BTN_DOWN;
            if(cursor_col != 0)
                return BTN_LEFT;
            return BTN_A;
        }
        {
            move_t m;
            char desc[100];
            if(build_move(&m, desc, sizeof(desc)) != BUILD_VALID) {
                printf("BAZAAR UITEST: plan type %d gave '%s'\n", ut_move.type, desc);
                ut_fail("selection did not build the planned move");
                ut_wait = 1e9f;
                return 0;
            }
            if(m.type != ut_move.type && !(ut_move.type == MOVE_TAKE_ONE && m.type == MOVE_TAKE_ONE)) {
                ut_fail("built move type differs from the plan");
                return 0;
            }
            ut_planned = false;
            ut_moves++;
            (void)pl;
            return BTN_X;
        }
    }
    case ST_ROUND_END:
        return state_time > 3.0f + UT_HOLD ? BTN_A : 0;
    case ST_GAME_OVER:
        if(state_time > 2.5f + UT_HOLD) {
            ut_games++;
            printf("BAZAAR UITEST: game %d finished.\n", ut_games);
            return BTN_START;
        }
        return 0;
    default:
        return 0;
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Frame                                                                */

static bool in_game_state(state_t s) {
    return s != ST_TITLE && s != ST_SETUP && s != ST_RULES;
}

static void draw_frame(float dt) {
    bool table = in_game_state(state) || (state == ST_RULES && in_game_state(rules_return));
    gfx_frame_begin();
    if(table) {
        gfx_background(JP_TEXID_TABLE, 0xffffffff);
        draw_opponent_panels();
        draw_market_area();
        draw_hand_area();
        draw_bazaar();
        draw_bottom_bar();
        draw_cards();
        draw_counts_over_cards();
        draw_cursor(dt);
        draw_hint_bar();
        draw_flights();
        if(demo_mode) {
            gfx_panel(232, 236, 176, 30, 12, 0xc0200828);
            gfx_text(FONT_SMALL, 320, 238, 0.8f,
                     fmodf(time_now, 1.2f) < 0.8f ? 0xffffd040 : 0xffffffff,
                     ALIGN_CENTER, "DEMO - PRESS START");
        }
    }
    else {
        gfx_background(JP_TEXID_TITLE, 0xffffffff);
    }

    switch(state) {
    case ST_TITLE: draw_title(); break;
    case ST_SETUP: draw_setup(); break;
    case ST_RULES: draw_rules(); break;
    case ST_PASS: draw_pass_screen(); break;
    case ST_ROUND_END: draw_round_end(); break;
    case ST_GAME_OVER: draw_game_over(); break;
    default: break;
    }
    draw_particles();
    draw_floaters();
    draw_banner();
    if(paused)
        draw_pause();
    gfx_frame_end();
}

int main(int argc, char **argv) {
    uint64_t last;
    (void)argc;
    (void)argv;

    printf("Totally Bazaar: booting.\n");
#ifdef BAZAAR_SELFTEST
    {
        int failures = jp_selftest();
        printf("Totally Bazaar self-test finished with %d failure(s).\n", failures);
    }
#endif
    if(gfx_init() < 0)
        return 1;
    audio_init();
    audio_music_start();
    audio_music_target(200);
    srand((unsigned)timer_us_gettime64());
    set_state(ST_TITLE);
#ifdef BAZAAR_AUTOPLAY
    demo_mode = true;
    setup_players = BAZAAR_AUTOPLAY;
    new_game();
#endif
    last = timer_us_gettime64();
    for(;;) {
        uint64_t now = timer_us_gettime64();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;
        if(dt > 0.05f)
            dt = 0.05f;
#ifdef BAZAAR_UITEST
        dt *= UT_SPEED;   /* run the scripted session fast */
#endif
        time_now += dt;
        state_time += dt;
        idle_time += dt;
        if(hint_flash > 0.0f)
            hint_flash -= dt;
        if(banner.dur > 0.0f)
            banner.t += dt;

        input_poll(dt);
#ifdef BAZAAR_UITEST
        {
            uint32_t b = ut_buttons(dt);
            in_pressed |= b;
            if(state == ST_TITLE)
                idle_time = 0.0f;
        }
#endif
        /* Classic Dreamcast soft reset: A+B+X+Y+START returns to the title. */
        if((in_held & (BTN_A | BTN_B | BTN_X | BTN_Y | BTN_START)) ==
           (BTN_A | BTN_B | BTN_X | BTN_Y | BTN_START)) {
            paused = false;
            demo_mode = false;
            set_state(ST_TITLE);
        }
        if(in_game_state(state) && state != ST_RULES)
            update_game(dt);
        else
            update_menus();
        if(in_game_state(state))
            layout_cards();
        update_cards(dt);
        update_particles(dt);
        update_floaters(dt);
        update_flights(dt);
        audio_music_target(paused ? 90 : 200);
        audio_update(dt);
        draw_frame(dt);
    }
    return 0;
}
