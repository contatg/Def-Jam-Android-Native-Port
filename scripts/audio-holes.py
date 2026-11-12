#!/usr/bin/env python3
"""Count the places where the title's looping mix buffer played the previous lap again.

Def Jam mixes its six-channel stream into a looping buffer (voices 0xF8-0xFA, 2400 frames)
and the APU reads it by the play cursor. A sample the title had not written yet is the sample
that was there one lap earlier, so a run of samples equal to the ones 2400 earlier is a place
the buffer was read before it was written: a click in what is heard. Other lags show none, so
equality at the lap is not chance.

    RECOMP_APU_VOICE_PCM=<prefix> game ...   # writes <prefix>F8.pcm, F9, FA (raw 48 kHz stereo s16)
    python scripts/audio-holes.py <prefix>   # or a folder and the prefix 'v'

Per voice: how many runs of at least --min equal samples (nonzero, above --floor), how many
samples they cover and how long the runs are. Compare builds on the same route
(`harness.py run intro --env RECOMP_APU_VOICE_PCM=...`); counts vary 20% between identical
runs, so use more than one. Needs numpy.
"""
import argparse
import os
import sys

try:
    import numpy as np
except ImportError:
    sys.exit('numpy is required (pip install numpy)')


def holes(path, lap, min_run, floor):
    x = np.fromfile(path, dtype=np.int16).reshape(-1, 2)[:, 0].astype(np.int32)
    if len(x) <= lap:
        return len(x) / 48000.0, []
    eq = (x[lap:] == x[:-lap]) & (np.abs(x[lap:]) > floor)
    edge = np.flatnonzero(np.diff(np.concatenate([[0], eq.astype(np.int8), [0]])))
    runs = [(int(edge[i]) + lap, int(edge[i + 1] - edge[i])) for i in range(0, len(edge), 2)
            if edge[i + 1] - edge[i] >= min_run]
    return len(x) / 48000.0, runs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('prefix', help='the RECOMP_APU_VOICE_PCM prefix (path up to the voice number)')
    ap.add_argument('--lap', type=int, default=2400, help='frames in the buffer (default 2400)')
    ap.add_argument('--min', type=int, default=6, help='shortest run of equal samples counted')
    ap.add_argument('--floor', type=int, default=64, help='ignore samples at or below this level')
    ap.add_argument('--voices', default='F8,FA', help='voice numbers (F9 is silent in the opening scene)')
    args = ap.parse_args()
    found = False
    for v in args.voices.split(','):
        path = f'{args.prefix}{v}.pcm'
        if not os.path.exists(path):
            continue
        found = True
        secs, runs = holes(path, args.lap, args.min, args.floor)
        lens = sorted(l for _, l in runs)
        print(f'{v}: {secs:6.1f} s  {len(runs):5d} holes = {len(runs) / max(secs, 1e-9):.2f}/s  '
              f'{sum(lens):6d} samples  median run {lens[len(lens) // 2] if lens else 0}  '
              f'longest {lens[-1] if lens else 0}')
    if not found:
        sys.exit(f'no {args.prefix}<voice>.pcm found')


if __name__ == '__main__':
    main()
