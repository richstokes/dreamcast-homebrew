#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "scipy>=1.11"]
# ///
"""Synthesize Kaiju Lunch Break's sounds.

Everything is generated from code with seeded RNGs, so the output is
original and reproducible. Outputs:

    assets/generated/sfx.pak         16-bit mono PCM, 32-byte aligned entries
    assets/generated/sound_assets.h  the effect table
    captures/audio-previews/*.wav    listening copies (git-ignored)

The voice lines are recordings (assets/voice/*.mp3), which need ffmpeg to
decode.

The music is not here: it is CD audio on the disc (tools/build_music.py).
"""

from __future__ import annotations

import subprocess
import sys
from functools import partial
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
import synthkit  # noqa: E402
from assetpak import write_sfx_pak, write_sound_header  # noqa: E402
from synthkit import SFX_RATE, TAU, add_reverb, fade, filt, finish_loop, finish_oneshot, midi_hz, place, place_wrap, softsat  # noqa: E402

SR = SFX_RATE
ROOT = Path(__file__).resolve().parent.parent
rng_for = partial(synthkit.rng_for, salt=1954)


def t_of(secs, sr=SR):
    return np.arange(int(secs * sr)) / sr


def env(n, a, d, sr=SR, curve=4.0):
    """Attack a seconds, then an exponential fall with time constant d."""
    t = np.arange(n) / sr
    return np.minimum(t / max(a, 1e-4), 1.0) * np.exp(-np.maximum(t - a, 0) / d * curve / 4.0)


def noise(n, name):
    return rng_for(name).standard_normal(n)


def sweep(f0, f1, secs, sr=SR, shape=1.0):
    t = t_of(secs, sr)
    f = f0 + (f1 - f0) * (t / secs) ** shape
    return np.sin(TAU * np.cumsum(f) / sr)


# ---------------------------------------------------------------------------
# The kaiju

def roar():
    secs = 2.2
    t = t_of(secs)
    f = 92 * (1 + 0.5 * np.exp(-t * 3) - 0.25 * t / secs) * (1 + 0.04 * np.sin(TAU * 11 * t))
    ph = TAU * np.cumsum(f) / SR
    voice = softsat(np.sin(ph) + 0.6 * np.sin(2 * ph + 1) + 0.5 * np.sin(3.01 * ph) + 0.3 * np.sin(5.02 * ph), 3.0)
    rasp = filt(noise(len(t), "roar"), "bp", 900, 0.8) * (0.5 + 0.5 * np.sin(TAU * 31 * t))
    scream = softsat(np.sin(ph * 7.03) * np.sin(TAU * 23 * t), 2.0) * 0.25
    x = (voice + rasp * 0.9 + scream) * np.minimum(t / 0.08, 1) * np.exp(-np.maximum(t - 1.2, 0) * 2.6)
    x = filt(filt(x, "peak", 420, 1.2, gain_db=8), "lp", 5200)
    return add_reverb(x, 0.35, 1.2, 1.4, seed="roar")


def step():
    n = int(0.6 * SR)
    x = sweep(78, 30, 0.6) * env(n, 0.004, 0.16) * 1.4 + filt(noise(n, "step"), "lp", 260) * env(n, 0.002, 0.07) * 1.2
    return softsat(x, 1.8)


def whoosh(name, f0, f1, secs):
    n = int(secs * SR)
    t = np.arange(n) / SR
    nz = noise(n, name)
    out = np.zeros(n)
    for k in range(0, n, 256):
        fc = f0 + (f1 - f0) * np.sin(np.pi * k / n)
        out[k:k + 256] = filt(nz[max(0, k - 512):k + 256], "bp", fc, 1.6)[-min(256, n - k):][:min(256, n - k)]
    return out * np.sin(np.pi * t / secs) ** 1.5


