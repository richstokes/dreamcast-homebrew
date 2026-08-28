! ---------------------------------------------------------------------------
! CHROMA CIRCUIT -- Scene 04: HYPERFOLD
!
! Early-2000s PC demos made a virtue of generated geometry: a small topology
! would be transformed hard, synchronized tightly, and redrawn in luminous
! layers until it felt much larger than its source data. HYPERFOLD follows that
! tradition without a texture, SDK call, or stored mesh.
!
! Sixteen four-dimensional vertices are rotated in XW, YZ, XY, and ZW planes,
! perspective-divided from 4D into 3D, then sent through the ordinary orbiting
! 3D camera. Twelve phase-delayed copies become geometric "time ghosts". The
! 32 edges of each copy are expanded into constant-width screen-space beams,
! so the PowerVR2 receives 768 glow triangles plus sixteen hot vertex nodes.
! The entire source object is only 256 bytes of coordinates and 64 bytes of
! topology; SH-4 generates every visible transformation at 60 Hz.
! ---------------------------------------------------------------------------

    .section .text,"ax"
    .align 2

draw_scene_hyperfold:
    sts.l   pr, @-r15
    mov.l   r10, @-r15

    mov.l   .Lhyper_scene_camera, r0
    jsr     @r0
    nop

    ! A restrained opaque field and the geometry HUD anchor the otherwise
    ! transparent object. Near HUD depth keeps the later glow behind its text.
    mov.l   .Lhyper_scene_opaque, r4
    mov.l   .Lhyper_scene_submit, r0
    jsr     @r0
    nop
    mov.l   .Lhyper_scene_backdrop, r4
    mov     #4, r10
.Lhyper_backdrop_loop:
    mov.l   .Lhyper_scene_submit, r0
    jsr     @r0
    nop
    dt      r10
    bf      .Lhyper_backdrop_loop
    nop
    mov.l   .Lhyper_scene_title, r0
    jsr     @r0
    nop
    mov.l   .Lhyper_scene_zero, r4
    mov.l   .Lhyper_scene_submit, r0
    jsr     @r0
    nop

    ! Oldest/dimmest fold first, current fold last. ONE/ONE blending makes the
    ! result order independent, while the per-copy hue drift creates moire-like
    ! interference wherever multiple 4D projections cross.
    mov.l   .Lhyper_scene_additive, r4
    mov.l   .Lhyper_scene_submit, r0
    jsr     @r0
    nop
    mov     #11, r10
.Lhyper_ghost_loop:
    mov     r10, r4
    mov.l   .Lhyper_scene_build, r0
    jsr     @r0
    nop
    mov     r10, r4
    mov.l   .Lhyper_scene_edges, r0
    jsr     @r0
    nop
    tst     r10, r10
    bf      .Lhyper_ghost_next
    mov.l   .Lhyper_scene_nodes, r0
    jsr     @r0
    nop
.Lhyper_ghost_next:
    add     #-1, r10
    cmp/pz  r10
    bt      .Lhyper_ghost_loop
    nop

    mov.l   @r15+, r10
    lds.l   @r15+, pr
    rts
    nop

    .align 2
.Lhyper_scene_camera:  .long update_hyper_camera
.Lhyper_scene_build:   .long build_hyper_cache
.Lhyper_scene_edges:   .long draw_hyper_edges
.Lhyper_scene_nodes:   .long draw_hyper_nodes
.Lhyper_scene_submit:  .long ta_submit
.Lhyper_scene_title:   .long draw_title
.Lhyper_scene_opaque:  .long opaque_header
.Lhyper_scene_additive:.long additive_header
.Lhyper_scene_backdrop:.long backdrop_vertices
.Lhyper_scene_zero:    .long zero_block

! ---------------------------------------------------------------------------
! Dedicated camera: a steady orbit is cross-modulated by pitch, roll, and a
! slow radial breath. The object itself rotates in four dimensions, so a calm
! camera makes its impossible folding readable before banking into the climax.
! ---------------------------------------------------------------------------

