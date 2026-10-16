# Runtime provenance

This is a native KallistiOS host for [FAKE-08](https://github.com/jtothebell/fake-08),
not an original reimplementation of its emulator core. No proprietary PICO-8
executable is needed or included.

Dependency sources:

| Component | Repository | Revision |
| --- | --- | --- |
| FAKE-08 core and LodePNG | https://github.com/jtothebell/fake-08 | `814991a2571ad3970e386cef48f3b148aa1c27b9` |
| Z8lua | https://github.com/jtothebell/z8lua | `e6928578d46b61fd5ea30cfcf547e855a30a0553` |
| SH4ZAM (ignored clone, not pinned) | https://github.com/gyrovorbis/sh4zam | Latest `master`, fetched before each build |

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
  uses the project's -O2/LTO and `-fwrapv`; a regression cartridge covers signed
  boundaries, fractional steps, calls, tables and coroutine yields.
  `luaV_execute` keeps the program counter in a local, storing it to
  `savedpc` before anything that can raise, call, collect or yield (as in
  Lua 5.4). It dispatches through a label-address table under GCC.
  GETTABUP/GETTABLE/SELF/SETTABUP/SETTABLE read and write existing
  short-string and array-part keys inline, and add missing keys directly when
  no `__index`/`__newindex` applies. Number comparisons and `#t` without
  `__len` are inline. Lua iterators in generic `for` loops are entered like
  OP_CALL instead of through a nested `luaD_call`. Rare opcodes (bitwise,
  POW, IDIV, PEEK, CONCAT, SETLIST, CLOSURE, VARARG) run out of line. The
  sandbox fallback for nil string-keyed reads uses a pre-created key and
  skips the second lookup when the table that missed is the sandbox.
- `libs/z8lua/lstate.h` and `lstate.c`: `lua_State::hookmask` moved next to
  `status` (within the SH-4's byte-load displacement); `global_State` holds
  the fixed `"__PICO8_SANDBOX"` string.
- `libs/z8lua/fix32.h`: on Dreamcast, division by a whole number other than
  -1 uses a 32-bit divide, which gives the same result as the 64-bit formula.
- `source/vm.cpp`: format diagnostic numbers with `snprintf`, since this
  libstdc++ lacks floating-point `std::to_string` overloads; reset pause state
  when the Dreamcast launcher replaces a cartridge.
- `source/Audio.cpp`, `Audio.h`, and `synth.cpp`: cache exact integer-note
  frequencies and double-precision note rates, cache last audible notes per
  callback, and bound reverb cursors instead of repeatedly dividing. Replace
  floor/modulo only where truncation is equivalent; oscillator phases use
  SH4ZAM's guarded truncation. A Dreamcast-only `FillMonoAudioBuffer` keeps
  the portable loop's arithmetic and order (including `rand()` draws) but
  hoists per-buffer state, uses tables for note volumes and filter settings,
  and updates silent channels in one step between music pattern changes.
  Twelve deterministic audio fixtures retain the original PCM hashes,
  including filters, music, custom instruments and RAM edits.
- `source/PicoRam.h`: on Dreamcast, keep each channel's per-sample fields
  ahead of its reverb delay lines, which are 32-byte aligned.
- `source/vm.cpp`: create the Lua state with the host's lock-free
  small-block allocator (`lua_alloc.cpp`).
- `libs/z8lua/ldo.c` is unchanged, but the Makefile defines
  `LUA_USE_LONGJMP` so Lua errors and coroutine yields do not throw C++
  exceptions.
- `source/graphics.cpp`: reject fully clipped sprites and visit only map tiles
  that intersect the camera-adjusted clip rectangle. Solid `circ` outlines
  plot clipped pixels directly, `rectfill` skips rows outside the clip window
  and solid spans are filled a word at a time.
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
See [SH4ZAM provenance and integration](SH4ZAM.md) for the tracking checkout,
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
