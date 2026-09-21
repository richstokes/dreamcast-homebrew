#include "screen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "font8x16.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

screen_t scr;

static const uint32_t ega_palette[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500,
    0xaaaaaa, 0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff,
    0xffff55, 0xffffff
};

static void mark_dirty(int top, int bottom) {
    if(top < 0)
        top = 0;
    if(bottom >= SCR_H)
        bottom = SCR_H - 1;
    if(top > bottom)
        return;
    if(scr.dirty_top > scr.dirty_bottom) {
        scr.dirty_top = top;
        scr.dirty_bottom = bottom;
        return;
    }
    if(top < scr.dirty_top)
        scr.dirty_top = top;
    if(bottom > scr.dirty_bottom)
        scr.dirty_bottom = bottom;
}

/* The VGA default palette: EGA colours, a grey ramp, then hue wheels at three
   brightness and three saturation levels. */
void scr_reset_palette(void) {
    static const uint8_t hue[24][3] = {
        {0, 0, 4}, {1, 0, 4}, {2, 0, 4}, {3, 0, 4}, {4, 0, 4}, {4, 0, 3},
        {4, 0, 2}, {4, 0, 1}, {4, 0, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0},
        {4, 4, 0}, {3, 4, 0}, {2, 4, 0}, {1, 4, 0}, {0, 4, 0}, {0, 4, 1},
        {0, 4, 2}, {0, 4, 3}, {0, 4, 4}, {0, 3, 4}, {0, 2, 4}, {0, 1, 4}
    };
    static const int level[3] = {255, 113, 65};
    static const int floor_percent[3] = {0, 49, 72};
    int i, bright, sat, h, c;

    memcpy(scr.palette, ega_palette, sizeof(ega_palette));
    for(i = 0; i < 16; ++i) {
        int v = i * 255 / 15;
        scr.palette[16 + i] = (uint32_t)(v << 16 | v << 8 | v);
    }
    i = 32;
    for(bright = 0; bright < 3; ++bright) {
        for(sat = 0; sat < 3; ++sat) {
            for(h = 0; h < 24; ++h) {
                uint32_t rgb = 0;
                for(c = 0; c < 3; ++c) {
                    int top = level[bright];
                    int low = top * floor_percent[sat] / 100;
                    int v = low + (top - low) * hue[h][c] / 4;
                    rgb = rgb << 8 | (uint32_t)v;
                }
                scr.palette[i++] = rgb;
            }
        }
    }
    while(i < 256)
        scr.palette[i++] = 0;
    scr.palette_dirty = true;
    mark_dirty(0, SCR_H - 1);
}

void scr_set_palette(int index, int r, int g, int b) {
    if(index < 0 || index > 255)
        return;
    scr.palette[index] = (uint32_t)((r & 255) << 16 | (g & 255) << 8 |
                                    (b & 255));
    scr.palette_dirty = true;
    mark_dirty(0, SCR_H - 1);
}

bool scr_mode_supported(int mode) {
    switch(mode) {
        case 0: case 1: case 2: case 7: case 8: case 9: case 12: case 13:
            return true;
        default:
            return false;
    }
}

void scr_set_mode(int mode) {
    if(!scr_mode_supported(mode))
        mode = 0;
    scr.mode = mode;
    scr.scale_x = scr.scale_y = 1;
    scr.origin_x = scr.origin_y = 0;
    scr.log_w = 640;
    scr.log_h = 480;
    scr.cell_w = 8;
    /* Text is inset so a television's overscan cannot hide it. */
    scr.text_x = 16;
    scr.text_y = 16;
    scr.cols = 76;
    scr.rows = 28;

    if(mode == 1 || mode == 7 || mode == 13) {
        scr.log_w = 320;
        scr.log_h = 200;
        scr.scale_x = scr.scale_y = 2;
    }
    else if(mode == 2 || mode == 8) {
        scr.log_h = 200;
        scr.scale_y = 2;
    }
    else if(mode == 9) {
        scr.log_h = 350;
    }
    if(mode != 0 && mode != 12) {
        /* PC modes keep their exact text/graphics alignment. */
        scr.origin_y = (SCR_H - scr.log_h * scr.scale_y) / 2;
        if(mode == 9)
            scr.origin_y = 40;
        scr.text_x = 0;
        scr.text_y = 40;
        scr.cell_w = 8 * scr.scale_x;
        scr.cols = SCR_W / scr.cell_w;
        scr.rows = 25;
    }
    scr.fg = mode == 13 ? 15 : 7;
    scr.bg = 0;
    scr_reset_palette();
    memset(scr.pixels, 0, sizeof(scr.pixels));
    scr.cur_col = scr.cur_row = 0;
    mark_dirty(0, SCR_H - 1);
}

