#!/usr/bin/env python3
"""Render the application icons and the Steam artwork from the committed masters.

    python scripts/make-branding.py render [--out branding]
    python scripts/make-branding.py masters --corpus <decoded-texture-corpus> [--out branding]
    python scripts/make-branding.py fan-tag [--font impact.ttf] [--out branding]

`render` needs only Pillow and NumPy and rebuilds everything under `branding/` from
`branding/masters/` (the shield, the title wordmark, the title-screen scene and its skyline,
and `cover.jpg`, the official cover scan, when present). `masters` rebuilds those four masters from the decoded
textures of a dump: the corpus is the one `scripts/hd_corpus.py` writes (`corpus.json` plus
`originals/`), and the source of every master is recorded in `branding/README.md`.

`render` also makes the "FAN RECOMP" set under `branding/fan/` (the wordmark with a tag under it, on the
logo, the wide and portrait capsules and the social preview) from `masters/fan-tag.png`, which `fan-tag`
draws once from a heavy condensed font so that rendering does not need the font. The Steam installer
prefers `fan/steam/` and falls back to `steam/`; the plain set is kept for future editing.

The artwork is the game's own and stays the property of its rights holders; Vlad approved
committing exactly these files (D98). `scripts/check-source-tree.py` pins them by the SHA-256
of `branding/SHA256SUMS`, which this script writes last.
"""
import argparse
import hashlib
import json
import struct
import sys
import zlib
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageOps

# Decoded textures (ids in the corpus of the user's own dump). See branding/README.md.
SOURCES = {
    'shield': '76ed1cf654f45b086d65ca7f1185c5a25cb62eaf23ba53667f8d77ed6d29b7bb',
    'wordmark': '7454dcc2937c1b2b6a32b3e82813a3e89e2be39dcbead547f4a06af82487e701',
    'sky': 'f3bcc263be615c3347a18a8f071c50f0cbaf07764c05b4235b17663bc64be705',
    'city': 'e2c11109ca2fe998a201ae3a3e236314d8a089fb6a0d7a341b7f839f1a2a31f5',
    'tower': '35be3a123d21c27827d740fb55a543e53ebbf99d908a07adecb867436661db5e',
}
ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 192, 256, 512, 1024)
INK = (16, 15, 14)


# ---------------------------------------------------------------- helpers
def smooth(x, lo, hi):
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def crisp_alpha(alpha, scale, blur, lo=0.38, hi=0.62):
    """Upscale a mostly two-tone alpha channel and re-sharpen its edge: bicubic, blur, smoothstep."""
    big = alpha.resize((alpha.width * scale, alpha.height * scale), Image.BICUBIC)
    big = big.filter(ImageFilter.GaussianBlur(blur))
    x = np.asarray(big, dtype=np.float32) / 255.0
    return Image.fromarray((smooth(x, lo, hi) * 255 + 0.5).astype(np.uint8))


def upscale_premultiplied(im, scale):
    """Lanczos on premultiplied channels, so transparent texels do not bleed into the colour."""
    arr = np.asarray(im.convert('RGBA'), dtype=np.float32) / 255
    al = arr[..., 3:4]
    pm = np.concatenate([arr[..., :3] * al, al], axis=2)
    size = (im.width * scale, im.height * scale)
    big = np.stack([np.asarray(Image.fromarray(pm[..., i]).resize(size, Image.LANCZOS)) for i in range(4)], axis=2)
    a2 = np.clip(big[..., 3], 0, 1)
    rgb = np.clip(np.where(a2[..., None] > 1e-4, big[..., :3] / np.maximum(a2[..., None], 1e-4), 0), 0, 1)
    return rgb, a2


def alpha_bbox(im, threshold=8):
    return im.getchannel('A').point(lambda v: 255 if v > threshold else 0).getbbox()


def load_texture(corpus, name):
    data = json.loads((corpus / 'corpus.json').read_text(encoding='utf-8'))
    wanted = SOURCES[name]
    for asset in data['assets']:
        if asset['id'] == wanted:
            return Image.open(corpus / asset['original']).convert('RGBA')
    raise SystemExit(f'texture {name} ({wanted}) is not in the corpus; is it the USA dump?')


# ---------------------------------------------------------------- masters
def master_shield(src):
    a = np.array(src)
    a[:, 178:, 3] = 0  # a stray strip from the neighbouring sprite
    alpha = crisp_alpha(Image.fromarray(a[..., 3]), 8, 4.5)
    alpha = alpha.crop(alpha.getbbox())
    out = Image.new('RGBA', alpha.size, (255, 255, 255, 0))
    out.putalpha(alpha)
    return out