def chomp():
    n = int(0.42 * SR)
    x = np.zeros(n)
    place(x, 0, sweep(520, 140, 0.09) * env(int(0.09 * SR), 0.002, 0.03), 1.0)
    place(x, int(0.1 * SR), filt(noise(int(0.2 * SR), "chomp"), "bp", 1500, 1.2) * env(int(0.2 * SR), 0.002, 0.05), 1.3)
    place(x, int(0.1 * SR), sweep(180, 60, 0.2) * env(int(0.2 * SR), 0.002, 0.07), 1.0)
    return softsat(x, 2.0)


def gulp():
    n = int(0.3 * SR)
    return sweep(180, 520, 0.3, shape=2.0) * np.sin(np.pi * np.arange(n) / n) ** 0.6 * (1 + 0.3 * np.sin(TAU * 26 * np.arange(n) / SR))


def beam_loop():
    n = 16384
    t = np.arange(n) / SR
    cyc = lambda f: np.round(f * n / SR) * SR / n   # noqa: E731  whole cycles in the loop
    x = sum(np.sin(TAU * cyc(f) * t + i) for i, f in enumerate((110, 221, 334, 667, 1001))) * 0.3
    x = softsat(x * (1 + 0.5 * np.sin(TAU * cyc(43) * t)), 2.5)
    nz = np.fft.irfft(np.fft.rfft(noise(n, "beam")) * (np.abs(np.fft.rfftfreq(n, 1 / SR) - 2600) < 1800), n)
    return finish_loop(x + nz * 0.5)


def hurt():
    n = int(0.3 * SR)
    return softsat(sweep(300, 120, 0.3) * env(n, 0.003, 0.09) + filt(noise(n, "hurt"), "bp", 800, 1.0) * env(n, 0.002, 0.05), 2.4)


# ---------------------------------------------------------------------------
# The city coming down

def crash(name, secs, low, bright):
    n = int(secs * SR)
    t = np.arange(n) / SR
    nz = noise(n, name)
    rumble = filt(nz, "lp", low) * env(n, 0.01, secs * 0.38) * 2.2
    grit = filt(nz, "bp", 1200, 0.6) * env(n, 0.004, secs * 0.2)
    r = rng_for(name + "g")
    glass = np.zeros(n)
    for _ in range(int(40 * bright * secs)):
        at = int(r.uniform(0, 0.6 * secs) * SR)
        m = int(r.uniform(0.02, 0.08) * SR)
        f = r.uniform(2500, 7000)
        place(glass, at, np.sin(TAU * f * np.arange(m) / SR) * env(m, 0.001, 0.02), r.uniform(0.15, 0.5) * bright)
    thump = sweep(90, 34, min(secs, 0.5)) * env(int(min(secs, 0.5) * SR), 0.003, 0.14) * 1.6
    x = rumble + grit + glass
    place(x, 0, thump)
    return softsat(x, 1.6) * (1 - 0.15 * np.sin(TAU * 9 * t))


def creak():
    secs = 1.7
    t = t_of(secs)
    f = 160 - 70 * t / secs + 22 * np.sin(TAU * 3.1 * t)
    ph = TAU * np.cumsum(f) / SR
    x = softsat(np.sin(ph) * np.sign(np.sin(ph * 0.5)) + 0.5 * np.sin(ph * 2.7), 4.0)
    x = filt(x, "bp", 640, 0.9) * np.minimum(t / 0.2, 1) * np.exp(-np.maximum(t - 1.0, 0) * 3)
    return x + filt(noise(len(t), "creak"), "lp", 200) * 0.4 * np.minimum(t / 0.4, 1)


def explode():
    n = int(1.5 * SR)
    x = filt(noise(n, "expl"), "lp", 900) * env(n, 0.004, 0.42) * 2.5 + sweep(110, 28, 1.5) * env(n, 0.004, 0.3) * 1.8
    return add_reverb(softsat(x, 2.4), 0.25, 0.9, 1.0, seed="expl")


def squish():
    n = int(0.3 * SR)
    x = filt(noise(n, "squish"), "bp", 2400, 0.8) * env(n, 0.002, 0.06) + sweep(240, 60, 0.3) * env(n, 0.002, 0.07) * 1.2
    return softsat(x, 2.2)


