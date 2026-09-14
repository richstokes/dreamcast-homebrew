! ---------------------------------------------------------------------------
! CHROMA CIRCUIT -- Scene 07: ABOVE THE WEATHER
!
! An original Dreamcast-native alpine flight inspired by the procedural
! landscape grammar of RGBA & TBC's Elevated: one unbroken low-altitude path,
! terrain that changes material with slope and season, deep atmospheric
! perspective, a hard sun, cloud strata, water, and restrained lens ghosts.
!
! No source, assets, geometry, text, or music from Elevated are used. The
! landscape below is an analytic SH-4 construction: two crossing trigonometric
! ridge fields plus a quadratic valley envelope, sampled directly into a
! 25 x 25 projected grid every frame. The camera follows the same centerline
! height function and banks into its slowly wandering path.
!
! The continuous 768-frame flight passes through four weather regions:
!     0..191  a cold blue ravine under layered mist
!   192..383  a severe white snowfield and slate peaks
!   384..575  a gold cloud-sea summit above the weather
!   576..767  thawed green ridges and a reflective mountain lake
!
! Fixed peak geometry, excluding the shared title / transition:
!   1152  terrain triangles (24 strips x 48)
!      2  sky triangles
!      4  cloud / horizon-strata triangles
!      2  lake triangles (thawed region only)
!      8  vignette triangles
!     96  illuminated ridge-line triangles
!     32  sun / halo triangles
!     16  sparse radial-ray triangles
!     24  lens-ghost triangles
!   ----
!   1336  triangles / 1,627 native parameter blocks at the peak.
!
! The projection grid and all writable scene state occupy exactly 7,576 bytes
! (7.40 KiB). Opaque geometry is terminated before the additive pass; the
! common renderer owns the final translucent-list terminator.
! ---------------------------------------------------------------------------

    .section .text,"ax"
    .align 2

! r9 is the scene-local frame (0..767). Preserve it for the shared transition.
draw_scene_elevated:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lelevated_scene_prepare, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_build, r0
    jsr     @r0
    nop

    ! Solid pass: atmospheric field first, then terrain and near-frame masks.
    mov.l   .Lelevated_scene_opaque, r4
    mov.l   .Lelevated_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_sky, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_terrain, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_lake, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_vignette, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_title, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_zero, r4
    mov.l   .Lelevated_scene_submit, r0
    jsr     @r0
    nop

    ! ONE/ONE is reserved for sun energy and sparse glints. Terrain remains
    ! opaque and stable instead of becoming a screen full of alpha overdraw.
    mov.l   .Lelevated_scene_additive, r4
    mov.l   .Lelevated_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_ridges, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_sun, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_rays, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_scene_lens, r0
    jsr     @r0
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
.Lelevated_scene_prepare:  .long prepare_elevated_flight
.Lelevated_scene_build:    .long build_elevated_grid
.Lelevated_scene_sky:      .long draw_elevated_sky
.Lelevated_scene_terrain:  .long draw_elevated_terrain
.Lelevated_scene_lake:     .long draw_elevated_lake
.Lelevated_scene_vignette: .long draw_elevated_vignette
.Lelevated_scene_ridges:   .long draw_elevated_ridge_light
.Lelevated_scene_sun:      .long draw_elevated_sun
.Lelevated_scene_rays:     .long draw_elevated_sun_rays
.Lelevated_scene_lens:     .long draw_elevated_lens_ghosts
.Lelevated_scene_submit:   .long ta_submit
.Lelevated_scene_title:    .long draw_title
.Lelevated_scene_opaque:   .long opaque_header
.Lelevated_scene_additive: .long additive_header
.Lelevated_scene_zero:     .long zero_block

! ---------------------------------------------------------------------------
! Continuous camera / weather preparation.
!
! The centerline terrain height is evaluated from the exact same two ridges
! used for the grid. Adding only a phase-specific clearance makes the camera
! actually follow the landscape rather than merely bob in screen space.
! ---------------------------------------------------------------------------

prepare_elevated_flight:
    mov     r9, r2
    mov     #0, r3
    mov.l   .Lelevated_phase_1, r0
    cmp/hs  r0, r2
    bf      .Lelevated_phase_ready
    mov     #1, r3
    mov.l   .Lelevated_phase_2, r0
    cmp/hs  r0, r2
    bf      .Lelevated_phase_ready
    mov     #2, r3
    mov.l   .Lelevated_phase_3, r0
    cmp/hs  r0, r2
    bf      .Lelevated_phase_ready
    mov     #3, r3
