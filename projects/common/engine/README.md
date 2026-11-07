# Engine

Shared game code for KallistiOS games in this repo. DreamKarts and Ridgeline
build on it, so a fix or speed-up here reaches both.

| Module | What | Used by |
| --- | --- | --- |
| `vecmath.h` | Vectors, scalar helpers, packed colours, the camera and its look-at, lean and orbit helpers (all inline) | DreamKarts, Ridgeline |
| `rand.h`, `rand.c` | The seeded generator; a game can hold it in its own file (`ENG_RAND_IMPL`) so calls inline | DreamKarts, Ridgeline |
| `input.h`, `input.c` | Two pads with edge detection and menu repeat, triggers as buttons, the keyboard | DreamKarts, Ridgeline |
| `render.h`, `render.c`, `xform.S` | PowerVR lists, header cache, texture pool, near-plane clipping, strips and rigid meshes sent through the store queues, SH-4 assembly vertex paths | DreamKarts, Ridgeline |
| `prof.h`, `prof.c`, `prof_engine.h` | Frame phase timers for the autotests' log lines; per-section cycle and stall counters for console benchmarks; the renderer's own ids | DreamKarts, Ridgeline |
| `frame.h`, `frame.c` | Frame pacing: the step follows the measured vblank period, missed vblanks run extra steps, FPS and CPU counters | DreamKarts, Ridgeline |
| `mesh.h`, `mesh.c` | Builds rigid models (vehicles) into `r_mesh_kv` strips with per-group lighting | DreamKarts, Ridgeline |
| `scenery.h`, `scenery.c` | Static scenery pool: strips and chunks with bounding spheres and distance LOD | Ridgeline |
| `sfx.h`, `sfx.c` | Effect bank in AICA RAM, one-shots, looping voices on reserved channels, pan and distance of a sound for the listener | DreamKarts, Ridgeline |
| `text.h`, `text.c` | Bitmap text from a font atlas | DreamKarts, Ridgeline |
| `sky.h`, `sky.c` | Gradient sky dome about the camera, lens flare ghosts | DreamKarts, Ridgeline |
| `bird.h` | A low-poly flapping bird for scenery life, held in the drawing file (`ENG_BIRD_IMPL`) so it inlines as before | DreamKarts, Ridgeline |
| `qa.h`, `qa.c` | Test builds: the shot line for `tools/capture.py`, a log window's frame statistics | DreamKarts, Ridgeline |
| `ui.h`, `ui.c` | Menu chrome: panels, heading plaques, selection pills, controller button glyphs, hints, keys, dots; colours from the game's theme | DreamKarts, Ridgeline |

`game.mk` holds the build: variants, object directories, the link stamp,
the version header and the engine's rules (each game's Makefile sets a few
variables and includes it).

Tools (`tools/`): `pvr_texture.py` (twiddled RGB565/ARGB4444 with mips),
`fontatlas.py` and `fonts/`, `meshkit.py` (procedural low-poly meshes) with
`meshasm.py` (welding, checks and the model header emit) and
`blender_preview.py` (the model previews' Blender scene, ground and camera),
`stripify.py`, `blender_props.py` (Blender prop baker), `synthkit.py` (the
sound generators' filters, fades, reverb and PCM finishing), `assetpak.py`
(the sfx and texture paks and their generated headers), `capture.py`
(screenshots at each `<PREFIX> SHOT` line of a capture build; the games'
`tools/capture.py` call it with their names), `flycast-run.sh` (the core
of the games' `run-flycast.sh`: build, boot in Flycast, stop on a pass or
fail line) and `microbench/` (cycles per vertex for `xform.S` on the
console).

## Using it

Add `-I../../common` and include `engine/render.h` and friends. Compile the
engine's `.c` files with your own flags (see either game's Makefile). Each
game supplies three headers on its include path:

| Header | Holds |
| --- | --- |
| `engine_config.h` | `ENG_NAME`, `ENG_DT`, the vertex buffer size, test switches (`ENG_PROF`, `ENG_INPUT_TAPS`, ...). A switch may expand to a game symbol (DreamKarts' `ENG_TA_SINK` reads its probe mask), so the engine then only links with that game's objects |
| `engine_assets.h` | Read by `render.c` only: the texture table and pak, mip bias, optional texture sets, PVR settings (bins, fog, clear colour) |
| `prof_ids.h` | Profile section and count ids, with the renderer's from `prof_engine.h` placed among them |

## Rule: no cost to existing games

Moving code here must not slow a game down on the console. Two ways to
show it, cheapest first:

1. Byte-identical: build the game's release, autotest, hwbench and capture
   variants before and after with a fixed SHA, `sh-elf-objcopy -O binary`
   each ELF and compare. Only `__TIME__` may differ, and then nothing can
   have moved. DreamKarts' renderer, input, profiler and RNG moved this way.
2. Otherwise `make hwbench` on the console, before and after (keep the
   old ELF and run both in one sitting: the numbers in a README are from
   other days and other builds). Console layout luck moves timings by 2-5% between unrelated builds, so
   read the CPU averages over the whole run, not one window. DreamKarts'
   HUD text, loop voices and kart strip builder moved this way.

Duplicating a module to keep a binary identical is not an option: once two
games want the same code it lives here, and the game that had it moves on
to the engine's copy.

## Gotchas

- Moving a function out of the file that calls it stops GCC inlining it
  (DreamKarts' RNG): hold it in the caller's file with an `_IMPL` macro
- Log strings are part of the image: keep a game's text identical with
  `ENG_NAME` and friends rather than rewording
- Link order sets the layout; keep a game's object order when swapping its
  sources for the engine's

## Not moved yet

DreamKarts' world chunk walk (`world.c` `draw_chunks`) does what
`scenery.c` does with per-track road sheen hooks on top, and its perf
overlay would make a module too. Each needs a console A/B when it moves.
