# Responsiveness profile — 2026-09-23

Measured with the checked-out KOS SDK and native ARM64 Flycast at revision
`628bd3dbb160ea2750230fc6b00c0bb8173cb1f6` on an Apple M5 Pro. Timings are
Dreamcast guest microseconds, not wall-clock latency on real hardware.
The baseline is the working tree at the start of this investigation, which
already had a cached BIOS font and incremental row rendering.

## Browser results

Both versions ran the same local 320-item document, 60 iterations per render
case. Full redraw and incremental timings exclude framebuffer transfer and
vblank waits. The baseline copy disables the immediate vblank wake and uses
the original renderer; the profiler and benchmark harness are otherwise shared.

| Measurement | Before | After |
| --- | ---: | ---: |
| Address caret composition | 1,084 µs | 672 µs |
| Mouse movement composition | 273 µs | 255 µs |
| 14-pixel scroll composition | 5,030 µs | 5,025 µs |
| Full text page composition | 7,670 µs | 7,707 µs |
| Full page with on-screen keyboard | 10,097 µs | 10,052 µs |
| Average interrupt-to-main wake | about 4,970 µs | about 20–30 µs |

Address editing is about 38% cheaper. Scrolling and full redraws are effectively
unchanged. The large responsiveness win is removing the extra 0–10 ms scheduler
delay before input handling. KOS still polls Maple at display refresh rate, and
a device-discovery poll periodically replaces an input poll; these measurements
are not a promise of 20 µs physical-key-to-screen latency.

`sem_signal()` only makes the main thread runnable. The vblank handler now
schedules that thread immediately when it was actually waiting, leaving page
loads and other main-thread work alone. The renderer avoids traversing page
items when an address-only dirty band is entirely behind the toolbar. Relative
mouse deltas are copied and consumed under a short IRQ lock, so a skipped poll
cannot repeat the previous movement or wheel notch.

## Keyboard delivery

The browser drains the KOS key queue fully before composing a frame. Flycast's
original keyboard path stores only currently held keys. A host keydown and
keyup processed before the next guest poll therefore disappear. Faster guest
rendering cannot recover those events. The local Flycast build includes a
bounded transition queue consumed by keyboard Maple reads; see
[the Flycast build notes](../../../tools/flycast/README.md).

The browser launcher also sets `config:rend.EmulateFramebuffer=yes` transiently.
It keeps host presentation/input pumping while the page is idle, and lets the
guest use ordinary framebuffer flips without emulator-specific VRAM writes.
No persistent Flycast settings or host networking changes are required.

The patched emulator was tested through its native window with F6, Ctrl+A,
a 26-character text/number string, repeated lowercase and uppercase letters,
punctuation, and a 100-character numeric burst. All **155 expected guest key
presses** arrived (shortcut keys included); the final burst produced an address
length of exactly 100. Before the emulator patch, the same short F6/A/B/C taps
produced zero guest key events. The two shorter strings were also checked
visually for exact content. Synthetic bursts drain at the guest's Maple polling
rate rather than appearing as an instantaneous paste.

Validation also passed 47,923 host C checks, six profiler tests, the standalone
Flycast transition-queue regressions, and the Dreamcast keyboard/mouse/render
self-tests. An additional temporary ASan/UBSan host harness checked 12,000
randomized incremental/full redraws and double-buffer presentations without
mismatches or sanitizer errors. A typed HTTPS navigation to `https://example.com/`
returned HTTP 200 and rendered successfully using Flycast's local picoTCP
backend (`DCNet=no`, guest `192.168.169.2`); this is outbound proxy networking,
not LAN bridging.

## SDK memory correction

The installed KOS keyboard driver also used `state + sizeof(kbd_state_t)` for
its private-state reset. C scales that offset by the size of the pointed-to
struct: the compiled driver cleared 76 bytes at offset 73,984 within a 348-byte
allocation. The one-line compatibility patch uses `state + 1`, offset 272.
`scripts/fix-kos-keyboard.sh` applies that exact patch and rebuilds the keyboard
object and SDK archives without recompiling unrelated drivers. The rebuilt
archive's disassembly confirms the corrected offset. This is a memory safety
repair, separate from the timing improvements above.

