# Reading a run: `scripts/triage.py`, then the original

A run log (`logs/run-<stamp>.log.err`) can be hundreds of megabytes, nearly all of it one line a
frame. `scripts/triage.py` reads it once and prints one screen. The digest is the **first rung**: it
is for the routine case, and it hands you the exact way into the original when it cannot explain a run.

```
python scripts/triage.py                          newest run
python scripts/triage.py logs/run-20261014-101500 a path, a stem, or either half (.log / .log.err)
python scripts/triage.py LOG --json               the same, for a script
```

## What the verdict means, and what to do

| Verdict | Meaning | You |
|---|---|---|
| `clean` | no fault signature, nothing unusual, **and** positive evidence of presentation (frame reports with presents above zero, the last one not zero) | move on |
| `explained` | one known stop signature **with** its function or address, and nothing unclassified before it | act on it: `callers.py`, `guest-stack.py`, the fix |
| `partial` | a known signature without enough detail; or unclassified lines come first (the stop may be a consequence); or a hang (the watchdog's stack is a scan, not a walk); or only symptoms (non-code calls, truncated batches, debug-layer messages) | **read the original**, starting at the line numbers the digest printed |
| `unexplained` | the run looks wrong and no known signature says why; or nothing shows a frame was presented (no frame reports, or all of them 0) | **read the original** |

Also read the original, and do not re-run the digest, when a fix based on an `explained` verdict does
not change the outcome. A digest is a summary of lines it recognised; the cause may be in the ones it did not.

## Reading the original, bounded

Every number the digest prints is a line number of the `.log.err`. Slice it with the same tool:

```
python scripts/triage.py LOG --around 4146 --context 30     numbered lines around a line
python scripts/triage.py LOG --lines 4100:4180               a range
python scripts/triage.py LOG --grep "ICALL|sub_0021" --max 40    matches, with the total (--last for the final ones)
python scripts/triage.py LOG --head 50    |    --tail 80
```

A slice is capped (80 lines by default, 400 at most), says how many lines it cut and how to continue,
and cuts a very long line with the count of what was cut. Do not `cat`, `type` or `Get-Content` a run
log whole, and do not open it in an editor-style read without a limit: that is the thing the digest
exists to avoid. Slice, and slice again. You lose nothing by it: the log is never modified.

## Where the digest is deliberately weak

- **Hangs.** A `[WATCHDOG]` is always `partial`. Its guest stack is a scan of stale words. Locate the
  live thread with `scripts/native-stacks.py` (every host; lldb on macOS, gdb on Linux) and
  `scripts/sample-threads.py` (Windows), then `scripts/guest-stack.py` (Windows). A log is weak
  evidence for a hang: a loop entered once at start-up never shows again (`docs/agent/diagnostics.md`, rule 1).
- **Order.** Unclassified lines *before* the stop keep a crash, an unresolved call or an untranslated
  instruction from being `explained`, for that reason: the last error in a log is often a consequence of
  an earlier one. A line with a fault word that no parser recognised (an unfamiliar `[ICALL]` message)
  is kept as unclassified, never dropped.
- **"Frames stopped".** The log has no clock. A hang without a watchdog is inferred from more than 400
  lines after the last frame report, so a sparse hang can be missed and a verbose normal shutdown can
  look like one. Treat it as a pointer to `--tail`, not as a measurement.
- **Two faults.** A `[CRASH]` followed by a `[WATCHDOG]` (or a second crash) is read as two events; a
  watchdog is always `partial`.
- **Counts.** Repeats of one message are collapsed to its first line and a count (numbers are
  normalised). A count is a total, not a rate.
- **Unknown noise.** A line the project knows to be harmless can be added to `config/triage-ignore.txt`
  (one regular expression per line). Keep it narrow; an over-broad pattern hides real faults.

## Keeping it honest

`tests/unit/test_triage.py` pins the digest on synthetic logs shaped like the runtime's output
(`src/main.c` for `[CRASH]`, the lifter's `[ICALL]` line). When the runtime changes a log line the
digest relies on, change the test and `scripts/triage.py` together. When a real run teaches the
digest a new signature, add the signature, a synthetic test of it, and say so in `PROGRESS.md` §6.
