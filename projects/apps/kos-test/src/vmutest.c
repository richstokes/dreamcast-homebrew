/* VMU test: the boot prompt that picks a memory card, then write and read
   tests on a two-block KOSTEST save, which also carries the benchmark
   baseline. Also the VMU LCD banner. */
#include "gfx.h"
#include "kt.h"
#include "../../../common/vmusave.h"

#include <dc/maple/vmu.h>
#include <dc/vmu_fb.h>
#include <kos/dbglog.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CARD_MAX 8

typedef struct {
    int port, unit;
    int free_blocks;
    uint32_t funcs;
    char name[31];
} card_t;

static card_t cards[CARD_MAX];
static int card_count, card_sel;
static int sel_port = -1, sel_unit = -1;
static maple_device_t *seen[CARD_MAX];

int vmu_selected_port(void) { return sel_port; }
int vmu_selected_unit(void) { return sel_unit; }

static maple_device_t *selected_card(void) {
    maple_device_t *d;

    if(sel_port < 0)
        return NULL;
    d = maple_enum_dev(sel_port, sel_unit);
    return d && (d->info.functions & MAPLE_FUNC_MEMCARD) ? d : NULL;
}

static void copy_name(char *out, const maple_device_t *d) {
    memcpy(out, d->info.product_name, 30);
    out[30] = 0;
    for(int i = 29; i >= 0 && out[i] == ' '; i--)
        out[i] = 0;
}

static int list_cards(maple_device_t **out) {
    int n = 0;

    for(int p = 0; p < MAPLE_PORT_COUNT; p++)
        for(int u = 1; u < MAPLE_UNIT_COUNT && n < CARD_MAX; u++) {
            maple_device_t *d = maple_enum_dev(p, u);
            if(d && (d->info.functions & MAPLE_FUNC_MEMCARD))
                out[n++] = d;
        }
    return n;
}

static void scan_cards(void) {
    maple_device_t *devs[CARD_MAX];

    memset(seen, 0, sizeof(seen));
    card_count = list_cards(devs);
    for(int i = 0; i < card_count; i++) {
        card_t *c = &cards[i];
        seen[i] = devs[i];
        c->port = devs[i]->port;
        c->unit = devs[i]->unit;
        c->funcs = devs[i]->info.functions;
        c->free_blocks = vmufs_free_blocks(devs[i]);
        copy_name(c->name, devs[i]);
    }
    if(card_sel > card_count)
        card_sel = card_count;
}

static bool cards_changed(void) {
    maple_device_t *devs[CARD_MAX];
    int n = list_cards(devs);

    if(n != card_count)
        return true;
    for(int i = 0; i < n; i++)
        if(devs[i] != seen[i])
            return true;
    return false;
}

/* ---- The save record ---- */

static const char *const icon_art[32] = {
    "................................",
    "..############################..",
    ".#bbbbbbbbbbbbbbbbbbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbbbbbbbbb#.",
    ".#bbwwbbbwwbbwwwwwwwwwwbbbbbbb#.",
    ".#bbwwbbwwbbbwwwwwwwwwwbbbbbbb#.",
    ".#bbwwbwwbbbbbbbbwwbbbbbbbbbbb#.",
    ".#bbwwwwbbbbbbbbbwwbbbbbbbbbbb#.",
    ".#bbwwwwbbbbbbbbbwwbbbbbbbbbbb#.",
    ".#bbwwbwwbbbbbbbbwwbbbbbbbbbbb#.",
    ".#bbwwbbwwbbbbbbbwwbbbbbbbbbbb#.",
    ".#bbwwbbbwwbbbbbbwwbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbbbgggbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbbgggbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbgggbbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbgggbbbbbb#.",
    ".#bbbbbgggbbbbbbbbbbgggbbbbbbb#.",
    ".#bbbbbbgggbbbbbbbbgggbbbbbbbb#.",
    ".#bbbbbbbgggbbbbbbgggbbbbbbbbb#.",
    ".#bbbbbbbbgggbbbbgggbbbbbbbbbb#.",
    ".#bbbbbbbbbgggbbgggbbbbbbbbbbb#.",
    ".#bbbbbbbbbbggggggbbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbggggbbbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbbggbbbbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbbbbbbbbb#.",
    ".#bbbbbbbbbbbbbbbbbbbbbbbbbbbb#.",
    "..############################..",
    "................................",
    "................................",
};
static const uint16_t icon_pal[] = { 0x0000, 0xffff, 0xf24a, 0xfeee, 0xf4e6 };

