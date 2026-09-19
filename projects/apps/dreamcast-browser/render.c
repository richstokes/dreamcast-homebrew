#include "browser.h"

#include <dc/biosfont.h>
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

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    int row;
    int col;
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }
    if(x + w > SCREEN_W) w = SCREEN_W - x;
    if(y + h > SCREEN_H) h = SCREEN_H - y;
    if(w <= 0 || h <= 0) return;
    for(row = 0; row < h; ++row)
        for(col = 0; col < w; ++col)
            vram_s[(y + row) * SCREEN_W + x + col] = color;
}

static void draw_text(int x, int y, uint16_t color, const char *text) {
    if(y < 0 || y + BFONT_HEIGHT > SCREEN_H || x < 0 || x >= SCREEN_W) return;
    bfont_draw_str_ex(vram_s + y * SCREEN_W + x, SCREEN_W,
                      color, C_PAGE, 16, false, text);
}

static void draw_image_clipped(const browser_image_t *image, int x, int y) {
    int iy, ix;
    if(!image->pixels || image->loaded <= 0) return;
    for(iy = 0; iy < image->height; ++iy) {
        int sy = y + iy;
        if(sy < PAGE_TOP || sy >= SCREEN_H) continue;
        for(ix = 0; ix < image->width; ++ix) {
            int sx = x + ix;
            if(sx >= 0 && sx < SCREEN_W)
                vram_s[sy * SCREEN_W + sx] = image->pixels[iy * image->width + ix];
        }
    }
}

static void draw_cursor(int x, int y) {
    int i;
    for(i = -7; i <= 7; ++i) {
        int sx = x + i;
        int sy = y + i;
        if(sx >= 0 && sx < SCREEN_W && y >= 0 && y < SCREEN_H)
            vram_s[y * SCREEN_W + sx] = i & 1 ? C_WHITE : C_INK;
        if(x >= 0 && x < SCREEN_W && sy >= 0 && sy < SCREEN_H)
            vram_s[sy * SCREEN_W + x] = i & 1 ? C_WHITE : C_INK;
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
        bfont_draw_str_ex(vram_s + 12 * SCREEN_W + 130, SCREEN_W,
                          C_INK, field, 16, false, ">");
        snprintf(visible, sizeof(visible), "%s", address + start);
        if(view->address_selected && visible[0]) {
            bfont_draw_str_ex(vram_s + 12 * SCREEN_W + ADDRESS_TEXT_X, SCREEN_W,
                              C_WHITE, C_BLUE, 16, false, visible);
        } else {
            bfont_draw_str_ex(vram_s + 12 * SCREEN_W + ADDRESS_TEXT_X, SCREEN_W,
                              C_INK, field, 16, false, visible);
            fill_rect(ADDRESS_TEXT_X + (int)(caret - start) * 12, 12, 2,
                      BFONT_HEIGHT, C_INK);
        }
    } else {
        snprintf(visible, sizeof(visible), "%s", address + start);
        bfont_draw_str_ex(vram_s + 12 * SCREEN_W + ADDRESS_TEXT_X, SCREEN_W,
                          C_INK, field, 16, false, visible);
    }
}

static void draw_help(void) {
    static const char *const lines[] = {
        "KEYBOARD SHORTCUTS",
        "Tab, Shift+Tab    Next/previous link or field",
        "Enter             Open link, edit or use field",
        "Up/Down arrows    Scroll a line",
        "Space, Shift+Spc  Scroll a screen down/up",
        "PgDn/PgUp         Scroll a screen down/up",
        "Home/End          Top/bottom of page",
        "Backspace, Alt+<  Back",
        "Shift+Bksp, Alt+> Forward",
        "F6, Ctrl+L        Edit address (Enter opens)",
        "F5, Ctrl+R        Reload   Alt+Home: home page",
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
        bfont_draw_str_ex(vram_s + (top + 8 + i * BFONT_HEIGHT) * SCREEN_W + 26,
                          SCREEN_W, i == 0 ? C_HEADING : C_INK, C_WHITE, 16,
                          false, lines[i]);
}

void render_browser(const browser_document_t *doc, const browser_view_t *view) {
    int i;
    int scroll_y = view->scroll_y;
    int focused_link = view->focused_link;
    int can_go_back = view->can_go_back;
    int can_go_forward = view->can_go_forward;
    const char *status = view->status;
    char footer[96];

    fill_rect(0, 0, SCREEN_W, SCREEN_H, C_PAGE);
    fill_rect(0, 0, SCREEN_W, 70, C_TEAL);
    fill_rect(6, 8, 54, 32, can_go_back ? C_BLUE : C_MUTED);
    bfont_draw_str_ex(vram_s + 12 * SCREEN_W + 9, SCREEN_W,
                      C_WHITE, can_go_back ? C_BLUE : C_MUTED, 16, false, "BACK");
    fill_rect(64, 8, 52, 32, can_go_forward ? C_BLUE : C_MUTED);
    bfont_draw_str_ex(vram_s + 12 * SCREEN_W + 72, SCREEN_W,
                      C_WHITE, can_go_forward ? C_BLUE : C_MUTED, 16, false, "FWD");
    draw_address(view);
    fill_rect(570, 8, 56, 32, C_BLUE);
    bfont_draw_str_ex(vram_s + 12 * SCREEN_W + 579, SCREEN_W,
                      C_WHITE, C_BLUE, 16, false, "GO");

    if(status && (!strncmp(status, "Loading ", 8) ||
                  !strncmp(status, "Connecting ", 11) ||
                  !strncmp(status, "Canceling ", 10))) {
        snprintf(footer, sizeof(footer), "%.51s", status);
    } else {
        /* Leave room for a permanent, right-aligned help hint. */
        snprintf(footer, sizeof(footer), "%.42s",
                 status && status[0] ? status : doc->title);
        bfont_draw_str_ex(vram_s + 44 * SCREEN_W + 542, SCREEN_W,
                          C_WHITE, C_TEAL, 16, false, "F1 Help");
    }
    bfont_draw_str_ex(vram_s + 44 * SCREEN_W + 14, SCREEN_W,
                      C_WHITE, C_TEAL, 16, false, footer);
    fill_rect(0, 68, SCREEN_W, 2, C_BLUE);

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
    if(view->show_help) draw_help();
    draw_cursor(view->mouse_x, view->mouse_y);
    /* Swap only during vertical blank so scanout never sees a half-cleared UI. */
    vid_waitvbl();
    vid_flip(-1);
}
