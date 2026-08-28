! CHROMA CIRCUIT
! A standalone Sega Dreamcast / SH-4 assembly demo.
!
! No C runtime and no SDK functions are linked. This program owns its stack,
! talks to the SH-4 SCIF and store queues directly, polls Holly ASIC events,
! and submits native 32-byte parameter blocks to the PowerVR2 tile accelerator.

    .section .text.entry,"ax"
    .align  5
    .global _start

! ---------------------------------------------------------------------------
! Hardware and VRAM layout
! ---------------------------------------------------------------------------

    .equ PVR_BASE,               0xa05f8000
    .equ PVR_RESET,              0x0008
    .equ PVR_ISP_START,          0x0014
    .equ PVR_ISP_VERTBUF_ADDR,   0x0020
    .equ PVR_ISP_TILEMAT_ADDR,   0x002c
    .equ PVR_FB_CFG_1,           0x0044
    .equ PVR_FB_ADDR,            0x0050
    .equ PVR_FB_IL_ADDR,         0x0054
    .equ PVR_RENDER_ADDR,        0x0060
    .equ PVR_PCLIP_X,            0x0068
    .equ PVR_PCLIP_Y,            0x006c
    .equ PVR_BGPLANE_Z,          0x0088
    .equ PVR_BGPLANE_CFG,        0x008c
    .equ PVR_VIDEO_CFG,          0x00e8
    .equ PVR_SCALER_CFG,         0x00f4
    .equ PVR_SYNC_STATUS,        0x010c
    .equ PVR_TA_OPB_START,       0x0124
    .equ PVR_TA_VERTBUF_START,   0x0128
    .equ PVR_TA_OPB_END,         0x012c
    .equ PVR_TA_VERTBUF_END,     0x0130
    .equ PVR_TA_VERTBUF_POS,     0x0138
    .equ PVR_TILEMAT_CFG,        0x013c
    .equ PVR_OPB_CFG,            0x0140
    .equ PVR_TA_INIT,            0x0144
    .equ PVR_TA_OPB_INIT,        0x0164

    .equ ASIC_ACK_A,             0xa05f6900
    .equ ASIC_ACK_C,             0xa05f6908
    .equ MAPLE_DMA_ADDR,         0xa05f6c04
    .equ MAPLE_DMA_TSEL,         0xa05f6c10
    .equ MAPLE_ENABLE,           0xa05f6c14
    .equ MAPLE_STATE,            0xa05f6c18
    .equ MAPLE_SPEED,            0xa05f6c80
    .equ MAPLE_DMA_PROT,         0xa05f6c8c
    .equ PVR_RAM_32,             0xa5000000
    .equ SQ_BASE,                0xe0000000
    .equ QACR0,                  0xff000038
    .equ QACR1,                  0xff00003c

    .equ SCIF_SCSMR2,            0xffe80000
    .equ SCIF_SCBRR2,            0xffe80004
    .equ SCIF_SCSCR2,            0xffe80008
    .equ SCIF_SCFTDR2,           0xffe8000c
    .equ SCIF_SCFSR2,            0xffe80010
    .equ SCIF_SCFCR2,            0xffe80018
    .equ SCIF_SCSPTR2,           0xffe80020
    .equ SCIF_SCLSR2,            0xffe80024

    .equ TMU_TSTR,               0xffd80004
    .equ TMU_TCOR1,              0xffd80014
    .equ TMU_TCNT1,              0xffd80018
    .equ TMU_TCR1,               0xffd8001c

    .equ FB0,                    0x00200000
    .equ FB1,                    0x00600000
    .equ VERT0,                  0x00000000
    .equ VERT1,                  0x00400000
    .equ OPB0,                   0x00080000
    .equ OPB1,                   0x00480000
    .equ TRANS_OPB_DELTA,        0x00009600
    .equ OPB_TOTAL,              0x00012c00
    .equ OPB_OVERFLOW_BANKS,     15
    .equ OPB_SPAN,               OPB_TOTAL * (OPB_OVERFLOW_BANKS + 1)
    ! Close flybys put hundreds of giant triangle references in a few tiles.
    ! Fifteen overflow banks follow each base OPB; the demo is textureless, so
    ! spending the otherwise idle VRAM prevents geometry/HUD dropouts outright.
    .equ TILE0,                  0x001b0100
    .equ TILE1,                  0x005b0100
    .equ VERT_SIZE,              0x00080000

    .equ CMD_VERTEX,             0xe0000000
    .equ CMD_VERTEX_EOL,         0xf0000000
    .equ CONT_DPAD_LEFT,         0x0040
    .equ CONT_DPAD_RIGHT,        0x0080

_start:
    ! Enter privileged mode with interrupts masked; all synchronization below
    ! is deliberate polling, so no vector table or interrupt runtime is needed.
    mov.l   .Lstack_top, r15
    mov.l   .Linitial_sr, r0
    ldc     r0, sr
    mov     #4, r0
    shll16  r0
    lds     r0, fpscr              ! single precision, denormals flushed

    ! Flycast's HLE loader enters with both SH-4 caches disabled, and warm
    ! real-hardware loaders are not required to leave them in a known state.
    ! Switch through the uncached P2 mirror, invalidate, and enable I/D cache.
    mov.l   .Lfn_cache_enable, r0
    jsr     @r0
    nop

    mov.l   .Lfn_clear_bss, r0
    jsr     @r0
    nop
    mov.l   .Lfn_maple_init, r0
    jsr     @r0
    nop
    mov.l   .Lfn_serial_init, r0
    jsr     @r0
    nop
    mov.l   .Lmsg_boot, r4
    mov.l   .Lfn_serial_puts, r0
    jsr     @r0
    nop
    mov.l   .Lmsg_maple, r4
    mov.l   .Lfn_serial_puts, r0
    jsr     @r0
    nop

    mov.l   .Lfn_video_init, r0
    jsr     @r0
    nop
    ! The soundtrack is a raw AICA wavetable tracker. It is initialized only
    ! after video_init has sampled the cable pins because both units share the
    ! system-control word at 0xa0702c00.
    mov.l   .Lfn_music_init, r0
    jsr     @r0
    nop
    mov.l   .Lfn_pvr_init, r0
    jsr     @r0
    nop
    mov.l   .Lmsg_pvr, r4
    mov.l   .Lfn_serial_puts, r0
    jsr     @r0
    nop

    ! Both sets of tile descriptors are immutable and are built once.
    mov.l   .Ltile0, r4
    mov.l   .Lopb0, r5
    mov.l   .Lfn_build_tile_matrix, r0
    jsr     @r0
    nop
    mov.l   .Ltile1, r4
    mov.l   .Lopb1, r5
    mov.l   .Lfn_build_tile_matrix, r0
    jsr     @r0
    nop

    ! Clear both RGB565 framebuffers while the display is still blank.
    mov.l   .Lvram_fb0, r4
    mov.l   .Lfb_bytes, r5
    mov.l   .Lfn_sq_clear_region, r0
    jsr     @r0
    nop
    mov.l   .Lvram_fb1, r4
    mov.l   .Lfb_bytes, r5
    mov.l   .Lfn_sq_clear_region, r0
    jsr     @r0
    nop

    ! Seed and warm the nonlinear field before video is unblanked. Runtime
    ! updates then require only twelve Lorenz substeps per visible frame.
    mov.l   .Lfn_lorenz_init, r0
    jsr     @r0
    nop

    mov.l   .Lfn_display_enable, r0
    jsr     @r0
    nop
    mov.l   .Lmsg_render, r4
    mov.l   .Lfn_serial_puts, r0
    jsr     @r0
    nop
    mov.l   .Lfn_timer_init, r0
    jsr     @r0
    nop

    mov     #0, r8                  ! current TA buffer
.ifdef CAMERA_CAPTURE_FRAME
    ! Visual-QA builds may freeze one exact global timeline frame with
    ! --defsym CAMERA_CAPTURE_FRAME=n. The normal ELF assembles neither this
    ! load nor its literal, so release timing and code layout stay untouched.
    mov.l   .Lcamera_capture_frame, r9
.else
    mov     #0, r9                  ! frame counter
.endif

.Lmain_loop:
    ! Consume the previous frame's raw Maple reply and launch the next request.
    ! A new D-pad edge may rewrite r9 before this visible frame is generated.
    mov.l   .Lfn_scene_controls, r0
    jsr     @r0
    nop
    ! Sequence before beginning TA submission so a manual scene jump changes
    ! picture and harmony on the same complete display frame. AICA performs all
    ! synthesis in hardware; this call only updates a handful of voice words.
    mov.l   .Lfn_music_update, r0
    jsr     @r0
    nop
    mov     r8, r4
    mov.l   .Lfn_ta_begin, r0
    jsr     @r0
    nop
    mov.l   .Lfn_draw_scene, r0
    jsr     @r0
    nop
    ! A second nonblocking sample after geometry generation catches very quick
    ! taps at roughly 120 Hz. Any scene change begins on the next complete
    ! display frame; this frame's submitted geometry always remains coherent.
    mov.l   .Lfn_scene_controls, r0
    jsr     @r0
    nop
    mov.l   .Lfn_ta_finish, r0
    jsr     @r0
    nop
    mov.l   .Lfn_perf_sample, r0
    jsr     @r0
    nop

    mov     #1, r0
    xor     r0, r8
.ifndef CAMERA_CAPTURE_FRAME
    add     #1, r9
.endif
    mov.l   .Ltimeline_period, r0
    cmp/eq  r0, r9
    bf      .Lmain_loop
    nop
    mov     #0, r9                  ! four 768-frame acts, then loop cleanly
    bra     .Lmain_loop
    nop

    .align 2
.Lstack_top:    .long 0x8cfff000
.Linitial_sr:   .long 0x500000f0
.Lmsg_boot:     .long msg_boot
.Lmsg_maple:    .long msg_maple
.Lmsg_pvr:      .long msg_pvr
.Lmsg_render:   .long msg_render
.Ltimeline_period:.long 3072
.Ltile0:        .long TILE0
.Ltile1:        .long TILE1
.Lopb0:         .long OPB0
.Lopb1:         .long OPB1
.Lvram_fb0:     .long PVR_RAM_32 + FB0
.Lvram_fb1:     .long PVR_RAM_32 + FB1
.Lfb_bytes:     .long 640 * 480 * 2
.Lfn_cache_enable:     .long cache_enable
.Lfn_clear_bss:         .long clear_bss
.Lfn_maple_init:        .long maple_init
.Lfn_serial_init:       .long serial_init
.Lfn_serial_puts:       .long serial_puts
.Lfn_timer_init:        .long timer_init
.Lfn_perf_sample:       .long perf_sample
.Lfn_video_init:        .long video_init
.Lfn_music_init:        .long music_init
.Lfn_music_update:      .long music_update
.Lfn_pvr_init:          .long pvr_init
.Lfn_build_tile_matrix: .long build_tile_matrix
.Lfn_sq_clear_region:   .long sq_clear_region
.Lfn_lorenz_init:       .long lorenz_init
.Lfn_scene_controls:    .long maple_poll_scene_controls
.Lfn_display_enable:    .long display_enable
.Lfn_ta_begin:          .long ta_begin
.Lfn_draw_scene:        .long draw_scene
.Lfn_ta_finish:         .long ta_finish_and_render
.ifdef CAMERA_CAPTURE_FRAME
.Lcamera_capture_frame: .long CAMERA_CAPTURE_FRAME
.endif

! ---------------------------------------------------------------------------
! Startup helpers
! ---------------------------------------------------------------------------

    .section .text,"ax"
    .align 2

! Enable the SH-4 instruction and operand caches without executing a cached
! access while CCR is changing. All operands are loaded before the P2 jump,
! since PC-relative literal loads through P2 would address the wrong mirror.
cache_enable:
    mov.l   .Lcache_ccr, r6
    mov.l   .Lcache_tags, r3
    mov.l   .Lcache_value, r4
    mov     #2, r1                  ! 512 operand-cache tag entries
    shll8   r1
    mov     #0, r2
    mova    .Lcache_uncached, r0
    mov.l   .Lcache_p2_mask, r7
    or      r7, r0
    jmp     @r0
    nop

    .align 2
.Lcache_uncached:
    mov.l   r2, @r3
    add     #32, r3
    dt      r1
    bf      .Lcache_uncached
    nop
    mov.l   r4, @r6
    ! SH7750 requires at least eight instructions after a CCR write.
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    rts
    nop

    .align 2
.Lcache_ccr:      .long 0xff00001c
.Lcache_tags:     .long 0xf4000000
.Lcache_value:    .long 0x0000090d   ! ICI | ICE | OCI | CB | OCE
.Lcache_p2_mask:  .long 0xa0000000

clear_bss:
    mov.l   .Lbss_begin, r0
    mov.l   .Lbss_end, r1
    mov     #0, r2
.Lclear_bss_loop:
    cmp/hs  r1, r0
    bt      .Lclear_bss_done
    mov.l   r2, @r0
    bra     .Lclear_bss_loop
    add     #4, r0
.Lclear_bss_done:
    rts
    nop
    .align 2
.Lbss_begin: .long __bss_start
.Lbss_end:   .long __bss_end

! ---------------------------------------------------------------------------
! Bare-metal Maple controller pipeline
!
! Port A's root device is queried directly with GETCOND. The DMA started at
! the end of one call is consumed on the next displayed frame, so controller
! traffic never spins inside the 60 Hz renderer. Only Left/Right edges alter
! r9; every other register convention remains the demo's private leaf ABI.
! ---------------------------------------------------------------------------

maple_init:
    ! clear_bss wrote these lines through cached P1 while the operand cache was
    ! in copy-back mode. Write them back and purge every line before the CPU and
    ! Maple hardware share only their uncached P2/physical aliases.
    mov.l   .Lmaple_table_p1, r0
    mov     #33, r1                 ! 32-byte table + 1024-byte response
.Lmaple_purge_dma_lines:
    ocbp    @r0
    add     #32, r0
    dt      r1
    bf      .Lmaple_purge_dma_lines
    nop

    ! One terminal descriptor: GETCOND to port A, root unit, with one function
    ! word. The response address stored in the table is the physical 0x0c...
    ! alias; CPU reads and writes use the uncached 0xac... mirror instead.
    mov.l   .Lmaple_table_p1, r4
    mov.l   .Lmaple_p2_bit, r0
    or      r0, r4
    mov.l   .Lmaple_desc_control, r0
    mov.l   r0, @r4
    add     #4, r4
    mov.l   .Lmaple_response_p1, r0
    mov.l   .Lmaple_phys_mask, r1
    and     r1, r0
    mov.l   r0, @r4
    add     #4, r4
    mov.l   .Lmaple_getcond_header, r0
    mov.l   r0, @r4
    add     #4, r4
    mov.l   .Lmaple_controller_function, r0
    mov.l   r0, @r4

    ! Take ownership of the bus in software-trigger mode. 0x6155404f opens only
    ! the retail 16 MiB main-RAM physical window to Maple DMA.
    mov.l   .Lmaple_enable_reg, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov.l   .Lmaple_state_reg, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_prot_reg, r0
    mov.l   .Lmaple_protection, r1
    mov.l   r1, @r0
    mov.l   .Lmaple_tsel_reg, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov.l   .Lmaple_speed_reg, r0
    mov.l   .Lmaple_speed_value, r1
    mov.l   r1, @r0
    mov.l   .Lmaple_asic_ack, r0
    mov.l   .Lmaple_event_mask, r1
    mov.l   r1, @r0
    mov.l   .Lmaple_enable_reg, r0
    mov     #1, r1
    mov.l   r1, @r0
    rts
    nop

maple_poll_scene_controls:
    ! If a request is pending, either consume it or return immediately while
    ! hardware owns the bus. Four busy display frames indicate a wedged bus.
    mov.l   .Lmaple_pending_ptr, r0
    mov.l   @r0, r1
    tst     r1, r1
    bf      .Lmaple_have_pending_poll
    bra     .Lmaple_launch_poll
    nop
.Lmaple_have_pending_poll:
    mov.l   .Lmaple_state_reg, r2
    mov.l   @r2, r3
    mov     #1, r4
    tst     r4, r3
    bt      .Lmaple_poll_complete

    mov.l   .Lmaple_busy_ptr, r0
    mov.l   @r0, r1
    add     #1, r1
    mov.l   r1, @r0
    mov     #8, r2                  ! two checks/frame: four-frame timeout
    cmp/hs  r2, r1
    bt      .Lmaple_poll_timeout
    rts
    nop

.Lmaple_poll_timeout:
    ! Abort and re-arm without blocking a rendered frame. Input validity is
    ! dropped; a direction present on the first recovered sample remains valid.
    mov.l   .Lmaple_state_reg, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov.l   .Lmaple_enable_reg, r0
    mov.l   r1, @r0
    mov     #1, r1
    mov.l   r1, @r0
    mov     #0, r1
    mov.l   .Lmaple_pending_ptr, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_busy_ptr, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_valid_ptr, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_previous_ptr, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_asic_ack, r0
    mov.l   .Lmaple_event_mask, r1
    mov.l   r1, @r0
    rts
    nop

.Lmaple_poll_complete:
    mov     #0, r1
    mov.l   .Lmaple_pending_ptr, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_busy_ptr, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_asic_ack, r0
    mov.l   .Lmaple_event_mask, r1
    mov.l   r1, @r0

    ! Response header bytes are response,destination,source,data-word count.
    ! Attached peripherals can alter the middle bytes, so validate only the
    ! DATATRF code, minimum length, and controller function word.
    mov.l   .Lmaple_response_p1, r4
    mov.l   .Lmaple_p2_bit, r0
    or      r0, r4
    mov.l   @r4, r0
    mov     r0, r1
    extu.b  r1, r1
    mov     #8, r2
    cmp/eq  r2, r1
    bf      .Lmaple_invalid_sample
    mov     r0, r1
    shlr16  r1
    shlr8   r1
    mov     #3, r2
    cmp/hs  r2, r1
    bf      .Lmaple_invalid_sample
    mov     r4, r5
    add     #4, r5
    mov.l   @r5, r0
    mov.l   .Lmaple_controller_function, r1
    cmp/eq  r1, r0
    bf      .Lmaple_invalid_sample

    ! Controller button bits arrive active-low in the next halfword. Retain
    ! only the two scene controls before performing press-edge detection.
    add     #4, r5
    mov.w   @r5, r0
    extu.w  r0, r0
    not     r0, r0
    mov.l   .Lmaple_direction_mask, r1
    and     r1, r0                  ! r0 = current Left/Right held mask
    mov.l   .Lmaple_valid_ptr, r2
    mov.l   @r2, r3
    tst     r3, r3
    bt      .Lmaple_arm_first_sample

    mov.l   .Lmaple_previous_ptr, r2
    mov.l   @r2, r1
    mov.l   r0, @r2
    not     r1, r1
    and     r0, r1                  ! r1 = newly pressed direction bits
.Lmaple_classify_edge:
    mov     #CONT_DPAD_LEFT, r0
    cmp/eq  r0, r1
    bt      .Lmaple_choose_left
    mov     #CONT_DPAD_LEFT, r0
    shll    r0                      ! 0x80 cannot be encoded as positive imm8
    cmp/eq  r0, r1
    bt      .Lmaple_choose_right
    bra     .Lmaple_launch_poll     ! neither or both directions: no action
    nop

.Lmaple_arm_first_sample:
    ! A direction already held on the first valid/recovered reply is itself a
    ! press. This avoids losing a very quick tap that straddles reconnect or
    ! startup, while a neutral first sample still just arms edge detection.
    mov     #1, r3
    mov.l   r3, @r2
    mov.l   .Lmaple_previous_ptr, r2
    mov.l   r0, @r2
    tst     r0, r0
    bt      .Lmaple_launch_poll
    mov     r0, r1
    bra     .Lmaple_classify_edge
    nop

.Lmaple_invalid_sample:
    ! Missing controllers and malformed replies are harmless. The next valid
    ! sample re-arms cleanly; a direction already held is accepted as a tap.
    mov     #0, r1
    mov.l   .Lmaple_valid_ptr, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_previous_ptr, r0
    mov.l   r1, @r0
    bra     .Lmaple_launch_poll
    nop

.Lmaple_choose_left:
    mov     #-1, r1
    bra     .Lmaple_find_scene
    nop
