# Rendering performance

The current renderer averages **47.97 FPS** in the complete Flycast tour with
the upgraded artwork. The measurements below separate the SH4ZAM integration
from the subsequent submission, geometry and final color sweeps.

## SH4ZAM integration

Measured on 2026-09-23 against game revision `6f16e9d` using the same
60-second four-district tour, 640×480 scene, assets and compiler flags.

| Build | Frames | Elapsed seconds | Average FPS |
| --- | ---: | ---: | ---: |
| Original renderer | 1,922 | 60.061827 | 32.0004 |
| SH4ZAM camera transform and reciprocals | 2,069 | 60.061323 | 34.4481 |
| Plus grouped car vertex cache | 2,079 | 60.039718 | 34.6271 |
| Plus paired sine/cosine calls | 2,080 | 60.045497 | 34.6404 |

The combined change improves the measured average by 8.25%. Most of the gain
comes from camera/reciprocal math; the smaller incremental differences should
not be treated as precise isolated costs. The tour advances with elapsed time,
so different frame rates also change the sampled animation and traffic states.
A second complete run reproduced the final row exactly. This is an average
target, not a guarantee that every frame exceeds 30 FPS.

## Changes

- Compose yaw/pitch/roll once per frame and keep the rotation in XMTRX. Each
  camera-relative vertex uses SH4ZAM's FTRV transform instead of three scalar
  rotations. Subtract the camera position first to preserve precision far
  from the streamed city's origin. The render pass owns XMTRX; another matrix
  API used inside that pass must restore the camera rotation before drawing.
- Use FSRRA reciprocals only after the existing positive near/far depth guard.
  Reflection view-vector normalization also uses reciprocal square root with
  its existing small-length floor. Driving and collision math are unchanged.
- Obtain sine/cosine together in the car, wheel, traffic and shadow helpers.
  KOS already accelerates individual sine/cosine calls; this avoids computing
  the same pair twice rather than replacing software trigonometry wholesale.
- Group each car vertex's projection, colors and reflection UVs in one
  32-byte cache line, with world position/material metadata in a second line.
  Previously several 4,096-element arrays mapped to competing locations in
  SH4's direct-mapped operand cache. This costs 76 KiB more main RAM.

The artwork, model detail, draw distances and particle limits are unchanged.
Textures/HUD remain 3,965,248 bytes; free VRAM remains 1,144,648 bytes after
initialization, including the existing 768 KiB PVR vertex buffer.

The unmodified MIT-licensed SH4ZAM 0.9.0 C headers come from the pinned Git
submodule at [`third_party/sh4zam`](third_party/SH4ZAM.md). The selected SH4 inline
APIs require no library archive, SDK installation, compiler upgrade or global
fast-math flags. The shipped binary contains FTRV, FSRRA and FSCA instructions.

## Verification and reproduction

The strict build uses KOS 2.3.0 / GCC 15.2.0 with
`-O2 -Wall -Wextra -Wpedantic -Werror`. Both benchmark builds use
`DRIFT_LA_SHOWCASE` and `DRIFT_LA_VISUAL_QA`; average FPS is rendered frames
divided by the complete run's elapsed timer interval, including audio work
and frame waits. All four districts must finish before accepting the result.

The comparison used one isolated Flycast binary, build `628bd3d`, SHA-256
`66c001b374907bc34569b0f0d10c3a36b13e8a466722f15ce589f312076c4fe5`.
Transient flags enabled the serial console and selected `audio:backend=null`;
the game still synthesized/decoded its audio normally. The same emulator and
flags were used for every row. Emulator timing and cache behavior do not
establish real-Dreamcast speed; measure the hardware before making that claim.

After sourcing KOS:

```sh
make qa-tools
make qa
make math-qa-build
# Boot render-math-qa.elf with serial output enabled.
make physics-qa-run
make qa-benchmark
```

