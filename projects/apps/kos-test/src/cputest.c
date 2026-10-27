/* CPU test suite: SH-4 integer and FPU instructions checked against known
   answers, registers, exceptions, caches, store queues, DMA, threads, libc
   and libm. One step per group; each group reports how many checks it ran
   and the first one that failed. */
#include "gfx.h"
#include "kt.h"

#include <dc/maple/vmu.h>
#include <dc/pvr.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <dc/sq.h>
#include <dc/matrix.h>
#include <kos/cond.h>
#include <kos/dbglog.h>
#include <kos/mutex.h>
#include <kos/sem.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kt_regfile(const uint32_t pat[13], uint32_t out[13]);
uint32_t kt_div1_u32(uint32_t n, uint32_t d);
void kt_fpu_banks(const float in[16], float out[16]);
uint32_t kt_illegal(void);
uint32_t kt_unaligned(const void *p);

/* Flycast models neither the caches, nor address errors, nor the perf
   counters; the missing counters are how the tests tell they are in it. */
static bool emulated(void) {
    return !cycles_available();
}

typedef struct {
    int n, bad;
    char first[64];
} chk_t;

static void chk(chk_t *c, bool ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void chk(chk_t *c, bool ok, const char *fmt, ...) {
    va_list ap;

    c->n++;
    if(ok || c->bad++)
        return;
    va_start(ap, fmt);
    vsnprintf(c->first, sizeof(c->first), fmt, ap);
    va_end(ap);
}

static void chk32(chk_t *c, const char *what, uint32_t got, uint32_t want) {
    chk(c, got == want, "%s=%08lx want %08lx", what, (unsigned long)got, (unsigned long)want);
}

static void chk64(chk_t *c, const char *what, uint64_t got, uint64_t want) {
    chk(c, got == want, "%s=%08lx%08lx", what, (unsigned long)(got >> 32), (unsigned long)got);
}

static void chkf(chk_t *c, const char *what, float got, float want, float tol) {
    float err = fabsf(got - want);
    if(fabsf(want) > 1.0f)
        err /= fabsf(want);
    chk(c, err <= tol, "%s=%g want %g", what, (double)got, (double)want);
}

static void chk_done(kt_report_t *r, const char *name, const chk_t *c) {
    if(c->bad)
        report_add(r, KT_FAIL, name, "%d of %d bad: %s", c->bad, c->n, c->first);
    else
        report_add(r, KT_PASS, name, "%d checks", c->n);
}

static uint32_t fbits(float f) {
    union { float f; uint32_t u; } x = { .f = f };
    return x.u;
}

static uint64_t dbits(double d) {
    union { double d; uint64_t u; } x = { .d = d };
    return x.u;
}

/* volatile inputs keep GCC from folding the answers at compile time */
static volatile uint32_t v_a = 0x12345678, v_b = 0x9abcdef0, v_ones = 0xffffffff,
                         v_one = 1, v_max = 0x7fffffff, v_min = 0x80000000;

/* ---- Integer ALU ---- */

#define T_AFTER(op, a, b) ({ uint32_t _t; \
    __asm__ volatile(op " %2,%1\n\tmovt %0" : "=r"(_t) : "r"(a), "r"(b) : "t"); _t; })

static void step_alu(kt_report_t *r) {
    chk_t c = { 0 };
    uint32_t lo, hi, t;

    /* 64-bit add through ADDC carries */
    lo = v_ones; hi = 0;
    __asm__("clrt\n\taddc %2,%0\n\taddc %3,%1" : "+r"(lo), "+r"(hi) : "r"(v_one), "r"(0) : "t");
    chk32(&c, "addc lo", lo, 0);
    chk32(&c, "addc hi", hi, 1);
    /* SUBC borrows */
    lo = 0; hi = 1;
    __asm__("clrt\n\tsubc %2,%0\n\tsubc %3,%1" : "+r"(lo), "+r"(hi) : "r"(v_one), "r"(0) : "t");
    chk32(&c, "subc lo", lo, 0xffffffff);
    chk32(&c, "subc hi", hi, 0);
    /* ADDV/SUBV signed overflow */
    t = v_max;
    __asm__("addv %2,%0\n\tmovt %1" : "+r"(t), "=r"(hi) : "r"(v_one) : "t");
    chk32(&c, "addv T", hi, 1);
    t = v_min;
    __asm__("subv %2,%0\n\tmovt %1" : "+r"(t), "=r"(hi) : "r"(v_one) : "t");
    chk32(&c, "subv T", hi, 1);
    /* NEGC: 0 - Rm - T */
    __asm__("sett\n\tnegc %2,%0\n\tmovt %1" : "=r"(lo), "=r"(hi) : "r"(0) : "t");
    chk32(&c, "negc", lo, 0xffffffff);
    chk32(&c, "negc T", hi, 1);
    /* DT */
    t = v_one;
    __asm__("dt %0\n\tmovt %1" : "+r"(t), "=r"(hi) : : "t");
    chk32(&c, "dt T", hi, 1);
    chk32(&c, "and", v_a & v_b, 0x12345670);
    chk32(&c, "or", v_a | v_b, 0x9abcdef8);
    chk32(&c, "xor", v_a ^ v_b, 0x88888888);
    chk32(&c, "not", ~v_a, 0xedcba987);
    chk32(&c, "neg", -v_a, 0xedcba988);

    /* Compares (cmp/xx Rm,Rn tests Rn against Rm) */
    chk32(&c, "cmp/eq", T_AFTER("cmp/eq", v_a, v_a), 1);
    chk32(&c, "cmp/hs", T_AFTER("cmp/hs", v_b, v_a), 1);
    chk32(&c, "cmp/hi", T_AFTER("cmp/hi", v_a, v_a), 0);
    chk32(&c, "cmp/ge", T_AFTER("cmp/ge", v_a, v_b), 1);   /* signed: a >= (negative) b */
    chk32(&c, "cmp/gt", T_AFTER("cmp/gt", v_b, v_a), 0);
    chk32(&c, "cmp/str", T_AFTER("cmp/str", v_a, (uint32_t)0xff34ffff), 1);
    chk32(&c, "cmp/str 0", T_AFTER("cmp/str", v_a, v_ones), 0);
    chk32(&c, "tst", T_AFTER("tst", v_a, (uint32_t)0x80000000), 1);
    __asm__("cmp/pz %1\n\tmovt %0" : "=r"(t) : "r"(v_min) : "t");
    chk32(&c, "cmp/pz", t, 0);
    __asm__("cmp/pl %1\n\tmovt %0" : "=r"(t) : "r"(v_one) : "t");
    chk32(&c, "cmp/pl", t, 1);

    /* Shifts */
#define SHAD(v, s) ({ uint32_t _v = (v); __asm__("shad %1,%0" : "+r"(_v) : "r"((int32_t)(s))); _v; })
#define SHLD(v, s) ({ uint32_t _v = (v); __asm__("shld %1,%0" : "+r"(_v) : "r"((int32_t)(s))); _v; })
    chk32(&c, "shad -4", SHAD(v_min, -4), 0xf8000000);
    chk32(&c, "shad 31", SHAD(v_one, 31), 0x80000000);
    chk32(&c, "shad -32", SHAD(v_min, -32), 0xffffffff);
    chk32(&c, "shld -4", SHLD(v_min, -4), 0x08000000);
    chk32(&c, "shld -32", SHLD(v_min, -32), 0);
    chk32(&c, "shld 8", SHLD(v_a, 8), 0x34567800);
    t = v_a;
    __asm__("shll16 %0\n\tshlr8 %0\n\tshll2 %0" : "+r"(t));
    chk32(&c, "shll16/shlr8/shll2", t, 0x0159e000);
    t = v_min;
    __asm__("shar %0\n\tshar %0" : "+r"(t) : : "t");
    chk32(&c, "shar", t, 0xe0000000);
    /* Rotates */
    t = v_min | 1;
    __asm__("rotl %0\n\tmovt %1" : "+r"(t), "=r"(hi) : : "t");
    chk32(&c, "rotl", t, 0x00000003);
    chk32(&c, "rotl T", hi, 1);
    t = v_one;
    __asm__("rotr %0" : "+r"(t) : : "t");
    chk32(&c, "rotr", t, 0x80000000);
    t = 0;
    __asm__("sett\n\trotcl %0\n\tmovt %1" : "+r"(t), "=r"(hi) : : "t");
    chk32(&c, "rotcl", t, 1);
    chk32(&c, "rotcl T", hi, 0);
    t = v_one;
    __asm__("clrt\n\trotcr %0\n\tmovt %1" : "+r"(t), "=r"(hi) : : "t");
    chk32(&c, "rotcr", t, 0);
    chk32(&c, "rotcr T", hi, 1);
    /* Byte and word moves */
    __asm__("swap.b %1,%0" : "=r"(t) : "r"(v_a));
    chk32(&c, "swap.b", t, 0x12347856);
    __asm__("swap.w %1,%0" : "=r"(t) : "r"(v_a));
    chk32(&c, "swap.w", t, 0x56781234);
    t = v_b;
    __asm__("xtrct %1,%0" : "+r"(t) : "r"(v_a));
    chk32(&c, "xtrct", t, 0x56789abc);
    __asm__("exts.b %1,%0" : "=r"(t) : "r"((uint32_t)0x1280));
    chk32(&c, "exts.b", t, 0xffffff80);
    __asm__("extu.b %1,%0" : "=r"(t) : "r"((uint32_t)0xffffff80));
    chk32(&c, "extu.b", t, 0x80);
    __asm__("exts.w %1,%0" : "=r"(t) : "r"((uint32_t)0x18000));
    chk32(&c, "exts.w", t, 0xffff8000);
    __asm__("extu.w %1,%0" : "=r"(t) : "r"(v_ones));
    chk32(&c, "extu.w", t, 0xffff);
    chk_done(r, "Integer ALU and shifts", &c);
}

