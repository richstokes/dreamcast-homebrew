/* DreamKarts save file: the online name, the music switch and volume, and the custom
   kart, so players set them once.

   It lives in a file called DREAMKARTS on a VMU, with an animated icon
   (tools/build_vmu.py) for the Dreamcast's memory manager. VMU block
   reads and writes take a maple frame or more each (a save touches the
   whole 13-block directory), so they run on a worker thread and the game
   never waits for the card. No card or a bad file just means the
   defaults.

   Which card: the first read looks at every card. One with a save is the
   card from then on; with no save anywhere it is the first card, if that
   has room. Two saves, or a first card with no room, are the player's to
   settle (the title asks, engine/cardpick.h), and until they have nothing
   is written. A write that fails says so and is tried again at the next
   change; the next time the title comes up the cards are looked at
   again. So they are when a change has no card to go to and one has been
   put in since. */
#include "save.h"
#include "vmu_assets.h"
#include "engine/cardpick.h"
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
    uint8_t volume1;     /* music volume + 1; 0 (files from before it) means the default */
    uint8_t drift_show1; /* drift counter mode + 1; 0 (files from before it) means Free Roam */
    uint8_t pad2;
    uint32_t check;
} SaveRecord;

SaveData save;

static mutex_t lock = MUTEX_INITIALIZER;
static bool worker_on, load_wanted, load_done, have_want, have_card;
static SaveData want;      /* what the player wants on the card */
static SaveData on_card;   /* what the worker believes the card holds (have_card) */
static SaveData loaded;    /* the last read's result */
static int gen;
/* 128-byte header + the icon frames + the payload fills whole blocks. */
#define PAYLOAD_BYTES 384
#define SAVE_BLOCKS vmusave_blocks(DK_VMU_ICON_FRAMES, PAYLOAD_BYTES)
/* The card, all under the lock: settled (card_port and card_unit), the
   player being asked which (asking: the survey, each save's name and
   settings), or the player would rather not save. */
static int card_port = -1, card_unit = -1;
static bool card_set, card_off, asking, write_failed;
static volatile bool fail_note;   /* a write failed and the player has not been told */
static volatile bool looking;     /* a read is wanted or under way: the title waits for it */
static uint32_t looked_mask;      /* the cards in at the last look (vmusave_plugged) */
static bool looked;
static int ask_kind, ask_count;
static vmusave_slot_t ask_slots[VMUSAVE_SLOTS];   /* without their files */
static char ask_notes[VMUSAVE_SLOTS][SAVE_NAME_LEN + 1];
static SaveData ask_data[VMUSAVE_SLOTS];

