"""Ordered log-event session driver using the toolkit's existing live-pad file.

Repeated screen anchors must occur again after the prior phase; old log hits
cannot complete a later phase. Timing is host seconds, not simulation frames.
Recipes need live calibration before being acceptance gates.
"""
import math
from pathlib import Path
import re

BUTTON = re.compile(r'(?:p[1-4]-)?(?:up|down|left|right|start|back|a|b|x|y|black|white|lt|rt|ls|rs|lleft|lright|lup|ldown|rleft|rright|rup|rdown)\Z')


class SessionDriver:
    def __init__(self, plan, live_path):
        if plan.get('input_clock') != 'host-seconds':
            raise ValueError('simulation-frame input is not implemented; recipe must say host-seconds')
        self.phases = [dict(p) for p in plan['phases']]
        if not self.phases:
            raise ValueError('session needs phases')
        names = [p.get('name') for p in self.phases]
        if len(set(names)) != len(names):
            raise ValueError('phase names must be unique')
        self.live_path = Path(live_path)
        self.live_path.write_text('', encoding='ascii')
        for phase in self.phases:
            if not phase.get('name') or not phase.get('anchor'):
                raise ValueError('every phase needs a name and anchor')
            if not math.isfinite(phase.get('after', 3)) or phase.get('after', 3) < 0:
                raise ValueError('invalid phase dwell')
            commands = []
            for command in phase.get('commands', []):
                if not BUTTON.fullmatch(command['button']):
                    raise ValueError('unsupported live button')
                if not math.isfinite(command['at']) or command['at'] < 0 or not 1 <= command.get('hold_ms', 300) <= 10000:
                    raise ValueError('invalid command time/hold')
                repeat = command.get('repeat', {})
                count, interval = repeat.get('count', 1), repeat.get('interval', 0)
                if type(count) is not int or not 1 <= count <= 2000 or not math.isfinite(interval) or interval < 0:
                    raise ValueError('invalid command repeat')
                if count > 1 and interval * 1000 <= command.get('hold_ms', 300) + 50:
                    raise ValueError('repeated presses need a release gap')
                commands += [dict(command, at=command['at'] + i * interval) for i in range(count)]
            if len(commands) > 10000:
                raise ValueError('too many session commands')
            # Copy phase dictionaries: the original recipe identity must remain
            # unchanged in reports even after expanding repeated presses.
            if any(not math.isfinite(c['at']) for c in commands):
                raise ValueError('repeated command time overflow')
            phase['commands'] = commands
        self.cursor = self.index = 0
        self.started = None
        self.sent = set()
        self.observed = []
        self.complete = False

    def poll(self, tail, elapsed):
        while self.cursor < len(tail.calls):
            call = tail.calls[self.cursor]
            self.cursor += 1
            candidate = self.index if self.started is None else self.index + 1
            if candidate >= len(self.phases):
                continue
            if self.phases[candidate]['anchor'].lower() in call.lower():
                self.index, self.started, self.sent = candidate, elapsed, set()
                self.observed.append({'phase': self.phases[candidate]['name'], 'seconds': elapsed,
                                      'call': call, 'commands_sent': []})
        if self.started is None:
            return False
        phase = self.phases[self.index]
        for i, command in enumerate(phase.get('commands', [])):
            if i not in self.sent and elapsed - self.started >= command['at']:
                # The runtime timestamps each complete line when sampled.
                with self.live_path.open('a', encoding='ascii') as f:
                    f.write('%s:%d\n' % (command['button'], command.get('hold_ms', 300)))
                self.sent.add(i)
                self.observed[-1]['commands_sent'].append({'index': i, 'seconds': elapsed})
        after = phase.get('after', 3)
        last_command = max((c['at'] for c in phase.get('commands', [])), default=0)
        self.complete = self.index == len(self.phases) - 1 and elapsed - self.started >= last_command + after
        return self.complete

    def assertions(self):
        seen = {o['phase'] for o in self.observed}
        return [dict(name='session.' + p['name'], status='pass' if p['name'] in seen else 'fail',
                     detail='ordered fresh screen event; not combat-state proof',
                     metrics={'anchor': p['anchor']}) for p in self.phases] + [
            dict(name='session.inputs_emitted',
                 status='pass' if all(len(o['commands_sent']) == len(self.phases[i].get('commands', []))
                                      for i, o in enumerate(self.observed)) else 'fail',
                 detail='live commands emitted; runtime sampling is not asserted', metrics={}),
            dict(name='session.completed', status='pass' if self.complete else 'fail',
                 detail='final checkpoint dwell completed', metrics={'observed': self.observed})]
