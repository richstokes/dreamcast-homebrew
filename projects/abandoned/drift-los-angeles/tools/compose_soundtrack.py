#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "scipy>=1.11", "numba>=0.59"]
# ///
"""Compose and render Drift Los Angeles's original outrun soundtrack.

Every note and every sound is defined here, so the soundtrack is fully
original and reproducible. The palette deliberately stays inside what a
1984-1988 studio could do: LinnDrum/DMX-style drum synthesis with a gated
reverb snare, Juno-style chorused saw pads, Jupiter-style brass, hard-sync
leads, DX7-style two-operator FM bells and electric piano, Fairlight-style
orchestra hits, tape-style ping-pong delay and a plate reverb.

The renderer writes one 16-bit stereo WAV per track at the Dreamcast mixer
rate plus a tracks.json playlist manifest. tools/build_music_asset.py then
packs them into the ADPCM bank.
"""

from __future__ import annotations

import argparse
import json
import math
import wave
from pathlib import Path

import numpy as np
from numba import njit
from scipy import signal

SR = 44_100
OUT_SR = 22_050
TARGET_RMS = 0.19  # Matches the loudness of the soundtrack this replaces.
PEAK_CEILING = 0.89


# --------------------------------------------------------------------------
# DSP kernels
# --------------------------------------------------------------------------


@njit(cache=True)
def _svf(x, fc, res, drive, mode):
    """Zero-delay-feedback state-variable filter with a soft-saturated input."""
    n = x.shape[0]
    y = np.empty(n, np.float32)
    ic1 = 0.0
    ic2 = 0.0
    k = 2.0 - 2.0 * res
    for i in range(n):
        f = fc[i]
        if f > SR * 0.45:
            f = SR * 0.45
        elif f < 12.0:
            f = 12.0
        g = math.tan(math.pi * f / SR)
        a1 = 1.0 / (1.0 + g * (g + k))
        a2 = g * a1
        a3 = g * a2
        v0 = math.tanh(x[i] * drive) / drive
        v3 = v0 - ic2
        v1 = a1 * ic1 + a2 * v3
        v2 = ic2 + a2 * ic1 + a3 * v3
        ic1 = 2.0 * v1 - ic1
        ic2 = 2.0 * v2 - ic2
        if mode == 0:
            y[i] = v2
        elif mode == 1:
            y[i] = v1 * k
        else:
            y[i] = v0 - k * v1 - v2
    return y


@njit(cache=True)
def _sync_saw(freq, ratio, rate):
    """Hard-synced sawtooth; rendered oversampled by the caller."""
    n = freq.shape[0]
    y = np.empty(n, np.float32)
    master = 0.0
    slave = 0.0
    for i in range(n):
        master += freq[i] / rate
        slave += freq[i] * ratio[i] / rate
        if master >= 1.0:
            master -= 1.0
            slave = master * ratio[i]
        slave -= math.floor(slave)
        y[i] = 2.0 * slave - 1.0
    return y


@njit(cache=True)
def _chorus(left, right, rate, base, depth, mix):
    """Juno-style bucket-brigade chorus: one LFO, opposite phase per side."""
    n = left.shape[0]
    size = 4096
    buf_l = np.zeros(size, np.float32)
    buf_r = np.zeros(size, np.float32)
    out_l = np.empty(n, np.float32)
    out_r = np.empty(n, np.float32)
    w = 0
    for i in range(n):
        buf_l[w] = left[i]
        buf_r[w] = right[i]
        lfo = 2.0 * abs(2.0 * ((i * rate / SR) % 1.0) - 1.0) - 1.0
        for side in range(2):
            d = (base + depth * lfo * (1.0 if side == 0 else -1.0)) * SR
            pos = w - d
            while pos < 0.0:
                pos += size
            j = int(pos)
            frac = pos - j
            buf = buf_l if side == 0 else buf_r
            wet = buf[j % size] * (1.0 - frac) + buf[(j + 1) % size] * frac
            if side == 0:
                out_l[i] = left[i] * (1.0 - mix * 0.5) + wet * mix
            else:
                out_r[i] = right[i] * (1.0 - mix * 0.5) + wet * mix
        w = (w + 1) % size
    return out_l, out_r


@njit(cache=True)
def _pingpong(mono, delay, feedback, damp):
    """Tape-style ping-pong echo with a darkening feedback path."""
    n = mono.shape[0]
    size = delay + 1
    buf_l = np.zeros(size, np.float32)
    buf_r = np.zeros(size, np.float32)
    out_l = np.empty(n, np.float32)
    out_r = np.empty(n, np.float32)
    lp_l = 0.0
    lp_r = 0.0
    w = 0
    for i in range(n):
        r = (w + 1) % size
        echo_l = buf_l[r]
        echo_r = buf_r[r]
        lp_l += (echo_l - lp_l) * damp
        lp_r += (echo_r - lp_r) * damp
        buf_l[w] = mono[i] + lp_r * feedback
        buf_r[w] = lp_l * feedback
        out_l[i] = echo_l
        out_r[i] = echo_r
        w = r
    return out_l, out_r


@njit(cache=True)
def _freeverb(mono, room, damp, spread):
    """Classic Schroeder/Moorer plate approximation (Freeverb tunings)."""
    combs = np.array([1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617])
    allpasses = np.array([556, 441, 341, 225])
    n = mono.shape[0]
    out = np.zeros((2, n), np.float32)
    feedback = room * 0.28 + 0.7
    damp1 = damp * 0.4
    for side in range(2):
        offset = spread if side == 1 else 0
        comb_bufs = [np.zeros(c + offset, np.float32) for c in combs]
        comb_idx = np.zeros(8, np.int64)
        comb_store = np.zeros(8)
        ap_bufs = [np.zeros(a + offset, np.float32) for a in allpasses]
        ap_idx = np.zeros(4, np.int64)
        for i in range(n):
            x = mono[i] * 0.015
            acc = 0.0
            for c in range(8):
                buf = comb_bufs[c]
                j = comb_idx[c]
                y = buf[j]
                comb_store[c] = y * (1.0 - damp1) + comb_store[c] * damp1
                buf[j] = x + comb_store[c] * feedback
                comb_idx[c] = (j + 1) % buf.shape[0]
                acc += y
            for a in range(4):
                buf = ap_bufs[a]
                j = ap_idx[a]
                b = buf[j]
                buf[j] = acc + b * 0.5
                acc = b - acc
                ap_idx[a] = (j + 1) % buf.shape[0]
            out[side, i] = acc
    return out


@njit(cache=True)
def _compress(left, right, threshold, ratio, attack, release, knee):
    """Stereo-linked feed-forward bus compressor; returns the gain curve."""
    n = left.shape[0]
    gain = np.empty(n, np.float32)
    env = 0.0
    a_att = math.exp(-1.0 / (attack * SR))
    a_rel = math.exp(-1.0 / (release * SR))
    for i in range(n):
        level = max(abs(left[i]), abs(right[i]))
        if level > env:
            env = a_att * env + (1.0 - a_att) * level
        else:
            env = a_rel * env + (1.0 - a_rel) * level
        db = 20.0 * math.log10(env + 1e-9)
        over = db - threshold
        if over <= -knee:
            reduction = 0.0
        elif over >= knee:
            reduction = over * (1.0 - 1.0 / ratio)
        else:
            reduction = (1.0 - 1.0 / ratio) * (over + knee) ** 2 / (4.0 * knee)
        gain[i] = 10.0 ** (-reduction / 20.0)
    return gain


