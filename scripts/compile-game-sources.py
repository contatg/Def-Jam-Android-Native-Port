#!/usr/bin/env python3
"""Compile every game-side source file without the dump: hooks, patches, main.c, recomp_manual.c.

CI builds the runtime libraries only (the lifted code needs the player's dump), so the files in
src/ that become part of the game executable were never compiled there; a compiler difference
(MSVC on Windows) would first show on a player's machine, in Setup. This configures the real game
target with two stand-ins in src/recomp/gen -- the toolkit's own `templates/runtime/recomp_types.h`
(identical to what the lift writes) and an empty `recomp_dispatch.c` -- and compiles, without
linking, every object of the game target that does not come from src/recomp/gen. No game code
is involved.

    python scripts/compile-game-sources.py              # build/ci-game-sources
    python scripts/compile-game-sources.py --keep       # leave the stand-ins in place

Refuses to run where lifted code is present (src/recomp/gen/recomp_0000.c): use a clean checkout.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GEN = REPO / 'src' / 'recomp' / 'gen'
TEMPLATE = REPO / 'tools' / 'xboxrecomp' / 'templates' / 'runtime' / 'recomp_types.h'
STANDINS = {'recomp_types.h': None, 'recomp_dispatch.c': '/* Stand-in for scripts/compile-game-sources.py: no game code. */\n'}


def game_objects(build):
    """Object files of the game target that are not lifted code, from Ninja's target list."""
    out = subprocess.run(['ninja', '-C', str(build), '-t', 'targets', 'all'], capture_output=True, text=True,
                         check=True).stdout
    objects = []
    for line in out.splitlines():
        target = line.split(':', 1)[0].strip()
        if '.dir/src/' in target and '/recomp/gen/' not in target and target.endswith(('.obj', '.o')):
            objects.append(target)
    return objects


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--build', default=str(REPO / 'build' / 'ci-game-sources'))
    ap.add_argument('--keep', action='store_true', help='leave the stand-ins in src/recomp/gen')
    a = ap.parse_args(argv)
    if (GEN / 'recomp_0000.c').exists():
        sys.exit('lifted code is present in src/recomp/gen; run this in a clean checkout')
    GEN.mkdir(parents=True, exist_ok=True)
    made = []
    for name, text in STANDINS.items():
        path = GEN / name
        if path.exists():
            continue
        if text is None:
            shutil.copyfile(TEMPLATE, path)
        else:
            path.write_text(text, encoding='utf-8')
        made.append(path)
    try:
        build = Path(a.build)
        subprocess.run(['cmake', '-S', str(REPO), '-B', str(build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                        '-DDEFJAM_BUILD_GAME=ON'], check=True)
        objects = game_objects(build)
        if not objects:
            sys.exit('the game target has no objects: did the configure keep DEFJAM_BUILD_GAME on?')
        print('compiling %d game-side source files' % len(objects), flush=True)
        r = subprocess.run(['ninja', '-C', str(build), '-k', '0'] + objects)
        return r.returncode
    finally:
        if not a.keep:
            for path in made:
                path.unlink()


if __name__ == '__main__':
    sys.exit(main())
