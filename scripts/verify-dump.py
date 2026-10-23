#!/usr/bin/env python3
"""Verify a user-supplied Def Jam: Fight for NY (Xbox, USA) dump against config/dump-manifest.json.

Usage: python scripts/verify-dump.py <path-to-extracted-disc-dir>
Exit 0 = OK, 1 = mismatch, 2 = usage error. Never copies or prints game data.
"""
import hashlib, json, os, sys

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()

def verify(game_dir, manifest):
    problems = []
    xbe = manifest["xbe"]
    xbe_path = os.path.join(game_dir, xbe["filename"])
    if not os.path.isfile(xbe_path):
        return [f"missing {xbe['filename']}"]
    size = os.path.getsize(xbe_path)
    if size != xbe["size"]:
        problems.append(f"{xbe['filename']} size {size} != expected {xbe['size']}")
    digest = sha256(xbe_path)
    if digest != xbe["sha256"]:
        problems.append(f"{xbe['filename']} sha256 mismatch (got {digest[:16]}...). Wrong region/version?")
    for rel in manifest["required_paths"]:
        if not os.path.exists(os.path.join(game_dir, rel)):
            problems.append(f"missing {rel}")
    count = sum(len(fs) for _, _, fs in os.walk(game_dir))
    if count < manifest["expected_file_count"]:
        problems.append(f"only {count} files, expected {manifest['expected_file_count']}")
    return problems

def main(argv):
    if len(argv) != 2:
        print(__doc__); return 2
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "config", "dump-manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    problems = verify(argv[1], manifest)
    if problems:
        print("DUMP CHECK FAILED:"); [print("  -", p) for p in problems]; return 1
    print(f"OK: {manifest['title']} ({manifest['title_id_catalog']}) dump verified at {argv[1]}")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
