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
holding it.

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
- Flycast: `/Applications/Flycast.app/Contents/MacOS/Flycast`
- Known working baseline: KOS 2.3.0, GCC 15.2.0, Flycast 2.7

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

A minimal project normally contains:

```text
projects/apps/example/
├── Makefile
├── example.c
└── run-flycast.sh
```

Use this Makefile shape for a single-file C application:

```make
TARGET = example.elf
OBJS = example.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS)

run: $(TARGET)
	./run-flycast.sh --skip-build

.PHONY: all clean run
```

For C++, use `.cpp` files and link with `kos-c++`. Add libraries after the
objects, for example `kos-cc -o $@ $(OBJS) -lm`. Prefer KOS APIs over host POSIX
assumptions; this is a freestanding SH-4 target, not a macOS executable.

A minimal source file starts like this:

```c
#include <kos.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Application loop. */
    return 0;
}
```

Use `KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET)` when networking is required. KOS
then initializes the available network device, runs DHCP, and handles ARP and
ICMP echo replies. Check `net_default_dev` before using its address or other
interface fields.

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

Always build through KOS's `Makefile.rules` and `kos-cc`/`kos-c++`; do not call
the host compiler. Keep generated `.o`, `.elf`, `.bin`, disc images, and other
build products out of source patches unless the user asks to retain an artifact.

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

## Dreamcast-specific implementation notes

- The retail display target is normally 640x480. Initialize video/PVR before
  rendering and keep the main loop responsive to the controller.
- Use `<dc/pvr.h>` for hardware-accelerated rendering and `<dc/biosfont.h>` for
  the built-in font. The BIOS font's default western encoding is ISO-8859-1,
  not UTF-8; encode non-ASCII glyphs accordingly (copyright is `\xa9`).
- PowerVR texture allocations come from `pvr_mem_malloc`; free them with
  `pvr_mem_free`. CPU-side buffers used by transfer routines should satisfy
  the required alignment.
- Use KOS timing functions such as `timer_ms_gettime64()` and SH-4 math helpers
  where appropriate.
- Handle allocation, missing controller, and missing network-device cases.
  Emit useful `printf` diagnostics because the Flycast serial console is the
  fastest way to diagnose startup failures.
- Keep application logic portable enough to run on real Dreamcast hardware.
  Emulator-only behavior must be documented and isolated in launch tooling.

## Existing reference project

`projects/apps/ping-cube/` is the known-good local example. It demonstrates:

- the standard KOS Makefile layout;
- direct ELF launching in Flycast;
- BBA initialization, DHCP, and `net_default_dev` inspection;
- BIOS-font text rendered into a PowerVR texture;
- a low-level textured 3D cube;
- controller input and clean resource shutdown.

When scaffolding a new project, copy its build and launcher structure, rename
targets and sources, and remove application-specific rendering/network code.
Do not modify Ping Cube merely to use it as a template.

## Definition of done

For each new Dreamcast application:

1. The project builds from a freshly sourced KOS environment.
2. `file` identifies the output as a valid Renesas SH ELF.
3. Flycast boots the ELF without a KOS panic or missing-resource error.
4. Core behavior is exercised in Flycast, with serial output inspected.
5. Network behavior is tested with the intended Flycast backend and its routing
   limitations are reported accurately.
6. Project-local build/run instructions are added to its README.
