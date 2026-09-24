# SH4ZAM

The [SH4ZAM](https://github.com/gyrovorbis/sh4zam) Git submodule at `sh4zam/`
contains the unmodified upstream repository, version **0.9.0**, pinned to commit
`0fd3a1e1fa0809d33198c062632b1494ec2f57df`.
The [MIT license](sh4zam/LICENSE) covers these files. API documentation is at
[sh4zam.com](https://sh4zam.com/).

From this project directory, initialize it before building with
`git submodule update --init --recursive -- third_party/sh4zam`.

The game uses the header-only SH4 implementations for reciprocal math,
normalization, paired sine/cosine, and XMTRX transforms. These inline calls
require no `libsh4zam.a` archive. KOS selects the SH4 backend automatically;
the selected APIs compile with GCC 15.2.0 and the game's strict warning flags.
No SDK installation or global fast-math flags are needed.

All 33 upstream C `.h` files are available under `sh4zam/include/sh4zam/`, including
the software fallback headers and transitive dependencies. C++ headers and
library source files are included in the submodule but are not built by this
project. Other APIs may require the upstream library;
in particular, the software XMTRX backend needs the upstream library source
to provide its matrix storage. Header availability alone does not make every
SH4ZAM API usable without linking that library.

To update, check out the desired upstream commit inside the submodule and
commit its new Git pointer in this repository. Update the version and
commit above and rerun
the game's strict build, numerical checks, physics QA, and graphics benchmark.
Keep upstream files unmodified so upgrades remain reviewable.
