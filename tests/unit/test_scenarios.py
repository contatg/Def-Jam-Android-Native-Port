"""scripts/scenarios.py: generated routes, fixture library, expectations, cutscene routes."""
import importlib.util
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


scenarios = load('scenarios')
harness = load('harness')

KEYS = ('stages', 'tail', 'until', 'after', 'secs', 'shots', 'env', 'expected_venue')


@pytest.mark.parametrize('mode,venue,length,existing', [
    ('one-on-one', 'foundation', 'short', 'fight'),
    ('one-on-one', 'foundation', 'result', 'fight-result'),
    ('one-on-one', 'terrordome', 'short', 'fight-terrordome'),
    ('free-for-all', 'foundation', 'short', 'ffa'),
    ('free-for-all', 'foundation', 'result', 'ffa-result'),
    ('free-for-all', 'terrordome', 'short', 'ffa-terrordome'),
])
def test_calibrated_combinations_are_the_harness_routes(mode, venue, length, existing):
    route = scenarios.fight_route(harness, mode, venue, length)
    want = harness.ROUTES[existing]
    assert {k: route.get(k) for k in KEYS} == {k: want.get(k) for k in KEYS}


def test_names_make_scenario_suite_treat_them_as_fights():
    assert scenarios.route_name('one-on-one', 'terrordome', 'short') == 'fight-gen-terrordome'
    assert scenarios.route_name('free-for-all', 'foundation', 'result') == 'ffa-gen-foundation-result'


def test_fighter_moves_replace_only_the_selection_stage():
    route = scenarios.fight_route(harness, 'one-on-one', 'foundation', p1='right@4;right@5;a@7', cpu='down@4;a@6')
    assert route['stages'][-2] == 'controllersetup(0,0=right@4;right@5;a@7'
    assert route['stages'][-1] == 'controllersetup(1,=down@4;a@6'
    assert route['stages'][:-2] == list(harness.ONE_ON_ONE_SETUP[:-1])
    ffa = scenarios.fight_route(harness, 'free-for-all', 'foundation', p1='a@5')
    assert 'controllersetup(0,0=a@5' in ffa['stages']
    assert sum(s.startswith('controllersetup(0,0=') for s in ffa['stages']) == 1


def test_unknown_names_say_what_is_known():
    with pytest.raises(SystemExit, match='terrordome'):
        scenarios.fight_route(harness, 'one-on-one', 'nowhere')
    with pytest.raises(SystemExit, match='free-for-all'):
        scenarios.fight_route(harness, 'tag', 'foundation')


def test_catalog_venues_and_modes_are_well_formed():
    cat = scenarios.catalog()
    for name, v in cat['venues'].items():
        assert 'steps' in v and isinstance(v.get('calibrated'), bool), name
        assert v.get('index') is None or isinstance(v['index'], int), name
    for entry in cat['fighters'].get('calibrated', []):
        assert entry['p1'] and all(isinstance(c, int) for c in entry['characters'].values()), entry
    assert cat['cutscenes']['launch_command'] is None or '%s' in cat['cutscenes']['launch_command']


def test_generated_route_carries_the_venue_index_the_suite_checks():
    route = scenarios.fight_route(harness, 'one-on-one', 'the-pit')
    assert route['venue_index'] == 19 and route['stages'][-1].startswith('getscreeninfo(battle/chsvenue=right@6;down@8')
    assert scenarios.fight_route(harness, 'one-on-one', 'terrordome')['venue_index'] == 6


LOG = ('[FUNCCALL] Game.StartGame()\n'
       '[TEST-EVENT] {"event":"match_start","match":1,"step":1,"host_ms":5,"match_type":0,"fighters":['
       '{"slot":0,"character":4,"cpu":0},{"slot":1,"character":9,"cpu":1}]}\n')


def test_expected_fighters_are_checked_from_telemetry():
    assert scenarios.match_fighters(LOG) == [(0, 4, 0), (1, 9, 1)]
    ok, msg = scenarios.check_expectations(LOG, {0: 4, 1: 9})
    assert ok and 'slot 0: character 4' in msg
    ok, msg = scenarios.check_expectations(LOG, {0: 5})
    assert not ok and 'slot 0: character 4, not 5' in msg
    ok, msg = scenarios.check_expectations('[FUNCCALL] x\n', {0: 4})
    assert not ok and 'no match_start' in msg
    assert scenarios.parse_expect(['0:4', '1:12']) == {0: 4, 1: 12}


def test_cutscene_route_stays_within_the_harness_capture_limit():
    r = scenarios.cutscene_route(harness, 'crib', 'getscreeninfo(story/crib#1=>Game.LoadNextRunModule()', 90, 3)
    times = [t for t in r['shots'].split(',') if not t.startswith('@')]
    assert len(times) <= 16 and float(times[-1]) <= 90
    harness.validate_shots(r['shots'])      # what the harness refused before: 30 captures