def horn():
    n = int(0.4 * SR)
    t = np.arange(n) / SR
    x = softsat(np.sin(TAU * 392 * t) + np.sin(TAU * 494 * t), 2.5)
    return filt(x, "lp", 3200) * np.minimum(t / 0.01, 1) * np.minimum((0.4 - t) / 0.04, 1)


def shot():
    n = int(0.5 * SR)
    return softsat(filt(noise(n, "shot"), "lp", 1800) * env(n, 0.001, 0.09) * 2.4 + sweep(160, 40, 0.5) * env(n, 0.002, 0.1), 2.6)


def heli_loop():
    n = 8192
    t = np.arange(n) / SR
    blade = np.round(22 * n / SR) * SR / n
    chop = np.maximum(np.sin(TAU * blade * t), 0) ** 6
    nz = np.fft.irfft(np.fft.rfft(noise(n, "heli")) * (np.fft.rfftfreq(n, 1 / SR) < 1400), n)
    return finish_loop(nz / np.max(np.abs(nz)) * (0.25 + chop) + 0.4 * np.sin(TAU * blade * 4 * t) * chop)


def siren_loop():
    n = 32768
    t = np.arange(n) / SR
    f = 760 + 280 * np.sin(TAU * (SR / n) * 2 * t)
    x = softsat(np.sin(TAU * np.cumsum(f) / SR), 2.0)
    x[-1] = x[-1]
    return finish_loop(filt(x, "bp", 1100, 0.7) - np.linspace(0, 1, n) * 0)


def crowd_loop():
    n = 32768
    r = rng_for("crowd")
    x = np.zeros(n)
    for _ in range(60):
        m = int(r.uniform(0.25, 0.7) * SR)
        f0 = r.uniform(500, 1100)
        tt = np.arange(m) / SR
        f = f0 * (1 + 0.25 * np.sin(np.pi * tt / (m / SR))) * (1 + 0.03 * np.sin(TAU * r.uniform(5, 9) * tt))
        ph = TAU * np.cumsum(f) / SR
        v = softsat(np.sin(ph) + 0.5 * np.sin(2 * ph), 2.0) * np.sin(np.pi * tt / (m / SR)) ** 0.7
        place_wrap(x, r.integers(0, n), v, r.uniform(0.2, 0.6))
    return finish_loop(filt(x, "bp", 1300, 0.6))


# ---------------------------------------------------------------------------
# Score and menu

def blip(notes, dur=0.07, tail=0.12, wave="tri"):
    n = int((dur * len(notes) + tail) * SR)
    x = np.zeros(n)
    for i, m in enumerate(notes):
        k = int((dur + tail) * SR)
        t = np.arange(k) / SR
        f = midi_hz(m)
        v = (2 / np.pi) * np.arcsin(np.sin(TAU * f * t)) if wave == "tri" else softsat(np.sin(TAU * f * t), 3.0)
        place(x, int(i * dur * SR), (v + 0.3 * np.sin(TAU * 2 * f * t)) * env(k, 0.003, tail * 0.5))
    return x


def bell():
    """The lunch bell: back to work."""
    n = int(1.6 * SR)
    t = np.arange(n) / SR
    x = sum(np.sin(TAU * f * t) * np.exp(-t * d) * a for f, d, a in ((880, 2.2, 1.0), (1320, 3.0, 0.6), (2217, 4.5, 0.4), (3520, 7.0, 0.25)))
    trem = 0.6 + 0.4 * np.sign(np.sin(TAU * 14 * t))
    return x * np.where(t < 0.9, trem, 1.0)


VOICES = set()   # packed as ADPCM


