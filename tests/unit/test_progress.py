"""scripts/progress.py reads PROGRESS.md in pieces; it must agree with the file it reads."""
import importlib.util
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
spec = importlib.util.spec_from_file_location('progress', os.path.join(ROOT, 'scripts', 'progress.py'))
progress = importlib.util.module_from_spec(spec)
sys.modules['progress'] = progress
spec.loader.exec_module(progress)

SAMPLE = """\
# Title

## 0. Status at a glance

| Area | State | Notes |
|---|---|---|
| Plan | ✅ | done long ago |
| Updater | ⏳ | waiting on a play-test |
| Mixed | ✅ implementation / ⏳ acceptance | PR open |

## 1. Decisions log

| # | Date | Decision | Rationale |
|---|---|---|---|
| D1 | 2026-09-17 | Data lives outside the repo | Shippable without assets. |
| D2 | 2026-09-18 | Use the XISO | Same partition. |

## 5. Open questions for Vlad

- **(2026-10-12) Mac updater.** Still to do: the interactive run.
- **(2026-10-11) HD textures.** Mac regressions.

## 6. Work log

- 2026-10-12 10:00 — first
- 2026-10-12 11:00 — second
- 2026-10-12 12:00 — third
- 2026-10-12 13:00 — fourth

## 7. Hand-off

Do this next.
"""


@pytest.fixture
def sample(tmp_path):
    p = tmp_path / 'PROGRESS.md'
    p.write_bytes(SAMPLE.replace('\n', '\r\n').encode('utf-8'))      # a Windows checkout
    return str(p)


def run(capsys, sample, *argv):
    assert progress.main(['--file', sample, *argv]) == 0
    return capsys.readouterr().out


def test_sections_are_split_on_their_numbers():
    s = progress.sections(SAMPLE)
    assert sorted(s) == [0, 1, 5, 6, 7]
    assert s[7].strip().endswith('Do this next.')


def test_section_prints_the_whole_section(sample, capsys):
    out = run(capsys, sample, 'section', '7')
    assert out.startswith('## 7. Hand-off') and 'Do this next.' in out and '\r' not in out


def test_unknown_section_is_an_error(sample):
    with pytest.raises(SystemExit):
        progress.main(['--file', sample, 'section', '9'])


def test_decision_prints_the_whole_row(sample, capsys):
    out = run(capsys, sample, 'decision', 'D2')
    assert out.strip() == '| D2 | 2026-09-18 | Use the XISO | Same partition. |'
    assert run(capsys, sample, 'decision', '1').startswith('| D1 |')
    with pytest.raises(SystemExit):
        progress.main(['--file', sample, 'decision', 'D99'])


def test_decisions_lists_one_line_each_and_filters(sample, capsys):
    out = run(capsys, sample, 'decisions').splitlines()
    assert out[0].startswith('D1  2026-09-17  Data lives outside')
    assert out[-1].startswith('-- 2 decision(s)')
    assert run(capsys, sample, 'decisions', '--grep', 'xiso').splitlines()[0].startswith('D2')
    assert run(capsys, sample, 'decisions', '--last', '1').splitlines()[0].startswith('D2')


def test_brief_shows_open_rows_recent_log_and_the_handoff(sample, capsys):
    out = run(capsys, sample, 'brief')
    assert 'Updater: ⏳' in out and 'Mixed: ✅ implementation / ⏳ acceptance' in out
    assert 'Plan' not in out and '1 rows are done' in out
    assert '— first' not in out and 'second' in out and 'fourth' in out           # latest three
    assert 'Mac updater' in out and 'Do this next.' in out


def test_grep_numbers_lines_and_reports_the_total(sample, capsys):
    out = run(capsys, sample, 'grep', 'mac', '--max', '1').splitlines()
    assert out[0].split(':')[0].isdigit() and 'Mac' in out[0]
    assert out[-1] == '-- 2 matching lines; 1 not shown (--max)'


def test_the_real_file_parses():
    text = progress.load(progress.DEFAULT)
    s = progress.sections(text)
    assert {0, 1, 5, 6, 7} <= set(s)
    ids = [i for i, _, _ in progress.decision_rows(text)]
    assert ids[0] == 'D1' and len(ids) > 90
