! ---------------------------------------------------------------------------
! CHROMA CIRCUIT -- direct Yamaha AICA soundtrack
! ---------------------------------------------------------------------------
!
! The Dreamcast does not contain a General MIDI instrument ROM. Its Yamaha
! AICA is instead a 64-slot PCM/ADPCM wavetable synthesizer, very much like the
! hardware that tracker musicians drove on late-1990s PCs. This score uses that
! silicon directly: SH-4 code uploads five 32-sample PCM8 oscillator waves,
! keys thirteen AICA slots once, and subsequently changes only pitch, pan, and
! total-level words. Two slots use AICA's own noise generator for percussion.
!
! There is no streamed PCM, software mixer, SDK sound driver, or ARM program.
! The ARM7 remains safely in reset while the autonomous AICA voices run. One
! display frame is one tracker tick and eight ticks make a sixteenth-note row,
! yielding roughly 112.40 BPM at the Dreamcast's 59.9453 Hz refresh. Exactly
! 96 rows (six four-beat bars) fit each 768-frame visual act. Seven acts make
! one 42-bar / 672-row score loop.

    .equ AICA_REG_BASE,          0xa0700000
    .equ AICA_RAM_BASE,          0xa0800000
    .equ AICA_ARM_CONTROL,       0xa0702c00
    .equ AICA_MASTER,            0xa0702800
    .equ AICA_MONITOR_SELECT,    0xa070280c
    .equ AICA_MONITOR_POSITION,  0xa0702814
    .equ AICA_FIFO_STATUS,       0xa05f688c

    .equ MUSIC_SAMPLE_BASE,      0x00030000
    .equ MUSIC_SAMPLE_SQUARE,    0x00030000
    .equ MUSIC_SAMPLE_PULSE25,   0x00030020
    .equ MUSIC_SAMPLE_PULSE12,   0x00030040
    .equ MUSIC_SAMPLE_TRIANGLE,  0x00030060
    .equ MUSIC_SAMPLE_SAW,       0x00030080

    .equ MUSIC_PCM8_LOOP_CTL,    0x00000283 ! PCM8, loop, SA high = 3
    .equ MUSIC_NOISE_LOOP_CTL,   0x00000603 ! hardware noise, loop, SA high = 3
    .equ MUSIC_VOICE_STRIDE,     0x00000080
    .equ MUSIC_MUTED_TL,         0x000000ff

! Pattern note values are MIDI note numbers minus 36. Keeping MIDI-style note
! values makes the tracker data readable without pretending AICA has a GM ROM.
    .equ MN_C2,   0
    .equ MN_CS2,  1
    .equ MN_D2,   2
    .equ MN_DS2,  3
    .equ MN_E2,   4
    .equ MN_F2,   5
    .equ MN_FS2,  6
    .equ MN_G2,   7
    .equ MN_GS2,  8
    .equ MN_A2,   9
    .equ MN_AS2, 10
    .equ MN_B2,  11
    .equ MN_C3,  12
    .equ MN_CS3, 13
    .equ MN_D3,  14
    .equ MN_DS3, 15
    .equ MN_E3,  16
    .equ MN_F3,  17
    .equ MN_FS3, 18
    .equ MN_G3,  19
    .equ MN_GS3, 20
    .equ MN_A3,  21
    .equ MN_AS3, 22
    .equ MN_B3,  23
    .equ MN_C4,  24
    .equ MN_CS4, 25
    .equ MN_D4,  26
    .equ MN_DS4, 27
    .equ MN_E4,  28
    .equ MN_F4,  29
    .equ MN_FS4, 30
    .equ MN_G4,  31
    .equ MN_GS4, 32
    .equ MN_A4,  33
    .equ MN_AS4, 34
    .equ MN_B4,  35
    .equ MN_C5,  36
    .equ MN_CS5, 37
    .equ MN_D5,  38
    .equ MN_DS5, 39
    .equ MN_E5,  40
    .equ MN_F5,  41
    .equ MN_FS5, 42
    .equ MN_G5,  43
    .equ MN_GS5, 44
    .equ MN_A5,  45
    .equ MN_AS5, 46
    .equ MN_B5,  47
    .equ MN_HOLD, 0xfe
    .equ MN_REST, 0xff

    .section .text.music,"ax"
    .align 2

! ---------------------------------------------------------------------------
! Bounded G2 FIFO synchronization
! ---------------------------------------------------------------------------

! All AICA registers and sound RAM sit behind the G2 bus. Its write FIFO holds
! eight dwords, so every batch below drains it before issuing at most eight.
! Returning an error instead of spinning forever preserves the visual demo if
! audio hardware is absent or a loader left the bus in a broken state.
aica_fifo_wait:
    mov.l   .Lmus_fifo_status, r1
    mov.l   .Lmus_fifo_timeout, r2
    mov     #0x11, r3
.Lmus_fifo_wait_loop:
    mov.l   @r1, r0
    tst     r3, r0
    bt      .Lmus_fifo_ready
    dt      r2
    bf      .Lmus_fifo_wait_loop
    nop
    mov     #-1, r0
    rts
    nop
.Lmus_fifo_ready:
    mov     #0, r0
    rts
    nop

    .align 2
.Lmus_fifo_status:  .long AICA_FIFO_STATUS
.Lmus_fifo_timeout: .long 1000000

! ---------------------------------------------------------------------------
! AICA startup
! ---------------------------------------------------------------------------

music_init:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lmi_enabled, r0
    mov     #0, r1
    mov.l   r1, @r0

    ! Hold the ARM7 in reset. Read-modify-write preserves the video cable and
    ! system-control bits that video_init configured immediately before us.
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bt      .Lmi_arm_fifo_ready
    bra     .Lmi_fail
    nop
.Lmi_arm_fifo_ready:
    mov.l   .Lmi_arm_control, r8
    mov.l   @r8, r1
    mov     #1, r2
    or      r2, r1
    mov.l   r1, @r8

    ! Preserve the common mixer mode but clear its low four master-volume bits
    ! while channels and RAM are being changed.
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bt      .Lmi_master_fifo_ready
    bra     .Lmi_fail
    nop
.Lmi_master_fifo_ready:
    mov.l   .Lmi_master, r8
    mov.l   @r8, r13
    mov.l   .Lmi_common_mask, r1       ! clear MVOL, MONO, and DAC18B
    and     r1, r13
    mov.l   r13, @r8

    ! Reset all 64 slots, not merely the thirteen used by this score. This prevents
    ! a warm loader or previous program from leaking a stale voice into the mix.
    mov.l   .Lmi_reg_base, r8
    mov     #64, r10
    mov     #64, r11
    shll    r11                       ! 0x80-byte slot stride
.Lmi_reset_slot:
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bt      .Lmi_reset_fifo_ready
    bra     .Lmi_fail
    nop
.Lmi_reset_fifo_ready:
    mov     r8, r1
    mov.l   .Lmi_keyoff, r0
    mov.l   r0, @r1                   ! +00 key off
    add     #0x10, r1
    mov     #0x1f, r0
    mov.l   r0, @r1                   ! +10 no software envelope
    add     #4, r1
    mov.l   r0, @r1                   ! +14 release defaults
    mov     r8, r1
    add     #0x2c, r1
    mov.l   .Lmi_filter_reset, r0
    mov.l   r0, @r1                   ! +2c filter envelope levels
    add     #4, r1
    mov.l   r0, @r1
    add     #4, r1
    mov.l   r0, @r1
    add     #4, r1
    mov.l   r0, @r1
    add     #4, r1
    mov.l   r0, @r1                   ! exactly eight writes in this batch
    add     r11, r8
    dt      r10
    bf      .Lmi_reset_slot
    nop

    ! Upload five signed PCM8 single-cycle waves. Five groups of eight dwords
    ! exactly match the FIFO depth and occupy only 160 bytes of the 2 MiB RAM.
    mov.l   .Lmi_waveforms, r8
    mov.l   .Lmi_wave_destination, r9
    mov     #5, r10
.Lmi_wave_group:
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bt      .Lmi_wave_fifo_ready
    bra     .Lmi_fail
    nop
.Lmi_wave_fifo_ready:
    mov     #8, r11
.Lmi_wave_word:
    mov.l   @r8+, r0
    mov.l   r0, @r9
    add     #4, r9
    dt      r11
    bf      .Lmi_wave_word
    nop
    dt      r10
    bf      .Lmi_wave_group
    nop

    ! Describe all thirteen hardware oscillators while muted. KYONB is armed on
    ! every slot, then a single global KYONEX write starts the bank in phase.
    mov.l   .Lmi_voice_descriptors, r9
    mov.l   .Lmi_reg_base, r8
    mov     #13, r10
    mov     #64, r11
    shll    r11                       ! 0x80
.Lmi_config_slot:
    mov.l   @r9+, r4                  ! sample-address low word
    mov.l   @r9+, r5                  ! loop end in samples
    mov.l   @r9+, r6                  ! initial pitch word
    mov.l   @r9+, r7                  ! direct-send/pan word
    mov.l   @r9+, r12                 ! format/loop/address control

    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmi_fail
    mov     r8, r1
    mov.l   .Lmi_keyoff, r0
    mov.l   r0, @r1                   ! +00
    add     #4, r1
    mov.l   r4, @r1                   ! +04 SA low
    add     #4, r1
    mov     #0, r0
    mov.l   r0, @r1                   ! +08 loop start
    add     #4, r1
    mov.l   r5, @r1                   ! +0c loop end
    add     #4, r1
    mov     #0x1f, r0
    mov.l   r0, @r1                   ! +10 no envelope
    add     #4, r1
    mov.l   r0, @r1                   ! +14 release defaults
    add     #4, r1
    mov.l   r6, @r1                   ! +18 pitch
    add     #4, r1
    mov     #0, r0
    mov.l   r0, @r1                   ! +1c LFO disabled

    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmi_fail
    mov     r8, r1
    add     #0x20, r1
    mov     #0, r0
    mov.l   r0, @r1                   ! +20 DSP send disabled
    add     #4, r1
    mov.l   r7, @r1                   ! +24 direct send + pan
    add     #4, r1
    mov.l   .Lmi_muted_word, r0
    mov.l   r0, @r1                   ! +28 LPF bypass + TL=255
    mov     #64, r0
    shll8   r0                         ! KYONB, but no KYONEX yet
    or      r0, r12
    mov.l   r12, @r8

    add     r11, r8
    dt      r10
    bf      .Lmi_config_slot
    nop

    ! Let AICA itself provide slow analog drift. LFORE remains clear so the
    ! oscillator's 32-sample loop does not continually reset the LFO phase.
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmi_fail
    mov.l   .Lmi_pad_lfo_slow, r0
    mov.l   .Lmi_pad_lfo_3, r1
    mov.l   r0, @r1
    mov.l   .Lmi_pad_lfo_9, r1
    mov.l   r0, @r1
    mov.l   .Lmi_pad_lfo_fast, r0
    mov.l   .Lmi_pad_lfo_8, r1
    mov.l   r0, @r1
    mov.l   .Lmi_pad_lfo_10, r1
    mov.l   r0, @r1
    mov.l   .Lmi_lead_lfo_word, r0
    mov.l   .Lmi_lead_lfo_11, r1
    mov.l   r0, @r1

    ! Execute all thirteen pending key-ons once through slot zero.
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmi_fail
    mov.l   .Lmi_reg_base, r8
    mov.l   .Lmi_execute_pcm, r0
    mov.l   r0, @r8

    ! Select slot zero for the common current-address monitor. A row-cadence
    ! readback later proves that the hardware oscillator is advancing.
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmi_fail
    mov.l   .Lmi_monitor_select, r8
    mov     #0, r0
    mov.l   r0, @r8

    ! Read back the first waveform dword before unmuting. This catches a broken
    ! G2 sound-RAM path without jeopardizing the visual part of the demo.
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmi_fail
    mov.l   .Lmi_wave_destination, r8
    mov.l   @r8, r0
    mov.l   .Lmi_waveforms, r1
    mov.l   @r1, r1
    cmp/eq  r1, r0
    bf      .Lmi_fail
    nop

    ! Restore stereo mode at full master level. Conservative per-slot TL values
    ! still leave over 12 dB of measured peak headroom in Flycast's AICA mix.
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmi_fail
    mov     #0x0f, r0
    or      r0, r13
    mov.l   .Lmi_master, r8
    mov.l   r13, @r8

    mov     #-1, r1
    mov.l   .Lmi_last_row, r0
    mov.l   r1, @r0
    mov.l   .Lmi_lead_age, r0
    mov.l   r1, @r0
    mov.l   .Lmi_echo_age, r0
    mov.l   r1, @r0
    mov.l   .Lmi_echo_delay, r0
    mov.l   r1, @r0
    mov.l   .Lmi_bass_age, r0
    mov.l   r1, @r0
    mov.l   .Lmi_kick_age, r0
    mov.l   r1, @r0
    mov.l   .Lmi_snare_age, r0
    mov.l   r1, @r0
    mov.l   .Lmi_hat_age, r0
    mov.l   r1, @r0
    mov     #1, r1
    mov.l   .Lmi_enabled, r0
    mov.l   r1, @r0

    mov.l   .Lmi_msg_ok, r4
    mov.l   .Lmi_serial_puts, r0
    jsr     @r0
    nop
    bra     .Lmi_done
    nop

