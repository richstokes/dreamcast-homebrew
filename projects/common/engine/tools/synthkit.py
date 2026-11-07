"""Sound synthesis helpers for the games' tools/build_audio.py generators.

Seeded RNGs, RBJ biquads, loop-safe processing, fades, placement, a
synthetic reverb and the finishing passes that turn a float signal into
22050 Hz 16-bit mono PCM for the AICA. Used by DreamKarts and Ridgeline.
"""

from __future__ import annotations

import zlib

import numpy as np
from scipy.signal import fftconvolve, lfilter

SFX_RATE = 22050
TAU = 2.0 * np.pi
ONESHOT_PEAK = 10 ** (-1.0 / 20)   # -1 dBFS
LOOP_PEAK = 10 ** (-4.0 / 20)      # -4 dBFS
MAX_SFX_SAMPLES = 65534            # AICA 16-bit length limit (KOS warns above)


def rng_for(name: str, salt: int = 0) -> np.random.Generator:
    """Independent seeded stream per asset, so editing one leaves others alone.
    salt separates one game's streams from another's."""
    return np.random.default_rng(zlib.crc32(name.encode()) + salt)


def midi_hz(n):
    return 440.0 * 2.0 ** ((np.asarray(n, dtype=np.float64) - 69) / 12.0)


# ======================================================================
# Filters
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
    return np.array(b) / a[0], np.array(a) / a[0]


def filt(x, kind, fc, q=0.7071, sr=SFX_RATE, gain_db=0.0):
    b, a = rbj(kind, fc, q, sr, gain_db)
    return lfilter(b, a, x, axis=0)


def periodic(fn, x):
    """Steady-state output of a (linear or memoryless) process over one
    period of a periodic signal (run three periods, keep the middle one),
    so the result loops seamlessly."""
    n = len(x)
    return fn(np.concatenate([x, x, x]))[n:2 * n]


def plfo(n, rate, rng, sr=SFX_RATE):
    """Periodic, band-limited random LFO in [-1, 1] that repeats every n."""
    k = max(1, int(rate * n / sr))
    spec = np.zeros(n // 2 + 1, dtype=complex)
    spec[1:k + 1] = rng.standard_normal(k) + 1j * rng.standard_normal(k)
    x = np.fft.irfft(spec, n=n)
    return x / (np.max(np.abs(x)) + 1e-12)


def softsat(x, drive=1.0):
    return np.tanh(drive * x) / np.tanh(drive)


# ======================================================================
# Envelopes, lengths and placement
# ======================================================================

def raised(n):
    return 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n))


def fade(x, in_s=0.0007, out_s=0.015, sr=SFX_RATE):
    x = x.copy()
    a = min(max(2, int(in_s * sr)), len(x))
    b = min(max(2, int(out_s * sr)), len(x))
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
    """Add sig into buf at sample start (clipped to buf; a negative start
    drops the head of sig)."""
    s = int(start)
    if s >= len(buf) or s + len(sig) <= 0:
        return
    if s < 0:
        sig, s = sig[-s:], 0
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


# ======================================================================
# Reverb
# ======================================================================

def make_ir(sr, seconds, t60, rng, lp_hz=7000.0, hp_hz=180.0, predelay=0.012, hf_ratio=0.5,
            early=()):
    """Synthetic diffuse reverb impulse with frequency-dependent decay.
    early is a tuple of (delay_s, gain) taps added as discrete reflections
    with random polarity."""
    n = int(seconds * sr)
    t = np.arange(n) / sr
    nz = rng.standard_normal(n)
    lo = filt(nz, "lp", 1800, sr=sr)
    ir = lo * 10 ** (-3 * t / t60) + (nz - lo) * 10 ** (-3 * t / (t60 * hf_ratio))
    ir = filt(filt(ir, "lp", min(lp_hz, 0.45 * sr), sr=sr), "hp", hp_hz, sr=sr)
    ir *= np.minimum(t / 0.02, 1.0)
    ir /= np.sqrt(np.sum(ir ** 2))
    if early:
        er = np.zeros(n)
        for d, g in early:
            er[int(d * sr)] += g * rng.choice([-1.0, 1.0])
        ir = ir + filt(er, "lp", 5000, sr=sr) * 0.6
        ir /= np.sqrt(np.sum(ir ** 2))
    return np.concatenate([np.zeros(int(predelay * sr)), ir])


def add_reverb(x, wet, seconds=1.2, t60=1.0, seed="rev", sr=SFX_RATE, lp_hz=6500.0, salt=0,
               early=()):
    ir = make_ir(sr, seconds, t60, rng_for(seed, salt), lp_hz=lp_hz, early=early)
    out = np.concatenate([x, np.zeros(len(ir))])
    out += wet * pad_to(fftconvolve(x, ir), len(out))
    return out


# ======================================================================
# Finishing
# ======================================================================

def finish_oneshot(x, sr=SFX_RATE, normalise=True):
    """DC-block, trim the silent tail, fade and (optionally) normalise a
    one-shot, padded to a multiple of 16 samples."""
    x = np.asarray(x, dtype=np.float64)
    x = filt(x, "hp", 22, 0.6, sr=sr)
    peak = np.max(np.abs(x))
    above = np.nonzero(np.abs(x) > peak * 10 ** (-66 / 20))[0]
    end = min(len(x), above[-1] + int(0.01 * sr)) if len(above) else len(x)
    x = x[:end]
    tail = np.sqrt(np.mean(x[-int(0.02 * sr):] ** 2))
    out_s = 0.02 if tail < peak * 0.01 else min(0.12, 0.2 * len(x) / sr)   # still ringing
    x = fade(x, 0.0006, min(out_s, 0.15 * len(x) / sr if out_s == 0.02 else out_s), sr)
    if normalise:
        x = x / np.max(np.abs(x)) * ONESHOT_PEAK
    if len(x) % 16:
        x = np.concatenate([x, np.zeros(16 - len(x) % 16)])
    return x


def finish_loop(x, normalise=True):
    x = np.asarray(x, dtype=np.float64)
    assert len(x) % 16 == 0, len(x)
    x = x - np.mean(x)
    if normalise:
        x = x / np.max(np.abs(x)) * LOOP_PEAK
    return x


def to_pcm16(x):
    return np.clip(np.rint(x * 32767), -32768, 32767).astype("<i2")
