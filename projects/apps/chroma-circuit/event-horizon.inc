! ---------------------------------------------------------------------------
! CHROMA CIRCUIT -- Scene 09: EVENT HORIZON
!
! A gravitationally lensed black hole.  Nothing here is a texture, sprite, or
! precomputed image: SH-4 builds an accretion disk every frame and bends every
! one of its vertices, plus every background star, through the point-mass lens
! equation before the PowerVR2 draws a single triangle.
!
! For a source that appears at angular offset beta from the hole, a point lens
! produces two images at
!
!      theta(+/-) = ( beta +/- sqrt(beta^2 + 4 thetaE^2) ) / 2
!
! where thetaE^2 = k * M * D_ls / (D_l * D_s).  Because the disk is a mesh, the
! same vertices are submitted twice: the primary image lifts the far side of
! the disk into the arch that appears to stand above the shadow, and the
! secondary image (theta-, on the opposite side of the hole) draws the thinner
! reflected arc beneath it.  Surface brightness is conserved by lensing, so the
! two images legitimately share one Gouraud color.  Near-side vertices sit in
! front of the opaque shadow disc in the reciprocal-Z buffer and pass over it;
! far-side vertices that the lens cannot lift clear of the shadow are hidden by
! it.  The mesh is generated in camera-relative azimuth, densified behind the
! hole, so the Einstein arch stays smooth even when the camera dips through the
! disk plane.
!
! Colour is analytic: a radial temperature ramp, Keplerian differential
! rotation of two turbulence harmonics, and Doppler beaming that brightens the
! approaching limb.  Stars are lensed by the same equation with the correct
! point-source magnification and are stretched tangentially as they pass the
! Einstein radius.  The act ends with the mass parameter growing until the
! horizon swallows the frame.
!
! Fixed geometry, excluding the shared title / transition:
!      2  backdrop triangles
!     46  opaque shadow-disc triangles
!   1920  primary disk-image triangles   (10 closed bands x 96 segments)
!   1920  secondary disk-image triangles
!    128  photon-ring triangles
!    640  star diamonds (320 x 2)
!   ----
!   4656  triangles / 5,346 native parameter blocks at the scene peak.
!
! Writable scene state is 33,792 bytes of projected disk vertices, 5,120 bytes
! of star directions, and under 256 bytes of pose data.
! ---------------------------------------------------------------------------

    .section .text,"ax"
    .align 2

    .equ EH_SEGMENTS,      96
    .equ EH_RINGS,         11
    .equ EH_VERTEX_BYTES,  32
    .equ EH_RING_BYTES,    EH_SEGMENTS * EH_VERTEX_BYTES
    .equ EH_STARS,         320

! r9 is the scene-local frame (0..767).  Preserve it for the shared transition.
draw_scene_event_horizon:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov.l   .Leh_scene_camera, r0
    jsr     @r0
    nop
    mov.l   .Leh_scene_rings, r0
    jsr     @r0
    nop
    mov.l   .Leh_scene_build, r0
    jsr     @r0
    nop

    ! Opaque pass: deep-space backdrop, the event-horizon shadow disc, and the
    ! shared HUD.  The disc writes depth 1/D so the reciprocal-Z buffer sorts
    ! the near disk in front of it and the lensed far disk behind it.
    mov.l   .Leh_scene_opaque, r4
    mov.l   .Leh_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Leh_scene_backdrop, r8
    mov     #4, r10
.Leh_backdrop_loop:
    mov     r8, r4
    mov.l   .Leh_scene_submit, r0
    jsr     @r0
    nop
    add     #32, r8
    dt      r10
    bf      .Leh_backdrop_loop
    nop
    mov.l   .Leh_scene_shadow, r0
    jsr     @r0
    nop
    mov.l   .Leh_scene_title, r0
    jsr     @r0
    nop
    mov.l   .Leh_scene_zero, r4
    mov.l   .Leh_scene_submit, r0
    jsr     @r0
    nop

    ! Additive pass: both lens images of the disk, the photon ring, and the
    ! lensed starfield.  ONE/ONE blending lets the arch and the near disk
    ! overlap without any CPU-side sorting.
    mov.l   .Leh_scene_additive, r4
    mov.l   .Leh_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Leh_scene_images, r0
    jsr     @r0
    nop
    mov.l   .Leh_scene_ring, r0
    jsr     @r0
    nop
    mov.l   .Leh_scene_stars, r0
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
.Leh_scene_camera:   .long eh_prepare_camera
.Leh_scene_rings:    .long eh_prepare_rings
.Leh_scene_build:    .long eh_build_disk
.Leh_scene_shadow:   .long eh_draw_shadow
.Leh_scene_images:   .long eh_draw_disk_images
.Leh_scene_ring:     .long eh_draw_photon_ring
.Leh_scene_stars:    .long eh_draw_stars
.Leh_scene_submit:   .long ta_submit
.Leh_scene_title:    .long draw_title
.Leh_scene_opaque:   .long opaque_header
.Leh_scene_additive: .long additive_header
.Leh_scene_zero:     .long zero_block
.Leh_scene_backdrop: .long eh_backdrop_vertices

! ---------------------------------------------------------------------------
! Eased timeline helper: fr4 = t, fr5 = t0, fr6 = 1/length.
! Returns fr0 = smoothstep(clamp((t - t0) / length)).  Clobbers fr1..fr4.
! ---------------------------------------------------------------------------

eh_smooth:
    fsub    fr5, fr4
    fmul    fr6, fr4
    fldi0   fr0
    fcmp/gt fr0, fr4
    bt      .Leh_smooth_positive
    rts
    nop
.Leh_smooth_positive:
    fldi1   fr0
    fcmp/gt fr4, fr0
    bt      .Leh_smooth_interior
    rts                             ! saturated at 1.0
    nop
.Leh_smooth_interior:
    fldi1   fr2
    fldi1   fr3
    fadd    fr3, fr2
    fadd    fr3, fr2                ! 3
    fmov    fr4, fr1
    fadd    fr4, fr1                ! 2u
    fsub    fr1, fr2                ! 3 - 2u
    fmov    fr4, fr0
    fmul    fr4, fr0                ! u^2
    fmul    fr2, fr0
    rts
    nop

! ---------------------------------------------------------------------------
! Camera choreography.
!
! 0..200   approach from D=60 to D=26 at 14 degrees above the disk plane
! 180..420 the inclination dips through the plane to -12 degrees and back,
!          swapping the arch from above the shadow to below it
! 420..680 climb to a 34 degree overview while a slow roll tilts the frame
! 570..718 plunge to D=14; from 636 the mass grows 4.5x so the horizon and the
!          lens both swell until the shadow covers the entire screen at 728,
!          just before the shared exit flash begins at 736
! ---------------------------------------------------------------------------

