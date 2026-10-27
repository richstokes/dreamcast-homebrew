/* System and I/O benchmarks: small libc calls, the romdisk and ramdisk,
   GD-ROM reads, maple round trips to a VMU, and a sound stream. They only
   read from VMUs and the disc. Check lines are added on a run's first pass
   only; auto mode repeats the steps. */
#include "bench.h"
#include "gfx.h"

#include <dc/cdrom.h>
#include <dc/maple/vmu.h>
#include <dc/sound/stream.h>
#include <dc/vmufs.h>
#include <dirent.h>
#include <fcntl.h>
#include <kos/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Microseconds of the fastest of runs calls to fn, after a warm-up call. */
static uint64_t best_us(void (*fn)(void *), void *arg, int runs) {
    uint64_t best = UINT64_MAX;

    fn(arg);
    for(int i = 0; i < runs; i++) {
        uint64_t t = now_us();
        fn(arg);
        t = now_us() - t;
        if(t < best)
            best = t;
    }
    return best ? best : 1;
}

/* ---- Small libc calls ----
   Called through volatile pointers, or GCC inlines its own copy for the
   constant sizes and newlib is never measured. */

#define CALLS 20000

static void *(*volatile p_memcpy)(void *, const void *, size_t) = memcpy;
static void *(*volatile p_memset)(void *, int, size_t) = memset;
static size_t (*volatile p_strlen)(const char *) = strlen;
static uint8_t mbuf[8192] __attribute__((aligned(32)));
static volatile size_t sink;

typedef struct {
    size_t size;
    int src_off, dst_off;
} copy_t;

static void w_memcpy(void *a) {
    const copy_t *c = a;
    void *(*f)(void *, const void *, size_t) = p_memcpy;
    uint8_t *dst = mbuf + 4096 + c->dst_off;
    const uint8_t *src = mbuf + c->src_off;

    for(int i = 0; i < CALLS; i++)
        f(dst, src, c->size);
}

static void w_memset(void *a) {
    void *(*f)(void *, int, size_t) = p_memset;

    for(int i = 0; i < CALLS; i++)
        f(mbuf + 4096, i, 64);
}

static void w_strlen(void *a) {
    size_t (*f)(const char *) = p_strlen;
    size_t n = 0;

    for(int i = 0; i < CALLS; i++)
        n += f((const char *)mbuf);
    sink = n;
}

static void step_small(kt_report_t *r) {
    static const copy_t copies[4] = { { 16, 0, 0 }, { 64, 0, 0 }, { 256, 0, 0 }, { 64, 1, 3 } };
    static const int ids[4] = { B_MEMCPY16, B_MEMCPY64, B_MEMCPY256, B_MEMCPY64U };

    for(int i = 0; i < 4; i++)
        bench_result(r, ids[i], CALLS / (double)best_us(w_memcpy, (void *)&copies[i], 5));
    bench_result(r, B_MEMSET64, CALLS / (double)best_us(w_memset, NULL, 5));
    memset(mbuf, 'a', 63);
    mbuf[63] = 0;
    bench_result(r, B_STRLEN64, CALLS / (double)best_us(w_strlen, NULL, 5));
}

/* ---- Romdisk and ramdisk ---- */

#define BIG_SIZE (512 * 1024)
#define SMALL_FILES 64
#define CHUNK 32768

static uint8_t iobuf[CHUNK] __attribute__((aligned(32)));
static char small_path[SMALL_FILES][24];
static uint32_t io_crc;
static size_t io_bytes;

static void read_all(const char *path) {
    file_t f = fs_open(path, O_RDONLY);
    ssize_t n;

    io_crc = 0;
    io_bytes = 0;
    if(f < 0)
        return;
    while((n = fs_read(f, iobuf, CHUNK)) > 0) {
        io_crc = crc32_buf(io_crc, iobuf, (size_t)n);
        io_bytes += (size_t)n;
    }
    fs_close(f);
}

