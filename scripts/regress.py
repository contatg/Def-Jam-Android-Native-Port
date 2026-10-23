#!/usr/bin/env python3
"""The regression run: everything that is checked after a change, in one command.

    python scripts/regress.py                 # all checks, release build (about 20 minutes)
    python scripts/regress.py --quick         # unit tests, the three golden frames, one fight
    python scripts/regress.py --only fight,intro
    python scripts/regress.py --preset win-x64-debug --soak 10

Checks, in order:
  unit     pytest tests/unit (no game needed)
  m2       the loading screen's frame signature        tests/golden/m2-loading-screen.json
  m3       the title screen's                          tests/golden/m3-title-screen.json
  m4a      the main menu's                             tests/golden/m4a-main-menu.json
  launcher the launcher window (macOS, Linux): its first screen is drawn, a tab and a
           checkbox are clicked through the real event path, the change is in the settings
           file, and Play goes on into the game
  overlay  the F1 overlay (macOS, Linux): opened by a key event, a tab clicked, drawn over
           the picture, and the window's frame read back (RECOMP_WINDOW_SHOT)
  skipintro (--only) default/file/environment startup skip, first screen, title golden and Start
  fight    a scripted One on One: reaches the fight, holds the frame rate, no crash
  ffa      a scripted four-fighter Free For All at the default venue, the same checks
  ffa-terrordome  the same at the Terrordome for four minutes (the v0.2.1 crash)
  combat   a One on One played to its result on a copy of the profiles: movement, a
           player attack, damage, the result and the summary screen asserted from
           game state, plus audio, frame pacing and memory (scripts/scenario_suite.py)
  versus   a One on One between two emulated pads: both join, pick fighters, walk and
           land damaging hits (the second pad through the USB model)
  two-matches  (--only) a One on One and a Free For All played to their results in
           one launch, each judged on its own events (about 12 minutes)
  ffa-result  (--only) a four-fighter match played to its result: every fighter but
           the winner put out once
  replay   one step-timed, seed-pinned fight: the first thirty seconds of game state
           must hash to tests/golden/fight-stream.json (how the fight plays)
  visual   (--only) crib and Learn Moves against the locally approved images in
           <data folder>/test-baselines/story-tour-v1; blocked when there are none
  repeat   (--only) the same step-timed, seed-pinned fight twice: the first thirty
           seconds of game state must be identical record for record, and match
           the recorded hash in tests/golden/fight-stream.json
  intro    Story from a new ID: both cutscenes play to the creator, no truncated batch
  crib     Story with the first saved profile reaches the crib
  gym      Learn Moves opens and a move's preview movie is requested
  soak     N boots to the main menu, none hung

The golden frames were taken without the title's gamma ramp, so those three runs
set RECOMP_GAMMA=0. Every run uses the harness's complete-save-root guard and
fails if the save area is not byte-identical afterwards.

Quick and full sets each play legal and all startup movies once, in m3, with
ordered log assertions and the title golden. Other runs use the harness's startup
skip default and anchor input/captures to the actual Press Start screen request.
Story cutscenes and move previews still play in their own checks.

A check that does not apply on this host (the launcher and overlay checks on Windows) reports SKIP:
it is counted as skipped, never as passed. Exit code 0 only if every check that ran passed. The
table is also written to logs/regress-<stamp>.txt.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("harness", os.path.join(HERE, "harness.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
spec = importlib.util.spec_from_file_location('test_evidence', os.path.join(HERE, 'test_evidence.py'))
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)
REPO = harness.REPO

ORDER = ["unit", "m2", "m3", "m4a", "launcher", "overlay", "unlockopt", "picture", "skipintro", "fight", "ffa", "fight-terrordome", "ffa-terrordome", "combat", "versus", "ffa-result", "two-matches", "replay", "repeat", "visual",
         "intro", "crib", "gym", "soak"]
# Opt-in (--only): the One on One at the Terrordome, which the Free For All covers, and
# the repeatability check, which is two more fights.
DEFAULT = [n for n in ORDER if n not in ("fight-terrordome", "ffa-result", "two-matches", "repeat", "visual", "skipintro")]
QUICK = ["unit", "m2", "m3", "m4a", "fight"]

# A 2 s line of a 60 Hz title holds 120 presents. Loading screens run at 30, so the
# floor is on the median, and lower for the debug build.
FPS_FLOOR = {"win-x64-release": 110, "win-x64-debug": 100, "posix-release": 110, "posix-debug": 100}
# Four fighters cost the unoptimised build its 60 frames a second (median 95 presents
# per 2 s measured 2026-10-05); the check there is for crashes and a playable rate.
FFA_FPS_FLOOR = {"win-x64-release": 110, "win-x64-debug": 80, "posix-release": 110, "posix-debug": 80}

def median(xs):
    return sorted(xs)[len(xs) // 2] if xs else 0


def common_faults(run):
    """What no run should show, whatever it was for."""
    faults = harness.run_faults(run)
    if "[UI] script target missing:" in run.text:
        faults.append("a scripted launcher control was unavailable")
    return faults


def verdict(faults, detail):
    return (not faults), ("; ".join(faults) if faults else detail)


def not_applicable(why):
    """What a check returns when it cannot be run on this host: neither a pass nor a failure."""
    return None, "not run: " + why


# --- checks ----------------------------------------------------------------

def check_unit(a):
    # The slow tests (rebuilding the branding set) run in CI and with `pytest tests/unit`; the
    # regression run is about the game, and a seven-second unit check is worth running first.
    r = subprocess.run([sys.executable, "-m", "pytest", "-q", "-m", "not slow", "tests/unit"],
                       cwd=REPO, capture_output=True, text=True)
    last = (r.stdout.strip().splitlines() or r.stderr.strip().splitlines() or ["no output"])[-1]
    return r.returncode == 0, last


def golden(a, name, frame, json_name, env, secs, normal_startup=False):
    e = {"RECOMP_GAMMA": "0"}
    if normal_startup and os.environ.get('RECOMP_HEADLESS'):
        e['RECOMP_HEADLESS'] = os.environ['RECOMP_HEADLESS']
    e.update(env)
    path = os.path.join(REPO, "logs", "shots", "golden-" + name, frame)
    run = harness.run_game("golden-" + name, secs=secs, preset=a.preset, env=e, scripted=False, quiet=True,
                           until_file=path, inherit_recomp=not normal_startup)
    faults = common_faults(run)
    if normal_startup:
        faults += normal_startup_faults(run.text)
    if not os.path.isfile(path):
        return False, "; ".join(faults + ["no capture (%s)" % os.path.relpath(run.log, REPO)])
    r = subprocess.run([sys.executable, os.path.join(HERE, "frame-signature.py"), path, "--check",
                        os.path.join(REPO, "tests", "golden", json_name)], cwd=REPO, capture_output=True, text=True)
    line = (r.stdout.strip().splitlines() or [r.stderr.strip() or "no output"])[-1]
    if r.returncode != 0 or not line.startswith("OK"):
        faults.append(line)
    return verdict(faults, ('legal, EA/THX/intro movies, Press Start; ' if normal_startup else '') + line)


# The loading screen has no [FUNCCALL] to anchor on, and which frame it is on
# depends on how fast the build starts (frame 150 in debug; release is past it by
# then). So several frames are captured and one of them must be the screen.
M2_FRAMES = [40, 70, 100, 150, 220]


def check_m2(a):
    shot = os.path.join(REPO, "logs", "shots", "golden-m2", "frame.bmp")
    last = os.path.join(REPO, "logs", "shots", "golden-m2", "frame-%d.bmp" % M2_FRAMES[-1])
    run = harness.run_game("golden-m2", secs=60, preset=a.preset, scripted=False, quiet=True, until_file=last,
                           env={"RECOMP_GAMMA": "0", "RECOMP_TRANS_SHOT": shot,
                                "RECOMP_TRANS_SHOT_FRAME": ",".join(map(str, M2_FRAMES))})
    faults = common_faults(run)
    seen = []
    for path in run.shots():
        r = subprocess.run([sys.executable, os.path.join(HERE, "frame-signature.py"), path, "--check",
                            os.path.join(REPO, "tests", "golden", "m2-loading-screen.json")],
                           cwd=REPO, capture_output=True, text=True)
        line = (r.stdout.strip().splitlines() or r.stderr.strip().splitlines() or ["no output"])[-1]
        if r.returncode == 0 and line.startswith("OK"):
            return verdict(faults, "%s matches" % os.path.basename(path))
        seen.append("%s: %s" % (os.path.basename(path), line))
    faults.append("no captured frame is the loading screen (%s)" % ("; ".join(seen) or "no captures"))
    return verdict(faults, "")


def check_m3(a):
    shot = os.path.join(REPO, "logs", "shots", "golden-m3", "title.bmp")
    return golden(a, "m3", "title-5s.bmp", "m3-title-screen.json",
                  {"RECOMP_SKIP_INTRO": "0", "RECOMP_SCRIPT_ANCHOR": harness.TITLE_ANCHOR,
                   "RECOMP_TRANS_SHOT": shot, "RECOMP_TRANS_SHOT_SECS": "5"}, 150,
                  normal_startup=True)


def check_m4a(a):
    shot = os.path.join(REPO, "logs", "shots", "golden-m4a", "menu.bmp")
    # Screen-request time precedes the old flow-file anchor. Keep the title's
    # native input delay, then dismiss storage notices and back out of Story.
    pad = ("start:14:14.3,start:17:17.3,a:20:20.3,a:23:23.3,a:26:26.3,a:29:29.3,"
           "a:32:32.3,a:35:35.3,a:38:38.3,b:42:42.3,b:46:46.3")
    return golden(a, "m4a", "menu-50s.bmp", "m4a-main-menu.json",
                  {"RECOMP_SCRIPT_ANCHOR": harness.TITLE_ANCHOR, "RECOMP_TRANS_SHOT": shot,
                   "RECOMP_TRANS_SHOT_SECS": "50", "RECOMP_PAD_SCRIPT": pad}, 200)


# --- the launcher and the overlay (drawn through SDL and Vulkan: macOS and Linux) -----------------

UI_TAB = (141, 45, 42)          # selected category, oxblood
UI_PLAY = (214, 173, 82)         # Play, ochre


def bmp_color_count(path, colors, tolerance=3):
    """How many pixels of a 32-bit BMP (as the game writes them) are within `tolerance` of each
    (r, g, b) in `colors`; also the image's size."""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError("not a BMP: " + path)
    offset, = struct.unpack_from("<I", data, 10)
    width, height, _planes, bits = struct.unpack_from("<iiHH", data, 18)
    if bits != 32:
        raise ValueError("expected a 32-bit BMP, got %d bits" % bits)
    counts = {}
    for (px,) in struct.iter_unpack("<I", data[offset:offset + abs(height) * width * 4]):
        counts[px & 0xFFFFFF] = counts.get(px & 0xFFFFFF, 0) + 1
    out = []
    for r, g, b in colors:
        n = 0
        for rgb, c in counts.items():
            if abs(((rgb >> 16) & 255) - r) <= tolerance and abs(((rgb >> 8) & 255) - g) <= tolerance \
                    and abs((rgb & 255) - b) <= tolerance:
                n += c
        out.append(n)
    return width, abs(height), out


