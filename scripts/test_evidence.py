"""Local evidence evaluation and reports. Standard library; no game launch.

Images and PCM remain local. Metrics do not establish console fidelity, audible
device output, or event synchronization. Missing required evidence fails closed.
"""
from array import array
import hashlib
import html
import json
import os
import math
from pathlib import Path
import re
import sys
import wave
import xml.etree.ElementTree as ET


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def assertion(name, passed, detail, **metrics):
    """`passed` is True, False, or None for a check that did not apply here (reported as skipped,
    never as a pass)."""
    status = 'skip' if passed is None else 'pass' if passed else 'fail'
    return dict(name=name, status=status, detail=detail, metrics=metrics)


def tree_manifest(root):
    root = Path(root)
    if not root.is_dir() or root.is_symlink():
        raise ValueError('fixture save must be a real directory')
    out = {}
    for p in sorted(root.rglob('*')):
        # Junctions must not escape a fixture even when Path.is_symlink is false.
        if p.is_symlink() or (hasattr(p, 'is_junction') and p.is_junction()):
            raise ValueError('fixture cannot contain links: ' + str(p))
        if p.is_file():
            out[p.relative_to(root).as_posix()] = digest(p)
    if not out:
        raise ValueError('fixture save is empty')
    return out


def fixture_identity(root):
    root = Path(root).resolve()
    meta = json.loads((root / 'fixture.json').read_text(encoding='utf-8'))
    actual = tree_manifest(root / 'save')
    if actual != meta['files']:
        raise ValueError('fixture contents differ from fixture.json')
    if not meta.get('profile'):
        raise ValueError('fixture needs an explicit profile description')
    return dict(meta, fixture_json_sha256=digest(root / 'fixture.json'), manifest_sha256=hashlib.sha256(
        json.dumps(actual, sort_keys=True).encode()).hexdigest())


LEVEL = re.compile(r'\[APU\] output peak (\d+) .*?buffers dropped (\d+), device ran dry (\d+)')


def fight_contexts(log):
    """Front-end requests at each StartGame boundary, not guest-state identity.

    Resetting between matches prevents an old venue/fighter from satisfying a
    later scenario in a shared process. Intermediate binding requests can use
    slot -1 and are not fighter selections.
    """
    contexts, fighters = [], {}
    venue = match_type = None
    for line in log.splitlines():
        if line.startswith('[FUNCCALL] '):
            call = line[11:].strip().lower()
        elif line.startswith('[FE-INJECT] after ') and ': ' in line:
            call = line.split(': ', 1)[1].strip().lower()
        else:
            continue
        if 'game.resetmatchdata(' in call:
            fighters, venue, match_type = {}, None, None
        if match := re.search(r'controller\.controllersetup\(\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(-?\d+)\s*\)', call):
            slot, host, fighter, difficulty = map(int, match.groups())
            if 0 <= slot < 4:
                fighters[slot] = {'fighter': fighter, 'host_controller': host, 'difficulty': difficulty}
        if match := re.search(r'game\.choosevenue\(\s*(\d+)\s*\)', call):
            venue = int(match[1])
        if match := re.search(r'game\.setmatchtype\(\s*(\d+)\s*\)', call):
            match_type = int(match[1])
        if 'game.startgame(' in call:
            contexts.append({'venue': venue, 'match_type': match_type,
                             'slots': {str(k): v for k, v in sorted(fighters.items())},
                             'evidence': 'front-end setup requests; guest identity is not independently verified'})
    return contexts


def pcm_window(boundaries, anchor, end_anchor=None):
    start = None
    for e in boundaries:
        if start is None and anchor.lower() in e['call'].lower():
            start = e
        elif start is not None and end_anchor and end_anchor.lower() in e['call'].lower():
            return {'start_byte': start['pcm_bytes'], 'end_byte': e['pcm_bytes'],
                    'start_observed_seconds': start['observed_seconds'], 'end_observed_seconds': e['observed_seconds']}
    return dict(start_byte=start['pcm_bytes'], end_byte=None,
                start_observed_seconds=start['observed_seconds']) if start else None


