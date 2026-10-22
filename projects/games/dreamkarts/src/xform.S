/* DreamKarts' hot vertex paths in SH-4 assembly (called from src/render.c).

   These replace C loops whose results they must match bit for bit: every
   floating-point operation is the one GCC emitted for the C it replaces, in
   the same order, so the Tile Accelerator receives the same words.

   dk_xf_stage   transform RVs with XMTRX (FTRV), compute each vertex's
                 clip outcode and its projected PVR vertex (FSRRA), into a
                 32-byte aligned staging buffer in the operand cache.
   dk_sq_run     copy staged vertices to the TA through the store queues,
                 64 bits at a time, the last one flagged end of strip.
   dk_xf_emit    transform, project and send a strip straight to the TA
                 (one known to need no clipping or culling).
   dk_sq_tris    an indexed triangle list from staged vertices: skip
                 triangles outside one clip plane, cull back faces, send the
                 rest; stop at a triangle that needs near-plane clipping.
   dk_xf_pos     dk_xf_stage for a kart mesh's distinct positions only.
   dk_sq_strips  a kart mesh wholly inside the clip planes, as strips:
                 back faces tested, runs of front faces sent as strips.

   Each has a C reference in render.c's -DDK_XF_CHECK build, which runs the
   C beside the assembly and counts any word that differs: change the C
   there first, then the assembly to match.

   Calling convention: arguments in r4-r7, r0-r7 and fr0-fr11 are
   scratch, r8-r14 and fr12-fr15 are preserved. FPSCR keeps PR = 0 (single
   precision); SZ is switched to 1 only around the 64-bit copies.

   Measured on the console (2026-10-19) and not worth their code: software
   pipelining these loops (under 0.1 ms a frame), pref at any distance (no
   change: streaming vertices from RAM costs about 20 cycles a 32-byte line
   however it is done), placing the staging buffer to dodge cache conflicts
   (nothing), lighting in assembly (no faster than the per-group C) and the
   4x4 matrix product in assembly (about 0.1 ms). On 2026-10-21, letting
   the PVR cull kart back faces (whole strips sent, no area tests) saved
   another 0.3-0.4 ms of CPU but cost 0.1-0.5 ms of render time. */

#define VTX_FLAGS 0xe0000000    /* PVR_CMD_VERTEX */
#define EOL_FLAGS 0xf0000000    /* PVR_CMD_VERTEX_EOL */

    .text

/* void dk_sq_run(const pvr_vertex_t *src, int n)

   Send n >= 1 consecutive staged vertices as one strip: each 32 bytes goes
   to the store queue that pvr_dr_target() would give, the last with the
   end-of-strip flag. */
    .balign 32
    .global _dk_sq_run
_dk_sq_run:
    mov.l   .Ldr_addr,r1
    mov.l   @r1,r2              /* pvr_dr_addr */
    mov     #32,r3
    mov.l   .Leol_flags,r7
    fschg                       /* SZ = 1: fmov moves 64 bits */
.Lrun_loop:
    xor     r3,r2
    fmov    @r4+,dr0
    fmov    @r4+,dr2
    fmov    @r4+,dr4
    fmov    @r4+,dr6
    mov     r2,r6
    add     #32,r6
    fmov    dr6,@-r6
    fmov    dr4,@-r6
    fmov    dr2,@-r6
    dt      r5
    bt.s    .Lrun_last
    fmov    dr0,@-r6
    bra     .Lrun_loop
    pref    @r2
.Lrun_last:
    mov.l   r7,@r6              /* end of strip */
    pref    @r2
    fschg
    rts
    mov.l   r2,@r1

    .balign 4
.Ldr_addr:
    .long   _pvr_dr_addr
.Leol_flags:
    .long   EOL_FLAGS

