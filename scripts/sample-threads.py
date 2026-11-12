#!/usr/bin/env python3
"""Sample where every thread of the running defjam_recomp.exe is, and name the guest function.

A hang leaves no crash record: the watchdog prints a guest stack scan, but not the guest program
counter. This suspends each thread for a few microseconds, reads its host RIP, and resolves it
against the linker map (build/<preset>/defjam_recomp.map, produced by /MAP) so the output names
the lifted function (sub_XXXXXXXX) or the runtime function the thread is inside.

Usage:  python scripts/sample-threads.py [--preset win-x64-debug] [--samples 5] [--interval 0.5]
Run it while the game is running (e.g. from a second shell after scripts/run.ps1).
"""
import argparse, bisect, ctypes, ctypes.wintypes as wt, os, re, sys, time

TH32CS_SNAPTHREAD = 0x4
THREAD_SUSPEND_RESUME = 0x2
THREAD_GET_CONTEXT = 0x8
THREAD_QUERY_INFORMATION = 0x40
CONTEXT_AMD64 = 0x100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_ulonglong), ("High", ctypes.c_longlong)]


class CONTEXT(ctypes.Structure):  # AMD64, must be 16-byte aligned
    _align_ = 16
    _fields_ = [("P1Home", ctypes.c_ulonglong), ("P2Home", ctypes.c_ulonglong), ("P3Home", ctypes.c_ulonglong),
                ("P4Home", ctypes.c_ulonglong), ("P5Home", ctypes.c_ulonglong), ("P6Home", ctypes.c_ulonglong),
                ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD),
                ("SegGs", wt.WORD), ("SegSs", wt.WORD), ("EFlags", wt.DWORD),
                ("Dr0", ctypes.c_ulonglong), ("Dr1", ctypes.c_ulonglong), ("Dr2", ctypes.c_ulonglong),
                ("Dr3", ctypes.c_ulonglong), ("Dr6", ctypes.c_ulonglong), ("Dr7", ctypes.c_ulonglong),
                ("Rax", ctypes.c_ulonglong), ("Rcx", ctypes.c_ulonglong), ("Rdx", ctypes.c_ulonglong),
                ("Rbx", ctypes.c_ulonglong), ("Rsp", ctypes.c_ulonglong), ("Rbp", ctypes.c_ulonglong),
                ("Rsi", ctypes.c_ulonglong), ("Rdi", ctypes.c_ulonglong), ("R8", ctypes.c_ulonglong),
                ("R9", ctypes.c_ulonglong), ("R10", ctypes.c_ulonglong), ("R11", ctypes.c_ulonglong),
                ("R12", ctypes.c_ulonglong), ("R13", ctypes.c_ulonglong), ("R14", ctypes.c_ulonglong),
                ("R15", ctypes.c_ulonglong), ("Rip", ctypes.c_ulonglong),
                ("FltSave", ctypes.c_byte * 512), ("VectorRegister", M128A * 26),
                ("VectorControl", ctypes.c_ulonglong), ("DebugControl", ctypes.c_ulonglong),
                ("LastBranchToRip", ctypes.c_ulonglong), ("LastBranchFromRip", ctypes.c_ulonglong),
                ("LastExceptionToRip", ctypes.c_ulonglong), ("LastExceptionFromRip", ctypes.c_ulonglong)]


def find_pid(name="defjam_recomp.exe"):
    import subprocess
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == name:
            return int(parts[1])
    return None


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(THREADENTRY32)
    tids = []
    if k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                tids.append(te.th32ThreadID)
            if not k32.Thread32Next(snap, ctypes.byref(te)):
                break
    k32.CloseHandle(snap)
    return tids


def sample(tid):
    h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, tid)
    if not h:
        return None
    try:
        if k32.SuspendThread(h) == 0xFFFFFFFF:
            return None
        try:
            ctx = CONTEXT(); ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER
            if not k32.GetThreadContext(h, ctypes.byref(ctx)):
                return None
            return (ctx.Rip, ctx.Rsp, ctx.Rax, ctx.Rcx, ctx.Rdx,
                    (ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi,
                     ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13,
                     ctx.R14, ctx.R15))
        finally:
            k32.ResumeThread(h)
    finally:
        k32.CloseHandle(h)


