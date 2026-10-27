/* Hardware self-tests in the spirit of an arcade board's power-on tests:
   main, video and sound RAM patterns, a heap stress test, ROM checksums,
   clocks, the GD-ROM drive and the maple bus. Plus a RAM burn-in loop. */
#include "gfx.h"
#include "kt.h"

#include <dc/cdrom.h>
#include <dc/flashrom.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

float measure_cpu_mhz(void);

/* ---- Main RAM ---- */

#define RAM_KEEP (768 * 1024)   /* left for KOS, threads and the network */

static uint32_t *ram;
static size_t ram_words;

typedef struct {
    uint32_t errors;
    uintptr_t first_addr;
    uint32_t want, got;
} ram_err_t;

static void ram_fail(ram_err_t *e, volatile uint32_t *p, uint32_t want, uint32_t got) {
    if(!e->errors++) {
        e->first_addr = (uintptr_t)p;
        e->want = want;
        e->got = got;
    }
}

static void report_ram(kt_report_t *r, const char *name, const ram_err_t *e,
                       size_t bytes, uint64_t us) {
    char size[24];

    fmt_bytes(size, sizeof(size), bytes);
    if(!e->errors)
        report_add(r, KT_PASS, name, "%s, %lu MB/s", size,
                   us ? (unsigned long)(bytes / us) : 0);
    else
        report_add(r, KT_FAIL, name, "%lu bad @%08lx want %08lx got %08lx",
                   (unsigned long)e->errors, (unsigned long)e->first_addr,
                   (unsigned long)e->want, (unsigned long)e->got);
}

static void step_ram_alloc(kt_report_t *r) {
    char a[24];
    size_t room = mem_above_break();
    size_t size = room > RAM_KEEP + 1024 * 1024 ? (room - RAM_KEEP) & ~0xfffu : 0;

    ram = size ? memalign(32, size) : NULL;
    if(!ram) {
        report_add(r, KT_FAIL, "RAM allocate", "no block of 1 MB or more");
        ram_words = 0;
        return;
    }
    ram_words = size / 4;
    fmt_bytes(a, sizeof(a), size);
    report_add(r, KT_INFO, "RAM allocate", "%s at %08lx of %lu MB", a,
               (unsigned long)(uintptr_t)ram, (unsigned long)(HW_MEMSIZE >> 20));
}

/* One word through the uncached alias, every bit alone. */
static void step_ram_databus(kt_report_t *r) {
    ram_err_t e = { 0 };

    if(!ram) {
        report_add(r, KT_SKIP, "RAM data bus", "no buffer");
        return;
    }
    /* No dirty lines left to land on top of uncached writes. */
    dcache_purge_all();
    volatile uint32_t *p = (volatile uint32_t *)(((uintptr_t)ram & 0x1fffffff) | 0xa0000000);
    for(int bit = 0; bit < 32; bit++) {
        uint32_t v = 1u << bit;
        *p = v;
        if(*p != v) ram_fail(&e, p, v, *p);
        *p = ~v;
        if(*p != ~v) ram_fail(&e, p, ~v, *p);
    }
    if(e.errors)
        report_add(r, KT_FAIL, "RAM data bus", "wrote %08lx, read %08lx",
                   (unsigned long)e.want, (unsigned long)e.got);
    else
        report_add(r, KT_PASS, "RAM data bus", "walking 1s and 0s on 32 bits");
}

/* Power-of-two offsets catch stuck or shorted address lines. */
static void step_ram_addrbus(kt_report_t *r) {
    ram_err_t e = { 0 };
    const uint32_t pat = 0xaaaaaaaa, anti = 0x55555555;

    if(!ram) {
        report_add(r, KT_SKIP, "RAM address bus", "no buffer");
        return;
    }
    dcache_purge_all();
    volatile uint32_t *base = (volatile uint32_t *)(((uintptr_t)ram & 0x1fffffff) | 0xa0000000);
    for(size_t off = 1; off < ram_words; off <<= 1)
        base[off] = pat;
    base[0] = anti;
    for(size_t off = 1; off < ram_words; off <<= 1)
        if(base[off] != pat) ram_fail(&e, &base[off], pat, base[off]);
    base[0] = pat;
    for(size_t test = 1; test < ram_words; test <<= 1) {
        base[test] = anti;
        if(base[0] != pat) ram_fail(&e, &base[0], pat, base[0]);
        for(size_t off = 1; off < ram_words; off <<= 1)
            if(off != test && base[off] != pat) ram_fail(&e, &base[off], pat, base[off]);
        base[test] = pat;
    }
    if(e.errors)
        report_add(r, KT_FAIL, "RAM address bus", "line fault near %08lx",
                   (unsigned long)e.first_addr);
    else
        report_add(r, KT_PASS, "RAM address bus", "%d address lines",
                   32 - __builtin_clz((unsigned)ram_words) + 1);
}

