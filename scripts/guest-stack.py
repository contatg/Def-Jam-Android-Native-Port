#!/usr/bin/env python3
"""Walk a thread's guest call stack, using the lifted C as the ground truth.

The crash handler and the watchdog both print a "guest stack", and both are
scanning rather than walking: they list every word on the stack that looks like
a code address, including stale ones from calls that returned long ago. That
suggests callers who were never on the path, and it has already cost this
project two wrong diagnoses.

The scan can be made exact. Every call the recompiler emits looks like

    PUSH32(esp, 0x0019B12Eu); RECOMP_ABI_CALL(0x00197650u, sub_00197650);

so the generated C contains the complete set of addresses that can legitimately
be on a guest stack as a return address, and says which function each one is in
and what it was calling. A stack word that is not in that set is not a return
address. A word that is in it is a real call site, and the ones that are still
live appear in increasing stack order with the innermost first.

    python scripts/guest-stack.py                  every thread running guest code
    python scripts/guest-stack.py --tid 1234       one thread
    python scripts/guest-stack.py --depth 400      how many stack words to scan
    python scripts/guest-stack.py --log logs/run-X.log.err
                                                   the scans RECOMP_WATCH_EXEC_STACK printed

Stale entries are still possible -- a word left by a returned call is
indistinguishable from a live one -- so this narrows the answer rather than
proving it. It is a far better starting point than the raw scan, and the
target name on each line usually makes the real chain obvious.
"""
import argparse, ctypes, json, os, re, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util

_spec = importlib.util.spec_from_file_location(
    "guest_regs", os.path.join(os.path.dirname(os.path.abspath(__file__)), "guest-regs.py"))
guest_regs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guest_regs)

k32 = guest_regs.k32
ntdll = guest_regs.ntdll

CALL_RE = re.compile(
    r"PUSH32\(esp, 0x([0-9A-Fa-f]{8})u\);\s*RECOMP_(?:ABI_CALL\(0x([0-9A-Fa-f]{8})u|ICALL_SAFE)")
FUNC_RE = re.compile(r"^void (sub_[0-9A-Fa-f]{8})(?:_gen)?\(void\)")


def build_call_sites(gen_dir, cache):
    """{return address: (containing function, call target or None)}.

    Cached because it is a scan of 52 MB of generated C, and the answer only
    changes when the image is re-lifted."""
    newest = max((os.path.getmtime(os.path.join(gen_dir, f))
                  for f in os.listdir(gen_dir) if f.endswith(".c")), default=0)
    if os.path.exists(cache) and os.path.getmtime(cache) >= newest:
        with open(cache) as f:
            return {int(k): tuple(v) for k, v in json.load(f).items()}

    sites = {}
    for name in sorted(os.listdir(gen_dir)):
        if not name.endswith(".c"):
            continue
        current = "?"
        with open(os.path.join(gen_dir, name), errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    current = m.group(1)
                    continue
                m = CALL_RE.search(line)
                if m:
                    ret = int(m.group(1), 16)
                    target = m.group(2)
                    sites[ret] = (current, "sub_" + target.upper() if target else "(indirect)")
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, "w") as f:
        json.dump({str(k): list(v) for k, v in sites.items()}, f)
    return sites


STACK_WORD_RE = re.compile(r"^\s+GS \+(\d+)\s+([0-9A-Fa-f]{8})\s*$")


