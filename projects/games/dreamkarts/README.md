# DreamKarts

A kart racer for the Sega Dreamcast with early-2000s Dreamcast looks and modern
Mario Kart-style handling. Hop into a drift, hold the powerslide round the
corner by counter-steering, watch the sparks at the rear tyres turn blue, orange
and then purple, and let go for a mini-turbo.

The first course is **Emerald Beach**: an oval with a wooden boardwalk straight
over a turquoise sea, a natural rock arch, a lighthouse, palm trees, beach huts,
a grandstand and Sonic-style dash panels. You drive a Chao in a standard kart
against five CPU Chao.

## Features

- **Drift handling inspired by Mario Kart World:**
  - Press the drift button to hop. If you land steering, you start a powerslide; a short grace window lets you pick the direction just after landing.
  - While sliding, steering into the turn tightens the slide and counter-steering widens it, down to a gentle arc. You can hold a slide through a whole corner.
  - The mini-turbo charges faster when you steer in tight and stops charging on sand. Stage 1 (blue) gives a 0.45 s boost, stage 2 (orange) 0.95 s and stage 3 (purple) 1.55 s.
  - Sparks spray from both rear tyres, with a glow, and a gauge at the bottom of the screen shows the charge.
  - Hold the drift button without steering to charge a jump (Mario Kart World's charge jump), then release to spring into the air.
  - A rocket start: press accelerate as the "2" fades and hold it through GO. Press too early and you burn out.
- **Grand Prix:** a 3-lap race against five CPU Chao. They drive a racing line, drift and use mini-turbos, and have mild rubber-banding.
- **Time Trial:** a solo 3-lap run with lap splits and your best lap.
- **50cc, 100cc and 150cc** engine classes.
- **Dash panels** (one on each straight and one on the exit of turn 1) give a boost, and any boost lets you cut through sand.
- **Presentation:**
  - Title screen with a rotating Sega-style sunburst, a bouncing logo whose letters drop in one by one, sparkles, and the grid in the background.
  - A slide-in race setup menu.
  - A fly-in intro, then a 3-2-1-GO countdown.
  - "FINAL LAP!" with the music sped up 6%, a FINISH! banner with confetti, and a results screen.
- **Audio:** an original synthesized tropical track, "Coral Speedway" (steel drums, marimba, slap bass, electric piano and brass; 128 BPM, 75 s seamless loop), plus 26 effects. The player's engine is two pitch-shifted loops crossfaded by speed, with tyre squeal, spark crackle and sand rumble loops alongside.
- **Graphics:** a custom PowerVR renderer using [SH4ZAM](https://sh4zam.com/). Matrix transforms use FTRV, perspective divides use FSRRA, polygons go straight to the store queues, and triangles crossing the near plane are clipped. It also has table fog, chunked frustum culling, distance LODs for palms and karts, an animated sea and shore foam, a sky dome, clouds and a lens flare.

## Controls

| Dreamcast controller | Flycast keyboard default | Action |
| --- | --- | --- |
| Analog stick or D-pad | I/J/K/L or arrow keys | Steer (menus: move) |
| A | X | Accelerate (menus: OK) |
| B | C | Brake / reverse (menus: back) |
| R trigger (or L trigger, or X) | V (or F, or S) | Hop and drift; hold without steering to charge a jump |
| Start | Return | Pause (continue / restart / quit) |

To drift: hold accelerate, press and hold R as you turn in, and keep holding it
through the corner. Push the stick toward the inside to tighten the slide and
away to widen it. Release R when the sparks are orange or purple.

## Build and run

From the repository root:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make -C projects/games/dreamkarts
file projects/games/dreamkarts/dreamkarts.elf
sh-elf-readelf -h projects/games/dreamkarts/dreamkarts.elf
```

Every build clones or fast-forwards an ignored `third_party/sh4zam` checkout
of upstream SH4ZAM `master` (see the repository README); it is never pinned.

Run it in Flycast with the serial console on your terminal:

```sh
projects/games/dreamkarts/run-flycast.sh
```

The launcher builds first unless you pass `--skip-build`. It uses only
transient `-config` flags. No networking or VMU saving is used.

## Tests and captures

- `make autotest` builds `dreamkarts-autotest.elf`: a CPU drives your kart through a full 150cc Grand Prix. It logs frame rate and per-phase CPU time every 2 seconds, prints a summary (lap times, drifts, mini-turbos by stage, walls hit, results), then exits on `DK AUTOTEST: PASS`. Add `EXTRA_CFLAGS=-DDK_AT_TT` for Time Trial or `-DDK_AI_TRACE` to log the CPU's drift decisions.
- `make VARIANT=capture dreamkarts-capture.elf` then `uv run tools/capture.py --out captures` plays scripted button presses through the title, menu, intro, countdown, a drift, a boost, the boardwalk and the results. It screenshots the Flycast window at each step without taking over the screen.

Last measured in Flycast 2.7 at 150cc: a steady 59.7 to 59.8 fps for a whole
race. Average CPU time is about 8 ms a frame with six karts, and about 5 ms in
Time Trial. Flycast does not model SH-4 timing exactly, and this has not yet
been measured on real hardware.

## Assets

Everything is generated from code, and the generated files are committed so a
plain build needs no Python:

| Command | Script | Output |
| --- | --- | --- |
| `make textures` | `tools/build_textures.py` | Procedural textures, the HUD font, the logo and the countdown lettering, packed as twiddled, mipmapped PVR textures (`tools/pvr_texture.py`) |
| `make models` | `tools/build_models.py` | The kart and Chao as rigid parts with LODs, rival paint slots and outward winding checks. Preview them with `tools/preview_models_blender.py` (Blender) |
| `make audio` | `tools/build_audio.py` | Effects and music synthesized with numpy and encoded to AICA ADPCM with KOS `wav2adpcm` |

The HUD and logo lettering uses the DejaVu Sans Bold Oblique font bundled with
matplotlib (Bitstream Vera licence). Previews go to `assets/generated/previews/`,
which Git ignores.

## Layout

| File | Contents |
| --- | --- |
| `src/kart.c` | Handling, drifting, boosts, laps and CPU drivers |
| `src/track.c` | Oval centreline, surfaces, walls and dash panels |
| `src/world.c` | Scenery building and the animated sky and sea |
| `src/render.c` | PVR lists, SH4ZAM transforms, clipping and submission |
| `src/models.c` | Lit, animated kart and Chao rendering |
| `src/fx.c` | Sparks, flames, smoke and confetti |
| `src/hud.c`, `src/title.c` | Overlays and menus |
| `src/audio.c` | Music stream and effect voices |
| `src/main.c` | Race flow and cameras |
