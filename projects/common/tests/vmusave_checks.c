/* Exercise vmusave.h on SH-4 against Flycast's VMUs in A1 and A2: card
   choice, headers, icons, read-back, overwrite and a full card. The runner
   gives Flycast blank cards and checks the images afterwards. */
#include <kos.h>
#include <dc/fs_vmu.h>
#include <dc/vmufs.h>
#include <assert.h>
#include <stdlib.h>
#include "../vmusave.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

static const char keys[] = ".ab";
static const uint16_t palette[3] = {0x0000, 0xff00, 0xf0f0};
static char rows[32][33];
static const char *art[32];
static uint8_t pixels[512];
static uint8_t record[700], small[100], big[120 * 1024], back[2048];

/* The pattern the runner checks too: column and row pick a, b or blank,
   with one character outside keys, which must draw color 0. */
static void draw_art(void) {
    for(int y = 0; y < 32; ++y) {
        for(int x = 0; x < 32; ++x)
            rows[y][x] = x == 31 && y == 0 ? 'z' : keys[(x + y) % 3];
        rows[y][32] = '\0';
        art[y] = rows[y];
    }
}

static int expected_pixel(int x, int y) {
    return x == 31 && y == 0 ? 0 : (x + y) % 3;
}

/* vmu_pkg_build pads the descriptions with spaces. */
static int same_text(const char *field, const char *want) {
    size_t length = strlen(want);

    if(strncmp(field, want, length))
        return 0;
    for(field += length; *field; ++field)
        if(*field != ' ')
            return 0;
    return 1;
}

static void parse(maple_device_t *card, const char *name, vmu_pkg_t *pkg,
                  void **raw, int *size) {
    assert(vmufs_read(card, name, raw, size) == 0);
    assert(vmu_pkg_parse(*raw, *size, pkg) == 0);
}

