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
