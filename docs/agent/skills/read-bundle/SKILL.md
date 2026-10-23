---
name: read-bundle
description: Investigate a play-test fault in Def Jam recomp (a hitch, a short visual glitch, an audio pop or gap, a problem at one venue, fighter or Story point) from the play-test recorder's bug bundle or session. Use when the owner reports such a fault, mentions F9, a bundle or logs/sessions, or before opening clip.rgb or audio.pcm.
---

# Read a bug bundle

Full guidance: `docs/agent/playtest.md`; what the recorder writes: `docs/playtest-recorder.md`.

1. `python scripts/bug-bundle.py latest` (or the bundle or session folder). Read `report.md` first:
   where it happened, pacing, hitches with their side, draw-count dips, audio gaps and clicks, flagged
   pictures, and the timed log and events around each.
2. Then the images it names (`frametime.png`, `audio.png`, `around-mark.png`, `anomaly-N.png`). Trust
   its numbers over your reading of a picture. Never open `clip.rgb` or `audio.pcm` directly.
3. No bundle yet: ask for a run with `[debug] record = true` (or `--record`) and F9 just after the fault.
4. Reproduce with `scripts/scenarios.py` (fixture, `generate --seed N --record`, `cutscene`), then the
   usual tools in `docs/agent/diagnostics.md`.
5. Bundles hold the game's pictures and sound: never commit or upload them. Note the path in `PROGRESS.md` §6.
