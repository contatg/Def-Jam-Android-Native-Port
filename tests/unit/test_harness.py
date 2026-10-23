"""The harness's pure parts: pad scripts from stages, and what a log says about a run.

No game is run here; the routes themselves are exercised by scripts/regress.py.
"""
import importlib.util
import json
import os
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), os.path.join(ROOT, "scripts", name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


harness = load("harness")
worklog = load("worklog-add")
regress = load("regress")
pipeline = load("pipeline-state")


def test_presses_expand():
    d = harness.DIRECTION_HOLD      # directions are held for less off Windows
    assert harness.expand_presses("right@5;a@8") == ["right:5:%g" % (5 + d), "a:8:8.3"]
    assert harness.expand_presses("a@8+4x3") == ["a:8:8.3", "a:12:12.3", "a:16:16.3"]
    assert harness.expand_presses("down@11+0.6x2") == ["down:11:%g" % (11 + d), "down:11.6:%g" % (11.6 + d)]


def test_guarded_data_root_matches_child_cwd(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    assert harness.guarded_data_root('runtime-data') == os.path.join(harness.REPO, 'runtime-data')
    assert harness.guarded_data_root(str(tmp_path)) == str(tmp_path)
    for value in ('', '   ', None):
        with pytest.raises(ValueError, match='nonempty save root'):
            harness.guarded_data_root(value)


def test_noncode_icall_is_a_shared_failure():
    run = harness.Run('synthetic', 'unused', '')
    run.save_ok = True
    run.summary = harness.summarise('[ICALL] target 0x00000000 is not code -- skipped 10 time(s)')
    assert harness.run_faults(run) == ['1 indirect calls to non-code']


def test_startup_skip_requires_first_screen_and_rejects_startup_movies():
    redirect = '[STARTUP] skip_intro: controller boot initialized; legal1 -> preStart\n'
    title = '[FUNCCALL] Control.GetScreenInfo(intMain/preStart.swf)\n'
    legal = '[FUNCCALL] Control.GetScreenInfo(intMain/legal1.swf)\n'
    movie = r'[PATH] \Device\CdRom0\movies\intro.mad' + '\n'
    menu = '[FUNCCALL] Control.GetScreenInfo(intMain/mainMenu.swf)\n'
    assert regress.startup_skip_faults(redirect + title, True) == []
    assert regress.startup_skip_faults(legal + movie + title, False) == []
    assert regress.startup_skip_faults(redirect + title + menu + movie, True) == []  # later attract/Story allowed
    for text in ('', 'unrelated preStart.swf\n', title, redirect + legal + title,
                 redirect + title + movie, redirect + redirect + title):
        assert regress.startup_skip_faults(text, True)
    assert regress.startup_skip_faults(redirect + title, False)


def test_normal_startup_requires_each_movie_in_order():
    stages = [
        '[FUNCCALL] Control.GetScreenInfo(intMain/legal1.swf)',
        '[FUNCCALL] Control.GetScreenInfo(intMain/bootLoad.swf)',
        '[FUNCCALL] Game.StartMainMenu( 0 )',
        r'  [PATH] \Device\CdRom0\movies\eagames.mad',
        r'  [PATH] \Device\CdRom0\movies\thx.mad',
        r'  [PATH] \Device\CdRom0\movies\intro.mad',
        '[FUNCCALL] Control.GetScreenInfo(intMain/preStart.swf)',
    ]
    assert regress.normal_startup_faults('\n'.join(stages)) == []
    for i in range(len(stages)):
        assert regress.normal_startup_faults('\n'.join(stages[:i] + stages[i + 1:]))
    stages[3], stages[4] = stages[4], stages[3]
    assert regress.normal_startup_faults('\n'.join(stages))


def test_harness_startup_defaults_allow_normal_and_file_only_runs():
    assert harness.startup_default({})['RECOMP_SKIP_INTRO'] == '1'
    assert harness.startup_default({'RECOMP_SKIP_INTRO': '0'})['RECOMP_SKIP_INTRO'] == '0'
    assert 'RECOMP_SKIP_INTRO' not in harness.startup_default({'RECOMP_SKIP_INTRO': ''})
    assert harness.TITLE_ANCHOR == 'getscreeninfo(intmain/prestart'


def test_pad_script_orders_stages_after_the_title_presses():
    s = harness.build_pad_script(["getscreeninfo(intmain/mainmenu=a@6", "game.startgame(=x@2"], "@tail,b:1:2")
    parts = s.split(",")
    assert parts[0] == "start:14:14.3"
    i = parts.index("@getscreeninfo(intmain/mainmenu")
    j = parts.index("@game.startgame(")
    assert parts[i + 1] == "a:6:6.3" and parts[j + 1] == "x:2:2.3" and i < j
    assert parts[-2:] == ["@tail", "b:1:2"]


def test_every_route_builds():
    for name, r in harness.ROUTES.items():
        script = harness.build_pad_script(r["stages"], r.get("tail", ""))
        assert script.count("@") >= len(r["stages"]), name
        assert r["secs"] > r.get("after", 0), name


def test_named_stress_routes_select_the_catalog_venue_independently_of_map_order():
    with open(os.path.join(ROOT, 'config/scenario-catalog.json')) as f:
        venue = json.load(f)['venues']['terrordome']['index']
    for name in ('fight-terrordome', 'ffa-terrordome'):
        route = harness.ROUTES[name]
        assert route['expected_venue'] == venue
        assert route['env']['RECOMP_FE_INJECT'] == 'game.choosevenue#1=>Game.ChooseVenue(%d)' % venue
        assert route['env']['RECOMP_START_LOG'] == '1'
        assert 'RECOMP_START_VENUE' not in route['env']


def test_final_venue_requires_positive_startgame_evidence():
    early = '[FE-INJECT] after "game.choosevenue" #1: Game.ChooseVenue(6)\n'
    start = '[START] venue(script)=6 fighters(gd+1aaa step 5): 4 9\n'
    assert regress.start_venue_faults(early + start, 6) == []
    for text in ('', '[FUNCCALL] Game.ChooseVenue(6)', early + '[START] venue(script)=14 fighters: 4 9',
                 start, start + early):
        assert regress.start_venue_faults(text, 6)


LOG = """\
  [D3D] 2.0s: 60 present (30.0 fps), 1 begin
[FUNCCALL] Control.GetScreenInfo(intMain/mainMenu.swf)
  [D3D] 2.0s: 120 present (60.0 fps), 4 begin
[FUNCCALL] Game.StartGame()
  [D3D] 2.0s: 118 present (59.0 fps), 4 begin
  [D3D] 2.0s: 120 present (60.0 fps), 4 begin
  [GPU] batch over 32768 indices truncated (prim 6, #1)
[CRASH] something
"""


def test_summarise():
    s = harness.summarise(LOG)
    assert s["presents"] == [60, 120, 118, 120]
    assert s["crashes"] == 1 and s["truncated"] == 1 and s["watchdogs"] == 0
    assert harness.reached(s, "getscreeninfo(intmain/mainmenu")
    assert harness.reached(s, "game.startgame(")
    assert not harness.reached(s, "getscreeninfo(story/crib")


def test_baseline_noops_do_not_hide_new_untranslated_instructions():
    text = '[UNIMPL-KNOWN] untranslated instruction REACHED: `cli` at 0x00100000\n'
    text += '[UNIMPL] untranslated instruction REACHED: `unknown` at 0x00100004\n'
    summary = harness.summarise(text)
    assert summary['baseline_noops'] == 1 and summary['unimplemented'] == 1


def test_presents_after_an_anchor():
    assert harness.presents_after(LOG, "game.startgame(") == [118, 120]
    assert harness.presents_after(LOG, "never.called(") == []


def test_log_tail_reads_incrementally(tmp_path):
    p = tmp_path / "run.log.err"
    p.write_bytes(b"  [D3D] 2.0s: 120 present\n[FUNCCALL] Game.Start")
    t = harness.LogTail(str(p))
    t.poll()
    assert t.presents == 1 and not t.saw("game.startgame(")     # the last line is incomplete
    with open(p, "ab") as f:
        f.write(b"Game()\n  [D3D] 2.0s: 119 present\n")
    t.poll()
    assert t.presents == 2 and t.saw("game.startgame(")


def test_trees_equal(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    for d in (a, b):
        (d / "sub").mkdir(parents=True)
        (d / "sub" / "f").write_bytes(b"same")
    assert harness.trees_equal(str(a), str(b))
    (b / "sub" / "f").write_bytes(b"diff")
    assert not harness.trees_equal(str(a), str(b))
    (b / "sub" / "f").write_bytes(b"same")
    (b / "extra").write_bytes(b"")
    assert not harness.trees_equal(str(a), str(b))


def test_save_guard_restores_userdata_caches_and_original_absence(tmp_path, monkeypatch):
    monkeypatch.setattr(harness, 'kill_stray', lambda: None)
    save = tmp_path / 'save'
    for name in ('UserData/profile', 'CacheData/movie'):
        path = save / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'original')
    with harness.SaveGuard(str(tmp_path)) as guard:
        (save / 'UserData/profile').write_bytes(b'changed!')
        (save / 'CacheData/movie').unlink()
        (save / 'new-file').write_bytes(b'new')
    assert guard.restored_equal
    assert (save / 'UserData/profile').read_bytes() == b'original'
    assert (save / 'CacheData/movie').read_bytes() == b'original'
    assert not (save / 'new-file').exists()
    absent = tmp_path / 'absent'
    with harness.SaveGuard(str(absent)) as guard:
        (absent / 'save').mkdir()
        (absent / 'save/new').write_bytes(b'created')
    assert guard.restored_equal and not (absent / 'save').exists()


def test_runs_unlock_everything_unless_told_otherwise():
    assert harness.unlock_default({})["RECOMP_UNLOCK_ALL"] == "1"
    assert harness.unlock_default({"RECOMP_UNLOCK_ALL": "0"})["RECOMP_UNLOCK_ALL"] == "0"
    assert "RECOMP_UNLOCK_ALL" not in harness.unlock_default({"RECOMP_UNLOCK_ALL": ""})


def test_save_guard_can_keep_what_the_run_wrote(tmp_path, monkeypatch):
    monkeypatch.setattr(harness, 'kill_stray', lambda: None)
    keep = tmp_path / 'kept'
    monkeypatch.setenv('DEFJAM_HARNESS_SNAPSHOT_SAVE', str(keep))
    save = tmp_path / 'data' / 'save'
    (save / 'UserData').mkdir(parents=True)
    (save / 'UserData/profile').write_bytes(b'original')
    with harness.SaveGuard(str(tmp_path / 'data')) as guard:
        (save / 'UserData/profile').write_bytes(b'unlocked')
    assert guard.restored_equal
    assert (save / 'UserData/profile').read_bytes() == b'original'
    assert (keep / 'UserData/profile').read_bytes() == b'unlocked'


@pytest.mark.parametrize('name', ['__pycache__', 'CVS', 'same-name'])
def test_save_comparison_does_not_ignore_names_or_shape(tmp_path, name):
    a, b = tmp_path / 'a', tmp_path / 'b'
    a.mkdir(); b.mkdir()
    (a / name).write_bytes(b'original')
    assert not harness.trees_equal(str(a), str(b))
    (b / name).mkdir()
    assert not harness.trees_equal(str(a), str(b))


def test_backup_status_never_overwrites_save_content_or_prunes_failed_restore(tmp_path, monkeypatch):
    monkeypatch.setattr(harness, 'kill_stray', lambda: None)
    monkeypatch.setattr(harness.time, 'sleep', lambda _: None)
    save = tmp_path / 'save'
    save.mkdir()
    (save / '.restore-ok').write_bytes(b'player file with an unfortunate name')
    with harness.SaveGuard(str(tmp_path)) as good:
        (save / '.restore-ok').write_bytes(b'changed')
    assert good.restored_equal
    assert (save / '.restore-ok').read_bytes() == b'player file with an unfortunate name'
    failed = harness.SaveGuard(str(tmp_path))
    failed.__enter__()
    copytree = harness.shutil.copytree
    def fail_restore(src, dst, *args, **kwargs):
        if str(dst) == str(save):
            raise OSError('synthetic restore failure')
        return copytree(src, dst, *args, **kwargs)
    monkeypatch.setattr(harness.shutil, 'copytree', fail_restore)
    failed.__exit__(None, None, None)
    assert failed.restored_equal is False
    assert (harness.Path(failed.copy) / '.restore-ok').read_bytes() == b'player file with an unfortunate name'
    monkeypatch.setattr(harness.shutil, 'copytree', copytree)
    copytree(failed.copy, save)
    for _ in range(4):
        with harness.SaveGuard(str(tmp_path)):
            pass
    assert harness.Path(failed.copy).is_dir()


@pytest.mark.parametrize('fault', ['crashes', 'watchdogs', 'debug_layer', 'truncated', 'icall_failures', 'unimplemented'])
def test_every_fault_fails_soak_and_regression(fault, monkeypatch):
    run = harness.Run('boot', 'synthetic.log', '')
    run.text = '[FUNCCALL] Control.GetScreenInfo(intMain/mainMenu.swf)\n'
    run.summary = harness.summarise(run.text)
    run.summary[fault] = 1
    run.save_ok = True
    monkeypatch.setattr(harness, 'run_route', lambda *a, **kw: run)
    assert harness.run_faults(run)
    assert harness.soak(1, 'synthetic') == (0, 0, 1)
    assert regress.common_faults(run)


def test_missing_story_route_is_a_failure(monkeypatch):
    run = regress.harness.Run('crib', 'synthetic.log', '')
    run.save_ok = True
    monkeypatch.setattr(regress.harness, 'run_route', lambda *a, **kw: run)
    args = type('Args', (), {'preset': 'synthetic'})()
    assert not regress.check_crib(args)[0]
    assert not regress.check_gym(args)[0]


def test_clean_log_without_route_target_still_fails(monkeypatch):
    run = harness.Run('unlock', 'synthetic.log', '')
    run.save_ok = True
    monkeypatch.setattr(harness, 'run_game', lambda *a, **kw: run)
    assert harness.run_faults(harness.run_route('unlock')) == ['never reached getscreeninfo(battle/unlockchar']


def pipeline_fixture(tmp_path):
    repo, toolkit = tmp_path / 'repo', tmp_path / 'toolkit'
    for root, relative, content in (
            (repo, 'config/seed_functions.json', '[]'), (repo, 'src/recomp_manual.c', '// manual'),
            (repo, 'CMakeLists.txt', '# cmake'), (repo, 'CMakePresets.json', '{}'),
            (toolkit, 'game_files/default.xbe', 'synthetic binary'),
            (toolkit, 'tools/recomp/lifter.py', '# synthetic tool'),
            (toolkit, 'src/kernel/runtime.c', '// runtime'), (toolkit, 'CMakeLists.txt', '# cmake')):
        p = root / relative
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding='utf-8')
    for name in pipeline.ANALYSIS_FILES:
        p = toolkit / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('{}', encoding='utf-8')
    pipeline.write_state(toolkit / 'game_files/defjam-analysis-state.json',
                         {'inputs': pipeline.analysis_inputs(repo, toolkit),
                          'outputs': pipeline.analysis_outputs(toolkit)})
    gen = repo / 'src/recomp/gen'
    gen.mkdir(parents=True)
    (gen / 'recomp_0000.c').write_text('// old generated set', encoding='utf-8')
    (gen / 'recomp_dispatch.c').write_text('// dispatcher', encoding='utf-8')
    pipeline.write_state(gen / 'lift-state.json', {'inputs': pipeline.lift_inputs(repo, toolkit),
                         'outputs': pipeline.hashes(gen, gen.glob('*.c'))})
    return repo, toolkit


def test_failed_lift_preserves_previous_set(tmp_path, monkeypatch):
    repo, toolkit = pipeline_fixture(tmp_path)
    def fail(*args, **kwargs):
        raise pipeline.subprocess.CalledProcessError(42, args[0])
    monkeypatch.setattr(pipeline.subprocess, 'run', fail)
    with pytest.raises(pipeline.subprocess.CalledProcessError):
        pipeline.lift(repo, toolkit, 1000, [])
    assert (repo / 'src/recomp/gen/recomp_0000.c').read_text() == '// old generated set'
    pipeline.verify_lift(repo, toolkit)


@pytest.mark.parametrize('option', ['--gen-dir', '--gen-dir=live', '--gen=live',
                                   '--exclude-manual=other', '--functions=other', '-o'])
def test_lift_refuses_destination_and_input_overrides_before_running(tmp_path, monkeypatch, option):
    repo, toolkit = pipeline_fixture(tmp_path)
    def unexpected(*args, **kwargs):
        pytest.fail('translator should not start')
    monkeypatch.setattr(pipeline.subprocess, 'run', unexpected)
    with pytest.raises(ValueError, match='controlled by the pipeline'):
        pipeline.lift(repo, toolkit, 1000, [option])
    assert (repo / 'src/recomp/gen/recomp_0000.c').read_text() == '// old generated set'


@pytest.mark.parametrize('original', [True, False])
def test_launch_failure_restores_full_save_root(tmp_path, monkeypatch, original):
    root = tmp_path / 'runtime-data'
    save = root / 'save'
    if original:
        for name in ('UserData/profile', 'CacheData/cache'):
            path = save / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'original')
    monkeypatch.setattr(harness, 'kill_stray', lambda: None)
    monkeypatch.setattr(harness.pipeline_state, 'verify_build', lambda *a: None)
    repo = tmp_path / 'repo'
    monkeypatch.setattr(harness, 'REPO', str(repo))
    for name in ('build/%s/%s' % (harness.RELEASE_PRESET, harness.EXE_NAME), 'game/default.xbe'):
        path = repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'synthetic fixture')
    def failed_launch(*args, **kwargs):
        path = save / 'CacheData/cache'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'changed')
        raise OSError('synthetic launch failure')
    monkeypatch.setattr(harness.subprocess, 'Popen', failed_launch)
    with pytest.raises(OSError, match='synthetic launch failure'):
        harness.run_game('synthetic-launch-failure', env={'DEFJAM_DATA': str(root)})
    if original:
        assert (save / 'UserData/profile').read_bytes() == b'original'
        assert (save / 'CacheData/cache').read_bytes() == b'original'
    else:
        assert not save.exists()


