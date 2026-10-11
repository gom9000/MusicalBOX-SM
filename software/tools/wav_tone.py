#!/usr/bin/env python3
"""Makes a darker (or brighter) copy of a folder of samples, once and for all,
so the engine does no extra work while playing.

    python3 tools/wav_tone.py media/tape/accordion media/tape/accordion-dark --high-cut 2500
    python3 tools/wav_tone.py IN OUT --treble-db -4 --treble-hz 3000 --low-cut 80

Then add an instrument with the new folder in instruments.json (same "files"
pattern). Filters are zero-phase (no smearing of the attack), loop points and
the other chunks of the WAV (smpl...) are kept as they are.

  --high-cut HZ     low-pass, 12 dB/octave (like the high_cut_hz of "tone")
  --low-cut HZ      high-pass, 12 dB/octave
  --treble-db DB    shelf above --treble-hz (default 3500 Hz)
  --bass-db DB      shelf below --bass-hz (default 120 Hz)
  --gain-db DB      overall level

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


def read_chunks(path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    chunks, pos = [], 12
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        chunks.append([cid, bytearray(data[pos + 8:pos + 8 + size])])
        pos += 8 + size + (size & 1)
    return chunks


def write_chunks(path, chunks):
    body = b"WAVE"
    for cid, payload in chunks:
        body += cid + struct.pack("<I", len(payload)) + bytes(payload) + (b"\0" if len(payload) & 1 else b"")
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def decode(fmt, raw):
    tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", bytes(fmt[:16]))
    if tag == 0xFFFE:  # extensible: the real format is in the sub-format
        tag = struct.unpack("<H", bytes(fmt[24:26]))[0]
    if tag == 3 and bits == 32:
        x = np.frombuffer(raw, "<f4").astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(raw, "<i2") / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(raw[: len(raw) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / 8388608.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(raw, "<i4") / 2147483648.0
    elif tag == 1 and bits == 8:
        x = (np.frombuffer(raw, np.uint8).astype(np.float64) - 128) / 128.0
    else:
        raise ValueError(f"format {tag}/{bits} bit not supported")
    return x[: len(x) // ch * ch].reshape(-1, ch), rate, tag, bits


def encode(x, tag, bits):
    flat = x.reshape(-1)
    if tag == 3:
        return flat.astype("<f4").tobytes()
    if bits == 16:
        return np.clip(np.round(flat * 32768), -32768, 32767).astype("<i2").tobytes()
    if bits == 24:
        v = np.clip(np.round(flat * 8388608), -8388608, 8388607).astype(np.int32)
        v = np.where(v < 0, v + (1 << 24), v).astype(np.uint32)
        return np.stack([v & 255, (v >> 8) & 255, (v >> 16) & 255], 1).astype(np.uint8).tobytes()
    if bits == 32:
        return np.clip(np.round(flat * 2147483648), -2147483648, 2147483647).astype("<i4").tobytes()
    return (np.clip(np.round(flat * 128 + 128), 0, 255)).astype(np.uint8).tobytes()


def response(freqs, a):
    """Magnitude of the requested tone, per frequency (zero phase)."""
    f = np.maximum(freqs, 1e-3)
    g = np.full_like(f, 10 ** (a.gain_db / 20))
    if a.high_cut:
        g /= np.sqrt(1 + (f / a.high_cut) ** 4)       # 2nd-order Butterworth magnitude
    if a.low_cut:
        g /= np.sqrt(1 + (a.low_cut / f) ** 4)
    if a.treble_db:
        k = 10 ** (a.treble_db / 20)
        g *= np.sqrt((1 + k * k * (f / a.treble_hz) ** 2) / (1 + (f / a.treble_hz) ** 2))  # 1st-order shelf
    if a.bass_db:
        k = 10 ** (a.bass_db / 20)
        g *= np.sqrt((k * k + (f / a.bass_hz) ** 2) / (1 + (f / a.bass_hz) ** 2))
    return g


def process(src, dst, a):
    chunks = read_chunks(src)
    fmt = next(c[1] for c in chunks if c[0] == b"fmt ")
    data = next(c for c in chunks if c[0] == b"data")
    x, rate, tag, bits = decode(fmt, bytes(data[1]))
    n = len(x)
    pad = int(0.05 * rate)  # silence around: the filter rings into it, not around the ends
    size = 1 << int(np.ceil(np.log2(n + 2 * pad)))
    spec = np.fft.rfft(np.pad(x, ((pad, size - n - pad), (0, 0))), axis=0)
    spec *= response(np.fft.rfftfreq(size, 1 / rate), a)[:, None]
    y = np.fft.irfft(spec, size, axis=0)[pad:pad + n]
    peak = np.abs(y).max() if n else 0
    note = ""
    if peak > 0.999:
        y *= 0.999 / peak
        note = f"  (lowered {20 * np.log10(peak / 0.999):.1f} dB: it would clip)"
    data[1] = bytearray(encode(y, tag, bits))
    write_chunks(dst, chunks)
    return note


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--high-cut", type=float, default=0)
    ap.add_argument("--low-cut", type=float, default=0)
    ap.add_argument("--treble-db", type=float, default=0)
    ap.add_argument("--treble-hz", type=float, default=3500)
    ap.add_argument("--bass-db", type=float, default=0)
    ap.add_argument("--bass-hz", type=float, default=120)
    ap.add_argument("--gain-db", type=float, default=0)
    a = ap.parse_args()
    src, dst = pathlib.Path(a.src), pathlib.Path(a.dst)
    if src.resolve() == dst.resolve():
        sys.exit("the destination must be another folder (the originals are never changed)")
    files = sorted(p for p in src.iterdir() if p.suffix.lower() == ".wav")
    if not files:
        sys.exit(f"no .wav files in {src}")
    dst.mkdir(parents=True, exist_ok=True)
    for p in files:
        try:
            note = process(p, dst / p.name, a)
            print(f"{p.name}{note}")
        except (ValueError, OSError, StopIteration) as e:
            print(f"{p.name}: skipped ({e})")
    print(f"{len(files)} files -> {dst}")


if __name__ == "__main__":
    main()
