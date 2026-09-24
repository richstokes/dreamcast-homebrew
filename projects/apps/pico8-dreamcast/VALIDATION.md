# Validation on 2026-09-24

Tested with the workspace's KOS 2.3.0 (`cd34037`), SH-4 GCC 15.2.0 and the
locally built Flycast documented in `tools/flycast/README.md`, on Apple Silicon.
No real Dreamcast hardware was available for this run.

## Build and boot

- Freshly sourced KOS environment; clean C++17/O3/LTO build succeeded.
- `file`: **ELF 32-bit LSB executable, Renesas SH**, statically linked.
- `sh-elf-readelf -h`: ELF32, little-endian, SuperH, entry `0x8c010000`.
- `sh-elf-nm -C` confirms `luaV_execute`, `pvr_scene_begin`, and `snd_stream_poll`.
- Linked text/data/BSS: 1,598,325 / 8,604 / 85,912 bytes. This is not a
  measurement of peak heap/stack consumption.
- Normal player boots to the five-game picker; layout inspected in Flycast.
- Serial showed no KOS panic, unhandled exception or missing sample resource.
- No networking is initialized or needed.

Normal ELF SHA-256 for this validation:
`bd054b48a8e38155c99d9d2b4c96817e9755b4f1fafa7f988471714b4d1bed32`.
Build timestamps and later source changes can change this value.

## Cartridge smoke suite

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

Session saves work; VMU persistence, additional disc carts, malformed-cart
fuzzing, multiplayer, mouse/keyboard peripherals and real hardware have not
been validated. This suite establishes basic compatibility, not universal
PICO-8 conformance.
