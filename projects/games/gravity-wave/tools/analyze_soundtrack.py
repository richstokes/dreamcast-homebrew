#!/usr/bin/env python3
"""Quality-control analysis for Gravity Wave's rendered soundtrack.

The analyzer accepts either the JSON manifest emitted by render_soundtrack.py
or a directory containing that manifest and its WAV files.  A directory of
WAV files without a manifest is also accepted, although form, note, and chord
checks are necessarily skipped in that mode.

The report deliberately combines hard signal-integrity tests with structural
proxies.  It cannot decide whether a tune is good, but it can catch the failure
modes that made earlier soundtrack iterations feel assembled from repeated
clips: duplicated forms or melodies, abrupt section joins, identical verse
variants, and tracks whose energy/onset/melodic contours are nearly the same.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np

try:
    from scipy import signal
    from scipy.io import wavfile
except ImportError as exc:  # pragma: no cover - exercised only on misconfigured hosts
    raise SystemExit(
        "analyze_soundtrack.py requires NumPy and SciPy (scipy.io.wavfile and "
        "scipy.signal)."
    ) from exc


EPSILON = 1.0e-12
DEFAULT_BEATS_PER_BAR = 4.0
ANALYSIS_VECTOR_SIZE = 128
CHROMA_SIZE = 12


@dataclass
class Issue:
    severity: str
    code: str
    message: str
    track: str | None = None
    section: str | None = None
    details: dict[str, Any] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "severity": self.severity,
            "code": self.code,
            "message": self.message,
        }
        if self.track is not None:
            result["track"] = self.track
        if self.section is not None:
            result["section"] = self.section
        if self.details:
            result["details"] = _json_safe(self.details)
        return result


@dataclass
class Cue:
    name: str
    kind: str
    start_sample: int
    end_sample: int
    source: dict[str, Any]

    @property
    def length(self) -> int:
        return self.end_sample - self.start_sample


@dataclass
class FeatureSet:
    onset: np.ndarray
    energy: np.ndarray
    intervals: np.ndarray
    chroma: np.ndarray
    harmony: np.ndarray
    brightness: np.ndarray
    waveform_hash: str
    melody_hash: str | None
    harmony_hash: str | None


def _json_safe(value: Any) -> Any:
    """Convert NumPy values and non-finite floats into strict JSON values."""
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, np.ndarray):
        return [_json_safe(item) for item in value.tolist()]
    if isinstance(value, np.generic):
        return _json_safe(value.item())
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    return value


def _number(value: Any, default: float | None = None) -> float | None:
    if isinstance(value, bool) or value is None:
        return default
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        text = value.strip()
        try:
            return float(text)
        except ValueError:
            if ":" in text:
                pieces = text.split(":")
                try:
                    total = 0.0
                    for piece in pieces:
                        total = total * 60.0 + float(piece)
                    return total
                except ValueError:
                    return default
    return default


def _first(mapping: dict[str, Any], names: Iterable[str], default: Any = None) -> Any:
    for name in names:
        if name in mapping and mapping[name] is not None:
            return mapping[name]
    return default


def _db(value: float, reference: float = 1.0) -> float:
    return 20.0 * math.log10(max(float(value), EPSILON) / max(reference, EPSILON))


def _rms(samples: np.ndarray) -> float:
    if samples.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(samples, dtype=np.float64))))


def _normalize_vector(values: Sequence[float] | np.ndarray, size: int) -> np.ndarray:
    array = np.asarray(values, dtype=np.float64).reshape(-1)
    if array.size == 0:
        return np.zeros(size, dtype=np.float64)
    if array.size == 1:
        return np.full(size, float(array[0]), dtype=np.float64)
    old_axis = np.linspace(0.0, 1.0, array.size)
    new_axis = np.linspace(0.0, 1.0, size)
    return np.interp(new_axis, old_axis, array)


def _unit_center(values: Sequence[float] | np.ndarray) -> np.ndarray:
    array = np.asarray(values, dtype=np.float64).reshape(-1)
    if array.size == 0:
        return array
    array = array - float(np.mean(array))
    norm = float(np.linalg.norm(array))
    if norm <= EPSILON:
        return np.zeros_like(array)
    return array / norm


def _cosine_similarity(left: Sequence[float], right: Sequence[float]) -> float:
    a = np.asarray(left, dtype=np.float64).reshape(-1)
    b = np.asarray(right, dtype=np.float64).reshape(-1)
    if a.size != b.size:
        size = max(min(max(a.size, b.size), ANALYSIS_VECTOR_SIZE), 8)
        a = _normalize_vector(a, size)
        b = _normalize_vector(b, size)
    denominator = float(np.linalg.norm(a) * np.linalg.norm(b))
    if denominator <= EPSILON:
        return 1.0 if np.allclose(a, b, atol=1.0e-9) else 0.0
    return float(np.clip(np.dot(a, b) / denominator, -1.0, 1.0))


def _shifted_correlation(left: Sequence[float], right: Sequence[float]) -> float:
    """Correlation after duration normalization and a small timing allowance."""
    a = _unit_center(_normalize_vector(left, ANALYSIS_VECTOR_SIZE))
    b = _unit_center(_normalize_vector(right, ANALYSIS_VECTOR_SIZE))
    if not np.any(a) or not np.any(b):
        return 1.0 if np.allclose(a, b, atol=1.0e-9) else 0.0
    best = -1.0
    for shift in range(-6, 7):
        shifted = np.roll(b, shift)
        # Do not let wrapped samples create an artificially good match.
        if shift < 0:
            aa, bb = a[:shift], shifted[:shift]
        elif shift > 0:
            aa, bb = a[shift:], shifted[shift:]
        else:
            aa, bb = a, shifted
        if aa.size >= 16:
            best = max(best, _cosine_similarity(aa, bb))
    return float(np.clip((best + 1.0) * 0.5, 0.0, 1.0))


def _canonical_label(label: str) -> str:
    text = re.sub(r"[^a-z0-9]+", " ", label.lower()).strip()
    if "final" in text and ("chorus" in text or "hook" in text or "refrain" in text):
        return "final_chorus"
    if text.startswith("final "):
        # Some tracks name the last hook after their own concept (for example
        # "FINAL PROPHECY") instead of spelling out "final chorus".
        return "final_chorus"
    if "pre" in text and ("chorus" in text or "hook" in text):
        return "prechorus"
    if "post" in text and ("chorus" in text or "hook" in text):
        return "postchorus"
    if "chorus" in text or "hook" in text or "refrain" in text:
        return "chorus"
    if "verse" in text:
        match = re.search(r"(?:verse|v)\s*([0-9]+)", text)
        return f"verse{match.group(1)}" if match else "verse"
    if "bridge" in text or "middle 8" in text:
        return "bridge"
    if "breakdown" in text or text == "break":
        return "breakdown"
    if "build" in text or "riser" in text:
        return "build"
    if "solo" in text or "instrumental" in text:
        return "solo"
    if "intro" in text:
        return "intro"
    if "outro" in text or "coda" in text:
        return "outro"
    return re.sub(r"\s+", "_", text) or "section"


def _note_name_to_midi(value: str) -> float | None:
    match = re.fullmatch(r"\s*([A-Ga-g])([#b]?)(-?[0-9]+)\s*", value)
    if not match:
        return None
    semitones = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
    pitch_class = semitones[match.group(1).upper()]
    if match.group(2) == "#":
        pitch_class += 1
    elif match.group(2) == "b":
        pitch_class -= 1
    octave = int(match.group(3))
    return float((octave + 1) * 12 + pitch_class)


def _pitch_value(note: dict[str, Any]) -> float | None:
    raw = _first(note, ("pitch", "midi", "midi_note", "note", "key"))
    numeric = _number(raw)
    if numeric is not None:
        return numeric
    return _note_name_to_midi(str(raw)) if raw is not None else None


def _event_beat(event: dict[str, Any], bpm: float, sample_rate: int) -> float | None:
    beat = _number(_first(event, ("start_beat", "beat", "beats", "time_beats")))
    if beat is not None:
        return beat
    bar = _number(_first(event, ("start_bar", "bar")))
    if bar is not None:
        beat_in_bar = _number(_first(event, ("beat_in_bar", "bar_beat")), 0.0) or 0.0
        return bar * DEFAULT_BEATS_PER_BAR + beat_in_bar
    sample = _number(_first(event, ("start_sample", "sample", "frame")))
    if sample is not None and sample_rate > 0:
        return sample / sample_rate * bpm / 60.0
    seconds = _number(_first(event, ("start_seconds", "seconds", "time", "start")))
    if seconds is not None:
        return seconds * bpm / 60.0
    return None


def _extract_events(
    track: dict[str, Any], manifest: dict[str, Any], event_type: str
) -> list[dict[str, Any]]:
    aliases = (event_type, "note_events") if event_type == "notes" else (event_type, "chord_events")
    for alias in aliases:
        value = track.get(alias)
        if isinstance(value, list):
            return [item for item in value if isinstance(item, dict)]

    global_events = manifest.get(event_type)
    if isinstance(global_events, dict):
        keys = [str(track.get(key, "")) for key in ("id", "name", "title", "slug")]
        for key in keys:
            value = global_events.get(key)
            if isinstance(value, list):
                return [item for item in value if isinstance(item, dict)]
    return []


def _melody_metadata(
    notes: list[dict[str, Any]], bpm: float, sample_rate: int, cue: Cue | None = None
) -> tuple[np.ndarray, np.ndarray, str | None]:
    if not notes:
        return np.array([], dtype=np.float64), np.array([], dtype=np.float64), None

    melodic_words = ("lead", "melody", "hook", "vocal", "sax", "guitar", "solo", "theme")
    excluded_words = ("drum", "kick", "snare", "hat", "perc", "bass", "pad", "chord", "arp")
    parsed: list[tuple[float, float, str]] = []
    fallback: list[tuple[float, float, str]] = []
    cue_start_beat = cue.start_sample / sample_rate * bpm / 60.0 if cue else None
    cue_end_beat = cue.end_sample / sample_rate * bpm / 60.0 if cue else None

    for note in notes:
        pitch = _pitch_value(note)
        beat = _event_beat(note, bpm, sample_rate)
        if pitch is None or beat is None:
            continue
        if cue_start_beat is not None and not (cue_start_beat <= beat < cue_end_beat):
            # render_soundtrack.py emits absolute start_beat values.  Treat the
            # timestamp as authoritative; a broad label such as "verse" may be
            # reused for several different verses and must not leak notes from
            # one realization into another.
            continue
        role = str(_first(note, ("role", "instrument", "voice", "part"), "")).lower()
        item = (float(beat), float(pitch), role)
        if any(word in role for word in melodic_words):
            parsed.append(item)
        elif not any(word in role for word in excluded_words):
            fallback.append(item)

    events = parsed or fallback
    events.sort(key=lambda item: (item[0], item[1]))
    # Collapse simultaneous notes to the highest voice; that is usually the tune.
    monophonic: list[tuple[float, float]] = []
    for beat, pitch, _role in events:
        if monophonic and abs(beat - monophonic[-1][0]) < 1.0e-5:
            monophonic[-1] = (beat, max(pitch, monophonic[-1][1]))
        else:
            monophonic.append((beat, pitch))
    if len(monophonic) < 2:
        return np.array([], dtype=np.float64), np.array([], dtype=np.float64), None

    beats = np.asarray([item[0] for item in monophonic], dtype=np.float64)
    pitches = np.asarray([item[1] for item in monophonic], dtype=np.float64)
    intervals = np.clip(np.diff(pitches), -24.0, 24.0)
    rhythm = np.diff(beats)
    positive_rhythm = rhythm[rhythm > 1.0e-5]
    if positive_rhythm.size:
        rhythm = rhythm / float(np.median(positive_rhythm))
    rhythm = np.clip(rhythm, 0.0, 8.0)

    if intervals.size >= 7:
        canonical = {
            "intervals": np.rint(intervals * 2.0).astype(int).tolist(),
            "rhythm": np.rint(rhythm * 4.0).astype(int).tolist(),
        }
        digest = hashlib.sha256(
            json.dumps(canonical, separators=(",", ":"), sort_keys=True).encode("utf-8")
        ).hexdigest()[:20]
    else:
        digest = None
    return intervals, rhythm, digest


def _root_pitch_class(value: Any) -> int | None:
    numeric = _number(value)
    if numeric is not None:
        return int(round(numeric)) % 12
    text = str(value).strip()
    match = re.match(r"^([A-Ga-g])([#b]?)", text)
    if not match:
        return None
    pitch_class = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}[match.group(1).upper()]
    if match.group(2) == "#":
        pitch_class += 1
    elif match.group(2) == "b":
        pitch_class -= 1
    return pitch_class % 12


def _quality_value(value: Any) -> float:
    text = str(value or "").lower()
    if "dim" in text or "half" in text and "dim" in text:
        return -0.75
    if "aug" in text or "+" in text:
        return 0.75
    if "sus" in text:
        return 0.25
    if "min" in text or re.search(r"(^|[^a-z])m(?:aj)?[0-9]*$", text):
        return -1.0
    if "7" in text:
        return 0.55
    return 1.0


def _harmony_metadata(
    chords: list[dict[str, Any]], bpm: float, sample_rate: int, cue: Cue | None = None
) -> tuple[np.ndarray, str | None]:
    parsed: list[tuple[float, int, float]] = []
    cue_start_beat = cue.start_sample / sample_rate * bpm / 60.0 if cue else None
    cue_end_beat = cue.end_sample / sample_rate * bpm / 60.0 if cue else None
    for chord in chords:
        beat = _event_beat(chord, bpm, sample_rate)
        root = _root_pitch_class(_first(chord, ("root", "tonic", "pitch", "chord")))
        if beat is None or root is None:
            continue
        if cue_start_beat is not None and not (cue_start_beat <= beat < cue_end_beat):
            continue
        parsed.append((beat, root, _quality_value(_first(chord, ("quality", "type", "suffix"), ""))))
    parsed.sort(key=lambda item: item[0])
    if not parsed:
        return np.array([], dtype=np.float64), None

    roots = np.asarray([item[1] for item in parsed], dtype=np.float64)
    qualities = np.asarray([item[2] for item in parsed], dtype=np.float64)
    if roots.size >= 2:
        motion = (np.diff(roots) + 6.0) % 12.0 - 6.0
        vector = np.empty(motion.size * 2 + 1, dtype=np.float64)
        vector[0] = qualities[0]
        vector[1::2] = motion / 6.0
        vector[2::2] = qualities[1:]
    else:
        vector = qualities.copy()
    digest = None
    if roots.size >= 4:
        canonical = {
            "motion": np.rint(((np.diff(roots) + 6.0) % 12.0 - 6.0) * 2.0).astype(int).tolist(),
            "quality": np.rint(qualities * 4.0).astype(int).tolist(),
        }
        digest = hashlib.sha256(
            json.dumps(canonical, separators=(",", ":"), sort_keys=True).encode("utf-8")
        ).hexdigest()[:20]
    return vector, digest


def _read_wav(path: Path) -> tuple[int, np.ndarray, dict[str, Any]]:
    sample_rate, raw = wavfile.read(path, mmap=False)
    original_dtype = str(raw.dtype)
    if raw.ndim == 1:
        raw = raw[:, np.newaxis]
    elif raw.ndim != 2:
        raise ValueError(f"expected mono/stereo PCM, got shape {raw.shape}")

    if np.issubdtype(raw.dtype, np.floating):
        audio = raw.astype(np.float64)
    elif np.issubdtype(raw.dtype, np.unsignedinteger):
        info = np.iinfo(raw.dtype)
        midpoint = (info.max + 1) / 2.0
        audio = (raw.astype(np.float64) - midpoint) / midpoint
    elif np.issubdtype(raw.dtype, np.signedinteger):
        info = np.iinfo(raw.dtype)
        audio = raw.astype(np.float64) / float(max(abs(info.min), info.max))
    else:
        raise ValueError(f"unsupported WAV dtype {raw.dtype}")

    metadata = {
        "dtype": original_dtype,
        "frames": int(audio.shape[0]),
        "channels": int(audio.shape[1]),
    }
    return int(sample_rate), audio, metadata


def _stft_features(mono: np.ndarray, sample_rate: int) -> dict[str, np.ndarray]:
    if mono.size < 32:
        zeros = np.zeros(ANALYSIS_VECTOR_SIZE, dtype=np.float64)
        return {
            "onset": zeros,
            "energy": zeros,
            "intervals": zeros,
            "chroma": np.zeros(CHROMA_SIZE, dtype=np.float64),
            "brightness": zeros,
        }

    frame_length = max(128, int(round(sample_rate * 0.0464)))
    frame_length = min(2048, 1 << int(round(math.log2(frame_length))))
    frame_length = min(frame_length, mono.size)
    hop = max(32, frame_length // 4)
    noverlap = max(0, frame_length - hop)
    frequencies, _times, spectrum = signal.stft(
        mono,
        fs=sample_rate,
        window="hann",
        nperseg=frame_length,
        noverlap=noverlap,
        boundary=None,
        padded=False,
    )
    magnitude = np.abs(spectrum).astype(np.float64)
    if magnitude.shape[1] == 0:
        magnitude = np.zeros((frequencies.size, 1), dtype=np.float64)

    log_magnitude = np.log1p(40.0 * magnitude)
    spectral_flux = np.zeros(log_magnitude.shape[1], dtype=np.float64)
    if log_magnitude.shape[1] > 1:
        spectral_flux[1:] = np.sum(np.maximum(np.diff(log_magnitude, axis=1), 0.0), axis=0)
    median_flux = float(np.median(spectral_flux))
    spectral_flux = np.maximum(spectral_flux - median_flux * 0.55, 0.0)

    energy = np.sqrt(np.mean(np.square(magnitude), axis=0))
    spectral_sum = np.sum(magnitude, axis=0) + EPSILON
    centroid = np.sum(frequencies[:, np.newaxis] * magnitude, axis=0) / spectral_sum
    brightness = np.clip(centroid / max(sample_rate * 0.5, 1.0), 0.0, 1.0)

    chroma = np.zeros(CHROMA_SIZE, dtype=np.float64)
    valid = (frequencies >= 55.0) & (frequencies <= min(6000.0, sample_rate * 0.48))
    valid_frequencies = frequencies[valid]
    if valid_frequencies.size:
        midi = np.rint(69.0 + 12.0 * np.log2(valid_frequencies / 440.0)).astype(int)
        weights = np.sum(np.square(magnitude[valid]), axis=1)
        for pitch_class in range(CHROMA_SIZE):
            chroma[pitch_class] = float(np.sum(weights[np.mod(midi, 12) == pitch_class]))
        if np.sum(chroma) > EPSILON:
            chroma /= float(np.sum(chroma))

    melodic_band = (frequencies >= 100.0) & (frequencies <= min(2200.0, sample_rate * 0.45))
    band_frequencies = frequencies[melodic_band]
    band_magnitude = magnitude[melodic_band]
    pitches: list[float] = []
    if band_frequencies.size:
        global_floor = float(np.percentile(np.max(band_magnitude, axis=0), 25.0))
        for frame in range(band_magnitude.shape[1]):
            column = band_magnitude[:, frame]
            index = int(np.argmax(column))
            if column[index] > max(global_floor * 0.75, EPSILON):
                pitches.append(69.0 + 12.0 * math.log2(band_frequencies[index] / 440.0))
    if len(pitches) >= 2:
        pitch_array = signal.medfilt(np.asarray(pitches), kernel_size=3)
        intervals = np.clip(np.diff(pitch_array), -24.0, 24.0)
    else:
        intervals = np.zeros(1, dtype=np.float64)

    return {
        "onset": _normalize_vector(spectral_flux, ANALYSIS_VECTOR_SIZE),
        "energy": _normalize_vector(energy, ANALYSIS_VECTOR_SIZE),
        "intervals": _normalize_vector(intervals, ANALYSIS_VECTOR_SIZE),
        "chroma": chroma,
        "brightness": _normalize_vector(brightness, ANALYSIS_VECTOR_SIZE),
    }


def _normalized_waveform_hash(samples: np.ndarray) -> str:
    mono = np.mean(samples, axis=1) if samples.ndim == 2 else samples
    vector = _normalize_vector(mono, 4096)
    peak = float(np.max(np.abs(vector))) if vector.size else 0.0
    if peak > EPSILON:
        vector = vector / peak
    quantized = np.rint(np.clip(vector, -1.0, 1.0) * 32767.0).astype("<i2")
    return hashlib.sha256(quantized.tobytes()).hexdigest()[:20]


def _feature_set(
    samples: np.ndarray,
    sample_rate: int,
    notes: list[dict[str, Any]],
    chords: list[dict[str, Any]],
    bpm: float,
    cue: Cue | None = None,
) -> FeatureSet:
    mono = np.mean(samples, axis=1)
    spectral = _stft_features(mono, sample_rate)
    metadata_intervals, metadata_rhythm, melody_hash = _melody_metadata(
        notes, bpm, sample_rate, cue
    )
    if metadata_intervals.size:
        combined = np.empty(metadata_intervals.size * 2, dtype=np.float64)
        combined[0::2] = metadata_intervals / 24.0
        combined[1::2] = _normalize_vector(metadata_rhythm, metadata_intervals.size) / 8.0
        intervals = _normalize_vector(combined, ANALYSIS_VECTOR_SIZE)
    else:
        intervals = spectral["intervals"]
    metadata_harmony, harmony_hash = _harmony_metadata(chords, bpm, sample_rate, cue)
    return FeatureSet(
        onset=spectral["onset"],
        energy=spectral["energy"],
        intervals=intervals,
        chroma=spectral["chroma"],
        harmony=metadata_harmony,
        brightness=spectral["brightness"],
        waveform_hash=_normalized_waveform_hash(samples),
        melody_hash=melody_hash,
        harmony_hash=harmony_hash,
    )


def _feature_similarity(left: FeatureSet, right: FeatureSet) -> dict[str, float]:
    onset = _shifted_correlation(left.onset, right.onset)
    energy = _shifted_correlation(left.energy, right.energy)
    melodic = _shifted_correlation(left.intervals, right.intervals)
    if left.harmony.size and right.harmony.size:
        harmonic = _shifted_correlation(left.harmony, right.harmony)
    else:
        harmonic = max(0.0, _cosine_similarity(left.chroma, right.chroma))
    brightness = _shifted_correlation(left.brightness, right.brightness)
    combined = 0.28 * onset + 0.20 * energy + 0.32 * melodic + 0.12 * harmonic + 0.08 * brightness
    return {
        "combined": float(combined),
        "onset": float(onset),
        "energy": float(energy),
        "melodic": float(melodic),
        "harmonic": float(harmonic),
        "brightness": float(brightness),
    }


def _frame_rms(mono: np.ndarray, sample_rate: int) -> np.ndarray:
    frame = max(1, int(round(sample_rate * 0.050)))
    hop = max(1, frame // 2)
    if mono.size <= frame:
        return np.asarray([_rms(mono)])
    starts = np.arange(0, mono.size - frame + 1, hop)
    return np.asarray([_rms(mono[start : start + frame]) for start in starts])


def _section_loudness(samples: np.ndarray) -> dict[str, float]:
    mono = np.mean(samples, axis=1)
    rms = _rms(mono)
    peak = float(np.max(np.abs(samples))) if samples.size else 0.0
    return {
        "rms": rms,
        "rms_dbfs": _db(rms),
        "peak": peak,
        "peak_dbfs": _db(peak),
    }


def _longest_true_run(mask: np.ndarray) -> int:
    if mask.size == 0 or not np.any(mask):
        return 0
    padded = np.concatenate(([False], mask.astype(bool), [False]))
    changes = np.flatnonzero(padded[1:] != padded[:-1])
    return int(np.max(changes[1::2] - changes[::2]))


def _transition_metrics(audio: np.ndarray, sample_rate: int, boundary: int) -> dict[str, float]:
    channels = audio.shape[1]
    if boundary <= 0 or boundary >= audio.shape[0]:
        return {}
    jump = float(np.max(np.abs(audio[boundary] - audio[boundary - 1])))

    local_radius = max(4, int(round(sample_rate * 0.100)))
    lo = max(0, boundary - local_radius)
    hi = min(audio.shape[0], boundary + local_radius)
    local = audio[lo:hi]
    derivative_channels = np.abs(np.diff(local, axis=0))
    derivative = derivative_channels.reshape(-1)
    derivative_frames = np.max(derivative_channels, axis=1) if derivative_channels.size else np.array([])
    local_p99 = float(np.percentile(derivative_frames, 99.0)) if derivative_frames.size else 0.0
    local_p999 = float(np.percentile(derivative_frames, 99.9)) if derivative_frames.size else 0.0
    derivative_cluster = int(np.count_nonzero(derivative_frames >= jump * 0.25)) if jump > 0.0 else 0

    deriv_radius = max(1, int(round(sample_rate * 0.001)))
    d_lo = max(1, boundary - deriv_radius)
    d_hi = min(audio.shape[0], boundary + deriv_radius)
    derivative_2ms = _rms(np.diff(audio[d_lo - 1 : d_hi], axis=0))
    derivative_local_rms = _rms(np.diff(local, axis=0))
    derivative_ratio = derivative_2ms / max(derivative_local_rms, EPSILON)

    five_ms = max(2, int(round(sample_rate * 0.005)))
    twenty_ms = max(2, int(round(sample_rate * 0.020)))
    hundred_ms = max(2, int(round(sample_rate * 0.100)))
    before_20 = audio[max(0, boundary - twenty_ms) : boundary]
    after_20 = audio[boundary : min(audio.shape[0], boundary + twenty_ms)]
    before_far = audio[max(0, boundary - 2 * twenty_ms) : max(0, boundary - twenty_ms)]
    after_far = audio[
        min(audio.shape[0], boundary + twenty_ms) : min(audio.shape[0], boundary + 2 * twenty_ms)
    ]
    if before_20.size and after_20.size:
        near_delta = np.mean(after_20, axis=0) - np.mean(before_20, axis=0)
        # Remove the smoothly varying component predicted by the neighboring
        # 20ms blocks.  Without this correction an ordinary bass waveform can
        # look like a DC step merely because 20ms is not an integer number of
        # cycles.  A real offset introduced at the join remains in near_delta.
        if before_far.size and after_far.size:
            incoming_slope = np.mean(before_20, axis=0) - np.mean(before_far, axis=0)
            outgoing_slope = np.mean(after_far, axis=0) - np.mean(after_20, axis=0)
            smooth_delta = 0.5 * (incoming_slope + outgoing_slope)
            dc_change = float(np.max(np.abs(near_delta - smooth_delta)))
        else:
            dc_change = float(np.max(np.abs(near_delta)))
    else:
        dc_change = 0.0

    before_100 = audio[max(0, boundary - hundred_ms) : boundary]
    after_100 = audio[boundary : min(audio.shape[0], boundary + hundred_ms)]
    rms_before = _rms(before_100)
    rms_after = _rms(after_100)
    rms_change_db = abs(_db(rms_after, max(rms_before, EPSILON)))

    mono = np.mean(local, axis=1)
    cutoff = min(4000.0, sample_rate * 0.35)
    if mono.size > 32 and cutoff > 50.0:
        sos = signal.butter(3, cutoff, btype="highpass", fs=sample_rate, output="sos")
        highpassed = signal.sosfilt(sos, mono)
        local_boundary = boundary - lo
        half_burst = max(1, five_ms // 2)
        burst = highpassed[
            max(0, local_boundary - half_burst) : min(highpassed.size, local_boundary + half_burst)
        ]
        reference = np.concatenate(
            (
                highpassed[max(0, local_boundary - twenty_ms) : max(0, local_boundary - five_ms)],
                highpassed[min(highpassed.size, local_boundary + five_ms) : min(highpassed.size, local_boundary + twenty_ms)],
            )
        )
        hf_burst_db = _db(_rms(burst), max(_rms(reference), EPSILON))
    else:
        hf_burst_db = 0.0

    gap_radius = max(2, int(round(sample_rate * 0.010)))
    gap_region = audio[
        max(0, boundary - gap_radius) : min(audio.shape[0], boundary + gap_radius)
    ]
    # A gap is truly silent only when every channel is nearly zero.
    silent = np.max(np.abs(gap_region), axis=1) <= 1.0e-5 if gap_region.size else np.array([])
    gap_samples = _longest_true_run(silent)

    return {
        "sample_discontinuity": jump,
        "local_derivative_p99": local_p99,
        "local_derivative_p999": local_p999,
        "discontinuity_to_p99": jump / max(local_p99, EPSILON),
        "discontinuity_to_p999": jump / max(local_p999, EPSILON),
        "derivative_cluster_samples_above_quarter_peak": derivative_cluster,
        "derivative_rms_2ms": derivative_2ms,
        "derivative_rms_local": derivative_local_rms,
        "derivative_rms_ratio": derivative_ratio,
        "hf_burst_db_5ms": hf_burst_db,
        "dc_change_20ms": dc_change,
        "rms_before_100ms": rms_before,
        "rms_after_100ms": rms_after,
        "rms_change_db_100ms": rms_change_db,
        "near_zero_gap_samples": gap_samples,
        "near_zero_gap_ms": gap_samples / sample_rate * 1000.0,
        "channels": channels,
    }


def _global_discontinuity_metrics(
    audio: np.ndarray,
    sample_rate: int,
    bpm: float,
    beats_per_bar: float,
    notes: list[dict[str, Any]],
) -> dict[str, Any]:
    """Find genuinely isolated clicks without mistaking drum attacks for seams."""
    if audio.shape[0] < 2:
        return {
            "max_jump": 0.0,
            "global_p999": 0.0,
            "sample": 0,
            "pathological": None,
        }

    derivative = np.max(np.abs(np.diff(audio, axis=0)), axis=1)
    global_index = int(np.argmax(derivative)) + 1
    global_jump = float(derivative[global_index - 1])
    global_p999 = float(np.percentile(derivative, 99.9))
    candidate_floor = max(0.12, global_p999 * 2.0)
    candidate_indices = np.flatnonzero(derivative > candidate_floor) + 1

    onset_samples: list[int] = []
    drum_samples: list[int] = []
    for note in notes:
        beat = _event_beat(note, bpm, sample_rate)
        if beat is None:
            continue
        event_sample = int(round(beat * 60.0 / bpm * sample_rate))
        onset_samples.append(event_sample)
        role = str(_first(note, ("role", "instrument", "voice", "part"), "")).lower()
        instrument = str(note.get("instrument", "")).lower()
        if any(word in role or word in instrument for word in ("drum", "kick", "snare", "clap", "hat", "tom", "perc")):
            drum_samples.append(event_sample)

    def nearest_ms(sample: int, candidates: list[int]) -> float | None:
        if not candidates:
            return None
        return min(abs(sample - candidate) for candidate in candidates) / sample_rate * 1000.0

    def candidate_metrics(sample: int) -> dict[str, Any]:
        jump = float(derivative[sample - 1])
        radius = max(16, int(round(sample_rate * 0.100)))
        lo = max(0, sample - 1 - radius)
        hi = min(derivative.size, sample - 1 + radius)
        local = derivative[lo:hi]
        local_p99 = float(np.percentile(local, 99.0)) if local.size else 0.0
        local_p999 = float(np.percentile(local, 99.9)) if local.size else 0.0
        local_ratio_p99 = jump / max(local_p99, EPSILON)
        cluster_count = int(np.count_nonzero(local >= jump * 0.25))
        beat = sample / sample_rate * bpm / 60.0
        phase = beat % beats_per_bar
        downbeat_distance_beats = min(phase, beats_per_bar - phase)
        downbeat_ms = downbeat_distance_beats * 60.0 / bpm * 1000.0
        onset_ms = nearest_ms(sample, onset_samples)
        drum_ms = nearest_ms(sample, drum_samples)
        isolated = local_ratio_p99 > 4.0 or cluster_count <= 3
        return {
            "sample": sample,
            "seconds": sample / sample_rate,
            "jump": jump,
            "local_p99": local_p99,
            "local_p999": local_p999,
            "jump_to_local_p99": local_ratio_p99,
            "cluster_samples_above_quarter_peak": cluster_count,
            "nearest_declared_onset_ms": onset_ms,
            "nearest_declared_drum_ms": drum_ms,
            "nearest_downbeat_ms": downbeat_ms,
            "isolated": isolated,
            "near_musical_transient": bool(
                downbeat_ms <= 40.0
                or onset_ms is not None and onset_ms <= 40.0
                or drum_ms is not None and drum_ms <= 40.0
            ),
        }

    global_detail = candidate_metrics(global_index)
    pathological: dict[str, Any] | None = None
    # Candidates occur in small clusters around a transient.  Inspect strongest
    # first, then skip the surrounding 2ms so one click does not create dozens
    # of duplicate diagnostics.
    suppressed: list[int] = []
    spacing = max(1, int(round(sample_rate * 0.002)))
    for sample in sorted(candidate_indices.tolist(), key=lambda item: derivative[item - 1], reverse=True):
        if any(abs(sample - previous) <= spacing for previous in suppressed):
            continue
        suppressed.append(sample)
        detail = candidate_metrics(sample)
        if detail["isolated"]:
            pathological = detail
            break

    return {
        "max_jump": global_jump,
        "global_p999": global_p999,
        "sample": global_index,
        "global_detail": global_detail,
        "candidate_count": int(candidate_indices.size),
        "pathological": pathological,
    }


def _resolve_manifest(source: Path) -> tuple[dict[str, Any], Path | None, Path]:
    source = source.expanduser().resolve()
    if source.is_file() and source.suffix.lower() in (".wav", ".wave"):
        return {"tracks": [{"name": source.stem, "file": source.name}]}, None, source.parent
    if source.is_file():
        with source.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
        manifest = {"tracks": payload} if isinstance(payload, list) else payload
        if not isinstance(manifest, dict) or not isinstance(manifest.get("tracks"), list):
            raise ValueError(f"{source} does not contain a tracks array")
        return manifest, source, source.parent
    if not source.is_dir():
        raise FileNotFoundError(source)

    preferred = (
        "soundtrack_manifest.json",
        "gravity_wave_soundtrack.json",
        "music_manifest.json",
        "manifest.json",
    )
    candidates = [source / name for name in preferred if (source / name).is_file()]
    candidates.extend(path for path in sorted(source.glob("*.json")) if path not in candidates)
    for candidate in candidates:
        try:
            with candidate.open("r", encoding="utf-8") as handle:
                payload = json.load(handle)
            manifest = {"tracks": payload} if isinstance(payload, list) else payload
            if isinstance(manifest, dict) and isinstance(manifest.get("tracks"), list):
                return manifest, candidate, candidate.parent
        except (OSError, ValueError, json.JSONDecodeError):
            continue

    wavs = sorted(list(source.glob("*.wav")) + list(source.glob("*.wave")))
    if not wavs:
        raise ValueError(f"no soundtrack manifest or WAV files found in {source}")
    return {"tracks": [{"name": path.stem, "file": path.name} for path in wavs]}, None, source


def _track_path(track: dict[str, Any], manifest: dict[str, Any], base_dir: Path) -> Path:
    raw = _first(track, ("file", "wav", "wav_file", "audio", "output", "path"))
    if isinstance(raw, dict):
        raw = _first(raw, ("wav", "file", "path"))
    if raw is None:
        name = str(_first(track, ("name", "id", "slug", "title"), "track"))
        raw = f"{name}.wav"
    path = Path(str(raw))
    if path.is_absolute():
        return path
    wav_directory = _first(manifest, ("wav_directory", "audio_directory", "output_directory"))
    if wav_directory:
        candidate = base_dir / str(wav_directory) / path
        if candidate.exists():
            return candidate.resolve()
    return (base_dir / path).resolve()


def _cue_position_samples(
    item: dict[str, Any],
    prefix: str,
    sample_rate: int,
    bpm: float,
    beats_per_bar: float,
) -> int | None:
    sample_keys = (
        ("start_sample", "sample", "start_frame", "frame")
        if prefix == "start"
        else ("end_sample", "sample_end", "end_frame", "frame_end")
    )
    sample = _number(_first(item, sample_keys))
    if sample is not None:
        return int(round(sample))
    second_keys = (
        ("start_seconds", "start_time", "time", "start")
        if prefix == "start"
        else ("end_seconds", "end_time", "end")
    )
    seconds = _number(_first(item, second_keys))
    if seconds is not None:
        return int(round(seconds * sample_rate))
    beat_keys = ("start_beat", "beat") if prefix == "start" else ("end_beat",)
    beat = _number(_first(item, beat_keys))
    if beat is not None:
        return int(round(beat * 60.0 / bpm * sample_rate))
    bar_keys = ("start_bar", "bar") if prefix == "start" else ("end_bar",)
    bar = _number(_first(item, bar_keys))
    if bar is not None:
        return int(round(bar * beats_per_bar * 60.0 / bpm * sample_rate))
    return None


def _parse_cues(
    track: dict[str, Any],
    manifest: dict[str, Any],
    sample_rate: int,
    frames: int,
    bpm: float,
    beats_per_bar: float,
) -> list[Cue]:
    raw: Any = _first(track, ("sections", "cues", "form", "arrangement"), [])
    if isinstance(raw, dict):
        raw = _first(raw, ("sections", "cues", "items", "events"), [])
    if not isinstance(raw, list):
        raw = []
    if not raw:
        global_sections = manifest.get("sections")
        if isinstance(global_sections, dict):
            keys = [str(track.get(key, "")) for key in ("id", "name", "title", "slug")]
            for key in keys:
                if isinstance(global_sections.get(key), list):
                    raw = global_sections[key]
                    break

    parsed: list[dict[str, Any]] = []
    for index, value in enumerate(raw):
        if isinstance(value, str):
            value = {"name": value}
        if not isinstance(value, dict):
            continue
        name = str(_first(value, ("name", "label", "id", "section", "kind", "type"), f"section_{index + 1}"))
        # The renderer's canonical schema uses start_bar + bars.  Resolve both
        # ends from the absolute bar grid so independent rounding of a duration
        # cannot create a one-sample overlap or gap between contiguous cues.
        start_bar = _number(value.get("start_bar"))
        bar_count = _number(value.get("bars"))
        if start_bar is not None:
            start = int(round(start_bar * beats_per_bar * 60.0 / bpm * sample_rate))
            if bar_count is not None:
                end = int(
                    round((start_bar + bar_count) * beats_per_bar * 60.0 / bpm * sample_rate)
                )
            else:
                end = _cue_position_samples(value, "end", sample_rate, bpm, beats_per_bar)
        else:
            start = _cue_position_samples(value, "start", sample_rate, bpm, beats_per_bar)
            end = _cue_position_samples(value, "end", sample_rate, bpm, beats_per_bar)
        if start is None and index == 0:
            start = 0
        if end is None:
            duration_samples = _number(_first(value, ("frames", "samples", "length_samples")))
            duration_seconds = _number(_first(value, ("duration_seconds", "duration", "length")))
            duration_beats = _number(_first(value, ("duration_beats", "beats")))
            duration_bars = _number(_first(value, ("length_bars", "duration_bars", "bars")))
            if start is not None and duration_samples is not None:
                end = start + int(round(duration_samples))
            elif start is not None and duration_seconds is not None:
                end = start + int(round(duration_seconds * sample_rate))
            elif start is not None and duration_beats is not None:
                end = start + int(round(duration_beats * 60.0 / bpm * sample_rate))
            elif start is not None and duration_bars is not None:
                end = start + int(round(duration_bars * beats_per_bar * 60.0 / bpm * sample_rate))
        parsed.append({"name": name, "kind": _canonical_label(name), "start": start, "end": end, "source": value})

    # Fill omitted starts/ends from neighboring cue positions.
    for index, item in enumerate(parsed):
        if item["start"] is None:
            item["start"] = parsed[index - 1]["end"] if index and parsed[index - 1]["end"] is not None else 0
    parsed.sort(key=lambda item: int(item["start"] or 0))
    for index, item in enumerate(parsed):
        if item["end"] is None:
            item["end"] = parsed[index + 1]["start"] if index + 1 < len(parsed) else frames
    return [
        Cue(
            name=item["name"],
            kind=item["kind"],
            start_sample=int(item["start"]),
            end_sample=int(item["end"]),
            source=item["source"],
        )
        for item in parsed
    ]


class SoundtrackAnalyzer:
    def __init__(self, manifest: dict[str, Any], manifest_path: Path | None, base_dir: Path, strict: bool):
        self.manifest = manifest
        self.manifest_path = manifest_path
        self.base_dir = base_dir
        self.strict = strict
        self.issues: list[Issue] = []
        self.results: list[dict[str, Any]] = []
        self.features: dict[str, FeatureSet] = {}
        self.section_features: dict[str, list[tuple[Cue, FeatureSet]]] = {}

    def issue(
        self,
        severity: str,
        code: str,
        message: str,
        track: str | None = None,
        section: str | None = None,
        **details: Any,
    ) -> None:
        self.issues.append(Issue(severity, code, message, track, section, details))

    def analyze(self) -> dict[str, Any]:
        tracks = self.manifest.get("tracks", [])
        if not tracks:
            self.issue("error", "no_tracks", "Manifest contains no tracks.")
        for index, raw_track in enumerate(tracks):
            if not isinstance(raw_track, dict):
                self.issue("error", "invalid_track", f"Track entry {index} is not an object.")
                continue
            self._analyze_track(raw_track, index)
        self._check_album_duplicates()
        pairwise = self._pairwise_analysis()

        errors = sum(issue.severity == "error" for issue in self.issues)
        warnings = sum(issue.severity == "warning" for issue in self.issues)
        failed = errors > 0 or (self.strict and warnings > 0)
        return {
            "schema_version": 1,
            "source": str(self.manifest_path or self.base_dir),
            "strict": self.strict,
            "status": "fail" if failed else "pass",
            "summary": {
                "tracks_analyzed": len(self.results),
                "errors": errors,
                "warnings": warnings,
                "strict_warning_failure": bool(self.strict and warnings),
            },
            "tracks": self.results,
            "pairwise": pairwise,
            "issues": [issue.as_dict() for issue in self.issues],
        }

    def _analyze_track(self, track: dict[str, Any], index: int) -> None:
        name = str(_first(track, ("name", "title", "id", "slug"), f"track_{index + 1}"))
        if name in self.features:
            self.issue("error", "duplicate_track_name", f"Duplicate track name {name!r}.", name)
            name = f"{name} #{index + 1}"
        path = _track_path(track, self.manifest, self.base_dir)
        if not path.is_file():
            self.issue("error", "missing_wav", f"WAV file does not exist: {path}", name)
            return
        try:
            sample_rate, full_audio, wav_metadata = _read_wav(path)
        except (OSError, ValueError) as exc:
            self.issue("error", "invalid_wav", f"Cannot read {path}: {exc}", name)
            return

        wav_frames, channels = full_audio.shape
        stream_duration = wav_frames / sample_rate if sample_rate else 0.0
        bpm = float(_number(_first(track, ("bpm", "tempo"), _first(self.manifest, ("bpm", "tempo"), 120.0)), 120.0) or 120.0)
        beats_per_bar = float(
            _number(_first(track, ("beats_per_bar", "meter_numerator"), _first(self.manifest, ("beats_per_bar", "meter_numerator"), DEFAULT_BEATS_PER_BAR)), DEFAULT_BEATS_PER_BAR)
            or DEFAULT_BEATS_PER_BAR
        )
        if sample_rate <= 0 or wav_frames <= 0 or channels <= 0:
            self.issue("error", "invalid_length", "WAV has no playable PCM frames.", name)
            return
        if not np.all(np.isfinite(full_audio)):
            self.issue("error", "nonfinite_audio", "WAV contains NaN or infinite samples.", name)
            full_audio = np.nan_to_num(full_audio)

        expected_rate = _number(_first(track, ("sample_rate", "rate"), self.manifest.get("sample_rate")))
        if expected_rate is not None and int(round(expected_rate)) != sample_rate:
            self.issue(
                "error",
                "sample_rate_mismatch",
                f"Manifest says {expected_rate:g} Hz; WAV is {sample_rate} Hz.",
                name,
            )
        declared_stream_value = _number(
            _first(track, ("stream_frames", "frames", "frame_count", "samples"))
        )
        declared_stream_frames = (
            int(round(declared_stream_value)) if declared_stream_value is not None else wav_frames
        )
        if declared_stream_frames != wav_frames:
            self.issue(
                "error",
                "frame_count_mismatch",
                f"Manifest says {declared_stream_frames} stream frames; WAV has {wav_frames}.",
                name,
                manifest_stream_frames=declared_stream_frames,
                wav_frames=wav_frames,
            )
        for field_name in ("stream_frames", "frames"):
            field_value = _number(track.get(field_name))
            if field_value is not None and int(round(field_value)) != wav_frames:
                # Avoid repeating the primary mismatch with the same wording,
                # but still identify a disagreement between redundant fields.
                if int(round(field_value)) != declared_stream_frames or declared_stream_frames == wav_frames:
                    self.issue(
                        "error",
                        "frame_field_mismatch",
                        f"Manifest {field_name} is {int(round(field_value))}; WAV has {wav_frames} frames.",
                        name,
                        field=field_name,
                    )

        playable_value = _number(track.get("playable_frames"))
        playable_frames = int(round(playable_value)) if playable_value is not None else min(wav_frames, declared_stream_frames)
        music_value = _number(track.get("music_frames"))
        music_frames = int(round(music_value)) if music_value is not None else playable_frames
        if not (0 < music_frames <= playable_frames <= declared_stream_frames):
            self.issue(
                "error",
                "invalid_stream_regions",
                "Expected 0 < music_frames <= playable_frames <= stream_frames.",
                name,
                music_frames=music_frames,
                playable_frames=playable_frames,
                stream_frames=declared_stream_frames,
            )
        analysis_end = max(0, min(playable_frames, wav_frames))
        form_end = max(0, min(music_frames, analysis_end))
        audio = full_audio[:analysis_end]
        frames = audio.shape[0]
        duration = frames / sample_rate
        if duration < 5.0:
            self.issue("error", "invalid_length", f"Track is only {duration:.3f}s long.", name, duration_seconds=duration)

        tail_frames = max(0, playable_frames - music_frames)
        declared_tail_value = _number(track.get("tail_frames"))
        if declared_tail_value is not None and int(round(declared_tail_value)) != tail_frames:
            self.issue(
                "error",
                "tail_length_mismatch",
                f"Manifest tail_frames is {int(round(declared_tail_value))}; region length is {tail_frames}.",
                name,
            )
        tail_seconds_value = _number(
            _first(track, ("tail_seconds", "release_tail_seconds"), self.manifest.get("tail_seconds"))
        )
        if tail_seconds_value is not None:
            expected_tail_frames = int(round(tail_seconds_value * sample_rate))
            if abs(expected_tail_frames - tail_frames) > 1:
                self.issue(
                    "error",
                    "tail_duration_mismatch",
                    f"Declared {tail_seconds_value:g}s tail implies {expected_tail_frames} frames; region has {tail_frames}.",
                    name,
                )

        guard_frames = max(0, declared_stream_frames - playable_frames)
        declared_guard_value = _number(track.get("guard_frames"))
        if declared_guard_value is not None and int(round(declared_guard_value)) != guard_frames:
            self.issue(
                "error",
                "guard_length_mismatch",
                f"Manifest guard_frames is {int(round(declared_guard_value))}; region length is {guard_frames}.",
                name,
            )
        minimum_guard = int(
            round(_number(_first(track, ("minimum_guard_frames",), self.manifest.get("minimum_guard_frames")), 0.0) or 0.0)
        )
        if guard_frames < minimum_guard:
            self.issue(
                "error",
                "guard_too_short",
                f"Guard has {guard_frames} frames; at least {minimum_guard} are required.",
                name,
            )
        guard_end = min(wav_frames, declared_stream_frames)
        guard_audio = full_audio[max(0, min(playable_frames, wav_frames)) : guard_end]
        guard_nonzero = int(np.count_nonzero(guard_audio))
        if guard_audio.shape[0] != guard_frames:
            self.issue(
                "error",
                "guard_region_truncated",
                f"WAV exposes {guard_audio.shape[0]} guard frames; manifest declares {guard_frames}.",
                name,
            )
        elif guard_nonzero:
            self.issue(
                "error",
                "guard_not_silent",
                f"Stream guard contains {guard_nonzero} non-zero PCM samples.",
                name,
                nonzero_samples=guard_nonzero,
            )

        tail_audio = full_audio[
            max(0, min(music_frames, wav_frames)) : max(0, min(playable_frames, wav_frames))
        ]
        tail_metrics = _section_loudness(tail_audio) if tail_audio.size else {
            "rms": 0.0,
            "rms_dbfs": _db(0.0),
            "peak": 0.0,
            "peak_dbfs": _db(0.0),
        }

        bars = _number(track.get("bars"))
        if bars is not None and bpm > 0.0:
            expected_music_frames = int(round(bars * beats_per_bar * 60.0 / bpm * sample_rate))
            difference = abs(music_frames - expected_music_frames)
            if difference > 1:
                self.issue(
                    "error",
                    "bar_duration_mismatch",
                    f"{bars:g} bars at {bpm:g} BPM imply {expected_music_frames} music frames; manifest has {music_frames}.",
                    name,
                    expected_music_frames=expected_music_frames,
                    actual_music_frames=music_frames,
                )

        peak = float(np.max(np.abs(audio)))
        clipped = int(np.count_nonzero(np.abs(audio) >= 0.9999))
        if clipped:
            self.issue(
                "error",
                "clipping",
                f"{clipped} samples reach digital full scale (peak {peak:.6f}).",
                name,
                clipped_samples=clipped,
                clipped_ratio=clipped / audio.size,
            )
        channel_dc = np.mean(audio, axis=0)
        if float(np.max(np.abs(channel_dc))) > 0.01:
            self.issue(
                "error",
                "dc_offset",
                f"Channel DC offset reaches {float(np.max(np.abs(channel_dc))):.4f} FS.",
                name,
            )
        elif float(np.max(np.abs(channel_dc))) > 0.003:
            self.issue("warning", "dc_offset", "Track has a noticeable DC offset.", name)

        mono = np.mean(audio, axis=1)
        rms = _rms(mono)
        frame_rms = _frame_rms(mono, sample_rate)
        active_rms = frame_rms[frame_rms > 1.0e-5]
        if active_rms.size:
            p10, p50, p95 = np.percentile(active_rms, [10.0, 50.0, 95.0])
        else:
            p10 = p50 = p95 = 0.0
        dynamic_range_db = _db(float(p95), max(float(p10), EPSILON))
        crest_factor_db = _db(peak, max(rms, EPSILON))

        notes = _extract_events(track, self.manifest, "notes")
        chords = _extract_events(track, self.manifest, "chords")
        track_features = _feature_set(audio, sample_rate, notes, chords, bpm)
        self.features[name] = track_features

        cues = _parse_cues(track, self.manifest, sample_rate, form_end, bpm, beats_per_bar)
        self._validate_cues(name, cues, form_end)
        sections: list[dict[str, Any]] = []
        feature_entries: list[tuple[Cue, FeatureSet]] = []
        transitions: list[dict[str, Any]] = []
        for cue_index, cue in enumerate(cues):
            safe_start = max(0, min(form_end, cue.start_sample))
            safe_end = max(safe_start, min(form_end, cue.end_sample))
            section_audio = audio[safe_start:safe_end]
            if section_audio.size:
                cue_features = _feature_set(section_audio, sample_rate, notes, chords, bpm, cue)
                feature_entries.append((cue, cue_features))
                loudness = _section_loudness(section_audio)
                sections.append(
                    {
                        "name": cue.name,
                        "kind": cue.kind,
                        "start_sample": cue.start_sample,
                        "end_sample": cue.end_sample,
                        "duration_seconds": cue.length / sample_rate,
                        "loudness": loudness,
                        "waveform_hash": cue_features.waveform_hash,
                        "melody_hash": cue_features.melody_hash,
                        "harmony_hash": cue_features.harmony_hash,
                    }
                )
            if cue_index > 0 and 0 < cue.start_sample < frames:
                metric = _transition_metrics(audio, sample_rate, cue.start_sample)
                transition = {
                    "from": cues[cue_index - 1].name,
                    "to": cue.name,
                    "sample": cue.start_sample,
                    "seconds": cue.start_sample / sample_rate,
                    **metric,
                }
                transitions.append(transition)
                self._check_transition(name, cue, metric)
        self.section_features[name] = feature_entries
        self._check_structure(name, feature_entries)

        discontinuity = _global_discontinuity_metrics(
            audio, sample_rate, bpm, beats_per_bar, notes
        )
        global_jump = float(discontinuity["max_jump"])
        global_p999 = float(discontinuity["global_p999"])
        global_index = int(discontinuity["sample"])
        pathological = discontinuity.get("pathological")
        if pathological is not None:
            self.issue(
                "error",
                "pathological_discontinuity",
                f"Isolated {pathological['jump']:.4f} FS sample jump at {pathological['seconds']:.3f}s.",
                name,
                **pathological,
            )

        form_signature = [cue.kind for cue in cues]
        form_durations = [round(cue.length / sample_rate * bpm / 60.0, 2) for cue in cues]
        _, _, melody_hash = _melody_metadata(notes, bpm, sample_rate)
        chord_signature = self._chord_signature(chords, bpm, sample_rate)
        active_roles = track.get("active_roles")
        self.results.append(
            {
                "name": name,
                "file": str(path),
                "wav": wav_metadata,
                "sample_rate": sample_rate,
                "frames": wav_frames,
                "music_frames": music_frames,
                "playable_frames": playable_frames,
                "stream_frames": declared_stream_frames,
                "tail_frames": tail_frames,
                "tail": tail_metrics,
                "guard_frames": guard_frames,
                "guard_nonzero_samples": guard_nonzero,
                "duration_seconds": duration,
                "music_duration_seconds": music_frames / sample_rate,
                "stream_duration_seconds": stream_duration,
                "bpm": bpm,
                "bars": bars,
                "peak": peak,
                "peak_dbfs": _db(peak),
                "clipped_samples": clipped,
                "dc_offset_by_channel": channel_dc.tolist(),
                "rms": rms,
                "rms_dbfs": _db(rms),
                "frame_rms_dbfs": {
                    "p10": _db(float(p10)),
                    "median": _db(float(p50)),
                    "p95": _db(float(p95)),
                },
                "dynamic_range_db_p95_p10": dynamic_range_db,
                "crest_factor_db": crest_factor_db,
                "global_max_sample_discontinuity": global_jump,
                "global_derivative_p999": global_p999,
                "global_discontinuity_detail": discontinuity.get("global_detail"),
                "discontinuity_candidate_count": discontinuity.get("candidate_count", 0),
                "form_signature": form_signature,
                "form_duration_beats": form_durations,
                "melody_hash": melody_hash,
                "harmony_hash": track_features.harmony_hash,
                "chord_signature": chord_signature,
                "note_event_count": len(notes),
                "chord_event_count": len(chords),
                "energy_curve": track.get("energy_curve"),
                "active_roles": active_roles,
                "sections": sections,
                "transitions": transitions,
            }
        )

    def _validate_cues(self, track_name: str, cues: list[Cue], frames: int) -> None:
        previous_end = 0
        for cue in cues:
            if cue.start_sample < 0 or cue.end_sample > frames or cue.end_sample <= cue.start_sample:
                self.issue(
                    "error",
                    "invalid_section_length",
                    f"Section {cue.name!r} has invalid range {cue.start_sample}:{cue.end_sample} for {frames} frames.",
                    track_name,
                    cue.name,
                )
            if cue.start_sample < previous_end:
                self.issue(
                    "error",
                    "overlapping_sections",
                    f"Section {cue.name!r} overlaps the preceding section.",
                    track_name,
                    cue.name,
                )
            if cue.start_sample > previous_end and previous_end > 0:
                self.issue(
                    "warning",
                    "section_coverage_gap",
                    f"There is an undeclared gap before {cue.name!r}.",
                    track_name,
                    cue.name,
                    gap_samples=cue.start_sample - previous_end,
                )
            previous_end = max(previous_end, cue.end_sample)

    def _check_transition(self, track_name: str, cue: Cue, metric: dict[str, float]) -> None:
        jump = metric.get("sample_discontinuity", 0.0)
        ratio = metric.get("discontinuity_to_p999", 0.0)
        if jump > 0.12 and ratio > 2.0:
            self.issue(
                "error",
                "section_discontinuity",
                f"Join into {cue.name!r} jumps {jump:.4f} FS ({ratio:.1f}x local p99.9 derivative).",
                track_name,
                cue.name,
            )
        dc_change = metric.get("dc_change_20ms", 0.0)
        # A finite 20ms average can be biased by a strong sub-bass waveform.
        # Treat it as a genuine offset step only when the boundary derivative
        # also departs from its local distribution; always retain the raw
        # metric in JSON for review.
        dc_isolated = (
            metric.get("discontinuity_to_p99", 0.0) > 4.0
            or metric.get("derivative_cluster_samples_above_quarter_peak", 0) <= 3
        )
        if dc_change > 0.01 and jump > 0.01 and dc_isolated:
            self.issue(
                "error",
                "section_dc_step",
                f"Join into {cue.name!r} changes 20ms DC by {dc_change:.4f} FS.",
                track_name,
                cue.name,
            )
        gap_ms = metric.get("near_zero_gap_ms", 0.0)
        if gap_ms > 2.0:
            self.issue(
                "error",
                "section_silence_gap",
                f"Join into {cue.name!r} contains {gap_ms:.2f}ms of digital silence.",
                track_name,
                cue.name,
            )
        hf_db = metric.get("hf_burst_db_5ms", 0.0)
        if hf_db > 8.0:
            self.issue(
                "error" if hf_db > 12.0 else "warning",
                "section_hf_burst",
                f"Join into {cue.name!r} has a {hf_db:.1f}dB 5ms high-frequency burst.",
                track_name,
                cue.name,
            )
        rms_db = metric.get("rms_change_db_100ms", 0.0)
        if rms_db > 8.0:
            self.issue(
                "warning",
                "section_loudness_step",
                f"Join into {cue.name!r} changes 100ms RMS by {rms_db:.1f}dB; verify that the transition is intentional.",
                track_name,
                cue.name,
            )

    def _check_structure(self, track_name: str, entries: list[tuple[Cue, FeatureSet]]) -> None:
        if not entries:
            self.issue("warning", "missing_form_metadata", "No section/cue data; structural checks were skipped.", track_name)
            return
        by_kind: dict[str, list[tuple[Cue, FeatureSet]]] = {}
        verses: list[tuple[Cue, FeatureSet]] = []
        choruses: list[tuple[Cue, FeatureSet]] = []
        for entry in entries:
            kind = entry[0].kind
            by_kind.setdefault(kind, []).append(entry)
            if kind.startswith("verse"):
                verses.append(entry)
            if kind in ("chorus", "final_chorus"):
                choruses.append(entry)

        explicit_v2 = by_kind.get("verse2", [])
        verse_pair = (verses[0], explicit_v2[0]) if verses and explicit_v2 and verses[0] != explicit_v2[0] else (verses[0], verses[1]) if len(verses) >= 2 else None
        if verse_pair:
            self._check_section_pair(track_name, verse_pair[0], verse_pair[1], "verse_repetition", 0.985, 0.94)
        else:
            self.issue("warning", "missing_verse_variation", "No distinct second verse was declared.", track_name)

        final_entries = by_kind.get("final_chorus", [])
        first_choruses = [entry for entry in choruses if entry[0].kind == "chorus"]
        if first_choruses and (final_entries or len(choruses) >= 2):
            final_entry = final_entries[-1] if final_entries else choruses[-1]
            self._check_section_pair(track_name, first_choruses[0], final_entry, "final_chorus_repetition", 0.995, 0.97)
        else:
            self.issue("warning", "missing_final_chorus_development", "Could not compare an opening and final chorus.", track_name)

        bridges = by_kind.get("bridge", []) + by_kind.get("breakdown", []) + by_kind.get("solo", [])
        transition_sections = by_kind.get("prechorus", []) + by_kind.get("build", [])
        if bridges and transition_sections:
            closest: tuple[dict[str, float], tuple[Cue, FeatureSet], tuple[Cue, FeatureSet]] | None = None
            for bridge in bridges:
                for transition in transition_sections:
                    similarity = _feature_similarity(bridge[1], transition[1])
                    if closest is None or similarity["combined"] > closest[0]["combined"]:
                        closest = (similarity, bridge, transition)
            assert closest is not None
            if closest[1][1].waveform_hash == closest[2][1].waveform_hash or closest[0]["combined"] > 0.985:
                self.issue(
                    "error",
                    "bridge_duplicates_transition",
                    f"{closest[1][0].name!r} is effectively the same realization as {closest[2][0].name!r}.",
                    track_name,
                    closest[1][0].name,
                    similarity=closest[0],
                )
            elif closest[0]["combined"] > 0.94:
                self.issue(
                    "warning",
                    "bridge_too_similar",
                    f"{closest[1][0].name!r} is unusually similar to {closest[2][0].name!r}.",
                    track_name,
                    closest[1][0].name,
                    similarity=closest[0],
                )
        elif not bridges:
            self.issue("warning", "missing_contrast_section", "No bridge, breakdown, or solo was declared.", track_name)

    def _check_section_pair(
        self,
        track_name: str,
        left: tuple[Cue, FeatureSet],
        right: tuple[Cue, FeatureSet],
        code: str,
        error_threshold: float,
        warning_threshold: float,
    ) -> None:
        similarity = _feature_similarity(left[1], right[1])
        exact_audio = left[1].waveform_hash == right[1].waveform_hash
        exact_melody = bool(left[1].melody_hash and left[1].melody_hash == right[1].melody_hash)
        if exact_audio or (similarity["combined"] >= error_threshold and similarity["energy"] >= 0.97):
            self.issue(
                "error",
                code,
                f"{right[0].name!r} is an effectively duplicated realization of {left[0].name!r}.",
                track_name,
                right[0].name,
                exact_audio=exact_audio,
                exact_melody=exact_melody,
                similarity=similarity,
            )
        elif similarity["combined"] >= warning_threshold:
            self.issue(
                "warning",
                f"{code}_similar",
                f"{right[0].name!r} may not develop enough beyond {left[0].name!r}.",
                track_name,
                right[0].name,
                exact_melody=exact_melody,
                similarity=similarity,
            )

    def _chord_signature(self, chords: list[dict[str, Any]], bpm: float, sample_rate: int) -> list[str]:
        parsed: list[tuple[float, str]] = []
        for chord in chords:
            beat = _event_beat(chord, bpm, sample_rate)
            root = _first(chord, ("root", "tonic", "pitch", "chord"))
            quality = str(_first(chord, ("quality", "type", "suffix"), ""))
            if beat is not None and root is not None:
                parsed.append((beat, f"{root}:{quality}"))
        parsed.sort(key=lambda item: item[0])
        return [value for _beat, value in parsed]

    def _check_album_duplicates(self) -> None:
        forms: dict[tuple[Any, ...], list[str]] = {}
        melodies: dict[str, list[str]] = {}
        role_stacks: dict[str, list[str]] = {}
        for track in self.results:
            form = tuple(zip(track.get("form_signature", []), track.get("form_duration_beats", [])))
            if len(form) >= 3:
                forms.setdefault(form, []).append(track["name"])
            melody_hash = track.get("melody_hash")
            if melody_hash:
                melodies.setdefault(melody_hash, []).append(track["name"])
            roles = track.get("active_roles")
            if roles:
                digest = json.dumps(roles, sort_keys=True, separators=(",", ":"))
                role_stacks.setdefault(digest, []).append(track["name"])

        for names in forms.values():
            if len(names) > 1:
                self.issue(
                    "error",
                    "duplicate_form",
                    f"Tracks share the exact same labeled form and section lengths: {', '.join(names)}.",
                    tracks=names,
                )
        for names in melodies.values():
            if len(names) > 1:
                self.issue(
                    "error",
                    "duplicate_melody",
                    f"Tracks share the same transposition-independent melody/rhythm signature: {', '.join(names)}.",
                    tracks=names,
                )
        for names in role_stacks.values():
            if len(names) > 1:
                self.issue(
                    "warning",
                    "duplicate_role_stack",
                    f"Tracks declare the same complete active-role arrangement: {', '.join(names)}.",
                    tracks=names,
                )

    def _pairwise_analysis(self) -> list[dict[str, Any]]:
        names = [track["name"] for track in self.results if track["name"] in self.features]
        comparisons: list[dict[str, Any]] = []
        for left_index, left_name in enumerate(names):
            for right_name in names[left_index + 1 :]:
                similarity = _feature_similarity(self.features[left_name], self.features[right_name])
                comparison = {"left": left_name, "right": right_name, **similarity}
                comparisons.append(comparison)
                same_melody = bool(
                    self.features[left_name].melody_hash
                    and self.features[left_name].melody_hash == self.features[right_name].melody_hash
                )
                too_similar = (
                    similarity["combined"] >= 0.93
                    and similarity["melodic"] >= 0.90
                    and (similarity["onset"] >= 0.86 or similarity["energy"] >= 0.92)
                )
                if same_melody or too_similar:
                    self.issue(
                        "error",
                        "cross_track_similarity",
                        f"{left_name!r} and {right_name!r} are too similar across melodic and arrangement proxies.",
                        similarity=similarity,
                    )
                elif similarity["combined"] >= 0.88 and similarity["melodic"] >= 0.86:
                    self.issue(
                        "warning",
                        "cross_track_similarity_high",
                        f"{left_name!r} and {right_name!r} have unusually similar identities.",
                        similarity=similarity,
                    )
        comparisons.sort(key=lambda item: item["combined"], reverse=True)
        return comparisons


def _format_report(report: dict[str, Any]) -> str:
    summary = report["summary"]
    lines = [
        f"GRAVITY WAVE SOUNDTRACK QA: {report['status'].upper()}",
        f"{summary['tracks_analyzed']} tracks | {summary['errors']} errors | {summary['warnings']} warnings",
        "",
    ]
    for track in report["tracks"]:
        transitions = track.get("transitions", [])
        max_jump = max((item.get("sample_discontinuity", 0.0) for item in transitions), default=0.0)
        max_hf = max((item.get("hf_burst_db_5ms", 0.0) for item in transitions), default=0.0)
        max_gap = max((item.get("near_zero_gap_ms", 0.0) for item in transitions), default=0.0)
        lines.append(
            f"- {track['name']}: {track['duration_seconds']:.2f}s, {track['bpm']:.1f} BPM, "
            f"peak {track['peak_dbfs']:.2f} dBFS, RMS {track['rms_dbfs']:.2f} dBFS, "
            f"DR {track['dynamic_range_db_p95_p10']:.2f} dB"
        )
        lines.append(
            f"  {len(track.get('sections', []))} sections; worst cue jump {max_jump:.4f} FS, "
            f"HF burst {max_hf:.1f} dB, silence gap {max_gap:.2f} ms"
        )
        if track.get("sections"):
            loudness = ", ".join(
                f"{section['name']} {section['loudness']['rms_dbfs']:.1f}dB"
                for section in track["sections"]
            )
            lines.append(f"  section RMS: {loudness}")

    if report["pairwise"]:
        lines.extend(("", "Closest cross-track identities:"))
        for pair in report["pairwise"][: min(8, len(report["pairwise"]))]:
            lines.append(
                f"- {pair['left']} / {pair['right']}: combined {pair['combined']:.3f}, "
                f"melody {pair['melodic']:.3f}, onset {pair['onset']:.3f}, energy {pair['energy']:.3f}"
            )

    if report["issues"]:
        lines.extend(("", "Issues:"))
        for issue in report["issues"]:
            location = issue.get("track", "album")
            if issue.get("section"):
                location += f" / {issue['section']}"
            lines.append(
                f"- {issue['severity'].upper()} [{issue['code']}] {location}: {issue['message']}"
            )
    else:
        lines.extend(("", "No signal-integrity, duplication, or structural proxy issues detected."))
    if report["strict"] and summary["warnings"]:
        lines.append("Strict mode treats warnings as failures.")
    return "\n".join(lines)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Analyze rendered Gravity Wave soundtrack WAVs and composition metadata."
    )
    parser.add_argument(
        "source",
        type=Path,
        help="render_soundtrack.py manifest JSON, a rendered WAV directory, or one WAV file",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="return failure for warnings as well as hard errors",
    )
    parser.add_argument(
        "--json-output",
        type=str,
        metavar="PATH",
        help="write the complete machine-readable report to PATH (use '-' for stdout)",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        manifest, manifest_path, base_dir = _resolve_manifest(args.source)
        analyzer = SoundtrackAnalyzer(manifest, manifest_path, base_dir, args.strict)
        report = _json_safe(analyzer.analyze())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"soundtrack QA setup failed: {exc}", file=sys.stderr)
        return 2

    print(_format_report(report))
    if args.json_output:
        encoded = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if args.json_output == "-":
            print("\nJSON REPORT")
            print(encoded, end="")
        else:
            output_path = Path(args.json_output).expanduser().resolve()
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_text(encoded, encoding="utf-8")
            print(f"\nJSON report: {output_path}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
