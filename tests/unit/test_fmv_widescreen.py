"""The FMV widescreen trial's window plan (scripts/fmv-widescreen.py).

The AI model takes a fixed number of frames, so a clip is cut into overlapping windows. A gap would leave
frames without sides; a window that does not start inside the previous one would let the sides jump.
"""
import importlib.util
import os

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("fmv_widescreen", os.path.join(ROOT, "scripts", "fmv-widescreen.py"))
fmv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fmv)


def test_trial_clip_is_two_windows():
    plan = fmv.plan_chunks(149, 81, 12)
    assert [start for start, _ in plan] == [0, 69]
    assert plan[1][1][-1] == 148 and plan[1][1][-2] == 148      # padded with the last frame


@pytest.mark.parametrize("n,chunk,overlap", [(1, 81, 12), (81, 81, 12), (82, 81, 12), (154, 81, 12),
                                             (500, 49, 8), (17, 17, 0)])
def test_windows_cover_every_frame_and_overlap(n, chunk, overlap):
    plan = fmv.plan_chunks(n, chunk, overlap)
    covered = set()
    for k, (start, idx) in enumerate(plan):
        assert len(idx) == chunk
        assert idx[0] == start and all(0 <= i < n for i in idx)
        if k:
            prev_start, prev_idx = plan[k - 1]
            assert start == prev_start + chunk - overlap
        covered.update(idx)
    assert covered == set(range(n))


def test_bad_overlap_and_empty_clip():
    assert fmv.plan_chunks(0, 81, 12) == []
    with pytest.raises(ValueError):
        fmv.plan_chunks(10, 8, 8)
