#include "browser.h"

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/pvr.h>
#include <dc/sq.h>
#include <dc/video.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Screen regions, as row ranges [top, bottom). The toolbar covers the page
   rows scrolled underneath it, and the scrollbar owns the rightmost columns
   of the page region; page content never reaches them (PAGE_WIDTH). */
#define TOOLBAR_BOTTOM 70
#define ADDRESS_ROW_TOP 8
#define ADDRESS_ROW_BOTTOM 40
#define FOOTER_ROW_TOP 44
#define FOOTER_ROW_BOTTOM (FOOTER_ROW_TOP + BFONT_HEIGHT)
#define SCROLLBAR_X (SCREEN_W - 8)
#define CURSOR_REACH 7
#define STATUS_COPY 96

/* Frames are composed in cached main RAM, where the many small glyph writes
   are cheap. Only the rows that changed are redrawn, and only they are burst
   to video RAM through the store queues; a full 640x480 copy costs more than
   drawing most frames. */
static uint16_t frame[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));

/* Rows the current drawing pass may touch. Every primitive clips to it, so
   redrawing a band of the screen is the same as drawing the whole scene and
   keeping that band. */
static int clip_y0;
static int clip_y1 = SCREEN_H;

/* Row ranges, kept sorted-free and non-overlapping. */
#define MAX_SPANS 6
typedef struct {
    int y0;
    int y1;
} row_span_t;
typedef struct {
    row_span_t spans[MAX_SPANS];
    int count;
} span_list_t;

static void spans_clear(span_list_t *list) {
    list->count = 0;
}

static void spans_add(span_list_t *list, int y0, int y1) {
    int i;
    if(y0 < 0) y0 = 0;
    if(y1 > SCREEN_H) y1 = SCREEN_H;
    if(y1 <= y0) return;
    /* Absorb every span that overlaps or touches the new one. */
    for(i = 0; i < list->count;) {
        row_span_t *span = &list->spans[i];
        if(span->y1 >= y0 && span->y0 <= y1) {
            if(span->y0 < y0) y0 = span->y0;
            if(span->y1 > y1) y1 = span->y1;
            *span = list->spans[--list->count];
            i = 0; /* The merged span may now touch an earlier one. */
        } else {
            ++i;
        }
    }
    if(list->count == MAX_SPANS) {
        /* Merge into the nearest span; over-copying a gap beats dropping rows. */
        int best = 0;
        int best_gap = SCREEN_H;
        for(i = 0; i < MAX_SPANS; ++i) {
            row_span_t *span = &list->spans[i];
            int gap = span->y0 > y1 ? span->y0 - y1 : y0 - span->y1;
            if(gap < best_gap) {
                best_gap = gap;
                best = i;
            }
        }
        if(list->spans[best].y0 < y0) y0 = list->spans[best].y0;
        if(list->spans[best].y1 > y1) y1 = list->spans[best].y1;
        list->spans[best] = list->spans[--list->count];
        spans_add(list, y0, y1);
        return;
    }
    list->spans[list->count].y0 = y0;
    list->spans[list->count].y1 = y1;
    list->count++;
}

static void spans_add_list(span_list_t *list, const span_list_t *other) {
    int i;
    for(i = 0; i < other->count; ++i)
        spans_add(list, other->spans[i].y0, other->spans[i].y1);
}

/* Rows of the RAM frame that changed since the last present, and rows that
   each video RAM buffer still lacks. With two buffers, the back buffer is
   always two frames stale, so a change is owed to both until copied. */
static span_list_t frame_dirty;
static span_list_t vram_pending[2];
static int flip_vblank = -1;

/* What the last composed frame showed, to work out what must be redrawn. */
typedef struct {
    int valid;
    int scroll_y;
    int mouse_x;
    int mouse_y;
    int focused_link;
    int editing;
    int address_caret;
    int address_selected;
    int can_go_back;
    int can_go_forward;
    int toolbar;
    int show_help;
    int osk_open;
    int osk_row;
    int osk_column;
    int osk_shift;
    unsigned generation;
    int doc_height;
    char address[MAX_URL];
    char status[STATUS_COPY];
    char title[MAX_TITLE];
} frame_state_t;

static frame_state_t last_state;

