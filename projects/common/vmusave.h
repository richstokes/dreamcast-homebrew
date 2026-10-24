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
    if(vmu_pkg_parse(*file, file_size, &pkg) < 0 || pkg.data_len < 0) {
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

#endif