.Lelevated_phase_ready:
    mov.l   .Lelevated_grade_ptr, r0
    mov.l   r3, @r0

    ! Forward sample phase = frame * 144. It is intentionally not reset at
    ! weather boundaries, so ridge silhouettes flow continuously through them.
    mov     r9, r2
    shll8   r2
    shlr    r2                    ! frame * 128
    mov     r9, r1
    shll2   r1
    shll2   r1                    ! frame * 16
    add     r1, r2
    mov.l   .Lelevated_travel_ptr, r0
    mov.l   r2, @r0

    ! A broad lateral wander is converted to phase rather than world X. The
    ! camera therefore crosses different ridges without moving the finite grid.
    mov     r9, r0
    shll8   r0
    shlr2   r0                    ! frame * 64
    mov     r9, r1
    shll2   r1
    shll2   r1
    sub     r1, r0                ! frame * 48
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lelevated_lateral_scale, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    ftrc    fr0, fpul
    sts     fpul, r1
    mov.l   .Lelevated_lateral_ptr, r0
    mov.l   r1, @r0

    ! Bank is the path derivative (cosine), restrained to +/-5.6 degrees.
    mov.l   .Lelevated_bank_units, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr1
    ftrc    fr1, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr2
    mov.l   .Lelevated_roll_ptr, r0
    fmov.s  fr2, @r0
    add     #4, r0
    fmov.s  fr3, @r0

    ! Exact center sample: row 0, column 12.
    mov.l   .Lelevated_travel_ptr, r0
    mov.l   @r0, r2
    mov.l   .Lelevated_lateral_ptr, r0
    mov.l   @r0, r1
    add     r1, r2
    mov.l   .Lelevated_center_angle_1, r0
    add     r0, r2
    lds     r2, fpul
    fsca    fpul, dr0
    fabs    fr0
    fmov    fr0, fr4
    fmul    fr0, fr4
    fmul    fr0, fr4              ! narrow first ridge

    mov.l   .Lelevated_travel_ptr, r0
    mov.l   @r0, r2
    mov.l   .Lelevated_lateral_ptr, r0
    mov.l   @r0, r1
    sub     r1, r2
    mov.l   .Lelevated_center_angle_2, r0
    add     r0, r2
    lds     r2, fpul
    fsca    fpul, dr2
    fabs    fr2
    fmul    fr2, fr2              ! broader crossing ridge

    mov.l   .Lelevated_grade_ptr, r0
    mov.l   @r0, r3
    shll2   r3
    mov.l   .Lelevated_amp1_table, r0
    add     r3, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr4
    mov.l   .Lelevated_amp2_table, r0
    add     r3, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr2
    fadd    fr2, fr4
    mov.l   .Lelevated_clearance_table, r0
    add     r3, r0
    fmov.s  @r0, fr5
    fadd    fr5, fr4
    mov.l   .Lelevated_ground_ptr, r0
    fmov.s  fr4, @r0

    ! Sun tracks slowly across the upper-right sky while the terrain rolls.
    mov     r9, r0
    shll2   r0
    shll2   r0
    shll    r0                    ! frame * 32
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lelevated_sun_x_amp, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    mov.l   .Lelevated_sun_x_base, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr0
    mov.l   .Lelevated_sun_ptr, r0
    fmov.s  fr0, @r0
    mov.l   .Lelevated_sun_y_amp, r1
    fmov.s  @r1, fr2
    fmul    fr2, fr1
    mov.l   .Lelevated_sun_y_base, r1
    fmov.s  @r1, fr3
    fadd    fr3, fr1
    add     #4, r0
    fmov.s  fr1, @r0
    add     #4, r0
    mov.l   .Lelevated_sun_depth, r1
    fmov.s  @r1, fr2
    fmov.s  fr2, @r0

    ! Select a contiguous 12-block sky packet for this weather region.
    mov.l   .Lelevated_grade_ptr, r0
    mov.l   @r0, r1
    shll2   r1
    mov.l   .Lelevated_sky_table, r0
    add     r1, r0
    mov.l   @r0, r2
    mov.l   .Lelevated_sky_selected_ptr, r0
    mov.l   r2, @r0
    rts
    nop

    .align 2
.Lelevated_phase_1:          .long 192
.Lelevated_phase_2:          .long 384
.Lelevated_phase_3:          .long 576
.Lelevated_grade_ptr:        .long elevated_grade
.Lelevated_travel_ptr:       .long elevated_travel_phase
.Lelevated_lateral_ptr:      .long elevated_lateral_phase
.Lelevated_ground_ptr:       .long elevated_camera_ground
.Lelevated_roll_ptr:         .long elevated_roll_sin
.Lelevated_sun_ptr:          .long elevated_sun_projected
.Lelevated_sky_selected_ptr: .long elevated_sky_selected
.Lelevated_sky_table:        .long elevated_sky_table
.Lelevated_amp1_table:       .long elevated_ridge_amplitude_1
.Lelevated_amp2_table:       .long elevated_ridge_amplitude_2
.Lelevated_clearance_table:  .long elevated_camera_clearance
.Lelevated_lateral_scale:    .long elevated_lateral_phase_scale
.Lelevated_bank_units:       .long elevated_bank_angle_units
.Lelevated_center_angle_1:   .long 16896  ! 12 * 1408
.Lelevated_center_angle_2:   .long -6144  ! -12 * 512
.Lelevated_sun_x_amp:        .long elevated_sun_x_amplitude
.Lelevated_sun_x_base:       .long elevated_sun_x_origin
.Lelevated_sun_y_amp:        .long elevated_sun_y_amplitude
.Lelevated_sun_y_base:       .long elevated_sun_y_origin
.Lelevated_sun_depth:        .long elevated_sun_reciprocal_z

! ---------------------------------------------------------------------------
! Build the 625-point projected terrain grid.
! ---------------------------------------------------------------------------

build_elevated_grid:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lelevated_grid_ptr, r8
    mov     #0, r10
.Lelevated_grid_row:
    mov     #0, r11
.Lelevated_grid_column:
    mov     r10, r4
    mov     r11, r5
    mov     r8, r6
    mov.l   .Lelevated_grid_point, r0
    jsr     @r0
    nop
    add     #12, r8
    add     #1, r11
    mov     #25, r0
    cmp/hs  r0, r11
    bf      .Lelevated_grid_column
    nop
    add     #1, r10
    mov     #25, r0
    cmp/hs  r0, r10
    bf      .Lelevated_grid_row
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
.Lelevated_grid_ptr:   .long elevated_grid_projected
.Lelevated_grid_point: .long build_elevated_grid_point

