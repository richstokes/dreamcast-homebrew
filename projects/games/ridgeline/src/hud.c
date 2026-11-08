/* Ridgeline HUD: a rev counter with the gear and speed, the drive mode,
   an inclinometer, the map, how far up the mountain, viewpoints found, and
   short messages. Everything in the translucent list, after the scene. */
#include "hud.h"
#include "truck.h"
#include "terrain.h"
#include "engine/text.h"
#include "font_metrics.h"

static EngFont font;
static const pvr_poly_hdr_t *h_dial, *h_icons, *h_map, *h_flat, *h_glow;
static char msg[48], msg_sub[64];
static float msg_t, msg_len;
static uint32_t msg_col;
static float tcs_lamp; /* seconds the traction control lamp stays lit */
float hud_hold_k;
const char *hud_hold_label;

#define Z 10.0f
/* The bottom edge every bottom-row element sits on: the rev counter, the
   traction lamp and the drive mode tabs. */
#define HUD_BOTTOM 460.0f

void hud_init(void) {
    font.hdr = r_header(TEX_FONT, HF_TR | HF_NOFOG | HF_CLAMP);
    font.glyphs = (const eng_glyph_t *)font_glyphs;
    font.row_h = FONT_ROW_H;
    font.tex_w = FONT_TEX_W;
    font.tex_h = FONT_TEX_H;
    font.cap_top = FONT_CAP_TOP;
    font.cap_bottom = FONT_CAP_BOTTOM;
    h_dial = r_header(TEX_DIAL, HF_TR | HF_NOFOG | HF_CLAMP);
    h_icons = r_header(TEX_ICONS, HF_TR | HF_NOFOG | HF_CLAMP);
    h_map = r_header(TEX_MAP, HF_TR | HF_NOFOG | HF_CLAMP);
    h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_NOFOG | HF_ADD);
}

const EngFont *hud_font(void) {
    return &font;
}

void hud_message(const char *text, uint32_t col, float seconds) {
    snprintf(msg, sizeof(msg), "%s", text);
    msg_sub[0] = 0;
    msg_col = col;
    msg_t = msg_len = seconds;
}

void hud_message_sub(const char *text) {
    snprintf(msg_sub, sizeof(msg_sub), "%s", text);
}

void hud_update(float dt) {
    if(msg_t > 0) msg_t -= dt;
    /* Hold the lamp briefly so short cuts don't flicker it. */
    if(trucks[0].tc < 0.9f) tcs_lamp = 0.4f;
    else if(tcs_lamp > 0) tcs_lamp -= dt;
}

void hud_text(float x, float y, float scale, int align, uint32_t col, const char *s) {
    text_draw(&font, x, y, scale, align, col, s);
}

void hud_text_c(float x, float cy, float scale, int align, uint32_t col, const char *s) {
    text_draw_c(&font, x, cy, scale, align, col, s);
}

/* A little gold cup, s pixels tall, centred on (cx, cy). */
void hud_trophy(float cx, float cy, float s, float alpha) {
    r_hdr(h_flat);
    for(int pass = 0; pass < 2; pass++) {
        /* A dark copy a pixel down and right, then the gold. */
        float o = pass ? 0 : 1.2f, z = Z + 0.6f + pass * 0.1f;
        uint32_t c = color_alpha(pass ? 0xffffc838 : 0xff201808, alpha), d = color_alpha(pass ? 0xffd89820 : 0xff201808, alpha);
        float x = cx + o, y = cy + o;
        float t = y - 0.5f * s, b = y + 0.08f * s;
        r_tri2d(x - 0.42f * s, t, x + 0.42f * s, t, x + 0.22f * s, b, c, c, d, z);
        r_tri2d(x - 0.42f * s, t, x + 0.22f * s, b, x - 0.22f * s, b, c, d, d, z);
        r_quad2d(x - 0.6f * s, t + 0.06f * s, x - 0.3f * s, t + 0.3f * s, 0, 0, 1, 1, d, z);
        r_quad2d(x + 0.3f * s, t + 0.06f * s, x + 0.6f * s, t + 0.3f * s, 0, 0, 1, 1, d, z);
        r_quad2d(x - 0.07f * s, b, x + 0.07f * s, y + 0.3f * s, 0, 0, 1, 1, d, z);
        r_quad2d(x - 0.26f * s, y + 0.3f * s, x + 0.26f * s, y + 0.46f * s, 0, 0, 1, 1, c, z);
    }
}

void hud_panel(float x0, float y0, float x1, float y1, uint32_t col) {
    r_hdr(h_flat);
    r_quad2d(x0, y0, x1, y1, 0, 0, 1, 1, col, Z - 1);
}

