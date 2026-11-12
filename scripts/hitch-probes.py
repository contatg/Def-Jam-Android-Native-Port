#!/usr/bin/env python3
"""Read the hitch probe's samples: what every thread was doing while a frame was late.

With RECOMP_HITCH_PROBE=1 (and the recorder on) the runtime samples the process when no
frame has been presented for 60 ms, and again every 50 ms while the frame stays late (six
times at most): which guest thread holds the guest CPU, and the native stack of every
thread. The holder goes into the frame's `hitch` event, everything into the session's
stalls.jsonl (src/hooks/recorder.c, guest_turn_probe in the toolkit's win32_compat.c).

    python scripts/hitch-probes.py <session or scene-perf run folder> [...]
    python scripts/hitch-probes.py <folder> --frame 1234 --threads   every stack of one frame

For each late frame it prints one line: how late it was, the game thread (the guest thread
that runs the title's main loop) and where it waits, the executor thread (nv2a_ack_thread,
which runs the push buffer and the renderer) and what it is doing, who held the guest CPU,
and whether the game read streamed data ([READ]) in that frame. --summary counts the
causes. Stacks name functions by their exported symbol; a frame in the program also
carries `exe+0x...`, which `atos -o build/<preset>/defjam_recomp -l 0x100000000
0x10<offset>` turns into a file and line (dladdr cannot see static functions).

Darwin only for now (Windows and Linux pin guest threads to a CPU and have no token).
"""

import argparse
import collections
import csv
import json
import re
import sys
from pathlib import Path

LIB = re.compile(r'^[^`"]*`')


def name(frame):
    """A frame's function, without its library, offset or program offset."""
    f = frame.split(' exe+')[0]
    f = LIB.sub('', f)
    return re.sub(r'\+0x[0-9a-f]+$', '', f)


def sessions(paths):
    for p in paths:
        p = Path(p)
        if (p / 'stalls.jsonl').exists():
            yield p
        else:
            for s in sorted(p.rglob('stalls.jsonl')):
                yield s.parent


def read_jsonl(path):
    out = []
    if path.exists():
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            try:
                out.append(json.loads(line))
            except ValueError:
                try:        # an early build wrote an empty frame for a zero return address
                    out.append(json.loads(line.replace(',,', ',').replace(',]', ']')))
                except ValueError:
                    pass
    return out


def role(t):
    s = [name(f) for f in t['stack']]
    if 'nv2a_ack_thread' in s:
        return 'executor'
    if t.get('guest'):
        if 'kernel_timer_thread' in s:
            return 'timer'
        return 'game' if not t.get('start') else 'guest'
    return 'host'


def executor_activity(t):
    s = [name(f) for f in t['stack']]
    joined = ' '.join(s)
    if 'shader_make' in s:
        return 'shader compile (shaderc)'
    if 'createPipelines' in joined or 'vkCreateGraphicsPipelines' in joined or 'MVKGraphicsPipeline' in joined:
        return 'pipeline create (MoltenVK/Metal)'
    if 'flip_pace' in s:
        return 'vsync pacing (flip_pace)'
    exe = [n for n, f in zip(s, t['stack']) if ' exe+' in f or n.startswith(('sub_', 'nv2a', 'pb_', 'd3d', 'vk'))]
    top = exe[0] if exe else (s[0] if s else '?')
    lib = s[0] if s else '?'
    return f'{top} ({lib})' if lib != top else top


def guest_site(t):
    s = [name(f) for f in t['stack']]
    sub = next((n for n in s if n.startswith('sub_') and not n.endswith('_enter')), '')
    host = next((n for n, f in zip(s, t['stack']) if ' exe+' in f and not n.startswith('sub_')), s[0] if s else '?')
    enter = next((n for n in s if n.endswith('_enter')), '')
    where = enter or host
    return f"{t.get('guest')} {t.get('state_ms', 0)}ms {where} in {sub} (call site {t.get('kernel_caller', '-')})"


