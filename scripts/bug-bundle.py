#!/usr/bin/env python3
"""Turn a play-test bug bundle into something an agent can read.

The play-test recorder (src/hooks/recorder.c, docs/playtest-recorder.md) writes a
bundle when the player presses the mark key, when the game crashes or hangs, or
after a long hitch. A bundle holds raw material: a ring of small pictures
(clip.rgb), a full-size still, the frame rows and events of those seconds, the end
of the log and of the mixed audio. None of that is readable as it is: an agent
cannot watch a clip or listen to a sound, and a log line has no time on it.

This script measures first and draws second:

    python scripts/bug-bundle.py <bundle folder>        one bundle
    python scripts/bug-bundle.py latest                 the newest bundle under logs/sessions
    python scripts/bug-bundle.py <session folder>       a whole session: hitches and
                                                        drop-outs over the run, no pictures

It writes, beside the bundle (or into --out):

    report.md          start here: what happened when, with numbers and the files to look at
    analysis.json      the same findings, for tools
    frametime.png      every frame's length over the window, hitches labelled
    audio.png          the sound's level and spectrum over the same seconds, gaps and clicks marked
    contact-sheet.png  pictures across the whole window
    around-mark.png    every picture of the last seconds before the mark
    anomaly-N.png      the pictures around each flagged moment
    log-timed.txt      the log's end, each line with its time and frame
    clip.mp4           with --mp4 and ffmpeg on the PATH, for a person

The log stays as the game wrote it. Every frame row records how many bytes of the
log had been written by then (log_bytes), so a line's time is the first frame
after it reached the file: exact to a frame while recording, since the recorder
flushes the log once a frame. Audio is placed the same way (pcm_bytes).

Needs Pillow and NumPy (pip install pillow numpy). The bundle is the game's own
pictures and sound: keep it and everything made from it local.
"""

import argparse
import bisect
import csv
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

try:
    import numpy as np
except ImportError:          # pragma: no cover - reported in the report
    np = None
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:          # pragma: no cover
    Image = None

# The reference palette (light surface): ink for text, one hue for the data, red only
# for a flagged moment, which is always labelled as well.
SURFACE = (252, 252, 251)
INK = (11, 11, 11)
INK_2 = (82, 81, 78)
GRID = (226, 225, 220)
SERIES = (42, 120, 214)
FLAG = (227, 73, 72)
SHADE = (250, 222, 220)

AUDIO_RATE = 48000
AUDIO_BYTES_PER_SECOND = AUDIO_RATE * 4      # s16le stereo


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def read_rows(path):
    rows = []
    if not path.exists():
        return rows
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: (float(v) if k not in ('frame', 'step', 'log_bytes', 'pcm_bytes', 'draws',
                                                       'vertices', 'clears', 'audio_dropped', 'audio_dry',
                                                       'tex_uploads', 'tex_evictions', 'tex_cached')
                                 else int(float(v))) for k, v in r.items() if v != ''})
            except (ValueError, TypeError):  # a run stopped mid-row leaves short last line
                continue
    return rows


def read_events(path):
    out = []
    if not path.exists():
        return out
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def find_target(arg):
    """A bundle folder, a session folder, or 'latest' (the newest bundle, else the newest session)."""
    if arg != 'latest':
        return Path(arg)
    sessions = REPO / 'logs' / 'sessions'
    bundles = sorted(sessions.glob('*/bundles/*/bundle.json'), key=lambda p: p.stat().st_mtime)
    if bundles:
        return bundles[-1].parent
    latest = sessions / 'LATEST'
    if latest.exists():
        return sessions / latest.read_text().strip()
    raise SystemExit('no sessions under %s' % sessions)


def load(folder):
    folder = Path(folder)
    b = {'folder': folder}
    if (folder / 'bundle.json').exists():
        b['kind'] = 'bundle'
        b['meta'] = json.loads((folder / 'bundle.json').read_text(encoding='utf-8'))
    elif (folder / 'session.json').exists():
        b['kind'] = 'session'
        b['meta'] = json.loads((folder / 'session.json').read_text(encoding='utf-8'))
    else:
        raise SystemExit('%s holds neither bundle.json nor session.json' % folder)
    b['rows'] = read_rows(folder / 'frames.csv')
    b['events'] = read_events(folder / 'events.jsonl')
    return b


# ---------------------------------------------------------------------------
# Frames
# ---------------------------------------------------------------------------

def percentile(values, p):
    if not values:
        return 0.0
    v = sorted(values)
    i = int(round(p * (len(v) - 1)))
    return v[max(0, min(len(v) - 1, i))]


