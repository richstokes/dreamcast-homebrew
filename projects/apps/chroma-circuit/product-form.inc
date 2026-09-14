! ---------------------------------------------------------------------------
! CHROMA CIRCUIT -- Scene 06: MACHINE DREAM
!
! An original Dreamcast-native salute to the procedural, industrial visual
! language that made Farbrausch's 2000 fr-08: .the .product so startling:
! warm machine spaces, nested cages, tiny repeated detail, severe letterbox
! framing, and sudden white-metal illumination. No source data, geometry,
! textures, typography, or music from that production is used here.
!
! One analytic assembly changes character four times during its 768 frames:
!     0..191  a perforated gold seed turns inside three dark gimbals
!   192..383  nine cage gates accelerate toward the camera
!   384..575  the cage becomes a high-key steel funnel and spot-beam burst
!   576..767  an amber mesh orb rises over a stark machine-tower silhouette
!
! Portal and gimbal points are projected once per frame into 5.23 KiB of
! cache/state.
! Both the opaque metal and additive edge passes consume that same cache.
! The exact worst-case geometry budget, excluding the shared title, is
! 924 triangles / 1,880 native vertex blocks -- deliberately conservative for
! the 60 Hz TA/PVR pipeline and much lighter than STRANGE FORM.
! ---------------------------------------------------------------------------

    .section .text,"ax"
    .align 2

! r9 is the private scene frame, 0..767. Preserve it for the common transition.
draw_scene_product_form:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lproduct_scene_prepare, r0
    jsr     @r0
    nop

    ! Opaque machinery. The tower is dormant until the amber coda; all other
    ! pieces stay resident so the phase cuts feel like lighting changes inside
    ! one impossible machine rather than unrelated scene swaps.
    mov.l   .Lproduct_scene_opaque, r4
    mov.l   .Lproduct_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_backdrop, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_tower, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_rails, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_portals, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_core, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_gimbals, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_apertures, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_masks, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_title, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_zero, r4
    mov.l   .Lproduct_scene_submit, r0
    jsr     @r0
    nop

    ! ONE/ONE edges make the generated structure read as brushed highlights.
    ! The white-steel phase increases widths and adds sixteen converging rays,
    ! but topology and peak TA load remain fixed and deterministic.
    mov.l   .Lproduct_scene_additive, r4
    mov.l   .Lproduct_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_portal_glow, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_gimbal_glow, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_light_rays, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_scene_flare, r0
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
.Lproduct_scene_prepare:     .long prepare_product_form
.Lproduct_scene_backdrop:    .long draw_product_backdrop
.Lproduct_scene_tower:       .long draw_product_tower
.Lproduct_scene_rails:       .long draw_product_rails
.Lproduct_scene_portals:     .long draw_product_portal_panels
.Lproduct_scene_core:        .long draw_product_core
.Lproduct_scene_gimbals:     .long draw_product_gimbal_panels
.Lproduct_scene_apertures:   .long draw_product_apertures
.Lproduct_scene_masks:       .long draw_product_masks
.Lproduct_scene_portal_glow: .long draw_product_portal_glow
.Lproduct_scene_gimbal_glow: .long draw_product_gimbal_glow
.Lproduct_scene_light_rays:  .long draw_product_light_rays
.Lproduct_scene_flare:       .long draw_product_flare
.Lproduct_scene_submit:      .long ta_submit
.Lproduct_scene_title:       .long draw_title
.Lproduct_scene_opaque:      .long opaque_header
.Lproduct_scene_additive:    .long additive_header
.Lproduct_scene_zero:        .long zero_block

! ---------------------------------------------------------------------------
! Phase / motion preparation.
!
! Travel is continuous at all three cuts: 2, 6, 12, then 4 fixed-point units
! per frame. A small sinusoidal image-bank is stored as sin/cos so all cached
! points share exactly one camera attitude.
! ---------------------------------------------------------------------------

prepare_product_form:
    sts.l   pr, @-r15
    mov.l   r8, @-r15

    mov     r9, r2
    mov     #0, r3
    mov.l   .Lproduct_phase_1, r0
    cmp/hs  r0, r2
    bf      .Lproduct_phase_selected
    mov     #1, r3
    mov.l   .Lproduct_phase_2, r0
    cmp/hs  r0, r2
    bf      .Lproduct_phase_selected
    mov     #2, r3
    mov.l   .Lproduct_phase_3, r0
    cmp/hs  r0, r2
    bf      .Lproduct_phase_selected
    mov     #3, r3
.Lproduct_phase_selected:
    mov.l   .Lproduct_grade_ptr, r0
    mov.l   r3, @r0

    ! Piecewise travel curve with continuous endpoints.
    tst     r3, r3
    bf      .Lproduct_travel_later
    mov     r9, r2
    shll    r2
    bra     .Lproduct_travel_ready
    nop
.Lproduct_travel_later:
    mov     #1, r0
    cmp/eq  r0, r3
    bf      .Lproduct_travel_white
    mov     r9, r2
    mov.l   .Lproduct_phase_1, r0
    sub     r0, r2
    mov     r2, r1
    shll2   r2
    shll    r1
    add     r1, r2                ! (frame-192) * 6
    mov.l   .Lproduct_travel_1, r0
    add     r0, r2
    bra     .Lproduct_travel_ready
    nop
.Lproduct_travel_white:
    mov     #2, r0
    cmp/eq  r0, r3
    bf      .Lproduct_travel_amber
    mov     r9, r2
    mov.l   .Lproduct_phase_2, r0
    sub     r0, r2
    mov     r2, r1
    shll2   r2
    shll2   r1
    shll    r1
    add     r1, r2                ! (frame-384) * 12
    mov.l   .Lproduct_travel_2, r0
    add     r0, r2
    bra     .Lproduct_travel_ready
    nop
.Lproduct_travel_amber:
    mov     r9, r2
    mov.l   .Lproduct_phase_3, r0
    sub     r0, r2
    shll2   r2
    mov.l   .Lproduct_travel_3, r0
    add     r0, r2
.Lproduct_travel_ready:
    mov.l   .Lproduct_travel_ptr, r0
    mov.l   r2, @r0

    ! Bank = sin(frame*64) * phase amplitude, converted back to binary angle.
    mov     r9, r0
    shll8   r0
    shlr2   r0                    ! frame * 64
    lds     r0, fpul
    fsca    fpul, dr0
    mov     r3, r0
    shll2   r0
    mov.l   .Lproduct_bank_table, r1
    add     r0, r1
    fmov.s  @r1, fr2
    fmul    fr2, fr0
    ftrc    fr0, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr2
    mov.l   .Lproduct_roll_ptr, r0
    fmov.s  fr2, @r0
    add     #4, r0
    fmov.s  fr3, @r0

    ! Select the native-block backdrop without copying it.
    mov     r3, r0
    shll2   r0
    mov.l   .Lproduct_backdrop_table, r1
    add     r0, r1
    mov.l   @r1, r2
    mov.l   .Lproduct_backdrop_ptr, r0
    mov.l   r2, @r0

    mov.l   .Lproduct_prepare_portals, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_prepare_rails, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_prepare_gimbals, r0
    jsr     @r0
    nop

    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lproduct_phase_1:          .long 192
.Lproduct_phase_2:          .long 384
.Lproduct_phase_3:          .long 576
.Lproduct_travel_1:         .long 384
.Lproduct_travel_2:         .long 1536
.Lproduct_travel_3:         .long 3840
.Lproduct_grade_ptr:        .long product_grade
.Lproduct_travel_ptr:       .long product_travel
.Lproduct_roll_ptr:         .long product_roll_sin
.Lproduct_bank_table:       .long product_bank_units
.Lproduct_backdrop_table:   .long product_backdrop_table
.Lproduct_backdrop_ptr:     .long product_backdrop_selected
.Lproduct_prepare_portals:  .long build_product_portal_cache
.Lproduct_prepare_rails:    .long build_product_rail_cache
.Lproduct_prepare_gimbals:  .long build_product_gimbal_cache