/* Fill through the cache, write it back to RAM, then read it from RAM. */
static void fill_check(ram_err_t *e, uint32_t v) {
    for(size_t i = 0; i < ram_words; i++)
        ram[i] = v;
    dcache_purge_all();
    for(size_t i = 0; i < ram_words; i++)
        if(ram[i] != v) ram_fail(e, &ram[i], v, ram[i]);
}

static void step_ram_solid(kt_report_t *r) {
    ram_err_t e = { 0 };

    if(!ram) {
        report_add(r, KT_SKIP, "RAM 00/FF fill", "no buffer");
        return;
    }
    uint64_t t = now_us();
    fill_check(&e, 0x00000000);
    fill_check(&e, 0xffffffff);
    report_ram(r, "RAM 00/FF fill", &e, ram_words * 8, now_us() - t);
}

static void step_ram_checker(kt_report_t *r) {
    ram_err_t e = { 0 };

    if(!ram) {
        report_add(r, KT_SKIP, "RAM checkerboard", "no buffer");
        return;
    }
    uint64_t t = now_us();
    fill_check(&e, 0x55555555);
    fill_check(&e, 0xaaaaaaaa);
    report_ram(r, "RAM checkerboard", &e, ram_words * 8, now_us() - t);
}

static void step_ram_address(kt_report_t *r) {
    ram_err_t e = { 0 };

    if(!ram) {
        report_add(r, KT_SKIP, "RAM own address", "no buffer");
        return;
    }
    uint64_t t = now_us();
    for(int pass = 0; pass < 2; pass++) {
        uint32_t x = pass ? 0xffffffff : 0;
        for(size_t i = 0; i < ram_words; i++)
            ram[i] = (uint32_t)(uintptr_t)&ram[i] ^ x;
        dcache_purge_all();
        for(size_t i = 0; i < ram_words; i++) {
            uint32_t want = (uint32_t)(uintptr_t)&ram[i] ^ x;
            if(ram[i] != want) ram_fail(&e, &ram[i], want, ram[i]);
        }
    }
    report_ram(r, "RAM own address", &e, ram_words * 8, now_us() - t);
}

/* March C-: up w0; up r0 w1; up r1 w0; down r0 w1; down r1 w0; r0. */
static void step_ram_march(kt_report_t *r) {
    ram_err_t e = { 0 };
    const uint32_t z = 0, o = 0xffffffff;

    if(!ram) {
        report_add(r, KT_SKIP, "RAM March C-", "no buffer");
        return;
    }
    uint64_t t = now_us();
    for(size_t i = 0; i < ram_words; i++) ram[i] = z;
    dcache_purge_all();
    for(size_t i = 0; i < ram_words; i++) { if(ram[i] != z) ram_fail(&e, &ram[i], z, ram[i]); ram[i] = o; }
    dcache_purge_all();
    for(size_t i = 0; i < ram_words; i++) { if(ram[i] != o) ram_fail(&e, &ram[i], o, ram[i]); ram[i] = z; }
    dcache_purge_all();
    for(size_t i = ram_words; i-- > 0;) { if(ram[i] != z) ram_fail(&e, &ram[i], z, ram[i]); ram[i] = o; }
    dcache_purge_all();
    for(size_t i = ram_words; i-- > 0;) { if(ram[i] != o) ram_fail(&e, &ram[i], o, ram[i]); ram[i] = z; }
    dcache_purge_all();
    for(size_t i = 0; i < ram_words; i++) if(ram[i] != z) ram_fail(&e, &ram[i], z, ram[i]);
    report_ram(r, "RAM March C-", &e, ram_words * 4 * 10, now_us() - t);
}

