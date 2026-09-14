# Gravity Wave

Gravity Wave is an infinite 3D arcade flight game for Sega Dreamcast, inspired
by the speed, readable combat, and cinematic staging of classic on-rails space
shooters. Fly a richly textured aerospace fighter through a continuously
generated world, break hostile formations, hunt biome guardians, master varied
traversal runs, and build a long scoring chain.

The complete game is embedded in one ELF. Hand-painted source atlases are
converted into aligned RGB565 and ARGB4444 data at build time, then uploaded to
PowerVR memory during boot. No runtime disc filesystem or emulator-only feature
is required, so the same executable is suitable for Flycast and real hardware.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave.elf)
is also available.

## Features

- Infinite deterministic terrain wrapped around a designed six-phrase route
  grammar with tight switchbacks, sustained S-bends, steep climbs, crests,
  valley dives, a winding energy river, material-mapped canyon walls, and
  distant ridges; later laps mirror and intensify the authored flight line
- Four fully differentiated biomes: the flooded wreckage of Azure Reach, the
  overgrown ruins of Emerald Veil, the floating crystal fields of Violet Rift,
  and the volcanic foundries of Ember Crown
- Deterministic hero landmarks including carrier ribs, root cathedrals, temple
  terraces, crystal crucibles, pipe arches, bridges, towers, and furnace gantries
- Twelve resident PowerVR textures with conditioned tile edges, ordered color
  dithering, bilinear filtering, and transparent foliage, mist, rift, and fire art
- A 90-triangle textured player fighter plus distinct interceptor, ace, bomber,
  and 112-triangle guardian meshes with per-face lighting and emissive materials
- Four named biome guardians with distinct composite silhouettes, movement
  language, telegraphed attack geometry, cadence, damage, durability, dedicated
  boss HUD identity, Nova resistance, destruction sequences, and guaranteed loot
- Arcade flight physics with boost, brake, barrel rolls, anticipatory turn
  banking, terrain and structure collisions, shields, responsive FOV changes,
  camera trauma, rigid crest/valley model transforms, and three Nova Pulses
  per run
- Three permanent laser levels plus magnetic Laser Core, Repair, Nova, Speed
  Boost, Fast Laser, and Phase Wave pickups dropped by defeated enemies
- Straight world-space weapon rays that converge on the HUD reticle, predictive
  enemy fire, swept high-speed hit detection, and terrain-anchored impacts
- Biome-authored enemy formations, light/ace/heavy craft, escalating speed,
  and encounters paced around traversal recovery windows
- Three mixed traversal challenges: animated Gravity Blooms with swept
  mechanical petals, three-stage Vector Lanes with alternating cyan channels
  bracketed by industrial pylons, and two-stage Shear Runs that demand rapid
  over/under transitions
- Vector Crown environmental arches with tapered polygonal ribs, floating
  keystones, biome-colored neon traces, and shape-matched collision
- Distance scoring, timed chains, and multipliers up to 8x
- Eight original continuous stereo synthwave/synthpop songs sequenced through
  a no-repeat shuffle bag, with a title-screen Sound Test, individual 36-bar
  forms at 116–148 BPM, and roughly 61–77 seconds of playable music and effects
  per track; vocal-formant, clipped-pulse, breath-flute, motorik, FM-mallet,
  hard-sync brass, chorus-guitar, and angular-siren identities sit over distinct
  harmony, bass, drum, delay, reverb, fill, and dynamic treatments
- Optional Jump Pack feedback for impacts, Nova Pulses, upgrades, and guardian
  destruction, with safe play when no vibration accessory is present
- Hardware fog, a layered gradient sky, stars, striped sun, a subtle animated
  neon route grid, cyan-magenta HUD framing, additive sprites, engine ribbons,
  explosions, speed streaks, impact feedback, and vector HUD
- Title, pause, game-over, retry, controller hot-plug handling on hardware, and
  automatic gamepad-or-keyboard selection in the macOS Flycast launcher
- A randomly selected biome backdrop every time the title screen is entered

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
**Sound Test**, or **Exit**, then confirm with Start or A (Return or X). Sound
Test uses Left/Right to audition all eight songs immediately from their first
intro, shows the live song part and two-bar form position, uses A (X) to restart
the selected song, and B (C) to return without accidentally beginning a run.
While paused, B (C) aborts the run and returns to the title. After a defeat,
Start or A (Return or X) retries and B (C) returns to the title.

A controller is required on real Dreamcast hardware; Gravity Wave waits safely
when one is missing and supports hot-plugging. On macOS, the launcher assigns
only a detected gamepad to Flycast's Dreamcast port A. If no gamepad is present
when Flycast starts, it assigns only the keyboard instead. Custom Flycast key
mappings still take precedence over the defaults shown above.

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

