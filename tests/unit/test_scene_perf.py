"""scripts/scene-perf.py: the scene window, the stop controller, pacing numbers, the summary."""
import csv
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


perf = load('scene-perf')
bb = load('bug-bundle')

COLUMNS = ['frame', 't_ms', 'interval_ms', 'present_ms', 'work_ms', 'capture_ms', 'draws', 'vertices', 'clears',
           'audio_dropped', 'audio_dry', 'step', 'log_bytes', 'pcm_bytes', 'tex_uploads', 'tex_upload_kb',
           'tex_evictions', 'tex_cached', 'tex_cache_mb', 'hd_cache_mb']


def fake_session(tmp_path, intervals, play_frame, end_frame, log_lines):
    """A session folder, its log and a runs.json entry: frame i lasts intervals[i]; the log's
    lines are (frame, text), written during that frame."""
    folder = tmp_path / 'logs' / 'sessions' / 's1'
    folder.mkdir(parents=True)
    log = tmp_path / 'logs' / 'run-1.log.err'
    data, offsets = b'', {}
    for frame, text in log_lines:
        data += text.encode() + b'\n'
        offsets[frame] = len(data)
    log.write_bytes(data)
    t, written = 0.0, 0
    with open(folder / 'frames.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(COLUMNS)
        for i, iv in enumerate(intervals):
            t += iv
            written = max([written] + [b for fr, b in offsets.items() if fr <= i])
            w.writerow([i, '%.3f' % t, '%.3f' % iv, '2.000', '%.3f' % max(0.0, iv - 2.0), '0.000', 100, 1000, 1,
                        0, 0, -1, written, -1, 0, '0.0', 0, 10, '5.00', '0.00'])
    times = {}
    acc = 0.0
    for i, iv in enumerate(intervals):
        acc += iv
        times[i] = acc
    events = [{'t_ms': times[play_frame - 3], 'frame': play_frame - 3, 'kind': 'cinema', 'scene': 'sc01'},
              {'t_ms': times[play_frame], 'frame': play_frame, 'kind': 'cinema', 'play': 'sc01'},
              {'t_ms': times[play_frame + 20], 'frame': play_frame + 20, 'kind': 'audio_dry', 'count': 1},
              {'t_ms': times[end_frame], 'frame': end_frame, 'kind': 'cinema', 'ended': 'sc01'},
              {'t_ms': times[end_frame + 1], 'frame': end_frame + 1, 'kind': 'devmod', 'method': 'Unload'}]
    (folder / 'events.jsonl').write_text('\n'.join(json.dumps(e) for e in events) + '\n')
    (folder / 'session.json').write_text(json.dumps({'log': str(log), 'build': 'test', 'platform': 'test',
                                                     'settings': {'display.aspect': '16:9'}}))
    return {'scene': 'sc01', 'profile': 'stress', 'log': 'logs/run-1.log.err', 'session': 'logs/sessions/s1',
            'played': True, 'ended': True, 'stop': 'ended'}


def test_measure_reads_only_the_scene_and_drops_capture_frames(tmp_path, monkeypatch):
    monkeypatch.setattr(perf, 'REPO', tmp_path)
    intervals = [16.67] * 400
    intervals[50] = 900.0          # the load: the frame `play` is logged on
    intervals[10] = 300.0          # before the scene: not counted
    intervals[150] = 120.0         # a hitch inside the scene
    intervals[200] = 33.4          # one missed vblank
    intervals[250] = 200.0         # a harness capture: left out
    intervals[380] = 500.0         # after the end: not counted
    lines = [(49, '[SCENE] playing sc01 (scene 1) in the game\'s cinema mode'),
             (100, '[FILE] D:\\sc01_02.viv -> not opened (game/sc01_02.viv: No such file or directory)'),
             (250, '[TRANS] captured the back buffer to x.bmp'),
             (300, '[SCENE] sc01 ended'),
             (390, '[CRASH] after the end')]
    entry = fake_session(tmp_path, intervals, 50, 300, lines)
    m = perf.measure(entry, bb, audio=False)
    assert m['load_ms'] == 900.0
    assert m['worst_ms'] == 120.0
    assert m['hitches'] == 1 and m['hitch_sides'] == {'game': 1}
    assert m['late_frames'] == 2 and m['missed_vblanks'] == 7     # 120 ms is six missed, 33.4 one
    assert m['shot_frames'] == 3
    assert m['audio_dry'] == 1
    assert m['returned_to_menu']
    assert m['missing_files_in_scene'] == ['D:\\sc01_02.viv']
    assert m['log_faults'] == {'crash': {'run': 1, 'scene': 0}}
    assert abs(m['median_ms'] - 16.67) < 0.01


def test_summary_and_compare_write_tables(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(perf, 'REPO', tmp_path)
    entry = fake_session(tmp_path, [16.67] * 400, 50, 300, [(49, '[SCENE] playing sc01 (scene 1)'),
                                                           (300, '[SCENE] sc01 ended')])
    run = tmp_path / 'logs' / 'scene-perf' / 'a'
    run.mkdir(parents=True)
    (run / 'runs.json').write_text(json.dumps([entry, {'scene': 'sc02', 'played': False, 'ended': False,
                                                       'session': None, 'log': 'logs/none'}]))
    assert perf.summarize(run, audio=False) == 1          # sc02 never played
    text = (run / 'summary.md').read_text()
    assert '| sc01 | yes |' in text and '| sc02 | **no recorder session** |' in text
    assert perf.compare(run, run) == 0
    assert '| sc01 |' in capsys.readouterr().out
    base = tmp_path / 'base.json'
    assert perf.baseline(run, base) == 0
    kept = json.loads(base.read_text())['scenes'][0]
    assert 'missing_files_in_scene' not in kept and kept['median_ms'] > 0
    assert perf.compare(base, run) == 0


def test_stop_controller_waits_for_the_end_then_stops(tmp_path):
    log = tmp_path / 'run.log.err'
    log.write_bytes(b'[SCENE] playing sc03 (scene 3)\n')
    tail = type('Tail', (), {'path': str(log)})()
    stop = perf.SceneStop('sc03', after=6.0, start_secs=100.0)
    assert not stop.poll(tail, 1.0)
    log.write_bytes(b'[SCENE] playing sc03 (scene 3)\n[SCENE] sc03 ended after 900 frames\n')
    assert not stop.poll(tail, 2.0)
    assert stop.reason == 'ended'
    assert not stop.poll(tail, 7.0)
    assert stop.poll(tail, 8.1)


def test_stop_controller_gives_up_when_nothing_starts(tmp_path):
    log = tmp_path / 'run.log.err'
    log.write_bytes(b'[D3D] 2.0s: 120 present\n')
    tail = type('Tail', (), {'path': str(log)})()
    stop = perf.SceneStop('sc03', start_secs=30.0)
    assert not stop.poll(tail, 10.0)
    assert stop.poll(tail, 31.0) and stop.reason == 'never started'


def test_profiles_are_environment_only():
    assert perf.PROFILES['stress'] == {'RECOMP_ASPECT': '16:9', 'RECOMP_RENDER_SCALE': '3',
                                       'RECOMP_PRESENT_VSYNC': '1'}
    assert perf.PROFILES['deck'] == {'RECOMP_ASPECT': '16:10', 'RECOMP_RENDER_SCALE': '2',
                                     'RECOMP_PRESENT_VSYNC': '1'}
    assert perf.PROFILES['default'] == {}