/* A textured quad turned by a about its centre. */
static void quad_rot(float cx, float cy, float hw, float hh, float a, float u0, float v0, float u1, float v1, uint32_t col) {
    float c = cosf(a), s = sinf(a);
    float xy[8], uv[8] = {u0, v0, u1, v0, u0, v1, u1, v1};
    static const float sx[4] = {-1, 1, -1, 1}, sy[4] = {-1, -1, 1, 1};
    for(int i = 0; i < 4; i++) {
        float x = sx[i] * hw, y = sy[i] * hh;
        xy[i * 2] = cx + x * c - y * s;
        xy[i * 2 + 1] = cy + x * s + y * c;
    }
    uint32_t cols[4] = {col, col, col, col};
    r_quad2d_pts(xy, uv, cols, Z);
}

static void dial(const Truck *t) {
    const float R = 66, cx = 560, cy = HUD_BOTTOM - R;
    r_hdr(h_dial);
    r_quad2d(cx - R, cy - R, cx + R, cy + R, 0, 0, 1, 1, 0xfff4ecd8, Z);
    /* The needle: 0 to 6000 rpm over 270 degrees, out at the rim so it
       never crosses the speed. */
    float k = clampf(t->rpm / 6000.0f, 0, 1.05f);
    float a = (135.0f + 270.0f * k) * ENG_PI / 180.0f;
    float ca = cosf(a), sa = sinf(a);
    float px = -sa, py = ca;
    r_hdr(h_flat);
    uint32_t nc = t->rpm > t->spec.redline_rpm * 0.97f ? 0xffff5030 : 0xfff09a2a;
    const float r0 = 34;
    r_tri2d(cx + ca * (R - 8), cy + sa * (R - 8), cx + px * 3.5f + ca * r0, cy + py * 3.5f + sa * r0,
            cx - px * 3.5f + ca * r0, cy - py * 3.5f + sa * r0, nc, nc, nc, Z + 0.5f);
    /* Speed in the middle, the gear in a tab in the gap at the bottom. */
    char buf[16];
    int mph = (int)(fabsf(t->speed) * MPS_TO_MPH + 0.5f);
    snprintf(buf, sizeof(buf), "%d", mph);
    hud_text_c(cx, cy - 4, 1.1f, ALIGN_CENTER, 0xffffffff, buf);
    hud_text_c(cx, cy + 17, 0.42f, ALIGN_CENTER, 0xffd8d0c0, "MPH");
    if(t->gear < 0) snprintf(buf, sizeof(buf), "R");
    else snprintf(buf, sizeof(buf), "%d", t->gear);
    r_hdr(h_flat);
    r_quad2d(cx - 12, cy + 34, cx + 12, cy + 56, 0, 0, 1, 1, t->gear < 0 ? 0xf0c04020 : 0xf0d08420, Z + 0.6f);
    hud_text_c(cx, cy + 45, 0.72f, ALIGN_CENTER, 0xffffffff, buf);
}

/* A thick 2D line from (x0,y0) to (x1,y1). */
static void seg2d(float x0, float y0, float x1, float y1, float w, uint32_t col) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if(l < 1e-3f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    float xy[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x0 - nx, y0 - ny, x1 - nx, y1 - ny};
    static const float uv[8] = {0};
    uint32_t c[4] = {col, col, col, col};
    r_quad2d_pts(xy, uv, c, Z + 0.3f);
}

/* The dashboard traction control lamp: a car from behind with two skid
   squiggles under it, on the bottom row left of the rev counter. Flashes while it
   is cutting the throttle; steady with OFF beside it when the driver has
   switched it off. */
