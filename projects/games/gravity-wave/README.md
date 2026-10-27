# Gravity Wave

<p align="center">
  <img src="../../../docs/screenshots/gravity-wave.png" alt="Gravity Wave fighter battling a biome guardian over volcanic terrain" width="640">
</p>

Gravity Wave is an infinite 3D arcade flight game for the Sega Dreamcast in the
spirit of classic on-rails space shooters. Fly a textured fighter along a
continuously generated route through four biomes, break enemy formations, hunt
each biome's guardian and keep a scoring chain alive.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave.cdi)**
for Flycast or GDEMU. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave.elf) is also available.
If you are burning a physical CD-R, you may want to try the padded
[CD-R image](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave-cdr.zip) instead (unzip before burning).

## Highlights

- Four biomes (Azure Reach, Emerald Veil, Violet Rift, Ember Crown), each with a named guardian boss and guaranteed loot
- An authored route of switchbacks, climbs, dives and canyon runs that mirrors and intensifies on later laps
- Boost, brake, barrel rolls, shields and three Nova Pulses per run
- Three permanent laser levels plus Speed Boost, Fast Laser and Phase Wave pickups
- Gravity Bloom, Vector Lane and Shear Run traversal challenges, timed chains and multipliers up to 8x
- Eight original stereo synthwave songs with a title-screen Sound Test, and optional Jump Pack feedback

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

On the title screen the D-pad (arrow keys) picks Start Flight, Sound Test or
Exit (to the Dreamcast BIOS menu, as does B) and Start or A (Return or X)
confirms. In Sound Test, Left/Right changes
song, A restarts it and B returns. While paused, B aborts the run. After a
defeat, Start or A retries and B returns to the title. Controllers can be
hot-plugged. A+B+X+Y+Start returns to the title screen, and on the title screen exits to the Dreamcast BIOS menu.

Fast Laser and Phase Wave replace each other; Speed Boost runs alongside
either, and the permanent laser level returns when a temporary weapon expires.
Nova Pulses erase enemy fire and ordinary craft but only damage a guardian. A
Laser Core collected at max level becomes score.

## Build and run

Needs the toolchain from the [root README setup](../../../README.md#one-time-setup).

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make                               # gravity-wave.elf
./run-flycast.sh                   # build, then boot in Flycast with the serial console
./run-flycast.sh --skip-build      # boot the existing ELF
./run-flycast.sh --input keyboard  # or gamepad; default takes a connected gamepad, else keyboard
```

`KOS_ENV` and `FLYCAST_BIN` override the KOS environment script and the
Flycast binary.

## Regenerating assets

The texture C files and the ADPCM album are checked in, so a normal build needs
only KOS.

| Target | Regenerates | Needs |
| --- | --- | --- |
| `make textures` | PVR-ready texture data and previews in `assets/generated/` from the atlases in `assets/source/` | `uv` |
| `make music` | The eight-song ADPCM album, followed by a clean rebuild (several minutes) | `uv`, Python 3, KOS, Flycast |

To render listening masters without touching the checked-in album:

```sh
uv run --with 'numpy==2.5.2' --with 'scipy==1.18.1' \
  tools/render_soundtrack.py --output-dir /tmp/gravity-wave-soundtrack
```

`--track` renders one song and `--quick` is a faster draft mode. The texture
converter is covered in `assets/README.md` and the atlas art prompts in
`assets/PROMPTS.md`.

## Gotchas

- Song tempo and form live in two places: `tools/render_soundtrack.py` and the
  `soundtrack_defs` / `music_song_forms` tables in `gravity-wave.c`. Change
  both, then run `make music`. The album carries a fingerprint of the runtime
  catalog and the game refuses it on a mismatch, so the music goes silent

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution. The source texture atlases were created for this project
with OpenAI ImageGen and are documented in `assets/PROMPTS.md`.
