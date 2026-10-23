"""Behavioral negative controls for reports, captures, fixtures and ownership."""
import importlib.util
import json
import re
from pathlib import Path
import struct
import xml.etree.ElementTree as ET
import wave
import pytest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ev, suite, harness, regress = [load(n) for n in ('test_evidence', 'scenario_suite', 'harness', 'regress')]


def bmp(path, pixels):
    width, height = len(pixels), 1
    rgb = bytes(c for pixel in pixels for c in pixel)
    bgr = bytearray(len(rgb))
    bgr[0::3], bgr[1::3], bgr[2::3] = rgb[2::3], rgb[1::3], rgb[0::3]
    bgr += bytes((-len(bgr)) % 4)
    header = b'BM' + struct.pack('<IHHI', 54 + len(bgr), 0, 0, 54)
    header += struct.pack('<IiiHHIIIIII', 40, width, height, 1, 24, 0, len(bgr), 0, 0, 0, 0)
    path.write_bytes(header + bgr)


def test_visual_rejects_wrong_image_with_same_nonblack_coverage(tmp_path):
    a, b = tmp_path / 'a.bmp', tmp_path / 'b.bmp'
    bmp(a, [(255, 0, 0), (0, 255, 0)])
    bmp(b, [(0, 255, 0), (255, 0, 0)])
    assert ev.visual_check(a, a, tmp_path / 'same.png', harness)['status'] == 'pass'
    check = ev.visual_check(a, b, tmp_path / 'diff.png', harness)
    assert check['status'] == 'fail' and check['metrics']['changed_fraction'] == 1
    assert (tmp_path / 'diff.png').read_bytes().startswith(b'\x89PNG')


def test_visual_masks_cannot_hide_entire_image(tmp_path):
    p = tmp_path / 'a.bmp'
    bmp(p, [(255, 0, 0)])
    with pytest.raises(ValueError, match='every pixel'):
        ev.visual_check(p, p, tmp_path / 'diff.png', harness, masks=[[0, 0, 1, 1]])


def pcm_checks(tmp_path, value, **kwargs):
    p = tmp_path / 'audio.pcm'
    p.write_bytes(struct.pack('<hh', value, value) * 20)
    text = '[FUNCCALL] Game.StartGame()\n[APU] output peak 1 over the last second; buffers dropped 0, device ran dry 0'
    return ev.audio_checks(p, text, sample_rate=10, min_seconds=1, **kwargs)


def test_injected_venue_request_replaces_menu_choice_before_start():
    text = ('[FUNCCALL] Game.ChooseVenue(14)\n'
            '[FE-INJECT] 1 rule(s) armed: Game.ChooseVenue(99)\n'
            '[FE-INJECT] after "game.choosevenue" #1: Game.ChooseVenue(6)\n'
            '[FUNCCALL] Game.StartGame()\n'
            '[FUNCCALL] Game.ResetMatchData()\n'
            '[FUNCCALL] Game.StartGame()\n')
    assert [c['venue'] for c in ev.fight_contexts(text)] == [6, None]


def test_audio_healthy_silent_and_clipped_controls(tmp_path):
    assert all(c['status'] == 'pass' for c in pcm_checks(tmp_path, 200))
    assert next(c for c in pcm_checks(tmp_path, 0, max_silent_seconds=1) if c['name'] == 'audio.silence')['status'] == 'fail'
    assert next(c for c in pcm_checks(tmp_path, 32767) if c['name'] == 'audio.clipping')['status'] == 'fail'


def test_audio_missing_queue_and_malformed_pcm_fail(tmp_path):
    p = tmp_path / 'bad.pcm'
    p.write_bytes(b'12345')
    checks = ev.audio_checks(p, '', sample_rate=10, min_seconds=1)
    assert next(c for c in checks if c['name'] == 'audio.queue_evidence')['status'] == 'fail'
    assert next(c for c in checks if c['name'] == 'audio.pcm_alignment')['status'] == 'fail'


def test_audio_startup_queue_faults_do_not_count_as_fight_faults(tmp_path):
    p = tmp_path / 'a.pcm'
    p.write_bytes(struct.pack('<hh', 200, 200) * 20)
    line = '[APU] output peak 200 over the last second; buffers dropped 0, device ran dry '
    text = line + '10\n[FUNCCALL] Game.StartGame()\n' + line + '0'
    assert all(c['status'] == 'pass' for c in ev.audio_checks(p, text, sample_rate=10, min_seconds=1))
    assert next(c for c in ev.audio_checks(p, text + '\n' + line + '1', sample_rate=10, min_seconds=1)
                if c['name'] == 'audio.dry_queue')['status'] == 'fail'


def test_pcm_boundaries_exclude_clipped_startup_and_result_audio(tmp_path):
    p = tmp_path / 'a.pcm'
    p.write_bytes(struct.pack('<hh', 32767, 32767) * 20 + struct.pack('<hh', 200, 200) * 20
                  + struct.pack('<hh', 32767, 32767) * 20)
    lines = '[FUNCCALL] Game.StartGame()\n[APU] output peak 200 over the last second; buffers dropped 0, device ran dry 0\n'
    lines += '[FUNCCALL] Control.GetScreenInfo(battle/matchSm1.swf)\n[APU] output peak 32767 over the last second; buffers dropped 9, device ran dry 9'
    bounds = [{'call': 'Game.StartGame()', 'pcm_bytes': 80, 'observed_seconds': 2},
              {'call': 'Control.GetScreenInfo(battle/matchSm1.swf)', 'pcm_bytes': 160, 'observed_seconds': 4}]
    window = ev.pcm_window(bounds, 'game.startgame(', 'getscreeninfo(battle/matchsm')
    assert window['start_byte'] == 80 and window['end_byte'] == 160
    checks = ev.audio_checks(p, lines, sample_rate=10, min_seconds=1, start_byte=80, end_byte=160,
                             end_anchor='getscreeninfo(battle/matchsm')
    assert all(c['status'] == 'pass' for c in checks)
    assert next(c for c in ev.audio_checks(p, lines, sample_rate=10, min_seconds=1) if c['name'] == 'audio.clipping')['status'] == 'fail'
    assert ev.pcm_window(bounds, 'absent') is None
    assert ev.pcm_window(bounds, 'game.startgame(')['end_byte'] is None


