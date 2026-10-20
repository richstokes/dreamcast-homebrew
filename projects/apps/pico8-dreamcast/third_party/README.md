# Runtime provenance

A native KallistiOS host for [FAKE-08](https://github.com/jtothebell/fake-08),
not a reimplementation of its core. No proprietary PICO-8 executable is needed
or included.

| Component | Repository | Revision |
| --- | --- | --- |
| FAKE-08 core and LodePNG | https://github.com/jtothebell/fake-08 | `814991a2571ad3970e386cef48f3b148aa1c27b9` |
| Z8lua | https://github.com/jtothebell/z8lua | `e6928578d46b61fd5ea30cfcf547e855a30a0553` |
| SH4ZAM (ignored clone, not pinned) | https://github.com/gyrovorbis/sh4zam | Latest `master`, fetched before each build |

`source`, `libs/z8lua` and `libs/lodepng` are copied from those revisions,
minus the unused `source/cartzip.h` pack-in archive. Other platform backends,
SDL, miniz and SimpleIni are not used. Upstream `main.cpp`, `logger.cpp` and
`hostCommonFunctions.cpp` stay for reference but are excluded from the build;
their replacements are in the project root.

## Local changes

Platform-specific changes sit under `#if defined(__DREAMCAST__)` or carry a
`Dreamcast:` comment explaining them. The `cart.cpp`/`cart.h` and `lstate.c`
changes are unconditional and unmarked. Re-apply all of these when refreshing
upstream.

- `libs/z8lua/fix32.h`: distinct 32-bit `int`/`unsigned int` conversions
  (KOS/newlib's `int32_t` is `long`), explicit unsigned wrapping in
  add/subtract so loop-overflow detection survives optimization, and a 32-bit
  divide for whole-number divisors other than -1
- `libs/z8lua/lvm.c`: upstream `O0` override removed; program counter kept in
  a local and saved before anything that can raise, call, collect or yield;
  label-address dispatch; inline table access, number comparisons and `#t`;
  iterators entered like `OP_CALL`; rare opcodes out of line; cheaper sandbox
  fallback for nil string-keyed reads
- `libs/z8lua/lstate.h`, `lstate.c`: `hookmask` moved next to `status`;
  `global_State` holds the pre-created `"__PICO8_SANDBOX"` string
- `libs/z8lua/ldo.c`: unchanged, but the Makefile defines `LUA_USE_LONGJMP`
- `source/vm.cpp`: `snprintf` for diagnostic numbers (this libstdc++ lacks
  float `std::to_string`), pause state reset when the launcher replaces a
  cart, Lua state created with the host's `lua_alloc.cpp`
- `source/Audio.cpp`, `Audio.h`, `synth.cpp`: cached note frequencies/rates
  and per-callback state, bounded reverb cursors, SH4ZAM truncation for
  oscillator phases, and a Dreamcast-only `FillMonoAudioBuffer` that keeps the
  portable loop's arithmetic and order. Twelve audio fixtures retain the
  original PCM hashes
- `source/PicoRam.h`: per-sample channel fields kept ahead of the 32-byte
  aligned reverb delay lines
- `source/graphics.cpp`, `graphics.h`, `nibblehelpers.h`: fully clipped
  sprites and off-clip map tiles rejected, direct solid `circ` and clipped
  `rectfill` paths, packed sprite and patterned-pen spans (unsupported cases
  fall back to upstream), sprite palette cache, inlined packed-pixel access
- `source/cart.cpp`, `cart.h`: cartridge storage initialized, PXA/legacy PNG
  decompression bounded against truncated data and invalid references;
  LodePNG builds with a 1 MiB per-allocation limit

## Licenses

See [FAKE-08's full notices](fake-08/LICENSE.MD), the copyright/license
headers in Z8lua and LodePNG, and the [sample licenses](../licenses/). FAKE-08
contains MIT, WTFPL, zlib and other attributed code; preserve the upstream
notices. The Dreamcast host and launcher use the repository's MIT license.
SH4ZAM's tracking checkout, header-only use and MIT notice: [SH4ZAM.md](SH4ZAM.md).

HTTPS uses the separately installed KOS `curl` port (tested 8.18.0, curl
license; source https://github.com/curl/curl/tree/curl-8_18_0, license also
embedded at `romdisk/certs/CURL-LICENSE.txt`), linking the `mbedtls` port
(tested 3.6.6, under its Apache-2.0 option; source
https://github.com/Mbed-TLS/mbedtls/tree/v3.6.6) and KOS's BSD-licensed
`libppp`. Their sources are in the SDK, not vendored here. The KOS port
disables Mbed TLS's built-in certificate date checks; `download.cpp` supplies
an explicit verification callback. Entropy comes from Mbed TLS's platform
source through KOS `/dev/urandom`.

`romdisk/certs/ca-bundle.pem` is the unmodified Mozilla root store converted
by curl (https://curl.se/ca/cacert.pem), retrieved 2026-09-24 with source data
dated 2026-08-13. SHA-256:
`f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9`. It is
MPL-2.0 licensed; see `romdisk/certs/README.txt` and `MPL-2.0.txt`. Update the
checked-in bundle and this digest together as trust roots change. The normal
player never includes the locally generated network-test root.
