# Dreamcast Homebrew

A collection of original Sega Dreamcast games, network applications, and
technical demos. Most projects use
[KallistiOS](https://kos-docs.dreamcast.wiki/); Chroma Circuit is a deliberately
bare-metal SH-4 assembly production with no SDK runtime. Each console project builds as
a standalone SH-4 ELF that can boot directly in Flycast or run on compatible
Dreamcast hardware.

## dcvmu.com

[DCVMU](https://dcvmu.com) backs up and shares Dreamcast VMU saves. The
[Dreamcast client](projects/dcvmu.com/client/) uploads saves and downloads them
to a VMU, remembers login, and supports public/private archives. The
[web service](projects/dcvmu.com/service/) provides registration, browsing,
and a local backup script.

**[Download DCVMU CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dcvmu-client.cdi)**
· [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dcvmu-client.elf)
· [Website](https://dcvmu.com)

CDI is a self-booting disc image for a compatible Dreamcast or Flycast. ELF is
available for development and compatible homebrew loaders. Both are built by CI.

<img src="projects/dcvmu.com/service/static/dcvmu-client.png" alt="DCVMU browsing VMU saves with game icons and Power Stone selected" width="760">

## Games

| Preview | Project | Latest download | Try in browser |
| --- | --- | --- | --- |
| <img src="docs/screenshots/drift-los-angeles.png" alt="Drift Los Angeles" width="360"> | **[Drift Los Angeles](projects/games/drift-los-angeles/)**<br>An open-city arcade street-drifting game with a C7-inspired coupe, four Los Angeles-style districts, traffic, drift chains, dense tire smoke, and recorded V8 audio. | **[CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/drift-los-angeles.cdi)**<br>[ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/drift-los-angeles.elf) | [Play](https://appsbyrich.com/dreamcast?rom=continuous%2Fdrift-los-angeles.elf&name=Drift%20Los%20Angeles) |
| <img src="docs/screenshots/gravity-wave.png" alt="Gravity Wave" width="360"> | **[Gravity Wave](projects/games/gravity-wave/)**<br>An infinite 3D arcade flight game featuring four biomes, procedural terrain, enemy formations, guardians, upgrades, and a synthesized soundtrack. | **[CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave.cdi)**<br>[ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/gravity-wave.elf) | [Play](https://appsbyrich.com/dreamcast?rom=continuous%2Fgravity-wave.elf&name=Gravity%20Wave) |
| <img src="docs/screenshots/demon-bazooka.png" alt="Demon Bazooka" width="360"> | **[Demon Bazooka](projects/games/demon-bazooka/)**<br>A compact 3D arena shooter with rockets, dashes, screen-clearing barrages, escalating demon waves, and runtime-generated visuals and audio. | **[CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/demon-bazooka.cdi)**<br>[ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/demon-bazooka.elf) | [Play](https://appsbyrich.com/dreamcast?rom=continuous%2Fdemon-bazooka.elf&name=Demon%20Bazooka) |

## Apps and demos

| Preview | Project | Latest download | Try in browser |
| --- | --- | --- | --- |
| <img src="docs/screenshots/chroma-circuit.png" alt="Chroma Circuit Hyperfold scene" width="360"> | **[Chroma Circuit](projects/apps/chroma-circuit/)**<br>An eight-act, 60 Hz Dreamcast demo built entirely in SH-4 assembly, with raw PowerVR2 graphics and AICA music. | **[CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/chroma-circuit.cdi)**<br>[ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/chroma-circuit.elf) | [Play](https://appsbyrich.com/dreamcast?rom=continuous%2Fchroma-circuit.elf&name=Chroma%20Circuit) |
| <img src="docs/screenshots/dreamcast-browser.png" alt="Dreamcast Browser" width="360"> | **[Dreamcast Browser](projects/apps/dreamcast-browser/)**<br>A deliberately limited HTTPS browser supporting basic text, links, raster images, and Dreamcast keyboard, mouse, and controller input. JavaScript is not executed. | **[CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.cdi)**<br>[ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.elf) | [Play](https://appsbyrich.com/dreamcast?rom=continuous%2Fdreamcast-browser.elf&name=Dreamcast%20Browser) |
| <img src="docs/screenshots/dreamcast-irc.png" alt="Dreamcast IRC" width="360"> | **[Dreamcast IRC](projects/apps/dreamcast-irc/)**<br>A Broadband Adapter IRC client with server and channel pages, fixed-size scrollback, keyboard input, and controller navigation. | **[CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc.cdi)**<br>[ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc.elf) | [Play](https://appsbyrich.com/dreamcast?rom=continuous%2Fdreamcast-irc.elf&name=Dreamcast%20IRC) |
| <img src="docs/screenshots/ping-cube.png" alt="Ping Cube" width="360"> | **[Ping Cube](projects/apps/ping-cube/)**<br>A networking and PowerVR demo that continuously pings `8.8.8.8`, visualizes latency through the cube's color, and displays live replies, loss, and timing statistics. | **[CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ping-cube.cdi)**<br>[ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ping-cube.elf) | [Play](https://appsbyrich.com/dreamcast?rom=continuous%2Fping-cube.elf&name=Ping%20Cube) |

Each project directory contains its own controls, dependencies, technical
notes, and verification instructions.

The CDI links are the easiest way to play: download the image and open it in
Flycast, or burn it to CD-R for a Dreamcast that supports MIL-CD. The smaller
ELF downloads are useful for direct emulator boot and development loaders.
[SHA-256 checksums](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/SHA256SUMS)
are published with every build.

## Building and running

The projects use the Dreamcast SH-4 cross-toolchain. Most also use KallistiOS;
Chroma Circuit invokes only `sh-elf-as`, `sh-elf-ld`, and the matching binary
inspection tools. The known-working development baseline is:

- KallistiOS 2.3.0
- GCC 15.2.0 SH-4 toolchain
- Flycast 2.7

Source the KOS environment, then build a project:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make -C projects/games/drift-los-angeles
```

Chroma Circuit needs no KOS build and can be assembled directly:

```sh
make -C projects/apps/chroma-circuit
make -C projects/apps/chroma-circuit verify
```

Launch it directly in Flycast:

```sh
./projects/games/drift-los-angeles/run-flycast.sh
```

The launchers build by default and accept `KOS_ENV` and `FLYCAST_BIN`
overrides. When the ELF already exists, use `--skip-build` or the project's
`make run` target.

DCVMU and Dreamcast Browser additionally require the KOS ports for curl, mbedTLS, zlib,
and stb_image. Its [project README](projects/apps/dreamcast-browser/README.md)
documents the included compatibility patch and installation commands. Gravity
Wave's generated texture sources are checked in. Drift Los Angeles likewise
ships its generated PowerVR textures, vehicle mesh, recorded audio, and music
sources; Python, `uv`, Blender, and `afconvert` are needed only when regenerating
those assets.

## Repository layout and releases

- `projects/dcvmu.com/`: client and web service.
- `projects/games/`: playable games.
- `projects/apps/`: applications and technical demos.
- `BIOS/` and `GAMES/`: untracked, user-owned runtime files.

[The release manifest](.github/console-projects.txt) is the single list of console
projects for CI building, validation, and ELF/CDI packaging. Every push to `main`
tests DCVMU, builds all eight console projects with pinned SDK dependencies,
and publishes [the rolling GitHub release](https://github.com/richstokes/dreamcast-homebrew/releases/latest).
The website links to these artifacts; no client binary needs uploading to its server.

## License and attribution

The original project code in this repository is available under the
[MIT License](LICENSE).

These projects are powered by
[KallistiOS](https://kos-docs.dreamcast.wiki/), the independent Dreamcast SDK.
KallistiOS is distributed separately under its BSD-like KOS license and
requires attribution.
