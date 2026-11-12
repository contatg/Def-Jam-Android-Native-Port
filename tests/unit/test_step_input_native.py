"""Step-timed input (RECOMP_TEST_INPUT) in src/hooks/test_telemetry.c: the pad-table
word of a scripted slot is replaced by the file's value for the current fight step,
and nothing else is touched."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test_observations.h"
unsigned char memory[0x400000];
#define ACTOR(n) (0x3b92e0u + (n) * 0x12e8u)
#define PAD(n) (0x3b8f48u + (n) * 0x20u)
static void put32(uint32_t a, uint32_t v) { memcpy(memory + a, &v, 4); }
static uint32_t get32(uint32_t a) { uint32_t v; memcpy(&v, memory + a, 4); return v; }

int main(void)
{
    int n, step;
    for (n = 0; n < 4; n++) put32(ACTOR(n) + 0x28, 0xFFFFFFFFu);
    for (n = 0; n < 2; n++) put32(ACTOR(n) + 0x28, (uint32_t)n);
    put32(0x3b8f30, 1);
    /* Before a match nothing is written, whatever the file says. */
    put32(PAD(0), 0xAAAA); put32(PAD(1), 0xBBBB);
    defjam_test_input(ACTOR(0));
    printf("pre %x %x\n", get32(PAD(0)), get32(PAD(1)));
    defjam_test_match_start();
    for (step = 1; step <= 12; step++) {
        defjam_test_step();
        /* What the host's own input path left there: must not survive for a scripted slot. */
        put32(PAD(0), 0xAAAA); put32(PAD(0) + 4, 0x1234); put32(PAD(1), 0xBBBB);
        defjam_test_input(ACTOR(0));
        defjam_test_input(ACTOR(1));        /* slot 1 is not in the file: left alone */
        defjam_test_input(ACTOR(0) + 4);    /* not a record: ignored */
        printf("%d %x %x %x\n", step, get32(PAD(0)), get32(PAD(0) + 4), get32(PAD(1)));
    }
    return 0;
}
'''

SCRIPT = '''# slot first last buttons
0 3 5 8
0 5 6 20   # overlaps: bits are combined
bad line
7 1 2 1
0 9 8 40
0 10 10 80
'''


@pytest.fixture(scope='module')
def input_tool(tmp_path_factory):
    compiler = shutil.which('cl')
    if not compiler:
        pytest.skip('Windows native step-input fixture needs MSVC')
    folder = tmp_path_factory.mktemp('step-input-native')
    include = folder / 'recomp/gen'
    include.mkdir(parents=True)
    (include / 'recomp_types.h').write_text(
        '#include <stdint.h>\nextern unsigned char memory[];\n#define MEM32(a) (*(uint32_t *)(memory+(a)))\n')
    driver = folder / 'driver.c'
    driver.write_text(DRIVER, encoding='utf-8')
    exe = folder / 'input.exe'
    result = subprocess.run([compiler, '/nologo', '/O2', '/TC', '/I' + str(folder), '/I' + str(ROOT / 'src/hooks'),
                             '/I' + str(ROOT / 'src'), str(driver), str(ROOT / 'src/hooks/test_observations.c'),
                             str(ROOT / 'src/hooks/test_telemetry.c'), str(ROOT / 'src/hooks/recorder.c'), '/Fe:' + str(exe)],
                            cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    script = folder / 'input.txt'
    script.write_text(SCRIPT, encoding='ascii')
    return exe, script


def rows(stdout):
    return {int(line.split()[0]): line.split()[1:] for line in stdout.splitlines()[1:] if line}


def test_step_input_replaces_only_the_scripted_slot_by_step(input_tool):
    exe, script = input_tool
    result = subprocess.run([str(exe)], env=dict(os.environ, RECOMP_TEST_OBSERVATIONS='1', RECOMP_TEST_INPUT=str(script)),
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == 'pre aaaa bbbb'
    table = rows(result.stdout)
    expected = {1: '0', 2: '0', 3: '8', 4: '8', 5: '28', 6: '20', 7: '0', 8: '0', 9: '0', 10: '80', 11: '0', 12: '0'}
    assert {step: values[0] for step, values in table.items()} == expected
    # Only the buttons word is written; the unscripted slot keeps what the host put there.
    assert all(values[1] == '1234' and values[2] == 'bbbb' for values in table.values())
    loaded = [json.loads(line.split('] ', 1)[1]) for line in result.stderr.splitlines() if 'input_loaded' in line]
    assert loaded == [dict(event='input_loaded', intervals=3, slots=1)]


@pytest.mark.parametrize('probes,use_script', [('1', False), ('0', True)])
def test_step_input_is_inert_without_the_file_or_the_probes(input_tool, probes, use_script):
    exe, script = input_tool
    env = dict(os.environ, RECOMP_TEST_OBSERVATIONS=probes, RECOMP_TEST_INPUT=str(script) if use_script else '')
    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
    assert result.returncode == 0
    assert all(values == ['aaaa', '1234', 'bbbb'] for values in rows(result.stdout).values())