void scr_init(void) {
    memset(&scr, 0, sizeof(scr));
    scr.dirty_top = 1;
    scr.dirty_bottom = 0;
    scr.status_on = true;
    scr_set_mode(0);
}

void scr_cls(void) {
    memset(scr.pixels, scr.bg, sizeof(scr.pixels));
    scr.cur_col = scr.cur_row = 0;
    mark_dirty(0, SCR_H - 1);
}

int scr_status_y(void) {
    return 16 + 27 * SCR_CELL_H;
}

void scr_status_mask(uint8_t mask[SCR_CELL_H][SCR_W]) {
    int i;

    memset(mask, 0, (size_t)SCR_CELL_H * SCR_W);
    for(i = 0; scr.status[i] && i < (SCR_W - 16) / 8; ++i)
        scr_glyph(mask[0] + 16 + i * 8, SCR_W, (unsigned char)scr.status[i],
                  8, 1, 0);
}

void scr_cursor_rect(int *x, int *y, int *width, int *height) {
    *x = scr.text_x + scr.cur_col * scr.cell_w;
    *y = scr.text_y + scr.cur_row * SCR_CELL_H + SCR_CELL_H - 3;
    *width = scr.cell_w;
    *height = 2;
}

int scr_usable_rows(void) {
    if(scr.status_on && (scr.mode == 0 || scr.mode == 12))
        return scr.rows - 1;
    return scr.rows;
}

void scr_set_status(const char *text) {
    if(strncmp(scr.status, text, SCR_STATUS_MAX) == 0)
        return;
    strncpy(scr.status, text, SCR_STATUS_MAX);
    scr.status[SCR_STATUS_MAX] = '\0';
    scr.status_dirty = true;
}

/* Code page 437 block and shade characters that games commonly print. */
static bool synthetic_glyph_bit(int ch, int x, int y) {
    switch(ch) {
        case 176: return (x + y * 2) % 4 == 0;
        case 177: return (x + y) % 2 == 0;
        case 178: return (x + y * 2) % 4 != 0;
        case 219: return true;
        case 220: return y >= 8;
        case 221: return x < 4;
        case 222: return x >= 4;
        case 223: return y < 8;
        case 196: return y == 7;
        case 179: return x == 3 || x == 4;
        case 254: return x >= 1 && x <= 6 && y >= 5 && y <= 10;
        default: return false;
    }
}

void scr_glyph(uint8_t *dest, int stride, int ch, int cell_w, int fg,
               int bg) {
    const unsigned char *glyph = NULL;
    const int repeat = cell_w / 8;
    int x, y, r;

    ch &= 255;
    if(ch >= FONT_FIRST && ch <= FONT_LAST)
        glyph = font8x16[ch - FONT_FIRST];
    for(y = 0; y < SCR_CELL_H; ++y) {
        uint8_t *out = dest + y * stride;
        for(x = 0; x < 8; ++x) {
            bool on = glyph ? (glyph[y] & (0x80 >> x)) != 0 :
                              synthetic_glyph_bit(ch, x, y);
            for(r = 0; r < repeat; ++r)
                *out++ = (uint8_t)(on ? fg : bg);
        }
    }
}

void scr_draw_char(int col, int row, int ch, int fg, int bg) {
    int x, y;

    if(col < 0 || row < 0 || col >= scr.cols || row >= scr.rows)
        return;
    x = scr.text_x + col * scr.cell_w;
    y = scr.text_y + row * SCR_CELL_H;
    scr_glyph(scr.pixels + y * SCR_W + x, SCR_W, ch, scr.cell_w, fg, bg);
    mark_dirty(y, y + SCR_CELL_H - 1);
}

void scr_draw_text(int col, int row, const char *text, int fg, int bg) {
    while(*text && col < scr.cols)
        scr_draw_char(col++, row, (unsigned char)*text++, fg, bg);
}

void scr_fill_cells(int col, int row, int width, int height, int bg) {
    int y;
    int x = scr.text_x + col * scr.cell_w;
    int top = scr.text_y + row * SCR_CELL_H;

    for(y = top; y < top + height * SCR_CELL_H && y < SCR_H; ++y)
        memset(scr.pixels + y * SCR_W + x, bg, (size_t)(width * scr.cell_w));
    mark_dirty(top, top + height * SCR_CELL_H - 1);
}