static uint32_t fnv(const void *data, size_t n) {
    uint32_t h = 2166136261u;
    const uint8_t *p = data;
    for(size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void defaults(SaveData *d) {
    memset(d, 0, sizeof(*d));
    d->music = true;
    d->volume = SAVE_VOLUME_DEFAULT;
    d->drift_show = DRIFT_SHOW_ROAM;
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
    if(r.volume1 >= 1 && r.volume1 <= SAVE_VOLUME_MAX + 1) out->volume = r.volume1 - 1;
    out->custom_on = r.custom_on != 0;
    if(r.drift_show1 >= 1 && r.drift_show1 <= DRIFT_SHOW_COUNT) out->drift_show = r.drift_show1 - 1;
    out->custom.body = r.custom[0] % PALETTE_COUNT;
    out->custom.trim = r.custom[1] % PALETTE_COUNT;
    out->custom.chao = r.custom[2] % PALETTE_COUNT;
    return true;
}

/* vmufs_read, tried three times. A block read that times out (the maple
   bus held up for 100 ms) fails the whole read, and taking that as no file
   would let the next settings change write the defaults over the player's
   save. A card without the file costs the worker a second or so. */
static int read_file(maple_device_t *dev, void **file, int *size) {
    int rv = -1;
    for(int tries = 0; tries < 3 && rv < 0; tries++) {
        if(tries) thd_sleep(50);
        rv = vmufs_read(dev, SAVE_FILE, file, size);
    }
    return rv;
}

/* The settled card, if it is still in. */
static maple_device_t *the_card(void) {
    mutex_lock(&lock);
    bool set = card_set;
    int port = card_port, unit = card_unit;
    mutex_unlock(&lock);
    if(!set) return NULL;
    maple_device_t *dev = maple_enum_dev(port, unit);
    return dev && (dev->info.functions & MAPLE_FUNC_MEMCARD) ? dev : NULL;
}

/* Reads the save: the settled card's once it is known to hold one, and
   until then looks at every card (see the top), so that a card put in
   since with a save on it is found. False, and the defaults, if there is
   none to read or the player has to say which. */
static bool read_card(SaveData *out) {
    defaults(out);
    mutex_lock(&lock);
    bool known = have_card && !write_failed;   /* (after a failed write: is there still room?) */
    mutex_unlock(&lock);
    maple_device_t *dev = known ? the_card() : NULL;
    if(dev) {
        void *file;
        int size;
        if(read_file(dev, &file, &size) < 0) return false;
        vmu_pkg_t pkg;
        bool ok = vmu_pkg_parse(file, size, &pkg) == 0 && parse_record(pkg.data, pkg.data_len, out);
        free(file);
        if(!ok) defaults(out);
        return ok;
    }
    mutex_lock(&lock);
    bool held = asking || card_off;
    mutex_unlock(&lock);
    if(held) return false;

    static vmusave_slot_t slots[VMUSAVE_SLOTS];
    static SaveData data[VMUSAVE_SLOTS];
    uint64_t t0 = timer_ms_gettime64();
    uint32_t plugged = vmusave_plugged();
    int n = vmusave_survey(SAVE_FILE, slots, VMUSAVE_SLOTS), pick = 0;
    if(n) printf("DreamKarts save: looked at %d cards in %lu ms\n", n, (unsigned long)(timer_ms_gettime64() - t0));
    for(int i = 0; i < n; i++) {
        const uint8_t *payload;
        int len = vmusave_slot_payload(&slots[i], &payload);
        defaults(&data[i]);
        if(slots[i].save && !(len >= 0 && parse_record(payload, len, &data[i]))) slots[i].save = false;
    }
    vmusave_survey_free(slots, n);
    int what = vmusave_decide(slots, n, SAVE_BLOCKS, &pick);
    bool got = false;
    mutex_lock(&lock);
    card_set = false;
    write_failed = false;
    looked = true;
    looked_mask = plugged;
    if(what == VMUSAVE_USE) {
        card_port = slots[pick].port;
        card_unit = slots[pick].unit;
        card_set = true;
        if(slots[pick].save) {
            *out = data[pick];
            got = true;
        }
    }
#if defined(DK_AUTOTEST) && !defined(DK_AT_CARDS)
    /* Test builds drive the title themselves and would answer the
       question by accident: they go without a save instead. */
    else if(what == VMUSAVE_ASK_WHICH || what == VMUSAVE_ASK_ROOM) card_off = true;
#endif
    else if(what == VMUSAVE_ASK_WHICH || what == VMUSAVE_ASK_ROOM) {
        asking = true;
        ask_kind = what == VMUSAVE_ASK_WHICH ? CARDPICK_WHICH : CARDPICK_ROOM;
        ask_count = n;
        for(int i = 0; i < n; i++) {
            ask_slots[i] = slots[i];
            ask_data[i] = data[i];
            snprintf(ask_notes[i], sizeof(ask_notes[i]), "%.*s", SAVE_NAME_LEN, data[i].name[0] ? data[i].name : "NO NAME");
        }
    }
    mutex_unlock(&lock);
    return got;
}

/* Writes to the settled card; false with none, or when it will not take
   the save (full, or pulled). */
static bool write_card(const SaveData *d) {
    maple_device_t *dev = the_card();
    bool ok = false;

    static uint8_t payload[PAYLOAD_BYTES];
    static uint8_t no_eyecatch;
    SaveRecord r;
    memset(&r, 0, sizeof(r));
    memcpy(r.magic, "DKSV", 4);
    r.version = SAVE_VERSION;
    strncpy(r.name, d->name, sizeof(r.name) - 1);
    r.music_off = !d->music;
    r.volume1 = (uint8_t)(d->volume + 1);
    r.custom_on = d->custom_on;
    r.drift_show1 = (uint8_t)(d->drift_show + 1);
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
    if(dev && vmu_pkg_build(&pkg, &file, &size) == 0) {
        /* (a write that times out fails too: twice more before giving
           up, unless the card said it has no room, -7) */
        int rv = -1;
        for(int tries = 0; tries < 3 && rv < 0 && rv != -7; tries++) {
            if(tries) thd_sleep(50);
            rv = vmufs_write(dev, SAVE_FILE, file, size, VMUFS_OVERWRITE);
        }
        ok = rv >= 0;
        free(file);
    }
    mutex_lock(&lock);
    write_failed = !ok;
    if(!ok) fail_note = true;
    mutex_unlock(&lock);
    return ok;
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
            looking = load_wanted;
        }
        else if(card_set && have_want && (!have_card || memcmp(&want, &on_card, sizeof(want)) != 0)) {
            d = want;
            mutex_unlock(&lock);
            write_card(&d);
            mutex_lock(&lock);
            /* Recorded even if the write failed, so it isn't retried until
               the cards have been looked at again or something else
               changes. */
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

/* With the lock held. */
static void want_load(void) {
    load_wanted = looking = true;
    load_done = false;
    wake_worker();
    if(!worker_on) load_wanted = looking = false;   /* no thread to be had: nothing is waited for */
}

void save_load(void) {
    mutex_lock(&lock);
    want_load();
    mutex_unlock(&lock);
}

bool save_looking(void) {
    return looking;
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

void save_finish(void) {
    for(int i = 0; i < 300; i++) {
        mutex_lock(&lock);
        bool busy = worker_on;
        mutex_unlock(&lock);
        if(!busy) return;
        thd_sleep(10);
    }
}

void save_store(void) {
    mutex_lock(&lock);
    want = save;
    have_want = true;
    /* With no card settled, one put in since the last look is found now. */
    if(!card_set && !asking && !card_off && !load_wanted && (!looked || vmusave_plugged() != looked_mask)) want_load();
    else wake_worker();
    mutex_unlock(&lock);
}

bool save_failed(void) {
    if(!fail_note) return false;   /* (every frame: no lock for the usual answer) */
    fail_note = false;
    return true;
}

#ifdef DK_AT_CARDS
void save_forget(void) {
    save_finish();
    mutex_lock(&lock);
    card_set = card_off = asking = write_failed = fail_note = looked = false;
    have_want = have_card = load_done = false;
    card_port = card_unit = -1;
    mutex_unlock(&lock);
    defaults(&save);
    save_load();
}
#endif

void save_title(void) {
    mutex_lock(&lock);
    bool same = looked && vmusave_plugged() == looked_mask;
    /* Declined, or no card that would do: nothing to learn until one
       comes or goes. */
    if(card_off && !same) card_off = false;
    bool look = !asking && !worker_on && !card_off && (write_failed || (!card_set && !same));
    if(look && write_failed) have_card = false;   /* what the card missed goes to the next */
    if(look) want_load();
    mutex_unlock(&lock);
}

int save_card_ask(const vmusave_slot_t **slots, const char *const **notes, int *count, int *need) {
    static const char *note_ptr[VMUSAVE_SLOTS];
    mutex_lock(&lock);
    int kind = asking ? ask_kind : -1;
    mutex_unlock(&lock);
    if(kind < 0) return -1;
    for(int i = 0; i < ask_count; i++) note_ptr[i] = ask_notes[i];
    *slots = ask_slots;
    *notes = note_ptr;
    *count = ask_count;
    *need = SAVE_BLOCKS;
    return kind;
}

void save_card_pick(int slot) {
    mutex_lock(&lock);
    if(asking) {
        asking = false;
        if(slot >= 0 && slot < ask_count) {
            card_port = ask_slots[slot].port;
            card_unit = ask_slots[slot].unit;
            card_set = true;
            if(ask_slots[slot].save) {
                /* As a read of it landing (save_tick). */
                loaded = on_card = ask_data[slot];
                have_card = true;
                load_done = true;
            }
            else have_card = false;
            wake_worker();   /* anything changed while asking is written */
        }
        else card_off = true;
    }
    mutex_unlock(&lock);
}
