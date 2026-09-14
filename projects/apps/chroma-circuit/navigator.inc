! ---------------------------------------------------------------------------
! CHROMA CIRCUIT -- Scene 08: NAVIGATOR
!
! A generated, genuinely three-dimensional diagnostic model of an organic
! teardrop spacecraft.  The visual language follows the user's reference:
! dense emerald monitor wire, a smooth dark lower bowl, three brighter dorsal
! ribs, and a central cleft cut into the forward crown.  No mesh or image asset
! is stored -- SH-4 builds and projects both surfaces every display frame.
!
! The upper shell is a 13 x 24 polar grid.  Its meridians, latitude rings, and
! alternating cell diagonals form the triangulated topology seen on the film's
! navigation display.  A separate 7 x 24 lower surface gives the craft real
! thickness and lets the opaque PVR depth buffer hide distant diagnostic wire.
!
! Fixed geometry, excluding the shared title / transition:
!      2  backdrop triangles
!    288  opaque lower-bowl triangles (six 50-vertex strips)
!    576  upper meridian beam triangles
!    576  upper ring beam triangles
!    288  alternating upper diagonal beam triangles
!    144  three paired dorsal-rib beam triangles
!    144  sparse lower meridian beam triangles
!    144  three lower contour-ring beam triangles
!   ----
!   2162  triangles / 4,051 native parameter blocks at the scene peak.
!
! Writable scene state is 5,824 bytes: 5,760 bytes of projected vertices and
! 64 bytes of pose/scratch data.  The peak parameter traffic is about 127 KiB,
! comfortably inside the 512 KiB TA vertex buffer.  The common renderer owns
! the final translucent-list terminator.
! ---------------------------------------------------------------------------

    .section .text,"ax"
    .align 2

! r9 is the scene-local frame (0..767).  Preserve it for the shared transition.
draw_scene_navigator:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Lnav_scene_pose, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_build, r0
    jsr     @r0
    nop

    ! The dark lower bowl is real opaque geometry.  Besides defining the
    ! silhouette, it occludes the far side of the additive mesh through PVR2's
    ! depth buffer, giving the diagnostic drawing much stronger volume.
    mov.l   .Lnav_scene_opaque, r4
    mov.l   .Lnav_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_backdrop, r8
    mov     #4, r10
.Lnav_backdrop_loop:
    mov     r8, r4
    mov.l   .Lnav_scene_submit, r0
    jsr     @r0
    nop
    add     #32, r8
    dt      r10
    bf      .Lnav_backdrop_loop
    nop
    mov.l   .Lnav_scene_bowl, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_title, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_zero, r4
    mov.l   .Lnav_scene_submit, r0
    jsr     @r0
    nop

    ! Dim topology first, bright structural ribs last.  ONE/ONE blending makes
    ! crossings bloom like phosphor while the deliberately restrained palette
    ! keeps the hundreds of mesh edges from flattening into white.
    mov.l   .Lnav_scene_additive, r4
    mov.l   .Lnav_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_meridians, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_rings, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_diagonals, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_lower_wire, r0
    jsr     @r0
    nop
    mov.l   .Lnav_scene_ribs, r0
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
.Lnav_scene_pose:       .long prepare_navigator_pose
.Lnav_scene_build:      .long build_navigator_mesh
.Lnav_scene_bowl:       .long draw_navigator_lower_bowl
.Lnav_scene_meridians:  .long draw_navigator_meridians
.Lnav_scene_rings:      .long draw_navigator_rings
.Lnav_scene_diagonals:  .long draw_navigator_diagonals
.Lnav_scene_lower_wire: .long draw_navigator_lower_wire
.Lnav_scene_ribs:       .long draw_navigator_dorsal_ribs
.Lnav_scene_submit:     .long ta_submit
.Lnav_scene_title:      .long draw_title
.Lnav_scene_opaque:     .long opaque_header
.Lnav_scene_additive:   .long additive_header
.Lnav_scene_zero:       .long zero_block
.Lnav_scene_backdrop:   .long navigator_backdrop_vertices

! ---------------------------------------------------------------------------
! Pose evolution.
!
! The asymmetric craft makes a full turn plus another half turn in 768 frames.
! Slow pitch, roll, distance, and framing waves keep revealing the top cleft,
! broad stern, sharp nose, and lower bowl rather than presenting a turntable
! locked to one elevation.
! ---------------------------------------------------------------------------

