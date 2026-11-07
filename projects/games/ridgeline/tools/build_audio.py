#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "scipy>=1.11"]
# ///
"""Synthesize Ridgeline's sound effects.

Everything is generated from code with seeded RNGs, so the output is
original and byte-for-byte reproducible. Outputs:

    assets/generated/sfx.pak         16-bit mono PCM, 32-byte aligned entries
    assets/generated/sound_assets.h  effect table and engine sound sets
    captures/audio-previews/*.wav    listening copies (git-ignored), including
                                     an rpm sweep per truck mixed the way the
                                     game mixes the engine loops

Engines are built from a per-cylinder model: each firing event drops an
exhaust blowdown pulse (shaped in crank degrees, so it shortens as rpm
rises), a valve-opening crack, turbulent flow noise, an intake event and a
block knock at its crank angle. The pulses go through exhaust, intake and
block responses applied in the frequency domain. Engine loops hold a whole
number of four-stroke cycles and every component repeats exactly over the
loop, so the loops wrap seamlessly; everything tonal is locked to crank
orders, so two loops played at rpm/loop_rpm line up harmonic for harmonic.
"""

from __future__ import annotations

import argparse
import sys
import wave
from dataclasses import dataclass
from functools import lru_cache, partial
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "common" / "engine" / "tools"))
import synthkit  # noqa: E402
from synthkit import (  # noqa: E402
    LOOP_PEAK, MAX_SFX_SAMPLES, ONESHOT_PEAK, SFX_RATE, TAU, fade, filt, finish_loop,
    finish_oneshot, make_ir, midi_hz, pad_to, periodic, place, place_wrap, plfo, softsat,
    to_pcm16)

SR = SFX_RATE
MAX_PAK_BYTES = 1_400_000        # leaves the AICA ~500 KiB free
ENGINE_LOOPS = 4

ROOT = Path(__file__).resolve().parent.parent

SEED = 1984
rng_for = partial(synthkit.rng_for, salt=SEED)
add_reverb = partial(synthkit.add_reverb, salt=SEED)


# ======================================================================
# DSP helpers
# ======================================================================

def pnoise(n, rng, lo=None, hi=None, sr=SR):
    """Periodic white noise band-limited by brick-wall FFT edges."""
    X = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / sr)
    if lo:
        X[f < lo] = 0
    if hi:
        X[f > hi] = 0
    x = np.fft.irfft(X, n=n)
    return x / (np.std(x) + 1e-12)


def fshape(x, H_fn, sr=SR):
    """Circular filtering by an analytic frequency response H_fn(f)."""
    n = len(x)
    f = np.fft.rfftfreq(n, 1 / sr)
    return np.fft.irfft(np.fft.rfft(x) * H_fn(f), n=n)


def reverb_circ(x, wet, seconds, t60, seed, sr, lp_hz=6000.0, hp_hz=180.0):
    """Reverb for a loop: the tail wraps round, so it stays seamless."""
    n = len(x)
    ir = make_ir(sr, seconds, t60, rng_for(seed), lp_hz=lp_hz, hp_hz=hp_hz)
    folded = np.zeros(n)
    for i in range(0, len(ir), n):
        folded[:len(ir[i:i + n])] += ir[i:i + n]
    return x + wet * np.fft.irfft(np.fft.rfft(x) * np.fft.rfft(folded), n=n)


def tame(x, pct=99.9, drive=1.3):
    """Even out sparse peaks (grains, clicks) without touching the body."""
    return softsat(x / (np.percentile(np.abs(x), pct) + 1e-12), drive)


# Analytic responses (complex, numpy rfft convention: a delay is e^-jwt).

def H_lp1(f, fc):
    return 1 / (1 + 1j * f / fc)


def H_hp1(f, fc):
    s = 1j * f / fc
    return s / (1 + s)


def H_lp2(f, fc, q=0.7071):
    s = 1j * f / fc
    return 1 / (s * s + s / q + 1)


def H_hp2(f, fc, q=0.7071):
    s = 1j * f / fc
    return s * s / (s * s + s / q + 1)


def H_bp2(f, fc, q):
    s = 1j * f / fc
    return (s / q) / (s * s + s / q + 1)


def H_peak(f, fc, q, gain_db):
    A = 10 ** (gain_db / 40)
    s = 1j * f / fc
    return (s * s + s * A / q + 1) / (s * s + s / (A * q) + 1)


C_HOT = 520.0     # speed of sound in hot exhaust gas, m/s
C_AIR = 343.0


def H_pipe(f, length, r_in, r_out, c=C_HOT, loss_hz=6000.0):
    """Pressure transfer along a pipe with partial reflections at both ends
    (r_in at the source end, r_out at the far end)."""
    z = np.exp(-1j * TAU * f * length / c) * np.exp(-f * length / (loss_hz * 2.0))
    return (1 + r_out) * z / (1 - r_in * r_out * z * z)


def H_chamber(f, length, m, c=C_HOT):
    """Expansion-chamber silencer (area ratio m), anechoic outlet."""
    k = TAU * f / c
    return 1 / (np.cos(k * length) + 0.5j * (m + 1 / m) * np.sin(k * length))


def H_delay(f, secs):
    return np.exp(-1j * TAU * f * secs)


# ======================================================================
# Engine model
# ======================================================================

@dataclass(eq=False)
class Engine:
    key: str
    label: str
    idle: float
    redline: float
    points: tuple              # nominal loop rpm, ascending
    durs: tuple                # target loop length (s) per point
    fire: tuple                # (TDC crank angle in the 720 deg cycle, bank) in firing order
    cyl_amp: tuple             # per-cylinder strength (mixture distribution)
    cyl_dth: tuple             # per-cylinder timing scatter, crank degrees
    bank_dth: tuple            # per-bank timing offset (uneven exhaust paths)
    bank_gain: tuple
    evo: float                 # exhaust valve opening, deg after TDC
    blow_peak: float           # blowdown decay, crank degrees (on load)
    blow_rise: float           # blowdown front, crank degrees (on load)
    crack: float               # valve-opening crack strength
    crack_tau: float           # its rise time, s
    flow: float                # turbulent flow noise in the pulse
    flow_band: tuple
    pump: float                # pulse strength with no combustion
    var_on: float              # cycle-to-cycle scatter
    var_off: float
    misfire_off: float         # chance a cylinder misses on overrun
    off_fire: float            # combustion strength on overrun (0 = fuel cut)
    pops_per_s: float          # overrun pops in the exhaust
    pop_amp: float
    shaper: float              # exhaust gas-dynamic steepening (0 = linear)
    exhaust_bank: object       # f -> H per bank, list of callables
    exhaust_common: object
    intake_H: object
    intake_gain: float
    roar_gain: float
    roar_band: tuple
    hiss_gain: float           # closed-throttle hiss
    hiss_hz: float
    knock_modes: tuple         # block modes (Hz, decay s, gain)
    knock_gain: float
    valve_events: tuple        # crank angles of valve seatings per cycle
    valve_modes: tuple
    valve_gain: float
    chain_order: float
    chain_gain: float
    di_gain: float = 0.0       # direct-injection tick (modern engines)
    pump_events: tuple = ()
    turbo_order: float = 0.0   # whistle frequency in crank orders
    turbo_gain: float = 0.0
    turbo_hiss: float = 0.0
    starter_ratio: float = 12.0
    crank_rpm: float = 210.0
    crank_secs: float = 0.8
    flare_rpm: float = 1400.0
    on_db: tuple = (-1.5, -0.5, 0.5, 1.5)    # loop loudness targets (RMS, dB)
    off_db: tuple = (-3.0, -4.3, -5.3, -6.3)


def blowdown(th, rise, decay):
    """Exhaust pulse at the valve: a steep front as the valve cracks open
    on high cylinder pressure, then an exponential let-down. Both scale in
    crank degrees, so the front sharpens with rpm."""
    x = np.maximum(th, 0.0)
    g = (1 - np.exp(-x / rise)) ** 2 * np.exp(-x / decay)
    xm = rise * np.log(1 + 2 * decay / rise)
    gm = (1 - np.exp(-xm / rise)) ** 2 * np.exp(-xm / decay)
    return np.where(th > 0, g / gm, 0.0)


def boost_of(e, rpm, load):
    return load * np.clip((rpm - 1500.0) / (0.55 * e.redline), 0.0, 1.0) ** 1.2


