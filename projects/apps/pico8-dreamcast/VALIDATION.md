# Validation on 2026-09-24

Tested with the workspace's KOS 2.3.0 (`cd34037`), SH-4 GCC 15.2.0 and the
locally built Flycast documented in `tools/flycast/README.md`, on Apple Silicon.
No real Dreamcast hardware was available for this run.

## Build and boot

- Freshly sourced KOS environment; clean C++17/O3/LTO build succeeded.
- `file`: **ELF 32-bit LSB executable, Renesas SH**, statically linked.
- `sh-elf-readelf -h`: ELF32, little-endian, SuperH, entry `0x8c010000`.
- `sh-elf-nm -C` confirms `luaV_execute`, `pvr_scene_begin`, `snd_stream_poll`,
  `curl_easy_perform`, `mbedtls_ssl_handshake`, and `ppp_connect`.
- Linked text/data/BSS: 2,903,809 / 10,776 / 114,272 bytes. This is not a
  measurement of peak heap/stack consumption.
- Normal player boots to the five-game picker; layout inspected in Flycast.
  The optimized release was also inspected after this pass; all five choices
  and the download controls were visible. Serial: `/tmp/pico8-optimized-release-boot.log`.
- Serial showed no KOS panic, unhandled exception or missing sample resource.
- KOS Ethernet and random-device support are initialized at boot. Modem dialing
  and HTTPS are only started by submitting a URL. Bundled games work offline.
- Debug sections are kept separately in `build/*.elf.debug`; the bootable ELF
  is approximately 3.1 MiB, below Flycast's 16 MiB file-size limit.

Normal ELF SHA-256 for this validation:
`18d471869309d626083cf0832cfc1c5a0317964406d088624a64d7123d677b8d`.
Build timestamps and later source changes can change this value.

## Initial cartridge performance baseline

`uv run tests/run_smoke.py --log /tmp/pico8-smoke-final.log` returned `PASS`.
All five ran for 360 VM ticks, stayed in their intended cart, produced changing
framebuffers and synthesized nonzero audio. Scripted input presses start/actions
and directions; it does not complete the games. Input is injected at the host's
PICO-8 input boundary, so this does not certify a physical controller mapping.

| Cartridge | Result | Framebuffer changes | Elapsed guest milliseconds | VM ticks/second |
| --- | --- | ---: | ---: | ---: |
| Anteform (`.p8`) | PASS | 27 | 7,695 | 46.8 |
| Bull Sheep (`.p8`) | PASS | 325 | 23,350 | 15.4 |
| Elephant in the Room (`.p8`) | PASS | 180 | 25,328 | 14.2 |
| Picolumia (`.p8.png`, PXA compressed) | PASS | 55 | 20,022 | 18.0 |
| Pikoralli (`.p8`) | PASS | 242 | 25,060 | 14.4 |

These measurements include title screens, gameplay, audio, rendering and the
test's framebuffer hashing. They are not whole-game benchmarks or hardware
measurements. **The initial version was not full speed for these samples.** The core
expects 60 VM ticks per second. Games using `_update` execute their game logic
only every other tick, so VM tick rates must not be presented as game FPS.
The player slows game time when overloaded. See the subsequent optimization
results below for the current timings and remaining limits.

Additional results:

```text
PICO8_TEST: PASS API
PICO8_TEST: PASS PICKER_PAUSE
PICO8_TEST: PASS SESSION_SAVE
PICO8_TEST: PASS ALL
PICO8: player exited cleanly status=0
```

The API cartridge checks PICO-8 syntax/fixed-point wraparound, bit operations,
table helpers, pixel/sprite/map drawing, camera/clip interaction, memory copies,
and cartdata. The picker test loads a game using the actual Lua menu, pauses and
resumes it, and replaces a paused game with the launcher. The save test writes
a value, switches carts, reloads the first cart and checks the value survives.
The normal build does not compile this scripted-input runner.

The normal picker and Bull Sheep gameplay were visually inspected. Synthetic
native-app keyboard events did not reach Flycast's SDL control mapping in this
session, so manual controller input and the physical trigger chord remain
unverified. Audio generation was checked by nonzero sample counts and AICA
stream initialization, not by a recorded listening comparison.

## SH4 optimization results

A fresh baseline was recorded immediately before this optimization pass, then
the same 360-tick scripted workload was run with audio enabled after the changes.
These are elapsed **guest** timings from Flycast; they are not measurements on
a retail Dreamcast and include cartridge startup, frame hashing, audio and
frame pacing. The normal benchmark has no PC sampler enabled.

