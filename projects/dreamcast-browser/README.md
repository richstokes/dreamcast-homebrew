# Dreamcast Browser

A small, deliberately limited web browser for Sega Dreamcast, built with
KallistiOS. It supports secure HTTP/HTTPS, a Dreamcast keyboard and mouse,
controller fallback, flowing and word-wrapped HTML text, links, headings,
emphasis, code, lists, preformatted text, and PNG/JPEG/GIF-style raster images
through `stb_image`. JavaScript is neither downloaded as executable code nor
run.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.elf)
is also available.

## Controls

- `F6` or `Ctrl+L`: open the address bar
- `Enter`: open the typed address or focused link
- `Backspace` or `Alt+Left`: return to the previous page
- `Shift+Backspace` or `Alt+Right`: move forward again
- `Esc`: cancel address editing or an active load; otherwise exit
- `Tab`: focus the next link
- arrows, Page Up/Down, Home/End, Space: scroll
- `F5`: reload
- mouse: point, click Back/Forward, links, or the address bar; use the wheel to
  scroll and right-click to cancel an active load
- controller: `B` or left trigger back, right trigger forward, `X` address bar,
  `Y` next link, `A` open, D-pad scroll, Start exit; `B` or Start cancels an
  active load

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

To exercise live HTTPS navigation, Back/Forward, and scroll restoration
automatically, build the serial-console regression variant:

```sh
make clean
make CPPFLAGS=-DBROWSER_HISTORY_SELF_TEST
./run-flycast.sh --skip-build
```

The test reports passing inline layout/style/reflow, asset, page-cancel,
history, and image-cancel checks before entering the normal browser loop. Run
`make clean && make` afterward to restore the release build.

For a cold-boot compatibility check against another homepage, override the URL
for that build only (the release default remains `https://appsbyrich.com/`):

```sh
make clean
make CPPFLAGS='-DBROWSER_HOME_URL=\"https://news.ycombinator.com/\"'
./run-flycast.sh --skip-build
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
`noscript` blocks and HTML comments are skipped. Decorative images are omitted,
and declared image dimensions keep small failed assets from becoming giant
placeholders. There is no CSS layout, JavaScript, forms, cookies, storage,
audio/video, downloads, tabs, or full Unicode font rendering.
Supported inline text keeps flowing across semantic tags instead of forcing a
new row, wraps at the display edge, and gives links, strong text, emphasis, and
code distinct colors. Lists keep their marker and text together, including when
wrapper elements are present, and legacy table rows degrade into readable text
rows with separated cells.

Oversized HTML is downloaded only to the fixed 512 KiB ceiling, then parsed as
a clearly marked shortened page. Oversized images are rejected from their HTTP
headers before the body is downloaded. Next.js image-optimizer URLs are reduced
to the Dreamcast's 640-pixel display width, image requests time out after eight
seconds, and receive bursts are bounded for BBA stability. Parsed page text is
shown before image downloads begin. If any asset is oversized, unsupported,
unreachable, or times out, it and all remaining page images become alt-text
placeholders without further network retries. The deliberately small 24 KiB
ceiling avoids a known Flycast BBA failure that can occur before
application-level size checks run; small web graphics still render normally.
Image downloads require a safe, declared size from a header-only probe;
unknown-length images become placeholders. The BBA IRQ is gated and a normal
KOS worker polls the adapter, avoiding a Flycast IRQ9 re-entry that otherwise
appears as a KOS double-fault panic.

While a request is active, the header shows an animated connection/download
state, transferred versus declared KiB, and the cancel controls. `Esc`,
controller `B`/Start, or right-click aborts the transfer. Canceling a page load
preserves the displayed page, URL, scroll position, and Back/Forward history;
canceling image loading keeps the parsed page and turns remaining assets into
placeholders.

This is a readable-web and small-site browser, not a modern desktop engine.
Back and Forward history each retain up to eight URLs and their scroll positions
in fixed-size buffers. Opening a new page after going Back clears the Forward
history, matching conventional browser behavior.
