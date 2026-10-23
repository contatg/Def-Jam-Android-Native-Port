#!/usr/bin/env python3
"""Drive the port unattended: scripted pad input, captures, and a summary of the run.

    python scripts/harness.py routes
    python scripts/harness.py run fight --secs 200
    python scripts/harness.py run intro --shots 16.5,46,58 --env RECOMP_PB_BATCH_DUMP=shot
    python scripts/harness.py run gym --stage "getscreeninfo(story/gym=down@4;down@6;a@9;x@16" \
                                      --shots "@getscreeninfo(story/gym,20,23"
    python scripts/harness.py soak --boots 20
    python scripts/harness.py png logs/shots/fight/fight-60s.bmp
    python scripts/harness.py montage out.png 3 2 a.bmp b.bmp c.bmp

How a run is scripted. The front end logs every call it makes into the game as a
`[FUNCCALL]` line. A *stage* is `<anchor>=<presses>`: the presses start when a
`[FUNCCALL]` line containing the anchor (case-insensitive) appears, and end when the
next stage's anchor does. Presses are `button@seconds`, separated by `;`, and
`a@8+3x6` is six presses three seconds apart. Captures (`--shots`) are seconds on
the clock of the last `@anchor` token before them, e.g. `@game.startgame(,60,120`.

Everything a run produces stays under `logs/` (ignored by git): the log pair
`logs/run-<stamp>.log(.err)` and the captures `logs/shots/<name>/`. Captured frames
are the game's artwork and must never be committed.

Every run can write saves or caches. The complete `save` root is copied aside
first and restored afterwards, including its original absence. A failed restore
retains the backup and fails the run.

Standard library only, so it runs wherever the repo does.
"""
import argparse
from datetime import datetime, timezone
import filecmp
import importlib.util
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import tempfile
import zlib
from pathlib import Path

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Windows builds with the win-x64 presets; every other host with the posix ones.
WINDOWS = os.name == "nt"
EXE_NAME = "defjam_recomp.exe" if WINDOWS else "defjam_recomp"
RELEASE_PRESET = "win-x64-release" if WINDOWS else "posix-release"
DEBUG_PRESET = "win-x64-debug" if WINDOWS else "posix-debug"
_state_spec = importlib.util.spec_from_file_location(
    'pipeline_state', os.path.join(REPO, 'scripts', 'pipeline-state.py'))
pipeline_state = importlib.util.module_from_spec(_state_spec)
_state_spec.loader.exec_module(pipeline_state)

# What every run needs: without these the title boots into a black window and a
# smoke test reports the wrong stage.
BASE_ENV = {
    "RECOMP_VBLANK": "1",
    "RECOMP_PB_EXEC": "1",
    "RECOMP_PB_D3D11": "1",
    "RECOMP_USB": "1",
    # A player's settings.ini must never change a test run: defaults only, nothing written.
    "RECOMP_SETTINGS": "none",
    # Reviewed goldens/replays use the original canvas, independent of player defaults.
    "RECOMP_ASPECT": "4:3",
}

# A screen request works with both normal and shortened startup. Counting flow
# file opens does not: skipping legal/bootLoad also skips the first flow load.
TITLE_ANCHOR = "getscreeninfo(intmain/prestart"


def startup_default(e):
    """Shorten test boots; explicit 0 keeps normal startup, empty uses the INI/default."""
    if "RECOMP_SKIP_INTRO" not in e:
        e["RECOMP_SKIP_INTRO"] = "1"
    elif e["RECOMP_SKIP_INTRO"] == "":
        del e["RECOMP_SKIP_INTRO"]
    return e


def unlock_default(e):
    """Test runs have every fighter and venue, whatever the save holds (src/patches/dev_unlock.c).

    A caller, or the shell, can say otherwise: RECOMP_UNLOCK_ALL=0 turns it off, and an empty value
    leaves it unset, so the settings file's own [debug] unlock_all applies."""
    if "RECOMP_UNLOCK_ALL" not in e:
        e["RECOMP_UNLOCK_ALL"] = "1"
    elif e["RECOMP_UNLOCK_ALL"] == "":
        del e["RECOMP_UNLOCK_ALL"]
    return e


def data_dir():
    """The game data root (dump, analysis, save area), outside the repo."""
    return os.environ.get("DEFJAM_DATA") or os.path.join(os.path.dirname(REPO), "defjam")


def guarded_data_root(value):
    """Match the child's REPO working directory and refuse its unguarded default."""
    if not value or not value.strip():
        raise ValueError('DEFJAM_DATA must name a nonempty save root for a guarded run')
    return os.path.abspath(value if os.path.isabs(value) else os.path.join(REPO, value))


def exe_path(preset):
    return os.path.join(REPO, "build", preset, EXE_NAME)


def validate_shots(spec):
    """The native writer names files by seconds only; prevent overwrites."""
    labels = set()
    for token in spec.split(',') if spec else []:
        if token.startswith('@'):
            continue
        seconds = float(token)
        if not math.isfinite(seconds) or seconds < 0:
            raise ValueError('invalid screenshot time')
        label = format(seconds, 'g')
        if label in labels:
            raise ValueError('duplicate screenshot time label would overwrite a checkpoint: ' + label)
        labels.add(label)
    if len(labels) > 16:
        raise ValueError('native screenshot limit is 16 checkpoints per launch')


# ---------------------------------------------------------------------------
# Pad scripts
# ---------------------------------------------------------------------------

