#!/usr/bin/env python3
"""Play Story scenes with the play-test recorder on and measure each one.

A performance pass over the in-engine cutscenes (docs/scene-selector.md): each scene
plays in its own launch through the developer module (RECOMP_SCENE), the recorder
keeps a session of the launch, and the run stops a few seconds after the scene
ends. `summarize` then reads every session between the scene's start and end
(the recorder's `cinema` events) and writes one table: load time, pacing,
hitches and which side they came from, missed vblanks, audio drop-outs, the
texture cache, memory, and the log's faults. `compare` puts two summaries (two
hosts, two builds) side by side.

    python scripts/scene-perf.py run all                    the 38 scenes, stress profile
    python scripts/scene-perf.py run sc05 gf1a --profile default
    python scripts/scene-perf.py summarize logs/scene-perf/<time>
    python scripts/scene-perf.py compare docs/research/scene-perf-macos-2026-10-18.json logs/scene-perf/<deck>
    python scripts/scene-perf.py baseline logs/scene-perf/<time> docs/research/<name>.json   (numbers only)

Profiles (environment variables only, so a player's settings.ini never changes a run):
  stress   16:9, render scale 3, vsync on (RECOMP_ASPECT, RECOMP_RENDER_SCALE, RECOMP_PRESENT_VSYNC)
  deck     16:10, render scale 2, vsync on: the Steam Deck's own screen shape at a sensible scale
  default  the built-in defaults: 4:3, render scale 2, vsync on
--env overrides a profile's variables (--env RECOMP_RENDER_SCALE=2 changes one thing only).
Vsync is used only on a display refreshing at a multiple of 60 Hz; the summary names the refresh.

`run` summarizes at the end by itself. The audio columns need NumPy (and the contact
sheets Pillow), as bug-bundle.py does; without them those columns are left empty.
Everything written holds the game's own pictures and sound: it stays under logs/,
keep it on your computer. Never run two game runs at once.
"""
import argparse
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
VBLANK_MS = 1000.0 / 60.0

PROFILES = {
    'stress': {'RECOMP_ASPECT': '16:9', 'RECOMP_RENDER_SCALE': '3', 'RECOMP_PRESENT_VSYNC': '1'},
    'deck': {'RECOMP_ASPECT': '16:10', 'RECOMP_RENDER_SCALE': '2', 'RECOMP_PRESENT_VSYNC': '1'},
    'default': {},
}