def ui_settings_file(name, text=""):
    folder = os.path.join(REPO, "logs", "ui-check")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, name + ".ini")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def check_launcher(a):
    if sys.platform == "win32":
        return not_applicable("this check is for the SDL launcher (macOS, Linux); the Windows one is Direct3D 11")
    shot = os.path.join(REPO, "logs", "shots", "ui-launcher", "launcher.bmp")
    os.makedirs(os.path.dirname(shot), exist_ok=True)
    if os.path.exists(shot):
        os.remove(shot)
    settings = ui_settings_file("launcher")
    # Visit direct routes, save skip in Interface, return Home, capture and click Play.
    script = "800:ui:Mods;1300:ui:Back;1800:ui:Configure;2300:ui:Back;2800:ui:Advanced;3300:ui:Interface;3800:ui:launcher.skip;4300:ui:Back;4700:move:1050:25;5100:shot:%s;5600:ui:Play" % shot
    run = harness.run_game("ui-launcher", secs=120, preset=a.preset, scripted=False, quiet=True, until_file=shot,
                           env={"RECOMP_SETTINGS": settings, "RECOMP_LAUNCHER_SCRIPT": script,
                                "RECOMP_INPUT_IGNORE_FOCUS": "1"})
    faults = common_faults(run)
    for needle in ("[UI] launcher shown", "[UI] launcher: frame written"):
        if needle not in run.text:
            faults.append("no %r in the log" % needle)
    if not os.path.isfile(shot):
        return False, "; ".join(faults + ["no frame captured"])
    width, height, (play,) = bmp_color_count(shot, [UI_PLAY])
    if play < 300:
        faults.append("no Play button in the captured frame (%d pixels)" % play)
    with open(settings, encoding="utf-8") as f:
        ini = f.read()
    if not re.search(r"^skip\s*=\s*true\s*$", ini, re.M):
        faults.append("the click on \"Skip the launcher at start\" did not reach settings.ini")
    if "[UI] launcher closed: play" not in run.text:
        faults.append("Play did not close the launcher")
    return verdict(faults, "%dx%d frame, Home and Play drawn, skip saved, Play went on" % (width, height))


