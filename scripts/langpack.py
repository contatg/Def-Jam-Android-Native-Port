#!/usr/bin/env python3
"""Make, check and apply language packs (docs/language-packs.md).

A language pack is a folder with `language.ini` and small binary deltas. Each
delta turns one file, or one entry inside the game's BIG archives, from the
player's own dump into its translated form. The pack carries only the bytes the
translator changed; everything else is rebuilt on the player's computer from
their own dump, which is why a pack can be shared without shipping the game.

    python scripts/langpack.py make ORIGINAL MODIFIED --out PACK [--info INI]
    python scripts/langpack.py check PACK [--game GAME]
    python scripts/langpack.py apply PACK --game GAME --out DIR
    python scripts/langpack.py info PACK

ORIGINAL is the extracted dump (the `game` folder); MODIFIED is a copy of it
with the translator's files in place. `make` finds every changed file and every
changed archive entry (into nested archives), and writes one delta per entry.
`apply` is the reference implementation of what the game does at start-up
(src/hooks/pc_language.c); `check` verifies that every delta's source is in the
dump. Standard library only, so a translator needs nothing but Python 3.8+.
"""

import argparse
import configparser
import hashlib
import os
import re
import struct
import sys

DELTA_MAGIC = b"DJDELTA1"
OP_END, OP_COPY, OP_ADD = 0, 1, 2
MIN_COPY = 12
SCHEMA = 1
# The only XBE sections a pack may change. Code is lifted to C ahead of time, so
# a change there would do nothing; refuse it rather than pretend.
XBE_DATA_SECTIONS = (".rdata", ".data")
TARGET_PART = re.compile(r"^[A-Za-z0-9_.\-]+(/[A-Za-z0-9_.\-]+)*$")
PATCH_PATH = re.compile(r"^[A-Za-z0-9_\-]+(/[A-Za-z0-9_.\-]+)*\.djd$")


class PackError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).digest()


# --- RefPack -----------------------------------------------------------------

def is_refpack(blob):
    return len(blob) >= 5 and (struct.unpack_from(">H", blob, 0)[0] & 0x3EFF) == 0x10FB