! ---------------------------------------------------------------------------
! Nine moving annular gates. Each cache point stores outer and inner projected
! XYZ triples (24 bytes); the seventeenth point duplicates the circular seam.
! ---------------------------------------------------------------------------

build_product_portal_cache:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lproduct_portal_cache, r8
    mov.l   .Lproduct_portal_travel, r0
    mov.l   @r0, r14
    mov     #0, r10
.Lproduct_portal_ring_loop:
    ! depth = 1.35 + ((ring*112 - travel) & 1023) * (18/1024)
    mov     r10, r0
    mov     #112, r1
    mulu.w  r1, r0
    sts     macl, r11
    sub     r14, r11
    mov.l   .Lproduct_portal_phase_mask, r0
    and     r0, r11
    lds     r11, fpul
    float   fpul, fr9
    mov.l   .Lproduct_portal_depth_step, r0
    fmov.s  @r0, fr10
    fmul    fr10, fr9
    mov.l   .Lproduct_portal_depth_near, r0
    fmov.s  @r0, fr10
    fadd    fr10, fr9
    mov.l   .Lproduct_work_depth, r0
    fmov.s  fr9, @r0

    ! Each depth slice follows a shallow sinusoidal bend through the machine.
    mov     r9, r0
    shll8   r0
    shlr2   r0                    ! frame * 64
    mov     r10, r1
    shll8   r1
    shll2   r1                    ! ring * 1024
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lproduct_portal_grade, r0
    mov.l   @r0, r1
    shll2   r1
    mov.l   .Lproduct_portal_bend_table, r0
    add     r1, r0
    fmov.s  @r0, fr2
    fmov    fr0, fr3
    fmul    fr2, fr3
    mov.l   .Lproduct_work_center_x, r0
    fmov.s  fr3, @r0
    fmov    fr1, fr3
    fmul    fr2, fr3
    mov.l   .Lproduct_portal_bend_y, r0
    fmov.s  @r0, fr4
    fmul    fr4, fr3
    mov.l   .Lproduct_work_center_y, r0
    fmov.s  fr3, @r0

    mov.l   .Lproduct_portal_grade, r0
    mov.l   @r0, r1
    shll2   r1
    mov.l   .Lproduct_portal_outer_table, r0
    add     r1, r0
    fmov.s  @r0, fr10
    mov.l   .Lproduct_work_outer, r0
    fmov.s  fr10, @r0
    mov.l   .Lproduct_portal_inner_table, r0
    add     r1, r0
    fmov.s  @r0, fr10
    mov.l   .Lproduct_work_inner, r0
    fmov.s  fr10, @r0

    ! Independent ring rotation includes a small depth-derived torsion.
    mov     r9, r12
    shll2   r12
    shll2   r12                   ! frame * 16
    mov     r9, r0
    shll2   r0
    shll2   r0
    shll    r0                    ! frame * 32
    add     r0, r12               ! frame * 48
    mov     r10, r0
    shll8   r0
    shll2   r0
    shll    r0                    ! ring * 2048
    add     r0, r12
    mov     r11, r0
    shll2   r0
    shll2   r0                    ! phase * 16
    add     r0, r12

    mov     #17, r13
.Lproduct_portal_point_loop:
    mov     r12, r0
    lds     r0, fpul
    fsca    fpul, dr0             ! fr0=sin, fr1=cos

    mov.l   .Lproduct_work_outer, r0
    fmov.s  @r0, fr2
    fmov    fr1, fr7
    fmul    fr2, fr7
    mov.l   .Lproduct_work_center_x, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr7
    fmov    fr0, fr8
    fmul    fr2, fr8
    mov.l   .Lproduct_work_center_y, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr8
    mov.l   .Lproduct_work_depth, r0
    fmov.s  @r0, fr9
    mov     r8, r6
    mov.l   .Lproduct_portal_project, r0
    jsr     @r0
    nop

    mov     r12, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lproduct_work_inner, r0
    fmov.s  @r0, fr2
    fmov    fr1, fr7
    fmul    fr2, fr7
    mov.l   .Lproduct_work_center_x, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr7
    fmov    fr0, fr8
    fmul    fr2, fr8
    mov.l   .Lproduct_work_center_y, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr8
    mov.l   .Lproduct_work_depth, r0
    fmov.s  @r0, fr9
    mov     r8, r6
    add     #12, r6
    mov.l   .Lproduct_portal_project, r0
    jsr     @r0
    nop

    add     #24, r8
    mov.l   .Lproduct_portal_angle_step, r0
    add     r0, r12
    dt      r13
    bf      .Lproduct_portal_point_loop
    nop

    add     #1, r10
    mov     #9, r0
    cmp/hs  r0, r10
    bf      .Lproduct_portal_ring_loop
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
.Lproduct_portal_cache:       .long product_portal_projected
.Lproduct_portal_travel:      .long product_travel
.Lproduct_portal_grade:       .long product_grade
.Lproduct_portal_phase_mask:  .long 1023
.Lproduct_portal_depth_step:  .long product_depth_step
.Lproduct_portal_depth_near:  .long product_depth_near
.Lproduct_portal_bend_table:  .long product_bend_amplitudes
.Lproduct_portal_bend_y:      .long product_bend_y_scale
.Lproduct_portal_outer_table: .long product_outer_radii
.Lproduct_portal_inner_table: .long product_inner_radii
.Lproduct_portal_angle_step:  .long 4096
.Lproduct_portal_project:     .long product_project_to_cache
.Lproduct_work_center_x:      .long product_work_center_x
.Lproduct_work_center_y:      .long product_work_center_y
.Lproduct_work_depth:         .long product_work_depth
.Lproduct_work_outer:         .long product_work_outer
.Lproduct_work_inner:         .long product_work_inner

! ---------------------------------------------------------------------------
! Sixteen long helical cage ribs, projected only at their endpoints. Wide dark
! beams belong to the opaque list; a one-pixel hot wire reuses the same cache.
! ---------------------------------------------------------------------------

build_product_rail_cache:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15

    mov.l   .Lproduct_rail_cache, r8
    mov     #0, r10
    mov     r9, r11
    shll2   r11
    shll2   r11                   ! frame * 16
    mov     r9, r0
    shll2   r0
    shll    r0                    ! frame * 8
    add     r0, r11               ! frame * 24
.Lproduct_rail_build_loop:
    mov     r10, r0
    shll8   r0
    shll2   r0
    shll2   r0                    ! rail * 4096
    add     r11, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lproduct_rail_radius, r0
    fmov.s  @r0, fr2
    fmov    fr1, fr7
    fmul    fr2, fr7
    fmov    fr0, fr8
    fmul    fr2, fr8
    mov.l   .Lproduct_rail_near_z, r0
    fmov.s  @r0, fr9
    mov     r8, r6
    mov.l   .Lproduct_rail_project, r0
    jsr     @r0
    nop

    mov     r10, r0
    shll8   r0
    shll2   r0
    shll2   r0
    add     r11, r0
    mov.l   .Lproduct_rail_twist, r1
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lproduct_rail_radius, r0
    fmov.s  @r0, fr2
    fmov    fr1, fr7
    fmul    fr2, fr7
    fmov    fr0, fr8
    fmul    fr2, fr8
    mov.l   .Lproduct_rail_far_z, r0
    fmov.s  @r0, fr9
    mov     r8, r6
    add     #12, r6
    mov.l   .Lproduct_rail_project, r0
    jsr     @r0
    nop

    add     #24, r8
    add     #1, r10
    mov     #16, r0
    cmp/hs  r0, r10
    bf      .Lproduct_rail_build_loop
    nop

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
.Lproduct_rail_cache:  .long product_rail_projected
.Lproduct_rail_radius: .long product_rail_radius
.Lproduct_rail_near_z: .long product_rail_near_z
.Lproduct_rail_far_z:  .long product_rail_far_z
.Lproduct_rail_twist:  .long 8192
.Lproduct_rail_project:.long product_project_to_cache

