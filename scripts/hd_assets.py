#!/usr/bin/env python3
"""Local-only FFNY asset census, lossless SHPX export, and reproducible GPU jobs.

Requires Pillow in a separate tools environment. No assets/models enter source.
Archive source identities are provisional until correlated with runtime dumps.
"""
import argparse
from collections import Counter, deque
import hashlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import struct
import subprocess
import time

MAX_BLOB = 128 * 1024 * 1024
MAX_DIM = 4096


def digest(data):
    return hashlib.sha256(data).hexdigest()


def texture_id(fmt, width, height, pixels, palette=b"", row_texels=None):
    """v1: explicit LE descriptor, tightly packed source rows/blocks, palette."""
    return digest(b"XRTEX01\0" + struct.pack("<IIIII", fmt, width, height,
                                           len(palette) // 4, row_texels or width) + pixels + palette)


def archive_entries(blob):
    if blob[:4] not in (b"BIGF", b"BIG4") or len(blob) < 16:
        raise ValueError("Invalid BIG header")
    count = struct.unpack_from(">I", blob, 8)[0]
    if count > 100000:
        raise ValueError("Excessive archive directory")
    pos = 16
    for _ in range(count):
        if pos + 8 > len(blob):
            raise ValueError("Truncated archive directory")
        off, size = struct.unpack_from(">II", blob, pos)
        end = blob.find(b"\0", pos + 8, min(len(blob), pos + 4096))
        if end < 0 or off + size > len(blob):
            raise ValueError("Invalid archive entry")
        yield blob[pos + 8:end].decode("latin1"), blob[off:off + size]
        pos = end + 1


def unpack_refpack(blob):
    # Production limits and strict completeness around the existing decoder.
    sig = int.from_bytes(blob[:2], "big")
    step = 4 if sig & 0x8000 else 3
    pos = 2 + (step if sig & 0x0100 else 0)
    if len(blob) < pos + step:
        raise ValueError("Truncated RefPack header")
    expected = int.from_bytes(blob[pos:pos + step], "big")
    if expected > MAX_BLOB:
        raise ValueError("Excessive RefPack output")
    # The decoder must not grow beyond advertised size. Use a bounded decoder.
    pos += step
    out = bytearray()
    while pos < len(blob):
        b0 = blob[pos]; pos += 1
        if b0 < 0x80:
            b1 = blob[pos]; pos += 1
            run, length, dist = b0 & 3, ((b0 & 0x1c) >> 2) + 3, ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xc0:
            b1, b2 = blob[pos:pos + 2]; pos += 2
            run, length, dist = (b1 >> 6) & 3, (b0 & 0x3f) + 4, ((b1 & 0x3f) << 8) + b2 + 1
        elif b0 < 0xe0:
            b1, b2, b3 = blob[pos:pos + 3]; pos += 3
            run, length, dist = b0 & 3, ((b0 & 0xc) << 6) + b3 + 5, ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        else:
            run, length, dist = (((b0 & 0x1f) << 2) + 4 if b0 < 0xfc else b0 & 3), 0, 0
        if pos + run > len(blob) or len(out) + run + length > expected:
            raise ValueError("RefPack bounds exceeded")
        out.extend(blob[pos:pos + run]); pos += run
        if length and (not dist or dist > len(out)):
            raise ValueError("Invalid RefPack backreference")
        for _ in range(length):
            out.append(out[-dist])
        if b0 >= 0xfc:
            break
    if len(out) != expected:
        raise ValueError("Incomplete RefPack output")
    return bytes(out)


def walk(blob, label, depth=0, selected=None):
    if selected is not None and not any(p == label or p.startswith(label + "::") for p in selected):
        return  # Do not decompress unrelated members during a bounded audition.
    if depth > 12 or len(blob) > MAX_BLOB:
        raise ValueError("Container limit exceeded")
    if len(blob) >= 2 and int.from_bytes(blob[:2], "big") & 0x3eff == 0x10fb:
        yield from walk(unpack_refpack(blob), label, depth + 1, selected)
    elif blob[:4] in (b"BIG4", b"BIGF"):
        for name, data in archive_entries(blob):
            yield from walk(data, label + "::" + name, depth + 1, selected)
    else:
        yield label, blob


def morton_offset(x, y, w, h):
    offset, target, bit = 0, 0, 0
    while (1 << bit) < w or (1 << bit) < h:
        if (1 << bit) < w:
            offset |= ((x >> bit) & 1) << target; target += 1
        if (1 << bit) < h:
            offset |= ((y >> bit) & 1) << target; target += 1
        bit += 1
    return offset


def unswizzle(data, w, h, bpp):
    out = bytearray(w * h * bpp)
    for y in range(h):
        for x in range(w):
            source = morton_offset(x, y, w, h) * bpp
            target = (y * w + x) * bpp
            if source + bpp > len(data):
                raise ValueError("Unsupported nonrectangular swizzle layout")
            out[target:target + bpp] = data[source:source + bpp]
    return bytes(out)


def dxt_image(data, w, h, kind):
    from PIL import Image
    header = [0] * 31
    header[0:7] = [124, 0x81007, h, w, len(data), 0, 1]
    header[18:21] = [32, 4, int.from_bytes({0x60: b"DXT1", 0x61: b"DXT3", 0x62: b"DXT5"}[kind], "little")]
    header[26] = 0x1000
    return Image.open(io.BytesIO(b"DDS " + struct.pack("<31I", *header) + data)).convert("RGBA")


def shpx_images(blob, decode=True, indices=None):
    from PIL import Image
    if len(blob) < 16 or blob[:4] != b"SHPX":
        raise ValueError("Invalid SHPX")
    size, count = struct.unpack_from("<II", blob, 4)
    if size != len(blob) or count > 65536 or 16 + count * 8 > size:
        raise ValueError("Invalid SHPX directory bounds")
    for index in range(count):
        if indices is not None and index not in indices:
            continue
        name, offset = struct.unpack_from("<4sI", blob, 16 + index * 8)
        record = dict(index=index, name=name.decode("latin1"), offset=offset)
        try:
            if offset + 16 > size:
                raise ValueError("Truncated image header")
            kind = blob[offset]
            if kind == 0x2a:  # Palette directory targets are not textures.
                continue
            w, h = struct.unpack_from("<HH", blob, offset + 4)
            flags = struct.unpack_from("<I", blob, offset + 12)[0]
            record.update(type=kind, width=w, height=h, swizzled=bool(flags & 0x2000))
            if not w or not h or max(w, h) > MAX_DIM:
                raise ValueError("Invalid image dimensions")
            if flags >> 28:
                raise ValueError("Mip-bearing SHPX needs validated layout")
            data_offset = offset + (struct.unpack_from("<I", blob, offset + 16)[0] if flags & 0x1000 else 16)
            if data_offset < offset + 16:
                raise ValueError("Invalid indirect image pointer")
            palette = b""
            if kind in (0x60, 0x61, 0x62):
                n = ((w + 3) // 4) * ((h + 3) // 4) * (8 if kind == 0x60 else 16)
                fmt = {0x60: 0x0c, 0x61: 0x0e, 0x62: 0x0f}[kind]
            elif kind in (0x7b, 0x7d):
                n = w * h * (1 if kind == 0x7b else 4)
                fmt = 0x0b if kind == 0x7b else (0x06 if flags & 0x2000 else 0x12)
            else:
                raise ValueError("Unsupported SHPX type 0x%02x" % kind)
            if data_offset + n > size:
                raise ValueError("Truncated pixels")
            pixels = blob[data_offset:data_offset + n]
            if kind == 0x7b:
                pos, seen = offset, set()
                while pos not in seen:
                    seen.add(pos)
                    delta = int.from_bytes(blob[pos + 1:pos + 4], "little")
                    if not delta:
                        break
                    pos += delta
                    if pos + 16 > size or len(seen) > 64:
                        raise ValueError("Invalid attachment chain")
                    if blob[pos] == 0x2a:
                        colors = struct.unpack_from("<H", blob, pos + 4)[0]
                        if not 1 <= colors <= 256 or pos + 16 + colors * 4 > size:
                            raise ValueError("Invalid palette")
                        palette = blob[pos + 16:pos + 16 + colors * 4]
                        break
                if not palette:
                    raise ValueError("No BGRA palette")
                if not decode:
                    record.update(source_id=texture_id(fmt, w, h, pixels, palette), nv2a_format=fmt,
                                  palette_entries=len(palette) // 4, status="inventoried")
                    yield record, None
                    continue
                linear = unswizzle(pixels, w, h, 1) if flags & 0x2000 else pixels
                if max(linear) >= len(palette) // 4:
                    raise ValueError("Palette index out of bounds")
                bgra = b"".join(palette[i * 4:i * 4 + 4] for i in linear)
                image = Image.frombytes("RGBA", (w, h), bgra, "raw", "BGRA")
            elif kind == 0x7d:
                if not decode:
                    record.update(source_id=texture_id(fmt, w, h, pixels), nv2a_format=fmt, status="inventoried")
                    yield record, None
                    continue
                linear = unswizzle(pixels, w, h, 4) if flags & 0x2000 else pixels
                image = Image.frombytes("RGBA", (w, h), linear, "raw", "BGRA")
            else:
                if not decode:
                    record.update(source_id=texture_id(fmt, w, h, pixels), nv2a_format=fmt, status="inventoried")
                    yield record, None
                    continue
                image = dxt_image(pixels, w, h, kind)
            record.update(source_id=texture_id(fmt, w, h, pixels, palette), nv2a_format=fmt,
                          pixel_hash=digest(image.tobytes()), palette_entries=len(palette) // 4,
                          decoded_id=texture_id(0x12,w,h,image.tobytes("raw","BGRA")))
            yield record, image
        except (ValueError, IndexError, struct.error, OSError) as error:
            record["error"] = str(error)
            yield record, None


def category(label):
    name = label.lower()
    # Actual container provenance wins over an asset name embedded in a UI screen.
    top=name.split("::")[0]
    if top.startswith("screens/"):
        return "ui"
    for needle, group in (("load0", "logos"), ("fonts", "fonts"),
                          ("icrowd", "crowd"), ("cincrowd", "crowd"), ("v2cm", "crowd"),
                          ("v2ip_cin", "cinematic-characters"), ("v2ip", "fighters"),
                          ("v2cp", "customization"), ("decalfx", "effects"), ("blingfx", "effects"),
                          ("v2bg_cin", "cinematic-environments"), ("v2bg", "venues"),
                          ("v_env", "environment-maps"), ("props", "props"), ("texanim", "animation"),
                          ("screens", "ui"), ("hud", "ui"), ("pause", "ui")):
        if needle in name:
            return group
    return "review"


def inventory(args):
    out = Path(args.output); out.mkdir(parents=True, exist_ok=True)
    records, counts, saved = [], Counter(), set()
    for path in sorted(Path(args.root).rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".viv", ".xsh", ".mad"):
            continue
        try:
            if path.stat().st_size > MAX_BLOB:
                records.append(dict(source=str(path), error="File exceeds traversal budget")); continue
            for label, blob in walk(path.read_bytes(), path.relative_to(args.root).as_posix()):
                magic = blob[:4]
                if magic == b"SHPX":
                    selected = not args.select or any(s.lower() in label.lower() for s in args.select)
                    for record, image in shpx_images(blob, selected and len(saved) < args.limit):
                        record.update(source=label, category=category(label))
                        counts["images"] += 1
                        if image is not None and selected and len(saved) < args.limit:
                            ident = record["source_id"]
                            destination = out / "originals" / record["category"] / (ident + ".png")
                            destination.parent.mkdir(parents=True, exist_ok=True)
                            image.save(destination)
                            record["export"] = str(destination)
                            saved.add(ident)
                        if "error" in record:
                            counts["unresolved"] += 1
                        records.append(record)
                elif magic in (b"MADk", b"MADm", b"MADe"):
                    counts["movie_entries"] += 1
                    records.append(dict(source=label, kind="video", hash=digest(blob), size=len(blob)))
                elif label.lower().endswith((".ttf", ".xfn")):
                    counts["font_entries"] += 1
                    records.append(dict(source=label, kind="font", hash=digest(blob), size=len(blob)))
        except (ValueError, IndexError, struct.error, SystemExit) as error:
            records.append(dict(source=path.relative_to(args.root).as_posix(), error=str(error)))
    result = dict(schema=1, source_root=str(Path(args.root).resolve()), counts=dict(counts),
                  exported=len(saved), records=records)
    (out / "inventory.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(dict(counts=counts, exported=len(saved))))


def bleed_rgb(image):
    """Nearest opaque colour into zero-alpha texels; alpha remains unchanged."""
    rgba = image.convert("RGBA"); w, h = rgba.size
    pixels = list(rgba.get_flattened_data() if hasattr(rgba,"get_flattened_data") else rgba.getdata())
    known = bytearray(p[3] != 0 for p in pixels)
    queue = deque()
    for i, present in enumerate(known):
        if present:
            x, y = i % w, i // w
            if any(not known[j] for j in (i - 1 if x else i, i + 1 if x + 1 < w else i,
                                          i - w if y else i, i + w if y + 1 < h else i)):
                queue.append(i)
    while queue:
        i = queue.popleft(); x, y = i % w, i // w
        for j in (i - 1 if x else i, i + 1 if x + 1 < w else i,
                  i - w if y else i, i + w if y + 1 < h else i):
            if not known[j]:
                pixels[j] = pixels[i][:3] + (pixels[j][3],)
                known[j] = 1; queue.append(j)
    rgba.putdata(pixels)
    return rgba


def padded(image, pad, wrap_u=False, wrap_v=False):
    from PIL import Image
    w, h = image.size
    result = Image.new(image.mode, (w + 2 * pad, h + 2 * pad))
    result.paste(image, (pad, pad))
    source, dest = image.load(), result.load()
    for y in range(h + 2 * pad):
        for x in range(w + 2 * pad):
            if pad <= x < pad + w and pad <= y < pad + h:
                continue
            u = (x - pad) % w if wrap_u else min(w - 1, max(0, x - pad))
            v = (y - pad) % h if wrap_v else min(h - 1, max(0, y - pad))
            dest[x, y] = source[u, v]
    return result


def upscale(args):
    from PIL import Image
    inputs = sorted(Path(args.input).glob("*.png"))
    out = Path(args.output); textures = out / "textures"; work = out / "work"
    textures.mkdir(parents=True, exist_ok=True); work.mkdir(exist_ok=True)
    tool = Path(args.tool).resolve() if args.tool else None
    model_hashes = {p.name: digest(p.read_bytes()) for p in (tool.parent / "models").glob(args.model + ".*")} if tool else {}
    if args.model != "lanczos" and (not tool or len(model_hashes) != 2):
        raise ValueError("GPU executable/model pair missing")
    recipe = dict(schema=1, model=args.model, model_hashes=model_hashes, scale=args.scale,
                  padding=args.padding, alpha=getattr(args,"alpha","lanczos") + "-separate", bleed=args.bleed,
                  wrap_u=args.wrap_u, wrap_v=args.wrap_v,
                  executable_hash=digest(tool.read_bytes()) if tool else None,
                  runner_hash=digest(Path(__file__).read_bytes()))
    completed, entries = [], []
    prior_path = out / "build.json"
    prior = json.loads(prior_path.read_text()) if prior_path.exists() else {}
    old = {r["id"]: r for r in prior.get("completed", [])} if prior.get("recipe") == recipe else {}
    for path in inputs[:args.limit]:
        ident = path.stem.lower()
        if len(ident) != 64 or any(c not in "0123456789abcdef" for c in ident):
            continue
        original_hash = digest(path.read_bytes()); destination = textures / (ident + ".png")
        if ident in old and old[ident]["source_hash"] == original_hash and destination.exists() and digest(destination.read_bytes()) == old[ident]["output_hash"]:
            completed.append(old[ident]); entries.append((ident, destination.name)); continue
        start = time.perf_counter()
        original = Image.open(path).convert("RGBA")
        prepared = bleed_rgb(original) if args.bleed else original
        rgb = prepared.convert("RGB"); dimensions = tuple(v * args.scale for v in rgb.size)
        if args.model == "lanczos":
            colour = padded(rgb,args.padding,args.wrap_u,args.wrap_v)
            colour = colour.resize(tuple(v*args.scale for v in colour.size),Image.Resampling.LANCZOS)
            p=args.padding*args.scale
            colour = colour.crop((p,p,colour.width-p,colour.height-p))
        else:
            rgb_path, generated = work / (ident + "-rgb.png"), work / (ident + "-4x.png")
            padded(rgb, args.padding, args.wrap_u, args.wrap_v).save(rgb_path)
            command = [str(tool), "-i", str(rgb_path), "-o", str(generated), "-m", str(tool.parent / "models"),
                       "-n", args.model, "-s", "4", "-t", "128", "-j", "1:1:1", "-f", "png"]
            run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
            (work / (ident + ".log")).write_bytes(run.stdout)
            if run.returncode or not generated.exists():
                raise RuntimeError("GPU job failed; inspect " + str(work / (ident + ".log")))
            colour = Image.open(generated).convert("RGB")
            expected = tuple((v + 2 * args.padding) * 4 for v in rgb.size)
            if colour.size != expected:
                raise ValueError("Unexpected GPU output dimensions")
            p = args.padding * 4
            colour = colour.crop((p, p, colour.width - p, colour.height - p))
            if args.scale != 4:
                colour = colour.resize(dimensions, Image.Resampling.LANCZOS)
        alpha = padded(original.getchannel("A"),args.padding,args.wrap_u,args.wrap_v)
        alpha = alpha.resize(tuple(v*args.scale for v in alpha.size),
                             Image.Resampling.NEAREST if getattr(args,"alpha","lanczos")=="nearest" else Image.Resampling.LANCZOS)
        p=args.padding*args.scale
        alpha = alpha.crop((p,p,alpha.width-p,alpha.height-p))
        colour.putalpha(alpha); colour.save(destination)
        completed.append(dict(id=ident, source_hash=original_hash, output_hash=digest(destination.read_bytes()),
                              dimensions=dimensions, seconds=round(time.perf_counter() - start, 3)))
        entries.append((ident, destination.name))
        prior_path.write_text(json.dumps(dict(recipe=recipe, completed=completed), indent=2), encoding="utf-8")
        print(ident[:12], dimensions, completed[-1]["seconds"], "seconds", flush=True)
    (out / "manifest.ini").write_text("[pack]\nschema=1\nname=" + args.model + " pilot\n[textures]\n" +
                                      "".join(i + "=textures/" + n + "\n" for i, n in entries), encoding="utf-8")
    print("Pack entries:", len(entries))


def batch(args):
    """Group observed assets by reviewed recipes; unknown assets stay in a review list."""
    runtime=Path(args.runtime); output=Path(args.output)
    recipes=json.loads(Path(args.recipes).read_text(encoding="utf-8"))
    if recipes.get("schema")!=1:
        raise ValueError("Unsupported recipes schema")
    mip_policy=recipes.get("png_mips","opacity")
    if mip_policy not in ("opacity","channels"):
        raise ValueError("Unsupported PNG mip policy")
    inventory=json.loads(Path(args.inventory).read_text(encoding="utf-8")) if args.inventory else {"records":[]}
    by_id,by_decoded={},{}
    for record in inventory["records"]:
        if "source_id" in record: by_id.setdefault(record["source_id"],[]).append(record)
        if "decoded_id" in record: by_decoded.setdefault(record["decoded_id"],[]).append(record)
    captured={r["id"]:r for line in (runtime/"textures.jsonl").read_text().splitlines() if (r:=json.loads(line))}
    groups,review={},[]
    for ident,record in captured.items():
        if len(ident)!=64 or any(c not in "0123456789abcdef" for c in ident):
            raise ValueError("Invalid observed texture ID")
        candidates=by_id.get(ident,[]) or by_decoded.get(record.get("decoded_id"),[])
        categories={r.get("category","review") for r in candidates}
        category_name=next(iter(categories)) if len(categories)==1 else "review"
        override=recipes.get("overrides",{}).get(ident,{})
        category_name=override.get("category",category_name)
        recipe=dict(recipes.get("defaults",{})); recipe.update(recipes.get("categories",{}).get(category_name,{})); recipe.update(override)
        if not recipe.get("approved",False) or recipe.get("exclude",False):
            review.append(dict(id=ident,category=category_name,sources=[r["source"] for r in candidates])); continue
        if not (runtime/(ident+".png")).exists():
            raise ValueError("Missing runtime original " + ident)
        settings={k:recipe.get(k,v) for k,v in dict(model="lanczos",scale=4,padding=8,alpha="lanczos",bleed=False,wrap_u=False,wrap_v=False).items()}
        if settings["model"] not in ("lanczos","realesrnet-x4plus","realesrgan-x4plus") or settings["scale"] not in (2,4) or not 0<=settings["padding"]<=64 or settings["alpha"] not in ("lanczos","nearest"):
            raise ValueError("Invalid recipe for " + ident)
        key=digest(json.dumps(settings,sort_keys=True).encode())[:16]
        group=groups.setdefault(key,dict(settings=settings,assets=[])); group["assets"].append(ident)
    entries=[]
    for key,group in groups.items():
        inputs=output/"inputs"/key; inputs.mkdir(parents=True,exist_ok=True)
        # Avoid stale jobs from older selections being included in the current manifest.
        for old in inputs.glob("*.png"):
            if old.stem not in group["assets"]: old.unlink()
        for ident in group["assets"]:
            (inputs/(ident+".png")).write_bytes((runtime/(ident+".png")).read_bytes())
        target=output/"jobs"/key
        upscale(SimpleNamespace(input=inputs,output=target,tool=args.tool,limit=len(group["assets"]),**group["settings"]))
        entries.extend((ident,"jobs/"+key+"/textures/"+ident+".png") for ident in group["assets"])
    output.mkdir(parents=True,exist_ok=True)
    (output/"review.json").write_text(json.dumps(review,indent=2),encoding="utf-8")
    temp=output/"manifest.ini.tmp"
    temp.write_text("[pack]\nschema=1\npng_mips="+mip_policy+"\nname=Faithful pilot\n[textures]\n"+"".join(ident+"="+path+"\n" for ident,path in sorted(entries)),encoding="utf-8")
    temp.replace(output/"manifest.ini")
    print(json.dumps(dict(pack_entries=len(entries),review=len(review),recipe_groups=len(groups))))


def export_aux(args):
    """Export raw MAD clips or original fonts to hash-named local files and retain aliases."""
    root,output=Path(args.root),Path(args.output)
    output.mkdir(parents=True,exist_ok=True)
    records,errors,saved=[],[],set()
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".viv",".mad"):
            continue
        if path.stat().st_size>MAX_BLOB:
            errors.append(dict(source=path.relative_to(root).as_posix(),reason="Traversal budget exceeded")); continue
        try:
            for label,blob in walk(path.read_bytes(),path.relative_to(root).as_posix()):
                video=blob[:4] in (b"MADk",b"MADm",b"MADe")
                suffix=Path(label.split("::")[-1]).suffix.lower()
                font=suffix in (".ttf",".xfn")
                if not (video if args.kind=="video" else font): continue
                if args.select and not any(term.lower() in label.lower() for term in args.select): continue
                ident=digest(blob); extension=".mad" if video else suffix
                destination=output/(ident+extension)
                if ident not in saved and len(saved)>=args.limit: continue
                if not destination.exists() or digest(destination.read_bytes())!=ident:
                    destination.write_bytes(blob)
                saved.add(ident)
                records.append(dict(source=label,hash=ident,size=len(blob),file=destination.name))
        except (ValueError,IndexError,struct.error) as error:
            errors.append(dict(source=path.relative_to(root).as_posix(),reason=str(error)))
    (output/"exports.json").write_text(json.dumps(dict(schema=1,kind=args.kind,unique=len(saved),records=records,errors=errors),indent=2),encoding="utf-8")
    print(json.dumps(dict(kind=args.kind,unique=len(saved),aliases=len(records),errors=len(errors))))


def select_assets(args):
    """Export exact archive-chain/directory-index selections with readable review labels."""
    source_root=Path(args.root).resolve(); output=Path(args.output)
    selection=json.loads(Path(args.selection).read_text(encoding="utf-8"))
    if selection.get("schema")!=1 or not 1<=len(selection.get("assets",[]))<=100:
        raise ValueError("Invalid bounded selection")
    output.mkdir(parents=True,exist_ok=True)
    originals=output/"originals"; originals.mkdir(exist_ok=True)
    grouped={}
    for asset in selection["assets"]:
        grouped.setdefault(asset["source"].split("::")[0],[]).append(asset)
    results=[]
    for container,assets in grouped.items():
        path=(source_root/container).resolve()
        if not path.is_relative_to(source_root) or path.stat().st_size>MAX_BLOB:
            raise ValueError("Invalid source container or budget")
        wanted={asset["source"] for asset in assets}; found=set()
        for label,blob in walk(path.read_bytes(),container,selected=wanted):
            if label not in wanted: continue
            selected=[a for a in assets if a["source"]==label]
            decoded={r["index"]:(r,image) for r,image in shpx_images(blob,indices={a["index"] for a in selected})}
            for asset in selected:
                record,image=decoded.get(asset["index"],({},None))
                if image is None: raise ValueError("Selection decode failed: "+label+" "+str(asset["index"])+" "+record.get("error","missing image"))
                destination=originals/(record["source_id"]+".png"); image.save(destination)
                results.append(dict(asset,**{k:v for k,v in record.items() if k not in asset},
                                    original="originals/"+destination.name,
                                    original_hash=digest(destination.read_bytes()),
                                    alpha_extrema=image.getchannel("A").getextrema()))
            found.add(label)
        if found!=wanted: raise ValueError("Missing selected archive entries: "+str(wanted-found))
    order={a["source"]+"::"+str(a["index"]):i for i,a in enumerate(selection["assets"])}
    results.sort(key=lambda r:order[r["source"]+"::"+str(r["index"])])
    for i,record in enumerate(results): record["sample"]="S"+str(i+1).zfill(2)
    (output/"selection.json").write_text(json.dumps(dict(schema=1,assets=results),indent=2),encoding="utf-8")
    print(json.dumps(dict(selected=len(results),unique=len({r['source_id'] for r in results}))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    census = sub.add_parser("inventory")
    census.add_argument("--root", required=True); census.add_argument("--output", required=True)
    census.add_argument("--select", action="append", default=[]); census.add_argument("--limit", type=int, default=50)
    gpu = sub.add_parser("upscale")
    gpu.add_argument("--input", required=True); gpu.add_argument("--output", required=True); gpu.add_argument("--tool")
    gpu.add_argument("--model", default="realesrnet-x4plus")
    gpu.add_argument("--scale", type=int, choices=(2, 4), default=4)
    gpu.add_argument("--padding", type=int, default=8); gpu.add_argument("--limit", type=int, default=50)
    gpu.add_argument("--alpha", choices=("lanczos","nearest"),default="lanczos")
    gpu.add_argument("--bleed", action="store_true"); gpu.add_argument("--wrap-u", action="store_true"); gpu.add_argument("--wrap-v", action="store_true")
    jobs=sub.add_parser("batch")
    jobs.add_argument("--runtime",required=True); jobs.add_argument("--recipes",required=True)
    jobs.add_argument("--output",required=True); jobs.add_argument("--inventory"); jobs.add_argument("--tool")
    aux=sub.add_parser("export")
    aux.add_argument("--root",required=True); aux.add_argument("--output",required=True)
    aux.add_argument("--kind",choices=("video","fonts"),required=True)
    aux.add_argument("--select",action="append",default=[]); aux.add_argument("--limit",type=int,default=3)
    selected=sub.add_parser("select")
    selected.add_argument("--root",required=True); selected.add_argument("--selection",required=True); selected.add_argument("--output",required=True)
    args = parser.parse_args()
    if args.action in ("inventory","upscale","export") and (args.limit < 1 or (args.action == "upscale" and not 0 <= args.padding <= 64)):
        parser.error("limit must be positive and padding must be between 0 and 64")
    {"inventory":inventory,"upscale":upscale,"batch":batch,"export":export_aux,"select":select_assets}[args.action](args)


if __name__ == "__main__":
    main()
