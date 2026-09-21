#include "browser.h"

#include <dc/biosfont.h>
#include <dc/sq.h>
#include <dc/video.h>
#include <stdio.h>
#include <string.h>

#define C_WHITE 0xffff
#define C_PAGE 0xef7d
#define C_INK 0x18e3
#define C_BLUE 0x1b3f
#define C_TEAL 0x0414
#define C_HEADING 0x9a20
#define C_STRONG 0x0000
#define C_EMPHASIS 0x7a2f
#define C_MUTED 0x738e
#define C_RULE 0xbdf7
#define C_FOCUS 0xff28
#define C_ERROR 0xd945

/* Frames are drawn in cached main RAM, where the many small glyph writes
   are cheap, then burst to video RAM through the store queues. Writing
   each pixel straight to uncached VRAM was several times slower. */
static uint16_t frame[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));

typedef uint32_t __attribute__((may_alias)) pixel_pair_t;

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    pixel_pair_t pair = ((uint32_t)color << 16) | color;
    int row;
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }
    if(x + w > SCREEN_W) w = SCREEN_W - x;
    if(y + h > SCREEN_H) h = SCREEN_H - y;
    if(w <= 0 || h <= 0) return;
    for(row = 0; row < h; ++row) {
        uint16_t *pixel = frame + (y + row) * SCREEN_W + x;
        pixel_pair_t *pairs;
        int n = w;
        if(((uintptr_t)pixel & 2) && n) {
            *pixel++ = color;
            n--;
        }
        pairs = (pixel_pair_t *)pixel;
        for(; n >= 8; n -= 8) {
            pairs[0] = pair;
            pairs[1] = pair;
            pairs[2] = pair;
            pairs[3] = pair;
            pairs += 4;
        }
        for(; n >= 2; n -= 2) *pairs++ = pair;
        if(n) *(uint16_t *)pairs = color;
    }
}

/* The BIOS font is read under a BIOS syscall lock per character, which
   dominated frame time on text-heavy pages. Each ISO-8859-1 glyph is
   instead captured once into 12-bit rows and drawn from RAM. */
static uint16_t glyph_rows[256][BFONT_HEIGHT];
static int glyphs_ready;

static void load_glyphs(void) {
    static uint16_t scratch[BFONT_THIN_WIDTH * BFONT_HEIGHT];
    int c, row, column;
    for(c = 0; c < 256; ++c) {
        memset(scratch, 0, sizeof(scratch));
        bfont_draw_ex(scratch, BFONT_THIN_WIDTH, 0xffff, 0, 16, true, (uint32_t)c,
                      false, false);
        for(row = 0; row < BFONT_HEIGHT; ++row) {
            uint16_t bits = 0;
            for(column = 0; column < BFONT_THIN_WIDTH; ++column)
                if(scratch[row * BFONT_THIN_WIDTH + column])
                    bits |= (uint16_t)(0x800 >> column);
            glyph_rows[c][row] = bits;
        }
    }
    glyphs_ready = 1;
}

/* Four pixels for each 4-bit slice of a glyph row, per color pair. */
typedef struct {
    uint16_t fg;
    uint16_t bg;
    uint16_t pixels[16][4];
} glyph_palette_t;

static const glyph_palette_t *palette_for(uint16_t fg, uint16_t bg) {
    static glyph_palette_t cache[8];
    static int count;
    static int next;
    glyph_palette_t *palette;
    int i, n;
    for(i = 0; i < count; ++i)
        if(cache[i].fg == fg && cache[i].bg == bg) return &cache[i];
    palette = &cache[count < 8 ? count++ : next++ % 8];
    palette->fg = fg;
    palette->bg = bg;
    for(n = 0; n < 16; ++n)
        for(i = 0; i < 4; ++i)
            palette->pixels[n][i] = (n & (8 >> i)) ? fg : bg;
    return palette;
}