def check_overlay(a):
    if sys.platform == "win32":
        return not_applicable("this check is for the Vulkan overlay (macOS, Linux); the Windows one is Direct3D 11")
    shot = os.path.join(REPO, "logs", "shots", "ui-overlay", "overlay.bmp")
    os.makedirs(os.path.dirname(shot), exist_ok=True)
    if os.path.exists(shot):
        os.remove(shot)
    settings = ui_settings_file("overlay", "[launcher]\nskip = true\n")
    # F1 opens the shared Advanced screen on Display; capture its selected category.
    script = "20000:key:F1;28000:key:F1"
    run = harness.run_game("ui-overlay", secs=150, preset=a.preset, scripted=False, quiet=True, until_file=shot,
                           env={"RECOMP_SETTINGS": settings, "RECOMP_HOST_SCRIPT": script,
                                "RECOMP_WINDOW_SHOT": shot, "RECOMP_WINDOW_SHOT_FRAME": "150"})
    faults = common_faults(run)
    for needle in ("[UI] overlay opened", "[UI] overlay ready", "window picture written"):
        if needle not in run.text:
            faults.append("no %r in the log" % needle)
    if "[UI] launcher shown" in run.text:
        faults.append("the launcher was shown although it is skipped")
    if not os.path.isfile(shot):
        return False, "; ".join(faults + ["no frame captured"])
    width, height, (tab,) = bmp_color_count(shot, [UI_TAB])
    if tab < 300:
        faults.append("no selected tab in the captured window (%d tab-coloured pixels): the overlay is not drawn or its tab was not clicked" % tab)
    return verdict(faults, "opened by F1, Display category drawn, %dx%d window frame has the overlay" % (width, height))