/* ---- Multiply and divide ---- */

static const struct { uint32_t n, d, q, m; } udivs[] = {
    { 1000000007, 13, 0x0495c0c5, 6 },
    { 0xffffffff, 3, 0x55555555, 0 },
    { 0x80000000, 0x10000, 0x8000, 0 },
    { 123456789, 987654321, 0, 123456789 },
    { 0xfffffffe, 0xffffffff, 0, 0xfffffffe },
    { 7, 1, 7, 0 },
    { 0xdeadbeef, 0x1234, 0xc3ba5, 0x76b },
};

static const struct { int32_t n, d, q, m; } sdivs[] = {
    { -1000000007, 13, -76923077, -6 },
    { 1000000007, -13, -76923077, 6 },
    { -2147483647, -1, 2147483647, 0 },
    { -7, 2, -3, -1 },
    { 7, -2, -3, 1 },
    { -2147483647 - 1, 2, -1073741824, 0 },
};

static void step_muldiv(kt_report_t *r) {
    chk_t c = { 0 };
    uint32_t hi, lo;

    __asm__("dmulu.l %2,%3\n\tsts mach,%0\n\tsts macl,%1" : "=r"(hi), "=r"(lo)
            : "r"(v_a), "r"(v_b) : "mach", "macl");
    chk64(&c, "dmulu.l", (uint64_t)hi << 32 | lo, 0x0b00ea4e242d2080ull);
    __asm__("dmuls.l %2,%3\n\tsts mach,%0\n\tsts macl,%1" : "=r"(hi), "=r"(lo)
            : "r"((int32_t)-123456789), "r"((int32_t)987654321) : "mach", "macl");
    chk64(&c, "dmuls.l", (uint64_t)hi << 32 | lo, 0xfe4eceeb0400ac7bull);
    __asm__("mul.l %1,%2\n\tsts macl,%0" : "=r"(lo) : "r"(v_a), "r"(v_b) : "macl");
    chk32(&c, "mul.l", lo, 0x242d2080);
    __asm__("mulu.w %1,%2\n\tsts macl,%0" : "=r"(lo) : "r"(v_ones), "r"(v_ones) : "macl");
    chk32(&c, "mulu.w", lo, 0xfffe0001);
    __asm__("muls.w %1,%2\n\tsts macl,%0" : "=r"(lo) : "r"(v_ones), "r"((uint32_t)2) : "macl");
    chk32(&c, "muls.w", lo, 0xfffffffe);

    /* MAC.L and MAC.W accumulate 64 bits with S clear */
    static const int32_t ma[4] = { 0x7fffffff, -5, 1000000, 123 };
    static const int32_t mb[4] = { 0x7fffffff, 7, 3000000, -321 };
    const int32_t *pa = ma, *pb = mb;
    __asm__ volatile("clrs\n\tclrmac\n\t"
                     "mac.l @%2+,@%3+\n\tmac.l @%2+,@%3+\n\tmac.l @%2+,@%3+\n\tmac.l @%2+,@%3+\n\t"
                     "sts mach,%0\n\tsts macl,%1"
                     : "=r"(hi), "=r"(lo), "+r"(pa), "+r"(pb) : "m"(ma), "m"(mb) : "mach", "macl");
    chk64(&c, "mac.l", (uint64_t)hi << 32 | lo, 0x400002b97dee95a3ull);
    static const int16_t wa[4] = { 0x7fff, -2, 300, -1 };
    static const int16_t wb[4] = { 0x7fff, 3, -200, -1 };
    const int16_t *qa = wa, *qb = wb;
    __asm__ volatile("clrs\n\tclrmac\n\t"
                     "mac.w @%2+,@%3+\n\tmac.w @%2+,@%3+\n\tmac.w @%2+,@%3+\n\tmac.w @%2+,@%3+\n\t"
                     "sts mach,%0\n\tsts macl,%1"
                     : "=r"(hi), "=r"(lo), "+r"(qa), "+r"(qb) : "m"(wa), "m"(wb) : "mach", "macl");
    chk32(&c, "mac.w", lo, 0x3ffe159c);

    for(size_t i = 0; i < sizeof(udivs) / sizeof(udivs[0]); i++) {
        volatile uint32_t n = udivs[i].n, d = udivs[i].d;
        chk32(&c, "udiv", n / d, udivs[i].q);
        chk32(&c, "umod", n % d, udivs[i].m);
        chk32(&c, "div1", kt_div1_u32(n, d), udivs[i].q);
    }
    for(size_t i = 0; i < sizeof(sdivs) / sizeof(sdivs[0]); i++) {
        volatile int32_t n = sdivs[i].n, d = sdivs[i].d;
        chk32(&c, "sdiv", (uint32_t)(n / d), (uint32_t)sdivs[i].q);
        chk32(&c, "smod", (uint32_t)(n % d), (uint32_t)sdivs[i].m);
    }
    volatile uint64_t n64 = 0xfedcba9876543210ull, d64 = 0x12345;
    chk64(&c, "u64 div", n64 / d64, 0xe0004fa01c4dull);
    chk64(&c, "u64 mod", n64 % d64, 0x10a4f);
    volatile int64_t s64 = -0x123456789abcll, e64 = 1000;
    chk64(&c, "s64 div", (uint64_t)(s64 / e64), 0xfffffffb56f41a79ull);
    chk64(&c, "s64 mod", (uint64_t)(s64 % e64), 0xfffffffffffffc9cull);
    chk64(&c, "u64 mul", (uint64_t)v_ones * v_ones, 0xfffffffe00000001ull);
    chk64(&c, "u64 shl", (uint64_t)v_a << 40, 0x3456780000000000ull);
    chk64(&c, "s64 shr", (uint64_t)((int64_t)0x8000000000000000ull >> (v_one * 60)),
          0xfffffffffffffff8ull);
    chk_done(r, "Multiply, MAC, divide", &c);
}

