# Native Flycast on macOS

All project launchers prefer the locally patched Flycast at
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
products stay outside this repository; only the patches and builder are tracked.
The installed app depends on the Homebrew libraries used during the build.

## Startup fix

The patches target [Flycast v2.7](https://github.com/flyinghead/flycast/tree/v2.7),
commit `5aa091fde632fb332c8d8c34e280d62dc951954c`.

`macos-startup.patch` fixes a macOS startup crash: AppKit could ask Flycast to
open the game before the emulator finished initializing, which tripped the
SH-4 recompiler's `sq_buffer` assertion. The patch queues the initial document
until initialization succeeds. `macos-build.patch` fixes build issues in the
command-line CMake build.

## TCP upload fix

`picotcp-upload-drain.patch` fixes intermittent upload timeouts with
`DCNet=no`. Flycast's TCP proxy could acknowledge a guest upload but leave part
of it queued, so the HTTPS server waited forever for the rest. Raising the
client timeout or retrying does not help; rebuild Flycast with the command
above to install the fix. Real BBA hardware does not use this proxy.

### Regression test

With KOS, curl/mbedTLS ports, Python 3 and OpenSSL installed, run:

```sh
python3 projects/dcvmu.com/client/tests/run_upload_stress.py \
  --host "$(ipconfig getifaddr en0)" --log-dir /tmp/dcvmu-upload-check
```

`--host` must be this Mac's reachable LAN IPv4 address, and
`--flycast /path/to/Flycast` selects another build. The test sends 30 synthetic
uploads to a temporary local HTTPS server; the public service and personal VMUs
are not touched. The unpatched proxy times out on the first upload.
