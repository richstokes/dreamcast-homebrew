/* The HUD and the screens (hud.h). */
#include "hud.h"
#include "score.h"
#include "kaiju.h"
#include "life.h"
#include "city.h"
#include "save.h"
#include "engine/ui.h"
#include "font_metrics.h"
#include "version.h"

static EngFont font;
static const pvr_poly_hdr_t *h_logo;
#define INK 0xff16121eu
#define C(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static const EngUiTheme theme = {
    .roles = {
        [UI_TITLE] = C(255, 255, 255, 255),   [UI_ITEM] = C(255, 255, 244, 220),     [UI_ITEM_SEL] = C(255, 255, 255, 255),
        [UI_LABEL] = C(255, 190, 200, 230),   [UI_VALUE] = C(255, 255, 244, 220),    [UI_VALUE_SEL] = C(255, 255, 255, 255),
        [UI_DIM] = C(255, 190, 200, 230),     [UI_TEXT] = C(255, 255, 248, 232),     [UI_HINT] = C(255, 255, 244, 220),
        [UI_ACCENT] = C(255, 255, 214, 80),   [UI_GOOD] = C(255, 126, 232, 120),     [UI_WARN] = C(255, 255, 110, 90),
    },
    .outline = INK,
    .shadow = C(110, 22, 18, 34),
    .panel_top = C(236, 70, 62, 124), .panel_bottom = C(236, 44, 38, 86),
    .well_top = C(255, 30, 26, 60), .well_bottom = C(255, 22, 18, 44),
    .plaque_top = C(255, 255, 110, 90), .plaque_bottom = C(255, 226, 64, 76), .plaque_glow = C(255, 255, 160, 110),
    .button_top = C(255, 100, 92, 160), .button_bottom = C(255, 66, 58, 118),
    .hint_top = C(232, 70, 62, 124), .hint_bottom = C(232, 44, 38, 86),
    .dim = C(150, 22, 18, 34),
};

void hud_init(void) {
    font.hdr = r_header(TEX_FONT, HF_TR | HF_NOFOG | HF_CLAMP);
    font.glyphs = (const eng_glyph_t *)font_glyphs;
    font.row_h = FONT_ROW_H;
    font.tex_w = FONT_TEX_W;
    font.tex_h = FONT_TEX_H;
    font.cap_top = FONT_CAP_TOP;
    font.cap_bottom = FONT_CAP_BOTTOM;
    h_logo = r_header(TEX_LOGO, HF_TR | HF_NOFOG | HF_CLAMP);
    ui_init(&font, &theme);
}

const EngFont *hud_font(void) { return &font; }

static void text(float x, float cy, float scale, int align, uint32_t col, const char *s) {
    text_draw_c(&font, x, cy, scale, align, col, s);
}

/* A bar: an inked pill with a fill of k (0-1). */
static void bar(float x0, float y0, float x1, float y1, float k, uint32_t top, uint32_t bottom) {
    ui_pill(x0 - 3, y0 - 3, x1 + 3, y1 + 3, INK, INK);
    ui_pill(x0, y0, x1, y1, 0xff3a3468, 0xff2a2450);
    k = clampf(k, 0, 1);
    if(k > 0.02f) ui_pill(x0, y0, x0 + (x1 - x0) * fmaxf(k, (y1 - y0) / (x1 - x0)), y1, top, bottom);
}

static void clock_text(char *out, size_t n) {
    /* The hour, 12:00 to 1:00, over the lunch's three minutes. */
    int mins = (int)((LUNCH_SECONDS - game.clock) * (60.0f / LUNCH_SECONDS));
    if(mins < 0) mins = 0;
    if(mins >= 60) snprintf(out, n, "1:00");
    else snprintf(out, n, "12:%02d", mins);
}