/* ---- Registers ---- */

#define STC_BANK(n, out) __asm__ volatile("stc r" #n "_bank,%0" : "=r"(out))
#define LDC_BANK(n, in) __asm__ volatile("ldc %0,r" #n "_bank" : : "r"(in))

static void step_regs(kt_report_t *r) {
    chk_t c = { 0 };
    uint32_t pat[13], out[13], seed = 0x1234567;

    for(int pass = 0; pass < 40; pass++) {
        for(int i = 0; i < 13; i++) {
            if(pass == 0) pat[i] = 0x55555555;
            else if(pass == 1) pat[i] = 0xaaaaaaaa;
            else pat[i] = pass < 34 ? 1u << ((pass - 2 + i) & 31) : rng_next(&seed);
        }
        memset(out, 0, sizeof(out));
        kt_regfile(pat, out);
        for(int i = 0; i < 13; i++)
            chk32(&c, "rN", out[i], pat[i]);
    }
    chk_done(r, "General registers", &c);

    /* R0-R7 of bank 1, which only exception entry uses */
    chk_t b = { 0 };
    uint32_t save[8], got[8];
    irq_mask_t old = irq_disable();
    STC_BANK(0, save[0]); STC_BANK(1, save[1]); STC_BANK(2, save[2]); STC_BANK(3, save[3]);
    STC_BANK(4, save[4]); STC_BANK(5, save[5]); STC_BANK(6, save[6]); STC_BANK(7, save[7]);
    for(int pass = 0; pass < 2; pass++) {
        uint32_t base = pass ? 0xa5a5a5a5 : 0x5a5a5a5a;
        LDC_BANK(0, base ^ 0x00); LDC_BANK(1, base ^ 0x11); LDC_BANK(2, base ^ 0x22);
        LDC_BANK(3, base ^ 0x33); LDC_BANK(4, base ^ 0x44); LDC_BANK(5, base ^ 0x55);
        LDC_BANK(6, base ^ 0x66); LDC_BANK(7, base ^ 0x77);
        STC_BANK(0, got[0]); STC_BANK(1, got[1]); STC_BANK(2, got[2]); STC_BANK(3, got[3]);
        STC_BANK(4, got[4]); STC_BANK(5, got[5]); STC_BANK(6, got[6]); STC_BANK(7, got[7]);
        for(int i = 0; i < 8; i++)
            chk32(&b, "rN_bank", got[i], base ^ (uint32_t)(i * 0x11));
    }
    LDC_BANK(0, save[0]); LDC_BANK(1, save[1]); LDC_BANK(2, save[2]); LDC_BANK(3, save[3]);
    LDC_BANK(4, save[4]); LDC_BANK(5, save[5]); LDC_BANK(6, save[6]); LDC_BANK(7, save[7]);

    /* GBR holds the TLS pointer: put it back before anything uses TLS. */
    uint32_t gbr, g2;
    __asm__ volatile("stc gbr,%0" : "=r"(gbr));
    __asm__ volatile("ldc %1,gbr\n\tstc gbr,%0\n\tldc %2,gbr" : "=&r"(g2) : "r"(0x8c123450u), "r"(gbr));
    irq_restore(old);
    chk32(&b, "gbr", g2, 0x8c123450u);
    chk_done(r, "Banked registers, GBR", &b);

    chk_t s = { 0 };
    uint32_t sr, vbr, ssr, spc, mach, macl;
    __asm__ volatile("stc sr,%0" : "=r"(sr));
    __asm__ volatile("stc vbr,%0" : "=r"(vbr));
    __asm__ volatile("stc ssr,%0" : "=r"(ssr));
    __asm__ volatile("stc spc,%0" : "=r"(spc));
    chk(&s, sr & (1u << 30), "SR.MD clear (user mode)");
    chk(&s, !(sr & (1u << 29)), "SR.RB set");
    chk(&s, !(sr & (1u << 28)), "SR.BL set");
    chk(&s, !(sr & (1u << 15)), "SR.FD set (FPU off)");
    chk(&s, (vbr & 0xff000000) == 0x8c000000, "VBR %08lx", (unsigned long)vbr);
    __asm__ volatile("lds %2,mach\n\tlds %3,macl\n\tsts mach,%0\n\tsts macl,%1"
                     : "=r"(mach), "=r"(macl) : "r"(v_a), "r"(v_b) : "mach", "macl");
    chk32(&s, "mach", mach, v_a);
    chk32(&s, "macl", macl, v_b);
    chk_done(r, "Control registers", &s);
    report_add(r, KT_INFO, "SR / VBR", "SR %08lx VBR %08lx SPC %08lx", (unsigned long)sr,
               (unsigned long)vbr, (unsigned long)spc);
    (void)ssr;
}

/* ---- FPU ---- */

/* a / b with FPSCR.RM set to rm for the one instruction. */
static float fdiv_rm(float a, float b, uint32_t rm) {
    uint32_t old, tmp;
    float q = a;

    __asm__ volatile("sts fpscr,%0\n\t"
                     "mov %0,%1\n\t"
                     "and %5,%1\n\t"
                     "or %4,%1\n\t"
                     "lds %1,fpscr\n\t"
                     "fdiv %3,%2\n\t"
                     "lds %0,fpscr"
                     : "=&r"(old), "=&r"(tmp), "+f"(q) : "f"(b), "r"(rm), "r"(~3u));
    return q;
}

