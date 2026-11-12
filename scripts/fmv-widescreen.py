#!/usr/bin/env python3
"""Widen one full-screen movie shot to 16:9, two ways, for review: a blurred side fill and local AI outpainting.

This is the pipeline of the 2026-10-11 FMV trial (docs/widescreen/08-fmv-ai-pipeline.md). It works on the
user's own dump and writes only under a work folder in logs/ (ignored by git): decoded frames and the results
are the game's artwork and are never committed or shared. Nothing here downloads anything; the tools and the
model are installed by hand as the guide describes.

    python scripts/fmv-widescreen.py extract --ffmpeg <ffmpeg bin> --movie movies/intro.mad \
        --start 28.248 --duration 4.917 --work logs/fmv-widescreen/intro-crew
    python scripts/fmv-widescreen.py blur    --ffmpeg <ffmpeg bin> --work logs/fmv-widescreen/intro-crew
    <venv python> scripts/fmv-widescreen.py ai --model <Wan2.1-VACE-1.3B-diffusers> \
        --work logs/fmv-widescreen/intro-crew --prompt "<what is in the shot and around it>"
    python scripts/fmv-widescreen.py compare --ffmpeg <ffmpeg bin> --work logs/fmv-widescreen/intro-crew
    <venv python> scripts/fmv-widescreen.py qa --work logs/fmv-widescreen/intro-crew

`--movie` is relative to $DEFJAM_DATA/extracted. `extract` seeks with -ss after -i: a .mad stream decoded from
a point between keyframes comes out corrupt, and the AI then draws from the corruption. `ai` and `qa` need the
trial venv (PyTorch with CUDA, diffusers >= 0.41, numpy, Pillow); the rest need only Python and ffmpeg.

Outputs in the work folder: frames/src_NNNN.png, src.mkv (lossless cut), blur.mp4, ai/ai_NNNN.png, ai.mp4,
compare.mp4 (blur on top, AI below), still-N.png, qa.txt.
"""
import argparse
import glob
import os
import subprocess
import sys
import time

WIDTH, HEIGHT, SRC_W = 848, 480, 640       # 848 = nearest multiple of 16 to 480 * 16/9
FPS = "30.30"                              # the .mad files' frame rate as ffmpeg reports it
NEGATIVE = ("bright colors, overexposed, static, blurred details, subtitles, text, logo, watermark, cartoon, "
            "painting, worst quality, low quality, jpeg artifacts, extra fingers, deformed, disfigured, split "
            "screen, border, frame")
BLUR_FILTER = ("[0:v]split[a][b];[a]scale={w}:{h},boxblur=24:3,eq=brightness=-0.12:saturation=0.8[bg];"
               "[bg][b]overlay={x}:0")


def plan_chunks(n, chunk, overlap):
    """Split n frames into windows of `chunk` frames that overlap by `overlap`.

    Returns a list of (start, indices); indices always has `chunk` entries, the last window padded by
    repeating the final frame (the model needs a fixed length). Every frame is covered, and each window
    after the first begins `overlap` frames before the previous one ended.
    """
    if n <= 0:
        return []
    if not 0 <= overlap < chunk:
        raise ValueError("overlap must be in [0, chunk)")
    plan, start = [], 0
    while True:
        idx = list(range(start, min(start + chunk, n)))
        idx += [n - 1] * (chunk - len(idx))
        plan.append((start, idx))
        if start + chunk >= n:
            return plan
        start += chunk - overlap


def ffmpeg(args, tool="ffmpeg"):
    exe = os.path.join(args.ffmpeg, tool + (".exe" if os.name == "nt" else ""))
    return [exe, "-hide_banner", "-loglevel", "error", "-y"]


def run(cmd):
    subprocess.run(cmd, check=True)


