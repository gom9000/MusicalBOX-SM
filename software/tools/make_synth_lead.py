#!/usr/bin/env python3
"""Generate a monophonic-synth lead instrument: a band-limited sawtooth (or
pulse) through a low-pass filter with resonance and a broad "formant" peak,
one looped WAV per note. The engine plays it like any other instrument.

    python3 tools/make_synth_lead.py media/synth/lead
    python3 tools/make_synth_lead.py media/synth/lead-dark --cutoff 5000 --peak-db 6

Then in instruments.json:
    { "id": "SYNTH_LEAD", "name": "Synth lead", "path": "synth/lead", "files": "{note}.wav" }
and in the program a short attack and release (adsr), "loop": "on".

The default sound is a bright solo voice: harmonics close to a raw sawtooth,
a broad peak around 3.5 kHz, no chorus.

  --wave saw|pulse    oscillator (default saw)
  --duty F            pulse width, 0.05..0.5 (default 0.5 = square)
  --cutoff HZ         low-pass cutoff (default 8000)
  --resonance Q       low-pass resonance, 0.5..10 (default 2)
  --peak-hz HZ        centre of the formant peak (default 3500)
  --peak-db DB        height of the peak, 0 = none (default 9)
  --peak-width OCT    width of the peak in octaves (default 1.0)
  --low/--high/--step notes, MIDI numbers (default 36..96 every 3 semitones)

Only numpy is needed (sudo apt install python3-numpy).
"""

import argparse
import pathlib
import struct
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is needed: sudo apt install python3-numpy")

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(midi):
    """MIDI number -> name with C3 = 60."""
    return f"{NOTE_NAMES[midi % 12]}{midi // 12 - 2}"


def harmonic_amplitudes(freqs, a):
    """Amplitude of each harmonic (freqs = harmonic frequencies, index 0 = fundamental)."""
    h = np.arange(1, len(freqs) + 1)
    if a.wave == "saw":
        osc = 1.0 / h
    else:
        osc = np.abs(np.sin(np.pi * h * a.duty)) / h
    s = freqs / a.cutoff
    lowpass = 1.0 / np.sqrt((1 - s * s) ** 2 + (s / a.resonance) ** 2)
    peak = 10 ** (a.peak_db * np.exp(-0.5 * (np.log2(freqs / a.peak_hz) / a.peak_width) ** 2) / 20)
    return osc * lowpass * peak


def build_note(midi, a):
    """Samples (float, -1..1) and the loop (start, end inclusive)."""
    rate = a.rate
    freq = 440.0 * 2.0 ** ((midi - 69) / 12.0)
    # Loop of about one second holding a whole number of cycles: the oscillator
    # is tuned by a fraction of a cent so that the loop is seamless.
    cycles = max(1, round(freq * 1.0))
    loop_len = round(cycles * rate / freq)
    f = cycles * rate / loop_len
    start = int(0.01 * rate)
    n = start + loop_len
    count = int(min(0.45 * rate, 4 * a.cutoff) // f)
    freqs = f * np.arange(1, count + 1)
    amps = harmonic_amplitudes(freqs, a)
    t = np.arange(n) / rate
    x = np.zeros(n)
    for k, (fk, ak) in enumerate(zip(freqs, amps), start=1):
        sign = -1.0 if (a.wave == "saw" and k % 2 == 0) else 1.0  # sawtooth phases
        x += sign * ak * np.sin(2 * np.pi * fk * t)
    # Same loudness on every note, peaks below 0 dBFS.
    x *= 10 ** (a.level_db / 20) / np.sqrt(np.mean(x[start:] ** 2))
    peak = np.abs(x).max()
    if peak > 0.89:
        x *= 0.89 / peak
    fade = int(0.002 * rate)
    x[:fade] *= np.linspace(0.0, 1.0, fade)
    return x, start, start + loop_len - 1


def write_wav(path, x, rate, root, loop_start, loop_end):
    rng = np.random.default_rng(root)
    dither = (rng.random(len(x)) - rng.random(len(x))) / 32768  # TPDF
    data = np.clip(np.round((x + dither) * 32767), -32768, 32767).astype("<i2").tobytes()
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    smpl = struct.pack("<9I", 0, 0, int(round(1e9 / rate)), root, 0, 0, 0, 1, 0)
    smpl += struct.pack("<6I", 0, 0, loop_start, loop_end, 0, 0)
    body = b"WAVE"
    for cid, payload in ((b"fmt ", fmt), (b"data", data), (b"smpl", smpl)):
        body += cid + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", help="output folder")
    ap.add_argument("--wave", choices=["saw", "pulse"], default="saw")
    ap.add_argument("--duty", type=float, default=0.5)
    ap.add_argument("--cutoff", type=float, default=8000)
    ap.add_argument("--resonance", type=float, default=2.0)
    ap.add_argument("--peak-hz", type=float, default=3500)
    ap.add_argument("--peak-db", type=float, default=9)
    ap.add_argument("--peak-width", type=float, default=1.0)
    ap.add_argument("--level-db", type=float, default=-18, help="RMS level of each note (default -18 dBFS)")
    ap.add_argument("--low", type=int, default=36)
    ap.add_argument("--high", type=int, default=96)
    ap.add_argument("--step", type=int, default=3)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--names", action="store_true", help="name files C3.wav ... instead of 60.wav ...")
    a = ap.parse_args()
    if not (0 <= a.low <= a.high <= 127) or a.step < 1:
        sys.exit("invalid note range or step")
    if not (0.05 <= a.duty <= 0.5 and 0.5 <= a.resonance <= 10 and 200 <= a.cutoff <= 20000
            and a.peak_width > 0 and a.peak_hz > 0):
        sys.exit("a value is out of range (see --help)")

    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for midi in range(a.low, a.high + 1, a.step):
        x, ls, le = build_note(midi, a)
        stem = note_name(midi) if a.names else str(midi)
        write_wav(out / f"{stem}.wav", x, a.rate, midi, ls, le)
        count += 1
    pattern = "{name}.wav" if a.names else "{note}.wav"
    print(f"wrote {count} files to {out} (pattern \"{pattern}\")")


if __name__ == "__main__":
    main()
