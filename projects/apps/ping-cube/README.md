# Ping Cube

Ping Cube is a tiny Sega Dreamcast homebrew demo built with KallistiOS. It
brings up the network — the Broadband Adapter with DHCP, or the Dreamcast modem
dialing a DreamPi-style PPP peer — continuously pings `8.8.8.8`, `1.1.1.1` or
your gateway, and shows the connection details on the six faces of a rotating
3D cube. Fast replies tint the cube green; progressively slower replies move it
toward red, and a timeout makes it red. Green means under 50 ms on a Broadband
Adapter, or under 200 ms on dial-up.

## What each face shows

| Face | Contents |
| --- | --- |
| Front | Title, the Dreamcast's IP address and the link status |
| Right | Gateway (PPP peer on dial-up) and netmask |
| Back | DNS server, broadcast address and MTU |
| Left | Adapter hardware, plus the MAC address (Ethernet) or carrier rate (modem) |
| Top | Live ping statistics for the current target: latest, average, min/max, packets sent and loss |
| Bottom | Session uptime, spin speed or snapped face, and a controls reminder |

The cube tumbles on two axes so every face comes into view, and a dim console
panel behind it logs sequence numbers, TTLs, latencies, timeouts and link
events. Ping requests are sent every 250 ms with a 2.5-second timeout.

Any attached VMU shows the target, the latest latency in large digits and a
graph of the last 48 pings along the bottom (dotted columns are timeouts).

## Controls

- **D-pad left/right**: cycle the ping target between `8.8.8.8`, `1.1.1.1` and
  the gateway (the PPP peer on dial-up). Statistics restart for each target.
- **A**: snap the face nearest the camera square and upright; press again to
  step through the other faces.
- **B**: resume tumbling.
- **R trigger**: spin faster (up to 8x).
- **L trigger**: spin slower, down to a full stop.
  Both triggers are analog, so light pressure adjusts the speed gently.
- **Start**: exit. On a dial-up link the demo hangs up first; KOS's dial and PPP
  calls cannot be interrupted, so exiting during dialing takes effect once the
  current step returns.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ping-cube.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/ping-cube.elf)
is also available.

## Build

Set up the official KallistiOS Dreamcast toolchain, then source its environment
and run `make`:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

This produces `ping-cube.elf`, which Flycast can launch directly.

Dial-up support needs the `kos-ppp-lifecycle.patch` from
[`projects/dcvmu.com/client/patches`](../../dcvmu.com/client/patches) applied to
your KOS tree, followed by a `libppp` rebuild:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
git -C "$KOS_BASE" apply ../../dcvmu.com/client/patches/kos-ppp-lifecycle.patch
make -C "$KOS_BASE/kernel" -j4
make -C "$KOS_BASE/addons/libppp" -j4
```

## Run in Flycast

```sh
./run-flycast.sh           # Broadband Adapter over DCNet
./run-flycast.sh --modem   # Dreamcast modem over Flycast's PPP peer
```

The script does not change saved Flycast settings. Set `KOS_ENV` or
`FLYCAST_BIN` if either dependency is installed somewhere else.

## Dial-up (modem / DreamPi)

A Broadband Adapter is always preferred; the modem is used only when no
Ethernet adapter is found.

Set up and test DreamPi (or an equivalent PPP answering setup) with another
application first. Ping Cube reads the phone number and PPP login from the
console's saved **PlanetWeb** ISP profile, falling back to **DreamPassport**,
and then to the DreamPi defaults (`555`, `dream` / `cast`). Only tone dialing
is supported.

Dialing can take up to 65 seconds. If an established link drops (the modem
loses carrier or PPP shuts down) Ping Cube hangs up and redials straight away.
A failed dial that might succeed later — no dial tone, no carrier, PPP failure —
is retried after 10 s, then 20 s, and so on up to once a minute, with a
countdown on the front face. A missing modem or unusable dial settings are
reported on the front face and not retried.

Real modem and DreamPi hardware validation is still outstanding; the dial-up
path, including redialing after a lost PPP link, has been tested with Flycast's
emulated modem.

Flycast draws lit VMU pixels in black on a black background, so with
`rend.FloatVMUs` the graph's bars blend into the surroundings. They show
normally on a real VMU.

### Flycast and LAN pings

Flycast does not bridge the emulated BBA onto your LAN, so other devices cannot
ping the address the cube shows. That needs real BBA hardware.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