def test_freshness_rejects_changed_analysis_generated_code_and_runtime(tmp_path):
    repo, toolkit = pipeline_fixture(tmp_path)
    folder = repo / 'build/synthetic'
    folder.mkdir(parents=True)
    exe = folder / pipeline.EXE_NAME
    exe.write_bytes(b'synthetic executable')
    (folder / 'CMakeCache.txt').write_text('DEFJAM_BUILD_GAME:BOOL=ON\n')
    pipeline.write_state(folder / 'build-state.json', {'toolkit': str(toolkit),
                         'inputs': pipeline.build_inputs(repo, toolkit), 'exe': pipeline.digest(exe)})
    pipeline.verify_build(repo, 'synthetic')
    runtime = toolkit / 'src/kernel/runtime.c'
    runtime.write_text('// changed runtime')
    with pytest.raises(ValueError, match='Build .* stale'):
        pipeline.verify_build(repo, 'synthetic')
    generated = repo / 'src/recomp/gen/recomp_0000.c'
    generated.write_text('// changed generated code')
    with pytest.raises(ValueError, match='Generated code is stale'):
        pipeline.verify_lift(repo, toolkit)
    (repo / 'config/seed_functions.json').write_text('[123]')
    with pytest.raises(ValueError, match='Analysis is stale'):
        pipeline.verify_analysis(repo, toolkit)


