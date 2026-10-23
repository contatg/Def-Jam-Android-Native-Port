#!/usr/bin/env python3
"""Everything the macOS setup payload carries besides the project's own source.

The setup compiles the player's dump on their Mac, so the payload brings what a Mac
with only the Xcode Command Line Tools lacks: Python, CMake, Ninja and the three
libraries the game links (SDL3, MoltenVK, shaderc). Every input is pinned by URL and
SHA-256 (or by git commit) in setup/dependencies-macos.json and, for SDL3, by the
toolkit's own pin. Nothing here touches a dump or game data.

Used by package-setup.py (--platform macos-arm64); run directly to build only the
runtime libraries:  python scripts/macos_payload.py --cache DIR --output PREFIX
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEPLOYMENT_TARGET = '11.0'
PACKAGES = ['pyxbe==1.0.4', 'capstone==5.0.9', 'Pillow==12.3.0', 'numpy==2.5.2']
PYTHON_PRUNE = ('bin/idle3', 'bin/idle3.13', 'bin/pip', 'bin/pip3', 'bin/pip3.13', 'bin/pydoc3', 'bin/pydoc3.13',
                'bin/python3-config', 'bin/python3.13-config', 'bin/python', 'share', 'include', 'lib/pkgconfig',
                'lib/itcl4.3.8', 'lib/tcl9', 'lib/tcl9.0', 'lib/thread3.0.6', 'lib/tk9.0',
                'lib/libtcl9.0.dylib', 'lib/libtcl9tk9.0.dylib')
PYTHON_PRUNE_LIB = ('test', 'idlelib', 'turtledemo', 'ensurepip', 'tkinter', 'lib2to3')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def pins():
    return json.loads((ROOT / 'setup/dependencies-macos.json').read_text(encoding='utf-8'))


def download(url, destination, expected):
    """Fetch once; an existing file is reused only if it still matches the pin."""
    destination = Path(destination)
    if destination.is_file() and digest(destination) == expected:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    pending = destination.with_suffix(destination.suffix + '.download')
    urllib.request.urlretrieve(url, pending)
    if digest(pending) != expected:
        pending.unlink()
        raise ValueError('Download SHA256 mismatch: ' + url)
    pending.replace(destination)
    return destination


def unpack(archive, destination):
    """Unpack an untrusted archive into a fresh folder, refusing traversal and escaping links."""
    destination = Path(destination)
    shutil.rmtree(destination, ignore_errors=True)
    destination.mkdir(parents=True)
    archive = str(archive)
    if archive.endswith(('.zip', '.whl')):
        with zipfile.ZipFile(archive) as package:
            for item in package.infolist():
                name = Path(item.filename)
                if name.is_absolute() or '..' in name.parts:
                    raise ValueError('Unsafe ZIP entry: ' + item.filename)
                if (item.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError('ZIP symlink is unsupported')
            package.extractall(destination)
            for item in package.infolist():
                mode = (item.external_attr >> 16) & 0o777
                if mode and not item.is_dir():
                    (destination / item.filename).chmod(mode)
    else:
        with tarfile.open(archive) as package:
            package.extractall(destination, filter='tar')
            for member in package.getmembers():
                if member.issym() or member.islnk():
                    target = (destination / member.name).parent / member.linkname
                    if destination.resolve() not in target.resolve().parents and target.resolve() != destination.resolve():
                        raise ValueError('Archive link leaves its folder: ' + member.name)


def run(argv, cwd=None, env=None):
    subprocess.run([str(a) for a in argv], cwd=cwd, env=env, check=True)


def sign(path):
    run(['/usr/bin/codesign', '--force', '--sign', '-', path])


def flatten_links(folder):
    """Payload files are regular files: replace every symlink by a copy of its target."""
    folder = Path(folder)
    for path in sorted(folder.rglob('*')):
        if path.is_symlink():
            target = path.resolve()
            if not target.is_file():
                path.unlink()
                continue
            path.unlink()
            shutil.copy2(target, path)


def thin_and_sign(dylib, name):
    """Keep the arm64 slice, name the library @rpath/<name> and sign it ad hoc."""
    dylib = Path(dylib)
    archs = subprocess.run(['/usr/bin/lipo', '-archs', dylib], capture_output=True, text=True, check=True).stdout.split()
    if archs != ['arm64']:
        pending = dylib.with_suffix('.thin')
        run(['/usr/bin/lipo', '-thin', 'arm64', '-output', pending, dylib])
        pending.replace(dylib)
    dylib.chmod(0o755)
    run(['/usr/bin/install_name_tool', '-id', '@rpath/' + name, dylib])
    sign(dylib)


def stage_python(staging, cache):
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
    # Same roots the Windows ._pth names; relative paths in a .pth are taken from site-packages.
    (site / 'defjam-setup.pth').write_text('../../../../source\n../../../../source/scripts\n'
                                           '../../../../source/tools/xboxrecomp\n', encoding='utf-8')
    wheels = cache / 'wheels'
    wheels.mkdir(exist_ok=True)
    expected = pins()['wheels']
    if not all((wheels / name).is_file() and digest(wheels / name) == sha for name, sha in expected.items()):
        run([sys.executable, '-m', 'pip', 'download', '--only-binary=:all:', '--no-deps', '--dest', wheels,
             '--platform', 'macosx_11_0_arm64', '--platform', 'macosx_14_0_arm64', '--platform', 'macosx_15_0_arm64',
             '--python-version', '313', '--implementation', 'cp', '--abi', 'cp313', *PACKAGES])
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


def stage_tools(staging, cache):
    provenance = []
    pin = pins()['cmake']
    archive = download(pin['url'], cache / 'cmake.tar.gz', pin['sha256'])
    work = cache / 'unpacked-cmake'
    unpack(archive, work)
    contents = next(work.glob('*/CMake.app/Contents'))
    tools = staging / 'tools'
    (tools / 'bin').mkdir(parents=True)
    shutil.copy2(contents / 'bin/cmake', tools / 'bin/cmake')
    share = next((contents / 'share').glob('cmake-*'))
    shutil.copytree(share, tools / 'share' / share.name, ignore=shutil.ignore_patterns('Help', '__pycache__'))
    provenance.append({'url': pin['url'], 'sha256': pin['sha256']})
    pin = pins()['ninja']
    archive = download(pin['url'], cache / 'ninja.zip', pin['sha256'])
    unpack(archive, cache / 'unpacked-ninja')
    shutil.copy2(cache / 'unpacked-ninja/ninja', tools / 'bin/ninja')
    provenance.append({'url': pin['url'], 'sha256': pin['sha256']})
    return provenance, tools / 'bin'


def toolkit_pin(name):
    text = (ROOT / 'tools/xboxrecomp/cmake/xbox_sdl3.cmake').read_text(encoding='utf-8')
    version = re.search(r'set\(XBOX_SDL3_VERSION "([^"]+)"', text).group(1)
    sha = re.search(r'set\(XBOX_SDL3_SHA256 "([^"]+)"', text).group(1)
    return version, sha


def build_sdl3(cache, prefix, tools):
    version, sha = toolkit_pin('sdl3')
    url = f'https://github.com/libsdl-org/SDL/releases/download/release-{version}/SDL3-{version}.tar.gz'
    archive = download(url, cache / f'sdl3-{version}.tar.gz', sha)
    source = cache / 'src-sdl3'
    unpack(archive, source)
    folder = next(source.iterdir())
    build = cache / 'build-sdl3'
    shutil.rmtree(build, ignore_errors=True)
    env = tool_env(tools)
    run(['cmake', '-S', folder, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={prefix}',
         '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TESTS=OFF', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_EXAMPLES=OFF',
         '-DCMAKE_OSX_ARCHITECTURES=arm64', f'-DCMAKE_OSX_DEPLOYMENT_TARGET={DEPLOYMENT_TARGET}'], env=env)
    run(['cmake', '--build', build], env=env)
    run(['cmake', '--install', build], env=env)
    (prefix / 'licenses').mkdir(exist_ok=True)
    shutil.copy2(folder / 'LICENSE.txt', prefix / 'licenses/SDL3-zlib.txt')
    return {'url': url, 'sha256': sha}


def build_shaderc(cache, prefix, tools):
    pin = pins()['shaderc']
    source = cache / 'src-shaderc'
    env = tool_env(tools)
    if not (source / '.git').is_dir():
        shutil.rmtree(source, ignore_errors=True)
        run(['git', 'clone', '--quiet', '--branch', pin['tag'], '--depth', '1', pin['url'], source])
    head = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=source, capture_output=True, text=True, check=True).stdout.strip()
    if head != pin['commit']:
        raise ValueError(f'shaderc {pin["tag"]} is {head}, expected {pin["commit"]}')
    # DEPS pins each dependency by commit; git-sync-deps fetches exactly those.
    run([sys.executable, 'utils/git-sync-deps'], cwd=source, env=env)
    build = cache / 'build-shaderc'
    shutil.rmtree(build, ignore_errors=True)
    run(['cmake', '-S', source, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DSHADERC_SKIP_TESTS=ON',
         '-DSHADERC_SKIP_EXAMPLES=ON', '-DSHADERC_SKIP_COPYRIGHT_CHECK=ON', '-DSPIRV_SKIP_EXECUTABLES=ON',
         '-DCMAKE_OSX_ARCHITECTURES=arm64', f'-DCMAKE_OSX_DEPLOYMENT_TARGET={DEPLOYMENT_TARGET}'], env=env)
    run(['cmake', '--build', build, '--target', 'shaderc_shared'], env=env)
    libraries = sorted(build.rglob('libshaderc_shared.*.dylib'))
    if not libraries:
        raise ValueError('shaderc_shared was not built')
    (prefix / 'lib').mkdir(exist_ok=True)
    shutil.copy2(libraries[0], prefix / 'lib/libshaderc_shared.1.dylib')
    thin_and_sign(prefix / 'lib/libshaderc_shared.1.dylib', 'libshaderc_shared.1.dylib')
    shutil.copytree(source / 'libshaderc/include/shaderc', prefix / 'include/shaderc', dirs_exist_ok=True)
    licenses = prefix / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(source / 'LICENSE', licenses / 'shaderc-Apache-2.0.txt')
    for name, relative in (('glslang', 'third_party/glslang/LICENSE.txt'),
                           ('spirv-tools', 'third_party/spirv-tools/LICENSE'),
                           ('spirv-headers', 'third_party/spirv-headers/LICENSE')):
        if (source / relative).is_file():
            shutil.copy2(source / relative, licenses / f'{name}.txt')
    return {'url': pin['url'], 'tag': pin['tag'], 'commit': pin['commit']}


def stage_moltenvk(cache, prefix):
    pin = pins()['moltenvk']
    archive = download(pin['url'], cache / 'MoltenVK-macos.tar', pin['sha256'])
    work = cache / 'unpacked-moltenvk'
    unpack(archive, work)
    root = next(work.glob('MoltenVK/MoltenVK'))
    (prefix / 'lib').mkdir(exist_ok=True)
    shutil.copy2(root / 'dynamic/dylib/macOS/libMoltenVK.dylib', prefix / 'lib/libMoltenVK.dylib')
    thin_and_sign(prefix / 'lib/libMoltenVK.dylib', 'libMoltenVK.dylib')
    for name in ('vulkan', 'vk_video'):
        shutil.copytree(root / 'include' / name, prefix / 'include' / name, dirs_exist_ok=True)
    notice = next((p for p in work.rglob('LICENSE*') if p.is_file()), None)
    (prefix / 'licenses').mkdir(exist_ok=True)
    if notice:
        shutil.copy2(notice, prefix / 'licenses/MoltenVK-Apache-2.0.txt')
    return {'url': pin['url'], 'sha256': pin['sha256']}


def tool_env(tools):
    """A fixed environment: our CMake and Ninja first, then only the system's own tools."""
    env = {'PATH': os.pathsep.join([str(tools), '/usr/bin', '/bin', '/usr/sbin', '/sbin']),
           'HOME': os.environ.get('HOME', ''), 'TMPDIR': os.environ.get('TMPDIR', '/tmp')}
    return env