static volatile float f_one = 1.0f, f_two = 2.0f, f_three = 3.0f, f_tenth = 0.1f,
                      f_fifth = 0.2f, f_big = 3e9f, f_zero = 0.0f;
static volatile double d_one = 1.0, d_two = 2.0, d_three = 3.0, d_big = 1e308;

static void step_fpu(kt_report_t *r) {
    chk_t c = { 0 };
    uint32_t fpscr;

    __asm__ volatile("sts fpscr,%0" : "=r"(fpscr));
    chk32(&c, "FPSCR.PR/SZ", fpscr & (3u << 19), 0);
    chk32(&c, "1/3", fbits(f_one / f_three), 0x3eaaaaab);
    chk32(&c, "sqrtf 2", fbits(sqrtf(f_two)), 0x3fb504f3);
    chk32(&c, "0.1+0.2", fbits(f_tenth + f_fifth), 0x3e99999a);
    chk32(&c, "RM nearest 1/3", fbits(fdiv_rm(1, 3, 0)), 0x3eaaaaab);
    chk32(&c, "RM zero 1/3", fbits(fdiv_rm(1, 3, 1)), 0x3eaaaaaa);
    chk32(&c, "RM zero 2/3", fbits(fdiv_rm(2, 3, 1)), 0x3f2aaaaa);
    /* fmac: fr0 * Fm + Fn */
    {
        register float f0 __asm__("fr0") = f_three;
        float acc = f_one, m = f_two;
        __asm__("fmac fr0,%1,%0" : "+f"(acc) : "f"(m), "f"(f0));
        chk32(&c, "fmac", fbits(acc), fbits(7.0f));
    }
    chk32(&c, "fabs", fbits(fabsf(-f_two)), fbits(2.0f));
    chk32(&c, "ftrc 3.99", (uint32_t)(int)(f_three + 0.99f), 3);
    chk32(&c, "ftrc -3.99", (uint32_t)(int)-(f_three + 0.99f), (uint32_t)-3);
    chk32(&c, "ftrc sat", (uint32_t)(int)f_big, 0x7fffffff);
    chk32(&c, "float 16777217", fbits((float)(v_one + 16777216)), 0x4b800000);
    float inf = f_one / f_zero;
    chk(&c, isinf(inf) && inf > 0, "1/0 not +inf");
    float nan = f_zero / f_zero;
    chk(&c, nan != nan, "NaN == NaN");
    chk32(&c, "-0.0", fbits(-f_zero), 0x80000000);
    chk64(&c, "1/3 double", dbits(d_one / d_three), 0x3fd5555555555555ull);
    chk64(&c, "sqrt 2 double", dbits(sqrt(d_two)), 0x3ff6a09e667f3bcdull);
    chk(&c, isinf(d_big * 10.0), "1e309 not inf");
    chk64(&c, "double mul", dbits(d_three * 0.5), dbits(1.5));
    chk_done(r, "FPU single and double", &c);

    /* FRCHG: the back bank holds values on its own */
    chk_t b = { 0 };
    float in[16], out[16];
    for(int i = 0; i < 16; i++)
        in[i] = (float)(i * 3 + 1) * 0.25f;
    irq_mask_t old = irq_disable();
    kt_fpu_banks(in, out);
    irq_restore(old);
    for(int i = 0; i < 16; i++)
        chk32(&b, "xf", fbits(out[i]), fbits(in[i]));
    chk_done(r, "FPU register banks", &b);
}

/* SH-4 vector instructions */
static float sh4_fsrra(float x) {
    __asm__("fsrra %0" : "+f"(x));
    return x;
}

static void sh4_fsca(uint32_t angle, float *s, float *co) {
    register float f0 __asm__("fr0");
    register float f1 __asm__("fr1");

    __asm__("lds %2,fpul\n\tfsca fpul,dr0" : "=f"(f0), "=f"(f1) : "r"(angle) : "fpul");
    *s = f0;
    *co = f1;
}

static float sh4_fipr(const float a[4], const float b[4]) {
    register float a0 __asm__("fr0") = a[0];
    register float a1 __asm__("fr1") = a[1];
    register float a2 __asm__("fr2") = a[2];
    register float a3 __asm__("fr3") = a[3];
    register float b0 __asm__("fr4") = b[0];
    register float b1 __asm__("fr5") = b[1];
    register float b2 __asm__("fr6") = b[2];
    register float b3 __asm__("fr7") = b[3];

    __asm__("fipr fv4,fv0" : "+f"(a3) : "f"(a0), "f"(a1), "f"(a2), "f"(b0), "f"(b1), "f"(b2), "f"(b3));
    return a3;
}

static void sh4_ftrv(const float in[4], float out[4]) {
    register float f0 __asm__("fr0") = in[0];
    register float f1 __asm__("fr1") = in[1];
    register float f2 __asm__("fr2") = in[2];
    register float f3 __asm__("fr3") = in[3];

    __asm__("ftrv xmtrx,fv0" : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3));
    out[0] = f0;
    out[1] = f1;
    out[2] = f2;
    out[3] = f3;
}

static void step_vector(kt_report_t *r) {
    chk_t c = { 0 };
    float worst = 0;

    static const float roots[] = { 1, 2, 4, 100, 0.25f, 12345.0f };
    for(size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        float want = 1.0f / sqrtf(roots[i]), got = sh4_fsrra(roots[i]);
        float err = fabsf(got - want) / want;
        if(err > worst) worst = err;
        chk(&c, err < 1e-5f, "sh4_fsrra(%g)=%g", (double)roots[i], (double)got);
    }
    report_add(r, KT_INFO, "FSRRA error", "worst %.2e relative", (double)worst);

    worst = 0;
    for(int i = 0; i < 64; i++) {
        uint32_t a = (uint32_t)i * 1024;
        float s, co, rad = a * (2.0f * (float)M_PI / 65536.0f);
        sh4_fsca(a, &s, &co);
        float e = fmaxf(fabsf(s - sinf(rad)), fabsf(co - cosf(rad)));
        if(e > worst) worst = e;
        chk(&c, e < 1e-4f, "sh4_fsca(%lu) off by %g", (unsigned long)a, (double)e);
    }
    report_add(r, KT_INFO, "FSCA error", "worst %.2e absolute", (double)worst);

    static const float va[4] = { 1.5f, -2.0f, 3.25f, 0.5f }, vb[4] = { 4.0f, 0.25f, -1.0f, 8.0f };
    float want = 0;
    for(int i = 0; i < 4; i++) want += va[i] * vb[i];
    chkf(&c, "fipr", sh4_fipr(va, vb), want, 1e-5f);

    static matrix_t m __attribute__((aligned(32))) = {
        { 1, 2, 3, 4 }, { -1, 0.5f, 2, 0 }, { 0, 1, -3, 1 }, { 10, 20, 30, 1 }
    };
    mat_load(&m);
    static const float v[4] = { 1.0f, -2.0f, 0.5f, 1.0f };
    float out[4];
    sh4_ftrv(v, out);
    for(int i = 0; i < 4; i++) {
        float e = 0;
        for(int j = 0; j < 4; j++) e += m[j][i] * v[j];
        chkf(&c, "ftrv", out[i], e, 1e-5f);
    }
    chk_done(r, "FSRRA, FSCA, FIPR, FTRV", &c);
}

