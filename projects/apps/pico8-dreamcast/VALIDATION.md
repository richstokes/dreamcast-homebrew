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

## Real hardware (2026-10-16)

A retail Dreamcast with a Broadband Adapter (HIT-0400), booting dcload-ip
2.0.3 from GDEMU/openMenu, driven by `tools/dc-hw.sh` (Shelly plug power
cycling). Two controllers, two VMUs and two Puru Puru packs were attached.
Same SDK and compiler as above, plus the maple patch described below. Logs
are under `build/hw/` (ignored).

Smoke-test seconds for 360 VM ticks (6.0 = 60 ticks per second), with the VM
and audio parts of that time. "Before" is the September build on the same
console; "after" is the final build of this pass (`make hwbench`).

| Cartridge | Before | VM | Audio | After | VM | Audio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Anteform | 7.0 | 2.07 | 2.45 | 6.8 | 1.44 | 0.71 |
| Bull Sheep | 13.7 | 7.39 | 5.54 | 7.8 | 5.86 | 0.90 |
| Elephant in the Room | 16.4 | 8.99 | 5.25 | 10.6 | 7.67 | 1.13 |
| NanoMan | | | | 6.7 | 3.27 | 2.01 |
| Picolumia | 12.5 | 6.39 | 4.50 | 8.4 | 4.99 | 1.31 |
| Pikoralli | 15.1 | 5.63 | 8.88 | 7.6 | 5.06 | 1.89 |
| WIZRND | | | | 6.4 | 1.47 | 1.19 |

Every result, kernel check, API/picker/pause/session test and the 12 audio
PCM hashes passed. Scripted input makes runs differ slightly (Picolumia and
Elephant vary by up to about a second between runs). The console is slower
than Flycast's guest timing for the same build: Flycast reports 6.0-9.1 s.

What changed, in order of measured effect:

- **Audio**: synthesis cost 20-29 microseconds per sample, 35-60% of the CPU.
  The Dreamcast mixer keeps the portable loop's arithmetic and order but
  hoists per-buffer state, inlines the SFX stepping, uses tables for note
  volumes and filter settings, replaces the per-sample vibrato `fmod` with
  an exact truncation, and brings silent channels up to date in one step
  between music events. The fixtures fell from 327-644 ms to 149-383 ms.
  Hashes depend on code generation (GCC fuses multiply-adds differently at
  `-O2`), so the audio files stay at `-O3` where the hashes were recorded.
- **Lua allocator**: KOS's `mutex_unlock` always scans its wait queues, and
  newlib takes that lock for each allocation. A lock-free small-block
  allocator for the player thread's Lua state cut Bull Sheep's VM time by a
  quarter.
- **`-O2`**: faster than `-O3` for the VM and rasterizers on this 8 KiB
  instruction cache. Layout matters: aligning the audio state alone changed
  the mixer's speed by about 20%.
- **Lua errors and yields** use `longjmp` instead of C++ exceptions. FAKE-08
  yields once per frame; each yield was a C++ throw.
- Solid `circ` and clipped `rectfill` rows draw directly (Elephant's iris
  transition draws up to 180 large circles per frame). A new kernel check
  compares solid circles with the per-pixel path.

The remaining time is mostly Z8lua's dispatch loop and string-keyed table
lookups.

Problems found on hardware:

- **KOS maple bug.** Builds whose memory layout put a dirty cache line over a
  maple receive buffer failed during boot with `Assertion "sizeof(cont_cond_t)
  == ..." failed at controller.c:170`, every time. The buffer lives in
  `calloc()`ed memory zeroed through the cache, is filled by DMA and read
  uncached; evicting a stale line overwrote the controller's reply.
  [patches/kos-maple-rx-cache.patch](patches/kos-maple-rx-cache.patch) writes
  back and invalidates those lines when a frame is initialized. With it, the
  failing build passed every run. CI applies it.
- **Networking under dcload-ip.** The normal ELF (INIT_NET) hung while KOS
  took the adapter over from dcload-ip. Hardware test builds and
  `pico8-hw.elf` start without networking. Download & Play was not tested on
  hardware.
- **Uploads.** About half the uploads after a program had exited did not start
  (dcload-ip accepted the upload and went back to waiting); one run hung in
  KOS's ISO9660 init. `dc-hw.sh` retries silent starts, power-cycling before
  the last try, and `--idle` stops runs that go quiet. dcload-ip's DHCP
  address changed several times (.74, .93, .249); the script now finds it.

`pico8-hw.elf` booted to the picker in 3 seconds and was left running for
interactive play. Physical controller play, sound quality by ear and the
disc image on GDEMU were not checked in this pass.

## Lua interpreter on hardware (2026-10-16, second pass)

Same console, SDK and `tools/dc-hw.sh` harness as above. Seconds for 360 VM
ticks, VM part / whole run, mean of two `make hwbench` runs each. "Before" is
commit `a29c3a8`, the final build of the pass above.

| Cartridge | VM before | VM after | Change | Run before | Run after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Anteform | 1.44 | 1.34 | −7% | 6.78 | 6.73 |
| Bull Sheep | 5.83 | 4.88 | −16% | 7.85 | 6.84 |
| Elephant in the Room | 7.32 | 6.04 | −17% | 10.13 | 8.32 |
| NanoMan | 3.24 | 3.25 | 0% | 6.77 | 6.52 |
| Picolumia | 5.40 | 4.56 | −16% | 9.34 | 7.73 |
| Pikoralli | 5.24 | 3.82 | −27% | 7.79 | 6.66 |
| WIZRND | 1.35 | 1.21 | −10% | 6.50 | 6.35 |

