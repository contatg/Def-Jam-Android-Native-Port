#!/usr/bin/env python3
"""Validate the narrow game-free setup release exception, never game binaries."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


engine = load('asset_setup_engine', ROOT / 'setup/engine.py')
package = load('asset_setup_package', ROOT / 'scripts/package-setup.py')


def inspect_payload(archive):
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary)
        package.safe_unpack(archive, folder)
        manifest = engine.verify_payload(folder, platform_of(archive))
        for name in package.HD_SOURCES:
            if 'source/' + name not in manifest['files']:
                raise ValueError('Required HD setup source missing: ' + name)
        for name in manifest['files']:
            if name.startswith('source/setup-deps/'):
                continue  # exact, hashed upstream SDL/ImGui source archives
            if name.startswith('source/'):
                relative = name[len('source/'):]
                toolkit = relative.startswith('tools/xboxrecomp/')
                if toolkit:
                    relative = relative[len('tools/xboxrecomp/'):]
                if relative == 'src/recomp/gen/.gitkeep':
                    continue
                if not package.source_allowed(relative, toolkit):
                    raise ValueError('Disallowed setup source input: ' + name)
            elif manifest['platform'] == 'linux-x64':
                if not name.startswith(('python/', 'engine/', 'licenses/')) \
                        and name not in {'launcher/launcher.sh', 'dependencies.json'}:
                    raise ValueError('Unexpected setup component: ' + name)
            elif manifest['platform'] == 'macos-arm64':
                if not name.startswith(('python/', 'engine/', 'tools/', 'deps/', 'licenses/')) \
                        and not name.startswith('launcher/Def Jam Recompiled.app/') and name != 'dependencies.json':
                    raise ValueError('Unexpected setup component: ' + name)
            elif not name.startswith(('python/', 'engine/')) and name not in {'DefJamLauncher.exe', 'dependencies.json'}:
                raise ValueError('Unexpected setup component: ' + name)
        # Probe the actual embedded runtime without generating new payload files.
        if manifest['platform'] == 'linux-x64':
            if sys.platform.startswith('linux'):
                import subprocess
                python = folder / 'python/bin/python3'
                python.chmod(0o755)   # safe_unpack keeps no modes; the shipped folder does
                subprocess.run([python, '-B', '-c',
                    'import capstone, xbe, tkinter, PIL, numpy, tools.disasm, tools.recomp, tools.xiso; print("Bundled runtime imports OK")'],
                    check=True, env={'PATH': '/usr/bin:/bin'})
                subprocess.run([python, '-B', folder / 'source/scripts/build-hd-pack.py', '--help'], check=True,
                               stdout=subprocess.DEVNULL, env={'PATH': '/usr/bin:/bin'})
        elif manifest['platform'] == 'macos-arm64':
            if sys.platform == 'darwin':
                import subprocess
                python = folder / 'python/bin/python3'
                subprocess.run([python, '-B', '-c',
                    'import capstone, xbe, PIL, numpy, tools.disasm, tools.recomp, tools.xiso; print("Bundled runtime imports OK")'],
                    check=True, env={'PATH': '/usr/bin:/bin'})
                subprocess.run([python, '-B', folder / 'source/scripts/build-hd-pack.py', '--help'], check=True,
                               stdout=subprocess.DEVNULL, env={'PATH': '/usr/bin:/bin'})
                subprocess.run([folder / 'tools/bin/cmake', '--version'], check=True, stdout=subprocess.DEVNULL)
                subprocess.run([folder / 'tools/bin/ninja', '--version'], check=True, stdout=subprocess.DEVNULL)
        elif sys.platform == 'win32':
            import subprocess
            subprocess.run([folder / 'python/python.exe', '-B', '-c',
                'import capstone, xbe, PIL, numpy, tools.disasm, tools.recomp, tools.xiso; print("Embedded runtime imports OK")'], check=True)
            subprocess.run([folder / 'python/python.exe', '-B',
                            folder / 'source/scripts/build-hd-pack.py', '--help'], check=True,
                           stdout=subprocess.DEVNULL)
        return manifest


def platform_of(archive):
    with zipfile.ZipFile(archive) as package_zip:
        return json.loads(package_zip.read('manifest.json'))['platform']


INSTALLER_EXTENSION = {'windows-x64': '.exe', 'macos-arm64': '.dmg', 'linux-x64': '.tar.gz'}


def validate_asset_names(version, names, platform='windows-x64'):
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('Invalid setup release version')
    stem = f'DefJamSetup-{version}-{platform}'
    extension = INSTALLER_EXTENSION[platform]
    expected = {stem + extension, stem + '.sha256', stem + '.provenance.json'}
    if set(names) != expected:
        raise ValueError('Release assets must be exactly the approved setup installer, checksum and provenance')


def verify_embedded_payload(installer, archive):
    """Inspect resource bytes without executing the installer or trusting its name."""
    if sys.platform != 'win32':
        raise ValueError('Setup PE resource inspection requires Windows')
    import ctypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    kernel.LoadLibraryExW.restype = ctypes.c_void_p
    kernel.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = ctypes.c_void_p
    kernel.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.LoadResource.restype = ctypes.c_void_p
    kernel.LockResource.argtypes = [ctypes.c_void_p]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.SizeofResource.restype = ctypes.c_uint32
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    handle = kernel.LoadLibraryExW(str(installer.resolve()), None, 2)  # LOAD_LIBRARY_AS_DATAFILE
    if not handle:
        raise ValueError('Installer is not a readable Windows executable')
    try:
        resource = kernel.FindResourceW(handle, 101, 10)  # RCDATA
        if not resource:
            raise ValueError('Installer payload resource missing')
        size = kernel.SizeofResource(handle, resource)
        pointer = kernel.LockResource(kernel.LoadResource(handle, resource))
        if not pointer or hashlib.sha256(ctypes.string_at(pointer, size)).hexdigest() != engine.digest(archive):
            raise ValueError('Installer resource does not match the inspected payload')
    finally:
        kernel.FreeLibrary(handle)


def verify_embedded_payload_macos(image, archive):
    """Mount the disk image read-only and compare the payload inside the app with the inspected one."""
    import subprocess
    with tempfile.TemporaryDirectory() as mountpoint:
        subprocess.run(['/usr/bin/hdiutil', 'attach', '-readonly', '-nobrowse', '-noverify', '-mountpoint', mountpoint,
                        str(image)], check=True, stdout=subprocess.DEVNULL)
        try:
            bundled = Path(mountpoint) / 'Def Jam Setup.app/Contents/Resources/payload.zip'
            if not bundled.is_file() or engine.digest(bundled) != engine.digest(archive):
                raise ValueError('Installer payload does not match the inspected payload')
        finally:
            subprocess.run(['/usr/bin/hdiutil', 'detach', '-force', mountpoint], check=True, stdout=subprocess.DEVNULL)


def verify_embedded_payload_linux(tarball, archive):
    """Every file under payload/ in the tarball is the inspected ZIP's, byte for byte, and nothing else ships."""
    import tarfile
    with zipfile.ZipFile(archive) as package_zip:
        expected = {name: hashlib.sha256(package_zip.read(name)).hexdigest()
                    for name in package_zip.namelist() if not name.endswith('/')}
    found, top = {}, set()
    with tarfile.open(tarball) as image:
        for member in image.getmembers():
            parts = member.name.split('/')
            if member.issym() or member.islnk() or not (member.isfile() or member.isdir()) or '..' in parts:
                raise ValueError('Unexpected entry in the setup archive: ' + member.name)
            if len(parts) > 1:
                top.add(parts[1])
            if member.isfile() and len(parts) > 2 and parts[1] == 'payload':
                found['/'.join(parts[2:])] = hashlib.sha256(image.extractfile(member).read()).hexdigest()
    if found != expected:
        raise ValueError('Installer payload does not match the inspected payload')
    if top - {'payload', 'DefJamSetup.sh', 'READ ME FIRST.txt', 'Licenses'}:
        raise ValueError('Unexpected files in the setup archive: ' + ', '.join(sorted(top)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--installer', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--development', action='store_true')
    args = parser.parse_args(argv)
    manifest = inspect_payload(args.payload)
    platform = manifest['platform']
    {'macos-arm64': verify_embedded_payload_macos, 'linux-x64': verify_embedded_payload_linux}.get(
        platform, verify_embedded_payload)(args.installer, args.payload)
    if manifest.get('development') and not args.development:
        raise ValueError('A development payload cannot be published')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    stem = f'DefJamSetup-{manifest["version"]}-{platform}'
    import shutil
    exe = args.output_dir / (stem + INSTALLER_EXTENSION[platform])
    shutil.copyfile(args.installer, exe)
    sha = engine.digest(exe)
    (args.output_dir / (stem + '.sha256')).write_text(sha + '  ' + exe.name + '\n', encoding='utf-8')
    (args.output_dir / (stem + '.provenance.json')).write_text(json.dumps(
        {'installer_sha256': sha, 'payload_sha256': engine.digest(args.payload), **manifest}, indent=2) + '\n', encoding='utf-8')
    validate_asset_names(manifest['version'], [p.name for p in args.output_dir.iterdir()], platform)
    print('Verified game-free setup assets:', stem)
    return 0


if __name__ == '__main__':
    sys.exit(main())