/* int dk_sq_tris(MeshSubmit *ms, const uint16_t *idx, int ntri, int cull)

   ms: {const pvr_vertex_t *pv; const uint8_t *codes; uint32_t clip_mask;
        uint32_t sent, back}. For each of ntri >= 1 triangles (a, b, c):
     skip it if codes a & b & c != 0 (outside one plane);
     stop if (a | b | c) & clip_mask (the caller clips it);
     with cull, skip it unless its screen area is positive (counted in back);
     send pv[a], pv[b], pv[c] as one strip (counted in sent).
   Returns the number of triangles handled before the one it stopped at. */

/* Send the 32-byte vertex at \src (advanced past it) through the next
   store queue, as pvr_dr_target()/pvr_dr_commit() would: r7 holds
   pvr_dr_addr, \k32 the constant 32; \eol: flag it end of strip (flags in
   \eolr). Uses r4 and, with FPSCR.SZ = 1, fr0-fr7. */
.macro SQ_VERTEX src, eol, k32, eolr
    xor     \k32,r7
    fmov    @\src+,dr0
    fmov    @\src+,dr2
    fmov    @\src+,dr4
    fmov    @\src+,dr6
    mov     r7,r4
    add     #32,r4
    fmov    dr6,@-r4
    fmov    dr4,@-r4
    fmov    dr2,@-r4
    fmov    dr0,@-r4
.if \eol
    mov.l   \eolr,@r4
.endif
    pref    @r7
.endm

.macro TRI_LOOP cull, label
\label\()_loop:
    mov.w   @r5+,r1
    mov.w   @r5+,r2
    mov.w   @r5+,r3
    mov     r1,r0
    mov.b   @(r0,r9),r4         /* code a */
    mov     r2,r0
    mov.b   @(r0,r9),r6         /* code b */
    mov     r3,r0
    mov.b   @(r0,r9),r0         /* code c */
    mov     r4,r14
    or      r6,r14
    or      r0,r14              /* a | b | c */
    and     r6,r4
    tst     r0,r4               /* a & b & c */
    bf      \label\()_next
    tst     r10,r14
    bf      .Ltri_exit          /* needs clipping */
    mov     #5,r0
    shld    r0,r1
    shld    r0,r2
    shld    r0,r3
    add     r8,r1
    add     r8,r2
    add     r8,r3
.if \cull
    /* area = (bx - ax)(cy - ay) + (by - ay)(ax - cx), as GCC computes
       (bx - ax)(cy - ay) - (by - ay)(cx - ax) */
    mov     r1,r0
    add     #4,r0
    fmov.s  @r0+,fr2            /* ax */
    fmov.s  @r0,fr4             /* ay */
    mov     r2,r0
    add     #4,r0
    fmov.s  @r0+,fr0            /* bx */
    fmov.s  @r0,fr1             /* by */
    mov     r3,r0
    add     #4,r0
    fmov.s  @r0+,fr5            /* cx */
    fmov.s  @r0,fr3             /* cy */
    fsub    fr4,fr1             /* by - ay */
    fsub    fr4,fr3             /* cy - ay */
    fsub    fr2,fr0             /* bx - ax */
    fsub    fr5,fr2             /* ax - cx */
    fmul    fr2,fr1
    fldi0   fr2
    fmac    fr0,fr3,fr1
    fcmp/gt fr2,fr1             /* area > 0 */
    bt      \label\()_send
    bra     \label\()_next
    add     #1,r12              /* back face */
.endif
\label\()_send:
    mov.l   .Leol_flags2,r6
    mov     #32,r0
    fschg
    SQ_VERTEX r1, 0, r0, r6
    SQ_VERTEX r2, 0, r0, r6
    SQ_VERTEX r3, 1, r0, r6
    fschg
    add     #1,r11
\label\()_next:
    dt      r13
    bf      \label\()_loop
    bra     .Ltri_exit
    nop
.endm

    .balign 32
    .global _dk_sq_tris
_dk_sq_tris:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    mov.l   r4,@-r15            /* ms */
    mov.l   r6,@-r15            /* ntri */
    mov.l   @r4+,r8             /* pv */
    mov.l   @r4+,r9             /* codes */
    mov.l   @r4+,r10            /* clip_mask */
    mov.l   @r4+,r11            /* sent */
    mov.l   @r4,r12             /* back */
    mov     r6,r13              /* triangles left */
    mov.l   .Ldr_addr2,r0
    tst     r7,r7
    bt.s    .Ltri_nocull_loop
    mov.l   @r0,r7              /* pvr_dr_addr */
    TRI_LOOP 1, .Ltri_cull
    TRI_LOOP 0, .Ltri_nocull