| Cartridge | Before (ms) | After (ms) | Before ticks/s | After ticks/s | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| Anteform | 7,781 | 6,628 | 46.3 | 54.3 | 1.17x |
| Bull Sheep | 23,603 | 8,759 | 15.3 | 41.1 | 2.69x |
| Elephant in the Room | 25,379 | 13,384 | 14.2 | 26.9 | 1.90x |
| Picolumia | 20,103 | 8,556 | 17.9 | 42.1 | 2.35x |
| Pikoralli | 25,288 | 7,710 | 14.2 | 46.7 | 3.28x |

Serial logs: `/tmp/pico8-perf-before.log` and
`/tmp/pico8-perf-verified.log`. All five games passed, with 27 / 325 / 180 /
55 / 243 framebuffer changes. Randomized games can produce different final
framebuffer hashes across runs; these results are one measured run, not a
statistical performance guarantee. Repeated development runs showed the same
large improvement, with timing variation.

**The 60-tick target is still not sustained by every game.** Tick rates are
not game FPS; `_update` games run logic every other tick. Anteform's first
tick includes approximately 655 ms of startup work. Post-change median / 95th
percentile tick durations, including pacing and audio, were:

| Cartridge | Median (ms) | 95th percentile (ms) | PVR frames submitted / 360 ticks |
| --- | ---: | ---: | ---: |
| Anteform | 15.3 | 36.6 | 28 |
| Bull Sheep | 26.3 | 41.6 | 326 |
| Elephant in the Room | 36.5 | 93.7 | 181 |
| Picolumia | 15.7 | 65.6 | 70 |
| Pikoralli | 16.6 | 37.9 | 244 |

Fewer submissions reflect unchanged pixels being retained; VM and input ticks
are not skipped. Palette-only changes can produce extra submissions even when
the packed framebuffer hash stays unchanged. Long ticks can still cause audio
underruns. This pass does not certify glitch-free audio or real-hardware speed.

The changes were guided by before/after scheduler PC profiles. Upstream's Lua
dispatch function had explicitly disabled compiler optimization; it now uses
O3 after making fixed-point addition/subtraction explicitly wrap. Rendering
uses pairwise RGB565 conversion, packed sprite blits, inlined pixel access and
patterned pen spans. It skips identical PowerVR frames. Audio uses SH4ZAM's
guarded truncation, exact cached rates and metadata, bounded reverb cursors,
and smaller AICA refills. No approximate game math or global fast-math is used.

Rendering and audio regression results:

- RGB565 conversion agrees with the original algorithm for all supported
  display transforms and representative unsupported mode values.
- Packed pixel access preserves the adjacent pixel for all 256 input byte
  values. Randomized sprite comparisons cover palettes, transparency, odd/even
  destination alignment and overlapping-memory fallback.
- 512 patterned-span comparisons agree with individual `pset` operations
  across camera, clip, palettes, transparency and read/write color masks.
- Frame-cache checks cover unchanged frames, framebuffer edits, palette-only
  changes, transforms, scale changes and modal-screen invalidation.
- All 12 audio fixtures retain their original PCM hashes: eight waveforms,
  effects, loops, filters, half rate, music, custom instruments and live RAM edits.
  Total synthesis time for the same 147,456 output samples fell from 3,764,981
  to 2,692,233 guest microseconds (28.5% less). Per-fixture reductions were
  approximately 18–34%; sample counts and 22,050 Hz output were unchanged.
- Optimized VM tests pass at both signed 16.16 loop boundaries, with fractional
  steps, empty loops, arithmetic wrap, recursion, tables, closures and coroutines.
  Existing API, picker/pause, session saves and all five cartridge tests pass.

`uv run tests/run_smoke.py --kernels` additionally passed after adding the
frame-cache checks (`/tmp/pico8-optimized-kernels.log`). The release ELF excludes
the sampler and scripted tests. At the time of these measurements, all 33
SH4ZAM C headers matched upstream revision
`0fd3a1e1fa0809d33198c062632b1494ec2f57df` byte for byte, and the MIT notice
was embedded. Builds now fetch the latest upstream `master`.
The final full smoke rerun includes frame-cache checks and the short-span
fallback threshold; it ends with `PASS ALL` and clean exit status 0.

The remaining active-thread profile is dominated by Lua execution/table access,
audio mixing, and drawing primitives in Elephant. Getting these games to a
consistent 60-tick target will need further core work and measurements on real
hardware; SH4ZAM floating-point routines cannot replace PICO-8's exact fixed-point
operations without risking compatibility.