def frame_stats(rows, hitch_floor=50.0, factor=2.5):
    """Pacing over the rows and the hitches in them.

    A hitch is a frame whose length, less the recorder's own picture read-back, is at
    least `hitch_floor` ms and `factor` times the median of the 120 frames before it:
    the rule the recorder uses, re-applied here so a bundle's rows are judged alike.
    Each is given a likely side: 'present' when most of it was spent in Present
    (the display or the graphics card), 'game' when most was before Present (the
    game's own work and the push-buffer executor), 'recorder' when the read-back made it."""
    iv = [r['interval_ms'] for r in rows if r.get('interval_ms', 0) > 0]
    stats = {'frames': len(rows), 'median_ms': percentile(iv, 0.5), 'p95_ms': percentile(iv, 0.95),
             'p99_ms': percentile(iv, 0.99), 'worst_ms': max(iv) if iv else 0.0}
    slowest = sorted(iv, reverse=True)[:max(1, len(iv) // 100)] if iv else []
    stats['one_percent_low_fps'] = 1000.0 / (sum(slowest) / len(slowest)) if slowest else 0.0
    stats['mean_fps'] = 1000.0 / (sum(iv) / len(iv)) if iv else 0.0
    capture = [r for r in rows if r.get('capture_ms', 0) > 0]
    stats['capture_frames'] = len(capture)
    stats['capture_mean_ms'] = sum(r['capture_ms'] for r in capture) / len(capture) if capture else 0.0
    hitches = []
    window = []
    for r in rows:
        interval = r.get('interval_ms', 0.0)
        if interval <= 0:
            continue
        own = interval - r.get('capture_ms', 0.0)
        median = percentile(window, 0.5) if len(window) >= 10 else 16.67
        if own >= hitch_floor and own >= factor * median:
            present, work = r.get('present_ms', 0.0), r.get('work_ms', 0.0)
            if r.get('capture_ms', 0.0) >= 0.5 * interval:
                side = 'recorder'
            elif present >= 0.5 * interval:
                side = 'present'
            elif work >= 0.5 * interval:
                side = 'game'
            else:
                side = 'mixed'
            hitches.append({'frame': r['frame'], 't_ms': r['t_ms'], 'interval_ms': interval, 'median_ms': median,
                            'present_ms': present, 'work_ms': work, 'capture_ms': r.get('capture_ms', 0.0),
                            'draws': r.get('draws'), 'side': side,
                            'tex_uploads': r.get('tex_uploads', 0), 'tex_upload_kb': r.get('tex_upload_kb', 0.0),
                            'tex_evictions': r.get('tex_evictions', 0)})
        else:
            window.append(interval)
            if len(window) > 120:
                window.pop(0)
    stats['hitches'] = hitches
    # The texture cache (sessions from before these columns have none of them).
    if rows and 'tex_uploads' in rows[0]:
        stats['textures'] = {
            'uploads': sum(r.get('tex_uploads', 0) for r in rows),
            'upload_mb': sum(r.get('tex_upload_kb', 0.0) for r in rows) / 1024.0,
            'evictions': sum(r.get('tex_evictions', 0) for r in rows),
            'busiest_frame_uploads': max(r.get('tex_uploads', 0) for r in rows),
            'cached_max': max(r.get('tex_cached', 0) for r in rows),
            'cache_mb_max': max(r.get('tex_cache_mb', 0.0) for r in rows),
            'hd_cache_mb_max': max(r.get('hd_cache_mb', 0.0) for r in rows)}
    stats['audio_dropped'] = sum(r.get('audio_dropped', 0) for r in rows)
    stats['audio_dry'] = sum(r.get('audio_dry', 0) for r in rows)
    return stats


def draw_outliers(rows, ratio=0.5, min_draws=20):
    """Frames whose draw count is far from both neighbours' while the neighbours agree:
    a frame that lost (or gained) a batch of geometry for one frame -- a flash of a
    missing character, a menu drawn without its panel."""
    out = []
    for i in range(1, len(rows) - 1):
        a, b, c = rows[i - 1].get('draws', 0), rows[i].get('draws', 0), rows[i + 1].get('draws', 0)
        ref = (a + c) / 2.0
        if ref < min_draws or abs(a - c) > ratio * 0.5 * ref:
            continue
        if abs(b - ref) > ratio * ref:
            out.append({'frame': rows[i]['frame'], 't_ms': rows[i]['t_ms'], 'draws': b, 'neighbours': [a, c]})
    return out


# ---------------------------------------------------------------------------
# Putting log lines and audio on the frames' clock
# ---------------------------------------------------------------------------

def byte_clock(rows, field):
    """(bytes, t_ms, frame) points where `field` grew, for bisecting."""
    pts = []
    last = -1
    for r in rows:
        v = r.get(field, -1)
        if v is None or v < 0:
            continue
        if v > last:
            pts.append((v, r['t_ms'], r['frame']))
            last = v
    return pts


def time_of_byte(points, offset):
    """The first frame by which `offset` bytes had been written; None before the window."""
    if not points:
        return None
    keys = [p[0] for p in points]
    i = bisect.bisect_left(keys, offset)
    if i >= len(points):
        return None
    if i == 0 and offset < points[0][0] and len(points) > 1 and points[0][0] - offset > (points[1][0] - points[0][0]) * 50:
        return None
    return points[i][1], points[i][2]


def timed_log(folder, meta, rows):
    """[(t_ms or None, frame or None, line)] for log-tail.txt."""
    path = folder / (meta.get('log_tail') or 'log-tail.txt')
    if not path.exists():
        return []
    data = path.read_bytes()
    offset = int(meta.get('log_tail_offset', 0) or 0)
    pts = byte_clock(rows, 'log_bytes')
    out = []
    pos = 0
    for raw in data.split(b'\n'):
        end = offset + pos + len(raw) + 1
        pos += len(raw) + 1
        if not raw.strip():
            continue
        t = time_of_byte(pts, end)
        out.append((t[0] if t else None, t[1] if t else None, raw.decode('utf-8', 'replace').rstrip('\r')))
    return out


def audio_time_map(rows, audio_offset):
    """t_ms of a sample index in the bundle's audio.pcm, by interpolating pcm_bytes."""
    pts = byte_clock(rows, 'pcm_bytes')
    if len(pts) < 2:
        return None
    xs = [p[0] - audio_offset for p in pts]
    ts = [p[1] for p in pts]

    def at(sample):
        b = sample * 4
        i = bisect.bisect_left(xs, b)
        if i <= 0:
            return ts[0] - (xs[0] - b) / AUDIO_BYTES_PER_SECOND * 1000.0
        if i >= len(xs):
            return ts[-1] + (b - xs[-1]) / AUDIO_BYTES_PER_SECOND * 1000.0
        x0, x1, t0, t1 = xs[i - 1], xs[i], ts[i - 1], ts[i]
        return t0 + (t1 - t0) * (b - x0) / max(1, x1 - x0)
    return at


# ---------------------------------------------------------------------------
# Audio
# ---------------------------------------------------------------------------

def analyse_audio(pcm_path, block_ms=10):
    """Level per block, and the moments that are heard as faults.

    gaps     the output fell to silence (below -60 dBFS) for 30 ms or more with sound on
             both sides: a drop-out, as against a quiet passage
    clipped  samples at full scale, in runs
    clicks   a step between two samples far beyond the signal's own slope around it:
             a discontinuity, which is heard as a click or pop"""
    if np is None or not Path(pcm_path).exists():
        return None
    raw = np.fromfile(str(pcm_path), dtype='<i2')
    if raw.size < 4:
        return None
    raw = raw[:raw.size - raw.size % 2].reshape(-1, 2).astype(np.float32)
    mono = raw.mean(axis=1)
    n = int(AUDIO_RATE * block_ms / 1000)
    blocks = mono.size // n
    if blocks == 0:
        return None
    b = mono[:blocks * n].reshape(blocks, n)
    rms = np.sqrt((b ** 2).mean(axis=1)) + 1e-9
    peak = np.abs(raw[:blocks * n]).reshape(blocks, n * 2).max(axis=1)
    db = 20 * np.log10(rms / 32768.0)

    gaps = []
    silent = db < -60.0
    i = 0
    while i < blocks:
        if silent[i]:
            j = i
            while j < blocks and silent[j]:
                j += 1
            length = (j - i) * block_ms
            before = db[max(0, i - 10):i]
            after = db[j:j + 10]
            if length >= 30 and before.size and after.size and before.max() > -40 and after.max() > -40:
                gaps.append({'sample': i * n, 'ms': length})
            i = j
        else:
            i += 1

    full = np.abs(raw).max(axis=1) >= 32767
    clipped = int(full.sum())
    clip_runs = []
    if clipped:
        idx = np.flatnonzero(full)
        starts = idx[np.r_[True, np.diff(idx) > 1]]
        for s in starts[:20]:
            clip_runs.append({'sample': int(s)})

    d = np.abs(np.diff(mono))
    clicks = []
    if d.size > 2 * n:
        k = np.ones(n, dtype=np.float32) / n
        local = np.convolve(d, k, mode='same')
        score = d / (local + 50.0)
        cand = np.flatnonzero((score > 12.0) & (d > 3000))
        last = -AUDIO_RATE
        for c in cand:
            if c - last > AUDIO_RATE // 20:          # one per 50 ms
                clicks.append({'sample': int(c), 'step': int(d[c])})
                last = c
            if len(clicks) >= 30:
                break
    return {'samples': int(mono.size), 'seconds': mono.size / AUDIO_RATE, 'block_ms': block_ms,
            'db': db, 'peak': peak, 'mono': mono, 'gaps': gaps, 'clipped_samples': clipped,
            'clip_runs': clip_runs, 'clicks': clicks, 'loudest_dbfs': float(db.max()),
            'silent_fraction': float(silent.mean())}


# ---------------------------------------------------------------------------
# Pictures
# ---------------------------------------------------------------------------

def load_clip(folder, meta):
    clip = meta.get('clip') or {}
    path = folder / (clip.get('file') or 'clip.rgb')
    w, h = clip.get('width', 0), clip.get('height', 0)
    frames = clip.get('frames') or []
    if np is None or not path.exists() or not w or not h or not frames:
        return None
    data = np.fromfile(str(path), dtype=np.uint8)
    count = min(len(frames), data.size // (w * h * 3))
    if count == 0:
        return None
    pics = data[:count * w * h * 3].reshape(count, h, w, 3)
    return {'pics': pics, 'frames': [int(f[0]) for f in frames[:count]], 't': [float(f[1]) for f in frames[:count]],
            'w': w, 'h': h}


def analyse_pictures(clip):
    """Per picture: brightness, change from the one before, share of black. Flags:

    black    a picture far darker than the ones on either side (a frame that drew nothing)
    flash    a picture unlike both neighbours while they are like each other: something
             that was wrong for one picture (a missing texture, a flash, a mis-sorted layer)
    cut      a large change that stays: a scene cut, reported but not flagged"""
    if clip is None:
        return None
    pics = clip['pics'].astype(np.int16)
    luma = (0.299 * pics[..., 0] + 0.587 * pics[..., 1] + 0.114 * pics[..., 2])
    mean = luma.mean(axis=(1, 2))
    black = (luma < 16).mean(axis=(1, 2))
    diff = np.zeros(len(pics))
    for i in range(1, len(pics)):
        diff[i] = np.abs(pics[i] - pics[i - 1]).mean()
    flags, cuts = [], []
    for i in range(1, len(pics) - 1):
        around = np.abs(pics[i + 1] - pics[i - 1]).mean()
        if mean[i] < 12 and mean[i - 1] > 30 and mean[i + 1] > 30:
            flags.append({'index': i, 'kind': 'black', 'luma': float(mean[i])})
        elif diff[i] > 18 and diff[i + 1] > 18 and around < 0.4 * min(diff[i], diff[i + 1]):
            flags.append({'index': i, 'kind': 'flash', 'change': float(diff[i]), 'neighbours_apart': float(around)})
        elif diff[i] > 30 and around > 0.7 * diff[i]:
            cuts.append({'index': i, 'change': float(diff[i])})
    for f in flags + cuts:
        f['frame'] = clip['frames'][f['index']]
        f['t_ms'] = clip['t'][f['index']]
    return {'mean_luma': mean, 'black_fraction': black, 'change': diff, 'flags': flags, 'cuts': cuts}


def font(size):
    for name in ('DejaVuSans.ttf', 'Arial.ttf', 'arial.ttf', 'Helvetica.ttc'):
        try:
            return ImageFont.truetype(name, size)
        except (OSError, IOError):
            continue
    return ImageFont.load_default()


def contact_sheet(clip, indices, labels, path, cols=6, title=None):
    if Image is None or clip is None or not indices:
        return None
    w, h = clip['w'], clip['h']
    scale = 1.0 if w <= 360 else 360.0 / w
    tw, th = int(w * scale), int(h * scale)
    rows = (len(indices) + cols - 1) // cols
    pad, label_h, head = 6, 18, 28 if title else 0
    sheet = Image.new('RGB', (cols * (tw + pad) + pad, head + rows * (th + label_h + pad) + pad), SURFACE)
    draw = ImageDraw.Draw(sheet)
    f = font(12)
    if title:
        draw.text((pad, 6), title, fill=INK, font=font(15))
    for k, (i, label) in enumerate(zip(indices, labels)):
        x = pad + (k % cols) * (tw + pad)
        y = head + pad + (k // cols) * (th + label_h + pad)
        im = Image.fromarray(clip['pics'][i])
        if scale != 1.0:
            im = im.resize((tw, th), Image.BILINEAR)
        sheet.paste(im, (x, y))
        flagged = label.startswith('!')
        draw.text((x, y + th + 2), label.lstrip('!'), fill=FLAG if flagged else INK_2, font=f)
        if flagged:
            draw.rectangle([x - 2, y - 2, x + tw + 1, y + th + 1], outline=FLAG, width=2)
    sheet.save(path)
    return path


def rel(t, mark):
    return (t - mark) / 1000.0


def chart_axes(draw, box, x0, x1, y0, y1, xlabel, ylabel, yticks, f):
    left, top, right, bottom = box
    for v in yticks:
        y = bottom - (v - y0) / (y1 - y0) * (bottom - top)
        draw.line([left, y, right, y], fill=GRID, width=1)
        draw.text((left - 6 - draw.textlength('%g' % v, font=f), y - 7), '%g' % v, fill=INK_2, font=f)
    span = x1 - x0
    step = next(s for s in (0.5, 1, 2, 5, 10, 20, 30, 60, 120, 300, 600, 1800, 3600) if span / s <= 12)
    v = math.ceil(x0 / step) * step
    while v <= x1:
        x = left + (v - x0) / span * (right - left)
        draw.line([x, bottom, x, bottom + 4], fill=INK_2, width=1)
        txt = '%g' % v
        draw.text((x - draw.textlength(txt, font=f) / 2, bottom + 6), txt, fill=INK_2, font=f)
        v += step
    draw.line([left, bottom, right, bottom], fill=INK_2, width=1)
    draw.text((right - draw.textlength(xlabel, font=f), bottom + 22), xlabel, fill=INK_2, font=f)
    draw.text((left, top - 18), ylabel, fill=INK_2, font=f)


def frametime_chart(rows, stats, mark, path, title):
    if Image is None or not rows:
        return None
    W, H = 1400, 420
    box = (70, 50, W - 30, H - 50)
    im = Image.new('RGB', (W, H), SURFACE)
    d = ImageDraw.Draw(im)
    f = font(12)
    d.text((box[0], 12), title, fill=INK, font=font(15))
    xs = [rel(r['t_ms'], mark) for r in rows]
    x0, x1 = min(xs), max(xs)
    if x1 <= x0:
        x1 = x0 + 1
    # Tall enough for the worst frame, up to a quarter of a second; longer ones are drawn at the top.
    top = min(max(40.0, stats['p99_ms'] * 1.6, stats['worst_ms'] * 1.1), 250.0)
    ticks = [v for v in (0, 8.3, 16.7, 33.3, 50, 75, 100, 150, 200, 250) if v <= top]
    chart_axes(d, box, x0, x1, 0, top, 'seconds from the mark', 'frame length (ms)', ticks, f)
    left, t, right, bottom = box

    def px(x, y):
        return (left + (x - x0) / (x1 - x0) * (right - left), bottom - min(y, top) / top * (bottom - t))
    # A step line: each frame is a level across the time it took, so a hitch is a wide plateau.
    pts = []
    for x, r in zip(xs, rows):
        iv = r.get('interval_ms', 0)
        if iv <= 0:
            continue
        pts.append(px(max(x0, x - iv / 1000.0), iv))
        pts.append(px(x, iv))
    if len(pts) > 1:
        d.line(pts, fill=SERIES, width=2)
    if x0 <= 0 <= x1 and mark:
        mx = px(0, 0)[0]
        for y in range(int(t), int(bottom), 8):
            d.line([mx, y, mx, y + 4], fill=INK, width=1)
        d.text((mx - 34 if mx > right - 40 else mx + 4, t), 'mark', fill=INK, font=f)
    for k, hch in enumerate(sorted(stats['hitches'], key=lambda h: -h['interval_ms'])[:8]):
        x, y = px(rel(hch['t_ms'], mark), hch['interval_ms'])
        d.ellipse([x - 5, y - 5, x + 5, y + 5], fill=FLAG, outline=SURFACE, width=2)
        label = '%.0f ms (%s)' % (hch['interval_ms'], hch['side'])
        d.text((x + 7, max(t, y - 16)), label, fill=INK, font=f)
    im.save(path)
    return path


def audio_chart(audio, tmap, mark, x_range, path, title):
    if Image is None or audio is None:
        return None
    W, H = 1400, 520
    im = Image.new('RGB', (W, H), SURFACE)
    d = ImageDraw.Draw(im)
    f = font(12)
    d.text((70, 12), title, fill=INK, font=font(15))
    n = int(AUDIO_RATE * audio['block_ms'] / 1000)

    def t_of(sample):
        return rel(tmap(sample), mark) if tmap else sample / AUDIO_RATE
    x0 = t_of(0)
    x1 = t_of(audio['samples'])
    if x_range:
        x0, x1 = max(x0, x_range[0]), min(x1, x_range[1]) if x_range[1] > x_range[0] else x1
    if x1 <= x0:
        x1 = x0 + 1
    box = (70, 50, W - 30, 250)
    chart_axes(d, box, x0, x1, -60, 0, 'seconds from the mark' if tmap else 'seconds', 'level (dBFS, per 10 ms)',
               [-60, -40, -20, 0], f)
    left, top, right, bottom = box

    def px(x, y):
        return (left + (x - x0) / (x1 - x0) * (right - left), bottom - (max(-60.0, min(0.0, y)) + 60) / 60 * (bottom - top))
    for g in audio['gaps']:
        a, b = px(t_of(g['sample']), 0)[0], px(t_of(g['sample'] + g['ms'] * AUDIO_RATE // 1000), 0)[0]
        d.rectangle([a, top, max(a + 2, b), bottom], fill=SHADE)
    pts = [px(t_of(i * n), float(v)) for i, v in enumerate(audio['db'])]
    pts = [p for p in pts if left <= p[0] <= right]
    if len(pts) > 1:
        d.line(pts, fill=SERIES, width=2)
    for c in audio['clicks'][:12]:
        x, _ = px(t_of(c['sample']), 0)
        d.line([x, top, x, bottom], fill=FLAG, width=1)
    for g in audio['gaps'][:8]:
        x, _ = px(t_of(g['sample']), 0)
        d.text((x + 3, top + 2), 'gap %d ms' % g['ms'], fill=INK, font=f)
    if mark and x0 <= 0 <= x1:
        mx = px(0, 0)[0]
        d.line([mx, top, mx, bottom], fill=INK, width=1)

    # Spectrum: 0-12 kHz, darker is louder.
    sbox = (70, 300, W - 30, H - 40)
    mono = audio['mono']
    hop = 1024
    win = np.hanning(2048).astype(np.float32)
    cols = max(1, (mono.size - 2048) // hop)
    width = sbox[2] - sbox[0]
    step = max(1, cols // width)
    spec = []
    for c in range(0, cols, step):
        seg = mono[c * hop:c * hop + 2048]
        if seg.size < 2048:
            break
        mag = np.abs(np.fft.rfft(seg * win))[:512]
        spec.append(20 * np.log10(mag + 1.0))
    if spec:
        s = np.array(spec).T[::-1]
        s = np.clip((s - s.max() + 90) / 90, 0, 1)
        img = (255 - s * 220).astype(np.uint8)
        sp = Image.fromarray(img).resize((width, sbox[3] - sbox[1]), Image.BILINEAR).convert('RGB')
        im.paste(sp, (sbox[0], sbox[1]))
        d.text((sbox[0], sbox[1] - 18), 'spectrum, 0 to 12 kHz (darker is louder), same time axis', fill=INK_2, font=f)
    im.save(path)
    return path


# ---------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------

def near(items, t, ms, key='t_ms'):
    return [x for x in items if x.get(key) is not None and abs(x[key] - t) <= ms]


def describe_event(e):
    k = e.get('kind')
    if k == 'fe':
        return 'front end: %s' % e.get('text', '')
    if k == 'game':
        return '%s %s' % (e.get('tag', 'game'), e.get('event', ''))
    if k == 'view':
        return 'view mode %s%s' % (e.get('mode'), ' (Story cutscene)' if e.get('cutscene') else '')
    if k in ('hitch',):
        return 'hitch %.0f ms' % e.get('interval_ms', 0)
    if k in ('audio_dry', 'audio_dropped'):
        return '%s x%s' % (k, e.get('count'))
    if k == 'memory':
        return 'memory %.0f MB' % e.get('private_mb', 0)
    rest = {a: b for a, b in e.items() if a not in ('t_ms', 'frame', 'kind')}
    return '%s %s' % (k, json.dumps(rest)[:160] if rest else '')


def context_at(events, t):
    """The screen, the view and the last match start before t."""
    screen = view = match = None
    for e in events:
        if e.get('t_ms', 0) > t:
            break
        if e.get('kind') == 'fe' and 'getscreeninfo(' in e.get('text', '').lower():
            screen = e['text']
        elif e.get('kind') == 'view':
            view = e
        elif e.get('kind') == 'game' and e.get('event') == 'match_start':
            match = e
    return screen, view, match


def fighters_text(match):
    if not match:
        return None
    fs = match.get('fighters') or []
    parts = []
    for f in fs:
        if isinstance(f, dict):
            parts.append('slot %s fighter %s%s' % (f.get('slot'), f.get('id', f.get('character', '?')),
                                                 ' (CPU)' if f.get('cpu') else ''))
    return '; '.join(parts) if parts else json.dumps({k: v for k, v in match.items() if k not in ('t_ms', 'frame')})[:300]


def analyse(target, out=None, mp4=False):
    b = load(target)
    folder = b['folder']
    out = Path(out) if out else folder
    out.mkdir(parents=True, exist_ok=True)
    meta, rows, events = b['meta'], b['rows'], b['events']
    mark = float(meta.get('mark_t_ms', 0.0)) if b['kind'] == 'bundle' else 0.0
    stats = frame_stats(rows)
    outliers = draw_outliers(rows)
    result = {'kind': b['kind'], 'folder': str(folder), 'reason': meta.get('reason'), 'detail': meta.get('detail'),
              'frames': {k: v for k, v in stats.items() if k != 'hitches'}, 'hitches': stats['hitches'],
              'draw_outliers': outliers, 'files': {}}
    notes = []
    if np is None or Image is None:
        notes.append('NumPy or Pillow is missing (pip install pillow numpy): no pictures, audio or charts were analysed.')

    # Log
    log = timed_log(folder, meta, rows) if b['kind'] == 'bundle' else []
    if log:
        with open(out / 'log-timed.txt', 'w', encoding='utf-8') as f:
            placed = False      # an unplaced line after a placed one was written after the last frame row
            for t, fr, line in log:
                placed = placed or t is not None
                f.write('%s %s  %s\n' % ('[%+9.3fs' % rel(t, mark) if t is not None else
                                         ('[  after  ' if placed else '[  before '),
                                         ('f%-7d]' % fr) if fr is not None else '        ]', line))
        result['files']['log'] = 'log-timed.txt'

    # Audio
    audio = None
    tmap = None
    if b['kind'] == 'bundle' and meta.get('audio'):
        audio = analyse_audio(folder / meta['audio'])
        tmap = audio_time_map(rows, int(meta.get('audio_offset', 0) or 0)) if audio else None
    elif b['kind'] == 'session':
        pcm = folder / 'audio.pcm'
        if not pcm.exists() and meta.get('audio'):
            pcm = Path(meta['audio'])
        if pcm.exists():
            audio = analyse_audio(pcm)
            tmap = audio_time_map(rows, 0) if audio else None
    if audio:
        def at(sample):
            return tmap(sample) if tmap else None
        result['audio'] = {
            'seconds': audio['seconds'], 'loudest_dbfs': audio['loudest_dbfs'], 'silent_fraction': audio['silent_fraction'],
            'clipped_samples': audio['clipped_samples'],
            'gaps': [dict(g, t_ms=at(g['sample'])) for g in audio['gaps']],
            'clicks': [dict(c, t_ms=at(c['sample'])) for c in audio['clicks']],
            'clip_runs': [dict(c, t_ms=at(c['sample'])) for c in audio['clip_runs']]}

    # Pictures
    clip = load_clip(folder, meta) if b['kind'] == 'bundle' else None
    pic = analyse_pictures(clip) if clip else None
    if pic:
        result['pictures'] = {'count': len(clip['frames']), 'size': [clip['w'], clip['h']], 'flags': pic['flags'],
                              'cuts': pic['cuts']}

    # Images
    if rows:
        title = ('%s bundle: frame length' % meta.get('reason', '')) if b['kind'] == 'bundle' else 'session: frame length'
        if frametime_chart(rows, stats, mark, out / 'frametime.png', title):
            result['files']['frametime'] = 'frametime.png'
    if audio:
        xr = (rel(rows[0]['t_ms'], mark), rel(rows[-1]['t_ms'], mark)) if rows and tmap else None
        if audio_chart(audio, tmap, mark, xr, out / 'audio.png', 'mixed audio: level and spectrum'):
            result['files']['audio'] = 'audio.png'
    anomalies = []
    if clip:
        n = len(clip['frames'])
        flagged = {f['index'] for f in (pic['flags'] if pic else [])}

        def label(i):
            return '%s%+.2fs f%d' % ('!' if i in flagged else '', rel(clip['t'][i], mark), clip['frames'][i])
        overview = sorted(set(int(round(k * (n - 1) / 23.0)) for k in range(24))) if n > 24 else list(range(n))
        if contact_sheet(clip, overview, [label(i) for i in overview], out / 'contact-sheet.png',
                         title='the whole window, seconds from the mark'):
            result['files']['contact_sheet'] = 'contact-sheet.png'
        last = [i for i in range(n) if -3000 <= clip['t'][i] - mark <= 3000][-36:]
        if last and contact_sheet(clip, last, [label(i) for i in last], out / 'around-mark.png',
                                  title='every picture from 3 s before the mark to 3 s after'):
            result['files']['around_mark'] = 'around-mark.png'
        # Moments worth a close look: flagged pictures, the worst hitches, audio faults.
        moments = [('picture %s' % f['kind'], f['t_ms']) for f in (pic['flags'] if pic else [])]
        moments += [('hitch %.0f ms' % h['interval_ms'], h['t_ms'])
                    for h in sorted(stats['hitches'], key=lambda h: -h['interval_ms'])[:3]]
        if audio and tmap:
            moments += [('audio gap %d ms' % g['ms'], tmap(g['sample'])) for g in audio['gaps'][:2]]
            moments += [('audio click', tmap(c['sample'])) for c in audio['clicks'][:2]]
        seen = []
        for what, t in moments:
            if t is None or any(abs(t - s) < 1000 for s in seen) or len(seen) >= 6:
                continue
            seen.append(t)
            idx = [i for i in range(n) if abs(clip['t'][i] - t) <= 1000][:18]
            name = 'anomaly-%d.png' % (len(seen))
            if idx and contact_sheet(clip, idx, [label(i) for i in idx], out / name,
                                     title='%s at %+.2f s: the pictures a second either side' % (what, rel(t, mark))):
                anomalies.append({'what': what, 't_ms': t, 'file': name})
    result['anomaly_sheets'] = anomalies

    if mp4 and clip and shutil.which('ffmpeg'):
        fps = max(1, round(len(clip['frames']) / max(0.001, (clip['t'][-1] - clip['t'][0]) / 1000.0)))
        cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s',
               '%dx%d' % (clip['w'], clip['h']), '-r', str(fps), '-i', str(folder / meta['clip']['file'])]
        if meta.get('audio'):
            cmd += ['-f', 's16le', '-ar', '48000', '-ac', '2', '-i', str(folder / meta['audio'])]
        cmd += ['-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-shortest', str(out / 'clip.mp4')]
        if subprocess.run(cmd).returncode == 0:
            result['files']['mp4'] = 'clip.mp4'

    report = write_report(b, result, stats, log, events, mark, notes)
    (out / 'report.md').write_text(report, encoding='utf-8')
    with open(out / 'analysis.json', 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=1, default=lambda o: None)
    return result


def write_report(b, result, stats, log, events, mark, notes):
    meta = b['meta']
    L = []
    if b['kind'] == 'bundle':
        L.append('# Bug bundle: %s' % meta.get('reason'))
        L.append('')
        L.append('`%s`. Times are seconds from the mark (negative is before it); `fN` is the frame.' % b['folder'])
        if meta.get('detail'):
            L.append('')
            L.append('Detail: %s' % meta['detail'])
    else:
        L.append('# Play-test session')
        L.append('')
        L.append('`%s`. Times are seconds from the start of the session.' % b['folder'])
    L.append('')
    L.append('Build `%s`, %s.' % (meta.get('build', '?'), meta.get('platform', 'platform not recorded')))
    for n in notes:
        L.append('')
        L.append('> ' + n)

    # Where
    screen, view, match = context_at(events, mark if b['kind'] == 'bundle' else float('inf'))
    screen = screen or meta.get('last_screen') or None
    L += ['', '## Where', '']
    L.append('- Screen: %s' % (screen or 'not seen in these seconds'))
    vm = view['mode'] if view else meta.get('view_mode', -1)
    L.append('- View: %s' % ({1: 'a Story cutscene (view mode 1)', 0: 'normal (view mode 0)',
                              2: 'a sub-view (mode 2)'}.get(vm, 'not reported')))
    match = match or meta.get('last_match_start')
    ft = fighters_text(match)
    L.append('- Match: %s' % (ft or 'none started in these seconds'))
    s = meta.get('settings') or {}
    if s:
        keys = ['display.aspect', 'display.render_scale', 'display.vsync', 'textures.enabled', 'debug.record_fps']
        # The key alone, unless it says nothing without its section (textures.enabled).
        L.append('- Settings: ' + ', '.join('%s=%s' % (k if k.endswith('.enabled') else k.split('.', 1)[1], s[k])
                                            for k in keys if k in s))

    # Frames
    fs = result['frames']
    L += ['', '## Frame pacing', '']
    L.append('| frames | median | p95 | p99 | worst | mean fps | 1% low fps | hitches | read-backs |')
    L.append('|---|---|---|---|---|---|---|---|---|')
    L.append('| %d | %.2f ms | %.2f ms | %.2f ms | %.1f ms | %.1f | %.1f | %d | %d at %.2f ms |' % (
        fs['frames'], fs['median_ms'], fs['p95_ms'], fs['p99_ms'], fs['worst_ms'], fs['mean_fps'],
        fs['one_percent_low_fps'], len(result['hitches']), fs['capture_frames'], fs['capture_mean_ms']))
    L.append('')
    L.append('A hitch is a frame of at least 50 ms and 2.5 times the median before it, after taking off the '
             "recorder's own read-back. Side: `present` waited in Present (display or graphics card), `game` was "
             'busy before it (game code, push-buffer executor, loading), `recorder` was the read-back.')
    if result['hitches']:
        L += ['', '| when | frame | length | present | before present | read-back | side | nearby |', '|---|---|---|---|---|---|---|---|']
        for h in sorted(result['hitches'], key=lambda h: -h['interval_ms'])[:15]:
            nearby = [describe_event(e) for e in near(events, h['t_ms'], 500) if e.get('kind') not in ('hitch', 'memory')]
            close = sorted((abs(t - h['t_ms']), ln) for t, fr, ln in log if t is not None and abs(t - h['t_ms']) <= 200)
            lines = [ln for _, ln in close[:3]]
            tex = []
            if h.get('tex_uploads'):
                tex.append('%d texture upload(s), %.0f KB' % (h['tex_uploads'], h.get('tex_upload_kb', 0.0)))
            if h.get('tex_evictions'):
                tex.append('%d eviction(s)' % h['tex_evictions'])
            ctx = '; '.join(tex + nearby[:3] + ['log: ' + ln[:90] for ln in lines])
            L.append('| %+.3f s | %d | %.1f ms | %.1f | %.1f | %.1f | %s | %s |' % (
                rel(h['t_ms'], mark), h['frame'], h['interval_ms'], h['present_ms'], h['work_ms'], h['capture_ms'],
                h['side'], ctx.replace('|', '/') or '-'))
    tx = fs.get('textures')
    if tx:
        L += ['', 'Texture cache in these rows: %d upload(s), %.1f MB (at most %d in one frame), %d eviction(s); '
              'at most %d textures, %.1f MB held; HD pack at most %.0f MB resident.' % (
                  tx['uploads'], tx['upload_mb'], tx['busiest_frame_uploads'], tx['evictions'], tx['cached_max'],
                  tx['cache_mb_max'], tx['hd_cache_mb_max'])]
    if result['draw_outliers']:
        L += ['', 'Frames whose draw count left both neighbours (possible one-frame missing geometry):', '']
        for o in result['draw_outliers'][:10]:
            L.append('- %+.3f s f%d: %d draws, neighbours %s' % (rel(o['t_ms'], mark), o['frame'], o['draws'], o['neighbours']))
    if 'frametime' in result['files']:
        L += ['', 'Chart: `frametime.png`.']

    # Audio
    L += ['', '## Audio', '']
    L.append('The device reported %d dropped buffer(s) and ran dry %d time(s) in these rows.' % (
        stats['audio_dropped'], stats['audio_dry']))
    a = result.get('audio')
    if a:
        L.append('')
        L.append('Analysed %.1f s of the mixed output: loudest %.1f dBFS, silent %.0f%% of the time, %d clipped sample(s).' % (
            a['seconds'], a['loudest_dbfs'], a['silent_fraction'] * 100, a['clipped_samples']))
        for g in a['gaps'][:10]:
            L.append('- gap of %d ms at %s' % (g['ms'], '%+.3f s' % rel(g['t_ms'], mark) if g['t_ms'] is not None else
                                                  'sample %d' % g['sample']))
        for c in a['clicks'][:10]:
            L.append('- click (a step of %d) at %s' % (c['step'], '%+.3f s' % rel(c['t_ms'], mark) if c['t_ms'] is not None
                                                        else 'sample %d' % c['sample']))
        if not a['gaps'] and not a['clicks'] and not a['clipped_samples']:
            L.append('- no gap, click or clipping found.')
        L.append('')
        L.append('This is the audio the game produced before the audio device, not what the device played: a '
                 'device-side drop-out shows in the counters above, not here. Chart: `audio.png`.')
    else:
        L.append('')
        L.append('No audio in this bundle ([debug] record_audio off, or not analysable).')

    # Pictures
    L += ['', '## Pictures', '']
    p = result.get('pictures')
    if p:
        L.append('%d pictures of %dx%d. Look at `contact-sheet.png` (the whole window) and `around-mark.png` '
                 '(the seconds around the mark)%s.' % (p['count'], p['size'][0], p['size'][1],
                                                       ', and `still.bmp` (full size, at the mark)' if meta.get('still') else ''))
        for f in p['flags'][:10]:
            L.append('- %s picture at %+.3f s (f%d)' % (f['kind'], rel(f['t_ms'], mark), f['frame']))
        if p['cuts']:
            L.append('- %d scene cut(s), not flagged.' % len(p['cuts']))
        if not p['flags']:
            L.append('- no single-picture flash or black picture found. A fault that lasts several pictures, or is '
                     'small, needs a look at the sheets.')
    else:
        L.append('No pictures ([debug] record_fps 0, or none had been taken).')
    for an in result['anomaly_sheets']:
        L.append('- `%s`: %s at %+.2f s' % (an['file'], an['what'], rel(an['t_ms'], mark)))

    # Events and log
    if b['kind'] == 'bundle':
        L += ['', '## Events, 5 s either side of the mark', '']
        shown = [e for e in events if abs(e.get('t_ms', 0) - mark) <= 5000 and e.get('kind') not in ('memory',)]
        tags = [e for e in shown if not (e.get('kind') == 'game' and e.get('tag') == 'TEST-STATE')]
        for e in tags[-40:]:
            L.append('- %+.3f s f%s: %s' % (rel(e['t_ms'], mark), e.get('frame'), describe_event(e)))
        if not tags:
            L.append('- none')
        L += ['', '## Log, 3 s before the mark to the end', '']
        lines = [(t, fr, ln) for t, fr, ln in log if t is not None and t >= mark - 3000]
        if lines:
            L.append('```')
            for t, fr, ln in lines[-40:]:
                L.append('%+8.3fs f%-6s %s' % (rel(t, mark), fr, ln[:200]))
            L.append('```')
            L.append('')
            L.append('All of it with times: `log-timed.txt`.')
        else:
            L.append('No log lines in these seconds (or the log could not be found).')
    else:
        L += ['', '## Screens in this session', '']
        for e in [e for e in events if e.get('kind') == 'fe' and 'getscreeninfo(' in e.get('text', '').lower()][:60]:
            L.append('- %.1f s: %s' % (e['t_ms'] / 1000.0, e['text']))

    L += ['', '## Next steps', '']
    L.append('- Reproduce: start from the nearest `scripts/harness.py` route or a fixture in `config/fixtures.json` '
             '(`python scripts/scenarios.py list`), and add a stage that reaches this screen.')
    L.append('- A hitch on the `game` side: `scripts/sample-threads.py` during a repeat, or `RECOMP_TRACE_PROFILE=1`.')
    L.append('- A picture fault: repeat with `RECOMP_PB_BATCH_DUMP=shot` and a capture at that moment.')
    L.append('- An audio fault: compare `audio.png` with the events above; `RECOMP_APU_LEVEL=1` adds the voice report.')
    return '\n'.join(L) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('target', help='a bundle folder, a session folder, or "latest"')
    ap.add_argument('--out', help='write the results here instead of beside the bundle')
    ap.add_argument('--mp4', action='store_true', help='also make clip.mp4 with ffmpeg, for a person')
    a = ap.parse_args(argv)
    target = find_target(a.target)
    result = analyse(target, a.out, a.mp4)
    out = Path(a.out) if a.out else target
    print('%s: %d frames, %d hitch(es), %d picture flag(s), %d audio gap(s); read %s' % (
        result['kind'], result['frames']['frames'], len(result['hitches']),
        len((result.get('pictures') or {}).get('flags', [])), len((result.get('audio') or {}).get('gaps', [])),
        out / 'report.md'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
