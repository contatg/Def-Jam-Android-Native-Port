# Delegating to sub-agents

Moved here from `AGENTS.md` §5/5a and reworded so it applies to any agent. Vlad asked for this
explicitly. The deciding question is whether the task is **serial or independent**, not whether it
is hard. Use whatever sub-agent facility your tool has. Where it lets you choose, pick a smaller, cheaper
model than the one you work on (delegation is not cheaper by itself: some tools fork the whole history
and the parent's model by default, and every sub-agent re-reads what it is given), run it in the
background, and keep working meanwhile.

**Delegate** when all of these hold:
- It is **read-only**: reading lifted C, the image, logs or docs. No edits, no builds, no runs.
- It is **bounded**: a named starting address or file and a question with an end, not "investigate the front end".
- It does **not need the build loop**: nothing it concludes depends on a measurement it would have to take itself.
- Two or more such tasks can run **at once**, or one can run while you keep measuring.
- Its output is a **written finding**: `docs/research/<topic>.md` plus a short summary that you can act on.

Reverse-engineering reads are the archetype: "read `sub_00284476` and its callees; report what it
checks and under what conditions it would judge the transfer a failure."

**Do not delegate** the measure-change-rebuild loop. It is serial by nature, since each step needs the
previous result, and a cold agent re-deriving context costs more than the step is worth. The same goes
for anything that must hold state across many runs, and for interpreting a contradictory measurement,
which is where wrong-but-plausible conclusions get made.

**Write the brief as if the reader has never seen this repo.** Some tools start a sub-agent with no
history, some fork all of it; a self-contained brief works either way. Include: the repo
path, one line on what the project is, the exact question, the addresses to start from, **what has
already been ruled out** (so it is not redone), the lifted-C conventions it needs, where to put the
report, and an explicit "do not edit code, do not build, do not run the game".

Two ready briefs are in `docs/agent/briefs/`: `re-reader` (read lifted C and the image, report) and
`log-triage` (digest a run log with `scripts/triage.py` and report in ten lines). Tools that support
named agents get generated copies of them (`scripts/sync-agent-adapters.py`); any other agent can paste
a brief into its delegation prompt and fill in the question. "Read-only" in a brief is an instruction;
it is enforced only where the tool restricts the agent's tools or sandbox (the generated Claude agents
list their tools; check what yours allows). Codex documents custom agents as `.codex/agents/*.toml`;
none are generated here yet.

Treat a sub-agent's report as evidence, not as fact. Check any claim that would change what gets built.