.Lmaple_choose_right:
    mov     #1, r1

.Lmaple_find_scene:
    ! Derive the current act from global r9 rather than the previous frame's
    ! label state. This remains correct on the exact frame the auto-loop wraps.
    mov     #0, r3
    mov.l   .Lmaple_scene_length, r2
    cmp/hs  r2, r9
    bf      .Lmaple_have_scene
    mov     #1, r3
    mov     r9, r4
    sub     r2, r4
    cmp/hs  r2, r4
    bf      .Lmaple_have_scene
    mov     #2, r3
    sub     r2, r4
    cmp/hs  r2, r4
    bf      .Lmaple_have_scene
    mov     #3, r3
.Lmaple_have_scene:
    cmp/pz  r1
    bt      .Lmaple_scene_right
    tst     r3, r3
    bf      .Lmaple_scene_left_decrement
    mov     #3, r3
    bra     .Lmaple_commit_scene
    nop
.Lmaple_scene_left_decrement:
    add     #-1, r3
    bra     .Lmaple_commit_scene
    nop
.Lmaple_scene_right:
    add     #1, r3
    mov     #4, r0
    cmp/eq  r0, r3
    bf      .Lmaple_commit_scene
    mov     #0, r3

.Lmaple_commit_scene:
    ! index * 768 = index * 3 * 256. Land at local frame 32 so a manual
    ! selection reveals its bottom-right title immediately after the flash.
    mov     r3, r0
    shll    r0
    add     r3, r0
    shll8   r0
    add     #32, r0
    mov     r0, r9

.Lmaple_launch_poll:
    ! Clear the first response cache line through P2, then make the immutable
    ! command table visible and start one software-triggered transfer.
    mov.l   .Lmaple_response_p1, r4
    mov.l   .Lmaple_p2_bit, r0
    or      r0, r4
    mov     #0, r0
    mov     #8, r1
.Lmaple_clear_response:
    mov.l   r0, @r4
    add     #4, r4
    dt      r1
    bf      .Lmaple_clear_response
    nop
    mov.l   .Lmaple_asic_ack, r0
    mov.l   .Lmaple_event_mask, r1
    mov.l   r1, @r0
    mov.l   .Lmaple_table_p1, r1
    mov.l   .Lmaple_phys_mask, r2
    and     r2, r1
    mov.l   .Lmaple_dma_addr_reg, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_state_reg, r0
    mov     #1, r1
    mov.l   r1, @r0
    mov.l   .Lmaple_pending_ptr, r0
    mov.l   r1, @r0
    mov.l   .Lmaple_busy_ptr, r0
    mov     #0, r1
    mov.l   r1, @r0
    rts
    nop

    .align 2
.Lmaple_table_p1:          .long maple_dma_table
.Lmaple_response_p1:       .long maple_response
.Lmaple_pending_ptr:       .long maple_pending
.Lmaple_busy_ptr:          .long maple_busy_frames
.Lmaple_valid_ptr:         .long maple_sample_valid
.Lmaple_previous_ptr:      .long maple_previous_buttons
.Lmaple_dma_addr_reg:      .long MAPLE_DMA_ADDR
.Lmaple_tsel_reg:          .long MAPLE_DMA_TSEL
.Lmaple_enable_reg:        .long MAPLE_ENABLE
.Lmaple_state_reg:         .long MAPLE_STATE
.Lmaple_speed_reg:         .long MAPLE_SPEED
.Lmaple_prot_reg:          .long MAPLE_DMA_PROT
.Lmaple_asic_ack:          .long ASIC_ACK_A
.Lmaple_p2_bit:            .long 0x20000000
.Lmaple_phys_mask:         .long 0x1fffffff
.Lmaple_protection:        .long 0x6155404f
.Lmaple_speed_value:       .long 0xc3500000
.Lmaple_event_mask:        .long 0x00003000
.Lmaple_desc_control:      .long 0x80000001
.Lmaple_getcond_header:    .long 0x01002009
.Lmaple_controller_function:.long 0x01000000
.Lmaple_direction_mask:    .long CONT_DPAD_LEFT | CONT_DPAD_RIGHT
.Lmaple_scene_length:      .long 768

! Polled 115200 8N1 serial. Flycast's Serial Console option mirrors this SCIF
! stream into the launching terminal, exactly as real serial hardware would.
serial_init:
    mov.l   .Lscscr2, r0
    mov     #0, r1
    mov.w   r1, @r0
    mov.l   .Lscfcr2, r0
    mov     #6, r1
    mov.w   r1, @r0
    mov.l   .Lscsmr2, r0
    mov     #0, r1
    mov.w   r1, @r0
    mov.l   .Lscbrr2, r0
    mov     #12, r1
    mov.b   r1, @r0
    mov.l   .Lserial_delay, r1
.Lserial_delay_a:
    dt      r1
    bf      .Lserial_delay_a
    nop
    mov.l   .Lscfcr2, r0
    mov     #0x40, r1
    mov.w   r1, @r0
    mov.l   .Lscsptr2, r0
    mov     #0, r1
    mov.w   r1, @r0
    mov.l   .Lscfsr2, r0
    mov.w   @r0, r2                 ! required read before clearing latches
    mov     #0x60, r1
    mov.w   r1, @r0
    mov.l   .Lsclsr2, r0
    mov.w   @r0, r2
    mov     #0, r1
    mov.w   r1, @r0
    mov.l   .Lscscr2, r0
    mov     #0x30, r1
    mov.w   r1, @r0
    mov.l   .Lserial_delay, r1
.Lserial_delay_b:
    dt      r1
    bf      .Lserial_delay_b
    nop
    rts
    nop

serial_puts:
    sts.l   pr, @-r15
    mov     r4, r7
.Lserial_next:
    mov.b   @r7+, r4
    tst     r4, r4
    bt      .Lserial_done
    bsr     serial_putc
    nop
    bra     .Lserial_next
    nop
.Lserial_done:
    lds.l   @r15+, pr
    rts
    nop

serial_putc:
    mov.l   .Lscfsr2, r0
    mov.l   .Lserial_timeout, r2
.Lserial_wait:
    mov.w   @r0, r1
    mov     #0x20, r3
    tst     r3, r1
    bf      .Lserial_ready
    dt      r2
    bf      .Lserial_wait
    nop
    rts
    nop

! r4 = value. Emit exactly eight hexadecimal digits without libc formatting.
serial_puthex32:
    sts.l   pr, @-r15
    mov     r4, r7
    mov     #8, r5
.Lserial_hex_loop:
    mov     r7, r0
    shlr16  r0
    shlr8   r0
    shlr2   r0
    shlr2   r0
    and     #0x0f, r0
    mov     #9, r1
    cmp/hi  r1, r0
    bf      .Lserial_hex_digit
    add     #7, r0
.Lserial_hex_digit:
    add     #48, r0
    mov     r0, r4
    bsr     serial_putc
    nop
    shll2   r7
    shll2   r7
    dt      r5
    bf      .Lserial_hex_loop
    nop
    lds.l   @r15+, pr
    rts
    nop
.Lserial_ready:
    mov.l   .Lscftdr2, r0
    mov.b   r4, @r0
    mov.l   .Lscfsr2, r0
    mov.w   @r0, r1
    mov.l   .Lfsr_mask, r2
    and     r2, r1
    mov.w   r1, @r0
    rts
    nop

    .align 2
.Lscsmr2:          .long SCIF_SCSMR2
.Lscbrr2:          .long SCIF_SCBRR2
.Lscscr2:          .long SCIF_SCSCR2
.Lscftdr2:         .long SCIF_SCFTDR2
.Lscfsr2:          .long SCIF_SCFSR2
.Lscfcr2:          .long SCIF_SCFCR2
.Lscsptr2:         .long SCIF_SCSPTR2
.Lsclsr2:          .long SCIF_SCLSR2
.Lserial_delay:    .long 800000
.Lserial_timeout:  .long 800000
.Lfsr_mask:        .long 0x0000ff9f

! ---------------------------------------------------------------------------
! Raw TMU1 frame telemetry
! ---------------------------------------------------------------------------

! TMU1 free-runs from PCLK/4 = 12,468,720 Hz. A normal 59.94/60 Hz interval
! is about 208k ticks; 300k cleanly distinguishes a missed vblank (~416k).
timer_init:
    mov.l   .Ltmu_tstr, r0
    mov.b   @r0, r1
    extu.b  r1, r1
    mov     #-3, r2
    and     r2, r1
    mov.b   r1, @r0               ! stop channel 1
    mov.l   .Ltmu_tcnt1, r0
    mov     #-1, r1
    mov.l   r1, @r0
    mov.l   .Ltmu_tcor1, r0
    mov.l   r1, @r0
    mov.l   .Ltmu_tcr1, r0
    mov     #0, r1
    mov.w   r1, @r0               ! /4, no IRQ, clear underflow
    mov.l   .Ltmu_tstr, r0
    mov.b   @r0, r1
    extu.b  r1, r1
    mov     #2, r2
    or      r2, r1
    mov.b   r1, @r0
    mov.l   .Lperf_previous, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov.l   .Lperf_countdown, r0
    mov.l   .Lperf_period, r1
    mov.l   r1, @r0
    mov.l   .Lperf_slow_count, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov.l   .Lperf_total_ticks, r0
    mov.l   r1, @r0
    rts
    nop

perf_sample:
    sts.l   pr, @-r15
    mov.l   .Ltmu_tcnt1, r0
    mov.l   @r0, r1               ! current down-counter
    mov.l   .Lperf_previous, r0
    mov.l   @r0, r2
    mov.l   r1, @r0
    tst     r2, r2
    bt      .Lperf_done             ! first completed frame primes baseline
    sub     r1, r2                ! unsigned elapsed, wrap-safe
    mov.l   .Lperf_total_ticks, r0
    mov.l   @r0, r1
    add     r2, r1
    mov.l   r1, @r0
    mov.l   .Lperf_slow_limit, r3
    cmp/hi  r3, r2
    bf      .Lperf_interval_ok
    mov.l   .Lperf_slow_count, r0
    mov.l   @r0, r1
    add     #1, r1
    mov.l   r1, @r0
.Lperf_interval_ok:
    mov.l   .Lperf_countdown, r0
    mov.l   @r0, r1
    add     #-1, r1
    mov.l   r1, @r0
    tst     r1, r1
    bf      .Lperf_done
    nop

    mov.l   .Lmsg_perf, r4
    bsr     serial_puts
    nop
    mov.l   .Lperf_slow_count, r0
    mov.l   @r0, r4
    bsr     serial_puthex32
    nop
    mov.l   .Lmsg_crlf, r4
    bsr     serial_puts
    nop
    mov.l   .Lmsg_perf_ticks, r4
    bsr     serial_puts
    nop
    mov.l   .Lperf_total_ticks, r0
    mov.l   @r0, r4
    bsr     serial_puthex32
    nop
    mov.l   .Lmsg_crlf, r4
    bsr     serial_puts
    nop
    ! AICA telemetry is emitted in the same deliberately unmeasured serial
    ! window as the renderer report below.
    mov.l   .Lfn_music_perf_report, r0
    jsr     @r0
    nop
    ! Serial is intentionally outside the measured window. Clear the baseline
    ! so the next completed frame is discarded as a fresh phase sample; this
    ! avoids counting the partial refresh that contains the serial report.
    mov     #0, r1
    mov.l   .Lperf_previous, r0
    mov.l   r1, @r0
    mov.l   .Lperf_slow_count, r0
    mov.l   r1, @r0
    mov.l   .Lperf_total_ticks, r0
    mov.l   r1, @r0
    mov.l   .Lperf_countdown, r0
    mov.l   .Lperf_period, r1
    mov.l   r1, @r0
.Lperf_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Ltmu_tstr:         .long TMU_TSTR
.Ltmu_tcor1:        .long TMU_TCOR1
.Ltmu_tcnt1:        .long TMU_TCNT1
.Ltmu_tcr1:         .long TMU_TCR1
.Lperf_previous:    .long perf_previous
.Lperf_countdown:   .long perf_countdown
.Lperf_slow_count:  .long perf_slow_count
.Lperf_total_ticks: .long perf_total_ticks
.Lperf_period:      .long 600
.Lperf_slow_limit:  .long 300000
.Lmsg_perf:         .long msg_perf
.Lmsg_perf_ticks:   .long msg_perf_ticks
.Lmsg_crlf:         .long msg_crlf
.Lfn_music_perf_report:.long music_perf_report

! ---------------------------------------------------------------------------
! Video and PowerVR2 initialization
! ---------------------------------------------------------------------------

video_init:
    sts.l   pr, @-r15
    ! Detect the cable on SH-4 GPIO pins 8/9, following Sega's electrical
    ! convention. Zero means VGA; all other values use 480i NTSC timings.
    mov.l   .Lpctra, r0
    mov.l   @r0, r1
    mov.l   .Lpctra_mask, r2
    and     r2, r1
    mov.l   .Lpctra_bits, r2
    or      r2, r1
    mov.l   r1, @r0
    mov.l   .Lpdtra, r0
    mov.w   @r0, r1
    shlr8   r1
    mov     #3, r2
    and     r2, r1
    mov.l   .Lcable_type_addr, r0
    mov.l   r1, @r0

    ! Blank before changing timing registers.
    mov.l   .Lpvr_base, r14
    mov.l   .Lvideo_cfg_off, r0
    mov.l   @(r0, r14), r2
    mov.l   .Lvideo_mode_mask, r3   ! clear stale line-doubling control
    and     r3, r2
    mov     #8, r3
    or      r3, r2
    mov.l   r2, @(r0, r14)
    mov     #PVR_FB_CFG_1, r0
    mov.l   @(r0, r14), r2
    mov     #-2, r3
    and     r3, r2
    mov.l   r2, @(r0, r14)

    tst     r1, r1
    bf      .Lvideo_ntsc
    mov.l   .Lvga_table, r4
    bra     .Lvideo_apply
    nop
.Lvideo_ntsc:
    mov.l   .Lntsc_table, r4
.Lvideo_apply:
    bsr     apply_pvr_table
    nop

    ! Program the physical cable selector bits as well.
    mov.l   .Lcable_type_addr, r0
    mov.l   @r0, r1
    mov.l   .Lcable_reg, r0
    mov.l   @r0, r2
    mov.l   .Lcable_reg_mask, r3
    and     r3, r2
    shll8   r1
    or      r1, r2
    mov.l   r2, @r0
    lds.l   @r15+, pr
    rts
    nop

pvr_init:
    sts.l   pr, @-r15
    mov.l   .Lpvr_base, r14
    mov     #PVR_RESET, r0
    mov     #-1, r1
    mov.l   r1, @(r0, r14)
    mov     #0, r1
    mov.l   r1, @(r0, r14)
    mov.l   .Lpvr_init_table, r4
    bsr     apply_pvr_table
    nop

    ! Interlaced output uses the PVR2 vertical smoothing step; progressive
    ! VGA deliberately leaves it at exactly 0x400.
    mov.l   .Lcable_type_addr, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Lpvr_scaler_done
    mov.l   .Lscaler_off, r0       ! 0xf4 does not fit signed mov #imm
    mov.l   .Lscaler_ntsc, r1
    mov.l   r1, @(r0, r14)
.Lpvr_scaler_done:
    lds.l   @r15+, pr
    rts
    nop

display_enable:
    mov.l   .Lpvr_base, r14
    mov.l   .Lvideo_cfg_off, r0
    mov.l   @(r0, r14), r1
    mov     #-9, r2                 ! clear blank bit 3
    and     r2, r1
    mov.l   r1, @(r0, r14)
    mov     #PVR_FB_CFG_1, r0
    mov.l   @(r0, r14), r1
    mov     #1, r2
    or      r2, r1
    mov.l   r1, @(r0, r14)
    rts
    nop

! r4 -> pairs of {register offset, value}; offset -1 terminates.
apply_pvr_table:
    mov.l   .Lpvr_base, r14
.Ltable_loop:
    mov.l   @r4+, r0
    cmp/eq  #-1, r0
    bt      .Ltable_done
    mov.l   @r4+, r1
    mov.l   r1, @(r0, r14)
    bra     .Ltable_loop
    nop
.Ltable_done:
    rts
    nop

    .align 2
.Lpctra:            .long 0xff80002c
.Lpdtra:            .long 0xff800030
.Lpctra_mask:       .long 0xfff0ffff
.Lpctra_bits:       .long 0x000a0000
.Lcable_type_addr:  .long cable_type
.Lcable_reg:        .long 0xa0702c00
.Lcable_reg_mask:   .long 0xfffffcff
.Lpvr_base:         .long PVR_BASE
.Lvideo_cfg_off:    .long PVR_VIDEO_CFG
.Lvideo_mode_mask:  .long 0xfffffeff
.Lscaler_off:       .long PVR_SCALER_CFG
.Lscaler_ntsc:      .long 0x00000401
.Lvga_table:        .long video_vga_table
.Lntsc_table:       .long video_ntsc_table
.Lpvr_init_table:   .long pvr_init_table

! ---------------------------------------------------------------------------
! Store queues and tile matrices
! ---------------------------------------------------------------------------

! Clear r5 bytes at 32-byte aligned external address r4 using both SH-4 store
! queues. The caller supplies the P2 VRAM address (0xa5xxxxxx).
sq_clear_region:
    mov     r4, r0
    shlr16  r0
    shlr8   r0
    and     #0x1c, r0
    mov.l   .Lqacr0, r1
    mov.l   r0, @r1
    mov.l   .Lqacr1, r1
    mov.l   r0, @r1
    mov.l   .Lsq_addr_mask, r0
    and     r4, r0
    mov.l   .Lsq_area_bits, r1
    or      r1, r0
    shlr2   r5
    shlr2   r5
    shlr2   r5
    mov     #0, r1
.Lsq_clear_loop:
    mov.l   r1, @(0, r0)
    mov.l   r1, @(4, r0)
    mov.l   r1, @(8, r0)
    mov.l   r1, @(12, r0)
    mov.l   r1, @(16, r0)
    mov.l   r1, @(20, r0)
    mov.l   r1, @(24, r0)
    mov.l   r1, @(28, r0)
    pref    @r0
    add     #32, r0
    dt      r5
    bf      .Lsq_clear_loop
    nop
    rts
    nop

! r4 = tile matrix VRAM offset, r5 = opaque OPB base offset.
build_tile_matrix:
    mov.l   .Lvram_base, r0
    or      r4, r0
    mov     r0, r7
    add     #-72, r7
    mov     #0, r1
    mov     #18, r2
.Ltile_zero_header:
    mov.l   r1, @r7
    add     #4, r7
    dt      r2
    bf      .Ltile_zero_header
    nop

    ! Initial descriptor required by the ISP.
    mov.l   .Ltile_init0, r1
    mov.l   r1, @r0
    add     #4, r0
    mov.l   .Ltile_ignore, r1
    mov.l   r1, @r0
    add     #4, r0
    mov.l   r1, @r0
    add     #4, r0
    mov.l   r1, @r0
    add     #4, r0
    mov.l   r1, @r0
    add     #4, r0
    mov.l   r1, @r0
    add     #4, r0

    mov     #0, r6                  ! x
.Ltile_x_loop:
    mov     #0, r7                  ! y
.Ltile_y_loop:
    ! control = (y << 8) | (x << 2)
    mov     r7, r1
    shll8   r1
    mov     r6, r2
    shll2   r2
    or      r2, r1
    mov     #19, r2
    cmp/eq  r2, r6
    bf      .Ltile_not_last
    mov     #14, r2
    cmp/eq  r2, r7
    bf      .Ltile_not_last
    mov.l   .Ltile_last_bit, r2
    or      r2, r1
.Ltile_not_last:
    mov.l   r1, @r0
    add     #4, r0

    ! tn = y * 20 + x; each 32-word OPB bin occupies 128 bytes.
    mov     r7, r1
    shll2   r1                      ! y*4
    add     r7, r1                  ! y*5
    shll2   r1                      ! y*20
    add     r6, r1
    shll8   r1
    shlr    r1                      ! *128
    add     r5, r1
    mov.l   r1, @r0                 ! opaque list pointer
    add     #4, r0
    mov.l   .Ltile_ignore, r2
    mov.l   r2, @r0                 ! opaque modifier
    add     #4, r0
    mov.l   .Ltrans_delta, r3
    add     r3, r1
    mov.l   r1, @r0                 ! translucent list pointer
    add     #4, r0
    mov.l   r2, @r0                 ! translucent modifier
    add     #4, r0
    mov.l   r2, @r0                 ! punch-through
    add     #4, r0

    add     #1, r7
    mov     #15, r1
    cmp/hs  r1, r7
    bf      .Ltile_y_loop
    nop
    add     #1, r6
    mov     #20, r1
    cmp/hs  r1, r6
    bf      .Ltile_x_loop
    nop
    rts
    nop