The numerical program checks 9,292 rotations, 8,487 visible projections,
513 positive-depth reciprocals, 516 reflection normalizations and 2,053
sin/cos pairs. It includes signed angles, asymmetric vectors, world origins
up to ±100,000, zero-length protection and an XMTRX scheduler round trip.
In Flycast, the maximum projection difference was 0.000083 pixels and rotation
relative error was 0.000000117; reciprocal/normalization/trig comparisons
matched their references. Hardware approximations can have different errors;
the test uses explicit tolerances and prints the measured maxima. All 21 host
QA checks, the strict asset audit, and the Flycast burnout/donut regression
also passed. Raw logs and parsed reports are retained locally under
`assets/generated/previews/visual-qa/sh4zam/` (ignored generated evidence).

For an ongoing scalar camera/reciprocal comparison on the current code, clean
and add `-DDRIFT_LA_SCALAR_RENDER_MATH` to the strict QA CFLAGS. That switch
retains the new cache layout and paired trig; compare the original revision
above to reproduce the complete before/after change. Clean and rebuild without
QA defines before returning to the normal playable build.

## Submission and geometry sweep

Measured on 2026-09-23 against `dfe5349`, using the same emulator, transient
flags, compiler flags, scene and 60-second elapsed-time benchmark above.
Each row adds one change to the preceding row.

| Build | Frames | Elapsed seconds | Average FPS |
| --- | ---: | ---: | ---: |
| SH4ZAM baseline | 2,080 | 60.045497 | 34.6404 |
| Direct store-queue vertex submission | 2,250 | 60.039675 | 37.4752 |
| Separate uncommon quad clipping path | 2,256 | 60.044142 | 37.5724 |
| Shade only referenced car vertices/materials | 2,875 | 60.037413 | 47.8868 |
| Reuse wheel rotation pairs | 2,940 | 60.036474 | 48.9702 |

This sweep improves the average by **41.4%** over the SH4ZAM baseline. As with
the first table, changing throughput changes which animated states are sampled;
small per-row differences are not precise isolated measurements. The result
remains an emulator average, not a real-Dreamcast guarantee.
A final complete timed run reproduced the last row exactly.

- Write each complete 32-byte vertex directly into a PowerVR store queue and
  commit it, avoiding the stack array and subsequent `pvr_prim` copy. Headers
  still precede vertices, and the renderer explicitly uses immediate mode.
  `DRIFT_LA_STAGED_SUBMISSION` retains the old submission path for comparisons.
- Move near-plane clipping storage and UV setup out of the common quad path.
  Clipped vertex order, UV interpolation and depth rejection are unchanged.
- Perform the existing car backface test before projection and shading. Only
  vertices referenced by candidate faces need that work, and only paint/glass
  need reflection calculations. The reference flag fits existing cache-record
  padding, so this pass adds no car-cache RAM. Reflections still use only faces
  that reach final submission.
- Compute the car and steering sine/cosine pairs once per wheel, preserving
  the two-stage transform arithmetic and every wheel detail.

No textures, mesh detail, draw distances or particle limits were reduced.
The texture/HUD allocation and free VRAM remain 3,965,248 and 1,144,648 bytes.

### Deterministic geometry comparison

`make geometry-qa-run` fixes simulation updates at 1/30 second and reports
whole-tour triangles/vertices, peaks and frame count. This separate mode
deliberately omits the benchmark aggregate, so the FPS analyzer rejects it.
It checks geometry counts, not pixel equality or throughput.

The instrumented `dfe5349` baseline, optimized direct submission and optimized
staged submission all produced exactly **1,801 frames, 9,157,359 triangles and
23,182,243 vertices**, with peaks of **6,542 triangles / 15,966 vertices**.
Each completed all four districts over 60.032761 simulated seconds. The
strict build, 31 host QA tests, asset audit and Flycast burnout/donut regression
passed. The car, reflections, wheels, smoke and streets were also inspected
in Flycast. Logs and parsed reports are retained locally under
`assets/generated/previews/visual-qa/render-sweep/` (ignored generated evidence).

Capture a reference and changed build with identical geometry-QA defines,
then run:

