"""Build a deduplicated, local-only Lanczos texture corpus and review server.

No runtime installation: source IDs remain provisional until runtime correlation.
Run build --root OWN_DUMP --inventory inventory.json --output LOCAL_ROOT.
Then serve --output LOCAL_ROOT; review flags persist locally in reviews.json.
"""
import argparse
from collections import Counter, defaultdict, OrderedDict
from concurrent.futures import ProcessPoolExecutor, wait, FIRST_COMPLETED
from functools import lru_cache
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import shutil
import threading
import time
from urllib.parse import urlparse

import hd_assets as hd


@lru_cache(maxsize=16)
def offsets(w, h):
    import numpy as np
    y, x = np.indices((h, w), dtype=np.uint32)
    result = np.zeros((h, w), dtype=np.uint32)
    target = 0
    for bit in range(max(w, h).bit_length()):
        if 1 << bit < w:
            result |= ((x >> bit) & 1) << target
            target += 1
        if 1 << bit < h:
            result |= ((y >> bit) & 1) << target
            target += 1
    return result.reshape(-1)


def fast_unswizzle(data, w, h, bpp):
    import numpy as np
    indices = offsets(w, h)
    if len(data) % bpp or int(indices.max()) >= len(data) // bpp:
        raise ValueError("Unsupported nonrectangular swizzle layout")
    return np.frombuffer(data, dtype=np.uint8).reshape(-1, bpp)[indices].tobytes()


def fast_padded(image, pad, wrap_u=False, wrap_v=False):
    import numpy as np
    from PIL import Image
    array = np.asarray(image)
    if wrap_u != wrap_v:
        raise ValueError("Full corpus audition uses the validated clamp policy")
    axes = ((pad,pad),(pad,pad)) + (((0,0),) if array.ndim == 3 else ())
    return Image.fromarray(np.pad(array,axes,mode="wrap" if wrap_u else "edge"))


def classify(source):
    s = source.lower()
    top = s.split("::")[0]
    member = s.split("::")[-1].replace(chr(92),"/").rsplit("/",1)[-1]
    if s.startswith("screens/"):
        if "/hud" in s or "/tutorial" in s or "/ingame" in s:
            return "HUD and tutorials"
        return "UI and menus"
    if top in ("assets/blingfx.viv", "assets/decalfx.viv"):
        return "Effects and masks"
    if top in ("assets/icrowd.viv", "assets/cincrowd.viv", "assets/v2cm.viv"):
        return "Crowds"
    if top == "movies/load0.xsh" or member in ("nis_logo.xsh", "loader.xsh"):
        return "Loading emblems"
    if top.startswith("assets/cinema/") or top == "assets/texanim.viv":
        return "Animated textures"
    if member.startswith(("v_env", "c_env")):
        return "Environment maps"
    if top == "assets/misc.viv" and member.startswith(("hud", "pause")):
        return "HUD and tutorials"
    if top == "assets/misc.viv" and member.startswith("popups"):
        return "UI and menus"
    group = hd.category(source)
    return {"fighters": "Fighters", "customization": "Customization",
        "venues": "Venues", "props": "Props", "crowd": "Crowds",
        "cinematic-characters": "Cinematic characters",
        "cinematic-environments": "Cinematic environments",
        "logos": "Loading emblems", "effects": "Effects and masks",
        "animation": "Animated textures", "environment-maps": "Environment maps",
        "fonts": "Font atlases", "ui": "UI and menus"}.get(group,
        "Props" if "v2wp" in s or "trophies" in s else "Other textures")


def atomic_save(image, path, **options):
    """Save to a file private to this process, then rename it over the final path."""
    path = Path(path)
    pending = path.with_name(path.name + ".%d.pending" % os.getpid())
    options.setdefault("format", {".png": "PNG", ".jpg": "JPEG"}[path.suffix.lower()])
    image.save(pending, **options)
    pending.replace(path)


