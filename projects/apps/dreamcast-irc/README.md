# Dreamcast IRC

Dreamcast IRC is a small KallistiOS client that connects over the Broadband
Adapter. By default it connects to `irc.libera.chat` and automatically joins
`#dreamcastdev`, trying plain-text port 6667 first and port 8000 as a fallback. It
renders a scrollable chat view with the Dreamcast BIOS font and accepts text
from a Dreamcast keyboard. A permanent server page is followed by up to ten
channel pages, each backed by its own 96-line fixed-size scrollback ring.

The client implements the essential IRC pieces needed for an interactive
channel session: registration, automatic nickname collision recovery,
`PING`/`PONG`, channel listing, multiple channel joins, messages, actions,
notices, topics, names, and
common join/part/quit/nick/kick events. Direct CTCP `VERSION` requests receive
a `DCIRC` reply with Dreamcast hardware and KallistiOS system details. CTCP
`CLIENTINFO` replies identify the supported requests and link to this project's
source repository. Its
default nickname is `DCIRC_` followed by a random three-digit number; a new
number is generated if that nickname is already in use.

DCIRC stores its active profile in `DCIRC_CFG` on the first available VMU,
preferring the same VMU on later saves. The three-block, BIOS-visible file
contains the last accepted nickname, IRC server and port choice, and up to ten
open channel names. It is updated shortly after confirmed nickname and channel
changes and after `/server` switches. On the next launch, DCIRC loads that
profile before connecting and automatically rejoins its saved channels. If no
VMU or valid profile is available, it safely falls back to a random nickname,
`irc.libera.chat`, and `#dreamcastdev`. Invalid, incompatible, or corrupted
profiles are ignored; an additional application CRC protects the versioned,
fixed-size record in addition to the standard VMU package CRC.

Connection setup is nonblocking after DNS resolution. Each resolved IPv4
endpoint has a fixed connection deadline, IRC registration and channel joins
have their own deadlines, and failed connections retry automatically with
bounded exponential backoff. A fixed-size outgoing queue prevents a stalled
network from blocking the UI. Fatal connection errors are shown on both the
server page and the active channel page; channel-specific join errors stay on
the affected channel page.

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
- A+B reconnects to the current IRC network.
- Start quits cleanly.

Available commands are `/server HOST [PORT]`, `/join #CHANNEL[,#CHANNEL]`,
`/part [#CHANNEL] [MESSAGE]`, `/list [#CHANNEL|#MASK*]`, `/me ACTION`, `/nick NAME`,
`/clear`, `/reconnect`, `/quit [MESSAGE]`, and `/help`. `/clear` affects only
the current page. `/server` disconnects the active network, clears its channel
pages, and connects to the new host; only one network is held in memory at a
time. Supplying a port tries only that port, while omitting it tries 6667 and
then 8000. Channels are not carried between networks, so use `/join` after the
new connection registers. The new server and its subsequently joined channels
become the saved launch profile. Bare `/list` safely requests channels with
more than 500 users; exact-channel and wildcard-mask lookups are also supported.
Results are capped at 80 on the server page, after which DCIRC cancels the
server stream.
Framebuffer rendering pauses briefly while LIST data is arriving to avoid a
KOS BIOS-font/BBA interrupt conflict.

## Network and security notes

This basic client uses unencrypted IRC and does not implement TLS, SASL, or
NickServ authentication. A custom `/server` port must therefore accept plain
IRC. Do not send passwords or other sensitive information through it. A
TLS-capable client would need a TLS library and a trusted certificate store in
addition to this project's current KOS-only dependencies.

Incoming UTF-8 is converted to the BIOS font's ISO-8859-1 character set;
characters outside that set appear as `?`.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
