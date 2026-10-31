#!/usr/bin/env python3
"""Fetch DreamKarts' music and press it into CD-DA tracks for the disc.

assets/music/tracks.txt lists the songs (sha256, file name, tempo, role,
title) in disc order; the role is theme, race or lobby and says where the
game plays the song. Each mp3 is downloaded from DK_MUSIC_URL (default
https://dcvmu.com/dreamkarts/music/) into assets/music/cache/ unless a copy
with the right sha256 is already there, or DK_MUSIC_SOURCE names a folder
holding the files (no download then). ffmpeg converts each to the 16-bit
44.1 kHz stereo WAV mkdcdisc takes, cdda/NN-<name>.wav, levelled to one
loudness (TARGET_LUFS, measured with its EBU R128 meter, peaks held under
PEAK_DB) so no song plays quieter than the rest, and the song table the
game shows in its sound test is written to assets/generated/music_tracks.h
(title, length and tempo per track).

Standard library only, so CI runs it with plain python3; locally `make
music` (or any target that needs the disc) runs it through uv.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import wave
from pathlib import Path

DEFAULT_URL = "https://dcvmu.com/dreamkarts/music/"
RATE = 44100
# The loudest of the songs sit at -9 LUFS with peaks at full scale; the
# quiet ones come up to meet them, a limiter catching what would clip.
TARGET_LUFS = -9.5
PEAK_DB = -1.0


ROLES = ("theme", "race", "lobby")


class Track:
    def __init__(self, sha256, name, bpm, role, title):
        self.sha256, self.name, self.bpm, self.role, self.title = sha256, name, bpm, role, title
        self.stem = name.rsplit(".", 1)[0]


def read_manifest(path):
    tracks = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 5 or len(parts[0]) != 64 or parts[3] not in ROLES:
            raise SystemExit(f"{path}: bad line: {line}")
        tracks.append(Track(parts[0], parts[1], float(parts[2]), parts[3], " ".join(parts[4:])))
    if not tracks:
        raise SystemExit(f"{path}: no tracks")
    if not any(t.role == "theme" for t in tracks):
        raise SystemExit(f"{path}: no theme track")
    return tracks


def sha256_of(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(track, cache, base_url, source):
    """The mp3 in the cache with the manifest's sha256; downloaded or copied in if not."""
    dest = cache / track.name
    if dest.exists() and sha256_of(dest) == track.sha256:
        return dest
    cache.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkstemp(dir=cache, prefix=".", suffix=".part")[1])
    try:
        if source:
            print(f"music: copying {track.name} from {source}")
            shutil.copyfile(source / track.name, tmp)
        else:
            url = base_url + track.name
            print(f"music: downloading {url}")
            req = urllib.request.Request(url, headers={"User-Agent": "dreamkarts-build"})
            with urllib.request.urlopen(req, timeout=60) as r, tmp.open("wb") as f:
                shutil.copyfileobj(r, f)
        got = sha256_of(tmp)
        if got != track.sha256:
            raise SystemExit(f"music: {track.name} has sha256 {got}, the manifest expects {track.sha256}")
        os.replace(tmp, dest)
    finally:
        if tmp.exists():
            tmp.unlink()
    return dest


def measure(mp3, ffmpeg):
    """Integrated loudness (LUFS) and true peak (dBTP) from ffmpeg's EBU R128 meter."""
    out = subprocess.run([ffmpeg, "-nostats", "-i", str(mp3), "-vn", "-af", "ebur128=peak=true", "-f", "null", "-"],
                         capture_output=True, text=True, check=True).stderr
    lufs = peak = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("I:") and line.endswith("LUFS"):
            lufs = float(line.split()[1])
        elif line.startswith("Peak:") and line.endswith("dBFS"):
            peak = float(line.split()[1])
    if lufs is None or peak is None:
        raise SystemExit(f"music: could not read the loudness of {mp3.name}")
    return lufs, peak


def convert(mp3, wav, ffmpeg):
    """CD-DA audio as mkdcdisc wants it: stereo, 44.1 kHz, 16-bit, no
    metadata chunks, brought to TARGET_LUFS with peaks held under PEAK_DB."""
    lufs, peak = measure(mp3, ffmpeg)
    gain = TARGET_LUFS - lufs
    filters = [f"volume={gain:.2f}dB"]
    limited = peak + gain > PEAK_DB
    if limited:
        filters.append(f"alimiter=limit={10 ** (PEAK_DB / 20):.4f}:attack=5:release=50:level=false")
    print(f"music:   {lufs:6.1f} LUFS, peak {peak:5.1f} dBTP: gain {gain:+.1f} dB{' + limiter' if limited else ''}")
    wav.parent.mkdir(parents=True, exist_ok=True)
    tmp = wav.with_suffix(".part.wav")
    subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(mp3), "-vn", "-af", ",".join(filters), "-ac", "2",
                    "-ar", str(RATE), "-sample_fmt", "s16", "-map_metadata", "-1", "-fflags", "+bitexact", "-f", "wav",
                    str(tmp)], check=True)
    os.replace(tmp, wav)


