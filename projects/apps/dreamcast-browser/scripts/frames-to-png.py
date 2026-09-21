#!/usr/bin/env python3
"""Decode FRAME-BEGIN/F/FRAME-END blocks from a Flycast serial log into PNGs."""
import base64, struct, sys, zlib, os

def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

log, out = sys.argv[1], sys.argv[2]
label = None
for line in open(log, "rb").read().decode("latin-1").splitlines():
    if line.startswith("FRAME-BEGIN"):
        _, label, w, h, _ = line.split()
        w, h, data = int(w), int(h), []
    elif line.startswith("F ") and label:
        data.append(line[2:].strip())
    elif line.startswith("FRAME-END") and label:
        pixels = zlib.decompress(base64.b64decode("".join(data)))
        rgb = bytearray()
        for (p,) in struct.iter_unpack("<H", pixels):
            r, g, b = (p >> 11) & 31, (p >> 5) & 63, p & 31
            rgb += bytes(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))
        path = os.path.join(out, label + ".png")
        png(path, w, h, bytes(rgb))
        print(path)
        label = None
