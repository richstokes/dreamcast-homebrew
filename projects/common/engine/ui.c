/* Engine: menu chrome (ui.h). Everything here is vertex-coloured quads,
   triangles and fans under the flat header; the font atlas is the only
   texture. The translucent list is drawn in submission order (no
   autosort), so each piece is drawn back to front: shadow, outline, body,
   gloss, then whatever sits on it. */
#include "ui.h"
#include <string.h>

static const pvr_poly_hdr_t *h_flat;
static const EngFont *font;
static EngUiTheme th;
float ui_time;

uint32_t ui_col(int role) {
    return th.roles[role];
}

/* A quarter arc in ARC_N segments, as unit vectors; small corners take
   every other one. */
#define ARC_N 6
static float arc_c[ARC_N + 1], arc_s[ARC_N + 1];

void ui_init(const EngFont *f, const EngUiTheme *theme) {
    font = f;
    th = *theme;
    h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
    for(int i = 0; i <= ARC_N; i++) {
        arc_c[i] = cosf((float)i / ARC_N * (ENG_PI * 0.5f));
        arc_s[i] = sinf((float)i / ARC_N * (ENG_PI * 0.5f));
    }
}

/* ---- shapes --------------------------------------------------------- */

static uint32_t grad(uint32_t top, uint32_t bottom, float y, float y0, float y1) {
    if(top == bottom) return top;
    return color_lerp(top, bottom, clampf((y - y0) / (y1 - y0), 0, 1));
}

/* A corner's fan: q is 0 top-left, 1 top-right, 2 bottom-right, 3
   bottom-left, the arc running clockwise. Each ring vertex is coloured
   once; the panel's gradient runs top to bottom. */
static void corner(float cx, float cy, float r, int q, int step, uint32_t top, uint32_t bottom, float y0, float y1) {
    float px[ARC_N + 1], py[ARC_N + 1];
    uint32_t pc[ARC_N + 1];
    int n = 0;
    for(int i = 0; i <= ARC_N; i += step, n++) {
        float c = arc_c[i] * r, sn = arc_s[i] * r;
        switch(q) {
            case 0: px[n] = cx - c; py[n] = cy - sn; break;
            case 1: px[n] = cx + sn; py[n] = cy - c; break;
            case 2: px[n] = cx + c; py[n] = cy + sn; break;
            default: px[n] = cx - sn; py[n] = cy + c; break;
        }
        pc[n] = grad(top, bottom, py[n], y0, y1);
    }
    uint32_t cc = grad(top, bottom, cy, y0, y1);
    for(int i = 0; i + 1 < n; i++) r_tri2d(cx, cy, px[i], py[i], px[i + 1], py[i + 1], cc, pc[i], pc[i + 1], 16.0f);
}

void ui_rrect(float x0, float y0, float x1, float y1, float r, uint32_t top, uint32_t bottom) {
    r_hdr(h_flat);
    float w = x1 - x0, h = y1 - y0;
    if(r > w * 0.5f) r = w * 0.5f;
    if(r > h * 0.5f) r = h * 0.5f;
    if(r < 1.0f) {
        r_quad2d_grad(x0, y0, x1, y1, top, bottom, 16.0f);
        return;
    }
    uint32_t cr = grad(top, bottom, y0 + r, y0, y1), cr1 = grad(top, bottom, y1 - r, y0, y1);
    /* Middle band, then the top and bottom strips between the corners. */
    r_quad2d_grad(x0, y0 + r, x1, y1 - r, cr, cr1, 16.0f);
    r_quad2d_grad(x0 + r, y0, x1 - r, y0 + r, top, cr, 16.0f);
    r_quad2d_grad(x0 + r, y1 - r, x1 - r, y1, cr1, bottom, 16.0f);
    int step = r < 10.0f ? 2 : 1;   /* a key's corner needs no six segments */
    corner(x0 + r, y0 + r, r, 0, step, top, bottom, y0, y1);
    corner(x1 - r, y0 + r, r, 1, step, top, bottom, y0, y1);
    corner(x1 - r, y1 - r, r, 2, step, top, bottom, y0, y1);
    corner(x0 + r, y1 - r, r, 3, step, top, bottom, y0, y1);
}

