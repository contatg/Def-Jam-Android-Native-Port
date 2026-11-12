#!/usr/bin/env python3
"""Scenarios on demand: a library of save fixtures, generated fight routes, cutscenes.

The harness's routes (scripts/harness.py) each reach one fixed place. Chasing a
bug that only shows at one venue, with one fighter or at one Story point needs a
run that starts there, repeatably, as often as asked. This script builds those
runs from three pieces and hands them to scripts/scenario_suite.py, which keeps
its saves guard, its evidence and its report:

  a fixture   a copy of a save at a known point, kept in a library in the data
              folder (never in the repository). Make one by playing to the point,
              quitting, and running `fixture add`.
  a route     menu presses that reach the match: a match type, a venue and the
              fighters, composed from config/scenario-catalog.json. The combinations
              the harness already had come out exactly as its routes do.
  controls    a pinned seed and step-timed input (a fight that repeats), the crowd
              controls (RECOMP_TEST_CROWD, see scripts/crowd-lab.py), the play-test
              recorder, front-end command injection (RECOMP_FE_INJECT), and Story scenes
              by name through the game's developer module (RECOMP_SCENE).

    python scripts/scenarios.py list
    python scripts/scenarios.py fixture add crib-day1 --story crib --note "after the first fight"
    python scripts/scenarios.py fixture list
    python scripts/scenarios.py generate --mode free-for-all --venue terrordome --print
    python scripts/scenarios.py generate --mode one-on-one --venue terrordome --fixture crib-day1 \\
        --seed 12345 --record --expect-character 0:4
    python scripts/scenarios.py cutscene --fixture crib-day1 --from crib --inject "getscreeninfo(story/crib#1=>Cmd(x))"
    python scripts/scenarios.py scenes                                         (the scenes in the dump)
    python scripts/scenarios.py scene sc12                                     (play it: no save, profile or menu)
    python scripts/scenarios.py scenes sc05 sc07a gf1a                         (each in turn, a contact sheet each; or all)

A generated route is called fight-gen-... or ffa-gen-... so scenario_suite treats it
as a fight (audio, pacing and memory gates). --expect-character SLOT:INDEX checks,
from the match_start telemetry, that the menus picked the fighter the moves were
meant to: a calibration that has drifted fails instead of quietly fighting someone
else. Never run two of these at once (docs/09-testing-harness.md).
"""
import argparse
import os
import importlib.util
import json
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
CATALOG = REPO / 'config' / 'scenario-catalog.json'


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def catalog():
    return json.loads(CATALOG.read_text(encoding='utf-8'))


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

def venue_stage(steps):
    """The venue screen: the moves, then confirm. The harness's own routes press A at 8 s
    when the cursor stays put and at 9 s after a move."""
    if not steps:
        return 'getscreeninfo(battle/chsvenue=a@8+4x4'
    return 'getscreeninfo(battle/chsvenue=%s;a@9+4x4' % steps


