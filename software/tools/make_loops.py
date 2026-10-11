#!/usr/bin/env python3
"""Add sustain loops to WAV files that have none (for "loop": "on" / organ mode).

For each file the tool:
  1. finds the sustain part (after the attack, before the sound starts to die away);
  2. picks a loop as long as possible inside it, with start and end chosen
     where the waveform is most alike;
  3. smooths the jump with a crossfade just before the loop end, so the
     return to the loop start has no click, even on sounds with chorus or
     ensemble (strings, choirs) that never repeat exactly;
  4. writes a copy of the file with the loop in the 'smpl' chunk.

The originals are never changed: the copies go to another folder (default:
the same name followed by "-loop"). Point the instrument's "path" there.

Usage:
    python3 tools/make_loops.py media/strings/ensemble
    python3 tools/make_loops.py media/strings/ensemble --out media/strings/ensemble-loop
    python3 tools/make_loops.py media/strings/ensemble --xfade 0.4 --max-loop 3
    python3 tools/make_loops.py media/strings/ensemble --dry-run      # only report

Needs numpy (on Raspberry Pi OS: sudo apt install python3-numpy).
"""

import argparse
import os
import struct
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is needed: sudo apt install python3-numpy")


# --------------------------------------------------------------------------- WAV I/O

def read_wav(path):
    """Return (samples float64 [frames, channels], rate, bits, chunks) for 16/24/32-bit PCM."""
    with open(path, "rb") as f:
        data = f.read()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos, fmt, audio, chunks = 12, None, None, []
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            audio = body
        elif cid != b"smpl":
            chunks.append((cid, body))  # kept as they are (except an old smpl)
        pos += 8 + size + (size & 1)
    if fmt is None or audio is None:
        raise ValueError("missing fmt or data chunk")
    tag, ch, rate, _, block, bits = struct.unpack("<HHIIHH", fmt[:16])
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack("<H", fmt[24:26])[0]
    if tag != 1 or bits not in (16, 24, 32):
        raise ValueError(f"only 16/24/32-bit PCM is supported (format {tag}, {bits} bit)")
    frames = len(audio) // block
    raw = np.frombuffer(audio[:frames * block], dtype=np.uint8)
    if bits == 16:
        x = raw.view("<i2").astype(np.float64) / 32768.0
    elif bits == 32:
        x = raw.view("<i4").astype(np.float64) / 2147483648.0
    else:
        b = raw.reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(np.float64) / 8388608.0
    return x.reshape(frames, ch), rate, bits, fmt, chunks


def write_wav(path, x, rate, bits, fmt, chunks, loop_start, loop_end, root):
    frames, ch = x.shape
    if bits == 16:
        audio = np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2").tobytes()
    elif bits == 32:
        audio = np.clip(np.round(x * 2147483647.0), -2147483648, 2147483647).astype("<i4").tobytes()
    else:
        v = np.clip(np.round(x * 8388607.0), -8388608, 8388607).astype(np.int32).reshape(-1)
        v = np.where(v < 0, v + (1 << 24), v)
        b = np.stack([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF], axis=1).astype(np.uint8)
        audio = b.tobytes()
    smpl = struct.pack("<9I", 0, 0, int(round(1e9 / rate)), root, 0, 0, 0, 1, 0)
    smpl += struct.pack("<6I", 0, 0, loop_start, loop_end, 0, 0)  # forward loop, inclusive end

    def chunk(cid, body):
        return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

    body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"data", audio) + chunk(b"smpl", smpl)
    for cid, b in chunks:
        if cid not in (b"fmt ", b"data"):
            body += chunk(cid, b)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def has_loop(path):
    with open(path, "rb") as f:
        data = f.read()
    i = data.find(b"smpl")
    return i > 0 and i + 8 + 36 <= len(data) and struct.unpack("<I", data[i + 8 + 28:i + 8 + 32])[0] > 0


# --------------------------------------------------------------------------- analysis

def envelope(mono, rate, win_s=0.02):
    w = max(1, int(win_s * rate))
    n = len(mono) // w
    return np.sqrt(np.mean(mono[:n * w].reshape(n, w) ** 2, axis=1)), w


def sustain_region(mono, rate):
    """Frames [start, end) of the steady part: after the attack, before the decay."""
    env, w = envelope(mono, rate)
    if len(env) < 20:
        return None
    # Smooth over 200 ms: chorus and ensemble make the level wobble.
    k = max(1, int(0.2 * rate / w))
    smooth = np.convolve(env, np.ones(k) / k, mode="same")
    peak = smooth.max()
    if peak <= 1e-6:
        return None
    level = np.median(smooth[smooth > peak * 0.25])  # the level the sound settles at
    inside = np.nonzero(smooth > level * 0.85)[0]    # within about 1.4 dB of it
    if len(inside) == 0:
        return None
    margin = int(0.15 * rate / w)
    start, end = (int(inside[0]) + margin) * w, (int(inside[-1]) - margin) * w
    return (start, end) if end - start > int(0.3 * rate) else None


