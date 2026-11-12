# THIRD-PARTY.md — win-fg attribution ledger


> **Note (2026-09-09, v0.3.0):** the project is named **Win-FG Native** since the chain
> moved into the host compositor. Every attribution below is unchanged by that
> — the same shaders from the same sources are compiled into the host build and
> into the layer `.so`. No third-party code was added by the move.
Every third-party pattern, algorithm, or design idea used in win-fg —
attributed by name, source, and license. This is the audit trail for the
clean-room protocol described in `docs/PROVENANCE.md`.

**No compiled binaries, no coefficient values, no SPIR-V bytecode** from any
third party enter `libwin_fg.so`. Every pattern below is implemented from
first principles or cited paper math; the entries here credit the design
inspiration, not code re-use.

---

## Algorithms

### AMD FidelityFX FSR3 optical flow
- **License:** MIT (© AMD, GPUOpen)
- **Source:** `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`
- **Used for:** motion-estimation shader family (`of3_luma`, `of3_downsample`, `of3_flow`, `of3_expand`). Our subgroup-free reimplementation of the FSR3 luminance pyramid + coarse-to-fine SAD block search. See `NOTICE_FIDELITYFX_OPTICALFLOW.md`.
- **Roadmap uses:**
  - `feat/fb-consistency` — reference implementation study of `ffx_frameinterpolation_disocclusion_mask.glsl`
  - `feat/flow-inpainting` — port pattern from `ffx_frameinterpolation_inpainting_pass.glsl` + inpainting pyramid pair
  - `feat/correlation-cost9` — reference implementation study of `ffx_frameinterpolation_optical_flow_vector_field.glsl`
  - `feat/fsr3-of-v5-alt-motion` (potential) — port `compute_optical_flow_advanced_pass_v5.glsl` as alternate motion estimator

### Softmax splatting for video frame interpolation
- **Reference:** Niklaus & Liu, *Softmax Splatting for Video Frame Interpolation*, CVPR 2020 (arxiv:2003.05534)
- **Used for:** importance-weighted blend of warped candidates in `wfg_synth.comp`. We adapt the forward-splat importance weighting into a scatter-free backward-warp blend suitable for mobile real-time.

### Horn-Schunck brightness constancy
- **Reference:** Horn & Schunck, *Determining Optical Flow*, Artificial Intelligence 17, 1981. Public domain math.
- **Used for:** photometric residual term in `wfg_synth.comp`.
- **Roadmap uses:** `feat/flow-regularization` — TV-L1 smoothness prior successor to H-S L2.

### Forward-backward consistency + occlusion detection
- **Reference:** Sundaram, Brox, Keutzer, *Dense point trajectories by GPU-accelerated large displacement optical flow*, ECCV 2010 (also OCAI arxiv:2403.18092)
- **Used for:** planned occlusion-gated confidence in `feat/fb-consistency` branch.

### RAFT correlation cost volume + iterative refinement
- **Reference:** Teed & Deng, *RAFT: Recurrent All-Pairs Field Transforms for Optical Flow*, ECCV 2020 (arxiv:2003.12039). BSD-3-Clause.
- **Roadmap uses:** `feat/correlation-cost9` (3×3 cost lookup + sub-pixel parabola fit), `feat/warp-follow-refine` (iterative flow refinement).

### Lucas-Kanade / Baker-Matthews inverse-compositional alignment
- **Reference:** Baker & Matthews, *Lucas-Kanade 20 Years On: A Unifying Framework*, IJCV 2004. Szeliski, *Computer Vision: Algorithms and Applications*, Ch. 6.2.
- **Roadmap uses:** `feat/global-motion-prewarp` — 6-parameter affine fit via LK gradient descent.

### Sub-pixel parabola fit for cost-volume argmin
- **Reference:** Lowe, *Distinctive Image Features from Scale-Invariant Keypoints*, IJCV 2004 §5.
- **Roadmap uses:** `feat/correlation-cost9`.

