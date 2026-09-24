# Runtime provenance

This is a native KallistiOS host for [FAKE-08](https://github.com/jtothebell/fake-08),
not an original reimplementation of its emulator core. No proprietary PICO-8
executable is needed or included.

The checked-in subset is pinned to:

| Component | Repository | Revision |
| --- | --- | --- |
| FAKE-08 core and LodePNG | https://github.com/jtothebell/fake-08 | `814991a2571ad3970e386cef48f3b148aa1c27b9` |
| Z8lua | https://github.com/jtothebell/z8lua | `e6928578d46b61fd5ea30cfcf547e855a30a0553` |

The `source`, `libs/z8lua`, and `libs/lodepng` directories were copied from those
revisions. The unused `source/cartzip.h` pack-in archive is deliberately omitted.
Other platform backends, SDL, miniz and SimpleIni are not needed by this port.
Upstream `main.cpp`, `logger.cpp` and `hostCommonFunctions.cpp` are preserved for
reference but excluded from the build. Their replacements are in the project root.

Local core changes (marked in the affected files):

- `libs/z8lua/fix32.h`: KOS/newlib's `int32_t` is `long`. Add distinct 32-bit
  `int`/`unsigned int` conversions under `__DREAMCAST__`, retaining 16.16 behavior.
- `source/vm.cpp`: format diagnostic numbers with `snprintf`, since this
  libstdc++ lacks floating-point `std::to_string` overloads; reset pause state
  when the Dreamcast launcher replaces a cartridge.
- `source/Audio.cpp`: cache the 64 exact integer-note frequencies on Dreamcast,
  avoiding repeated `exp2` calls without changing their results.
- `source/graphics.cpp`: reject fully clipped sprites and visit only map tiles
  that intersect the camera-adjusted clip rectangle.

See [FAKE-08's full notices](fake-08/LICENSE.MD), the copyright/license headers
in Z8lua and LodePNG, and [sample licenses](../licenses/). FAKE-08 contains
MIT, WTFPL, zlib and other attributed code; preserve the upstream notices.
The Dreamcast host and launcher use the repository's MIT license.
