# Ping Cube

Ping Cube is a tiny Sega Dreamcast homebrew demo built with KallistiOS. It
brings up the network — the Broadband Adapter with DHCP, or the Dreamcast modem
dialing a DreamPi-style PPP peer — continuously pings `8.8.8.8`, and shows the
connection details on the six faces of a rotating 3D cube. Replies below 50 ms
tint the cube green; progressively slower replies move it toward red, and a
timeout makes it red.

## What each face shows

| Face | Contents |
| --- | --- |
| Front | Title, the Dreamcast's IP address and the link status |
| Left | Gateway (PPP peer on dial-up) and netmask |
| Back | DNS server, broadcast address and MTU |
| Right | Adapter hardware, plus the MAC address (Ethernet) or carrier rate (modem) |
| Top | Live ping statistics: latest, average, min/max, packets sent and loss |
| Bottom | Session uptime, the current spin speed and the trigger hint |

The cube tumbles on two axes so every face comes into view, and a dim console
panel behind it logs sequence numbers, TTLs, latencies, timeouts and link
events. Ping requests are sent every 250 ms with a 2.5-second timeout.

## Controls

- **R trigger**: spin faster (up to 8x).
- **L trigger**: spin slower, down to a full stop so a face can be read.
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

Dialing can take up to 65 seconds. Failures leave a short reason on the front
face (`NO DIAL TONE`, `NO CARRIER`, `PPP FAILED`, …); restart to dial again. A
dropped link is not redialed. A healthy ~150 ms modem link shows as orange
rather than green.

Real modem and DreamPi hardware validation is still outstanding; the dial-up
path has been tested with Flycast's emulated modem.

### Flycast and LAN pings

Flycast does not bridge the emulated BBA onto your LAN, so other devices cannot
ping the address the cube shows. That needs real BBA hardware.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