! Submit one aligned 32-byte block at r4 to the TA FIFO via SQ0.
ta_submit:
    mov.l   .Lsq_ta, r0
    mov     #8, r1
.Lsubmit_loop:
    mov.l   @r4+, r2
    mov.l   r2, @r0
    add     #4, r0
    dt      r1
    bf      .Lsubmit_loop
    nop
    add     #-32, r0
    pref    @r0
    rts
    nop

    .align 2
.Lqacr0:        .long QACR0
.Lqacr1:        .long QACR1
.Lsq_addr_mask: .long 0x03ffffe0
.Lsq_area_bits: .long SQ_BASE
.Lsq_ta:        .long SQ_BASE
.Lvram_base:    .long PVR_RAM_32
.Ltile_init0:   .long 0x10000000
.Ltile_ignore:  .long 0x80000000
.Ltile_last_bit:.long 0x80000000
.Ltrans_delta:  .long TRANS_OPB_DELTA

! ---------------------------------------------------------------------------
! Synchronous TA / ISP frame pipeline
! ---------------------------------------------------------------------------

! r4 = set index (0/1)
ta_begin:
    mov.l   .Lasic_ack, r0
    mov     #-1, r1
    mov.l   r1, @r0
    mov.l   .Lasic_ack_c, r0
    mov.l   r1, @r0                ! discard stale PVR error events

    tst     r4, r4
    bf      .Lta_set1
    mov.l   .Lvert0, r10
    mov.l   .Lopb0c, r11
    mov.l   .Ltile0c, r12
    mov.l   .Lfb0c, r13
    bra     .Lta_set_ready
    nop
.Lta_set1:
    mov.l   .Lvert1, r10
    mov.l   .Lopb1c, r11
    mov.l   .Ltile1c, r12
    mov.l   .Lfb1c, r13
.Lta_set_ready:
    mov.l   .Lcurrent_vert, r0
    mov.l   r10, @r0
    mov.l   .Lcurrent_tile, r0
    mov.l   r12, @r0
    mov.l   .Lcurrent_fb, r0
    mov.l   r13, @r0

    mov.l   .Lpvr_base2, r14
    mov.l   .Lopb_start_off, r0
    mov.l   r11, @(r0, r14)
    mov.l   .Lopb_total, r1
    add     r11, r1
    mov.l   .Lopb_init_off, r0
    mov.l   r1, @(r0, r14)
    mov.l   .Lopb_span, r1
    add     r11, r1                  ! base bank + fifteen overflow banks
    mov.l   .Lopb_end_off, r0
    mov.l   r1, @(r0, r14)
    mov.l   .Lvert_start_off, r0
    mov.l   r10, @(r0, r14)
    mov.l   .Lvert_size, r1
    add     r10, r1
    mov.l   .Lvert_end_off, r0
    mov.l   r1, @(r0, r14)
    mov.l   .Ltile_cfg_off, r0
    mov.l   .Ltile_cfg, r1
    mov.l   r1, @(r0, r14)
    mov.l   .Lopb_cfg_off, r0
    mov.l   .Lopb_cfg, r1
    mov.l   r1, @(r0, r14)
    mov.l   .Lta_init_off, r0
    mov.l   .Lta_go, r1
    mov.l   r1, @(r0, r14)
    mov.l   @(r0, r14), r1         ! required readback

    ! Route both queues to the TA external area (0x10xxxxxx).
    mov.l   .Lqacr0b, r0
    mov     #0x10, r1
    mov.l   r1, @r0
    mov.l   .Lqacr1b, r0
    mov.l   r1, @r0
    rts
    nop

ta_finish_and_render:
    sts.l   pr, @-r15
    ! draw_scene has already switched lists; terminate the translucent list.
    mov.l   .Lzero_block, r4
    bsr     ta_submit
    nop

    ! Wait until Holly confirms both opaque and translucent lists were binned.
    mov.l   .Lasic_ack, r0
    mov.l   .Lta_done_mask, r2
    mov.l   .Lwait_budget, r3
.Lwait_ta_done:
    mov.l   @r0, r1
    and     r2, r1
    cmp/eq  r2, r1
    bt      .Lta_done_ready
    mov.l   .Lasic_ack_c, r4
    mov.l   @r4, r5
    mov.l   .Lpvr_error_mask, r6
    tst     r6, r5
    bf      .Lta_hardware_fault
    dt      r3
    bf      .Lwait_ta_done
    nop
    mov.l   .Lmsg_ta_timeout, r4
    bra     pvr_fatal
    nop
.Lta_hardware_fault:
    mov.l   .Lmsg_ta_fault, r4
    bra     pvr_fatal
    nop
.Lta_done_ready:
    ! Completion and a TA fault may latch on the same cycle.  Check ACK_C
    ! before clearing ACK_A so an OPB overflow can never be acknowledged away.
    mov.l   .Lasic_ack_c, r4
    mov.l   @r4, r5
    mov.l   .Lpvr_error_mask, r6
    tst     r6, r5
    bf      .Lta_hardware_fault
    mov.l   r2, @r0

    ! The background plane lives immediately after TA-emitted parameters.
    mov.l   .Lpvr_base2, r14
    mov.l   .Lvert_pos_off, r0
    mov.l   @(r0, r14), r1
    mov     r1, r6
    mov.l   .Lvram_base2, r0
    or      r1, r0
    mov.l   .Lbkg_template, r4
    mov     #15, r2
.Lbkg_copy:
    mov.l   @r4+, r3
    mov.l   r3, @r0
    add     #4, r0
    dt      r2
    bf      .Lbkg_copy
    nop

    mov.l   .Lcurrent_vert, r0
    mov.l   @r0, r10
    sub     r10, r6
    shll    r6
    mov.l   .Lbkg_cfg_prefix, r1
    or      r1, r6

    mov.l   .Lisp_tile_off, r0
    mov.l   .Lcurrent_tile, r1
    mov.l   @r1, r2
    mov.l   r2, @(r0, r14)
    mov     #PVR_ISP_VERTBUF_ADDR, r0
    mov.l   r10, @(r0, r14)
    mov     #PVR_RENDER_ADDR, r0
    mov.l   .Lcurrent_fb, r1
    mov.l   @r1, r2
    mov.l   r2, @(r0, r14)
    mov.l   .Lbkg_cfg_off, r0
    mov.l   r6, @(r0, r14)
    mov.l   .Lbkg_z_off, r0
    mov.l   .Lbkg_z, r1
    mov.l   r1, @(r0, r14)
    mov     #PVR_PCLIP_X, r0
    mov.l   .Lpclip_x, r1
    mov.l   r1, @(r0, r14)
    mov     #PVR_PCLIP_Y, r0
    mov.l   .Lpclip_y, r1
    mov.l   r1, @(r0, r14)

    mov.l   .Lasic_ack, r0
    mov     #4, r1
    mov.l   r1, @r0
    mov     #PVR_ISP_START, r0
    mov     #-1, r1
    mov.l   r1, @(r0, r14)

    mov.l   .Lasic_ack, r0
    mov.l   .Lwait_budget, r3
.Lwait_render:
    mov.l   @r0, r1
    mov     #4, r2
    tst     r2, r1
    bf      .Lrender_done
    mov.l   .Lasic_ack_c, r4
    mov.l   @r4, r5
    mov.l   .Lpvr_error_mask, r6
    tst     r6, r5
    bf      .Lrender_hardware_fault
    dt      r3
    bf      .Lwait_render
    nop
    mov.l   .Lmsg_render_timeout, r4
    bra     pvr_fatal
    nop
.Lrender_hardware_fault:
    mov.l   .Lmsg_render_fault, r4
    bra     pvr_fatal
    nop
.Lrender_done:
    mov     #4, r1
    mov.l   r1, @r0

    ! Flip only during vertical blank to avoid tearing.
    bsr     wait_vblank
    nop
    mov.l   .Lcurrent_fb, r0
    mov.l   @r0, r1
    mov.l   .Lpvr_base2, r14
    mov     #PVR_FB_ADDR, r0
    mov.l   r1, @(r0, r14)
    mov.l   .Lcable_type2, r0
    mov.l   @r0, r2
    tst     r2, r2
    bt      .Lflip_done
    mov.l   .Lline_bytes, r2
    add     r1, r2
    mov     #PVR_FB_IL_ADDR, r0
    mov.l   r2, @(r0, r14)
.Lflip_done:
    lds.l   @r15+, pr
    rts
    nop

wait_vblank:
    mov.l   .Lpvr_sync_addr, r0
    ! The low nine status bits are the current scanline counter. First leave
    ! scanline zero, then wait for the next transition back to zero; the
    ! caller's framebuffer write therefore lands exactly on the refresh edge.
    mov.l   .Lwait_budget, r3
.Lvbl_wait_active:
    mov.l   @r0, r1
    mov.l   .Lvbl_mask, r2
    and     r2, r1
    tst     r1, r1
    bf      .Lvbl_seen_active
    dt      r3
    bf      .Lvbl_wait_active
    nop
    mov.l   .Lmsg_vblank_timeout, r4
    bra     pvr_fatal
    nop
.Lvbl_seen_active:
    mov.l   .Lwait_budget, r3
.Lvbl_wait_zero:
    mov.l   @r0, r1
    and     r2, r1
    tst     r1, r1
    bt      .Lvbl_done
    dt      r3
    bf      .Lvbl_wait_zero
    nop
    mov.l   .Lmsg_vblank_timeout, r4
    bra     pvr_fatal
    nop
.Lvbl_done:
    rts
    nop

! r4 = diagnostic string. A failed hardware transaction is not safe to retry
! blindly; report it over SCIF and stop with interrupts still masked.
pvr_fatal:
    mov.l   .Lfatal_serial_puts, r0
    jsr     @r0
    nop
.Lpvr_fatal_halt:
    bra     .Lpvr_fatal_halt
    nop

    .align 2
.Lasic_ack:       .long ASIC_ACK_A
.Lasic_ack_c:     .long ASIC_ACK_C
.Lvert0:          .long VERT0
.Lvert1:          .long VERT1
.Lopb0c:          .long OPB0
.Lopb1c:          .long OPB1
.Ltile0c:         .long TILE0
.Ltile1c:         .long TILE1
.Lfb0c:           .long FB0
.Lfb1c:           .long FB1
.Lcurrent_vert:   .long current_vert
.Lcurrent_tile:   .long current_tile
.Lcurrent_fb:     .long current_fb
.Lpvr_base2:      .long PVR_BASE
.Lopb_total:      .long OPB_TOTAL
.Lopb_span:       .long OPB_SPAN
.Lopb_start_off:  .long PVR_TA_OPB_START
.Lopb_init_off:   .long PVR_TA_OPB_INIT
.Lopb_end_off:    .long PVR_TA_OPB_END
.Lvert_start_off: .long PVR_TA_VERTBUF_START
.Lvert_end_off:   .long PVR_TA_VERTBUF_END
.Lvert_size:      .long VERT_SIZE
.Ltile_cfg_off:   .long PVR_TILEMAT_CFG
.Ltile_cfg:       .long 0x000e0013
.Lopb_cfg_off:    .long PVR_OPB_CFG
.Lopb_cfg:        .long 0x00000303
.Lta_init_off:    .long PVR_TA_INIT
.Lta_go:          .long 0x80000000
.Lqacr0b:         .long QACR0
.Lqacr1b:         .long QACR1
.Lzero_block:     .long zero_block
.Lta_done_mask:   .long 0x00000280
.Lpvr_error_mask: .long 0x0000003f
.Lwait_budget:    .long 20000000
.Lfatal_serial_puts:.long serial_puts
.Lmsg_ta_timeout: .long msg_ta_timeout
.Lmsg_ta_fault:   .long msg_ta_fault
.Lmsg_render_timeout:.long msg_render_timeout
.Lmsg_render_fault:.long msg_render_fault
.Lmsg_vblank_timeout:.long msg_vblank_timeout
.Lvert_pos_off:   .long PVR_TA_VERTBUF_POS
.Lvram_base2:     .long PVR_RAM_32
.Lbkg_template:   .long background_plane
.Lbkg_cfg_prefix: .long 0x01000000
.Lisp_tile_off:   .long PVR_ISP_TILEMAT_ADDR
.Lbkg_cfg_off:    .long PVR_BGPLANE_CFG
.Lbkg_z_off:      .long PVR_BGPLANE_Z
.Lbkg_z:          .long 0x38d1b717     ! 0.0001f clipping tolerance
.Lpclip_x:        .long 0x027f0000
.Lpclip_y:        .long 0x01df0000
.Lcable_type2:    .long cable_type
.Lline_bytes:     .long 1280
.Lpvr_sync_addr:  .long PVR_BASE + PVR_SYNC_STATUS
.Lvbl_mask:       .long 0x000003ff     ! complete 10-bit SPG scanline field

! ---------------------------------------------------------------------------
! Scene choreography
! ---------------------------------------------------------------------------

draw_scene:
    sts.l   pr, @-r15
    mov.l   r9, @-r15

    ! The global frame counter is wrapped at 3072 by the main loop. Convert it
    ! here into a scene number and a private 0..767 local frame so every act
    ! can use the same compact time arithmetic and restart deterministically.
    mov.l   .Lscene_length, r1
    cmp/hs  r1, r9
    bt      .Lscene_dispatch_later
    mov     #0, r0
    mov.l   .Lscene_index_ptr, r2
    mov.l   r0, @r2
    mov.l   .Lfn_scene_orbit, r0
    jsr     @r0
    nop
    bra     .Lscene_dispatch_transition
    nop

.Lscene_dispatch_later:
    sub     r1, r9
    cmp/hs  r1, r9
    bt      .Lscene_dispatch_wave
    mov     #1, r0
    mov.l   .Lscene_index_ptr, r2
    mov.l   r0, @r2
    mov.l   .Lfn_scene_vault, r0
    jsr     @r0
    nop
    bra     .Lscene_dispatch_transition
    nop

.Lscene_dispatch_wave:
    sub     r1, r9
    cmp/hs  r1, r9
    bt      .Lscene_dispatch_hyper
    mov     #2, r0
    mov.l   .Lscene_index_ptr, r2
    mov.l   r0, @r2
    mov.l   .Lfn_scene_wave, r0
    jsr     @r0
    nop
    bra     .Lscene_dispatch_transition
    nop

.Lscene_dispatch_hyper:
    sub     r1, r9
    mov     #3, r0
    mov.l   .Lscene_index_ptr, r2
    mov.l   r0, @r2
    mov.l   .Lfn_scene_hyper, r0
    jsr     @r0
    nop

.Lscene_dispatch_transition:
    mov.l   .Lfn_scene_transition, r0
    jsr     @r0
    nop
    mov.l   @r15+, r9
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lscene_length:       .long 768
.Lscene_index_ptr:    .long scene_index
.Lfn_scene_orbit:     .long draw_scene_orbit
.Lfn_scene_vault:     .long draw_scene_vault
.Lfn_scene_wave:      .long draw_scene_wave
.Lfn_scene_hyper:     .long draw_scene_hyperfold
.Lfn_scene_transition:.long draw_scene_transition

draw_scene_orbit:
    sts.l   pr, @-r15

    ! Build one analytic spherical-orbit camera per frame. Every world-space
    ! vertex below shares this view, so the cost is paid once rather than per
    ! object and the whole scene banks coherently around the torus surface.
    mov.l   .Lfn_update_camera, r0
    jsr     @r0
    nop

    ! Opaque pass: a very distant color field and the generated 576-triangle
    ! torus. The reciprocal-Z depth buffer makes the centerpiece occlude every
    ! later holographic layer without any CPU sorting.
    mov.l   .Lscene_opaque_header, r4
    bsr     ta_submit
    nop
    mov.l   .Lscene_backdrop, r6
    mov     #4, r7
.Lscene_backdrop_loop:
    mov     r6, r4
    bsr     ta_submit
    nop
    add     #32, r6
    dt      r7
    bf      .Lscene_backdrop_loop
    nop
    mov.l   .Lfn_draw_torus, r0
    jsr     @r0
    nop
    ! The geometry HUD writes near depth in the opaque pass so later additive
    ! convergence cannot wash out the labels.
    mov.l   .Lfn_draw_title, r0
    jsr     @r0
    nop
    mov.l   .Lscene_zero, r4
    bsr     ta_submit
    nop

    ! Translucent pass: ONE/ONE blend turns low-intensity geometry into neon.
    mov.l   .Lscene_add_header, r4
    bsr     ta_submit
    nop
    mov.l   .Lfn_draw_core, r0
    jsr     @r0
    nop
    mov.l   .Lfn_draw_helicoid, r0
    jsr     @r0
    nop
    mov.l   .Lfn_draw_stars, r0
    jsr     @r0
    nop
    mov.l   .Lfn_draw_grid, r0
    jsr     @r0
    nop
    mov.l   .Lfn_draw_orbits, r0
    jsr     @r0
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lscene_opaque_header: .long opaque_header
.Lscene_backdrop:      .long backdrop_vertices
.Lscene_zero:          .long zero_block
.Lscene_add_header:    .long additive_header
.Lfn_update_camera:    .long update_camera
.Lfn_draw_torus:       .long draw_torus
.Lfn_draw_core:        .long draw_energy_core
.Lfn_draw_helicoid:    .long draw_helicoid
.Lfn_draw_stars:       .long draw_star_tunnel
.Lfn_draw_grid:        .long draw_grid
.Lfn_draw_orbits:      .long draw_orbits
.Lfn_draw_title:       .long draw_title

! Temporary fallbacks are replaced below by the independent scene renderers.
! Keeping the dispatcher linkable while each act is developed makes every
! intermediate revision boot-testable on the target.
draw_scene_vault:
    sts.l   pr, @-r15
    mov.l   .Lvault_fn_build, r0
    jsr     @r0
    nop

    ! Dark broken wall bays give the glowing architecture something solid to
    ! disappear behind; all other vault geometry is emitted in the next list.
    mov.l   .Lvault_opaque_header, r4
    bsr     ta_submit
    nop
    mov.l   .Lvault_backdrop, r6
    mov     #4, r7
.Lvault_backdrop_loop:
    mov     r6, r4
    bsr     ta_submit
    nop
    add     #32, r6
    dt      r7
    bf      .Lvault_backdrop_loop
    nop
    mov.l   .Lvault_fn_walls, r0
    jsr     @r0
    nop
    mov.l   .Lvault_fn_title, r0
    jsr     @r0
    nop
    mov.l   .Lvault_zero, r4
    bsr     ta_submit
    nop

    ! Portal annuli, longitudinal buttresses, and nested gates share ONE/ONE
    ! blending. Equal-depth wall conflicts are avoided by a tiny Z bias.
    mov.l   .Lvault_add_header, r4
    bsr     ta_submit
    nop
    mov.l   .Lvault_fn_arches, r0
    jsr     @r0
    nop
    mov.l   .Lvault_fn_rails, r0
    jsr     @r0
    nop
    mov.l   .Lvault_fn_gates, r0
    jsr     @r0
    nop

    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lvault_opaque_header:.long opaque_header
.Lvault_add_header:   .long additive_header
.Lvault_backdrop:     .long backdrop_vertices
.Lvault_zero:         .long zero_block
.Lvault_fn_build:     .long build_vault_cache
.Lvault_fn_walls:     .long draw_vault_walls
.Lvault_fn_arches:    .long draw_vault_arches
.Lvault_fn_rails:     .long draw_vault_rails
.Lvault_fn_gates:     .long draw_vault_gates
.Lvault_fn_title:     .long draw_title

draw_scene_wave:
    sts.l   pr, @-r15
    mov.l   .Lchaos_fn_camera, r0
    jsr     @r0
    nop

    mov.l   .Lchaos_opaque_header, r4
    bsr     ta_submit
    nop
    mov.l   .Lchaos_backdrop, r6
    mov     #4, r7