def title_presses():
    """START past the title, then A every three seconds until a stage takes over.

    On the title anchor's clock. The A presses carry the run through the legal
    screens and the memory-card notice, and stop mattering once the first stage's
    anchor arrives (a later anchor ends the entries before it)."""
    out = ["start:14:14.3", "start:17:17.3"]
    out += ["a:%d:%d.3" % (t, t) for t in range(20, 90, 3)]
    return out


# How long a scripted direction press is held. The front end repeats a held
# direction after about a quarter of a second: on macOS a 0.3 s hold on the main
# menu moved the cursor twice and 0.1 and 0.2 s moved it once (2026-10-06, one
# run each). The Windows routes were all tuned with 0.3 s, where the press is seen
# later, and have not been run with anything else, so Windows keeps it.
DIRECTION_HOLD = 0.3 if WINDOWS else 0.2


def expand_presses(keys):
    """`right@5;a@8+4x6` -> ['right:5:5.3', 'a:8:8.3', 'a:12:12.3', ...]."""
    out = []
    for k in keys.split(";"):
        k = k.strip()
        if not k:
            continue
        name, rest = k.split("@", 1)
        if "+" in rest:
            t0, rep = rest.split("+", 1)
            step, n = rep.split("x", 1)
            times = [float(t0) + i * float(step) for i in range(int(n))]
        else:
            times = [float(rest)]
        hold = DIRECTION_HOLD if name.split("-")[-1] in ("up", "down", "left", "right") else 0.3
        out += ["%s:%g:%g" % (name, t, t + hold) for t in times]
    return out


def build_pad_script(stages, raw_tail=""):
    """The RECOMP_PAD_SCRIPT value for a list of `anchor=presses` stages.

    `raw_tail` is appended as written: entries in the runtime's own syntax, for
    what the stage syntax cannot say (a repeating hold, `x:10:1200:0.8:0.15`)."""
    out = title_presses()
    for stage in stages:
        anchor, keys = stage.split("=", 1)
        out.append("@" + anchor)
        out += expand_presses(keys)
    if raw_tail:
        out.append(raw_tail)
    return ",".join(out)


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

# A fighter that walks right and throws every attack, for as long as the run lasts.
FIGHT_TAIL = ("@game.startgame(,right:8:1200:4:1.2,x:10:1200:0.8:0.15,y:10.2:1200:0.8:0.15,"
              "a:10.4:1200:0.8:0.15,b:10.6:1200:0.8:0.15")

# Typing "AAA" on the user-ID screen and taking DONE.
NEW_ID_KEYS = "a@5;a@7;a@8;a@9;down@11+0.6x9;right@17.5;a@19;a@23+3x6"

# Main menu -> Battle. The presses wait for the menu music, which starts once the menu
# is live: with HD texture packs the menu loads its art first (about 3 s on a Steam
# Deck), and presses timed from the screen's first call landed before it could take them.
MAIN_MENU_BATTLE = "audio.playmusic(=right@3;a@6"

# Battle mode up to the second fighter being chosen; the venue screen follows.
ONE_ON_ONE_SETUP = [
    MAIN_MENU_BATTLE,
    # With more than one user ID in the save area Battle asks for one first.
    "getscreeninfo(options/userid=down@4;a@7+4x3",
    "getscreeninfo(battle/cmtype=a@5+4x7",
    "setmatchtype(=a@5+4x7",
    "getscreeninfo(options/battleid=start@6+4x7",
    "controllerbind(0=a@5+4x8",
    "setbmcurrentuserindex(=a@5+5x6",
    "controllersetup(0,0=right@4;a@6;right@10;a@12;a@16;a@20;a@24;a@28",
]

# Free For All (the third match type) up to the fourth fighter being chosen. A
# fighter can be picked once, so each CPU slot moves off the ones before it.
FFA_SETUP = [
    MAIN_MENU_BATTLE,
    "getscreeninfo(options/userid=down@4;a@7+4x3",
    "getscreeninfo(battle/cmtype=right@5;right@6.5;a@9",
    "setmatchtype(=a@5+4x7",
    "getscreeninfo(options/battleid=start@6+4x7",
    "controllerbind(0=a@5+4x8",
    "setbmcurrentuserindex(=a@5+5x6",
    "getscreeninfo(battle/choosef4=a@6+3x5",
    "controllersetup(0,0=right@3;a@5+3x4",
    "controllersetup(1,=right@3;right@4;a@6+3x4",
    "controllersetup(2,=right@3;down@4;a@6+3x4",
    "controllersetup(3,=back@9999",
]

# On the venue map the Terrordome is one step right of the Foundation.
TERRORDOME = "getscreeninfo(battle/chsvenue=right@6;a@9+4x4"

def raw_stages(stages):
    """Stages in the runtime's own pad-script syntax, to follow a repeating tail.

    A route's `tail` is appended to the script as written, and an anchor in it
    ends the repeating entries before it. That is how a second match is driven
    after the first one's scripted fighter: `anchor#N` waits for the Nth call."""
    out = []
    for stage in stages:
        anchor, keys = stage.split("=", 1)
        out.append("@" + anchor)
        out += expand_presses(keys)
    return ",".join(out)


