#!/usr/bin/env python3
"""Measure the real pitch of WAV files and tell which note-name convention they use.

Sample libraries name their files with notes ("C3.wav", "Bb2.wav"), but there are
two conventions for the octave number:
  - C3 = 60 (Yamaha, SamplerBox, the default of MusicalBOX SM): middle C is "C3"
  - C4 = 60 (scientific pitch, most libraries and DAWs): middle C is "C4"
This tool measures the fundamental of each file and compares it with the name.

Usage:
    python3 tools/check_pitch.py media/tape/violins          # 8 files spread over the folder
    python3 tools/check_pitch.py media/tape/violins --all    # every file
    python3 tools/check_pitch.py media/tape/violins/C3.wav
    python3 tools/check_pitch.py media/epiano/harp           # also "C0_loop.wav" names

Only the Python standard library is used (a few seconds per folder).
"""

import math
import os
import re
import struct
import sys
import wave

NAMES = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}


def name_to_midi_c3(name):
    """'C#3' -> MIDI number with C3 = 60, or None."""
    # The note may be the whole name ("C3") or a part of it ("C0_loop", "Piano-F#2-v3").
    m = re.fullmatch(r"([A-Ga-g])([#b]?)(-?\d{1,2})", name) or \
        re.search(r"(?<![A-Za-z0-9])([A-Ga-g])([#b]?)(-?\d)(?![0-9])", name)
    if not m:
        return None
    n = NAMES[m.group(1).lower()] + (1 if m.group(2) == "#" else -1 if m.group(2) == "b" else 0)
    return n + (int(m.group(3)) + 2) * 12


def read_mono(path, seconds=0.12, skip=0.3):
    """A short mono segment of the file, as floats, plus the sample rate."""
    with wave.open(path, "rb") as w:
        rate, ch, width, frames = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        start = min(int(skip * rate), max(0, frames - int(seconds * rate)))
        w.setpos(start)
        raw = w.readframes(int(seconds * rate))
    n = len(raw) // (width * ch)
    out = []
    for i in range(n):
        off = i * width * ch  # first channel only
        b = raw[off:off + width]
        if width == 2:
            v = struct.unpack("<h", b)[0] / 32768.0
        elif width == 3:
            v = int.from_bytes(b, "little", signed=True) / 8388608.0
        elif width == 4:
            v = struct.unpack("<i", b)[0] / 2147483648.0
        else:
            v = (b[0] - 128) / 128.0
        out.append(v)
    return out, rate


FMIN, FMAX = 40.0, 1500.0


def midi_to_hz(midi):
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def fundamental(x, rate, fmin=FMIN, fmax=FMAX):
    """Fundamental frequency by normalised autocorrelation (good enough for tuning checks)."""
    n = len(x)
    mean = sum(x) / n
    x = [v - mean for v in x]
    lo, hi = int(rate / fmax), min(int(rate / fmin), n // 2)
    energy = sum(v * v for v in x) / n or 1e-12
    corr = {}
    for lag in range(lo - 1, hi + 2):
        m = n - lag
        # Average over the overlap only, so long lags are not penalised.
        corr[lag] = sum(x[i] * x[i + lag] for i in range(0, m, 3)) / ((m + 2) // 3) / energy
    # Only real peaks count (a smooth low note correlates highly at tiny lags too).
    peaks = [lag for lag in range(lo, hi + 1) if corr[lag] > 0 and corr[lag] >= corr[lag - 1] and corr[lag] >= corr[lag + 1]]
    if not peaks:
        return None
    best = max(corr[lag] for lag in peaks)
    # The shortest lag that is almost as good is the period (avoids octave-down errors).
    for lag in peaks:
        if corr[lag] > 0.9 * best:
            # Parabolic interpolation for a sub-sample period.
            a, b, c = corr[lag - 1], corr[lag], corr[lag + 1]
            den = a - 2 * b + c
            shift = 0.5 * (a - c) / den if den else 0.0
            return rate / (lag + shift)
    return None


def main():
    args = [a for a in sys.argv[1:] if a != "--all"]
    if len(args) != 1:
        sys.exit(__doc__)
    target = args[0]
    files = [target] if os.path.isfile(target) else sorted(
        os.path.join(target, f) for f in os.listdir(target) if f.lower().endswith(".wav"))
    if not files:
        sys.exit("no WAV files found")
    # Very low and very high notes are outside what the measurement can track
    # (about 40-1500 Hz): leave them out instead of reporting false mismatches.
    def measurable(path):
        midi = name_to_midi_c3(os.path.splitext(os.path.basename(path))[0])
        if midi is None:
            return True
        lo, hi = midi_to_hz(midi - 12), midi_to_hz(midi)   # the two conventions
        return lo >= FMIN * 1.1 and hi <= FMAX / 1.1
    skipped = [f for f in files if not measurable(f)]
    files = [f for f in files if measurable(f)]
    if skipped:
        print(f"({len(skipped)} files with very low or very high notes left out: their pitch cannot be measured reliably)")
    if not files:
        sys.exit("no file in the measurable range")
    if "--all" not in sys.argv and len(files) > 8:
        step = len(files) / 8
        files = [files[int(i * step)] for i in range(8)]

    votes = {"C3 = 60": 0, "C4 = 60": 0}
    print(f"{'file':14} {'measured Hz':>11}  {'note (C3=60)':>13}  {'if C3=60':>9}  {'if C4=60':>9}")
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        midi = name_to_midi_c3(stem)
        try:
            x, rate = read_mono(path)
            f = fundamental(x, rate)
        except (wave.Error, EOFError, OSError) as e:
            print(f"{stem:14} error: {e}")
            continue
        if not f:
            print(f"{stem:14} no clear pitch")
            continue
        measured = 69 + 12 * math.log2(f / 440.0)
        if midi is None:
            print(f"{stem:14} {f:11.1f}  {measured:13.2f}  (name is not a note)")
            continue
        d3 = measured - midi          # error if the name means C3 = 60
        d4 = measured - (midi - 12)   # error if the name means C4 = 60
        if abs(d3) < 0.5:
            votes["C3 = 60"] += 1
        elif abs(d4) < 0.5:
            votes["C4 = 60"] += 1
        print(f"{stem:14} {f:11.1f}  {measured:13.2f}  {d3:+8.2f}st  {d4:+8.2f}st")

    print()
    total = sum(votes.values())
    if total and min(votes.values()) == 0 and total < len(files) * 0.75:
        print(f"result: uncertain, only {total}/{len(files)} files match - the others are an octave off, "
              "usually a measuring error on sounds with several voices (choirs). Check by ear: play a "
              "chromatic scale and listen for octave jumps between neighbouring keys.")
    elif total == 0:
        print("result: the names match neither convention within half a semitone - check a file by ear")
    elif votes["C4 = 60"] > votes["C3 = 60"]:
        print(f"result: C4 = 60 ({votes['C4 = 60']}/{len(files)} files). "
              "These names are one octave higher than MusicalBOX SM reads them: add \"name_octave\": \"C4\" "
              "to this instrument in instruments.json.")
    else:
        print(f"result: C3 = 60 ({votes['C3 = 60']}/{len(files)} files). Nothing to change.")


if __name__ == "__main__":
    main()
