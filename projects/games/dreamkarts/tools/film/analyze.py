"""Races and highlight events from a film run's log, plus contact sheets.

usage: uv run --with pillow --with matplotlib tools/film/analyze.py <run name> [--every 2] [--sheets] [--until secs]
Reads captures/film/runs/<name>.log and .path; writes captures/film/sheets/<name>-NN.jpg,
each a grid of frames labelled with their time in the recording."""
import re, subprocess, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

S = Path(__file__).resolve().parents[2] / 'captures' / 'film'
name = sys.argv[1]
every = float(sys.argv[sys.argv.index('--every') + 1]) if '--every' in sys.argv else 2.0
log = [(float(l[:9]), l[10:].strip()) for l in (S / 'runs' / f'{name}.log').read_text().splitlines()]
src = (S / 'runs' / f'{name}.path').read_text().strip()

races, cur = [], None
for t, line in log:
    m = re.match(r'DK FILM state (\w+) track (\d) (.+) players (\d)', line)
    if m:
        st, track, tname, pl = m.group(1), int(m.group(2)), m.group(3), int(m.group(4))
        if st in ('intro', 'countdown') and (cur is None or cur.get('end')):
            cur = {'track': tname, 'players': pl, 'start': t, 'events': []}
            races.append(cur)
        elif st == 'race' and (cur is None or cur.get('end')):
            cur = {'track': tname, 'players': pl, 'start': t, 'events': [], 'joined_late': True}
            races.append(cur)
        if cur and st == 'race' and 'go' not in cur:
            cur['go'] = t
        if cur and st == 'results' and 'end' not in cur:
            cur['results'] = t
        if cur and st not in ('intro', 'countdown', 'race', 'results') and 'results' in cur and 'end' not in cur:
            cur['end'] = t
        continue
    if cur and not cur.get('end') and not line.startswith('DK FILM bump'):
        cur['events'].append((t, line[8:]))

for i, r in enumerate(races):
    ev = r['events']
    kinds = {}
    for _, e in ev:
        k = e.split()[0]
        kinds[k] = kinds.get(k, 0) + 1
    print(f"race {i}: {r['track']} {r['players']}P start {r['start']:.1f} go {r.get('go', 0):.1f} "
          f"results {r.get('results', 0):.1f} end {r.get('end', 0):.1f} {'(joined late) ' if r.get('joined_late') else ''}{kinds}")
    for t, e in ev:
        if e.startswith(('cam', 'lap', 'fall', 'finish', 'dolphins', 'pass')) and (not e.startswith('pass') or e.endswith('to 1') or e.endswith('to 2')):
            print(f'   {t:7.1f} {e}')

if '--sheets' in sys.argv:
    out = S / 'sheets'
    out.mkdir(exist_ok=True)
    frames = S / 'frames' / name
    frames.mkdir(parents=True, exist_ok=True)
    if not any(frames.iterdir()):
        until = ['-t', sys.argv[sys.argv.index('--until') + 1]] if '--until' in sys.argv else []
        subprocess.run(['nice', '-n', '19', 'ffmpeg', '-v', 'error', '-threads', '3', *until, '-i', src, '-vf', f'fps=1/{every},crop=1440:1080:240:0,scale=320:240',
                        '-q:v', '4', str(frames / '%05d.jpg')], check=True)
    files = sorted(frames.glob('*.jpg'))
    import matplotlib
    font = ImageFont.truetype(str(Path(matplotlib.get_data_path()) / 'fonts/ttf/DejaVuSans-Bold.ttf'), 22)
    cols, rows = 6, 5
    per = cols * rows
    for k in range(0, len(files), per):
        sheet = Image.new('RGB', (cols * 320, rows * 240))
        d = ImageDraw.Draw(sheet)
        for j, f in enumerate(files[k:k + per]):
            x, y = (j % cols) * 320, (j // cols) * 240
            sheet.paste(Image.open(f), (x, y))
            t = (k + j) * every
            d.text((x + 6, y + 4), f'{t:.0f}s', font=font, fill=(255, 255, 0), stroke_width=2, stroke_fill=(0, 0, 0))
        sheet.save(out / f'{name}-{k // per:02d}.jpg', quality=80)
    print('sheets', (len(files) + per - 1) // per)
