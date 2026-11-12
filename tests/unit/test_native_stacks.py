"""scripts/native-stacks.py off Windows: the debugger's output cut into one block a thread."""
import importlib.util
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('native_stacks', ROOT / 'scripts/native-stacks.py')
ns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ns)

LLDB = """(lldb) process attach --pid 4242
Process 4242 stopped
* thread #1, queue = 'com.apple.main-thread', stop reason = signal SIGSTOP
    frame #0: 0x0000000185339308 libsystem_kernel.dylib`__semwait_signal + 8
->  0x185339308 <+8>:  b.lo   0x185339328               ; <+40>
Target 0: (defjam_recomp) stopped.
(lldb) thread backtrace all -c 5
* thread #1, queue = 'com.apple.main-thread', stop reason = signal SIGSTOP
  * frame #0: 0x0000000185339308 libsystem_kernel.dylib`__semwait_signal + 8
    frame #1: 0x0000000102588444 defjam_recomp`d3d8_present + 52
  thread #2
    frame #0: 0x0000000185339308 libsystem_kernel.dylib`__psynch_cvwait + 8
    frame #1: 0x0000000102588464 defjam_recomp`nv2a_executor_wait + 20
(lldb) process detach
Process 4242 detached
"""

GDB = """[New LWP 101]
0x00007f in poll () from /usr/lib/libc.so.6

Thread 2 (Thread 0x7f12 (LWP 101) "defjam_recomp"):
#0  0x00007f in futex_wait () from /usr/lib/libc.so.6
#1  0x0040 in nv2a_executor_wait () at src/nv2a/exec.c:10

Thread 1 (Thread 0x7f11 (LWP 100) "defjam_recomp"):
#0  0x00007f in poll () from /usr/lib/libc.so.6
#1  0x0041 in d3d8_present () at src/d3d/present.c:20
[Inferior 1 (process 100) detached]
"""


def test_lldb_keeps_only_the_backtraces():
    blocks = ns.split_threads(LLDB)
    assert len(blocks) == 2
    assert 'd3d8_present' in blocks[0] and '->' not in blocks[0]
    assert blocks[1].strip().startswith('thread #2') and 'nv2a_executor_wait' in blocks[1]
    assert not any('detached' in b for b in blocks)


def test_gdb_threads():
    blocks = ns.split_threads(GDB)
    assert [b.split('(')[0].strip() for b in blocks] == ['Thread 2', 'Thread 1']
    assert 'nv2a_executor_wait' in blocks[0] and 'd3d8_present' in blocks[1]


@pytest.mark.skipif(sys.platform == 'win32', reason='the debugger commands are the macOS and Linux path')
def test_debugger_command_attaches_by_pid(monkeypatch):
    monkeypatch.setattr(ns.shutil, 'which', lambda name: '/usr/bin/' + name)
    cmd, stdin = ns.debugger_command(4242, 12)
    assert '4242' in cmd
    assert any('12' in part for part in cmd + [stdin or ''])
    if sys.platform == 'darwin':
        assert '--batch' not in cmd and 'process detach' in stdin   # batch mode stops at the game's own traps