@pytest.mark.parametrize('start,end', [(1, 4), (-4, 4), (8, 4), (0, 1000)])
def test_pcm_bounds_reject_invalid_ranges(tmp_path, start, end):
    p = tmp_path / 'a.pcm'
    p.write_bytes(bytes(80))
    assert ev.audio_checks(p, '', start_byte=start, end_byte=end)[-1]['status'] == 'fail'


def test_audio_preview_preserves_evaluated_samples_and_portable_html(tmp_path):
    p, output = tmp_path / 'a.pcm', tmp_path / 'preview.wav'
    expected = struct.pack('<hh', 200, -300) * 10
    p.write_bytes(bytes(40) + expected + bytes(40))
    preview = ev.audio_preview(p, output, start_byte=40, end_byte=80, sample_rate=10)
    assert preview['seconds'] == 1
    with wave.open(str(output), 'rb') as f:
        assert f.getframerate() == 10 and f.getnchannels() == 2 and f.getsampwidth() == 2
        assert f.readframes(100) == expected
    ev.write_report(tmp_path, {'name': 'audio', 'assertions': [ev.assertion('synthetic', True, 'test')],
                               'audio': [str(output)]})
    page = (tmp_path / 'index.html').read_text()
    assert 'src="preview.wav"' in page and 'href="report.json"' in page


def test_fight_requests_are_scoped_to_each_match():
    text = '''[FUNCCALL] Game.ResetMatchData()
[FUNCCALL] Game.SetMatchType(0)
[FUNCCALL] Controller.ControllerSetup(-1,0,0,0)
[FUNCCALL] Controller.ControllerSetup(0,0,56,0)
[FUNCCALL] Controller.ControllerSetup(1,-1,55,1)
[FUNCCALL] Game.ChooseVenue(6)
[FUNCCALL] Game.StartGame()
[FUNCCALL] Game.ResetMatchData()
[FUNCCALL] Game.StartGame()'''
    contexts = ev.fight_contexts(text)
    assert contexts[0]['venue'] == 6 and contexts[0]['slots']['0']['fighter'] == 56
    assert len(contexts[0]['slots']) == 2
    assert contexts[1]['venue'] is None and contexts[1]['slots'] == {}


def test_fixture_is_copied_and_tampering_rejected(tmp_path):
    source = tmp_path / 'source'
    source.mkdir()
    (source / 'profile').write_bytes(b'profile')
    output = tmp_path / 'fixture'
    identity = suite.create_fixture(source, output, 'VY3: tutorial complete')
    assert identity['rng_status'] == 'not controlled'
    assert (source / 'profile').read_bytes() == b'profile'
    (output / 'save/profile').write_bytes(b'changed')
    with pytest.raises(ValueError, match='differ'):
        ev.fixture_identity(output)


def test_fixture_output_inside_source_is_rejected(tmp_path):
    (tmp_path / 'profile').write_bytes(b'profile')
    with pytest.raises(ValueError, match='inside'):
        suite.create_fixture(tmp_path, tmp_path / 'nested', 'profile')


def test_scenario_settings_are_explicit_and_reject_ambiguous_values(tmp_path):
    settings, env = suite.scenario_settings()
    assert settings == {'render_scale': 2, 'gamma': True, 'vsync': True,
                        'brightness': 100, 'vignette': True, 'vignette_strength': 100}
    assert env['RECOMP_RENDER_SCALE'] == '2' and env['RECOMP_GAMMA'] == '1'
    assert env['RECOMP_BRIGHTNESS'] == '100' and env['RECOMP_VIGNETTE'] == '1' and env['RECOMP_VIGNETTE_STRENGTH'] == '100'
    p = tmp_path / 'settings.json'
    p.write_text(json.dumps({'render_scale': 3, 'gamma': False}))
    settings, env = suite.scenario_settings(p)
    assert env['RECOMP_RENDER_SCALE'] == '3' and env['RECOMP_GAMMA'] == '0'
    p.write_text(json.dumps({'brightness': 150, 'vignette': False, 'vignette_strength': 40}))
    settings, env = suite.scenario_settings(p)
    assert env['RECOMP_BRIGHTNESS'] == '150' and env['RECOMP_VIGNETTE'] == '0' and env['RECOMP_VIGNETTE_STRENGTH'] == '40'
    for bad in ({'brightness': 49}, {'brightness': 201}, {'vignette_strength': 101}, {'brightness': '120'}):
        p.write_text(json.dumps(bad))
        with pytest.raises(ValueError, match='integer'):
            suite.scenario_settings(p)
    p.write_text(json.dumps({'gamma': 'false'}))
    with pytest.raises(ValueError, match='booleans'):
        suite.scenario_settings(p)
    p.write_text(json.dumps({'game_folder': 'unexpected'}))
    with pytest.raises(ValueError, match='unknown'):
        suite.scenario_settings(p)


