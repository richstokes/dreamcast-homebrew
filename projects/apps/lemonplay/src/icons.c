/* LemonPlay's icons, drawn at boot into an ARGB4444 atlas: the 64x64 lemon
   logo in colour, and 24x24 white masks (tinted when drawn) for the rest.
   Shapes are signed distances, sampled 4x4 per pixel for smooth edges. */
#include "icons.h"

#include <math.h>
#include <string.h>

#define SS 4   /* samples per pixel, each way */

typedef struct {
    float r, g, b, a;
} rgba_t;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static float circle(float x, float y, float cx, float cy, float r) {
    return hypotf(x - cx, y - cy) - r;
}

static float box(float x, float y, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(x - cx) - hw + r, qy = fabsf(y - cy) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return hypotf(ox, oy) + fminf(fmaxf(qx, qy), 0) - r;
}

static float capsule(float x, float y, float ax, float ay, float bx, float by, float r) {
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    float h = clampf((px * dx + py * dy) / (dx * dx + dy * dy), 0, 1);
    return hypotf(px - dx * h, py - dy * h) - r;
}

/* Distance to an ellipse, good enough near the edge for antialiasing. */
static float ellipse(float x, float y, float a, float b) {
    float k = hypotf(x / a, y / b);
    return (k - 1) * fminf(a, b);
}

static float smin(float a, float b, float k) {
    float h = clampf(0.5f + 0.5f * (b - a) / k, 0, 1);
    return b + (a - b) * h - k * h * (1 - h);
}

