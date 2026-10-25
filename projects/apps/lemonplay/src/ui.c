#include "ui.h"

#include "gfx.h"

#include <kos.h>
#include <math.h>
#include <stdio.h>

static float hint_x;

void ui_header(const char *subtitle) {
    gfx_rect(0, 0, SCREEN_W, HEADER_H, COL_PANEL);
    gfx_rect(0, HEADER_H, SCREEN_W, 2, COL_LEMON_DIM);
    gfx_icon(ICON_LEMON, SAFE_X - 4, SAFE_Y - 4, 56, 0xffffffff);
    float x = gfx_text(SAFE_X + 56, SAFE_Y + 4, COL_LEMON, 2, "LemonPlay");
    if(subtitle && subtitle[0]) {
        x += 16;
        gfx_text_clip(x, SAFE_Y + 22, COL_MUTED, 1, subtitle, SCREEN_W - SAFE_X - x);
    }
}

void ui_footer(void) {
    gfx_gradient(0, FOOTER_Y - 12, SCREEN_W, 12, 0x001d1a11, 0xff1d1a11);
    gfx_rect_alpha(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, 0xff1d1a11);
    hint_x = SAFE_X;
}

void ui_hint(char button, const char *label) {
    hint_x = gfx_hint(hint_x, FOOTER_Y + 10, button, label, COL_MUTED);
}

void ui_hint_right(char button, const char *label) {
    float w = gfx_hint_width(button, label);
    gfx_hint(SCREEN_W - SAFE_X - w, FOOTER_Y + 10, button, label, COL_LEMON_DIM);
}

void ui_spinner(float cx, float cy) {
    float phase = (timer_ms_gettime64() % 960) / 120.0f;
    for(int i = 0; i < 8; i++) {
        float a = i * (float)M_PI / 4;
        float age = fmodf(phase - i + 8, 8);   /* 0 = newest */
        int alpha = 255 - (int)(age * 28);
        gfx_icon(ICON_DISC, cx + 14 * sinf(a) - 4, cy - 14 * cosf(a) - 4, 8, (uint32_t)alpha << 24 | 0xf7d33c);
    }
}

void ui_progress(float x, float y, float w, float fraction, int highlight) {
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    gfx_pill(x, y, w, 6, 0xff4a4535);
    if(fraction > 0) gfx_pill(x, y, 6 + (w - 6) * fraction, 6, highlight ? COL_LEMON : 0xffcfb244);
    float knob = highlight ? 18 : 14;
    gfx_icon(ICON_DISC, x + (w - 6) * fraction + 3 - knob / 2, y + 3 - knob / 2, knob, COL_LEMON);
}

void ui_toast(const char *text, int error) {
    float w = gfx_text_width(text, 1);
    float max = SCREEN_W - 2 * SAFE_X - 48;
    if(w > max) w = max;
    float x = (SCREEN_W - w - 40) / 2, y = FOOTER_Y - 52;
    gfx_pill(x - 8, y, w + 56, 36, error ? 0xf0402818 : 0xf0302c1a);
    gfx_icon(error ? ICON_WARNING : ICON_LEMON, x + 2, y + 6, 24, error ? COL_ERROR : 0xffffffff);
    gfx_text_clip(x + 32, y + 6, error ? COL_ERROR : COL_LEMON, 1, text, w + 4);
}

void ui_format_time(char *out, int size, double secs) {
    int t = secs < 0 ? 0 : (int)secs;
    if(t >= 3600) snprintf(out, size, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, size, "%d:%02d", t / 60, t % 60);
}