update_hyper_camera:
    ! yaw = frame * 144 (128 + 16)
    mov     r9, r0
    shll8   r0
    shlr    r0
    mov     r9, r1
    shll2   r1
    shll2   r1
    add     r1, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lhyper_cam_yaw, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0

    ! pitch wave = frame * 72; convert +/-4096 binary-angle units to sin/cos.
    mov     r9, r0
    shll2   r0
    shll2   r0
    shll2   r0                    ! *64
    mov     r9, r1
    shll2   r1                    ! *4
    add     r1, r0
    add     r1, r0                ! *72
    lds     r0, fpul
    fsca    fpul, dr2
    fmov    fr2, fr10
    mov.l   .Lhyper_cam_pitch_units, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr4
    mov.l   .Lhyper_cam_pitch, r0
    fmov.s  fr4, @r0
    add     #4, r0
    fmov.s  fr5, @r0

    ! Quadrature drives a stronger bank, giving the final act a hovering,
    ! impossible-camera feel instead of another simple horizontal orbit.
    fmov    fr3, fr10
    mov.l   .Lhyper_cam_roll_units, r0
    fmov.s  @r0, fr11
    fmul    fr11, fr10
    ftrc    fr10, fpul
    sts     fpul, r0
    lds     r0, fpul
    fsca    fpul, dr6
    mov.l   .Lhyper_cam_roll, r0
    fmov.s  fr6, @r0
    add     #4, r0
    fmov.s  fr7, @r0

    ! Radius = 6.6 + 0.40*sin(frame*72). Even at its closest, the camera is
    ! outside the strongest 4D projection, preserving positive camera Z.
    fmov    fr2, fr9
    mov.l   .Lhyper_cam_radius_amp, r0
    fmov.s  @r0, fr10
    fmul    fr10, fr9
    mov.l   .Lhyper_cam_radius_base, r0
    fmov.s  @r0, fr10
    fadd    fr10, fr9

    fmov    fr9, fr10
    fmul    fr5, fr10
    fmov    fr10, fr11
    fmul    fr0, fr11
    fmov    fr9, fr12
    fmul    fr4, fr12
    fmov    fr10, fr13
    fmul    fr1, fr13
    mov.l   .Lhyper_cam_position, r0
    fmov.s  fr11, @r0
    add     #4, r0
    fmov.s  fr12, @r0
    add     #4, r0
    fmov.s  fr13, @r0
    rts
    nop

    .align 2
.Lhyper_cam_yaw:         .long camera_yaw
.Lhyper_cam_pitch:       .long camera_pitch
.Lhyper_cam_roll:        .long camera_roll
.Lhyper_cam_position:    .long camera_position
.Lhyper_cam_pitch_units: .long hyper_camera_pitch_units
.Lhyper_cam_roll_units:  .long hyper_camera_roll_units
.Lhyper_cam_radius_base: .long hyper_camera_radius
.Lhyper_cam_radius_amp:  .long hyper_camera_radius_amplitude

! ---------------------------------------------------------------------------
! Build one projected 16-point copy.
!
! r4 = ghost age, in six-frame steps. Four incommensurate angular velocities
! ensure the delayed copies do not collapse into a conventional motion trail.
! The 4D eye distance breathes once per musical bar (128 display frames), so
! the cube repeatedly folds inside-out on the sustained synth chords.
! ---------------------------------------------------------------------------

