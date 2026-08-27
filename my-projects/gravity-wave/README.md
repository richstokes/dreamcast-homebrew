# Gravity Wave

Gravity Wave is an infinite 3D arcade flight game for Sega Dreamcast, inspired
by the speed, readable combat, and cinematic staging of classic on-rails space
shooters. Fly a richly textured aerospace fighter through a continuously
generated world, break hostile formations, hunt biome guardians, thread energy
gates, and build a long scoring chain.

The complete game is embedded in one ELF. Hand-painted source atlases are
converted into aligned RGB565 and ARGB4444 data at build time, then uploaded to
PowerVR memory during boot. No runtime disc filesystem or emulator-only feature
is required, so the same executable is suitable for Flycast and real hardware.

## Features

- Infinite deterministic terrain with material-mapped slopes, a winding energy
  river, canyon walls, distant ridges, and smoothly changing landforms
- Four fully differentiated biomes: the flooded wreckage of Azure Reach, the
  overgrown ruins of Emerald Veil, the floating crystal fields of Violet Rift,
  and the volcanic foundries of Ember Crown
- Deterministic hero landmarks including carrier ribs, root cathedrals, temple
  terraces, crystal crucibles, pipe arches, bridges, towers, and furnace gantries
- Twelve resident PowerVR textures with conditioned tile edges, ordered color
  dithering, bilinear filtering, and transparent foliage, mist, rift, and fire art
- A 90-triangle textured player fighter plus distinct interceptor, ace, bomber,
  and 112-triangle guardian meshes with per-face lighting and emissive materials
- Recurring biome guardians with sustained movement, spread fire, a dedicated
  boss HUD, Nova resistance, large destruction sequences, and guaranteed loot
- Arcade flight physics with boost, brake, barrel rolls, terrain collisions,
  structure collisions, shields, responsive FOV changes, camera trauma, and
  three Nova Pulses per run
- Three permanent laser levels plus magnetic Laser Core, Repair, Nova, Speed
  Boost, Fast Laser, and Phase Wave pickups dropped by defeated enemies
- Procedural enemy formations, aimed fire, light/ace/heavy craft, escalating
  speed, and increasing encounter pressure
- Animated Gravity Bloom accelerator gates with swept mechanical petals,
  counter-rotating safe-zone halos, and clear-burst effects
- Vector Crown environmental arches with tapered polygonal ribs, floating
  keystones, biome-colored neon traces, and shape-matched collision
- Distance scoring, timed chains, and multipliers up to 8x
- A three-track stereo chiptune/synthwave score with alternating A/B sections,
  synthesized at boot with drums, pulse bass, chord pads, arpeggios, leads,
  and ping-pong echo
- Optional Jump Pack feedback for impacts, Nova Pulses, upgrades, and guardian
  destruction, with safe play when no vibration accessory is present
- Hardware fog, a layered gradient sky, stars, striped sun, a subtle animated
  neon route grid, cyan-magenta HUD framing, additive sprites, engine ribbons,
  explosions, speed streaks, impact feedback, and vector HUD
- Title, pause, game-over, retry, and controller hot-plug handling
- A randomly selected biome backdrop every time the title screen is entered

## Controls

| Control | Action |
| --- | --- |
| Analog stick or D-pad | Fly |
| Hold A | Fire current weapon |
| Hold B | Boost while energy remains |
| Hold X | Brake |
| Press Y | Nova Pulse |
| L or R trigger | Barrel roll with brief damage immunity |
| Start | Pause or resume |

On the title screen, Start or A begins and B exits. While paused, B aborts the
run and returns to the title. After a defeat, Start or A retries and B returns
to the title.

A Dreamcast controller is required for play. Gravity Wave waits safely when one
is missing and supports hot-plugging.

## Powerups

Defeated enemies occasionally release magnetic pickups. Heavy craft have a
higher drop chance, while every biome guardian leaves a permanent upgrade, a
temporary weapon, and either repair energy or a speed pickup.