static void tcs_icon(const Truck *t) {
    if(!t->spec.tcs) return;
    bool off = t->tcs_off;
    if(!off && tcs_lamp <= 0) return;
    const float k = 0.62f, cy = HUD_BOTTOM - 22 * k;
    const float cx = 560 - 66 - 10 - 22 * k;
    uint32_t col = off ? 0xffff7050 : 0xffffb040;
    if(!off && fmodf(game.time, 0.3f) > 0.18f) col = color_alpha(col, 0.35f);
    hud_panel(cx - (off ? 60 : 22) * k, cy - 22 * k, cx + 22 * k, cy + 22 * k, 0x90182028);
    r_hdr(h_flat);
#define PX(x) (cx + (x) * k)
#define PY(y) (cy + (y) * k)
    const uint32_t win = 0xff182028;
    /* Cabin, with a dark rear window, then the body and the tyres. */
    r_tri2d(PX(-7), PY(-15), PX(7), PY(-15), PX(-11), PY(-7), col, col, col, Z + 0.2f);
    r_tri2d(PX(7), PY(-15), PX(11), PY(-7), PX(-11), PY(-7), col, col, col, Z + 0.2f);
    r_tri2d(PX(-5), PY(-13), PX(5), PY(-13), PX(-8), PY(-9), win, win, win, Z + 0.3f);
    r_tri2d(PX(5), PY(-13), PX(8), PY(-9), PX(-8), PY(-9), win, win, win, Z + 0.3f);
    r_quad2d(PX(-14), PY(-7), PX(14), PY(1), 0, 0, 1, 1, col, Z + 0.2f);
    r_quad2d(PX(-13), PY(1), PX(-8), PY(5), 0, 0, 1, 1, col, Z + 0.2f);
    r_quad2d(PX(8), PY(1), PX(13), PY(5), 0, 0, 1, 1, col, Z + 0.2f);
    /* Skid marks: one S curve under each side, fanning out. */
    for(int side = -1; side <= 1; side += 2) {
        float px = 0, py = 0;
        for(int i = 0; i <= 8; i++) {
            float f = i / 8.0f;
            float x = PX(side * (7.0f + 3.0f * f) + 3.2f * sinf(f * 2.0f * ENG_PI));
            float y = PY(8 + 12 * f);
            if(i) seg2d(px, py, x, y, 2.4f * k + 0.3f, col);
            px = x;
            py = y;
        }
    }
#undef PX
#undef PY
    if(off) hud_text_c(cx - 39 * k, cy, 0.42f, ALIGN_CENTER, col, "OFF");
}

static void drive_mode(const Truck *t) {
    static const char *const names[DRIVE_COUNT] = {"2WD", "4WD", "LOCK"};
    float x = 34, y = HUD_BOTTOM - 15;
    for(int m = 0; m < DRIVE_COUNT; m++) {
        bool on = t->mode == m;
        float w = m == 2 ? 62 : 52;
        hud_panel(x, y - 15, x + w, y + 15, on ? 0xf0d08420 : 0x80202830);
        hud_text_c(x + w * 0.5f, y, 0.72f, ALIGN_CENTER, on ? 0xffffffff : 0xffb8b0a0, names[m]);
        x += w + 6;
    }
    if(t->mode == DRIVE_LOCK) hud_text_c(222, y, 0.48f, ALIGN_LEFT, 0xffe8c070, "LOW + LOCKERS");
    if(t->stalled) hud_text_c(320, 160, 0.7f, ALIGN_CENTER, 0xffff7050, "STALLED - WATER IN THE INTAKE");
}

static void inclinometer(const Truck *t) {
    /* Side view tilts with pitch, rear view with roll. */
    float y = HUD_BOTTOM - 91;
    hud_panel(28, y - 34, 212, y + 40, 0x70182028);
    r_hdr(h_icons);
    float p = clampf(t->pitch, -60, 60) * ENG_PI / 180.0f, r = clampf(t->roll, -60, 60) * ENG_PI / 180.0f;
    uint32_t cp = fabsf(t->pitch) > 30 ? 0xffff8040 : 0xfff4ecd8, cr = fabsf(t->roll) > 25 ? 0xffff8040 : 0xfff4ecd8;
    quad_rot(72, y - 2, 34, 17, -p, 0, 0, 0.5f, 1, cp);
    quad_rot(168, y - 2, 30, 15, r, 0.5f, 0, 1, 1, cr);
    char buf[16];
    snprintf(buf, sizeof(buf), "PITCH %d", (int)roundf(t->pitch));
    hud_text_c(72, y + 28, 0.5f, ALIGN_CENTER, cp, buf);
    snprintf(buf, sizeof(buf), "ROLL %d", (int)roundf(t->roll));
    hud_text_c(168, y + 28, 0.5f, ALIGN_CENTER, cr, buf);
}

/* The corner map, or the large one (X), which sits between the drive mode
   tabs, the speed and the summit panel and covers the inclinometer. */