build_hyper_cache:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov     r4, r14

    ! XW phase = frame*224 - age*1344.
    mov     r9, r0
    shll8   r0
    mov     r9, r1
    shll2   r1
    shll2   r1
    shll    r1
    sub     r1, r0
    mov     r14, r2
    shll8   r2                    ! age*256
    mov     r2, r3
    shll2   r3                    ! age*1024
    add     r2, r3
    mov     r14, r2
    shll2   r2
    shll2   r2
    shll2   r2                    ! age*64
    add     r2, r3
    sub     r3, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lhyper_rotations, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0

    ! YZ phase = 0x2000 + frame*160 - age*960.
    mov     r9, r0
    shll8   r0
    shlr    r0                    ! frame*128
    mov     r9, r1
    shll2   r1
    shll2   r1
    shll    r1                    ! frame*32
    add     r1, r0
    mov.l   .Lhyper_phase_yz, r1
    add     r1, r0
    mov     r14, r2
    shll2   r2
    shll2   r2
    shll2   r2                    ! age*64
    mov     r14, r3
    shll8   r3
    shll2   r3                    ! age*1024
    sub     r2, r3                ! age*960
    sub     r3, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lhyper_rotations, r0
    add     #8, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0

    ! XY phase = 0x4800 + frame*96 - age*576.
    mov     r9, r0
    shll2   r0
    shll2   r0
    shll2   r0                    ! frame*64
    mov     r9, r1
    shll2   r1
    shll2   r1
    shll    r1                    ! frame*32
    add     r1, r0
    mov.l   .Lhyper_phase_xy, r1
    add     r1, r0
    mov     r14, r2
    shll8   r2
    shll    r2                    ! age*512
    mov     r14, r3
    shll2   r3
    shll2   r3
    shll2   r3                    ! age*64
    add     r3, r2
    sub     r2, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lhyper_rotations, r0
    add     #16, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0

    ! ZW phase = 0x7000 + frame*128 - age*768.
    mov     r9, r0
    shll8   r0
    shlr    r0
    mov.l   .Lhyper_phase_zw, r1
    add     r1, r0
    mov     r14, r2
    shll8   r2                    ! age*256
    mov     r2, r3
    shll    r3                    ! age*512
    add     r3, r2                ! age*768
    sub     r2, r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lhyper_rotations, r0
    add     #24, r0
    fmov.s  fr0, @r0
    add     #4, r0
    fmov.s  fr1, @r0

    ! 4D eye distance = 4.25 + 0.55*cos(frame*512). The minimum 3.70
    ! remains 1.70 beyond the maximum |W| of the rotated unit hypercube.
    mov     r9, r0
    shll8   r0
    shll    r0
    lds     r0, fpul
    fsca    fpul, dr0
    mov.l   .Lhyper_fold_amplitude, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr1
    mov.l   .Lhyper_fold_base, r0
    fmov.s  @r0, fr2
    fadd    fr2, fr1
    mov.l   .Lhyper_fold_distance, r0
    fmov.s  fr1, @r0

    mov.l   .Lhyper_source_vertices, r13
    mov.l   .Lhyper_cache, r8
    mov     #0, r10
    mov     #16, r11
