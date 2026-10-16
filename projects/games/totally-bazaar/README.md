# Totally Bazaar

Totally Bazaar is a traditional market trading card game for Sega Dreamcast,
for 2 to 4 players on one screen. Trade in the market, herd camels, sell goods
for rupee tokens and earn two Seals of Excellence to become the Maharaja's
merchant. Any seat can be a human or a CPU merchant (Easy or Hard), so it
also plays solo.

## Features

- The traditional market game rule set: take one, take all camels, multi-card
  exchanges with hand and herd, selling with the precious-goods minimum,
  shuffled 3/4/5-card bonus tokens, the camel token, both round-end
  conditions, tie-breaks, and best-of-seals matches
- 2-4 players; 3 and 4 player games use the same rules and components
- Hot-seat play with privacy: hands flip face down between human turns and a
  "pass the controller" screen recaps the last moves
- Every card is tracked individually and animates between deck, market,
  hands, herds and the discard pile with 3D flips
- Token coins fly from the bazaar to the seller, with secret bonus values
- Pre-rendered 3D goods art and logo (Blender), a sunset-palace title screen
  with a spinning card carousel, particles and confetti
- An original synthesized soundtrack (tanpura, tabla, sitar, bansuri and
  santoor over a kick/clap groove) plus 15 sound effects
- A hint button, a pause menu with a music on/off switch, a five-page How
  to Play, and an attract-mode demo after 30 seconds on the title screen

## Controls

| Dreamcast controller | Flycast keyboard default | Action |
| --- | --- | --- |
| D-pad or analog stick | Arrow keys | Move the cursor between market and hand |
| A | X | Pick or unpick a card; on your herd, add a camel |
| X | S | Make the trade shown in the hint bar |
| B or L trigger | C or F | Clear your picks |
| Y | D | Hint: the CPU picks a good move for you |
| R trigger | V | Pick every card of the hovered type |
| Start | Return | Pause menu (resume, music on/off, rules, quit) |

Picking a camel in the market picks all of them, as the rules require. The
hint bar always says what X will do ("Sell 3 Leather: 4+3+2 +bonus",
"Exchange: give 1 more card") or why the picks are not a legal move.
A+B+X+Y+Start returns to the title screen.

Any connected controller drives the current human merchant, so players can
share one pad or each hold their own. With two or more humans, hand the pad
over when the pass screen appears and press A when ready.

## Rules summary

Each turn you either **take** or **sell**:

- **Take one** goods card from the market; the deck refills its slot.
- **Take all camels** from the market into your herd.
- **Exchange** two or more market goods for the same number of cards from
  your hand and/or herd. You can't take and give back the same goods type.
- **Sell** any number of one goods type for tokens from that stack, highest
  first. Diamonds, gold and silver need at least two. Selling 3, 4 or 5+
  cards also earns a secret bonus token (1-3, 4-6 or 8-10 rupees).

Your hand holds at most 7 goods cards; camels don't count. A round ends at
once when three token stacks are empty or the market can't be refilled. The
largest herd earns the 5-rupee camel token (no award on a tie); the richest
merchant earns a Seal, with ties broken by bonus tokens, then goods tokens
(a tie that survives both gives each tied leader a Seal).
The round's lowest scorer starts the next round. A player who alone holds two
or more Seals wins.

The traditional game is for two players. For 3-4 players Totally Bazaar
keeps every rule and component unchanged: five cards each are dealt and turns
pass clockwise. The match goes on until one player alone leads with two or
more Seals.

## Build

Source KallistiOS, then build:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

