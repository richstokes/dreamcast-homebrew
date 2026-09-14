#!/usr/bin/env python3
"""Build Gravity Wave's streamed, whole-song ADPCM album.

The production path consumes the host renderer's JSON manifest and complete
stereo 16-bit PCM WAVs. A legacy mode still accepts the Dreamcast-side A/B/C
section export used by older tests. In both modes every complete song is
encoded with one continuous Yamaha ADPCM predictor history. The AICA predictor
is reset only when the runtime stops and restarts the stream.

Album format (all integers are little endian):

    0x00  char[4]  "GWAM"
    0x04  uint16   format version (1)
    0x06  uint16   track count (8)
    0x08  uint32   sample rate (21500)
    0x0c  uint32   payload offset (160)
    0x10  uint32   catalog fingerprint
    0x14  uint32   payload FNV-1a fingerprint
    0x18  uint32   payload byte count
    0x1c  uint32   reserved (0)
    0x20  8 * { uint32 payload_offset, stream_bytes,
                playable_frames, reserved }
    0xa0  32-byte-aligned interleaved ADPCM payload

For stereo 4-bit ADPCM, one interleaved byte contains one frame: the left
nibble is high and the right nibble is low.  ``stream_bytes`` includes an
encoded-silence guard for KOS's read-ahead; ``playable_frames`` does not.
"""

from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
from typing import Iterable, Sequence
import wave


ALBUM_MAGIC = b"GWAM"
ALBUM_VERSION = 1
TRACK_COUNT = 8
SECTION_COUNT = 3
FORM_STEP_COUNT = 18
SAMPLE_RATE = 21_500
EDGE_FRAMES = 1_024
FADE_FRAMES = 6_450
END_SILENCE_FRAMES = 3_225
STREAM_GUARD_FRAMES = 65_408
ALIGNMENT = 32
HEADER_BYTES = 32
ENTRY_BYTES = 16
DATA_OFFSET = HEADER_BYTES + TRACK_COUNT * ENTRY_BYTES

EXPECTED_TRACK_NAMES = (
    "MIDNIGHT VECTOR",
    "MAGENTA CIRCUIT",
    "GLASS HORIZON",
    "STATIC HEART",
    "AFTERIMAGE RUN",
    "NEON AFTERBURN",
    "CHROME DEVOTION",
    "REDLINE PROPHECY",
)
EXPECTED_TRACK_BPMS = (122.0, 138.0, 116.0, 132.0, 126.0, 148.0, 120.0, 144.0)
EXPECTED_GENERATOR = "gravity-wave-full-song-v1"

STEP_TABLE = (230, 230, 230, 230, 307, 409, 512, 614)
HEX_RE = re.compile(r"^[0-9a-fA-F]+$")
FINGERPRINT_RE = re.compile(r"^(?:0[xX])?([0-9a-fA-F]{1,8})$")


class AlbumBuildError(RuntimeError):
    """An invalid or incomplete music export."""


@dataclass(frozen=True)
class SectionExport:
    sample_frames: int
    data: bytes


@dataclass(frozen=True)
class ExportManifest:
    catalog_fingerprint: int
    sections: dict[tuple[int, int], SectionExport]
    forms: tuple[tuple[int, ...], ...]


@dataclass
class AdpcmState:
    history: int = 0
    step_size: int = 127


@dataclass(frozen=True)
class AlbumTrack:
    stream: bytes
    playable_frames: int


@dataclass(frozen=True)
class PcmTrackSpec:
    name: str
    path: Path
    music_frames: int
    playable_frames: int
    stream_frames: int


def _parse_decimal(token: str, description: str, line_number: int) -> int:
    try:
        return int(token, 10)
    except ValueError as exc:
        raise AlbumBuildError(
            f"line {line_number}: invalid {description}: {token!r}"
        ) from exc


def _finalize_section(
    key: tuple[int, int] | None,
    sample_frames: int,
    hex_chunks: list[str],
    sections: dict[tuple[int, int], SectionExport],
) -> None:
    if key is None:
        return
    try:
        data = bytes.fromhex("".join(hex_chunks))
    except ValueError as exc:
        raise AlbumBuildError(
            f"track {key[0]} section {key[1]} contains malformed hexadecimal"
        ) from exc
    if len(data) != sample_frames:
        raise AlbumBuildError(
            f"track {key[0]} section {key[1]} exported {len(data)} bytes; "
            f"expected {sample_frames}"
        )
    sections[key] = SectionExport(sample_frames=sample_frames, data=data)


