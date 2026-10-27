#!/usr/bin/env python3
"""Record a KOS_Test auto run in benchmark_results/.

Reads the last KT RESULTS block from a dc-tool-ip or Flycast log, writes the
run out as runs/<date>_<target>_kos-<commit>.txt, adds a row to
benchmarks.csv and compares the benchmarks with the previous run on the same
target. A change only counts when it is bigger than that benchmark's noise:
the spread between passes in either run, or between earlier runs on the
same KOS build, and never less than 2% (code layout alone moves some
benchmarks that much between builds).

    tools/record_results.py build/auto/console.log
    tools/record_results.py --dry-run build/auto/console.log    # compare only
    tools/record_results.py --all ...                           # list every benchmark
"""
import argparse
import csv
from datetime import datetime
from pathlib import Path
import sys

PROJECT = Path(__file__).resolve().parents[1]
META_COLUMNS = ['date', 'target', 'kos', 'kos_commit', 'kos_patches', 'gcc', 'kos_test_commit',
                'cpu_mhz', 'passes', 'pass', 'fail', 'skip', 'run_file']
FLOOR_PCT = 2.0


def parse(log_text):
    """The last results block as (meta, tests, benches), or None."""
    lines = [line.rstrip('\r') for line in log_text.splitlines()]
    starts = [i for i, line in enumerate(lines) if line.startswith('KT RESULTS BEGIN')]
    if not starts:
        return None
    meta, tests, benches = {}, [], []
    for line in lines[starts[-1] + 1:]:
        if line.startswith('KT RESULTS END'):
            return meta, tests, benches
        if not line.startswith('KT '):
            continue    # Flycast's own log lines can land in the block
        body = line[3:]
        if body.startswith('TEST '):
            status, name, detail = (body[5:].split('|', 2) + ['', ''])[:3]
            tests.append((status, name, detail))
        elif body.startswith('BENCH '):
            # key|median|unit|name, then (format 2) spread %|samples|hi or lo,
            # then (format 3) each pass's value
            f = body[6:].split('|') + [''] * 7
            benches.append({'key': f[0], 'value': float(f[1]), 'unit': f[2], 'name': f[3],
                            'spread': float(f[4] or 0), 'samples': int(f[5] or 1),
                            'lower': f[6] == 'lo', 'passes': f[7]})
        else:
            key, _, value = body.partition(' ')
            meta[key] = value
    return None     # cut off before the end


def summary_counts(meta):
    counts = dict(part.split('=', 1) for part in meta.get('summary', '').split() if '=' in part)
    return counts.get('pass', ''), counts.get('fail', ''), counts.get('skip', ''), counts.get('ms', '')


def run_text(meta, tests, benches, when):
    passed, failed, skipped, ms = summary_counts(meta)
    seconds = f'{int(ms) / 1000:.1f} s' if ms.isdigit() else '?'
    mhz = meta.get('cpu_mhz', 'n/a')
    out = [
        'KOS_Test results',
        '',
        f'Date        {when:%Y-%m-%d %H:%M}',
        f'Target      {meta.get("target", "?")}' + ('' if mhz == 'n/a' else f' ({mhz} MHz)'),
        f'KOS         {meta.get("kos", "?")} {meta.get("kos_commit", "?")}'
        f' (local changes {meta.get("kos_patches", "?")})',
        f'GCC         {meta.get("gcc", "?")}',
        f'KOS_Test    {meta.get("kos_test_commit", "?")}, built {meta.get("built", "?")}',
        f'Loader      {meta.get("loader", "?")}',
        f'Video       {meta.get("video", "?")}',
        f'Hot-plug    {meta.get("maple_events", "?")}',
        f'Summary     {passed} pass, {failed} fail, {skipped} skip in {seconds}',
        '',
        'Tests',
    ]
    out += [f'{status:<5} {name:<30} {detail}' for status, name, detail in tests]
    out += ['', f'Benchmarks (median of {meta.get("passes", "1")} passes, spread between passes)']
    for b in benches:
        noise = f'+/-{b["spread"] / 2:.1f}%' if b['samples'] > 1 else ''
        better = 'lower is better' if b['lower'] else ''
        passes = f' [{b["passes"]}]' if b['samples'] > 1 and b['passes'] else ''
        out.append(f'{b["key"]:<13} {b["value"]:>12.3f} {b["unit"]:<10} {noise:<9} {b["name"]}'
                   + (f' ({better})' if better else '') + passes)
    return '\n'.join(out) + '\n'


def number(row, key):
    try:
        return float(row.get(key) or 'nan')
    except ValueError:
        return float('nan')


