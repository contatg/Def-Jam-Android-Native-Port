# SPDX-License-Identifier: GPL-3.0-or-later
"""Isolate hakuX's SDL3 inside the Def Jam APK (no original files changed).

The replacement strings have exactly the same length. SDL's JNI_OnLoad uses
FindClass strings, and DT_SONAME/DT_NEEDED use dynamic-string-table entries;
no dynamic-symbol names or symbol hashes are changed. Original binaries are
the ARM64 hakuX 1.3.0 release supplied with the source bundle.
"""
import argparse
import hashlib
from pathlib import Path

EXPECTED = {
    'libSDL3.so': '303899002f1891317c74e7f3a71cc78cbdaae88b46055cba241c73d5b24c65ff',
    'libhakuxcore.so': '45e4968858b5fcd07fd15b5b052fdacb3a9f046ffd5279265fae204138763f7b',
}


def replace_exact(data, old, new, count):
    if len(old) != len(new) or data.count(old) != count:
        raise ValueError('Unexpected input: %r appears %d times, expected %d' % (old, data.count(old), count))
    return data.replace(old, new)


def check_elf(data):
    if data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != 183:
        raise ValueError('Not an Android ARM64 ELF binary')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--input', required=True, type=Path, help='Directory with the original ARM64 libraries')
    p.add_argument('--output', required=True, type=Path, help='Directory for rewritten copies')
    p.add_argument('--allow-rebuilt', action='store_true', help='Permit your own rebuilt hakuX libraries')
    a = p.parse_args()
    result = {}
    for name in ('libSDL3.so', 'libhakuxcore.so'):
        data = (a.input / name).read_bytes()
        check_elf(data)
        digest = hashlib.sha256(data).hexdigest()
        if not a.allow_rebuilt and digest != EXPECTED[name]:
            raise ValueError('Original library hash differs: %s' % name)
        data = replace_exact(data, b'libSDL3.so', b'libhSD3.so', 1)
        if name == 'libSDL3.so':
            data = replace_exact(data, b'org/libsdl/app/', b'org/libsdl/hak/', 5)
            name = 'libhSD3.so'
        result[name] = data
    a.output.mkdir(parents=True, exist_ok=True)
    for name, data in result.items():
        (a.output / name).write_bytes(data)
        print(name, hashlib.sha256(data).hexdigest())


if __name__ == '__main__':
    main()