void hud_draw(void) {
    char buf[64];
    /* The clock. */
    bool late = game.clock < 20.0f && game.clock > 0;
    float pulse = late ? 0.5f + 0.5f * sinf(game.time * 10.0f) : 0;
    clock_text(buf, sizeof(buf));
    ui_rrect(22, 22, 232, 84, 12, INK, INK);
    ui_rrect(25, 25, 229, 81, 10, late ? color_lerp(0xff6a2a4a, 0xffe84058, pulse) : 0xff4a4284, late ? 0xff4a1c38 : 0xff2e2858);
    text(36, 43, 0.4f, ALIGN_LEFT, 0xffbec8e6, "LUNCH BREAK");
    text(218, 43, 0.62f, ALIGN_RIGHT, 0xffffffff, buf);
    bar(36, 62, 218, 72, game.clock / LUNCH_SECONDS, late ? 0xffff7060 : 0xff7ee878, late ? 0xffe04050 : 0xff48b860);

    /* The bill. */
    score_money(buf, sizeof(buf), score.total);
    text(616, 30, 0.4f, ALIGN_RIGHT, 0xffffffff, "DAMAGE");
    text(616, 56, 0.72f, ALIGN_RIGHT, 0xffffd650, buf);

    /* The combo. */
    if(score.chain || score.pot) {
        float pop = 1.0f + 0.35f * score.pop;
        uint32_t col = score.mult >= 25 ? 0xffff5868 : score.mult >= 10 ? 0xffffa040 : score.mult >= 3 ? 0xffffd650 : 0xffffffff;
        snprintf(buf, sizeof(buf), "x%d", score.mult);
        text(320, 44, 1.25f * pop, ALIGN_CENTER, col, buf);
        bar(250, 74, 390, 84, score.timer / score.timer_max, 0xffffffff, col);
        const char *rank = score_rank(score.mult);
        if(rank[0]) text(320, 100, 0.5f, ALIGN_CENTER, col, rank);
        char money[40];
        score_money(money, sizeof(money), score.pot);
        snprintf(buf, sizeof(buf), "%s  x%d", money, score.mult);
        text(320, rank[0] ? 120 : 102, 0.42f, ALIGN_CENTER, 0xffffffff, buf);
    }
    else if(score.bank_age < 1.8f && score.bank_amount) {
        float k = score.bank_age < 0.15f ? score.bank_age / 0.15f : 1.0f;
        char money[40];
        score_money(money, sizeof(money), score.bank_amount);
        snprintf(buf, sizeof(buf), "BANKED %s", money);
        text(320, 50, 0.7f * (0.6f + 0.4f * k), ALIGN_CENTER, color_alpha(0xff7ee878, 2.0f * (1.8f - score.bank_age)), buf);
    }
    /* Tricks, newest first, down the left. */
    float y = 116;
    for(int i = 0; i < SCORE_FEED; i++) {
        if(score.feed[i].age > 2.6f || !score.feed[i].name) continue;
        float a = score.feed[i].age, in = a < 0.12f ? a / 0.12f : 1.0f;
        uint32_t col = color_alpha(i == 0 && a < 0.5f ? 0xffffffff : 0xffffd650, (2.6f - a) * 3.0f);
        if(score.feed[i].times > 1) snprintf(buf, sizeof(buf), "%s x%d  +%d", score.feed[i].name, score.feed[i].times, score.feed[i].bonus);
        else snprintf(buf, sizeof(buf), "%s  +%d", score.feed[i].name, score.feed[i].bonus);
        text(30 - 60 * (1 - in), y, 0.5f, ALIGN_LEFT, col, buf);
        y += 24;
    }

    /* The breath. */
    bool full = kj.energy > 0.98f;
    text(30, 420, 0.4f, ALIGN_LEFT, 0xffffffff, "ATOMIC BREATH");
    bar(30, 436, 200, 450, kj.energy, full && ((int)(game.time * 6) & 1) ? 0xffffffff : 0xffa0f4ff, 0xff40b8f0);

    /* The army's interest. */
    int threat = life_threat();
    text(610, 420, 0.4f, ALIGN_RIGHT, 0xffffffff, "THREAT");
    for(int i = 0; i < LIFE_THREAT_MAX; i++) {
        float cx = 610 - 12 - (float)(LIFE_THREAT_MAX - 1 - i) * 27, cy = 443;
        ui_circle(cx, cy, 11, INK);
        ui_circle(cx, cy, 8, i < threat ? 0xffff5868 : 0xff3a3468);
    }

    if(game.show_fps) {
        snprintf(buf, sizeof(buf), "%.0f FPS  %.1f MS", (double)game.fps, (double)game.cpu_ms);
        text(320, 464, 0.4f, ALIGN_CENTER, 0xe0ffffff, buf);
    }

    /* What is in the jaws: from behind there is no seeing it. */
    if(kaiju_carrying()) {
        snprintf(buf, sizeof(buf), "CARRYING %s", kaiju_carrying());
        text(320, 414, 0.5f, ALIGN_CENTER, 0xffffffff, buf);
        text(320, 436, 0.4f, ALIGN_CENTER, 0xffffd650, "LET GO OF R TO THROW");
    }

    /* How to play, at the start. */
    if(game.clock > LUNCH_SECONDS - 14.0f && game.intro <= 0) {
        float a = LUNCH_SECONDS - game.clock;
        ui_hint_at(320, 400, a < 7.0f ? "A: CLAWS   X: TAIL   B: STOMP   Y: BREATH" : "R: CHOMP, HOLD TO GRAB   L: CHARGE   UP: ROAR");
    }
    /* The cards. */
    if(game.intro > 0) {
        float u = 2.4f - game.intro, k = u < 0.2f ? u / 0.2f * 1.2f : u < 0.3f ? 1.2f - (u - 0.2f) * 2.0f : 1.0f;
        text(320, 200, 1.6f * k, ALIGN_CENTER, 0xffffd650, game.intro > 0.7f ? "LUNCH TIME!" : "DIG IN!");
        if(game.intro > 0.7f) text(320, 256, 0.6f, ALIGN_CENTER, 0xffffffff, "ONE HOUR. ONE CITY.");
    }
    else if(game.outro > 0) {
        float u = game.outro, k = u < 0.2f ? u / 0.2f * 1.2f : u < 0.3f ? 1.2f - (u - 0.2f) * 2.0f : 1.0f;
        text(320, 210, 1.5f * k, ALIGN_CENTER, 0xffff7060, "BACK TO WORK!");
    }
    else if(game.clock <= 10.0f && game.clock > 0) {
        float fr = game.clock - floorf(game.clock);
        snprintf(buf, sizeof(buf), "%d", (int)game.clock + 1);
        text(320, 190, 1.6f + 1.2f * fr, ALIGN_CENTER, color_alpha(0xffff7060, 0.4f + fr), buf);
    }
}