! ---------------------------------------------------------------------------
! Three planar gimbals around the product seed. Two are crushed into ellipses;
! their changing relative phase creates the mesh-orb / orbital-ring illusion
! without a stored mesh or any texture lookup.
! ---------------------------------------------------------------------------

build_product_gimbal_cache:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lproduct_gimbal_cache, r8
    mov.l   .Lproduct_gimbal_grade, r0
    mov.l   @r0, r14
    mov     r14, r0
    shll2   r0
    mov.l   .Lproduct_gimbal_depth_table, r1
    add     r0, r1
    fmov.s  @r1, fr9
    mov.l   .Lproduct_core_depth_ptr, r0
    fmov.s  fr9, @r0
    mov.l   .Lproduct_gimbal_radius_table, r1
    mov     r14, r0
    shll2   r0
    add     r0, r1
    fmov.s  @r1, fr10
    mov.l   .Lproduct_core_radius_world, r0
    fmov.s  fr10, @r0

    ! Amber coda lifts the generated orb above its tower.
    mov.l   .Lproduct_gimbal_zero, r0
    fmov.s  @r0, fr8
    mov     #3, r0
    cmp/eq  r0, r14
    bf      .Lproduct_gimbal_center_ready
    mov.l   .Lproduct_gimbal_coda_y, r0
    fmov.s  @r0, fr8
.Lproduct_gimbal_center_ready:
    mov.l   .Lproduct_core_center_y, r0
    fmov.s  fr8, @r0

    ! Cache the screen-space seed center and its projected radius.
    mov.l   .Lproduct_gimbal_zero, r0
    fmov.s  @r0, fr7
    mov.l   .Lproduct_core_depth_ptr, r0
    fmov.s  @r0, fr9
    mov.l   .Lproduct_core_center, r6
    mov.l   .Lproduct_gimbal_project, r0
    jsr     @r0
    nop
    mov.l   .Lproduct_core_radius_world, r0
    fmov.s  @r0, fr0
    mov.l   .Lproduct_gimbal_scale, r0
    fmov.s  @r0, fr1
    fmul    fr1, fr0
    mov.l   .Lproduct_core_center, r0
    add     #8, r0
    fmov.s  @r0, fr1
    fmul    fr1, fr0
    mov.l   .Lproduct_core_radius_pixels, r0
    fmov.s  fr0, @r0

    mov     #0, r10
.Lproduct_gimbal_plane_loop:
    mov     r10, r0
    shll2   r0
    mov.l   .Lproduct_gimbal_velocity_table, r1
    add     r0, r1
    mov.l   @r1, r1
    mov     r9, r0
    muls.w  r1, r0
    sts     macl, r12
    mov     r10, r0
    shll8   r0
    shll2   r0
    shll2   r0
    shll    r0                    ! plane * 8192
    add     r0, r12
    mov     #17, r13
.Lproduct_gimbal_point_loop:
    mov     r12, r0
    lds     r0, fpul
    fsca    fpul, dr0

    mov.l   .Lproduct_core_radius_world, r0
    fmov.s  @r0, fr2
    mov     r10, r0
    shll2   r0
    mov.l   .Lproduct_gimbal_xscale_table, r1
    add     r0, r1
    fmov.s  @r1, fr3
    mov.l   .Lproduct_gimbal_yscale_table, r1
    add     r0, r1
    fmov.s  @r1, fr4
    fmov    fr1, fr7
    fmul    fr2, fr7
    fmul    fr3, fr7
    fmov    fr0, fr8
    fmul    fr2, fr8
    fmul    fr4, fr8
    mov.l   .Lproduct_core_center_y, r0
    fmov.s  @r0, fr5
    fadd    fr5, fr8
    mov.l   .Lproduct_core_depth_ptr, r0
    fmov.s  @r0, fr9
    mov     r8, r6
    mov.l   .Lproduct_gimbal_project, r0
    jsr     @r0
    nop

    ! Inner edge is a constant 86 percent radial scale.
    mov     r12, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lproduct_core_radius_world, r0
    fmov.s  @r0, fr2
    mov.l   .Lproduct_gimbal_inner_scale, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr2
    mov     r10, r0
    shll2   r0
    mov.l   .Lproduct_gimbal_xscale_table, r1
    add     r0, r1
    fmov.s  @r1, fr3
    mov.l   .Lproduct_gimbal_yscale_table, r1
    add     r0, r1
    fmov.s  @r1, fr4
    fmov    fr1, fr7
    fmul    fr2, fr7
    fmul    fr3, fr7
    fmov    fr0, fr8
    fmul    fr2, fr8
    fmul    fr4, fr8
    mov.l   .Lproduct_core_center_y, r0
    fmov.s  @r0, fr5
    fadd    fr5, fr8
    mov.l   .Lproduct_core_depth_ptr, r0
    fmov.s  @r0, fr9
    mov     r8, r6
    add     #12, r6
    mov.l   .Lproduct_gimbal_project, r0
    jsr     @r0
    nop

    add     #24, r8
    mov.l   .Lproduct_gimbal_angle_step, r0
    add     r0, r12
    dt      r13
    bf      .Lproduct_gimbal_point_loop
    nop

    add     #1, r10
    mov     #3, r0
    cmp/hs  r0, r10
    bf      .Lproduct_gimbal_plane_loop
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
.Lproduct_gimbal_cache:          .long product_gimbal_projected
.Lproduct_gimbal_grade:          .long product_grade
.Lproduct_gimbal_depth_table:    .long product_core_depths
.Lproduct_gimbal_radius_table:   .long product_core_radii
.Lproduct_gimbal_velocity_table: .long product_gimbal_velocities
.Lproduct_gimbal_xscale_table:   .long product_gimbal_xscales
.Lproduct_gimbal_yscale_table:   .long product_gimbal_yscales
.Lproduct_gimbal_inner_scale:    .long product_gimbal_inner_scale
.Lproduct_gimbal_angle_step:     .long 4096
.Lproduct_gimbal_zero:           .long product_float_zero
.Lproduct_gimbal_coda_y:         .long product_core_coda_y
.Lproduct_gimbal_scale:          .long product_projection_scale
.Lproduct_gimbal_project:        .long product_project_to_cache
.Lproduct_core_depth_ptr:        .long product_core_depth
.Lproduct_core_radius_world:     .long product_core_radius_world
.Lproduct_core_radius_pixels:    .long product_core_radius_pixels
.Lproduct_core_center_y:         .long product_core_center_y
.Lproduct_core_center:           .long product_core_projected

