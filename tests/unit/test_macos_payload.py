"""The macOS setup payload builder's safety checks, with synthetic archives only."""
import importlib.util
import json
from pathlib import Path
import os
import re
import subprocess
import sys
import tarfile
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


payload = load('macos_payload_module', ROOT / 'scripts/macos_payload.py')
assets = load('macos_assets_module', ROOT / 'scripts/check-setup-assets.py')


def test_every_pin_is_complete_and_checksummed():
    pins = payload.pins()
    for name in ('python', 'cmake', 'ninja', 'moltenvk'):
        assert pins[name]['url'].startswith('https://github.com/')
        assert re.fullmatch('[0-9a-f]{64}', pins[name]['sha256'])
    assert re.fullmatch('[0-9a-f]{40}', pins['shaderc']['commit'])
    assert pins['wheels'] and all(re.fullmatch('[0-9a-f]{64}', sha) for sha in pins['wheels'].values())
    assert all('macosx' in name or name.endswith('py3-none-any.whl') for name in pins['wheels'])
    assert all('arm64' in name or name.endswith('py3-none-any.whl') for name in pins['wheels'])


@pytest.mark.skipif(not (ROOT / 'tools/xboxrecomp/cmake/xbox_sdl3.cmake').is_file(), reason='requires recursive checkout')
def test_sdl3_comes_from_the_toolkits_own_pin():
    version, sha = payload.toolkit_pin('sdl3')
    assert re.fullmatch(r'\d+\.\d+\.\d+', version) and re.fullmatch('[0-9a-f]{64}', sha)


def test_download_refuses_a_file_that_does_not_match_its_pin(tmp_path):
    source = tmp_path / 'tool.bin'
    source.write_bytes(b'not what was pinned')
    with pytest.raises(ValueError, match='mismatch'):
        payload.download(source.as_uri(), tmp_path / 'cache/tool.bin', '0' * 64)
    assert not (tmp_path / 'cache/tool.bin').exists()
    ok = payload.download(source.as_uri(), tmp_path / 'cache/tool.bin', payload.digest(source))
    assert ok.read_bytes() == b'not what was pinned'


def test_unpack_rejects_traversal_and_symlinks_in_untrusted_archives(tmp_path):
    bad = tmp_path / 'bad.zip'
    with zipfile.ZipFile(bad, 'w') as archive:
        archive.writestr('../escape.txt', 'x')
    with pytest.raises(ValueError, match='Unsafe'):
        payload.unpack(bad, tmp_path / 'one')
    link = tmp_path / 'link.zip'
    with zipfile.ZipFile(link, 'w') as archive:
        info = zipfile.ZipInfo('shortcut')
        info.external_attr = 0o120777 << 16
        archive.writestr(info, '/etc/passwd')
    with pytest.raises(ValueError, match='symlink'):
        payload.unpack(link, tmp_path / 'two')
    escaping = tmp_path / 'escape.tar'
    with tarfile.open(escaping, 'w') as archive:
        info = tarfile.TarInfo('inside/link')
        info.type = tarfile.SYMTYPE
        info.linkname = '../../outside'
        archive.addfile(info)
    with pytest.raises(Exception):
        payload.unpack(escaping, tmp_path / 'three')
    assert not (tmp_path / 'escape.txt').exists()


@pytest.mark.skipif(os.name == 'nt', reason='POSIX execute bits: Windows files have none')
def test_unpack_keeps_the_execute_bit_of_bundled_tools(tmp_path):
    archive = tmp_path / 'tools.zip'
    with zipfile.ZipFile(archive, 'w') as package:
        info = zipfile.ZipInfo('bin/tool')
        info.external_attr = 0o100755 << 16
        package.writestr(info, '#!/bin/sh\n')
    payload.unpack(archive, tmp_path / 'out')
    assert (tmp_path / 'out/bin/tool').stat().st_mode & 0o111


@pytest.mark.skipif(os.name == 'nt', reason='needs symbolic links, which Windows creates only with a privilege (WinError 1314)')
def test_flatten_links_leaves_only_regular_files(tmp_path):
    (tmp_path / 'lib').mkdir()
    (tmp_path / 'lib/libreal.1.dylib').write_bytes(b'library')
    (tmp_path / 'lib/libreal.dylib').symlink_to('libreal.1.dylib')
    (tmp_path / 'lib/dangling').symlink_to('missing')
    payload.flatten_links(tmp_path)
    assert (tmp_path / 'lib/libreal.dylib').read_bytes() == b'library'
    assert not (tmp_path / 'lib/libreal.dylib').is_symlink()
    assert not (tmp_path / 'lib/dangling').exists() and not (tmp_path / 'lib/dangling').is_symlink()


def setup_unpack_commands(archive, destination):
    """The tool calls Setup.unpack() in setup/macos/Setup/main.swift makes, with its paths filled in."""
    body = (ROOT / 'setup/macos/Setup/main.swift').read_text(encoding='utf-8').split('func unpack()', 1)[1]
    body = body.split('func engineProcess', 1)[0]
    commands = []
    for tool, arguments in re.findall(r'tool\("([^"]+)", \[([^\]]*)\]\)', body):
        values = {'archive.path': str(archive), 'payload.path': str(destination)}
        commands.append([tool] + [values.get(a.strip(), a.strip().strip('"')) for a in arguments.split(',')])
    return commands


def test_setup_unpacks_its_payload_without_the_download_quarantine():
    commands = setup_unpack_commands('A', 'P')
    assert commands[0] == ['/usr/bin/ditto', '-x', '-k', '--noqtn', 'A', 'P']
    assert ['/usr/bin/xattr', '-d', '-r', 'com.apple.quarantine', 'P'] in commands


@pytest.mark.skipif(sys.platform != 'darwin', reason='macOS quarantine attributes and ditto')
def test_a_quarantined_payload_unpacks_to_files_gatekeeper_will_run(tmp_path):
    archive = tmp_path / 'payload.zip'
    with zipfile.ZipFile(archive, 'w') as package:
        info = zipfile.ZipInfo('python/bin/python3')
        info.external_attr = 0o100755 << 16
        package.writestr(info, '#!/bin/sh\n')
    # What a browser stamps on a download; the disk image and everything in it inherit it.
    subprocess.run(['/usr/bin/xattr', '-w', 'com.apple.quarantine', '0081;6ad6628e;Firefox;', archive], check=True)
    for command in setup_unpack_commands(archive, tmp_path / 'payload'):
        subprocess.run(command, check=True)
    unpacked = tmp_path / 'payload/python/bin/python3'
    assert unpacked.stat().st_mode & 0o111
    attributes = subprocess.run(['/usr/bin/xattr', unpacked], capture_output=True, text=True, check=True).stdout
    assert 'com.apple.quarantine' not in attributes


def test_macos_release_assets_are_exactly_the_dmg_checksum_and_provenance():
    names = ['DefJamSetup-0.6.1-macos-arm64' + ext for ext in ('.dmg', '.sha256', '.provenance.json')]
    assets.validate_asset_names('0.6.1', names, 'macos-arm64')
    with pytest.raises(ValueError):
        assets.validate_asset_names('0.6.1', names + ['defjam_recomp'], 'macos-arm64')
    with pytest.raises(ValueError):
        assets.validate_asset_names('0.6.1', ['DefJamSetup-0.6.1-macos-arm64.exe'] + names[1:], 'macos-arm64')
