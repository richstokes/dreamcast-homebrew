/* FlappyKarts: an endless flapper hidden behind Sonic 1's level select
   code on the title screen. The player's kart, seen side on at x = 0 facing +X,
   flaps (A) up through gaps in an endless run of columns that scroll past
   it; one point a column, over when it clips a column, the ground or the
   top of the screen. The columns are random, as a run of them, each gap
   within reach of the last. The scene is the background course's sky,
   clouds and mountains (world_draw_* with the scenery and sea probed off)
   over a ground plane and columns textured from that course's own set, and
   every WARP_SCORE points the run flashes white and moves on to the next
   course (a track load, up to a second, hidden behind the flash). */
#include "flappy.h"
#include "audio.h"
#include "hud.h"
#include "kart.h"
#include "models.h"
#include "render.h"
#include "texture_assets.h"
#include "title.h"
#include "track.h"
#include "ui.h"
#include "world.h"

/* The playfield, in metres: the camera is 17 m back, looking along +Z, so
   the screen spans about x -6.6..16.6 and y -1.1..16.3. */
#define CAM_X      5.0f
#define CAM_Y      7.6f
#define CAM_Z     -17.0f
#define START_Y    7.6f
#define CEIL_Y    15.4f    /* the kart stops here (and dies above it) */
#define GRAVITY   30.0f
#define FLAP_V    10.0f
#define FALL_V   -19.0f
#define HIT_X      1.0f    /* kart hit box half extents */
#define HIT_Y      0.6f
#define COL_HALF   1.3f    /* column half width and depth */
#define CAP_HALF   1.6f    /* its lip, like a pipe's */
#define CAP_H      0.9f
#define COL_SPACE 10.0f
#define COL_FIRST 24.0f    /* the first column of a run */
#define COL_AHEAD 36.0f    /* columns are kept out to here, well off the right edge (x ~16.6) */
#define GAP_LO     3.6f    /* gap centre range */
#define GAP_HI    11.4f
#define GAP_STEP   4.5f    /* most the gap centre moves from one column to the next */
#define GAP_MAX    5.4f    /* gap height at score 0 ... */
#define GAP_MIN    4.2f    /* ... and from score 40 on */
#define SPEED0     7.5f
#define SPEED1     9.5f    /* at score 40 */
#define MAX_COLS   7
#define WARP_SCORE 25      /* points between courses */
#define WARP_IN    0.45f   /* seconds to white ... */
#define WARP_OUT   0.6f    /* ... and back */
#define NAME_SECS  2.2f    /* the course name after a warp */
#define COL_GONE  20.0f    /* a column this far behind leaves */

enum { FL_READY, FL_PLAY, FL_WARP, FL_DEAD };

typedef struct {
    float x, gap_y, gap_h;
    bool scored;
} Column;

/* Columns, lip and ground textures per background course, with the
   metres a repeat covers (the columns' across and up). */
typedef struct {
    int side, cap, ground;
    float side_mu, side_mv, cap_m, ground_m;
} Theme;

static const Theme themes[TRACK_COUNT] = {
    [TRACK_BEACH] = {TEX_BOARDWALK, TEX_CURB, TEX_SAND, 2.6f, 2.6f, 1.6f, 4.0f},
    [TRACK_FIGURE8] = {TEX_ROCK, TEX_CHECKER, TEX_GRASS, 2.6f, 2.6f, 1.6f, 4.0f},
    [TRACK_HARBOUR] = {TEX_BARRIER, TEX_WOOD, TEX_GRASS, 5.2f, 5.2f, 1.6f, 4.0f},   /* the chevron panel */
    [TRACK_TEMPLE] = {TEX_STONE, TEX_STONE, TEX_GRASS, 2.6f, 2.6f, 3.2f, 4.0f},
    [TRACK_SKYWAY] = {TEX_TOWER, TEX_TOWER, TEX_CLOUDSEA, 5.2f, 5.2f, 3.2f, 8.0f},
    [TRACK_CYBERSTONE] = {TEX_TOWER, TEX_TOWER, TEX_CLOUDSEA, 5.2f, 5.2f, 3.2f, 8.0f},
    [TRACK_CANYON] = {TEX_ISLANDROCK, TEX_EHCHECKER, TEX_MEADOW, 2.6f, 2.6f, 3.2f, 4.0f},
    [TRACK_TSUDOKEN] = {TEX_ISHIGAKI, TEX_KAWARA, TEX_VERGE, 2.6f, 2.6f, 1.6f, 4.0f},
    [TRACK_APPLE] = {TEX_AVTYRES, TEX_AVMETAL, TEX_AVDESERT, 8.0f, 1.1f, 1.6f, 4.0f},   /* a tyre stack */
};