def process_leaf(blob, selected, out_text, recipe):
    from PIL import Image
    hd.unswizzle = fast_unswizzle
    hd.padded = fast_padded
    out = Path(out_text)
    completed = []
    wanted = {r["index"]: r for r in selected}
    # Resume validates hashes; never trusts dimensions or timestamps alone.
    active = []
    for record in selected:
        path = out / "records" / (record["source_id"] + ".json")
        # A cancelled older generator may leave a partial cache record. Cached
        # metadata is disposable: rebuild rather than make resume impossible.
        try:
            old = json.loads(path.read_text(encoding="utf8"))
            valid = old["recipe_id"] == recipe["id"] and old["id"] == record["source_id"] and all(
                old[k] == prefix + record["source_id"] + suffix and
                hd.digest((out / old[k]).read_bytes()) == old[k+"_hash"]
                for k, prefix, suffix in (("original", "originals/", ".png"),
                    ("upscaled", "textures/", ".png"), ("thumbnail", "thumbs/", ".jpg")))
        except (OSError, ValueError, KeyError, TypeError):
            old, valid = None, False
        if valid:
            completed.append(old)
        else:
            active.append(record["index"])
    for decoded, original in hd.shpx_images(blob, indices=set(active)):
        request = wanted[decoded["index"]]
        if original is None:
            raise ValueError(request["source"] + ": " + decoded.get("error", "Decode failed"))
        if decoded["source_id"] != request["source_id"]:
            raise ValueError("Inventory/source fingerprint mismatch")
        ident = decoded["source_id"]
        paths = {"original": "originals/"+ident+".png", "upscaled": "textures/"+ident+".png",
                 "thumbnail": "thumbs/"+ident+".jpg"}
        start = time.perf_counter()
        cat = classify(request["source"])
        ui = cat in ("UI and menus", "HUD and tutorials", "Loading emblems", "Font atlases")
        alpha_range = original.getchannel("A").getextrema()
        bleed = ui and alpha_range[0] == 0 and alpha_range[1] > 0
        prepared = hd.bleed_rgb(original) if bleed else original
        colour = hd.padded(prepared.convert("RGB"), 8)
        colour = colour.resize((colour.width*4, colour.height*4), Image.Resampling.LANCZOS)
        colour = colour.crop((32, 32, colour.width-32, colour.height-32))
        alpha = hd.padded(original.getchannel("A"), 8)
        alpha_method = "nearest" if cat == "Effects and masks" else "lanczos"
        alpha = alpha.resize((alpha.width*4, alpha.height*4),
            Image.Resampling.NEAREST if alpha_method == "nearest" else Image.Resampling.LANCZOS)
        colour.putalpha(alpha.crop((32, 32, alpha.width-32, alpha.height-32)))
        # The same image can sit in several containers, so two workers can write one
        # id at once: every write goes to a name of its own and is renamed into place
        # (the content is identical, and a reader never sees a half-written file).
        atomic_save(original, out / paths["original"], compress_level=3)
        # Installed packs may hard-link this file. Replace the path, never mutate
        # a shared inode when repairing a cache or building a changed recipe.
        atomic_save(colour, out / paths["upscaled"], format="PNG", compress_level=3)
        thumb = colour.copy()
        thumb.thumbnail((192, 144), Image.Resampling.LANCZOS)
        bg = Image.new("RGB", (192, 144), (40, 44, 51))
        bg.paste(thumb, ((192-thumb.width)//2, (144-thumb.height)//2), thumb)
        atomic_save(bg, out / paths["thumbnail"], format="JPEG", quality=86)
        result = dict(id=ident, source_id=ident, recipe_id=recipe["id"],
            width=original.width, height=original.height, alpha=list(alpha_range),
            alpha_method=alpha_method, bleed=bleed, category=cat,
            decoded_id=decoded["decoded_id"], pixel_hash=decoded["pixel_hash"],
            seconds=round(time.perf_counter()-start, 4), **paths)
        for k, path in paths.items():
            result[k+"_hash"] = hd.digest((out / path).read_bytes())
        record_path = out / "records" / (ident+".json")
        pending_record = record_path.with_suffix(".%d.pending" % os.getpid())
        pending_record.write_text(json.dumps(result), encoding="utf8")
        pending_record.replace(record_path)
        completed.append(result)
    return completed


def build(args):
    from PIL import __version__ as pillow_version
    out = args.output.resolve()
    for directory in ("originals", "textures", "thumbs", "records"):
        (out / directory).mkdir(parents=True, exist_ok=True)
    inventory = json.loads(args.inventory.read_text(encoding="utf8"))
    records = [r for r in inventory["records"] if r.get("source_id")]
    by_source = defaultdict(list)
    aliases = defaultdict(list)
    for r in records:
        aliases[r["source_id"]].append(dict(source=r["source"], index=r["index"], name=r["name"], category=classify(r["source"])))
    chosen = {}
    for r in records:
        chosen.setdefault(r["source_id"], r)
    for r in chosen.values():
        by_source[r["source"]].append(r)
    recipe = dict(model="Lanczos", scale=4, colour_alpha="separate", padding=8,
        wrap="clamp; material axes not validated", ui_bleed=True,
        alpha="nearest for effects; Lanczos otherwise", png_compression=3,
        pillow=pillow_version, runner_hash=hd.digest(Path(__file__).read_bytes()),
        decoder_hash=hd.digest(Path(hd.__file__).read_bytes()),
        inventory_hash=hd.digest(args.inventory.read_bytes()))
    recipe["id"] = hd.digest(json.dumps(recipe, sort_keys=True).encode())
    (out / "recipe.json").write_text(json.dumps(recipe, indent=2), encoding="utf8")
    start = time.perf_counter()
    completed, pending, seen = [], set(), set()
    def collect(all_pending=False):
        nonlocal pending
        done, pending = wait(pending, return_when=FIRST_COMPLETED) if not all_pending else (pending, set())
        for future in done:
            completed.extend(future.result())
        if len(completed) // 250 != collect.bucket:
            collect.bucket = len(completed) // 250
            print("PROGRESS", len(completed), "/", len(chosen), "elapsed", round(time.perf_counter()-start), "s", flush=True)
            (out / "progress.json").write_text(json.dumps(dict(completed=len(completed), expected=len(chosen), elapsed=time.perf_counter()-start)), encoding="utf8")
    collect.bucket = -1
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        for path in sorted(args.root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in (".viv", ".xsh", ".mad") or path.stat().st_size > hd.MAX_BLOB:
                continue
            top = path.relative_to(args.root).as_posix()
            if not any(s == top or s.startswith(top+"::") for s in by_source):
                continue
            print("CONTAINER", top, flush=True)
            for label, blob in hd.walk(path.read_bytes(), top):
                if label not in by_source:
                    continue
                seen.add(label)
                pending.add(pool.submit(process_leaf, blob, by_source[label], str(out), recipe))
                if len(pending) >= args.workers*2:
                    collect()
        collect(True)
    if seen != set(by_source) or len(completed) != len(chosen):
        raise ValueError("Incomplete source coverage: " + str(set(by_source)-seen))
    known = json.loads(args.selection.read_text())["assets"] if args.selection else []
    labels = {a["source_id"]: a["label"] for a in known}
    assets = []
    for r in completed:
        origin = aliases[r["id"]]
        name = labels.get(r["id"], Path(origin[0]["source"].split("::")[-1]).name+" · "+origin[0]["name"].strip()+" ["+str(origin[0]["index"])+"]")
        assets.append(dict(r, label=name, aliases=origin, categories=sorted({v["category"] for v in origin})))
    unresolved = [r for r in inventory["records"] if r.get("error") and "index" in r]
    for n, r in enumerate(unresolved):
        assets.append(dict(id="unresolved-"+hd.digest((r["source"]+str(r["index"])).encode()), label="Unresolved · "+r["name"].strip(),
            width=r.get("width",0), height=r.get("height",0), category="Unresolved", categories=["Unresolved"],
            error=r["error"], aliases=[dict(source=r["source"],index=r["index"],name=r["name"],category="Unresolved")]))
    assets.sort(key=lambda a: (-a["width"]*a["height"], a["label"], a["id"]))
    metadata = dict(schema=1, corpus_id=recipe["id"], recipe=recipe, assets=assets,
        totals=dict(entries=len(records), unique=len(chosen), unresolved=len(unresolved),
            fonts=inventory["counts"].get("font_entries",0), movies=inventory["counts"].get("movie_entries",0)),
        generation_seconds=round(time.perf_counter()-start,3))
    (out / "corpus.json").write_text(json.dumps(metadata, separators=(",",":")), encoding="utf8")
    shutil.copyfile(Path(__file__).with_name("hd_corpus.html"), out / "index.html")
    (out / "verification.json").write_text(json.dumps(dict(unique=len(completed), source_entries=len(records), unresolved=unresolved,
        decode_ids_matched=True, output_hashes_recorded=True, generation_seconds=metadata["generation_seconds"]),indent=2), encoding="utf8")
    print("READY", len(completed), "textures;", len(unresolved), "unresolved;", metadata["generation_seconds"], "s", flush=True)


def serve(args):
    from PIL import Image
    out = args.output.resolve()
    corpus = json.loads((out / "corpus.json").read_text())
    assets = {a["id"]: a for a in corpus["assets"]}
    lock = threading.Lock()
    review_file = out / "reviews.json"
    reviews = json.loads(review_file.read_text()) if review_file.exists() else dict(schema=1, corpus_id=corpus["corpus_id"], reviews={})
    if reviews["corpus_id"] != corpus["corpus_id"]:
        raise ValueError("Existing review belongs to a different corpus; preserve and inspect")
    cache = OrderedDict()
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(out), **kw)
        def json_response(self, value, status=200):
            payload = json.dumps(value).encode()
            self.send_response(status); self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload))); self.send_header("Cache-Control", "no-store")
            self.end_headers(); self.wfile.write(payload)
        def do_GET(self):
            route = urlparse(self.path).path
            if route == "/api/reviews":
                with lock: self.json_response(reviews)
                return
            if route.startswith("/api/image/"):
                try:
                    _, _, _, ident, which, channel = route.split("/")
                    if which not in ("original", "upscaled") or channel not in ("rgb", "alpha"):
                        raise ValueError("Invalid channel")
                    key = (ident, which, channel)
                    with lock: payload = cache.get(key)
                    if payload is None:
                        asset = assets[ident]
                        with Image.open(out / asset[which]) as im:
                            image = im.convert("RGB") if channel == "rgb" else im.getchannel("A")
                            buffer = io.BytesIO(); image.save(buffer,format="PNG",compress_level=3); payload = buffer.getvalue()
                        with lock:
                            cache[key] = payload
                            while sum(len(v) for v in cache.values()) > 64*1024*1024:
                                cache.popitem(last=False)
                    self.send_response(200); self.send_header("Content-Type","image/png")
                    self.send_header("Content-Length",str(len(payload))); self.end_headers(); self.wfile.write(payload)
                except (KeyError,ValueError,OSError): self.send_error(404)
                return
            super().do_GET()
        def do_POST(self):
            if urlparse(self.path).path != "/api/review":
                self.send_error(404); return
            # Only the local gallery may mutate local review notes.
            if self.headers.get("Origin") not in (None, "http://127.0.0.1:"+str(args.port), "http://localhost:"+str(args.port)):
                self.send_error(403); return
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= 32768: raise ValueError("Invalid review size")
                item = json.loads(self.rfile.read(size))
                ident = item["id"]
                if ident not in assets or item.get("status") not in ("", "concern", "good"):
                    raise ValueError("Invalid texture or review status")
                if item.get("tag","") not in ("", "tattoo", "logo", "material", "alpha", "seam", "other"):
                    raise ValueError("Invalid tag")
                if not isinstance(item.get("notes",""),str) or len(item.get("notes","")) > 4000:
                    raise ValueError("Invalid notes")
                record = dict(id=ident, status=item["status"],tag=item.get("tag",""),notes=item.get("notes",""), updated=time.strftime("%Y-%m-%d %H:%M:%S"))
                with lock:
                    if record["status"] or record["notes"]: reviews["reviews"][ident] = record
                    else: reviews["reviews"].pop(ident,None)
                    temp = review_file.with_suffix(".json.tmp")
                    temp.write_text(json.dumps(reviews,indent=2),encoding="utf8"); temp.replace(review_file)
                self.json_response(dict(saved=True, review=record))
            except (ValueError,KeyError,json.JSONDecodeError): self.json_response(dict(error="Invalid review"),400)
        def log_message(self, *a):
            pass
    print("Serving local corpus at http://127.0.0.1:"+str(args.port),flush=True)
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=("build", "serve")); ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--root", type=Path); ap.add_argument("--inventory",type=Path)
    ap.add_argument("--selection",type=Path); ap.add_argument("--workers",type=int,default=6)
    ap.add_argument("--port",type=int,default=8767)
    args = ap.parse_args()
    if args.action == "build":
        if not args.root or not args.inventory or not 1 <= args.workers <= 12: ap.error("build requires root/inventory and1..12 workers")
        build(args)
    else: serve(args)


if __name__ == "__main__":
    main()
