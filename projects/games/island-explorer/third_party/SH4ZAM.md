# SH4ZAM

`sh4zam/` is an ignored clone of [upstream SH4ZAM](https://github.com/gyrovorbis/sh4zam),
tracking its latest `master`. No library source, archive, submodule or version
pin is committed. [API documentation](https://sh4zam.com/).

The Makefile uses the repository's `tools/sh4zam.mk` wrapper, as do Drift Los
Angeles and PICO-8 Player. Before an inner make reads dependencies, the wrapper
runs `tools/update-sh4zam.sh`: a missing checkout is cloned; an existing one is
fetched and fast-forwarded. A failed fetch or non-fast-forward update stops the
build. `make clean` works without fetching. `run-flycast.sh --skip-build` runs
the already-built executable without fetching or rebuilding.

The fetched C headers implement these operations inline on the SH-4:

- XMTRX camera rotation and FTRV vertex transformation, including sky, water
  and character vertices. Camera-relative subtraction precedes rotation to
  retain precision at the far end of the level.
- FSRRA positive-depth reciprocals, only after the near-plane guard. Signed
  clipping divisions and collision arithmetic retain their original behavior.
- Paired FSCA sine/cosine for character rendering.
- Direct 32-byte Store Queue copies of aligned stage vertices to PowerVR,
  using KOS's direct-render targets while KOS holds the queue lock. These
  copies preserve XMTRX and avoid an intermediate triangle array.

The rendering pass owns XMTRX; helpers called within the pass must not overwrite
its camera matrix. No global fast-math flags or separate SH4ZAM library build
are required. GCC 15.2.0 is the locally tested compiler.

Shared water corners also cache their wave calculation and transformed position
for one frame. This preserves the same ocean geometry and animation, at a cost
of approximately 21 KB of main RAM and no extra VRAM.

The fetched upstream MIT notice is embedded in every game ELF by `assets.S`.
The complete checkout and its license remain available locally in `sh4zam/`.
Use `git -C third_party/sh4zam rev-parse HEAD` to inspect the current build's
revision. Recording a revision in local benchmark evidence does not pin builds.

Run `make check-math` for independent scalar comparisons on the SH-4 and
`make benchmark` for a fixed-pose before/after comparison. The reference variant
keeps the former KOS stage transform, scalar effects transform and division.
See [performance notes](../PERFORMANCE.md) for measurements and limits.
