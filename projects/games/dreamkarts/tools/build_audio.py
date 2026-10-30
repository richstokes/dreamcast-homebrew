#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "scipy>=1.11"]
# ///
"""Synthesize DreamKarts' sound effects.

Everything is generated from code with seeded RNGs, so the output is
original and byte-for-byte reproducible. Outputs in --output-dir:

    sfx.pak          16-bit mono PCM effects at 22050 Hz, 32-byte aligned
    sound_assets.h   effect table
    previews/*.wav   listening copies (git-ignored)

The music is CD-DA (tools/build_music.py); the instrument models below
survive from the songs that used to be composed here because a few
effects (the fanfares, the lobby bell) are built from them.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
import wave
import zlib
from functools import lru_cache
from pathlib import Path

import numpy as np
from scipy.signal import fftconvolve, lfilter, resample_poly

SFX_RATE = 22050
TAU = 2.0 * np.pi
ONESHOT_PEAK = 10 ** (-1.0 / 20)   # -1 dBFS
LOOP_PEAK = 10 ** (-4.0 / 20)      # -4 dBFS
MAX_SFX_SAMPLES = 65534            # AICA 16-bit length limit (KOS warns above)


def rng_for(name: str) -> np.random.Generator:
    """Independent seeded stream per asset, so editing one leaves others alone."""
    return np.random.default_rng(zlib.crc32(name.encode()) + 2026)


def midi_hz(n):
    return 440.0 * 2.0 ** ((np.asarray(n, dtype=np.float64) - 69) / 12.0)


# ======================================================================
# DSP helpers
# ======================================================================

def rbj(kind, fc, q, sr, gain_db=0.0):
    fc = min(max(fc, 5.0), 0.49 * sr)
    w0 = TAU * fc / sr
    cw, sw = np.cos(w0), np.sin(w0)
    alpha = sw / (2 * q)
    A = 10 ** (gain_db / 40)
    if kind == "lp":
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
        a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "hp":
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
        a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "bp":      # 0 dB peak gain
        b = [alpha, 0.0, -alpha]
        a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "peak":
        b = [1 + alpha * A, -2 * cw, 1 - alpha * A]
        a = [1 + alpha / A, -2 * cw, 1 - alpha / A]
    else:
        raise ValueError(kind)
    b = np.array(b) / a[0]
    a = np.array(a) / a[0]
    return b, a


def filt(x, kind, fc, q=0.7071, sr=SFX_RATE, gain_db=0.0):
    b, a = rbj(kind, fc, q, sr, gain_db)
    return lfilter(b, a, x, axis=0)


def periodic(fn, x):
    """Apply a (linear or memoryless) process to one period of a periodic
    signal and return its steady state, so the result loops seamlessly."""
    n = len(x)
    reps = np.concatenate([x, x, x], axis=0)
    return fn(reps)[n:2 * n]


def svf(x, fc, q, sr, mode="lp"):
    """Time-varying TPT state-variable filter (per-sample coefficients)."""
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    fc = np.broadcast_to(np.asarray(fc, dtype=np.float64), (n,))
    q = np.broadcast_to(np.asarray(q, dtype=np.float64), (n,))
    g = np.tan(np.pi * np.clip(fc, 10.0, 0.45 * sr) / sr)
    k = 1.0 / q
    a1 = 1.0 / (1.0 + g * (g + k))
    a2 = g * a1
    a3 = g * a2
    xs, A1, A2, A3, K = x.tolist(), a1.tolist(), a2.tolist(), a3.tolist(), k.tolist()
    ic1 = ic2 = 0.0
    out = [0.0] * n
    sel = {"lp": 0, "bp": 1, "hp": 2}[mode]
    for i in range(n):
        v3 = xs[i] - ic2
        v1 = A1[i] * ic1 + A2[i] * v3
        v2 = ic2 + A2[i] * ic1 + A3[i] * v3
        ic1 = 2.0 * v1 - ic1
        ic2 = 2.0 * v2 - ic2
        if sel == 0:
            out[i] = v2
        elif sel == 1:
            out[i] = v1
        else:
            out[i] = xs[i] - K[i] * v1 - v2
    return np.array(out)


def lp2_mag(f, fc, q):
    """Magnitude of a resonant 2-pole low-pass at frequency f (vectorised)."""
    x = f / fc
    return 1.0 / np.sqrt((1 - x * x) ** 2 + (x / q) ** 2)


def additive(f, sr, amp_fn, hmax=200, phase=None):
    """Band-limited harmonic oscillator. f is per-sample fundamental (Hz);
    amp_fn(h) returns a scalar or per-sample amplitude for harmonic h.
    Harmonics fade out smoothly as they approach Nyquist."""
    f = np.asarray(f, dtype=np.float64)
    ph = TAU * np.cumsum(f) / sr if phase is None else phase
    out = np.zeros(len(f))
    fmin = max(float(np.min(f)), 1.0)
    for h in range(1, hmax + 1):
        if h * fmin > 0.46 * sr:
            break
        a = amp_fn(h)
        if np.isscalar(a) and a == 0:
            continue
        lim = np.clip((0.45 * sr - h * f) / (0.05 * sr), 0.0, 1.0)
        out += a * lim * np.sin(h * ph)
    return out


def fix_cycles(f, sr):
    """Scale a frequency track so it completes a whole number of cycles."""
    c = np.sum(f) / sr
    return f * (max(1, round(c)) / c)


def plfo(n, rate, rng, sr=SFX_RATE):
    """Periodic, band-limited random LFO in [-1, 1] that repeats every n."""
    k = max(1, int(rate * n / sr))
    spec = np.zeros(n // 2 + 1, dtype=complex)
    spec[1:k + 1] = rng.standard_normal(k) + 1j * rng.standard_normal(k)
    x = np.fft.irfft(spec, n=n)
    return x / (np.max(np.abs(x)) + 1e-12)


def raised(n):
    return 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n))


def fade(x, in_s=0.0007, out_s=0.015, sr=SFX_RATE):
    x = x.copy()
    a = max(2, int(in_s * sr))
    b = max(2, int(out_s * sr))
    a = min(a, len(x))
    b = min(b, len(x))
    x[:a] *= raised(a)
    x[-b:] *= raised(b)[::-1]
    return x


def pad_to(x, n):
    if len(x) >= n:
        return x[:n]
    return np.concatenate([x, np.zeros(n - len(x))])


def fit(x, n, sr=SFX_RATE):
    """Pad or truncate to n samples, fading out a truncated tail."""
    if len(x) > n:
        return fade(x[:n], 0.0, min(0.015, 0.3 * n / sr), sr)
    return pad_to(x, n)


def place(buf, start, sig, gain=1.0):
    """Add sig into buf at sample start (clipped to buf)."""
    s = int(start)
    if s >= len(buf):
        return
    m = min(len(sig), len(buf) - s)
    buf[s:s + m] += sig[:m] * gain


def place_wrap(buf, start, sig, gain=1.0):
    """Add sig into a circular buffer (tails wrap round the loop)."""
    n = len(buf)
    s = int(start) % n
    pos = 0
    while pos < len(sig):
        take = min(len(sig) - pos, n - s)
        buf[s:s + take] += sig[pos:pos + take] * gain
        pos += take
        s = 0


def make_ir(sr, seconds, t60, rng, lp_hz=7000.0, hp_hz=180.0, predelay=0.012, hf_ratio=0.5):
    """Synthetic diffuse reverb impulse with frequency-dependent decay."""
    n = int(seconds * sr)
    t = np.arange(n) / sr
    nz = rng.standard_normal(n)
    lo = filt(nz, "lp", 1800, sr=sr)
    hi = nz - lo
    ir = lo * 10 ** (-3 * t / t60) + hi * 10 ** (-3 * t / (t60 * hf_ratio))
    ir = filt(ir, "lp", lp_hz, sr=sr)
    ir = filt(ir, "hp", hp_hz, sr=sr)
    ir *= np.minimum(t / 0.02, 1.0)
    ir /= np.sqrt(np.sum(ir ** 2))
    er = np.zeros(n)
    for d, g in ((0.006, 0.55), (0.011, 0.4), (0.017, 0.35), (0.023, 0.3), (0.031, 0.22),
                 (0.043, 0.18)):
        er[int(d * sr)] += g * rng.choice([-1.0, 1.0])
    ir = ir + filt(er, "lp", 5000, sr=sr) * 0.6
    ir /= np.sqrt(np.sum(ir ** 2))
    return np.concatenate([np.zeros(int(predelay * sr)), ir])


def add_reverb(x, wet, seconds=1.2, t60=1.0, seed="rev", sr=SFX_RATE, lp_hz=6500.0):
    ir = make_ir(sr, seconds, t60, rng_for(seed), lp_hz=lp_hz)
    out = np.concatenate([x, np.zeros(len(ir))])
    out += wet * pad_to(fftconvolve(x, ir), len(out))
    return out


def softsat(x, drive=1.0):
    return np.tanh(drive * x) / np.tanh(drive)


# ======================================================================
# Shared instruments (used by the music at 32 kHz and fanfares at 22 kHz)
# ======================================================================

@lru_cache(maxsize=None)
def steelpan(note, dur, sr):
    """Steel drum: fundamental plus a blooming octave, slight strike bend."""
    f = float(midi_hz(note))
    n = int((dur + 1.0) * sr)
    t = np.arange(n) / sr
    bend = 1 + 0.010 * np.exp(-t / 0.010)
    ph = TAU * np.cumsum(f * bend) / sr
    k = 1.0 + f / 900.0
    y = np.sin(ph) * np.exp(-t * 2.2 * k)
    y += 0.62 * np.sin(2 * ph + 0.4) * (1 - np.exp(-t / 0.011)) * np.exp(-t * 3.3 * k)
    y += 0.10 * np.sin(2.007 * ph) * np.exp(-t * 2.6 * k)
    for r, a, d in ((3.0, 0.24, 6.0), (4.0, 0.12, 9.0), (5.02, 0.05, 14.0), (6.0, 0.03, 18.0)):
        if r * f < 0.44 * sr:
            y += a * np.sin(r * ph + r) * np.exp(-t * d * k)
    rng = rng_for(f"steel-mallet-{sr}")
    m = int(0.004 * sr)
    tick = filt(rng.standard_normal(m), "bp", min(2.5 * f, 0.4 * sr), 0.8, sr=sr)
    y[:m] += tick * np.exp(-np.arange(m) / (0.0012 * sr)) * 0.25
    y *= np.minimum(t / 0.0015, 1.0)
    rel = dur + 0.18
    y *= np.where(t > rel, np.exp(-(t - rel) * 7.0), 1.0)
    return fade(y, 0.0, 0.05, sr)


@lru_cache(maxsize=None)
def marimba(note, sr):
    f = float(midi_hz(note))
    n = int(0.9 * sr)
    t = np.arange(n) / sr
    k = 1.0 + f / 700.0
    y = np.sin(TAU * f * t) * np.exp(-t * 3.6 * k)
    y += 0.20 * np.sin(TAU * f * 3.93 * t + 0.7) * np.exp(-t * 14 * k)
    if 9.2 * f < 0.44 * sr:
        y += 0.05 * np.sin(TAU * f * 9.2 * t) * np.exp(-t * 45)
    thump = np.sin(TAU * f * 0.5 * t) * np.exp(-t / 0.006) * 0.25
    y = (y + thump) * np.minimum(t / 0.001, 1.0)
    return fade(y, 0.0, 0.05, sr)


@lru_cache(maxsize=None)
def glock(note, sr, length=1.4):
    f = float(midi_hz(note))
    n = int(length * sr)
    t = np.arange(n) / sr
    y = np.zeros(n)
    for r, a, d in ((1.0, 1.0, 1.8), (2.71, 0.38, 4.5), (5.15, 0.14, 9.0), (8.43, 0.06, 16.0)):
        if r * f < 0.44 * sr:
            y += a * np.sin(TAU * f * r * t + r) * np.exp(-t * d)
    y *= np.minimum(t / 0.0008, 1.0)
    return fade(y, 0.0, 0.08, sr)


@lru_cache(maxsize=None)
def rhodes(note, dur, sr, vel=0.8):
    """Two-operator FM electric piano with a tine 'bark'."""
    f = float(midi_hz(note))
    n = int((dur + 0.5) * sr)
    t = np.arange(n) / sr
    idx = vel * (1.5 * np.exp(-t / 0.28) + 0.3)
    body = np.sin(TAU * f * t + idx * np.sin(TAU * f * t + 0.3))
    tine_i = 0.8 * vel * np.exp(-t / 0.025)
    tine = np.sin(TAU * f * t + tine_i * np.sin(TAU * 14.0 * f * t)) - np.sin(TAU * f * t)
    y = body + 0.6 * tine
    y *= np.minimum(t / 0.0015, 1.0) * np.exp(-t * (0.9 + f / 900.0))
    r = int(min(dur, (n / sr) - 0.1) * sr)
    rl = int(0.09 * sr)
    y[r:r + rl] *= raised(rl)[::-1][:len(y[r:r + rl])]
    y[r + rl:] = 0
    return y


@lru_cache(maxsize=None)
def slap_bass(note, dur, sr, kind="n"):
    """Resonant-filter synth/slap bass rendered additively (alias-free).

    kind: 'n' thumb note, 'pop' bright popped octave, 'ghost' muted dead note."""
    f = float(midi_hz(note))
    if kind == "ghost":
        dur = min(dur, 0.06)
    n = int((dur + 0.05) * sr)
    t = np.arange(n) / sr
    pop = kind == "pop"
    fenv = np.exp(-t / (0.045 if pop else 0.075))
    fc = f * (1.7 + (15.0 if pop else 7.0) * fenv)
    q = 1.9 if pop else 1.5
    bend = 1 + (0.015 if pop else 0.006) * np.exp(-t / 0.008)
    ph = TAU * np.cumsum(f * bend) / sr
    hmax = int(min(0.45 * sr / f, 90))
    y = additive(f * bend, sr, lambda h: lp2_mag(h * f, fc, q) / h, hmax=hmax, phase=ph)
    y += 0.55 * np.sin(ph)
    if kind == "ghost":
        rng = rng_for(f"ghost-{sr}")
        y = y * 0.35 + filt(rng.standard_normal(n), "lp", 900, sr=sr) * np.exp(-t / 0.012) * 0.5
    env = np.minimum(t / 0.0012, 1.0) * (0.6 + 0.4 * np.exp(-t / 0.12))
    if kind == "ghost":
        env *= np.exp(-t / 0.02)
    y *= env
    if pop:
        rng = rng_for(f"pop-{sr}")
        click = filt(rng.standard_normal(int(0.006 * sr)), "hp", 1800, sr=sr)
        y[:len(click)] += click * np.exp(-np.arange(len(click)) / (0.0015 * sr)) * 0.4
    r = int(dur * sr)
    rl = int(0.025 * sr)
    if r + rl < n:
        y[r:r + rl] *= raised(rl)[::-1]
        y[r + rl:] = 0
    return softsat(y * 0.8, 1.4)


@lru_cache(maxsize=None)
def brass_note(note, dur, sr, bright=1.0):
    """Three detuned saw voices with a swelling filter: a brass section."""
    f0 = float(midi_hz(note))
    n = int((dur + 0.12) * sr)
    t = np.arange(n) / sr
    out = np.zeros((n, 2))
    fa = 1 - np.exp(-t / 0.022)
    fe = fa * (0.45 + 0.55 * np.exp(-t / 0.16))
    vib = 1 + 0.004 * np.sin(TAU * 5.4 * t) * np.clip((t - 0.25) / 0.2, 0, 1)
    scoop = 1 - 0.012 * np.exp(-t / 0.03)
    for det, pan in ((-7.0, -0.55), (0.0, 0.0), (7.0, 0.55)):
        f = f0 * 2 ** (det / 1200) * vib * scoop
        fc = f0 * (1.1 + 6.0 * bright * fe)
        hmax = int(min(0.45 * sr / f0, 60))
        y = additive(f, sr, lambda h: lp2_mag(h * f0, fc, 0.9) / h, hmax=hmax)
        a = (pan + 1) * np.pi / 4
        out[:, 0] += y * np.cos(a)
        out[:, 1] += y * np.sin(a)
    env = np.minimum(t / 0.014, 1.0) * (0.75 + 0.25 * np.exp(-t / 0.1))
    r = int(dur * sr)
    rl = int(0.07 * sr)
    env[r:r + rl] *= raised(rl)[::-1][:len(env[r:r + rl])]
    env[r + rl:] = 0
    return out * env[:, None] * 0.5


@lru_cache(maxsize=None)
def pad_chord(notes, dur, sr):
    n = int((dur + 0.6) * sr)
    t = np.arange(n) / sr
    out = np.zeros((n, 2))
    for i, note in enumerate(notes):
        f = float(midi_hz(note))
        for ch, det in ((0, -6.0), (1, 6.0)):
            fv = f * 2 ** ((det + (-1) ** i * 2.0) / 1200)
            hmax = int(min(0.45 * sr / fv, 40))
            y = additive(np.full(n, fv), sr, lambda h: lp2_mag(h * f, 1600.0, 0.7) / h,
                         hmax=hmax)
            out[:, ch] += y
    env = np.clip(t / 0.35, 0, 1) ** 1.5
    r = int(dur * sr)
    rl = int(0.5 * sr)
    env[r:r + rl] *= raised(rl)[::-1][:len(env[r:r + rl])]
    env[r + rl:] = 0
    return out * env[:, None] * 0.25


@lru_cache(maxsize=None)
def lead_synth(note, dur, sr, bright=1.0):
    """Fusion lead: two detuned saws and a square an octave down through a
    resonant low-pass that opens on the attack, with a pitch scoop and
    delayed vibrato, in the manner of a late-80s lyricon or synth lead."""
    f0 = float(midi_hz(note))
    n = int((dur + 0.15) * sr)
    t = np.arange(n) / sr
    vib = 1 + 0.0055 * np.sin(TAU * 5.6 * t) * np.clip((t - 0.18) / 0.25, 0, 1)
    scoop = 1 - 0.018 * np.exp(-t / 0.025)
    fc = f0 * (2.0 + 5.5 * bright * np.exp(-t / 0.2))
    hmax = int(min(0.45 * sr / f0, 48))
    out = np.zeros((n, 2))
    for det, pan in ((-7.0, -0.4), (7.0, 0.4)):
        f = f0 * 2 ** (det / 1200) * vib * scoop
        y = additive(f, sr, lambda h: lp2_mag(h * f0, fc, 1.25) / h, hmax=hmax)
        a = (pan + 1) * np.pi / 4
        out[:, 0] += y * np.cos(a)
        out[:, 1] += y * np.sin(a)
    sub = additive(0.5 * f0 * vib * scoop, sr,
                   lambda h: (lp2_mag(h * 0.5 * f0, fc, 0.8) / h) if h % 2 else 0, hmax=2 * hmax)
    out += 0.35 * sub[:, None]
    env = np.minimum(t / 0.006, 1.0) * (0.82 + 0.18 * np.exp(-t / 0.09))
    r = int(dur * sr)
    rl = int(0.08 * sr)
    env[r:r + rl] *= raised(rl)[::-1][:len(env[r:r + rl])]
    env[r + rl:] = 0
    return out * env[:, None] * 0.42


@lru_cache(maxsize=None)
def flute(note, dur, sr):
    """Breathy wooden flute: a near-sine tone with a chiff on the attack,
    breath noise around the pitch and a vibrato that blooms on held notes."""
    f0 = float(midi_hz(note))
    n = int((dur + 0.2) * sr)
    t = np.arange(n) / sr
    bloom = np.clip((t - 0.18) / 0.3, 0, 1)
    vib = 1 + 0.006 * np.sin(TAU * 5.1 * t + 0.4) * bloom
    bend = 1 - 0.014 * np.exp(-t / 0.03)
    ph = TAU * np.cumsum(f0 * vib * bend) / sr
    y = np.sin(ph) + 0.24 * np.sin(2 * ph + 0.3) + 0.09 * np.sin(3 * ph + 1.1)
    if 4 * f0 < 0.44 * sr:
        y += 0.03 * np.sin(4 * ph + 0.6)
    rng = rng_for(f"flute-{note}-{sr}")
    nz = rng.standard_normal(n)
    breath = (filt(nz, "bp", min(f0, 0.4 * sr), 3.0, sr=sr) * 0.5 +
              filt(nz, "hp", 2500, sr=sr) * 0.03)
    chiff = filt(nz, "bp", min(3.0 * f0, 0.4 * sr), 1.5, sr=sr) * np.exp(-t / 0.018) * 0.45
    env = (1 - np.exp(-t / 0.03)) * (1 + 0.07 * np.sin(TAU * 5.1 * t + 0.4) * bloom)
    y = (y + breath) * env + chiff
    r = int(dur * sr)
    rl = int(0.1 * sr)
    y[r:r + rl] *= raised(rl)[::-1][:len(y[r:r + rl])]
    y[r + rl:] = 0
    return fade(y, 0.0, 0.02, sr) * 0.6


@lru_cache(maxsize=None)
def kalimba(note, sr):
    """Thumb piano: a bright tine with an inharmonic overtone and a click."""
    f = float(midi_hz(note))
    n = int(1.1 * sr)
    t = np.arange(n) / sr
    y = np.sin(TAU * f * t) * np.exp(-t * (2.6 + f / 800.0))
    for r, a, d in ((5.93, 0.32, 22.0), (2.0, 0.08, 6.0), (13.1, 0.08, 60.0)):
        if r * f < 0.44 * sr:
            y += a * np.sin(TAU * f * r * t + r) * np.exp(-t * d)
    rng = rng_for(f"kalimba-{sr}")
    m = int(0.003 * sr)
    y[:m] += filt(rng.standard_normal(m), "bp", 3000, 1.0, sr=sr) * np.exp(-np.arange(m) / (0.001 * sr)) * 0.3
    return fade(y * np.minimum(t / 0.0008, 1.0), 0.0, 0.05, sr)


@lru_cache(maxsize=None)
def funk_guitar(notes, dur, sr, mute=False):
    """Clean single-coil chop: plucked strings (pick-position comb, faster
    decay up the harmonics) strummed downward a few ms apart; muted chops
    are short percussive scratches."""
    dur = 0.03 if mute else dur
    n = int((dur + 0.06) * sr)
    t = np.arange(n) / sr
    out = np.zeros(n)
    for i, note in enumerate(notes):
        f = float(midi_hz(note))
        hmax = int(min(0.44 * sr / f, 40))
        y = additive(np.full(n, f), sr,
                     lambda h: abs(np.sin(np.pi * h * 0.2)) / h ** 0.9 * np.exp(-t * (3.0 + 2.2 * h)),
                     hmax=hmax)
        lag = int(i * 0.004 * sr)
        out[lag:] += y[:n - lag]
    rng = rng_for(f"guitar-{sr}-{mute}")
    scratch = filt(rng.standard_normal(n), "bp", 2200, 0.9, sr=sr) * np.exp(-t / 0.008)
    out = out * (0.25 if mute else 1.0) + scratch * (0.9 if mute else 0.25)
    env = np.minimum(t / 0.001, 1.0)
    r = int(dur * sr)
    rl = int(0.03 * sr)
    env[r:r + rl] *= raised(rl)[::-1][:len(env[r:r + rl])]
    env[r + rl:] = 0
    return out * env


@lru_cache(maxsize=None)
def tom(f0, sr, variant=0, length=0.6):
    """Tuned tom (or, pitched low and long, a taiko): falling skin pitch,
    two overtones, stick attack."""
    n = int(length * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"tom-{f0}-{sr}-{variant}")
    bend = 1 + 0.22 * np.exp(-t / 0.035)
    ph = TAU * np.cumsum(f0 * bend) / sr
    dec = 0.16 + 18.0 / f0
    y = np.sin(ph) * np.exp(-t / dec)
    y += 0.28 * np.sin(1.52 * ph + 1.0) * np.exp(-t / (dec * 0.45))
    y += 0.12 * np.sin(2.31 * ph + 2.0) * np.exp(-t / (dec * 0.25))
    nz = rng.standard_normal(n)
    y += 0.5 * filt(nz, "bp", 1800, 0.8, sr=sr) * np.exp(-t / 0.005)
    y += 0.4 * filt(nz, "lp", 400, sr=sr) * np.exp(-t / 0.02)
    return fade(softsat(1.2 * y * np.minimum(t / 0.0005, 1), 1.2), 0.0, 0.05, sr)


@lru_cache(maxsize=None)
def poly_pad(notes, dur, sr, bright=1.0):
    """Juno-style polysynth pad: five detuned saws per note, spread across
    the stereo field, through a gentle low-pass, with a slow swell."""
    n = int((dur + 0.7) * sr)
    t = np.arange(n) / sr
    out = np.zeros((n, 2))
    fc = 2300.0 * bright
    for note in notes:
        f = float(midi_hz(note))
        for det, pan in ((-15.0, -0.8), (-6.0, -0.35), (0.0, 0.0), (6.0, 0.35), (15.0, 0.8)):
            fv = f * 2 ** (det / 1200)
            hmax = int(min(0.45 * sr / fv, 36))
            y = additive(np.full(n, fv), sr, lambda h: lp2_mag(h * f, fc, 0.75) / h, hmax=hmax)
            a = (pan + 1) * np.pi / 4
            out[:, 0] += y * np.cos(a)
            out[:, 1] += y * np.sin(a)
    env = np.clip(t / 0.22, 0, 1) ** 1.6
    r = int(dur * sr)
    rl = int(0.55 * sr)
    env[r:r + rl] *= raised(rl)[::-1][:len(env[r:r + rl])]
    env[r + rl:] = 0
    return out * env[:, None] * 0.12


@lru_cache(maxsize=None)
def synth_bass(note, dur, sr):
    """Pulsing synthwave bass: saw plus a square an octave down through a
    low-pass that snaps open on each note."""
    f = float(midi_hz(note))
    n = int((dur + 0.04) * sr)
    t = np.arange(n) / sr
    fc = f * (1.6 + 9.0 * np.exp(-t / 0.055))
    hmax = int(min(0.45 * sr / f, 60))
    y = additive(np.full(n, f), sr, lambda h: lp2_mag(h * f, fc, 1.5) / h, hmax=hmax)
    y += 0.7 * additive(np.full(n, 0.5 * f), sr,
                        lambda h: (lp2_mag(h * 0.5 * f, fc, 0.9) / h) if h % 2 else 0, hmax=2 * hmax)
    env = np.minimum(t / 0.002, 1.0) * (0.7 + 0.3 * np.exp(-t / 0.08))
    r = int(dur * sr)
    rl = int(0.025 * sr)
    env[r:r + rl] *= raised(rl)[::-1][:len(env[r:r + rl])]
    env[r + rl:] = 0
    return softsat(y * env * 0.7, 1.3)


@lru_cache(maxsize=None)
def arp_pluck(note, sr):
    """Short square-wave pluck for sequenced arpeggios."""
    f = float(midi_hz(note))
    n = int(0.35 * sr)
    t = np.arange(n) / sr
    fc = f * (1.3 + 8.0 * np.exp(-t / 0.045))
    hmax = int(min(0.45 * sr / f, 40))
    y = additive(np.full(n, f), sr, lambda h: (lp2_mag(h * f, fc, 1.2) / h) if h % 2 else 0, hmax=hmax)
    return fade(y * np.exp(-t / 0.14) * np.minimum(t / 0.001, 1.0), 0.0, 0.03, sr)


@lru_cache(maxsize=None)
def gated_snare(sr, variant=0):
    """80s gated-reverb snare: the drum plus a dense, bright room tail that
    is cut off sharply after a quarter of a second."""
    n = int(0.34 * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"gsnare-{sr}-{variant}")
    dry = pad_to(snare(sr, variant), n)
    nz = filt(filt(rng.standard_normal(n), "hp", 900, sr=sr), "lp", 7500, sr=sr)
    gate = np.clip((0.27 - t) / 0.012, 0, 1)
    room = nz * np.exp(-t / 0.5) * gate * np.minimum(t / 0.006, 1.0)
    return fade(dry + 0.55 * room, 0.0, 0.01, sr)


@lru_cache(maxsize=None)
def clap(sr, variant=0):
    n = int(0.3 * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"clap-{sr}-{variant}")
    nz = filt(filt(rng.standard_normal(n), "bp", 1300, 0.7, sr=sr), "hp", 700, sr=sr)
    env = np.zeros(n)
    for d in (0.0, 0.009, 0.018):
        env += np.where(t >= d, np.exp(-np.clip(t - d, 0, None) / 0.004), 0)
    env += np.where(t >= 0.024, np.exp(-np.clip(t - 0.024, 0, None) / 0.07), 0) * 0.8
    return fade(nz * env, 0.0, 0.02, sr)


@lru_cache(maxsize=None)
def sax(note, dur, sr, vel=0.9):
    """Tenor-ish saxophone: a reedy harmonic spectrum shaped by two formants,
    brightness that blooms with breath, a scoop into each note, delayed
    vibrato, breath noise and a touch of growl."""
    f0 = float(midi_hz(note))
    n = int((dur + 0.15) * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"sax-{note}-{sr}")
    scoop = 1 - 0.028 * np.exp(-t / 0.045)
    vib = 1 + 0.0075 * np.sin(TAU * 5.4 * t) * np.clip((t - 0.28) / 0.3, 0, 1)
    f = f0 * scoop * vib
    swell = np.clip((t - 0.3) / 0.8, 0, 1)
    cut = 900.0 + 2600.0 * vel * (0.6 + 0.4 * np.exp(-t / 0.2)) + 900.0 * swell

    def amp(h):
        fh = h * f0
        form = 0.5 + lp2_mag(fh, 550.0, 1.6) / 1.6 + 0.8 * lp2_mag(fh, 1550.0, 3.0) / 3.0
        return form * h ** -0.55 * np.exp(-fh / cut)

    hmax = int(min(0.45 * sr / f0, 40))
    y = additive(f, sr, amp, hmax=hmax)
    growl = 1 + 0.08 * filt(rng.standard_normal(n), "lp", 35, sr=sr) * 8
    breath = filt(rng.standard_normal(n), "bp", 2600, 1.2, sr=sr) * 0.05
    y = y / (np.max(np.abs(y)) + 1e-9)
    env = (1 - np.exp(-t / 0.03)) * (0.85 + 0.15 * swell)
    y = (y * growl + breath) * env
    r = int(dur * sr)
    rl = int(0.08 * sr)
    y[r:r + rl] *= raised(rl)[::-1][:len(y[r:r + rl])]
    y[r + rl:] = 0
    return fade(y, 0.0, 0.02, sr) * 0.6


# ---------- percussion ----------

@lru_cache(maxsize=None)
def kick(sr, variant=0):
    n = int(0.45 * sr)
    t = np.arange(n) / sr
    f = 54 + 95 * np.exp(-t / 0.032) + 260 * np.exp(-t / 0.004)
    ph = TAU * np.cumsum(f) / sr
    y = np.sin(ph) * np.exp(-t / 0.24)
    rng = rng_for(f"kick-{sr}-{variant}")
    click = filt(rng.standard_normal(n), "lp", 4500, sr=sr) * np.exp(-t / 0.0025) * 0.35
    return fade(softsat(1.4 * (y + click), 1.3), 0.0, 0.05, sr)


@lru_cache(maxsize=None)
def snare(sr, variant=0):
    n = int(0.3 * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"snare-{sr}-{variant}")
    f1 = 188 * (1 + 0.1 * np.exp(-t / 0.008))
    tone = 1.0 * np.sin(TAU * np.cumsum(f1) / sr) * np.exp(-t / 0.06)
    tone += 0.45 * np.sin(TAU * 335 * t + 1.0) * np.exp(-t / 0.035)
    nz = rng.standard_normal(n)
    wires = filt(filt(nz, "hp", 1400, sr=sr), "peak", 3600, 1.0, sr=sr, gain_db=3)
    wires = filt(wires, "lp", min(8500, 0.45 * sr), sr=sr)
    y = tone + 0.6 * wires * np.exp(-t / 0.1) + 0.45 * wires * np.exp(-t / 0.004)
    return fade(y * np.minimum(t / 0.0005, 1), 0.0, 0.04, sr)


@lru_cache(maxsize=None)
def hat(sr, variant=0, open_=False):
    n = int((0.45 if open_ else 0.09) * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"hat-{sr}-{variant}")
    nz = filt(rng.standard_normal(n), "hp", 7000, sr=sr)
    nz = filt(nz, "peak", min(10000, 0.4 * sr), 1.2, sr=sr, gain_db=5)
    metal = np.zeros(n)
    for fr in (3240, 4410, 5530, 6870, 8120):
        if fr < 0.42 * sr:
            metal += np.sign(np.sin(TAU * fr * t + rng.uniform(0, 6)))
    metal = filt(metal, "hp", 6000, sr=sr) * 0.12
    dec = 0.17 if open_ else 0.022
    y = (nz + metal) * np.exp(-t / dec)
    y = filt(y, "lp", min(11000, 0.44 * sr), sr=sr)
    return fade(y, 0.0003, 0.02, sr)


@lru_cache(maxsize=None)
def shaker(sr, variant=0):
    n = int(0.09 * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"shaker-{sr}-{variant}")
    nz = filt(filt(rng.standard_normal(n), "bp", 7200, 1.3, sr=sr), "hp", 4000, sr=sr)
    env = np.clip(t / 0.007, 0, 1) * np.exp(-np.clip(t - 0.007, 0, None) / 0.03)
    return fade(nz * env, 0.0, 0.01, sr)


@lru_cache(maxsize=None)
def tambourine(sr, variant=0):
    n = int(0.22 * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"tamb-{sr}-{variant}")
    nz = filt(rng.standard_normal(n), "hp", 6000, sr=sr) * np.exp(-t / 0.05)
    jing = np.zeros(n)
    for fr in (6400, 7900, 9300):
        if fr < 0.42 * sr:
            jing += np.sin(TAU * fr * t + rng.uniform(0, 6)) * np.exp(-t / 0.07)
    return fade(nz + 0.25 * jing, 0.0005, 0.02, sr)


@lru_cache(maxsize=None)
def conga(kind, sr, variant=0):
    n = int(0.35 * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"conga-{kind}-{sr}-{variant}")
    f0 = 235.0 if kind == "lo" else 330.0
    bend = 1 + 0.06 * np.exp(-t / 0.02)
    ph = TAU * np.cumsum(f0 * bend) / sr
    mute = kind in ("mute", "slap")
    dk = 0.25 if mute else 1.0
    y = np.sin(ph) * np.exp(-t / (0.17 * dk))
    y += 0.25 * np.sin(1.59 * ph + 1) * np.exp(-t / (0.08 * dk))
    y += 0.12 * np.sin(2.14 * ph + 2) * np.exp(-t / (0.05 * dk))
    nz = rng.standard_normal(n)
    if kind == "slap":
        y = 0.6 * y + 1.1 * filt(nz, "bp", 2600, 0.7, sr=sr) * np.exp(-t / 0.014)
    elif kind == "mute":
        y += 0.5 * filt(nz, "lp", 1200, sr=sr) * np.exp(-t / 0.008)
    else:
        y += 0.25 * filt(nz, "bp", 1800, 0.8, sr=sr) * np.exp(-t / 0.004)
    return fade(y * np.minimum(t / 0.0005, 1), 0.0, 0.03, sr)


@lru_cache(maxsize=None)
def timbale(kind, sr, variant=0):
    n = int(0.45 * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"timb-{kind}-{sr}-{variant}")
    f0 = 520.0 if kind == "hi" else 385.0
    bend = 1 + 0.03 * np.exp(-t / 0.015)
    ph = TAU * np.cumsum(f0 * bend) / sr
    y = np.zeros(n)
    for r, a, d in ((1.0, 1.0, 0.22), (1.51, 0.5, 0.16), (2.27, 0.32, 0.11), (3.1, 0.2, 0.07),
                    (4.3, 0.1, 0.05)):
        if r * f0 < 0.44 * sr:
            y += a * np.sin(r * ph + r) * np.exp(-t / d)
    rim = filt(rng.standard_normal(n), "bp", 3200, 0.8, sr=sr) * np.exp(-t / 0.008)
    return fade((y + 0.9 * rim) * np.minimum(t / 0.0004, 1), 0.0, 0.04, sr)


@lru_cache(maxsize=None)
def clave(sr):
    n = int(0.12 * sr)
    t = np.arange(n) / sr
    y = np.sin(TAU * 2450 * t) * np.exp(-t / 0.022) + 0.3 * np.sin(TAU * 4020 * t) * np.exp(-t / 0.01)
    return fade(y * np.minimum(t / 0.0004, 1), 0.0, 0.02, sr)


@lru_cache(maxsize=None)
def crash(sr, variant=0, length=2.2):
    n = int(length * sr)
    t = np.arange(n) / sr
    rng = rng_for(f"crash-{sr}-{variant}")
    nz = filt(rng.standard_normal(n), "hp", 3500, sr=sr)
    nz = filt(nz, "peak", 6500, 0.8, sr=sr, gain_db=4)
    metal = np.zeros(n)
    for fr in rng.uniform(2800, min(11000, 0.42 * sr), 40):
        metal += np.sin(TAU * fr * t + rng.uniform(0, 6)) * np.exp(-t / rng.uniform(0.4, 0.9))
    metal = filt(metal, "hp", 2500, sr=sr) * 0.07
    y = nz * (0.55 * np.exp(-t / 0.6) + 0.45 * np.exp(-t / 0.08)) + metal
    y = filt(y, "lp", min(12000, 0.45 * sr), sr=sr)
    return fade(y * np.minimum(t / 0.001, 1), 0.0, 0.3, sr)


# ======================================================================
# Sound effects (mono, 22050 Hz)
# ======================================================================

SR = SFX_RATE


def engine_loop(name, period, cycles, high):
    """2-stroke kart engine: exhaust pulse per firing through pipe resonances.

    The excitation is exactly periodic over the loop (whole firing cycles,
    per-cycle variation repeats each loop), so the steady-state output loops
    seamlessly with no crossfade."""
    rng = rng_for(name)
    L = period * cycles
    sr = SR
    ff = sr / period
    tp = np.arange(int(0.012 * sr)) / sr
    tr, td = (0.00025, 0.0026) if not high else (0.0002, 0.0012)
    pulse = np.exp(-tp / td) - np.exp(-tp / tr)
    pulse /= pulse.max()
    shape = pulse.copy()
    rd = int((0.0036 if not high else 0.0017) * sr)
    shape[rd:] -= 0.5 * pulse[:len(pulse) - rd]
    amps = 1 + (0.12 if not high else 0.05) * rng.standard_normal(cycles)
    if not high:
        amps[1::2] *= rng.uniform(0.74, 0.95, cycles // 2)   # uneven "ring-ding"
    jit = rng.integers(-3 if not high else -1, 4 if not high else 2, cycles)
    exc = np.zeros(L)
    for k in range(cycles):
        place_wrap(exc, k * period + jit[k], shape, amps[k])

    nz = rng.standard_normal(L)
    cyc_t = (np.arange(L) % period) / sr
    comb = nz * np.exp(-cyc_t / (0.0045 if not high else 0.0022)) * np.repeat(amps, period)
    comb = periodic(lambda s: filt(filt(s, "bp", 1500, 0.7), "lp", 3200), comb)

    if not high:
        res = [(130, 2.0, 0.45), (390, 3.0, 0.9), (780, 3.0, 0.8), (1550, 2.5, 0.55),
               (2500, 3.0, 0.25)]
    else:
        res = [(240, 2.0, 0.45), (580, 3.0, 0.9), (1180, 3.0, 0.8), (2300, 2.5, 0.55),
               (3400, 3.0, 0.25)]
    body = 0.7 * exc
    for f, q, g in res:
        body += g * periodic(lambda s, f=f, q=q: filt(s, "bp", f, q), exc)
    body += comb * (0.22 if not high else 0.3)
    if high:
        # Intake/exhaust rasp: band noise breathing with the cycle.
        win = 0.5 - 0.5 * np.cos(TAU * ((np.arange(L) % period) / period + 0.35))
        rasp = periodic(lambda s: filt(filt(s, "bp", 950, 0.9), "lp", 4000),
                        rng.standard_normal(L)) * win
        body += rasp * 0.7
    # Thin the firing fundamental so the buzz carries on small TV speakers.
    body = periodic(lambda s: filt(s, "hp", 140 if not high else 200, 0.6), body)
    body /= np.max(np.abs(body))
    y = softsat(body, 2.4 if not high else 3.0)
    tt = np.arange(L) / sr
    whine_h = 11 if not high else 5
    y += 0.035 * np.sin(TAU * ff * whine_h * tt) * (1 + 0.3 * np.sin(TAU * ff * tt))
    lp = 3600 if not high else 5200
    y = periodic(lambda s: filt(filt(s, "lp", lp, 0.6), "lp", lp * 1.15, 0.6), y)
    y = periodic(lambda s: filt(s, "hp", 35), y)
    return y


def sfx_engine_low():
    return engine_loop("engine_low", 450, 24, high=False)    # 49 Hz firing, 0.490 s


def sfx_engine_high():
    return engine_loop("engine_high", 184, 64, high=True)    # 119.8 Hz firing, 0.534 s


def sfx_drift():
    rng = rng_for("drift")
    L = 13232                                                 # 0.60 s
    slow = plfo(L, 2.5, rng)
    fast = plfo(L, 22.0, rng)
    grit = plfo(L, 85.0, rng)
    swell = plfo(L, 4.0, rng)
    f1 = fix_cycles(1040 * (1 + 0.035 * slow + 0.009 * fast), SR)
    ph1 = TAU * np.cumsum(f1) / SR
    f2 = fix_cycles(1040 * 1.53 * (1 + 0.03 * slow - 0.012 * fast), SR)
    ph2 = TAU * np.cumsum(f2) / SR
    tone = np.sin(ph1) + 0.16 * np.sin(2 * ph1 + 0.5) + 0.05 * np.sin(3 * ph1 + 1.3)
    tone += 0.32 * np.sin(ph2)
    am = (1 - 0.38 * (0.5 + 0.5 * np.tanh(2.5 * grit))) * (0.85 + 0.15 * swell)
    scrub = periodic(lambda s: filt(filt(s, "bp", 2300, 0.8), "hp", 600),
                     rng.standard_normal(L))
    scrub *= 0.2 * (0.6 + 0.4 * np.abs(grit))
    rumble = periodic(lambda s: filt(s, "lp", 250), rng.standard_normal(L)) * 0.35
    y = tone * am + scrub + rumble
    return periodic(lambda s: filt(filt(s, "lp", 4800), "hp", 60), y)


def sfx_brake():
    # Higher and harsher than the drift squeal: a stick-slip screech whose
    # pitch judders as the tyre grabs and lets go.
    rng = rng_for("brake")
    L = 13232                                                 # 0.60 s
    slow = plfo(L, 3.0, rng)
    judder = plfo(L, 34.0, rng)
    grit = plfo(L, 120.0, rng)
    f1 = fix_cycles(2350 * (1 + 0.02 * slow + 0.014 * judder), SR)
    ph1 = TAU * np.cumsum(f1) / SR
    f2 = fix_cycles(2350 * 1.41 * (1 + 0.025 * slow - 0.02 * judder), SR)
    ph2 = TAU * np.cumsum(f2) / SR
    tone = np.sin(ph1) + 0.3 * np.sin(2 * ph1 + 0.7) + 0.12 * np.sin(3 * ph1 + 2.1)
    tone += 0.22 * np.sin(ph2)
    am = (1 - 0.5 * (0.5 + 0.5 * np.tanh(3.0 * judder))) * (1 - 0.2 * (0.5 + 0.5 * grit))
    scrub = periodic(lambda s: filt(filt(s, "bp", 3600, 0.9), "hp", 1200),
                     rng.standard_normal(L))
    scrub *= 0.28 * (0.5 + 0.5 * np.abs(judder))
    y = softsat(tone * am, 1.6) + scrub
    return periodic(lambda s: filt(filt(s, "lp", 7500), "hp", 300), y)


def sfx_sparks():
    rng = rng_for("sparks")
    L = 13232                                                 # 0.60 s
    flick = np.clip(0.55 + 0.6 * plfo(L, 40.0, rng), 0.05, None)
    fizz = periodic(lambda s: filt(filt(s, "hp", 3200), "peak", 6800, 1.0, gain_db=6),
                    rng.standard_normal(L)) * flick * 0.16
    crack = np.zeros(L)
    for _ in range(int(120 * L / SR)):
        m = int(rng.integers(6, 40))
        burst = rng.standard_normal(m) * np.exp(-np.arange(m) / (m / 4))
        place_wrap(crack, rng.integers(0, L), burst, min(rng.pareto(2.2) + 0.25, 2.5) * 0.5)
    crack = periodic(lambda s: filt(filt(s, "hp", 1600), "peak", 4200, 1.2, gain_db=4), crack)
    # Electric buzz: noise chopped at a whole-number mains-like rate, gated.
    bz_f = round(150 * L / SR) / (L / SR)
    tt = np.arange(L) / SR
    chop = (np.sin(TAU * bz_f * tt) > 0.6).astype(float)
    gate = np.clip(plfo(L, 6.0, rng) * 2.0 - 0.3, 0, 1)
    buzz = periodic(lambda s: filt(s, "bp", 3000, 1.0), rng.standard_normal(L) * chop) * gate
    y = fizz + crack + 0.35 * buzz
    y = periodic(lambda s: filt(s, "lp", 9000), y)
    return softsat(y / np.percentile(np.abs(y), 99.9), 1.3)    # tame crest factor


def sfx_offroad():
    rng = rng_for("offroad")
    L = 13232                                                 # 0.60 s
    grains = np.zeros(L)
    for _ in range(int(380 * L / SR)):
        m = int(rng.integers(int(0.003 * SR), int(0.014 * SR)))
        g = rng.standard_normal(m) * np.hanning(m)
        g = filt(g, "bp", rng.uniform(350, 1800), 1.2)
        place_wrap(grains, rng.integers(0, L), g, rng.lognormal(0, 0.5))
    tt = np.arange(L) / SR
    wheel = 0.75 + 0.25 * np.sin(TAU * 10 * tt) * (0.7 + 0.3 * plfo(L, 3, rng))   # 6 cycles
    rumble = periodic(lambda s: filt(filt(s, "lp", 170, 0.9), "hp", 40),
                      rng.standard_normal(L)) * wheel * 2.0
    hiss = periodic(lambda s: filt(filt(s, "bp", 3500, 0.7), "lp", 6000),
                    rng.standard_normal(L)) * (0.6 + 0.4 * plfo(L, 12, rng)) * 0.07
    y = grains * 0.5 * (0.7 + 0.3 * plfo(L, 9, rng)) + rumble + hiss
    y = periodic(lambda s: filt(s, "lp", 5000), y)
    return softsat(y / np.percentile(np.abs(y), 99.9), 1.3)    # even out grain peaks


def sfx_hop():
    n = int(0.18 * SR)
    t = np.arange(n) / SR
    f = (240 + 430 * (1 - np.exp(-t / 0.035))) * (1 + 0.07 * np.sin(TAU * 34 * t) * np.exp(-t / 0.045))
    ph = TAU * np.cumsum(f) / SR
    boing = (np.sin(ph) + 0.25 * np.sin(2 * ph) + 0.08 * np.sin(3 * ph)) * np.exp(-t / 0.055)
    boing *= np.minimum(t / 0.004, 1)
    thump_f = 95 + 60 * np.exp(-t / 0.01)
    thump = np.sin(TAU * np.cumsum(thump_f) / SR) * np.exp(-t / 0.03)
    rng = rng_for("hop")
    scuff = filt(rng.standard_normal(n), "bp", 1800, 0.9) * np.exp(-t / 0.012) * 0.3
    return 0.8 * boing + 0.9 * thump + scuff


def sfx_land():
    n = int(0.26 * SR)
    t = np.arange(n) / SR
    rng = rng_for("land")
    f = 46 + 40 * np.exp(-t / 0.025)
    thud = np.sin(TAU * np.cumsum(f) / SR) * np.exp(-t / 0.075)
    body = filt(rng.standard_normal(n), "lp", 380) * np.exp(-t / 0.035) * 1.4
    spring_f = 175 * (1 + 0.05 * np.exp(-t / 0.05))
    spring = np.sin(TAU * np.cumsum(spring_f) / SR) * np.exp(-t / 0.06) * 0.35
    spring *= np.clip((t - 0.01) / 0.01, 0, 1)
    rattle = np.zeros(n)
    for k in range(7):
        st = rng.uniform(0.02, 0.17)
        m = int(rng.integers(20, 70))
        c = filt(rng.standard_normal(m), "bp", rng.uniform(2200, 4800), 3.0) * np.hanning(m)
        place(rattle, st * SR, c, 0.5 * np.exp(-st / 0.08) * rng.uniform(0.5, 1.0))
    clack = body_hit("landclack", 0.26, [(310, 0.03, 1.0), (680, 0.022, 0.7), (1450, 0.012, 0.4)],
                     (160, 110, 0.03), 0, 0.0, thunk_gain=0.0)
    y = 0.8 * thud + 0.7 * body + spring + 0.8 * rattle + 0.8 * clack
    return softsat(y / np.max(np.abs(y)), 2.2)


def bell(f, n, partials, strike=0.0, seed="bell"):
    t = np.arange(n) / SR
    y = np.zeros(n)
    for r, a, d in partials:
        if r * f < 0.44 * SR:
            y += a * np.sin(TAU * f * r * t + r) * np.exp(-t / d)
    if strike:
        rng = rng_for(seed)
        y += filt(rng.standard_normal(n), "hp", 5000) * np.exp(-t / 0.004) * strike
    return fade(y * np.minimum(t / 0.0008, 1), 0.0, min(0.03, 0.2 * n / SR))


CHIME = ((1.0, 1.0, 0.22), (2.0, 0.45, 0.12), (3.0, 0.22, 0.07), (4.17, 0.14, 0.04),
         (5.43, 0.07, 0.025))


def sfx_charge1():
    n = int(0.4 * SR)
    y = bell(midi_hz(91), n, CHIME, strike=0.25, seed="c1")        # G6
    y += 0.4 * bell(midi_hz(98), n, CHIME[:2], seed="c1b")         # D7 shimmer
    return add_reverb(y, 0.22, seconds=0.5, t60=0.4, seed="c1r")[:int(0.5 * SR)]


def sfx_charge2():
    n = int(0.45 * SR)
    y = bell(midi_hz(96), n, CHIME, strike=0.3, seed="c2")         # C7
    d = int(0.055 * SR)
    y2 = bell(midi_hz(103), n - d, CHIME, strike=0.25, seed="c2b")  # G7
    y[d:] += 0.8 * y2
    return add_reverb(y, 0.25, seconds=0.55, t60=0.45, seed="c2r")[:int(0.58 * SR)]


def sfx_charge3():
    rng = rng_for("charge3")
    n = int(0.62 * SR)
    t = np.arange(n) / SR
    y = np.zeros(n)
    for i, note in enumerate((93, 97, 100, 105, 109)):             # A6 C#7 E7 A7 C#8
        st = int(i * 0.045 * SR)
        y[st:] += 0.7 * bell(midi_hz(note), n - st, CHIME[:3], strike=0.15, seed=f"c3{i}")
    fc = 2200 * 2 ** (np.clip(t / 0.35, 0, 1) * 2.0)
    sh = svf(rng.standard_normal(n), fc, 6.0, SR, "bp")
    sh *= (0.6 + 0.4 * np.sin(TAU * 28 * t)) * np.sin(np.pi * np.clip(t / 0.55, 0, 1)) * 0.25
    sparkle = np.zeros(n)
    for _ in range(30):
        st = rng.uniform(0.05, 0.45)
        sparkle_n = int(0.05 * SR)
        place(sparkle, st * SR, bell(rng.uniform(4000, 8500), sparkle_n, ((1, 1, 0.012),)),
              rng.uniform(0.05, 0.18))
    return add_reverb(y + sh + sparkle, 0.28, seconds=0.6, t60=0.5, seed="c3r")[:int(0.66 * SR)]


def sfx_boost():
    rng = rng_for("boost")
    n = int(0.9 * SR)
    t = np.arange(n) / SR
    pf = 45 + 70 * np.exp(-t / 0.03)
    punch = np.sin(TAU * np.cumsum(pf) / SR) * np.exp(-t / 0.11) * 1.1
    punch += filt(rng.standard_normal(n), "lp", 2200) * np.exp(-t / 0.03) * 0.6
    brown = lfilter([1.0], [1.0, -0.985], rng.standard_normal(n))
    brown = filt(brown, "hp", 40) / 12.0
    rfc = np.where(t < 0.12, 350 + 2700 * (t / 0.12), 3050 * np.exp(-(t - 0.12) / 0.5) + 500)
    turb = 1 + 0.35 * plfo(n, 45, rng)
    roar = svf(brown, rfc, 1.3, SR, "lp") * turb
    wfc = 600 * 2 ** (np.clip(t / 0.4, 0, 1) * 3.0)
    whoosh = svf(rng.standard_normal(n), wfc, 2.0, SR, "bp") * 0.5
    whoosh *= np.sin(np.pi * np.clip(t / 0.6, 0, 1)) ** 1.5
    jf = np.where(t < 0.35, 115 + 80 * (t / 0.35), 195 - 45 * np.clip((t - 0.35) / 0.55, 0, 1))
    jet = additive(jf, SR, lambda h: 1.0 / h, hmax=40)
    jet = filt(jet, "lp", 1500)
    d = (0.002 + 0.0015 * np.sin(TAU * 2.5 * t)) * SR
    idx = np.clip(np.arange(n) - d, 0, n - 1)
    jet = 0.5 * (jet + np.interp(idx, np.arange(n), jet))          # flanged jet
    env = np.minimum(t / 0.012, 1) * np.where(t < 0.35, 1.0, np.exp(-(t - 0.35) / 0.22))
    y = punch + (roar * 1.1 + whoosh + jet * 0.35) * env
    return softsat(y * 0.7, 1.4)


def sfx_dash():
    rng = rng_for("dash")
    n = int(0.8 * SR)
    t = np.arange(n) / SR
    zf = 450 * 2 ** (np.clip(t / 0.11, 0, 1) * 2.3)
    zip_ = np.sin(TAU * np.cumsum(zf) / SR) * np.clip(1 - t / 0.13, 0, 1) * np.minimum(t / 0.005, 1) * 0.5
    st = int(0.07 * SR)
    m = n - st
    tm = np.arange(m) / SR
    shing = np.zeros(m)
    for r, a, d in ((1.0, 1.0, 0.5), (1.47, 0.75, 0.4), (2.09, 0.6, 0.34), (2.56, 0.45, 0.28),
                    (3.17, 0.3, 0.2), (3.93, 0.2, 0.14)):
        fr = 1700 * r
        if fr < 0.44 * SR:
            for det in (0.0, 2.5):
                shing += a * 0.5 * np.sin(TAU * (fr + det) * tm + rng.uniform(0, 6)) * np.exp(-tm / d)
    shing *= np.minimum(tm / 0.002, 1)
    shing[:int(0.01 * SR)] += filt(rng.standard_normal(int(0.01 * SR)), "hp", 4000) * 0.6
    wfc = np.where(t < 0.25, 900 * 2 ** (t / 0.25 * 2.5), 5100 * np.exp(-(t - 0.25) / 0.4) + 900)
    whoosh = svf(rng.standard_normal(n), wfc, 1.5, SR, "bp")
    whoosh *= np.sin(np.pi * np.clip(t / 0.7, 0, 1)) ** 1.3 * 0.3
    y = zip_ + whoosh
    y[st:] += 0.7 * shing
    return add_reverb(y, 0.2, seconds=0.6, t60=0.5, seed="dashr")[:int(0.8 * SR)]


def body_hit(name, length, modes, thunk, rattle_n, rattle_amp, thunk_gain=1.0):
    rng = rng_for(name)
    n = int(length * SR)
    t = np.arange(n) / SR
    y = np.zeros(n)
    for f, tau, g in modes:          # ringing body modes (Hz, decay s, gain)
        fm = f * (1 + 0.02 * np.exp(-t / 0.01))
        y += g * np.sin(TAU * np.cumsum(fm) / SR + rng.uniform(0, 6)) * np.exp(-t / tau)
    y *= np.minimum(t / 0.0005, 1)
    m = int(0.004 * SR)
    y[:m] += filt(rng.standard_normal(m), "lp", 4000) * np.hanning(m) * 0.6
    f0, f1, dec = thunk
    tf = f1 + (f0 - f1) * np.exp(-t / 0.02)
    y += np.sin(TAU * np.cumsum(tf) / SR) * np.exp(-t / dec) * thunk_gain
    y += filt(rng.standard_normal(n), "lp", 500) * np.exp(-t / 0.02) * 0.6 * thunk_gain
    for k in range(rattle_n):
        st = rng.uniform(0.025, 0.2)
        mm = int(rng.integers(20, 60))
        c = filt(rng.standard_normal(mm), "bp", rng.uniform(1800, 4500), 2.5) * np.hanning(mm)
        place(y, st * SR, c, rattle_amp * np.exp(-st / 0.09) * rng.uniform(0.5, 1.0))
    return y


def sfx_bump():
    y = body_hit("bump", 0.36, [(235, 0.05, 1.0), (512, 0.038, 0.8), (1130, 0.022, 0.5),
                                (2310, 0.012, 0.3), (3400, 0.008, 0.15)],
                 (125, 72, 0.07), 9, 0.45, thunk_gain=0.7)
    return softsat(y / np.max(np.abs(y)), 3.0)


def sfx_kart_bump():
    rng = rng_for("kbonk")
    n = int(0.24 * SR)
    t = np.arange(n) / SR
    f = 300 + 140 * np.exp(-t / 0.025)
    ph = TAU * np.cumsum(f) / SR
    bonk = (np.sin(ph) + 0.35 * np.sin(2.3 * ph + 1)) * np.exp(-t / 0.06)
    hit = body_hit("kbump", 0.24, [(640, 0.03, 0.6), (1380, 0.018, 0.3)], (180, 120, 0.04), 3,
                   0.3)
    return bonk + 0.6 * hit + filt(rng.standard_normal(n), "hp", 2500) * np.exp(-t / 0.003) * 0.3


def beep(f, dur, total, harm=1.0):
    n = int(total * SR)
    t = np.arange(n) / SR
    sq = additive(np.full(n, f), SR, lambda h: (1.0 / h) * (0.5 ** ((h - 1) / 6)) if h % 2 else 0.0,
                  hmax=25) * harm
    y = sq + 0.6 * np.sin(TAU * f * t)
    y = filt(y, "lp", 3200)
    env = np.minimum(t / 0.004, 1)
    r = int(dur * SR)
    rl = int(0.05 * SR)
    env[r:r + rl] *= raised(rl)[::-1]
    env[r + rl:] = 0
    return y * env


def sfx_count_beep():
    y = beep(440.0, 0.26, 0.32)
    return add_reverb(y, 0.12, seconds=0.35, t60=0.3, seed="beepr")[:int(0.37 * SR)]


def sfx_go():
    n = int(0.8 * SR)
    t = np.arange(n) / SR
    y = beep(880.0, 0.68, 0.8)
    vib = np.clip((t - 0.2) / 0.2, 0, 1)
    for note, a in ((85, 0.35), (88, 0.3), (81 - 12, 0.25)):
        f = midi_hz(note) * (1 + 0.006 * np.sin(TAU * 6 * t) * vib)
        tone = additive(f, SR, lambda h: 1.0 / h if h % 2 else 0.3 / h, hmax=12)
        gate = np.minimum(t / 0.006, 1) * np.clip(1 - (t - 0.68) / 0.05, 0, 1)
        y += a * filt(tone, "lp", 3500) * gate
    return add_reverb(y, 0.18, seconds=0.6, t60=0.5, seed="gor")[:int(0.82 * SR)]


def sfx_lap():
    n = int(0.75 * SR)
    y = np.zeros(n)
    for i, note in enumerate((81, 85, 88, 93)):                    # A5 C#6 E6 A6
        st = int(i * 0.065 * SR)
        tone = bell(midi_hz(note), n - st, ((1, 1, 0.25), (2.0, 0.3, 0.12), (3.0, 0.12, 0.06)))
        tone += 0.5 * fit(rhodes(note - 12, 0.15, SR, 0.9), n - st)
        y[st:] += tone * (0.8 + 0.07 * i)
    return add_reverb(y, 0.25, seconds=0.6, t60=0.5, seed="lapr")[:int(0.66 * SR)]


def mono(st):
    return st.mean(axis=1) if st.ndim == 2 else st


def third_below(note, pcs=(9, 11, 1, 2, 4, 6, 8)):
    """Diatonic third below in A major (or another pitch-class set)."""
    n = note - 1
    steps = 0
    while steps < 2:
        if n % 12 in pcs:
            steps += 1
            if steps == 2:
                break
        n -= 1
    return n


def sfx_final_lap():
    total = 1.75
    n = int(total * SR)
    y = np.zeros(n)
    s = 0.075
    mel = [(0, 1, 76), (1, 1, 81), (2, 1, 85), (3, 2, 88), (6, 1, 86), (7, 9, 88)]
    for k, (st, ln, note) in enumerate(mel):
        dur = ln * s * 0.9 if k < len(mel) - 1 else 0.75
        t0 = int(st * s * SR)
        b = mono(brass_note(note, dur, SR, 1.2)) * 0.9
        b += mono(brass_note(third_below(note), dur, SR, 1.0)) * 0.55
        place(y, t0, b)
        place(y, t0, steelpan(note, dur, SR), 0.55)
        place(y, t0, timbale("hi" if k % 2 == 0 else "lo", SR, k % 3), 0.35)
    t_end = int(7 * s * SR)
    for note in (69, 73, 76):
        place(y, t_end, mono(brass_note(note, 0.75, SR, 1.0)), 0.5)
    place(y, t_end, crash(SR, 1, 1.2), 0.35)
    place(y, t_end, kick(SR), 0.7)
    for k in range(7):
        place(y, int(k * s * 0.5 * SR), snare(SR, k % 3), 0.12 + 0.03 * k)
    for i, note in enumerate((93, 97, 100, 105)):
        place(y, t_end + int((0.05 + 0.06 * i) * SR), glock(note, SR, 0.8), 0.18)
    tt = np.arange(n) / SR
    y *= np.where(tt > 1.2, np.clip(1 - (tt - 1.2) / 0.42, 0, 1), 1.0)
    return add_reverb(y, 0.22, seconds=0.6, t60=0.6, seed="flr")[:int(1.62 * SR)]


def sfx_finish():
    total = 2.6
    n = int(total * SR)
    y = np.zeros(n)
    trip = 0.1
    for k in range(3):                                              # E5 E5 E5 pickup
        place(y, int(k * trip * SR), mono(brass_note(76, 0.07, SR, 1.1)), 0.9)
        place(y, int(k * trip * SR), mono(brass_note(73, 0.07, SR, 0.9)), 0.5)
        place(y, int(k * trip * SR), snare(SR, k), 0.3)
    hits = [(0.30, (69, 73, 76, 81), 0.24), (0.60, (65, 69, 72, 84), 0.24),
            (0.90, (67, 71, 74, 86), 0.24), (1.20, (69, 73, 76, 81, 88), 1.05)]
    for i, (st, chord, dur) in enumerate(hits):
        s0 = int(st * SR)
        for j, note in enumerate(chord):
            top = j == len(chord) - 1
            place(y, s0, mono(brass_note(note, dur, SR, 1.2 if top else 0.9)), 0.8 if top else 0.42)
        place(y, s0, steelpan(chord[-1], dur, SR), 0.5)
        place(y, s0, timbale("hi" if i % 2 == 0 else "lo", SR, i), 0.4)
        place(y, s0, kick(SR), 0.6)
    s_end = int(1.2 * SR)
    place(y, s_end, crash(SR, 2, 1.4), 0.4)
    for k in range(10):                                             # timbale roll
        place(y, s_end + int((0.3 + k * 0.05) * SR), timbale("lo" if k % 2 else "hi", SR, k % 3),
              0.12 + 0.02 * k)
    for i, note in enumerate((93, 88, 85, 81, 85, 88, 93, 97)):
        place(y, s_end + int((0.08 + 0.07 * i) * SR), glock(note, SR, 0.9), 0.16)
        place(y, s_end + int((0.08 + 0.07 * i) * SR), steelpan(note - 12, 0.1, SR), 0.18)
    tt = np.arange(n) / SR
    y *= np.where(tt > 1.95, np.clip(1 - (tt - 1.95) / 0.45, 0, 1), 1.0)
    return add_reverb(y, 0.25, seconds=0.8, t60=0.7, seed="finr")[:int(2.45 * SR)]


def sfx_menu_move():
    n = int(0.045 * SR)
    t = np.arange(n) / SR
    f = 1450 + 350 * np.exp(-t / 0.006)
    ph = TAU * np.cumsum(f) / SR
    y = (np.sin(ph) + 0.2 * np.sin(2 * ph)) * np.exp(-t / 0.012)
    return y * np.minimum(t / 0.0008, 1)


def sfx_menu_select():
    n = int(0.36 * SR)
    y = np.zeros(n)
    for i, note in enumerate((88, 93)):                             # E6 then A6
        st = int(i * 0.07 * SR)
        y[st:] += bell(midi_hz(note), n - st, ((1, 1, 0.12), (2.0, 0.35, 0.06), (3.0, 0.15, 0.03)),
                       strike=0.1, seed=f"ms{i}") * (0.85 + 0.15 * i)
    return add_reverb(y, 0.15, seconds=0.35, t60=0.3, seed="msr")[:int(0.42 * SR)]


def sfx_menu_back():
    n = int(0.26 * SR)
    y = np.zeros(n)
    for i, note in enumerate((81, 76)):                             # A5 then E5
        st = int(i * 0.075 * SR)
        y[st:] += fit(marimba(note, SR), n - st) * (1.0 - 0.15 * i)
        y[st:] += bell(midi_hz(note), n - st, ((1, 0.5, 0.08), (2, 0.15, 0.04)))
    return add_reverb(y, 0.12, seconds=0.3, t60=0.25, seed="mbr")[:int(0.3 * SR)]


# ---------- Chao voice ----------

FORMANT_WEIGHTS = (1.0, 0.8, 0.3)


def formant_voice(n, f0, formants, amp, breath=0.015, seed="voice", tilt=0.8):
    """Additive formant synthesis: each harmonic of the glottal source is
    weighted by a parallel bank of formant resonances (what a pulse train
    through formant filters gives, but band-limited and smooth under fast
    formant and pitch glides).

    f0: per-sample pitch; formants: list of (F_track, bandwidth) pairs."""
    ph = TAU * np.cumsum(f0) / SR

    def amp_h(h):
        fh = h * f0
        g = 0.015
        for (F, bw), w in zip(formants, FORMANT_WEIGHTS):
            q = F / bw
            g = g + w * lp2_mag(fh, F, q) / q
        return g * h ** -tilt

    y = additive(f0, SR, amp_h, hmax=30, phase=ph)
    rng = rng_for(seed)
    nz = rng.standard_normal(n)
    asp = np.zeros(n)
    for F, bw in formants[:2]:
        asp += svf(nz, F, F / bw, SR, "bp")
    asp /= np.max(np.abs(asp)) + 1e-9
    y = y / (np.max(np.abs(y)) + 1e-9) + breath * asp
    return filt(y * amp, "lp", 6500)


def chao_syllable(n, start, f_a, f_o, pitch_a, pitch_o, rng_seed, end_up=False):
    """One 'cha-o' call; returns array of length n starting at sample start."""
    t = np.arange(n) / SR - start
    T = 0.44
    # Pitch: 'cha' rises a little, then 'o' starts higher and glides.
    pa = pitch_a * (1 + 0.08 * np.clip((t - 0.03) / 0.12, 0, 1))
    if end_up:
        po = pitch_o * (0.92 + 0.22 * np.clip((t - 0.22) / 0.2, 0, 1) ** 1.5)
    else:
        po = pitch_o * (1.18 - 0.3 * np.clip((t - 0.2) / 0.22, 0, 1) ** 0.8)
    xo = np.clip((t - 0.17) / 0.07, 0, 1)
    xo = xo * xo * (3 - 2 * xo)
    f0 = pa * (1 - xo) + po * xo
    f0 *= 1 + 0.015 * np.sin(TAU * 7.0 * t) * np.clip((t - 0.22) / 0.08, 0, 1)
    f0 = np.where(t < 0, pitch_a, f0)
    F = [(f_a[i] * (1 - xo) + f_o[i] * xo, bw) for i, bw in enumerate((130.0, 170.0, 280.0))]
    on = np.clip((t - 0.03) / 0.025, 0, 1)
    dip = 1 - 0.38 * np.exp(-((t - 0.19) / 0.03) ** 2)
    off = np.clip((T - t) / 0.16, 0, 1) ** 1.3
    amp = on * dip * off * np.where(t < 0, 0, 1)
    v = formant_voice(n, np.clip(f0, 200, 2000), F, amp, seed=rng_seed)
    # 'ch' onset: tiny burst then a soft sh-like fricative.
    rng = rng_for(rng_seed + "ch")
    nz = rng.standard_normal(n)
    fric = filt(filt(nz, "bp", 4200, 1.2), "lp", 7000)
    fenv = np.clip(t / 0.008, 0, 1) * np.clip((0.05 - t) / 0.025, 0, 1)
    burst = np.exp(-np.clip(t, 0, None) / 0.003) * (t >= 0)
    return v + fric * fenv * 0.2 + filt(nz, "hp", 2000) * burst * 0.12


A_VOWEL = (900.0, 1380.0, 3000.0)
O_VOWEL = (520.0, 900.0, 2750.0)


def sfx_bell():
    """The lobby bell: a two-note ding-dong when someone joins."""
    n = int(1.3 * SR)
    y = np.zeros(n)
    for i, note in enumerate((88, 84)):                             # E6 then C6
        st = int(i * 0.32 * SR)
        y[st:] += bell(midi_hz(note), n - st, ((1, 1, 0.5), (2.0, 0.4, 0.22), (2.76, 0.22, 0.12), (4.1, 0.08, 0.05)),
                       strike=0.12, seed=f"bl{i}") * (1.0 - 0.12 * i)
        y[st:] += 0.35 * fit(glock(note, SR, 0.6), n - st)
    return add_reverb(y, 0.25, seconds=1.0, t60=0.9, seed="blr")[:int(1.4 * SR)]


def sfx_chao():
    n = int(0.5 * SR)
    y = chao_syllable(n, 0.0, A_VOWEL, O_VOWEL, 720.0, 760.0, "chao1")
    return add_reverb(y, 0.08, seconds=0.3, t60=0.25, seed="chaor")[:int(0.56 * SR)]


def sfx_chao_happy():
    n = int(1.3 * SR)
    y = chao_syllable(n, 0.0, A_VOWEL, O_VOWEL, 700.0, 740.0, "chaoh1")
    y += 1.05 * chao_syllable(n, 0.36, A_VOWEL, O_VOWEL, 820.0, 900.0, "chaoh2", end_up=True)
    # Giggle: three quick 'hi' syllables, last one longer and falling.
    i_vowel = (480.0, 2600.0, 3600.0)
    for k, (st, ln, p) in enumerate(((0.86, 0.07, 1050.0), (0.97, 0.07, 1120.0),
                                     (1.08, 0.16, 1180.0))):
        t = np.arange(n) / SR - st
        f0 = p * (1 - 0.12 * np.clip(t / ln, 0, 1)) * (1 + 0.02 * np.sin(TAU * 9 * t))
        amp = np.clip(t / 0.012, 0, 1) * np.clip((ln - t) / (ln * 0.6), 0, 1) * (t >= 0)
        F = [(np.full(n, i_vowel[j]), bw) for j, bw in enumerate((140.0, 220.0, 300.0))]
        v = formant_voice(n, np.clip(f0, 200, 2000), F, amp, breath=0.08, seed=f"giggle{k}")
        h = filt(rng_for(f"gh{k}").standard_normal(n), "bp", 2500, 0.8)
        hen = np.clip(t / 0.005, 0, 1) * np.clip((0.025 - t) / 0.015, 0, 1)
        y += 0.8 * v + 0.15 * h * hen
    return add_reverb(y, 0.08, seconds=0.3, t60=0.25, seed="chaohr")[:int(1.24 * SR)]


U_VOWEL = (360.0, 820.0, 2500.0)
I_VOWEL = (420.0, 2500.0, 3400.0)


def voice_syllable(n, start, dur, vowels, pitches, seed, onset=None, breath=0.02, growl=0.0):
    """One syllable whose formants and pitch glide through evenly spaced key
    values over dur seconds. onset: 'ch' (fricative), 'h' (breathy), 'b'
    (lip thump) or None (a vowel onset like 'w'/'y' comes from the vowels).
    growl adds a rough 32 Hz flutter for a pouty, grumbly voice."""
    lead = 0.035 if onset in ("ch", "h") else 0.0   # the fricative comes first
    tf = np.arange(n) / SR - start
    t = tf - lead
    x = np.clip(t / dur, 0, 1)
    keys = np.linspace(0, 1, len(pitches))
    f0 = np.interp(x, keys, pitches) * (1 + 0.012 * np.sin(TAU * 7.5 * t) * np.clip((t - 0.08) / 0.06, 0, 1))
    vk = np.linspace(0, 1, len(vowels))
    F = [(np.interp(x, vk, [v[i] for v in vowels]), bw) for i, bw in enumerate((130.0, 180.0, 280.0))]
    on = np.clip(t / 0.018, 0, 1)
    off = np.clip((dur - t) / (dur * 0.38), 0, 1) ** 1.3
    amp = on * off * (t >= 0)
    if growl:
        amp = amp * (1 - growl + growl * (0.5 + 0.5 * np.sin(TAU * 32.0 * t)))
    v = formant_voice(n, np.clip(f0, 200, 2000), F, amp, breath=breath, seed=seed)
    nz = rng_for(seed + "on").standard_normal(n)
    fenv = np.clip(tf / 0.008, 0, 1) * np.clip((lead + 0.012 - tf) / 0.02, 0, 1)
    if onset == "ch":
        v = v + filt(filt(nz, "bp", 4200, 1.2), "lp", 7000) * fenv * 0.22
    elif onset == "h":
        v = v + filt(nz, "bp", 1800, 0.7) * fenv * 0.3
    elif onset == "b":
        thump = filt(nz, "lp", 400) * np.exp(-np.clip(t, 0, None) / 0.006) * (t >= 0)
        v = v * np.clip(t / 0.03, 0, 1) + 0.5 * thump
    return v


def chao_reverb(y, seed, secs):
    return add_reverb(y, 0.08, seconds=0.3, t60=0.25, seed=seed)[:int(secs * SR)]


def sfx_chao_hey():
    """'Chao?': the call with a questioning lift (waves)."""
    n = int(0.5 * SR)
    return chao_reverb(chao_syllable(n, 0.0, A_VOWEL, O_VOWEL, 760.0, 820.0, "chaoq", end_up=True), "chaoqr", 0.56)


def sfx_chao_chu():
    """'Chu-chu!': two quick bright chirps (waves)."""
    n = int(0.45 * SR)
    y = voice_syllable(n, 0.0, 0.11, [U_VOWEL, (420.0, 1000.0, 2600.0)], [960.0, 1080.0], "chu1", onset="ch")
    y += voice_syllable(n, 0.16, 0.15, [U_VOWEL, (430.0, 1050.0, 2650.0)], [1040.0, 1180.0, 1100.0], "chu2",
                        onset="ch")
    return chao_reverb(y, "chur", 0.45)


def sfx_chao_yay():
    """'Ya-ya!': rising, cheerful (waves)."""
    n = int(0.55 * SR)
    y = voice_syllable(n, 0.0, 0.13, [I_VOWEL, A_VOWEL], [840.0, 940.0], "ya1")
    y += 1.1 * voice_syllable(n, 0.17, 0.26, [I_VOWEL, A_VOWEL, A_VOWEL], [960.0, 1150.0, 1020.0], "ya2")
    return chao_reverb(y, "yar", 0.55)


def sfx_chao_wah():
    """'Wah!': knocked sideways, high and falling (kart bumps)."""
    n = int(0.45 * SR)
    y = voice_syllable(n, 0.0, 0.32, [U_VOWEL, A_VOWEL, A_VOWEL, O_VOWEL], [1100.0, 1260.0, 980.0, 680.0],
                       "wah", breath=0.04)
    return chao_reverb(y, "wahr", 0.45)


def sfx_chao_buu():
    """'Buu!': a pouty, grumbly complaint (kart bumps)."""
    n = int(0.5 * SR)
    y = voice_syllable(n, 0.0, 0.38, [U_VOWEL, (380.0, 780.0, 2450.0), U_VOWEL], [600.0, 680.0, 540.0],
                       "buu", onset="b", growl=0.35)
    return chao_reverb(y, "buur", 0.5)


def sfx_chao_hya():
    """'Hya!': a startled yelp (kart bumps)."""
    n = int(0.36 * SR)
    y = voice_syllable(n, 0.0, 0.2, [I_VOWEL, A_VOWEL, A_VOWEL], [1300.0, 1500.0, 1050.0], "hya", onset="h",
                       breath=0.05)
    return chao_reverb(y, "hyar", 0.36)


def sfx_whoosh():
    rng = rng_for("whoosh")
    n = int(0.6 * SR)
    t = np.arange(n) / SR
    pink = lfilter([0.049922, -0.095993, 0.050613, -0.004408],
                   [1, -2.494956, 2.017265, -0.522189], rng.standard_normal(n))
    fc = np.where(t < 0.32, 350 * 2 ** (t / 0.32 * 2.65), 2200 * 2 ** (-(t - 0.32) / 0.28 * 1.3))
    body = svf(pink * 8, fc, 1.6, SR, "bp")
    air = filt(rng.standard_normal(n), "hp", 3200) * 0.25
    x = np.clip(t / 0.6, 0, 1)
    env = np.sin(np.pi * x ** 0.85) ** 2
    return (body + air * np.roll(env, int(0.03 * SR))) * env


def sfx_star():
    rng = rng_for("star")
    n = int(0.5 * SR)
    t = np.arange(n) / SR
    y = np.zeros(n)
    for st, note in ((0.0, 100), (0.035, 105), (0.08, 97), (0.13, 109), (0.2, 104), (0.27, 112)):
        s = int(st * SR)
        y[s:] += bell(midi_hz(note), n - s, ((1, 1, 0.07), (2.32, 0.3, 0.03), (4.25, 0.1, 0.015)))
    sh = filt(rng.standard_normal(n), "hp", 7000) * (0.5 + 0.5 * np.sin(TAU * 31 * t)) * \
        np.exp(-t / 0.12) * 0.15
    return add_reverb(y * 0.6 + sh, 0.35, seconds=0.5, t60=0.45, seed="starr")[:int(0.52 * SR)]


def sfx_emerald():
    """Picking up a Chaos Emerald: a quick rising chime, Sonic style."""
    rng = rng_for("emerald")
    n = int(0.75 * SR)
    t = np.arange(n) / SR
    y = np.zeros(n)
    for st, note, a in ((0.0, 88, 0.8), (0.06, 92, 0.85), (0.12, 95, 0.9), (0.18, 100, 1.0), (0.30, 104, 0.6)):
        s = int(st * SR)
        y[s:] += a * bell(midi_hz(note), n - s, CHIME, strike=0.12, seed=f"em{note}")
    shimmer = filt(rng.standard_normal(n), "hp", 6500) * np.exp(-t / 0.18) * (0.5 + 0.5 * np.sin(TAU * 41 * t)) * 0.12
    return add_reverb(y * 0.7 + shimmer, 0.3, seconds=0.6, t60=0.5, seed="emr")[:int(0.8 * SR)]


def sfx_spear():
    """A Chaos Spear leaving: a rising zap and a burst of air."""
    rng = rng_for("spear")
    n = int(0.55 * SR)
    t = np.arange(n) / SR
    zf = 260 * 2 ** (np.clip(t / 0.14, 0, 1) * 3.4)
    zap = np.sin(TAU * np.cumsum(zf) / SR) * np.exp(-t / 0.09) * np.minimum(t / 0.003, 1)
    zap += 0.4 * np.sin(TAU * np.cumsum(zf * 1.5) / SR) * np.exp(-t / 0.06)
    wf = 500 * 2 ** (np.clip(t / 0.3, 0, 1) * 2.2)
    whoosh = svf(rng.standard_normal(n), wf, 1.8, SR, "bp") * np.sin(np.pi * np.clip(t / 0.5, 0, 1)) ** 1.3 * 0.9
    crackle = filt(rng.standard_normal(n), "hp", 4000) * np.exp(-t / 0.05) * 0.35
    return softsat(zap * 0.9 + whoosh + crackle, 1.3)


def sfx_spear_hit():
    """A spear landing: a thump, a crack and the kart whipping round."""
    rng = rng_for("spearhit")
    n = int(0.7 * SR)
    t = np.arange(n) / SR
    tf = 40 + 110 * np.exp(-t / 0.035)
    thump = np.sin(TAU * np.cumsum(tf) / SR) * np.exp(-t / 0.14) * 1.1
    crack = filt(rng.standard_normal(n), "bp", 2200, 0.8) * np.exp(-t / 0.03) * 0.9
    wf = 900 * 2 ** (-np.clip(t / 0.5, 0, 1) * 2.0)
    trem = 0.5 + 0.5 * np.sin(TAU * 18 * t)
    whirl = svf(rng.standard_normal(n), wf, 3.0, SR, "bp") * trem * np.exp(-t / 0.22) * 0.8
    tone = np.sin(TAU * np.cumsum(wf * 0.5) / SR) * np.exp(-t / 0.2) * 0.25
    return softsat(thump + crack + whirl + tone, 1.4)


def sfx_barrier():
    """The Barrier going up: a swell into a soft, bright chord."""
    rng = rng_for("barrier")
    n = int(0.8 * SR)
    t = np.arange(n) / SR
    swell = svf(rng.standard_normal(n), 300 + 2500 * np.clip(t / 0.25, 0, 1), 2.5, SR, "bp")
    swell *= np.sin(np.pi * np.clip(t / 0.3, 0, 1)) ** 2 * 0.6
    chord = np.zeros(n)
    for note, a in ((64, 0.5), (71, 0.4), (76, 0.45), (83, 0.3)):
        f = midi_hz(note)
        chord += a * (np.sin(TAU * f * t) + 0.4 * np.sin(TAU * f * 2.01 * t)) * np.minimum(t / 0.08, 1)
    chord *= np.where(t < 0.3, 1.0, np.exp(-(t - 0.3) / 0.25))
    tick = bell(midi_hz(100), n, CHIME[:3], seed="bar")
    tick = np.roll(tick, int(0.22 * SR)) * 0.35
    tick[:int(0.22 * SR)] = 0
    return add_reverb(swell + chord * 0.35 + tick, 0.3, seconds=0.6, t60=0.5, seed="barr")[:int(0.85 * SR)]


def sfx_barrier_pop():
    """A spear bursting on the Barrier: a glassy pop."""
    rng = rng_for("barrierpop")
    n = int(0.45 * SR)
    t = np.arange(n) / SR
    pop = filt(rng.standard_normal(n), "hp", 2500) * np.exp(-t / 0.018) * 1.2
    pf = 1400 * np.exp(-t / 0.05) + 200
    body = np.sin(TAU * np.cumsum(pf) / SR) * np.exp(-t / 0.06) * 0.8
    shards = np.zeros(n)
    for st, note in ((0.02, 103), (0.05, 99), (0.09, 106), (0.13, 96)):
        s = int(st * SR)
        shards[s:] += 0.4 * bell(midi_hz(note), n - s, CHIME[:3], seed=f"sh{note}")
    return add_reverb(pop + body + shards, 0.25, seconds=0.4, t60=0.35, seed="popr")[:int(0.5 * SR)]


def sfx_mine():
    """Dropping a Mine: a heavy clank on the road and a two-tone arming beep."""
    n = int(0.55 * SR)
    y = np.zeros(n)
    clank = body_hit("mineclank", 0.32, [(290, 0.07, 1.0), (640, 0.045, 0.7), (1420, 0.025, 0.45),
                                        (2700, 0.012, 0.25)], (95, 58, 0.06), 6, 0.4, thunk_gain=1.3)
    clank /= max(np.max(np.abs(clank)), 1e-6)
    y[:len(clank)] += clank * 0.9
    for st, f in ((0.17, 1480.0), (0.31, 2220.0)):
        b = beep(f, 0.06, 0.13, harm=0.5) * 0.28
        place(y, st * SR, b)
    return softsat(y, 1.2)


def sfx_mine_hit():
    """A Mine going off: a deep boom, a crack of air and debris pattering down."""
    rng = rng_for("minehit")
    n = int(0.95 * SR)
    t = np.arange(n) / SR
    bf = 28 + 160 * np.exp(-t / 0.04)
    boom = np.sin(TAU * np.cumsum(bf) / SR) * np.exp(-t / 0.24) * 1.3
    crack = filt(rng.standard_normal(n), "bp", 1900, 0.6) * np.exp(-t / 0.045) * 1.1
    rumble = svf(rng.standard_normal(n), 230 * np.exp(-t / 0.3) + 60, 1.2, SR, "lp") * np.exp(-t / 0.32) * 1.3
    debris = np.zeros(n)
    for _ in range(16):
        st = 0.12 + rng.uniform(0, 0.55)
        m = int(0.04 * SR)
        tb = np.arange(m) / SR
        f = rng.uniform(900, 3400)
        place(debris, st * SR, np.sin(TAU * f * tb) * np.exp(-tb / 0.012) * rng.uniform(0.08, 0.2))
    return softsat(boom + crack + rumble + debris, 1.5)


def sfx_plane():
    """Spiral Canyon's biplane: a radial engine's lumpy drone and the prop's
    chop, as a seamless loop. The game retunes it each frame for distance
    and doppler as the plane weaves ahead of the kart. 73.5 Hz exactly
    (300 samples), the lumps a quarter of that, so 12000 samples hold
    whole cycles of everything."""
    rng = rng_for("plane")
    L = 12000
    t = np.arange(L)
    ph = TAU * t / 300
    drone = np.zeros(L)
    for k in range(1, 28):
        drone += np.sin(k * ph + rng.uniform(0, TAU)) / k ** 0.85
    lump = 1.0 + 0.35 * np.sin(ph * 0.5 + 0.7) + 0.2 * np.sin(ph * 0.25)
    chop = 0.75 + 0.25 * np.sin(2 * ph)
    body = periodic(lambda x: filt(x, "lp", 2600, 0.8), drone * lump * chop)
    air = periodic(lambda x: filt(x, "bp", 1500, 0.9), rng.standard_normal(L)) * 0.25
    return softsat(body * 0.32 + air, 1.3)


SFX = [
    ("ENGINE_LOW", sfx_engine_low, True, 0.75),
    ("ENGINE_HIGH", sfx_engine_high, True, 0.65),
    ("DRIFT", sfx_drift, True, 0.55),
    ("SPARKS", sfx_sparks, True, 0.45),
    ("OFFROAD", sfx_offroad, True, 0.6),
    ("BRAKE", sfx_brake, True, 0.5),
    ("HOP", sfx_hop, False, 0.6),
    ("LAND", sfx_land, False, 0.75),
    ("CHARGE1", sfx_charge1, False, 0.55),
    ("CHARGE2", sfx_charge2, False, 0.55),
    ("CHARGE3", sfx_charge3, False, 0.6),
    ("BOOST", sfx_boost, False, 0.85),
    ("DASH", sfx_dash, False, 0.8),
    ("BUMP", sfx_bump, False, 0.8),
    ("KART_BUMP", sfx_kart_bump, False, 0.7),
    ("COUNT_BEEP", sfx_count_beep, False, 0.7),
    ("GO", sfx_go, False, 0.75),
    ("LAP", sfx_lap, False, 0.7),
    ("FINAL_LAP", sfx_final_lap, False, 0.8),
    ("FINISH", sfx_finish, False, 0.85),
    ("MENU_MOVE", sfx_menu_move, False, 0.45),
    ("MENU_SELECT", sfx_menu_select, False, 0.6),
    ("MENU_BACK", sfx_menu_back, False, 0.55),
    ("CHAO", sfx_chao, False, 0.7),
    ("CHAO_HAPPY", sfx_chao_happy, False, 0.7),
    ("CHAO_HEY", sfx_chao_hey, False, 0.7),
    ("CHAO_CHU", sfx_chao_chu, False, 0.65),
    ("CHAO_YAY", sfx_chao_yay, False, 0.7),
    ("CHAO_WAH", sfx_chao_wah, False, 0.7),
    ("CHAO_BUU", sfx_chao_buu, False, 0.7),
    ("CHAO_HYA", sfx_chao_hya, False, 0.65),
    ("WHOOSH", sfx_whoosh, False, 0.6),
    ("STAR", sfx_star, False, 0.5),
    ("EMERALD", sfx_emerald, False, 0.6),
    ("SPEAR", sfx_spear, False, 0.75),
    ("SPEAR_HIT", sfx_spear_hit, False, 0.8),
    ("BARRIER", sfx_barrier, False, 0.6),
    ("BARRIER_POP", sfx_barrier_pop, False, 0.7),
    ("MINE", sfx_mine, False, 0.7),
    ("MINE_HIT", sfx_mine_hit, False, 0.85),
    ("BELL", sfx_bell, False, 0.75),
    ("PLANE", sfx_plane, True, 0.8, 2),
]


def finish_oneshot(x):
    x = np.asarray(x, dtype=np.float64)
    x = filt(x, "hp", 22, 0.6)
    peak = np.max(np.abs(x))
    above = np.nonzero(np.abs(x) > peak * 10 ** (-66 / 20))[0]
    end = min(len(x), above[-1] + int(0.01 * SR)) if len(above) else len(x)
    x = x[:end]
    tail = np.sqrt(np.mean(x[-int(0.02 * SR):] ** 2))
    out_s = 0.02 if tail < peak * 0.01 else min(0.12, 0.2 * len(x) / SR)   # still ringing
    x = fade(x, 0.0006, min(out_s, 0.15 * len(x) / SR if out_s == 0.02 else out_s))
    x = x / np.max(np.abs(x)) * ONESHOT_PEAK
    if len(x) % 16:
        x = np.concatenate([x, np.zeros(16 - len(x) % 16)])
    return x


def finish_loop(x):
    x = np.asarray(x, dtype=np.float64)
    assert len(x) % 16 == 0, len(x)
    x = x - np.mean(x)
    return x / np.max(np.abs(x)) * LOOP_PEAK


def to_pcm16(x):
    return np.clip(np.rint(x * 32767), -32768, 32767).astype("<i2")


def write_wav(path, data, rate, channels):
    pcm = to_pcm16(np.asarray(data))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def write_wav_int(path, pcm, rate, channels):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.asarray(pcm, dtype="<i2").tobytes())


# ======================================================================
# Main
# ======================================================================

def build_sfx(out, previews):
    pak = bytearray()
    entries = []
    print(f"{'effect':12s} {'secs':>5s} {'peak':>6s} {'rms':>6s} {'dc':>8s}  notes")
    for name, fn, loop, vol, *down in SFX:
        x = np.asarray(fn(), dtype=np.float64)
        if not np.all(np.isfinite(x)):
            raise SystemExit(f"{name}: non-finite samples")
        x = finish_loop(x) if loop else finish_oneshot(x)
        # A trailing factor stores the effect at a fraction of the rate
        # (main RAM is tight; a low drone loses nothing at 11 kHz).
        div = down[0] if down else 1
        if div > 1:
            # Loops resample as three copies, keeping the middle one, so
            # the seam stays continuous.
            n = len(x)
            x = resample_poly(np.tile(x, 3) if loop else x, 1, div)
            if loop:
                x = x[n // div:2 * n // div]
        rate = SR // div
        if len(x) > MAX_SFX_SAMPLES:
            raise SystemExit(f"{name}: {len(x)} samples exceeds AICA limit")
        pcm = to_pcm16(x)
        while len(pak) % 32:
            pak.append(0)
        off = len(pak)
        pak += pcm.tobytes()
        if loop:
            # Guard samples: the start of the loop again, outside `bytes`, so
            # interpolation that peeks past the loop end reads continuous data.
            pak += pcm[:16].tobytes()
        entries.append((name, off, len(pcm) * 2, int(loop), vol, rate))
        d = np.abs(np.diff(x))
        peak = 20 * np.log10(np.max(np.abs(x)))
        rms = 20 * np.log10(np.sqrt(np.mean(x ** 2)))
        if loop:
            seam = abs(x[0] - x[-1])
            note = (f"loop seam |d|={seam:.4f} (median {np.median(d):.4f}, "
                    f"p99.9 {np.percentile(d, 99.9):.4f})")
            if seam > np.percentile(d, 99.9):
                raise SystemExit(f"{name}: loop seam discontinuity")
            preview = np.tile(x, 4)
        else:
            note = f"edges {abs(x[0]):.4f}/{np.max(np.abs(x[-32:])):.4f}"
            preview = x
        print(f"{name:12s} {len(x) / rate:5.2f} {peak:6.1f} {rms:6.1f} {np.mean(x):+8.5f}  {note}")
        write_wav(previews / f"{name.lower()}.wav", preview, rate, 1)
    while len(pak) % 32:
        pak.append(0)
    (out / "sfx.pak").write_bytes(bytes(pak))
    print(f"sfx.pak: {len(pak)} bytes ({len(pak) / 1024:.0f} KiB), {len(entries)} effects")
    return entries


# Names shown in the sound test.
SFX_LABELS = {
    "ENGINE_LOW": "ENGINE (LOW)", "ENGINE_HIGH": "ENGINE (HIGH)", "DRIFT": "TYRE SQUEAL",
    "SPARKS": "DRIFT SPARKS", "OFFROAD": "SAND RUMBLE", "BRAKE": "BRAKE SCREECH", "HOP": "HOP", "LAND": "LANDING",
    "CHARGE1": "MINI-TURBO 1", "CHARGE2": "MINI-TURBO 2", "CHARGE3": "MINI-TURBO 3",
    "BOOST": "BOOST", "DASH": "DASH PANEL", "BUMP": "WALL HIT", "KART_BUMP": "KART BUMP",
    "COUNT_BEEP": "COUNTDOWN BEEP", "GO": "GO!", "LAP": "LAP", "FINAL_LAP": "FINAL LAP",
    "FINISH": "FINISH", "MENU_MOVE": "MENU MOVE", "MENU_SELECT": "MENU SELECT",
    "MENU_BACK": "MENU BACK", "CHAO": "CHAO", "CHAO_HAPPY": "HAPPY CHAO", "CHAO_HEY": "CHAO?",
    "CHAO_CHU": "CHU-CHU", "CHAO_YAY": "YA-YA", "CHAO_WAH": "WAH!", "CHAO_BUU": "BUU!", "CHAO_HYA": "HYA!",
    "WHOOSH": "WHOOSH", "STAR": "STAR",
    "EMERALD": "EMERALD", "SPEAR": "CHAOS SPEAR", "SPEAR_HIT": "SPEAR HIT", "BARRIER": "BARRIER",
    "BARRIER_POP": "BARRIER POP", "MINE": "MINE", "MINE_HIT": "MINE BLAST", "BELL": "LOBBY BELL",
    "PLANE": "BIPLANE",
}


def write_header(out, entries):
    h = ["/* Generated by tools/build_audio.py. Do not edit by hand. */",
         "#ifndef DK_SOUND_ASSETS_H", "#define DK_SOUND_ASSETS_H", "",
         "#include <stdint.h>", "",
         f"#define DK_SFX_RATE {SFX_RATE}",
         "",
         "/* offset/bytes into sfx.pak, loop=1 for seamless loops, sample rate */",
         "typedef struct { uint32_t offset, bytes, loop, rate; } dk_sfx_asset_t;", "",
         "typedef enum {"]
    for name, *_ in entries:
        h.append(f"    DK_SFX_{name},")
    h += ["    DK_SFX_COUNT", "} dk_sfx_id_t;", "",
          "/* Trailing comment: suggested snd_sfx volume (0-255) at full intensity. */",
          "static const dk_sfx_asset_t dk_sfx_assets[DK_SFX_COUNT] __attribute__((unused)) = {"]
    for name, off, size, loop, vol, rate in entries:
        h.append(f"    {{ {off}u, {size}u, {loop}u, {rate}u }}, /* {name:12s} vol {int(round(vol * 255))} */")
    h += ["};", "", "/* Effect names for the sound test. */",
          "static const char *const dk_sfx_label[DK_SFX_COUNT] __attribute__((unused)) = {"]
    for name, *_ in entries:
        h.append(f'    "{SFX_LABELS[name]}",')
    h += ["};", "", "#endif /* DK_SOUND_ASSETS_H */", ""]
    (out / "sound_assets.h").write_text("\n".join(h))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output-dir", type=Path, required=True)
    args = ap.parse_args()
    out = args.output_dir
    previews = out / "previews"
    previews.mkdir(parents=True, exist_ok=True)

    entries = build_sfx(out, previews)
    write_header(out, entries)
    print(f"wrote {out / 'sound_assets.h'}")


if __name__ == "__main__":
    main()
