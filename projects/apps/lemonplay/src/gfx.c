/* The BIOS font is drawn once into a 256x512 ARGB4444 atlas (21 columns of
   12x24 glyphs for codes 32-255); text is then one translucent quad per
   glyph, tinted by vertex colour. */
#include "gfx.h"

#include "icons.h"

#include <dc/biosfont.h>
#include <kos.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_W 256
#define ATLAS_H 512
#define ATLAS_COLS (ATLAS_W / FONT_W)
#define ICONS_W 256
#define ICONS_H 64

enum { HDR_NONE, HDR_FLAT, HDR_FONT, HDR_FONT_SMOOTH, HDR_ICONS, HDR_OTHER };

static pvr_poly_hdr_t flat_op_hdr, flat_tr_hdr, font_hdr, font_smooth_hdr, icons_hdr;
static pvr_ptr_t atlas, icons;
static int list_open = -1;   /* PVR list being filled, or -1 */
static int current_hdr = HDR_NONE;

static void build_atlas(void) {
    uint16_t *pixels = calloc(ATLAS_W * ATLAS_H, sizeof(uint16_t));
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    for(int c = 32; c < 256; c++) {
        int slot = c - 32;
        uint16_t *cell = pixels + (slot / ATLAS_COLS) * FONT_H * ATLAS_W + (slot % ATLAS_COLS) * FONT_W;
        if(c >= 127 && c < 160) continue;   /* no glyphs here */
        bfont_draw_ex(cell, ATLAS_W, 0xffff, 0x0fff, 16, true, c, false, false);
    }
    atlas = pvr_mem_malloc(ATLAS_W * ATLAS_H * 2);
    pvr_txr_load(pixels, atlas, ATLAS_W * ATLAS_H * 2);
    free(pixels);

    pixels = calloc(ICONS_W * ICONS_H, sizeof(uint16_t));
    icons_draw(pixels, ICONS_W);
    icons = pvr_mem_malloc(ICONS_W * ICONS_H * 2);
    pvr_txr_load(pixels, icons, ICONS_W * ICONS_H * 2);
    free(pixels);
}

/* Translucent draws stack in submission order (autosort is off), so they
   ignore depth: everything sits at the same z. */
static void compile_tr_txr(pvr_poly_hdr_t *hdr, int w, int h, pvr_ptr_t txr, int filter, int shading) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED, w, h, txr, filter);
    cxt.gen.shading = shading;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(hdr, &cxt);
}

void gfx_init(void) {
    pvr_init_params_t params = {
        {PVR_BINSIZE_16, 0, PVR_BINSIZE_16, 0, 0},
        512 * 1024, 0, 0, 1, 2, 0   /* translucent polys draw in submission order */
    };
    pvr_init(&params);
    pvr_set_bg_color(0.08f, 0.075f, 0.047f);

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&flat_op_hdr, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&flat_tr_hdr, &cxt);

    build_atlas();
    /* Text at whole-number sizes stays pixel-sharp; other sizes and the
       icons are filtered. */
    compile_tr_txr(&font_hdr, ATLAS_W, ATLAS_H, atlas, PVR_FILTER_NONE, PVR_SHADE_FLAT);
    compile_tr_txr(&font_smooth_hdr, ATLAS_W, ATLAS_H, atlas, PVR_FILTER_BILINEAR, PVR_SHADE_FLAT);
    compile_tr_txr(&icons_hdr, ICONS_W, ICONS_H, icons, PVR_FILTER_BILINEAR, PVR_SHADE_FLAT);
}

int gfx_ready(void) {
    return pvr_check_ready() == 0;
}

void gfx_begin(void) {
    pvr_wait_ready();
    pvr_scene_begin();
    list_open = -1;
    current_hdr = HDR_NONE;
}

static void use_list(int list) {
    if(list_open == list) return;
    if(list_open >= 0) pvr_list_finish();
    pvr_list_begin(list);
    list_open = list;
    current_hdr = HDR_NONE;
}

void gfx_end(void) {
    if(list_open >= 0) pvr_list_finish();
    list_open = -1;
    pvr_scene_finish();
}

