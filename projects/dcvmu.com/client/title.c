/* A small, asset-free Dreamcast title: opaque PVR shapes and a pixel LCD.
   All title resources are released before the framebuffer-based app starts. */
#include "client.h"
#include <kos.h>
#include <dc/biosfont.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CREAM   0xfffaf5e9
#define NAVY    0xff24445f
#define BLUE    0xff438ba9
#define PALE    0xffddebea
#define ORANGE  0xffed7945
#define WHITE   0xfffffdf4
#define LCD     0xffc3d4a5
#define PIXEL   0xff3a5950
#define PI     3.14159265f
#define CIRCLE_STEPS 32

typedef struct { float x, y; } title_point_t;
static title_point_t circle[CIRCLE_STEPS + 1];
static float origin_x, origin_y, rotate_c = 1.0f, rotate_s, depth;
static pvr_poly_hdr_t shapes;

/* Original 5x7 lettering. Joining runs keeps the logo crisp and the polygon
   count small; the title needs neither a texture allocation nor disc assets. */
static const uint8_t alphabet[][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {31,4,4,4,4,4,31},     {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},   {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},     {31,1,2,4,8,16,31}
};
static const uint8_t lowercase[][7] = {
    {0,0,14,1,15,17,15},    {16,16,30,17,17,17,30},
    {0,0,14,16,16,17,14},   {1,1,15,17,17,17,15},
    {0,0,14,17,31,16,14},   {6,9,8,28,8,8,8},
    {0,14,17,17,15,1,14},   {16,16,30,17,17,17,17},
    {4,0,12,4,4,4,14},     {2,0,6,2,2,18,12},
    {16,16,18,20,24,20,18}, {12,4,4,4,4,4,14},
    {0,0,26,21,21,17,17},  {0,0,30,17,17,17,17},
    {0,0,14,17,17,17,14},  {0,0,30,17,30,16,16},
    {0,0,15,17,15,1,1},    {0,0,22,25,16,16,16},
    {0,0,15,16,14,1,30},   {8,8,28,8,8,9,6},
    {0,0,17,17,17,19,13},  {0,0,17,17,17,10,4},
    {0,0,17,17,21,21,10},  {0,0,17,10,4,10,17},
    {0,0,17,17,15,1,14},   {0,0,31,2,4,8,31}
};
static const uint8_t ampersand[7] = {12,18,20,8,21,18,13};

static void transform(float x, float y, float angle) {
    origin_x = x; origin_y = y;
    rotate_c = cosf(angle); rotate_s = sinf(angle);
}

static void vertex(float x, float y, uint32_t color, int last) {
    pvr_vertex_t v = {
        .flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
        .x = origin_x + x * rotate_c - y * rotate_s,
        .y = origin_y + x * rotate_s + y * rotate_c,
        .z = depth, .argb = color
    };
    pvr_prim(&v, sizeof(v));
}

static void rect(float x, float y, float w, float h, uint32_t color) {
    depth += 0.001f;
    vertex(x, y, color, 0); vertex(x + w, y, color, 0);
    vertex(x, y + h, color, 0); vertex(x + w, y + h, color, 1);
}

static void triangle(title_point_t a, title_point_t b, title_point_t c, uint32_t color) {
    depth += 0.001f;
    vertex(a.x, a.y, color, 0); vertex(b.x, b.y, color, 0);
    vertex(c.x, c.y, color, 1);
}

static void ellipse(float x, float y, float rx, float ry, uint32_t color) {
    depth += 0.001f;
    for(int i = 0; i < CIRCLE_STEPS; ++i) {
        vertex(x, y, color, 0);
        vertex(x + circle[i].x * rx, y + circle[i].y * ry, color, 0);
        vertex(x + circle[i + 1].x * rx, y + circle[i + 1].y * ry, color, 1);
    }
}

