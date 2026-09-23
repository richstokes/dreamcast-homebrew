#!/usr/bin/env python3
"""Summarize BROWSER_PROFILE serial records without treating idle time as FPS.

Usage: analyze-profile.py flycast.log [--expect-keys 200] [--json]
       cat flycast.log | analyze-profile.py

Only complete PROF timing records are counted. Status, BENCH, and other serial
output are ignored. Means are approximate because the firmware prints integer
averages; iteration phases are weighted by iterations and drawing/presentation
by rendered frames. A quiet window with no frames cannot lower drawing cost.
The last, not-yet-reported window is absent from the log, including its keys.
"""

import argparse
import contextlib
import json
import re
import sys


PROFILE = re.compile(
    r"\bPROF iters (?P<iterations>\d+) frames (?P<frames>\d+) "
    r"keys (?P<keys>\d+) \| wait (?P<wait>\d+) us/it "
    r"(?:poll (?P<poll>\d+) us/it wake (?P<wake>\d+) us/it "
    r"\(max (?P<wake_max>\d+)\) )?"
    r"input (?P<input>\d+) us/it draw (?P<draw>\d+) us/frame "
    r"\(max (?P<draw_max>\d+)\) present (?P<present>\d+) us/frame "
    r"\| iter max (?P<iteration_max>\d+) us(?:\s|$)"
)
PROFILE_START = re.compile(r"\bPROF iters\b")
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
ITERATION_PHASES = ("wait", "poll", "wake", "input")
FRAME_PHASES = ("draw", "present")


class ProfileSummary:
    """Streaming aggregation, with constant memory regardless of log length."""

    def __init__(self):
        self.windows = 0
        self.iterations = 0
        self.frames = 0
        self.keys = 0
        self.totals = dict.fromkeys(ITERATION_PHASES + FRAME_PHASES, 0)
        self.samples = dict.fromkeys(self.totals, 0)
        self.maxima = {"wake": None, "draw": None, "iteration": None}

    def add_line(self, line):
        """Return whether a record was read; reject damaged timing records."""
        line = ANSI.sub("", line)
        match = PROFILE.search(line)
        if not match:
            if PROFILE_START.search(line):
                raise ValueError("incomplete or unsupported PROF timing record")
            return False
        values = {name: int(value) if value is not None else None
                  for name, value in match.groupdict().items()}
        self.windows += 1
        self.iterations += values["iterations"]
        self.frames += values["frames"]
        self.keys += values["keys"]
        for phase in self.totals:
            if values[phase] is None:
                continue
            count = values["iterations" if phase in ITERATION_PHASES else "frames"]
            self.totals[phase] += values[phase] * count
            self.samples[phase] += count
        for phase in self.maxima:
            count = values["frames" if phase == "draw" else "iterations"]
            value = values[phase + "_max"]
            if count and value is not None:
                previous = self.maxima[phase]
                self.maxima[phase] = value if previous is None else max(previous, value)
        return True

    def result(self):
        return {
            "profile_windows": self.windows,
            "iterations": self.iterations,
            "frames": self.frames,
            "keys": self.keys,
            "mean_us": {phase: self.totals[phase] / self.samples[phase]
                        if self.samples[phase] else None for phase in self.totals},
            "sample_counts": dict(self.samples),
            "max_us": dict(self.maxima),
        }


def print_summary(result):
    print(f"Profile windows: {result['profile_windows']}")
    print(f"Iterations: {result['iterations']}  Frames: {result['frames']}  "
          f"Observed keys: {result['keys']}")
    print("Weighted mean timings (approximate):")
    for phase, mean in result["mean_us"].items():
        unit = "iteration" if phase in ITERATION_PHASES else "frame"
        count = result["sample_counts"][phase]
        if mean is None:
            print(f"  {phase:7} unavailable (no samples)")
        else:
            print(f"  {phase:7} {mean:10.2f} us/{unit} ({count} {unit}s)")
    peaks = [f"{phase} {value} us" for phase, value in result["max_us"].items()
             if value is not None]
    print("Recorded maxima: " + (", ".join(peaks) if peaks else "unavailable"))
    print("Counts cover complete profile windows only; no FPS is inferred.")


def nonnegative_int(text):
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must be zero or greater")
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="*", metavar="LOG",
                        help="serial log(s); default or '-' reads standard input")
    parser.add_argument("--expect-keys", type=nonnegative_int, metavar="N",
                        help="fail unless complete records contain exactly N keys")
    parser.add_argument("--json", action="store_true", help="emit a JSON summary")
    args = parser.parse_args(argv)
    summary = ProfileSummary()
    try:
        for path in args.logs or ["-"]:
            stream = (contextlib.nullcontext(sys.stdin) if path == "-" else
                      open(path, encoding="utf-8", errors="replace"))
            with stream as lines:
                for line_number, line in enumerate(lines, 1):
                    try:
                        summary.add_line(line)
                    except ValueError as error:
                        raise ValueError(f"{path}:{line_number}: {error}") from error
        if not summary.windows:
            raise ValueError("no complete PROF timing records found")
    except (OSError, ValueError) as error:
        print(f"analyze-profile: {error}", file=sys.stderr)
        return 2

    result = summary.result()
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print_summary(result)
    if args.expect_keys is not None and summary.keys != args.expect_keys:
        print(f"analyze-profile: expected {args.expect_keys} keys, "
              f"observed {summary.keys} in complete profile windows", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