/* ---- Exceptions ---- */

static volatile int trap_hits, illegal_hits, address_hits;

static void on_trap(irq_t code, irq_context_t *cx, void *d) {
    trap_hits++;
    cx->r[0] = 0x600d;
}

static void on_illegal(irq_t code, irq_context_t *cx, void *d) {
    illegal_hits++;
    cx->r[0] = 0x1234;
    cx->pc += 2;
}

static void on_address(irq_t code, irq_context_t *cx, void *d) {
    address_hits++;
    cx->pc += 2;
}

static void step_exceptions(kt_report_t *r) {
    irq_cb_t old_trap = irq_get_handler(IRQ_TRAP_CODE(0x42));
    irq_cb_t old_ill = irq_get_handler(EXC_ILLEGAL_INSTR);
    irq_cb_t old_addr = irq_get_handler(EXC_DATA_ADDRESS_READ);
    uint32_t rv;

    trap_hits = illegal_hits = address_hits = 0;
    irq_set_handler(IRQ_TRAP_CODE(0x42), on_trap, NULL);
    __asm__ volatile("mov #0,r0\n\ttrapa #0x42\n\tmov r0,%0" : "=r"(rv) : : "r0", "memory");
    irq_set_handler(IRQ_TRAP_CODE(0x42), old_trap.hdl, old_trap.data);
    report_check(r, trap_hits == 1 && rv == 0x600d, "TRAPA #0x42",
                 "handler ran %d time(s), r0=%04lx", trap_hits, (unsigned long)rv);

    irq_set_handler(EXC_ILLEGAL_INSTR, on_illegal, NULL);
    rv = kt_illegal();
    irq_set_handler(EXC_ILLEGAL_INSTR, old_ill.hdl, old_ill.data);
    report_check(r, illegal_hits == 1 && rv == 0x1234, "Illegal instruction",
                 "0xfffd trapped %d time(s), resumed", illegal_hits);

    static uint32_t words[2] = { 0x11223344, 0x55667788 };
    irq_set_handler(EXC_DATA_ADDRESS_READ, on_address, NULL);
    kt_unaligned((const uint8_t *)words + 2);
    irq_set_handler(EXC_DATA_ADDRESS_READ, old_addr.hdl, old_addr.data);
    if(address_hits == 1)
        report_add(r, KT_PASS, "Address error", "misaligned read trapped and resumed");
    else if(emulated())
        report_add(r, KT_SKIP, "Address error", "not trapped (emulators skip this check)");
    else
        report_add(r, KT_FAIL, "Address error", "misaligned read not trapped");
}

/* ---- Caches, store queues, DMA ---- */

static void step_cache(kt_report_t *r) {
    static volatile uint32_t line[8] __attribute__((aligned(32)));
    volatile uint32_t *p2 = (volatile uint32_t *)(((uintptr_t)line & 0x1fffffff) | 0xa0000000);

    if(emulated())
        report_add(r, KT_SKIP, "Operand cache", "Flycast has no cache model");
    else {
        chk_t c = { 0 };
        line[0] = 1;
        dcache_wback_range((uintptr_t)line, 32);
        line[0] = 2;            /* dirty in the copy-back cache */
        chk32(&c, "RAM before write-back", p2[0], 1);
        dcache_wback_range((uintptr_t)line, 32);
        chk32(&c, "RAM after write-back", p2[0], 2);
        p2[0] = 3;              /* behind the cache's back */
        chk32(&c, "stale cached copy", line[0], 2);
        dcache_inval_range((uintptr_t)line, 32);
        chk32(&c, "after invalidate", line[0], 3);
        chk_done(r, "Operand cache copy-back", &c);
    }

    /* Code written as data runs once the caches are synced. */
    static uint16_t code[16] __attribute__((aligned(32)));
    chk_t s = { 0 };
    for(int k = 0; k < 3; k++) {
        int8_t imm = (int8_t)(17 + k * 40);
        code[0] = (uint16_t)(0xe000 | (uint8_t)imm);    /* mov #imm,r0 */
        code[1] = 0x000b;                               /* rts */
        code[2] = 0x0009;                               /* nop */
        icache_sync_range((uintptr_t)code, sizeof(code));
        int (*fn)(void) = (int (*)(void))(uintptr_t)code;
        chk32(&s, "generated code", (uint32_t)fn(), (uint32_t)(int32_t)imm);
    }
    chk_done(r, "Self-modifying code", &s);

    /* Store queues to RAM */
    chk_t q = { 0 };
    uint32_t *src = memalign(32, 8192), *dst = memalign(32, 8192);
    uint32_t seed = 99;
    for(int i = 0; i < 2048; i++) src[i] = rng_next(&seed);
    memset(dst, 0, 8192);
    dcache_purge_range((uintptr_t)dst, 8192);
    sq_cpy(dst, src, 8192);
    sq_wait();
    dcache_inval_range((uintptr_t)dst, 8192);
    chk(&q, !memcmp(src, dst, 8192), "sq_cpy data differs");
    sq_set32(dst, 0xdeadbeef, 8192);
    sq_wait();
    dcache_inval_range((uintptr_t)dst, 8192);
    int bad = 0;
    for(int i = 0; i < 2048; i++) if(dst[i] != 0xdeadbeef) bad++;
    chk(&q, !bad, "sq_set32: %d words wrong", bad);

    /* sq_cpy() works in 1 MiB chunks and pvr_txr_load() goes through it;
       KallistiOS#1529 read every chunk after the first from the wrong place. */
    size_t big = (1024 + 8) * 1024;
    uint32_t *bs = memalign(32, big), *bd = memalign(32, big);
    if(bs && bd) {
        for(size_t i = 0; i < big / 4; i++) bs[i] = rng_next(&seed);
        dcache_purge_range((uintptr_t)bd, big);
        sq_cpy(bd, bs, big);
        sq_wait();
        dcache_inval_range((uintptr_t)bd, big);
        chk(&q, !memcmp(bs, bd, big), "sq_cpy past 1 MiB differs");
        pvr_ptr_t tex = pvr_mem_malloc(big);
        if(tex) {
            pvr_txr_load(bs, tex, big);
            chk(&q, !memcmp(bs, (const void *)tex, big), "pvr_txr_load past 1 MiB differs");
            pvr_mem_free(tex);
        }
    }
    free(bs);
    free(bd);
    chk_done(r, "Store queues", &q);

    /* PVR DMA (SH-4 DMAC channel 2) to VRAM, read back over the bus */
    pvr_ptr_t vram = pvr_mem_malloc(8192);
    if(vram) {
        dcache_wback_range((uintptr_t)src, 8192);
        int rv = pvr_txr_load_dma(src, vram, 8192, true, NULL, NULL);
        bool same = rv == 0 && !memcmp(src, (const void *)vram, 8192);
        report_check(r, same, "PVR DMA to VRAM", rv ? "transfer failed" : same ? "8 KB verified" :
                     "data differs");
        pvr_mem_free(vram);
    }

    /* G2 DMA to sound RAM */
    snd_init();
    uint32_t aram = snd_mem_malloc(8192);
    if(aram) {
        dcache_wback_range((uintptr_t)src, 8192);
        int rv = spu_dma_transfer(src, aram, 8192, 1, NULL, NULL);
        memset(dst, 0, 8192);
        spu_memread(dst, aram, 8192);
        bool same = rv == 0 && !memcmp(src, dst, 8192);
        report_check(r, same, "G2 DMA to sound RAM", rv ? "transfer failed" : same ?
                     "8 KB verified" : "data differs");
        snd_mem_free(aram);
    }
    free(src);
    free(dst);
}