static void rounded(float x, float y, float w, float h, float r, uint32_t color) {
    depth += 0.001f;
    for(int i = 0; i < CIRCLE_STEPS; ++i) {
        vertex(x + w * 0.5f, y + h * 0.5f, color, 0);
        for(int j = i; j <= i + 1; ++j) {
            float px = x + (circle[j].x >= 0 ? w - r : r) + circle[j].x * r;
            float py = y + (circle[j].y >= 0 ? h - r : r) + circle[j].y * r;
            vertex(px, py, color, j == i + 1);
        }
    }
}

static void ring(float x, float y, float rx, float ry, float width, uint32_t color) {
    depth += 0.001f;
    for(int i = 0; i <= CIRCLE_STEPS; ++i) {
        vertex(x + circle[i].x * rx, y + circle[i].y * ry, color, 0);
        vertex(x + circle[i].x * (rx - width), y + circle[i].y * (ry - width),
               color, i == CIRCLE_STEPS);
    }
}

static void lettering(float x, float y, float scale, uint32_t color, const char *s) {
    for(; *s; ++s, x += 6 * scale) {
        if(*s == '.') { rect(x + scale, y + 6 * scale, scale, scale, color); continue; }
        if(*s == '!') {
            rect(x + 2 * scale, y, scale, 5 * scale, color);
            rect(x + 2 * scale, y + 6 * scale, scale, scale, color);
        }
        const uint8_t *glyph;
        if(*s >= 'A' && *s <= 'Z') glyph = alphabet[*s - 'A'];
        else if(*s >= 'a' && *s <= 'z') glyph = lowercase[*s - 'a'];
        else if(*s == '&') glyph = ampersand;
        else continue;
        for(int row = 0; row < 7; ++row) {
            unsigned bits = glyph[row];
            for(int col = 0; col < 5;) {
                if(!(bits & (16 >> col))) { ++col; continue; }
                int start = col++;
                while(col < 5 && (bits & (16 >> col))) ++col;
                rect(x + start * scale, y + row * scale, (col - start) * scale, scale, color);
            }
        }
    }
}

static void centered(float y, float scale, uint32_t color, const char *s) {
    lettering((640 - ((int)strlen(s) * 6 - 1) * scale) * 0.5f, y, scale, color, s);
}

static void sparkle(float x, float y, float size, uint32_t color) {
    title_point_t points[] = {{x,y-size}, {x+size*.25f,y-size*.25f}, {x+size,y},
                        {x+size*.25f,y+size*.25f}, {x,y+size},
                        {x-size*.25f,y+size*.25f}, {x-size,y}, {x-size*.25f,y-size*.25f}};
    for(int i = 0; i < 8; ++i) triangle((title_point_t){x,y}, points[i], points[(i+1)%8], color);
}

/* Face coordinates are actual 48x32 LCD pixels, magnified 2x on the shell. */
static void lcd_rect(int x, int y, int w, int h, uint32_t color) {
    rect(-48 + x * 2, -65 + y * 2, w * 2, h * 2, color);
}

static void eye(int x, int closed) {
    if(closed) {
        lcd_rect(x, 11, 2, 2, PIXEL); lcd_rect(x+2, 9, 4, 2, PIXEL);
        lcd_rect(x+6, 11, 2, 2, PIXEL);
    } else {
        lcd_rect(x+1, 7, 6, 11, PIXEL); lcd_rect(x, 9, 8, 6, PIXEL);
        lcd_rect(x+1, 7, 3, 4, WHITE); lcd_rect(x+5, 14, 2, 2, LCD);
    }
}

