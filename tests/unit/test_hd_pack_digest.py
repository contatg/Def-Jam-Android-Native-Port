"""hd-pack-digest.py: the pixel digest ignores PNG compression and sees any pixel change."""
from pathlib import Path
import subprocess
import sys

import pytest

Image = pytest.importorskip('PIL.Image')
SCRIPT = Path(__file__).parents[2] / 'scripts/hd-pack-digest.py'


def digest(*packs):
    out = subprocess.run([sys.executable, SCRIPT, '--workers', '1', *packs], check=True,
                         capture_output=True, text=True).stdout
    return dict(line.rsplit(' ', 1) for line in out.splitlines())


def pack(root, name, level, pixel=(10, 20, 30, 40)):
    folder = root / name / 'textures'
    folder.mkdir(parents=True)
    image = Image.new('RGBA', (16, 8), (200, 100, 50, 255))
    image.putpixel((3, 4), pixel)
    image.save(folder / ('%064x.png' % 1), compress_level=level)
    Image.new('RGBA', (4, 4), (1, 2, 3, 4)).save(folder / ('%064x.png' % 2), compress_level=level)
    return root / name


def test_same_pixels_with_other_compression_give_the_same_digest(tmp_path):
    fast, small = pack(tmp_path, 'a', 0), pack(tmp_path, 'b', 9)
    one, other = digest(fast), digest(small)
    assert one['images'] == other['images'] == '2'
    assert one['file bytes'] != other['file bytes']
    assert one['pixel digest'] == other['pixel digest']


def test_one_changed_pixel_changes_the_digest(tmp_path):
    assert digest(pack(tmp_path, 'a', 6))['pixel digest'] != digest(pack(tmp_path, 'b', 6, (10, 20, 30, 41)))['pixel digest']
