# Dreamcast Homebrew

A collection of original Sega Dreamcast games, network applications, and
technical demos built with [KallistiOS](https://kos-docs.dreamcast.wiki/).
Each project builds as a standalone SH-4 ELF that can boot directly in Flycast
or run on compatible Dreamcast hardware.

## Projects

| Preview | Project |
| --- | --- |
| <img src="docs/screenshots/gravity-wave.png" alt="Gravity Wave" width="360"> | **[Gravity Wave](projects/gravity-wave/)**<br>An infinite 3D arcade flight game featuring four biomes, procedural terrain, enemy formations, guardians, upgrades, and a synthesized soundtrack. |
| <img src="docs/screenshots/demon-bazooka.png" alt="Demon Bazooka" width="360"> | **[Demon Bazooka](projects/demon-bazooka/)**<br>A compact 3D arena shooter with rockets, dashes, screen-clearing barrages, escalating demon waves, and runtime-generated visuals and audio. |
| <img src="docs/screenshots/dreamcast-browser.png" alt="Dreamcast Browser" width="360"> | **[Dreamcast Browser](projects/dreamcast-browser/)**<br>A deliberately limited HTTPS browser supporting basic text, links, raster images, and Dreamcast keyboard, mouse, and controller input. JavaScript is not executed. |
| <img src="docs/screenshots/dreamcast-irc.png" alt="Dreamcast IRC" width="360"> | **[Dreamcast IRC](projects/dreamcast-irc/)**<br>A Broadband Adapter IRC client with server and channel pages, fixed-size scrollback, keyboard input, and controller navigation. |
| <img src="docs/screenshots/ping-cube.png" alt="Ping Cube" width="360"> | **[Ping Cube](projects/ping-cube/)**<br>A networking and PowerVR demo that continuously pings `8.8.8.8`, visualizes latency through the cube's color, and displays live replies, loss, and timing statistics. |

Each project directory contains its own controls, dependencies, technical
notes, and verification instructions.

## Building and running

The projects use the KallistiOS SH-4 toolchain. The known-working development
baseline is:

- KallistiOS 2.3.0
- GCC 15.2.0 SH-4 toolchain
- Flycast 2.7

Source the KOS environment, then build a project:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make -C projects/gravity-wave
```

Launch it directly in Flycast:

```sh
./projects/gravity-wave/run-flycast.sh
```

The launchers build by default and accept `KOS_ENV` and `FLYCAST_BIN`
overrides. When the ELF already exists, use `--skip-build` or the project's
`make run` target.

Dreamcast Browser additionally requires the KOS ports for curl, mbedTLS, zlib,
and stb_image. Its [project README](projects/dreamcast-browser/README.md)
documents the included compatibility patch and installation commands. Gravity
Wave's generated texture sources are checked in; Python and `uv` are needed
only when regenerating them.

## Flycast networking

The networked projects use Flycast's Broadband Adapter emulation. With
`DCNet=no`, Flycast provides a private picoTCP proxy suitable for the outbound
connections used by Dreamcast Browser and Dreamcast IRC.

Stock Flycast 2.7 does not bridge the emulated Dreamcast onto the Mac's
physical LAN. The private picoTCP address—and the address assigned through the
DCNet VPN—cannot ordinarily be reached or pinged directly from the host or
another LAN device. Inbound servers require port forwarding, a custom bridged
backend, or real Dreamcast BBA hardware.

Dreamcast IRC connects without TLS or SASL; do not use it to send passwords or
other sensitive information.

## Repository hygiene

Generated objects, ELF files, disc images, temporary files, and other build
outputs are intentionally untracked. Root-level `BIOS/` and `GAMES/`
directories are also ignored because they contain user-owned runtime data and
are not part of these projects.

## License and attribution

The original project code in this repository is available under the
[MIT License](LICENSE).

These projects are powered by
[KallistiOS](https://kos-docs.dreamcast.wiki/), the independent Dreamcast SDK.
KallistiOS is distributed separately under its BSD-like KOS license and
requires attribution.