static void mascot(float seconds, int greeting) {
    float bob = sinf(seconds * 2.5f);
    uint32_t blink = (uint32_t)(seconds * 1000) % 4800;
    int closed = greeting || (blink > 3650 && blink < 3810) || (blink > 3950 && blink < 4080);

    ellipse(320, 353, 65 + bob * 5, 9, 0xffd8e2db);
    transform(320, 258 + bob * 6, sinf(seconds * 1.65f) * 0.055f);

    /* The clipped cap, portrait shell and blue controls give it the silhouette
       of an original white VMU. A tiny side bevel adds a toy-like thickness. */
    rounded(-43, -105, 86, 24, 7, NAVY);
    rounded(-38, -101, 76, 16, 4, 0xffc3d4d8);
    rect(-28, -101, 56, 4, WHITE);
    rounded(-69, -90, 145, 185, 24, NAVY);
    rounded(-68, -92, 137, 181, 22, 0xffa7c5cf);
    rounded(-65, -92, 129, 176, 21, WHITE);
    rounded(-60, -87, 119, 162, 17, 0xffeef0e5);
    rounded(-56, -78, 112, 86, 12, NAVY);
    rounded(-52, -74, 104, 78, 8, 0xff7d9b91);
    rect(-48, -65, 96, 64, LCD);
    /* Subtle rows evoke the original reflective monochrome LCD. */
    for(int y = 1; y < 32; y += 2) lcd_rect(0, y, 48, 1, 0xffbdcfa0);
    eye(10, closed); eye(30, greeting || (blink > 3950 && blink < 4080) ? 0 : closed);
    lcd_rect(5, 19, 2, 3, 0xff819c79); lcd_rect(8, 20, 2, 3, 0xff819c79);
    lcd_rect(39, 20, 2, 3, 0xff819c79); lcd_rect(42, 19, 2, 3, 0xff819c79);
    /* A little cat mouth, or an open smile when Start is pressed. */
    if(greeting) {
        lcd_rect(20, 22, 9, 2, PIXEL); lcd_rect(21, 24, 7, 3, PIXEL);
        lcd_rect(23, 27, 3, 1, PIXEL);
    } else {
        lcd_rect(19, 22, 2, 2, PIXEL); lcd_rect(23, 22, 2, 2, PIXEL);
        lcd_rect(27, 22, 2, 2, PIXEL); lcd_rect(21, 24, 2, 1, PIXEL);
        lcd_rect(25, 24, 2, 1, PIXEL);
    }
    lettering(-17, 13, 1, BLUE, "VMU");
    /* Raised D-pad and two round action buttons. */
    rounded(-49, 42, 43, 17, 4, NAVY); rounded(-36, 29, 17, 43, 4, NAVY);
    rect(-46, 46, 37, 8, BLUE); rect(-32, 33, 8, 34, BLUE);
    rect(-32, 33, 8, 3, 0xff91cbd5);
    ellipse(22, 57, 12, 12, NAVY); ellipse(22, 55, 9, 9, BLUE);
    ellipse(45, 37, 12, 12, NAVY); ellipse(45, 35, 9, 9, BLUE);
    ellipse(19, 52, 3, 2, 0xff91cbd5); ellipse(42, 32, 3, 2, 0xff91cbd5);
    transform(0, 0, 0);
}

static void frame(uint64_t elapsed, int greeting) {
    float seconds = (float)(elapsed % 120000) / 1000.0f;
    depth = 1.0f;
    transform(0, 0, 0);
    pvr_wait_ready();
    pvr_scene_begin(); pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&shapes, sizeof(shapes));
    rect(0, 0, 640, 480, CREAM);

    /* An airy, early-online-console backdrop, kept inside TV-safe margins. */
    ellipse(320, 258, 207, 105, 0xffedf0e5);
    ring(320, 259, 191, 96, 2, PALE);
    ring(320, 259, 171, 84, 2, PALE);
    for(int side = 0; side < 2; ++side) {
        int x = side ? 560 : 56;
        for(int row = 0; row < 6; ++row)
            rect(x + (row % 2) * 12, 181 + row * 20, 3, 3, 0xffadc9ce);
    }
    for(int i = 0; i < 5; ++i) {
        float a = seconds * .35f + i * (2 * PI / 5);
        ellipse(320 + cosf(a) * 190, 259 + sinf(a) * 95, 4, 4, 0xffadc9ce);
    }
    sparkle(190, 215, 10 + sinf(seconds * 3) * 3, ORANGE);
    sparkle(455, 275, 12 + sinf(seconds * 3 + 2) * 3, BLUE);
    sparkle(433, 187, 6, 0xffc1d3c9);
    sparkle(194, 311, 5, 0xffc1d3c9);

    centered(32, 1.5f, BLUE, "Backup & Restore VMU Saves Online!");
    /* A stepped drop shadow and a warm accent are deliberately era-appropriate. */
    lettering(146, 65, 9, NAVY, "DCVMU");
    lettering(142, 61, 9, ORANGE, "DCVMU");
    lettering(138, 57, 9, BLUE, "DCVMU");
    lettering(411, 96, 3, NAVY, ".COM");
    centered(136, 1.5f, NAVY, "CLOUD SAVES FOR DREAMCAST");

    mascot(seconds, greeting);

    /* The prompt breathes between two readable colors instead of vanishing. */
    rounded(176, 376, 288, 42, 21, NAVY);
    rounded(178, 376, 284, 38, 19, greeting ? ORANGE : BLUE);
    const char *prompt = greeting ? "HERE WE GO!" : "PRESS START";
    centered(387, 2, (!greeting && elapsed % 1600 > 1100) ? 0xffd2e4df : WHITE, prompt);
    rect(36, 452, 568, 2, 0xffd7e2da);
    for(int i = 0; i < 8; ++i)
        rect(280 + i * 10, 450, 5, 5, i == (int)(elapsed / 200 % 8) ? ORANGE : 0xffadc9ce);
    pvr_list_finish(); pvr_scene_finish();
}