static int phase;
static float phase_t;           /* seconds in this phase */
static float ky, kvy;           /* the kart's height and vertical speed */
static float pitch;             /* nose up with a flap, down in a fall */
static float scroll, speed;
static float bounce;            /* dead: a bounce left when it hits the ground */
static int score, best;
static bool paused, new_best, warped;   /* warped: this warp's load is done */
static float name_t;                    /* seconds of course name left to show */
static Column cols[MAX_COLS];
static int ncols;
static const Theme *theme;
static const pvr_poly_hdr_t *h_side, *h_cap, *h_ground, *h_shadow, *h_flat;

static Kart *kart(void) {
    return &karts[game.player];
}

/* Light: the course's tint (bluer and darker at night), scaled. */
static uint32_t lit(float s) {
    int r = (int)(world_light[0] * s), g = (int)(world_light[1] * s), b = (int)(world_light[2] * s);
    return argb(255, r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

static float gap_height(void) {
    return lerpf(GAP_MAX, GAP_MIN, clampf(score / 40.0f, 0, 1));
}

static void add_column(void) {
    if(ncols >= MAX_COLS) return;
    Column *c = &cols[ncols];
    float x = ncols ? cols[ncols - 1].x + COL_SPACE : COL_FIRST;
    float last = ncols ? cols[ncols - 1].gap_y : START_Y;
    float lo = fmaxf(GAP_LO, last - GAP_STEP), hi = fminf(GAP_HI, last + GAP_STEP);
    c->x = x;
    c->gap_y = lo + frand() * (hi - lo);
    c->gap_h = gap_height();
    c->scored = false;
    ncols++;
}

/* Columns out to COL_AHEAD, so a new one never pops in on screen. */
static void fill_columns(void) {
    while(ncols < MAX_COLS && (ncols == 0 || cols[ncols - 1].x < COL_AHEAD)) add_column();
}

/* The kart's pose: side on at x = 0, nose along +X, upright, the nose
   lifting with a flap and dropping in a fall; the player's own colours. */
static void pose_kart(void) {
    Kart *k = kart();
    k->pos = v3(0, ky, 0);
    k->heading = DK_PI * 0.5f;
    k->roll = 0;
    k->pitch = pitch;
    k->vis_yaw = k->lean = k->nose = 0;
    k->grounded = false;
    k->gnd = 0;
    k->speed = 14.0f + fmaxf(0.0f, kvy) * 0.8f;   /* the engine revs with a flap */
}

void flappy_init(void) {
    h_shadow = r_header(TEX_SHADOW, HF_TR);
    h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
}

/* The current course's textures. */
static void set_theme(void) {
    theme = &themes[game.track];
    h_side = r_header(theme->side, HF_OP);
    h_cap = r_header(theme->cap, HF_OP);
    h_ground = r_header(theme->ground, HF_OP);
}

static void start_run(void) {
    phase = FL_READY;
    phase_t = 0;
    ky = START_Y;
    kvy = 0;
    pitch = 0;
    scroll = 0;
    bounce = 0;
    speed = SPEED0;
    score = 0;
    ncols = 0;
    paused = new_best = warped = false;
    name_t = 0;
    fill_columns();
    Kart *k = kart();
    k->squash = k->squash_v = 0;
    k->trick_a = 0;
    k->wave_t = k->cheer_t = k->raise_t = 0;
    k->boost_t = k->burnout_t = k->spin_t = k->shield_t = 0;
    k->arm_up[0] = k->arm_up[1] = 0;
    k->item = 0;
    k->finished = false;
    pose_kart();
}

void flappy_enter(void) {
    set_theme();
    game_set_state(ST_FLAPPY);
    start_run();
    audio_sfx(DK_SFX_WHOOSH, 180, 128);
    audio_chao(CHAO_HI, kart()->voice, 200, 128);
}

static void leave(void) {
    audio_sfx(DK_SFX_MENU_BACK, 200, 128);
    race_setup();   /* the karts back on the grid behind the title */
    game_set_state(ST_TITLE);
}

static void flap(void) {
    Kart *k = kart();
    kvy = FLAP_V;
    k->squash = 0.22f;
    k->arm_up[0] = k->arm_up[1] = 1.0f;   /* the Chao flaps */
    audio_sfx_pitch(DK_SFX_HOP, 150, 128, 1.25f);
}

static void die(void) {
#ifdef DK_AT_FLAPPY
    printf("DK FLAPPY dead score=%d y=%.2f vy=%.1f", score, (double)ky, (double)kvy);
    for(int i = 0; i < ncols; i++)
        if(fabsf(cols[i].x) < 4.0f) printf(" col x=%.2f gap=%.2f+-%.2f", (double)cols[i].x, (double)cols[i].gap_y, (double)(cols[i].gap_h * 0.5f));
    printf("\n");
#endif
    phase = FL_DEAD;
    phase_t = 0;
    bounce = 1;
    kvy = fminf(kvy, 2.0f);
    audio_sfx(DK_SFX_BUMP, 210, 128);
    audio_chao(CHAO_OUCH, kart()->voice, 210, 128);
    if(score > best) {
        best = score;
        new_best = score > 0;
    }
}

static bool hits_column(const Column *c) {
    if(fabsf(c->x) > COL_HALF + HIT_X) return false;
    return ky - HIT_Y < c->gap_y - c->gap_h * 0.5f || ky + HIT_Y > c->gap_y + c->gap_h * 0.5f;
}

void flappy_update(float dt) {
    Kart *k = kart();
    uint32_t pressed = pad.pressed | pad2.pressed;
    phase_t += dt;
    if(phase == FL_PLAY && (pressed & BTN_START)) {
        paused = !paused;
        audio_sfx(DK_SFX_MENU_SELECT, 200, 128);
    }
    if(paused) {
        if(pressed & BTN_B) leave();
        audio_kart_loops(0, k, dt);
        return;
    }
    switch(phase) {
        case FL_READY:
            ky = START_Y + 0.35f * sinf(phase_t * 3.0f);
            pitch = 0.08f * cosf(phase_t * 3.0f);
            if(pressed & BTN_B) { leave(); return; }
            if(pressed & BTN_A) {
                phase = FL_PLAY;
                phase_t = 0;
                flap();
            }
            break;
        case FL_PLAY:
            if(pressed & BTN_A) flap();
            kvy = fmaxf(FALL_V, kvy - GRAVITY * dt);
            ky += kvy * dt;
            if(ky > CEIL_Y) { ky = CEIL_Y; die(); }
            speed = lerpf(SPEED0, SPEED1, clampf(score / 40.0f, 0, 1));
            scroll += speed * dt;
            for(int i = 0; i < ncols; i++) {
                Column *c = &cols[i];
                c->x -= speed * dt;
                if(!c->scored && c->x < 0) {
                    c->scored = true;
                    score++;
                    audio_sfx_pitch(DK_SFX_EMERALD, 120, 128, 1.0f + 0.004f * (score % 10));
                    if(score % WARP_SCORE == 0) {
                        phase = FL_WARP;
                        phase_t = 0;
                        warped = false;
                        audio_sfx(DK_SFX_LAP, 200, 128);
                    }
                }
                if(hits_column(c)) die();
            }
            if(ky - HIT_Y < 0) { ky = HIT_Y; die(); }
            /* Columns off the left leave; the next spawns to keep the spacing. */
            while(ncols > 0 && cols[0].x < -COL_GONE) {
                memmove(&cols[0], &cols[1], (ncols - 1) * sizeof(cols[0]));
                ncols--;
            }
            fill_columns();
            break;
        case FL_WARP:
            /* The kart hangs where it was while the screen goes white; the
               next course loads behind the white (game_load_track regrids
               the karts, which pose_kart undoes), fresh columns ahead of it,
               and the white clears on the new scene. */
            kvy = 0;
            if(phase_t >= WARP_IN && !warped) {
                warped = true;
                game_load_track(track_step(game.track, 1));
                set_theme();
                ncols = 0;
                fill_columns();
                name_t = NAME_SECS;
            }
            if(phase_t >= WARP_IN + WARP_OUT) {
                phase = FL_PLAY;
                phase_t = 0;
            }
            break;
        case FL_DEAD:
            kvy = fmaxf(FALL_V, kvy - GRAVITY * dt);
            ky += kvy * dt;
            if(ky - HIT_Y < 0) {
                ky = HIT_Y;
                if(bounce > 0 && kvy < -3.0f) {
                    kvy = -kvy * 0.3f;
                    bounce = 0;
                    audio_sfx(DK_SFX_LAND, 160, 128);
                }
                else kvy = 0;
            }
            /* It tumbles as it goes down, and settles on a quarter turn
               (its wheels, side or roof) once down. */
            if(ky > HIT_Y + 0.01f) k->trick_a += dt * 7.0f;
            else k->trick_a = approach(k->trick_a, roundf(k->trick_a / (DK_PI * 0.5f)) * DK_PI * 0.5f, dt * 4.0f);
            if(phase_t > 0.8f) {
                if(pressed & BTN_A) {
                    audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
                    start_run();
                    return;
                }
                if(pressed & BTN_B) { leave(); return; }
            }
            break;
    }
    if(name_t > 0) name_t -= dt;
    /* Nose up on the way up, down in a fall, quickly. */
    if(phase != FL_READY) {
        float target = clampf(kvy * 0.07f, -1.0f, 0.45f);
        pitch += (target - pitch) * smooth_k(9.0f, dt);
    }
    k->squash = approach(k->squash, 0.0f, dt * 1.1f);
    for(int a = 0; a < 2; a++) k->arm_up[a] = approach(k->arm_up[a], 0.0f, dt * 2.4f);
    k->wheel_spin += speed / 0.23f * dt;
    if(k->wheel_spin > DK_TAU) k->wheel_spin -= DK_TAU * floorf(k->wheel_spin / DK_TAU);
    pose_kart();
    audio_kart_loops(0, k, dt);
}

void flappy_camera(Camera *cam) {
    cam->pos = v3(CAM_X, CAM_Y, CAM_Z);
    cam->fwd = v3(0, 0, 1);
    cam->right = v3(1, 0, 0);
    cam->up = v3(0, 1, 0);
    cam->fov = 1.20f;
    cam->shake = 0;
}

/* A box's five faces that can show (all but the back), lit per face,
   textured at m metres a repeat with the U origin at u0 (metres). */
static void box(float x0, float x1, float y0, float y1, float z0, float z1, float mu, float mv, float u0) {
    float im = 1.0f / mu, iv = 1.0f / mv;
    uint32_t front = lit(0.92f), left = lit(0.68f), right = lit(0.80f), top = lit(1.0f), bottom = lit(0.5f);
    RV v[4];
    /* Front (-Z). */
    v[0] = (RV){x0, y1, z0, (x0 - u0) * im, y1 * iv, front};
    v[1] = (RV){x1, y1, z0, (x1 - u0) * im, y1 * iv, front};
    v[2] = (RV){x0, y0, z0, (x0 - u0) * im, y0 * iv, front};
    v[3] = (RV){x1, y0, z0, (x1 - u0) * im, y0 * iv, front};
    r_strip(v, 4);
    /* Left (-X). */
    v[0] = (RV){x0, y1, z1, (z1 - z0) * im, y1 * iv, left};
    v[1] = (RV){x0, y1, z0, 0, y1 * iv, left};
    v[2] = (RV){x0, y0, z1, (z1 - z0) * im, y0 * iv, left};
    v[3] = (RV){x0, y0, z0, 0, y0 * iv, left};
    r_strip(v, 4);
    /* Right (+X). */
    v[0] = (RV){x1, y1, z0, 0, y1 * iv, right};
    v[1] = (RV){x1, y1, z1, (z1 - z0) * im, y1 * iv, right};
    v[2] = (RV){x1, y0, z0, 0, y0 * iv, right};
    v[3] = (RV){x1, y0, z1, (z1 - z0) * im, y0 * iv, right};
    r_strip(v, 4);
    /* Top (+Y). */
    v[0] = (RV){x0, y1, z1, (x0 - u0) * im, 0, top};
    v[1] = (RV){x1, y1, z1, (x1 - u0) * im, 0, top};
    v[2] = (RV){x0, y1, z0, (x0 - u0) * im, (z1 - z0) * im, top};
    v[3] = (RV){x1, y1, z0, (x1 - u0) * im, (z1 - z0) * im, top};
    r_strip(v, 4);
    /* Bottom (-Y). */
    v[0] = (RV){x0, y0, z0, (x0 - u0) * im, 0, bottom};
    v[1] = (RV){x1, y0, z0, (x1 - u0) * im, 0, bottom};
    v[2] = (RV){x0, y0, z1, (x0 - u0) * im, (z1 - z0) * im, bottom};
    v[3] = (RV){x1, y0, z1, (x1 - u0) * im, (z1 - z0) * im, bottom};
    r_strip(v, 4);
}

static void draw_column(const Column *c) {
    float lo = c->gap_y - c->gap_h * 0.5f, hi = c->gap_y + c->gap_h * 0.5f;
    float x0 = c->x - COL_HALF, x1 = c->x + COL_HALF;
    float cx0 = c->x - CAP_HALF, cx1 = c->x + CAP_HALF;
    r_hdr(h_side);
    box(x0, x1, -0.5f, lo - CAP_H, -COL_HALF, COL_HALF, theme->side_mu, theme->side_mv, x0);
    box(x0, x1, hi + CAP_H, 24.0f, -COL_HALF, COL_HALF, theme->side_mu, theme->side_mv, x0);
    r_hdr(h_cap);
    box(cx0, cx1, lo - CAP_H, lo, -CAP_HALF, CAP_HALF, theme->cap_m, theme->cap_m, cx0);
    box(cx0, cx1, hi, hi + CAP_H, -CAP_HALF, CAP_HALF, theme->cap_m, theme->cap_m, cx0);
}

/* The ground: cells with their own wrapped UVs (large UV values swim on
   the console), small near the camera, large further out, scrolling with
   the columns. */
static void ground_cell(float x0, float x1, float z0, float z1, float m, float off, uint32_t col) {
    float im = 1.0f / m;
    RV v[4] = {
        {x0, 0, z1, off, 0, col}, {x1, 0, z1, off + (x1 - x0) * im, 0, col},
        {x0, 0, z0, off, (z1 - z0) * im, col}, {x1, 0, z0, off + (x1 - x0) * im, (z1 - z0) * im, col}
    };
    r_strip(v, 4);
}

static void draw_ground(void) {
    float m = theme->ground_m;
    /* The scroll's fraction of a repeat: cells are whole repeats wide. */
    float off = scroll / m;
    off -= floorf(off);
    r_hdr(h_ground);
    uint32_t near = lit(0.95f), far = lit(0.88f);
    const float cell = 12.0f;
    for(int j = 0; j < 4; j++)
        for(int i = 0; i < 6; i++)
            ground_cell(-24.0f + i * cell, -24.0f + (i + 1) * cell, -14.0f + j * cell, -14.0f + (j + 1) * cell, m, off, near);
    const float big = 96.0f;
    for(int j = 0; j < 4; j++)
        for(int i = 0; i < 7; i++)
            ground_cell(-254.0f + i * big, -254.0f + (i + 1) * big, 34.0f + j * big, 34.0f + (j + 1) * big, m, off, far);
    ground_cell(-254.0f, -24.0f, -14.0f, 34.0f, m, off, far);   /* either side of the near cells */
    ground_cell(48.0f, 418.0f, -14.0f, 34.0f, m, off, far);
}

void flappy_draw_op(void) {
    r_load_vp();
    draw_ground();
    for(int i = 0; i < ncols; i++) draw_column(&cols[i]);
    models_draw_kart(game.player);
}

void flappy_draw_tr(void) {
    r_load_vp();
    /* A soft blob under the kart, smaller and fainter the higher it flies. */
    float h = clampf(ky / 16.0f, 0, 1);
    float w = 1.5f - 0.5f * h, d = 1.0f - 0.3f * h;
    uint32_t col = color_alpha(0xff000000, 0.55f * (1.0f - h * 0.7f));
    RV v[4] = {
        {-w, 0.03f, d, 0, 0, col}, {w, 0.03f, d, 1, 0, col},
        {-w, 0.03f, -d, 0, 1, col}, {w, 0.03f, -d, 1, 1, col}
    };
    r_hdr(h_shadow);
    r_strip(v, 4);
}

void flappy_draw_hud(void) {
    char buf[32];
    if(phase != FL_READY) {
        snprintf(buf, sizeof(buf), "%d", score);
        hud_text(320, 28, 2.2f, ALIGN_CENTER, 0xffffffff, buf);
    }
    if(best > 0 && phase != FL_DEAD) {
        snprintf(buf, sizeof(buf), "BEST %d", best);
        hud_text(626, 14, 0.62f, ALIGN_RIGHT, ui_col(UI_ACCENT), buf);
    }
    if(phase == FL_READY) {
        ui_heading(320, 110, "FLAPPYKARTS");
        float pulse = 0.5f + 0.5f * sinf(game.title_t * 4.5f);
        hud_text_c(320, 160, 0.8f, ALIGN_CENTER, color_lerp(ui_col(UI_TEXT), 0xffffffff, pulse), "PRESS A TO FLAP");
        ui_hint("A: FLAP   B: BACK");
    }
    else if(phase == FL_DEAD && phase_t > 0.8f) {
        float in = clampf((phase_t - 0.8f) * 4.0f, 0, 1);
        in = in * in * (3 - 2 * in);
        float y0 = 150.0f + (1 - in) * 60.0f;
        ui_panel(200, y0, 440, y0 + 150);
        float y = ui_title(200, y0, 440, "GAME OVER");
        snprintf(buf, sizeof(buf), "SCORE  %d", score);
        hud_text_c(320, y + 28, 0.9f, ALIGN_CENTER, 0xffffffff, buf);
        snprintf(buf, sizeof(buf), "BEST  %d", best);
        hud_text_c(320, y + 62, 0.72f, ALIGN_CENTER, ui_col(UI_ACCENT), buf);
        if(new_best && ((int)(phase_t * 5) & 1))
            hud_text_c(320, y + 92, 0.68f, ALIGN_CENTER, ui_col(UI_GOOD), "NEW BEST!");
        ui_hint("A: AGAIN   B: TITLE");
    }
    if(name_t > 0 && phase != FL_DEAD) ui_heading(320, 150, track.name);   /* the course just warped to */
    if(paused) {
        ui_dim();
        ui_heading(320, 200, "PAUSED");
        ui_hint("START: CONTINUE   B: TITLE");
    }
    if(phase == FL_WARP) {
        float a = phase_t < WARP_IN ? phase_t / WARP_IN : 1.0f - (phase_t - WARP_IN) / WARP_OUT;
        uint32_t col = color_alpha(0xffffffff, clampf(a, 0, 1));
        r_hdr(h_flat);
        r_quad2d_grad(0, 0, 640, 480, col, col, 60.0f);
    }
}

#ifdef DK_AT_FLAPPY
/* Capture build: the level select code on the title, a shot of the ready
   screen, columns flown on autopilot with a shot at the third and one of
   the next course's name after the warp at twenty-five, then hands off the
   controls, a shot of the game over panel, and B back to the title; passes
   if it got past the warp (a wrong hit box would kill it sooner) and the
   kart is back on the grid. */
void flappy_autotest(void) {
    static float t, tap_t;
    static int step;
    static bool shot_ready, shot_play, shot_warp, shot_dead, left, shot_back, done;
    t += DK_DT;
    if(game.state == ST_TITLE && left) {
        static bool back_ok;
        if(game.time > 1.0f && !shot_back) {
            shot_back = true;
            const Kart *k = kart();
            back_ok = fabsf(k->roll) < 0.1f && k->grounded && fabsf(k->pos.y - k->gnd) < 0.5f && k->tp.s != 0;
            printf("DK SHOT flappy-back\nDK FLAPPY back roll=%.2f y=%.2f gnd=%.2f s=%.1f\n", (double)k->roll,
                   (double)k->pos.y, (double)k->gnd, (double)k->tp.s);
        }
        /* The verdict a moment after the shot: the launcher closes Flycast
           on it, and the capture tool needs the window still up. */
        if(game.time > 2.0f && !done) {
            done = true;
            printf("DK AUTOTEST: %s\n", score > WARP_SCORE && back_ok ? "PASS" : "FAIL");
        }
        return;
    }
    if(game.state == ST_TITLE) {
        static const uint32_t code[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A | BTN_START};
        if(t > 1.5f && step < 5 && (tap_t += DK_DT) >= 0.25f) {
            tap_t = 0;
            input_film_tap(0, code[step++]);
        }
        return;
    }
    if(game.state != ST_FLAPPY) return;
    if(phase == FL_READY) {
        if(phase_t > 1.0f && !shot_ready) { shot_ready = true; printf("DK SHOT flappy-ready\n"); }
        if(phase_t > 1.6f) pad.pressed |= BTN_A;
    }
    else if(phase == FL_PLAY) {
        /* Aim for the next gap until three are through, then let go. */
        float target = START_Y;
        for(int i = 0; i < ncols; i++)
            if(cols[i].x > -COL_HALF - HIT_X) { target = cols[i].gap_y; break; }
        /* A flap climbs 1.7 m, so flapping from 0.9 m under the centre keeps
           the kart within about 1 m of it either way. */
        if(score < WARP_SCORE + 1 && ky < target - 0.9f && kvy < 1.0f) pad.pressed |= BTN_A;
        float ahead = 1e9f;
        for(int i = 0; i < ncols; i++)
            if(cols[i].x > 0 && cols[i].x < ahead) ahead = cols[i].x;
        if(score == 2 && !shot_play && ahead > 2.0f && ahead < 5.0f) {
            shot_play = true;
            printf("DK SHOT flappy-play\n");
        }
        if(score == WARP_SCORE && name_t > 0 && name_t < NAME_SECS - 0.4f && !shot_warp) {
            shot_warp = true;
            printf("DK SHOT flappy-warp\n");
        }
    }
    else if(phase == FL_DEAD) {
        if(phase_t > 1.6f && !shot_dead) { shot_dead = true; printf("DK SHOT flappy-dead\n"); }
        if(phase_t > 2.4f && !left) {
            left = true;
            printf("DK FLAPPY score=%d\n", score);
            pad.pressed |= BTN_B;   /* back to the title: the kart must be on the grid again */
        }
    }
}
#endif
