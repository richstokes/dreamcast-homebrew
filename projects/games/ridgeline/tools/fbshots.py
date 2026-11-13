#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=1.26", "pillow>=10.1"]
# ///
"""Pictures out of a capture build's serial log: a build with
-DRL_AT_FBDUMP (src/autotest.c) writes each shot's frame to the console,
which is how to see the game when Flycast's window can't be photographed
(the Mac's screen locked). Run Flycast with
RL_FLYCAST_CONFIG=config:rend.EmulateFramebuffer=yes,config:rend.vsync=no.

Usage: uv run tools/fbshots.py console.log outdir"""
import base64
import sys
from pathlib import Path

import numpy as np
from PIL import Image

log, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
name, rows, n, w, h = None, [], 0, 640, 480
for raw in log.open("rb"):
    line = raw.decode("latin-1").rstrip()
    if line.startswith("RL FB:") and name:
        rows.append(base64.b64decode(line[6:])[:w * 2])
    elif line.startswith("RL FB END") and name:
        if len(rows) == h and all(len(r) == w * 2 for r in rows):
            px = np.frombuffer(b"".join(rows), dtype="<u2").reshape(h, w)
            rgb = np.dstack([(px >> 11) << 3, ((px >> 5) & 63) << 2, (px & 31) << 3]).astype(np.uint8)
            Image.fromarray(rgb, "RGB").save(out / f"{name}.png")
            n += 1
        else:
            print(f"{name}: {len(rows)} rows, skipped")
        name = None
    elif line.startswith("RL FB "):
        f = line.split()
        name, rows, w, h = f[2], [], int(f[3]), int(f[4])
print(f"{n} pictures in {out}")