! r4=row, r5=column, r6=destination {screen X, screen Y, reciprocal Z}.
build_elevated_grid_point:
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10

    ! Local X and its valley-envelope square.
    mov     r9, r11
    add     #-12, r11
    lds     r11, fpul
    float   fpul, fr6
    mov.l   .Lelevated_point_x_step, r0
    fmov.s  @r0, fr7
    fmul    fr6, fr7              ! world/local X

    ! Ridge one: travel + lateral + row*384 + column*1408.
    mov.l   .Lelevated_point_travel, r0
    mov.l   @r0, r2
    mov.l   .Lelevated_point_lateral, r0
    mov.l   @r0, r1
    add     r1, r2
    mov     r8, r0
    shll8   r0
    mov     r8, r1
    shll8   r1
    shlr    r1
    add     r1, r0                ! row * 384
    add     r0, r2
    mov     r9, r0
    shll8   r0
    shll2   r0                    ! column * 1024
    mov     r9, r1
    shll8   r1                    ! column * 256
    add     r1, r0
    mov     r9, r1
    shll8   r1
    shlr    r1                    ! column * 128
    add     r1, r0
    add     r0, r2
    lds     r2, fpul
    fsca    fpul, dr0
    fabs    fr0
    fmov    fr0, fr4
    fmul    fr0, fr4
    fmul    fr0, fr4              ! sharp |sin|^3 crest

    ! Ridge two crosses the first: travel - lateral + row*704 - column*512.
    mov.l   .Lelevated_point_travel, r0
    mov.l   @r0, r2
    mov.l   .Lelevated_point_lateral, r0
    mov.l   @r0, r1
    sub     r1, r2
    mov     r8, r0
    shll8   r0
    shll    r0                    ! row * 512
    mov     r8, r1
    shll8   r1
    shlr    r1                    ! row * 128
    add     r1, r0
    mov     r8, r1
    shll2   r1
    shll2   r1
    shll2   r1                    ! row * 64
    add     r1, r0
    add     r0, r2
    mov     r9, r0
    shll8   r0
    shll    r0                    ! column * 512
    sub     r0, r2
    lds     r2, fpul
    fsca    fpul, dr2
    fabs    fr2
    fmul    fr2, fr2              ! broad |sin|^2 shoulder

    mov.l   .Lelevated_point_grade, r0
    mov.l   @r0, r3
    shll2   r3
    mov.l   .Lelevated_point_amp1, r0
    add     r3, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr4
    mov.l   .Lelevated_point_amp2, r0
    add     r3, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr2
    fadd    fr2, fr4

    ! Quadratic side walls turn the central route into a dramatic ravine.
    fmov    fr6, fr5
    fmul    fr6, fr5
    mov.l   .Lelevated_point_side, r0
    add     r3, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr5
    fadd    fr5, fr4

    ! Terrain-relative camera Y, then a slight fixed look-down pitch.
    mov.l   .Lelevated_point_ground, r0
    fmov.s  @r0, fr5
    fsub    fr5, fr4
    lds     r8, fpul
    float   fpul, fr9
    mov.l   .Lelevated_point_z_step, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr9
    mov.l   .Lelevated_point_z_near, r0
    fmov.s  @r0, fr5
    fadd    fr5, fr9
    mov.l   .Lelevated_point_pitch, r0
    add     r3, r0
    fmov.s  @r0, fr5
    fmul    fr9, fr5
    fadd    fr5, fr4              ! pitched camera-space Y

    ! Image-plane bank follows the curved flight path.
    mov.l   .Lelevated_point_roll, r0
    fmov.s  @r0, fr0              ! sin
    add     #4, r0
    fmov.s  @r0, fr1              ! cos
    fmov    fr7, fr2
    fmul    fr1, fr2
    fmov    fr4, fr3
    fmul    fr0, fr3
    fadd    fr3, fr2              ! banked X
    fmov    fr7, fr3
    fmul    fr0, fr3
    fneg    fr3
    fmov    fr4, fr5
    fmul    fr1, fr5
    fadd    fr5, fr3              ! banked Y

    mov.l   .Lelevated_point_one, r0
    fmov.s  @r0, fr10
    fdiv    fr9, fr10
    mov.l   .Lelevated_point_scale, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr2
    fmul    fr10, fr2
    mov.l   .Lelevated_point_cx, r0
    fmov.s  @r0, fr5
    fadd    fr5, fr2
    fmul    fr11, fr3
    fmul    fr10, fr3
    fneg    fr3
    mov.l   .Lelevated_point_cy, r0
    fmov.s  @r0, fr5
    fadd    fr5, fr3
    fmov.s  fr2, @r10
    add     #4, r10
    fmov.s  fr3, @r10
    add     #4, r10
    fmov.s  fr10, @r10

    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r9
    mov.l   @r15+, r8
    rts
    nop

    .align 2
.Lelevated_point_travel: .long elevated_travel_phase
.Lelevated_point_lateral:.long elevated_lateral_phase
.Lelevated_point_grade:  .long elevated_grade
.Lelevated_point_ground: .long elevated_camera_ground
.Lelevated_point_roll:   .long elevated_roll_sin
.Lelevated_point_amp1:   .long elevated_ridge_amplitude_1
.Lelevated_point_amp2:   .long elevated_ridge_amplitude_2
.Lelevated_point_side:   .long elevated_side_slope
.Lelevated_point_pitch:  .long elevated_pitch_slope
.Lelevated_point_x_step: .long elevated_grid_x_step
.Lelevated_point_z_step: .long elevated_grid_z_step
.Lelevated_point_z_near: .long elevated_grid_z_near
.Lelevated_point_one:    .long float_one
.Lelevated_point_scale:  .long elevated_projection_scale
.Lelevated_point_cx:     .long center_x
.Lelevated_point_cy:     .long elevated_center_y

! ---------------------------------------------------------------------------
! Cached terrain submission.
! ---------------------------------------------------------------------------

! r4=cache pointer, r6=vertex command, r7=ARGB.
elevated_emit_cached_vertex:
    fmov.s  @r4+, fr12
    fmov.s  @r4+, fr13
    fmov.s  @r4, fr14
    mov     r6, r4
    mov     r7, r5
    mov.l   .Lelevated_cached_emit, r0
    jmp     @r0
    nop

    .align 2
.Lelevated_cached_emit: .long emit_vertex

draw_elevated_terrain:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lelevated_terrain_cache, r8
    mov     #0, r10
.Lelevated_terrain_row:
    mov     r8, r12
    mov.l   .Lelevated_terrain_row_stride, r0
    add     r0, r12
    mov     #0, r11
