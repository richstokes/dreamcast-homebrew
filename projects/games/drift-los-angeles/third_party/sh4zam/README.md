# SH4ZAM

Unmodified C headers from [SH4ZAM](https://github.com/gyrovorbis/sh4zam),
version **0.9.0**, pinned to commit
`0fd3a1e1fa0809d33198c062632b1494ec2f57df`.
The [MIT license](LICENSE) covers these files. API documentation is at
[sh4zam.com](https://sh4zam.com/).

The game uses the header-only SH4 implementations for reciprocal math,
normalization, paired sine/cosine, and XMTRX transforms. These inline calls
require no `libsh4zam.a` archive. KOS selects the SH4 backend automatically;
the selected APIs compile with GCC 15.2.0 and the game's strict warning flags.
No SDK installation or global fast-math flags are needed.

All 33 upstream C `.h` files are preserved under `include/sh4zam/`, including
the software fallback headers and transitive dependencies. C++ headers and
library source files are omitted. Other APIs may require the upstream library;
in particular, the software XMTRX backend needs the upstream library source
to provide its matrix storage. Header availability alone does not make every
SH4ZAM API usable without linking that library.

To update, check out the desired upstream commit in a separate directory,
replace this `include/sh4zam/` tree with every upstream `*.h` file while
preserving paths, and copy the upstream `LICENSE`. Update the version and
commit above, verify the copied files are byte-for-byte identical, and rerun
the game's strict build, numerical checks, physics QA, and graphics benchmark.
Keep upstream files unmodified so upgrades remain reviewable.
