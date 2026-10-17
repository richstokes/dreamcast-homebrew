/* HUD and 2D text. The font atlas holds white glyphs with a navy outline,
   tinted by vertex colour; the countdown lettering is pre-styled. */
#include "hud.h"
#include "kart.h"
#include "render.h"
#include "track.h"
#include "fx.h"
#include "texture_assets.h"
#include "font_metrics.h"

const char *const kart_names[MAX_KARTS] = {"BUBBLES", "MANGO", "CORAL", "PEBBLE", "SUNNY", "YOU"};

static const pvr_poly_hdr_t *h_font, *h_big, *h_flat, *h_glow;
static char msg_text[32];
static uint32_t msg_col;
static float msg_t, msg_len;
static char lap_text[48];
static float lap_t;
static bool lap_best;
static float hud_time;

void hud_init(void) {
    h_font = r_header(TEX_FONT, HF_TR | HF_NOFOG | HF_NOZTEST | HF_CLAMP);
    h_big = r_header(TEX_BIGTEXT, HF_TR | HF_NOFOG | HF_NOZTEST | HF_CLAMP);
    h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG | HF_NOZTEST);
}

void hud_reset(void) {
    msg_t = lap_t = 0;
}

void hud_update(float dt) {
    hud_time += dt;
    if(msg_t > 0) msg_t -= dt;
    if(lap_t > 0) lap_t -= dt;
}

float hud_text_width(const char *s, float scale) {
    float w = 0;
    for(; *s; s++) {
        unsigned c = (unsigned char)*s;
        if(c < 32 || c > 127) c = '?';
        w += font_advance[c - 32] * scale;
    }
    return w;
}

void hud_text(float x, float y, float scale, int align, uint32_t col, const char *s) {
    float w = hud_text_width(s, scale);
    if(align == ALIGN_CENTER) x -= w * 0.5f;
    else if(align == ALIGN_RIGHT) x -= w;
    r_hdr(h_font);
    const float cu = (float)FONT_CELL_W / FONT_TEX_W, cv = (float)FONT_CELL_H / FONT_TEX_H;
    for(; *s; s++) {
        unsigned c = (unsigned char)*s;
        if(c < 32 || c > 127) c = '?';
        unsigned i = c - 32;
        if(c != ' ') {
            float u0 = (float)(i % FONT_COLS) * cu, v0 = (float)(i / FONT_COLS) * cv;
            r_quad2d(x, y, x + FONT_CELL_W * scale, y + FONT_CELL_H * scale, u0, v0, u0 + cu, v0 + cv, col, 20.0f);
        }
        x += font_advance[i] * scale;
    }
}

void hud_panel(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom, float z) {
    r_hdr(h_flat);
    r_quad2d_grad(x0, y0, x1, y1, top, bottom, z);
    /* Bright rim along the top edge. */
    r_quad2d_grad(x0, y0, x1, y0 + 3, color_alpha(0xffffffff, 0.55f), color_alpha(0xffffffff, 0.0f), z + 0.1f);
}

void hud_bigtext(int which, float cx, float cy, float scale, uint32_t col) {
    float u0, v0, u1, v1, w, h;
    if(which < 4) {
        u0 = which * 0.25f; v0 = 0; u1 = u0 + 0.25f; v1 = 0.5f;
        w = h = 128;
    }
    else {
        u0 = 0; v0 = 0.5f; u1 = 1; v1 = 1;
        w = 512; h = 128;
    }
    w *= scale;
    h *= scale;
    r_hdr(h_big);
    r_quad2d(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, u0, v0, u1, v1, col, 22.0f);
}

