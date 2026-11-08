# Ridgeline

<p align="center">
  <img src="../../../docs/screenshots/ridgeline.png" alt="A Kestrel 4x4 climbing the logging road on Cascade Ridge" width="640">
</p>

An off-road exploration game for the Sega Dreamcast. Pick a truck and climb
Cascade Ridge, a Pacific Northwest mountain in autumn, from the meadow at its
foot to the fire lookout on the summit. No race and no clock: how high you've
been, and the viewpoints you've found.

- Two trucks: CASCADE 84, an early-80s pickup (2.4 L four, no traction
  control), and KESTREL, a modern two-door 4x4 (twin-turbo V6, 35 inch tyres,
  traction control, off when the lockers are in), four liveries each
- Three ways up from the fork, meeting at a lake before the summit trail:
  the logging road (long, gentle, gravel then muddy ruts and fallen logs),
  the creek bed (water crossings, mud, wet rock) and the ridge line (steep,
  rock ledges, boulders). Each gets harder the higher it climbs
- A meadow at the bottom to play in first: a creek ford, a pond, a mud
  wallow, logs and a rock pile
- Physics: suspension per wheel with ledge-climbing tyre contact, tyres
  that slide by surface (rock, dirt, gravel, grass, mud, scree, snow, wet
  rock), water with drag and a stall if it reaches the intake, an automatic
  box, and the driveline solved per drive mode: 2WD with an open rear diff,
  4WD with the centre locked, LOCK with low range and both axles locked.
  Low range holds the automatic to the gears that redline under ~30 mph
  (2nd in the pickup, ~22 mph; 4th in the Kestrel, ~28 mph)
- 2WD is the loose one: nose-heavy trucks (the pickup ~60% front) with a
  limited-slip rear, so the tail comes round under power and a spinning
  tyre gives up its cornering grip. Feed it on the progressive throttle and
  catch it with opposite lock. The Kestrel's traction control reins it in
  (in 2WD and 4WD) until you hold D-pad left to switch it off
- A spinning tyre flings a roostertail of whatever it's in (mud clods,
  dirt, stones, snow, spray), and the rear tyres leave tread tracks in soft
  ground
- Things to find off the trail: ten viewpoints, among them a boulder
  field, a stone arch across a deep ford the old pickup can't wade, an
  abandoned truck in the woods and a camp past the lake. The logging road
  has a washout, a log bridge and a steep shortcut that skips ~480 m of it
- Best times to the summit per truck and per way up (or your own way),
  saved on the VMU and shown on the truck select. A viewpoint jump from
  the pause menu means no time for that run
- Deer in the meadows that graze, look up as you come near and bolt when
  you get close or sound the horn
- Up to three CPU trucks for company, each taking its own route
- Online: ten lobbies of four on the same mountain, by broadband adapter,
  W5500 or modem (a DreamPi or any dial-up server). Drop in and out
  whenever you like; there is no race. Everyone's name floats over their
  truck, horns carry, the lobby shares one time of day, and reaching the
  summit puts a trophy by your name for the session
- Birds: jays, juncos and ravens perched on logs, rocks, the cabin roof
  and the cairns burst away as a truck comes near (or at the horn), hawks
  circle over the meadow and the bluffs, and now and then a V of geese
  crosses the sky
- A day that goes by as you drive, an hour a minute: first light, sunrise
  mist, noon, the long afternoon shadows, sunset, dusk and a starry
  moonlit night, the hills' shadows swinging round with the sun. Pick
  when to set off on the truck select (dawn, morning, afternoon, dusk or
  night); photo mode moves the clock with the D-pad. Headlights are yours
  to switch: off, dipped or full beam (which lights the Kestrel's roof bar)
- Synthesized engine sound from four rpm points on and off load, tyres,
  wind, the forest and the creek. No music
- Sound test from the main menu: every effect by name, loops toggle on A,
  left/right changes the pitch
- Leave the title alone for ten seconds and a demo runs: four CPU trucks
  set off up the mountain and the camera cuts between them every few
  seconds, chase, helicopter and trackside shots, until a button is pressed

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ridgeline.cdi)**
for Flycast or GDEMU. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ridgeline.elf) is also available.
If you are burning a physical CD-R, you may want to try the padded
[CD-R image](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ridgeline-cdr.zip) instead (unzip before burning).

## Controls

| Input | Does |
| --- | --- |
| Analog stick | Steer |
| R / L trigger | Throttle / brake; hold L at a stop to reverse |
| D-pad left / right | Drive mode: 2WD, 4WD, LOCK (low range goes in or out only below ~8 mph); hold left 2 s to switch traction control off or on (Kestrel) |
| D-pad up | Horn |
| D-pad down | Lights: off, dipped, full beam |
| A | Handbrake |
| B | Camera: chase, far chase, bumper |
| X | Map: corner, large, off |
| Y (hold) | Recover the truck to the nearest trail |
| Start | Pause: recover, back to a viewpoint you've found (left/right picks which), FPS counter, HUD on/off, quit |
| Y (paused) | Photo mode: no HUD, the stick turns the camera round the truck, R/L zoom, D-pad left/right moves the clock, B back |
| Start (online) | The menu, over a mountain that keeps going: recover, viewpoints, leave the lobby. The lobby keeps the clock |

