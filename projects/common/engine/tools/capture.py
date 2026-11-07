#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyobjc-framework-Quartz"]
# ///
"""Run a game's capture build through its run-flycast.sh and screenshot the
Flycast window at each "<PREFIX> SHOT <name>" line on the serial console.
The games' tools/capture.py call this with their project folder, prefix
and capture ELF; the window is found by the ELF's name in its title, so
parallel captures need distinct ELF names."""
import argparse, os, subprocess, sys, time
from pathlib import Path
import Quartz


def flycast_window(stem):
    """The window running our ELF (titled after it); otherwise the newest
    Flycast window, so another Flycast left open is never grabbed first."""
    best = None
    for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID):
        if w.get("kCGWindowOwnerName") != "Flycast" or w.get("kCGWindowBounds", {}).get("Height", 0) <= 200:
            continue
        if stem in (w.get("kCGWindowName") or ""):
            return w["kCGWindowNumber"]
        if best is None or w["kCGWindowNumber"] > best:
            best = w["kCGWindowNumber"]
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", type=Path, required=True, help="the game's folder (holds run-flycast.sh)")
    ap.add_argument("--prefix", required=True, help="the game's log prefix: DK, RL")
    ap.add_argument("--elf", required=True, help="the capture ELF in that folder")
    ap.add_argument("--out", type=Path, default=Path("captures"))
    ap.add_argument("--delay", type=float, default=0.25)
    ap.add_argument("--log", type=Path, help="also keep the whole serial console here")
    ap.add_argument("--until", help="stop once this shot is taken (a menu check without the whole run)")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    p = a.prefix
    env = dict(os.environ)
    env[f"{p}_ELF"] = a.elf
    env[f"{p}_EXIT_ON_LOG"] = f"{p} AUTOTEST: PASS"
    env[f"{p}_FAIL_ON_LOG"] = f"{p} AUTOTEST: FAIL"
    proc = subprocess.Popen([str(a.project / "run-flycast.sh"), "--skip-build"], cwd=a.project, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = open(a.log, "w") if a.log else None
    shot = f"{p} SHOT "
    for raw in proc.stdout:
        line = raw.decode("latin-1").rstrip()
        if log:
            log.write(line + "\n")
            log.flush()
        if line.startswith(shot):
            name = line[len(shot):].strip()
            time.sleep(a.delay)
            wid = flycast_window(Path(a.elf).stem)
            if wid:
                subprocess.run(["screencapture", "-x", "-o", "-l", str(wid), str(a.out / f"{name}.png")])
                print("captured", name, flush=True)
            if a.until and name == a.until:
                proc.terminate()
                proc.wait()
                return 0
        elif line.startswith(f"{p} A") or line.startswith(f"{p} STATE") or "panic" in line:
            print(line, flush=True)
    return proc.wait()


if __name__ == "__main__":
    sys.exit(main())