static void save_header(vmu_pkg_t *pkg, uint8_t icon[512]) {
    vmusave_header(pkg, KT_SAVE_NAME, KT_TITLE, KT_TITLE " VMU test save");
    vmusave_icon(pkg, icon, icon_art, ".#bwg", icon_pal);
}

void vmu_save_seal(kt_save_t *rec) {
    rec->magic = KT_SAVE_MAGIC;
    rec->version = KT_SAVE_VERSION;
    rec->size = sizeof(*rec);
    rec->crc = crc32_buf(0, rec, offsetof(kt_save_t, crc));
}

bool vmu_save_valid(const kt_save_t *rec) {
    return rec->magic == KT_SAVE_MAGIC && rec->version == KT_SAVE_VERSION &&
           rec->size == sizeof(*rec) &&
           rec->crc == crc32_buf(0, rec, offsetof(kt_save_t, crc));
}

int vmu_save_write(const kt_save_t *rec) {
    maple_device_t *card = selected_card();
    vmu_pkg_t pkg;
    uint8_t icon[512];

    if(!card)
        return -1;
    save_header(&pkg, icon);
    return vmusave_write(card, KT_SAVE_NAME, &pkg, rec, sizeof(*rec));
}

/* 0 when read and valid, -1 no card or file, -2 corrupt. */
int vmu_save_read(kt_save_t *rec) {
    maple_device_t *card = selected_card();

    if(!card)
        return -1;
    int n = vmusave_read(card, KT_SAVE_NAME, rec, sizeof(*rec));
    if(n == -1)
        return -1;
    if(n != (int)sizeof(*rec) || !vmu_save_valid(rec))
        return -2;
    return 0;
}

/* ---- Test steps ---- */

static kt_save_t old_rec, new_rec;
static bool old_found, wrote;
static int free_before;

static void step_card(kt_report_t *r) {
    maple_device_t *card = selected_card();
    vmu_root_t root;
    char name[31];

    if(!card) {
        report_add(r, KT_FAIL, "Card present", "VMU %c%d was removed", 'A' + sel_port, sel_unit);
        return;
    }
    copy_name(name, card);
    report_add(r, KT_INFO, "Device", "%c%d %s", 'A' + sel_port, sel_unit, name);
    report_add(r, KT_INFO, "Functions", "%s", maple_pcaps(card->info.functions));
    uint64_t t = now_us();
    int rv = vmufs_root_read(card, &root);
    t = now_us() - t;
    bool magic = true;
    for(int i = 0; i < 16; i++)
        if(root.magic[i] != 0x55)
            magic = false;
    report_check(r, rv == 0 && magic, "Root block", "%s, %d user blocks, %lu ms",
                 rv ? "read failed" : magic ? "formatted" : "bad magic",
                 rv ? 0 : root.blk_cnt, (unsigned long)(t / 1000));
    if(rv == 0)
        report_add(r, KT_INFO, "Layout", "FAT @%u x%u, dir @%u x%u",
                   root.fat_loc, root.fat_size, root.dir_loc, root.dir_size);
    free_before = vmufs_free_blocks(card);
    report_check(r, free_before >= 0, "Free blocks", "%d", free_before);
}

static void step_dir(kt_report_t *r) {
    maple_device_t *card = selected_card();
    vmu_dir_t *dir = NULL;
    int n = 0, files = 0;

    if(!card) {
        report_add(r, KT_SKIP, "Directory", "no card");
        return;
    }
    uint64_t t = now_us();
    int rv = vmufs_readdir(card, &dir, &n);
    t = now_us() - t;
    old_found = false;
    for(int i = 0; rv == 0 && i < n; i++) {
        if(!dir[i].filetype)
            continue;
        files++;
        if(!strncmp(dir[i].filename, KT_SAVE_NAME, sizeof(dir[i].filename))) {
            old_found = true;
            report_check(r, dir[i].filesize == 2, "Save entry",
                         "%s, %u blocks, first block %u", KT_SAVE_NAME,
                         dir[i].filesize, dir[i].firstblk);
        }
    }
    free(dir);
    report_check(r, rv == 0, "Directory read", "%d files, %lu ms", files,
                 (unsigned long)(t / 1000));
}

