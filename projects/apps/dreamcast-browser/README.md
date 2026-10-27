# Dreamcast Browser

<p align="center">
  <img src="../../../docs/screenshots/dreamcast-browser.png" alt="Dreamcast Browser loading a page with text, links and an image placeholder" width="640">
</p>

A deliberately limited HTTPS browser for the Sega Dreamcast, built with
KallistiOS. It renders word-wrapped text, links, headings, lists, data tables,
basic forms and PNG/JPEG/GIF images over a Broadband Adapter, a WIZnet W5500
serial-port adapter or a modem (DreamPi), driven by a Dreamcast keyboard, a
mouse, or a controller with an on-screen keyboard. JavaScript is never
downloaded or run and there is no CSS layout. It is meant for the readable web
and small sites.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.cdi)**
for Flycast or GDEMU. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser.elf) is also available.
If you are burning a physical CD-R, you may want to try the padded
[CD-R image](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dreamcast-browser-cdr.zip) instead (unzip before burning).

## Controls

| Action | Keyboard | Mouse / controller |
| --- | --- | --- |
| Show every shortcut | `F1` or `?` | |
| Address bar, current URL selected | `F6` or `Ctrl+L` | click it / `X` |
| Open link, press control, Go | `Enter` | click / `A` on whatever the pointer is over |
| Move the pointer | | mouse / analog stick (scrolls when held at the top or bottom edge) |
| Back | `Backspace` or `Alt+Left` | Back button / `B` or left trigger |
| Forward | `Shift+Backspace` or `Alt+Right` | Forward button / right trigger |
| Home page | `Alt+Home` | |
| Scroll | arrows, `Page Up/Down`, `Home/End`, `Space`, `Shift+Space` | wheel / D-pad |
| Next or previous link or field | `Tab`, `Shift+Tab` | `Y` (then `A` opens it) |
| Cancel a load, then clear focus; a second `Esc` with nothing to cancel exits | `Esc` | right-click / `B` |
| Reload, keeping the scroll position | `F5` or `Ctrl+R` | |
| Reader or full-page view | `F7` | hold `Y` + left trigger |
| Load this page's images | `F4` | hold `Y` + right trigger |
| Hide or show the toolbar | `Ctrl+B` | |
| Bookmark this page / bookmarks page | `Ctrl+D` / `Ctrl+Shift+B` | `Start` (also the menu, whose Exit goes to the Dreamcast BIOS menu) |
| Submit the form being edited | `Ctrl+Enter` | |

While editing a field or the address bar: Left/Right (`Ctrl` for words),
Home/End, Delete, `Ctrl+A` selects all, `Ctrl+W` or `Ctrl+Backspace` deletes
the previous word, `Ctrl+U` deletes to the start, `Shift+Enter` starts a new
line in a text area, arrows or a letter pick a drop-down option, and `Esc`
restores the old value. Without a keyboard, clicking a text box (or pressing a
controller button while editing) opens the on-screen keyboard: D-pad or
pointer picks a key, `A` types it, `B` deletes, `X` is space, `Y` is Shift for
one character, the triggers move the cursor and `Start` finishes.

The address bar defaults to HTTPS. Text that is not an address is searched
with DuckDuckGo Lite; start with `?` to force a search. Build with
`CPPFLAGS='-DBROWSER_SEARCH_URL=\"...\"'` to use another engine.

## What it supports

- HTML: flowing text, headings, emphasis, code, lists, preformatted text,
  links and in-page section links, `hidden` and inline `display:none`. A
  table with headings, a caption or a border is laid out as data; layout
  tables flow as text. Reader view hides navigation when a page has a
  main/article body; it is a heuristic, so press `F7` if something is missing
- Text: the BIOS font's ISO-8859-1 characters. UTF-8 and windows-1252 pages
  are decoded, typographic punctuation folds to ASCII, anything else shows as `?`
- Images: PNG, JPEG and GIF through `stb_image`, loaded on demand with `F4`.
  Sources come from `src`, lazy-loading `data-*` attributes, `srcset` and
  `<picture>`; WebP, AVIF and SVG keep a placeholder. Insecure images on
  HTTPS pages are fetched over HTTPS. At most 12 per page, 192 KiB each,
  768 KiB per page and 1,024 pixels per side
- Forms: HTTPS only. Text-like inputs, hidden, checkbox, radio, drop-down,
  text area, submit, `<button>` and image buttons. GET may go to another
  HTTPS site, POST stays on the same origin, password fields never use GET.
  301/302/303 are refetched with GET; 307/308 resend the data to the same
  site only. A file upload blocks its form. Cookies live in RAM. Login and
  logout on https://dcvmu.com work
- Network: HTTP and HTTPS (TLS 1.2 or newer, bundled Mozilla CA store,
  hostname and certificate dates verified, so the console clock must be set). One connection and TLS session per site is reused, and
  any load can be canceled. A Broadband Adapter or W5500 is preferred;
  otherwise the modem dials with the console's saved PlanetWeb or
  DreamPassport profile, falling back to DreamPi defaults (`555`, `dream` /
  `cast`). Tone dialing only, flash is never written. Dialing can take a
  minute; `Esc` or `B` cancels after the current step, `F5` retries, and a
  dropped call redials on the next page
- Bookmarks and history: up to 16 bookmarks in `DCBROWSE.BMK` on the first
  VMU found (RAM only without one). Back and Forward hold 8 pages each, and
  the HTML of the last 8 pages is kept (3 MiB) so going back needs no network
