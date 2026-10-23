---
name: triage-run
description: Diagnose a failed or suspicious Def Jam recomp run from its log. Use when a harness route, regress.py row, smoke test or manual run failed, crashed, hung or looks wrong, or before reading any logs/run-*.log.err.
---

# Triage a run

Never read a run log whole: it is hundreds of megabytes. Full guidance: `docs/agent/triage.md`.

1. `python scripts/triage.py` (newest run) or `python scripts/triage.py <log>`. One screen: verdict,
   stop signature, function or address, frames, unclassified lines, and the next commands.
2. `clean` or `explained`: act on it (`scripts/callers.py`, `scripts/guest-stack.py`, then the fix).
3. `partial`, `unexplained`, or a fix that did not change the outcome: go to the original, bounded, with
   the line numbers the digest printed:
   `triage.py <log> --around LINE --context 30`, `--lines A:B`, `--grep REGEX --max 40` (`--last`), `--head N`, `--tail N`.
4. Hangs are always `partial`: find the live thread (`scripts/native-stacks.py` on every host,
   `scripts/sample-threads.py` on Windows), then `scripts/guest-stack.py` (Windows). The watchdog's own stack is a scan.
5. Record the finding in `PROGRESS.md` §6 with the log name.
