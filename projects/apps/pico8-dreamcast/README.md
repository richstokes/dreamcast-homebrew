# PICO-8 player for Dreamcast

A KallistiOS port of [FAKE-08](https://github.com/jtothebell/fake-08), with a
controller-driven cartridge picker and five freely licensed games embedded
in the ELF. This interprets actual PICO-8 cartridge code using Z8lua's 16.16
fixed-point numbers and PICO-8 syntax. It is a cartridge runtime, not the
proprietary PICO-8 editor/development environment. It is not affiliated with
Lexaloffle.

## Build and play

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make -C projects/apps/pico8-dreamcast -j8
./projects/apps/pico8-dreamcast/run-flycast.sh --skip-build
```

Run those commands from the repository root. The launcher builds by default
when `--skip-build` is omitted, resolves absolute paths and respects `KOS_ENV`
and `FLYCAST_BIN`. It uses transient serial-console configuration and requires
no network. All interpreter dependencies and cartridges are checked in.
The baseline is KOS 2.3.0 / SH-4 GCC 15.2.0. Build/link use `kos-c++` and KOS's
`Makefile.rules`; Z8lua's `.c` sources must also compile as C++.

## Controls

| Dreamcast control | Action |
| --- | --- |
| D-pad or analog stick | Select a game / PICO-8 directions |
| Left/right in the picker | Show game credits |
| A or X | PICO-8 O button (button 4) / launch selected game |
| B or Y | PICO-8 X button (button 5) / launch selected game |
| Start | Pause/resume; games can provide their own pause-menu items |
| Hold both triggers and press Start | Return to the cartridge picker |

In Flycast, use the keys assigned to these Dreamcast controls in your existing
controller mapping. No persistent input configuration is changed by the launcher.
Missing/disconnected controllers are handled without crashing.

Picolumia starts with **A+B together**. Pikoralli uses left/right to steer,
up/down to accelerate/reverse and A for the handbrake. The other games show
their controls in-game. Anteform's pause menu includes help.

## Bundled games and adding your own

Anteform, Pikoralli, Elephant in the Room, Bull Sheep and Picolumia are bundled.
See [sample selection, sources and licenses](samples/README.md). These were
selected for verifiable MIT/GPL licensing; they are not a global top-five chart.

Copy additional `.p8` or `.p8.png` cartridges into `romdisk/carts/`, then run
`make`. They appear automatically in the picker and become part of the ELF.
Keep any `.p8` `#include` files beside their cartridge in the expected relative
paths. The ROM disk is regenerated when carts are added, changed or removed.
For a disc build, an additional `/carts/`
directory at the CD root is scanned as `/cd/carts/`; real CD/GDEMU loading has
not been tested on hardware. The five built-in games never depend on it.

The ROM disk is mounted read-only at `/rd`. There is no network browser or
Splore integration. Unknown PNGs are not game cartridges: use PICO-8's cart
PNG format, which carries the code and data inside the image.

## Runtime support and limits

- PICO-8 Lua dialect through Z8lua, drawing primitives, sprites/maps, palettes,
  camera/clip, memory operations and the upstream FAKE-08 APIs.
- 128x128 RGB565 texture rendered through PowerVR at a centered 3x integer
  scale on a 640x480 output. No smoothing is applied.
- Four-channel PICO-8 music/SFX synthesis, mixed to 22,050 Hz mono PCM for AICA.
  The host polls audio on the VM thread, avoiding races with cart resets.
- A 60 Hz VM tick target; the core schedules `_update` at 30 Hz and
  `_update60` at 60 Hz. This is a target, not a guarantee: demanding carts
  slow down on the 200 MHz SH-4. See the measured smoke-test timings in
  [VALIDATION.md](VALIDATION.md).
- `cartdata`/`dget`/`dset` persist across cartridge switches **for this session
  only** (up to 32 keys). There is no VMU persistence; quitting loses saves.
- One controller/player. Mouse, keyboard text input, networking, screenshots,
  external file writes and save states are not implemented by this host.
- Compatibility inherits FAKE-08's limitations. A passing short smoke test
  does not certify an entire game or compatibility with every PICO-8 version.
  Bad-cart/runtime errors return to the picker and are reported on serial.

## Verification

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
cd projects/apps/pico8-dreamcast
file pico8-dreamcast.elf
sh-elf-readelf -h pico8-dreamcast.elf
uv run tests/check_samples.py
uv run tests/run_smoke.py
```

The smoke runner builds a separate `pico8-smoke.elf`, boots it in Flycast and
checks serial results. It exercises 360 VM ticks for each game with scripted
input, checks framebuffer changes, reports audio sample counts and timings,
tests fixed-point arithmetic/graphics/memory/cartdata, and drives the real
picker and pause/resume path. It stops only the Flycast process it starts.
Use `--timeout 300 --log /tmp/pico8-smoke.log` to customize it. It needs a
desktop session; it is not a headless unit test.

For visual inspection, `./run-flycast.sh --smoke-test` leaves test execution
visible. Use `make clean` to remove generated objects and ELF/ROM-disk outputs.
Normal `make` never enables scripted controls.

The Dreamcast host/launcher use the repository's MIT license. See
[runtime provenance and local patches](third_party/README.md) and the
individual [game license notices](licenses/). Full license notices are also
embedded in the ELF's ROM disk at `/rd/NOTICE.txt`.
