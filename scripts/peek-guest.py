#!/usr/bin/env python3
"""Read guest memory out of the running game, and find fields that change over time.

Some of the hardest bring-up questions are about a structure rather than about code: which field of the
graphics device is a counter, which one is frozen, which pointer was never written. Those are invisible in a
disassembly and awkward in a debugger, but obvious the moment you look at the same bytes twice.

    python scripts/peek-guest.py 0x00234770 0x2E00 --diff 3
        Sample that guest range three times a second apart and report every dword that changed, with how
        many times it moved and by how much. A swap throttle shows up as one field climbing steadily while
        its partner sits still.

    python scripts/peek-guest.py 0x00234770 0x80
        Dump the range once.

Guest addresses are translated with the runtime's own offset, read out of the process rather than assumed,
and the translation is checked against a known pointer before anything is reported.
"""
import argparse, ctypes, ctypes.wintypes as wt, re, subprocess, sys, time

PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def find_pid(name="defjam_recomp.exe"):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == name:
            return int(parts[1])
    return None


def read(h, addr, size):
    buf = (ctypes.c_ubyte * size)()
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return bytes(buf[:got.value])


def map_symbol(mappath, want):
    """Static address of a symbol from the linker map. Valid because the game links /DYNAMICBASE:NO."""
    sym = re.compile(r"^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")
    with open(mappath, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = sym.match(line)
            if m and m.group(1) in (want, "_" + want):
                return int(m.group(2), 16)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", help="guest VA, e.g. 0x00234770")
    ap.add_argument("size", nargs="?", default="0x100", help="bytes to read")
    ap.add_argument("--diff", type=int, default=0, metavar="N", help="sample N times, 1 s apart, report changes")
    ap.add_argument("--preset", default="win-x64-debug")
    a = ap.parse_args()
    addr, size = int(a.addr, 0), int(str(a.size), 0)

    pid = find_pid()
    if not pid:
        print("defjam_recomp.exe is not running"); return 1
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        print("could not open the process"); return 1

    import os
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mappath = os.path.join(root, "build", a.preset, "defjam_recomp.map")
    off_addr = map_symbol(mappath, "g_xbox_mem_offset") if os.path.exists(mappath) else None
    offset = None
    if off_addr:
        raw = read(h, off_addr, 8)
        if raw:
            offset = int.from_bytes(raw, "little")
    if offset is None:
        print("could not read g_xbox_mem_offset from the process; is the map current?"); return 1

    # Prove the translation before trusting anything it produces: the device pointer at 0x00234768
    # must point at 0x00234770, which is the relationship the boot log reports.
    probe = read(h, 0x00234768 + offset, 4)
    ok = probe and int.from_bytes(probe, "little") == 0x00234770
    print(f"pid {pid}  guest offset 0x{offset:X}  translation check: "
          f"{'ok' if ok else 'UNVERIFIED (device pointer not where expected)'}")

    samples = []
    for i in range(max(1, a.diff)):
        data = read(h, addr + offset, size)
        if data is None or len(data) < size:
            print(f"read failed at guest 0x{addr:08X}"); return 1
        samples.append(data)
        if a.diff and i < a.diff - 1:
            time.sleep(1.0)

    if not a.diff:
        for o in range(0, size, 16):
            words = [int.from_bytes(samples[0][o + j:o + j + 4], "little") for j in range(0, 16, 4)]
            print(f"  +0x{o:04X}  " + " ".join(f"{w:08X}" for w in words))
        return 0

    print(f"changed dwords over {a.diff} samples:")
    changed = 0
    for o in range(0, size, 4):
        series = [int.from_bytes(s[o:o + 4], "little") for s in samples]
        if len(set(series)) == 1:
            continue
        changed += 1
        deltas = [series[i + 1] - series[i] for i in range(len(series) - 1)]
        print(f"  +0x{o:04X}  " + " -> ".join(f"{v:08X}" for v in series) +
              "   step " + ",".join(str(d) for d in deltas))
    if not changed:
        print("  none: every dword in this range is frozen")
    return 0


if __name__ == "__main__":
    sys.exit(main())