# After a One on One: through the summary, rewards and save back to the match-type
# menu, then a four-fighter Free For All. Counts are of calls since the launch: the
# first match made one SetMatchType, one battleID screen and so on; each match asks
# for the summary twice.
SECOND_MATCH_FFA = raw_stages([
    "game.getmatchsummary(=a@4+3x12",
    "getscreeninfo(battle/cmtype#2=right@5;right@6.5;a@9",
    "setmatchtype(#2=a@5+4x7",
    "getscreeninfo(options/battleid#2=start@6+4x7",
    "controllerbind(0#2=a@5+4x8",
    "setbmcurrentuserindex(#2=a@5+5x6",
    "getscreeninfo(battle/choosef4=a@6+3x5",
    "controllersetup(0#2=right@3;a@5+3x4",
    "controllersetup(1#2=right@3;right@4;a@6+3x4",
    "controllersetup(2,=right@3;down@4;a@6+3x4",
    "controllersetup(3,=back@9999",
    "getscreeninfo(battle/chsvenue#2=a@8+4x4",
])

ROUTES = {
    "boot": {
        "help": "start-up to the main menu, nothing pressed there",
        "stages": ["getscreeninfo(intmain/mainmenu=back@9999"],
        "until": "getscreeninfo(intmain/mainmenu", "after": 8, "secs": 170,
        "shots": "@getscreeninfo(intmain/mainmenu,5",
    },
    "fight": {
        "help": "Battle -> One on One, defaults all the way, then a scripted fighter",
        "stages": ONE_ON_ONE_SETUP + ["controllersetup(1,=a@4+4x6"],
        "tail": FIGHT_TAIL,
        "until": "game.startgame(", "after": 120, "secs": 420,
        "shots": "@game.startgame(,60,110",
    },
    "fight-result": {
        "help": "One on One at the default venue, played until the game shows the match summary",
        "stages": ONE_ON_ONE_SETUP + ["controllersetup(1,=a@4+4x6"],
        "tail": FIGHT_TAIL,
        "until": "game.getmatchsummary(", "after": 6, "secs": 900,
        "shots": "@game.startgame(,60",
    },
    "ffa": {
        "help": "Battle -> Free For All with four fighters at the default venue, then a scripted fighter",
        "stages": FFA_SETUP + ["getscreeninfo(battle/chsvenue=a@8+4x4"],
        "tail": FIGHT_TAIL,
        "until": "game.startgame(", "after": 150, "secs": 520,
        "shots": "@game.startgame(,60,140",
    },
    "two-matches": {
        "help": "One on One to its result, back through the menus, then a Free For All to its result, in one launch",
        "stages": ONE_ON_ONE_SETUP + ["controllersetup(1,=a@4+4x6"],
        "tail": ",".join([FIGHT_TAIL, SECOND_MATCH_FFA, FIGHT_TAIL.replace("@game.startgame(", "@game.startgame(#2")]),
        "until": "game.getmatchsummary(#3", "after": 6, "secs": 1800,
        "shots": "@game.startgame(,60",
    },
    "versus": {
        "help": "One on One between two pads: both press START, each picks a fighter, default venue",
        "stages": [
            MAIN_MENU_BATTLE,
            "getscreeninfo(options/userid=down@4;a@7+4x3",
            "getscreeninfo(battle/cmtype=a@5+4x7",
            "setmatchtype(=a@5+4x7",
            # Each pad joins with START and confirms its user ID with A.
            "getscreeninfo(options/battleid=start@6;p2-start@10;a@14;p2-a@18;a@22;p2-a@26",
            "getscreeninfo(battle/choosef2=right@4;a@7;p2-right@10;p2-right@11.5;p2-a@14;a@18;p2-a@20;a@24;p2-a@26",
            "getscreeninfo(battle/chsvenue=a@8+4x4",
        ],
        "tail": FIGHT_TAIL + "," + FIGHT_TAIL.split(",", 1)[1].replace("right:", "p2-left:").replace(",x:", ",p2-x:")
                .replace(",y:", ",p2-y:").replace(",a:", ",p2-a:").replace(",b:", ",p2-b:"),
        "env": {"RECOMP_USB_PADS": "2"},
        "until": "game.startgame(", "after": 120, "secs": 480,
        "shots": "@game.startgame(,60,110",
    },
    "ffa-result": {
        "help": "Free For All with four fighters at the default venue, played until the match summary",
        "stages": FFA_SETUP + ["getscreeninfo(battle/chsvenue=a@8+4x4"],
        "tail": FIGHT_TAIL,
        "until": "game.getmatchsummary(", "after": 6, "secs": 1200,
        "shots": "@game.startgame(,60",
    },
    "ffa-terrordome": {
        "help": "Free For All with four fighters at the Terrordome (venue 6), played for four minutes",
        "stages": FFA_SETUP + [TERRORDOME],
        "tail": FIGHT_TAIL,
        "until": "game.startgame(", "after": 240, "secs": 620,
        "shots": "@getscreeninfo(battle/oldmload,1,@game.startgame(,15,60,230",
        # Unlocking every venue changes traversal of the map. Pin the actual
        # retail FE ID through ChooseVenue before the loading screen starts.
        "env": {"RECOMP_FE_INJECT": "game.choosevenue#1=>Game.ChooseVenue(6)",
                "RECOMP_START_LOG": "1"}, "expected_venue": 6,
    },
    "fight-terrordome": {
        "help": "One on One at the Terrordome (venue 6), played for four minutes",
        "stages": ONE_ON_ONE_SETUP + ["controllersetup(1,=back@9999", TERRORDOME],
        "tail": FIGHT_TAIL,
        "until": "game.startgame(", "after": 240, "secs": 620,
        "shots": "@getscreeninfo(battle/oldmload,1,@game.startgame(,15,60,230",
        "env": {"RECOMP_FE_INJECT": "game.choosevenue#1=>Game.ChooseVenue(6)",
                "RECOMP_START_LOG": "1"}, "expected_venue": 6,
    },
    # The Story routes pick the THIRD user ID (down twice): the save area has four profiles since 2026-10-06 and the
    # second one no longer has a Story save. If profiles are added or removed, this is the number to revisit.
    "crib": {
        "help": "Story with the first profile in the list, into the crib (NO to the messages prompt)",
        "stages": [
            "getscreeninfo(intmain/mainmenu=a@6",
            "getscreeninfo(options/userid=down@4;down@6;a@9",
            "getscreeninfo(story/crib=down@4;a@6",
        ],
        "until": "getscreeninfo(story/crib", "after": 12, "secs": 260,
        "shots": "@getscreeninfo(story/crib,9",
        "story": True,
    },
    "gym": {
        "help": "crib -> Map -> Shop District -> Stapleton Athletics -> Learn Moves, preview the first move",
        "stages": [
            "getscreeninfo(intmain/mainmenu=a@6",
            "getscreeninfo(options/userid=down@4;down@6;a@9",
            "getscreeninfo(story/crib=down@4;a@6;a@10;a@16;down@24;a@29",
            "getscreeninfo(story/gym=down@4;down@6;a@9;x@16",
        ],
        "until": "getscreeninfo(story/gym", "after": 26, "secs": 300,
        "shots": "@getscreeninfo(story/gym,13,23",
        "story": True,
    },
    "unlock": {
        "help": "Unlock Rewards -> Unlock Fighters, moving the selector around the grid (nothing is bought)",
        "stages": [
            "getscreeninfo(intmain/mainmenu=right@5;right@6.5;a@8",
            "getscreeninfo(options/userid=down@4;down@6;a@9",
            "getscreeninfo(battle/buyrewards=a@6",
            "getscreeninfo(battle/unlockchar=right@8;right@10;right@12;down@15;down@17;left@20",
        ],
        "until": "getscreeninfo(battle/unlockchar", "after": 26, "secs": 280,
        "shots": "@getscreeninfo(battle/unlockchar,6,13.5,18.5,22",
        "story": True,
    },
    "intro": {
        "help": "Story from a new user ID: the opening cutscenes, up to the creator's first screen",
        "stages": [
            "getscreeninfo(intmain/mainmenu=a@6",
            "getscreeninfo(options/userid=" + NEW_ID_KEYS,
            "game.startstorymode(=back@9999",
        ],
        "until": "getscreeninfo(story/chardec", "after": 10, "secs": 330,
        "shots": "@game.startstorymode(,16.5,46,58,@getscreeninfo(story/chardec,8",
        "story": True,
    },
}