.Ltri_exit:
    mov.l   .Ldr_addr2,r0
    mov.l   r7,@r0
    mov.l   @r15+,r0            /* ntri */
    sub     r13,r0              /* handled */
    mov.l   @r15+,r4            /* ms */
    mov.l   r11,@(12,r4)
    mov.l   r12,@(16,r4)
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

    .balign 4
.Ldr_addr2:
    .long   _pvr_dr_addr
.Leol_flags2:
    .long   EOL_FLAGS

/* uint32_t dk_xf_stage(const RV *v, int n, pvr_vertex_t *out, const XfParams *p)

   For each of n >= 1 vertices (x, y, z, u, v, argb; 24 bytes):
     (X, Y, -, W) = XMTRX * (x, y, z, 1)
     code = 16 (W < near) | 8 (Y > y1 W) | 4 (Y < y0 W) | 2 (X > 640 W) | 1 (X < 0)
     inv = fsrra(W * W)
     out = {VERTEX, X inv, Y inv, inv, u, v, argb, 0}
   codes go to p->codes[i]. Returns the AND of the codes | their OR << 8.
   p: {float near_w, xmax, y0, y1; uint8_t *codes; const uint32_t *col}.

   uint32_t dk_xf_stage_kv(const KV *v, int n, pvr_vertex_t *out, const XfParams *p)

   The same for kart vertices (x, y, z, group; 16 bytes), with u = v = 0
   and vertex i coloured p->col[v[i].group]. */


/* For each vertex: FTRV, the five outcode tests (each FCMP's T rotated
   into the code), FSRRA for 1/W, then the 32-byte PVR vertex written
   backwards from its last word. \kv: a KV (x, y, z, group) whose colour is
   the group's in p->col, with u = v = 0; else an RV. */
.macro XF_STAGE kv, label
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    fmov.s  @r7+,fr12           /* near_w */
    fmov.s  @r7+,fr13           /* 640 */
    fmov.s  @r7+,fr14           /* y0 */
    fmov.s  @r7+,fr15           /* y1 */
    mov.l   @r7+,r3             /* codes */
.if \kv
    mov.l   @r7,r7              /* colours, by group */
.endif
    fldi0   fr6
    mov     #0,r0
    mov.l   \label\()_flags,r1
    mov     #31,r8              /* AND of the codes */
    mov     #0,r9               /* OR of the codes */
    add     #28,r6              /* r6 -> the vertex's last word */
\label\()_loop:
    fmov.s  @r4+,fr8
    fmov.s  @r4+,fr9
    fmov.s  @r4+,fr10
    fldi1   fr11
    ftrv    xmtrx,fv8
.if \kv
    mov.w   @r4+,r12            /* group */
    mov     #0,r10              /* u */
    mov     #0,r11              /* v */
    extu.w  r12,r12
    shll2   r12
    add     r7,r12
    mov.l   @r12,r12            /* argb */
    add     #2,r4
.else
    mov.l   @r4+,r10            /* u */
    mov.l   @r4+,r11            /* v */
    mov.l   @r4+,r12            /* argb */
