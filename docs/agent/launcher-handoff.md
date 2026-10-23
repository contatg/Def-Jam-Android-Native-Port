# Launcher handover — PR #58

Start with `AGENTS.md`, then `python3 scripts/progress.py brief`. This is a continuation note;
repository policy and current project state remain in those files.

## Stable release and entry point

[PR #58](https://github.com/vyanhursky/DefJamFFNY-recomp/pull/58) merged at `2e8ec40`,
the exact **v0.10.6** tag. [Improved Game Launcher — stable/latest](https://github.com/vyanhursky/DefJamFFNY-recomp/releases/tag/v0.10.6);
The owner authorized stable promotion on October 20 (D145); tag/assets are unchanged.
He accepted Mac appearance and confirmed the physical controller
works, then requested beta deployment for Deck and fresh README/docs launcher screenshots (D144).
Candidate CI passed 28/28; tagged release 39452474210 passed 30/30. All nine game-free setup/checksum/
provenance assets were downloaded and verified against GitHub digests and source/toolkit identity.
The source branch was cleaned after merging. Keep the local worktree/app for feedback.

Local worktree: `/Users/vlad/.codex/worktrees/launcher-redesign/DefJamFFNY-recomp`.
`build/Def Jam Launcher Playtest.app` forces Home and uses the owner's normal data by default.
Keep this local app/worktree available; built game executables must not be published.

## Code map

Paths below are under `src/hooks/`:

| File | Responsibility |
|---|---|
| `pc_launcher_home.*` | Home, common settings, controller summary, utility navigation |
| `pc_ui.cpp` | Shared Advanced/overlay widgets; Display, Controls, Mods, Interface, Diagnostics, Maintenance |
| `pc_ui_theme.*` | Palette, fonts, procedural brickwork/paper, responsive geometry |
| `pc_ui_model.h` | Pack inventory/order and live/startup timing helpers |
| `pc_ui_actions.*` | Native paths, clipboard, Setup selection and child launch |
| `pc_launcher.cpp`, `pc_launcher_sdl.cpp` | Windows and SDL/Metal launcher hosts |

Installed context/prefill comes from `setup/main.cpp`, the macOS Swift wrappers and Linux launcher/wizard.
The [implementation report](../research/launcher-ui-implementation-2026-10-19.md) has details;
the [player guide](../launcher-and-overlay.md) explains the settings layout.

## Preserve these behaviors

- Home and Advanced/overlay share settings/defaults. Environment overrides show effective, locked values;
  labels distinguish live changes from next-launch changes. Existing binding capture remains shared.
- Later selected packs win; missing selections stay removable. The existing list payload limit is
  95 bytes. Inventory checks manifest headers; the renderer validates images.
- Setup opens its existing wizard after launcher teardown, without inheriting `play.lock` or starting
  an install automatically. Long chosen paths live in `<data>/launcher-setup-path.txt`.
- New/reset defaults are Windowed, 1920×1080, Widescreen (Auto), 2×; saved choices survive upgrades.
  Auto follows the monitor. Render scale is internal height (2× = 960p, 3× = 1440p), separate from window size.
- Retain brighter slider tracks, Back/focus restoration and narrow/200% layouts. Theme assets are procedural/system fonts.

## Evidence and next work

Units: **527 passed / 32 skipped**. Native macOS UI: **11/11**, plus constrained-tooltip capture.
Final quick **5/5**, UI **4/4**, combat retry **27/27**; source CI **28/28** at `205582d`.
The first full run remains **16/18**: one combat audio underrun beside a host stall, plus replay's
historical stream target `0x001E3C14` (D108), despite exact golden records. Do not report a clean full run.
Receipts/captures are local in `logs/launcher-review/`; the report names regression logs.

2026-10-20 follow-up: fresh/saved settings and Auto boot pass; compact controls and complete help
are verified at 100/200/250%, including 640×480. Latest quick 5/5 (`logs/regress-20261020-105429.txt`),
picture/overlay 2/2 (`logs/regress-20261020-105615.txt`); quick units 528 pass/32 skip/one slow deselected.
The report records the initial 250% failure and its fix (D143).
Beta version/media preparation: native 0.10.6 rebuild and Home/Advanced captures pass; save root
restored; units 529 passed/32 skipped/one slow deselected, 62 local docs links valid, hygiene 432/0.

1. Mac appearance and physical controller are owner-accepted. Collect Deck feedback, text scaling, actual
   HD packs/priority, folder/log actions and real Setup selection/prefill. Windows/Deck GUI acceptance
   remains pending; CI compilation does not cover it.
2. After changes, use the focused model/action fixtures and real UI targets
   (`regress.py --only launcher,overlay,unlockopt,picture`), then the applicable AGENTS checks.
   Run games serially with `DEFJAM_DATA` pointing to the disposable
   `logs/launcher-review/runtime-data`; leave the owner's save alone.
3. Keep PROGRESS current. v0.10.6 is stable/latest as **Improved Game Launcher** (D145).
   Use the Linux setup, existing install/data folders, Clean Update + Bundled or Latest.
   Collect any further Deck/Windows reports; do not infer detailed playtest results from release approval.
   Publish only reviewed game-free setups through the tagged workflow.
