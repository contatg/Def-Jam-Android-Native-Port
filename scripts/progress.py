#!/usr/bin/env python3
"""Read PROGRESS.md in pieces, so a session does not have to load all of it.

    python scripts/progress.py brief                 status table, open questions, last work-log entries, hand-off
    python scripts/progress.py section 7             one section, whole (0..7); `section 6` is the work log
    python scripts/progress.py decision D63 D81      those rows of the decisions log, whole
    python scripts/progress.py decisions             every decision, one line each (id, date, start of the text)
    python scripts/progress.py decisions --grep updater --last 10
    python scripts/progress.py grep "Terrordome" --max 20          matching lines, numbered
    python scripts/progress.py --file other.md ...   read another file with the same layout

PROGRESS.md is still the single source of truth and is still what gets edited and exported; this
only reads it. The decisions log is most of its size (D1..D104, never renumbered), and the rows
are read by number or by keyword. Standard library only.
"""
import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = os.path.join(REPO, 'PROGRESS.md')
SECTION_RE = re.compile(r'^## (\d+)\. ', re.M)
ROW_RE = re.compile(r'^\| (D\d+) \| ([^|]*) \| (.*)$')


def load(path):
    try:
        with open(path, encoding='utf-8', newline='') as f:
            return f.read().replace('\r\n', '\n')
    except OSError as ex:
        raise SystemExit('cannot read %s: %s' % (path, ex))


def sections(text):
    """{number: text of the section, heading included}."""
    marks = [(int(m.group(1)), m.start()) for m in SECTION_RE.finditer(text)]
    out = {}
    for i, (num, start) in enumerate(marks):
        end = marks[i + 1][1] if i + 1 < len(marks) else len(text)
        out[num] = text[start:end].rstrip('\n') + '\n'
    return out


def decision_rows(text):
    """[(id, date, whole row)] from section 1, in file order."""
    rows = []
    for line in sections(text).get(1, '').splitlines():
        m = ROW_RE.match(line)
        if m:
            rows.append((m.group(1), m.group(2).strip(), line))
    return rows


def cut(s, n):
    s = ' '.join(s.split())
    return s if len(s) <= n else s[:n - 1] + '…'


def status_lines(section0, only_open=True):
    """Rows of the status table. By default only those not finished: ⏳, ⬜ or a mixed state."""
    out, done = [], 0
    for line in section0.splitlines():
        if line.startswith('|') and not line.startswith('|---') and not line.startswith('| Area'):
            cells = [c.strip() for c in line.strip('|').split('|')]
            if len(cells) >= 2:
                if only_open and cells[1].startswith('✅') and '⏳' not in cells[1] and '⬜' not in cells[1]:
                    done += 1
                    continue
                note = cells[2] if len(cells) > 2 else ''
                out.append('- %s: %s%s' % (cut(cells[0], 60), cells[1], ('  ' + cut(note, 110)) if note else ''))
    if done:
        out.append('(%d rows are done, ✅; `section 0` lists them)' % done)
    return out


def bullets(section_text, width):
    out, cur = [], []
    for line in section_text.splitlines()[1:]:
        if line.startswith('- '):
            if cur:
                out.append(cut(' '.join(cur), width))
            cur = [line[2:]]
        elif line.strip() and cur:
            cur.append(line.strip())
    if cur:
        out.append(cut(' '.join(cur), width))
    return out


def cmd_brief(text, args):
    s = sections(text)
    print('== 0. Status at a glance: rows not finished (notes cut; `section 0` for all) ==')
    print('\n'.join(status_lines(s.get(0, ''))))
    print('\n== 5. Open questions (first 220 characters; `section 5` for all) ==')
    for b in bullets(s.get(5, ''), 220):
        print('- ' + b)
    print('\n== 6. Work log, latest 3 (first 260 characters; `section 6` for all) ==')
    for b in bullets(s.get(6, ''), 260)[-3:]:
        print('- ' + b)
    print('\n== 7. Hand-off ==')
    print(s.get(7, '(no section 7)').rstrip())
    print('\nDecisions are read by number or keyword: `decision D<n>`, `decisions --grep TEXT`.')


def cmd_section(text, args):
    s = sections(text)
    for n in args.numbers:
        if n not in s:
            raise SystemExit('no section %s (have %s)' % (n, ', '.join(map(str, sorted(s)))))
        print(s[n].rstrip())


def cmd_decision(text, args):
    rows = decision_rows(text)
    missing = []
    for want in args.ids:
        key = want.upper() if want.upper().startswith('D') else 'D' + want
        found = [row for i, _, row in rows if i == key]      # a number can have more than one row (D100, D101)
        if found:
            print('\n'.join(found))
        else:
            missing.append(want)
    if missing:
        raise SystemExit('no such decision: ' + ', '.join(missing))


def cmd_decisions(text, args):
    rows = decision_rows(text)
    if args.grep:
        rx = re.compile(args.grep, re.I)
        rows = [r for r in rows if rx.search(r[2])]
    if args.last:
        rows = rows[-args.last:]
    for i, date, row in rows:
        body = row.split('|')[3] if row.count('|') >= 4 else row
        print('%s  %s  %s' % (i, date, cut(body, args.width)))
    print('-- %d decision(s); `decision %s` prints one whole' % (len(rows), rows[0][0] if rows else 'D1'))


def cmd_grep(text, args):
    rx = re.compile(args.pattern, re.I)
    shown = total = 0
    for n, line in enumerate(text.splitlines(), 1):
        if rx.search(line):
            total += 1
            if shown < args.max:
                print('%d: %s' % (n, cut(line, 240)))
                shown += 1
    print('-- %d matching lines%s' % (total, '; %d not shown (--max)' % (total - shown) if total > shown else ''))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--file', default=DEFAULT)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('brief').set_defaults(fn=cmd_brief)
    p = sub.add_parser('section')
    p.add_argument('numbers', nargs='+', type=int)
    p.set_defaults(fn=cmd_section)
    p = sub.add_parser('decision')
    p.add_argument('ids', nargs='+')
    p.set_defaults(fn=cmd_decision)
    p = sub.add_parser('decisions')
    p.add_argument('--grep')
    p.add_argument('--last', type=int)
    p.add_argument('--width', type=int, default=120)
    p.set_defaults(fn=cmd_decisions)
    p = sub.add_parser('grep')
    p.add_argument('pattern')
    p.add_argument('--max', type=int, default=30)
    p.set_defaults(fn=cmd_grep)
    args = ap.parse_args(argv)
    try:
        args.fn(load(args.file), args)
    except BrokenPipeError:      # `| head`
        try:
            sys.stdout.close()
        except OSError:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