static void scroll_up(void) {
    const int usable = scr_usable_rows();
    const int top = scr.text_y;
    const int height = (usable - 1) * SCR_CELL_H;

    memmove(scr.pixels + top * SCR_W,
            scr.pixels + (top + SCR_CELL_H) * SCR_W, (size_t)height * SCR_W);
    memset(scr.pixels + (top + height) * SCR_W, scr.bg,
           (size_t)SCR_CELL_H * SCR_W);
    mark_dirty(top, top + usable * SCR_CELL_H - 1);
}

static void newline(void) {
    scr.cur_col = 0;
    if(++scr.cur_row >= scr_usable_rows()) {
        scr.cur_row = scr_usable_rows() - 1;
        scroll_up();
    }
}

void scr_putc(int ch) {
    ch &= 255;
    if(ch == '\n') {
        newline();
        return;
    }
    if(ch == '\r') {
        scr.cur_col = 0;
        return;
    }
    if(ch == 8) {
        if(scr.cur_col > 0)
            --scr.cur_col;
        return;
    }
    if(ch == 7)
        return;
    if(ch == 9) {
        do {
            scr_putc(' ');
        } while(scr.cur_col % 8);
        return;
    }
    if(ch == 12) {
        scr_cls();
        return;
    }
    if(scr.cur_row >= scr_usable_rows())
        scr.cur_row = scr_usable_rows() - 1;
    scr_draw_char(scr.cur_col, scr.cur_row, ch, scr.fg, scr.bg);
    if(++scr.cur_col >= scr.cols)
        newline();
}

void scr_puts(const char *text) {
    while(*text)
        scr_putc((unsigned char)*text++);
}

void scr_locate(int row, int col) {
    if(row < 0)
        row = 0;
    if(row >= scr.rows)
        row = scr.rows - 1;
    if(col < 0)
        col = 0;
    if(col >= scr.cols)
        col = scr.cols - 1;
    scr.cur_row = row;
    scr.cur_col = col;
}

/* Fills logical columns x1..x2 of one logical row. */
static void span(int x1, int x2, int y, int color) {
    int row;
    int top;

    if(y < 0 || y >= scr.log_h)
        return;
    if(x1 < 0)
        x1 = 0;
    if(x2 >= scr.log_w)
        x2 = scr.log_w - 1;
    if(x1 > x2)
        return;
    top = scr.origin_y + y * scr.scale_y;
    for(row = top; row < top + scr.scale_y; ++row) {
        memset(scr.pixels + row * SCR_W + scr.origin_x + x1 * scr.scale_x,
               color, (size_t)((x2 - x1 + 1) * scr.scale_x));
    }
    mark_dirty(top, top + scr.scale_y - 1);
}

void scr_pset(int x, int y, int color) {
    if(x < 0 || x >= scr.log_w)
        return;
    span(x, x, y, color);
}

int scr_point(int x, int y) {
    if(x < 0 || y < 0 || x >= scr.log_w || y >= scr.log_h)
        return -1;
    return scr.pixels[(scr.origin_y + y * scr.scale_y) * SCR_W +
                      scr.origin_x + x * scr.scale_x];
}

static bool clip_far_line(int *x1, int *y1, int *x2, int *y2) {
    /* Keeps Bresenham bounded when a program draws far off screen. */
    const double limit = 4096.0;
    double ax = *x1, ay = *y1, bx = *x2, by = *y2;
    int pass;

    for(pass = 0; pass < 2; ++pass) {
        double *px = pass ? &bx : &ax, *py = pass ? &by : &ay;
        double ox = pass ? ax : bx, oy = pass ? ay : by;
        if(fabs(*px) > limit) {
            double target = *px > 0 ? limit : -limit;
            if(fabs(ox) > limit && (ox > 0) == (*px > 0))
                return false;
            *py = oy + (*py - oy) * (target - ox) / (*px - ox);
            *px = target;
        }
        if(fabs(*py) > limit) {
            double target = *py > 0 ? limit : -limit;
            if(fabs(oy) > limit && (oy > 0) == (*py > 0))
                return false;
            *px = ox + (*px - ox) * (target - oy) / (*py - oy);
            *py = target;
        }
    }
    *x1 = (int)ax; *y1 = (int)ay; *x2 = (int)bx; *y2 = (int)by;
    return true;
}