static void step_old(kt_report_t *r) {
    memset(&old_rec, 0, sizeof(old_rec));
    if(!old_found) {
        report_add(r, KT_INFO, "Existing save", "none, a new one will be made");
        return;
    }
    uint64_t t = now_us();
    int rv = vmu_save_read(&old_rec);
    t = now_us() - t;
    if(rv == 0) {
        report_add(r, KT_PASS, "Existing save", "valid, run %lu, %lu ms",
                   (unsigned long)old_rec.runs, (unsigned long)(t / 1000));
        if(old_rec.bench_count) {
            bench_load_baseline(old_rec.bench, (int)old_rec.bench_count,
                                old_rec.kos, old_rec.build);
            report_add(r, KT_INFO, "Bench baseline", "KOS %.15s, %.23s", old_rec.kos,
                       old_rec.build);
        }
    }
    else {
        report_add(r, KT_FAIL, "Existing save", rv == -2 ? "corrupt (CRC or size)" : "read failed");
        memset(&old_rec, 0, sizeof(old_rec));
    }
}

static void step_write(kt_report_t *r) {
    wrote = false;
    if(!selected_card()) {
        report_add(r, KT_SKIP, "Write", "no card");
        return;
    }
    if(!old_found && free_before < 2) {
        report_add(r, KT_FAIL, "Write", "card full: %d blocks free, 2 needed", free_before);
        return;
    }
    new_rec = vmu_save_valid(&old_rec) ? old_rec : (kt_save_t){ 0 };
    new_rec.runs++;
    /* A different pattern every run, so the read-back proves a fresh write. */
    uint32_t seed = new_rec.runs * 2654435761u;
    for(size_t i = 0; i < sizeof(new_rec.fill); i++)
        new_rec.fill[i] = (uint8_t)rng_next(&seed);
    vmu_save_seal(&new_rec);

    uint64_t t = now_us();
    int rv = vmu_save_write(&new_rec);
    t = now_us() - t;
    wrote = rv == 0;
    report_check(r, rv == 0, "Write", "%s, 1024 bytes in %lu ms",
                 rv == 0 ? "ok" : "failed", (unsigned long)(t / 1000));
}

static void step_readback(kt_report_t *r) {
    kt_save_t back;

    if(!selected_card() || !wrote) {
        report_add(r, KT_SKIP, "Read back", "nothing written");
        return;
    }
    memset(&back, 0, sizeof(back));
    uint64_t t = now_us();
    int rv = vmu_save_read(&back);
    t = now_us() - t;
    int diff = -1;
    if(rv == 0)
        for(size_t i = 0; i < sizeof(back); i++)
            if(((uint8_t *)&back)[i] != ((uint8_t *)&new_rec)[i]) {
                diff = (int)i;
                break;
            }
    if(rv == 0 && diff < 0)
        report_add(r, KT_PASS, "Read back", "matches, run %lu, %lu ms",
                   (unsigned long)back.runs, (unsigned long)(t / 1000));
    else if(rv == 0)
        report_add(r, KT_FAIL, "Read back", "differs at byte %d", diff);
    else
        report_add(r, KT_FAIL, "Read back", rv == -2 ? "corrupt" : "read failed");
}