static void step_ram_free(kt_report_t *r) {
    struct mallinfo before = mallinfo();

    free(ram);
    ram = NULL;
    struct mallinfo after = mallinfo();
    report_check(r, after.uordblks < before.uordblks, "RAM release", "%lu KB returned to heap",
                 (unsigned long)((before.uordblks - after.uordblks) >> 10));
}

/* ---- Video RAM ---- */

static void step_vram(kt_report_t *r) {
    size_t avail = pvr_mem_available(), size = avail & ~0xffffu;
    pvr_ptr_t chunk = NULL;
    char a[24];

    while(size >= 0x10000 && !(chunk = pvr_mem_malloc(size)))
        size -= 0x10000;
    if(!chunk) {
        report_add(r, KT_FAIL, "VRAM allocate", "%lu bytes free, none allocatable",
                   (unsigned long)avail);
        return;
    }
    /* Let the frame on screen finish rendering before writing VRAM hard. */
    pvr_wait_render_done();
    volatile uint32_t *p = (volatile uint32_t *)chunk;
    size_t words = size / 4;
    ram_err_t e = { 0 };
    static const uint32_t pats[] = { 0x00000000, 0xffffffff, 0x55555555, 0xaaaaaaaa };
    uint64_t t = now_us();
    for(int k = 0; k < 4; k++) {
        for(size_t i = 0; i < words; i++) p[i] = pats[k];
        for(size_t i = 0; i < words; i++) if(p[i] != pats[k]) ram_fail(&e, &p[i], pats[k], p[i]);
    }
    for(size_t i = 0; i < words; i++) p[i] = (uint32_t)i * 4 + 0x05000000;
    for(size_t i = 0; i < words; i++) {
        uint32_t want = (uint32_t)i * 4 + 0x05000000;
        if(p[i] != want) ram_fail(&e, &p[i], want, p[i]);
    }
    t = now_us() - t;
    pvr_mem_free(chunk);
    fmt_bytes(a, sizeof(a), size);
    report_add(r, KT_INFO, "VRAM area", "%s of 8 MB at %08lx", a, (unsigned long)(uintptr_t)chunk);
    report_ram(r, "VRAM patterns", &e, size * 10, t);
}

/* ---- Sound RAM ---- */

static void step_aram(kt_report_t *r) {
    static uint32_t buf[16384];
    char a[24];

    snd_init();
    uint32_t avail = snd_mem_available();
    uint32_t size = avail & ~0xffffu, chunk = 0;
    while(size >= 0x10000 && !(chunk = snd_mem_malloc(size)))
        size -= 0x10000;
    if(!chunk) {
        report_add(r, KT_FAIL, "Sound RAM allocate", "%lu bytes free", (unsigned long)avail);
        return;
    }
    ram_err_t e = { 0 };
    static const uint32_t pats[] = { 0x00000000, 0xffffffff, 0x55aa55aa, 0xa55aa55a };
    uint64_t t = now_us();
    for(int k = 0; k < 5; k++) {
        for(uint32_t off = 0; off < size; off += sizeof(buf)) {
            for(uint32_t i = 0; i < 16384; i++)
                buf[i] = k < 4 ? pats[k] : chunk + off + i * 4;
            spu_memload(chunk + off, buf, sizeof(buf));
        }
        for(uint32_t off = 0; off < size; off += sizeof(buf)) {
            spu_memread(buf, chunk + off, sizeof(buf));
            for(uint32_t i = 0; i < 16384; i++) {
                uint32_t want = k < 4 ? pats[k] : chunk + off + i * 4;
                if(buf[i] != want)
                    ram_fail(&e, (volatile uint32_t *)(uintptr_t)(0xa0800000 + chunk + off + i * 4),
                             want, buf[i]);
            }
        }
    }
    t = now_us() - t;
    snd_mem_free(chunk);
    fmt_bytes(a, sizeof(a), size);
    report_add(r, KT_INFO, "Sound RAM area", "%s of 2 MB at +%06lx", a, (unsigned long)chunk);
    report_ram(r, "Sound RAM patterns", &e, (size_t)size * 10, t);
}

