/* HUD and 2D text. The font atlas holds white glyphs with a navy outline,
   tinted by vertex colour; the countdown lettering is pre-styled. */
#include "hud.h"
#include "audio.h"
#include "kart.h"
#include "render.h"
#include "track.h"
#include "fx.h"
#include "items.h"
#include "perfhud.h"
#include "texture_assets.h"
#include "font_metrics.h"

static const pvr_poly_hdr_t *h_font, *h_big, *h_flat, *h_glow;
static struct {
    char msg_text[32];
    uint32_t msg_col;
    float msg_t, msg_len;
    char lap_text[48];
    float lap_t;
    bool lap_best;
} pm[MAX_PLAYERS];
static float hud_time;

void hud_init(void) {
    h_font = r_header(TEX_FONT, HF_TR | HF_NOFOG | HF_NOZTEST | HF_CLAMP);
    h_big = r_header(TEX_BIGTEXT, HF_TR | HF_NOFOG | HF_NOZTEST | HF_CLAMP);
    h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG | HF_NOZTEST);
}

void hud_reset(void) {
    for(int p = 0; p < MAX_PLAYERS; p++) pm[p].msg_t = pm[p].lap_t = 0;
}

void hud_update(float dt) {
    hud_time += dt;
    for(int p = 0; p < MAX_PLAYERS; p++) {
        if(pm[p].msg_t > 0) pm[p].msg_t -= dt;
        if(pm[p].lap_t > 0) pm[p].lap_t -= dt;
    }
}

float hud_text_width(const char *s, float scale) {
    float w = 0;
    for(; *s; s++) {
        unsigned c = (unsigned char)*s;
        if(c < 32 || c > 127) c = '?';
        w += font_glyphs[c - 32].adv * scale;
    }
    return w;
}

