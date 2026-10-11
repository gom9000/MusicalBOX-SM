#!/usr/bin/env python3
"""Extract the samples of a SoundFont (.sf2) as WAV files ready for MusicalBOX SM.

Every sample becomes a 16-bit WAV named "<note>_<sample name>.wav", where
<note> is its MIDI root note (the instrument's "overriding root key" when the
SoundFont sets one, otherwise the sample's own pitch). The SoundFont's loop
points go into the 'smpl' chunk, so "loop": "on" works at once.

In instruments.json the folder is then read with:
    "files": "{note}_*.wav"

Usage:
    python3 tools/sf2_extract.py Strings.sf2                  # list only
    python3 tools/sf2_extract.py Strings.sf2 --out media/strings/sf2

Only the Python standard library is used.
"""

import argparse
import os
import re
import struct
import sys

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Generator numbers of the SoundFont 2 specification.
GEN_KEY_RANGE, GEN_VEL_RANGE, GEN_SAMPLE_ID, GEN_ROOT_KEY = 43, 44, 53, 58
GEN_INSTRUMENT = 41


def note_name(n):
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 2}"  # C3 = 60, as MusicalBOX SM


def chunks(data, pos, end):
    """Yield (id, body_start, size) for the RIFF chunks in data[pos:end]."""
    while pos + 8 <= end:
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        yield cid, pos + 8, size
        pos += 8 + size + (size & 1)