def test_stage_refuses_inputs_changed_after_work_started(tmp_path):
    state = tmp_path / 'analysis-state.json'
    pipeline.begin_stage(state, {'seeds': 'before'})
    with pytest.raises(ValueError, match='changed during the stage'):
        pipeline.finish_stage_inputs(state, {'seeds': 'after'})
    assert not state.exists()
    assert pipeline.finish_stage_inputs(state, {'seeds': 'before'}) == {'seeds': 'before'}


def test_build_refuses_runtime_only_configuration(tmp_path):
    (tmp_path / 'CMakeCache.txt').write_text('DEFJAM_BUILD_GAME:BOOL=OFF\n')
    with pytest.raises(ValueError, match='disabled in CMake'):
        pipeline.verify_game_configuration(tmp_path)
    with pytest.raises(ValueError, match='Runtime-only presets'):
        pipeline.verify_build(tmp_path, 'ci-runtime-only')


def test_backup_copy_failure_keeps_original_and_never_launches(tmp_path, monkeypatch):
    save = tmp_path / 'save'
    save.mkdir()
    (save / 'profile').write_bytes(b'original')
    def failed_copy(*args, **kwargs):
        raise OSError('synthetic backup failure')
    monkeypatch.setattr(harness.shutil, 'copytree', failed_copy)
    guard = harness.SaveGuard(str(tmp_path))
    with pytest.raises(OSError, match='synthetic backup failure'):
        guard.__enter__()
    assert (save / 'profile').read_bytes() == b'original'
    assert guard.restored_equal is None