.Lmi_fail:
    mov.l   .Lmi_enabled, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov.l   .Lmi_msg_fail, r4
    mov.l   .Lmi_serial_puts, r0
    jsr     @r0
    nop

.Lmi_done:
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
.Lmi_enabled:          .long music_enabled
.Lmi_arm_control:      .long AICA_ARM_CONTROL
.Lmi_master:           .long AICA_MASTER
.Lmi_reg_base:         .long AICA_REG_BASE
.Lmi_monitor_select:   .long AICA_MONITOR_SELECT
.Lmi_wave_destination:.long AICA_RAM_BASE + MUSIC_SAMPLE_BASE
.Lmi_waveforms:        .long music_waveforms
.Lmi_voice_descriptors:.long music_voice_descriptors
.Lmi_keyoff:           .long 0x00008000
.Lmi_filter_reset:     .long 0x00001ff8
.Lmi_muted_word:       .long 0x0000ff24
.Lmi_execute_pcm:      .long 0x0000c283
.Lmi_common_mask:      .long 0xffff7ef0
.Lmi_pad_lfo_slow:     .long 0x00003220
.Lmi_pad_lfo_fast:     .long 0x00003a20
.Lmi_lead_lfo_word:    .long 0x00004e20
.Lmi_pad_lfo_3:        .long AICA_REG_BASE + 3 * MUSIC_VOICE_STRIDE + 0x1c
.Lmi_pad_lfo_8:        .long AICA_REG_BASE + 8 * MUSIC_VOICE_STRIDE + 0x1c
.Lmi_pad_lfo_9:        .long AICA_REG_BASE + 9 * MUSIC_VOICE_STRIDE + 0x1c
.Lmi_pad_lfo_10:       .long AICA_REG_BASE + 10 * MUSIC_VOICE_STRIDE + 0x1c
.Lmi_lead_lfo_11:      .long AICA_REG_BASE + 11 * MUSIC_VOICE_STRIDE + 0x1c
.Lmi_last_row:         .long music_last_row
.Lmi_lead_age:         .long music_lead_age
.Lmi_echo_age:         .long music_echo_age
.Lmi_echo_delay:       .long music_echo_delay
.Lmi_bass_age:         .long music_bass_age
.Lmi_kick_age:         .long music_kick_age
.Lmi_snare_age:        .long music_snare_age
.Lmi_hat_age:          .long music_hat_age
.Lmi_msg_ok:           .long msg_music_ok
.Lmi_msg_fail:         .long msg_music_fail
.Lmi_serial_puts:      .long serial_puts

! ---------------------------------------------------------------------------
! Frame-synchronous tracker
! ---------------------------------------------------------------------------

music_update:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lmu_enabled, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Lmu_done
    nop

    ! Eight frames per row gives a relaxed 112.40 BPM synthwave grid and turns
    ! the division into three shifts. Four rows are one beat, sixteen one bar.
    mov     r9, r10
    shlr2   r10
    shlr    r10                        ! quotient / tracker row
    mov     r9, r11
    mov     #7, r0
    and     r0, r11                    ! remainder / tracker tick
.Lmu_divide_done:
    mov.l   .Lmu_current_row, r0
    mov.l   r10, @r0
    mov.l   .Lmu_current_tick, r0
    mov.l   r11, @r0

    mov.l   .Lmu_last_row, r0
    mov.l   @r0, r12
    cmp/eq  r12, r10
    bt      .Lmu_animate
    nop

    ! Mark a discontinuity so Left/Right jumps and timeline wrap revoice the
    ! new act rather than leaving a bass note or drum tail from the old one.
    mov     #0, r13
    cmp/pz  r12
    bf      .Lmu_row_is_jump
    mov     r12, r1
    add     #1, r1
    cmp/eq  r1, r10
    bt      .Lmu_store_jump
    nop
.Lmu_row_is_jump:
    mov     #1, r13
.Lmu_store_jump:
    mov.l   .Lmu_row_jump, r0
    mov.l   r13, @r0
    mov.l   .Lmu_last_row, r0
    mov.l   r10, @r0
    mov.l   .Lmu_rows_seen, r0
    mov.l   @r0, r1
    add     #1, r1
    mov.l   r1, @r0

    mov     r10, r4
    bsr     music_decode_row
    nop
    bsr     music_monitor_position
    nop

.Lmu_animate:
    bsr     music_animate_voices
    nop

.Lmu_done:
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
.Lmu_enabled:      .long music_enabled
.Lmu_current_row:  .long music_current_row
.Lmu_current_tick: .long music_current_tick
.Lmu_last_row:     .long music_last_row
.Lmu_row_jump:     .long music_row_jump
.Lmu_rows_seen:    .long music_rows_seen

! Decode one of 42 twelve-byte bar descriptors. Byte zero is the chord root,
! byte one is 1 for minor / 0 for major, bytes 2..9 are the eight lead events,
! byte 10 is an explicit color interval, and byte 11 is a true low bass root.
music_decode_row:
    sts.l   pr, @-r15
    mov.l   .Lmd_current_row, r0
    mov.l   r4, @r0

    mov.l   .Lmd_row_jump, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Lmd_no_revoice
    nop
    bsr     music_scene_revoice
    nop
.Lmd_no_revoice:
    mov.l   .Lmd_current_row, r0
    mov.l   @r0, r4
    mov     r4, r10
    mov     #0x0f, r0
    and     r0, r10                    ! row within 16-row bar
    mov     r4, r11
    shlr2   r11
    shlr2   r11                        ! bar 0..41
    mov     #0, r12
    mov     #6, r0
    cmp/hs  r0, r11
    bf      .Lmd_scene_ready
    mov     #1, r12
    mov     #12, r0
    cmp/hs  r0, r11
    bf      .Lmd_scene_ready
    mov     #2, r12
    mov     #18, r0
    cmp/hs  r0, r11
    bf      .Lmd_scene_ready
    mov     #3, r12
    mov     #24, r0
    cmp/hs  r0, r11
    bf      .Lmd_scene_ready
    mov     #4, r12
    mov     #30, r0
    cmp/hs  r0, r11
    bf      .Lmd_scene_ready
    mov     #5, r12
    mov     #36, r0
    cmp/hs  r0, r11
    bf      .Lmd_scene_ready
    mov     #6, r12
.Lmd_scene_ready:
    mov.l   .Lmd_current_scene, r0
    mov.l   r12, @r0

    ! descriptor = music_bars + bar*12
    mov     r11, r0
    shll2   r0                         ! bar*4
    mov     r11, r1
    shll2   r1
    shll    r1                         ! bar*8
    add     r1, r0
    mov.l   .Lmd_bars, r8
    add     r0, r8
    mov.b   @r8, r13
    extu.b  r13, r13                   ! chord root note
    mov     #1, r0
    mov.b   @(r0, r8), r14
    extu.b  r14, r14                   ! minor flag
    mov.l   .Lmd_root, r0
    mov.l   r13, @r0
    mov.l   .Lmd_quality, r0
    mov.l   r14, @r0
    mov     #10, r0
    mov.b   @(r0, r8), r1
    extu.b  r1, r1
    mov.l   .Lmd_color, r0
    mov.l   r1, @r0
    mov     #11, r0
    mov.b   @(r0, r8), r1
    extu.b  r1, r1
    mov.l   .Lmd_true_bass, r0
    mov.l   r1, @r0

    ! Four softly mixed oscillator slots form an actual stereo chord pad:
    ! root, third, fifth, and high root. Separate voices are still cheaper than
    ! mixing one sample on SH-4, and make far better use of AICA's 64 slots.
    mov     #94, r7                    ! intro pad headroom
    tst     r12, r12
    bf      .Lmd_pad_not_intro
    mov     #16, r0
    cmp/hs  r0, r4
    bf      .Lmd_pad_level_chosen
    mov     #88, r7
    bra     .Lmd_pad_level_chosen
    nop
.Lmd_pad_not_intro:
    mov     #1, r0
    cmp/eq  r0, r12
    bf      .Lmd_pad_chaos_level
    mov     #82, r7                    ! chorus opens up
    bra     .Lmd_pad_level_chosen
    nop
.Lmd_pad_chaos_level:
    mov     #86, r7
    mov     #2, r0
    cmp/eq  r0, r12
    bt      .Lmd_pad_level_chosen
    mov     #76, r7                    ! HYPERFOLD is the wide fourth-act drop
    mov     #3, r0
    cmp/eq  r0, r12
    bt      .Lmd_pad_level_chosen
    mov     #90, r7                    ! hard act-five drums keep their headroom
    mov     #4, r0
    cmp/eq  r0, r12
    bt      .Lmd_pad_level_chosen
    mov     #80, r7                    ! act six reopens into a broad Y2K pad
    mov     #5, r0
    cmp/eq  r0, r12
    bt      .Lmd_pad_level_chosen
    mov     #78, r7                    ! HIGH COUNTRY carries an epic air pad
.Lmd_pad_level_chosen:

    ! Root on slot 3.
    mov     r13, r4
    bsr     music_note_pitch
    nop
    mov     r0, r5
    mov     #3, r4
    bsr     music_set_pitch
    nop
    mov     #3, r4
    mov     r7, r5
    bsr     music_set_tl
    nop

    ! Third on slot 8, detuned two FNS steps for width.
    mov     r13, r4
    tst     r14, r14
    bt      .Lmd_pad_major
    add     #3, r4
    bra     .Lmd_pad_third_pitch
    nop
.Lmd_pad_major:
    add     #4, r4
.Lmd_pad_third_pitch:
    bsr     music_note_pitch
    nop
    add     #-2, r0
    mov     r0, r5
    mov     #8, r4
    bsr     music_set_pitch
    nop
    mov     #8, r4
    mov     r7, r5
    add     #4, r5
    bsr     music_set_tl
    nop

    ! Fifth on slot 9, detuned two FNS steps the other way.
    mov     r13, r4
    add     #7, r4
    bsr     music_note_pitch
    nop
    add     #2, r0
    mov     r0, r5
    mov     #9, r4
    bsr     music_set_pitch
    nop
    mov     #9, r4
    mov     r7, r5
    add     #6, r5
    bsr     music_set_tl
    nop

    ! The fourth pad slot holds an explicit add9, major-7, minor-7, or sixth
    ! color from the score instead of merely duplicating the root.
    mov     r13, r4
    mov.l   .Lmd_color, r0
    mov.l   @r0, r1
    add     r1, r4
    bsr     music_note_pitch
    nop
    add     #1, r0
    mov     r0, r5
    mov     #10, r4
    bsr     music_set_pitch
    nop
    mov     #10, r4
    mov     r7, r5
    add     #10, r5
    bsr     music_set_tl
    nop

    ! The bass pulses on eighth notes rather than every tracker row. Roots are
    ! interrupted by two fifth pickups and one octave lift per bar.
    mov     r10, r0
    mov     #1, r1
    and     r1, r0
    tst     r0, r0
    bt      .Lmd_bass_event
    mov.l   .Lmd_row_jump, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Lmd_after_bass
    nop