def test_missing_telemetry_is_not_a_green_skip(tmp_path):
    checks = ev.combat_checks('[PAD] script: x sampled')
    assert checks[0]['status'] == 'blocked'
    assert not ev.write_report(tmp_path, {'name': '<script>alert(1)</script>', 'assertions': checks})
    xml = ET.parse(tmp_path / 'junit.xml').getroot()
    assert xml.get('errors') == '1' and xml.find('.//skipped') is None
    assert '<script>alert(1)</script>' not in (tmp_path / 'index.html').read_text()


def played_fight(match=1):
    """The event shapes a live One on One produced (values abbreviated)."""
    roster = [dict(slot=0, character=4, cpu=0, health=265.6), dict(slot=1, character=9, cpu=1, health=278.4)]
    done = [dict(roster[0], health=0), dict(roster[1], health=30.9)]
    return [dict(event='rng', match=match, step=0, seed=1, increment=3, forced=False),
            dict(event='match_start', match=match, step=1, match_type=2, fighters=roster),
            dict(event='movement', match=match, step=121, slot=0, distance=2.1, held=24),
            dict(event='attack', match=match, step=130, slot=0, target_slot=1, pressed=32, steps_since_press=4),
            dict(event='damage', match=match, step=131, slot=1, cpu=1, health_before=278.4, health_after=277.4),
            dict(event='result', match=match, step=17320, code=33, decisive=True, draw=False, time_up=False,
                 winners=[1], losers=[0], clock=[5, 46], fighters=done),
            dict(event='match_over', match=match, step=17321, winner_slot=1, phase=3, fighters=done)]


def fight_log(events, summary=True):
    lines = ['[TEST-EVENT] ' + json.dumps(e) for e in events]
    return '\n'.join(lines + (['[FUNCCALL] Game.GetMatchSummary(0)'] if summary else []))


def statuses(checks):
    return {c['name']: c['status'] for c in checks}


def test_combat_passes_on_the_shape_of_a_real_fight():
    checks = ev.combat_checks(fight_log(played_fight()))
    assert set(statuses(checks).values()) == {'pass'}
    assert [c['name'] for c in checks] == ['combat.telemetry', 'combat.setup', 'combat.movement', 'combat.attack',
                                           'combat.damage', 'combat.result', 'combat.results_screen']
    result = next(c for c in checks if c['name'] == 'combat.result')
    assert result['metrics']['winner_is_human'] is False and result['metrics']['loser_health'] == [0]


@pytest.mark.parametrize('name,change', [
    ('combat.setup', lambda e: e[1]['fighters'].pop()),                       # a fighter record missing
    ('combat.setup', lambda e: [f.update(cpu=1) for f in e[1]['fighters']]),  # nobody human
    ('combat.movement', lambda e: e.pop(2)),
    ('combat.movement', lambda e: e[2].update(slot=1)),                       # the CPU walked, not the player
    ('combat.attack', lambda e: e[3].update(steps_since_press=600)),          # a hit long after any press
    ('combat.attack', lambda e: e[3].update(slot=1)),
    ('combat.damage', lambda e: e[4].update(health_after=278.4)),             # no health lost
    ('combat.damage', lambda e: e[4].update(slot=0)),                         # somebody else was hurt
    ('combat.damage', lambda e: e[4].update(step=400)),                       # long after the hit
    ('combat.result', lambda e: e[5].update(draw=True)),
    ('combat.result', lambda e: e[5].update(decisive=False)),
    ('combat.result', lambda e: e[5].update(winners=[])),
    ('combat.result', lambda e: e[5].update(winners=[1], losers=[1])),
    ('combat.result', lambda e: e[6].update(winner_slot=0)),                  # the game's two records disagree
    ('combat.result', lambda e: e.pop(6)),                                    # never reached the decided phase
    ('combat.result', lambda e: e.insert(6, dict(e[5]))),                     # two decisive results
    ('combat.telemetry', lambda e: e[4].update(step=5)),                      # steps going backwards
])
def test_combat_fails_when_the_fight_did_not_happen_as_claimed(name, change):
    events = played_fight()
    change(events)
    assert statuses(ev.combat_checks(fight_log(events)))[name] == 'fail'


def test_combat_results_screen_must_follow_the_result():
    events = played_fight()
    assert statuses(ev.combat_checks(fight_log(events, summary=False)))['combat.results_screen'] == 'fail'
    early = '[FUNCCALL] Game.GetMatchSummary(0)\n' + fight_log(events, summary=False)
    assert statuses(ev.combat_checks(early))['combat.results_screen'] == 'fail'


def test_combat_does_not_stitch_two_matches_together():
    first, second = played_fight(1), played_fight(2)
    del first[3:]                                   # the first match has no attack, damage or result
    log = fight_log(first + second)
    assert statuses(ev.combat_checks(log))['combat.attack'] == 'fail'
    assert statuses(ev.combat_checks(log))['combat.result'] == 'fail'
    assert set(statuses(ev.combat_checks(log, match=2)).values()) == {'pass'}


def test_combat_can_stop_before_the_result_and_counts_malformed_lines():
    events = played_fight()[:5]
    checks = ev.combat_checks(fight_log(events, summary=False), require_result=False)
    assert set(statuses(checks).values()) == {'pass'} and len(checks) == 5
    broken = fight_log(events, summary=False) + '\n[TEST-EVENT] {"event":"damage","match":1,"st'
    assert statuses(ev.combat_checks(broken, require_result=False))['combat.telemetry'] == 'fail'
    assert statuses(ev.combat_checks(fight_log(played_fight()), fighters=4))['combat.setup'] == 'fail'