Every result, kernel check, API/conformance/picker/pause/session test and the
12 audio PCM hashes passed on the console and in Flycast
(`uv run tests/run_smoke.py`). Elephant, NanoMan and Pikoralli are
deterministic and ended on the same frame hash as before in every run;
Anteform has two possible endings in both builds, and Picolumia's run time is
bimodal in both (7.7 or 9-10 s). Individual runs of the same build differ by
about 5%.

**The 60-tick target is not met by all four slow carts.** Pikoralli
(6.6-6.7 s) and Bull Sheep (6.6-7.1 s) are close to it. In Elephant, drawing
(`_private_h_line` and `circ`) is now about 30% of the samples. Picolumia
makes about 1,500 C API calls per tick, mostly `pal()`, and its audio costs
1.2-1.8 s.

Z8lua changes, in the order they were made:

- **Program counter in a register.** `luaV_execute` used to load and store
  `ci->u.l.savedpc` for every instruction. It now keeps `pc` in a local and
  stores it before anything that can raise an error, call, run the collector
  or yield, as Lua 5.4 does.
- **Dispatch through a table of label addresses** (GCC labels-as-values),
  with no range check. `L->hookmask` moved within the SH-4's 15-byte `mov.b`
  displacement, so the per-instruction hook test is one load.
- **Inline table access.** GETTABUP, GETTABLE, SELF, SETTABUP and SETTABLE
  look up short-string keys and array-part integers directly. When the key
  is missing and there is no `__index`/`__newindex`, they finish the operation
  without calling `luaV_gettable` or `luaV_settable`, which would repeat the
  lookup. Number comparisons and `#t` without `__len` are also inline.
- **FAKE-08's sandbox fallback** (string-keyed reads that produce nil) used
  to intern `"__PICO8_SANDBOX"` on every such read. The key is now created
  once. The second lookup is skipped when the table that missed is the
  sandbox itself.
- **Exact fix32 division by whole numbers** uses a 32-bit divide instead of
  the 64-bit `__divdi3` call. (a·2¹⁶)/(d·2¹⁶) truncates like a/d, and −1 is
  excluded.
- **Iterators run in the caller's VM invocation.** A Lua iterator such as
  `all()` used to be called through `luaD_call`, a fresh `luaV_execute` on
  the C stack, once per loop iteration. OP_TFORCALL now enters it the way
  OP_CALL does, and its OP_RETURN resumes the loop. In single runs this cut
  Bull Sheep's VM time by about 5% and Pikoralli's by about 7%. Adding new
  keys directly (above) took about 6% more off Bull Sheep, which builds four
  small tables per collision test.
- **Rare opcodes out of line.** Bitwise operators, POW, IDIV, PEEK, CONCAT,
  SETLIST, CLOSURE and VARARG now run outside the main loop, which is
  6.9 KiB (7.0 KiB before) despite the new fast paths.

Tried and dropped: a `switch` dispatch with the range check removed was no
faster. Moving DIV and MOD out of line, which would remove the FPSCR mode
switches GCC adds around every call in a function with labels-as-values
dispatch, was about 4% slower. GCC cannot split cold blocks into a separate
section on this target (`-freorder-blocks-and-partition` is disabled).

New regression checks (`romdisk/tests/performance.p8`): exact division for
108 whole and fractional divisor cases, checked against the original 64-bit
formula; `%` and `\`; array, hash, fractional and out-of-range keys;
`__index`, `__newindex`, `__len`, `__eq`, `__lt`, `__le`; the `_ENV`
sandbox fallback; iterators that yield inside a coroutine or raise errors;
and the reported line of six kinds of runtime error. A stale saved `pc` gives
the wrong line, and removing one `savepc` made this check fail. The checks
also pass with the original interpreter.

Problems found:

- **The test carts' `assert` never failed.** FAKE-08's `assert()` prints and
  calls `stop()`, which the main loop clears, so failed checks in `api.p8`
  and `performance.p8` went unnoticed (an `assert(false)` still passed).
  Both carts now override `assert` to log `PICO8_TEST: FAIL ASSERT` and
  raise a runtime error. The existing checks still pass.
- **Audio speed depended on the ROM disk's size.** Adding about 9 KiB of
  test code moved every later data address. The mixer then took up to 60%
  longer in-game (NanoMan's audio went from 1.8 to 2.9 s). It also changes
  speed between builds whose audio code is identical: the kernel fixtures
  took 168, 217 and 171 ms in three such builds. Padding the image by
  another 7 KiB, to a 16 KiB move (the size of the direct-mapped operand
  cache), restored the earlier times. The Makefile now pads `romdisk.img` to
  a multiple of 16 KiB. Code changes still move data.
- **Two runs out of 21 stopped producing output** (90 s idle limit), each
  right after a burst of cart-loading log lines and both with the same
  intermediate build, which completed its other three runs. The dcload-ip
  freeze described above fits this. Five later runs with the same
  interpreter code completed, and Flycast never hung. The cause was not
  established.

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
