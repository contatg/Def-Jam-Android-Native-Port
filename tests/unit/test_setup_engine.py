"""Installer boundaries and recovery behavior with synthetic, game-free data."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


engine = load('setup_engine', ROOT / 'setup/engine.py')
package = load('setup_package', ROOT / 'scripts/package-setup.py')
assets = load('setup_assets', ROOT / 'scripts/check-setup-assets.py')


def payload(tmp_path):
    folder = tmp_path / 'payload'
    folder.mkdir()
    (folder / 'source/scripts').mkdir(parents=True)
    (folder / 'source/config').mkdir()
    (folder / 'source/src').mkdir()
    (folder / 'source/config/dump-manifest.json').write_text('{}')
    (folder / 'source/scripts/verify-dump.py').write_text(
        "def verify(folder, manifest):\n    from pathlib import Path\n    return [] if (Path(folder)/'default.xbe').read_bytes() == b'synthetic' else ['wrong dump']\n")
    (folder / 'source/src/maintained.c').write_text('/* synthetic source */')
    inventory = {p.relative_to(folder).as_posix(): engine.digest(p) for p in folder.rglob('*') if p.is_file()}
    manifest = dict(schema=1, platform=engine.HOST_PLATFORM, version='0.4.1',
                    source_commit='a'*40, toolkit_commit='b'*40, files=inventory)
    engine.write_json(folder / 'manifest.json', manifest)
    return folder


def installer(tmp_path):
    folder = payload(tmp_path)
    dump = tmp_path / 'dump'
    dump.mkdir()
    (dump / 'default.xbe').write_bytes(b'synthetic')
    (dump / 'other.txt').write_bytes(b'files')
    args = argparse.Namespace(payload=str(folder), install_dir=str(tmp_path/'app'),
        data_dir=str(tmp_path/'data'), dump=str(dump), log=None, status_file=None,
        cancel_file=None, install_prerequisites=False, silent=True, no_shortcuts=True, no_desktop_shortcut=False)
    result = engine.Engine(args)
    result.data.mkdir()
    result.prepare_source()
    return result


@pytest.mark.parametrize('name', ['../escape', '/escape', 'C:/escape', 'a\\b', 'foo/../bar',
                                  'CON', 'nul.txt', 'COM1.log', 'a/b.', 'a//b'])
def test_payload_and_image_names_cannot_escape_or_alias(name):
    with pytest.raises(engine.SetupError):
        engine.safe_relative(name)


def test_payload_tampering_and_extra_files_fail_before_execution(tmp_path):
    folder = payload(tmp_path)
    assert engine.verify_payload(folder)['version'] == '0.4.1'
    extra = folder / 'rogue.py'
    extra.write_text('unexpected')
    with pytest.raises(engine.SetupError, match='Unexpected'):
        engine.verify_payload(folder)
    extra.unlink()
    (folder / 'source/src/maintained.c').write_text('tampered')
    with pytest.raises(engine.SetupError, match='verification'):
        engine.verify_payload(folder)


@pytest.mark.parametrize('data,dump', [('app/data','dump'), ('data','app'), ('data','data/dump')])
def test_nested_destinations_are_refused(tmp_path, data, dump):
    with pytest.raises(engine.SetupError, match='separate'):
        engine.validate_locations(tmp_path/'app', tmp_path/data, tmp_path/dump)


def test_extraction_resume_requires_outputs_and_preserves_user_saves(tmp_path):
    setup = installer(tmp_path)
    saves = setup.data / 'save'
    saves.mkdir()
    (saves / 'profile').write_bytes(b'keep exactly')
    setup.extract()
    setup.extract()
    (setup.data/'extracted/other.txt').write_bytes(b'corrupted')
    with pytest.raises(engine.SetupError, match='differs'):
        setup.extract()
    assert (saves/'profile').read_bytes() == b'keep exactly'
    assert (setup.dump/'other.txt').read_bytes() == b'files'


def test_failed_extraction_never_publishes_a_partial_dump(tmp_path):
    setup = installer(tmp_path)
    (setup.dump/'default.xbe').write_bytes(b'wrong')
    with pytest.raises(engine.SetupError, match='verification'):
        setup.extract()
    assert not (setup.data/'extracted').exists()
    assert not list(setup.data.glob('extract-pending-*'))


def test_resume_repairs_source_without_removing_build_artifacts(tmp_path):
    setup = installer(tmp_path)
    (setup.source/'src/maintained.c').write_text('corrupted')
    generated = setup.source / 'src/recomp/gen/recomp_000.c'
    generated.parent.mkdir(parents=True)
    generated.write_text('/* synthetic generated fixture */')
    setup.prepare_source()
    assert (setup.source/'src/maintained.c').read_text() == '/* synthetic source */'
    assert generated.exists()


def test_process_arguments_are_literal_and_failed_stage_is_not_success(tmp_path):
    setup = installer(tmp_path)
    script = tmp_path / 'path with spaces & quote.py'
    script.write_text('import sys\nprint(sys.argv[1])\nsys.exit(7)\n')
    with pytest.raises(engine.SetupError, match='exit 7'):
        setup.run([sys.executable, script, '$(not a shell); & literal'])


def test_cancellation_prevents_a_new_stage(tmp_path):
    setup = installer(tmp_path)
    cancel = tmp_path/'cancel'
    cancel.touch()
    setup.args.cancel_file = str(cancel)
    with pytest.raises(engine.SetupError) as error:
        setup.run([sys.executable, '-c', 'raise RuntimeError("should not execute")'])
    assert error.value.code == 6


def test_install_lock_excludes_another_process_and_recovers(tmp_path):
    root = tmp_path/'app'
    with engine.install_lock(root):
        with pytest.raises(engine.SetupError) as error:
            with engine.install_lock(root):
                pass
        assert error.value.code == 5
    with engine.install_lock(root):
        pass


def test_dependency_zip_traversal_is_rejected(tmp_path):
    archive = tmp_path/'unsafe.zip'
    with zipfile.ZipFile(archive, 'w') as stream:
        stream.writestr('../escape.txt', 'bad')
    with pytest.raises(ValueError, match='Unsafe'):
        package.safe_unpack(archive, tmp_path/'output')
    assert not (tmp_path/'escape.txt').exists()


@pytest.mark.parametrize('name', ['src/recomp/gen/recomp_00.c', 'game_files/default.xbe',
                                 'notes/hidden.exe', 'logs/foo.txt', 'audio/test.wav'])
def test_release_payload_source_filter_excludes_game_and_binary_files(name):
    assert not package.source_allowed(name, True)


def test_release_asset_exception_is_exact_and_cannot_admit_the_game():
    names = ['DefJamSetup-0.5.0-windows-x64' + ext for ext in ('.exe', '.sha256', '.provenance.json')]
    assets.validate_asset_names('0.5.0', names)
    with pytest.raises(ValueError):
        assets.validate_asset_names('0.5.0', names + ['defjam_recomp.exe'])


def test_payload_manifest_cannot_disable_file_checks(tmp_path):
    folder = payload(tmp_path)
    manifest = json.loads((folder/'manifest.json').read_text())
    manifest['files'] = {}
    engine.write_json(folder/'manifest.json', manifest)
    with pytest.raises(engine.SetupError, match='Unexpected'):
        engine.verify_payload(folder)


def test_cancel_mid_copy_does_not_publish_or_remove_saves(tmp_path, monkeypatch):
    setup = installer(tmp_path)
    save = setup.data / 'save'
    save.mkdir()
    (save/'profile').write_bytes(b'keep')
    def cancel():
        raise engine.SetupError('cancel', 6)
    monkeypatch.setattr(setup, 'cancelled', cancel)
    with pytest.raises(engine.SetupError):
        setup.extract()
    assert not (setup.data/'extracted').exists()
    assert not list(setup.data.glob('extract-pending-*'))
    assert (save/'profile').read_bytes() == b'keep'


def test_new_release_only_reuses_lift_after_pipeline_validation(tmp_path):
    setup = installer(tmp_path)
    old = setup.install / 'versions/old/source'
    (old / 'src/recomp/gen').mkdir(parents=True)
    (old / 'src/recomp/gen/lift-state.json').write_text('{"synthetic":"uncertified"}')
    (old / 'src/recomp/gen/recomp_00.c').write_text('/* synthetic */')
    engine.write_json(setup.install/'installed.json', {'source': str(old)})
    setup.prepare_source()
    assert (setup.source/'src/recomp/gen/lift-state.json').exists()
    # Cached files never imply successful certification: a real verifier reads
    # inputs/outputs and will reject this deliberately incomplete state.
    assert json.loads((setup.source/'src/recomp/gen/lift-state.json').read_text()) == {'synthetic':'uncertified'}


def test_update_cannot_switch_the_data_root(tmp_path):
    setup = installer(tmp_path)
    engine.write_json(setup.install/'installed.json', {'source':str(setup.source),
        'data':str(tmp_path/'other-data'), 'executable':str(setup.source/'game.exe')})
    with pytest.raises(engine.SetupError, match='data folder'):
        setup.check_existing_install()


def test_install_refuses_unrelated_nonempty_destination(tmp_path):
    setup = installer(tmp_path)
    (setup.install/'personal.txt').write_bytes(b'keep')
    with pytest.raises(engine.SetupError, match='empty install folder'):
        setup.check_install_destination()
    assert (setup.install/'personal.txt').read_bytes() == b'keep'


def test_owned_failed_install_can_resume_without_a_launch_receipt(tmp_path):
    setup = installer(tmp_path)
    engine.write_json(setup.install/'setup-owner.json', {'schema':1, 'product':'DefJamRecompiled'})
    setup.check_install_destination()
    assert (setup.source/'src/maintained.c').exists()


@pytest.mark.skipif(sys.platform != 'win32', reason='CMD environment expansion')
def test_toolchain_handoff_accepts_spaces_ampersands_and_literal_percent(tmp_path):
    setup = installer(tmp_path)
    batch = tmp_path/'space & %PATH% folder'/'toolchain.cmd'
    batch.parent.mkdir()
    batch.write_text('@echo off\necho TOOLCHAIN_OK\n')
    setup.env['DEFJAM_SETUP_ENV_SCRIPT'] = '"' + str(batch) + '"'
    output = setup.run(['cmd.exe', '/d', '/c', '%DEFJAM_SETUP_ENV_SCRIPT%'], capture=True)
    assert output.strip() == 'TOOLCHAIN_OK'


def test_captured_toolchain_environment_is_not_written_to_logs(tmp_path):
    import io
    setup = installer(tmp_path)
    setup.log = io.StringIO()
    setup.env['SETUP_TEST_CAPTURE'] = 'private-environment-fixture'
    output = setup.run([sys.executable, '-c', 'import os; print(os.environ["SETUP_TEST_CAPTURE"])'],
                       capture=True, log_output=False)
    assert output.strip() == setup.env['SETUP_TEST_CAPTURE']
    assert setup.env['SETUP_TEST_CAPTURE'] not in setup.log.getvalue()


def test_nested_destination_failure_creates_log_before_engine_initialization(tmp_path):
    folder = payload(tmp_path)
    log = tmp_path/'diagnostics/session.log'
    code = engine.main(['--payload', str(folder), '--silent', '--dump', str(tmp_path/'dump'),
        '--install-dir', str(tmp_path/'app'), '--data-dir', str(tmp_path/'app/data'), '--log', str(log)])
    assert code == 2
    assert 'separate, non-nested' in log.read_text()
    assert not (tmp_path/'app').exists()


@pytest.mark.skipif(not engine.IS_WINDOWS, reason='Windows launcher and Start-menu shortcut')
@pytest.mark.parametrize('desktop', [True, False])
def test_desktop_choice_preserves_start_menu_shortcut_and_launch_guidance(tmp_path, monkeypatch, capsys, desktop):
    setup = installer(tmp_path)
    (setup.payload/'DefJamLauncher.exe').write_bytes(b'synthetic launcher')
    setup.args.no_shortcuts = False
    setup.args.no_desktop_shortcut = not desktop
    calls = []
    monkeypatch.setattr(setup, 'run', lambda argv: calls.append((argv, setup.env['DEFJAM_SETUP_DESKTOP_SHORTCUT'])))
    setup.activate()
    assert len(calls) == 1
    assert calls[0][1] == ('1' if desktop else '0')
    assert str(calls[0][0][-1]).endswith('shortcuts.ps1')
    assert str(setup.install/'DefJamLauncher.exe') in capsys.readouterr().out
    assert (setup.install/'installed.ini').exists()


@pytest.mark.skipif(not (ROOT/'tools/xboxrecomp/tools/xiso').is_dir(), reason='requires recursive checkout')
def test_image_traversal_is_rejected_before_any_file_publication(tmp_path):
    setup = installer(tmp_path)
    # Minimal XDVDFS descriptor/directory: malicious name with a real in-bounds body.
    import struct
    image = bytearray(35 * 2048)
    image[32*2048:32*2048+20] = b'MICROSOFT*XBOX*MEDIA'
    struct.pack_into('<II', image, 32*2048+0x14, 33, 2048)
    name = b'../outside'
    struct.pack_into('<HHIIBB', image, 33*2048, 0, 0, 34, 1, 0, len(name))
    image[33*2048+14:33*2048+14+len(name)] = name
    iso = tmp_path/'bad.iso'
    iso.write_bytes(image)
    setup.dump = iso
    # Fixture source needs the exact pinned reader, not a different extractor.
    import shutil
    shutil.copytree(ROOT/'tools/xboxrecomp/tools/xiso', setup.source/'tools/xboxrecomp/tools/xiso')
    with pytest.raises(engine.SetupError, match='Unsafe'):
        setup.extract()
    assert not (setup.data/'extracted').exists()
    assert not (setup.data/'outside').exists()


@pytest.mark.skipif(not (ROOT/'tools/xboxrecomp/tools/xiso').is_dir(), reason='requires recursive checkout')
def test_valid_synthetic_xiso_extracts_and_resumes(tmp_path):
    import shutil
    import struct
    setup = installer(tmp_path)
    image = bytearray(35 * 2048)
    image[32*2048:32*2048+20] = b'MICROSOFT*XBOX*MEDIA'
    struct.pack_into('<II', image, 32*2048+0x14, 33, 2048)
    name, body = b'default.xbe', b'synthetic'
    struct.pack_into('<HHIIBB', image, 33*2048, 0, 0, 34, len(body), 0, len(name))
    image[33*2048+14:33*2048+14+len(name)] = name
    image[34*2048:34*2048+len(body)] = body
    setup.dump = tmp_path/'valid.iso'
    setup.dump.write_bytes(image)
    shutil.copytree(ROOT/'tools/xboxrecomp/tools/xiso', setup.source/'tools/xboxrecomp/tools/xiso')
    setup.extract()
    setup.extract()
    assert (setup.data/'extracted/default.xbe').read_bytes() == body


@pytest.mark.skipif(not (ROOT/'tools/xboxrecomp/tools/xiso').is_dir(), reason='requires recursive checkout')
def test_invalid_disc_image_has_documented_exit_and_no_partial_output(tmp_path):
    import shutil
    setup = installer(tmp_path)
    setup.dump = tmp_path/'wrong.iso'
    setup.dump.write_bytes(b'not a disc')
    shutil.copytree(ROOT/'tools/xboxrecomp/tools/xiso', setup.source/'tools/xboxrecomp/tools/xiso')
    with pytest.raises(engine.SetupError, match='Invalid Xbox disc') as error:
        setup.extract()
    assert error.value.code == 2
    assert not (setup.data/'extracted').exists()
    assert not list(setup.data.glob('extract-pending-*'))


@pytest.mark.skipif(sys.platform != 'win32', reason='Windows lifetime lock')
def test_game_lock_blocks_setup_and_releases_after_close(tmp_path):
    with engine.game_lock(tmp_path):
        with pytest.raises(engine.SetupError) as error:
            with engine.game_lock(tmp_path):
                pass
        assert error.value.code == 5
    with engine.game_lock(tmp_path):
        pass


@pytest.mark.skipif(sys.platform != 'win32', reason='Windows uninstall')
def test_uninstall_preserves_data_and_unrelated_install_files(tmp_path, monkeypatch):
    setup = installer(tmp_path)
    (setup.data/'save').mkdir()
    (setup.data/'save/profile').write_bytes(b'keep')
    (setup.install/'unrelated.txt').write_bytes(b'keep too')
    engine.write_json(setup.install/'installed.json', {'schema':1, 'source':str(setup.source), 'data':str(setup.data)})
    (setup.install/'DefJamLauncher.exe').write_bytes(b'synthetic')
    monkeypatch.setattr(engine.subprocess, 'run', lambda *a, **k: None)
    engine.uninstall(setup.install)
    assert not (setup.install/'versions').exists()
    assert not (setup.install/'DefJamLauncher.exe').exists()
    assert (setup.install/'unrelated.txt').read_bytes() == b'keep too'
    assert (setup.data/'save/profile').read_bytes() == b'keep'


def synthetic_launcher_template(payload):
    template = payload / 'launcher' / engine.LAUNCHER_BUNDLE
    (template / 'Contents/MacOS').mkdir(parents=True)
    (template / 'Contents/MacOS/DefJamLauncher').write_bytes(b'synthetic launcher')
    return template


@pytest.mark.skipif(not engine.IS_MACOS, reason='macOS launcher bundle')
@pytest.mark.parametrize('desktop', [True, False])
def test_macos_launcher_bundle_shortcuts_follow_the_choices_and_stay_in_the_home_folder(
        tmp_path, monkeypatch, capsys, desktop):
    setup = installer(tmp_path)
    home = tmp_path / 'home'
    (home / 'Desktop').mkdir(parents=True)
    monkeypatch.setenv('HOME', str(home))
    synthetic_launcher_template(setup.payload)
    setup.args.no_shortcuts = False
    setup.args.no_desktop_shortcut = not desktop
    calls = []
    monkeypatch.setattr(setup, 'run', lambda argv, **kwargs: calls.append([str(a) for a in argv]))
    setup.activate()
    bundle = setup.install / engine.LAUNCHER_BUNDLE
    assert (bundle / 'Contents/Resources/install-root.txt').read_text() == str(setup.install)
    applications = home / 'Applications' / engine.LAUNCHER_BUNDLE
    assert (applications / 'Contents/Resources/install-root.txt').read_text() == str(setup.install)
    assert any('--make-alias' in call for call in calls) == desktop
    receipt = json.loads((setup.install / 'installed.json').read_text())
    assert str(applications) in receipt['shortcuts']
    assert (str(home / 'Desktop' / 'Def Jam Recompiled') in receipt['shortcuts']) == desktop
    assert str(bundle) in capsys.readouterr().out
    assert not (setup.install / 'installed.ini').exists()


@pytest.mark.skipif(not engine.IS_MACOS, reason='macOS launcher bundle')
def test_macos_no_shortcuts_leaves_the_home_folder_alone(tmp_path, monkeypatch):
    setup = installer(tmp_path)
    home = tmp_path / 'home'
    home.mkdir()
    monkeypatch.setenv('HOME', str(home))
    synthetic_launcher_template(setup.payload)
    monkeypatch.setattr(setup, 'run', lambda argv, **kwargs: None)
    setup.activate()
    assert (setup.install / engine.LAUNCHER_BUNDLE).is_dir()
    assert list(home.iterdir()) == []


@pytest.mark.skipif(not engine.IS_MACOS, reason='macOS launcher bundle')
def test_macos_uninstall_removes_only_recorded_shortcuts_that_still_name_this_install(tmp_path, monkeypatch):
    setup = installer(tmp_path)
    home = tmp_path / 'home'
    (home / 'Desktop').mkdir(parents=True)
    monkeypatch.setenv('HOME', str(home))
    synthetic_launcher_template(setup.payload)
    setup.args.no_shortcuts = False
    monkeypatch.setattr(setup, 'run', lambda argv, **kwargs: Path(argv[-1]).write_bytes(b'alias')
                        if '--make-alias' in [str(a) for a in argv] else None)
    setup.activate()
    mine = home / 'Applications' / engine.LAUNCHER_BUNDLE
    other = tmp_path / 'other-install'
    other.mkdir()
    (mine / 'Contents/Resources/install-root.txt').write_text(str(other))   # now names another install
    (setup.data / 'settings.ini').write_text('keep')
    engine.uninstall(setup.install)
    assert mine.is_dir()                      # not ours any more: left alone
    assert not (home / 'Desktop/Def Jam Recompiled').exists()
    assert not (setup.install / engine.LAUNCHER_BUNDLE).exists()
    assert (setup.data / 'settings.ini').read_text() == 'keep'


@pytest.mark.skipif(engine.IS_WINDOWS or not engine.IS_MACOS, reason='macOS toolchain')
def test_macos_setup_refuses_intel_and_missing_command_line_tools_with_the_documented_exit(tmp_path, monkeypatch):
    setup = installer(tmp_path)
    monkeypatch.setattr(engine.platform, 'machine', lambda: 'x86_64')
    with pytest.raises(engine.SetupError, match='Apple Silicon') as intel:
        setup.toolchain()
    assert intel.value.code == 3
    monkeypatch.setattr(engine.platform, 'machine', lambda: 'arm64')
    monkeypatch.setattr(engine.subprocess, 'run', lambda *a, **k: subprocess.CompletedProcess(a, 2, '', 'no tools'))
    with pytest.raises(engine.SetupError, match='Command Line Tools') as missing:
        setup.toolchain()
    assert missing.value.code == 3
    assert engine.APPLE_COMMAND_LINE_TOOLS_URL in str(missing.value)


@pytest.mark.skipif(engine.IS_WINDOWS, reason='POSIX process groups')
def test_cancellation_stops_the_whole_process_tree(tmp_path):
    import time
    setup = installer(tmp_path)
    marker = tmp_path / 'child.pid'
    cancel = tmp_path / 'cancel'
    setup.args.cancel_file = str(cancel)
    script = f"sleep 300 & echo $! > '{marker}'; wait"

    def raise_cancel():
        while not marker.exists():
            time.sleep(.05)
        cancel.write_text('cancel')

    import threading
    threading.Thread(target=raise_cancel, daemon=True).start()
    with pytest.raises(engine.SetupError) as stopped:
        setup.run(['sh', '-c', script])
    assert stopped.value.code == 6
    pid = int(marker.read_text())
    time.sleep(.3)
    with pytest.raises(ProcessLookupError):
        import os
        os.kill(pid, 0)


def test_hd_textures_are_refused_on_a_host_whose_renderer_cannot_draw_them(tmp_path, monkeypatch):
    monkeypatch.setattr(engine, 'HD_SUPPORTED', False)
    setup = installer(tmp_path)
    setup.args.hd_textures = True
    with pytest.raises(engine.SetupError, match='not supported') as refused:
        setup.execute()
    assert refused.value.code == 2
    assert not (setup.install / 'installed.json').exists()


def test_hd_textures_are_supported_on_windows_macos_and_linux():
    assert engine.HD_SUPPORTED == (engine.IS_WINDOWS or engine.IS_MACOS or engine.IS_LINUX)
