/* Ridgeline on the VMU screens. In the menus, the title's demo and on any
   VMU but the player's, the mountain: the lookout on the summit, the sun
   or the moon crossing the sky with the game's clock and a truck crawling
   along the foot. Out on the mountain the VMU in the player's controller
   is an inclinometer (side and rear views tilting with the truck, in
   degrees) over the altitude and the drive mode; a viewpoint, the summit,
   the pause menu and photo mode get a screen of their own for a while.
   Drawn and sent through vmulcd.h. */
#include "vmuscreen.h"
#include "terrain.h"
#include "tod.h"
#include "truck.h"
#include "vmulcd.h"

static float lcd_t;
static uint8_t seen_views;
static bool seen_summit;
static float view_t, summit_t;     /* seconds left on those screens */
static int view_count;

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

/* '#' art, rotated by a (radians, anticlockwise on the screen) about the
   middle of the art, which lands on (cx, cy). */
static void art_rot(const char *const *rows, int w, int h, float cx, float cy, float a) {
    float c = cosf(a), s = sinf(a);
    float hw = w * 0.5f, hh = h * 0.5f;
    int r = (int)sqrtf(hw * hw + hh * hh) + 1;
    for(int y = -r; y <= r; y++)
        for(int x = -r; x <= r; x++) {
            /* Back from the screen into the art (y grows downwards). */
            float dx = x + 0.5f, dy = y + 0.5f;
            int u = (int)floorf(dx * c - dy * s + hw), v = (int)floorf(dx * s + dy * c + hh);
            if(u >= 0 && u < w && v >= 0 && v < h && rows[v][u] == '#') vmulcd_pset((int)cx + x, (int)cy + y);
        }
}

static void degree(int x, int y) {
    vmulcd_fill(x, y, x + 3, y + 1);
    vmulcd_fill(x, y + 2, x + 3, y + 3);
    vmulcd_pset(x, y + 1);
    vmulcd_pset(x + 2, y + 1);
}

/* "-12" and a degree sign, centred on x. */
static void angle_text(int x, int y, float deg) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int)roundf(deg));
    int w = vmulcd_text_w(buf) + 4;
    vmulcd_text(x - w / 2, y, buf);
    degree(x - w / 2 + w - 3, y);
}

/* ------------------------------------------------------------------ */
/* The mountain                                                        */

/* The ridge's skyline, left to right: x, y. */
static const int8_t ridge[][2] = {
    {0, 21}, {5, 17}, {9, 18}, {15, 11}, {18, 12}, {23, 5}, {27, 9}, {30, 8}, {35, 14}, {39, 13}, {43, 17}, {47, 19},
};
#define RIDGE_N (int)(sizeof(ridge) / sizeof(ridge[0]))

static int ridge_y(int x) {
    for(int i = 1; i < RIDGE_N; i++)
        if(x <= ridge[i][0]) {
            int x0 = ridge[i - 1][0], y0 = ridge[i - 1][1], x1 = ridge[i][0], y1 = ridge[i][1];
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        }
    return ridge[RIDGE_N - 1][1];
}

/* The mountain down to row base: dark, with snow (clear) on the top. */
static void mountain(int base) {
    for(int x = 0; x < VMULCD_W; x++) {
        int top = ridge_y(x);
        vmulcd_fill(x, top, x + 1, base);
        /* The snow line wanders a little. */
        int snow = 11 + ((x * 7) % 5 == 0) - ((x * 3) % 4 == 0);
        if(top < snow) vmulcd_blank(x, top + 1, x + 1, snow);
    }
    /* The fire lookout on the summit. */
    vmulcd_fill(22, 1, 25, 3);
    vmulcd_line(22, 3, 22, 5);
    vmulcd_line(24, 3, 24, 5);
    vmulcd_pset(23, 0);
}

/* The sun crosses the sky by day, the moon by night, with the game's clock. */
static void sky(void) {
    float h = tod.hour;
    bool day = h >= 6.0f && h < 19.0f;
    float k = day ? (h - 6.0f) / 13.0f : (h >= 19.0f ? h - 19.0f : h + 5.0f) / 11.0f;
    int x = 3 + (int)(k * 41.0f), y = 9 - (int)(sinf(k * ENG_PI) * 7.0f);
    if(!day) {
        static const int8_t stars[][2] = {{2, 2}, {9, 5}, {14, 1}, {33, 2}, {40, 6}, {45, 1}, {37, 9}};
        for(int i = 0; i < 7; i++) vmulcd_pset(stars[i][0], stars[i][1]);
    }
    if(y + 2 >= ridge_y(x)) return;    /* behind the mountain */
    if(day) {
        vmulcd_fill(x - 1, y - 2, x + 2, y + 3);
        vmulcd_fill(x - 2, y - 1, x + 3, y + 2);
    }
    else {
        /* A crescent: a disc with a disc cut out of it. */
        vmulcd_fill(x - 1, y - 2, x + 2, y + 3);
        vmulcd_fill(x - 2, y - 1, x + 3, y + 2);
        vmulcd_blank(x, y - 2, x + 3, y + 2);
    }
}

