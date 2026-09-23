/*
 * VMU screen for Ping Cube: the ping target, the latest latency in
 * double-size digits and a 48-sample latency graph, drawn on every attached
 * VMU's 48x32 LCD.
 *
 * vmu_draw_lcd() only queues a Maple frame and returns MAPLE_EAGAIN while
 * the previous one is in flight, so a screen that could not be sent is simply
 * sent again on a later frame.
 */

#include "ping-cube.h"

#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_fb.h>

#include <string.h>

#define GRAPH_TOP 19
#define GRAPH_HEIGHT (VMU_SCREEN_HEIGHT - GRAPH_TOP)
#define REDRAW_INTERVAL_MS 200

static uint8_t pixels[VMU_SCREEN_HEIGHT][VMU_SCREEN_WIDTH];
static int32_t history[VMU_SCREEN_WIDTH];
static int history_count;
static vmufb_t framebuffer;
static vmufb_t presented;
static int pending_present;
static uint64_t last_present_ms;

void vmu_graph_reset(void) {
    history_count = 0;
}

void vmu_graph_push(int32_t latency_ms) {
    if(history_count == VMU_SCREEN_WIDTH) {
        memmove(history, history + 1, sizeof(history) - sizeof(history[0]));
        history_count--;
    }

    history[history_count++] = latency_ms;
}

static void set_pixel(int x, int y) {
    if(x >= 0 && x < VMU_SCREEN_WIDTH && y >= 0 && y < VMU_SCREEN_HEIGHT)
        pixels[y][x] = 1;
}

/* Draw with the VMU's built-in 4x6 font, optionally scaled up. Glyph bits
   are packed MSB first, one font->w wide row after another. */
static int draw_text(int x, int y, int scale, const char *text) {
    const vmufb_font_t *font = vmu_get_font();
    unsigned row;
    unsigned column;
    int dx;
    int dy;

    for(; *text; ++text) {
        const uint8_t *glyph = &font->fontdata[(uint8_t)*text * font->stride];

        for(row = 0; row < font->h; ++row) {
            for(column = 0; column < font->w; ++column) {
                unsigned bit = row * font->w + column;

                if(!(glyph[bit / 8] & (0x80 >> (bit % 8))))
                    continue;

                for(dy = 0; dy < scale; ++dy)
                    for(dx = 0; dx < scale; ++dx)
                        set_pixel(x + (int)column * scale + dx,
                                  y + (int)row * scale + dy);
            }
        }

        x += (int)font->w * scale;
    }

    return x;
}

static int text_width(const char *text, int scale) {
    return (int)strlen(text) * (int)vmu_get_font()->w * scale;
}

/* Bars scale to the slowest recent reply, but never below floor_ms, so a
   steady link shows short bars instead of noise stretched to full height.
   Timeouts are drawn as dotted full-height columns. */
static void draw_graph(int floor_ms) {
    int32_t scale_ms = floor_ms;
    int i;
    int y;

    for(i = 0; i < history_count; ++i) {
        if(history[i] > scale_ms)
            scale_ms = history[i];
    }

    for(i = 0; i < history_count; ++i) {
        const int x = VMU_SCREEN_WIDTH - history_count + i;

        if(history[i] < 0) {
            for(y = GRAPH_TOP; y < VMU_SCREEN_HEIGHT; y += 2)
                set_pixel(x, y);
        }
        else {
            int height = (int)((history[i] * GRAPH_HEIGHT + scale_ms - 1) /
                               scale_ms);

            if(height < 1)
                height = 1;
            for(y = VMU_SCREEN_HEIGHT - height; y < VMU_SCREEN_HEIGHT; ++y)
                set_pixel(x, y);
        }
    }
}

static void pack_framebuffer(void) {
    uint8_t row_bits[VMU_SCREEN_WIDTH / 8];
    int x;
    int y;

    for(y = 0; y < VMU_SCREEN_HEIGHT; ++y) {
        memset(row_bits, 0, sizeof(row_bits));
        for(x = 0; x < VMU_SCREEN_WIDTH; ++x) {
            if(pixels[y][x])
                row_bits[x / 8] |= 0x80 >> (x % 8);
        }
        vmufb_paint_area(&framebuffer, 0, y, VMU_SCREEN_WIDTH, 1, row_bits);
    }
}

/* vmufb_present() without discarding the result: a VMU in a controller
   faces the other way from one in a light gun, so its image is rotated. */
static int present_to(maple_device_t *device, const vmufb_t *fb) {
    maple_device_t *controller = maple_enum_dev(device->port, 0);

    if(controller && (controller->info.functions & MAPLE_FUNC_CONTROLLER) &&
       controller->info.connector_direction != device->info.connector_direction)
        return vmu_draw_lcd(device, fb->data);

    return vmu_draw_lcd_rotated(device, fb->data);
}

static void present_all(const vmufb_t *fb) {
    maple_device_t *device;
    int busy = 0;
    int i;

    for(i = 0; (device = maple_enum_type(i, MAPLE_FUNC_LCD)); ++i) {
        if(present_to(device, fb) == MAPLE_EAGAIN)
            busy = 1;
    }

    pending_present = busy;
}

void vmu_display_draw(const char *title, const char *headline,
                      const char *unit, int floor_ms, uint64_t now_ms) {
    int x;

    memset(pixels, 0, sizeof(pixels));
    draw_text((VMU_SCREEN_WIDTH - text_width(title, 1)) / 2, 0, 1, title);

    /* Double-size headline with a small unit beside it when both fit. */
    if(text_width(headline, 2) + text_width(unit, 1) <= VMU_SCREEN_WIDTH) {
        x = (VMU_SCREEN_WIDTH - text_width(headline, 2) -
             text_width(unit, 1)) / 2;
        x = draw_text(x, 7, 2, headline);
        draw_text(x, 13, 1, unit);
    }
    else {
        draw_text((VMU_SCREEN_WIDTH - text_width(headline, 1)) / 2, 10, 1,
                  headline);
    }

    draw_graph(floor_ms);
    pack_framebuffer();

    if(memcmp(&framebuffer, &presented, sizeof(framebuffer)) != 0) {
        presented = framebuffer;
        pending_present = 1;
    }

    /* Maple is shared with controller polling; keep LCD traffic modest. */
    if(pending_present && now_ms - last_present_ms >= REDRAW_INTERVAL_MS) {
        last_present_ms = now_ms;
        present_all(&presented);
    }
}

void vmu_display_clear(void) {
    int attempt;

    vmufb_clear(&presented);
    for(attempt = 0; attempt < 10; ++attempt) {
        present_all(&presented);
        if(!pending_present)
            break;
        thd_sleep(20);
    }
}
