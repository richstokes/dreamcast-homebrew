# Dreamcast IRC

<p align="center">
  <img src="../../../docs/screenshots/dreamcast-irc.png" alt="Dreamcast IRC joined to #netsplit on Libera.Chat" width="640">
</p>

A small KallistiOS IRC client for the Broadband Adapter, a WIZnet W5500
serial-port Ethernet adapter, or a Dreamcast modem with DreamPi. By default it
connects to `irc.libera.chat` and joins `#dreamcastdev`. There is a server
page plus up to ten channel pages; type with a Dreamcast keyboard and move
around with the controller. Nickname, server and open channels are saved to
`DCIRC_CFG` on the first VMU and restored next launch (defaults without a
VMU). The default nickname is `DCIRC_` plus three random digits.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc.cdi)**
for Flycast or GDEMU. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc.elf) is also available.
If you are burning a physical CD-R, you may want to try the padded
[CD-R image](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc-cdr.zip) instead (unzip before burning).

## Controls

| Input | Action |
| --- | --- |
| Keyboard, Enter | Type and send |
| D-pad or keyboard Left/Right | Switch between the server and channel pages |
| D-pad Up/Down, keyboard Up/Down/PgUp/PgDn | Scroll the current page |
| X+Y | Jump back to the newest messages |
| A+B | Reconnect to the current network |
| Start | Quit cleanly to the Dreamcast BIOS menu |

| Command | Notes |
| --- | --- |
| `/server HOST [PORT]` | Disconnects and connects to the new host; one network at a time. Without a port it tries 6667, then 8000 |
| `/join #CHANNEL[,#CHANNEL]` | |
| `/part [#CHANNEL] [MESSAGE]` | |
| `/list [#CHANNEL\|#MASK*]` | Bare `/list` asks for channels with more than 500 users; results are capped at 80 |
| `/me ACTION`, `/nick NAME`, `/clear`, `/reconnect`, `/quit [MESSAGE]`, `/help` | |

## Modem / DreamPi

Set up and test DreamPi with another app first. A BBA or W5500 is always
preferred; the modem is only used when no Ethernet adapter is found.

- Dials at startup with the console's saved PlanetWeb ISP settings, then
  DreamPassport, then the DreamPi defaults (`555`, `dream` / `cast`). Tone
  dialing only
- Dialing can take ~65 seconds before timing out; progress shows on the server
  page
- Carrier loss triggers an automatic redial and channel rejoin. `/reconnect` or
  A+B hangs up and redials

## Build and run

Modem support needs the repository's [KOS patches](../../../tools/kos/README.md)
(PPP lifecycle in particular) applied once to your KOS tree, then a rebuild
(CI does this). From this directory:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
../../../tools/kos/apply-patches.sh && make -C "$KOS_BASE" -j4
make
./run-flycast.sh            # BBA over Flycast's picoTCP proxy; --skip-build works
./run-flycast.sh --modem    # Flycast's modem emulation, not a physical DreamPi
```

The launcher puts a keyboard on port B fed by the host keyboard and leaves the
saved Flycast configuration alone. `KOS_ENV` and `FLYCAST_BIN` override the
default paths.

## Tests

`tests/run_network.py` builds a temporary ELF, runs a private IRC fixture
server and drives Flycast against it; no public IRC network is contacted. Run
from the repository root with the Mac's LAN IPv4 address (`--flycast` and
`--kos-env` override tool paths).

| Command | Checks |
| --- | --- |
| `python3 projects/apps/dreamcast-irc/tests/run_network.py --host <Mac-LAN-IP> --log-dir <dir>` | BBA: register, join, chat, recover from a server-side drop and from `/reconnect`, clean shutdown |
| `... --modem --host <Mac-LAN-IP> --log-dir <dir>` | The same over emulated PPP, plus a forced carrier loss and automatic redial |
| `... --modem --cancel-dial --host <Mac-LAN-IP> --log-dir <dir>` | Cancelling mid-dial shuts down cleanly |

`--log-dir` receives `build.log`, `flycast.log` and `result.txt`.

## Known limitations

- Plain-text IRC only: no TLS, SASL or NickServ. Do not send passwords through
  it
- Incoming UTF-8 is mapped to the BIOS font's ISO-8859-1; anything outside it
  shows as `?`
- Not yet tested on a real Dreamcast with DreamPi
- One network at a time, up to ten channels

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