.Lchaos_backdrop_loop:
    mov     r6, r4
    bsr     ta_submit
    nop
    add     #32, r6
    dt      r7
    bf      .Lchaos_backdrop_loop
    nop
    mov.l   .Lchaos_fn_title, r0
    jsr     @r0
    nop
    mov.l   .Lchaos_zero, r4
    bsr     ta_submit
    nop

    mov.l   .Lchaos_add_header, r4
    bsr     ta_submit
    nop
    mov.l   .Lchaos_fn_advance, r0
    jsr     @r0
    nop
    mov.l   .Lchaos_fn_draw, r0
    jsr     @r0
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lchaos_opaque_header:.long opaque_header
.Lchaos_add_header:   .long additive_header
.Lchaos_backdrop:     .long backdrop_vertices
.Lchaos_zero:         .long zero_block
.Lchaos_fn_camera:    .long update_chaos_camera
.Lchaos_fn_title:     .long draw_title
.Lchaos_fn_advance:   .long attractor_advance
.Lchaos_fn_draw:      .long draw_attractor

draw_scene_transition:
    sts.l   pr, @-r15
    ! A symmetric 32-frame additive flash hides each hard geometry swap. The
    ! center of an act costs nothing; only its opening/closing half-second emits
    ! a single fullscreen quad.
    mov     r9, r0
    mov     #32, r1
    cmp/hs  r1, r0
    bt      .Ltransition_test_end
    mov     #32, r1
    sub     r0, r1                  ! 32..1 at scene entrance
    mov     r1, r0
    bra     .Ltransition_have_level
    nop
.Ltransition_test_end:
    mov.l   .Ltransition_end, r1
    cmp/hs  r1, r0
    bf      .Ltransition_done
    sub     r1, r0
    add     #1, r0                  ! 1..32 at scene exit
.Ltransition_have_level:
    shll2   r0
    shll    r0                      ! eight intensity steps per frame
    mov     r0, r1
    mov.l   .Ltransition_max, r2
    cmp/hi  r2, r1
    bf      .Ltransition_level_capped
    mov     r2, r1                  ! exact white at the scene boundary
.Ltransition_level_capped:
    mov     r1, r2
    shll8   r2
    or      r2, r1
    mov     r2, r3
    shll8   r3
    or      r3, r1
    mov.l   .Ltransition_alpha, r2
    or      r2, r1
    mov.l   .Ltransition_color_ptr, r0
    mov.l   r1, @r0
    mov.l   .Ltransition_depth_ptr, r0
    mov.l   .Ltransition_depth, r1
    mov.l   r1, @r0
    mov     #0, r4
    mov     #0, r5
    mov.l   .Ltransition_x1, r6
    mov.l   .Ltransition_y1, r7
    mov.l   .Ltransition_emit_rect, r0
    jsr     @r0
    nop
.Ltransition_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Ltransition_end:      .long 736
.Ltransition_max:      .long 255
.Ltransition_alpha:    .long 0xff000000
.Ltransition_color_ptr:.long quad_color
.Ltransition_depth_ptr:.long quad_depth
.Ltransition_depth:    .long 0x40800000 ! reciprocal-Z 4.0, over the HUD
.Ltransition_x1:       .long 640
.Ltransition_y1:       .long 480
.Ltransition_emit_rect:.long emit_rect

! ---------------------------------------------------------------------------
! Scene 02: Neon Fractal Vault
!
! Eighteen cached octagonal sections form a continuously advancing, curved
! cathedral. The cache is rebuilt once per frame; opaque broken wall bays and
! three additive architectural passes then reuse those points without paying
! for trigonometry again.
!
! Cache record (84 bytes): z, center X, center Y, then nine seam-closed XY
! pairs. All points are already in camera space for the dedicated projector.
! ---------------------------------------------------------------------------

build_vault_cache:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    ! Subtract the camera's own curve phase from every section center. This
    ! makes the nearest portal stay centered while distant portals bend away.
    mov     r9, r0
    shll8   r0
    shlr2   r0                    ! frame * 64
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lvault_ref_x_ptr, r0
    fmov.s  fr0, @r0

    mov     r9, r0
    shll2   r0
    shll    r0                    ! frame * 8
    mov     r9, r1
    shll2   r1
    shll2   r1
    shll    r1                    ! frame * 32
    add     r1, r0
    mov.l   .Lvault_y_phase, r1
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lvault_ref_y_ptr, r0
    fmov.s  fr0, @r0

    mov     r9, r0
    and     #31, r0
    lds     r0, fpul
    float   fpul, fr0
    mov.l   .Lvault_frac_scale, r0
    fmov.s  @r0, fr1
    fmul    fr1, fr0
    mov.l   .Lvault_frac_ptr, r0
    fmov.s  fr0, @r0

    mov     r9, r11
    shlr2   r11
    shlr2   r11
    shlr    r11                   ! floor(frame / 32)
    mov     #1, r12               ! ring ordinal 1..18
    mov     #18, r10
    mov.l   .Lvault_cache_ptr, r8

.Lvault_build_ring:
    ! z = 0.85 + ordinal - fractional forward travel.
    lds     r12, fpul
    float   fpul, fr8
    mov.l   .Lvault_z_base, r0
    fmov.s  @r0, fr9
    fadd    fr9, fr8
    mov.l   .Lvault_frac_ptr, r0
    fmov.s  @r0, fr9
    fsub    fr9, fr8
    fmov.s  fr8, @r8

    mov     r11, r13
    add     r12, r13               ! absolute tunnel section n

    ! Curved center X: .50 * (sin(2048*n) - camera reference).
    mov     r13, r0
    shll8   r0
    shll2   r0
    shll    r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lvault_ref_x_ptr, r0
    fmov.s  @r0, fr2
    fsub    fr2, fr0
    mov.l   .Lvault_curve_x, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    mov     r8, r0
    add     #4, r0
    fmov.s  fr0, @r0

    ! Curved center Y uses an incommensurate spatial frequency.
    mov     r13, r0
    shll8   r0                    ! n * 256
    mov     r0, r1
    shll2   r1                    ! n * 1024
    add     r1, r0                ! n * 1280
    mov.l   .Lvault_y_phase, r1
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lvault_ref_y_ptr, r0
    fmov.s  @r0, fr2
    fsub    fr2, fr0
    mov.l   .Lvault_curve_y, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    mov     r8, r0
    add     #8, r0
    fmov.s  fr0, @r0

    ! The octagon breathes independently of its continuous axial roll.
    mov     r13, r0
    shll8   r0
    shll2   r0
    shll2   r0                    ! n * 4096
    mov     r9, r1
    shll2   r1
    shll    r1                    ! frame * 8
    mov     r1, r2
    shll    r2                    ! frame * 16
    add     r2, r1                ! frame * 24
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lvault_radius_amp, r0
    fmov.s  @r0, fr10
    fmul    fr0, fr10
    mov.l   .Lvault_radius_base, r0
    fmov.s  @r0, fr11
    fadd    fr11, fr10             ! fr10 = section radius

    mov     r13, r0
    shll8   r0                    ! n * 256
    mov     r0, r1
    shll    r1
    add     r1, r0                ! n * 768
    shll    r0                    ! n * 1536
    mov     r9, r1
    shll2   r1
    shll2   r1                    ! frame * 16
    mov     r1, r2
    shll    r2                    ! frame * 32
    add     r2, r1                ! frame * 48
    add     r1, r0
    mov     r0, r13               ! first vertex angle
    mov     r8, r6
    add     #12, r6
    mov     #9, r14

.Lvault_build_point:
    lds     r13, fpul
    fsca    fpul, dr0             ! fr0=sin, fr1=cos
    fmov    fr1, fr2
    fmul    fr10, fr2
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr2
    fmov.s  fr2, @r6
    add     #4, r6
    fmov    fr0, fr2
    fmul    fr10, fr2
    mov     r8, r0
    add     #8, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr2
    fmov.s  fr2, @r6
    add     #4, r6
    mov.l   .Lvault_octant_step, r0
    add     r0, r13
    dt      r14
    bf      .Lvault_build_point
    nop

    add     #84, r8
    add     #1, r12
    dt      r10
    bf      .Lvault_build_ring
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lvault_cache_ptr:  .long vault_cache
.Lvault_ref_x_ptr:  .long vault_reference_x
.Lvault_ref_y_ptr:  .long vault_reference_y
.Lvault_frac_ptr:   .long vault_fraction
.Lvault_frac_scale: .long vault_fraction_scale
.Lvault_z_base:     .long vault_z_base
.Lvault_curve_x:    .long vault_curve_x
.Lvault_curve_y:    .long vault_curve_y
.Lvault_radius_base:.long vault_radius_base
.Lvault_radius_amp: .long vault_radius_amplitude
.Lvault_y_phase:    .long 0x00003000
.Lvault_octant_step:.long 8192

! Opaque quads between neighboring cached rings. Every fourth bay is omitted,
! producing windows that reveal the luminous ribs and recursive gates beyond.
draw_vault_walls:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lvault_walls_cache, r8
    ! Cache slot zero represents absolute section floor(frame/32)+1. Keep the
    ! broken-window and palette phases attached to that absolute section so
    ! the architecture does not pop when the sliding cache advances.
    mov     r9, r0
    shlr2   r0
    shlr2   r0
    shlr    r0
    add     #1, r0
    mov     r0, r9
    mov     #17, r10
    mov     #0, r11

.Lvault_wall_segment:
    mov     r8, r13
    add     #12, r13
    mov     r8, r14
    add     #96, r14               ! next record + point-array offset
    mov     #0, r12

.Lvault_wall_side:
    mov     r9, r0
    add     r11, r0
    add     r12, r0
    and     #3, r0
    tst     r0, r0
    bt      .Lvault_wall_skip

    mov     r9, r0
    add     r11, r0
    mov     r12, r1
    shll    r1
    add     r1, r0
    and     #7, r0
    shll2   r0
    mov.l   .Lvault_wall_palette, r1
    mov.l   @(r0, r1), r7

    ! Near j, far j, near j+1, far j+1 form one independent wall strip.
    fmov.s  @r13, fr7
    mov     r13, r0
    add     #4, r0
    fmov.s  @r0, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_vertex_normal, r6
    mov.l   .Lvault_project_fn, r0
    jsr     @r0
    nop

    fmov.s  @r14, fr7
    mov     r14, r0
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #84, r0
    fmov.s  @r0, fr9
    mov.l   .Lvault_vertex_normal, r6
    mov.l   .Lvault_project_fn, r0
    jsr     @r0
    nop

    mov     r13, r0
    add     #8, r0
    fmov.s  @r0, fr7
    add     #4, r0
    fmov.s  @r0, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_vertex_normal, r6
    mov.l   .Lvault_project_fn, r0
    jsr     @r0
    nop

    mov     r14, r0
    add     #8, r0
    fmov.s  @r0, fr7
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #84, r0
    fmov.s  @r0, fr9
    mov.l   .Lvault_vertex_eol, r6
    mov.l   .Lvault_project_fn, r0
    jsr     @r0
    nop

.Lvault_wall_skip:
    add     #8, r13
    add     #8, r14
    add     #1, r12
    mov     #8, r0
    cmp/hs  r0, r12
    bf      .Lvault_wall_side
    nop
    add     #84, r8
    add     #1, r11
    dt      r10
    bf      .Lvault_wall_segment
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lvault_walls_cache:   .long vault_cache
.Lvault_wall_palette:  .long vault_wall_palette
.Lvault_project_fn:    .long project_vault_vertex
.Lvault_vertex_normal: .long CMD_VERTEX
.Lvault_vertex_eol:    .long CMD_VERTEX_EOL

! A thick octagonal annulus at every cached section supplies the primary
! high-energy portal rhythm. Inner vertices are derived from the cached outer
! contour around each section center.
draw_vault_arches:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lvault_arch_cache, r8
    mov.l   .Lvault_arch_palette, r14
    mov     #18, r10
    mov     #0, r11

.Lvault_arch_ring:
    mov     r8, r13
    add     #12, r13
    mov     #0, r12
.Lvault_arch_side:
    mov     r11, r0
    add     r12, r0
    mov     r9, r1
    shlr2   r1
    add     r1, r0
    and     #7, r0
    shll2   r0
    mov.l   @(r0, r14), r7

    ! Outer j.
    fmov.s  @r13, fr7
    mov     r13, r0
    add     #4, r0
    fmov.s  @r0, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_arch_bias, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr9
    mov.l   .Lvault_arch_normal, r6
    mov.l   .Lvault_arch_project, r0
    jsr     @r0
    nop

    ! Inner j = center + .86 * (outer - center).
    fmov.s  @r13, fr7
    mov     r13, r0
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr7
    add     #4, r0
    fmov.s  @r0, fr11
    fsub    fr11, fr8
    mov.l   .Lvault_arch_inner, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr7
    fmul    fr12, fr8
    fadd    fr10, fr7
    fadd    fr11, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_arch_bias, r0
    fmov.s  @r0, fr12
    fsub    fr12, fr9
    mov.l   .Lvault_arch_normal, r6
    mov.l   .Lvault_arch_project, r0
    jsr     @r0
    nop

    ! Outer j+1.
    mov     r13, r0
    add     #8, r0
    fmov.s  @r0, fr7
    add     #4, r0
    fmov.s  @r0, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_arch_bias, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr9
    mov.l   .Lvault_arch_normal, r6
    mov.l   .Lvault_arch_project, r0
    jsr     @r0
    nop

    ! Inner j+1 closes the independent side strip.
    mov     r13, r0
    add     #8, r0
    fmov.s  @r0, fr7
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr7
    add     #4, r0
    fmov.s  @r0, fr11
    fsub    fr11, fr8
    mov.l   .Lvault_arch_inner, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr7
    fmul    fr12, fr8
    fadd    fr10, fr7
    fadd    fr11, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_arch_bias, r0
    fmov.s  @r0, fr12
    fsub    fr12, fr9
    mov.l   .Lvault_arch_eol, r6
    mov.l   .Lvault_arch_project, r0
    jsr     @r0
    nop

    add     #8, r13
    add     #1, r12
    mov     #8, r0
    cmp/hs  r0, r12
    bf      .Lvault_arch_side
    nop
    add     #84, r8
    add     #1, r11
    dt      r10
    bf      .Lvault_arch_ring
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lvault_arch_cache:  .long vault_cache
.Lvault_arch_palette:.long vault_arch_palette
.Lvault_arch_inner:  .long vault_arch_inner
.Lvault_arch_bias:   .long vault_depth_bias
.Lvault_arch_project:.long project_vault_vertex
.Lvault_arch_normal: .long CMD_VERTEX
.Lvault_arch_eol:    .long CMD_VERTEX_EOL

! Thin longitudinal strips connect matching octagon corners, turning the gate
! stack into a curved architectural volume rather than disconnected rings.
draw_vault_rails:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lvault_rail_cache, r8
    mov     #17, r10
    mov     #0, r11

.Lvault_rail_segment:
    mov     r8, r13
    add     #12, r13
    mov     r8, r14
    add     #96, r14
    mov     #0, r12
.Lvault_rail_side:
    mov     r11, r0
    add     r12, r0
    mov     r9, r1
    shlr2   r1
    shlr    r1
    add     r1, r0
    and     #7, r0
    shll2   r0
    mov.l   .Lvault_rail_palette, r1
    mov.l   @(r0, r1), r7

    ! Current outer point.
    fmov.s  @r13, fr7
    mov     r13, r0
    add     #4, r0
    fmov.s  @r0, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_rail_bias, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr9
    mov.l   .Lvault_rail_normal, r6
    mov.l   .Lvault_rail_project, r0
    jsr     @r0
    nop

    ! Current inner point at .94 radius.
    fmov.s  @r13, fr7
    mov     r13, r0
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr7
    add     #4, r0
    fmov.s  @r0, fr11
    fsub    fr11, fr8
    mov.l   .Lvault_rail_inner, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr7
    fmul    fr12, fr8
    fadd    fr10, fr7
    fadd    fr11, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_rail_bias, r0
    fmov.s  @r0, fr12
    fsub    fr12, fr9
    mov.l   .Lvault_rail_normal, r6
    mov.l   .Lvault_rail_project, r0
    jsr     @r0
    nop

    ! Next outer point.
    fmov.s  @r14, fr7
    mov     r14, r0
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #84, r0
    fmov.s  @r0, fr9
    mov.l   .Lvault_rail_bias, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr9
    mov.l   .Lvault_rail_normal, r6
    mov.l   .Lvault_rail_project, r0
    jsr     @r0
    nop

    ! Next inner point closes the buttress strip.
    fmov.s  @r14, fr7
    mov     r14, r0
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #88, r0               ! next record center X
    fmov.s  @r0, fr10
    fsub    fr10, fr7
    add     #4, r0
    fmov.s  @r0, fr11
    fsub    fr11, fr8
    mov.l   .Lvault_rail_inner, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr7
    fmul    fr12, fr8
    fadd    fr10, fr7
    fadd    fr11, fr8
    mov     r8, r0
    add     #84, r0
    fmov.s  @r0, fr9
    mov.l   .Lvault_rail_bias, r0
    fmov.s  @r0, fr12
    fsub    fr12, fr9
    mov.l   .Lvault_rail_eol, r6
    mov.l   .Lvault_rail_project, r0
    jsr     @r0
    nop

    add     #8, r13
    add     #8, r14
    add     #1, r12
    mov     #8, r0
    cmp/hs  r0, r12
    bf      .Lvault_rail_side
    nop
    add     #84, r8
    add     #1, r11
    dt      r10
    bf      .Lvault_rail_segment
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lvault_rail_cache:  .long vault_cache
.Lvault_rail_palette:.long vault_rail_palette
.Lvault_rail_inner:  .long vault_rail_inner
.Lvault_rail_bias:   .long vault_depth_bias
.Lvault_rail_project:.long project_vault_vertex
.Lvault_rail_normal: .long CMD_VERTEX
.Lvault_rail_eol:    .long CMD_VERTEX_EOL

! Every third section contains a second, floating annulus deep inside the main
! portal. The repeated scale hierarchy reads as a recursive fractal gate while
! costing only six additional octagons.
draw_vault_gates:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lvault_gate_cache, r8
    ! Choose the first cached record whose absolute section is divisible by
    ! three. The start index cycles 2,1,0 as floor(frame/32) cycles 0,1,2.
    mov     r9, r0
    shlr2   r0
    shlr2   r0
    shlr    r0
    mov     #3, r1
.Lvault_gate_mod3:
    cmp/hs  r1, r0
    bf      .Lvault_gate_mod_ready
    sub     r1, r0
    bra     .Lvault_gate_mod3
    nop
.Lvault_gate_mod_ready:
    tst     r0, r0
    bf      .Lvault_gate_mod_one
    mov     #84, r1
    add     r1, r8
    add     r1, r8                 ! remainder 0: start at cache record 2
    bra     .Lvault_gate_start_ready
    nop
.Lvault_gate_mod_one:
    mov     #1, r1
    cmp/eq  r1, r0
    bf      .Lvault_gate_start_ready
    mov     #84, r1
    add     r1, r8                 ! remainder 1: start at cache record 1
.Lvault_gate_start_ready:           ! remainder 2 already starts at record 0
    mov.l   .Lvault_gate_palette, r14
    mov     #6, r10
    mov     #0, r11

.Lvault_gate_ring:
    mov     r8, r13
    add     #12, r13
    mov     #0, r12
