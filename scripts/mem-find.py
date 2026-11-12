#!/usr/bin/env python3
"""Search the running title's guest memory for a byte pattern.

Some questions are only answerable by looking at what is actually in memory.
"Did that archive decompress" is one: the compressed form is on disc and the
decompressed form should be in RAM, and a watchpoint cannot tell you whether a
buffer full of the right bytes ever came into being. A search can.

    python scripts/mem-find.py BIGF
    python scripts/mem-find.py --hex 10FB
    python scripts/mem-find.py FWS --max 20

Prints the guest address of each hit with a short printable context. Reads
only, through ReadProcessMemory, so it cannot disturb what it is measuring --
though it is a snapshot of a moving target, and a buffer that is filled and
freed between two reads can be missed entirely. A hit is evidence; no hits
means not seen, which is the same asymmetry the watchpoints have.

Windows are the low one (0x1000..0x04000000) and the contiguous one
(0x80000000..0x84000000), which is where physical RAM is mapped.
"""

import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

# The helper is guest-regs.py, whose name is not importable as a module, so
# it is loaded by path the same way guest-stack.py loads it.
import importlib.util
_spec = importlib.util.spec_from_file_location(
    "guest_regs", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "guest-regs.py"))
guest_regs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guest_regs)

WINDOWS = ((0x00001000, 0x04000000), (0x80000000, 0x84000000))
CHUNK = 1 << 20




def main():
    argv = sys.argv[1:]
    limit = 10
    if "--max" in argv:
        k = argv.index("--max")
        limit = int(argv[k + 1])
        del argv[k:k + 2]
    as_hex = False
    if "--hex" in argv:
        as_hex = True
        argv.remove("--hex")
    if not argv:
        raise SystemExit(__doc__)

    needle = bytes.fromhex(argv[0]) if as_hex else argv[0].encode("latin-1")

    import glob
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    maps = sorted(glob.glob(os.path.join(root, "build", "*", "defjam_recomp.map")),
                  key=os.path.getmtime, reverse=True)
    if not maps:
        raise SystemExit("no linker map; build first")
    _off, _tls, funcs = guest_regs.parse_map(maps[0])
    offset_va = next((va for va, n in funcs if n == "g_xbox_mem_offset"), None)
    if offset_va is None:
        raise SystemExit("g_xbox_mem_offset is not in the map")

    pid = guest_regs.find_pid()
    if not pid:
        raise SystemExit("defjam_recomp.exe is not running")

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    h = k32.OpenProcess(0x10 | 0x400, False, pid)   # VM_READ | QUERY_INFORMATION
    if not h:
        raise SystemExit("could not open pid %d" % pid)

    offset = guest_regs.u64(guest_regs.read(h, offset_va, 8))
    print("pid %d  guest offset 0x%X  searching for %r" % (pid, offset, needle))

    hits = 0
    for lo, hi in WINDOWS:
        va = lo
        while va < hi and hits < limit:
            size = min(CHUNK, hi - va)
            data = guest_regs.read(h, va + offset, size)
            if data:
                start = 0
                while hits < limit:
                    i = data.find(needle, start)
                    if i < 0:
                        break
                    start = i + 1
                    ctx = data[i:i + 24]
                    text = "".join(chr(b) if 0x20 <= b < 0x7F else "."
                                   for b in ctx)
                    print("  guest 0x%08X  %s" % (va + i, text))
                    hits += 1
            va += size
    if not hits:
        print("  not found in either window")


if __name__ == "__main__":
    main()
