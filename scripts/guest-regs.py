#!/usr/bin/env python3
"""Read a running thread's guest register file.

The generated code keeps the guest registers thread-local, so a thread's eax or ebx is not a global you
can read and not something a host context exposes. That matters as soon as a thread is spinning on an
address, because the address is in a guest register and nothing else in the process knows it.

Windows stores each thread's thread-local block behind its TEB, so the value is reachable from outside:
read ThreadLocalStoragePointer out of the target thread's TEB, index it by the image's TLS slot, and add
the offset the linker map already records for each register. The map gives that offset directly, as the
section-relative part of a `.tls` symbol, which is what a TLS offset is.

    python scripts/guest-regs.py                 every thread that is executing guest code
    python scripts/guest-regs.py --all           every thread, including ones parked in a wait

Only valid because the game links /DYNAMICBASE:NO, so map addresses are the addresses in memory.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, subprocess, sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
TH32CS_SNAPTHREAD = 0x4
REGS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG),
                ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class THREAD_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("ExitStatus", ctypes.c_long), ("TebBaseAddress", ctypes.c_void_p),
                ("UniqueProcessId", ctypes.c_void_p), ("UniqueThreadId", ctypes.c_void_p),
                ("AffinityMask", ctypes.c_void_p), ("Priority", ctypes.c_long),
                ("BasePriority", ctypes.c_long)]


def find_pid(name="defjam_recomp.exe"):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = [x.strip('"') for x in line.split('","')]
        if len(p) > 1 and p[0].lower() == name:
            return int(p[1])
    return None


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(THREADENTRY32)
    out = []
    if k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                out.append(te.th32ThreadID)
            if not k32.Thread32Next(snap, ctypes.byref(te)):
                break
    k32.CloseHandle(snap)
    return out


def read(h, addr, size):
    buf = (ctypes.c_ubyte * size)(); got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return bytes(buf[:got.value])


def u32(b): return int.from_bytes(b, "little") if b else None
def u64(b): return int.from_bytes(b, "little") if b else None


def parse_map(path):
    """TLS-relative offsets for the guest registers, plus _tls_index's address."""
    tls_section = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s+([0-9a-f]{4}):[0-9a-f]{8}\s+[0-9a-fA-F]+H\s+(\S+)", line)
        if m and m.group(2) == ".tls":
            tls_section = m.group(1)
            break
    offsets, tls_index_va, funcs = {}, None, []
    sym = re.compile(r"^\s+([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = sym.match(line)
        if not m:
            continue
        sect, off, name, va = m.group(1), int(m.group(2), 16), m.group(3), int(m.group(4), 16)
        short = name[2:] if name.startswith("g_") else name.lstrip("_")
        if sect == tls_section and short in REGS:
            offsets[short] = off
        elif name in ("_tls_index", "tls_index"):
            tls_index_va = va
        funcs.append((va, name))
    funcs.sort()
    return offsets, tls_index_va, funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="win-x64-debug")
    ap.add_argument("--all", action="store_true", help="include threads parked in a wait")
    a = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mappath = os.path.join(root, "build", a.preset, "defjam_recomp.map")
    if not os.path.exists(mappath):
        print(f"no linker map at {mappath}"); return 1
    offsets, tls_index_va, funcs = parse_map(mappath)
    if not offsets or tls_index_va is None:
        print("could not find the guest registers or the TLS slot in the map"); return 1

    pid = find_pid()
    if not pid:
        print("defjam_recomp.exe is not running"); return 1
    h = k32.OpenProcess(0x10 | 0x400, False, pid)      # VM_READ | QUERY_INFORMATION
    if not h:
        print("could not open the process"); return 1

    tls_index = u32(read(h, tls_index_va, 4))
    if tls_index is None:
        print("could not read the TLS slot index"); return 1
    print(f"pid {pid}  tls slot {tls_index}  registers at .tls+"
          + ",".join(f"{r}:0x{offsets[r]:X}" for r in REGS if r in offsets))

    import bisect
    starts = [va for va, _ in funcs]
    for tid in threads_of(pid):
        th = k32.OpenThread(0x40 | 0x8, False, tid)     # QUERY_INFORMATION | GET_CONTEXT
        if not th:
            continue
        try:
            tbi = THREAD_BASIC_INFORMATION()
            if ntdll.NtQueryInformationThread(th, 0, ctypes.byref(tbi),
                                              ctypes.sizeof(tbi), None) != 0:
                continue
            teb = int(tbi.TebBaseAddress or 0)
            if not teb:
                continue
            tls_ptr = u64(read(h, teb + 0x58, 8))       # ThreadLocalStoragePointer
            if not tls_ptr:
                continue
            block = u64(read(h, tls_ptr + tls_index * 8, 8))
            if not block:
                continue
            vals = {}
            for r in REGS:
                if r in offsets:
                    vals[r] = u32(read(h, block + offsets[r], 4))
            if not a.all and not any(vals.get(r) for r in ("esp", "ebx", "esi")):
                continue
            print(f"  tid {tid:6d}  " + "  ".join(
                f"{r}={vals[r]:08X}" for r in REGS if vals.get(r) is not None))
        finally:
            k32.CloseHandle(th)
    return 0


if __name__ == "__main__":
    sys.exit(main())