def filter_log(path, sites):
    """The same filter, applied to the scans RECOMP_WATCH_EXEC_STACK prints.

    A live read only sees where a thread is now, and the interesting stack is
    often a moment that has passed -- a burst during loading that is over by
    the time anyone looks. The breakpoint prints the raw words at the instant
    of the call; this keeps the ones that are real call sites."""
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("[WATCH] entry"):
                print(line.rstrip()[:120])
                continue
            m = STACK_WORD_RE.match(line)
            if m:
                word = int(m.group(2), 16)
                if word in sites:
                    fn, target = sites[word]
                    print(f"    [esp+{m.group(1):<5}] 0x{word:08X}  in {fn} -> {target}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default="win-x64-debug")
    ap.add_argument("--tid", type=int, help="one thread instead of all of them")
    ap.add_argument("--depth", type=int, default=256, help="stack words to scan")
    ap.add_argument("--samples", type=int, default=1)
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--log", help="filter the stack scans an entry breakpoint printed "
                                  "(RECOMP_WATCH_EXEC_STACK) instead of reading a live process")
    a = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if a.log:
        sites = build_call_sites(os.path.join(root, "src", "recomp", "gen"),
                                 os.path.join(root, "build", a.preset, "call-sites.json"))
        return filter_log(a.log, sites)
    mappath = os.path.join(root, "build", a.preset, "defjam_recomp.map")
    if not os.path.exists(mappath):
        print(f"no linker map at {mappath}")
        return 1

    sites = build_call_sites(os.path.join(root, "src", "recomp", "gen"),
                             os.path.join(root, "build", a.preset, "call-sites.json"))
    print(f"{len(sites)} call sites in the lifted image")

    offsets, tls_index_va, funcs = guest_regs.parse_map(mappath)
    offset_va = next((va for va, n in funcs if n == "g_xbox_mem_offset"), None)
    if not offsets or tls_index_va is None or offset_va is None:
        print("the map is missing the guest registers, the TLS slot or the memory offset")
        return 1

    pid = guest_regs.find_pid()
    if not pid:
        print("defjam_recomp.exe is not running")
        return 1
    h = k32.OpenProcess(0x10 | 0x400, False, pid)
    if not h:
        print("could not open the process")
        return 1

    tls_index = guest_regs.u32(guest_regs.read(h, offset_va, 4))
    mem_offset = guest_regs.u64(guest_regs.read(h, offset_va, 8))
    tls_index = guest_regs.u32(guest_regs.read(h, tls_index_va, 4))
    if mem_offset is None or tls_index is None:
        print("could not read the memory offset or the TLS slot")
        return 1
    print(f"pid {pid}  guest offset 0x{mem_offset:X}")

    tids = [a.tid] if a.tid else guest_regs.threads_of(pid)
    for _ in range(a.samples):
        for tid in tids:
            th = k32.OpenThread(0x40 | 0x8, False, tid)
            if not th:
                continue
            try:
                tbi = guest_regs.THREAD_BASIC_INFORMATION()
                if ntdll.NtQueryInformationThread(th, 0, ctypes.byref(tbi),
                                                  ctypes.sizeof(tbi), None) != 0:
                    continue
                teb = int(tbi.TebBaseAddress or 0)
                if not teb:
                    continue
                tls_ptr = guest_regs.u64(guest_regs.read(h, teb + 0x58, 8))
                if not tls_ptr:
                    continue
                slot = guest_regs.u64(guest_regs.read(h, tls_ptr + tls_index * 8, 8))
                if not slot:
                    continue
                esp = guest_regs.u32(guest_regs.read(h, slot + offsets["esp"], 4))
                if not esp or esp < 0x1000:
                    continue

                data = guest_regs.read(h, esp + mem_offset, a.depth * 4)
                if not data:
                    continue
                hits = []
                for i in range(len(data) // 4):
                    word = int.from_bytes(data[i * 4:i * 4 + 4], "little")
                    if word in sites:
                        hits.append((i * 4, word, sites[word]))
                if not hits:
                    continue
                print(f"\n  tid {tid}  guest esp 0x{esp:08X}  "
                      f"{len(hits)} return address(es):")
                for depth, (off, word, (fn, target)) in enumerate(hits[:24]):
                    print(f"    [esp+{off:<5}] 0x{word:08X}  in {fn} -> {target}")
            finally:
                k32.CloseHandle(th)
        if a.samples > 1:
            time.sleep(a.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