# One process already traverses the crib on the way to the gym. Capture and
# evaluate both rather than rebooting once per checkpoint.
ROUTES['story-tour'] = dict(ROUTES['gym'], help='Story profile -> crib -> gym in one launch',
                           shots='@getscreeninfo(story/crib,9,@getscreeninfo(story/gym,13,23')


# ---------------------------------------------------------------------------
# The save area
# ---------------------------------------------------------------------------

def save_area():
    return os.path.join(data_dir(), "save")


def trees_equal(a, b):
    """Same names, same bytes. Read outright: filecmp caches by size and time, and
    a save rewritten within the same second with the same length would pass."""
    c = filecmp.dircmp(a, b, ignore=[])
    if c.left_only or c.right_only or c.funny_files or c.common_funny:
        return False
    for f in c.common_files:
        try:
            with open(os.path.join(a, f), "rb") as fa, open(os.path.join(b, f), "rb") as fb:
                while True:
                    left, right = fa.read(1024 * 1024), fb.read(1024 * 1024)
                    if left != right:
                        return False
                    if not left:
                        break
        except OSError:
            return False
    return all(trees_equal(os.path.join(a, d), os.path.join(b, d)) for d in c.common_dirs)


class SaveGuard:
    """Copy the save area aside, and put it back when the block ends.

    The copies live beside the save area (`save-guard/<stamp>`), not in the repo,
    and the three most recent are kept: a scripted run uses the player's own
    profile, and a guard that failed to restore must leave something to restore
    from by hand."""

    def __init__(self, data_root=None):
        self.data_root = data_root or data_dir()
        self.src = os.path.join(self.data_root, "save")
        self.copy = None
        self.backup = None
        self.restored_equal = None
        self.existed = False

    def __enter__(self):
        self.existed = os.path.isdir(self.src)
        if os.path.exists(self.src) and not self.existed:
            raise RuntimeError("save root is not a directory: " + self.src)
        root = os.path.join(self.data_root, "save-guard")
        os.makedirs(root, exist_ok=True)
        self.backup = tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-"), dir=root)
        self.copy = os.path.join(self.backup, 'payload')
        os.mkdir(self.copy)
        if self.existed:
            shutil.copytree(self.src, self.copy, dirs_exist_ok=True)
        return self

    def __exit__(self, *exc):
        if not self.copy:
            return False
        kill_stray()
        # DEFJAM_HARNESS_SNAPSHOT_SAVE=<dir>: keep what the run wrote, before the restore
        # puts the original back (a profile the game itself produced, e.g. with RECOMP_UNLOCK_ALL).
        snapshot = os.environ.get("DEFJAM_HARNESS_SNAPSHOT_SAVE")
        if snapshot and os.path.isdir(self.src):
            shutil.rmtree(snapshot, ignore_errors=True)
            shutil.copytree(self.src, snapshot)
        for attempt in range(5):
            try:
                if os.path.isdir(self.src):
                    shutil.rmtree(self.src)
                elif os.path.lexists(self.src):
                    os.remove(self.src)
                if self.existed:
                    shutil.copytree(self.copy, self.src)
                break
            except OSError:
                time.sleep(1.0)
        try:
            self.restored_equal = (trees_equal(self.src, self.copy) if self.existed
                                   else not os.path.exists(self.src))
        except OSError:
            self.restored_equal = False
        root = os.path.dirname(self.backup)
        # Preserve failed backups even after later successful runs.
        if self.restored_equal:
            status_root = os.path.join(root, '.status')
            os.makedirs(status_root, exist_ok=True)
            open(os.path.join(status_root, os.path.basename(self.backup) + '.ok'), 'w').close()
            successful = [d for d in sorted(os.listdir(root))
                          if os.path.isfile(os.path.join(status_root, d + '.ok'))]
            for old in successful[:-3]:
                shutil.rmtree(os.path.join(root, old), ignore_errors=True)
                if not os.path.exists(os.path.join(root, old)):
                    os.remove(os.path.join(status_root, old + '.ok'))
        return False


# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------

_OWNED_PROCESSES = set()


def kill_stray():
    """Reap only this harness's children; other worktrees may be running games."""
    for process in list(_OWNED_PROCESSES):
        if process.poll() is None:
            process.kill()
        process.wait(timeout=15)
        _OWNED_PROCESSES.discard(process)


def read_log(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


PRESENT_RE = re.compile(r"\[D3D\] 2\.0s: (\d+) present")
LAST_LOG = None          # the .log.err of the most recent run_game, so a caller can point at it


def triage_hint(log):
    """The command that digests a run log; the first thing to run on a failed one."""
    rel = os.path.relpath(log, REPO)
    return "python scripts/triage.py %s" % rel.replace(os.sep, "/")


def summarise(text):
    """What a log says about a run. Pure, so it can be tested without the game."""
    presents = [int(m) for m in PRESENT_RE.findall(text)]
    calls = [l[len("[FUNCCALL] "):].strip() for l in text.splitlines() if l.startswith("[FUNCCALL] ")]
    return {
        "presents": presents,
        "crashes": text.count("[CRASH]"),
        "watchdogs": text.count("[WATCHDOG]"),
        "debug_layer": text.count("[D3D11-DEBUG]"),
        "truncated": text.count("indices truncated"),
        "icall_failures": text.count("[ICALL] Failed to resolve"),
        "icall_noncode": len(re.findall(r"\[ICALL\] target .* is not code -- skipped", text)),
        "unimplemented": text.count("[UNIMPL] untranslated instruction REACHED"),
        "baseline_noops": text.count("[UNIMPL-KNOWN] untranslated instruction REACHED"),
        "calls": calls,
    }


def reached(summary, anchor):
    """Whether a `[FUNCCALL]` line contains the anchor, the way the runtime matches it."""
    a, _, nth = anchor.lower().partition("#")     # `anchor#N`: the Nth such call
    return sum(a in c.lower() for c in summary["calls"]) >= (int(nth) if nth else 1)


def presents_after(text, anchor):
    """The `[D3D] 2.0s` present counts logged after the anchor's first `[FUNCCALL]`."""
    a = anchor.lower()
    seen = False
    out = []
    for line in text.splitlines():
        if not seen:
            if line.startswith("[FUNCCALL] ") and a in line.lower():
                seen = True
            continue
        m = PRESENT_RE.search(line)
        if m:
            out.append(int(m.group(1)))
    return out


class LogTail:
    """Follow a growing log without re-reading it: a long run's log is hundreds of megabytes."""

    def __init__(self, path):
        self.path = path
        self.pos = 0
        self.rest = b""
        self.presents = 0
        self.calls = []
        self.timeline = []

    def poll(self, elapsed=None):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return
        self.pos += len(data)
        lines = (self.rest + data).split(b"\n")
        self.rest = lines.pop()
        for raw in lines:
            if raw.startswith(b"[FUNCCALL] "):
                call = raw[11:].decode("utf-8", "replace").strip().lower()
                self.calls.append(call)
                self.timeline.append({'call': call, 'observed_seconds': elapsed})
            elif b"[D3D] 2.0s:" in raw:
                self.presents += 1

    def saw(self, anchor):
        """Has a call containing `anchor` been logged? `anchor#N` asks for the Nth such call."""
        a, _, nth = anchor.lower().partition('#')
        return sum(a in c for c in self.calls) >= (int(nth) if nth else 1)


class Run:
    def __init__(self, name, log, shots_dir):
        self.name = name
        self.log = log                  # the .log.err path: everything of interest is on stderr
        self.shots_dir = shots_dir
        self.text = ""
        self.summary = summarise("")
        self.save_ok = None             # None: no guard; True/False: restored identical or not
        self.hung = False
        self.exit_code = None
        self.unexpected_exit = False
        self.missing_anchor = None
        self.identity = {}
        self.elapsed = 0.0
        self.timeline = []
        self.pcm_boundaries = []
        self.memory = []                # private bytes sampled every five seconds

    def shots(self):
        if not os.path.isdir(self.shots_dir):
            return []
        return sorted(os.path.join(self.shots_dir, f) for f in os.listdir(self.shots_dir)
                      if f.lower().endswith(".bmp"))


def process_memory(pid):
    """(private bytes, working set) of a process, or None. No dependency.

    Off Windows both numbers are the resident set from `ps`: there is no cheap
    private-bytes figure, and growth is what the caller looks for."""
    if os.name != "nt":
        try:
            out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True, timeout=5)
            rss = int(out.stdout.split()[0]) * 1024
            return rss, rss
        except (OSError, ValueError, IndexError, subprocess.SubprocessError):
            return None
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                    ("PrivateUsage", ctypes.c_size_t)]
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel32.OpenProcess(0x1000 | 0x0010, False, pid)   # QUERY_LIMITED_INFORMATION | VM_READ
    if not handle:
        return None
    try:
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if not kernel32.K32GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return None
        return counters.PrivateUsage, counters.WorkingSetSize
    finally:
        kernel32.CloseHandle(handle)


