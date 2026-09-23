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

Both project copies were decoded from Freesound's high-quality previews and
downsampled to 32 kHz stereo. `tools/build_audio_assets.py` extracts seamless
loops from them.

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
downsampled to 32 kHz stereo.

# Music

The soundtrack is original to this project and has no recorded source. Its
notation, instruments and mix all live in `tools/compose_soundtrack.py`, which
synthesizes three mid-1980s-style outrun tracks:

| # | Title | Key / tempo | Character |
| --- | --- | --- | --- |
| 1 | Blue Hour Boulevard | A minor, 112 BPM | Octave-pumping bass, Jupiter-style brass hook, gated snare |
| 2 | Pacific Coast Highway | D minor, 104 BPM | Moroder sixteenth bass, DX-style electric piano, glide lead |
| 3 | Neon Strip | E minor, 126 BPM | Hard-sync lead, Fairlight-style orchestra hits, four-on-the-floor |

The game plays them in order as a looping playlist. `tools/build_music_asset.py`
encodes them as one 22.05 kHz stereo 4-bit IMA ADPCM bank (about 4.9 MiB).