def check_unlock_option(a):
    """The launcher's "Debug - Unlock all Fighters and Venues" checkbox, and what it does.

    Part one clicks it in the launcher and expects `unlock_all = true` under [debug] in the settings
    file. Part two starts the game from a file that says so, with no environment variable, and expects
    the unlock routine to run; a file without it must leave the routine alone."""
    if sys.platform == "win32":
        return not_applicable("this check is for the SDL launcher (macOS, Linux); the Windows one is Direct3D 11")
    faults = []
    shot = os.path.join(REPO, "logs", "shots", "ui-unlockopt", "launcher.bmp")
    os.makedirs(os.path.dirname(shot), exist_ok=True)
    if os.path.exists(shot):
        os.remove(shot)
    settings = ui_settings_file("unlockopt")
    # Open Diagnostics, click unlock, capture and click Play. The harness turns the
    # unlock on for its runs, which would lock the checkbox, so the variable is left unset here.
    script = "800:ui:Advanced;1300:ui:Diagnostics;1900:ui:debug.unlock_all;2400:shot:%s;2800:ui:Play" % shot
    run = harness.run_game("ui-unlockopt", secs=120, preset=a.preset, scripted=False, quiet=True, until_file=shot,
                           env={"RECOMP_SETTINGS": settings, "RECOMP_LAUNCHER_SCRIPT": script,
                                "RECOMP_INPUT_IGNORE_FOCUS": "1", "RECOMP_UNLOCK_ALL": ""})
    faults += common_faults(run)
    with open(settings, encoding="utf-8") as f:
        ini = f.read()
    if not re.search(r"^\[debug\][^\[]*^unlock_all\s*=\s*true\s*$", ini, re.M | re.S):
        faults.append("the click on the Debug checkbox did not reach settings.ini")
    if "[UI] launcher closed: play" not in run.text:
        faults.append("Play did not close the launcher")
    if faults:
        return False, "; ".join(faults)
    # Part two: the saved choice reaches the game, and its absence leaves the game alone.
    for want, text in ((True, "[launcher]\nskip = true\n[debug]\nunlock_all = true\n"),
                       (False, "[launcher]\nskip = true\n")):
        path = ui_settings_file("unlockopt-" + ("on" if want else "off"), text)
        run = harness.run_route("boot", preset=a.preset, quiet=True,
                                env={"RECOMP_SETTINGS": path, "RECOMP_UNLOCK_ALL": ""})
        faults += common_faults(run)
        started = "[UNLOCK] RECOMP_UNLOCK_ALL:" in run.text
        ran = "[UNLOCK] sub_000158F0 run" in run.text
        if want and not (started and ran):
            faults.append("unlock_all = true in the file, but the unlock routine did not run")
        if not want and (started or ran):
            faults.append("unlock_all is off in the file, but the unlock routine ran")
    return verdict(faults, "the checkbox saves [debug] unlock_all; from the file alone the unlock runs, and without it does not")


def check_picture_options(a):
    """The launcher's Picture controls (issues #44 and #47) and what reaches the game.

    Clicks the Brightness slider at three quarters of its track and the Vignette checkbox, then
    Play. Expects [display] brightness above 100 and vignette = false in the settings file, and the
    game to start with them ([PICTURE] line). What they do to the picture is measured by
    docs/research/picture-options.md, not here: a fight is too long for this check."""
    if sys.platform == "win32":
        return not_applicable("this check is for the SDL launcher (macOS, Linux); the Windows one is Direct3D 11")
    faults = []
    shot = os.path.join(REPO, "logs", "shots", "ui-picture", "launcher.bmp")
    os.makedirs(os.path.dirname(shot), exist_ok=True)
    if os.path.exists(shot):
        os.remove(shot)
    settings = ui_settings_file("picture")
    script = "800:ui:Advanced;1400:ui:display.brightness;2000:ui:display.vignette;2500:shot:%s;2900:ui:Play" % shot
    run = harness.run_game("ui-picture", secs=60, preset=a.preset, scripted=False, quiet=True,
                           env={"RECOMP_SETTINGS": settings, "RECOMP_LAUNCHER_SCRIPT": script,
                                "RECOMP_INPUT_IGNORE_FOCUS": "1"})
    faults += common_faults(run)
    with open(settings, encoding="utf-8") as f:
        ini = f.read()
    display = re.search(r"^\[display\]([^\[]*)", ini, re.M)
    display = display.group(1) if display else ""
    brightness = re.search(r"^brightness\s*=\s*(\d+)\s*$", display, re.M)
    if not brightness or not 100 < int(brightness.group(1)) <= 200:
        faults.append("the Brightness slider did not reach settings.ini (%s)" % (brightness.group(0) if brightness else "missing"))
    if not re.search(r"^vignette\s*=\s*false\s*$", display, re.M):
        faults.append("the Vignette checkbox did not reach settings.ini")
    if "[UI] launcher closed: play" not in run.text:
        faults.append("Play did not close the launcher")
    started = re.search(r"^\[PICTURE\] brightness (\d+), vignette off", run.text, re.M)
    if not started or not brightness or started.group(1) != brightness.group(1):
        faults.append("the game did not start with the saved brightness and the vignette off")
    return verdict(faults, "Brightness %s and Vignette off saved by the launcher and applied when the game started"
                   % (brightness.group(1) if brightness else "?"))


def startup_skip_faults(text, enabled):
    """Require a positive first-screen observation; only inspect the startup interval."""
    startup = re.split(r'^\[FUNCCALL\] Control\.GetScreenInfo\(intMain/mainMenu', text,
                       maxsplit=1, flags=re.M | re.I)[0]
    screens = re.findall(r'^\[FUNCCALL\] Control\.GetScreenInfo\(([^)]*)\)', startup, re.M | re.I)
    want = 'intmain/prestart.swf' if enabled else 'intmain/legal1.swf'
    faults = []
    if not screens or screens[0].lower() != want:
        faults.append('first front-end screen %r, expected %s' % (screens[0] if screens else None, want))
    redirects = startup.count('[STARTUP] skip_intro: controller boot initialized;')
    if redirects != int(enabled):
        faults.append('startup redirect count %d, expected %d' % (redirects, int(enabled)))
    if enabled:
        if any(s.lower() in ('intmain/legal1.swf', 'intmain/bootload.swf') for s in screens):
            faults.append('legal or bootLoad requested with skip enabled')
        if re.search(r'\[PATH\][^\r\n]*[\\/]movies[\\/](eagames|thx|intro)\.mad', startup, re.I):
            faults.append('a startup movie opened with skip enabled')
    return faults