/* ---- Stereo sample split ----
   snd_stream_fill() splits interleaved 16-bit stereo with snd_pcm16_split()
   when the data is 32-byte aligned and plays the first buffer panned left;
   snd_pcm16_split_sq() splits straight into sound RAM. Both must put the
   left samples in `left`. KOS 2.3.0's asm swaps them (KallistiOS#1557). */

#define SPLIT_FRAMES 512

/* How many of out's pairs hold the matching pair of want, in either order:
   the split also swaps neighbouring samples, the way the AICA reads them. */
static int split_pairs(const uint16_t *out, uint16_t want) {
    int ok = 0;
    for(int i = 0; i < SPLIT_FRAMES; i += 2) {
        uint16_t a = (uint16_t)(want + i), b = (uint16_t)(want + i + 1);
        ok += (out[i] == a && out[i + 1] == b) || (out[i] == b && out[i + 1] == a);
    }
    return ok;
}

static void split_check(kt_report_t *r, const char *name, const uint16_t *left,
                        const uint16_t *right) {
    int lok = split_pairs(left, 0x1000), rok = split_pairs(right, 0x2000);
    int pairs = SPLIT_FRAMES / 2;

    if(lok == pairs && rok == pairs)
        report_add(r, KT_PASS, name, "%d frames, channels in place", SPLIT_FRAMES);
    else if(split_pairs(left, 0x2000) == pairs && split_pairs(right, 0x1000) == pairs)
        report_add(r, KT_FAIL, name, "left and right swapped (KallistiOS#1557)");
    else
        report_add(r, KT_FAIL, name, "%d and %d of %d pairs where they belong", lok, rok, pairs);
}

static void step_split(kt_report_t *r) {
    static uint16_t in[SPLIT_FRAMES * 2] __attribute__((aligned(32)));
    static uint16_t left[SPLIT_FRAMES] __attribute__((aligned(32)));
    static uint16_t right[SPLIT_FRAMES] __attribute__((aligned(32)));

    for(int i = 0; i < SPLIT_FRAMES; i++) {
        in[i * 2] = (uint16_t)(0x1000 + i);
        in[i * 2 + 1] = (uint16_t)(0x2000 + i);
    }
    memset(left, 0, sizeof(left));
    memset(right, 0, sizeof(right));
    snd_pcm16_split((uint32_t *)in, (uint32_t *)left, (uint32_t *)right, sizeof(in));
    split_check(r, "pcm16 split", left, right);

    snd_init();
    uint32_t aram = snd_mem_malloc(sizeof(in));
    if(!aram) {
        report_add(r, KT_SKIP, "pcm16 split to sound RAM", "no sound RAM free");
        return;
    }
    snd_pcm16_split_sq((uint32_t *)in, aram, aram + sizeof(left), sizeof(in));
    spu_memread(left, aram, sizeof(left));
    spu_memread(right, aram + sizeof(left), sizeof(right));
    snd_mem_free(aram);
    split_check(r, "pcm16 split to sound RAM", left, right);
}

/* ---- Maple DMA against the cache (KallistiOS#1576) ----
   A maple frame's reply is DMA'd into its receive buffer and read through
   P2. maple_frame_init() clears the buffer through P2 too, but frames live
   in calloc()ed memory zeroed through the cache, so dirty lines can still
   sit over it; one evicted after the DMA lands its zeros on the reply.
   KOS with the fix purges those lines in maple_frame_init().

   This makes the lines dirty the way calloc() does, then a helper thread
   writes 32 KB through the cache (evicting the whole operand cache) at a
   set time after the frame goes out, and the read is checked against a
   known good copy. Read-only: VMU block reads. */

#define DMA_TRIALS 60

static maple_frame_t *dma_frame;
static volatile int dma_delay_us;
static uint8_t dma_flush[32768] __attribute__((aligned(32)));
static semaphore_t dma_go;

static void *dma_evictor(void *unused) {
    sem_wait(&dma_go);
    uint64_t give_up = now_us() + 100000;

    /* The frame is marked sent as the vblank builds the DMA list. */
    while(dma_frame->state != MAPLE_FRAME_SENT && now_us() < give_up)
        ;
    uint64_t t = now_us();
    while(now_us() - t < (uint64_t)dma_delay_us)
        ;
    memset(dma_flush, (int)t, sizeof(dma_flush));
    return NULL;
}

static void step_maple_dma(kt_report_t *r) {
    maple_device_t *card = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    static uint8_t good[512], got[512];
    static const int delays[] = { 200, 400, 800, 1200, 1600, 2400 };
    int corrupt = 0, errors = 0, hit_delays = 0;

    if(!card) {
        report_add(r, KT_SKIP, "Maple DMA vs cache", "no VMU");
        return;
    }
    if(emulated()) {
        report_add(r, KT_SKIP, "Maple DMA vs cache", "Flycast has no cache model");
        return;
    }
    if(vmu_block_read(card, 255, good) != MAPLE_EOK) {
        report_add(r, KT_FAIL, "Maple DMA vs cache", "first block read failed");
        return;
    }
    dma_frame = &card->frame;
    /* The buffer maple_frame_init() uses, through the cache */
    uintptr_t buf = ((uintptr_t)card->frame.recv_buf_arr + 31) & ~(uintptr_t)31;
    uint8_t *p1 = (uint8_t *)((buf & 0x1fffffff) | 0x80000000);

    /* Failed reads would each print an error, a slow syscall under dcload. */
    int level = dbglog_level;
    dbglog_set_level(DBG_CRITICAL);
    sem_init(&dma_go, 0);
    for(size_t d = 0; d < sizeof(delays) / sizeof(delays[0]); d++) {
        int bad_here = 0;
        for(int i = 0; i < DMA_TRIALS / 6; i++) {
            dma_delay_us = delays[d];
            memset(got, 0, sizeof(got));
            /* The helper exists before the lines are dirtied: making a
               thread touches enough memory to evict them early. */
            kthread_t *t = thd_create(false, dma_evictor, NULL);
            thd_pass();
            /* Held so nothing else uses the frame meanwhile. It goes back by
               hand: maple_frame_unlock() asserts the frame got a reply. */
            maple_frame_lock(&card->frame);
            memset(p1, 0, 1024);        /* dirty lines, as calloc() leaves them */
            card->frame.state = MAPLE_FRAME_VACANT;
            sem_signal(&dma_go);
            int rv = vmu_block_read(card, 255, got);
            if(t)
                thd_join(t, NULL);
            if(rv != MAPLE_EOK)
                errors++, bad_here++;
            else if(memcmp(got, good, sizeof(good)))
                corrupt++, bad_here++;
        }
        if(bad_here)
            hit_delays++;
    }
    dbglog_set_level(level);
    sem_destroy(&dma_go);
    report_check(r, !errors && !corrupt, "Maple DMA vs cache",
                 "%d of %d reads hit (%d failed, %d wrong data), %d of 6 delays",
                 errors + corrupt, DMA_TRIALS, errors, corrupt, hit_delays);
}

