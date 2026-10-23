# Agent playbooks

Longer how-to material for the people and agents working on this repository. `AGENTS.md` (the
instructions every agent reads) keeps the rules and the working loop, and points here by task, so a
session loads only what it needs.

| File | Read it when |
|---|---|
| [launcher-handoff.md](launcher-handoff.md) | you are continuing the launcher redesign in PR #58 |
| `triage.md` | a run, a regression row or a smoke test failed or looks wrong |
| `diagnostics.md` | the digest does not explain a stop; the table of watch/trace tools and the three rules |
| `playtest.md` | the owner reports a fault seen in play, or hands you a bug bundle |
| `harness.md` | you are scripting a run, a route or a scenario |
| `lifter-overrides.md` | you are about to write or change something in `src/recomp_manual.c` |
| `delegation.md` | you are considering a sub-agent |
| `environment.md` | a build or a shell behaves oddly |

`skills/<name>/SKILL.md` and `briefs/<name>.md` are the single sources for the per-tool copies
(`.claude/skills`, `.codex/skills`, `.claude/agents`). Edit these, then run
`python scripts/sync-agent-adapters.py`; `tests/unit/test_agent_instructions.py` fails if a copy differs.
Frontmatter in a skill is `name` and `description` only, which is what both tools read.
