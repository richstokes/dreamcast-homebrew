# Dreamcast Browser

A small, deliberately limited web browser for Sega Dreamcast, built with
KallistiOS. It supports secure HTTP/HTTPS, a Dreamcast keyboard and mouse,
controller fallback, basic HTML text/links, and PNG/JPEG/GIF-style raster
images through `stb_image`. JavaScript is neither downloaded as executable code
nor run.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.elf)
is also available.

## Controls

- `F6` or `Ctrl+L`: open the address bar
- `Enter`: open the typed address or focused link
- `Backspace` or `Alt+Left`: return to the previous page
- `Esc`: cancel address editing; outside the address bar it exits
- `Tab`: focus the next link
- arrows, Page Up/Down, Home/End, Space: scroll
- `F5`: reload
- mouse: point, click Back, links, or the address bar; use the wheel to scroll
- controller: `B` back, `X` address bar, `Y` next link, `A` open, D-pad scroll,
  Start exit

The address bar defaults to HTTPS when no scheme is entered. HTTPS verifies
the server certificate and hostname with the bundled Mozilla CA store and will
not negotiate a protocol older than TLS 1.2.

## Build dependencies

The project uses the official KOS ports for `curl`, `mbedtls`, `zlib`, and
`stb_image`. The KOS 2.3/ports versions used here need two small compatibility
fixes: enabling KOS's `/dev/urandom` for mbedTLS, and compiling the C-compatible
stb implementation without an optional SH-4 G++ binary. Apply the included
patch, then install the ports from a sourced KOS environment:

```sh
git -C "$KOS_PORTS" apply \
  /absolute/path/to/dreamcast-browser/patches/kos-ports-kos23.patch
make -C "$KOS_PORTS/mbedtls" force-install
make -C "$KOS_PORTS/curl" install
make -C "$KOS_PORTS/stb_image" install
```

If a port is already current, its normal `install` target exits without
rebuilding; use `force-install` after changing its configuration.

Refresh the CA bundle occasionally (this needs host internet access):

```sh
make update-cacert
```

Build and inspect the target:

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
file dreamcast-browser.elf
sh-elf-readelf -h dreamcast-browser.elf
```

Run in Flycast with BBA emulation and the serial console:

```sh
./run-flycast.sh
```

Flycast's `DCNet=no` picoTCP proxy permits the browser to make outbound
connections. It is not bridged networking. The launcher transiently attaches
an emulated Dreamcast keyboard on Maple port A, mouse on port B, and controller
on port C; it does not overwrite the devices in Flycast's saved configuration.
Click inside the Flycast window once if macOS has not given it input focus.

## Deliberate limits and graceful degradation

Dreamcast has 16 MB of main RAM, so the browser places hard bounds on remote
content: 512 KiB per HTML response, 24 KiB compressed per image, 64 KiB of
compressed images in total, six image slots,
512 layout items, and 96 links. Large documents are shortened. Oversized,
failed, or unsupported images become placeholders. Unknown HTML tags are
ignored while their text remains visible; scripts, styles, SVG, canvas, and
`noscript` blocks are skipped. There is no CSS layout, JavaScript, forms,
cookies, storage, audio/video, downloads, tabs, or full Unicode font rendering.

Known oversized responses are rejected from their HTTP headers before the body
is downloaded. Next.js image-optimizer URLs are reduced to the Dreamcast's
640-pixel display width, image requests time out after eight seconds, and receive
bursts are bounded for BBA stability. Parsed page text is shown before image
downloads begin. If any asset is oversized, unsupported, unreachable, or times
out, it and all remaining page images become alt-text placeholders without
further network retries. The deliberately small 24 KiB ceiling avoids a known
Flycast BBA failure that can occur before application-level size checks run;
small web graphics still render normally. BBA IRQs are gated after each request
and the adapter is polled from the idle UI loop, avoiding a Flycast IRQ9
re-entry that otherwise appears as a KOS double-fault panic.

While a request is active, the header shows an animated connection/download
state and transferred versus declared KiB instead of leaving a static startup
message onscreen.

This is a readable-web and small-site browser, not a modern desktop engine.
Navigation history retains the eight most recent URLs and their scroll
positions in a fixed-size buffer.
