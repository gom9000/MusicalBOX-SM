#!/usr/bin/env python3
"""Inventory of a sample library: format, length, loops and RAM needed by the engine.

For every folder containing WAV files it prints one summary line: number of
files, sample rates, bit depths, channels, size on disk, how many files carry
loop points, and the RAM the engine will use (it keeps audio as 16-bit).
With --files it also lists every file.

Usage:
    python3 tools/wav_inspect.py media
    python3 tools/wav_inspect.py media/tape --files

Only the Python standard library is used.
"""

import argparse
import os
import struct
import sys
from collections import defaultdict


def read_wav_info(path):
    """Parse RIFF chunks; return a dict or raise ValueError."""
    with open(path, "rb") as f:
        header = f.read(12)
        if len(header) < 12 or header[0:4] != b"RIFF" or header[8:12] != b"WAVE":
            raise ValueError("not a RIFF/WAVE file")
        info = {"loops": 0, "loop": None, "root": None}
        while True:
            head = f.read(8)
            if len(head) < 8:
                break
            cid, size = head[0:4], struct.unpack("<I", head[4:8])[0]
            if cid == b"fmt ":
                body = f.read(size)
                tag, ch, rate, _, block, bits = struct.unpack("<HHIIHH", body[:16])
                if tag == 0xFFFE and len(body) >= 26:
                    tag = struct.unpack("<H", body[24:26])[0]
                info.update(format=tag, channels=ch, rate=rate, block=block, bits=bits)
            elif cid == b"data":
                info["data_bytes"] = size
                f.seek(size, 1)
            elif cid == b"smpl" and size >= 36:
                body = f.read(size)
                root = struct.unpack("<I", body[12:16])[0]
                count = struct.unpack("<I", body[28:32])[0]
                info["root"] = root
                info["loops"] = count
                if count and len(body) >= 36 + 24:
                    _, _, start, end, _, _ = struct.unpack("<6I", body[36:60])
                    info["loop"] = (start, end)
            else:
                f.seek(size, 1)
            if size % 2:
                f.seek(1, 1)  # chunks are word aligned
    if "rate" not in info or "data_bytes" not in info:
        raise ValueError("missing fmt or data chunk")
    info["frames"] = info["data_bytes"] // max(1, info["block"])
    info["seconds"] = info["frames"] / info["rate"]
    info["ram"] = info["frames"] * info["channels"] * 2
    info["disk"] = os.path.getsize(path)
    return info


def fmt_set(values):
    return "/".join(str(v) for v in sorted(values))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("root", help="folder to scan (recursively)")
    p.add_argument("--files", action="store_true", help="list every file")
    args = p.parse_args()

    if not os.path.isdir(args.root):
        sys.exit(f"{args.root}: not a folder")

    folders = defaultdict(list)
    errors = []
    for dirpath, _, filenames in os.walk(args.root):
        for name in sorted(filenames):
            if not name.lower().endswith(".wav"):
                continue
            path = os.path.join(dirpath, name)
            try:
                folders[dirpath].append((name, read_wav_info(path)))
            except (ValueError, struct.error, OSError) as e:
                errors.append(f"{path}: {e}")

    if not folders:
        sys.exit("no WAV files found")

    total_disk = total_ram = 0
    print(f"{'folder':40} {'files':>5} {'rate':>11} {'bits':>6} {'ch':>4} {'length s':>13} "
          f"{'loops':>6} {'disk MB':>8} {'RAM MB':>7}")
    for folder in sorted(folders):
        items = folders[folder]
        rel = os.path.relpath(folder, args.root)
        secs = [i["seconds"] for _, i in items]
        disk = sum(i["disk"] for _, i in items)
        ram = sum(i["ram"] for _, i in items)
        loops = sum(1 for _, i in items if i["loop"])
        total_disk += disk
        total_ram += ram
        print(f"{rel[:40]:40} {len(items):5} {fmt_set({i['rate'] for _, i in items}):>11} "
              f"{fmt_set({i['bits'] for _, i in items}):>6} {fmt_set({i['channels'] for _, i in items}):>4} "
              f"{min(secs):6.1f}-{max(secs):<6.1f} {loops:6} {disk / 1048576:8.1f} {ram / 1048576:7.1f}")
        if args.files:
            for name, i in items:
                loop = f"loop {i['loop'][0]}-{i['loop'][1]}" if i["loop"] else "no loop"
                print(f"    {name:36} {i['rate']} Hz {i['bits']} bit {i['channels']} ch "
                      f"{i['seconds']:6.2f} s  {loop}")

    print(f"\ntotal: {total_disk / 1048576:.1f} MB on disk, {total_ram / 1048576:.1f} MB in the engine's RAM")
    for e in errors:
        print(f"error: {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
