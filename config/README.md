# config/

Project configuration. None of these files contain game data; they describe it.

| File | Purpose |
|---|---|
| `dump-manifest.json` | Hashes and expected contents of the user-supplied dump. `scripts/verify-dump.py` checks a dump against it. |
| `seed_functions.json` | Function entry points that static analysis cannot see (indirect-call targets, thread start routines), discovered at runtime and fed back by `scripts/seed-from-log.ps1`. Passed to the disassembler by `scripts/analyze.ps1`. |
There is deliberately no list of hand-written functions here. `src/recomp_manual.c` is the single source of truth: `scripts/recomp.ps1` passes it to the lifter with `--exclude-manual`, which reads the definitions out of the file itself so the two cannot drift.

## Current manual overrides

- **`0x002016B0` — CRT `memmove`/`memcpy`.** MSVC dispatches this routine's alignment and tail cases through jump tables embedded in its body. Function detection ends the body at `0x00201952`, but those arms live at `0x0020194C`–`0x002019D4`, past that end, so the lifter emitted indirect calls instead of local jumps and the tail bytes of every affected copy were dropped. Reimplemented natively; also a large speed-up.
