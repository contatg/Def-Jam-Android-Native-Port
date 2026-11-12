#!/usr/bin/env python3
"""Analysis and lift on any host: what analyze.ps1 and recomp.ps1 do on Windows.

    python scripts/pipeline.py analyze            # verify the dump, analyse default.xbe
    python scripts/pipeline.py recomp [--split N] [-- extra lifter args]
    python scripts/pipeline.py build [--preset posix-release] [--clean]

The data folder comes from --data or DEFJAM_DATA and holds `extracted/`.
Analysis outputs are archived to `<data>/analysis` unless --no-archive is passed. The XBE working copy and
the generated C stay local and ignored, as on Windows. Needs pyxbe and capstone.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
SCRIPTS = REPO / 'scripts'
ANALYSIS_DIRS = ('tools/disasm/output', 'tools/func_id/output',
                 'tools/abi_analysis/output', 'tools/recomp/output')


def run(what, cmd, cwd=None):
    if subprocess.run([sys.executable, *map(str, cmd)], cwd=cwd).returncode:
        sys.exit(f'{what} failed')


def data_dir(args):
    d = args.data or os.environ.get('DEFJAM_DATA')
    if not d:
        sys.exit('set DEFJAM_DATA or pass --data: the folder that holds extracted/')
    return Path(d).expanduser().resolve()


def analyze(args):
    data, tk = data_dir(args), args.toolkit.resolve()
    run('dump check', [SCRIPTS / 'verify-dump.py', data / 'extracted'])
    (tk / 'game_files').mkdir(exist_ok=True)
    shutil.copyfile(data / 'extracted/default.xbe', tk / 'game_files/default.xbe')
    run('analysis input snapshot', [SCRIPTS / 'pipeline-state.py', 'begin-analysis', '--toolkit', tk])
    xbe = 'game_files/default.xbe'
    seeds = REPO / 'config/seed_functions.json'
    seed_args = ['--seed-functions', seeds] if seeds.exists() else []
    run('XBE parser', ['-m', 'tools.xbe_parser', xbe, '--json', 'game_files/default_analysis.json'], tk)
    run('disassembly', ['-m', 'tools.disasm', xbe, '--force', '--extra-sections',
                        'D3D,D3DX,DSOUND,XPP,XGRPH,DOLBY', *seed_args], tk)
    run('function identification', ['-m', 'tools.func_id', xbe, '-v'], tk)
    run('ABI analysis', ['-m', 'tools.abi_analysis', xbe, '-v'], tk)
    run('analysis state validation', [SCRIPTS / 'pipeline-state.py', 'record-analysis', '--toolkit', tk])
    if getattr(args, 'no_archive', False):
        return  # Setup candidates keep analysis local; preserve player data caches.
    archive = data / 'analysis'
    archive.mkdir(exist_ok=True)
    for d in ANALYSIS_DIRS:
        if (tk / d).is_dir():
            shutil.copytree(tk / d, archive / d.replace('/', '_'), dirs_exist_ok=True)
    shutil.copyfile(tk / 'game_files/default_analysis.json', archive / 'default_analysis.json')
    print(f'Analysis outputs archived to {archive}')


def recomp(args):
    tk = args.toolkit.resolve()
    if not (tk / 'tools/abi_analysis/output/abi_functions.json').exists():
        sys.exit('run "pipeline.py analyze" first')
    run('recompilation (previous generated sources preserved)',
        [SCRIPTS / 'pipeline-state.py', 'lift', '--toolkit', tk, '--split', args.split, '--', *args.extra])
    gen = [p for p in (REPO / 'src/recomp/gen').iterdir() if p.is_file()]
    print(f"Generated: {sum(p.suffix == '.c' for p in gen)} C files, "
          f"{round(sum(p.stat().st_size for p in gen) / 2**20)} MB")


def build(args):
    """Configure and build a preset, recording what it was built from so the
    harness can refuse a stale executable (build.ps1 does the same)."""
    tk = args.toolkit.resolve()
    state = [SCRIPTS / 'pipeline-state.py']
    folder = REPO / 'build' / args.preset
    run('generated code freshness check', state + ['verify-lift', '--toolkit', tk])
    if args.clean and folder.is_dir():
        shutil.rmtree(folder)
    run('build input snapshot', state + ['begin-build', '--toolkit', tk, '--preset', args.preset])
    for what, cmd in (('configure', ['cmake', '--preset', args.preset, f'-DXBOXRECOMP_DIR={tk}',
                                     '-DDEFJAM_BUILD_GAME=ON']),
                      ('build', ['cmake', '--build', '--preset', args.preset, '--target', 'defjam_recomp'])):
        if subprocess.run(cmd, cwd=REPO).returncode:
            sys.exit(f'{what} failed')
    run('build state validation', state + ['record-build', '--toolkit', tk, '--preset', args.preset])
    print(f'Built build/{args.preset}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--toolkit', type=Path, default=REPO / 'tools/xboxrecomp')
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('analyze')
    a.add_argument('--data')
    a.add_argument('--no-archive', action='store_true', help='Keep analysis in the candidate toolkit; do not write data/analysis')
    a.set_defaults(fn=analyze)
    r = sub.add_parser('recomp')
    r.add_argument('--split', type=int, default=1000)
    r.add_argument('extra', nargs='*')
    r.set_defaults(fn=recomp)
    b = sub.add_parser('build')
    b.add_argument('--preset', default='win-x64-release' if os.name == 'nt' else 'posix-release')
    b.add_argument('--clean', action='store_true')
    b.set_defaults(fn=build)
    args = ap.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