.Lvault_gate_side:
    mov     r11, r0
    add     r12, r0
    mov     r9, r1
    shlr    r1
    add     r1, r0
    and     #7, r0
    shll2   r0
    mov.l   @(r0, r14), r7

    ! Four scaled points make one nested annulus side.
    fmov.s  @r13, fr7
    mov     r13, r0
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr7
    add     #4, r0
    fmov.s  @r0, fr11
    fsub    fr11, fr8
    mov.l   .Lvault_gate_outer, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr7
    fmul    fr12, fr8
    fadd    fr10, fr7
    fadd    fr11, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_gate_bias, r0
    fmov.s  @r0, fr12
    fsub    fr12, fr9
    mov.l   .Lvault_gate_normal, r6
    mov.l   .Lvault_gate_project, r0
    jsr     @r0
    nop

    fmov.s  @r13, fr7
    mov     r13, r0
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr7
    add     #4, r0
    fmov.s  @r0, fr11
    fsub    fr11, fr8
    mov.l   .Lvault_gate_inner, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr7
    fmul    fr12, fr8
    fadd    fr10, fr7
    fadd    fr11, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_gate_bias, r0
    fmov.s  @r0, fr12
    fsub    fr12, fr9
    mov.l   .Lvault_gate_normal, r6
    mov.l   .Lvault_gate_project, r0
    jsr     @r0
    nop

    mov     r13, r0
    add     #8, r0
    fmov.s  @r0, fr7
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr7
    add     #4, r0
    fmov.s  @r0, fr11
    fsub    fr11, fr8
    mov.l   .Lvault_gate_outer, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr7
    fmul    fr12, fr8
    fadd    fr10, fr7
    fadd    fr11, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_gate_bias, r0
    fmov.s  @r0, fr12
    fsub    fr12, fr9
    mov.l   .Lvault_gate_normal, r6
    mov.l   .Lvault_gate_project, r0
    jsr     @r0
    nop

    mov     r13, r0
    add     #8, r0
    fmov.s  @r0, fr7
    add     #4, r0
    fmov.s  @r0, fr8
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr10
    fsub    fr10, fr7
    add     #4, r0
    fmov.s  @r0, fr11
    fsub    fr11, fr8
    mov.l   .Lvault_gate_inner, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr7
    fmul    fr12, fr8
    fadd    fr10, fr7
    fadd    fr11, fr8
    fmov.s  @r8, fr9
    mov.l   .Lvault_gate_bias, r0
    fmov.s  @r0, fr12
    fsub    fr12, fr9
    mov.l   .Lvault_gate_eol, r6
    mov.l   .Lvault_gate_project, r0
    jsr     @r0
    nop

    add     #8, r13
    add     #1, r12
    mov     #8, r0
    cmp/hs  r0, r12
    bf      .Lvault_gate_side
    nop
    mov.l   .Lvault_gate_stride, r0
    add     r0, r8
    add     #1, r11
    dt      r10
    bf      .Lvault_gate_ring
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lvault_gate_cache:  .long vault_cache
.Lvault_gate_palette:.long vault_gate_palette
.Lvault_gate_outer:  .long vault_gate_outer
.Lvault_gate_inner:  .long vault_gate_inner
.Lvault_gate_bias:   .long vault_depth_bias
.Lvault_gate_stride: .long 252
.Lvault_gate_project:.long project_vault_vertex
.Lvault_gate_normal: .long CMD_VERTEX
.Lvault_gate_eol:    .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Scene 03: Chaos Bloom
!
! A live Lorenz system feeds a 512-point circular history. The initial path is
! warmed before video starts, then twelve nonlinear Euler substeps and three
! replacement samples are computed per displayed frame. No trajectory table is
! stored in the ELF.
! ---------------------------------------------------------------------------

! Advance the raw Lorenz state once. All deltas use the old x/y/z values.
lorenz_step:
    mov.l   .Llorenz_state_step, r0
    fmov.s  @r0, fr0              ! x
    add     #4, r0
    fmov.s  @r0, fr1              ! y
    add     #4, r0
    fmov.s  @r0, fr2              ! z

    fmov    fr1, fr3
    fsub    fr0, fr3
    mov.l   .Llorenz_sigma_dt, r0
    fmov.s  @r0, fr6
    fmul    fr6, fr3              ! dx

    mov.l   .Llorenz_rho_step, r0
    fmov.s  @r0, fr6
    fsub    fr2, fr6
    fmul    fr0, fr6
    fsub    fr1, fr6
    mov.l   .Llorenz_dt, r0
    fmov.s  @r0, fr7
    fmul    fr7, fr6              ! dy

    fmov    fr0, fr4
    fmul    fr1, fr4
    mov.l   .Llorenz_beta, r0
    fmov.s  @r0, fr5
    fmul    fr2, fr5
    fsub    fr5, fr4
    fmul    fr7, fr4              ! dz

    fadd    fr3, fr0
    fadd    fr6, fr1
    fadd    fr4, fr2
    mov.l   .Llorenz_state_step, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0
    add     #4, r0
    fmov.s  fr2, @r0
    rts
    nop

    .align 2
.Llorenz_state_step:.long lorenz_state
.Llorenz_rho_step:  .long lorenz_rho
.Llorenz_sigma_dt:  .long lorenz_sigma_dt
.Llorenz_dt:        .long lorenz_dt
.Llorenz_beta:      .long lorenz_beta

! Convert the current raw state into bounded world coordinates and store at r8.
! Clobbers r0-r1 and fr0-fr5; r8 is advanced by one 12-byte point.
lorenz_store_point:
    mov.l   .Llorenz_state_store, r0
    fmov.s  @r0, fr0              ! raw x
    add     #4, r0
    fmov.s  @r0, fr1              ! raw y
    add     #4, r0
    fmov.s  @r0, fr2              ! raw z
    mov.l   .Llorenz_scale_x, r0
    fmov.s  @r0, fr3
    fmul    fr3, fr0
    mov.l   .Llorenz_z_center, r0
    fmov.s  @r0, fr3
    fsub    fr3, fr2
    mov.l   .Llorenz_scale_y, r0
    fmov.s  @r0, fr3
    fmul    fr3, fr2
    mov.l   .Llorenz_scale_z, r0
    fmov.s  @r0, fr3
    fmul    fr3, fr1
    fmov.s  fr0, @r8              ! world X
    add     #4, r8
    fmov.s  fr2, @r8              ! world Y = centered raw Z
    add     #4, r8
    fmov.s  fr1, @r8              ! world Z = raw Y
    add     #4, r8
    rts
    nop

    .align 2
.Llorenz_state_store:.long lorenz_state
.Llorenz_scale_x:   .long lorenz_world_scale_x
.Llorenz_scale_y:   .long lorenz_world_scale_y
.Llorenz_scale_z:   .long lorenz_world_scale_z
.Llorenz_z_center:  .long lorenz_world_center_z

lorenz_init:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15

    mov.l   .Llorenz_init_state, r0
    mov.l   .Llorenz_seed, r1
    mov.l   r1, @r0
    mov     #0, r1
    add     #4, r0
    mov.l   r1, @r0
    add     #4, r0
    mov.l   r1, @r0
    mov.l   .Llorenz_init_rho, r0
    mov.l   .Llorenz_rho_base, r1
    mov.l   r1, @r0

    mov.l   .Llorenz_warm_count, r10
.Llorenz_warm_loop:
    bsr     lorenz_step
    nop
    dt      r10
    bf      .Llorenz_warm_loop
    nop

    mov.l   .Llorenz_buffer_init, r8
    mov.l   .Llorenz_point_count, r10
.Llorenz_fill_loop:
    mov     #4, r11
.Llorenz_fill_substep:
    bsr     lorenz_step
    nop
    dt      r11
    bf      .Llorenz_fill_substep
    nop
    bsr     lorenz_store_point
    nop
    dt      r10
    bf      .Llorenz_fill_loop
    nop
    mov.l   .Llorenz_head_init, r0
    mov     #0, r1
    mov.l   r1, @r0

    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Llorenz_init_state: .long lorenz_state
.Llorenz_init_rho:   .long lorenz_rho
.Llorenz_seed:       .long 0x3dcccccd
.Llorenz_rho_base:   .long 0x41e00000
.Llorenz_warm_count: .long 1536
.Llorenz_point_count:.long 512
.Llorenz_buffer_init:.long lorenz_points
.Llorenz_head_init:  .long lorenz_head

attractor_advance:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15

    ! rho = 28 + 3*sin(frame*256): three parameter breaths per act.
    mov     r9, r0
    shll8   r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lattractor_rho_amp, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    mov.l   .Lattractor_rho_base, r0
    fmov.s  @r0, fr2
    fadd    fr2, fr0
    mov.l   .Lattractor_rho_ptr, r0
    fmov.s  fr0, @r0

    mov     #3, r10
.Lattractor_new_sample:
    mov     #4, r11
.Lattractor_substep:
    bsr     lorenz_step
    nop
    dt      r11
    bf      .Lattractor_substep
    nop

    mov.l   .Lattractor_head_ptr, r0
    mov.l   @r0, r1
    mov     r1, r8
    shll2   r8                    ! head * 4
    mov     r1, r2
    shll2   r2
    shll    r2                    ! head * 8
    add     r2, r8                ! head * 12
    mov.l   .Lattractor_buffer, r2
    add     r2, r8
    bsr     lorenz_store_point
    nop

    mov.l   .Lattractor_head_ptr, r0
    mov.l   @r0, r1
    add     #1, r1
    mov.l   .Lattractor_head_mask, r2
    and     r2, r1
    mov.l   r1, @r0
    dt      r10
    bf      .Lattractor_new_sample
    nop

    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lattractor_rho_amp: .long lorenz_rho_amplitude
.Lattractor_rho_base:.long lorenz_rho_base
.Lattractor_rho_ptr: .long lorenz_rho
.Lattractor_head_ptr:.long lorenz_head
.Lattractor_head_mask:.long 511
.Lattractor_buffer:  .long lorenz_points

draw_attractor:
    sts.l   pr, @-r15
    ! Symmetric geometry envelope blooms the field out of the white transition.
    mov     r9, r0
    mov.l   .Lattractor_last_frame, r1
    sub     r9, r1
    cmp/hi  r1, r0
    bf      .Lattractor_edge_ready
    mov     r1, r0
.Lattractor_edge_ready:
    shll2   r0
    mov.l   .Lattractor_env_max, r1
    cmp/hi  r1, r0
    bf      .Lattractor_env_capped
    mov     r1, r0
.Lattractor_env_capped:
    mov     #8, r1
    cmp/hs  r1, r0
    bf      .Lattractor_draw_done
    lds     r0, fpul
    float   fpul, fr0
    mov.l   .Lattractor_env_scale, r0
    fmov.s  @r0, fr1
    fmul    fr1, fr0
    mov.l   .Lattractor_env_ptr, r0
    fmov.s  fr0, @r0

    ! Spark size has its own faster pulse but follows the geometry envelope.
    mov     r9, r0
    shll8   r0
    shll    r0
    lds     r0, fpul
    fsca    fpul, dr2
    mov.l   .Lattractor_spark_amp, r0
    fmov.s  @r0, fr4
    fmul    fr2, fr4
    mov.l   .Lattractor_spark_base, r0
    fmov.s  @r0, fr5
    fadd    fr5, fr4
    fmul    fr0, fr4
    mov.l   .Lattractor_spark_ptr, r0
    fmov.s  fr4, @r0

    mov.l   .Lattractor_fn_broad, r0
    jsr     @r0
    nop
    mov.l   .Lattractor_fn_hot, r0
    jsr     @r0
    nop
    mov.l   .Lattractor_fn_sparks, r0
    jsr     @r0
    nop
.Lattractor_draw_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lattractor_last_frame:.long 767
.Lattractor_env_max:  .long 255
.Lattractor_env_scale:.long lorenz_env_scale
.Lattractor_env_ptr:  .long attractor_envelope
.Lattractor_spark_amp:.long attractor_spark_amplitude
.Lattractor_spark_base:.long attractor_spark_base
.Lattractor_spark_ptr:.long attractor_spark_size
.Lattractor_fn_broad: .long draw_attractor_broad
.Lattractor_fn_hot:   .long draw_attractor_hot
.Lattractor_fn_sparks:.long draw_attractor_sparks

draw_attractor_broad:
    mov     #0, r4                  ! Y-axis ribbon thickness
    mov.l   .Lattractor_broad_width, r5
    mov.l   .Lattractor_broad_colors, r6
    bra     draw_attractor_ribbon
    nop
    .align 2
.Lattractor_broad_width: .long attractor_broad_width
.Lattractor_broad_colors:.long chaos_broad_palette

draw_attractor_hot:
    mov     #1, r4                  ! X-axis ribbon thickness
    mov.l   .Lattractor_hot_width, r5
    mov.l   .Lattractor_hot_colors, r6
    bra     draw_attractor_ribbon
    nop
    .align 2
.Lattractor_hot_width: .long attractor_hot_width
.Lattractor_hot_colors:.long chaos_hot_palette

! r4=offset axis (0 Y, 1 X), r5=width pointer, r6=16-color palette.
draw_attractor_ribbon:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov     r4, r12
    mov     r5, r13
    mov     r6, r14

    mov.l   .Lattractor_ribbon_head, r0
    mov.l   @r0, r1
    mov     r1, r8
    shll2   r8
    mov     r1, r2
    shll2   r2
    shll    r2
    add     r2, r8
    mov.l   .Lattractor_ribbon_base, r2
    add     r2, r8
    mov.l   .Lattractor_ribbon_end, r9
    mov.l   .Lattractor_ribbon_count, r10
    mov     #0, r11

.Lattractor_ribbon_point:
    ! Age controls brightness; the IEEE sign bit of X chooses cyan/magenta.
    mov.l   @r8, r0
    mov     r11, r1
    shlr2   r1
    shlr2   r1
    shlr2   r1                    ! age / 64 = 0..7
    cmp/pz  r0
    bf      .Lattractor_ribbon_color_ready
    add     #8, r1
.Lattractor_ribbon_color_ready:
    shll2   r1
    mov     r1, r0
    mov.l   @(r0, r14), r7

    ! Negative side of the ribbon.
    fmov.s  @r8, fr7
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr8
    add     #4, r0
    fmov.s  @r0, fr9
    mov.l   .Lattractor_ribbon_env, r0
    fmov.s  @r0, fr6
    fmul    fr6, fr7
    fmul    fr6, fr8
    fmul    fr6, fr9
    fmov.s  @r13, fr10
    fmul    fr6, fr10
    tst     r12, r12
    bf      .Lattractor_ribbon_minus_x
    fsub    fr10, fr8
    bra     .Lattractor_ribbon_minus_ready
    nop
.Lattractor_ribbon_minus_x:
    fsub    fr10, fr7
.Lattractor_ribbon_minus_ready:
    mov.l   .Lattractor_ribbon_normal, r6
    mov.l   .Lattractor_ribbon_project, r0
    jsr     @r0
    nop

    ! Positive side; terminate only the final point in this continuous strip.
    fmov.s  @r8, fr7
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr8
    add     #4, r0
    fmov.s  @r0, fr9
    mov.l   .Lattractor_ribbon_env, r0
    fmov.s  @r0, fr6
    fmul    fr6, fr7
    fmul    fr6, fr8
    fmul    fr6, fr9
    fmov.s  @r13, fr10
    fmul    fr6, fr10
    tst     r12, r12
    bf      .Lattractor_ribbon_plus_x
    fadd    fr10, fr8
    bra     .Lattractor_ribbon_plus_ready
    nop
.Lattractor_ribbon_plus_x:
    fadd    fr10, fr7
.Lattractor_ribbon_plus_ready:
    mov.l   .Lattractor_ribbon_normal, r6
    mov     #1, r0
    cmp/eq  r0, r10
    bf      .Lattractor_ribbon_not_eol
    mov.l   .Lattractor_ribbon_eol, r6
.Lattractor_ribbon_not_eol:
    mov.l   .Lattractor_ribbon_project, r0
    jsr     @r0
    nop

    add     #12, r8
    cmp/hs  r9, r8
    bf      .Lattractor_ribbon_no_wrap
    mov.l   .Lattractor_ribbon_base, r8
.Lattractor_ribbon_no_wrap:
    add     #1, r11
    dt      r10
    bf      .Lattractor_ribbon_point
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lattractor_ribbon_head:   .long lorenz_head
.Lattractor_ribbon_base:   .long lorenz_points
.Lattractor_ribbon_end:    .long lorenz_points + 512 * 12
.Lattractor_ribbon_count:  .long 512
.Lattractor_ribbon_env:    .long attractor_envelope
.Lattractor_ribbon_normal: .long CMD_VERTEX
.Lattractor_ribbon_eol:    .long CMD_VERTEX_EOL
.Lattractor_ribbon_project:.long project_world_vertex

draw_attractor_sparks:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lattractor_sparks_head, r0
    mov.l   @r0, r1
    mov     r1, r8
    shll2   r8
    mov     r1, r2
    shll2   r2
    shll    r2
    add     r2, r8
    mov.l   .Lattractor_sparks_base, r14
    add     r14, r8
    mov.l   .Lattractor_sparks_end, r9
    mov     #64, r10
    mov     #0, r11

.Lattractor_spark_point:
    ! Every eighth spark is a hot gold control point; the rest inherit the
    ! cyan/magenta wing split directly from the stored float sign bit.
    mov     r11, r0
    and     #7, r0
    tst     r0, r0
    bt      .Lattractor_spark_gold
    mov.l   @r8, r0
    cmp/pz  r0
    bf      .Lattractor_spark_cyan
    mov.l   .Lattractor_spark_magenta, r7
    bra     .Lattractor_spark_color_ready
    nop
.Lattractor_spark_cyan:
    mov.l   .Lattractor_spark_cyan_color, r7
    bra     .Lattractor_spark_color_ready
    nop
.Lattractor_spark_gold:
    mov.l   .Lattractor_spark_gold_color, r7
.Lattractor_spark_color_ready:

    ! Cache this point after applying the act envelope; six offset-table
    ! vertices below turn it into two crossed triangular shards.
    fmov.s  @r8, fr0
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr1
    add     #4, r0
    fmov.s  @r0, fr2
    mov.l   .Lattractor_sparks_env, r0
    fmov.s  @r0, fr3
    fmul    fr3, fr0
    fmul    fr3, fr1
    fmul    fr3, fr2
    mov.l   .Lattractor_sparks_xyz, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0
    add     #4, r0
    fmov.s  fr2, @r0

    mov.l   .Lattractor_sparks_size, r0
    fmov.s  @r0, fr0
    mov     r11, r0
    and     #7, r0
    tst     r0, r0
    bf      .Lattractor_spark_size_ready
    fadd    fr0, fr0
.Lattractor_spark_size_ready:
    mov.l   .Lattractor_sparks_current_size, r0
    fmov.s  fr0, @r0

    mov.l   .Lattractor_spark_offsets, r12
    mov     #0, r13
.Lattractor_spark_vertex_loop:
    mov     r12, r4
    mov.l   .Lattractor_spark_normal, r5
    mov     #2, r0
    cmp/eq  r0, r13
    bt      .Lattractor_spark_vertex_eol
    mov     #5, r0
    cmp/eq  r0, r13
    bf      .Lattractor_spark_vertex_command_ready
.Lattractor_spark_vertex_eol:
    mov.l   .Lattractor_spark_eol, r5
.Lattractor_spark_vertex_command_ready:
    bsr     attractor_spark_vertex
    nop
    add     #12, r12
    add     #1, r13
    mov     #6, r0
    cmp/hs  r0, r13
    bf      .Lattractor_spark_vertex_loop
    nop

    add     #96, r8               ! every eighth trajectory sample
    cmp/hs  r9, r8
    bf      .Lattractor_spark_no_wrap
    mov.l   .Lattractor_sparks_bytes, r0
    sub     r0, r8
.Lattractor_spark_no_wrap:
    add     #1, r11
    dt      r10
    bf      .Lattractor_spark_point
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

! r4=offset vector, r5=command, r7=color. Tail-chain through the world camera.
attractor_spark_vertex:
    mov.l   .Lattractor_spark_xyz_ptr, r0
    fmov.s  @r0, fr7
    add     #4, r0
    fmov.s  @r0, fr8
    add     #4, r0
    fmov.s  @r0, fr9
    mov.l   .Lattractor_spark_current_ptr, r0
    fmov.s  @r0, fr6
    fmov.s  @r4, fr0
    add     #4, r4
    fmov.s  @r4, fr1
    add     #4, r4
    fmov.s  @r4, fr2
    fmul    fr6, fr0
    fmul    fr6, fr1
    fmul    fr6, fr2
    fadd    fr0, fr7
    fadd    fr1, fr8
    fadd    fr2, fr9
    mov     r5, r6
    bra     project_world_vertex
    nop

    .align 2