/* Timed reads skip the CRC so they measure the filesystem. */
static void w_read(void *a) {
    file_t f = fs_open(a, O_RDONLY);

    if(f < 0)
        return;
    while(fs_read(f, iobuf, CHUNK) > 0)
        ;
    fs_close(f);
}

static void w_open(void *a) {
    for(int i = 0; i < SMALL_FILES; i++) {
        file_t f = fs_open(small_path[i], O_RDONLY);
        if(f >= 0)
            fs_close(f);
    }
}

static void w_stat(void *a) {
    struct stat st;

    for(int i = 0; i < SMALL_FILES; i++)
        fs_stat(small_path[i], &st, 0);
}

static int dir_entries;

static void w_dir(void *a) {
    DIR *d = opendir("/rd/small");
    int n = 0;

    if(!d)
        return;
    struct dirent *e;
    while((e = readdir(d)) != NULL)
        if(strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            n++;
    closedir(d);
    dir_entries = n;
}

static void w_ram_write(void *a) {
    fs_unlink("/ram/kt.bin");
    file_t f = fs_open("/ram/kt.bin", O_WRONLY | O_CREAT | O_TRUNC);
    if(f < 0)
        return;
    for(int i = 0; i < BIG_SIZE / CHUNK; i++)
        fs_write(f, iobuf, CHUNK);
    fs_close(f);
}

static void step_fs(kt_report_t *r) {
    static unsigned seen;
    bool first = bench_first_pass(&seen);
    char want[16] = "";

    for(int i = 0; i < SMALL_FILES; i++)
        snprintf(small_path[i], sizeof(small_path[i]), "/rd/small/f%02d", i);
    file_t f = fs_open("/rd/big.crc", O_RDONLY);
    if(f >= 0) {
        ssize_t n = fs_read(f, want, sizeof(want) - 1);
        want[n > 0 ? n : 0] = 0;
        fs_close(f);
    }
    if(first) {
        read_all("/rd/big.bin");
        char got[16];
        snprintf(got, sizeof(got), "%08lx", (unsigned long)io_crc);
        report_check(r, io_bytes == BIG_SIZE && !strncmp(got, want, 8), "Romdisk data",
                     "%lu bytes, CRC %s (want %.8s)", (unsigned long)io_bytes, got, want);
    }
    bench_result(r, B_ROM_READ, BIG_SIZE / (double)best_us(w_read, "/rd/big.bin", 3));
    bench_result(r, B_ROM_OPEN, SMALL_FILES * 1000.0 / (double)best_us(w_open, NULL, 3));
    bench_result(r, B_ROM_STAT, SMALL_FILES * 1000.0 / (double)best_us(w_stat, NULL, 3));
    bench_result(r, B_ROM_DIR, 1e6 / (double)best_us(w_dir, NULL, 3));
    if(first)
        report_check(r, dir_entries == SMALL_FILES, "Romdisk readdir", "%d of %d entries",
                     dir_entries, SMALL_FILES);

    /* Ramdisk: the romdisk file's last chunk, written out 16 times */
    read_all("/rd/big.bin");
    bench_result(r, B_RAM_WRITE, BIG_SIZE / (double)best_us(w_ram_write, NULL, 3));
    bench_result(r, B_RAM_READ, BIG_SIZE / (double)best_us(w_read, "/ram/kt.bin", 3));
    if(first) {
        uint32_t want_crc = 0;
        for(int i = 0; i < BIG_SIZE / CHUNK; i++)
            want_crc = crc32_buf(want_crc, iobuf, CHUNK);
        read_all("/ram/kt.bin");
        report_check(r, io_bytes == BIG_SIZE && io_crc == want_crc, "Ramdisk data",
                     "%lu bytes read back, CRC %s", (unsigned long)io_bytes,
                     io_crc == want_crc ? "matches" : "differs");
    }
    fs_unlink("/ram/kt.bin");
}

/* ---- GD-ROM ---- */

static uint8_t sectors[32 * 2048] __attribute__((aligned(32)));

static void step_gdrom(kt_report_t *r) {
    static unsigned seen;
    bool first = bench_first_pass(&seen);
    int status = -1, type = -1;
    cd_toc_t toc;

    if(cdrom_get_status(&status, &type) != ERR_OK || status == CD_STATUS_NO_DISC ||
       status == CD_STATUS_OPEN) {
        if(first)
            report_add(r, KT_SKIP, "GD-ROM benchmarks", "no disc");
        return;
    }
    if(cdrom_read_toc(&toc, type == CD_GDROM) != ERR_OK && cdrom_read_toc(&toc, false) != ERR_OK) {
        if(first)
            report_add(r, KT_FAIL, "GD-ROM benchmarks", "TOC read failed");
        return;
    }
    uint32_t start = cdrom_locate_data_track(&toc), end = TOC_LBA(toc.leadout_sector);
    if(!start || end <= start + 16) {
        if(first)
            report_add(r, KT_SKIP, "GD-ROM benchmarks", "data track too short");
        return;
    }
    uint32_t count = end - start > 256 ? 256 : end - start;
    uint64_t t = now_us();
    int bad = 0;
    for(uint32_t s = 0; s < count; s += 32) {
        uint32_t n = count - s > 32 ? 32 : count - s;
        if(cdrom_read_sectors_ex(sectors, start + s, n, true) != ERR_OK)
            bad++;
    }
    t = now_us() - t;
    if(!bad)
        bench_result(r, B_GD_READ, count * 2048.0 / 1024 / (t / 1e6));
    uint32_t seed = 12345;
    t = now_us();
    for(int i = 0; i < 8; i++)
        if(cdrom_read_sectors_ex(sectors, start + rng_next(&seed) % (end - start), 1, true) != ERR_OK)
            bad++;
    t = now_us() - t;
    if(!bad)
        bench_result(r, B_GD_SEEK, t / 8000.0);
    if(first)
        report_check(r, !bad, "GD-ROM reads", "%lu sectors from FAD %lu, %d errors",
                     (unsigned long)count, (unsigned long)start, bad);
}

/* ---- Maple, through a VMU ----
   Maple frames go out on the vblank, so each loop starts on a vblank edge,
   and the result is the median of single timed operations so a read that
   now and then waits an extra frame doesn't move it. */

static void vblank_edge(void) {
    int v = pvr_get_vbl_count();

    while(pvr_get_vbl_count() == v)
        thd_pass();
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Median of n timings, in milliseconds. */
static double median_ms(uint64_t *us, int n) {
    qsort(us, (size_t)n, sizeof(us[0]), cmp_u64);
    return us[n / 2] / 1000.0;
}

static void step_maple(kt_report_t *r) {
    static unsigned seen;
    bool first = bench_first_pass(&seen);
    maple_device_t *card = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    static uint8_t block[512];
    uint64_t us[16];

    if(!card) {
        if(first)
            report_add(r, KT_SKIP, "Maple benchmarks", "no VMU");
        return;
    }
    int bad = 0;
    vblank_edge();
    for(int i = 0; i < 16; i++) {
        uint64_t t = now_us();
        if(vmu_block_read(card, 255, block) != MAPLE_EOK)
            bad++;
        us[i] = now_us() - t;
    }
    if(!bad)
        bench_result(r, B_VMU_BLOCK, median_ms(us, 16));
    if(card->info.functions & MAPLE_FUNC_CLOCK) {
        time_t when;
        vblank_edge();
        for(int i = 0; i < 16; i++) {
            uint64_t t = now_us();
            if(vmu_get_datetime(card, &when) != MAPLE_EOK)
                bad++;
            us[i] = now_us() - t;
        }
        if(!bad)
            bench_result(r, B_VMU_CLOCK, median_ms(us, 16));
    }
    vmu_dir_t *dir = NULL;
    int n = 0;
    vblank_edge();
    for(int i = 0; i < 3; i++) {
        uint64_t t = now_us();
        if(vmufs_readdir(card, &dir, &n) < 0)
            bad++;
        us[i] = now_us() - t;
        free(dir);
        dir = NULL;
    }
    if(!bad)
        bench_result(r, B_VMU_DIR, median_ms(us, 3));
    if(first)
        report_check(r, !bad, "Maple round trips", "VMU %c%d, %d errors", 'A' + card->port,
                     card->unit, bad);
}

/* ---- Sound stream ----
   441 Hz is 100 samples at 44.1 kHz, so 4400 frames loop cleanly; the table
   holds two loops so any request up to 4400 frames reads straight through. */

#define LOOP_FRAMES 4400

static int16_t tone[LOOP_FRAMES * 2 * 2] __attribute__((aligned(32)));
static size_t tone_pos;
static uint64_t cb_from_us, cb_last_us, cb_bytes;
static int cb_calls;

static void *stream_cb(snd_stream_hnd_t h, int bytes, int *got) {
    int frames = bytes / 4;
    uint64_t now = now_us();

    if(frames > LOOP_FRAMES)
        frames = LOOP_FRAMES;
    void *p = &tone[tone_pos * 2];
    tone_pos = (tone_pos + (size_t)frames) % LOOP_FRAMES;
    *got = frames * 4;
    /* The rate counts from the first callback after the start-up fill. */
    if(now >= cb_from_us) {
        if(cb_last_us)
            cb_bytes += (uint64_t)frames * 4;
        else
            cb_from_us = now;
        cb_last_us = now;
    }
    cb_calls++;
    return p;
}

static void step_stream(kt_report_t *r) {
    static bool ready;
    static unsigned seen;
    bool first = bench_first_pass(&seen);

    if(!ready) {
        snd_stream_init();
        for(int i = 0; i < LOOP_FRAMES * 2; i++) {
            int16_t s = (int16_t)(6000 * __builtin_sinf((i % 100) * 6.2831853f / 100));
            tone[i * 2] = tone[i * 2 + 1] = s;
        }
        ready = true;
    }
    snd_stream_hnd_t h = snd_stream_alloc(stream_cb, 16384);
    if(h == SND_STREAM_INVALID) {
        if(first)
            report_add(r, KT_FAIL, "Sound stream", "snd_stream_alloc failed");
        return;
    }
    tone_pos = 0;
    cb_bytes = 0;
    cb_last_us = 0;
    cb_calls = 0;
    uint64_t start = now_us(), poll_us = 0;
    cb_from_us = start + 300000;
    snd_stream_volume(h, 80);
    snd_stream_start(h, 44100, 1);
    while(now_us() - start < 2000000) {
        uint64_t t = now_us();
        snd_stream_poll(h);
        poll_us += now_us() - t;
        thd_sleep(10);
    }
    uint64_t elapsed = now_us() - start;
    snd_stream_stop(h);
    snd_stream_destroy(h);

    bench_result(r, B_STREAM_CPU, poll_us * 100.0 / elapsed);
    if(first) {
        double span = (cb_last_us - cb_from_us) / 1e6;
        double hz = span > 0 ? cb_bytes / 4.0 / span : 0;
        report_check(r, hz > 44100 * 0.97 && hz < 44100 * 1.03, "Sound stream rate",
                     "%.0f Hz from %d callbacks (44100 wanted)", hz, cb_calls);
    }
}

const kt_step_t bench_io_steps[] = {
    { "Small libc calls", step_small },
    { "Romdisk and ramdisk", step_fs },
    { "GD-ROM reads", step_gdrom },
    { "Maple round trips", step_maple },
    { "Sound stream", step_stream },
};
const int bench_io_step_count = sizeof(bench_io_steps) / sizeof(bench_io_steps[0]);