/* Draws whole glyphs only; text never wraps into the next scanline. */
static void draw_string(int x, int y, uint16_t fg, uint16_t bg, const char *text) {
    const glyph_palette_t *palette;
    if(y < 0 || y + BFONT_HEIGHT > SCREEN_H || x < 0) return;
    if(!glyphs_ready) load_glyphs();
    palette = palette_for(fg, bg);
    for(; *text && x + BFONT_THIN_WIDTH <= SCREEN_W; ++text, x += BFONT_THIN_WIDTH) {
        unsigned char c = (unsigned char)*text == '\t' ? ' ' : (unsigned char)*text;
        const uint16_t *rows = glyph_rows[c];
        uint16_t *pixel = frame + y * SCREEN_W + x;
        int row;
        for(row = 0; row < BFONT_HEIGHT; ++row, pixel += SCREEN_W) {
            const uint16_t *a = palette->pixels[(rows[row] >> 8) & 15];
            const uint16_t *b = palette->pixels[(rows[row] >> 4) & 15];
            const uint16_t *d = palette->pixels[rows[row] & 15];
            pixel[0] = a[0]; pixel[1] = a[1]; pixel[2] = a[2]; pixel[3] = a[3];
            pixel[4] = b[0]; pixel[5] = b[1]; pixel[6] = b[2]; pixel[7] = b[3];
            pixel[8] = d[0]; pixel[9] = d[1]; pixel[10] = d[2]; pixel[11] = d[3];
        }
    }
}

static void draw_text(int x, int y, uint16_t color, const char *text) {
    draw_string(x, y, color, C_PAGE, text);
}

static void draw_image_clipped(const browser_image_t *image, int x, int y) {
    int iy;
    int first = x < 0 ? -x : 0;
    int last = x + image->width > SCREEN_W ? SCREEN_W - x : image->width;
    if(!image->pixels || image->loaded <= 0 || first >= last) return;
    for(iy = 0; iy < image->height; ++iy) {
        int sy = y + iy;
        if(sy < PAGE_TOP || sy >= SCREEN_H) continue;
        memcpy(frame + sy * SCREEN_W + x + first,
               image->pixels + iy * image->width + first,
               (size_t)(last - first) * sizeof(uint16_t));
    }
}

static void draw_cursor(int x, int y) {
    int i;
    for(i = -7; i <= 7; ++i) {
        int sx = x + i;
        int sy = y + i;
        if(sx >= 0 && sx < SCREEN_W && y >= 0 && y < SCREEN_H)
            frame[y * SCREEN_W + sx] = i & 1 ? C_WHITE : C_INK;
        if(x >= 0 && x < SCREEN_W && sy >= 0 && sy < SCREEN_H)
            frame[sy * SCREEN_W + x] = i & 1 ? C_WHITE : C_INK;
    }
}

#define ADDRESS_COLUMNS 33
#define ADDRESS_TEXT_X 154

static void draw_address(const browser_view_t *view) {
    const char *address = view->address;
    size_t len = strlen(address);
    size_t start = len > ADDRESS_COLUMNS ? len - ADDRESS_COLUMNS : 0;
    uint16_t field = view->editing ? C_WHITE : 0xce59;
    char visible[ADDRESS_COLUMNS + 1];

    fill_rect(122, 8, 440, 32, field);
    if(view->editing) {
        size_t caret = (size_t)view->address_caret;
        if(caret > len) caret = len;
        if(caret < start) start = caret;
        draw_string(130, 12, C_INK, field, ">");
        snprintf(visible, sizeof(visible), "%s", address + start);
        if(view->address_selected && visible[0]) {
            draw_string(ADDRESS_TEXT_X, 12, C_WHITE, C_BLUE, visible);
        } else {
            draw_string(ADDRESS_TEXT_X, 12, C_INK, field, visible);
            fill_rect(ADDRESS_TEXT_X + (int)(caret - start) * 12, 12, 2,
                      BFONT_HEIGHT, C_INK);
        }
    } else {
        snprintf(visible, sizeof(visible), "%s", address + start);
        draw_string(ADDRESS_TEXT_X, 12, C_INK, field, visible);
    }
}