static void quad(float x, float y, float w, float h, uint32_t argb,
                 float u0, float v0, float u1, float v1) {
    pvr_vertex_t v;
    v.z = 1.0f;
    v.argb = argb;
    v.oargb = 0;
    v.flags = PVR_CMD_VERTEX;
    v.x = x;     v.y = y;     v.u = u0; v.v = v0; pvr_prim(&v, sizeof(v));
    v.x = x + w; v.y = y;     v.u = u1; v.v = v0; pvr_prim(&v, sizeof(v));
    v.x = x;     v.y = y + h; v.u = u0; v.v = v1; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = x + w; v.y = y + h; v.u = u1; v.v = v1; pvr_prim(&v, sizeof(v));
}

void gfx_rect(float x, float y, float w, float h, uint32_t argb) {
    use_list(PVR_LIST_OP_POLY);
    if(current_hdr != HDR_FLAT) {
        pvr_prim(&flat_op_hdr, sizeof(flat_op_hdr));
        current_hdr = HDR_FLAT;
    }
    quad(x, y, w, h, argb, 0, 0, 0, 0);
}

void gfx_rect_alpha(float x, float y, float w, float h, uint32_t argb) {
    use_list(PVR_LIST_TR_POLY);
    if(current_hdr != HDR_FLAT) {
        pvr_prim(&flat_tr_hdr, sizeof(flat_tr_hdr));
        current_hdr = HDR_FLAT;
    }
    quad(x, y, w, h, argb, 0, 0, 0, 0);
}

void gfx_textured(const pvr_poly_hdr_t *hdr, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1) {
    use_list(PVR_LIST_OP_POLY);
    pvr_prim(hdr, sizeof(*hdr));
    current_hdr = HDR_OTHER;
    quad(x, y, w, h, 0xffffffff, u0, v0, u1, v1);
}

static void glyph(float x, float y, uint32_t argb, float scale, unsigned char c) {
    if(c < 33 || (c >= 127 && c < 160)) return;
    int slot = c - 32;
    float u0 = (float)((slot % ATLAS_COLS) * FONT_W) / ATLAS_W;
    float v0 = (float)((slot / ATLAS_COLS) * FONT_H) / ATLAS_H;
    quad(x, y, FONT_W * scale, FONT_H * scale, argb,
         u0, v0, u0 + (float)FONT_W / ATLAS_W, v0 + (float)FONT_H / ATLAS_H);
}

static void font_begin(float scale) {
    int smooth = scale != (int)scale;
    int want = smooth ? HDR_FONT_SMOOTH : HDR_FONT;
    use_list(PVR_LIST_TR_POLY);
    if(current_hdr != want) {
        pvr_prim(smooth ? &font_smooth_hdr : &font_hdr, sizeof(font_hdr));
        current_hdr = want;
    }
}

float gfx_text(float x, float y, uint32_t argb, float scale, const char *text) {
    font_begin(scale);
    for(const unsigned char *p = (const unsigned char *)text; *p; p++) {
        glyph(x, y, argb, scale, *p);
        x += FONT_W * scale;
    }
    return x;
}

void gfx_text_clip(float x, float y, uint32_t argb, float scale, const char *text, float max_w) {
    size_t len = strlen(text);
    int fit = (int)(max_w / (FONT_W * scale));
    if((int)len <= fit) {
        gfx_text(x, y, argb, scale, text);
        return;
    }
    if(fit < 4) return;
    font_begin(scale);
    for(int i = 0; i < fit - 3; i++) {
        glyph(x, y, argb, scale, (unsigned char)text[i]);
        x += FONT_W * scale;
    }
    gfx_text(x, y, argb, scale, "...");
}

float gfx_text_width(const char *text, float scale) {
    return strlen(text) * FONT_W * scale;
}

void gfx_gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    use_list(PVR_LIST_TR_POLY);
    if(current_hdr != HDR_FLAT) {
        pvr_prim(&flat_tr_hdr, sizeof(flat_tr_hdr));
        current_hdr = HDR_FLAT;
    }
    pvr_vertex_t v = {0};
    v.z = 1.0f;
    v.flags = PVR_CMD_VERTEX;
    v.argb = top;    v.x = x;     v.y = y;     pvr_prim(&v, sizeof(v));
    v.argb = top;    v.x = x + w; v.y = y;     pvr_prim(&v, sizeof(v));
    v.argb = bottom; v.x = x;     v.y = y + h; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.argb = bottom; v.x = x + w; v.y = y + h; pvr_prim(&v, sizeof(v));
}