def master_wordmark(src):
    """4x, premultiplied Lanczos plus unsharp, crisp alpha; the letters are holes in the texture
    (the game draws it on black), so fill them with ink to keep the logo readable on any backdrop."""
    scale = 4
    rgb, a2 = upscale_premultiplied(src, scale)
    rgbi = Image.fromarray((rgb * 255).astype(np.uint8)).filter(ImageFilter.UnsharpMask(radius=2, percent=140, threshold=1))
    ai = Image.fromarray((a2 * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(2.0))
    alpha = Image.fromarray((smooth(np.asarray(ai, dtype=np.float32) / 255, 0.30, 0.70) * 255 + 0.5).astype(np.uint8))
    logo = rgbi.convert('RGBA')
    logo.putalpha(alpha)
    logo = logo.crop(alpha_bbox(logo))
    pad = 8
    canvas = Image.new('RGBA', (logo.width + 2 * pad, logo.height + 2 * pad), (0, 0, 0, 0))
    canvas.paste(logo, (pad, pad))
    solid = canvas.getchannel('A').point(lambda v: 255 if v > 100 else 0)
    sealed = solid.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.MinFilter(7))
    ImageDraw.floodfill(sealed, (0, 0), 128)
    holes = np.asarray(sealed) == 0  # enclosed by the logo
    sol = np.asarray(solid) > 0
    above = np.maximum.accumulate(sol, axis=0)
    below = np.maximum.accumulate(sol[::-1], axis=0)[::-1]
    plate = above & below & ~sol  # the plate behind FIGHT FOR NY: letters open at its edge
    plate[:, :pad + 330] = False
    holes_img = Image.fromarray(((holes | plate) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
    fill = Image.new('RGBA', canvas.size, INK + (255,))
    fill.putalpha(holes_img)
    return Image.alpha_composite(fill, canvas)


def master_scene(corpus):
    base = Image.new('RGBA', (640, 480), (0, 0, 0, 255))
    for name in ('sky', 'city', 'tower'):
        base.alpha_composite(load_texture(corpus, name).crop((0, 0, 640, 480)))
    return base.convert('RGB')


def master_skyline(corpus):
    return load_texture(corpus, 'sky').crop((0, 131, 640, 317)).convert('RGB')


def master_fan_tag(font_path, text='FAN RECOMP', cap=152, tracking=24, seed=5):
    """Distressed silver letters with a dark outline, in the wordmark's own style: rough edge, speckle
    and scratches like its plate, a grainy silver face, and the plate's ink as the outline."""
    font = ImageFont.truetype(str(font_path), round(cap * 1.38))
    widths = [font.getlength(c) for c in text]
    w = round(sum(widths) + tracking * (len(text) - 1)) + 80
    h = round(cap * 1.8)
    mask = Image.new('L', (w, h), 0)
    draw = ImageDraw.Draw(mask)
    x = 40
    for c, cw in zip(text, widths):
        draw.text((x, h * 0.5), c, font=font, fill=255, anchor='lm')
        x += cw + tracking
    rng = np.random.default_rng(seed)

    def noise(sigma, shape=(h, w)):
        raw = Image.fromarray((rng.random(shape) * 255).astype(np.uint8))
        if shape != (h, w):
            raw = raw.resize((w, h), Image.BICUBIC)
        return np.asarray(raw.filter(ImageFilter.GaussianBlur(sigma)), np.float32) / 255

    base = np.asarray(mask.filter(ImageFilter.GaussianBlur(1.6)), np.float32) / 255
    rough = base + (noise(2.2) - 0.5) * 1.1                       # roughen the edge
    letters = smooth(rough, 0.45, 0.58)
    speck = noise(1.0)
    letters *= 1 - (speck > speck.mean() + 2.6 * speck.std())     # speckle holes
    scratch = Image.new('L', (w, h), 0)
    sd = ImageDraw.Draw(scratch)
    for _ in range(9):
        x0, y0 = rng.integers(40, w - 40), rng.integers(10, h - 10)
        length = rng.integers(90, 320)
        angle = np.radians(rng.normal(-6, 9))
        sd.line((x0, y0, x0 + length * np.cos(angle), y0 + length * np.sin(angle)), fill=255, width=int(rng.integers(1, 3)))
    letters *= 1 - np.asarray(scratch.filter(ImageFilter.GaussianBlur(0.5)), np.float32) / 255 * 0.9
    lum = 206 + (noise(0.9) - 0.5) * 70 + (noise(0, (h // 8 + 1, w // 8 + 1)) - 0.5) * 50
    face = Image.fromarray(np.stack([lum, lum, lum - 3], axis=2).clip(0, 255).astype(np.uint8)).convert('RGBA')
    alpha = Image.fromarray((letters * 255).astype(np.uint8))
    face.putalpha(alpha)
    outline = alpha.filter(ImageFilter.MaxFilter(11)).filter(ImageFilter.GaussianBlur(1.2))
    ink = Image.new('RGBA', (w, h), INK + (255,))
    ink.putalpha(outline)
    tag = Image.alpha_composite(ink, face)
    return tag.crop(tag.getchannel('A').getbbox())


def with_fan_tag(wordmark, tag, gap=-10):
    """The wordmark with the tag under it, right-aligned to the plate."""
    canvas = Image.new('RGBA', (wordmark.width, wordmark.height + gap + tag.height), (0, 0, 0, 0))
    canvas.alpha_composite(wordmark, (0, 0))
    canvas.alpha_composite(tag, (wordmark.width - tag.width - 24, wordmark.height + gap))
    return canvas


# ---------------------------------------------------------------- icons
def lerp_gradient(h, w, top, bottom, power=1.6):
    y = np.linspace(0, 1, h, dtype=np.float32)[:, None, None] ** power
    return np.array(top, np.float32) + (np.array(bottom, np.float32) - np.array(top, np.float32)) * y * np.ones((1, w, 1), np.float32)


def icon(shield, n, mac=False):
    """The shield on a dark plate with the glow of the title screen's skyline."""
    ss = 4 if n <= 512 else 2
    big = n * ss
    margin = round(big * (0.098 if mac else 0.02))
    box = big - 2 * margin
    rgb = lerp_gradient(box, box, (27, 24, 22), (70, 44, 14))
    yy, xx = np.mgrid[0:box, 0:box].astype(np.float32) / box
    glow = np.clip(1 - np.hypot((xx - 0.5) * 1.1, (yy - 1.08)) / 0.85, 0, 1) ** 2
    rgb += glow[..., None] * np.array([255, 150, 40], np.float32) * 0.30
    tile = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    mask = Image.new('L', (box, box), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, box - 1, box - 1), radius=round(box * 0.225), fill=255)
    canvas = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    if mac:
        shadow = Image.new('L', (big, big), 0)
        shadow.paste(mask, (margin, margin + round(big * 0.012)))
        shadow = shadow.filter(ImageFilter.GaussianBlur(big * 0.012)).point(lambda v: int(v * 0.4))
        sh = Image.new('RGBA', (big, big), (0, 0, 0, 255))
        sh.putalpha(shadow)
        canvas.alpha_composite(sh)
    plate = tile.convert('RGBA')
    plate.putalpha(mask)
    canvas.alpha_composite(plate, (margin, margin))
    width = round(box * (0.74 if n <= 32 else 0.64))
    mark = shield.resize((width, round(width * shield.height / shield.width)), Image.LANCZOS)
    x = margin + (box - mark.width) // 2
    y = margin + round(box * 0.53) - mark.height // 2
    if n > 32:
        shade = Image.new('RGBA', mark.size, (0, 0, 0, 255))
        shade.putalpha(mark.getchannel('A').point(lambda v: int(v * 0.55)))
        halo = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        halo.paste(shade, (x, y + round(box * 0.012)))
        canvas.alpha_composite(halo.filter(ImageFilter.GaussianBlur(box * 0.015)))
    canvas.alpha_composite(mark, (x, y))
    return canvas.resize((n, n), Image.LANCZOS)


def ico_bytes(frames):
    """A Windows icon: PNG for 256, 32-bit DIB with an AND mask below it (what rc.exe and old shells read)."""
    blobs = []
    for im in frames:
        n = im.width
        if n >= 256:
            buf = BytesIO()
            im.save(buf, 'PNG', optimize=True)
            blobs.append((n, buf.getvalue()))
            continue
        bgra = np.asarray(im.convert('RGBA'))[::-1][..., [2, 1, 0, 3]].tobytes()
        mask = bytes(((n + 31) // 32) * 4 * n)
        header = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, len(bgra) + len(mask), 0, 0, 0, 0)
        blobs.append((n, header + bgra + mask))
    out = struct.pack('<HHH', 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    for n, data in blobs:
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b''.join(data for _, data in blobs)


def icns_bytes(frames):
    kinds = {16: b'icp4', 32: b'icp5', 64: b'icp6', 128: b'ic07', 256: b'ic08', 512: b'ic09', 1024: b'ic10'}
    retina = {32: b'ic11', 64: b'ic12', 256: b'ic13', 512: b'ic14'}  # 16@2x, 32@2x, 128@2x, 256@2x
    body = b''
    for n, kind in list(kinds.items()) + list(retina.items()):
        im = frames.get(n)
        if im is None:
            continue
        buf = BytesIO()
        im.save(buf, 'PNG', optimize=True)
        body += kind + struct.pack('>I', 8 + len(buf.getvalue())) + buf.getvalue()
    return b'icns' + struct.pack('>I', 8 + len(body)) + body


# ---------------------------------------------------------------- Steam art
def grade(rgb, strength=1.0, grain=2.6, seed=7, gain=1.0):
    """Vignette, a little bloom on the lights and film grain; the scene is 640x480, so the grain
    also hides the resampling."""
    h, w = rgb.shape[:2]
    rgb = 255 * (1 - np.exp(-np.asarray(rgb, np.float32) * gain / 255 * 1.6)) / (1 - np.exp(-1.6))  # soft lift, keeps the highlights
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.hypot((xx / w - 0.5) * 1.15, (yy / h - 0.5) * 1.3)
    rgb = rgb * (1 - 0.55 * strength * smooth(d, 0.35, 0.95))[..., None]
    glow = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(max(w, h) / 120))
    rgb = rgb + 0.35 * np.clip(np.asarray(glow, np.float32) - 90, 0, None)
    rng = np.random.default_rng(seed)
    rgb = rgb + rng.normal(0, grain, (h, w, 1))
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))


def scene_layer(scene, scale, size, x0, y0=0, rows=None):
    """The scene scaled up and placed on a black canvas of `size`, with its edge ramps as a mask."""
    big = scene.resize((round(640 * scale), round(480 * scale)), Image.LANCZOS)
    big = big.filter(ImageFilter.UnsharpMask(radius=scale * 0.8, percent=60, threshold=2))
    canvas = Image.new('RGB', size, (0, 0, 0))
    canvas.paste(big, (x0, y0))
    return canvas, big.size


def hero(scene, skyline, size=(3840, 1240)):
    w, h = size
    s = 3.0
    x0 = 1500
    layer, (bw, bh) = scene_layer(scene, s, size, x0)
    sky_big = skyline.resize((round(640 * s), round(186 * s)), Image.LANCZOS).filter(ImageFilter.GaussianBlur(5))
    tile = Image.new('RGB', (w, h), (0, 0, 0))
    y_sky = round(131 * s)
    for i in range(-1, 4):
        t = sky_big if i % 2 == 0 else ImageOps.mirror(sky_big)
        tile.paste(t, (x0 + i * sky_big.width, y_sky))
    xs = np.arange(w, dtype=np.float32)[None, :]
    ys = np.arange(h, dtype=np.float32)[:, None]
    mask = smooth(xs, x0 + 20, x0 + 330) * (1 - smooth(xs, x0 + bw - 520, x0 + bw - 20))
    mask = mask * (1 - smooth(ys, h - 260, h - 20))
    side = 0.25 + 0.75 * smooth(xs, 0, 1100) * (1 - smooth(xs, w - 500, w))
    # the extension keeps the skyline and its glow; the cloud layer above is feathered out so the
    # mirrored tiles do not read as a pattern
    band = smooth(ys, y_sky + 170, y_sky + 400) * (1 - smooth(ys, y_sky + sky_big.height - 120, y_sky + sky_big.height))
    horizon = np.asarray(tile, np.float32) * (1 - mask[..., None]) * side[..., None] * band[..., None]
    rgb = horizon + np.asarray(layer, np.float32) * mask[..., None]
    # keep the left clear and dark for the logo Steam draws there
    rgb = rgb * (1 - 0.6 * (1 - smooth(xs, 0, 1500)))[..., None]
    return grade(rgb, gain=1.45)


def wide(scene, wordmark, size, wordmark_width=0.62, y_center=0.66, crop_top=24):
    w, h = size
    s = w / 640
    big = scene.resize((w, round(480 * s)), Image.LANCZOS).filter(ImageFilter.UnsharpMask(radius=s * 0.8, percent=60, threshold=2))
    rgb = np.asarray(big.crop((0, round(crop_top * s), w, round(crop_top * s) + h)), np.float32)
    ys = np.arange(h, dtype=np.float32)[:, None]
    rgb = rgb * (1 - 0.55 * smooth(ys, h * 0.42, h))[..., None]
    base = grade(rgb, strength=0.8, grain=2.0, seed=11, gain=1.3).convert('RGBA')
    mark = wordmark.resize((round(w * wordmark_width), round(w * wordmark_width * wordmark.height / wordmark.width)), Image.LANCZOS)
    shade = Image.new('RGBA', mark.size, (0, 0, 0, 255))
    shade.putalpha(mark.getchannel('A').point(lambda v: int(v * 0.6)))
    x = (w - mark.width) // 2
    y = round(h * y_center) - mark.height // 2
    halo = Image.new('RGBA', base.size, (0, 0, 0, 0))
    halo.paste(shade, (x, y + round(h * 0.015)))
    base.alpha_composite(halo.filter(ImageFilter.GaussianBlur(h * 0.02)))
    base.alpha_composite(mark, (x, y))
    return base.convert('RGB')


def portrait(scene, wordmark, shield, size=(1200, 1800)):
    w, h = size
    s = 3.75
    big = scene.resize((round(640 * s), round(480 * s)), Image.LANCZOS).filter(ImageFilter.UnsharpMask(radius=3, percent=60, threshold=2))
    rgb = np.asarray(big.crop((0, 0, w, h)), np.float32)
    ys = np.arange(h, dtype=np.float32)[:, None]
    rgb = rgb * (1 - 0.8 * smooth(ys, h * 0.55, h * 0.97))[..., None]
    base = grade(rgb, strength=0.9, grain=2.2, seed=13, gain=1.3).convert('RGBA')
    mark = wordmark.resize((round(w * 0.88), round(w * 0.88 * wordmark.height / wordmark.width)), Image.LANCZOS)
    base.alpha_composite(mark, ((w - mark.width) // 2, round(h * 0.07)))
    sh = shield.resize((round(w * 0.30), round(w * 0.30 * shield.height / shield.width)), Image.LANCZOS)
    base.alpha_composite(sh, ((w - sh.width) // 2, round(h * 0.77)))
    return base.convert('RGB')


def portrait_from_cover(cover, wordmark, size=(600, 900)):
    """The official cover (the scan without its logo), cropped to 2:3, with the wordmark re-drawn
    crisply on top: the scan's own logo is a 660x930 raster, the master is 4x."""
    w, h = size
    keep = round(cover.height * w / h)
    left = (cover.width - keep) // 2
    base = cover.crop((left, 0, left + keep, cover.height)).resize(size, Image.LANCZOS).convert('RGBA')
    mark = wordmark.resize((round(w * 0.9), round(w * 0.9 * wordmark.height / wordmark.width)), Image.LANCZOS)
    x, y = (w - mark.width) // 2, round(h * 0.075)
    shade = Image.new('RGBA', mark.size, (0, 0, 0, 255))
    shade.putalpha(mark.getchannel('A').point(lambda v: int(v * 0.7)))
    halo = Image.new('RGBA', base.size, (0, 0, 0, 0))
    halo.paste(shade, (x, y + 4))
    base.alpha_composite(halo.filter(ImageFilter.GaussianBlur(7)))
    base.alpha_composite(mark, (x, y))
    return base.convert('RGB')


def steam_logo(wordmark):
    return wordmark.resize((1280, round(1280 * wordmark.height / wordmark.width)), Image.LANCZOS)


# ---------------------------------------------------------------- driver
def save_jpeg(im, path, quality=90):
    im.save(path, 'JPEG', quality=quality, subsampling=0, optimize=True, progressive=True)


def render(out):
    masters = out / 'masters'
    shield = Image.open(masters / 'shield.png').convert('RGBA')
    wordmark = Image.open(masters / 'wordmark.png').convert('RGBA')
    scene = Image.open(masters / 'scene.png').convert('RGB')
    skyline = Image.open(masters / 'skyline.png').convert('RGB')
    for sub in ('icon/png', 'icon/macos', 'icon/window', 'steam', 'github'):
        (out / sub).mkdir(parents=True, exist_ok=True)

    frames = {n: icon(shield, n) for n in ICON_SIZES}
    for n, im in frames.items():
        im.save(out / 'icon/png' / f'defjam-{n}.png', optimize=True)
    (out / 'icon/defjam.ico').write_bytes(ico_bytes([frames[n] for n in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)]))
    mac = {n: icon(shield, n, mac=True) for n in (16, 32, 64, 128, 256, 512, 1024)}
    (out / 'icon/macos/defjam.icns').write_bytes(icns_bytes(mac))
    mac[1024].save(out / 'icon/macos/defjam-1024.png', optimize=True)
    # Raw RGBA for the SDL window icon (src/host_posix.c); cmake/app_icon.cmake turns it into a header.
    (out / 'icon/window/window-128.rgba').write_bytes(frames[128].tobytes())
    (out / 'icon/window/window-128-macos.rgba').write_bytes(mac[128].tobytes())

    hero(scene, skyline).resize((1920, 620), Image.LANCZOS).save(out / 'steam/hero.png', optimize=True)
    steam_logo(wordmark).save(out / 'steam/logo.png', optimize=True)
    frames[256].save(out / 'steam/icon.png', optimize=True)
    big_wide = wide(scene, wordmark, (1840, 860))
    big_wide.resize((920, 430), Image.LANCZOS).save(out / 'steam/wide.png', optimize=True)
    cover = masters / 'cover.jpg'  # the official cover scan, optional; without it the portrait is built from the scene
    if cover.exists():
        base = portrait_from_cover(Image.open(cover).convert('RGB'), wordmark)
    else:
        base = portrait(scene, wordmark, shield).resize((600, 900), Image.LANCZOS)
    base.save(out / 'steam/portrait.png', optimize=True)
    save_jpeg(wide(scene, wordmark, (1280, 640), wordmark_width=0.7, y_center=0.5, crop_top=40), out / 'github/social-preview-1280x640.jpg')

    tag_file = masters / 'fan-tag.png'
    if tag_file.exists():
        fan = with_fan_tag(wordmark, Image.open(tag_file).convert('RGBA'))
        for sub in ('fan/steam', 'fan/github'):
            (out / sub).mkdir(parents=True, exist_ok=True)
        steam_logo(fan).save(out / 'fan/steam/logo.png', optimize=True)
        wide(scene, fan, (1840, 860)).resize((920, 430), Image.LANCZOS).save(out / 'fan/steam/wide.png', optimize=True)
        if cover.exists():
            fan_base = portrait_from_cover(Image.open(cover).convert('RGB'), fan)
        else:
            fan_base = portrait(scene, fan, shield).resize((600, 900), Image.LANCZOS)
        fan_base.save(out / 'fan/steam/portrait.png', optimize=True)
        save_jpeg(wide(scene, fan, (1280, 640), wordmark_width=0.7, y_center=0.5, crop_top=40),
                  out / 'fan/github/social-preview-1280x640.jpg')


def write_sums(out):
    lines = []
    for path in sorted(p for p in out.rglob('*') if p.is_file() and p.name not in ('SHA256SUMS', 'README.md')):
        lines.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(out).as_posix()}')
    (out / 'SHA256SUMS').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    print(f'{len(lines)} files; SHA256SUMS sha256 {hashlib.sha256((out / "SHA256SUMS").read_bytes()).hexdigest()}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('render', 'masters', 'fan-tag'))
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parents[1] / 'branding')
    parser.add_argument('--corpus', type=Path)
    parser.add_argument('--font', type=Path, default=Path(r'C:\Windows\Fonts\impact.ttf'),
                        help='fan-tag: a heavy condensed font for the FAN RECOMP tag (default: Impact on Windows)')
    args = parser.parse_args()
    if args.command == 'masters':
        if not args.corpus:
            parser.error('masters needs --corpus')
        masters = args.out / 'masters'
        masters.mkdir(parents=True, exist_ok=True)
        master_shield(load_texture(args.corpus, 'shield')).save(masters / 'shield.png', optimize=True)
        master_wordmark(load_texture(args.corpus, 'wordmark')).save(masters / 'wordmark.png', optimize=True)
        master_scene(args.corpus).save(masters / 'scene.png', optimize=True)
        master_skyline(args.corpus).save(masters / 'skyline.png', optimize=True)
    if args.command == 'fan-tag':
        (args.out / 'masters').mkdir(parents=True, exist_ok=True)
        master_fan_tag(args.font).save(args.out / 'masters/fan-tag.png', optimize=True)
    render(args.out)
    write_sums(args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
