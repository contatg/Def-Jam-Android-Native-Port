# Golden baselines

These files describe what the renderer produced. They never contain it.

The frames this port renders are Def Jam: Fight for NY's own artwork, and this repository is public, so a
baseline here stores only measurements: the bounding box of the drawn region, how many pixels and distinct
colours it has, and a hash. That is enough to catch a regression from "renders the screen" to "renders
nothing", or to something structurally different, which is what the L4 row of `docs/02-test-plan.md` asks
for. Actual captures land in `logs/`, which is gitignored.

Produce a capture by running the game with the host's back-buffer grab enabled:

```
$env:RECOMP_PB_D3D11=1; $env:RECOMP_GAMMA=0; $env:RECOMP_TRANS_SHOT="$PWD\logs\frame.bmp"; $env:RECOMP_TRANS_SHOT_FRAME=150
.\scripts\run.ps1 -TimeoutSecs 60
python scripts\frame-signature.py logs\frame.bmp --check tests\golden\m2-loading-screen.json
```

`RECOMP_GAMMA=0` since 2026-10-02: captures now carry the title's gamma ramp, and the goldens were taken
before it was applied.

The M2 signature was rewritten on 2026-10-02 (patch 0106): the logo is drawn with the linear filter the title
asks for, where it had been point-sampled.

Frame 150 since 2026-09-29: start-up got faster (the PTIMER and vsync fixes, 0071 and 0074), and frame 900
now lands in the intro movie. Frames 150-160 hold the loading screen; the golden itself is unchanged.

`RECOMP_TRANS_SHOT_FRAME` matters: the first frames are a clear and nothing else, because the title is still
loading, so a capture of frame zero is black no matter how well the renderer works. It takes a list
(`30,3600`), one file per frame with the number before the extension.

For screens reached with `RECOMP_PAD_SCRIPT`, aim by time instead: `RECOMP_TRANS_SHOT_SECS=200,230` takes
the first frame after each point in seconds since process start, the clock the pad script uses, and names the
files `<stem>-200s.bmp`. Frame counts drift by hundreds between runs, so a frame number cannot be aimed at
the screen a scripted press leads to.

Even seconds since process start drift by a minute (the title appears anywhere from 100 to 190 s), so
anchor the clock to the title instead: `RECOMP_SCRIPT_ANCHOR=getscreeninfo(intmain/prestart` starts both
the pad script's and the capture's clock at the Press Start screen request. This works with normal
startup and with `[debug] skip_intro`; the old `feflow.xml#2` counter needs the legal/movie path.
The main-menu capture:

```
$env:RECOMP_SCRIPT_ANCHOR="getscreeninfo(intmain/prestart"; $env:RECOMP_TRANS_SHOT="$PWD\logs\menu.bmp"; $env:RECOMP_TRANS_SHOT_SECS=50
$env:RECOMP_PAD_SCRIPT="start:14:14.3,start:17:17.3,a:20:20.3,a:23:23.3,a:26:26.3,a:29:29.3,a:32:32.3,a:35:35.3,a:38:38.3,b:42:42.3,b:46:46.3"
.\scripts\run.ps1 -TimeoutSecs 280
python scripts\frame-signature.py logs\menu-50s.bmp --check tests\golden\m4a-main-menu.json
```

START twice (the title ignores the first press now and then), A until both memory-card popups have gone
(they ignore input for a varying 5–15 s), then B twice: the As that land after the popups choose Story, which
opens the user-ID screen and sometimes its keyboard, and each B backs out one level to the main menu.

| Baseline | What it is |
|---|---|
| `m2-loading-screen.json` | The game's first loading screen — crest logo and LOADING wordmark, centred, on black. The first screen this port has ever rendered, and the same screen xemu reaches before it freezes. |
| `m3-title-screen.json` | The title screen: skyline, logo, "Press START", copyright. Capture it 5 s after the screen-request anchor (`RECOMP_TRANS_SHOT_SECS=5`); frame 3,600 used to land on it but now and then lands on a black transition instead. |
| `m4a-main-menu.json` | The main menu, idle, captured as above: "MAIN MENU" header, the eight mode tiles, the Story panel. Loose by design (region and drawn-pixel count within 5%): it also accepts the menu with another tile selected and the match-type screen, and rejects the title. |

## Checking all three

`python scripts/regress.py --only m2,m3,m4a` runs the game three times and checks each signature. The
loading screen (M2) has no front-end call to anchor a capture on, and which frame it is on depends on how
fast the build starts, so that check captures several frames and passes if one of them is the screen.
M3 explicitly retains the complete legal/EA/THX/intro sequence and asserts its order; the other runs
skip startup. Both quick and full regression therefore check the normal startup once per set.

## fight-stream.json

The SHA-256 of the game-state stream of the first 1,800 simulation steps of a One on One whose
input is step-timed and whose random seeds are pinned (`scripts/regress.py --only replay`). It was
identical on the Release and Debug builds and on two save fixtures. It is a hash of derived state;
no game data is stored. Update it with `--only repeat --update-golden` when a change is meant to
alter how the fight plays, and say so in the commit.