# Guest 0x00340320 is 1 once the full audio initialization (AudioInit(1)) has run. A normal
# boot makes it after the startup movies; skipping them must not lose it, or the Press Start
# and menu transition sound effects never play (v0.10.1 to v0.10.4).
AUDIO_MODE_PEEK = '0x340320'


def audio_init_faults(text):
    values = re.findall(r'\[PEEK\] 0x00340320 = ([0-9A-Fa-f]{8})', text)
    if not values:
        return ['no audio mode observation; the run ended before the first state sample']
    if int(values[-1], 16) != 1:
        return ['full audio initialization missing (0x00340320 = %s); menu sound effects would be silent' % values[-1]]
    return []


def normal_startup_faults(text):
    """The one complete normal boot in both quick/full sets must positively prove each stage."""
    faults = startup_skip_faults(text, False)
    stages = (
        ('legal', r'^\[FUNCCALL\] Control\.GetScreenInfo\(intMain/legal1\.swf\)'),
        ('bootLoad', r'^\[FUNCCALL\] Control\.GetScreenInfo\(intMain/bootLoad\.swf\)'),
        ('movie controller', r'^\[FUNCCALL\] Game\.StartMainMenu\(\s*0\s*\)'),
        ('EA movie', r'\[PATH\][^\r\n]*[\\/]movies[\\/]eagames\.mad'),
        ('THX movie', r'\[PATH\][^\r\n]*[\\/]movies[\\/]thx\.mad'),
        ('intro movie', r'\[PATH\][^\r\n]*[\\/]movies[\\/]intro\.mad'),
        ('Press Start', r'^\[FUNCCALL\] Control\.GetScreenInfo\(intMain/preStart\.swf\)'),
    )
    end = 0
    for label, pattern in stages:
        match = re.search(pattern, text[end:], re.M | re.I)
        if not match:
            faults.append('normal startup missing or out of order: ' + label)
            break
        end += match.end()
    return faults


def check_skip_intro(a):
    """Actual startup with defaults, INI alone, and both directions of the env override."""
    faults = []
    title = 'getscreeninfo(intmain/prestart'
    menu = 'getscreeninfo(intmain/mainmenu'
    for name, ini_value, override, enabled in (
            ('default', None, None, False), ('file', True, None, True),
            ('env-off', True, '0', False), ('env-on', False, '1', True),
            ('with-unlock', True, None, True)):
        combined = name == 'with-unlock'
        env = {'RECOMP_UNLOCK_ALL': '1' if combined else '0', 'RECOMP_SKIP_INTRO': ''}
        if os.environ.get('RECOMP_HEADLESS'):
            env['RECOMP_HEADLESS'] = os.environ['RECOMP_HEADLESS']
        if ini_value is not None:
            env['RECOMP_SETTINGS'] = ui_settings_file('skipintro-' + name,
                '[launcher]\nskip = true\n[debug]\nskip_intro = %s\n' % str(ini_value).lower())
        if override is not None:
            env['RECOMP_SKIP_INTRO'] = override
        if enabled:
            # Skipping the first flow load invalidates feflow.xml#2. Anchor to the
            # actual title request, keep its native animation/input delay, then Start.
            env['RECOMP_SCRIPT_ANCHOR'] = title
            env['RECOMP_PEEK'] = AUDIO_MODE_PEEK
            env['RECOMP_PAD_SCRIPT'] = ','.join(harness.title_presses())
            env['RECOMP_GAMMA'] = '0'
        run = harness.run_game('skipintro-' + name, preset=a.preset, scripted=False, quiet=True,
            env=env, inherit_recomp=False, secs=170,
            until='getscreeninfo(options/userid' if combined else (menu if enabled else 'getscreeninfo(intmain/legal1'),
            after=4 if enabled else 2, shots='@' + title + ',5' if enabled else '')
        faults += [name + ': ' + f for f in common_faults(run) + startup_skip_faults(run.text, enabled)]
        if enabled:
            if not harness.reached(run.summary, menu):
                faults.append(name + ': Start did not reach the main menu')
            faults += [name + ': ' + f for f in audio_init_faults(run.text)]
            if combined and '[UNLOCK] sub_000158F0 run' not in run.text:
                faults.append(name + ': startup skip prevented the unlock routine from running')
            shot = os.path.join(run.shots_dir, 'skipintro-' + name + '-5s.bmp')
            result = subprocess.run([sys.executable, os.path.join(HERE, 'frame-signature.py'),
                                     shot, '--check', os.path.join(REPO, 'tests/golden/m3-title-screen.json')],
                                    capture_output=True, text=True)
            if result.returncode:
                faults.append(name + ': skipped title does not match the existing golden: ' +
                              (result.stdout + result.stderr).strip())
    return verdict(faults, '5 startup cases: defaults, INI, both env overrides, combined unlock; title golden and Start to menu')