def test_combat_bootstrap_closes_static_setup_before_live_input():
    stages = suite.bootstrap_stages('fight-terrordome')
    assert stages[-1] == 'game.startgame(=back@9999'
    assert stages[:-1] == harness.ROUTES['fight-terrordome']['stages']


def test_capture_labels_cannot_overwrite_checkpoints_across_anchors():
    harness.validate_shots('@fight,15,60,@results,4,@menu#2,5')
    with pytest.raises(ValueError, match='overwrite'):
        harness.validate_shots('@fight,4,@results,4.0')
    with pytest.raises(ValueError, match='limit'):
        harness.validate_shots(','.join(str(i) for i in range(17)))


def test_cleanup_only_owns_this_harness_children(monkeypatch):
    class Process:
        dead = False
        def poll(self): return 0 if self.dead else None
        def kill(self): self.dead = True
        def wait(self, **kwargs): assert self.dead
    owned, other = Process(), Process()
    monkeypatch.setattr(harness, '_OWNED_PROCESSES', {owned})
    harness.kill_stray()
    assert owned.dead and not other.dead and not harness._OWNED_PROCESSES


def test_shared_story_uses_one_launch_and_fails_missing_gym(tmp_path, monkeypatch):
    calls = []
    run = regress.harness.Run('story-tour', 'unused', '')
    run.save_ok = True
    run.text = '[FUNCCALL] Control.GetScreenInfo(story/crib)'
    run.summary = regress.harness.summarise(run.text)
    def launch(*args, **kwargs): calls.append(args); return run
    monkeypatch.setattr(regress.harness, 'run_route', launch)
    monkeypatch.setattr(regress, 'REPO', str(tmp_path))
    assert regress.main(['--only', 'crib,gym', '--shared-story']) == 1
    assert len(calls) == 1 and calls[0] == ('story-tour',)
    report = next((tmp_path / 'logs').glob('regress-*/report.json'))
    checks = json.loads(report.read_text())['assertions']
    assert checks[0]['status'] == 'pass' and checks[1]['status'] == 'fail'


def test_full_regression_has_the_terrordome_match_and_the_combat_check():
    # v0.2.1 fixed the venue crash: the four-fighter match is in the full run,
    # the One on One there stays selectable only, and neither is in the quick run.
    assert 'ffa-terrordome' in regress.DEFAULT and 'combat' in regress.DEFAULT
    assert 'fight-terrordome' in regress.CHECKS and 'fight-terrordome' not in regress.DEFAULT
    assert 'repeat' in regress.CHECKS and 'repeat' not in regress.DEFAULT
    assert 'replay' in regress.DEFAULT and 'replay' not in regress.QUICK
    assert 'visual' in regress.CHECKS and 'visual' not in regress.DEFAULT
    assert 'versus' in regress.DEFAULT and 'ffa-result' in regress.CHECKS and 'ffa-result' not in regress.DEFAULT
    assert 'two-matches' in regress.CHECKS and 'two-matches' not in regress.DEFAULT
    assert not {'ffa-terrordome', 'fight-terrordome', 'combat'} & set(regress.QUICK)
    assert set(regress.ORDER) == set(regress.CHECKS)
    assert harness.ROUTES['story-tour']['until'] == harness.ROUTES['gym']['until']


def test_session_repeated_menu_needs_new_event_and_commands(tmp_path):
    driver_mod = load('session_driver')
    plan = {'input_clock': 'host-seconds', 'phases': [
        {'name': 'menu-before', 'anchor': 'menu', 'commands': [{'button': 'a', 'at': 1}]},
        {'name': 'fight', 'anchor': 'startgame', 'commands': []},
        {'name': 'menu-after', 'anchor': 'menu', 'after': 2, 'commands': []}]}
    driver = driver_mod.SessionDriver(plan, tmp_path / 'pad.txt')
    tail = type('Tail', (), {'calls': ['menu']})()
    assert not driver.poll(tail, 0)
    assert not driver.poll(tail, 1)
    assert (tmp_path / 'pad.txt').read_text() == 'a:300\n'
    assert not driver.poll(tail, 2)
    assert len(driver.observed) == 1
    tail.calls.append('startgame')
    driver.poll(tail, 3)
    assert not driver.poll(tail, 10)  # old menu cannot satisfy the return
    tail.calls.append('menu')
    assert not driver.poll(tail, 11)
    assert driver.poll(tail, 13)
    assert all(c['status'] == 'pass' for c in driver.assertions())


@pytest.mark.parametrize('clock', ['simulation-frames', None])
def test_session_refuses_unimplemented_clock(tmp_path, clock):
    with pytest.raises(ValueError, match='simulation-frame'):
        load('session_driver').SessionDriver({'input_clock': clock, 'phases': []}, tmp_path / 'pad.txt')


def test_session_repeats_have_release_gaps_and_preserve_recipe(tmp_path):
    driver_mod = load('session_driver')
    plan = {'input_clock': 'host-seconds', 'phases': [
        {'name': 'fight', 'anchor': 'startgame', 'after': 1,
         'commands': [{'button': 'x', 'at': 1, 'repeat': {'count': 3, 'interval': 1}}]}]}
    original = json.dumps(plan)
    driver = driver_mod.SessionDriver(plan, tmp_path / 'pad.txt')
    assert json.dumps(plan) == original
    tail = type('Tail', (), {'calls': ['startgame']})()
    for t in range(4):
        assert not driver.poll(tail, t)
    assert driver.poll(tail, 4)
    assert (tmp_path / 'pad.txt').read_text() == 'x:300\nx:300\nx:300\n'
    plan['phases'][0]['commands'][0]['repeat']['interval'] = .3
    with pytest.raises(ValueError, match='release gap'):
        driver_mod.SessionDriver(plan, tmp_path / 'invalid.txt')