```sh
uv run tools/compare_geometry_logs.py before.log after.log
```

Use `make geometry-qa-build` to produce the ELF without launching it. To compare
submission paths, clean and build with
`-O2 -Wall -Wextra -Wpedantic -Werror -DDRIFT_LA_SHOWCASE -DDRIFT_LA_VISUAL_QA -DDRIFT_LA_GEOMETRY_QA -DDRIFT_LA_STAGED_SUBMISSION`.
Clean and rebuild without QA defines before returning to the playable build.

## Final color sweep and stopping point

Measured on 2026-09-23 against the game at `f003d6e`, with the same full tour,
compiler flags and Flycast configuration. Only color inlining was retained.

| Candidate | Frames | Elapsed seconds | Average FPS | Decision |
| --- | ---: | ---: | ---: | --- |
| Previous renderer | 2,940 | 60.036474 | 48.9702 | Baseline |
| Cache traffic rotations | 2,934 | 60.046588 | 48.8621 | Reverted |
| Inline color packing | 3,013 | 60.042540 | 50.1811 | Kept |
| Color packing plus wheel-ring cache | 3,014 | 60.037792 | 50.2017 | Wheel cache reverted |

Forcing the small `pack_color` helper inline exposes its constant arguments to
the compiler. Of 258 call sites, 159 have entirely literal arguments; their
clamps, conversions and channel packing can fold. The expression and dynamic
color behavior are unchanged. This improves average FPS by **2.47%** while
reducing the benchmark ELF's text segment by **464 bytes**, with unchanged
data/BSS and VRAM allocations. Artwork and scene detail are unchanged.

The traffic cache did not improve throughput. The wheel cache added only
0.0206 FPS over color inlining while introducing persistent tables and scratch
storage, so it was also removed. This is the stopping point for this sweep:
the remaining city/box caching ideas require more complexity without a clear
expected benefit. These remain emulator results; hardware profiling is the
useful next step before more speculative optimization.

A repeat timed tour reproduced 50.1811 FPS exactly. Fixed-step geometry still
matches the previous renderer: 1,801 frames, 9,157,359 triangles and 23,182,243
vertices, with identical peaks. The strict build, all 31 host QA tests, asset
audit and Flycast driving regressions passed. Logs and reports are retained
locally under `assets/generated/previews/visual-qa/final-color-sweep/`.


## Visibility correctness fix

The earlier renderer could cull an entire visible block during a turn. For
Downtown cell `(-1,0)`, with the player at `(-10,80)` and camera yaw −21° / roll
+.08, the old centre/radius estimate rejected a building whose corner was at
screen **(193,225)**. Camera-relative frustum planes now test full block and
building bounds, including rooftop caps, projecting pavilions and landmarks.
A matched camera-pose comparison in Flycast restored the missing building.

Road geometry previously used the containing block index to choose a road.
Crossing a centreline could therefore switch lane markings, arrows and road
props to the next street. Nearest-road indices now remain stable across the
centreline. Road patches and reflections also use world segment coordinates
for their patterns, so moving the draw window does not reposition them.

`make visibility-qa-build` produces a standalone SH-4 regression program.
All **16,257 checks** passed in Flycast, including 16,200 independent box/corner
comparisons, the actual bad camera pose, frustum tangencies and signed road
boundaries. The strict build, 31 host QA checks and asset audit also passed.
Correctly restored geometry means the previous whole-tour geometry totals are
not an equality target for this fix. Artwork, draw distance and VRAM allocations
are unchanged. Evidence is retained locally under
`assets/generated/previews/visual-qa/visibility-fix/`.

The final full tour rendered **2,880 frames in 60.038410 seconds = 47.9693 FPS**,
above the 30 FPS average target. Peak submission was 6,841 triangles / 16,531
vertices. The prior 50.1811 FPS result predates these corrections and was
omitting some visible geometry. The final driving regressions passed and the
normal playable build was restored. Real Dreamcast timing remains unverified.
