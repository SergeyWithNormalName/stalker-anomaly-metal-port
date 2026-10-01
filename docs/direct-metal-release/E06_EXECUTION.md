# E06 execution packet — 2026-09-27

**Paused at user request after Bar regression work; see [safe stop](E06_CIRCLES_FIX.md). E06 is not closed.**

E06 only. Gemini census reused; two GPT-6 Luna leaf workers, one central integrator. No commits, staging, reset, shader toolchain upgrade, E07 skinning migration or E08 optimization. Windows reference and human V2 remain OPEN.

## Changes and boundaries

- **6A water:** original Windows blur contract restored (UV 0..0.5 and half viewport for both blur directions/no-blur); `ssfx_wind_anim` scalar is bound in W as read by the original HLSL. SSR/history/waves/final order retained. Shoreline and reflection image match are not proven.
- **6C active SSFX bloom:** exact original `pp_blur → ssfx_bloom → 6 down → 5 up → combine` writes a half-resolution RGBA16F texture consumed by `combine_2`. The active profile bypasses legacy bloom filters after luminance. Authored starburst/dirt/lens DDSs use XRFS. Private uploads become durable only after frame submission; per-pass vertices/constants are immutable arena slices.
- **6C emitter subset:** full-resolution BGRA8 emissive target, cleared before lighting and sampled by bloom. Exact `models\\selflightl` CPU-SKIN_0 world and non-strict HUD leaves use `deffer_model_flat.vs/accum_emissivel.ps`; live PDA factor has an engine-frame owner, HUD uses near depth 0..0.02. Original `models\\selflight`, static/strict/HW-SKIN_4 emitters, first accumulator contribution and HUD mask remain OPEN. No bright-texture heuristic substitutes these families.
- **6E lens:** real effects_sun adapters and value-only producers; sun source before luminance, flare/gradient overlay after combine_2. Reflection, packed color/UV, original quad indices, source blend predicate and submit/discard ownership audited. Authored .tga identities now follow Windows logical-stem→DDS resolution through XRFS; current sunny runtime is tracked below.
- **6E rain:** separate captured DM_Drop splash mesh and single-target splash PSO share the refraction producer with falling rain. Occlusion, wetness accumulation, volumetric input and thunderbolt geometry remain OPEN. Legacy model_CreateDM caused startup exit139; a bounded CPU-only reader now loads the same authored DM fields/bounds without graphics-capability/shader creation. Subsequent S3 and S6 runs exit0. Falling-rain quad topology now matches QuadIB; Metal initializes the same installed SSFX-rain policy used by the CPU simulation.
- **6F diagnostic:** 1W raw nonfinite UVs now reject with vertex index; a negative NaN fixture was added to the existing DynamicWorld self-test. This closes an input validation hole; it does not prove the cause of shader interpolant reports.

- **6D AO/IL:** [source contract review](E06_AO_IL_REVIEW.md) confirmed active IL blur incorrectly used full viewport/UV at scale6.666667. All four blur passes now retain reduced viewport and UV1/scale (288×162 raster footprint at1920×1080); the original HLSL blur_setup.x remains1. AO blur has a separate sampling scale from full viewport. Float viewport division replaces floor. E04 matrices/history reset/copy/submit ownership retained.

- **6C exact luminance source:** restored mandatory `generic1→bloom_build→rt_Bloom1(256² RGBA8)→64→8→1` producer, with original four UV lanes/EPS, exact two-CBV reflection, frame-owned threshold, immutable vertex/parameter slices and clamp RT sampler. Legacy bloom filters remain bypassed by SSFX; this pre-luminance producer is always active. [Luminance source review](E06_LUMINANCE_REVIEW.md); new original-source cook/admission in `luminance-e06`.

## Provenance and verification

Prestate: [initial status/diff/copies](../../scratch/release_runs/20260927-e06-start/), [water](../../scratch/release_runs/20260927-e06-water-prestate/), [lens](../../scratch/release_runs/20260927-e06-lens-prestate/), [emitter](../../scratch/release_runs/20260927-e06-6c-emissive-prestate/), [rain](../../scratch/release_runs/20260927-e06-rain-splashes-prestate/).

Exact DXC 1.9 / MSC 4.0.1 cooks: `bloom-e06`, `emissive-e06`, `rain-splash-e06`, `luminance-e06`; 51 output hashes (including6 original luminance/bloom-build shaders) verified in [final receipt verification](../../scratch/release_runs/20260927-e06-start/e06-receipts-verified-final.json). Source-tree hashes/defines/compiler versions/reflection are in each bundle receipt. Bloom source tree has zero differences against matching MO2-winning profile files. Converted emitter model VS is byte-identical to the prior dynamic-world-e03 SKIN_0 VS. Runtime admission additionally checks artifact hashes, scope/compiler and reflection before creating pipelines.

