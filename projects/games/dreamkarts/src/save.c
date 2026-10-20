/* DreamKarts save file: the online name, so players type it once.

   It lives in a file called DREAMKARTS on the first VMU found, with an
   animated icon (tools/build_vmu.py) for the Dreamcast's memory manager. VMU block reads and writes take a
   maple frame or more each (a save touches the whole 13-block directory),
   so they run on a worker thread and the game never waits for the card.
   No card, a full card or a bad file just means no saved name. */
#include "save.h"
#include "net.h"
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
#define SAVE_VERSION 1

typedef struct {
    char magic[4];
    uint8_t version;
    uint8_t pad[3];
    char name[16];
    uint32_t check;
} SaveRecord;

static mutex_t lock = MUTEX_INITIALIZER;
static bool worker_on, load_wanted, load_done;
static char want[NET_NAME_LEN];        /* name to write; empty for none */
static char on_card[NET_NAME_LEN];     /* name the card is known to hold */
static char loaded[NET_NAME_LEN];
static int card_port = -1, card_unit = -1;   /* the worker's own */

static uint32_t record_check(const SaveRecord *r) {
    uint32_t h = 2166136261u;          /* FNV-1a over everything but the check */
    const uint8_t *p = (const uint8_t *)r;
    for(size_t i = 0; i < offsetof(SaveRecord, check); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* The name entry's rules: 1-10 of A-Z 0-9 space - . with a letter or digit. */
static bool name_valid(const char *s, size_t size) {
    size_t len = strnlen(s, size);
    if(len == 0 || len >= NET_NAME_LEN) return false;
    bool any = false;
    for(size_t i = 0; i < len; i++) {
        char c = s[i];
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if(!alnum && c != ' ' && c != '-' && c != '.') return false;
        if(alnum) any = true;
    }
    return any;
}

/* Reads the first valid name on any card; empty if there is none. */
static void read_name(char *out) {
    maple_device_t *dev;
    out[0] = '\0';
    for(int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_MEMCARD)); i++) {
        void *file;
        int size;
        if(vmufs_read(dev, SAVE_FILE, &file, &size) < 0) continue;
        vmu_pkg_t pkg;
        SaveRecord r;
        bool ok = vmu_pkg_parse(file, size, &pkg) == 0 && pkg.data_len >= (int)sizeof(r);
        if(ok) memcpy(&r, pkg.data, sizeof(r));
        free(file);
        if(!ok || memcmp(r.magic, "DKSV", 4) != 0 || r.version != SAVE_VERSION || r.check != record_check(&r) ||
           !name_valid(r.name, sizeof(r.name)))
            continue;
        strcpy(out, r.name);
        card_port = dev->port;
        card_unit = dev->unit;
        return;
    }
}

/* Writes to the card the name came from if it's still there, otherwise
   the first card. */
static bool write_name(const char *name) {
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
    strncpy(r.name, name, sizeof(r.name) - 1);
    r.check = record_check(&r);
    memset(payload, 0, sizeof(payload));
    memcpy(payload, &r, sizeof(r));

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "DREAMKARTS");
    strcpy(pkg.desc_long, "DREAMKARTS ONLINE NAME");
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
    char name[NET_NAME_LEN];
    mutex_lock(&lock);
    for(;;) {
        if(load_wanted) {
            load_wanted = false;
            mutex_unlock(&lock);
            read_name(name);
            mutex_lock(&lock);
            strcpy(on_card, name);
            strcpy(loaded, name);   /* a pulled or junked card forgets the old one */
            if(name[0]) load_done = true;
        }
        else if(want[0] && strcmp(want, on_card) != 0) {
            strcpy(name, want);
            mutex_unlock(&lock);
            write_name(name);
            mutex_lock(&lock);
            /* Recorded even if the write failed, so a full card or a pulled
               one isn't retried until the name changes again. */
            strcpy(on_card, name);
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

void save_load_name(void) {
    mutex_lock(&lock);
    load_wanted = true;
    load_done = false;
    wake_worker();
    mutex_unlock(&lock);
}

bool save_peek_name(char *out, size_t size) {
    mutex_lock(&lock);
    bool got = loaded[0] != '\0';
    if(got) snprintf(out, size, "%s", loaded);
    mutex_unlock(&lock);
    return got;
}

bool save_take_name(char *out, size_t size) {
    mutex_lock(&lock);     /* the worker never holds it across card access */
    bool got = load_done;
    if(got) snprintf(out, size, "%s", loaded);
    load_done = false;
    mutex_unlock(&lock);
    return got;
}

void save_store_name(const char *name) {
    mutex_lock(&lock);
    snprintf(want, sizeof(want), "%s", name);
    strcpy(loaded, want);   /* the name to show from now on, card or no card */
    wake_worker();
    mutex_unlock(&lock);
}