.Lmd_bass_event:
    mov.l   .Lmd_true_bass, r0
    mov.l   @r0, r4
    mov     r10, r0
    cmp/eq  #6, r0
    bt      .Lmd_bass_fifth
    mov     r10, r0
    cmp/eq  #14, r0
    bt      .Lmd_bass_fifth
    mov     r10, r0
    cmp/eq  #12, r0
    bf      .Lmd_bass_note_ready
    add     #12, r4                    ! octave lift
    bra     .Lmd_bass_note_ready
    nop
.Lmd_bass_fifth:
    add     #7, r4
.Lmd_bass_note_ready:
    bsr     music_note_pitch
    nop
    ! Act five drops the bass an octave without a software mixer or another
    ! AICA slot, leaving its unstable upper harmony cold and metallic.
    mov.l   .Lmd_current_scene, r1
    mov.l   @r1, r2
    mov     #4, r3
    cmp/eq  r3, r2
    bf      .Lmd_bass_octave_ready
    mov.l   .Lmd_octave_down, r1
    add     r1, r0
.Lmd_bass_octave_ready:
    mov.l   .Lmd_bass_pitch, r1
    mov.l   r0, @r1
    mov.l   .Lmd_bass_age, r1
    mov     #0, r0
    mov.l   r0, @r1
    mov     #2, r4
    mov.l   .Lmd_bass_pitch, r0
    mov.l   @r0, r5
    bsr     music_set_pitch
    nop
.Lmd_after_bass:

    ! Normal playback reads a lead event only on even rows. A manual jump may
    ! land on an odd row; in that case the paired even-row event is restored so
    ! the destination scene sounds complete immediately.
    mov     r10, r0
    mov     #1, r1
    and     r1, r0
    tst     r0, r0
    bt      .Lmd_read_lead
    mov.l   .Lmd_row_jump, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Lmd_after_lead
    nop
.Lmd_read_lead:
    mov     r10, r0
    shlr    r0
    add     #2, r0
    mov.b   @(r0, r8), r0
    extu.b  r0, r0
    mov     #-1, r1
    extu.b  r1, r1                     ! 0xff REST
    cmp/eq  r1, r0
    bt      .Lmd_lead_rest
    mov     #-2, r1
    extu.b  r1, r1                     ! 0xfe HOLD
    cmp/eq  r1, r0
    bt      .Lmd_after_lead
    nop

    mov     r0, r4
    bsr     music_note_pitch
    nop
    mov.l   .Lmd_lead_pitch, r1
    mov.l   r0, @r1
    mov.l   .Lmd_lead_active, r1
    mov     #1, r2
    mov.l   r2, @r1
    mov.l   .Lmd_lead_age, r1
    mov     #0, r2
    mov.l   r2, @r1
    ! Schedule the same note an octave down twelve frames later. This is an
    ! actual audible delay tap, not the old immediate overlap at note changes.
    mov     r0, r2
    mov.l   .Lmd_octave_down, r3
    add     r3, r2
    mov.l   .Lmd_echo_pending_pitch, r1
    mov.l   r2, @r1
    mov.l   .Lmd_echo_delay, r1
    mov     #12, r2
    mov.l   r2, @r1
    bra     .Lmd_after_lead
    nop

.Lmd_lead_rest:
    mov.l   .Lmd_lead_active, r1
    mov     #0, r2
    mov.l   r2, @r1
    mov     #1, r4
    mov     #-1, r5
    extu.b  r5, r5
    bsr     music_set_tl
    nop
    mov     #11, r4
    bsr     music_set_tl
    nop

.Lmd_after_lead:
    mov.l   .Lmd_current_row, r0
    mov.l   @r0, r4
    bsr     music_decode_drums
    nop
    bsr     music_apply_pan
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmd_current_row:   .long music_current_row
.Lmd_current_scene: .long music_current_scene
.Lmd_row_jump:      .long music_row_jump
.Lmd_bars:          .long music_bars
.Lmd_root:          .long music_chord_root
.Lmd_quality:       .long music_chord_quality
.Lmd_color:         .long music_chord_color
.Lmd_true_bass:     .long music_true_bass_root
.Lmd_bass_pitch:    .long music_bass_pitch
.Lmd_bass_age:      .long music_bass_age
.Lmd_lead_active:   .long music_lead_active
.Lmd_lead_pitch:    .long music_lead_pitch
.Lmd_lead_age:      .long music_lead_age
.Lmd_echo_active:   .long music_echo_active
.Lmd_echo_pitch:    .long music_echo_pitch
.Lmd_echo_age:      .long music_echo_age
.Lmd_echo_pending_pitch:.long music_echo_pending_pitch
.Lmd_echo_delay:    .long music_echo_delay
.Lmd_octave_down:   .long 0xfffff800

! Cut tails when the timeline discontinuously changes acts. The fresh row is
! decoded immediately afterward, so this produces a deliberate hard tracker
! cut rather than a frame of silence.
music_scene_revoice:
    sts.l   pr, @-r15
    mov     #0, r1
    mov.l   .Lmsr_lead_active, r0
    mov.l   r1, @r0
    mov.l   .Lmsr_echo_active, r0
    mov.l   r1, @r0
    mov     #-1, r1
    mov.l   .Lmsr_kick_age, r0
    mov.l   r1, @r0
    mov.l   .Lmsr_snare_age, r0
    mov.l   r1, @r0
    mov.l   .Lmsr_hat_age, r0
    mov.l   r1, @r0
    mov.l   .Lmsr_echo_delay, r0
    mov.l   r1, @r0
    mov     #-1, r5
    extu.b  r5, r5
    mov     #1, r4
    bsr     music_set_tl
    nop
    mov     #4, r4
    bsr     music_set_tl
    nop
    mov     #5, r4
    bsr     music_set_tl
    nop
    mov     #6, r4
    bsr     music_set_tl
    nop
    mov     #7, r4
    bsr     music_set_tl
    nop
    mov     #11, r4
    bsr     music_set_tl
    nop
    mov     #12, r4
    bsr     music_set_tl
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmsr_lead_active:.long music_lead_active
.Lmsr_echo_active:.long music_echo_active
.Lmsr_kick_age:   .long music_kick_age
.Lmsr_snare_age:  .long music_snare_age
.Lmsr_hat_age:    .long music_hat_age
.Lmsr_echo_delay: .long music_echo_delay

! ---------------------------------------------------------------------------
! Drum pattern decoder
! ---------------------------------------------------------------------------

! Slot 4 is a pitch-swept triangle kick. Slots 5/6 are independent AICA noise
! voices for snare and hat/crash, so their decays may overlap. Triggering only
! resets a software age; the oscillators themselves remain continuously keyed.
music_decode_drums:
    sts.l   pr, @-r15
    mov     r4, r8
    mov     r4, r0
    mov     #0x0f, r1
    and     r1, r0
    mov     r0, r9                      ! row within bar
    mov.l   .Lmdr_early_96, r1
    cmp/hs  r1, r8
    bt      .Lmdr_not_orbit

    ! Orbit Core stays sparse for the opening bar. A gated fill lands exactly
    ! at row 15 / frame 120, where the camera leaves its two-second hold.
    mov     #16, r1
    cmp/hs  r1, r8
    bt      .Lmdr_orbit_full
    mov     r8, r0
    cmp/eq  #0, r0
    bt      .Lmdr_kick_only
    mov     r8, r0
    cmp/eq  #8, r0
    bt      .Lmdr_kick_only
    mov     r8, r0
    cmp/eq  #15, r0
    bt      .Lmdr_intro_fill
    bra     .Lmdr_done
    nop
.Lmdr_intro_fill:
    bsr     music_trigger_kick
    nop
    bsr     music_trigger_snare
    nop
    bsr     music_trigger_crash
    nop
    bra     .Lmdr_done
    nop

.Lmdr_orbit_full:
.Lmdr_orbit_kicks:
    mov     r9, r0
    cmp/eq  #0, r0
    bt      .Lmdr_orbit_kick
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_orbit_kick
    mov     r9, r0
    cmp/eq  #8, r0
    bt      .Lmdr_orbit_kick
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_orbit_snare
.Lmdr_orbit_kick:
    bsr     music_trigger_kick
    nop
.Lmdr_orbit_snare:
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_orbit_do_snare
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_orbit_hat
.Lmdr_orbit_do_snare:
    bsr     music_trigger_snare
    nop
.Lmdr_orbit_hat:
    mov     r9, r0
    cmp/eq  #2, r0
    bt      .Lmdr_orbit_do_hat
    mov     r9, r0
    cmp/eq  #6, r0
    bt      .Lmdr_orbit_do_hat
    mov     r9, r0
    cmp/eq  #10, r0
    bt      .Lmdr_orbit_do_hat
    mov     r9, r0
    cmp/eq  #14, r0
    bf      .Lmdr_orbit_cues
.Lmdr_orbit_do_hat:
    bsr     music_trigger_hat
    nop
.Lmdr_orbit_cues:
    mov     r8, r0
    cmp/eq  #31, r0                    ! frame 248 surface-skim accent
    bt      .Lmdr_orbit_crash
    mov     r8, r0
    cmp/eq  #95, r0                    ! pre-cut fill
    bt      .Lmdr_orbit_final_fill
    bra     .Lmdr_done
    nop
.Lmdr_orbit_final_fill:
    bsr     music_trigger_snare
    nop
.Lmdr_orbit_crash:
    bsr     music_trigger_crash
    nop
    bra     .Lmdr_done
    nop

.Lmdr_kick_only:
    bsr     music_trigger_kick
    nop
    bra     .Lmdr_done
    nop

.Lmdr_not_orbit:
    mov.l   .Lmdr_early_192, r1
    cmp/hs  r1, r8
    bt      .Lmdr_chaos

    ! Neon Vault opens into the song's chorus: four-on-the-floor kick, gated
    ! backbeat, offbeat hats, and one syncopated pickup per bar.
    mov     r8, r0
    mov.l   .Lmdr_early_96, r1
    cmp/eq  r1, r0
    bf      .Lmdr_vault_kick_test
    bsr     music_trigger_crash
    nop
.Lmdr_vault_kick_test:
    mov     r9, r0
    cmp/eq  #0, r0
    bt      .Lmdr_vault_kick
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_vault_kick
    mov     r9, r0
    cmp/eq  #8, r0
    bt      .Lmdr_vault_kick
    mov     r9, r0
    cmp/eq  #10, r0
    bt      .Lmdr_vault_kick
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_vault_snare
.Lmdr_vault_kick:
    bsr     music_trigger_kick
    nop
.Lmdr_vault_snare:
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_vault_do_snare
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_vault_hat
.Lmdr_vault_do_snare:
    bsr     music_trigger_snare
    nop
.Lmdr_vault_hat:
    mov     r9, r0
    cmp/eq  #2, r0
    bt      .Lmdr_vault_do_hat
    mov     r9, r0
    cmp/eq  #6, r0
    bt      .Lmdr_vault_do_hat
    mov     r9, r0
    cmp/eq  #10, r0
    bt      .Lmdr_vault_do_hat
    mov     r9, r0
    cmp/eq  #14, r0
    bf      .Lmdr_vault_exit
.Lmdr_vault_do_hat:
    bsr     music_trigger_hat
    nop
.Lmdr_vault_exit:
    mov.l   .Lmdr_early_191, r1
    cmp/eq  r1, r8
    bf      .Lmdr_done
    bsr     music_trigger_snare
    nop
    bsr     music_trigger_crash
    nop
    bra     .Lmdr_done
    nop

    .align 2
