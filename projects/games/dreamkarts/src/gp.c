/* Grand Prix: every course in order, the same field every race, points
   for each finishing place (as the online lobbies award them) and the
   standings between the races. The results screen has two pages, the
   race with its points and the standings, and a quit prompt so a cup is
   never lost to a stray B. From the second race the grid lines up by the
   standings, leader in front. */
#include "gp.h"
#include "audio.h"
#include "fx.h"
#include "hud.h"
#include "kart.h"
#include "render.h"

GrandPrix gp;

static const int gp_points[MAX_KARTS] = {10, 8, 6, 4, 2, 1};
static const pvr_poly_hdr_t *h_flat;
static float confetti_t;

/* Insertion sort by points, then the better place in the latest race. */
static void rank_field(void) {
    int n = kart_count;
    for(int i = 0; i < n; i++) gp.rank[i] = i;
    for(int i = 1; i < n; i++) {
        int v = gp.rank[i], j = i - 1;
        while(j >= 0) {
            int a = gp.rank[j];
            bool v_ahead = gp.points[v] > gp.points[a] ||
                           (gp.points[v] == gp.points[a] && gp.place[v] < gp.place[a]);
            if(!v_ahead) break;
            gp.rank[j + 1] = gp.rank[j];
            j--;
        }
        gp.rank[j + 1] = v;
    }
}

void gp_start(void) {
    memset(&gp, 0, sizeof(gp));
    gp.active = true;
    gp.count = TRACK_COUNT;
#ifdef DK_AT_GP_RACES
    /* Test builds: a shorter cup. */
    if(gp.count > DK_AT_GP_RACES) gp.count = DK_AT_GP_RACES;
#endif
    for(int i = 0; i < TRACK_COUNT; i++) gp.order[i] = i;
    for(int i = 0; i < MAX_KARTS; i++) {
        gp.place[i] = MAX_KARTS;
        gp.rank[i] = i;
    }
    if(game.track != gp.order[0]) game_load_track(gp.order[0]);
    else race_setup();
    game_set_state(ST_INTRO);
}

void gp_race_done(void) {
    if(gp.race >= gp.count) return;
    for(int pos = 0; pos < kart_count; pos++) {
        int k = game.finish_order[pos];
        gp.gained[k] = gp_points[pos];
        gp.points[k] += gp.gained[k];
        gp.place[k] = pos + 1;
        gp.history[gp.race][k] = (uint8_t)(pos + 1);
    }
    gp.race++;
    rank_field();
    gp.page = 0;
    gp.quit_prompt = false;
    confetti_t = 0;
    printf("DK GP: race %d/%d %s:", gp.race, gp.count, track.name);
    for(int r = 0; r < kart_count; r++)
        printf(" %d.%s=%d", r + 1, karts[gp.rank[r]].name, gp.points[gp.rank[r]]);
    printf("\n");
}

bool gp_over(void) {
    return gp.race >= gp.count;
}

int gp_grid_slot(int kart) {
    for(int i = 0; i < kart_count; i++)
        if(gp.rank[i] == kart) return i;
    return kart;
}

int gp_standing(int kart) {
    return gp_grid_slot(kart) + 1;
}

static void gp_quit(void) {
    gp.active = false;
    race_setup();
    game_set_state(ST_TITLE);
}

/* The champion's moment on the final standings: confetti and a cheer. */
static void champion_party(bool first) {
    const Kart *c = &karts[gp.rank[0]];
    fx_confetti(vadd(c->pos, v3(0, 1.5f, 0)), c->pos.y, game.num_players > 1 ? 50 : 90);
    if(first) {
        audio_sfx(DK_SFX_FINISH, 240, 128);
        audio_chao(CHAO_HI, c->voice, 210, 128);
    }
}