def render_engine(e: Engine, rpm, load, fire, seed, circular, tail=0, cycles=None,
                  misfire=None, starter=None):
    """Render the engine for per-sample rpm/load/fire tracks.

    circular=True renders one period of a loop (rpm constant, `cycles` whole
    cycles) with every event wrapping, so the output repeats exactly.
    Otherwise the output has `tail` extra samples for the filters to ring.
    Returns (mix, parts) where parts holds the stems for analysis."""
    rng = rng_for(seed)
    n = len(rpm)
    m = n + tail
    if circular:
        theta = np.arange(n + 1) * (720.0 * cycles / n)
    else:
        theta = np.concatenate([[0.0], np.cumsum(rpm * 6.0 / SR)])
    nbank = len(e.bank_gain)
    port = np.zeros((nbank, m))
    flow = np.zeros((nbank, m))
    pops = np.zeros(m)
    intake = np.zeros(m)
    roar = np.zeros(m)
    mech = np.zeros(m)
    idx = np.arange(n + 1, dtype=np.float64)
    ncyc = cycles if circular else int(np.ceil(theta[-1] / 720.0)) + 1

    def at(th_e):
        """Fractional sample time of crank angle th_e (None if outside)."""
        if circular:
            return (th_e % theta[-1]) * n / theta[-1]
        if th_e < 0 or th_e > theta[-1]:
            return None
        return float(np.interp(th_e, theta, idx))

    def window(t0, R, span_deg, extra_s=0.0):
        w = int((span_deg / (6.0 * R) + extra_s) * SR) + 3
        if circular:
            w = min(w, n - 1)
        k0 = int(np.floor(t0))
        ks = k0 + np.arange(w)
        return ks, (ks - t0) / SR

    def add(buf, ks, vals):
        if circular:
            np.add.at(buf, ks % m, vals)
        else:
            ok = ks < m
            buf[ks[ok]] += vals[ok]

    # Each cylinder rings the block much the same way every time.
    mode_ph = rng_for(seed + "-modes").uniform(0, TAU, (len(e.fire) + 1, 8))

    def modes_burst(t, modes, phase_rng, who=-1):
        y = np.zeros(len(t))
        tt = np.maximum(t, 0)
        for q, (f0, tau, g) in enumerate(modes):
            ph = mode_ph[who, q] + phase_rng.normal(0, 0.4)
            y += g * np.sin(TAU * f0 * tt + ph) * np.exp(-tt / tau)
        return y * (1 - np.exp(-tt / 0.00015)) * (t >= 0)

    for c in range(ncyc):
        for j, (tdc, bank) in enumerate(e.fire):
            th0 = c * 720.0 + tdc + e.cyl_dth[j] + e.bank_dth[bank]
            t_tdc = at(th0)
            if t_tdc is None:
                continue
            k = min(int(t_tdc), n - 1)
            R, L, F = float(rpm[k]), float(load[k]), float(fire[k])
            # Combustion strength for this cylinder this cycle.
            var = e.var_on * L + e.var_off * (1 - L)
            comb = F * (0.28 + 0.72 * L) * e.cyl_amp[j] * max(0.0, 1 + var * rng.standard_normal())
            p_mis = e.misfire_off * (1 - L) * F if misfire is None else float(misfire[k])
            if rng.random() < p_mis:
                comb *= 0.08
            # Pressure at the valve and the gas velocity both climb with rpm.
            A = (e.pump * (0.6 + 0.4 * (1 - L)) + comb) * e.bank_gain[bank] * (R / 3000.0) ** 0.25
            vel = R / 3000.0
            # --- exhaust blowdown at EVO ---
            t_evo = at(th0 + e.evo)
            if t_evo is not None:
                ks, ts = window(t_evo, R, 330)
                th = ts * 6.0 * R
                g = blowdown(th, e.blow_rise * (1.6 - 0.6 * L), e.blow_peak * (1.2 - 0.2 * L))
                push = 0.22 * np.exp(-((th - 175.0) / 45.0) ** 2)
                scav = 0.16 * np.exp(-((th - 265.0) / 35.0) ** 2)
                tc = np.maximum(ts, 0) / e.crack_tau
                crack = np.where(ts > 0, tc * tc * np.exp(2 * (1 - tc)), 0.0)
                crack_amp = e.crack * comb ** 1.3 + 0.15 * e.crack * e.pump
                add(port[bank], ks, A * (g + push - scav) + crack_amp * crack)
                nz = rng.standard_normal(len(ks))
                add(flow[bank], ks, nz * vel * (A ** 1.5) * (g ** 0.8 + 0.2 * push))
            # --- intake: suction through the stroke, then the IVC slam ---
            t_in = at(th0 - 360.0)
            if t_in is not None:
                ks, ts = window(t_in, R, 260)
                th = ts * 6.0 * R
                suction = -np.exp(-((th - 95.0) / 40.0) ** 2)
                slam = 0.7 * np.exp(-((th - 225.0) / 14.0) ** 2)
                thr = 0.25 + 0.75 * L
                add(intake, ks, (suction * thr + slam * (0.3 + 0.7 * L)) * e.cyl_amp[j])
                add(roar, ks, rng.standard_normal(len(ks)) * np.exp(-((th - 110.0) / 55.0) ** 2) * thr ** 1.5)
            # --- combustion knock through the block ---
            t_k = at(th0 + 8.0)
            if t_k is not None and comb > 0:
                ks, ts = window(t_k, R, 0, 0.012)
                kg = e.knock_gain * comb * (R / 3000.0) ** 0.4
                add(mech, ks, kg * modes_burst(ts, e.knock_modes, rng, j))
            # --- direct-injection tick during the intake stroke ---
            if e.di_gain and F > 0.05:
                t_di = at(th0 - 300.0)
                if t_di is not None:
                    ks, ts = window(t_di, R, 0, 0.004)
                    add(mech, ks, e.di_gain * F * modes_burst(ts, ((6100, 0.0005, 1.0), (7700, 0.0004, 0.7),
                                                                   (4300, 0.0007, 0.4)), rng))
            # --- overrun pops (afterfire in the pipe) ---
            if e.pops_per_s > 0 and L < 0.5:
                rate = e.pops_per_s * (1 - 2 * L) * np.clip((R - e.idle * 1.3) / 800.0, 0, 1)
                if rng.random() < rate * (120.0 / R) / len(e.fire):
                    t_pop = at(th0 + e.evo + rng.uniform(20, 70))
                    if t_pop is not None:
                        ks, ts = window(t_pop, R, 0, 0.012)
                        tt = np.maximum(ts, 0)
                        burst = rng.standard_normal(len(ks)) * np.exp(-tt / 0.0009)
                        thump = np.sin(TAU * 140 * tt) * np.exp(-tt / 0.004)
                        amp = e.pop_amp * rng.uniform(0.4, 1.0)
                        add(pops, ks, amp * (burst + 1.5 * thump) * (ts >= 0))
        # --- valve seatings (cam events, per cycle) ---
        for va in e.valve_events:
            t_v = at(c * 720.0 + va + rng.normal(0, 0.6))
            if t_v is None:
                continue
            R = float(rpm[min(int(t_v), n - 1)])
            ks, ts = window(t_v, R, 0, 0.006)
            vg = e.valve_gain * (0.55 + 0.45 * min(R / 3000.0, 1.6)) * rng.uniform(0.75, 1.2)
            add(mech, ks, vg * modes_burst(ts, e.valve_modes, rng))
        for pa in e.pump_events:
            t_p = at(c * 720.0 + pa)
            if t_p is None:
                continue
            R = float(rpm[min(int(t_p), n - 1)])
            ks, ts = window(t_p, R, 0, 0.005)
            add(mech, ks, 0.6 * e.di_gain * modes_burst(ts, ((3900, 0.0012, 1.0), (5300, 0.0007, 0.5)), rng))

    f = np.fft.rfftfreq(m, 1 / SR)

    def spec(x):
        return np.fft.rfft(x)

    def back(X):
        return np.fft.irfft(X, n=m)

    # Exhaust: banks -> merge -> common system.
    lo, hi = e.flow_band
    flow_H = H_hp2(f, lo, 0.6) * H_lp2(f, hi, 0.6)
    ex = np.zeros(len(f), dtype=complex)
    for b in range(nbank):
        p = back(spec(port[b]) + spec(flow[b]) * flow_H * e.flow)
        if e.shaper:
            # Large pulses steepen as they travel (finite-amplitude waves):
            # an asymmetric curve adds the raspy upper harmonics.
            s = e.shaper
            p = p + s * np.maximum(p, 0) ** 2 - 0.25 * s * np.minimum(p, 0) ** 2
            p -= np.mean(p)
        ex += spec(p) * e.exhaust_bank[b](f)
    ex += spec(pops) * H_bp2(f, 900, 0.5) * 0.8
    ex *= e.exhaust_common(f)
    # Intake (pulses and roar) and the block.
    it = spec(intake) * e.intake_H(f) * e.intake_gain
    rlo, rhi = e.roar_band
    it += spec(roar) * H_hp2(f, rlo, 0.7) * H_lp2(f, rhi, 0.7) * e.roar_gain
    me = spec(mech)
    out_exh, out_int, out_mech = back(ex), back(it), back(me)

    # Order-locked tones and steady noise.
    th = theta[:n]
    rad = th * np.pi / 180.0
    tones = np.zeros(m)
    R = rpm
    L = load
    lfo_rng = rng_for(seed + "-lfo")
    if circular:
        wob1, wob2, wob3 = (plfo(n, 3.0, lfo_rng) for _ in range(3))
    else:
        wob1, wob2, wob3 = (pad_to(plfo(n + (n & 1), 3.0, lfo_rng), n) for _ in range(3))
    # Timing chain / cam drive whine: rises with rpm, scruffier off load.
    chain = (np.sin(e.chain_order * rad + 0.6 * wob1) + 0.35 * np.sin(2 * e.chain_order * rad + 1.0))
    tones[:n] += e.chain_gain * (R / 3000.0) ** 1.5 * (1.0 + 0.3 * (1 - L)) * chain
    if e.turbo_order:
        b = boost_of(e, R, L)
        spin = (R / e.redline) ** 2
        amp = e.turbo_gain * (b + 0.12 * spin)
        whistle = (np.sin(e.turbo_order * rad + 1.4 * wob2) +
                   0.8 * np.sin(e.turbo_order * 1.004 * rad + 1.7 * wob3 + 2.0))
        flutter = 1 + 0.12 * np.sin(1.5 * rad)          # bank pulses hitting each turbine
        tones[:n] += amp * whistle * flutter
        if circular:
            hiss = fshape(pnoise(n, rng_for(seed + "-hiss")), lambda ff: H_hp2(ff, 2600, 0.7) * H_lp2(ff, 6500, 0.7))
        else:
            hiss = filt(filt(rng_for(seed + "-hiss").standard_normal(n), "hp", 2600), "lp", 7500)
            hiss /= np.std(hiss)
        tones[:n] += e.turbo_hiss * (b + 0.08 * spin) * hiss
    # Closed-throttle hiss past the butterfly (steady, slightly pulsing).
    if circular:
        hs = fshape(rng_for(seed + "-thr").standard_normal(n), lambda ff: H_bp2(ff, e.hiss_hz, 2.5))
    else:
        hs = filt(rng_for(seed + "-thr").standard_normal(n), "bp", e.hiss_hz, 2.5)
    hs /= np.std(hs) + 1e-12
    tones[:n] += e.hiss_gain * (1 - L) * (R / 3000.0) ** 0.7 * hs * (1 + 0.3 * np.sin(len(e.fire) / 2 * rad))
    if starter is not None:
        tones[:len(starter)] += starter[:m]

    mix = out_exh + out_int + out_mech + tones
    # Listener: a few metres behind the truck.
    mix = back(spec(mix) * H_hp2(f, 32, 0.7) * H_lp2(f, 9000, 0.7) * H_lp1(f, 14000))
    parts = dict(exhaust=out_exh, intake=out_int, mech=out_mech, tones=tones)
    return mix, parts