void ui_pill(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom) {
    ui_rrect(x0, y0, x1, y1, (y1 - y0) * 0.5f, top, bottom);
}

void ui_circle(float cx, float cy, float r, uint32_t col) {
    r_hdr(h_flat);
    const int n = 18;
    for(int i = 0; i < n; i++) {
        float t0 = (float)i / n * ENG_TAU, t1 = (float)(i + 1) / n * ENG_TAU;
        r_tri2d(cx, cy, cx + cosf(t0) * r, cy + sinf(t0) * r, cx + cosf(t1) * r, cy + sinf(t1) * r, col, col, col, 16.0f);
    }
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col) {
    r_hdr(h_flat);
    r_tri2d(x0, y0, x1, y1, x2, y2, col, col, col, 16.0f);
}

/* A heading plaque or selection pill: outline, the theme's plaque
   gradient breathing toward plaque_glow, a gloss across the top. The
   brightest it breathes to still leaves white lettering its outline to
   stand on. */
static void plaque(float x0, float y0, float x1, float y1, float glow) {
    ui_pill(x0 - 3, y0 - 3, x1 + 3, y1 + 3, th.outline, th.outline);
    ui_pill(x0, y0, x1, y1, color_lerp(th.plaque_top, th.plaque_glow, glow), th.plaque_bottom);
    ui_pill(x0 + 6, y0 + 3, x1 - 6, y0 + (y1 - y0) * 0.45f, argb(80, 255, 255, 255), argb(8, 255, 255, 255));
}

/* ---- panels --------------------------------------------------------- */

void ui_panel(float x0, float y0, float x1, float y1) {
    /* An outline round the body with a glossy top half, over its shadow. */
    ui_rrect(x0, y0 + 6, x1, y1 + 7, 16, th.shadow, th.shadow);
    ui_rrect(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 16, th.outline, th.outline);
    ui_rrect(x0, y0, x1, y1, 13, th.panel_top, th.panel_bottom);
    ui_rrect(x0 + 3, y0 + 3, x1 - 3, y0 + (y1 - y0) * 0.42f, 11, argb(60, 255, 255, 255), argb(8, 255, 255, 255));
}

void ui_well(float x0, float y0, float x1, float y1) {
    ui_rrect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 9, th.outline, th.outline);
    ui_rrect(x0, y0, x1, y1, 7, th.well_top, th.well_bottom);
}

float ui_title(float x0, float y0, float x1, const char *text) {
    float cx = (x0 + x1) * 0.5f;
    float w = text_width(font, text, 0.78f) + 48, h = 36;
    plaque(cx - w * 0.5f, y0 - 18, cx + w * 0.5f, y0 - 18 + h, 0);
    text_draw_c(font, cx, y0 - 18 + h * 0.5f, 0.78f, ALIGN_CENTER, th.roles[UI_TITLE], text);
    return y0 + 30;
}

void ui_heading(float cx, float cy, const char *text) {
    float w = text_width(font, text, 0.9f) + 56, h = 44;
    ui_pill(cx - w * 0.5f, cy - h * 0.5f + 5, cx + w * 0.5f, cy + h * 0.5f + 6, th.shadow, th.shadow);
    plaque(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, 0);
    text_draw_c(font, cx, cy, 0.9f, ALIGN_CENTER, th.roles[UI_TITLE], text);
}

/* ---- rows and buttons ------------------------------------------------ */

void ui_row(float x0, float y0, float x1, float y1, bool sel) {
    if(!sel) return;
    float p = 0.5f + 0.5f * sinf(ui_time * 6.0f);   /* the breathing highlight */
    plaque(x0, y0, x1, y1, p);
}

