"""Compile the startup hook with synthetic guest memory and the real settings registry.

Exercise opt-in/file/environment precedence, the one-shot boundary, and the guest
stack/register contract of the controller initialization and the full audio
initialization (the menu sound effects) that skipping the startup movies would
otherwise lose.
"""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp/gen/recomp_types.h"
#include "pc_ui.h"
unsigned char memory[8192];
uint32_t g_eax = 0x1234, g_ecx = 0x100, g_edx = 0x5678, g_esp = 0x1000;
static int boot_calls;
static int audio_calls;
static int unlock_calls;
void defjam_skip_intro_enter(void);
void defjam_unlock_all_on_funccall(const char *text);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return 0; }
void sub_000158F0(void) { unlock_calls++; g_esp += 4; }
void sub_00055640(void)
{
    assert(g_esp == 0x1000 - 12);
    assert(!MEM32(g_esp) && !MEM32(g_esp + 4) && !MEM32(g_esp + 8));
    boot_calls++;
    g_eax = 77; g_ecx = 88; g_edx = 99;
    g_esp += 12;
}
void sub_0003DE20(void)
{
    /* AudioInit(1), cdecl: return word then one argument; the caller drops the argument. */
    assert(g_esp == 0x1000 - 8);
    assert(!MEM32(g_esp) && MEM32(g_esp + 4) == 1);
    audio_calls++;
    g_eax = 11; g_ecx = 22; g_edx = 33;
    g_esp += 4;
}
int main(int argc, char **argv)
{
    RecompSetting settings[16];
    size_t n = pc_ui_settings(settings, 16);
    recomp_settings_register(settings, n);
    recomp_settings_load(argv[1]);
    MEM32(0x19C) = (uint32_t)atoi(argv[2]);
    MEM32(0x198) = 42;
    defjam_skip_intro_enter();
    printf("first %u boots %d audio %d\n", MEM32(0x19C), boot_calls, audio_calls);
    assert(g_eax == 0x1234 && g_ecx == 0x100 && g_edx == 0x5678 && g_esp == 0x1000);
    assert(MEM32(0x198) == 42); /* the generated selector still owns the current screen */
    /* A later UI edit and another legal-screen request must not restart the skip. */
    recomp_settings_set("debug", "skip_intro", 1);
    MEM32(0x19C) = 0;
    defjam_skip_intro_enter();
    printf("later %u boots %d audio %d\n", MEM32(0x19C), boot_calls, audio_calls);
    if (argc > 3) {
        /* With startup skipped there is no Game.StartMainMenu(0) event. */
        defjam_unlock_all_on_funccall("Control.GetScreenInfo(intMain/mainMenu.swf)");
        assert(unlock_calls == 1); /* The first menu must already have the unlocks. */
        defjam_unlock_all_on_funccall("Control.GetScreenInfo(battle/cmtype.swf)");
        printf("unlock %d\n", unlock_calls);
        assert(g_eax == 0x1234 && g_ecx == 0x100 && g_edx == 0x5678 && g_esp == 0x1000);
    }
    recomp_settings_save(argv[1]);
    return 0;
}
'''


@pytest.fixture(scope='module')
def startup_tool(tmp_path_factory):
    if not (ROOT / 'tools/xboxrecomp/src/settings/recomp_settings.c').is_file():
        pytest.skip('native startup fixture needs the initialized toolkit submodule')
    compiler = shutil.which('cl') if os.name == 'nt' else shutil.which('cc')
    if not compiler:
        pytest.skip('native startup fixture needs a C compiler')
    folder = tmp_path_factory.mktemp('skip-intro-native')
    inc = folder / 'recomp/gen'
    inc.mkdir(parents=True)
    (inc / 'recomp_types.h').write_text(
        '#include <stdint.h>\nextern unsigned char memory[];\n'
        'extern uint32_t g_eax, g_ecx, g_edx, g_esp;\n'
        'typedef void (*recomp_func_t)(void);\n'
        '#define MEM32(a) (*(uint32_t *)(memory + (a)))\n'
        '#define MEM8(a) (*(uint8_t *)(memory + (a)))\n')
    (folder / 'host.h').write_text('/* No host APIs needed by this fixture. */\n')
    driver = folder / 'driver.c'
    driver.write_text(DRIVER)
    exe = folder / ('startup.exe' if os.name == 'nt' else 'startup')
    includes = [folder, ROOT / 'src/hooks', ROOT / 'tools/xboxrecomp/src/settings']
    sources = [driver, ROOT / 'src/patches/skip_intro.c', ROOT / 'src/patches/dev_unlock.c', ROOT / 'src/hooks/pc_ui_settings.c',
               ROOT / 'tools/xboxrecomp/src/settings/recomp_settings.c']
    if os.name == 'nt':
        cmd = [compiler, '/nologo', '/O2', '/TC', *['/I' + str(p) for p in includes],
               *map(str, sources), '/Fe:' + str(exe)]
    else:
        cmd = [compiler, '-O2', *['-I' + str(p) for p in includes], *map(str, sources), '-o', str(exe), '-pthread']
    result = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return exe


@pytest.mark.parametrize('file_value,env_value,pending,want,boots', [
    (None, None, 0, 0, 0),
    ('false', None, 0, 0, 0),
    ('true', None, 0, 1, 1),
    ('true', '0', 0, 0, 0),
    ('true', 'false', 0, 0, 0),
    ('false', '1', 0, 1, 1),
    (None, 'true', 0, 1, 1),
    ('true', None, 14, 14, 0),
])
def test_startup_skip_boundary_and_settings(startup_tool, tmp_path, file_value, env_value, pending, want, boots):
    ini = tmp_path / 'settings.ini'
    ini.write_text('' if file_value is None else '[debug]\nskip_intro = ' + file_value + '\n')
    env = dict(os.environ)
    env.pop('RECOMP_DEV_DEBUG', None)
    env.pop('RECOMP_SKIP_INTRO', None)
    if env_value is not None:
        env['RECOMP_SKIP_INTRO'] = env_value
    result = subprocess.run([str(startup_tool), str(ini), str(pending)], env=env,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.splitlines() == [f'first {want} boots {boots} audio {boots}',
                                      f'later 0 boots {boots} audio {boots}']
    assert 'skip_intro = true' in ini.read_text()


def test_skip_intro_and_unlock_can_be_enabled_together(startup_tool, tmp_path):
    ini = tmp_path / 'settings.ini'
    ini.write_text('[debug]\nskip_intro = true\n')
    env = dict(os.environ, RECOMP_SKIP_INTRO='1', RECOMP_UNLOCK_ALL='1')
    env.pop('RECOMP_DEV_DEBUG', None)
    result = subprocess.run([str(startup_tool), str(ini), '0', 'unlock'], env=env,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.splitlines() == ['first 1 boots 1 audio 1', 'later 0 boots 1 audio 1', 'unlock 2']
