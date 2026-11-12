"""The Mac screen helper must invalidate an executable built from older inputs."""
import importlib.util
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('build_source_state', ROOT / 'scripts/pipeline-state.py')
state = importlib.util.module_from_spec(spec)
spec.loader.exec_module(state)


def test_changed_objective_c_source_rejects_certified_build(tmp_path, monkeypatch):
    repo, toolkit = tmp_path / 'game-free-fixture', tmp_path / 'toolkit'
    helper = repo / 'src/host_macos.m'
    helper.parent.mkdir(parents=True)
    helper.write_text('int fixture_screen(void) { return 1; }')
    gen = repo / 'src/recomp/gen'
    gen.mkdir(parents=True)
    (gen / 'lift-state.json').write_text('{}')
    for name in ('CMakeLists.txt', 'CMakePresets.json'):
        (repo / name).write_text('synthetic fixture')
    toolkit.mkdir()
    (toolkit / 'CMakeLists.txt').write_text('synthetic runtime fixture')
    monkeypatch.setattr(state, 'verify_lift', lambda *args: None)
    folder = repo / 'build/posix-release'
    folder.mkdir(parents=True)
    (folder / 'CMakeCache.txt').write_text('DEFJAM_BUILD_GAME:BOOL=ON\n')
    exe = folder / state.EXE_NAME
    exe.write_bytes(b'game-free synthetic executable fixture')
    certificate = {'toolkit': str(toolkit), 'inputs': state.build_inputs(repo, toolkit),
                   'exe': state.digest(exe)}
    (folder / 'build-state.json').write_text(json.dumps(certificate))
    assert state.verify_build(repo, 'posix-release') == certificate
    helper.write_text('int fixture_screen(void) { return 2; }')
    with pytest.raises(ValueError, match='stale'):
        state.verify_build(repo, 'posix-release')