prepare_navigator_pose:
    ! Object yaw: frame * 128 = exactly 1.5 revolutions over this act.
    mov     r9, r0
    shll8   r0
    shlr    r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lnav_pose_yaw, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0

    ! Pitch: -17.6 degrees with a +/-23.6 degree breathing inspection tilt.
    mov     r9, r0
    shll2   r0
    shll2   r0
    shll2   r0
    shll    r0                    ! frame * 64
    lds     r0, fpul
    fsca    fpul, dr0
    fmov    fr1, fr4              ! quadrature retained for roll
    mov.l   .Lnav_pose_pitch_amp, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    mov.l   .Lnav_pose_pitch_base, r0
    fmov.s  @r0, fr2
    fadd    fr2, fr0
    ftrc    fr0, fpul
    fsca    fpul, dr2
    mov.l   .Lnav_pose_pitch, r0
    fmov.s  fr2, @r0
    add     #4, r0
    fmov.s  fr3, @r0

    ! Very small roll stops the reference-monitor view from feeling sterile.
    mov.l   .Lnav_pose_roll_amp, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr4
    ftrc    fr4, fpul
    fsca    fpul, dr2
    mov.l   .Lnav_pose_roll, r0
    fmov.s  fr2, @r0
    add     #4, r0
    fmov.s  fr3, @r0

    ! One slower wave controls dolly and a subtle CRT-style framing drift.
    mov     r9, r0
    shll2   r0
    shll2   r0                    ! *16
    mov     r9, r1
    shll2   r1
    shll2   r1
    add     r1, r0
    add     r1, r0                ! *48
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lnav_pose_distance_amp, r0
    fmov.s  @r0, fr2
    fmul    fr1, fr2
    mov.l   .Lnav_pose_distance_base, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr2
    mov.l   .Lnav_pose_distance, r0
    fmov.s  fr2, @r0

    mov.l   .Lnav_pose_center_x_amp, r0
    fmov.s  @r0, fr2
    fmul    fr0, fr2
    mov.l   .Lnav_pose_center_x_base, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr2
    mov.l   .Lnav_pose_center_x, r0
    fmov.s  fr2, @r0

    mov.l   .Lnav_pose_center_y_amp, r0
    fmov.s  @r0, fr2
    fmul    fr1, fr2
    mov.l   .Lnav_pose_center_y_base, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr2
    mov.l   .Lnav_pose_center_y, r0
    fmov.s  fr2, @r0
    rts
    nop

    .align 2
.Lnav_pose_yaw:          .long navigator_yaw
.Lnav_pose_pitch:        .long navigator_pitch
.Lnav_pose_roll:         .long navigator_roll
.Lnav_pose_distance:     .long navigator_distance
.Lnav_pose_center_x:     .long navigator_center_x
.Lnav_pose_center_y:     .long navigator_center_y
.Lnav_pose_pitch_amp:    .long navigator_pitch_units
.Lnav_pose_pitch_base:   .long navigator_pitch_bias
.Lnav_pose_roll_amp:     .long navigator_roll_units
.Lnav_pose_distance_amp: .long navigator_distance_amplitude
.Lnav_pose_distance_base:.long navigator_distance_base
.Lnav_pose_center_x_amp: .long navigator_center_x_amplitude
.Lnav_pose_center_x_base:.long navigator_center_x_base
.Lnav_pose_center_y_amp: .long navigator_center_y_amplitude
.Lnav_pose_center_y_base:.long navigator_center_y_base

! ---------------------------------------------------------------------------
! Procedural shell construction.
! ---------------------------------------------------------------------------

build_navigator_mesh:
    sts.l   pr, @-r15

    mov.l   .Lnav_build_upper_cache, r4
    mov     #13, r5
    mov.l   .Lnav_build_upper_inverse, r6
    mov     #0, r7
    mov.l   .Lnav_build_surface, r0
    jsr     @r0
    nop

    mov.l   .Lnav_build_lower_cache, r4
    mov     #7, r5
    mov.l   .Lnav_build_lower_inverse, r6
    mov     #1, r7
    mov.l   .Lnav_build_surface, r0
    jsr     @r0
    nop

    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lnav_build_upper_cache:  .long navigator_upper_projected
.Lnav_build_lower_cache:  .long navigator_lower_projected
.Lnav_build_upper_inverse:.long navigator_upper_ring_inverse
.Lnav_build_lower_inverse:.long navigator_lower_ring_inverse
.Lnav_build_surface:      .long navigator_build_surface

! r4=destination, r5=ring count, r6=float inverse of final ring index,
! r7=0 upper / 1 lower.  Each surface is a polar height field over an
! asymmetric teardrop plan.  Both close on the same zero-height rim.
navigator_build_surface:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov     r4, r8
    mov     r5, r11
    mov     r6, r12
    mov     r7, r13
    mov     #0, r10

