#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Check a complete Flycast QA serial log against the 30 FPS average target.

Average FPS is calculated from rendered frames / real elapsed time. Periodic
PVR FPS observations are reported separately and are never averaged to infer
whole-run performance. Logs predating aggregate telemetry remain readable,
but cannot pass the whole-run performance gate.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Any


HARDWARE_VRAM_BYTES = 8 * 1024 * 1024
NUMBER = r"[-+]?(?:\d+(?:\.\d+)?|\.\d+)(?:[eE][-+]?\d+)?"
FIELDS = re.compile(rf"([a-z_]+)=({NUMBER})")
SAMPLE = re.compile(
    rf"Drift Los Angeles (?:visual QA|showcase): t=(\d+) "
    rf"district=(.*?) fps=({NUMBER}) reg=({NUMBER})ms"
)
REQUIRED_FIELDS = (
    "frames", "elapsed_us", "average_fps", "peak_tri", "peak_vtx",
    "texture_bytes", "vram_free", "vertex_buffer_bytes",
)


def analyze_log(source: str, minimum_fps: float = 30.0,
                minimum_seconds: float = 30.0) -> dict[str, Any]:
    # An appended log may contain several launches. An unfinished final run
    # must not accidentally pass using an earlier run's completion record.
    source = source.rsplit("Drift Los Angeles booting.", 1)[-1]
    failures: list[str] = []
    samples = []
    for match in SAMPLE.finditer(source):
        samples.append({
            "game_time_seconds": int(match[1]),
            "district": match[2],
            "sample_fps": float(match[3]),
            "registration_ms": float(match[4]),
        })
    report: dict[str, Any] = {
        "minimum_average_fps": minimum_fps,
        "minimum_measured_seconds": minimum_seconds,
        "samples": samples,
        "districts_observed": list(dict.fromkeys(sample["district"] for sample in samples)),
        "sample_fps_minimum": min((sample["sample_fps"] for sample in samples), default=None),
        "sample_registration_ms_maximum": max(
            (sample["registration_ms"] for sample in samples), default=None),
        "benchmark": None,
        "failures": failures,
    }
    if re.search(r"allocation failed|initialization failed|panic|out of (?:video )?memory",
                 source, re.I):
        failures.append("serial log contains a graphics/runtime failure")
    lines = [line for line in source.splitlines()
             if "Drift Los Angeles benchmark:" in line]
    if not lines:
        failures.append("no completed aggregate benchmark; periodic FPS samples cannot establish an average")
        return report
    fields = {key: float(value) for key, value in FIELDS.findall(lines[-1])}
    missing = [key for key in REQUIRED_FIELDS if key not in fields]
    if missing:
        failures.append("benchmark is missing fields: " + ", ".join(missing))
        return report
    if any(not math.isfinite(value) or value < 0 for value in fields.values()):
        failures.append("benchmark fields must be finite and nonnegative")
        return report
    integer_fields = [key for key in REQUIRED_FIELDS if key != "average_fps"]
    if any(not fields[key].is_integer() for key in integer_fields):
        failures.append("benchmark counts and byte sizes must be integers")
        return report
    benchmark: dict[str, Any] = {
        key: int(value) if key in integer_fields else value
        for key, value in fields.items()
    }
    report["benchmark"] = benchmark
    frames, elapsed_us = benchmark["frames"], benchmark["elapsed_us"]
    if not frames or not elapsed_us:
        failures.append("benchmark must include positive rendered frames and elapsed time")
        return report
    elapsed_seconds = elapsed_us / 1_000_000
    average_fps = frames / elapsed_seconds
    benchmark["reported_average_fps"] = benchmark["average_fps"]
    benchmark["average_fps"] = round(average_fps, 4)
    benchmark["elapsed_seconds"] = elapsed_seconds
    if abs(average_fps - benchmark["reported_average_fps"]) > 0.011:
        failures.append("reported average FPS disagrees with frames / elapsed time")
    if average_fps < minimum_fps:
        failures.append(f"whole-run average {average_fps:.2f} FPS is below {minimum_fps:.2f} FPS")
    if elapsed_seconds < minimum_seconds:
        failures.append(f"measured duration {elapsed_seconds:.2f}s is below {minimum_seconds:.2f}s")

    texture_bytes = benchmark["texture_bytes"]
    free_bytes = benchmark["vram_free"]
    buffer_bytes = benchmark["vertex_buffer_bytes"]
    if not 0 < texture_bytes < HARDWARE_VRAM_BYTES:
        failures.append("runtime texture bytes are outside the hardware VRAM range")
    if not 0 <= free_bytes < HARDWARE_VRAM_BYTES or texture_bytes + free_bytes >= HARDWARE_VRAM_BYTES:
        failures.append("runtime VRAM inventory leaves no space for framebuffers and PVR allocations")
    if not 0 < buffer_bytes < HARDWARE_VRAM_BYTES:
        failures.append("configured vertex buffer size is outside the hardware VRAM range")
    benchmark["vertex_stream_lower_bound_bytes"] = benchmark["peak_vtx"] * 32
    if benchmark["vertex_stream_lower_bound_bytes"] > buffer_bytes:
        failures.append("submitted vertices alone exceed the configured PVR vertex buffer")
    benchmark["memory_note"] = (
        "Free VRAM is measured after runtime allocation. Vertex stream size is "
        "a lower bound: polygon headers also occupy the vertex buffer. "
        "No arbitrary texture-only budget or minimum free-memory reserve is imposed."
    )
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--minimum-fps", type=float, default=30.0)
    parser.add_argument("--minimum-seconds", type=float, default=30.0)
    parser.add_argument("--json", dest="json_path", type=Path)
    args = parser.parse_args()
    if any(not math.isfinite(value) or value < 0
           for value in (args.minimum_fps, args.minimum_seconds)):
        parser.error("minimums must be finite and nonnegative")
    report = analyze_log(args.log.read_text(encoding="utf-8", errors="replace"),
                         args.minimum_fps, args.minimum_seconds)
    report["log"] = str(args.log.resolve())
    serialized = json.dumps(report, indent=2) + "\n"
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(serialized, encoding="utf-8")
    print(serialized, end="")
    if report["failures"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