def test_visual_manifest_requires_hash_locked_approval(tmp_path):
    base, shot = tmp_path / 'base.bmp', tmp_path / 'shot.bmp'
    bmp(base, [(200, 0, 0)])
    bmp(shot, [(200, 0, 0)])
    manifest = {'routes': {'fight': [{'capture': 'shot.bmp', 'baseline': 'base.bmp',
                                    'approved': True, 'sha256': ev.digest(base)}]}}
    p = tmp_path / 'manifest.json'
    p.write_text(json.dumps(manifest))
    run = type('Run', (), {'shots_dir': str(tmp_path)})()
    assert suite.visual_assertions(p, 'fight', run, tmp_path)[0]['status'] == 'pass'
    bmp(base, [(0, 200, 0)])
    assert suite.visual_assertions(p, 'fight', run, tmp_path)[0]['status'] == 'fail'


def test_baseline_candidates_preserve_failed_source_without_approving(tmp_path):
    report_dir = tmp_path / 'report'
    (report_dir / 'captures').mkdir(parents=True)
    bmp(report_dir / 'captures/shot.bmp', [(200, 0, 0)])
    report = report_dir / 'report.json'
    report.write_text(json.dumps({'name': 'fight', 'identity': {'fixture': 'synthetic'},
                                  'assertions': [ev.assertion('runtime', False, 'synthetic failure')]}))
    output = tmp_path / 'review'
    manifest_path = suite.baseline_candidates(report, output)
    manifest = json.loads(manifest_path.read_text())
    assert manifest['source_assertions'][0]['status'] == 'fail'
    item = manifest['routes']['fight'][0]
    assert item['approved'] is False and item['sha256'] == ev.digest(output / 'shot.bmp')
    run = type('Run', (), {'shots_dir': str(report_dir / 'captures')})()
    assert suite.visual_assertions(manifest_path, 'fight', run, tmp_path)[0]['status'] == 'fail'
    with pytest.raises(FileExistsError):
        suite.baseline_candidates(report, output)


def test_missing_build_writes_failed_report_before_copying_fixture(tmp_path, monkeypatch):
    src = tmp_path / 'source'
    src.mkdir()
    (src / 'profile').write_bytes(b'synthetic')
    fixture = tmp_path / 'fixture'
    suite.create_fixture(src, fixture, 'synthetic profile')
    def missing_build(*args): raise ValueError('synthetic missing build certificate')
    monkeypatch.setattr(suite.harness.pipeline_state, 'verify_build', missing_build)
    monkeypatch.setattr(suite, 'source_identity', lambda _: {})
    output = tmp_path / 'report'
    assert suite.main(['run', 'fight-terrordome', '--fixture', str(fixture), '--output', str(output)]) == 1
    report = json.loads((output / 'report.json').read_text())
    assert report['assertions'][0]['status'] == 'fail'
    assert 'synthetic missing build' in report['assertions'][0]['detail']
    assert not list(output.glob('runtime-*'))


@pytest.mark.parametrize('sample,expected_exit', [(200, 0), (0, 1)])
@pytest.mark.parametrize('packs', [None, 'synthetic-hd'])
def test_scenario_runner_requires_audio_and_retains_browsable_evidence(tmp_path, monkeypatch, sample, expected_exit, packs):
    """Exercise the report path with synthetic launch evidence, including a
    silent-audio negative control despite otherwise healthy fight evidence."""
    src = tmp_path / 'source'
    src.mkdir()
    (src / 'profile').write_bytes(b'synthetic')
    fixture = tmp_path / 'fixture'
    suite.create_fixture(src, fixture, 'synthetic')
    monkeypatch.setattr(suite, 'source_identity', lambda _: {'head': 'synthetic'})
    monkeypatch.setattr(suite.harness.pipeline_state, 'verify_build', lambda *a: {})
    received = {}
    monkeypatch.setenv('RECOMP_TEXTURE_PACKS', 'inherited-must-not-enter-scenario')
    def launch(route, **kwargs):
        received.update(kwargs)
        text = ('[FUNCCALL] Game.ResetMatchData()\n[FUNCCALL] Controller.ControllerSetup(0,0,1,0)\n'
                '[FUNCCALL] Controller.ControllerSetup(1,-1,2,1)\n[FUNCCALL] Game.ChooseVenue(6)\n'
                '[FUNCCALL] Game.StartGame()\n')
        text += ('[D3D] 2.0s: 120 present\n[APU] output peak 200 over the last second; buffers dropped 0, device ran dry 0\n'
                 '[PACING] 120 presents, interval 16.40 / 16.67 / 17.02 ms (min/mean/max), median 16.67, 0 late, sync interval 2\n') * 30
        log = tmp_path / 'game.log.err'
        log.write_text(text)
        shots = tmp_path / 'shots'
        shots.mkdir()
        bmp(shots / 'fight-terrordome-15s.bmp', [(200, 0, 0)])
        Path(kwargs['env']['RECOMP_APU_PCM']).write_bytes(struct.pack('<hh', sample, sample) * (48000 * 30))
        run = suite.harness.Run(route, str(log), str(shots))
        run.text, run.summary, run.save_ok = text, suite.harness.summarise(text), True
        run.pcm_boundaries = [{'call': 'game.startgame()', 'pcm_bytes': 0, 'observed_seconds': 0}]
        run.timeline = [{'call': 'game.startgame()', 'observed_seconds': 0}]
        run.memory = [{'seconds': 20 + 5 * i, 'private_bytes': 300 << 20, 'working_set': 200 << 20} for i in range(10)]
        run.identity = {'environment': {k:v for k,v in kwargs['env'].items() if k.startswith('RECOMP_')}}
        return run
    monkeypatch.setattr(suite.harness, 'run_route', launch)
    monkeypatch.setattr(suite.evidence, 'environment_identity', lambda: {'os': 'synthetic'})
    output = tmp_path / 'report'
    arguments=['run', 'fight-terrordome', '--fixture', str(fixture), '--output', str(output)]
    if packs:
        arguments += ['--texture-packs', packs, '--texture-root', str(tmp_path/'mods')]
    assert suite.main(arguments) == expected_exit
    assert received['inherit_recomp'] is False
    assert received['env']['RECOMP_RENDER_SCALE'] == '2'
    report = json.loads((output / 'report.json').read_text())
    recorded=report['identity']['run']['environment']
    if packs:
        assert recorded['RECOMP_TEXTURE_PACKS']==packs
        assert recorded['RECOMP_TEXTURE_ROOT']==str((tmp_path/'mods').resolve())
    else:
        assert 'RECOMP_TEXTURE_PACKS' not in recorded and 'RECOMP_TEXTURE_ROOT' not in recorded
    assert report['scope']['audio_health_required'] is True and report['scope']['full_gameplay_acceptance'] is False
    assert report['captures'][0]['sha256'] == ev.digest(output / 'captures/fight-terrordome-15s.bmp')
    assert (output / 'audio-preview.wav').is_file()
    assert not list(output.glob('runtime-*'))
    silence = next(c for c in report['assertions'] if c['name'] == 'audio.silence')
    assert silence['status'] == ('pass' if sample else 'fail')