def start_venue_faults(text, expected):
    """Require early venue selection and the final value given to StartGame."""
    match = re.search(r'^\[START\] venue\(script\)=(-?\d+)\b', text, re.M)
    if not match:
        return ['no final StartGame venue observation']
    faults = []
    actual = int(match.group(1))
    if actual != expected:
        faults.append('StartGame venue %d, expected %d' % (actual, expected))
    early = re.search(r'^\[FE-INJECT\][^\r\n]*: Game\.ChooseVenue\(%d\)' % expected, text, re.M)
    if not early or early.start() > match.start():
        faults.append('no venue selection before StartGame')
    return faults


def check_fight(a, route="fight"):
    run = harness.run_route(route, preset=a.preset, quiet=True)
    faults = common_faults(run)
    expected = harness.ROUTES[route].get('expected_venue')
    if expected is not None:
        faults += start_venue_faults(run.text, expected)
    if not harness.reached(run.summary, "game.startgame("):
        faults.append("never reached the fight")
    p = harness.presents_after(run.text, "game.startgame(")
    floor = (FFA_FPS_FLOOR if route.startswith("ffa") else FPS_FLOOR).get(a.preset, 100)
    if p and median(p) < floor:
        faults.append("median %d presents per 2 s, floor %d" % (median(p), floor))
    if len(p) < 30:
        faults.append("only %d s of fight" % (2 * len(p)))
    return verdict(faults, "%d s of fight, median %d presents per 2 s, minimum %d"
                   % (2 * len(p), median(p), min(p) if p else 0))


def check_ffa(a):
    return check_fight(a, route="ffa")


def scenario_check(a, route, label, needed):
    """Run one scenario with the combat check required and summarise its report."""
    spec = importlib.util.spec_from_file_location("scenario_suite", os.path.join(HERE, "scenario_suite.py"))
    suite = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(suite)
    try:
        fixture = a.fixture or str(suite.default_fixture())
    except (OSError, ValueError) as ex:
        return False, "no fixture: %s" % ex
    folder = os.path.join(REPO, "logs", "scenarios", "regress-%s-%s" % (label, time.strftime("%Y%m%d-%H%M%S")))
    code = suite.main(["run", route, "--fixture", fixture, "--require-combat",
                       "--preset", a.preset, "--output", folder])
    try:
        with open(os.path.join(folder, "report.json"), encoding="utf-8") as f:
            checks = json.load(f)["assertions"]
    except (OSError, ValueError, KeyError) as ex:
        return False, "no scenario report: %s" % ex
    bad = [c["name"] for c in checks if c["status"] != "pass"]
    names = {c["name"] for c in checks}
    missing = sorted(set(needed) - names)
    combat = [c for c in checks if c["name"].startswith("combat.")]
    detail = "%d of %d assertions (%d combat)" % (len(checks) - len(bad), len(checks), len(combat))
    if bad:
        detail += "; failed: " + ", ".join(bad[:6])
    if missing:
        detail += "; not evaluated: " + ", ".join(missing)
    return code == 0 and not bad and not missing, detail + "  " + os.path.relpath(folder, REPO)


def check_combat(a):
    return scenario_check(a, "fight-result", "combat", ("combat.result", "combat.results_screen", "combat.damage"))


def check_versus(a):
    return scenario_check(a, "versus", "versus", ("combat.two_players",))


def check_two_matches(a):
    return scenario_check(a, "two-matches", "two-matches",
                          ("combat.result.match1", "combat.result.match2", "combat.eliminations.match2"))


def check_ffa_result(a):
    return scenario_check(a, "ffa-result", "ffa-result", ("combat.eliminations", "combat.result"))


def check_visual(a):
    spec = importlib.util.spec_from_file_location("scenario_suite", os.path.join(HERE, "scenario_suite.py"))
    suite = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(suite)
    manifest = os.path.join(harness.data_dir(), "test-baselines", "story-tour-v1", "manifest.json")
    if not os.path.isfile(manifest):
        # Baselines are the game's artwork and are made and approved on each machine.
        return False, "blocked: no approved baselines at %s (docs/09-testing-harness.md, Visual baselines)" % manifest
    try:
        fixture = a.fixture or str(suite.default_fixture())
    except (OSError, ValueError) as ex:
        return False, "no fixture: %s" % ex
    folder = os.path.join(REPO, "logs", "scenarios", "regress-visual-" + time.strftime("%Y%m%d-%H%M%S"))
    code = suite.main(["run", "story-tour", "--fixture", fixture, "--baselines", manifest,
                       "--preset", a.preset, "--output", folder])
    try:
        with open(os.path.join(folder, "report.json"), encoding="utf-8") as f:
            checks = [c for c in json.load(f)["assertions"] if c["name"].startswith("visual.")]
    except (OSError, ValueError, KeyError) as ex:
        return False, "no scenario report: %s" % ex
    bad = [c["name"] for c in checks if c["status"] != "pass"]
    detail = "%d of %d image comparisons" % (len(checks) - len(bad), len(checks))
    if bad:
        detail += "; failed: " + ", ".join(bad)
    return code == 0 and bool(checks) and not bad, detail + "  " + os.path.relpath(folder, REPO)