def module_base(pid, name="defjam_recomp.exe"):
    TH32CS_SNAPMODULE = 0x8
    class MODULEENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                    ("modBaseSize", wt.DWORD), ("hModule", ctypes.c_void_p), ("szModule", ctypes.c_char * 256),
                    ("szExePath", ctypes.c_char * 260)]
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(MODULEENTRY32)
    base, size = None, 0
    if k32.Module32First(snap, ctypes.byref(me)):
        while True:
            if me.szModule.decode(errors="replace").lower() == name:
                base, size = me.modBaseAddr, me.modBaseSize; break
            if not k32.Module32Next(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    return base, size


def read_mem(pid, addr, size):
    h = k32.OpenProcess(0x10 | 0x400, False, pid)
    if not h:
        return None
    try:
        buf = (ctypes.c_ubyte * size)()
        got = ctypes.c_size_t(0)
        k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                          ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
            return None
        return bytes(buf[:got.value])
    finally:
        k32.CloseHandle(h)


SYM = re.compile(r"^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")


def load_map(path):
    syms = []
    pref = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s+Preferred load address is ([0-9a-fA-F]{16})", line)
            if m:
                pref = int(m.group(1), 16)
            m = SYM.match(line)
            if m:
                syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    return pref, [a for a, _ in syms], [n for _, n in syms]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="win-x64-debug")
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--interval", type=float, default=0.5)
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mappath = os.path.join(root, "build", a.preset, "defjam_recomp.map")
    pid = find_pid()
    if not pid:
        print("defjam_recomp.exe is not running"); return 1
    base, size = module_base(pid)
    pref, addrs, names = load_map(mappath) if os.path.exists(mappath) else (None, [], [])
    if not addrs:
        print(f"(no linker map at {mappath}; showing raw RIPs)")
    print(f"pid {pid} base 0x{base:X} size 0x{size:X} preferred 0x{pref or 0:X} symbols {len(addrs)}")
    # Where guest memory is mapped, read out of the process so a guest pointer
    # sitting in a host register can be recognised and translated back.
    guest_base = 0
    if addrs:
        i = bisect.bisect_left(addrs, 0)
        off_va = next((a for a, n in zip(addrs, names) if n in ("g_xbox_mem_offset", "_g_xbox_mem_offset")), None)
        if off_va is not None and base is not None and pref is not None:
            raw = read_mem(pid, off_va - pref + base, 8)
            if raw:
                guest_base = int.from_bytes(raw, "little")
    guest_ptrs = {}
    hits = {}
    for s in range(a.samples):
        for tid in threads_of(pid):
            r = sample(tid)
            if not r:
                continue
            rip, rsp, rax, rcx, rdx, allregs = r
            # A host register holding a guest pointer reads as the guest VA plus
            # the mapping offset. Spotting those is how you find out which
            # structure a spinning loop is watching, which the guest register
            # file cannot tell you from outside: those are thread-local.
            if guest_base:
                for name, v in zip(("rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                                    "r8", "r9", "r10", "r11", "r12", "r13",
                                    "r14", "r15"), allregs):
                    # Skip the bare mapping base: the generated code keeps that
                    # in a register permanently, so it says nothing about what
                    # a loop is looking at.
                    if guest_base + 0x1000 <= v < guest_base + 0x08000000:
                        guest_ptrs.setdefault(tid, {}).setdefault(
                            f"{name}=0x{v - guest_base:08X}", 0)
                        guest_ptrs[tid][f"{name}=0x{v - guest_base:08X}"] += 1
            name = "?"
            if addrs and base is not None and base <= rip < base + size:
                va = rip - base + (pref or base)
                i = bisect.bisect_right(addrs, va) - 1
                if i >= 0:
                    name = f"{names[i]}+0x{va - addrs[i]:X}"
            elif base is not None and not (base <= rip < base + size):
                name = "(outside exe: kernel32/ntdll wait or syscall)"
            hits.setdefault(tid, []).append(name)
            if s == 0:
                print(f"  tid {tid:6d} rip 0x{rip:X} rsp 0x{rsp:X} rax 0x{rax:X} rcx 0x{rcx:X} rdx 0x{rdx:X}  {name}")
        time.sleep(a.interval)
    if guest_ptrs:
        print("--- guest pointers seen in host registers ---")
        for tid, d in guest_ptrs.items():
            top = sorted(d.items(), key=lambda x: -x[1])[:6]
            print(f"  tid {tid:6d}: " + ", ".join(f"{k} x{v}" for k, v in top))
    print("--- per-thread sample histogram ---")
    for tid, ns in hits.items():
        top = {}
        for n in ns:
            top[n] = top.get(n, 0) + 1
        print(f"  tid {tid:6d}: " + ", ".join(f"{n} x{c}" for n, c in sorted(top.items(), key=lambda x: -x[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