.Lnav_build_ring:
    ! t is radial position 0..1; envelope 1-t^2 closes both shells at rim.
    lds     r10, fpul
    float   fpul, fr0
    fmov.s  @r12, fr1
    fmul    fr1, fr0
    mov.l   .Lnav_build_t, r0
    fmov.s  fr0, @r0
    fmov    fr0, fr1
    fmul    fr0, fr1
    mov.l   .Lnav_build_one, r0
    fmov.s  @r0, fr2
    fsub    fr1, fr2
    mov.l   .Lnav_build_envelope, r0
    fmov.s  fr2, @r0

    mov.l   .Lnav_build_angle_ptr, r0
    mov     #0, r1
    mov.l   r1, @r0
    mov     #24, r14

.Lnav_build_segment:
    mov.l   .Lnav_build_angle_ptr, r0
    mov.l   @r0, r1
    lds     r1, fpul
    fsca    fpul, dr0              ! fr0=sin theta, fr1=cos theta

    ! Broad rear / sharp nose planform:
    ! X=t*sin(theta)*(2.80-0.18*cos(theta))
    ! Z=t*cos(theta)*(2.35+0.36*cos(theta))
    mov.l   .Lnav_build_t, r0
    fmov.s  @r0, fr4
    mov.l   .Lnav_build_width_asym, r0
    fmov.s  @r0, fr5
    fmul    fr1, fr5
    mov.l   .Lnav_build_width, r0
    fmov.s  @r0, fr6
    fsub    fr5, fr6
    fmul    fr4, fr6
    fmul    fr0, fr6
    mov.l   .Lnav_build_x, r0
    fmov.s  fr6, @r0

    mov.l   .Lnav_build_length_asym, r0
    fmov.s  @r0, fr5
    fmul    fr1, fr5
    mov.l   .Lnav_build_length, r0
    fmov.s  @r0, fr6
    fadd    fr5, fr6
    fmul    fr4, fr6
    fmul    fr1, fr6
    mov.l   .Lnav_build_z, r0
    fmov.s  fr6, @r0

    tst     r13, r13
    bf      .Lnav_build_lower
    nop

    ! Upper shell.  The negative cos(3*theta) coefficient raises paired organic
    ! shoulder lobes around the nose and shapes the broad stern.  A front-only
    ! cos^4 depression cuts the unmistakable central cleft without splitting
    ! the watertight rim.
    mov.l   .Lnav_build_angle_ptr, r0
    mov.l   @r0, r2
    mov     r2, r3
    shll    r2
    add     r3, r2
    lds     r2, fpul
    fsca    fpul, dr2              ! fr3=cos(3 theta)
    mov.l   .Lnav_build_t, r0
    fmov.s  @r0, fr4
    mov.l   .Lnav_build_upper_lobe, r0
    fmov.s  @r0, fr5
    fmul    fr4, fr5
    fmul    fr3, fr5
    mov.l   .Lnav_build_upper_base, r0
    fmov.s  @r0, fr6
    fadd    fr5, fr6
    mov.l   .Lnav_build_upper_front, r0
    fmov.s  @r0, fr5
    fmul    fr4, fr5
    fmul    fr1, fr5
    fadd    fr5, fr6
    mov.l   .Lnav_build_envelope, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr6               ! uncloven upper Y

    fldi0   fr0
    fcmp/gt fr0, fr1               ! cleft exists only on forward half
    bf      .Lnav_build_y_ready
    nop
    fmov    fr1, fr2
    fmul    fr1, fr2
    fmul    fr2, fr2               ! cos^4 theta
    mov.l   .Lnav_build_one, r0
    fmov.s  @r0, fr3
    fsub    fr4, fr3               ! 1-t
    fmul    fr4, fr3               ! t*(1-t)
    fmul    fr2, fr3
    mov.l   .Lnav_build_cleft, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr3
    fsub    fr3, fr6
    bra     .Lnav_build_y_ready
    nop

.Lnav_build_lower:
    ! The underbody is shallower and subtly deeper toward the nose.  Unlike a
    ! flat cap it keeps changing silhouette throughout the spin.
    mov.l   .Lnav_build_lower_front, r0
    fmov.s  @r0, fr5
    fmul    fr4, fr5               ! converge all azimuths at lower pole
    fmul    fr1, fr5
    mov.l   .Lnav_build_lower_base, r0
    fmov.s  @r0, fr6
    fadd    fr5, fr6
    mov.l   .Lnav_build_envelope, r0
    fmov.s  @r0, fr5
    fmul    fr5, fr6
    fneg    fr6

