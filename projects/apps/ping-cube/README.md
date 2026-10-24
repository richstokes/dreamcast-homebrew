# Ping Cube

<p align="center">
  <img src="../../../docs/screenshots/ping-cube.png" alt="Ping Cube's spinning cube over live ping replies and timing statistics" width="640">
</p>

Ping Cube is a small KallistiOS demo and the reference project for this
workspace's Makefile and launcher layout. It brings up the network (Broadband
Adapter or a WIZnet W5500 serial-port adapter with DHCP, or the modem dialing
a DreamPi-style PPP peer when neither is present), pings `8.8.8.8`, `1.1.1.1`
or the gateway every 250 ms, and shows the link details and ping statistics on
the six faces of a tumbling cube. The cube is green for fast replies (under 50
ms on a BBA, 200 ms on dial-up), shades to red as they slow and turns red on a
timeout. A console panel behind it logs each reply, and any attached VMU shows
the latest latency with a graph of recent pings.

It demonstrates:

- The standard KOS Makefile and `run-flycast.sh` launcher that new projects copy
- BBA or W5500 DHCP, or modem PPP through `libppp` on a worker thread, with `net_default_dev` inspection
- ICMP echo through `net_icmp_send_echo` and the `net_icmp_echo_cb` reply hook
- BIOS-font text rendered into PowerVR textures on a low-level textured cube
- Controller input, including the analog triggers, and VMU LCD drawing
- Clean shutdown of textures, callbacks and the modem link

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ping-cube.cdi)**
for Flycast or GDEMU. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ping-cube.elf) is also available.
If you are burning a physical CD-R, you may want to try the padded
[CD-R image](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ping-cube-cdr.zip) instead (unzip before burning).

## Controls

| Input | Action |
| --- | --- |
| D-pad Left / Right | Cycle the ping target between `8.8.8.8`, `1.1.1.1` and the gateway; statistics restart |
| A | Snap the nearest face square to the camera; press again to step through the other faces |
| B | Resume tumbling |
| R / L trigger | Spin faster (up to 8x) or slower, down to a stop; both are analog |
| Start | Exit to the Dreamcast BIOS menu (on dial-up, after the current dial step returns) |

## Build and run

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
./run-flycast.sh              # Broadband Adapter over DCNet; builds first
./run-flycast.sh --modem      # Dreamcast modem over Flycast's PPP peer
./run-flycast.sh --skip-build
```

`KOS_ENV` and `FLYCAST_BIN` override the installed dependencies; the launcher
does not change saved Flycast settings.

Dial-up support needs the repository's [KOS patches](../../../tools/kos/README.md)
(PPP lifecycle in particular) applied to your KOS tree, then a rebuild (run
from this directory):

```sh
../../../tools/kos/apply-patches.sh
make -C "$KOS_BASE" -j4
```

## Dial-up (modem / DreamPi)

Set up and test DreamPi (or an equivalent PPP answering setup) with another
application first. The number and PPP login come from the console's saved
PlanetWeb ISP profile, then DreamPassport, then the DreamPi defaults (`555`,
`dream` / `cast`). Tone dialing only. Dialing can take up to 65 s; a dropped
link or a failed dial redials on its own, with a countdown on the front face.

## Gotchas

- Stock KOS 2.3 `ppp_shutdown` does not stop its receive thread, so without the lifecycle patch above the modem cannot hang up or redial safely
- KOS's dial and PPP calls block for up to a minute and cannot be interrupted; they run on a worker thread and Start waits for the current call to return
- Flycast draws lit VMU pixels black on a black background, so with `rend.FloatVMUs` the graph's bars vanish. They show on a real VMU

## Known limitations

- Flycast does not bridge the emulated BBA onto your LAN, so other devices cannot ping the address the cube shows. That needs real BBA hardware
- Real modem and DreamPi hardware are untested; the dial-up path, including redial after a lost link, was checked with Flycast's emulated modem

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