.Lattractor_sparks_head: .long lorenz_head
.Lattractor_sparks_base: .long lorenz_points
.Lattractor_sparks_end:  .long lorenz_points + 512 * 12
.Lattractor_sparks_bytes:.long 512 * 12
.Lattractor_sparks_env:  .long attractor_envelope
.Lattractor_sparks_xyz:  .long attractor_xyz
.Lattractor_sparks_size: .long attractor_spark_size
.Lattractor_sparks_current_size:.long attractor_current_spark_size
.Lattractor_spark_offsets:.long attractor_spark_offsets
.Lattractor_spark_normal: .long CMD_VERTEX
.Lattractor_spark_eol:    .long CMD_VERTEX_EOL
.Lattractor_spark_cyan_color:.long 0xff18a0c0
.Lattractor_spark_magenta:.long 0xffc03070
.Lattractor_spark_gold_color:.long 0xffffb050
.Lattractor_spark_xyz_ptr:.long attractor_xyz
.Lattractor_spark_current_ptr:.long attractor_current_spark_size

! ---------------------------------------------------------------------------
! Camera choreography
!
! Orbit Core is cut like a miniature camera shot rather than an endless sine
! wave. Frames 0..119 hold a wide, elevated establishing view. Frames 120..247
! cosine-ease a hard dolly from radius 8.6 to 3.2 while the lens levels with
! the torus. The remaining shot accelerates into a close spherical rail: two
! fast major-axis laps and almost three pitch loops, with radius closest at
! every equator crossing and roll driven by twice the pitch phase. That
! Lissajous path reads as a camera corkscrewing around the torus surface while
! keeping every vertex in front of the unclipped reciprocal-Z projector.
! ---------------------------------------------------------------------------

update_camera:
    ! Keep the opening pan deliberately slow. At frame 248 the yaw rate jumps
    ! from 64 to 256 binary-angle units per frame without changing position.
    mov     r9, r0
    mov     #124, r1
    shll    r1                      ! 248: end of the dive
    cmp/hs  r1, r0
    bt      .Lcam_close_yaw
    shll8   r0
    shlr2   r0                      ! opening yaw = frame * 64
    bra     .Lcam_yaw_ready
    nop

.Lcam_close_yaw:
    sub     r1, r0                  ! close local frame
    shll8   r0                      ! close yaw = local * 256
    mov.l   .Lcam_dive_end_yaw, r1
    add     r1, r0                  ! continuity with 248 * 64

.Lcam_yaw_ready:
    lds     r0, fpul
    fsca    fpul, dr0              ! fr0 = sin yaw, fr1 = cos yaw
    mov.l   .Lcam_yaw_store, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0

    ! The first two seconds are a stable wide composition. Holding a readable
    ! torus silhouette makes the later change in scale impossible to mistake
    ! for object rotation.
    mov     r9, r0
    mov     #120, r1
    cmp/hs  r1, r0
    bt      .Lcam_dive_or_close

    mov.l   .Lcam_establish_pitch, r0
    lds     r0, fpul
    fsca    fpul, dr4              ! fixed +22.5 degree reveal
    fldi0   fr6
    fldi1   fr7
    mov.l   .Lcam_radius_far, r0
    fmov.s  @r0, fr9               ! radius 8.6
    bra     .Lcam_store_attitude
    nop

.Lcam_dive_or_close:
    mov     #124, r2
    shll    r2                      ! 248
    cmp/hs  r2, r0
    bt      .Lcam_close_rail

    ! Dive frame 0..127. angle spans [0,pi), so 5.9 + 2.7*cos(angle)
    ! eases exactly from 8.6 toward 3.2 with zero velocity at both ends.
    sub     r1, r0                  ! dive frame = frame - 120
    mov     r0, r2
    shll8   r0
    lds     r0, fpul
    fsca    fpul, dr2              ! fr2 = sin ease, fr3 = cos ease
    mov.l   .Lcam_radius_dive_mid, r0
    fmov.s  @r0, fr9
    mov.l   .Lcam_radius_dive_half, r0
    fmov.s  @r0, fr10
    fmul    fr3, fr10
    fadd    fr10, fr9

    ! Level the opening +22.5 degree view linearly during the dive.
    mov     r2, r0
    shll2   r0
    shll2   r0
    shll    r0                      ! dive frame * 32
    mov.l   .Lcam_establish_pitch, r1
    sub     r0, r1
    lds     r1, fpul
    fsca    fpul, dr4

    ! A restrained half-sine bank sells acceleration but returns to level at
    ! the splice into the close rail.
    fmov    fr2, fr10
    mov.l   .Lcam_dive_roll_units, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr6
    bra     .Lcam_store_attitude
    nop

.Lcam_close_rail:
    sub     r2, r0                  ! close frame = frame - 248

    ! Pitch phase = close frame * 384. It winds almost three times before the
    ! exit flash, while the 256-unit yaw completes almost two major-axis laps.
    mov     r0, r1
    shll8   r0
    shll8   r1
    shlr    r1
    add     r1, r0
    mov     r0, r2                  ! preserve phase for its second harmonic
    lds     r0, fpul
    fsca    fpul, dr2               ! fr2 = pitch wave, fr3 = quadrature

    fmov    fr2, fr10
    mov.l   .Lcam_close_pitch_units, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr4              ! pitch sweeps +/-22.5 degrees

    ! The second harmonic makes every equator crossing a close pass. Radius
    ! breathes 3.2..4.2: the skim fills the lens, then the climb restores a
    ! readable full-ring silhouette. Both stay beyond the 2.95 scene envelope.
    mov     r2, r0
    shll    r0
    lds     r0, fpul
    fsca    fpul, dr8              ! fr8 = sin(2p), fr9 = cos(2p)
    mov.l   .Lcam_radius_close_amp, r0
    fmov.s  @r0, fr10
    fmul    fr9, fr10
    mov.l   .Lcam_radius_close_base, r0
    fmov.s  @r0, fr9
    fsub    fr10, fr9

    ! Bank changes direction through each climb/dive and is level at the two
    ! surface-skimming equator beats.
    fmov    fr8, fr10
    mov.l   .Lcam_close_roll_units, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr6

.Lcam_store_attitude:
    mov.l   .Lcam_pitch_store, r0
    fmov.s  fr4, @r0
    add     #4, r0
    fmov.s  fr5, @r0
    mov.l   .Lcam_roll_store, r0
    fmov.s  fr6, @r0
    add     #4, r0
    fmov.s  fr7, @r0

    ! Camera position on the same analytic sphere as the view basis.
    fmov    fr9, fr10
    fmul    fr5, fr10              ! radius * cos pitch
    fmov    fr10, fr11
    fmul    fr0, fr11              ! X = R*cp*sin(yaw)
    fmov    fr9, fr12
    fmul    fr4, fr12              ! Y = R*sin(pitch)
    fmov    fr10, fr13
    fmul    fr1, fr13              ! Z = R*cp*cos(yaw)
    mov.l   .Lcam_pos_store, r0
    fmov.s  fr11, @r0
    add     #4, r0
    fmov.s  fr12, @r0
    add     #4, r0
    fmov.s  fr13, @r0
    rts
    nop

    .align 2
.Lcam_yaw_store:   .long camera_yaw
.Lcam_pitch_store: .long camera_pitch
.Lcam_roll_store:  .long camera_roll
.Lcam_pos_store:   .long camera_position
.Lcam_dive_end_yaw:       .long 15872
.Lcam_establish_pitch:    .long 4096
.Lcam_radius_far:         .long camera_radius_far
.Lcam_radius_dive_mid:    .long camera_radius_dive_mid
.Lcam_radius_dive_half:   .long camera_radius_dive_half
.Lcam_dive_roll_units:    .long camera_dive_roll_units
.Lcam_close_pitch_units:  .long camera_close_pitch_units
.Lcam_close_roll_units:   .long camera_close_roll_units
.Lcam_radius_close_base:  .long camera_radius_close_base
.Lcam_radius_close_amp:   .long camera_radius_close_amplitude

! The strange attractor benefits from a closer, calmer inspection orbit than
! the torus flyby. It still uses the same analytic camera representation, but
! holds a 4.35 radius and limits pitch/roll so the butterfly remains legible.
update_chaos_camera:
    mov     r9, r0
    shll8   r0
    mov     r9, r1
    shll8   r1
    shlr2   r1
    sub     r1, r0                 ! yaw = frame * 192
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lchaos_cam_yaw, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0

    mov     r9, r0
    shll8   r0
    shlr2   r0                    ! slow wave = frame * 64
    lds     r0, fpul
    fsca    fpul, dr2
    fmov    fr2, fr10
    mov.l   .Lchaos_cam_pitch_units, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr4
    mov.l   .Lchaos_cam_pitch, r0
    fmov.s  fr4, @r0
    add     #4, r0
    fmov.s  fr5, @r0

    fmov    fr3, fr10
    mov.l   .Lchaos_cam_roll_units, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr6
    mov.l   .Lchaos_cam_roll, r0
    fmov.s  fr6, @r0
    add     #4, r0
    fmov.s  fr7, @r0

    mov.l   .Lchaos_cam_radius, r0
    fmov.s  @r0, fr9
    fmov    fr9, fr10
    fmul    fr5, fr10
    fmov    fr10, fr11
    fmul    fr0, fr11
    fmov    fr9, fr12
    fmul    fr4, fr12
    fmov    fr10, fr13
    fmul    fr1, fr13
    mov.l   .Lchaos_cam_position, r0
    fmov.s  fr11, @r0
    add     #4, r0
    fmov.s  fr12, @r0
    add     #4, r0
    fmov.s  fr13, @r0
    rts
    nop

    .align 2
.Lchaos_cam_yaw:        .long camera_yaw
.Lchaos_cam_pitch:      .long camera_pitch
.Lchaos_cam_roll:       .long camera_roll
.Lchaos_cam_position:   .long camera_position
.Lchaos_cam_radius:     .long chaos_camera_radius
.Lchaos_cam_pitch_units:.long chaos_camera_pitch_units
.Lchaos_cam_roll_units: .long chaos_camera_roll_units

! ---------------------------------------------------------------------------
! Parametric torus
!
! The SH-4 FSCA instruction produces sine and cosine together from a 16-bit
! binary angle. Every vertex is generated, transformed by the live camera,
! perspective-divided, shaded, and streamed straight into PVR2.
! ---------------------------------------------------------------------------

draw_torus:
    sts.l   pr, @-r15

    mov.l   .Ltorus_palette, r14
    mov     #24, r10               ! longitudinal strips
    mov     #0, r11                 ! world-lock facets so camera yaw is visible

.Ltorus_strip_loop:
    mov     #13, r13               ! 12 sides, repeat seam vertex
    mov     #0, r12                 ! fixed V mesh; palette still pulses in time

.Ltorus_side_loop:
    ! Keep the rainbow mostly world-locked: a slow two-step shimmer per close
    ! orbit preserves life without masquerading as object rotation.
    mov     r10, r0
    add     r13, r0
    mov     r9, r1
    shlr8   r1
    add     r1, r0
    mov     #15, r1
    and     r1, r0
    shll2   r0
    mov.l   @(r0, r14), r7

    mov     r11, r4
    mov     r12, r5
    mov.l   .Lvertex_normal, r6
    bsr     torus_vertex
    nop

    mov     r11, r4
    mov.l   .Lu_step, r0
    add     r0, r4
    mov     r12, r5
    mov.l   .Lvertex_normal, r6
    mov     #1, r0
    cmp/eq  r0, r13
    bf      .Ltorus_not_eol
    mov.l   .Lvertex_eol, r6
.Ltorus_not_eol:
    bsr     torus_vertex
    nop

    mov.l   .Lv_step, r0
    add     r0, r12
    dt      r13
    bf      .Ltorus_side_loop
    nop

    mov.l   .Lu_step, r0
    add     r0, r11
    dt      r10
    bf      .Ltorus_strip_loop
    nop

    lds.l   @r15+, pr
    rts
    nop

! Inputs: r4=u angle, r5=v angle, r6=TA vertex command, r7=ARGB.
! Tail-branches to emit_vertex, preserving the caller's PR.
torus_vertex:
    lds     r4, fpul
    fsca    fpul, dr0              ! fr0=sin U, fr1=cos U
    lds     r5, fpul
    fsca    fpul, dr2              ! fr2=sin V, fr3=cos V

    mov.l   .Lmajor_addr, r0
    fmov.s  @r0, fr4
    mov.l   .Lminor_addr, r0
    fmov.s  @r0, fr5
    fmov    fr5, fr6
    fmul    fr3, fr6
    fadd    fr4, fr6               ! radial = R + r*cos(V)
    fmov    fr6, fr7
    fmul    fr1, fr7               ! object X
    fmov    fr5, fr8
    fmul    fr2, fr8               ! object Y
    fmov    fr6, fr9
    fmul    fr0, fr9               ! object Z
    bra     project_world_vertex
    nop

    .align 2
.Ltorus_palette: .long torus_palette
.Lvertex_normal: .long CMD_VERTEX
.Lvertex_eol:    .long CMD_VERTEX_EOL
.Lu_step:        .long 2731
.Lv_step:        .long 5461
.Lmajor_addr:    .long torus_major
.Lminor_addr:    .long torus_minor

! Inputs: fr7/fr8/fr9 = world XYZ, r6 = TA command, r7 = ARGB.
! The analytic view transform subtracts camera position, rotates into a
! yaw/pitch/roll basis, perspective-divides, and tail-calls emit_vertex.
project_world_vertex:
    mov.l   .Lcamera_position, r0
    fmov.s  @r0, fr6
    fsub    fr6, fr7
    add     #4, r0
    fmov.s  @r0, fr6
    fsub    fr6, fr8
    add     #4, r0
    fmov.s  @r0, fr6
    fsub    fr6, fr9

    mov.l   .Lcamera_yaw, r0
    fmov.s  @r0, fr0               ! sin yaw
    add     #4, r0
    fmov.s  @r0, fr1               ! cos yaw
    mov.l   .Lcamera_pitch, r0
    fmov.s  @r0, fr2               ! sin pitch
    add     #4, r0
    fmov.s  @r0, fr3               ! cos pitch
    mov.l   .Lcamera_roll, r0
    fmov.s  @r0, fr4               ! sin roll
    add     #4, r0
    fmov.s  @r0, fr5               ! cos roll

    ! Yaw into tangent/right and inward/forward axes.
    fmov    fr7, fr10
    fmul    fr1, fr10
    fmov    fr9, fr11
    fmul    fr0, fr11
    fsub    fr11, fr10             ! view X
    fmov    fr7, fr11
    fmul    fr0, fr11
    fneg    fr11
    fmov    fr9, fr12
    fmul    fr1, fr12
    fsub    fr12, fr11             ! yawed forward Z

    ! Pitch around view X.
    fmov    fr8, fr12
    fmul    fr3, fr12
    fmov    fr11, fr13
    fmul    fr2, fr13
    fadd    fr13, fr12             ! pitched Y
    fmov    fr8, fr13
    fmul    fr2, fr13
    fneg    fr13
    fmov    fr11, fr14
    fmul    fr3, fr14
    fadd    fr14, fr13             ! positive camera Z

    ! Camera bank/roll in the image plane.
    fmov    fr10, fr14
    fmul    fr5, fr14
    fmov    fr12, fr15
    fmul    fr4, fr15
    fadd    fr15, fr14             ! rolled X
    fmov    fr10, fr15
    fmul    fr4, fr15
    fneg    fr15
    fmov    fr12, fr6
    fmul    fr5, fr6
    fadd    fr6, fr15              ! rolled Y

    ! No near-plane branch is needed for this closed scene: the staged path's
    ! 3.2 minimum camera radius exceeds the 2.95 maximum geometry radius, so
    ! camera-space Z remains positive. Preserve that invariant when retuning.
    mov.l   .Lone_addr, r0
    fmov.s  @r0, fr9
    fdiv    fr13, fr9              ! reciprocal W / PVR depth
    mov.l   .Lscale_addr, r0
    fmov.s  @r0, fr6
    fmul    fr6, fr14
    fmul    fr9, fr14
    mov.l   .Lcenter_x_addr, r0
    fmov.s  @r0, fr6
    fadd    fr6, fr14
    fmov    fr14, fr12
    mov.l   .Lscale_addr, r0
    fmov.s  @r0, fr6
    fmul    fr6, fr15
    fmul    fr9, fr15
    fneg    fr15
    mov.l   .Lcenter_y_addr, r0
    fmov.s  @r0, fr6
    fadd    fr6, fr15
    fmov    fr15, fr13
    fmov    fr9, fr14

    mov     r6, r4
    mov     r7, r5
    bra     emit_vertex
    nop

    .align 2
.Lcamera_position: .long camera_position
.Lcamera_yaw:      .long camera_yaw
.Lcamera_pitch:    .long camera_pitch
.Lcamera_roll:     .long camera_roll
.Lone_addr:        .long float_one
.Lscale_addr:      .long projection_scale
.Lcenter_x_addr:   .long center_x
.Lcenter_y_addr:   .long center_y

! Inputs: fr7/fr8/fr9 = vault camera-space XYZ, r6/r7 = TA command/color.
! The cached tunnel is already expressed in view space, so it needs only the
! reciprocal projection. Its minimum Z is 0.88125 by construction.
project_vault_vertex:
    mov.l   .Lvault_project_one, r0
    fmov.s  @r0, fr14
    fdiv    fr9, fr14
    mov.l   .Lvault_project_scale, r0
    fmov.s  @r0, fr10
    fmov    fr7, fr12
    fmul    fr10, fr12
    fmul    fr14, fr12
    mov.l   .Lvault_project_cx, r0
    fmov.s  @r0, fr11
    fadd    fr11, fr12
    fmov    fr8, fr13
    fmul    fr10, fr13
    fmul    fr14, fr13
    fneg    fr13
    mov.l   .Lvault_project_cy, r0
    fmov.s  @r0, fr11
    fadd    fr11, fr13
    mov     r6, r4
    mov     r7, r5
    bra     emit_vertex
    nop

    .align 2
.Lvault_project_one:  .long float_one
.Lvault_project_scale:.long projection_scale
.Lvault_project_cx:   .long center_x
.Lvault_project_cy:   .long vault_center_y

! ---------------------------------------------------------------------------
! Counter-rotating energy core
!
! Two additive octahedra inhabit the torus hole. Their 16 faces are small in
! vertex cost but read as a completely separate animated object, with proper
! world-space camera motion and opaque-torus occlusion.
! ---------------------------------------------------------------------------

draw_energy_core:
    sts.l   pr, @-r15

    ! Outer crystal: rapid clockwise yaw with a tilted axis.
    mov     r9, r4
    shll8   r4
    mov     r4, r0
    shlr    r0
    add     r0, r4                 ! frame * 384 angle units
    mov.l   .Lcore_outer_scale, r5
    mov.l   .Lcore_outer_palette, r6
    bsr     draw_core_pass
    nop

    ! Inner crystal: smaller, brighter, and counter-rotating.
    mov     r9, r4
    shll8   r4
    shll    r4                     ! frame * 512
    neg     r4, r4
    mov.l   .Lcore_phase_bias, r0
    add     r0, r4
    mov.l   .Lcore_inner_scale, r5
    mov.l   .Lcore_inner_palette, r6
    bsr     draw_core_pass
    nop

    lds.l   @r15+, pr
    rts
    nop

! r4=binary rotation angle, r5=float scale pointer, r6=8-color palette.
draw_core_pass:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov     r4, r8
    mov     r5, r14
    mov     r6, r13

    lds     r8, fpul
    fsca    fpul, dr0              ! Y rotation
    mov.l   .Lcore_y_store, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0
    mov     r8, r0
    shlr    r0
    mov.l   .Lcore_tilt_bias, r1
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr2              ! independent X rotation
    mov.l   .Lcore_x_store, r0
    fmov.s  fr2, @r0
    add     #4, r0
    fmov.s  fr3, @r0

    mov.l   .Lcore_vertices, r12
    mov     #24, r10               ! eight independent triangle strips
    mov     #3, r11

