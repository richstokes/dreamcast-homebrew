# DreamKarts

<p align="center">
  <img src="../../../docs/screenshots/dreamkarts.png" alt="DreamKarts Chao drifting with purple mini-turbo sparks on the beach oval" width="640">
</p>

A kart racer for the Sega Dreamcast: early-2000s looks, modern Mario
Kart-style handling. Hop into a drift, hold the powerslide with
counter-steer, watch the sparks turn blue, orange and purple, then let go
for a mini-turbo. You drive a Chao against five CPU rivals, a friend in
split screen, or up to five other Dreamcasts online.

Seven courses:

- Emerald Beach: a beach oval with a boardwalk straight over the sea and dash panels
- Coral Crossing: a figure 8 whose ramp jumps one leg over the other
- Palm Harbour: a twisty harbour circuit with a chicane and two hairpins
- Temple Hill: a jungle hill climb (18 m) with switchbacks, a summit hump and a stone viaduct
- Neon Skyway: 1,056 m of neon road above a cyberpunk city at night, with a banked loop, a gap jump and open edges to fall off
- Cyberstone: Neon Skyway's city on a layout after Silverstone's National circuit, all hills and no jumps
- Spiral Canyon: a hill-zone valley of palms, bushes, sunflowers, checkered loops, totem poles and tiki torches at golden hour, a retrowave sun setting behind the hills, where two banked corkscrews climb and descend 20 m as ledges round slate towers, through torchlit tunnels where they pass under themselves, with a hump on the summit ridge and a chicane on the way home

Highlights:

- Grand Prix: all seven courses back to back against five CPU rivals, 10/8/6/4/2/1 points a race and the standings in between; Single Race on any course, with race again or a new track from the results; Time Trial; all at 3, 5 or 10 laps and 50/100/150cc
- Two-player split screen in every mode (Versus instead of Time Trial), each player picking their own kart
- Online multiplayer over a Broadband Adapter, a WIZnet W5500 serial-port adapter or dial-up modem, with lobbies, points and a spectator TV director
- CPU rivals with their own driving styles that drift, mini-turbo and rubber-band mildly
- Chaos Emerald power-ups: Power Sneakers, a homing Chaos Spear, a Mine dropped behind you and a Barrier, offline and online
- Rocket starts, charge jumps, burnouts, emotes and final-lap dolphins
- Four original songs, 32 effects, Chao voices by colour and Jump Pack rumble
- A Settings menu with the sound test, your online name, a music switch and a Customize screen: paint your own kart (body, trim and Chao from a 16-colour palette), saved to the VMU and raced offline or online as CUSTOM
- VMU screens for the countdown, place, lap and results

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamkarts.cdi)**
for Flycast or GDEMU. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamkarts.elf) is also available.
If you are burning a physical CD-R, you may want to try the padded
[CD-R image](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamkarts-cdr.zip) instead (unzip before burning).

## Controls

| Dreamcast controller | Flycast keyboard default | Action |
| --- | --- | --- |
| Analog stick or D-pad | I/J/K/L or arrow keys | Steer (menus: move) |
| A | X | Accelerate (menus: OK) |
| B | C | Brake, even while holding A; reverse with A released (menus: back) |
| R trigger (or L trigger, or X) | V (or F, or S) | Hop and drift; hold without steering to charge a jump |
| Y | D | Use the held power-up (with down held, throw a Chaos Spear backwards; a Mine always drops behind); wave when holding nothing, arms taking turns |
| Both triggers together | F and V together | Look behind while held |
| Start | Return | Pause (continue / restart / music on-off / performance overlay on-off / quit) |

Player 2 uses the second controller with the same layout; a Jump Pack in
either slot vibrates for that player. To drift, hold A, press and hold R as
you turn in, push the stick inward to tighten or outward to widen, and
release when the sparks are orange or purple.

## Power-ups

Chaos Emeralds hover over the road at two to four bands a lap, each in one
of four slots across the road, picked per race from a seed every console
shares, so the inside or outside slot costs you a tenth or two. Drive
through one and your Chao keeps it: a gem trails your kart and a small one
sits beside the lap counter with a Y. A taken emerald bursts and pops
straight back in a new colour, so a whole pack through the band each gets
one; a kart already holding something drives past.

