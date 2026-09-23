# Dreamcast Browser

A small, deliberately limited web browser for Sega Dreamcast, built with
KallistiOS. It supports secure HTTP/HTTPS, a Dreamcast keyboard, mouse, or
controller alone (with an on-screen keyboard), search from the address bar,
bookmarks saved to a VMU, basic forms, flowing and word-wrapped HTML text,
links, headings, emphasis, code, lists, preformatted text, and PNG/JPEG/GIF-style
raster images through `stb_image`. JavaScript is neither downloaded as
executable code nor run.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.elf)
is also available.

## Controls

- `F1` or `?`: show every keyboard shortcut; any key closes the list
- `F6` or `Ctrl+L`: open the address bar with the current URL selected
- `Ctrl+B`: hide or show the address bar and status line, giving the page the
  full screen; the bar returns while editing an address or loading a page
- `Ctrl+D`: bookmark this page; `Ctrl+Shift+B`: open or close the bookmarks page
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
  the start; the insertion point is drawn in place. `Shift+Enter` starts a new
  line in a text area; in a drop-down, arrows or a letter choose an option
- mouse: point, click Back/Forward, links, or the address bar; use the wheel to
  scroll and right-click to cancel an active load. Without a keyboard attached,
  clicking a text box opens the on-screen keyboard
- controller: `A` open or edit, `B` or left trigger back, right trigger
  forward, `X` address bar, `Y` next link, D-pad scroll, Start for bookmarks and
  the menu (which also has Exit); `B` or Start cancels an active load
- on-screen keyboard: D-pad picks a key, `A` types it, `B` deletes, `X` types a
  space, `Y` is Shift (for one character), the triggers move the cursor, and
  Start finishes. A controller button during keyboard editing brings it up

The address bar defaults to HTTPS when no scheme is entered. Text that is not an
address, such as words with spaces or a name without a domain, is searched with
DuckDuckGo Lite; start with `?` to force a search. Build with
`CPPFLAGS='-DBROWSER_SEARCH_URL=\"...\"'` to use another engine. HTTPS verifies
the server certificate and hostname with the bundled Mozilla CA store and will
not negotiate a protocol older than TLS 1.2.

## Bookmarks

Start or `Ctrl+Shift+B` opens the bookmarks page, which offers to bookmark the page
you came from and lists up to sixteen bookmarks, each with a remove link.
Bookmarks are saved to the first VMU found as `DCBROWSE.BMK` and reloaded at
startup; without a VMU they last until the browser exits. The page's actions
work only on the page itself, so a web page cannot link to them.

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

The installed KOS revision also needs the keyboard-attachment fix before
building. It corrects a pointer calculation that otherwise clears memory
outside the keyboard state:

```sh
./scripts/fix-kos-keyboard.sh
```

The script is safe to rerun, accepts `KOS_ENV`, and rebuilds only the keyboard
object and SDK archives, preserving local networking changes. Existing ELFs
need relinking afterward; this project's Makefile tracks the SDK archive.
If a newer SDK has already fixed the bug differently, the script stops without
changing it.

Refresh the CA bundle occasionally (this needs host internet access):

```sh
make update-cacert
```

