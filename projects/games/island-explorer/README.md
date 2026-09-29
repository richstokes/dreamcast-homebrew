# Island Explorer

A native Dreamcast / KallistiOS coastal exploration game. This first pass brings
both Sonic sections of Emerald Coast into a new engine, with a walking explorer,
jumping, swimming, a collision-aware orbit camera, a six-stop travel journal,
animated ocean, shoreline wash, and an original stereo coastal soundscape.

The visual direction is a quiet, bright summer holiday: original low-resolution
textures, soft distance haze, turquoise water, unobtrusive field-note typography,
and no time limit or score. The explorer is deliberately a simple animated
placeholder with a sunhat and backpack.

## Play

From the repository root:

```sh
projects/games/island-explorer/run-flycast.sh
```

The launcher sources KOS, builds, and opens the ELF in Flycast with serial output.
Use `--skip-build` to run the existing build. It uses the repository's Flycast
resolver and does not change persistent emulator or network settings. Networking
is not used. Launch from a terminal to keep startup and performance diagnostics.

Every build fetches the latest upstream [SH4ZAM](https://sh4zam.com/) `master`
into an ignored local checkout before compiling. It is not bundled or pinned;
a failed update stops the build. `make clean` does not need network access.
See [integration details](third_party/SH4ZAM.md) and
[measured performance](PERFORMANCE.md).

| Action | Dreamcast controller | Flycast default keyboard mapping |
| --- | --- | --- |
| Walk | Analog stick | I / J / K / L |
| Jog | Hold B while moving | Hold C |
| Jump / leave the water | A | X |
| Orbit camera | L / R triggers or D-pad left/right | F / V or left/right arrows |
| Camera tilt | D-pad up/down | Up/down arrows |
| Recenter | Both triggers | F + V |
| Travel journal | Y | D |
| Choose destination | D-pad up/down, then A | Up/down arrows, then X |
| Close travel journal | B | C |
| Pause | Start | Return |
| Toggle postcard view | X | S |
| Toggle sound while paused | B | C |

Flycast's Tab key opens its own menu. Its Space key is fast-forward, not jump.
Custom emulator mappings take precedence. Exit the game with
Start + A + B + X + Y, or close Flycast. The game continues safely without a
controller; connect one to resume input.

A real Dreamcast keyboard is also supported if configured as a Maple keyboard:
WASD walk, E jog, Space jump, arrows look, P pause, Tab travel, C postcard, F3
diagnostics. This is distinct from a host keyboard emulating a controller.

## Rebuild the local assets

The asset converter reads the user's GDI and track files without modifying them.
The tested disc is **Sonic Adventure v1.003 PAL (M5)**. The converter validates
stage table counts and data pointers; other revisions need their offsets checked.

```sh
cd projects/games/island-explorer
uv run tools/extract_stage.py --gdi "/absolute/path/to/Sonic Adventure.gdi"
uv run tools/build_atmosphere.py
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
file island-explorer.elf
sh-elf-readelf -h island-explorer.elf
```

On this workstation the extractor also discovers the existing PAL GDI in
`~/Dropbox/Games/ROMs/DREAMCAST/Sonic Adventure (EU) # SDC/` when `--gdi` is omitted.
After sourcing KOS, `make assets GDI="/absolute/path/to/disc.gdi"` runs both steps.
Python dependencies are managed by `uv` script metadata. Font generation uses
the workstation's Avenir Next, falling back to Arial.

Generated files are local and ignored by Git:

- `assets/extracted/`: selected disc files, decoded texture previews, editable
  mesh JSON and a manifest with SHA-256 hashes of the extracted source files.
- `assets/generated/`: native geometry/texture packs, font, PCM, a WAV preview
  and audio measurements.
- `assets/island-explorer.blend`: editable Blender scenes with packed textures.
- `build/` and `captures/`: diagnostic logs and screenshots.
- `third_party/sh4zam/`: the fetched upstream library checkout.

Sonic Adventure geometry, textures, and original placements remain Sega's work.
They are extracted locally and are not included in the source patch. This is a
local development prototype; no game data has been uploaded or published.

## Blender handoff

Open `assets/island-explorer.blend`. There are two named Emerald Coast scenes;
the original default scene is preserved. Each scene separates visible geometry
from a hidden collision-only collection. Meshes retain original UVs, texture
indices and surface/material flags as custom properties. Coordinates convert
Dreamcast `(x,y,z)` to Blender `(x,-z,y)`.

To regenerate the scene in Blender's Python console:

```python
import runpy
runpy.run_path("/absolute/path/to/island-explorer/tools/import_blender.py")
```

Save edited scenes under another filename before regenerating. The importer
replaces its named scenes. The current engine pipeline builds from extracted
mesh data; Blender edits are not yet exported back into the runtime.

## Engine and conversion

- `main.c`: controller input, movement, gravity, swimming, camera and travel.
- `world.c`: validated geometry pack access, walkable floors, body/wall sliding
  and camera ray queries. The original stage collision meshes are retained.
- `render.c`: PowerVR rendering at 640x480, SH-4 matrix transforms, material
  batches, near clipping, CPU culling, fog, ocean, foam, explorer and UI.
- `render_math.h`: SH4ZAM camera transforms, guarded positive reciprocals and
  paired sine/cosine. Physics retains its scalar arithmetic.
- `audio.c`: AICA stereo streaming plus footsteps. The original procedural
  28-second soundscape layers rolling surf, breaking foam and distant gulls.
- `tools/disc.py`, `ninja.py`, `pvr.py`, `extract_stage.py`: bounded PRS decoding,
  GDI/ISO reading, Ninja BASIC geometry and native PVR texture conversion.

The two areas contain 122,331 triangles in 6,781 material batches, including
1,049 decorative/traversable SET placements and the original long ocean pier.
286 original textures stay in their native mipmapped/VQ formats for Dreamcast
VRAM efficiency. Both areas are resident, allowing immediate travel. Small
props are distance-culled; a 12,000-triangle submission limit protects the PVR
buffer. These are compromises for the retail 16 MB RAM / 8 MB VRAM target.

Format layouts and stage offsets were checked against
[X-Hax SA Tools](https://github.com/X-Hax/sa_tools) at commit
`5d52a39a04e056b8a7e690ce87386750b4b07058`, particularly
`GameConfig/DC_SA1/STG01.ini`, SAModel's Ninja structures and the Emerald Coast
level-effects definition. A small portable converter is used here instead of
requiring the Windows-oriented editor. The Blender scene was created through
the connected Blender MCP; it does not require the Sonic Adventure IO add-on.

## Verification

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make clean
make
make check-import
make check-math
make check-runtime
make benchmark
```

The diagnostic ELF checks floor/wall/camera queries, menu actions and truncated
assets on the SH-4, then visits all six locations. Each stop must register a
jump, an airborne state and meaningful movement. It prints a PASS/FAIL summary
and exits KOS cleanly after about 72 seconds. This drives the game logic directly;
it is not a physical controller-input test.

The check runner starts each diagnostic ELF serially, verifies its completion
marker and clean exit, and terminates only the emulator process it created.
Close other Flycast instances before running performance checks. Logs and the
24-pose benchmark comparison are written to ignored `build/` files. Set
`FLYCAST_BIN` to choose another Flycast executable.

Validated on 2026-09-29 with KOS 2.3.0, GCC 15.2.0 and the repository's locally
built Flycast. The normal ELF is 32-bit, little-endian Renesas SH. The six-stop
tour passed with no KOS panic, missing resource or audio underrun. The initial
renderer measured roughly 28–59 fps depending on viewpoint; the repeatable
SH4ZAM comparison is recorded in [PERFORMANCE.md](PERFORMANCE.md). The procedural audio
measurement reports no clipped samples and zero sample discontinuity at its
loop seam. These checks do not replace listening on speakers/headphones.

## First-pass boundaries

Real Dreamcast hardware has not been tested. Character art, transitions between
the two areas, some collision edge cases and worst-case frame pacing still need
refinement. Travel connects the two original areas; there is no seamless world
streaming yet. This preserves the level as an exploration environment, not its
original enemies, scripted set pieces, spring routes, objects or mission logic.
Big's alternate layout is not imported. There are no saves, collectibles or
objectives in this pass.
