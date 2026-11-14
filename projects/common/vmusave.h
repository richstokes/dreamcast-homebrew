/* One small save file on a VMU, packaged the way the BIOS memory manager
   lists it: a short and long description, an application id and an
   optional 32x32 icon. Header only. A save looks like:

       static int port = -1, unit = -1;
       maple_device_t *card = vmusave_card(&port, &unit);
       vmu_pkg_t pkg;
       vmusave_header(&pkg, "MYGAME", "My Game", "My Game settings");
       if(!card || vmusave_write(card, "MYGAME", &pkg, &record, sizeof(record)) < 0)
           ... report it ...

   vmusave_read() and vmusave_load() return exactly the bytes written.
   Files saved through fs_vmu read back too, padded with zeros to whole
   blocks.

   This avoids fs_vmu (fs_open on /vmu/a1/NAME) for packaged files. In KOS
   2.3 vmufs_write rounds a file up to whole 512-byte blocks by reading past
   the end of its buffer, and fs_vmu's packages always end 128 bytes into a
   block, so every fs_vmu save carried 384 bytes of stray heap. fs_vmu's
   reads also run past the end of the file by the header's size. Anything
   else handing vmufs_write a package should pad it with vmusave_build. */
#ifndef DC_VMUSAVE_H
#define DC_VMUSAVE_H

#include <dc/maple.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <kos/thread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The VMU to save to: the one at *port and *unit while it is still plugged
   in, otherwise the first VMU found, which they then remember (start both
   at -1). NULL when no VMU is connected. */
static inline maple_device_t *vmusave_card(int *port, int *unit) {
    maple_device_t *card = NULL;

    if(*port >= 0) {
        card = maple_enum_dev(*port, *unit);
        if(card && !(card->info.functions & MAPLE_FUNC_MEMCARD))
            card = NULL;
    }
    if(!card && (card = maple_enum_type(0, MAPLE_FUNC_MEMCARD)) != NULL) {
        *port = card->port;
        *unit = card->unit;
    }
    return card;
}

/* A header with no icon or eyecatch. The file holds 16 characters of
   desc_short, 32 of desc_long and 15 of app_id; longer text is cut there
   (vmu_pkg_build would copy it over the next field). */
static inline void vmusave_header(vmu_pkg_t *pkg, const char *app_id,
                                  const char *desc_short, const char *desc_long) {
    memset(pkg, 0, sizeof(*pkg));
    snprintf(pkg->desc_short, sizeof(pkg->desc_short), "%.16s", desc_short);
    snprintf(pkg->desc_long, sizeof(pkg->desc_long), "%.32s", desc_long);
    snprintf(pkg->app_id, sizeof(pkg->app_id), "%.15s", app_id);
    pkg->eyecatch_type = VMUPKG_EC_NONE;
}

/* A still icon drawn as 32 strings of 32 characters: each character's
   position in keys picks its color from palette (ARGB4444, at most 16).
   A character not in keys draws color 0. pixels must stay valid until
   vmusave_write returns. */
static inline void vmusave_icon(vmu_pkg_t *pkg, uint8_t pixels[512],
                                const char *const art[32], const char *keys,
                                const uint16_t *palette) {
    size_t colors = strlen(keys);
    int x, y;

    if(colors > 16)
        colors = 16;
    memcpy(pkg->icon_pal, palette, colors * sizeof(palette[0]));
    for(y = 0; y < 32; ++y) {
        for(x = 0; x < 32; x += 2) {
            const char *left = art[y][x] ? strchr(keys, art[y][x]) : NULL;
            const char *right = art[y][x + 1] ? strchr(keys, art[y][x + 1]) : NULL;
            int a = left && left - keys < (int)colors ? (int)(left - keys) : 0;
            int b = right && right - keys < (int)colors ? (int)(right - keys) : 0;

            pixels[y * 16 + x / 2] = (uint8_t)(a << 4 | b);
        }
    }
    pkg->icon_cnt = 1;
    pkg->icon_anim_speed = 0;
    pkg->icon_data = pixels;
}