- Page limits: 2 MiB of HTML, 12,288 layout items, 4,096 links and 2,048
  section targets. A notice says when a limit cut something

## Dependencies

The browser links the kos-ports `curl`, `mbedtls`, `zlib` and `stb_image`,
installed in step 5 of the [root README setup](../../../README.md#one-time-setup).
`make update-cacert` refreshes the CA store shared with the other HTTPS
projects, `projects/common/cacert.pem` (needs host internet access).

## Build and run

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
./run-flycast.sh            # or make run, which skips the build
```

The launcher attaches an emulated keyboard, mouse and controller without
changing Flycast's saved settings; click the window once if macOS has not
focused it. `BROWSER_NETWORK=modem` removes the emulated Broadband Adapter so
the browser dials Flycast's modem. For scripted runs, `BROWSER_EXIT_ON_LOG`
and `BROWSER_FAIL_ON_LOG` name serial console text that ends the run as a
pass or a failure, and `BROWSER_TEST_SECONDS` (default 300) bounds it.

## Tests and QA

| Command | Checks |
| --- | --- |
| `make -C tests` | Host unit tests, no KOS needed: parser (with a randomized robustness pass), text decoding, on-screen keyboard, address bar, bookmark record, image loader, page cache, profile analyzer and fixture self-check. CI runs this |
| `make CPPFLAGS=-DBROWSER_HISTORY_SELF_TEST && ./run-flycast.sh --skip-build` | History, keyboard, controller, on-screen keyboard, forms, bookmarks and VMU (an existing bookmark file is restored); incremental frames match full redraws pixel for pixel; render benchmark |
| `make CPPFLAGS=-DBROWSER_FORM_SELF_TEST ...` | Form submission with credentials from `romdisk/test-credentials.txt` |
| `make CPPFLAGS=-DBROWSER_SITE_SELF_TEST ...` | Visits Wikipedia, Hacker News, BBC News and DuckDuckGo with timings. A survey, not a regression test |
| `./scripts/profile.sh LOG` then `python3 scripts/analyze-profile.py LOG` | Main-loop timings and a 320-item render benchmark on the local bookmarks page; `--expect-keys N` checks key delivery. Guest microseconds, and idle frame counts are not FPS |
| Loading self-test (below) | Streaming, cancellation, editing during loads, reader view, section links, images, form redirects, connection reuse, and a two-round leak comparison (about two minutes) |

Measured results, with dates and build context, are in
[PERFORMANCE.md](PERFORMANCE.md).

For the loading self-test, serve the fixtures on the Mac's LAN address, build
against it and let the launcher watch the serial console:

```sh
python3 tests/serve_fixtures.py --bind HOST_LAN_IP --delay 2
make clean
make CPPFLAGS='-DBROWSER_LOADING_SELF_TEST -DBROWSER_PERF_SELF_TEST -DBROWSER_FIXTURE_BASE=\"http://HOST_LAN_IP:8765\"'
BROWSER_EXIT_ON_LOG='LOADING SELF-TEST PASSED' \
BROWSER_FAIL_ON_LOG='SELF-TEST FAILED' ./run-flycast.sh --skip-build
```

`-DBROWSER_LOADING_STRESS=40` repeats the heaviest fixture traffic that many
times. `-DBROWSER_EXIT_AFTER_TESTS` exits after any self-test to cover
shutdown, `-DBROWSER_PROFILE` prints main-loop timings every two seconds, and
`-DBROWSER_FRAME_DUMP` prints frames that `scripts/frames-to-png.py <log> <dir>`
turns into PNGs. Run `make clean && make` after any of these. A form self-test
build embeds credentials and a fixture build posts over plain HTTP, so never
distribute either.

## Lessons learned

- KOS never reset a connection that `close()` released while the server was
  still sending. Each one stranded a socket and 128 KiB of buffers, and after
  about eight, transfers stopped. If the heap grows in 128 KiB steps after
  cancels or oversized images, check the linked SDK has KallistiOS#1551
  (KOS master has it)
- A KOS TCP receive notification could deadlock against poll and socket
  cleanup after a few page loads, with even cancellation stuck.
  KOS master moves the notification outside both locks (KallistiOS#1550)
- Stock Flycast raises BBA interrupts from its network thread. Under
  `-DBROWSER_LOADING_STRESS=40` about one run in four ended in an emulator
  crash, a guest double fault or a stall, release builds included. The
  patches in `tools/flycast/` fix it; rerun on a patched build before
  suspecting the browser
- Stock Flycast also drops a key press and release that both land between
  guest Maple polls, however fast the guest renders.
  `tools/flycast/keyboard-input.patch` queues them
- Flycast's BBA can re-assert IRQ9 while KOS is still dispatching it, which
  KOS reports as a double fault. The browser gates that event and drains the
  adapter from a worker thread instead (`net.c`)
- Idle TLS connections hold their buffers, so curl keeps only a few and not
  for long (`net.c`)
- The fixture server must bind the Mac's LAN IP. `127.0.0.1` is the guest's
  own loopback under Flycast's picoTCP proxy

## Known limitations

- No CSS layout, JavaScript, audio or video, downloads, tabs, or storage
  beyond VMU bookmarks
- The BIOS font is ISO-8859-1, so other scripts show as `?`
- Layout runs on the main thread once a download finishes; a 1.2 MiB page
  pauses input for about a second and a half in Flycast
- WebP, AVIF and SVG images are not decoded
- The modem path is tested with Flycast's emulated modem only; a real modem
  and DreamPi have not been tried

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