.Lmdr_early_96:  .long 96
.Lmdr_early_191: .long 191
.Lmdr_early_192: .long 192

.Lmdr_chaos:
    mov.l   .Lmdr_chaos_288, r1
    cmp/hs  r1, r8
    bf      .Lmdr_chaos_body
    bra     .Lmdr_hyperfold
    nop
.Lmdr_chaos_body:
    ! Chaos Bloom keeps the chorus harmony but fractures the kick around the
    ! attractor. Hats stay on offbeats; every bar's final hat opens into noise.
    mov.l   .Lmdr_chaos_192, r1
    cmp/eq  r1, r8
    bf      .Lmdr_chaos_kicks
    bsr     music_trigger_crash
    nop
.Lmdr_chaos_kicks:
    mov     r9, r0
    cmp/eq  #0, r0
    bt      .Lmdr_chaos_kick
    mov     r9, r0
    cmp/eq  #3, r0
    bt      .Lmdr_chaos_kick
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_chaos_kick
    mov     r9, r0
    cmp/eq  #8, r0
    bt      .Lmdr_chaos_kick
    mov     r9, r0
    cmp/eq  #11, r0
    bt      .Lmdr_chaos_kick
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_chaos_snare
.Lmdr_chaos_kick:
    bsr     music_trigger_kick
    nop
.Lmdr_chaos_snare:
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_chaos_do_snare
    mov     r9, r0
    cmp/eq  #12, r0
    bt      .Lmdr_chaos_do_snare
    mov     r9, r0
    cmp/eq  #15, r0
    bf      .Lmdr_chaos_hat
.Lmdr_chaos_do_snare:
    bsr     music_trigger_snare
    nop
.Lmdr_chaos_hat:
    mov     r9, r0
    cmp/eq  #14, r0
    bt      .Lmdr_chaos_open
    mov     r9, r0
    cmp/eq  #2, r0
    bt      .Lmdr_chaos_closed
    mov     r9, r0
    cmp/eq  #6, r0
    bt      .Lmdr_chaos_closed
    mov     r9, r0
    cmp/eq  #10, r0
    bf      .Lmdr_chaos_exit
.Lmdr_chaos_closed:
    bsr     music_trigger_hat
    nop
    bra     .Lmdr_chaos_exit
    nop
.Lmdr_chaos_open:
    bsr     music_trigger_crash
    nop
.Lmdr_chaos_exit:
    mov.l   .Lmdr_chaos_284, r1
    cmp/hs  r1, r8
    bf      .Lmdr_done
    bsr     music_trigger_snare
    nop
    mov     r8, r0
    mov.l   .Lmdr_chaos_287, r1
    cmp/eq  r1, r8
    bf      .Lmdr_done
    bsr     music_trigger_kick
    nop
    bsr     music_trigger_crash
    nop
.Lmdr_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmdr_chaos_192: .long 192
.Lmdr_chaos_284: .long 284
.Lmdr_chaos_287: .long 287
.Lmdr_chaos_288: .long 288

.Lmdr_hyperfold:
    mov.l   .Lmdr_hyper_384, r1
    cmp/hs  r1, r8
    bt      .Lmdr_strange
    ! HYPERFOLD mirrors the kick around each half-bar and doubles the hat rate,
    ! so the fourth act feels like a genuine final drop rather than more Chaos.
    ! Full-spectrum impacts mark the entrance and both two-bar geometry folds.
    mov.l   .Lmdr_hyper_288, r1
    cmp/eq  r1, r8
    bt      .Lmdr_hyperfold_impact
    mov.l   .Lmdr_hyper_320, r1
    cmp/eq  r1, r8
    bt      .Lmdr_hyperfold_fold
    mov.l   .Lmdr_hyper_352, r1
    cmp/eq  r1, r8
    bt      .Lmdr_hyperfold_fold
    bra     .Lmdr_hyperfold_exit_test
    nop
.Lmdr_hyperfold_impact:
    bsr     music_trigger_crash
    nop
    bra     .Lmdr_hyperfold_exit_test
    nop
.Lmdr_hyperfold_fold:
    bsr     music_trigger_snare
    nop
    bsr     music_trigger_crash
    nop

.Lmdr_hyperfold_exit_test:
    ! A four-row snare staircase tears the score open before the loop wraps.
    mov.l   .Lmdr_hyper_380, r1
    cmp/hs  r1, r8
    bf      .Lmdr_hyperfold_kick_test
    bsr     music_trigger_snare
    nop
    mov.l   .Lmdr_hyper_383, r1
    cmp/eq  r1, r8
    bf      .Lmdr_hyperfold_done
    bsr     music_trigger_kick
    nop
    bsr     music_trigger_crash
    nop
    bra     .Lmdr_hyperfold_done
    nop

.Lmdr_hyperfold_kick_test:
    mov     r9, r0
    cmp/eq  #0, r0
    bt      .Lmdr_hyperfold_kick
    mov     r9, r0
    cmp/eq  #3, r0
    bt      .Lmdr_hyperfold_kick
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_hyperfold_kick
    mov     r9, r0
    cmp/eq  #8, r0
    bt      .Lmdr_hyperfold_kick
    mov     r9, r0
    cmp/eq  #11, r0
    bt      .Lmdr_hyperfold_kick
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_hyperfold_snare_test
.Lmdr_hyperfold_kick:
    bsr     music_trigger_kick
    nop

.Lmdr_hyperfold_snare_test:
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_hyperfold_snare
    mov     r9, r0
    cmp/eq  #12, r0
    bt      .Lmdr_hyperfold_snare
    ! Alternating row 7 / row 15 snares make adjacent bars fold into mirrors.
    mov     r8, r0
    shlr2   r0
    shlr2   r0
    mov     #1, r1
    and     r1, r0
    tst     r0, r0
    bf      .Lmdr_hyperfold_odd_bar
    mov     r9, r0
    cmp/eq  #7, r0
    bf      .Lmdr_hyperfold_hat
    bra     .Lmdr_hyperfold_snare
    nop
.Lmdr_hyperfold_odd_bar:
    mov     r9, r0
    cmp/eq  #15, r0
    bf      .Lmdr_hyperfold_hat
.Lmdr_hyperfold_snare:
    bsr     music_trigger_snare
    nop

.Lmdr_hyperfold_hat:
    ! Eighth-note hat drive adds speed without changing the 112.40 BPM clock.
    mov     r9, r0
    mov     #1, r1
    and     r1, r0
    tst     r0, r0
    bt      .Lmdr_hyperfold_done
    bsr     music_trigger_hat
    nop
.Lmdr_hyperfold_done:
    bra     .Lmdr_done
    nop

    .align 2
.Lmdr_hyper_288: .long 288
.Lmdr_hyper_320: .long 320
.Lmdr_hyper_352: .long 352
.Lmdr_hyper_380: .long 380
.Lmdr_hyper_383: .long 383
.Lmdr_hyper_384: .long 384

.Lmdr_strange:
    mov.l   .Lmdr_strange_480, r1
    cmp/hs  r1, r8
    bf      .Lmdr_strange_body
    bra     .Lmdr_product
    nop
.Lmdr_strange_body:
    ! STRANGE FORM changes the perceived meter without changing the tracker:
    ! interlocked 3-3-2 kick/snare gaps create a 168.6 BPM industrial break
    ! over the existing 112.4 BPM clock. Three crashes drive visual ignitions.
    mov.l   .Lmdr_strange_384, r1
    cmp/eq  r1, r8
    bt      .Lmdr_strange_impact
    mov.l   .Lmdr_strange_416, r1
    cmp/eq  r1, r8
    bt      .Lmdr_strange_impact
    mov.l   .Lmdr_strange_448, r1
    cmp/eq  r1, r8
    bt      .Lmdr_strange_impact
    bra     .Lmdr_strange_final_test
    nop
.Lmdr_strange_impact:
    bsr     music_trigger_crash
    nop

.Lmdr_strange_final_test:
    mov.l   .Lmdr_strange_476, r1
    cmp/hs  r1, r8
    bf      .Lmdr_strange_kick_test
    bsr     music_trigger_snare
    nop
    mov.l   .Lmdr_strange_479, r1
    cmp/eq  r1, r8
    bf      .Lmdr_strange_done
    bsr     music_trigger_kick
    nop
    bsr     music_trigger_crash
    nop
    bra     .Lmdr_done
    nop

.Lmdr_strange_kick_test:
    mov     r9, r0
    cmp/eq  #0, r0
    bt      .Lmdr_strange_kick
    mov     r9, r0
    cmp/eq  #6, r0
    bt      .Lmdr_strange_kick
    mov     r9, r0
    cmp/eq  #7, r0
    bt      .Lmdr_strange_kick
    mov     r9, r0
    cmp/eq  #11, r0
    bt      .Lmdr_strange_kick
    mov     r9, r0
    cmp/eq  #13, r0
    bf      .Lmdr_strange_snare_test
.Lmdr_strange_kick:
    bsr     music_trigger_kick
    nop

.Lmdr_strange_snare_test:
    mov     r9, r0
    cmp/eq  #3, r0
    bt      .Lmdr_strange_snare
    mov     r9, r0
    cmp/eq  #8, r0
    bt      .Lmdr_strange_snare
    mov     r9, r0
    cmp/eq  #14, r0
    bf      .Lmdr_strange_hat_test
.Lmdr_strange_snare:
    bsr     music_trigger_snare
    nop

.Lmdr_strange_hat_test:
    ! Silence only local rows 2, 6, 10, and 14; the remaining hats form a
    ! dense broken lattice instead of Hyperfold's even eighth-note motorik.
    mov     r9, r0
    cmp/eq  #2, r0
    bt      .Lmdr_strange_done
    mov     r9, r0
    cmp/eq  #6, r0
    bt      .Lmdr_strange_done
    mov     r9, r0
    cmp/eq  #10, r0
    bt      .Lmdr_strange_done
    mov     r9, r0
    cmp/eq  #14, r0
    bt      .Lmdr_strange_done
    bsr     music_trigger_hat
    nop
.Lmdr_strange_done:
    bra     .Lmdr_done
    nop

    .align 2
.Lmdr_strange_384: .long 384
.Lmdr_strange_416: .long 416
.Lmdr_strange_448: .long 448
.Lmdr_strange_476: .long 476
.Lmdr_strange_479: .long 479
.Lmdr_strange_480: .long 480

.Lmdr_product:
    mov.l   .Lmdr_product_576, r1
    cmp/hs  r1, r8
    bt      .Lmdr_elevated
    ! MACHINE DREAM deliberately snaps back to a turn-of-the-millennium
    ! four-on-the-floor pulse. Offbeat noise hats, broad stereo arpeggios, and
    ! three two-bar impacts evoke a tiny realtime synth without quoting fr-08.
    mov.l   .Lmdr_product_480, r1
    cmp/eq  r1, r8
    bt      .Lmdr_product_impact
    mov.l   .Lmdr_product_512, r1
    cmp/eq  r1, r8
    bt      .Lmdr_product_impact
    mov.l   .Lmdr_product_544, r1
    cmp/eq  r1, r8
    bt      .Lmdr_product_impact
    bra     .Lmdr_product_final_test
    nop
.Lmdr_product_impact:
    bsr     music_trigger_crash
    nop

.Lmdr_product_final_test:
    mov.l   .Lmdr_product_572, r1
    cmp/hs  r1, r8
    bf      .Lmdr_product_kick_test
    bsr     music_trigger_snare
    nop
    mov.l   .Lmdr_product_575, r1
    cmp/eq  r1, r8
    bf      .Lmdr_product_done
    bsr     music_trigger_kick
    nop
    bsr     music_trigger_crash
    nop
    bra     .Lmdr_done
    nop