# --- the two trucks ---

def cascade_bank(f):
    # Cast 4-into-1 manifold feeding the head pipe.
    return H_pipe(f, 0.55, 0.55, 0.55) * H_lp1(f, 6000)


def cascade_common(f):
    head = H_pipe(f, 1.9, 0.35, -0.55, loss_hz=5000)
    muffler = H_chamber(f, 0.42, 6.5) * H_lp1(f, 2400) * H_peak(f, 1250, 1.4, 3.0)
    tail = H_pipe(f, 0.7, 0.45, -0.75, loss_hz=7000)
    return 2.2 * head * muffler * tail * H_hp1(f, 95) * H_peak(f, 140, 1.2, 3.0)


def cascade_intake(f):
    # Round air cleaner on the carburettor: box Helmholtz plus snorkel.
    return H_bp2(f, 165, 2.2) + 0.5 * H_bp2(f, 470, 3.0) + 0.15 * H_bp2(f, 1150, 2.0)


def kestrel_bank_a(f):
    # Short log manifold into the bank's turbine, which soaks up the pulse edges.
    return H_pipe(f, 0.33, 0.6, 0.5) * H_lp1(f, 1500) * H_lp1(f, 4000)


def kestrel_bank_b(f):
    return 0.92 * H_pipe(f, 0.58, 0.6, 0.5) * H_lp1(f, 1400) * H_lp1(f, 3800) * H_delay(f, 0.0004)


def kestrel_common(f):
    down = H_pipe(f, 2.3, 0.3, -0.5, loss_hz=5000)
    resonator = H_chamber(f, 0.3, 4.0)
    muffler = H_chamber(f, 0.52, 9.0) * H_lp1(f, 1300)
    tails = 0.5 * (H_pipe(f, 0.45, 0.4, -0.75) + H_pipe(f, 0.6, 0.4, -0.75))
    return 3.4 * down * resonator * muffler * tails * H_hp1(f, 70) * H_peak(f, 95, 1.0, 4.0)


def kestrel_intake(f):
    # Through the compressors and intercooler: only the low lumps survive.
    return H_bp2(f, 120, 1.5) * 1.0 + 0.2 * H_bp2(f, 380, 2.0)


CASCADE = Engine(
    key="CASCADE", label="CASCADE 84", idle=800, redline=5000,
    points=(800, 1450, 2650, 4800), durs=(0.6, 0.5, 0.42, 0.40),
    fire=((0, 0), (180, 0), (360, 0), (540, 0)),           # 1-3-4-2
    cyl_amp=(1.0, 0.86, 1.12, 0.93), cyl_dth=(0.0, 2.4, -1.8, 1.0),
    bank_dth=(0.0,), bank_gain=(1.0,),
    evo=128.0, blow_peak=38.0, blow_rise=3.5, crack=0.55, crack_tau=0.00022,
    flow=0.15, flow_band=(700, 4800), pump=0.15,
    var_on=0.06, var_off=0.32, misfire_off=0.10, off_fire=0.38,
    pops_per_s=7.0, pop_amp=0.9, shaper=0.35,
    exhaust_bank=(cascade_bank,), exhaust_common=cascade_common,
    intake_H=cascade_intake, intake_gain=0.2, roar_gain=0.05, roar_band=(350, 1700),
    hiss_gain=0.006, hiss_hz=2600,
    knock_modes=((1120, 0.0042, 1.0), (1790, 0.0031, 0.7), (2640, 0.0022, 0.5), (3480, 0.0015, 0.3)),
    knock_gain=0.10,
    # SOHC, 8 valves: each seating as the cam lets go.
    valve_events=(20, 200, 380, 560, 225, 405, 585, 45),
    valve_modes=((3200, 0.0011, 1.0), (4700, 0.0008, 0.7), (6300, 0.0005, 0.4)),
    valve_gain=0.030, chain_order=19.0, chain_gain=0.004,
    starter_ratio=13.0, crank_rpm=205.0, crank_secs=0.95, flare_rpm=1450.0,
)

KESTREL = Engine(
    key="KESTREL", label="KESTREL", idle=700, redline=5800,
    points=(700, 1390, 2760, 5450), durs=(0.55, 0.5, 0.42, 0.40),
    # 1-4-2-5-3-6: banks alternate every 120 degrees.
    fire=((0, 0), (120, 1), (240, 0), (360, 1), (480, 0), (600, 1)),
    cyl_amp=(1.0, 0.97, 1.03, 0.99, 1.02, 0.96), cyl_dth=(0.0, 0.5, -0.4, 0.3, 0.2, -0.5),
    bank_dth=(0.0, 7.0), bank_gain=(1.0, 0.86),
    evo=135.0, blow_peak=45.0, blow_rise=5.0, crack=0.30, crack_tau=0.0003,
    flow=0.08, flow_band=(500, 3500), pump=0.24,
    # Fuel cut on overrun, so no afterfire: a pop baked into these short
    # loops came round every pass and sounded like a bubble bursting.
    var_on=0.03, var_off=0.05, misfire_off=0.0, off_fire=0.0,
    pops_per_s=0.0, pop_amp=0.0, shaper=0.12,
    exhaust_bank=(kestrel_bank_a, kestrel_bank_b), exhaust_common=kestrel_common,
    intake_H=kestrel_intake, intake_gain=0.15, roar_gain=0.03, roar_band=(250, 1200),
    hiss_gain=0.005, hiss_hz=2200,
    knock_modes=((1380, 0.0028, 0.8), (2150, 0.0022, 0.6), (3050, 0.0016, 0.35)),
    knock_gain=0.03,
    valve_events=(),          # DOHC roller followers: quiet
    valve_modes=((4200, 0.0007, 1.0),), valve_gain=0.0,
    chain_order=24.0, chain_gain=0.0025,
    di_gain=0.008, pump_events=(0.0, 240.0, 480.0),
    turbo_order=50.0, turbo_gain=0.02, turbo_hiss=0.008,
    starter_ratio=11.0, crank_rpm=230.0, crank_secs=0.5, flare_rpm=1250.0,
    on_db=(-1.5, -0.6, 0.4, 1.4), off_db=(-2.8, -4.2, -5.2, -6.2),
)

ENGINES = (CASCADE, KESTREL)


def loop_geometry(e: Engine, i: int):
    """Whole cycles in a 16-sample-multiple loop; the loop rpm is adjusted
    so both hold exactly."""
    nominal = e.points[i]
    cycles = max(1, round(e.durs[i] * nominal / 120.0))
    n = 16 * round(cycles * 120.0 * SR / nominal / 16)
    return cycles, n, cycles * 120.0 * SR / n


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(np.asarray(x) ** 2)) + 1e-12)


def cycle_spectrum(x, cycles, rpm, fmax):
    """Complex amplitudes of the cycle-locked harmonics (order h/2 of the
    crank) below fmax Hz."""
    X = np.fft.rfft(x)
    hmax = int(fmax * 120.0 / rpm)
    h = np.arange(1, hmax + 1)
    return X[h * cycles] / len(x)