### TV-L1 optical flow / anisotropic smoothness prior
- **Reference:** Chambolle & Pock, *A First-Order Primal-Dual Algorithm for Convex Problems*, JMIV 2011. Public domain math.
- **Roadmap uses:** `feat/flow-regularization`.

### Median filter as robust flow smoothing
- **Reference:** Tukey, *Exploratory Data Analysis*, 1977. Public domain.
- **Roadmap uses:** `feat/median-flow-filter`.

---

## Design patterns (no code copied)

### Isygold Vegas-DXVK framegen kit (zlib/libpng, © isygold, 2026)
- **HUD-rect exclusion push-constant API** — pattern borrowed for `feat/hud-rect-mask`; `wfg_synth.comp` implementation independent.
- **GPU-saturation skip window** (N=5 slow-trips, W=60 skip window) — starting constants borrowed as tuning seed for `feat/gpu-saturation-skip`; behavior re-implemented.
- **Tier-gated auto activation** (T2 ≤29 ms, T3 ≤33 ms) — Snapdragon starting points; state machine independent for `feat/tier-gated-auto`.
- **Bimodal-dominance stats SSBO** — atomic-counter pattern for per-block regime classification. Adopted for `feat/adaptive-gate` diagnostics.
- **Adreno TBDR occupancy notes** — 11.5 KB shared-memory cliff on A610, packed-atomicMin ordering trap. Adopted repo-wide as `<8 KB shared per workgroup` discipline.
- **No source code, coefficient values, or SPIR-V bytecode copied.** See `docs/research/07-isygold-vegas-inspirations.md`.

### maxjivi05 WinNative PR #537 [`frame-gen`]
- **RAFT-family multi-pass architecture** (pyramid → conv → cost-volume → flow-reg → warp-follow → generate) — architectural pattern confirms our port plan; implementation independent from RAFT paper.
- **Median filter on flow** — one-shader post-process pattern; implemented from Tukey 1977 for `feat/median-flow-filter`.
- **Multi-input generate pass** (occlusion + features + flows) — synthesis binding pattern for `feat/fb-consistency` synth extension.
- **Empirical confidence calibration workflow** — measure gate thresholds against ground-truth midpoints instead of hand-tune; adopted for `feat/capture-mode` + auto-calibration tool.
- **Runtime `debug.<pkg>.<flag>` sysprop opt-in gate** — Android-native toggle pattern; adopted for SANFG deployment.
- **No source code, coefficient values, or SPIR-V bytecode copied.** See `docs/research/06-max-winnative-inspirations.md`. Explicitly excluded from any porting: the `wnfg_XX_spv.h` pre-compiled blobs and `weights_v2/wnfg_XX.weights.fp16` coefficient values (provenance unverified).

### AMD FSR 3.1 anti-ghosting fixes
- **Ellipsoid color clamp** (replacing AABB) for ghost-streak reduction. Documented in FSR 3.1 release notes; adopted as pattern (not code) for possible `feat/ellipsoid-clamp` synth improvement.
- **Velocity factor for bright-pixel flicker** — same source.
- **"Identify UI by diffing with/without-UI frames"** — documented HUD-detector pattern in FSR3 docs; adopted as reference for `feat/heuristic-hud-detect`.

### SmoothVideo Project (SVP) split-license model
- **Design lesson:** GPL open motion-search + closed rendering worked for 20 years. Adopted operationally: our clean-room synth stays MIT; if we ever add a GPL algorithm we isolate it.

---

## Runtimes (Phase 2 — neural, not yet integrated)

### RIFE v4.25 / IFNet
- **License:** MIT (© hzwer)
- **Source:** `hzwer/Practical-RIFE`
- **Planned use:** `feat/rife-lite-fine-tune` — fine-tune RIFE-4.25.lite on self-captured Bannerlator game frame triplets with HUD-preservation loss extension. Ship modified weights under MIT.
- **Warm-start weights:** hzwer's public MIT-licensed 4.25.lite checkpoint. Fine-tune on self-captured data erases the Vimeo90K pretraining taint through data replacement.