void hud_text(float x, float y, float scale, int align, uint32_t col, const char *s) {
    float w = hud_text_width(s, scale);
    if(align == ALIGN_CENTER) x -= w * 0.5f;
    else if(align == ALIGN_RIGHT) x -= w;
    r_hdr(h_font);
    const float du = 1.0f / FONT_TEX_W, dv = (float)FONT_ROW_H / FONT_TEX_H;
    for(; *s; s++) {
        unsigned c = (unsigned char)*s;
        if(c < 32 || c > 127) c = '?';
        const dk_glyph_t *g = &font_glyphs[c - 32];
        if(g->w) {
            float x0 = x + g->ox * scale, u0 = g->x * du, v0 = g->row * dv;
            r_quad2d(x0, y, x0 + g->w * scale, y + FONT_ROW_H * scale, u0, v0, u0 + g->w * du, v0 + dv, col, 20.0f);
        }
        x += g->adv * scale;
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
        u0 = bigtext_cells[which][0] / 512.0f; v0 = 0; v1 = 0.5f;
        w = bigtext_cells[which][1];
        u1 = u0 + w / 512.0f;
        h = 128;
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
    sprintf(buf, "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
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

void hud_message(int player, const char *text, uint32_t col, float seconds) {
    strncpy(pm[player].msg_text, text, sizeof(pm[player].msg_text) - 1);
    pm[player].msg_text[sizeof(pm[player].msg_text) - 1] = 0;
    pm[player].msg_col = col;
    pm[player].msg_t = pm[player].msg_len = seconds;
}

void hud_lap_popup(int player, int lap, float time, bool best) {
    char t[20];
    hud_format_time(t, time);
    snprintf(pm[player].lap_text, sizeof(pm[player].lap_text), "LAP %d  %s", lap, t);
    pm[player].lap_t = 2.6f;
    pm[player].lap_best = best;
}

void hud_draw_track_map(float cx, float cy, float sc, float z, bool show_karts) {
    r_hdr(h_flat);
    /* Road band; a jump is drawn last, in orange, bridging the crossing. */
    for(int pass = 0; pass < 2; pass++) {
        for(int i = 0; i < TRACK_SAMPLES; i += 4) {
            const TrackSample *a = &track.smp[i], *b = &track.smp[(i + 4) % TRACK_SAMPLES];
            bool jump = fabsf(track_jump_pos(a->s + track.ds * 2.0f)) < 50.0f;
            if(jump != (pass == 1)) continue;
            float pts[8], uv[8] = {0};
            V3 pa0 = vmad(a->p, a->r, -9), pa1 = vmad(a->p, a->r, 9);
            V3 pb0 = vmad(b->p, b->r, -9), pb1 = vmad(b->p, b->r, 9);
            pts[0] = cx + pa0.x * sc; pts[1] = cy - pa0.z * sc;
            pts[2] = cx + pa1.x * sc; pts[3] = cy - pa1.z * sc;
            pts[4] = cx + pb0.x * sc; pts[5] = cy - pb0.z * sc;
            pts[6] = cx + pb1.x * sc; pts[7] = cy - pb1.z * sc;
            uint32_t c = jump ? argb(230, 255, 170, 40) :
                         a->section == SEC_BOARDWALK ? argb(200, 255, 220, 160) : argb(200, 255, 255, 255);
            uint32_t cols[4] = {c, c, c, c};
            r_quad2d_pts(pts, uv, cols, z + pass * 0.05f);
        }
    }
    /* Start line tick. */
    V3 s0 = track_point(0, -10), s1 = track_point(0, 10);
    r_quad2d(cx + s0.x * sc - 1, cy - s0.z * sc, cx + s1.x * sc + 1, cy - s1.z * sc, 0, 0, 1, 1, 0xffff3030, z + 0.2f);
    if(!show_karts) return;
    /* Karts: rivals first, humans on top with a pulse and a white rim. */
    for(int pass = 0; pass < 2; pass++) {
        for(int i = 0; i < kart_count; i++) {
            bool me = kart_is_human(i);
            if(me != (pass == 1)) continue;
            const Kart *k = &karts[i];
            if(k->gone) continue;
            float x = cx + k->pos.x * sc, y = cy - k->pos.z * sc;
            float r = me ? 5.0f + sinf(hud_time * 8) * 0.8f : 3.6f;
            r_quad2d(x - r - 1.5f, y - r - 1.5f, x + r + 1.5f, y + r + 1.5f, 0, 0, 1, 1,
                     me ? 0xffffffff : 0xff102040, z + 0.4f + pass);
            r_quad2d(x - r, y - r, x + r, y + r, 0, 0, 1, 1, k->ui_color | 0xff000000, z + 0.5f + pass);
        }
    }
}

void hud_draw_race(int pi, float y0, float y1) {
    const Kart *p = &karts[game.pl[pi].kart];
    bool split = y1 - y0 < 400.0f;
    float k = split ? 0.72f : 1.0f;       /* HUD scale */
    char buf[48];
    /* Lap counter. */
    int lap = p->lap < 1 ? 1 : (p->lap > game.laps ? game.laps : p->lap);
    float lx = split ? 14 : 24, ly = y0 + (split ? 8 : 22);
    hud_panel(lx, ly, lx + 158 * k, ly + 48 * k, argb(150, 20, 60, 160), argb(150, 10, 30, 90), 15);
    hud_text(lx + 10 * k, ly + 3 * k, 0.75f * k, ALIGN_LEFT, argb(255, 255, 220, 80), "LAP");
    snprintf(buf, sizeof(buf), "%d/%d", lap, game.laps);
    hud_text(lx + 66 * k, ly, 1.05f * k, ALIGN_LEFT, 0xffffffff, buf);
    if(split) {
        /* Player tag in their colour. */
        hud_text(lx + 166 * k, ly + 8 * k, 0.75f * k, ALIGN_LEFT, p->ui_color | 0xff000000, pi ? "P2" : "P1");
    }
    else {
        /* Race time. */
        hud_panel(436, 22, 616, 70, argb(150, 20, 60, 160), argb(150, 10, 30, 90), 15);
        hud_format_time(buf, p->finished ? p->finish_time : game.race_time);
        hud_text(606, 25, 1.0f, ALIGN_RIGHT, 0xffffffff, buf);
    }
    /* The held power-up, as a small gem beside the lap counter. */
    items_draw_hud(pi, split ? lx + 236 * k : lx + 158 * k + 30, ly + 23 * k, k);
    if(game.mode == MODE_TT && p->best_lap > 0 && game.num_players == 1) {
        char t[20];
        hud_format_time(t, p->best_lap);
        snprintf(buf, sizeof(buf), "BEST %s", t);
        hud_text(606, 72, 0.6f, ALIGN_RIGHT, argb(255, 255, 230, 120), buf);
    }

    /* Position (also between the two players in versus). */
    if(game.mode != MODE_TT || game.num_players > 1) {
        uint32_t c = hud_place_color(p->place);
        snprintf(buf, sizeof(buf), "%d", p->place);
        float pulse = 1.0f + 0.04f * sinf(hud_time * 6.0f);
        /* Split: top view's place in its top-right corner, bottom view's in
           its bottom-right, leaving the divider for the clock and map. */
        float px = split ? 584 : 560, py = !split ? 362 : (y0 < 1.0f ? y0 + 4 : y1 - 72);
        hud_text(px, py, 2.4f * k * pulse, ALIGN_RIGHT, c, buf);
        hud_text(px + 2, py + 24 * k, 1.0f * k, ALIGN_LEFT, c, hud_ordinal(p->place));
    }

    if(!split) hud_draw_track_map(98, 402, 0.40f, 18.0f, true);

    /* Mini-turbo charge gauge while drifting. */
    if(p->drift == DRIFT_ACTIVE) {
        float gw = split ? 120 : 160, x0 = 320 - gw * 0.5f, x1 = 320 + gw * 0.5f, y = split ? y1 - 22 : 440;
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
    if(pm[pi].msg_t > 0) {
        float age = pm[pi].msg_len - pm[pi].msg_t;
        float slide = age < 0.3f ? (0.3f - age) / 0.3f : (pm[pi].msg_t < 0.3f ? -(0.3f - pm[pi].msg_t) / 0.3f : 0);
        float x = 320 + slide * 500;
        float s = (1.6f + 0.08f * sinf(hud_time * 10)) * k;
        hud_text(x, y0 + (split ? 70 : 150), s, ALIGN_CENTER, pm[pi].msg_col, pm[pi].msg_text);
    }
    if(pm[pi].lap_t > 0) {
        float a = clampf(pm[pi].lap_t / 0.4f, 0, 1);
        float ly2 = y0 + (split ? 118 : 212);
        hud_text(320, ly2, 0.9f * k, ALIGN_CENTER, color_alpha(pm[pi].lap_best ? argb(255, 255, 230, 90) : 0xffffffff, a),
                 pm[pi].lap_text);
        if(pm[pi].lap_best)
            hud_text(320, ly2 + 32 * k, 0.7f * k, ALIGN_CENTER, color_alpha(argb(255, 120, 255, 160), a), "BEST LAP!");
    }
    /* Wrong way: a red banner, its lettering flashing white and yellow. */
    if(game.pl[pi].wrong_way_t > 1.0f && !p->finished) {
        float a = clampf((game.pl[pi].wrong_way_t - 1.0f) / 0.2f, 0, 1);
        float cy = y0 + (split ? 96 : 104), w = 140 * k, h = 26 * k;
        hud_panel(320 - w, cy - h, 320 + w, cy + h, color_alpha(argb(255, 210, 30, 30), 0.85f * a),
                  color_alpha(argb(255, 110, 0, 0), 0.85f * a), 15);
        uint32_t col = ((int)(hud_time * 4) & 1) ? 0xffffffff : argb(255, 255, 220, 60);
        hud_text(320, cy - 23 * k, 1.1f * k, ALIGN_CENTER, color_alpha(col, a), "WRONG WAY");
    }
    const Pad *pd = input_pad(pi);
    if(!pd->connected && ((int)(hud_time * 2) & 1)) {
        snprintf(buf, sizeof(buf), "CONNECT CONTROLLER %d", pi + 1);
        hud_text(320, y0 + (y1 - y0) * 0.62f, 0.8f * k, ALIGN_CENTER, 0xffff8080, buf);
    }
}

void hud_draw_split_overlay(void) {
    /* Divider with the shared race clock in the middle and the map on the
       left, straddling both views. */
    r_hdr(h_flat);
    r_quad2d_grad(0, 224, 640, 256, 0xff1c3a8a, 0xff0c1a48, 17.0f);
    r_quad2d(0, 224, 640, 226, 0, 0, 1, 1, 0xfff0f0ff, 17.05f);
    r_quad2d(0, 254, 640, 256, 0, 0, 1, 1, 0xfff0f0ff, 17.05f);
    char buf[24];
    hud_panel(250, 224, 390, 256, argb(220, 20, 60, 160), argb(220, 10, 30, 90), 17.2f);
    hud_format_time(buf, game.race_time);
    hud_text(320, 223, 0.8f, ALIGN_CENTER, 0xffffffff, buf);
    hud_panel(470, 208, 616, 272, argb(150, 10, 30, 90), argb(150, 10, 30, 90), 17.2f);
    hud_draw_track_map(543, 240, 0.40f, 18.0f, true);
}

void hud_draw_countdown(float y0, float y1) {
    float t = game.countdown;
    float vk = (y1 - y0) < 400.0f ? 0.6f : 1.0f;
    float cy = y0 + (y1 - y0) * (200.0f / 480.0f);
    if(t < 0) return;
    int which = (int)t;            /* 0: "3", 1: "2", 2: "1", 3: GO */
    if(which > 3) return;
    float f = t - (float)which;
    float scale = which < 3 ? 1.5f - 0.5f * clampf(f * 4.0f, 0, 1) : 1.4f + f * 0.6f;
    float alpha = which < 3 ? clampf((1.0f - f) * 3.0f, 0, 1) : clampf(1.0f - (f - 0.5f) * 2.0f, 0, 1);
    float bounce = which < 3 ? -18.0f * vk * sinf(clampf(f * 4, 0, 1) * DK_PI) : 0;
    hud_bigtext(which, 320, cy + bounce, scale * vk, color_alpha(0xffffffff, alpha));
}

void hud_trophy(float x, float y, float s, uint32_t col) {
    uint32_t dark = color_scale(col, 0.7f);
    r_hdr(h_flat);
    r_tri2d(x - 7 * s, y, x + 7 * s, y, x, y + 10 * s, col, col, dark, 16.8f);
    r_quad2d(x - 7 * s, y - 1 * s, x + 7 * s, y + 3 * s, 0, 0, 1, 1, col, 16.8f);
    r_quad2d(x - 10 * s, y + 1 * s, x - 7 * s, y + 5 * s, 0, 0, 1, 1, dark, 16.75f);
    r_quad2d(x + 7 * s, y + 1 * s, x + 10 * s, y + 5 * s, 0, 0, 1, 1, dark, 16.75f);
    r_quad2d(x - 1.5f * s, y + 8 * s, x + 1.5f * s, y + 13 * s, 0, 0, 1, 1, dark, 16.8f);
    r_quad2d(x - 5 * s, y + 13 * s, x + 5 * s, y + 16 * s, 0, 0, 1, 1, col, 16.8f);
}

void hud_swatch(float x, float y, uint32_t paint1, uint32_t ui) {
    r_hdr(h_flat);
    r_quad2d(x - 1, y - 1, x + 25, y + 15, 0, 0, 1, 1, 0xff0c1430, 16.4f);
    r_quad2d(x, y, x + 12, y + 14, 0, 0, 1, 1, (paint1 ? paint1 : 0xffe02828) | 0xff000000, 16.5f);
    r_quad2d(x + 12, y, x + 24, y + 14, 0, 0, 1, 1, ui | 0xff000000, 16.5f);
}

void hud_draw_results(void) {
    float a = clampf(game.time / 0.5f, 0, 1);
    float x0 = 100 + (1 - a) * 440, x1 = x0 + 440, cx = (x0 + x1) * 0.5f;
    hud_panel(x0, 60, x1, 420, argb(220, 30, 90, 200), argb(220, 10, 30, 100), 15);
    hud_text(cx, 70, 1.1f, ALIGN_CENTER, argb(255, 255, 220, 60),
             game.mode != MODE_TT ? "RESULTS" : game.num_players > 1 ? "VERSUS" : "TIME TRIAL");
    char buf[48], t[20];
    if(game.mode != MODE_TT || game.num_players > 1) {
        for(int pos = 0; pos < kart_count; pos++) {
            int i = game.finish_order[pos];
            const Kart *k = &karts[i];
            float y = 122 + pos * 40;
            bool me = kart_is_human(i);
            if(me) {
                r_hdr(h_flat);
                float glow = 0.25f + 0.15f * sinf(hud_time * 5);
                r_quad2d(x0 + 12, y - 2, x1 - 12, y + 36, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
            }
            snprintf(buf, sizeof(buf), "%d%s", pos + 1, hud_ordinal(pos + 1));
            hud_text(x0 + 24, y, 0.8f, ALIGN_LEFT, hud_place_color(pos + 1), buf);
            hud_swatch(x0 + 92, y + 10, k->paint1, k->ui_color);
            hud_text(x0 + 128, y + 3, 0.68f, ALIGN_LEFT, me ? argb(255, 255, 255, 140) : 0xffffffff, k->name);
            hud_format_time(t, k->finish_time);
            hud_text(x1 - 24, y + 3, 0.68f, ALIGN_RIGHT, 0xffffffff, k->finished ? t : "--'--\"--");
        }
    }
    else {
        const Kart *k = &karts[game.pl[0].kart];
        /* Three laps in one roomy column; five tighter; ten in two columns,
           numbered without "LAP" to fit. */
        int cols = game.laps > 5 ? 2 : 1, rows = (game.laps + cols - 1) / cols;
        float pitch = rows > 3 ? 36.0f : 44.0f, scale = cols > 1 ? 0.62f : rows > 3 ? 0.8f : 0.9f;
        float col_w = 340.0f / cols;
        for(int l = 0; l < game.laps; l++) {
            hud_format_time(t, k->lap_times[l]);
            snprintf(buf, sizeof(buf), cols > 1 ? "%d" : "LAP %d", l + 1);
            bool best = k->lap_times[l] == k->best_lap;
            float lx = x0 + 50 + (l / rows) * (col_w + 20), ly = 130 + (l % rows) * pitch;
            hud_text(lx, ly, scale, ALIGN_LEFT, cols > 1 ? argb(255, 150, 200, 255) : 0xffffffff, buf);
            hud_text(lx + col_w - (cols > 1 ? 20 : 0), ly, scale, ALIGN_RIGHT, best ? argb(255, 255, 220, 60) : 0xffffffff, t);
        }
        float total_y = rows > 3 ? 130 + rows * pitch + 14 : 280;
        hud_format_time(t, k->finish_time);
        hud_text(x0 + 50, total_y, 1.0f, ALIGN_LEFT, argb(255, 120, 230, 255), "TOTAL");
        hud_text(x1 - 50, total_y, 1.0f, ALIGN_RIGHT, argb(255, 120, 230, 255), t);
    }
    /* What next: left/right picks, A goes. */
    static const char *const next[RESULTS_ITEMS] = {"RACE AGAIN", "NEW TRACK", "TITLE"};
    float in = clampf(game.time - 0.6f, 0, 1);
    for(int i = 0; i < RESULTS_ITEMS; i++) {
        bool sel = i == game.results_sel;
        float x = x0 + 80 + i * 140, y = 376;
        if(sel) {
            r_hdr(h_flat);
            float glow = (0.16f + 0.08f * sinf(hud_time * 7)) * in;
            r_quad2d(x - 66, y - 4, x + 66, y + 30, 0, 0, 1, 1, color_alpha(0xffffffff, glow), 16);
        }
        float s = sel ? 0.56f + 0.03f * sinf(hud_time * 8) : 0.5f;
        uint32_t col = sel ? argb(255, 255, 230, 60) : argb(255, 200, 220, 255);
        hud_text(x, y + (sel ? 0 : 2), s, ALIGN_CENTER, color_alpha(col, in), next[i]);
    }
}

void hud_draw_pause(void) {
    const char *items[PAUSE_ITEMS] = {"CONTINUE", "RESTART", audio_music_enabled() ? "MUSIC: ON" : "MUSIC: OFF",
                                      perfhud_on ? "PERF: ON" : "PERF: OFF", "QUIT"};
    r_hdr(h_flat);
    r_quad2d(0, 0, 640, 480, 0, 0, 1, 1, argb(120, 0, 10, 40), 14);
    hud_panel(184, 140, 456, 402, argb(230, 30, 90, 200), argb(230, 10, 30, 100), 15);
    hud_text(320, 152, 1.2f, ALIGN_CENTER, argb(255, 255, 220, 60), "PAUSE");
    for(int i = 0; i < PAUSE_ITEMS; i++) {
        bool sel = i == game.pause_sel;
        float s = sel ? 0.95f + 0.05f * sinf(hud_time * 8) : 0.85f;
        hud_text(320, 210 + i * 36, s, ALIGN_CENTER, sel ? argb(255, 255, 255, 120) : argb(255, 170, 200, 240), items[i]);
    }
}