static void step_package(kt_report_t *r) {
    maple_device_t *card = selected_card();
    void *file = NULL;
    int size = 0;
    vmu_pkg_t pkg;

    if(!card || !wrote) {
        report_add(r, KT_SKIP, "Package", "nothing written");
        return;
    }
    if(vmufs_read(card, KT_SAVE_NAME, &file, &size) < 0) {
        report_add(r, KT_FAIL, "Package", "file read failed");
        return;
    }
    int rv = vmu_pkg_parse(file, size, &pkg);
    report_check(r, size == 1024, "File size", "%d bytes (2 blocks)", size);
    report_check(r, rv == 0 && !strncmp(pkg.app_id, KT_SAVE_NAME, 7) && pkg.icon_cnt == 1,
                 "Package header", "%s, app id %.16s, %d icon",
                 rv == 0 ? "CRC ok" : "bad CRC", rv == 0 ? pkg.app_id : "-",
                 rv == 0 ? pkg.icon_cnt : 0);
    free(file);
    int free_after = vmufs_free_blocks(card);
    int expect = old_found ? free_before : free_before - 2;
    report_check(r, free_after == expect, "Free after", "%d blocks (expected %d)",
                 free_after, expect);
}

/* The selected card, waiting up to 2 s for it: on the console a card that
   has just done a run of flash writes can miss enough hot-plug polls to be
   detached, then attached again. */
static maple_device_t *wait_card(int *gone_ms) {
    uint64_t start = timer_ms_gettime64();
    maple_device_t *card;

    while(!(card = selected_card()) && timer_ms_gettime64() - start < 2000)
        thd_sleep(20);
    *gone_ms = (int)(timer_ms_gettime64() - start);
    return card;
}

static void step_blocks(kt_report_t *r) {
    static uint8_t a[512], b[512];
    int gone;
    maple_device_t *card = wait_card(&gone);

    if(!card) {
        report_add(r, KT_FAIL, "Raw block read", "%c%d gone from the bus for over 2 s",
                   'A' + sel_port, sel_unit);
        return;
    }
    if(gone >= 20)
        report_add(r, KT_INFO, "Card dropped off", "%c%d detached for %d ms after the writes",
                   'A' + sel_port, sel_unit, gone);
    uint64_t t = now_us();
    int ok = 0;
    for(int i = 0; i < 4; i++) {
        if(vmu_block_read(card, 255, i & 1 ? b : a) == MAPLE_EOK)
            ok++;
    }
    t = now_us() - t;
    report_check(r, ok == 4 && !memcmp(a, b, 512), "Raw block read",
                 "root x4, %s, %lu.%lu ms/block", ok == 4 ? "consistent" : "errors",
                 (unsigned long)(t / 4000), (unsigned long)(t / 400 % 10));
}