eh_prepare_camera:
    sts.l   pr, @-r15
    lds     r9, fpul
    float   fpul, fr7               ! t as float, preserved across helpers

    ! Distance.
    fmov    fr7, fr4
    fldi0   fr5
    mov.l   .Leh_cam_inv200, r0
    lds     r0, fpul
    fsts    fpul, fr6
    bsr     eh_smooth
    nop
    mov.l   .Leh_cam_approach, r0
    lds     r0, fpul
    fsts    fpul, fr8
    fmul    fr0, fr8
    mov.l   .Leh_cam_far, r0
    lds     r0, fpul
    fsts    fpul, fr9
    fadd    fr8, fr9                ! 60 - 34 s
    fmov    fr7, fr4
    mov.l   .Leh_cam_t620, r0
    lds     r0, fpul
    fsts    fpul, fr5
    mov.l   .Leh_cam_inv148, r0
    lds     r0, fpul
    fsts    fpul, fr6
    bsr     eh_smooth
    nop
    mov.l   .Leh_cam_plunge, r0
    lds     r0, fpul
    fsts    fpul, fr8
    fmul    fr0, fr8
    fadd    fr8, fr9                ! - 12 s
    mov.l   .Leh_cam_dist, r0
    fmov.s  fr9, @r0
    fldi1   fr8
    fdiv    fr9, fr8
    mov.l   .Leh_cam_inv_dist, r0
    fmov.s  fr8, @r0
    mov.l   .Leh_cam_ring_lift, r0
    lds     r0, fpul
    fsts    fpul, fr10
    fmul    fr10, fr8
    mov.l   .Leh_cam_ring_depth, r0
    fmov.s  fr8, @r0

    ! Inclination in degrees: 14 - 26 dip + 20 climb.
    fmov    fr7, fr4
    mov.l   .Leh_cam_t180, r0
    lds     r0, fpul
    fsts    fpul, fr5
    mov.l   .Leh_cam_inv120, r0
    lds     r0, fpul
    fsts    fpul, fr6
    bsr     eh_smooth
    nop
    fmov    fr0, fr10
    fmov    fr7, fr4
    mov.l   .Leh_cam_t300, r0
    lds     r0, fpul
    fsts    fpul, fr5
    mov.l   .Leh_cam_inv120, r0
    lds     r0, fpul
    fsts    fpul, fr6
    bsr     eh_smooth
    nop
    fsub    fr0, fr10               ! dip 0..1..0
    mov.l   .Leh_cam_dip_deg, r0
    lds     r0, fpul
    fsts    fpul, fr11
    fmul    fr10, fr11
    mov.l   .Leh_cam_base_deg, r0
    lds     r0, fpul
    fsts    fpul, fr10
    fadd    fr11, fr10
    fmov    fr7, fr4
    mov.l   .Leh_cam_t420, r0
    lds     r0, fpul
    fsts    fpul, fr5
    mov.l   .Leh_cam_inv260, r0
    lds     r0, fpul
    fsts    fpul, fr6
    bsr     eh_smooth
    nop
    mov.l   .Leh_cam_climb_deg, r0
    lds     r0, fpul
    fsts    fpul, fr11
    fmul    fr0, fr11
    fadd    fr11, fr10
    mov.l   .Leh_cam_deg_units, r0
    lds     r0, fpul
    fsts    fpul, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    fsca    fpul, dr0
    mov.l   .Leh_cam_sin_i, r0
    fmov.s  fr0, @r0
    mov.l   .Leh_cam_cos_i, r0
    fmov.s  fr1, @r0

    ! Orbit azimuth: 0.9 turn per act, integer binary angle.
    mov     r9, r0
    mov     #77, r1
    mul.l   r1, r0
    sts     macl, r0
    mov.l   .Leh_cam_azimuth, r1
    mov.l   r0, @r1

    ! Roll: 6 deg sin(1.5 turns) plus a 22 degree tilt during the plunge.
    mov     r9, r0
    shll8   r0
    shlr    r0                      ! frame * 128
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Leh_cam_roll_deg, r0
    lds     r0, fpul
    fsts    fpul, fr10
    fmul    fr0, fr10
    fmov    fr7, fr4
    mov.l   .Leh_cam_t660, r0
    lds     r0, fpul
    fsts    fpul, fr5
    mov.l   .Leh_cam_inv108, r0
    lds     r0, fpul
    fsts    fpul, fr6
    bsr     eh_smooth
    nop
    mov.l   .Leh_cam_tilt_deg, r0
    lds     r0, fpul
    fsts    fpul, fr11
    fmul    fr0, fr11
    fadd    fr11, fr10
    mov.l   .Leh_cam_deg_units, r0
    lds     r0, fpul
    fsts    fpul, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    sts     fpul, r0
    mov.l   .Leh_cam_roll_int, r1
    mov.l   r0, @r1
    fsca    fpul, dr0
    mov.l   .Leh_cam_sin_roll, r0
    fmov.s  fr0, @r0
    mov.l   .Leh_cam_cos_roll, r0
    fmov.s  fr1, @r0

    ! Mass growth and the disk fade of the final collapse.
    fmov    fr7, fr4
    mov.l   .Leh_cam_t690, r0
    lds     r0, fpul
    fsts    fpul, fr5
    mov.l   .Leh_cam_inv78, r0
    lds     r0, fpul
    fsts    fpul, fr6
    bsr     eh_smooth
    nop
    fmul    fr0, fr0
    mov.l   .Leh_cam_mass_gain, r0
    lds     r0, fpul
    fsts    fpul, fr10
    fmul    fr0, fr10
    fldi1   fr11
    fadd    fr11, fr10              ! mass
    mov.l   .Leh_cam_mass, r0
    fmov.s  fr10, @r0
    ! The shadow's depth advances with the mass, so the growing horizon also
    ! engulfs the near half of the disk instead of letting it float in front.
    fldi1   fr11
    fmov    fr10, fr12
    fsub    fr11, fr12
    mov.l   .Leh_cam_engulf, r0
    lds     r0, fpul
    fsts    fpul, fr13
    fmul    fr13, fr12
    fadd    fr11, fr12              ! 1 + 8 (M - 1)
    mov.l   .Leh_cam_inv_dist, r0
    fmov.s  @r0, fr13
    fmul    fr12, fr13
    fmov.s  fr13, @r0
    mov.l   .Leh_cam_ring_lift, r0
    lds     r0, fpul
    fsts    fpul, fr12
    fmul    fr12, fr13
    mov.l   .Leh_cam_ring_depth, r0
    fmov.s  fr13, @r0
    fmov    fr7, fr4
    mov.l   .Leh_cam_t700, r0
    lds     r0, fpul
    fsts    fpul, fr5
    mov.l   .Leh_cam_inv68, r0
    lds     r0, fpul
    fsts    fpul, fr6
    bsr     eh_smooth
    nop
    fldi1   fr11
    fsub    fr0, fr11
    mov.l   .Leh_cam_fade, r0
    fmov.s  fr11, @r0

    ! Lens strength k M / D, star lens (sources at a finite distance behind the
    ! hole), and the projected shadow radius 560 * 2.3 * M / D.
    mov.l   .Leh_cam_dist, r0
    fmov.s  @r0, fr9
    mov.l   .Leh_cam_lens_k, r0
    lds     r0, fpul
    fsts    fpul, fr11
    fmul    fr10, fr11
    fdiv    fr9, fr11
    mov.l   .Leh_cam_lens, r0
    fmov.s  fr11, @r0
    mov.l   .Leh_cam_half, r0
    lds     r0, fpul
    fsts    fpul, fr12
    fmul    fr12, fr11
    mov.l   .Leh_cam_star_lens, r0
    fmov.s  fr11, @r0
    mov.l   .Leh_cam_shadow_scale, r0
    lds     r0, fpul
    fsts    fpul, fr11
    fmul    fr10, fr11
    fdiv    fr9, fr11
    mov.l   .Leh_cam_shadow_px, r0
    fmov.s  fr11, @r0
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Leh_cam_dist:        .long eh_dist
.Leh_cam_inv_dist:    .long eh_inv_dist
.Leh_cam_ring_depth:  .long eh_ring_depth
.Leh_cam_sin_i:       .long eh_sin_i
.Leh_cam_cos_i:       .long eh_cos_i
.Leh_cam_sin_roll:    .long eh_sin_roll
.Leh_cam_cos_roll:    .long eh_cos_roll
.Leh_cam_roll_int:    .long eh_roll_int
.Leh_cam_azimuth:     .long eh_azimuth
.Leh_cam_mass:        .long eh_mass
.Leh_cam_fade:        .long eh_fade
.Leh_cam_lens:        .long eh_lens
.Leh_cam_star_lens:   .long eh_star_lens
.Leh_cam_shadow_px:   .long eh_shadow_px
.Leh_cam_inv200:      .long 0x3ba3d70a  ! 1/200
.Leh_cam_inv148:      .long 0x3bdd67c9  ! 1/148
.Leh_cam_inv120:      .long 0x3c088889  ! 1/120
.Leh_cam_inv260:      .long 0x3b7c1f08  ! 1/260
.Leh_cam_inv108:      .long 0x3c17b426  ! 1/108
.Leh_cam_inv78:       .long 0x3c321643  ! 1/92
.Leh_cam_inv68:       .long 0x3c579436  ! 1/76
.Leh_cam_t180:        .long 0x43340000  ! 180
.Leh_cam_t300:        .long 0x43960000  ! 300
.Leh_cam_t420:        .long 0x43d20000  ! 420
.Leh_cam_t620:        .long 0x440e8000  ! 570
.Leh_cam_t660:        .long 0x44188000  ! 610
.Leh_cam_t690:        .long 0x441f0000  ! 636
.Leh_cam_t700:        .long 0x44228000  ! 650
.Leh_cam_far:         .long 0x42700000  ! 60
.Leh_cam_approach:    .long 0xc2080000  ! -34
.Leh_cam_plunge:      .long 0xc1400000  ! -12
.Leh_cam_base_deg:    .long 0x41600000  ! 14
.Leh_cam_dip_deg:     .long 0xc1d00000  ! -26
.Leh_cam_climb_deg:   .long 0x41a00000  ! 20
.Leh_cam_roll_deg:    .long 0x40c00000  ! 6
.Leh_cam_tilt_deg:    .long 0x41b00000  ! 22
.Leh_cam_deg_units:   .long 0x43360b61  ! 65536/360
.Leh_cam_mass_gain:   .long 0x40600000  ! 3.5
.Leh_cam_lens_k:      .long 0x400ccccd  ! 2.2
.Leh_cam_half:        .long 0x3f000000  ! 0.5
.Leh_cam_shadow_scale:.long 0x44a10000  ! 560 * 2.3
.Leh_cam_ring_lift:   .long 0x3f80418a  ! 1.001
.Leh_cam_engulf:      .long 0x41000000  ! 8

