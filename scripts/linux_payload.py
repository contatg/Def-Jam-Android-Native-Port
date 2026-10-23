#!/usr/bin/env python3
"""Everything the Linux setup payload carries besides the project's own source.

The setup compiles the player's dump on their computer, so the payload brings
the Python that runs the wizard, the engine and the lifter (with Tk for the
wizard, the two wheels the pipeline needs and the two the optional HD texture
upscale needs), the wizard itself and the launcher template. It carries no compiler and no libraries: the game is built
with the computer's own toolchain or, where there is none (SteamOS), in a
build container pinned in setup/dependencies-linux.json, and it runs on the
computer's own SDL3, Vulkan loader and shaderc. Every download is pinned by
URL and SHA-256. Nothing here touches a dump or game data.

Used by package-setup.py (--platform linux-x64).
"""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ['pyxbe==1.0.4', 'capstone==5.0.9', 'Pillow==12.3.0', 'numpy==2.5.2']
PYTHON_PRUNE = ('bin/idle3', 'bin/idle3.13', 'bin/pip', 'bin/pip3', 'bin/pip3.13', 'bin/pydoc3', 'bin/pydoc3.13',
                'bin/python3-config', 'bin/python3.13-config', 'bin/python', 'share', 'include', 'lib/pkgconfig')
PYTHON_PRUNE_LIB = ('test', 'idlelib', 'turtledemo', 'ensurepip', 'lib2to3')

_spec = importlib.util.spec_from_file_location('macos_payload', ROOT / 'scripts/macos_payload.py')
_shared = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_shared)
digest, download, unpack, flatten_links = _shared.digest, _shared.download, _shared.unpack, _shared.flatten_links


def pins():
    return json.loads((ROOT / 'setup/dependencies-linux.json').read_text(encoding='utf-8'))


def stage_python(staging, cache):
    """python-build-standalone, pruned, with Tk kept for the wizard and the pipeline's wheels added."""
    pin = pins()['python']
    archive = download(pin['url'], cache / 'python.tar.gz', pin['sha256'])
    work = cache / 'unpacked-python'
    unpack(archive, work)
    python = staging / 'python'
    shutil.copytree(work / 'python', python, symlinks=True)
    for name in PYTHON_PRUNE:
        target = python / name
        if target.is_symlink() or target.is_file():
            target.unlink()
        elif target.is_dir():
            shutil.rmtree(target)
    library = next((python / 'lib').glob('python3.*'))
    for name in PYTHON_PRUNE_LIB:
        shutil.rmtree(library / name, ignore_errors=True)
    for folder in list(python.rglob('__pycache__')):
        shutil.rmtree(folder, ignore_errors=True)
    # python3 -> python3.13: keep one real executable.
    real = python / 'bin' / next(p.name for p in (python / 'bin').iterdir() if re.fullmatch(r'python3\.\d+', p.name))
    link = python / 'bin/python3'
    if link.is_symlink():
        link.unlink()
    real.rename(link)
    flatten_links(python)
    site = library / 'site-packages'
    site.mkdir(exist_ok=True)
    (site / 'defjam-setup.pth').write_text('../../../../source\n../../../../source/scripts\n'
                                           '../../../../source/tools/xboxrecomp\n', encoding='utf-8')
    wheels = cache / 'wheels'
    wheels.mkdir(exist_ok=True)
    expected = pins()['wheels']
    if not all((wheels / name).is_file() and digest(wheels / name) == sha for name, sha in expected.items()):
        subprocess.run([sys.executable, '-m', 'pip', 'download', '--only-binary=:all:', '--no-deps', '--dest',
                        str(wheels), '--platform', 'manylinux2014_x86_64', '--platform', 'manylinux_2_17_x86_64',
                        '--platform', 'manylinux_2_28_x86_64',
                        '--python-version', '313', '--implementation', 'cp', '--abi', 'cp313', *PACKAGES], check=True)
    provenance = []
    for name, sha in expected.items():
        wheel = wheels / name
        if not wheel.is_file() or digest(wheel) != sha:
            raise ValueError('Python dependency checksum mismatch: ' + name)
        unpack(wheel, cache / 'unpacked-wheel')
        shutil.copytree(cache / 'unpacked-wheel', site, dirs_exist_ok=True)
        provenance.append({'package': name, 'sha256': sha})
    provenance.append({'url': pin['url'], 'sha256': pin['sha256']})
    return provenance


def stage(staging, cache):
    """The engine, the wizard, the launcher template and Python."""
    (staging / 'engine').mkdir()
    for name in ('engine.py', 'releases.py', 'transaction.py'):
        shutil.copyfile(ROOT / 'setup' / name, staging / 'engine' / name)
    shutil.copyfile(ROOT / 'setup/linux/wizard.py', staging / 'engine/wizard.py')
    (staging / 'launcher').mkdir()
    shutil.copyfile(ROOT / 'setup/linux/launcher.sh', staging / 'launcher/launcher.sh')
    provenance = stage_python(staging, cache)
    container = pins()['container']
    provenance.append({'container_image': container['image'], 'archive_date': container['archive_date'],
                       'packages': container['packages'], 'note': 'fetched on the player\'s computer only '
                       'when it has no toolchain of its own'})
    return provenance
