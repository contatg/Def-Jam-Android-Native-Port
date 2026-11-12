# src/patches — gameplay patches (opt-in)

Reserved for behavioral changes to the game itself, such as a widescreen view.
Each goes behind a runtime setting so the unmodified path stays testable. CMake
picks up every `.c` file here. Fixes that make the game run belong in `src/hooks`
or `src/recomp_manual.c`.

- `dev_unlock.c`: `RECOMP_UNLOCK_ALL=1` (every fighter and venue, through the developers'
  own routine at `0x000158F0`) and `RECOMP_DEV_DEBUG=1` (the in-fight debug overlay). Both off
  by default; [the investigation](../../docs/research/unlock-everything.md).
- `skip_intro.c`: `[debug] skip_intro` / `RECOMP_SKIP_INTRO=1` selects Press Start on the
  first screen load, retaining native controller initialization. Off by default; later
  Story and attract movies are unchanged. [The investigation](../../docs/research/startup-skip-flow.md).
- `picture.c`: `[display] brightness` (a curve on top of the title's gamma ramp), `vignette` and
  `vignette_strength` (the venue post effect's darkened edges: removed, or blended towards the
  undarkened picture). Defaults leave the picture exactly as before; all three apply as they change.
  [The investigation](../../docs/research/picture-options.md).