static void draw_mountain(float t) {
    sky();
    mountain(23);
    /* A truck crawls along the foot of it, left to right. */
    int x = (int)(t * 3.0f) % 60 - 6;
    int bob = (int)(t * 6.0f) & 1;
    vmulcd_blank(x - 1, 18, x + 7, 23);
    vmulcd_fill(x + 2, 19 + bob, x + 5, 20 + bob);
    vmulcd_fill(x, 20 + bob, x + 6, 22);
    vmulcd_text_c(26, "RIDGELINE");
}

/* ------------------------------------------------------------------ */
/* Driving                                                             */

static const char *const side_art[] = {
    "......######.....",
    ".....#...#..#....",
    "....#....#...#...",
    "#..#.....#....###",
    "#################",
    "#################",
    ".###.......###...",
    ".###.......###...",
};
static const char *const rear_art[] = {
    "...#######...",
    "...#.....#...",
    "..#########..",
    "#############",
    "#.#########.#",
    "#############",
    ".##.......##.",
    ".##.......##.",
};

/* A truck tilting over level ground (dotted), the angle under it; it
   flashes past where the HUD's turns orange. */
static void tilt(const char *const *art, int w, int x, float deg, float warn) {
    float a = clampf(deg, -60, 60) * (ENG_PI / 180.0f);
    for(int i = x - 11; i <= x + 11; i += 2) vmulcd_pset(i, 13);
    art_rot(art, w, 8, x + 0.5f, 8, a);
    if(fabsf(deg) <= warn || ((int)(lcd_t * 4.0f) & 1)) angle_text(x, 17, deg);
}

static void draw_drive(const Truck *t) {
    static const char *const modes[DRIVE_COUNT] = {"2WD", "4WD", "LOCK"};
    tilt(side_art, 17, 11, t->pitch, 30);
    tilt(rear_art, 13, 36, t->roll, 25);
    vmulcd_fill(0, 24, VMULCD_W, 25);
    if(t->stalled) {
        if((int)(lcd_t * 2.0f) & 1) vmulcd_text_c(26, "STALLED");
        return;
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%dFT", (int)(2380.0f + t->pos.y * M_TO_FT));
    vmulcd_text(0, 26, buf);
    vmulcd_text(VMULCD_W - vmulcd_text_w(modes[t->mode]), 26, modes[t->mode]);
}

static void draw_view(void) {
    char buf[16];
    /* A pair of binoculars. */
    for(int i = 0; i < 2; i++) {
        int x = 14 + i * 12;
        vmulcd_fill(x + 1, 2, x + 6, 3);
        vmulcd_fill(x, 3, x + 8, 10);
        vmulcd_blank(x + 2, 5, x + 6, 9);
    }
    vmulcd_fill(22, 4, 26, 7);
    vmulcd_text_c(14, "VIEWPOINT");
    snprintf(buf, sizeof(buf), "%d OF %d", view_count, LVL_VIEWS);
    vmulcd_text_c(22, buf);
}

static void draw_summit(void) {
    mountain(23);
    /* A flag on the lookout. */
    vmulcd_line(25, 0, 25, 3);
    vmulcd_fill(26, 0, 30, 2);
    if((int)(lcd_t * 2.0f) & 1) vmulcd_text_c(26, "SUMMIT!");
    else {
        char buf[16];
        int s = (int)game.clock;
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
        vmulcd_text_c(26, buf);
    }
}

static void draw_pause(void) {
    vmulcd_fill(15, 3, 21, 19);
    vmulcd_fill(27, 3, 33, 19);
    vmulcd_text_c(24, "PAUSED");
}

static void draw_photo(void) {
    /* A camera. */
    vmulcd_fill(19, 3, 26, 5);
    vmulcd_fill(12, 5, 36, 19);
    vmulcd_blank(19, 7, 29, 17);
    vmulcd_fill(21, 9, 27, 15);
    vmulcd_blank(23, 11, 25, 13);
    vmulcd_text_c(24, "PHOTO");
}

/* ------------------------------------------------------------------ */

static void compose(int port) {
    bool mine = pad.connected && port == pad.port;
    if(!mine || game.state != ST_DRIVE || game.demo) draw_mountain(lcd_t);
    else if(game.photo) draw_photo();
    else if(game.paused) draw_pause();
    else if(summit_t > 0) draw_summit();
    else if(view_t > 0) draw_view();
    else draw_drive(&trucks[0]);
}

static float gap(int port) {
    return game.state == ST_DRIVE && !game.demo && pad.connected && port == pad.port ? 0.1f : 0.25f;
}

void vmuscreen_update(float dt) {
    lcd_t += dt;
    /* A new viewpoint or the summit, since the last frame. */
    if(game.state == ST_DRIVE && !game.demo) {
        if(game.views_found & ~seen_views) {
            view_t = 3.0f;
            view_count = __builtin_popcount(game.views_found);
        }
        if(game.summit_reached && !seen_summit) summit_t = 7.0f;
    }
    seen_views = game.views_found;
    seen_summit = game.summit_reached;
    if(view_t > 0) view_t -= dt;
    if(summit_t > 0) summit_t -= dt;
    vmulcd_update(dt, gap, compose);
}