static void draw_help(void) {
    static const char *const lines[] = {
        "KEYBOARD SHORTCUTS",
        "Tab, Shift+Tab    Next/previous link or field",
        "Enter             Open link, edit or use field",
        "Arrows, Home/End  Scroll a line, top/bottom",
        "Space, PgDn/PgUp  Scroll a screen down/up",
        "Backspace, Alt+<  Back",
        "Shift+Bksp, Alt+> Forward",
        "F6, Ctrl+L        Address bar: URL or search",
        "F5, Ctrl+R        Reload   Alt+Home: home page",
        "Ctrl+D, Ctrl+B    Bookmark page, bookmarks",
        "Esc               Clear focus or cancel a load",
        "Esc, Esc          Exit the browser",
        "Editing: arrows, Home/End, Del, Ctrl+A all,",
        "  Ctrl+W/Ctrl+Bksp word, Ctrl+U to start",
        "Press any key to close this help",
    };
    const int count = (int)(sizeof(lines) / sizeof(lines[0]));
    const int top = PAGE_TOP + 4;
    int i;

    fill_rect(14, top, SCREEN_W - 28, count * BFONT_HEIGHT + 16, C_BLUE);
    fill_rect(18, top + 4, SCREEN_W - 36, count * BFONT_HEIGHT + 8, C_WHITE);
    for(i = 0; i < count; ++i)
        draw_string(26, top + 8 + i * BFONT_HEIGHT, i == 0 ? C_HEADING : C_INK,
                    C_WHITE, lines[i]);
}

static void draw_toolbar(const browser_document_t *doc, const browser_view_t *view) {
    const char *status = view->status;
    char footer[96];

    fill_rect(0, 0, SCREEN_W, 70, C_TEAL);
    fill_rect(6, 8, 54, 32, view->can_go_back ? C_BLUE : C_MUTED);
    draw_string(9, 12, C_WHITE, view->can_go_back ? C_BLUE : C_MUTED, "BACK");
    fill_rect(64, 8, 52, 32, view->can_go_forward ? C_BLUE : C_MUTED);
    draw_string(72, 12, C_WHITE, view->can_go_forward ? C_BLUE : C_MUTED, "FWD");
    draw_address(view);
    fill_rect(570, 8, 56, 32, C_BLUE);
    draw_string(579, 12, C_WHITE, C_BLUE, "GO");

    if(status && (!strncmp(status, "Loading ", 8) ||
                  !strncmp(status, "Connecting ", 11) ||
                  !strncmp(status, "Canceling ", 10))) {
        snprintf(footer, sizeof(footer), "%.51s", status);
    } else {
        /* Leave room for a permanent, right-aligned help hint. */
        snprintf(footer, sizeof(footer), "%.42s",
                 status && status[0] ? status : doc->title);
        draw_string(542, 44, C_WHITE, C_TEAL, "F1 Help");
    }
    draw_string(14, 44, C_WHITE, C_TEAL, footer);
    fill_rect(0, 68, SCREEN_W, 2, C_BLUE);
}

static void draw_osk(const browser_view_t *view) {
    int row, column;

    fill_rect(0, OSK_TOP, SCREEN_W, SCREEN_H - OSK_TOP, C_TEAL);
    fill_rect(0, OSK_TOP, SCREEN_W, 2, C_BLUE);
    for(row = 0; row < OSK_ROWS; ++row) {
        for(column = 0; column < osk_columns(row); ++column) {
            osk_key_t key;
            int x, y, w, h;
            int length;
            uint16_t background = C_WHITE;
            uint16_t ink = C_INK;
            osk_key(row, column, view->osk_shift, &key);
            osk_key_rect(row, column, &x, &y, &w, &h);
            if(key.action == OSK_SHIFT && view->osk_shift) {
                background = C_BLUE;
                ink = C_WHITE;
            }
            if(row == view->osk_row && column == view->osk_column) {
                background = C_FOCUS;
                ink = C_INK;
            }
            fill_rect(x, y, w, h, background);
            length = (int)strlen(key.label);
            draw_string(x + (w - length * 12) / 2, y + (h - BFONT_HEIGHT) / 2, ink,
                        background, key.label);
        }
    }
}