def refpack_decompress(src):
    sig = struct.unpack_from(">H", src, 0)[0]
    pos = 2
    step = 4 if sig & 0x8000 else 3
    if sig & 0x0100:
        pos += step
    size = int.from_bytes(src[pos:pos + step], "big")
    pos += step
    out = bytearray()
    while pos < len(src):
        b0 = src[pos]
        pos += 1
        if b0 < 0x80:
            b1 = src[pos]
            pos += 1
            run, length, dist = b0 & 3, ((b0 & 0x1C) >> 2) + 3, ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = src[pos], src[pos + 1]
            pos += 2
            run, length, dist = (b1 >> 6) & 3, (b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = src[pos], src[pos + 1], src[pos + 2]
            pos += 3
            run, length = b0 & 3, ((b0 & 0x0C) << 6) + b3 + 5
            dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            run, length, dist = ((b0 & 0x1F) << 2) + 4, 0, 0
        else:
            run, length, dist = b0 & 3, 0, 0
        out += src[pos:pos + run]
        pos += run
        if length:
            if dist > len(out):
                raise PackError("corrupt RefPack stream")
            for _ in range(length):
                out.append(out[-dist])
        if b0 >= 0xFC:
            break
    # The header's size is the data. A stream may run up to three bytes over it: ZARF's
    # encoder pads its last literal block, and the game reads only `size` bytes.
    if not size <= len(out) <= size + 3:
        raise PackError("RefPack size mismatch: %d, header %d" % (len(out), size))
    return bytes(out[:size])


def refpack_store(data):
    """A RefPack stream of literal blocks only. The game's loader takes it like
    any other; it is what ZARF's own patch writes, tested on a console."""
    n = len(data)
    if n < 1 << 24:
        out = bytearray(b"\x10\xfb") + n.to_bytes(3, "big")
    else:
        out = bytearray(b"\x90\xfb") + n.to_bytes(4, "big")
    pos = 0
    while n - pos >= 4:
        run = min(112, (n - pos) & ~3)
        out.append(0xE0 + ((run - 4) >> 2))
        out += data[pos:pos + run]
        pos += run
    out.append(0xFC + (n - pos))
    out += data[pos:]
    return bytes(out)


# --- BIG archives ------------------------------------------------------------

def is_big(blob):
    return blob[:4] in (b"BIGF", b"BIG4") and len(blob) >= 16


def big_entries(blob):
    """[(name, offset, size, directory position)] in directory order."""
    count = struct.unpack_from(">I", blob, 8)[0]
    pos, out = 16, []
    for _ in range(count):
        off, size = struct.unpack_from(">II", blob, pos)
        end = blob.index(b"\0", pos + 8)
        out.append((blob[pos + 8:end].decode("latin-1"), off, size, pos))
        if off + size > len(blob):
            raise PackError("archive entry %r runs past the end" % out[-1][0])
        pos = end + 1
    return out


def big_alignment(entries):
    align = 4096
    while align > 1 and any(off % align for _, off, _, _ in entries):
        align //= 2
    return align


def big_find(entries, name):
    folded = name.lower()
    for entry in entries:
        if entry[0].lower() == folded:
            return entry
    raise PackError("no entry %r" % name)


def big_replace(blob, replacements):
    """Append each replaced entry and point the directory at it; every other
    entry keeps its bytes and its offset."""
    entries = big_entries(blob)
    align = big_alignment(entries)
    out = bytearray(blob)
    for entry in entries:
        if entry[0] not in replacements:
            continue
        data = replacements[entry[0]]
        out += b"\0" * (-len(out) % align)
        struct.pack_into(">II", out, entry[3], len(out), len(data))
        out += data
    struct.pack_into("<I", out, 4, len(out))
    return bytes(out)


def unpack_entry(raw):
    return refpack_decompress(raw) if is_refpack(raw) else raw


def repack_entry(original_raw, data):
    return refpack_store(data) if is_refpack(original_raw) else data


# --- Deltas ------------------------------------------------------------------

def delta_make(source, target):
    ops = []

    def add(chunk):
        if ops and ops[-1][0] == OP_ADD:
            ops[-1] = (OP_ADD, ops[-1][1] + chunk)
        else:
            ops.append((OP_ADD, chunk))

    if len(source) == len(target):
        # Same size: an in-place edit (textures, fixed records). Small equal gaps
        # go into the added run, which is cheaper than another copy op.
        i, n = 0, len(target)
        while i < n:
            j = i
            while j < n and source[j] == target[j]:
                j += 1
            if j - i >= MIN_COPY or j == n:
                if j > i:
                    ops.append((OP_COPY, i, j - i))
            else:
                add(target[i:j])
            i = j
            while j < n and source[j] != target[j]:
                j += 1
            if j > i:
                add(target[i:j])
            i = j
    else:
        key = 8
        index = {}
        for off in range(len(source) - key + 1):
            index.setdefault(source[off:off + key], []).append(off)
        i, n, last = 0, len(target), -1
        while i < n:
            best_off, best_len = -1, 0
            candidates = index.get(target[i:i + key], [])
            if 0 <= last < len(source):
                candidates = [last] + candidates[:64]
            else:
                candidates = candidates[:64]
            for off in candidates:
                k = 0
                while i + k < n and off + k < len(source) and source[off + k] == target[i + k]:
                    k += 1
                if k > best_len:
                    best_off, best_len = off, k
            if best_len >= MIN_COPY:
                ops.append((OP_COPY, best_off, best_len))
                i += best_len
                last = best_off + best_len
            else:
                add(target[i:i + 1])
                i += 1
                if last >= 0:
                    last += 1
    out = bytearray(DELTA_MAGIC)
    out += struct.pack("<II", len(source), len(target)) + sha256(source) + sha256(target)
    for op in ops:
        if op[0] == OP_COPY:
            out += struct.pack("<BII", OP_COPY, op[1], op[2])
        else:
            out += struct.pack("<BI", OP_ADD, len(op[1])) + op[1]
    out.append(OP_END)
    return bytes(out)


def delta_header(delta):
    if len(delta) < 81 or delta[:8] != DELTA_MAGIC:
        raise PackError("not a language pack delta")
    src_size, dst_size = struct.unpack_from("<II", delta, 8)
    return src_size, dst_size, delta[16:48], delta[48:80]


def delta_apply(delta, source):
    src_size, dst_size, src_hash, dst_hash = delta_header(delta)
    if len(source) != src_size or sha256(source) != src_hash:
        raise PackError("the delta was made for a different original")
    out = bytearray()
    pos = 80
    while True:
        if pos >= len(delta):
            raise PackError("delta has no end marker")
        op = delta[pos]
        pos += 1
        if op == OP_END:
            break
        if op == OP_COPY:
            off, length = struct.unpack_from("<II", delta, pos)
            pos += 8
            if off + length > len(source):
                raise PackError("delta copies past the original")
            out += source[off:off + length]
        elif op == OP_ADD:
            (length,) = struct.unpack_from("<I", delta, pos)
            pos += 4
            if pos + length > len(delta):
                raise PackError("delta data runs past its end")
            out += delta[pos:pos + length]
            pos += length
        else:
            raise PackError("unknown delta op %d" % op)
        if len(out) > dst_size:
            raise PackError("delta writes past its stated size")
    if len(out) != dst_size or sha256(out) != dst_hash:
        raise PackError("delta result does not match its stated hash")
    return bytes(out)


def delta_stats(delta):
    pos, copied, added = 80, 0, 0
    while delta[pos] != OP_END:
        if delta[pos] == OP_COPY:
            copied += struct.unpack_from("<I", delta, pos + 5)[0]
            pos += 9
        else:
            length = struct.unpack_from("<I", delta, pos + 1)[0]
            added += length
            pos += 5 + length
    return copied, added


# --- XBE ---------------------------------------------------------------------

def xbe_sections(xbe):
    base = struct.unpack_from("<I", xbe, 0x104)[0]
    count = struct.unpack_from("<I", xbe, 0x11C)[0]
    headers = struct.unpack_from("<I", xbe, 0x120)[0] - base
    out = []
    for i in range(count):
        _, _, _, raw, raw_size, name_va = struct.unpack_from("<IIIIII", xbe, headers + i * 56)
        name_at = name_va - base
        out.append((xbe[name_at:xbe.index(b"\0", name_at)].decode("latin-1"), raw, raw_size))
    return out


def xbe_check_data_only(original, patched):
    if len(original) != len(patched):
        raise PackError("default.xbe must keep its size")
    allowed = [(raw, raw + size) for name, raw, size in xbe_sections(original) if name in XBE_DATA_SECTIONS]
    for i, (a, b) in enumerate(zip(original, patched)):
        if a != b and not any(lo <= i < hi for lo, hi in allowed):
            raise PackError("default.xbe changes byte 0x%X outside %s; code changes need a "
                            "runtime patch, not a pack" % (i, "/".join(XBE_DATA_SECTIONS)))


# --- Pack files --------------------------------------------------------------

def parse_target(target):
    parts = [p.strip() for p in target.split("|")]
    if not parts or not all(TARGET_PART.match(p) and ".." not in p.split("/") for p in parts):
        raise PackError("bad target %r" % target)
    return parts


def read_pack(folder):
    ini = configparser.ConfigParser(interpolation=None, comment_prefixes=(";", "#"),
                                    inline_comment_prefixes=None, strict=True)
    ini.optionxform = str
    path = os.path.join(folder, "language.ini")
    with open(path, encoding="utf-8-sig") as handle:
        ini.read_file(handle)
    if not ini.has_section("language") or ini.get("language", "schema", fallback="") != str(SCHEMA):
        raise PackError("%s: [language] schema=%d is required" % (path, SCHEMA))
    for key in ("name", "language"):
        if not ini.get("language", key, fallback="").strip():
            raise PackError("%s: [language] %s is required" % (path, key))
    files = []
    for target, patch in (ini.items("files") if ini.has_section("files") else []):
        patch = patch.strip()
        if not PATCH_PATH.match(patch) or ".." in patch.split("/"):
            raise PackError("bad delta path %r" % patch)
        files.append((parse_target(target), patch))
    if not files:
        raise PackError("%s: [files] lists nothing" % path)
    seen = set()
    for parts, _ in files:
        key = "|".join(parts).lower()
        if key in seen:
            raise PackError("target listed twice: %s" % key)
        seen.add(key)
    return dict(ini.items("language")), files


def find_file(root, relative):
    """Case-insensitive, as the Xbox file system is."""
    path = root
    for part in relative.split("/"):
        exact = os.path.join(path, part)
        if os.path.exists(exact):
            path = exact
            continue
        try:
            match = [n for n in os.listdir(path) if n.lower() == part.lower()]
        except OSError:
            match = []
        if not match:
            raise PackError("not in the game folder: %s" % relative)
        path = os.path.join(path, match[0])
    return path


def rebuild(blob, edits):
    """edits: [(entry path parts, delta bytes)] relative to this blob."""
    whole = [d for parts, d in edits if not parts]
    if whole:
        if len(edits) != 1:
            raise PackError("a target and an entry inside it are both listed")
        return delta_apply(whole[0], blob)
    if not is_big(blob):
        raise PackError("not an archive, but entries inside it are listed")
    entries = big_entries(blob)
    grouped = {}
    for parts, d in edits:
        name = big_find(entries, parts[0])[0]
        grouped.setdefault(name, []).append((parts[1:], d))
    replacements = {}
    for name, sub in grouped.items():
        _, off, size, _ = big_find(entries, name)
        raw = blob[off:off + size]
        replacements[name] = repack_entry(raw, rebuild(unpack_entry(raw), sub))
    return big_replace(blob, replacements)


def apply_pack(folder, game):
    """{relative path: rebuilt bytes}"""
    _, files = read_pack(folder)
    by_file = {}
    for parts, patch in files:
        with open(os.path.join(folder, patch), "rb") as handle:
            by_file.setdefault(parts[0].lower(), (parts[0], []))[1].append((parts[1:], handle.read()))
    out = {}
    for _, (relative, edits) in sorted(by_file.items()):
        with open(find_file(game, relative), "rb") as handle:
            original = handle.read()
        out[relative] = rebuild(original, edits)
        if relative.lower() == "default.xbe":
            xbe_check_data_only(original, out[relative])
    return out


# --- make --------------------------------------------------------------------

def diff_blobs(original, modified, prefix, found):
    if original == modified:
        return
    if is_big(original) and is_big(modified):
        a = {e[0].lower(): e for e in big_entries(original)}
        b = {e[0].lower(): e for e in big_entries(modified)}
        if set(a) != set(b):
            raise PackError("%s: entries added or removed (%s); only changes to existing entries "
                            "are supported" % ("|".join(prefix), ", ".join(sorted(set(a) ^ set(b)))))
        for key in sorted(a):
            name, off, size, _ = a[key]
            _, off2, size2, _ = b[key]
            diff_blobs(unpack_entry(original[off:off + size]), unpack_entry(modified[off2:off2 + size2]),
                       prefix + [name], found)
        return
    found.append((prefix, original, modified))


def make(args):
    found = []
    for base, _, names in os.walk(args.modified):
        for name in names:
            path = os.path.join(base, name)
            relative = os.path.relpath(path, args.modified).replace(os.sep, "/")
            with open(path, "rb") as handle:
                modified = handle.read()
            try:
                with open(find_file(args.original, relative), "rb") as handle:
                    original = handle.read()
            except PackError:
                raise PackError("new file %s: only changes to the game's own files are supported" % relative)
            if relative.lower() == "default.xbe" and original != modified:
                xbe_check_data_only(original, modified)
            diff_blobs(original, modified, [relative], found)
    if not found:
        raise PackError("no differences found")
    os.makedirs(os.path.join(args.out, "patches"), exist_ok=True)
    lines = []
    total = 0
    for i, (parts, original, modified) in enumerate(sorted(found, key=lambda f: [p.lower() for p in f[0]])):
        stem = re.sub(r"[^A-Za-z0-9_.\-]", "_", parts[-1].rsplit("/", 1)[-1])
        patch = "patches/%03d-%s.djd" % (i + 1, stem)
        delta = delta_make(original, modified)
        delta_apply(delta, original)
        with open(os.path.join(args.out, patch), "wb") as handle:
            handle.write(delta)
        copied, added = delta_stats(delta)
        total += len(delta)
        print("%-70s %8d -> %8d bytes, delta %8d (%d new)" % ("|".join(parts), len(original),
                                                            len(modified), len(delta), added))
        lines.append("%s = %s" % ("|".join(parts), patch))
    header = DEFAULT_INFO
    if args.info:
        with open(args.info, encoding="utf-8-sig") as handle:
            header = handle.read().rstrip() + "\n"
        if re.search(r"^\[files\]", header, re.M):
            raise PackError("--info must not have a [files] section; make writes it")
    with open(os.path.join(args.out, "language.ini"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(header + "\n[files]\n" + "\n".join(lines) + "\n")
    print("%d deltas, %d bytes, in %s" % (len(lines), total, args.out))
    read_pack(args.out)


DEFAULT_INFO = """[language]
schema=1
name=Language name, in that language
language=xx
version=1.0.0
authors=
licence=
homepage=
; all: subtitles in every Story cutscene (a translation needs this); game: as the game decides
story_subtitles=all
"""


# --- check / apply / info ----------------------------------------------------

def check(args):
    info, files = read_pack(args.pack)
    for _, patch in files:
        with open(os.path.join(args.pack, patch), "rb") as handle:
            delta = handle.read()
        delta_header(delta)
    if args.game:
        out = apply_pack(args.pack, args.game)
        print("ok: %d deltas apply to %s, %d files rebuilt" % (len(files), args.game, len(out)))
    else:
        print("ok: %s (%s), %d deltas" % (info["name"], info["language"], len(files)))


def apply(args):
    for relative, data in apply_pack(args.pack, args.game).items():
        path = os.path.join(args.out, *relative.split("/"))
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)
        print("%s: %d bytes, sha256 %s" % (relative, len(data), hashlib.sha256(data).hexdigest()))


def info(args):
    meta, files = read_pack(args.pack)
    for key, value in meta.items():
        print("%s: %s" % (key, value))
    for parts, patch in files:
        with open(os.path.join(args.pack, patch), "rb") as handle:
            delta = handle.read()
        src_size, dst_size, _, _ = delta_header(delta)
        copied, added = delta_stats(delta)
        print("  %s: %d -> %d bytes (%d copied, %d new)" % ("|".join(parts), src_size, dst_size, copied, added))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("make", help="write a pack from an original and a modified game folder")
    p.add_argument("original")
    p.add_argument("modified")
    p.add_argument("--out", required=True)
    p.add_argument("--info", help="a [language] section to use instead of the template")
    p.set_defaults(func=make)
    p = sub.add_parser("check", help="validate a pack, and with --game that it applies")
    p.add_argument("pack")
    p.add_argument("--game")
    p.set_defaults(func=check)
    p = sub.add_parser("apply", help="write the rebuilt files (what the game does at start-up)")
    p.add_argument("pack")
    p.add_argument("--game", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=apply)
    p = sub.add_parser("info", help="describe a pack")
    p.add_argument("pack")
    p.set_defaults(func=info)
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except (PackError, OSError, configparser.Error) as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