def fight_route(harness, mode, venue, length='short', p1=None, cpu=None, cat=None):
    """A route dict in harness.ROUTES form for a match type, a venue and the fighters."""
    cat = cat or catalog()
    if mode not in cat['modes']:
        raise SystemExit('unknown mode %r; known: %s' % (mode, ', '.join(cat['modes'])))
    if venue not in cat['venues']:
        raise SystemExit('unknown venue %r; known: %s (add one to config/scenario-catalog.json)'
                         % (venue, ', '.join(cat['venues'])))
    steps = cat['venues'][venue]['steps']
    if mode == 'one-on-one':
        stages = list(harness.ONE_ON_ONE_SETUP)
        if p1:
            stages[-1] = 'controllersetup(0,0=' + p1
        if steps:
            # As fight-terrordome: the CPU slot is left to the venue screen's presses.
            stages += ['controllersetup(1,=' + (cpu or 'back@9999'), venue_stage(steps)]
        else:
            stages += ['controllersetup(1,=' + (cpu or cat['fighters']['cpu_default'])]
    else:
        stages = list(harness.FFA_SETUP)
        if p1:
            i = next(k for k, s in enumerate(stages) if s.startswith('controllersetup(0,0='))
            stages[i] = 'controllersetup(0,0=' + p1
        stages.append(venue_stage(steps))
    four = mode != 'one-on-one'
    route = {'help': 'generated: %s at %s' % (mode, venue), 'stages': stages, 'tail': harness.FIGHT_TAIL,
             'generated': {'mode': mode, 'venue': venue, 'length': length, 'p1': p1, 'cpu': cpu}}
    if 'index' in cat['venues'][venue]:
        route['venue_index'] = cat['venues'][venue]['index']    # what scenario_suite checks Game.ChooseVenue against
    if venue == 'terrordome':
        # Map traversal changes with the unlocked venues. Select the retail FE ID
        # after the menu's choice, before loading, as the named stress routes do.
        route['env'] = {'RECOMP_FE_INJECT': 'game.choosevenue#1=>Game.ChooseVenue(%d)' % route['venue_index'],
                        'RECOMP_START_LOG': '1'}
        route['expected_venue'] = route['venue_index']
    if length == 'result':
        route.update(until='game.getmatchsummary(', after=6, secs=1200 if four else 900, shots='@game.startgame(,60')
    elif length == 'long' or steps:
        route.update(until='game.startgame(', after=240, secs=620, shots='@game.startgame(,15,60,230')
        if venue == 'terrordome':
            route['shots'] = '@getscreeninfo(battle/oldmload,1,' + route['shots']
    else:
        route.update(until='game.startgame(', after=150 if four else 120, secs=520 if four else 420,
                     shots='@game.startgame(,60,140' if four else '@game.startgame(,60,110')
    return route


def route_name(mode, venue, length):
    return '%s-gen-%s%s' % ('fight' if mode == 'one-on-one' else 'ffa', venue, '' if length == 'short' else '-' + length)


