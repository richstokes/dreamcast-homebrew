/* Small helpers shared by the tests: CRC32, a PRNG, timing. */
#include "kt.h"

#include <dc/perfctr.h>
#include <kos/mm.h>
#include <malloc.h>
#include <stdio.h>

static uint32_t crc_table[256];

uint32_t crc32_buf(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;

    if(!crc_table[1]) {
        for(uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for(int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    while(len--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* xorshift32 */
uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9e3779b9u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void fmt_bytes(char *out, size_t n, uint64_t bytes) {
    if(bytes >= 1024 * 1024)
        snprintf(out, n, "%lu.%02lu MB", (unsigned long)(bytes >> 20),
                 (unsigned long)((bytes & 0xfffff) * 100 >> 20));
    else if(bytes >= 1024)
        snprintf(out, n, "%lu KB", (unsigned long)(bytes >> 10));
    else
        snprintf(out, n, "%lu B", (unsigned long)bytes);
}

/* Free heap: what dlmalloc holds free plus everything above the break.
   Failed allocations print "Out of memory" from mm_sbrk, which under
   dcload-ip is a slow syscall each, so sizes come from here instead. */
size_t mem_unallocated(void) {
    struct mallinfo mi = mallinfo();
    uintptr_t brk = (uintptr_t)mm_sbrk(0);

    return (size_t)mi.fordblks + (_arch_mem_top - THD_KERNEL_STACK_SIZE - brk);
}

/* Room above the break alone: a block this size always fits. */
size_t mem_above_break(void) {
    return _arch_mem_top - THD_KERNEL_STACK_SIZE - (uintptr_t)mm_sbrk(0);
}

uint64_t now_us(void) {
    return timer_us_gettime64();
}

/* Maple hot-plug events after boot, per port and unit. A device that keeps
   dropping off the bus shows here, and disturbs any timing it overlaps. */
static volatile uint16_t attaches[MAPLE_PORT_COUNT][MAPLE_UNIT_COUNT];
static volatile uint16_t detaches[MAPLE_PORT_COUNT][MAPLE_UNIT_COUNT];

static void on_attach(maple_device_t *dev, void *unused) {
    attaches[dev->port][dev->unit]++;
}

static void on_detach(maple_device_t *dev, void *unused) {
    detaches[dev->port][dev->unit]++;
}

void maple_events_start(void) {
    maple_attach_callback(MAPLE_FUNC_ANY, on_attach, NULL);
    maple_detach_callback(MAPLE_FUNC_ANY, on_detach, NULL);
}

/* "B1 off 2 on 2, ..." or "none"; returns the number of events. */
int maple_events_text(char *out, size_t n) {
    size_t len = 0;
    int total = 0;

    out[0] = 0;
    for(int p = 0; p < MAPLE_PORT_COUNT; p++)
        for(int u = 0; u < MAPLE_UNIT_COUNT; u++) {
            int off = detaches[p][u], on = attaches[p][u];
            if(!off && !on)
                continue;
            total += off + on;
            if(len < n)
                len += (size_t)snprintf(out + len, n - len, "%s%c%d off %d on %d", len ? ", " : "",
                                        'A' + p, u, off, on);
        }
    if(!total)
        snprintf(out, n, "none");
    return total;
}

/* KOS keeps PRFC0 as its nanosecond clock; PRFC1 is free. */
void cycles_start(void) {
    perf_cntr_stop(PRFC1);
    perf_cntr_clear(PRFC1);
    perf_cntr_start(PRFC1, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_CPU_CYCLES);
}

uint64_t cycles_read(void) {
    return perf_cntr_count(PRFC1);
}

bool cycles_available(void) {
    static int known = -1;

    if(known < 0) {
        cycles_start();
        timer_spin_delay_us(200);
        known = cycles_read() > 1000;
        perf_cntr_stop(PRFC1);
    }
    return known;
}