## Build

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
```

The HTML parser, text decoding, on-screen keyboard, address bar, and bookmark
record run as host unit tests with the Mac or Linux C compiler, no KallistiOS
needed. They include a randomized parser robustness pass, and CI runs them:

```sh
make -C tests
```

Self-test builds are available with `make CPPFLAGS=-DBROWSER_HISTORY_SELF_TEST`
and `-DBROWSER_FORM_SELF_TEST`; results are printed to the serial console. The
history self-test also covers keyboard, on-screen keyboard, form control,
bookmark, and VMU behavior (restoring any existing bookmark file), checks that
incremental frames match full redraws pixel for pixel, and prints a render
benchmark. Adding `-DBROWSER_PROFILE` prints aggregate main-loop timings (frame wait,
Maple poll, interrupt-to-wake delay, input, draw, present) every two seconds,
without logging typed characters or URLs. Adding `-DBROWSER_FRAME_DUMP` prints a frame over serial;
`scripts/frames-to-png.py <log> <directory>` turns it into a PNG. Run
`make clean && make` afterward to restore the release build, and never
distribute a form self-test build because it embeds test credentials.

## Run in Flycast

```sh
./run-flycast.sh
```

The launcher attaches an emulated Dreamcast keyboard, mouse, and controller
without changing Flycast's saved configuration. Click inside the Flycast window
once if macOS has not given it input focus.

## Frame loop and rendering

The main loop sleeps until the vertical blank interrupt, waits for that
frame's Maple poll to finish so key presses are read the frame they happen,
handles input, and composes the frame in main RAM. Only the rows that changed
since the previous frame are redrawn and copied to the back buffer (a line
scroll shifts the page in RAM and draws just the exposed band), then the
buffers are flipped. Nothing is drawn into a buffer until the vertical blank
after it was flipped away from, so frames never tear.

The vblank interrupt resumes a waiting frame immediately, avoiding an extra
scheduler tick. Relative mouse motion is consumed once per Maple report,
including frames where device discovery skips a poll. Drawing skips layers
outside the changed rows, so typing in the address bar never traverses the
page underneath it.

The launcher enables `config:rend.EmulateFramebuffer=yes` for this CPU-rendered
application. Flycast then refreshes idle pages regularly and displays buffer
flips without extra guest VRAM writes. This is a transient emulator option;
real Dreamcast hardware uses the same ordinary double-buffered renderer.

## Profiling and input regression checks

```sh
./scripts/profile.sh /tmp/browser-profile.log
python3 scripts/analyze-profile.py /tmp/browser-profile.log
```

The profile build starts on the local bookmarks page, checks keyboard editing
and mouse report consumption, compares incremental rendering with full redraws,
and runs a fixed 320-item render benchmark. No remote test server is required.
`--expect-keys N` on the analyzer checks the total key events in completed
reporting windows (include shortcut keys in N, and wait for the last two-second
report before exiting). The log reports guest microseconds; idle frame counts
are not FPS measurements. Full and incremental render benchmarks both measure
composition only, excluding video copies and display waits.

After profiling, source KOS and run `make clean && make` to restore the normal
home page and release build. See [performance notes](PERFORMANCE.md) for the
measured results and the Flycast keyboard fix.

## Limits

This is a readable-web and small-site browser, not a modern desktop engine.
There is no CSS layout, JavaScript, storage beyond VMU bookmarks, audio/video,
downloads, or tabs. Text uses the BIOS font's ISO-8859-1 characters: pages in
UTF-8 or windows-1252 (from the HTTP header, a `<meta>` tag, or detection) are
decoded, and typographic punctuation such as curly quotes, dashes, and
ellipses becomes its ASCII equivalent. Other characters show as `?`.

The Dreamcast has 16 MB of main RAM, so remote content is bounded: 512 KiB per
HTML page, 24 KiB per image, six images per page, and 96 links. Larger pages
are shortened, and oversized, failed, or unsupported images become alt-text
placeholders. Insecure images on HTTPS pages are requested over HTTPS instead.
Back and Forward each remember up to eight pages.

Any active load can be canceled with `Esc`, controller `B`/Start, or
right-click, keeping the current page.

## Forms

Same-origin HTTPS forms support text-like inputs (text, email, password,
search, URL, number, date, and similar types, all typed as text), hidden
inputs, checkboxes, radio buttons, drop-down lists, text areas, and submit
buttons, including `<button>` and image buttons. File uploads are not
supported, so a form containing one is blocked. Focus a field with Tab and
Enter, type, then press Enter to finish or Tab to move on; Escape restores the
previous value. Values are submitted as UTF-8. Passwords display as asterisks,
and cookies are kept in RAM only. Registration, login, and logout on
https://dcvmu.com work.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
