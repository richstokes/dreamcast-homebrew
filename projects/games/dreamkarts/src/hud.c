/* HUD and 2D text. The font atlas (font_metrics.h, drawn by the engine's
   text module) holds white glyphs with a navy outline, tinted by vertex
   colour; the countdown lettering is pre-styled. */
#include "hud.h"
#include "audio.h"
#include "kart.h"
#include "engine/render.h"
#include "track.h"
#include "fx.h"
#include "items.h"
#include "perfhud.h"
#include "texture_assets.h"
#include "font_metrics.h"
#include "ui.h"
#include "save.h"

static const pvr_poly_hdr_t *h_font, *h_big, *h_flat, *h_glow;
static EngFont font;
static struct {
    char msg_text[32];
    uint32_t msg_col;
    float msg_t, msg_len;
    char lap_text[48];
    float lap_t;
    bool lap_best;
} pm[MAX_PLAYERS];
static float hud_time;
static char toast[64];           /* hud_toast's notice, shown while toast_t > 0 */
static float toast_t;
#define TOAST_SECS 2.4f

void hud_init(void) {
    h_font = r_header(TEX_FONT, HF_TR | HF_NOFOG | HF_NOZTEST | HF_CLAMP);
    _Static_assert(sizeof(dk_glyph_t) == sizeof(eng_glyph_t), "font_metrics.h glyph layout");
    font = (EngFont){h_font, (const eng_glyph_t *)font_glyphs, FONT_ROW_H, FONT_TEX_W, FONT_TEX_H, FONT_CAP_TOP,
                     FONT_CAP_BOTTOM};
    h_big = r_header(TEX_BIGTEXT, HF_TR | HF_NOFOG | HF_NOZTEST | HF_CLAMP);
    h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG | HF_NOZTEST);
}

void hud_reset(void) {
    for(int p = 0; p < MAX_PLAYERS; p++) pm[p].msg_t = pm[p].lap_t = 0;
}

void hud_update(float dt) {
    hud_time += dt;
    if(toast_t > 0) toast_t -= dt;
    for(int p = 0; p < MAX_PLAYERS; p++) {
        if(pm[p].msg_t > 0) pm[p].msg_t -= dt;
        if(pm[p].lap_t > 0) pm[p].lap_t -= dt;
    }
}

const EngFont *hud_font(void) {
    return &font;
}

float hud_text_width(const char *s, float scale) {
    return text_width(&font, s, scale);
}

void hud_text(float x, float y, float scale, int align, uint32_t col, const char *s) {
    text_draw(&font, x, y, scale, align, col, s);
}

void hud_text_c(float x, float cy, float scale, int align, uint32_t col, const char *s) {
    text_draw_c(&font, x, cy, scale, align, col, s);
}

void hud_letter_c(float cx, float cy, float scale, uint32_t col, char c) {
    text_letter_c(&font, cx, cy, scale, col, c);
}

void hud_panel(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom, float z) {
    (void)z;   /* the translucent list draws in submission order */
    /* Rounded, outlined and glossed like the menu panels, as faint as the
       panel itself: the race HUD's boxes, the spectator overlays and the
       intro band then match the menus. */
    float a = (float)(top >> 24) / 255.0f;
    uint32_t navy = color_alpha(argb(255, 14, 24, 64), a * 0.9f);
    ui_rrect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 9, navy, navy);
    ui_rrect(x0, y0, x1, y1, 7, top, bottom);
    ui_rrect(x0 + 2, y0 + 2, x1 - 2, y0 + (y1 - y0) * 0.45f, 6, color_alpha(0xffffffff, a * 0.22f),
             color_alpha(0xffffffff, a * 0.03f));
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

/* The maps are drawn round the world's origin at a scale that fits every
   course laid out within MAP_HX by MAP_HZ metres of it. One that reaches
   further (Bay City Hills) is centred on its own bounds and shrunk to
   that size instead. */
