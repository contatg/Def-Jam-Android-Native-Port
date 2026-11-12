#!/usr/bin/env python3
"""Evaluate many assertions/captures in one existing harness route.

python scripts/scenario_suite.py fixture --source C:/local/test-save --output logs/fixtures/story --profile VY3
python scripts/scenario_suite.py run ffa-terrordome --fixture logs/fixtures/story --audio
python scripts/scenario_suite.py run story-tour --fixture logs/fixtures/story

Output is a new local directory containing JSON, JUnit and HTML. Fixtures and
baselines are local only. Combat telemetry is explicitly unfinished.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


harness = load('harness')
# Routes that play a fight until the game records its result.
RESULT_ROUTES = ('fight-result', 'ffa-result')
# Routes in which every fighter is driven by a pad.
TWO_PAD_ROUTES = ('versus',)
# Routes that play several matches in one launch: (match ordinal, fighters) for each.
SESSION_ROUTES = {'two-matches': ((1, 2), (2, 4))}
evidence = load('test_evidence')
session_driver = load('session_driver')


def source_identity(repo):
    repo = Path(repo).resolve()
    def git(*args):
        r = subprocess.run(['git', '-c', 'safe.directory=' + repo.as_posix(), '-C', str(repo), *args],
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else 'unavailable'
    sources = [p for base in ('scripts', 'src/hooks') for p in (repo / base).rglob('*')
               if p.is_file() and p.suffix in ('.py', '.c', '.h')]
    return {'head': git('rev-parse', 'HEAD'), 'status': git('status', '--porcelain'),
            'toolkit_gitlink': git('ls-tree', 'HEAD', 'tools/xboxrecomp'),
            'test_and_hook_hashes': {p.relative_to(repo).as_posix(): evidence.digest(p) for p in sources},
            'native_entry_hashes': {name: evidence.digest(repo / name) for name in ('src/main.c', 'src/recomp_manual.c', 'CMakeLists.txt')},
            'host': platform.platform(), 'python': platform.python_version()}


def create_fixture(source, output, profile):
    source, output = Path(source).resolve(), Path(output).resolve()
    files = evidence.tree_manifest(source)
    if source == output or source in output.parents:
        raise ValueError('fixture output cannot be inside source saves')
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(source, output / 'save')
    metadata = []
    for path in sorted(source.rglob('SaveMeta.xbx')):
        raw = path.read_bytes()
        text = raw.decode('utf-16', errors='replace') if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else raw.decode('utf-8', errors='replace')
        name = next((line[5:] for line in text.splitlines() if line.startswith('Name=')), None)
        metadata.append({'path': path.relative_to(source).as_posix(), 'name': name})
    meta = {'schema': 1, 'profile': profile, 'files': files, 'save_metadata': metadata,
            'selection': 'caller-prepared save root; route must be calibrated for this exact profile list',
            'rng_seed': None, 'rng_status': 'not controlled', 'input_clock': 'anchored wall seconds'}
    (output / 'fixture.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
    return evidence.fixture_identity(output)


def default_fixture(data_root=None):
    """A fixture made from the player's own profiles, for runs that were not given one.

    Copies only `save/UserData` (profiles and options, a few hundred kilobytes) from
    the data folder into a new local fixture; the caches and partition images, which
    are gigabytes and which the game recreates, are left out. The source is read,
    never written.
    """
    data_root = Path(data_root or harness.data_dir())
    profiles = data_root / 'save' / 'UserData'
    if not profiles.is_dir():
        raise ValueError('no saved profiles to make a fixture from: ' + str(profiles))
    output = Path(harness.REPO) / 'logs' / 'fixtures' / ('auto-' + time.strftime('%Y%m%d-%H%M%S'))
    with tempfile.TemporaryDirectory(prefix='fixture-source-', dir=output.parent if output.parent.is_dir() else None) as staging:
        shutil.copytree(profiles, Path(staging) / 'UserData')
        create_fixture(staging, output, "the player's profiles as found; not a curated profile list")
    return output


def baseline_candidates(report_path, output):
    """Package retained captures for human review; approval stays false."""
    report_path, output = Path(report_path).resolve(), Path(output).resolve()
    report = json.loads(report_path.read_text(encoding='utf-8'))
    captures = report_path.parent / 'captures'
    shots = sorted(captures.glob('*.bmp'))
    if not shots:
        raise ValueError('report has no retained BMP captures')
    output.mkdir(parents=True, exist_ok=False)
    items = []
    for shot in shots:
        retained = output / shot.name
        shutil.copyfile(shot, retained)
        items.append({'capture': shot.name, 'baseline': shot.name, 'approved': False,
                      'sha256': evidence.digest(retained), 'max_mae': 3.0,
                      'max_changed_fraction': .02, 'pixel_threshold': 16, 'masks': []})
    manifest = {'schema': 1, 'source_report_sha256': evidence.digest(report_path),
                'source_identity': report['identity'], 'source_assertions': report['assertions'],
                'review': 'Human review required; captures from failed runs are not validated baselines.',
                'routes': {report['name']: items}}
    path = output / 'manifest.json'
    path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    return path


def step_input(path, seconds=900, slots=(0,)):
    """Write a step-timed input file that plays like the harness's scripted fighter.

    Holds right for 72 steps in every 240 and presses each face button for 9
    steps in every 48, one after another (the host-timed FIGHT_TAIL: right for
    1.2 s every 4 s, a button for 0.15 s every 0.8 s). Starts at step 600, ten
    seconds in, when the walk-in is over. A second slot walks left instead,
    towards the first, and presses half a cycle later.
    """
    lines = ['# slot first_step last_step buttons(hex); generated by scenario_suite.step_input']
    end = seconds * 60
    for order, slot in enumerate(slots):
        for start in range(600, end, 240):
            lines.append('%d %d %d %x' % (slot, start, start + 71, 8 if order == 0 else 4))
        for index, button in enumerate((0x10, 0x20, 0x40, 0x80)):
            for start in range(600 + 12 * index + 24 * order, end, 48):
                lines.append('%d %d %d %x' % (slot, start, start + 8, button))
    Path(path).write_text('\n'.join(lines) + '\n', encoding='ascii')
    return path


def bootstrap_stages(route):
    stages = list(harness.ROUTES[route]['stages'])
    if route.startswith(('fight', 'ffa', 'versus', 'two-matches')):
        # The final venue setup contains fallback A presses. Close that stage
        # when the match starts so live combat input has sole ownership.
        stages.append('game.startgame(=back@9999')
    return stages


def scenario_settings(path=None):
    settings = {'render_scale': 2, 'gamma': True, 'vsync': True,
                'brightness': 100, 'vignette': True, 'vignette_strength': 100}
    if path:
        supplied = json.loads(Path(path).read_text(encoding='utf-8'))
        if set(supplied) - set(settings):
            raise ValueError('unknown scenario display setting')
        settings.update(supplied)
    for key, low, high in (('render_scale', 1, 4), ('brightness', 50, 200), ('vignette_strength', 0, 100)):
        if type(settings[key]) is not int or not low <= settings[key] <= high:
            raise ValueError('%s must be an integer from %d to %d' % (key, low, high))
    if any(type(settings[k]) is not bool for k in ('gamma', 'vsync', 'vignette')):
        raise ValueError('gamma/vsync/vignette must be booleans')
    env = {'RECOMP_RENDER_SCALE': str(settings['render_scale']),
           'RECOMP_GAMMA': str(int(settings['gamma'])), 'RECOMP_PRESENT_VSYNC': str(int(settings['vsync'])),
           'RECOMP_BRIGHTNESS': str(settings['brightness']), 'RECOMP_VIGNETTE': str(int(settings['vignette'])),
           'RECOMP_VIGNETTE_STRENGTH': str(settings['vignette_strength'])}
    return settings, env


def route_assertions(route, run, preset):
    checks = [evidence.assertion('runtime.faults', not harness.run_faults(run),
                                '; '.join(harness.run_faults(run)) or 'no detected runtime faults'),
              evidence.assertion('save.restoration', run.save_ok is True, 'complete disposable save-root restoration')]
    anchors = {'story-tour': ['getscreeninfo(story/crib', 'getscreeninfo(story/gym'],
               'intro': ['game.startstorymode(', 'getscreeninfo(story/chardec'],
               'crib': ['getscreeninfo(story/crib'], 'gym': ['getscreeninfo(story/gym']}
    fight = route.startswith(('fight', 'ffa', 'versus', 'two-matches'))
    for anchor in (['game.startgame('] if fight else anchors.get(route, [])):
        checks.append(evidence.assertion('route.' + anchor, harness.reached(run.summary, anchor), 'expected FUNCCALL'))
    if route in ('gym', 'story-tour'):
        hit = run.text.lower().find('getscreeninfo(story/gym')
        count = run.text[hit:].lower().count('blazin_') if hit >= 0 else 0
        checks.append(evidence.assertion('gym.preview_request', count > 0, 'preview path requested; not decode/render proof', requests=count))
    if fight:
        contexts = evidence.fight_contexts(run.text)
        context = contexts[0] if contexts else {}
        slots = context.get('slots', {})
        # A generated route names its venue (scenarios.py, the catalog's index); the harness's own: 6 or 1.
        expected_venue = harness.ROUTES.get(route, {}).get('venue_index', 6 if 'terrordome' in route else 1)
        checks += [evidence.assertion('setup.venue_request', context.get('venue') == expected_venue,
                                      'front-end venue request before StartGame', context=context, expected=expected_venue),
                   evidence.assertion('setup.fighter_requests', len(slots) == (4 if route.startswith('ffa') else 2),
                                      'front-end selected fighter slots before StartGame', slots=slots)]
        p = harness.presents_after(run.text, 'game.startgame(')
        median = sorted(p)[len(p) // 2] if p else 0
        floor = 110 if preset.endswith('-release') else (80 if route.startswith('ffa') else 100)
        checks += [evidence.assertion('fight.presentation_duration', len(p) >= 30, 'two-second reporting intervals', seconds=2 * len(p)),
                   evidence.assertion('fight.presentation_rate', bool(p) and median >= floor,
                                      'median presents per two seconds', median=median, floor=floor, minimum=min(p) if p else 0)]
    return checks


def visual_assertions(manifest_path, route, run, folder):
    manifest_path = Path(manifest_path).resolve()
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    items = manifest['routes'].get(route, [])
    if not items:
        return [evidence.assertion('visual.baselines', False, 'no approved checkpoints for route')]
    checks = []
    for i, item in enumerate(items):
        base = (manifest_path.parent / item['baseline']).resolve()
        current = Path(run.shots_dir) / item['capture']
        # Hash locks approval to the actual baseline bytes; never auto-bless captures.
        if not item.get('approved') or not base.is_file() or evidence.digest(base) != item.get('sha256'):
            checks.append(evidence.assertion('visual.baseline.' + str(i), False, 'missing, unapproved or changed baseline'))
            continue
        if not current.is_file():
            checks.append(evidence.assertion('visual.capture.' + str(i), False, 'required checkpoint capture missing'))
            continue
        check = evidence.visual_check(current, base, folder / ('diff-%d.png' % i), harness,
                                      max_mae=item.get('max_mae', 3),
                                      max_changed_fraction=item.get('max_changed_fraction', .02),
                                      pixel_threshold=item.get('pixel_threshold', 16), masks=item.get('masks', []))
        checks.append(check)
    return checks


def recorder_sessions():
    root = Path(harness.REPO) / 'logs' / 'sessions'
    return {p.name for p in root.iterdir() if p.is_dir()} if root.is_dir() else set()


def recorder_results(before, folder):
    """The session the run's recorder made, analysed with scripts/bug-bundle.py, and each bundle in it."""
    root = Path(harness.REPO) / 'logs' / 'sessions'
    new = sorted(recorder_sessions() - before)
    if not new:
        return {'session': None}
    session = root / new[-1]
    bug_bundle = load_hyphenated('bug-bundle')
    out = {'session': str(session), 'bundles': []}
    try:
        summary = bug_bundle.analyse(session, folder / 'recorder')
        out['session_report'] = str(folder / 'recorder' / 'report.md')
        out['hitches'] = len(summary['hitches'])
        out['frames'] = summary['frames']
    except (OSError, ValueError, SystemExit) as ex:
        out['session_error'] = str(ex)
    for bundle in sorted((session / 'bundles').glob('*/bundle.json')):
        try:
            bug_bundle.analyse(bundle.parent)
            out['bundles'].append(str(bundle.parent / 'report.md'))
        except (OSError, ValueError, SystemExit) as ex:
            out['bundles'].append('%s: %s' % (bundle.parent, ex))
    return out