.Lelevated_terrain_column:
    ! Cross-slope material class plus a distance dither. In snow this yields
    ! alternating ice/slate ribs; in the thaw it reads as rock/grass exposure.
    mov     r11, r0
    add     #-12, r0
    cmp/pz  r0
    bt      .Lelevated_material_abs_ready
    neg     r0, r0
.Lelevated_material_abs_ready:
    shlr2   r0
    mov     r10, r1
    shlr2   r1
    add     r1, r0
    mov     r10, r1
    add     r11, r1
    mov     #1, r2
    and     r2, r1
    add     r1, r0
    and     #3, r0
    shll2   r0
    mov.l   .Lelevated_terrain_grade, r1
    mov.l   @r1, r1
    shll2   r1
    shll2   r1                    ! grade * 16 bytes
    add     r1, r0
    mov.l   .Lelevated_terrain_palettes, r14
    add     r0, r14
    mov.l   @r14, r7

    mov     r8, r4
    mov.l   .Lelevated_terrain_normal, r6
    mov.l   .Lelevated_terrain_emit, r0
    jsr     @r0
    nop
    mov     r12, r4
    mov.l   .Lelevated_terrain_normal, r6
    mov     #24, r0
    cmp/eq  r0, r11
    bf      .Lelevated_terrain_second_normal
    mov.l   .Lelevated_terrain_eol, r6
.Lelevated_terrain_second_normal:
    mov.l   .Lelevated_terrain_emit, r0
    jsr     @r0
    nop
    add     #12, r8
    add     #12, r12
    add     #1, r11
    mov     #25, r0
    cmp/hs  r0, r11
    bf      .Lelevated_terrain_column
    nop
    add     #1, r10
    mov     #24, r0
    cmp/hs  r0, r10
    bf      .Lelevated_terrain_row
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
.Lelevated_terrain_cache:      .long elevated_grid_projected
.Lelevated_terrain_grade:      .long elevated_grade
.Lelevated_terrain_palettes:   .long elevated_terrain_palettes
.Lelevated_terrain_row_stride: .long 300
.Lelevated_terrain_emit:       .long elevated_emit_cached_vertex
.Lelevated_terrain_normal:     .long CMD_VERTEX
.Lelevated_terrain_eol:        .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Sky, clouds, lake, and near-frame vignette are prebuilt native strips.
! ---------------------------------------------------------------------------

draw_elevated_sky:
    sts.l   pr, @-r15
    mov.l   r10, @-r15
    mov.l   .Lelevated_sky_selected, r0
    mov.l   @r0, r4
    mov     #12, r10
.Lelevated_sky_submit_loop:
    mov.l   .Lelevated_sky_submit, r0
    jsr     @r0
    nop
    dt      r10
    bf      .Lelevated_sky_submit_loop
    nop
    mov.l   @r15+, r10
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lelevated_sky_selected:.long elevated_sky_selected
.Lelevated_sky_submit:  .long ta_submit

draw_elevated_lake:
    sts.l   pr, @-r15
    mov.l   r10, @-r15
    mov.l   .Lelevated_lake_grade, r0
    mov.l   @r0, r1
    mov     #3, r0
    cmp/eq  r0, r1
    bf      .Lelevated_lake_done
    mov.l   .Lelevated_lake_vertices, r4
    mov     #4, r10
.Lelevated_lake_loop:
    mov.l   .Lelevated_lake_submit, r0
    jsr     @r0
    nop
    dt      r10
    bf      .Lelevated_lake_loop
    nop
.Lelevated_lake_done:
    mov.l   @r15+, r10
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lelevated_lake_grade:   .long elevated_grade
.Lelevated_lake_vertices:.long elevated_lake_vertices
.Lelevated_lake_submit:  .long ta_submit

draw_elevated_vignette:
    sts.l   pr, @-r15
    mov.l   r10, @-r15
    mov.l   .Lelevated_vignette_vertices, r4
    mov     #16, r10
.Lelevated_vignette_loop:
    mov.l   .Lelevated_vignette_submit, r0
    jsr     @r0
    nop
    dt      r10
    bf      .Lelevated_vignette_loop
    nop
    mov.l   @r15+, r10
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lelevated_vignette_vertices:.long elevated_vignette_vertices
.Lelevated_vignette_submit:  .long ta_submit

! ---------------------------------------------------------------------------
! Additive beam primitive used by ridge glints and sun rays.
! r4/r5 = cached endpoints, r6 = width-float pointer, r7 = ARGB.
! ---------------------------------------------------------------------------

elevated_emit_beam:
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
    mov     r7, r11
    fmov.s  @r6, fr9
    fmov.s  @r8+, fr0
    fmov.s  @r8+, fr1
    fmov.s  @r8, fr2
    fmov.s  @r9+, fr3
    fmov.s  @r9+, fr4
    fmov.s  @r9, fr5
    ! The shared additive list uses strict GREATER depth testing. Nudge beams
    ! toward the lens so ridge glints survive equality with their source mesh
    ! instead of z-fighting or disappearing on exact cached endpoints.
    mov.l   .Lelevated_beam_z_bias, r0
    fmov.s  @r0, fr10
    fadd    fr10, fr2
    fadd    fr10, fr5
    fmov    fr3, fr6
    fsub    fr0, fr6
    fmov    fr4, fr7
    fsub    fr1, fr7
    fmov    fr6, fr8
    fmul    fr6, fr8
    fmov    fr7, fr10
    fmul    fr7, fr10
    fadd    fr10, fr8
    mov.l   .Lelevated_beam_epsilon, r0
    fmov.s  @r0, fr10
    fadd    fr10, fr8
    fsrra   fr8
    fmul    fr8, fr6
    fmul    fr9, fr6              ! screen normal Y
    fmul    fr8, fr7
    fmul    fr9, fr7
    fneg    fr7                   ! screen normal X

    fmov    fr0, fr12
    fadd    fr7, fr12
    fmov    fr1, fr13
    fadd    fr6, fr13
    fmov    fr2, fr14
    mov.l   .Lelevated_beam_normal, r4
    mov     r11, r5
    mov.l   .Lelevated_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr0, fr12
    fsub    fr7, fr12
    fmov    fr1, fr13
    fsub    fr6, fr13
    fmov    fr2, fr14
    mov.l   .Lelevated_beam_normal, r4
    mov     r11, r5
    mov.l   .Lelevated_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr3, fr12
    fadd    fr7, fr12
    fmov    fr4, fr13
    fadd    fr6, fr13
    fmov    fr5, fr14
    mov.l   .Lelevated_beam_normal, r4
    mov     r11, r5
    mov.l   .Lelevated_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr3, fr12
    fsub    fr7, fr12
    fmov    fr4, fr13
    fsub    fr6, fr13
    fmov    fr5, fr14
    mov.l   .Lelevated_beam_eol, r4
    mov     r11, r5
    mov.l   .Lelevated_beam_emit, r0
    jsr     @r0
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
.Lelevated_beam_epsilon:.long elevated_beam_epsilon
.Lelevated_beam_z_bias: .long elevated_beam_z_bias
.Lelevated_beam_emit:   .long emit_vertex
.Lelevated_beam_normal: .long CMD_VERTEX
.Lelevated_beam_eol:    .long CMD_VERTEX_EOL