def audio_preview(pcm, output, start_byte=0, end_byte=None, sample_rate=48000, max_seconds=60):
    """Playable WAV of the evaluated PCM tail, with its original sample levels."""
    pcm, output = Path(pcm), Path(output)
    size = pcm.stat().st_size
    end = size if end_byte is None else end_byte
    if size % 4 or type(start_byte) is not int or type(end) is not int or not 0 <= start_byte <= end <= size or start_byte % 4 or end % 4:
        raise ValueError('invalid preview PCM bounds')
    start = max(start_byte, end - int(max_seconds * sample_rate) * 4)
    with pcm.open('rb') as src, wave.open(str(output), 'wb') as dst:
        dst.setnchannels(2)
        dst.setsampwidth(2)
        dst.setframerate(sample_rate)
        src.seek(start)
        while src.tell() < end:
            block = src.read(min(1024 * 1024, end - src.tell()))
            if not block:
                break
            dst.writeframesraw(block)
    return {'path': str(output), 'start_byte': start, 'end_byte': end,
            'seconds': (end - start) / (sample_rate * 4), 'sha256': digest(output)}


def audio_checks(pcm_path, log, anchor='game.startgame(', sample_rate=48000,
                 min_seconds=30, max_silent_seconds=5, silence_rms=25,
                 max_clipped_fraction=0.0, max_drops=0, max_dry=0, tail_seconds=60,
                 start_byte=None, end_byte=None, end_anchor=None):
    """PCM health over generated time; queue counters only after a FUNCCALL anchor.

    PCM is written before submit, so it may include rejected buffers. This is
    intentionally not an event-aligned speech/SFX or hardware-output oracle.
    """
    lines = log.splitlines()
    hit = next((i for i, line in enumerate(lines)
                if line.startswith('[FUNCCALL]') and anchor.lower() in line.lower()), None)
    stop = next((i for i, line in enumerate(lines) if hit is not None and i > hit and end_anchor
                 and line.startswith('[FUNCCALL]') and end_anchor.lower() in line.lower()), len(lines))
    levels = [tuple(map(int, m.groups())) for line in lines[hit + 1:stop] if (m := LEVEL.search(line))] if hit is not None else []
    checks = [assertion('audio.queue_evidence', len(levels) >= min_seconds,
                        'post-anchor level reports (runtime caps reports at 600)', reports=len(levels))]
    if levels:
        # One dropped buffer was seen in a healthy fifteen-minute run and none in four
        # two-to-four-minute fights, so the allowance grows by one per five minutes.
        # Both fights that were played to a knockout dropped exactly one buffer in the
        # slow motion right after the result (and none before it), so a recorded
        # result in the window allows one more. That drop is logged in known issues.
        allowed = max_drops + len(levels) // 300 + (
            1 if any('[TEST-EVENT] ' in line and '"event":"result"' in line for line in lines[hit + 1:stop]) else 0)
        checks += [assertion('audio.dropped_buffers', sum(x[1] for x in levels) <= allowed,
                             'post-anchor interval counters', drops=sum(x[1] for x in levels), allowed=allowed),
                   assertion('audio.dry_queue', sum(x[2] for x in levels) <= max_dry,
                             'post-anchor interval counters', dry=sum(x[2] for x in levels))]
    path = Path(pcm_path)
    if not path.is_file() or not path.stat().st_size:
        return checks + [assertion('audio.pcm', False, 'missing or empty generated PCM')]
    size = path.stat().st_size
    checks.append(assertion('audio.pcm_alignment', size % 4 == 0, '48 kHz signed16 little-endian stereo', bytes=size))
    start, end = 0 if start_byte is None else start_byte, size // 4 * 4 if end_byte is None else end_byte
    if any(type(v) is not int or v < 0 or v % 4 for v in (start, end)) or not 0 <= start <= end <= size:
        return checks + [assertion('audio.pcm_window', False, 'invalid frame-aligned PCM byte bounds', start=start, end=end)]
    count = clipped = peak = silent_run = longest_silence = 0
    sums = [0, 0]
    with path.open('rb') as f:
        # A generated-time tail avoids judging a whole loading sequence as
        # gameplay silence. It is not sample-aligned to FUNCCALL log events.
        begin = max(start, end - int(tail_seconds * sample_rate) * 4) if tail_seconds is not None else start
        f.seek(begin)
        while f.tell() < end:
            chunk = f.read(min(sample_rate * 4, end - f.tell()))
            if not chunk:
                break
            complete = len(chunk) // 4 * 4
            if not complete:
                break
            values = array('h')
            values.frombytes(chunk[:complete])
            if sys.byteorder != 'little':
                values.byteswap()
            energy = sum(v * v for v in values)
            seconds = complete / (sample_rate * 4)
            rms = math.sqrt(energy / len(values))
            silent_run = silent_run + seconds if rms < silence_rms else 0
            longest_silence = max(longest_silence, silent_run)
            clipped += sum(v == -32768 or v == 32767 for v in values)
            peak = max(peak, max(abs(v) for v in values))
            for channel in (0, 1):
                sums[channel] += sum(v * v for v in values[channel::2])
            count += len(values)
    duration = count / (sample_rate * 2)
    fraction = clipped / count if count else 1.0
    checks += [assertion('audio.pcm_duration', duration >= min_seconds, 'generated duration, not wall time', seconds=duration),
               assertion('audio.clipping', fraction <= max_clipped_fraction, 'full-scale sample fraction', fraction=fraction, peak=peak),
               assertion('audio.silence', longest_silence <= max_silent_seconds,
                         'generated-time tail within byte bounds; not sample-accurate event alignment', longest_seconds=longest_silence,
                         start_byte=begin, end_byte=end,
                         rms=[math.sqrt(s / (count / 2)) if count else 0 for s in sums])]
    return checks


