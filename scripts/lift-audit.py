#!/usr/bin/env python3
"""Count the lift patterns known to produce silently wrong code, without running anything.

Every lifter bug found so far in this title surfaced as game behaviour -- a
movie that never loaded, a copy that came out short -- and cost hours of
runtime tracing to reduce to one instruction. Most of them leave a shape in
the generated C that a text scan can see. This counts those shapes, so a
lifter change can be judged by re-lifting and re-counting rather than by
playing, and a regression shows up as a number going back up.

    python scripts/lift-audit.py                    (src/recomp/gen)
    python scripts/lift-audit.py path/to/gen [--list]
    python scripts/lift-audit.py --max unassigned_flags=41 --max cmps_then_sbb=0

What is counted:

  unassigned_flags  A read of `_flags` whose nearest flag writer in the same
                    basic block is not a `_flags =` assignment. `_flags` is a
                    fallback the lifter declares and only the rep-compare lift
                    assigns, so such a read is a branch or setcc that is
                    silently always false. Most of what remains is data bytes
                    lifted as code, which never runs.
  fcmp_fallback     A magnitude test at a block boundary guarded by `_fcmp`:
                    right when the flags came from a cmp, always false when
                    they came from arithmetic.
  cmps_then_sbb     `repe cmps` followed within a few lines by the `sbb`
                    carry-extend MSVC uses for memcmp ordering, in a lift where
                    the compare never writes `_cf`. Zero once the rep-compare
                    lift publishes the carry.

--max NAME=N makes the exit status non-zero when a count exceeds N, which is
how the unit test uses it. Reads only the generated C; never the game image.

These are historical pattern metrics. Functions using dynamic `_fv` flag
validity are excluded: this scanner cannot prove their control-flow semantics.
`--max` refuses such input instead of reporting a misleading green gate.
"""

import collections
import glob
import os
import re
import sys

FN = re.compile(r'^void (sub_[0-9A-F]{8})\(void\)')
LABEL = re.compile(r'^\s*loc_[0-9A-F]{8}:\s*;')
USE = re.compile(r'\b_flags\b')
ASSIGN = re.compile(r'\b_flags\s*=[^=]')
ZF_ASSIGN = re.compile(r'\b_zf\s*=[^=]')
FCMP = re.compile(r'\(_fcmp && _f')
REP_CMPS = re.compile(r'/\* repn?[ez]? cmps')
CARRY_WRITE = re.compile(r'\b_cf\s*=[^=]')
METRICS = ("unassigned_flags", "fcmp_fallback", "cmps_then_sbb")


def audit(gen_dir):
    counts = collections.Counter()
    where = collections.defaultdict(collections.Counter)
    for path in sorted(glob.glob(os.path.join(gen_dir, "recomp_0*.c"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        fn, state, dynamic = None, None, False
        for i, line in enumerate(lines):
            m = FN.match(line)
            if m:
                fn, state, dynamic = m.group(1), None, False
                continue
            if re.search(r'\bunsigned\s+_fv\b', line):
                dynamic = True
                counts["dynamic_flags_functions"] += 1
            if dynamic:
                continue
            if LABEL.match(line):
                state = "label"
                continue
            if FCMP.search(line):
                counts["fcmp_fallback"] += 1
                where[fn]["fcmp_fallback"] += 1
            if REP_CMPS.search(line):
                # Only the compare's own lines count: a `_cf = 0` from the xor
                # before the loop is exactly the stale carry being looked for.
                start = i
                while start > max(0, i - 15) and "RECOMP_DF_STEP" not in lines[start]:
                    start -= 1
                block = "\n".join(lines[start:i + 1])
                after = " ".join(lines[i + 1:i + 6])
                if "sbb self (CF extend)" in after and not CARRY_WRITE.search(block):
                    counts["cmps_then_sbb"] += 1
                    where[fn]["cmps_then_sbb"] += 1
            if "fallback flag var" in line:
                continue
            if ASSIGN.search(line):
                state = "flags"
                continue
            if ZF_ASSIGN.search(line):
                state = "zf"
            if USE.search(line) and state != "flags":
                counts["unassigned_flags"] += 1
                where[fn]["unassigned_flags"] += 1
    for key in METRICS:
        counts.setdefault(key, 0)
    return counts, where


def main(argv):
    gen_dir = os.path.join(os.path.dirname(__file__), "..", "src", "recomp", "gen")
    limits, show = {}, False
    it = iter(argv)
    for arg in it:
        if arg == "--list":
            show = True
        elif arg == "--max":
            name, _, value = next(it).partition("=")
            limits[name] = int(value)
        else:
            gen_dir = arg
    if not glob.glob(os.path.join(gen_dir, "recomp_0*.c")):
        print("no lifted C in %s (run scripts/recomp.ps1)" % gen_dir)
        return 2
    counts, where = audit(gen_dir)
    unknown = set(limits) - set(METRICS)
    if unknown:
        print("unsupported --max metrics: " + ", ".join(sorted(unknown)))
        return 2
    if counts["dynamic_flags_functions"]:
        print("%d dynamic-flag functions excluded; these metrics do not validate their flags."
              % counts["dynamic_flags_functions"])
        if limits:
            print("REFUSED: historical --max gates cannot certify dynamic-flag input; use compiled recompiler tests.")
            return 2
        print("Historical metrics below apply only to remaining legacy functions:")
    for key in METRICS:
        n_fn = sum(1 for c in where.values() if c[key])
        print("%-17s %6d  in %d functions" % (key, counts[key], n_fn))
    if show:
        for fn in sorted(where):
            print("  %s  %s" % (fn, dict(where[fn])))
    over = [k for k, n in limits.items() if counts.get(k, 0) > n]
    for k in over:
        print("FAIL: %s = %d, limit %d" % (k, counts[k], limits[k]))
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
