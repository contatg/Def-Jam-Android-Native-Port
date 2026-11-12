"""scripts/hitch-probes.py: reading stalls.jsonl, naming the threads, the per-frame lines and the summary."""
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hp = load('hitch-probes')

GAME = {'name': '', 'guest': 'run', 'state_ms': 0, 'run': 'running', 'cpu_ms': 1000, 'pri': 0, 'depressed': True,
        'kernel_caller': '0x001ECF0E',
        'stack': ['libsystem_kernel.dylib`swtch_pri+0x8', 'sub_0021EB90_enter+0xb0 exe+0x57bc',
                  'sub_0021EB90+0x24 exe+0xebc1dc', 'sub_0021EDC0+0x57c exe+0xeff118']}
EXECUTOR = {'name': '', 'run': 'running', 'cpu_ms': 500, 'pri': 31,
            'stack': ['libshaderc_shared.1.dylib+0x2f82bc', 'shader_make+0x84 exe+0xf15b8',
                      'nv2a_ack_thread+0x598 exe+0xbbcf0', 'libsystem_pthread.dylib`thread_start+0x8']}
TIMER = {'name': '', 'guest': 'off', 'state_ms': 1, 'run': 'waiting', 'cpu_ms': 5, 'pri': 46,
         'stack': ['WaitForSingleObject+0x1ec exe+0xfdaec', 'kernel_timer_thread+0xbc exe+0xb55a8']}


def probe(frame, since, t_ms, gap, game_cpu, ex_cpu):
    game = dict(GAME, cpu_ms=game_cpu)
    ex = dict(EXECUTOR, cpu_ms=ex_cpu)
    return {'t_ms': t_ms, 'frame': frame, 'since_ms': since, 'probe': 1, 'cost_ms': 2.0, 'prober_gap_ms': gap,
            'holder': dict(game, holder=True),
            'all': {'exe': '0x100000000', 'sample_us': 400,
                    'gpu': {'run': '0x00012000', 'put': '0x00034000', 'backlog': 139264, 'swm_wait_ms': -1,
                            'swm_queued': 0, 'pgraph_intr': '0x0', 'servicing': 0, 'fifo': '0x1',
                            'fence': '0x535', 'latest': '0x545', 'released': '0x535', 'fence_by': 'none'},
                    'threads': [game, ex, TIMER]}}


def test_roles_and_activity():
    assert hp.role(GAME) == 'game'
    assert hp.role(EXECUTOR) == 'executor'
    assert hp.role(TIMER) == 'timer'
    assert hp.executor_activity(EXECUTOR) == 'shader compile (shaderc)'
    assert hp.name('libsystem_kernel.dylib`swtch_pri+0x8') == 'swtch_pri'
    assert hp.name('sub_0021EB90+0x24 exe+0xebc1dc') == 'sub_0021EB90'


def test_reads_an_early_line_with_an_empty_frame(tmp_path):
    good = json.dumps(probe(400, 61.0, 9000.0, 5.0, 1000, 500))
    bad = good.replace('"stack": [', '"stack": [,', 1).replace('"stack": [,', '"stack": ["0x0",,', 1)
    (tmp_path / 'stalls.jsonl').write_text(good + '\n' + bad + '\n', encoding='utf-8')
    assert len(hp.read_jsonl(tmp_path / 'stalls.jsonl')) == 2


def test_lines_and_summary(tmp_path, capsys, monkeypatch):
    s = tmp_path / 'sessions' / 'sc00'
    s.mkdir(parents=True)
    rows = [probe(400, 61.0, 9000.0, 45.0, 1000, 500), probe(400, 112.0, 9051.0, 6.0, 1003, 504)]
    (s / 'stalls.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows), encoding='utf-8')
    (s / 'events.jsonl').write_text(json.dumps({'t_ms': 9100.0, 'frame': 400, 'kind': 'hitch',
                                                'interval_ms': 140.0}) + '\n', encoding='utf-8')
    monkeypatch.setattr(sys, 'argv', ['hitch-probes.py', str(tmp_path), '--summary'])
    assert hp.main() == 0
    out = capsys.readouterr().out
    assert 'frame 400' in out and '(hitch)' in out
    assert 'sub_0021EB90_enter in sub_0021EB90' in out
    assert 'shader compile (shaderc)' in out
    assert 'cpu executor 4/51 ms, game 3/51 ms' in out
    assert 'game pri 0 depressed' in out
    assert "1  game in Direct3D's fence hold at the first probe" in out
    assert '1  the prober itself ran 20+ ms late' in out
    assert '1  game thread priority-depressed' in out