## Build and run

Set up KOS and Flycast as in the [root README](../../../README.md#one-time-setup).

```sh
make -C projects/games/ridgeline run
```

| Target | Does |
| --- | --- |
| `make` | Builds `ridgeline.elf` |
| `make run` | Builds and boots it in Flycast |
| `make autotest` | The CPU drives the player up the logging road with three companions, logging frame times every 10 s; passes at the summit, fails after 120 s without progress or 900 s in all |
| `make vmutest` | The best-times save on eight blank Flycast cards (never the real ones): a time and the online name save and read back, and a slower time doesn't replace it |
| `make online-test` | Online in Flycast against a server and bot on this Mac: joins lobby 9, sees the bot's truck, both claim the summit, the bot leaves and comes back. `RL_NET=modem` dials in instead, `RL_QA_LIVE=1` uses dcvmu.com ([server/README.md](server/README.md)) |
| `make hwbench` | 180 s of the same on the console, as a bench ([real hardware](../../../README.md#real-hardware)) |
| `make hwbench HW_OFF=` then `make hwreport` | When a console bench goes quiet: reads where it stopped out of the console's memory (then `tools/dc-hw.sh power off`) |
| `make fxbench` | Tyre effects on the console: 15 s of driving, then 25 s of wheelspin in the meadow mud |
| `make hw` | Plays on the console |
| `make disc` | A self-booting CDI |
| `uv run tools/capture.py --out captures` | Capture build: title, select, a scenery tour, two views through the day and the drive as screenshots (`EXTRA_CFLAGS=-DRL_AT_TOD=TOD_NIGHT` starts the drive at another hour) |
| `make -C tools/hostsim && tools/hostsim/sim route <truck 0/1> <route 1-3>` | The truck physics and CPU driver natively on the Mac: a full route in about a second |

Assets (generated output is committed):

| Target | Builds |
| --- | --- |
| `make level` | Terrain, routes, obstacles, trees and colours (`tools/build_level.py`), preview in `captures/level-preview.png` |
| `make textures` | Textures and the font (`tools/build_textures.py`) |
| `make trucks` | Truck models (`tools/build_trucks.py`), previews with `tools/preview_trucks_blender.py` |
| `make audio` | Every sound (`tools/build_audio.py`) |

## Performance

`make hwbench` on the console (2026-11-06): the CPU drives the logging road
with three CPU trucks for 180 s, 59-60 fps throughout. 61 of 10,800 frames
late, 35 of them in the first 10 s (title, menus and scenery tour).

| | Console, ms a frame | Flycast |
| --- | --- | --- |
| CPU, average / worst 10 s window / worst frame | 13.0 / 14.5 / 22.6 | 8.5 / 10.0 / 12.9 |
| Terrain | 3.1-4.4 | 2.2-2.4 |
| Trees, rocks, props | 0.7-4.0 | 1.5-2.4 |
| Trucks | 1.4-1.9 | 1.2 |
| Physics, four trucks | 1.3-2.1 | 1.0-1.7 |

| Terrain change (console, same route, 18 windows) | Terrain ms | CPU ms | Late frames |
| --- | --- | --- | --- |
| Before: separate height and colour, wider LOD rings, every skirt drawn | 4.88 | 13.56 | 78 |
| After: interleaved, tighter rings, far skirts only where grids differ | 3.85 | 13.04 | 61 |

The terrain went from ~8,900 to ~5,900 vertices a frame and from ~10,900 to
~7,100 operand-cache misses. Trees came out 0.1-0.15 ms slower in every
window without being touched: code layout drift, as DreamKarts sees between
unrelated builds. The bench logs now carry the PVR's render time too.

`make hwbench` A/B on the console (2026-11-07), before and after the set
pieces, birds, deer, best times and tower fix: no change in the drive.

| 180 s on the logging road | Before (7ffab7d) | After (b0e9190) |
| --- | --- | --- |
| fps, average | 58.9 | 58.9 |
| CPU, ms a frame | 13.18 | 13.02 |
| PVR render, ms | 10.77 | 10.86 |
| Late frames | 144 | 144 |

The bench also times each scenery-tour view; the new set pieces render
in 7.5-13.8 ms. Its occasional single-frame render spikes (35-85 ms, a
different view each run) come with the RL SHOT prints, which stall the
console under dcload.

`make fxbench` (2026-11-07), wheelspin in mud with roostertails and tracks.
The soft haze puffs flung at the camera were the cost (switching each
effect off in turn: puffs off took the render from 19.7 to 11.2 ms; tracks
off, 16.3; clods off, nothing), so puffs got a hard cap of 36, a fade near
the camera and a third of the mud haze.

| Wheelspin in mud, console | PVR render, average / worst | fps |
| --- | --- | --- |
| Before | 17-29 / 24-34 ms | 30-40 |
| After | 11-13 / 15-18 ms | 57-59 |

Tried and dropped (console, 2026-11-07): SH4ZAM's FSRRA divide on the
physics step's 24 variable-denominator divisions, and its atan2f, atanf, tanf
and asinf for the steering. Physics went 1.67 to 1.76 ms with the divides and
1.70 ms with the trig added; the FPU-stall share never moved, so FDIV latency
is not where the time goes. Fewer substeps or less work per step is the lever.

Time of day, `make hwbench` A/B on the console (2026-11-07), the bench
now running from 17:00 through sunset into the night (the stars and the
moon are drawn; the bench's lights are on two thirds of the time):

| 180 s on the logging road | Before (b0e9190) | Time of day |
| --- | --- | --- |
| fps, average | 58.9 | 59.3 |
| CPU, ms a frame | 13.02 | 14.16 |
| PVR render, ms | 10.86 | 11.15 |
| Late frames in the drive | ~110 | 56 |

`make hwbench` again on 2026-11-08 with the 3 m trail grid, the single
headlight beam and the drifting clouds: CPU 14.60 ms a frame (terrain
4.07, up 0.5 for ~500 more terrain vertices a frame; trees 2.77, trucks
1.67, relight 0.81), PVR render 11.36 ms, 59.1 fps, 89 late frames in the
drive. The late frames cluster where the logging road climbs through the
forest past the lake turn (~14,000 triangles, CPU 16-16.6 ms), the one
stretch that now sits on the 60 fps line. With `EXTRA_CFLAGS=-DENG_PROF`
the terrain and tree sections spend ~40% of their cycles stalled on
operand-cache misses (~6,700 and ~7,000 misses a frame), the trucks and
physics ~10-17%; that cache traffic, not arithmetic, is where the frame
goes.

Relighting the world as the clock runs is 0.8 ms a frame (0.6-0.9 in
Flycast): a slice of the 236,000 ground and scenery vertices each frame,
so a whole pass takes ~3.5 s while the sun moves under a degree. A pass
all at once (a clock jump in a test) is ~165 ms on the console, ~115 in
Flycast. The night costs another ~0.9 ms of TR submission, nearly all of
it the four trucks' headlights, which used to be on a third of the time.

## Gotchas

- A console run under dcload that goes quiet and stays quiet is usually
  stuck inside a print, not in the game: dcload waits for the Mac's reply
  with interrupts off and a lost packet leaves it waiting. `make hwreport`
  says which. The bench's prints a third of a second apart never stalled;
  silencing them so prints came a second apart stalled 5 runs out of 5
- Rebuild the level and then the textures: the minimap comes from the level
- A truck placed level on a steep slope starts with its body in the hill and
  is thrown off it; `truck_place` sits it on the ground under its wheels
- Route carving: two routes over the same ground at different heights
  carve a step into it. Routes from the fork avoid the forest road, meeting
  points are pinned to one height, and hairpins get a level, wider landing
- The CPU drivers are winched past spots they can't climb, which hid
  undrivable bits of route: the creek bed once ran through 6 m of lake.
  Count winches in hostsim (`sim route ... | grep -c winch`) after changing
  the level. Route search now never takes deep water, a last pass puts each
  route's band back to its height where another route's carving moved it,
  and boulders on a hairpin go to its edges. The ridge line ends on the
  logging road and shares its last stretch: it used to run beside it, 2.5-7
  m away and lower, and the two couldn't both be carved
- A* turns are capped at ~95 degrees a step so hairpins have a radius;
  tighter caps made routes loop for kilometres
- The automatic shifts on road speed and holds its gear while the tyres
  spin; on wheel speed it hunted between 1st and 2nd on loose climbs
- When a tyre breaks away, scale its sideways force with its driving
  force: scaling only the drive left a spinning tyre half its cornering
  grip, and 2WD could not slide. `tools/hostsim/sim turn<mode> <truck>`
  measures the slide angle (`SIM_THROTTLE`, `SIM_DRIFT`)
- CPU trucks get a gentle pull back toward the trail and are winched past
  anything that holds them up for 25 s; the player drives unaided
- `tools/hostsim` reads `level.h` at compile time and `level.bin` at run
  time, so rebuild it after `make level`

## Known limitations

- One scene (autumn); the desert palette is not built yet
- Headlights light the truck and a beam on the trail ahead, not the
  terrain or trees around it
- No rumble yet; the VMU keeps best times and the online name only
- Online has no chat. Trucks bump each other, but each console pushes only
  its own truck against the others as they were ~150 ms ago, so a shove
  lands a little late
- Rock-ledge climbing is approximate: the tyre is sampled at seven points,
  not swept

## Credits

Lilita One by Juan Montoreano (SIL Open Font License). Everything else
(terrain, textures, trucks and sounds) is generated by the tools here.
