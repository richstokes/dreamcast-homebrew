#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Compare complete fixed-step geometry QA tours, independently of elapsed time.

Both logs must come from SHOWCASE + VISUAL_QA + GEOMETRY_QA builds. Their final
geometry records and four district samples establish completion; shutdown text
is optional because the launcher may stop as soon as completion is reported.
This checks submitted geometry counts, not pixel or vertex-value equivalence.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Any


PREFIX = "Drift Los Angeles geometry QA: complete; "
DISTRICTS = {"DOWNTOWN CORE", "PACIFIC COAST", "ARTS QUARTER", "NEON STRIP"}
INTEGER_FIELDS = ("frames", "step_hz", "total_tri", "total_vtx", "peak_tri",
                  "peak_vtx", "elapsed_us")
COMPARE_FIELDS = (*INTEGER_FIELDS[:-1], "simulated_seconds")
NUMBER = r"[-+]?(?:\d+(?:\.\d+)?|\.\d+)(?:[eE][-+]?\d+)?"
SAMPLE = re.compile(
    rf"Drift Los Angeles visual QA: t=(\d+) district=(.*?) "
    rf"fps=({NUMBER}) reg=({NUMBER})ms"
)


def parse_geometry_log(source: str) -> dict[str, Any]:
    # Never reuse a completed earlier launch when an appended final run fails.
    source = source.rsplit("Drift Los Angeles booting.", 1)[-1]
    failures: list[str] = []
    result: dict[str, Any] = {"geometry": None, "districts_observed": [],
                              "failures": failures}
    if "Drift Los Angeles benchmark:" in source:
        failures.append("normal benchmark record is not fixed-step geometry QA")
    if re.search(r"allocation failed|initialization failed|panic|out of (?:video )?memory",
                 source, re.I):
        failures.append("serial log contains a runtime failure")
    completions = list(re.finditer(re.escape(PREFIX) + r"([^\r\n]*)", source))
    if len(completions) != 1:
        failures.append("expected exactly one completed geometry QA record")
        return result
    completion = completions[0]
    payload = completion[1]
    if not payload.endswith("."):
        failures.append("geometry QA completion record is truncated")
        return result
    fields: dict[str, str] = {}
    for token in payload[:-1].split():
        pair = token.split("=", 1)
        if len(pair) != 2 or pair[0] in fields:
            failures.append("geometry QA completion has malformed or duplicate fields")
            return result
        fields[pair[0]] = pair[1]
    missing = set((*INTEGER_FIELDS, "simulated_seconds")) - fields.keys()
    if missing:
        failures.append("geometry QA completion is missing fields: " + ", ".join(sorted(missing)))
        return result
    if any(not re.fullmatch(r"\d+", fields[key]) for key in INTEGER_FIELDS):
        failures.append("geometry QA counts, step_hz and elapsed_us must be positive integers")
        return result
    if not re.fullmatch(NUMBER, fields["simulated_seconds"]):
        failures.append("simulated_seconds must be a finite number")
        return result
    seconds = float(fields["simulated_seconds"])
    if not math.isfinite(seconds):
        failures.append("simulated_seconds must be a finite number")
        return result
    geometry = {key: int(fields[key]) for key in INTEGER_FIELDS}
    geometry["simulated_seconds"] = seconds
    result["geometry"] = geometry
    if any(geometry[key] <= 0 for key in INTEGER_FIELDS):
        failures.append("geometry QA counts, step_hz and elapsed_us must be positive")
    if geometry["step_hz"] != 30:
        failures.append("step_hz must be 30 for this deterministic tour")
    if not 60.0 <= seconds <= 60.05:
        failures.append("simulated_seconds must complete approximately 60 seconds")
    elif geometry["step_hz"] > 0 and abs(geometry["frames"] / geometry["step_hz"] - seconds) > .01:
        failures.append("frames and step_hz disagree with simulated_seconds")
    for suffix in ("tri", "vtx"):
        if not geometry[f"peak_{suffix}"] <= geometry[f"total_{suffix}"] <= (
                geometry["frames"] * geometry[f"peak_{suffix}"]):
            failures.append(f"total_{suffix} is inconsistent with frames and peak_{suffix}")
    # Samples after a completion cannot establish that its tour was complete.
    samples = SAMPLE.findall(source[:completion.start()])
    observed = {district for time, district, _, _ in samples if 0 < int(time) <= 60}
    result["districts_observed"] = sorted(observed)
    if not DISTRICTS <= observed:
        failures.append("missing four-district visual QA samples: " + ", ".join(sorted(DISTRICTS - observed)))
    return result


def compare_logs(baseline: str, candidate: str) -> dict[str, Any]:
    reports = {"baseline": parse_geometry_log(baseline),
               "candidate": parse_geometry_log(candidate)}
    failures = [f"{name}: {failure}" for name, report in reports.items()
                for failure in report["failures"]]
    mismatches = {}
    if not failures:
        for field in COMPARE_FIELDS:
            before = reports["baseline"]["geometry"][field]
            after = reports["candidate"]["geometry"][field]
            if before != after:
                mismatches[field] = {"baseline": before, "candidate": after}
                failures.append(f"{field} differs: baseline={before}, candidate={after}")
    return {**reports, "mismatches": mismatches, "failures": failures,
            "match": not failures,
            "note": "Elapsed time is excluded; matching counts do not establish identical rendered pixels."}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--json", dest="json_path", type=Path)
    args = parser.parse_args()
    try:
        baseline = args.baseline.read_text(encoding="utf-8", errors="replace")
        candidate = args.candidate.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        parser.error(str(error))
    report = compare_logs(baseline, candidate)
    report["baseline"]["log"] = str(args.baseline.resolve())
    report["candidate"]["log"] = str(args.candidate.resolve())
    serialized = json.dumps(report, indent=2) + "\n"
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(serialized, encoding="utf-8")
    print(serialized, end="")
    if report["failures"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
