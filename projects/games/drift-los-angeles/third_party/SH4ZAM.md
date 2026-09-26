# SH4ZAM

The `sh4zam/` directory is a Git-ignored clone of upstream
[SH4ZAM](https://github.com/gyrovorbis/sh4zam) that contains the unmodified
repository and tracks [`master`](https://github.com/gyrovorbis/sh4zam/tree/master).
It is neither vendored nor pinned: this repository records no SH4ZAM revision.
The [MIT license](sh4zam/LICENSE) covers these files. API documentation is at
[sh4zam.com](https://sh4zam.com/).

Every build runs `tools/update-sh4zam.sh`, which clones the directory if
necessary and fast-forwards it to the latest upstream commit before reading
header dependencies. CI does the same before building. A failed clone or fetch
stops compilation. `make clean` does not fetch.

The game uses the header-only SH4 implementations for reciprocal math,
normalization, paired sine/cosine, and XMTRX transforms. These inline calls
require no `libsh4zam.a` archive. KOS selects the SH4 backend automatically;
the selected APIs compile with GCC 15.2.0 and the game's strict warning flags.
No SDK installation or global fast-math flags are needed.

The upstream C `.h` files are available under `sh4zam/include/sh4zam/`, including
the software fallback headers and transitive dependencies. C++ headers and
library source files are included in the checkout but are not built by this
project. Other APIs may require the upstream library;
in particular, the software XMTRX backend needs the upstream library source
to provide its matrix storage. Header availability alone does not make every
SH4ZAM API usable without linking that library.

Use `git -C third_party/sh4zam rev-parse HEAD` from the project directory to
see the revision used by the most recent build. The game's strict build,
numerical checks, physics QA and graphics benchmark exercise the integration.
Keep upstream files unmodified.