draw_elevated_ridge_light:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lelevated_ridge_cache, r8
    mov.l   .Lelevated_ridge_offset_a, r0
    add     r0, r8                 ! row 6
    mov     #0, r12
.Lelevated_ridge_line:
    mov.l   .Lelevated_ridge_grade, r0
    mov.l   @r0, r1
    shll2   r1
    mov.l   .Lelevated_ridge_colors, r0
    add     r1, r0
    mov.l   @r0, r7
    mov     #24, r10
.Lelevated_ridge_segment:
    mov     r8, r4
    mov     r8, r5
    add     #12, r5
    mov.l   .Lelevated_ridge_width, r6
    mov.l   .Lelevated_ridge_beam, r0
    jsr     @r0
    nop
    add     #12, r8
    dt      r10
    bf      .Lelevated_ridge_segment
    nop
    add     #1, r12
    mov     #2, r0
    cmp/hs  r0, r12
    bt      .Lelevated_ridge_done
    mov.l   .Lelevated_ridge_cache, r8
    mov.l   .Lelevated_ridge_offset_b, r0
    add     r0, r8                 ! row 14
    bra     .Lelevated_ridge_line
    nop
.Lelevated_ridge_done:
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
.Lelevated_ridge_cache:   .long elevated_grid_projected
.Lelevated_ridge_grade:   .long elevated_grade
.Lelevated_ridge_colors:  .long elevated_ridge_colors
.Lelevated_ridge_width:   .long elevated_ridge_width
.Lelevated_ridge_beam:    .long elevated_emit_beam
.Lelevated_ridge_offset_a:.long 1800
.Lelevated_ridge_offset_b:.long 4200

! ---------------------------------------------------------------------------
! Screen-space additive discs. r4=center cache, r5=radius pointer,
! r6=color, r7=segment count (8 or 16).
! ---------------------------------------------------------------------------

draw_elevated_disc:
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
    mov     #0, r12
    mov     #16, r0
    cmp/eq  r0, r11
    bf      .Lelevated_disc_step_8
    mov.l   .Lelevated_disc_step16, r13
    bra     .Lelevated_disc_loop
    nop
.Lelevated_disc_step_8:
    mov.l   .Lelevated_disc_step8, r13
.Lelevated_disc_loop:
    ! Triangle center.
    fmov.s  @r8, fr12
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr13
    add     #4, r0
    fmov.s  @r0, fr14
    mov.l   .Lelevated_disc_normal, r4
    mov     r10, r5
    mov.l   .Lelevated_disc_emit, r0
    jsr     @r0
    nop

    ! First rim point.
    lds     r12, fpul
    fsca    fpul, dr0
    fmov.s  @r9, fr2
    fmul    fr2, fr0
    fmul    fr2, fr1
    fmov.s  @r8, fr12
    fadd    fr0, fr12
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr13
    fadd    fr1, fr13
    add     #4, r0
    fmov.s  @r0, fr14
    mov.l   .Lelevated_disc_normal, r4
    mov     r10, r5
    mov.l   .Lelevated_disc_emit, r0
    jsr     @r0
    nop

    ! Second rim point closes the independent triangle.
    mov     r12, r0
    add     r13, r0
    lds     r0, fpul
    fsca    fpul, dr0
    fmov.s  @r9, fr2
    fmul    fr2, fr0
    fmul    fr2, fr1
    fmov.s  @r8, fr12
    fadd    fr0, fr12
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr13
    fadd    fr1, fr13
    add     #4, r0
    fmov.s  @r0, fr14
    mov.l   .Lelevated_disc_eol, r4
    mov     r10, r5
    mov.l   .Lelevated_disc_emit, r0
    jsr     @r0
    nop
    add     r13, r12
    dt      r11
    bf      .Lelevated_disc_loop
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
.Lelevated_disc_step16:.long 4096
.Lelevated_disc_step8: .long 8192
.Lelevated_disc_emit:  .long emit_vertex
.Lelevated_disc_normal:.long CMD_VERTEX
.Lelevated_disc_eol:   .long CMD_VERTEX_EOL

