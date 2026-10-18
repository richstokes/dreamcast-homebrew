"""Cut an edit decision list into one video.

usage: edit.py <edl.json> <out.mp4> [--preview]

edl.json: {"xfade": 0.4, "clips": [{"src": "...mkv", "start": 12.3, "dur": 8,
            "overlays": [{"png": "cards/cap-beach.png", "at": 0.5, "dur": 4}],
            "xfade": 0.4 (into the next clip; 0 for a hard cut)}]}

Each clip is cut (frame-accurate re-encode) with its overlays faded in and
out, then the clips are joined with video and audio crossfades, the start
fades up from black, the end fades out, and the audio is loudness-normalised
for YouTube (-14 LUFS). Output: H.264 High 1080p60, AAC 320k, faststart."""
import json, subprocess, sys
from pathlib import Path

FADE = 0.35   # overlay fade in/out


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(' '.join(map(str, cmd)))
        print(r.stderr[-3000:])
        sys.exit(1)
    return r


def cut(i, c, tmp, base):
    out = tmp / f'clip{i:03d}.mkv'
    key = json.dumps(c, sort_keys=True)
    stamp = tmp / f'clip{i:03d}.json'
    if out.exists() and stamp.exists() and stamp.read_text() == key:
        return out
    cmd = ['ffmpeg', '-v', 'error', '-y', '-ss', f"{c['start']:.3f}", '-t', f"{c['dur']:.3f}", '-i', c['src']]
    chain = '[0:v]setpts=PTS-STARTPTS,fps=60'
    graph, last = [], 'v0'
    graph.append(f'{chain}[v0]')
    for j, o in enumerate(c.get('overlays', [])):
        cmd += ['-loop', '1', '-t', f"{c['dur']:.3f}", '-i', str(base / o['png'])]
        a, d = o['at'], o['dur']
        graph.append(f'[{j + 1}:v]format=rgba,fade=in:st={a}:d={FADE}:alpha=1,'
                     f'fade=out:st={a + d - FADE}:d={FADE}:alpha=1[o{j}]')
        graph.append(f'[{last}][o{j}]overlay=0:0:shortest=1[v{j + 1}]')
        last = f'v{j + 1}'
    graph.append('[0:a]asetpts=PTS-STARTPTS,aresample=48000:async=1,'
                 f"apad,atrim=0:{c['dur']:.3f}[a]")
    cmd += ['-filter_complex', ';'.join(graph), '-map', f'[{last}]', '-map', '[a]',
            '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '10', '-pix_fmt', 'yuv420p',
            '-c:a', 'pcm_s16le', '-ar', '48000', '-ac', '2', str(out)]
    run(cmd)
    stamp.write_text(key)
    return out


def main():
    edl_path, out = Path(sys.argv[1]), Path(sys.argv[2])
    preview = '--preview' in sys.argv
    edl = json.loads(edl_path.read_text())
    base = edl_path.parent
    tmp = base / ('tmp-' + edl_path.stem)
    tmp.mkdir(exist_ok=True)
    clips = edl['clips']
    files = [cut(i, c, tmp, base) for i, c in enumerate(clips)]
    default_x = edl.get('xfade', 0.4)
    cmd = ['ffmpeg', '-v', 'error', '-y']
    for f in files:
        cmd += ['-i', str(f)]
    graph = []
    vlast, alast = '0:v', '0:a'
    t = clips[0]['dur']
    for i in range(1, len(files)):
        x = clips[i - 1].get('xfade', default_x)
        if x <= 0:
            x = 1 / 60   # a cut: the shortest crossfade
        off = t - x
        graph.append(f'[{vlast}][{i}:v]xfade=transition=fade:duration={x:.4f}:offset={off:.4f}[xv{i}]')
        graph.append(f'[{alast}][{i}:a]acrossfade=d={x:.4f}:c1=tri:c2=tri[xa{i}]')
        vlast, alast = f'xv{i}', f'xa{i}'
        t = off + clips[i]['dur']
    total = t
    end_fade = edl.get('end_fade', 1.2)
    graph.append(f'[{vlast}]fade=in:st=0:d=0.6,fade=out:st={total - end_fade:.3f}:d={end_fade},'
                 f'format=yuv420p[vout]')
    graph.append(f'[{alast}]afade=in:st=0:d=0.4,afade=out:st={total - end_fade:.3f}:d={end_fade},'
                 'loudnorm=I=-14:TP=-1.5:LRA=11,aresample=48000[aout]')
    cmd += ['-filter_complex', ';'.join(graph), '-map', '[vout]', '-map', '[aout]']
    if preview:
        cmd += ['-c:v', 'libx264', '-preset', 'veryfast', '-crf', '24', '-s', '960x540']
    else:
        cmd += ['-c:v', 'libx264', '-preset', 'slow', '-crf', '16', '-profile:v', 'high', '-level', '4.2',
                '-g', '30', '-bf', '2', '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709',
                '-color_range', 'tv']
    cmd += ['-r', '60', '-c:a', 'aac', '-b:a', '320k', '-ar', '48000', '-movflags', '+faststart', str(out)]
    run(cmd)
    print(f'{out}: {total:.1f} s, {len(files)} clips')


if __name__ == '__main__':
    main()
