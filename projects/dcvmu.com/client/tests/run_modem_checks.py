#!/usr/bin/env python3
"""Run SH-4 modem settings/failure checks without dialing or touching real VMUs."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

CLIENT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log-dir', required=True, type=Path)
    parser.add_argument('--flycast', type=Path, default=Path.home() /
                        '.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast')
    parser.add_argument('--kos-env', type=Path, default=Path.home() /
                        '.local/share/dreamcast/kos/environ.sh')
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dcvmu-modem-checks-') as directory:
        work = Path(directory)
        (work / 'tests').mkdir()
        (work / 'vmus').mkdir()
        for name in ('modem.c', 'client.h', 'tests/modem_settings.c'):
            shutil.copyfile(CLIENT / name, work / name)
        (work / 'Makefile').write_text('''TARGET = checks.elf
OBJS = tests/modem_settings.o
KOS_BUILD_SUBARCHS = pristine
all: $(TARGET)
include $(KOS_BASE)/Makefile.rules
$(TARGET): $(OBJS)
\tkos-cc -o $@ $(OBJS)
''')
        with (args.log_dir / 'build.log').open('w') as output:
            subprocess.run(['bash', '-c', 'source "$1" && make -C "$2" && '
                            'file "$2/checks.elf" && sh-elf-readelf -h "$2/checks.elf"',
                            'modem-checks', str(args.kos_env.resolve()), str(work)],
                           check=True, stdout=output, stderr=output)
        config = ('network:EmulateBBA=no,network:DCNet=no,audio:backend=null,'
                  'config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no,'
                  f'config:Dreamcast.VMUPath={work / "vmus"},config:PerGameVmu=no')
        serial = args.log_dir / 'flycast.log'
        marker = 'MODEM SETTINGS AND FAILURE CHECKS PASSED'
        with serial.open('w') as output:
            process = subprocess.Popen([str(args.flycast.resolve()), '-config', config,
                                        str(work / 'checks.elf')], cwd=work,
                                       stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 60
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
        summary = f'{"PASS" if passed else "FAIL"}: modem settings and failure checks\n'
        (args.log_dir / 'result.txt').write_text(summary)
        print(summary, end='')
        return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
