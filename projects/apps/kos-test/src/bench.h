/* Benchmark ids. The VMU baseline stores results by id (the first
   BENCH_MAX of them), so only ever add new ids at the end, before
   BENCH_COUNT. */
#ifndef KT_BENCH_H
#define KT_BENCH_H

#include "kt.h"

enum {
    B_CRC32, B_SIEVE, B_QSORT, B_SPRINTF,
    B_FMADD, B_FDIV, B_FSQRT, B_FTRV, B_FIPR, B_SINF, B_FSCA, B_DMADD,
    B_MEMCPY, B_MEMSET, B_MEMREAD, B_SQCPY, B_P2WRITE,
    B_VRAM_SQ, B_VRAM_DMA, B_ARAM_WRITE, B_ARAM_DMA,
    B_THD_SWITCH, B_MALLOC, B_MUTEX, B_TIMER,
    B_GPU_FILL_OP, B_GPU_FILL_TR, B_GPU_FILL_TEX,
    B_GPU_TRI_FLAT, B_GPU_TRI_TEX, B_GPU_SPRITE,
    B_GPU_TA_PRIM, B_GPU_TA_DR, B_GPU_SCENE,
    B_VMU_BLOCK, B_VMU_CLOCK, B_VMU_DIR,
    B_ROM_READ, B_ROM_OPEN, B_ROM_STAT, B_ROM_DIR, B_RAM_WRITE, B_RAM_READ,
    B_GD_READ, B_GD_SEEK,
    B_MEMCPY16, B_MEMCPY64, B_MEMCPY256, B_MEMCPY64U, B_MEMSET64, B_STRLEN64,
    B_STREAM_CPU,
    BENCH_COUNT
};

/* Record one sample of a result. A single pass adds its report line now,
   repeated passes add the medians at the end of the run. */
void bench_result(kt_report_t *r, int id, double value);
/* True the first time a step calls it in a run, so check lines are added
   once however many passes repeat the step. */
bool bench_first_pass(unsigned *seen);

#endif