draw_elevated_sun:
    sts.l   pr, @-r15
    mov.l   .Lelevated_sun_grade, r0
    mov.l   @r0, r1
    shll2   r1
    mov.l   .Lelevated_sun_halo_colors, r0
    add     r1, r0
    mov.l   @r0, r6
    mov.l   .Lelevated_sun_center, r4
    mov.l   .Lelevated_sun_halo_radius, r5
    mov     #16, r7
    mov.l   .Lelevated_sun_disc, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_sun_grade, r0
    mov.l   @r0, r1
    shll2   r1
    mov.l   .Lelevated_sun_core_colors, r0
    add     r1, r0
    mov.l   @r0, r6
    mov.l   .Lelevated_sun_center, r4
    mov.l   .Lelevated_sun_core_radius, r5
    mov     #16, r7
    mov.l   .Lelevated_sun_disc, r0
    jsr     @r0
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lelevated_sun_grade:       .long elevated_grade
.Lelevated_sun_center:      .long elevated_sun_projected
.Lelevated_sun_halo_colors: .long elevated_sun_halo_colors
.Lelevated_sun_core_colors: .long elevated_sun_core_colors
.Lelevated_sun_halo_radius: .long elevated_sun_halo_radius
.Lelevated_sun_core_radius: .long elevated_sun_core_radius
.Lelevated_sun_disc:        .long draw_elevated_disc

draw_elevated_sun_rays:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lelevated_ray_scratch, r8
    mov.l   .Lelevated_ray_center, r9
    mov     #0, r11
    mov     #8, r10
.Lelevated_ray_loop:
    ! Copy sun center to endpoint A.
    mov.l   @r9, r0
    mov.l   r0, @r8
    mov.l   @(4,r9), r0
    mov.l   r0, @(4,r8)
    mov.l   @(8,r9), r0
    mov.l   r0, @(8,r8)

    lds     r11, fpul
    fsca    fpul, dr0
    mov.l   .Lelevated_ray_length, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    fmul    fr2, fr1
    fmov.s  @r9, fr3
    fadd    fr0, fr3
    mov     r8, r12
    add     #12, r12
    fmov.s  fr3, @r12
    mov     r9, r0
    add     #4, r0
    fmov.s  @r0, fr3
    fadd    fr1, fr3
    mov     r12, r0
    add     #4, r0
    fmov.s  fr3, @r0
    mov     r9, r0
    add     #8, r0
    fmov.s  @r0, fr3
    mov     r12, r0
    add     #8, r0
    fmov.s  fr3, @r0

    mov.l   .Lelevated_ray_grade, r0
    mov.l   @r0, r1
    shll2   r1
    mov.l   .Lelevated_ray_colors, r0
    add     r1, r0
    mov.l   @r0, r7
    mov     r8, r4
    mov     r12, r5
    mov.l   .Lelevated_ray_width, r6
    mov.l   .Lelevated_ray_beam, r0
    jsr     @r0
    nop
    mov.l   .Lelevated_ray_step, r0
    add     r0, r11
    dt      r10
    bf      .Lelevated_ray_loop
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
.Lelevated_ray_scratch:.long elevated_beam_scratch
.Lelevated_ray_center: .long elevated_sun_projected
.Lelevated_ray_grade:  .long elevated_grade
.Lelevated_ray_colors: .long elevated_ray_colors
.Lelevated_ray_length: .long elevated_ray_length
.Lelevated_ray_width:  .long elevated_ray_width
.Lelevated_ray_beam:   .long elevated_emit_beam
.Lelevated_ray_step:   .long 8192

draw_elevated_lens_ghosts:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lelevated_lens_factors, r8
    mov.l   .Lelevated_lens_radii, r9
    mov     #0, r10
.Lelevated_lens_loop:
    mov.l   .Lelevated_lens_sun, r0
    fmov.s  @r0, fr0
    mov.l   .Lelevated_lens_cx, r1
    fmov.s  @r1, fr1
    fsub    fr0, fr1              ! center - sun
    fmov.s  @r8, fr2
    fmul    fr2, fr1
    fadd    fr0, fr1
    mov.l   .Lelevated_lens_center, r11
    fmov.s  fr1, @r11

    add     #4, r0
    fmov.s  @r0, fr0
    mov.l   .Lelevated_lens_cy, r1
    fmov.s  @r1, fr1
    fsub    fr0, fr1
    fmul    fr2, fr1
    fadd    fr0, fr1
    mov     r11, r0
    add     #4, r0
    fmov.s  fr1, @r0
    add     #4, r0
    mov.l   .Lelevated_lens_depth, r1
    fmov.s  @r1, fr1
    fmov.s  fr1, @r0

    mov.l   .Lelevated_lens_grade, r0
    mov.l   @r0, r1
    mov     r1, r2
    shll2   r1
    shll2   r1                    ! grade * 16 bytes (four entries padded)
    mov     r10, r2
    shll2   r2
    add     r2, r1
    mov.l   .Lelevated_lens_colors, r0
    add     r1, r0
    mov.l   @r0, r6
    mov     r11, r4
    mov     r9, r5
    mov     #8, r7
    mov.l   .Lelevated_lens_disc, r0
    jsr     @r0
    nop
    add     #4, r8
    add     #4, r9
    add     #1, r10
    mov     #3, r0
    cmp/hs  r0, r10
    bf      .Lelevated_lens_loop
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
.Lelevated_lens_sun:    .long elevated_sun_projected
.Lelevated_lens_center: .long elevated_flare_projected
.Lelevated_lens_grade:  .long elevated_grade
.Lelevated_lens_factors:.long elevated_lens_factors
.Lelevated_lens_radii:  .long elevated_lens_radii
.Lelevated_lens_colors: .long elevated_lens_colors
.Lelevated_lens_cx:     .long center_x
.Lelevated_lens_cy:     .long elevated_screen_center_y
.Lelevated_lens_depth:  .long elevated_lens_depth
.Lelevated_lens_disc:   .long draw_elevated_disc

! ---------------------------------------------------------------------------
! Read-only landscape / lighting data.
! ---------------------------------------------------------------------------

    .section .rodata.elevated,"a"
    .align 5

elevated_sky_table:
    .long elevated_sky_ravine
    .long elevated_sky_snow
    .long elevated_sky_gold
    .long elevated_sky_thaw

