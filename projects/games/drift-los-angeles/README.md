# Drift Los Angeles

Drift Los Angeles is an original open-city street-drifting game built for Sega
Dreamcast with KallistiOS. Drive an original American grand tourer through an
unbounded, deterministic Los Angeles-inspired street grid at blue hour, link
drifts between wide boulevards, and bank increasingly valuable score chains.

The car is an original model influenced by the proportions and angular design
language of the Corvette C7 era: a long hood, fastback glass, rear haunches,
sharp lamps, splitter, diffuser, ducktail, and four separately modeled wheels.
It contains no manufacturer badge or copied production geometry.

## Highlights

- An endless, streamed open city with no world edge or loading screens
- Four distinct neighborhoods, each with its own landmark, lighting, and street
  furniture: Downtown Core, Pacific Coast, Arts Quarter, and Neon Strip
- A detailed C7-inspired coupe with working lights, steering, and wheels
- Drift physics with throttle oversteer, clutch kicks, handbrake initiation,
  burnouts, and power donuts
- Thirty-six traffic cars that obey lanes and signals, plus pedestrians and
  dense street life
- Blue-hour lighting with streetlamps, neon, headlights, and brake lamps
- Drift scoring with a six-times chain multiplier, hold timer, and duration
  bonuses
- Long skid marks, layered tire smoke, and high-RPM exhaust flames
- A recorded V8 engine and tire squeal, plus a full racing soundtrack by
  MintoDog
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

Source the installed KallistiOS environment and run `make`:

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

## Rebuild the audio

The generated engine, tire, and music banks are checked in, so the normal
build does not need a host audio tool. To rebuild them:

```sh
make audio
```

`make music` rebuilds only the soundtrack (it uses macOS `afconvert`). Source
links and licenses are recorded in `assets/source/audio/README.md`.

## Regenerate the car mesh

The car is authored procedurally in Blender, and the generated `model_data.h`
is checked in so Blender is not needed for a normal build. To regenerate it:

```sh
make model
```

## QA

`make qa` checks the car mesh and texture budget, `make qa-run` runs a
60-second district tour with telemetry, and `make physics-qa-run` runs the
burnout and donut regression suite in Flycast.

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
Music is [“Pure Raceway” by
MintoDog](https://opengameart.org/content/pure-raceway), released under CC0.
Conversion and loop-processing details are in `assets/source/audio/README.md`.
