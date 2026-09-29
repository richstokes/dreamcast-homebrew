# Island Explorer performance

Measured on 2026-09-29 using KOS 2.3.0, GCC 15.2.0 and the repository's native
macOS Flycast build, with its default 200 MHz emulated SH-4 and 640x480 output.
Both builds use the same extracted geometry, textures, camera distances,
visibility rules, triangle budget, fog, audio, and UI.

## Fixed-pose comparison

`make benchmark` visits each of the six travel stops at four camera angles.
Each pose warms up for 12 frames, then records 48 frames, giving 1,152 measured
frames per build. Animation time is fixed so wave height, character pose and
UI transitions do not change the workload between builds. The reference runs
first, then the optimized build, in separate emulator processes.

| Metric | Reference renderer | SH4ZAM + shared water corners |
| --- | ---: | ---: |
| Mean render CPU time, excluding `pvr_wait_ready` | 18.888 ms | 17.334 ms |
| Mean complete frame time, including camera/audio and PVR wait | 21.444 ms | 20.292 ms |
| Measured frames / total frame time | 46.63 fps | 49.28 fps |
| Starting beach, default view | 30.98 fps | 34.54 fps |

The change cuts rendering CPU time by **8.2%** and improves overall measured
throughput by **5.7%**. Triangle and visible-batch counts match at all 24 poses.
The runner rejects comparisons with different geometry counts and preserves
the per-pose logs and JSON in ignored `build/` files.

These are Flycast measurements, not physical-console benchmarks or a claim
that arbitrary gameplay stays above a particular frame rate. Waiting for the
PVR limits the gains at simpler views; dense scenes remain more expensive.

## Changes that produced the result

The original renderer already used KOS FTRV for stage vertices. SH4ZAM now also
accelerates the sky, water and explorer, supplies guarded positive-depth FSRRA
reciprocals, and calculates paired sine/cosine values. Subtracting camera position
before rotation avoids cancellation close to the camera at the far end of the
level. Collision and signed clipping calculations keep their previous math.

The largest additional gain comes from SH4ZAM's 32-byte Store Queue copy
routine: already-projected stage vertices go directly to KOS's PowerVR target.
The former intermediate three-vertex array is unnecessary. The copy variant
preserves XMTRX, and KOS still controls list boundaries and Store Queue locking.

Ocean and lagoon tiles reuse the wave, color, UV and camera transform for shared
corners within each frame. Tessellation and wave animation are unchanged. This
uses approximately 21 KB of main RAM; texture and vertex-buffer VRAM use stays
the same. Camera/math changes alone measured a smaller 2.2% render CPU reduction.

## Numerical and runtime validation

`make check-math` runs independent scalar comparisons on the emulated SH-4:

- 2,430 camera probes, including points close to the near plane and camera
  origins at the far end of Emerald Coast.
- 10,001 positive reciprocal probes from 0.8 to 40,000 units.
- 1,441 sine/cosine pairs.

All pass. The maximum projection difference is below 0.00008 pixels, normalized
transform error below 0.00000012, and trig error below 0.000096. The reciprocal
routine is used only after the positive near-plane test; signed division is
retained for triangle clipping. No global fast-math compiler option is enabled.

`make check-runtime` exercises floor/wall/camera collision, pause, mute, postcard
mode, travel and clean shutdown, then walks and jumps at all six destinations,
including swimming in the raised lagoon. `make check-import` verifies the
asset readers. The normal and diagnostic executables build with warnings
treated as errors.

## Reproduce

Extract the local assets as described in the [README](README.md), close other
Flycast instances, then run:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make clean
make -j4
make check-import check-math
make check-runtime
make benchmark
```

The build fetches current upstream SH4ZAM before compiling. Its checkout, logs,
ELFs and benchmark evidence are ignored by Git. There is no revision pin:
`git -C third_party/sh4zam rev-parse HEAD` identifies the fetched checkout,
and new benchmark reports record the tested revision and executable hashes.
Fresh upstream changes should be rechecked with the same commands.
