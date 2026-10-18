# Native Flycast on macOS

All project launchers prefer the locally built Flycast at
`~/.local/share/dreamcast/flycast/Flycast.app`. They fall back to
`/Applications/Flycast.app`, and an explicit `FLYCAST_BIN` overrides either.
Apple Silicon runs natively; Rosetta is not needed.

To build and install:

```sh
brew install cmake pkg-config sdl2
./tools/flycast/build-macos.sh
./projects/dcvmu.com/client/run-flycast.sh
```

Xcode command-line tools are required. The build uses OpenGL and the host SDL
library. It leaves the application in `/Applications` alone. Sources and build
products stay outside this repository; only the patch and builder are tracked.
The installed app depends on the Homebrew libraries used during the build.

## Pinned revision

The build uses Flycast master at
[`628bd3d`](https://github.com/flyinghead/flycast/commit/628bd3dbb160ea2750230fc6b00c0bb8173cb1f6),
which is newer than v2.7 and includes three fixes these projects rely on:

- [#2492](https://github.com/flyinghead/flycast/pull/2492): opening a game at
  launch on macOS no longer trips the SH-4 recompiler's `sq_buffer` assertion.
- [#2500](https://github.com/flyinghead/flycast/pull/2500): that fix no longer
  crashes with Homebrew's `sdl2`, which is the sdl2-compat shim over SDL3.
- [#2497](https://github.com/flyinghead/flycast/pull/2497): uploads through the
  TCP proxy (`DCNet=no`) no longer stall and time out. Raising the client
  timeout or retrying does not help on older builds, including v2.7; rebuild
  with the command above. Real BBA hardware does not use this proxy.

Four local patches are applied by the builder:

- `macos-build.patch` turns off precompiled headers for the `flycast` target,
  which the command-line CMake build cannot share between C++ and Objective-C++.
- `keyboard-input.patch` preserves short keyboard presses until the Dreamcast
  reads them. Upstream keeps only the keys currently held, so a press and release
  arriving between guest polls disappears. The patch queues keys together with
  their modifiers and consumes one transition per actual Maple keyboard read.
  This applies to local Dreamcast keyboards; arcade, controller mappings, and
  GGPO retain their existing input path.
- `bba-rx-thread.patch` and `pico-eth-thread.patch` move the emulated
  Broadband Adapter's frame handoffs onto the right threads; see
  [Network threading](#network-threading) below.

Keyboard queues reset on menu changes, device removal/remapping, savestate loads,
and guest keyboard resets. Each port holds at most 256 transitions; overflow
releases guest keys and resynchronizes to the current host state. Very large
synthetic bursts beyond this limit may be shortened. Held keys still use the
guest's normal repeat behavior.

The focused keyboard regression tests run automatically before each build, or
independently with a C++17 compiler and Git, without cloning Flycast:

```sh
./tools/flycast/test-keyboard-input.sh
```

## Network threading

`bba-rx-thread.patch` hands received network frames to the emulated
Broadband Adapter on the emulation thread. Upstream, the TCP proxy and DCNet
threads call into the RTL8139 model directly, so its receive interrupt is
raised from another thread while the SH-4 rebuilds its interrupt table (KOS
reprograms the timer's interrupt priority on most thread wakeups). The race
leaves stray pending interrupts: a networked KOS program panics with
"Unhandled exception ... code 3" and a random event such as `0640` (DMAC
channel 0) or `03a0`, or simply freezes, typically after a minute or so of
steady traffic such as an online race. Frames now wait in a 64-frame queue
that the SH-4 scheduler drains every 100 us.

`pico-eth-thread.patch` does the same in the other direction for the TCP
proxy. Frames the guest sends were pushed into picoTCP from the emulation
thread, but picoTCP is built without `PICO_SUPPORT_MUTEX`, so its device
queue is unlocked and Flycast itself crashes on the PicoTCP thread
(`Trace/BPT trap`, in `devloop_in` or `pico_socket_udp_deliver`). The
frames are now queued and fed to picoTCP from its own thread before each
stack tick.

Neither networking race is fixed upstream as of October 2026; real Dreamcast
hardware is unaffected.

## Upload regression test

With KOS, curl/mbedTLS ports, Python 3 and OpenSSL installed, run:

```sh
python3 projects/dcvmu.com/client/tests/run_upload_stress.py \
  --host "$(ipconfig getifaddr en0)" --log-dir /tmp/dcvmu-upload-check
```

`--host` must be this Mac's reachable LAN IPv4 address, and
`--flycast /path/to/Flycast` selects another build. The test sends 30 synthetic
uploads to a temporary local HTTPS server; the public service and personal VMUs
are not touched. Flycast v2.7 and earlier time out on the first upload.