.endif
    mov     #0,r13
    movca.l r0,@r6              /* oargb = 0; claims the line without a read */
    mov.l   r12,@-r6
    mov.l   r11,@-r6
    mov.l   r10,@-r6
    fmov    fr11,fr0
    fmul    fr0,fr0             /* W * W */
    fmov    fr13,fr1
    fmul    fr11,fr1            /* 640 W */
    fmov    fr14,fr2
    fmul    fr11,fr2            /* y0 W */
    fmov    fr15,fr3
    fmul    fr11,fr3            /* y1 W */
    fsrra   fr0                 /* inv = 1 / |W| */
    fcmp/gt fr11,fr12           /* near > W */
    rotcl   r13
    fcmp/gt fr3,fr9             /* Y > y1 W */
    rotcl   r13
    fcmp/gt fr9,fr2             /* y0 W > Y */
    rotcl   r13
    fcmp/gt fr1,fr8             /* X > 640 W */
    rotcl   r13
    fcmp/gt fr8,fr6             /* 0 > X */
    rotcl   r13
    fmul    fr0,fr8
    fmul    fr0,fr9
    and     r13,r8
    or      r13,r9
    mov.b   r13,@r3
    add     #1,r3
    fmov.s  fr0,@-r6            /* z = inv */
    fmov.s  fr9,@-r6
    fmov.s  fr8,@-r6
    mov.l   r1,@-r6             /* flags */
    dt      r5
    bf.s    \label\()_loop
    add     #60,r6              /* next vertex's last word */
    mov     r9,r0
    shll8   r0
    or      r8,r0
    fmov.s  @r15+,fr15
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8
    .balign 4
\label\()_flags:
    .long   VTX_FLAGS
.endm

    .balign 32
    .global _dk_xf_stage
_dk_xf_stage:
    XF_STAGE 0, .Lxs

    .balign 32
    .global _dk_xf_stage_kv
_dk_xf_stage_kv:
    XF_STAGE 1, .Lxskv

/* void dk_xf_emit(const RV *v, int n)

   A strip known to need no clipping or culling, straight to the TA: for
   each of n >= 1 vertices the same transform and projection as
   dk_xf_stage, written through the next store queue, the last vertex
   flagged end of strip. */
    .balign 32
    .global _dk_xf_emit
_dk_xf_emit:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   .Ldr_addr3,r1
    mov.l   @r1,r2              /* pvr_dr_addr */
    mov.l   .Lvtx_flags3,r3
    mov     #0,r0
    mov     #32,r7
.Lemit_loop:
    fmov.s  @r4+,fr8
    fmov.s  @r4+,fr9
    fmov.s  @r4+,fr10
    fldi1   fr11
    ftrv    xmtrx,fv8
    mov.l   @r4+,r8             /* u */
    mov.l   @r4+,r9             /* v */
    mov.l   @r4+,r10            /* argb */
    xor     r7,r2               /* next store queue */
    mov     r2,r11
    add     #32,r11
    mov.l   r0,@-r11            /* oargb */
    mov.l   r10,@-r11
    mov.l   r9,@-r11
    mov.l   r8,@-r11
    fmov    fr11,fr4
    fmul    fr4,fr4             /* W * W */
    fsrra   fr4                 /* inv */
    fmul    fr4,fr8
    fmul    fr4,fr9
    fmov.s  fr4,@-r11           /* z = inv */
    fmov.s  fr9,@-r11
    fmov.s  fr8,@-r11
    dt      r5
    bt      .Lemit_last
    mov.l   r3,@-r11            /* flags */
    bra     .Lemit_loop
    pref    @r2
.Lemit_last:
    mov.l   .Leol_flags3,r3
    mov.l   r3,@-r11
    pref    @r2
    mov.l   r2,@r1
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

    .balign 4
.Ldr_addr3:
    .long   _pvr_dr_addr
.Lvtx_flags3:
    .long   VTX_FLAGS
.Leol_flags3:
    .long   EOL_FLAGS

/* uint32_t dk_xf_pos(const float (*pos)[4], int n, float (*out)[4], const XfParams *p)

   dk_xf_stage for a kart mesh's distinct positions {x, y, z, -}: for each
   of n >= 1, the same transform, outcode and projection, out = {X inv,
   Y inv, inv, (unwritten)}. Returns the AND of the codes | their OR << 8. */
    .balign 32
    .global _dk_xf_pos
_dk_xf_pos:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    fmov.s  @r7+,fr12           /* near_w */
    fmov.s  @r7+,fr13           /* 640 */
    fmov.s  @r7+,fr14           /* y0 */
    fmov.s  @r7+,fr15           /* y1 */
    fldi0   fr6
    mov     #31,r8              /* AND of the codes */
    mov     #0,r9               /* OR of the codes */
    add     #12,r6              /* r6 -> past the record's inv */
