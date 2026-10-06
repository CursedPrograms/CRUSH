#!/usr/bin/env python3
"""
CRUSH — a from-scratch archiver.

The compression math lives in crush/core.py (a binary range coder + an adaptive
context model, no zlib). This file is the WinRAR/7-zip-shaped wrapper around it:
a .crush container that holds many files, each with its length and a CRC32 so you
can verify the archive is intact.

    python crush.py a out.crush file1 file2 ...   create / add
    python crush.py x out.crush [dest_dir]        extract
    python crush.py l out.crush                   list contents + ratios
    python crush.py t out.crush                   test integrity (CRC32)

A file is stored compressed only when that actually makes it smaller; otherwise
it's kept raw (method 0), exactly like a real archiver, so random/already-packed
data never grows.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crush.core import compress, decompress

MAGIC = b"CRUSH\x01"
METHOD_RAW = 0
METHOD_CRUSH = 1

# ── CRC32 (IEEE, our own table — no zlib) ────────────────────────────────────
_CRC_TABLE = []
for _n in range(256):
    _c = _n
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if (_c & 1) else (_c >> 1)
    _CRC_TABLE.append(_c)


def crc32(data):
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


# ── Container ────────────────────────────────────────────────────────────────
def create(archive, files):
    total_in = total_out = 0
    with open(archive, "wb") as w:
        w.write(MAGIC)
        w.write(struct.pack("<I", len(files)))
        for path in files:
            with open(path, "rb") as f:
                data = f.read()
            comp = compress(data)
            if len(comp) < len(data):
                method, stored = METHOD_CRUSH, comp
            else:
                method, stored = METHOD_RAW, data        # never let it grow
            name = os.path.basename(path).encode("utf-8")
            w.write(struct.pack("<B", method))
            w.write(struct.pack("<H", len(name)))
            w.write(name)
            w.write(struct.pack("<Q", len(data)))
            w.write(struct.pack("<I", crc32(data)))
            w.write(struct.pack("<Q", len(stored)))
            w.write(stored)
            total_in += len(data)
            total_out += len(stored)
            pct = (100.0 * len(stored) / len(data)) if data else 100.0
            print(f"  + {os.path.basename(path):30} {len(data):>10} -> {len(stored):>10}  ({pct:5.1f}%)")
    arc = os.path.getsize(archive)
    saved = (100.0 * (1 - arc / total_in)) if total_in else 0.0
    print(f"\n{archive}: {len(files)} file(s), {total_in} -> {arc} bytes ({saved:.1f}% saved)")


def _read_entries(r):
    magic = r.read(len(MAGIC))
    if magic != MAGIC:
        raise ValueError("not a CRUSH archive")
    (count,) = struct.unpack("<I", r.read(4))
    for _ in range(count):
        (method,) = struct.unpack("<B", r.read(1))
        (nlen,) = struct.unpack("<H", r.read(2))
        name = r.read(nlen).decode("utf-8")
        (osize,) = struct.unpack("<Q", r.read(8))
        (crc,) = struct.unpack("<I", r.read(4))
        (ssize,) = struct.unpack("<Q", r.read(8))
        stored = r.read(ssize)
        yield method, name, osize, crc, stored


def _restore(method, osize, stored):
    return stored if method == METHOD_RAW else decompress(stored, osize)


def extract(archive, dest="."):
    os.makedirs(dest, exist_ok=True)
    with open(archive, "rb") as r:
        for method, name, osize, crc, stored in _read_entries(r):
            data = _restore(method, osize, stored)
            ok = crc32(data) == crc and len(data) == osize
            out = os.path.join(dest, os.path.basename(name))
            with open(out, "wb") as w:
                w.write(data)
            print(f"  {'OK ' if ok else 'BAD'}  {name}  ({osize} bytes)")
            if not ok:
                print(f"      ! CRC mismatch — {name} may be corrupt")


def list_archive(archive):
    with open(archive, "rb") as r:
        print(f"{'name':30} {'size':>12} {'stored':>12} {'ratio':>7}  method")
        for method, name, osize, crc, stored in _read_entries(r):
            pct = (100.0 * len(stored) / osize) if osize else 100.0
            m = "crush" if method == METHOD_CRUSH else "raw"
            print(f"{name:30} {osize:>12} {len(stored):>12} {pct:6.1f}%  {m}")


def test(archive):
    bad = 0
    with open(archive, "rb") as r:
        for method, name, osize, crc, stored in _read_entries(r):
            data = _restore(method, osize, stored)
            ok = crc32(data) == crc and len(data) == osize
            print(f"  {'OK ' if ok else 'BAD'}  {name}")
            bad += 0 if ok else 1
    print("All files OK." if bad == 0 else f"{bad} file(s) failed.")
    return bad == 0


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd, archive, rest = argv[1], argv[2], argv[3:]
    if cmd == "a":
        if not rest:
            print("nothing to add")
            return 1
        create(archive, rest)
    elif cmd == "x":
        extract(archive, rest[0] if rest else ".")
    elif cmd == "l":
        list_archive(archive)
    elif cmd == "t":
        return 0 if test(archive) else 2
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