PROGRESS = """\
# Progress

## 6. Work log

> Recent entries only. Everything before 2026-09-01 10:00 is in `docs/worklog/` (one file a month).

- 2026-09-30 10:00 — first
  second line
- 2026-10-01 11:00 — second
- 2026-10-02 12:00 — third

## 7. Hand-off
"""


def test_worklog_moves_the_oldest_to_their_own_months():
    out, moved = worklog.add_and_trim(PROGRESS, "- 2026-10-03 09:00 — fourth", 2, "unused")
    assert sorted(moved) == ["2026-09", "2026-10"]
    assert moved["2026-09"] == ["- 2026-09-30 10:00 — first\n  second line"]
    assert moved["2026-10"] == ["- 2026-10-01 11:00 — second"]
    assert "Everything before 2026-10-02 12:00 is in" in out
    assert out.index("third") < out.index("fourth") < out.index("## 7.")
    assert "first" not in out


def test_worklog_leaves_a_short_log_alone():
    out, moved = worklog.add_and_trim(PROGRESS, "", 20, "unused")
    assert out == PROGRESS and not moved


def _bmp32(path, width, height, pixels):
    """A top-down 32-bit BMP of (r, g, b) pixels, as the toolkit's RECOMP_WINDOW_SHOT writes it."""
    import struct
    data = b"".join(struct.pack("<BBBB", b, g, r, 255) for r, g, b in pixels)
    head = b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54)
    head += struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, len(data), 2835, 2835, 0, 0)
    path.write_bytes(head + data)


