#!/usr/bin/env python3
"""Which unresolved stubs are real code that a lifted function jumps into.

    python scripts/stub-targets.py            # report
    python scripts/stub-targets.py --seeds    # print seed entries for the real ones

The lifter turns a direct jump to an address it has no function for into a call
of a stub in recomp_stubs_unresolved.c, and a stub only pops a return address.
When the target is the middle of the jumping function's own body -- a shared
epilogue the function extent stopped short of -- the function returns without
its epilogue: the stack pointer and the saved registers are left wrong, and the
caller fails later, somewhere else. That is how a Terrordome match crashed
(sub_001D83B0 -> 0x001D87AA, `pop esi; add esp, 0x28; ret`).

Seeding such a target makes the lifter translate it as a fragment, which works
because guest registers and the stack pointer are shared state: the fragment
continues exactly where the jump left off.

Only self-contained exits are seeded (`--seeds`): straight-line code from the
target to a `ret`, with no branch, ending where the containing function ends and
with no other label inside it. A seed ends the containing function's extent at
the seed, so a fragment that branches back into its parent, or a parent that
jumps past the seed, just turns one stub into several (seeding all 77 decodable
targets on 2026-10-05 produced 51 new ones) and a loop split across fragments
becomes unbounded mutual recursion. Those need the function's extent fixed in
the lifter instead, and the report lists them as "needs extent".

A target counts as real code when it is reached by a jump (not a call) from a
translated function, lies in an executable section, decodes without a break to
a `ret` or an unconditional jump, and sits on an instruction boundary of the
translated function that contains it (found by decoding that function from its
start). The last test matters most: some jumping "functions" are data that was
translated as code, their targets land mid-instruction, and a seed there would
cut a working function in two at the wrong byte. Those are left alone.

Reads the dump and the generated code; writes nothing.
"""
import glob
import os
import re
import sys

import capstone
from xbe import Xbe

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.environ.get("STUB_TARGETS_GEN") or os.path.join(REPO, "src", "recomp", "gen")
XBE = os.path.join(REPO, "game", "default.xbe")
MAX_BYTES = 4096


def stub_uses():
    text = open(os.path.join(GEN, "recomp_stubs_unresolved.c"), encoding="utf-8").read()
    stubs = set(re.findall(r"^void (sub_[0-9A-F]{8})\(void\) \{", text, re.M))
    uses, cur = {}, None
    for path in sorted(glob.glob(os.path.join(GEN, "recomp_0*.c"))):
        for line in open(path, encoding="utf-8", errors="replace"):
            m = re.match(r"void (sub_[0-9A-F]{8})\(void\)", line)
            if m:
                cur = m.group(1)
                continue
            for s in re.findall(r"(sub_[0-9A-F]{8})\(\)", line):
                if s in stubs and "return;" in line:      # a jump, lifted as a tail call
                    uses.setdefault(s, set()).add(cur)
    return stubs, uses


def decode(md, sections, va):
    """(ok, instructions) -- follows straight-line code to a ret or a jmp."""
    for base, data, executable in sections:
        if base <= va < base + len(data):
            if not executable:
                return False, []
            code = data[va - base: va - base + MAX_BYTES]
            out = []
            for ins in md.disasm(code, va):
                out.append(ins)
                if ins.mnemonic in ("ret", "retf", "jmp"):
                    return True, out
                if ins.mnemonic in ("int3", "hlt", "in", "out", "insb", "outsb", "iretd", "les", "lds",
                                    "aaa", "aas", "daa", "das", "arpl", "bound", "into", "salc"):
                    return False, out
                if len(out) >= 400:
                    break
            return False, out
    return False, []


def function_extents():
    """[(start, end, labels)] of every translated function, from the generated code."""
    out = []
    for path in sorted(glob.glob(os.path.join(GEN, "recomp_0*.c"))):
        text = open(path, encoding="utf-8", errors="replace").read()
        heads = list(re.finditer(r"Original: 0x([0-9A-F]{8}) - 0x([0-9A-F]{8})", text))
        for i, m in enumerate(heads):
            body = text[m.end(): heads[i + 1].start() if i + 1 < len(heads) else len(text)]
            labels = set(int(x, 16) for x in re.findall(r"^loc_([0-9A-F]{8}):", body, re.M))
            out.append((int(m.group(1), 16), int(m.group(2), 16), labels))
    return sorted(out, key=lambda e: e[0])


def self_contained(extents, va, ins):
    """Straight-line to a ret, ending the containing function, nothing else inside."""
    if not ins or ins[-1].mnemonic != "ret":
        return False
    if any(i.mnemonic.startswith("j") or i.mnemonic.startswith("loop") for i in ins):
        return False
    end = ins[-1].address + ins[-1].size
    for start, fend, labels in extents:
        if start < va < fend:
            return end == fend and not any(va < l < end for l in labels)
    return True


def on_boundary(md, sections, extents, va):
    """True/False when a translated function contains va; None when none does."""
    for start, end, _labels in extents:
        if start < va < end:
            for base, data, _ in sections:
                if base <= start < base + len(data):
                    at = start
                    for ins in md.disasm(data[start - base: va - base + 16], start):
                        if at == va:
                            return True
                        if at > va:
                            return False
                        at += ins.size
                    return at == va
    return None


def main():
    stubs, uses = stub_uses()
    xbe = Xbe.from_file(XBE)
    sections = []
    for name, sec in xbe.sections.items():
        h = sec.header
        sections.append((h.virtual_addr, bytes(sec.data), bool(h.flags & 0x4)))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    extents = function_extents()
    real, rejected = [], []
    for s in sorted(uses):
        va = int(s[4:], 16)
        ok, ins = decode(md, sections, va)
        if ok and on_boundary(md, sections, extents, va) is False:
            ok = False
        (real if ok else rejected).append((s, va, ins))
    if "--seeds" in sys.argv:
        for s, va, ins in real:
            if not self_contained(extents, va, ins):
                continue
            src = ", ".join(sorted(uses[s])[:2])
            print(' {\n  "start": "0x%08X",\n  "note": "Jump target inside %s that the lifter stubbed '
                  '(scripts/stub-targets.py); translated as a fragment so its epilogue runs."\n },' % (va, src))
        return 0
    print("%d stubs, %d reached by a jump: %d decode as code, %d do not" %
          (len(stubs), len(uses), len(real), len(rejected)))
    for s, va, ins in real:
        head = "; ".join("%s %s" % (i.mnemonic, i.op_str) for i in ins[:4]).strip()
        kind = "seed        " if self_contained(extents, va, ins) else "needs extent"
        print("  %s  %s %3d insns  from %-28s %s%s" % (s, kind, len(ins), ",".join(sorted(uses[s])[:2]), head[:60],
                                                      " ..." if len(ins) > 4 else ""))
    print("not code:")
    for s, va, ins in rejected:
        print("  %s  from %s" % (s, ",".join(sorted(uses[s])[:3])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
