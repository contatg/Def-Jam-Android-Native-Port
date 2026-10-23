"""Audition official SwinIR classical x4 on an existing local 40-texture selection.

Uses the official network with strict checkpoint loading and FP32 CUDA inference.
Keeps RGB/alpha separate and renders a new gallery without changing prior reviews.
2x is reduced from x4 colour, not a native x2 checkpoint. Game media stays local.
"""
import argparse
import copy
import html as html_module
import importlib.metadata
import json
import subprocess
import sys
import time
from pathlib import Path

import hd_assets as hd
import hd_review

MODEL = "swinir-classical-x4"
CHECKPOINT = "001_classicalSR_DIV2K_s48w8_SwinIR-M_x4.pth"


def generate(root, output, source, checkpoint):
    import numpy as np
    import torch
    from PIL import Image

    if not torch.cuda.is_available():
        raise RuntimeError("This audition requires local CUDA; CPU fallback is disabled")
    if checkpoint.name != CHECKPOINT:
        raise ValueError("Expected the official DIV2K classical x4 checkpoint")
    sys.path.insert(0, str(source))
    from models.network_swinir import SwinIR

    assets = json.loads((root / "selection.json").read_text(encoding="utf8"))["assets"]
    if len(assets) != 40 or len({a["source_id"] for a in assets}) != 40:
        raise ValueError("Expected the existing 40 distinct representative textures")
    recipe = dict(model=MODEL, checkpoint=CHECKPOINT,
        checkpoint_sha256=hd.digest(checkpoint.read_bytes()),
        checkpoint_url="https://github.com/JingyunLiang/SwinIR/releases/download/v0.0/" + CHECKPOINT,
        source_commit=subprocess.check_output(["git", "-c", "safe.directory=" + source.as_posix(),
                                              "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
        network_sha256=hd.digest((source / "models/network_swinir.py").read_bytes()),
        runner_sha256=hd.digest(Path(__file__).read_bytes()),
        padding_helper_sha256=hd.digest(Path(hd.__file__).read_bytes()),
        precision="FP32", tf32=False, inference="whole image; no tiling; no ensemble",
        scale=4, padding=8, wrap_u=False, wrap_v=False,
        window_padding="official test-script flipped extension; adds 8 when aligned",
        alpha="same independently resampled alpha as existing Lanczos candidate",
        ui_bleed="same nearest nontransparent RGB spread as prior audition",
        packages={p: importlib.metadata.version(p) for p in ("torch", "torchvision", "timm", "numpy", "pillow")},
        cuda=torch.version.cuda, device=torch.cuda.get_device_name(0))
    output.mkdir(parents=True, exist_ok=True)
    state_path = output / "build.json"
    previous = json.loads(state_path.read_text()) if state_path.exists() else {}
    cached = {a["id"]: a for a in previous.get("completed", [])} if previous.get("recipe") == recipe else {}
    completed = []
    torch.set_num_threads(4)
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    model = SwinIR(upscale=4, in_chans=3, img_size=48, window_size=8, img_range=1.,
        depths=[6]*6, embed_dim=180, num_heads=[6]*6, mlp_ratio=2,
        upsampler="pixelshuffle", resi_connection="1conv")
    weights = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model.load_state_dict(weights["params"], strict=True)
    model = model.eval().to("cuda")
    print("MODEL", json.dumps(recipe), flush=True)
    start_all = time.perf_counter()
    for asset in assets:
        ident = asset["source_id"]
        original_path = (root / asset["original"]).resolve()
        if not original_path.is_relative_to(root.resolve()):
            raise ValueError("Original escapes review root")
        original_hash = hd.digest(original_path.read_bytes())
        if original_hash != asset["original_hash"]:
            raise ValueError("Original fingerprint changed: " + asset["sample"])
        paths = {str(scale): output / "variants" / str(scale) / (ident + ".png") for scale in (2, 4)}
        old = cached.get(ident)
        if old and old["original_hash"] == original_hash and all(
                p.exists() and hd.digest(p.read_bytes()) == old["outputs"][scale]["sha256"]
                for scale, p in paths.items()):
            completed.append(old)
            print("RESUME", asset["sample"], flush=True)
            continue
        started = time.perf_counter()
        original = Image.open(original_path).convert("RGBA")
        prepared = hd.bleed_rgb(original) if asset["bleed"] else original
        padded = hd.padded(prepared.convert("RGB"), 8)
        tensor = torch.from_numpy(np.array(padded).astype(np.float32) / 255.)
        tensor = tensor.permute(2, 0, 1).unsqueeze(0).contiguous().to("cuda")
        h, w = tensor.shape[-2:]
        hp, wp = (h // 8 + 1) * 8 - h, (w // 8 + 1) * 8 - w
        tensor = torch.cat([tensor, torch.flip(tensor, [2])], 2)[:, :, :h+hp, :]
        tensor = torch.cat([tensor, torch.flip(tensor, [3])], 3)[:, :, :, :w+wp]
        torch.cuda.reset_peak_memory_stats()
        with torch.inference_mode():
            result = model(tensor)[..., :h*4, :w*4]
        result = result.squeeze(0).float().cpu().clamp_(0, 1).permute(1, 2, 0).numpy()
        colour4 = Image.fromarray((result * 255.).round().astype(np.uint8)).crop(
            (32, 32, 32+original.width*4, 32+original.height*4))
        outputs = {}
        for scale, path in paths.items():
            factor = int(scale)
            colour = colour4.copy() if factor == 4 else colour4.resize(
                (original.width*2, original.height*2), Image.Resampling.LANCZOS)
            alpha = hd.padded(original.getchannel("A"), 8)
            alpha = alpha.resize((alpha.width*factor, alpha.height*factor),
                Image.Resampling.NEAREST if asset["alpha"] == "nearest" else Image.Resampling.LANCZOS)
            pad = 8*factor
            alpha = alpha.crop((pad, pad, alpha.width-pad, alpha.height-pad))
            reference = Image.open(root / "variants/lanczos" / scale / (ident+".png")).convert("RGBA")
            if alpha.tobytes() != reference.getchannel("A").tobytes():
                raise ValueError("Alpha differs from Lanczos baseline: " + asset["sample"])
            colour.putalpha(alpha)
            path.parent.mkdir(parents=True, exist_ok=True)
            colour.save(path)
            outputs[scale] = dict(path=path.relative_to(output).as_posix(),
                sha256=hd.digest(path.read_bytes()), dimensions=list(colour.size),
                alpha_sha256=hd.digest(alpha.tobytes()))
        record = dict(id=ident, sample=asset["sample"], original_hash=original_hash,
            outputs=outputs, seconds=round(time.perf_counter()-started, 4),
            peak_allocated_bytes=torch.cuda.max_memory_allocated())
        completed.append(record)
        state_path.write_text(json.dumps(dict(schema=1, recipe=recipe, completed=completed,
            session_seconds=round(time.perf_counter()-start_all, 4)), indent=2), encoding="utf8")
        print("DONE", asset["sample"], asset["label"], record["seconds"], "seconds", flush=True)
        del tensor, result
    print("COMPLETE", len(completed), "assets", round(time.perf_counter()-start_all, 3), "seconds", flush=True)


def render(root, output):
    from PIL import Image
    previous = json.loads((root / "review.json").read_text(encoding="utf8"))
    build = json.loads((output / "build.json").read_text(encoding="utf8"))
    if len(build["completed"]) != 40:
        raise ValueError("Incomplete SwinIR audition")
    records = {r["id"]: r for r in build["completed"]}
    data = copy.deepcopy(previous)
    data["recipes"] = {MODEL: build["recipe"]}
    for asset in data["assets"]:
        asset["original"] = "../../" + asset["original"]
        asset["preview"] = "../../" + asset["preview"]
        asset["variants"] = {k: "../../"+v for k, v in asset["variants"].items()}
        asset["channels"] = {k: "../../"+v for k, v in asset["channels"].items()}
        for scale in (2, 4):
            key = MODEL + ":" + str(scale)
            record = records[asset["source_id"]]["outputs"][str(scale)]
            path = output / record["path"]
            if hd.digest(path.read_bytes()) != record["sha256"]:
                raise ValueError("Output fingerprint changed")
            rgba = Image.open(path).convert("RGBA")
            if rgba.size != (asset["width"]*scale, asset["height"]*scale):
                raise ValueError("Output dimensions changed")
            asset["variants"][key] = record["path"]
            for channel in ("rgb", "alpha"):
                dest = output / "channels" / channel / str(scale) / path.name
                dest.parent.mkdir(parents=True, exist_ok=True)
                (rgba.convert("RGB") if channel == "rgb" else rgba.getchannel("A")).save(dest)
                asset["channels"][key+":"+channel] = dest.relative_to(output).as_posix()
    data["review_id"] = hd.digest(json.dumps(dict(previous=previous["review_id"],
        recipe=build["recipe"]), sort_keys=True).encode())
    data["note"] = "Official DIV2K SwinIR-M classical x4 (s48w8), FP32 CUDA, whole-image inference. Eight-pixel clamped padding, existing UI bleed, and unchanged separately processed alpha. 2x colour is reduced from x4; it is not a native x2 checkpoint. Wrap axes, shader masks and in-game SwinIR behavior remain unverified."
    render_details(root, output, data["assets"])
    html = hd_review.HTML.replace("RealESRGAN", "SwinIR classical").replace("realesrgan-x4plus", MODEL)
    html = html.replace("40 representative texture auditions", "40 textures · SwinIR classical audition")
    html = html.replace("Thumbnails show RealESRNet at 4× for navigation.", "Existing thumbnails are for navigation; SwinIR is shown in the full candidate pane.")
    html = html.replace("Printable comparison sheets:", "Earlier Net/GAN comparison sheets:")
    html = html.replace('href="sheets/', 'href="../../sheets/')
    html = html.replace("Compare the original with Lanczos, RealESRNet and SwinIR classical.",
        "Compare the original with Lanczos, RealESRNet and SwinIR classical. Check small tattoos, logos and jersey perforations before choosing a whole-game approach.")
    html = html.replace("<h2>All 40 assets</h2>", '<p><a href="../../review.html">Earlier 40-asset gallery</a> · <a href="../../lanczos-in-game/index.html">Lanczos in-game captures</a></p><h2>All 40 assets</h2>')
    html = html.replace('<main><h2 id="title">', '<main><p><a href="details.html">Focused tattoo, logo, jersey, denim and wood comparisons</a></p><h2 id="title">')
    payload = json.dumps(data).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    (output / "review.json").write_text(json.dumps(data, indent=2), encoding="utf8")
    (output / "index.html").write_text(html.replace("__DATA__", payload), encoding="utf8")
    print("READY", output / "index.html", flush=True)


def render_details(root, output, assets):
    from PIL import Image
    regions = [
        ("House sun tattoo", "S01", (360, 426, 424, 490)),
        ("House graffiti logo", "S01", (288, 350, 352, 414)),
        ("Crown loading emblem", "S25", (0, 0, 128, 128)),
        ("Blaze jersey perforations", "S03", (132, 24, 196, 88)),
        ("Blaze denim", "S03", (144, 368, 208, 432)),
        ("Weathered wood", "S17", (96, 80, 160, 144)),
    ]
    labels = ["Original · nearest 4x", "Lanczos 4x", "RealESRNet 4x", "SwinIR classical 4x"]
    rows, records = [], []
    for number, (label, sample, box) in enumerate(regions):
        asset = next(a for a in assets if a["sample"] == sample)
        paths = [output / asset["original"]] + [output / asset["variants"][m+":4"]
            for m in ("lanczos", "realesrnet-x4plus", MODEL)]
        cells, files = [], []
        for column, (name, path) in enumerate(zip(labels, paths)):
            image = Image.open(path).convert("RGB")
            if column == 0:
                crop = image.crop(box).resize(((box[2]-box[0])*4, (box[3]-box[1])*4), Image.Resampling.NEAREST)
            else:
                crop = image.crop(tuple(v*4 for v in box))
            dest = output / "details" / (str(number+1)+"-"+str(column)+".png")
            dest.parent.mkdir(parents=True, exist_ok=True)
            crop.save(dest)
            relative = dest.relative_to(output).as_posix()
            cells.append('<figure><figcaption>'+html_module.escape(name)+'</figcaption><a href="'+relative+'"><img src="'+relative+'" alt="'+html_module.escape(label+' '+name)+'"></a></figure>')
            files.append(dict(path=relative, sha256=hd.digest(dest.read_bytes()), dimensions=list(crop.size)))
        rows.append('<h2>'+html_module.escape(label)+'</h2><p>'+sample+' · source crop '+str(box)+'</p><div class="row">'+''.join(cells)+'</div>')
        records.append(dict(label=label, sample=sample, source_box=box, inputs=[dict(path=str(p.resolve()),
            sha256=hd.digest(p.read_bytes())) for p in paths], outputs=files))
    (output / "details.json").write_text(json.dumps(records, indent=2), encoding="utf8")
    (output / "details.html").write_text('<!doctype html><meta charset="utf-8"><title>SwinIR detail audition</title><style>body{background:#181c23;color:#eee;font:16px system-ui;margin:24px}p{max-width:1100px;line-height:1.5}.row{display:flex;gap:12px;overflow:auto}figure{margin:0;flex:none}figcaption{font-size:14px;margin-bottom:8px}img{width:512px;height:512px;image-rendering:pixelated}h2{font-size:20px}a{color:#9cf}</style><h1>SwinIR classical · small feature comparisons</h1><p>Same RGB regions, without alpha masking. The 64-pixel source crops display at 8x source size (2x output pixels); the 128-pixel crown displays at 4x source size (1x output pixels). Display enlargement is nearest-neighbour. Click an image for its actual pixels. Look for changed tattoo line connections, logo shapes, smoothing, ringing and invented texture.</p><p><a href="index.html">All 40 full textures and alpha controls</a></p>'+''.join(rows), encoding="utf8")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=("generate", "render"))
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--swinir", type=Path)
    ap.add_argument("--checkpoint", type=Path)
    args = ap.parse_args()
    output = args.root / "experiments/swinir-classical"
    if args.action == "generate":
        if not args.swinir or not args.checkpoint:
            ap.error("generate requires --swinir and --checkpoint")
        generate(args.root, output, args.swinir, args.checkpoint)
    else:
        render(args.root, output)


if __name__ == "__main__":
    main()