def build_spread(rows, previous, key):
    """Spread (%) of key across the rows on the previous run's KOS build."""
    same = [number(row, key) for row in rows
            if all(row.get(k) == previous.get(k) for k in ('target', 'kos_commit', 'kos_patches'))]
    same = sorted(v for v in same if v == v)
    if len(same) < 2 or same[len(same) // 2] == 0:
        return 0.0
    return (same[-1] - same[0]) / same[len(same) // 2] * 100


def compare(rows, previous, benches, show_all):
    """Lines comparing benches with a previous CSV row, worst first (only
    those beyond their noise unless show_all), and how many moved each way."""
    lines, slower, faster = [], 0, 0
    for b in benches:
        old = number(previous, b['key'])
        if old != old or old == 0:
            continue
        pct = (b['value'] - old) / old * 100
        gain = -pct if b['lower'] else pct
        noise = max(FLOOR_PCT, b['spread'], number(previous, b['key'] + '_spread') or 0,
                    build_spread(rows, previous, b['key']))
        noise = noise if noise == noise else FLOOR_PCT
        verdict = ''
        if gain < -noise:
            verdict, slower = 'SLOWER', slower + 1
        elif gain > noise:
            verdict, faster = 'FASTER', faster + 1
        if verdict or show_all:
            lines.append((gain, f'  {b["key"]:<13} {old:>11.3f} -> {b["value"]:>11.3f} {b["unit"]:<10}'
                                f' {pct:+6.1f}%  noise {noise:4.1f}%  {verdict}'.rstrip()))
    return [text for _, text in sorted(lines)], slower, faster


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('log', type=Path)
    parser.add_argument('--out', type=Path, default=PROJECT / 'benchmark_results',
                        help='results folder (default: benchmark_results)')
    parser.add_argument('--dry-run', action='store_true', help='compare, write nothing')
    parser.add_argument('--all', action='store_true', help='list every benchmark, not just changes')
    args = parser.parse_args()

    parsed = parse(args.log.read_text(errors='replace'))
    if not parsed:
        sys.exit(f'{args.log}: no complete KT RESULTS block')
    meta, tests, benches = parsed
    when = datetime.now()
    target = meta.get('target', 'unknown')
    commit = meta.get('kos_commit', 'unknown')

    csv_path = args.out / 'benchmarks.csv'
    rows, columns = [], list(META_COLUMNS)
    if csv_path.exists():
        with csv_path.open(newline='') as f:
            reader = csv.DictReader(f)
            rows = list(reader)
            columns += [c for c in reader.fieldnames or [] if c not in columns]
    columns += [b['key'] for b in benches if b['key'] not in columns]
    columns += [b['key'] + '_spread' for b in benches if b['key'] + '_spread' not in columns]

    previous = next((row for row in reversed(rows) if row.get('target') == target), None)
    if previous:
        print(f'Against the last {target} run ({previous["date"]}, KOS {previous["kos_commit"]}'
              f' + {previous["kos_patches"]}):')
        lines, slower, faster = compare(rows, previous, benches, args.all)
        if lines:
            print('\n'.join(lines))
        print(f'  {slower} slower, {faster} faster beyond their noise; the rest within it')
    else:
        print(f'First {target} run recorded here.')
    passed, failed, skipped, _ = summary_counts(meta)
    print(f'Tests: {passed} pass, {failed} fail, {skipped} skip')
    for status, name, detail in tests:
        if status == 'FAIL':
            print(f'  FAIL {name}: {detail}')
    if args.dry_run:
        return

    runs = args.out / 'runs'
    runs.mkdir(parents=True, exist_ok=True)
    stem = f'{when:%Y-%m-%d}_{target}_kos-{commit}'
    run_path = runs / f'{stem}.txt'
    n = 2
    while run_path.exists():
        run_path = runs / f'{stem}-{n}.txt'
        n += 1
    run_path.write_text(run_text(meta, tests, benches, when))

    row = {'date': f'{when:%Y-%m-%d %H:%M}', 'target': target, 'kos': meta.get('kos', ''),
           'kos_commit': commit, 'kos_patches': meta.get('kos_patches', ''), 'gcc': meta.get('gcc', ''),
           'kos_test_commit': meta.get('kos_test_commit', ''), 'cpu_mhz': meta.get('cpu_mhz', ''),
           'passes': meta.get('passes', '1'), 'pass': passed, 'fail': failed, 'skip': skipped,
           'run_file': run_path.relative_to(args.out).as_posix()}
    row.update({b['key']: f'{b["value"]:.4f}' for b in benches})
    row.update({b['key'] + '_spread': f'{b["spread"]:.2f}' for b in benches if b['samples'] > 1})
    with csv_path.open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows + [row])
    print(f'Wrote {run_path} and a row in {csv_path}')


if __name__ == '__main__':
    main()