int main(void) {
    maple_device_t *a1, *a2;
    int port = -1, unit = -1, size, free_before;
    vmu_pkg_t pkg;
    void *raw;
    uint8_t *loaded;
    size_t loaded_size;
    file_t file;
    uint64_t until = timer_ms_gettime64() + 3000;

    while(!maple_enum_dev(0, 2) && timer_ms_gettime64() < until)
        thd_sleep(50);
    a1 = maple_enum_dev(0, 1);
    a2 = maple_enum_dev(0, 2);
    assert(a1 && (a1->info.functions & MAPLE_FUNC_MEMCARD));
    assert(a2 && (a2->info.functions & MAPLE_FUNC_MEMCARD));

    /* The first card, then whichever is remembered while it is plugged in. */
    assert(vmusave_card(&port, &unit) == a1 && port == 0 && unit == 1);
    port = 0;
    unit = 2;
    assert(vmusave_card(&port, &unit) == a2 && port == 0 && unit == 2);
    port = 3;
    unit = 1;
    assert(vmusave_card(&port, &unit) == a1 && port == 0 && unit == 1);
    /* A controller is not a card. */
    port = 0;
    unit = 0;
    assert(vmusave_card(&port, &unit) == a1 && unit == 1);

    assert(vmusave_read(a1, "VMUSAVE_T", back, sizeof(back)) == -1);
    assert(!vmusave_load(a1, "VMUSAVE_T", &loaded_size));
    /* A file that is not a package is told apart from a missing one. */
    memset(big, 0x5a, 512);
    assert(vmufs_write(a1, "VMUSAVE_BAD", big, 512, VMUFS_OVERWRITE) == 0);
    assert(vmusave_read(a1, "VMUSAVE_BAD", back, sizeof(back)) == -2);
    assert(!vmusave_load(a1, "VMUSAVE_BAD", &loaded_size));
    assert(vmufs_delete(a1, "VMUSAVE_BAD") == 0);
    memset(big, 0, 512);

    /* Two blocks of payload with an icon: exactly what was written comes
       back, and the file is padded with zeros, not whatever followed it. */
    for(size_t i = 0; i < sizeof(record); ++i)
        record[i] = (uint8_t)(i * 7 + 1);
    draw_art();
    vmusave_header(&pkg, "DCSAVETEST", "Save test", "Shared vmusave.h check");
    vmusave_icon(&pkg, pixels, art, keys, palette);
    assert(vmusave_write(a1, "VMUSAVE_T", &pkg, record, sizeof(record)) == 0);
    assert(vmusave_read(a1, "VMUSAVE_T", back, sizeof(back)) == (int)sizeof(record));
    assert(!memcmp(back, record, sizeof(record)));
    loaded = vmusave_load(a1, "VMUSAVE_T", &loaded_size);
    assert(loaded && loaded_size == sizeof(record) && !memcmp(loaded, record, sizeof(record)));
    assert(loaded[sizeof(record)] == 0);
    free(loaded);

    memset(&pkg, 0, sizeof(pkg));
    parse(a1, "VMUSAVE_T", &pkg, &raw, &size);
    assert(size == 1536);
    for(int i = 128 + 512 + (int)sizeof(record); i < size; ++i)
        assert(((uint8_t *)raw)[i] == 0);
    assert(same_text(pkg.desc_short, "Save test") && same_text(pkg.desc_long, "Shared vmusave.h check"));
    assert(same_text(pkg.app_id, "DCSAVETEST") && pkg.eyecatch_type == VMUPKG_EC_NONE);
    assert(pkg.icon_cnt == 1 && !memcmp(pkg.icon_pal, palette, sizeof(palette)));
    assert(pkg.icon_pal[3] == 0 && pkg.icon_pal[15] == 0);
    for(int y = 0; y < 32; ++y)
        for(int x = 0; x < 32; ++x) {
            uint8_t byte = pkg.icon_data[y * 16 + x / 2];
            assert(((x & 1) ? byte & 15 : byte >> 4) == expected_pixel(x, y));
        }
    assert(pkg.data_len == (int)sizeof(record) && !memcmp(pkg.data, record, sizeof(record)));
    free(raw);

    /* fs_vmu still reads it, as the apps' older loaders did. */
    file = fs_open("/vmu/a1/VMUSAVE_T", O_RDONLY);
    assert(file != FILEHND_INVALID);
    assert(fs_read(file, back, sizeof(record)) == (ssize_t)sizeof(record));
    fs_close(file);
    assert(!memcmp(back, record, sizeof(record)));

    /* A save made the old way, through fs_vmu, reads back padded with zeros
       to the block. */
    file = fs_open("/vmu/a1/VMUSAVE_OLD", O_WRONLY | O_TRUNC);
    assert(file != FILEHND_INVALID);
    assert(fs_write(file, record, 100) == 100);
    assert(fs_vmu_set_header(file, &pkg) == 0);
    assert(fs_close(file) == 0);
    memset(back, 0xee, sizeof(back));
    assert(vmusave_read(a1, "VMUSAVE_OLD", back, sizeof(back)) == 512);
    assert(!memcmp(back, record, 100));
    for(int i = 100; i < 512; ++i)
        assert(back[i] == 0);
    assert(vmufs_delete(a1, "VMUSAVE_OLD") == 0);

    /* Overwriting with less data shrinks the file. */
    free_before = vmufs_free_blocks(a1);
    for(size_t i = 0; i < sizeof(small); ++i)
        small[i] = (uint8_t)(200 - i);
    vmusave_header(&pkg, "DCSAVETEST", "Save test", "Shared vmusave.h check");
    vmusave_icon(&pkg, pixels, art, keys, palette);
    assert(vmusave_write(a1, "VMUSAVE_T", &pkg, small, sizeof(small)) == 0);
    assert(vmufs_free_blocks(a1) == free_before + 1);
    assert(vmusave_read(a1, "VMUSAVE_T", back, sizeof(back)) == (int)sizeof(small));
    assert(!memcmp(back, small, sizeof(small)));
    /* A short buffer gets the start of the payload. */
    assert(vmusave_read(a1, "VMUSAVE_T", back, 10) == 10 && !memcmp(back, small, 10));

    /* No icon, on the second card. */
    vmusave_header(&pkg, "DCSAVETEST", "No icon", "Second card");
    assert(vmusave_write(a2, "VMUSAVE_2", &pkg, small, sizeof(small)) == 0);
    parse(a2, "VMUSAVE_2", &pkg, &raw, &size);
    assert(pkg.icon_cnt == 0 && same_text(pkg.desc_short, "No icon") && size == 512);
    free(raw);

    /* Text longer than the file's fields is cut, not run into the next. */
    vmusave_header(&pkg, "AN_APP_ID_THAT_IS_LONG", "A short desc too long",
                   "A long description, longer than it can be");
    assert(vmusave_write(a2, "VMUSAVE_3", &pkg, small, sizeof(small)) == 0);
    parse(a2, "VMUSAVE_3", &pkg, &raw, &size);
    assert(same_text(pkg.desc_short, "A short desc too") && same_text(pkg.app_id, "AN_APP_ID_THAT_"));
    assert(same_text(pkg.desc_long, "A long description, longer than"));
    assert(pkg.icon_cnt == 0 && pkg.data_len == (int)sizeof(small));
    free(raw);
    assert(vmufs_delete(a2, "VMUSAVE_3") == 0);
    vmusave_header(&pkg, "DCSAVETEST", "A short desc too long", "");
    assert(vmusave_write(a2, "VMUSAVE_3", &pkg, small, sizeof(small)) == 0);
    parse(a2, "VMUSAVE_3", &pkg, &raw, &size);
    assert(same_text(pkg.desc_short, "A short desc too") && same_text(pkg.desc_long, ""));
    free(raw);
    assert(vmufs_delete(a2, "VMUSAVE_3") == 0);

    /* More than a card holds fails and leaves the other files alone. */
    free_before = vmufs_free_blocks(a1);
    vmusave_header(&pkg, "DCSAVETEST", "Too big", "Does not fit");
    assert(vmusave_write(a1, "VMUSAVE_BIG", &pkg, big, sizeof(big)) < 0);
    assert(vmufs_free_blocks(a1) == free_before);
    assert(vmusave_read(a1, "VMUSAVE_T", back, sizeof(back)) == (int)sizeof(small));
    assert(!memcmp(back, small, sizeof(small)));

    printf("VMUSAVE CHECKS PASSED\n");
    while(1)
        thd_sleep(1000);
}
