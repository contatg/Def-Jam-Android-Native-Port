#!/usr/bin/env python3
"""List or inspect one entry of a BIG4 archive from the user's own dump.

When the title loads an asset and nothing appears, the first question is
whether what it was handed is what it expected. That is answerable offline:
the archive's directory says where each entry starts and how long it is, and
the first bytes of the entry say what kind of thing it is. Doing it here
rather than by watching reads go past avoids guessing at a stream mid-flight.

    python scripts/big-entry.py screens/screens.viv
    python scripts/big-entry.py screens/screens.viv screens/intMain/legal1.big

Paths are relative to $DEFJAM_DATA/extracted. Prints offsets, sizes and a
short hex-and-ASCII head of the entry. It never writes game bytes anywhere.
"""

import os
import struct
import sys


def entries(data):
    """(name, offset, size) for each entry of a BIG4 directory."""
    if data[:4] not in (b"BIG4", b"BIGF"):
        raise SystemExit("not a BIG archive: %r" % data[:4])
    # BIG4: magic, total size (LE), file count (BE), header size (BE).
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
        name = data[pos:end].decode("latin-1")
        pos = end + 1
        out.append((name, off, size))
    return out


def main():
    argv = sys.argv[1:]
    if not argv:
        raise SystemExit(__doc__)

    root = os.path.join(os.environ.get("DEFJAM_DATA", ""), "extracted")
    path = os.path.join(root, argv[0].replace("/", os.sep))
    if not os.path.exists(path):
        raise SystemExit("no such archive: " + path)

    # The directory lives at the front; reading a slice keeps a 96 MB archive
    # off the heap when all that is wanted is the table.
    with open(path, "rb") as fh:
        head = fh.read(2 * 1024 * 1024)
        table = entries(head)

        if len(argv) < 2:
            print("%d entries" % len(table))
            for name, off, size in table[:40]:
                print("  %-44s off 0x%08X  %8d bytes" % (name, off, size))
            return

        want = argv[1]
        for name, off, size in table:
            if name == want or name.endswith("/" + want):
                print("%s\n  offset 0x%08X  size %d bytes" % (name, off, size))
                fh.seek(off)
                blob = fh.read(min(size, 64))
                hexs = " ".join("%02X" % b for b in blob[:32])
                text = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in blob[:32])
                print("  head %s" % hexs)
                print("       %s" % text)
                return
        print("no entry named %r" % want)


if __name__ == "__main__":
    main()
