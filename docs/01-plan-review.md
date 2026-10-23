# Plan review: Gemini's "AI-Assisted Def Jam FFNY Native PC Port"

Reviewed 2026-09-17 against the actual toolkit, the actual disc, and three research passes
(`docs/research/`). Verdict: **the goal is sound and the roadmap is usable, but four premises were wrong
and one risk was missing.** Corrected roadmap at the bottom.

## What the plan got right
- Original Xbox over PS2/GC: correct. Single `default.xbe`, XDK 5849, highest-quality assets, plain XML data tables.
- `sp00nznet/xboxrecomp` exists, is MIT, and does exactly the pipeline described (XBE, analysis, C, CMake).
- Local multiplayer and Windows support are cheap; custom fighters and netcode belong to a later decomp phase.
- The "AI error loop" (compile, crash, stub or shim, repeat) is literally how the toolkit's own README says bring-up works.

## Premises that were wrong (and what we do instead)
| Plan said | Reality | Consequence |
|---|---|---|
| Recomp gives a mature, "natively supported" path | Toolkit is 6 months old, one maintainer, AI-co-authored; only Burnout 3 boots to rendering; lifter semantics bugs were fixed as recently as 2026-09-16 | **Treat as R&D.** Keep xemu as the reference baseline; pause and re-evaluate if bring-up stalls (decision D4). |
| Config is `.toml` | It is JSON (`functions.json`, `abi_functions.json`, ...) | Docs and scripts use the real files. |
| "Steam Deck / Linux compiled via GCC/Clang" | Runtime is D3D11-first; the Linux/OpenGL backend is far less tested | **Windows build under Proton first** (decision D6); native Linux is a later milestone. (Superseded by D95: native Linux and the Deck shipped in v0.7.0.) |
| Windows = MSVC (any) | README requires **VS 2022**; the machine had only VS 2019 Build Tools | VS 2022 install pending (UAC); VS 2019 tried in parallel. |
| "60+ FPS: AI writes delta-time patches" rated Medium | The Xbox is x86, so recomp gives no free speed. The AKI engine is almost certainly fixed-tick; unlocking a fighting game touches hit-frames, animation timing and input windows | Re-rated **High**. Do it after boot and gameplay are stable, as an opt-in patch validated against xemu reference footage. |

## Risk the plan omitted
**Legal and publishing.** EA has issued DMCA takedowns against at least one fan decompilation. For an eventual public
repo (decision D5): no game bytes are ever tracked, the manifest holds hashes only, the user supplies their own dump,
there are no links to ROM sites, and built executables that embed lifted code are never distributed. The lifted C
**is** a derivative of EA's binary, so the repo ships tools and patches, not the output, exactly like the
N64Recomp and XenonRecomp projects.

## Facts established today that shape the work
- 16,051 functions, 732k instructions, 1,048 `thiscall` methods (heavy C++); 393 unresolved call targets after lifting.
- 160 kernel imports including `KeConnectInterrupt`, `HalReadWritePCISpace`, `MmClaimGpuInstanceMemory`,
  `AvSendTVEncoderOption`, `XeLoadSection`/`XeUnloadSection`, `NtQueueApcThread`, `KeSaveFloatingPointState`.
  The game talks to hardware and the section loader directly. xemu's "PTimer unsupported" freeze for this title
  is the first place to look when it hangs.
- Data-driven engine: fighters, venues, combos and front-end flow are XML; audio, textures and models are in EA BIGF
  `.viv` archives; FMV is EA `MADk`. Texture and soundtrack modding is therefore a file-IO redirection problem.
- No RenderWare; AKI in-house engine. The toolkit's RenderWare heuristics contribute nothing, so Ghidra symbol recovery matters more.

## Corrected roadmap (milestones with exit criteria)
| M | Milestone | Exit criterion (see `docs/02-test-plan.md`) |
|---|---|---|
| M0 | Environment and skeleton | `verify-dump.py` OK, analysis and lift run, CI green. **Done 2026-09-17.** |
| M1 | It links | `defjam_recomp.exe` builds with zero unresolved externals |
| M2 | Boot to EA logo | smoke stage S3: D3D device created, first frame presented |
| M3 | **Boot to title screen** (first milestone) | golden screenshot matches the xemu reference within tolerance; runs 60 s without the watchdog firing |
| M4 | Menus and one exhibition match, in four phases (D36). **Closed 2026-10-02 (D45).** | M4a menu: the main menu draws and navigates, popups visible. M4b match setup: Battle through mode, fighter and venue select to a fight loading. M4c fight: a controllable fighter, hits land, a KO, back to the menu. M4d full checklist: pads, audio, FMV, stability over three fights |
| M4e | Vertex programs on the GPU (D43) | a fight presents 60 frames a second in the debug build at the round clock's design rate, no new crash over three fights; M2, M3, M4a goldens match; the CPU path stays behind `RECOMP_VSH_GPU=0`. Design: `docs/research/m4e-gpu-vertex-programs.md` |
| M4f | Looks like the console (D44) | on the fight and the Story route up to the tutorial fight, Vlad finds no difference from console video worth fixing; M2, M3, M4a goldens match |
| M6 | PC features and enhancements (in progress, D63/D84; `docs/07-m6-plan.md`) | a settings file, hotkeys, an overlay and a launcher sharing one settings core; fullscreen, window size, render scale and aspect-correct presentation; non-Xbox pads, keyboard play, remapping, rumble and local multiplayer; HD texture replacement packs next (proposed v0.6.0 after native v0.5.0); true 16:9 remains unscheduled ToDo; each feature has a check in `docs/02-test-plan.md` and leaves `scripts/regress.py` green |
| M5 | Steam Deck via Proton (after M6; superseded by D95: native Linux, v0.7.0) | the M4 checklist on the Deck; controller glyphs; 16:10 handling |
| M9 | macOS (after M5, D45) | the M4 checklist on macOS. The renderer is Direct3D 11 only, so this needs a second graphics backend or a translation layer; to be scoped before it starts |
| M7 | Native Linux (stretch) | OpenGL backend or Vulkan bridge; Flatpak |
| M8 | Decomp phase (stretch) | matching C++ for gameplay systems, enabling custom fighters and netcode |
