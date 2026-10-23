#!/usr/bin/env python3
"""One digest of the generated HD packs' pixels, to compare packs made on different platforms.

PNG files made by the same recipe can differ in bytes between platforms (the zlib that
Pillow bundles is not the same build everywhere) while holding the same pixels. This
decodes every PNG under the given pack folders to 8-bit RGBA and prints, for the whole
set, a SHA-256 over "<file name> <width>x<height> <pixel SHA-256>" lines sorted by name,
next to the image count and the total file bytes. Equal pixel digests mean equal packs.

    python scripts/hd-pack-digest.py <data>/mods/faithful-hd-*-opacity <data>/mods/faithful-hd-*-channels

Needs Pillow. Reads only; takes a few minutes for the full 18,971-image set.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import hashlib
import os
from pathlib import Path


def pixels(path):
    from PIL import Image
    with Image.open(path) as image:
        rgba = image.convert('RGBA')
        return path.name, rgba.size, hashlib.sha256(rgba.tobytes()).hexdigest(), path.stat().st_size


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('packs', nargs='+', type=Path, help='pack folders (each holding textures/*.png)')
    parser.add_argument('--workers', type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument('--list', type=Path, help='also write the per-image lines here')
    args = parser.parse_args()
    files = sorted((png for pack in args.packs for png in pack.rglob('*.png')), key=lambda p: p.name)
    if not files:
        parser.error('no PNG files under the given folders')
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        results = list(pool.map(pixels, files, chunksize=32))
    lines = ['%s %dx%d %s' % (name, size[0], size[1], digest) for name, size, digest, _ in results]
    if args.list:
        args.list.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('images', len(results))
    print('file bytes', sum(r[3] for r in results))
    print('pixel digest', hashlib.sha256(('\n'.join(lines) + '\n').encode('utf-8')).hexdigest())


if __name__ == '__main__':
    main()