def pacing_log(windows, late=0, median=16.67, worst=17.0, presents=120):
    line = ('[PACING] %d presents, interval 16.40 / 16.67 / %.2f ms (min/mean/max), median %.2f, %d late, sync interval 2'
            % (presents, worst, median, late))
    return '[FUNCCALL] Game.StartGame()\n' + '\n'.join([line] * windows)


def test_pacing_checks_judge_only_full_rate_windows_after_the_anchor():
    assert {c['status'] for c in ev.pacing_checks(pacing_log(30))} == {'pass'}
    # Too little evidence is a failure, not a pass.
    assert ev.pacing_checks(pacing_log(5))[0]['status'] == 'fail'
    assert ev.pacing_checks('[PACING] nothing here')[0]['status'] == 'fail'
    # A menu's 30 fps windows are not judged, so thirty of them are no evidence either.
    assert ev.pacing_checks(pacing_log(30, presents=60))[0]['status'] == 'fail'
    # Wrong cadence and too many late presents each fail their own assertion.
    slow = {c['name']: c['status'] for c in ev.pacing_checks(pacing_log(30, median=33.3))}
    assert slow['performance.frame_interval'] == 'fail'
    late = {c['name']: c['status'] for c in ev.pacing_checks(pacing_log(30, late=6))}
    assert late['performance.late_presents'] == 'fail' and late['performance.frame_interval'] == 'pass'
    # Reports before the anchor are ignored.
    before = pacing_log(30, median=33.3).replace('[FUNCCALL] Game.StartGame()\n', '') + '\n' + pacing_log(30)
    assert {c['status'] for c in ev.pacing_checks(before)} == {'pass'}


def test_memory_checks_measure_growth_during_play():
    def samples(values, first=30):
        return [{'seconds': first + 5 * i, 'private_bytes': int(v * 1048576), 'working_set': 0} for i, v in enumerate(values)]
    steady = ev.memory_checks(samples([300, 320, 318, 319, 321, 320, 322]), start_seconds=30)
    assert steady[0]['status'] == 'pass' and steady[0]['metrics']['growth_mb'] == 2.0
    leak = ev.memory_checks(samples([300, 320, 340, 380, 420, 460, 500]), start_seconds=30)
    assert leak[0]['status'] == 'fail' and leak[0]['metrics']['growth_mb'] == 160.0
    # Loading before play does not count, and too few samples is not a pass.
    loading = ev.memory_checks(samples([50, 100, 200, 300, 320, 318, 319, 321, 320, 322], first=0), start_seconds=15)
    assert loading[0]['status'] == 'pass'
    assert ev.memory_checks(samples([300, 320]), start_seconds=30)[0]['status'] == 'fail'


def test_default_fixture_copies_only_profiles_and_leaves_the_source_alone(tmp_path, monkeypatch):
    data = tmp_path / 'data'
    (data / 'save/UserData/45410049/AAAA').mkdir(parents=True)
    (data / 'save/UserData/45410049/AAAA/profile').write_bytes(b'profile')
    (data / 'save/Cache').mkdir()
    (data / 'save/Cache/big').write_bytes(b'x' * 4096)
    (data / 'save/Partition1.img').write_bytes(b'y' * 4096)
    before = {p.relative_to(data).as_posix(): p.read_bytes() for p in data.rglob('*') if p.is_file()}
    monkeypatch.setattr(suite.harness, 'REPO', str(tmp_path / 'repo'))
    (tmp_path / 'repo/logs/fixtures').mkdir(parents=True)
    fixture = suite.default_fixture(data)
    copied = sorted(p.relative_to(fixture / 'save').as_posix() for p in (fixture / 'save').rglob('*') if p.is_file())
    assert copied == ['UserData/45410049/AAAA/profile']
    assert {p.relative_to(data).as_posix(): p.read_bytes() for p in data.rglob('*') if p.is_file()} == before
    assert not list((tmp_path / 'repo/logs/fixtures').glob('fixture-source-*'))
    with pytest.raises(ValueError, match='no saved profiles'):
        suite.default_fixture(tmp_path / 'empty')


