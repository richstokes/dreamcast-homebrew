# Chroma Circuit

![Chroma Circuit's four-dimensional Hyperfold scene](../../../docs/screenshots/chroma-circuit.png)

Chroma Circuit is a standalone Dreamcast demoscene production written entirely
in SH-4 assembly. It does not link KallistiOS, a C runtime, or any other
library: it drives the Dreamcast's video, PowerVR2, and Yamaha AICA hardware
directly. All geometry is generated at runtime with no textures or stored
meshes, and the music is synthesized live on the AICA.

The demo is a nine-act, 60 Hz camera sequence that loops after about 115
seconds, with an original 54-bar score that follows the same timeline:

1. **Orbit Core** - a corkscrewing dive around a faceted torus with a helicoid
   and pulsing energy cores threaded through it
2. **Neon Vault** - a race down a curved, rolling nave of stained-glass bays,
   emissive arches, and recursively nested gates
3. **Chaos Bloom** - a live Lorenz attractor traced by luminous ribbons and
   flaring shards
4. **Hyperfold** - a real four-dimensional hypercube folding inside-out, with
   cyan/violet time ghosts
5. **Strange Form** - a cold green-and-steel fracture engine that explodes and
   reassembles to industrial drum-and-bass
6. **Machine Dream** - a fly-through of bronze generators, industrial cages,
   and a steel funnel
7. **High Country** - a terrain-following flight from cloud through a gorge to
   a green-gold alpine panorama
8. **Navigator** - an orbiting inspection of an organic wireframe spacecraft
9. **Event Horizon** - a gravitationally lensed black hole with an accretion
   disk, ending as the horizon swallows the frame

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/chroma-circuit.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/chroma-circuit.elf)
is also available.

## Controls

The demo runs continuously and a controller is optional. Press **D-pad Left**
or **D-pad Right** (the arrow keys in Flycast) to jump to the previous or next
act. Each act names itself in the bottom-right plaque.

## Demoscene inspiration

Every act is original geometry, code, and music, but several are tributes to
the PC demoscene. None include assets, source, or music from the originals.

- **Hyperfold** borrows the visual vocabulary of 2000-2004 demos such as
  Kewlers' [Variform](https://www.pouet.net/prod.php?which=7138), Farbrausch's
  [fr-019: poemtoahorse](https://www.pouet.net/prod.php?which=5569), and ASD's
  [Dreamchild](https://www.pouet.net/prod.php?which=10530)
- **Strange Form** draws on Conspiracy's 2006
  [*Chaos Theory*](https://chaostheory.conspiracy.hu/about.php)
- **Machine Dream** is a tribute to Farbrausch's 2000 64 KiB intro
  [*fr-08: .the .product*](https://demozoo.org/productions/10103/)
- **High Country** is an interpretation of RGBA and TBC's 2009 4 KiB intro
  [*Elevated*](https://demozoo.org/productions/102/)
- **Navigator** is modeled on the spacecraft silhouette from *Flight of the
  Navigator*
- **Event Horizon** follows the look of the *Interstellar* black hole and the
  2019 EHT image, using a thin-lens approximation rather than a ray tracer

## Build

The Makefile invokes the installed Dreamcast cross-binutils directly. Do not
source the KOS environment; no KOS build rule or compiler wrapper is used.

```sh
cd projects/apps/chroma-circuit
make
make verify
```

`make verify` checks the ELF headers, entry point, and undefined symbols.

To use a different cross-binutils installation, override `TOOLCHAIN`:

```sh
make TOOLCHAIN=/absolute/path/to/sh-elf/bin
```

## Run in Flycast

```sh
./run-flycast.sh
```

Or, after an existing build:

```sh
./run-flycast.sh --skip-build
```

On macOS the launcher uses a connected gamepad, or the keyboard if none is
present, without changing saved Flycast preferences. Use `--input gamepad` or
`--input keyboard` to override. Boot diagnostics are printed to the serial
console in the launching terminal.

## Hardware status

Tested in Flycast 2.7, where it holds a steady 60 Hz with no missed vertical
blanks. The code includes VGA and 480i timing paths intended for real hardware,
but it has not yet been run on a physical Dreamcast.