.Lnav_build_y_ready:
    mov.l   .Lnav_build_x, r0
    fmov.s  @r0, fr7
    fmov    fr6, fr8
    mov.l   .Lnav_build_z, r0
    fmov.s  @r0, fr9
    mov     r8, r4
    mov.l   .Lnav_build_project, r0
    jsr     @r0
    nop
    add     #12, r8

    mov.l   .Lnav_build_angle_ptr, r0
    mov.l   @r0, r1
    mov.l   .Lnav_build_angle_step, r2
    add     r2, r1
    mov.l   r1, @r0
    dt      r14
    bf      .Lnav_build_segment
    nop

    add     #1, r10
    dt      r11
    bf      .Lnav_build_ring
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
.Lnav_build_project:      .long navigator_transform_project
.Lnav_build_t:            .long navigator_radial_t
.Lnav_build_envelope:     .long navigator_radial_envelope
.Lnav_build_x:            .long navigator_scratch_x
.Lnav_build_z:            .long navigator_scratch_z
.Lnav_build_angle_ptr:    .long navigator_build_angle
.Lnav_build_one:          .long float_one
.Lnav_build_width:        .long navigator_width
.Lnav_build_width_asym:   .long navigator_width_asymmetry
.Lnav_build_length:       .long navigator_length
.Lnav_build_length_asym:  .long navigator_length_asymmetry
.Lnav_build_upper_base:   .long navigator_upper_height
.Lnav_build_upper_lobe:   .long navigator_upper_lobe
.Lnav_build_upper_front:  .long navigator_upper_front_bias
.Lnav_build_cleft:        .long navigator_front_cleft
.Lnav_build_lower_base:   .long navigator_lower_height
.Lnav_build_lower_front:  .long navigator_lower_front_bias
.Lnav_build_angle_step:   .long 2731

! Inputs: fr7/fr8/fr9 = model XYZ, r4 -> projected XYZ destination.
navigator_transform_project:
    mov     r4, r6

    ! Continuous object yaw.
    mov.l   .Lnav_project_yaw, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1
    fmov    fr7, fr10
    fmul    fr1, fr10
    fmov    fr9, fr11
    fmul    fr0, fr11
    fadd    fr11, fr10             ! X1
    fmov    fr9, fr11
    fmul    fr1, fr11
    fmov    fr7, fr12
    fmul    fr0, fr12
    fsub    fr12, fr11             ! Z1

    ! Inspection pitch.
    mov.l   .Lnav_project_pitch, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1
    fmov    fr8, fr12
    fmul    fr1, fr12
    fmov    fr11, fr13
    fmul    fr0, fr13
    fsub    fr13, fr12             ! Y2
    fmov    fr8, fr13
    fmul    fr0, fr13
    fmov    fr11, fr14
    fmul    fr1, fr14
    fadd    fr14, fr13             ! Z2

    ! Screen-plane roll.
    mov.l   .Lnav_project_roll, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1
    fmov    fr10, fr14
    fmul    fr1, fr14
    fmov    fr12, fr15
    fmul    fr0, fr15
    fsub    fr15, fr14             ! X3
    fmov    fr10, fr15
    fmul    fr0, fr15
    fmov    fr12, fr9
    fmul    fr1, fr9
    fadd    fr9, fr15              ! Y3

    mov.l   .Lnav_project_distance, r0
    fmov.s  @r0, fr9
    fadd    fr13, fr9              ! positive camera Z; audited minimum > 3.68
    mov.l   .Lnav_project_one, r0
    fmov.s  @r0, fr13
    fdiv    fr9, fr13              ! reciprocal depth
    mov.l   .Lnav_project_scale, r0
    fmov.s  @r0, fr10
    fmul    fr10, fr14
    fmul    fr13, fr14
    mov.l   .Lnav_project_center_x, r0
    fmov.s  @r0, fr11
    fadd    fr11, fr14
    fmul    fr10, fr15
    fmul    fr13, fr15
    fneg    fr15
    mov.l   .Lnav_project_center_y, r0
    fmov.s  @r0, fr11
    fadd    fr11, fr15
    fmov.s  fr14, @r6
    add     #4, r6
    fmov.s  fr15, @r6
    add     #4, r6
    fmov.s  fr13, @r6
    rts
    nop

    .align 2
.Lnav_project_yaw:      .long navigator_yaw
.Lnav_project_pitch:    .long navigator_pitch
.Lnav_project_roll:     .long navigator_roll
.Lnav_project_distance: .long navigator_distance
.Lnav_project_center_x: .long navigator_center_x
.Lnav_project_center_y: .long navigator_center_y
.Lnav_project_one:      .long float_one
.Lnav_project_scale:    .long navigator_projection_scale