/* ---- Heap ---- */

#define STRESS_MAX 3000

static void step_malloc(kt_report_t *r) {
    static void *blocks[STRESS_MAX];
    static uint32_t sizes[STRESS_MAX];
    uint32_t seed = 12345;
    int n = 0, bad = 0;
    size_t total = 0;
    struct mallinfo before = mallinfo();

    uint64_t t = now_us();
    while(n < STRESS_MAX && total < 6 * 1024 * 1024) {
        uint32_t sz = 16 + rng_next(&seed) % (rng_next(&seed) & 1 ? 256 : 16384);
        void *p = malloc(sz);
        if(!p)
            break;
        memset(p, (int)(n & 0xff), sz);
        blocks[n] = p;
        sizes[n++] = sz;
        total += sz;
    }
    for(int i = 0; i < n; i++) {
        uint8_t *p = blocks[i];
        if(p[0] != (uint8_t)i || p[sizes[i] - 1] != (uint8_t)i)
            bad++;
    }
    /* Free in a shuffled order. */
    for(int i = n - 1; i > 0; i--) {
        int j = (int)(rng_next(&seed) % (uint32_t)(i + 1));
        void *tp = blocks[i]; blocks[i] = blocks[j]; blocks[j] = tp;
    }
    for(int i = 0; i < n; i++)
        free(blocks[i]);
    t = now_us() - t;
    struct mallinfo after = mallinfo();
    report_check(r, !bad && n > 100, "malloc stress", "%d blocks, %lu KB, %lu ms", n,
                 (unsigned long)(total >> 10), (unsigned long)(t / 1000));
    report_check(r, after.uordblks == before.uordblks, "Heap after free",
                 "%ld bytes leaked", (long)(after.uordblks - before.uordblks));

    int misaligned = 0;
    for(int i = 0; i < 64; i++) {
        size_t al = (size_t)32 << (i % 4);
        void *p = memalign(al, 100 + i * 37);
        if(!p || ((uintptr_t)p & (al - 1))) misaligned++;
        free(p);
    }
    report_check(r, !misaligned, "memalign", "%d of 64 misaligned (32-256)", misaligned);

    uint8_t *p = malloc(64);
    for(int i = 0; i < 64; i++) p[i] = (uint8_t)i;
    p = realloc(p, 100000);
    bool keep = p != NULL;
    for(int i = 0; keep && i < 64; i++) if(p[i] != (uint8_t)i) keep = false;
    p = keep ? realloc(p, 16) : p;
    for(int i = 0; keep && i < 16; i++) if(p[i] != (uint8_t)i) keep = false;
    free(p);
    report_check(r, keep, "realloc", "grow to 100000 and shrink keep the data");

    uint32_t *c = calloc(4096, 4);
    bool zero = c != NULL;
    for(int i = 0; zero && i < 4096; i++) if(c[i]) zero = false;
    free(c);
    report_check(r, zero, "calloc", "16 KB zeroed");
}

/* ---- ROM checksums ---- */

static void step_rom(kt_report_t *r) {
    uint64_t t = now_us();
    uint32_t bios = crc32_buf(0, (const void *)0xa0000000, 2 * 1024 * 1024);
    t = now_us() - t;
    report_add(r, KT_INFO, "BIOS ROM CRC32", "%08lx (2 MB, %lu ms)", (unsigned long)bios,
               (unsigned long)(t / 1000));
    uint32_t flash = crc32_buf(0, (const void *)0xa0200000, 128 * 1024);
    report_add(r, KT_INFO, "Flash ROM CRC32", "%08lx (128 KB)", (unsigned long)flash);

    /* A blank or missing BIOS reads as all ones or zeros. */
    const uint32_t *w = (const uint32_t *)0xa0000000;
    bool blank = true;
    for(int i = 1; i < 256; i++)
        if(w[i] != w[0]) blank = false;
    report_check(r, !blank, "BIOS present", blank ? "ROM reads blank" : "boot vector %08lx",
                 (unsigned long)w[0]);

    static const char *const parts[] = { "factory", "reserved", "block 1", "settings", "block 2" };
    int ok = 0;
    for(int p = 0; p < 5; p++) {
        int start, size;
        if(flashrom_info(p, &start, &size) == 0)
            ok++;
    }
    report_check(r, ok == 5, "Flash partitions", "%d of 5 found (%s to %s)", ok, parts[0], parts[4]);
    int start, size;
    char code[6] = { 0 };
    if(flashrom_info(FLASHROM_PT_SYSTEM, &start, &size) == 0 &&
       flashrom_read(start, code, 5) == 5)
        report_add(r, KT_INFO, "Factory code", "\"%s\" at %05x (%d bytes)", code, start, size);
}

