#!/usr/bin/env python3
"""Attribute scheduler PC samples to symbols in the matching, unmodified ELF."""
import argparse
from bisect import bisect_right
from collections import Counter
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", type=Path)
parser.add_argument("--elf", type=Path, default=Path(__file__).resolve().parents[1] / "pico8-profile.elf")
args = parser.parse_args()
symbols = []
for line in subprocess.check_output(["sh-elf-nm", "-n", "-C", str(args.elf)], text=True).splitlines():
    match = re.match(r"([0-9a-f]+) [TtWw] (.+)", line)
    if match and not match[2].startswith("."):
        symbols.append((int(match[1], 16), match[2]))
addresses = [address for address, _ in symbols]
counts = Counter()
for line in args.log.read_text().splitlines():
    if "PICO8_PROFILE: BEGIN " in line:
        print("\n" + line.split("BEGIN ", 1)[1])
        counts.clear()
    elif "PICO8_PROFILE: END" in line:
        total = sum(counts.values())
        print(f"{total} active-thread samples (statistical attribution, not exact cycle counts)")
        for name, count in counts.most_common(25):
            print(f"{100 * count / total:6.2f}% {count:6d}  {name}")
    else:
        match = re.search(r"PICO8_PROFILE: ([0-9a-f]{8}) (\d+)", line)
        if match:
            index = bisect_right(addresses, int(match[1], 16) + 8) - 1
            counts[symbols[index][1] if index >= 0 else "unknown"] += int(match[2])
