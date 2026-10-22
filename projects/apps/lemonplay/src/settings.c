#include "settings.h"

#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
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

static maple_device_t *first_vmu(void) {
    return maple_enum_type(0, MAPLE_FUNC_MEMCARD);
}

static void path_for(const maple_device_t *dev, char *out, size_t size) {
    snprintf(out, size, "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, FILENAME);
}

void settings_load(void) {
    memset(&settings, 0, sizeof(settings));
#if !defined(LP_QA) && !defined(LP_BENCH) && !defined(LP_DECODE_BENCH)
    maple_device_t *dev = first_vmu();
    if(!dev) return;
    char path[40];
    path_for(dev, path, sizeof(path));
    file_t f = fs_open(path, O_RDONLY);
    if(f == FILEHND_INVALID) return;
    record_t r;
    ssize_t n = fs_read(f, &r, sizeof(r));
    fs_close(f);
    if(n != (ssize_t)sizeof(r) || memcmp(r.magic, magic, sizeof(magic)) || r.version != 1) return;
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
    maple_device_t *dev = first_vmu();
    if(!dev) return;

    static record_t r;
    static uint8_t pixels[512];
    static uint8_t no_eyecatch;
    memset(&r, 0, sizeof(r));
    memcpy(r.magic, magic, sizeof(magic));
    r.version = 1;
    r.picture = settings.picture;
    snprintf(r.server, sizeof(r.server), "%s", settings.server);

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    snprintf(pkg.desc_short, sizeof(pkg.desc_short), "LemonPlay");
    snprintf(pkg.desc_long, sizeof(pkg.desc_long), "LemonPlay settings");
    snprintf(pkg.app_id, sizeof(pkg.app_id), "LEMONPLAY");
    /* Palette (ARGB4444): blank, lemon, lemon shade, leaf, highlight. */
    static const uint16_t palette[16] = {0x0000, 0xfee4, 0xfca2, 0xf3a3, 0xffff};
    memcpy(pkg.icon_pal, palette, sizeof(palette));
    for(int y = 0; y < 32; y++) {
        for(int x = 0; x < 32; x += 2) {
            static const char keys[] = ".YDGW";
            int a = strchr(keys, icon_art[y][x]) - keys, b = strchr(keys, icon_art[y][x + 1]) - keys;
            pixels[y * 16 + x / 2] = (uint8_t)(a << 4 | b);
        }
    }
    pkg.icon_cnt = 1;
    pkg.icon_data = pixels;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.eyecatch_data = &no_eyecatch;
    pkg.data_len = sizeof(r);
    pkg.data = (const uint8_t *)&r;

    char path[40];
    path_for(dev, path, sizeof(path));
    file_t f = fs_open(path, O_WRONLY | O_TRUNC);
    if(f == FILEHND_INVALID) return;
    fs_write(f, &r, sizeof(r));
    fs_vmu_set_header(f, &pkg);
    fs_close(f);
}

void settings_poll(void) {
    if(!dirty || timer_ms_gettime64() < save_at) return;
    dirty = 0;
#if !defined(LP_QA) && !defined(LP_BENCH) && !defined(LP_DECODE_BENCH)
    save();
#endif
}