def visual_check(current, baseline, output, image_api, max_mae=3.0,
                 max_changed_fraction=0.02, pixel_threshold=16, masks=()):
    """Compare RGB with explicit excluded rectangles; write amplified diff PNG."""
    w, h, rows = image_api.load_bmp(str(current))
    bw, bh, base = image_api.load_bmp(str(baseline))
    if (w, h) != (bw, bh):
        return assertion('visual.' + Path(current).stem, False, 'resolution mismatch')
    for rect in masks:
        if len(rect) != 4 or not (0 <= rect[0] < rect[2] <= w and 0 <= rect[1] < rect[3] <= h):
            raise ValueError('invalid visual exclusion rectangle')
    errors = changed = pixels = 0
    diff = []
    for y in range(h):
        row = bytearray(w * 3)
        for x in range(w):
            if any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in masks):
                continue
            delta = [abs(rows[y][x * 3 + c] - base[y][x * 3 + c]) for c in range(3)]
            pixels += 1
            errors += sum(delta)
            changed += max(delta) > pixel_threshold
            row[x * 3:x * 3 + 3] = bytes(min(255, d * 4) for d in delta)
        diff.append(row)
    if not pixels:
        raise ValueError('visual masks exclude every pixel')
    image_api.write_png(str(output), w, h, diff)
    mae, fraction = errors / (pixels * 3), changed / pixels
    return assertion('visual.' + Path(current).stem,
                     mae <= max_mae and fraction <= max_changed_fraction,
                     'tolerant local approved-baseline comparison', mae=mae, changed_fraction=fraction,
                     baseline_sha256=digest(baseline), current_sha256=digest(current), diff=str(output))