.Lxp_loop:
    fmov.s  @r4+,fr8
    fmov.s  @r4+,fr9
    fmov.s  @r4+,fr10
    fldi1   fr11
    ftrv    xmtrx,fv8
    add     #4,r4
    mov     #0,r1
    fmov    fr11,fr0
    fmul    fr0,fr0             /* W * W */
    fmov    fr13,fr1
    fmul    fr11,fr1            /* 640 W */
    fmov    fr14,fr2
    fmul    fr11,fr2            /* y0 W */
    fmov    fr15,fr3
    fmul    fr11,fr3            /* y1 W */
    fsrra   fr0                 /* inv = 1 / |W| */
    fcmp/gt fr11,fr12           /* near > W */
    rotcl   r1
    fcmp/gt fr3,fr9             /* Y > y1 W */
    rotcl   r1
    fcmp/gt fr9,fr2             /* y0 W > Y */
    rotcl   r1
    fcmp/gt fr1,fr8             /* X > 640 W */
    rotcl   r1
    fcmp/gt fr8,fr6             /* 0 > X */
    rotcl   r1
    fmul    fr0,fr8
    fmul    fr0,fr9
    and     r1,r8
    or      r1,r9
    fmov.s  fr0,@-r6
    fmov.s  fr9,@-r6
    fmov.s  fr8,@-r6
    dt      r5
    bf.s    .Lxp_loop
    add     #28,r6              /* past the next record's inv */
    mov     r9,r0
    shll8   r0
    or      r8,r0
    fmov.s  @r15+,fr15
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

/* void dk_sq_strips(StripSubmit *ss, const uint16_t *strips)

   ss: {const float (*pos)[4] (from dk_xf_pos); const uint32_t *col;
        uint32_t sent, back}. A kart mesh wholly inside the clip planes,
   as strips (format in render.h, at most 32 triangles each). Each strip's
   triangles are tested first, area > 0 as in dk_sq_tris (else counted in
   ss->back), into a mask; then each run of consecutive front faces goes
   to the TA as one strip (counted in ss->sent), each vertex built from
   its position and colour, {VERTEX, x, y, inv, 0, 0, col, 0}, the run's
   last flagged end of strip. */

/* T = area of (\a, \b, \c) > 0; each points at a position's x, r0 = 4. */
.macro STRIP_AREA a, b, c
    fmov.s  @\a,fr2             /* ax */
    fmov.s  @(r0,\a),fr4        /* ay */
    fmov.s  @\b,fr0             /* bx */
    fmov.s  @(r0,\b),fr1        /* by */
    fmov.s  @\c,fr5             /* cx */
    fmov.s  @(r0,\c),fr3        /* cy */
    fsub    fr4,fr1
    fsub    fr4,fr3
    fsub    fr2,fr0
    fsub    fr5,fr2
    fmul    fr2,fr1
    fmac    fr0,fr3,fr1
    fcmp/gt fr6,fr1
.endm

/* Send the strip vertex at @(\disp,r14) (position offset, colour offset)
   through the next store queue. r8 = positions, r9 = colours, r7 =
   pvr_dr_addr, r10 = 32, fr6 = 0, fr7 = VERTEX flags, fr11 = EOL flags. */
.macro STRIP_SEND disp, eol
    mov.w   @(\disp+2,r14),r0
    extu.w  r0,r0
    mov.l   @(r0,r9),r2         /* argb */
    mov.w   @(\disp,r14),r0
    extu.w  r0,r3
    add     r8,r3
    fmov.s  @r3+,fr8
    fmov.s  @r3+,fr9
    fmov.s  @r3,fr10
    xor     r10,r7
    mov     r7,r4
    add     #32,r4
    fmov.s  fr6,@-r4            /* oargb */
    mov.l   r2,@-r4
    fmov.s  fr6,@-r4            /* v */
    fmov.s  fr6,@-r4            /* u */
    fmov.s  fr10,@-r4           /* z = inv */
    fmov.s  fr9,@-r4
    fmov.s  fr8,@-r4