! ---------------------------------------------------------------------------
! Cached-vertex and geometric-beam helpers.
! ---------------------------------------------------------------------------

! r4=projected point, r6=TA command, r7=ARGB.
navigator_emit_cached_vertex:
    fmov.s  @r4+, fr12
    fmov.s  @r4+, fr13
    fmov.s  @r4, fr14
    mov     r6, r4
    mov     r7, r5
    mov.l   .Lnav_cached_emit, r0
    jmp     @r0
    nop

    .align 2
.Lnav_cached_emit: .long emit_vertex

! r4/r5=projected endpoints, r6=width-float pointer, r7=ARGB.  A constant
! screen-width quadrilateral is one four-vertex strip = two triangles.
navigator_emit_beam:
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
    mov.l   .Lnav_beam_epsilon, r0
    fmov.s  @r0, fr10
    fadd    fr10, fr8
    fsrra   fr8
    fmul    fr8, fr6
    fmul    fr9, fr6               ! normal Y
    fmul    fr8, fr7
    fmul    fr9, fr7
    fneg    fr7                    ! normal X

    fmov    fr0, fr12
    fadd    fr7, fr12
    fmov    fr1, fr13
    fadd    fr6, fr13
    fmov    fr2, fr14
    mov.l   .Lnav_beam_normal, r4
    mov     r11, r5
    mov.l   .Lnav_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr0, fr12
    fsub    fr7, fr12
    fmov    fr1, fr13
    fsub    fr6, fr13
    fmov    fr2, fr14
    mov.l   .Lnav_beam_normal, r4
    mov     r11, r5
    mov.l   .Lnav_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr3, fr12
    fadd    fr7, fr12
    fmov    fr4, fr13
    fadd    fr6, fr13
    fmov    fr5, fr14
    mov.l   .Lnav_beam_normal, r4
    mov     r11, r5
    mov.l   .Lnav_beam_emit, r0
    jsr     @r0
    nop
    fmov    fr3, fr12
    fsub    fr7, fr12
    fmov    fr4, fr13
    fsub    fr6, fr13
    fmov    fr5, fr14
    mov.l   .Lnav_beam_eol, r4
    mov     r11, r5
    mov.l   .Lnav_beam_emit, r0
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
.Lnav_beam_emit:   .long emit_vertex
.Lnav_beam_epsilon:.long navigator_beam_epsilon
.Lnav_beam_normal: .long CMD_VERTEX
.Lnav_beam_eol:    .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Opaque lower shell: six closed radial strips, 48 triangles each.
! ---------------------------------------------------------------------------

draw_navigator_lower_bowl:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lnav_bowl_cache, r8
    mov     #6, r10
    mov     #0, r11

.Lnav_bowl_band:
    mov     r11, r0
    and     #3, r0
    shll2   r0
    mov.l   .Lnav_bowl_palette, r1
    mov.l   @(r0, r1), r7
    mov     r8, r12
    mov.l   .Lnav_bowl_stride, r0
    add     r0, r12                ! next radial ring
    mov     r8, r13
    mov     r12, r14
    mov     #24, r9

.Lnav_bowl_segment:
    mov     r13, r4
    mov.l   .Lnav_bowl_normal, r6
    mov.l   .Lnav_bowl_emit, r0
    jsr     @r0
    nop
    mov     r14, r4
    mov.l   .Lnav_bowl_normal, r6
    mov.l   .Lnav_bowl_emit, r0
    jsr     @r0
    nop
    add     #12, r13
    add     #12, r14
    dt      r9
    bf      .Lnav_bowl_segment
    nop

    ! Repeat segment zero to close this one strip.
    mov     r8, r4
    mov.l   .Lnav_bowl_normal, r6
    mov.l   .Lnav_bowl_emit, r0
    jsr     @r0
    nop
    mov     r12, r4
    mov.l   .Lnav_bowl_eol, r6
    mov.l   .Lnav_bowl_emit, r0
    jsr     @r0
    nop

    mov     r12, r8
    add     #1, r11
    dt      r10
    bf      .Lnav_bowl_band
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
.Lnav_bowl_cache:  .long navigator_lower_projected
.Lnav_bowl_palette:.long navigator_bowl_palette
.Lnav_bowl_emit:   .long navigator_emit_cached_vertex
.Lnav_bowl_stride: .long 288
.Lnav_bowl_normal: .long CMD_VERTEX
.Lnav_bowl_eol:    .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Upper diagnostic topology.
! ---------------------------------------------------------------------------

! 24 meridians x 12 radial intervals = 288 beams.
draw_navigator_meridians:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lnav_meridian_cache, r8
    mov     #24, r10
    mov     #0, r11

