#!/usr/bin/env python3
"""Native (host) call stacks of every thread in the running port.

scripts/sample-threads.py reports where each thread's instruction pointer is,
which says "outside the exe" for any thread blocked in the system or a driver
-- exactly the threads a hang is usually about. This walks each thread's host
stack with dbghelp's StackWalk64 and names every frame from the build's PDB,
so a thread waiting inside d3d11.dll still shows which of our functions
called it and from where.

    python scripts/native-stacks.py                 # every thread, 24 frames
    python scripts/native-stacks.py --grep nv2a     # only threads whose stack mentions nv2a
    python scripts/native-stacks.py --depth 40
    python scripts/native-stacks.py --pid 1234      # a given process (default: the newest game)

Each thread is suspended for the few milliseconds its walk takes and resumed.
Reads only. No debugger needed on Windows: dbghelp.dll ships with it. On macOS
the stacks come from lldb (Xcode's command-line tools; macOS may ask once to
allow developer-tool access), on Linux from gdb or elfutils' eu-stack (ptrace:
as root, or with kernel.yama.ptrace_scope at 0). Names come from the build's
own symbols there.
"""

import ctypes
import glob
import os
import shutil
import signal
import subprocess
import sys

if os.name == "nt":
    import ctypes.wintypes as wt
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    dbg = ctypes.WinDLL("dbghelp", use_last_error=True)

TH32CS_SNAPTHREAD = 0x4
PROCESS_ALL = 0x001F0FFF
THREAD_ALL = 0x001F03FF
CONTEXT_FULL = 0x0010000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664


if os.name == "nt":
    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                    ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                    ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                    ("dwFlags", wt.DWORD)]


def find_pid(name="defjam_recomp.exe"):
    if os.name != "nt":
        # The newest process whose name is exactly the game's.
        out = subprocess.run(["pgrep", "-n", "-x", "defjam_recomp"], capture_output=True, text=True).stdout
        return int(out.split()[0]) if out.split() else None
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + name, "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == name:
            return int(parts[1])
    return None


def threads_of(pid):
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    tids = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            tids.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return tids


def debugger_command(pid, depth):
    """(command, its standard input) that prints every thread's stack of `pid` on this host, or None."""
    if sys.platform == "darwin":
        if shutil.which("lldb"):
            # On-demand symbols: indexing the generated code's debug info first takes minutes. The
            # commands go in on stdin, not --batch -o: the game traps memory accesses on purpose
            # (EXC_BAD_ACCESS), batch mode takes the one it attaches into for a crash and stops there.
            return (["lldb", "-O", "settings set symbols.load-on-demand true", "-p", str(pid)],
                    "thread backtrace all -c %d\nprocess detach\nquit\n" % depth)
        return None
    if shutil.which("gdb"):
        return (["gdb", "-p", str(pid), "-batch", "-nx", "-ex", "set pagination off",
                 "-ex", "thread apply all bt %d" % depth], None)
    if shutil.which("eu-stack"):
        return (["eu-stack", "-p", str(pid), "-n", str(depth)], None)
    return None


def split_threads(text):
    """The debugger's output cut into one block a thread (lldb `thread #`, gdb `Thread `, eu-stack `TID `)."""
    blocks, cur = [], None
    # lldb echoes its commands; only what `thread backtrace` printed counts (attaching prints a stop too).
    if "(lldb) thread backtrace" in text:
        text = text.split("(lldb) thread backtrace", 1)[1].split("\n", 1)[-1].split("(lldb) ", 1)[0]
    for line in text.splitlines():
        head = line.lstrip("* ").startswith(("thread #", "Thread ", "TID "))
        if head:
            cur = [line]
            blocks.append(cur)
        elif cur is not None and line.strip():
            cur.append(line)
    return ["\n".join(b) for b in blocks]


def posix_stacks(pid, depth, grep):
    found = debugger_command(pid, depth)
    if not found:
        raise SystemExit("no debugger found: install Xcode's command-line tools (lldb) on macOS, "
                         "gdb or elfutils (eu-stack) on Linux")
    try:
        cmd, stdin = found
        r = subprocess.run(cmd, input=stdin, capture_output=True, text=True, errors="replace", timeout=60)
    except subprocess.TimeoutExpired:
        # A debugger killed while attached can leave the game stopped: let it run on.
        os.kill(pid, signal.SIGCONT)
        if sys.platform == "darwin" and shutil.which("sample"):
            print("lldb did not answer in 60 s; macOS `sample` instead (a call tree per thread):")
            r = subprocess.run(["sample", str(pid), "1", "-mayDie"], capture_output=True, text=True,
                               errors="replace", timeout=120)
            sys.stdout.write(r.stdout)
            return
        raise SystemExit("%s did not answer in 60 s" % found[0][0])
    blocks = split_threads(r.stdout)
    if not blocks:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("%s printed no thread stacks for pid %d (exit %d)" % (cmd[0], pid, r.returncode))
    for b in blocks:
        if grep and grep not in b.lower():
            continue
        print(b)


