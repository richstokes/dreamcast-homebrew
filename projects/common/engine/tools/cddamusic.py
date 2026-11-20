"""CD-DA music for the games' discs: fetch pinned mp3s and press them to WAVs.

A game's music is audio tracks on its disc, which the GD-ROM plays into the
AICA at no cost to the SH-4. The mp3s are not kept in git: a game's
tools/build_music.py lists them with their sha256 and calls press(), which
downloads each (or copies it from a folder) into the game's
assets/music/cache/, and has ffmpeg cut cdda/NN-<name>.wav from it: 16-bit
44.1 kHz stereo as mkdcdisc takes, levelled to one loudness (TARGET_LUFS by
ffmpeg's EBU R128 meter, peaks held under PEAK_DB) so no song plays quieter
than the rest. tools/flycast/elf-to-cdi.sh --cdda puts the WAVs on the disc.

Standard library only, so CI can run a game's script with plain python3.
Used by DreamKarts and Kaiju Lunch Break.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tempfile
import urllib.request
import wave
from pathlib import Path

RATE = 44100
# The loudest songs sit near -9 LUFS with peaks at full scale; quieter
# ones come up to meet them, a limiter catching what would clip.
TARGET_LUFS = -9.5
PEAK_DB = -1.0


def sha256_of(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(track, cache, base_url, source, agent):
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
            req = urllib.request.Request(url, headers={"User-Agent": agent})
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


def press(project, tracks, base_url, source, script, agent):
    """Make cdda/NN-<stem>.wav in project for each of tracks (objects with
    sha256, name, stem and title), converting those missing or older than
    script (the game's build_music.py), and drop WAVs no track owns.
    Returns each track's length in seconds."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("music: ffmpeg not found (brew install ffmpeg, or apt-get install ffmpeg)")
    cache = project / "assets/music/cache"
    cdda = project / "cdda"
    base_url = base_url if base_url.endswith("/") else base_url + "/"
    newest = max(Path(script).stat().st_mtime, Path(__file__).stat().st_mtime)
    seconds = []
    fresh = 0
    for i, t in enumerate(tracks, 1):
        wav = cdda / f"{i:02d}-{t.stem}.wav"
        if not wav.exists() or wav.stat().st_mtime < newest:
            mp3 = fetch(t, cache, base_url, source, agent)
            print(f"music: track {i:2d} {t.title}: {t.name} -> {wav.name}")
            convert(mp3, wav, ffmpeg)
            fresh += 1
        seconds.append(wav_seconds(wav))
    # A track dropped from the manifest must not stay on the disc.
    for stale in cdda.glob("*.wav") if cdda.is_dir() else []:
        if stale.name not in {f"{i:02d}-{t.stem}.wav" for i, t in enumerate(tracks, 1)}:
            stale.unlink()
            print(f"music: removed {stale.name}")
    total = sum(seconds)
    print(f"music: {len(tracks)} CD-DA tracks in {cdda.relative_to(project)}/, {fresh} converted, "
          f"{total / 60:.1f} min, {total * RATE * 4 / 2**20:.0f} MiB on the disc")
    return seconds
