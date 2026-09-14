#!/usr/bin/env python3
"""Build seamless Dreamcast PCM loops from Drift Los Angeles's recorded V8 source."""

from __future__ import annotations

import argparse
import math
import wave
from array import array
from dataclasses import dataclass
from pathlib import Path


EXPECTED_RATE = 32_000
EXPECTED_CHANNELS = 2


@dataclass(frozen=True)
class LoopSpec:
    name: str
    start_seconds: float
    end_seconds: float
    crossfade_seconds: float
    base_rpm: float
    target_rms: float
    max_peak: float


# Keep the runtime bank deliberately old-school: one genuinely stationary idle
# recording and one genuinely stationary held-rev recording. Each voice is used
# over a narrow playback-rate range and the game crossfades between them.
LOOPS = (
    LoopSpec("idle", 4.85, 7.00, 0.200, 950.0, 0.120, 0.42),
    LoopSpec("load", 17.10, 19.25, 0.200, 3500.0, 0.140, 0.48),
)

TIRE_LOOP = LoopSpec("squeal", 0.10, 1.18, 0.180, 0.0, 0.120, 0.65)


def level_circular_macro_envelope(values: array, rate: int, channels: int,
                                  window_seconds: float = 0.18,
                                  passes: int = 2) -> array:
    """Remove slow loop-rate pumping while preserving individual firings."""
    output = array("f", values)
    frames = len(output) // channels
    window = max(1, min(frames - 1, round(window_seconds * rate)))
    half = window // 2
    for _ in range(passes):
        energy = []
        for frame in range(frames):
            total = 0.0
            for channel in range(channels):
                sample = output[frame * channels + channel]
                total += sample * sample
            energy.append(total / channels)
        target_energy = sum(energy) / len(energy)
        wrapped = energy + energy + energy
        prefix = [0.0]
        for sample_energy in wrapped:
            prefix.append(prefix[-1] + sample_energy)
        gains = []
        for frame in range(frames):
            center = frames + frame
            start = center - half
            local_energy = (prefix[start + window] - prefix[start]) / window
            gain = math.sqrt(target_energy / max(local_energy, 1e-12))
            gains.append(max(0.78, min(1.28, gain)))
        for frame, gain in enumerate(gains):
            for channel in range(channels):
                output[frame * channels + channel] *= gain
    return output


def read_pcm(path: Path) -> tuple[int, int, array]:
    with wave.open(str(path), "rb") as source:
        if source.getsampwidth() != 2:
            raise ValueError(f"{path} must be 16-bit PCM")
        rate = source.getframerate()
        channels = source.getnchannels()
        if rate != EXPECTED_RATE or channels != EXPECTED_CHANNELS:
            raise ValueError(
                f"{path} must be {EXPECTED_RATE} Hz stereo; got {rate} Hz, "
                f"{channels} channel(s)"
            )
        pcm = array("h")
        pcm.frombytes(source.readframes(source.getnframes()))
    if pcm.itemsize != 2:
        raise RuntimeError("host does not provide 16-bit array('h') values")
    return rate, channels, pcm


