#include "settings.h"
#include "../../../common/vmusave.h"

#include <kos.h>
#include <stdio.h>
#include <string.h>

#define FILENAME "LEMONPLAY"
#define SAVE_DELAY_MS 1500

/* Small enough that header, icon and record fit in two VMU blocks. */
typedef struct {
    char magic[8];
    int32_t version;
    int32_t picture;
    char server[96];
} record_t;

static const char magic[8] = {'L', 'E', 'M', 'O', 'N', 'C', 'F', '1'};

settings_t settings;
static int dirty;
static uint64_t save_at;
static int card_port = -1, card_unit = -1;

const char *picture_name(int mode) {
    static const char *names[] = {"FIT", "ZOOM", "WIDE"};
    return names[mode];
}

const char *picture_help(int mode) {
    static const char *help[] = {
        "Whole picture, with black bars",
        "Widescreen cropped to fill the screen",
        "For a 16:9 TV set to stretch",
    };
    return help[mode];
}

void settings_load(void) {
    memset(&settings, 0, sizeof(settings));
#if !defined(LP_QA) && !defined(LP_BENCH) && !defined(LP_DECODE_BENCH)
    maple_device_t *dev = vmusave_card(&card_port, &card_unit);
    if(!dev) return;
    record_t r;
    int n = vmusave_read(dev, FILENAME, &r, sizeof(r));
    if(n != (int)sizeof(r) || memcmp(r.magic, magic, sizeof(magic)) || r.version != 1) return;
    if(r.picture >= 0 && r.picture < PICTURE_MODES) settings.picture = r.picture;
    r.server[sizeof(r.server) - 1] = '\0';
    snprintf(settings.server, sizeof(settings.server), "%s", r.server);
#endif
}

void settings_changed(void) {
    dirty = 1;
    save_at = timer_ms_gettime64() + SAVE_DELAY_MS;
}

/* A 32x32 lemon for the VMU file manager. */
static const char *const icon_art[32] = {
    "................................",
    "................................",
    "......................GG........",
    "....................GGGG........",
    "...................GGGG.........",
    "..................GGG...........",
    "...........YYYYYYYY.............",
    "........YYYYYYYYYYYYYY..........",
    "......YYYYYWWYYYYYYYYYYY........",
    ".....YYYYWWWYYYYYYYYYYYYY.......",
    "....YYYYWWYYYYYYYYYYYYYYYY......",
    "...YYYYYWYYYYYYYYYYYYYYYYYY.....",
    "..YYYYYYYYYYYYYYYYYYYYYYYYYY....",
    "..YYYYYYYYYYYYYYYYYYYYYYYYYYY...",
    ".YYYYYYYYYYYYYYYYYYYYYYYYYYYY...",
    ".YYYYYYYYYYYYYYYYYYYYYYYYYYYYY..",
    ".YYYYYYYYYYYYYYYYYYYYYYYYYYYYY..",
    ".YYYYYYYYYYYYYYYYYYYYYYYYYYYYY..",
    "..YYYYYYYYYYYYYYYYYYYYYYYYYYYY..",
    "..YYYYYYYYYYYYYYYYYYYYYYYYYYYD..",
    "...YYYYYYYYYYYYYYYYYYYYYYYYYD...",
    "....YYYYYYYYYYYYYYYYYYYYYYYDD...",
    ".....YYYYYYYYYYYYYYYYYYYYYDD....",
    "......YYYYYYYYYYYYYYYYYYDDD.....",
    "........YYYYYYYYYYYYYYDDD.......",
    "..........DDYYYYYYYDDDD.........",
    "............DDDDDDDD............",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
};

static void save(void) {
    maple_device_t *dev = vmusave_card(&card_port, &card_unit);
    if(!dev) return;

    static record_t r;
    static uint8_t pixels[512];
    memset(&r, 0, sizeof(r));
    memcpy(r.magic, magic, sizeof(magic));
    r.version = 1;
    r.picture = settings.picture;
    snprintf(r.server, sizeof(r.server), "%s", settings.server);

    vmu_pkg_t pkg;
    vmusave_header(&pkg, "LEMONPLAY", "LemonPlay", "LemonPlay settings");
    /* Palette (ARGB4444): blank, lemon, lemon shade, leaf, highlight. */
    static const uint16_t palette[] = {0x0000, 0xfee4, 0xfca2, 0xf3a3, 0xffff};
    vmusave_icon(&pkg, pixels, icon_art, ".YDGW", palette);
    if(vmusave_write(dev, FILENAME, &pkg, &r, sizeof(r)) < 0)
        printf("LemonPlay: settings save failed on VMU %c%d\n", 'A' + dev->port, dev->unit);
}

void settings_poll(void) {
    if(!dirty || timer_ms_gettime64() < save_at) return;
    dirty = 0;
#if !defined(LP_QA) && !defined(LP_BENCH) && !defined(LP_DECODE_BENCH)
    save();
#endif
}
