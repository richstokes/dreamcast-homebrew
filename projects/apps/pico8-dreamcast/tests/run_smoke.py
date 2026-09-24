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
args = parser.parse_args()
if args.timeout <= 0:
    parser.error("--timeout must be positive")
args.log.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(["make", "-C", str(ROOT), "pico8-smoke.elf"], check=True)
command = [str(ROOT / "run-flycast.sh"), "--skip-build", "--smoke-test"]
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
                expected += ["PASS API", "PASS PICKER_PAUSE", "PASS SESSION_SAVE", "PASS ALL"]
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