.Lhyper_build_vertex:
    fmov.s  @r13+, fr4             ! X
    fmov.s  @r13+, fr5             ! Y
    fmov.s  @r13+, fr6             ! Z
    fmov.s  @r13+, fr7             ! W

    ! Rotate XW.
    mov.l   .Lhyper_rotations, r0
    fmov.s  @r0, fr0               ! sin A
    add     #4, r0
    fmov.s  @r0, fr1               ! cos A
    fmov    fr4, fr8
    fmul    fr1, fr8
    fmov    fr7, fr9
    fmul    fr0, fr9
    fsub    fr9, fr8               ! X1
    fmov    fr4, fr9
    fmul    fr0, fr9
    fmov    fr7, fr10
    fmul    fr1, fr10
    fadd    fr10, fr9              ! W1

    ! Rotate YZ.
    mov.l   .Lhyper_rotations, r0
    add     #8, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1
    fmov    fr5, fr10
    fmul    fr1, fr10
    fmov    fr6, fr11
    fmul    fr0, fr11
    fsub    fr11, fr10             ! Y1
    fmov    fr5, fr11
    fmul    fr0, fr11
    fmov    fr6, fr12
    fmul    fr1, fr12
    fadd    fr12, fr11             ! Z1

    ! Rotate the resulting XY pair.
    mov.l   .Lhyper_rotations, r0
    add     #16, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1
    fmov    fr8, fr4
    fmul    fr1, fr4
    fmov    fr10, fr5
    fmul    fr0, fr5
    fsub    fr5, fr4               ! X2
    fmov    fr8, fr5
    fmul    fr0, fr5
    fmov    fr10, fr6
    fmul    fr1, fr6
    fadd    fr6, fr5               ! Y2

    ! Rotate the ZW pair last.
    mov.l   .Lhyper_rotations, r0
    add     #24, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1
    fmov    fr11, fr6
    fmul    fr1, fr6
    fmov    fr9, fr7
    fmul    fr0, fr7
    fsub    fr7, fr6               ! Z2
    fmov    fr11, fr7
    fmul    fr0, fr7
    fmov    fr9, fr8
    fmul    fr1, fr8
    fadd    fr8, fr7               ! W2

    ! 4D perspective k = distance / (distance - W2), followed by a restrained
    ! world scale. The violent apparent expansion is projection, not a mesh
    ! animation, which is why its topology remains perfectly coherent.
    mov.l   .Lhyper_fold_distance, r0
    fmov.s  @r0, fr0
    fmov    fr0, fr1
    fsub    fr7, fr1
    fdiv    fr1, fr0
    mov.l   .Lhyper_world_scale, r0
    fmov.s  @r0, fr2
    fmul    fr2, fr0
    fmul    fr0, fr4
    fmul    fr0, fr5
    fmul    fr0, fr6
    fmov    fr4, fr7
    fmov    fr5, fr8
    fmov    fr6, fr9
    mov     r8, r4
    mov.l   .Lhyper_project, r0
    jsr     @r0
    nop

    add     #12, r8
    add     #1, r10
    dt      r11
    bf      .Lhyper_build_vertex
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
.Lhyper_rotations:      .long hyper_rotations
.Lhyper_fold_distance:  .long hyper_fold_distance
.Lhyper_source_vertices:.long hyper_vertices_4d
.Lhyper_cache:          .long hyper_projected
.Lhyper_project:        .long hyper_project_point
.Lhyper_fold_base:      .long hyper_fold_base
.Lhyper_fold_amplitude: .long hyper_fold_amplitude
.Lhyper_world_scale:    .long hyper_world_scale
.Lhyper_phase_yz:       .long 0x00002000
.Lhyper_phase_xy:       .long 0x00004800
.Lhyper_phase_zw:       .long 0x00007000

! Inputs: fr7/fr8/fr9 world XYZ, r4 -> three-float destination. This is the
! ordinary scene camera/projector split from project_world_vertex, except its
! result is cached rather than immediately submitted to the TA.
hyper_project_point:
    mov     r4, r6
    mov.l   .Lhyper_project_position, r0
    fmov.s  @r0, fr0
    fsub    fr0, fr7
    add     #4, r0
    fmov.s  @r0, fr0
    fsub    fr0, fr8
    add     #4, r0
    fmov.s  @r0, fr0
    fsub    fr0, fr9

    mov.l   .Lhyper_project_yaw, r0
    fmov.s  @r0, fr0
    add     #4, r0
    fmov.s  @r0, fr1
    mov.l   .Lhyper_project_pitch, r0
    fmov.s  @r0, fr2
    add     #4, r0
    fmov.s  @r0, fr3
    mov.l   .Lhyper_project_roll, r0
    fmov.s  @r0, fr4
    add     #4, r0
    fmov.s  @r0, fr5

    fmov    fr7, fr10
    fmul    fr1, fr10
    fmov    fr9, fr11
    fmul    fr0, fr11
    fsub    fr11, fr10
    fmov    fr7, fr11
    fmul    fr0, fr11
    fneg    fr11
    fmov    fr9, fr12
    fmul    fr1, fr12
    fsub    fr12, fr11

    fmov    fr8, fr12
    fmul    fr3, fr12
    fmov    fr11, fr13
    fmul    fr2, fr13
    fadd    fr13, fr12
    fmov    fr8, fr13
    fmul    fr2, fr13
    fneg    fr13
    fmov    fr11, fr14
    fmul    fr3, fr14
    fadd    fr14, fr13             ! positive camera Z

    fmov    fr10, fr14
    fmul    fr5, fr14
    fmov    fr12, fr15
    fmul    fr4, fr15
    fadd    fr15, fr14
    fmov    fr10, fr15
    fmul    fr4, fr15
    fneg    fr15
    fmov    fr12, fr7
    fmul    fr5, fr7
    fadd    fr7, fr15

    mov.l   .Lhyper_project_one, r0
    fmov.s  @r0, fr9
    fdiv    fr13, fr9
    mov.l   .Lhyper_project_scale, r0
    fmov.s  @r0, fr8
    fmul    fr8, fr14
    fmul    fr9, fr14
    mov.l   .Lhyper_project_cx, r0
    fmov.s  @r0, fr8
    fadd    fr8, fr14
    mov.l   .Lhyper_project_scale, r0
    fmov.s  @r0, fr8
    fmul    fr8, fr15
    fmul    fr9, fr15
    fneg    fr15
    mov.l   .Lhyper_project_cy, r0
    fmov.s  @r0, fr8
    fadd    fr8, fr15
    fmov.s  fr14, @r6
    add     #4, r6
    fmov.s  fr15, @r6
    add     #4, r6
    fmov.s  fr9, @r6
    rts
    nop

    .align 2
