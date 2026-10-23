#!/usr/bin/env python3
"""Make fight pictures repeat: test the crowd controls, and learn masks for what still moves.

A pinned fight (--rng-seed with step-timed input) repeats its fighters, HUD and
arena pixel for pixel, but not its two crowds (docs/research/crowd-nondeterminism.md),
which is why whole-frame fight baselines have not been usable. Two ways round it,
and this script does both:

1. Control the crowd. src/hooks/crowd_control.c has two test controls
   (RECOMP_TEST_CROWD=hold, =reseed) and the runtime can run the game's worker
   threads inline (RECOMP_WORKERS=inline), which takes host timing out of the
   loading window. `run` plays the same pinned fight twice per variant and reports,
   per variant, whether the random-draw counts agree at the first fight step,
   where the state streams part, and how much of each step capture differs:

       python scripts/crowd-lab.py run --fixture crib-day1
       python scripts/crowd-lab.py run --fixture crib-day1 --variants none,hold,both --steps 300,900,1800

   A variant that brings the step-1 counts and the pictures into agreement is the
   one to make the default for visual fight baselines (and to record a new
   tests/golden/fight-stream.json with, since reseed changes the stream).

2. Mask what still moves. `mask` compares captures of the same fight step from
   several runs and writes the cells that differ as rectangles, in the format a
   visual baseline manifest takes (`masks`), with a picture of them:

       python scripts/crowd-lab.py mask logs/scenarios/*/captures/fight-step900.bmp --out logs/crowd-mask-900.json

   Fighters and HUD repeat, so in a healthy pair only the crowd differs and the
   mask is the crowd. If a mask covers more than --max-cover of the picture the
   runs parted for some other reason (a shove that moved a fighter): it says so
   rather than masking the fight away.

Runs need a build and a fixture (scripts/scenarios.py fixture add). Captures stay
local. Never run two game runs at once.
"""
import argparse
import importlib.util
import json
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

try:
    import numpy as np
    from PIL import Image
except ImportError:          # pragma: no cover
    np = None
    Image = None

VARIANTS = {
    'none': {},
    'hold': {'crowd': 'hold'},
    'reseed': {'crowd': 'reseed'},
    'both': {'crowd': 'hold,reseed'},
    'workers': {'route_env': {'RECOMP_WORKERS': 'inline'}},
    'workers-both': {'crowd': 'hold,reseed', 'route_env': {'RECOMP_WORKERS': 'inline'}},
}


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), HERE / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# Pictures
# ---------------------------------------------------------------------------

