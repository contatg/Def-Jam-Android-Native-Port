#!/usr/bin/env python3
"""List every command the front end can send the game, to find the one that starts a scene.

The menus reach the game only through FuncCall strings such as
"Game.StartStoryMode()" (src/recomp_manual.c, funccall_hook), and
RECOMP_FE_INJECT (src/hooks/fe_inject.c) can send any of them at a chosen moment.
Which command plays a Story scene, and what it takes, is the open question in
docs/research/scenario-launching.md. The answer is in the dump: the screens'
compiled ActionScript holds the command strings, and the game's image holds the
method names its dispatcher accepts. This lists both, offline:

    python scripts/funccall-catalog.py                       Markdown to logs/funccall-catalog.md
    python scripts/funccall-catalog.py --grep cinema --grep scene

  commands     strings shaped like Module.Method or Module.Method( from every
               movie in screens/screens.viv, with the screens that hold them
  keywords     any string in those movies mentioning cinema, scene, movie, cutscene,
               story, nis, intro or extro (or --grep words)
  image        the same words in default.xbe, with the guest address of each
               (grep the lifted C for that address to find who uses it)
  cinema       the scene files under assets/cinema

Read from $DEFJAM_DATA/extracted. The output names strings and files of the
user's own dump: it stays under logs/, never in the repository.
"""
import argparse
import importlib.util
import os
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import refpack  # noqa: E402  decompress(), big_entries()

CMD_RE = re.compile(r'^[A-Z][A-Za-z0-9_]{1,40}\.[A-Za-z][A-Za-z0-9_]{0,60}(?:\(.*)?$')
KEYWORDS = ('cinema', 'scene', 'movie', 'cutscene', 'story', 'nis', 'intro', 'extro', 'cine')


def ascii_runs(data, minimum=4):
    """NUL- or control-delimited printable ASCII strings of at least `minimum` characters."""
    return [m.group().decode('ascii') for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minimum, data)]


def is_refpack(blob):
    return len(blob) > 2 and (blob[0] & 0x3E) == 0x10 and blob[1] == 0xFB


def movies(archive_bytes):
    """(entry name, bytes of each file inside it) for every entry of an outer BIG archive,
    decompressing RefPack entries and opening inner BIG archives."""
    table = refpack.big_entries(archive_bytes) or []
    for name, off, size in table:
        blob = archive_bytes[off:off + size]
        if is_refpack(blob):
            try:
                blob = refpack.decompress(blob)
            except (Exception, SystemExit):     # a corrupt or unexpected entry is reported, not fatal
                yield name, [('(undecodable)', b'')]
                continue
        inner = refpack.big_entries(blob)
        if inner:
            yield name, [(n, blob[o:o + s]) for n, o, s in inner]
        else:
            yield name, [(name, blob)]


def classify(strings, words=KEYWORDS):
    commands = sorted({s.strip() for s in strings if CMD_RE.match(s.strip())})
    low = [w.lower() for w in words]
    hits = sorted({s.strip() for s in strings if any(w in s.lower() for w in low)})
    return commands, hits


def load_string_va():
    spec = importlib.util.spec_from_file_location('string_va', HERE / 'string-va.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def image_hits(xbe, words):
    sva = load_string_va()
    _, secs = sva.sections(xbe)
    low = [w.lower() for w in words]
    out = []
    for m in re.finditer(rb'[\x20-\x7e]{5,}', xbe):
        s = m.group().decode('ascii')
        if any(w in s.lower() for w in low) or CMD_RE.match(s):
            out.append((sva.to_va(secs, m.start()), s))
    return out


def catalog(extracted, words):
    extracted = Path(extracted)
    commands = defaultdict(set)
    keyword_hits = defaultdict(set)
    screens = extracted / 'screens' / 'screens.viv'
    notes = []
    if screens.exists():
        data = screens.read_bytes()
        for entry, files in movies(data):
            strings = []
            for _, blob in files:
                strings += ascii_runs(blob)
            cmds, hits = classify(strings, words)
            for c in cmds:
                commands[c].add(entry)
            for h in hits:
                keyword_hits[h].add(entry)
    else:
        notes.append('no %s' % screens)
    image = []
    xbe = extracted / 'default.xbe'
    if xbe.exists():
        image = image_hits(xbe.read_bytes(), words)
    else:
        notes.append('no %s' % xbe)
    cinema = sorted(p.name for p in (extracted / 'assets' / 'cinema').glob('*')) \
        if (extracted / 'assets' / 'cinema').is_dir() else []
    return {'commands': commands, 'keywords': keyword_hits, 'image': image, 'cinema': cinema, 'notes': notes}


def markdown(c):
    L = ['# Front-end commands and scene leads', '',
         'From the user\'s own dump; keep this file local. Send a command with '
         '`RECOMP_FE_INJECT="<anchor>#1=><command>"` or `scripts/scenarios.py cutscene --inject`.', '']
    for n in c['notes']:
        L.append('> ' + n)
    by_module = defaultdict(list)
    for cmd, where in c['commands'].items():
        by_module[cmd.split('.', 1)[0]].append((cmd, where))
    L += ['## Commands by module', '']
    for module in sorted(by_module):
        L.append('### %s' % module)
        for cmd, where in sorted(by_module[module]):
            L.append('- `%s` (%s)' % (cmd, ', '.join(sorted(where)[:4]) + (' ...' if len(where) > 4 else '')))
        L.append('')
    L += ['## Keyword strings in the screens', '']
    for s, where in sorted(c['keywords'].items()):
        L.append('- `%s` (%s)' % (s[:120], ', '.join(sorted(where)[:3])))
    L += ['', '## Keyword strings in default.xbe', '', '| guest address | string |', '|---|---|']
    for va, s in c['image']:
        L.append('| %s | `%s` |' % ('0x%08X' % va if va is not None else '-', s[:120].replace('|', '/')))
    L += ['', '## Scene files (assets/cinema)', '', ', '.join(c['cinema']) or '(none found)', '']
    return '\n'.join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--data', default=os.environ.get('DEFJAM_DATA'), help='the data folder (default $DEFJAM_DATA)')
    ap.add_argument('--out', default=str(REPO / 'logs' / 'funccall-catalog.md'))
    ap.add_argument('--grep', action='append', help='extra words to look for (adds to the defaults)')
    a = ap.parse_args(argv)
    if not a.data:
        raise SystemExit('set DEFJAM_DATA or pass --data')
    words = KEYWORDS + tuple(a.grep or ())
    c = catalog(Path(a.data) / 'extracted', words)
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(markdown(c), encoding='utf-8')
    print('%d command(s) in %d module(s), %d keyword string(s), %d in the image, %d scene file(s) -> %s' % (
        len(c['commands']), len({k.split('.', 1)[0] for k in c['commands']}), len(c['keywords']),
        len(c['image']), len(c['cinema']), out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