#define MAP_HX 180.0f
#define MAP_HZ 131.0f
static int fit_id = -1;
static float fit_len, fit_hx, fit_hz;

static void map_fit(float *cx, float *cy, float *sc) {
    static float ox, oz, k = 1.0f;
    if(fit_id != track.id || fit_len != track.length) {
        fit_id = track.id;
        fit_len = track.length;
        float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for(int i = 0; i < TRACK_SAMPLES; i++) {
            V3 p = track.smp[i].p;
            x0 = fminf(x0, p.x - 9.0f);
            x1 = fmaxf(x1, p.x + 9.0f);
            z0 = fminf(z0, p.z - 9.0f);
            z1 = fmaxf(z1, p.z + 9.0f);
        }
        ox = oz = 0.0f;
        k = 1.0f;
        if(fmaxf(-x0, x1) > MAP_HX + 9.0f || fmaxf(-z0, z1) > MAP_HZ + 9.0f) {
            ox = 0.5f * (x0 + x1);
            oz = 0.5f * (z0 + z1);
            k = fminf(1.0f, fminf((MAP_HX + 9.0f) / (0.5f * (x1 - x0)), (MAP_HZ + 9.0f) / (0.5f * (z1 - z0))));
        }
        /* How far it reaches from the map's centre, as drawn (k applied). */
        fit_hx = k * fmaxf(x1 - ox, ox - x0);
        fit_hz = k * fmaxf(z1 - oz, oz - z0);
    }
    *sc *= k;
    *cx -= ox * *sc;
    *cy += oz * *sc;
}

float hud_track_map_fit(float sc, float half_w, float half_h) {
    float cx = 0, cy = 0, s1 = 1.0f;
    map_fit(&cx, &cy, &s1);   /* brings fit_hx and fit_hz up to date */
    return fminf(sc, fminf(half_w / fit_hx, half_h / fit_hz));
}

