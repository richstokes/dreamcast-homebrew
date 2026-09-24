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
- Linked text/data/BSS: 2,905,469 / 10,776 / 106,048 bytes. This is not a
  measurement of peak heap/stack consumption.
- Normal player boots to the five-game picker; layout inspected in Flycast.
- Serial showed no KOS panic, unhandled exception or missing sample resource.
- KOS Ethernet and random-device support are initialized at boot. Modem dialing
  and HTTPS are only started by submitting a URL. Bundled games work offline.
- Debug sections are kept separately in `build/*.elf.debug`; the bootable ELF
  is approximately 3.1 MiB, below Flycast's 16 MiB file-size limit.

Normal ELF SHA-256 for this validation:
`ee3d7efd10cfceb1699d2517547d6c1a16fec62f8481e17a9411ffdf56a4095f`.
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
measurements. **This version is not full speed for these samples.** The core
expects 60 VM ticks per second. Games using `_update` execute their game logic
only every other tick, so VM tick rates must not be presented as game FPS.
The player currently slows game time when overloaded; further SH-4 profiling
and optimization are needed, especially for action games and audio-heavy scenes.

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
