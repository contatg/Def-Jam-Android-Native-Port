"""Generate a local Lanczos pack from an own dump; never downloads game artwork.

Creates immutable recipe directories, links/copies accepted PNGs, verifies hashes,
then atomically publishes manifests. Source IDs are candidate runtime keys; known
exact runtime aliases may be supplied. Full game coverage remains a release gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
from types import SimpleNamespace

import hd_assets as hd
import hd_corpus


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_suffix('.pending')
    pending.write_text(json.dumps(value, indent=2), encoding='utf-8')
    pending.replace(path)


def file_hash(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def mip_policy(asset):
    # Transparent UI is opacity; material/data alpha retains independent channels.
    return 'opacity' if asset['alpha'][0] == 255 or asset['category'] in (
        'UI and menus', 'HUD and tutorials', 'Loading emblems', 'Font atlases') else 'channels'


def assemble(corpus_root, mods, aliases=None):
    from PIL import Image
    corpus_root, mods = Path(corpus_root).resolve(), Path(mods)
    mods.mkdir(parents=True, exist_ok=True)
    if mods.is_symlink() or mods.resolve() != mods.absolute():
        raise ValueError('Mods destination must not be a link')
    corpus = json.loads((corpus_root / 'corpus.json').read_text())
    ident = corpus['corpus_id']
    if not re.fullmatch('[0-9a-f]{64}', ident):
        raise ValueError('Invalid corpus recipe ID')
    assets = {a['id']: a for a in corpus['assets'] if not a.get('error')}
    entries = {key: key for key in assets}
    for runtime, source in (aliases or {}).items():
        if not re.fullmatch('[0-9a-f]{64}', runtime) or source not in assets:
            raise ValueError('Invalid runtime correlation alias')
        if runtime in entries and entries[runtime] != source:
            raise ValueError('Conflicting runtime alias')
        entries[runtime] = source
    names, count, total = [], 0, 0
    for policy in ('opacity', 'channels'):
        selected = {key: source for key, source in entries.items() if mip_policy(assets[source]) == policy}
        if not selected:
            continue
        name = 'faithful-hd-' + ident[:12] + '-' + policy
        pack = mods / name
        ownership = dict(schema=1, product='DefJamFaithfulHD', recipe_id=ident, png_mips=policy)
        marker = pack / 'builder.json'
        if pack.exists() and (pack.is_symlink() or pack.resolve() != pack.absolute() or not marker.is_file() or json.loads(marker.read_text()) != ownership):
            raise ValueError('HD pack destination is not owned by this recipe: ' + str(pack))
        pack.mkdir(exist_ok=True)
        write_json(marker, ownership)
        textures = pack / 'textures'
        if textures.is_symlink() or textures.resolve() != textures.absolute():
            raise ValueError('HD texture directory must not be a link')
        textures.mkdir(exist_ok=True)
        for source in sorted(set(selected.values())):
            asset = assets[source]
            original = (corpus_root / asset['upscaled']).resolve()
            if not original.is_relative_to(corpus_root) or not re.fullmatch('[0-9a-f]{64}', source):
                raise ValueError('Unsafe corpus image path')
            if file_hash(original) != asset['upscaled_hash']:
                raise ValueError('HD output checksum mismatch: ' + source)
            with Image.open(original) as image:
                if image.size != (asset['width']*4, asset['height']*4) or image.mode != 'RGBA':
                    raise ValueError('HD output dimensions/channels mismatch: ' + source)
            target = textures / (source + '.png')
            if target.is_symlink():
                raise ValueError('HD output must not be a link')
            if not target.exists():
                try:
                    os.link(original, target)
                except OSError:
                    shutil.copyfile(original, target)
            elif file_hash(target) != asset['upscaled_hash']:
                raise ValueError('Existing HD pack file changed: ' + source)
            count += 1
            total += original.stat().st_size
        pending = pack / 'manifest.ini.pending'
        pending.write_text('[pack]\nschema=1\nname=Faithful Lanczos HD\npng_mips=' + policy +
                           '\n[textures]\n' + ''.join(key+'=textures/'+source+'.png\n' for key, source in sorted(selected.items())), encoding='utf-8')
        pending.replace(pack / 'manifest.ini')
        names.append(name)
    return dict(schema=1, recipe_id=ident, packs=names, images=count, bytes=total,
                runtime_aliases=len(aliases or {}), unresolved=corpus['totals']['unresolved'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--mods', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=6)
    parser.add_argument('--aliases', type=Path)
    args = parser.parse_args()
    if not 1 <= args.workers <= 16:
        parser.error('Use 1..16 workers')
    args.work.mkdir(parents=True, exist_ok=True)
    print('Inventorying static textures from your dump', flush=True)
    census = args.work / 'census'
    hd.inventory(SimpleNamespace(root=args.root, output=census, select=['__census_only_no_export__'], limit=0))
    inventory = census / 'inventory.json'
    from PIL import __version__
    fingerprint = hd.digest(b''.join(p.read_bytes() for p in (Path(__file__), Path(hd.__file__), Path(hd_corpus.__file__), inventory)) + __version__.encode())
    # Different code/input recipes never overwrite files hard-linked by older packs.
    output = args.work / fingerprint
    print('Upscaling Lanczos 4x (CPU only; several minutes)', flush=True)
    hd_corpus.build(SimpleNamespace(output=output, inventory=inventory, root=args.root,
                                  selection=None, workers=args.workers))
    aliases = json.loads(args.aliases.read_text())['aliases'] if args.aliases else {}
    print('Verifying and assembling HD packs', flush=True)
    receipt = assemble(output, args.mods, aliases)
    receipt['corpus'] = str(output)
    write_json(args.receipt, receipt)
    print('HD textures ready: '+str(receipt['images'])+' unique images', flush=True)


if __name__ == '__main__':
    main()