! ---------------------------------------------------------------------------
! Per-ring, per-frame values: turbulence phases and the Doppler coefficient.
! ---------------------------------------------------------------------------

eh_prepare_rings:
    mov.l   .Leh_ring_table, r1     ! rodata: r, sqrt(0.5/r), omega, R, G, B
    mov.l   .Leh_ring_state, r2     ! bss: phase3, phase5, vc, pad
    mov.l   .Leh_ring_azimuth, r0
    mov.l   @r0, r3
    mov.l   .Leh_ring_cos_i, r0
    fmov.s  @r0, fr1
    mov     #EH_RINGS, r5
    mov     #0, r6                  ! ring index
.Leh_ring_loop:
    ! base = azimuth - omega * frame (Keplerian pattern rotation)
    mov     r1, r0
    add     #8, r0
    mov.l   @r0, r0
    mul.l   r9, r0
    sts     macl, r0
    mov     r3, r4
    sub     r0, r4
    ! phase3 = 3 base + k * 10640
    mov     r4, r0
    shll    r0
    add     r4, r0
    mov.l   .Leh_ring_off3, r7
    mul.l   r6, r7
    sts     macl, r7
    add     r7, r0
    mov.l   r0, @r2
    ! phase5 = 5 base - k * 13770
    mov     r4, r0
    shll2   r0
    add     r4, r0
    mov.l   .Leh_ring_off5, r7
    mul.l   r6, r7
    sts     macl, r7
    sub     r7, r0
    mov     r2, r7
    add     #4, r7
    mov.l   r0, @r7
    ! vc = sqrt(0.5/r) cos i
    mov     r1, r0
    add     #4, r0
    fmov.s  @r0, fr0
    fmul    fr1, fr0
    add     #4, r7
    fmov.s  fr0, @r7
    add     #24, r1
    add     #16, r2
    add     #1, r6
    dt      r5
    bf      .Leh_ring_loop
    nop
    rts
    nop

    .align 2
.Leh_ring_table:   .long eh_ring_constants
.Leh_ring_state:   .long eh_ring_state
.Leh_ring_azimuth: .long eh_azimuth
.Leh_ring_cos_i:   .long eh_cos_i
.Leh_ring_off3:    .long 10640
.Leh_ring_off5:    .long 13770

! ---------------------------------------------------------------------------
! Disk construction: 11 rings x 96 segments, projected through the lens.
! ---------------------------------------------------------------------------

eh_build_disk:
    sts.l   pr, @-r15
    mov.l   .Leh_build_cache, r8
    mov.l   .Leh_build_table, r10
    mov.l   .Leh_build_state, r11
    mov     #EH_RINGS, r13
.Leh_build_ring:
    mov     #0, r12                 ! uniform parameter alpha
    mov     #EH_SEGMENTS, r14
.Leh_build_segment:
    mov.l   .Leh_build_project, r0
    jsr     @r0
    nop
    mov.l   .Leh_build_shade, r0
    jsr     @r0
    nop
    add     #EH_VERTEX_BYTES, r8
    mov.l   .Leh_build_alpha_step, r0
    add     r0, r12
    dt      r14
    bf      .Leh_build_segment
    nop
    add     #24, r10
    add     #16, r11
    dt      r13
    bf      .Leh_build_ring
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Leh_build_cache:      .long eh_disk_cache
.Leh_build_table:      .long eh_ring_constants
.Leh_build_state:      .long eh_ring_state
.Leh_build_project:    .long eh_project_vertex
.Leh_build_shade:      .long eh_shade_vertex
.Leh_build_alpha_step: .long 683       ! 65536 / 96

! Inputs: r8 = vertex slot, r10 = ring constants, r12 = alpha (binary angle).
! Writes px, py (primary image), sx, sy (secondary image), and 1/z to the
! slot, and leaves sin(psi), the lens term w, and psi in scratch for shading.
eh_project_vertex:
    ! psi = alpha - 0.85 sin(alpha): angular density concentrates behind the
    ! hole, where the lens stretches the far disk into the Einstein arch.
    lds     r12, fpul
    fsca    fpul, dr0
    lds     r12, fpul
    float   fpul, fr2
    mov.l   .Leh_proj_warp, r0
    lds     r0, fpul
    fsts    fpul, fr3
    fmul    fr0, fr3
    fsub    fr3, fr2
    ftrc    fr2, fpul
    sts     fpul, r7
    mov.l   .Leh_proj_psi, r0
    mov.l   r7, @r0
    lds     r7, fpul
    fsca    fpul, dr0               ! fr0 = sin psi, fr1 = cos psi
    mov.l   .Leh_proj_sin_psi, r0
    fmov.s  fr0, @r0

    ! Camera space with the lens on the optical axis:
    !   x = r sin psi, y = r sin(i) cos psi, z = D + r cos(i) cos psi
    fmov.s  @r10, fr2               ! ring radius
    fmov    fr2, fr4
    fmul    fr0, fr4                ! x
    fmul    fr1, fr2                ! r cos psi
    mov.l   .Leh_proj_sin_i, r0
    fmov.s  @r0, fr6
    fmul    fr2, fr6                ! y
    mov.l   .Leh_proj_cos_i, r0
    fmov.s  @r0, fr7
    fmul    fr2, fr7
    mov.l   .Leh_proj_dist, r0
    fmov.s  @r0, fr3
    fadd    fr3, fr7                ! z (always positive: D > r)

    ! Roll about the optical axis.
    mov.l   .Leh_proj_sin_roll, r0
    fmov.s  @r0, fr8
    mov.l   .Leh_proj_cos_roll, r0
    fmov.s  @r0, fr9
    fmov    fr4, fr10
    fmul    fr9, fr10
    fmov    fr6, fr11
    fmul    fr8, fr11
    fsub    fr11, fr10              ! x cos - y sin
    fmul    fr8, fr4
    fmul    fr9, fr6
    fadd    fr4, fr6                ! x sin + y cos -> fr6

    fldi1   fr12
    fdiv    fr7, fr12               ! 1/z
    mov     r8, r0
    add     #16, r0
    fmov.s  fr12, @r0
    fmul    fr12, fr10              ! bx
    fmul    fr12, fr6               ! by

    ! w = k (z - D) / z for the far half, else zero.
    fmul    fr3, fr12               ! D / z
    fldi1   fr14
    fsub    fr12, fr14
    fldi0   fr13
    fcmp/gt fr13, fr14
    bt      .Leh_proj_far
    fldi0   fr14
    bra     .Leh_proj_lens
    nop