static void step_extras(kt_report_t *r) {
    maple_device_t *card = selected_card();

    if(!card)
        return;
    if(card->info.functions & MAPLE_FUNC_CLOCK) {
        time_t when = 0;
        if(vmu_get_datetime(card, &when) == MAPLE_EOK) {
            struct tm tm;
            gmtime_r(&when, &tm);
            report_add(r, KT_INFO, "VMU clock", "%04d-%02d-%02d %02d:%02d:%02d",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
        }
        else
            report_add(r, KT_INFO, "VMU clock", "no answer");
    }
    if(card->info.functions & MAPLE_FUNC_LCD) {
        vmufb_t fb;
        vmufb_clear(&fb);
        vmufb_print_string(&fb, NULL, "VMU TEST\nWRITE OK\nREAD OK");
        vmufb_present(&fb, card);
        report_add(r, KT_INFO, "LCD", "test text sent to the screen");
    }
}

/* vmufs_write() rounds a short file up to whole blocks; stock KOS 2.3 fills
   the rest from past the caller's buffer, the fix (KallistiOS#1574) pads
   with zeros. Each write is a temporary 2-block file, deleted after. */
#define PAD_NAME "KOSTEST.PAD"

static int stray_bytes(maple_device_t *card, const uint8_t *src, int size, int *file_size,
                       bool *kept) {
    void *file = NULL;
    int got = 0, stray = 0;

    if(vmufs_write(card, PAD_NAME, (void *)src, size, VMUFS_OVERWRITE) < 0)
        return -1;
    if(vmufs_read(card, PAD_NAME, &file, &got) < 0) {
        vmufs_delete(card, PAD_NAME);
        return -2;
    }
    *kept = !memcmp(file, src, (size_t)size);
    for(int i = size; i < got; i++)
        if(((uint8_t *)file)[i])
            stray++;
    *file_size = got;
    free(file);
    vmufs_delete(card, PAD_NAME);
    return stray;
}

static void step_padding(kt_report_t *r) {
    maple_device_t *card = selected_card();
    static uint8_t buf[1024];
    int size = 0;
    bool kept = false;
    int level = dbglog_level;

    if(!card) {
        report_add(r, KT_SKIP, "vmufs_write padding", "no card");
        return;
    }
    int before = vmufs_free_blocks(card);
    if(before < 2) {
        report_add(r, KT_SKIP, "vmufs_write padding", "needs 2 free blocks, %d free", before);
        return;
    }
    for(int i = 0; i < 600; i++)
        buf[i] = (uint8_t)(i * 7 + 1);
    memset(buf + 600, 'Z', sizeof(buf) - 600);
    dbglog_set_level(DBG_ERROR);       /* KOS warns about each padded write */
    int stray = stray_bytes(card, buf, 600, &size, &kept);
    if(stray < 0)
        report_add(r, KT_FAIL, "vmufs_write padding", stray == -1 ? "600-byte write failed" :
                   "read back failed");
    else if(stray)
        report_add(r, KT_FAIL, "vmufs_write padding", "%d of %d pad bytes from past the buffer "
                   "(#1574)", stray, size - 600);
    else
        report_check(r, kept && size == 1024, "vmufs_write padding", "600 bytes %s, %d padding "
                     "bytes zero", kept ? "kept" : "CHANGED", size - 600);

    /* A zero-byte write still makes a block */
    memset(buf, 'Z', 512);
    stray = stray_bytes(card, buf, 0, &size, &kept);
    dbglog_set_level(level);
    if(stray < 0)
        report_add(r, KT_INFO, "vmufs_write 0 bytes", "refused (%s)",
                   stray == -1 ? "write" : "read back");
    else if(stray)
        report_add(r, KT_FAIL, "vmufs_write 0 bytes", "%d of %d bytes from the buffer (#1574)",
                   stray, size);
    else
        report_add(r, KT_PASS, "vmufs_write 0 bytes", "%d-byte file, all zero", size);
    int after = vmufs_free_blocks(card);
    report_check(r, after == before, "Temporary file", "deleted, %d blocks free (%d before)", after,
                 before);
}

static const kt_step_t vmu_steps[] = {
    { "Card and root block", step_card },
    { "Directory", step_dir },
    { "Existing save", step_old },
    { "Write test save", step_write },
    { "Read back", step_readback },
    { "Package check", step_package },
    { "vmufs_write padding", step_padding },
    { "Raw block reads", step_blocks },
    { "Clock and LCD", step_extras },
};

/* For auto mode with AUTO_VMU=1: the same tests on the card with the most
   room, since the first one may well be full. */
static void step_pick(kt_report_t *r) {
    maple_device_t *devs[CARD_MAX], *best = NULL;
    int n = list_cards(devs), most = -1;

    for(int i = 0; i < n; i++) {
        int f = vmufs_free_blocks(devs[i]);
        if(f > most) {
            most = f;
            best = devs[i];
        }
    }
    if(!best) {
        report_add(r, KT_SKIP, "VMU", "none connected");
        return;
    }
    sel_port = best->port;
    sel_unit = best->unit;
    report_add(r, KT_INFO, "VMU", "testing %c%d, %d blocks free of %d cards", 'A' + sel_port,
               sel_unit, most, n);
}

const kt_step_t vmu_auto_steps[] = {
    { "Pick a VMU", step_pick },
    { "Card and root block", step_card },
    { "Directory", step_dir },
    { "Existing save", step_old },
    { "Write test save", step_write },
    { "Read back", step_readback },
    { "Package check", step_package },
    { "vmufs_write padding", step_padding },
    { "Raw block reads", step_blocks },
};
const int vmu_auto_step_count = sizeof(vmu_auto_steps) / sizeof(vmu_auto_steps[0]);

static kt_report_t vmu_report;

static void start_test(void) {
    sel_port = cards[card_sel].port;
    sel_unit = cards[card_sel].unit;
    KT_LOG("VMU test on %c%d\n", 'A' + sel_port, sel_unit);
    kt_pop();
    runner_start("VMU TEST", vmu_steps, sizeof(vmu_steps) / sizeof(vmu_steps[0]),
                 &vmu_report, NULL);
}

/* ---- The prompt ---- */

static int prompt_frames;

static void prompt_enter(void) {
    prompt_frames = 0;
    scan_cards();
    card_sel = 0;
    if(sel_port >= 0)
        for(int i = 0; i < card_count; i++)
            if(cards[i].port == sel_port && cards[i].unit == sel_unit)
                card_sel = i;
}

static void prompt_update(const kt_input_t *in) {
    int items = card_count + 1;

    prompt_frames++;
    if(prompt_frames % 30 == 0 && cards_changed())
        scan_cards();
#ifdef KT_AUTOTEST
    if(autotest_active() && prompt_frames >= 60) {
        if(card_count)
            start_test();
        else
            kt_pop();
        return;
    }
#endif
    if(in->repeat & CONT_DPAD_DOWN) card_sel = (card_sel + 1) % items;
    if(in->repeat & CONT_DPAD_UP) card_sel = (card_sel + items - 1) % items;
    if(in->pressed & CONT_Y)
        scan_cards();
    if(in->pressed & CONT_B) {
        kt_pop();
        return;
    }
    if(in->pressed & (CONT_A | CONT_START)) {
        if(card_sel < card_count)
            start_test();
        else
            kt_pop();
    }
}

static void prompt_draw(void) {
    ui_background();
    ui_header(KT_TITLE " " KT_VERSION, "VMU TEST");
    gfx_text_big(40, 60, C_TEXT, "TEST A VMU?");
    gfx_text(40, 92, C_DIM, "Writes a 2-block KOSTEST save (reused on later runs), reads");
    gfx_text(40, 110, C_DIM, "it back and checks it. The save also keeps your benchmark");
    gfx_text(40, 128, C_DIM, "baseline. Other saves on the card are not touched.");

    float y = 168;
    for(int i = 0; i <= card_count; i++) {
        bool on = i == card_sel;
        if(on) {
            gfx_grad(40, y - 3, 560, 26, 0xff2d66b8, 0xff1c4888);
            gfx_box(40, y - 3, 560, 26, 1, C_HI);
        }
        if(i < card_count) {
            const card_t *c = &cards[i];
            char line[80];
            snprintf(line, sizeof(line), "%c%d  %-20.20s %3d FREE%s%s", 'A' + c->port,
                     c->unit, c->name, c->free_blocks < 0 ? 0 : c->free_blocks,
                     c->funcs & MAPLE_FUNC_LCD ? "  LCD" : "",
                     c->funcs & MAPLE_FUNC_CLOCK ? "  CLOCK" : "");
            gfx_text_big(52, y, on ? C_HI : C_TEXT, line);
        }
        else
            gfx_text_big(52, y, on ? C_HI : C_TEXT, "SKIP VMU TEST");
        y += 30;
    }
    if(!card_count)
        gfx_text(52, y + 8, C_WARN, "No VMU found. Plug one into a controller, then press Y.");
    ui_footer("UP/DOWN SELECT   A TEST   B SKIP   Y RESCAN");
}

const kt_screen_t vmu_screen = {
    "VMU test", prompt_enter, prompt_update, prompt_draw, NULL
};

void vmu_prompt_start(void) {
    kt_push(&vmu_screen);
}

/* ---- LCD banner ---- */

void vmu_lcd_banner(const char *line1, const char *line2) {
    vmufb_t fb;
    char text[64];
    maple_device_t *d;

    vmufb_clear(&fb);
    if(line1) {
        snprintf(text, sizeof(text), "%s\n%.16s", line1, line2 ? line2 : "");
        vmufb_print_string(&fb, NULL, text);
    }
    for(int n = 0; (d = maple_enum_type(n, MAPLE_FUNC_LCD)) != NULL; n++)
        vmufb_present(&fb, d);
}