.if \eol
    fmov.s  fr11,@-r4
.else
    fmov.s  fr7,@-r4
.endif
    pref    @r7
.endm

    .balign 32
    .global _dk_sq_strips
_dk_sq_strips:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    mov.l   r4,@-r15            /* ss */
    mov.l   @r4+,r8             /* positions */
    mov.l   @r4+,r9             /* colours */
    mov.l   @r4+,r11            /* sent */
    mov.l   @r4,r12             /* back */
    mov.l   .Lsc_dr,r0
    mov.l   @r0,r7              /* pvr_dr_addr */
    mov     #32,r10
    mova    .Lsc_flags,r0
    fmov.s  @r0+,fr7            /* VERTEX */
    fmov.s  @r0,fr11            /* EOL */
.Lsc_strip:
    mov.w   @r5+,r6             /* n */
    extu.w  r6,r6
    tst     r6,r6
    bf      .Lsc_go
    bra     .Lsc_done
    nop
.Lsc_go:
    mov     r5,r14              /* s */
    mov     r6,r0
    shll2   r0
    add     r0,r5               /* the next strip */
    add     #-2,r6              /* m triangles */
    add     r6,r12              /* back += m, less the front faces below */
    /* Pass 1: the mask, triangle 0 in bit m - 1 (rotcl). */
    mov.w   @r14+,r1
    extu.w  r1,r1
    add     r8,r1
    add     #2,r14
    mov.w   @r14+,r2
    extu.w  r2,r2
    add     r8,r2
    add     #2,r14
    mov     r6,r13              /* triangles left */
    mov     #0,r4               /* mask */
    fldi0   fr6
    mov     #4,r0
.Lsc_even:
    mov.w   @r14+,r3
    extu.w  r3,r3
    add     r8,r3
    STRIP_AREA r1, r2, r3
    rotcl   r4
    add     #2,r14
    mov     r2,r1
    dt      r13
    bt.s    .Lsc_tested
    mov     r3,r2
    mov.w   @r14+,r3
    extu.w  r3,r3
    add     r8,r3
    STRIP_AREA r2, r1, r3
    rotcl   r4
    add     #2,r14
    mov     r2,r1
    dt      r13
    bf.s    .Lsc_even
    mov     r3,r2
.Lsc_tested:
    /* Front faces to the top: triangle k in bit 31 after k shifts. */
    mov     #32,r0
    sub     r6,r0
    shld    r0,r4
    mov     r4,r13
    mov     r6,r0
    shll2   r0
    add     #8,r0
    sub     r0,r14              /* back to s */
    mov     #0,r1               /* a run is open */
.Lsc_emit:
    shll    r13                 /* T = triangle k is a front face */
    bf      .Lsc_back
    add     #1,r11
    add     #-1,r12
    tst     r1,r1
    bf      .Lsc_third
    STRIP_SEND 0, 0
    STRIP_SEND 4, 0
    mov     #1,r1
.Lsc_third:
    cmp/pz  r13                 /* T: triangle k + 1 is not a front face */
    bf      .Lsc_more
    STRIP_SEND 8, 1
    bra     .Lsc_next
    mov     #0,r1
.Lsc_more:
    STRIP_SEND 8, 0
    bra     .Lsc_next
    nop
.Lsc_back:
    mov     #0,r1
.Lsc_next:
    dt      r6
    bf.s    .Lsc_emit
    add     #4,r14
    bra     .Lsc_strip
    nop
.Lsc_done:
    mov.l   .Lsc_dr,r0
    mov.l   r7,@r0
    mov.l   @r15+,r4
    mov.l   r11,@(8,r4)
    mov.l   r12,@(12,r4)
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

    .balign 4
.Lsc_dr:
    .long   _pvr_dr_addr
.Lsc_flags:
    .long   VTX_FLAGS
    .long   EOL_FLAGS
