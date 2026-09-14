#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "numpy==2.5.2",
#   "scipy==1.18.1",
# ]
# ///
"""Render Gravity Wave's original full-length soundtrack.

This is a deterministic, host-side composition and mastering tool.  Unlike the
legacy three-clip score, every song is rendered as one continuous timeline:
instrument releases, delay, reverb, fills, risers, and section automation all
cross section boundaries naturally.  The resulting WAV is suitable for later
conversion to interleaved stereo AICA ADPCM, which KOS splits into the two
planar sound-RAM channels while streaming.

Run with:

    uv run tools/render_soundtrack.py --output-dir /tmp/gravity-wave-wav

The first 36 bars are the authored song.  An audible effects/fade tail follows,
then a large silent stream guard.  ``playable_frames`` in the JSON manifest
includes the tail but excludes the guard; ``stream_frames`` and ``frames``
describe the complete WAV.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, lfilter, sosfilt


GENERATOR_VERSION = "gravity-wave-full-song-v1"
DEFAULT_SAMPLE_RATE = 21_500
PLAYABLE_BARS = 36
BEATS_PER_BAR = 4
PHRASE_BEATS = 16
TAIL_SECONDS = 2.6
MIN_GUARD_FRAMES = 65_408

# Track-specific second-statement development.  Values are semitone changes
# for the opening, midpoint, and cadence notes; the timing index gets a small
# written hesitation.  This keeps six/eight-bar sections from replaying an
# identical four-bar array while retaining each song's hook.
ANSWER_RULES: dict[str, tuple[int, int, int, int]] = {
    "midnight-vector": (2, -2, 4, 1),
    "magenta-circuit": (-2, 3, -5, 4),
    "glass-horizon": (0, 5, -3, 2),
    "static-heart": (3, 1, -2, 5),
    "afterimage-run": (1, -1, 6, 3),
    "neon-afterburn": (5, -2, 7, 4),
    "chrome-devotion": (2, 4, -5, 1),
    "redline-prophecy": (-1, 6, 1, 2),
}


@dataclass(frozen=True)
class Note:
    beat: float
    duration: float
    pitch: int
    velocity: float = 0.82
    articulation: str = "legato"


@dataclass(frozen=True)
class Harmony:
    beat: float
    duration: float
    root: int
    quality: str


@dataclass(frozen=True)
class Section:
    name: str
    bars: int
    harmony: str
    phrase: str | None
    energy: float
    drums: float
    bass: float
    comp: float
    lead: float
    arp: float = 0.0
    variant: str = "base"


@dataclass(frozen=True)
class Track:
    name: str
    slug: str
    bpm: float
    tonic_midi: int
    lead_root_midi: int
    mode: str
    identity: str
    lead_voice: str
    comp_voice: str
    bass_voice: str
    drum_voice: str
    arp_voice: str | None
    swing: float
    seed: int
    sections: tuple[Section, ...]
    harmony: dict[str, tuple[Harmony, ...]]
    phrases: dict[str, tuple[Note, ...]]


def N(beat: float, duration: float, pitch: int, velocity: float = 0.82,
      articulation: str = "legato") -> Note:
    return Note(beat, duration, pitch, velocity, articulation)


def H(beat: float, duration: float, root: int, quality: str) -> Harmony:
    return Harmony(beat, duration, root, quality)


def S(name: str, bars: int, harmony: str, phrase: str | None,
      energy: float, drums: float, bass: float, comp: float, lead: float,
      arp: float = 0.0, variant: str = "base") -> Section:
    return Section(name, bars, harmony, phrase, energy, drums, bass, comp,
                   lead, arp, variant)


def four_bar_chords(*chords: tuple[int, str]) -> tuple[Harmony, ...]:
    """Convenience for data entry, not a shared progression generator."""
    if len(chords) != 4:
        raise ValueError("four_bar_chords requires four explicitly authored chords")
    return tuple(H(i * 4.0, 4.0, root, quality)
                 for i, (root, quality) in enumerate(chords))


# Every phrase below is independently authored on a 16-beat/four-bar canvas.
# Pitch is a semitone offset from the track's lead root.  The first two bars are
# the antecedent and the latter two answer or develop it.  Long durations and
# written rests keep the album out of the old universal sequencer cadence.
TRACKS: tuple[Track, ...] = (
    Track(
        name="MIDNIGHT VECTOR", slug="midnight-vector", bpm=122.0,
        tonic_midi=52, lead_root_midi=64, mode="E major / Lydian",
        identity="Dotted headlight calls, rising fifths, vocal lead and one glass-arp showcase.",
        lead_voice="vocal_formant", comp_voice="poly_brass",
        bass_voice="rubber_bass", drum_voice="night_drive",
        arp_voice="glass_arp", swing=0.0, seed=0x4D560122,
        sections=(
            S("INTRO", 4, "intro", "verse", .48, .25, .38, .62, .34, .82, "intro"),
            S("VERSE 1", 8, "verse", "verse", .68, .66, .78, .70, .72, .48),
            S("PRE-CHORUS", 4, "pre", "pre", .82, .78, .84, .88, .76, .28, "rise"),
            S("CHORUS", 4, "chorus", "chorus", 1.0, 1.0, 1.0, 1.0, 1.0, .34),
            S("VERSE 2", 4, "verse2", "verse2", .74, .73, .86, .74, .82, .22, "develop"),
            S("BRIDGE", 4, "bridge", "bridge", .56, .22, .00, .72, .76, .00, "subtract"),
            S("FINAL CHORUS", 4, "final", "final", 1.08, 1.0, 1.0, 1.0, 1.06, .36, "lift"),
            S("OUTRO", 4, "outro", "outro", .42, .30, .42, .58, .38, .40, "fade"),
        ),
        harmony={
            "intro": four_bar_chords((0, "maj9"), (2, "maj"), (11, "min7"), (7, "add9")),
            "verse": four_bar_chords((0, "maj9"), (6, "min7"), (2, "maj"), (7, "add9")),
            "pre": (H(0, 4, 9, "min7"), H(4, 4, 11, "min7"), H(8, 2, 2, "maj"), H(10, 2, 4, "min7"), H(12, 4, 7, "sus4")),
            "chorus": four_bar_chords((0, "maj"), (2, "maj"), (4, "min7"), (7, "add9")),
            "verse2": four_bar_chords((0, "maj9"), (11, "min7"), (6, "min7"), (2, "maj")),
            "bridge": four_bar_chords((9, "min9"), (4, "min7"), (2, "maj"), (7, "sus4")),
            "final": four_bar_chords((0, "maj9"), (2, "maj"), (9, "min7"), (7, "add9")),
            "outro": four_bar_chords((0, "maj9"), (6, "min7"), (2, "maj"), (0, "maj9")),
        },
        phrases={
            "verse": (N(.0,1.5,4,.70), N(2.5,1.0,6,.66), N(4.0,2.0,7,.76), N(7.0,1.0,11,.68), N(8.5,1.5,9,.76), N(11.0,1.0,6,.66), N(12.0,3.0,4,.78)),
            "pre": (N(.5,1.0,6,.68), N(2.0,1.5,9,.74), N(4.0,2.0,11,.80), N(6.5,1.0,13,.76), N(8.0,2.0,14,.84), N(11.0,1.0,16,.82), N(12.0,3.5,18,.88)),
            "chorus": (N(.0,1.5,4,.90,"accent"), N(2.0,2.0,11,.96,"scoop"), N(4.5,1.0,9,.84), N(6.0,2.0,16,.98,"accent"), N(8.0,1.5,14,.90), N(10.5,1.0,11,.82), N(12.0,1.0,9,.84), N(13.5,2.5,4,.94)),
            "verse2": (N(.0,1.0,4,.74), N(1.5,1.5,6,.72), N(4.0,2.5,11,.82), N(7.0,1.0,9,.72), N(8.0,1.5,13,.82), N(10.0,1.0,11,.74), N(12.0,3.5,6,.82)),
            "bridge": (N(.0,3.0,9,.76), N(4.0,2.0,6,.70), N(7.0,1.0,2,.66), N(8.0,3.0,4,.78), N(12.0,3.5,7,.82)),
            "final": (N(.0,1.5,11,.94,"accent"), N(2.0,2.0,18,1.0,"scoop"), N(4.5,1.0,16,.90), N(6.0,2.0,23,1.0,"accent"), N(8.0,1.5,21,.94), N(10.5,1.0,18,.88), N(12.0,1.0,16,.88), N(13.5,2.5,11,.98)),
            "outro": (N(.0,2.5,4,.60), N(4.0,2.5,11,.56), N(8.0,3.0,9,.50), N(12.0,3.5,4,.46)),
        },
    ),
    Track(
        name="MAGENTA CIRCUIT", slug="magenta-circuit", bpm=138.0,
        tonic_midi=49, lead_root_midi=61, mode="C-sharp Dorian",
        identity="Clipped offbeat pulse hook, FM bass and phase-distortion comp.",
        lead_voice="clipped_pulse", comp_voice="phase_comp",
        bass_voice="fm_bass", drum_voice="magenta_electro",
        arp_voice=None, swing=0.04, seed=0x4D430138,
        sections=(
            S("COLD OPEN", 2, "intro", "verse", .48, .32, .44, .60, .54, variant="pickup"),
            S("VERSE 1", 8, "verse", "verse", .69, .76, .86, .72, .76),
            S("PRE-CHORUS", 2, "pre", "pre", .84, .80, .90, .90, .82, variant="rise"),
            S("CHORUS", 4, "chorus", "chorus", 1.0, 1.0, 1.0, 1.0, 1.0),
            S("POST-CHORUS", 2, "post", "chorus", .86, .88, .92, .82, .68, variant="answer"),
            S("VERSE 2", 6, "verse2", "verse2", .76, .82, .92, .78, .84, variant="develop"),
            S("BREAK", 2, "break", "bridge", .44, .12, .00, .62, .72, variant="subtract"),
            S("BRIDGE", 4, "bridge", "bridge", .72, .54, .70, .90, .86, variant="open"),
            S("FINAL CHORUS", 4, "final", "final", 1.08, 1.0, 1.0, 1.0, 1.08, variant="lift"),
            S("OUTRO", 2, "outro", "outro", .40, .28, .36, .56, .42, variant="fade"),
        ),
        harmony={
            "intro": four_bar_chords((0,"min9"),(5,"dom7"),(10,"maj7"),(7,"min7")),
            "verse": (H(0,2,0,"min7"),H(2,2,3,"maj"),H(4,4,5,"dom7"),H(8,2,10,"maj7"),H(10,2,7,"min7"),H(12,4,0,"min9")),
            "pre": four_bar_chords((3,"maj7"),(5,"dom7"),(7,"min7"),(10,"sus2")),
            "chorus": four_bar_chords((0,"min7"),(10,"maj7"),(5,"dom7"),(3,"maj7")),
            "post": four_bar_chords((7,"min7"),(5,"dom7"),(3,"maj7"),(0,"min7")),
            "verse2": four_bar_chords((0,"min9"),(3,"maj7"),(10,"maj7"),(5,"dom7")),
            "break": four_bar_chords((8,"maj7"),(10,"maj7"),(0,"min7"),(0,"min7")),
            "bridge": (H(0,4,8,"maj7"),H(4,2,3,"maj7"),H(6,2,5,"dom7"),H(8,4,10,"maj7"),H(12,4,7,"min7")),
            "final": four_bar_chords((0,"min9"),(10,"maj7"),(5,"dom7"),(8,"maj7")),
            "outro": four_bar_chords((3,"maj7"),(5,"dom7"),(0,"min9"),(0,"min9")),
        },
        phrases={
            "verse": (N(.5,.75,3,.70,"staccato"),N(1.75,.5,5,.66,"staccato"),N(3.0,1.0,7,.74),N(4.5,.75,10,.76,"accent"),N(6.0,1.5,8,.70),N(8.25,.75,7,.76),N(9.5,.5,3,.68),N(10.5,1.5,5,.76),N(13.0,2.5,0,.80)),
            "pre": (N(.5,.5,5,.72),N(1.5,.5,7,.74),N(2.5,1.0,8,.78),N(4.5,1.0,10,.80),N(6.0,1.5,12,.84),N(8.5,.5,15,.80),N(9.5,.5,17,.82),N(10.5,1.0,19,.86),N(12.0,3.5,17,.88)),
            "chorus": (N(.5,.5,12,.94,"accent"),N(1.5,.5,12,.90),N(2.5,1.5,10,.96),N(4.5,.5,7,.88),N(5.5,.5,8,.90),N(6.5,1.5,5,.94),N(8.25,.75,3,.88),N(9.5,.5,5,.90),N(10.5,2.0,7,.98),N(13.0,2.5,0,.94)),
            "verse2": (N(.25,.75,3,.74),N(1.5,.75,7,.72),N(3.0,1.0,5,.76),N(4.25,1.25,10,.82),N(6.0,1.5,8,.74),N(8.5,.5,12,.82),N(9.5,1.0,10,.78),N(11.0,1.0,7,.76),N(13.0,2.5,3,.84)),
            "bridge": (N(.0,2.0,8,.78),N(3.0,1.0,7,.72),N(4.0,3.0,3,.80),N(8.0,1.5,5,.78),N(10.0,1.0,8,.76),N(12.0,3.5,10,.86)),
            "final": (N(.5,.5,15,.96,"accent"),N(1.5,.5,15,.94),N(2.5,1.5,12,1.0),N(4.5,.5,10,.92),N(5.5,.5,12,.94),N(6.5,1.5,8,.98),N(8.25,.75,7,.92),N(9.5,.5,10,.94),N(10.5,2.0,12,1.0),N(13.0,2.5,3,.98)),
            "outro": (N(.5,1.0,7,.58),N(2.0,1.5,5,.54),N(4.5,2.0,3,.50),N(8.5,2.0,0,.46),N(12.0,3.0,-2,.40)),
        },
    ),
    Track(
        name="GLASS HORIZON", slug="glass-horizon", bpm=116.0,
        tonic_midi=44, lead_root_midi=68, mode="A-flat major, 12/8 feel",
        identity="Breath flute over electric piano, fretless bass and sparse FM-bell answers.",
        lead_voice="breath_flute", comp_voice="electric_piano",
        bass_voice="fretless_bass", drum_voice="twelve_eight",
        arp_voice="fm_bell", swing=1.0 / 3.0, seed=0x47480116,
        sections=(
            S("INTRO",4,"intro","verse",.38,.16,.00,.72,.42,.16,"open"),
            S("VERSE 1",8,"verse","verse",.60,.50,.72,.78,.82,.10),
            S("PRE-CHORUS",4,"pre","pre",.72,.58,.78,.86,.86,.08,"rise"),
            S("CHORUS",4,"chorus","chorus",.90,.72,.88,.96,1.0,.12),
            S("VERSE 2",4,"verse2","verse2",.64,.54,.78,.80,.88,.08,"develop"),
            S("SAX-FLUTE SOLO",4,"solo","bridge",.76,.48,.68,.76,1.0,.18,"solo"),
            S("FINAL CHORUS",4,"final","final",.96,.78,.94,1.0,1.06,.16,"lift"),
            S("OUTRO",4,"outro","outro",.30,.08,.00,.62,.48,.14,"fade"),
        ),
        harmony={
            "intro": four_bar_chords((0,"maj9"),(4,"min7"),(5,"maj9"),(7,"sus4")),
            "verse": four_bar_chords((0,"maj7"),(9,"min7"),(5,"maj9"),(7,"dom7")),
            "pre": (H(0,6,2,"min7"),H(6,2,4,"min7"),H(8,4,5,"maj7"),H(12,4,7,"sus4")),
            "chorus": four_bar_chords((5,"maj9"),(7,"maj"),(4,"min7"),(9,"min7")),
            "verse2": four_bar_chords((0,"maj9"),(4,"min7"),(2,"min7"),(7,"dom7")),
            "solo": four_bar_chords((9,"min9"),(5,"maj7"),(2,"min7"),(7,"dom7")),
            "final": four_bar_chords((5,"maj9"),(7,"maj"),(0,"maj9"),(9,"min7")),
            "outro": four_bar_chords((5,"maj9"),(2,"min7"),(0,"maj9"),(0,"maj9")),
        },
        phrases={
            "verse": (N(.0,2.5,4,.66),N(3.0,1.0,7,.62),N(4.0,3.0,9,.72),N(8.0,2.0,7,.70),N(10.666,1.0,5,.62),N(12.0,3.5,4,.74)),
            "pre": (N(.0,2.0,5,.68),N(2.666,1.333,7,.70),N(4.0,2.5,9,.74),N(7.0,1.0,12,.72),N(8.0,3.0,14,.80),N(12.0,3.5,16,.84)),
            "chorus": (N(.0,3.0,16,.92,"scoop"),N(4.0,2.0,7,.86),N(6.666,1.0,9,.78),N(8.0,3.0,14,.90),N(12.0,1.5,9,.82),N(14.0,2.0,4,.90)),
            "verse2": (N(.0,2.0,4,.70),N(2.666,1.0,9,.68),N(4.0,3.0,12,.76),N(8.0,2.0,10,.74),N(10.666,1.0,7,.68),N(12.0,3.5,5,.78)),
            "bridge": (N(.0,1.5,9,.80),N(2.0,1.0,12,.76),N(4.0,2.5,17,.88),N(7.0,1.0,16,.80),N(8.0,3.0,12,.84),N(12.0,3.5,7,.82)),
            "final": (N(.0,3.0,19,.96,"scoop"),N(4.0,2.0,10,.90),N(6.666,1.0,12,.84),N(8.0,3.0,16,.94),N(12.0,1.5,12,.88),N(14.0,2.0,7,.94)),
            "outro": (N(.0,3.0,9,.54),N(4.0,3.0,7,.50),N(8.0,3.0,5,.46),N(12.0,3.5,0,.42)),
        },
    ),
    Track(
        name="STATIC HEART", slug="static-heart", bpm=132.0,
        tonic_midi=42, lead_root_midi=54, mode="F-sharp Dorian",
        identity="Motorik pulse ostinato and ring-mod answers; deliberately no hero lead.",
        lead_voice="ring_response", comp_voice="motorik_pulse",
        bass_voice="picked_bass", drum_voice="motorik",
        arp_voice=None, swing=0.0, seed=0x53480132,
        sections=(
            S("COUNT-IN",2,"intro","verse",.50,.60,.42,.72,.20,variant="pulse"),
            S("VERSE 1",6,"verse","verse",.70,.86,.90,.82,.42),
            S("VOLTAGE RISE",2,"pre","pre",.82,.92,.96,.94,.54,variant="rise"),
            S("REFRAIN",6,"chorus","chorus",.98,1.0,1.0,1.0,.68),
            S("POWER CUT",2,"break","bridge",.32,.00,.00,.62,.34,variant="subtract"),
            S("VERSE 2",6,"verse2","verse2",.76,.90,.96,.88,.50,variant="develop"),
            S("RING-MOD BRIDGE",2,"bridge","bridge",.62,.34,.54,.38,.82,variant="response"),
            S("FINAL REFRAIN",6,"final","final",1.06,1.0,1.0,1.0,.78,variant="lift"),
            S("TAPE STOP",4,"outro","outro",.38,.36,.46,.64,.28,variant="fade"),
        ),
        harmony={
            "intro": four_bar_chords((0,"min7"),(0,"min7"),(5,"dom7"),(3,"maj7")),
            "verse": (H(0,8,0,"min7"),H(8,4,5,"dom7"),H(12,4,10,"maj7")),
            "pre": four_bar_chords((3,"maj7"),(5,"dom7"),(7,"min7"),(10,"maj7")),
            "chorus": four_bar_chords((0,"min7"),(5,"dom7"),(10,"maj7"),(7,"min7")),
            "break": four_bar_chords((0,"sus2"),(1,"maj7"),(0,"sus2"),(0,"sus2")),
            "verse2": four_bar_chords((0,"min9"),(3,"maj7"),(5,"dom7"),(10,"maj7")),
            "bridge": four_bar_chords((1,"maj7"),(3,"maj7"),(8,"min7"),(5,"dom7")),
            "final": four_bar_chords((0,"min9"),(10,"maj7"),(5,"dom7"),(3,"maj7")),
            "outro": four_bar_chords((0,"min7"),(5,"dom7"),(0,"min7"),(0,"min7")),
        },
        phrases={
            "verse": (N(.0,.5,0,.62,"staccato"),N(1.0,.5,0,.58),N(2.0,.5,3,.64),N(3.0,.5,0,.58),N(4.0,.5,5,.66),N(5.0,.5,5,.60),N(6.0,1.0,3,.66),N(8.0,.5,0,.62),N(9.0,.5,1,.66),N(10.0,.5,0,.60),N(11.0,.5,7,.70),N(12.0,1.0,5,.68),N(14.0,1.5,0,.72)),
            "pre": (N(.0,.5,3,.66),N(1.0,.5,5,.68),N(2.0,.5,7,.70),N(3.0,.5,8,.72),N(4.0,1.0,10,.76),N(6.0,.5,8,.72),N(8.0,1.0,12,.78),N(10.0,1.0,13,.80),N(12.0,3.0,15,.84)),
            "chorus": (N(.0,.75,12,.78,"accent"),N(1.5,.5,12,.72),N(3.0,1.0,7,.76),N(4.0,.75,10,.80),N(5.5,.5,8,.74),N(7.0,1.0,5,.78),N(8.0,.75,3,.76),N(9.5,.5,4,.80),N(11.0,1.0,3,.76),N(12.0,3.0,0,.84)),
            "verse2": (N(.0,.5,0,.64),N(1.0,.5,3,.62),N(2.0,.5,0,.60),N(3.0,.5,5,.66),N(4.0,.5,7,.70),N(5.0,.5,5,.64),N(6.0,1.0,3,.68),N(8.0,.5,1,.66),N(9.0,.5,0,.62),N(10.0,.5,8,.72),N(11.0,.5,7,.68),N(12.0,1.0,5,.70),N(14.0,1.5,3,.74)),
            "bridge": (N(.0,1.0,1,.72,"accent"),N(2.0,1.0,7,.78),N(4.0,2.0,13,.84),N(7.0,1.0,8,.76),N(8.0,2.0,4,.80),N(12.0,3.0,1,.84)),
            "final": (N(.0,.75,15,.82,"accent"),N(1.5,.5,15,.78),N(3.0,1.0,10,.82),N(4.0,.75,13,.86),N(5.5,.5,12,.80),N(7.0,1.0,8,.84),N(8.0,.75,7,.82),N(9.5,.5,8,.84),N(11.0,1.0,7,.82),N(12.0,3.0,3,.88)),
            "outro": (N(.0,.5,0,.50),N(2.0,.5,0,.46),N(4.0,1.0,3,.44),N(8.0,1.0,1,.40),N(12.0,2.5,0,.36)),
        },
    ),
    Track(
        name="AFTERIMAGE RUN", slug="afterimage-run", bpm=126.0,
        tonic_midi=50, lead_root_midi=62, mode="D harmonic minor / Phrygian",
        identity="Half-time swung toms, sliding sub, FM mallet and muted-brass answers.",
        lead_voice="fm_mallet", comp_voice="muted_brass",
        bass_voice="sub_slide", drum_voice="swung_toms",
        arp_voice=None, swing=0.16, seed=0x41520126,
        sections=(
            S("DUST INTRO",4,"intro","verse",.38,.20,.44,.58,.48,variant="distant"),
            S("VERSE 1",8,"verse","verse",.64,.64,.84,.70,.78),
            S("PRE-CHORUS",4,"pre","pre",.78,.74,.90,.84,.84,variant="rise"),
            S("CHORUS",4,"chorus","chorus",.96,.90,1.0,.96,1.0),
            S("VERSE 2",4,"verse2","verse2",.69,.70,.90,.74,.86,variant="develop"),
            S("MIRAGE SOLO",4,"bridge","bridge",.74,.48,.54,.52,1.0,variant="solo"),
            S("FINAL CHORUS",4,"final","final",1.04,1.0,1.0,1.0,1.06,variant="lift"),
            S("AFTERIMAGE",4,"outro","outro",.34,.18,.32,.54,.48,variant="fade"),
        ),
        harmony={
            "intro": four_bar_chords((0,"min_add9"),(1,"maj7"),(7,"dom_b9"),(0,"min")),
            "verse": four_bar_chords((0,"min"),(1,"maj7"),(5,"min7"),(7,"dom_b9")),
            "pre": (H(0,4,5,"min7"),H(4,4,8,"maj7"),H(8,2,1,"maj7"),H(10,2,6,"dim"),H(12,4,7,"dom_b9")),
            "chorus": four_bar_chords((8,"maj7"),(5,"min7"),(7,"dom_b9"),(0,"min_add9")),
            "verse2": four_bar_chords((0,"min_add9"),(3,"min7"),(1,"maj7"),(7,"dom7")),
            "bridge": four_bar_chords((6,"dim"),(0,"min"),(10,"aug"),(7,"dom_b9")),
            "final": four_bar_chords((8,"maj7"),(5,"min9"),(7,"dom_b9"),(0,"min_add9")),
            "outro": four_bar_chords((1,"maj7"),(7,"dom_b9"),(0,"min_add9"),(0,"min_add9")),
        },
        phrases={
            "verse": (N(.0,1.5,0,.66),N(2.0,1.0,1,.70,"slide"),N(4.0,2.5,7,.74),N(7.0,1.0,6,.68),N(8.0,1.5,8,.76),N(10.0,1.0,7,.70),N(12.0,3.0,3,.78)),
            "pre": (N(.0,1.5,3,.70),N(2.0,1.5,6,.74),N(4.0,2.0,7,.78),N(6.5,1.0,10,.76),N(8.0,2.0,11,.82),N(10.5,1.0,13,.80),N(12.0,3.5,16,.86)),
            "chorus": (N(.0,1.5,12,.90,"accent"),N(2.0,1.0,11,.84),N(4.0,2.5,7,.92),N(7.0,1.0,1,.84,"slide"),N(8.0,1.5,8,.90),N(10.0,1.0,7,.84),N(12.0,3.5,0,.94)),
            "verse2": (N(.0,1.0,0,.70),N(1.5,1.5,3,.72),N(4.0,2.0,8,.78),N(6.5,1.0,7,.72),N(8.0,1.5,11,.82),N(10.0,1.0,10,.76),N(12.0,3.0,6,.82)),
            "bridge": (N(.0,.75,6,.78),N(1.0,.75,7,.80),N(2.0,1.5,11,.84),N(4.0,2.5,18,.92),N(7.0,1.0,16,.84),N(8.0,1.5,13,.86),N(10.0,1.0,10,.80),N(12.0,3.0,7,.86)),
            "final": (N(.0,1.5,15,.94,"accent"),N(2.0,1.0,14,.88),N(4.0,2.5,10,.96),N(7.0,1.0,4,.88,"slide"),N(8.0,1.5,11,.94),N(10.0,1.0,10,.88),N(12.0,3.5,3,.98)),
            "outro": (N(.0,2.0,7,.54),N(4.0,2.5,1,.50),N(8.0,3.0,0,.46),N(12.0,3.5,-5,.40)),
        },
    ),
    Track(
        name="NEON AFTERBURN", slug="neon-afterburn", bpm=148.0,
        tonic_midi=43, lead_root_midi=67, mode="G Mixolydian",
        identity="3+3+2 hard-sync brass calls, distorted picked bass and a rock kit; no arp.",
        lead_voice="hard_sync_brass", comp_voice="rock_brass",
        bass_voice="distorted_pick", drum_voice="rock_332",
        arp_voice=None, swing=0.0, seed=0x4E410148,
        sections=(
            S("IGNITION",2,"intro","verse",.52,.58,.54,.66,.42,variant="count"),
            S("VERSE 1",6,"verse","verse",.72,.88,.94,.76,.80),
            S("LAUNCH RISE",2,"pre","pre",.86,.96,1.0,.90,.88,variant="rise"),
            S("CHORUS",6,"chorus","chorus",1.0,1.0,1.0,1.0,1.0),
            S("VERSE 2",4,"verse2","verse2",.78,.92,1.0,.80,.88,variant="develop"),
            S("DRUM BREAK",2,"break","bridge",.56,.92,.00,.00,.46,variant="subtract"),
            S("BURNER SOLO",4,"solo","bridge",.82,.78,.86,.62,1.0,variant="solo"),
            S("FINAL CHORUS",8,"final","final",1.08,1.0,1.0,1.0,1.08,variant="lift"),
            S("KILL SWITCH",2,"outro","outro",.40,.38,.42,.46,.38,variant="fade"),
        ),
        harmony={
            "intro": four_bar_chords((0,"sus2"),(10,"maj"),(5,"maj"),(0,"maj")),
            "verse": (H(0,6,0,"maj"),H(6,2,10,"maj"),H(8,4,5,"maj"),H(12,4,3,"maj")),
            "pre": four_bar_chords((2,"min7"),(5,"maj"),(10,"maj"),(10,"sus2")),
            "chorus": four_bar_chords((0,"maj"),(10,"maj"),(5,"maj"),(0,"sus4")),
            "verse2": four_bar_chords((0,"maj"),(3,"maj"),(10,"maj"),(5,"maj")),
            "break": four_bar_chords((0,"sus2"),(0,"sus2"),(10,"maj"),(10,"maj")),
            "solo": (H(0,2,7,"min7"),H(2,2,10,"maj"),H(4,4,5,"maj"),H(8,4,3,"maj"),H(12,4,10,"maj")),
            "final": four_bar_chords((0,"add9"),(10,"maj"),(5,"maj"),(3,"maj")),
            "outro": four_bar_chords((10,"maj"),(5,"maj"),(0,"sus2"),(0,"maj")),
        },
        phrases={
            "verse": (N(.0,.75,0,.76,"accent"),N(1.5,.75,7,.80),N(3.0,1.0,5,.76),N(4.0,.75,10,.82),N(5.5,.75,7,.78),N(7.0,1.0,5,.76),N(8.0,1.5,3,.80),N(10.0,1.0,5,.78),N(12.0,3.0,0,.84)),
            "pre": (N(.0,1.0,5,.80),N(1.5,1.0,7,.82),N(3.0,1.0,10,.84),N(4.0,1.5,12,.86),N(6.0,1.0,14,.84),N(8.0,2.0,15,.90),N(10.5,1.0,17,.88),N(12.0,3.0,19,.94)),
            "chorus": (N(.0,1.5,0,.92,"accent"),N(1.5,1.5,12,.98,"scoop"),N(3.0,1.0,10,.90),N(4.0,1.5,7,.94),N(5.5,1.5,19,1.0),N(7.0,1.0,17,.92),N(8.0,1.5,15,.94),N(9.5,1.5,12,.90),N(11.0,1.0,10,.88),N(12.0,3.5,7,.96)),
            "verse2": (N(.0,.75,7,.80),N(1.5,.75,12,.84),N(3.0,1.0,10,.80),N(4.0,.75,15,.86),N(5.5,.75,12,.82),N(7.0,1.0,10,.80),N(8.0,1.5,8,.84),N(10.0,1.0,7,.80),N(12.0,3.0,5,.88)),
            "bridge": (N(.0,1.0,7,.84,"accent"),N(1.5,1.0,10,.84),N(3.0,1.0,15,.90),N(4.0,2.0,22,.98),N(7.0,1.0,19,.90),N(8.0,2.0,17,.92),N(12.0,3.0,12,.94)),
            "final": (N(.0,1.5,12,.96,"accent"),N(1.5,1.5,24,1.0,"scoop"),N(3.0,1.0,22,.94),N(4.0,1.5,19,.98),N(5.5,1.5,31,1.0),N(7.0,1.0,29,.96),N(8.0,1.5,27,.98),N(9.5,1.5,24,.94),N(11.0,1.0,22,.92),N(12.0,3.5,19,1.0)),
            "outro": (N(.0,1.5,12,.56),N(3.0,1.0,10,.50),N(4.0,2.0,7,.48),N(8.0,2.0,5,.44),N(12.0,3.0,0,.38)),
        },
    ),
    Track(
        name="CHROME DEVOTION", slug="chrome-devotion", bpm=120.0,
        tonic_midi=49, lead_root_midi=61, mode="D-flat major with borrowed iv and bVII",
        identity="Chorus-guitar lead, DX keys, vowel pad and fingered bass.",
        lead_voice="chorus_guitar", comp_voice="dx_vowel",
        bass_voice="fingered_bass", drum_voice="chrome_pop",
        arp_voice=None, swing=0.03, seed=0x43440120,
        sections=(
            S("CINEMATIC INTRO",4,"intro","verse",.40,.18,.28,.82,.34,variant="open"),
            S("VERSE 1",8,"verse","verse",.64,.62,.82,.82,.76),
            S("PRE-CHORUS",2,"pre","pre",.76,.68,.86,.92,.82,variant="rise"),
            S("CHORUS",6,"chorus","chorus",.94,.84,.94,1.0,1.0),
            S("VERSE 2",4,"verse2","verse2",.70,.68,.88,.86,.84,variant="develop"),
            S("DEVOTION BRIDGE",4,"bridge","bridge",.56,.24,.54,.92,.78,variant="subtract"),
            S("FINAL CHORUS",4,"final","final",1.0,.92,1.0,1.0,1.06,variant="lift"),
            S("LONG GOODBYE",4,"outro","outro",.34,.16,.32,.74,.48,variant="fade"),
        ),
        harmony={
            "intro": four_bar_chords((0,"maj9"),(9,"min7"),(5,"maj7"),(7,"sus4")),
            "verse": four_bar_chords((0,"maj7"),(4,"min7"),(9,"min7"),(5,"maj9")),
            "pre": (H(0,4,2,"min7"),H(4,4,4,"min7"),H(8,2,5,"maj7"),H(10,2,6,"min7"),H(12,4,7,"sus4")),
            "chorus": four_bar_chords((0,"maj"),(5,"maj7"),(5,"min7"),(11,"maj")),
            "verse2": four_bar_chords((9,"min7"),(4,"min7"),(2,"min7"),(7,"dom7")),
            "bridge": four_bar_chords((5,"min9"),(11,"maj7"),(0,"maj9"),(4,"min7")),
            "final": four_bar_chords((0,"maj9"),(5,"maj7"),(5,"min9"),(11,"maj")),
            "outro": four_bar_chords((5,"min7"),(11,"maj7"),(0,"maj9"),(0,"maj9")),
        },
        phrases={
            "verse": (N(.0,2.0,4,.66),N(3.0,1.0,5,.62),N(4.0,3.0,7,.72),N(8.0,2.0,9,.74),N(11.0,1.0,7,.66),N(12.0,3.5,4,.76)),
            "pre": (N(.0,1.5,5,.68),N(2.0,1.5,7,.72),N(4.0,2.0,9,.76),N(6.5,1.0,12,.74),N(8.0,2.0,14,.82),N(11.0,1.0,16,.80),N(12.0,3.5,17,.86)),
            "chorus": (N(.0,2.0,12,.92,"accent"),N(3.0,1.0,9,.84),N(4.0,3.0,7,.90),N(8.0,1.5,5,.86),N(10.0,1.0,4,.82),N(12.0,3.5,0,.94)),
            "verse2": (N(.0,1.5,4,.70),N(2.0,1.0,9,.70),N(4.0,2.5,12,.78),N(7.0,1.0,9,.70),N(8.0,2.0,7,.76),N(11.0,1.0,5,.70),N(12.0,3.5,2,.80)),
            "bridge": (N(.0,3.0,5,.76),N(4.0,2.0,0,.70),N(7.0,1.0,-1,.68),N(8.0,3.0,4,.78),N(12.0,3.5,9,.84)),
            "final": (N(.0,2.0,16,.96,"accent"),N(3.0,1.0,12,.88),N(4.0,3.0,9,.94),N(8.0,1.5,7,.90),N(10.0,1.0,5,.86),N(12.0,3.5,4,.98)),
            "outro": (N(.0,3.0,9,.54),N(4.0,3.0,5,.50),N(8.0,3.0,4,.46),N(12.0,3.5,0,.42)),
        },
    ),
    Track(
        name="REDLINE PROPHECY", slug="redline-prophecy", bpm=144.0,
        tonic_midi=47, lead_root_midi=59, mode="B Aeolian / Phrygian; F Lydian bridge",
        identity="Angular ring-mod siren, wavetable bass and a broken metallic kit; no warm pad.",
        lead_voice="angular_siren", comp_voice="metal_comp",
        bass_voice="wavetable_bass", drum_voice="broken_metal",
        arp_voice=None, swing=0.0, seed=0x52500144,
        sections=(
            S("WARNING",2,"intro","verse",.52,.46,.58,.46,.50,variant="alarm"),
            S("VERSE 1",6,"verse","verse",.72,.82,.94,.66,.82),
            S("PRE-CHORUS",4,"pre","pre",.86,.92,1.0,.76,.90,variant="rise"),
            S("CHORUS",4,"chorus","chorus",1.0,1.0,1.0,.86,1.0),
            S("VERSE 2",4,"verse2","verse2",.78,.88,1.0,.70,.90,variant="develop"),
            S("VOID BREAK",2,"break","bridge",.34,.08,.00,.28,.66,variant="subtract"),
            S("F LYDIAN BRIDGE",4,"bridge","bridge",.74,.54,.72,.74,.96,variant="keyshift"),
            S("ACCELERATOR",2,"build","pre",.90,1.0,1.0,.82,.90,variant="riser"),
            S("FINAL PROPHECY",6,"final","final",1.09,1.0,1.0,.92,1.08,variant="lift"),
            S("HARD CUTOFF",2,"outro","outro",.38,.32,.44,.34,.42,variant="fade"),
        ),
        harmony={
            "intro": four_bar_chords((0,"min"),(1,"maj"),(8,"maj"),(6,"dom7")),
            "verse": (H(0,4,0,"min"),H(4,2,1,"maj"),H(6,2,8,"maj"),H(8,4,5,"min7"),H(12,4,6,"dom_b9")),
            "pre": four_bar_chords((1,"maj7"),(5,"min7"),(8,"maj"),(6,"dom_b9")),
            "chorus": four_bar_chords((0,"min"),(8,"maj"),(1,"maj"),(6,"dom_b9")),
            "verse2": four_bar_chords((0,"min_add9"),(5,"min7"),(1,"maj7"),(6,"dom7")),
            "break": four_bar_chords((0,"sus2"),(6,"dim"),(0,"sus2"),(0,"sus2")),
            "bridge": four_bar_chords((6,"maj7_sharp11"),(8,"maj"),(5,"min7"),(1,"maj7")),
            "build": (H(0,2,6,"maj7_sharp11"),H(2,2,8,"maj"),H(4,2,10,"min7"),H(6,2,1,"maj"),H(8,2,3,"min7"),H(10,2,5,"min7"),H(12,4,6,"dom_b9")),
            "final": four_bar_chords((0,"min_add9"),(1,"maj"),(8,"maj"),(6,"dom_b9")),
            "outro": four_bar_chords((1,"maj"),(6,"dom_b9"),(0,"min"),(0,"min")),
        },
        phrases={
            "verse": (N(.0,1.0,0,.76,"accent"),N(2.0,1.5,6,.82),N(4.0,1.0,1,.78),N(6.0,1.5,8,.86),N(8.0,1.0,5,.80),N(10.0,1.5,11,.88),N(12.0,3.0,6,.86)),
            "pre": (N(.0,1.0,1,.80),N(2.0,1.0,5,.82),N(4.0,1.5,6,.86),N(6.0,1.0,8,.84),N(8.0,1.5,11,.90),N(10.0,1.0,13,.88),N(12.0,3.0,17,.94)),
            "chorus": (N(.0,1.0,12,.96,"accent"),N(2.0,2.0,6,.92,"slide"),N(4.0,1.0,13,.98),N(6.0,2.0,20,1.0),N(8.0,1.0,17,.94),N(10.0,1.5,11,.90),N(12.0,3.5,6,.98)),
            "verse2": (N(.0,1.0,0,.80),N(1.5,1.0,8,.86),N(4.0,1.0,1,.82),N(5.5,1.5,11,.90),N(8.0,1.0,6,.84),N(9.5,1.0,13,.90),N(12.0,3.0,8,.90)),
            "bridge": (N(.0,2.0,6,.82),N(3.0,1.0,8,.80),N(4.0,2.0,13,.88),N(7.0,1.0,18,.90),N(8.0,2.0,20,.94),N(12.0,3.0,25,.98)),
            "final": (N(.0,1.0,15,.98,"accent"),N(2.0,2.0,9,.96,"slide"),N(4.0,1.0,16,1.0),N(6.0,2.0,23,1.0),N(8.0,1.0,20,.98),N(10.0,1.5,14,.94),N(12.0,3.5,9,1.0)),
            "outro": (N(.0,1.0,6,.58),N(2.0,1.5,1,.52),N(4.0,2.0,0,.48),N(8.0,2.0,-4,.42),N(12.0,3.0,-5,.36)),
        },
    ),
)


def midi_frequency(note: float) -> float:
    return 440.0 * 2.0 ** ((note - 69.0) / 12.0)


def clamp01(values: np.ndarray) -> np.ndarray:
    return np.clip(values, 0.0, 1.0)


def equal_power_pan(pan: float) -> tuple[float, float]:
    angle = (np.clip(pan, -1.0, 1.0) + 1.0) * math.pi * 0.25
    return math.cos(angle), math.sin(angle)


def envelope(sample_rate: int, gate_seconds: float, release_seconds: float,
             attack: float, decay: float, sustain: float) -> np.ndarray:
    gate = max(1, int(round(gate_seconds * sample_rate)))
    release = max(1, int(round(release_seconds * sample_rate)))
    total = gate + release
    env = np.empty(total, dtype=np.float64)
    attack_n = min(gate, max(1, int(round(attack * sample_rate))))
    decay_n = min(gate - attack_n, max(1, int(round(decay * sample_rate))))
    env[:attack_n] = np.linspace(0.0, 1.0, attack_n, endpoint=False)
    if decay_n:
        env[attack_n:attack_n + decay_n] = np.linspace(
            1.0, sustain, decay_n, endpoint=False)
    env[attack_n + decay_n:gate] = sustain
    env[gate:] = np.linspace(sustain, 0.0, release, endpoint=True)
    return env


def phase_for_frequency(frequency: float, t: np.ndarray,
                        vibrato_depth: float = 0.0,
                        vibrato_rate: float = 5.0,
                        pitch_scoop: float = 0.0) -> np.ndarray:
    modulation = np.ones_like(t)
    if vibrato_depth:
        delayed = clamp01((t - 0.18) * 4.0)
        modulation *= 2.0 ** ((vibrato_depth * delayed *
                              np.sin(2.0 * np.pi * vibrato_rate * t)) / 12.0)
    if pitch_scoop:
        modulation *= 2.0 ** ((pitch_scoop * np.exp(-t * 14.0)) / 12.0)
    return 2.0 * np.pi * np.cumsum(frequency * modulation) / max(1, len(t) / (t[-1] + (t[1] if len(t) > 1 else 1.0)))


def additive_wave(phase: np.ndarray, kind: str, frequency: float,
                  sample_rate: int, quick: bool, duty: float = 0.5) -> np.ndarray:
    harmonic_limit = max(1, int((sample_rate * 0.46) / max(frequency, 1.0)))
    harmonic_limit = min(harmonic_limit, 7 if quick else 22)
    result = np.zeros_like(phase)
    if kind == "saw":
        for harmonic in range(1, harmonic_limit + 1):
            result += ((-1.0) ** (harmonic + 1) / harmonic) * np.sin(harmonic * phase)
        return result * (2.0 / math.pi)
    if kind == "pulse":
        for harmonic in range(1, harmonic_limit + 1):
            coefficient = (2.0 * math.sin(math.pi * harmonic * duty) /
                           (math.pi * harmonic))
            result += coefficient * np.cos(harmonic * (phase - math.pi * duty))
        return result
    if kind == "triangle":
        for harmonic in range(1, harmonic_limit + 1, 2):
            result += ((-1.0) ** ((harmonic - 1) // 2) /
                       (harmonic * harmonic)) * np.sin(harmonic * phase)
        return result * (8.0 / (math.pi * math.pi))
    return np.sin(phase)


def filtered_noise(rng: np.random.Generator, count: int, sample_rate: int,
                   low: float | None = None,
                   high: float | None = None) -> np.ndarray:
    noise = rng.standard_normal(count)
    nyquist = sample_rate * 0.5
    if low and high:
        sos = butter(2, [low / nyquist, min(high / nyquist, .98)],
                     btype="band", output="sos")
        return sosfilt(sos, noise)
    if low:
        sos = butter(2, min(low / nyquist, .98), btype="high", output="sos")
        return sosfilt(sos, noise)
    if high:
        sos = butter(2, min(high / nyquist, .98), btype="low", output="sos")
        return sosfilt(sos, noise)
    return noise


def synth_voice(kind: str, frequency: float, gate_seconds: float,
                sample_rate: int, rng: np.random.Generator,
                articulation: str, quick: bool) -> np.ndarray:
    settings = {
        "vocal_formant": (.045,.16,.72,.55), "clipped_pulse": (.008,.07,.58,.14),
        "breath_flute": (.075,.16,.82,.70), "ring_response": (.012,.10,.62,.28),
        "fm_mallet": (.006,.18,.18,.65), "hard_sync_brass": (.018,.13,.68,.30),
        "chorus_guitar": (.012,.20,.58,.72), "angular_siren": (.010,.10,.70,.38),
        "poly_brass": (.085,.22,.74,.72), "phase_comp": (.014,.12,.62,.28),
        "electric_piano": (.008,.32,.38,.90), "motorik_pulse": (.006,.08,.54,.12),
        "muted_brass": (.025,.12,.56,.25), "rock_brass": (.020,.14,.66,.24),
        "dx_vowel": (.070,.24,.72,.90), "metal_comp": (.008,.10,.48,.22),
        "glass_arp": (.004,.12,.12,.75), "fm_bell": (.003,.20,.08,1.10),
        "rubber_bass": (.008,.10,.64,.16), "fm_bass": (.004,.09,.58,.14),
        "fretless_bass": (.030,.12,.76,.24), "picked_bass": (.004,.08,.56,.12),
        "sub_slide": (.018,.12,.72,.24), "distorted_pick": (.004,.07,.50,.14),
        "fingered_bass": (.012,.11,.68,.19), "wavetable_bass": (.005,.08,.55,.15),
    }
    attack, decay, sustain, release = settings.get(kind, (.015,.12,.65,.25))
    if articulation == "staccato":
        gate_seconds *= .58
        release *= .55
    elif articulation == "accent":
        attack *= .55
    elif articulation in {"slide", "scoop"}:
        attack *= .75
    env = envelope(sample_rate, gate_seconds, release, attack, decay, sustain)
    t = np.arange(len(env), dtype=np.float64) / sample_rate
    scoop = -2.5 if articulation == "scoop" else (-1.0 if articulation == "slide" else 0.0)
    vibrato = .18 if kind in {"vocal_formant","breath_flute","chorus_guitar"} else 0.0
    phase = phase_for_frequency(frequency, t, vibrato, 5.1, scoop)
    sine = np.sin(phase)
    if kind == "vocal_formant":
        voice = sine * .54 + np.sin(2*phase) * .17 + np.sin(3*phase) * .16 + np.sin(5*phase) * .08
        voice *= 1.0 + .10 * np.sin(2*np.pi*1.9*t)
    elif kind == "clipped_pulse":
        pulse = additive_wave(phase,"pulse",frequency,sample_rate,quick,.28)
        voice = np.tanh((pulse*.78 + sine*.22)*1.8)
    elif kind == "breath_flute":
        breath = filtered_noise(rng,len(t),sample_rate,1200,min(7000,sample_rate*.44))
        voice = sine*.77 + np.sin(2*phase)*.10 + breath*.075
    elif kind == "ring_response":
        carrier = additive_wave(phase,"pulse",frequency,sample_rate,quick,.42)
        voice = carrier*.54 + carrier*np.sin(phase*1.497+.2)*.46
    elif kind in {"fm_mallet","fm_bell","electric_piano","fm_bass"}:
        ratio = {"fm_mallet":2.73,"fm_bell":3.01,"electric_piano":2.0,"fm_bass":1.01}[kind]
        index = {"fm_mallet":3.8,"fm_bell":5.2,"electric_piano":2.1,"fm_bass":1.5}[kind]
        mod_env = np.exp(-t * ({"fm_mallet":3.8,"fm_bell":2.5,"electric_piano":2.0,"fm_bass":5.0}[kind]))
        voice = np.sin(phase + index*mod_env*np.sin(phase*ratio))
    elif kind == "hard_sync_brass":
        saw = additive_wave(phase,"saw",frequency,sample_rate,quick)
        synced = additive_wave((phase % (2*np.pi))*1.73,"saw",frequency*1.73,sample_rate,quick)
        voice = saw*.58 + synced*.34 + sine*.08
    elif kind == "chorus_guitar":
        phases = (phase, phase*1.003+0.23, phase*.997-0.17)
        voice = sum(additive_wave(p,"triangle",frequency,sample_rate,quick) for p in phases)/3
        pick = filtered_noise(rng,len(t),sample_rate,1800,min(8000,sample_rate*.45))*np.exp(-t*28)
        voice = voice*.90 + pick*.10
    elif kind == "angular_siren":
        saw = additive_wave(phase,"saw",frequency,sample_rate,quick)
        voice = saw*.52 + saw*np.sin(phase*math.sqrt(2.0))*.40 + sine*.08
    elif kind in {"poly_brass","muted_brass","rock_brass"}:
        saw = additive_wave(phase,"saw",frequency,sample_rate,quick)
        tri = additive_wave(phase*.999,"triangle",frequency,sample_rate,quick)
        brightness = {"poly_brass":.56,"muted_brass":.32,"rock_brass":.70}[kind]
        voice = saw*brightness + tri*(1.0-brightness)
    elif kind == "phase_comp":
        voice = np.sin(phase + 2.2*np.sin(phase)*(.65+.35*np.exp(-t*4)))
    elif kind in {"motorik_pulse","metal_comp"}:
        pulse = additive_wave(phase,"pulse",frequency,sample_rate,quick,.22 if kind=="motorik_pulse" else .37)
        voice = pulse if kind=="motorik_pulse" else pulse*np.sin(phase*1.25)
    elif kind == "dx_vowel":
        voice = sine*.42 + np.sin(2*phase+.15)*.18 + np.sin(3*phase)*.22 + np.sin(6*phase)*.10
    elif kind == "glass_arp":
        voice = np.sin(phase + 3.0*np.exp(-t*4.5)*np.sin(phase*2.01))*.82 + np.sin(phase*4)*.08
    elif kind in {"rubber_bass","fretless_bass","picked_bass","sub_slide","distorted_pick","fingered_bass","wavetable_bass"}:
        sub = np.sin(phase)
        tri = additive_wave(phase,"triangle",frequency,sample_rate,quick)
        saw = additive_wave(phase,"saw",frequency,sample_rate,quick)
        pulse = additive_wave(phase,"pulse",frequency,sample_rate,quick,.30)
        mixes = {
            "rubber_bass": sub*.55+tri*.30+pulse*.15,
            "fretless_bass": sub*.68+tri*.27+saw*.05,
            "picked_bass": sub*.42+pulse*.38+saw*.20,
            "sub_slide": sub*.82+tri*.18,
            "distorted_pick": np.tanh((sub*.35+saw*.65)*1.9),
            "fingered_bass": sub*.58+tri*.30+saw*.12,
            "wavetable_bass": sub*.34+pulse*.34+saw*.32,
        }
        voice = mixes[kind]
    else:
        voice = sine
    return (voice * env).astype(np.float32)


def add_mono(bus: np.ndarray, mono: np.ndarray, start: int, amplitude: float,
             pan: float) -> None:
    if start >= len(bus) or start + len(mono) <= 0:
        return
    source_start = max(0, -start)
    dest_start = max(0, start)
    count = min(len(mono)-source_start, len(bus)-dest_start)
    if count <= 0:
        return
    left, right = equal_power_pan(pan)
    bus[dest_start:dest_start+count,0] += mono[source_start:source_start+count] * amplitude * left
    bus[dest_start:dest_start+count,1] += mono[source_start:source_start+count] * amplitude * right


CHORD_INTERVALS = {
    "maj": (0,4,7), "min": (0,3,7), "maj7": (0,4,7,11),
    "min7": (0,3,7,10), "dom7": (0,4,7,10), "sus2": (0,2,7),
    "sus4": (0,5,7), "add9": (0,4,7,14), "maj9": (0,4,7,11,14),
    "min9": (0,3,7,10,14), "min_add9": (0,3,7,14),
    "dim": (0,3,6), "aug": (0,4,8), "dom_b9": (0,4,7,10,13),
    "maj7_sharp11": (0,4,7,11,18),
}


def chord_voicing(root_midi: int, quality: str,
                  previous: Sequence[int] | None) -> tuple[int, ...]:
    intervals = CHORD_INTERVALS[quality]
    base = [root_midi + interval for interval in intervals[:4]]
    if len(base) == 3:
        base.append(root_midi + 12)
    candidates: list[tuple[float, tuple[int, ...]]] = []
    for inversion in range(4):
        notes = base[:]
        for _ in range(inversion):
            notes.append(notes.pop(0)+12)
        for shift in (-24,-12,0,12,24):
            voiced = tuple(sorted(n+shift for n in notes))
            if voiced[0] < 43 or voiced[-1] > 82 or voiced[-1]-voiced[0] > 22:
                continue
            center_cost = abs(sum(voiced)/len(voiced)-61.0)*.35
            if previous:
                movement = sum(abs(a-b) for a,b in zip(voiced,previous))
                common = len(set(voiced)&set(previous))
                score = movement + center_cost - common*3.5
            else:
                score = center_cost
            candidates.append((score,voiced))
    if not candidates:
        return tuple(base[:4])
    return min(candidates,key=lambda item:item[0])[1]


def swing_beat(beat: float, swing: float) -> float:
    if swing <= 0.0:
        return beat
    eighth = beat*2.0
    index = int(math.floor(eighth+1e-7))
    if index & 1:
        return beat + swing*.25
    return beat


def drum_sample(kind: str, sample_rate: int, rng: np.random.Generator,
                quick: bool) -> np.ndarray:
    durations = {"kick":.42,"snare":.34,"hat":.12,"open_hat":.34,
                 "tom_low":.42,"tom_high":.32,"metal":.44,"clap":.28}
    length = max(8,int(durations[kind]*sample_rate))
    t = np.arange(length)/sample_rate
    if kind == "kick":
        phase = 2*np.pi*np.cumsum(48+105*np.exp(-t*28))/sample_rate
        out = np.sin(phase)*np.exp(-t*11)+np.tanh(np.sin(phase)*3)*np.exp(-t*35)*.12
    elif kind in {"snare","clap"}:
        noise = filtered_noise(rng,length,sample_rate,700,min(9000,sample_rate*.46))
        tone = np.sin(2*np.pi*(185 if kind=="snare" else 260)*t)
        burst = (1+.45*np.sin(2*np.pi*33*t)) if kind=="clap" else 1
        out = (noise*.72+tone*.28)*np.exp(-t*(13 if kind=="snare" else 18))*burst
    elif kind in {"hat","open_hat"}:
        noise = filtered_noise(rng,length,sample_rate,4200,None)
        out = noise*np.exp(-t*(46 if kind=="hat" else 13))
    elif kind in {"tom_low","tom_high"}:
        f0 = 92 if kind=="tom_low" else 148
        phase = 2*np.pi*np.cumsum(f0+42*np.exp(-t*18))/sample_rate
        out = (np.sin(phase)+.18*np.sin(2*phase))*np.exp(-t*9)
    else:
        noise = filtered_noise(rng,length,sample_rate,1500,min(9500,sample_rate*.46))
        ring = np.sin(2*np.pi*713*t)*np.sin(2*np.pi*1093*t)
        out = (noise*.46+ring*.54)*np.exp(-t*8)
    # Even percussive transients need a finite attack.  Starting a noise burst
    # at an arbitrary nonzero sample produced very audible ADPCM clicks.  A
    # 1.5 ms sin-squared onset retains punch; the short terminal taper prevents
    # a one-sample discontinuity when a truncated tail reaches zero.
    attack_n=min(length,max(2,int(round(sample_rate*.0015))))
    release_n=min(length//3,max(2,int(round(sample_rate*.0040))))
    out[:attack_n]*=np.sin(np.linspace(0,math.pi*.5,attack_n))**2
    out[-release_n:]*=np.cos(np.linspace(0,math.pi*.5,release_n))**2
    peak=max(float(np.max(np.abs(out))),1e-9)
    return (out/peak).astype(np.float32)


def drum_events(style: str, bar: int, section: Section,
                final_bar: bool) -> list[tuple[float,str,float,float]]:
    if section.drums <= 0.01:
        return []
    events: list[tuple[float,str,float,float]]=[]
    if style == "night_drive":
        for b in (0,1,2,3): events.append((b,"kick",.76,0))
        for b in (1,3): events.append((b,"snare",.68,.08))
        for i in range(8): events.append((i*.5,"hat",.26,-.28 if i&1 else .28))
    elif style == "magenta_electro":
        for b in (0,1.5,2.75): events.append((b,"kick",.78,-.05))
        for b in (1,3): events.append((b,"clap",.66,.12))
        for i in range(8):
            if i not in (2,6): events.append((i*.5+.25,"hat",.25,.32 if i&1 else -.32))
    elif style == "twelve_eight":
        for b in (0,2.666): events.append((b,"kick",.62,-.06))
        events.append((2.0,"snare",.58,.06))
        for b in (0,2/3,4/3,2,8/3,10/3): events.append((b,"hat",.17,-.22 if int(b*3)&1 else .22))
    elif style == "motorik":
        for b in (0,1,2,3): events.append((b,"kick",.74,0))
        for b in (1,3): events.append((b,"snare",.62,.04))
        for i in range(16): events.append((i*.25,"hat",.18 if i&1 else .23,.26 if i&1 else -.26))
    elif style == "swung_toms":
        for b in (0,2.5): events.append((b,"kick",.70,-.08))
        events.append((2,"snare",.57,.08))
        for b,k in ((.0,"tom_low"),(1.5,"tom_high"),(3.25,"tom_low")): events.append((b,k,.48,-.35 if k=="tom_low" else .35))
        for i in range(8): events.append((i*.5+(.10 if i&1 else 0),"hat",.16,.3 if i&1 else -.3))
    elif style == "rock_332":
        for b in (0,1.5,3.0): events.append((b,"kick",.86,-.04))
        for b in (1,3): events.append((b,"snare",.76,.06))
        for i in range(8): events.append((i*.5,"hat",.30,.30 if i&1 else -.30))
    elif style == "chrome_pop":
        for b in (0,2,2.75): events.append((b,"kick",.68,-.05))
        for b in (1,3): events.append((b,"snare",.64,.08))
        for i in range(8): events.append((i*.5,"hat",.20,.22 if i&1 else -.22))
        if bar&1: events.append((3.5,"open_hat",.23,.38))
    else:
        for b in (0,.75,2.0,3.25): events.append((b,"kick",.74,-.08))
        for b in (1.25,3): events.append((b,"metal",.68,.16))
        for i in (0,2,3,6,7,10,12,15): events.append((i*.25,"hat",.20,.36 if i&1 else -.36))
    if final_bar:
        events.extend([(3.0,"tom_low",.52,-.35),(3.25,"tom_high",.55,.35),
                       (3.5,"snare",.52,-.12),(3.75,"snare",.64,.18)])
    return events


BASS_PATTERNS = {
    "rubber_bass": ((0,1.25,0),(1.5,.5,7),(2.0,1.0,12),(3.25,.5,7)),
    "fm_bass": ((0,.75,0),(1.25,.5,0),(2,.75,7),(3,.75,10)),
    "fretless_bass": ((0,1.75,0),(2.0,1.25,7),(3.5,.4,11)),
    "picked_bass": ((0,.65,0),(1,.65,0),(2,.65,7),(3,.65,10)),
    "sub_slide": ((0,2.0,0),(2.5,1.25,7)),
    "distorted_pick": ((0,.6,0),(1.5,.6,7),(3,.8,10)),
    "fingered_bass": ((0,1.0,0),(1.5,.75,7),(2.5,1.0,12)),
    "wavetable_bass": ((0,.75,0),(1,.5,1),(2,.75,7),(3.25,.5,6)),
}


def pattern_events(pattern: tuple[Harmony,...], total_beats: float) -> Iterable[Harmony]:
    cycle = max(event.beat+event.duration for event in pattern)
    cycle = max(cycle,PHRASE_BEATS)
    offset=0.0
    while offset < total_beats-1e-6:
        for event in pattern:
            start=offset+event.beat
            if start >= total_beats-1e-6:
                continue
            yield Harmony(start,min(event.duration,total_beats-start),event.root,event.quality)
        offset += cycle


def role_fade(section: Section, local_bar: float) -> float:
    if section.variant == "fade":
        return max(0.12,1.0-local_bar/max(section.bars,1)*.82)
    if section.variant in {"rise","riser"}:
        return .72+.28*local_bar/max(section.bars,1)
    return 1.0


def add_riser(bus: np.ndarray, end_frame: int, duration_seconds: float,
              sample_rate: int, rng: np.random.Generator, level: float) -> None:
    rise_count=max(1,int(duration_seconds*sample_rate))
    start=max(0,end_frame-rise_count)
    rise_count=end_frame-start
    release_count=max(2,int(round(sample_rate*.012)))
    count=rise_count+release_count
    if rise_count<=0:return
    t=np.arange(count)/sample_rate
    noise=filtered_noise(rng,count,sample_rate,500,min(9000,sample_rate*.46))
    env=np.empty(count,dtype=np.float64)
    env[:rise_count]=(np.arange(rise_count)/max(rise_count-1,1))**2
    env[rise_count:]=np.cos(np.linspace(0,math.pi*.5,release_count))**2
    tone=np.sin(2*np.pi*(220*t+260*t*t/max(t[-1],1e-6)))
    mono=(noise*.24+tone*.18)*env
    add_mono(bus,mono.astype(np.float32),start,level,-.25)
    # A reversed riser began at its loudest sample and clicked.  Use a
    # decorrelated, equally tapered side instead.
    side=(mono*(.82+.18*np.sin(2*np.pi*3.7*t+.4))).astype(np.float32)
    add_mono(bus,side,start,level*.45,.30)


def reversed_zero_ended(source: np.ndarray) -> np.ndarray:
    """Reverse an already tapered sound while retaining exact-zero endpoints."""
    reversed_source=source[::-1].copy()
    if len(reversed_source):
        reversed_source[0]=0.0
        reversed_source[-1]=0.0
    return reversed_source


def add_bespoke_transition(track: Track, incoming_name: str,
                           boundary_beat: float, end_frame: int,
                           seconds_per_beat: float, sample_rate: int,
                           drum_cache: dict[str,np.ndarray],
                           buses: dict[str,np.ndarray], quick: bool) -> list[dict]:
    """Prepare three deliberately sparse exits without flattening their drop."""
    events: list[dict]=[]
    transition_rng=np.random.default_rng(
        track.seed^int(round(boundary_beat*1024.0))^0x5452414E
    )

    if track.name=="STATIC HEART" and incoming_name=="VERSE 2":
        # POWER CUT is intentionally almost empty.  A reversed ring-mod answer
        # and snare breathe back into the motorik pulse during only its last
        # three quarters of a beat, preserving the cut while announcing the
        # full-band return.  Both sources begin and end on exact zero.
        ring=synth_voice("ring_response",midi_frequency(track.lead_root_midi+7),
                         seconds_per_beat*.16,sample_rate,transition_rng,"accent",quick)
        ring=reversed_zero_ended(ring)
        ring_start=end_frame-len(ring)
        add_mono(buses["lead"],ring,ring_start,.090,-.24)
        reverse_snare=reversed_zero_ended(drum_cache["snare"])
        snare_start=end_frame-len(reverse_snare)
        add_mono(buses["drums"],reverse_snare,snare_start,.140,.20)
        events.extend((
            {"role":"transition_fx","instrument":"reverse_ring_response",
             "section":incoming_name,
             "start_beat":boundary_beat-len(ring)/sample_rate/seconds_per_beat,
             "duration_beats":len(ring)/sample_rate/seconds_per_beat,
             "midi":track.lead_root_midi+7,"velocity":.090},
            {"role":"transition_fx","instrument":"reverse_snare",
             "section":incoming_name,
             "start_beat":boundary_beat-len(reverse_snare)/sample_rate/seconds_per_beat,
             "duration_beats":len(reverse_snare)/sample_rate/seconds_per_beat,
             "midi":None,"velocity":.140},
        ))

    elif track.name=="REDLINE PROPHECY" and incoming_name=="F LYDIAN BRIDGE":
        # The void now develops a one-beat angular siren/noise lift into the
        # surprise key change.  Its final 8ms taper reaches exact zero at the
        # bar line, where the bridge instruments take over cleanly.
        count=max(8,int(round(seconds_per_beat*sample_rate)))
        t=np.arange(count,dtype=np.float64)/sample_rate
        progress=np.linspace(0.0,1.0,count)
        start_frequency=midi_frequency(track.lead_root_midi-12)
        instant_frequency=start_frequency*2.0**((9.0*progress**1.45)/12.0)
        phase=2.0*np.pi*np.cumsum(instant_frequency)/sample_rate
        angular=np.sin(phase)*.58+np.sin(phase)*np.sin(phase*math.sqrt(2.0))*.34
        noise=filtered_noise(transition_rng,count,sample_rate,1100,
                             min(7600,sample_rate*.45))
        noise/=max(float(np.max(np.abs(noise))),1e-9)
        env=np.sin(progress*math.pi*.5)**2
        release_count=min(count//3,max(2,int(round(sample_rate*.008))))
        env[-release_count:]*=np.cos(np.linspace(0,math.pi*.5,release_count))**2
        env[0]=0.0
        env[-1]=0.0
        lift=((angular*.82+noise*.18)*env).astype(np.float32)
        lift[0]=0.0
        lift[-1]=0.0
        start=end_frame-count
        add_mono(buses["fx"],lift,start,.085,-.10)
        events.append(
            {"role":"transition_fx","instrument":"angular_siren_noise_lift",
             "section":incoming_name,"start_beat":boundary_beat-1.0,
             "duration_beats":1.0,"midi":track.lead_root_midi-12,
             "velocity":.085}
        )

    elif track.name=="MAGENTA CIRCUIT" and incoming_name=="BRIDGE":
        # A reversed clap supplies the inhale, while two short FM-bass pickup
        # notes answer the track's clipped offbeat hook.  Every source releases
        # before (or exactly on) the bridge downbeat.
        reverse_clap=reversed_zero_ended(drum_cache["clap"])
        clap_start=end_frame-len(reverse_clap)
        add_mono(buses["drums"],reverse_clap,clap_start,.160,.18)
        events.append(
            {"role":"transition_fx","instrument":"reverse_clap",
             "section":incoming_name,
             "start_beat":boundary_beat-len(reverse_clap)/sample_rate/seconds_per_beat,
             "duration_beats":len(reverse_clap)/sample_rate/seconds_per_beat,
             "midi":None,"velocity":.160}
        )
        for beat_offset,pitch_offset,pan in ((-.75,0,-.18),(-.25,7,.18)):
            pickup=synth_voice("fm_bass",
                               midi_frequency(track.tonic_midi-12+pitch_offset),
                               seconds_per_beat*.10,sample_rate,transition_rng,
                               "staccato",quick)
            pickup[0]=0.0
            pickup[-1]=0.0
            start=int(round(end_frame+beat_offset*seconds_per_beat*sample_rate))
            add_mono(buses["bass"],pickup,start,.105,pan)
            events.append(
                {"role":"transition_fx","instrument":"fm_bass_pickup",
                 "section":incoming_name,"start_beat":boundary_beat+beat_offset,
                 "duration_beats":len(pickup)/sample_rate/seconds_per_beat,
                 "midi":track.tonic_midi-12+pitch_offset,"velocity":.105}
            )

    return events


def apply_delay(source: np.ndarray, sample_rate: int, beat_seconds: float,
                taps: Sequence[tuple[float,float,float]]) -> np.ndarray:
    wet=np.zeros_like(source)
    for beat_delay,gain,cross in taps:
        delay=max(1,int(round(beat_delay*beat_seconds*sample_rate)))
        for repeat in range(1,5):
            shift=delay*repeat
            if shift>=len(source):break
            g=gain**repeat
            wet[shift:,0]+=source[:-shift,0]*g*(1-cross)+source[:-shift,1]*g*cross
            wet[shift:,1]+=source[:-shift,1]*g*(1-cross)+source[:-shift,0]*g*cross
    return wet


def apply_reverb(source: np.ndarray, sample_rate: int, amount: float,
                 quick: bool) -> np.ndarray:
    wet=np.zeros_like(source)
    taps=(.061,.089,.127,.181,.257,.383) if quick else \
         (.043,.061,.089,.127,.181,.257,.383,.541,.773,1.091,1.487,1.927)
    for index,seconds in enumerate(taps):
        delay=int(seconds*sample_rate)
        if delay>=len(source):continue
        gain=amount*math.exp(-seconds*1.35)*(0.82 if index&1 else 1.0)
        if index&1:
            wet[delay:,0]+=source[:-delay,1]*gain
            wet[delay:,1]+=source[:-delay,0]*gain
        else:
            wet[delay:]+=source[:-delay]*gain
    cutoff=min(7200.0,sample_rate*.42)
    sos=butter(2,cutoff/(sample_rate*.5),btype="low",output="sos")
    return sosfilt(sos,wet,axis=0).astype(np.float32)


def gentle_master(mix: np.ndarray, sample_rate: int) -> tuple[np.ndarray,float,float]:
    pre_peak=float(np.max(np.abs(mix)))
    mono=np.max(np.abs(mix),axis=1)
    coeff=math.exp(-1.0/(sample_rate*.035))
    power=lfilter([1.0-coeff],[1.0,-coeff],mono*mono)
    env=np.sqrt(np.maximum(power,1e-12))
    threshold=.34
    ratio=2.2
    target=np.ones_like(env)
    over=env>threshold
    compressed=threshold+(env[over]-threshold)/ratio
    target[over]=compressed/env[over]
    smooth_coeff=math.exp(-1.0/(sample_rate*.080))
    gain=lfilter([1.0-smooth_coeff],[1.0,-smooth_coeff],target)
    mastered=mix*gain[:,None]
    mastered=np.tanh(mastered*1.12)/math.tanh(1.12)
    peak=max(float(np.max(np.abs(mastered))),1e-9)
    if peak>.92:
        mastered*=.92/peak
    # Album-consistent post-compression trim uses the available 4-bit ADPCM
    # range without flattening the song-level macro dynamics.  The ceiling is
    # -1.2 dBFS; quieter masters approach -20 dBFS RMS until that headroom is
    # exhausted.
    peak=max(float(np.max(np.abs(mastered))),1e-9)
    rms=max(float(np.sqrt(np.mean(mastered.astype(np.float64)**2))),1e-9)
    ceiling=10.0**(-1.2/20.0)
    target_rms=10.0**(-20.0/20.0)
    trim=min(target_rms/rms,ceiling/peak,1.8)
    mastered*=trim
    return mastered.astype(np.float32),pre_peak,20.0*math.log10(trim)


def render_track(track: Track, output_dir: Path, sample_rate: int,
                 quick: bool) -> dict:
    seconds_per_beat=60.0/track.bpm
    music_frames=int(round(PLAYABLE_BARS*BEATS_PER_BAR*seconds_per_beat*sample_rate))
    tail_frames=int(math.ceil(TAIL_SECONDS*sample_rate))
    playable_frames=music_frames+tail_frames
    # Preserve the exact musical/tail boundary.  Alignment belongs entirely
    # to the silent guard so the runtime transition timestamp cannot drift.
    unaligned_stream_frames=playable_frames+MIN_GUARD_FRAMES
    guard_frames=MIN_GUARD_FRAMES+(-unaligned_stream_frames)%32
    stream_frames=playable_frames+guard_frames
    buses={name:np.zeros((playable_frames,2),dtype=np.float32)
           for name in ("drums","bass","comp","lead","arp","fx")}
    rng=np.random.default_rng(track.seed)
    drum_rng=np.random.default_rng(track.seed^0xD41A5EED)
    drum_cache={kind:drum_sample(kind,sample_rate,drum_rng,quick)
                for kind in ("kick","snare","hat","open_hat","tom_low","tom_high","metal","clap")}
    section_manifest=[]
    note_manifest=[]
    chord_manifest=[]
    cue_cells=[]
    previous_voicing:tuple[int,...]|None=None
    current_bar=0
    section_boundaries=[]

    for section_index,section in enumerate(track.sections):
        section_start_bar=current_bar
        section_start_beat=section_start_bar*BEATS_PER_BAR
        section_beats=section.bars*BEATS_PER_BAR
        section_manifest.append({"name":section.name,"start_bar":section_start_bar,
                                 "bars":section.bars,"variant":section.variant})
        if section_index:
            section_boundaries.append((section_start_beat,section.name))

        harmonies=list(pattern_events(track.harmony[section.harmony],section_beats))
        harmony_for_bar:dict[int,Harmony]={}
        for harmony in harmonies:
            global_beat=section_start_beat+harmony.beat
            root=track.tonic_midi+harmony.root
            voiced=chord_voicing(root,harmony.quality,previous_voicing)
            previous_voicing=voiced
            local_bar=harmony.beat/BEATS_PER_BAR
            fade=role_fade(section,local_bar)
            duration_seconds=harmony.duration*seconds_per_beat
            start_frame=int(round(global_beat*seconds_per_beat*sample_rate))
            for voice_index,midi in enumerate(voiced):
                mono=synth_voice(track.comp_voice,midi_frequency(midi),duration_seconds*.97,
                                 sample_rate,rng,"legato",quick)
                pan=(-.64,-.22,.24,.62)[voice_index]
                add_mono(buses["comp"],mono,start_frame,
                         .105*section.comp*section.energy*fade,pan)
            chord_manifest.append({"section":section.name,
                                   "start_bar":global_beat/BEATS_PER_BAR,
                                   "duration_beats":harmony.duration,
                                   "root_midi":root,"quality":harmony.quality,
                                   "voicing":list(voiced)})
            first_bar=int(math.floor(harmony.beat/4.0))
            last_bar=int(math.ceil((harmony.beat+harmony.duration)/4.0))
            for bar in range(first_bar,last_bar):
                harmony_for_bar[bar]=harmony

        if section.phrase:
            phrase=track.phrases[section.phrase]
            cycle=0.0
            cycle_index=0
            while cycle<section_beats-1e-6:
                for event_index,note in enumerate(phrase):
                    event_beat=note.beat
                    event_pitch=note.pitch
                    event_duration=note.duration
                    event_velocity=note.velocity
                    event_articulation=note.articulation
                    if cycle_index:
                        opening,midpoint,cadence,timing_index=ANSWER_RULES[track.slug]
                        if event_index==0:
                            event_pitch+=opening
                            event_velocity=min(1.0,event_velocity+.025)
                        if event_index==len(phrase)//2:
                            event_pitch+=midpoint
                        if event_index==len(phrase)-1:
                            event_pitch+=cadence
                            event_duration*=1.14
                            event_articulation="accent"
                        if event_index==timing_index%len(phrase):
                            event_beat+=(1.0/3.0 if track.name=="GLASS HORIZON" else .25)
                            event_duration*=.82
                        elif (event_index+cycle_index)%4==2:
                            event_duration*=1.08
                    local=cycle+event_beat
                    if local>=section_beats-1e-6:continue
                    duration=min(event_duration,section_beats-local)
                    global_beat=section_start_beat+local
                    start_frame=int(round(swing_beat(global_beat,track.swing)*seconds_per_beat*sample_rate))
                    midi=track.lead_root_midi+event_pitch
                    fade=role_fade(section,local/4.0)
                    mono=synth_voice(track.lead_voice,midi_frequency(midi),duration*seconds_per_beat,
                                     sample_rate,rng,event_articulation,quick)
                    pan=((event_index%5)-2)*.14
                    add_mono(buses["lead"],mono,start_frame,
                             .19*section.lead*section.energy*event_velocity*fade,pan)
                    note_manifest.append({"role":"lead","instrument":track.lead_voice,
                                          "section":section.name,"start_beat":global_beat,
                                          "duration_beats":duration,"midi":midi,
                                          "velocity":event_velocity,
                                          "articulation":event_articulation,
                                          "statement":cycle_index+1})
                    if section.variant=="lift" and event_index%2==0:
                        counter=synth_voice(track.lead_voice,midi_frequency(midi-12),
                                            duration*seconds_per_beat*.92,sample_rate,rng,
                                            "legato",quick)
                        add_mono(buses["lead"],counter,start_frame,
                                 .052*section.lead*fade,-pan)
                        note_manifest.append({"role":"counterlead",
                                              "instrument":track.lead_voice,
                                              "section":section.name,
                                              "start_beat":global_beat,
                                              "duration_beats":duration*.92,
                                              "midi":midi-12,
                                              "velocity":.052*section.lead*fade,
                                              "articulation":"legato",
                                              "statement":cycle_index+1})
                cycle+=PHRASE_BEATS
                cycle_index+=1

        for local_bar in range(section.bars):
            global_bar=section_start_bar+local_bar
            bar_beat=global_bar*4.0
            fade=role_fade(section,float(local_bar))
            final_bar=local_bar==section.bars-1 and section_index<len(track.sections)-1
            for beat,kind,velocity,pan in drum_events(track.drum_voice,global_bar,section,final_bar):
                start=int(round(swing_beat(bar_beat+beat,track.swing)*seconds_per_beat*sample_rate))
                add_mono(buses["drums"],drum_cache[kind],start,
                         .23*section.drums*section.energy*velocity*fade,pan)
                note_manifest.append({"role":"drum","instrument":kind,
                                      "section":section.name,"start_beat":bar_beat+beat,
                                      "duration_beats":0.0,"midi":None,
                                      "velocity":velocity})

            harmony=harmony_for_bar.get(local_bar)
            if harmony and section.bass>0.01:
                bass_root=track.tonic_midi-12+harmony.root
                for event_index,(beat,duration,offset) in enumerate(BASS_PATTERNS[track.bass_voice]):
                    start_beat=bar_beat+swing_beat(beat,track.swing)
                    start=int(round(start_beat*seconds_per_beat*sample_rate))
                    midi=bass_root+offset
                    mono=synth_voice(track.bass_voice,midi_frequency(midi),
                                     duration*seconds_per_beat,sample_rate,rng,
                                     "accent" if event_index==0 else "legato",quick)
                    add_mono(buses["bass"],mono,start,
                             .22*section.bass*section.energy*fade,
                             -.05 if event_index&1 else .05)
                    note_manifest.append({"role":"bass","instrument":track.bass_voice,
                                          "section":section.name,"start_beat":start_beat,
                                          "duration_beats":duration,"midi":midi,
                                          "velocity":section.bass*fade})

            if track.arp_voice and section.arp>0.01 and harmony:
                intervals=CHORD_INTERVALS[harmony.quality]
                # Midnight alone uses a truly prominent glass sequence.  Glass
                # Horizon's bell role is a sparse answer every other beat.
                steps=(0,.5,1.5,2.5,3.5) if track.arp_voice=="glass_arp" else (1.333,3.333)
                for arp_index,beat in enumerate(steps):
                    midi=track.tonic_midi+12+harmony.root+intervals[arp_index%len(intervals)]
                    start_beat=bar_beat+beat
                    start=int(round(start_beat*seconds_per_beat*sample_rate))
                    accent_duration=.34 if track.arp_voice=="glass_arp" else .56
                    mono=synth_voice(track.arp_voice,midi_frequency(midi),
                                     accent_duration*seconds_per_beat,
                                     sample_rate,rng,"staccato",quick)
                    add_mono(buses["arp"],mono,start,
                             .10*section.arp*section.energy*fade,
                             -.55 if arp_index&1 else .55)
                    note_manifest.append({"role":"accent","instrument":track.arp_voice,
                                          "section":section.name,"start_beat":start_beat,
                                          "duration_beats":accent_duration,"midi":midi,
                                          "velocity":section.arp*fade})
        current_bar+=section.bars

    if current_bar!=PLAYABLE_BARS:
        raise ValueError(f"{track.name}: {current_bar} bars, expected {PLAYABLE_BARS}")

    for boundary_beat,name in section_boundaries:
        end_frame=int(round(boundary_beat*seconds_per_beat*sample_rate))
        if "CHORUS" in name or "REFRAIN" in name or "RISE" in name or "ACCELERATOR" in name:
            add_riser(buses["fx"],end_frame,seconds_per_beat*2.0,
                      sample_rate,rng,.10)
        note_manifest.extend(
            add_bespoke_transition(track,name,boundary_beat,end_frame,
                                    seconds_per_beat,sample_rate,drum_cache,
                                    buses,quick)
        )

    # Effects process complete buses, so their tails cross every formal seam.
    lead_delay=apply_delay(buses["lead"],sample_rate,seconds_per_beat,
                           ((.75,.28,.18),(1.5,.19,.32)))
    if track.name in {"GLASS HORIZON","CHROME DEVOTION"}:
        lead_delay+=apply_delay(buses["lead"],sample_rate,seconds_per_beat,
                                ((2.0,.16,.44),))
    room_source=buses["comp"]*.48+buses["lead"]*.54+buses["arp"]*.42+buses["drums"]*.10
    reverb=apply_reverb(room_source,sample_rate,.17 if track.name!="GLASS HORIZON" else .24,quick)
    mix=(buses["drums"]+buses["bass"]+buses["comp"]+buses["lead"]+
         buses["arp"]+buses["fx"]+lead_delay+reverb)

    # Authored tail: stop new events at bar 36, preserve delay/reverb, and fade
    # those residuals before the completely silent stream guard begins.
    fade_start=max(0,music_frames-int(.12*sample_rate))
    fade_count=playable_frames-fade_start
    fade=np.cos(np.linspace(0,math.pi*.5,fade_count))**1.7
    mix[fade_start:]*=fade[:,None]
    if tail_frames>int(.18*sample_rate):
        mix[playable_frames-int(.16*sample_rate):]=0.0

    nyquist=sample_rate*.5
    sos=butter(2,max(20.0/nyquist,.001),btype="high",output="sos")
    mix=sosfilt(sos,mix,axis=0).astype(np.float32)
    mastered,pre_peak,master_trim_db=gentle_master(mix,sample_rate)
    stream=np.zeros((stream_frames,2),dtype=np.float32)
    stream[:playable_frames]=mastered
    dither_rng=np.random.default_rng(track.seed^0x51A7D17E)
    dither=(dither_rng.random((playable_frames,2))-
            dither_rng.random((playable_frames,2)))/65536.0
    stream[:playable_frames]=np.clip(stream[:playable_frames]+dither,-1.0,1.0)
    pcm=stream
    pcm16=np.round(pcm*32767.0).astype("<i2")
    filename=f"{TRACKS.index(track)+1:02d}-{track.slug}.wav"
    output_path=output_dir/filename
    wavfile.write(output_path,sample_rate,pcm16)
    payload=output_path.read_bytes()

    for cue in range(PLAYABLE_BARS//2):
        start_bar=cue*2
        section_name=next(section["name"] for section in reversed(section_manifest)
                          if section["start_bar"]<=start_bar)
        cue_cells.append({"index":cue,"start_bar":start_bar,"bars":2,
                          "start_frame":int(round(start_bar*4*seconds_per_beat*sample_rate)),
                          "end_frame":int(round((start_bar+2)*4*seconds_per_beat*sample_rate)),
                          "section":section_name})

    audible=mastered[:playable_frames]
    rms=float(np.sqrt(np.mean(audible.astype(np.float64)**2)))
    peak=float(np.max(np.abs(audible)))
    dc=[float(np.mean(audible[:,channel])) for channel in (0,1)]
    return {
        "name":track.name,"file":filename,"bpm":track.bpm,
        "sample_rate":sample_rate,"bars":PLAYABLE_BARS,"frames":stream_frames,
        "music_frames":music_frames,"playable_frames":playable_frames,
        "stream_frames":stream_frames,"tail_frames":tail_frames,
        # Planar stereo AICA ADPCM consumes one aggregate byte per stereo
        # sample frame (two four-bit channel samples).
        "playable_bytes":playable_frames,"stream_bytes":stream_frames,
        "adpcm_bytes_per_stereo_frame":1,
        "guard_frames":guard_frames,"seconds":stream_frames/sample_rate,
        "playable_seconds":playable_frames/sample_rate,
        "mode":track.mode,"identity":track.identity,"seed":track.seed,
        "quality":"quick" if quick else "production",
        "sections":section_manifest,"notes":note_manifest,"chords":chord_manifest,
        "cue_cells":cue_cells,
        "instruments":{"lead":track.lead_voice,"comp":track.comp_voice,
                       "bass":track.bass_voice,"drums":track.drum_voice,
                       "accent":track.arp_voice},
        "stats":{"sha256":hashlib.sha256(payload).hexdigest(),
                 "bytes":len(payload),"peak":peak,"pre_master_peak":pre_peak,
                 "master_trim_db":master_trim_db,
                 "rms":rms,"crest_db":20*math.log10(max(peak,1e-9)/max(rms,1e-9)),
                 "dc_offset":dc,"clipped_samples":int(np.count_nonzero(np.abs(audible)>=.999))},
    }


def validate_catalog(tracks: Sequence[Track]) -> None:
    expected=("MIDNIGHT VECTOR","MAGENTA CIRCUIT","GLASS HORIZON","STATIC HEART",
              "AFTERIMAGE RUN","NEON AFTERBURN","CHROME DEVOTION","REDLINE PROPHECY")
    actual=tuple(track.name for track in tracks)
    if actual!=expected:
        raise ValueError(f"album order mismatch: {actual!r}")
    chorus_signatures=set()
    prominent_arps=0
    for track in tracks:
        bars=sum(section.bars for section in track.sections)
        if bars!=PLAYABLE_BARS:
            raise ValueError(f"{track.name}: {bars} bars, expected 36")
        if any(section.bars%2 for section in track.sections):
            raise ValueError(f"{track.name}: every section must align to two-bar cue cells")
        if track.arp_voice=="glass_arp":
            prominent_arps+=1
        for required in ("verse","chorus","verse2","bridge","final","outro"):
            if required not in track.phrases:
                raise ValueError(f"{track.name}: missing {required} phrase")
            phrase=track.phrases[required]
            if not phrase or max(note.beat for note in phrase)<8.0:
                raise ValueError(f"{track.name}: {required} lacks antecedent/consequent span")
            if any(note.beat<0 or note.beat>=PHRASE_BEATS or note.duration<=0 for note in phrase):
                raise ValueError(f"{track.name}: invalid {required} event")
        chorus=track.phrases["chorus"]
        signature=tuple(note.pitch-chorus[0].pitch for note in chorus)
        if signature in chorus_signatures:
            raise ValueError(f"{track.name}: shared chorus pitch permutation")
        chorus_signatures.add(signature)
    if prominent_arps>2:
        raise ValueError("more than two tracks use prominent arpeggios")


def select_tracks(selector: str) -> tuple[Track,...]:
    if selector.lower()=="all":
        return TRACKS
    normalized=re.sub(r"[^a-z0-9]+","",selector.lower())
    if normalized.isdigit():
        index=int(normalized)-1
        if 0<=index<len(TRACKS):return (TRACKS[index],)
    matches=tuple(track for track in TRACKS
                  if normalized in {re.sub(r"[^a-z0-9]+","",track.name.lower()),
                                    re.sub(r"[^a-z0-9]+","",track.slug.lower())})
    if not matches:
        choices=", ".join(f"{i+1}:{t.slug}" for i,t in enumerate(TRACKS))
        raise ValueError(f"unknown track {selector!r}; choose all or {choices}")
    return matches


def parse_args() -> argparse.Namespace:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir",type=Path,required=True,
                        help="directory for WAV files and soundtrack_manifest.json")
    parser.add_argument("--sample-rate",type=int,default=DEFAULT_SAMPLE_RATE,
                        help=f"output sample rate (default: {DEFAULT_SAMPLE_RATE})")
    parser.add_argument("--track",default="all",
                        help="all, 1-8, exact title, or slug")
    parser.add_argument("--quick",action="store_true",
                        help="use fewer oscillator/reverb components for a deterministic smoke render")
    return parser.parse_args()


def main() -> int:
    args=parse_args()
    if args.sample_rate<6000 or args.sample_rate>48000:
        raise SystemExit("--sample-rate must be between 6000 and 48000")
    validate_catalog(TRACKS)
    try:
        selected=select_tracks(args.track)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    args.output_dir.mkdir(parents=True,exist_ok=True)
    rendered=[]
    for track in selected:
        print(f"Rendering {track.name} — {track.bpm:.0f} BPM, {track.mode} ...",flush=True)
        entry=render_track(track,args.output_dir,args.sample_rate,args.quick)
        rendered.append(entry)
        stats=entry["stats"]
        print(f"  {entry['file']}: {entry['playable_seconds']:.2f}s playable, "
              f"{entry['stream_frames']} frames, peak {stats['peak']:.3f}, "
              f"RMS {stats['rms']:.3f}, SHA256 {stats['sha256'][:16]}...",flush=True)
    manifest={
        "generator":GENERATOR_VERSION,"deterministic":True,
        "sample_rate":args.sample_rate,"playable_bars":PLAYABLE_BARS,
        "cue_cell_bars":2,"tail_seconds":TAIL_SECONDS,
        "minimum_guard_frames":MIN_GUARD_FRAMES,
        "quality":"quick" if args.quick else "production",
        "tracks":rendered,
    }
    manifest_path=args.output_dir/"soundtrack_manifest.json"
    manifest_path.write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    print(f"Wrote {manifest_path} ({len(rendered)} track(s)).")
    return 0


if __name__=="__main__":
    raise SystemExit(main())