/* pkg's descriptions and icon with data, as a VMU file image padded with
   zeros to whole blocks, in a new buffer (free it). Returns its size, or
   -1. For callers that write it with vmufs_write themselves. */
static inline int vmusave_build(const vmu_pkg_t *pkg, const void *data,
                                size_t size, uint8_t **file) {
    vmu_pkg_t copy = *pkg;
    uint8_t *built, *padded;
    int built_size, file_size;

    copy.data = (const uint8_t *)data;
    copy.data_len = (int)size;
    if(vmu_pkg_build(&copy, &built, &built_size) < 0)
        return -1;
    file_size = (built_size + 511) & ~511;
    padded = (uint8_t *)realloc(built, (size_t)file_size);
    if(!padded) {
        memset(built, 0, (size_t)built_size);
        free(built);
        return -1;
    }
    memset(padded + built_size, 0, (size_t)(file_size - built_size));
    *file = padded;
    return file_size;
}

/* Replace the file name (up to 12 characters) on card with data, packaged
   with pkg's descriptions and icon. Returns 0, or -1 when it could not be
   written (most often a full card). */
static inline int vmusave_write(maple_device_t *card, const char *name,
                                const vmu_pkg_t *pkg, const void *data, size_t size) {
    uint8_t *file;
    int file_size = vmusave_build(pkg, data, size, &file), result;

    if(file_size < 0)
        return -1;
    result = vmufs_write(card, name, file, file_size, VMUFS_OVERWRITE);
    free(file);
    return result < 0 ? -1 : 0;
}

/* Read name from card and find its payload in the file (free *file once
   done with *data). Returns the payload's length, -1 when there is no such
   file, or -2 when it is not a valid package. */
static inline int vmusave_payload(maple_device_t *card, const char *name,
                                  void **file, const uint8_t **data) {
    vmu_pkg_t pkg;
    int file_size;

    if(vmufs_read(card, name, file, &file_size) < 0)
        return -1;
    if(vmu_pkg_parse((uint8_t *)*file, file_size, &pkg) < 0 || pkg.data_len < 0) {
        free(*file);
        return -2;
    }
    *data = pkg.data;
    return pkg.data_len;
}

/* Copy up to size bytes of the payload of name on card into out. Returns
   the bytes copied, -1 when there is no such file, or -2 when it is not a
   valid package. */
static inline int vmusave_read(maple_device_t *card, const char *name,
                               void *out, size_t size) {
    void *file;
    const uint8_t *data;
    int length = vmusave_payload(card, name, &file, &data);

    if(length < 0)
        return length;
    if(length > (int)size)
        length = (int)size;
    memcpy(out, data, (size_t)length);
    free(file);
    return length;
}

/* The whole payload of name on card in a new buffer (free it) with a zero
   byte after it, its length in *size. NULL when there is no such file, it
   is not a valid package or memory ran out. */
static inline uint8_t *vmusave_load(maple_device_t *card, const char *name,
                                    size_t *size) {
    void *file;
    const uint8_t *data;
    int length = vmusave_payload(card, name, &file, &data);
    uint8_t *copy;

    if(length < 0)
        return NULL;
    copy = (uint8_t *)malloc((size_t)length + 1);
    if(copy) {
        memcpy(copy, data, (size_t)length);
        copy[length] = 0;
        *size = (size_t)length;
    }
    free(file);
    return copy;
}