.Leh_proj_far:
    mov.l   .Leh_proj_lens_k, r0
    fmov.s  @r0, fr13
    fmul    fr13, fr14
.Leh_proj_lens:
    mov.l   .Leh_proj_w, r0
    fmov.s  fr14, @r0

    ! theta(+/-)/beta = 0.5 +/- 0.5 sqrt(beta^2 + 4w) / beta
    fmov    fr10, fr13
    fmul    fr10, fr13
    fmov    fr6, fr2
    fmul    fr6, fr2
    fadd    fr2, fr13
    mov.l   .Leh_proj_epsilon, r0
    lds     r0, fpul
    fsts    fpul, fr2
    fadd    fr2, fr13               ! beta^2
    fmov    fr13, fr15
    fsrra   fr15                    ! 1/beta
    fmov    fr14, fr2
    fadd    fr14, fr2
    fadd    fr2, fr2                ! 4w
    fadd    fr13, fr2
    fsqrt   fr2                     ! q
    fmul    fr15, fr2
    mov.l   .Leh_proj_half, r0
    lds     r0, fpul
    fsts    fpul, fr3
    fmul    fr3, fr2                ! h
    fmov    fr3, fr4
    fadd    fr2, fr4                ! primary scale
    fsub    fr2, fr3                ! secondary scale (negative)

    mov.l   .Leh_proj_focal, r0
    lds     r0, fpul
    fsts    fpul, fr5
    fmul    fr5, fr10               ! F bx
    fmul    fr5, fr6                ! F by
    mov.l   .Leh_proj_center_x, r0
    lds     r0, fpul
    fsts    fpul, fr8
    mov.l   .Leh_proj_center_y, r0
    lds     r0, fpul
    fsts    fpul, fr9
    fmov    fr10, fr11
    fmul    fr4, fr11
    fadd    fr8, fr11
    fmov.s  fr11, @r8               ! px
    fmov    fr6, fr11
    fmul    fr4, fr11
    fneg    fr11
    fadd    fr9, fr11
    mov     r8, r0
    add     #4, r0
    fmov.s  fr11, @r0               ! py
    fmul    fr3, fr10
    fadd    fr8, fr10
    add     #4, r0
    fmov.s  fr10, @r0               ! sx
    fmul    fr3, fr6
    fneg    fr6
    fadd    fr9, fr6
    add     #4, r0
    fmov.s  fr6, @r0                ! sy
    rts
    nop

    .align 2
.Leh_proj_warp:     .long 0x460b0700  ! 0.85 * 65536 / 2pi = 8865.75
.Leh_proj_psi:      .long eh_scratch_psi
.Leh_proj_sin_psi:  .long eh_scratch_sin_psi
.Leh_proj_w:        .long eh_scratch_w
.Leh_proj_sin_i:    .long eh_sin_i
.Leh_proj_cos_i:    .long eh_cos_i
.Leh_proj_dist:     .long eh_dist
.Leh_proj_sin_roll: .long eh_sin_roll
.Leh_proj_cos_roll: .long eh_cos_roll
.Leh_proj_lens_k:   .long eh_lens
.Leh_proj_epsilon:  .long 0x2edbe6ff  ! 1e-10
.Leh_proj_half:     .long 0x3f000000
.Leh_proj_focal:    .long 0x440c0000  ! 560
.Leh_proj_center_x: .long 0x43a00000  ! 320
.Leh_proj_center_y: .long 0x436c0000  ! 236

! Inputs: r8 = vertex slot, r10 = ring constants, r11 = ring state, plus the
! scratch left by eh_project_vertex.  Writes the primary and secondary ARGB.
eh_shade_vertex:
    sts.l   pr, @-r15
    mov.l   .Leh_shade_psi, r0
    mov.l   @r0, r7

    ! Two turbulence harmonics ride on differentially rotating phases.
    mov     r7, r0
    shll    r0
    add     r7, r0                  ! 3 psi
    mov.l   @r11, r1
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr2               ! fr2 = sin(3 phi)
    mov     r7, r0
    shll2   r0
    add     r7, r0                  ! 5 psi
    mov     r11, r1
    add     #4, r1
    mov.l   @r1, r1
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr4               ! fr4 = sin(5 phi)
    mov.l   .Leh_shade_amp3, r0
    lds     r0, fpul
    fsts    fpul, fr6
    fmul    fr2, fr6
    mov.l   .Leh_shade_amp5, r0
    lds     r0, fpul
    fsts    fpul, fr7
    fmul    fr4, fr7
    fadd    fr7, fr6
    fldi1   fr7
    fadd    fr7, fr6                ! turbulence 0.4 .. 1.6

    ! Doppler beaming: (1 / (1 - v cos i sin psi))^2, capped.
    mov     r11, r0
    add     #8, r0
    fmov.s  @r0, fr8                ! vc
    mov.l   .Leh_shade_sin_psi, r0
    fmov.s  @r0, fr9
    fmul    fr9, fr8
    fldi1   fr9
    fsub    fr8, fr9
    fldi1   fr8
    fdiv    fr9, fr8
    fmul    fr8, fr8
    mov.l   .Leh_shade_dop_cap, r0
    lds     r0, fpul
    fsts    fpul, fr9
    fcmp/gt fr9, fr8
    bf      .Leh_shade_dop_ok
    fmov    fr9, fr8
.Leh_shade_dop_ok:
    fmul    fr8, fr6
    mov.l   .Leh_shade_fade, r0
    fmov.s  @r0, fr8
    fmul    fr8, fr6                ! gain

    mov     r10, r0
    add     #12, r0
    fmov.s  @r0, fr11
    add     #4, r0
    fmov.s  @r0, fr12
    add     #4, r0
    fmov.s  @r0, fr13
    fmul    fr6, fr11
    fmul    fr6, fr12
    fmul    fr6, fr13
    bsr     eh_pack_rgb
    nop
    mov     r8, r1
    add     #20, r1
    mov.l   r0, @r1

    ! The secondary image fades in with the lens term so the near half, whose
    ! theta- collapses onto the hole, contributes nothing.
    mov.l   .Leh_shade_w, r0
    fmov.s  @r0, fr6
    mov.l   .Leh_shade_sec_gain, r0
    lds     r0, fpul
    fsts    fpul, fr7
    fmul    fr7, fr6
    fldi1   fr7
    fcmp/gt fr7, fr6
    bf      .Leh_shade_sec_ok
    fmov    fr7, fr6
.Leh_shade_sec_ok:
    fmul    fr6, fr11
    fmul    fr6, fr12
    fmul    fr6, fr13
    bsr     eh_pack_rgb
    nop
    mov     r8, r1
    add     #24, r1
    mov.l   r0, @r1
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Leh_shade_psi:      .long eh_scratch_psi
.Leh_shade_sin_psi:  .long eh_scratch_sin_psi
.Leh_shade_w:        .long eh_scratch_w
.Leh_shade_fade:     .long eh_fade
.Leh_shade_amp3:     .long 0x3eb33333  ! 0.35
.Leh_shade_amp5:     .long 0x3e800000  ! 0.25
.Leh_shade_dop_cap:  .long 0x4019999a  ! 2.4
.Leh_shade_sec_gain: .long 0x42200000  ! 40

