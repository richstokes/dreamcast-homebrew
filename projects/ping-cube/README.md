# Ping Cube

Ping Cube is a tiny Sega Dreamcast homebrew demo built with KallistiOS. It asks
the Broadband Adapter for an IPv4 address with DHCP, continuously pings
`8.8.8.8`, and maps the address onto all six faces of a slowly rotating 3D
cube. Replies below 50 ms tint the cube green; progressively slower replies
move it toward red, and a timeout makes it red.

The project uses KallistiOS directly: `INIT_NET` for BBA discovery, DHCP, and
ICMP echo replies; the BIOS font for the runtime-generated IP texture; and the
low-level PowerVR API for rendering. It has no asset or kos-ports dependencies.

## Build

Set up the official KallistiOS Dreamcast toolchain, then source its environment
and run `make`:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

This produces `ping-cube.elf`, which Flycast can launch directly.

## Run in Flycast with BBA support

```sh
./run-flycast.sh
```

The run script enables Flycast's Broadband Adapter and DCNet backends
transiently, so it does not rewrite the user's saved emulator settings. DCNet
is used because Flycast's local picoTCP backend synthesizes ICMP replies rather
than measuring the real route to `8.8.8.8`. Set `KOS_ENV` or `FLYCAST_BIN` if
either dependency is installed somewhere else.

When an address is assigned, the cube shows `ICMP ECHO READY`. No application
socket is needed for ping: KallistiOS validates ICMP echo requests and sends
echo replies from its IPv4 receive path.

A dim console panel behind the cube shows transmitted packets, replies, loss,
average latency, the most recent result, sequence numbers, TTL values, and
timeouts. Ping requests are sent every 250 ms with a 2.5-second timeout.

### Flycast network reachability

Flycast 2.7 does not provide a bridged BBA backend. Its `Use DCNet` mode joins
the Dreamcast to the DCNet VPN (typically producing a `172.20.4.x` address),
while disabling DCNet selects Flycast's embedded picoTCP proxy (a private
`192.168.169.x` network). Both modes can provide outbound connectivity, but
neither places the emulated BBA on the Mac's physical LAN. Consequently, a
different device on the LAN cannot ping the address shown by stock Flycast,
even though Ping Cube itself correctly handles ICMP echo requests.

True LAN ping requires a layer-2-capable Flycast backend (for example a future
TAP/vmnet bridge) or real Dreamcast BBA hardware. Merely changing the guest IP
or turning off DCNet does not create that bridge.

Press Start to exit the demo.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