void hud_draw_title(void) {
    float bob = sinf(game.time * 2.0f) * 5.0f;
    r_hdr(h_logo);
    r_quad2d(64, 30 + bob, 576, 286 + bob, 0, 0, 1, 1, 0xffffffff, 19.0f);
    if((int)(game.time * 2.0f) & 1) text(320, 330, 0.85f, ALIGN_CENTER, 0xffffffff, "PRESS START");
    char buf[80], money[40];
    if(save.best_total) {
        static const char *const grades[6] = {"D", "C", "B", "A", "S", "S+"};
        score_money(money, sizeof(money), save.best_total);
        snprintf(buf, sizeof(buf), "BIGGEST LUNCH  %s  (%s)", money, grades[save.grade > 5 ? 5 : save.grade]);
        text(320, 378, 0.5f, ALIGN_CENTER, 0xffffd650, buf);
    }
    text(320, 448, 0.36f, ALIGN_CENTER, 0xc0ffffff, "BUILD " KL_GIT_SHA);
}

void hud_draw_pause(void) {
    static const char *const items[PAUSE_ITEMS] = {
        [PAUSE_RESUME] = "KEEP EATING", [PAUSE_RESTART] = "START AGAIN", [PAUSE_MUSIC] = "MUSIC", [PAUSE_FPS] = "SHOW FPS",
        [PAUSE_QUIT] = "QUIT TO TITLE"};
    ui_dim();
    ui_panel(180, 108, 460, 384);
    float y = ui_title(180, 108, 460, "PAUSED") + 24;
    for(int i = 0; i < PAUSE_ITEMS; i++) {
        bool sel = i == game.pause_sel;
        ui_row(196, y - 18, 444, y + 18, sel);
        if(i == PAUSE_FPS || i == PAUSE_MUSIC) {
            /* A setting: its name, and what it is set to between arrows. */
            bool on = i == PAUSE_FPS ? game.show_fps : !(save.options & SAVE_OPT_NO_MUSIC);
            text(212, y, 0.62f, ALIGN_LEFT, ui_col(sel ? UI_ITEM_SEL : UI_ITEM), items[i]);
            text(400, y, 0.62f, ALIGN_CENTER, ui_col(sel ? UI_VALUE_SEL : on ? UI_GOOD : UI_DIM), on ? "ON" : "OFF");
            if(sel) ui_arrows(366, 434, y, 0.5f, ui_col(UI_VALUE_SEL));
        }
        else text(320, y, 0.62f, ALIGN_CENTER, ui_col(sel ? UI_ITEM_SEL : UI_ITEM), items[i]);
        y += 44;
    }
    ui_hint("A: OK   START: BACK");
}

