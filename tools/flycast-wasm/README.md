# Flycast WebAssembly core

The Flycast core behind [dcvmu.com/play](https://dcvmu.com/play). It is
[flycast-wasm](https://github.com/nasomers/flycast-wasm) v1.0 (Flycast with a
WebAssembly JIT, for EmulatorJS 4.2.3) rebuilt from source, plus the patches
here.

```sh
./tools/flycast-wasm/build.sh
projects/dcvmu.com/service/package_flycast_core.py
```

The first command fetches everything pinned in `build.sh` (emsdk 3.1.74,
Flycast `2c48c01`, flycast-wasm v1.0, EmulatorJS's RetroArch fork) into
`~/.local/share/dreamcast/flycast-wasm` (override with `FLYCAST_WASM_WORK`)
and builds the core. It needs about 3 GB there and takes ~15 minutes the
first time; later runs rebuild only what changed. The second command packages
the result into `static/play/flycast-wasm.data` and `flycast.json`.

| Patch | Why |
| --- | --- |
| `bba-js-frames.patch` | Broadband adapter without sockets or threads. If the page defines `globalThis.flycastNet` before the game loads, the BBA is on and its Ethernet frames go through that object (contract in `core/network/jsnet.h`). Flycast's picoTCP and DCNet backends both need a thread, which this build doesn't have, so they are left out |

## Gotchas

- The build matches the v1.0 release's wasm imports and strings except for
  the BBA patch: four `flycast_net_*` imports added, picoTCP, DCNet and UPnP
  code dropped. Matching needs RetroArch's OpenAL audio and CHD (`ra_flags`
  in `build.sh`), and
  the fork as of February 2026; its May 2026 API is not what EmulatorJS 4.2.3
  calls
- EmulatorJS applies `EJS_defaultOptions` only after the game starts on a
  first visit, too late for the BBA option, hence the `flycastNet` switch
- DCNet's Ethernet tunnel (TCP 7655) wants the `DCNET\x01` prolog framed like
  any other frame: 2-byte little-endian length first
- `flycast_stubs.c` is recovered from the release's prebuilt
  `flycast_stubs.o` bitcode, so nothing is linked as a binary blob
- Upstream `audiostream.cpp` has CRLF line endings, so the JIT patch applies
  with `--ignore-whitespace`
- EmulatorJS caches the core in IndexedDB until `flycast.json`'s
  `buildStart` changes; the packager derives it from the core's hash