def run_game(name, stages=(), tail="", shots="", secs=240, preset=RELEASE_PRESET, env=None,
             until=None, after=0, story=False, scripted=True, stall_secs=0, on_stall=None, quiet=False,
             until_file=None, controller=None, inherit_recomp=True):
    """Run the game once. Returns a Run.

    until/after: stop `after` seconds after the first `[FUNCCALL]` containing
    `until`, instead of waiting out `secs` (which stays the upper bound).
    stall_secs: if no `[D3D] 2.0s` line arrives for this long before `until` is
    reached, the run is a hang: `on_stall(run, process)` is called while the
    process is still alive, then it is stopped.
    until_file: stop a few seconds after this file exists (a capture that is all
    the run is for)."""
    exe = exe_path(preset)
    validate_shots(shots)
    if not os.path.isfile(exe):
        raise SystemExit("not built: %s (scripts/build.ps1 -Preset %s, or scripts/pipeline.py build --preset %s)"
                         % (exe, preset, preset))
    try:
        build_state = pipeline_state.verify_build(Path(REPO), preset)
    except (OSError, ValueError, KeyError) as ex:
        raise SystemExit(str(ex)) from ex
    if not os.path.exists(os.path.join(REPO, "game", "default.xbe")):
        raise SystemExit("./game/default.xbe missing: make a junction to your extracted dump")
    kill_stray()

    logs = os.path.join(REPO, "logs")
    shots_dir = os.path.join(logs, "shots", name)
    os.makedirs(shots_dir, exist_ok=True)
    for f in os.listdir(shots_dir):
        if f.lower().endswith((".bmp", ".png")):
            os.remove(os.path.join(shots_dir, f))
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = os.path.join(logs, "run-%s.log" % stamp)

    e = {k: v for k, v in os.environ.items() if inherit_recomp or not k.upper().startswith('RECOMP_')}
    e.update(BASE_ENV)
    e["DEFJAM_DATA"] = data_dir()
    if scripted:
        e["RECOMP_SCRIPT_ANCHOR"] = TITLE_ANCHOR
        e["RECOMP_PAD_SCRIPT"] = build_pad_script(stages, tail)
    if shots:
        e["RECOMP_TRANS_SHOT"] = os.path.join(shots_dir, name + ".bmp")
        e["RECOMP_TRANS_SHOT_SECS"] = shots
    e.update(env or {})
    unlock_default(e)
    startup_default(e)
    e['DEFJAM_DATA'] = guarded_data_root(e['DEFJAM_DATA'])

    run = Run(name, log + ".err", shots_dir)
    run.identity = {'preset': preset, 'executable': exe, 'build_state': build_state,
                    'environment': {k: v for k, v in e.items() if k.startswith('RECOMP_')},
                    'data_root': e['DEFJAM_DATA'], 'started_utc': datetime.now(timezone.utc).isoformat(),
                    'inherits_recomp_environment': inherit_recomp,
                    'input_schedule': {'stages': list(stages), 'tail': tail, 'shots': shots,
                                       'clock': 'anchor-relative host seconds', 'timeout_seconds': secs},
                    'log_observation_interval_seconds': 0.05 if controller is not None else 2.0}
    guard = SaveGuard(e['DEFJAM_DATA'])
    guard.__enter__()
    try:
        with open(log, "wb") as out, open(log + ".err", "wb") as err:
            p = subprocess.Popen([exe], cwd=REPO, stdout=out, stderr=err, env=e)
            _OWNED_PROCESSES.add(p)
            t0 = time.time()
            hit = None
            last_presents, last_change = -1, time.time()
            tail_log = LogTail(run.log)
            memory_at = 0.0
            while p.poll() is None and time.time() - t0 < secs:
                # Live recipes can contain adjacent menu presses. A two-second
                # monitor poll merges them into one held button report.
                time.sleep(0.05 if controller is not None else 2.0)
                tail_log.poll(time.time() - t0)
                if time.time() - memory_at >= 5.0:
                    # The game's private bytes over the run, for a growth check.
                    memory_at = time.time()
                    sample = process_memory(p.pid)
                    if sample:
                        run.memory.append({'seconds': round(memory_at - t0, 1),
                                           'private_bytes': sample[0], 'working_set': sample[1]})
                pcm = e.get('RECOMP_APU_PCM')
                if pcm and os.path.isfile(pcm):
                    # File offsets observed by the host, after log delivery.
                    # This excludes startup PCM from coarse scenario health
                    # gates; it is not sample-accurate speech/hit alignment.
                    offset = os.path.getsize(pcm)
                    run.pcm_boundaries.extend(dict(event, pcm_bytes=offset)
                        for event in tail_log.timeline[len(run.pcm_boundaries):])
                if controller is not None and controller.poll(tail_log, time.time() - t0):
                    break
                if until and hit is None and tail_log.saw(until):
                    hit = time.time()
                if hit is not None and time.time() - hit >= after:
                    break
                if until_file and os.path.isfile(until_file):
                    time.sleep(3.0)
                    break
                if stall_secs and hit is None:
                    n = tail_log.presents
                    if n != last_presents:
                        last_presents, last_change = n, time.time()
                    elif n > 0 and time.time() - last_change >= stall_secs:
                        run.hung = True
                        if on_stall:
                            on_stall(run, p)
                        break
            if p.poll() is None:
                p.kill()
                p.wait(timeout=15)
            else:
                run.unexpected_exit = True
            run.exit_code = p.returncode
            run.elapsed = time.time() - t0
            tail_log.poll(run.elapsed)
            run.timeline = tail_log.timeline
    finally:
        guard.__exit__(None, None, None)
        run.save_ok = guard.restored_equal
    run.text = read_log(run.log)
    run.summary = summarise(run.text)
    global LAST_LOG
    LAST_LOG = run.log
    if not quiet:
        print_run(run)
    return run