/* ---- Threads ---- */

static mutex_t lock = MUTEX_INITIALIZER;
static volatile int counter;
static int atomic_counter;
static semaphore_t ping, pong;
static __thread int tls_value;
static volatile int tls_bad;

static void *count_worker(void *arg) {
    for(int i = 0; i < 5000; i++) {
        mutex_lock(&lock);
        counter++;
        mutex_unlock(&lock);
        __atomic_fetch_add(&atomic_counter, 1, __ATOMIC_SEQ_CST);
        if((i & 255) == 0)
            thd_pass();
    }
    tls_value = (int)(uintptr_t)arg;
    thd_pass();
    if(tls_value != (int)(uintptr_t)arg)
        tls_bad++;
    return (void *)((uintptr_t)arg * 10);
}

static void *pong_worker(void *arg) {
    for(int i = 0; i < 500; i++) {
        sem_wait(&ping);
        sem_signal(&pong);
    }
    return NULL;
}

static mutex_t cv_lock = MUTEX_INITIALIZER;
static condvar_t cv;
static volatile int cv_items, cv_seen;

static void *consumer(void *arg) {
    mutex_lock(&cv_lock);
    while(cv_seen < 50) {
        while(!cv_items)
            cond_wait(&cv, &cv_lock);
        cv_items--;
        cv_seen++;
    }
    mutex_unlock(&cv_lock);
    return NULL;
}

static void *pt_worker(void *arg) {
    return (void *)((uintptr_t)arg + 1);
}

/* New threads take their creator's FPU modes (KallistiOS#1519). They used
   to start with FPSCR.DN the other way round, so a denormal result trapped
   and KOS skipped the instruction. */
static volatile float f_tiny = 1.5e-38f;
static uint32_t fpu_seen[2];

static void *fpu_worker(void *unused) {
    uint32_t fpscr;

    __asm__ volatile("sts fpscr,%0" : "=r"(fpscr));
    fpu_seen[0] = fpscr;
    fpu_seen[1] = fbits(f_tiny * 0.5f);
    return NULL;
}

static void step_threads(kt_report_t *r) {
    kthread_t *t[4];
    void *ret;
    int ret_ok = 0;

    counter = 0;
    atomic_counter = 0;
    tls_bad = 0;
    tls_value = -1;
    uint64_t us = now_us();
    for(int i = 0; i < 4; i++)
        t[i] = thd_create(false, count_worker, (void *)(uintptr_t)(i + 1));
    for(int i = 0; i < 4; i++)
        if(t[i] && thd_join(t[i], &ret) == 0 && (uintptr_t)ret == (uintptr_t)(i + 1) * 10)
            ret_ok++;
    us = now_us() - us;
    report_check(r, counter == 20000, "Mutex counter", "%d of 20000 in %lu ms", counter,
                 (unsigned long)(us / 1000));
    report_check(r, atomic_counter == 20000, "Atomic add (gUSA)", "%d of 20000", atomic_counter);
    report_check(r, ret_ok == 4, "thd_join values", "%d of 4 threads returned theirs", ret_ok);
    report_check(r, !tls_bad && tls_value == -1, "Thread-local storage", "%s",
                 tls_bad ? "a thread saw another's value" : "each thread kept its own");

    sem_init(&ping, 0);
    sem_init(&pong, 0);
    kthread_t *pt = thd_create(false, pong_worker, NULL);
    int rounds = 0;
    us = now_us();
    for(int i = 0; i < 500; i++) {
        sem_signal(&ping);
        if(sem_wait_timed(&pong, 1000) == 0)
            rounds++;
    }
    us = now_us() - us;
    thd_join(pt, NULL);
    sem_destroy(&ping);
    sem_destroy(&pong);
    report_check(r, rounds == 500, "Semaphore ping-pong", "%d of 500 rounds, %lu us each",
                 rounds, (unsigned long)(us / 500));

    cond_init(&cv);
    cv_items = cv_seen = 0;
    kthread_t *ct = thd_create(false, consumer, NULL);
    for(int i = 0; i < 50; i++) {
        mutex_lock(&cv_lock);
        cv_items++;
        cond_signal(&cv);
        mutex_unlock(&cv_lock);
        if(i % 8 == 0)
            thd_pass();
    }
    thd_join(ct, NULL);
    cond_destroy(&cv);
    report_check(r, cv_seen == 50, "Condition variable", "consumer took %d of 50", cv_seen);

    pthread_t p;
    void *pr = NULL;
    int prv = pthread_create(&p, NULL, pt_worker, (void *)41);
    if(prv == 0)
        pthread_join(p, &pr);
    report_check(r, prv == 0 && (uintptr_t)pr == 42, "pthreads", "create and join %s",
                 prv ? "failed" : (uintptr_t)pr == 42 ? "ok" : "lost the value");

    /* Rounding, exception enables, DN, PR, SZ and FR; not the sticky flags */
    const uint32_t modes = 0x003c0f83;
    uint32_t fpscr, tiny;
    __asm__ volatile("sts fpscr,%0" : "=r"(fpscr));
    tiny = fbits(f_tiny * 0.5f);
    memset(fpu_seen, 0xff, sizeof(fpu_seen));
    kthread_t *ft = thd_create(false, fpu_worker, NULL);
    if(ft)
        thd_join(ft, NULL);
    report_check(r, ft && (fpu_seen[0] & modes) == (fpscr & modes) && fpu_seen[1] == tiny,
                 "FPSCR in a new thread", "%08lx (main %08lx), denormal %08lx (main %08lx)",
                 (unsigned long)fpu_seen[0], (unsigned long)fpscr, (unsigned long)fpu_seen[1],
                 (unsigned long)tiny);
}

/* ---- libc and libm ---- */

