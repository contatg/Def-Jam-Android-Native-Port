#!/usr/bin/env python3
"""Who calls this guest function, and who calls them.

Walking a call chain upwards by hand is a grep for the address, then an awk to
find which function the hit landed in, repeated for every level -- and the
answer that matters is usually three or four levels up, where something that
does run meets something that does not.

    python scripts/callers.py 0x0006FDC0            direct callers
    python scripts/callers.py 0x0006FDC0 --depth 3  and their callers, and so on

Direct calls only. An indirect call through a vtable or a dispatch table is
invisible here, so an empty result means "nothing calls this by name", not
"nothing calls this" -- and in this image the front end reaches a great deal
of its code indirectly. A function whose only entry is indirect shows up as a
root with no callers, which is itself worth knowing.
"""

import os
import re
import sys

GEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "src", "recomp", "gen")

DEF = re.compile(r"^void (sub_[0-9A-F]{8})\(void\)")


def index_files():
    """Each generated file as (lines, [(line_no, function_name)])."""
    out = {}
    for name in sorted(os.listdir(GEN)):
        if not name.endswith(".c") or name == "recomp_dispatch.c":
            continue
        path = os.path.join(GEN, name)
        with open(path, "r", errors="replace") as fh:
            lines = fh.readlines()
        defs = []
        for i, line in enumerate(lines):
            m = DEF.match(line)
            if m:
                defs.append((i, m.group(1)))
        out[name] = (lines, defs)
    return out


def enclosing(defs, line_no):
    best = None
    for i, name in defs:
        if i <= line_no:
            best = name
        else:
            break
    return best


def callers_of(index, addr):
    """Function names that call `addr` directly."""
    needle = "0x%08Xu" % addr
    found = set()
    for _name, (lines, defs) in index.items():
        for i, line in enumerate(lines):
            if needle in line and "RECOMP_ABI_CALL" in line:
                fn = enclosing(defs, i)
                if fn and fn != "sub_%08X" % addr:
                    found.add(fn)
    return found


def main():
    argv = sys.argv[1:]
    depth = 1
    if "--depth" in argv:
        k = argv.index("--depth")
        depth = int(argv[k + 1])
        del argv[k:k + 2]
    if not argv:
        raise SystemExit(__doc__)

    addr = int(argv[0], 16)
    if not os.path.isdir(GEN):
        raise SystemExit("no lifted C at " + GEN)

    index = index_files()
    level = {addr}
    seen = {addr}
    for d in range(1, depth + 1):
        names = set()
        for a in level:
            names |= callers_of(index, a)
        names = {n for n in names if int(n[4:], 16) not in seen}
        if not names:
            print("level %d: no direct callers (reached only indirectly)" % d)
            break
        print("level %d:" % d)
        for n in sorted(names):
            print("   ", n)
        level = {int(n[4:], 16) for n in names}
        seen |= level


if __name__ == "__main__":
    main()