def run_route(route, preset=RELEASE_PRESET, secs=None, shots=None, env=None, extra_stages=(),
              to_end=False, **kw):
    r = ROUTES[route]
    stages = list(r["stages"]) + list(extra_stages)
    if r.get("env"):
        env = dict(r["env"], **(env or {}))     # what the route needs; the caller's values win
    run = run_game(route, stages=stages, tail=r.get("tail", ""),
                    shots=r.get("shots", "") if shots is None else shots,
                    secs=secs or r["secs"], preset=preset, env=env,
                    until=None if to_end else r.get("until"), after=r.get("after", 0),
                    story=r.get("story", False), **kw)
    if r.get('until') and not reached(run.summary, r['until']):
        run.missing_anchor = r['until']
    return run


def print_run(run):
    s = run.summary
    p = s["presents"]
    print("log      %s" % os.path.relpath(run.log, REPO))
    if p:
        srt = sorted(p)
        print("presents %d lines of 2 s: min %d, median %d, max %d" % (len(p), srt[0], srt[len(srt) // 2], srt[-1]))
    print("crashes  %d   watchdogs %d   debug-layer %d   truncated batches %d   unresolved calls %d"
          % (s["crashes"], s["watchdogs"], s["debug_layer"], s["truncated"], s["icall_failures"]))
    if run.hung:
        print("HUNG     presenting stopped")
    if run.save_ok is not None:
        print("save     %s" % ("restored, identical" if run.save_ok else "RESTORE FAILED: see save-guard/ beside the save area"))
    faults = run_faults(run)
    if faults:
        print("FAIL     " + "; ".join(faults))
        print("triage   " + triage_hint(run.log))
    if s['baseline_noops']:
        print("baseline %d reported verified inherited omissions" % s['baseline_noops'])
    for f in run.shots():
        print("shot     %s" % os.path.relpath(f, REPO))


# ---------------------------------------------------------------------------
# Hang catcher
# ---------------------------------------------------------------------------

def dump_hang(run, process):
    """Everything worth having from a process that stopped presenting, while it lives."""
    out = os.path.join(REPO, "logs", "hang-" + time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    for script, args, name in (("native-stacks.py", [], "native.txt"),
                               ("guest-stack.py", [], "guest.txt"),
                               ("sample-threads.py", ["--samples", "6", "--interval", "0.3"], "sample.txt"),
                               ("hang-peek.py", [], "state.txt")):
        path = os.path.join(REPO, "scripts", script)
        if not os.path.isfile(path):
            continue
        try:
            r = subprocess.run([sys.executable, path] + args, cwd=REPO, capture_output=True, timeout=120)
            with open(os.path.join(out, name), "wb") as f:
                f.write(r.stdout + r.stderr)
        except Exception as ex:                                   # a diagnostic must not end the catch
            with open(os.path.join(out, name), "w") as f:
                f.write("failed: %r\n" % (ex,))
    shutil.copyfile(run.log, os.path.join(out, "log.err"))
    print("hang dump: %s" % os.path.relpath(out, REPO))


def soak(boots, preset, stop_on_hang=True):
    """Boot to the main menu `boots` times; a boot that stops presenting is a hang."""
    ok = hung = failed = 0
    for k in range(1, boots + 1):
        r = run_route("boot", preset=preset, shots="", stall_secs=10, on_stall=dump_hang, quiet=True)
        if r.hung:
            hung += 1
            verdict = "HUNG"
        elif reached(r.summary, "getscreeninfo(intmain/mainmenu") and not run_faults(r):
            ok += 1
            verdict = "ok"
        else:
            failed += 1
            verdict = "FAILED (" + "; ".join(run_faults(r) or ["no main menu"]) + ")"
        print("boot %d/%d %s  %s" % (k, boots, verdict, os.path.relpath(r.log, REPO)), flush=True)
        if r.hung and stop_on_hang:
            break
    print("soak: %d ok, %d hung, %d failed" % (ok, hung, failed))
    return ok, hung, failed


DIR_RE = re.compile(r'\[DIR\] .* pattern "([^"*?]+)" -> 0x00000000 (?:dir|file) (\S+)')


def run_faults(run):
    """Shared failure policy for routes, goldens, standalone runs and soaks."""
    out = []
    for key, label in (("crashes", "[CRASH]"), ("watchdogs", "[WATCHDOG]"),
                       ("debug_layer", "[D3D11-DEBUG]"), ("truncated", "truncated batches"),
                       ("icall_failures", "unresolved indirect calls"),
                       ("icall_noncode", "indirect calls to non-code"),
                       ("unimplemented", "reached untranslated instructions")):
        if run.summary[key]:
            out.append("%d %s" % (run.summary[key], label))
    if run.hung:
        out.append("hung")
    if run.missing_anchor:
        out.append("never reached " + run.missing_anchor)
    if run.unexpected_exit:
        out.append("unexpected process exit %s" % run.exit_code)
    if run.save_ok is not True:
        out.append("save root not restored identical")
    wrong = [m for m in DIR_RE.findall(run.text) if m[0].lower() != m[1].lower()]
    if wrong:
        out.append('directory search for "%s" answered "%s"' % wrong[0])
    return out


# ---------------------------------------------------------------------------
# Captures: BMP -> PNG and a contact sheet, for looking at them locally
# ---------------------------------------------------------------------------

def load_bmp(path):
    b = open(path, "rb").read()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    bottom_up = h > 0
    h = abs(h)
    step = bpp // 8
    stride = (w * step + 3) & ~3
    rows = []
    for y in range(h):
        sy = h - 1 - y if bottom_up else y
        row = b[off + sy * stride: off + sy * stride + w * step]
        rgb = bytearray(w * 3)
        rgb[0::3] = row[2::step]
        rgb[1::3] = row[1::step]
        rgb[2::3] = row[0::step]
        rows.append(bytes(rgb))
    return w, h, rows


def write_png(path, w, h, rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = zlib.compress(b"".join(b"\x00" + bytes(r) for r in rows), 6)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def bmp_to_png(src, dst=None):
    w, h, rows = load_bmp(src)
    dst = dst or os.path.splitext(src)[0] + ".png"
    write_png(dst, w, h, rows)
    return dst


def montage(out, cols, scale, paths):
    tiles = [load_bmp(p) for p in paths]
    w, h = tiles[0][0] // scale, tiles[0][1] // scale
    nrows = (len(tiles) + cols - 1) // cols
    img = [bytearray(w * cols * 3) for _ in range(h * nrows)]
    for i, (tw, th, rows) in enumerate(tiles):
        cx, cy = (i % cols) * w, (i // cols) * h
        for y in range(min(h, th // scale)):
            src = rows[y * scale]
            line = bytearray(w * 3)
            for k in range(3):
                line[k::3] = src[k::3 * scale][:w]
            img[cy + y][cx * 3:(cx + w) * 3] = line
    write_png(out, w * cols, h * nrows, img)
    return out


# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("routes", help="list the scripted routes")

    r = sub.add_parser("run", help="run one route")
    r.add_argument("route", choices=sorted(ROUTES))
    r.add_argument("--preset", default=os.environ.get("RECOMP_PRESET", RELEASE_PRESET))
    r.add_argument("--secs", type=int, help="upper bound on the run (the route has a default)")
    r.add_argument("--shots", help="capture times, RECOMP_TRANS_SHOT_SECS syntax; '' for none")
    r.add_argument("--stage", action="append", default=[], help="an extra `anchor=presses` stage (repeatable)")
    r.add_argument("--env", action="append", default=[], help="NAME=VALUE for the game (repeatable)")
    r.add_argument("--to-end", action="store_true", help="run the full --secs; do not stop at the route's anchor")
    r.add_argument("--png", action="store_true", help="also write a PNG beside each capture")

    s = sub.add_parser("soak", help="boot to the main menu repeatedly; dump state at a hang")
    s.add_argument("--boots", type=int, default=10)
    s.add_argument("--preset", default=os.environ.get("RECOMP_PRESET", DEBUG_PRESET))
    s.add_argument("--keep-going", action="store_true", help="do not stop at the first hang")

    p = sub.add_parser("png", help="convert captures to PNG for viewing")
    p.add_argument("bmp", nargs="+")

    m = sub.add_parser("montage", help="a contact sheet of captures")
    m.add_argument("out")
    m.add_argument("cols", type=int)
    m.add_argument("scale", type=int, help="shrink each tile by this factor")
    m.add_argument("bmp", nargs="+")

    a = ap.parse_args(argv)
    if a.cmd == "routes":
        for name in sorted(ROUTES):
            print("%-6s %s  [save guard]" % (name, ROUTES[name]["help"]))
        return 0
    if a.cmd == "run":
        env = dict(kv.split("=", 1) for kv in a.env)
        run = run_route(a.route, preset=a.preset, secs=a.secs, shots=a.shots, env=env,
                        extra_stages=a.stage, to_end=a.to_end)
        if a.png:
            for f in run.shots():
                print("png      %s" % os.path.relpath(bmp_to_png(f), REPO))
        return 1 if run_faults(run) else 0
    if a.cmd == "soak":
        ok, hung, failed = soak(a.boots, a.preset, stop_on_hang=not a.keep_going)
        return 1 if (hung or failed) else 0
    if a.cmd == "png":
        for f in a.bmp:
            print(bmp_to_png(f))
        return 0
    if a.cmd == "montage":
        print(montage(a.out, a.cols, a.scale, a.bmp))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