def stream_log(health=(100, 90), steps=(1, 31, 61), match=1, extra=''):
    lines = ['[TEST-EVENT] ' + json.dumps(dict(event='match_start', match=match, step=1, host_ms=5, fighters=[]))]
    for index, step in enumerate(steps):
        lines.append('[TEST-STATE] ' + json.dumps(dict(event='sample', match=match, step=step, host_ms=1000 + index,
                                                       rng=7, fighters=[dict(slot=0, health=health[0]), dict(slot=1, health=health[1])])))
    return '\n'.join(lines) + extra


def test_state_stream_ignores_the_host_clock_and_stops_at_the_step_limit():
    one = ev.state_stream(stream_log())
    other = ev.state_stream(stream_log().replace('"host_ms": 1001', '"host_ms": 999999'))
    assert one == other and len(one) == 4 and all('host_ms' not in line for line in one)
    assert len(ev.state_stream(stream_log(), max_step=31)) == 3
    # Another match in the same log, and a line cut short by a crash, are left out.
    mixed = stream_log() + '\n' + stream_log(match=2, health=(5, 5)) + '\n[TEST-STATE] {"event":"sample","match":1,"st'
    assert ev.state_stream(mixed) == one
    assert ev.state_stream(mixed, match=2) != one


def test_stream_checks_name_the_step_where_two_runs_part():
    recorded = ev.state_stream(stream_log())
    assert ev.stream_checks(stream_log(), recorded)[0]['status'] == 'pass'
    changed = ev.stream_checks(stream_log(health=(100, 89)), recorded)[0]
    assert changed['status'] == 'fail' and changed['metrics']['step'] == 1
    assert changed['metrics']['differing_fields'] == ['fighters']
    later = stream_log().replace('"step": 61, "host_ms": 1002, "rng": 7', '"step": 61, "host_ms": 1002, "rng": 8')
    parted = ev.stream_checks(later, recorded)[0]
    assert parted['status'] == 'fail' and parted['metrics']['step'] == 61 and parted['metrics']['differing_fields'] == ['rng']
    # A run that ended early agrees as far as it goes but is not a pass; no telemetry is blocked.
    assert ev.stream_checks(stream_log(steps=(1, 31)), recorded)[0]['status'] == 'fail'
    assert ev.stream_checks('nothing', recorded)[0]['status'] == 'blocked'
    assert ev.stream_checks(stream_log(), [])[0]['status'] == 'fail'


def test_stream_options_need_step_input_and_a_seed(tmp_path):
    with pytest.raises(SystemExit):
        suite.main(['run', 'fight', '--fixture', str(tmp_path), '--stream-out', str(tmp_path / 's.txt')])
    with pytest.raises(SystemExit):
        suite.main(['run', 'fight', '--fixture', str(tmp_path), '--stream-expect', str(tmp_path / 's.txt'),
                    '--step-input', 'default'])


def test_generated_step_input_is_well_formed(tmp_path):
    path = suite.step_input(tmp_path / 'input.txt', seconds=60)
    rows = [line.split() for line in path.read_text().splitlines() if not line.startswith('#')]
    assert rows and all(len(r) == 4 and r[0] == '0' and 600 <= int(r[1]) <= int(r[2]) < 3600 + 80 for r in rows)
    assert {r[3] for r in rows} == {'8', '10', '20', '40', '80'}
    # No two face buttons are down on the same step.
    held = {}
    for slot, first, last, buttons in rows:
        if buttons != '8':
            for step in range(int(first), int(last) + 1):
                assert held.setdefault(step, buttons) == buttons


def test_fight_stream_golden_is_a_hash_and_matches_the_repeat_check():
    golden = json.loads((ROOT / 'tests/golden/fight-stream.json').read_text(encoding='utf-8'))
    assert re.fullmatch(r'[0-9a-f]{64}', golden['sha256']) and golden['records'] > 0
    # The parameters the hash depends on are the ones the repeat check uses.
    source = (ROOT / 'scripts/regress.py').read_text(encoding='utf-8')
    assert '"--rng-seed", "%d"' % golden['rng_seed'] in source and golden['route'] == 'fight'
    assert golden['max_step'] == 1800 and '--stream-steps' not in source
    # Nothing but identifiers and numbers: no game state is stored, only its hash.
    assert set(golden) == {'schema', 'what', 'route', 'rng_seed', 'step_input', 'max_step', 'records', 'sha256',
                           'recorded', 'note'}


