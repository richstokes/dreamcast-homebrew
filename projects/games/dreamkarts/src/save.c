/* DreamKarts save file: the online name, the music switch and the custom
   kart, so players set them once.

   It lives in a file called DREAMKARTS on the first VMU found, with an
   animated icon (tools/build_vmu.py) for the Dreamcast's memory manager.
   VMU block reads and writes take a maple frame or more each (a save
   touches the whole 13-block directory), so they run on a worker thread
   and the game never waits for the card. No card, a full card or a bad
   file just means the defaults. */
#include "save.h"
#include "vmu_assets.h"
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_FILE "DREAMKARTS"
#define SAVE_VERSION 2

/* Version 1 held the name alone; a version 2 file adds the switches. The
   check is FNV-1a over everything before it. */
typedef struct {
    char magic[4];
    uint8_t version;
    uint8_t pad[3];
    char name[16];
    uint32_t check;
} SaveRecordV1;

typedef struct {
    char magic[4];
    uint8_t version;
    uint8_t pad[3];
    char name[16];
    uint8_t music_off;
    uint8_t custom_on;
    uint8_t custom[3];   /* body, trim, chao palette indices */
    uint8_t pad2[3];
    uint32_t check;
} SaveRecord;

SaveData save;

static mutex_t lock = MUTEX_INITIALIZER;
static bool worker_on, load_wanted, load_done, have_want, have_card;
static SaveData want;      /* what the player wants on the card */
static SaveData on_card;   /* what the worker believes the card holds (have_card) */
static SaveData loaded;    /* the last read's result */
static int gen;
static int card_port = -1, card_unit = -1;   /* the worker's own */

static uint32_t fnv(const void *data, size_t n) {
    uint32_t h = 2166136261u;
    const uint8_t *p = data;
    for(size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void defaults(SaveData *d) {
    memset(d, 0, sizeof(*d));
    d->music = true;
    d->custom = kart_custom_default;
}

/* The name entry's rules: 1-10 of A-Z 0-9 space - . with a letter or digit;
   or no name yet. */
static bool name_valid(const char *s, size_t size) {
    size_t len = strnlen(s, size);
    if(len == 0) return true;
    if(len >= SAVE_NAME_LEN) return false;
    bool any = false;
    for(size_t i = 0; i < len; i++) {
        char c = s[i];
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if(!alnum && c != ' ' && c != '-' && c != '.') return false;
        if(alnum) any = true;
    }
    return any;
}

/* A file's data as settings; false if it is not ours or is damaged. */
static bool parse_record(const uint8_t *data, int len, SaveData *out) {
    defaults(out);
    if(len < (int)sizeof(SaveRecordV1) || memcmp(data, "DKSV", 4) != 0) return false;
    if(data[4] == 1) {
        SaveRecordV1 r;
        memcpy(&r, data, sizeof(r));
        if(r.check != fnv(&r, offsetof(SaveRecordV1, check)) || !name_valid(r.name, sizeof(r.name)) || !r.name[0])
            return false;
        strcpy(out->name, r.name);
        return true;
    }
    if(data[4] != SAVE_VERSION || len < (int)sizeof(SaveRecord)) return false;
    SaveRecord r;
    memcpy(&r, data, sizeof(r));
    if(r.check != fnv(&r, offsetof(SaveRecord, check)) || !name_valid(r.name, sizeof(r.name))) return false;
    strcpy(out->name, r.name);
    out->music = !r.music_off;
    out->custom_on = r.custom_on != 0;
    out->custom.body = r.custom[0] % PALETTE_COUNT;
    out->custom.trim = r.custom[1] % PALETTE_COUNT;
    out->custom.chao = r.custom[2] % PALETTE_COUNT;
    return true;
}

/* Reads the first valid file on any card; false (and the defaults) if
   there is none. */
static bool read_card(SaveData *out) {
    maple_device_t *dev;
    defaults(out);
    for(int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_MEMCARD)); i++) {
        void *file;
        int size;
        if(vmufs_read(dev, SAVE_FILE, &file, &size) < 0) continue;
        vmu_pkg_t pkg;
        bool ok = vmu_pkg_parse(file, size, &pkg) == 0 && parse_record(pkg.data, pkg.data_len, out);
        free(file);
        if(!ok) continue;
        card_port = dev->port;
        card_unit = dev->unit;
        return true;
    }
    return false;
}

