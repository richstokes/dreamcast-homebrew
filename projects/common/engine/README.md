# Engine

Shared game code for KallistiOS games in this repo. DreamKarts and Ridgeline
build on it, so a fix or speed-up here reaches both.

| Module | What | Used by |
| --- | --- | --- |
| `vecmath.h` | Vectors, scalar helpers, packed colours, the camera (all inline) | DreamKarts, Ridgeline |
| `rand.h`, `rand.c` | The seeded generator; a game can hold it in its own file (`ENG_RAND_IMPL`) so calls inline | DreamKarts, Ridgeline |
| `input.h`, `input.c` | Two pads with edge detection and menu repeat, triggers as buttons, the keyboard | DreamKarts, Ridgeline |
| `render.h`, `render.c`, `xform.S` | PowerVR lists, header cache, texture pool, near-plane clipping, strips and rigid meshes sent through the store queues, SH-4 assembly vertex paths | DreamKarts, Ridgeline |
| `prof.h`, `prof.c` | Per-section cycle and stall counters for console benchmarks | DreamKarts, Ridgeline |
| `mesh.h`, `mesh.c` | Builds rigid models (vehicles) into `r_mesh_kv` strips with per-group lighting | Ridgeline |
| `scenery.h`, `scenery.c` | Static scenery pool: strips and chunks with bounding spheres and distance LOD | Ridgeline |
| `sfx.h`, `sfx.c` | Effect bank in AICA RAM, one-shots, looping voices on reserved channels | Ridgeline |
| `text.h`, `text.c` | Bitmap text from a font atlas | Ridgeline |

Tools (`tools/`): `pvr_texture.py` (twiddled RGB565/ARGB4444 with mips),
`fontatlas.py` and `fonts/`, `meshkit.py` (procedural low-poly meshes),
`stripify.py`, `blender_props.py` (Blender prop baker) and `microbench/`
(cycles per vertex for `xform.S` on the console).

## Using it

Add `-I../../common` and include `engine/render.h` and friends. Compile the
engine's `.c` files with your own flags (see either game's Makefile). Each
game supplies three headers on its include path:

| Header | Holds |
| --- | --- |
| `engine_config.h` | `ENG_NAME`, `ENG_DT`, the vertex buffer size, test switches (`ENG_PROF`, `ENG_INPUT_TAPS`, ...) |
| `engine_assets.h` | Read by `render.c` only: the texture table and pak, mip bias, optional texture sets, PVR settings (bins, fog, clear colour) |
| `prof_ids.h` | Profile section and count ids, including the renderer's |

## Rule: no cost to existing games

Moving code here must not slow a game down. The check that settles it:
build the game's release, autotest, hwbench and capture variants before and
after with a fixed SHA, `sh-elf-objcopy -O binary` each ELF and compare.
Only `__TIME__` may differ. DreamKarts passed this when its renderer, input,
profiler and RNG moved here; console layout luck moves timings by 2-5%
between unrelated builds, so anything that isn't byte-identical needs an
A/B on the console.

## Gotchas

- Moving a function out of the file that calls it stops GCC inlining it
  (DreamKarts' RNG): hold it in the caller's file with an `_IMPL` macro
- Log strings are part of the image: keep a game's text identical with
  `ENG_NAME` and friends rather than rewording
- Link order sets the layout; keep a game's object order when swapping its
  sources for the engine's

## Not moved yet

These live in DreamKarts and have engine counterparts or would make good
modules, but moving them changes DreamKarts' code layout, so each needs a
console A/B first: the kart mesh grouping (`models.c`, now `mesh.c`), its
audio voices (`audio.c`, now `sfx.c`), the world chunk walk (`world.c`,
now `scenery.c`), HUD text (`hud.c`, now `text.c`), the frame pacing loop
in `main.c`, and the perf overlay.