void hud_draw_results(void) {
    static const char *const grades[6] = {"D", "C", "B", "A", "S", "S+"};
    static const char *const verdict[6] = {"A LIGHT SNACK", "A PACKED LUNCH", "A SQUARE MEAL", "THREE COURSES", "ALL YOU CAN EAT",
                                           "EXTINCTION BUFFET"};
    char buf[64], money[40];
    float t = game.time;
    int grade = score_grade(score.total);
    ui_panel(70, 54, 570, 418);
    float y = ui_title(70, 54, 570, "THE BILL") + 26;
    const struct { const char *label; int kind; uint64_t v; } rows[6] = {
        {"DAMAGE", 0, score.total}, {"BIGGEST COMBO", 0, score.best_combo}, {"TOP MULTIPLIER", 1, (uint64_t)score.best_mult},
        {"BUILDINGS FLATTENED", 2, (uint64_t)score.buildings}, {"VEHICLES WRECKED", 3, (uint64_t)score.vehicles},
        {"THINGS EATEN", 3, (uint64_t)score.eaten}};
    for(int i = 0; i < 6; i++) {
        if(t < 0.4f + 0.25f * (float)i) break;
        text(96, y, 0.5f, ALIGN_LEFT, ui_col(UI_LABEL), rows[i].label);
        if(rows[i].kind == 0) score_money(buf, sizeof(buf), rows[i].v);
        else if(rows[i].kind == 1) snprintf(buf, sizeof(buf), "x%d", (int)rows[i].v);
        else if(rows[i].kind == 2) snprintf(buf, sizeof(buf), "%d OF %d", (int)rows[i].v, city_total());
        else snprintf(buf, sizeof(buf), "%d", (int)rows[i].v);
        text(544, y, i == 0 ? 0.66f : 0.5f, ALIGN_RIGHT, i == 0 ? ui_col(UI_ACCENT) : ui_col(UI_VALUE), buf);
        y += i == 0 ? 36 : 30;
    }
    if(t > 2.2f) {
        float u = t - 2.2f, k = u < 0.2f ? u / 0.2f * 1.3f : u < 0.35f ? 1.3f - (u - 0.2f) * 2.0f : 1.0f;
        ui_well(96, 300, 544, 392);
        text(150, 346, 2.0f * k, ALIGN_CENTER, grade >= 4 ? 0xffff5868 : grade >= 2 ? 0xffffd650 : 0xffffffff, grades[grade]);
        text(380, 332, 0.62f, ALIGN_CENTER, 0xffffffff, verdict[grade]);
        score_money(money, sizeof(money), save.best_total);
        if(score.total >= save.best_total && score.total) text(380, 366, 0.5f, ALIGN_CENTER, (int)(t * 4) & 1 ? 0xff7ee878 : 0xffffffff, "A NEW RECORD!");
        else {
            snprintf(buf, sizeof(buf), "RECORD  %s", money);
            text(380, 366, 0.42f, ALIGN_CENTER, ui_col(UI_DIM), buf);
        }
        ui_hint("A: ANOTHER LUNCH   B: TITLE");
    }
}