void ui_button(float cx, float cy, float w, const char *text, bool sel) {
    float h = 36, s = sel ? 0.64f : 0.6f;
    float tw = text_width(font, text, s);
    if(tw > w - 24) s *= (w - 24) / tw;   /* long lettering shrinks to fit */
    float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f, y0 = cy - h * 0.5f, y1 = cy + h * 0.5f;
    if(sel) {
        ui_row(x0, y0, x1, y1, true);
        text_draw_c(font, cx, cy, s, ALIGN_CENTER, th.roles[UI_ITEM_SEL], text);
        return;
    }
    ui_pill(x0 - 2, y0 - 2, x1 + 2, y1 + 2, th.outline, th.outline);
    ui_pill(x0, y0, x1, y1, th.button_top, th.button_bottom);
    text_draw_c(font, cx, cy, s, ALIGN_CENTER, th.roles[UI_ITEM], text);
}

void ui_arrows(float xl, float xr, float cy, float scale, uint32_t col) {
    float bob = 3.0f * sinf(ui_time * 9);
    text_draw_c(font, xl - bob, cy, scale, ALIGN_CENTER, col, "<");
    text_draw_c(font, xr + bob, cy, scale, ALIGN_CENTER, col, ">");
}

/* ---- hints and glyphs ------------------------------------------------ */

/* The controller's own button colours and a pale key cap: the same on
   every game, like the controller. */
#define GLYPH_BODY argb(255, 232, 236, 246)
#define GLYPH_BODY_LO argb(255, 200, 206, 222)
#define GLYPH_INK argb(255, 30, 40, 80)

static void glyph_disc(float cx, float cy, float r, uint32_t col, const char *letter) {
    ui_circle(cx, cy, r + 2, th.outline);
    ui_circle(cx, cy, r, col);
    ui_circle(cx - r * 0.2f, cy - r * 0.3f, r * 0.55f, color_lerp(col, 0xffffffff, 0.25f));
    text_letter_c(font, cx, cy, 0.4f, 0xffffffff, letter[0]);
}

float ui_glyph(float x, float cy, const char *key, bool draw) {
    const float r = 10;
    uint32_t col = 0;
    if(!strcmp(key, "A")) col = argb(255, 232, 62, 48);
    else if(!strcmp(key, "B")) col = argb(255, 42, 118, 236);
    else if(!strcmp(key, "X")) col = argb(255, 250, 200, 40);
    else if(!strcmp(key, "Y")) col = argb(255, 62, 190, 92);
    if(col) {
        if(draw) glyph_disc(x + r, cy, r, col, key);
        return r * 2;
    }
    uint32_t body = GLYPH_BODY, ink = GLYPH_INK;
    if(!strcmp(key, "START")) {
        float w = 46;
        if(draw) {
            ui_pill(x - 2, cy - 11, x + w + 2, cy + 11, th.outline, th.outline);
            ui_pill(x, cy - 9, x + w, cy + 9, body, GLYPH_BODY_LO);
            text_draw_c(font, x + w * 0.5f, cy + 1, 0.3f, ALIGN_CENTER, ink, "START");
        }
        return w;
    }
    if(!strcmp(key, "LEFT/RIGHT") || !strcmp(key, "UP/DOWN")) {
        float w = 38;
        if(draw) {
            ui_pill(x - 2, cy - 11, x + w + 2, cy + 11, th.outline, th.outline);
            ui_pill(x, cy - 9, x + w, cy + 9, body, GLYPH_BODY_LO);
            if(key[0] == 'L') {
                tri(x + 8, cy, x + 16, cy - 6, x + 16, cy + 6, ink);
                tri(x + w - 8, cy, x + w - 16, cy - 6, x + w - 16, cy + 6, ink);
            }
            else {
                tri(x + w * 0.5f - 10, cy - 1, x + w * 0.5f - 4, cy - 7, x + w * 0.5f + 2, cy - 1, ink);
                tri(x + w * 0.5f - 2, cy + 1, x + w * 0.5f + 4, cy + 7, x + w * 0.5f + 10, cy + 1, ink);
            }
        }
        return w;
    }
    if(!strcmp(key, "L+R")) {
        float w = 48;
        if(draw) {
            for(int i = 0; i < 2; i++) {
                float gx = x + i * 26;
                ui_rrect(gx - 2, cy - 11, gx + 24, cy + 11, 7, th.outline, th.outline);
                ui_rrect(gx, cy - 9, gx + 22, cy + 9, 5, body, GLYPH_BODY_LO);
                text_letter_c(font, gx + 11, cy, 0.34f, ink, i ? 'R' : 'L');
            }
        }
        return w;
    }
    return 0;
}