! Inputs fr7/fr8/fr9 = camera-space XYZ, r6 = destination {screen X,Y,1/Z}.
product_project_to_cache:
    mov.l   .Lproduct_project_roll, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1

    ! Image-plane bank. The tunnel's world Z remains positive and untouched.
    fmov    fr7, fr2
    fmul    fr1, fr2
    fmov    fr8, fr3
    fmul    fr0, fr3
    fadd    fr3, fr2             ! rolled X
    fmov    fr7, fr3
    fmul    fr0, fr3
    fneg    fr3
    fmov    fr8, fr4
    fmul    fr1, fr4
    fadd    fr4, fr3             ! rolled Y

    mov.l   .Lproduct_project_one, r0
    fmov.s  @r0, fr10
    fdiv    fr9, fr10
    mov.l   .Lproduct_project_scale, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr2
    fmul    fr10, fr2
    mov.l   .Lproduct_project_cx, r0
    fmov.s  @r0, fr4
    fadd    fr4, fr2
    fmul    fr11, fr3
    fmul    fr10, fr3
    fneg    fr3
    mov.l   .Lproduct_project_cy, r0
    fmov.s  @r0, fr4
    fadd    fr4, fr3
    fmov.s  fr2, @r6
    add     #4, r6
    fmov.s  fr3, @r6
    add     #4, r6
    fmov.s  fr10, @r6
    rts
    nop

    .align 2
.Lproduct_project_roll: .long product_roll_sin
.Lproduct_project_one:  .long float_one
.Lproduct_project_scale:.long product_projection_scale
.Lproduct_project_cx:   .long center_x
.Lproduct_project_cy:   .long center_y

! ---------------------------------------------------------------------------
! Submission helpers shared by every cached layer.
! ---------------------------------------------------------------------------

! r4 = projected point pointer, r6 = TA command, r7 = ARGB.
product_emit_cached_vertex:
    fmov.s  @r4+, fr12
    fmov.s  @r4+, fr13
    fmov.s  @r4, fr14
    mov     r6, r4
    mov     r7, r5
    mov.l   .Lproduct_cached_emit, r0
    jmp     @r0
    nop

    .align 2
.Lproduct_cached_emit: .long emit_vertex

! r4/r5 endpoints, r6 width-float pointer, r7 color. Two triangles.
product_emit_beam:
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
    fmov    fr3, fr6
    fsub    fr0, fr6
    fmov    fr4, fr7
    fsub    fr1, fr7
    fmov    fr6, fr8
    fmul    fr6, fr8
    fmov    fr7, fr10
    fmul    fr7, fr10
    fadd    fr10, fr8
    mov.l   .Lproduct_beam_epsilon, r0
    fmov.s  @r0, fr10
    fadd    fr10, fr8
    fsrra   fr8
    fmul    fr8, fr6
    fmul    fr9, fr6             ! normal Y
    fmul    fr8, fr7
    fmul    fr9, fr7
    fneg    fr7                  ! normal X

    fmov    fr0, fr12
    fadd    fr7, fr12
    fmov    fr1, fr13
    fadd    fr6, fr13
    fmov    fr2, fr14
    mov.l   .Lproduct_beam_normal, r4
    mov     r11, r5
    mov.l   .Lproduct_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr0, fr12
    fsub    fr7, fr12
    fmov    fr1, fr13
    fsub    fr6, fr13
    fmov    fr2, fr14
    mov.l   .Lproduct_beam_normal, r4
    mov     r11, r5
    mov.l   .Lproduct_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr3, fr12
    fadd    fr7, fr12
    fmov    fr4, fr13
    fadd    fr6, fr13
    fmov    fr5, fr14
    mov.l   .Lproduct_beam_normal, r4
    mov     r11, r5
    mov.l   .Lproduct_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr3, fr12
    fsub    fr7, fr12
    fmov    fr4, fr13
    fsub    fr6, fr13
    fmov    fr5, fr14
    mov.l   .Lproduct_beam_eol, r4
    mov     r11, r5
    mov.l   .Lproduct_beam_emit, r0
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
.Lproduct_beam_epsilon:.long product_beam_epsilon
.Lproduct_beam_emit:   .long emit_vertex
.Lproduct_beam_normal: .long CMD_VERTEX
.Lproduct_beam_eol:    .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Opaque pass.
! ---------------------------------------------------------------------------

draw_product_backdrop:
    sts.l   pr, @-r15
    mov.l   r10, @-r15
    mov.l   .Lproduct_draw_backdrop_ptr, r0
    mov.l   @r0, r4
    mov     #4, r10
.Lproduct_draw_backdrop_loop:
    mov.l   .Lproduct_draw_backdrop_submit, r0
    jsr     @r0
    nop
    dt      r10
    bf      .Lproduct_draw_backdrop_loop
    nop
    mov.l   @r15+, r10
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lproduct_draw_backdrop_ptr:   .long product_backdrop_selected
.Lproduct_draw_backdrop_submit:.long ta_submit

draw_product_portal_panels:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lproduct_panel_cache, r8
    mov.l   .Lproduct_panel_grade, r0
    mov.l   @r0, r1
    shll2   r1
    shll2   r1                    ! four colors * four bytes
    mov.l   .Lproduct_panel_palettes, r13
    add     r1, r13
    mov     #0, r10
.Lproduct_panel_ring_loop:
    ! In the wasteland coda only alternating solid gates remain. All nine hot
    ! outlines still draw later, turning the dense tunnel into sparse black
    ! circular forms around the distant tower/orb silhouette.
    mov.l   .Lproduct_panel_grade, r0
    mov.l   @r0, r1
    mov     #3, r0
    cmp/eq  r0, r1
    bf      .Lproduct_panel_ring_visible
    mov     r10, r0
    and     #1, r0
    tst     r0, r0
    bt      .Lproduct_panel_ring_visible
    mov.l   .Lproduct_panel_cache_stride, r0
    add     r0, r8
    bra     .Lproduct_panel_ring_next
    nop
.Lproduct_panel_ring_visible:
    mov     #0, r11
.Lproduct_panel_segment_loop:
    ! Two absent panels per ring preserve the perforated/cage silhouette.
    mov     r10, r0
    shll    r0
    add     r10, r0
    add     r11, r0
    and     #7, r0
    cmp/eq  #7, r0
    bt      .Lproduct_panel_skip

    mov     r10, r0
    add     r11, r0
    and     #3, r0
    shll2   r0
    mov     r13, r1
    add     r0, r1
    mov.l   @r1, r14
    mov     r8, r4
    mov.l   .Lproduct_panel_normal, r6
    mov     r14, r7
    bsr     product_emit_cached_vertex
    nop
    mov     r8, r4
    add     #12, r4
    mov.l   .Lproduct_panel_normal, r6
    mov     r14, r7
    bsr     product_emit_cached_vertex
    nop
    mov     r8, r4
    add     #24, r4
    mov.l   .Lproduct_panel_normal, r6
    mov     r14, r7
    bsr     product_emit_cached_vertex
    nop
    mov     r8, r4
    add     #36, r4
    mov.l   .Lproduct_panel_eol, r6
    mov     r14, r7
    bsr     product_emit_cached_vertex
    nop
.Lproduct_panel_skip:
    add     #24, r8
    add     #1, r11
    mov     #16, r0
    cmp/hs  r0, r11
    bf      .Lproduct_panel_segment_loop
    nop
    add     #24, r8               ! duplicated seam point
.Lproduct_panel_ring_next:
    add     #1, r10
    mov     #9, r0
    cmp/hs  r0, r10
    bf      .Lproduct_panel_ring_loop
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
.Lproduct_panel_cache:   .long product_portal_projected
.Lproduct_panel_grade:   .long product_grade
.Lproduct_panel_palettes:.long product_panel_palettes
.Lproduct_panel_cache_stride:.long 408
.Lproduct_panel_normal:  .long CMD_VERTEX
.Lproduct_panel_eol:     .long CMD_VERTEX_EOL

