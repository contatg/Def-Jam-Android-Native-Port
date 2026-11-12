#!/usr/bin/env python3
"""What a stalled process's Direct3D device, GPU FIFO and tick clock look like.

    python scripts/hang-peek.py [--samples 4] [--preset win-x64-release]

Reads guest memory of the running game, several times a second apart, and prints:

  the device  MEM32(0x234768): the fence counter (+0x2C), the semaphore the GPU
              writes back (+0x30), and the words around them. A title waiting for
              a fence shows a counter ahead of the semaphore that never closes.
  the FIFO    PUT and GET (equal = the GPU has nothing to do), the push state,
              and the interrupt status registers.
  the ticks   the 120 Hz counter at 0x3C9684 that the movie player uses as its
              clock, and the object its tick thread waits on (0x3C9614). A counter
              that is the same in every sample is a stopped clock (patch 0103).

Values that change between samples are what is alive. Run it with
scripts/native-stacks.py and scripts/guest-stack.py; scripts/harness.py soak does
all three when a boot stops presenting.

The preset is taken from the running process's path unless given.
"""
import argparse
import importlib.util
import os
import struct
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("pg", os.path.join(REPO, "scripts", "peek-guest.py"))
pg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pg)


def running_preset():
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command",
                            "(Get-Process defjam_recomp -ErrorAction SilentlyContinue | Select-Object -First 1).Path"],
                           capture_output=True, text=True, timeout=30)
        path = r.stdout.strip()
        if path:
            return os.path.basename(os.path.dirname(path))
    except Exception:
        pass
    return None


def fx(v):
    return "unreadable" if v is None else "%#010x" % v


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--samples", type=int, default=4)
    ap.add_argument("--preset")
    a = ap.parse_args(argv)
    preset = a.preset or running_preset() or "win-x64-debug"
    mapfile = os.path.join(REPO, "build", preset, "defjam_recomp.map")
    pid = pg.find_pid()
    if not pid:
        sys.exit("the game is not running")
    h = pg.k32.OpenProcess(pg.PROCESS_VM_READ | pg.PROCESS_QUERY_INFORMATION, False, pid)
    off = int.from_bytes(pg.read(h, pg.map_symbol(mapfile, "g_xbox_mem_offset"), 8), "little")

    def m32(addr):
        try:
            return struct.unpack("<I", pg.read(h, addr + off, 4))[0]
        except Exception:
            return None

    print("pid %d  preset %s" % (pid, preset))
    for it in range(a.samples):
        dev = m32(0x234768)
        print("-- sample %d: device %s" % (it, fx(dev)))
        if dev:
            f = {o: m32(dev + o) for o in (0x0, 0x4, 0x8, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44)}
            print("   " + "  ".join("+%X=%s" % (o, fx(v)) for o, v in f.items()))
            if f[0x30]:
                print("   semaphore [%s] = %s   fence counter %s" % (fx(f[0x30]), fx(m32(f[0x30])), fx(f[0x2C])))
        for name, addr in (("PFIFO PUT", 0xFD003240), ("PFIFO GET", 0xFD003244), ("CACHE1 PUSH0", 0xFD003200),
                           ("DMA PUSH", 0xFD003220), ("PFIFO INTR", 0xFD002100), ("PGRAPH INTR", 0xFD400100),
                           ("PMC INTR", 0xFD000100), ("PMC INTR_EN", 0xFD000140)):
            print("   %-13s %s" % (name, fx(m32(addr))))
        print("   ticks  rate %s  count %s  count2 %s  enable %s"
              % tuple(fx(m32(x)) for x in (0x3C9680, 0x3C9684, 0x3C9688, 0x3C9624)))
        print("   tick thread's object 0x3C9614: " + " ".join(fx(m32(0x3C9614 + 4 * k)) for k in range(4)))
        if it + 1 < a.samples:
            time.sleep(1.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