.Lnav_meridian_outer:
    mov     r8, r12
    mov     #12, r13
    mov     r11, r0
    and     #3, r0
    shll2   r0
    mov.l   .Lnav_meridian_palette, r1
    mov.l   @(r0, r1), r14
.Lnav_meridian_inner:
    mov     r12, r4
    mov     r12, r5
    mov.l   .Lnav_meridian_stride, r0
    add     r0, r5
    mov.l   .Lnav_meridian_width, r6
    mov     r14, r7
    mov.l   .Lnav_meridian_beam, r0
    jsr     @r0
    nop
    mov.l   .Lnav_meridian_stride, r0
    add     r0, r12
    dt      r13
    bf      .Lnav_meridian_inner
    nop
    add     #12, r8
    add     #1, r11
    dt      r10
    bf      .Lnav_meridian_outer
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
.Lnav_meridian_cache:  .long navigator_upper_projected
.Lnav_meridian_palette:.long navigator_meridian_palette
.Lnav_meridian_width:  .long navigator_wire_width
.Lnav_meridian_beam:   .long navigator_emit_beam
.Lnav_meridian_stride: .long 288

! Rings 1..12.  One ring is promoted to mint-white every sixteen frames,
! producing an inspection scanner that travels over the hull without adding
! any geometry or changing the fixed TA budget.
draw_navigator_rings:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lnav_ring_cache, r8
    mov.l   .Lnav_ring_stride, r0
    add     r0, r8                 ! omit degenerate center ring
    mov     r9, r14
    shlr2   r14
    shlr2   r14                    ! frame / 16
.Lnav_ring_scanner_reduce:
    mov     #12, r0
    cmp/hs  r0, r14
    bf      .Lnav_ring_scanner_ready
    nop
    add     #-12, r14
    bra     .Lnav_ring_scanner_reduce
    nop
.Lnav_ring_scanner_ready:
    add     #1, r14
    mov     #12, r10
    mov     #1, r11

.Lnav_ring_outer:
    mov.l   .Lnav_ring_color, r7
    mov.l   .Lnav_ring_width, r6
    cmp/eq  r14, r11
    bf      .Lnav_ring_style_ready
    nop
    mov.l   .Lnav_ring_scan_color, r7
    mov.l   .Lnav_ring_scan_width, r6
.Lnav_ring_style_ready:
    mov     r8, r12
    mov     #23, r13
.Lnav_ring_inner:
    mov     r12, r4
    mov     r12, r5
    add     #12, r5
    mov.l   .Lnav_ring_beam, r0
    jsr     @r0
    nop
    add     #12, r12
    dt      r13
    bf      .Lnav_ring_inner
    nop
    mov     r12, r4               ! segment 23 -> segment 0
    mov     r8, r5
    mov.l   .Lnav_ring_beam, r0
    jsr     @r0
    nop

    mov.l   .Lnav_ring_stride, r0
    add     r0, r8
    add     #1, r11
    dt      r10
    bf      .Lnav_ring_outer
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
.Lnav_ring_cache:      .long navigator_upper_projected
.Lnav_ring_beam:       .long navigator_emit_beam
.Lnav_ring_width:      .long navigator_wire_width
.Lnav_ring_scan_width: .long navigator_scanner_width
.Lnav_ring_color:      .long 0xff00582c
.Lnav_ring_scan_color: .long 0xff68ffc0
.Lnav_ring_stride:     .long 288

! One diagonal in alternating cells: even bands slope forward, odd bands
! backward.  Together with rings and meridians this reads as a triangulated
! surface rather than a stack of hoops.
draw_navigator_diagonals:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lnav_diagonal_cache, r8
    mov     #12, r10
    mov     #0, r11

.Lnav_diagonal_band:
    mov     r8, r12
    mov.l   .Lnav_diagonal_stride, r0
    add     r0, r12                ! next ring base
    mov     r8, r13                ! A pointer
    mov     r12, r14               ! B pointer
    mov     r11, r0
    and     #1, r0
    tst     r0, r0
    bf      .Lnav_diagonal_odd
    nop
    add     #12, r14               ! even: A[j] -> B[j+1]
    bra     .Lnav_diagonal_pointers_ready
    nop
.Lnav_diagonal_odd:
    add     #12, r13               ! odd: A[j] -> B[j-1]
.Lnav_diagonal_pointers_ready:
    mov     #12, r9