def health_observations(log):
    """Summarize diagnostic probes without claiming control, fighter IDs or KO.

    A decrease is linked only to a preceding hit-resolution invocation for the
    same defender/update. These are actual health writes, but resolution can be
    called for an environmental hit too; player intent is not established.
    """
    events = [json.loads(line.split('[TEST-OBSERVATION] ', 1)[1]) for line in log.splitlines()
              if '[TEST-OBSERVATION] ' in line]
    hits = {}
    linked = []
    for e in events:
        if e.get('kind') == 'hit_resolution':
            hits[(e.get('update'), e.get('defender_slot'))] = e
        elif e.get('kind') == 'health_change' and e.get('before', 0) > e.get('after', 0) >= 0:
            hit = hits.pop((e.get('update'), e.get('slot')), None)
            if hit:
                linked.append({'hit': hit, 'health': e})
    return {'clock': 'fighter-update routine entries; not simulation frames',
            'events': events, 'linked_health_decreases': linked,
            'limits': 'diagnostic evidence only; no input causality, fighter identity, movement or KO assertions'}


def telemetry_events(log, tag='TEST-EVENT'):
    """Parsed `[TEST-EVENT]` (or `[TEST-STATE]`) records, and how many lines were not valid JSON."""
    marker = '[' + tag + '] '
    events, bad = [], 0
    for line in log.splitlines():
        if marker not in line:
            continue
        try:
            events.append(json.loads(line.split(marker, 1)[1]))
        except ValueError:
            bad += 1
    return events, bad


# An action press this many fight steps before a hit still counts as its cause.
# In a live One on One all 84 linked attacks were within 30 steps (half a second).
ATTACK_PRESS_STEPS = 60
# A damage event belongs to an attack that resolved at most this many steps earlier.
DAMAGE_AFTER_ATTACK_STEPS = 5


