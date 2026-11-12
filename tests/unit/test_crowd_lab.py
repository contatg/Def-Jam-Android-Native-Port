"""scripts/crowd-lab.py: masks learned from repeated captures, and reading the experiment's logs."""
import importlib.util
import json
from pathlib import Path

import pytest

np = pytest.importorskip('numpy')
pytest.importorskip('PIL')

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('crowd_lab', ROOT / 'scripts/crowd-lab.py')
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)


def scene(seed):
    """A 160x120 'fight': fixed fighters and HUD, a crowd band whose pixels depend on the run."""
    rng = np.random.default_rng(seed)
    im = np.zeros((120, 160, 3), dtype=np.int16)
    im[:, :] = (40, 60, 80)
    im[70:110, 30:60] = (200, 150, 120)          # a fighter, the same every run
    im[0:8, :] = (255, 255, 255)                 # the HUD
    im[20:40, :] = rng.integers(0, 255, size=(20, 160, 3))   # the crowd
    return im


def test_mask_covers_the_crowd_and_nothing_else():
    images = [scene(s) for s in (1, 2, 3)]
    rects, grid, covered = lab.learn_mask(images, cell=16)
    assert rects and all(r[1] >= 16 and r[3] <= 48 for r in rects)          # rows 16..48 hold the band
    assert all(lab.masked_change(images[0], im, rects) == 0.0 for im in images[1:])
    assert 0.1 < covered < 0.35


def test_a_moved_fighter_is_still_seen_through_the_mask():
    a, b = scene(1), scene(2)
    rects, _, _ = lab.learn_mask([a, b], cell=16)
    c = scene(3)
    c[70:110, 30:60] = (40, 60, 80)
    c[70:110, 40:70] = (200, 150, 120)            # the fighter was shoved ten pixels
    assert lab.masked_change(a, c, rects) > 0.01


def test_mask_cli_refuses_a_mask_over_most_of_the_picture(tmp_path):
    from PIL import Image
    paths = []
    for s in (1, 2):
        rng = np.random.default_rng(s)
        p = tmp_path / ('n%d.png' % s)
        Image.fromarray(rng.integers(0, 255, size=(64, 64, 3), dtype=np.uint8)).save(p)
        paths.append(str(p))
    out = tmp_path / 'mask.json'
    assert lab.main(['mask'] + paths + ['--out', str(out), '--png', str(tmp_path / 'm.png')]) == 1
    assert json.loads(out.read_text())['covered_fraction'] > 0.9
    assert (tmp_path / 'm.png').exists()


def test_mask_needs_two_pictures_of_one_size():
    with pytest.raises(ValueError):
        lab.learn_mask([scene(1)])
    with pytest.raises(ValueError):
        lab.learn_mask([scene(1), scene(1)[:60]])


LOG = '\n'.join([
    '[TEST-DRAWS] {"event":"setup","match":1,"step":0,"host_ms":1,"generator":1,"caller":898450,"count":60}',
    '[TEST-DRAWS] {"event":"step1","match":1,"step":1,"host_ms":2,"generator":1,"caller":898450,"count":231}',
    '[TEST-DRAWS] {"event":"step1","match":1,"step":1,"host_ms":2,"generator":3628904,"caller":7,"count":313}',
    '[TEST-DRAWS] {"event":"step1","match":1,"step":1,"host_ms":2,"utility_state":5,"logic_state":6,'
    '"match_state":7,"screen_updates":906,"crowd_updates":902}',
    '[TEST-CROWD] {"event":"first_step","updates_in_window":880,"held":true,"reseeded":2}',
])


def test_draw_report_and_crowd_events_are_read():
    counts, summary = lab.draws_at(LOG)
    assert counts == {(1, 898450): 231, (3628904, 7): 313}
    assert summary['crowd_updates'] == 902 and summary['screen_updates'] == 906
    assert lab.crowd_lines(LOG) == [{'event': 'first_step', 'updates_in_window': 880, 'held': True, 'reseeded': 2}]


def test_streams_compared_record_by_record():
    a = ['{"step":1,"x":1}', '{"step":2,"x":2}', '{"step":3,"x":3}']
    assert lab.first_difference(a, list(a)) is None
    b = list(a)
    b[2] = '{"step":3,"x":4}'
    i = lab.first_difference(a, b)
    assert i == 2 and lab.stream_step(a[i]) == 3
    assert lab.first_difference(a, a[:2]) == 2


def test_report_table_has_a_row_per_variant():
    results = {'none': {'runs_ok': '2/2', 'draws_equal_at_step1': False, 'logic_equal_at_step1': False,
                        'crowd_updates': [902, 903],
                        'stream_parts_at_step': 871, 'pictures': {300: 0.12}},
               'both': {'runs_ok': '2/2', 'draws_equal_at_step1': False, 'logic_equal_at_step1': True,
                        'crowd_updates': [880, 880],
                        'stream_parts_at_step': None, 'pictures': {300: 0.0}}}
    table = lab.report_table(results, [300])
    assert '| none | 2/2 | no | no | 902 / 903 | 871 | 12.00% |' in table
    assert '| both | 2/2 | no | yes | 880 / 880 | never | 0.00% |' in table
