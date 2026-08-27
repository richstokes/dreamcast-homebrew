# Dreamcast IRC

Dreamcast IRC is a small KallistiOS client that connects over the Broadband
Adapter to `irc.libera.chat:6667` and automatically joins `#netsplit`. It
renders a scrollable chat view with the Dreamcast BIOS font and accepts text
from a Dreamcast keyboard. A permanent server page is followed by up to ten
channel pages, each backed by its own 96-line fixed-size scrollback ring.

The client implements the essential IRC pieces needed for an interactive
channel session: registration, automatic nickname collision recovery,
`PING`/`PONG`, channel listing, multiple channel joins, messages, actions,
notices, topics, names, and
common join/part/quit/nick/kick events. Direct CTCP `VERSION` requests receive
a `DCIRC` reply with Dreamcast hardware and KallistiOS system details. Its
default nickname is `DCIRC_` followed by a random three-digit number; a new
number is generated if that nickname is already in use.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-irc.elf)
is also available.

## Build

Source the KallistiOS environment and build with the SH-4 toolchain:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

This produces `dreamcast-irc.elf`, which Flycast can boot directly.

## Run in Flycast

```sh
./run-flycast.sh
```

The launcher makes transient Flycast settings only. It enables the emulated
BBA with Flycast's local picoTCP proxy, keeps a controller on port A, adds a
Dreamcast keyboard on port B, forwards host keyboard input, and enables serial
diagnostics in the launching terminal. It does not modify the saved Flycast
configuration.

Flycast's picoTCP backend gives the Dreamcast outbound Internet access, which
is all this client needs. It is not a bridged LAN connection. On real hardware,
boot the ELF using the normal loader and provide a working BBA network setup.

## Controls

- Type with a Dreamcast keyboard and press Enter to send.
- D-pad or keyboard Left/Right switches between the server and channel pages.
- D-pad Up/Down or keyboard Up/Down/Page Up/Page Down scrolls the current page.
- X+Y returns to the newest messages.
- A+B reconnects to Libera.Chat.
- Start quits cleanly.

Available commands are `/join #CHANNEL[,#CHANNEL]`,
`/part [#CHANNEL] [MESSAGE]`, `/list [FILTER]`, `/me ACTION`, `/nick NAME`,
`/clear`, `/reconnect`, `/quit [MESSAGE]`, and `/help`. `/clear` affects only
the current page. `/list` displays at most 80 entries on the server page so a
large network channel list cannot displace unbounded amounts of memory.

## Network and security notes

This basic client uses unencrypted IRC on TCP port 6667 and does not implement
SASL or NickServ authentication. Do not send passwords or other sensitive
information through it. A TLS-capable client would need a TLS library and a
trusted certificate store in addition to this project's current KOS-only
dependencies.

Incoming UTF-8 is converted to the BIOS font's ISO-8859-1 character set;
characters outside that set appear as `?`.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