.Lhyper_project_position:.long camera_position
.Lhyper_project_yaw:     .long camera_yaw
.Lhyper_project_pitch:   .long camera_pitch
.Lhyper_project_roll:    .long camera_roll
.Lhyper_project_one:     .long float_one
.Lhyper_project_scale:   .long projection_scale
.Lhyper_project_cx:      .long center_x
.Lhyper_project_cy:      .long center_y

! ---------------------------------------------------------------------------
! Expand all 32 projected edges to antialiased-looking geometric beams.
!
! r4 = ghost age. A reciprocal-square-root normalizes the 2D edge normal;
! adding 0.25 pixel^2 before FSRRA prevents a near-end-on edge from exploding.
! ---------------------------------------------------------------------------

draw_hyper_edges:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15

    mov     r4, r0
    shll2   r0
    mov.l   .Lhyper_edge_palette, r1
    mov.l   @(r0, r1), r11
    mov.l   .Lhyper_edge_widths, r1
    fmov.s  @(r0, r1), fr9
    mov.l   .Lhyper_edge_topology, r14
    mov.l   .Lhyper_edge_cache, r8
    mov     #32, r10

.Lhyper_edge_loop:
    mov.b   @r14+, r0
    extu.b  r0, r0
    mov     r0, r1
    shll2   r0
    shll    r1
    shll2   r1
    add     r1, r0                 ! index * 12
    mov     r8, r12
    add     r0, r12
    mov.b   @r14+, r0
    extu.b  r0, r0
    mov     r0, r1
    shll2   r0
    shll    r1
    shll2   r1
    add     r1, r0
    mov     r8, r13
    add     r0, r13

    fmov.s  @r12, fr0              ! x0
    mov     r12, r0
    add     #4, r0
    fmov.s  @r0, fr1               ! y0
    add     #4, r0
    fmov.s  @r0, fr2               ! z0
    fmov.s  @r13, fr3              ! x1
    mov     r13, r0
    add     #4, r0
    fmov.s  @r0, fr4               ! y1
    add     #4, r0
    fmov.s  @r0, fr5               ! z1

    fmov    fr3, fr6
    fsub    fr0, fr6               ! dx
    fmov    fr4, fr7
    fsub    fr1, fr7               ! dy
    fmov    fr6, fr8
    fmul    fr6, fr8
    fmov    fr7, fr10
    fmul    fr7, fr10
    fadd    fr10, fr8
    mov.l   .Lhyper_edge_epsilon, r0
    fmov.s  @r0, fr10
    fadd    fr10, fr8
    fsrra   fr8                     ! reciprocal screen-space length
    fmul    fr8, fr6
    fmul    fr9, fr6               ! +normal Y
    fmul    fr8, fr7
    fmul    fr9, fr7
    fneg    fr7                     ! +normal X

    fmov    fr0, fr12
    fadd    fr7, fr12
    fmov    fr1, fr13
    fadd    fr6, fr13
    fmov    fr2, fr14
    mov.l   .Lhyper_edge_normal, r4
    mov     r11, r5
    mov.l   .Lhyper_edge_emit, r0
    jsr     @r0
    nop

    fmov    fr0, fr12
    fsub    fr7, fr12
    fmov    fr1, fr13
    fsub    fr6, fr13
    fmov    fr2, fr14
    mov.l   .Lhyper_edge_normal, r4
    mov     r11, r5
    mov.l   .Lhyper_edge_emit, r0
    jsr     @r0
    nop

    fmov    fr3, fr12
    fadd    fr7, fr12
    fmov    fr4, fr13
    fadd    fr6, fr13
    fmov    fr5, fr14
    mov.l   .Lhyper_edge_normal, r4
    mov     r11, r5
    mov.l   .Lhyper_edge_emit, r0
    jsr     @r0
    nop

    fmov    fr3, fr12
    fsub    fr7, fr12
    fmov    fr4, fr13
    fsub    fr6, fr13
    fmov    fr5, fr14
    mov.l   .Lhyper_edge_eol, r4
    mov     r11, r5
    mov.l   .Lhyper_edge_emit, r0
    jsr     @r0
    nop

    dt      r10
    bf      .Lhyper_edge_loop
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
.Lhyper_edge_palette: .long hyper_ghost_palette
.Lhyper_edge_widths:  .long hyper_ghost_widths
.Lhyper_edge_topology:.long hyper_edges
.Lhyper_edge_cache:   .long hyper_projected
.Lhyper_edge_epsilon: .long hyper_edge_epsilon
.Lhyper_edge_emit:    .long emit_vertex
.Lhyper_edge_normal:  .long CMD_VERTEX
.Lhyper_edge_eol:     .long CMD_VERTEX_EOL