static int cmp_int(const void *a, const void *b) {
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

static jmp_buf jump;

static void jump_back(int v) {
    longjmp(jump, v);
}

static void step_libc(kt_report_t *r) {
    chk_t c = { 0 };
    static uint8_t a[160], b[160], ref[160];
    char buf[96];

    /* memcpy at every alignment and many lengths */
    for(int so = 0; so < 8; so++)
        for(int dof = 0; dof < 8; dof++)
            for(int len = 0; len < 72; len += 7) {
                for(int i = 0; i < 160; i++) { a[i] = (uint8_t)(i * 7 + so); b[i] = ref[i] = 0xee; }
                memcpy(b + dof, a + so, len);
                for(int i = 0; i < len; i++) ref[dof + i] = a[so + i];
                chk(&c, !memcmp(b, ref, 160), "memcpy so=%d do=%d len=%d", so, dof, len);
            }
    for(int i = 0; i < 160; i++) a[i] = (uint8_t)i;
    memmove(a + 3, a, 100);
    chk(&c, a[3] == 0 && a[102] == 99, "memmove forward overlap");
    for(int i = 0; i < 160; i++) a[i] = (uint8_t)i;
    memmove(a, a + 5, 100);
    chk(&c, a[0] == 5 && a[99] == 104, "memmove backward overlap");
    for(int i = 0; i < 160; i++) a[i] = (uint8_t)i;
    memset(a + 1, 0x5a, 77);
    chk(&c, a[0] == 0 && a[1] == 0x5a && a[77] == 0x5a && a[78] == 78, "memset bounds");

    chk(&c, strlen("dreamcast") == 9, "strlen");
    chk(&c, strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && !strcmp("x", "x"), "strcmp");
    chk(&c, !strncmp("kallistios", "kallisti", 8), "strncmp");
    chk(&c, strtol("-0x1f", NULL, 16) == -31 && strtoul("4294967295", NULL, 10) == 4294967295u,
        "strtol/strtoul");
    chk(&c, fabs(strtod("6.02e23", NULL) - 6.02e23) < 1e17, "strtod");
    chk(&c, atoi("  -1234x") == -1234, "atoi");
    snprintf(buf, sizeof(buf), "%d|%u|%x|%08.3f|%.2e|%s|%c|%%|%lld", -42, 3000000000u, 0xbeef,
             3.14159, 12345.678, "sh4", 'Z', -9000000000ll);
    chk(&c, !strcmp(buf, "-42|3000000000|beef|0003.142|1.23e+04|sh4|Z|%|-9000000000"),
        "snprintf: %.40s", buf);
    int x = 0, y = 0;
    char word[16];
    chk(&c, sscanf("12 -7 kos", "%d %d %15s", &x, &y, word) == 3 && x == 12 && y == -7 &&
        !strcmp(word, "kos"), "sscanf");

    int arr[200];
    uint32_t seed = 7;
    for(int i = 0; i < 200; i++) arr[i] = (int)(rng_next(&seed) % 1000) - 500;
    qsort(arr, 200, sizeof(int), cmp_int);
    bool sorted = true;
    for(int i = 1; i < 200; i++) if(arr[i - 1] > arr[i]) sorted = false;
    chk(&c, sorted, "qsort");

    volatile int stage = 0;
    int v = setjmp(jump);
    if(!v) {
        stage = 1;
        jump_back(77);
        stage = 2;
    }
    chk(&c, v == 77 && stage == 1, "setjmp/longjmp");
    chk_done(r, "C library", &c);
}

static void step_libm(kt_report_t *r) {
    chk_t c = { 0 };
    volatile float one = 1.0f, two = 2.0f, ten = 10.0f;
    volatile double done = 1.0, dtwo = 2.0;

    chkf(&c, "sinf(1)", sinf(one), 0.841470985f, 1e-6f);
    chkf(&c, "cosf(1)", cosf(one), 0.540302306f, 1e-6f);
    chkf(&c, "tanf(1)", tanf(one), 1.557407725f, 2e-6f);
    chkf(&c, "expf(1)", expf(one), 2.718281828f, 1e-6f);
    chkf(&c, "logf(10)", logf(ten), 2.302585093f, 1e-6f);
    chkf(&c, "powf(2,10)", powf(two, ten), 1024.0f, 0);
    chkf(&c, "atan2f(1,1)", atan2f(one, one), (float)M_PI / 4, 1e-6f);
    chkf(&c, "fmodf", fmodf(ten, 3.0f), 1.0f, 0);
    chkf(&c, "floorf", floorf(-2.5f * one), -3.0f, 0);
    chkf(&c, "ceilf", ceilf(-2.5f * one), -2.0f, 0);
    chkf(&c, "roundf", roundf(2.5f * one), 3.0f, 0);
    chk(&c, fabs(sin(done) - 0.8414709848078965) < 1e-15, "sin(1) double");
    chk(&c, fabs(exp(done) - 2.718281828459045) < 1e-15, "exp(1) double");
    chk(&c, fabs(log(dtwo) - 0.6931471805599453) < 1e-15, "log(2) double");
    chk(&c, fabs(pow(dtwo, 0.5) - 1.4142135623730951) < 1e-15, "pow(2,.5) double");
    chk(&c, isnan(sqrtf(-one)), "sqrtf(-1) not NaN");
    chk_done(r, "Math library", &c);
}

static void step_misc(kt_report_t *r) {
    chk_t c = { 0 };
    union { uint32_t w; uint8_t b[4]; } e = { .w = 0x01020304 };

    chk(&c, e.b[0] == 4, "not little-endian");
    chk(&c, sizeof(int) == 4 && sizeof(long) == 4 && sizeof(void *) == 4, "int/long/pointer size");
    chk(&c, sizeof(long long) == 8 && sizeof(double) == 8 && sizeof(float) == 4,
        "long long/double size");
    chk(&c, ((uintptr_t)__builtin_frame_address(0) & 3) == 0, "stack misaligned");
    uint8_t raw[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t un;
    memcpy(&un, raw + 1, 4);
    chk32(&c, "unaligned memcpy", un, 0x05040302);
    chk_done(r, "Data layout", &c);
}

const kt_step_t cpu_steps[] = {
    { "Integer ALU", step_alu },
    { "Multiply and divide", step_muldiv },
    { "Registers", step_regs },
    { "FPU", step_fpu },
    { "FPU vector instructions", step_vector },
    { "Exceptions", step_exceptions },
    { "Caches, SQ, DMA", step_cache },
    { "Stereo sample split", step_split },
    { "Maple DMA vs cache", step_maple_dma },
    { "Threads", step_threads },
    { "C library", step_libc },
    { "Math library", step_libm },
    { "Data layout", step_misc },
};
const int cpu_step_count = sizeof(cpu_steps) / sizeof(cpu_steps[0]);

static kt_report_t cpu_report;

void cpu_suite_run(void) {
    runner_start("CPU TEST SUITE", cpu_steps, cpu_step_count, &cpu_report, NULL);
}
