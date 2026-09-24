# Runtime provenance

This is a native KallistiOS host for [FAKE-08](https://github.com/jtothebell/fake-08),
not an original reimplementation of its emulator core. No proprietary PICO-8
executable is needed or included.

Dependency sources:

| Component | Repository | Revision |
| --- | --- | --- |
| FAKE-08 core and LodePNG | https://github.com/jtothebell/fake-08 | `814991a2571ad3970e386cef48f3b148aa1c27b9` |
| Z8lua | https://github.com/jtothebell/z8lua | `e6928578d46b61fd5ea30cfcf547e855a30a0553` |
| SH4ZAM (Git submodule) | https://github.com/gyrovorbis/sh4zam | Latest `master`, fetched before each build |

The `source`, `libs/z8lua`, and `libs/lodepng` directories were copied from those
revisions. The unused `source/cartzip.h` pack-in archive is deliberately omitted.
Other platform backends, SDL, miniz and SimpleIni are not needed by this port.
Upstream `main.cpp`, `logger.cpp` and `hostCommonFunctions.cpp` are preserved for
reference but excluded from the build. Their replacements are in the project root.

Local core changes (marked in the affected files):

- `libs/z8lua/fix32.h`: KOS/newlib's `int32_t` is `long`. Add distinct 32-bit
  `int`/`unsigned int` conversions under `__DREAMCAST__`, retaining 16.16 behavior.
  Addition/subtraction explicitly wrap through unsigned arithmetic, preserving
  the VM's loop-overflow detection when optimized.
- `libs/z8lua/lvm.c`: remove the upstream `O0` override for Dreamcast. The VM
  uses the project's O3/LTO and `-fwrapv`; a regression cartridge covers signed
  boundaries, fractional steps, calls, tables and coroutine yields.
- `source/vm.cpp`: format diagnostic numbers with `snprintf`, since this
  libstdc++ lacks floating-point `std::to_string` overloads; reset pause state
  when the Dreamcast launcher replaces a cartridge.
- `source/Audio.cpp`, `Audio.h`, and `synth.cpp`: cache exact integer-note
  frequencies and double-precision note rates, cache last audible notes per
  callback, and bound reverb cursors instead of repeatedly dividing. Replace
  floor/modulo only where truncation is equivalent; oscillator phases use
  SH4ZAM's guarded truncation. Twelve deterministic audio fixtures retain the
  original PCM hashes, including filters, music, custom instruments and RAM edits.
- `source/graphics.cpp`: reject fully clipped sprites and visit only map tiles
  that intersect the camera-adjusted clip rectangle.
  Dreamcast packed sprite and patterned-pen span paths preserve palette,
  transparency and write-mask behavior. Unsupported sprite cases retain the
  original rasterizer; overlapping source/destination memory uses that fallback.
  `graphics.h` holds the sprite palette cache; `nibblehelpers.h` inlines packed
  pixel access to avoid calls inside rasterizer loops.
- `source/cart.cpp` and `cart.h`: initialize cartridge storage and bound PXA/
  legacy PNG decompression, rejecting truncated data and invalid references.
  Downloads also validate text section sizes before invoking upstream parsers.
  LodePNG builds with a 1 MiB per-allocation limit.

See [FAKE-08's full notices](fake-08/LICENSE.MD), the copyright/license headers
in Z8lua and LodePNG, and [sample licenses](../licenses/). FAKE-08 contains
MIT, WTFPL, zlib and other attributed code; preserve the upstream notices.
The Dreamcast host and launcher use the repository's MIT license.
See [SH4ZAM provenance and integration](SH4ZAM.md) for the tracking submodule,
header-only usage and its MIT notice.

HTTPS uses the separately installed KOS `curl` port (tested: 8.18.0, curl license),
linking the `mbedtls` port (tested: 3.6.6), using
its Apache-2.0 option, plus KOS's BSD-licensed `libppp`. Their sources are in
the SDK, not vendored here. Mbed TLS source:
https://github.com/Mbed-TLS/mbedtls/tree/v3.6.6.
The KOS port disables built-in certificate date checks; `download.cpp` supplies
an explicit verification callback. Randomness uses Mbed TLS's platform source
through KOS `/dev/urandom`; it inherits the SDK's entropy implementation.

`romdisk/certs/ca-bundle.pem` is the unmodified Mozilla root store converted by
curl, retrieved on 2026-09-24, with source data dated 2026-08-13:
https://curl.se/ca/cacert.pem.
SHA-256: `f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9`.
It is MPL-2.0 licensed; see `romdisk/certs/README.txt` and `MPL-2.0.txt`.
Update the checked-in bundle and this digest together as trust roots change.
The normal player never includes the locally generated network-test root.

curl source: https://github.com/curl/curl/tree/curl-8_18_0.
Its license is also embedded at `romdisk/certs/CURL-LICENSE.txt`.
