#!/usr/bin/env python3
"""Make the romdisk the filesystem benchmarks read: a 512 KB file with its
CRC32, and 64 small files for open, stat and readdir.

    tools/make_romdisk.py build/romdisk
"""
from pathlib import Path
import sys
import zlib

out = Path(sys.argv[1])
(out / 'small').mkdir(parents=True, exist_ok=True)
# xorshift32, so every build has the same bytes
x, data = 0x2545F491, bytearray()
for _ in range(512 * 1024 // 4):
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    data += x.to_bytes(4, 'little')
(out / 'big.bin').write_bytes(data)
(out / 'big.crc').write_text(f'{zlib.crc32(data):08x}\n')
for i in range(64):
    (out / 'small' / f'f{i:02d}').write_bytes(bytes((i + k) & 0xff for k in range(100)))