void hud_format_time(char *buf, float t) {
    if(t < 0) t = 0;
    int cs = (int)(t * 100.0f + 0.5f);
    sprintf(buf, "%d'%02d\"%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

const char *hud_ordinal(int place) {
    static const char *const s[] = {"th", "st", "nd", "rd", "th", "th", "th"};
    return s[place >= 0 && place <= 6 ? place : 0];
}

uint32_t hud_place_color(int place) {
    switch(place) {
        case 1: return argb(255, 255, 214, 40);
        case 2: return argb(255, 210, 225, 240);
        case 3: return argb(255, 240, 150, 80);
        default: return argb(255, 120, 200, 255);
    }
}

void hud_message(const char *text, uint32_t col, float seconds) {
    strncpy(msg_text, text, sizeof(msg_text) - 1);
    msg_text[sizeof(msg_text) - 1] = 0;
    msg_col = col;
    msg_t = msg_len = seconds;
}

void hud_lap_popup(int lap, float time, bool best) {
    char t[20];
    hud_format_time(t, time);
    snprintf(lap_text, sizeof(lap_text), "LAP %d  %s", lap, t);
    lap_t = 2.6f;
    lap_best = best;
}

static void draw_minimap(void) {
    const float cx = 98, cy = 402, sc = 0.40f;
    r_hdr(h_flat);
    /* Road band. */
    for(int i = 0; i < TRACK_SAMPLES; i += 4) {
        const TrackSample *a = &track.smp[i], *b = &track.smp[(i + 4) % TRACK_SAMPLES];
        float pts[8], uv[8] = {0};
        V3 pa0 = vmad(a->p, a->r, -9), pa1 = vmad(a->p, a->r, 9);
        V3 pb0 = vmad(b->p, b->r, -9), pb1 = vmad(b->p, b->r, 9);
        pts[0] = cx + pa0.x * sc; pts[1] = cy - pa0.z * sc;
        pts[2] = cx + pa1.x * sc; pts[3] = cy - pa1.z * sc;
        pts[4] = cx + pb0.x * sc; pts[5] = cy - pb0.z * sc;
        pts[6] = cx + pb1.x * sc; pts[7] = cy - pb1.z * sc;
        uint32_t c = a->section == SEC_BOARDWALK ? argb(200, 255, 220, 160) : argb(200, 255, 255, 255);
        uint32_t cols[4] = {c, c, c, c};
        r_quad2d_pts(pts, uv, cols, 18.0f);
    }
    /* Start line tick. */
    V3 s0 = track_point(0, -10), s1 = track_point(0, 10);
    r_quad2d(cx + s0.x * sc - 1, cy - s0.z * sc, cx + s1.x * sc + 1, cy - s1.z * sc, 0, 0, 1, 1, 0xffff3030, 18.2f);
    /* Karts: rivals first, the player on top with a pulse. */
    for(int pass = 0; pass < 2; pass++) {
        for(int i = 0; i < kart_count; i++) {
            bool me = i == game.player;
            if(me != (pass == 1)) continue;
            const Kart *k = &karts[i];
            float x = cx + k->pos.x * sc, y = cy - k->pos.z * sc;
            float r = me ? 5.5f + sinf(hud_time * 8) * 0.8f : 4.0f;
            r_quad2d(x - r - 1.5f, y - r - 1.5f, x + r + 1.5f, y + r + 1.5f, 0, 0, 1, 1, 0xff102040, 18.4f + pass);
            r_quad2d(x - r, y - r, x + r, y + r, 0, 0, 1, 1, me ? 0xffffffff : (k->chao | 0xff000000), 18.5f + pass);
        }
    }
}

void hud_draw_race(void) {
    const Kart *p = &karts[game.player];
    char buf[48];
    /* Lap counter. */
    int lap = p->lap < 1 ? 1 : (p->lap > RACE_LAPS ? RACE_LAPS : p->lap);
    hud_panel(24, 22, 182, 70, argb(150, 20, 60, 160), argb(150, 10, 30, 90), 15);
    hud_text(34, 25, 0.75f, ALIGN_LEFT, argb(255, 255, 220, 80), "LAP");
    snprintf(buf, sizeof(buf), "%d/%d", lap, RACE_LAPS);
    hud_text(90, 22, 1.05f, ALIGN_LEFT, 0xffffffff, buf);

    /* Race time. */
    hud_panel(436, 22, 616, 70, argb(150, 20, 60, 160), argb(150, 10, 30, 90), 15);
    hud_format_time(buf, p->finished ? p->finish_time : game.race_time);
    hud_text(606, 25, 1.0f, ALIGN_RIGHT, 0xffffffff, buf);
    if(game.mode == MODE_TT && p->best_lap > 0) {
        char t[20];
        hud_format_time(t, p->best_lap);
        snprintf(buf, sizeof(buf), "BEST %s", t);
        hud_text(606, 72, 0.6f, ALIGN_RIGHT, argb(255, 255, 230, 120), buf);
    }

    /* Position. */
    if(game.mode == MODE_GP && p->place > 0) {
        uint32_t c = hud_place_color(p->place);
        snprintf(buf, sizeof(buf), "%d", p->place);
        float pulse = 1.0f + 0.04f * sinf(hud_time * 6.0f);
        hud_text(560, 362, 2.4f * pulse, ALIGN_RIGHT, c, buf);
        hud_text(562, 386, 1.0f, ALIGN_LEFT, c, hud_ordinal(p->place));
    }

    draw_minimap();

    /* Mini-turbo charge gauge while drifting. */
    if(p->drift == DRIFT_ACTIVE) {
        float x0 = 240, x1 = 400, y = 440;
        float f = clampf(p->charge / 3.1f, 0, 1);
        hud_panel(x0 - 4, y - 4, x1 + 4, y + 14, argb(140, 10, 20, 60), argb(140, 10, 20, 60), 15);
        uint32_t c = fx_stage_color(p->stage);
        if(p->stage == 0) c = argb(255, 240, 240, 240);
        r_hdr(h_flat);
        r_quad2d_grad(x0, y, x0 + (x1 - x0) * f, y + 10, color_lerp(c, 0xffffffff, 0.5f), c, 16);
        for(int t = 1; t <= 2; t++) {
            float tx = x0 + (x1 - x0) * (t == 1 ? 0.85f / 3.1f : 1.9f / 3.1f);
            r_quad2d(tx - 1, y - 2, tx + 1, y + 12, 0, 0, 1, 1, 0xc0ffffff, 16.5f);
        }
    }

    /* Messages. */
    if(msg_t > 0) {
        float age = msg_len - msg_t;
        float slide = age < 0.3f ? (0.3f - age) / 0.3f : (msg_t < 0.3f ? -(0.3f - msg_t) / 0.3f : 0);
        float x = 320 + slide * 500;
        float s = 1.6f + 0.08f * sinf(hud_time * 10);
        hud_text(x, 150, s, ALIGN_CENTER, msg_col, msg_text);
    }
    if(lap_t > 0) {
        float a = clampf(lap_t / 0.4f, 0, 1);
        hud_text(320, 212, 0.9f, ALIGN_CENTER, color_alpha(lap_best ? argb(255, 255, 230, 90) : 0xffffffff, a), lap_text);
        if(lap_best) hud_text(320, 244, 0.7f, ALIGN_CENTER, color_alpha(argb(255, 120, 255, 160), a), "BEST LAP!");
    }
    if(!pad.connected && ((int)(hud_time * 2) & 1))
        hud_text(320, 300, 0.8f, ALIGN_CENTER, 0xffff8080, "CONNECT A CONTROLLER");
}

void hud_draw_countdown(void) {
    float t = game.countdown;
    if(t < 0) return;
    int which = (int)t;            /* 0: "3", 1: "2", 2: "1", 3: GO */
    if(which > 3) return;
    float f = t - (float)which;
    float scale = which < 3 ? 1.5f - 0.5f * clampf(f * 4.0f, 0, 1) : 1.4f + f * 0.6f;
    float alpha = which < 3 ? clampf((1.0f - f) * 3.0f, 0, 1) : clampf(1.0f - (f - 0.5f) * 2.0f, 0, 1);
    float bounce = which < 3 ? -18.0f * sinf(clampf(f * 4, 0, 1) * DK_PI) : 0;
    hud_bigtext(which, 320, 200 + bounce, scale, color_alpha(0xffffffff, alpha));
}

void hud_draw_results(void) {
    float a = clampf(game.time / 0.5f, 0, 1);
    float x0 = 120 + (1 - a) * 400;
    hud_panel(x0, 60, x0 + 400, 420, argb(220, 30, 90, 200), argb(220, 10, 30, 100), 15);
    hud_text(x0 + 200, 70, 1.1f, ALIGN_CENTER, argb(255, 255, 220, 60),
             game.mode == MODE_GP ? "RESULTS" : "TIME TRIAL");
    char buf[48], t[20];
    if(game.mode == MODE_GP) {
        for(int pos = 0; pos < kart_count; pos++) {
            int i = game.finish_order[pos];
            const Kart *k = &karts[i];
            float y = 122 + pos * 42;
            bool me = i == game.player;
            if(me) {
                r_hdr(h_flat);
                float glow = 0.25f + 0.15f * sinf(hud_time * 5);
                r_quad2d(x0 + 12, y - 2, x0 + 388, y + 36, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
            }
            snprintf(buf, sizeof(buf), "%d%s", pos + 1, hud_ordinal(pos + 1));
            hud_text(x0 + 24, y, 0.8f, ALIGN_LEFT, hud_place_color(pos + 1), buf);
            r_hdr(h_flat);
            r_quad2d(x0 + 92, y + 8, x0 + 112, y + 28, 0, 0, 1, 1, 0xff102040, 16.5f);
            r_quad2d(x0 + 94, y + 10, x0 + 110, y + 26, 0, 0, 1, 1, k->chao ? (k->chao | 0xff000000) : 0xff8cc8f0, 16.6f);
            hud_text(x0 + 120, y + 3, 0.68f, ALIGN_LEFT, me ? argb(255, 255, 255, 140) : 0xffffffff, kart_names[i]);
            hud_format_time(t, k->finish_time);
            hud_text(x0 + 384, y + 3, 0.68f, ALIGN_RIGHT, 0xffffffff, k->finished ? t : "--'--\"--");
        }
    }
    else {
        const Kart *k = &karts[game.player];
        for(int l = 0; l < RACE_LAPS; l++) {
            hud_format_time(t, k->lap_times[l]);
            snprintf(buf, sizeof(buf), "LAP %d", l + 1);
            bool best = k->lap_times[l] == k->best_lap;
            hud_text(x0 + 40, 130 + l * 44, 0.9f, ALIGN_LEFT, 0xffffffff, buf);
            hud_text(x0 + 360, 130 + l * 44, 0.9f, ALIGN_RIGHT, best ? argb(255, 255, 220, 60) : 0xffffffff, t);
        }
        hud_format_time(t, k->finish_time);
        hud_text(x0 + 40, 280, 1.0f, ALIGN_LEFT, argb(255, 120, 230, 255), "TOTAL");
        hud_text(x0 + 360, 280, 1.0f, ALIGN_RIGHT, argb(255, 120, 230, 255), t);
    }
    if(game.time > 1.0f && ((int)(hud_time * 2.5f) & 1) == 0)
        hud_text(x0 + 200, 380, 0.7f, ALIGN_CENTER, 0xffffffff, "A: RACE AGAIN    B: TITLE");
}

void hud_draw_pause(void) {
    static const char *const items[] = {"CONTINUE", "RESTART", "QUIT"};
    r_hdr(h_flat);
    r_quad2d(0, 0, 640, 480, 0, 0, 1, 1, argb(120, 0, 10, 40), 14);
    hud_panel(200, 140, 440, 340, argb(230, 30, 90, 200), argb(230, 10, 30, 100), 15);
    hud_text(320, 152, 1.2f, ALIGN_CENTER, argb(255, 255, 220, 60), "PAUSE");
    for(int i = 0; i < 3; i++) {
        bool sel = i == game.pause_sel;
        float s = sel ? 0.95f + 0.05f * sinf(hud_time * 8) : 0.85f;
        hud_text(320, 214 + i * 38, s, ALIGN_CENTER, sel ? argb(255, 255, 255, 120) : argb(255, 170, 200, 240), items[i]);
    }
}
