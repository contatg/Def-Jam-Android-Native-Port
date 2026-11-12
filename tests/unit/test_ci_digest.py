"""scripts/ci-digest.py: which CI jobs a change needs, and the one-table result of a run."""
import importlib.util
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ci_digest', os.path.join(HERE, '..', '..', 'scripts', 'ci-digest.py'))
ci = importlib.util.module_from_spec(spec)
sys.modules['ci_digest'] = ci
spec.loader.exec_module(ci)


def test_only_markdown_is_docs_only():
    assert ci.docs_only(['README.md', 'docs/agent/triage.md', 'PROGRESS.MD'])
    assert not ci.docs_only(['README.md', 'scripts/triage.py'])
    assert not ci.docs_only(['docs/media/launcher.png'])
    assert not ci.docs_only([])                                   # nothing to go on is not docs-only
    assert not ci.docs_only(['.github/workflows/ci.yml'])


def git(cwd, *args):
    subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True)


def make_repo(tmp_path):
    git(tmp_path, 'init', '-q')
    git(tmp_path, 'config', 'user.email', 'a@b')
    git(tmp_path, 'config', 'user.name', 'n')
    (tmp_path / 'a.py').write_text('x = 1\n')
    (tmp_path / 'README.md').write_text('one\n')
    git(tmp_path, 'add', '-A')
    git(tmp_path, 'commit', '-qm', 'one')
    return subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=tmp_path, capture_output=True, text=True).stdout.strip()


def head_of(tmp_path):
    return subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=tmp_path, capture_output=True, text=True).stdout.strip()


def test_decide_on_a_real_repository(tmp_path, monkeypatch):
    base = make_repo(tmp_path)
    monkeypatch.chdir(tmp_path)
    (tmp_path / 'README.md').write_text('two\n')
    git(tmp_path, 'commit', '-qam', 'docs')
    code, reason = ci.decide(base, head_of(tmp_path))
    assert code is False and 'all Markdown' in reason
    (tmp_path / 'a.py').write_text('x = 2\n')
    (tmp_path / 'README.md').write_text('three\n')
    git(tmp_path, 'commit', '-qam', 'code and docs')
    code, reason = ci.decide(base, head_of(tmp_path))
    assert code is True and 'a.py' in reason


def test_decide_is_conservative_when_unsure(tmp_path, monkeypatch):
    base = make_repo(tmp_path)
    monkeypatch.chdir(tmp_path)
    head = head_of(tmp_path)
    assert ci.decide('', head)[0] is True                        # a new branch has no "before"
    assert ci.decide('0' * 40, head)[0] is True                  # nor does a tag push
    assert ci.decide(base, head)[0] is True                      # an empty diff
    assert ci.decide('deadbeef' * 5, head)[0] is True            # a commit git does not have (force-push)
    assert ci.decide(base, '')[0] is True


def test_changes_prints_the_job_output_line(tmp_path, monkeypatch, capsys):
    base = make_repo(tmp_path)
    monkeypatch.chdir(tmp_path)
    (tmp_path / 'README.md').write_text('two\n')
    git(tmp_path, 'commit', '-qam', 'docs')
    assert ci.main(['changes', '--base', base, '--head', head_of(tmp_path)]) == 0
    assert capsys.readouterr().out.strip() == 'code=false'


ALWAYS = ['changes', 'hygiene', 'python-tools']
HEAVY = ['windows-setup', 'runtime-posix']


def needs(code='true', **results):
    base = {'changes': 'success', 'hygiene': 'success', 'python-tools': 'success',
            'windows-setup': 'success', 'runtime-posix': 'success'}
    base.update(results)
    return {k: {'result': v, 'outputs': ({'code': code} if k == 'changes' else {})} for k, v in base.items()}


def run_summary(monkeypatch, tmp_path, data, always=ALWAYS, heavy=HEAVY):
    monkeypatch.setenv('NEEDS', json.dumps(data))
    monkeypatch.setenv('GITHUB_STEP_SUMMARY', str(tmp_path / 'summary.md'))
    code = ci.main(['summary', '--always', ','.join(always), '--heavy', ','.join(heavy)])
    path = tmp_path / 'summary.md'
    return code, (path.read_text() if path.exists() else '')