def frame_reads(folder, frames, frame):
    """[READ] lines the game logged during `frame`, from the log's byte marks in frames.csv."""
    if frame not in frames or frame - 1 not in frames:
        return None
    sess = folder / 'session.json'
    try:
        log = json.loads(sess.read_text(encoding='utf-8')).get('log')
    except (OSError, ValueError):
        return None
    lo, hi = frames[frame - 1], frames[frame]
    if not log or lo < 0 or hi < lo or not Path(log).exists():
        return None
    with open(log, 'rb') as f:
        f.seek(lo)
        return f.read(hi - lo).count(b'[READ]')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folders', nargs='+')
    ap.add_argument('--frame', type=int, help='only this frame')
    ap.add_argument('--threads', action='store_true', help='print every thread\'s stack')
    ap.add_argument('--summary', action='store_true', help='count late frames by what the executor and game were doing')
    a = ap.parse_args()

    causes = collections.Counter()
    stats = collections.Counter()
    for folder in sessions(a.folders):
        probes = read_jsonl(folder / 'stalls.jsonl')
        hitches = {e.get('frame'): e for e in read_jsonl(folder / 'events.jsonl') if e.get('kind') == 'hitch'}
        rows = {}
        if (folder / 'frames.csv').exists():
            with open(folder / 'frames.csv', newline='') as f:
                for r in csv.DictReader(f):
                    try:
                        rows[int(r['frame'])] = (float(r['interval_ms']), int(r['log_bytes']))
                    except (TypeError, ValueError):
                        pass        # a row cut short when the game was stopped
        log_marks = {k: v[1] for k, v in rows.items()}
        by_frame = collections.OrderedDict()
        for p in probes:
            by_frame.setdefault(p['frame'], []).append(p)
        print(f'## {folder}: {len(probes)} probes in {len(by_frame)} late frames')
        for frame, ps in by_frame.items():
            if a.frame is not None and frame != a.frame:
                continue
            interval = rows.get(frame, (None,))[0]
            reads = frame_reads(folder, log_marks, frame)
            head = (f"frame {frame} t={ps[0]['t_ms'] / 1000:.2f}s lasted "
                    f"{interval:.0f} ms" if interval else f"frame {frame} t={ps[0]['t_ms'] / 1000:.2f}s")
            head += f", {len(ps)} probes, [READ] {reads if reads is not None else '?'}"
            if frame in hitches:
                head += ' (hitch)'
            print(head)
            prev = None
            for p in ps:
                threads = p['all']['threads'] if p.get('all') else []
                cpu = {role(t): t['cpu_ms'] for t in threads if role(t) in ('game', 'executor')}
                game = [t for t in threads if role(t) == 'game']
                ex = [t for t in threads if role(t) == 'executor']
                hold = p.get('holder') or {}
                line = f"  +{p['since_ms']:.0f}ms"
                if 'prober_gap_ms' in p:
                    line += f" (prober slept {p['prober_gap_ms']:.0f} ms for 4)"
                if prev:
                    dt = p['t_ms'] - prev[0]
                    line += ' cpu ' + ', '.join(f"{k} {cpu[k] - prev[1][k]}/{dt:.0f} ms"
                                                for k in sorted(cpu) if k in prev[1])
                prev = (p['t_ms'], cpu)
                line += f" | game: {guest_site(game[0]) if game else '?'}"
                line += f" | executor: {executor_activity(ex[0]) + ' (' + ex[0]['run'] + ')' if ex else '?'}"
                if hold.get('stack'):
                    hs = [name(f) for f in hold['stack']]
                    hsub = next((n for n in hs if n.startswith('sub_')), '?')
                    line += f" | holder: {hold.get('start', 'game' if hold.get('start') is None else '')} in {hsub} ({hs[0]})"
                else:
                    line += ' | holder: none'
                for t in game + ex:
                    if 'pri' in t:
                        line += f" | {role(t)} pri {t['pri']}{' depressed' if t.get('depressed') else ''} {t['run']}"
                gpu = (p.get('all') or {}).get('gpu')
                if gpu:
                    line += (f" | gpu: backlog {gpu['backlog']}, swm wait {gpu['swm_wait_ms']} ms,"
                             f" queued {gpu['swm_queued']}, intr {gpu['pgraph_intr']}, servicing {gpu['servicing']},"
                             f" fence {gpu['fence']}/{gpu['latest']} (released {gpu['released']}, by {gpu['fence_by']})")
                print(line)
                if a.threads:
                    for t in threads:
                        print(f"    [{role(t)}] {t['name'] or '-'} {t.get('guest', '')} {t['run']} cpu {t['cpu_ms']} ms")
                        for f in t['stack']:
                            print(f'        {f}')
            first = ps[0]
            ex = [t for t in (first['all'] or {}).get('threads', []) if role(t) == 'executor']
            game = [t for t in (first['all'] or {}).get('threads', []) if role(t) == 'game']
            key = (executor_activity(ex[0]).split(' (')[0] if ex else '?',
                   ('game waits: ' + next((name(f) for f in game[0]['stack'] if name(f).endswith('_enter') or
                                           (' exe+' in f and not name(f).startswith('sub_'))), '?')) if game else '?')
            causes[key] += 1
            gaps = [q.get('prober_gap_ms', 0) for q in ps]
            stats['late frames'] += 1
            if max(gaps) >= 20:
                stats['the prober itself ran 20+ ms late (the process was not scheduled)'] += 1
            if game and any(name(f).endswith('_enter') and 'sub_0021EB90' in name(f) for f in game[0]['stack']):
                stats["game in Direct3D's fence hold at the first probe"] += 1
            if ex and executor_activity(ex[0]).startswith(('shader compile', 'pipeline create')):
                stats['executor compiling a shader or pipeline'] += 1
            if game and game[0].get('depressed'):
                stats['game thread priority-depressed (a yield)'] += 1
    if a.summary:
        print('\n## Late frames')
        for k, n in stats.items():
            print(f'{n:4}  {k}')
        print('\n## Late frames by first probe (executor activity, game thread)')
        for (ex, g), n in causes.most_common():
            print(f'{n:4}  {ex:40}  {g}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