.Lmdr_product_kick_test:
    mov     r9, r0
    cmp/eq  #0, r0
    bt      .Lmdr_product_kick
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_product_kick
    mov     r9, r0
    cmp/eq  #8, r0
    bt      .Lmdr_product_kick
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_product_snare_test
.Lmdr_product_kick:
    bsr     music_trigger_kick
    nop

.Lmdr_product_snare_test:
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_product_snare
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_product_hat_test
.Lmdr_product_snare:
    bsr     music_trigger_snare
    nop

.Lmdr_product_hat_test:
    mov     r9, r0
    cmp/eq  #2, r0
    bt      .Lmdr_product_hat
    mov     r9, r0
    cmp/eq  #6, r0
    bt      .Lmdr_product_hat
    mov     r9, r0
    cmp/eq  #10, r0
    bt      .Lmdr_product_hat
    mov     r9, r0
    cmp/eq  #14, r0
    bf      .Lmdr_product_done
.Lmdr_product_hat:
    bsr     music_trigger_hat
    nop
.Lmdr_product_done:
    bra     .Lmdr_done
    nop

    .align 2
.Lmdr_product_480: .long 480
.Lmdr_product_512: .long 512
.Lmdr_product_544: .long 544
.Lmdr_product_572: .long 572
.Lmdr_product_575: .long 575
.Lmdr_product_576: .long 576

.Lmdr_elevated:
    ! HIGH COUNTRY exhales after the machine pulse: one nearly weightless bar
    ! emerges through cloud, then a restrained half-time drive grows under the
    ! gorge chase. Crashes at two-bar boundaries mark the white-field reveal,
    ! knife-edge pass, and final green-gold panorama without copying Elevated's
    ! score or vocal contour.
    mov.l   .Lmdr_576, r1
    cmp/eq  r1, r8
    bt      .Lmdr_elevated_impact
    mov.l   .Lmdr_608, r1
    cmp/eq  r1, r8
    bt      .Lmdr_elevated_impact
    mov.l   .Lmdr_640, r1
    cmp/eq  r1, r8
    bt      .Lmdr_elevated_impact
    bra     .Lmdr_elevated_final_test
    nop
.Lmdr_elevated_impact:
    bsr     music_trigger_crash
    nop

.Lmdr_elevated_final_test:
    mov.l   .Lmdr_668, r1
    cmp/hs  r1, r8
    bf      .Lmdr_elevated_intro_test
    bsr     music_trigger_snare
    nop
    mov.l   .Lmdr_671, r1
    cmp/eq  r1, r8
    bf      .Lmdr_elevated_done
    bsr     music_trigger_kick
    nop
    bsr     music_trigger_crash
    nop
    bra     .Lmdr_done
    nop

.Lmdr_elevated_intro_test:
    mov.l   .Lmdr_592, r1
    cmp/hs  r1, r8
    bt      .Lmdr_elevated_kick_test
    mov     r9, r0
    cmp/eq  #0, r0
    bt      .Lmdr_elevated_kick
    mov     r9, r0
    cmp/eq  #6, r0
    bt      .Lmdr_elevated_hat
    mov     r9, r0
    cmp/eq  #14, r0
    bt      .Lmdr_elevated_hat
    bra     .Lmdr_elevated_done
    nop

.Lmdr_elevated_kick_test:
    mov     r9, r0
    cmp/eq  #0, r0
    bt      .Lmdr_elevated_kick
    mov     r9, r0
    cmp/eq  #8, r0
    bt      .Lmdr_elevated_kick
    mov.l   .Lmdr_640, r1
    cmp/hs  r1, r8
    bf      .Lmdr_elevated_snare_test
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_elevated_snare_test
.Lmdr_elevated_kick:
    bsr     music_trigger_kick
    nop

.Lmdr_elevated_snare_test:
    mov     r9, r0
    cmp/eq  #4, r0
    bt      .Lmdr_elevated_snare
    mov     r9, r0
    cmp/eq  #12, r0
    bf      .Lmdr_elevated_hat_test
.Lmdr_elevated_snare:
    bsr     music_trigger_snare
    nop

.Lmdr_elevated_hat_test:
    mov     r9, r0
    cmp/eq  #2, r0
    bt      .Lmdr_elevated_hat
    mov     r9, r0
    cmp/eq  #6, r0
    bt      .Lmdr_elevated_hat
    mov     r9, r0
    cmp/eq  #10, r0
    bt      .Lmdr_elevated_hat
    mov     r9, r0
    cmp/eq  #14, r0
    bf      .Lmdr_elevated_done
.Lmdr_elevated_hat:
    bsr     music_trigger_hat
    nop
.Lmdr_elevated_done:
    bra     .Lmdr_done
    nop

music_trigger_kick:
    sts.l   pr, @-r15
    mov.l   .Lmdr_kick_age, r0
    mov     #0, r1
    mov.l   r1, @r0
    ! A two-step global execute first keys off only slot 4, then keys it back
    ! on at sample position zero. Already-active KYONB voices ignore the scan.
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmdr_kick_retrigger_done
    mov.l   .Lmdr_kick_control, r1
    mov.l   .Lmdr_kick_off_execute, r0
    mov.l   r0, @r1
    bsr     aica_fifo_wait
    nop
    tst     r0, r0
    bf      .Lmdr_kick_retrigger_done
    mov.l   .Lmdr_kick_control, r1
    mov.l   .Lmdr_kick_on_execute, r0
    mov.l   r0, @r1
.Lmdr_kick_retrigger_done:
    lds.l   @r15+, pr
    rts
    nop
music_trigger_snare:
    mov.l   .Lmdr_snare_age, r0
    mov     #0, r1
    mov.l   r1, @r0
    rts
    nop
music_trigger_hat:
    ! Do not truncate a still-ringing crash with a closed hat.
    mov.l   .Lmdr_hat_age, r0
    mov.l   @r0, r1
    cmp/pz  r1
    bf      .Lmdr_hat_start
    mov.l   .Lmdr_hat_kind, r2
    mov.l   @r2, r3
    tst     r3, r3
    bf      .Lmdr_hat_keep_crash
.Lmdr_hat_start:
    mov     #0, r1
    mov.l   r1, @r0
    mov.l   .Lmdr_hat_kind, r0
    mov.l   r1, @r0
.Lmdr_hat_keep_crash:
    rts
    nop
music_trigger_crash:
    mov.l   .Lmdr_hat_age, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov.l   .Lmdr_hat_kind, r0
    mov     #1, r1
    mov.l   r1, @r0
    rts
    nop

    .align 2
.Lmdr_576:       .long 576
.Lmdr_592:       .long 592
.Lmdr_608:       .long 608
.Lmdr_640:       .long 640
.Lmdr_668:       .long 668
.Lmdr_671:       .long 671
.Lmdr_kick_age:  .long music_kick_age
.Lmdr_snare_age: .long music_snare_age
.Lmdr_hat_age:   .long music_hat_age
.Lmdr_hat_kind:  .long music_hat_kind
.Lmdr_kick_control:    .long AICA_REG_BASE + 4 * MUSIC_VOICE_STRIDE
.Lmdr_kick_off_execute:.long 0x00008283
.Lmdr_kick_on_execute: .long 0x0000c283

! Stereo motion is deliberately broad and slow for the first three scenes.
! HYPERFOLD and MACHINE DREAM exchange sides once per beat; STRANGE FORM
! becomes still wider and swaps every two rows with its broken percussion.
! HIGH COUNTRY opens the image and drifts only once every two bars.
music_apply_pan:
    sts.l   pr, @-r15
    mov.l   .Lmap_current_row, r0
    mov.l   @r0, r8
    mov.l   .Lmap_current_scene, r0
    mov.l   @r0, r9
    mov     #0, r12                    ! normal-width pan by default
    tst     r9, r9
    bf      .Lmap_not_orbit
    shlr2   r8
    shlr2   r8                        ! one change per bar
    bra     .Lmap_phase
    nop
.Lmap_not_orbit:
    mov     #6, r0
    cmp/eq  r0, r9
    bt      .Lmap_elevated
    mov     #5, r0
    cmp/eq  r0, r9
    bt      .Lmap_wide_beat
    mov     #4, r0
    cmp/eq  r0, r9
    bt      .Lmap_strange
    mov     #3, r0
    cmp/eq  r0, r9
    bt      .Lmap_wide_beat
    shlr2   r8
    shlr2   r8                        ! one change per bar
    bra     .Lmap_phase
    nop
.Lmap_wide_beat:
    mov     #1, r12
    shlr2   r8                        ! one sweep per beat
    bra     .Lmap_phase
    nop
.Lmap_strange:
    mov     #1, r12
    shlr    r8                         ! nervous two-row stereo alternation
    bra     .Lmap_phase
    nop
.Lmap_elevated:
    mov     #1, r12
    shlr2   r8
    shlr2   r8
    shlr    r8                         ! one panoramic drift per two bars
.Lmap_phase:
    mov     #1, r0
    and     r0, r8
    tst     r8, r8
    bf      .Lmap_swapped
    mov     #0x13, r10                ! moderately left
    mov     #0x03, r11                ! moderately right
    tst     r12, r12
    bt      .Lmap_write
    mov     #0x17, r10                ! HYPERFOLD moves farther out
    mov     #0x07, r11
    bra     .Lmap_write
    nop
.Lmap_swapped:
    mov     #0x03, r10
    mov     #0x13, r11
    tst     r12, r12
    bt      .Lmap_write
    mov     #0x07, r10
    mov     #0x17, r11
.Lmap_write:
    mov     #0, r4
    mov     r10, r5
    bsr     music_set_pan
    nop
    mov     #1, r4
    mov     r11, r5
    bsr     music_set_pan
    nop
    mov     #3, r4
    mov     r11, r5
    bsr     music_set_pan
    nop
    mov     #7, r4
    mov     r10, r5
    bsr     music_set_pan
    nop
    mov     #8, r4
    mov     r10, r5
    bsr     music_set_pan
    nop
    mov     #9, r4
    mov     r11, r5
    bsr     music_set_pan
    nop
    mov     #10, r4
    mov     r10, r5
    bsr     music_set_pan
    nop
    mov     #11, r4
    mov     r10, r5
    bsr     music_set_pan
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmap_current_row:  .long music_current_row
.Lmap_current_scene:.long music_current_scene

! ---------------------------------------------------------------------------
! Per-frame voice animation
! ---------------------------------------------------------------------------

music_animate_voices:
    sts.l   pr, @-r15

    ! A restrained row-rate 16th-note sequence replaces the old 30 Hz pitch
    ! chatter. Root/third/fifth/octave now breathe with the four-beat harmony.
    mov.l   .Lma_row, r0
    mov.l   @r0, r10
    mov.l   .Lma_root, r0
    mov.l   @r0, r4
    mov     r10, r0
    mov     #3, r1
    and     r1, r0
    tst     r0, r0
    bt      .Lma_arp_note_ready
    cmp/eq  #1, r0
    bf      .Lma_arp_not_third
    mov.l   .Lma_quality, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Lma_arp_major
    add     #3, r4
    bra     .Lma_arp_note_ready
    nop
.Lma_arp_major:
    add     #4, r4
    bra     .Lma_arp_note_ready
    nop
.Lma_arp_not_third:
    cmp/eq  #2, r0
    bf      .Lma_arp_octave
    add     #7, r4
    bra     .Lma_arp_note_ready
    nop
.Lma_arp_octave:
    mov.l   .Lma_color, r0
    mov.l   @r0, r1
    add     r1, r4
.Lma_arp_note_ready:
    bsr     music_note_pitch
    nop
    mov     r0, r5
    mov     #0, r4
    bsr     music_set_pitch
    nop
    ! Staged levels turn the same oscillator from an intro glimmer into the
    ! chorus pulse while retaining ample headroom for the new stereo pad.
    mov     #78, r5
    mov     #16, r1
    cmp/hs  r1, r10
    bt      .Lma_arp_after_intro
    mov     #112, r5
    bra     .Lma_arp_accent
    nop