/* Part of an icon's cell, by fractions of its width. */
static void icon_part(int icon, float x, float y, float w, float h, uint32_t argb, float f0, float f1) {
    int cx, cy, size;
    icons_cell(icon, &cx, &cy, &size);
    use_list(PVR_LIST_TR_POLY);
    if(current_hdr != HDR_ICONS) {
        pvr_prim(&icons_hdr, sizeof(icons_hdr));
        current_hdr = HDR_ICONS;
    }
    float u0 = (cx + size * f0) / ICONS_W, u1 = (cx + size * f1) / ICONS_W;
    quad(x, y, w, h, argb, u0, (float)cy / ICONS_H, u1, (float)(cy + size) / ICONS_H);
}

void gfx_icon(int icon, float x, float y, float size, uint32_t argb) {
    icon_part(icon, x, y, size, size, argb, 0, 1);
}

void gfx_pill(float x, float y, float w, float h, uint32_t argb) {
    float r = h / 2;
    if(w < h) w = h;
    icon_part(ICON_DISC, x, y, r, h, argb, 0, 0.5f);
    gfx_rect_alpha(x + r, y, w - h, h, argb);
    icon_part(ICON_DISC, x + w - r, y, r, h, argb, 0.5f, 1);
}

/* Dreamcast controller colours. */
float gfx_button(float x, float y, char button) {
    uint32_t colour, ink = 0xffffffff;
    switch(button) {
        case 'A': colour = 0xffd64534; break;
        case 'B': colour = 0xff3f6fd6; break;
        case 'X': colour = 0xffe8bd2a; ink = 0xff2a2410; break;
        case 'Y': colour = 0xff39a659; break;
        default:  colour = 0xff6b6658; break;
    }
    if(button == 'S' || button == 'L' || button == 'R') {
        const char *label = button == 'S' ? "START" : button == 'L' ? "L" : "R";
        float w = gfx_text_width(label, 0.75f) + 16;
        gfx_pill(x, y + 2, w, 20, colour);
        gfx_text(x + 8, y + 3, ink, 0.75f, label);
        return x + w;
    }
    gfx_icon(ICON_DISC, x, y, 24, colour);
    char letter[2] = {button, 0};
    gfx_text(x + 7.5f, y + 3, ink, 0.75f, letter);
    return x + 24;
}

float gfx_hint(float x, float y, char button, const char *label, uint32_t argb) {
    x = gfx_button(x, y, button) + 8;
    return gfx_text(x, y, argb, 1, label) + 24;
}

float gfx_hint_width(char button, const char *label) {
    float b = button == 'S' ? gfx_text_width("START", 0.75f) + 16 : button == 'L' || button == 'R' ? 25 : 24;
    return b + 8 + gfx_text_width(label, 1);
}

void utf8_to_latin1(char *dst, size_t size, const char *src) {
    size_t n = 0;
    const unsigned char *s = (const unsigned char *)src;
    while(*s && n + 1 < size) {
        unsigned cp;
        int extra;
        if(*s < 0x80) { cp = *s; extra = 0; }
        else if((*s & 0xe0) == 0xc0) { cp = *s & 0x1f; extra = 1; }
        else if((*s & 0xf0) == 0xe0) { cp = *s & 0x0f; extra = 2; }
        else if((*s & 0xf8) == 0xf0) { cp = *s & 0x07; extra = 3; }
        else { s++; dst[n++] = '?'; continue; }
        s++;
        for(int i = 0; i < extra; i++) {
            if((*s & 0xc0) != 0x80) { cp = '?'; break; }
            cp = (cp << 6) | (*s++ & 0x3f);
        }
        if(cp == 0x2018 || cp == 0x2019) cp = '\'';
        else if(cp == 0x201c || cp == 0x201d) cp = '"';
        else if(cp == 0x2013 || cp == 0x2014) cp = '-';
        else if(cp == 0x2026) cp = '.';
        dst[n++] = (cp < 32 || (cp >= 127 && cp < 160) || cp > 255) ? (cp == '\t' ? ' ' : '?') : (char)cp;
    }
    dst[n] = '\0';
}
