"""src/hooks/test_telemetry.c against synthetic fighter records: the events it
emits, that it only reads guest memory, and the one opt-in write (the RNG seed)."""
import importlib.util
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
static unsigned char before[sizeof(memory)];
#define ACTOR(n) (0x3b92e0u + (n) * 0x12e8u)
static void put32(uint32_t a, uint32_t v) { memcpy(memory + a, &v, 4); }
static void putf(uint32_t a, float v) { memcpy(memory + a, &v, 4); }
static int changed(void) { return memcmp(memory, before, sizeof(memory)) != 0; }
static void snapshot(void) { memcpy(before, memory, sizeof(memory)); }

int main(void)
{
    uint32_t stack = 0x1000, winners = 0x20000, losers = 0x20010;
    int n, step;
    for (n = 0; n < 4; n++) put32(ACTOR(n) + 0x28, 0xFFFFFFFFu);      /* all slots unused */
    for (n = 0; n < 2; n++) {
        put32(ACTOR(n) + 0x28, (uint32_t)n);
        put32(ACTOR(n) + 0x2c, 4u + 5u * (uint32_t)n);
        putf(ACTOR(n) + 0x44, n ? 25.0f : -25.0f);
        putf(ACTOR(n) + 0x350, 100.0f);
        putf(ACTOR(n) + 0x354, 100.0f);
    }
    put32(ACTOR(1) + 0x934, 0x01000000u);                             /* +0x937 bit 0: CPU */
    put32(0x3b8f30, 1);                                               /* phase: fighting */
    put32(stack + 4, 111); put32(stack + 8, 333);
    put32(winners, 0); put32(winners + 4, 0xFFFFFFFFu);
    put32(losers, 1); put32(losers + 4, 0xFFFFFFFFu);

    /* Nothing before a match start, and nothing at all when disabled. */
    snapshot();
    defjam_test_step();
    defjam_test_result(winners, losers, 0x21);
    if (changed()) return 1;

    defjam_test_rng(stack);                 /* writes the seed only if RECOMP_TEST_RNG_SEED is set */
    printf("seed=%u\n", *(uint32_t *)(memory + stack + 4));
    snapshot();

    defjam_test_match_start();
    defjam_test_step();                                               /* step 1: match_start */

    /* The game repositions the player by 20 units in one step: not a walk. */
    put32(ACTOR(0) + 0x9e0, 0x8);
    defjam_test_step();
    putf(ACTOR(0) + 0x44, -5.0f);
    defjam_test_step();
    /* Then the player walks 0.25 units a step while right is held. */
    for (step = 0; step < 12; step++) {
        putf(ACTOR(0) + 0x44, -5.0f + 0.25f * (float)(step + 1));
        defjam_test_step();
    }
    /* The CPU fighter walking produces nothing. */
    put32(ACTOR(1) + 0x9e0, 0x4);
    for (step = 0; step < 12; step++) {
        putf(ACTOR(1) + 0x44, 25.0f - 0.25f * (float)(step + 1));
        defjam_test_step();
    }

    /* A hit by the CPU is not a player attack; a player hit with no press is not either. */
    defjam_test_hit(ACTOR(1), ACTOR(0), 0);
    defjam_test_hit(ACTOR(0), ACTOR(1), 0);
    /* The player presses an action, and three steps later the hit resolves and lands. */
    put32(ACTOR(0) + 0x9f0, 0x20);
    defjam_test_step();
    put32(ACTOR(0) + 0x9f0, 0);
    defjam_test_step(); defjam_test_step(); defjam_test_step();
    defjam_test_hit(ACTOR(0), ACTOR(1), 0);
    defjam_test_health_begin(ACTOR(1), 0);
    putf(ACTOR(1) + 0x350, 90.0f);
    defjam_test_health_notify(ACTOR(1), 0x3d);
    /* A health increase is not damage. */
    defjam_test_health_begin(ACTOR(1), 0);
    putf(ACTOR(1) + 0x350, 95.0f);
    defjam_test_health_notify(ACTOR(1), 0x3d);

    /* A result that is not decisive, then the decisive one, then the decided phase. */
    defjam_test_result(winners, losers, 0x100);
    putf(ACTOR(1) + 0x350, 0.0f);
    defjam_test_result(winners, losers, 0x21);
    put32(0x3b8f40, 0);
    put32(0x3b8f30, 2);
    defjam_test_step();
    defjam_test_step();                                               /* match_over is reported once */

    /* A wild list pointer is not followed. */
    defjam_test_result(0xFFFFFF00u, 4, 0x21);
    return 0;
}
'''


@pytest.fixture(scope='module')
def telemetry_tool(tmp_path_factory):
    compiler = shutil.which('cl')
    if not compiler:
        pytest.skip('Windows native telemetry fixture needs MSVC')
    folder = tmp_path_factory.mktemp('telemetry-native')
    include = folder / 'recomp/gen'
    include.mkdir(parents=True)
    (include / 'recomp_types.h').write_text(
        '#include <stdint.h>\nextern unsigned char memory[];\n#define MEM32(a) (*(uint32_t *)(memory+(a)))\n')
    driver = folder / 'driver.c'
    driver.write_text(DRIVER, encoding='utf-8')
    exe = folder / 'telemetry.exe'
    result = subprocess.run([compiler, '/nologo', '/O2', '/TC', '/I' + str(folder), '/I' + str(ROOT / 'src/hooks'),
                             '/I' + str(ROOT / 'src'), str(driver), str(ROOT / 'src/hooks/test_observations.c'),
                             str(ROOT / 'src/hooks/test_telemetry.c'), str(ROOT / 'src/hooks/recorder.c'), '/Fe:' + str(exe)],
                            cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return exe


def run(tool, **env):
    result = subprocess.run([str(tool)], env=dict(os.environ, **env), capture_output=True, text=True)
    events = [json.loads(line.split('] ', 1)[1]) for line in result.stderr.splitlines() if line.startswith('[TEST-EVENT]')]
    return result, events


def test_telemetry_is_silent_and_read_only_when_disabled(telemetry_tool):
    result, events = run(telemetry_tool, RECOMP_TEST_OBSERVATIONS='0', RECOMP_TEST_RNG_SEED='7')
    assert result.returncode == 0 and not result.stderr, result.stderr
    assert result.stdout.strip() == 'seed=111'            # the seed override needs the probes enabled too


def test_telemetry_events_from_a_synthetic_fight(telemetry_tool):
    result, events = run(telemetry_tool, RECOMP_TEST_OBSERVATIONS='1', RECOMP_TEST_RNG_SEED='')
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == 'seed=111'
    kinds = [e['event'] for e in events]
    assert kinds == ['rng', 'match_start', 'movement', 'attack', 'damage', 'result', 'result', 'match_over', 'result']
    rng, start, move, attack, damage, minor, decisive, over, wild = events
    assert rng == dict(rng, seed=111, increment=333, forced=False) and rng['step'] == 0
    assert [(f['slot'], f['character'], f['cpu']) for f in start['fighters']] == [(0, 4, 0), (1, 9, 1)]
    assert start['step'] == 1 and start['match'] == 1
    # The 20-unit reposition is not counted: 2 units of walking takes nine 0.25-unit steps.
    assert move['slot'] == 0 and 2.0 < move['distance'] < 2.6 and move['held'] == 8
    assert attack == dict(attack, slot=0, target_slot=1, pressed=32, steps_since_press=3)
    assert damage == dict(damage, slot=1, cpu=1, health_before=100, health_after=90)
    assert minor['decisive'] is False and minor['code'] == 0x100
    assert decisive == dict(decisive, code=33, decisive=True, draw=False, time_up=False, winners=[0], losers=[1])
    assert over['winner_slot'] == 0 and over['phase'] == 2 and over['step'] > decisive['step']
    assert wild['winners'] == [] and wild['losers'] == []
    steps = [e['step'] for e in events]
    assert steps == sorted(steps)


def test_rng_seed_override_is_the_only_write(telemetry_tool):
    result, events = run(telemetry_tool, RECOMP_TEST_OBSERVATIONS='1', RECOMP_TEST_RNG_SEED='4242')
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == 'seed=4242'
    assert events[0] == dict(events[0], event='rng', seed=4242, forced=True)


def test_synthetic_fight_satisfies_the_combat_checks(telemetry_tool):
    result, _ = run(telemetry_tool, RECOMP_TEST_OBSERVATIONS='1', RECOMP_TEST_RNG_SEED='')
    spec = importlib.util.spec_from_file_location('evidence', ROOT / 'scripts/test_evidence.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # Drop the deliberately wild second decisive record; the checker rejects two.
    lines = result.stderr.splitlines()
    log = '\n'.join(lines[:-1] + ['[FUNCCALL] Game.GetMatchSummary(0)'])
    checks = {c['name']: c['status'] for c in module.combat_checks(log)}
    assert set(checks.values()) == {'pass'}, checks
    assert module.combat_checks('\n'.join(lines))[5]['status'] == 'fail'