/* ---- Which card ----

   A game with one save looks at every memory card before it settles on
   one, so that a save on the second card is found, two saves are the
   player's choice, and a full first card is not written to in vain:

       vmusave_slot_t slots[VMUSAVE_SLOTS];
       int pick, n = vmusave_survey("MYGAME", slots, VMUSAVE_SLOTS);
       ... clear .save on any slot whose file is not a save of ours ...
       switch(vmusave_decide(slots, n, vmusave_blocks(1, sizeof(record)), &pick)) {
           case VMUSAVE_USE:        slots[pick] is the card
           case VMUSAVE_ASK_WHICH:  two or more hold a save: ask
           case VMUSAVE_ASK_ROOM:   none does and the first is full: ask
           case VMUSAVE_NO_CARD:    nothing to save to
       }
       vmusave_survey_free(slots, n);

   The engine's cardpick.h asks. */

/* One memory card as vmusave_survey found it. */
typedef struct {
    int8_t port, unit;
    int16_t free_blocks;   /* -1 for a card that would not be read */
    int16_t file_blocks;   /* the size of the file of that name on it; 0 for none */
    bool save;             /* it holds that file (a game clears this for one it cannot use) */
    void *file;            /* the file as it is on the card, file_blocks * 512 bytes */
} vmusave_slot_t;
#define VMUSAVE_SLOTS 8   /* four ports, two cards on each */

/* The blocks a save of size bytes takes with icon_frames icons. */
static inline int vmusave_blocks(int icon_frames, size_t size) {
    return (int)((128 + 512 * (size_t)icon_frames + size + 511) / 512);
}

/* One try at a card: its directory, its free blocks and the file. The
   blocks cost a maple frame or more each: 15 and the file's own. Returns
   1; 0 when a block would not read (worth another try); or -1 for a card
   that is not a formatted one. KOS trusts the root block's sizes and the
   file's chain, and a card of noise would have it reading thousands of
   blocks or past its buffers. */
static inline int vmusave_look(maple_device_t *card, const char *name, vmusave_slot_t *slot) {
    vmu_root_t root;
    vmu_dir_t *dir = NULL;
    uint16_t *fat = NULL;
    void *file = NULL;
    int ok = 0, at, entries;

    vmufs_mutex_lock();
    if(vmufs_root_read(card, &root) < 0)
        goto out;
    ok = -1;
    for(int i = 0; i < 16; i++)
        if(root.magic[i] != 0x55)
            goto out;
    entries = root.fat_size * 256;
    if(root.dir_size < 1 || root.dir_size > 64 || root.fat_size < 1 || root.fat_size > 8 || root.blk_cnt > entries)
        goto out;
    ok = 0;
    dir = (vmu_dir_t *)calloc(1, (size_t)vmufs_dir_blocks(&root));
    fat = (uint16_t *)malloc((size_t)vmufs_fat_blocks(&root));
    if(!dir || !fat || vmufs_dir_read(card, &root, dir) < 0 || vmufs_fat_read(card, &root, fat) < 0)
        goto out;
    at = vmufs_dir_find(&root, dir, name);
    if(at >= 0) {
        /* A file whose chain of blocks is broken counts as no save, though
           it has the name: there is nothing to read, and a save written
           over it puts it right. */
        int blocks = dir[at].filesize, block = dir[at].firstblk, left = blocks;

        while(left > 0 && block < entries) {
            block = fat[block];
            left--;
        }
        if(blocks > 0 && blocks <= entries && left == 0 && block == VMU_FAT_LAST_IN_FILE) {
            file = malloc((size_t)blocks * 512);
            if(!file || vmufs_file_read(card, fat, &dir[at], file) < 0) {
                free(file);
                goto out;
            }
            slot->file = file;
            slot->file_blocks = (int16_t)blocks;
            slot->save = true;
        }
    }
    slot->free_blocks = (int16_t)vmufs_fat_free(&root, fat);
    ok = 1;
out:
    vmufs_mutex_unlock();
    free(dir);
    free(fat);
    return ok;
}

/* The memory cards plugged in, a bit each: to tell whether one has come
   or gone since. */
