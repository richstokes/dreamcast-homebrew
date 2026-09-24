# SH4ZAM 0.9.0

The [SH4ZAM](https://sh4zam.com/) Git submodule at `sh4zam/` is pinned to
[revision 0fd3a1e1fa0809d33198c062632b1494ec2f57df](https://github.com/gyrovorbis/sh4zam/tree/0fd3a1e1fa0809d33198c062632b1494ec2f57df).
It contains the unmodified upstream repository. This project uses only its C
API headers; the C++23 wrappers and library implementation are not built.
License: [MIT](sh4zam/LICENSE); a copy is embedded at `/rd/SH4ZAM-LICENSE.txt`.

From the repository root, initialize dependencies with
`git submodule update --init --recursive` before building.

The C++17 Dreamcast host uses the header-only `shz_truncf` through
`audio_math.h` to wrap oscillator phases with SH-4 FTRC/FLOAT instructions.
The wrapper bounds inputs and retains libm for large/nonfinite phases.
No separate SDK installation or library archive is required.

PICO-8's fixed-point game arithmetic retains its existing exact operations.
Approximate reciprocal, trigonometric and matrix routines are not appropriate
substitutes for those APIs. The build does not enable global `-ffast-math`.
Rendering uses packed integer operations; KOS already supplies the PowerVR
upload implementation.

To update, check out the desired upstream revision inside the submodule and
commit its new Git pointer in this repository. Update these version/revision
notes and `README.md`, refresh `../romdisk/SH4ZAM-LICENSE.txt` and
`../romdisk/NOTICE.txt` as needed, then rerun the build and smoke tests. Keep
upstream files unmodified.
