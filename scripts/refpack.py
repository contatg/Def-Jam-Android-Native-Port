#!/usr/bin/env python3
"""Decompress EA RefPack, to see what an asset actually contains.

The title's screens are RefPack-compressed entries (signature 0x10FB) holding
a nested BIG archive. When a screen loads and nothing appears, the useful
question is what the loader was supposed to end up with, and that is
answerable here without running anything.

    python scripts/refpack.py screens/screens.viv screens/intMain/legal1.big

Prints the decompressed size and, when the result is itself an archive, its
directory. Metadata only -- names, offsets and sizes. It never writes game
bytes anywhere, which is the same line the rest of this repo draws.
"""

import os
import struct
import sys


def decompress(src):
    """EA RefPack. Returns the decompressed bytes."""
    sig = struct.unpack_from(">H", src, 0)[0]
    if sig & 0x3EFF != 0x10FB:
        raise SystemExit("not RefPack: signature 0x%04X" % sig)
    pos = 2
    wide = bool(sig & 0x8000)
    step = 4 if wide else 3
    if sig & 0x0100:              # compressed size present, and unused here
        pos += step
    size = int.from_bytes(src[pos:pos + step], "big")
    pos += step

    out = bytearray()
    while pos < len(src):
        b0 = src[pos]; pos += 1
        if b0 < 0x80:
            b1 = src[pos]; pos += 1
            run = b0 & 0x03
            length = ((b0 & 0x1C) >> 2) + 3
            dist = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1 = src[pos]; b2 = src[pos + 1]; pos += 2
            run = (b1 >> 6) & 0x03
            length = (b0 & 0x3F) + 4
            dist = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = src[pos], src[pos + 1], src[pos + 2]; pos += 3
            run = b0 & 0x03
            length = ((b0 & 0x0C) << 6) + b3 + 5
            dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            run = ((b0 & 0x1F) << 2) + 4
            length = dist = 0
        else:
            run = b0 & 0x03
            length = dist = 0

        out += src[pos:pos + run]
        pos += run
        for _ in range(length):
            out.append(out[-dist])
        if b0 >= 0xFC:
            break

    if len(out) != size:
        print("  (decompressed %d bytes, header said %d)" % (len(out), size))
    return bytes(out)


def big_entries(data):
    if data[:4] not in (b"BIG4", b"BIGF"):
        return None
    count = struct.unpack_from(">I", data, 8)[0]
    pos = 16
    out = []
    for _ in range(count):
        if pos + 8 > len(data):
            break
        off, size = struct.unpack_from(">II", data, pos)
        pos += 8
        end = data.find(b"\x00", pos)
        if end < 0:
            break
        out.append((data[pos:end].decode("latin-1"), off, size))
        pos = end + 1
    return out


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        raise SystemExit(__doc__)

    root = os.path.join(os.environ.get("DEFJAM_DATA", ""), "extracted")
    archive = os.path.join(root, argv[0].replace("/", os.sep))
    want = argv[1]

    with open(archive, "rb") as fh:
        head = fh.read(2 * 1024 * 1024)
        table = big_entries(head)
        if table is None:
            raise SystemExit("outer file is not a BIG archive")
        for name, off, size in table:
            if name == want or name.endswith("/" + want):
                fh.seek(off)
                blob = fh.read(size)
                out = decompress(blob)
                print("%s: %d compressed -> %d bytes, starts %r"
                      % (name, size, len(out), out[:4]))
                inner = big_entries(out)
                if inner is None:
                    head_hex = " ".join("%02X" % b for b in out[:24])
                    print("  not an archive; head %s" % head_hex)
                    return
                print("  %d entries:" % len(inner))
                for n, o, s in inner[:30]:
                    print("    %-40s off 0x%06X  %8d bytes" % (n, o, s))
                return
        raise SystemExit("no entry named %r" % want)


if __name__ == "__main__":
    main()