def played_free_for_all():
    """Event shapes of a live four-fighter match: two fighters put out, then the decisive result."""
    def roster(health):
        return [dict(slot=i, character=4 + i, cpu=int(i > 0), health=h) for i, h in enumerate(health)]
    return [dict(event='match_start', match=1, step=1, match_type=2, fighters=roster([265.6, 278.4, 265.6, 265.6])),
            dict(event='movement', match=1, step=700, slot=0, distance=2.2, held=8),
            dict(event='attack', match=1, step=800, slot=0, target_slot=2, pressed=32, steps_since_press=2),
            dict(event='damage', match=1, step=801, slot=2, cpu=1, health_before=265.6, health_after=260.0),
            dict(event='result', match=1, step=2450, code=32, decisive=False, draw=False, time_up=False,
                 winners=[3], losers=[2], fighters=roster([199.5, 264.3, 0, 216.1])),
            dict(event='result', match=1, step=10864, code=32, decisive=False, draw=False, time_up=False,
                 winners=[3], losers=[0], fighters=roster([0, 60.3, 0, 65.8])),
            dict(event='result', match=1, step=16633, code=9, decisive=True, draw=False, time_up=False,
                 winners=[3], losers=[1], fighters=roster([0, 0, 0, 36.3])),
            dict(event='match_over', match=1, step=16634, winner_slot=3, phase=2, fighters=roster([0, 0, 0, 36.3]))]


def test_free_for_all_requires_every_other_fighter_to_be_put_out():
    checks = statuses(ev.combat_checks(fight_log(played_free_for_all()), fighters=4))
    assert set(checks.values()) == {'pass'} and 'combat.eliminations' in checks
    # A One on One has no such assertion.
    assert 'combat.eliminations' not in statuses(ev.combat_checks(fight_log(played_fight())))
    for change in (lambda e: e.pop(5),                                   # a fighter was never put out
                   lambda e: e[5].update(losers=[2]),                    # the same fighter twice
                   lambda e: e[6]['fighters'][0].update(health=12.0),    # a "loser" still standing
                   lambda e: e[6]['fighters'][3].update(health=0)):      # the winner at zero
        events = played_free_for_all()
        change(events)
        assert statuses(ev.combat_checks(fight_log(events), fighters=4))['combat.eliminations'] == 'fail'


def played_versus():
    roster = [dict(slot=0, character=9, cpu=0, health=265.6), dict(slot=1, character=14, cpu=0, health=265.6)]
    return [dict(event='match_start', match=1, step=1, match_type=2, fighters=roster),
            dict(event='movement', match=1, step=650, slot=0, distance=2.1, held=8),
            dict(event='movement', match=1, step=655, slot=1, distance=2.1, held=4),
            dict(event='attack', match=1, step=700, slot=0, target_slot=1, pressed=32, steps_since_press=3),
            dict(event='damage', match=1, step=701, slot=1, cpu=0, health_before=265.6, health_after=262.0),
            dict(event='attack', match=1, step=760, slot=1, target_slot=0, pressed=16, steps_since_press=1),
            dict(event='damage', match=1, step=760, slot=0, cpu=0, health_before=265.6, health_after=261.0)]


def test_two_pad_match_needs_both_players_to_move_and_hit():
    log = fight_log(played_versus(), summary=False)
    checks = statuses(ev.combat_checks(log, humans=2, require_result=False))
    assert set(checks.values()) == {'pass'} and 'combat.two_players' in checks
    assert 'combat.two_players' not in statuses(ev.combat_checks(log, require_result=False))
    for change in (lambda e: e[0]['fighters'][1].update(cpu=1),     # the second fighter is the CPU
                   lambda e: e.pop(2),                               # the second pad never walked
                   lambda e: e.pop(5),                               # the second pad never hit
                   lambda e: e[6].update(health_after=265.6)):       # its hit did nothing
        events = played_versus()
        change(events)
        result = statuses(ev.combat_checks(fight_log(events, summary=False), humans=2, require_result=False))
        assert result['combat.two_players'] == 'fail'


def test_versus_route_plugs_two_pads_and_scripts_both():
    route = harness.ROUTES['versus']
    assert route['env'] == {'RECOMP_USB_PADS': '2'}
    assert 'p2-start' in ''.join(route['stages']) and 'p2-left:' in route['tail'] and 'p2-x:' in route['tail']
    path = suite.step_input(Path(__file__).parent / '..' / '..' / 'logs' / 'unit-step-input.txt', seconds=30, slots=(0, 1)) \
        if (Path(__file__).parent / '..' / '..' / 'logs').is_dir() else None
    if path:
        rows = [line.split() for line in Path(path).read_text().splitlines() if not line.startswith('#')]
        assert {r[0] for r in rows} == {'0', '1'}
        assert {r[3] for r in rows if r[0] == '0'} == {'8', '10', '20', '40', '80'}
        assert {r[3] for r in rows if r[0] == '1'} == {'4', '10', '20', '40', '80'}
        Path(path).unlink()


def test_anchor_can_wait_for_the_nth_call():
    summary = {'calls': ['Game.StartGame()', 'Game.GetMatchSummary(0, 1)', 'Game.StartGame()']}
    assert harness.reached(summary, 'game.startgame(') and harness.reached(summary, 'game.startgame(#2')
    assert not harness.reached(summary, 'game.startgame(#3') and not harness.reached(summary, 'game.getmatchsummary(#2')
    # A comma would end the anchor in the runtime's script, and lose the count with it.
    for part in harness.SECOND_MATCH_FFA.split(','):
        assert not part.startswith('@') or part.count('#') <= 1
    anchors = [p for p in harness.SECOND_MATCH_FFA.split(',') if p.startswith('@')]
    assert '@controllersetup(0#2' in anchors and '@getscreeninfo(battle/cmtype#2' in anchors


def test_two_matches_route_judges_each_match_on_its_own():
    route = harness.ROUTES['two-matches']
    assert route['until'] == 'game.getmatchsummary(#3' and '@game.startgame(#2' in route['tail']
    assert suite.SESSION_ROUTES['two-matches'] == ((1, 2), (2, 4))