| Emerald | Item | On Y |
| --- | --- | --- |
| Blue | Power Sneakers | A boost like a purple mini-turbo, 2.1 s in first place to 3.1 s in sixth, and half as long again when you are last; cuts through sand like any boost |
| Red | Chaos Spear | A homing shot that flies along the track at the next kart ahead within 60 m (behind you with down held), spinning out whoever it hits for a second; it follows bends and jumps, and a late swerve can dodge it |
| Yellow | Mine | Dropped on the road behind you, where it waits 25 s for anyone (you included, after a moment) to drive over it and spin out; a charge jump clears it |
| Green | Barrier | A bubble for six seconds that soaks up one Spear or Mine |

CPU rivals take emeralds on their line and use them after a moment's
thought, steer round three mines in four, and leave the leading human alone
on their final lap. The race setup menu's ITEMS switch turns them off; a
solo time trial never has them, so lap times stay honest. Online the lobby
leader has the same switch, and a Spear's or Mine's hit is judged only by
the console driving the kart it hits (see
[server/PROTOCOL.md](server/PROTOCOL.md)).

## Build and run

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make -C projects/games/dreamkarts
projects/games/dreamkarts/run-flycast.sh      # builds first unless --skip-build
```

Every build fast-forwards the ignored `third_party/sh4zam` checkout (see the
repository README); asset generators and QA scripts run through `uv run`.
Before a hardware build, apply
[`patches/kos-dhcp-fast-retry.patch`](patches/kos-dhcp-fast-retry.patch) to
KOS and rebuild it (`make -C $KOS_BASE/kernel`), as the release workflow
does. Debug info is split into `dreamkarts.elf.debug` for `sh-elf-addr2line`.
The title screen shows the commit the game was built from (bottom right,
`-dirty` when built with uncommitted changes); please include it in bug
reports. `DK_GIT_SHA=...` on the make line overrides it.

Launcher environment: `DK_ELF` picks another ELF, `DK_EXIT_ON_LOG` and
`DK_FAIL_ON_LOG` stop Flycast on a serial line, `DK_NET=modem` swaps the
emulated broadband adapter for the emulated modem, `DK_SHOW_VMU=1` shows the
VMU screens over the game, `DK_KEYBOARD=1` puts a Dreamcast keyboard in port
B fed by the Mac's keyboard (for the lobby chat), `DK_VMU_DIR=<folder>` swaps
in that folder's memory cards (every slot's file must already exist).

## Online play

Pick ONLINE MULTIPLAYER, type a name (up to 10 of `A-Z 0-9 - .`) and press
Start. A Broadband, LAN or WIZnet W5500 (serial port) adapter is used if one
is plugged in (DHCP);
otherwise the modem dials with the saved PlanetWeb or DreamPassport settings,
falling back to `555` with login `dream` / `cast`, which a
[DreamPi](https://github.com/Kazade/dreampi) accepts. The name is saved to
the first VMU found, alongside the music switch and the custom kart (the
Settings menu edits all three).

- Ten lobbies of six at `dcvmu.com`, UDP 7535. The first player in leads: picks the track, laps, class and whether power-ups are on, and starts the race, alone if nobody else has turned up yet (a bell rings when someone joins; they race from the next one)
- Start mid-race offers to drop out: that goes back to the lobby room, not the browser, so a practice lap ends the moment company arrives
- Each console drives its own kart; the server starts the countdown and decides places. Points (10/8/6/4/2/1) add up while you stay in the lobby
- X in the lobby room opens the chat: type on the on-screen keys (quick phrases included) or a Dreamcast keyboard in any port, Start or Enter sends. Closed, a strip shows the newest line and a badge counts what you have not read. Chat is per lobby, watchers can read it, and the server stars out bad words
- Lobby 10 is a demo lobby of bots, watch only
- An old console is told a newer version is out, or refused if too old to play

The Go server, its test bot, the local test recipe and the checklist for any
change to online play are in [server/README.md](server/README.md); the wire
format is in [server/PROTOCOL.md](server/PROTOCOL.md). A race uses about
1 KB/s up and under 3 KB/s down on a 56k modem.

## Assets

Everything is generated and the outputs are committed, so a plain build
needs no Python.

| Target | Regenerates |
| --- | --- |
| `make textures` | Procedural textures, the HUD font, logo and countdown lettering (`assets/generated/textures.pak`) |
| `make models` | The kart and Chao meshes with LODs (`models.h`); preview with `tools/preview_models_blender.py` |
| `make canyon-rock` | Spiral Canyon's rock (`canyon_rock.h`): a `-DDK_DUMP_ROCK` build prints the rock's height field, tunnel bores and road clearance, and Blender (`tools/build_canyon_rock.py`, ~5 s) cuts one mesh from them; open `captures/canyon_rock.blend` to look at it. Rerun after changing the track's layout, heights or the rock's shape |
| `make audio` | Songs and effects, encoded to AICA ADPCM (`sfx.pak`, `music.adpcm`) |
| `make vmu` | The save file's icon and the VMU screen sprites and fonts |
| `make assets` | All of the above |

## Tests and captures

| Command | What it checks |
| --- | --- |
| `make autotest` | A CPU drives a full 150cc Grand Prix in Flycast; logs frame rate, CPU time and physics stepping every 2 s, every emerald taken, spear thrown, mine dropped and hit as `DK ITEM` lines (and `misses band` for a kart that crossed a band off the slot), prints a summary and exits on `DK AUTOTEST: PASS` (fails on time, a gap rescue, a missed jump, or tyres drawn 2 cm or more into the road in over 10% of kart-frames; the `tyres vs drawn road` line has the depths) |
| `make VARIANT=capture dreamkarts-capture.elf` then `uv run tools/capture.py --out captures` | Scripted title, menus, race and results, screenshotting the Flycast window at each `DK SHOT` (an emerald ahead, the held gem, a spear, a barrier, a spin-out and a mine ahead included); `--log FILE` keeps the whole serial console |
| `make hwbench` | The same scripted race on the real console through `tools/dc-hw.sh` (ask before touching it): 10 s timing lines, `DK GPU` / `DK CPU` feature probes, `DK PC` performance counters, screenshots in `captures/hw/`; powers the console off after. Split screen: `make hwbench EXTRA_CFLAGS=-DDK_AT_2P HW_LOG=captures/hw/hwbench2p.log` |
| `make hw` | Plays the release build on the console and leaves it on (`tools/dc-hw.sh power off` afterwards) |
| `make -C tools/microbench run` | Cycles per vertex or triangle for each `src/xform.S` routine on the console, about a minute |
| `go test ./...` in `server/` | Server, protocol and version-header agreement; `go test ./demo` loads every demo recording |
| `make online-qa` | Every online scenario against a local lobby server, each in its own Flycast with the bots it needs: race and chat, custom karts, practice, spectating, a server on a newer protocol (the too-old screen must come up) and a server restart mid-race (the console must get into the next race); `DK_QA_LIVE=1` runs the first four against dcvmu.com, `QA="race restart"` picks scenarios, logs in `captures/online-qa/` |

`EXTRA_CFLAGS` for the autotest, capture and hwbench builds:

| Flag | Effect |
| --- | --- |
| `-DDK_AT_TRACK=n` | Track 0-6 (Beach, Coral, Harbour, Temple, Skyway, Cyberstone, Spiral Canyon) |
| `-DDK_AT_CC=0/1/2` | 50/100/150cc (the time limit scales) |
| `-DDK_AT_TT`, `-DDK_AT_2P`, `-DDK_AT_LAPS=n` | Time Trial, split screen with both humans CPU-driven (plus TT = Versus), lap count |
| `-DDK_AT_GP` (`-DDK_AT_GP_RACES=n`) | A whole Grand Prix instead of one race: a summary per race, then the final standings with a points audit; `n` races instead of all seven |
| `-DDK_AT_NEXT_TRACK` (capture) | Picks NEW TRACK from the results, the next course along, and races that too |
| `-DDK_AT_NO_ITEMS`, `-DDK_AT_SOLO`, `-DDK_AT_SHIELD_TEST`, `-DDK_AT_MINE_TEST` | Race without power-ups; race the humans alone, for clean captures of the track; hand the player a Barrier every 12 s, to measure its fill; hand the player a Mine every 12 s, or every `=<s>` (the capture looks behind at it; alone with `=30` the player laps round to its own) |
| `-DDK_AT_WRONG_WAY` | Turns the player round 10 s in (player 2 at 12 s) and drives it back up the track for 5 s; `DK WRONG WAY` lines log the banner coming and going (capture shots `wrong-way`, `wrong-way-p2`) |
| `-DDK_AT_FIXED` | One 1/60 s step a frame and held screenshot frames: two runs give pixel-identical captures and `DK STATE` fingerprints, for before/after diffs |
| `-DDK_XF_CHECK` (with `-DDK_AT_FIXED`) | Runs the C beside each `src/xform.S` routine; `DK XFCHECK ... mismatch=` must stay 0 |
| `-DDK_AT_ONLINE -DDK_NET_TEST_HOST=<ip>` | Goes online: joins lobby 9, chats with the bot (a line from a faked keyboard, one from the on-screen keys, the bot's second line starred by the word filter), races, passes when the results arrive (recipe in server/README.md) |
| `-DDK_AT_ONLINE -DDK_AT_PRACTICE -DDK_AT_LOBBY=<n>` | Online practice: starts a race alone in an empty lobby, drops out, leaves and rejoins the lobby and gets into its next race, hears the bell as a bot joins mid-race, drops out back to the lobby room, races the bot, sees the bot's kart leave when it drops out (`dk-bot -retire`), drops out again (recipe in server/README.md) |
| `-DDK_AT_ONLINE -DDK_AT_SPECTATE` | Watches the demo lobby through every camera |
| `-DDK_AT_ONLINE -DDK_AT_REJECT` | Against a server started with `-reject-proto 9`: passes on THIS VERSION IS TOO OLD with the download prompt, fails on anything else (builds up to 17 showed LOST CONNECTION after a protocol bump) |
| `-DDK_AT_ONLINE -DDK_AT_RESTART -DDK_AT_LOBBY=<n>` | Starts a race alone, prints `restart the server now`, and once the harness has restarted the server with the same `-race-seed` must come back through the browser, rejoin and get into the next race, whose id repeats (builds up to 18 sat in the room showing RACE IN PROGRESS) |
| `-DDK_AT_LIVE` | With the online autotests against dcvmu.com: the chat check no longer expects the bot's second line starred (the live word list has no BLORP) |
| `-DDK_AT_NAME=QA1 -DDK_AT_NAME_WANT=QA0` (`-DDK_AT_NAME_CORRUPT`) | Saved-name round trip on the VMU: the name screen must open on the saved name (and recover from a junk file); writes Flycast's real A1 card, delete its `DREAMKARTS` file afterwards |
| `-DDK_AT_SETTINGS` (capture), then `-DDK_AT_SETTINGS_WANT` | Walks the Settings menu: music off, a custom kart painted and saved, CUSTOM on the race setup's colour row, the online name saved; the second build checks a reboot loads them all. Run both with `DK_VMU_DIR=<folder of blank cards>` so the real VMU is left alone |
| `-DDK_AT_ONLINE -DDK_AT_CUSTOM` | Goes online with a custom kart; `DK ONLINE: kart` lines show every racer's colour and picks as the lobby sent them (pair with `dk-bot -colour 8 -custom 2,3,4`, the rest as in server/README.md) |
| `-DDK_AT_CAMTOUR` (capture) | Screenshots every spectator camera on every track to `captures/tv` |
| `-DDK_AT_VIEWS` (capture) | Spiral Canyon's rock: `=1` the rock from outside (both mountains from above and afar, the tunnel mouths from overhead, the waterfalls), `=2` the chase camera every 8 m round the lap with a look left and right every 16 m (`sweep-NNN-f/l/r`, the check that matches what a player sees), `=3` the leak test: cameras inside the rock, each shot twice, the real world and the rock's height function drawn on its own. Add `-DDK_ROCK_TINT` to colour the rock red and everything else grey with the sky black; the leak test needs it, and `uv run --with pillow --with numpy tools/leakcheck.py captures/<dir> --ref <any normal shot>` then lists every shot where the baked rock falls short of the height function, with a sheet of the worst. `-DDK_LEAK_FROM=a -DDK_LEAK_TO=b` restricts the leak test to a stretch of the lap, `-DDK_SWEEP_FROM=a -DDK_SWEEP_TO=b` the sweep |
| `-DDK_AT_VIEWS=2` (hwbench) | The sweep on the console: no screenshots, one `DK HWVIEW` line per point with its PVR render and render CPU time (average/max) and triangles, forward only; run with `tools/dc-hw.sh run ... dreamkarts-hwbench.elf` as `make hwbench` does |
| `-DDK_CHUNK_STATS` | Spiral Canyon: strips and vertices the world sends a frame, by kind (ground, deck, rock, props, torches), every 2 s (`DK CHUNKS`); at 0.41 us a vertex and 1.6 us a strip on the console |
| `-DDK_PERF_HUD` | Starts with the performance overlay on (the pause menu's PERF switch): frame CPU time and its phases, PVR render time, vertex buffer, triangles and missed vblanks, half-second averages and maxima against the 16.68 ms frame. It costs ~0.3 ms of CPU a frame on the console. In Flycast the PVR row is a constant 7.50 ms and VTX reads `--`, so judge headroom on hardware. hwbench adds a `-perfhud` probe step |
| `-DDK_AT_PVRSPIKE=15.0f` | Logs where any race frame's PVR render runs over that many ms (`DK PVRSPIKE`, the player's lap distance and heading), at most one line in 2.5 s |
| `-DDK_AT_WALLRIDE`, `-DDK_AT_ZIGZAG` | The player rides the right rail all of lap 1 and the left all of lap 2, or slams the rails at an angle every 1.5 s; any kart ending up past a rail logs `DK ESCAPE` |
| `-DDK_AT_FALL` | Steers off Neon Skyway's open edge on lap 1 to count falls (fails on time by design) |
| `-DDK_AI_TRACE`, `-DDK_HOP_TRACE`, `-DDK_AIR_TRACE`, `-DDK_VOICE_TRACE`, `-DDK_RUMBLE_TRACE`, `-DDK_NET_TRACE`, `-DDK_PATH_LOG` | Per-event logs: CPU drift decisions, short hops (a stream of them on hills means karts stuttering over the road), airtime, voice lines, rumble levels, network start-up timing (never on hardware builds), the kart path `dk-bot -path` replays |

Other builds: `VARIANT=record` with `-DDK_RECORD` records the server's demo
races (see [server/README.md](server/README.md#demo-lobby)); `VARIANT=film`
with `-DDK_FILM_SPECTATE` or `-DDK_FILM_SHOW` plays trailer footage with
`DK FILM` highlight lines, recorded through OBS and cut by `tools/film/`
(`-DDK_FILM_NO_MUSIC` for effects only, `-DDK_FILM_CLEAN` for no HUD). To film
chosen tracks, point it at a local lobby server (`-DDK_NET_TEST_HOST=<Mac IP>`)
whose demo races are limited to those tracks.

## Performance

Real console, 2026-10-19, 150cc, six karts, a full race, before and after
the SH-4 assembly vertex loops and the other savings below. CPU is the
game's time a frame (the display's frame is 16.68 ms); the worst window is
the highest 10 s average in the race; dropped vblanks count the whole run
and now fall almost entirely in the menus while screenshots are written.

| | CPU avg | Worst frame | Worst 10 s window | Dropped vblanks |
| --- | --- | --- | --- | --- |
| Emerald Beach 1P | 11.8 → 8.7 ms | 17.7 → 14.2 ms | 13.9 → 10.1 ms | 5 → 4 of ~5,800 |
| Coral Crossing 1P | 12.2 → 8.5 ms | 22.6 → 14.1 ms | 13.6 → 10.1 ms | 6 → 3 of ~6,300 |
| Palm Harbour 1P | 9.7 → 7.6 ms | 16.0 → 12.5 ms | 11.5 → 8.4 ms | 2 → 4 of ~6,400 |
| Temple Hill 1P | 11.7 → 8.3 ms | 19.7 → 13.7 ms | 13.5 → 9.9 ms | 5 → 4 of ~7,050 |
| Neon Skyway 1P | 11.3 → 8.1 ms | 21.0 → 18.0 ms | 13.6 → 10.2 ms | 3 → 4 of ~8,000 |
| Cyberstone 1P | 8.7 → 6.9 ms | 13.5 → 11.4 ms | 10.6 → 7.9 ms | 3 → 4 of ~8,250 |
| Emerald Beach 2P split | 13.5 → 9.8 ms | 20.8 → 15.5 ms | 15.2 → 10.9 ms | 58 → 5 of ~5,750 |
| Temple Hill 2P split | 12.9 → 9.5 ms | 22.6 → 16.8 ms | 15.4 → 11.6 ms | 60 → 3 of ~7,000 |

Spiral Canyon on the console (2026-10-21, `make hwbench`, 150cc, six
karts), before and after cutting its world draw (better strips for the
baked rock, a coarser rock and road copy beyond 25 m in a split screen):

| | CPU avg | Worst frame | Dropped race frames |
| --- | --- | --- | --- |
| Spiral Canyon 1P | 10.5 → 9.2 ms | 23.5 → 24.5 ms | 6 → 1 of ~7,500 |
| Spiral Canyon 2P split | 13.3 → 11.2 ms | 47.6 → 20.0 ms | 197 → 4 of ~7,500 |

With the totem poles, bushes and golden-hour sunset (2026-10-22): 1P 9.9 ms
and 3 dropped race frames, 2P 11.5 ms and 7, most on lap 1 with the pack
bunched. The sun and its glow cost ~1 ms of PVR render. Props need to be
joined strips: drawn as separate pieces the totems and bushes cost ~0.4 ms a
frame in split screen and took the 2P drops from 4 to 19. The scenery alone
peaks at 10.8 ms of PVR render (the run down to the start line, both
mountains in view; `-DDK_AT_VIEWS=2` on the console), so the rare render
spikes over 16.7 ms in a race come from karts and their effects.

The PowerVR's render time peaks at 14 to 16 ms in the heaviest scenes, so
the GPU is now as close to the limit as the CPU. Baseline, 2026-10-17: the
first hardware build dropped 208 vblanks in a 1P race and 1,142 in 2P,
mostly to the translucency autosort. Flycast averages about 6 ms of CPU in
the 1P race, so the console is about 1.4x slower for this code; `make
hwbench` is the measurement that counts.

Power-ups (2026-10-19, after the assembly loops): the same 150cc Emerald
Beach race with emeralds, spears and barriers in play dropped 7 of 5,717
frames (one in the race, at a 16 ms render) at 7.7 ms average CPU and a
16.0 ms worst frame, within the table above. With `-DDK_AT_SHIELD_TEST`
handing the player a Barrier every 12 s (36 of the race's 69 s under one),
the PVR stayed at 11 to 13.5 ms and no race frame dropped. The GPU probe
puts the items together with the particles at 0.1 to 0.7 ms.

What each change saved on the console (ms a frame unless stated):

| Change | Saving | Lesson |
| --- | --- | --- |
| Translucency presorted, PVR autosort off (`src/render.c`) | ~7 | Autosort peels layers per tile; submit back to front instead |
| Vertex loops in SH-4 assembly (`src/xform.S`) | ~1.5 | 111 → 88 cycles a world vertex, 88 → 57 a kart vertex, 111 → 86 a kart triangle. The CPU, not the Tile Accelerator, was the limit |
| Bounding spheres on world strips (`src/render.c`) | 0.6-0.9 | Whole strips culled or sent without outcodes; only edge-crossers tested per vertex |
| Kart lighting once per normal/paint group (`src/models.c`) | 0.7-0.8 | 950 groups cover 1,614 vertices |
| Sea and cloud-sea colours cached per camera cell (`src/world.c`) | 0.3-0.55 | |
| Kart triangles skip outcodes when the mesh is wholly on screen | 0.2-0.45 | |
| Kart meshes as strips over each mesh's distinct positions (`src/models.c`, `src/xform.S`) | 0.1-0.4 CPU, 0.05-0.25 render | 959 of 1,614 kart vertices differ only in normal or paint, so only those positions are transformed; strips alone saved render time, not CPU |
| Final-lap music speed-up retuned in place (`src/audio.c`) | 16.5 ms spike → 0.06 ms | Restarting the stream refilled its whole 32 KiB AICA buffer in one frame and dropped a vblank |
| Physics step = measured vblank period (`src/main.c`) | one-frame hitch every 4-16 s gone | A fixed 1/60 s step owed the 16.68 ms display a double step; 18 double and 2 zero-step frames of 5,269 → 0 |
| Split views on whole 32-pixel tile rows with the PVR user tile clip | | Software clipping at a 240-pixel split broke most ground strips into triangles |
| Kart LODs with hysteresis (`src/models.c`); far model for split-screen rivals | ~140 → 10 dropped frames a split race | |
| `-ffast-math` (`Makefile`) | | Without it `sinf`/`cosf` stay libm calls despite `-mfsca` |
| Kart matrices and lighting shared by both split views; less split-screen scenery; near smoke puffs skipped and capped; "GO!" glow removed | | Translucent fill is the console's bottleneck |

Measured and not worth the code: TA DMA (the CPU waits on the TA only
~0.1 ms a frame), software-pipelining the assembly loops (under 0.1 ms),
`pref` at any distance (streaming vertices from RAM costs ~20 cycles a
32-byte line regardless), staging-buffer placement (nothing), lighting or
the 4x4 matrix product in assembly (~0.1 ms), letting the PVR cull kart
back faces (0.3-0.4 ms less CPU, 0.1-0.5 ms more render time).

## Gotchas

- An ELF over 16 MB will not boot in Flycast; the link strips debug info into `dreamkarts.elf.debug` (`Makefile`)
- Stock KOS resends a lost first DHCP DISCOVER after 4 s, so online took 7-12 s to connect on the console; `patches/kos-dhcp-fast-retry.patch` backs off from 1 s (4-5 s now)
- Under dcload-ip every printf runs with IRQs off and can stall or freeze the console: hardware builds log every 10 s and keep `-DDK_NET_TRACE` off (`src/main.c`, `src/netlink.c`)
- Change a vertex loop in its C reference first, then `src/xform.S` to match, and keep `-DDK_XF_CHECK` at 0 mismatches
- Looping voices (engine, squeal, crackle) need channels reserved with `snd_sfx_chn_alloc()`, or one-shots steal them and pitch updates retune the wrong sound (`src/audio.c`)
- Flycast's DCNet dial-up does not complete the game's PPP login; test the modem path with `DK_NET=modem`
- Flycast does not emulate the W5500, and probing for one there spins on SCI timeouts for ~0.9 s. It runs once, on the link thread while the player types a name, so nobody waits on it
- Online, two level karts each see the other ~150 ms behind, so a power-up hit is only ever judged by the console driving the kart hit; a spear spawned from a packet is moved on by the draw delay, and a mine from a packet catches a kart that crossed its spot in that delay (`src/items.c`)
- One emerald per band starves a tight pack if it takes any time to come back: six CPUs cross a band within a second, so it pops back at once
- Anything translucent drawn round your own kart fills most of the chase view: the Barrier's first bubble (three 2.5-2.9 m billboards) pushed the PVR to 20 ms and dropped ~70 frames a race; it is a 1.3 m core and ten small orbiting sparks now (`src/items.c`)

## Known limitations

- Online races have no CPU karts
- A server restart, including every deploy, empties the lobbies and sends everyone back to the lobby browser
- Dial-up needs a PPP server that accepts the saved ISP settings or `555` with `dream` / `cast`; dialling can take up to a minute

## Credits

Built on KallistiOS and SH4ZAM; the font, tools and thanks are in
[CREDITS.md](CREDITS.md), with the licence notices binary releases carry.