.Lma_arp_after_intro:
    mov     #32, r1
    cmp/hs  r1, r10
    bt      .Lma_arp_after_build
    mov     #92, r5
    bra     .Lma_arp_accent
    nop
.Lma_arp_after_build:
    mov     #96, r1
    cmp/hs  r1, r10
    bt      .Lma_arp_after_chorus
    mov     #82, r5
    bra     .Lma_arp_accent
    nop
.Lma_arp_after_chorus:
    mov.l   .Lma_hyperfold_row, r1
    cmp/hs  r1, r10
    bf      .Lma_arp_accent
    mov.l   .Lma_strange_row, r1
    cmp/hs  r1, r10
    bf      .Lma_arp_hyperfold
    mov.l   .Lma_elevated_row, r1
    cmp/hs  r1, r10
    bt      .Lma_arp_elevated
    mov.l   .Lma_product_row, r1
    cmp/hs  r1, r10
    bt      .Lma_arp_product
    mov     #80, r5                    ! percussion owns act five's foreground
    bra     .Lma_arp_accent
    nop
.Lma_arp_hyperfold:
    mov     #68, r5                    ! fourth-act arpeggio steps forward
    bra     .Lma_arp_accent
    nop
.Lma_arp_product:
    mov     #70, r5                    ! bright generator pulse for act six
    bra     .Lma_arp_accent
    nop
.Lma_arp_elevated:
    mov     #86, r5                    ! a distant Jarre-like pulse in cloud
    mov.l   .Lma_elevated_arrival, r1
    cmp/hs  r1, r10
    bf      .Lma_arp_accent
    mov     #72, r5                    ! octave arrival opens above the ridge
.Lma_arp_accent:
    mov     r10, r0
    mov     #3, r1
    and     r1, r0
    tst     r0, r0
    bf      .Lma_arp_tick_decay
    add     #-8, r5                    ! beat-head accent
.Lma_arp_tick_decay:
    mov.l   .Lma_tick, r0
    mov.l   @r0, r1
    shlr    r1
    add     r1, r5                     ! gentle eight-frame pluck
.Lma_arp_level:
    mov     #0, r4
    bsr     music_set_tl
    nop

    bsr     music_animate_lead
    nop
    bsr     music_animate_bass
    nop
    bsr     music_animate_echo
    nop
    bsr     music_animate_kick
    nop
    bsr     music_animate_snare
    nop
    bsr     music_animate_hat
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lma_tick:   .long music_current_tick
.Lma_row:    .long music_current_row
.Lma_root:   .long music_chord_root
.Lma_quality:.long music_chord_quality
.Lma_color:  .long music_chord_color
.Lma_hyperfold_row:.long 288
.Lma_strange_row: .long 384
.Lma_product_row: .long 480
.Lma_elevated_row:.long 576
.Lma_elevated_arrival:.long 640

music_animate_lead:
    sts.l   pr, @-r15
    mov.l   .Lmal_active, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Lmal_done
    mov.l   .Lmal_age, r0
    mov.l   @r0, r8
    mov     #16, r1
    cmp/hs  r1, r8
    bf      .Lmal_age_ready
    mov     #15, r8                    ! HOLD pins the voice at sustain
.Lmal_age_ready:

    mov.l   .Lmal_pitch, r0
    mov.l   @r0, r5
    mov.l   .Lmal_tick, r0
    mov.l   @r0, r0
    mov.l   .Lmal_vibrato, r1
    mov.b   @(r0, r1), r2
    exts.b  r2, r2
    add     r2, r5
    mov     #1, r4
    bsr     music_set_pitch
    nop
    mov.l   .Lmal_scene, r0
    mov.l   @r0, r0
    cmp/eq  #4, r0
    bt      .Lmal_strange_detune
    cmp/eq  #5, r0
    bt      .Lmal_product_detune
    cmp/eq  #6, r0
    bt      .Lmal_elevated_detune
    add     #3, r5                     ! subtle hardware-slot chorus double
    bra     .Lmal_double_ready
    nop
.Lmal_strange_detune:
    add     #12, r5                    ! abrasive close detune for the alarm lead
    bra     .Lmal_double_ready
    nop
.Lmal_product_detune:
    add     #6, r5                     ! clean but obvious V2-era supersaw width
    bra     .Lmal_double_ready
    nop
.Lmal_elevated_detune:
    add     #2, r5                     ! nearly unison, wide and wind-softened
.Lmal_double_ready:
    mov     #11, r4
    bsr     music_set_pitch
    nop
    mov     r8, r0
    mov.l   .Lmal_envelope, r1
    mov.b   @(r0, r1), r5
    extu.b  r5, r5
    mov.l   .Lmal_scene, r0
    mov.l   @r0, r0
    cmp/eq  #3, r0
    bf      .Lmal_level_ready
    add     #-6, r5                    ! lift the lead over the climax drums
.Lmal_level_ready:
    mov     #1, r4
    bsr     music_set_tl
    nop
    add     #12, r5
    mov     #11, r4
    bsr     music_set_tl
    nop
    add     #1, r8
    mov.l   .Lmal_age, r0
    mov.l   r8, @r0
    bra     .Lmal_done
    nop
.Lmal_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmal_active:  .long music_lead_active
.Lmal_age:     .long music_lead_age
.Lmal_pitch:   .long music_lead_pitch
.Lmal_tick:    .long music_current_tick
.Lmal_vibrato: .long music_lead_vibrato
.Lmal_envelope:.long music_lead_envelope
.Lmal_scene:   .long music_current_scene

music_animate_bass:
    sts.l   pr, @-r15
    mov.l   .Lmab_age, r0
    mov.l   @r0, r8
    cmp/pz  r8
    bf      .Lmab_done
    mov     #15, r0
    cmp/hi  r0, r8
    bf      .Lmab_age_ready
    mov     #15, r8
.Lmab_age_ready:
    mov     r8, r0
    mov.l   .Lmab_envelope, r1
    mov.b   @(r0, r1), r5
    extu.b  r5, r5
    mov     #2, r4
    bsr     music_set_tl
    nop
    mov.l   .Lmab_age, r0
    mov.l   @r0, r1
    add     #1, r1
    mov.l   r1, @r0
.Lmab_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmab_age:     .long music_bass_age
.Lmab_envelope:.long music_bass_envelope

music_animate_echo:
    sts.l   pr, @-r15
    mov.l   .Lmae_delay, r0
    mov.l   @r0, r8
    cmp/pz  r8
    bf      .Lmae_check_active
    tst     r8, r8
    bt      .Lmae_fire_pending
    add     #-1, r8
    mov.l   r8, @r0
    bra     .Lmae_check_active
    nop
.Lmae_fire_pending:
    mov     #-1, r1
    mov.l   r1, @r0
    mov.l   .Lmae_pending_pitch, r0
    mov.l   @r0, r1
    mov.l   .Lmae_pitch, r0
    mov.l   r1, @r0
    mov.l   .Lmae_active, r0
    mov     #1, r1
    mov.l   r1, @r0
    mov.l   .Lmae_age, r0
    mov     #0, r1
    mov.l   r1, @r0
.Lmae_check_active:
    mov.l   .Lmae_active, r0
    mov.l   @r0, r1
    tst     r1, r1
    bt      .Lmae_done
    mov.l   .Lmae_age, r0
    mov.l   @r0, r8
    mov     #24, r1
    cmp/hs  r1, r8
    bt      .Lmae_cut
    mov.l   .Lmae_pitch, r0
    mov.l   @r0, r5
    mov     #7, r4
    bsr     music_set_pitch
    nop
    mov     r8, r0
    mov.l   .Lmae_envelope, r1
    mov.b   @(r0, r1), r5
    extu.b  r5, r5
    mov     #7, r4
    bsr     music_set_tl
    nop
    add     #1, r8
    mov.l   .Lmae_age, r0
    mov.l   r8, @r0
    bra     .Lmae_done
    nop
.Lmae_cut:
    mov.l   .Lmae_active, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov     #7, r4
    mov     #-1, r5
    extu.b  r5, r5
    bsr     music_set_tl
    nop
.Lmae_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmae_active:  .long music_echo_active
.Lmae_age:     .long music_echo_age
.Lmae_pitch:   .long music_echo_pitch
.Lmae_envelope:.long music_echo_envelope
.Lmae_pending_pitch:.long music_echo_pending_pitch
.Lmae_delay:   .long music_echo_delay

music_animate_kick:
    sts.l   pr, @-r15
    mov.l   .Lmak_age, r0
    mov.l   @r0, r8
    cmp/pz  r8
    bf      .Lmak_done
    mov     #10, r1
    cmp/hs  r1, r8
    bt      .Lmak_cut
    mov     r8, r0
    shll    r0
    mov.l   .Lmak_pitch_curve, r1
    mov.w   @(r0, r1), r5
    extu.w  r5, r5
    mov     #4, r4
    bsr     music_set_pitch
    nop
    mov     r8, r0
    mov.l   .Lmak_envelope, r1
    mov.b   @(r0, r1), r5
    extu.b  r5, r5
    mov     #4, r4
    bsr     music_set_tl
    nop
    add     #1, r8
    mov.l   .Lmak_age, r0
    mov.l   r8, @r0
    bra     .Lmak_done
    nop
.Lmak_cut:
    mov     #4, r4
    mov     #-1, r5
    extu.b  r5, r5
    bsr     music_set_tl
    nop
    mov.l   .Lmak_age, r0
    mov     #-1, r1
    mov.l   r1, @r0
.Lmak_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmak_age:        .long music_kick_age
.Lmak_pitch_curve:.long music_kick_pitch_curve
.Lmak_envelope:   .long music_kick_envelope

music_animate_snare:
    sts.l   pr, @-r15
    mov.l   .Lmas_age, r0
    mov.l   @r0, r8
    cmp/pz  r8
    bf      .Lmas_done
    mov     #8, r1
    cmp/hs  r1, r8
    bt      .Lmas_cut
    tst     r8, r8
    bf      .Lmas_level
    mov     #5, r4
    mov.l   .Lmas_noise_pitch, r5
    bsr     music_set_pitch
    nop
.Lmas_level:
    mov     r8, r0
    mov.l   .Lmas_envelope, r1
    mov.b   @(r0, r1), r5
    extu.b  r5, r5
    mov     #5, r4
    bsr     music_set_tl
    nop
    mov     r8, r0
    shll    r0
    mov.l   .Lmas_body_pitch, r1
    mov.w   @(r0, r1), r5
    extu.w  r5, r5
    mov     #12, r4
    bsr     music_set_pitch
    nop
    mov     r8, r0
    mov.l   .Lmas_body_envelope, r1
    mov.b   @(r0, r1), r5
    extu.b  r5, r5
    mov     #12, r4
    bsr     music_set_tl
    nop
    add     #1, r8
    mov.l   .Lmas_age, r0
    mov.l   r8, @r0
    bra     .Lmas_done
    nop
.Lmas_cut:
    mov     #5, r4
    mov     #-1, r5
    extu.b  r5, r5
    bsr     music_set_tl
    nop
    mov     #12, r4
    bsr     music_set_tl
    nop
    mov.l   .Lmas_age, r0
    mov     #-1, r1
    mov.l   r1, @r0
.Lmas_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmas_age:        .long music_snare_age
.Lmas_envelope:   .long music_snare_envelope
.Lmas_body_envelope:.long music_snare_body_envelope
.Lmas_body_pitch: .long music_snare_body_pitch
.Lmas_noise_pitch:.long 0x00007000

music_animate_hat:
    sts.l   pr, @-r15
    mov.l   .Lmah_age, r0
    mov.l   @r0, r8
    cmp/pz  r8
    bf      .Lmah_done
    mov.l   .Lmah_kind, r0
    mov.l   @r0, r9
    tst     r9, r9
    bf      .Lmah_crash
    mov     #4, r1
    cmp/hs  r1, r8
    bt      .Lmah_cut
    tst     r8, r8
    bf      .Lmah_closed_level
    mov     #6, r4
    mov.l   .Lmah_closed_pitch, r5
    bsr     music_set_pitch
    nop