def svf(x, fc, res=0.0, drive=1.0, mode="lp"):
    fc = np.broadcast_to(np.asarray(fc, np.float32), x.shape).astype(np.float32)
    code = {"lp": 0, "bp": 1, "hp": 2}[mode]
    return _svf(x.astype(np.float32), fc, float(res), float(drive), code)


def butter(x, cutoff, kind, order=2):
    sos = signal.butter(order, cutoff, kind, fs=SR, output="sos")
    return signal.sosfilt(sos, x, axis=-1).astype(np.float32)


def shelf(x, freq, gain_db, kind):
    """RBJ-cookbook shelving EQ (slope 1)."""
    a = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * np.pi * freq / SR
    cos_w, alpha = np.cos(w0), np.sin(w0) / 2.0 * np.sqrt(2.0)
    root = 2.0 * np.sqrt(a) * alpha
    sign = 1.0 if kind == "high" else -1.0
    b = [a * ((a + 1) + sign * (a - 1) * cos_w + root),
         -2 * sign * a * ((a - 1) + sign * (a + 1) * cos_w),
         a * ((a + 1) + sign * (a - 1) * cos_w - root)]
    den = [(a + 1) - sign * (a - 1) * cos_w + root,
           2 * sign * ((a - 1) - sign * (a + 1) * cos_w),
           (a + 1) - sign * (a - 1) * cos_w - root]
    return signal.lfilter(b, den, x, axis=-1).astype(np.float32)


# --------------------------------------------------------------------------
# Oscillators and envelopes
# --------------------------------------------------------------------------


NOTE_OFFSETS = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def n(name: str) -> int:
    """MIDI number for names such as 'A4', 'F#3' or 'Bb2'."""
    pitch = NOTE_OFFSETS[name[0]]
    rest = name[1:]
    while rest and rest[0] in "#b":
        pitch += 1 if rest[0] == "#" else -1
        rest = rest[1:]
    return pitch + 12 * (int(rest) + 1)


def hz(midi: float) -> float:
    return 440.0 * 2.0 ** ((midi - 69.0) / 12.0)


def _polyblep(t, dt):
    out = np.zeros_like(t)
    lo = t < dt
    x = t[lo] / dt[lo]
    out[lo] = x + x - x * x - 1.0
    hi = t > 1.0 - dt
    x = (t[hi] - 1.0) / dt[hi]
    out[hi] = x * x + x + x + 1.0
    return out


def phase_of(freq, phase0=0.0):
    freq = np.asarray(freq, np.float64)
    return (phase0 + np.cumsum(freq) / SR) % 1.0


def saw(freq, count, phase0=0.0):
    freq = np.broadcast_to(np.asarray(freq, np.float64), (count,))
    t = phase_of(freq, phase0)
    dt = np.minimum(freq / SR, 0.5)
    return (2.0 * t - 1.0 - _polyblep(t, dt)).astype(np.float32)


def pulse(freq, count, width=0.5, phase0=0.0):
    freq = np.broadcast_to(np.asarray(freq, np.float64), (count,))
    return (saw(freq, count, phase0) - saw(freq, count, (phase0 + width) % 1.0)) * 0.5


def sine(freq, count, phase0=0.0):
    freq = np.broadcast_to(np.asarray(freq, np.float64), (count,))
    return np.sin(2.0 * np.pi * phase_of(freq, phase0)).astype(np.float32)


def times(count):
    return np.arange(count, dtype=np.float32) / SR


def adsr(count, gate, attack, decay, sustain, release):
    """Linear-attack, exponential-decay/release envelope sampled per frame."""
    t = times(count)
    env = np.where(
        t < attack,
        t / max(attack, 1e-4),
        sustain + (1.0 - sustain) * np.exp(-(t - attack) / max(decay, 1e-4)),
    )
    held = env[min(int(gate * SR), count - 1)]
    after = t >= gate
    env[after] = held * np.exp(-(t[after] - gate) / max(release, 1e-4))
    return env.astype(np.float32)


def vibrato(count, freq, depth_cents=10.0, rate=5.5, delay=0.25):
    t = times(count)
    fade = np.clip((t - delay) / 0.35, 0.0, 1.0)
    return freq * 2.0 ** (depth_cents * fade * np.sin(2 * np.pi * rate * t) / 1200.0)


# --------------------------------------------------------------------------
# Song container and mixer
# --------------------------------------------------------------------------


BUS_FX = {
    # name: (chorus, delay send, reverb send)
    "drums": (False, 0.0, 0.06),
    "toms": (False, 0.0, 0.30),
    "bass": (False, 0.0, 0.0),
    "pad": (True, 0.0, 0.34),
    "arp": (False, 0.34, 0.16),
    "keys": (True, 0.18, 0.22),
    "bell": (True, 0.32, 0.30),
    "lead": (False, 0.24, 0.24),
    "brass": (True, 0.10, 0.26),
    "hits": (False, 0.12, 0.42),
    "fx": (False, 0.0, 0.50),
}


