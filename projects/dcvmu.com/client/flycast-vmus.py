#!/usr/bin/env python3
"""Expose all local Flycast VMUs in selectable banks of persistent test copies."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile

SLOTS = ('A2', 'B1', 'B2', 'C1', 'C2')  # A1 remembers login; port D is a keyboard.


def save_names(path):
    data = path.read_bytes()
    if len(data) != 131072:
        return []
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
    parser.add_argument('--vmu-image', action='append', type=Path, default=[])
    parser.add_argument('--vmus-dir', type=Path, help='Override VMU discovery directory')
    parser.add_argument('--bank', type=int, default=1)
    parser.add_argument('--list-vmus', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    os.umask(0o077)
    if args.vmu_image:
        images = [p.expanduser().resolve() for p in args.vmu_image]
    else:
        base = Path.home() / 'Library/Application Support/Flycast'
        directories = [args.vmus_dir.expanduser()] if args.vmus_dir else [base / 'data']
        if not args.vmus_dir and (base / 'emu.cfg').exists():
            for line in (base / 'emu.cfg').read_text().splitlines():
                key, sep, value = line.partition('=')
                if sep and key.strip() == 'Dreamcast.VMUPath' and value.strip():
                    directories.append(Path(value.strip()).expanduser())
        images = sorted({p.resolve() for folder in directories for p in folder.glob('*vmu*.bin')
                         if p.is_file() and p.stat().st_size == 131072})
    for p in images:
        if not p.is_file() or p.stat().st_size != 131072:
            parser.error(f'Expected a 128 KiB VMU image: {p}')
    names = {p: save_names(p) for p in images}
    if not args.vmu_image:
        images.sort(key=lambda p: (not bool(names[p]), p.name.lower(), str(p)))
    banks = max(1, (len(images) + len(SLOTS) - 1) // len(SLOTS))
    if args.list_vmus:
        for i, p in enumerate(images):
            print(f'Bank {i // 5 + 1}, {SLOTS[i % 5]}: {p.name} ({len(names[p])} saves)')
            if names[p]:
                print('  ' + ', '.join(names[p]))
        print(f'{len(images)} images, {banks} banks. A1 is reserved for persistent DCVMU login.')
        return
    if not 1 <= args.bank <= banks:
        parser.error(f'Choose --bank 1 through {banks}')
    root = Path.home() / 'Library/Application Support/DCVMU/VMUs'
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (root / '.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error('A DCVMU test session is already using these VMUs. Close it first.')
        with tempfile.TemporaryDirectory(prefix='dcvmu-bank-') as temporary:
            mount = Path(temporary)
            (mount / 'vmu_save_A1.bin').symlink_to(root / 'login.bin')
            print(f'VMU bank {args.bank}/{banks}. A1: persistent DCVMU login card', flush=True)
            selected = images[(args.bank-1)*5:args.bank*5]
            for slot, source in zip(SLOTS, selected):
                original = source.read_bytes()
                identity = hashlib.sha256(str(source).encode()).hexdigest()[:16]
                version = hashlib.sha256(original).hexdigest()[:16]
                copy = root / f'{identity}-{version}.bin'
                if not copy.exists():
                    with copy.open('xb') as target:
                        target.write(original)
                (mount / f'vmu_save_{slot}.bin').symlink_to(copy)
                print(f'{slot}: {source.name} -> persistent test copy', flush=True)
            # Host input ports are zero-based: route the Mac keyboard to D (3),
            # matching the emulated keyboard at device4; A-C hold VMUs.
            config = ('network:EmulateBBA=yes,network:DCNet=no,config:Debug.SerialConsoleEnabled=yes,'
                      'config:UploadCrashLogs=no,config:PerGameVmu=no,'
                      f'config:Dreamcast.VMUPath={mount},config:Dreamcast.SavePath={mount},'
                      'input:device1=0,input:device1.1=1,input:device1.2=1,'
                      'input:device2=0,input:device2.1=1,input:device2.2=1,'
                      'input:device3=0,input:device3.1=1,input:device3.2=1,'
                      'input:device4=5,input:maple_sdl_keyboard=3')
            command = (["/usr/bin/arch", '-' + args.arch] if args.arch else []) + [args.flycast, '-config', config, str(args.elf.resolve())]
            print(f'Original images stay unchanged. Test writes are retained in {root}', flush=True)
            if not args.dry_run:
                return subprocess.call(command)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