draw_product_gimbal_panels:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lproduct_gpanel_cache, r8
    mov.l   .Lproduct_gpanel_grade, r0
    mov.l   @r0, r1
    shll2   r1
    shll2   r1
    mov.l   .Lproduct_gpanel_palettes, r13
    add     r1, r13
    mov     #0, r10
.Lproduct_gpanel_plane_loop:
    mov     #0, r11
.Lproduct_gpanel_segment_loop:
    mov     r10, r0
    add     r11, r0
    and     #3, r0
    shll2   r0
    mov     r13, r1
    add     r0, r1
    mov.l   @r1, r14
    mov     r8, r4
    mov.l   .Lproduct_gpanel_normal, r6
    mov     r14, r7
    bsr     product_emit_cached_vertex
    nop
    mov     r8, r4
    add     #12, r4
    mov.l   .Lproduct_gpanel_normal, r6
    mov     r14, r7
    bsr     product_emit_cached_vertex
    nop
    mov     r8, r4
    add     #24, r4
    mov.l   .Lproduct_gpanel_normal, r6
    mov     r14, r7
    bsr     product_emit_cached_vertex
    nop
    mov     r8, r4
    add     #36, r4
    mov.l   .Lproduct_gpanel_eol, r6
    mov     r14, r7
    bsr     product_emit_cached_vertex
    nop
    add     #24, r8
    add     #1, r11
    mov     #16, r0
    cmp/hs  r0, r11
    bf      .Lproduct_gpanel_segment_loop
    nop
    add     #24, r8
    add     #1, r10
    mov     #3, r0
    cmp/hs  r0, r10
    bf      .Lproduct_gpanel_plane_loop
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
.Lproduct_gpanel_cache:   .long product_gimbal_projected
.Lproduct_gpanel_grade:   .long product_grade
.Lproduct_gpanel_palettes:.long product_gimbal_palettes
.Lproduct_gpanel_normal:  .long CMD_VERTEX
.Lproduct_gpanel_eol:     .long CMD_VERTEX_EOL

draw_product_rails:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   .Lproduct_rails_cache, r8
    mov.l   .Lproduct_rails_grade, r0
    mov.l   @r0, r1
    shll2   r1
    shll2   r1
    mov.l   .Lproduct_rails_palettes, r12
    add     r1, r12
    mov     #0, r10
.Lproduct_rails_loop:
    mov     r10, r0
    and     #3, r0
    shll2   r0
    mov     r12, r1
    add     r0, r1
    mov.l   @r1, r7
    mov     r8, r4
    mov     r8, r5
    add     #12, r5
    mov.l   .Lproduct_rails_width, r6
    bsr     product_emit_beam
    nop
    add     #24, r8
    add     #1, r10
    mov     #16, r0
    cmp/hs  r0, r10
    bf      .Lproduct_rails_loop
    nop
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lproduct_rails_cache:   .long product_rail_projected
.Lproduct_rails_grade:   .long product_grade
.Lproduct_rails_palettes:.long product_rail_palettes
.Lproduct_rails_width:   .long product_rail_opaque_width

! Sixteen-triangle camera-facing seed / mesh-orb silhouette.
! r4 = radius pointer, r5 = four-color palette pointer.
draw_product_disc:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov     r5, r9
    mov.l   .Lproduct_disc_center, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1
    add     #4, r0
    fmov.s  @r0, fr2
    fmov.s  @r4, fr3
    mov     #0, r11
.Lproduct_disc_wedge:
    mov     r11, r0
    and     #3, r0
    shll2   r0
    mov     r9, r1
    add     r0, r1
    mov.l   @r1, r14
    mov     r11, r0
    shll8   r0
    shll2   r0
    shll2   r0
    lds     r0, fpul
    fsca    fpul, dr4
    add     #1, r11
    mov     r11, r0
    shll8   r0
    shll2   r0
    shll2   r0
    lds     r0, fpul
    fsca    fpul, dr6
    fmov    fr0, fr12
    fmov    fr1, fr13
    fmov    fr2, fr14
    mov.l   .Lproduct_disc_normal, r4
    mov     r14, r5
    mov.l   .Lproduct_disc_emit, r0
    jsr     @r0
    nop
    fmov    fr5, fr12
    fmul    fr3, fr12
    fadd    fr0, fr12
    fmov    fr4, fr13
    fmul    fr3, fr13
    fadd    fr1, fr13
    fmov    fr2, fr14
    mov.l   .Lproduct_disc_normal, r4
    mov     r14, r5
    mov.l   .Lproduct_disc_emit, r0
    jsr     @r0
    nop
    fmov    fr7, fr12
    fmul    fr3, fr12
    fadd    fr0, fr12
    fmov    fr6, fr13
    fmul    fr3, fr13
    fadd    fr1, fr13
    fmov    fr2, fr14
    mov.l   .Lproduct_disc_eol, r4
    mov     r14, r5
    mov.l   .Lproduct_disc_emit, r0
    jsr     @r0
    nop
    mov     #16, r0
    cmp/hs  r0, r11
    bf      .Lproduct_disc_wedge
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
.Lproduct_disc_center:.long product_core_projected
.Lproduct_disc_emit:  .long emit_vertex
.Lproduct_disc_normal:.long CMD_VERTEX
.Lproduct_disc_eol:   .long CMD_VERTEX_EOL

draw_product_core:
    sts.l   pr, @-r15
    mov.l   .Lproduct_core_grade, r0
    mov.l   @r0, r1
    shll2   r1
    shll2   r1
    mov.l   .Lproduct_core_palettes, r5
    add     r1, r5
    mov.l   .Lproduct_core_radius, r4
    mov.l   .Lproduct_core_disc, r0
    jsr     @r0
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lproduct_core_grade:   .long product_grade
.Lproduct_core_palettes:.long product_core_palettes
.Lproduct_core_radius:  .long product_core_radius_pixels
.Lproduct_core_disc:    .long draw_product_disc

! Twenty-four luminous perforations: sixteen outer, eight inner. They are tiny
! screen-space quads because the original illusion is materially richer when
! the eye reads repeated manufactured holes rather than another smooth disc.
draw_product_apertures:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lproduct_aperture_center, r0
    fmov.s  @r0+, fr8
    fmov.s  @r0+, fr9
    fmov.s  @r0, fr10
    mov.l   .Lproduct_aperture_depth_ptr, r0
    mov.l   .Lproduct_aperture_depth_bias, r1
    lds     r1, fpul
    fsts    fpul, fr0
    fadd    fr0, fr10
    fmov.s  fr10, @r0
    mov.l   .Lproduct_aperture_radius, r0
    fmov.s  @r0, fr11
    mov.l   .Lproduct_aperture_grade, r0
    mov.l   @r0, r1
    shll2   r1
    mov.l   .Lproduct_aperture_colors, r0
    add     r1, r0
    mov.l   @r0, r14

    mov     #0, r10
    mov     r9, r12
    shll8   r12
    shlr2   r12                   ! frame * 64
.Lproduct_aperture_outer_loop:
    mov     r10, r0
    shll8   r0
    shll2   r0
    shll2   r0                    ! point * 4096
    add     r12, r0
    lds     r0, fpul
    fsca    fpul, dr0
    fmov    fr11, fr2
    mov.l   .Lproduct_aperture_outer_x, r0
    fmov.s  @r0, fr3
    fmul    fr3, fr2
    fmul    fr1, fr2
    fadd    fr8, fr2
    ftrc    fr2, fpul
    sts     fpul, r4
    fmov    fr11, fr2
    mov.l   .Lproduct_aperture_outer_y, r0
    fmov.s  @r0, fr3
    fmul    fr3, fr2
    fmul    fr0, fr2
    fadd    fr9, fr2
    ftrc    fr2, fpul
    sts     fpul, r5
    mov     #2, r6
    mov     r14, r7
    mov.l   .Lproduct_aperture_box, r0
    jsr     @r0
    nop
    add     #1, r10
    mov     #16, r0
    cmp/hs  r0, r10
    bf      .Lproduct_aperture_outer_loop
    nop

    mov     #0, r10
    mov.l   .Lproduct_aperture_inner_phase, r0
    add     r0, r12