def test_ui_check_counts_the_pixels_it_is_looking_for(tmp_path):
    tab, play, other = regress.UI_TAB, regress.UI_PLAY, (10, 20, 30)
    near_tab = (tab[0] + 2, tab[1] - 2, tab[2])
    pixels = [tab] * 5 + [near_tab] * 2 + [play] * 3 + [other] * 6
    path = tmp_path / "frame.bmp"
    _bmp32(path, 4, 4, pixels)
    assert regress.bmp_color_count(str(path), [tab, play]) == (4, 4, [7, 3])
    assert regress.bmp_color_count(str(path), [tab], tolerance=0) == (4, 4, [5])


def test_ui_check_refuses_a_picture_it_cannot_read(tmp_path):
    (tmp_path / "x.bmp").write_bytes(b"not a bitmap at all, nothing like one")
    with pytest.raises(ValueError):
        regress.bmp_color_count(str(tmp_path / "x.bmp"), [(0, 0, 0)])


def test_the_ui_checks_are_not_in_the_quick_run_and_are_selectable():
    assert "launcher" in regress.ORDER and "overlay" in regress.ORDER
    assert "launcher" not in regress.QUICK and "overlay" not in regress.QUICK
    assert set(["launcher", "overlay"]) <= set(regress.CHECKS)


