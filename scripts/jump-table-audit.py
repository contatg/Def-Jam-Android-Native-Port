#!/usr/bin/env python3
"""Find indexed jumps whose arms were never lifted.

The recompiler detects a function's extent before it lifts it, and an indexed
jump's arms often sit past the point where that detection stops. When that
happens the lifter emits a `goto` for the arms it has and leaves the rest to
RECOMP_ITAIL, which dispatches the raw address at runtime -- and a mid-function
address is not a function, so nothing resolves it. The switch silently loses
cases. A state machine missing one arm behaves exactly like a title that runs
fine and never advances, which is why this is worth auditing rather than
waiting to trip over.

The generated C cannot show this on its own: an arm the lifter could not place
simply is not mentioned. So this reads the tables out of the image. Each
emitted switch says where its table is and how many entries it has

    { uint32_t _jt = MEM32(eax * 4 + 0x17DC60); /* switch: 6 entries, 6 targets */

and the `goto` lines that follow say which of those the lifter placed. Anything
in the table that is not one of the function's own labels was lost.

    python scripts/jump-table-audit.py
    python scripts/jump-table-audit.py --xbe game/default.xbe --verbose

Note that "entries" and "targets" differing in the comment is normal and not a
symptom: several case values commonly share one arm.

**What this cannot see.** It is driven by the switches the lifter emitted, so
it checks every table the lifter found. A table in code the lifter never
reached at all -- because an earlier truncation cut the body before it -- is
invisible here and to the generated C both. Catching those needs real function
extents to compare against, which is item 1 of docs/04-improvement-backlog.md.
So a clean result means "every table that was lifted is complete", not "no code
was lost".
"""
import argparse, os, re, struct, sys

SWITCH_RE = re.compile(
    r"_jt = MEM32\([^)]*?\+ 0x([0-9A-Fa-f]+)\);\s*/\* switch: (\d+) entries")
GOTO_RE = re.compile(r"_jt == 0x([0-9A-Fa-f]{8})u")
LABEL_RE = re.compile(r"^loc_([0-9A-Fa-f]{8}):")
FUNC_RE = re.compile(r"^void (sub_([0-9A-Fa-f]{8}))(?:_gen)?\(void\)")


def load_image(path):
    """VA -> bytes reader for the image's sections."""
    try:
        from xbe import Xbe
    except ImportError:
        sys.exit("pyxbe is not installed: py -3 -m pip install pyxbe")
    xbe = Xbe.from_file(path)
    spans = []
    for s in xbe.sections.values():
        spans.append((s.header.virtual_addr, s.header.virtual_size, bytes(s.data)))

    def read32(va):
        for base, size, data in spans:
            if base <= va < base + size:
                off = va - base
                if off + 4 <= len(data):
                    return struct.unpack_from("<I", data, off)[0]
                return None                     # in a .bss tail
        return None
    return read32


def functions(gen_dir):
    """Each function with its labels and its switch blocks, in source order."""
    for name in sorted(os.listdir(gen_dir)):
        if not name.endswith(".c"):
            continue
        current = None
        labels, switches, pending = set(), [], None
        with open(os.path.join(gen_dir, name), errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    if current:
                        if pending:
                            switches.append(pending)
                        yield name, current, labels, switches
                    current, labels, switches, pending = m.group(2), set(), [], None
                    continue
                m = LABEL_RE.match(line)
                if m:
                    labels.add(int(m.group(1), 16))
                m = SWITCH_RE.search(line)
                if m:
                    if pending:
                        switches.append(pending)
                    pending = {"table": int(m.group(1), 16),
                               "count": int(m.group(2)),
                               "placed": set()}
                    continue
                if pending:
                    g = GOTO_RE.search(line)
                    if g:
                        pending["placed"].add(int(g.group(1), 16))
                    elif "RECOMP_ITAIL" in line:
                        switches.append(pending)
                        pending = None
        if current:
            if pending:
                switches.append(pending)
            yield name, current, labels, switches


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xbe", default="game/default.xbe")
    ap.add_argument("--gen", default="src/recomp/gen")
    ap.add_argument("--verbose", action="store_true", help="list every lost arm")
    a = ap.parse_args()

    read32 = load_image(a.xbe)
    tables = lost_tables = lost_arms = unreadable = 0
    report = []

    for _, func, labels, switches in functions(a.gen):
        fva = int(func, 16)
        for sw in switches:
            tables += 1
            missing = []
            for i in range(sw["count"]):
                target = read32(sw["table"] + i * 4)
                if target is None:
                    unreadable += 1
                    continue
                if target not in labels and target != fva:
                    missing.append((i, target))
            if missing:
                lost_tables += 1
                lost_arms += len(missing)
                report.append((func, sw["table"], sw["count"], missing))

    print(f"{tables} jump tables read from the image")
    print(f"{lost_tables} of them have arms the lifter never placed "
          f"({lost_arms} arms in total)")
    if unreadable:
        print(f"{unreadable} table entries could not be read (uninitialised section)")
    report.sort(key=lambda r: -len(r[3]))
    for func, table, count, missing in report[: (len(report) if a.verbose else 20)]:
        print(f"\n  sub_{func}  table 0x{table:08X}, {count} entries, "
              f"{len(missing)} lost:")
        for i, target in missing[: (len(missing) if a.verbose else 6)]:
            print(f"    case {i:<3} -> 0x{target:08X}  (no label in this function)")
    if report:
        print("\nEach of these is fixed by hand-writing the function into "
              "src/recomp_manual.c, which the lifter then stops generating.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
