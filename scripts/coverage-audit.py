#!/usr/bin/env python3
"""How much of the image was lifted, and what was left behind.

Every generated function carries its extent in a comment:

    * Original: 0x0021EA70 - 0x0021EB1F (175 bytes, 58 insns)

so the lifted output says exactly which bytes it covers. Anything in an
executable section that no function covers was not translated, and the title
can only reach it through a path nothing has exercised yet -- which is the one
remaining way this port could be missing code without anything logging it.

    python scripts/coverage-audit.py
    python scripts/coverage-audit.py --min-gap 64 --show 40

Not every gap is a defect. Compilers put jump tables, string literals and
alignment padding in .text, and those are data that should not be lifted. The
report prints the first bytes of each gap so the difference is visible: a run
of 0xCC is padding, ASCII is a string, a column of addresses in the image's own
range is a jump table, and anything that looks like a prologue (`55 8B EC`,
`83 EC`, `53 56 57`) is code that was missed.

scripts/jump-table-audit.py answers the narrower question of whether the tables
that *were* lifted lost any arms.
"""
import argparse, os, re, sys

EXTENT_RE = re.compile(r"Original:\s*0x([0-9A-Fa-f]+)\s*-\s*0x([0-9A-Fa-f]+)")
PROLOGUES = (b"\x55\x8b\xec", b"\x53\x56\x57", b"\x83\xec", b"\x81\xec",
             b"\x56\x8b\xf1", b"\x8b\xff\x55\x8b\xec")


def lifted_extents(gen_dir):
    spans = []
    for name in sorted(os.listdir(gen_dir)):
        if not name.endswith(".c"):
            continue
        with open(os.path.join(gen_dir, name), errors="replace") as f:
            for line in f:
                m = EXTENT_RE.search(line)
                if m:
                    spans.append((int(m.group(1), 16), int(m.group(2), 16)))
    spans.sort()
    merged = []
    for lo, hi in spans:
        if merged and lo <= merged[-1][1]:
            if hi > merged[-1][1]:
                merged[-1] = (merged[-1][0], hi)
        else:
            merged.append((lo, hi))
    return merged


def classify(data):
    """A one-word guess at what a gap holds, from its first bytes."""
    if not data:
        return "unreadable"
    if all(b in (0xCC, 0x90, 0x00) for b in data[:16]):
        return "padding"
    printable = sum(1 for b in data[:32] if 0x20 <= b < 0x7F)
    if printable >= 28:
        return "string data"
    if any(data.startswith(p) for p in PROLOGUES):
        return "CODE (looks like a prologue)"
    return "unclassified"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xbe", default="game/default.xbe")
    ap.add_argument("--gen", default="src/recomp/gen")
    ap.add_argument("--min-gap", type=int, default=32)
    ap.add_argument("--show", type=int, default=20)
    a = ap.parse_args()

    try:
        from xbe import Xbe
    except ImportError:
        sys.exit("pyxbe is not installed: py -3 -m pip install pyxbe")

    xbe = Xbe.from_file(a.xbe)
    merged = lifted_extents(a.gen)
    print(f"{len(merged)} contiguous lifted runs from the generated C")

    total_code = total_lifted = 0
    gaps = []
    per_section = []
    # A section counts as code when at least one lifted function is inside it.
    # The XBE's own flags do not discriminate -- this image marks .data
    # executable too -- so the lifted output is the better witness.
    for s in xbe.sections.values():
        base, size = s.header.virtual_addr, s.header.virtual_size
        if not any(base <= lo < base + size for lo, _ in merged):
            continue
        lifted_here = 0
        total_code += size
        cursor = base
        for lo, hi in merged:
            if hi <= base or lo >= base + size:
                continue
            lo = max(lo, base)
            hi = min(hi, base + size)
            if lo > cursor:
                gaps.append((lo - cursor, cursor, lo, s.name))
            lifted_here += hi - max(lo, cursor)
            cursor = max(cursor, hi)
        if cursor < base + size:
            gaps.append((base + size - cursor, cursor, base + size, s.name))
        total_lifted += lifted_here
        per_section.append((s.name, size, lifted_here))

    pct = (100.0 * total_lifted / total_code) if total_code else 0.0
    print(f"{total_lifted} of {total_code} bytes in code-bearing sections "
          f"lifted ({pct:.2f}%)")
    for name, size, lifted in per_section:
        share = (100.0 * lifted / size) if size else 0.0
        print(f"  {name:<12s} {lifted:8d} / {size:8d}  {share:6.2f}%")

    gaps = [g for g in gaps if g[0] >= a.min_gap]
    gaps.sort(reverse=True)
    print(f"{len(gaps)} gaps of {a.min_gap} bytes or more:")

    def section_bytes(va, n):
        for s in xbe.sections.values():
            b, sz = s.header.virtual_addr, s.header.virtual_size
            if b <= va < b + sz:
                off = va - b
                return bytes(s.data)[off:off + n]
        return b""

    for size, lo, hi, name in gaps[:a.show]:
        head = section_bytes(lo, 32)
        print(f"  0x{lo:08X}..0x{hi:08X}  {size:6d} bytes  {name:<10s} "
              f"{classify(head)}")
        print(f"      {head[:16].hex(' ')}")
    suspicious = [g for g in gaps if classify(section_bytes(g[1], 32)).startswith("CODE")]
    if suspicious:
        print(f"\n{len(suspicious)} gap(s) start with something that looks like a "
              f"function prologue. Those are the ones worth seeding:")
        for size, lo, hi, name in suspicious[:a.show]:
            print(f"  0x{lo:08X}  {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
