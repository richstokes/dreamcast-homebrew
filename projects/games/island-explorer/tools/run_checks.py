# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Run the existing diagnostic ELFs serially and retain ignored log evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FLYCAST = Path.home() / '.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast'
if not DEFAULT_FLYCAST.is_file():
    DEFAULT_FLYCAST = Path('/Applications/Flycast.app/Contents/MacOS/Flycast')


def run(flycast, elf, name, success, timeout=180):
    log = ROOT / 'build' / f'{name}.log'
    log.parent.mkdir(exist_ok=True)
    with log.open('w') as output:
        process = subprocess.Popen([str(flycast), '-config',
            'config:Debug.SerialConsoleEnabled=yes', str(ROOT / elf)],
            stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                text = log.read_text(errors='replace')
                if '[shutdown] Clean exit' in text:
                    break
                if process.poll() is not None:
                    break
                time.sleep(.2)
            else:
                raise RuntimeError(f'{name} timed out; inspect {log}')
            text = log.read_text(errors='replace')
            if success not in text or '[shutdown] Clean exit' not in text:
                raise RuntimeError(f'{name} did not complete successfully; inspect {log}')
            if re.search(r'\[test\] FAIL|\[math\] FAIL|panic|stream underrun', text, re.I):
                raise RuntimeError(f'{name} reported a runtime failure; inspect {log}')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    print(f'{name}: PASS ({log})', flush=True)
    return text


def benchmark(text):
    rows = []
    for match in re.finditer(r'\[bench\] stop=(\d+) view=(\d+) frames=(\d+) '
                            r'frame_us=(\d+) render_us=(\d+) camera_us=(\d+) '
                            r'tris=(\d+) batches=(\d+)', text):
        rows.append(dict(zip(('stop','view','frames','frame_us','render_us',
                              'camera_us','tris','batches'), map(int,match.groups()))))
    if len(rows) != 24 or len({(r['stop'],r['view']) for r in rows}) != 24:
        raise RuntimeError('Incomplete benchmark: expected 24 distinct camera poses')
    frames = sum(r['frames'] for r in rows)
    return dict(poses=rows, frames=frames,
                frame_ms=sum(r['frame_us'] for r in rows)/frames/1000,
                render_ms=sum(r['render_us'] for r in rows)/frames/1000,
                camera_ms=sum(r['camera_us'] for r in rows)/frames/1000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--flycast', type=Path,
                        default=Path(os.environ.get('FLYCAST_BIN', DEFAULT_FLYCAST)))
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--math', action='store_true')
    parser.add_argument('--traversal', action='store_true')
    args = parser.parse_args()
    if not any((args.benchmark,args.math,args.traversal)):
        parser.error('Choose --benchmark, --math and/or --traversal')
    if args.math:
        run(args.flycast,'render-math-qa.elf','math-qa','[math] PASS')
    if args.benchmark:
        reference = benchmark(run(args.flycast,'island-explorer-reference.elf',
                                  'benchmark-reference','[bench] DONE mode=reference'))
        fast = benchmark(run(args.flycast,'island-explorer-bench.elf',
                             'benchmark-sh4zam','[bench] DONE mode=sh4zam'))
        deltas = [dict(stop=a['stop'],view=a['view'],tris=b['tris']-a['tris'],
                       batches=b['batches']-a['batches'])
                  for a,b in zip(reference['poses'],fast['poses'])
                  if a['tris']!=b['tris'] or a['batches']!=b['batches']]
        report = dict(reference=reference,sh4zam=fast,geometry_deltas=deltas,
                      render_reduction_percent=100*(1-fast['render_ms']/reference['render_ms']))
        report['sh4zam_revision'] = subprocess.check_output(
            ['git','-C',str(ROOT/'third_party/sh4zam'),'rev-parse','HEAD'],text=True).strip()
        report['elf_sha256'] = {name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
                               for name in ('island-explorer-reference.elf','island-explorer-bench.elf')}
        (ROOT/'build/benchmark.json').write_text(json.dumps(report,indent=2)+'\n')
        print(f"Render CPU: {reference['render_ms']:.3f} -> {fast['render_ms']:.3f} ms "
              f"({report['render_reduction_percent']:.1f}% less); "
              f"geometry differences: {len(deltas)}",flush=True)
        if deltas:
            raise RuntimeError('Benchmark geometry differs; inspect build/benchmark.json before accepting timings')
    if args.traversal:
        run(args.flycast,'island-explorer-test.elf','traversal-sh4zam',
            '[test] PASS: traversal complete, 0 failures')


if __name__ == '__main__':
    main()