! fr11/fr12/fr13 = linear RGB (clamped here to 0..1) -> r0 = packed ARGB.
! Clobbers fr0, fr1, fr2, r1, r2.
eh_pack_rgb:
    mov.l   .Leh_pack_255, r1
    lds     r1, fpul
    fsts    fpul, fr1
    fldi1   fr0
    fmov    fr11, fr2
    fcmp/gt fr0, fr2
    bf      .Leh_pack_r_ok
    fmov    fr0, fr2
.Leh_pack_r_ok:
    fmul    fr1, fr2
    ftrc    fr2, fpul
    sts     fpul, r0
    shll8   r0
    shll8   r0
    fmov    fr12, fr2
    fcmp/gt fr0, fr2
    bf      .Leh_pack_g_ok
    fmov    fr0, fr2
.Leh_pack_g_ok:
    fmul    fr1, fr2
    ftrc    fr2, fpul
    sts     fpul, r2
    shll8   r2
    or      r2, r0
    fmov    fr13, fr2
    fcmp/gt fr0, fr2
    bf      .Leh_pack_b_ok
    fmov    fr0, fr2
.Leh_pack_b_ok:
    fmul    fr1, fr2
    ftrc    fr2, fpul
    sts     fpul, r2
    or      r2, r0
    mov.l   .Leh_pack_alpha, r2
    or      r2, r0
    rts
    nop

    .align 2
.Leh_pack_255:   .long 0x437f0000  ! 255
.Leh_pack_alpha: .long 0xff000000

! ---------------------------------------------------------------------------
! Cached-vertex emission.  r4 = slot, r5 = position offset (0 primary /
! 8 secondary), r6 = color offset (20 / 24), r7 = TA command.
! ---------------------------------------------------------------------------

eh_emit_cached:
    mov.l   .Leh_emit_sq, r0
    mov.l   r7, @r0
    mov     r4, r1
    add     r5, r1
    fmov.s  @r1+, fr12
    fmov.s  @r1, fr13
    mov     r4, r1
    add     #16, r1
    fmov.s  @r1, fr14
    mov     r4, r1
    add     r6, r1
    mov.l   @r1, r3
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
    mov.l   r3, @r1
    add     #4, r1
    mov.l   r2, @r1
    pref    @r0
    rts
    nop

    .align 2
.Leh_emit_sq: .long SQ_BASE

! Both images: ten closed strips per image, each alternating ring k / k+1.
eh_draw_disk_images:
    sts.l   pr, @-r15
    mov     #0, r13                 ! image: 0 primary, 1 secondary
.Leh_images_pass:
    mov.l   .Leh_images_cache, r8
    mov     #EH_RINGS - 1, r10
.Leh_images_band:
    mov     r8, r11                 ! ring k
    mov     r8, r12
    mov.l   .Leh_images_ring_bytes, r0
    add     r0, r12                 ! ring k+1
    mov     #EH_SEGMENTS, r14
.Leh_images_segment:
    mov     r11, r4
    mov.l   .Leh_images_normal, r7
    bsr     eh_emit_image_vertex
    nop
    mov     r12, r4
    mov.l   .Leh_images_normal, r7
    bsr     eh_emit_image_vertex
    nop
    add     #EH_VERTEX_BYTES, r11
    add     #EH_VERTEX_BYTES, r12
    dt      r14
    bf      .Leh_images_segment
    nop
    ! Close the band on segment zero.
    mov     r8, r4
    mov.l   .Leh_images_normal, r7
    bsr     eh_emit_image_vertex
    nop
    mov     r8, r4
    mov.l   .Leh_images_ring_bytes, r0
    add     r0, r4
    mov.l   .Leh_images_eol, r7
    bsr     eh_emit_image_vertex
    nop
    mov.l   .Leh_images_ring_bytes, r0
    add     r0, r8
    dt      r10
    bf      .Leh_images_band
    nop
    add     #1, r13
    mov     #2, r0
    cmp/eq  r0, r13
    bf      .Leh_images_pass
    nop
    lds.l   @r15+, pr
    rts
    nop

! r4 = slot, r7 = command, r13 = image selector.
eh_emit_image_vertex:
    mov     #0, r5
    mov     #20, r6
    tst     r13, r13
    bt      .Leh_image_vertex_go
    mov     #8, r5
    mov     #24, r6
.Leh_image_vertex_go:
    mov.l   .Leh_images_emit, r0
    jmp     @r0
    nop

    .align 2
.Leh_images_cache:      .long eh_disk_cache
.Leh_images_ring_bytes: .long EH_RING_BYTES
.Leh_images_normal:     .long CMD_VERTEX
.Leh_images_eol:        .long CMD_VERTEX_EOL
.Leh_images_emit:       .long eh_emit_cached

! ---------------------------------------------------------------------------
! Opaque shadow disc: 48 rim points folded into one zig-zag strip.
! ---------------------------------------------------------------------------

eh_draw_shadow:
    sts.l   pr, @-r15
    mov     #1, r11                 ! ascending index
    mov     #47, r12                ! descending index
    mov     #23, r13
    mov     #0, r4
    mov.l   .Leh_shadow_normal, r5
    bsr     eh_emit_shadow_point
    nop
.Leh_shadow_loop:
    mov     r11, r4
    mov.l   .Leh_shadow_normal, r5
    bsr     eh_emit_shadow_point
    nop
    mov     r12, r4
    mov.l   .Leh_shadow_normal, r5
    bsr     eh_emit_shadow_point
    nop
    add     #1, r11
    add     #-1, r12
    dt      r13
    bf      .Leh_shadow_loop
    nop
    mov     #24, r4
    mov.l   .Leh_shadow_eol, r5
    bsr     eh_emit_shadow_point
    nop
    lds.l   @r15+, pr
    rts
    nop

! r4 = rim index, r5 = command.
eh_emit_shadow_point:
    mov.l   .Leh_shadow_step, r0
    mul.l   r4, r0
    sts     macl, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Leh_shadow_radius, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    fmul    fr2, fr1
    mov.l   .Leh_shadow_center_x, r0
    lds     r0, fpul
    fsts    fpul, fr12
    fadd    fr1, fr12
    mov.l   .Leh_shadow_center_y, r0
    lds     r0, fpul
    fsts    fpul, fr13
    fadd    fr0, fr13
    mov.l   .Leh_shadow_depth, r0
    fmov.s  @r0, fr14
    mov     r5, r4
    mov.l   .Leh_shadow_black, r5
    mov.l   .Leh_shadow_emit, r0
    jmp     @r0
    nop

    .align 2
.Leh_shadow_step:     .long 1365       ! 65536 / 48
.Leh_shadow_radius:   .long eh_shadow_px
.Leh_shadow_depth:    .long eh_inv_dist
.Leh_shadow_center_x: .long 0x43a00000
.Leh_shadow_center_y: .long 0x436c0000
.Leh_shadow_black:    .long 0xff000000
.Leh_shadow_normal:   .long CMD_VERTEX
.Leh_shadow_eol:      .long CMD_VERTEX_EOL
.Leh_shadow_emit:     .long emit_vertex

! ---------------------------------------------------------------------------
! Photon ring: a thin additive band just outside the shadow whose brightness
! follows the same Doppler asymmetry as the disk.  The ring is deliberately
! left out of the collapse fade so the final frames are a black disc rimmed
! with light before the transition flash.
! ---------------------------------------------------------------------------

eh_draw_photon_ring:
    sts.l   pr, @-r15
    mov     #0, r11                 ! rim index 0..64
    mov     #65, r12