def voice(key, name):
    """A voice line: the recording, cut to the speech. A line too long for
    one AICA sample at the effects' rate is packed at a lower one."""
    def cut(sr):
        raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(ROOT / "assets" / "voice" / f"{name}.mp3"),
                              "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"], check=True, capture_output=True).stdout
        x = np.frombuffer(raw, dtype="<f4").astype(float)
        loud = np.nonzero(np.abs(x) > np.max(np.abs(x)) * 10 ** (-45 / 20))[0]
        return x[max(0, loud[0] - int(0.01 * sr)):loud[-1] + int(0.05 * sr)]
    sr, x = SR, cut(SR)
    if len(x) > 65000:
        sr = int(SR * 65000 / len(x)) // 50 * 50
        x = cut(sr)
    VOICES.add(key)
    return key, one(x, sr), False, sr


# ---------------------------------------------------------------------------

def one(x, sr=SR):
    x = finish_oneshot(np.asarray(x, dtype=float), sr)
    return synthkit.fit(x, 65520, sr) if len(x) > 65520 else x


def render():
    return [
        ("ROAR", one(roar()), False, SR),
        ("STEP", one(step()), False, SR),
        ("SWIPE", one(whoosh("swipe", 700, 2600, 0.3)), False, SR),
        ("TAIL", one(whoosh("tail", 300, 1500, 0.55)), False, SR),
        ("JUMP", one(sweep(120, 420, 0.35, shape=0.6) * np.sin(np.pi * t_of(0.35) / 0.35)), False, SR),
        ("CHOMP", one(chomp()), False, SR),
        ("GULP", one(gulp()), False, SR),
        ("HURT", one(hurt()), False, SR),
        ("CRASH_S", one(crash("cs", 0.9, 420, 1.0)), False, SR),
        ("CRASH_M", one(crash("cm", 1.6, 320, 0.8)), False, SR),
        ("CRASH_L", one(add_reverb(crash("cl", 2.6, 240, 0.5), 0.2, 1.0, 1.2, seed="cl")), False, SR),
        ("CREAK", one(creak()), False, SR),
        ("EXPLODE", one(explode()), False, SR),
        ("SQUISH", one(squish()), False, SR),
        ("HORN", one(horn()), False, SR),
        ("SHOT", one(shot()), False, SR),
        ("TICK", one(blip([84], 0.05, 0.14)), False, SR),
        ("TRICK", one(blip([76, 83, 88], 0.06, 0.2)), False, SR),
        ("BANK", one(blip([72, 76, 79, 84, 88], 0.07, 0.35)), False, SR),
        ("DROP", one(blip([67, 62, 55], 0.09, 0.2, "sq")), False, SR),
        ("MOVE", one(blip([79], 0.03, 0.06)), False, SR),
        ("OK", one(blip([79, 86], 0.05, 0.12)), False, SR),
        ("BELL", one(bell()), False, SR),
        ("BEEP", one(blip([81], 0.08, 0.1, "sq")), False, SR),
        voice("WELCOME", "welcome"),
        voice("GOBBLE", "gobble"),
        voice("PRAISE", "praise"),
        voice("TRYHARDER", "tryharder"),
        voice("WOW", "wow"),
        ("BEAM", beam_loop(), True, SR),
        ("HELI", heli_loop(), True, SR),
        ("SIREN", siren_loop(), True, SR),
        ("CROWD", crowd_loop(), True, SR),
    ]


def main():
    out = ROOT / "assets" / "generated"
    previews = ROOT / "captures" / "audio-previews"
    out.mkdir(parents=True, exist_ok=True)
    previews.mkdir(parents=True, exist_ok=True)
    sounds = render()
    entries = write_sfx_pak(out, previews, sounds, max_bytes=1_900_544, adpcm=VOICES)
    write_sound_header(
        out, "KL", entries,
        notes=[f"{name:10s} {len(x) / rate:.2f} s" for (name, x, _, rate) in sounds],
        names=[name.replace("_", " ") for name, *_ in entries],
        struct_comment="offset/bytes into sfx.pak (32-byte aligned entries), loop=1 for seamless loops and 2 for a 4-bit ADPCM "
                       "one-shot, sample rate",
        names_symbol="kl_sfx_names", names_comment="display names")


if __name__ == "__main__":
    main()