.Lproduct_aperture_inner_loop:
    mov     r10, r0
    shll8   r0
    shll2   r0
    shll2   r0
    shll    r0                    ! point * 8192
    add     r12, r0
    lds     r0, fpul
    fsca    fpul, dr0
    fmov    fr11, fr2
    mov.l   .Lproduct_aperture_inner_x, r0
    fmov.s  @r0, fr3
    fmul    fr3, fr2
    fmul    fr1, fr2
    fadd    fr8, fr2
    ftrc    fr2, fpul
    sts     fpul, r4
    fmov    fr11, fr2
    mov.l   .Lproduct_aperture_inner_y, r0
    fmov.s  @r0, fr3
    fmul    fr3, fr2
    fmul    fr0, fr2
    fadd    fr9, fr2
    ftrc    fr2, fpul
    sts     fpul, r5
    mov     #2, r6
    mov     r14, r7
    mov.l   .Lproduct_aperture_box, r0
    jsr     @r0
    nop
    add     #1, r10
    mov     #8, r0
    cmp/hs  r0, r10
    bf      .Lproduct_aperture_inner_loop
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
.Lproduct_aperture_center:     .long product_core_projected
.Lproduct_aperture_radius:     .long product_core_radius_pixels
.Lproduct_aperture_grade:      .long product_grade
.Lproduct_aperture_colors:     .long product_aperture_colors
.Lproduct_aperture_depth_ptr:  .long quad_depth
.Lproduct_aperture_depth_bias: .long 0x3b03126f ! 0.002
.Lproduct_aperture_outer_x:    .long product_aperture_outer_x
.Lproduct_aperture_outer_y:    .long product_aperture_outer_y
.Lproduct_aperture_inner_x:    .long product_aperture_inner_x
.Lproduct_aperture_inner_y:    .long product_aperture_inner_y
.Lproduct_aperture_inner_phase:.long 4096
.Lproduct_aperture_box:        .long emit_box

! Amber final movement: a deliberately flat black machine tower turns the orb
! into a distant landmark, echoing turn-of-the-century procedural wastelands.
draw_product_tower:
    sts.l   pr, @-r15
    mov.l   .Lproduct_tower_grade, r0
    mov.l   @r0, r1
    mov     #3, r0
    cmp/eq  r0, r1
    bf      .Lproduct_tower_done
    mov.l   .Lproduct_tower_depth_ptr, r0
    mov.l   .Lproduct_tower_depth, r1
    mov.l   r1, @r0
    mov.l   .Lproduct_tower_color_ptr, r0
    mov.l   .Lproduct_tower_color, r1
    mov.l   r1, @r0
    mov.l   .Lproduct_tower_rect, r0
    mov.l   r0, @-r15
    mov.l   .Lproduct_tower_trunk_x0, r4
    mov     #232, r5
    mov.l   .Lproduct_tower_trunk_x1, r6
    mov.l   .Lproduct_tower_bottom, r7
    jsr     @r0
    nop
    mov.l   @r15, r0
    mov.l   .Lproduct_tower_cross_x0, r4
    mov.l   .Lproduct_tower_cross_y0, r5
    mov.l   .Lproduct_tower_cross_x1, r6
    mov.l   .Lproduct_tower_cross_y1, r7
    jsr     @r0
    nop
    mov.l   @r15, r0
    mov.l   .Lproduct_tower_cross2_x0, r4
    mov.l   .Lproduct_tower_cross2_y0, r5
    mov.l   .Lproduct_tower_cross2_x1, r6
    mov.l   .Lproduct_tower_cross2_y1, r7
    jsr     @r0
    nop
    mov.l   @r15, r0
    mov.l   .Lproduct_tower_cross3_x0, r4
    mov.l   .Lproduct_tower_cross3_y0, r5
    mov.l   .Lproduct_tower_cross3_x1, r6
    mov.l   .Lproduct_tower_cross3_y1, r7
    jsr     @r0
    nop
    mov.l   @r15+, r0
.Lproduct_tower_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lproduct_tower_grade:    .long product_grade
.Lproduct_tower_depth_ptr:.long quad_depth
.Lproduct_tower_color_ptr:.long quad_color
.Lproduct_tower_depth:    .long 0x3d75c28f ! 0.06
.Lproduct_tower_color:    .long 0xff090403
.Lproduct_tower_rect:     .long emit_rect
.Lproduct_tower_trunk_x0: .long 302
.Lproduct_tower_trunk_x1: .long 338
.Lproduct_tower_bottom:   .long 440
.Lproduct_tower_cross_x0: .long 276
.Lproduct_tower_cross_y0: .long 282
.Lproduct_tower_cross_x1: .long 364
.Lproduct_tower_cross_y1: .long 288
.Lproduct_tower_cross2_x0:.long 264
.Lproduct_tower_cross2_y0:.long 334
.Lproduct_tower_cross2_x1:.long 376
.Lproduct_tower_cross2_y1:.long 342
.Lproduct_tower_cross3_x0:.long 250
.Lproduct_tower_cross3_y0:.long 394
.Lproduct_tower_cross3_x1:.long 390
.Lproduct_tower_cross3_y1:.long 404

! Letterbox bars and sparse one-pixel instrumentation ticks. The shared title
! is emitted after these masks, keeping both the identity and scene plaque on
! top of the cinematic frame.
draw_product_masks:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   .Lproduct_mask_depth_ptr, r0
    mov.l   .Lproduct_mask_depth, r1
    mov.l   r1, @r0
    mov.l   .Lproduct_mask_color_ptr, r0
    mov.l   .Lproduct_mask_bar_color, r1
    mov.l   r1, @r0
    mov     #0, r4
    mov     #0, r5
    mov.l   .Lproduct_mask_width, r6
    mov     #44, r7
    mov.l   .Lproduct_mask_rect, r0
    jsr     @r0
    nop
    mov     #0, r4
    mov.l   .Lproduct_mask_bottom, r5
    mov.l   .Lproduct_mask_width, r6
    mov.l   .Lproduct_mask_height, r7
    mov.l   .Lproduct_mask_rect, r0
    jsr     @r0
    nop

    mov.l   .Lproduct_mask_color_ptr, r0
    mov.l   .Lproduct_mask_tick_color, r1
    mov.l   r1, @r0
    mov     #64, r8
.Lproduct_mask_tick_loop:
    mov     r8, r4
    mov.l   .Lproduct_mask_tick_y0, r5
    mov     r8, r6
    add     #2, r6
    mov.l   .Lproduct_mask_tick_y1, r7
    mov.l   .Lproduct_mask_rect, r0
    jsr     @r0
    nop
    add     #32, r8
    mov.l   .Lproduct_mask_tick_end, r0
    cmp/hs  r0, r8
    bf      .Lproduct_mask_tick_loop
    nop
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lproduct_mask_depth_ptr:.long quad_depth
.Lproduct_mask_color_ptr:.long quad_color
.Lproduct_mask_rect:     .long emit_rect
.Lproduct_mask_depth:    .long 0x3feccccd ! 1.85
.Lproduct_mask_bar_color:.long 0xff010100
.Lproduct_mask_tick_color:.long 0xff50351c
.Lproduct_mask_width:    .long 640
.Lproduct_mask_height:   .long 480
.Lproduct_mask_bottom:   .long 436
.Lproduct_mask_tick_y0:  .long 408
.Lproduct_mask_tick_y1:  .long 412
.Lproduct_mask_tick_end: .long 592