.Leh_ring_segment:
    mov.l   .Leh_photon_step, r0
    mul.l   r11, r0
    sts     macl, r13               ! rim angle
    ! Doppler factor from the angle relative to the rolled disk plane.
    mov.l   .Leh_photon_roll_int, r0
    mov.l   @r0, r0
    add     r13, r0
    lds     r0, fpul
    fsca    fpul, dr0               ! fr1 = cos
    mov.l   .Leh_photon_cos_i, r0
    fmov.s  @r0, fr2
    mov.l   .Leh_photon_speed, r0
    lds     r0, fpul
    fsts    fpul, fr3
    fmul    fr3, fr2
    fmul    fr1, fr2
    fldi1   fr3
    fsub    fr2, fr3
    fldi1   fr2
    fdiv    fr3, fr2
    fmul    fr2, fr2
    mov.l   .Leh_photon_cap, r0
    lds     r0, fpul
    fsts    fpul, fr3
    fcmp/gt fr3, fr2
    bf      .Leh_ring_dop_ok
    fmov    fr3, fr2
.Leh_ring_dop_ok:
    mov.l   .Leh_photon_gain, r0
    lds     r0, fpul
    fsts    fpul, fr3
    fmul    fr3, fr2
    fmov    fr2, fr11
    mov.l   .Leh_photon_green, r0
    lds     r0, fpul
    fsts    fpul, fr12
    fmul    fr2, fr12
    mov.l   .Leh_photon_blue, r0
    lds     r0, fpul
    fsts    fpul, fr13
    fmul    fr2, fr13
    mov.l   .Leh_photon_pack, r0
    jsr     @r0
    nop
    mov     r0, r14                 ! inner color

    lds     r13, fpul
    fsca    fpul, dr0
    mov.l   .Leh_photon_radius, r0
    fmov.s  @r0, fr2
    fmov    fr1, fr4
    fmul    fr2, fr4                ! cos * R
    fmov    fr0, fr5
    fmul    fr2, fr5                ! sin * R
    mov.l   .Leh_photon_center_x, r0
    lds     r0, fpul
    fsts    fpul, fr6
    mov.l   .Leh_photon_center_y, r0
    lds     r0, fpul
    fsts    fpul, fr7
    mov.l   .Leh_photon_depth, r0
    fmov.s  @r0, fr14
    fmov    fr6, fr12
    fadd    fr4, fr12
    fmov    fr7, fr13
    fadd    fr5, fr13
    mov.l   .Leh_photon_normal, r4
    mov     r14, r5
    mov.l   .Leh_photon_emit, r0
    jsr     @r0
    nop
    mov.l   .Leh_photon_outer, r0
    lds     r0, fpul
    fsts    fpul, fr8
    fmul    fr8, fr4
    fmul    fr8, fr5
    fmov    fr6, fr12
    fadd    fr4, fr12
    fmov    fr7, fr13
    fadd    fr5, fr13
    mov.l   .Leh_photon_normal, r4
    mov     #1, r0
    cmp/eq  r0, r12
    bf      .Leh_ring_outer_cmd
    mov.l   .Leh_photon_eol, r4
.Leh_ring_outer_cmd:
    mov.l   .Leh_photon_black, r5
    mov.l   .Leh_photon_emit, r0
    jsr     @r0
    nop
    add     #1, r11
    dt      r12
    bf      .Leh_ring_segment
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Leh_photon_step:     .long 1024       ! 65536 / 64
.Leh_photon_roll_int: .long eh_roll_int
.Leh_photon_cos_i:    .long eh_cos_i
.Leh_photon_radius:   .long eh_shadow_px
.Leh_photon_depth:    .long eh_ring_depth
.Leh_photon_pack:     .long eh_pack_rgb
.Leh_photon_emit:     .long emit_vertex
.Leh_photon_speed:    .long 0x3ed70a3d  ! 0.42
.Leh_photon_cap:      .long 0x400ccccd  ! 2.2
.Leh_photon_gain:     .long 0x3f666666  ! 0.9
.Leh_photon_green:    .long 0x3f59999a  ! 0.85
.Leh_photon_blue:     .long 0x3f0ccccd  ! 0.55
.Leh_photon_outer:    .long 0x3f91eb85  ! 1.14
.Leh_photon_center_x: .long 0x43a00000
.Leh_photon_center_y: .long 0x436c0000
.Leh_photon_black:    .long 0xff000000
.Leh_photon_normal:   .long CMD_VERTEX
.Leh_photon_eol:      .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Lensed starfield.
! ---------------------------------------------------------------------------

! Directions are generated once from a 32-bit LCG: no star table is stored.
! Even-numbered stars are squashed toward the disk plane into a loose band.
eh_init_stars:
    mov.l   .Leh_init_seed, r1
    mov.l   .Leh_init_table, r8
    mov.l   .Leh_init_count, r9
    mov     #0, r10
.Leh_init_star:
    ! azimuth from the high half of one LCG step
    mov.l   .Leh_init_mul, r2
    mul.l   r2, r1
    sts     macl, r1
    mov.l   .Leh_init_add, r2
    add     r2, r1
    mov     r1, r3
    shlr16  r3
    ! elevation in -1..1 from the next step
    mov.l   .Leh_init_mul, r2
    mul.l   r2, r1
    sts     macl, r1
    mov.l   .Leh_init_add, r2
    add     r2, r1
    mov     r1, r0
    shlr8   r0
    lds     r0, fpul
    float   fpul, fr2
    mov.l   .Leh_init_inv24, r0
    lds     r0, fpul
    fsts    fpul, fr3
    fmul    fr3, fr2                ! 0..1
    fadd    fr2, fr2
    fldi1   fr4
    fsub    fr4, fr2                ! -1..1
    mov     r10, r0
    and     #1, r0
    tst     r0, r0
    bf      .Leh_init_elevation_ready
    mov.l   .Leh_init_band, r0
    lds     r0, fpul
    fsts    fpul, fr4
    fmul    fr4, fr2
.Leh_init_elevation_ready:
    ! brightness class from a third step
    mov.l   .Leh_init_mul, r2
    mul.l   r2, r1
    sts     macl, r1
    mov.l   .Leh_init_add, r2
    add     r2, r1
    mov     r1, r0
    shlr8   r0
    lds     r0, fpul
    float   fpul, fr5
    fmul    fr3, fr5

    fmov    fr2, fr6
    fmul    fr2, fr6
    fldi1   fr7
    fsub    fr6, fr7
    fsqrt   fr7                     ! horizontal radius
    lds     r3, fpul
    fsca    fpul, dr0
    fmul    fr7, fr0                ! x = rad sin
    fmul    fr7, fr1                ! z = rad cos
    fmov.s  fr0, @r8
    mov     r8, r0
    add     #4, r0
    fmov.s  fr2, @r0
    add     #4, r0
    fmov.s  fr1, @r0
    add     #4, r0
    fmov.s  fr5, @r0
    add     #16, r8
    add     #1, r10
    dt      r9
    bf      .Leh_init_star
    nop
    mov.l   .Leh_init_ready, r0
    mov     #1, r1
    mov.l   r1, @r0
    rts
    nop

    .align 2
.Leh_init_seed:  .long 0x2f6e2b1d
.Leh_init_mul:   .long 1664525
.Leh_init_add:   .long 1013904223
.Leh_init_table: .long eh_star_table
.Leh_init_ready: .long eh_stars_ready
.Leh_init_count: .long EH_STARS
.Leh_init_inv24: .long 0x33800000  ! 2^-24
.Leh_init_band:  .long 0x3eb33333  ! 0.35

eh_draw_stars:
    sts.l   pr, @-r15
    mov.l   .Leh_stars_ready, r0
    mov.l   @r0, r0
    tst     r0, r0
    bf      .Leh_stars_generated
    mov.l   .Leh_stars_init, r0
    jsr     @r0
    nop
.Leh_stars_generated:
    mov.l   .Leh_stars_table, r8
    mov.l   .Leh_stars_count, r9
    mov.l   .Leh_stars_azimuth, r0
    mov.l   @r0, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Leh_stars_sin_az, r0
    fmov.s  fr0, @r0
    mov.l   .Leh_stars_cos_az, r0
    fmov.s  fr1, @r0