def parse_export_log(path: Path) -> ExportManifest:
    """Parse and strictly validate a Flycast music-export serial log."""

    sections: dict[tuple[int, int], SectionExport] = {}
    form_rows: dict[tuple[int, int], tuple[int, int]] = {}
    fingerprints: list[int] = []
    format_rows: list[tuple[int, ...]] = []
    active_key: tuple[int, int] | None = None
    active_frames = 0
    active_hex: list[str] = []

    try:
        log_file = path.open("r", encoding="utf-8", errors="replace")
    except OSError as exc:
        raise AlbumBuildError(f"cannot read export log {path}: {exc}") from exc

    with log_file:
        for line_number, raw_line in enumerate(log_file, start=1):
            line = raw_line.strip()
            if line.startswith("GW_MUSIC_SECTION "):
                _finalize_section(
                    active_key, active_frames, active_hex, sections
                )
                fields = line.split()
                if len(fields) != 4:
                    raise AlbumBuildError(
                        f"line {line_number}: malformed GW_MUSIC_SECTION row"
                    )
                track = _parse_decimal(fields[1], "track index", line_number)
                section = _parse_decimal(
                    fields[2], "section index", line_number
                )
                sample_frames = _parse_decimal(
                    fields[3], "section frame count", line_number
                )
                key = (track, section)
                if not 0 <= track < TRACK_COUNT:
                    raise AlbumBuildError(
                        f"line {line_number}: track index {track} is out of range"
                    )
                if not 0 <= section < SECTION_COUNT:
                    raise AlbumBuildError(
                        f"line {line_number}: section index {section} is out of range"
                    )
                if key in sections or key == active_key:
                    raise AlbumBuildError(
                        f"line {line_number}: duplicate track {track} section {section}"
                    )
                if sample_frames <= EDGE_FRAMES:
                    raise AlbumBuildError(
                        f"line {line_number}: section must exceed its "
                        f"{EDGE_FRAMES}-frame tail"
                    )
                if sample_frames % (ALIGNMENT * 2) != 0:
                    raise AlbumBuildError(
                        f"line {line_number}: section frame count {sample_frames} "
                        "is not a multiple of 64"
                    )
                active_key = key
                active_frames = sample_frames
                active_hex = []
                continue

            if line.startswith("GWHEX "):
                if active_key is None:
                    raise AlbumBuildError(
                        f"line {line_number}: GWHEX data has no section header"
                    )
                fields = line.split()
                if (
                    len(fields) != 2
                    or len(fields[1]) % 2 != 0
                    or not HEX_RE.fullmatch(fields[1])
                ):
                    raise AlbumBuildError(
                        f"line {line_number}: malformed GWHEX payload"
                    )
                active_hex.append(fields[1])
                continue

            if line.startswith("GW_MUSIC_FINGERPRINT "):
                fields = line.split()
                if len(fields) != 2:
                    raise AlbumBuildError(
                        f"line {line_number}: malformed GW_MUSIC_FINGERPRINT row"
                    )
                match = FINGERPRINT_RE.fullmatch(fields[1])
                if match is None:
                    raise AlbumBuildError(
                        f"line {line_number}: invalid catalog fingerprint "
                        f"{fields[1]!r}"
                    )
                fingerprints.append(int(match.group(1), 16))
                continue

            if line.startswith("GW_MUSIC_FORMAT "):
                fields = line.split()
                if len(fields) != 8:
                    raise AlbumBuildError(
                        f"line {line_number}: malformed GW_MUSIC_FORMAT row"
                    )
                format_rows.append(
                    tuple(
                        _parse_decimal(token, "format value", line_number)
                        for token in fields[1:]
                    )
                )
                continue

            if line.startswith("GW_MUSIC_FORM "):
                fields = line.split()
                if len(fields) != 5:
                    raise AlbumBuildError(
                        f"line {line_number}: malformed GW_MUSIC_FORM row"
                    )
                track = _parse_decimal(fields[1], "track index", line_number)
                step = _parse_decimal(fields[2], "form step", line_number)
                section = _parse_decimal(
                    fields[3], "section index", line_number
                )
                volume_delta = _parse_decimal(
                    fields[4], "volume delta", line_number
                )
                if not 0 <= track < TRACK_COUNT:
                    raise AlbumBuildError(
                        f"line {line_number}: track index {track} is out of range"
                    )
                if not 0 <= step < FORM_STEP_COUNT:
                    raise AlbumBuildError(
                        f"line {line_number}: form step {step} is out of range"
                    )
                if not 0 <= section < SECTION_COUNT:
                    raise AlbumBuildError(
                        f"line {line_number}: section index {section} is out of range"
                    )
                if not -255 <= volume_delta <= 255:
                    raise AlbumBuildError(
                        f"line {line_number}: volume delta {volume_delta} is implausible"
                    )
                key = (track, step)
                if key in form_rows:
                    raise AlbumBuildError(
                        f"line {line_number}: duplicate track {track} form step {step}"
                    )
                # The full-song master owns its energy curve. Retain the
                # legacy delta only to validate the authored form manifest.
                form_rows[key] = (section, volume_delta)

    _finalize_section(active_key, active_frames, active_hex, sections)

    expected_sections = TRACK_COUNT * SECTION_COUNT
    if len(sections) != expected_sections:
        missing = [
            f"{track}:{section}"
            for track in range(TRACK_COUNT)
            for section in range(SECTION_COUNT)
            if (track, section) not in sections
        ]
        raise AlbumBuildError(
            f"export contains {len(sections)}/{expected_sections} sections; "
            f"missing {', '.join(missing) or 'unknown'}"
        )

    expected_form_rows = TRACK_COUNT * FORM_STEP_COUNT
    if len(form_rows) != expected_form_rows:
        missing = [
            f"{track}:{step}"
            for track in range(TRACK_COUNT)
            for step in range(FORM_STEP_COUNT)
            if (track, step) not in form_rows
        ]
        raise AlbumBuildError(
            f"export contains {len(form_rows)}/{expected_form_rows} form rows; "
            f"missing {', '.join(missing) or 'unknown'}"
        )

    if not fingerprints:
        raise AlbumBuildError("export contains no GW_MUSIC_FINGERPRINT row")
    if any(value != fingerprints[0] for value in fingerprints[1:]):
        raise AlbumBuildError("export contains conflicting catalog fingerprints")
    expected_format = (
        ALBUM_VERSION,
        SAMPLE_RATE,
        EDGE_FRAMES,
        FORM_STEP_COUNT,
        FADE_FRAMES,
        END_SILENCE_FRAMES,
        STREAM_GUARD_FRAMES,
    )
    if format_rows and any(row != expected_format for row in format_rows):
        raise AlbumBuildError(
            "export format does not match the album builder "
            f"(expected {' '.join(str(value) for value in expected_format)})"
        )

    forms = tuple(
        tuple(form_rows[(track, step)][0] for step in range(FORM_STEP_COUNT))
        for track in range(TRACK_COUNT)
    )
    return ExportManifest(
        catalog_fingerprint=fingerprints[0],
        sections=sections,
        forms=forms,
    )