def similarity(mono, a, b, n):
    """Normalised correlation of mono[a:a+n] with mono[b:b+n]."""
    x, y = mono[a:a + n], mono[b:b + n]
    den = np.sqrt(np.dot(x, x) * np.dot(y, y)) or 1e-12
    return float(np.dot(x, y) / den)


def best_pair(mono, rate, start, end, max_len, xfade):
    """Loop start/end inside [start, end) with the most alike surroundings.

    The loop start keeps `xfade` frames of sustain before it, because the
    crossfade borrows them. Starts are tried across the slack left by
    max_len, ends sample by sample within 30 ms of the target length.
    """
    probe = int(0.02 * rate)  # compare 20 ms of waveform
    lo_start = start + xfade
    if end - lo_start < int(0.3 * rate):
        return None
    length = min(end - lo_start, int(max_len * rate))
    slack = max(0, end - lo_start - length)
    span = int(0.03 * rate)
    best = None
    for s in sorted(set(np.linspace(lo_start, lo_start + slack, 12).astype(int))):
        e_target = min(s + length, end)
        for e in range(max(s + int(0.3 * rate), e_target - span), e_target + 1, 2):
            if e + probe > len(mono):
                break
            c = similarity(mono, s - probe // 2, e - probe // 2, probe)
            if best is None or c > best[0]:
                best = (c, int(s), int(e))
    return best


def crossfade(x, s, e, n):
    """Blend the n frames before e with the n frames before s (equal power)."""
    t = (np.arange(n) + 0.5) / n
    fade_in, fade_out = np.sin(t * np.pi / 2)[:, None], np.cos(t * np.pi / 2)[:, None]
    y = x.copy()
    y[e - n:e] = x[e - n:e] * fade_out + x[s - n:s] * fade_in
    return y


def root_from_name(stem):
    import re
    m = re.search(r"(?<![0-9])(\d{1,3})(?![0-9])", stem)
    if m and 0 <= int(m.group(1)) <= 127:
        return int(m.group(1))
    return 60


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder")
    ap.add_argument("--out", help="output folder (default: <folder>-loop)")
    ap.add_argument("--max-loop", type=float, default=4.0, help="longest loop in seconds (default 4)")
    ap.add_argument("--xfade", type=float, default=0.3, help="crossfade in seconds (default 0.3)")
    ap.add_argument("--force", action="store_true", help="also files that already have a loop")
    ap.add_argument("--dry-run", action="store_true", help="only report, write nothing")
    args = ap.parse_args()

    folder = args.folder.rstrip("/")
    out = args.out or folder + "-loop"
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith(".wav"))
    if not files:
        sys.exit("no WAV files in " + folder)
    if not args.dry_run:
        os.makedirs(out, exist_ok=True)

    print(f"{'file':34} {'sustain s':>13} {'loop s':>15} {'match':>6}  result")
    done = 0
    for name in files:
        path = os.path.join(folder, name)
        try:
            if has_loop(path) and not args.force:
                print(f"{name:34} {'':>13} {'':>15} {'':>6}  has a loop already (--force to redo)")
                continue
            x, rate, bits, fmt, chunks = read_wav(path)
        except (ValueError, OSError) as e:
            print(f"{name:34} error: {e}")
            continue
        mono = x.mean(axis=1)
        region = sustain_region(mono, rate)
        if not region:
            print(f"{name:34} {'':>13} {'':>15} {'':>6}  no steady part long enough - skipped")
            continue
        start, end = region
        xf = min(int(args.xfade * rate), (end - start) // 4)
        pair = best_pair(mono, rate, start, end, args.max_loop, xf)
        if not pair:
            print(f"{name:34} {start / rate:5.2f}-{end / rate:5.2f}  {'':>15} {'':>6}  sustain too short - skipped")
            continue
        score, s, e = pair
        n = min(xf, (e - s) // 2, s)
        y = crossfade(x, s, e, n)
        status = "ok" if score > 0.9 else "ok, check by ear" if score > 0.6 else "weak match, check by ear"
        print(f"{name:34} {start / rate:5.2f}-{end / rate:5.2f}   {s / rate:6.2f}-{e / rate:6.2f}  {score:6.2f}  {status}")
        if not args.dry_run:
            write_wav(os.path.join(out, name), y, rate, bits, fmt, chunks, s, e - 1,
                      root_from_name(os.path.splitext(name)[0]))
            done += 1

    if not args.dry_run:
        print(f"\n{done} file(s) written to {out}")


if __name__ == "__main__":
    main()
