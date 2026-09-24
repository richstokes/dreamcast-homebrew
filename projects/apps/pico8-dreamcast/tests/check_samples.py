#!/usr/bin/env python3
"""Offline verification of the five distributable, pinned sample cartridges."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / "samples/manifest.json").read_text())
assert len(manifest) == 5, "The default bundle must contain five games"
listed = {ROOT / game["file"] for game in manifest}
assert listed == set((ROOT / "romdisk/carts").iterdir()), "Unrecorded/missing cartridge"
for game in manifest:
    cartridge = ROOT / game["file"]
    data = cartridge.read_bytes()
    assert hashlib.sha256(data).hexdigest() == game["sha256"], cartridge
    assert game["license"] in {"MIT", "GPL-3.0-only"}, game["license"]
    assert len(game["revision"]) == 40, game["revision"]
    name = cartridge.name.split(".")[0]
    assert (ROOT / "licenses" / name / "LICENSE").is_file(), name
    if cartridge.suffix == ".png":
        assert data.startswith(b"\x89PNG\r\n\x1a\n"), cartridge
        source = ROOT / "samples/source" / (name + ".p8")
        assert source.is_file(), "PNG cartridge must include readable source"
    else:
        source = cartridge
    text = source.read_text()
    assert text.startswith("pico-8 cartridge"), source
    assert "\n__lua__\n" in text, source
    assert "\n#include " not in text, "All sample includes must be bundled"
    print(f"PASS {game['title']}: {game['license']}, pinned SHA-256")
