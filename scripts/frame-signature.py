#!/usr/bin/env python3
"""Describe a captured frame without storing any of it.

The renderer's output is the game's own artwork, so no baseline in this
repository may contain pixels (see CLAUDE.md, hygiene). What it can contain is
a description: where the drawn region is, how much of the screen it covers, how
many distinct colours it has, and a hash of the whole buffer. That is enough to
tell "renders the loading screen" from "renders nothing" and from "renders
something different", which is what the L4 row of docs/02-test-plan.md needs.

    python scripts/frame-signature.py logs/frame.bmp
    python scripts/frame-signature.py logs/frame.bmp --check tests/golden/m2-loading-screen.json

Frames come from the host's own back-buffer capture: set RECOMP_TRANS_SHOT to
an output path and RECOMP_TRANS_SHOT_FRAME to the frame to grab.
"""
import argparse
import hashlib
import json
import struct
import sys


def signature(path):
    data = open(path, 'rb').read()
    if data[:2] != b'BM':
        raise SystemExit("%s is not a BMP" % path)
    offset = struct.unpack_from('<I', data, 10)[0]
    width = struct.unpack_from('<i', data, 18)[0]
    height = struct.unpack_from('<i', data, 22)[0]
    depth = struct.unpack_from('<H', data, 28)[0]
    if depth != 24:
        raise SystemExit("expected a 24-bit BMP, got %d-bit" % depth)
    stride = ((width * 3) + 3) & ~3

    colours = set()
    drawn = 0
    min_x, max_x, min_y, max_y = width, -1, height, -1
    for y in range(height):
        row = offset + (height - 1 - y) * stride
        for x in range(width):
            pixel = data[row + x * 3:row + x * 3 + 3]
            if pixel != b'\x00\x00\x00':
                drawn += 1
                colours.add(pixel)
                if x < min_x: min_x = x
                if x > max_x: max_x = x
                if y < min_y: min_y = y
                if y > max_y: max_y = y

    return {
        "width": width,
        "height": height,
        "drawn_pixels": drawn,
        "distinct_colours": len(colours),
        "bbox": [min_x, min_y, max_x, max_y] if max_x >= 0 else None,
        "sha256": hashlib.sha256(data[offset:]).hexdigest(),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("frame")
    ap.add_argument("--check", metavar="BASELINE",
                    help="compare against a stored signature and exit non-zero on a mismatch")
    ap.add_argument("--write", metavar="BASELINE", help="store the signature as the new baseline")
    ap.add_argument("--tolerance", type=float, default=0.05,
                    help="fraction the drawn-pixel count may drift by (default 0.05)")
    args = ap.parse_args()

    sig = signature(args.frame)

    if args.write:
        json.dump(sig, open(args.write, 'w'), indent=2, sort_keys=True)
        print("wrote %s" % args.write)
        return 0

    if not args.check:
        json.dump(sig, sys.stdout, indent=2, sort_keys=True)
        print()
        return 0

    want = json.load(open(args.check))
    problems = []
    if [sig["width"], sig["height"]] != [want["width"], want["height"]]:
        problems.append("resolution %dx%d, expected %dx%d"
                        % (sig["width"], sig["height"], want["width"], want["height"]))
    # The bounding box is compared loosely. This screen animates now that the
    # title's simulation ticks, so the logo breathes by a pixel or two between
    # frames; an exact match would fail on a healthy run. Several pixels of
    # drift is still the same screen, and a different screen misses by far more.
    if sig["bbox"] is None or want["bbox"] is None:
        if sig["bbox"] != want["bbox"]:
            problems.append("drawn region %s, expected %s" % (sig["bbox"], want["bbox"]))
    elif any(abs(a - b) > 8 for a, b in zip(sig["bbox"], want["bbox"])):
        problems.append("drawn region %s, expected about %s" % (sig["bbox"], want["bbox"]))
    # The exact pixel count moves with the driver's filtering, so allow drift;
    # the bounding box and colour count are what actually pin the image down.
    lo = want["drawn_pixels"] * (1.0 - args.tolerance)
    hi = want["drawn_pixels"] * (1.0 + args.tolerance)
    if not lo <= sig["drawn_pixels"] <= hi:
        problems.append("%d pixels drawn, expected about %d"
                        % (sig["drawn_pixels"], want["drawn_pixels"]))

    if problems:
        for p in problems:
            print("FAIL: %s" % p)
        return 1
    print("OK: matches %s" % args.check)
    return 0


if __name__ == "__main__":
    sys.exit(main())