def main():
    argv = sys.argv[1:]
    depth, grep, pid = 24, None, None
    if "--depth" in argv:
        i = argv.index("--depth"); depth = int(argv[i + 1]); del argv[i:i + 2]
    if "--grep" in argv:
        i = argv.index("--grep"); grep = argv[i + 1].lower(); del argv[i:i + 2]
    if "--pid" in argv:
        i = argv.index("--pid"); pid = int(argv[i + 1]); del argv[i:i + 2]

    pid = pid or find_pid()
    if not pid:
        raise SystemExit("defjam_recomp is not running")
    if os.name != "nt":
        posix_stacks(pid, depth, grep)
        return
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    builds = sorted(glob.glob(os.path.join(root, "build", "*")), key=os.path.getmtime, reverse=True)
    search = ";".join(builds).encode()

    k32.OpenProcess.restype = wt.HANDLE
    k32.OpenThread.restype = wt.HANDLE
    hp = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not hp:
        raise SystemExit("could not open pid %d" % pid)
    dbg.SymSetOptions(0x2 | 0x10 | 0x200)          # UNDNAME | LOAD_LINES | DEFERRED_LOADS
    if not dbg.SymInitialize(wt.HANDLE(hp), search, True):
        raise SystemExit("SymInitialize failed (%d)" % ctypes.get_last_error())
    dbg.SymFunctionTableAccess64.restype = ctypes.c_void_p
    dbg.SymGetModuleBase64.restype = ctypes.c_uint64
    fta = ctypes.CFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_uint64)(
        lambda h, a: dbg.SymFunctionTableAccess64(wt.HANDLE(h), ctypes.c_uint64(a)))
    gmb = ctypes.CFUNCTYPE(ctypes.c_uint64, wt.HANDLE, ctypes.c_uint64)(
        lambda h, a: dbg.SymGetModuleBase64(wt.HANDLE(h), ctypes.c_uint64(a)))
    dbg.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p]

    sym = ctypes.create_string_buffer(88 + 512)
    def name_of(addr):
        ctypes.memset(sym, 0, len(sym))
        ctypes.c_uint32.from_buffer(sym, 0).value = 88       # SizeOfStruct
        ctypes.c_uint32.from_buffer(sym, 80).value = 500      # MaxNameLen
        disp = ctypes.c_uint64(0)
        if dbg.SymFromAddr(wt.HANDLE(hp), ctypes.c_uint64(addr), ctypes.byref(disp), sym):
            n = ctypes.c_uint32.from_buffer(sym, 76).value
            return "%s+0x%X" % (sym.raw[84:84 + n].decode(errors="replace"), disp.value)
        return "0x%X" % addr

    # CONTEXT (x64) must be 16-byte aligned; STACKFRAME64 generously sized.
    ctx_raw = ctypes.create_string_buffer(1232 + 16)
    base = (ctypes.addressof(ctx_raw) + 15) & ~15
    for tid in threads_of(pid):
        ht = k32.OpenThread(THREAD_ALL, False, tid)
        if not ht:
            continue
        frames = []
        if k32.SuspendThread(wt.HANDLE(ht)) != 0xFFFFFFFF:
            try:
                ctypes.memset(base, 0, 1232)
                ctypes.c_uint32.from_address(base + 0x30).value = CONTEXT_FULL
                if k32.GetThreadContext(wt.HANDLE(ht), ctypes.c_void_p(base)):
                    rip = ctypes.c_uint64.from_address(base + 0xF8).value
                    rsp = ctypes.c_uint64.from_address(base + 0x98).value
                    rbp = ctypes.c_uint64.from_address(base + 0xA0).value
                    sf = ctypes.create_string_buffer(1024)
                    for off, val in ((0, rip), (32, rbp), (48, rsp)):
                        ctypes.c_uint64.from_buffer(sf, off).value = val
                        ctypes.c_uint32.from_buffer(sf, off + 12).value = 3   # AddrModeFlat
                    for _ in range(depth):
                        if not dbg.StackWalk64(IMAGE_FILE_MACHINE_AMD64, hp, ht, sf,
                                               ctypes.c_void_p(base), None, fta, gmb, None):
                            break
                        pc = ctypes.c_uint64.from_buffer(sf, 0).value
                        if not pc:
                            break
                        frames.append(name_of(pc))
            finally:
                k32.ResumeThread(wt.HANDLE(ht))
        k32.CloseHandle(ht)
        if grep and not any(grep in f.lower() for f in frames):
            continue
        print("tid %d:" % tid)
        for f in frames:
            print("    " + f)
    dbg.SymCleanup(wt.HANDLE(hp))


if __name__ == "__main__":
    main()
