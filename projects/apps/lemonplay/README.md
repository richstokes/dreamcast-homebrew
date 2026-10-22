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
| A / Start | Open folder, play | Pause / resume |
| B | Up a folder | Stop, back to the list |
| D-pad Up/Down | Move | Volume |
| D-pad Left/Right | | Seek 30 s (hold to scrub) |
| L / R triggers | Page | Seek 5 min |
| Y | Refresh | Show / hide the bar |
| X | | Stats: decode time, buffers, network |

At the end of a file the next one in the folder plays.

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
| `make VARIANT=bench EXTRA_CFLAGS=...LP_SERVER...` | Real-time playback with a stats line every 10 s |
| `make hw` | Real Dreamcast via `tools/dc-hw.sh` |

## Performance

MPEG-1 decode costs a roughly fixed time per 16x16 macroblock, rising with
bitrate. Decode benchmark, 10 s of 23.976 fps Friends at 0.5 bits/pixel:

| Size | kbps | Flycast avg / p95 / max ms |
| --- | --- | --- |
| 320x176 | 718 | 12.4 / 19.0 / 44.7 |
| 368x208 | 955 | 16.8 / 26.4 / 53.8 |
| 448x240 | 1310 | 23.4 / 35.5 / 71.7 |
| 512x288 | 1761 | 31.9 / 47.4 / 94.4 |
| 640x352 | 2650 | 48.7 / 65.7 / 130.8 |

That is ~18,000 macroblocks/s in Flycast at 0.5 bpp (~27,000 at 0.25,
~14,000 at 1.0). I-frames take ~2.7x the average. Uploading a picture through
the YUV converter costs under 1 ms. Real hardware numbers are still to come;
until then the server's budget is a conservative 7,500 macroblocks/s
(368x208 at 24 fps).

## Gotchas

- KOS can't send IP multicast, so SSDP discovery is a broadcast M-SEARCH.
  Servers bound to `0.0.0.0:1900` (most are) still answer
- The Dreamcast pl_mpeg port's buffers are power-of-two rings: feed it with
  `plm_buffer_create_with_capacity` + `plm_buffer_write`. Its fixed-memory
  mode silently decodes garbage
- Translucent UI polys use depth compare ALWAYS with autosort off, so they
  stack in submission order over the video
- Yamaha ADPCM would cut audio bandwidth 4x, but the AICA restarts its
  predictor whenever a stream starts, so resuming mid-file after a pause or
  rebuffer drifts. PCM is only ~176 KB/s on a LAN

## Limitations

- Broadband Adapter (or LAN adapter) only; the modem is far too slow
- Plays only ZeroConfigDLNA `--dreamcast-mode` streams. Photos aren't shown yet
- No subtitles

## Credits

MPEG-1 decoding by [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (Dominic
Szablewski, MIT), via the SH-4 port by Ian Micheal and Andy Barajas
([Dreamcast-Projects/pl_mpeg](https://github.com/Dreamcast-Projects/pl_mpeg)),
vendored in `third_party/pl_mpeg`.