def combat_checks(log, fighters=2, match=None, require_result=True, results_anchor='game.getmatchsummary(',
                  humans=1):
    """Assert a played fight from the game's own state (src/hooks/test_telemetry.c).

    Absent telemetry is `blocked`, never a pass. `fighters` is how many fighter
    records the match must have; `match` picks one match of a process that played
    several (default: the first). Each assertion names what it proves:

      setup      the match started with that many fighters, at least one human
      movement   a human fighter walked while a direction was held
      attack     a hit by a human fighter resolved within ATTACK_PRESS_STEPS of
                 an action press by that fighter
      damage     that attack's target then lost health
      result     the game recorded one decisive, non-draw result naming a winner
                 and a loser, and its "decided" phase agreed on the winner
      eliminations  (more than two fighters) every fighter but the winner was
                 recorded as a loser, each once, and ended at zero health
      two_players  (humans > 1) that many fighters are pad-controlled, each walked,
                 and each landed a hit on another that took damage
      results    the front end asked for the match summary after the result

    What it does not prove: which move was performed, that the damage came from
    that hit rather than another in the same step, or the reason for the result
    (the code's meaning beyond decisive/draw/time-up is not mapped).
    """
    events, bad = telemetry_events(log)
    if not events:
        return [dict(name='combat.telemetry', status='blocked',
                     detail='no [TEST-EVENT] lines: run with --observe-combat on a build that has the probes', metrics={})]
    matches = sorted({e.get('match') for e in events if isinstance(e.get('match'), int)})
    started = sorted({e['match'] for e in events if e.get('event') == 'match_start' and isinstance(e.get('match'), int)})
    chosen = match if match is not None else (started[0] if started else None)
    mine = [e for e in events if e.get('match') == chosen]
    steps = [e.get('step') for e in mine]
    ordered = all(isinstance(s, int) and s >= 0 for s in steps) and steps == sorted(steps)
    checks = [assertion('combat.telemetry', bad == 0 and bool(mine) and ordered,
                        'well-formed events with non-decreasing fight steps',
                        events=len(mine), malformed=bad, matches=matches, match=chosen)]

    def first(kind, accept=lambda e: True):
        return next((e for e in mine if e.get('event') == kind and accept(e)), None)

    start = first('match_start')
    roster = (start or {}).get('fighters', [])
    humans_list = sorted(f['slot'] for f in roster if not f.get('cpu'))
    humans_set = set(humans_list)
    checks.append(assertion('combat.setup', bool(start) and len(roster) == fighters and bool(humans_list),
                            'match started with the expected fighter records and a human-controlled one',
                            fighters=len(roster), expected=fighters, human_slots=humans_list,
                            characters=[f.get('character') for f in roster]))

    move = first('movement', lambda e: e.get('slot') in humans_set and e.get('distance', 0) > 0)
    checks.append(assertion('combat.movement', bool(move), 'a human fighter walked while a direction was held', event=move))

    attack = first('attack', lambda e: e.get('slot') in humans_set and
                   0 <= e.get('steps_since_press', -1) <= ATTACK_PRESS_STEPS and e.get('pressed'))
    checks.append(assertion('combat.attack', bool(attack),
                            'a hit by a human fighter resolved shortly after its action press', event=attack))

    damage = None
    for a in (e for e in mine if e.get('event') == 'attack' and e.get('slot') in humans_set
              and 0 <= e.get('steps_since_press', -1) <= ATTACK_PRESS_STEPS):
        damage = first('damage', lambda e: e.get('slot') == a.get('target_slot') and
                       0 <= e['step'] - a['step'] <= DAMAGE_AFTER_ATTACK_STEPS and
                       e.get('health_before', 0) > e.get('health_after', 0) >= 0)
        if damage:
            damage = dict(damage, attack_step=a['step'], attacker_slot=a['slot'])
            break
    checks.append(assertion('combat.damage', bool(damage),
                            "the target of a human fighter's hit lost health within a few steps", event=damage))

    if humans > 1:
        walked = sorted({e['slot'] for e in mine if e.get('event') == 'movement' and e.get('slot') in humans_set})
        landed = set()
        for a in (e for e in mine if e.get('event') == 'attack' and e.get('slot') in humans_set
                  and 0 <= e.get('steps_since_press', -1) <= ATTACK_PRESS_STEPS):
            if first('damage', lambda e: e.get('slot') == a.get('target_slot') and
                     0 <= e['step'] - a['step'] <= DAMAGE_AFTER_ATTACK_STEPS and
                     e.get('health_before', 0) > e.get('health_after', 0) >= 0):
                landed.add(a['slot'])
        checks.append(assertion('combat.two_players',
                                len(humans_list) == humans and walked == humans_list and sorted(landed) == humans_list,
                                'every pad-controlled fighter walked and landed a hit that did damage',
                                human_slots=humans_list, expected=humans, walked=walked, landed_hits=sorted(landed)))
    if not require_result:
        return checks
    decisive = [e for e in mine if e.get('event') == 'result' and e.get('decisive')]
    result = decisive[0] if decisive else None
    over = first('match_over')
    ok = bool(result) and len(decisive) == 1 and not result.get('draw')
    winners, losers = (result or {}).get('winners', []), (result or {}).get('losers', [])
    slots = {f['slot'] for f in roster}
    ok = ok and bool(winners) and bool(losers) and set(winners) | set(losers) <= slots and not set(winners) & set(losers)
    ok = ok and bool(over) and over.get('winner_slot') == winners[0] and over['step'] >= result['step']
    summary = {k: (result or {}).get(k) for k in ('step', 'code', 'time_up', 'winners', 'losers', 'clock')}
    summary['winner_is_human'] = bool(winners) and winners[0] in humans_set
    summary['loser_health'] = [f.get('health') for f in (result or {}).get('fighters', []) if f.get('slot') in losers]
    summary['decided_winner_slot'] = (over or {}).get('winner_slot')
    checks.append(assertion('combat.result', ok,
                            'one decisive result with a winner and a loser, confirmed by the decided phase', **summary))

    if fighters > 2:
        # A Free For All records one result per fighter put out (not decisive) and
        # a decisive one for the last. Seen live: codes 32, 32, then 9.
        recorded = [e for e in mine if e.get('event') == 'result' and e['step'] <= (result or {}).get('step', -1)]
        out = [slot for e in recorded for slot in e.get('losers', [])]
        final = {f['slot']: f.get('health') for f in (result or {}).get('fighters', [])}
        alive = sorted(slot for slot, health in final.items() if health and health > 0)
        good = (bool(result) and len(out) == fighters - 1 and len(set(out)) == len(out)
                and set(out) | set(winners) == slots and alive == sorted(winners)
                and all(e.get('winners') for e in recorded))
        checks.append(assertion('combat.eliminations', good,
                                'every fighter but the winner was put out once and ended at zero health',
                                order=out, steps=[e['step'] for e in recorded], codes=[e.get('code') for e in recorded],
                                alive_at_end=alive))
    # Line order in the log: the decisive result of this match, then the request.
    marker = position = -1
    for number, line in enumerate(log.splitlines()):
        if marker < 0 and '[TEST-EVENT] ' in line and re.search(r'"event":\s*"result"', line) \
                and re.search(r'"match":\s*%d\b' % (chosen or 0), line) and re.search(r'"decisive":\s*true', line):
            marker = number
        elif marker >= 0 and position < 0 and results_anchor in line.lower():
            position = number
    checks.append(assertion('combat.results_screen', bool(result) and 0 <= marker < position,
                            'front end requested the match summary after the result was recorded', anchor=results_anchor))
    return checks