! ---------------------------------------------------------------------------
! Additive pass.
! ---------------------------------------------------------------------------

draw_product_portal_glow:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lproduct_pglow_cache, r8
    mov.l   .Lproduct_pglow_grade, r0
    mov.l   @r0, r1
    shll2   r1
    shll2   r1
    mov.l   .Lproduct_pglow_palettes, r13
    add     r1, r13
    mov     #0, r10
.Lproduct_pglow_ring_loop:
    mov     #0, r11
.Lproduct_pglow_segment_loop:
    mov     r10, r0
    add     r11, r0
    mov     r0, r12
    and     #3, r0
    shll2   r0
    mov     r13, r1
    add     r0, r1
    mov.l   @r1, r14
    mov     r12, r0
    and     #1, r0
    tst     r0, r0
    bf      .Lproduct_pglow_inner_test
    mov     r8, r4
    mov     r8, r5
    add     #24, r5
    mov.l   .Lproduct_pglow_width, r6
    mov     r14, r7
    bsr     product_emit_beam
    nop
.Lproduct_pglow_inner_test:
    mov     r12, r0
    and     #3, r0
    tst     r0, r0
    bf      .Lproduct_pglow_next
    mov     r8, r4
    add     #12, r4
    mov     r8, r5
    add     #36, r5
    mov.l   .Lproduct_pglow_width, r6
    mov     r14, r7
    bsr     product_emit_beam
    nop
.Lproduct_pglow_next:
    add     #24, r8
    add     #1, r11
    mov     #16, r0
    cmp/hs  r0, r11
    bf      .Lproduct_pglow_segment_loop
    nop
    add     #24, r8
    add     #1, r10
    mov     #9, r0
    cmp/hs  r0, r10
    bf      .Lproduct_pglow_ring_loop
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
.Lproduct_pglow_cache:   .long product_portal_projected
.Lproduct_pglow_grade:   .long product_grade
.Lproduct_pglow_palettes:.long product_glow_palettes
.Lproduct_pglow_width:   .long product_portal_glow_width

draw_product_gimbal_glow:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lproduct_gglow_cache, r8
    mov.l   .Lproduct_gglow_grade, r0
    mov.l   @r0, r1
    shll2   r1
    shll2   r1
    mov.l   .Lproduct_gglow_palettes, r13
    add     r1, r13
    mov     #0, r10
.Lproduct_gglow_plane_loop:
    mov     #0, r11
.Lproduct_gglow_segment_loop:
    mov     r10, r0
    add     r11, r0
    and     #3, r0
    shll2   r0
    mov     r13, r1
    add     r0, r1
    mov.l   @r1, r14
    mov     r8, r4
    mov     r8, r5
    add     #24, r5
    mov.l   .Lproduct_gglow_width, r6
    mov     r14, r7
    bsr     product_emit_beam
    nop
    mov     r11, r0
    and     #1, r0
    tst     r0, r0
    bf      .Lproduct_gglow_next
    mov     r8, r4
    add     #12, r4
    mov     r8, r5
    add     #36, r5
    mov.l   .Lproduct_gglow_width, r6
    mov     r14, r7
    bsr     product_emit_beam
    nop
.Lproduct_gglow_next:
    add     #24, r8
    add     #1, r11
    mov     #16, r0
    cmp/hs  r0, r11
    bf      .Lproduct_gglow_segment_loop
    nop
    add     #24, r8
    add     #1, r10
    mov     #3, r0
    cmp/hs  r0, r10
    bf      .Lproduct_gglow_plane_loop
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
.Lproduct_gglow_cache:   .long product_gimbal_projected
.Lproduct_gglow_grade:   .long product_grade
.Lproduct_gglow_palettes:.long product_gimbal_glow_palettes
.Lproduct_gglow_width:   .long product_gimbal_glow_width

! The steel movement adds sixteen converging spot beams. Other movements keep
! only the thin cage-wire pass, allowing a strong rhythmic contrast at frame
! 384 without changing the expensive cached topology.
draw_product_light_rays:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   .Lproduct_rays_grade, r0
    mov.l   @r0, r12
    mov.l   .Lproduct_rays_cache, r8
    mov.l   .Lproduct_rays_palettes, r11
    mov     r12, r0
    shll2   r0
    shll2   r0
    add     r0, r11
    mov     #0, r10
.Lproduct_rays_rail_loop:
    mov     r10, r0
    and     #3, r0
    shll2   r0
    mov     r11, r1
    add     r0, r1
    mov.l   @r1, r7
    mov     r8, r4
    mov     r8, r5
    add     #12, r5
    mov.l   .Lproduct_rays_wire_width, r6
    bsr     product_emit_beam
    nop
    add     #24, r8
    add     #1, r10
    mov     #16, r0
    cmp/hs  r0, r10
    bf      .Lproduct_rays_rail_loop
    nop

    mov     #2, r0
    cmp/eq  r0, r12
    bf      .Lproduct_rays_done
    mov.l   .Lproduct_rays_target_cache, r8
    mov     #0, r10
.Lproduct_rays_spot_loop:
    mov     r10, r0
    and     #3, r0
    shll2   r0
    mov     r11, r1
    add     r0, r1
    mov.l   @r1, r7
    mov.l   .Lproduct_rays_center, r4
    mov     r8, r5
    mov.l   .Lproduct_rays_spot_width, r6
    bsr     product_emit_beam
    nop
    add     #24, r8
    add     #1, r10
    mov     #16, r0
    cmp/hs  r0, r10
    bf      .Lproduct_rays_spot_loop
    nop
.Lproduct_rays_done:
    mov.l   @r15+, r12
    mov.l   @r15+, r11
    mov.l   @r15+, r10
    mov.l   @r15+, r8
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lproduct_rays_grade:       .long product_grade
.Lproduct_rays_cache:       .long product_rail_projected
.Lproduct_rays_target_cache:.long product_portal_projected
.Lproduct_rays_center:      .long product_core_projected
.Lproduct_rays_palettes:    .long product_rail_glow_palettes
.Lproduct_rays_wire_width:  .long product_rail_glow_width
.Lproduct_rays_spot_width:  .long product_spot_width

draw_product_flare:
    sts.l   pr, @-r15
    mov.l   .Lproduct_flare_grade, r0
    mov.l   @r0, r1
    mov     r1, r2
    shll2   r1
    mov.l   .Lproduct_flare_radius_table, r4
    add     r1, r4
    shll2   r1
    mov.l   .Lproduct_flare_palettes, r5
    add     r1, r5
    mov.l   .Lproduct_flare_disc, r0
    jsr     @r0
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lproduct_flare_grade:       .long product_grade
.Lproduct_flare_radius_table:.long product_flare_radii
.Lproduct_flare_palettes:    .long product_flare_palettes
.Lproduct_flare_disc:        .long draw_product_disc

! ---------------------------------------------------------------------------
! Read-only scene data.
! ---------------------------------------------------------------------------

    .section .rodata.product,"a"
    .align 5

product_backdrop_table:
    .long product_backdrop_gold
    .long product_backdrop_cage
    .long product_backdrop_steel
    .long product_backdrop_amber

