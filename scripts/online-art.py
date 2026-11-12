#!/usr/bin/env python3
# MIT PC online-art extractor adapted for Android deployment, build 118.
"""Local-only: the game's own art and fonts for the online screens, from the player's dump.

The online screens (lobby, room, fighter choice, venue vote) are drawn by the port, in the
game's look: its fonts, fighter portraits, venue thumbnails, the ONLINE poster EA left in
the main menu's assets, backgrounds and button glyphs. This reads them from the player's own
`screens/screens.viv` and `fonts/fonts.viv` and writes PNG and TTF files to
`<data>/online-art/`, which only the player's own copy of the port reads. Nothing here or in
the repository contains game bytes: only archive entry names and image names.

    python scripts/online-art.py [--data <DEFJAM_DATA>] [--check]

Layout of the output: fighters/<id>.png (the choose-character portrait without its padding; `id` is the
fighter.xml ID), venues/<feID>.png (venue.xml feID: its photo with
its sign, 256x256) and venues/<feID>_name.png (the gold name banner), ui/<name>.png, fonts/<file>.ttf,
and manifest.json. Needs Pillow (the installed Python has it for the HD textures).
"""
import argparse
import hashlib
import shlex
import shutil
import subprocess
import uuid
import json
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hd_assets  # noqa: E402

VERSION = 4

# (output name, screens.viv entry, SHPX image name). See docs/research/frontend-menu-integration.md §6.
UI_IMAGES = [
    ("online_poster", "screens/intMain/support/mainMenuButtons.big", "15"),
    ("online_icon", "screens/intMain/support/mainMenuLogosIcons.big", "17"),
    ("button_a", "screens/xHead.big", "15"),
    ("button_y", "screens/xHead.big", "17"),
    ("button_b", "screens/xHead.big", "19"),
    ("button_x", "screens/xHead.big", "21"),
    ("header_strip", "screens/xHead.big", "7"),
    ("menu_wall", "screens/intMain/mainMenu.big", "5"),
    ("menu_door", "screens/intMain/mainMenu.big", "53"),
    ("door_shutter", "screens/intMain/mainMenu.big", "18"),
    ("paper_sign", "screens/options/options.big", "4"),
    ("paper_edge", "screens/options/options.big", "7"),
    ("selector", "screens/battle/cmtype.big", "10"),
    ("label_red", "screens/options/options.big", "26"),
    ("glow", "screens/intMain/support/mainMenuButtons.big", "77"),
]
MATCH_LOGO = "screens/match/2_type/mTypeLogo_%s_e.big"
FONTS = ["TradeBld.ttf", "TradeCon.ttf", "SchwartzHand_Bold2.ttf", "FERRO___.TTF",
         "TradeGothicLTStd-BoldExt.ttf", "AachenStd-Bold.ttf"]


def default_data():
    return os.environ.get("DEFJAM_DATA") or str(Path(__file__).resolve().parents[2] / "data")


def inner_files(entry_blob):
    """The files inside one RefPack-wrapped screen BIGF."""
    return dict(hd_assets.archive_entries(hd_assets.unpack_refpack(entry_blob)))


def images(entry_blob):
    """{SHPX image name: PIL image} of a screen entry's texture bank."""
    out = {}
    for name, data in inner_files(entry_blob).items():
        if name.lower().endswith(".xsh"):
            for record, image in hd_assets.shpx_images(data):
                if image is not None:
                    out[record["name"].strip()] = image
    return out


def trim_padding(image, threshold=3.0):
    """Cut the texture padding off the right and bottom: rows and columns the encoder filled by
    repeating the last real one (they differ from their neighbour by almost nothing)."""
    from PIL import ImageChops, ImageStat
    w, h = image.size
    rgb = image.convert("RGB")

    def column(x):
        return rgb.crop((x, 0, x + 1, h))

    def row(y):
        return rgb.crop((0, y, w, y + 1))

    right = w
    while right > w // 2 and sum(ImageStat.Stat(ImageChops.difference(column(right - 1), column(right - 2))).mean) < threshold:
        right -= 1
    bottom = h
    while bottom > h // 2 and sum(ImageStat.Stat(ImageChops.difference(row(bottom - 1), row(bottom - 2))).mean) < threshold:
        bottom -= 1
    return image.crop((0, 0, right, bottom))


def maybe_refpack(blob):
    return hd_assets.unpack_refpack(blob) if blob[:2] in (b"\x10\xfb", b"\x11\xfb", b"\x90\xfb", b"\x91\xfb") else blob


