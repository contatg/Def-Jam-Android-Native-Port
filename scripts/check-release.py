#!/usr/bin/env python3
"""Validate a source release tag, version, notes and main ancestry. No game input."""
import argparse
import re
import subprocess
from pathlib import Path
import importlib.util


def validate_version(tag, cmake, notes_present):
    if not re.fullmatch(r'v\d+\.\d+\.\d+', tag):
        raise ValueError('Use a stable vMAJOR.MINOR.PATCH source release tag')
    match = re.search(r'project\(defjam_recomp\s+VERSION\s+(\d+\.\d+\.\d+)', cmake)
    if not match or tag != 'v' + match.group(1):
        raise ValueError('Tag must match the CMake project version')
    if not notes_present:
        raise ValueError('Missing docs/releases/<tag>.md')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    validate_version(args.tag, (repo / 'CMakeLists.txt').read_text(encoding='utf-8'),
                     (repo / 'docs/releases' / (args.tag + '.md')).is_file())

    def git(*arguments):
        return subprocess.check_output(['git', *arguments], cwd=repo, text=True).strip()

    head = git('rev-parse', 'HEAD')
    if git('rev-parse', '--verify', 'refs/tags/' + args.tag + '^{commit}') != head:
        raise ValueError('Checkout must be the exact release tag commit')
    subprocess.run(['git', 'merge-base', '--is-ancestor', head, 'origin/main'],
                   cwd=repo, check=True)
    if git('status', '--porcelain', '--untracked-files=no'):
        raise ValueError('Release checkout has tracked changes')
    spec = importlib.util.spec_from_file_location('source_tree', repo / 'scripts/check-source-tree.py')
    source_tree = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(source_tree)
    count, problems = source_tree.check(repo)
    if problems:
        raise ValueError('Source policy failed: ' + ', '.join(name for name, _ in problems))
    print(f'{args.tag}: {head}; {count} source files; version, notes, main ancestry and hygiene verified')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