def wav_seconds(path):
    with wave.open(str(path), "rb") as w:
        if w.getnchannels() != 2 or w.getframerate() != RATE or w.getsampwidth() != 2:
            raise SystemExit(f"{path}: not 16-bit 44.1 kHz stereo")
        return w.getnframes() / RATE


def write_header(path, tracks, seconds):
    h = ["/* Generated by tools/build_music.py from assets/music/tracks.txt. Do not edit by hand. */",
         "#ifndef DK_MUSIC_TRACKS_H", "#define DK_MUSIC_TRACKS_H", "",
         "/* The disc's CD-DA tracks in order. */",
         f"#define DK_MUSIC_TRACKS {len(tracks)}",
         f"#define DK_MUSIC_TITLE_TRACK {next(i for i, t in enumerate(tracks) if t.role == 'theme')}",
         "/* Where each song plays: the title screen, the races' shuffle or the online lobby's. */",
         "typedef enum { DK_MUSIC_THEME, DK_MUSIC_RACE, DK_MUSIC_LOBBY } dk_music_role_t;",
         "static const uint8_t dk_music_role[DK_MUSIC_TRACKS] __attribute__((unused)) = {"
         + ", ".join(f"DK_MUSIC_{t.role.upper()}" for t in tracks) + "};",
         "static const char *const dk_music_title[DK_MUSIC_TRACKS] __attribute__((unused)) = {"]
    h += [f'    "{t.title}",' for t in tracks]
    h += ["};", "/* Length in seconds. */",
          "static const float dk_music_secs[DK_MUSIC_TRACKS] __attribute__((unused)) = {"
          + ", ".join(f"{s:.1f}f" for s in seconds) + "};",
          "/* Rough tempo, for the sound test's beat. */",
          "static const float dk_music_bpm[DK_MUSIC_TRACKS] __attribute__((unused)) = {"
          + ", ".join(f"{t.bpm:.1f}f" for t in tracks) + "};",
          "", "#endif /* DK_MUSIC_TRACKS_H */", ""]
    text = "\n".join(h)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
        print(f"music: wrote {path}")


def main():
    project = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--project-dir", type=Path, default=project)
    ap.add_argument("--url", default=os.environ.get("DK_MUSIC_URL", DEFAULT_URL),
                    help="where the mp3s are downloaded from (DK_MUSIC_URL)")
    ap.add_argument("--source", type=Path, default=os.environ.get("DK_MUSIC_SOURCE") or None,
                    help="a folder already holding the mp3s, copied instead of downloaded (DK_MUSIC_SOURCE)")
    args = ap.parse_args()
    project = args.project_dir
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("music: ffmpeg not found (brew install ffmpeg, or apt-get install ffmpeg)")
    tracks = read_manifest(project / "assets/music/tracks.txt")
    cache = project / "assets/music/cache"
    cdda = project / "cdda"
    base_url = args.url if args.url.endswith("/") else args.url + "/"
    seconds = []
    fresh = 0
    for i, t in enumerate(tracks, 1):
        wav = cdda / f"{i:02d}-{t.stem}.wav"
        if not wav.exists() or wav.stat().st_mtime < Path(__file__).stat().st_mtime:
            mp3 = fetch(t, cache, base_url, args.source)
            print(f"music: track {i:2d} {t.title}: {t.name} -> {wav.name}")
            convert(mp3, wav, ffmpeg)
            fresh += 1
        seconds.append(wav_seconds(wav))
    # A track dropped from the manifest must not stay on the disc.
    for stale in cdda.glob("*.wav") if cdda.is_dir() else []:
        if stale.name not in {f"{i:02d}-{t.stem}.wav" for i, t in enumerate(tracks, 1)}:
            stale.unlink()
            print(f"music: removed {stale.name}")
    write_header(project / "assets/generated/music_tracks.h", tracks, seconds)
    total = sum(seconds)
    print(f"music: {len(tracks)} CD-DA tracks in {cdda.relative_to(project)}/, {fresh} converted, "
          f"{total / 60:.1f} min, {total * RATE * 4 / 2**20:.0f} MiB on the disc")


if __name__ == "__main__":
    main()
