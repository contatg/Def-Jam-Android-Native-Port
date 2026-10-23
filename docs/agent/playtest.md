# Play-test reports and bug bundles

Read this when the owner reports a fault seen in play (a hitch, a glitch for a second, an audio pop, a
problem at one venue, fighter or Story point), or hands you a bundle. Human guide:
`docs/playtest-recorder.md`.

1. **Get a bundle.** Ask for a run with `[debug] record = true` (or `--record`) and F9 pressed just after
   the fault; a crash or hang writes one by itself. Bundles are under `logs/sessions/<s>/bundles/<b>/`.
   They hold the game's pictures and sound: never commit, upload or paste them anywhere public.
2. **Read the analysis, not the raw files.** `python scripts/bug-bundle.py latest` (or the folder) writes
   `report.md`; read that first. It places every log line, audio sample and picture on the frames'
   clock, and lists hitches (with the side: `present`, `game`, `recorder`), one-frame draw-count dips,
   audio gaps, clicks and clipping, and flagged pictures, with the log and events next to each.
3. **Then look at the pictures it names**: `frametime.png`, `audio.png`, `around-mark.png`,
   `anomaly-N.png`. Trust the numbers over your own reading of a picture; vision models miss glitches
   (GlitchBench), and the analysis says what it did not check.
4. **Whole session, no mark**: `python scripts/bug-bundle.py logs/sessions/<s>` lists every hitch and
   drop-out of the run with the screen it happened on.
5. **Reproduce** before fixing: a fixture at that Story point (`scripts/scenarios.py fixture list`), a
   generated route for the venue and fighters (`scenarios.py generate ... --seed N --record`), or a
   cutscene (`scenarios.py cutscene`). `docs/research/scenario-launching.md` says what is calibrated.
   A repeat with `--record-hitch-ms` writes a bundle for each long frame by itself.
6. **Go deeper** with the usual tools (`docs/agent/diagnostics.md`): a `game`-side hitch that does not
   repeat with `RECOMP_HITCH_PROBE=1` on the repeat (macOS; `scene-perf.py run ... --env
   RECOMP_HITCH_PROBE=1`), then `python scripts/hitch-probes.py <session> --summary`: the threads'
   stacks, priorities and CPU time while the frame was late, so an event once a minute is caught in the
   act; a repeatable one with `scripts/sample-threads.py` during the repeat or `RECOMP_TRACE_PROFILE=1`; a picture fault with
   `RECOMP_PB_BATCH_DUMP=shot`; an audio fault with `RECOMP_APU_LEVEL=1`.
7. Record the bundle's path and what it showed in `PROGRESS.md` §6.

Costs to keep in mind: each kept picture is a synchronous GPU read-back (`capture_ms` in every row; the
analysis subtracts it). For pacing measurements set `[debug] record_fps = 0`.
