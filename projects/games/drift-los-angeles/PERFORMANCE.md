# SH4ZAM rendering performance

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

The unmodified MIT-licensed SH4ZAM 0.9.0 C headers are pinned in
[`third_party/sh4zam`](third_party/sh4zam/README.md). The selected SH4 inline
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