.Lnav_diagonal_inner:
    mov     r13, r4
    mov     r14, r5
    mov.l   .Lnav_diagonal_width, r6
    mov.l   .Lnav_diagonal_color, r7
    mov.l   .Lnav_diagonal_beam, r0
    jsr     @r0
    nop
    add     #24, r13
    add     #24, r14
    dt      r9
    bf      .Lnav_diagonal_inner
    nop
    mov     r12, r8
    add     #1, r11
    dt      r10
    bf      .Lnav_diagonal_band
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
.Lnav_diagonal_cache: .long navigator_upper_projected
.Lnav_diagonal_beam:  .long navigator_emit_beam
.Lnav_diagonal_width: .long navigator_diagonal_width
.Lnav_diagonal_color: .long 0xff003c24
.Lnav_diagonal_stride:.long 288

! ---------------------------------------------------------------------------
! Sparse underbody wire: every other meridian plus rings 2, 4, and 6.
! ---------------------------------------------------------------------------

draw_navigator_lower_wire:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    ! Twelve lower meridians, six intervals each.
    mov.l   .Lnav_lower_wire_cache, r8
    mov     #12, r10
.Lnav_lower_meridian_outer:
    mov     r8, r12
    mov     #6, r13
.Lnav_lower_meridian_inner:
    mov     r12, r4
    mov     r12, r5
    mov.l   .Lnav_lower_wire_stride, r0
    add     r0, r5
    mov.l   .Lnav_lower_wire_width, r6
    mov.l   .Lnav_lower_wire_color, r7
    mov.l   .Lnav_lower_wire_beam, r0
    jsr     @r0
    nop
    mov.l   .Lnav_lower_wire_stride, r0
    add     r0, r12
    dt      r13
    bf      .Lnav_lower_meridian_inner
    nop
    add     #24, r8                ! skip alternate azimuth segment
    dt      r10
    bf      .Lnav_lower_meridian_outer
    nop

    ! Closed contour rings 2, 4, and 6.
    mov.l   .Lnav_lower_wire_cache, r8
    mov.l   .Lnav_lower_ring_start, r0
    add     r0, r8
    mov     #3, r10
.Lnav_lower_ring_outer:
    mov     r8, r12
    mov     #23, r13
.Lnav_lower_ring_inner:
    mov     r12, r4
    mov     r12, r5
    add     #12, r5
    mov.l   .Lnav_lower_wire_width, r6
    mov.l   .Lnav_lower_ring_color, r7
    mov.l   .Lnav_lower_wire_beam, r0
    jsr     @r0
    nop
    add     #12, r12
    dt      r13
    bf      .Lnav_lower_ring_inner
    nop
    mov     r12, r4
    mov     r8, r5
    mov.l   .Lnav_lower_wire_width, r6
    mov.l   .Lnav_lower_ring_color, r7
    mov.l   .Lnav_lower_wire_beam, r0
    jsr     @r0
    nop
    mov.l   .Lnav_lower_ring_step, r0
    add     r0, r8
    dt      r10
    bf      .Lnav_lower_ring_outer
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
.Lnav_lower_wire_cache: .long navigator_lower_projected
.Lnav_lower_wire_beam:  .long navigator_emit_beam
.Lnav_lower_wire_width: .long navigator_lower_wire_width
.Lnav_lower_wire_color: .long 0xff002c20
.Lnav_lower_ring_color: .long 0xff005038
.Lnav_lower_wire_stride:.long 288
.Lnav_lower_ring_start: .long 576
.Lnav_lower_ring_step:  .long 576

! Three full front-to-back dorsal paths are six radial half-ribs.  Their index
! pairs are central (0/12), starboard (2/10), and port (22/14).  The central
! pair visibly follows the depressed front cleft before climbing the crown.
draw_navigator_dorsal_ribs:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lnav_rib_indices, r8
    mov.l   .Lnav_rib_palette, r14
    mov     #6, r10
    mov     #0, r11

.Lnav_rib_outer:
    mov.b   @r8+, r0
    extu.b  r0, r0
    mov     r0, r1
    shll2   r0
    shll    r1
    shll2   r1
    add     r1, r0                 ! segment index * 12
    mov.l   .Lnav_rib_cache, r12
    add     r0, r12
    mov     r11, r0
    shll2   r0
    mov.l   @(r0, r14), r9
    mov     #12, r13
.Lnav_rib_inner:
    mov     r12, r4
    mov     r12, r5
    mov.l   .Lnav_rib_stride, r0
    add     r0, r5
    mov.l   .Lnav_rib_width, r6
    mov     r9, r7
    mov.l   .Lnav_rib_beam, r0
    jsr     @r0
    nop
    mov.l   .Lnav_rib_stride, r0
    add     r0, r12
    dt      r13
    bf      .Lnav_rib_inner
    nop
    add     #1, r11
    dt      r10
    bf      .Lnav_rib_outer
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
.Lnav_rib_cache:  .long navigator_upper_projected
.Lnav_rib_indices:.long navigator_dorsal_indices
.Lnav_rib_palette:.long navigator_dorsal_palette
.Lnav_rib_width:  .long navigator_dorsal_width
.Lnav_rib_beam:   .long navigator_emit_beam
.Lnav_rib_stride: .long 288

