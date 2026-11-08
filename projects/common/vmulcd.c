/* VMU screens: see vmulcd.h. */
#include "vmulcd.h"
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <stdbool.h>
#include <string.h>

#define SLOTS (MAPLE_PORT_COUNT * 2)   /* a VMU in either slot of each controller */

typedef struct {
    maple_device_t *dev;
    uint32_t sent[48];
    bool valid;
    float since;
} vmulcd_slot_t;

static vmulcd_slot_t slots[SLOTS];
static uint32_t fb_words[48];          /* 32 rows of 6 bytes; must be 4-byte aligned */
static uint8_t *const fb = (uint8_t *)fb_words;

/* ' '..'Z': width, then rows (bit 7 leftmost). */
static const struct {
    uint8_t w, rows[5];
} font[59] = {
    {2, {0x00, 0x00, 0x00, 0x00, 0x00}}, /*   */
    {1, {0x80, 0x80, 0x80, 0x00, 0x80}}, /* ! */
    {3, {0xa0, 0xa0, 0x00, 0x00, 0x00}}, /* " */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* # */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* $ */
    {3, {0xa0, 0x20, 0x40, 0x80, 0xa0}}, /* % */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* & */
    {1, {0x80, 0x80, 0x00, 0x00, 0x00}}, /* ' */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* ( */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* ) */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /*  *  */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* + */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* , */
    {3, {0x00, 0x00, 0xe0, 0x00, 0x00}}, /* - */
    {1, {0x00, 0x00, 0x00, 0x00, 0x80}}, /* . */
    {3, {0x20, 0x20, 0x40, 0x80, 0x80}}, /*  /  */
    {3, {0xe0, 0xa0, 0xa0, 0xa0, 0xe0}}, /* 0 */
    {3, {0x40, 0xc0, 0x40, 0x40, 0xe0}}, /* 1 */
    {3, {0xc0, 0x20, 0x40, 0x80, 0xe0}}, /* 2 */
    {3, {0xc0, 0x20, 0x40, 0x20, 0xc0}}, /* 3 */
    {3, {0xa0, 0xa0, 0xe0, 0x20, 0x20}}, /* 4 */
    {3, {0xe0, 0x80, 0xc0, 0x20, 0xc0}}, /* 5 */
    {3, {0x60, 0x80, 0xe0, 0xa0, 0xe0}}, /* 6 */
    {3, {0xe0, 0x20, 0x40, 0x40, 0x40}}, /* 7 */
    {3, {0xe0, 0xa0, 0xe0, 0xa0, 0xe0}}, /* 8 */
    {3, {0xe0, 0xa0, 0xe0, 0x20, 0xc0}}, /* 9 */
    {1, {0x00, 0x80, 0x00, 0x80, 0x00}}, /* : */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* ; */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* < */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* = */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* > */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* ? */
    {0, {0x00, 0x00, 0x00, 0x00, 0x00}}, /* @ */
    {3, {0x40, 0xa0, 0xe0, 0xa0, 0xa0}}, /* A */
    {3, {0xc0, 0xa0, 0xc0, 0xa0, 0xc0}}, /* B */
    {3, {0x60, 0x80, 0x80, 0x80, 0x60}}, /* C */
    {3, {0xc0, 0xa0, 0xa0, 0xa0, 0xc0}}, /* D */
    {3, {0xe0, 0x80, 0xc0, 0x80, 0xe0}}, /* E */
    {3, {0xe0, 0x80, 0xc0, 0x80, 0x80}}, /* F */
    {3, {0x60, 0x80, 0xa0, 0xa0, 0x60}}, /* G */
    {3, {0xa0, 0xa0, 0xe0, 0xa0, 0xa0}}, /* H */
    {3, {0xe0, 0x40, 0x40, 0x40, 0xe0}}, /* I */
    {3, {0x20, 0x20, 0x20, 0xa0, 0x40}}, /* J */
    {3, {0xa0, 0xa0, 0xc0, 0xa0, 0xa0}}, /* K */
    {3, {0x80, 0x80, 0x80, 0x80, 0xe0}}, /* L */
    {5, {0x88, 0xd8, 0xa8, 0x88, 0x88}}, /* M */
    {4, {0x90, 0xd0, 0xb0, 0x90, 0x90}}, /* N */
    {3, {0x40, 0xa0, 0xa0, 0xa0, 0x40}}, /* O */
    {3, {0xc0, 0xa0, 0xc0, 0x80, 0x80}}, /* P */
    {3, {0x40, 0xa0, 0xa0, 0xc0, 0x60}}, /* Q */
    {3, {0xc0, 0xa0, 0xc0, 0xa0, 0xa0}}, /* R */
    {3, {0x60, 0x80, 0x40, 0x20, 0xc0}}, /* S */
    {3, {0xe0, 0x40, 0x40, 0x40, 0x40}}, /* T */
    {3, {0xa0, 0xa0, 0xa0, 0xa0, 0xe0}}, /* U */
    {3, {0xa0, 0xa0, 0xa0, 0xa0, 0x40}}, /* V */
    {5, {0x88, 0x88, 0xa8, 0xd8, 0x88}}, /* W */
    {3, {0xa0, 0xa0, 0x40, 0xa0, 0xa0}}, /* X */
    {3, {0xa0, 0xa0, 0x40, 0x40, 0x40}}, /* Y */
    {3, {0xe0, 0x20, 0x40, 0x80, 0xe0}}, /* Z */
};

