# Native Flycast on macOS

Project launchers use `~/.local/share/dreamcast/flycast/Flycast.app` when it
exists, otherwise `/Applications/Flycast.app`. `FLYCAST_BIN` overrides both
(see `resolve.sh`). The local build runs natively on Apple Silicon.

```sh
brew install cmake pkg-config sdl2
./tools/flycast/build-macos.sh
```

The build needs the Xcode command-line tools. Sources and build products go
under `~/Library/Caches` (override with `FLYCAST_BUILD_DIR`); only the patches
and the builder are tracked here, and `/Applications/Flycast.app` is left
alone.

## Why not stock Flycast 2.7

The builder pins Flycast master at
[`628bd3d`](https://github.com/flyinghead/flycast/commit/628bd3dbb160ea2750230fc6b00c0bb8173cb1f6),
newer than v2.7, for three merged fixes:

- [#2492](https://github.com/flyinghead/flycast/pull/2492) and
  [#2500](https://github.com/flyinghead/flycast/pull/2500): opening a game at
  launch on macOS no longer crashes (SH-4 recompiler `sq_buffer` assertion,
  then a crash with Homebrew's sdl2-compat)
- [#2497](https://github.com/flyinghead/flycast/pull/2497): uploads through the
  TCP proxy (`DCNet=no`) no longer stall. Longer timeouts and retries do not
  help on v2.7, so DCVMU uploads need this build. Real BBA hardware is
  unaffected

It then applies four local patches. Each patch explains itself in code
comments; the short version:

| Patch | Why |
| --- | --- |
| `macos-build.patch` | Precompiled headers off; the command-line CMake build cannot share them between C++ and Objective-C++ |
| `keyboard-input.patch` | Short host key presses were lost when pressed and released between guest polls. Needed by the keyboard apps (BASIC, IRC, Browser). Local Dreamcast keyboards only; controller mappings, arcade and GGPO are untouched |
| `bba-rx-thread.patch` | Received BBA frames raised the receive interrupt from a network thread, racing the SH-4 interrupt table. KOS programs panicked with "Unhandled exception ... code 3" or froze after a minute or so of steady traffic (online races) |
| `pico-eth-thread.patch` | Sent frames were pushed into picoTCP from the emulation thread with no locking, and Flycast itself crashed (`Trace/BPT trap` in `devloop_in` or `pico_socket_udp_deliver`) |

The two threading fixes are proposed upstream in
[#2569](https://github.com/flyinghead/flycast/pull/2569), still open as of
October 2026. Drop those patches once the pinned revision includes it.

## Tests

| Command | Checks |
| --- | --- |
| `./tools/flycast/test-keyboard-input.sh` | The queue helper from `keyboard-input.patch`, host-side with any C++17 compiler, no Flycast clone needed. Also runs before every build |
| `python3 projects/dcvmu.com/client/tests/run_upload_stress.py --host "$(ipconfig getifaddr en0)" --log-dir /tmp/dcvmu-upload-check` | 30 synthetic uploads through the TCP proxy to a temporary local HTTPS server. Needs KOS, the curl/mbedTLS ports, Python 3 and OpenSSL. `--host` is this Mac's LAN IPv4 and `--flycast` picks another build. The public service and real VMUs are not touched. Flycast v2.7 fails on the first upload |