! Sixteen hot cores mark the current 4D slice. They are deliberately screen-
! aligned: the old-school sprite/impostor look contrasts the exact edge mesh.
draw_hyper_nodes:
    sts.l   pr, @-r15
    mov.l   r8, @-r15
    mov.l   r9, @-r15
    mov.l   r10, @-r15
    mov.l   r11, @-r15
    mov.l   r12, @-r15
    mov.l   r13, @-r15
    mov.l   r14, @-r15
    mov.l   .Lhyper_node_cache, r8
    mov.l   .Lhyper_node_colors, r9
    mov     #0, r11
    mov     #16, r10
.Lhyper_node_loop:
    fmov.s  @r8, fr0
    mov     r8, r0
    add     #4, r0
    fmov.s  @r0, fr1
    add     #4, r0
    fmov.s  @r0, fr2
    mov.l   .Lhyper_node_size, r0
    fmov.s  @r0, fr3
    mov     r11, r0
    and     #3, r0
    shll2   r0
    mov.l   @(r0, r9), r12

    fmov    fr0, fr12
    fsub    fr3, fr12
    fmov    fr1, fr13
    fadd    fr3, fr13
    fmov    fr2, fr14
    mov.l   .Lhyper_node_normal, r4
    mov     r12, r5
    mov.l   .Lhyper_node_emit, r0
    jsr     @r0
    nop
    fmov    fr0, fr12
    fsub    fr3, fr12
    fmov    fr1, fr13
    fsub    fr3, fr13
    fmov    fr2, fr14
    mov.l   .Lhyper_node_normal, r4
    mov     r12, r5
    mov.l   .Lhyper_node_emit, r0
    jsr     @r0
    nop
    fmov    fr0, fr12
    fadd    fr3, fr12
    fmov    fr1, fr13
    fadd    fr3, fr13
    fmov    fr2, fr14
    mov.l   .Lhyper_node_normal, r4
    mov     r12, r5
    mov.l   .Lhyper_node_emit, r0
    jsr     @r0
    nop
    fmov    fr0, fr12
    fadd    fr3, fr12
    fmov    fr1, fr13
    fsub    fr3, fr13
    fmov    fr2, fr14
    mov.l   .Lhyper_node_eol, r4
    mov     r12, r5
    mov.l   .Lhyper_node_emit, r0
    jsr     @r0
    nop

    add     #12, r8
    add     #1, r11
    dt      r10
    bf      .Lhyper_node_loop
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
.Lhyper_node_cache: .long hyper_projected
.Lhyper_node_colors:.long hyper_node_palette
.Lhyper_node_size:  .long hyper_node_half_size
.Lhyper_node_emit:  .long emit_vertex
.Lhyper_node_normal:.long CMD_VERTEX
.Lhyper_node_eol:   .long CMD_VERTEX_EOL