class Song:
    def __init__(self, title: str, bpm: float, bars: int, seed: int, tail: float = 2.6):
        self.title = title
        self.bpm = bpm
        self.bars = bars
        self.spb = 60.0 / bpm
        self.length = int(round((bars * 4 * self.spb + tail) * SR))
        self.buses = {name: np.zeros((2, self.length), np.float32) for name in BUS_FX}
        self.gated = np.zeros(self.length, np.float32)
        self.snare_hits: list[int] = []
        self.rng = np.random.default_rng(seed)

    # Time helpers -------------------------------------------------------
    def sample(self, beat: float) -> int:
        return int(round(beat * self.spb * SR))

    def secs(self, beats: float) -> float:
        return beats * self.spb

    def add(self, bus, beat, mono, pan=0.0, gain=1.0):
        start = self.sample(beat)
        if start >= self.length:
            return
        seg = mono[: self.length - start] * gain
        angle = (np.clip(pan, -1.0, 1.0) + 1.0) * np.pi / 4.0
        self.buses[bus][0, start : start + seg.size] += seg * np.cos(angle) * np.sqrt(2)
        self.buses[bus][1, start : start + seg.size] += seg * np.sin(angle) * np.sqrt(2)

    def rand(self, spread=1.0):
        return float(self.rng.uniform(-spread, spread))

    def phase(self):
        return float(self.rng.uniform(0.0, 1.0))

    # Drum machine --------------------------------------------------------
    def kick(self, beat, vel=1.0):
        count = int(0.5 * SR)
        t = times(count)
        f = 54.0 + 118.0 * np.exp(-t / 0.032) + 30.0 * np.exp(-t / 0.004)
        body = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / 0.19)
        body = np.tanh(body * 1.8) / np.tanh(1.8)
        click = svf(self.rng.standard_normal(count).astype(np.float32), 3200.0, 0.2, mode="bp")
        click *= np.exp(-t / 0.0035)
        self.add("drums", beat, (body * 0.85 + click * 0.6) * vel)

    def snare(self, beat, vel=1.0, gate_send=1.0):
        count = int(0.42 * SR)
        t = times(count)
        drop = 1.0 + 0.25 * np.exp(-t / 0.012)
        tone = (sine(185.0 * drop, count) * 0.62 + sine(332.0 * drop, count) * 0.34) * np.exp(-t / 0.055)
        noise = self.rng.standard_normal(count).astype(np.float32)
        noise = svf(noise, 5200.0, 0.1) - svf(noise, 900.0, 0.0)
        noise *= np.exp(-t / 0.13) * 0.9 + np.exp(-t / 0.018) * 0.5
        hit = (tone * 0.75 + noise * 0.95) * vel
        self.add("drums", beat, hit, pan=0.04)
        start = self.sample(beat)
        seg = hit[: max(0, self.length - start)] * gate_send
        self.gated[start : start + seg.size] += seg
        if gate_send > 0.0:
            self.snare_hits.append(start)

    def clap(self, beat, vel=1.0):
        count = int(0.35 * SR)
        t = times(count)
        noise = svf(self.rng.standard_normal(count).astype(np.float32), 1250.0, 0.45, mode="bp")
        env = np.zeros(count, np.float32)
        for offset in (0.0, 0.011, 0.023):
            env += np.where(t >= offset, np.exp(-(t - offset) / 0.0065), 0.0)
        env += np.where(t >= 0.03, np.exp(-(t - 0.03) / 0.11), 0.0) * 0.8
        hit = noise * env * vel * 0.9
        self.add("drums", beat, hit, pan=-0.06)
        start = self.sample(beat)
        seg = hit[: max(0, self.length - start)] * 0.6
        self.gated[start : start + seg.size] += seg

    def _metal(self, count):
        t = np.arange(count) / SR
        partials = (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)
        tone = sum(np.sign(np.sin(2 * np.pi * f * 1.62 * t + self.phase() * 6.28)) for f in partials)
        tone = tone.astype(np.float32) / 6.0
        noise = self.rng.standard_normal(count).astype(np.float32) * 0.6
        return svf(tone + noise, 7600.0, 0.3, mode="hp")

    def hat(self, beat, vel=0.6, open_=False, pan=0.22):
        decay = 0.19 if open_ else 0.034
        count = int((decay * 6 + 0.01) * SR)
        env = np.exp(-times(count) / decay)
        self.add("drums", beat, self._metal(count) * env * vel * 0.75, pan=pan)

    def crash(self, beat, vel=0.8):
        count = int(2.6 * SR)
        t = times(count)
        body = self._metal(count) * 0.7 + svf(self.rng.standard_normal(count).astype(np.float32), 5000.0, 0.0, mode="hp")
        env = np.exp(-t / 0.75) * (1.0 - np.exp(-t / 0.002))
        self.add("drums", beat, body * env * vel * 0.34, pan=-0.3)

    def tom(self, beat, pitch, vel=1.0, pan=0.0):
        count = int(0.6 * SR)
        t = times(count)
        f = pitch * (1.0 + 0.55 * np.exp(-t / 0.045))
        body = sine(f, count) * np.exp(-t / 0.24)
        skin = svf(self.rng.standard_normal(count).astype(np.float32), pitch * 5.0, 0.3, mode="bp")
        skin *= np.exp(-t / 0.03) * 0.4
        self.add("toms", beat, (body + skin) * vel * 0.85, pan=pan)

    def tom_fill(self, beat, steps=8, start_pitch=210.0, end_pitch=95.0):
        for i in range(steps):
            frac = i / max(steps - 1, 1)
            pitch = start_pitch * (end_pitch / start_pitch) ** frac
            self.tom(beat + i * 0.25, pitch, 0.75 + 0.25 * frac, pan=0.55 - 1.1 * frac)

    def snare_roll(self, beat, beats=4.0, per_beat=4):
        steps = int(beats * per_beat)
        for i in range(steps):
            frac = i / steps
            self.snare(beat + i / per_beat, 0.25 + 0.65 * frac, gate_send=0.35 * frac)

    # Synth voices -------------------------------------------------------
    def bass(self, beat, midi, beats, accent=1.0, cutoff=320.0, env_amt=2400.0,
             decay=0.11, res=0.42, sub=0.08):
        dur = self.secs(beats)
        count = int((dur + 0.05) * SR)
        f = hz(midi)
        osc = saw(f, count, self.phase()) * 0.7 + pulse(f * 1.003, count, 0.5, self.phase()) * 0.45
        osc += sine(f * 0.5, count) * sub
        t = times(count)
        y = svf(osc, cutoff + env_amt * accent * np.exp(-t / decay), res, drive=1.6)
        amp = adsr(count, dur, 0.002, 0.25, 0.8, 0.018)
        self.add("bass", beat, y * amp * (0.8 + 0.2 * accent) * 0.8)

    def pad(self, beat, notes, beats, cutoff=1900.0, attack=0.35, release=0.9, gain=1.0, bus="pad"):
        dur = self.secs(beats)
        count = int((dur + release * 5) * SR)
        t = times(count)
        mix = np.zeros(count, np.float32)
        for i, midi in enumerate(notes):
            f = hz(midi)
            for cents in (-6.0, 5.0):
                mix += saw(f * 2 ** (cents / 1200), count, self.phase()) * 0.5
            mix += pulse(f * 0.5, count, 0.42, self.phase()) * (0.22 if i == 0 else 0.0)
        mix /= max(len(notes), 1) ** 0.5
        fc = cutoff * (1.0 + 0.18 * np.sin(2 * np.pi * 0.23 * t + self.phase() * 6.28))
        y = svf(mix, fc * (0.55 + 0.45 * (1 - np.exp(-t / (attack * 1.5)))), 0.12)
        amp = adsr(count, dur, attack, 1.0, 1.0, release)
        self.add(bus, beat, y * amp * 0.34 * gain)

    def pluck(self, beat, midi, beats, vel=1.0, cutoff=700.0, env_amt=3600.0,
              decay=0.09, width=0.3, pan=0.0, bus="arp"):
        dur = self.secs(beats)
        count = int((dur + 0.25) * SR)
        f = hz(midi)
        t = times(count)
        osc = pulse(f, count, width, self.phase()) * 0.8 + saw(f * 1.004, count, self.phase()) * 0.3
        y = svf(osc, cutoff + env_amt * vel * np.exp(-t / decay), 0.35)
        amp = adsr(count, dur, 0.001, 0.16, 0.25, 0.06)
        self.add(bus, beat, y * amp * vel * 0.42, pan=pan)

    def brass(self, beat, midi, beats, vel=1.0, pan=0.0, bus="brass", bright=1.0):
        dur = self.secs(beats)
        count = int((dur + 0.4) * SR)
        f = vibrato(count, hz(midi), 7.0, 5.2, 0.3)
        t = times(count)
        osc = saw(f * 2 ** (-7 / 1200), count, self.phase()) + saw(f * 2 ** (7 / 1200), count, self.phase())
        swell = 1.0 - np.exp(-t / 0.045)
        fc = (700.0 + 3800.0 * bright * swell * (0.6 + 0.4 * np.exp(-t / 0.3))) * (0.7 + 0.3 * vel)
        y = svf(osc * 0.5, fc, 0.18, drive=1.3)
        amp = adsr(count, dur, 0.022, 0.4, 0.82, 0.11)
        self.add(bus, beat, y * amp * vel * 0.6, pan=pan)

    def square_lead(self, beat, midi, beats, vel=1.0, prev=None, glide=0.05, pan=0.0):
        """Mellow pulse lead with portamento and delayed vibrato."""
        dur = self.secs(beats)
        count = int((dur + 0.35) * SR)
        t = times(count)
        target = hz(midi)
        f = np.full(count, target)
        if prev is not None:
            start = hz(prev)
            f = target + (start - target) * np.exp(-t / glide)
        f = vibrato(count, f, 11.0, 5.6, 0.22)
        osc = pulse(f, count, 0.42, self.phase()) * 0.75 + saw(f * 1.002, count, self.phase()) * 0.3
        y = svf(osc, 2600.0 + 1400.0 * np.exp(-t / 0.2), 0.22)
        amp = adsr(count, dur, 0.012, 0.5, 0.85, 0.12)
        self.add("lead", beat, y * amp * vel * 0.58, pan=pan)

    def sync_lead(self, beat, midi, beats, vel=1.0, prev=None, glide=0.035, pan=0.0):
        """Screaming hard-sync lead, rendered at 4x and decimated."""
        over = 4
        dur = self.secs(beats)
        count = int((dur + 0.3) * SR)
        big = count * over
        t = np.arange(big, dtype=np.float64) / (SR * over)
        target = hz(midi)
        f = np.full(big, target)
        if prev is not None:
            f = target + (hz(prev) - target) * np.exp(-t / glide)
        f = f * 2.0 ** (12.0 * np.clip((t - 0.28) / 0.3, 0, 1) * np.sin(2 * np.pi * 5.8 * t) / 1200.0)
        ratio = 1.6 + 2.2 * np.exp(-t / 0.16) + 0.25 * np.sin(2 * np.pi * 0.9 * t)
        raw = _sync_saw(f.astype(np.float64), ratio.astype(np.float64), float(SR * over))
        raw = signal.resample_poly(raw, 1, over).astype(np.float32)[:count]
        body = saw(f[::over][:count] * 0.5, count, self.phase()) * 0.35
        y = svf(raw * 0.7 + body, 4200.0, 0.1)
        amp = adsr(count, dur, 0.006, 0.4, 0.85, 0.09)
        self.add("lead", beat, y * amp * vel * 0.5, pan=pan)

    def fm_bell(self, beat, midi, beats, vel=1.0, pan=0.0, ratio=3.5, index=2.6, decay=1.6, bus="bell"):
        dur = self.secs(beats)
        count = int((dur + decay * 2.2) * SR)
        t = times(count)
        f = hz(midi)
        mod = np.sin(2 * np.pi * f * ratio * t) * index * vel * np.exp(-t / (decay * 0.35))
        car = np.sin(2 * np.pi * f * t + mod) * np.exp(-t / decay)
        car += np.sin(2 * np.pi * f * 2.0 * t) * 0.18 * np.exp(-t / (decay * 0.3))
        release = adsr(count, dur + decay * 0.4, 0.0015, 1.0, 1.0, 0.25)
        self.add(bus, beat, (car * release * vel * 0.36).astype(np.float32), pan=pan)

    def epiano(self, beat, midi, beats, vel=0.8, pan=0.0):
        """DX7 E.PIANO 1 flavour: a soft 1:1 body stack plus a 14:1 tine."""
        dur = self.secs(beats)
        count = int((dur + 0.6) * SR)
        t = times(count)
        f = hz(midi)
        body_mod = np.sin(2 * np.pi * f * t) * (0.9 + 1.2 * vel) * np.exp(-t / 0.55)
        body = np.sin(2 * np.pi * f * 1.002 * t + body_mod) * np.exp(-t / 1.6)
        tine_ratio = 14.0 if f * 14 < 9000 else 7.0
        tine_mod = np.sin(2 * np.pi * f * tine_ratio * t) * 1.6 * vel * np.exp(-t / 0.035)
        tine = np.sin(2 * np.pi * f * t + tine_mod) * np.exp(-t / 0.35) * 0.4
        release = adsr(count, dur, 0.001, 1.0, 1.0, 0.16)
        self.add("keys", beat, ((body + tine) * release * vel * 0.3).astype(np.float32), pan=pan)

    def orch_hit(self, beat, root, vel=1.0, pan=0.0, chord=(0, 7, 12, 19, 24)):
        count = int(0.9 * SR)
        t = times(count)
        stack = np.zeros(count, np.float32)
        for interval in chord:
            f = hz(root + interval) * (1.0 + 0.01 * np.exp(-t / 0.05))
            stack += saw(f * 2 ** (self.rand(9) / 1200), count, self.phase())
        stack /= len(chord) ** 0.5
        noise = self.rng.standard_normal(count).astype(np.float32) * np.exp(-t / 0.025) * 0.8
        y = svf(stack + noise, 900.0 + 7000.0 * np.exp(-t / 0.09), 0.1)
        amp = np.exp(-t / 0.16) * (1.0 - np.exp(-t / 0.0015))
        self.add("hits", beat, y * amp * vel * 0.62, pan=pan)

    def riser(self, beat, beats, gain=0.5):
        dur = self.secs(beats)
        count = int(dur * SR)
        t = times(count)
        frac = t / dur
        noise = self.rng.standard_normal(count).astype(np.float32)
        y = svf(noise, 300.0 * (40.0 ** frac), 0.55, mode="bp")
        self.add("fx", beat, y * frac**1.6 * gain * 0.5, pan=0.0)

    # Mixdown ------------------------------------------------------------
    def render(self) -> np.ndarray:
        drums = self.buses["drums"]
        if self.snare_hits:
            # The 1980s gated-reverb snare: a big room chopped off after
            # ~260 ms so it explodes without washing over the groove.
            room = _freeverb(butter(self.gated, 250.0, "highpass"), 0.93, 0.22, 23)
            gate = np.zeros(self.length, np.float32)
            open_len = int(0.26 * SR)
            for hit in self.snare_hits:
                gate[hit : hit + open_len] = 1.0
            gate = signal.lfilter([0.02], [1.0, -0.98], gate).astype(np.float32)
            drums = drums + room * gate * 3.4

        verb_send = np.zeros(self.length, np.float32)
        delay_send = np.zeros(self.length, np.float32)
        mix = np.zeros((2, self.length), np.float32)
        for name, (chorus, delay, verb) in BUS_FX.items():
            bus = drums if name == "drums" else self.buses[name]
            if not np.any(bus):
                continue
            if chorus:
                bus = np.stack(_chorus(bus[0], bus[1], 0.48, 0.0045, 0.0019, 0.62))
            mix += bus
            mono = bus.sum(axis=0) * 0.5
            verb_send += mono * verb
            delay_send += mono * delay

        dotted_eighth = int(round(self.spb * 0.75 * SR))
        echo = np.stack(_pingpong(butter(delay_send, 350.0, "highpass"), dotted_eighth, 0.42, 0.35))
        mix += echo * 0.55
        verb_send += echo.sum(axis=0) * 0.12
        predelay = int(0.024 * SR)
        verb_in = np.concatenate([np.zeros(predelay, np.float32), verb_send[:-predelay]])
        verb_in = butter(butter(verb_in, 280.0, "highpass"), 7000.0, "lowpass")
        mix += _freeverb(verb_in, 0.86, 0.45, 23) * 1.25

        # Master: low cut, gentle glue compression, then level.
        mix = butter(mix, 38.0, "highpass", order=4)
        mix = shelf(mix, 90.0, -2.5, "low")
        mix = shelf(mix, 3200.0, 3.0, "high")
        mix *= 0.5 / (np.sqrt(np.mean(mix**2)) + 1e-9) * 0.35
        gain = _compress(mix[0], mix[1], -18.0, 2.6, 0.012, 0.16, 6.0)
        mix = mix * gain
        # Final fade on the release tail so the playlist hand-off is clean.
        fade = int(0.9 * SR)
        mix[:, -fade:] *= np.linspace(1.0, 0.0, fade, dtype=np.float32) ** 2
        return mix