.Leh_stars_loop:
    mov.l   .Leh_stars_one, r0
    jsr     @r0
    nop
    add     #16, r8
    dt      r9
    bf      .Leh_stars_loop
    nop
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Leh_stars_ready:   .long eh_stars_ready
.Leh_stars_init:    .long eh_init_stars
.Leh_stars_table:   .long eh_star_table
.Leh_stars_count:   .long EH_STARS
.Leh_stars_azimuth: .long eh_azimuth
.Leh_stars_sin_az:  .long eh_sin_az
.Leh_stars_cos_az:  .long eh_cos_az
.Leh_stars_one:     .long eh_draw_one_star

! r8 = star record (x, y, z, m).  Rotates the direction by the orbit azimuth,
! pitches and rolls it into camera space, lenses it, and emits one diamond
! stretched tangentially by theta/beta and squeezed radially by dtheta/dbeta.
eh_draw_one_star:
    sts.l   pr, @-r15
    fmov.s  @r8, fr0                ! x
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr1                ! y
    add     #4, r0
    fmov.s  @r0, fr2                ! z
    add     #4, r0
    fmov.s  @r0, fr3                ! m

    ! Yaw by the orbit azimuth.
    mov.l   .Leh_star_sin_az, r0
    fmov.s  @r0, fr4
    mov.l   .Leh_star_cos_az, r0
    fmov.s  @r0, fr5
    fmov    fr0, fr6
    fmul    fr5, fr6
    fmov    fr2, fr7
    fmul    fr4, fr7
    fadd    fr7, fr6                ! x'
    fmul    fr5, fr2
    fmul    fr4, fr0
    fsub    fr0, fr2                ! z'
    ! Pitch by the inclination.
    mov.l   .Leh_star_sin_i, r0
    fmov.s  @r0, fr4
    mov.l   .Leh_star_cos_i, r0
    fmov.s  @r0, fr5
    fmov    fr1, fr7
    fmul    fr5, fr7
    fmov    fr2, fr8
    fmul    fr4, fr8
    fadd    fr8, fr7                ! yc
    fmul    fr5, fr2
    fmul    fr4, fr1
    fsub    fr1, fr2                ! zc
    mov.l   .Leh_star_near, r0
    lds     r0, fpul
    fsts    fpul, fr0
    fcmp/gt fr0, fr2
    bt      .Leh_star_visible
    bra     .Leh_star_done
    nop
.Leh_star_visible:
    ! Roll, then perspective direction.
    mov.l   .Leh_star_sin_roll, r0
    fmov.s  @r0, fr4
    mov.l   .Leh_star_cos_roll, r0
    fmov.s  @r0, fr5
    fmov    fr6, fr8
    fmul    fr5, fr8
    fmov    fr7, fr9
    fmul    fr4, fr9
    fsub    fr9, fr8                ! xr
    fmul    fr4, fr6
    fmul    fr5, fr7
    fadd    fr6, fr7                ! yr
    fldi1   fr0
    fdiv    fr2, fr0
    fmul    fr0, fr8                ! bx
    fmul    fr0, fr7                ! by

    ! Lens: ib = 1/beta, iq = 1/q, q = sqrt(beta^2 + 4w).
    fmov    fr8, fr9
    fmul    fr8, fr9
    fmov    fr7, fr10
    fmul    fr7, fr10
    fadd    fr10, fr9
    mov.l   .Leh_star_epsilon, r0
    lds     r0, fpul
    fsts    fpul, fr10
    fadd    fr10, fr9               ! beta^2
    fmov    fr9, fr10
    fsrra   fr10                    ! ib
    mov.l   .Leh_star_lens, r0
    fmov.s  @r0, fr11               ! w
    fmov    fr11, fr12
    fadd    fr11, fr12              ! 2w
    fmov    fr12, fr13
    fadd    fr12, fr13              ! 4w
    fadd    fr9, fr13
    fsrra   fr13                    ! iq
    fadd    fr9, fr12               ! beta^2 + 2w
    fmul    fr10, fr12
    fmul    fr13, fr12              ! (beta^2 + 2w) / (beta q)
    mov.l   .Leh_star_half, r0
    lds     r0, fpul
    fsts    fpul, fr0
    fmov    fr12, fr4
    fmul    fr0, fr4
    fadd    fr0, fr4                ! point-source magnification mu+
    mov.l   .Leh_star_mag_cap, r0
    lds     r0, fpul
    fsts    fpul, fr1
    fcmp/gt fr1, fr4
    bf      .Leh_star_mag_ok
    fmov    fr1, fr4
.Leh_star_mag_ok:
    fmov    fr9, fr1
    fmul    fr10, fr1               ! beta
    fmov    fr1, fr14
    fmul    fr13, fr14              ! beta / q
    fmul    fr0, fr14
    fadd    fr0, fr14               ! radial squeeze 0.5 + 0.5 beta/q
    fldi1   fr2
    fdiv    fr13, fr2               ! q
    fmul    fr10, fr2
    fmul    fr0, fr2
    fadd    fr0, fr2                ! theta/beta: tangential stretch

    ! Unit radial axis (bx, -by)/beta in screen space (Y points down), then
    ! the lensed image position center + F b theta/beta.
    fmov    fr8, fr6
    fmul    fr10, fr6               ! rx
    fmov    fr7, fr9
    fmul    fr10, fr9
    fneg    fr9                     ! ry
    mov.l   .Leh_star_focal, r0
    lds     r0, fpul
    fsts    fpul, fr0
    fmul    fr2, fr0
    fmul    fr0, fr8
    fmul    fr0, fr7
    mov.l   .Leh_star_center_x, r0
    lds     r0, fpul
    fsts    fpul, fr0
    fadd    fr0, fr8                ! px
    mov.l   .Leh_star_center_y, r0
    lds     r0, fpul
    fsts    fpul, fr0
    fsub    fr7, fr0
    fmov    fr0, fr7                ! py

    ! Brightness: class, magnification, fade -> fr11/fr12/fr13.
    fmov    fr3, fr11
    fmul    fr3, fr11
    mov.l   .Leh_star_bright_var, r0
    lds     r0, fpul
    fsts    fpul, fr12
    fmul    fr12, fr11
    mov.l   .Leh_star_bright_base, r0
    lds     r0, fpul
    fsts    fpul, fr12
    fadd    fr12, fr11
    mov.l   .Leh_star_half, r0
    lds     r0, fpul
    fsts    fpul, fr12
    fmul    fr12, fr4
    fadd    fr12, fr4               ! 0.5 + 0.5 mu
    fldi1   fr12
    fcmp/gt fr12, fr4
    bf      .Leh_star_mag_scale_ok
    fmov    fr12, fr4
.Leh_star_mag_scale_ok:
    fmul    fr4, fr11
    mov.l   .Leh_star_fade, r0
    fmov.s  @r0, fr12
    fmul    fr12, fr11              ! brightness = R
    mov.l   .Leh_star_tint_g, r0
    lds     r0, fpul
    fsts    fpul, fr12
    fmul    fr3, fr12
    mov.l   .Leh_star_tint_g_base, r0
    lds     r0, fpul
    fsts    fpul, fr13
    fadd    fr13, fr12
    fmul    fr11, fr12              ! G
    mov.l   .Leh_star_tint_b, r0
    lds     r0, fpul
    fsts    fpul, fr13
    fmul    fr3, fr13
    fldi1   fr0
    fsub    fr13, fr0
    fmov    fr0, fr13
    fmul    fr11, fr13              ! B

    ! Half-sizes: radial squeezed, tangential stretched and capped.
    mov.l   .Leh_star_size_var, r0
    lds     r0, fpul
    fsts    fpul, fr4
    fmul    fr3, fr4
    mov.l   .Leh_star_size_base, r0
    lds     r0, fpul
    fsts    fpul, fr5
    fadd    fr5, fr4                ! s0
    fmov    fr4, fr5
    fmul    fr14, fr5               ! radial half-size
    fmul    fr4, fr2                ! tangential half-size
    mov.l   .Leh_star_stretch_cap, r0
    lds     r0, fpul
    fsts    fpul, fr0
    fmul    fr0, fr4
    fcmp/gt fr4, fr2
    bf      .Leh_star_stretch_ok
    fmov    fr4, fr2