def test_cutscene_route_injects_after_the_base_route_and_keeps_capturing():
    r = scenarios.cutscene_route(harness, 'crib', 'getscreeninfo(story/crib#1=>Story.Play(sc05)', 9, 3)
    assert r['env']['RECOMP_FE_INJECT'] == 'getscreeninfo(story/crib#1=>Story.Play(sc05)'
    assert r['shots'] == '@getscreeninfo(story/crib,3,6,9'
    assert r['stages'] == harness.ROUTES['crib']['stages'] and r['until'] is None
    harness.validate_shots(r['shots'])


def test_fixture_library_add_list_and_refuse_duplicates(tmp_path, monkeypatch):
    data = tmp_path / 'data'
    (data / 'save' / 'UserData' / 'ABC').mkdir(parents=True)
    (data / 'save' / 'UserData' / 'ABC' / 'profile.dat').write_bytes(b'x' * 32)
    monkeypatch.setenv('DEFJAM_DATA', str(data))
    suite = load('scenario_suite')
    out, entry = scenarios.fixture_add(harness, suite, 'crib-day1', None, 'crib', 'after the first fight')
    assert (out / 'fixture.json').exists() and (out / 'save' / 'UserData' / 'ABC' / 'profile.dat').exists()
    lib = json.loads((data / 'fixtures' / 'library.json').read_text())
    assert lib['fixtures']['crib-day1']['story'] == 'crib'
    assert scenarios.fixture_path(harness, 'crib-day1') == out
    with pytest.raises(SystemExit, match='exists'):
        scenarios.fixture_add(harness, suite, 'crib-day1', None, 'crib', '')
    with pytest.raises(SystemExit, match='letters'):
        scenarios.fixture_add(harness, suite, '../escape', None, None, None)
    assert scenarios.main(['fixture', 'verify']) == 0


def test_scene_names_come_from_the_dump_sc_first(tmp_path, monkeypatch):
    cinema = tmp_path / 'extracted' / 'assets' / 'cinema'
    cinema.mkdir(parents=True)
    for n in ('gf1a', 'sc10', 'sc00', 'sc07a'):
        (cinema / (n + '.viv')).write_bytes(b'')
    monkeypatch.setattr(harness, 'data_dir', lambda: str(tmp_path))
    assert scenarios.scene_names(harness) == ['sc00', 'sc07a', 'sc10', 'gf1a']


def test_contact_sheet_orders_captures_by_their_seconds(tmp_path):
    Image = pytest.importorskip('PIL.Image')
    for t, shade in ((11.2, 200), (5.6, 100), (90, 50)):
        Image.new('RGB', (64, 48), (shade, 0, 0)).save(tmp_path / ('scene-sc05-%gs.png' % t))
    out = tmp_path / 'sheet.png'
    assert scenarios.contact_sheet(tmp_path, out, 'sc05', width=64)
    sheet = Image.open(out)
    assert sheet.size == (4 * 64, 24 + 48 + 18)
    assert [sheet.getpixel((x * 64 + 30, 40))[0] for x in range(3)] == [100, 200, 50]   # 5.6 s, 11.2 s, 90 s


def test_contact_sheet_keeps_a_widescreen_capture_shape(tmp_path):
    Image = pytest.importorskip('PIL.Image')
    Image.new('RGB', (160, 90), (90, 0, 0)).save(tmp_path / 'scene-sc05-6s.png')
    out = tmp_path / 'sheet.png'
    assert scenarios.contact_sheet(tmp_path, out, 'sc05', width=64)
    assert Image.open(out).size == (4 * 64, 24 + 36 + 18)


def test_scene_shots_follow_the_scene_file_and_stay_within_the_limit():
    spec = scenarios.scene_shots('sc12', 90, 3)
    assert spec.startswith('@sc12.viv#1,')
    harness.validate_shots(spec)
    times = [float(t) for t in spec.split(',')[1:]]
    assert len(times) <= 16 and times[-1] <= 90


def test_play_scene_reads_the_launch(tmp_path, monkeypatch):
    class Run:
        text = '[SCENE] loading sc12\n[SCENE] playing sc12 (scene 13) in the game\'s cinema mode\n[SCENE] sc12 ended; x\n'
        log = str(tmp_path / 'run.log.err')
        def shots(self):
            return []
    seen = {}
    def run_game(name, **kw):
        seen.update(kw, name=name)
        return Run()
    monkeypatch.setattr(harness, 'run_game', run_game)
    played, ended, sheet, run = scenarios.play_scene(harness, 'sc12', secs=30, every=5, out=tmp_path)
    assert played and ended and sheet is None
    assert seen['name'] == 'scene-sc12' and seen['env'] == {'RECOMP_SCENE': 'sc12'} and seen['scripted'] is False