def master_to_output(mix: np.ndarray) -> np.ndarray:
    out = signal.resample_poly(mix, 1, SR // OUT_SR, axis=1).astype(np.float32)
    # Normalise loudness, then a soft ceiling for the rare peaks.
    for _ in range(3):
        out *= TARGET_RMS / (np.sqrt(np.mean(out**2)) + 1e-9)
        out = PEAK_CEILING * np.tanh(out / PEAK_CEILING)
    return out


# --------------------------------------------------------------------------
# Music helpers
# --------------------------------------------------------------------------


def play_line(song: Song, voice, bar: int, notes, **kwargs):
    """Play (note, beat, beats) triples; legato voices glide from the prior note."""
    prev = None
    for name, beat, beats in notes:
        midi = n(name) if isinstance(name, str) else name
        if voice in (song.square_lead, song.sync_lead):
            voice(bar * 4 + beat, midi, beats, prev=prev, **kwargs)
            prev = midi
        else:
            voice(bar * 4 + beat, midi, beats, **kwargs)


def shift_line(notes, semitones):
    return [(n(name) + semitones, beat, beats) for name, beat, beats in notes]


def harmony_below(notes, scale):
    """Diatonic third below each melody note within the given scale."""
    out = []
    pcs = sorted(scale)
    for name, beat, beats in notes:
        midi = n(name)
        steps = 0
        cand = midi
        while steps < 2:
            cand -= 1
            if cand % 12 in pcs:
                steps += 1
        out.append((cand, beat, beats))
    return out


def bars_of(lines):
    """Flatten a list of per-bar note lists into (note, beat, beats) triples."""
    out = []
    for bar, line in enumerate(lines):
        out.extend((name, bar * 4 + beat, beats) for name, beat, beats in line)
    return out


def drum_bar(song: Song, bar, kick, snare, hats, hat_vel=None, open_hats=(), clap=False):
    beat0 = bar * 4
    for step in kick:
        song.kick(beat0 + step / 4, 1.0 if step % 4 == 0 else 0.82)
    for step in snare:
        song.snare(beat0 + step / 4)
        if clap:
            song.clap(beat0 + step / 4, 0.7)
    for i, step in enumerate(hats):
        vel = hat_vel[i % len(hat_vel)] if hat_vel else 0.6
        song.hat(beat0 + step / 4, vel * (0.92 + 0.08 * song.rand()))
    for step in open_hats:
        song.hat(beat0 + step / 4, 0.5, open_=True, pan=-0.18)


# --------------------------------------------------------------------------
# Track 1: Blue Hour Boulevard (A minor, 112 BPM)
# --------------------------------------------------------------------------


def blue_hour_boulevard() -> Song:
    s = Song("Blue Hour Boulevard", 112.0, 36, seed=1984)
    A_MINOR = {9, 11, 0, 2, 4, 5, 7}
    chords = {
        "Am": ([n("A3"), n("C4"), n("E4"), n("A4")], n("A1")),
        "F": ([n("A3"), n("C4"), n("F4"), n("A4")], n("F1")),
        "C": ([n("G3"), n("C4"), n("E4"), n("G4")], n("C2")),
        "G": ([n("G3"), n("B3"), n("D4"), n("G4")], n("G1")),
        "Dm": ([n("A3"), n("D4"), n("F4"), n("A4")], n("D2")),
        "E": ([n("G#3"), n("B3"), n("E4"), n("G#4")], n("E1")),
    }
    arp_shape = [0, 1, 2, 3, 2, 1, 2, 3, 0, 1, 2, 3, 2, 3, 1, 2]

    progression = (
        ["Am", "F", "C", "G"]  # intro 0-3
        + ["Am", "F", "C", "G"] * 2  # verse 4-11
        + ["Am", "F", "C", "G"] * 2  # chorus 12-19
        + ["Dm", "F", "G", "E"]  # bridge 20-23
        + ["Am", "F", "C", "G"] * 2  # chorus 24-31
        + ["F", "G", "Am", "Am"]  # ending 32-35
    )

    chorus_melody = [
        [("A4", 0, 1.5), ("C5", 1.5, 0.5), ("E5", 2, 1), ("D5", 3, 0.5), ("C5", 3.5, 0.5)],
        [("C5", 0, 1.5), ("A4", 1.5, 0.5), ("C5", 2, 1), ("D5", 3, 0.5), ("E5", 3.5, 0.5)],
        [("E5", 0, 1.5), ("G5", 1.5, 0.5), ("E5", 2, 1), ("D5", 3, 0.5), ("C5", 3.5, 0.5)],
        [("D5", 0, 1.5), ("B4", 1.5, 0.5), ("G4", 2, 1.5), ("E5", 3.5, 0.5)],
        [("E5", 0, 0.5), ("A4", 0.5, 1), ("C5", 1.5, 0.5), ("E5", 2, 1), ("A5", 3, 1)],
        [("G5", 0, 1), ("F5", 1, 0.5), ("E5", 1.5, 0.5), ("F5", 2, 1), ("E5", 3, 0.5), ("C5", 3.5, 0.5)],
        [("E5", 0, 1.5), ("D5", 1.5, 0.5), ("C5", 2, 1), ("D5", 3, 1)],
        [("D5", 0, 1.5), ("B4", 1.5, 0.5), ("D5", 2, 1), ("E5", 3, 1)],
    ]
    verse_bells = [
        [("E5", 0, 1), ("A5", 1, 1), ("G5", 2, 0.5), ("E5", 2.5, 1.5)],
        [("C5", 0, 1), ("F5", 1, 1), ("E5", 2, 2)],
        [("G5", 0, 1), ("E5", 1, 1), ("D5", 2, 1), ("C5", 3, 1)],
        [("B4", 0, 1), ("D5", 1, 1), ("G5", 2, 2)],
    ]
    bridge_lead = [
        [("F5", 0, 2), ("E5", 2, 1), ("D5", 3, 1)],
        [("C5", 0, 2), ("A4", 2, 1), ("C5", 3, 1)],
        [("B4", 0, 1), ("D5", 1, 1), ("G5", 2, 2)],
        [("G#5", 0, 2), ("B5", 2, 1.5), ("E5", 3.5, 0.5)],
    ]

    for bar, name in enumerate(progression):
        notes, root = chords[name]
        beat0 = bar * 4
        final = bar >= 34
        if final:
            if bar == 34:
                s.pad(beat0, notes + [n("E5")], 7.0, cutoff=2400.0, attack=0.05, release=1.4, gain=1.2)
                s.bass(beat0, root, 6.0, cutoff=260.0, env_amt=1500.0, decay=0.4)
                s.kick(beat0)
                s.crash(beat0, 1.0)
                s.snare(beat0, 0.9)
                s.orch_hit(beat0, n("A3"), 0.7, chord=(0, 7, 12, 15, 19, 24))
                for i, bell in enumerate(["E6", "C6", "A5", "E5", "C5", "A4"]):
                    s.fm_bell(beat0 + 1 + i * 0.5, n(bell), 0.5, 0.55 - i * 0.05, pan=0.4 - i * 0.16)
            continue

        intro = bar < 4
        verse = 4 <= bar < 12
        chorus = 12 <= bar < 20 or 24 <= bar < 32
        bridge = 20 <= bar < 24
        ending = bar >= 32

        # Pads throughout; brighter in the choruses.
        s.pad(beat0, notes, 4.0, cutoff=2600.0 if chorus else 1700.0, attack=0.3 if intro else 0.12)

        # Sixteenth-note arpeggio: a filter opening across the intro.
        if not verse:
            arp_notes = [notes[1] + 12, notes[2] + 12, notes[3] + 12, notes[1] + 24]
            opening = min(1.0, (bar + 1) / 4.0) if intro else 1.0
            for step, idx in enumerate(arp_shape):
                s.pluck(beat0 + step / 4, arp_notes[idx], 0.2, vel=0.55 + 0.25 * (step % 4 == 0),
                        cutoff=400.0 + 900.0 * opening, env_amt=900.0 + 2400.0 * opening,
                        pan=-0.35 if step % 2 else 0.35)

        # Octave-pumping bass.
        if not intro:
            for step in range(0, 16, 2):
                midi = root + (12 if step % 4 == 2 else 0)
                s.bass(beat0 + step / 4, midi, 0.4, accent=1.0 if step % 8 == 0 else 0.7)

        # Drums.
        if bar == 3:
            s.kick(beat0)
            s.riser(beat0, 4.0, 0.55)
            s.tom_fill(beat0 + 2, 8)
        elif verse:
            drum_bar(s, bar, (0, 8, 10), (4, 12), range(0, 16, 2), [0.5, 0.75])
        elif chorus or ending:
            drum_bar(s, bar, (0, 4, 8, 12), (4, 12), range(16), [0.7, 0.35, 0.5, 0.35],
                     open_hats=(14,) if bar % 2 else (), clap=True)
        elif bridge:
            drum_bar(s, bar, (0, 10), (8,), range(0, 16, 4), [0.5])
            if bar == 23:
                s.snare_roll(beat0 + 2, 2.0)
                s.riser(beat0, 4.0, 0.5)

        if bar in (4, 12, 24):
            s.crash(beat0)

        # Melodic parts.
        if verse:
            play_line(s, s.fm_bell, bar, verse_bells[(bar - 4) % 4], vel=0.8, pan=0.2)
        if chorus:
            line = chorus_melody[(bar - (12 if bar < 20 else 24)) % 8]
            play_line(s, s.brass, bar, line, vel=1.0, bus="lead")
            if bar >= 24:
                play_line(s, s.brass, bar, [(m, b, d) for m, b, d in harmony_below(line, A_MINOR)],
                          vel=0.7, pan=-0.25)
                play_line(s, s.fm_bell, bar, shift_line(line, 12), vel=0.35, pan=0.3)
        if bridge:
            play_line(s, s.square_lead, bar, bridge_lead[bar - 20], vel=0.9)
            s.brass(beat0, notes[1], 3.5, vel=0.55, pan=-0.3, bright=0.6)
            s.brass(beat0, notes[2], 3.5, vel=0.55, pan=0.3, bright=0.6)
        if ending:
            play_line(s, s.brass, bar, chorus_melody[6 + bar - 32], vel=0.9, bus="lead")
    return s


# --------------------------------------------------------------------------
# Track 2: Pacific Coast Highway (D minor, 104 BPM)
# --------------------------------------------------------------------------


def pacific_coast_highway() -> Song:
    s = Song("Pacific Coast Highway", 104.0, 33, seed=1986)
    chords = {
        "Bbmaj7": ([n("Bb3"), n("D4"), n("F4"), n("A4")], n("Bb1")),
        "C": ([n("G3"), n("C4"), n("E4"), n("G4")], n("C2")),
        "Am7": ([n("G3"), n("A3"), n("C4"), n("E4")], n("A1")),
        "Dm": ([n("A3"), n("D4"), n("F4"), n("A4")], n("D2")),
        "F": ([n("A3"), n("C4"), n("F4"), n("A4")], n("F1")),
        "Gm": ([n("G3"), n("Bb3"), n("D4"), n("G4")], n("G1")),
        "Bb": ([n("Bb3"), n("D4"), n("F4"), n("Bb4")], n("Bb1")),
        "A": ([n("A3"), n("C#4"), n("E4"), n("A4")], n("A1")),
    }
    verse_prog = ["Bbmaj7", "C", "Am7", "Dm"]
    chorus_prog = ["F", "C", "Dm", "Bb", "Gm", "Bb", "C", "A"]
    progression = (
        verse_prog  # intro 0-3
        + verse_prog * 2  # verse 4-11
        + chorus_prog  # chorus 12-19
        + verse_prog  # breakdown 20-23
        + chorus_prog  # chorus 24-31
        + ["Dm"]  # final chord 32
    )

    verse_melody = [
        [("D5", 0, 1.5), ("F5", 1.5, 1), ("A5", 2.5, 1.5)],
        [("G5", 0, 1.5), ("E5", 1.5, 0.5), ("C5", 2, 2)],
        [("E5", 0, 1.5), ("G5", 1.5, 0.5), ("A5", 2, 1), ("C6", 3, 1)],
        [("A5", 0, 3), ("F5", 3, 0.5), ("G5", 3.5, 0.5)],
        [("A5", 0, 1.5), ("F5", 1.5, 0.5), ("D5", 2, 2)],
        [("E5", 0, 1), ("G5", 1, 1), ("C6", 2, 1), ("Bb5", 3, 1)],
        [("A5", 0, 1.5), ("G5", 1.5, 0.5), ("E5", 2, 2)],
        [("D5", 0, 4)],
    ]
    chorus_melody = [
        [("C6", 0, 1), ("A5", 1, 0.5), ("C6", 1.5, 1), ("D6", 2.5, 0.5), ("C6", 3, 1)],
        [("G5", 0, 1.5), ("E5", 1.5, 0.5), ("G5", 2, 1), ("C6", 3, 1)],
        [("A5", 0, 1.5), ("F5", 1.5, 0.5), ("D5", 2, 1), ("F5", 3, 0.5), ("G5", 3.5, 0.5)],
        [("F5", 0, 2), ("D5", 2, 1), ("F5", 3, 1)],
        [("G5", 0, 1.5), ("Bb5", 1.5, 0.5), ("D6", 2, 1), ("C6", 3, 0.5), ("Bb5", 3.5, 0.5)],
        [("A5", 0, 1.5), ("F5", 1.5, 0.5), ("D5", 2, 2)],
        [("E5", 0, 1), ("G5", 1, 1), ("C6", 2, 1), ("E6", 3, 1)],
        [("C#6", 0, 2), ("A5", 2, 1), ("E5", 3, 1)],
    ]
    moroder = [0, 0, 12, 0]
    arp_shape = [0, 2, 1, 3, 2, 0, 3, 1]
    comp_steps = [(0, 1.5), (6, 0.5), (10, 1.0), (14, 0.5)]

    for bar, name in enumerate(progression):
        notes, root = chords[name]
        beat0 = bar * 4
        if bar == 32:
            s.pad(beat0, notes + [n("E5")], 6.0, cutoff=2200.0, attack=0.04, release=1.6, gain=1.1)
            s.bass(beat0, root, 5.0, cutoff=240.0, env_amt=1200.0, decay=0.5)
            s.kick(beat0)
            s.crash(beat0, 0.9)
            for i, note in enumerate(["D5", "F5", "A5", "E6"]):
                s.fm_bell(beat0 + i * 0.75, n(note), 1.0, 0.6, pan=-0.3 + 0.2 * i)
            continue

        intro = bar < 4
        verse = 4 <= bar < 12
        chorus = 12 <= bar < 20 or 24 <= bar < 32
        breakdown = 20 <= bar < 24

        s.pad(beat0, notes, 4.0, cutoff=2300.0 if chorus else 1500.0, attack=0.45 if intro or breakdown else 0.2,
              gain=1.1 if breakdown else 1.0)

        # Moroder-style sixteenth bass; the breakdown sweeps its filter open.
        if not intro:
            sweep = (bar - 20 + 1) / 4.0 if breakdown else 1.0
            for step in range(16):
                midi = root + moroder[step % 4]
                s.bass(beat0 + step / 4, midi, 0.2, accent=1.0 if step % 4 == 0 else 0.6,
                       cutoff=180.0 + 220.0 * sweep, env_amt=600.0 + 1600.0 * sweep, decay=0.07, res=0.5)

        # Arpeggio with ping-pong echoes.
        if intro or chorus or breakdown:
            tones = [notes[0] + 12, notes[1] + 12, notes[2] + 12, notes[3] + 12]
            open_amt = min(1.0, (bar + 1) / 4.0) if intro else 1.0
            for step in range(16):
                s.pluck(beat0 + step / 4, tones[arp_shape[step % 8]], 0.2, vel=0.45 + 0.2 * (step % 4 == 0),
                        cutoff=500.0 + 700.0 * open_amt, env_amt=800.0 + 2000.0 * open_amt,
                        width=0.25, pan=0.4 if step % 2 else -0.4)

        # DX electric piano comping in the verses.
        if verse:
            for step, length in comp_steps:
                for note in notes[1:]:
                    s.epiano(beat0 + step / 4, note, length * 0.9, vel=0.7 if step == 0 else 0.55,
                             pan=0.25 if note % 2 else -0.25)

        # Drums.
        if bar == 3:
            s.riser(beat0, 4.0, 0.5)
            s.snare_roll(beat0 + 3, 1.0)
        elif verse:
            drum_bar(s, bar, (0, 8, 11) if bar % 2 else (0, 8), (4, 12), range(0, 16, 2), [0.55, 0.4])
        elif chorus:
            drum_bar(s, bar, (0, 6, 8, 11) if bar % 2 else (0, 8, 10), (4, 12), range(16),
                     [0.6, 0.3, 0.45, 0.3], open_hats=(14,) if bar % 4 == 3 else (), clap=True)
            if bar in (19, 31):
                s.tom_fill(beat0 + 2, 8, 230.0, 110.0)
        elif breakdown:
            s.kick(beat0)
            if bar >= 22:
                s.kick(beat0 + 2)
            if bar == 23:
                s.snare_roll(beat0, 4.0, 4)
                s.riser(beat0, 4.0, 0.6)

        if bar in (4, 12, 24):
            s.crash(beat0)

        # Melodies.
        if verse:
            play_line(s, s.fm_bell, bar, verse_melody[bar - 4], vel=0.85, pan=0.15, ratio=1.0, index=1.4, decay=1.1)
        if chorus:
            idx = (bar - (12 if bar < 20 else 24)) % 8
            play_line(s, s.square_lead, bar, chorus_melody[idx], vel=1.0)
            if bar >= 24:
                play_line(s, s.fm_bell, bar, chorus_melody[idx], vel=0.4, pan=-0.35, decay=0.8)
        if breakdown:
            for i, bell in enumerate(notes):
                s.fm_bell(beat0 + i * 0.75, bell + 12, 0.75, 0.45, pan=-0.45 + 0.3 * i)
    return s


# --------------------------------------------------------------------------
# Track 3: Neon Strip (E minor, 126 BPM)
# --------------------------------------------------------------------------


def neon_strip() -> Song:
    s = Song("Neon Strip", 126.0, 37, seed=1987)
    E_MINOR = {4, 6, 7, 9, 11, 0, 2}
    chords = {
        "Em": ([n("G3"), n("B3"), n("E4"), n("G4")], n("E1")),
        "C": ([n("G3"), n("C4"), n("E4"), n("G4")], n("C2")),
        "D": ([n("F#3"), n("A3"), n("D4"), n("F#4")], n("D2")),
        "B": ([n("F#3"), n("B3"), n("D#4"), n("F#4")], n("B1")),
        "Bm": ([n("F#3"), n("B3"), n("D4"), n("F#4")], n("B1")),
        "Am": ([n("A3"), n("C4"), n("E4"), n("A4")], n("A1")),
    }
    riff_prog = ["Em", "C", "D", "B"]
    chorus_prog = ["C", "D", "Bm", "Em", "C", "D", "B", "B"]
    progression = (
        riff_prog  # intro 0-3
        + riff_prog * 2  # riff 4-11
        + chorus_prog  # chorus 12-19
        + ["Am", "C", "D", "B"]  # break 20-23
        + chorus_prog  # chorus 24-31
        + riff_prog  # outro riff 32-35
        + ["Em"]  # final hit 36
    )

    riff = [
        [("B4", 0, 0.5), ("E5", 0.5, 0.5), ("G5", 1, 0.5), ("B5", 1.5, 1), ("A5", 2.5, 0.5), ("G5", 3, 0.5), ("F#5", 3.5, 0.5)],
        [("G5", 0, 1.5), ("E5", 1.5, 0.5), ("C5", 2, 1), ("E5", 3, 1)],
        [("F#5", 0, 0.5), ("A5", 0.5, 0.5), ("D6", 1, 1), ("C6", 2, 0.5), ("B5", 2.5, 0.5), ("A5", 3, 1)],
        [("B5", 0, 1.5), ("A5", 1.5, 0.5), ("F#5", 2, 1), ("D#5", 3, 1)],
    ]
    riff_turn = [("B5", 0, 2), ("F#5", 2, 0.5), ("A5", 2.5, 0.5), ("B5", 3, 1)]
    chorus_melody = [
        [("E5", 0, 1), ("G5", 1, 1), ("C6", 2, 1.5), ("B5", 3.5, 0.5)],
        [("A5", 0, 1.5), ("F#5", 1.5, 0.5), ("D5", 2, 2)],
        [("F#5", 0, 1), ("B5", 1, 1), ("D6", 2, 1.5), ("B5", 3.5, 0.5)],
        [("E6", 0, 2), ("D6", 2, 0.5), ("B5", 2.5, 0.5), ("G5", 3, 1)],
        [("E5", 0, 1), ("G5", 1, 1), ("C6", 2, 1.5), ("E6", 3.5, 0.5)],
        [("D6", 0, 1.5), ("C6", 1.5, 0.5), ("A5", 2, 1), ("F#5", 3, 1)],
        [("D#6", 0, 1.5), ("B5", 1.5, 0.5), ("F#5", 2, 2)],
        [("B5", 0, 2), ("A5", 2, 1), ("F#5", 3, 1)],
    ]
    bass_seq = [0, 0, 12, 0, 0, 12, 0, 10]
    arp_shape = [0, 1, 2, 3, 1, 2, 3, 2]

    for bar, name in enumerate(progression):
        notes, root = chords[name]
        beat0 = bar * 4
        if bar == 36:
            s.orch_hit(beat0, n("E3"), 1.0, chord=(0, 7, 12, 15, 19, 24))
            s.pad(beat0, notes + [n("B4")], 5.0, cutoff=2600.0, attack=0.03, release=1.5, gain=1.2)
            s.bass(beat0, root, 4.0, cutoff=260.0, env_amt=2000.0, decay=0.3)
            s.kick(beat0)
            s.snare(beat0)
            s.crash(beat0, 1.0)
            s.sync_lead(beat0, n("E6"), 3.0, vel=0.8, prev=n("B5"))
            continue

        intro = bar < 4
        riff_sec = 4 <= bar < 12 or 32 <= bar < 36
        chorus = 12 <= bar < 20 or 24 <= bar < 32
        brk = 20 <= bar < 24

        # Driving sixteenth bass; the intro opens it up bar by bar.
        opening = (bar + 1) / 4.0 if intro else 1.0
        for step in range(16):
            midi = root + bass_seq[step % 8]
            s.bass(beat0 + step / 4, midi, 0.2, accent=1.0 if step % 4 == 0 else 0.55,
                   cutoff=160.0 + 260.0 * opening, env_amt=500.0 + 2600.0 * opening, decay=0.06, res=0.55)

        if not intro:
            s.pad(beat0, notes, 4.0, cutoff=2800.0 if chorus else 1800.0, attack=0.1,
                  gain=0.8 if riff_sec else 1.0)

        # Drums.
        if intro:
            for step in range(16):
                s.hat(beat0 + step / 4, 0.25 + 0.3 * opening * (step % 2 == 0))
            if bar == 3:
                s.snare_roll(beat0, 4.0, 4)
                s.riser(beat0, 4.0, 0.6)
        elif riff_sec or chorus:
            drum_bar(s, bar, (0, 4, 8, 12), (4, 12), range(16), [0.65, 0.3, 0.5, 0.3],
                     open_hats=(2, 6, 10, 14) if chorus else (), clap=chorus)
            if bar in (11, 19, 31):
                s.tom_fill(beat0 + 2, 8, 240.0, 100.0)
        elif brk:
            s.kick(beat0)
            s.kick(beat0 + 2.5)
            drum_bar(s, bar, (), (12,), range(0, 16, 2), [0.4])
            if bar == 23:
                s.snare_roll(beat0, 4.0, 4)
                s.riser(beat0, 4.0, 0.6)

        if bar in (4, 12, 24, 32):
            s.crash(beat0)

        # Fairlight stabs and brass punctuation in the riff.
        if riff_sec:
            s.orch_hit(beat0, root + 24, 0.85, pan=-0.1)
            if bar % 2 == 1:
                s.orch_hit(beat0 + 1.5, root + 24, 0.6, pan=0.15)
            for step in (6, 14):
                for note in notes[1:]:
                    s.brass(beat0 + step / 4, note + 12, 0.35, vel=0.55, pan=0.3 if note % 2 else -0.3)
            idx = (bar - 4) % 4 if bar < 12 else bar - 32
            line = riff_turn if bar in (11, 35) else riff[idx]
            play_line(s, s.sync_lead, bar, line, vel=0.95)

        if chorus or brk:
            tones = [notes[1] + 12, notes[2] + 12, notes[3] + 12, notes[1] + 24]
            for step in range(16):
                s.pluck(beat0 + step / 4, tones[arp_shape[step % 8]], 0.2, vel=0.5, width=0.2,
                        cutoff=900.0, env_amt=2600.0, pan=0.45 if step % 2 else -0.45)

        if chorus:
            idx = (bar - (12 if bar < 20 else 24)) % 8
            play_line(s, s.sync_lead, bar, chorus_melody[idx], vel=1.0)
            if bar >= 24:
                play_line(s, s.brass, bar, shift_line(chorus_melody[idx], -12), vel=0.75, pan=-0.2)
                play_line(s, s.brass, bar, harmony_below(chorus_melody[idx], E_MINOR), vel=0.55, pan=0.25)
            if bar in (12, 16, 24, 28):
                s.orch_hit(beat0, root + 24, 0.8)
        if brk:
            s.brass(beat0, notes[1] + 12, 3.8, vel=0.6, pan=-0.3, bright=0.7)
            s.brass(beat0, notes[2] + 12, 3.8, vel=0.6, pan=0.3, bright=0.7)
            s.fm_bell(beat0 + 1.5, notes[3] + 12, 1.0, 0.5, pan=0.3)
    return s


TRACKS = [
    ("01-blue-hour-boulevard", blue_hour_boulevard),
    ("02-pacific-coast-highway", pacific_coast_highway),
    ("03-neon-strip", neon_strip),
]


def write_wav(path: Path, audio: np.ndarray) -> None:
    rng = np.random.default_rng(0)
    dither = (rng.random(audio.shape) - rng.random(audio.shape)) / 32768.0
    pcm = np.clip(np.round((audio + dither) * 32767.0), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(OUT_SR)
        stream.writeframes(pcm.T.copy().tobytes())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    manifest = []
    for stem, compose in TRACKS:
        song = compose()
        audio = master_to_output(song.render())
        path = args.output_dir / f"{stem}.wav"
        write_wav(path, audio)
        manifest.append({"title": song.title, "file": path.name})
        rms = float(np.sqrt(np.mean(audio**2)))
        print(f"{song.title}: {audio.shape[1] / OUT_SR:.1f}s, {song.bpm:g} BPM, "
              f"RMS {rms:.3f}, peak {np.abs(audio).max():.3f} -> {path}")
    (args.output_dir / "tracks.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