! Each packet is three native four-vertex strips: sky, distant cloud shelf,
! and horizon haze. Reciprocal depths keep the sky behind every terrain point.
elevated_sky_ravine:
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3a83126f,0,0,0xff8aa5b4,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3a83126f,0,0,0xff173451,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3a83126f,0,0,0xffb9c7c4,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3a83126f,0,0,0xff284c68,0
    .long CMD_VERTEX,     0x00000000,0x43660000,0x3b03126f,0,0,0xff6f8998,0
    .long CMD_VERTEX,     0x00000000,0x43200000,0x3b03126f,0,0,0xff9aabb0,0
    .long CMD_VERTEX,     0x44200000,0x43660000,0x3b03126f,0,0,0xff899da4,0
    .long CMD_VERTEX_EOL, 0x44200000,0x43200000,0x3b03126f,0,0,0xff647f91,0
    .long CMD_VERTEX,     0x00000000,0x43960000,0x3b449ba6,0,0,0xffb3c1c0,0
    .long CMD_VERTEX,     0x00000000,0x43660000,0x3b449ba6,0,0,0xffd3d8d0,0
    .long CMD_VERTEX,     0x44200000,0x43960000,0x3b449ba6,0,0,0xffc3cbc6,0
    .long CMD_VERTEX_EOL, 0x44200000,0x43660000,0x3b449ba6,0,0,0xffabb9b7,0

elevated_sky_snow:
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3a83126f,0,0,0xffdce5e7,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3a83126f,0,0,0xff3f6688,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3a83126f,0,0,0xffeef0ec,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3a83126f,0,0,0xff688aa2,0
    .long CMD_VERTEX,     0x00000000,0x435c0000,0x3b03126f,0,0,0xffb8c8d1,0
    .long CMD_VERTEX,     0x00000000,0x43160000,0x3b03126f,0,0,0xffe7eceb,0
    .long CMD_VERTEX,     0x44200000,0x435c0000,0x3b03126f,0,0,0xffd3dcde,0
    .long CMD_VERTEX_EOL, 0x44200000,0x43160000,0x3b03126f,0,0,0xff9db5c5,0
    .long CMD_VERTEX,     0x00000000,0x439b0000,0x3b449ba6,0,0,0xffdfe5e2,0
    .long CMD_VERTEX,     0x00000000,0x435c0000,0x3b449ba6,0,0,0xfff4f2e8,0
    .long CMD_VERTEX,     0x44200000,0x439b0000,0x3b449ba6,0,0,0xffe9e9e2,0
    .long CMD_VERTEX_EOL, 0x44200000,0x435c0000,0x3b449ba6,0,0,0xffd4dcda,0

elevated_sky_gold:
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3a83126f,0,0,0xffd1ad65,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3a83126f,0,0,0xff24445e,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3a83126f,0,0,0xfff0c979,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3a83126f,0,0,0xff49657a,0
    .long CMD_VERTEX,     0x00000000,0x43660000,0x3b03126f,0,0,0xffd2ad69,0
    .long CMD_VERTEX,     0x00000000,0x43160000,0x3b03126f,0,0,0xffffe0a1,0
    .long CMD_VERTEX,     0x44200000,0x43660000,0x3b03126f,0,0,0xffe1bd75,0
    .long CMD_VERTEX_EOL, 0x44200000,0x43160000,0x3b03126f,0,0,0xffe9c985,0
    .long CMD_VERTEX,     0x00000000,0x43a00000,0x3b449ba6,0,0,0xffdfc280,0
    .long CMD_VERTEX,     0x00000000,0x43660000,0x3b449ba6,0,0,0xffffe5a8,0
    .long CMD_VERTEX,     0x44200000,0x43a00000,0x3b449ba6,0,0,0xffebcf92,0
    .long CMD_VERTEX_EOL, 0x44200000,0x43660000,0x3b449ba6,0,0,0xffffdc91,0

elevated_sky_thaw:
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3a83126f,0,0,0xff9cb49f,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3a83126f,0,0,0xff31596d,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3a83126f,0,0,0xffc6c8a5,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3a83126f,0,0,0xff54768a,0
    .long CMD_VERTEX,     0x00000000,0x43610000,0x3b03126f,0,0,0xff98ada8,0
    .long CMD_VERTEX,     0x00000000,0x431b0000,0x3b03126f,0,0,0xffc4d1c5,0
    .long CMD_VERTEX,     0x44200000,0x43610000,0x3b03126f,0,0,0xffafbeb1,0
    .long CMD_VERTEX_EOL, 0x44200000,0x431b0000,0x3b03126f,0,0,0xff879f9e,0
    .long CMD_VERTEX,     0x00000000,0x43960000,0x3b449ba6,0,0,0xffb7c5b4,0
    .long CMD_VERTEX,     0x00000000,0x43610000,0x3b449ba6,0,0,0xffd8d5b8,0
    .long CMD_VERTEX,     0x44200000,0x43960000,0x3b449ba6,0,0,0xffc4cbb9,0
    .long CMD_VERTEX_EOL, 0x44200000,0x43610000,0x3b449ba6,0,0,0xffaebeb1,0

elevated_terrain_palettes:
    .long 0xff182d3b,0xff294454,0xff3e5c69,0xff6f8183 ! blue ravine
    .long 0xff43515c,0xff71808a,0xffaeb9bd,0xffe4e6df ! snow / slate
    .long 0xff5b4931,0xff7b6646,0xffa48a5a,0xffd5bd7d ! gold summit
    .long 0xff304335,0xff4d6347,0xff756d4d,0xff9b9470 ! thawed rock / grass

elevated_ridge_colors:
    .long 0xff244b64,0xff698aa0,0xffb48b45,0xff537960
elevated_sun_halo_colors:
    .long 0xff30485c,0xff546674,0xff705a30,0xff5b6441
elevated_sun_core_colors:
    .long 0xff94b3c0,0xffc9d6d6,0xffffd078,0xffe6d68e
elevated_ray_colors:
    .long 0xff182b3b,0xff344553,0xff443315,0xff303721