The title screen identifies the active track and includes a Sound Test for
auditioning the full album. Automatic playback uses a shuffled eight-song bag:
every song starts once before any can repeat, and the first song after a
reshuffle can never match the previous one. The bag advances when a new full
track is started, so replaced transition requests do not consume selections.
Leaving Sound Test treats the auditioned song as the first heard entry of a
fresh pass, then schedules the other seven. Each song owns its complete 36-bar
form, effects tail, fade, and short silence before the next handoff; biome and
mode requests can change the title/game gain but never cut a verse or chorus
into another song. Left/Right and A in Sound Test are deliberate immediate
seeks. Normal loops and handoffs stop and restart the stream after the authored
silence so the AICA ADPCM predictor begins every song from a known state.

- **Midnight Vector** — dotted headlight calls, rising fifths, a vocal-formant
  lead, poly-brass bed, rubber bass, and the album's prominent glass arpeggio
- **Magenta Circuit** — clipped offbeat pulse hook, FM bass, phase-distortion
  comp, and a compact electro form with a real drop and middle eight
- **Glass Horizon** — broad 12/8 breath-flute phrases over electric piano,
  fretless bass, restrained drums, and sparse FM-bell answers
- **Static Heart** — a motorik pulse ostinato, picked bass, and ring-mod replies
  in place of the album's usual headline lead
- **Afterimage Run** — swung half-time toms, sliding sub bass, FM mallet, muted
  brass, and a darker harmonic-minor/Phrygian palette
- **Neon Afterburn** — 3+3+2 hard-sync brass calls, distorted picked bass, rock
  drums, and an arena-sized final answer without a decorative arpeggio
- **Chrome Devotion** — chorus-guitar lead, DX-style keys and vowel pad,
  fingered bass, borrowed-chord color, and long major-key suspensions
- **Redline Prophecy** — angular ring-mod siren, wavetable bass, broken metallic
  drums, and an Aeolian/Phrygian drive that opens into a Lydian bridge

`tools/render_soundtrack.py` is the production score source. It deterministically
renders each song as one continuous 21.5 kHz stereo PCM timeline: song-specific
sections, harmony and voice-leading; independent verse, chorus, developed verse,
bridge/solo, final, and outro phrases; track-specific synthesis and drum roles;
and complete-bus delay, reverb, risers, fills, mastering, and energy automation.
Releases and effects therefore cross formal boundaries naturally instead of
being restarted from a small bank of two-bar clips. Every master contains 36
authored bars, a 2.6-second effects/fade tail, and an aligned silent guard for
stream read-ahead. The current album runs from about 61 to 77 playable seconds
per song, including that tail.

`tools/analyze_soundtrack.py` checks the rendered manifest and WAVs for signal
integrity, clipping, DC and high-frequency seam transients, silence gaps,
loudness jumps, form development, repeated melody/harmony signatures, false
variety, and track-level identity. `tools/build_music_album.py` then requires all
eight titles in canonical order, verifies 36-bar timing and exact stereo PCM16
frame metadata, confirms at least 65,408 frames of digital-zero guard, and
encodes every complete WAV with one uninterrupted stereo Yamaha/AICA ADPCM
history. Its versioned `GWAM` container stores per-track byte offsets, encoded
lengths and playable lengths plus catalog and payload fingerprints.

The checked album is approximately 11.84 MiB and is linked into the ELF, so
playback needs no runtime filesystem or disc reads. It is streamed from the
embedded image into a 65,408-byte stereo AICA ring (two 32,704-byte channel
buffers) rather than loading the entire album into 2 MiB sound RAM; KOS also
uses a 32,704-byte SH-4-side split buffer. Effects occupy their own small AICA
allocations. At boot, Gravity Wave verifies the container version, track table,
runtime catalog fingerprint, payload checksum, alignment, playable lengths,
and read-ahead guards before enabling music; the current release reports about
1.7 MiB of AICA RAM still free after the stream and effects are loaded. A failed
check disables only the score and leaves the game responsive. The handoff clock
follows the greater of simulation time and unclamped KOS elapsed time, so a slow
emulator frame cannot let real-time AICA playback outrun the next-song
transition.

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

## Rebuild the soundtrack asset

The checked-in ADPCM album makes normal builds fast and independent of the host
audio toolchain. After changing `tools/render_soundtrack.py`, the track catalog,
form timing, album format, or streaming contract, regenerate it with:

```sh
make music
```

This target performs the complete release pipeline:

1. `render_soundtrack.py` renders all eight production PCM masters and a rich
   `soundtrack_manifest.json` in a temporary directory using NumPy and SciPy.
2. `analyze_soundtrack.py` runs the signal, form, melody, harmony, transition,
   and identity audit; hard QA errors stop the bake.
3. A strict catalog-only Dreamcast ELF boots briefly in Flycast interpreter
   mode (`Dynarec.Enabled=no`) and emits the exact runtime catalog fingerprint.
4. `build_music_album.py` validates the manifest and WAV contract, continuously
   encodes each stereo song to interleaved AICA ADPCM, builds the versioned
   album table and checksums, then atomically installs the generated asset.
