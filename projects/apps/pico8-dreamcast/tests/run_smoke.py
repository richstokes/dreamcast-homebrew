#!/usr/bin/env python3
"""Boot the SH-4 test ELF, require serial success, then stop our Flycast process."""
import argparse
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--timeout", type=int, default=300)
parser.add_argument("--log", type=Path, default=ROOT / "build/smoke.log")
parser.add_argument("--profile", action="store_true", help="Collect scheduler PC samples; use analyze_profile.py afterward")
parser.add_argument("--kernels", action="store_true", help="Run only audio/rendering conformance kernels")
args = parser.parse_args()
if args.timeout <= 0:
    parser.error("--timeout must be positive")
args.log.parent.mkdir(parents=True, exist_ok=True)
if args.profile and args.kernels:
    parser.error("--profile and --kernels cannot be combined")
target = "pico8-kernels.elf" if args.kernels else "pico8-profile.elf" if args.profile else "pico8-smoke.elf"
subprocess.run(["make", "-C", str(ROOT), target], check=True)
command = [str(ROOT / "run-flycast.sh"), "--skip-build", "--kernels" if args.kernels else "--profile" if args.profile else "--smoke-test"]
result = "FAIL: timed out waiting for Dreamcast tests"
with args.log.open("w") as output:
    process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, env=os.environ.copy())
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            log = args.log.read_text(errors="replace")
            if "PICO8_TEST: FAIL" in log or "Kernel panic" in log or "Unhandled exception" in log:
                result = "FAIL: see serial log"
                break
            if "PICO8: player exited cleanly status=0" in log:
                expected = ["PASS /rd/carts/" + name for name in (
                    "anteform.p8", "bull_sheep.p8", "elephant.p8", "picolumia.p8.png", "pikoralli.p8")]
                expected += ["PASS KERNELS", "PASS FRAME_CACHE", "PASS API", "PASS PERFORMANCE_CONFORMANCE", "PASS PICKER_PAUSE", "PASS SESSION_SAVE", "PASS ALL"]
                if args.kernels: expected = ["PASS KERNELS", "PASS FRAME_CACHE"]
                result = "PASS" if all("PICO8_TEST: " + item in log for item in expected) else "FAIL: missing results"
                break
            if process.poll() is not None:
                result = "FAIL: Flycast exited before tests finished"
                break
            time.sleep(0.25)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
print(f"{result}\nSerial log: {args.log}")
raise SystemExit(0 if result == "PASS" else 1)
