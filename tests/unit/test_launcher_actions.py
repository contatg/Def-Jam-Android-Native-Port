"""Game-free native path/setup context checks, including Unicode and a moved setup tool."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r'''
#include "pc_ui_actions.h"
#include <cstdlib>
#include <iostream>
extern "C" const char *pc_settings_path() { const char *p=getenv("TEST_SETTINGS_PATH");return p?p:""; }
int main(int argc,char**argv) {
    if(argc>1 && std::string(argv[1])=="missing") {
        std::cout<<pcui::open_path(argv[2])<<"\n"<<pcui::action_status()<<"\n";return 0;
    }
    if(argc>1 && std::string(argv[1])=="setup") {
        std::cout<<pcui::setup_path()<<"\n"<<pcui::setup_ready()<<"\n";return 0;
    }
    for(auto f:{pcui::Folder::Data,pcui::Folder::Saves,pcui::Folder::Mods,pcui::Folder::Install,
                pcui::Folder::Settings,pcui::Folder::Logs})std::cout<<pcui::folder_path(f)<<"\n";
}
'''


@pytest.fixture(scope='module')
def action_tool(tmp_path_factory):
    folder = tmp_path_factory.mktemp('launcher-actions')
    source = folder / 'main.cpp'
    source.write_text(DRIVER)
    compiler = shutil.which('cl') or shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        pytest.skip('C++ compiler required')
    msvc = Path(compiler).stem.lower() == 'cl'
    exe = folder / ('actions.exe' if msvc else 'actions')
    actions = ROOT / 'src/hooks/pc_ui_actions.cpp'
    if msvc:
        cmd = [compiler, '/nologo', '/std:c++17', '/EHsc', '/I' + str(ROOT / 'src/hooks'),
               str(source), str(actions), '/Fe:' + str(exe), '/link', 'shell32.lib', 'comdlg32.lib', 'user32.lib']
    else:
        if not shutil.which('pkg-config'):
            pytest.skip('SDL3 development package required')
        pkg = subprocess.run(['pkg-config', '--cflags', '--libs', 'sdl3'], capture_output=True, text=True)
        if pkg.returncode:
            pytest.skip('SDL3 development package required')
        cmd = [compiler, '-std=c++17', '-pthread', '-I' + str(ROOT / 'src/hooks'), str(source), str(actions),
               '-o', str(exe)] + shlex.split(pkg.stdout)
    result = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return exe


def run(tool, folder, command=(), **env):
    child = {k: v for k, v in os.environ.items()
             if not k.startswith(('DEFJAM_', 'RECOMP_TEXTURE_'))}
    child.update({k: str(v) for k, v in env.items()})
    result = subprocess.run([str(tool), *map(str, command)], cwd=folder, env=child,
                            capture_output=True, encoding='utf-8', check=True)
    return result.stdout.splitlines()


def test_installed_paths_respect_unicode_and_separate_data_from_settings(action_tool, tmp_path):
    data = tmp_path / 'Data NY 街'
    install, logs = tmp_path / 'App folder', tmp_path / 'Actual logs'
    settings = tmp_path / 'custom.ini'
    out = run(action_tool, tmp_path, DEFJAM_DATA=data, DEFJAM_INSTALL_DIR=install,
              DEFJAM_LOG_DIR=logs, TEST_SETTINGS_PATH=settings)
    assert [Path(x) for x in out] == [data, data / 'save', data / 'mods', install, settings, logs]


def test_checkout_paths_fall_back_to_settings_parent_and_working_directory(action_tool, tmp_path):
    settings = tmp_path / 'Preferences' / 'settings.ini'
    out = [Path(x) for x in run(action_tool, tmp_path, TEST_SETTINGS_PATH=settings)]
    assert out == [settings.parent, settings.parent / 'save', settings.parent / 'mods', tmp_path,
                   settings, tmp_path / 'logs']


def test_texture_root_override_is_the_inventory_folder(action_tool, tmp_path):
    mods = tmp_path / 'Other mods'
    out = run(action_tool, tmp_path, DEFJAM_DATA=tmp_path, RECOMP_TEXTURE_ROOT=mods)
    assert Path(out[2]) == mods


def test_missing_path_reports_failure_without_starting_a_file_manager(action_tool, tmp_path):
    missing = tmp_path / 'not-created'
    out = run(action_tool, tmp_path, ('missing', missing))
    assert out[0] == '0' and 'unavailable' in out[1] and str(missing) in out[1]


def test_remembered_setup_path_can_exceed_setting_string_limit_and_detects_move(action_tool, tmp_path):
    setup = tmp_path / ('setup-' + 'x' * 110)
    setup.write_text('fixture, never executed')
    setup.chmod(0o755)
    (tmp_path / 'launcher-setup-path.txt').write_text(str(setup) + '\n')
    out = run(action_tool, tmp_path, ('setup',), DEFJAM_DATA=tmp_path)
    assert Path(out[0]) == setup and out[1] == '1'
    setup.unlink()
    assert run(action_tool, tmp_path, ('setup',), DEFJAM_DATA=tmp_path)[1] == '0'


def test_explicit_setup_path_overrides_remembered_selection(action_tool, tmp_path):
    setup = tmp_path / 'selected-setup'
    setup.write_text('fixture, never executed')
    setup.chmod(0o755)
    (tmp_path / 'launcher-setup-path.txt').write_text('missing-old-setup\n')
    out = run(action_tool, tmp_path, ('setup',), DEFJAM_DATA=tmp_path, DEFJAM_SETUP_PATH=setup)
    assert Path(out[0]) == setup and out[1] == '1'
