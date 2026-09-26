# Drift Los Angeles

Drift Los Angeles is an original open-city street-drifting game built for Sega
Dreamcast with KallistiOS. Drive an original American grand tourer through an
unbounded, deterministic Los Angeles-inspired street grid at blue hour, link
drifts between wide boulevards, and bank increasingly valuable score chains.

The car is an original model influenced by the proportions and angular design
language of the Corvette C7 era: a long hood, fastback glass, rear haunches,
sharp lamps, splitter, diffuser, ducktail, and four separately modeled wheels.
It contains no manufacturer badge or copied production geometry.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/drift-los-angeles.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/drift-los-angeles.elf)
is also available.

## Highlights

- An endless, streamed open city with no world edge or loading screens
- Four distinct neighborhoods, each with its own landmark, lighting, and street
  furniture: Downtown Core, Pacific Coast, Arts Quarter, and Neon Strip
- Every block is a ring of two to four street-wall properties per side, each
  with its own height, facade, tint, setback, cornice and rooftop (plant, water
  tank, billboard or mast), with the block's tower or warehouse rising behind
- Block geometry is generated once into a static cache with blue-hour lighting
  baked into the vertices (warm street level, cooler and darker upper floors,
  shaded undersides), then replayed each frame with one transform per vertex
- Textured street furniture from painted rigs: benches, litter bins, hydrants,
  parking meters, newspaper boxes, bollards, bus shelters with ad panels,
  magazine kiosks, mailboxes, dumpsters, cafe tables and chairs, planters and
  street signs, tinted per district where they are painted metal
- A detailed C7-inspired coupe with hard-edge normals, baked ambient occlusion,
  view-dependent paint/glass reflections, working lights, steering, and wheels,
  and a modelled cabin (dash, seats, wheel) visible through translucent glass
- Drift physics with throttle oversteer, clutch kicks, handbrake initiation,
  burnouts, and power donuts
- Thirty-six traffic cars that obey lanes and signals, lofted as five real body
  types (sedan, hatchback, SUV, van, pickup, plus taxis) with spinning spoked
  wheels, lamps, grilles and plates at three detail levels; parked cars share
  the same models
- Articulated pedestrians: sixteen-part textured bodies with faces, hair,
  caps, glasses, four outfit styles, denim and shoes, each tinted from its
  seed and walking a knee-and-elbow cycle, with two mesh detail levels and a
  billboard fallback in the distance
- Solid street collisions: lampposts and parked cars take a rigid-body
  impulse that bounces and spins the car, while pedestrians you run down are
  thrown tumbling into the air and lie on the pavement until you leave the block
- Modular storefronts, upper floors and cornices with mipmapped architecture
- Blue-hour lighting with soft headlights, oriented tire/contact shadows,
  curved streetlamps and neon
- Detailed palm fronds, dimensional tree crowns, Art Deco roof tiers and
  sawtooth warehouse roofs
- Drift scoring with a six-times chain multiplier, hold timer, and duration
  bonuses
- Long skid marks, layered tire smoke, and high-RPM exhaust flames
- A recorded V8 engine and tire squeal, plus an original three-track outrun
  soundtrack in the style of 1980s synth records
- Compact arcade HUD with a traffic-aware minimap
- Title screen, automated demo tour, pause, and controller hot-plugging

## Screenshots

![Drift Los Angeles title screen](assets/screenshots/title-screen-v2.jpg)

| Downtown Core | Pacific Coast |
| --- | --- |
| ![Downtown Core](assets/screenshots/downtown-core-v5.jpg) | ![Pacific Coast](assets/screenshots/pacific-coast-v5.jpg) |

| Arts Quarter | Neon Strip |
| --- | --- |
| ![Arts Quarter](assets/screenshots/arts-quarter-v5.jpg) | ![Neon Strip](assets/screenshots/neon-strip-v5.jpg) |

| Layered drift smoke | High-RPM exhaust burst |
| --- | --- |
| ![Layered sustained-drift smoke](assets/screenshots/drift-smoke-v6.jpg) | ![Four-pipe exhaust flame burst](assets/screenshots/exhaust-flames-v6.jpg) |

## Controls

| Control | Action |
| --- | --- |
| Analog stick or D-pad left/right | Steer |
| R trigger or D-pad up | Accelerate |
| L trigger or D-pad down | Brake / reverse |
| Hold R + L together | Brake-standing rear-wheel burnout |
| Full steering lock + R at low speed | Power donut; modulate R to balance its radius |
| Hold A | Handbrake; rapidly locks the rear tires and pivots the car |
| Tap X with throttle | Clutch kick; dumps a short torque/rev pulse into the rear tires |
| B | Toggle close/wide chase camera |
| Y | Reset to the starting boulevard |
| Start | Pause / resume |

On the title screen, Start or A begins, X launches the automated four-district
demo tour, and B exits. In the demo, Start or A takes control and B returns to
the title. From the pause screen, B returns to the title.

## Build

From this project directory, source the installed KallistiOS environment and
run `make`. The build fetches the latest upstream SH4ZAM `master` first:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

