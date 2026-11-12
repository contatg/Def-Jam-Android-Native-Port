"""src/hooks/recorder.c built on its own and driven through a synthetic run, then
scripts/bug-bundle.py on what it wrote.

The driver presents 300 frames with a planted fault of every kind the analysis looks
for -- a long frame, one picture unlike its neighbours, a draw-count dip, an audio
gap and a click, a screen change -- marks the moment, and the test checks that the
session and the bundle hold what docs/playtest-recorder.md says and that each fault
is found at the right place. A second run checks the crash path."""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recorder.h"
#if defined(_WIN32)
#include <windows.h>
static void nap(int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void nap(int ms) { usleep(ms * 1000); }
#endif

static unsigned char img[96 * 64 * 4];

/* "async": pictures are requested by tag and handed over two frames later with their
 * bytes in B, G, R, A order, as an asynchronous read-back delivers them. */
static unsigned long long late_tag[3];
static unsigned char late_img[3][96 * 64 * 4];
static int late_age[3];

static void deliver_late(int all)
{
    int k;
    for (k = 0; k < 3; k++) {
        if (late_tag[k] && (all || ++late_age[k] >= 2)) {
            recorder_picture_tagged(late_tag[k], late_img[k], 96 * 4, 96, 64, 1, NULL);
            late_tag[k] = 0;
        }
    }
}

int main(int argc, char **argv)
{
    char log[1024], pcm[1024];
    FILE *lf, *af;
    RecorderConfig c;
    RecorderFrameIn in;
    int f, pictures = 0, crash = argc > 2 && !strcmp(argv[2], "crash");
    int late = argc > 2 && !strcmp(argv[2], "async");
    long long sample = 0;

    snprintf(log, sizeof log, "%s/fake.log", argv[1]);
    snprintf(pcm, sizeof pcm, "%s/fake.pcm", argv[1]);
    lf = fopen(log, "w");
    af = fopen(pcm, "wb");
    memset(&c, 0, sizeof c);
    c.enabled = 1;
    c.root = argv[1];
    c.video_fps = 30;
    c.ring_seconds = 5;
    c.thumb_height = 48;
    c.after_seconds = 0;
    c.hitch_ms = 50;
    c.keep_sessions = 3;
    c.log_path = log;
    c.audio_path = pcm;
    c.build = "test-build";
    c.settings_json = "{\"display.aspect\":\"4:3\",\"display.render_scale\":\"2\"}";
    c.mark_vk = 0x78;
    if (!recorder_init(&c)) return 2;
    memset(&in, 0, sizeof in);
    in.step = -1;
    for (f = 0; f < 300; f++) {
        int i, want;
        nap(f == 200 ? 90 : 3);                     /* the game's work; frame 200 is a hitch */
        recorder_frame_begin();
        if (late) deliver_late(0);
        want = recorder_picture_wanted();
        if (want) {
            pictures++;
            for (i = 0; i < 96 * 64; i++) {
                int x = i % 96, y = i / 96;
                if (pictures == 20) {               /* one picture of a different scene */
                    img[i * 4 + 0] = 250; img[i * 4 + 1] = 0; img[i * 4 + 2] = 250;
                } else {
                    img[i * 4 + 0] = (unsigned char)(x * 2);
                    img[i * 4 + 1] = (unsigned char)(y * 3);
                    img[i * 4 + 2] = 90;
                }
                img[i * 4 + 3] = 255;
            }
            if (late) {
                unsigned long long tag = recorder_picture_request();
                int k, slot = -1;
                for (k = 0; k < 3; k++) if (!late_tag[k]) slot = k;
                if (tag && slot >= 0) {
                    for (i = 0; i < 96 * 64; i++) {
                        late_img[slot][i * 4 + 0] = img[i * 4 + 2];
                        late_img[slot][i * 4 + 1] = img[i * 4 + 1];
                        late_img[slot][i * 4 + 2] = img[i * 4 + 0];
                        late_img[slot][i * 4 + 3] = 255;
                    }
                    late_tag[slot] = tag;
                    late_age[slot] = 0;
                }
            } else {
                recorder_picture(img, 96 * 4, 96, 64, NULL);
            }
        }
        /* 800 samples a frame: a 440 Hz tone, silent for frames 100-105, one click at 230. */
        for (i = 0; i < 800; i++, sample++) {
            short s = (f >= 100 && f < 106) ? 0 : (short)(8000.0 * sin(sample * 2.0 * 3.14159265 * 440.0 / 48000.0));
            if (f == 230 && i == 400) s = 30000;
            fwrite(&s, 2, 1, af);
            fwrite(&s, 2, 1, af);
        }
        fflush(af);
        fprintf(lf, "[TEST] frame %d\n", f);
        fflush(lf);
        in.present_ms = 1.0;
        in.log_bytes = ftell(lf);
        in.draws += f == 150 ? 10 : 100;            /* frame 150 drew a tenth of its neighbours */
        in.vertices += 3000;
        in.clears += 1;
        if (f == 250) in.audio_dropped++;
        recorder_frame_end(&in);
        if (f == 120) recorder_event_text("fe", "text", "Control.GetScreenInfo(story/crib.swf)");
        if (f == 125) recorder_set_view_mode(1);
    }
    if (late) deliver_late(1);
    for (f = 0; f < 40; f++) recorder_set_view_mode(1);   /* held long enough to count */
    if (crash) {
        recorder_on_crash("synthetic crash");
    } else {
        recorder_mark("test");
        nap(3500);                                  /* the worker waits for a still that never comes */
    }
    printf("%s\n", recorder_session_dir());
    recorder_shutdown();
    fclose(lf);
    fclose(af);
    return 0;
}
'''


def compiler():
    if os.name == 'nt':
        return shutil.which('cl')
    return shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')


@pytest.fixture(scope='module')
def driver(tmp_path_factory):
    cc = compiler()
    if not cc:
        pytest.skip('no C compiler')
    folder = tmp_path_factory.mktemp('recorder-native')
    src = folder / 'driver.c'
    src.write_text(DRIVER, encoding='utf-8')
    if os.name == 'nt':
        exe = folder / 'driver.exe'
        cmd = [cc, '/nologo', '/O2', '/TC', '/I' + str(ROOT / 'src/hooks'), str(src),
               str(ROOT / 'src/hooks/recorder.c'), '/Fe:' + str(exe)]
    else:
        exe = folder / 'driver'
        cmd = [cc, '-O2', '-std=gnu11', '-Wall', '-I' + str(ROOT / 'src/hooks'), str(src),
               str(ROOT / 'src/hooks/recorder.c'), '-o', str(exe), '-lpthread', '-lm']
    r = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return exe


def bug_bundle():
    spec = importlib.util.spec_from_file_location('bug_bundle', ROOT / 'scripts/bug-bundle.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(driver, root, *args):
    r = subprocess.run([str(driver), str(root)] + list(args), capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    return Path(r.stdout.strip().splitlines()[-1]), r.stderr


@pytest.fixture(scope='module')
def marked(driver, tmp_path_factory):
    root = tmp_path_factory.mktemp('marked')
    session, err = run(driver, root)
    bundles = sorted((session / 'bundles').iterdir())
    assert len(bundles) == 1, err
    return session, bundles[0], root


def test_session_holds_rows_events_and_identity(marked):
    session, _, root = marked
    rows = (session / 'frames.csv').read_text().splitlines()
    assert rows[0].startswith('frame,t_ms,interval_ms,present_ms,work_ms,capture_ms,draws')
    assert len(rows) == 301
    events = [json.loads(l) for l in (session / 'events.jsonl').read_text().splitlines()]
    kinds = [e['kind'] for e in events]
    for k in ('session_start', 'fe', 'hitch', 'audio_dropped', 'view', 'mark', 'bundle'):
        assert k in kinds, k
    # The planted hitch, found by its frame: a shared CI runner can stall a frame of its own
    # past the 50 ms threshold (seen at frame 90 on Windows), which is not what this checks.
    hitches = [e for e in events if e['kind'] == 'hitch']
    assert any(h['frame'] == 200 and h['interval_ms'] >= 80 for h in hitches), hitches
    view = next(e for e in events if e['kind'] == 'view')
    assert view['mode'] == 1 and view['cutscene'] is True
    meta = json.loads((session / 'session.json').read_text())
    assert meta['build'] == 'test-build' and meta['settings']['display.aspect'] == '4:3'
    assert (root / 'sessions' / 'LATEST').read_text().strip() == session.name


def test_bundle_files_and_index(marked):
    _, bundle, _ = marked
    meta = json.loads((bundle / 'bundle.json').read_text())
    assert meta['format'] == 'defjam-bug-bundle/1' and meta['reason'] == 'test'
    clip = meta['clip']
    assert clip['width'] == 72 and clip['height'] == 48       # 96x64 kept at its shape, 48 rows
    size = (bundle / 'clip.rgb').stat().st_size
    assert size == len(clip['frames']) * clip['width'] * clip['height'] * 3 and len(clip['frames']) > 20
    assert meta['last_screen'] == 'Control.GetScreenInfo(story/crib.swf)'
    assert meta['view_mode'] == 1
    assert meta['summary']['frames'] == 300
    mark = next(json.loads(l) for l in (bundle / 'events.jsonl').read_text().splitlines()
                if json.loads(l)['kind'] == 'mark')
    assert meta['frame_at_mark'] == mark['frame']   # the mark's frame, not the one the bundle was written in
    assert (bundle / 'log-tail.txt').read_text().endswith('[TEST] frame 299\n')
    assert (bundle / 'audio.pcm').stat().st_size % 4 == 0 and meta['audio_offset'] >= 0
    assert 'bug-bundle.py' in (bundle / 'README.txt').read_text()


def test_analysis_finds_every_planted_fault(marked, tmp_path):
    _, bundle, _ = marked
    bb = bug_bundle()
    pytest.importorskip('numpy')
    pytest.importorskip('PIL')
    result = bb.analyse(bundle, tmp_path)
    hitch = [h for h in result['hitches'] if h['frame'] == 200]
    assert hitch and hitch[0]['side'] == 'game'
    assert [o['frame'] for o in result['draw_outliers']] == [150]
    flags = result['pictures']['flags']
    assert any(f['kind'] == 'flash' for f in flags)
    gaps = result['audio']['gaps']
    assert len(gaps) == 1 and 90 <= gaps[0]['ms'] <= 110
    assert gaps[0]['t_ms'] is not None
    assert any(c['step'] > 20000 for c in result['audio']['clicks'])
    for name in ('report.md', 'analysis.json', 'frametime.png', 'audio.png', 'contact-sheet.png', 'around-mark.png',
                 'log-timed.txt'):
        assert (tmp_path / name).exists(), name
    report = (tmp_path / 'report.md').read_text()
    assert 'story/crib' in report and 'Story cutscene' in report
    timed = (tmp_path / 'log-timed.txt').read_text().splitlines()
    assert timed[-1].endswith('[TEST] frame 299') and 'f299' in timed[-1]


def test_crash_bundle_is_written_at_once(driver, tmp_path):
    session, err = run(driver, tmp_path, 'crash')
    bundles = list((session / 'bundles').iterdir())
    assert len(bundles) == 1 and '-crash-' in bundles[0].name
    meta = json.loads((bundles[0] / 'bundle.json').read_text())
    assert meta['reason'] == 'crash' and meta['detail'] == 'synthetic crash'
    assert (bundles[0] / 'clip.rgb').exists()


def test_old_sessions_keep_bundles_and_lose_bulk(driver, tmp_path):
    import time
    sessions = []
    for _ in range(4):
        sessions.append(run(driver, tmp_path, 'crash')[0])
        time.sleep(1.1)                              # session folders are named by the second
    oldest = sessions[0]
    assert not (oldest / 'frames.csv').exists() and not (oldest / 'audio.pcm').exists()
    assert (oldest / 'events.jsonl').exists() and list((oldest / 'bundles').iterdir())
    assert (sessions[-1] / 'frames.csv').exists()


def test_late_pictures_are_filed_under_their_request(driver, tmp_path):
    """Pictures read back asynchronously arrive two frames late, B, G, R, A: the clip
    must hold them as the synchronous run does, in R, G, B and in request order."""
    session, err = run(driver, tmp_path, 'async')
    bundles = sorted((session / 'bundles').iterdir())
    assert len(bundles) == 1, err
    meta = json.loads((bundles[0] / 'bundle.json').read_text())
    clip = meta['clip']
    assert clip['width'] == 72 and clip['height'] == 48 and len(clip['frames']) > 20
    data = (bundles[0] / 'clip.rgb').read_bytes()
    n = clip['width'] * clip['height'] * 3
    blues = {data[i * n + 2] for i in range(len(clip['frames']))}     # first pixel of each picture
    assert 90 in blues                                                # B stays B (250 is the odd one out)
    reds = {data[i * n] for i in range(len(clip['frames']))}
    assert reds <= {0, 1, 2, 250}                                     # R from x*2 at the left edge
    frames = [c[0] for c in clip['frames']]                           # [frame, t_ms] of the request
    times = [c[1] for c in clip['frames']]
    assert frames == sorted(frames) and len(set(frames)) == len(frames) and times == sorted(times)