def state_stream(log, max_step=1800, match=None):
    """The game-state telemetry of one match up to a fight step, in a comparable form.

    Every `[TEST-STATE]` and `[TEST-EVENT]` record of the match with `step` at
    most `max_step`, in log order, with the host clock removed and the fields
    sorted. Two runs of a fight whose input is step-timed and whose seeds are
    pinned should produce the same stream; the first record that differs says at
    which step the fight stopped being the same fight.
    """
    records = []
    for line in log.splitlines():
        for tag in ('[TEST-STATE] ', '[TEST-EVENT] '):
            if tag in line:
                try:
                    record = json.loads(line.split(tag, 1)[1])
                except ValueError:
                    continue
                record['tag'] = tag[1:-2]
                records.append(record)
    starts = sorted({r['match'] for r in records if r.get('event') == 'match_start' and isinstance(r.get('match'), int)})
    chosen = match if match is not None else (starts[0] if starts else None)
    out = []
    for record in records:
        if record.get('match') != chosen or not isinstance(record.get('step'), int) or record['step'] > max_step:
            continue
        if record.get('event') in ('input_loaded', 'input_error'):
            continue
        record.pop('host_ms', None)
        out.append(json.dumps(record, sort_keys=True, separators=(',', ':')))
    return out


def stream_checks(log, expected_lines, max_step=1800, match=None):
    """Compare this run's state stream with a recorded one, record by record."""
    actual = state_stream(log, max_step, match)
    expected = [line for line in expected_lines if line.strip()]
    if not expected:
        return [assertion('determinism.reference', False, 'the recorded stream is empty')]
    if not actual:
        return [dict(name='determinism.stream', status='blocked',
                     detail='no telemetry in this run to compare', metrics={})]
    for index, (mine, theirs) in enumerate(zip(actual, expected)):
        if mine != theirs:
            a, b = json.loads(mine), json.loads(theirs)
            fields = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
            return [assertion('determinism.stream', False, 'state stream differs from the recorded run',
                              first_difference_record=index, step=a.get('step'), recorded_step=b.get('step'),
                              event=a.get('event'), differing_fields=fields, records=len(actual), recorded=len(expected))]
    same_length = len(actual) == len(expected)
    return [assertion('determinism.stream', same_length,
                      'state stream identical to the recorded run' if same_length else
                      'streams agree but one is shorter: the runs covered different step ranges',
                      records=len(actual), recorded=len(expected), max_step=max_step,
                      sha256=hashlib.sha256('\n'.join(actual).encode()).hexdigest())]