Networking was rerun after the host/audio changes. The full BBA suite passed,
including public GitHub, certificate/error cases and URL-screen launch
(`/tmp/pico8-optimized-bba.log`). A fresh modem run passed text/PNG downloads,
public GitHub, repeated dialing, launcher input and URL-screen launch, with
clean exit (`/tmp/pico8-optimized-modem-retry.log`). An earlier modem run stalled
during the public download and was interrupted; the cause was not established.
The successful fresh run does not establish that intermittent network stalls
are impossible. No SDK/network implementation changes were made in this pass.

## Bundle integrity

`uv run tests/check_samples.py`: five PASS results, with pinned SHA-256 hashes,
source availability and MIT/GPL notices checked. Four carts are byte-identical
to upstream; Elephant's includes were expanded into a single `.p8`. No changes
were made to gameplay. Picolumia's separate readable source is also flattened.

Session saves work; VMU persistence, additional disc carts, exhaustive malformed-cart
fuzzing, multiplayer, physical peripherals and real hardware have not
been validated. This suite establishes basic compatibility, not universal
PICO-8 conformance.

## HTTPS download validation

After the networking changes, the full five-cart 360-tick regression suite was
rerun: all games, API checks, picker/pause and session saves passed, with clean
exit status 0. Serial: `/tmp/pico8-smoke-network-feature.log`. Framebuffer-change
counts were 27 / 325 / 180 / 55 / 243; guest elapsed milliseconds were
7,792 / 23,436 / 25,343 / 20,155 / 25,277 in the table's cartridge order.
The existing below-full-speed limitation remains.

The final curl/Mbed TLS implementation passed both:

- `uv run tests/run_network.py --screen-slow`, with Flycast BBA /
  `DCNet=no`. Serial log: `/tmp/pico8-network-curl2.log`.
- `uv run tests/run_network.py --modem --screen-slow`, with Flycast's modem
  PPP peer / `DCNet=no`. Serial log: `/tmp/pico8-modem-final.log`.

The BBA suite passed 22 transfer cases plus URL-screen entry and launch:
text and PXA PNG carts, relative redirects and chunked bodies, 404, oversized
content, HTML, unsupported includes, truncated bodies, malformed text graphics/
SFX and compressed PNGs, malformed chunks, HTTP downgrades, redirect loops,
cancellation, unknown CA, expired certificate, wrong hostname, plain HTTP,
URL userinfo, and a pinned Bull Sheep download from public GitHub.

The modem suite dialed the saved ISP profile, negotiated PPP, downloaded and
launched text and PNG carts, downloaded Bull Sheep from GitHub, hung up, and
redialed successfully for the next request. Both suites ended with
`PASS URL_SCREEN_LAUNCH`, `PASS ALL`, and clean player exit status 0.
The on-screen keyboard, URL, progress counter, and cancel instructions were
visually inspected during a throttled modem download.

A final modem rerun (`--modem --no-public`, serial
`/tmp/pico8-menu-final.log`) also passed `LAUNCHER_INPUT_IDLE`: 360 consecutive
idle launcher frames remained identical after initial drawing, raw controller Y
requested the download screen once per press in the picker, and Y still mapped
to the PICO-8 action button in games. Text/PNG downloads, URL-screen launch and
clean exit passed again. Synthetic Mac key events were not detected by the
emulated controller, so this mapping check uses a test controller state at the
same host input boundary, not a physical peripheral.

Some automated Flycast window captures showed only the launcher's logo while
others showed the full menu. Temporary diagnostics confirmed the CPU RGB buffer
and uploaded VRAM texture retained identical hashes (`f29a6f6d`), with the
correct blue background and palette, through both observations. The discrepancy
is downstream of the uploaded texture; the Flycast display/capture cause was
not established. Diagnostic code was removed from the final build.

Tests use locally generated TLS fixture certificates appended to a separate
test ROM disk. The normal ELF's CA bundle is the unmodified Mozilla bundle;
no fixture keys or trust roots are in the release ROM.

An earlier bespoke HTTP implementation encountered an interrupt panic and a
stall in Flycast during repeated connections. It was replaced by curl, the
stack already used by the workspace's DCVMU client. The final BBA and modem
suites completed without a panic or stall. No real BBA, telephone modem,
DreamPi, or physical keyboard/controller was available to validate hardware
behavior. Scripted URL entry exercises the editor and launch flow, not a
physical peripheral.
