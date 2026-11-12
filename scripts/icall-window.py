#!/usr/bin/env python3
"""Read the running game's last indirect-call targets, and name them.

The runtime keeps a sixteen-entry ring of every indirect call the lifted code
dispatches (g_icall_trace in xbox_memory_layout.c, written by the RECOMP_ICALL
macro). That ring is the cheapest view there is of what a title is doing at the
game level: direct calls are compiled away into ordinary C calls and are
invisible, but a title's per-frame work goes through vtables and function
tables, so it lands here.

Sampling it repeatedly turns it into a profile. A title that is working shows a
changing set of targets; a title that is parked shows the same handful forever,
and those are the functions to read.

    python scripts/icall-window.py --samples 10 --interval 0.5

Addresses are guest ones. Resolve a name with the linker map the same way
scripts/sample-threads.py does, or just read sub_<address> in src/recomp/gen.
"""
import argparse, collections, ctypes, ctypes.wintypes as wt, re, subprocess, sys, time

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


def find_pid(name="defjam_recomp.exe"):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/NH", "/FO", "CSV"],
                         capture_output=True, text=True).stdout
    m = re.search(r'"[^"]+","(\d+)"', out)
    if not m:
        sys.exit(f"{name} is not running")
    return int(m.group(1))


def map_symbol(map_path, symbol):
    """The symbol's load address, straight out of the /MAP file.

    The image is linked /DYNAMICBASE:NO, so the address in the map is the
    address in the process and no rebasing is needed."""
    with open(map_path, errors="ignore") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[1] == symbol:
                return int(parts[2], 16)
    sys.exit(f"{symbol} is not in {map_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preset", default="win-x64-debug")
    ap.add_argument("--samples", type=int, default=10)
    ap.add_argument("--interval", type=float, default=0.5)
    args = ap.parse_args()

    map_path = f"build/{args.preset}/defjam_recomp.map"
    ring = map_symbol(map_path, "g_icall_trace")
    idx = map_symbol(map_path, "g_icall_trace_idx")

    pid = find_pid()
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        sys.exit(f"could not open pid {pid} (error {ctypes.get_last_error()})")
    print(f"pid {pid}  ring 0x{ring:X}")

    def read(addr, n):
        buf = (ctypes.c_ubyte * n)()
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return None
        return bytes(buf)

    seen = collections.Counter()
    order = []
    for _ in range(args.samples):
        data = read(ring, 16 * 4)
        pos = read(idx, 4)
        if data is None:
            break
        targets = [int.from_bytes(data[i * 4:i * 4 + 4], "little") for i in range(16)]
        cur = int.from_bytes(pos, "little") if pos else 0
        for t in targets:
            if t:
                seen[t] += 1
        order.append(cur)
        time.sleep(args.interval)

    print(f"ring index moved: {order[0]} -> {order[-1]}"
          if order else "no samples")
    print(f"{len(seen)} distinct targets across {args.samples} samples:")
    for t, n in seen.most_common(24):
        print(f"  sub_{t:08X}  seen in {n} sample(s)")


if __name__ == "__main__":
    main()
