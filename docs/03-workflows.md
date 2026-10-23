# Workflows (day-to-day)

For first-time setup and normal Release play, use [Build and play](build-and-play.md).
This page covers maintainer workflows. Select your own external data folder with
`$env:DEFJAM_DATA` before running these commands.

## One-time setup
```powershell
git clone --recursive <repo>            # or: git submodule update --init --recursive
pip install pyxbe capstone pytest
# Put extract-xiso.exe at $env:DEFJAM_DATA\tools\extract-xiso\artifacts\  (XboxDev GitHub releases)
.\scripts\extract-disc.ps1 -Image "<your own dump>.iso"     # writes $DEFJAM_DATA\extracted and verifies the hash
New-Item -ItemType Junction -Path .\game -Target (Join-Path $env:DEFJAM_DATA 'extracted')
```

## The pipeline (re-run after a toolkit bump)
```powershell
.\scripts\analyze.ps1          # xbe_parser, disasm, func_id, abi_analysis   (about 2 min, Python only)
.\scripts\recomp.ps1           # lift to src\recomp\gen  (20 C files, about 87 MB)
.\scripts\build.ps1            # cmake --preset win-x64-debug and build       (MSVC)
.\scripts\run.ps1 -WatchdogSecs 10
.\tests\smoke\boot-smoke.ps1   # exit code = boot stage reached
```
From a non-PowerShell shell: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Preset win-x64-debug`.

`run.ps1` supplies the four normal runtime settings (`RECOMP_VBLANK`,
`RECOMP_PB_EXEC`, `RECOMP_PB_D3D11`, `RECOMP_USB`) when absent, matching the
test harness. They enable display timing, push-buffer execution, D3D11 rendering
and controller support. The launcher preserves explicit overrides and removes
settings it added when the game exits. No manual flags are needed for a normal run.

Each analysis command must succeed before the next runs. Local hash manifests bind
analysis to the XBE, unchanged seeds and Python tools, lifting to its analysis,
manual overrides and runtime templates, and each executable to its generated code
and runtime sources. Builds, `run.ps1` and the harness reject stale or missing state.
Analysis and builds compare input hashes before and after the stage. Game builds
request `defjam_recomp` explicitly and reject runtime-only configurations.
After an older build, run the full pipeline once to create these local manifests.
`recomp.ps1` translates into an ignored staging directory and publishes it only on
success; prior output stays in `logs/lift-backups/`. Failed staging output is retained
for diagnosis. `-ToolkitDir` on analyze/recomp/build supports isolated migration checks.
Extra lifter arguments cannot override the pipeline's output or analysis/manual-input paths.

Automated harness runs protect the entire `DEFJAM_DATA/save` tree, including caches
and its original absence. Relative data roots resolve against the game's working
directory; empty overrides are rejected. Failed restores retain their backups;
success metadata stays outside the copied payload. All route and soak
checks reject unresolved calls and non-code ICALL skips, crashes, watchdogs, debug-layer errors, truncated
batches, unexpected process exits and new reached untranslated instructions.
The accepted baseline already omitted CLI, STI, WBINVD and nine specific PUSHFD/POPFD
sites (recorded in `src/recomp_manual.c` from the preserved lift); these are explicitly
reported as `[UNIMPL-KNOWN]` and remain inherited runtime limitations. Their budget
is separate so repeated baseline omissions cannot hide a new `[UNIMPL]` report.
Builds and game runs must be serial: `build.ps1` stops the game before compiling.

Existing save folders created before this rebase may carry the old lifter's
incorrect final hash digit. `src/hooks/save_compat.c` keeps them usable without
renaming or merging files: the game replacement at `sub_001F8360` prefers an
existing canonical folder, then accepts only the exact legacy spelling whose
UTF-16 `SaveMeta.xbx` Name matches. `[SAVE-COMPAT]` identifies that fallback.
It uses the runtime-resolved UDATA host path, including its default and Unicode
paths. Newly created names use the correct upstream CPU semantics (D52).

`scripts/lift-audit.py` is a historical pattern scanner. It excludes functions
with dynamic `_fv` flag validity and refuses `--max` gates on that input; it
cannot validate those control-flow joins. Use compiled recompiler tests for
flag behavior, and retain the separate coverage and jump-table audits.

## The bring-up loop (M1 to M3)
1. Run with the watchdog, then digest the log with `python scripts/triage.py` (newest run) instead of reading `logs\run-*.log.err`, which can be hundreds of megabytes; it prints the verdict and the line numbers to slice (`--around`, `--grep`, `--lines`, `--tail`), see `docs/agent/triage.md`. The VEH handler prints the guest function (`sub_XXXXXXXX`), Xbox registers and a guest stack scan.
2. Locate the function in `tools\xboxrecomp\tools\disasm\output\asm\` or in the Ghidra project under `$DEFJAM_DATA\ghidra`.
3. Decide which of three fixes applies. A missing kernel or D3D shim is implemented in the toolkit and is upstream-able. A lifter bug is overridden in `src\recomp_manual.c` through `recomp_lookup_manual()`. A hardware poke we do not need (TV encoder, PCI space, interrupts) is stubbed in `src\hooks\`.
4. Rebuild. Ninja is incremental, and generated chunks only recompile after a re-lift.
5. Re-run the smoke test and log the stage and the fix in `PROGRESS.md` section 6.

## Symbol naming with Ghidra (optional, recommended)
Ghidra 12.1.3 is at `$DEFJAM_DATA\tools\ghidra\ghidra_12.1.3_PUBLIC\` and JDK 21 is installed. The toolkit's
`tools\ghidra_naming\run_ghidra.sh` is a bash script: run it from Git Bash with `XBE=game_files/default.xbe`
and `GHIDRA_INSTALL_DIR` set, then `py -3 tools\ghidra_naming\merge_names.py --apply` and re-run `recomp.ps1`.
Add the XboxDev `ghidra-xbe` loader for a proper XBE import.

## xemu reference
xemu is installed through winget. Its first launch asks for BIOS, MCPX, EEPROM and an HDD image, which you provide.
Load the XISO from `$DEFJAM_DATA\xiso\`. Use it to capture title-screen goldens and to confirm the native frame rate.

## Git hygiene for the public repo
- Never add anything under `game/`, `src/recomp/gen/`, `logs/` or `build/`. The CI job `hygiene` enforces the extensions.
- Commit messages for bring-up fixes follow `fix(boot): stub sub_0001B9F0 (HalReadWritePCISpace probe)`.
- Generic toolkit changes are validated commits on our xboxrecomp fork; publish the commit before
  updating the parent gitlink. Upstream PRs to sp00nznet/xboxrecomp need Vlad's separate approval.
  Game-specific bits stay here. The old numbered toolkit patches are retired; the
  [ledger](research/toolkit-rebase-ledger.md) maps each one to the fork.

## Runtime diagnostics (environment variables read by the toolkit runtime)
Full table with source references: `docs/research/toolkit-bringup-notes.md`. Everything logs to stderr; `scripts/run.ps1` captures it to `logs/`.

| Variable | Use |
|---|---|
| `RECOMP_WATCHDOG_SECS=10` | dump the guest call stack when the title stops making progress (main.c calls `xbox_WatchdogStart()`) |
| `RECOMP_INPUT_HOST=1`, `RECOMP_INPUT_NO_PADS=1`, `RECOMP_INPUT_IGNORE_FOCUS=1`, `RECOMP_PAD_SCRIPT_KEYS=1`, `RECOMP_RUMBLE_LOG=1` | host input diagnostics for unattended runs (start the input layer in a harness run, open no pad, ignore window focus, turn the scripted pad into keyboard presses, log rumble); see [Controllers, keyboard and mouse](10-input.md#diagnostics-maintainers) |
| `RECOMP_WORKERS=inline` | run PsCreateSystemThreadEx workers inline: tells a threading bug from a logic bug |
| `RECOMP_KERNEL_LOG_BUDGET=N` | cap kernel-call log volume (the first run produced 1.26M calls) |
| `RECOMP_KERNEL_WATCH=0x<guest VA>` | watch kernel calls made from one guest return address (takes a VA, not an export name) |
| `RECOMP_TRACE_ARGS`, `RECOMP_TRACE_DEREF` | dump stack args / one pointer level at traced calls |
| `RECOMP_WATCH_VA=0x...` | hardware watchpoint on a guest address |
| `RECOMP_TRAP_NULL=1` | fault immediately on null guest pointer instead of surfacing later as NaN |
| `RECOMP_NV2A_TRACE`, `RECOMP_PB_EXEC_VERBOSE`, `RECOMP_FB_WINDOW`, `RECOMP_FB_DUMP=prefix`, `RECOMP_TEX_DUMP=prefix` | GPU pushbuffer / framebuffer / texture diagnostics for M2 and M3 |
| `RECOMP_PB_BATCH_DUMP=<flip,...>` or `=shot` | every draw batch of those flips (texture, combiner, stage state); `shot` dumps the flip after each `RECOMP_TRANS_SHOT` capture |
| `RECOMP_PB_PROBE=x,y[;x,y...]` | with a batch dump: every triangle covering those pixels (up to 8), with its depth there and its depth-write, blend and stencil state -- the layers at a point, in draw order |
| `RECOMP_GAMMA=0` | leaves out the gamma ramp the title sets at start-up (an S curve applied at presentation and to captures). The golden frames were taken without it: set this when checking them. |
| `RECOMP_CS_TRACE_CRT`, `RECOMP_CS_WATCH` | critical-section tracing |
| `RECOMP_RECORD=1` (or `[debug] record`, `--record`) | the play-test recorder: `logs/sessions/`, F9 bug bundles, read with `scripts/bug-bundle.py` ([guide](playtest-recorder.md)) |
| `RECOMP_HITCH_PROBE=1` (or milliseconds; with the recorder on, macOS) | when no frame has come for 60 ms, sample which thread holds the guest CPU, every thread's native stack, priority and CPU time, and the push-buffer executor's state, into the frame's `hitch` event and the session's `stalls.jsonl`; read with `scripts/hitch-probes.py` |
| `RECOMP_YIELD_DEPRESS=1`, `RECOMP_EXECUTOR_QOS=0` | macOS: yield with `sched_yield()` (which drops the caller to priority 0) instead of `thread_switch()`, and run the push-buffer executor at the default QoS instead of user-interactive -- the two causes of F1's random stalls (D134), to compare |
| `RECOMP_RECORD_SYNC=1` | the recorder reads its pictures back synchronously (as before the asynchronous read-back), to compare `capture_ms` |
| `RECOMP_FE_INJECT="anchor#N=>command"` | send the front end's FuncCall handler a command after the Nth call containing the anchor ([scenario launching](research/scenario-launching.md)) |
| `RECOMP_TEST_CROWD=hold[:N],reseed` | crowd test controls, with `RECOMP_TEST_OBSERVATIONS=1`; measured by `scripts/crowd-lab.py` |
| `RECOMP_TICK_THREAD=0`, `RECOMP_PRECISE_SLEEP=0`, `RECOMP_GUEST_TURN_PRIO=0`, `RECOMP_GUEST_QOS=0`, `RECOMP_APU_SLICE_PACE=0` | turn off one of the audio crackle fixes (D120-D122), each on by default, to compare; `RECOMP_APU_VOICE_PCM=<prefix>` dumps voices `0xF8`-`0xFA` for `scripts/audio-holes.py` |

Game-side switches (`src/patches/dev_unlock.c`; off unless set; the test harness turns the first on):

| Variable | Effect |
|---|---|
| `RECOMP_UNLOCK_ALL=1` | every fighter and venue on any profile, through the developers' own routine at guest `0x000158F0`; the `[debug] unlock_all` setting sets it at start-up. `0` turns it off, including for the harness. |
| `RECOMP_SKIP_INTRO=1` | start at Press Start, bypassing startup legal and EA/THX/intro movies only. Overrides `[debug] skip_intro`; `0` restores normal boot. Harness/scenario runs default to skip enabled and use the preStart request as their anchor; m3 keeps one complete normal startup in each regression set. `regress.py --only skipintro` checks the option and both override directions. |
| `RECOMP_DEV_DEBUG=1` | the in-fight debug label under each health bar (holds the byte at `0x0039C1DA`) |
| `RECOMP_START_FIGHTERS=a,b[,c,d]`, `RECOMP_START_VENUE=n`, `RECOMP_START_LOG=1` | replace the fighters and venue when the match is made; see [the testing harness](09-testing-harness.md#choosing-the-fighters-and-the-venue) |
| `DEFJAM_HARNESS_SNAPSHOT_SAVE=<dir>` | harness only: keep the save area as the run left it, before the guard restores the original |

Compile-time: `-DRECOMP_ICALL_FEEDBACK` plus `python -m tools.recomp.icall_feedback` closes the loop on indirect-call targets that static analysis cannot see; `python -m tools.seed_from_log <run.log>` seeds functions from `Failed to resolve VA` lines.

## Replacing a badly lifted function by hand
1. Write it in `src/recomp_manual.c` as `void sub_XXXXXXXX(void)`. Read arguments off the guest stack (`MEM32(g_esp + 4)` is the first cdecl argument, since `g_esp` points at the return address on entry), put the result in `g_eax`, and consume the return address with `g_esp += 4` for a plain `ret`, or `g_esp += 4 + N` for `ret N`.
2. Nothing else to declare. `scripts/recomp.ps1` passes the file to the lifter with `--exclude-manual`, which scans it for `void sub_XXXXXXXX(void)` definitions and stops generating those bodies, so yours links for direct callers too.
3. Re-run `scripts/recomp.ps1`, then rebuild. The lifter reports how many it excluded and how many it wrapped.

To **wrap indirect calls**, add `extern void sub_XXXXXXXX_gen(void);` beside your definition and call it from the middle. The real body is emitted under that name, and direct call sites are also rewritten to call `_gen`, bypassing the wrapper. To observe every caller at entry, define `void sub_XXXXXXXX_enter(void)` instead and re-lift; its hook precedes guest instructions while retaining the generated body. Retired overrides can stay inside `#if 0`, which the scanner skips.

Find candidates by comparing each indexed jump's table arms against its function's detected bounds: arms past the end mean the body was truncated and the lifter could not prove the jump was local, so it emitted an indirect call that fails at runtime.
