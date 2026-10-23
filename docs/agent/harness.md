# Driving the game unattended

Moved here from the agent instructions (`AGENTS.md` §4b) unchanged. Read it before scripting a run.

`scripts/harness.py` runs the game with scripted pad input, takes captures and summarises the log; its
docstring has the syntax. The front end logs each call into the game as a `[FUNCCALL]` line, and a stage is
`<anchor>=<presses>`: the presses start when a line containing the anchor appears.

```
python scripts/harness.py routes                    # boot, fight, crib, gym, intro
python scripts/harness.py run intro --png           # Story from a new ID: the cutscenes, to the creator
python scripts/harness.py run gym --stage "getscreeninfo(story/gym=down@4;down@6;a@9;x@16"
python scripts/harness.py soak --boots 20           # stops at a boot that stops presenting and dumps its state
python scripts/regress.py [--quick] [--only fight,intro]
python scripts/scenario_suite.py run fight-result --fixture logs/fixtures/NAME --require-combat
```

- Every harness route and soak boot uses a complete-save-root guard, including profiles and caches.
  The run fails unless restoration is byte-identical. Keep only three successful backups; retain every
  failed backup. For rebase tests use the preserved disposable `runtime-data` root, never the original save.
- Harness runs have every fighter and venue unlocked: `harness.py` sets `RECOMP_UNLOCK_ALL=1` (`--env RECOMP_UNLOCK_ALL=0`, or `scenario_suite.py --no-unlock`, turns it off). `replay` and `repeat` run without it, because it changes the pinned fight. `RECOMP_START_FIGHTERS` / `RECOMP_START_VENUE` choose the match (`docs/09-testing-harness.md`).
- Captures land in `logs/shots/<route>/` and remain local. Only the two owner-approved README visuals (D58) may be committed, with exact CI fingerprints.
- `scripts/scenario_suite.py` runs one route on a throw-away copy of a hashed save fixture and writes a
  JSON/JUnit/HTML report under `logs/scenarios/`. `--require-combat` asserts movement, a player attack,
  damage and the match result from game state (`src/hooks/test_telemetry.c`, on with
  `RECOMP_TEST_OBSERVATIONS=1`). The full `regress.py` runs it as the `combat` check, with the
  four-fighter Terrordome match. `--step-input default --rng-seed N` makes a fight repeat; the `replay`
  check compares thirty seconds of game state with `tests/golden/fight-stream.json` and fails when a
  change alters the simulation (`--only repeat --update-golden` if that is intended).
  `versus` (two emulated pads, in the full run), `ffa-result`, `two-matches` and `visual` (still screens
  against reviewed baselines in `<data>/test-baselines/`, never committed) are further checks; the last
  three are `--only`. In a pad script `anchor#N` waits for the Nth call; an anchor cannot contain a comma.
  Guide: `docs/09-testing-harness.md`.
- Input (v0.3.0, `docs/10-input.md`). `settings.ini` and `RECOMP_SETTINGS=none` runs leave the host input layer off so the harness keeps its one scripted pad. To exercise the real paths in a route add `--env RECOMP_INPUT_HOST=1 --env RECOMP_INPUT_IGNORE_FOCUS=1 --env RECOMP_PAD_HOST=1`, then either `--env RECOMP_INPUT_NO_PADS=1 --env RECOMP_PAD_SCRIPT_KEYS=1` (the scripted pad of the keyboard's player becomes key presses; `--env RECOMP_KEYBOARD_PLAYER=2` makes the keyboard player 2 beside a scripted pad 1) or `--env RECOMP_INPUT_NO_PADS=1 --env RECOMP_INPUT_VIRTUAL_PADS=2 --env RECOMP_PAD_SCRIPT_VIRTUAL=1` (SDL virtual gamepads pressed by the script, through the real SDL path); `--env RECOMP_RUMBLE_LOG=1` logs the rumble the game sends. The launcher never shows in a `RECOMP_SETTINGS=none` or scripted run; to test the overlay add `--env RECOMP_INPUT_VIRTUAL_CHORD_MS=40000` (virtual pad 1 opens it with both stick clicks and closes it with B; see `docs/launcher-and-overlay.md`).
- Scenarios on demand (`docs/research/scenario-launching.md`): `scripts/scenarios.py fixture add NAME --story ID`
  keeps a save at a Story point in a library in the data folder; `scenarios.py generate --mode --venue [--p1 --cpu]
  --fixture NAME [--seed N] [--record] [--expect-character SLOT:INDEX]` composes and runs a fight route (the
  calibrated combinations are exactly the harness's routes); `scenarios.py cutscene --fixture NAME --inject
  "anchor#1=>command"` reaches a Story point and sends a front-end command. `scenario_suite.py run ... --record`
  turns the play-test recorder on and analyses its session and bundles. `scripts/crowd-lab.py` tests the crowd
  controls and learns masks for fight baselines.
- A new screen to reach: run the nearest route with extra `--stage`s and `--shots`, look at the captures
  (`harness.py montage`), and add the stage to `ROUTES` once it is right.
- Scripts written for one investigation belong in the session's scratch directory. Anything used twice
  belongs in `scripts/`, with a docstring and, if it has logic worth breaking, a test in `tests/unit`.
