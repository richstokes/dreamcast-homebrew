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

On macOS, AppKit can send `application:openFile:` before `SDL_main` finishes
initializing the emulator. Dispatching that callback to the main queue does not
ensure initialization has completed. The game loader can enter
`addrspace::initMappings()` while `ram_base` is still null, allocating separate
RAM buffers; `addrspace::reserve()` then establishes a different memory layout.
The SH-4 recompiler correctly rejects those inconsistent addresses with its
`sq_buffer` assertion. This occurs before any Dreamcast program runs.

`macos-startup.patch` queues the initial document until `flycast_init` succeeds,
then lets the normal main loop load it. Documents opened after initialization
retain their normal behavior. No memory checks are bypassed, and the native
recompiler stays enabled. `macos-build.patch` fixes Objective-C precompiled-header
and disabled-Vulkan build issues for the command-line CMake build.

Verified locally with an arm64 build: DCVMU boots, attaches six VMUs, obtains a
BBA address and restores its saved login. The other project launchers are also
smoke-tested for startup. This is an emulator fix; it changes no KallistiOS or
distributed Dreamcast binaries.

## TCP upload fix

`picotcp-upload-drain.patch` fixes intermittent upload timeouts with `DCNet=no`.
The proxy copies guest TCP data into a 1,510-byte buffer and writes it to the
host socket asynchronously. A single picoTCP read notification can cover more
than one buffer of data. Previously, write completion only replayed pending
events; if no further read event arrived, the remaining bytes stayed queued.
The guest could receive TCP acknowledgements for the entire upload while the
HTTPS server waited indefinitely for the rest of a TLS record.

Write completion now resumes reading from picoTCP once its output buffer is
available. It stops when the queue is drained, preserving asynchronous
backpressure and the existing close/error handling. The patch guards against
reading a closed or detached picoTCP socket. Increasing the client timeout or
retrying a POST does not fix this stalled queue.

Rebuild with the command above to install the fix. An ELF rebuild alone cannot
update the emulator's TCP proxy. Real BBA hardware does not use this proxy.

### Regression test

With KOS, curl/mbedTLS ports, Python 3 and OpenSSL installed, run:

```sh
python3 projects/dcvmu.com/client/tests/run_upload_stress.py \
  --host "$(ipconfig getifaddr en0)" --log-dir /tmp/dcvmu-upload-check
```

`--host` must be this Mac's reachable LAN IPv4 address (choose the appropriate
interface if it is not `en0`). `--flycast /path/to/Flycast` selects another build.
The runner builds a temporary KOS ELF using the actual client's upload code,
creates a temporary verified TLS endpoint and isolated VMUs, and checks 30
uploads cycling through 4,608, 8,704 and 98,816 bytes. It verifies every received
save byte and the number/order of requests. Only synthetic data and a dummy
token are used; the public service and personal VMUs are not accessed.
Build/serial logs and results remain in `--log-dir`; temporary test assets are
removed and the test emulator is stopped automatically.

Locally, the original proxy timed out on the first 4,608-byte upload after
60 seconds with no HTTP response; the patched proxy passed all 30 uploads.