This produces `totally-bazaar.elf` (KOS 2.3.0, GCC 15.2.0). The generated
texture, sound and music data in `assets/generated/` are checked in, so a
normal build needs no host tools. Each build first fetches the latest
[SH4ZAM](https://sh4zam.com/) master into the ignored `third_party/sh4zam`
(`tools/update-sh4zam.sh`), which supplies the FSCA sine/cosine and fast
`exp` used for animation.

## Run in Flycast

```sh
./run-flycast.sh
```

The launcher builds first unless given `--skip-build`. Each connected macOS
gamepad (up to four) gets its own Dreamcast port; with no gamepad, the
keyboard drives port A for pass-and-play. Force a mode with
`--input keyboard` or `--input gamepad`. Settings are passed as transient
`-config` flags, so saved Flycast mappings are untouched. The game does not
use the network or VMU saves.

## Run on a Dreamcast

`tools/dc-hw.sh` (repository root) power-cycles the console through its
Shelly smart plug, waits for GDEMU/openMenu to boot dcload-ip, uploads with
`dc-tool-ip` and captures the console output. dcload-ip takes its address
from DHCP, so the script finds it on the local network; the plug is at
192.168.1.173. Override with `DC_IP` and `DC_PLUG_IP`.

```sh
make hw        # play the release build on the console
make hwbench   # 150 s of four CPU merchants with a 10 s performance heartbeat
make hwsoak    # 30 minutes of two CPU merchants; fails on any stall report
```

Output goes to `hw-console.log` (`HW_LOG=` to change it; `HW_SECONDS=` and
`SOAK_SECONDS=` set the run lengths). The heartbeat reports frame rate, CPU
time per frame (average/max), the slowest CPU move decision, and the
PowerVR registration and render times. A watchdog thread reports which part
of the frame loop stops advancing if one ever does.

Builds made for the console never log per move: under dcload-ip each
`printf` is a network round trip made with interrupts disabled, and a lost
packet freezes the whole console (frozen picture, looping audio). Release
builds print only their boot lines.

On hardware the table runs at 60 fps with the PowerVR rendering a busy frame
in about 12-14 ms and the SH-4 spending about 1.5 ms per frame. Cards are
punch-through polygons, so stacked and overlapped cards cost about one
layer; the static HUD panels are baked into the opaque table texture; and
full-screen overlays tint the background instead of drawing a translucent
layer over the table.

## Tests

All three run inside Flycast and stop it automatically:

```sh
make selftest   # 310 rules checks + 120 simulated 2-4 player games
make uitest     # two hot-seat games played through the real controls
make autoplay   # four CPU merchants play two full games
```

- `selftest` covers every rule (camel takes, exchange limits, same-type
  swaps, hand limit, precious minimum, token order, bonuses, both round ends,
  camel token ties, seals, next starter) and checks card and token
  conservation after every simulated move. It also checks the Hard CPU beats
  random play and the Easy CPU.
- `uitest` drives the human seats with injected button presses: cursor
  moves, picks, herd camels, hints, pause menu, pass screens, round results,
  a 3-player (2 humans + CPU) game and a 4-player (3 humans + CPU) game.
  Every planned move must be produced by the real hint-bar move builder.
- `autoplay` is the attract-mode demo with log output; use it to soak-test.

## Regenerate assets

```sh
make art      # Blender renders of the goods/logo, then texture packing
make textures # texture packing only (Pillow, via uv)
make audio    # sound effects and the music loop (numpy/scipy, via uv)
```

`tools/render_art_blender.py` builds every 3D icon procedurally and renders
it with Cycles (`blender` on PATH, Blender 5.2 tested).
`tools/build_textures.py` composes card faces, the card back, token atlas,
bitmap fonts (Inter, as shipped with Blender, OFL), table and title
backgrounds, converts them to dithered 16-bit PowerVR formats and writes
previews to `assets/generated/previews/`. `tools/build_audio.py` synthesizes
the effects and the 76.8-second, 100 BPM music loop and encodes it with KOS
`wav2adpcm`; the loop is encoded three times over and the middle copy kept, so
the ADPCM predictor is continuous across the loop seam.

## Source layout

```text
src/rules.[ch]     rules engine (pure C, card ids, validation, scoring)
src/ai.[ch]        CPU merchants: one-ply evaluation on public information
src/selftest.[ch]  rules and simulation tests (BAZAAR_SELFTEST builds)
src/gfx.[ch]       PowerVR 2D layer: textures, sprites, 9-slice panels, text,
                   punch-through card batch
src/fastmath.h     SH4ZAM wrappers for per-frame trig and easing
src/audio.[ch]     looping ADPCM music stream and sound effects
src/main.c         game flow, layout, card animation, input, HUD, screens
src/blobs.S        links the generated texture, sound and music data
```