/* Writes to the card the file came from if it's still there, otherwise
   the first card. */
static bool write_card(const SaveData *d) {
    maple_device_t *dev = NULL;
    if(card_port >= 0) {
        dev = maple_enum_dev(card_port, card_unit);
        if(dev && !(dev->info.functions & MAPLE_FUNC_MEMCARD)) dev = NULL;
    }
    if(!dev) dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if(!dev) return false;

    /* 128-byte header + the icon frames + this fills whole blocks. */
    static uint8_t payload[384];
    static uint8_t no_eyecatch;
    SaveRecord r;
    memset(&r, 0, sizeof(r));
    memcpy(r.magic, "DKSV", 4);
    r.version = SAVE_VERSION;
    strncpy(r.name, d->name, sizeof(r.name) - 1);
    r.music_off = !d->music;
    r.custom_on = d->custom_on;
    r.custom[0] = d->custom.body;
    r.custom[1] = d->custom.trim;
    r.custom[2] = d->custom.chao;
    r.check = fnv(&r, offsetof(SaveRecord, check));
    memset(payload, 0, sizeof(payload));
    memcpy(payload, &r, sizeof(r));

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "DREAMKARTS");
    strcpy(pkg.desc_long, "DREAMKARTS SETTINGS");
    strcpy(pkg.app_id, "DREAMKARTS");
    memcpy(pkg.icon_pal, dk_vmu_icon_pal, sizeof(pkg.icon_pal));
    pkg.icon_cnt = DK_VMU_ICON_FRAMES;
    pkg.icon_anim_speed = 12;
    pkg.icon_data = (uint8_t *)dk_vmu_icon;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.eyecatch_data = &no_eyecatch;
    pkg.data_len = sizeof(payload);
    pkg.data = payload;

    uint8_t *file;
    int size;
    if(vmu_pkg_build(&pkg, &file, &size) < 0) return false;
    int written = vmufs_write(dev, SAVE_FILE, file, size, VMUFS_OVERWRITE);
    free(file);
    if(written < 0) return false;
    card_port = dev->port;
    card_unit = dev->unit;
    return true;
}

static void *worker(void *arg) {
    SaveData d;
    mutex_lock(&lock);
    for(;;) {
        if(load_wanted) {
            load_wanted = false;
            mutex_unlock(&lock);
            bool got = read_card(&d);
            mutex_lock(&lock);
            on_card = d;
            have_card = got;
            loaded = d;
            load_done = true;
        }
        else if(have_want && (!have_card || memcmp(&want, &on_card, sizeof(want)) != 0)) {
            d = want;
            mutex_unlock(&lock);
            write_card(&d);
            mutex_lock(&lock);
            /* Recorded even if the write failed, so a full card or a pulled
               one isn't retried until something changes again. */
            on_card = d;
            have_card = true;
        }
        else break;
    }
    worker_on = false;
    mutex_unlock(&lock);
    return NULL;
}

/* With the lock held. */
static void wake_worker(void) {
    if(worker_on) return;
    worker_on = thd_create(true, worker, NULL) != NULL;
}

void save_init(void) {
    defaults(&save);
    save_load();
}

void save_load(void) {
    mutex_lock(&lock);
    load_wanted = true;
    load_done = false;
    wake_worker();
    mutex_unlock(&lock);
}

bool save_tick(void) {
    mutex_lock(&lock);     /* the worker never holds it across card access */
    bool got = load_done;
    SaveData d = loaded;
    bool keep = have_want;   /* the player's choices this session stand */
    load_done = false;
    mutex_unlock(&lock);
    if(!got) return false;
    gen++;
    if(keep || memcmp(&d, &save, sizeof(d)) == 0) return false;
    save = d;
    return true;
}

int save_gen(void) {
    return gen;
}

void save_store(void) {
    mutex_lock(&lock);
    want = save;
    have_want = true;
    wake_worker();
    mutex_unlock(&lock);
}