This creates `drift-los-angeles.elf`.

## Run in Flycast

```sh
./run-flycast.sh
```

The launcher builds by default. On macOS it uses a connected gamepad, or the
keyboard if none is present, without changing saved Flycast preferences.

Override automatic selection when testing with:

```sh
./run-flycast.sh --input gamepad
./run-flycast.sh --input keyboard
```

Launch an existing build with:

```sh
./run-flycast.sh --skip-build
```

Set `KOS_ENV` or `FLYCAST_BIN` to override either installed dependency.

For a hands-off visual tour that changes district every fifteen seconds:

```sh
make showcase
```

The same tour is available from the normal title screen by pressing X.

## Rebuild the textures

The generated C assets are checked in, so the normal Dreamcast build has no
host-side image dependency. After changing a source atlas or the title art,
regenerate the PVR data and local previews with:

```sh
make textures
```

This needs `uv`. The ImageGen prompts are preserved in `assets/PROMPTS.md`.
The target also rebuilds the reproducible architecture crops and procedural
effect sprites. Run `make architecture` to extract separate upper-wall,
storefront and cornice modules from the source paintings, or `make effects`
to regenerate smoke, contact-shadow and palm alpha sprites independently.
The final converter emits aligned, twiddled 16-bit textures, including
mip levels for selected world materials and ARGB4444 alpha for effect sprites.

## Rebuild the audio

The generated engine, tire, and music banks are checked in, so the normal
build does not need a host audio tool. To rebuild them:

```sh
make audio
```

`make music` rebuilds only the soundtrack: `tools/compose_soundtrack.py`
renders the three songs from their notation, then `tools/build_music_asset.py`
packs them into a 4-bit ADPCM playlist bank (both run through `uv`). Rendered
WAV previews land in `assets/generated/previews/soundtrack/`. Source links and
licenses are recorded in `assets/source/audio/README.md`.

## Regenerate the car mesh

The car is authored procedurally in Blender, and the generated `model_data.h`
is checked in so Blender is not needed for a normal build. To regenerate it:

```sh
make model
```

## Regenerate the vehicles

Traffic and parked cars are authored procedurally in `tools/build_vehicles.py`
from cross-section stations, sharing the rig format and tintable-atlas
approach of the pedestrians (`tools/rig_export.py`). The generated
`assets/source/vehicle-atlas.png` and `vehicle_data.h` are checked in:

```sh
make vehicles
make textures
```

## Regenerate the street furniture

`tools/build_props.py` paints the prop atlas and builds the furniture rigs;
`assets/source/prop-atlas.png` and `prop_data.h` are checked in:

```sh
make props
make textures
```

## Regenerate the pedestrians

The pedestrian atlas and the two-level articulated mesh are authored
procedurally in `tools/build_pedestrians.py`; the generated
`assets/source/pedestrian-atlas.png` and `pedestrian_data.h` are checked in.
To regenerate them and render a Blender contact sheet of posed variants into
`assets/generated/previews/pedestrians-*.png`:

```sh
make pedestrians
make textures
```

The atlas is tintable: every region is painted in light neutral tones and the
game multiplies each body part by a per-figure colour, so skin tones, shirts,
trousers, hair and shoes all come from the pedestrian's seed. Parts swap atlas
cells for short sleeves, jacket sleeves or bare shins.

## QA

`make qa` checks the car mesh and texture inventory, `make qa-run` runs a
60-second district tour with telemetry, and `make physics-qa-run` runs the
burnout and donut regression suite in Flycast. `make collision-qa-run` drives
the car into a lamppost, a parked car and a walking pedestrian in turn and
checks that each impact registers, that the car never passes through the
obstacle, and that the pedestrian is launched and comes to rest.

`make visibility-qa-build` builds `render-visibility-qa.elf`. Boot it in Flycast
or on a Dreamcast and check for `Render visibility QA: PASS`. It reproduces the
turning-camera building disappearance, compares box culling against an
independent corner reference, and checks road selection across signed grid
boundaries.

`make qa-benchmark` records the full district tour, exits Flycast when the
aggregate telemetry arrives, and checks a **30 FPS average** target. The log
and JSON report are saved to `assets/generated/previews/visual-qa/benchmark.log`
and `benchmark.log.json`; override the log location with `QA_LOG=/path/to/run.log`.
The average uses rendered frame count divided by real elapsed time, including
frame waits and audio work. Periodic PVR FPS and registration-time samples are
reported separately. Emulator results do not establish real-hardware performance.

An existing log can be checked with:

```sh
uv run tools/analyze_render_log.py /path/to/run.log
```

The parser requires aggregate telemetry and at least 30 measured seconds;
older logs containing only FPS samples cannot establish a whole-run average.
`make qa-tools` runs regression checks for the log parser, mesh audit and
PowerVR texture/mipmap packing.

Texture bytes are an inventory rather than a fixed 4 MiB allowance: the
Dreamcast's 8 MiB VRAM also holds framebuffers, vertex buffers and tile bins.
The benchmark reports successful resident allocations and remaining VRAM.

