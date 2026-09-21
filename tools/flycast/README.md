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

`macos-build.patch` is the only local change: it turns off precompiled headers
for the `flycast` target, which the command-line CMake build cannot share
between C++ and Objective-C++ sources.

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
