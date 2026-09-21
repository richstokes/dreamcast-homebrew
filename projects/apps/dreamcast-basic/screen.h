/*
 * Portable 640x480 indexed-colour screen shared by the editor, the dialogs and
 * running BASIC programs. The platform layer converts it to the display.
 */
#ifndef SCREEN_H
#define SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#define SCR_W 640
#define SCR_H 480
#define SCR_CELL_H 16
#define SCR_STATUS_MAX 80

typedef struct {
    uint8_t pixels[SCR_W * SCR_H];
    uint32_t palette[256];          /* 0x00RRGGBB */
    bool palette_dirty;
    int dirty_top, dirty_bottom;    /* inclusive rows; top > bottom = clean */

    /* Current BASIC screen mode. */
    int mode;
    int log_w, log_h;               /* logical graphics resolution */
    int scale_x, scale_y;           /* logical pixel size on the display */
    int origin_x, origin_y;         /* display position of logical (0,0) */

    /* Text grid. */
    int text_x, text_y;             /* display position of the grid */
    int cell_w;                     /* 8 or 16 display pixels */
    int cols, rows;
    int cur_col, cur_row;           /* zero based */
    int fg, bg;
    bool cursor_on;

    /* One-row overlay the platform composes over the bottom text row. */
    bool status_on;
    char status[SCR_STATUS_MAX + 1];
    bool status_dirty;
} screen_t;

extern screen_t scr;

void scr_init(void);
void scr_set_mode(int mode);        /* false modes fall back to 0 */
bool scr_mode_supported(int mode);
void scr_reset_palette(void);
void scr_set_palette(int index, int r, int g, int b);
void scr_cls(void);
void scr_set_status(const char *text);
int scr_status_y(void);
/* One byte per pixel of the status row: 1 where its text is inked. */
void scr_status_mask(uint8_t mask[SCR_CELL_H][SCR_W]);
/* Display rectangle of the text cursor. */
void scr_cursor_rect(int *x, int *y, int *width, int *height);
int scr_usable_rows(void);          /* rows not covered by the status bar */

/* Text. */
void scr_putc(int ch);
void scr_puts(const char *text);
void scr_locate(int row, int col);  /* zero based, clamped */
void scr_draw_char(int col, int row, int ch, int fg, int bg);
void scr_draw_text(int col, int row, const char *text, int fg, int bg);
void scr_fill_cells(int col, int row, int width, int height, int bg);
void scr_glyph(uint8_t *dest, int stride, int ch, int cell_w, int fg, int bg);

/* Graphics, in logical coordinates. */
void scr_pset(int x, int y, int color);
int scr_point(int x, int y);
void scr_line(int x1, int y1, int x2, int y2, int color);
void scr_box(int x1, int y1, int x2, int y2, int color, bool fill);
void scr_circle(int cx, int cy, double radius, int color, double start,
                double end, double aspect);
void scr_paint(int x, int y, int color, int border);

#endif
