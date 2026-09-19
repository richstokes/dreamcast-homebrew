# Gravity Wave

Gravity Wave is an infinite 3D arcade flight game for Sega Dreamcast, inspired
by the speed, readable combat, and cinematic staging of classic on-rails space
shooters. Fly a richly textured aerospace fighter through a continuously
generated world, break hostile formations, hunt biome guardians, master varied
traversal runs, and build a long scoring chain.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave.elf)
is also available.

## Features

- Infinite terrain built around an authored route of switchbacks, climbs,
  valley dives, and canyon runs that mirrors and intensifies on later laps
- Four biomes: the flooded wreckage of Azure Reach, the overgrown ruins of
  Emerald Veil, the floating crystal fields of Violet Rift, and the volcanic
  foundries of Ember Crown
- A named guardian boss for each biome, with guaranteed loot
- Arcade flight with boost, brake, barrel rolls, shields, and three Nova
  Pulses per run
- Three permanent laser levels plus Repair, Nova, Speed Boost, Fast Laser, and
  Phase Wave pickups
- Traversal challenges: Gravity Blooms, Vector Lanes, and Shear Runs
- Distance scoring, timed chains, and multipliers up to 8x
- Eight original stereo synthwave songs with a title-screen Sound Test
- Optional Jump Pack feedback
- Title, pause, game-over, and retry flow with controller hot-plugging

## Controls

| Dreamcast controller | Flycast keyboard default | Action |
| --- | --- | --- |
| Analog stick or D-pad | I/J/K/L or arrow keys | Fly |
| Hold A | Hold X | Fire current weapon |
| Hold B | Hold C | Boost while energy remains |
| Hold X | Hold S | Brake |
| Press Y | Press D | Nova Pulse |
| L or R trigger | F or V | Barrel roll with brief damage immunity |
| Start | Return | Pause or resume |

On the title screen, use the D-pad (arrow keys) to choose **Start Flight**,
**Sound Test**, or **Exit**, then confirm with Start or A (Return or X). In
Sound Test, Left/Right changes song, A (X) restarts it, and B (C) returns.
While paused, B (C) aborts the run. After a defeat, Start or A retries and
B returns to the title.

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

## Tips

Destroying enemies before the chain timer expires raises the multiplier, and
clean, boosted lines through traversal challenges earn larger awards. Nova
Pulses erase enemy fire and ordinary craft but only damage a guardian, so save
charges for boss patterns. A Laser Core collected at max level becomes score.

## Soundtrack

Eight songs play from a shuffle bag, so every song is heard once before any
repeats: Midnight Vector, Magenta Circuit, Glass Horizon, Static Heart,
Afterimage Run, Neon Afterburn, Chrome Devotion, and Redline Prophecy. The
score is rendered by `tools/render_soundtrack.py` and embedded in the ELF as
AICA ADPCM.

## Build

Source KallistiOS, then build:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

This produces `gravity-wave.elf`.

## Rebuild the texture assets

The checked-in generated C files make a normal Dreamcast build independent of
host image libraries. If one of the source atlases in `assets/source/` changes,
regenerate the PVR-ready data and previews with:

```sh
make textures
```

This needs `uv`. See `assets/README.md` and `assets/PROMPTS.md` for the pipeline
and art prompts.

## Rebuild the soundtrack asset

The checked-in ADPCM album makes normal builds fast and independent of the host
audio toolchain. After changing `tools/render_soundtrack.py`, the track catalog,
form timing, album format, or streaming contract, regenerate it with:

```sh
make music
```

This renders and audits all eight songs, encodes them to AICA ADPCM, and
rebuilds. It needs `uv`, Python 3, KallistiOS, and Flycast, and takes several
minutes. When changing tempo or form, keep `render_soundtrack.py`,
`soundtrack_defs`, and `music_song_forms` synchronized.

To render listening masters without replacing the checked-in asset:

```sh
uv run --with 'numpy==2.5.2' --with 'scipy==1.18.1' \
  tools/render_soundtrack.py --output-dir /tmp/gravity-wave-soundtrack
```

The renderer also accepts `--track` and a faster `--quick` mode.

## Run in Flycast

The launcher sources the KOS environment, builds, enables Flycast's serial
console for diagnostics, selects one host input, and boots the ELF by absolute
path:

```sh
./run-flycast.sh
```

To launch an already-built ELF:

```sh
./run-flycast.sh --skip-build
```

On macOS the launcher uses a connected gamepad, or the keyboard if none is
present. To force one:

```sh
./run-flycast.sh --input gamepad
./run-flycast.sh --input keyboard
```

Override either dependency location when necessary:

```sh
KOS_ENV=/path/to/kos/environ.sh \
FLYCAST_BIN=/path/to/Flycast \
./run-flycast.sh
```

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution. The source texture atlases were created for this project
with OpenAI ImageGen and are documented in `assets/PROMPTS.md`.