static bool glyph_key(const char *key) {
    static const char *const keys[] = {"A", "B", "X", "Y", "START", "LEFT/RIGHT", "UP/DOWN", "L+R"};
    for(size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if(!strcmp(key, keys[i])) return true;
    return false;
}

/* "A: OK   B: BACK": items split on runs of spaces, a known key before the
   colon drawn as its glyph. Lays the line out from x (or only measures it). */
static float hint_line(float x, float cy, const char *text, bool draw) {
    const float scale = 0.44f;
    float w = 0;
    const char *p = text;
    while(*p) {
        const char *e = p;
        while(*e && !(e[0] == ' ' && e[1] == ' ')) e++;
        char item[64];
        size_t len = (size_t)(e - p);
        if(len > sizeof(item) - 1) len = sizeof(item) - 1;
        memcpy(item, p, len);
        item[len] = 0;
        char *colon = strchr(item, ':');
        if(colon) {
            *colon = 0;
            if(glyph_key(item)) {
                w += ui_glyph(x + w, cy, item, draw) + 7;
                const char *label = colon + 1;
                while(*label == ' ') label++;
                if(draw) text_draw_c(font, x + w, cy, scale, ALIGN_LEFT, th.roles[UI_HINT], label);
                w += text_width(font, label, scale);
            }
            else {
                *colon = ':';
                if(draw) text_draw_c(font, x + w, cy, scale, ALIGN_LEFT, th.roles[UI_HINT], item);
                w += text_width(font, item, scale);
            }
        }
        else {
            if(draw) text_draw_c(font, x + w, cy, scale, ALIGN_LEFT, th.roles[UI_HINT], item);
            w += text_width(font, item, scale);
        }
        p = e;
        while(*p == ' ') p++;
        if(*p) w += 24;
    }
    return w;
}

void ui_hint_at(float cx, float cy, const char *text) {
    float w = hint_line(0, 0, text, false);
    ui_pill(cx - w * 0.5f - 20, cy - 17, cx + w * 0.5f + 20, cy + 17, th.outline, th.outline);
    ui_pill(cx - w * 0.5f - 17, cy - 14, cx + w * 0.5f + 17, cy + 14, th.hint_top, th.hint_bottom);
    hint_line(cx - w * 0.5f, cy, text, true);
}

void ui_hint(const char *text) {
    ui_hint_at(320, 453, text);
}

void ui_key(float x0, float y0, float x1, float y1, bool sel) {
    if(sel) {
        ui_row(x0, y0, x1, y1, true);
        return;
    }
    ui_rrect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 8, th.outline, th.outline);
    ui_rrect(x0, y0, x1, y1, 6, th.button_top, th.button_bottom);
}

/* ---- odds and ends -------------------------------------------------- */

void ui_swatch(float x, float y, float w, float h, uint32_t a, uint32_t b) {
    a |= 0xff000000u;
    b |= 0xff000000u;
    ui_rrect(x - 2, y - 2, x + w + 2, y + h + 2, 4, th.outline, th.outline);
    ui_rrect(x, y, x + w * 0.5f + 2, y + h, 3, a, a);
    ui_rrect(x + w * 0.5f, y, x + w, y + h, 3, b, b);
}

void ui_dots(float cx, float cy, int n, int cur) {
    for(int i = 0; i < n; i++) {
        float dx = cx + ((float)i - (n - 1) * 0.5f) * 18.0f;
        bool on = i == cur;
        if(on) ui_circle(dx, cy, 6.5f, th.outline);
        ui_circle(dx, cy, on ? 5.0f : 3.5f, on ? th.roles[UI_ACCENT] : argb(150, 255, 255, 255));
    }
}

void ui_dim(void) {
    r_hdr(h_flat);
    r_quad2d(0, 0, 640, 480, 0, 0, 1, 1, th.dim, 16.0f);
}