def build_runtime_prefix(cache, prefix, tools):
    prefix = Path(prefix)
    shutil.rmtree(prefix, ignore_errors=True)
    (prefix / 'lib').mkdir(parents=True)
    (prefix / 'include').mkdir()
    provenance = [build_sdl3(cache, prefix, tools), build_shaderc(cache, prefix, tools), stage_moltenvk(cache, prefix)]
    # Only what the game build and run need: shared libraries, headers and SDL's CMake package.
    flatten_links(prefix)
    library = prefix / 'lib'
    for path in list(library.iterdir()):
        if path.is_file() and (path.suffix == '.a' or path.name == 'libSDL3.dylib'):
            path.unlink()
    for path in list(library.glob('libSDL3.0.*.dylib')):
        path.unlink()
    thin_and_sign(library / 'libSDL3.0.dylib', 'libSDL3.0.dylib')
    for pattern in ('SDL3test*', 'SDL3_test*'):
        for path in library.rglob(pattern):
            path.unlink()
    shutil.rmtree(library / 'pkgconfig', ignore_errors=True)
    shutil.rmtree(prefix / 'share', ignore_errors=True)
    shutil.rmtree(prefix / 'include/SDL3/SDL_test', ignore_errors=True)
    return provenance


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT / 'build/setup-downloads/macos')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/macos-runtime')
    args = parser.parse_args(argv)
    args.cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='macos-tools-', dir=args.cache) as scratch:
        _, tools = stage_tools(Path(scratch), args.cache)
        provenance = build_runtime_prefix(args.cache, args.output, tools)
        (args.output / 'provenance.json').write_text(json.dumps(provenance, indent=2), encoding='utf-8')
        print(json.dumps(provenance, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
