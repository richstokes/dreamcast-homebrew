#!/usr/bin/env python3
"""Find functions that fight over the SH-4's instruction cache.

The I-cache is 8 KB, direct-mapped, with 32-byte lines: a code address can
only sit in line (address >> 5) & 255. Two hot functions whose code maps to
the same line index at different addresses keep evicting each other, and a
loop through both refills those lines every time round. Code layout moves
with every link, so a small benchmark can speed up or slow down 10-40%
between builds of identical source code. Run this on both ELFs before
calling such a change a regression.

    tools/icache_clash.py -f romdisk_readdir,readdir,strncpy,strnlen,strcpy,strcmp \\
        kos-test-auto.elf kos-test-auto-kos-368a2eb.elf

Assumes the cache's default index mode (CCR.IIX clear), as KOS leaves it.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

LINE = 32
LINES = 256


def find_nm():
    for candidate in (os.environ.get('NM'), shutil.which('sh-elf-nm'),
                      os.path.join(os.environ.get('KOS_CC_BASE', ''), 'bin', 'sh-elf-nm'),
                      os.path.expanduser('~/.local/share/dreamcast/sh-elf/bin/sh-elf-nm')):
        if candidate and os.access(candidate, os.X_OK):
            return candidate
    sys.exit('sh-elf-nm not found: source the KOS environment or set NM')


def functions(nm, elf):
    """{name: (address, size)} for the ELF's code symbols. Assembly routines
    often have no size; theirs runs to the next function, skipping their own
    local labels (newlib's strcmp has a Longword_loop 12 bytes in)."""
    out = subprocess.run([nm, '-n', '-S', '--defined-only', str(elf)], check=True,
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in 'tTwW':
            syms.append((int(parts[0], 16), int(parts[1], 16), parts[2], parts[3]))
        elif len(parts) == 3 and parts[1] in 'tTwW':
            syms.append((int(parts[0], 16), 0, parts[1], parts[2]))
    found = {}
    for i, (addr, size, kind, name) in enumerate(syms):
        if not size:
            later = [a for a, s, k, _ in syms[i + 1:] if a > addr and (s or k in 'TW')]
            size = min(later[0] - addr, 4096) if later else LINE
        found.setdefault(name.lstrip('_'), (addr, size))
    return found


def report(nm, elf, wanted):
    syms = functions(nm, elf)
    lines = {}
    print(elf)
    for name in wanted:
        if name not in syms:
            print(f'  {name:18} not in this ELF')
            continue
        addr, size = syms[name]
        lines[name] = {a // LINE for a in range(addr, addr + size)}
        first, last = min(lines[name]) % LINES, max(lines[name]) % LINES
        print(f'  {name:18} {addr:08x} {size:5d} bytes  lines {first:3d}-{last:3d}')
    names = list(lines)
    clashes = 0
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            # Same index, different line: neighbours sharing one line are fine.
            fight = sorted({x % LINES for x in lines[a] for y in lines[b]
                            if x != y and x % LINES == y % LINES})
            if fight:
                clashes += 1
                print(f'  CLASH {a} / {b}: {len(fight)} line{"s" * (len(fight) > 1)} '
                      f'({", ".join(map(str, fight))})')
    if not clashes:
        print('  no clashes')
    return clashes


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('elf', nargs='+', type=Path)
    parser.add_argument('-f', '--functions', required=True,
                        help='comma-separated functions on the hot path (no leading _)')
    args = parser.parse_args()
    nm = find_nm()
    wanted = [f.strip() for f in args.functions.split(',') if f.strip()]
    for elf in args.elf:
        report(nm, elf, wanted)


if __name__ == '__main__':
    main()