static void map(void) {
    if(game.map_mode == 2) return;
    bool big = game.map_mode == 1;
    const float x0 = big ? 124 : 498, y0 = big ? 104 : 20, S = big ? 300 : 124;
    const float m = big ? 1.6f : 1.0f; /* marker size */
    hud_panel(x0 - 4, y0 - 4, x0 + S + 4, y0 + S + 4, big ? 0xc0182028 : 0xa0182028);
    r_hdr(h_map);
    r_quad2d(x0, y0, x0 + S, y0 + S, 0, 0, 1, 1, 0xffffffff, Z);
    float k = S / LEVEL_SIZE;
#define MX(x) (x0 + ((x) - LEVEL_ORG) * k)
#define MY(z) (y0 + S - ((z) - LEVEL_ORG) * k)
    r_hdr(h_flat);
    for(int i = 0; i < LVL_VIEWS; i++) {
        bool found = game.views_found & (1u << i);
        float x = MX(lvl_views[i].x), y = MY(lvl_views[i].z), r = 2.5f * m;
        uint32_t c = found ? 0xff40c060 : 0xffe04a8a;
        r_quad2d(x - r, y - r, x + r, y + r, 0, 0, 1, 1, c, Z + 0.2f);
    }
    for(int i = game.ntrucks - 1; i >= 0; i--) {
        const Truck *t = &trucks[i];
        if(t->absent) continue;
        float x = MX(t->pos.x), y = MY(t->pos.z), h = truck_heading(t);
        float fx = sinf(h), fy = -cosf(h);
        uint32_t c = i == 0 ? 0xffffffff : 0xfff0a030;
        float s = (i == 0 ? 6.0f : 4.0f) * m;
        r_tri2d(x + fx * s, y + fy * s, x - fx * s * 0.6f - fy * s * 0.6f, y - fy * s * 0.6f + fx * s * 0.6f,
                x - fx * s * 0.6f + fy * s * 0.6f, y - fy * s * 0.6f - fx * s * 0.6f, c, c, c, Z + 0.4f);
    }
    float sx = MX(LEVEL_SUMMIT_X), sy = MY(LEVEL_SUMMIT_Z);
    r_tri2d(sx, sy - 5 * m, sx + 4.5f * m, sy + 3 * m, sx - 4.5f * m, sy + 3 * m, 0xffffe060, 0xffffe060,
            0xffffe060, Z + 0.3f);
#undef MX
#undef MY
}

static void progress(const Truck *t) {
    hud_panel(20, 18, 234, 92, 0x90182028);
    char buf[32];
    int pct = (int)(game.summit_frac * 100.0f + 0.5f);
    snprintf(buf, sizeof(buf), "SUMMIT %d%%", pct);
    hud_text_c(30, 34, 0.75f, ALIGN_LEFT, 0xfff4ecd8, buf);
    /* The bar, with the best so far behind where you are now. */
    float cur = clampf((t->pos.y - LEVEL_START_Y) / (LEVEL_SUMMIT_Y - LEVEL_START_Y), 0, 1);
    hud_panel(30, 50, 224, 58, 0xa0404850);
    hud_panel(30, 50, 30 + 194 * game.summit_frac, 58, 0xc0e8a030);
    hud_panel(29 + 194 * cur, 47, 32 + 194 * cur, 61, 0xffffffff);
    int ft = (int)(2380.0f + t->pos.y * M_TO_FT);
    int found = 0;
    for(int i = 0; i < LVL_VIEWS; i++) found += (game.views_found >> i) & 1;
    snprintf(buf, sizeof(buf), "%d FT", ft);
    hud_text_c(30, 76, 0.52f, ALIGN_LEFT, 0xffd8d0c0, buf);
    snprintf(buf, sizeof(buf), "VIEWS %d/%d", found, LVL_VIEWS);
    hud_text_c(224, 76, 0.52f, ALIGN_RIGHT, 0xffd8d0c0, buf);
}

void hud_draw_map(void) {
    map();
}

void hud_draw(void) {
    if(game.hud_hidden) return;
    const Truck *t = &trucks[0];
    progress(t);
    map();
    dial(t);
    tcs_icon(t);
    drive_mode(t);
    if(game.map_mode != 1) inclinometer(t);
    if(msg_t > 0) {
        float a = clampf(msg_t / 0.4f, 0, 1) * clampf((msg_len - msg_t) / 0.15f, 0, 1);
        hud_text_c(320, 124, 1.0f, ALIGN_CENTER, color_alpha(msg_col, a), msg);
        if(msg_sub[0]) hud_text_c(320, 162, 0.62f, ALIGN_CENTER, color_alpha(0xfff8f0dc, a), msg_sub);
    }
    if(hud_hold_k > 0 && hud_hold_label) {
        hud_panel(240, 300, 400, 330, 0xa0182028);
        hud_panel(244, 322, 244 + 152 * hud_hold_k, 326, 0xffe8a030);
        hud_text_c(320, 311, 0.6f, ALIGN_CENTER, 0xffffffff, hud_hold_label);
    }
}