def make_loop(pcm: array, rate: int, channels: int, spec: LoopSpec) -> array:
    start_frame = round(spec.start_seconds * rate)
    end_frame = round(spec.end_seconds * rate)
    fade_frames = round(spec.crossfade_seconds * rate)
    if start_frame < 0 or end_frame <= start_frame:
        raise ValueError(f"invalid region for {spec.name}")
    if end_frame * channels > len(pcm):
        raise ValueError(f"{spec.name} region exceeds source recording")
    if fade_frames <= 0 or fade_frames * 2 >= end_frame - start_frame:
        raise ValueError(f"invalid crossfade for {spec.name}")

    clip = array("f", pcm[start_frame * channels:end_frame * channels])
    channel_means = []
    frame_count = len(clip) // channels
    for channel in range(channels):
        channel_means.append(
            sum(clip[channel::channels]) / float(frame_count)
        )
    for frame in range(frame_count):
        for channel in range(channels):
            index = frame * channels + channel
            clip[index] -= channel_means[channel]

    # Fold the tail over the head. Equal-power weights are essential here:
    # ordinary linear crossfades lose roughly 3 dB at their midpoint when the
    # two pieces are decorrelated, producing the exact loud/quiet loop pulse
    # this builder is intended to prevent.
    output_frames = frame_count - fade_frames
    loop = array("f", clip[:output_frames * channels])
    for frame in range(fade_frames):
        blend = (frame + 1.0) / (fade_frames + 1.0)
        head_weight = math.sin(blend * math.pi * 0.5)
        tail_weight = math.cos(blend * math.pi * 0.5)
        tail_frame = output_frames + frame
        for channel in range(channels):
            index = frame * channels + channel
            tail_index = tail_frame * channels + channel
            loop[index] = (
                clip[tail_index] * tail_weight + clip[index] * head_weight
            )

    if spec.base_rpm > 0.0:
        loop = level_circular_macro_envelope(loop, rate, channels)

    peak = max(abs(sample) for sample in loop)
    rms = math.sqrt(sum(sample * sample for sample in loop) / len(loop))
    if peak <= 0.0 or rms <= 0.0:
        raise ValueError(f"{spec.name} region is silent")
    gain = min(spec.target_rms * 32768.0 / rms,
               spec.max_peak * 32767.0 / peak)

    result = array("h")
    for sample in loop:
        result.append(max(-32768, min(32767, round(sample * gain))))
    return result


def format_array(symbol: str, values: array) -> str:
    lines = []
    for offset in range(0, len(values), 12):
        row = ", ".join(str(value) for value in values[offset:offset + 12])
        lines.append(f"    {row},")
    return (
        f"const int16_t {symbol}[{len(values)}] "
        "__attribute__((aligned(32))) = {\n"
        + "\n".join(lines)
        + "\n};\n"
    )


def planarize(values: array, channels: int) -> array:
    """Arrange stereo as a complete left plane followed by a right plane."""
    planar = array("h")
    for channel in range(channels):
        planar.extend(values[channel::channels])
    return planar


def macro_loudness_ratio(values: array, rate: int, channels: int,
                         window_seconds: float = 0.20,
                         hop_seconds: float = 0.025) -> tuple[float, float]:
    """Return circular p95/p05 and minimum/mean RMS over slow windows."""
    frames = len(values) // channels
    window = max(1, round(window_seconds * rate))
    hop = max(1, round(hop_seconds * rate))
    energy = []
    for frame in range(frames):
        total = 0.0
        for channel in range(channels):
            sample = values[frame * channels + channel] / 32768.0
            total += sample * sample
        energy.append(total / channels)
    wrapped = energy + energy[:window]
    prefix = [0.0]
    for sample_energy in wrapped:
        prefix.append(prefix[-1] + sample_energy)
    levels = []
    for start in range(0, frames, hop):
        square_mean = (prefix[start + window] - prefix[start]) / window
        levels.append(math.sqrt(max(0.0, square_mean)))
    ordered = sorted(levels)
    low = ordered[round((len(ordered) - 1) * 0.05)]
    high = ordered[round((len(ordered) - 1) * 0.95)]
    mean = sum(levels) / len(levels)
    return high / max(low, 1e-9), min(levels) / max(mean, 1e-9)


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def smoothstep(value: float) -> float:
    value = clamp(value, 0.0, 1.0)
    return value * value * (3.0 - 2.0 * value)


def loop_sample(pcm: array, phase: float, channel: int) -> float:
    frames = len(pcm) // EXPECTED_CHANNELS
    whole = int(phase) % frames
    following = (whole + 1) % frames
    fraction = phase - math.floor(phase)
    a = pcm[whole * EXPECTED_CHANNELS + channel]
    b = pcm[following * EXPECTED_CHANNELS + channel]
    return (a + (b - a) * fraction) / 32768.0