PACING = re.compile(r'\[PACING\] (\d+) presents, interval ([\d.]+) / ([\d.]+) / ([\d.]+) ms \(min/mean/max\), '
                    r'median ([\d.]+), (\d+) late, sync interval (-?\d+)')


def pacing_checks(log, anchor='game.startgame(', end_anchor=None, target_ms=1000.0 / 60,
                  tolerance_ms=1.0, max_late_fraction=0.02, min_windows=15):
    """Frame pacing from the runtime's own two-second reports (RECOMP_PRESENT_PACING=1).

    Only windows after the anchor that presented at the full rate are judged: a
    loading screen or a 30 fps menu is not a pacing fault. `late` is the
    runtime's count of intervals more than half as long again as the window's
    median. Limits come from healthy Release fights on a 120 Hz display
    (median 16.67 ms, late presents well under 1 percent).
    """
    lines = log.splitlines()
    hit = next((i for i, line in enumerate(lines)
                if line.startswith('[FUNCCALL]') and anchor.lower() in line.lower()), None)
    stop = next((i for i, line in enumerate(lines) if hit is not None and i > hit and end_anchor
                 and line.startswith('[FUNCCALL]') and end_anchor.lower() in line.lower()), len(lines))
    windows = [m.groups() for line in lines[(hit or 0) + 1:stop] if (m := PACING.search(line))] if hit is not None else []
    full = [(int(n), float(lo), float(mean), float(hi), float(med), int(late))
            for n, lo, mean, hi, med, late, _ in windows if int(n) >= 100]
    if len(full) < min_windows:
        return [assertion('performance.pacing_evidence', False, 'too few full-rate two-second windows after the anchor',
                          windows=len(full), reports=len(windows))]
    medians = sorted(w[4] for w in full)
    median = medians[len(medians) // 2]
    presents = sum(w[0] for w in full)
    late = sum(w[5] for w in full)
    worst = max(w[3] for w in full)
    ordered = sorted(w[3] for w in full)
    p95_window_max = ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))]
    return [assertion('performance.pacing_evidence', True, 'full-rate two-second windows after the anchor', windows=len(full)),
            assertion('performance.frame_interval', abs(median - target_ms) <= tolerance_ms,
                      'median present interval across windows', median_ms=median, target_ms=round(target_ms, 2)),
            assertion('performance.late_presents', late / presents <= max_late_fraction,
                      'presents more than 1.5x their window median', late=late, presents=presents,
                      fraction=round(late / presents, 5), worst_interval_ms=worst, p95_window_max_ms=p95_window_max)]


def memory_checks(samples, start_seconds, max_growth_mb=64.0, min_samples=6):
    """Growth of the game's private bytes from the start of play to the end of the run.

    A fight streams assets for its first seconds, so the baseline is the highest
    of the first three samples after `start_seconds`. Growth is the last sample
    minus that. The limit is a guard against a leak that grows every second, not
    a tuned budget; one fight is too short to show a slow one.
    """
    play = [s for s in samples if s['seconds'] >= start_seconds]
    if len(play) < min_samples:
        return [assertion('performance.memory_evidence', False, 'too few memory samples during play', samples=len(play))]
    baseline = max(s['private_bytes'] for s in play[:3])
    growth = (play[-1]['private_bytes'] - baseline) / 1048576.0
    peak = max(s['private_bytes'] for s in play) / 1048576.0
    return [assertion('performance.memory_growth', growth <= max_growth_mb,
                      'private bytes at the end of play against the start of play',
                      growth_mb=round(growth, 1), limit_mb=max_growth_mb, baseline_mb=round(baseline / 1048576.0, 1),
                      peak_mb=round(peak, 1), samples=len(play), seconds=play[-1]['seconds'] - play[0]['seconds'])]