.Lmah_closed_level:
    mov     r8, r0
    mov.l   .Lmah_closed_env, r1
    mov.b   @(r0, r1), r5
    extu.b  r5, r5
    bra     .Lmah_write_level
    nop
.Lmah_crash:
    mov     #16, r1
    cmp/hs  r1, r8
    bt      .Lmah_cut
    tst     r8, r8
    bf      .Lmah_crash_level
    mov     #6, r4
    mov.l   .Lmah_crash_pitch, r5
    bsr     music_set_pitch
    nop
.Lmah_crash_level:
    mov     r8, r0
    mov.l   .Lmah_crash_env, r1
    mov.b   @(r0, r1), r5
    extu.b  r5, r5
.Lmah_write_level:
    mov     #6, r4
    bsr     music_set_tl
    nop
    add     #1, r8
    mov.l   .Lmah_age, r0
    mov.l   r8, @r0
    bra     .Lmah_done
    nop
.Lmah_cut:
    mov     #6, r4
    mov     #-1, r5
    extu.b  r5, r5
    bsr     music_set_tl
    nop
    mov.l   .Lmah_age, r0
    mov     #-1, r1
    mov.l   r1, @r0
.Lmah_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmah_age:         .long music_hat_age
.Lmah_kind:        .long music_hat_kind
.Lmah_closed_env:  .long music_hat_envelope
.Lmah_crash_env:   .long music_crash_envelope
.Lmah_crash_pitch: .long 0x00006800
.Lmah_closed_pitch:.long 0x00007800

! ---------------------------------------------------------------------------
! Small register helpers
! ---------------------------------------------------------------------------

! Convert MIDI-minus-36 note in r4 to an AICA OCT/FNS pitch word in r0.
music_note_pitch:
    shll    r4
    mov     r4, r0
    mov.l   .Lmnp_table, r1
    mov.w   @(r0, r1), r0
    extu.w  r0, r0
    rts
    nop

! r4 = slot, r5 = pitch word
music_set_pitch:
    sts.l   pr, @-r15
    mov.l   .Lmsp_fifo_wait, r0
    jsr     @r0
    nop
    tst     r0, r0
    bf      .Lmsp_done
    mov     r4, r0
    shll8   r0
    shlr    r0                         ! slot * 0x80
    mov.l   .Lmsp_reg_base, r1
    add     r0, r1
    add     #0x18, r1
    mov.l   r5, @r1
.Lmsp_done:
    lds.l   @r15+, pr
    rts
    nop

! r4 = slot, r5 = attenuation 0 (loud) .. 255 (mute)
music_set_tl:
    sts.l   pr, @-r15
    mov.l   .Lmst_fifo_wait, r0
    jsr     @r0
    nop
    tst     r0, r0
    bf      .Lmst_done
    mov     r4, r0
    shll8   r0
    shlr    r0
    mov.l   .Lmst_reg_base, r1
    add     r0, r1
    add     #0x28, r1
    mov     r5, r2
    shll8   r2
    mov     #0x24, r0                  ! LPF bypass
    or      r0, r2
    mov.l   r2, @r1
.Lmst_done:
    lds.l   @r15+, pr
    rts
    nop

! r4 = slot, r5 = AICA pan code (0 center, 0x1f hard left, 0x0f hard right)
music_set_pan:
    sts.l   pr, @-r15
    mov.l   .Lmspa_fifo_wait, r0
    jsr     @r0
    nop
    tst     r0, r0
    bf      .Lmspa_done
    mov     r4, r0
    shll8   r0
    shlr    r0
    mov.l   .Lmspa_reg_base, r1
    add     r0, r1
    add     #0x24, r1
    mov     r5, r2
    mov.l   .Lmspa_direct_send, r0
    or      r0, r2
    mov.l   r2, @r1
.Lmspa_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmnp_table:       .long music_pitch_table
.Lmsp_fifo_wait:   .long aica_fifo_wait
.Lmst_fifo_wait:   .long aica_fifo_wait
.Lmspa_fifo_wait:  .long aica_fifo_wait
.Lmsp_reg_base:    .long AICA_REG_BASE
.Lmst_reg_base:    .long AICA_REG_BASE
.Lmspa_reg_base:   .long AICA_REG_BASE
.Lmspa_direct_send:.long 0x00000f00

! Sample slot zero's current address once per tracker row. Different values are
! counted for the serial health report; a nonzero count proves KYON execution,
! pitch stepping, looping, and the common monitor path are all alive.
music_monitor_position:
    sts.l   pr, @-r15
    mov.l   .Lmmp_fifo_wait, r0
    jsr     @r0
    nop
    tst     r0, r0
    bf      .Lmmp_done
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    mov.l   .Lmmp_position_reg, r1
    mov.l   @r1, r0
    extu.w  r0, r0
    mov.l   .Lmmp_previous, r1
    mov.l   @r1, r2
    mov.l   r0, @r1
    cmp/eq  r2, r0
    bt      .Lmmp_done
    mov.l   .Lmmp_moves, r1
    mov.l   @r1, r2
    add     #1, r2
    mov.l   r2, @r1
.Lmmp_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmmp_fifo_wait:   .long aica_fifo_wait
.Lmmp_position_reg:.long AICA_MONITOR_POSITION
.Lmmp_previous:    .long music_previous_position
.Lmmp_moves:       .long music_position_moves

! Appended to the existing 600-frame TMU report. This serial output occurs
! before perf_sample clears its timing baseline and therefore cannot create a
! false missed-vblank result.
music_perf_report:
    sts.l   pr, @-r15
    mov.l   .Lmpr_msg_rows, r4
    mov.l   .Lmpr_serial_puts, r0
    jsr     @r0
    nop
    mov.l   .Lmpr_rows, r0
    mov.l   @r0, r4
    mov.l   .Lmpr_serial_hex, r0
    jsr     @r0
    nop
    mov.l   .Lmpr_msg_moves, r4
    mov.l   .Lmpr_serial_puts, r0
    jsr     @r0
    nop
    mov.l   .Lmpr_moves, r0
    mov.l   @r0, r4
    mov.l   .Lmpr_serial_hex, r0
    jsr     @r0
    nop
    mov.l   .Lmpr_crlf, r4
    mov.l   .Lmpr_serial_puts, r0
    jsr     @r0
    nop
    mov     #0, r1
    mov.l   .Lmpr_rows, r0
    mov.l   r1, @r0
    mov.l   .Lmpr_moves, r0
    mov.l   r1, @r0
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lmpr_msg_rows:  .long msg_music_rows
.Lmpr_msg_moves: .long msg_music_moves
.Lmpr_crlf:      .long msg_crlf
.Lmpr_rows:      .long music_rows_seen
.Lmpr_moves:     .long music_position_moves
.Lmpr_serial_puts:.long serial_puts
.Lmpr_serial_hex: .long serial_puthex32

! ---------------------------------------------------------------------------
! Read-only score and timbre data
! ---------------------------------------------------------------------------

    .section .rodata.music,"a"
    .align 5

! Zero-DC single-cycle PCM8 waves. The deliberately sharp pulse spectra are
! the recognizable keygen timbre; triangle/glass and saw voices broaden it.
music_waveforms:
music_wave_square:
    .byte 0,24,44,52,52,52,52,52,52,52,52,52,52,44,24,0
    .byte 0,-24,-44,-52,-52,-52,-52,-52,-52,-52,-52,-52,-52,-44,-24,0
music_wave_pulse25:
    .rept 8
    .byte 78
    .endr
    .rept 24
    .byte -26
    .endr
music_wave_pulse12:
    .byte 48,72,72,48,-6,-6
    .rept 24
    .byte -9
    .endr
    .byte -6,-6
music_wave_triangle:
    .byte 0,10,20,30,40,50,60,70,80,70,60,50,40,30,20,10
    .byte 0,-10,-20,-30,-40,-50,-60,-70,-80,-70,-60,-50,-40,-30,-20,-10
music_wave_saw:
    .byte -46,-43,-40,-37,-34,-31,-28,-25
    .byte -22,-19,-16,-13,-10,-7,-4,-1
    .byte 1,4,7,10,13,16,19,22
    .byte 25,28,31,34,37,40,43,46
music_waveforms_end:
    .align 2

! sample-low, loop-end, initial pitch, direct-send/pan, control-low
music_voice_descriptors:
    .long MUSIC_SAMPLE_SQUARE   & 0xffff, 32, 0x00006ad1, 0x00000f13, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_PULSE12  & 0xffff, 32, 0x0000711b, 0x00000f03, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_PULSE25  & 0xffff, 32, 0x000062d1, 0x00000f00, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_TRIANGLE & 0xffff, 32, 0x0000700d, 0x00000f13, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_TRIANGLE & 0xffff, 32, 0x00006a13, 0x00000f00, MUSIC_PCM8_LOOP_CTL
    .long 0,                              32, 0x00007000, 0x00000f13, MUSIC_NOISE_LOOP_CTL
    .long 0,                              32, 0x00007800, 0x00000f03, MUSIC_NOISE_LOOP_CTL
    .long MUSIC_SAMPLE_SAW      & 0xffff, 32, 0x00006ad1, 0x00000f13, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_TRIANGLE & 0xffff, 32, 0x0000711b, 0x00000f13, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_TRIANGLE & 0xffff, 32, 0x0000726f, 0x00000f03, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_SQUARE   & 0xffff, 32, 0x000078d2, 0x00000f13, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_PULSE25  & 0xffff, 32, 0x0000711b, 0x00000f13, MUSIC_PCM8_LOOP_CTL
    .long MUSIC_SAMPLE_TRIANGLE & 0xffff, 32, 0x0000691b, 0x00000f00, MUSIC_PCM8_LOOP_CTL

! Exact AICA pitch words for MIDI 36 (C2) through MIDI 83 (B5), assuming a
! 32-sample oscillator. Values encode signed OCT and ten-bit FNS directly.
music_pitch_table:
    .word 0x5a13,0x5a6f,0x5ad1,0x5b39,0x5ba7,0x600d,0x604b,0x608d
    .word 0x60d2,0x611b,0x6169,0x61bb,0x6213,0x626f,0x62d1,0x6339
    .word 0x63a7,0x680d,0x684b,0x688d,0x68d2,0x691b,0x6969,0x69bb
    .word 0x6a13,0x6a6f,0x6ad1,0x6b39,0x6ba7,0x700d,0x704b,0x708d
    .word 0x70d2,0x711b,0x7169,0x71bb,0x7213,0x726f,0x72d1,0x7339
    .word 0x73a7,0x780d,0x784b,0x788d,0x78d2,0x791b,0x7969,0x79bb

    .align 2
music_lead_vibrato:
    .byte 0,1,2,1,0,-1,-2,-1
music_lead_envelope:
    .byte 74,64,58,54,52,52,53,54,55,56,58,60,62,64,66,68
music_bass_envelope:
    .byte 56,54,55,58,62,68,76,86,98,114,134,158,184,210,232,248
music_echo_envelope:
    .byte 96,96,98,100,103,106,110,114,119,124,130,136
    .byte 143,150,158,167,177,188,200,213,226,238,248,255
music_kick_envelope:
    .byte 52,54,58,64,74,88,108,132,164,208
music_kick_pitch_curve:
    .word 0x6339,0x6213,0x608d,0x5ad1,0x5a13
    .word 0x5a13,0x5a13,0x5a13,0x5a13,0x5a13
music_snare_envelope:
    .byte 66,70,77,87,102,122,150,192
music_snare_body_envelope:
    .byte 70,72,78,88,104,126,156,202
music_snare_body_pitch:
    .word 0x691b,0x68d2,0x688d,0x684b,0x680d,0x63a7,0x63a7,0x63a7
music_hat_envelope:
    .byte 100,132,178,232