- **Speed Boost** — eight seconds of automatic overdrive, without draining the
  normal boost gauge
- **Fast Laser** — fifteen seconds of high-velocity, double-damage alternating
  laser fire
- **Phase Wave** — fifteen seconds of broad, double-damage energy crescents;
  each wave can pierce as many as four targets

Fast Laser and Phase Wave replace one another when collected, while Speed Boost
can run alongside either weapon. The underlying permanent laser level returns
when the temporary weapon expires.

## Soundtrack

The title screen identifies the active track, and biome changes rotate the
score automatically:

- **Neon Undertow** — nocturnal D-minor outrun theme
- **Chrome Canopy** — syncopated E-minor crystalline chase theme
- **Rift Burner** — aggressive C-sharp-minor pulse-wave theme

All three songs use two evolving arrangements. Dual stereo AICA pairs overlap
93 ms baked fade edges, so section and track changes crossfade cleanly on phrase
boundaries. Every song and sound effect is generated in memory during boot; no
streamed files or disc access are needed.

## Build and verify

Source KallistiOS, then build:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

This produces `gravity-wave.elf`. Verify that it is a 32-bit, little-endian
Renesas SH executable:

```sh
file gravity-wave.elf
sh-elf-readelf -h gravity-wave.elf
```

Generated `.o` and `.elf` files are build products and can be removed with
`make clean`.

## Rebuild the texture assets

The checked-in generated C files make a normal Dreamcast build independent of
host image libraries. If one of the source atlases in `assets/source/` changes,
regenerate the PVR-ready data and previews with:

```sh
make textures
```

This uses `uv` to run `tools/build_textures.py` with Pillow in an isolated
environment. The converter crops each atlas quadrant, downsamples it to a
PowerVR-compatible size, conditions wrapping material edges, applies ordered
dithering, packs RGB565 or ARGB4444 texels, and emits 32-byte-aligned C arrays.
See `assets/README.md` and `assets/PROMPTS.md` for the complete pipeline and art
prompts.

## Run in Flycast

The launcher sources the KOS environment, builds, enables Flycast's serial
console for diagnostics, and boots the ELF by absolute path:

```sh
./run-flycast.sh
```

To launch an already-built ELF:

```sh
./run-flycast.sh --skip-build
```

Override either dependency location when necessary:

```sh
KOS_ENV=/path/to/kos/environ.sh \
FLYCAST_BIN=/path/to/Flycast \
./run-flycast.sh
```

`make run` is also available, but the KOS environment must already be sourced
because the Makefile includes `$(KOS_BASE)/Makefile.rules`.

The game is offline and the launcher makes no persistent Flycast changes or
network configuration changes.

## Gameplay notes

Enemies, pickups, gates, and structures are expressed in route-relative
coordinates, so encounters follow the procedural flight path. Destroying
enemies before the chain timer expires raises the multiplier. A Gravity Bloom's
broken inner halo marks its exact scoring aperture; clearing one while boosting
earns a larger award. Nova
Pulses erase enemy fire and ordinary craft, but only damage a guardian, so
saving charges for boss patterns is usually worthwhile. Weapon upgrades last
for the current run; collecting a max-level Laser Core converts it into score.

## Verified with

- KallistiOS 2.3.0
- SH-4 GCC 15.2.0
- Flycast 2.7 with serial console enabled
- Direct ELF boot, title screen, live gameplay, enemy/gate/pickup encounters,
  guardian defeat, all four biome render paths, transitions, synthesized audio,
  structure/terrain impacts, pause, retry, and game-over flow
- 59.9–60.0 fps in dense guardian and neon-grid scenes at 640x480, with roughly
  9–12.5 ms PVR registration time and 288 KiB of resident texture data

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution. The source texture atlases were created for this project
with OpenAI ImageGen and are documented in `assets/PROMPTS.md`.