def test_summary_passes_when_everything_succeeded(monkeypatch, tmp_path):
    code, text = run_summary(monkeypatch, tmp_path, needs())
    assert code == 0 and text.startswith('CI passed')


def test_summary_puts_the_failing_job_first(monkeypatch, tmp_path, capsys):
    code, text = run_summary(monkeypatch, tmp_path, needs(**{'python-tools': 'failure'}))
    assert code == 1 and text.startswith('**CI failed:** python-tools (failure)')
    assert text.index('| python-tools | failure |') < text.index('| hygiene | success |')
    assert '::error::CI failed: python-tools (failure)' in capsys.readouterr().out


def test_heavy_jobs_may_be_skipped_only_for_an_established_docs_only_change(monkeypatch, tmp_path):
    skipped = needs(code='false', **{'windows-setup': 'skipped', 'runtime-posix': 'skipped'})
    code, text = run_summary(monkeypatch, tmp_path, skipped)
    assert code == 0 and '2 job(s) skipped because only Markdown changed' in text
    # the same skip while the change touches code is a failure, not a pass
    wrong = needs(code='true', **{'windows-setup': 'skipped'})
    code, text = run_summary(monkeypatch, tmp_path, wrong)
    assert code == 1 and 'windows-setup (skipped although the change touches more than Markdown)' in text
    # and so is a skip when the changes job gave no answer
    unknown = needs(code='', **{'runtime-posix': 'skipped'})
    assert run_summary(monkeypatch, tmp_path, unknown)[0] == 1


def test_a_skipped_always_job_is_a_failure(monkeypatch, tmp_path):
    code, text = run_summary(monkeypatch, tmp_path, needs(code='false', hygiene='skipped'))
    assert code == 1 and 'hygiene (skipped)' in text


def test_missing_results_fail_closed(monkeypatch, tmp_path):
    assert run_summary(monkeypatch, tmp_path, {})[0] == 1                 # NEEDS={}
    data = needs()
    del data['changes']
    code, text = run_summary(monkeypatch, tmp_path, data)
    assert code == 1 and 'changes (missing)' in text
    data = needs()
    del data['windows-setup']
    code, text = run_summary(monkeypatch, tmp_path, data)
    assert code == 1 and 'windows-setup (missing)' in text


def test_the_changes_job_must_have_succeeded(monkeypatch, tmp_path):
    for bad in ('failure', 'skipped', 'cancelled'):
        assert run_summary(monkeypatch, tmp_path, needs(code='false', changes=bad))[0] == 1


def test_cancelled_and_unknown_states_are_failures(monkeypatch, tmp_path):
    assert run_summary(monkeypatch, tmp_path, needs(**{'windows-setup': 'cancelled'}))[0] == 1
    assert run_summary(monkeypatch, tmp_path, needs(**{'windows-setup': 'weird'}))[0] == 1
    extra = needs()
    extra['other'] = {'result': 'failure', 'outputs': {}}
    assert run_summary(monkeypatch, tmp_path, extra)[0] == 1


def test_bad_needs_json_fails_cleanly(monkeypatch):
    monkeypatch.setenv('NEEDS', '{not json')
    assert ci.main(['summary', '--always', 'a', '--heavy', 'b']) == 1


def test_the_flags_are_required():
    import pytest
    with pytest.raises(SystemExit):
        ci.main(['summary'])


def test_the_workflow_passes_every_job_to_the_gate():
    """ci-result's needs, its --always list and its --heavy list agree with the jobs the workflow defines."""
    import re
    text = open(os.path.join(HERE, '..', '..', '.github', 'workflows', 'ci.yml'), encoding='utf-8').read()
    jobs_block = text[text.index('\njobs:'):]
    ids = re.findall(r'(?m)^  ([a-z][\w-]*):\s*$', jobs_block)
    gate = 'ci-result'
    assert gate in ids
    needs_line = re.search(r'(?m)^    needs: \[([^\]]*)\]', jobs_block[jobs_block.index('  %s:' % gate):]).group(1)
    needed = {n.strip() for n in needs_line.split(',')}
    assert needed == set(ids) - {gate}, 'ci-result must wait for every other job'
    flags = re.search(r'--always (\S+)\s+--heavy (\S+)', jobs_block)
    listed = set(flags.group(1).split(',')) | set(flags.group(2).split(','))
    assert listed == needed, 'the gate must judge every job it waits for: %s' % (listed ^ needed)