def cutscene_route(harness, base, inject, secs, every):
    """A Story route that, once it is where the base route stops, injects a front-end
    command and then takes a capture every `every` seconds for `secs` seconds."""
    r = dict(harness.ROUTES[base])
    anchor = inject.split('=>', 1)[0].split('#', 1)[0]
    # The harness takes at most 16 captures a launch: spread them wider rather than refuse the run.
    every = max(every, secs / 16.0)
    times = ','.join('%g' % round(t * every, 1) for t in range(1, min(16, int(secs // every)) + 1))
    r.update(help='cutscene: %s then %s' % (base, inject), until=None, after=0,
             secs=r['secs'] + secs + 30, shots='@%s,%s' % (anchor, times),
             env=dict(r.get('env', {}), RECOMP_FE_INJECT=inject), generated={'cutscene': inject, 'from': base})
    return r


# ---------------------------------------------------------------------------
# The fixture library (in the data folder)
# ---------------------------------------------------------------------------

def library_root(harness):
    return Path(harness.data_dir()) / 'fixtures'


def library(harness):
    path = library_root(harness) / 'library.json'
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else {'schema': 1, 'fixtures': {}}


def save_library(harness, lib):
    root = library_root(harness)
    root.mkdir(parents=True, exist_ok=True)
    (root / 'library.json').write_text(json.dumps(lib, indent=2, sort_keys=True), encoding='utf-8')


def fixture_path(harness, name_or_path):
    p = Path(name_or_path)
    if (p / 'fixture.json').exists():
        return p
    q = library_root(harness) / name_or_path
    if (q / 'fixture.json').exists():
        return q
    raise SystemExit('no fixture %r (a folder with fixture.json, or a name in %s)' % (name_or_path, library_root(harness)))


def fixture_add(harness, suite, name, source, story, note):
    if not re.match(r'^[A-Za-z0-9][A-Za-z0-9_.-]*$', name):
        raise SystemExit('a fixture name is letters, digits, dot, dash and underscore')
    lib = library(harness)
    if name in lib['fixtures']:
        raise SystemExit('fixture %r exists; pick another name' % name)
    src = Path(source) if source else Path(harness.data_dir()) / 'save'
    profiles = src / 'UserData' if (src / 'UserData').is_dir() else src
    if not profiles.is_dir():
        raise SystemExit('no saved profiles at ' + str(profiles))
    out = library_root(harness) / name
    import shutil
    import tempfile
    library_root(harness).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fixture-source-', dir=library_root(harness)) as staging:
        shutil.copytree(profiles, Path(staging) / 'UserData')
        identity = suite.create_fixture(staging, out, '%s: %s' % (story or 'unlabelled', note or ''))
    lib['fixtures'][name] = {'story': story, 'note': note, 'created': time.strftime('%Y-%m-%d %H:%M'),
                             'manifest_sha256': identity['manifest_sha256']}
    save_library(harness, lib)
    return out, lib['fixtures'][name]


# ---------------------------------------------------------------------------
# Checking what the menus picked
# ---------------------------------------------------------------------------

def match_fighters(log_text):
    """[(slot, character, cpu)] from the first match_start telemetry event."""
    for line in log_text.splitlines():
        if line.startswith('[TEST-EVENT]') and '"match_start"' in line:
            try:
                ev = json.loads(line.split('] ', 1)[1])
            except (IndexError, ValueError):
                continue
            return [(f['slot'], f['character'], f['cpu']) for f in ev.get('fighters', [])]
    return None


def check_expectations(log_text, expected):
    """expected: {slot: character}. Returns (ok, message)."""
    got = match_fighters(log_text)
    if got is None:
        return False, 'no match_start telemetry in the log (did the match start?)'
    picked = {s: c for s, c, _ in got}
    wrong = {s: (c, picked.get(s)) for s, c in expected.items() if picked.get(s) != c}
    if wrong:
        return False, 'menus picked ' + ', '.join('slot %d: character %s, not %s' % (s, g, w) for s, (w, g) in wrong.items())
    return True, 'menus picked ' + ', '.join('slot %d: character %d' % (s, c) for s, c, _ in got)


def parse_expect(items):
    out = {}
    for item in items or []:
        slot, _, character = item.partition(':')
        out[int(slot)] = int(character)
    return out


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def run_suite(suite, name, route, fixture, a, extra=()):
    suite.harness.ROUTES[name] = route
    argv = ['run', name, '--fixture', str(fixture)]
    if a.seed is not None:
        argv += ['--rng-seed', str(a.seed), '--step-input', 'default']
    if a.record:
        argv += ['--record']
    if getattr(a, 'require_combat', False):
        argv += ['--require-combat']
    for item in getattr(a, 'test_env', None) or []:
        argv += ['--test-env', item]
    argv += list(extra)
    started = time.time()
    code = suite.main(argv)
    reports = sorted((REPO / 'logs' / 'scenarios').glob(name + '-*/report.json'), key=lambda p: p.stat().st_mtime)
    report = reports[-1] if reports and reports[-1].stat().st_mtime >= started else None
    return code, report


def scene_names(harness):
    """The scene names in the user's dump (assets/cinema/<name>.viv), sorted: sc00..sc21, then gf*."""
    folder = Path(harness.data_dir()) / 'extracted' / 'assets' / 'cinema'
    names = [p.stem for p in folder.glob('*.viv')] if folder.is_dir() else []
    return sorted(names, key=lambda n: (not n.startswith('sc'), n))


def _seconds(path):
    m = re.search(r'-([0-9.]+)s\.(?:bmp|png)$', Path(path).name)
    return float(m.group(1)) if m else 0.0


def contact_sheet(captures, out, title, width=320, label='s after the scene began'):
    """Captures (a folder of *-<secs>s.png, or a list of files) in time order, labelled, as one PNG;
    False without Pillow or pictures."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return False
    if isinstance(captures, (str, Path)):
        captures = list(Path(captures).glob('*s.png'))
    shots = sorted((Path(p) for p in captures), key=_seconds)
    if not shots:
        return False
    first = Image.open(shots[0])
    # The captures' own shape: a widescreen run's are wider than 4:3.
    cols, height = 4, max(1, round(width * first.height / max(1, first.width)))
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * width, rows * (height + 18) + 24), (16, 16, 16))
    draw = ImageDraw.Draw(sheet)
    draw.text((6, 6), title, fill=(240, 200, 60))
    for i, shot in enumerate(shots):
        x, y = (i % cols) * width, 24 + (i // cols) * (height + 18)
        sheet.paste(Image.open(shot).convert('RGB').resize((width, height)), (x, y))
        draw.text((x + 4, y + height + 2), '%g%s' % (_seconds(shot), label), fill=(200, 200, 200))
    sheet.save(out)
    return True


def scene_shots(scene, secs, every):
    """Captures on the clock of the scene's own file opening, at most the harness's 16 a launch."""
    every = max(every, secs / 16.0)
    times = [round(t * every, 1) for t in range(1, min(16, int(secs // every)) + 1)]
    return '@%s.viv#1,' % scene + ','.join('%g' % t for t in times)


def play_scene(harness, scene, secs=60, every=4, record=False, out=None):
    """One launch: the developer module plays `scene` once the title screen is up (RECOMP_SCENE,
    src/hooks/dev_module.c, scene_launcher.c), then the game returns to the main menu. No save,
    profile or menu is involved. Returns (played, ended, sheet or None, the run)."""
    env = {'RECOMP_SCENE': scene}
    if record:
        env['RECOMP_RECORD'] = '1'
    run = harness.run_game('scene-' + scene, shots=scene_shots(scene, secs, every), secs=int(110 + secs),
                           scripted=False, quiet=True, env=env)
    played = ('[SCENE] playing %s ' % scene) in run.text
    ended = ('[SCENE] %s ended' % scene) in run.text
    sheet = None
    if played and out is not None:
        path = Path(out) / ('%s.png' % scene)
        if contact_sheet(run.shots(), path, scene):
            sheet = path
    return played, ended, sheet, run


def play_scenes(harness, a):
    """`scene` and `scenes`: list the dump's scenes, or play each named one in its own launch."""
    known = scene_names(harness)
    if not a.names:
        print('%d scenes in %s:' % (len(known), Path(harness.data_dir()) / 'extracted' / 'assets' / 'cinema'))
        print(' '.join(known) or '(none found: is DEFJAM_DATA set?)')
        print('play some: scenarios.py scenes sc05 sc07a   (or all)')
        return 0
    names = known if a.names == ['all'] else a.names
    unknown = [n for n in names if known and n not in known]
    if unknown:
        raise SystemExit('not in the dump: %s; known: %s' % (', '.join(unknown), ' '.join(known)))
    out = REPO / 'logs' / 'scene-tour' / time.strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    rows = []
    for scene in names:
        played, ended, sheet, run = play_scene(harness, scene, a.secs, a.every, a.record, out)
        rows.append((scene, 'yes' if played else 'NO', 'yes' if ended else 'no', sheet.name if sheet else '-',
                     os.path.relpath(run.log, REPO)))
        print('%-6s played: %-3s ended: %-3s sheet: %s' % rows[-1][:4])
    index = ['# Scene tour, %s' % time.strftime('%Y-%m-%d %H:%M'), '',
             'Each scene played by the game\'s developer cinema mode in its own launch (RECOMP_SCENE), '
             'captures every %g s for %g s after the scene\'s file opened. Game pictures: keep this folder local.'
             % (max(a.every, a.secs / 16.0), a.secs), '',
             '| scene | played | ended in the window | sheet | log |', '|---|---|---|---|---|']
    index += ['| %s | %s | %s | %s | %s |' % r for r in rows]
    (out / 'index.md').write_text('\n'.join(index) + '\n', encoding='utf-8')
    print('index: %s' % (out / 'index.md'))
    return 0 if all(r[1] == 'yes' for r in rows) else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='action', required=True)
    sub.add_parser('list', help='modes, venues, Story points and the fixture library')
    fx = sub.add_parser('fixture', help='the fixture library in the data folder')
    fxs = fx.add_subparsers(dest='fixture_action', required=True)
    add = fxs.add_parser('add', help='copy the current saved profiles (or --from) into the library')
    add.add_argument('name')
    add.add_argument('--from', dest='source', help='a save folder (holding UserData); default: <data>/save')
    add.add_argument('--story', help='which Story point this is (an id from the catalog, or your own)')
    add.add_argument('--note', help='anything else worth knowing: the profile, what was just done')
    fxs.add_parser('list')
    fxs.add_parser('verify', help='check every library fixture still matches its hashes')

    gen = sub.add_parser('generate', help='a fight route from a mode, a venue and fighter moves')
    gen.add_argument('--mode', default='one-on-one')
    gen.add_argument('--venue', default='foundation')
    gen.add_argument('--length', choices=('short', 'long', 'result'), default='short')
    gen.add_argument('--p1', help="player 1's fighter-select presses, harness stage syntax")
    gen.add_argument('--cpu', help="the CPU fighter's presses (one on one)")
    gen.add_argument('--print', action='store_true', help='print the route and stop')
    gen.add_argument('--fixture', help='a library name or a fixture folder')
    gen.add_argument('--seed', type=int, help='pin the seeds and step-time the fight (a fight that repeats)')
    gen.add_argument('--record', action='store_true', help='turn the play-test recorder on')
    gen.add_argument('--require-combat', action='store_true')
    gen.add_argument('--expect-character', action='append', metavar='SLOT:INDEX',
                     help='fail unless that slot got that character (match_start telemetry)')
    gen.add_argument('--test-env', action='append', metavar='RECOMP_TEST_NAME=VALUE',
                     help='passed to scenario_suite, e.g. RECOMP_TEST_CROWD=hold,reseed')

    cut = sub.add_parser('cutscene', help='reach a Story point, inject a front-end command, capture what follows')
    cut.add_argument('--fixture', required=True)
    cut.add_argument('--from', dest='base', default='crib', help='the harness route that reaches the starting point')
    cut.add_argument('--inject', required=True, help='a raw RECOMP_FE_INJECT rule: "anchor[#N]=>command[;command]" '
                     '(to play a scene by name, use `scene`)')
    cut.add_argument('--secs', type=float, default=90, help='how long to watch after the injection')
    cut.add_argument('--every', type=float, default=3, help='seconds between captures')
    cut.add_argument('--test-env', action='append', metavar='RECOMP_TEST_NAME=VALUE',
                     help='passed to scenario_suite, e.g. RECOMP_TEST_CROWD=hold')

    for verb, helptext in (('scene', 'play Story scenes by name, each in its own launch (developer module)'),
                           ('scenes', 'the Story scenes in the dump: list them, or play several (all) in turn')):
        scn = sub.add_parser(verb, help=helptext)
        scn.add_argument('names', nargs='*', help="scenes to play (sc05 gf1a ...), or 'all'; none: list them")
        scn.add_argument('--secs', type=float, default=60, help='how long to capture after the scene begins')
        scn.add_argument('--every', type=float, default=4, help='seconds between captures (16 at most)')
        scn.add_argument('--record', action='store_true', help='keep a play-test session of each launch')
    cut.add_argument('--seed', type=int)
    cut.add_argument('--record', action='store_true', default=True)
    cut.add_argument('--print', action='store_true')
    a = ap.parse_args(argv)

    harness = load('harness')
    cat = catalog()
    if a.action == 'list':
        print('modes:   ' + ', '.join('%s%s' % (k, '' if v.get('calibrated') else ' (uncalibrated)') for k, v in cat['modes'].items()))
        print('venues:  ' + ', '.join('%s%s' % (k, '' if v.get('calibrated') else ' (uncalibrated)') for k, v in cat['venues'].items()))
        print('story:   ' + ', '.join(p['id'] for p in cat['story_points']))
        print('cutscene command: %s' % (cat['cutscenes']['launch_command'] or 'not known yet (docs/research/scenario-launching.md)'))
        try:
            lib = library(harness)
        except SystemExit:
            lib = {'fixtures': {}}
        print('library: %s' % (library_root(harness) if lib['fixtures'] else '(empty)'))
        for name, f in sorted(lib['fixtures'].items()):
            print('  %-24s %-14s %s' % (name, f.get('story') or '-', f.get('note') or ''))
        return 0

    if a.action == 'fixture':
        suite = load('scenario_suite')
        if a.fixture_action == 'add':
            out, entry = fixture_add(harness, suite, a.name, a.source, a.story, a.note)
            print('added %s at %s (%s)' % (a.name, out, entry['manifest_sha256'][:12]))
        elif a.fixture_action == 'list':
            for name, f in sorted(library(harness)['fixtures'].items()):
                print('%-24s %-14s %-17s %s' % (name, f.get('story') or '-', f.get('created'), f.get('note') or ''))
        else:
            bad = 0
            for name, f in sorted(library(harness)['fixtures'].items()):
                try:
                    ident = suite.evidence.fixture_identity(library_root(harness) / name)
                    ok = ident['manifest_sha256'] == f['manifest_sha256']
                except (OSError, ValueError) as ex:
                    ok, ident = False, {'error': str(ex)}
                bad += not ok
                print('%-24s %s' % (name, 'ok' if ok else 'CHANGED or unreadable'))
            return 1 if bad else 0
        return 0

    if a.action == 'generate':
        route = fight_route(harness, a.mode, a.venue, a.length, a.p1, a.cpu, cat)
        name = route_name(a.mode, a.venue, a.length)
        if a.print or not a.fixture:
            print(json.dumps({name: route}, indent=2))
            if not a.fixture and not a.print:
                print('(no --fixture: printed only)', file=sys.stderr)
            return 0
        suite = load('scenario_suite')
        expected = parse_expect(a.expect_character)
        code, report = run_suite(suite, name, route, fixture_path(harness, a.fixture), a)
        if expected and report:
            log = (report.parent / 'game.log.err').read_text(encoding='utf-8', errors='replace')
            ok, message = check_expectations(log, expected)
            print(('EXPECTED FIGHTERS: ' if ok else 'WRONG FIGHTERS: ') + message)
            code = code or (0 if ok else 1)
        return code

    if a.action in ('scene', 'scenes'):
        return play_scenes(harness, a)

    # cutscene
    inject = a.inject
    if a.base not in harness.ROUTES:
        raise SystemExit('unknown route %r' % a.base)
    route = cutscene_route(harness, a.base, inject, a.secs, a.every)
    name = 'cutscene-inject'
    if a.print:
        print(json.dumps({name: route}, indent=2))
        return 0
    suite = load('scenario_suite')
    code, report = run_suite(suite, name, route, fixture_path(harness, a.fixture), a)
    if report and not (report.parent / 'game.log.err').exists():
        print('the run did not start: read %s' % report.parent / 'report.json')
        return code or 1
    if report:
        log = (report.parent / 'game.log.err').read_text(encoding='utf-8', errors='replace')
        injected = [l for l in log.splitlines() if l.startswith('[FE-INJECT] after')]
        print('injected: %d command(s); captures in %s' % (len(injected), report.parent / 'captures'))
        if not injected:
            print('the anchor was never reached: check the route and the anchor text')
            code = code or 1
    return code


if __name__ == '__main__':
    sys.exit(main())
