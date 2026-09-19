# Native Flycast on macOS

All project launchers prefer the locally built Flycast at
`~/.local/share/dreamcast/flycast/Flycast.app`. They fall back to
`/Applications/Flycast.app`, and an explicit `FLYCAST_BIN` overrides either.
Apple Silicon runs natively; Rosetta is not needed.

To build and install:

```sh
brew install cmake pkg-config
./tools/flycast/build-macos.sh
./projects/dcvmu.com/client/run-flycast.sh
```

Xcode command-line tools are required. The build uses OpenGL and Flycast's bundled
SDL. It leaves the application in `/Applications` alone. Sources and build
products stay outside this repository; only the patch and builder are tracked.
The installed app depends on the Homebrew libraries used during the build.

## Pinned revision

The build uses Flycast master at
[`dd7a5f0`](https://github.com/flyinghead/flycast/commit/dd7a5f06201a4f8960ed162e2c6a5b61659bb9cd),
which is newer than v2.7 and includes two fixes these projects rely on:

- [#2492](https://github.com/flyinghead/flycast/pull/2492): opening a game at
  launch on macOS no longer trips the SH-4 recompiler's `sq_buffer` assertion.
- [#2497](https://github.com/flyinghead/flycast/pull/2497): uploads through the
  TCP proxy (`DCNet=no`) no longer stall and time out. Raising the client
  timeout or retrying does not help on older builds, including v2.7; rebuild
  with the command above. Real BBA hardware does not use this proxy.

`macos-build.patch` is the only local change. It lets the command-line CMake
build compile the bundled SDL's Objective-C sources, and turns off precompiled
headers for the `flycast` target, which cannot be shared between C++ and
Objective-C++ sources.

Homebrew's `sdl2` is now the sdl2-compat shim over SDL3, and with it the #2492
fix crashes when a game is passed on the command line, so the build does not
use the host SDL.

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