static inline uint32_t vmusave_plugged(void) {
    uint32_t mask = 0;

    for(int port = 0; port < MAPLE_PORT_COUNT; port++)
        for(int unit = 1; unit < MAPLE_UNIT_COUNT; unit++) {
            maple_device_t *card = maple_enum_dev(port, unit);
            if(card && (card->info.functions & MAPLE_FUNC_MEMCARD))
                mask |= 1u << (port * MAPLE_UNIT_COUNT + unit);
        }
    return mask;
}

/* Every memory card plugged in, in port order: its room and its file of
   that name, read. Returns how many (max at most); free the files with
   vmusave_survey_free. A read that fails is tried three times, as on the
   console a block now and then times out, and a card that still will not
   read (or is not formatted) has free_blocks -1: never taken for an empty
   one, which the next save would write over. */
static inline int vmusave_survey(const char *name, vmusave_slot_t *slots, int max) {
    int count = 0;

    for(int port = 0; port < MAPLE_PORT_COUNT; port++)
        for(int unit = 1; unit < MAPLE_UNIT_COUNT && count < max; unit++) {
            maple_device_t *card = maple_enum_dev(port, unit);
            vmusave_slot_t *slot = &slots[count];

            if(!card || !(card->info.functions & MAPLE_FUNC_MEMCARD))
                continue;
            count++;
            memset(slot, 0, sizeof(*slot));
            slot->port = (int8_t)port;
            slot->unit = (int8_t)unit;
            slot->free_blocks = -1;
            for(int tries = 0; tries < 3; tries++) {
                if(tries)
                    thd_sleep(50);
                if(vmusave_look(card, name, slot))
                    break;
            }
        }
    return count;
}

static inline void vmusave_survey_free(vmusave_slot_t *slots, int count) {
    for(int i = 0; i < count; i++) {
        free(slots[i].file);
        slots[i].file = NULL;
    }
}

/* The payload of a slot's file, in place. Returns its length, -1 when the
   card has no such file, or -2 when it is not a valid package. */
static inline int vmusave_slot_payload(const vmusave_slot_t *slot, const uint8_t **data) {
    vmu_pkg_t pkg;

    if(!slot->file)
        return -1;
    if(vmu_pkg_parse((uint8_t *)slot->file, slot->file_blocks * 512, &pkg) < 0 || pkg.data_len < 0)
        return -2;
    *data = pkg.data;
    return pkg.data_len;
}

/* Whether a save of need blocks can be written to the card (over its file
   of that name, if it has one). */
static inline bool vmusave_fits(const vmusave_slot_t *slot, int need) {
    return slot->free_blocks >= 0 && slot->free_blocks + slot->file_blocks >= need;
}

/* The card as maple has it now, or NULL once it has been pulled. */
static inline maple_device_t *vmusave_slot_card(const vmusave_slot_t *slot) {
    maple_device_t *card = maple_enum_dev(slot->port, slot->unit);

    return card && (card->info.functions & MAPLE_FUNC_MEMCARD) ? card : NULL;
}

enum {
    VMUSAVE_NO_CARD,     /* no card that reads */
    VMUSAVE_USE,         /* *pick: the one card with a save, or with none anywhere the first card, which has room */
    VMUSAVE_ASK_WHICH,   /* two or more hold a save */
    VMUSAVE_ASK_ROOM     /* no save anywhere, and the first card (*pick) has no room for one */
};

/* What a survey means for a save of need blocks. */
static inline int vmusave_decide(const vmusave_slot_t *slots, int count, int need, int *pick) {
    int saves = 0, first = -1;

    for(int i = 0; i < count; i++) {
        if(slots[i].free_blocks < 0)
            continue;
        if(first < 0)
            first = i;
        if(slots[i].save && !saves++)
            *pick = i;
    }
    if(first < 0)
        return VMUSAVE_NO_CARD;
    if(saves)
        return saves > 1 ? VMUSAVE_ASK_WHICH : VMUSAVE_USE;
    *pick = first;
    return vmusave_fits(&slots[first], need) ? VMUSAVE_USE : VMUSAVE_ASK_ROOM;
}

#endif