void hud_draw_track_map(float cx, float cy, float sc, float z, bool show_karts) {
    r_hdr(h_flat);
    if(!track_is_roam()) map_fit(&cx, &cy, &sc);
    /* Free Roam: the lot inside the ring road, where everyone actually is. */
    if(track_is_roam()) {
        float hx = (LOT_HX - 9.0f) * sc, hz = (LOT_HZ - 9.0f) * sc;
        r_quad2d(cx - hx, cy - hz, cx + hx, cy + hz, 0, 0, 1, 1, argb(110, 200, 210, 255), z - 0.05f);
    }
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

/* The slide's score counting up in its stage colour, centred at sy, then
   the score just banked for a couple of seconds (gold when it is a new
   best). Callers put it over the charge gauge, where the eye is while
   drifting; a split screen's kart sits there, so top centre of the view
   instead. */
static void draw_drift_score(const Kart *p, float sy, float k) {
    char buf[32];
    if(p->drift == DRIFT_ACTIVE && p->drift_score >= 1.0f) {
        uint32_t c = p->stage ? fx_stage_color(p->stage) : argb(255, 240, 240, 240);
        snprintf(buf, sizeof(buf), "DRIFT %d", (int)p->drift_score);
        hud_text_c(320, sy, 0.9f * k, ALIGN_CENTER, c, buf);
    }
    else if(p->drift_pop_t > 0) {
        float a = p->drift_pop_t < 0.4f ? p->drift_pop_t / 0.4f : 1.0f;
        uint32_t c = p->drift_best_new ? argb(255, 255, 220, 80) : argb(255, 240, 240, 240);
        snprintf(buf, sizeof(buf), p->drift_best_new ? "NEW BEST %d" : "DRIFT %d", (int)p->last_drift);
        hud_text_c(320, sy, 0.9f * k, ALIGN_CENTER, color_alpha(c, a), buf);
    }
}

/* Free Roam's corner: no laps or places, a FREE ROAM tag with the speed,
   the time on the lot on the right. */
static void draw_roam_corner(int pi, float y0, float y1) {
    const Kart *p = &karts[game.pl[pi].kart];
    bool split = y1 - y0 < 400.0f;
    float k = split ? 0.72f : 1.0f;
    char buf[32];
    /* Two panels of one size: the mode and speed, the best drift under
       it (unless Settings has the drift counter off), labels and readings
       at matching scales. No clock: nothing is being timed. */
    bool drifts = save.drift_show != DRIFT_SHOW_OFF;
    const float box_h = 40 * k, pad = 12 * k, gap = 6 * k, ls = 0.62f * k, vs = 0.8f * k;
    float lx = split ? 14 : 24, ly = y0 + (split ? 8 : 22);
    float label_w = fmaxf(hud_text_width("FREE ROAM", ls), hud_text_width("BEST DRIFT", ls));
    float val_w = fmaxf(hud_text_width("999 MPH", vs), hud_text_width("99999", vs));
    float lw = pad + label_w + 10 * k + val_w + pad;
    const char *labels[2] = {"FREE ROAM", "BEST DRIFT"};
    for(int row = 0; row < (drifts ? 2 : 1); row++) {
        float ry = ly + row * (box_h + gap), cy = ry + box_h * 0.5f;
        hud_panel(lx, ry, lx + lw, ry + box_h, argb(150, 20, 60, 160), argb(150, 10, 30, 90), 15);
        hud_text_c(lx + pad, cy + 1.0f * k, ls, ALIGN_LEFT, argb(255, 255, 220, 80), labels[row]);
        if(row == 0) snprintf(buf, sizeof(buf), "%d MPH", (int)(fabsf(p->speed) * 2.23694f + 0.5f));
        else snprintf(buf, sizeof(buf), "%d", (int)p->best_drift);
        hud_text_c(lx + lw - pad, cy, vs, ALIGN_RIGHT, 0xffffffff, buf);
    }
    if(split) hud_text_c(lx + lw + 10 * k, ly + box_h * 0.5f, 0.75f * k, ALIGN_LEFT, p->ui_color | 0xff000000, pi ? "P2" : "P1");
    if(drifts) draw_drift_score(p, split ? ly + box_h * 0.5f : 440 - 22 * k, k);
}

void hud_draw_race(int pi, float y0, float y1) {
    const Kart *p = &karts[game.pl[pi].kart];
    bool split = y1 - y0 < 400.0f;
    float k = split ? 0.72f : 1.0f;       /* HUD scale */
    char buf[48];
    if(track_is_roam()) {
        draw_roam_corner(pi, y0, y1);
        goto common;
    }
    /* Lap counter, in a box sized to the race's widest reading (so it never
       changes size mid-race), the number centred in its slot and the LAP
       sharing its baseline. The font's digits are tabular, so only a
       ten-lap race's single-digit laps leave slack, split either side. */
    int lap = p->lap < 1 ? 1 : (p->lap > game.laps ? game.laps : p->lap);
    const float box_h = 40 * k, pad = 12 * k;
    float lx = split ? 14 : 24, ly = y0 + (split ? 8 : 22), cy = ly + box_h * 0.5f;
    snprintf(buf, sizeof(buf), "%d/%d", game.laps, game.laps);
    float label_w = hud_text_width("LAP", 0.75f * k), count_w = hud_text_width(buf, 1.05f * k);
    float lw = pad + label_w + 8 * k + count_w + pad;
    hud_panel(lx, ly, lx + lw, ly + box_h, argb(150, 20, 60, 160), argb(150, 10, 30, 90), 15);
    hud_text_c(lx + pad, cy + 3.3f * k, 0.75f * k, ALIGN_LEFT, argb(255, 255, 220, 80), "LAP");
    snprintf(buf, sizeof(buf), "%d/%d", lap, game.laps);
    hud_text_c(lx + pad + label_w + 8 * k + count_w * 0.5f, cy, 1.05f * k, ALIGN_CENTER, 0xffffffff, buf);
    if(split) {
        /* Player tag in their colour. */
        hud_text_c(lx + lw + 10 * k, cy, 0.75f * k, ALIGN_LEFT, p->ui_color | 0xff000000, pi ? "P2" : "P1");
    }
    else {
        /* Race time, or in a time trial on your own the current lap, so it
           can be read against the BEST below it. */
        bool lap_clock = game.mode == MODE_TT && game.num_players == 1;
        float tw = hud_text_width("0:00.00", 1.0f) + 2 * pad;
        hud_panel(616 - tw, 22, 616, 22 + box_h, argb(150, 20, 60, 160), argb(150, 10, 30, 90), 15);
        hud_format_time(buf, p->finished ? p->finish_time : lap_clock ? game.race_time - p->lap_start : game.race_time);
        hud_text_c(616 - pad, 22 + box_h * 0.5f, 1.0f, ALIGN_RIGHT, 0xffffffff, buf);
    }
    /* The held power-up, as a small gem beside the lap counter. */
    items_draw_hud(pi, lx + lw + (split ? 60 * k : 26), cy, k);
    if(game.mode == MODE_TT && p->best_lap > 0 && game.num_players == 1) {
        char t[20];
        hud_format_time(t, p->best_lap);
        snprintf(buf, sizeof(buf), "BEST %s", t);
        hud_text(604, 22 + box_h + 2, 0.6f, ALIGN_RIGHT, argb(255, 255, 230, 120), buf);
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

    /* The drift score outside Free Roam, when Settings asks for it. */
    if(save.drift_show == DRIFT_SHOW_ALWAYS && !p->finished) draw_drift_score(p, split ? y0 + 30 * k : 440 - 22 * k, k);

common:
    if(!split) hud_draw_track_map(98, 402, track_is_roam() ? 0.34f : 0.40f, 18.0f, true);

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
    if(!track_is_roam()) {   /* nothing is timed in Free Roam */
        char buf[24];
        hud_panel(250, 224, 390, 256, argb(220, 20, 60, 160), argb(220, 10, 30, 90), 17.2f);
        hud_format_time(buf, game.race_time);
        hud_text(320, 223, 0.8f, ALIGN_CENTER, 0xffffffff, buf);
    }
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
    ui_swatch(x, y, 24, 14, paint1 ? paint1 : 0xffe02828, ui);
}

void hud_draw_results(void) {
    float a = clampf(game.time / 0.5f, 0, 1);
    float x0 = 100 + (1 - a) * 440, x1 = x0 + 440, cx = (x0 + x1) * 0.5f;
    float y0 = 54, y1 = 424;
    ui_panel(x0, y0, x1, y1);
    float yc = ui_title(x0, y0, x1, game.mode != MODE_TT ? "RESULTS" : game.num_players > 1 ? "VERSUS" : "TIME TRIAL");
    char buf[48], t[20];
    if(game.mode != MODE_TT || game.num_players > 1) {
        const float pitch = 38;
        for(int pos = 0; pos < kart_count; pos++) {
            int i = game.finish_order[pos];
            const Kart *k = &karts[i];
            float y = yc + 4 + pos * pitch, cy = y + 17;
            bool me = kart_is_human(i);
            ui_row(x0 + 14, y, x1 - 14, y + 34, me);
            snprintf(buf, sizeof(buf), "%d%s", pos + 1, hud_ordinal(pos + 1));
            hud_text_c(x0 + 30, cy, 0.75f, ALIGN_LEFT, me ? ui_col(UI_ITEM_SEL) : hud_place_color(pos + 1), buf);
            hud_swatch(x0 + 100, cy - 7, k->paint1, k->ui_color);
            hud_text_c(x0 + 138, cy, 0.66f, ALIGN_LEFT, ui_col(me ? UI_ITEM_SEL : UI_TEXT), k->name);
            hud_format_time(t, k->finish_time);
            hud_text_c(x1 - 30, cy, 0.66f, ALIGN_RIGHT, ui_col(me ? UI_ITEM_SEL : UI_TEXT), k->finished ? t : "--'--\"--");
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
            float lx = x0 + 50 + (l / rows) * (col_w + 20), ly = yc + 14 + (l % rows) * pitch;
            hud_text_c(lx, ly, scale, ALIGN_LEFT, ui_col(cols > 1 ? UI_LABEL : UI_TEXT), buf);
            hud_text_c(lx + col_w - (cols > 1 ? 20 : 0), ly, scale, ALIGN_RIGHT, best ? ui_col(UI_ACCENT) : ui_col(UI_TEXT), t);
        }
        float total_y = rows > 3 ? yc + 14 + rows * pitch + 14 : yc + 170;
        hud_format_time(t, k->finish_time);
        hud_text_c(x0 + 50, total_y, 1.0f, ALIGN_LEFT, ui_col(UI_ACCENT), "TOTAL");
        hud_text_c(x1 - 50, total_y, 1.0f, ALIGN_RIGHT, ui_col(UI_ACCENT), t);
    }
    /* What next: left/right picks, A goes. */
    static const char *const next[RESULTS_ITEMS] = {"RACE AGAIN", "NEW TRACK", "TITLE"};
    float in = clampf(game.time - 0.6f, 0, 1);
    if(in > 0)
        for(int i = 0; i < RESULTS_ITEMS; i++) ui_button(cx + (i - 1) * 142, y1 - 34, 130, next[i], i == game.results_sel);
}

void hud_toast(const char *text) {
    snprintf(toast, sizeof(toast), "%s", text);
    toast_t = TOAST_SECS;
}

/* The same slim band as the online join toast: top centre, clear of the
   lap and timer boxes and above the pause panel, fading in over 0.2 s and
   out over the last 0.4 s. Drawn after the pause menu's dim so it stays
   bright while the game is paused. */
void hud_draw_toast(void) {
    if(toast_t <= 0) return;
    float a = fminf((TOAST_SECS - toast_t) / 0.2f, fminf(toast_t / 0.4f, 1.0f));
    float w = hud_text_width(toast, 0.42f) + 36;
    ui_pill(320 - w * 0.5f, 6, 320 + w * 0.5f, 34, color_alpha(argb(255, 18, 40, 108), a), color_alpha(argb(255, 8, 16, 52), a));
    hud_text_c(320, 20, 0.42f, ALIGN_CENTER, color_alpha(ui_col(UI_ACCENT), a), toast);
}

void hud_draw_pause(void) {
    static const char *const perf[PERF_MODES] = {"PERF: OFF", "PERF: OVERLAY", "PERF: FPS"};
    const char *items[PAUSE_ITEMS] = {"CONTINUE", "RESTART", audio_music_enabled() ? "MUSIC: ON" : "MUSIC: OFF",
                                      "NEXT SONG", perf[perfhud_mode], "QUIT"};
    ui_dim();
    float x0 = 160, x1 = 480, y0 = 134;
    const float pitch = 42;
    ui_panel(x0, y0, x1, y0 + 54 + PAUSE_ITEMS * pitch + 14);
    float yc = ui_title(x0, y0, x1, "PAUSE") + 10;
    for(int i = 0; i < PAUSE_ITEMS; i++) {
        bool sel = i == game.pause_sel;
        float y = yc + i * pitch;
        ui_row(x0 + 14, y, x1 - 14, y + pitch - 4, sel);
        float s = sel ? 0.8f + 0.02f * sinf(hud_time * 8) : 0.76f;
        hud_text_c(320, y + (pitch - 4) * 0.5f, s, ALIGN_CENTER, ui_col(sel ? UI_ITEM_SEL : UI_ITEM), items[i]);
    }
}