The packet compiled and linked; selected probes passed **5/5** with API/shader validation: FrameUploadGpu, DynamicWorldAdapter, HUD, ParticleAdapter, NightVisionFoundation. See [latest build](../../scratch/release_runs/20260927-e06-start/build-luminance-source.log), [latest tests](../../scratch/release_runs/20260927-e06-start/tests-luminance-source.log). Core/XRFS dependencies and no-Lua diagnostics were repaired only for these standalone probes. Tests are not live visual acceptance.

## Runtime evidence so far

| Run | Result | Boundary |
|---|---|---|
| `20260927-e06-S3-before` | exit0, 1920x1080 screenshots + 15s motion | Prior binary diagnostic; not isolated before/after proof |
| `20260927-e06-S3-validation` | 38 interpolant NaN/Inf reports; combined capture/validation then aborts | Apple explicitly does not support capturing shader validation; reports preceding that failure remain real |
| `20260927-e06-S3-capture` | exit0; trace finalized, 72 encoders/12797 draws | Diagnostic trace, no validation/timing; 15 bloom passes and actual resource bindings confirmed; water229 draws/waves512 |
| `20260927-e06-S3-lens` / `-lens-active` | exit0, no lens draw | Clear sun-facing variant; paused-weather sun override is diagnostic and did not yet prove producer exercise |
| `20260927-e06-S6-rain` / `-rain-active` | exit0, no rain draw | Scheduled clear/storm/clear labels and explicit-density override are not positive draw evidence |
| `20260927-e06-S3-validation-only` | startup exit139 before scene | Regression in the new packet; no validation verdict from this attempt |

Trace export [bloom output](../../scratch/release_runs/20260927-e06-S3-capture/gpu-export/bloom-combine.png) demonstrates intermediate pixels, not SDR or Windows parity. [88 dynamic draws / 74 buffers](../../scratch/release_runs/20260927-e06-start/dynamic-trace-finite.json) from one normal frame had finite positions/UVs; that is not the original validation frame and does not explain away the 38 reports.

The runner now rejects `--gpu-capture --metal-validation` before launching. Optional `--scenario-script` copies/hashes the diagnostic source and always marks its run non-E00-admitted. Canonical save/registry/scenario and live saved games are preserved. Historical frozen save with the exact fixture SHA was used via `XRAY_PARITY_SAVE_SOURCE_DIR`, because the live autosave has changed.

## Current integration evidence and remaining gate

Independent [cloud review](E06_CLOUDS_REVIEW.md) confirms all active-profile weather records have cloud alpha0; tested w_clear1/w_storm1/w_cloudy3_dark also have empty cloud texture. Current sky contains its authored baked clouds. No positive separate-layer preset exists in this profile; no profile was changed to invent one.

`20260927-e06-S3-lens-final` exits0 on the latest luma/lens binary. XRFS reads authored `anamflares\sun_day1.dds` (786560B); source frame373 submitted1/rejected0 and overlay frame672 submitted1/rejected0 after combine_2. Camera forward matches CPU vSunDir (dot1). [12-second movie sample](../../scratch/release_runs/20260927-e06-S3-lens-final/lens-motion-12s.png) has no conspicuous solar disc/flare; only runtime exercise is accepted. CPU visibility/fBlend/opacity needs pixel diagnosis, and Windows image matching remains OPEN.


- CPU-only DM repair: `20260927-e06-S3-validation-repaired` exits0 with API/shader validation enabled and zero reported invalid Metal usage. This verifies the startup repair; the cause of the older 38 interpolant reports is still unproven.
- Exact emissive-bundle admission now uses `.reflection.json`, rather than legacy `.json` filenames. Actual-bundle positive and legacy-filename negative admission checks pass. `20260927-e06-S6-wet-policy` admits both world/HUD emitter PSOs and exercises world `models\selflightl`; full emitter coverage remains partial.
- Frozen diagnostic `20260927-e06-S6-wet-policy` proves density1 + 2500 CPU items but zero captured rain vertices. The canonical harness calls `freeze_time 1` (device clock0); stationary newborn drops above the downward-facing camera are not a runtime rain test. Dedicated scenario copies unfreeze/refreeze the device while keeping weather paused; their hashes are recorded and admission is always diagnostic. Canonical fixture/save/profile files are preserved.
- `20260927-e06-S6-wet-live` exits0; 631 capture samples, maximum4808 falling vertices and30 splash vertices (two authored meshes). Falling draws logged at samples1/300/600. Its CPU splash capture is positive, but the original sparse draw log sampled rain frames and missed rare splashes; current code logs the first successful splash draw separately.
- `20260927-e06-S6-wet-live-validation` exits0 with API/shader validation and zero reports. It captures up to15 splash vertices, but no falling vertices in this slow validation run; do not assign falling-PSO validation coverage from that run.
- Integrator corrected a source-confirmed order mismatch: Windows water/forward/rain/particles precede `phase_bloom→phase_luminance`; Metal previously did luminance before water. Luminance now follows forward families and precedes distortion/scope/SSFX bloom. SSFX bloom also now precedes nightvision, matching the original relative order. Broader DOF/LUT/NV coverage remains in the matrix.

