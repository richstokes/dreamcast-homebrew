# LemonPlay

<p align="center">
  <img src="../../../docs/screenshots/lemonplay.png" alt="LemonPlay browsing a folder of TV episodes on a DLNA server" width="640">
</p>

A DLNA media client for the Dreamcast with a Broadband Adapter. It finds media
servers on the LAN, browses their folders and plays video and music full
screen. The PC does the hard work: run
[ZeroConfigDLNA](https://github.com/richstokes/ZeroConfigDLNA) with
`--dreamcast-mode` and it transcodes each file on the fly into MPEG-1 video
and PCM audio sized for what the SH-4 can decode in real time.

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/lemonplay.cdi)**
for GDEMU or Flycast. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/lemonplay.elf) is also available.
If you are burning a physical CD-R, you may want to try the padded
[CD-R image](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/lemonplay-cdr.zip) instead (unzip before burning).

## Server

On the PC with the media (needs `ffmpeg`, e.g. `brew install ffmpeg`):

```sh
git clone https://github.com/richstokes/ZeroConfigDLNA.git
python3 ZeroConfigDLNA/app.py --dreamcast-mode -d /path/to/media
```

LemonPlay lists any DLNA server, but only plays files from one in Dreamcast
mode. If discovery finds nothing (some networks drop broadcasts), pick
**Enter an address** and type the server's IP; ZeroConfigDLNA uses port 8200.

## Controls

| Input | Browsing | Playing |
| --- | --- | --- |
| A | Open folder, play | Pause / resume |
| Start | Picture mode | Picture mode |
| B | Up a folder | Stop, back to the list |
| D-pad Up/Down | Move | Volume |
| D-pad Left/Right | | Seek 30 s (hold to scrub) |
| L / R triggers | Page | Seek 5 min |
| Y | Refresh | Show / hide the bar |
| X | | Stats: decode time, buffers, network |
| A+B+X+Y+Start | Back to the server list; on it, exit to the Dreamcast BIOS menu | Stop, back to the server list |

At the end of a file the next one in the folder plays.

## Picture modes

Start cycles through these; the choice and the last server are saved to the
first VMU.

| Mode | For | Widescreen video | 4:3 video |
| --- | --- | --- | --- |
| FIT | 4:3 TVs, and 16:9 TVs showing the Dreamcast as 4:3 | Black bars top and bottom | Full screen |
| ZOOM | The same TVs, without the bars | Sides cropped to fill the screen (by the server, so the whole decode budget goes on what you see) | Full screen |
| WIDE | 16:9 TVs set to stretch the picture | Full screen | Black bars at the sides |

Switching into or out of ZOOM restarts the stream where you are, which takes
about a second.

## Build and run

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
make
./run-flycast.sh --skip-build
```

Flycast's BBA proxy reaches a server on the Mac's LAN address. Broadcast
discovery worked through it in testing; if not, enter the address by hand or
build it in with `EXTRA_CFLAGS='-DLP_SERVER=\"http://IP:8200/description.xml\"'`.

| Target | What |
| --- | --- |
| `make` | `lemonplay.elf` |
| `make VARIANT=qa EXTRA_CFLAGS=...LP_SERVER...` | Plays the first video from 5:00, seeks, prints `LP QA: PASS`. Run with `LP_ELF=lemonplay-qa.elf LP_EXIT_ON_LOG="LP QA: done" ./run-flycast.sh --skip-build`. Add `-DLP_QA_LINGER=4000` to pause on each screen for screenshots |
| `make VARIANT=decodebench EXTRA_CFLAGS=...LP_SERVER...` | Downloads 10 s clips at several sizes and bitrates and times the decoder (`LP DECODEBENCH` lines) |
| `make VARIANT=bench EXTRA_CFLAGS=...LP_SERVER...` | Plays 45 s at each of several server budgets and prints `LP PLAYBENCH` lines: decode load, dropped frames, hitches (gaps over 2.5 frames), I-frame skips, rebuffers |
| `make hw` | Real Dreamcast via `tools/dc-hw.sh` |

## Performance

Measured on a real Dreamcast with a Broadband Adapter, 23.976 fps Friends.
MPEG-1 decode costs a roughly fixed time per 16x16 macroblock that rises
with bitrate: about **26 + 78 x bits-per-pixel us per macroblock** in
isolation (`decodebench`), plus ~25% during playback for networking and
drawing. I-frames take 2.5-3x the average.

| Size | bpp | Isolated decode avg / max | Playback decode avg | Decoder busy | Result |
| --- | --- | --- | --- | --- | --- |
| 368x208 | 0.5 | 19.7 / 55 ms | 19.8 ms | 48% | Smooth |
| **448x256** | **0.35** | | **27.6-29.5 ms** | **66-71%** | **Server default: smooth over 3 x 60 s** |
| 448x256 | 0.5 | 27.4 / 76 (448x240) | 36.5 ms | 85% | 1 skip, 2 hitches in 45 s |
| 448x336 | 0.35 | | 35.1 ms | 84% | Smooth, borderline |
| 512x288 | 0.5 | 40.0 / 97 | 41.9 ms | 90% | 3 hitches in 10 s |
| 640x352 | 0.5 | 57.9 / 134 | | | Not real time |

Playback stays smooth while the decoder thread is under ~80% busy and starts
skipping (dropping to the next I-frame) around 85%. ZeroConfigDLNA's default
budget, 11,000 macroblocks/s at 0.35 bpp, sits at ~70%: 448x256 for 24 fps
widescreen, 384x288 in ZOOM or for 4:3. TCP from a Mac reached ~4.5 MB/s;
a stream needs ~0.3 MB/s, so the network is never the limit. Uploading a
picture through the YUV converter costs ~1 ms (3.7 ms at 640 wide).

## Gotchas

- KOS can't send IP multicast, so SSDP discovery is a broadcast M-SEARCH.
  Servers bound to `0.0.0.0:1900` (most are) still answer
- The Dreamcast pl_mpeg port's buffers are power-of-two rings: feed it with
  `plm_buffer_create_with_capacity` + `plm_buffer_write`. Its fixed-memory
  mode silently decodes garbage
- Decoding is bit-exact between Flycast and the console (`decodebench`
  prints a hash per clip), so a picture problem that only shows on hardware
  is in the upload or display path, not the decoder
- Translucent UI polys use depth compare ALWAYS with autosort off, so they
  stack in submission order over the video
- The YUV converter is part of the TA, which KOS re-initialises from its
  interrupt handler when a scene's lists are done. Feed it only between
  `pvr_scene_begin` and the first list, from the render thread; uploads from
  another thread at arbitrary times garbled pictures on the console
- Flycast decodes ~17% faster than the console and doesn't emulate the
  cache: tune budgets on hardware (`make VARIANT=bench`, `decodebench`)
- Yamaha ADPCM would cut audio bandwidth 4x, but the AICA restarts its
  predictor whenever a stream starts, so resuming mid-file after a pause or
  rebuffer drifts. PCM is only ~176 KB/s on a LAN

## Limitations

- Broadband Adapter (or LAN adapter) only; the modem is far too slow
- Plays only ZeroConfigDLNA `--dreamcast-mode` streams. Photos aren't shown yet
- No subtitles
- In WIDE mode the menus and on-screen bar are stretched by the TV too

## Credits

MPEG-1 decoding by [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (Dominic
Szablewski, MIT), via the SH-4 port by Ian Micheal and Andy Barajas
([Dreamcast-Projects/pl_mpeg](https://github.com/Dreamcast-Projects/pl_mpeg)),
vendored in `third_party/pl_mpeg`.