void gp_results_update(void) {
    uint32_t pressed = pad.pressed | pad2.pressed;
#ifdef DK_AUTOTEST
    /* Nobody at the pad: turn each page after a look at it. */
    if(game.time > 4.0f && !gp.quit_prompt) pressed |= BTN_A;
#endif
    bool over = gp_over();
    if(over && gp.page == 1 && (confetti_t -= DK_DT) <= 0) {
        confetti_t = 2.5f;
        champion_party(false);
    }
    if(game.time < 1.0f) return;
    if(gp.quit_prompt) {
        if(pressed & BTN_A) {
            audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
            gp_quit();
        }
        else if(pressed & BTN_B) {
            audio_sfx(DK_SFX_MENU_BACK, 200, 128);
            gp.quit_prompt = false;
        }
        return;
    }
    if(pressed & BTN_A) {
        audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
        if(gp.page == 0) {
            gp.page = 1;
            game.time = 0;
            if(over) {
                champion_party(true);
                confetti_t = 2.5f;
            }
        }
        else if(!over) {
            audio_sfx(DK_SFX_WHOOSH, 160, 128);
            game_load_track(gp.order[gp.race]);
            game_set_state(ST_INTRO);
        }
        else {
            gp_quit();
        }
    }
    else if(pressed & BTN_B) {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        if(over) gp_quit();
        else gp.quit_prompt = true;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

#define PANEL_TOP argb(225, 30, 100, 220)
#define PANEL_BOT argb(225, 12, 36, 120)
#define GOLD      argb(255, 255, 220, 60)
#define PALE      argb(220, 200, 225, 255)
#define POINTS    argb(255, 255, 226, 70)
#define GAIN      argb(255, 120, 255, 140)

static void row_highlight(float x0, float x1, float y) {
    if(!h_flat) h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
    r_hdr(h_flat);
    float glow = 0.2f + 0.1f * sinf(game.title_t * 5);
    r_quad2d(x0 + 12, y - 4, x1 - 12, y + 32, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 15.5f);
}

static void draw_race_page(float x0, float x1) {
    char buf[48], t[20];
    float cx = (x0 + x1) * 0.5f;
    hud_text(cx, 64, 1.1f, ALIGN_CENTER, GOLD, "RESULTS");
    snprintf(buf, sizeof(buf), "RACE %d OF %d  -  %s", gp.race, gp.count, track.name);
    hud_text(cx, 108, 0.5f, ALIGN_CENTER, PALE, buf);
    hud_text(x1 - 88, 134, 0.42f, ALIGN_RIGHT, PALE, "RACE");
    hud_text(x1 - 16, 134, 0.42f, ALIGN_RIGHT, PALE, "TOTAL");
    for(int pos = 0; pos < kart_count; pos++) {
        int i = game.finish_order[pos];
        const Kart *k = &karts[i];
        float y = 156 + pos * 36;
        bool me = kart_is_human(i);
        if(me) row_highlight(x0, x1, y);
        snprintf(buf, sizeof(buf), "%d%s", pos + 1, hud_ordinal(pos + 1));
        hud_text(x0 + 22, y - 2, 0.75f, ALIGN_LEFT, hud_place_color(pos + 1), buf);
        hud_swatch(x0 + 88, y + 6, k->paint1, k->ui_color);
        hud_text(x0 + 122, y + 2, 0.62f, ALIGN_LEFT, me ? argb(255, 255, 255, 140) : 0xffffffff, k->name);
        hud_format_time(t, k->finish_time);
        hud_text(x1 - 150, y + 4, 0.55f, ALIGN_RIGHT, 0xffffffff, t);
        snprintf(buf, sizeof(buf), "+%d", gp.gained[i]);
        hud_text(x1 - 88, y + 2, 0.62f, ALIGN_RIGHT, GAIN, buf);
        snprintf(buf, sizeof(buf), "%d", gp.points[i]);
        hud_text(x1 - 16, y + 2, 0.62f, ALIGN_RIGHT, POINTS, buf);
    }
}

static void draw_standings_page(float x0, float x1) {
    char buf[48];
    float cx = (x0 + x1) * 0.5f;
    bool over = gp_over();
    hud_text(cx, 64, 1.1f, ALIGN_CENTER, GOLD, over ? "GRAND PRIX RESULTS" : "STANDINGS");
    if(over) snprintf(buf, sizeof(buf), "FINAL STANDINGS AFTER %d RACE%s", gp.count, gp.count == 1 ? "" : "S");
    else snprintf(buf, sizeof(buf), "AFTER RACE %d OF %d", gp.race, gp.count);
    hud_text(cx, 108, 0.5f, ALIGN_CENTER, PALE, buf);
    hud_text(x1 - 16, 134, 0.42f, ALIGN_RIGHT, PALE, "PTS");
    /* One small number a race: where each kart finished. */
    float hx = x0 + 262, hw = 19.0f;
    for(int r = 0; r < gp.race; r++) {
        snprintf(buf, sizeof(buf), "%d", r + 1);
        hud_text(hx + r * hw + hw * 0.5f, 136, 0.36f, ALIGN_CENTER, PALE, buf);
    }
    for(int pos = 0; pos < kart_count; pos++) {
        int i = gp.rank[pos];
        const Kart *k = &karts[i];
        float y = 156 + pos * 36;
        bool me = kart_is_human(i);
        if(me) row_highlight(x0, x1, y);
        if(over && pos < 3) {
            hud_trophy(x0 + 40, y + 6, 1.1f, pos == 0 ? HUD_GOLD : pos == 1 ? HUD_SILVER : HUD_BRONZE);
        }
        else {
            snprintf(buf, sizeof(buf), "%d%s", pos + 1, hud_ordinal(pos + 1));
            hud_text(x0 + 22, y - 2, 0.75f, ALIGN_LEFT, hud_place_color(pos + 1), buf);
        }
        hud_swatch(x0 + 88, y + 6, k->paint1, k->ui_color);
        hud_text(x0 + 122, y + 2, 0.62f, ALIGN_LEFT, me ? argb(255, 255, 255, 140) : 0xffffffff, k->name);
        for(int r = 0; r < gp.race; r++) {
            int place = gp.history[r][i];
            snprintf(buf, sizeof(buf), "%d", place);
            hud_text(hx + r * hw + hw * 0.5f, y + 6, 0.46f, ALIGN_CENTER, hud_place_color(place), buf);
        }
        if(!over) {
            snprintf(buf, sizeof(buf), "+%d", gp.gained[i]);
            hud_text(x1 - 72, y + 4, 0.5f, ALIGN_RIGHT, GAIN, buf);
        }
        snprintf(buf, sizeof(buf), "%d", gp.points[i]);
        hud_text(x1 - 16, y + 2, 0.62f, ALIGN_RIGHT, POINTS, buf);
    }
    /* The humans' standings, or what comes next. */
    float fy = 372;
    if(over) {
        if(game.num_players > 1) {
            int s1 = gp_standing(game.pl[0].kart), s2 = gp_standing(game.pl[1].kart);
            snprintf(buf, sizeof(buf), "P1 %d%s   P2 %d%s", s1, hud_ordinal(s1), s2, hud_ordinal(s2));
        }
        else {
            int s = gp_standing(game.pl[0].kart);
            snprintf(buf, sizeof(buf), s == 1 ? "CHAMPION!" : "YOU FINISHED %d%s", s, hud_ordinal(s));
        }
        float pulse = 1.0f + 0.04f * sinf(game.title_t * 6.0f);
        hud_text(cx, fy - 4, 0.7f * pulse, ALIGN_CENTER, GOLD, buf);
    }
    else {
        snprintf(buf, sizeof(buf), "NEXT: %s", track_names[gp.order[gp.race]]);
        hud_text(cx, fy, 0.55f, ALIGN_CENTER, 0xffffffff, buf);
    }
}

void gp_draw_results(void) {
    float a = clampf(game.time / 0.5f, 0, 1);
    float x0 = 70 + (1 - a) * 500, x1 = x0 + 500;
    hud_panel(x0, 52, x1, 424, PANEL_TOP, PANEL_BOT, 15);
    if(gp.page == 0) draw_race_page(x0, x1);
    else draw_standings_page(x0, x1);
    bool over = gp_over();
    const char *hint = gp.page == 0 ? "A: STANDINGS" : over ? "A: TITLE" : "A: NEXT RACE   B: QUIT";
    if(game.time > 1.0f && ((int)(game.title_t * 2.5f) & 1) == 0)
        hud_text((x0 + x1) * 0.5f, 396, 0.55f, ALIGN_CENTER, 0xffffffff, hint);
    if(gp.quit_prompt) {
        hud_panel(150, 180, 490, 300, PANEL_TOP, PANEL_BOT, 18);
        hud_text(320, 196, 0.8f, ALIGN_CENTER, GOLD, "QUIT THE GRAND PRIX?");
        hud_text(320, 250, 0.6f, ALIGN_CENTER, 0xffffffff, "A: QUIT   B: KEEP RACING");
    }
}