5. A final `-Wextra -Werror` production build confirms that the generated album
   links cleanly; the normal boot-time checks then enforce catalog agreement.

`uv`, Python 3, KallistiOS, and Flycast are required for this one-time bake.
Temporary WAVs and reports are removed automatically; expect production
rendering and analysis to take several minutes. The host renderer and runtime
catalog deliberately mirror the eight-song order, tempos, and two-bar display
cells. When changing tempo or form, keep `render_soundtrack.py`,
`soundtrack_defs`, and `music_song_forms` synchronized, then run `make music`.
For an incompatible `GWAM` change, bump `MUSIC_ALBUM_VERSION` and the packager's
matching `ALBUM_VERSION`. Increment `MUSIC_SYNTH_REVISION` when the runtime
catalog interpretation changes without otherwise changing its hashed fields.

To render and inspect listening masters without replacing the checked asset:

```sh
uv run --with 'numpy==2.5.2' --with 'scipy==1.18.1' \
  tools/render_soundtrack.py --output-dir /tmp/gravity-wave-soundtrack

uv run --with 'numpy==2.5.2' --with 'scipy==1.18.1' \
  tools/analyze_soundtrack.py \
  /tmp/gravity-wave-soundtrack/soundtrack_manifest.json \
  --json-output /tmp/gravity-wave-soundtrack/qa.json
```

The renderer also accepts `--track`, by album number, title, or slug, and a
lower-cost `--quick` mode for iteration. `make music` always requests the full
production album; quick renders are intended only for listening and QA. Pass
`--strict` to the analyzer for an optional review that promotes all warnings to
failures; the release bake uses the normal policy, where hard errors fail and
warnings remain visible for judgment.

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

On macOS, a connected HID gamepad or joystick is selected exclusively. With no
pad connected, Flycast's default keyboard controls are selected exclusively.
The choice is printed in the launching terminal. Connect or disconnect the pad
before starting Flycast, or force a profile while troubleshooting:

```sh
./run-flycast.sh --input gamepad
./run-flycast.sh --input keyboard
```

`GRAVITY_WAVE_INPUT=auto|gamepad|keyboard` provides the same override for
scripts. Input routing is passed through Flycast's transient `-config` option;
it does not rewrite the user's saved emulator settings.

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

Each biome is staged as a complete arcade chapter: an establishing formation,
a first traversal lesson, a named landmark and development fight, a harder
second traversal, a biome-specific guardian, then a protected reward/recovery
window. Props repeat in recognizable districts and traversal hardware inherits
the materials and colors of its biome. Enemies, pickups, traversal objectives,
and structures are expressed in route-relative coordinates, so every encounter
follows the same authored flight line. Fired weapons capture their launch ray
in world space, however, so shots remain straight as the course turns or climbs.
Destroying enemies before the chain timer expires raises the multiplier. A
Gravity Bloom's broken inner halo marks its exact scoring aperture; Vector
Lanes' paired cyan rails mark the exact lane center and dim red fields mark
the space outside it, while Shear Run arrows call for an alternating climb or
dive. Clean, boosted lines earn larger awards. Nova Pulses erase enemy fire and
ordinary craft, but only damage a guardian, so saving charges for boss patterns
is usually worthwhile. Weapon upgrades last for the current run; collecting a
max-level Laser Core converts it into score.

## Verified with

- KallistiOS 2.3.0
- SH-4 GCC 15.2.0
- Flycast 2.7 with serial console enabled
- macOS no-gamepad auto-detection, keyboard-only port-A fallback, forced
  gamepad-profile boot, and unchanged saved Flycast configuration values
- Direct ELF boot, title screen, live gameplay, enemy/gate/pickup encounters,
  guardian defeat, all four biome render paths, transitions, embedded AICA audio,
  structure/terrain impacts, pause, retry, and game-over flow
- Version-1 `GWAM` album validation across eight aligned continuous streams,
  matching runtime-catalog and payload fingerprints, rendered-tail metadata,
  playable/guard frame bounds, a 65,408-byte stereo AICA ring, clean
  predictor-reset handoffs, and frame-by-frame polling; deterministic tests
  cover four complete no-repeat shuffle bags, full-form validation, deferred
  outro-only transition commits, Sound Test selection/restart/return and
  outro-to-intro looping, plus a complete eight-track album cycle
- Authored-course diagnostics across all four biomes (route continuity,
  lateral and vertical span, peak turn/grade, corridor relief, traversal
  clearance and Prism Crucible collision), world-space combat geometry,
  steep-grade rigid-model spans, and all four guardian profiles
- 59.9–60.0 fps in dense guardian and neon-grid scenes at 640x480, with roughly
  9–12.5 ms PVR registration time and 288 KiB of resident texture data

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution. The source texture atlases were created for this project
with OpenAI ImageGen and are documented in `assets/PROMPTS.md`.