def load_hyphenated(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def execute(args):
    started = time.monotonic()
    folder = Path(args.output or (Path(harness.REPO) / 'logs/scenarios' / (args.route + '-' + time.strftime('%Y%m%d-%H%M%S')))).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    fight_route = args.route.startswith(('fight', 'ffa', 'versus', 'two-matches'))
    audio = args.audio if args.audio is not None else fight_route
    report = {'schema': 1, 'name': args.route, 'identity': source_identity(harness.REPO),
              'scope': {'audio_health_required': audio, 'approved_visual_required': bool(args.baselines),
                        'combat_required': args.require_combat,
                        'combat_result_required': args.require_combat and args.route in RESULT_ROUTES,
                        'full_gameplay_acceptance': False},
              'assertions': [], 'images': [], 'seconds': 0}
    try:
        fixture = evidence.fixture_identity(args.fixture)
        settings, display_env = scenario_settings(args.settings)
        report['identity']['fixture'] = fixture
        report['identity']['environment'] = evidence.environment_identity()
        report['identity']['scenario_settings'] = settings
        if args.settings:
            report['identity']['settings_sha256'] = evidence.digest(args.settings)
        report['identity']['evidence_limits'] = ['menu input uses anchored wall seconds'
                                                + ('; fight input is step-timed' if args.step_input else ''),
                                                'RNG seed is the game\'s own unless --rng-seed is given',
                                                'generated PCM precedes device submission']
        report['identity']['rng_seed_override'] = args.rng_seed
        # Fail before copying a potentially large save/cache tree when no
        # certified game build exists in this isolated worktree.
        harness.pipeline_state.verify_build(Path(harness.REPO), args.preset)
        with tempfile.TemporaryDirectory(prefix='runtime-', dir=folder) as disposable:
            shutil.copytree(Path(args.fixture) / 'save', Path(disposable) / 'save')
            env = {'DEFJAM_DATA': disposable, 'RECOMP_SETTINGS': 'none',
                   'RECOMP_PAD_HOST': '0', 'RECOMP_PRESENT_PACING': '1'}
            env.update(display_env)
            # Fixture saves live in a disposable data root; HD pack assets can
            # be read explicitly from the caller's separate mods directory.
            # Ordinary scenarios still suppress inherited pack diagnostics.
            if getattr(args, 'texture_packs', None) is not None:
                env['RECOMP_TEXTURE_PACKS'] = args.texture_packs
            if getattr(args, 'texture_root', None):
                env['RECOMP_TEXTURE_ROOT'] = str(Path(args.texture_root).resolve())
            if args.no_unlock:
                env['RECOMP_UNLOCK_ALL'] = '0'
            if args.observe_combat or args.require_combat:
                env['RECOMP_TEST_OBSERVATIONS'] = '1'
            for item in args.test_env or []:
                name, _, value = item.partition('=')
                if not name.startswith('RECOMP_TEST_'):
                    raise ValueError('--test-env only sets RECOMP_TEST_* variables')
                env[name] = value
            if args.rng_seed is not None:
                env['RECOMP_TEST_RNG_SEED'] = str(args.rng_seed)
            sessions_before = recorder_sessions() if getattr(args, 'record', False) else None
            if sessions_before is not None:
                # The play-test recorder (docs/playtest-recorder.md): frame rows, events and pictures of the run.
                env['RECOMP_RECORD'] = '1'
                if args.record_hitch_ms:
                    env['RECOMP_RECORD_AUTO_HITCH_MS'] = str(args.record_hitch_ms)
            if args.step_shots:
                steps = [int(x) for x in args.step_shots.split(',')]
                if any(x <= 0 for x in steps) or steps != sorted(set(steps)):
                    raise ValueError('--step-shots needs increasing positive steps')
                shots_dir = Path(harness.REPO) / 'logs' / 'shots' / args.route
                env['RECOMP_TEST_OBSERVATIONS'] = '1'
                env['RECOMP_TEST_SHOT'] = str(shots_dir / (args.route + '.bmp'))
                env['RECOMP_TEST_SHOT_STEPS'] = ','.join(map(str, steps))
            if audio:
                env.update(RECOMP_APU_LEVEL='1', RECOMP_APU_PCM=str(folder / 'audio.pcm'))
            controller = None
            if args.session_plan:
                plan = json.loads(Path(args.session_plan).read_text(encoding='utf-8'))
                expected_fixture = plan.get('fixture_manifest_sha256')
                if expected_fixture and expected_fixture != fixture['manifest_sha256']:
                    raise ValueError('session recipe requires a different fixture manifest')
                controller = session_driver.SessionDriver(plan, folder / 'live-pad.txt')
                report['identity']['session_plan'] = plan
                report['identity']['session_plan_sha256'] = evidence.digest(args.session_plan)
                env['RECOMP_PAD_LIVE'] = str(controller.live_path)
                bootstrap = plan.get('bootstrap_route', 'boot')
                if bootstrap not in harness.ROUTES:
                    raise ValueError('unknown session bootstrap route: ' + str(bootstrap))
                # Reuse a calibrated front-end setup, then let the live driver
                # own fight/return/next-mode input. No repeating static tail.
                run = harness.run_game(args.route, stages=bootstrap_stages(bootstrap),
                                       secs=plan.get('timeout_seconds', 900), shots=plan.get('shots', ''),
                                       preset=args.preset, env=env, quiet=True, controller=controller, inherit_recomp=False)
            elif args.step_input:
                # The fight's input comes from the step-timed file; the host-timed
                # script only drives the menus and is closed when the match starts.
                source = Path(args.step_input)
                if args.step_input == 'default':
                    source = step_input(folder / 'step-input.txt',
                                        slots=(0, 1) if args.route in TWO_PAD_ROUTES else (0,))
                env['RECOMP_TEST_OBSERVATIONS'] = '1'
                env['RECOMP_TEST_INPUT'] = str(source.resolve())
                report['identity']['step_input_sha256'] = evidence.digest(source)
                route = harness.ROUTES[args.route]
                env = dict(route.get('env', {}), **env)
                run = harness.run_game(args.route, stages=bootstrap_stages(args.route), secs=route['secs'],
                                       shots=route.get('shots', ''), preset=args.preset, env=env, quiet=True,
                                       until=route.get('until'), after=route.get('after', 0), inherit_recomp=False)
                if route.get('until') and not harness.reached(run.summary, route['until']):
                    run.missing_anchor = route['until']
            else:
                run = harness.run_route(args.route, preset=args.preset, env=env, quiet=True, inherit_recomp=False)
            report['identity']['run'] = run.identity
            report['run_seconds'] = run.elapsed
            report['event_timeline'] = run.timeline
            report['pcm_boundaries'] = run.pcm_boundaries
            shutil.copyfile(run.log, folder / 'game.log.err')
            report['identity']['log'] = str(folder / 'game.log.err')
            report['assertions'] += route_assertions(args.route, run, args.preset)
            if controller is not None:
                report['assertions'] += controller.assertions()
            report['assertions'].append(evidence.assertion('fixture.unchanged', evidence.fixture_identity(args.fixture) == fixture,
                                                           'fixture source identity after run'))
            captures = folder / 'captures'
            captures.mkdir()
            report['captures'] = []
            for shot in run.shots():
                retained = captures / Path(shot).name
                shutil.copyfile(shot, retained)
                report['captures'].append({'path': 'captures/' + retained.name,
                                           'sha256': evidence.digest(retained), 'bytes': retained.stat().st_size})
                report['images'].append(harness.bmp_to_png(str(retained)))
            if audio:
                audio_anchor = 'game.startgame(' if args.route.startswith(('fight', 'ffa', 'versus', 'two-matches')) else (
                    'game.startstorymode(' if args.route == 'intro' else 'getscreeninfo(story/crib')
                end_anchor = 'game.getmatchsummary(' if args.route.startswith(('fight', 'ffa', 'versus', 'two-matches')) else (
                    'getscreeninfo(story/chardec' if args.route == 'intro' else None)
                window = evidence.pcm_window(run.pcm_boundaries, audio_anchor, end_anchor)
                report['assertions'].append(evidence.assertion('audio.anchor_offset', window is not None,
                    'PCM offset sampled after host observes scenario anchor; uncertainty is the log polling interval', window=window))
                report['assertions'] += evidence.audio_checks(folder / 'audio.pcm', run.text,
                                                              anchor=audio_anchor,
                                                              end_anchor=end_anchor,
                                                              start_byte=window['start_byte'] if window else None,
                                                              end_byte=window['end_byte'] if window else None,
                                                              max_silent_seconds=args.max_silent_seconds)
                pcm = folder / 'audio.pcm'
                if pcm.is_file() and pcm.stat().st_size % 4 == 0 and window:
                    report['audio_preview'] = evidence.audio_preview(pcm, folder / 'audio-preview.wav',
                        start_byte=window['start_byte'], end_byte=window['end_byte'])
                    report['audio'] = [str(folder / 'audio-preview.wav')]
            if fight_route:
                start = next((e['observed_seconds'] for e in run.timeline
                              if 'game.startgame(' in e['call'].lower()), None)
                report['memory'] = run.memory
                report['assertions'] += evidence.pacing_checks(run.text, end_anchor='game.getmatchsummary(')
                report['assertions'] += evidence.memory_checks(run.memory, (start or 0) + 20)
            if args.stream_out:
                lines = evidence.state_stream(run.text, args.stream_steps)
                Path(args.stream_out).write_text('\n'.join(lines) + '\n', encoding='utf-8')
                report['assertions'].append(evidence.assertion('determinism.recorded', bool(lines),
                    'state stream written for a later run to be compared with', records=len(lines),
                    max_step=args.stream_steps, path=str(args.stream_out)))
            if args.stream_expect:
                expected = Path(args.stream_expect).read_text(encoding='utf-8').splitlines()
                report['identity']['stream_expect_sha256'] = evidence.digest(args.stream_expect)
                report['assertions'] += evidence.stream_checks(run.text, expected, args.stream_steps)
            if sessions_before is not None:
                report['recorder'] = recorder_results(sessions_before, folder)
                report['assertions'].append(evidence.assertion(
                    'recorder.session', bool(report['recorder'].get('session')),
                    'the play-test recorder kept a session for this run', **report['recorder']))
            if args.baselines:
                report['assertions'] += visual_assertions(args.baselines, args.route, run, folder)
                report['images'] += [str(p) for p in folder.glob('diff-*.png')]
            if args.require_combat and args.route in SESSION_ROUTES:
                # Each match is judged on its own events; nothing carries over.
                for ordinal, count in SESSION_ROUTES[args.route]:
                    for check in evidence.combat_checks(run.text, fighters=count, match=ordinal):
                        report['assertions'].append(dict(check, name='%s.match%d' % (check['name'], ordinal)))
            elif args.require_combat:
                report['assertions'] += evidence.combat_checks(
                    run.text, fighters=4 if args.route.startswith('ffa') else 2,
                    humans=2 if args.route in TWO_PAD_ROUTES else 1,
                    require_result=args.route in RESULT_ROUTES)
            if args.observe_combat:
                report['observations'] = evidence.health_observations(run.text)
                linked = report['observations']['linked_health_decreases']
                report['assertions'].append(evidence.assertion('probe.hit_linked_health_decrease', bool(linked),
                    'diagnostic hit-resolution followed by defender health decrease; no player-input causality or KO proof', count=len(linked)))
                report['identity']['evidence_limits'].append('health observations use routine-entry counts, not simulation frames')
    except (SystemExit, OSError, ValueError, KeyError) as ex:
        report['assertions'].append(evidence.assertion('prerequisites_or_evaluation', False, str(ex)))
    finally:
        harness.kill_stray()
        report['seconds'] = time.monotonic() - started
        ok = evidence.write_report(folder, report)
    print(('SELECTED GATES PASS' if ok else 'FAIL/BLOCKED') + ': ' + str(folder / 'index.html'))
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    fixture = sub.add_parser('fixture')
    fixture.add_argument('--source', required=True, help='caller-prepared save root, copied without changes')
    fixture.add_argument('--output', required=True, help='new ignored/local directory')
    fixture.add_argument('--profile', required=True, help='explicit identity and progression description')
    candidates = sub.add_parser('baseline-candidates', help='copy report captures into a new local review folder, unapproved')
    candidates.add_argument('--report', required=True, help='retained report.json')
    candidates.add_argument('--output', required=True, help='new local folder; keep under ignored logs')
    run = sub.add_parser('run')
    run.add_argument('route', choices=sorted(harness.ROUTES))
    run.add_argument('--fixture', required=True)
    run.add_argument('--output')
    run.add_argument('--preset', default=harness.RELEASE_PRESET)
    run.add_argument('--settings', help='optional JSON render_scale/gamma/vsync/brightness/vignette/vignette_strength; inherited RECOMP diagnostics are suppressed')
    run.add_argument('--texture-packs', help='explicit semicolon-separated HD pack folders for this scenario')
    run.add_argument('--texture-root', help='local mods folder read by the scenario; saves still use a disposable root')
    audio_group = run.add_mutually_exclusive_group()
    audio_group.add_argument('--audio', action='store_true', default=None, help='strict health gates (default for fights)')
    audio_group.add_argument('--no-audio', dest='audio', action='store_false', help='explicit runtime-only smoke run; omit audio gates')
    run.add_argument('--observe-combat', action='store_true', help='experimental read-only hit/health observations; does not satisfy --require-combat')
    run.add_argument('--max-silent-seconds', type=float, default=5)
    run.add_argument('--test-env', action='append', metavar='RECOMP_TEST_NAME=VALUE',
                     help='a probe switch for this run (repeatable); recorded in the report')
    run.add_argument('--step-input', metavar='FILE|default',
                     help="fight input timed in simulation steps (RECOMP_TEST_INPUT); 'default' generates the scripted fighter")
    run.add_argument('--step-shots', metavar='N,N,...',
                     help='capture the frame at these fight steps (comparable between runs of a repeatable fight)')
    run.add_argument('--stream-out', metavar='FILE',
                     help='record the game-state stream of the first --stream-steps fight steps')
    run.add_argument('--stream-expect', metavar='FILE',
                     help='require this run to reproduce a recorded stream (needs --step-input and --rng-seed)')
    run.add_argument('--stream-steps', type=int, default=1800,
                     help='fight steps the stream covers (default 1800, thirty seconds)')
    run.add_argument('--rng-seed', type=int,
                     help="replace the seed of the match's random number generator (changes CPU behaviour; for repeatable runs)")
    run.add_argument('--no-unlock', action='store_true',
                     help='run without RECOMP_UNLOCK_ALL (the harness turns it on); the pinned fight stream was recorded without it')
    run.add_argument('--baselines', help='local approved-baseline manifest')
    run.add_argument('--session-plan', help='experimental ordered-screen recipe using live pad input')
    run.add_argument('--record', action='store_true',
                     help='turn the play-test recorder on and analyse its session and bundles (docs/playtest-recorder.md)')
    run.add_argument('--record-hitch-ms', type=int, default=0,
                     help='with --record: a frame this long writes a bundle by itself')
    run.add_argument('--require-combat', action='store_true',
                     help='assert movement, a player attack, damage and (on routes that play to the end) the result from game state')
    args = parser.parse_args(argv)
    if args.action == 'fixture':
        print(json.dumps(create_fixture(args.source, args.output, args.profile), indent=2))
        return 0
    if args.action == 'baseline-candidates':
        print(baseline_candidates(args.report, args.output))
        return 0
    if (args.stream_out or args.stream_expect) and not (args.step_input and args.rng_seed is not None):
        parser.error('a state stream is only repeatable with --step-input and --rng-seed')
    if args.texture_root and args.texture_packs is None:
        parser.error('--texture-root requires explicit --texture-packs')
    return execute(args)


if __name__ == '__main__':
    raise SystemExit(main())