def cmd_extract(args):
    data = os.environ.get("DEFJAM_DATA")
    if not data:
        sys.exit("set DEFJAM_DATA to the data folder")
    movie = os.path.join(data, "extracted", args.movie.replace("/", os.sep))
    os.makedirs(os.path.join(args.work, "frames"), exist_ok=True)
    src = os.path.join(args.work, "src.mkv")
    run(ffmpeg(args) + ["-an", "-i", movie, "-ss", str(args.start), "-t", str(args.duration),
                        "-vf", "setpts=PTS-STARTPTS", "-c:v", "ffv1", src])
    run(ffmpeg(args) + ["-i", src, os.path.join(args.work, "frames", "src_%04d.png")])
    print(len(glob.glob(os.path.join(args.work, "frames", "src_*.png"))), "frames")


def cmd_blur(args):
    x = (WIDTH - SRC_W) // 2
    run(ffmpeg(args) + ["-i", os.path.join(args.work, "src.mkv"), "-filter_complex",
                        BLUR_FILTER.format(w=WIDTH, h=HEIGHT, x=x), "-c:v", "libx264", "-crf", "16",
                        "-pix_fmt", "yuv420p", os.path.join(args.work, "blur.mp4")])


def cmd_ai(args):
    import torch
    from PIL import Image
    from diffusers import AutoencoderKLWan, WanVACEPipeline
    from diffusers.schedulers.scheduling_unipc_multistep import UniPCMultistepScheduler

    x0 = (WIDTH - SRC_W) // 2
    src = [Image.open(p).convert("RGB")
           for p in sorted(glob.glob(os.path.join(args.work, "frames", "src_*.png")))]
    if not src:
        sys.exit("no frames: run extract first")
    out_dir = os.path.join(args.work, "ai")
    os.makedirs(out_dir, exist_ok=True)

    vae = AutoencoderKLWan.from_pretrained(args.model, subfolder="vae", torch_dtype=torch.float32)
    pipe = WanVACEPipeline.from_pretrained(args.model, vae=vae, torch_dtype=torch.bfloat16)
    pipe.scheduler = UniPCMultistepScheduler.from_config(pipe.scheduler.config, flow_shift=3.0)
    vae.enable_tiling()               # without it the VAE overflows 16 GB at 81 frames and the run stalls
    pipe.enable_model_cpu_offload()

    gray = Image.new("RGB", (WIDTH, HEIGHT), (127, 127, 127))
    generate = Image.new("L", (WIDTH, HEIGHT), 255)
    generate.paste(0, (x0, 0, x0 + SRC_W, HEIGHT))
    keep = Image.new("L", (WIDTH, HEIGHT), 0)
    result = [None] * len(src)
    for start, idx in plan_chunks(len(src), args.chunk, args.overlap):
        video, masks = [], []
        for j, i in enumerate(idx):
            if start > 0 and j < args.overlap and result[i] is not None:
                video.append(result[i]); masks.append(keep)        # continue the previous window's sides
            else:
                frame = gray.copy(); frame.paste(src[i], (x0, 0))
                video.append(frame); masks.append(generate)
        t0 = time.time()
        frames = pipe(video=video, mask=[m.convert("RGB") for m in masks], prompt=args.prompt,
                      negative_prompt=NEGATIVE, height=HEIGHT, width=WIDTH, num_frames=args.chunk,
                      num_inference_steps=args.steps, guidance_scale=args.guidance,
                      generator=torch.Generator("cpu").manual_seed(args.seed), output_type="pil").frames[0]
        print(f"window from {start}: {time.time() - t0:.0f} s", flush=True)
        for j, i in enumerate(idx):
            if result[i] is None or (start > 0 and j >= args.overlap):
                frame = frames[j].convert("RGB").resize((WIDTH, HEIGHT))
                frame.paste(src[i], (x0, 0))                      # the original picture, untouched
                result[i] = frame
    for i, frame in enumerate(result):
        frame.save(os.path.join(out_dir, f"ai_{i:04d}.png"))


