#!/usr/bin/env python3
"""Generate a synthetic test instrument, so the engine can be tried without real samples.

Writes one WAV per note (every few semitones) with an organ-like tone, a short
fade-in, a steady middle section marked as loop in the 'smpl' chunk, and a
fade-out tail. The default sample rate is 44.1 kHz on purpose: it exercises the
engine's rate conversion (the engine runs at 48 kHz).

Usage:
    python3 tools/make_test_instrument.py                    # -> media/test/tone/{note}.wav
    python3 tools/make_test_instrument.py --names            # -> media/test/tone/{name}.wav (C3.wav ...)
    python3 tools/make_test_instrument.py --out media/test/x --low 48 --high 72 --step 1

Only the Python standard library is used.
"""

import argparse
import math
import os
import struct
import sys

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(midi):
    """MIDI number -> name with C3 = 60."""
    return f"{NOTE_NAMES[midi % 12]}{midi // 12 - 2}"


def note_freq(midi):
    """MIDI 69 = A 440 Hz (the C3 = 60 naming does not change pitch)."""
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def build_tone(midi, rate, seconds):
    """Return (samples as int16 list, loop_start, loop_end)."""
    freq = note_freq(midi)
    n = int(seconds * rate)
    period = rate / freq

    # Loop: about the middle half of the file, a whole number of periods long,
    # so the loop point is nearly seamless.
    loop_start = int(0.25 * n)
    periods = max(1, int((0.5 * n) / period))
    loop_end = loop_start + int(round(periods * period)) - 1

    attack = int(0.01 * rate)
    release_start = int(0.85 * n)
    harmonics = [(1, 1.0), (2, 0.5), (3, 0.33), (4, 0.25), (6, 0.12)]
    harmonics = [(h, a) for h, a in harmonics if freq * h < rate / 2 * 0.9]
    norm = sum(a for _, a in harmonics)

    out = []
    for i in range(n):
        t = i / rate
        s = sum(a * math.sin(2 * math.pi * freq * h * t) for h, a in harmonics) / norm
        if i < attack:
            env = i / attack
        elif i >= release_start:
            env = max(0.0, 1.0 - (i - release_start) / (n - release_start))
        else:
            env = 1.0
        out.append(int(round(0.35 * 32767 * s * env)))  # about -11 dBFS: room for chords
    return out, loop_start, loop_end


def write_wav(path, samples, rate, root, loop_start, loop_end):
    data = struct.pack(f"<{len(samples)}h", *samples)
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    sample_period = int(round(1e9 / rate))
    smpl = struct.pack("<9I", 0, 0, sample_period, root, 0, 0, 0, 1, 0)
    smpl += struct.pack("<6I", 0, 0, loop_start, loop_end, 0, 0)

    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(data)) + data
    chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", default=os.path.join("media", "test", "tone"), help="output folder")
    p.add_argument("--low", type=int, default=36, help="lowest MIDI note (default 36 = C1)")
    p.add_argument("--high", type=int, default=96, help="highest MIDI note (default 96 = C6)")
    p.add_argument("--step", type=int, default=3, help="semitones between files (default 3)")
    p.add_argument("--rate", type=int, default=44100, help="sample rate (default 44100)")
    p.add_argument("--seconds", type=float, default=2.0, help="length of each file (default 2)")
    p.add_argument("--names", action="store_true", help="name files C3.wav ... instead of 60.wav ...")
    args = p.parse_args()

    if not (0 <= args.low <= args.high <= 127) or args.step < 1:
        sys.exit("invalid note range or step")

    os.makedirs(args.out, exist_ok=True)
    count = 0
    for midi in range(args.low, args.high + 1, args.step):
        samples, ls, le = build_tone(midi, args.rate, args.seconds)
        stem = note_name(midi) if args.names else str(midi)
        write_wav(os.path.join(args.out, f"{stem}.wav"), samples, args.rate, midi, ls, le)
        count += 1
    pattern = "{name}.wav" if args.names else "{note}.wav"
    print(f"wrote {count} files to {args.out} (pattern \"{pattern}\")")


if __name__ == "__main__":
    main()