The September 2026 graphics upgrade completed the 60-second four-district
Flycast tour at **32.00 FPS average** (1,922 frames / 60.062 seconds), compared
with 27.20 FPS before the upgrade under the same host configuration. This is
an average target, not a locked minimum: individual sampled intervals can
fall below 30 FPS. Textures and HUD occupy 3,965,248 bytes (3.78 MiB), with
1,144,648 bytes (1.09 MiB) free after PVR allocations and a 768 KiB vertex
buffer. The benchmark kept game audio synthesis enabled and used Flycast's
null host-audio backend. Real Dreamcast hardware remains to be measured.

The articulated pedestrians add a 256x256 mipmapped atlas (170 KiB) and up to
about 480 triangles per close figure. With them, the same tour averaged
**46.54 FPS** (2,794 frames / 60.034 seconds) with a peak of 7,014 triangles
per frame.

The world upgrade that followed (lofted traffic vehicles with a second
256x256 atlas, per-lot street walls on every block, and the player car's cabin
behind translucent glass) averaged **37.13 FPS** (2,230 frames / 60.052
seconds) with a peak of 7,312 triangles per frame.

The static geometry cache, textured street furniture and baked vertex
lighting then took the tour to **35.63 FPS** (2,214 frames / 62.139 seconds,
peak 7,984 triangles) with considerably more on screen: every block's walls,
roofs, props, palms and road kit are generated once into a 3x3 sub-cell cache
(nine full-detail blocks around the car, forty volume-only blocks beyond) and
replayed with one transform per vertex, culled per sub-cell against the
frustum, per quad against its outward normal, and by a detail level that
mirrors the old distance gates. The player car uses one FTRV per vertex with
lighting in car-local space and refreshes its shading only when the heading
changes; its wheels are rigs rather than hundreds of immediate primitives.
Per-frame phase timing is printed by the benchmark build (`benchmark phases`
and `benchmark city` lines). Flycast's JIT does not reward FTRV over scalar
math, so real-hardware figures should be better than these. Textures and HUD
occupy 4,489,600 bytes (4.28 MiB) with 620,200 bytes free after allocation,
and the ELF is 14.4 MB including the 3.6 MB cache.

Vertex-count telemetry gives a lower bound on stream bytes; polygon headers
also consume the PVR vertex buffer. The mesh audit checks the renderer's
4,096-vertex and 4,096-face limits after UV/normal/material splits, welded
body proportions, noncollapsed texture UVs, atlas bounds, AO values and
material face ranges. Each material has separate vertices for cached shading.

## SH4 rendering acceleration

The renderer uses [SH4ZAM](https://sh4zam.com/) headers from the ignored,
unpinned clone at `third_party/sh4zam`, updated to the latest upstream `master`
before each build, for
batched camera transforms, positive-depth reciprocals, reflection normalization
and paired sine/cosine. No extra SDK installation is required; see the
[integration notes](third_party/SH4ZAM.md). The car's draw
cache keeps each vertex's frequently used fields together on SH4 cache lines.
Vertices now go straight to PowerVR through the store queues, and the car shades
only vertices referenced by front-facing faces. Wheel transforms reuse their
rotation pairs, and fixed scene colors are folded at their call sites. These
changes and the visibility corrections give a **47.97 FPS** four-district tour
in Flycast with the upgraded artwork and unchanged VRAM allocations. See [PERFORMANCE.md](PERFORMANCE.md)
for the measurements, limits and reproducible checks.

Build the standalone numerical checks with `make math-qa-build`, then boot
`render-math-qa.elf` in Flycast or on a Dreamcast. It prints maximum errors and
`Render math QA: PASS` to the serial console. Run `make physics-qa-run` and
`make qa-benchmark` for the driving regression and full graphics tour.

`make geometry-qa-run` runs the tour with a fixed 30 Hz simulation step and
prints whole-tour triangle/vertex totals. Compare two captured logs with
`uv run tools/compare_geometry_logs.py before.log after.log`. This checks
geometry counts independently of renderer throughput; it does not measure FPS
or compare pixel colors. For a submission-path comparison, clean and add
`-DDRIFT_LA_STAGED_SUBMISSION` to the geometry QA CFLAGS. Rebuild without QA
defines to return to normal play.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent Sega
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.

Recorded engine audio uses [“Eight-cylinder engine idling.wav” by
Lumamorph](https://freesound.org/people/Lumamorph/sounds/636066/) under CC BY
4.0 and [“Hot-Rod-V-8-BigSpchg-RoughIdle-RevUps-IdleDR025-30sec.wav” by
Ears68](https://freesound.org/people/Ears68/sounds/144454/) under CC BY 3.0.
The tire recording is [“Distant car tire screetch” by
Sadiquecat](https://freesound.org/people/Sadiquecat/sounds/737192/) under CC0.
The soundtrack (“Blue Hour Boulevard”, “Pacific Coast Highway” and “Neon
Strip”) is original to this project, composed and synthesized in
`tools/compose_soundtrack.py`.
Conversion and loop-processing details are in `assets/source/audio/README.md`.