## Reproduce

```sh
./scripts/profile.sh /tmp/browser-profile.log
python3 scripts/analyze-profile.py /tmp/browser-profile.log
make -C tests
```

The self-tests use local data and cover address editing, focus/help/exit keys,
mouse movement across skipped polls, and pixel-for-pixel incremental/full
render equivalence. `--expect-keys N` checks received events in complete profile
windows; count shortcut keys too, and wait for the final two-second report.
Profile logs contain counts and timings, not the typed text.

Restore a normal build afterward with a sourced KOS environment:

```sh
make clean && make
```

The follow-up usability work moves HTTP/TLS to a serialized worker while the
main thread continues processing input and frames. Text appears before optional
image downloads. Parsing and image decoding remain bounded main-thread work;
this does not add a CSS or JavaScript engine.

## Real-site usability follow-up

The same emulator was exercised with BBC News, Wikipedia's Dreamcast article,
Hacker News and DuckDuckGo Lite. Reader mode is on by default when semantic
article/main content is present; `F7` restores the full document from retained
HTML. Optional images use `F4`, so slow or unsupported images do not delay text.

- BBC News places its heading and first headline on the first screen. Opening
  a headline renders the article; unloaded image placeholders occupy one small
  row rather than several screens of empty image space.
- Wikipedia's 1,200,498-byte Dreamcast response produces 6,159 layout items and
  2,136 working links without shortening. `#Hardware` scrolls to the section
  without a second HTTP request. The document occupies 2,376 KiB of static RAM;
  the post-load heap report shows 2,462 KiB allocated and 9,014 KiB available.
  Three Page Down redraws composed in about 9.2–12.0 ms each, with about 1.5 ms
  for presentation. This is a spot check, not a whole-page latency distribution.
- Hacker News keeps its thirtieth story, footer links, More link and search
  field active beyond the old 96-link cutoff.
- Typing `dreamcast homebrew` in the address bar returns DuckDuckGo Lite
  search results. `Ctrl+Enter` now submits focused form fields, including
  buttonless search forms; HTTPS GET searches may cross origins.

The final 320-item benchmark remains comparable: 8,063 µs for a full compose,
10,407 µs with the on-screen keyboard, 673 µs for the address caret, 5,307 µs
for a 14-pixel scroll and 323 µs for mouse movement. Enlarging the document
limits does not make address-only redraws scan the page.

The local streaming fixture exercises editing, scrolling, cancellation,
replacement navigation, history, same-document section navigation and reload,
reader switching, image cancellation and form policies. Host parser/input tests
pass 48,857 checks, image tests pass 106 checks, and both pass ASan/UBSan. All
57 guest loading checks pass, including latest-request handling in the gap
between worker completion and deferred navigation. The
fixture also verifies that unsupported SVG and a failed image HEAD request do
not prevent a following valid PNG from loading. See the README for commands.

## SDK TCP lock correction

Repeated HTTPS navigation exposed a receive/poll/cleanup deadlock in the
checked-out SDK. TCP receive held `tcp_sem` while notifying the poll mutex;
polling held that mutex while trying to acquire a read lock; socket cleanup
could hold the writer gate while waiting for existing readers. In that state
the UI remained responsive but the HTTP worker could not finish or cancel.

`scripts/fix-kos-tcp-poll.sh` applies `patches/kos-tcp-poll-lock.patch`, moving
the receive notification outside both TCP locks, and rebuilds only the TCP
object and SDK archives. Descriptor/event values are copied before unlocking,
so notification does not dereference a socket that cleanup may have freed.
The patched SDK passed the streaming suite and the live-site navigation above.

A rapid sequence of requests also temporarily retains TCP buffers during the
SDK's 30-second TIME_WAIT period. An idle observation returned heap allocation
from about 2,098 KiB to 299 KiB; this transient retention is separate from the
worker/source lifetimes. These results use Flycast's outbound picoTCP proxy,
not LAN bridging, and have not been measured on physical Dreamcast hardware.