music_crash_envelope:
    .byte 82,86,91,97,104,112,121,131,142,154,168,184,202,222,240,255

! One 12-byte bar descriptor: chord root, minor flag, eight even-row melody
! events, color interval, and explicit low bass root. The original progression
! uses add9, major-7, minor-7, sixth, and dominant colors rather than cycling
! generic triads.
    .macro MUSIC_BAR root, minor, color, bass, n0, n1, n2, n3, n4, n5, n6, n7
    .byte \root, \minor, \n0, \n1, \n2, \n3, \n4, \n5, \n6, \n7, \color, \bass
    .endm

    .align 2
music_bars:
    ! ORBIT CORE: nocturnal ignition, plunge, then the first complete hook.
    MUSIC_BAR MN_FS4, 1, 14, MN_FS2, MN_REST,MN_REST,MN_REST,MN_REST,MN_REST,MN_REST,MN_CS5, MN_HOLD
    MUSIC_BAR MN_D4,  0, 11, MN_D2,  MN_FS4, MN_HOLD,MN_A4,  MN_HOLD,MN_CS5, MN_HOLD,MN_FS5, MN_HOLD
    MUSIC_BAR MN_A3,  0, 14, MN_A2,  MN_CS5, MN_HOLD,MN_E5,  MN_CS5, MN_REST,MN_A4,  MN_HOLD,MN_GS4
    MUSIC_BAR MN_E4,  0,  9, MN_E2,  MN_FS4, MN_HOLD,MN_A4,  MN_HOLD,MN_CS5, MN_B4,  MN_A4,  MN_REST
    MUSIC_BAR MN_FS4, 1, 14, MN_FS2, MN_CS5, MN_HOLD,MN_E5,  MN_FS5, MN_REST,MN_E5,  MN_CS5, MN_A4
    MUSIC_BAR MN_CS4, 0, 10, MN_CS2, MN_GS4, MN_HOLD,MN_CS5, MN_HOLD,MN_F5,  MN_DS5, MN_CS5, MN_HOLD

    ! NEON VAULT: the wide chorus arrives, breathes for one bar, then climbs.
    MUSIC_BAR MN_B3,  1, 10, MN_B2,  MN_REST,MN_REST,MN_REST,MN_REST,MN_REST,MN_REST,MN_REST,MN_REST
    MUSIC_BAR MN_D4,  0, 11, MN_D2,  MN_FS4, MN_HOLD,MN_A4,  MN_B4,  MN_REST,MN_D5,  MN_CS5, MN_REST
    MUSIC_BAR MN_A3,  0, 14, MN_A2,  MN_A4,  MN_HOLD,MN_CS5, MN_HOLD,MN_FS5, MN_E5,  MN_CS5, MN_REST
    MUSIC_BAR MN_E4,  0,  9, MN_E2,  MN_GS4, MN_B4,  MN_REST,MN_CS5, MN_HOLD,MN_B4,  MN_GS4, MN_REST
    MUSIC_BAR MN_FS4, 1, 14, MN_FS2, MN_CS5, MN_HOLD,MN_E5,  MN_FS5, MN_REST,MN_E5,  MN_CS5, MN_A4
    MUSIC_BAR MN_CS4, 0, 10, MN_CS2, MN_GS4, MN_HOLD,MN_CS5, MN_HOLD,MN_F5,  MN_DS5, MN_CS5, MN_REST

    ! CHAOS BLOOM: luminous fractured bridge and dominant launch into act four.
    MUSIC_BAR MN_FS4, 1, 14, MN_FS2, MN_FS5, MN_HOLD,MN_E5,  MN_CS5, MN_REST,MN_A4,  MN_CS5, MN_REST
    MUSIC_BAR MN_E4,  0,  9, MN_E2,  MN_B4,  MN_HOLD,MN_E5,  MN_FS5, MN_REST,MN_E5,  MN_B4,  MN_REST
    MUSIC_BAR MN_D4,  0, 11, MN_D2,  MN_A4,  MN_HOLD,MN_CS5, MN_D5,  MN_REST,MN_FS5, MN_E5,  MN_CS5
    MUSIC_BAR MN_A3,  0, 14, MN_CS2, MN_E5,  MN_HOLD,MN_CS5, MN_B4,  MN_A4,  MN_REST,MN_CS5, MN_HOLD
    MUSIC_BAR MN_B3,  1, 10, MN_B2,  MN_REST,MN_REST,MN_REST,MN_A4,  MN_HOLD,MN_CS5, MN_REST,MN_REST
    MUSIC_BAR MN_CS4, 0, 10, MN_CS2, MN_GS4, MN_HOLD,MN_B4,  MN_CS5, MN_REST,MN_F5,  MN_DS5, MN_CS5

    ! HYPERFOLD: six-bar final drop. Bass inversions make the familiar harmony
    ! seem to rotate under a new, continuous high-register answer to the hook.
    MUSIC_BAR MN_FS4, 1, 14, MN_FS2, MN_A4,  MN_CS5, MN_FS5, MN_HOLD,MN_E5,  MN_CS5, MN_A4,  MN_CS5
    MUSIC_BAR MN_D4,  0, 11, MN_A2,  MN_A4,  MN_D5,  MN_FS5, MN_A5,  MN_HOLD,MN_FS5, MN_E5,  MN_CS5
    MUSIC_BAR MN_A3,  0, 14, MN_CS2, MN_CS5, MN_E5,  MN_A5,  MN_HOLD,MN_GS5, MN_E5,  MN_CS5, MN_B4
    MUSIC_BAR MN_E4,  0,  9, MN_GS2, MN_B4,  MN_E5,  MN_GS5, MN_B5,  MN_HOLD,MN_GS5, MN_E5,  MN_CS5
    MUSIC_BAR MN_B3,  1, 10, MN_B2,  MN_B4,  MN_D5,  MN_FS5, MN_A5,  MN_FS5, MN_D5,  MN_CS5, MN_B4
    MUSIC_BAR MN_CS4, 0, 10, MN_CS2, MN_GS4, MN_CS5, MN_F5,  MN_GS5, MN_F5,  MN_DS5, MN_CS5, MN_HOLD

    ! STRANGE FORM: an original industrial-DnB coda. Tritones, a minor-major
    ! seventh, and flat-nine pressure replace the clean synthwave resolution;
    ! its F#-D-B-C# bass cycle still launches naturally into the opening loop.
    MUSIC_BAR MN_FS4, 1,  6, MN_FS2, MN_FS5,MN_REST,MN_C5, MN_HOLD,MN_A4, MN_REST,MN_DS5,MN_REST
    MUSIC_BAR MN_D4,  1, 11, MN_D2,  MN_F5, MN_REST,MN_B4, MN_HOLD,MN_GS4,MN_REST,MN_CS5,MN_REST
    MUSIC_BAR MN_B3,  1,  6, MN_B2,  MN_B4, MN_D5, MN_F5, MN_REST,MN_FS5,MN_REST,MN_C5, MN_HOLD
    MUSIC_BAR MN_CS4, 0, 13, MN_CS2, MN_GS4,MN_CS5,MN_D5, MN_REST,MN_G4, MN_REST,MN_F5, MN_REST
    MUSIC_BAR MN_FS4, 1,  6, MN_CS2, MN_A4, MN_CS5,MN_FS5,MN_REST,MN_C5, MN_HOLD,MN_DS5,MN_REST
    MUSIC_BAR MN_CS4, 0, 13, MN_CS2, MN_GS4,MN_CS5,MN_D5, MN_G4, MN_F5, MN_DS5,MN_CS5,MN_REST

    ! MACHINE DREAM: an original Y2K generator-trance finale. A clean
    ! F#m-E-D-A-Bm-C# ascent restores four-on-the-floor momentum after the
    ! industrial coda, while the final dominant bar points back to Orbit Core.
    MUSIC_BAR MN_FS4, 1, 14, MN_FS2, MN_REST,MN_CS5,MN_FS5,MN_A5, MN_HOLD,MN_FS5,MN_E5, MN_CS5
    MUSIC_BAR MN_E4,  0, 11, MN_E2,  MN_B4,  MN_E5, MN_GS5,MN_B5, MN_HOLD,MN_GS5,MN_FS5,MN_E5
    MUSIC_BAR MN_D4,  0, 14, MN_D2,  MN_A4,  MN_D5, MN_FS5,MN_A5, MN_HOLD,MN_FS5,MN_E5, MN_D5
    MUSIC_BAR MN_A3,  0, 14, MN_A2,  MN_A4,  MN_CS5,MN_E5, MN_A5, MN_GS5,MN_E5, MN_CS5,MN_B4
    MUSIC_BAR MN_B3,  1, 10, MN_B2,  MN_B4,  MN_D5, MN_FS5,MN_B5, MN_A5, MN_FS5,MN_D5, MN_CS5
    MUSIC_BAR MN_CS4, 0, 10, MN_CS2, MN_GS4, MN_CS5,MN_F5, MN_GS5,MN_F5, MN_DS5,MN_CS5,MN_HOLD

    ! HIGH COUNTRY: an original epic-electronic release of pressure. Suspended
    ! minor colors and open fifths emerge through cloud, then a wide octave
    ! arrival crests over the thawed panorama before C# points back to F#.
    MUSIC_BAR MN_FS4, 1, 14, MN_FS2, MN_REST,MN_REST,MN_CS5,MN_HOLD,MN_FS5,MN_HOLD,MN_A5, MN_HOLD
    MUSIC_BAR MN_D4,  0, 11, MN_D2,  MN_A4,  MN_HOLD,MN_D5, MN_HOLD,MN_FS5,MN_HOLD,MN_A5, MN_HOLD
    MUSIC_BAR MN_A3,  0, 14, MN_A2,  MN_E5,  MN_HOLD,MN_CS5,MN_E5,  MN_A5, MN_HOLD,MN_E5, MN_CS5
    MUSIC_BAR MN_E4,  0,  9, MN_E2,  MN_B4,  MN_HOLD,MN_E5, MN_FS5, MN_GS5,MN_HOLD,MN_B5, MN_GS5
    MUSIC_BAR MN_B3,  1, 10, MN_B2,  MN_FS5, MN_HOLD,MN_B5, MN_A5,  MN_FS5,MN_D5, MN_FS5,MN_A5
    MUSIC_BAR MN_CS4, 0, 10, MN_CS2, MN_GS4, MN_CS5,MN_F5, MN_GS5, MN_F5, MN_DS5,MN_CS5,MN_HOLD
music_bars_end:

msg_music_ok:
    .asciz "AICA: thirteen-slot raw synthwave / DnB / trance / ambient tracker online at 112.40 BPM\r\n"
msg_music_fail:
    .asciz "AICA WARNING: sound path unavailable; visuals continue\r\n"
msg_music_rows:
    .asciz "AICA tracker rows / 600 frames = 0x"
msg_music_moves:
    .asciz "  slot-0 phase changes = 0x"

! ---------------------------------------------------------------------------
! Tracker state
! ---------------------------------------------------------------------------

    .section .bss.music,"aw",@nobits
    .align 5
music_enabled:           .space 4
music_last_row:          .space 4
music_current_row:       .space 4
music_current_tick:      .space 4
music_current_scene:     .space 4
music_row_jump:          .space 4
music_chord_root:        .space 4
music_chord_quality:     .space 4
music_chord_color:       .space 4
music_true_bass_root:    .space 4
music_lead_active:       .space 4
music_lead_pitch:        .space 4
music_lead_age:          .space 4
music_echo_active:       .space 4
music_echo_pitch:        .space 4
music_echo_age:          .space 4
music_echo_pending_pitch:.space 4
music_echo_delay:        .space 4
music_bass_pitch:        .space 4
music_bass_age:          .space 4
music_kick_age:          .space 4
music_snare_age:         .space 4
music_hat_age:           .space 4
music_hat_kind:          .space 4
music_rows_seen:         .space 4
music_previous_position: .space 4
music_position_moves:    .space 4
    .align 5