typedef uint32_t __attribute__((may_alias)) pixel_pair_t;

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    pixel_pair_t pair = ((uint32_t)color << 16) | color;
    int row;
    if(x < 0) { w += x; x = 0; }
    if(y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if(x + w > SCREEN_W) w = SCREEN_W - x;
    if(y + h > clip_y1) h = clip_y1 - y;
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

/* Draws whole glyph columns only; rows are clipped, so text at the edge of
   the screen or of a redraw band is drawn exactly as far as it shows. */
static void draw_string(int x, int y, uint16_t fg, uint16_t bg, const char *text) {
    const glyph_palette_t *palette;
    int first_row = y < clip_y0 ? clip_y0 - y : 0;
    int last_row = y + BFONT_HEIGHT > clip_y1 ? clip_y1 - y : BFONT_HEIGHT;
    if(first_row >= last_row || x < 0) return;
    if(!glyphs_ready) load_glyphs();
    palette = palette_for(fg, bg);
    for(; *text && x + BFONT_THIN_WIDTH <= SCREEN_W; ++text, x += BFONT_THIN_WIDTH) {
        unsigned char c = (unsigned char)*text == '\t' ? ' ' : (unsigned char)*text;
        const uint16_t *rows = glyph_rows[c];
        uint16_t *pixel = frame + (y + first_row) * SCREEN_W + x;
        int row;
        for(row = first_row; row < last_row; ++row, pixel += SCREEN_W) {
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
        if(sy < clip_y0 || sy >= clip_y1) continue;
        memcpy(frame + sy * SCREEN_W + x + first,
               image->pixels + iy * image->width + first,
               (size_t)(last - first) * sizeof(uint16_t));
    }
}

static void draw_cursor(int x, int y) {
    int i;
    for(i = -CURSOR_REACH; i <= CURSOR_REACH; ++i) {
        int sx = x + i;
        int sy = y + i;
        if(sx >= 0 && sx < SCREEN_W && y >= clip_y0 && y < clip_y1)
            frame[y * SCREEN_W + sx] = i & 1 ? C_WHITE : C_INK;
        if(x >= 0 && x < SCREEN_W && sy >= clip_y0 && sy < clip_y1)
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

static const char *const help_lines[] = {
    "KEYBOARD SHORTCUTS",
    "Tab, Shift+Tab    Next/previous link or field",
    "Enter open/edit; Ctrl+Enter submits a form",
    "Arrows, Home/End  Scroll a line, top/bottom",
    "Space, PgDn/PgUp  Scroll a screen down/up",
    "Backspace, Alt+<  Back",
    "Shift+Bksp, Alt+> Forward",
    "F6, Ctrl+L        Address bar: URL or search",
    "F5/Ctrl+R reload; F4 images; F7 reader view",
    "Ctrl+B            Hide or show the address bar",
    "Ctrl+D, Ctrl+Sh+B Bookmark page, bookmarks",
    "Esc               Clear focus or cancel a load",
    "Esc, Esc          Exit the browser",
    "Editing: arrows, Home/End, Del, Ctrl+A all,",
    "  Ctrl+W/Ctrl+Bksp word, Ctrl+U to start",
    "Press any key to close this help",
};
#define HELP_LINES ((int)(sizeof(help_lines) / sizeof(help_lines[0])))

static void draw_help(void) {
    const int top = PAGE_TOP + 4;
    int i;

    fill_rect(14, top, SCREEN_W - 28, HELP_LINES * BFONT_HEIGHT + 16, C_BLUE);
    fill_rect(18, top + 4, SCREEN_W - 36, HELP_LINES * BFONT_HEIGHT + 8, C_WHITE);
    for(i = 0; i < HELP_LINES; ++i)
        draw_string(26, top + 8 + i * BFONT_HEIGHT, i == 0 ? C_HEADING : C_INK,
                    C_WHITE, help_lines[i]);
}

static int status_is_transfer(const char *status) {
    return status && (!strncmp(status, "Loading ", 8) ||
                      !strncmp(status, "Connecting ", 11) ||
                      !strncmp(status, "Canceling ", 10));
}

static void draw_toolbar(const browser_document_t *doc, const browser_view_t *view) {
    const char *status = view->status;
    char footer[96];

    fill_rect(0, 0, SCREEN_W, TOOLBAR_BOTTOM, C_TEAL);
    if(clip_y0 < ADDRESS_ROW_BOTTOM && clip_y1 > ADDRESS_ROW_TOP) {
        fill_rect(6, 8, 54, 32, view->can_go_back ? C_BLUE : C_MUTED);
        draw_string(9, 12, C_WHITE, view->can_go_back ? C_BLUE : C_MUTED, "BACK");
        fill_rect(64, 8, 52, 32, view->can_go_forward ? C_BLUE : C_MUTED);
        draw_string(72, 12, C_WHITE, view->can_go_forward ? C_BLUE : C_MUTED, "FWD");
        draw_address(view);
        fill_rect(570, 8, 56, 32, C_BLUE);
        draw_string(579, 12, C_WHITE, C_BLUE, "GO");
    }
    if(clip_y0 < FOOTER_ROW_BOTTOM && clip_y1 > FOOTER_ROW_TOP) {
        if(status_is_transfer(status)) {
            snprintf(footer, sizeof(footer), "%.51s", status);
        } else {
            /* Leave room for a permanent, right-aligned help hint. */
            snprintf(footer, sizeof(footer), "%.42s",
                     status && status[0] ? status : doc->title);
            draw_string(542, FOOTER_ROW_TOP, C_WHITE, C_TEAL, "F1 Help");
        }
        draw_string(14, FOOTER_ROW_TOP, C_WHITE, C_TEAL, footer);
    }
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
            osk_key_rect(row, column, &x, &y, &w, &h);
            if(y >= clip_y1 || y + h <= clip_y0) continue;
            osk_key(row, column, view->osk_shift, &key);
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

/* Scrollbar thumb rows for a document height and scroll position. */
static int scrollbar_thumb(int doc_height, int scroll_y, int origin, int *top,
                           int *height) {
    int track = SCREEN_H - origin - 8;
    int thumb;
    int max_scroll;
    if(doc_height <= SCREEN_H - origin) return 0;
    thumb = track * (SCREEN_H - origin) / doc_height;
    max_scroll = doc_height - (SCREEN_H - origin);
    if(thumb < 18) thumb = 18;
    *top = origin + 4 + (max_scroll ? scroll_y * (track - thumb) / max_scroll : 0);
    *height = thumb;
    return 1;
}

/* Where page coordinate 0 lands on screen, and the first row the page owns. */
static int page_origin(const browser_view_t *view) {
    return view->toolbar ? PAGE_TOP : 0;
}

static int page_region_top(const browser_view_t *view) {
    return view->toolbar ? TOOLBAR_BOTTOM : 0;
}

static void draw_page(const browser_document_t *doc, const browser_view_t *view) {
    int i;
    int scroll_y = view->scroll_y;
    int focused_link = view->focused_link;
    int origin = page_origin(view);
    int thumb_y, thumb_h;

    for(i = 0; i < doc->item_count; ++i) {
        const document_item_t *item = &doc->items[i];
        int y = origin + item->y - scroll_y;
        uint16_t color = C_INK;
        /* Rows an item can touch: its focus outline above, a rule's two
           rows, and placeholder text that may run past a short image box.
           Anything above the page region is hidden by the toolbar. */
        if(y - 1 >= clip_y1 || y + item->height + 2 * BFONT_HEIGHT <= clip_y0) continue;
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
                    snprintf(placeholder, sizeof(placeholder), "[F4 image] %.64s", image->alt);
                int columns = (item->width - 16) / BFONT_THIN_WIDTH;
                if(columns >= 0 && columns < (int)sizeof(placeholder)) placeholder[columns] = 0;
                if(item->width >= 96 && item->height >= 24)
                    draw_text(item->x + 8, y + (item->height < 68 ? (item->height - 24) / 2 : 22),
                              C_MUTED, placeholder);
            }
        }
    }

    if(scrollbar_thumb(doc->height, scroll_y, origin, &thumb_y, &thumb_h)) {
        fill_rect(SCREEN_W - 7, origin + 4, 3, SCREEN_H - origin - 8, C_RULE);
        fill_rect(SCREEN_W - 8, thumb_y, 5, thumb_h, C_BLUE);
    }
}

/* Draws every layer, bottom to top, clipped to the current row band. */
static void draw_scene(const browser_document_t *doc, const browser_view_t *view) {
    fill_rect(0, 0, SCREEN_W, SCREEN_H, C_PAGE);
    /* The toolbar and keyboard fully cover their rows. In particular,
       typing in the address bar should never walk the page's item list. */
    if(clip_y1 > page_region_top(view) &&
       (!view->osk_open || clip_y0 < OSK_TOP))
        draw_page(doc, view);
    /* The toolbar goes on top so rows scrolled partly under it are hidden. */
    if(view->toolbar && clip_y0 < TOOLBAR_BOTTOM) draw_toolbar(doc, view);
    if(view->osk_open && clip_y1 > OSK_TOP) draw_osk(view);
    if(view->show_help && clip_y1 > PAGE_TOP + 4 &&
       clip_y0 < PAGE_TOP + 4 + HELP_LINES * BFONT_HEIGHT + 16)
        draw_help();
    draw_cursor(view->mouse_x, view->mouse_y);
}

static void draw_band(const browser_document_t *doc, const browser_view_t *view,
                      int y0, int y1) {
    clip_y0 = y0;
    clip_y1 = y1;
    draw_scene(doc, view);
    clip_y0 = 0;
    clip_y1 = SCREEN_H;
}

/* Moves page rows by delta (positive scrolls the content up) without
   touching the scrollbar columns, so only the newly exposed band and the
   scrollbar thumb need drawing. */
static void shift_page_rows(int top, int bottom, int delta) {
    const size_t bytes = SCROLLBAR_X * sizeof(uint16_t);
    int y;
    if(delta > 0) {
        for(y = top; y < bottom - delta; ++y)
            memcpy(frame + y * SCREEN_W, frame + (y + delta) * SCREEN_W, bytes);
    } else {
        for(y = bottom - 1; y >= top - delta; --y)
            memcpy(frame + y * SCREEN_W, frame + (y + delta) * SCREEN_W, bytes);
    }
}

static void capture_state(const browser_document_t *doc, const browser_view_t *view,
                          frame_state_t *out) {
    memset(out, 0, sizeof(*out));
    out->valid = 1;
    out->scroll_y = view->scroll_y;
    out->mouse_x = view->mouse_x;
    out->mouse_y = view->mouse_y;
    out->focused_link = view->focused_link;
    out->editing = view->editing;
    out->address_caret = view->address_caret;
    out->address_selected = view->address_selected;
    out->can_go_back = view->can_go_back;
    out->can_go_forward = view->can_go_forward;
    out->toolbar = view->toolbar;
    out->show_help = view->show_help;
    out->osk_open = view->osk_open;
    out->osk_row = view->osk_row;
    out->osk_column = view->osk_column;
    out->osk_shift = view->osk_shift;
    out->generation = doc->generation;
    out->doc_height = doc->height;
    snprintf(out->address, sizeof(out->address), "%s", view->address ? view->address : "");
    snprintf(out->status, sizeof(out->status), "%s", view->status ? view->status : "");
    snprintf(out->title, sizeof(out->title), "%s", doc->title);
}

static void mark_cursor(span_list_t *dirty, int y, int reach) {
    spans_add(dirty, y - reach, y + reach + 1);
}

/* Works out which rows differ from the last frame and redraws just those. */
void render_frame(const browser_document_t *doc, const browser_view_t *view) {
    span_list_t dirty = { .count = 0 };
    span_list_t shifted = { .count = 0 };
    frame_state_t now;
    const frame_state_t *last = &last_state;
    int i;

    if(!glyphs_ready) load_glyphs();
    capture_state(doc, view, &now);

    if(!last->valid || now.toolbar != last->toolbar) {
        spans_add(&dirty, 0, SCREEN_H);
    } else {
        int page_top = page_region_top(view);
        int page_bottom = now.osk_open ? OSK_TOP : SCREEN_H;
        int page_changed = now.generation != last->generation ||
                           now.doc_height != last->doc_height ||
                           now.focused_link != last->focused_link ||
                           now.osk_open != last->osk_open ||
                           now.show_help != last->show_help;
        int delta = now.scroll_y - last->scroll_y;

        if(now.toolbar && (now.editing != last->editing ||
                           now.address_caret != last->address_caret ||
                           now.address_selected != last->address_selected ||
                           now.can_go_back != last->can_go_back ||
                           now.can_go_forward != last->can_go_forward ||
                           strcmp(now.address, last->address)))
            spans_add(&dirty, ADDRESS_ROW_TOP, ADDRESS_ROW_BOTTOM);
        if(now.toolbar && (strcmp(now.status, last->status) ||
                           strcmp(now.title, last->title)))
            spans_add(&dirty, FOOTER_ROW_TOP, FOOTER_ROW_BOTTOM);

        if(page_changed) {
            spans_add(&dirty, page_top, SCREEN_H);
        } else if(delta && !now.show_help && abs(delta) < page_bottom - page_top) {
            int origin = page_origin(view);
            int thumb_y, thumb_h;
            int reach = CURSOR_REACH + abs(delta);
            shift_page_rows(page_top, page_bottom, delta);
            if(delta > 0) spans_add(&dirty, page_bottom - delta, page_bottom);
            else spans_add(&dirty, page_top, page_top - delta);
            if(scrollbar_thumb(last->doc_height, last->scroll_y, origin, &thumb_y, &thumb_h))
                spans_add(&dirty, thumb_y, thumb_y + thumb_h);
            if(scrollbar_thumb(now.doc_height, now.scroll_y, origin, &thumb_y, &thumb_h))
                spans_add(&dirty, thumb_y, thumb_y + thumb_h);
            /* The cursor moved with the rows; redraw where it was and where
               its copy landed. A real move marks the new spot below. */
            mark_cursor(&dirty, last->mouse_y, reach);
            spans_add(&shifted, page_top, page_bottom);
        } else if(delta) {
            spans_add(&dirty, page_top, SCREEN_H);
        }

        if(now.osk_open && (now.osk_row != last->osk_row ||
                            now.osk_column != last->osk_column ||
                            now.osk_shift != last->osk_shift))
            spans_add(&dirty, OSK_TOP, SCREEN_H);
        if(now.mouse_x != last->mouse_x || now.mouse_y != last->mouse_y) {
            mark_cursor(&dirty, last->mouse_y, CURSOR_REACH);
            mark_cursor(&dirty, now.mouse_y, CURSOR_REACH);
        }
    }

    for(i = 0; i < dirty.count; ++i)
        draw_band(doc, view, dirty.spans[i].y0, dirty.spans[i].y1);
    spans_add_list(&frame_dirty, &dirty);
    spans_add_list(&frame_dirty, &shifted);
    last_state = now;
}

/* The framebuffer address takes effect at the next vertical blank, so the
   buffer just flipped away from is still being scanned out until then. */
int render_present_ready(void) {
    return maple_state.vbl_cntr != flip_vblank;
}

static int back_buffer_index(void) {
    uint32_t offset = (uint32_t)(uintptr_t)vram_s & (PVR_RAM_SIZE - 1);
    int index = vid_mode->fb_size ? (int)(offset / vid_mode->fb_size) : 0;
    return index >= 0 && index < 2 ? index : 0;
}

/* Copies the changed rows to the back buffer and shows it. */
void render_present(void) {
    int back = back_buffer_index();
    span_list_t *pending = &vram_pending[back];
    int i;

    spans_add_list(&vram_pending[0], &frame_dirty);
    spans_add_list(&vram_pending[1], &frame_dirty);
    spans_clear(&frame_dirty);
    if(!render_present_ready()) vid_waitvbl();
    for(i = 0; i < pending->count; ++i) {
        int y0 = pending->spans[i].y0;
        int rows = pending->spans[i].y1 - y0;
        sq_cpy(vram_s + y0 * SCREEN_W, frame + y0 * SCREEN_W,
               (size_t)rows * SCREEN_W * sizeof(uint16_t));
    }
    spans_clear(pending);
    vid_flip(-1);
    flip_vblank = maple_state.vbl_cntr;
}

void render_invalidate(void) {
    last_state.valid = 0;
}

const uint16_t *render_frame_pixels(void) {
    return frame;
}

void render_browser(const browser_document_t *doc, const browser_view_t *view) {
    render_frame(doc, view);
    render_present();
}

/* Draws every row into the video back buffer without presenting it. */
void render_draw(const browser_document_t *doc, const browser_view_t *view) {
    render_invalidate();
    render_frame(doc, view);
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
