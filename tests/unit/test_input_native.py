"""The input settings table (src/hooks/pc_input.c) with the toolkit's settings and mapping code:
every default is valid, nothing is bound twice for the keyboard player, the file round-trips, and a
bad value falls back instead of breaking the player's pad."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOLKIT = ROOT / 'tools/xboxrecomp'

DRIVER = r'''
#include <stdio.h>
#include <string.h>
#include "pc_input.h"
#include "pc_ui.h"
#include "input_map.h"
#include "recomp_settings.h"

static RecompSetting table[96];

/* The USB model's hook, which pc_input.c hands the host layer; not under test here. */
void xbox_UsbSetPadCount(int n) { (void)n; }

int main(int argc, char **argv)
{
    const char *ini = argv[1];
    size_t n = pc_input_settings(table, 64), i, j;
    n += pc_ui_settings(table + n, 16);
    char text[RECOMP_SETTING_TEXT_MAX];
    int bad = 0;
    int owner[INPUT_KEY_COUNT];
    InputBindings b;
    InputPadMap def;

    printf("count %u\n", (unsigned)n);
    /* No two settings share a section and key. */
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (!strcmp(table[i].section, table[j].section) && !strcmp(table[i].key, table[j].key)) bad++;
    printf("duplicate_keys %d\n", bad);

    recomp_settings_register(table, n);
    recomp_settings_load(ini);

    /* Every keyboard default is understood in full, and no key is bound to two controls. */
    memset(owner, -1, sizeof(owner));
    memset(&b, 0, sizeof(b));
    bad = 0;
    {
        int rejected = 0, empty = 0, shared = 0, c;
        for (c = 0; c < INPUT_CONTROL_COUNT; c++) {
            int k;
            recomp_settings_get_text("keyboard", input_control_name(c), text, sizeof(text), "");
            rejected += input_bindings_parse(&b, c, text);
            if (!b.count[c]) empty++;
            for (k = 0; k < b.count[c]; k++) {
                int code = b.key[c][k];
                if (owner[code] >= 0 && owner[code] != c) shared++;
                owner[code] = c;
            }
        }
        printf("keyboard rejected %d empty %d shared %d\n", rejected, empty, shared);
    }

    /* The pad defaults are the identity map. */
    input_padmap_defaults(&def);
    bad = 0;
    for (i = 0; i < INPUT_PAD_CONTROL_COUNT; i++)
        if (input_source_from_name(recomp_settings_get_text("gamepad", input_control_name((int)i), text, sizeof(text), "")) != def.source[i]) bad++;
    printf("padmap_differs %d\n", bad);
    printf("deadzone %d %d trigger %d\n", recomp_settings_get("gamepad", "left_deadzone", -1),
           recomp_settings_get("gamepad", "right_deadzone", -1),
           recomp_settings_get("gamepad", "trigger_threshold", -1));
    printf("rumble %d players %d keyboard_player %d\n", recomp_settings_get("input", "rumble", -1),
           recomp_settings_get("input", "players", -1), recomp_settings_get("input", "keyboard_player", -1));
    printf("a=%s\n", recomp_settings_get_text("keyboard", "a", text, sizeof(text), "?"));
    printf("source_a=%d\n", input_source_from_name(recomp_settings_get_text("gamepad", "a", text, sizeof(text), "")));
    printf("source_b=%d\n", input_source_from_name(recomp_settings_get_text("gamepad", "b", text, sizeof(text), "")));
    /* The launcher shows on every launch unless skipped; flags and a held Shift override the setting; a test run never shows it. */
    printf("decision %d %d %d %d %d %d %d\n",
           pc_launcher_wanted(0, 0, 0, 0, 0), pc_launcher_wanted(1, 0, 0, 0, 0), pc_launcher_wanted(1, 0, 0, 1, 0),
           pc_launcher_wanted(1, 1, 0, 0, 0), pc_launcher_wanted(0, 0, 1, 0, 0), pc_launcher_wanted(0, 1, 0, 1, 1),
           pc_launcher_wanted(0, 1, 1, 0, 0));
    {
        char key[RECOMP_SETTING_TEXT_MAX], pad[RECOMP_SETTING_TEXT_MAX];
        recomp_settings_get_text("ui", "overlay_key", key, sizeof(key), "?");
        recomp_settings_get_text("ui", "overlay_pad", pad, sizeof(pad), "?");
        printf("ui %s|%s|%d|%d\n", key, pad, recomp_settings_get("ui", "scale", -1),
               recomp_settings_get("launcher", "skip", -1));
    }
    printf("save %d\n", recomp_settings_save(ini));
    return 0;
}
'''

INI = '''[input]
keyboard_player = 2
rumble = 150
[gamepad]
left_deadzone = 25
a = east
b = nonsense
[keyboard]
a = F, Mouse3
x = banana
'''


@pytest.fixture(scope='module')
def tool(tmp_path_factory):
    compiler = shutil.which('cl')
    if not compiler:
        pytest.skip('Windows native input fixture needs MSVC')
    if not (TOOLKIT / 'src/input/input_host.c').exists():
        pytest.skip('toolkit without the host input layer')
    folder = tmp_path_factory.mktemp('input-native')
    driver = folder / 'driver.c'
    driver.write_text(DRIVER, encoding='utf-8')
    exe = folder / 'input.exe'
    sources = [driver, ROOT / 'src/hooks/pc_input.c', ROOT / 'src/hooks/pc_ui_settings.c', TOOLKIT / 'src/input/input_map.c',
               TOOLKIT / 'src/input/input_host.c', TOOLKIT / 'src/settings/recomp_settings.c']
    cmd = [compiler, '/nologo', '/O2', '/TC', '/I' + str(ROOT / 'src/hooks'), '/I' + str(TOOLKIT / 'src/input'),
           '/I' + str(TOOLKIT / 'src/settings'), '/I' + str(TOOLKIT / 'src')] + [str(s) for s in sources] + [
           '/Fe:' + str(exe), '/link', 'xinput.lib', 'winmm.lib', 'user32.lib']
    result = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return exe, folder


def run(tool, ini_text):
    exe, folder = tool
    ini = folder / 'settings.ini'
    if ini_text is None:
        ini.unlink(missing_ok=True)
    else:
        ini.write_text(ini_text, encoding='ascii')
    result = subprocess.run([str(exe), str(ini)], cwd=folder, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    return {line.split(' ', 1)[0].split('=')[0]: line for line in result.stdout.splitlines()}, ini


def test_defaults_are_valid_and_complete(tool):
    out, ini = run(tool, None)
    assert out['count'] == 'count 61'
    assert out['decision'] == 'decision 1 0 1 1 0 0 0'
    assert out['ui'] == 'ui F1|guide, left_stick_click+right_stick_click|100|0'
    assert out['duplicate_keys'] == 'duplicate_keys 0'
    assert out['keyboard'] == 'keyboard rejected 0 empty 0 shared 0'
    assert out['padmap_differs'] == 'padmap_differs 0'
    assert out['deadzone'] == 'deadzone 15 15 trigger 5'
    assert out['rumble'] == 'rumble 100 players 0 keyboard_player 0'
    assert out['a'] == 'a=K, Space, Mouse1'
    # The file written for a first run lists every setting with its help.
    text = ini.read_text(encoding='ascii')
    for section in ('[input]', '[gamepad]', '[keyboard]'):
        assert section in text
    assert 'left_stick_up = W' in text and 'right_stick_right = Right' in text
    assert 'a = south' in text


def test_file_values_load_and_bad_ones_fall_back(tool):
    out, ini = run(tool, INI)
    assert out['keyboard'] == 'keyboard rejected 1 empty 1 shared 0'      # "banana" is not a key: X is unbound
    assert out['deadzone'] == 'deadzone 25 15 trigger 5'
    assert out['rumble'] == 'rumble 100 players 0 keyboard_player 4'      # 150 clamps to 100; "2" is the fifth choice
    assert out['a'] == 'a=F, Mouse3'
    assert out['source_a'] == 'source_a=2'                                # a = east
    assert out['source_b'] == 'source_b=-1'                               # "nonsense": pc_input keeps the default
    # What was read is what is written back: the settings round trip.
    text = ini.read_text(encoding='ascii')
    assert 'a = F, Mouse3' in text and 'left_deadzone = 25' in text


def test_every_setting_is_in_the_reference(tool):
    """docs/settings-reference.md names every key of the settings file the game writes."""
    _, ini = run(tool, None)
    reference = (ROOT / 'docs/settings-reference.md').read_text(encoding='utf-8')
    section, missing = None, []
    for line in ini.read_text(encoding='ascii').splitlines():
        if line.startswith('['):
            section = line.strip('[]')
            assert ('## [' + section + ']') in reference, section
        elif '=' in line and not line.startswith(';'):
            key = line.split('=', 1)[0].strip()
            if ('`' + key + '`') not in reference:
                missing.append(section + '.' + key)
    assert not missing, 'not in docs/settings-reference.md: ' + ', '.join(missing)
