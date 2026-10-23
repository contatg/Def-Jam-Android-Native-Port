#!/usr/bin/env python3
"""Find the guest virtual address of a byte string inside the title image.

The lifted C has no string literals in it. A printf-style format that the
original code passed by pointer comes through as a bare constant -- a
`PUSH32(esp, 0x2972C4)` with nothing to say what is there -- so searching the
lifted source for "%s/%s.big" finds nothing at all, and a search for the
function that builds a screen path gets nowhere.

The string is still in the image, though, and the image says where each of its
sections lands. Resolving the string to a virtual address turns an
unsearchable question into a grep: find the address here, then grep the lifted
C for that constant, and every site that names the string is a site that uses
it.

    python scripts/string-va.py "%s/%s.big"
    python scripts/string-va.py --raw 3C666566            (hex bytes)

Prints every match as `VA  file-offset  context`, because a format string
usually sits in a run of related ones and the neighbours are often the better
lead.
"""

import os
import struct
import sys


def sections(data):
    """(virtual_addr, virtual_size, raw_addr, raw_size) for each section."""
    if data[:4] != b"XBEH":
        raise SystemExit("not an XBE: bad magic")
    base = struct.unpack_from("<I", data, 0x104)[0]
    count = struct.unpack_from("<I", data, 0x11C)[0]
    headers_va = struct.unpack_from("<I", data, 0x120)[0]
    # Header addresses are virtual; inside the header region the file is
    # mapped one-to-one from the base, so subtracting it gives the offset.
    off = headers_va - base
    out = []
    for i in range(count):
        h = off + i * 0x38
        va, vsize, raw, rsize = struct.unpack_from("<IIII", data, h + 4)
        out.append((va, vsize, raw, rsize))
    return base, out


def to_va(secs, offset):
    for va, _vsize, raw, rsize in secs:
        if raw <= offset < raw + rsize:
            return va + (offset - raw)
    return None


def main():
    argv = sys.argv[1:]
    raw_mode = False
    if argv and argv[0] == "--raw":
        raw_mode = True
        argv = argv[1:]
    if not argv:
        raise SystemExit(__doc__)

    needle = bytes.fromhex(argv[0]) if raw_mode else argv[0].encode("latin-1")

    xbe = os.path.join(os.environ.get("DEFJAM_DATA", ""), "extracted", "default.xbe")
    if len(argv) > 1:
        xbe = argv[1]
    if not os.path.exists(xbe):
        raise SystemExit("cannot find the image: " + xbe)

    data = open(xbe, "rb").read()
    _base, secs = sections(data)

    found = 0
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        start = i + 1
        va = to_va(secs, i)
        if va is None:
            continue
        # A printable run around the hit, which is usually the neighbouring
        # formats and is often what you actually wanted.
        lo = i
        while lo > 0 and 0x20 <= data[lo - 1] < 0x7F:
            lo -= 1
        hi = i + len(needle)
        while hi < len(data) and 0x20 <= data[hi] < 0x7F:
            hi += 1
        ctx = data[lo:hi].decode("latin-1")
        print("VA 0x%08X  file 0x%06X  %s" % (va, i, ctx))
        found += 1

    if not found:
        print("not found in any mapped section")


if __name__ == "__main__":
    main()