def _strict_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise AlbumBuildError(f"manifest contains duplicate key {key!r}")
        result[key] = value
    return result


def _manifest_integer(
    mapping: dict[str, object], key: str, context: str
) -> int:
    value = mapping.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise AlbumBuildError(f"{context}: {key} must be an integer")
    if value < 0 or value > 0xFFFF_FFFF:
        raise AlbumBuildError(f"{context}: {key} is outside uint32 range")
    return value


def parse_pcm_manifest(path: Path) -> tuple[int, tuple[PcmTrackSpec, ...]]:
    """Validate the host-renderer manifest and resolve its eight WAVs."""

    try:
        document = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_strict_json_object,
            parse_constant=lambda value: (_ for _ in ()).throw(
                AlbumBuildError(
                    f"manifest contains non-finite JSON value {value!r}"
                )
            ),
        )
    except AlbumBuildError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AlbumBuildError(f"cannot parse manifest {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise AlbumBuildError("manifest root must be a JSON object")
    if document.get("generator") != EXPECTED_GENERATOR:
        raise AlbumBuildError(
            f"manifest generator must be {EXPECTED_GENERATOR!r}"
        )
    if document.get("deterministic") is not True:
        raise AlbumBuildError("manifest must declare a deterministic render")
    if document.get("quality") != "production":
        raise AlbumBuildError(
            "manifest must contain production masters; quick renders cannot be packaged"
        )

    sample_rate = _manifest_integer(document, "sample_rate", "manifest")
    if sample_rate != SAMPLE_RATE:
        raise AlbumBuildError(
            f"manifest sample_rate is {sample_rate}; runtime requires {SAMPLE_RATE}"
        )
    playable_bars = _manifest_integer(document, "playable_bars", "manifest")
    if playable_bars != 36:
        raise AlbumBuildError(
            f"manifest contains {playable_bars} authored bars; exactly 36 required"
        )
    track_rows = document.get("tracks")
    if not isinstance(track_rows, list) or len(track_rows) != TRACK_COUNT:
        count = len(track_rows) if isinstance(track_rows, list) else 0
        raise AlbumBuildError(
            f"manifest contains {count}/{TRACK_COUNT} tracks"
        )

    try:
        manifest_dir = path.resolve(strict=True).parent
    except OSError as exc:
        raise AlbumBuildError(f"cannot resolve manifest {path}: {exc}") from exc
    resolved_files: set[Path] = set()
    specs: list[PcmTrackSpec] = []
    for index, row in enumerate(track_rows):
        context = f"manifest track {index + 1}"
        if not isinstance(row, dict):
            raise AlbumBuildError(f"{context} must be a JSON object")
        name = row.get("name")
        if name != EXPECTED_TRACK_NAMES[index]:
            raise AlbumBuildError(
                f"{context} name/order mismatch: expected "
                f"{EXPECTED_TRACK_NAMES[index]!r}, found {name!r}"
            )
        file_name = row.get("file")
        if not isinstance(file_name, str) or not file_name.strip():
            raise AlbumBuildError(f"{context}: file must be a non-empty string")
        relative_path = Path(file_name)
        if relative_path.is_absolute() or relative_path.suffix.lower() != ".wav":
            raise AlbumBuildError(
                f"{context}: file must be a relative .wav path"
            )
        try:
            wav_path = (manifest_dir / relative_path).resolve(strict=True)
            wav_path.relative_to(manifest_dir)
        except (OSError, ValueError) as exc:
            raise AlbumBuildError(
                f"{context}: WAV escapes the manifest directory or is missing: "
                f"{file_name!r}"
            ) from exc
        if not wav_path.is_file() or wav_path in resolved_files:
            raise AlbumBuildError(f"{context}: WAV path is not a unique file")
        resolved_files.add(wav_path)

        track_rate = row.get("sample_rate", sample_rate)
        if isinstance(track_rate, bool) or not isinstance(track_rate, int) or \
           track_rate != sample_rate:
            raise AlbumBuildError(
                f"{context}: sample_rate must match {sample_rate}"
            )
        bars = row.get("bars", playable_bars)
        if isinstance(bars, bool) or not isinstance(bars, int) or \
           bars != playable_bars:
            raise AlbumBuildError(
                f"{context}: bars must match the 36-bar runtime form"
            )
        if row.get("quality") != "production":
            raise AlbumBuildError(
                f"{context}: only production-quality masters may be packaged"
            )
        music_frames = _manifest_integer(row, "music_frames", context)
        playable_frames = _manifest_integer(row, "playable_frames", context)
        stream_frames = _manifest_integer(row, "stream_frames", context)
        if music_frames == 0 or playable_frames <= music_frames:
            raise AlbumBuildError(
                f"{context}: playable_frames must include a tail after music_frames"
            )
        if stream_frames <= playable_frames:
            raise AlbumBuildError(
                f"{context}: stream_frames must include a guard after playable_frames"
            )
        guard_frames = stream_frames - playable_frames
        if guard_frames < STREAM_GUARD_FRAMES:
            raise AlbumBuildError(
                f"{context}: guard is {guard_frames} frames; "
                f"at least {STREAM_GUARD_FRAMES} required"
            )
        if stream_frames % ALIGNMENT != 0:
            raise AlbumBuildError(
                f"{context}: stream_frames must be {ALIGNMENT}-frame aligned"
            )
        if "frames" in row and _manifest_integer(row, "frames", context) != \
           stream_frames:
            raise AlbumBuildError(
                f"{context}: frames and stream_frames disagree"
            )
        if "guard_frames" in row and \
           _manifest_integer(row, "guard_frames", context) != guard_frames:
            raise AlbumBuildError(
                f"{context}: guard_frames metadata disagrees with frame bounds"
            )
        bpm = row.get("bpm")
        if isinstance(bpm, bool) or not isinstance(bpm, (int, float)) or \
           not math.isfinite(float(bpm)) or float(bpm) <= 0.0:
            raise AlbumBuildError(
                f"{context}: positive finite bpm is required to verify 36 bars"
            )
        if abs(float(bpm) - EXPECTED_TRACK_BPMS[index]) > 0.001:
            raise AlbumBuildError(
                f"{context}: bpm must match the runtime catalog "
                f"({EXPECTED_TRACK_BPMS[index]:g})"
            )
        expected_music_frames = int(round(
            bars * 4.0 * 60.0 / float(bpm) * sample_rate
        ))
        if abs(music_frames - expected_music_frames) > 1:
            raise AlbumBuildError(
                f"{context}: music_frames={music_frames} does not describe "
                f"{bars} bars at {float(bpm):g} BPM ({expected_music_frames})"
            )
        specs.append(PcmTrackSpec(
            name=name,
            path=wav_path,
            music_frames=music_frames,
            playable_frames=playable_frames,
            stream_frames=stream_frames,
        ))
    return sample_rate, tuple(specs)


def _clamp16(value: int) -> int:
    return max(-32_768, min(32_767, value))


def _truncate_division(numerator: int, denominator: int) -> int:
    """C99 signed integer division (truncate toward zero)."""

    if numerator < 0:
        return -((-numerator) // denominator)
    return numerator // denominator


def _decode_nibble(code: int, state: AdpcmState) -> int:
    magnitude = code & 7
    difference = ((1 + (magnitude << 1)) * state.step_size) >> 3
    difference = max(0, min(32_767, difference))
    # The AICA Yamaha decoder applies this small high-pass before every
    # predictor step.  Match KOS's wav2adpcm decoder exactly.
    state.history = _truncate_division(state.history * 254, 256)
    if code & 8:
        state.history -= difference
    else:
        state.history += difference
    state.history = _clamp16(state.history)
    state.step_size = max(
        127, min(24_576, (STEP_TABLE[magnitude] * state.step_size) >> 8)
    )
    return state.history


def decode_planar_channel(data: bytes) -> array:
    """Decode low-nibble-first Yamaha ADPCM into signed 16-bit samples."""

    output = array("h")
    state = AdpcmState()
    append = output.append
    for packed in data:
        append(_decode_nibble(packed & 0x0F, state))
        append(_decode_nibble((packed >> 4) & 0x0F, state))
    return output


def decode_section(section: SectionExport) -> tuple[array, array]:
    if len(section.data) % 2 != 0:
        raise AlbumBuildError("planar stereo section has an odd byte count")
    channel_bytes = len(section.data) // 2
    left = decode_planar_channel(section.data[:channel_bytes])
    right = decode_planar_channel(section.data[channel_bytes:])
    if len(left) != section.sample_frames or len(right) != section.sample_frames:
        raise AlbumBuildError("decoded section frame count does not match manifest")
    return left, right


def _overlap_add(
    destination: array,
    phrase: Sequence[int],
    previous_tail: Sequence[int] | None,
) -> None:
    if previous_tail is None:
        destination.extend(phrase)
        return
    if len(previous_tail) != EDGE_FRAMES or len(phrase) < EDGE_FRAMES:
        raise AlbumBuildError("section tail cannot be overlap-added")
    destination.extend(
        _clamp16(phrase[index] + previous_tail[index])
        for index in range(EDGE_FRAMES)
    )
    destination.extend(phrase[EDGE_FRAMES:])


def _fade_tail(channel: array, frames: int) -> None:
    fade_frames = min(frames, len(channel))
    if fade_frames <= 0:
        return
    start = len(channel) - fade_frames
    denominator = max(1, fade_frames - 1)
    for offset in range(fade_frames):
        numerator = denominator - offset
        sample = channel[start + offset]
        if sample < 0:
            channel[start + offset] = -(
                ((-sample) * numerator + denominator // 2) // denominator
            )
        else:
            channel[start + offset] = (
                sample * numerator + denominator // 2
            ) // denominator


def assemble_song(
    track: int,
    manifest: ExportManifest,
    decoded_sections: dict[tuple[int, int], tuple[array, array]],
) -> tuple[array, array, int]:
    """Arrange one song and return left, right, and playable frame count."""

    section_frames = {
        manifest.sections[(track, section)].sample_frames
        for section in range(SECTION_COUNT)
    }
    if len(section_frames) != 1:
        raise AlbumBuildError(
            f"track {track} sections have inconsistent frame counts"
        )
    phrase_frames = section_frames.pop() - EDGE_FRAMES
    if phrase_frames < EDGE_FRAMES:
        raise AlbumBuildError(
            f"track {track} phrase is too short for a {EDGE_FRAMES}-frame overlap"
        )

    left_song = array("h")
    right_song = array("h")
    previous_left_tail: Sequence[int] | None = None
    previous_right_tail: Sequence[int] | None = None
    final_left_tail: Sequence[int] | None = None
    final_right_tail: Sequence[int] | None = None

    for section_index in manifest.forms[track]:
        left_section, right_section = decoded_sections[(track, section_index)]
        _overlap_add(
            left_song,
            left_section[:phrase_frames],
            previous_left_tail,
        )
        _overlap_add(
            right_song,
            right_section[:phrase_frames],
            previous_right_tail,
        )
        final_left_tail = left_section[phrase_frames:]
        final_right_tail = right_section[phrase_frames:]
        previous_left_tail = final_left_tail
        previous_right_tail = final_right_tail

    if final_left_tail is None or final_right_tail is None:
        raise AlbumBuildError(f"track {track} has an empty form")
    left_song.extend(final_left_tail)
    right_song.extend(final_right_tail)

    _fade_tail(left_song, FADE_FRAMES)
    _fade_tail(right_song, FADE_FRAMES)
    left_song.extend(array("h", [0]) * END_SILENCE_FRAMES)
    right_song.extend(array("h", [0]) * END_SILENCE_FRAMES)
    playable_frames = len(left_song)

    left_song.extend(array("h", [0]) * STREAM_GUARD_FRAMES)
    right_song.extend(array("h", [0]) * STREAM_GUARD_FRAMES)
    padding_frames = (-len(left_song)) % ALIGNMENT
    if padding_frames:
        left_song.extend(array("h", [0]) * padding_frames)
        right_song.extend(array("h", [0]) * padding_frames)

    if len(left_song) != len(right_song):
        raise AlbumBuildError(f"track {track} channel lengths diverged")
    return left_song, right_song, playable_frames


def _encode_nibble(sample: int, state: AdpcmState) -> int:
    # Match the public-domain encoder math used by Gravity Wave and KOS's
    # wav2adpcm utility.  Encoder prediction is deliberately continuous.
    difference = (sample & -8) - state.history
    magnitude = (abs(difference) * 4) // state.step_size
    magnitude = min(7, magnitude)
    code = magnitude | (8 if difference < 0 else 0)
    decoded_difference = ((1 + (magnitude << 1)) * state.step_size) >> 3
    decoded_difference = max(0, min(32_767, decoded_difference))
    if difference < 0:
        state.history -= decoded_difference
    else:
        state.history += decoded_difference
    state.history = _clamp16(state.history)
    state.step_size = max(
        127, min(24_576, (STEP_TABLE[magnitude] * state.step_size) >> 8)
    )
    return code


def read_pcm16_stereo(spec: PcmTrackSpec, sample_rate: int) -> bytes:
    """Read one strict, uncompressed, little-endian stereo PCM16 WAV."""

    try:
        with wave.open(str(spec.path), "rb") as wav_file:
            channels = wav_file.getnchannels()
            width = wav_file.getsampwidth()
            rate = wav_file.getframerate()
            frames = wav_file.getnframes()
            compression = wav_file.getcomptype()
            if channels != 2 or width != 2 or rate != sample_rate or \
               compression != "NONE":
                raise AlbumBuildError(
                    f"{spec.name}: WAV must be uncompressed stereo 16-bit "
                    f"PCM at {sample_rate} Hz (found channels={channels}, "
                    f"width={width}, rate={rate}, compression={compression})"
                )
            if frames != spec.stream_frames:
                raise AlbumBuildError(
                    f"{spec.name}: WAV has {frames} frames; manifest says "
                    f"{spec.stream_frames}"
                )
            pcm = wav_file.readframes(frames)
            if wav_file.readframes(1):
                raise AlbumBuildError(
                    f"{spec.name}: WAV contains unreported sample frames"
                )
    except AlbumBuildError:
        raise
    except (OSError, EOFError, wave.Error) as exc:
        raise AlbumBuildError(
            f"{spec.name}: cannot read strict PCM WAV {spec.path}: {exc}"
        ) from exc
    expected_bytes = spec.stream_frames * 4
    if len(pcm) != expected_bytes:
        raise AlbumBuildError(
            f"{spec.name}: WAV data is truncated ({len(pcm)}/{expected_bytes} bytes)"
        )
    guard = pcm[spec.playable_frames * 4:]
    if len(guard) < STREAM_GUARD_FRAMES * 4 or any(guard):
        raise AlbumBuildError(
            f"{spec.name}: stream guard must contain at least "
            f"{STREAM_GUARD_FRAMES} frames of exact digital zero"
        )
    return pcm


def encode_pcm16_interleaved(pcm: bytes, frame_count: int) -> bytes:
    """Encode an interleaved PCM16 song without resetting either predictor."""

    samples = array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    if len(samples) != frame_count * 2:
        raise AlbumBuildError("PCM sample count does not match frame metadata")
    left_state = AdpcmState()
    right_state = AdpcmState()
    output = bytearray(frame_count)
    for frame in range(frame_count):
        left_code = _encode_nibble(samples[frame * 2], left_state)
        right_code = _encode_nibble(samples[frame * 2 + 1], right_state)
        output[frame] = (left_code << 4) | right_code
    return bytes(output)


def build_pcm_manifest_tracks(
    manifest_path: Path,
) -> tuple[AlbumTrack, ...]:
    sample_rate, specs = parse_pcm_manifest(manifest_path)
    tracks: list[AlbumTrack] = []
    for index, spec in enumerate(specs):
        print(
            f"Encoding {index + 1:02d}/{TRACK_COUNT:02d} {spec.name}: "
            f"{spec.stream_frames} continuous frames...",
            flush=True,
        )
        pcm = read_pcm16_stereo(spec, sample_rate)
        stream = encode_pcm16_interleaved(pcm, spec.stream_frames)
        if len(stream) != spec.stream_frames or len(stream) % ALIGNMENT != 0:
            raise AlbumBuildError(
                f"{spec.name}: encoded stream length/alignment is invalid"
            )
        tracks.append(AlbumTrack(
            stream=stream,
            playable_frames=spec.playable_frames,
        ))
    return tuple(tracks)


def encode_interleaved(left: Sequence[int], right: Sequence[int]) -> bytes:
    """Encode a whole stereo song with one persistent state per channel."""

    if len(left) != len(right):
        raise AlbumBuildError("cannot encode stereo channels of different lengths")
    left_state = AdpcmState()
    right_state = AdpcmState()
    output = bytearray(len(left))
    for index in range(len(left)):
        left_code = _encode_nibble(left[index], left_state)
        right_code = _encode_nibble(right[index], right_state)
        output[index] = (left_code << 4) | right_code
    return bytes(output)


def build_tracks(manifest: ExportManifest) -> tuple[AlbumTrack, ...]:
    decoded_sections = {
        key: decode_section(section)
        for key, section in manifest.sections.items()
    }
    tracks: list[AlbumTrack] = []
    for track in range(TRACK_COUNT):
        left, right, playable_frames = assemble_song(
            track, manifest, decoded_sections
        )
        stream = encode_interleaved(left, right)
        if len(stream) % ALIGNMENT != 0:
            raise AlbumBuildError(
                f"track {track} stream is not {ALIGNMENT}-byte aligned"
            )
        if playable_frames <= 0 or playable_frames >= len(stream):
            raise AlbumBuildError(f"track {track} playable length is invalid")
        tracks.append(
            AlbumTrack(stream=stream, playable_frames=playable_frames)
        )
    return tuple(tracks)


def fnv1a32(data: Iterable[int]) -> int:
    value = 2_166_136_261
    for byte in data:
        value ^= byte
        value = (value * 16_777_619) & 0xFFFF_FFFF
    return value


def serialize_album(
    catalog_fingerprint: int,
    tracks: Sequence[AlbumTrack],
) -> tuple[bytes, int]:
    if len(tracks) != TRACK_COUNT:
        raise AlbumBuildError(
            f"cannot serialize {len(tracks)}/{TRACK_COUNT} album tracks"
        )
    payload = bytearray()
    entries = bytearray()

    for track_index, track in enumerate(tracks):
        padding = (-len(payload)) % ALIGNMENT
        if padding:
            payload.extend(b"\0" * padding)
        offset = len(payload)
        if offset % ALIGNMENT != 0 or len(track.stream) % ALIGNMENT != 0:
            raise AlbumBuildError(
                f"track {track_index} violates {ALIGNMENT}-byte alignment"
            )
        if not 0 < track.playable_frames <= len(track.stream):
            raise AlbumBuildError(
                f"track {track_index} has invalid playable frame metadata"
            )
        if len(track.stream) - track.playable_frames < STREAM_GUARD_FRAMES:
            raise AlbumBuildError(
                f"track {track_index} lacks the KOS read-ahead guard"
            )
        if offset > 0xFFFF_FFFF or len(track.stream) > 0xFFFF_FFFF or \
           track.playable_frames > 0xFFFF_FFFF:
            raise AlbumBuildError(
                f"track {track_index} exceeds the GWAM uint32 table"
            )
        entries.extend(
            struct.pack(
                "<IIII",
                offset,
                len(track.stream),
                track.playable_frames,
                0,
            )
        )
        payload.extend(track.stream)

    if len(entries) != TRACK_COUNT * ENTRY_BYTES:
        raise AlbumBuildError("internal album table size mismatch")
    if len(payload) > 0xFFFF_FFFF:
        raise AlbumBuildError("album payload exceeds the GWAM uint32 header")
    payload_fingerprint = fnv1a32(payload)
    header = struct.pack(
        "<4sHHIIIIII",
        ALBUM_MAGIC,
        ALBUM_VERSION,
        TRACK_COUNT,
        SAMPLE_RATE,
        DATA_OFFSET,
        catalog_fingerprint,
        payload_fingerprint,
        len(payload),
        0,
    )
    if len(header) != HEADER_BYTES or DATA_OFFSET != 160:
        raise AlbumBuildError("internal album header size mismatch")
    album = header + bytes(entries) + bytes(payload)
    if len(album) != DATA_OFFSET + len(payload):
        raise AlbumBuildError("internal album size mismatch")
    return album, payload_fingerprint


def build_album(manifest: ExportManifest) -> tuple[bytes, int]:
    """Build an album from the retained legacy A/B/C export format."""

    return serialize_album(manifest.catalog_fingerprint, build_tracks(manifest))


def write_album_atomic(output_path: Path, album: bytes) -> None:
    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{output_path.name}.",
            dir=output_path.parent,
            delete=False,
        ) as output_file:
            temporary_path = Path(output_file.name)
            output_file.write(album)
            output_file.flush()
            os.fsync(output_file.fileno())
        os.replace(temporary_path, output_path)
    except OSError as exc:
        try:
            temporary_path.unlink(missing_ok=True)
        except (OSError, UnboundLocalError):
            pass
        raise AlbumBuildError(f"cannot write album {output_path}: {exc}") from exc


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="assemble Gravity Wave's streamed ADPCM album"
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--log",
        type=Path,
        help="legacy Flycast A/B/C section-export serial log",
    )
    source.add_argument(
        "--manifest",
        type=Path,
        help="soundtrack_manifest.json plus its complete PCM WAV files",
    )
    parser.add_argument(
        "--catalog-fingerprint",
        metavar="HEX",
        help="runtime catalog fingerprint (required with --manifest)",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="destination gravity_wave_music.adpcm file",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        if arguments.manifest is not None:
            if arguments.catalog_fingerprint is None:
                raise AlbumBuildError(
                    "--catalog-fingerprint HEX is required with --manifest"
                )
            match = FINGERPRINT_RE.fullmatch(arguments.catalog_fingerprint)
            if match is None:
                raise AlbumBuildError(
                    f"invalid catalog fingerprint "
                    f"{arguments.catalog_fingerprint!r}"
                )
            catalog_fingerprint = int(match.group(1), 16)
            tracks = build_pcm_manifest_tracks(arguments.manifest)
            album, payload_fingerprint = serialize_album(
                catalog_fingerprint, tracks
            )
        else:
            if arguments.catalog_fingerprint is not None:
                raise AlbumBuildError(
                    "--catalog-fingerprint is only valid with --manifest"
                )
            legacy_manifest = parse_export_log(arguments.log)
            catalog_fingerprint = legacy_manifest.catalog_fingerprint
            album, payload_fingerprint = build_album(legacy_manifest)
        write_album_atomic(arguments.output, album)
    except AlbumBuildError as exc:
        print(f"build_music_album.py: error: {exc}", file=sys.stderr)
        return 1

    print(
        f"Wrote {arguments.output} ({len(album)} bytes, "
        f"catalog {catalog_fingerprint:08x}, "
        f"payload {payload_fingerprint:08x})."
    )
    return 0



if __name__ == "__main__":
    raise SystemExit(main())
