# Dreamcast Browser

A small, deliberately limited web browser for Sega Dreamcast, built with
KallistiOS. It supports secure HTTP/HTTPS over a Broadband Adapter or a modem
(DreamPi), a Dreamcast keyboard, mouse, or controller alone (with an on-screen
keyboard), search from the address bar, bookmarks saved to a VMU, basic forms,
flowing and word-wrapped HTML text, links, headings, emphasis, code, lists,
data tables, preformatted text, and PNG/JPEG/GIF-style raster images through
`stb_image`. JavaScript is neither downloaded as executable code nor run.

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
- `F7`: switch between reader and full-page views
- `F4`: load this page's images; text is usable immediately without waiting for them
- `Ctrl+Enter` while editing a field: submit its form, including search forms with no button
- while editing the address bar or a form field: Left/Right (with `Ctrl` for
  whole words), Home/End, Delete, `Ctrl+A` to select all, `Ctrl+W` or
  `Ctrl+Backspace` to delete the previous word, and `Ctrl+U` to delete back to
  the start; the insertion point is drawn in place. `Shift+Enter` starts a new
  line in a text area; in a drop-down, arrows or a letter choose an option
- mouse: point, click Back/Forward, links, or the address bar; use the wheel to
  scroll and right-click to cancel an active load. Without a keyboard attached,
  clicking a text box opens the on-screen keyboard
- controller: the analog stick moves the pointer, slowly for a small push and
  quickly for a full one, and scrolls the page when held against its top or
  bottom edge. `A` presses whatever the pointer is on (a link, a form control,
  Back, Forward, the address bar or Go), `B` or left trigger back, right
  trigger forward, `X` address bar, `Y` next link (`A` then opens that link),
  D-pad scroll, Start for bookmarks and the menu (which also has Exit); `B`
  cancels an active load. Hold `Y` and press the left trigger for
  reader/full-page view or the right trigger for images
- on-screen keyboard: the D-pad or the pointer picks a key, `A` types it, `B`
  deletes, `X` types a space, `Y` is Shift (for one character), the triggers
  move the cursor, and Start finishes. A controller button during keyboard
  editing brings it up

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

The checked-out SDK also needs the TCP poll lock-order repair. A receive
notification can otherwise deadlock with polling and socket cleanup after
several page loads, leaving even cancellation waiting forever:

```sh
./scripts/fix-kos-tcp-poll.sh
```

This script moves poll notifications outside both TCP locks, rebuilds only
the TCP object and SDK archives, and preserves the other local SDK changes.
It is safe to rerun and stops without changes if the source does not match.

Apply the closed-socket repair after it:

```sh
./scripts/fix-kos-tcp-orphans.sh
```

Without it, a connection closed while the server is still sending (a
canceled load, or an image over the size limit) is never reset. Its socket
and both 64 KiB buffers are then kept for good, and after several of them
transfers stop altogether. The repair resets such a connection, as RFC 9293
asks, and gives every closed socket thirty seconds to finish closing.

These repairs have been offered to KallistiOS, and each local patch and
script can be removed once its fix is in the SDK being used:

| Local patch | Upstream |
| --- | --- |
| `kos-keyboard-attach.patch` | [KallistiOS#1537](https://github.com/KallistiOS/KallistiOS/pull/1537) (the same fix, from another contributor) |
| `kos-tcp-poll-lock.patch` | [KallistiOS#1550](https://github.com/KallistiOS/KallistiOS/pull/1550) |
| `kos-tcp-orphan-reset.patch` | [KallistiOS#1551](https://github.com/KallistiOS/KallistiOS/pull/1551) |

Relink the browser after applying any SDK repair. The modem also needs
[DCVMU's PPP lifecycle patch](../../dcvmu.com/client/patches/kos-ppp-lifecycle.patch)
and links `libppp` from the SDK's add-ons. Release CI applies all of these
after the shared DCVMU networking patches, before building its pinned SDK.

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
needed. The image loader and the page cache are tested the same way. They
include a randomized parser robustness pass, and CI runs them:

```sh
make -C tests
```

Self-test builds are available with `make CPPFLAGS=-DBROWSER_HISTORY_SELF_TEST`
and `-DBROWSER_FORM_SELF_TEST`; results are printed to the serial console. The
history self-test also covers keyboard, controller, on-screen keyboard, form
control, bookmark, and VMU behavior (restoring any existing bookmark file),
checks that incremental frames match full redraws pixel for pixel, and prints
a render benchmark. `-DBROWSER_SITE_SELF_TEST` visits Wikipedia, Hacker News,
BBC News and DuckDuckGo and reports how long each step took; those sites
change, so it is a survey, not a regression test. Adding
`-DBROWSER_EXIT_AFTER_TESTS` exits once the tests have run, which exercises
the shutdown path. Adding `-DBROWSER_PROFILE` prints aggregate main-loop timings (frame wait,
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

`BROWSER_NETWORK=modem ./run-flycast.sh` removes the emulated Broadband
Adapter, so the browser dials through Flycast's emulated modem instead. For
scripted runs, `BROWSER_EXIT_ON_LOG` and `BROWSER_FAIL_ON_LOG` name serial
console text that ends the run as a pass or a failure, and
`BROWSER_TEST_SECONDS` (default 300) bounds it.

## Dial-up and DreamPi

A Broadband Adapter or LAN adapter is always preferred. Without one, opening
a page dials the modem first, using the phone number and PPP login from the
console's saved **PlanetWeb** profile, then **DreamPassport**, then the DreamPi
defaults (`555`, `dream` / `cast`). Flash memory is only read. Only tone
dialing of a plain number is supported.

Dialing and signing in can take a minute; the status line shows each step and
`Esc` or `B` cancels once the step in progress returns. A failure is
explained on the page, and `F5` tries again. If the call drops, the next page
redials. Transfers are given several times longer before they time out.
This path is tested with Flycast's emulated modem; a real modem and DreamPi
have not been tested.

## Source layout

| File | What it holds |
| --- | --- |
| `dreamcast-browser.c` | The browser's state, the frame loop, start and finish |
| `loader.c` | The loading state machine, history, kept pages and internal pages |
| `input.c` | Keyboard, mouse, controller, on-screen keyboard, editing, focus |
| `forms.c` | Following links and submitting forms |
| `net.c`, `modem.c` | The HTTP worker and the dial-up connection |
| `html.c`, `images.c`, `cache.c` | Parsing and layout, image loading, kept pages |
| `render.c` | Drawing |
| `osk.c`, `address.c`, `bookmarks.c`, `storage.c` | On-screen keyboard layout, address bar, bookmarks, VMU |
| `selftest.c`, `tests/*.inc` | Self-tests; empty in a release build |

`app.h` declares what the first four share; `browser.h` declares the rest.

## Frame loop and rendering

The main loop sleeps until the vertical blank interrupt, waits for that
frame's Maple poll to finish so key presses are read the frame they happen,
handles input, advances whatever is loading, and composes the frame in main
RAM. Loading is a state machine with four phases (idle, dialing, page,
images): a transfer is started, the loop carries on, and the phase ends when
the network worker reports back. Nothing in the loop waits for the network,
and input is only ever handled from the loop itself. Only the rows that changed
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

The Dreamcast has 16 MB of main RAM, so remote content stays bounded: 2 MiB per
HTML response, 12,288 layout items, 4,096 links and 2,048 section targets. Pages
show notices when a limit omits content or controls. Referenced sections and
headings take precedence over incidental HTML IDs. A page's text and link
addresses are stored at their own length, so a page uses the memory it needs
and not the most a page could: the Wikipedia article on the Dreamcast takes
about 620 KiB laid out, where every page used to take 2.4 MiB.

Back and Forward each remember up to eight pages. The HTML of the last eight
pages shown is kept, within 3 MiB, so going back to one of them lays it out
again without asking the network; older pages are downloaded again. Kept
pages are the first thing released when memory runs short. Following a link
and `F5` always download a fresh copy.

Requests to one site share a connection and its TLS session, so only the
first pays for a handshake.

Images are optional (`F4`), with at most twelve slots, 192 KiB per download and
768 KiB per page. Each request gets at most fifteen seconds, within a
forty-five-second page budget. An image larger than the limit is refused from
its headers when the server declares its size, and cut off at the limit when it
does not. The decoder rejects sources above 1,024 pixels in either dimension
or 524,288 total pixels before allocating the bitmap, and a page may keep
1,310,720 decoded pixels in all. Failed, oversized and unsupported images keep
placeholders while later images are still attempted; cancellation, memory
exhaustion or the shared budget stops the batch. Reloading allows another
attempt. Insecure images on HTTPS pages use HTTPS instead.

The address comes from `src`, or from `data-src`, `data-lazy-src` or
`data-original` when `src` is missing or a placeholder. `srcset` supplies the
narrowest candidate at least as wide as the page. Inside `<picture>`, a JPEG,
PNG or GIF `<source>` replaces an image in a format that cannot be decoded,
and a converted copy named after its original (`photo.jpg.webp`) is requested
as the original. WebP, AVIF and SVG themselves are not decoded.

Any active load can be canceled with `Esc`, controller `B`, or right-click,
keeping the current page. HTTP/TLS and image decoding run on one worker thread
while the main thread continues handling keyboard, mouse, controller and
rendering. You can scroll the previous page, edit an address or replace a
pending navigation. Completion preserves a new address draft; canceled
navigation does not alter history. Each image appears as it arrives, and the
text being read keeps its place. Laying out a page still runs on the main
thread once its download has finished; in Flycast a 1.2 MiB page takes about
a second and a half.

## Reading pages

Reader view is preferred when a page identifies a main/article body. It uses
HTML semantics and common CMS content markers, hides navigation and sidebars,
and collapses excessive nested block spacing. `F7` restores the full view from
the retained HTML without downloading it again; it also restores infobox/specs
tables omitted by reader view. This is a heuristic, so use full view if useful
content is missing. HTML `hidden` and inline `display:none` are honored in both
views, while hidden form values remain available for submission. Local section
links scroll directly rather than fetching the page again.

A table with headings, a caption or a border is laid out as data: each row is
a line, with a bar between its cells. Tables that only position a page flow as
ordinary text.

## Forms

HTTPS forms support text-like inputs (text, email, password,
search, URL, number, date, and similar types, all typed as text), hidden
inputs, checkboxes, radio buttons, drop-down lists, text areas, and submit
buttons, including `<button>` and image buttons. File uploads are not
supported, so a form containing one is blocked. Focus a field with Tab and
Enter, type, then press Enter to finish or Tab to move on; Escape restores the
previous value. `Ctrl+Enter` submits from a field, using the first enabled submit
button if one exists. GET search forms may navigate to another HTTPS site;
POST must stay on the same origin, and password fields cannot use GET.
A form answered with a redirect is followed as browsers follow it: 301, 302
and 303 are fetched with GET and without the form data, which is how most
sign-in pages respond; 307 and 308 send the data again, and only to the same
site. A redirect to an insecure address is refused.
Values are submitted as UTF-8. Passwords display as asterisks,
and cookies are kept in RAM only. Registration, login, and logout on
https://dcvmu.com work.

## Slow-loading regression fixtures

The fixture server needs only Python's standard library:

```sh
python3 tests/serve_fixtures.py --check
python3 tests/serve_fixtures.py --bind HOST_LAN_IP --delay 2
```

Open `http://HOST_LAN_IP:8765/` in the Dreamcast browser. Use the Mac's LAN
address, not `127.0.0.1`: loopback belongs to the guest. Flycast's local picoTCP
proxy connects outward to the host; this does not enable LAN bridging. The
server exits after fifteen minutes by default (`--duration` changes it), and
never logs query strings or form contents. `/slow` and `/image-page` exercise
streaming, cancellation and editing; `/` covers reader view, section links,
more than 96 links and a valid image after failed images. `/gallery` has
images without a declared size, over the size limit, lazy and responsive
sources and a data table; `/post-redirect/301` to `/308` answer a form with
each kind of redirect; `/stats` counts the connections and requests that
reached the server, which is how connection reuse and kept pages are checked.

For automated guest checks, keep the fixture server running and build:

```sh
make clean
make CPPFLAGS='-DBROWSER_LOADING_SELF_TEST -DBROWSER_PERF_SELF_TEST -DBROWSER_FIXTURE_BASE=\"http://HOST_LAN_IP:8765\"'
./run-flycast.sh --skip-build
```

Look for `LOADING SELF-TEST PASSED` in the serial output, or let the launcher
watch for it:

```sh
BROWSER_EXIT_ON_LOG='LOADING SELF-TEST PASSED' \
BROWSER_FAIL_ON_LOG='SELF-TEST FAILED' ./run-flycast.sh --skip-build
```

Adding `-DBROWSER_LOADING_STRESS=40` first repeats the heaviest traffic the
fixture has that many times. Flycast's network thread is not synchronized
with its CPU thread, and under that traffic roughly one run in four ends in
an emulator crash, a guest "double fault" from an interrupt delivered while
interrupts were masked, or a stall. A release build behaves the same, so
rerun before suspecting the browser.

The test uses fixture data and does not write VMU bookmarks. It ends by
repeating a round of loads twice and comparing the memory in use after each,
which takes about two minutes. A build with `BROWSER_FIXTURE_BASE` may post
form data to that server's `/post` addresses over plain HTTP, so never
distribute one. Restore a release with `make clean && make`.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
