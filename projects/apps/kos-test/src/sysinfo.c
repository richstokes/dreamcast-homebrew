/* System information: SDK and build, CPU, memory, video and console
   settings. Gathered once on entry; the clock line updates live. */
#include "gfx.h"
#include "kt.h"

#include <dc/flashrom.h>
#include <dc/fs_dcload.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/banner.h>
#include <malloc.h>
#include <newlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ROWS_MAX 40

typedef struct {
    char key[24];
    char value[60];
} row_t;

static row_t rows[ROWS_MAX];
static int row_count;
static float cpu_mhz;


static void add(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(const char *key, const char *fmt, ...) {
    va_list ap;

    if(row_count >= ROWS_MAX)
        return;
    row_t *r = &rows[row_count++];
    snprintf(r->key, sizeof(r->key), "%s", key);
    va_start(ap, fmt);
    vsnprintf(r->value, sizeof(r->value), fmt, ap);
    va_end(ap);
}

const char *cable_name(int cable) {
    switch(cable) {
        case CT_VGA: return "VGA";
        case CT_RGB: return "RGB / SCART";
        case CT_COMPOSITE: return "Composite / S-Video";
        case CT_NONE: return "None";
        default: return "Unknown";
    }
}

const char *region_name(int region) {
    switch(region) {
        case FLASHROM_REGION_JAPAN: return "Japan";
        case FLASHROM_REGION_US: return "USA";
        case FLASHROM_REGION_EUROPE: return "Europe";
        default: return "Unknown";
    }
}

static const char *lang_name(int lang) {
    static const char *const names[] = { "Japanese", "English", "German",
                                         "French", "Spanish", "Italian" };
    return lang >= 0 && lang < 6 ? names[lang] : "Unknown";
}

/* CPU clock from performance counter 1 against the TMU clock. */
float measure_cpu_mhz(void) {
    if(!cycles_available())
        return 0.0f;
    cycles_start();
    uint64_t t0 = now_us(), c0 = cycles_read();
    timer_spin_delay_us(20000);
    uint64_t t1 = now_us(), c1 = cycles_read();
    return (float)(c1 - c0) / (float)(t1 - t0);
}

static void gather(void) {
    char a[32], b[32];
    const char *banner = kos_get_banner();
    flashrom_syscfg_t cfg;
    struct mallinfo mi = mallinfo();
    uint32_t fpscr;

    row_count = 0;
    /* First line of the banner, without the newline. */
    char line[60];
    snprintf(line, sizeof(line), "%s", banner);
    char *nl = strchr(line, '\n');
    if(nl) *nl = 0;
    add("KallistiOS", "%s", KOS_VERSION_STRING);
    add("KOS banner", "%s", line);
    add("Compiler", "GCC %s", __VERSION__);
    add("C library", "newlib %s", _NEWLIB_VERSION);
    add("Built", "%s", kt_build_date);
    add("Loaded by", "%s", dcload_type == DCLOAD_TYPE_IP ? "dcload-ip" :
                            dcload_type == DCLOAD_TYPE_SER ? "dcload-serial" : "disc / emulator");

    cpu_mhz = measure_cpu_mhz();
    if(cpu_mhz > 0)
        add("CPU", "Hitachi SH-4 (SH7091), %.1f MHz measured", (double)cpu_mhz);
    else
        add("CPU", "Hitachi SH-4 (SH7091), clock n/a (no perf counters)");
    __asm__ volatile("sts fpscr, %0" : "=r"(fpscr));
    add("FPSCR", "0x%08lx  PR=%lu SZ=%lu FR=%lu DN=%lu RM=%lu", (unsigned long)fpscr,
        (unsigned long)(fpscr >> 19 & 1), (unsigned long)(fpscr >> 20 & 1),
        (unsigned long)(fpscr >> 21 & 1), (unsigned long)(fpscr >> 18 & 1),
        (unsigned long)(fpscr & 3));
    add("Caches", "8 KB instruction, 16 KB operand, store queues");

    fmt_bytes(a, sizeof(a), HW_MEMSIZE);
    add("Main RAM", "%s at 0x8c000000", a);
    fmt_bytes(a, sizeof(a), (uint32_t)end - 0x8c010000);
    add("Program", "%s (code, data, bss)", a);
    fmt_bytes(a, sizeof(a), (uint64_t)mi.uordblks);
    fmt_bytes(b, sizeof(b), mem_unallocated());
    add("Heap", "%s used, %s free", a, b);
    fmt_bytes(a, sizeof(a), pvr_mem_available());
    add("Video RAM", "8 MB, %s free for textures", a);
    add("Sound RAM", "2 MB (AICA, ARM7 at 22.5 MHz)");

    add("PowerVR", "CLX2, ID 0x%08lx rev 0x%02lx", (unsigned long)PVR_GET(PVR_ID),
        (unsigned long)PVR_GET(PVR_REVISION));
    add("Video cable", "%s", cable_name(vid_check_cable()));
    add("Video mode", "%dx%d, %d scanlines%s%s", vid_mode->width, vid_mode->height,
        vid_mode->scanlines, vid_mode->flags & VID_INTERLACE ? ", interlaced" : "",
        vid_mode->flags & VID_PAL ? ", PAL" : "");

    add("Region", "%s", region_name(flashrom_get_region()));
    if(flashrom_get_syscfg(&cfg) == 0)
        add("BIOS settings", "%s, %s, autostart %s", lang_name(cfg.language),
            cfg.audio ? "stereo" : "mono", cfg.autostart ? "on" : "off");
    else
        add("BIOS settings", "not readable");
    add("Maple devices", "%d attached", maple_enum_count());
}

static void sysinfo_enter(void) {
    gather();
}

static void sysinfo_update(const kt_input_t *in) {
    if(in->pressed & (CONT_B | CONT_START))
        kt_pop();
    if(in->pressed & CONT_A)
        gather();
}

static void sysinfo_draw(void) {
    time_t t = rtc_unix_secs();
    struct tm tm;
    uint64_t up = timer_ms_gettime64() / 1000;

    ui_background();
    ui_header("SYSTEM INFORMATION", KT_TITLE);
    for(int i = 0; i < row_count; i++) {
        float y = 48 + i * 17;
        gfx_text(16, y, C_DIM, rows[i].key);
        gfx_text_clip(176, y, C_TEXT, rows[i].value, 56);
    }
    float y = 48 + row_count * 17 + 6;
    gmtime_r(&t, &tm);
    gfx_text(16, y, C_DIM, "Clock (RTC)");
    gfx_textf(176, y, C_HI, "%04d-%02d-%02d %02d:%02d:%02d   uptime %lu:%02lu:%02lu",
              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
              tm.tm_sec, (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
              (unsigned long)(up % 60));
    ui_footer("A REFRESH   B BACK");
}

const kt_screen_t sysinfo_screen = {
    "System info", sysinfo_enter, sysinfo_update, sysinfo_draw, NULL
};