void vmulcd_pset(int x, int y) {
    if(x >= 0 && x < VMULCD_W && y >= 0 && y < VMULCD_H) fb[y * 6 + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

void vmulcd_fill(int x0, int y0, int x1, int y1) {
    for(int y = y0; y < y1; y++)
        for(int x = x0; x < x1; x++) vmulcd_pset(x, y);
}

void vmulcd_blank(int x0, int y0, int x1, int y1) {
    for(int y = y0 < 0 ? 0 : y0; y < y1 && y < VMULCD_H; y++)
        for(int x = x0 < 0 ? 0 : x0; x < x1 && x < VMULCD_W; x++) fb[y * 6 + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
}

void vmulcd_line(int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for(;;) {
        vmulcd_pset(x0, y0);
        if(x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if(e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if(e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void vmulcd_bits(const uint64_t *rows, int h, int x, int y) {
    for(int j = 0; j < h; j++) {
        uint64_t r = rows[j];
        for(int i = 0; r; i++, r <<= 1)
            if(r & (1ull << 63)) vmulcd_pset(x + i, y + j);
    }
}

void vmulcd_bits_cut(const uint64_t *rows, int h, int x, int y) {
    for(int j = -1; j <= h; j++) {
        uint64_t r = 0;
        for(int k = j - 1; k <= j + 1; k++)
            if(k >= 0 && k < h) r |= rows[k];
        r |= r >> 1 | r << 1;
        for(int i = 0; r; i++, r <<= 1)
            if(r & (1ull << 63)) vmulcd_blank(x + i, y + j, x + i + 1, y + j + 1);
    }
    vmulcd_bits(rows, h, x, y);
}

int vmulcd_text_w(const char *s) {
    int w = 0;
    for(; *s; s++) {
        int c = *s - 32;
        if(c >= 0 && c < 59) w += font[c].w + 1;
    }
    return w > 0 ? w - 1 : 0;
}

void vmulcd_text(int x, int y, const char *s) {
    for(; *s; s++) {
        int c = *s - 32;
        if(c < 0 || c >= 59) continue;
        for(int j = 0; j < 5; j++)
            for(int i = 0; i < font[c].w; i++)
                if(font[c].rows[j] & (0x80 >> i)) vmulcd_pset(x + i, y + j);
        x += font[c].w + 1;
    }
}

void vmulcd_text_c(int y, const char *s) {
    vmulcd_text((VMULCD_W - vmulcd_text_w(s)) / 2, y, s);
}


void vmulcd_update(float dt, float (*gap)(int port), void (*draw)(int port)) {
    for(int s = 0; s < SLOTS; s++) slots[s].since += dt;
    maple_device_t *dev;
    for(int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_LCD)); i++) {
        int s = dev->port * 2 + dev->unit - 1;
        if(s < 0 || s >= SLOTS) continue;
        vmulcd_slot_t *sl = &slots[s];
        if(sl->dev != dev) {
            sl->dev = dev;
            sl->valid = false;
        }
        if(sl->since < gap(dev->port)) continue;
        memset(fb_words, 0, sizeof(fb_words));
        draw(dev->port);
        if(sl->valid && memcmp(sl->sent, fb_words, sizeof(fb_words)) == 0) continue;
        if(vmu_draw_lcd_rotated(dev, fb_words) == MAPLE_EOK) {
            memcpy(sl->sent, fb_words, sizeof(fb_words));
            sl->valid = true;
            sl->since = 0;
        }
    }
}
