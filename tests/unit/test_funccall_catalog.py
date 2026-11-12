"""scripts/funccall-catalog.py on a synthetic screens archive (no game data)."""
import importlib.util
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('funccall_catalog', ROOT / 'scripts/funccall-catalog.py')
cat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cat)


def big(entries):
    """A BIG4 archive of (name, bytes)."""
    names = b''.join(struct.pack('>II', 0, 0) + n.encode() + b'\0' for n, _ in entries)
    header = 16 + len(names)
    out, table, pos = b'', b'', header
    for n, data in entries:
        table += struct.pack('>II', pos, len(data)) + n.encode() + b'\0'
        out += data
        pos += len(data)
    return b'BIG4' + struct.pack('<I', pos) + struct.pack('>II', len(entries), header) + table + out


def refpack_literal(data):
    """RefPack with literal runs only: what the decoder must read back unchanged."""
    while len(data) % 4:
        data += b'\0'
    out = b'\x10\xfb' + len(data).to_bytes(3, 'big')
    for i in range(0, len(data), 112):
        chunk = data[i:i + 112]
        out += bytes([0xE0 + len(chunk) // 4 - 1]) + chunk
    return out + b'\xfc'


APT = b'Apt Data\0\0Story.PlayScene(\0Game.StartStoryMode()\0assets/cinema/sc05\0hello\0'


def test_commands_keywords_and_screens_from_nested_archives(tmp_path):
    inner = big([('crib.apt', APT), ('crib.const', b'Apt constant file\0Control.GetScreenInfo(\0')])
    plain = big([('menu.apt', b'Options.SetVolume(\0')])
    outer = big([('screens/crib.big', refpack_literal(inner)), ('screens/menu.big', plain)])
    extracted = tmp_path / 'extracted'
    (extracted / 'screens').mkdir(parents=True)
    (extracted / 'screens' / 'screens.viv').write_bytes(outer)
    (extracted / 'assets' / 'cinema').mkdir(parents=True)
    (extracted / 'assets' / 'cinema' / 'sc05.viv').write_bytes(b'')
    c = cat.catalog(extracted, cat.KEYWORDS)
    assert set(c['commands']) == {'Story.PlayScene(', 'Game.StartStoryMode()', 'Control.GetScreenInfo(',
                                  'Options.SetVolume('}
    assert c['commands']['Story.PlayScene('] == {'screens/crib.big'}
    assert 'assets/cinema/sc05' in c['keywords']
    assert c['cinema'] == ['sc05.viv']
    assert any('no ' in n and 'default.xbe' in n for n in c['notes'])
    md = cat.markdown(c)
    assert '### Story' in md and '`Story.PlayScene(` (screens/crib.big)' in md


def test_classify_and_runs():
    assert cat.ascii_runs(b'\x00abc\x00defgh\x01ij') == ['defgh']
    cmds, hits = cat.classify(['Game.X()', 'not a command', 'Story.Cinema(', 'x.Y'], ('cinema',))
    assert cmds == ['Game.X()', 'Story.Cinema(']
    assert hits == ['Story.Cinema(']


def test_an_undecodable_entry_is_reported_not_fatal():
    outer = big([('bad.big', b'\x10\xfb\x00\x00\x10\xff')])
    assert list(cat.movies(outer))[0][0] == 'bad.big'
