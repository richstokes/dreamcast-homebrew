#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyobjc-framework-Quartz"]
# ///
"""Run the DK_CAPTURE build and screenshot the Flycast window at each
"DK SHOT <name>" line. Usage: uv run tools/capture.py --out captures"""
import argparse, os, subprocess, sys, time
from pathlib import Path
import Quartz

def flycast_window():
    for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID):
        if w.get("kCGWindowOwnerName") == "Flycast" and w.get("kCGWindowBounds", {}).get("Height", 0) > 200:
            return w["kCGWindowNumber"]
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("captures"))
    ap.add_argument("--elf", default="dreamkarts-capture.elf")
    ap.add_argument("--delay", type=float, default=0.25)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent.parent
    env = dict(os.environ, DK_ELF=a.elf, DK_EXIT_ON_LOG="DK AUTOTEST: PASS", DK_FAIL_ON_LOG="DK AUTOTEST: FAIL")
    proc = subprocess.Popen([str(here / "run-flycast.sh"), "--skip-build"], cwd=here, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for raw in proc.stdout:
        line = raw.decode("latin-1").rstrip()
        if line.startswith("DK SHOT "):
            name = line[8:].strip()
            time.sleep(a.delay)
            wid = flycast_window()
            if wid:
                subprocess.run(["screencapture", "-x", "-o", "-l", str(wid), str(a.out / f"{name}.png")])
                print("captured", name, flush=True)
        elif line.startswith("DK AUTOTEST") or "panic" in line:
            print(line, flush=True)
    return proc.wait()

if __name__ == "__main__":
    sys.exit(main())
