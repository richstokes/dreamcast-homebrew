#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "scipy>=1.11"]
# ///
"""Synthesize Totally Bazaar's sound effects and its bazaar groove soundtrack.

Everything is generated from code: no samples. Outputs in --output-dir:

    sfx.pak             16-bit mono PCM effects, 32-byte aligned
    sound_assets.[ch]   effect table
    bazaar_music.adpcm  seamless stereo AICA ADPCM playlist loop (wav2adpcm)

The music is a three-track playlist that plays in order and loops: "Bazaar
Groove" (100 BPM sitar and bansuri over kick/clap/bass), "Spice Route"
(116 BPM dhol and shehnai) and "Moonlit Caravan" (6/8 santoor, bansuri and
camel bells), joined with crossfades and encoded as one continuous loop.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
import wave
from pathlib import Path

import numpy as np
from scipy.signal import lfilter

SFX_RATE = 22050
MUSIC_RATE = 22050
WAV2ADPCM = os.path.expanduser("~/.local/share/dreamcast/kos/utils/wav2adpcm/wav2adpcm")
RNG = np.random.default_rng(2026)


def midi_hz(n):
    return 440.0 * 2.0 ** ((np.asarray(n, dtype=np.float64) - 69) / 12.0)


def env_adsr(n, rate, a=0.005, d=0.05, s=0.6, r=0.1):
    t = np.arange(n) / rate
    e = np.ones(n)
    e = np.where(t < a, t / max(a, 1e-6), e)
    dm = (t >= a) & (t < a + d)
    e = np.where(dm, 1 - (1 - s) * (t - a) / max(d, 1e-6), e)
    e = np.where(t >= a + d, s, e)
    rel = int(r * rate)
    if rel > 0 and rel < n:
        e[-rel:] *= np.linspace(1, 0, rel)
    return e


def lowpass(x, cutoff, rate):
    a = np.exp(-2 * np.pi * cutoff / rate)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    return y


def onepole_fast(x, cutoff, rate):
    a = np.exp(-2 * np.pi * cutoff / rate)
    return lfilter([1 - a], [1, -a], x)


def noise(n):
    return RNG.uniform(-1, 1, n)


# ======================================================================
# Sound effects
# ======================================================================

def sfx_tick():
    n = int(0.035 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    return np.sin(2 * np.pi * 1350 * t) * np.exp(-t * 120) * 0.5


def sfx_pop(up=True):
    n = int(0.09 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    f0, f1 = (520, 980) if up else (900, 480)
    f = f0 + (f1 - f0) * (t / t[-1]) ** 0.6
    ph = 2 * np.pi * np.cumsum(f) / SFX_RATE
    return (np.sin(ph) + 0.25 * np.sin(2 * ph)) * np.exp(-t * 38) * 0.6


def sfx_confirm():
    out = np.zeros(int(0.45 * SFX_RATE))
    for k, (note, start) in enumerate(((79, 0.0), (86, 0.07))):
        n = len(out) - int(start * SFX_RATE)
        t = np.arange(n) / SFX_RATE
        f = midi_hz(note)
        tone = (np.sin(2 * np.pi * f * t) + 0.4 * np.sin(2 * np.pi * f * 2.01 * t) +
                0.2 * np.sin(2 * np.pi * f * 3.98 * t)) * np.exp(-t * 7)
        out[int(start * SFX_RATE):] += tone * 0.35
    return out


def sfx_error():
    n = int(0.28 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    sq = np.sign(np.sin(2 * np.pi * 140 * t)) * 0.5 + np.sign(np.sin(2 * np.pi * 147 * t)) * 0.5
    gate = ((t < 0.1) | ((t > 0.14) & (t < 0.26))).astype(float)
    return lowpass(sq * gate, 1800, SFX_RATE) * 0.35


def sfx_card():
    n = int(0.16 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    x = noise(n)
    cut = 6000 * np.exp(-t * 16) + 600
    y = np.zeros(n)
    acc = 0.0
    for i in range(n):
        a = np.exp(-2 * np.pi * cut[i] / SFX_RATE)
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    env = np.minimum(t / 0.008, 1) * np.exp(-t * 22)
    return y * env * 1.6


def sfx_flip():
    n = int(0.06 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    return (noise(n) * np.exp(-t * 90) * 0.5 +
            np.sin(2 * np.pi * 900 * t) * np.exp(-t * 70) * 0.3)


def sfx_coin(pitch=1.0):
    n = int(0.45 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    partials = [(2093, 1.0, 9), (3136, 0.6, 12), (4699, 0.45, 16), (5588, 0.3, 22),
                (2637, 0.5, 10)]
    y = np.zeros(n)
    for f, a, d in partials:
        y += a * np.sin(2 * np.pi * f * pitch * t + RNG.uniform(0, 6)) * np.exp(-t * d)
    y += noise(n) * np.exp(-t * 200) * 0.4
    # Second, quieter bounce.
    b = int(0.06 * SFX_RATE)
    y[b:] += y[:-b] * 0.35
    return y * 0.22


def sfx_bonus():
    out = np.zeros(int(0.9 * SFX_RATE))
    notes = [74, 78, 81, 86, 90]
    for i, note in enumerate(notes):
        s = int(i * 0.07 * SFX_RATE)
        n = len(out) - s
        t = np.arange(n) / SFX_RATE
        f = midi_hz(note)
        out[s:] += (np.sin(2 * np.pi * f * t) + 0.3 * np.sin(2 * np.pi * f * 3.01 * t)) * \
            np.exp(-t * 6) * 0.22
    return out


def sfx_camel():
    """A comic dromedary grumble: buzzy pulse through moving vowel formants."""
    n = int(0.75 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    f0 = 95 + 25 * np.sin(2 * np.pi * 2.3 * t) * (1 - t) + 12 * np.sin(2 * np.pi * 17 * t)
    ph = 2 * np.pi * np.cumsum(f0) / SFX_RATE
    src = np.zeros(n)
    for h in range(1, 30):
        src += np.sin(h * ph) / h
    src += noise(n) * 0.15
    y = np.zeros(n)
    formants = [(500, 900, 90), (1100, 1500, 120), (2400, 2600, 200)]
    for fa, fb, bw in formants:
        fc = fa + (fb - fa) * np.clip(np.sin(np.pi * t / t[-1]), 0, 1)
        # Two-pole resonator per sample (time-varying).
        r = np.exp(-np.pi * bw / SFX_RATE)
        y1 = y2 = 0.0
        out = np.empty(n)
        for i in range(n):
            c = 2 * r * np.cos(2 * np.pi * fc[i] / SFX_RATE)
            v = src[i] + c * y1 - r * r * y2
            y2, y1 = y1, v
            out[i] = v
        y += out / np.max(np.abs(out))
    env = np.minimum(t / 0.05, 1) * np.clip((t[-1] - t) / 0.2, 0, 1)
    return y * env * 0.3


def sfx_shuffle():
    out = np.zeros(int(0.7 * SFX_RATE))
    for i in range(14):
        s = int((i * 0.04 + RNG.uniform(0, 0.01)) * SFX_RATE)
        c = sfx_flip() * RNG.uniform(0.4, 0.8)
        out[s:s + len(c)] += c[:len(out) - s]
    return out


def sfx_whoosh():
    n = int(0.4 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    x = noise(n)
    cut = 400 + 3500 * np.sin(np.pi * t / t[-1])
    y = np.zeros(n)
    acc = 0.0
    for i in range(n):
        a = np.exp(-2 * np.pi * cut[i] / SFX_RATE)
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    return y * np.sin(np.pi * t / t[-1]) * 1.2


def sfx_turn():
    """Temple-bell 'your turn' cue."""
    n = int(1.1 * SFX_RATE)
    t = np.arange(n) / SFX_RATE
    y = np.zeros(n)
    for ratio, amp, dec in ((1.0, 1.0, 3), (2.76, 0.5, 5), (5.4, 0.3, 8), (8.9, 0.15, 12)):
        y += amp * np.sin(2 * np.pi * 523 * ratio * t) * np.exp(-t * dec)
    return y * np.minimum(t / 0.004, 1) * 0.25


def fanfare(length=1.6, big=False):
    out = np.zeros(int(length * SFX_RATE))
    seq = [(62, 0.0), (66, 0.12), (69, 0.24), (74, 0.36), (78, 0.48), (81, 0.6)]
    if big:
        seq += [(86, 0.78), (90, 0.9), (93, 1.02)]
    for note, start in seq:
        s = int(start * SFX_RATE)
        n = len(out) - s
        t = np.arange(n) / SFX_RATE
        f = midi_hz(note)
        tone = np.zeros(n)
        for h in range(1, 12):
            tone += np.sin(2 * np.pi * f * h * t) / h * np.exp(-t * (2.5 + h * 0.8))
        out[s:] += tone * 0.18
    # Final chord swell.
    s = int((1.0 if big else 0.72) * SFX_RATE)
    n = len(out) - s
    t = np.arange(n) / SFX_RATE
    for note in (74, 78, 81, 86):
        out[s:] += np.sin(2 * np.pi * midi_hz(note) * t) * np.exp(-t * 1.6) * 0.1
    return out


SFX = [
    ("MOVE", sfx_tick),
    ("SELECT", lambda: sfx_pop(True)),
    ("DESELECT", lambda: sfx_pop(False)),
    ("CONFIRM", sfx_confirm),
    ("ERROR", sfx_error),
    ("CARD", sfx_card),
    ("FLIP", sfx_flip),
    ("COIN", sfx_coin),
    ("BONUS", sfx_bonus),
    ("CAMEL", sfx_camel),
    ("SHUFFLE", sfx_shuffle),
    ("WHOOSH", sfx_whoosh),
    ("TURN", sfx_turn),
    ("FANFARE", lambda: fanfare(1.6)),
    ("VICTORY", lambda: fanfare(2.6, True)),
]


# ======================================================================
# Music
# ======================================================================

BPM = 100.0
BEAT = 60.0 / BPM
BARS = 32
LOOP_SECONDS = BARS * 4 * BEAT
N = int(round(LOOP_SECONDS * MUSIC_RATE))


class Mix:
    def __init__(self, n=None):
        self.n = N if n is None else n
        self.dry = np.zeros((self.n, 2))
        self.wet = np.zeros((self.n, 2))

    def add(self, start_sec, sig, pan=0.0, gain=1.0, reverb=0.0):
        """Adds wrapping around the loop so tails continue seamlessly."""
        n = self.n
        s = int(round(start_sec * MUSIC_RATE)) % n
        lg = gain * np.cos((pan + 1) * np.pi / 4) * np.sqrt(2)
        rg = gain * np.sin((pan + 1) * np.pi / 4) * np.sqrt(2)
        idx = (np.arange(len(sig)) + s) % n
        np.add.at(self.dry[:, 0], idx, sig * lg * (1 - reverb * 0.3))
        np.add.at(self.dry[:, 1], idx, sig * rg * (1 - reverb * 0.3))
        if reverb:
            np.add.at(self.wet[:, 0], idx, sig * lg * reverb)
            np.add.at(self.wet[:, 1], idx, sig * rg * reverb)


def beat_t(bar, beat):
    return (bar * 4 + beat) * BEAT


def tanpura(f, dur):
    n = int(dur * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    y = np.zeros(n)
    peak = 6 + 10 * (0.5 + 0.5 * np.sin(2 * np.pi * 0.35 * t))  # jawari sweep
    for h in range(1, 28):
        amp = (1.0 / h) * (0.35 + np.exp(-((h - peak) ** 2) / 8.0))
        y += amp * np.sin(2 * np.pi * f * h * t * (1 + 0.0004 * h))
    env = np.minimum(t / 0.03, 1) * np.exp(-t * 0.7)
    return y * env * 0.08


def sitar(notes):
    """Additive plucked string with buzzing bridge, glides via note tuples.

    notes: list of (start, dur, midi, glide_from) in seconds/midi.
    """
    out = []
    for start, dur, note, glide in notes:
        n = int((dur + 1.2) * MUSIC_RATE)
        t = np.arange(n) / MUSIC_RATE
        f_target = midi_hz(note)
        if glide is not None:
            f_start = midi_hz(glide)
            g = np.clip(t / min(0.12, dur * 0.5), 0, 1)
            f = f_start + (f_target - f_start) * (1 - (1 - g) ** 2)
        else:
            f = np.full(n, f_target)
        f = f * (1 + 0.003 * np.sin(2 * np.pi * 5.5 * t) * np.clip(t - 0.2, 0, 1))
        ph = 2 * np.pi * np.cumsum(f) / MUSIC_RATE
        y = np.zeros(n)
        sweep = 18 * np.exp(-t * 6) + 4
        for h in range(1, 24):
            amp = (1.0 / h ** 0.8) * (0.25 + np.exp(-((h - sweep) ** 2) / 10.0))
            y += amp * np.sin(h * ph) * np.exp(-t * (1.6 + h * 0.35))
        y *= np.minimum(t / 0.002, 1)
        cut = int((dur + 0.9) * MUSIC_RATE)
        if cut < n:
            y[cut:] *= np.linspace(1, 0, n - cut)
        out.append((start, y * 0.12))
    return out


def flute(start, dur, note, prev=None):
    n = int((dur + 0.15) * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    f = np.full(n, midi_hz(note))
    if prev is not None:
        g = np.clip(t / 0.08, 0, 1)
        f = midi_hz(prev) + (midi_hz(note) - midi_hz(prev)) * g
    vib = 1 + 0.008 * np.sin(2 * np.pi * 5.2 * t) * np.clip((t - 0.25) / 0.4, 0, 1)
    ph = 2 * np.pi * np.cumsum(f * vib) / MUSIC_RATE
    y = np.sin(ph) + 0.18 * np.sin(2 * ph) + 0.06 * np.sin(3 * ph)
    breath = onepole_fast(noise(n), 2500, MUSIC_RATE) * 0.25
    env = env_adsr(n, MUSIC_RATE, 0.06, 0.1, 0.8, 0.15)
    return (y + breath * (0.5 + 0.5 * np.exp(-t * 8))) * env * 0.11


def santoor(note):
    n = int(0.9 * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    f = midi_hz(note)
    y = (np.sin(2 * np.pi * f * t) + 0.5 * np.sin(2 * np.pi * f * 2.003 * t) +
         0.25 * np.sin(2 * np.pi * f * 3.01 * t)) * np.exp(-t * 5)
    y += np.sin(2 * np.pi * f * 1.004 * t) * np.exp(-t * 4) * 0.6  # course chorus
    return y * np.minimum(t / 0.001, 1) * 0.05


def tabla(stroke):
    n = int(0.6 * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    y = np.zeros(n)
    dayan_f = midi_hz(62 + 12)  # tuned to Sa
    if stroke in ("na", "dha", "ta"):
        for h, a, d in ((1, 1.0, 9), (2, 0.6, 12), (3, 0.45, 14), (4, 0.3, 18)):
            y += a * np.sin(2 * np.pi * dayan_f * h * t) * np.exp(-t * d)
        y += noise(n) * np.exp(-t * 160) * 0.5
    if stroke in ("tin", "dhi"):
        for h, a, d in ((1, 0.4, 8), (2, 1.0, 9), (3, 0.5, 11)):
            y += a * np.sin(2 * np.pi * dayan_f * h * t) * np.exp(-t * d)
        y += noise(n) * np.exp(-t * 200) * 0.4
    if stroke in ("ge", "dha", "dhi"):
        f = 82 + 38 * np.clip(t / 0.18, 0, 1)
        ph = 2 * np.pi * np.cumsum(f) / MUSIC_RATE
        y += 1.6 * np.sin(ph) * np.exp(-t * 6)
    if stroke in ("ti", "te", "ka", "ke"):
        y += noise(n) * np.exp(-t * 90) * 0.7
        y += np.sin(2 * np.pi * 380 * t) * np.exp(-t * 60) * 0.5
        if stroke in ("ka", "ke"):
            y += np.sin(2 * np.pi * 120 * t) * np.exp(-t * 40) * 0.6
    return y * 0.16


def kick():
    n = int(0.35 * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    f = 45 + 110 * np.exp(-t * 30)
    ph = 2 * np.pi * np.cumsum(f) / MUSIC_RATE
    return np.sin(ph) * np.exp(-t * 9) * 0.55


def clap():
    n = int(0.3 * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    x = noise(n)
    env = np.zeros(n)
    for o in (0.0, 0.011, 0.022):
        env += np.where(t >= o, np.exp(-(t - o) * (60 if o < 0.02 else 18)), 0)
    y = x * env
    y = y - onepole_fast(y, 900, MUSIC_RATE)
    return y * 0.22


def shaker():
    n = int(0.06 * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    x = noise(n)
    x = x - onepole_fast(x, 5000, MUSIC_RATE)
    return x * np.exp(-t * 70) * 0.09


def bass(note, dur):
    n = int((dur + 0.05) * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    f = midi_hz(note)
    ph = 2 * np.pi * f * t
    saw = np.zeros(n)
    for h in range(1, 14):
        saw += np.sin(h * ph) / h * np.exp(-t * h * 2.2)
    y = saw + 0.6 * np.sin(ph)
    env = env_adsr(n, MUSIC_RATE, 0.004, 0.12, 0.55, 0.04)
    return y * env * 0.2


def pad(notes, dur):
    n = int((dur + 0.4) * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    y = np.zeros(n)
    for note in notes:
        for det in (-0.08, 0.0, 0.08):
            f = midi_hz(note + det)
            ph = 2 * np.pi * f * t + RNG.uniform(0, 6)
            for h in range(1, 7):
                y += np.sin(h * ph) / (h * 1.5)
    env = env_adsr(n, MUSIC_RATE, 0.5, 0.3, 0.8, 0.5)
    return y * env * 0.012


CHORDS_A = [50, 48, 43, 50, 50, 48, 43, 45]            # D C G D | D C G A
CHORDS_B = [43, 50, 48, 50, 43, 50, 45, 50]            # G D C D | G D A D
TRIADS = {50: (62, 66, 69), 48: (60, 64, 67), 43: (55, 59, 62), 45: (57, 61, 64)}

SITAR_A = [
    (0, 1.0, 74, 72), (1.0, 0.5, 72, None), (1.5, 0.5, 71, None), (2.0, 1.0, 69, None),
    (3.0, 0.5, 71, None), (3.5, 0.5, 72, None), (4.0, 1.5, 74, 71), (5.5, 0.5, 76, None),
    (6.0, 1.0, 74, None), (7.0, 1.0, 69, None),
    (8.0, 0.5, 67, None), (8.5, 0.5, 69, None), (9.0, 1.0, 71, 69), (10.0, 0.5, 69, None),
    (10.5, 0.5, 67, None), (11.0, 1.0, 66, None), (12.0, 1.0, 64, None), (13.0, 0.5, 66, None),
    (13.5, 0.5, 67, None), (14.0, 2.0, 62, 64),
    (16.0, 1.0, 69, None), (17.0, 0.5, 71, None), (17.5, 0.5, 72, None), (18.0, 1.0, 74, 72),
    (19.0, 1.0, 76, None), (20.0, 1.5, 78, 76), (21.5, 0.5, 76, None), (22.0, 1.0, 74, None),
    (23.0, 1.0, 72, None), (24.0, 0.5, 71, None), (24.5, 0.5, 72, None), (25.0, 1.0, 74, None),
    (26.0, 0.5, 72, None), (26.5, 0.5, 71, None), (27.0, 1.0, 69, None), (28.0, 1.0, 71, None),
    (29.0, 1.0, 67, None), (30.0, 2.0, 69, 67),
]

FLUTE_B = [
    (0, 2, 81), (2, 1, 79), (3, 1, 78), (4, 3, 76), (7, 1, 74),
    (8, 1.5, 76), (9.5, 0.5, 78), (10, 2, 79), (12, 1, 78), (13, 1, 76), (14, 2, 74),
    (16, 2, 74), (18, 1, 76), (19, 1, 78), (20, 2, 79), (22, 1, 81), (23, 1, 83),
    (24, 1, 81), (25, 1, 79), (26, 1, 78), (27, 1, 76), (28, 4, 74),
]

THEKA = ["dha", "ge", "na", "ti", "na", "ka", "dhi", "na"]
FILL = ["dha", "ti", "te", "ka", "dha", "ti", "te", "ka", "dha", "ge", "na", "dhi",
        "na", "ka", "dha", None]


def compose():
    """Track 1, "Bazaar Groove": 100 BPM, D Khamaj, sitar and bansuri."""
    mx = Mix()
    # Tanpura drone throughout: Pa Sa Sa Sa' each bar.
    for bar in range(BARS):
        for i, note in enumerate((57, 62, 62, 50)):
            mx.add(beat_t(bar, i), tanpura(midi_hz(note), 3.2), pan=-0.3 + 0.2 * i,
                   gain=0.9, reverb=0.3)

    def section(bar):
        if bar < 4:
            return "intro"
        if bar < 12:
            return "A"
        if bar < 20:
            return "B"
        if bar < 24:
            return "break"
        return "A2"

    for bar in range(BARS):
        sec = section(bar)
        # Tabla.
        if sec != "intro" or bar >= 2:
            pattern = FILL if (bar % 4 == 3 and sec != "intro") else THEKA
            step = 0.25 if pattern is FILL else 0.5
            for i, st in enumerate(pattern):
                if st:
                    soft = 0.85 if sec == "intro" else 1.0
                    mx.add(beat_t(bar, i * step), tabla(st), pan=0.15, gain=soft, reverb=0.12)
        # Electronic groove.
        if sec in ("A", "B", "A2"):
            for b in (0, 2):
                mx.add(beat_t(bar, b), kick(), gain=1.0)
            mx.add(beat_t(bar, 2.75), kick(), gain=0.6)
            for b in (1, 3):
                mx.add(beat_t(bar, b), clap(), pan=-0.1, reverb=0.25)
            for k in range(16):
                acc = 1.0 if k % 2 else 0.55
                mx.add(beat_t(bar, k * 0.25), shaker(), pan=0.45, gain=acc)
        # Bass.
        if sec in ("A", "B", "A2", "break"):
            chords = CHORDS_B if sec == "B" else CHORDS_A
            root = chords[(bar - (4 if sec != "B" else 12)) % 8] - 12
            if sec == "break":
                root = 38
                for b in (0, 2.5):
                    mx.add(beat_t(bar, b), bass(root, 0.9), gain=0.9)
            else:
                for b, d, off in ((0, 0.7, 0), (1.0, 0.25, 0), (1.5, 0.4, 12), (2.5, 0.4, 7),
                                  (3.0, 0.4, 0), (3.5, 0.4, 10)):
                    mx.add(beat_t(bar, b), bass(root + off, d * BEAT), gain=1.0)
        # Santoor arpeggios in the intro, B section and break.
        if sec in ("intro", "B", "break"):
            chords = CHORDS_B if sec == "B" else CHORDS_A
            root = chords[bar % 8] + 24
            tri = TRIADS[chords[bar % 8]]
            arp = [tri[0] + 12, tri[1] + 12, tri[2] + 12, tri[0] + 24]
            for k in range(16):
                note = arp[(k if k % 8 < 4 else 7 - k % 8) % 4]
                g = 0.7 if sec == "B" else (1.8 if sec == "intro" else 1.0)
                mx.add(beat_t(bar, k * 0.25), santoor(note), pan=0.5 * (1 if k % 2 else -1),
                       gain=g, reverb=0.45)
        # Pads under B and A2.
        if sec in ("B", "A2"):
            chords = CHORDS_B if sec == "B" else CHORDS_A
            tri = TRIADS[chords[(bar - (12 if sec == "B" else 24)) % 8]]
            mx.add(beat_t(bar, 0), pad(tri, 4 * BEAT), gain=1.0, reverb=0.5)

    # Sitar A section (bars 4-11) and A2 (bars 24-31).
    for base in (4, 24):
        notes = [(beat_t(base, s), d * BEAT, n, g) for s, d, n, g in SITAR_A]
        for start, sig in sitar(notes):
            mx.add(start, sig, pan=-0.2, gain=1.0, reverb=0.35)
    # Bansuri over B, and a harmony line a third below in A2.
    prev = None
    for s, d, n in FLUTE_B:
        mx.add(beat_t(12, s), flute(0, d * BEAT, n, prev), pan=0.25, gain=1.0, reverb=0.5)
        prev = n
    for s, d, n, g in SITAR_A[20:]:
        mx.add(beat_t(24, s), flute(0, d * BEAT, n + 12 - 3), pan=0.35, gain=0.6, reverb=0.5)
    # Break: sitar call and response over tabla and drone.
    call = [(0, 0.5, 74, None), (0.5, 0.5, 76, None), (1, 1, 78, 76), (2, 2, 74, None),
            (4, 0.5, 69, None), (4.5, 0.5, 71, None), (5, 1, 72, None), (6, 2, 69, 67),
            (8, 0.5, 74, None), (8.5, 0.5, 76, None), (9, 0.5, 78, None), (9.5, 0.5, 79, None),
            (10, 2, 81, 79), (12, 1, 78, None), (13, 1, 76, None), (14, 2, 74, 72)]
    notes = [(beat_t(20, s), d * BEAT, n, g) for s, d, n, g in call]
    for start, sig in sitar(notes):
        mx.add(start, sig, pan=0.0, gain=1.0, reverb=0.4)

    # Simple stereo reverb: exponentially decaying noise IR, circular.
    ir_len = int(1.4 * MUSIC_RATE)
    t = np.arange(ir_len) / MUSIC_RATE
    out = mx.dry.copy()
    for ch in range(2):
        ir = noise(ir_len) * np.exp(-t * 4.0)
        ir = onepole_fast(ir, 3500, MUSIC_RATE)
        ir /= np.sqrt(np.sum(ir ** 2))
        spec = np.fft.rfft(mx.wet[:, ch], n=N) * np.fft.rfft(ir, n=N)
        out[:, ch] += np.fft.irfft(spec, n=N) * 0.45
    peak = np.max(np.abs(out))
    out = out / peak * 0.89
    # Gentle saturation for glue.
    out = np.tanh(out * 1.2) / np.tanh(1.2)
    return out


def finish_mix(mx, reverb_secs=1.4, reverb_decay=4.0, wet_gain=0.45, seed_salt=0):
    """Circular reverb, peak normalise and soft saturation (as track 1)."""
    n = mx.n
    ir_len = int(reverb_secs * MUSIC_RATE)
    t = np.arange(ir_len) / MUSIC_RATE
    out = mx.dry.copy()
    for ch in range(2):
        ir = noise(ir_len) * np.exp(-t * reverb_decay)
        ir = onepole_fast(ir, 3500, MUSIC_RATE)
        ir /= np.sqrt(np.sum(ir ** 2))
        spec = np.fft.rfft(mx.wet[:, ch], n=n) * np.fft.rfft(ir, n=n)
        out[:, ch] += np.fft.irfft(spec, n=n) * wet_gain
    out = out / np.max(np.abs(out)) * 0.89
    return np.tanh(out * 1.2) / np.tanh(1.2)


def shehnai(dur, note, prev=None):
    """Double-reed wedding oboe: buzzy, nasal, wide vibrato and slides."""
    n = int((dur + 0.12) * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    f = np.full(n, midi_hz(note))
    if prev is not None:
        g = np.clip(t / 0.09, 0, 1)
        f = midi_hz(prev) + (midi_hz(note) - midi_hz(prev)) * (1 - (1 - g) ** 2)
    vib = 1 + 0.012 * np.sin(2 * np.pi * 6.2 * t) * np.clip((t - 0.15) / 0.3, 0, 1)
    ph = 2 * np.pi * np.cumsum(f * vib) / MUSIC_RATE
    y = np.zeros(n)
    for h in range(1, 20):
        fh = midi_hz(note) * h
        formant = np.exp(-((fh - 1250.0) / 700.0) ** 2) + 0.35 * np.exp(-((fh - 2900.0) / 600.0) ** 2)
        y += (0.5 / h + formant) * np.sin(h * ph)
    y += onepole_fast(noise(n), 3000, MUSIC_RATE) * 0.12
    env = env_adsr(n, MUSIC_RATE, 0.035, 0.08, 0.85, 0.1)
    return y * env * 0.045


def dhol(stroke):
    """Barrel drum: deep 'dagga' bass side and a sharp 'tilli' stick side."""
    n = int(0.5 * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    y = np.zeros(n)
    if stroke in ("dum", "both"):
        f = 70 + 45 * np.exp(-t * 18)
        ph = 2 * np.pi * np.cumsum(f) / MUSIC_RATE
        y += np.sin(ph) * np.exp(-t * 7) * 1.0
        y += noise(n) * np.exp(-t * 60) * 0.25
    if stroke in ("tak", "both"):
        y += noise(n) * np.exp(-t * 45) * 0.55
        y += np.sin(2 * np.pi * 520 * t) * np.exp(-t * 30) * 0.35
    if stroke == "ghost":
        y += noise(n) * np.exp(-t * 80) * 0.18
    return y * 0.22


def frame_drum(stroke):
    """Daf: a big frame drum, 'dum' in the centre, jingly 'tek' at the rim."""
    n = int(0.7 * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    y = np.zeros(n)
    if stroke == "dum":
        f = 62 + 20 * np.exp(-t * 12)
        ph = 2 * np.pi * np.cumsum(f) / MUSIC_RATE
        y += np.sin(ph) * np.exp(-t * 4.5)
    else:
        x = noise(n)
        y += (x - onepole_fast(x, 4000, MUSIC_RATE)) * np.exp(-t * 18) * 0.6
        for fr in (3100, 4300, 5600):  # rings on the frame
            y += np.sin(2 * np.pi * fr * t + RNG.uniform(0, 6)) * np.exp(-t * 14) * 0.08
    return y * 0.2


def bells(count=5):
    """A camel's neck bells jingling with its stride."""
    n = int(0.9 * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    y = np.zeros(n)
    for k in range(count):
        o = int(RNG.uniform(0, 0.12) * MUSIC_RATE)
        f = RNG.uniform(2300, 3400)
        tt = t[: n - o]
        y[o:] += (np.sin(2 * np.pi * f * tt) + 0.5 * np.sin(2 * np.pi * f * 2.76 * tt)) * \
            np.exp(-tt * 9) * 0.25
    return y * 0.08


def wind(dur):
    n = int(dur * MUSIC_RATE)
    t = np.arange(n) / MUSIC_RATE
    x = onepole_fast(noise(n), 900, MUSIC_RATE)
    x -= onepole_fast(x, 200, MUSIC_RATE)
    swell = 0.5 - 0.5 * np.cos(2 * np.pi * t / dur)
    return x * swell * 0.35


def compose_spice_route():
    """Track 2, "Spice Route": 116 BPM, E Phrygian (Bhairavi colour), dhol and
    shehnai with sitar answers over a syncopated bass."""
    bpm = 116.0
    beat = 60.0 / bpm
    bars = 40
    mx = Mix(int(round(bars * 4 * beat * MUSIC_RATE)))

    def bt(bar, b):
        return (bar * 4 + b) * beat

    def sec(bar):
        if bar < 4:
            return "intro"
        if bar < 12:
            return "A"
        if bar < 20:
            return "B"
        if bar < 28:
            return "C"
        if bar < 36:
            return "A2"
        return "outro"

    for bar in range(bars):
        for i, note in enumerate((59, 64, 64, 52)):
            mx.add(bt(bar, i), tanpura(midi_hz(note), 3.0), pan=-0.25 + 0.15 * i,
                   gain=0.75, reverb=0.3)

    chords = [52, 50, 48, 50]           # E D C D  (i bVII bVI bVII)
    triads = {52: (64, 67, 71), 50: (62, 66, 69), 48: (60, 64, 67)}
    chaal = ["both", "ghost", "tak", "dum", "ghost", "tak", "dum", "tak"]
    for bar in range(bars):
        s = sec(bar)
        root = chords[bar % 4]
        # Dhol chaal: the bhangra gallop, with fills every fourth bar.
        if s != "intro" or bar >= 2:
            fill = bar % 4 == 3 and s not in ("intro", "outro")
            pattern = (["tak", "tak", "both", "tak", "both", "tak", "both", "both"] if fill
                       else chaal)
            for i, st in enumerate(pattern):
                g = 0.7 if s in ("intro", "outro") else 1.0
                mx.add(bt(bar, i * 0.5), dhol(st), pan=0.1, gain=g, reverb=0.15)
        if s in ("A", "B", "A2"):
            mx.add(bt(bar, 0), kick(), gain=0.9)
            mx.add(bt(bar, 2), kick(), gain=0.8)
            for b in (1.5, 3.5):
                mx.add(bt(bar, b), clap(), pan=-0.15, gain=0.8, reverb=0.3)
            for k in range(8):
                mx.add(bt(bar, k * 0.5 + 0.25), shaker(), pan=0.5, gain=0.8)
        if s in ("C",):
            for b in (1, 3):
                mx.add(bt(bar, b), clap(), pan=0.0, gain=1.0, reverb=0.35)
        if s not in ("intro",):
            for b, d, off in ((0, 0.4, 0), (0.75, 0.25, 12), (1.5, 0.4, 0), (2.25, 0.25, 10),
                              (2.5, 0.4, 0), (3.25, 0.25, 7), (3.5, 0.4, 12)):
                g = 0.6 if s == "outro" else 0.95
                mx.add(bt(bar, b), bass(root - 12 + off, d * beat), gain=g)
        if s in ("B", "C"):
            mx.add(bt(bar, 0), pad(triads[root], 4 * beat), gain=0.9, reverb=0.5)
        if s == "C":
            tri = triads[root]
            arp = [tri[0] + 12, tri[2] + 12, tri[1] + 12, tri[2] + 12]
            for k in range(16):
                mx.add(bt(bar, k * 0.25), santoor(arp[k % 4]), pan=0.45 if k % 2 else -0.45,
                       gain=0.9, reverb=0.45)

    theme = [
        (0, 1, 76, 74), (1, .5, 77, None), (1.5, .5, 76, None), (2, 1, 74, None),
        (3, 1, 72, None), (4, 1.5, 71, 72), (5.5, .5, 72, None), (6, 1, 71, None),
        (7, 1, 69, None), (8, .5, 67, None), (8.5, .5, 69, None), (9, 1, 71, None),
        (10, 1, 72, None), (11, 1, 74, None), (12, 2, 76, 74), (14, 1, 77, None),
        (15, 1, 76, None),
        (16, 1, 79, 77), (17, 1, 77, None), (18, 1, 76, None), (19, 1, 74, None),
        (20, 2, 72, 74), (22, 1, 71, None), (23, 1, 72, None), (24, .5, 74, None),
        (24.5, .5, 72, None), (25, 1, 71, None), (26, 1, 69, None), (27, 1, 67, None),
        (28, 1, 65, 67), (29, 1, 67, None), (30, 2, 64, 65),
    ]
    for base in (4, 28):
        for s0, d, note, prev in theme:
            mx.add(bt(base, s0), shehnai(d * beat, note, prev), pan=0.15, gain=1.0,
                   reverb=0.4)
    # Second time round the sitar doubles the theme an octave down.
    notes = [(bt(28, s0), d * beat, n - 12, p - 12 if p else None) for s0, d, n, p in theme]
    for start, sig in sitar(notes):
        mx.add(start, sig, pan=-0.35, gain=0.6, reverb=0.35)
    # B section: sitar call, shehnai answer.
    call = [(0, .5, 64, None), (.5, .5, 65, None), (1, .5, 67, None), (1.5, .5, 69, None),
            (2, 2, 71, 69), (4, .5, 72, None), (4.5, .5, 71, None), (5, 1, 69, None),
            (6, 2, 67, 65)]
    answer = [(8, 1, 76, None), (9, 1, 74, None), (10, 1, 72, None), (11, 1, 71, None),
              (12, 2, 69, 71), (14, 2, 64, 65)]
    for rep_bar in (12, 16):
        notes = [(bt(rep_bar, s0), d * beat, n, p) for s0, d, n, p in call]
        for start, sig in sitar(notes):
            mx.add(start, sig, pan=-0.2, gain=1.0, reverb=0.35)
        for s0, d, note, prev in answer:
            mx.add(bt(rep_bar, s0), shehnai(d * beat, note, prev), pan=0.2, gain=0.9,
                   reverb=0.45)
    return finish_mix(mx)


def compose_moonlit_caravan():
    """Track 3, "Moonlit Caravan": a 6/8 caravan gait in G Lydian (Yaman
    colour) - santoor, bansuri, frame drum, camel bells and desert wind."""
    eighth = 60.0 / 52.0 / 3.0           # dotted-quarter pulse at 52 BPM
    bars = 36
    bar_len = 6 * eighth
    mx = Mix(int(round(bars * bar_len * MUSIC_RATE)))

    def et(bar, e):
        return (bar * 6 + e) * eighth

    def sec(bar):
        if bar < 4:
            return "intro"
        if bar < 12:
            return "A"
        if bar < 20:
            return "B"
        if bar < 28:
            return "C"
        return "outro"

    for bar in range(bars):
        for i, note in enumerate((62, 67, 67, 55)):
            mx.add(et(bar, i * 1.5), tanpura(midi_hz(note), 3.4), pan=-0.3 + 0.2 * i,
                   gain=0.8, reverb=0.35)
    for k in range(0, bars, 4):
        mx.add(et(k, 0), wind(4 * bar_len), pan=-0.6 if (k // 4) % 2 else 0.6,
               gain=0.9, reverb=0.2)

    chords = [43, 40, 36, 38]           # G Em C D
    triads = {43: (67, 71, 74), 40: (64, 67, 71), 36: (60, 64, 67), 38: (62, 66, 69)}
    for bar in range(bars):
        s = sec(bar)
        root = chords[(bar // 2) % 4]
        tri = triads[root]
        # Santoor ostinato rocking in eighths.
        arp = [tri[0] + 12, tri[1] + 12, tri[2] + 12, tri[1] + 24, tri[2] + 12, tri[1] + 12]
        g = 0.8 if s in ("A", "C") else 1.1
        for e in range(6):
            mx.add(et(bar, e), santoor(arp[e]), pan=0.5 if e % 2 else -0.5, gain=g,
                   reverb=0.5)
        # Frame drum dadra: dum . tek dum tek tek
        if s != "intro" or bar >= 2:
            for e, st in ((0, "dum"), (2, "tek"), (3, "dum"), (4, "tek"), (5, "tek")):
                mx.add(et(bar, e), frame_drum(st), pan=-0.1, gain=0.9 if st == "dum" else 0.7,
                       reverb=0.25)
        if s in ("B", "C"):
            for e, st in enumerate(["dha", None, "na", "dha", "ti", "na"]):
                if st:
                    mx.add(et(bar, e), tabla(st), pan=0.2, gain=0.55, reverb=0.2)
            mx.add(et(bar, 0), bells(), pan=0.6, gain=1.0, reverb=0.4)
            mx.add(et(bar, 3), bells(3), pan=0.5, gain=0.7, reverb=0.4)
        if s != "intro":
            mx.add(et(bar, 0), bass(root - 12, 2.6 * eighth), gain=0.75)
            mx.add(et(bar, 3), bass(root - 5, 1.6 * eighth), gain=0.55)
        if bar % 2 == 0 and s in ("A", "B", "C"):
            mx.add(et(bar, 0), pad(tri, 2 * bar_len), gain=1.1, reverb=0.6)

    a = [(0, 3, 74), (3, 1.5, 76), (4.5, 1.5, 78), (6, 4, 79), (10, 2, 78),
         (12, 2, 76), (14, 2, 73), (16, 2, 74), (18, 6, 71),
         (24, 3, 74), (27, 1.5, 76), (28.5, 1.5, 78), (30, 3, 81), (33, 3, 79),
         (36, 2, 78), (38, 2, 76), (40, 2, 73), (42, 6, 74)]
    prev = None
    for s0, d, note in a:
        mx.add(et(4, s0), flute(0, d * eighth, note, prev), pan=0.2, gain=1.1, reverb=0.55)
        prev = note
    # B: sitar sings the tune lower, with bells and tabla underneath.
    notes = [(et(12, s0), d * eighth, n - 12, None) for s0, d, n in a]
    for start, sig in sitar(notes):
        mx.add(start, sig, pan=-0.2, gain=1.0, reverb=0.4)
    # C: bansuri up high with a counter-line, then home.
    c_line = [(0, 3, 81), (3, 3, 83), (6, 4, 86), (10, 2, 85), (12, 3, 83), (15, 3, 81),
              (18, 6, 79), (24, 2, 78), (26, 2, 79), (28, 2, 81), (30, 6, 83),
              (36, 3, 81), (39, 3, 78), (42, 6, 79)]
    prev = None
    for s0, d, note in c_line:
        mx.add(et(20, s0), flute(0, d * eighth, note, prev), pan=0.25, gain=1.0, reverb=0.6)
        prev = note
    notes = [(et(20, s0), d * eighth, n - 12, None) for s0, d, n in a]
    for start, sig in sitar(notes):
        mx.add(start, sig, pan=-0.3, gain=0.55, reverb=0.45)
    return finish_mix(mx, reverb_secs=1.8, reverb_decay=3.2, wet_gain=0.5)


TRACKS = [
    ("Bazaar Groove", compose),
    ("Spice Route", compose_spice_route),
    ("Moonlit Caravan", compose_moonlit_caravan),
]
CROSSFADE = 2.0


def build_playlist():
    """Render every track and join them into one circular, crossfaded loop.

    Returns (audio, starts) where starts[i] is the frame where track i
    becomes the main voice. Tracks are matched in loudness to track 1."""
    rendered = []
    for name, fn in TRACKS:
        audio = fn()
        rendered.append((name, audio))
        print(f"  {name}: {len(audio) / MUSIC_RATE:.1f}s, "
              f"rms {np.sqrt(np.mean(audio ** 2)):.3f}")
    ref = np.sqrt(np.mean(rendered[0][1] ** 2))
    xf = int(CROSSFADE * MUSIC_RATE)
    total = sum(len(a) - xf for _, a in rendered)
    out = np.zeros((total, 2))
    starts = []
    pos = 0
    ramp = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, xf))
    for name, audio in rendered:
        a = audio * (ref / np.sqrt(np.mean(audio ** 2)))
        a = np.tanh(a * 1.05) / np.tanh(1.05)
        a = a.copy()
        a[:xf] *= ramp[:, None]
        a[-xf:] *= ramp[::-1, None]
        idx = (np.arange(len(a)) + pos) % total
        np.add.at(out[:, 0], idx, a[:, 0])
        np.add.at(out[:, 1], idx, a[:, 1])
        starts.append((pos + xf // 2) % total)
        pos += len(a) - xf
    peak = np.max(np.abs(out))
    if peak > 0.97:
        out *= 0.97 / peak
    return out, starts


# ======================================================================
# Output
# ======================================================================

def write_wav(path, data, rate, channels):
    pcm = np.clip(np.rint(data * 32767), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--skip-music", action="store_true")
    ap.add_argument("--preview-wav", type=Path)
    args = ap.parse_args()
    out = args.output_dir
    out.mkdir(parents=True, exist_ok=True)

    pak = bytearray()
    entries = []
    for name, fn in SFX:
        sig = np.asarray(fn(), dtype=np.float64)
        peak = np.max(np.abs(sig))
        if peak > 0.98:
            sig = sig / peak * 0.98
        pcm = np.clip(np.rint(sig * 32767), -32768, 32767).astype("<i2")
        if len(pcm) % 16:
            pcm = np.concatenate([pcm, np.zeros(16 - len(pcm) % 16, "<i2")])
        while len(pak) % 32:
            pak.append(0)
        entries.append((name, len(pak), len(pcm) * 2))
        pak += pcm.tobytes()
    (out / "sfx.pak").write_bytes(bytes(pak))

    h = ["/* Generated by tools/build_audio.py. Do not edit by hand. */",
         "#ifndef BAZAAR_SOUND_ASSETS_H", "#define BAZAAR_SOUND_ASSETS_H", "",
         "#include <stdint.h>", "", f"#define JP_SFX_RATE {SFX_RATE}",
         f"#define JP_MUSIC_RATE {MUSIC_RATE}", "", "typedef enum {"]
    for i, (name, _, _) in enumerate(entries):
        h.append(f"    JP_SFX_{name} = {i},")
    h += [f"    JP_SFX_COUNT = {len(entries)}", "} jp_sfx_id_t;", "",
          "typedef struct { uint32_t offset; uint32_t bytes; } jp_sfx_asset_t;", "",
          "extern const jp_sfx_asset_t jp_sfx_assets[JP_SFX_COUNT];",
          "extern const uint8_t jp_sfx_pak[];",
          "extern const uint8_t jp_music_adpcm[];",
          "extern const uint8_t jp_music_adpcm_end[];", "",
          f"#define JP_MUSIC_TRACKS {len(TRACKS)}",
          "/* Stereo ADPCM: one byte per frame, so frames are byte offsets. */",
          "extern const uint32_t jp_music_track_start[JP_MUSIC_TRACKS];",
          "extern const char *const jp_music_track_name[JP_MUSIC_TRACKS];", "",
          "#endif", ""]
    (out / "sound_assets.h").write_text("\n".join(h))
    c = ["/* Generated by tools/build_audio.py. Do not edit by hand. */",
         '#include "sound_assets.h"', "",
         "const jp_sfx_asset_t jp_sfx_assets[JP_SFX_COUNT] = {"]
    for name, off, size in entries:
        c.append(f"    {{ {off}u, {size}u }}, /* {name} */")
    c.append("};")
    print(f"sfx.pak: {len(pak)} bytes, {len(entries)} effects")

    if args.skip_music:
        prev = (out / "sound_assets.c").read_text()
        keep = prev[prev.index("const uint32_t jp_music_track_start"):]
        (out / "sound_assets.c").write_text("\n".join(c) + "\n\n" + keep)
        return
    loop, starts = build_playlist()
    c.append("")
    c.append("const uint32_t jp_music_track_start[JP_MUSIC_TRACKS] = { " +
             ", ".join(f"{s}u" for s in starts) + " };")
    c.append("const char *const jp_music_track_name[JP_MUSIC_TRACKS] = { " +
             ", ".join(f'"{name}"' for name, _ in TRACKS) + " };")
    (out / "sound_assets.c").write_text("\n".join(c) + "\n")
    if args.preview_wav:
        write_wav(args.preview_wav, np.concatenate([loop, loop]), MUSIC_RATE, 2)
    # Encode three copies continuously and keep the middle one, so the
    # ADPCM predictor state at the loop seam matches its own start.
    with tempfile.TemporaryDirectory() as td:
        wav_path = Path(td) / "loop3.wav"
        adpcm_path = Path(td) / "loop3.adpcm"
        write_wav(wav_path, np.concatenate([loop, loop, loop]), MUSIC_RATE, 2)
        subprocess.run([WAV2ADPCM, "-n", "-i", "-t", str(wav_path), str(adpcm_path)],
                       check=True, stdout=subprocess.DEVNULL)
        data = adpcm_path.read_bytes()
    frames = len(loop)
    if len(data) < frames * 3:
        raise SystemExit(f"unexpected ADPCM size {len(data)} for {frames} frames")
    middle = data[frames:frames * 2]
    (out / "bazaar_music.adpcm").write_bytes(middle)
    print(f"bazaar_music.adpcm: {len(middle)} bytes, {frames / MUSIC_RATE:.1f}s playlist "
          f"of {len(TRACKS)} tracks")


if __name__ == "__main__":
    main()