! Four native vertices per field. Copper/brown, oxidized cage, high-key steel,
! then amber dusk provide the four hard lighting movements.
product_backdrop_gold:
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3a83126f,0,0,0xff090604,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3a83126f,0,0,0xff1b1208,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3a83126f,0,0,0xff140b05,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3a83126f,0,0,0xff35200c,0
product_backdrop_cage:
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3a83126f,0,0,0xff080b0a,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3a83126f,0,0,0xff17201c,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3a83126f,0,0,0xff13100a,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3a83126f,0,0,0xff302814,0
product_backdrop_steel:
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3a83126f,0,0,0xff313b3f,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3a83126f,0,0,0xff8b9697,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3a83126f,0,0,0xff4e5655,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3a83126f,0,0,0xffb8b9ae,0
product_backdrop_amber:
    .long CMD_VERTEX,     0x00000000,0x43f00000,0x3a83126f,0,0,0xff120704,0
    .long CMD_VERTEX,     0x00000000,0x00000000,0x3a83126f,0,0,0xff4c1b09,0
    .long CMD_VERTEX,     0x44200000,0x43f00000,0x3a83126f,0,0,0xff250d05,0
    .long CMD_VERTEX_EOL, 0x44200000,0x00000000,0x3a83126f,0,0,0xff8b3f12,0

product_panel_palettes:
    .long 0xff171108,0xff2a1c0c,0xff3c2b12,0xff5a401a
    .long 0xff111615,0xff1b2421,0xff29332e,0xff403c28
    .long 0xff4c575a,0xff69777a,0xff899497,0xffadb2ac
    .long 0xff0c0503,0xff150804,0xff201006,0xff2c1608
product_gimbal_palettes:
    .long 0xff2b1b08,0xff503411,0xff7a541c,0xffa87828
    .long 0xff141915,0xff283028,0xff414735,0xff625938
    .long 0xff596064,0xff7d888b,0xffa0a8a8,0xffd0cec1
    .long 0xff160704,0xff271006,0xff3b1908,0xff56270c
product_rail_palettes:
    .long 0xff090704,0xff100b05,0xff171007,0xff20170a
    .long 0xff080b09,0xff0d120f,0xff131914,0xff1d2118
    .long 0xff303638,0xff41494a,0xff535a59,0xff6c6d67
    .long 0xff070302,0xff0b0402,0xff100603,0xff170804
product_core_palettes:
    .long 0xff050403,0xff0b0804,0xff171006,0xff24170a
    .long 0xff040605,0xff09100d,0xff111b15,0xff1e261b
    .long 0xff20282a,0xff303b3d,0xff414b4c,0xff59605d
    .long 0xff070302,0xff110603,0xff1d0a04,0xff301006

product_glow_palettes:
    .long 0xff3c2108,0xff57340c,0xff764d12,0xff94681c
    .long 0xff183028,0xff28503c,0xff447052,0xff6b8060
    .long 0xff38585c,0xff587c80,0xff80a4a4,0xffb4c0b8
    .long 0xff5c2108,0xff853510,0xffb54f18,0xffe87528
product_gimbal_glow_palettes:
    .long 0xff68400c,0xff936318,0xffc68d27,0xffe8b84a
    .long 0xff244638,0xff3c6850,0xff659071,0xff8ba782
    .long 0xff5b7c80,0xff83a4a6,0xffb0c8c5,0xffe0ddd0
    .long 0xff7b2d0b,0xffad4512,0xffdc671e,0xffff9a38
product_rail_glow_palettes:
    .long 0xff281807,0xff3b250a,0xff52370e,0xff6a4b15
    .long 0xff10251e,0xff1c382b,0xff2c4d39,0xff46634a
    .long 0xff49686c,0xff719093,0xff9fb5b4,0xffd0d8cf
    .long 0xff421607,0xff62230b,0xff893510,0xffb65018
product_flare_palettes:
    .long 0xff5c370c,0xff805514,0xffad7c24,0xffd8ad45
    .long 0xff244538,0xff3c6650,0xff648a6b,0xff8aa57e
    .long 0xff6c9094,0xff9db8b8,0xffcbd4ce,0xffeee8d8
    .long 0xff8a320c,0xffba4b14,0xffe66e22,0xffffa344
product_aperture_colors:
    .long 0xffc89230,0xff7f9b61,0xffe6e0c8,0xffff8628

product_bank_units:
    .long 0x44800000,0x45000000,0x45800000,0x44c00000 ! 1024,2048,4096,1536
product_bend_amplitudes:
    .long 0x3e3851ec,0x3e8f5c29,0x3ed70a3d,0x3e75c28f ! .18,.28,.42,.24
product_outer_radii:
    .long 0x4059999a,0x4079999a,0x408ccccd,0x404ccccd ! 3.4,3.9,4.4,3.2
product_inner_radii:
    .long 0x40333333,0x4051eb85,0x406e147b,0x402b851f ! 2.8,3.28,3.72,2.68
product_core_depths:
    .long 0x41180000,0x41500000,0x41900000,0x41280000 ! 9.5,13,18,10.5
product_core_radii:
    .long 0x40166666,0x40000000,0x40333333,0x40200000 ! 2.35,2,2.8,2.5
product_gimbal_velocities:
    .long 40,-56,72
product_gimbal_xscales:
    .long 0x3f800000,0x3f800000,0x3ea3d70a ! 1,1,.32
product_gimbal_yscales:
    .long 0x3f800000,0x3ea3d70a,0x3f800000 ! 1,.32,1
product_flare_radii:
    .long 0x41200000,0x41600000,0x41e00000,0x41900000 ! 10,14,28,18 px

product_depth_step:          .long 0x3c900000 ! 18/1024
product_depth_near:          .long 0x3faccccd ! 1.35
product_bend_y_scale:        .long 0x3f0ccccd ! 0.55
product_projection_scale:    .long 0x43960000 ! 300
product_rail_radius:         .long 0x40833333 ! 4.10
product_rail_near_z:         .long 0x3fb9999a ! 1.45
product_rail_far_z:          .long 0x41a40000 ! 20.5
product_gimbal_inner_scale: .long 0x3f5c28f6 ! 0.86
product_core_coda_y:         .long 0x3fcccccd ! 1.60 world units / rises on screen
product_float_zero:          .long 0x00000000
product_aperture_outer_x:    .long 0x3f2e147b ! 0.68
product_aperture_outer_y:    .long 0x3ef5c28f ! 0.48
product_aperture_inner_x:    .long 0x3eb33333 ! 0.35
product_aperture_inner_y:    .long 0x3e8f5c29 ! 0.28
product_beam_epsilon:        .long 0x3e800000 ! 0.25 pixel squared
product_rail_opaque_width:   .long 0x40a00000 ! 5.0 pixels
product_portal_glow_width:   .long 0x3fa66666 ! 1.3 pixels
product_gimbal_glow_width:   .long 0x3fcccccd ! 1.6 pixels
product_rail_glow_width:     .long 0x3f400000 ! 0.75 pixels
product_spot_width:          .long 0x40200000 ! 2.5 pixels

! ---------------------------------------------------------------------------
! Writable scene state and projection caches.
! ---------------------------------------------------------------------------

    .section .bss.product,"aw",@nobits
    .align 5
product_grade:              .space 4
product_travel:             .space 4
product_backdrop_selected:  .space 4
product_roll_sin:           .space 4
product_roll_cos:           .space 4
product_work_center_x:      .space 4
product_work_center_y:      .space 4
product_work_depth:         .space 4
product_work_outer:         .space 4
product_work_inner:         .space 4
product_core_depth:         .space 4
product_core_radius_world:  .space 4
product_core_radius_pixels: .space 4
product_core_center_y:      .space 4
    .align 5
product_core_projected:     .space 12
product_portal_projected:   .space 9 * 17 * 2 * 12
product_rail_projected:     .space 16 * 2 * 12
product_gimbal_projected:   .space 3 * 17 * 2 * 12

    .section .text,"ax"
