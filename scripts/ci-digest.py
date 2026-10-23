#!/usr/bin/env python3
"""Two small helpers for .github/workflows/ci.yml, kept here so they can be tested.

    python scripts/ci-digest.py changes --base SHA --head SHA     prints `code=true` or `code=false`
    python scripts/ci-digest.py summary --always a,b --heavy c,d  reads the `needs` context from $NEEDS (JSON)

`changes` decides whether the expensive jobs (three setup builds, the MSVC runtime, the POSIX runtime and the
native fixtures) have anything to check: a change that only touches Markdown files cannot alter them. The
cheap jobs (source hygiene and the unit tests, which do read some Markdown) always run. Whenever it cannot be
sure, it says `code=true`: no base commit (a new branch, a tag, a force-push), a git failure, an empty diff.

`summary` turns the results of every job into one short table (appended to $GITHUB_STEP_SUMMARY when set) and
fails closed: exit 1 unless every `--always` job (including `changes`) succeeded and every `--heavy` job either
succeeded or was skipped because `changes` established a docs-only change. A job that is missing from the
results, was skipped for any other reason, failed, was cancelled or is in any other state is a problem. The
failing jobs are listed first, so whoever (or whatever) reads a failed run starts there and does not have to
list every job to find it.

Standard library only.
"""
import argparse
import json
import os
import subprocess
import sys

NO_COMMIT = {'', '0' * 40}


def docs_only(paths):
    """True when every changed path is a Markdown file (and there is at least one)."""
    paths = [p for p in paths if p.strip()]
    return bool(paths) and all(p.lower().endswith('.md') for p in paths)


def changed_paths(base, head, run=subprocess.run):
    r = run(['git', 'diff', '--name-only', '-z', base, head], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError((r.stderr or b'git diff failed').decode('utf-8', 'replace').strip())
    return [p.decode('utf-8', 'replace') for p in r.stdout.split(b'\0') if p]


def decide(base, head, run=subprocess.run):
    """(code, reason)"""
    base, head = (base or '').strip(), (head or '').strip()
    if base in NO_COMMIT or not head:
        return True, 'no base commit to compare with'
    try:
        paths = changed_paths(base, head, run)
    except (RuntimeError, OSError) as ex:
        return True, 'git could not compare the commits: %s' % ex
    if not paths:
        return True, 'the diff is empty'
    if docs_only(paths):
        return False, '%d changed file(s), all Markdown' % len(paths)
    other = [p for p in paths if not p.lower().endswith('.md')]
    return True, '%d changed file(s); first non-Markdown: %s' % (len(paths), other[0])


def cmd_changes(args):
    code, reason = decide(args.base, args.head)
    print('code=%s' % ('true' if code else 'false'))
    print('docs-only check: %s -> code=%s' % (reason, str(code).lower()), file=sys.stderr)
    return 0


def judge(needs, always, heavy):
    """Problems, as `job (why)` strings. Fails closed: a job that is missing, skipped without cause, failed,
    cancelled or in any unexpected state is a problem. Only the `heavy` jobs may be skipped, and only when
    the `changes` job succeeded and said the change is docs-only (`code` = false)."""
    problems = []
    code = (needs.get('changes') or {}).get('outputs', {}).get('code')
    docs_only_established = (needs.get('changes') or {}).get('result') == 'success' and code == 'false'
    for job in always:
        result = (needs.get(job) or {}).get('result')
        if result != 'success':
            problems.append('%s (%s)' % (job, result or 'missing'))
    for job in heavy:
        result = (needs.get(job) or {}).get('result')
        if result == 'success' or (result == 'skipped' and docs_only_established):
            continue
        if result == 'skipped':
            problems.append('%s (skipped although the change touches more than Markdown)' % job)
        else:
            problems.append('%s (%s)' % (job, result or 'missing'))
    for job, info in sorted(needs.items()):
        if job not in always and job not in heavy and (info or {}).get('result') in ('failure', 'cancelled'):
            problems.append('%s (%s)' % (job, info['result']))
    return problems


def summarise(needs, always, heavy):
    """(markdown, problems)"""
    problems = judge(needs, always, heavy)
    bad = {p.split(' ')[0] for p in problems}
    names = list(dict.fromkeys(list(always) + list(heavy) + sorted(needs)))
    order = sorted(names, key=lambda k: (k not in bad, (needs.get(k) or {}).get('result') == 'success', k))
    lines = ['| Job | Result |', '|---|---|']
    lines += ['| %s | %s |' % (k, (needs.get(k) or {}).get('result', 'missing')) for k in order]
    head = ('**CI failed:** ' + '; '.join(problems)) if problems else 'CI passed'
    skipped = [k for k in heavy if (needs.get(k) or {}).get('result') == 'skipped']
    note = ('\n\n%d job(s) skipped because only Markdown changed.' % len(skipped)
            if skipped and not problems else '')
    return head + '\n\n' + '\n'.join(lines) + note + '\n', problems


def cmd_summary(args):
    try:
        needs = json.loads(os.environ.get('NEEDS', '{}'))
    except json.JSONDecodeError as ex:
        print('NEEDS is not JSON: %s' % ex, file=sys.stderr)
        return 1
    always = [j for j in args.always.split(',') if j]
    heavy = [j for j in args.heavy.split(',') if j]
    text, problems = summarise(needs, always, heavy)
    out = os.environ.get('GITHUB_STEP_SUMMARY')
    if out:
        with open(out, 'a', encoding='utf-8') as f:
            f.write(text)
    print(text.splitlines()[0])
    if problems:
        print('::error::CI failed: %s' % '; '.join(problems))
    return 1 if problems else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('changes')
    p.add_argument('--base', default='')
    p.add_argument('--head', default='')
    p.set_defaults(fn=cmd_changes)
    p = sub.add_parser('summary')
    p.add_argument('--always', required=True, help='comma-separated jobs that must succeed on every run (include `changes`)')
    p.add_argument('--heavy', required=True, help='comma-separated jobs that may be skipped only for a docs-only change')
    p.set_defaults(fn=cmd_summary)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