.Lcore_vertex_loop:
    fmov.s  @r12+, fr7
    fmov.s  @r12+, fr8
    fmov.s  @r12+, fr9
    fmov.s  @r14, fr6
    fmul    fr6, fr7
    fmul    fr6, fr8
    fmul    fr6, fr9

    mov.l   .Lcore_y_load, r0
    fmov.s  @r0, fr0               ! sin Y
    add     #4, r0
    fmov.s  @r0, fr1               ! cos Y
    fmov    fr7, fr4
    fmul    fr1, fr4
    fmov    fr9, fr5
    fmul    fr0, fr5
    fadd    fr5, fr4               ! rotated X
    fmov    fr9, fr5
    fmul    fr1, fr5
    fmov    fr7, fr6
    fmul    fr0, fr6
    fsub    fr6, fr5               ! rotated Z

    mov.l   .Lcore_x_load, r0
    fmov.s  @r0, fr0               ! sin X
    add     #4, r0
    fmov.s  @r0, fr1               ! cos X
    fmov    fr8, fr7
    fmul    fr1, fr7
    fmov    fr5, fr6
    fmul    fr0, fr6
    fsub    fr6, fr7               ! final Y
    fmov    fr8, fr9
    fmul    fr0, fr9
    fmov    fr5, fr6
    fmul    fr1, fr6
    fadd    fr6, fr9               ! final Z
    fmov    fr7, fr8
    fmov    fr4, fr7               ! final X

    mov.l   @r13, r7
    mov.l   .Lcore_vertex_normal, r6
    mov     #1, r0
    cmp/eq  r0, r11
    bf      .Lcore_not_eol
    mov.l   .Lcore_vertex_eol, r6
.Lcore_not_eol:
    bsr     project_world_vertex
    nop

    add     #-1, r10
    dt      r11
    bf      .Lcore_same_face
    nop
    mov     #3, r11
    add     #4, r13
.Lcore_same_face:
    tst     r10, r10
    bf      .Lcore_vertex_loop
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lcore_outer_scale:   .long core_outer_scale
.Lcore_inner_scale:   .long core_inner_scale
.Lcore_outer_palette: .long core_palette_outer
.Lcore_inner_palette: .long core_palette_inner
.Lcore_phase_bias:    .long 0x00002000
.Lcore_tilt_bias:     .long 0x00001000
.Lcore_y_store:       .long core_rotation_y
.Lcore_x_store:       .long core_rotation_x
.Lcore_y_load:        .long core_rotation_y
.Lcore_x_load:        .long core_rotation_x
.Lcore_vertices:      .long core_vertices
.Lcore_vertex_normal: .long CMD_VERTEX
.Lcore_vertex_eol:    .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Energy helicoid
!
! A 64-rung, 126-triangle additive sheet twists through the ring's axis. The
! opaque torus depth buffer slices it into alternating foreground/background
! arcs, making camera parallax and scene depth obvious from every angle.
! ---------------------------------------------------------------------------

draw_helicoid:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov     #64, r10
    mov     r9, r11
    shll8   r11
    shlr2   r11                    ! slow independent rotation, 1024 frames/lap
    mov.l   .Lhelix_y_start, r0
    mov.l   @r0, r1
    mov.l   .Lhelix_y_current, r0
    mov.l   r1, @r0
    mov.l   .Lhelix_palette, r14

.Lhelix_loop:
    lds     r11, fpul
    fsca    fpul, dr0              ! fr0=sin theta, fr1=cos theta
    mov.l   .Lhelix_radius, r0
    fmov.s  @r0, fr2
    fmov    fr1, fr7
    fmul    fr2, fr7               ! +X edge
    fmov    fr0, fr9
    fmul    fr2, fr9               ! +Z edge
    mov.l   .Lhelix_pair, r0
    fmov.s  fr7, @r0
    add     #4, r0
    fmov.s  fr9, @r0
    mov.l   .Lhelix_y_current, r0
    fmov.s  @r0, fr8

    mov     #64, r0
    sub     r10, r0
    mov     #7, r1
    and     r1, r0
    shll2   r0
    mov.l   @(r0, r14), r7
    mov.l   .Lhelix_vertex_normal, r6
    bsr     project_world_vertex
    nop

    ! Opposite edge of this rung. Alternating endpoints form one long strip.
    mov.l   .Lhelix_pair, r0
    fmov.s  @r0, fr7
    fneg    fr7
    add     #4, r0
    fmov.s  @r0, fr9
    fneg    fr9
    mov.l   .Lhelix_y_current, r0
    fmov.s  @r0, fr8
    mov     #64, r0
    sub     r10, r0
    add     #4, r0
    mov     #7, r1
    and     r1, r0
    shll2   r0
    mov.l   @(r0, r14), r7
    mov.l   .Lhelix_vertex_normal, r6
    mov     #1, r0
    cmp/eq  r0, r10
    bf      .Lhelix_not_eol
    mov.l   .Lhelix_vertex_eol, r6
.Lhelix_not_eol:
    bsr     project_world_vertex
    nop

    mov.l   .Lhelix_y_current, r0
    fmov.s  @r0, fr0
    mov.l   .Lhelix_y_step, r1
    fmov.s  @r1, fr1
    fadd    fr1, fr0
    fmov.s  fr0, @r0
    mov.l   .Lhelix_angle_step, r0
    add     r0, r11
    dt      r10
    bf      .Lhelix_loop
    nop

    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lhelix_radius:        .long helicoid_radius
.Lhelix_y_start:       .long helicoid_y_start
.Lhelix_y_step:        .long helicoid_y_step
.Lhelix_y_current:     .long helicoid_current_y
.Lhelix_pair:          .long helicoid_pair
.Lhelix_palette:       .long helicoid_palette
.Lhelix_angle_step:    .long 1024
.Lhelix_vertex_normal: .long CMD_VERTEX
.Lhelix_vertex_eol:    .long CMD_VERTEX_EOL

! Inputs: fr12=x, fr13=y, fr14=reciprocal-Z, r4=flags, r5=ARGB.
! One native parameter block is fired directly through store queue zero.
emit_vertex:
    mov.l   .Lemit_sq, r0
    mov.l   r4, @r0
    mov     r0, r1
    add     #4, r1
    fmov.s  fr12, @r1
    add     #4, r1
    fmov.s  fr13, @r1
    add     #4, r1
    fmov.s  fr14, @r1
    add     #4, r1
    mov     #0, r2
    mov.l   r2, @r1
    add     #4, r1
    mov.l   r2, @r1
    add     #4, r1
    mov.l   r5, @r1
    add     #4, r1
    mov.l   r2, @r1
    pref    @r0
    rts
    nop

    .align 2
.Lemit_sq: .long SQ_BASE

! ---------------------------------------------------------------------------
! Screen-space primitive helpers for particles, grid lines, and vector UI.
! The depth/color live in two globals so tight callers retain all four integer
! argument registers for coordinates.
! ---------------------------------------------------------------------------

! r0=x integer, r1=y integer, r4=vertex command. Tail call to emit_vertex.
screen_point:
    lds     r0, fpul
    float   fpul, fr12
    lds     r1, fpul
    float   fpul, fr13
    mov.l   .Lquad_depth_ptr, r2
    fmov.s  @r2, fr14
    mov.l   .Lquad_color_ptr, r2
    mov.l   @r2, r5
    bra     emit_vertex
    nop

! r4=x0, r5=y0, r6=x1, r7=y1. Emits one axis-aligned quad.
emit_rect:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11

    mov     r8, r0
    mov     r11, r1
    mov.l   .Lscreen_normal, r4
    bsr     screen_point
    nop
    mov     r8, r0
    mov     r9, r1
    mov.l   .Lscreen_normal, r4
    bsr     screen_point
    nop
    mov     r10, r0
    mov     r11, r1
    mov.l   .Lscreen_normal, r4
    bsr     screen_point
    nop
    mov     r10, r0
    mov     r9, r1
    mov.l   .Lscreen_eol, r4
    bsr     screen_point
    nop

    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

! r4=center X, r5=center Y, r6=half-size, r7=color.
emit_box:
    mov.l   .Lquad_color_ptr, r0
    mov.l   r7, @r0
    mov     r4, r0
    sub     r6, r0
    mov     r4, r2
    add     r6, r2
    mov     r5, r1
    sub     r6, r1
    mov     r5, r3
    add     r6, r3
    mov     r0, r4
    mov     r1, r5
    mov     r2, r6
    mov     r3, r7
    bra     emit_rect
    nop

! r4=top X, r5=top Y, r6=bottom X, r7=bottom Y.
emit_grid_ray:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11

    mov     r8, r0
    add     #-1, r0
    mov     r9, r1
    mov.l   .Lscreen_normal, r4
    bsr     screen_point
    nop
    mov     r8, r0
    add     #1, r0
    mov     r9, r1
    mov.l   .Lscreen_normal, r4
    bsr     screen_point
    nop
    mov     r10, r0
    add     #-2, r0
    mov     r11, r1
    mov.l   .Lscreen_normal, r4
    bsr     screen_point
    nop
    mov     r10, r0
    add     #2, r0
    mov     r11, r1
    mov.l   .Lscreen_eol, r4
    bsr     screen_point
    nop

    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lquad_depth_ptr: .long quad_depth
.Lquad_color_ptr: .long quad_color
.Lscreen_normal:  .long CMD_VERTEX
.Lscreen_eol:     .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Expanding polar star tunnel
! ---------------------------------------------------------------------------

draw_star_tunnel:
    sts.l   pr, @-r15
    mov.l   .Lstar_depth_ptr, r0
    mov.l   .Lstar_depth, r1
    mov.l   r1, @r0
    mov     #80, r10
    mov     r9, r11
    shll8   r11

.Lstar_loop:
    mov     #80, r0
    sub     r10, r0                ! stable star index
    mov     #37, r2
    mulu.w  r2, r0
    sts     macl, r0
    mov     r9, r1
    shll2   r1
    add     r1, r0
    and     #0xff, r0
    mov     r0, r6                 ! radius 0..255

    lds     r11, fpul
    fsca    fpul, dr0
    lds     r6, fpul
    float   fpul, fr2
    fmov    fr1, fr3
    fmul    fr2, fr3
    mov.l   .Lstar_cx, r0
    fmov.s  @r0, fr4
    fadd    fr4, fr3
    fmov    fr0, fr4
    fmul    fr2, fr4
    mov.l   .Lstar_yscale, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr4
    mov.l   .Lstar_cy, r0
    fmov.s  @r0, fr5
    fadd    fr5, fr4
    ftrc    fr3, fpul
    sts     fpul, r4
    ftrc    fr4, fpul
    sts     fpul, r5

    mov     r6, r0
    shlr2   r0
    shlr2   r0
    shlr2   r0
    shlr2   r0
    shlr2   r0
    shlr    r0
    add     #1, r0
    mov     r0, r6                 ! 1..4 pixel half-size
    mov     r10, r0
    and     #3, r0
    shll2   r0
    mov.l   .Lstar_palette, r1
    mov.l   @(r0, r1), r7
    bsr     emit_box
    nop

    mov.l   .Lstar_angle_step, r0
    add     r0, r11
    dt      r10
    bf      .Lstar_loop
    nop

    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lstar_depth:      .long 0x3ce56042
.Lstar_depth_ptr:  .long quad_depth
.Lstar_cx:         .long center_x
.Lstar_cy:         .long center_y
.Lstar_yscale:     .long star_y_scale
.Lstar_palette:    .long star_palette
.Lstar_angle_step: .long 811

! ---------------------------------------------------------------------------
! Perspective synth-grid
! ---------------------------------------------------------------------------

draw_grid:
    sts.l   pr, @-r15
    mov.l   .Lgrid_depth_ptr, r0
    mov.l   .Lgrid_depth, r1
    mov.l   r1, @r0
    mov.l   .Lgrid_color_ptr, r0
    mov.l   .Lgrid_h_color, r1
    mov.l   r1, @r0

    mov     #11, r10
    mov     #0, r11
.Lgrid_horizontal_loop:
    mov     r11, r0
    mulu.w  r0, r0
    sts     macl, r5
    shll    r5
    mov.l   .Lgrid_horizon, r0
    add     r0, r5
    mov     #0, r4
    mov.l   .Lscreen_width, r6
    mov     r5, r7
    add     #1, r7
    bsr     emit_rect
    nop
    add     #1, r11
    dt      r10
    bf      .Lgrid_horizontal_loop
    nop

    mov.l   .Lgrid_color_ptr, r0
    mov.l   .Lgrid_v_color, r1
    mov.l   r1, @r0
    mov     #11, r10
    mov     #-80, r11
.Lgrid_ray_loop:
    mov     r11, r0
    mov.l   .Lscreen_center_i, r1
    sub     r1, r0
    shar    r0
    shar    r0
    shar    r0
    add     r1, r0
    mov     r0, r4
    mov.l   .Lgrid_horizon, r5
    mov     r11, r6
    mov.l   .Lscreen_bottom, r7
    bsr     emit_grid_ray
    nop
    add     #80, r11
    dt      r10
    bf      .Lgrid_ray_loop
    nop

    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lgrid_depth:     .long 0x3c449ba6
.Lgrid_depth_ptr: .long quad_depth
.Lgrid_color_ptr: .long quad_color
.Lgrid_h_color:   .long 0xff001522
.Lgrid_v_color:   .long 0xff00101c
.Lgrid_horizon:   .long 286
.Lscreen_width:   .long 640
.Lscreen_center_i:.long 320
.Lscreen_bottom:  .long 480

! ---------------------------------------------------------------------------
! Dotted additive orbital instruments
! ---------------------------------------------------------------------------

draw_orbits:
    sts.l   pr, @-r15
    mov.l   .Lorbit_depth_ptr, r0
    mov.l   .Lorbit_depth, r1
    mov.l   r1, @r0

    mov     #64, r10
    mov     r9, r11
    shll8   r11
.Lorbit_loop:
    lds     r11, fpul
    fsca    fpul, dr0
    mov.l   .Lorbit_rx, r0
    fmov.s  @r0, fr2
    fmul    fr1, fr2
    mov.l   .Lorbit_cx, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr2
    mov.l   .Lorbit_ry, r0
    fmov.s  @r0, fr3
    fmul    fr0, fr3
    mov.l   .Lorbit_cy, r0
    fmov.s  @r0, fr4
    fadd    fr4, fr3
    ftrc    fr2, fpul
    sts     fpul, r4
    ftrc    fr3, fpul
    sts     fpul, r5
    mov     #1, r6
    mov     r10, r0
    mov     #7, r1
    and     r1, r0
    tst     r0, r0
    bf      .Lorbit_small
    mov     #2, r6
.Lorbit_small:
    mov     r10, r0
    and     #3, r0
    shll2   r0
    mov.l   .Lorbit_palette, r1
    mov.l   @(r0, r1), r7
    bsr     emit_box
    nop
    mov.l   .Lorbit_step, r0
    add     r0, r11
    dt      r10
    bf      .Lorbit_loop
    nop

    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lorbit_depth:  .long 0x3d75c28f
.Lorbit_depth_ptr:.long quad_depth
.Lorbit_rx:     .long orbit_radius_x
.Lorbit_ry:     .long orbit_radius_y
.Lorbit_cx:     .long center_x
.Lorbit_cy:     .long orbit_center_y
.Lorbit_palette:.long orbit_palette
.Lorbit_step:   .long 1024

! ---------------------------------------------------------------------------
! Tiny software vector console: a hand-packed 5x7 font becomes PVR quads.
! Keeping it as geometry lets the title remain completely independent of any
! BIOS font service or texture library.
! ---------------------------------------------------------------------------

draw_title:
    sts.l   pr, @-r15

    ! Small opaque instrument panels preserve contrast during the near-camera
    ! portal beat without hiding the rest of the scene.
    mov.l   .Ltitle_depth_ptr, r0
    mov.l   .Ltitle_panel_depth, r1
    mov.l   r1, @r0
    mov.l   .Ltitle_color_ptr, r0
    mov.l   .Ltitle_panel_color, r1
    mov.l   r1, @r0
    mov     #16, r4
    mov     #14, r5
    mov.l   .Ltitle_panel_x1, r6
    mov     #52, r7
    bsr     emit_rect
    nop
    mov.l   .Ltitle_panel2_x0, r4
    mov.l   .Ltitle_panel2_y0, r5
    mov.l   .Ltitle_panel2_x1, r6
    mov.l   .Ltitle_panel2_y1, r7
    bsr     emit_rect
    nop

    mov.l   .Ltitle_depth_ptr, r0
    mov.l   .Ltitle_depth, r1
    mov.l   r1, @r0
    mov.l   .Ltitle_color_ptr, r0
    mov.l   .Ltitle_cyan, r1
    mov.l   r1, @r0
    mov.l   .Ltitle_main, r4
    mov     #24, r5
    mov     #20, r6
    mov     #3, r7
    bsr     draw_text
    nop

    mov.l   .Ltitle_color_ptr, r0
    mov.l   .Ltitle_magenta, r1
    mov.l   r1, @r0
    mov.l   .Ltitle_scene_index, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Ltitle_orbit_label
    mov     #1, r0
    cmp/eq  r0, r1
    bt      .Ltitle_vault_label
    mov     #2, r0
    cmp/eq  r0, r1
    bt      .Ltitle_wave_label
    mov.l   .Ltitle_hyper, r4
    bra     .Ltitle_label_ready
    nop
.Ltitle_wave_label:
    mov.l   .Ltitle_wave, r4
    bra     .Ltitle_label_ready
    nop
.Ltitle_vault_label:
    mov.l   .Ltitle_vault, r4
    bra     .Ltitle_label_ready
    nop
.Ltitle_orbit_label:
    mov.l   .Ltitle_orbit, r4
.Ltitle_label_ready:
    mov.l   .Ltitle_tech_x, r5
    mov.l   .Ltitle_tech_y, r6
    mov     #2, r7
    bsr     draw_text
    nop

    ! Two asymmetric rules complete the classic tracker/demo HUD silhouette.
    mov.l   .Ltitle_color_ptr, r0
    mov.l   .Ltitle_rule_color, r1
    mov.l   r1, @r0
    mov     #24, r4
    mov     #45, r5
    mov.l   .Ltitle_rule_x, r6
    mov     #47, r7
    bsr     emit_rect
    nop
    mov.l   .Ltitle_rule2_x0, r4
    mov.l   .Ltitle_rule2_y0, r5
    mov.l   .Ltitle_rule2_x1, r6
    mov.l   .Ltitle_rule2_y1, r7
    bsr     emit_rect
    nop

    lds.l   @r15+, pr
    rts
    nop

! r4=zero-terminated text, r5=x, r6=y, r7=pixel scale.
draw_text:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11

.Ltext_char_loop:
    mov.b   @r8+, r0
    extu.b  r0, r0
    tst     r0, r0
    bt      .Ltext_done
    mov     #32, r1
    cmp/eq  r1, r0
    bt      .Ltext_advance

    mov     #65, r1
    cmp/hs  r1, r0
    bf      .Ltext_try_digit
    mov     #91, r1
    cmp/hs  r1, r0
    bt      .Ltext_advance
    add     #-65, r0
    mov     r0, r1
    shll2   r0
    shll    r0
    sub     r1, r0                  ! glyph index * 7
    mov.l   .Lfont_letters, r12
    add     r0, r12
    bra     .Ltext_have_glyph
    nop

.Ltext_try_digit:
    mov     #48, r1
    cmp/hs  r1, r0
    bf      .Ltext_advance
    mov     #58, r1
    cmp/hs  r1, r0
    bt      .Ltext_advance
    add     #-48, r0
    mov     r0, r1
    shll2   r0
    shll    r0
    sub     r1, r0
    mov.l   .Lfont_digits, r12
    add     r0, r12

.Ltext_have_glyph:
    mov     #0, r13
.Ltext_row_loop:
    mov.b   @r12+, r7
    extu.b  r7, r7
    mov     #0, r14
.Ltext_col_loop:
    mov     r7, r0
    and     #0x10, r0
    tst     r0, r0
    bt      .Ltext_skip_pixel

    mov.l   r7, @-r15
    mov.l   r14, @-r15
    mov     r14, r0
    mulu.w  r11, r0
    sts     macl, r4
    add     r9, r4
    mov     r13, r0
    mulu.w  r11, r0
    sts     macl, r5
    add     r10, r5
    mov     r4, r6
    add     r11, r6
    mov     r5, r7
    add     r11, r7
    bsr     emit_rect
    nop
    mov.l   @r15+, r14
    mov.l   @r15+, r7

.Ltext_skip_pixel:
    shll    r7
    add     #1, r14
    mov     #5, r0
    cmp/hs  r0, r14
    bf      .Ltext_col_loop
    nop
    add     #1, r13
    mov     #7, r0
    cmp/hs  r0, r13
    bf      .Ltext_row_loop
    nop