def environment_identity():
    """The machine a report came from: OS, CPU, GPU and driver. Best effort."""
    import platform
    import subprocess
    info = {'os': platform.platform(), 'machine': platform.machine(), 'processor': platform.processor()}
    if os.name == 'nt':
        try:
            out = subprocess.run(['powershell', '-NoProfile', '-Command',
                                  'Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentHorizontalResolution } | '
                                  'Select-Object Name, DriverVersion, CurrentHorizontalResolution, CurrentVerticalResolution, '
                                  'CurrentRefreshRate | ConvertTo-Json -Compress'],
                                 capture_output=True, text=True, timeout=30).stdout.strip()
            gpus = json.loads(out) if out else []
            info['display_adapters'] = gpus if isinstance(gpus, list) else [gpus]
        except (OSError, ValueError, subprocess.SubprocessError) as ex:
            info['display_adapters_error'] = str(ex)
    return info


def write_report(folder, report):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    checks = report['assertions']
    # A blocked required check is an error, never a green JUnit skip.
    root = ET.Element('testsuite', name=report['name'], tests=str(len(checks)),
                      failures=str(sum(c['status'] == 'fail' for c in checks)),
                      errors=str(sum(c['status'] == 'blocked' for c in checks)),
                      skipped=str(sum(c['status'] == 'skip' for c in checks)),
                      time=str(report.get('seconds', 0)))
    for c in checks:
        node = ET.SubElement(root, 'testcase', name=c['name'], time=str(c.get('seconds', 0)))
        if c['status'] == 'skip':
            ET.SubElement(node, 'skipped', message=c['detail'])
        elif c['status'] != 'pass':
            ET.SubElement(node, 'error' if c['status'] == 'blocked' else 'failure', message=c['detail'])
        ET.SubElement(node, 'system-out').text = json.dumps(c.get('metrics', {}), sort_keys=True)
    ET.ElementTree(root).write(folder / 'junit.xml', encoding='utf-8', xml_declaration=True)
    (folder / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True), encoding='utf-8')
    escape = lambda s: html.escape(str(s))
    rows = ''.join('<tr><td>' + escape(c['name']) + '</td><td>' + escape(c['status']) +
                   '</td><td>' + escape(c['detail']) + '</td><td><pre>' +
                   escape(json.dumps(c.get('metrics', {}), indent=2)) + '</pre></td></tr>' for c in checks)
    def asset(p):
        resolved = Path(p).resolve()
        try:
            return resolved.relative_to(folder.resolve()).as_posix()
        except ValueError:
            return resolved.as_uri()
    images = ''.join('<figure><img style="max-width:640px" src="' + escape(asset(p)) +
                     '"><figcaption>' + escape(Path(p).name) + '</figcaption></figure>' for p in report.get('images', []))
    audios = ''.join('<p>' + escape(Path(p).name) + '<br><audio controls src="' + escape(asset(p)) + '"></audio></p>'
                     for p in report.get('audio', []))
    page = '<!doctype html><meta charset="utf-8"><title>Game test report</title>' + \
           '<style>body{font:15px system-ui;margin:30px}td{padding:8px;border:1px solid #ccc}pre{white-space:pre-wrap}</style>' + \
           '<h1>' + escape(report['name']) + '</h1><p>Selected evidence gates; complete gameplay acceptance is not established.</p>' + \
           '<p><a href="report.json">JSON</a> · <a href="junit.xml">JUnit</a></p>' + \
           '<h2>Scope</h2><pre>' + escape(json.dumps(report.get('scope', {}), indent=2)) + '</pre><table>' + rows + '</table>' + images + audios + \
           '<h2>Observations</h2><pre>' + escape(json.dumps(report.get('observations', {}), indent=2)) + '</pre>' + \
           '<h2>Identity</h2><pre>' + escape(json.dumps(report.get('identity', {}), indent=2)) + '</pre>'
    (folder / 'index.html').write_text(page, encoding='utf-8')
    return all(c['status'] == 'pass' for c in checks) and bool(checks)
