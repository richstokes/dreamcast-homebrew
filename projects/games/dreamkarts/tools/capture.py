#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyobjc-framework-Quartz"]
# ///
"""Run the DK_CAPTURE build and screenshot the Flycast window at each
"DK SHOT <name>" line (the engine's tools/capture.py with this game's
names). Usage: uv run tools/capture.py --out captures"""
import runpy, sys
from pathlib import Path

here = Path(__file__).resolve()
sys.argv[1:1] = ["--project", str(here.parents[1]), "--prefix", "DK", "--elf", "dreamkarts-capture.elf"]
runpy.run_path(str(here.parents[3] / "common" / "engine" / "tools" / "capture.py"), run_name="__main__")