def check_replay(a):
    spec = importlib.util.spec_from_file_location("scenario_suite", os.path.join(HERE, "scenario_suite.py"))
    suite = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(suite)
    try:
        fixture = a.fixture or str(suite.default_fixture())
    except (OSError, ValueError) as ex:
        return False, "no fixture: %s" % ex
    stamp = time.strftime("%Y%m%d-%H%M%S")
    folder = os.path.join(REPO, "logs", "scenarios", "regress-replay-" + stamp)
    stream = os.path.join(REPO, "logs", "scenarios", "regress-replay-%s.stream" % stamp)
    code = suite.main(["run", "fight", "--fixture", fixture, "--no-audio", "--observe-combat", "--step-input", "default",
                       "--rng-seed", "20261006", "--no-unlock", "--preset", a.preset, "--stream-out", stream, "--output", folder])
    try:
        with open(stream, encoding="utf-8") as f:
            lines = [line for line in f.read().splitlines() if line]
        with open(os.path.join(REPO, "tests", "golden", "fight-stream.json"), encoding="utf-8") as f:
            golden = json.load(f)
    except (OSError, ValueError) as ex:
        return False, "no stream: %s" % ex
    actual = hashlib.sha256("\n".join(lines).encode()).hexdigest()
    same = actual == golden["sha256"] and len(lines) == golden["records"]
    detail = ("%d records over %d steps match the golden hash" % (len(lines), golden["max_step"]) if same else
              "the fight no longer plays as recorded (%d records, golden %d; hash %s...): a change altered the "
              "simulation, or the run was disturbed; `--only repeat` tells which"
              % (len(lines), golden["records"], actual[:12]))
    return code == 0 and same, detail + "  " + os.path.relpath(folder, REPO)


def check_repeat(a):
    spec = importlib.util.spec_from_file_location("scenario_suite", os.path.join(HERE, "scenario_suite.py"))
    suite = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(suite)
    try:
        fixture = a.fixture or str(suite.default_fixture())
    except (OSError, ValueError) as ex:
        return False, "no fixture: %s" % ex
    stamp = time.strftime("%Y%m%d-%H%M%S")
    first = os.path.join(REPO, "logs", "scenarios", "regress-repeat-a-" + stamp)
    second = os.path.join(REPO, "logs", "scenarios", "regress-repeat-b-" + stamp)
    stream = os.path.join(REPO, "logs", "scenarios", "regress-repeat-%s.stream" % stamp)
    common = ["run", "fight", "--fixture", fixture, "--no-audio", "--observe-combat", "--step-input", "default",
              "--rng-seed", "20261006", "--no-unlock", "--preset", a.preset]
    if suite.main(common + ["--stream-out", stream, "--output", first]) != 0:
        return False, "the recording run failed  " + os.path.relpath(first, REPO)
    code = suite.main(common + ["--stream-expect", stream, "--output", second])
    try:
        with open(os.path.join(second, "report.json"), encoding="utf-8") as f:
            check = next(c for c in json.load(f)["assertions"] if c["name"] == "determinism.stream")
    except (OSError, ValueError, KeyError, StopIteration) as ex:
        return False, "no comparison: %s" % ex
    metrics = check.get("metrics", {})
    golden_ok = True
    if check["status"] == "pass":
        detail = "%d records identical over %d steps" % (metrics.get("records", 0), metrics.get("max_step", 0))
        # The stream was the same on Release and Debug and on two fixtures, so its
        # hash is a reference for how the fight plays, not just for this machine.
        path = os.path.join(REPO, "tests", "golden", "fight-stream.json")
        with open(path, encoding="utf-8") as f:
            golden = json.load(f)
        if a.update_golden:
            golden.update(sha256=metrics["sha256"], records=metrics["records"],
                          recorded=time.strftime("%Y-%m-%d") + ", " + a.preset)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(json.dumps(golden, indent=1) + "\n")
            detail += "; golden updated"
        elif metrics.get("sha256") != golden["sha256"]:
            golden_ok = False
            detail += "; but the fight no longer plays as recorded in tests/golden/fight-stream.json" \
                      " (if the change is meant to alter play, rerun with --update-golden)"
        else:
            detail += ", matching the golden hash"
    else:
        detail = "%s at step %s (%s)" % (check["detail"], metrics.get("step"), ", ".join(metrics.get("differing_fields", [])[:4]))
    return code == 0 and check["status"] == "pass" and golden_ok, detail + "  " + os.path.relpath(second, REPO)


def check_intro(a):
    run = harness.run_route("intro", preset=a.preset, quiet=True)
    faults = common_faults(run)
    for anchor, what in (("game.startstorymode(", "Story start"), ("getscreeninfo(story/chardec", "the creator")):
        if not harness.reached(run.summary, anchor):
            faults.append("never reached " + what)
    p = harness.presents_after(run.text, "game.startstorymode(")
    floor = FPS_FLOOR.get(a.preset, 100)
    if p and median(p) < floor:
        faults.append("median %d presents per 2 s, floor %d" % (median(p), floor))
    return verdict(faults, "cutscenes to the creator, median %d presents per 2 s, %d captures"
                   % (median(p), len(run.shots())))


def check_crib(a):
    run = harness.run_route("crib", preset=a.preset, quiet=True)
    faults = common_faults(run)
    if not harness.reached(run.summary, "getscreeninfo(story/crib"):
        faults.append("never reached the crib (is there a Story profile in the save area?)")
    return verdict(faults, "crib reached with the first profile")


