#!/usr/bin/env python3
"""Cut film takes into a trailer on the beat of the game's music.

usage: cut.py <edl.json> <out.mp4>

edl.json: {"takes": ["...mkv"], "bpm": 162, "sfx_db": 14, "music_db": -7,
           "music": {"drums": "mus_drums.wav", "bass": ..., "brass": ...},
           "brass_from_beat": 16,
           "cuts": [["scene", take, start seconds, beats], ...]}

Straight cuts only, each a whole number of beats long. The takes carry the
game's sound effects (film builds mute the music, which scenes starting and
stopping would chop up); the music's own loops go under the whole cut, or
leave "music" out for the effects alone. OBS
records 1920x1080 with the 4:3 game pillarboxed: the output is the game's
1440x1080. Output: H.264 1080p60, AAC 320k, -14 LUFS, faststart.
"""
import json, subprocess, sys, tempfile
from pathlib import Path


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(' '.join(map(str, cmd)) + '\n' + r.stderr[-3000:])


def main():
    edl = json.loads(Path(sys.argv[1]).read_text())
    out, base = Path(sys.argv[2]), Path(sys.argv[1]).resolve().parent
    beat = 60.0 / edl['bpm']
    tmp = Path(tempfile.mkdtemp(prefix='klcut'))
    clips, total = [], 0.0
    for i, (name, take, start, beats) in enumerate(edl['cuts']):
        # Whole frames, so the cuts stay on the beat over the length of the edit.
        end = round((total + beats * beat) * 60) / 60
        dur = end - total
        clip = tmp / f'c{i:02d}.mkv'
        run(['ffmpeg', '-v', 'error', '-y', '-ss', f'{start:.3f}', '-t', f'{dur:.4f}', '-i', edl['takes'][take],
             '-vf', 'crop=1440:1080:240:0,fps=60,setpts=PTS-STARTPTS', '-af', f'asetpts=PTS-STARTPTS,apad,atrim=0:{dur:.4f}',
             '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '12', '-pix_fmt', 'yuv420p', '-c:a', 'pcm_s16le', '-ar', '48000', str(clip)])
        clips.append(clip)
        total = end
        print(f'{i:2d} {name:14s} {dur:.2f} s')
    lst = tmp / 'list.txt'
    lst.write_text(''.join(f"file '{c}'\n" for c in clips))
    joined = tmp / 'joined.mkv'
    run(['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(lst), '-c', 'copy', str(joined)])
    m = edl.get('music')
    late = edl.get('brass_from_beat', 0) * beat
    cmd = ['ffmpeg', '-v', 'error', '-y', '-i', str(joined)]
    tail = (f"alimiter=limit=0.89,loudnorm=I=-14:TP=-1.5:LRA=11,afade=t=out:st={total - 0.25:.3f}:d=0.25[a]")
    if m:
        for k in ('drums', 'bass', 'brass'):
            cmd += ['-stream_loop', '-1', '-i', str(base / m[k])]
        graph = (f"[0:a]volume={edl.get('sfx_db', 0)}dB[sfx];"
                 f"[1:a]aresample=48000,atrim=0:{total:.3f}[d];[2:a]aresample=48000,atrim=0:{total:.3f}[b];"
                 f"[3:a]aresample=48000,atrim=0:{total:.3f},volume='if(lt(t,{late:.3f}),0,1)':eval=frame[h];"
                 f"[d][b][h]amix=inputs=3:normalize=0,volume={edl.get('music_db', 0)}dB[mus];"
                 f"[sfx][mus]amix=inputs=2:normalize=0," + tail)
    else:
        # No "music" in the list: the takes' own sound only.
        graph = f"[0:a]volume={edl.get('sfx_db', 0)}dB," + tail
    cmd += ['-filter_complex', graph, '-map', '0:v', '-map', '[a]', '-t', f'{total:.3f}', '-c:v', 'libx264', '-preset', 'slow', '-crf', '17',
            '-pix_fmt', 'yuv420p', '-r', '60', '-c:a', 'aac', '-b:a', '320k', '-ar', '48000', '-movflags', '+faststart', str(out)]
    run(cmd)
    print(f'{out}: {total:.2f} s')


if __name__ == '__main__':
    main()
