# Chroma Circuit

<p align="center">
  <img src="../../../docs/screenshots/chroma-circuit.png" alt="Chroma Circuit's four-dimensional Hyperfold scene" width="640">
</p>

Chroma Circuit is a Dreamcast demoscene production written entirely in SH-4
assembly. It links no KallistiOS, C runtime or other library and drives the
video hardware, PowerVR2 and Yamaha AICA directly. Nine acts run at 60 Hz and
loop after about 115 seconds, with an original 54-bar score on the same
timeline. All geometry is generated at runtime (no textures or stored meshes)
and the music is synthesized live on the AICA.

1. **Orbit Core** - a corkscrewing dive around a faceted torus threaded with a helicoid and pulsing energy cores
2. **Neon Vault** - a race down a curved, rolling nave of stained-glass bays and nested gates
3. **Chaos Bloom** - a live Lorenz attractor traced by luminous ribbons and flaring shards
4. **Hyperfold** - a four-dimensional hypercube folding inside-out, with cyan/violet time ghosts
5. **Strange Form** - a green-and-steel fracture engine that explodes and reassembles to drum-and-bass
6. **Machine Dream** - a fly-through of bronze generators, industrial cages and a steel funnel
7. **High Country** - a terrain-following flight from cloud through a gorge to an alpine panorama
8. **Navigator** - an orbiting inspection of an organic wireframe spacecraft
9. **Event Horizon** - a gravitationally lensed black hole whose horizon ends up swallowing the frame

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/chroma-circuit.cdi)**
for Flycast or GDEMU. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/chroma-circuit.elf) is also available.
If you are burning a physical CD-R, you may want to try the padded
[CD-R image](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/chroma-circuit-cdr.zip) instead (unzip before burning).

## Controls

The demo runs on its own; a controller is optional. Each act names itself in
the bottom-right plaque.

| Input | Action |
| --- | --- |
| D-pad Left (left arrow in Flycast) | Previous act |
| D-pad Right (right arrow in Flycast) | Next act |

## Build and run

No KOS environment is needed. The Makefile calls `sh-elf-as` and `sh-elf-ld`
directly; override their location with `TOOLCHAIN=/path/to/sh-elf/bin`.

```sh
cd projects/apps/chroma-circuit
make
make verify
./run-flycast.sh               # builds first; add --skip-build to reuse the ELF
```

The launcher uses a connected USB gamepad, or the keyboard if none is present,
without changing saved Flycast preferences (`--input gamepad|keyboard` or
`CHROMA_CIRCUIT_INPUT` overrides it; `FLYCAST_BIN` selects another Flycast).
Boot diagnostics are printed to the serial console in the launching terminal.

## Tests and QA

| Command | Verifies |
| --- | --- |
| `make verify` | Renesas SH ELF, `_start` at `0x8c010000`, no undefined symbols |
| `./run-flycast.sh` serial log | A `PERF missed-vblank frames / 600 = 0x...` line every 10 s; `0` is a clean 60 Hz |
| `make hwdebug && ./hwdebug-run.sh [elf] [seconds]` | Boot on the real console: power-cycles the Shelly plug (`DC_PLUG_IP`), waits for dcload-ip (`DC_IP`), uploads the ELF and captures its console to `dcload-console.log` |

The hwdebug build paints the PVR border a colour per boot stage and blinks a
code on a fatal PVR error; the colours are listed beside the `DBG` calls at
`_start` in `chroma-circuit.s`. QA-only assembler hooks
`--defsym CAMERA_CAPTURE_FRAME=n` (freeze one timeline frame) and
`CAMERA_START_FRAME=n` (start there and keep running) are described at the top
of the main loop.

## Hardware status

- Flycast 2.7: steady 60 Hz with no missed vertical blanks
- Real NTSC Dreamcast via dcload-ip: 60 Hz except the first moments of Chaos Bloom, and Event Horizon, which runs at roughly 22 fps
- Disc boots (GDEMU/CDI) have not been checked on hardware yet

## Demoscene inspiration

Every act is original geometry, code and music, but several are tributes to
the PC demoscene. None include assets, source or music from the originals.

- **Hyperfold** borrows the visual vocabulary of 2000-2004 demos such as Kewlers' [Variform](https://www.pouet.net/prod.php?which=7138), Farbrausch's [fr-019: poemtoahorse](https://www.pouet.net/prod.php?which=5569) and ASD's [Dreamchild](https://www.pouet.net/prod.php?which=10530)
- **Strange Form** draws on Conspiracy's 2006 [*Chaos Theory*](https://chaostheory.conspiracy.hu/about.php)
- **Machine Dream** is a tribute to Farbrausch's 2000 64 KiB intro [*fr-08: .the .product*](https://demozoo.org/productions/10103/)
- **High Country** is an interpretation of RGBA and TBC's 2009 4 KiB intro [*Elevated*](https://demozoo.org/productions/102/)
- **Navigator** is modeled on the spacecraft silhouette from *Flight of the Navigator*
- **Event Horizon** follows the look of the *Interstellar* black hole and the 2019 EHT image
