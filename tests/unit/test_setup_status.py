"""The wizard's progress file is best effort (issue #40): a refused replace never stops setup."""
import contextlib
import os
from pathlib import Path

import pytest

from test_setup_updates import engine

ROOT = Path(__file__).resolve().parents[2]


def deny_status_replace(monkeypatch, failures):
    """Refuse os.replace onto a file named status.txt `failures` times (None: always), as Windows
    does while the wizard holds it without delete sharing. Other replaces are untouched."""
    real = os.replace
    calls = []

    def replace(src, dst, *a, **k):
        if Path(dst).name == 'status.txt':
            calls.append(Path(dst))
            if failures is None or len(calls) <= failures:
                raise PermissionError(5, 'Access is denied', str(src), None, str(dst))
        return real(src, dst, *a, **k)
    monkeypatch.setattr(engine.os, 'replace', replace)
    monkeypatch.setattr(engine.time, 'sleep', lambda seconds: None)
    return calls


def test_transient_denial_is_retried(tmp_path, monkeypatch):
    status = tmp_path / 'session' / 'status.txt'
    calls = deny_status_replace(monkeypatch, 2)
    assert engine.write_status(status, 'Downloading setup: 50 / 51 MiB') is True
    assert status.read_text(encoding='utf-8') == 'Downloading setup: 50 / 51 MiB'
    assert len(calls) == 3


def test_persistent_denial_keeps_the_previous_label(tmp_path, monkeypatch):
    status = tmp_path / 'status.txt'
    status.write_text('Downloading setup: 2 / 51 MiB', encoding='utf-8')
    calls = deny_status_replace(monkeypatch, None)
    assert engine.write_status(status, 'Downloading setup: 3 / 51 MiB') is False
    assert status.read_text(encoding='utf-8') == 'Downloading setup: 2 / 51 MiB'
    assert len(calls) == engine.STATUS_ATTEMPTS


def test_unwritable_status_folder_does_not_raise(tmp_path):
    blocker = tmp_path / 'file'
    blocker.write_text('not a folder')
    assert engine.write_status(blocker / 'status.txt', 'Preparing') is False


def selection_fixture(tmp_path, monkeypatch, download_labels, result=0):
    from test_setup_engine import installer
    obj = installer(tmp_path)
    monkeypatch.setattr(engine, 'install_lock', lambda path: contextlib.nullcontext())
    scratch = tmp_path / 'scratch'
    def select(tag, platform, cache, verify, previous, operation, cancelled, progress):
        scratch.mkdir()
        for label in download_labels:
            progress(label)
        return scratch, {'version': tag.lstrip('v')}
    monkeypatch.setattr(engine.releases, 'select_payload', select)
    monkeypatch.setattr(engine.releases, 'run_selected', lambda *a: result)
    status = tmp_path / 'session' / 'status.txt'
    log = tmp_path / 'logs' / 'session.log'
    log.parent.mkdir()      # the wizard creates it
    args = ['--payload', str(obj.payload), '--install-dir', str(obj.install), '--release', 'v0.9.1',
            '--status-file', str(status), '--log', str(log)]
    return args, status, log, scratch


def test_denied_progress_does_not_abort_release_download(tmp_path, monkeypatch):
    """The reported failure: every progress replace refused during the selected-release
    download. Before the fix main() stopped with exit 4 at the first one."""
    labels = ['Downloading setup: %d / 51 MiB' % (n // 16) for n in range(51 * 16)]
    args, status, log, scratch = selection_fixture(tmp_path, monkeypatch, labels)
    calls = deny_status_replace(monkeypatch, None)
    assert engine.main(args) == 0
    assert not scratch.exists()
    # Repeated labels (one per 64 KiB chunk) are published once each, in the log too.
    assert len(calls) == 51 * engine.STATUS_ATTEMPTS
    lines = log.read_text(encoding='utf-8').splitlines()
    assert lines == sorted(set(labels), key=labels.index)


def test_error_reporting_survives_a_denied_status_file(tmp_path, monkeypatch):
    args, status, log, scratch = selection_fixture(tmp_path, monkeypatch, ['Downloading setup: 1 / 51 MiB'])
    def fail(*a):
        raise engine.SetupError('Selected setup failed', 3)
    monkeypatch.setattr(engine.releases, 'run_selected', fail)
    deny_status_replace(monkeypatch, None)
    code = engine.main(args)
    assert code == 3
    assert 'Setup stopped: Selected setup failed' in log.read_text(encoding='utf-8')


def test_wizard_reads_status_with_delete_sharing():
    """The Windows wizard must not hold status.txt in a way that refuses the engine's replace."""
    source = (ROOT / 'setup' / 'main.cpp').read_text(encoding='utf-8')
    assert 'FILE_SHARE_DELETE' in source
    timer = source[source.index('if (message == WM_TIMER)'):]
    timer = timer[:timer.index('WaitForSingleObject')]
    assert 'readShared(statusPath' in timer and 'ifstream' not in timer


windows = pytest.mark.skipif(os.name != 'nt', reason='Windows file-sharing semantics')


def win32_reader():
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    invalid = ctypes.c_void_p(-1).value

    def open_(path, share):
        handle = kernel.CreateFileW(str(path), 0x80000000, share, None, 3, 0x80, None)  # GENERIC_READ, OPEN_EXISTING
        return None if handle in (None, invalid) else handle

    def read(handle):
        buffer = ctypes.create_string_buffer(4096)
        got = wintypes.DWORD()
        kernel.ReadFile(handle, buffer, len(buffer), ctypes.byref(got), None)
        return buffer.raw[:got.value].decode('utf-8')
    return open_, read, kernel.CloseHandle


SHARE_READ, SHARE_WRITE, SHARE_DELETE = 1, 2, 4


@windows
def test_windows_reader_without_delete_sharing_never_stops_setup(tmp_path):
    """std::ifstream's sharing (no FILE_SHARE_DELETE), as the old wizard read the file."""
    open_, read, close = win32_reader()
    status = tmp_path / 'status.txt'
    assert engine.write_status(status, 'first')
    handle = open_(status, SHARE_READ | SHARE_WRITE)
    assert handle
    try:
        assert engine.write_status(status, 'second') is False
    finally:
        close(handle)
    assert status.read_text(encoding='utf-8') == 'first'
    assert engine.write_status(status, 'third') is True


@windows
def test_windows_concurrent_wizard_reader_and_engine_writer(tmp_path):
    """The wizard's reader (setup/main.cpp readShared) polling as fast as it can while setup publishes."""
    import threading
    open_, read, close = win32_reader()
    status = tmp_path / 'status.txt'
    labels = ['Downloading setup: %d / 51 MiB' % n for n in range(300)]
    assert engine.write_status(status, labels[0])
    seen, stop = [], threading.Event()

    def poll():
        while not stop.is_set():
            handle = open_(status, SHARE_READ | SHARE_WRITE | SHARE_DELETE)
            if handle:
                try:
                    seen.append(read(handle))
                finally:
                    close(handle)
    reader = threading.Thread(target=poll)
    reader.start()
    try:
        results = [engine.write_status(status, label) for label in labels[1:]]
    finally:
        stop.set()
        reader.join()
    assert seen and set(seen) <= set(labels)      # never a torn or empty label
    assert any(results)
    assert engine.write_status(status, 'done') is True
    assert status.read_text(encoding='utf-8') == 'done'