def read_sf2(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[0:4] != b"RIFF" or data[8:12] != b"sfbk":
        raise ValueError("not a SoundFont 2 file")
    smpl = None
    pdta = {}
    info_name = ""
    for cid, start, size in chunks(data, 12, len(data)):
        if cid != b"LIST":
            continue
        kind = data[start:start + 4]
        for sub, s2, n2 in chunks(data, start + 4, start + size):
            if kind == b"sdta" and sub == b"smpl":
                smpl = (s2, n2)
            elif kind == b"pdta":
                pdta[sub.decode("latin-1")] = data[s2:s2 + n2]
            elif kind == b"INFO" and sub == b"INAM":
                info_name = data[s2:s2 + n2].split(b"\0")[0].decode("latin-1", "replace")
    if smpl is None or "shdr" not in pdta:
        raise ValueError("no sample data in this SoundFont")
    return data, smpl, pdta, info_name


def records(blob, fmt):
    size = struct.calcsize(fmt)
    return [struct.unpack(fmt, blob[i:i + size]) for i in range(0, len(blob) - size + 1, size)]


def parse(pdta):
    """Return the sample headers and, per sample, what the instruments say about it."""
    shdr = []
    for r in records(pdta["shdr"], "<20sIIIIIBbHH")[:-1]:  # the last record is "EOS"
        name = r[0].split(b"\0")[0].decode("latin-1", "replace").strip()
        shdr.append(dict(name=name, start=r[1], end=r[2], loop_start=r[3], loop_end=r[4],
                         rate=r[5], pitch=r[6], correction=r[7], link=r[8], type=r[9]))

    inst = records(pdta.get("inst", b""), "<20sH")
    ibag = records(pdta.get("ibag", b""), "<HH")
    igen = records(pdta.get("igen", b""), "<HH")
    usage = {}  # sample index -> list of (instrument, key range, velocity range, root override)
    for i in range(len(inst) - 1):
        iname = inst[i][0].split(b"\0")[0].decode("latin-1", "replace").strip()
        for b in range(inst[i][1], inst[i + 1][1]):
            if b + 1 >= len(ibag):
                break
            gens = {}
            for g in range(ibag[b][0], ibag[b + 1][0]):
                if g < len(igen):
                    gens[igen[g][0]] = igen[g][1]
            if GEN_SAMPLE_ID not in gens:
                continue  # global zone
            keys = gens.get(GEN_KEY_RANGE)
            vels = gens.get(GEN_VEL_RANGE)
            usage.setdefault(gens[GEN_SAMPLE_ID], []).append(dict(
                instrument=iname,
                keys=(keys & 0xFF, keys >> 8) if keys is not None else (0, 127),
                vels=(vels & 0xFF, vels >> 8) if vels is not None else (0, 127),
                root=gens.get(GEN_ROOT_KEY) if gens.get(GEN_ROOT_KEY, 255) <= 127 else None))
    return shdr, usage


def wav_bytes(samples, rate, loop, root):
    data = samples
    smpl = struct.pack("<9I", 0, 0, int(round(1e9 / rate)), root, 0, 0, 0, 1 if loop else 0, 0)
    if loop:
        smpl += struct.pack("<6I", 0, 0, loop[0], loop[1], 0, 0)
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)

    def chunk(cid, body):
        return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

    body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"data", data) + chunk(b"smpl", smpl)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def safe(name):
    return re.sub(r"[^A-Za-z0-9#+-]+", "-", name).strip("-") or "sample"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sf2")
    ap.add_argument("--out", help="folder for the WAV files (without it: only list the samples)")
    args = ap.parse_args()

    try:
        data, (smpl_pos, smpl_size), pdta, title = read_sf2(args.sf2)
        shdr, usage = parse(pdta)
    except (ValueError, OSError, struct.error) as e:
        sys.exit(f"{args.sf2}: {e}")

    print(f"{os.path.basename(args.sf2)}: {title or '(no name)'} - {len(shdr)} samples")
    print(f"{'#':>3} {'sample':24} {'root':>9} {'keys':>11} {'vel':>8} {'rate':>6} {'length s':>8}  loop")
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    written, roots = 0, {}
    for idx, h in enumerate(shdr):
        if h["type"] & 0x8000:  # ROM sample: no data in the file
            continue
        uses = usage.get(idx, [])
        root = next((u["root"] for u in uses if u["root"] is not None), None)
        if root is None:
            root = h["pitch"] if h["pitch"] <= 127 else 60
        keys = (min(u["keys"][0] for u in uses), max(u["keys"][1] for u in uses)) if uses else None
        vels = (min(u["vels"][0] for u in uses), max(u["vels"][1] for u in uses)) if uses else None
        frames = h["end"] - h["start"]
        if frames <= 0 or smpl_pos + 2 * h["end"] > smpl_pos + smpl_size:
            print(f"{idx:3} {h['name'][:24]:24} bad sample bounds - skipped")
            continue
        has_loop = h["start"] <= h["loop_start"] < h["loop_end"] <= h["end"] and h["loop_end"] - h["loop_start"] > 8
        # SoundFont loop end is the first sample after the loop; the smpl chunk wants the last one.
        loop = (h["loop_start"] - h["start"], h["loop_end"] - h["start"] - 1) if has_loop else None
        stereo = {2: " (right)", 4: " (left)"}.get(h["type"] & 0x7, "")
        print(f"{idx:3} {h['name'][:24]:24} {root:3} {note_name(root):>5} "
              f"{(f'{note_name(keys[0])}-{note_name(keys[1])}' if keys else '-'):>11} "
              f"{(f'{vels[0]}-{vels[1]}' if vels else '-'):>8} {h['rate']:6} {frames / h['rate']:8.2f}  "
              f"{'yes' if loop else 'no'}{stereo}")
        roots.setdefault(root, []).append(h["name"])

        if args.out:
            pcm = data[smpl_pos + 2 * h["start"]:smpl_pos + 2 * h["end"]]
            name = f"{root:03d}_{safe(h['name'])}.wav"
            with open(os.path.join(args.out, name), "wb") as f:
                f.write(wav_bytes(pcm, h["rate"], loop, root))
            written += 1

    dup = {r: n for r, n in roots.items() if len(n) > 1}
    if dup:
        print("\nWarning: several samples share a root note (velocity layers or left/right pairs):")
        for r, names in sorted(dup.items()):
            print(f"  {r} {note_name(r)}: {', '.join(names)}")
        print("The engine keeps one sample per note and velocity: split them into folders, or")
        print("use a pattern that tells them apart.")
    if args.out:
        print(f"\n{written} WAV file(s) written to {args.out} - in instruments.json: \"files\": \"{{note}}_*.wav\"")


if __name__ == "__main__":
    main()
