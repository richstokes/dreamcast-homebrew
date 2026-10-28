# Dreamcast Development Workspace

This workspace is for Sega Dreamcast homebrew built with KallistiOS (KOS).
Put games in `projects/games/<name>/`, apps and demos in
`projects/apps/<name>/`, and DCVMU in `projects/dcvmu.com/{client,service}/`. Treat `BIOS/`
and `GAMES/` as user-owned runtime data: do not modify, move, or commit their
contents unless the user explicitly asks.

DCVMU's shared production host is managed by
[infra/servers/netsplit.vip](https://github.com/richstokes/infra/tree/main/servers/netsplit.vip)
(normally the sibling `../infra` checkout). Caddy, shared ports and application
registration belong there. Keep DCVMU deployment, its base service unit and
database backups in this repository; preserve shared host configuration.
Use `uv` for Python package management and virtual environments; DCVMU keeps
its dependency versions in `projects/dcvmu.com/service/requirements.txt`.

DreamKarts' online lobby server (`projects/games/dreamkarts/server/`, Go, UDP
7535 on the same host, infra app id `dreamkarts`) must match the consoles:
`projects/games/dreamkarts/src/net_version.h` holds the protocol, build and
minimum build both sides use. When a change touches online play (that header,
`src/net.c`, `src/online.c`, kart state sent online, the track list, or
anything under `server/`), bump `DK_NET_BUILD` (and `DK_NET_MIN_BUILD` if old
consoles would desync), update `server/lobby/proto.go` to match (its tests
check), and redeploy with `projects/games/dreamkarts/server/deploy/deploy.sh`,
which ends by checking the live server reports the new build. See
`projects/games/dreamkarts/server/README.md`.

## Worktrees and branches

Finished work belongs on `main`. Sessions often run in a git worktree under
`.claude/worktrees/` on their own branch; when the work there is done, commit
it and push it to `origin/main` (rebase onto `origin/main`, then
`git push origin HEAD:main`), including work a side session left on its own
branch. Do not leave finished changes only in a worktree, a local branch or a
stash. Before calling a task done, run `tools/check-worktrees.sh`: it fetches
origin and lists any worktree with uncommitted files or commits not on
`origin/main`, any branch with unmerged commits, and any stash, and exits 1
if anything has not reached `main`. Work another session is still doing will
show up there too; leave that to its session rather than merging it.

## Real Dreamcast hardware

`tools/dc-hw.sh` runs programs on the physical console (power through a
Shelly smart plug, dcload-ip over the network). Ask the user in that turn
whether the console is free before anything that powers, power-cycles,
resets or uploads to it, and check `ps aux | grep dc-hw` for another session
holding it. The plug is opt-in and remembered in `~/.cache/dc-hw`; if
`dc-hw.sh status` reports no plug, ask the user rather than running
`--use-shelly` yourself.

Under dcload-ip every `printf` is a network round trip made with interrupts
off, and a lost packet freezes the console. Hardware builds log a line every
few seconds at most, never per frame, per move or per packet. `INIT_NET` can
hang at boot under dcload, which owns the adapter, so hardware builds skip
KOS networking unless the test needs it (DreamKarts' online hardware test
shows how). Never detach dc-tool-ip mid-run.

When you are done with an explicit hardware test, switch the console off:
pass `-o`/`--off` to `dc-hw.sh run` for scripted tests and benchmarks (the
`hwbench`/`hwsoak` Makefile targets already do), or run
`tools/dc-hw.sh power off` afterwards, and confirm with `tools/dc-hw.sh
status` that the plug reads off. Only leave it on when the user is going to
play on it (`make hw`).

## Installed toolchain

- KallistiOS: `/Users/rich/.local/share/dreamcast/kos`
- KOS environment: `/Users/rich/.local/share/dreamcast/kos/environ.sh`
- SH-4 compiler: `/Users/rich/.local/share/dreamcast/sh-elf`
- ARM7 compiler: `/Users/rich/.local/share/dreamcast/arm-eabi`
- kos-ports: `/Users/rich/.local/share/dreamcast/kos-ports`
- Flycast: `~/.local/share/dreamcast/flycast/Flycast.app`, built by
  `tools/flycast/build-macos.sh`. The launchers fall back to
  `/Applications/Flycast.app`, and `FLYCAST_BIN` overrides both
