# Test plan

The gameplay test harness is described in [09-testing-harness.md](09-testing-harness.md).
Since v0.2.2 the full regression run includes a combat check that asserts movement, a
player attack, damage and the match result from the game's own state, a four-fighter
Terrordome match, and audio, frame-pacing and memory gates. Input timed in simulation
steps, approved visual baselines and sound-to-event checks are not done. The milestone
exit criteria below are unchanged.

For current hosted CI coverage, see [Release process](releasing.md#ci-coverage).
The milestone gates below include planned checks as well as completed ones;
[PROGRESS.md](../PROGRESS.md) and the [rebase acceptance](research/toolkit-rebase-acceptance.md)
record the actual results. Hosted CI has no game dump and cannot validate gameplay.

Principle: CI never sees game data. Everything that needs the dump runs locally, is scripted, and writes a log
that is summarized in `PROGRESS.md`. This copies the pattern of UnleashedRecomp and Zelda64Recomp:
build-validation CI, local "does it boot" gates, and manual gameplay checklists.

## Layers
| Layer | What | How | Where |
|---|---|---|---|
| L0 Hygiene | no game assets, binaries, generated guest source or copied guest bodies in Markdown | `ci.yml` hygiene (`scripts/check-source-tree.py`); private-to-public transitions also audit history | CI, every push; history before publication |
| L1 Unit | project Python helpers and native save compatibility; toolkit CPU/disassembly tests; registered Release kernel bridge/directory, USB and VSH/WARP fixtures | `pytest tests/unit`; toolkit pytest; separate fixture CMake/CTest projects | CI and local; additional fixtures remain local |
| L2 Build | runtime libs build on MSVC (CI); full game exe builds locally | preset `ci-runtime-only` (CI); `scripts/build.ps1` (local) | CI / local |
| L3 Boot smoke | stages S0 to S4 from log markers, watchdog for hangs | `tests/smoke/boot-smoke.ps1 -TimeoutSecs 30`; exit code = stage | local, after every bring-up change |
| L4 Golden | screenshots vs the xemu reference, which is only available up to the loading screen | `tests/golden/` holds only our own screenshot hashes and diff masks, never assets; compare with a small Python PSNR/SSIM script (todo) | local |
| L5 Gameplay checklist | manual, per milestone (below) | `docs/checklists/*.md` (todo) | local |
| L6 Performance | frame-time histogram over a 60 s fight at 1080p and 1280x800 (Deck) | runtime frame stats if available, else PresentMon | local |

## Boot smoke stages (`tests/smoke/boot-smoke.ps1`)
| Stage | Marker |
|---|---|
| S0 | process started, "XBE loaded" |
| S1 | "=== Initialization complete ===" (memory layout, kernel, bridge) |
| S2 | "Starting game..." and no exception within the first seconds |
| S3 | D3D11 device created and at least one Present |
| S4 | full timeout elapsed with no crash and a silent watchdog |

## Milestone gates
- **M1 link**: `cmake --build` exit 0; zero `LNK2019`.
- **M2 EA logo**: smoke stage 3 or higher; a window appears; the log shows device creation and at least one Present.
- **M3 title screen**: stage 4 for 60 s; the title card renders and START advances to the main menu. No xemu golden diff is possible here, because no emulator reaches this screen; see the baseline section below.
- **M4a menu**: the main menu draws recognisably (a golden frame, as for M3), the d-pad moves the selection and A/B enter and leave a sub-menu, and the memory-card popup is visible.
- **M4b match setup**: from the menu, Battle -> a mode -> fighter select -> venue select -> the fight's loading screen, driven by `RECOMP_PAD_SCRIPT`, and the fight starts (the arena draws).
- **M4c fight**: one fighter answers the pad, hits land (health bars move), a KO ends the round, and the game returns to the menu.
- **M4e GPU vertex programs**: a round presents 60 frames a second in the debug build (`[D3D] 2.0s` lines), the round clock at 1.2 ticks a step, no crash over three fights; the M2, M3 and M4a goldens match; a fight capture differs from `RECOMP_VSH_GPU=0` only by perspective-correct interpolation.
- **M4f looks like the console**: captures of a fight and of the Story intro (scripted, `RECOMP_TRANS_SHOT_SECS`) set beside console video frames; Vlad signs off the route up to the tutorial fight; the M2, M3 and M4a goldens match after every graphics change.
- **M4d match (the full M4 checklist)**: checklist. Two pads detected via XInput. Music, announcer and hit SFX audible. FMV intro plays or is skippable. Load Battle, Free for All. Complete a KO. Return to menu. Private bytes stable over three fights.
- **M5 Deck** (superseded by D95: the Deck runs the native Linux build, not Proton): the M4 checklist under Proton on SteamOS; Steam Input mapping; no keyboard needed; 1280x800 letterboxing correct.
- **M6 PC features** (D63, one gate per release). v0.2.0: the executable starts with no environment set and no `run.ps1`; `settings.ini` round-trips (unit test) and an environment variable overrides it; Alt+Enter and F11 toggle borderless fullscreen and back without a crash or a lost frame counter; in a 16:9 window the picture is 4:3 with black bars (letterbox maths unit-tested, a window capture confirms); a fight still presents 60 frames a second with `vsync` on and off, with `RECOMP_PRESENT_PACING=1` showing a 16.67 ms median and no window of two seconds with more than two late presents; the goldens match. v0.3.0 (input): unit tests for binding parsing and resolution, deadzone arithmetic and pad remapping (`tools/xboxrecomp/tests/input_map`), for slot assignment, hot-plug, remapping, focus and rumble against SDL's virtual gamepads (`tests/input_host`), and for the settings defaults and file round trip (`tests/unit/test_input_native.py`); scripted matches that drive the keyboard path end to end (`RECOMP_PAD_SCRIPT_KEYS=1`: One on One as player 1, `versus` with the keyboard as player 2) and a scripted match that still passes with a second, idle controller on the hub; the rumble the game sends decodes (`RECOMP_RUMBLE_LOG=1`); `scripts/regress.py` stays green. By hand, the owner's: a DualSense and an Xbox pad each play a match, the keyboard plays a match, rebinding in `settings.ini` survives a restart, rumble is felt, a pad and the keyboard play against each other, and two pads fight each other. v0.4.0 (launcher and overlay): the launcher shows on every launch unless skipped (`pc_launcher_wanted` unit-tested for the flags, Shift and test runs), edits settings by mouse, keyboard and pad, captures a key binding and saves it to `settings.ini`, and its Play starts the game; the overlay opens with F1 or the pad chord (a virtual pad in the real game), draws over the picture without appearing in captures, applies full screen and window size live, polls the game at rest while open (host-layer unit test) and closes again; every setting is in the settings reference (unit test); `scripts/regress.py` stays green. By hand, the owner's: the launcher and overlay with a mouse, a keyboard and a pad; changing a key binding and a pad button and seeing them work in a match; that the overlay's controllers page shows the pads and tests rumble. D84/D85: HD precedes widescreen; public v0.5.0 is the native platform release, so HD is proposed as v0.6.0. Widescreen remains unscheduled ToDo: a 16:9 fight shows more arena at the sides with round characters and the HUD in place.

## HD texture release gates (D83–D86)

Renderer support and art coverage are separate claims. The initial Windows prototype is not release acceptance.

- Known-answer full source identities include palettes, omit addresses/pitch padding, and distinguish tiny pixel or palette changes; validate swizzled/linear/palette/DXT lossless dumps.
- Enabled PNG and classic BC1/BC2/BC3 DDS replacements visibly render with original UV/atlas geometry, correct alpha/mips/addressing/filtering across stages 0–3. Verify linear row padding and integer scales.
- Missing, malformed and unsupported assets fall back; manifest schema/path validation, precedence, restart controls, bounded memory, bound-stage-safe eviction and temporary-pressure recovery pass focused fixtures.
- Original text, movie frames, render surfaces and unsupported dynamic assets stay live. Review alpha-test/additive/channel semantics rather than applying opacity processing blindly.
- A representative 30–50-image pilot covers UI/logos, palette/atlas/cutout, fighter faces/clothes/tattoos, venue/props and repeat seams. Compare 2x/4x world assets; preserve identities and branded lettering.
- Release and Debug quick/full regressions pass on reconciled public v0.5.0 sources. Packs off retain existing 4:3 goldens. Pack-on evidence is separately reviewed, including creator/name entry, fight HUD, multiple fighters/venues, Story/movie transitions and multi-fight soak.
- Record solo-baseline versus enabled cold/warm load time, frame pacing and memory with dumping/inference off. Keep the game's existing simulation and frame-rate gates; investigate any additional hitch or sustained regression.
- Preserve macOS/Linux build compatibility after reconciling the native port; disclose that initial WIC/CNG replacement loading is Windows-only unless a backend-neutral implementation is completed and tested. (Superseded: texture packs load on macOS since v0.6.1 and on Linux since v0.7.2.)
- Fresh-checkout source hygiene/CI pass. Owner accepts the concrete pack/gameplay build before release. Ship source tooling/docs/recipes only; full texture coverage, font host integration and HD video playback must not be claimed from a small pilot.

## Setup updater release gates (D100–D101)

The [setup update acceptance plan](setup-update-test-plan.md) defines required
operation, option, network, integrity, interruption and native platform coverage.
Clean updates preserve player data; historical releases are not downgrade targets.
Future capable versions need explicit downgrade compatibility and acceptance.
Linux product acceptance targets Steam Deck; Ubuntu CI is portable validation.
Game-free CI and packaged smoke cannot replace owner-dump runs on Windows, Apple
Silicon and Deck. No updater gate is complete until its results ledger has evidence.

## Regression discipline
- `python scripts/regress.py` is the regression run: unit tests, the M2/M3/M4a golden frames, a scripted
  fight, Story from a new ID through both cutscenes to the creator, Story with a saved profile into the crib
  and into Learn Moves, and a boot soak. Each route also fails on a `[CRASH]`, a `[WATCHDOG]`, a Direct3D
  debug-layer message, a truncated batch, a directory search answered with the wrong name, or a save area
  that is not byte-identical afterwards. `--quick` (unit, goldens, fight) after every change; the full run
  before a commit that touches the runtime, the lifter or the renderer.
- A new feature or a fixed bug that a run can show adds a check there, or a route in `scripts/harness.py`.
- Every bring-up fix is one commit that names the guest function (`sub_XXXXXXXX`) and the symptom.
- Logs cited by name in PROGRESS.md are kept by `scripts/prune-logs.py`; the rest go after three days.
- Toolkit bumps (submodule) get the full regression run before merge.

## xemu reference baseline, and its hard limit

Set up and working as of 2026-09-19. It boots the disc and reaches the game's own loading screen, so it is a
usable reference for everything up to that point: the EA logo, the intro, and the loading screen itself,
which is the whole range M2 covers. The console files it needs are the owner's and are never stored here.

**It cannot go further, and this is not fixable by us.** xemu freezes at that first loading screen on an
unimplemented NV2A PTimer. That is xemu issue 1124, open since June 2022 with no fix, workaround or pull
request. Cxbx-Reloaded does not get past the loading screen either. Both mature emulators are stopped by
this title.

Consequences for the gates above:
- **M2 can be verified against xemu.** Everything it covers happens before the freeze.
- **M3 cannot.** There is no emulator that can show us this game's title screen, so the golden-image
  criterion needs a different source of truth: real hardware capture, or recorded footage used by eye rather
  than committed as a diff target. Decide which when M3 starts.
- The freeze is xemu's, not something our port has to reproduce. The register involved is
  `PTIMER_INTR_EN_0` at `0xFD009140`, which this title writes exactly once, to disable PTimer interrupts,
  during device creation. Our runtime backs that aperture with plain memory, the write lands, and the port
  is already well past it.