def build(data, out):
    screens = Path(data) / "extracted" / "screens" / "screens.viv"
    fonts = Path(data) / "extracted" / "fonts" / "fonts.viv"
    if not screens.is_file() or not fonts.is_file():
        raise SystemExit("No game files under %s (expected extracted/screens/screens.viv and fonts/fonts.viv)" % data)
    tmp = out.with_name(out.name + ".tmp-" + uuid.uuid4().hex[:12])
    for sub in ("fighters", "venues", "ui", "fonts", "modes"):
        (tmp / sub).mkdir(parents=True, exist_ok=True)
    (tmp / ".nomedia").write_bytes(b"")
    blob = screens.read_bytes()
    entries = dict(hd_assets.archive_entries(blob))
    manifest = {"version": VERSION, "fighters": [], "venues": [], "ui": [], "modes": [], "fonts": [], "missing": []}

    for name, blob_entry in entries.items():
        m = re.fullmatch(r"screens/portrait/p_CT/p_CT_(\d+)\.big", name)
        if m:      # p_CT_<n> is fighter.xml ID n-1 (the battle roster's FE_ID + 1)
            fid = int(m.group(1)) - 1
            imgs = images(blob_entry)
            if imgs:   # the 128x256 texture holds the portrait and padding to its right and below
                trim_padding(next(iter(imgs.values()))).save(tmp / "fighters" / ("%d.png" % fid))
                manifest["fighters"].append(fid)
            continue
        m = re.fullmatch(r"screens/portrait/(vp|mVen)(\d+)\.big", name)
        if m:      # vp<n>: the venue's photo with its sign (256x256); mVen<n>: its name banner;
            fe = int(m.group(2)) - 1     # both are venue.xml feID n-1
            imgs = images(blob_entry)
            if imgs:
                image = next(iter(imgs.values()))
                if m.group(1) == "vp":
                    image.save(tmp / "venues" / ("%d.png" % fe))
                    manifest["venues"].append(fe)
                else:
                    image.save(tmp / "venues" / ("%d_name.png" % fe))

    cache = {}
    for out_name, entry, image_name in UI_IMAGES:
        if entry not in entries:
            manifest["missing"].append(entry)
            continue
        if entry not in cache:
            cache[entry] = images(entries[entry])
        image = cache[entry].get(image_name)
        if image is None:
            manifest["missing"].append("%s#%s" % (entry, image_name))
            continue
        image.save(tmp / "ui" / (out_name + ".png"))
        manifest["ui"].append(out_name)

    for name in entries:
        m = re.fullmatch(MATCH_LOGO.replace(".", r"\.").replace("%s", r"(\w+?)"), name)
        if m:
            imgs = images(entries[name])
            if imgs:
                # the logo is split into pieces; keep the largest
                best = max(imgs.values(), key=lambda im: im.size[0] * im.size[1])
                best.save(tmp / "modes" / (m.group(1) + ".png"))
                manifest["modes"].append(m.group(1))

    font_entries = dict(hd_assets.archive_entries(fonts.read_bytes()))
    for font in FONTS:
        if font not in font_entries:
            manifest["missing"].append("fonts/" + font)
            continue
        (tmp / "fonts" / font).write_bytes(maybe_refpack(font_entries[font]))
        manifest["fonts"].append(font)

    manifest["fighters"].sort()
    manifest["venues"].sort()
    (tmp / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    if out.exists():
        out.rename(out.with_name(out.name + ".backup-" + uuid.uuid4().hex[:12]))
    tmp.rename(out)
    return manifest



def deploy(adb, data, out, root):
    """Deploy only locally extracted art, with original-disc and copy checks."""
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", root) or ".." in Path(root).parts:
        raise RuntimeError("Invalid Android data root")
    def call(*args, optional=False):
        r = subprocess.run([adb, *args], capture_output=True, text=True)
        if r.returncode and not optional:
            raise RuntimeError((r.stderr or r.stdout or "ADB failed").strip())
        return r
    def shell(command, optional=False):
        return call("shell", command, optional=optional)
    if shell("pidof org.defjamffny.android:game", optional=True).stdout.strip():
        raise RuntimeError("Close Def Jam completely before deploying online art")
    for rel in ("screens/screens.viv", "fonts/fonts.viv"):
        source = Path(data) / "extracted" / rel
        wanted = hashlib.sha256(source.read_bytes()).hexdigest()
        remote = shell("sha256sum " + shlex.quote(root + "/game/" + rel))
        if remote.stdout.split()[0].lower() != wanted:
            raise RuntimeError("Phone and PC original archives differ: " + rel)
    sums = []
    for p in sorted(out.rglob("*")):
        if p.is_file() and p.name != "SHA256SUMS":
            sums.append(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.relative_to(out).as_posix())
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
    stage = root + "/online-art.build118-" + uuid.uuid4().hex[:12]
    target = root + "/online-art"
    backup = target + ".backup-" + uuid.uuid4().hex[:12]
    moved = False
    committed = False
    try:
        shell("touch " + shlex.quote(root + "/.nomedia"))
        call("push", str(out), stage)
        shell("cd " + shlex.quote(stage) + " && sha256sum -c SHA256SUMS")
        exists = shell("test -e " + shlex.quote(target), optional=True)
        if exists.returncode == 0:
            shell("mv " + shlex.quote(target) + " " + shlex.quote(backup))
            moved = True
        shell("mv " + shlex.quote(stage) + " " + shlex.quote(target))
        committed = True
        print("Native online art installed from your own original disc")
        if moved:
            print("Previous online art preserved at " + backup)
    finally:
        if not committed and moved:
            shell("mv " + shlex.quote(backup) + " " + shlex.quote(target))
        shell("rm -rf " + shlex.quote(stage), optional=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--data", default=default_data())
    ap.add_argument("--adb", help="also deploy to a connected phone; no assets are included in the APK")
    ap.add_argument("--root", default="/storage/emulated/0/DefJamFFNY")
    ap.add_argument("--check", action="store_true", help="only say whether the art is there and current")
    args = ap.parse_args()
    out = Path(args.data) / "online-art"
    if args.check:
        try:
            m = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
            ok = m.get("version") == VERSION
        except (OSError, ValueError):
            ok = False
        print("online art: %s" % ("current" if ok else "missing or old"))
        return 0 if ok else 1
    m = build(args.data, out)
    print("online art: %d fighters, %d venues, %d ui images, %d mode logos, %d fonts -> %s"
          % (len(m["fighters"]), len(m["venues"]), len(m["ui"]), len(m["modes"]), len(m["fonts"]), out))
    if m["missing"]:
        print("missing: " + ", ".join(m["missing"]))
    if args.adb:
        deploy(args.adb, args.data, out, args.root.rstrip("/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())

