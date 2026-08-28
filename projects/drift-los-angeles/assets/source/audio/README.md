# Recorded V8 sources

`v8-steady-idle.wav` is a 32 kHz, 16-bit stereo working copy of the real field
recording **“Eight-cylinder engine idling.wav”** by Freesound user
**Lumamorph**.

- Source: <https://freesound.org/people/Lumamorph/sounds/636066/>
- Original publication date: May 30, 2022
- License: [Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/)
- Original description: a seamless stereo recording of an American
  eight-cylinder engine idling
- Project WAV SHA-256:
  `56e58c81bd2dd6ddbd727945497be5c7ecdf284888d3cd6a5896e35800dfaabd`

`v8-steady-high.wav` is a 32 kHz, 16-bit stereo working copy of the real field
recording **“Hot-Rod-V-8-BigSpchg-RoughIdle-RevUps-IdleDR025-30sec.wav”** by
Freesound user **Ears68**.

- Source: <https://freesound.org/people/Ears68/sounds/144454/>
- Original publication date: January 30, 2012
- License: [Creative Commons Attribution 3.0](https://creativecommons.org/licenses/by/3.0/)
- Original description: a supercharged V8 hot rod that revs and then holds a
  higher RPM, recorded trackside on a Tascam DR-05
- Project WAV SHA-256:
  `740556ccc4a789c1aeeec4b5918daf036a3024fb4d6da78265b382710a2ed4a9`

Both project copies were decoded from Freesound's high-quality previews,
downsampled to 32 kHz, and retained in stereo. The builder extracts measured
stationary regions, removes DC, and applies equal-power loop crossfades. It also
applies circular macro-leveling and conservative peak ceilings, then rejects an
engine loop when its circular 200 ms loudness windows reveal a slow dropout or
repeating volume pulse.

`v8-engine-rev.wav` is the superseded CC0 recording **“v8 engine rev.wav”** by
Freesound user **overmedium** (<https://freesound.org/people/overmedium/sounds/651534/>).
It remains as an auditable source reference but is not compiled into the game:
its repeated throttle envelope was unsuitable for a stationary engine loop.

`tire-screech.wav` is a 32 kHz, 16-bit stereo working copy of the real-world
field recording **“Distant car tire screetch”** by Freesound user
**Sadiquecat**.

- Source: <https://freesound.org/people/Sadiquecat/sounds/737192/>
- Original publication date: May 25, 2024
- License: [Creative Commons CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
- Original description: a car tire screech captured with a Zoom H2n and
  subsequently trimmed and denoised
- Project WAV SHA-256:
  `7470401554d27b30a791e6573e587281bb4d9c74efb81f99d5c017767be436a5`

The project copy was decoded from Freesound's high-quality preview and
downsampled from 44.1 kHz to 32 kHz stereo. The build tool isolates its clean
squeal body, removes DC, crossfades the loop boundary, and normalizes it. The
runtime varies its playback rate and blends it with slip-driven scrub detail so
it can sustain naturally through a long drift.

# Music source

`music/pure_raceway_bpm160.ogg` is the complete 96-second loopable track
**“Pure Raceway”** by OpenGameArt user **MintoDog**.

- Source: <https://opengameart.org/content/pure-raceway>
- Original publication date: February 6, 2024
- License: [Creative Commons CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
- Original description: upbeat 160 BPM synth music composed for a racing game
- Project Ogg SHA-256:
  `e384208fc0bed123b68fd31e28df2d7bc3e8463551e49d681d700b2c21565e5d`

`tools/build_music_asset.py` decodes the master at 22.05 kHz stereo, removes
the small measured DC offset, and encodes it as G.711 mu-law. The resulting
4.04 MiB embedded bank preserves the complete arrangement and stereo image at
one byte per channel sample. Runtime decoding is only two table lookups per
frame, which is considerably cheaper than the superseded procedural music
synth and leaves the SH-4 budget available to the city renderer and physics.
