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

static const char *visible_address(const char *address) {
    size_t len = strlen(address);
    if(len <= 34) return address;
    return address + len - 34;
}

void render_browser(const browser_document_t *doc, int scroll_y, int mouse_x,
                    int mouse_y, int focused_link, const char *address,
                    int editing, int can_go_back, int can_go_forward,
                    const char *status) {
    int i;
    char bar[64];
    char footer[96];

    fill_rect(0, 0, SCREEN_W, SCREEN_H, C_PAGE);
    fill_rect(0, 0, SCREEN_W, 70, C_TEAL);
    fill_rect(6, 8, 54, 32, can_go_back ? C_BLUE : C_MUTED);
    bfont_draw_str_ex(vram_s + 12 * SCREEN_W + 9, SCREEN_W,
                      C_WHITE, can_go_back ? C_BLUE : C_MUTED, 16, false, "BACK");
    fill_rect(64, 8, 52, 32, can_go_forward ? C_BLUE : C_MUTED);
    bfont_draw_str_ex(vram_s + 12 * SCREEN_W + 72, SCREEN_W,
                      C_WHITE, can_go_forward ? C_BLUE : C_MUTED, 16, false, "FWD");
    fill_rect(122, 8, 440, 32, editing ? C_WHITE : 0xce59);
    snprintf(bar, sizeof(bar), "%s%s", editing ? "> " : "  ", visible_address(address));
    bfont_draw_str_ex(vram_s + 12 * SCREEN_W + 130, SCREEN_W,
                      C_INK, editing ? C_WHITE : 0xce59, 16, false, bar);
    fill_rect(570, 8, 56, 32, C_BLUE);
    bfont_draw_str_ex(vram_s + 12 * SCREEN_W + 579, SCREEN_W,
                      C_WHITE, C_BLUE, 16, false, "GO");

    if(status && (!strncmp(status, "Loading ", 8) ||
                  !strncmp(status, "Connecting ", 11)))
        snprintf(footer, sizeof(footer), "%.52s", status);
    else
        snprintf(footer, sizeof(footer), "Alt+arrows history | F6 address | %.14s",
                 status && status[0] ? status : doc->title);
    bfont_draw_str_ex(vram_s + 44 * SCREEN_W + 14, SCREEN_W,
                      C_WHITE, C_TEAL, 16, false, footer);
    fill_rect(0, 68, SCREEN_W, 2, C_BLUE);

    for(i = 0; i < doc->item_count; ++i) {
        const document_item_t *item = &doc->items[i];
        int y = PAGE_TOP + item->y - scroll_y;
        uint16_t color = C_INK;
        if(y + item->height < PAGE_TOP || y >= SCREEN_H) continue;
        if(item->link_id == focused_link)
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
    draw_cursor(mouse_x, mouse_y);
    /* Swap only during vertical blank so scanout never sees a half-cleared UI. */
    vid_waitvbl();
    vid_flip(-1);
}