def write_preview(path: Path, loops: list[tuple[LoopSpec, array]],
                  tire_pcm: array) -> None:
    """Write a representative idle/rev sweep followed by the tire loop."""
    path.parent.mkdir(parents=True, exist_ok=True)
    loop_by_name = {spec.name: (spec, pcm) for spec, pcm in loops}
    idle_spec, idle_pcm = loop_by_name["idle"]
    load_spec, load_pcm = loop_by_name["load"]
    combined = array("h")
    idle_phase = 0.0
    load_phase = 0.0
    duration = 22.0
    for frame in range(round(duration * EXPECTED_RATE)):
        time = frame / EXPECTED_RATE
        if time < 3.0:
            rpm = 950.0
            throttle = 0.08
        elif time < 9.0:
            amount = (time - 3.0) / 6.0
            rpm = 950.0 + amount * 5950.0
            throttle = 0.92
        elif time < 13.0:
            rpm = 6900.0
            throttle = 1.0
        elif time < 19.0:
            amount = (time - 13.0) / 6.0
            rpm = 6900.0 - amount * 5950.0
            throttle = 0.05
        else:
            rpm = 950.0
            throttle = 0.08

        blend = smoothstep((rpm - 1350.0) / 1050.0)
        idle_weight = math.sqrt(1.0 - blend) * (
            0.90 + 0.10 * (1.0 - throttle)
        )
        load_weight = math.sqrt(blend) * (0.40 + 0.60 * throttle)
        idle_pitch = clamp(rpm / idle_spec.base_rpm, 0.84, 1.95)
        load_pitch = clamp(rpm / load_spec.base_rpm, 0.52, 1.90)
        for channel in range(EXPECTED_CHANNELS):
            sample = (
                loop_sample(idle_pcm, idle_phase, channel) * idle_weight
                + loop_sample(load_pcm, load_phase, channel) * load_weight
            ) * 0.82
            combined.append(round(clamp(sample, -0.94, 0.94) * 32767.0))
        idle_phase = (idle_phase + idle_pitch) % (len(idle_pcm) // EXPECTED_CHANNELS)
        load_phase = (load_phase + load_pitch) % (len(load_pcm) // EXPECTED_CHANNELS)

    silence = array("h", [0] * (EXPECTED_RATE // 3 * EXPECTED_CHANNELS))
    combined.extend(silence)
    combined.extend(tire_pcm)
    combined.extend(tire_pcm)
    with wave.open(str(path), "wb") as preview:
        preview.setnchannels(EXPECTED_CHANNELS)
        preview.setsampwidth(2)
        preview.setframerate(EXPECTED_RATE)
        preview.writeframes(combined.tobytes())


def build(idle_source_path: Path, load_source_path: Path,
          tire_source_path: Path, output_dir: Path,
          preview_path: Path | None) -> None:
    rate, channels, idle_source_pcm = read_pcm(idle_source_path)
    load_rate, load_channels, load_source_pcm = read_pcm(load_source_path)
    if load_rate != rate or load_channels != channels:
        raise ValueError("idle and held-rev sources must use the same PCM format")
    source_by_name = {
        "idle": idle_source_pcm,
        "load": load_source_pcm,
    }
    built = [
        (spec, make_loop(source_by_name[spec.name], rate, channels, spec))
        for spec in LOOPS
    ]
    tire_rate, tire_channels, tire_source_pcm = read_pcm(tire_source_path)
    if tire_rate != rate or tire_channels != channels:
        raise ValueError("engine and tire sources must use the same PCM format")
    tire_pcm = make_loop(tire_source_pcm, tire_rate, tire_channels, TIRE_LOOP)
    output_dir.mkdir(parents=True, exist_ok=True)

    header = """/* Generated by tools/build_audio_assets.py. Do not edit. */
#ifndef DRIFT_LA_AUDIO_ASSETS_H
#define DRIFT_LA_AUDIO_ASSETS_H

#include <stdint.h>

#define DLA_AUDIO_ASSET_RATE 32000u

"""
    source = """/* Generated by tools/build_audio_assets.py. Do not edit. */
#include "audio_assets.h"

"""
    for spec, pcm in built:
        frames = len(pcm) // channels
        if frames > 65_534:
            raise ValueError(
                f"{spec.name} has {frames} frames; AICA loop ends are 16-bit"
            )
        upper = spec.name.upper()
        header += f"#define DLA_ENGINE_{upper}_FRAMES {frames}u\n"
        header += f"#define DLA_ENGINE_{upper}_BASE_RPM {spec.base_rpm:.1f}f\n"
        header += (
            f"extern const int16_t dla_engine_{spec.name}_pcm[{len(pcm)}];\n\n"
        )
        source += format_array(
            f"dla_engine_{spec.name}_pcm", planarize(pcm, channels)
        ) + "\n"

        first_left = pcm[0] / 32768.0
        last_left = pcm[-channels] / 32768.0
        peak = max(abs(value) for value in pcm) / 32768.0
        rms = math.sqrt(sum(value * value for value in pcm) / len(pcm)) / 32768.0
        macro_ratio, macro_floor = macro_loudness_ratio(pcm, rate, channels)
        if macro_ratio > 1.15 or macro_floor < 0.90:
            raise ValueError(
                f"{spec.name} loop has a slow loudness pulse: "
                f"p95/p05={macro_ratio:.2f}, floor={macro_floor:.2f}"
            )
        print(
            f"{spec.name:7s}: {frames:5d} frames, {frames / rate:.3f}s, "
            f"base {spec.base_rpm:.0f} rpm, RMS {rms:.3f}, peak {peak:.3f}, "
            f"boundary jump {abs(first_left - last_left):.5f}, "
            f"macro p95/p05 {macro_ratio:.2f}, floor {macro_floor:.2f}"
        )

    tire_frames = len(tire_pcm) // channels
    if tire_frames > 65_534:
        raise ValueError(
            f"tire has {tire_frames} frames; AICA loop ends are 16-bit"
        )
    header += f"#define DLA_TIRE_SQUEAL_FRAMES {tire_frames}u\n"
    header += f"extern const int16_t dla_tire_squeal_pcm[{len(tire_pcm)}];\n\n"
    source += format_array(
        "dla_tire_squeal_pcm", planarize(tire_pcm, channels)
    ) + "\n"
    tire_first = tire_pcm[0] / 32768.0
    tire_last = tire_pcm[-channels] / 32768.0
    tire_peak = max(abs(value) for value in tire_pcm) / 32768.0
    tire_rms = math.sqrt(
        sum(value * value for value in tire_pcm) / len(tire_pcm)
    ) / 32768.0
    print(
        f"tire   : {tire_frames:5d} frames, {tire_frames / rate:.3f}s, "
        f"RMS {tire_rms:.3f}, peak {tire_peak:.3f}, "
        f"boundary jump {abs(tire_first - tire_last):.5f}"
    )

    header += "#endif\n"
    (output_dir / "audio_assets.h").write_text(header, encoding="utf-8")
    (output_dir / "audio_assets.c").write_text(source, encoding="utf-8")
    if preview_path is not None:
        write_preview(preview_path, built, tire_pcm)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idle-source", required=True, type=Path)
    parser.add_argument("--load-source", required=True, type=Path)
    parser.add_argument("--tire-source", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    build(args.idle_source.resolve(), args.load_source.resolve(),
          args.tire_source.resolve(),
          args.output_dir.resolve(),
          args.preview.resolve() if args.preview else None)


if __name__ == "__main__":
    main()