def cmd_compare(args):
    w = args.work
    run(ffmpeg(args) + ["-framerate", FPS, "-i", os.path.join(w, "ai", "ai_%04d.png"), "-c:v", "libx264",
                        "-crf", "16", "-pix_fmt", "yuv420p", os.path.join(w, "ai.mp4")])
    run(ffmpeg(args) + ["-i", os.path.join(w, "blur.mp4"), "-i", os.path.join(w, "ai.mp4"), "-filter_complex",
                        f"[0:v]pad={WIDTH}:{HEIGHT + 20}:0:20:black[a];[1:v]pad={WIDTH}:{HEIGHT + 20}:0:20:black[b];"
                        "[a][b]vstack", "-c:v", "libx264", "-crf", "16", "-pix_fmt", "yuv420p",
                        os.path.join(w, "compare.mp4")])
    for n in [int(s) for s in args.stills.split(",") if s]:
        run(ffmpeg(args) + ["-i", os.path.join(w, "blur.mp4"), "-i", os.path.join(w, "ai.mp4"), "-filter_complex",
                            f"[0:v]select=eq(n\\,{n})[a];[1:v]select=eq(n\\,{n})[b];[a][b]vstack",
                            "-frames:v", "1", os.path.join(w, f"still-{n}.png")])


def cmd_qa(args):
    """Frame-to-frame change of each side against the centre: a side that jumps while the centre does not is a pop."""
    import numpy as np
    from PIL import Image
    x0 = (WIDTH - SRC_W) // 2
    files = sorted(glob.glob(os.path.join(args.work, "ai", "ai_*.png")))
    rows, prev = [], None
    for i, f in enumerate(files):
        a = np.asarray(Image.open(f).convert("L"), dtype=np.float32)
        if prev is not None:
            d = np.abs(a - prev)
            rows.append((i, d[:, :x0].mean(), d[:, x0:x0 + SRC_W].mean(), d[:, x0 + SRC_W:].mean()))
        prev = a
    if not rows:
        sys.exit("no AI frames")
    m = np.array(rows)
    lines = ["mean change left %.2f centre %.2f right %.2f" % tuple(m[:, 1:].mean(0))]
    for i, left, centre, right in rows:
        cut = centre > 3 * m[:, 2].mean()
        pop = [s for s, v, avg in (("left", left, m[:, 1].mean()), ("right", right, m[:, 3].mean()))
               if v > 3 * avg and not cut]
        if cut:
            lines.append(f"frame {i}: cut (centre {centre:.1f}); sides left {left:.1f} right {right:.1f}")
        elif pop:
            lines.append(f"frame {i}: side pop {'/'.join(pop)} (left {left:.1f} centre {centre:.1f} right {right:.1f})")
    text = "\n".join(lines)
    print(text)
    with open(os.path.join(args.work, "qa.txt"), "w") as fh:
        fh.write(text + "\n")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("extract", "blur", "compare"):
        s = sub.add_parser(name)
        s.add_argument("--ffmpeg", required=True, help="folder holding ffmpeg and ffprobe")
        s.add_argument("--work", required=True)
    sub.choices["extract"].add_argument("--movie", required=True, help="path under $DEFJAM_DATA/extracted")
    sub.choices["extract"].add_argument("--start", type=float, required=True, help="seconds")
    sub.choices["extract"].add_argument("--duration", type=float, required=True, help="seconds")
    sub.choices["compare"].add_argument("--stills", default="20,75,140", help="frame numbers to stack")
    s = sub.add_parser("ai")
    s.add_argument("--model", required=True, help="Wan2.1-VACE-1.3B-diffusers folder")
    s.add_argument("--work", required=True)
    s.add_argument("--prompt", required=True, help="the shot and what plausibly lies beside it")
    s.add_argument("--steps", type=int, default=30)
    s.add_argument("--chunk", type=int, default=81, help="frames per window (4k+1)")
    s.add_argument("--overlap", type=int, default=12, help="frames carried into the next window")
    s.add_argument("--guidance", type=float, default=5.0)
    s.add_argument("--seed", type=int, default=42)
    s = sub.add_parser("qa")
    s.add_argument("--work", required=True)
    args = p.parse_args(argv)
    {"extract": cmd_extract, "blur": cmd_blur, "ai": cmd_ai, "compare": cmd_compare, "qa": cmd_qa}[args.cmd](args)


if __name__ == "__main__":
    main()