def test_a_check_that_does_not_apply_is_skipped_never_passed(monkeypatch, tmp_path):
    """The launcher and overlay checks cannot run on Windows (their Direct3D 11 versions are not
    these): they say so with None, which the table and the report show as SKIP, not as a pass."""
    monkeypatch.setattr(regress.sys, "platform", "win32")
    for check in (regress.check_launcher, regress.check_overlay):
        ok, detail = check(None)
        assert ok is None and detail.startswith("not run: ")
    assert regress.status_word(None) == "SKIP"
    assert regress.status_word(True) == "ok" and regress.status_word(False) == "FAIL"

    evidence = regress.evidence
    skipped = evidence.assertion("launcher", None, "not run: Windows")
    passed = evidence.assertion("m2", True, "matches")
    assert skipped["status"] == "skip" and passed["status"] == "pass"
    ok = evidence.write_report(str(tmp_path), {"schema": 1, "name": "regression", "seconds": 1,
                                               "assertions": [passed, skipped]})
    assert ok is False                                   # a skip does not make the run "all passed"
    junit = (tmp_path / "junit.xml").read_text(encoding="utf-8")
    assert 'skipped="1"' in junit and 'failures="0"' in junit and "<skipped" in junit


def test_a_failed_run_points_at_the_triage_digest(capsys):
    run = harness.Run('synthetic', os.path.join(harness.REPO, 'logs', 'run-20261014-101500.log.err'), '')
    run.save_ok = True
    run.text = LOG
    run.summary = harness.summarise(LOG)
    harness.print_run(run)
    out = capsys.readouterr().out
    assert 'FAIL' in out
    assert 'triage   python scripts/triage.py logs/run-20261014-101500.log.err' in out
