#!/usr/bin/env python3
"""Launch the client in Flycast on Flycast's own VMUs, backing each card up first."""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess


def save_names(path):
    data = path.read_bytes()
    start, count = struct.unpack_from('<HH', data, 255 * 512 + 0x4a)
    if not 0 < count <= 16 or not count <= start < 255:
        return []
    names = []
    for block in range(start, start - count, -1):
        for offset in range(block * 512, (block + 1) * 512, 32):
            entry = data[offset:offset + 32]
            if entry[0] == 0x33:
                name = entry[4:16].rstrip(b'\0 ').decode('ascii', 'replace')
                if name != 'DCVMU_AUTH':
                    names.append(name)
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', required=True, type=Path)
    parser.add_argument('--flycast', required=True)
    parser.add_argument('--arch', default='')
    parser.add_argument('--list-vmus', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    os.umask(0o077)
    base = Path.home() / 'Library/Application Support/Flycast'
    directories = [base / 'data']
    if (base / 'emu.cfg').exists():
        for line in (base / 'emu.cfg').read_text().splitlines():
            key, sep, value = line.partition('=')
            if sep and key.strip() == 'Dreamcast.VMUPath' and value.strip():
                directories.append(Path(value.strip()).expanduser())
    # The cards Flycast uses with per-game VMUs off, named after their slot.
    images = sorted(p for folder in directories for p in folder.glob('vmu_save_[A-D][12].bin')
                    if p.is_file() and p.stat().st_size == 131072)
    for p in images:
        names = save_names(p)
        print(f'{p.name[9:11]}: {p.name} ({len(names)} saves)')
        if names:
            print('  ' + ', '.join(names))
    if args.list_vmus:
        return 0
    backups = Path.home() / 'Library/Application Support/DCVMU/backups'
    backups.mkdir(parents=True, exist_ok=True, mode=0o700)
    for p in images:
        original = p.read_bytes()
        backup = backups / f'{p.stem}-{hashlib.sha256(original).hexdigest()[:16]}.bin'
        if not backup.exists():
            backup.write_bytes(original)
    print(f"Using Flycast's own VMUs. Each card is backed up in {backups}", flush=True)
    # Host input ports are zero-based: route the Mac keyboard to D (3),
    # matching the emulated keyboard at device4.
    config = ('network:EmulateBBA=yes,network:DCNet=no,config:Debug.SerialConsoleEnabled=yes,'
              'config:UploadCrashLogs=no,config:PerGameVmu=no,'
              'input:device4=5,input:maple_sdl_keyboard=3')
    command = (["/usr/bin/arch", '-' + args.arch] if args.arch else []) + [args.flycast, '-config', config, str(args.elf.resolve())]
    return 0 if args.dry_run else subprocess.call(command)


if __name__ == '__main__':
    raise SystemExit(main())