void scr_line(int x1, int y1, int x2, int y2, int color) {
    int dx, dy, sx, sy, err;

    if(!clip_far_line(&x1, &y1, &x2, &y2))
        return;
    if(y1 == y2) {
        span(x1 < x2 ? x1 : x2, x1 < x2 ? x2 : x1, y1, color);
        return;
    }
    dx = abs(x2 - x1);
    dy = -abs(y2 - y1);
    sx = x1 < x2 ? 1 : -1;
    sy = y1 < y2 ? 1 : -1;
    err = dx + dy;
    for(;;) {
        int e2;
        scr_pset(x1, y1, color);
        if(x1 == x2 && y1 == y2)
            break;
        e2 = 2 * err;
        if(e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if(e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
}

void scr_box(int x1, int y1, int x2, int y2, int color, bool fill) {
    int y;

    if(x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if(y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if(fill) {
        if(y1 < 0)
            y1 = 0;
        if(y2 >= scr.log_h)
            y2 = scr.log_h - 1;
        for(y = y1; y <= y2; ++y)
            span(x1, x2, y, color);
        return;
    }
    scr_line(x1, y1, x2, y1, color);
    scr_line(x1, y2, x2, y2, color);
    scr_line(x1, y1, x1, y2, color);
    scr_line(x2, y1, x2, y2, color);
}

void scr_circle(int cx, int cy, double radius, int color, double start,
                double end, double aspect) {
    const bool spoke_start = start < 0, spoke_end = end < 0;
    double rx = radius, ry = radius;
    double sweep, step, angle;
    int steps, i, px = 0, py = 0;

    if(radius < 0 || radius > 8192)
        return;
    if(aspect <= 0)
        aspect = 1;
    if(aspect < 1)
        ry = radius * aspect;
    else
        rx = radius / aspect;
    start = fabs(start);
    end = fabs(end);
    sweep = end - start;
    if(sweep <= 0)
        sweep += 2 * M_PI;
    steps = (int)(radius * sweep) + 8;
    if(steps > 4096)
        steps = 4096;
    step = sweep / steps;
    for(i = 0; i <= steps; ++i) {
        int x, y;
        angle = start + step * i;
        x = cx + (int)lround(cos(angle) * rx);
        y = cy - (int)lround(sin(angle) * ry);
        if(i == 0)
            scr_pset(x, y, color);
        else
            scr_line(px, py, x, y, color);
        if((i == 0 && spoke_start) || (i == steps && spoke_end))
            scr_line(cx, cy, x, y, color);
        px = x;
        py = y;
    }
}

typedef struct { short x1, x2, y, dy; } fill_span_t;

void scr_paint(int x, int y, int color, int border) {
    fill_span_t *stack;
    int capacity = 1024, count = 0;

#define FILLABLE(px, py) \
    (scr_point((px), (py)) != border && scr_point((px), (py)) != color)
#define PUSH(a, b, c, d) do { \
        if((c) + (d) >= 0 && (c) + (d) < scr.log_h) { \
            if(count == capacity) { \
                fill_span_t *grown = realloc(stack, sizeof(*stack) * \
                                             (size_t)capacity * 2); \
                if(!grown) { free(stack); return; } \
                stack = grown; capacity *= 2; \
            } \
            stack[count].x1 = (short)(a); stack[count].x2 = (short)(b); \
            stack[count].y = (short)(c); stack[count].dy = (short)(d); \
            ++count; \
        } \
    } while(0)

    if(x < 0 || y < 0 || x >= scr.log_w || y >= scr.log_h ||
       !FILLABLE(x, y))
        return;
    stack = malloc(sizeof(*stack) * (size_t)capacity);
    if(!stack)
        return;
    PUSH(x, x, y, 1);
    PUSH(x, x, y + 1, -1);
    while(count > 0) {
        fill_span_t s = stack[--count];
        int row = s.y + s.dy;
        int left = s.x1, cursor;

        if(FILLABLE(left, row)) {
            while(left > 0 && FILLABLE(left - 1, row))
                --left;
            cursor = s.x1;
        }
        else {
            cursor = s.x1;
            while(cursor <= s.x2 && !FILLABLE(cursor, row))
                ++cursor;
            left = cursor;
        }
        while(cursor <= s.x2) {
            int start = left;
            while(cursor < scr.log_w && FILLABLE(cursor, row))
                ++cursor;
            if(cursor > start) {
                span(start, cursor - 1, row, color);
                PUSH(start, cursor - 1, row, s.dy);
                if(start < s.x1)
                    PUSH(start, s.x1 - 1, row, -s.dy);
                if(cursor - 1 > s.x2)
                    PUSH(s.x2 + 1, cursor - 1, row, -s.dy);
            }
            ++cursor;
            while(cursor <= s.x2 && !FILLABLE(cursor, row))
                ++cursor;
            left = cursor;
        }
    }
    free(stack);
#undef PUSH
#undef FILLABLE
}