! ---------------------------------------------------------------------------
! Compact generated-object data and visual tuning.
! ---------------------------------------------------------------------------

    .section .rodata,"a"
    .align 5

hyper_vertices_4d:
    ! Bit order is X,Y,Z,W; negative is bit clear, positive is bit set.
    .long 0xbf800000,0xbf800000,0xbf800000,0xbf800000
    .long 0x3f800000,0xbf800000,0xbf800000,0xbf800000
    .long 0xbf800000,0x3f800000,0xbf800000,0xbf800000
    .long 0x3f800000,0x3f800000,0xbf800000,0xbf800000
    .long 0xbf800000,0xbf800000,0x3f800000,0xbf800000
    .long 0x3f800000,0xbf800000,0x3f800000,0xbf800000
    .long 0xbf800000,0x3f800000,0x3f800000,0xbf800000
    .long 0x3f800000,0x3f800000,0x3f800000,0xbf800000
    .long 0xbf800000,0xbf800000,0xbf800000,0x3f800000
    .long 0x3f800000,0xbf800000,0xbf800000,0x3f800000
    .long 0xbf800000,0x3f800000,0xbf800000,0x3f800000
    .long 0x3f800000,0x3f800000,0xbf800000,0x3f800000
    .long 0xbf800000,0xbf800000,0x3f800000,0x3f800000
    .long 0x3f800000,0xbf800000,0x3f800000,0x3f800000
    .long 0xbf800000,0x3f800000,0x3f800000,0x3f800000
    .long 0x3f800000,0x3f800000,0x3f800000,0x3f800000

hyper_edges:
    .byte 0,1,0,2,0,4,0,8,1,3,1,5,1,9,2,3
    .byte 2,6,2,10,3,7,3,11,4,5,4,6,4,12,5,7
    .byte 5,13,6,7,6,14,7,15,8,9,8,10,8,12,9,11
    .byte 9,13,10,11,10,14,11,15,12,13,12,14,13,15,14,15
    .align 2

! Index zero is the present shell; later entries fade and change hue as their
! six-frame temporal displacement grows.
hyper_ghost_palette:
    .long 0xff68e8ff,0xff38a8e0,0xff286898,0xff403878
    .long 0xff582860,0xff401c50,0xff282044,0xff142038
    .long 0xff0c182c,0xff081020,0xff050a16,0xff03050c

hyper_ghost_widths:
    .long 0x40333333,0x4019999a,0x40033333,0x3fe00000
    .long 0x3fc00000,0x3fa66666,0x3f8ccccd,0x3f733333
    .long 0x3f51eb85,0x3f333333,0x3f19999a,0x3f000000

hyper_node_palette:
    .long 0xffb8f8ff,0xff40d8ff,0xffff48b0,0xffffd060

hyper_fold_base:             .long 0x40880000 ! 4.25
hyper_fold_amplitude:        .long 0x3f0ccccd ! 0.55
hyper_world_scale:           .long 0x3f8f5c29 ! 1.12
hyper_edge_epsilon:          .long 0x3e800000 ! 0.25 pixel squared
hyper_node_half_size:        .long 0x40400000 ! 3.0 pixels
hyper_camera_radius:         .long 0x40d33333 ! 6.60
hyper_camera_radius_amplitude:.long 0x3ecccccd ! 0.40
hyper_camera_pitch_units:    .long 0x45800000 ! +/-4096 = 22.5 degrees
hyper_camera_roll_units:     .long 0x45c00000 ! +/-6144 = 33.75 degrees

    .section .bss,"aw",@nobits
    .align 5
hyper_rotations:    .space 32      ! four {sin,cos} pairs
hyper_fold_distance:.space 4
    .align 5
hyper_projected:    .space 16 * 12 ! screen X, screen Y, reciprocal Z
