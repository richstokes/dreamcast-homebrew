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

- `F1` or `?`: show every keyboard shortcut; any key closes the list
- `F6` or `Ctrl+L`: open the address bar with the current URL selected
- `Enter`: open the typed address, or open, edit or toggle the focused control
- `Backspace` or `Alt+Left`: return to the previous page
- `Shift+Backspace` or `Alt+Right`: move forward again
- `Alt+Home`: return to the home page
- `Esc`: cancel address or field editing, cancel an active load, or clear link
  focus; with nothing left to cancel, a second, separate `Esc` exits
- `Tab` and `Shift+Tab`: focus the next or previous link or form control,
  scrolling it into view; after scrolling away, focus resumes from what is on
  screen rather than from the link that was left behind
- arrows, Page Up/Down, Home/End, Space, `Shift+Space`: scroll
- `F5` or `Ctrl+R`: reload, keeping the scroll position
- while editing the address bar or a form field: Left/Right (with `Ctrl` for
  whole words), Home/End, Delete, `Ctrl+A` to select all, `Ctrl+W` or
  `Ctrl+Backspace` to delete the previous word, and `Ctrl+U` to delete back to
  the start; the insertion point is drawn in place
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
`stb_image`. Apply the included compatibility patch, then install the ports
from a sourced KOS environment:

```sh
git -C "$KOS_PORTS" apply \
  /absolute/path/to/dreamcast-browser/patches/kos-ports-kos23.patch
make -C "$KOS_PORTS/mbedtls" force-install
make -C "$KOS_PORTS/curl" install
make -C "$KOS_PORTS/stb_image" install
```

Refresh the CA bundle occasionally (this needs host internet access):

```sh
make update-cacert
```

## Build

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

Self-test builds are available with `make CPPFLAGS=-DBROWSER_HISTORY_SELF_TEST`
and `-DBROWSER_FORM_SELF_TEST`; results are printed to the serial console. Run
`make clean && make` afterward to restore the release build, and never
distribute a form self-test build because it embeds test credentials.

## Run in Flycast

```sh
./run-flycast.sh
```

The launcher attaches an emulated Dreamcast keyboard, mouse, and controller
without changing Flycast's saved configuration. Click inside the Flycast window
once if macOS has not given it input focus.

## Limits

This is a readable-web and small-site browser, not a modern desktop engine.
There is no CSS layout, JavaScript, persistent storage, audio/video, downloads,
tabs, or full Unicode font rendering.

The Dreamcast has 16 MB of main RAM, so remote content is bounded: 512 KiB per
HTML page, 24 KiB per image, six images per page, and 96 links. Larger pages
are shortened, and oversized, failed, or unsupported images become alt-text
placeholders. Back and Forward each remember up to eight pages.

Any active load can be canceled with `Esc`, controller `B`/Start, or
right-click, keeping the current page.

## Forms

Same-origin HTTPS forms support text, email, password, hidden, checkbox, and
submit inputs. Focus a field with Tab and Enter, type, then press Enter to
finish or Tab to move on; Escape restores the previous value. Passwords display
as asterisks, and cookies are kept in RAM only. Registration, login, and logout
on https://dcvmu.com work.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