`20260927-e06-S6-luma-final-validation` exercises the final luminance-source/AO/IL/rain packet: engine exit0, frame128, API/shader validation enabled, zero reports, scenarioOK, screenshots and movie present. It logs `rt_Bloom1_RGBA8` chain256→64→8→1, successful2500-drop draw and15000-vertex/12000-triangle splash draw. [Compact result](../../scratch/release_runs/20260927-e06-S6-luma-final-validation/e06-final-summary.json). This is validation of the active diagnostic graph, not Windows image parity or performance acceptance.

Final normal composite `20260927-e06-S3-composite-final` exits0, frame1129, scenarioOK; canonical S3/save/scenario captures1920×1080 screenshots and15s motion on the same final binary as luma-validation. Water229 draws/waves512², AO/IL5+5, luminance256→64→8→1 and SSFX bloom15-pass chain run together. [Scene](../../scratch/release_runs/20260927-e06-S3-composite-final/scene_no_hud.png), [10s motion sample](../../scratch/release_runs/20260927-e06-S3-composite-final/composite-motion-10s.png). Visual spot-check of the screenshot and10s motion sample shows the aerial industrial/marsh scene remains dark; this view does not expose a useful shoreline close-up. No white sheet is apparent, but shoreline softness/reflection match cannot be accepted from these frames. This is current native diagnostic evidence; no matched Windows image exists.

**Next critical 6D contract:** [E06_SSR_CONTRACT.md](E06_SSR_CONTRACT.md). General SSR is active even at quality0 (8 steps/max-hit72), fully separate from water, and currently absent in Metal. Original winning HLSL cook fails in DXC at `screenspace_reflections.h:57` on `[unroll(q_ssr_steps[SSFX_SSR_QUALITY].x)]`. HV2021 does not repair that syntax and adds a legacy-vector-ternary incompatibility. No source/profile HLSL was changed. A proposal-only copied source folds the annotation to `[unroll(8)]` for active quality0; all5 shaders pass DXC→MSC, loop bounds/ray math remain unchanged. [Exact proposal diff](../../scratch/release_runs/20260927-e06-start/SSR_ANNOTATION_PROPOSAL.patch) and [manifest](../../scratch/release_runs/20260927-e06-start/SSR_ANNOTATION_PROPOSAL.json) are reviewable; they are not runtime-admitted. User clarification is pending because plan §10 requires original HLSL.

Latest AO/IL packet `20260927-e06-S6-up-final-validation` exits0 with API/shader validation enabled, zero reports, scenarioOK and complete screenshots/movie. Successful falling draw submits2500drops/10000vertices and successful splash draw submits15000vertices/12000triangles. Both PSOs are now runtime/validation-exercised; this upward diagnostic camera is not a Windows comparison or rain visual acceptance. Subsequent luma-source packet runtime is separate below.

Ground diagnostic `20260927-e06-S6-ground-final-validation`: engine exits0, scenarioOK, zero API/shader validation reports, positive successful splash draw at frame96 (7740vertices/6192triangles, dropped0). Up to10980 splash vertices/26352indices captured. **Fixture capture incomplete**: a60s video exceeded the runner's30s acknowledgement timeout, so no motion artifact; it is shader-draw evidence only, not complete visual evidence. Runner now derives the ack timeout as motion seconds+15 (canonical15→30 unchanged, diagnostic60→75); bash syntax checked. The later upward-view15s validation run is intended to exercise newborn falling droplets despite slow validation advancing absolute lifetimes faster than capped frame-delta motion.

Complete emitter/material/atmospheric coverage, old NaN attribution, same-fixture Windows reference and V2 remain OPEN. No CPU/GPU/present before/after performance acceptance is claimed for this packet. Remaining active fog/DOF/sun/local volumetric, materials and HUD gaps are listed in [PARITY_MATRIX.md](PARITY_MATRIX.md).


## Bar circle regression / stop

Confirmed semantic AO/IL texture/sampler swaps and hardcoded combine controls were corrected against named DXIL tables. Main build, FrameUploadGpu probe and Bar API/GPU-validation run passed; both executable copies restored to corrected SHA4282715a54a91b89b3df7b9b98bb419f50cd2e1dd60333cbe976506e7b142e0e. The user provisionally observed circles gone, reported other problems and requested stopping before the exact Street corrected capture. [Full handoff and evidence](E06_CIRCLES_FIX.md).
