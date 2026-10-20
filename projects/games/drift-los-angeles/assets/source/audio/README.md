# Recorded V8 sources

`v8-steady-idle.wav` is a 32 kHz, 16-bit stereo working copy of the field
recording **“Eight-cylinder engine idling.wav”** by Freesound user
**Lumamorph**.

- Source: <https://freesound.org/people/Lumamorph/sounds/636066/>
- Original publication date: May 30, 2022
- License: [Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/)
- Original description: a seamless stereo recording of an American
  eight-cylinder engine idling
- Project WAV SHA-256:
  `56e58c81bd2dd6ddbd727945497be5c7ecdf284888d3cd6a5896e35800dfaabd`

`v8-steady-high.wav` is a 32 kHz, 16-bit stereo working copy of the field
recording **“Hot-Rod-V-8-BigSpchg-RoughIdle-RevUps-IdleDR025-30sec.wav”** by
Freesound user **Ears68**.

- Source: <https://freesound.org/people/Ears68/sounds/144454/>
- Original publication date: January 30, 2012
- License: [Creative Commons Attribution 3.0](https://creativecommons.org/licenses/by/3.0/)
- Original description: a supercharged V8 hot rod that revs and then holds a
  higher RPM, recorded trackside on a Tascam DR-05
- Project WAV SHA-256:
  `740556ccc4a789c1aeeec4b5918daf036a3024fb4d6da78265b382710a2ed4a9`

`tire-screech.wav` is a 32 kHz, 16-bit stereo working copy of the field
recording **“Distant car tire screetch”** by Freesound user **Sadiquecat**.

- Source: <https://freesound.org/people/Sadiquecat/sounds/737192/>
- Original publication date: May 25, 2024
- License: [Creative Commons CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
- Original description: a car tire screech captured with a Zoom H2n and
  subsequently trimmed and denoised
- Project WAV SHA-256:
  `7470401554d27b30a791e6573e587281bb4d9c74efb81f99d5c017767be436a5`

All three project copies were decoded from Freesound's high-quality previews
and downsampled to 32 kHz stereo. `tools/build_audio_assets.py` (`make audio`)
cuts the engine loops from the two V8 recordings and builds the tire loop from
the sustained core of the screech rather than looping the one-shot directly;
the loop rules are commented in that script.

`v8-engine-rev.wav` is the superseded CC0 recording **“v8 engine rev.wav”** by
Freesound user **overmedium** (<https://freesound.org/people/overmedium/sounds/651534/>).
It is kept as an auditable source reference and is not compiled into the game;
its repeated throttle envelope does not loop as a stationary engine.

# Music

The soundtrack is original to this project and has no recorded source. Its
notation, instruments and mix live in `tools/compose_soundtrack.py`;
`tools/build_music_asset.py` packs the three tracks into one 22.05 kHz stereo
4-bit IMA ADPCM bank (about 4.9 MiB) that the game plays as a looping playlist.

| # | Title | Key / tempo |
| --- | --- | --- |
| 1 | Blue Hour Boulevard | A minor, 112 BPM |
| 2 | Pacific Coast Highway | D minor, 104 BPM |
| 3 | Neon Strip | E minor, 126 BPM |