def read_image(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def changed(a, b, threshold=16):
    """Pixels where any channel differs by more than `threshold`."""
    return (np.abs(a - b) > threshold).any(axis=2)


def learn_mask(images, cell=16, threshold=16, min_fraction=0.02):
    """Cells (cell x cell) in which any pair of the images differs on more than
    `min_fraction` of the pixels. Returns (rectangles, cell grid, covered fraction)."""
    if len(images) < 2:
        raise ValueError('a mask needs at least two pictures of the same moment')
    h, w = images[0].shape[:2]
    if any(im.shape[:2] != (h, w) for im in images):
        raise ValueError('pictures of different sizes')
    diff = np.zeros((h, w), dtype=bool)
    for i in range(len(images)):
        for j in range(i + 1, len(images)):
            diff |= changed(images[i], images[j], threshold)
    rows, cols = (h + cell - 1) // cell, (w + cell - 1) // cell
    grid = np.zeros((rows, cols), dtype=bool)
    for r in range(rows):
        for c in range(cols):
            block = diff[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell]
            grid[r, c] = block.mean() > min_fraction
    rects = []
    for r in range(rows):
        c = 0
        while c < cols:
            if grid[r, c]:
                start = c
                while c < cols and grid[r, c]:
                    c += 1
                rects.append([start * cell, r * cell, min(w, c * cell), min(h, (r + 1) * cell)])
            else:
                c += 1
    # Merge identical runs on consecutive rows into taller rectangles.
    merged = []
    for rect in rects:
        prev = next((m for m in merged if m[0] == rect[0] and m[2] == rect[2] and m[3] == rect[1]), None)
        if prev:
            prev[3] = rect[3]
        else:
            merged.append(list(rect))
    covered = float(grid.mean())
    return merged, grid, covered


def masked_change(a, b, rects, threshold=16):
    """Fraction of unmasked pixels that changed between a and b."""
    keep = np.ones(a.shape[:2], dtype=bool)
    for x0, y0, x1, y1 in rects:
        keep[y0:y1, x0:x1] = False
    if not keep.any():
        return 0.0
    return float(changed(a, b, threshold)[keep].mean())


def mask_picture(image, rects, path):
    im = Image.fromarray(image.astype('uint8'))
    overlay = Image.new('RGB', im.size, (227, 73, 72))
    alpha = Image.new('L', im.size, 0)
    from PIL import ImageDraw
    d = ImageDraw.Draw(alpha)
    for x0, y0, x1, y1 in rects:
        d.rectangle([x0, y0, x1 - 1, y1 - 1], fill=110)
    im.paste(overlay, (0, 0), alpha)
    im.save(path)


# ---------------------------------------------------------------------------
# Logs
# ---------------------------------------------------------------------------

def draws_at(log_text, when='step1'):
    """{(generator, caller): count} and the summary fields of a [TEST-DRAWS] report."""
    counts, summary = {}, {}
    for line in log_text.splitlines():
        if not line.startswith('[TEST-DRAWS]'):
            continue
        try:
            ev = json.loads(line.split('] ', 1)[1])
        except (IndexError, ValueError):
            continue
        if ev.get('event') != when:
            continue
        if 'generator' in ev:
            counts[(ev['generator'], ev['caller'])] = ev['count']
        else:
            summary = {k: v for k, v in ev.items() if k not in ('event', 'match', 'step', 'host_ms')}
    return counts, summary


def crowd_lines(log_text):
    out = []
    for line in log_text.splitlines():
        if line.startswith('[TEST-CROWD]'):
            try:
                out.append(json.loads(line.split('] ', 1)[1]))
            except (IndexError, ValueError):
                pass
    return out


def first_difference(a_lines, b_lines):
    """Index of the first differing state-stream record, or None when they agree."""
    for i, (x, y) in enumerate(zip(a_lines, b_lines)):
        if x != y:
            return i
    return None if len(a_lines) == len(b_lines) else min(len(a_lines), len(b_lines))


def stream_step(line):
    m = re.search(r'"step":(\d+)', line)
    return int(m.group(1)) if m else None


# ---------------------------------------------------------------------------
# The experiment
# ---------------------------------------------------------------------------

def run_variant(scenarios, suite, harness, name, variant, fixture, seed, steps, out, repeat):
    base = dict(harness.ROUTES['fight'])
    if variant.get('route_env'):
        base['env'] = dict(base.get('env', {}), **variant['route_env'])
    route_name = 'fight-crowd-%s' % name
    suite.harness.ROUTES[route_name] = base
    stream = out / ('%s-%d.stream' % (name, repeat))
    argv = ['run', route_name, '--fixture', str(fixture), '--rng-seed', str(seed), '--step-input', 'default',
            '--step-shots', ','.join(map(str, steps)), '--test-env', 'RECOMP_TEST_DRAWS=1',
            '--stream-out', str(stream), '--no-audio',
            '--output', str(out / ('%s-%d' % (name, repeat)))]
    if variant.get('crowd'):
        argv += ['--test-env', 'RECOMP_TEST_CROWD=' + variant['crowd']]
    code = suite.main(argv)
    folder = out / ('%s-%d' % (name, repeat))
    log = (folder / 'game.log.err').read_text(encoding='utf-8', errors='replace') if (folder / 'game.log.err').exists() else ''
    shots = {s: next(iter(sorted((folder / 'captures').glob('*-step%d.bmp' % s))), None) for s in steps} \
        if (folder / 'captures').is_dir() else {}
    return {'code': code, 'log': log, 'stream': stream.read_text().splitlines() if stream.exists() else [],
            'shots': shots, 'folder': str(folder)}


def compare_pair(a, b, steps):
    da, sa = draws_at(a['log'])
    db, sb = draws_at(b['log'])
    idx = first_difference(a['stream'], b['stream'])
    # Draw counts are taken before a reseed acts, so they cannot agree under it; the
    # logic state at step 1 is what a reseed makes repeat.
    row = {'draws_equal_at_step1': bool(da) and da == db,
           'logic_equal_at_step1': sa.get('logic_state') is not None and sa.get('logic_state') == sb.get('logic_state'),
           'crowd_updates': [sa.get('crowd_updates'), sb.get('crowd_updates')],
           'stream_parts_at_step': None if idx is None else (stream_step(a['stream'][idx]) if idx < len(a['stream']) else idx),
           'pictures': {}}
    for s in steps:
        pa, pb = a['shots'].get(s), b['shots'].get(s)
        if pa and pb and np is not None:
            ia, ib = read_image(pa), read_image(pb)
            row['pictures'][s] = float(changed(ia, ib).mean()) if ia.shape == ib.shape else None
    return row


def report_table(results, steps):
    L = ['| variant | runs ok | draws equal at step 1 | logic state equal at step 1 | crowd updates | '
         'streams part at step | ' + ' | '.join('changed at step %d' % s for s in steps) + ' |',
         '|---|---|---|---|---|---|' + '---|' * len(steps)]
    for name, r in results.items():
        pics = ' | '.join(('%.2f%%' % (100 * r['pictures'][s]) if r['pictures'].get(s) is not None else '-') for s in steps)
        L.append('| %s | %s | %s | %s | %s | %s | %s |' % (
            name, r['runs_ok'], 'yes' if r['draws_equal_at_step1'] else 'no',
            'yes' if r.get('logic_equal_at_step1') else 'no',
            ' / '.join(str(x) for x in r['crowd_updates']),
            'never' if r['stream_parts_at_step'] is None else r['stream_parts_at_step'], pics))
    return '\n'.join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='action', required=True)
    m = sub.add_parser('mask', help='learn a mask from captures of the same fight step')
    m.add_argument('images', nargs='+')
    m.add_argument('--out', required=True, help='JSON: rectangles for a baseline manifest, and the coverage')
    m.add_argument('--png', help='also draw the mask over the first picture')
    m.add_argument('--cell', type=int, default=16)
    m.add_argument('--threshold', type=int, default=16)
    m.add_argument('--min-fraction', type=float, default=0.02)
    m.add_argument('--max-cover', type=float, default=0.30,
                   help='refuse a mask over more than this share of the picture (the runs parted)')
    r = sub.add_parser('run', help='the crowd-control matrix on one pinned fight')
    r.add_argument('--fixture', required=True)
    r.add_argument('--variants', default='none,hold,reseed,both,workers')
    r.add_argument('--repeats', type=int, default=2)
    r.add_argument('--seed', type=int, default=12345)
    r.add_argument('--steps', default='300,900')
    a = ap.parse_args(argv)
    if np is None:
        raise SystemExit('needs NumPy and Pillow (pip install numpy pillow)')

    if a.action == 'mask':
        images = [read_image(p) for p in a.images]
        rects, grid, covered = learn_mask(images, a.cell, a.threshold, a.min_fraction)
        result = {'masks': rects, 'covered_fraction': covered, 'cell': a.cell, 'threshold': a.threshold,
                  'images': [str(p) for p in a.images]}
        Path(a.out).write_text(json.dumps(result, indent=1), encoding='utf-8')
        if a.png:
            mask_picture(images[0], rects, a.png)
        print('%d rectangle(s), %.1f%% of the picture -> %s' % (len(rects), covered * 100, a.out))
        if covered > a.max_cover:
            print('MORE THAN %.0f%% MOVES: these runs parted (a fighter moved?), not only the crowd. '
                  'Do not use this mask; compare the state streams.' % (a.max_cover * 100))
            return 1
        return 0

    scenarios = load('scenarios')
    harness = load('harness')
    suite = load('scenario_suite')
    fixture = scenarios.fixture_path(harness, a.fixture)
    steps = [int(s) for s in a.steps.split(',')]
    out = REPO / 'logs' / 'crowd-lab' / time.strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    results = {}
    for name in a.variants.split(','):
        if name not in VARIANTS:
            raise SystemExit('unknown variant %r; known: %s' % (name, ', '.join(VARIANTS)))
        runs = [run_variant(scenarios, suite, harness, name, VARIANTS[name], fixture, a.seed, steps, out, k)
                for k in range(a.repeats)]
        row = compare_pair(runs[0], runs[1], steps) if len(runs) >= 2 else {}
        row['runs_ok'] = '%d/%d' % (sum(1 for x in runs if x['code'] == 0), len(runs))
        row['crowd_events'] = [crowd_lines(x['log']) for x in runs]
        row['folders'] = [x['folder'] for x in runs]
        results[name] = row
        (out / 'results.json').write_text(json.dumps(results, indent=1), encoding='utf-8')
    table = report_table(results, steps)
    (out / 'report.md').write_text('# Crowd lab\n\nSeed %d, fixture %s, %d runs a variant.\n\n%s\n'
                                   % (a.seed, fixture, a.repeats, table), encoding='utf-8')
    print(table)
    print('\n' + str(out / 'report.md'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
