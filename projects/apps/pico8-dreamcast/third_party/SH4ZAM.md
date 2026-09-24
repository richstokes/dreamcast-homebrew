# SH4ZAM

The [SH4ZAM](https://sh4zam.com/) Git submodule at `sh4zam/` tracks upstream
[`master`](https://github.com/gyrovorbis/sh4zam/tree/master).
It contains the unmodified upstream repository. This project uses only its C
API headers; the C++23 wrappers and library implementation are not built.
License: [MIT](sh4zam/LICENSE); a copy is embedded at `/rd/SH4ZAM-LICENSE.txt`.

Every build initializes the submodule if necessary and fetches the latest
upstream commit before reading header dependencies. CI also updates it before
building. A failed fetch stops compilation. `make clean` does not fetch.

The C++17 Dreamcast host uses the header-only `shz_truncf` through
`audio_math.h` to wrap oscillator phases with SH-4 FTRC/FLOAT instructions.
The wrapper bounds inputs and retains libm for large/nonfinite phases.
No separate SDK installation or library archive is required.

PICO-8's fixed-point game arithmetic retains its existing exact operations.
Approximate reciprocal, trigonometric and matrix routines are not appropriate
substitutes for those APIs. The build does not enable global `-ffast-math`.
Rendering uses packed integer operations; KOS already supplies the PowerVR
upload implementation.

The build refreshes `../romdisk/SH4ZAM-LICENSE.txt` from the fetched upstream
license before creating the ROM disk. Keep upstream files unmodified. Use
`git -C third_party/sh4zam rev-parse HEAD` from the project directory to see
the revision used by the most recent build.
