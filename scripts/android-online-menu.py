#!/usr/bin/env python3
"""Prepare the original ONLINE menu from your own USA dump and deploy over ADB.
No game data is added to the APK, patch ZIP or GitHub repository.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys
import time
import uuid


def load_patcher():
    path = Path(__file__).with_name("android-online-menu-apt.py")
    spec = importlib.util.spec_from_file_location("android_online_apt", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(8 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def prepare(data):
    patcher = load_patcher()
    src = data / "extracted/screens/screens.viv"
    dst = data / "override/screens/screens.viv"
    if not src.is_file():
        raise RuntimeError("Missing extracted/screens/screens.viv in your PC data folder.")
    plan = patcher.Plan(str(src))  # refuses unknown source payloads BEFORE any write
    for line in plan.lines:
        print(line)
    if dst.is_file() and not patcher.compare_override(plan, str(dst)):
        print("Original ONLINE menu override is already current.")
    else:
        if dst.exists():
            backup = dst.with_name(dst.name + ".pre-online110-" + uuid.uuid4().hex[:12])
            shutil.copy2(dst, backup)
            print("Preserved previous local override:", backup)
        patcher.write_override(plan, str(dst))  # verifies written output before atomic rename
        print("Prepared original ONLINE menu from your own dump:", dst)
    return src, dst


def deploy(adb, root, src, dst):
    if not root.startswith("/") or ".." in PurePosixPath(root).parts or not re.fullmatch(r"/[A-Za-z0-9_./-]+", root):
        raise RuntimeError("Invalid Android data root.")
    def run(args, optional=False):
        result = subprocess.run([str(adb), *args], text=True, capture_output=True)
        if result.returncode and not optional:
            raise RuntimeError((result.stderr or result.stdout or "ADB failed").strip())
        return result
    def shell(command, optional=False):
        return run(["shell", command], optional)
    def remote_digest(path, optional=False):
        result = shell("sha256sum " + shlex.quote(path), optional)
        if result.returncode:
            return None
        match = re.match(r"([a-fA-F0-9]{64})\s", result.stdout)
        if not match:
            raise RuntimeError("Cannot verify the phone file: " + path)
        return match.group(1).lower()
    # Replacing menu data while a guest holds archive offsets is unsafe.
    process = shell("pidof org.defjamffny.android:game", optional=True)
    if process.returncode == 0 and process.stdout.strip():
        raise RuntimeError("Close Def Jam completely before deploying the original menu.")
    phone_src = root.rstrip("/") + "/game/screens/screens.viv"
    if remote_digest(phone_src) != digest(src):
        raise RuntimeError("Phone and PC screens.viv differ. Nothing was deployed; use the same original USA dump.")
    folder = root.rstrip("/") + "/override/screens"
    target = folder + "/screens.viv"
    expected = digest(dst)
    if remote_digest(target, optional=True) == expected:
        print("Phone original ONLINE menu is already current.")
        return
    shell("mkdir -p " + shlex.quote(folder))
    tmp = target + ".online110-" + uuid.uuid4().hex[:12] + ".tmp"
    try:
        result = run(["push", str(dst), tmp])
        print(result.stdout.strip())
        if remote_digest(tmp) != expected:
            raise RuntimeError("Phone menu copy failed SHA-256 verification.")
        if remote_digest(target, optional=True):
            backup = target + ".pre-online110-" + str(int(time.time())) + "-" + uuid.uuid4().hex[:8]
            shell("cp " + shlex.quote(target) + " " + shlex.quote(backup))
            print("Preserved previous phone override:", backup)
        shell("mv " + shlex.quote(tmp) + " " + shlex.quote(target))
        if remote_digest(target) != expected:
            raise RuntimeError("Deployed menu checksum does not match.")
        print("Original ONLINE menu installed. Original game files and saves were not modified.")
    finally:
        shell("rm -f " + shlex.quote(tmp), optional=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", required=True)
    parser.add_argument("--adb", help="also deploy to a connected phone after APK installation")
    parser.add_argument("--root", default="/storage/emulated/0/DefJamFFNY")
    args = parser.parse_args()
    try:
        src, dst = prepare(Path(args.data).resolve())
        if args.adb:
            deploy(args.adb, args.root, src, dst)
        return 0
    except Exception as error:
        print("Original menu setup failed: " + str(error), file=sys.stderr)
        print("The Android Online panel remains available; no source hash checks were bypassed.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