- Known working baseline: KOS 2.3.0 master `d81aa3f` (see `tools/kos/README.md`
  for moving it), GCC 15.2.0, Flycast master `628bd3d`
  with the patches in `tools/flycast/`
- KOS patches: `tools/kos/patches/`, applied to the installed SDK and by
  CI. `tools/kos/apply-patches.sh --check` confirms the SDK has them; a new
  SDK fix goes there, not in a project folder
- Other KOS versions: `tools/kos/kos-at.sh [--patched] REF [-- COMMAND]`
  builds any upstream commit, branch or `pr/<n>` beside the installed SDK
  and runs a command against it (KOS_Test: `make auto KOS=<ref>`)

Stock Flycast 2.7 stalls uploads through its TCP proxy, drops short keyboard
presses that land between guest polls, and crashes or panics the guest under
steady BBA traffic. Anything that tests networking or a Dreamcast keyboard in
Flycast needs the local build; `tools/flycast/README.md` has the details.

Use the checked-out SDK headers, examples, and source as the primary API
reference. Useful locations:

```text
$KOS_BASE/include/                  Public KOS headers
$KOS_BASE/examples/dreamcast/      Working Dreamcast examples
$KOS_BASE/kernel/                  KOS implementation details
$KOS_PORTS/                        Optional third-party libraries
```

The official documentation is at <https://kos-docs.dreamcast.wiki/>. Search
the local SDK with `rg` before guessing an API or copying old Dreamcast code
from the web.

## Starting a project

`projects/apps/ping-cube/` is the known-good reference: the standard KOS
Makefile layout, a `run-flycast.sh` launcher that sources KOS, builds and
boots the ELF, BBA and modem bring-up with `net_default_dev` inspection,
BIOS-font text on a PowerVR texture, controller input and clean shutdown.
Copy its Makefile and launcher, rename the target and sources, and remove
the rendering and network code you do not need. Do not modify Ping Cube
merely to use it as a template.

Code more than one project needs lives in `projects/common/`: the soft
reset, modem dial-up, VMU saves and HTTPS so far (its README lists them). Use it
instead of copying code from another project, and move code there when a
second project wants it. Its headers must stay valid C++.

Always build through KOS's `Makefile.rules` with `kos-cc` (or `kos-c++` for
`.cpp` sources); never call the host compiler. Add libraries after the
objects, for example `kos-cc -o $@ $(OBJS) -lm`. Prefer KOS APIs over host
POSIX assumptions; this is a freestanding SH-4 target, not a macOS
executable. Use `KOS_INIT_FLAGS(INIT_DEFAULT)`, or `INIT_DEFAULT | INIT_NET`
when networking is required: KOS then initializes the available network
device, runs DHCP, and handles ARP and ICMP echo replies. Check
`net_default_dev` before using its address or other interface fields.

Projects that use SH4ZAM build against an ignored `third_party/sh4zam`
clone held at the commit pinned in `tools/update-sh4zam.sh` (`tools/sh4zam.mk`
checks it out before each build). It is never vendored. The weekly SH4ZAM
canary (`.github/workflows/sh4zam-canary.yml`) fails once upstream `master`
moves past the pin, listing what changed and building every project against
it. Move the pin only when the user asks, and test first: build each SH4ZAM
project against both commits (`SH4ZAM_COMMIT=<commit> make` picks another
one) and diff the disassembly function by function, look for SH4ZAM
deprecation warnings, run the Flycast suites, and A/B on the console whatever
code actually changed.

## Building

KOS environment scripts expect some optional variables to be unset, so scripts
that use `set -u` must temporarily disable nounset while sourcing the SDK:

```sh
set +u
source /Users/rich/.local/share/dreamcast/kos/environ.sh
set -u
make -C /absolute/path/to/project
```

For an interactive shell without nounset enabled, this is sufficient:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make -C projects/apps/example
```

Keep generated `.o`, `.elf`, `.bin`, disc images, and other build products
out of source patches unless the user asks to retain an artifact.

After building, perform at least these checks:

```sh
file example.elf
sh-elf-readelf -h example.elf
```

The result must be a 32-bit little-endian Renesas SH ELF. Use `sh-elf-nm` when
confirming that a KOS subsystem or symbol was linked.

## Running in Flycast

Flycast can boot a KOS ELF directly. A reusable launcher should resolve its own
project directory, source KOS as shown above, optionally build, and then run:

```sh
/Applications/Flycast.app/Contents/MacOS/Flycast \
  -config "network:EmulateBBA=yes,network:DCNet=no,config:Debug.SerialConsoleEnabled=yes" \
  /absolute/path/to/example.elf