def check_gym(a):
    run = harness.run_route("gym", preset=a.preset, quiet=True)
    faults = common_faults(run)
    if not harness.reached(run.summary, "getscreeninfo(story/gym"):
        faults.append("never reached Stapleton Athletics")
    tail = run.text[run.text.lower().find("getscreeninfo(story/gym"):]
    movies = len(re.findall(r"\[PATH\] .*blazin_\d+\.mad", tail))
    if not movies:
        faults.append("no move preview movie was requested")
    return verdict(faults, "Learn Moves, preview movie requested")


def check_soak(a):
    ok, hung, failed = harness.soak(a.soak, a.preset)
    return (ok == a.soak and hung == 0 and failed == 0), "%d of %d boots reached the main menu" % (ok, a.soak)


CHECKS = {"unit": check_unit, "m2": check_m2, "m3": check_m3, "m4a": check_m4a, "launcher": check_launcher,
          "overlay": check_overlay, "unlockopt": check_unlock_option, "picture": check_picture_options, "skipintro": check_skip_intro, "fight": check_fight, "ffa": check_ffa,
          "fight-terrordome": lambda a: check_fight(a, 'fight-terrordome'),
          "ffa-terrordome": lambda a: check_fight(a, 'ffa-terrordome'),
          "combat": check_combat, "versus": check_versus, "ffa-result": check_ffa_result, "two-matches": check_two_matches, "replay": check_replay, "visual": check_visual, "repeat": check_repeat, "intro": check_intro, "crib": check_crib, "gym": check_gym, "soak": check_soak}


def status_word(ok):
    return "SKIP" if ok is None else "ok" if ok else "FAIL"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--preset", default=os.environ.get("RECOMP_PRESET", harness.RELEASE_PRESET))
    ap.add_argument("--quick", action="store_true", help="unit, the golden frames and a fight")
    ap.add_argument("--only", help="comma-separated checks: " + ",".join(ORDER))
    ap.add_argument("--soak", type=int, default=3, help="boots in the soak check (default 3)")
    ap.add_argument("--fixture", help="save fixture for the combat check (default: a fresh copy of the saved profiles)")
    ap.add_argument("--update-golden", action="store_true",
                    help="repeat check: record the new state-stream hash instead of comparing with it")
    ap.add_argument("--shared-story", action="store_true", help="evaluate crib and gym from one story-tour launch")
    a = ap.parse_args(argv)

    names = QUICK if a.quick else DEFAULT
    if a.only:
        names = [n for n in ORDER if n in set(a.only.split(","))]
        unknown = set(a.only.split(",")) - set(ORDER)
        if unknown:
            ap.error("unknown check: " + ", ".join(sorted(unknown)))

    rows = []
    shared_story = None
    t_all = time.time()
    for n in names:
        t0 = time.time()
        log_before = harness.LAST_LOG
        try:
            if a.shared_story and n in ('crib', 'gym'):
                if shared_story is None:
                    shared_story = harness.run_route('story-tour', preset=a.preset, quiet=True)
                faults = common_faults(shared_story)
                anchor = 'getscreeninfo(story/' + n
                if not harness.reached(shared_story.summary, anchor):
                    faults.append('never reached ' + n)
                if n == 'gym' and not re.search(r'\[PATH\] .*blazin_\d+\.mad', shared_story.text):
                    faults.append('no move preview movie requested')
                ok, detail = verdict(faults, n + ' reached in shared Story launch')
            else:
                ok, detail = CHECKS[n](a)
        except SystemExit as ex:                 # not built, no dump: say so and stop
            ok, detail = False, str(ex)
        rows.append((n, ok, detail, time.time() - t0))
        print("%-6s %-4s %4d s  %s" % (n, status_word(ok), rows[-1][3], detail), flush=True)
        if ok is False and harness.LAST_LOG and harness.LAST_LOG != log_before:
            print("       " + harness.triage_hint(harness.LAST_LOG), flush=True)
    harness.kill_stray()

    failed = [r for r in rows if r[1] is not None and not r[1]]
    skipped = [r for r in rows if r[1] is None]
    ran = len(rows) - len(skipped)
    lines = ["regress %s  preset %s" % (time.strftime("%Y-%m-%d %H:%M"), a.preset)]
    lines += ["%-6s %-4s %4d s  %s" % (n, status_word(ok), dt, d) for n, ok, d, dt in rows]
    lines.append("%d of %d passed%s in %d min" % (ran - len(failed), ran,
                 (", %d skipped (%s)" % (len(skipped), ", ".join(r[0] for r in skipped))) if skipped else "",
                 (time.time() - t_all) / 60))
    os.makedirs(os.path.join(REPO, "logs"), exist_ok=True)
    out = os.path.join(REPO, "logs", "regress-" + time.strftime("%Y%m%d-%H%M%S") + ".txt")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    report_dir = os.path.splitext(out)[0]
    evidence.write_report(report_dir, {'schema': 1, 'name': 'regression',
                          'seconds': time.time() - t_all,
                          'identity': {'preset': a.preset, 'shared_story': a.shared_story,
                                       'checks': names, 'text_log': out},
                          'assertions': [dict(evidence.assertion(n, ok, d), seconds=dt)
                                         for n, ok, d, dt in rows]})
    print(lines[-1] + "  (" + os.path.relpath(out, REPO) + ")")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