! ---------------------------------------------------------------------------
! Visual tuning and generated-object constants.
! ---------------------------------------------------------------------------

    .section .rodata,"a"
    .align 5

! Native four-vertex strip: almost-black green CRT field with a cyan-black
! lower corner.  It costs two triangles and gives additive phosphor headroom.
navigator_backdrop_vertices:
    .long CMD_VERTEX,     0x00000000, 0x43f00000, 0x3a83126f
    .long 0, 0, 0xff00100c, 0
    .long CMD_VERTEX,     0x00000000, 0x00000000, 0x3a83126f
    .long 0, 0, 0xff020806, 0
    .long CMD_VERTEX,     0x44200000, 0x43f00000, 0x3a83126f
    .long 0, 0, 0xff001814, 0
    .long CMD_VERTEX_EOL, 0x44200000, 0x00000000, 0x3a83126f
    .long 0, 0, 0xff06100e, 0

navigator_meridian_palette:
    .long 0xff00683c,0xff007848,0xff005838,0xff008050
navigator_bowl_palette:
    .long 0xff03130f,0xff051a15,0xff04100f,0xff061813
navigator_dorsal_palette:
    .long 0xff78ffd0,0xff48dca8
    .long 0xff20b888,0xff20b888
    .long 0xff18a878,0xff18a878
navigator_dorsal_indices:
    .byte 0,12,2,10,22,14
    .align 2

navigator_upper_ring_inverse: .long 0x3daaaaab ! 1/12
navigator_lower_ring_inverse: .long 0x3e2aaaab ! 1/6
navigator_width:              .long 0x40333333 ! 2.80
navigator_width_asymmetry:    .long 0x3e3851ec ! 0.18
navigator_length:             .long 0x40166666 ! 2.35
navigator_length_asymmetry:   .long 0x3eb851ec ! 0.36
navigator_upper_height:       .long 0x3faccccd ! 1.35
navigator_upper_lobe:         .long 0xbee66666 ! -0.45, lifts the paired shoulder lobes
navigator_upper_front_bias:   .long 0x3e3851ec ! 0.18
navigator_front_cleft:        .long 0x3fe00000 ! 1.75
navigator_lower_height:       .long 0x3f1eb852 ! 0.62
navigator_lower_front_bias:   .long 0x3da3d70a ! 0.08

navigator_projection_scale:   .long 0x43d70000 ! 430.0
navigator_distance_base:      .long 0x40d33333 ! 6.60
navigator_distance_amplitude: .long 0x3ec28f5c ! 0.38
navigator_center_x_base:      .long 0x43a00000 ! 320.0
navigator_center_x_amplitude: .long 0x41000000 ! 8.0 pixels
navigator_center_y_base:      .long 0x43600000 ! 224.0
navigator_center_y_amplitude: .long 0x40800000 ! 4.0 pixels
navigator_pitch_units:        .long 0x45866000 ! 4300 binary-angle units
navigator_pitch_bias:         .long 0xc5480000 ! -3200 binary-angle units
navigator_roll_units:         .long 0x44960000 ! 1200 binary-angle units

navigator_wire_width:         .long 0x3f8ccccd ! 1.10 pixels
navigator_diagonal_width:     .long 0x3f4ccccd ! 0.80 pixels
navigator_lower_wire_width:   .long 0x3f666666 ! 0.90 pixels
navigator_scanner_width:      .long 0x3fe66666 ! 1.80 pixels
navigator_dorsal_width:       .long 0x40100000 ! 2.25 pixels
navigator_beam_epsilon:       .long 0x3e800000 ! 0.25 pixel squared

    .section .bss,"aw",@nobits
    .align 5
navigator_yaw:             .space 8
navigator_pitch:           .space 8
navigator_roll:            .space 8
navigator_distance:        .space 4
navigator_center_x:        .space 4
navigator_center_y:        .space 4
navigator_radial_t:        .space 4
navigator_radial_envelope: .space 4
navigator_scratch_x:       .space 4
navigator_scratch_z:       .space 4
navigator_build_angle:     .space 4
    .align 5
navigator_upper_projected: .space 13 * 24 * 12
navigator_lower_projected: .space 7 * 24 * 12
