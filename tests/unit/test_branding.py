"""The application icons and Steam artwork (D98): the approved set is exactly what is tracked, the
files have the structure the platforms read, and the generator rebuilds all of it from the masters."""
import hashlib
import importlib.util
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BRANDING = ROOT / 'branding'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tree = load('branding_source_tree', ROOT / 'scripts/check-source-tree.py')


def sums():
    lines = (BRANDING / 'SHA256SUMS').read_text(encoding='utf-8').splitlines()
    return {line.split('  ', 1)[1]: line.split('  ', 1)[0] for line in lines}


def png_size(path):
    data = Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    return struct.unpack('>II', data[16:24])


def test_the_fingerprint_list_is_pinned_and_describes_exactly_the_tracked_files():
    data = (BRANDING / 'SHA256SUMS').read_bytes()
    assert hashlib.sha256(data).hexdigest() == tree.BRANDING_SUMS_SHA256, \
        'branding changed: the owner approves a new pin in scripts/check-source-tree.py'
    listed = sums()
    on_disk = {p.relative_to(BRANDING).as_posix() for p in BRANDING.rglob('*')
               if p.is_file() and p.name not in ('SHA256SUMS', 'README.md')}
    assert set(listed) == on_disk
    for name, digest in listed.items():
        assert hashlib.sha256((BRANDING / name).read_bytes()).hexdigest() == digest, name


def test_the_policy_accepts_the_approved_set_and_nothing_else(tmp_path, monkeypatch):
    repo = tmp_path / 'repo'
    (repo / 'branding').mkdir(parents=True)
    (repo / 'branding/a.png').write_bytes(b'\x89PNG synthetic a')
    (repo / 'branding/b.png').write_bytes(b'\x89PNG synthetic b')
    listing = ''.join(f"{hashlib.sha256((repo / 'branding' / n).read_bytes()).hexdigest()}  {n}\n" for n in ('a.png', 'b.png'))
    (repo / 'branding/SHA256SUMS').write_bytes(listing.encode())
    subprocess.run(['git', 'init', '-q'], cwd=repo, check=True)
    subprocess.run(['git', 'add', '-A'], cwd=repo, check=True)

    monkeypatch.setattr(tree, 'BRANDING_SUMS_SHA256', hashlib.sha256(listing.encode()).hexdigest())
    count, problems = tree.check(repo)
    assert problems == []

    (repo / 'branding/a.png').write_bytes(b'\x89PNG changed')                      # a file changed
    assert [n for n, _ in tree.check(repo)[1]] == ['branding/a.png']
    (repo / 'branding/a.png').write_bytes(b'\x89PNG synthetic a')
    (repo / 'branding/c.png').write_bytes(b'\x89PNG extra')                         # a file not on the list
    subprocess.run(['git', 'add', '-A'], cwd=repo, check=True)
    assert [n for n, _ in tree.check(repo)[1]] == ['branding/c.png']
    monkeypatch.setattr(tree, 'BRANDING_SUMS_SHA256', 'f' * 64)                      # the list itself changed
    assert 'branding/SHA256SUMS' in [n for n, _ in tree.check(repo)[1]]


