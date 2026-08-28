# Chroma Circuit

![Chroma Circuit's four-dimensional Hyperfold scene](../../docs/screenshots/chroma-circuit.png)

Chroma Circuit is a standalone Dreamcast demoscene effect written entirely in
SH-4 assembly. It does not link KallistiOS, a C runtime, libc, libgcc, or any
other library. The executable enters at `0x8c010000` and drives the Dreamcast's
video, PowerVR2 tile accelerator, Yamaha AICA synthesizer, Holly event
registers, SH-4 store queues, SCIF serial port, caches, and FPU directly.

The demo is a 3,072-frame, four-act camera sequence rather than a fixed-view
object spin. At Dreamcast's 59.9453 Hz refresh rate the complete loop lasts
about 51.25 seconds; each act gets 768 frames and the cuts are masked by a short
additive white-energy flash.

1. **Orbit Core** - the shot holds a wide, elevated view for two seconds, then
   cosine-eases from radius 8.6 to 3.2 in a 128-frame plunge. The camera then
   accelerates through almost two close major-axis laps while a second phase
   climbs, dives, and banks it through nearly three corkscrew loops. Equator
   crossings skim 0.87 world units above the torus surface; the intervening
   4.2-radius crests restore a readable full-ring silhouette. The torus facets
   are world-locked so their parallax proves that the lens—not the object—is
   orbiting. A translucent helicoid threads through the ring while two
   independently counter-rotating octahedral energy cores pulse inside it.
2. **Neon Vault** - the camera races down a curved, rolling octagonal nave made
   from 18 runtime-generated cross-sections. Broken stained-glass wall bays,
   emissive arches, longitudinal rails, and recursively nested gates turn the
   PowerVR2 into a moving piece of impossible architecture. The nearest ring
   continually opens onto the next one, so the scene never reads as a flat
   tunnel texture.
3. **Chaos Bloom** - a live Lorenz system feeds a 512-sample circular history
   buffer. Two crossed luminous ribbons trace the strange attractor while 64
   paired shards flare along its age gradient. The parameters breathe and new
   simulation points are integrated every rendered frame, so this is evolving
   geometry rather than a prerecorded butterfly mesh.
4. **Hyperfold** - sixteen vertices of a real four-dimensional hypercube rotate
   independently through its XW, YZ, XY, and ZW planes before a live 4D-to-3D
   perspective divide and the ordinary 3D camera projection. Twelve
   phase-delayed copies become cyan/violet geometric time ghosts; all 32 edges
   are expanded into luminous screen-space beams and the current slice carries
   16 hot nodes. A bar-synchronized fourth-dimensional eye distance repeatedly
   folds the nested cubes inside-out while the camera orbits and banks around
   the result.

An original 24-bar nocturnal synthwave score follows the same timeline. Orbit
Core begins with a restrained F-sharp-minor add9 pad, brings in the pulse and
drums as the camera dives, and accents the close surface skim. Neon Vault opens
into the wide hook; Chaos Bloom fractures it into a bridge. Hyperfold is a new
six-bar final drop with bass inversions, a continuous high-register answer,
wider beat-rate stereo motion, mirrored kicks, and impacts on the two major
geometry folds before a C-sharp dominant cadence resolves seamlessly into the
loop. The harmony uses explicit add9, major-7, minor-7, sixth, and dominant
color tones rather than a generic repeating triad.

The busiest act, Chaos Bloom, submits 2,172 world-space triangles per frame:
2,044 from its two ribbons and 128 from the crossed shards. Neon Vault submits
860 world-space triangles, while Orbit Core submits 718. Hyperfold submits 768
beam triangles and 32 node triangles after computing 192 four-dimensional
vertex transforms per frame. Torus, helicoid, vault, attractor, and hypercube
geometry are generated or updated by SH-4 assembly at runtime; camera
transforms, reciprocal-Z projection, color selection, and PowerVR submission
also happen per vertex. No transformed mesh table is stored in the ELF.

## Demoscene inspiration

Hyperfold is an original effect built from the visual vocabulary of the
2000-2004 PC scene rather than a reproduction of one production. Contemporary
demos increasingly made the polygon their primary primitive and commonly used
3D flybys, tunnels, particles, interference, ribbons, blur, and continuous
procedural transformation. In particular, Kewlers' 2002
[Variform](https://www.pouet.net/prod.php?which=7138) is remembered for
interference and glitter, Farbrausch's 2002
[fr-019: poemtoahorse](https://www.pouet.net/prod.php?which=5569) for generated
organic/faceted forms inside 64 KiB, and ASD's 2003
[Dreamchild](https://www.pouet.net/prod.php?which=10530) for transformations
that flow instead of merely cutting. The implementation translates those
principles to Dreamcast-native strengths: tiny topology, analytical motion,
additive geometry, and a deterministic music timeline.

## Build

The Makefile invokes the installed Dreamcast cross-binutils directly. Do not
source the KOS environment; no KOS build rule or compiler wrapper is used.

```sh
cd projects/chroma-circuit
make
make verify
```

`make verify` prints the file type and ELF/program headers, verifies `_start`,
and rejects undefined symbols. A valid result is a 32-bit little-endian Renesas
SH ELF with `_start` at `0x8c010000` and a load segment beginning there.

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

The launcher uses an absolute ELF path, enables Flycast's serial console and
vertical sync, disables duplicate-frame presentation, and forces both manual
and automatic frame skipping off. It also detects Flycast 2.7's intermittent
macOS dynarec address-reservation assertion and retries a fresh process rather
than misreporting that host-emulator failure as a guest crash. A successful
boot prints:

```text
CHROMA CIRCUIT // bare SH-4 entry
MAPLE: direct A0 DMA, LEFT/RIGHT scene select
AICA: thirteen-slot raw synthwave tracker online at 112.40 BPM
PVR2: direct registers, tile matrix, no SDK runtime
TA: four-act orbit + vault + chaos + hyperfold online
```

The demo runs continuously with a controller optional. Press **D-pad Left** or
**D-pad Right** to jump immediately to the previous or next act; the selection
wraps at either end. Flycast's default keyboard mapping uses the **Left Arrow**
and **Right Arrow** keys. Every act identifies itself in the bottom-right
plaque as `01 ORBIT CORE`, `02 NEON VAULT`, `03 CHAOS BLOOM`, or
`04 HYPERFOLD`. Manual jumps land just beyond the transition flash so the new
title and scene are visible immediately; the tracker cuts old drum tails and
revoices the destination harmony on that same complete frame. Close Flycast,
or press Control-C in the launching terminal, to stop it.

## Raw AICA soundtrack

Dreamcast does not contain a General MIDI sound set or ROM instrument bank.
Its MIDI registers are serial I/O, while the Yamaha AICA itself is a 64-slot
PCM/ADPCM wavetable synthesizer. Chroma Circuit implements the period-correct
equivalent of a tracker/module player directly in SH-4 assembly:

- Five signed, zero-DC, 32-sample PCM8 oscillator waves occupy only 160 bytes
  at sound-RAM offset `0x30000`. No prerecorded song or streamed mix is hidden
  in the executable.
- Thirteen autonomous hardware slots provide a rounded digital arpeggio,
  doubled pulse lead, true low pulse bass, four-note stereo pad, delayed saw
  tap, re-keyed triangle kick, gated noise snare with a tonal body, and separate
  hat/crash noise.
- AICA's own pitch LFO supplies slow, different-rate drift to the pad and a
  subtle chorus offset to the doubled lead. The effect continues in hardware
  without consuming SH-4 transform time.
- One tracker tick equals one displayed frame. Eight ticks make a row, four
  rows make a beat, and the effective tempo is 112.40 BPM. Six 16-row bars fit
  each 768-frame scene exactly.
- The ARM7 remains in reset: SH-4 uploads the oscillator waves over G2, starts
  the complete voice bank with one `KYONEX`, then writes only pitch, pan, and
  total-level registers. A two-execute key-off/key-on sequence gives the kick
  a deterministic zero-phase attack while already-running voices continue.
- Every G2 transaction is 32-bit and uses a bounded FIFO drain. Waveform
  readback gates startup, and a 600-frame serial counter verifies both tracker
  row cadence and live slot-zero sample-position changes.

This is real AICA synthesis on Dreamcast hardware, not host MIDI playback, an
emulator audio trick, KOS's sound server, or a software PCM mixer.

## Low-level rendering path

- Detects VGA versus standard A/V cable pins and programs 640x480 timings.
- Enables and invalidates the SH-4 instruction and operand caches through P2.
- Builds a raw port-A Maple `GETCOND` DMA descriptor, reads controller replies
  through an uncached P2 alias, validates response/function words, converts the
  active-low button mask, and edge-detects Left/Right without KOS. The command
  table and 1 KiB response area are isolated on 32-byte cache lines.
- Runs the Maple pipe asynchronously twice per rendered frame for quick taps:
  no controller wait can stall the renderer. A bounded four-frame recovery
  path safely re-arms a missing or wedged device.
- Creates two native PVR tile matrices, opaque/translucent object pointer
  buffers, 512 KiB vertex regions, and RGB565 framebuffers in VRAM. Fifteen
  complete OPB overflow banks per set consume otherwise idle texture space so
  lens-filling triangles cannot evict the vector HUD from a hot 32x32 tile.
- Streams 32-byte polygon and vertex parameter blocks through store queue zero.
- Polls TA list-complete, ISP render-complete, and scanline hardware status,
  then flips the framebuffer at the vertical refresh boundary.
- Bounds every hardware wait and reports TA, render, or scanline failures over
  SCIF before halting, so a malformed PVR transaction is not a silent freeze.
- Uses reciprocal-Z depth testing for solid geometry and ONE/ONE blending for
  the helicoid, dual crystal core, star tunnel, grid, orbital markers, vault
  light structures, Lorenz ribbons, shards, Hyperfold beams/nodes, and
  scene-change flash. The depth buffer makes intersecting layers disappear
  behind and re-emerge in front of one another without CPU-side triangle
  sorting.
- Builds a shared spherical-orbit camera once per frame. A staged hold/dive
  timeline feeds a close Lissajous rail with independent yaw, pitch, roll, and
  second-harmonic dolly motion. Radius minima coincide with zero pitch, so the
  close beats actually cross the torus equator instead of missing it above or
  below, while every submitted world vertex retains positive camera-space Z.
- Builds the vault's 18 seam-closed octagonal rings into a compact per-frame
  cache, then reuses them across wall, arch, rail, and recursive-gate passes.
- Advances a nonlinear Lorenz integrator continuously during Chaos Bloom and
  projects its 512-point history through a dedicated analytic orbit camera.
- Rotates a unit hypercube through four independent 4D planes, performs a
  fourth-dimensional perspective divide, projects 12 delayed copies through a
  live 3D camera, and expands their 384 edges into constant-width screen-space
  beams with SH-4's reciprocal-square-root instruction.
- Uses TMU1 as a free-running hardware timer and prints a 600-frame cadence
  report over SCIF. The counter is re-primed after serial output so diagnostics
  cannot contaminate the next measurement window.

The checked-out KOS headers and implementation were used only as a local
register-level reference. No KOS object, header, startup routine, or API is
part of the build.

The assembly routines use a deliberately private register convention tuned for
the closed demo call graph. They are not drop-in SH ELF ABI functions without
adding the usual callee-save wrappers.

## Tested configuration

Tested by direct ELF boot in Flycast 2.7 with its Vulkan renderer and default
keyboard-to-port-A controller mapping. The serial startup completed without a
panic or missing resource; a diagnostic run received 600 valid Maple replies
in 600 frames with zero malformed replies and zero DMA timeouts. Orbit Core was
also rendered in assembly-time visual-QA builds frozen at local frames 96, 184,
248, 269, 291, and 333 to inspect the wide view, dive, skim, bank, crest, and
second skim independently. Those builds freeze only the timeline and preserve
normal framebuffer alternation; saved captures were decoded at pixel level so
desktop preview/differential-capture artifacts could not masquerade as missing
HUD geometry. The closest and most overlap-heavy frames were also used to size
the TA overflow area conservatively for real hardware.
All four acts, their named bottom-right plaques, and their transitions remained
stable across repeated full 3,072-frame loops, and both framebuffer sets
rendered correctly.
Repeated independent 600-frame windows—including the final twice-per-frame
input sampler—reported zero missed vertical blanks and `0x077516c4` or
`0x077516e0` TMU1 ticks, within 28 ticks (about 2.3 microseconds total) of
Flycast's theoretical 600-refresh interval at 59.9453 Hz. The launcher
transiently forces vertical sync on, duplicate-frame presentation off, and both
manual and automatic frame skipping off, keeping presentation centered around
60 fps without depending on saved Flycast preferences. The code includes VGA
and 480i timing paths intended for real hardware, but it has not yet been
exercised on a physical Dreamcast.

The final soundtrack was also captured from Flycast's actual SDL AICA output
as 44.1 kHz signed 16-bit stereo for 64.55 seconds, longer than one complete
song loop. The capture had no clipped or near-clipped samples, only 0.0005%
zero-valued stereo frames, negligible DC offset (under one sample unit per
channel), and a peak of -12.34 dBFS. Per-act stereo RMS remained consistent:
-30.97 dBFS for Orbit Core, -30.43 for Neon Vault, -30.50 for Chaos Bloom,
and -28.55 for Hyperfold's intentional final lift. Ordinary 600-frame windows
reported `0x4b` or `0x4c` decoded rows and the same number of observed
slot-zero phase changes, while the renderer continued to report zero missed
vertical blanks.

Generated `.o`, `.elf`, and screenshot files are intentionally ignored. Use
`make clean` to remove build products.