### NCNN (Vulkan backend)
- **License:** BSD-3-Clause (© Tencent)
- **Source:** `Tencent/ncnn`
- **Planned use:** `feat/ncnn-vulkan-inference` — inference runtime for the fine-tuned RIFE weights on Adreno-class hardware. Cross-vendor Vulkan; no Qualcomm lock-in.
- **Additional:** `feat/rife-warp-kernel` may study/port `nihui/rife-ncnn-vulkan/warp_pack4.comp` (MIT) as fp16-aware bilinear-warp reference.

### Qualcomm QNN Runtime (optional Snapdragon fast path)
- **License:** Qualcomm proprietary, free-use runtime
- **Source:** Maven `com.qualcomm.qti:qnn-runtime`
- **Planned use:** `feat/qnn-htp-delegate` — Hexagon HTP inference path for Snapdragon devices. Fallback to NCNN Vulkan on other hardware. **Models we ship remain fully ours.**

---

## Explicitly NOT USED

The following are read for algorithmic understanding only. **No code,
kernels, weights, or bytecode from these enter `libwin_fg.so` in any
form:**

- **LSFG (Lossless Scaling)** — proprietary, closed. Redistributing any
  extracted shader is the takedown pattern that killed bionic-fg.
- **`lsfg-vk`** (PancakeTAS) — GPL-3.0 layer + user-supplied proprietary
  DLL. GPL viral + provenance liability.
- **DLSS3-FG, XeSS-FG** — proprietary NVIDIA / Intel closed weights.
- **XVFI weights** — non-commercial research license.
- **AMT weights** (all variants) — CC-BY-NC 4.0.
- **HopperRender, mpv-frame-interpolator** — GPL-3.0 OpenCL kernels.
  Viral license; read only for algorithm intuition.
- **FFmpeg `libavfilter/vf_minterpolate.c`** — LGPL; academic reference only.
- **Any `.spv` SPIR-V bytecode blob** shipped without matching `.comp`
  source, including Isygold's `fg_median.spv` / `fg_warp.spv` and Max's
  `wnfg_XX_spv.h` — treat as read-only. Re-implement from published math.

---

## Data formats / encoders

### QOI (Quite OK Image) — lossless
- **License:** public domain / MIT (© Dominic Szablewski). Spec: [qoiformat.org](https://qoiformat.org).
- **Used for:** the lossless RGBA8 encoding of the training-data capture-mode output (`src/capture.hpp`). Our encoder is a clean-room implementation of the published QOI spec (no third-party code); the blobs are packed inside our own `.wfgcap` container. QOI is used because it is lossless (no JPEG/video artifacts to poison training) and cheap to encode (keeps the capture overhead low). See the capture-mode docs in `README.md`.

---

## Training data (Phase 2)

- **Self-captured Bannerlator game footage** (Mode A: 120fps native capture; Mode B: pre-classical flow ground truth via the capture mode below) — the sole source for the final fine-tune data. Owned by us. Not redistributed.
- **RIFE upstream pretraining checkpoint (Vimeo90K)** — used as warm-start only. Full data replacement in fine-tune erases the Vimeo90K taint.
- **No scraped YouTube / Twitch / commercial-platform video** enters our training pipeline.

---

## Research documents

Full research writeups with per-source citations live at
[`docs/research/`](docs/research/):

- `01-optical-flow-algorithms.md`
- `02-learned-fg-models.md`
- `03-training-pipelines.md`
- `04-adreno-deployment.md`
- `05-prior-art.md`
- `06-max-winnative-inspirations.md`
- `07-isygold-vegas-inspirations.md`
- `08-reusable-shaders.md`
- `ROADMAP.md`
- `SANFG.md`

Every research doc lists its sources at the bottom. This file summarizes
the licenses of everything cited across all research docs.