static void draw_page(const browser_document_t *doc, const browser_view_t *view) {
    int i;
    int scroll_y = view->scroll_y;
    int focused_link = view->focused_link;

    for(i = 0; i < doc->item_count; ++i) {
        const document_item_t *item = &doc->items[i];
        int y = PAGE_TOP + item->y - scroll_y;
        uint16_t color = C_INK;
        if(y + item->height < PAGE_TOP || y >= SCREEN_H) continue;
        if(focused_link >= 0 && item->link_id == focused_link)
            fill_rect(item->x - 3, y - 1, item->width + 6, item->height, C_FOCUS);
        if(item->type == ITEM_TEXT || item->type == ITEM_NOTICE) {
            if(item->style == TEXT_LINK) color = C_BLUE;
            else if(item->style == TEXT_HEADING) color = C_HEADING;
            else if(item->style == TEXT_STRONG) color = C_STRONG;
            else if(item->style == TEXT_EMPHASIS) color = C_EMPHASIS;
            else if(item->style == TEXT_MUTED || item->type == ITEM_NOTICE) color = C_ERROR;
            else if(item->style == TEXT_CODE) color = C_MUTED;
            draw_text(item->x, y, color, item->text);
        } else if(item->type == ITEM_RULE) {
            fill_rect(item->x, y, item->width, 2, C_RULE);
        } else if(item->type == ITEM_IMAGE && item->image_id >= 0) {
            const browser_image_t *image = &doc->images[item->image_id];
            if(image->loaded > 0) draw_image_clipped(image, item->x, y);
            else {
                char placeholder[96];
                fill_rect(item->x, y, item->width, item->height, C_RULE);
                if(image->loaded < 0)
                    snprintf(placeholder, sizeof(placeholder),
                             "[image omitted: %.58s]", image->alt);
                else
                    snprintf(placeholder, sizeof(placeholder), "[loading image]");
                if(item->width >= 96 && item->height >= 24)
                    draw_text(item->x + 8, y + 22, C_MUTED, placeholder);
            }
        }
    }

    if(doc->height > SCREEN_H - PAGE_TOP) {
        int track = SCREEN_H - PAGE_TOP - 8;
        int thumb = track * (SCREEN_H - PAGE_TOP) / doc->height;
        int max_scroll = doc->height - (SCREEN_H - PAGE_TOP);
        int thumb_y;
        if(thumb < 18) thumb = 18;
        thumb_y = PAGE_TOP + 4 + (max_scroll ? scroll_y * (track - thumb) / max_scroll : 0);
        fill_rect(SCREEN_W - 7, PAGE_TOP + 4, 3, track, C_RULE);
        fill_rect(SCREEN_W - 8, thumb_y, 5, thumb, C_BLUE);
    }
}

/* Draws a frame into the video back buffer without presenting it. */
void render_draw(const browser_document_t *doc, const browser_view_t *view) {
    fill_rect(0, 0, SCREEN_W, SCREEN_H, C_PAGE);
    draw_page(doc, view);
    /* The toolbar goes on top so rows scrolled partly under it are hidden. */
    draw_toolbar(doc, view);
    if(view->osk_open) draw_osk(view);
    if(view->show_help) draw_help();
    draw_cursor(view->mouse_x, view->mouse_y);
    sq_cpy(vram_s, frame, sizeof(frame));
}

#ifdef BROWSER_FRAME_DUMP
#include <zlib/zlib.h>

/* Prints the last drawn frame over the serial console as base64 zlib data,
   so emulator runs can be inspected without screen capture. */
void render_dump_frame(const char *label) {
    static unsigned char packed[SCREEN_W * SCREEN_H * 2 + 1024];
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uLongf size = sizeof(packed);
    uLongf i;
    char line[81];
    int n = 0;

    if(compress2(packed, &size, (const Bytef *)frame, sizeof(frame), 9) != Z_OK) {
        printf("FRAME-ERROR %s\n", label);
        return;
    }
    printf("FRAME-BEGIN %s %d %d %lu\n", label, SCREEN_W, SCREEN_H, (unsigned long)size);
    for(i = 0; i < size; i += 3) {
        uint32_t v = (uint32_t)packed[i] << 16 |
                     (i + 1 < size ? (uint32_t)packed[i + 1] << 8 : 0) |
                     (i + 2 < size ? packed[i + 2] : 0);
        line[n++] = alphabet[(v >> 18) & 63];
        line[n++] = alphabet[(v >> 12) & 63];
        line[n++] = i + 1 < size ? alphabet[(v >> 6) & 63] : '=';
        line[n++] = i + 2 < size ? alphabet[v & 63] : '=';
        if(n == 76) {
            line[n] = 0;
            printf("F %s\n", line);
            n = 0;
        }
    }
    line[n] = 0;
    if(n) printf("F %s\n", line);
    printf("FRAME-END %s\n", label);
}
#endif

void render_browser(const browser_document_t *doc, const browser_view_t *view) {
    render_draw(doc, view);
    /* Swap only during vertical blank so scanout never sees a half-cleared UI. */
    vid_waitvbl();
    vid_flip(-1);
}