/* ---- Clocks and timers ---- */

static void step_clocks(kt_report_t *r) {
    time_t t0 = rtc_unix_secs();
    struct tm tm;
    gmtime_r(&t0, &tm);
    report_check(r, tm.tm_year + 1900 >= 1999, "RTC date", "%04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    /* Count RTC seconds against the TMU for just over a second. */
    while(rtc_unix_secs() == t0)
        ;
    uint64_t u0 = now_us();
    time_t t1 = rtc_unix_secs();
    while(rtc_unix_secs() == t1)
        ;
    uint64_t tick = now_us() - u0;
    report_check(r, tick > 900000 && tick < 1100000, "RTC vs TMU",
                 "one RTC second = %lu ms of TMU time", (unsigned long)(tick / 1000));

    /* Time 60 whole frames from a vblank edge: counting vblanks in a fixed
       window is a frame out either way. */
    int v0 = pvr_get_vbl_count();
    while(pvr_get_vbl_count() == v0)
        thd_pass();
    v0 = pvr_get_vbl_count();
    uint64_t s0 = now_us();
    while(pvr_get_vbl_count() - v0 < 60)
        thd_pass();
    float hz = 60e6f / (float)(now_us() - s0);
    report_check(r, (hz > 49 && hz < 51) || (hz > 59 && hz < 61), "Vertical blank",
                 "%.3f Hz", (double)hz);

    uint64_t last = now_us();
    int backwards = 0;
    for(int i = 0; i < 100000; i++) {
        uint64_t n = now_us();
        if(n < last) backwards++;
        last = n;
    }
    report_check(r, !backwards, "Timer monotonic", "%d steps back in 100000 reads", backwards);

    uint64_t s = now_us();
    thd_sleep(50);
    uint64_t slept = now_us() - s;
    report_check(r, slept >= 49000 && slept < 80000, "thd_sleep(50)", "%lu.%lu ms",
                 (unsigned long)(slept / 1000), (unsigned long)(slept / 100 % 10));

    float mhz = measure_cpu_mhz();
    if(mhz > 0)
        report_check(r, mhz > 195 && mhz < 205, "CPU clock", "%.2f MHz (perf counter vs TMU)",
                     (double)mhz);
    else
        report_add(r, KT_SKIP, "CPU clock", "no perf counters (Flycast)");
}

/* ---- GD-ROM ---- */

static const char *cd_status_name(int s) {
    switch(s) {
        case CD_STATUS_BUSY: return "busy";
        case CD_STATUS_PAUSED: return "paused";
        case CD_STATUS_STANDBY: return "standby";
        case CD_STATUS_PLAYING: return "playing";
        case CD_STATUS_SEEKING: return "seeking";
        case CD_STATUS_SCANNING: return "scanning";
        case CD_STATUS_OPEN: return "lid open";
        case CD_STATUS_NO_DISC: return "no disc";
        case CD_STATUS_RETRY: return "retry";
        case CD_STATUS_ERROR: return "error";
        default: return "unknown";
    }
}

static const char *cd_type_name(int t) {
    switch(t) {
        case CD_CDDA: return "audio CD / none";
        case CD_CDROM: return "CD-ROM";
        case CD_CDROM_XA: return "CD-ROM XA";
        case CD_CDI: return "CD-i";
        case CD_GDROM: return "GD-ROM";
        default: return "unknown";
    }
}

static void step_gdrom(kt_report_t *r) {
    int status = -1, type = -1;
    static uint8_t sectors[16 * 2048] __attribute__((aligned(32)));

    int rv = cdrom_get_status(&status, &type);
    report_check(r, rv == ERR_OK, "Drive status", "%s, %s", cd_status_name(status),
                 cd_type_name(type));
    if(rv != ERR_OK || status == CD_STATUS_NO_DISC || status == CD_STATUS_OPEN) {
        report_add(r, KT_SKIP, "Disc read", "no disc");
        return;
    }
    cd_toc_t toc;
    if(cdrom_read_toc(&toc, type == CD_GDROM) != ERR_OK &&
       cdrom_read_toc(&toc, false) != ERR_OK) {
        report_add(r, KT_FAIL, "TOC", "read failed");
        return;
    }
    int first = TOC_TRACK(toc.first), last = TOC_TRACK(toc.last);
    report_add(r, KT_PASS, "TOC", "tracks %d-%d, leadout FAD %lu", first, last,
               (unsigned long)TOC_LBA(toc.leadout_sector));
    uint32_t data = cdrom_locate_data_track(&toc);
    if(!data) {
        report_add(r, KT_SKIP, "Sector read", "no data track");
        return;
    }
    uint64_t t = now_us();
    rv = cdrom_read_sectors_ex(sectors, data, 16, true);
    t = now_us() - t;
    report_check(r, rv == ERR_OK, "Sector read", "16 sectors at FAD %lu, %lu KB/s",
                 (unsigned long)data, rv == ERR_OK && t ? (unsigned long)(32768000ull / t) : 0);
}

/* ---- Maple bus ---- */

static void step_maple(kt_report_t *r) {
    int count = 0;

    for(int p = 0; p < MAPLE_PORT_COUNT; p++)
        for(int u = 0; u < MAPLE_UNIT_COUNT; u++) {
            maple_device_t *d = maple_enum_dev(p, u);
            char name[31], label[16];
            if(!d)
                continue;
            count++;
            memcpy(name, d->info.product_name, 30);
            name[30] = 0;
            for(int i = 29; i >= 0 && name[i] == ' '; i--) name[i] = 0;
            snprintf(label, sizeof(label), "%c%d %.11s", 'A' + p, u, name);
            report_add(r, KT_INFO, label, "%s, %u.%umA max", maple_pcaps(d->info.functions),
                       d->info.max_power / 10, d->info.max_power % 10);
        }
    report_check(r, count > 0, "Maple devices", "%d found", count);
}

/* ---- Step lists ---- */

#define RAM_STEPS \
    { "Main RAM: allocate", step_ram_alloc }, \
    { "Main RAM: data bus", step_ram_databus }, \
    { "Main RAM: address bus", step_ram_addrbus }, \
    { "Main RAM: 00/FF fill", step_ram_solid }, \
    { "Main RAM: checkerboard", step_ram_checker }, \
    { "Main RAM: own address", step_ram_address }, \
    { "Main RAM: March C-", step_ram_march }, \
    { "Main RAM: release", step_ram_free }

const kt_step_t selftest_steps[] = {
    RAM_STEPS,
    { "Video RAM", step_vram },
    { "Sound RAM", step_aram },
    { "Heap stress", step_malloc },
    { "BIOS and flash ROM", step_rom },
    { "Clocks and timers", step_clocks },
    { "GD-ROM drive", step_gdrom },
    { "Maple bus", step_maple },
};
const int selftest_step_count = sizeof(selftest_steps) / sizeof(selftest_steps[0]);

static const kt_step_t ram_steps[] = { RAM_STEPS };

static kt_report_t self_report;

/* ---- RAM burn-in ---- */

static uint32_t burn_passes, burn_errors;
static uint64_t burn_started;

static void burn_enter(void) {
    kt_report_t scratch;

    report_begin(&scratch, "burn");
    step_ram_alloc(&scratch);
    burn_passes = burn_errors = 0;
    burn_started = timer_ms_gettime64();
}

static void burn_leave(void) {
    free(ram);
    ram = NULL;
    KT_LOG("RAM burn-in: %lu passes, %lu errors\n", (unsigned long)burn_passes,
           (unsigned long)burn_errors);
}

static void burn_update(const kt_input_t *in) {
    if(in->pressed & (CONT_B | CONT_START)) {
        kt_pop();
        return;
    }
    if(!ram)
        return;
    /* One pattern per frame keeps the screen alive. */
    ram_err_t e = { 0 };
    static const uint32_t pats[] = { 0, 0xffffffff, 0x55555555, 0xaaaaaaaa };
    switch(burn_passes % 5) {
        case 4:
            for(size_t i = 0; i < ram_words; i++) ram[i] = (uint32_t)(uintptr_t)&ram[i] ^ burn_passes;
            dcache_purge_all();
            for(size_t i = 0; i < ram_words; i++) {
                uint32_t want = (uint32_t)(uintptr_t)&ram[i] ^ burn_passes;
                if(ram[i] != want) ram_fail(&e, &ram[i], want, ram[i]);
            }
            break;
        default:
            fill_check(&e, pats[burn_passes % 5]);
    }
    burn_errors += e.errors;
    burn_passes++;
}

static void burn_draw(void) {
    char a[24];
    uint64_t s = (timer_ms_gettime64() - burn_started) / 1000;

    ui_background();
    ui_header("RAM BURN-IN", "LOOPING");
    fmt_bytes(a, sizeof(a), ram_words * 4);
    gfx_textf(40, 70, C_TEXT, "Testing %s of main RAM, one pattern per frame.", a);
    gfx_text_big(40, 110, C_DIM, "PASSES");
    gfx_textf(200, 114, C_HI, "%lu", (unsigned long)burn_passes);
    gfx_text_big(40, 140, C_DIM, "ERRORS");
    gfx_textf(200, 144, burn_errors ? C_FAIL : C_PASS, "%lu", (unsigned long)burn_errors);
    gfx_text_big(40, 170, C_DIM, "TIME");
    gfx_textf(200, 174, C_TEXT, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
              (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    if(!ram)
        gfx_text(40, 220, C_FAIL, "Could not allocate the test buffer.");
    ui_footer("B STOP");
}

static const kt_screen_t burn_screen = {
    "RAM burn-in", burn_enter, burn_update, burn_draw, burn_leave
};

/* ---- Menu ---- */

static const char *const items[] = {
    "RUN ALL SELF-TESTS",
    "MAIN RAM",
    "VIDEO RAM",
    "SOUND RAM",
    "HEAP STRESS",
    "BIOS & FLASH ROM",
    "CLOCKS & TIMERS",
    "GD-ROM DRIVE",
    "MAPLE BUS SCAN",
    "RAM BURN-IN (LOOP)",
};
#define ITEMS ((int)(sizeof(items) / sizeof(items[0])))

static int sel;

static void selftest_update(const kt_input_t *in) {
    if(in->repeat & CONT_DPAD_DOWN) sel = (sel + 1) % ITEMS;
    if(in->repeat & CONT_DPAD_UP) sel = (sel + ITEMS - 1) % ITEMS;
    if(in->pressed & (CONT_B | CONT_START)) {
        kt_pop();
        return;
    }
    if(!(in->pressed & CONT_A))
        return;
    int ram_n = sizeof(ram_steps) / sizeof(ram_steps[0]);
    switch(sel) {
        case 0: runner_start("SELF-TEST", selftest_steps, selftest_step_count, &self_report, NULL); break;
        case 1: runner_start("MAIN RAM TEST", ram_steps, ram_n, &self_report, NULL); break;
        case 9: kt_push(&burn_screen); break;
        default: {
            const kt_step_t *s = &selftest_steps[ram_n + sel - 2];
            runner_start(s->name, s, 1, &self_report, NULL);
        }
    }
}

static void selftest_draw(void) {
    ui_background();
    ui_header("MEMORY & HARDWARE SELF-TEST", NULL);
    ui_menu(110, 56, 420, items, ITEMS, sel, 0, ITEMS);
    ui_footer("UP/DOWN SELECT   A RUN   B BACK");
}

const kt_screen_t selftest_screen = {
    "Self-test", NULL, selftest_update, selftest_draw, NULL
};