static int in_triangle(float x, float y, float ax, float ay, float bx, float by, float cx, float cy) {
    float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
    float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
    float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
    int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

/* Is sample (x, y) of a 24x24 mask icon inside it? */
static int mask_inside(int icon, float x, float y) {
    switch(icon) {
        case ICON_FOLDER:
            return fminf(box(x, y, 12, 14, 10.5f, 7.5f, 2), box(x, y, 7.5f, 6, 5, 2.5f, 1.5f)) < 0;
        case ICON_FILM: {
            float d = box(x, y, 12, 12, 10.5f, 8.5f, 2);
            d = fmaxf(d, -box(x, y, 12, 12, 5.5f, 6, 1));
            for(int i = 0; i < 4; i++) {
                float hy = 6 + i * 4;
                d = fmaxf(d, -box(x, y, 4, hy, 1.1f, 1, 0.3f));
                d = fmaxf(d, -box(x, y, 20, hy, 1.1f, 1, 0.3f));
            }
            return d < 0;
        }
        case ICON_NOTE: {
            float head = ellipse(x - 8.5f, y - 18, 4.5f, 3.5f);
            float stem = capsule(x, y, 12.2f, 18, 12.2f, 3.5f, 1.2f);
            float flag = capsule(x, y, 12.2f, 3.5f, 18.5f, 9.5f, 1.6f);
            return fminf(head, fminf(stem, flag)) < 0;
        }
        case ICON_SERVER: {
            float d = fminf(box(x, y, 12, 7, 10, 4.5f, 1.8f), box(x, y, 12, 17.5f, 10, 4.5f, 1.8f));
            d = fmaxf(d, -circle(x, y, 17.5f, 7, 1.4f));
            d = fmaxf(d, -circle(x, y, 17.5f, 17.5f, 1.4f));
            d = fmaxf(d, -box(x, y, 8, 7, 3.5f, 0.8f, 0.4f));
            d = fmaxf(d, -box(x, y, 8, 17.5f, 3.5f, 0.8f, 0.4f));
            return d < 0;
        }
        case ICON_PLAY:
            return in_triangle(x, y, 6, 3.5f, 6, 20.5f, 20.5f, 12);
        case ICON_PAUSE:
            return fminf(box(x, y, 7.5f, 12, 3, 8.5f, 1), box(x, y, 16.5f, 12, 3, 8.5f, 1)) < 0;
        case ICON_DISC:
            return circle(x, y, 12, 12, 11.5f) < 0;
        case ICON_SEARCH:
            return fminf(fabsf(circle(x, y, 10, 10, 6)) - 1.7f, capsule(x, y, 14.5f, 14.5f, 20.5f, 20.5f, 2)) < 0;
        case ICON_PLUS:
            return fminf(capsule(x, y, 12, 4, 12, 20, 2), capsule(x, y, 4, 12, 20, 12, 2)) < 0;
        case ICON_WARNING: {
            int tri = in_triangle(x, y, 12, 2, 1.5f, 21, 22.5f, 21);
            int mark = capsule(x, y, 12, 8.5f, 12, 13.5f, 1.4f) < 0 || circle(x, y, 12, 17.3f, 1.5f) < 0;
            return tri && !mark;
        }
    }
    return 0;
}

static rgba_t mix(rgba_t a, rgba_t b, float t) {
    t = clampf(t, 0, 1);
    return (rgba_t){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

/* One sample of the lemon logo (64x64). */
static rgba_t lemon_sample(float x, float y) {
    rgba_t out = {0, 0, 0, 0};
    /* Body: an ellipse leaning up to the right, with a nub at each end. */
    const float cx = 30, cy = 35, angle = -0.48f;
    float ca = cosf(angle), sa = sinf(angle);
    float lx = (x - cx) * ca + (y - cy) * sa, ly = -(x - cx) * sa + (y - cy) * ca;
    float body = ellipse(lx, ly, 23, 17.5f);
    body = smin(body, circle(lx, ly, 24.5f, 0, 3.6f), 5);
    body = smin(body, circle(lx, ly, -24.5f, 0, 3.6f), 5);
    if(body < 0) {
        rgba_t light = {1.0f, 0.95f, 0.45f, 1}, mid = {0.97f, 0.83f, 0.20f, 1}, dark = {0.85f, 0.58f, 0.06f, 1};
        /* Lit from the top left, darker towards the rim. */
        float shade = clampf((lx * 0.35f + ly * 0.94f) / 18 * 0.5f + 0.5f, 0, 1);
        out = shade < 0.5f ? mix(light, mid, shade * 2) : mix(mid, dark, (shade - 0.5f) * 2);
        out = mix(out, dark, clampf((body + 3.5f) / 3.5f, 0, 1) * 0.55f);
        /* Pores, faintly. */
        float pore = sinf(x * 1.9f) * sinf(y * 2.3f + x * 0.7f);
        if(pore > 0.92f) out = mix(out, dark, 0.25f);
        /* Highlight. */
        float glint = ellipse(lx + 7, ly + 8.5f, 7, 2.6f);
        if(glint < 0) out = mix(out, (rgba_t){1, 1, 0.88f, 1}, clampf(-glint / 1.5f, 0, 1) * 0.85f);
    }
    /* Stem and leaf at the upper tip. */
    float stem = capsule(x, y, 51.5f, 21, 49, 15, 1.5f);
    if(stem < 0) out = (rgba_t){0.47f, 0.34f, 0.12f, 1};
    float leaf_angle = 0.62f, la = cosf(leaf_angle), lsa = sinf(leaf_angle);
    float fx = (x - 42) * la + (y - 11) * lsa, fy = -(x - 42) * lsa + (y - 11) * la;
    float leaf = fmaxf(circle(fx, fy, 0, 6.5f, 10.5f), circle(fx, fy, 0, -6.5f, 10.5f));
    if(leaf < 0) {
        rgba_t green = {0.38f, 0.72f, 0.28f, 1}, deep = {0.20f, 0.50f, 0.16f, 1};
        out = mix(green, deep, clampf((fy + 4) / 8, 0, 1));
        if(fabsf(fy) < 0.6f && fabsf(fx) < 7) out = mix(out, (rgba_t){0.62f, 0.86f, 0.45f, 1}, 0.8f);
    }
    return out;
}

static uint16_t pack4444(rgba_t c) {
    int a = (int)(clampf(c.a, 0, 1) * 15 + 0.5f);
    int r = (int)(clampf(c.r, 0, 1) * 15 + 0.5f);
    int g = (int)(clampf(c.g, 0, 1) * 15 + 0.5f);
    int b = (int)(clampf(c.b, 0, 1) * 15 + 0.5f);
    return (uint16_t)(a << 12 | r << 8 | g << 4 | b);
}

void icons_cell(int icon, int *x, int *y, int *size) {
    if(icon == ICON_LEMON) {
        *x = 0;
        *y = 0;
        *size = 64;
        return;
    }
    int i = icon - 1;
    *x = 64 + (i % 6) * 32;
    *y = (i / 6) * 32;
    *size = 24;
}

void icons_draw(uint16_t *pixels, int stride) {
    for(int icon = 0; icon < ICON_COUNT; icon++) {
        int ox, oy, size;
        icons_cell(icon, &ox, &oy, &size);
        for(int py = 0; py < size; py++) {
            for(int px = 0; px < size; px++) {
                rgba_t sum = {0, 0, 0, 0};
                for(int sy = 0; sy < SS; sy++) {
                    for(int sx = 0; sx < SS; sx++) {
                        float x = px + (sx + 0.5f) / SS, y = py + (sy + 0.5f) / SS;
                        rgba_t s;
                        if(icon == ICON_LEMON) {
                            s = lemon_sample(x, y);
                        }
                        else {
                            float in = mask_inside(icon, x, y) ? 1.0f : 0.0f;
                            s = (rgba_t){1, 1, 1, in};
                        }
                        /* Premultiply while averaging so edges keep their colour. */
                        sum.r += s.r * s.a;
                        sum.g += s.g * s.a;
                        sum.b += s.b * s.a;
                        sum.a += s.a;
                    }
                }
                rgba_t c = {1, 1, 1, sum.a / (SS * SS)};
                if(sum.a > 0) {
                    c.r = sum.r / sum.a;
                    c.g = sum.g / sum.a;
                    c.b = sum.b / sum.a;
                }
                pixels[(oy + py) * stride + ox + px] = pack4444(c);
            }
        }
    }
}