def load(name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------

class SceneStop:
    """A harness controller: stop `after` seconds after the scene ends, at once on a crash or a
    name the game does not know, and when no scene has started `start_secs` into the run. It reads
    the log the harness's own tail follows (`tail_log.path`): the harness names the log when it
    starts the game, which can be a second after this object is made."""

    def __init__(self, scene, after=6.0, start_secs=120.0):
        self.after, self.start_secs = after, start_secs
        self.ended = ('[SCENE] %s ended' % scene).encode()
        self.played = ('[SCENE] playing %s ' % scene).encode()
        self.pos, self.rest, self.checked = 0, b'', 0.0
        self.saw_play = False
        self.stop_at = None
        self.reason = None

    def poll(self, tail_log, elapsed):
        if elapsed - self.checked < 0.5:
            return self.stop_at is not None and elapsed >= self.stop_at
        self.checked = elapsed
        try:
            with open(tail_log.path, 'rb') as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            data = b''
        self.pos += len(data)
        chunk = self.rest + data
        self.rest = chunk[-200:]
        if not self.saw_play and self.played in chunk:
            self.saw_play = True
        if self.stop_at is None:
            if self.ended in chunk:
                self.stop_at, self.reason = elapsed + self.after, 'ended'
            elif b'[CRASH]' in chunk:
                self.stop_at, self.reason = elapsed + 2.0, 'crash'
            elif b'is not a scene of this game' in chunk:
                self.stop_at, self.reason = elapsed, 'unknown scene'
            elif not self.saw_play and elapsed > self.start_secs:
                self.stop_at, self.reason = elapsed, 'never started'
        return self.stop_at is not None and elapsed >= self.stop_at


def session_of(log):
    """The recorder's session folder whose session.json names this run's log."""
    name = os.path.basename(log)
    folders = sorted((REPO / 'logs' / 'sessions').glob('*/session.json'), key=lambda p: p.stat().st_mtime,
                     reverse=True)
    for meta in folders[:10]:
        try:
            if os.path.basename(json.loads(meta.read_text(encoding='utf-8')).get('log', '')) == name:
                return meta.parent
        except (OSError, ValueError):
            continue
    return None


def keep_session(folder, keep, scene, audio):
    """Copy the session's rows and events into the run folder, and measure its audio now: the
    recorder keeps only the newest `[debug] keep_sessions` (20) sessions whole, so a tour of 38
    scenes would find the first ones' rows and audio gone. Returns the copy (None without one)."""
    if folder is None:
        return None
    keep.mkdir(parents=True, exist_ok=True)
    for name in ('frames.csv', 'events.jsonl', 'session.json', 'stalls.jsonl'):
        if (folder / name).exists():
            shutil.copy2(folder / name, keep / name)
    result = {}
    if audio and (folder / 'audio.pcm').exists():
        bb = load('bug-bundle')
        if bb.np is not None:
            rows = bb.read_rows(folder / 'frames.csv')
            play, ended = scene_window(bb.read_events(folder / 'events.jsonl'), scene)
            if rows and play:
                result = scene_audio(bb, folder, rows, play, ended)
    (keep / 'audio.json').write_text(json.dumps({'source': os.path.relpath(folder, REPO), 'result': result}) + '\n',
                                    encoding='utf-8')
    return keep


def run_scenes(a):
    harness = load('harness')
    scenarios = load('scenarios')
    known = scenarios.scene_names(harness)
    names = known if a.names == ['all'] else a.names
    unknown = [n for n in names if known and n not in known]
    if unknown or not names:
        raise SystemExit('not in the dump: %s; known: %s' % (', '.join(unknown) or '(none named)', ' '.join(known)))
    out = Path(a.out) if a.out else REPO / 'logs' / 'scene-perf' / ('%s-%s' % (time.strftime('%Y%m%d-%H%M%S'),
                                                                            a.profile))
    out.mkdir(parents=True, exist_ok=True)
    runs_path = out / 'runs.json'
    runs = json.loads(runs_path.read_text(encoding='utf-8')) if runs_path.exists() else []
    done = {r['scene'] for r in runs}
    env_profile = dict(PROFILES[a.profile])
    for item in a.env or []:
        key, _, value = item.partition('=')
        env_profile[key] = value
    for scene in names:
        if scene in done and not a.again:
            print('%-6s already in %s' % (scene, runs_path))
            continue
        env = dict(env_profile, RECOMP_SCENE=scene, RECOMP_RECORD='1', RECOMP_RECORD_FPS=str(a.record_fps))
        if a.hitch_bundles:
            env['RECOMP_RECORD_AUTO_HITCH_MS'] = str(a.hitch_bundles)
        shots = scenarios.scene_shots(scene, a.every * 16, a.every) if a.every else ''
        stop = SceneStop(scene, start_secs=a.start_secs)
        started = time.time()
        run = harness.run_game('scene-' + scene, shots=shots, secs=int(a.start_secs + a.max_secs),
                               scripted=False, quiet=True, env=env, controller=stop)
        session = keep_session(session_of(run.log), out / 'sessions' / scene, scene, not a.no_audio)
        sheet = None
        if a.every and run.shots():
            path = out / ('%s.png' % scene)
            if scenarios.contact_sheet(run.shots(), path, '%s (%s)' % (scene, a.profile)):
                sheet = path.name
        entry = {'scene': scene, 'profile': a.profile, 'env': env, 'log': os.path.relpath(run.log, REPO),
                 'session': os.path.relpath(session, REPO) if session else None, 'stop': stop.reason or 'timeout',
                 'played': ('[SCENE] playing %s ' % scene) in run.text,
                 'ended': ('[SCENE] %s ended' % scene) in run.text,
                 'exit_code': run.exit_code, 'unexpected_exit': bool(getattr(run, 'unexpected_exit', False)),
                 'wall_s': round(time.time() - started, 1), 'sheet': sheet,
                 'shots': [os.path.basename(s) for s in run.shots()]}
        runs = [r for r in runs if r['scene'] != scene] + [entry]
        runs_path.write_text(json.dumps(runs, indent=1) + '\n', encoding='utf-8')
        print('%-6s played: %-3s ended: %-3s stop: %-13s %5.0f s  %s' % (
            scene, 'yes' if entry['played'] else 'NO', 'yes' if entry['ended'] else 'no', entry['stop'],
            entry['wall_s'], entry['session'] or 'no session'))
    return summarize(out, audio=not a.no_audio)


# ---------------------------------------------------------------------------
# Measuring
# ---------------------------------------------------------------------------

LOG_FAULTS = [
    ('crash', re.compile(rb'\[CRASH\]')),
    ('watchdog', re.compile(rb'\[WATCHDOG\]')),
    ('icall_unresolved', re.compile(rb'\[ICALL\] Failed to resolve')),
    ('unimplemented', re.compile(rb'\[UNIMPL\] untranslated instruction REACHED')),
    ('debug_layer', re.compile(rb'\[D3D11-DEBUG\]')),
    ('indices_truncated', re.compile(rb'indices truncated')),
]
MISSING_FILE = re.compile(rb'\[FILE\] (\S+) -> not opened')
SHOT_LINE = re.compile(rb'\[TRANS\] captured the back buffer')
REFRESH = re.compile(rb'(\d+(?:\.\d+)?) ?Hz')


def scan_log(path, start_byte, end_byte):
    """Faults, missing files and capture moments in the log, split by whether they fall inside
    the scene's byte range [start_byte, end_byte) (None: unknown, everything counts as outside)."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    inside = lambda pos: start_byte is not None and end_byte is not None and start_byte <= pos < end_byte
    faults = {}
    for key, rx in LOG_FAULTS:
        hits = [m.start() for m in rx.finditer(data)]
        faults[key] = {'run': len(hits), 'scene': sum(1 for h in hits if inside(h))}
    missing = {}
    for m in MISSING_FILE.finditer(data):
        name = m.group(1).decode('utf-8', 'replace')
        e = missing.setdefault(name, {'run': 0, 'scene': 0})
        e['run'] += 1
        e['scene'] += 1 if inside(m.start()) else 0
    shots = [m.end() for m in SHOT_LINE.finditer(data)]
    refresh = None
    for line in data.splitlines():       # the line that says whether vsync is used wins over a bare refresh rate
        low = line.lower()
        if (b'refresh' in low or b'vsync' in low) and REFRESH.search(line):
            if refresh is None or b'vsync' in low:
                refresh = line.decode('utf-8', 'replace').strip()[:160]
            if b'vsync' in low:
                break
    return {'faults': faults, 'missing_files': missing, 'shot_bytes': shots, 'refresh_line': refresh,
            'log_bytes': len(data)}


def log_byte_of(path, needle):
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    i = data.find(needle)
    return None if i < 0 else i


def scene_window(events, scene):
    """(play event, ended event) of the scene in the recorder's events."""
    play = next((e for e in events if e.get('kind') == 'cinema' and e.get('play') == scene), None)
    ended = next((e for e in events if e.get('kind') == 'cinema' and e.get('ended') == scene
                  and (play is None or e['frame'] >= play['frame'])), None)
    return play, ended


def measure(entry, bb, audio=True):
    """One scene's numbers from its session (`bb` is bug-bundle.py, loaded)."""
    m = {'scene': entry['scene'], 'played': entry['played'], 'ended': entry['ended'], 'stop': entry.get('stop')}
    if not entry.get('session'):
        m['error'] = 'no recorder session'
        return m
    folder = REPO / entry['session']
    rows = bb.read_rows(folder / 'frames.csv')
    events = bb.read_events(folder / 'events.jsonl')
    play, ended = scene_window(events, entry['scene'])
    if not rows or play is None:
        m['error'] = 'the scene never started' if rows else 'no frame rows'
        return m
    by_frame = {r['frame']: r for r in rows}
    end_frame = ended['frame'] if ended else rows[-1]['frame']
    # The frame that carried the load (play is logged when it is presented) and its neighbours.
    load_rows = [r for r in rows if play['frame'] - 1 <= r['frame'] <= play['frame'] + 1]
    m['load_ms'] = round(max((r['interval_ms'] for r in load_rows), default=0.0), 1)
    # Before play: from the scene's file opening (`cinema scene`) to play.
    opened = next((e for e in events if e.get('kind') == 'cinema' and e.get('scene') == entry['scene']), None)
    m['open_to_play_ms'] = round(play['t_ms'] - opened['t_ms'], 1) if opened else None
    scene_rows = [r for r in rows if play['frame'] + 2 <= r['frame'] <= end_frame]
    m['seconds'] = round(((ended or rows[-1])['t_ms'] - play['t_ms']) / 1000.0, 1)

    # Frames the harness's own back-buffer captures touched (a synchronous read and a file write).
    log = REPO / entry['log']
    clock = bb.byte_clock(rows, 'log_bytes')
    start_byte = log_byte_of(log, ('[SCENE] playing %s ' % entry['scene']).encode())
    end_byte = log_byte_of(log, ('[SCENE] %s ended' % entry['scene']).encode())
    scan = scan_log(log, start_byte, end_byte if end_byte is not None else 1 << 62) or {}
    shot_frames = set()
    for b in scan.get('shot_bytes', []):
        at = bb.time_of_byte(clock, b)
        if at:
            shot_frames.update({at[1] - 1, at[1], at[1] + 1})
    m['shot_frames'] = len([f for f in shot_frames if play['frame'] + 2 <= f <= end_frame])
    measured = [r for r in scene_rows if r['frame'] not in shot_frames]
    st = bb.frame_stats(measured)
    m.update({k: round(st[k], 2) for k in ('median_ms', 'p95_ms', 'p99_ms', 'worst_ms', 'one_percent_low_fps',
                                           'mean_fps')})
    m['frames'] = st['frames']
    iv = [r['interval_ms'] for r in measured if r.get('interval_ms', 0) > 0]
    late = [x for x in iv if x > 1.5 * VBLANK_MS]
    m['late_frames'] = len(late)
    m['missed_vblanks'] = int(sum(max(0, round(x / VBLANK_MS) - 1) for x in late))
    m['late_pct'] = round(100.0 * len(late) / len(iv), 2) if iv else 0.0
    work = [r.get('work_ms', 0.0) for r in measured]
    present = [r.get('present_ms', 0.0) for r in measured]
    m['work_p50_ms'] = round(bb.percentile(work, 0.5), 2)
    m['work_p99_ms'] = round(bb.percentile(work, 0.99), 2)
    m['present_p50_ms'] = round(bb.percentile(present, 0.5), 2)
    m['present_p99_ms'] = round(bb.percentile(present, 0.99), 2)
    m['capture_mean_ms'] = round(st['capture_mean_ms'], 2)
    draws = [r.get('draws', 0) for r in measured]
    m['draws_p50'] = int(bb.percentile(draws, 0.5))
    m['draws_max'] = max(draws) if draws else 0
    m['vertices_max'] = max((r.get('vertices', 0) for r in measured), default=0)
    hitches = st['hitches']
    m['hitches'] = len(hitches)
    m['hitch_sides'] = {}
    for h in hitches:
        m['hitch_sides'][h['side']] = m['hitch_sides'].get(h['side'], 0) + 1
    m['worst_hitches'] = [{'at_s': round((h['t_ms'] - play['t_ms']) / 1000.0, 2), 'ms': round(h['interval_ms'], 1),
                           'side': h['side'], 'work_ms': round(h['work_ms'], 1),
                           'present_ms': round(h['present_ms'], 1), 'tex_uploads': h['tex_uploads'],
                           'tex_upload_kb': round(h['tex_upload_kb']), 'draws': h['draws']}
                          for h in sorted(hitches, key=lambda h: -h['interval_ms'])[:5]]
    tex = st.get('textures', {})
    m['tex_uploads'] = tex.get('uploads', 0)
    m['tex_upload_mb'] = round(tex.get('upload_mb', 0.0), 1)
    m['tex_evictions'] = tex.get('evictions', 0)
    m['tex_cache_mb_max'] = round(tex.get('cache_mb_max', 0.0), 1)
    m['tex_cached_max'] = tex.get('cached_max', 0)
    in_scene = lambda e: play['t_ms'] <= e['t_ms'] <= (ended or rows[-1])['t_ms']
    m['audio_dry'] = sum(e.get('count', 1) for e in events if e.get('kind') == 'audio_dry' and in_scene(e))
    m['audio_dropped'] = sum(e.get('count', 1) for e in events if e.get('kind') == 'audio_dropped' and in_scene(e))
    mem = [e['private_mb'] for e in events if e.get('kind') == 'memory' and 'private_mb' in e]
    mem_scene = [e['private_mb'] for e in events if e.get('kind') == 'memory' and 'private_mb' in e and in_scene(e)]
    m['memory_mb_start'] = mem_scene[0] if mem_scene else None
    m['memory_mb_max'] = max(mem) if mem else None
    m['memory_mb_end'] = mem[-1] if mem else None
    m['log_faults'] = {k: v for k, v in scan.get('faults', {}).items() if v['run']}
    m['missing_files_in_scene'] = sorted(k for k, v in scan.get('missing_files', {}).items() if v['scene'])
    m['refresh_line'] = scan.get('refresh_line')
    after = [e for e in events if e.get('kind') == 'devmod' and e.get('method') == 'Unload']
    m['returned_to_menu'] = bool(after)
    if (folder / 'audio.json').exists():
        m.update(json.loads((folder / 'audio.json').read_text(encoding='utf-8')).get('result', {}))
    elif audio and bb.np is not None and (folder / 'audio.pcm').exists():
        m.update(scene_audio(bb, folder, rows, play, ended))
    return m


def scene_audio(bb, folder, rows, play, ended):
    """Gaps, clicks and clipping in the mixed output between play and end (sample times put on
    the frames' clock through pcm_bytes, as bug-bundle.py does)."""
    try:
        result = bb.analyse_audio(folder / 'audio.pcm')
    except Exception as ex:                       # noqa: BLE001 - reported, never fatal
        return {'audio_error': str(ex)}
    tmap = bb.audio_time_map(rows, 0) if result else None
    if not result or tmap is None:
        return {}
    t0, t1 = play['t_ms'], (ended or rows[-1])['t_ms']
    inside = lambda item: t0 <= tmap(item['sample']) <= t1
    return {'audio_gaps': [{'at_s': round((tmap(g['sample']) - t0) / 1000.0, 2), 'ms': g['ms']}
                           for g in result['gaps'] if inside(g)],
            'audio_clicks': sum(1 for c in result['clicks'] if inside(c)),
            'audio_clip_runs': sum(1 for c in result['clip_runs'] if inside(c)),
            'audio_loudest_dbfs': round(result['loudest_dbfs'], 1)}


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def host_line(runs):
    for r in runs:
        if r.get('session'):
            try:
                meta = json.loads((REPO / r['session'] / 'session.json').read_text(encoding='utf-8'))
            except (OSError, ValueError):
                continue
            s = meta.get('settings', {})
            return {'build': meta.get('build'), 'platform': meta.get('platform'),
                    'aspect': s.get('display.aspect'), 'render_scale': s.get('display.render_scale'),
                    'vsync': s.get('display.vsync'), 'window': '%sx%s' % (s.get('display.window_width'),
                                                                         s.get('display.window_height')),
                    'record_fps': meta.get('recorder', {}).get('video_fps')}
    return {}


def fmt(v, spec='%.1f'):
    return '-' if v is None else (spec % v)


def summarize(folder, audio=True):
    folder = Path(folder)
    runs = json.loads((folder / 'runs.json').read_text(encoding='utf-8'))
    runs.sort(key=lambda r: (not r['scene'].startswith('sc'), r['scene']))     # Story order, as scene_names
    bb = load('bug-bundle')
    rows = []
    for entry in runs:
        try:
            rows.append(measure(entry, bb, audio=audio))
        except Exception as ex:                   # noqa: BLE001 - one bad session must not lose the rest
            rows.append({'scene': entry['scene'], 'played': entry.get('played'), 'ended': entry.get('ended'),
                         'error': '%s: %s' % (type(ex).__name__, ex)})
    host = host_line(runs)
    host['refresh'] = next((r['refresh_line'] for r in rows if r.get('refresh_line')), None)
    summary = {'format': 'defjam-scene-perf/1', 'folder': os.path.relpath(folder, REPO), 'host': host,
               'profile': runs[0].get('profile') if runs else None, 'scenes': rows}
    (folder / 'summary.json').write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')
    (folder / 'summary.md').write_text(summary_markdown(summary), encoding='utf-8')
    print('summary: %s' % (folder / 'summary.md'))
    bad = [r['scene'] for r in rows if not r.get('played') or not r.get('ended') or r.get('error')]
    return 1 if bad else 0


def summary_markdown(s):
    rows = s['scenes']
    h = s.get('host', {})
    out = ['# Scene performance, %s' % s['folder'], '',
           'Build `%s`, %s; profile `%s`: aspect %s, render scale %s, vsync %s, window %s, recorder %s pictures/s.'
           % (h.get('build'), h.get('platform'), s.get('profile'), h.get('aspect'), h.get('render_scale'),
              h.get('vsync'), h.get('window'), h.get('record_fps')),
           'Display: `%s`.' % (h.get('refresh') or 'refresh not logged'), '',
           'Measured from the scene\'s first frame after its load to its end (the recorder\'s `cinema` events), '
           'leaving out frames next to a harness capture. *load* is the frame that carried the scene\'s load; '
           '*late* frames are longer than 1.5 vblanks (25 ms); a *hitch* is 50 ms and 2.5x the running median '
           '(bug-bundle.py\'s rule); *work* is time before Present (game and executor), *present* the time in it.', '',
           '| scene | ok | secs | load ms | median | p99 | worst | 1% low fps | late | missed vbl | hitches (side) '
           '| work p99 | present p99 | tex MB up / evict | dry | audio gaps | faults |',
           '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for r in rows:
        if r.get('error'):
            out.append('| %s | **%s** | | | | | | | | | | | | | | | |' % (r['scene'], r['error']))
            continue
        ok = 'yes' if r.get('ended') else '**no end**'
        sides = ', '.join('%s %d' % kv for kv in sorted(r.get('hitch_sides', {}).items()))
        faults = ', '.join('%s %d' % (k, v['scene']) for k, v in r.get('log_faults', {}).items() if v['scene'])
        gaps = r.get('audio_gaps')
        out.append('| %s | %s | %s | %s | %s | %s | %s | %s | %d (%.1f%%) | %d | %d%s | %s | %s | %s / %d | %d | %s | %s |' % (
            r['scene'], ok, fmt(r.get('seconds')), fmt(r.get('load_ms'), '%.0f'), fmt(r.get('median_ms'), '%.2f'),
            fmt(r.get('p99_ms')), fmt(r.get('worst_ms')), fmt(r.get('one_percent_low_fps')),
            r.get('late_frames', 0), r.get('late_pct', 0.0), r.get('missed_vblanks', 0), r.get('hitches', 0),
            ' (%s)' % sides if sides else '', fmt(r.get('work_p99_ms')), fmt(r.get('present_p99_ms')),
            fmt(r.get('tex_upload_mb')), r.get('tex_evictions', 0), r.get('audio_dry', 0),
            '-' if gaps is None else (', '.join('%g ms @%gs' % (g['ms'], g['at_s']) for g in gaps[:3]) or '0'),
            faults or '-'))
    ok = [r for r in rows if not r.get('error') and r.get('frames')]
    if ok:
        total_frames = sum(r['frames'] for r in ok)
        out += ['', '## Totals', '',
                '- %d of %d scenes played and ended; %.1f minutes measured, %d frames.' % (
                    sum(1 for r in rows if r.get('ended')), len(rows), sum(r['seconds'] for r in ok) / 60.0,
                    total_frames),
                '- Late frames: %d (%.2f%%), %d missed vblanks; hitches: %d.' % (
                    sum(r['late_frames'] for r in ok), 100.0 * sum(r['late_frames'] for r in ok) / max(1, total_frames),
                    sum(r['missed_vblanks'] for r in ok), sum(r['hitches'] for r in ok)),
                '- Scene loads: median %.0f ms, longest %.0f ms (%s).' % (
                    sorted(r['load_ms'] for r in ok)[len(ok) // 2], max(r['load_ms'] for r in ok),
                    max(ok, key=lambda r: r['load_ms'])['scene']),
                '- Audio ran dry %d times in scenes.' % sum(r.get('audio_dry', 0) for r in ok)]
    out += ['', '## Worst hitches per scene', '']
    for r in ok:
        if r.get('worst_hitches'):
            out.append('- **%s**: ' % r['scene'] + '; '.join(
                '%g ms at +%gs (%s; work %g, present %g, %d uploads %d KB)' % (
                    h['ms'], h['at_s'], h['side'], h['work_ms'], h['present_ms'], h['tex_uploads'], h['tex_upload_kb'])
                for h in r['worst_hitches'][:3]))
    missing = sorted({f for r in ok for f in r.get('missing_files_in_scene', [])})
    if missing:
        out += ['', '## Files the game asked for during scenes and did not find', '',
                '%d distinct; first 20: %s' % (len(missing), ', '.join('`%s`' % f for f in missing[:20]))]
    return '\n'.join(out) + '\n'


COMPARE_KEYS = [('load_ms', 'load ms', '%.0f'), ('median_ms', 'median', '%.2f'), ('p99_ms', 'p99', '%.1f'),
                ('worst_ms', 'worst', '%.0f'), ('one_percent_low_fps', '1% low', '%.1f'),
                ('late_pct', 'late %', '%.2f'), ('hitches', 'hitches', '%d'), ('audio_dry', 'dry', '%d')]


def read_summary(path):
    """A run folder's summary.json, or a summary file itself (a committed baseline)."""
    path = Path(path)
    return json.loads((path / 'summary.json' if path.is_dir() else path).read_text(encoding='utf-8'))


BASELINE_DROP = ('missing_files_in_scene', 'log_faults', 'refresh_line', 'worst_hitches')


def baseline(folder, out):
    """A numbers-only copy of a run's summary, fit to commit: no log paths, file names or hitch lists."""
    s = read_summary(folder)
    s['folder'] = str(out)
    s['scenes'] = [{k: v for k, v in r.items() if k not in BASELINE_DROP} for r in s['scenes']]
    Path(out).write_text(json.dumps(s, indent=1) + '\n', encoding='utf-8')
    print('baseline: %s' % out)
    return 0


def compare(a_dir, b_dir):
    a = read_summary(a_dir)
    b = read_summary(b_dir)
    bs = {r['scene']: r for r in b['scenes']}
    la = '%s/%s' % (a['host'].get('platform'), a.get('profile'))
    lb = '%s/%s' % (b['host'].get('platform'), b.get('profile'))
    head = '| scene | ' + ' | '.join('%s A / B' % label for _, label, _ in COMPARE_KEYS) + ' |'
    out = ['# Scene performance: A = %s (`%s`), B = %s (`%s`)' % (la, a['folder'], lb, b['folder']), '', head,
           '|---|' + '---|' * len(COMPARE_KEYS)]
    for r in a['scenes']:
        o = bs.get(r['scene'], {})
        cells = []
        for key, _, spec in COMPARE_KEYS:
            cells.append('%s / %s' % (fmt(r.get(key), spec), fmt(o.get(key), spec)))
        out.append('| %s | %s |' % (r['scene'], ' | '.join(cells)))
    text = '\n'.join(out) + '\n'
    print(text)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='action', required=True)
    r = sub.add_parser('run', help='play scenes with the recorder on, then summarize')
    r.add_argument('names', nargs='+', help="scenes (sc05 gf1a ...) or 'all'")
    r.add_argument('--profile', choices=sorted(PROFILES), default='stress')
    r.add_argument('--out', help='folder for runs.json and the summary (default logs/scene-perf/<time>-<profile>); '
                                 'an existing one is resumed, scenes already in it are skipped')
    r.add_argument('--again', action='store_true', help='re-run scenes already in --out')
    r.add_argument('--every', type=float, default=6.0, help='seconds between contact-sheet captures (16 at most); '
                                                             '0 for none')
    r.add_argument('--record-fps', type=int, default=10, help='recorder pictures a second (0 for pacing only)')
    r.add_argument('--hitch-bundles', type=int, default=0, metavar='MS',
                   help='write a bug bundle for a frame of MS or more (five a run at most)')
    r.add_argument('--start-secs', type=float, default=120.0, help='how long a scene may take to begin')
    r.add_argument('--max-secs', type=float, default=360.0, help='how long a scene may run')
    r.add_argument('--env', action='append', metavar='RECOMP_NAME=VALUE',
                   help='another variable for every launch (an A/B comparison); repeat for more')
    r.add_argument('--no-audio', action='store_true', help='skip the audio analysis')
    s = sub.add_parser('summarize', help='summary.md and summary.json for a run folder')
    s.add_argument('folder')
    s.add_argument('--no-audio', action='store_true')
    c = sub.add_parser('compare', help='two run folders (or summary files) side by side')
    c.add_argument('a')
    c.add_argument('b')
    bl = sub.add_parser('baseline', help='a numbers-only copy of a summary, fit to commit')
    bl.add_argument('folder')
    bl.add_argument('out')
    a = ap.parse_args(argv)
    if a.action == 'run':
        return run_scenes(a)
    if a.action == 'summarize':
        return summarize(a.folder, audio=not a.no_audio)
    if a.action == 'baseline':
        return baseline(a.folder, a.out)
    return compare(a.a, a.b)


if __name__ == '__main__':
    sys.exit(main())
