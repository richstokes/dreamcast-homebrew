! SH-4 routines for the CPU test suite, where C cannot say which registers
! or instructions are used.

    .text
    .align 2

! void kt_regfile(const uint32_t pat[13], uint32_t out[13])
! Loads r0-r3 and r6-r14 from pat, holds them a few cycles, stores them to
! out. r4 and r5 carry the pointers throughout.
    .globl _kt_regfile
_kt_regfile:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    mov.l   @(0,r4),r0
    mov.l   @(4,r4),r1
    mov.l   @(8,r4),r2
    mov.l   @(12,r4),r3
    mov.l   @(16,r4),r6
    mov.l   @(20,r4),r7
    mov.l   @(24,r4),r8
    mov.l   @(28,r4),r9
    mov.l   @(32,r4),r10
    mov.l   @(36,r4),r11
    mov.l   @(40,r4),r12
    mov.l   @(44,r4),r13
    mov.l   @(48,r4),r14
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    mov.l   r0,@(0,r5)
    mov.l   r1,@(4,r5)
    mov.l   r2,@(8,r5)
    mov.l   r3,@(12,r5)
    mov.l   r6,@(16,r5)
    mov.l   r7,@(20,r5)
    mov.l   r8,@(24,r5)
    mov.l   r9,@(28,r5)
    mov.l   r10,@(32,r5)
    mov.l   r11,@(36,r5)
    mov.l   r12,@(40,r5)
    mov.l   r13,@(44,r5)
    mov.l   r14,@(48,r5)
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

! uint32_t kt_div1_u32(uint32_t n, uint32_t d)
! Unsigned 32/32 division by 32 DIV1 steps (the SH-4 manual's 64/32
! sequence with a zero upper half). The caller rules out d == 0.
    .globl _kt_div1_u32
_kt_div1_u32:
    mov     #0,r1
    div0u
    .rept 32
    rotcl   r4
    div1    r5,r1
    .endr
    rotcl   r4
    rts
    mov     r4,r0

! void kt_fpu_banks(const float in[16], float out[16])
! Writes in to the back bank (XF0-XF15) with FRCHG, then reads the back bank
! out again. Call with interrupts off. Leaves the front bank alone.
    .globl _kt_fpu_banks
_kt_fpu_banks:
    frchg
    fmov.s  @r4+,fr0
    fmov.s  @r4+,fr1
    fmov.s  @r4+,fr2
    fmov.s  @r4+,fr3
    fmov.s  @r4+,fr4
    fmov.s  @r4+,fr5
    fmov.s  @r4+,fr6
    fmov.s  @r4+,fr7
    fmov.s  @r4+,fr8
    fmov.s  @r4+,fr9
    fmov.s  @r4+,fr10
    fmov.s  @r4+,fr11
    fmov.s  @r4+,fr12
    fmov.s  @r4+,fr13
    fmov.s  @r4+,fr14
    fmov.s  @r4+,fr15
    frchg
    nop
    frchg
    add     #64,r5
    fmov.s  fr15,@-r5
    fmov.s  fr14,@-r5
    fmov.s  fr13,@-r5
    fmov.s  fr12,@-r5
    fmov.s  fr11,@-r5
    fmov.s  fr10,@-r5
    fmov.s  fr9,@-r5
    fmov.s  fr8,@-r5
    fmov.s  fr7,@-r5
    fmov.s  fr6,@-r5
    fmov.s  fr5,@-r5
    fmov.s  fr4,@-r5
    fmov.s  fr3,@-r5
    fmov.s  fr2,@-r5
    fmov.s  fr1,@-r5
    fmov.s  fr0,@-r5
    rts
    frchg

! uint32_t kt_illegal(void)
! Executes an undefined opcode. The test's exception handler steps over it
! and sets r0; without a handler it would panic.
    .globl _kt_illegal
_kt_illegal:
    mov     #0,r0
    .word   0xfffd
    rts
    nop

! uint32_t kt_unaligned(const void *p)
! A 32-bit read from p (which the test makes misaligned). The address
! error handler steps over it.
    .globl _kt_unaligned
_kt_unaligned:
    mov     #0,r0
    mov.l   @r4,r0
    rts
    nop
