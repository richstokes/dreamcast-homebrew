#!/usr/bin/env python3
"""Run vmusave.h's SH-4 checks in Flycast on blank cards, then check the card images.

The images are read back with the DCVMU service's VMU parser, which checks
the package CRC and header independently of KOS.
"""
import argparse
import base64
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

COMMON = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(COMMON.parent / 'dcvmu.com' / 'service'))
from vmu_tools import extract_image  # noqa: E402
from vmu_validation import header_metadata  # noqa: E402


def check_cards(vmus):
    """Return the problems found in the A1 and A2 images the checks left."""
    problems = []
    a1 = {row['filename']: row for row in extract_image((vmus / 'vmu_save_A1.bin').read_bytes(), 'vmu_save_A1.bin')}
    a2 = {row['filename']: row for row in extract_image((vmus / 'vmu_save_A2.bin').read_bytes(), 'vmu_save_A2.bin')}
    if set(a1) != {'VMUSAVE_T'} or set(a2) != {'VMUSAVE_2'}:
        return [f'unexpected files: A1 {sorted(a1)}, A2 {sorted(a2)}']
    for row, label, description, icon in ((a1['VMUSAVE_T'], 'Save test', 'Shared vmusave.h check', True),
                                          (a2['VMUSAVE_2'], 'No icon', 'Second card', False)):
        if row['reason']:
            problems.append(f'{row["filename"]}: {row["reason"]}')
            continue
        data = base64.b64decode(row['data'])
        meta = header_metadata(data[:128])
        if meta != {'vmu_label': label, 'description': description, 'application_id': 'DCSAVETEST'}:
            problems.append(f'{row["filename"]}: header {meta}')
        if row['has_icon'] != icon:
            problems.append(f'{row["filename"]}: has_icon {row["has_icon"]}')
        payload = data[128 + (512 if icon else 0):]
        if payload[:100] != bytes((200 - i) & 255 for i in range(100)) or any(payload[100:]):
            problems.append(f'{row["filename"]}: payload differs')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log-dir', required=True, type=Path)
    parser.add_argument('--flycast', type=Path, default=Path.home() /
                        '.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast')
    parser.add_argument('--kos-env', type=Path, default=Path.home() /
                        '.local/share/dreamcast/kos/environ.sh')
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vmusave-checks-') as directory:
        work = Path(directory)
        # Flycast moves a real card into a custom VMU folder that lacks it and
        # deletes the original, so give every slot a blank card of its own.
        (work / 'vmus').mkdir()
        for slot in ('A1', 'A2', 'B1', 'B2', 'C1', 'C2', 'D1', 'D2'):
            (work / 'vmus' / f'vmu_save_{slot}.bin').write_bytes(bytes(131072))
        (work / 'tests').mkdir()
        for name in ('vmusave.h', 'tests/vmusave_checks.c'):
            shutil.copyfile(COMMON / name, work / name)
        (work / 'tests' / 'Makefile').write_text('''TARGET = checks.elf
OBJS = vmusave_checks.o
KOS_BUILD_SUBARCHS = pristine
all: $(TARGET)
include $(KOS_BASE)/Makefile.rules
$(TARGET): $(OBJS)
\tkos-cc -o $@ $(OBJS)
''')
        build = work / 'tests'
        with (args.log_dir / 'build.log').open('w') as output:
            subprocess.run(['bash', '-c', 'source "$1" && make -C "$2" && '
                            'file "$2/checks.elf" && sh-elf-readelf -h "$2/checks.elf"',
                            'vmusave-checks', str(args.kos_env.resolve()), str(build)],
                           check=True, stdout=output, stderr=output)
        # A controller in port A with VMUs in both of its slots.
        config = ('network:EmulateBBA=no,network:DCNet=no,audio:backend=null,'
                  'config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no,'
                  'input:device1=0,input:device1.1=1,input:device1.2=1,'
                  f'config:Dreamcast.VMUPath={work / "vmus"},config:PerGameVmu=no')
        serial = args.log_dir / 'flycast.log'
        marker = 'VMUSAVE CHECKS PASSED'
        with serial.open('w') as output:
            process = subprocess.Popen([str(args.flycast.resolve()), '-config', config,
                                        str(build / 'checks.elf')], cwd=work,
                                       stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    log = serial.read_text(errors='replace')
                    if marker in log or 'assertion' in log.lower() or process.poll() is not None:
                        break
                    time.sleep(0.25)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        passed = marker in serial.read_text(errors='replace')
        problems = check_cards(work / 'vmus') if passed else []
        for problem in problems:
            print('Card check:', problem)
        passed = passed and not problems
        summary = f'{"PASS" if passed else "FAIL"}: vmusave card choice, header, icon, overwrite and full-card checks\n'
        (args.log_dir / 'result.txt').write_text(summary)
        print(summary, end='')
        return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