```

`Debug.SerialConsoleEnabled=yes` sends `printf` and KOS diagnostics to the
launching terminal. Use absolute ELF paths so launching is independent of the
caller's working directory.

It is fine to run up to ten Flycast instances at once when that helps, for
example to test several builds, tracks or online clients in parallel. Close
only the instances you started, never the user's own.

For non-networked programs, BBA configuration may be omitted. For networked
programs:

- `network:DCNet=no` selects Flycast's local picoTCP proxy. DHCP normally gives
  the guest `192.168.169.2`; outbound TCP/UDP works.
- `network:DCNet=yes` selects the DCNet VPN and commonly gives a `172.20.4.x`
  address. Use this only when DCNet behavior is specifically needed.
- Stock Flycast 2.7 does not provide true TAP/pcap/vmnet LAN bridging. Neither
  private address is directly pingable from the Mac or another LAN device.
- KOS does reply to ICMP echo requests, but Flycast's proxy/VPN boundary keeps
  ordinary LAN pings from reaching it.
- A Dreamcast program acting as a TCP/UDP server needs a Flycast inbound port
  forward, a custom bridged backend, or real BBA hardware. A Dreamcast client
  connecting outward to a server on the Mac works with the picoTCP backend.

Never pass `config:Dreamcast.VMUPath=<folder>` unless that folder already holds
a `vmu_save_<slot>.bin` for every slot (A1 to D2; zero-filled 131072-byte files
are formatted on load). Flycast otherwise moves the user's real card into the
folder and deletes the original. `projects/dcvmu.com/client/tests/isolated_cards.py`
creates the blank set.

Do not describe `DCNet=no` as bridged mode. Do not make persistent Flycast or
macOS network changes when transient `-config` flags are sufficient.

## Dreamcast-specific notes

- The retail display target is 640x480. Initialize video/PVR before
  rendering and keep the main loop responsive to the controller.
- The BIOS font (`<dc/biosfont.h>`) is ISO-8859-1, not UTF-8; encode
  non-ASCII glyphs accordingly (copyright is `\xa9`).
- PowerVR textures come from `pvr_mem_malloc` and go back with
  `pvr_mem_free`. CPU-side buffers used by transfer routines need the
  required alignment.
- Handle a missing controller and a missing network device, and print useful
  diagnostics: the Flycast serial console is the fastest way to diagnose a
  startup failure.
- Keep the code portable to real hardware. Emulator-only behavior must be
  documented and isolated in launch tooling.

## Documentation

READMEs are for humans with short attention spans. A project README covers
what it is, download links, controls, build and run, make-target tables for
assets and checks, a compact performance table, gotchas, known limitations
and credits, in roughly that order. Project descriptions in the root README
tables are one sentence. No implementation detail: the code is the source of
truth, so put mechanics in a code comment instead. No narration of how a
result was reached. Keep benchmark numbers and lessons learned; for any other
line, ask whether it will aid future development or stop us repeating past
effort, and cut it if not.

READMEs are written for someone who is not on this machine. The root README's
"One-time setup" covers KOS, `environ.sh`, the KOS patches, kos-ports and
Flycast; project READMEs link to it rather than repeating those steps. Any
target that touches the real console (`make hw`, `hwbench` and the like) links
to its "Real hardware" section, since `tools/dc-hw.sh` needs dcload-ip and
only switches power through a Shelly plug once `--use-shelly` names one.

## Definition of done

For each new Dreamcast application:

1. The project builds from a freshly sourced KOS environment.
2. `file` identifies the output as a valid Renesas SH ELF.
3. Flycast boots the ELF without a KOS panic or missing-resource error.
4. Core behavior is exercised in Flycast, with serial output inspected.
5. Network behavior is tested with the intended Flycast backend and its routing
   limitations are reported accurately.
6. Project-local build/run instructions are added to its README, following
   the Documentation rules above.
