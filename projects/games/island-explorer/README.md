# Island Explorer

<p align="center">
  <img src="../../../docs/screenshots/island-explorer.png" alt="Island Explorer on the resort beach at Emerald Coast" width="640">
</p>

A coastal exploration prototype for the Dreamcast, built on KallistiOS. It
brings both Sonic sections of Emerald Coast into a new engine: walk, jog, jump
and swim around the beaches, pier, lagoon and cliffs with a collision-aware
orbit camera, a six-stop travel journal, animated ocean and shoreline wash,
leaping dolphin pods and an original stereo soundscape. No timer, no score.

This is an experimental prototype, not a release. It needs Sonic Adventure
assets extracted locally from your own disc image, so there is no downloadable
build and no game data in this repository.

## Controls

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

Exit with Start + A + B + X + Y, or close Flycast. The game keeps running
without a controller. Flycast's Tab opens its own menu and its Space is
fast-forward, not jump. A Maple keyboard also works: WASD walk, E jog, Space
jump, arrows look, P pause, Tab travel, C postcard, F3 diagnostics.

## Build and run

Tested disc: Sonic Adventure v1.003 PAL (M5). The extractor checks the stage
table counts and data pointers and stops on any other layout; other revisions
need their offsets checked in `tools/extract_stage.py`.

```sh
cd projects/games/island-explorer
uv run tools/extract_stage.py --gdi "/absolute/path/to/Sonic Adventure.gdi"
uv run tools/build_atmosphere.py
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
./run-flycast.sh --skip-build
```

`run-flycast.sh` without `--skip-build` builds first; run it from a terminal
to see the serial diagnostics. `make assets GDI=/path/to/disc.gdi` runs both
extraction steps, and a normal `make` re-runs the extractor whenever the
converter changes, so pass `GDI=` there too unless the disc is at the default
`~/Dropbox/Games/ROMs/DREAMCAST/Sonic Adventure (EU) # SDC/`. Python
dependencies come from `uv` script metadata; the font generator wants Avenir
Next and falls back to Arial.

Every build fetches the latest upstream [SH4ZAM](https://sh4zam.com/) `master`
into the ignored `third_party/sh4zam/` before compiling. A failed fetch stops
the build; `make clean` needs no network. See
[third_party/SH4ZAM.md](third_party/SH4ZAM.md).

Everything generated (`assets/extracted/`, `assets/generated/`,
`assets/island-explorer.blend`, `build/`, `captures/`, `third_party/sh4zam/`)
is ignored by Git.

## Blender

`assets/island-explorer.blend` holds an editable scene per area. Regenerate it
from Blender's Python console:

```python
import runpy
runpy.run_path("/absolute/path/to/island-explorer/tools/import_blender.py")
```

The importer replaces its named scenes, so save edits under another filename
first. Blender edits are not exported back into the runtime yet.

## Checks

| Target | Verifies |
| --- | --- |
| `make check-import` | Python unit tests for the disc, PRS, Ninja and PVR readers |
| `make check-math` | SH4ZAM camera transform, reciprocal and sine/cosine against scalar math on the emulated SH-4 |
| `make check-runtime` | Collision queries, menus and truncated-asset handling, then a walk-and-jump tour of all six stops (about 72 s) |
| `make check-dolphins` | Pod schedules in both areas, then an 88 s visual tour of every pod |
| `make benchmark` | SH4ZAM renderer against the reference renderer at 24 fixed poses |

Each target runs its diagnostic ELF in its own Flycast process and terminates
only that one; close other Flycast instances before `make benchmark`.
`FLYCAST_BIN` selects the emulator. Logs and benchmark JSON land in `build/`.

## Performance

Flycast measurements, not console numbers. Method and full tables are in
[PERFORMANCE.md](PERFORMANCE.md).

| Change | Result (24-pose benchmark) | Lesson |
| --- | --- | --- |
| SH4ZAM transforms, FSRRA reciprocals, Store Queue vertex copies, shared water corners (2026-09-29) | Render CPU 18.9 -> 17.3 ms, 46.6 -> 49.3 fps | Copying already-projected vertices straight to the TA was the biggest single win; camera math alone gave 2.2% |
| `-fno-math-errno -ffinite-math-only` on the game objects (see `Makefile`) | Render CPU 17.4 -> 11.4 ms | Without it `clampf`'s fminf/fmaxf stay libm calls. The render-math QA build keeps IEEE semantics so its `isfinite()` checks hold |

## Lessons learned

- Standard layout-2 (uncompressed, mipmapped) PVR files omit four bytes of the
  hardware mip-chain prefix. Upload one verbatim and every level shifts,
  giving dotted clouds and foliage. `tools/pvr.py` restores the bytes when it
  writes the `IET2` pack and `render.c` refuses older `IET1` packs
- The render pass owns XMTRX from camera load to scene submission; no
  matrix-mutating SH4ZAM or KOS helper may run inside it (`render_math.h`)
- FSRRA reciprocals are used only after the near-plane test; triangle clipping
  and physics keep signed division
- Submission is capped at 12,000 triangles a frame so the 1280 KB PVR vertex
  buffer cannot overflow. Small SET props and wildlife are culled beyond
  1,000 units (`render.c`)

## Known limitations

- Not tested on real Dreamcast hardware
- Travel between the two areas is a menu jump; nothing streams
- Enemies, set pieces, springs, objects and mission logic are not imported,
  nor is Big's alternate layout. No saves, collectibles or objectives
- The explorer is a placeholder character; some collision edge cases and
  worst-case frame pacing still need work

## Credits and licenses

Sonic Adventure geometry, textures and placements are Sega's work. They are
extracted locally from the user's own disc and are not included in this
repository or published anywhere. Format layouts and stage offsets were
cross-checked against [X-Hax SA Tools](https://github.com/X-Hax/sa_tools);
the small portable converter here stands in for the Windows editor. SH4ZAM is
MIT licensed: its notice is embedded in the ELF by `assets.S`, and the full
checkout with its license lives in `third_party/sh4zam/`.
