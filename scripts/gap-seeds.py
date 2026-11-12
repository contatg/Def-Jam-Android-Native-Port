#!/usr/bin/env python3
"""Find functions the lifter never reached, in the gaps coverage-audit.py reports.

A function called only through data -- a vtable slot, a driver table, a
callback registered once -- has no direct caller, so the lifter's analysis never
finds it, and the first sign is an `[ICALL] Failed to resolve` in a run that
happened to go that way. The bytes are sitting in the image the whole time, in
the gaps between lifted functions.

A gap is also where jump tables, strings and padding live, so this skips
leading padding (int3, nop and the multi-byte nops MSVC aligns with), rejects a
gap whose first dword is an address in the image (a jump or pointer table), and
keeps a start only if it disassembles cleanly to a ret or jmp inside the gap.

    python scripts/gap-seeds.py                 # list candidates
    python scripts/gap-seeds.py --write         # add them to config/seed_functions.json

Then analyze.ps1, recomp.ps1, build.ps1 as for any seed. Compare the
`Original:` extents before and after the re-lift: a seed should add functions,
not move the edges of existing ones. On 2026-09-24 it found 427, among them the
function the title calls when START is pressed on the title screen.
"""
import argparse, importlib.util, json, os, struct, sys

CODE_SECTIONS = ('.text', 'XGRPH', 'D3D', 'D3DX', 'DSOUND', 'XPP')
IMAGE = (0x11000, 0x2D54C8)                  # .text through .rdata
NOTE = ('Uncovered code gap (scripts/gap-seeds.py): decodes cleanly to a return; '
        'reached only through data.')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--xbe', default='game/default.xbe')
    ap.add_argument('--gen', default='src/recomp/gen')
    ap.add_argument('--seeds', default='config/seed_functions.json')
    ap.add_argument('--write', action='store_true', help='append new candidates to --seeds')
    a = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location('ca', os.path.join(here, 'coverage-audit.py'))
    ca = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ca)
    try:
        from xbe import Xbe
        import capstone
    except ImportError:
        sys.exit('needs pyxbe and capstone: py -3 -m pip install pyxbe capstone')

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    secs = [(s.name, s.header.virtual_addr, s.header.virtual_size, bytes(s.data))
            for s in Xbe.from_file(a.xbe).sections.values()]
    merged = ca.lifted_extents(a.gen)

    gaps = []
    for name, base, size, data in secs:
        if name not in CODE_SECTIONS:
            continue
        cur = base
        for lo, hi in merged:
            if hi <= base or lo >= base + size:
                continue
            lo, hi = max(lo, base), min(hi, base + size)
            if lo > cur:
                gaps.append((cur, lo, data[cur - base:lo - base]))
            cur = max(cur, hi)

    found = []
    for lo, hi, b in gaps:
        i = 0
        while i < len(b):
            if b[i] in (0xCC, 0x90):
                i += 1
            elif b[i:i + 3] == b'\x8d\x49\x00' or b[i:i + 2] == b'\x8b\xff':
                i += 3 if b[i] == 0x8D else 2
            elif b[i:i + 7] == b'\x8d\xa4\x24\x00\x00\x00\x00':
                i += 7
            elif b[i:i + 6] == b'\x8d\x9b\x00\x00\x00\x00':
                i += 6
            else:
                break
        if len(b) - i < 8 or b[i] == 0:
            continue
        if IMAGE[0] <= struct.unpack_from('<I', b, i)[0] < IMAGE[1]:
            continue                          # a table of addresses
        start, n = lo + i, 0
        for ins in md.disasm(b[i:], start):
            n += 1
            if ins.mnemonic in ('ret', 'retn', 'jmp') and n >= 3:
                if ins.address + ins.size <= hi:
                    found.append((start, n))
                break
            if ins.mnemonic in ('int3', 'hlt'):
                break

    for start, n in found:
        print(f'0x{start:08X}  {n} instructions to the first return')
    print(f'{len(found)} candidate(s)', file=sys.stderr)

    if a.write:
        seeds = json.load(open(a.seeds))
        have = {int(e['start'], 16) for e in seeds}
        new = [s for s, _ in found if s not in have]
        seeds += [{'start': f'0x{s:08X}', 'note': NOTE} for s in new]
        with open(a.seeds, 'w', newline='\n') as f:
            f.write(json.dumps(seeds, indent=1) + '\n')
        print(f'{len(new)} added to {a.seeds}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