/* Consume all queued keys while on the title. Waiting for release after the
   greeting keeps Start from immediately exiting (or Enter editing) the app. */
static int title_input(uint32_t *previous, int *released) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *pad = dev ? maple_dev_status(dev) : NULL;
    uint32_t buttons = pad ? pad->buttons : 0;
    int start = (buttons & ~*previous & CONT_START) != 0;
    *previous = buttons;
    *released = !(buttons & CONT_START);
    dev = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    if(dev) {
        int raw;
        while((raw = kbd_queue_pop(dev, 0)) != KBD_QUEUE_END)
            if((raw & 255) == KBD_KEY_ENTER) start = 1;
        kbd_state_t *keyboard = kbd_get_state(dev);
        if(keyboard && keyboard->key_states[KBD_KEY_ENTER].is_down) *released = 0;
    }
    return start;
}

void client_title_screen(uint32_t *previous_buttons) {
    const pvr_init_params_t params = {
        .opb_sizes = {PVR_BINSIZE_16, 0, 0, 0, 0},
        .vertex_buf_size = 512 * 1024, .opb_overflow_count = 3
    };
    int graphics = pvr_init(&params) == 0;
    if(graphics) {
        for(int i = 0; i <= CIRCLE_STEPS; ++i) {
            float a = i * (2 * PI / CIRCLE_STEPS);
            circle[i] = (title_point_t){cosf(a), sinf(a)};
        }
        pvr_poly_cxt_t context;
        pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
        context.gen.culling = PVR_CULLING_NONE;
        pvr_poly_compile(&shapes, &context);
    } else {
        printf("dcvmu: title PVR unavailable; using text fallback\n");
        vid_clear(250, 245, 233);
        bfont_draw_str_ex(vram_s + 180 * 640 + 266, 640, 0x244b, 0, 16, false, "DCVMU.com");
        bfont_draw_str_ex(vram_s + 260 * 640 + 206, 640, 0x244b, 0, 16, false, "Press START / Enter");
    }
    int released, accepted = 0;
    title_input(previous_buttons, &released);
    uint64_t began = timer_ms_gettime64(), accepted_at = 0;
    printf("dcvmu: title ready - waiting for Start / Enter\n");
    for(;;) {
        uint64_t now = timer_ms_gettime64();
        int start = title_input(previous_buttons, &released);
        if(start && !accepted) {
            accepted = 1; accepted_at = now;
            printf("dcvmu: title accepted - waiting for button release\n");
        }
        if(graphics) frame(now - began, accepted);
        if(accepted && released && now - accepted_at >= 450) break;
        thd_sleep(16);
    }
    if(graphics) { pvr_wait_ready(); pvr_shutdown(); }
    vid_set_mode(DM_640x480, PM_RGB565);
    printf("dcvmu: title complete - starting app\n");
}