.Ltext_advance:
    mov     r11, r0
    shll    r0
    mov     r11, r1
    shll2   r1
    add     r1, r0                  ! six pixels per character
    add     r0, r9
    bra     .Ltext_char_loop
    nop

.Ltext_done:
    mov.l   @r15+, r14
    mov.l   @r15+, r13
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Ltitle_depth_ptr:.long quad_depth
.Ltitle_color_ptr:.long quad_color
.Ltitle_panel_depth:.long 0x3fe66666
.Ltitle_panel_color:.long 0xff020718
.Ltitle_panel_x1: .long 288
.Ltitle_panel2_x0:.long 424
.Ltitle_panel2_y0:.long 438
.Ltitle_panel2_x1:.long 624
.Ltitle_panel2_y1:.long 476
.Ltitle_depth:    .long 0x40000000
.Ltitle_cyan:     .long 0xff0080a8
.Ltitle_magenta:  .long 0xffa02070
.Ltitle_rule_color:.long 0xff004860
.Ltitle_main:     .long title_main
.Ltitle_orbit:    .long title_orbit
.Ltitle_vault:    .long title_vault
.Ltitle_wave:     .long title_wave
.Ltitle_hyper:    .long title_hyper
.Ltitle_scene_index:.long scene_index
.Ltitle_tech_x:   .long 436
.Ltitle_tech_y:   .long 446
.Ltitle_rule_x:   .long 276
.Ltitle_rule2_x0: .long 430
.Ltitle_rule2_y0: .long 470
.Ltitle_rule2_x1: .long 616
.Ltitle_rule2_y1: .long 472
.Lfont_letters:   .long font_letters
.Lfont_digits:    .long font_digits

! ---------------------------------------------------------------------------
! Read-only native PVR parameter blocks and register tables
! ---------------------------------------------------------------------------

    .section .rodata,"a"
    .align 5

video_vga_table:
    .long 0x0044, 0x00800004       ! RGB565, VGA, display disabled
    .long 0x0048, 0x00000009       ! RGB565 render target + dithering
    .long 0x004c, 0x000000a0       ! 1280-byte row / eight
    .long 0x0050, FB0
    .long 0x0054, FB0
    .long 0x005c, 0x00177d3f
    .long 0x00cc, 0x00150208
    .long 0x00d0, 0x00000100
    .long 0x00d4, 0x007e0345
    .long 0x00dc, 0x00240204
    .long 0x00d8, 0x020c0359
    .long 0x00ec, 0x000000ac
    .long 0x00f0, 0x00280028
    .long -1, 0

video_ntsc_table:
    .long 0x0044, 0x00000004       ! RGB565, interlaced, disabled
    .long 0x0048, 0x00000009
    .long 0x004c, 0x000000a0
    .long 0x0050, FB0
    .long 0x0054, FB0 + 1280
    .long 0x005c, 0x1413bd3f
    .long 0x00cc, 0x00150104
    .long 0x00d0, 0x00000150
    .long 0x00d4, 0x007e0345
    .long 0x00dc, 0x00240204
    .long 0x00d8, 0x020c0359
    .long 0x00ec, 0x000000a4
    .long 0x00f0, 0x00120012
    .long -1, 0

pvr_init_table:
    .long 0x00f4, 0x00000400
    .long 0x00a8, 0x15d1c951
    .long 0x00a0, 0x00000020
    .long 0x0110, 0x00093f39
    .long 0x0098, 0x00800408
    .long 0x0084, 0x00000000
    .long 0x0030, 0x00000101
    .long 0x00b0, 0x00101830
    .long 0x00b4, 0x00203050
    .long 0x00c0, 0x00000000
    .long 0x00bc, 0xffffffff
    .long 0x0080, 0x00000007
    .long 0x0074, 0x00000001
    .long 0x007c, 0x0027df77
    .long 0x00e4, 0x00000000
    .long 0x00b8, 0x0000ff07
    .long 0x0118, 0x00008040
    .long -1, 0

! Native colored, gouraud-shaded, untextured opaque polygon header.
opaque_header:
    .long 0x80840002, 0x80000000, 0x20800000, 0x00000000
    .long 0, 0, 0, 0

additive_header:
    .long 0x82840002, 0x84000000, 0x24900000, 0x00000000
    .long 0, 0, 0, 0

trans_empty_header:
    .long 0x82840012, 0x84000000, 0x94900000, 0x00000000
    .long 0, 0, 0, 0

zero_block:
    .long 0, 0, 0, 0, 0, 0, 0, 0

! Full-screen distant strip. Its restrained corners leave headroom for the
! additive scene while still producing a subtle four-way color field.
backdrop_vertices:
    .long CMD_VERTEX,     0x00000000, 0x43f00000, 0x3a83126f
    .long 0, 0, 0xff001420, 0
    .long CMD_VERTEX,     0x00000000, 0x00000000, 0x3a83126f
    .long 0, 0, 0xff090012, 0
    .long CMD_VERTEX,     0x44200000, 0x43f00000, 0x3a83126f
    .long 0, 0, 0xff002a36, 0
    .long CMD_VERTEX_EOL, 0x44200000, 0x00000000, 0x3a83126f
    .long 0, 0, 0xff20002e, 0

! ISP background plane: near-black blue so uncovered tiles are intentional.
background_plane:
    .long 0x90800000, 0x20800440, 0
    .long 0x00000000, 0x43f00000, 0x34000000, 0xff02020c
    .long 0x00000000, 0x00000000, 0x34000000, 0xff02020c
    .long 0x44200000, 0x43f00000, 0x34000000, 0xff02020c

    .align 2
torus_major:       .long 0x3fe00000    ! 1.75
torus_minor:       .long 0x3f147ae1    ! 0.58
float_one:         .long 0x3f800000
projection_scale:  .long 0x43c80000    ! 400.0
center_x:          .long 0x43a00000    ! 320.0
center_y:          .long 0x435e0000    ! 222.0
camera_radius_far:       .long 0x4109999a ! 8.6 establishing radius
camera_radius_dive_mid:  .long 0x40bccccd ! 5.9 = midpoint of 8.6 and 3.2
camera_radius_dive_half: .long 0x402ccccd ! 2.7; eased endpoint approaches 3.2
camera_radius_close_base:.long 0x406ccccd ! 3.70
camera_radius_close_amplitude:.long 0x3f000000 ! 0.50 -> 3.20 .. 4.20
camera_dive_roll_units:  .long 0x45000000 ! +/-2048 = 11.25 degrees
camera_close_pitch_units:.long 0x45800000 ! +/-4096 = 22.5 degrees
camera_close_roll_units: .long 0x45800000 ! +/-4096 = 22.5 degrees
core_outer_scale:        .long 0x3f6b851f ! 0.92
core_inner_scale:        .long 0x3f147ae1 ! 0.58
helicoid_radius:          .long 0x40100000 ! 2.25
helicoid_y_start:         .long 0xbff33333 ! -1.9
helicoid_y_step:          .long 0x3d770f71 ! 3.8 / 63
vault_fraction_scale:     .long 0x3d000000 ! 1/32
vault_z_base:             .long 0x3f59999a ! 0.85
vault_curve_x:            .long 0x3f000000 ! 0.50
vault_curve_y:            .long 0x3ea3d70a ! 0.32
vault_radius_base:        .long 0x402ccccd ! 2.70
vault_radius_amplitude:   .long 0x3eb33333 ! 0.35
vault_arch_inner:         .long 0x3f5c28f6 ! 0.86
vault_rail_inner:         .long 0x3f70a3d7 ! 0.94
vault_gate_outer:         .long 0x3f1eb852 ! 0.62
vault_gate_inner:         .long 0x3eeb851f ! 0.46
vault_depth_bias:         .long 0x3c800000 ! 1/64 toward the camera
vault_center_y:           .long 0x43700000 ! 240.0
lorenz_sigma_dt:          .long 0x3d75c28f ! 0.06
lorenz_dt:                .long 0x3bc49ba6 ! 0.006
lorenz_beta:              .long 0x402aaaab ! 8/3
lorenz_rho_base:          .long 0x41e00000 ! 28.0
lorenz_rho_amplitude:     .long 0x40400000 ! 3.0
lorenz_world_center_z:    .long 0x41c80000 ! 25.0
lorenz_world_scale_x:     .long 0x3dc28f5c ! 0.095
lorenz_world_scale_y:     .long 0x3d958106 ! 0.073
lorenz_world_scale_z:     .long 0x3d872b02 ! 0.066
lorenz_env_scale:         .long 0x3b808081 ! 1/255
attractor_broad_width:    .long 0x3d03126f ! 0.032
attractor_hot_width:      .long 0x3c9374bc ! 0.018
attractor_spark_base:     .long 0x3d8f5c29 ! 0.070
attractor_spark_amplitude:.long 0x3ca3d70a ! 0.020
chaos_camera_radius:      .long 0x408b3333 ! 4.35
chaos_camera_pitch_units: .long 0x45800000 ! +/-22.5 degrees
chaos_camera_roll_units:  .long 0x45000000 ! +/-11.25 degrees
orbit_center_y:    .long 0x435c0000    ! 220.0
star_y_scale:      .long 0x3f3851ec    ! 0.72
orbit_radius_x:    .long 0x435c0000    ! 220.0
orbit_radius_y:    .long 0x42980000    ! 76.0

    .align 2
torus_palette:
    .long 0xff00d8ff, 0xff00f0e0, 0xff20ff90, 0xffa0ff30
    .long 0xffffd020, 0xffff8030, 0xffff4060, 0xffff30b0
    .long 0xffd030ff, 0xff8040ff, 0xff4060ff, 0xff2088ff
    .long 0xff00b8ff, 0xff00f0ff, 0xff20ffd0, 0xff60ff80

vault_wall_palette:
    .long 0xff020612, 0xff05041a, 0xff02101a, 0xff080515
    .long 0xff03131c, 0xff0b0614, 0xff04101a, 0xff06091a

vault_arch_palette:
    .long 0xff004060, 0xff102050, 0xff283000, 0xff401018
    .long 0xff300040, 0xff102860, 0xff004838, 0xff403000

vault_rail_palette:
    .long 0xff001828, 0xff080c20, 0xff101800, 0xff20080c
    .long 0xff180020, 0xff081428, 0xff002018, 0xff201800

vault_gate_palette:
    .long 0xff0080a0, 0xff403080, 0xff708000, 0xffa02840
    .long 0xff800090, 0xff2860a0, 0xff00a070, 0xffa06000

chaos_broad_palette:
    .long 0xff001020, 0xff001830, 0xff002440, 0xff003050
    .long 0xff004060, 0xff005878, 0xff087898, 0xff20a0b0
    .long 0xff100818, 0xff180c24, 0xff281038, 0xff381448
    .long 0xff501860, 0xff702078, 0xff983090, 0xffc048a0

chaos_hot_palette:
    .long 0xff002038, 0xff003050, 0xff00486c, 0xff006080
    .long 0xff087c98, 0xff1898ac, 0xff38b8c0, 0xff70d8d0
    .long 0xff281028, 0xff401438, 0xff581c50, 0xff782460
    .long 0xff983070, 0xffb84080, 0xffd85890, 0xffff78a0

! Two independent 3-vertex strips: an XY shard and an XZ shard.
attractor_spark_offsets:
    .long 0xbf800000,0xbf800000,0x00000000
    .long 0x3f800000,0xbf800000,0x00000000
    .long 0x00000000,0x3f800000,0x00000000
    .long 0xbf800000,0x00000000,0xbf800000
    .long 0x3f800000,0x00000000,0xbf800000
    .long 0x00000000,0x00000000,0x3f800000

star_palette:
    .long 0xff081830, 0xff100a28, 0xff061c22, 0xff181018

orbit_palette:
    .long 0xff006080, 0xff580060, 0xff006860, 0xff601830

core_palette_outer:
    .long 0xff183880, 0xff006898, 0xff006050, 0xff305800
    .long 0xff703000, 0xff681038, 0xff480070, 0xff183878

core_palette_inner:
    .long 0xff2860b0, 0xff00a0c0, 0xff00a070, 0xff70a000
    .long 0xffb06000, 0xffa02058, 0xff8030b0, 0xff2860b0

helicoid_palette:
    .long 0xff001828, 0xff002838, 0xff002820, 0xff182800
    .long 0xff281400, 0xff280818, 0xff200028, 0xff0c1428

! Eight triangular faces of a unit octahedron. The camera and both independent
! object rotations are applied at runtime; this is topology, not a baked mesh.
core_vertices:
    ! top, +X, +Z
    .long 0x00000000,0x3f800000,0x00000000
    .long 0x3f800000,0x00000000,0x00000000
    .long 0x00000000,0x00000000,0x3f800000
    ! top, +Z, -X
    .long 0x00000000,0x3f800000,0x00000000
    .long 0x00000000,0x00000000,0x3f800000
    .long 0xbf800000,0x00000000,0x00000000
    ! top, -X, -Z
    .long 0x00000000,0x3f800000,0x00000000
    .long 0xbf800000,0x00000000,0x00000000
    .long 0x00000000,0x00000000,0xbf800000
    ! top, -Z, +X
    .long 0x00000000,0x3f800000,0x00000000
    .long 0x00000000,0x00000000,0xbf800000
    .long 0x3f800000,0x00000000,0x00000000
    ! bottom, +Z, +X
    .long 0x00000000,0xbf800000,0x00000000
    .long 0x00000000,0x00000000,0x3f800000
    .long 0x3f800000,0x00000000,0x00000000
    ! bottom, -X, +Z
    .long 0x00000000,0xbf800000,0x00000000
    .long 0xbf800000,0x00000000,0x00000000
    .long 0x00000000,0x00000000,0x3f800000
    ! bottom, -Z, -X
    .long 0x00000000,0xbf800000,0x00000000
    .long 0x00000000,0x00000000,0xbf800000
    .long 0xbf800000,0x00000000,0x00000000
    ! bottom, +X, -Z
    .long 0x00000000,0xbf800000,0x00000000
    .long 0x3f800000,0x00000000,0x00000000
    .long 0x00000000,0x00000000,0xbf800000

title_main: .asciz "CHROMA CIRCUIT"
title_orbit:.asciz "01 ORBIT CORE"
title_vault:.asciz "02 NEON VAULT"
title_wave: .asciz "03 CHAOS BLOOM"
title_hyper:.asciz "04 HYPERFOLD"

! Five-bit rows, seven rows per glyph, A-Z then 0-9.
font_letters:
    .byte 0x0e,0x11,0x11,0x1f,0x11,0x11,0x11  ! A
    .byte 0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e  ! B
    .byte 0x0e,0x11,0x10,0x10,0x10,0x11,0x0e  ! C
    .byte 0x1e,0x11,0x11,0x11,0x11,0x11,0x1e  ! D
    .byte 0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f  ! E
    .byte 0x1f,0x10,0x10,0x1e,0x10,0x10,0x10  ! F
    .byte 0x0e,0x11,0x10,0x17,0x11,0x11,0x0e  ! G
    .byte 0x11,0x11,0x11,0x1f,0x11,0x11,0x11  ! H
    .byte 0x1f,0x04,0x04,0x04,0x04,0x04,0x1f  ! I
    .byte 0x07,0x02,0x02,0x02,0x12,0x12,0x0c  ! J
    .byte 0x11,0x12,0x14,0x18,0x14,0x12,0x11  ! K
    .byte 0x10,0x10,0x10,0x10,0x10,0x10,0x1f  ! L
    .byte 0x11,0x1b,0x15,0x15,0x11,0x11,0x11  ! M
    .byte 0x11,0x19,0x19,0x15,0x13,0x13,0x11  ! N
    .byte 0x0e,0x11,0x11,0x11,0x11,0x11,0x0e  ! O
    .byte 0x1e,0x11,0x11,0x1e,0x10,0x10,0x10  ! P
    .byte 0x0e,0x11,0x11,0x11,0x15,0x12,0x0d  ! Q
    .byte 0x1e,0x11,0x11,0x1e,0x14,0x12,0x11  ! R
    .byte 0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e  ! S
    .byte 0x1f,0x04,0x04,0x04,0x04,0x04,0x04  ! T
    .byte 0x11,0x11,0x11,0x11,0x11,0x11,0x0e  ! U
    .byte 0x11,0x11,0x11,0x11,0x11,0x0a,0x04  ! V
    .byte 0x11,0x11,0x11,0x15,0x15,0x1b,0x11  ! W
    .byte 0x11,0x11,0x0a,0x04,0x0a,0x11,0x11  ! X
    .byte 0x11,0x11,0x0a,0x04,0x04,0x04,0x04  ! Y
    .byte 0x1f,0x01,0x02,0x04,0x08,0x10,0x1f  ! Z

font_digits:
    .byte 0x0e,0x11,0x13,0x15,0x19,0x11,0x0e  ! 0
    .byte 0x04,0x0c,0x04,0x04,0x04,0x04,0x0e  ! 1
    .byte 0x0e,0x11,0x01,0x02,0x04,0x08,0x1f  ! 2
    .byte 0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e  ! 3
    .byte 0x02,0x06,0x0a,0x12,0x1f,0x02,0x02  ! 4
    .byte 0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e  ! 5
    .byte 0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e  ! 6
    .byte 0x1f,0x01,0x02,0x04,0x08,0x08,0x08  ! 7
    .byte 0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e  ! 8
    .byte 0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e  ! 9
    .align 2

msg_boot:   .asciz "\r\nCHROMA CIRCUIT // bare SH-4 entry\r\n"
msg_maple:  .asciz "MAPLE: direct A0 DMA, LEFT/RIGHT scene select\r\n"
msg_pvr:    .asciz "PVR2: direct registers, tile matrix, no SDK runtime\r\n"
msg_render: .asciz "TA: four-act orbit + vault + chaos + hyperfold online\r\n"
msg_ta_timeout: .asciz "PVR FATAL: TA completion timeout\r\n"
msg_ta_fault: .asciz "PVR FATAL: TA error event\r\n"
msg_render_timeout: .asciz "PVR FATAL: render completion timeout\r\n"
msg_render_fault: .asciz "PVR FATAL: render error event\r\n"
msg_vblank_timeout: .asciz "PVR FATAL: scanline counter timeout\r\n"
msg_perf: .asciz "PERF missed-vblank frames / 600 = 0x"
msg_perf_ticks: .asciz "PERF TMU1 ticks / 600 frames = 0x"
msg_crlf: .asciz "\r\n"

! ---------------------------------------------------------------------------
! Writable state and aligned scratch blocks
! ---------------------------------------------------------------------------

    .section .bss,"aw",@nobits
    .align 5
cable_type:  .space 4
current_vert:.space 4
current_tile:.space 4
current_fb:  .space 4
scene_index: .space 4
camera_yaw:      .space 8
camera_pitch:    .space 8
camera_roll:     .space 8
camera_position: .space 12
core_rotation_y: .space 8
core_rotation_x: .space 8
helicoid_current_y:.space 4
helicoid_pair:     .space 8
perf_previous:     .space 4
perf_countdown:    .space 4
perf_slow_count:   .space 4
perf_total_ticks:  .space 4
quad_depth:  .space 4
quad_color:  .space 4
vault_reference_x:.space 4
vault_reference_y:.space 4
vault_fraction:   .space 4
    .align 5
vault_cache:      .space 18 * 84
    .align 5
lorenz_points:    .space 512 * 12
lorenz_state:     .space 12
lorenz_head:      .space 4
lorenz_rho:       .space 4
attractor_envelope:.space 4
attractor_spark_size:.space 4
attractor_current_spark_size:.space 4
attractor_xyz:     .space 12

! Cached controller bookkeeping is deliberately separated from the raw DMA
! area. maple_init purges the following 33 cache lines once; every later CPU
! access to the table and reply uses P2, so hardware never races dirty P1 data.
maple_pending:          .space 4
maple_busy_frames:      .space 4
maple_sample_valid:     .space 4
maple_previous_buttons: .space 4
    .align 5
maple_dma_table:        .space 32
maple_response:         .space 1024
    .align 5

! The two large subsystems live in separate, heavily commented assembly
! includes so the already substantial core renderer remains navigable.
    .include "hyperfold.inc"
    .include "aica-music.inc"