.Leh_star_stretch_ok:
    fmov    fr2, fr4                ! tangential half-size survives packing
    mov.l   .Leh_star_pack, r0
    jsr     @r0
    nop
    mov     r0, r10                 ! packed color

    mov.l   .Leh_star_depth, r0
    lds     r0, fpul
    fsts    fpul, fr14
    ! Diamond strip: radial+, tangential+, tangential-, radial-.
    fmov    fr8, fr12
    fmov    fr6, fr0
    fmul    fr5, fr0
    fadd    fr0, fr12
    fmov    fr7, fr13
    fmov    fr9, fr0
    fmul    fr5, fr0
    fadd    fr0, fr13
    mov.l   .Leh_star_normal, r4
    mov     r10, r5
    mov.l   .Leh_star_emit, r0
    jsr     @r0
    nop
    fmov    fr8, fr12
    fmov    fr9, fr0
    fmul    fr4, fr0
    fsub    fr0, fr12               ! tangential axis = (-ry, rx)
    fmov    fr7, fr13
    fmov    fr6, fr0
    fmul    fr4, fr0
    fadd    fr0, fr13
    mov.l   .Leh_star_normal, r4
    mov     r10, r5
    mov.l   .Leh_star_emit, r0
    jsr     @r0
    nop
    fmov    fr8, fr12
    fmov    fr9, fr0
    fmul    fr4, fr0
    fadd    fr0, fr12
    fmov    fr7, fr13
    fmov    fr6, fr0
    fmul    fr4, fr0
    fsub    fr0, fr13
    mov.l   .Leh_star_normal, r4
    mov     r10, r5
    mov.l   .Leh_star_emit, r0
    jsr     @r0
    nop
    fmov    fr8, fr12
    fmov    fr6, fr0
    fmul    fr5, fr0
    fsub    fr0, fr12
    fmov    fr7, fr13
    fmov    fr9, fr0
    fmul    fr5, fr0
    fsub    fr0, fr13
    mov.l   .Leh_star_eol, r4
    mov     r10, r5
    mov.l   .Leh_star_emit, r0
    jsr     @r0
    nop
.Leh_star_done:
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Leh_star_sin_az:       .long eh_sin_az
.Leh_star_cos_az:       .long eh_cos_az
.Leh_star_sin_i:        .long eh_sin_i
.Leh_star_cos_i:        .long eh_cos_i
.Leh_star_sin_roll:     .long eh_sin_roll
.Leh_star_cos_roll:     .long eh_cos_roll
.Leh_star_lens:         .long eh_star_lens
.Leh_star_fade:         .long eh_fade
.Leh_star_pack:         .long eh_pack_rgb
.Leh_star_emit:         .long emit_vertex
.Leh_star_near:         .long 0x3da3d70a  ! 0.08
.Leh_star_epsilon:      .long 0x2edbe6ff  ! 1e-10
.Leh_star_half:         .long 0x3f000000
.Leh_star_mag_cap:      .long 0x40c00000  ! 6
.Leh_star_bright_base:  .long 0x3f0ccccd  ! 0.55
.Leh_star_bright_var:   .long 0x3ee66666  ! 0.45
.Leh_star_tint_g:       .long 0x3d4ccccd  ! 0.05
.Leh_star_tint_g_base:  .long 0x3f733333  ! 0.95
.Leh_star_tint_b:       .long 0x3e19999a  ! 0.15
.Leh_star_size_base:    .long 0x400ccccd  ! 2.2
.Leh_star_size_var:     .long 0x40266666  ! 2.6
.Leh_star_stretch_cap:  .long 0x40800000  ! 4
.Leh_star_focal:        .long 0x440c0000  ! 560
.Leh_star_center_x:     .long 0x43a00000
.Leh_star_center_y:     .long 0x436c0000
.Leh_star_depth:        .long 0x3a83126f  ! far, behind everything opaque
.Leh_star_normal:       .long CMD_VERTEX
.Leh_star_eol:          .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Constants and generated-object tables.
! ---------------------------------------------------------------------------

    .section .rodata,"a"
    .align 5

! Native four-vertex strip: deep space with a barely warm lower horizon.
eh_backdrop_vertices:
    .long CMD_VERTEX,     0x00000000, 0x43f00000, 0x3a83126f
    .long 0, 0, 0xff030206, 0
    .long CMD_VERTEX,     0x00000000, 0x00000000, 0x3a83126f
    .long 0, 0, 0xff010104, 0
    .long CMD_VERTEX,     0x44200000, 0x43f00000, 0x3a83126f
    .long 0, 0, 0xff040206, 0
    .long CMD_VERTEX_EOL, 0x44200000, 0x00000000, 0x3a83126f
    .long 0, 0, 0xff010105, 0

! Per ring: radius, Keplerian sqrt(0.5/r), pattern speed (binary angle units
! per frame, 1 turn / 120 frames at the inner edge, falling as r^-1.5), and
! the temperature ramp already scaled by the 0.55 additive headroom factor.
eh_ring_constants:
    .long 0x40400000, 0x3ed105ec, 546, 0x3f0ccccd, 0x3f072b02, 0x3ee147ae ! r=3.0
    .long 0x40799999, 0x3ec2cbe3, 371, 0x3f0ccccd, 0x3ef33333, 0x3ea8f5c3 ! r=3.9
    .long 0x40999999, 0x3eb6e6c4, 265, 0x3f0ccccd, 0x3ed851ec, 0x3e6147ae ! r=4.8
    .long 0x40b66666, 0x3eac9d1a, 196, 0x3f094a3c, 0x3eb7fcb9, 0x3e126e98 ! r=5.7
    .long 0x40d33333, 0x3ea3b4c0, 150, 0x3f02428f, 0x3e926e98, 0x3dcac083 ! r=6.6
    .long 0x40f00000, 0x3e9bd5a0, 117, 0x3ef66666, 0x3e59fcb9, 0x3d6147ae ! r=7.5
    .long 0x41066666, 0x3e94d0c1, 93,  0x3ed7e1c5, 0x3e1c0ebe, 0x3ceecbfb ! r=8.4
    .long 0x41133333, 0x3e8e6db3, 76,  0x3ea8f5c3, 0x3dd5fcb9, 0x3cb43958 ! r=9.3
    .long 0x41200000, 0x3e88a6e0, 62,  0x3e73c22c, 0x3d67b5f0, 0x3c7333c1 ! r=10.2
    .long 0x412ccccd, 0x3e838cd1, 52,  0x3e034b3d, 0x3cb43958, 0x3bea9f47 ! r=11.1
    .long 0x41400000, 0x3e7e8ac4, 43,  0x00000000, 0x00000000, 0x00000000 ! r=12.0

    .section .bss,"aw",@nobits
    .align 5
eh_dist:          .space 4
eh_inv_dist:      .space 4
eh_ring_depth:    .space 4
eh_sin_i:         .space 4
eh_cos_i:         .space 4
eh_sin_roll:      .space 4
eh_cos_roll:      .space 4
eh_roll_int:      .space 4
eh_azimuth:       .space 4
eh_sin_az:        .space 4
eh_cos_az:        .space 4
eh_mass:          .space 4
eh_fade:          .space 4
eh_lens:          .space 4
eh_star_lens:     .space 4
eh_shadow_px:     .space 4
eh_scratch_psi:   .space 4
eh_scratch_sin_psi:.space 4
eh_scratch_w:     .space 4
eh_stars_ready:   .space 4
    .align 5
eh_ring_state:    .space EH_RINGS * 16
    .align 5
eh_star_table:    .space EH_STARS * 16
    .align 5
eh_disk_cache:    .space EH_RINGS * EH_RING_BYTES