! Four colors per phase; the unused fourth entry keeps phase stride at 16.
elevated_lens_colors:
    .long 0xff203c50,0xff142b38,0xff31516a,0
    .long 0xff455c6b,0xff263944,0xff70828b,0
    .long 0xff58401e,0xff302514,0xff866637,0
    .long 0xff3a543d,0xff233529,0xff61785a,0

elevated_ridge_amplitude_1:
    .long 0x400ccccd,0x40333333,0x3fe66666,0x40000000 ! 2.2,2.8,1.8,2.0
elevated_ridge_amplitude_2:
    .long 0x3f4ccccd,0x3f99999a,0x3f19999a,0x3f666666 ! .8,1.2,.6,.9
elevated_side_slope:
    .long 0x3d6147ae,0x3d23d70a,0x3c23d70a,0x3c83126f ! .055,.04,.010,.016
elevated_pitch_slope:
    .long 0x3d8f5c29,0x3d75c28f,0x3da3d70a,0x3d99999a ! .07,.06,.08,.075
elevated_camera_clearance:
    .long 0x3f933333,0x3fa00000,0x40000000,0x3fcccccd ! 1.15,1.25,2.00,1.60

elevated_grid_x_step:          .long 0x3f8ccccd ! 1.10
elevated_grid_z_step:          .long 0x3f933333 ! 1.15
elevated_grid_z_near:          .long 0x3fe66666 ! 1.80
elevated_projection_scale:     .long 0x43960000 ! 300
elevated_center_y:             .long 0x435a0000 ! 218
elevated_screen_center_y:      .long 0x43700000 ! 240
elevated_lateral_phase_scale:  .long 0x45000000 ! 2048 angle units
elevated_bank_angle_units:     .long 0x44200000 ! 640 angle units / 5.625 deg
elevated_sun_x_amplitude:      .long 0x420c0000 ! 35
elevated_sun_x_origin:         .long 0x44020000 ! 520
elevated_sun_y_amplitude:      .long 0x41900000 ! 18
elevated_sun_y_origin:         .long 0x42b80000 ! 92
elevated_sun_reciprocal_z:     .long 0x3b449ba6 ! .003
elevated_sun_halo_radius:      .long 0x42280000 ! 42
elevated_sun_core_radius:      .long 0x41800000 ! 16
elevated_beam_epsilon:         .long 0x3e800000 ! .25 px^2
elevated_beam_z_bias:          .long 0x38d1b717 ! .0001 reciprocal-Z
elevated_ridge_width:          .long 0x3f333333 ! .70 px
elevated_ray_width:            .long 0x3fa66666 ! 1.30 px
elevated_ray_length:           .long 0x42c80000 ! 100 px
elevated_lens_factors:
    .long 0x3ee66666,0x3f666666,0x3faccccd ! .45,.90,1.35
elevated_lens_radii:
    .long 0x41100000,0x40a00000,0x41400000 ! 9,5,12 px
elevated_lens_depth:           .long 0x3f4ccccd ! .80 / foreground ghosts

! A final-region lake wedge. Varying reciprocal Z anchors it into the valley.
elevated_lake_vertices:
    .long CMD_VERTEX,     0x43900000,0x43e60000,0x3e19999a,0,0,0xff173e4a,0
    .long CMD_VERTEX,     0x439a0000,0x43890000,0x3d23d70a,0,0,0xff668f94,0
    .long CMD_VERTEX,     0x43fa0000,0x43e60000,0x3e19999a,0,0,0xff294f52,0
    .long CMD_VERTEX_EOL, 0x43aa0000,0x43890000,0x3d23d70a,0,0,0xffa6b7aa,0

! Four near-depth black-blue strips create a subtle optical vignette.
elevated_vignette_vertices:
    .long CMD_VERTEX,     0x00000000,0x41900000,0x3f400000,0,0,0xff071019,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3f400000,0,0,0xff010306,0
    .long CMD_VERTEX,     0x44200000,0x41900000,0x3f400000,0,0,0xff071019,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3f400000,0,0,0xff010306,0
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3f400000,0,0,0xff010306,0
    .long CMD_VERTEX,     0x00000000,0x43e30000,0x3f400000,0,0,0xff071019,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3f400000,0,0,0xff010306,0
    .long CMD_VERTEX_EOL, 0x44200000,0x43e30000,0x3f400000,0,0,0xff071019,0
    .long CMD_VERTEX,     0x41400000,0x43f00000,0x3f400000,0,0,0xff071019,0
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3f400000,0,0,0xff010306,0
    .long CMD_VERTEX,     0x41400000,0x00000000,0x3f400000,0,0,0xff071019,0
    .long CMD_VERTEX_EOL, 0x00000000,0x00000000,0x3f400000,0,0,0xff010306,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3f400000,0,0,0xff010306,0
    .long CMD_VERTEX,     0x441d0000,0x43f00000,0x3f400000,0,0,0xff071019,0
    .long CMD_VERTEX,     0x44200000,0x00000000,0x3f400000,0,0,0xff010306,0
    .long CMD_VERTEX_EOL, 0x441d0000,0x00000000,0x3f400000,0,0,0xff071019,0

! ---------------------------------------------------------------------------
! Writable state. The grid comes first, keeping the exact section payload at
! 7,500 + 76 = 7,576 bytes without alignment padding between cache and state.
! ---------------------------------------------------------------------------

    .section .bss.elevated,"aw",@nobits
    .align 5
elevated_grid_projected: .space 25 * 25 * 12
elevated_grade:          .space 4
elevated_travel_phase:   .space 4
elevated_lateral_phase:  .space 4
elevated_camera_ground:  .space 4
elevated_roll_sin:       .space 4
elevated_roll_cos:       .space 4
elevated_sky_selected:   .space 4
elevated_sun_projected:  .space 12
elevated_flare_projected:.space 12
elevated_beam_scratch:   .space 24

    .section .text,"ax"
