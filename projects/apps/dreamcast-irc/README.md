# Dreamcast IRC

Dreamcast IRC is a small KallistiOS IRC client that connects over a Broadband
Adapter or a Dreamcast modem with a working DreamPi setup. By default it
connects to `irc.libera.chat` and joins `#dreamcastdev`. It renders a
scrollable chat view with the Dreamcast BIOS font and accepts text from a
Dreamcast keyboard, with a server page followed by up to ten channel pages.

It supports multiple channels, messages, actions, notices, topics, names,
channel listing, and automatic reconnects. The default nickname is `DCIRC_`
plus a random three-digit number.

Your nickname, server, and open channels are saved to a `DCIRC_CFG` file on the
first available VMU and restored on the next launch. Without a VMU it falls
back to the defaults.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc.elf)
is also available.

## Build

Modem support requires the repository's
[PPP lifecycle patch](../../dcvmu.com/client/patches/kos-ppp-lifecycle.patch).
Apply it once to your KOS tree from this project directory, then rebuild KOS
and PPP (CI does this automatically):

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
git -C "$KOS_BASE" apply "$PWD/../../dcvmu.com/client/patches/kos-ppp-lifecycle.patch"
make -C "$KOS_BASE/kernel" -j4
make -C "$KOS_BASE/addons/libppp" -j4
```

From this project directory, build with the SH-4 toolchain:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

This produces `dreamcast-irc.elf`, which Flycast can boot directly.

## Run in Flycast

```sh
./run-flycast.sh
# Exercise the modem/PPP path instead of BBA:
./run-flycast.sh --modem
```

The launcher attaches a Dreamcast keyboard, forwards host keyboard input, and
does not modify the saved Flycast configuration. `--modem` uses Flycast's modem
emulation, not a physical DreamPi. `--skip-build` works with either mode.

## Modem / DreamPi

Configure and test DreamPi with another app first. A Broadband Adapter is
always preferred; the modem is used only when no Ethernet adapter is found.

The client dials on startup using the console's saved PlanetWeb ISP settings,
falling back to DreamPassport, and then to the DreamPi defaults (`555`,
`dream` / `cast`). Only tone dialing is supported.

Dialing can take about 65 seconds before timing out, with progress shown on
the server page. Carrier loss triggers an automatic redial and channel rejoin.
`/reconnect` or A+B hangs up and redials.

Real Dreamcast/DreamPi hardware testing is still required.

## Tests

The network test runs a private IRC server and a temporary ELF in Flycast; no
public IRC network is contacted. From the repository root, supply the Mac's
LAN IPv4 address:

```sh
python3 projects/apps/dreamcast-irc/tests/run_network.py --host <Mac-LAN-IP>
python3 projects/apps/dreamcast-irc/tests/run_network.py --modem --host <Mac-LAN-IP>
python3 projects/apps/dreamcast-irc/tests/run_network.py --modem --cancel-dial --host <Mac-LAN-IP>
```

## Controls

- Type with a Dreamcast keyboard and press Enter to send.
- D-pad or keyboard Left/Right switches between the server and channel pages.
- D-pad Up/Down or keyboard Up/Down/Page Up/Page Down scrolls the current page.
- X+Y returns to the newest messages.
- A+B reconnects to the current IRC network.
- Start quits cleanly.

Available commands are `/server HOST [PORT]`, `/join #CHANNEL[,#CHANNEL]`,
`/part [#CHANNEL] [MESSAGE]`, `/list [#CHANNEL|#MASK*]`, `/me ACTION`, `/nick NAME`,
`/clear`, `/reconnect`, `/quit [MESSAGE]`, and `/help`.

`/server` disconnects from the current network and connects to the new host;
only one network is connected at a time. Omitting the port tries 6667 and then
8000. Bare `/list` requests channels with more than 500 users, and results are
capped at 80.

## Network and security notes

This client uses unencrypted IRC and does not implement TLS, SASL, or NickServ
authentication. Do not send passwords or other sensitive information through
it.

Incoming UTF-8 is converted to the BIOS font's ISO-8859-1 character set;
characters outside that set appear as `?`.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