def test_the_windows_icon_has_every_size_and_a_png_frame_for_256():
    data = (BRANDING / 'icon/defjam.ico').read_bytes()
    reserved, kind, count = struct.unpack('<HHH', data[:6])
    assert (reserved, kind) == (0, 1)
    sizes = []
    for i in range(count):
        width, height, _, _, planes, bits, length, offset = struct.unpack('<BBBBHHII', data[6 + 16 * i:22 + 16 * i])
        size = width or 256
        sizes.append(size)
        assert height in (width, 0) and bits == 32 and offset + length <= len(data)
        frame = data[offset:offset + length]
        if size == 256:
            assert frame[:8] == b'\x89PNG\r\n\x1a\n'
        else:
            header_size, w, h = struct.unpack('<Iii', frame[:12])
            assert (header_size, w, h) == (40, size, size * 2)
            mask_rows = ((size + 31) // 32) * 4 * size
            assert length == 40 + size * size * 4 + mask_rows
    assert sizes == [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def test_the_macos_icon_carries_png_frames_for_every_dock_size():
    data = (BRANDING / 'icon/macos/defjam.icns').read_bytes()
    assert data[:4] == b'icns' and struct.unpack('>I', data[4:8])[0] == len(data)
    kinds, pos = [], 8
    while pos < len(data):
        kind, length = data[pos:pos + 4], struct.unpack('>I', data[pos + 4:pos + 8])[0]
        assert data[pos + 8:pos + 16] == b'\x89PNG\r\n\x1a\n'
        kinds.append(kind)
        pos += length
    assert pos == len(data)
    assert {b'icp4', b'icp5', b'icp6', b'ic07', b'ic08', b'ic09', b'ic10', b'ic11', b'ic12', b'ic13', b'ic14'} == set(kinds)


def test_the_png_icons_the_steam_art_and_the_window_icons_have_the_sizes_their_readers_expect():
    for size in (16, 20, 24, 32, 40, 48, 64, 96, 128, 192, 256, 512, 1024):
        assert png_size(BRANDING / f'icon/png/defjam-{size}.png') == (size, size)
    steam = BRANDING / 'steam'
    assert png_size(steam / 'portrait.png') == (600, 900)
    assert png_size(steam / 'wide.png') == (920, 430)
    assert png_size(steam / 'hero.png') == (1920, 620)
    assert png_size(steam / 'logo.png')[0] == 1280 and png_size(steam / 'logo.png')[1] <= 720
    assert png_size(steam / 'icon.png') == (256, 256)
    fan = BRANDING / 'fan'
    assert png_size(fan / 'steam/portrait.png') == (600, 900)
    assert png_size(fan / 'steam/wide.png') == (920, 430)
    assert png_size(fan / 'steam/logo.png')[0] == 1280
    assert png_size(BRANDING / 'masters/fan-tag.png')[0] > 500
    for name in ('window-128.rgba', 'window-128-macos.rgba'):
        assert (BRANDING / 'icon/window' / name).stat().st_size == 128 * 128 * 4


@pytest.mark.slow
def test_the_generator_rebuilds_the_whole_set_from_the_masters(tmp_path):
    pytest.importorskip('PIL')
    pytest.importorskip('numpy')
    import shutil
    out = tmp_path / 'branding'
    shutil.copytree(BRANDING / 'masters', out / 'masters')
    result = subprocess.run([__import__('sys').executable, '-I', str(ROOT / 'scripts/make-branding.py'), 'render', '--out', str(out)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    rebuilt = {p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file()}
    assert rebuilt == {p.relative_to(BRANDING).as_posix() for p in BRANDING.rglob('*')
                       if p.is_file() and p.name != 'README.md'}
    assert png_size(out / 'steam/portrait.png') == (600, 900)
    assert png_size(out / 'steam/hero.png') == (1920, 620)
    assert png_size(out / 'fan/steam/portrait.png') == (600, 900) and png_size(out / 'fan/steam/wide.png') == (920, 430)


def test_each_platforms_payload_carries_only_the_branding_it_uses():
    package = load('branding_package', ROOT / 'scripts/package-setup.py')
    tracked = sorted(p.relative_to(ROOT).as_posix() for p in BRANDING.rglob('*') if p.is_file())

    def wanted(platform):
        return [n for n in tracked if package.payload_wants(n, platform)]

    # What each build and setup reads at install time is there...
    assert 'branding/icon/defjam.ico' in wanted('windows-x64')
    assert 'branding/icon/window/window-128-macos.rgba' in wanted('macos-arm64')
    linux = wanted('linux-x64')
    for name in ('branding/icon/window/window-128.rgba', 'branding/icon/png/defjam-128.png',
                 'branding/icon/png/defjam-256.png', 'branding/steam/hero.png', 'branding/steam/icon.png',
                 'branding/fan/steam/portrait.png', 'branding/fan/steam/wide.png', 'branding/fan/steam/logo.png'):
        assert name in linux
    # ...the repository-only files are not, and a payload outside branding/ is unfiltered.
    for platform in ('windows-x64', 'macos-arm64', 'linux-x64'):
        assert not [n for n in wanted(platform) if '/masters/' in n or 'social-preview' in n or 'macos/' in n]
        assert package.payload_wants('setup/engine.py', platform)
    # The macOS payload was 7.6 MB heavier with all of branding/ and hdiutil ran out of room.
    assert sum((ROOT / n).stat().st_size for n in wanted('macos-arm64')) < 200_000
    assert sum((ROOT / n).stat().st_size for n in wanted('windows-x64')) < 300_000