def align_to(refs, x, cycles):
    """Rotate loop x so that, played at the same rpm as each reference loop,
    its cycle-locked harmonics line up in phase with them. Crossfading
    then adds instead of partly cancelling; rotation keeps the loop
    seamless. refs: (loop, its cycles, rpm the two meet at, weight)."""
    grid = 4096
    total = np.zeros(grid)
    for ref, ref_cycles, rpm_play, weight in refs:
        hmax = int(1500.0 * 120.0 / rpm_play)        # the harmonics carrying the level
        h = np.arange(1, hmax + 1)
        A = np.fft.rfft(ref)[h * ref_cycles]
        B = np.fft.rfft(x)[h * cycles]
        spec = np.zeros(grid // 2 + 1, dtype=complex)
        spec[1:hmax + 1] = A * np.conj(B)
        corr = np.fft.irfft(spec, n=grid)
        total += weight * corr / (np.max(np.abs(corr)) + 1e-30)
    tau = np.argmax(total) / grid                    # engine cycles to delay x by
    return np.roll(x, int(round(tau * len(x) / cycles)))


@lru_cache(maxsize=None)
def engine_set(e: Engine):
    """All 8 loops for one truck, levelled to the loudness targets and
    scaled together so the loudest peak sits at the loop ceiling."""
    on, off, rpms = [], [], []
    for i in range(ENGINE_LOOPS):
        cycles, n, rpm = loop_geometry(e, i)
        rpms.append(rpm)
        for want, store in ((True, on), (False, off)):
            if want:
                L, F = 1.0, 1.0
            elif i == 0:
                L, F = 0.35, 1.0          # closed-throttle idle still burns fuel
            else:
                L, F = 0.0, e.off_fire    # overrun
            y, _ = render_engine(e, np.full(n, rpm), np.full(n, L), np.full(n, F),
                                 f"{e.key}-{'on' if want else 'off'}-{i}", True, cycles=cycles)
            y -= np.mean(y)
            # Gentle limiting: keeps the crest factor sane without dulling.
            y = softsat(y / np.percentile(np.abs(y), 99.95), 1.15)
            store.append(y)
    # Line every loop up with its neighbour at the rpm they crossfade at,
    # and each overrun loop with its on-load partner.
    geo = [loop_geometry(e, i) for i in range(ENGINE_LOOPS)]
    for i in range(1, ENGINE_LOOPS):
        mid = 0.5 * (rpms[i - 1] + rpms[i])
        on[i] = align_to([(on[i - 1], geo[i - 1][0], mid, 1.0)], on[i], geo[i][0])
    off[0] = align_to([(on[0], geo[0][0], rpms[0], 1.0)], off[0], geo[0][0])
    for i in range(1, ENGINE_LOOPS):
        mid = 0.5 * (rpms[i - 1] + rpms[i])
        off[i] = align_to([(on[i], geo[i][0], rpms[i], 1.0), (off[i - 1], geo[i - 1][0], mid, 1.0)],
                          off[i], geo[i][0])
    for loops, targets in ((on, e.on_db), (off, e.off_db)):
        for i in range(ENGINE_LOOPS):
            loops[i] = loops[i] * 10 ** ((targets[i] - rms_db(loops[i])) / 20)
    peak = max(np.max(np.abs(x)) for x in on + off)
    g = LOOP_PEAK / peak
    return [x * g for x in on], [x * g for x in off], rpms


# ======================================================================
# Engine start (starter + catch + settle), one-shot per truck
# ======================================================================

def engine_start(e: Engine):
    rng = rng_for(e.key + "-start")
    t_sol = 0.07                       # solenoid clack, then the starter bites
    t_catch = t_sol + 0.06 + e.crank_secs
    total = t_catch + 1.25
    n = int(total * SR)
    t = np.arange(n) / SR
    rpm = np.zeros(n)
    theta = 0.0
    r = 0.0
    ncomp = len(e.fire) / 2.0          # compressions per crank rev
    caught = t >= t_catch
    for k in range(n):
        tk = t[k]
        if tk < t_sol:
            target, tau = 0.0, 0.02
        elif tk < t_catch:
            # Cranking: the starter labours on each compression stroke.
            ripple = 1 - 0.32 * np.cos(ncomp * theta * np.pi / 180.0 - 0.6)
            target, tau = e.crank_rpm * ripple, 0.025
        else:
            x = tk - t_catch
            if x < 0.2:
                target, tau = e.flare_rpm, 0.07
            else:
                target = e.idle * 1.06 + (e.flare_rpm - e.idle) * 0.25 * np.exp(-(x - 0.2) / 0.25)
                tau = 0.18
        r += (target - r) * (1.0 / SR) / tau
        rpm[k] = max(r, 1.0)
        theta += rpm[k] * 6.0 / SR
    x = np.maximum(t - t_catch, 0)
    load = np.where(caught, 0.35 + 0.55 * np.exp(-x / 0.25), 0.0)
    fire = np.where(caught, 1.0, 0.0)
    misfire = np.where(caught, 0.45 * np.exp(-x / 0.12), 0.0)

    # Starter motor: armature whine and pinion mesh while engaged. Once the
    # engine fires, the overrunning clutch lets the pinion freewheel, then
    # it drops out and the armature spins down.
    th = np.cumsum(rpm * 6.0 / SR) * np.pi / 180.0
    t_off = t_catch + 0.12
    arm_max = e.starter_ratio * e.crank_rpm * 1.35 / 60.0
    arm = np.minimum(e.starter_ratio * rpm / 60.0, arm_max)
    k_off = int(t_off * SR)
    spin = arm.copy()
    spin[k_off:] = arm[k_off] * np.exp(-(t[k_off:] - t_off) / 0.12)
    ph = TAU * np.cumsum(spin) / SR
    amp = np.clip((t - t_sol) / 0.015, 0, 1) * np.where(t < t_off, 1.0, np.exp(-(t - t_off) / 0.08))
    labour = 1 + 0.6 * (e.crank_rpm - np.clip(rpm, 0, e.crank_rpm * 1.35)) / e.crank_rpm
    wob = 0.4 * np.sin(TAU * 31 * t)
    comm = np.sin(24 * ph + wob) + 0.4 * np.sin(48 * ph + 1)
    mesh = np.sin(9 * ph) + 0.35 * np.sin(18 * ph + 0.5)
    brush = filt(filt(rng.standard_normal(n), "bp", 2800, 0.7), "lp", 5000)
    brush *= 1.0 + 0.5 * np.sin(24 * ph)
    starter = (0.010 * comm + 0.022 * mesh + 0.018 * brush) * amp * labour
    # Compression chug: the whole truck heaves on each stroke while cranking.
    comp = np.maximum(0.0, np.cos(ncomp * th - 0.9)) ** 6
    chug = filt(rng.standard_normal(n), "lp", 160) * 0.5 + np.sin(TAU * 62 * t)
    starter += 0.06 * comp * chug * np.clip((t - t_sol) / 0.05, 0, 1) * np.clip((t_catch + 0.05 - t) / 0.05, 0, 1)
    starter = filt(starter, "lp", 6500)
    # Solenoid: clack in, softer clack out.
    for ts, g in ((t_sol, 1.0), (t_off + 0.01, 0.4)):
        m_ = int(0.05 * SR)
        tt = np.arange(m_) / SR
        clack = (np.sin(TAU * 2300 * tt) * np.exp(-tt / 0.004) + 0.8 * np.sin(TAU * 820 * tt) * np.exp(-tt / 0.009)
                 + 0.7 * filt(rng.standard_normal(m_), "hp", 2000) * np.exp(-tt / 0.0015))
        place(starter, ts * SR, clack, 0.05 * g)

    y, _ = render_engine(e, rpm, load, fire, e.key + "-start-eng", False, tail=int(0.25 * SR),
                         misfire=misfire, starter=starter)
    y = y[:n] - np.mean(y[:n])
    # Level it so the idle it settles into matches the closed-throttle idle
    # loop at the same volume, then fade over the last 0.4 s while the game
    # brings that loop in.
    off0 = engine_set(e)[1][0]
    settle = y[n - int(0.75 * SR):n - int(0.4 * SR)]
    y *= np.sqrt(np.mean(off0 ** 2)) / np.sqrt(np.mean(settle ** 2))
    if np.max(np.abs(y)) > ONESHOT_PEAK:
        y = ONESHOT_PEAK * softsat(y / ONESHOT_PEAK, 1.0)
    return fade(y, 0.002, 0.4)


# ======================================================================
# Other loops
# ======================================================================

def horn_loop(key, freqs, buzz, dur):
    """Electric horn: a diaphragm driven by an interrupter, so the wave is
    a clipped, asymmetric buzz with a trumpet formant."""
    rng = rng_for(key)
    n = 16 * round(dur * SR / 16)
    y = np.zeros(n)
    for i, f0 in enumerate(freqs):
        cyc = round(f0 * n / SR)
        f = cyc * SR / n
        wob = plfo(n, 6.0, rng) * 0.6 + plfo(n, 40.0, rng) * 0.25 * buzz
        ph = TAU * f * np.arange(n) / SR + wob
        x = np.sin(ph) + 0.45 * np.sin(2 * ph + 0.7) + 0.2 * np.sin(3 * ph + 0.3)
        x = np.tanh((1.5 + 3.0 * buzz) * (x + 0.35))           # interrupter contacts clip the drive
        y += x * (1.0 - 0.15 * i)
    y -= np.mean(y)
    rasp = pnoise(n, rng, 1500, 6000) * (0.5 + 0.5 * np.sin(TAU * round(freqs[0] * n / SR) * np.arange(n) / n))
    y += 0.08 * buzz * rasp
    form = lambda f: (H_bp2(f, 2300 if buzz > 0.5 else 2700, 2.0) * 1.0 + H_bp2(f, freqs[0] * 2.0, 1.6) * 0.6
                      + H_hp2(f, 300, 0.7) * 0.25) * H_lp2(f, 7000)
    return fshape(y, form)


def sfx_horn_cascade():
    return horn_loop("horn-cascade", (405.0,), 1.0, 0.45)


def sfx_horn_kestrel():
    return horn_loop("horn-kestrel", (420.0, 500.0), 0.35, 0.45)


def grains_loop(n, rng, rate, len_ms, band, q, amp_sigma, sr, env=None):
    y = np.zeros(n)
    count = int(rate * n / sr)
    for _ in range(count):
        m = int(rng.uniform(*len_ms) * sr / 1000)
        g = rng.standard_normal(m) * np.exp(-np.arange(m) / (m / 4))
        fc = np.exp(rng.uniform(np.log(band[0]), np.log(band[1])))
        g = filt(g, "bp", fc, q, sr=sr)
        pos = int(rng.integers(0, n))
        a = rng.lognormal(0, amp_sigma) * (env[pos] if env is not None else 1.0)
        place_wrap(y, pos, g, a)
    return y


def sfx_gravel():
    rng = rng_for("gravel")
    sr = SR
    n = 16 * round(0.8 * sr / 16)
    tread = np.clip(0.6 + 0.5 * np.sin(TAU * 13 * np.arange(n) / n) * (0.6 + 0.4 * plfo(n, 2, rng)), 0.15, None)
    dens = np.clip(0.7 + 0.5 * plfo(n, 7, rng), 0.1, None) * tread
    stones = grains_loop(n, rng, 900, (1.0, 5.0), (900, 5200), 2.0, 0.7, sr, dens)
    pebbles = grains_loop(n, rng, 160, (4.0, 14.0), (350, 1300), 1.5, 0.5, sr, dens)
    sizzle = pnoise(n, rng, 1800, 7000) * (0.6 + 0.4 * plfo(n, 20, rng)) * 0.06
    rumble = fshape(pnoise(n, rng), lambda f: H_lp2(f, 150, 0.9) * H_hp2(f, 35)) * tread * 0.3
    y = 0.55 * stones + 0.5 * pebbles + sizzle + rumble
    return fshape(tame(y, 99.8, 1.4), lambda f: H_lp2(f, 7500))


def sfx_dirt():
    rng = rng_for("dirt")
    sr = 16000
    n = 16 * round(0.8 * sr / 16)
    lug = 0.7 + 0.3 * np.sin(TAU * 7 * np.arange(n) / n) ** 2
    rumble = fshape(pnoise(n, rng, sr=sr), lambda f: H_lp2(f, 170, 1.0) * H_hp2(f, 30), sr) * lug
    clods = grains_loop(n, rng, 90, (6, 20), (180, 700), 1.4, 0.5, sr, lug)
    grass = pnoise(n, rng, 1800, 6500, sr) * np.clip(0.5 + 0.6 * plfo(n, 9, rng, sr), 0.05, None) * 0.07
    y = rumble * 0.9 + 0.5 * clods + grass
    return tame(y, 99.9, 1.2)


def bubble(f0, dur, rise, sr, rng=None):
    m = int(dur * sr)
    t = np.arange(m) / sr
    f = f0 * (1 + rise * t / dur)
    y = np.sin(TAU * np.cumsum(f) / sr) * np.exp(-t / (dur / 3.5))
    return y * np.minimum(t / 0.0015, 1)


def sfx_mud():
    """Tyres churning mud: a thick low slurry, wet slaps as the tread pulls
    out, and clods spattering the wheel arches. Nothing pitched: tones or
    pitch sweeps here (it once had both) read as bubbles popping."""
    rng = rng_for("mud")
    sr = SR
    n = 16 * round(1.2 * sr / 16)

    def unit(x):
        return x / (np.sqrt(np.mean(x * x)) + 1e-12)

    churn = fshape(pnoise(n, rng), lambda f: H_lp2(f, 240, 0.7) * H_hp2(f, 35)) * (0.7 + 0.3 * plfo(n, 5, rng))
    slaps = np.zeros(n)
    for _ in range(17):
        d = rng.uniform(0.03, 0.07)
        m = int(d * sr)
        t = np.arange(m) / sr
        env = np.minimum(t / 0.003, 1) * np.exp(-t / (d / 3))
        s = filt(rng.standard_normal(m) * env, "bp", rng.uniform(250, 600), 0.7, sr=sr)
        place_wrap(slaps, rng.integers(0, n), filt(s, "lp", 1400, sr=sr), rng.uniform(0.5, 1.0))
    spatter = grains_loop(n, rng, 70, (3, 9), (700, 2400), 0.8, 0.5, sr)
    y = unit(churn) + 0.8 * unit(slaps) + 0.35 * unit(spatter)
    return tame(y, 99.9, 1.3)


def sfx_water():
    rng = rng_for("water")
    sr = SR
    n = 16 * round(0.85 * sr / 16)
    surge = 0.75 + 0.25 * plfo(n, 3, rng)
    wash = pnoise(n, rng, 600, 7000) * surge * np.clip(0.6 + 0.5 * plfo(n, 25, rng), 0.1, None)
    wash = fshape(wash, lambda f: H_peak(f, 1800, 0.8, 4.0) * H_lp1(f, 4500))
    bow = fshape(pnoise(n, rng), lambda f: H_lp2(f, 320, 0.8) * H_hp2(f, 45)) * surge
    bubbles = np.zeros(n)
    for _ in range(160):
        f0 = np.exp(rng.uniform(np.log(350), np.log(3200)))
        b = bubble(f0, rng.uniform(0.006, 0.03) * (900 / f0) ** 0.5, rng.uniform(0.1, 0.6), sr)
        place_wrap(bubbles, rng.integers(0, n), b, rng.uniform(0.1, 0.4))
    drops = grains_loop(n, rng, 60, (0.5, 2.0), (2500, 6500), 2.0, 0.6, sr)
    y = 0.22 * wash + 0.55 * bow + 0.4 * bubbles + 0.25 * drops
    return tame(y, 99.9, 1.2)


def sfx_scrub():
    """Tyre rubber dragging across rock: stick-slip groan plus grit."""
    rng = rng_for("scrub")
    sr = SR
    n = 16 * round(0.6 * sr / 16)
    slow = plfo(n, 3, rng)
    judder = plfo(n, 30, rng)
    f = 205 * (1 + 0.06 * slow + 0.03 * judder)
    f *= round(np.sum(f) / sr) / (np.sum(f) / sr)
    ph = TAU * np.cumsum(f) / sr
    saw = sum(np.sin(h * ph + h) / h ** 1.1 for h in range(1, 18))
    gate = 0.55 + 0.45 * np.tanh(3 * plfo(n, 22, rng))
    groan = fshape(saw * gate, lambda ff: H_bp2(ff, 650, 1.2) + 0.6 * H_bp2(ff, 1500, 2.0))
    grit = grains_loop(n, rng, 500, (0.6, 3.0), (1500, 6000), 2.5, 0.3, sr) * 0.5
    rough = pnoise(n, rng, 300, 3000) * gate * 0.15
    y = 0.8 * groan + grit + rough
    return tame(y, 99.9, 1.3)


def sfx_wind():
    rng = rng_for("wind")
    sr = 16000
    n = 16 * round(1.6 * sr / 16)
    gust = 0.7 + 0.3 * plfo(n, 1.3, rng, sr)
    body = fshape(pnoise(n, rng, sr=sr), lambda f: H_lp2(f, 700, 0.6) * H_hp2(f, 60), sr)
    roar = pnoise(n, rng, 1200, 5500, sr) * 0.15
    # Whistles round the mirrors and roof rack: narrow bands that wander.
    wh = np.zeros(n)
    for fc in (620, 1040, 1730):
        nz = rng.standard_normal(n)
        wander = fc * (1 + 0.08 * plfo(n, 0.8, rng, sr))
        band = periodic(lambda s, fc=fc: filt(s, "bp", fc, 9.0, sr=sr), nz)
        wh += band * np.clip(0.4 + 0.8 * plfo(n, 1.0, rng, sr), 0, None) * (wander / fc)
    buffet = fshape(pnoise(n, rng, sr=sr), lambda f: H_lp2(f, 45, 0.8), sr) * 0.6
    y = (body * 0.9 + roar + 0.35 * wh) * gust + buffet * 0.5
    return y


def bird_call(kind, sr, rng):
    if kind == "teeoo":
        parts = []
        for f0, f1, d in ((3300, 3550, 0.16), (None, None, 0.05), (2750, 2450, 0.24)):
            m = int(d * sr)
            if f0 is None:
                parts.append(np.zeros(m))
                continue
            t = np.arange(m) / sr
            f = f0 + (f1 - f0) * t / d
            env = np.sin(np.pi * t / d) ** 0.8
            ph = TAU * np.cumsum(f) / sr
            parts.append((np.sin(ph) + 0.08 * np.sin(2 * ph)) * env)
        return np.concatenate(parts)
    if kind == "trill":
        out = np.zeros(int(0.75 * sr))
        for i in range(11):
            d = 0.032
            m = int(d * sr)
            t = np.arange(m) / sr
            f = 5200 - 1300 * t / d
            env = np.sin(np.pi * t / d) ** 2 * (0.6 + 0.4 * np.sin(np.pi * i / 10))
            place(out, i * 0.066 * sr, np.sin(TAU * np.cumsum(f) / sr) * env)
        return out
    if kind == "warble":
        notes = []
        for i in range(int(rng.integers(6, 10))):
            d = rng.uniform(0.05, 0.09)
            m = int(d * sr)
            t = np.arange(m) / sr
            f0 = rng.uniform(2400, 4600)
            f = f0 + rng.uniform(-600, 600) * t / d + 120 * np.sin(TAU * 32 * t)
            env = np.sin(np.pi * t / d) ** 1.2
            notes.append(np.sin(TAU * np.cumsum(f) / sr) * env)
            notes.append(np.zeros(int(rng.uniform(0.01, 0.04) * sr)))
        return np.concatenate(notes)
    if kind == "coo":
        parts = []
        for f0, d, a in ((520, 0.22, 0.8), (None, 0.12, 0), (560, 0.35, 1.0), (None, 0.08, 0), (500, 0.22, 0.7)):
            m = int(d * sr)
            if f0 is None:
                parts.append(np.zeros(m))
                continue
            t = np.arange(m) / sr
            f = f0 * (1 + 0.04 * np.sin(np.pi * t / d))
            ph = TAU * np.cumsum(f) / sr
            env = np.sin(np.pi * t / d) ** 1.5
            parts.append((np.sin(ph) + 0.25 * np.sin(2 * ph) + 0.1 * np.sin(3 * ph)) * env * a)
        return np.concatenate(parts)
    raise ValueError(kind)


def sfx_forest():
    rng = rng_for("forest")
    sr = 8000        # nothing in the breeze is above ~2.5 kHz
    n = 16 * round(3.2 * sr / 16)
    swell = np.clip(0.55 + 0.45 * plfo(n, 0.6, rng, sr), 0.1, None)
    flutter = np.clip(0.5 + 0.7 * plfo(n, 14, rng, sr), 0.0, None)
    leaves = fshape(pnoise(n, rng, sr=sr), lambda f: H_hp2(f, 1200, 0.7) * H_lp1(f, 2500), sr)
    leaves *= swell * (0.3 + 0.7 * flutter) * 0.035
    air = fshape(pnoise(n, rng, sr=sr), lambda f: H_lp2(f, 400, 0.6) * H_hp2(f, 60), sr) * swell * 0.05
    # Just the breeze: birds baked into a 3 s loop sang every half second
    # and repeated the same phrase; the game plays them one at a time now,
    # seconds apart (BIRD_*).
    return leaves + air


def sfx_bird(kind, dist):
    """One bird call with the forest's reverb tail, for the game to play now
    and then from somewhere about."""
    rng = rng_for("bird-" + kind)
    sr = 16000
    call = bird_call(kind, sr, rng)
    call = filt(call, "lp", 7000 - 2500 * (1 - dist), sr=sr)
    y = np.concatenate([call, np.zeros(int(0.9 * sr))])
    y = reverb_circ(y, 0.5, 1.2, 1.1, "bird-rev-" + kind, sr, lp_hz=5000, hp_hz=300)
    y[-int(0.25 * sr):] *= np.linspace(1, 0, int(0.25 * sr))
    return resample_poly(y, 3, 4)   # 12 kHz: the calls stay under 5.5 kHz


def sfx_creek():
    rng = rng_for("creek")
    sr = 16000
    n = 16 * round(2.0 * sr / 16)
    y = np.zeros(n)
    for _ in range(int(140 * n / sr)):
        f0 = np.exp(rng.normal(np.log(1100), 0.45))
        f0 = min(f0, 4500)
        b = bubble(f0, rng.uniform(0.008, 0.035) * (1000 / f0) ** 0.6, rng.uniform(0.1, 0.8), sr)
        place_wrap(y, rng.integers(0, n), b, rng.lognormal(-1.4, 0.6))
    babble = pnoise(n, rng, 300, 2600, sr) * np.clip(0.5 + 0.7 * plfo(n, 12, rng, sr), 0, None) * 0.12
    hiss = pnoise(n, rng, 2500, 7000, sr) * (0.8 + 0.2 * plfo(n, 2, rng, sr)) * 0.03
    gurgle = fshape(pnoise(n, rng, sr=sr), lambda f: H_bp2(f, 180, 1.5), sr) * np.clip(plfo(n, 4, rng, sr), 0, None) * 0.1
    y = 0.5 * y + babble + hiss + gurgle
    return tame(y, 99.95, 1.2)


# ======================================================================
# One-shots
# ======================================================================

def modal(n, modes, rng, sr=SR, bend=0.015):
    """Struck object: decaying modes (Hz, decay s, gain) with a slight
    pitch drop as the strike settles."""
    t = np.arange(n) / sr
    y = np.zeros(n)
    for f, tau, g in modes:
        fm = f * (1 + bend * np.exp(-t / 0.008))
        y += g * np.sin(TAU * np.cumsum(fm) / sr + rng.uniform(0, TAU)) * np.exp(-t / tau)
    return y * np.minimum(t / 0.0004, 1)


def thud(n, f0, f1, tau, sr=SR):
    t = np.arange(n) / sr
    f = f1 + (f0 - f1) * np.exp(-t / 0.02)
    return np.sin(TAU * np.cumsum(f) / sr) * np.exp(-t / tau) * np.minimum(t / 0.001, 1)


def rattle(n, rng, count, span, amp, band=(1800, 4800), sr=SR):
    y = np.zeros(n)
    for _ in range(count):
        st = rng.uniform(*span)
        m = int(rng.integers(20, 70))
        c = filt(rng.standard_normal(m), "bp", rng.uniform(*band), 3.0, sr=sr) * np.hanning(m)
        place(y, st * sr, c, amp * np.exp(-st / (span[1] * 0.6)) * rng.uniform(0.4, 1.0))
    return y


def sfx_click():
    rng = rng_for("click")
    n = int(0.09 * SR)
    y = modal(n, ((3600, 0.0025, 1.0), (5200, 0.0015, 0.6), (1450, 0.006, 0.5)), rng)
    y[:30] += rng.standard_normal(30) * np.hanning(30) * 0.6
    second = modal(n, ((2500, 0.003, 0.6), (1100, 0.008, 0.6)), rng)
    place(y, 0.011 * SR, second, 0.55)
    return filt(y, "hp", 400)


def sfx_clunk():
    """Drive-mode engage: actuator motor whirs, then the collar clunks home."""
    rng = rng_for("clunk")
    n = int(0.5 * SR)
    t = np.arange(n) / SR
    wd = 0.17
    whir_env = np.clip(t / 0.02, 0, 1) * np.clip((wd - t) / 0.02, 0, 1)
    f = 95 + 40 * np.clip(t / wd, 0, 1)
    ph = TAU * np.cumsum(f) / SR
    whir = (np.sin(7 * ph) * 0.6 + np.sin(14 * ph) * 0.3 + 0.4 * np.sin(23 * ph)) * whir_env
    whir += filt(rng.standard_normal(n), "bp", 2400, 1.0) * whir_env * 0.25
    whir = filt(whir, "lp", 5000) * 0.35
    hit = np.zeros(n)
    m = n - int(wd * SR)
    clunk = (thud(m, 140, 70, 0.06) * 1.0
             + modal(m, ((520, 0.05, 0.7), (1240, 0.03, 0.5), (2380, 0.016, 0.35), (3900, 0.008, 0.2)), rng)
             + filt(rng.standard_normal(m), "lp", 900) * np.exp(-np.arange(m) / (0.012 * SR)) * 0.6)
    place(hit, wd * SR, clunk)
    return whir + hit + rattle(n, rng, 4, (0.19, 0.3), 0.2)


def sfx_shift():
    rng = rng_for("shift")
    n = int(0.3 * SR)
    lever = modal(n, ((1900, 0.006, 0.6), (3100, 0.004, 0.4)), rng)
    clunk = thud(n, 110, 62, 0.07) + modal(n, ((410, 0.04, 0.6), (930, 0.025, 0.4), (1700, 0.012, 0.2)), rng)
    clunk += filt(rng.standard_normal(n), "lp", 600) * np.exp(-np.arange(n) / (0.015 * SR)) * 0.5
    y = 0.5 * lever
    place(y, 0.035 * SR, clunk, 1.0)
    return y


def suspension_bump(seed, heavy):
    rng = rng_for(seed)
    n = int((0.62 if heavy else 0.36) * SR)
    t = np.arange(n) / SR
    y = thud(n, 95 if heavy else 130, 42 if heavy else 68, 0.11 if heavy else 0.06) * 1.2
    y += filt(rng.standard_normal(n), "lp", 380 if heavy else 520) * np.exp(-t / (0.04 if heavy else 0.025)) * 1.2
    # Coil spring ringing after the hit.
    spring = np.sin(TAU * (175 if heavy else 210) * t + 0.3) * np.exp(-t / 0.08) * np.clip((t - 0.01) / 0.01, 0, 1)
    y += spring * (0.35 if heavy else 0.2)
    body = modal(n, ((240, 0.05, 0.6), (610, 0.03, 0.4), (1350, 0.015, 0.25)), rng)
    y += body * (0.7 if heavy else 0.35)
    y += rattle(n, rng, 10 if heavy else 4, (0.015, 0.25 if heavy else 0.14), 0.5 if heavy else 0.3)
    if heavy:
        # Bump stop: a second, harder clonk as the axle tops out.
        stop = thud(n, 160, 80, 0.04) + modal(n, ((780, 0.025, 0.5), (1900, 0.012, 0.3)), rng)
        place(y, 0.045 * SR, stop, 0.7)
    return softsat(y / np.max(np.abs(y)), 2.0)


def sfx_bump_light():
    return suspension_bump("bump-light", False)


def sfx_bump_heavy():
    return suspension_bump("bump-heavy", True)


def sfx_hit():
    """Skid plate on rock: a hard metallic strike, then a short scrape."""
    rng = rng_for("hit")
    n = int(0.75 * SR)
    t = np.arange(n) / SR
    strike = modal(n, ((1730, 0.06, 0.6), (2890, 0.04, 0.45), (4470, 0.025, 0.3), (690, 0.08, 0.5),
                       (6100, 0.012, 0.2)), rng)
    strike += thud(n, 150, 75, 0.07) * 1.2
    strike += filt(rng.standard_normal(n), "hp", 1500) * np.exp(-t / 0.003) * 0.8
    scrape_env = np.clip((t - 0.03) / 0.02, 0, 1) * np.exp(-np.maximum(t - 0.05, 0) / 0.18)
    chatter = np.clip(0.4 + 0.8 * filt(rng.standard_normal(n), "lp", 60), 0, None)
    scrape = filt(rng.standard_normal(n), "bp", 2400, 1.2) + 0.6 * filt(rng.standard_normal(n), "bp", 4800, 2.0)
    scrape *= scrape_env * chatter * 0.45
    grind = rattle(n, rng, 40, (0.03, 0.45), 0.35, (1500, 6000))
    return strike + scrape + grind


def sfx_splash():
    rng = rng_for("splash")
    n = int(0.85 * SR)
    t = np.arange(n) / SR
    whump = filt(rng.standard_normal(n), "lp", 160) * np.exp(-t / 0.07) * 3.0
    whump += thud(n, 90, 45, 0.08)
    crash_env = np.minimum(t / 0.008, 1) * np.exp(-t / 0.16)
    crash = filt(filt(rng.standard_normal(n), "hp", 500), "peak", 2200, 0.8, gain_db=5) * crash_env * 0.7
    falls = np.zeros(n)
    for _ in range(150):
        st = rng.uniform(0.02, 0.75) ** 1.3
        f0 = np.exp(rng.uniform(np.log(500), np.log(3500)))
        b = bubble(f0, rng.uniform(0.006, 0.025), rng.uniform(0.2, 0.8), SR)
        place(falls, st * SR, b, rng.uniform(0.1, 0.35) * np.exp(-st / 0.35))
    drops = np.zeros(n)
    for _ in range(80):
        st = rng.uniform(0.05, 0.8)
        m = int(rng.integers(10, 40))
        drops_g = filt(rng.standard_normal(m), "bp", rng.uniform(2500, 6000), 2.0) * np.hanning(m)
        place(drops, st * SR, drops_g, 0.3 * np.exp(-st / 0.3))
    return whump + crash + falls + drops


def sfx_blowoff():
    """Blow-off valve dumping boost, with the turbo whistle winding down."""
    rng = rng_for("blowoff")
    n = int(0.75 * SR)
    t = np.arange(n) / SR
    env = np.minimum(t / 0.006, 1) * np.exp(-t / 0.14) + 0.15 * np.exp(-t / 0.35) * np.minimum(t / 0.01, 1)
    nz = rng.standard_normal(n)
    psh = filt(filt(nz, "hp", 1200), "peak", 3400, 1.0, gain_db=7) * env
    psh = filt(psh, "lp", 8000)
    flutter = 1 + 0.25 * np.sin(TAU * 38 * t) * np.exp(-t / 0.08)
    f = 6400 * np.exp(-t / 0.5) + 1500
    whistle = np.sin(TAU * np.cumsum(f) / SR) * np.exp(-t / 0.3) * 0.08
    chuff = filt(nz, "lp", 400) * np.exp(-t / 0.02) * 0.8
    return psh * flutter + whistle + chuff


def bell(f, n, partials, strike=0.0, seed="bell", sr=SR):
    t = np.arange(n) / sr
    y = np.zeros(n)
    for r, a, d in partials:
        if r * f < 0.44 * sr:
            y += a * np.sin(TAU * f * r * t + r) * np.exp(-t / d)
    if strike:
        y += filt(rng_for(seed).standard_normal(n), "hp", 5000, sr=sr) * np.exp(-t / 0.004) * strike
    return fade(y * np.minimum(t / 0.0008, 1), 0.0, min(0.03, 0.2 * n / sr), sr)


def pluck(note, n, bright=1.0, decay=0.35):
    """Soft kalimba-ish pluck: warm, a bit woody, suits the outdoors."""
    f = float(midi_hz(note))
    t = np.arange(n) / SR
    y = np.sin(TAU * f * t) * np.exp(-t / decay)
    y += 0.25 * bright * np.sin(TAU * f * 2 * t + 0.4) * np.exp(-t / (decay * 0.4))
    y += 0.12 * bright * np.sin(TAU * f * 5.4 * t + 1.1) * np.exp(-t / 0.03)
    return y * np.minimum(t / 0.0012, 1)


def sfx_menu_move():
    n = int(0.06 * SR)
    t = np.arange(n) / SR
    y = np.sin(TAU * 1320 * t) * np.exp(-t / 0.014) + 0.35 * np.sin(TAU * 2640 * t + 1) * np.exp(-t / 0.006)
    return y * np.minimum(t / 0.0008, 1)


def sfx_menu_select():
    n = int(0.42 * SR)
    y = np.zeros(n)
    for i, note in enumerate((79, 86)):                 # G5, D6
        place(y, i * 0.065 * SR, pluck(note, n, 1.0, 0.1), 0.85 + 0.15 * i)
    y = fade(y, 0.0, 0.08)
    return add_reverb(y, 0.14, seconds=0.35, t60=0.3, seed="msel")


def sfx_menu_back():
    n = int(0.32 * SR)
    y = np.zeros(n)
    for i, note in enumerate((79, 74)):                 # G5, D5
        place(y, i * 0.07 * SR, pluck(note, n, 0.6, 0.07), 1.0 - 0.15 * i)
    y = fade(y, 0.0, 0.06)
    return add_reverb(y, 0.1, seconds=0.3, t60=0.25, seed="mback")


CHIME = ((1.0, 1.0, 0.55), (2.0, 0.35, 0.25), (3.0, 0.12, 0.12), (4.2, 0.06, 0.06))


def sfx_chime():
    """Viewpoint / milestone: a rising three-note chime."""
    n = int(1.0 * SR)
    y = np.zeros(n)
    for i, note in enumerate((79, 83, 86)):             # G5 B5 D6
        st = int(i * 0.11 * SR)
        place(y, st, bell(midi_hz(note), n - st, CHIME, strike=0.06, seed=f"ch{i}"), 0.8 + 0.1 * i)
        place(y, st, pluck(note - 12, n - st, 0.4, 0.18), 0.35)
    y = fade(y, 0.0, 0.25)
    return fade(add_reverb(y, 0.3, seconds=0.8, t60=0.8, seed="chr")[:int(1.25 * SR)], 0.0, 0.3)


def sfx_summit():
    """Summit: quick arpeggio up two octaves into a ringing chord."""
    n = int(1.75 * SR)
    y = np.zeros(n)
    arp = (67, 71, 74, 79, 83, 86, 91)                  # G major up to G6
    for i, note in enumerate(arp):
        st = int(i * 0.075 * SR)
        place(y, st, bell(midi_hz(note), n - st, CHIME, strike=0.05, seed=f"su{i}"), 0.55 + 0.05 * i)
    st = int(len(arp) * 0.075 * SR + 0.05 * SR)
    t = np.arange(n - st) / SR
    chord = np.zeros(n - st)
    for note, a in ((55, 0.7), (62, 0.5), (67, 0.5), (71, 0.4), (74, 0.35), (79, 0.3)):
        f = float(midi_hz(note))
        for det in (-0.004, 0.004):
            ph = TAU * f * (1 + det) * t
            chord += a * (np.sin(ph) + 0.3 * np.sin(2 * ph) + 0.1 * np.sin(3 * ph))
    chord *= np.minimum(t / 0.05, 1) * np.exp(-t / 0.5)
    place(y, st, chord, 0.22)
    for i, note in enumerate((91, 95, 98)):            # sparkle on top of the chord
        s2 = st + int(i * 0.05 * SR)
        place(y, s2, bell(midi_hz(note), n - s2, CHIME[:2], seed=f"sp{i}"), 0.3)
    y = fade(y, 0.0, 0.35)
    return fade(add_reverb(y, 0.35, seconds=0.9, t60=0.9, seed="sur")[:int(2.0 * SR)], 0.0, 0.35)


# ======================================================================
# Asset table
# ======================================================================

def engine_assets():
    out = []
    for e in ENGINES:
        for kind in ("on", "off"):
            for i in range(ENGINE_LOOPS):
                fn = (lambda e=e, kind=kind, i=i: engine_set(e)[0 if kind == "on" else 1][i])
                out.append((f"{e.key}_{kind.upper()}_{i}", fn, True, SR, False))
    return out


SFX = engine_assets() + [
    # name, generator, loop, rate, normalise
    ("HORN_CASCADE", sfx_horn_cascade, True, SR, True),
    ("HORN_KESTREL", sfx_horn_kestrel, True, SR, True),
    ("GRAVEL", sfx_gravel, True, SR, True),
    ("DIRT", sfx_dirt, True, 16000, True),
    ("MUD", sfx_mud, True, SR, True),
    ("WATER", sfx_water, True, SR, True),
    ("SCRUB", sfx_scrub, True, SR, True),
    ("WIND", sfx_wind, True, 16000, True),
    ("FOREST", sfx_forest, True, 8000, True),
    ("CREEK", sfx_creek, True, 16000, True),
    ("BIRD_TEEOO", lambda: sfx_bird("teeoo", 1.0), False, 12000, True),
    ("BIRD_WARBLE", lambda: sfx_bird("warble", 0.7), False, 12000, True),
    ("BIRD_TRILL", lambda: sfx_bird("trill", 0.6), False, 12000, True),
    ("BIRD_COO", lambda: sfx_bird("coo", 1.0), False, 12000, True),
    # Starts are stored at 16 kHz: nothing in them needs the top octave.
    ("START_CASCADE", lambda: resample_poly(engine_start(CASCADE), 320, 441), False, 16000, False),
    ("START_KESTREL", lambda: resample_poly(engine_start(KESTREL), 320, 441), False, 16000, False),
    ("CLICK", sfx_click, False, SR, True),
    ("CLUNK", sfx_clunk, False, SR, True),
    ("SHIFT", sfx_shift, False, SR, True),
    ("BUMP_LIGHT", sfx_bump_light, False, SR, True),
    ("BUMP_HEAVY", sfx_bump_heavy, False, SR, True),
    ("HIT", sfx_hit, False, SR, True),
    ("SPLASH", sfx_splash, False, SR, True),
    ("BLOWOFF", sfx_blowoff, False, SR, True),
    ("MENU_MOVE", sfx_menu_move, False, SR, True),
    ("MENU_SELECT", sfx_menu_select, False, SR, True),
    ("MENU_BACK", sfx_menu_back, False, SR, True),
    ("CHIME", sfx_chime, False, SR, True),
    ("SUMMIT", sfx_summit, False, SR, True),
]


def write_wav(path, data, rate):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(to_pcm16(np.asarray(data)).tobytes())


def seam_report(x):
    """Wrap discontinuity against the loop's own sample-to-sample motion."""
    d = np.abs(np.diff(x))
    seam = abs(x[0] - x[-1])
    d2 = np.abs(np.diff(x, 2))
    seam2 = abs(x[1] - 2 * x[0] + x[-1])
    return seam, np.percentile(d, 99.9), seam2, np.percentile(d2, 99.9)


def build_sfx(out, previews):
    pak = bytearray()
    entries = []
    print(f"{'effect':16s} {'secs':>5s} {'rate':>5s} {'KiB':>5s} {'peak':>6s} {'rms':>6s}  notes")
    for name, fn, loop, rate, normalise in SFX:
        x = np.asarray(fn(), dtype=np.float64)
        if not np.all(np.isfinite(x)):
            raise SystemExit(f"{name}: non-finite samples")
        x = finish_loop(x, normalise) if loop else finish_oneshot(x, rate, normalise)
        if len(x) > MAX_SFX_SAMPLES:
            raise SystemExit(f"{name}: {len(x)} samples exceeds AICA limit")
        if np.max(np.abs(x)) > 0.999:
            raise SystemExit(f"{name}: clips")
        pcm = to_pcm16(x)
        while len(pak) % 32:
            pak.append(0)
        off = len(pak)
        pak += pcm.tobytes()
        if loop:
            # Guard samples: the loop start again, outside `bytes`, so
            # interpolation that peeks past the loop end reads continuous data.
            pak += pcm[:16].tobytes()
        entries.append((name, off, len(pcm) * 2, int(loop), rate))
        peak = 20 * np.log10(np.max(np.abs(x)))
        if loop:
            seam, p999, seam2, p999_2 = seam_report(x)
            note = f"seam {seam:.4f} (p99.9 {p999:.4f}), curve {seam2:.4f} (p99.9 {p999_2:.4f})"
            if seam > p999 or seam2 > p999_2:
                raise SystemExit(f"{name}: loop seam discontinuity")
            preview = np.tile(x, max(2, int(np.ceil(2.0 * rate / len(x)))))
        else:
            note = f"edges {abs(x[0]):.4f}/{np.max(np.abs(x[-32:])):.4f}"
            preview = x
        print(f"{name:16s} {len(x) / rate:5.2f} {rate:5d} {len(pcm) * 2 / 1024:5.1f} {peak:6.1f} "
              f"{rms_db(x):6.1f}  {note}")
        write_wav(previews / f"{name.lower()}.wav", preview, rate)
    while len(pak) % 32:
        pak.append(0)
    if len(pak) > MAX_PAK_BYTES:
        raise SystemExit(f"sfx.pak is {len(pak)} bytes, over the {MAX_PAK_BYTES} budget")
    (out / "sfx.pak").write_bytes(bytes(pak))
    print(f"sfx.pak: {len(pak)} bytes ({len(pak) / 1024:.0f} KiB), {len(entries)} effects")
    return entries


def engine_report():
    for e in ENGINES:
        on, off, rpms = engine_set(e)
        print(f"{e.label}: loop rpm " + ", ".join(f"{r:.1f}" for r in rpms))
        for kind, loops in (("on ", on), ("off", off)):
            lv = [rms_db(x) for x in loops]
            steps = [lv[i + 1] - lv[i] for i in range(len(lv) - 1)]
            print(f"  {kind} rms dB " + " ".join(f"{v:6.1f}" for v in lv) +
                  "   neighbour steps " + " ".join(f"{s:+.1f}" for s in steps))
            if max(abs(s) for s in steps) > 1.5:
                raise SystemExit(f"{e.key} {kind}: neighbouring loops differ by more than 1.5 dB")


def write_header(out, entries):
    ids = {name: i for i, (name, *_) in enumerate(entries)}
    h = ["/* Generated by tools/build_audio.py. Do not edit by hand. */",
         "#ifndef RL_SOUND_ASSETS_H", "#define RL_SOUND_ASSETS_H", "",
         "#include <stdint.h>", "",
         f"#define RL_SFX_RATE {SFX_RATE}", "",
         "/* offset/bytes into sfx.pak (32-byte aligned entries), loop=1 for seamless loops, sample rate */",
         "typedef struct { uint32_t offset, bytes, loop, rate; } rl_sfx_asset_t;", "",
         "typedef enum {"]
    for name, *_ in entries:
        h.append(f"    RL_SFX_{name},")
    h += ["    RL_SFX_COUNT", "} rl_sfx_id_t;", "",
          "static const rl_sfx_asset_t rl_sfx_assets[RL_SFX_COUNT] __attribute__((unused)) = {"]
    for name, off, size, loop, rate in entries:
        secs = size / 2 / rate
        h.append(f"    {{ {off}u, {size}u, {loop}u, {rate}u }}, /* {name:14s} {secs:.2f} s */")
    h += ["};", "", "/* display names, for the sound test */",
          "static const char *const rl_sfx_names[RL_SFX_COUNT] __attribute__((unused)) = {"]
    for name, *_ in entries:
        h.append(f'    "{name.replace("_", " ")}",')
    h += ["};", "", f"#define RL_ENGINE_LOOPS {ENGINE_LOOPS}", "",
          "typedef struct {",
          "    uint16_t on[RL_ENGINE_LOOPS], off[RL_ENGINE_LOOPS];  /* sfx ids, rpm points ascending */",
          "    float rpm[RL_ENGINE_LOOPS];",
          "    float idle_rpm, redline_rpm;",
          "} rl_engine_sound_t;", "",
          "/* Play each loop at rate rpm / loop rpm. Each holds a whole number of",
          "   four-stroke cycles starting at cylinder 1's firing, so channels",
          "   started together stay in step. */",
          "static const rl_engine_sound_t rl_engine_sounds[2] __attribute__((unused)) = {"]
    for e in ENGINES:
        _, _, rpms = engine_set(e)
        on = ", ".join(f"RL_SFX_{e.key}_ON_{i}" for i in range(ENGINE_LOOPS))
        off = ", ".join(f"RL_SFX_{e.key}_OFF_{i}" for i in range(ENGINE_LOOPS))
        rp = ", ".join(f"{r:.2f}f" for r in rpms)
        h += [f"    {{ /* {e.label} */",
              f"        {{ {on} }},",
              f"        {{ {off} }},",
              f"        {{ {rp} }},",
              f"        {e.idle:.1f}f, {e.redline:.1f}f",
              "    },"]
    h += ["};", "",
          "/* per truck: the horn loop, and the engine start one-shot */",
          "static const uint16_t rl_horn_sfx[2] __attribute__((unused)) = { RL_SFX_HORN_CASCADE, RL_SFX_HORN_KESTREL };",
          "static const uint16_t rl_start_sfx[2] __attribute__((unused)) = { RL_SFX_START_CASCADE, RL_SFX_START_KESTREL };",
          "/* forest birds, one-shots the game plays now and then */",
          "#define RL_BIRDS 4",
          "static const uint16_t rl_bird_sfx[RL_BIRDS] __attribute__((unused)) = { RL_SFX_BIRD_TEEOO, RL_SFX_BIRD_WARBLE, RL_SFX_BIRD_TRILL, RL_SFX_BIRD_COO };",
          "", "#endif /* RL_SOUND_ASSETS_H */", ""]
    for name in ("HORN_CASCADE", "HORN_KESTREL", "START_CASCADE", "START_KESTREL"):
        assert name in ids
    (out / "sound_assets.h").write_text("\n".join(h))


# ======================================================================
# rpm sweep preview: the engine loops mixed the way the game mixes them
# ======================================================================

def sweep_track(e: Engine, secs=10.0, sr=SR):
    n = int(secs * sr)
    t = np.arange(n) / sr
    rpm = np.empty(n)
    thr = np.empty(n)
    r = e.idle
    for k in range(n):
        tk = t[k]
        if tk < 0.6:
            target, tau, th = e.idle, 0.3, 0.0
        elif tk < 5.0:
            target, tau, th = e.redline * 1.02, 1.6, 1.0     # on throttle, climbing
        elif tk < 9.3:
            target, tau, th = e.idle * 0.98, 1.1, 0.0        # lift off, overrun
        else:
            target, tau, th = e.idle, 0.3, 0.0
        r += (target - r) / (tau * sr)
        rpm[k] = min(max(r, e.idle), e.redline)
        thr[k] = th
    # Throttle moves over ~120 ms like a foot does.
    kern = np.hanning(int(0.12 * sr))
    thr = np.convolve(np.pad(thr, (len(kern), len(kern)), mode="edge"), kern / kern.sum(), "same")[len(kern):-len(kern)]
    return rpm, thr


def rpm_weights(rpms, r, curve="linear"):
    w = np.zeros(len(rpms))
    if r <= rpms[0]:
        w[0] = 1.0
    elif r >= rpms[-1]:
        w[-1] = 1.0
    else:
        i = int(np.searchsorted(rpms, r) - 1)
        x = (r - rpms[i]) / (rpms[i + 1] - rpms[i])
        if curve == "power":
            w[i], w[i + 1] = np.cos(x * np.pi / 2), np.sin(x * np.pi / 2)
        elif curve == "pow15":
            # Midway between linear (right for coherent loops) and equal
            # power (right for uncorrelated ones): 0.59 each at the middle.
            w[i], w[i + 1] = np.cos(x * np.pi / 2) ** 1.5, np.sin(x * np.pi / 2) ** 1.5
        else:
            w[i], w[i + 1] = 1 - x, x
    return w


def render_sweep(e: Engine, curve="linear", secs=10.0):
    on, off, rpms = engine_set(e)
    rpms = np.array(rpms)
    rpm, thr = sweep_track(e, secs)
    n = len(rpm)
    # Every channel starts together at sample 0 and keeps its own position;
    # AICA steps through each loop with linear interpolation.
    out = np.zeros(n)
    W = np.array([rpm_weights(rpms, r, curve) for r in rpm])
    for i in range(ENGINE_LOOPS):
        rate = rpm / rpms[i]
        pos = np.concatenate([[0.0], np.cumsum(rate)[:-1]])
        for loops, g in ((on, thr), (off, 1 - thr)):
            x = loops[i]
            L = len(x)
            p = pos % L
            i0 = np.floor(p).astype(int)
            fr = p - i0
            s = x[i0] * (1 - fr) + x[(i0 + 1) % L] * fr
            out += W[:, i] * g * s
    return out, rpm, thr


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output-dir", type=Path, default=ROOT / "assets" / "generated")
    ap.add_argument("--preview-dir", type=Path, default=ROOT / "captures" / "audio-previews")
    args = ap.parse_args()
    out = args.output_dir
    previews = args.preview_dir
    out.mkdir(parents=True, exist_ok=True)
    previews.mkdir(parents=True, exist_ok=True)

    engine_report()
    entries = build_sfx(out, previews)
    write_header(out, entries)
    for e in ENGINES:
        for curve, suffix in (("linear", ""), ("pow15", "_pow15")):
            y, _, _ = render_sweep(e, curve)
            write_wav(previews / f"sweep_{e.key.lower()}{suffix}.wav", y / max(1.0, np.max(np.abs(y)) / 0.98), SR)
    print(f"wrote {out / 'sfx.pak'}, {out / 'sound_assets.h'} and previews in {previews}")


if __name__ == "__main__":
    main()
