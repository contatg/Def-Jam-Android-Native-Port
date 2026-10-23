# Diagnostics, and how to read them

Moved here from the agent instructions (`AGENTS.md` §4a) unchanged except for the `triage.py` row.
Read it when a run stops and the digest does not explain why.

These were all built while chasing real bugs in this title; each exists because a question had no other
answer. Reach for them before writing a new one.

| Tool | Answers |
|---|---|
| `scripts/triage.py` | A one-screen digest of a run log: the stop signature, the function or address, frames, unclassified lines, a verdict, and the exact commands to read deeper. Start here; see `docs/agent/triage.md`. |
| `RECOMP_WATCH_EXEC=a,b,c,d` | Was this guest function entered, and how often? Up to four addresses across DR0-DR3, resolved through `recomp_lookup`, so it sees direct and indirect calls alike. |
| `RECOMP_WATCH_EXEC_STR=1` | The string at `ecx` and at the first argument. Half the interesting functions take a name. |
| `RECOMP_WATCH_EXEC_FIELD=0x20` | `MEM32(ecx + 0x20)` at the call. The ordinary shape of a member read. |
| `RECOMP_WATCH_EXEC_CHAIN=0,8` | Follows a pointer chain from `ecx` **at the instant of the call**. |
| `RECOMP_APU_VOICE_PCM=<prefix>` | What voices `0xF8`-`0xFA` (the title's six-channel mix buffer) produced, before volume, in `<prefix>F8.pcm` etc. `python scripts/audio-holes.py <prefix>` counts the places where the buffer replayed the previous lap, the audible click. `RECOMP_APU_SLICE_PACE=1/0` turns per-slice APU pacing on or off (on by default on Linux; D120). |
| `RECOMP_WATCH_WRITE` / `_LEN` / `_RW` | Who wrote this address. Prints the guest registers and a stack scan. |
| `RECOMP_GUARD_CODE=1` | What is writing over the image. Noisy by construction -- XBE sections mix code and data -- and the reports that matter are the ones on jump tables. |
| `RECOMP_PEEK` (toolkit) | The current value of a few guest addresses. Covers both memory windows. |
| `[debug] record` / `RECOMP_RECORD=1` / `--record` | The play-test recorder: per-frame timing, events, audio and the last seconds of pictures; F9 (or a crash, hang or long hitch) writes a bug bundle. `scripts/bug-bundle.py` reads one (`docs/agent/playtest.md`). The cheapest way to locate a short-lived fault in time. |
| `RECOMP_FE_INJECT="anchor#N=>cmd"` | Sends the front end's own FuncCall handler a command after the Nth call containing the anchor: start a mode or a scene without the menus. `scripts/funccall-catalog.py` lists the commands the screens contain. |
| `RECOMP_TEST_CROWD=hold,reseed` | Test controls for the two crowds (with the test probes on); `scripts/crowd-lab.py` measures what they do. |
| `RECOMP_POST_CMD` | Feeds the front end a synthetic command, to separate "the machinery is broken" from "nothing triggers it". |
| `scripts/string-va.py` | A byte string's guest address. **The most useful of these.** The lifted C has no string literals -- a format string is a bare `PUSH32(esp, 0x29C0DC)` -- so searching for `.big` finds nothing while searching for the address finds every user. |
| `scripts/callers.py` | Direct callers, N levels up. Says "reached only indirectly" as an answer. |
| `scripts/gap-seeds.py` | Functions the lifter never reached (vtable slots, driver tables): code-shaped starts in the gaps between lifted functions. `--write` seeds them. Check `Original:` extents before and after the re-lift. |
| `scripts/stub-targets.py` | Which unresolved stubs are real code a lifted function jumps into, and which can be seeded. A stub reached at run time skips code and leaves the stack wrong; the failure shows up callers away (the Terrordome crash, v0.2.1). `[ABI] ... esp(epilogue never ran)` from the `win-x64-abicheck` preset names the function. |
| `scripts/mem-find.py` | Searches a live process's guest memory for a byte pattern. |
| `scripts/native-stacks.py` | Every thread's **host** call stack, named from the PDB (dbghelp, no debugger; lldb on macOS, gdb or eu-stack on Linux). The answer when a thread is "outside the exe": which lock or wait, and which of our functions got it there. |
| `scripts/big-entry.py`, `scripts/refpack.py` | Read the dump offline: list a BIG4 directory, decompress RefPack and list the nested archive. Metadata only -- they never write game bytes anywhere. |

**Three rules, each learned by getting it wrong.**

1. **A hit is evidence; a miss is not.** An entry breakpoint fires on the edge, so a long-running loop
   entered once during start-up is invisible to it for ever -- that produced a confident "the front-end
   loop never runs" about a loop that was running the whole time. A data watchpoint has missed a write this
   runtime provably made. Confirm a miss another way: `scripts/guest-stack.py` reads stacks as they are.
2. **Read totals, not printed lines.** Every per-event log here is budgeted. `RECOMP_WATCH_EXEC` prints a
   running total every five seconds -- use that. Reading a capped counter as a measurement has produced two
   retractions: a DPC that looked queued once and was queued ten times, and a tick that looked like exactly
   8 and was 9, 17 or 41 depending on the run.
3. **Ask which address space a number is in.** `symbolize.py` reports a **host** offset into compiled code;
   it is not a guest offset and must never be pasted into a brief as one. A physical address from a
   hardware model is not a guest address -- `phys_to_guest` exists for that, and getting it wrong cost a day
   on USB. Run-to-run variance is also large here: frame counts and call counts differ between identical
   runs, so one run is never a measurement.
