# Ping Cube

Ping Cube is a tiny Sega Dreamcast homebrew demo built with KallistiOS. It
brings up the network — the Broadband Adapter with DHCP, or the Dreamcast modem
dialing a DreamPi-style PPP peer — continuously pings `8.8.8.8`, and shows the
connection details on the six faces of a rotating 3D cube. Replies below 50 ms
tint the cube green; progressively slower replies move it toward red, and a
timeout makes it red.

The project uses KallistiOS directly: `INIT_NET` for adapter discovery, DHCP and
ICMP echo replies; `libppp` for dial-up; the BIOS font for the runtime-generated
face textures; and the low-level PowerVR API for rendering. It has no asset or
kos-ports dependencies.

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

The dial-up path needs the `kos-ppp-lifecycle.patch` from
[`projects/dcvmu.com/client/patches`](../../dcvmu.com/client/patches). It lets
KOS initialize TCP/IP when no Ethernet adapter is present, and stops and joins
the PPP receive worker before the modem buffers are freed. CI applies it before
building KOS; apply it to a local SDK the same way, then rebuild `libppp`:

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

The run script enables Flycast's network backends transiently, so it does not
rewrite the user's saved emulator settings. DCNet is used for the BBA because
Flycast's local picoTCP backend synthesizes ICMP replies rather than measuring
the real route to `8.8.8.8`. Set `KOS_ENV` or `FLYCAST_BIN` if either dependency
is installed somewhere else.

When an address is assigned, the cube shows `ICMP ECHO READY`. No application
socket is needed for ping: KallistiOS validates ICMP echo requests and sends
echo replies from its IPv4 receive path.

## Dial-up (modem / DreamPi)

An Ethernet adapter always wins. The BBA and the modem share the expansion
port, so the modem is probed only when `INIT_NET` found no default interface;
a BBA that fails DHCP does not fall back to dialing.

Set up and test DreamPi (or an equivalent PPP answering setup) with another
application first — Ping Cube never configures DreamPi, writes console flash or
interprets modem AT initialization strings. It reads the primary phone number,
PPP username/password, blind-dial flag and optional DNS from the console's
saved **PlanetWeb** ISP profile, falling back to **DreamPassport**. The first
profile with a nonempty primary number wins; credentials are never mixed
between profiles. With no usable profile it uses the KOS DreamPi example's
`555` number and `dream` / `cast` login. ISP credentials are cleared from RAM
after use and are never printed.

KOS dials DTMF tones only: pulse dialing and dial strings containing pauses or
waits are rejected rather than silently dialed differently. Spaces, hyphens,
parentheses and periods are ignored, and the saved area code is prepended when
the profile asks for it.

Detection, dialing (up to 65 seconds for the KOS dial-tone and carrier waits)
and PPP negotiation run on a worker thread, so the cube keeps rendering and the
console panel logs each step. Failures leave a short reason on the front face
(`NO DIAL TONE`, `NO CARRIER`, `PPP FAILED`, …); restart to dial again. A
dropped link is not automatically redialed — pings simply start timing out and
the cube turns red.

Latency colouring is unchanged on dial-up, so a healthy ~150 ms modem link
shows as orange rather than green. That is an honest reading of the route, not
a fault.

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

`--modem` uses Flycast's emulated modem, which answers any number and hands out
a `192.168.167.x` PPP address through picoTCP. That verifies the dial, PPP and
ICMP paths, but real modem, phone-line and DreamPi hardware validation is still
outstanding.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
