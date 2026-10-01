# R1 policy: observed membership and bounded correctness controls

2026-09-29. Read-only source/data audit by Sol. No production edit, build, game launch or GPU work by this agent. The two frozen R1 patches are unchanged.

## The admitted OFF observation

Evidence: `runs/r1-policy-off-01/native/output/r1-observer-summary.json`, engine frame360, completed E01 serial360, device phase1 at wall50042ms. The fixed camera is (-221.082565,-18.284866,-168.182037), forward +Z, 1920x1200, l01_escape/w_clear2. The summary contains 90 captured parents /430 records, including 59 source-static parents /354 records. These captured totals are not final rendered counts.

The actual final-plan join has zero missing-parent, static-label or predicate contradictions. All 37 rejected rendered source-static parents have precisely face ordinals0..5: 222 faces. The 18 kept rendered parents are nine dynamic point/Omni parents and nine dynamic spots: 63 faces. Three of those spots have volumetric enabled. The 59 captured static parents all have HUD=0 and volumetric=0 in this particular frame; this does not redefine the source setting's contract.

| Existing work unit | All rendered faces | Source-policy rejected faces | Kept faces |
|---|---:|---:|---:|
| Material rows | 7,571 | 4,999 | 2,572 |
| Main static-geometry indexed draws | 1,210 | 856 | 354 |
| Submitted dynamic-caster proxy | 1,647 | 964 | 683 |
| Flora shadow draws | 2,963 | 2,161 | 802 |
| Static-transform payload bytes | 1,938,176 | 1,279,744 | 658,432 |

The whole completed frame has20,920 indexed draws,14,840 PSO requests and27,894,108 updated bytes. The table partitions its selected shadow work; it is not a time estimate. Source-static describes the light parent. A disabled static light also stops casting shadows from dynamic objects, exactly as Windows' whole-package admission does. No FPS claim follows from this table or the overlapping local-shadow CPU scope.

Root reports ON frame360 at wall44648ms in phase0. Keep it as warmup membership evidence only. `summarize_observer.py` deliberately requires phase1 and must continue rejecting that record. Do not compare its work counts with OFF frame360 as a matched simulation/calendar A/B. Ordinary observer-free phase1 timing and setting correctness are separate evidence.

## Source contract and stale-contribution boundary

`Light_DB.cpp:199` rejects `bStatic && !R2FLAG_R1LIGHTS` before `export_()`. The source setting disables the entire point/spot package; it has no HUD/volumetric exception. Sun/reflected handling, level geometry and lightmaps are separate. The seven-line candidate applies that exact predicate before the active End capture appends or expands a parent. It neither deletes engine light objects nor changes shader constants, geometry, wind, motion, material resources, shadows of accepted lights or lightmap textures.

The R1 bit is read live on each frame; the environment opt-in is captured once. With R1 enabled the predicate is false for every source-static parent and the former append path runs unchanged. Dynamic parents always pass this predicate.

The bridge submits a new value snapshot every frame (`MetalRenderDeviceRender.mm:1228`); `MetalR4Backend.mm:3289` copies and replaces the shared snapshot even if its light vector is empty. `MetalR4EngineScene.mm:4846` clears the accumulator before sun/local work. The local planner and accumulation iterate that current snapshot, and each volumetric pass is built only from the just-rendered batch (`:5193`). An omitted parent therefore has no valid stale atlas consumer in this graph. Test the transition visibly; this source audit does not certify all runtime transitions.

Parent/light IDs are pointer-derived and stable only within one engine process (`IR4Backend.h:530`). The observer join is correct within a frame. Never treat IDs from two separately launched runs as persistent scene IDs.

## Small existing image/GPU seams

No additional renderer is needed for the first correctness pass:

```text
XRAY_METAL_COLOR_PROBE_DIR=<isolated correctness output>/color
XRAY_METAL_COLOR_PROBE_EVERY=700
XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_DIR=<isolated correctness output>/local
XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_EVERY=700
```

`COLOR_PROBE` captures actual production G-buffer-world, sun/emissive accumulator before local lighting, post-local accumulator, base combine and scene-before-postprocess. `MetalR4MotionValidation.mm:946` analyzes readbacks only after successful command completion, retaining referenced textures through that callback. It writes original-value finite/nonzero/min/max metrics and tone-mapped PPM previews. It does not persist raw HDR/depth/stencil. Its exact-pair mode compares two textures in one descriptor; it is not an off/on whole-scene replay seam.

`LOCAL_SHADOW_BATCH_PROBE` at `MetalR4EngineScene.mm:5224` snapshots the actual current atlas after that batch's accumulation and volumetric consumers, before a later batch overwrites it. `MetalR4LocalShadowBatchValidation.mm:384` writes row-packed raw R32 atlas and RGBA16F accumulator; JSON includes current light/parent/face identity, tile coverage/hash, receivers in the light frustum, finite counts, accepted view/projection, primary-depth hash and geometry-stencil-bit hash. Depth/stencil readbacks are analyzed but are not written as raw files. The accumulator at batch N already contains sun/emissive and earlier batches; it is not that light's isolated contribution. Tile/batch indices may change legitimately when static packages are removed.

The batch validator requires some nonclear atlas coverage per batch. A correctly admitted batch of empty shadow casters can report `pass=false`; inspect `gpu_completed`, finite data and explicit all-clear coverage rather than mislabeling that alone as a renderer failure. Nonzero receivers/coverage do not independently prove each light's nonzero radiance. Use actual accumulator differences and a visible dynamic positive control as well.

Capture cost excludes these frames from performance evidence. A color interval of700 also captures camera cuts and AO/IL forced frames; it is not a strict one-frame selector. Bound the run, retain those extra reports with their labels, and use the device phase ledger to label frame700 accurately. No repeated live capture during the performance screen is proposed.

## Minimum qualification matrix after the current run

| Control | Expected contract | Small witness |
|---|---|---|
| Policy opt-in=1, `r2_allow_r1_lights off` | No captured source-static parents/packages; accepted dynamic parents retained | Current snapshot/static count, completed final-plan membership and dynamic positive lighting/atlas witness |
| Policy opt-in=1, `r2_allow_r1_lights on` | Static packages restored through the unchanged append path | Positive static package count plus visible static-light contribution; do not use an all-dark scene |
| Policy opt-in absent, `r2_allow_r1_lights on` | Same source path as preceding control | Matched pre-local and post-local images on static, nonmoving receiver regions; exact source predicate proof applies, live-run pixels remain subject to input drift |
| In-process R1 on ->off ->on in a separate correctness tail | No surviving static contribution after omission; contribution returns after re-enable; dynamic sources remain | Existing per300-frame normal capture count and bounded color/batch captures; identities can be joined only within this process |

The current observer is fixed to frame360. Do not silently change it or its parser to obtain frame700 membership evidence. If detailed per-parent membership at700 is necessary, make a separately reviewed observer-only frame selector after this run; no renderer change is required. Otherwise the existing normal static-record counts and the batch JSON show source setting and final shadow membership at selected frames, with the limitation that batch JSON has no static source label.

Run these as source-setting controls using Root's existing camera/save/weather harness. Its Lua already calls `get_console():execute`, so setting `r2_allow_r1_lights` at a labelled control boundary requires no engine implementation. Record the live setting at that boundary rather than trusting only initial user.ltx. Frame700 may be phase1 or a later correctness tail; label it from actual phase data, never force it into the measured cohort.

For baked-light preservation, inspect unchanged static material/lightmap admission and pre-local `gbuffer_world` diffuse plus `sun_accumulator`/`emissive_accumulator` on nonmoving wall/floor regions. The candidate cannot touch these paths; the existing LMH route explicitly binds the qualified `lmap#` texture (`MetalBootstrap.mm:2039`). Across live runs, animated flora/NPCs and timing can change images, so do not claim whole-frame byte equality from PPMs. Post-local illumination should change with R1 off because former native output was incorrect; the accepted target is the matched Windows setting. A controlled dynamic lamp/flashlight must retain positive output, and a static-light positive control must actually restore output.

Check one reload/initial frame and an on->off->on transition, empty-static-package handling, ordinary dynamic point+spot, and the observed volumetric dynamic spots. HUD/3DSS/scope should remain visibly usable because accepted dynamic packages follow unchanged paths. No geometric exemption is added to this explicit source setting. Any future fog/frustum/LOD policy receives its own observer, consumer review and image gates.

Root subsequently reports a completed live control: R1-on frame360 includes59 static parents/354 records (91/436 total), with actual static shadow faces. The live flag was switched off atframe2001 after the measurement window; subsequent captures2100/2400/2700 contain static0,94 records,81 shadowed plus7 unshadowed dynamic admitted. The main wall/HUD image persists without obvious corruption. These are Root's reported control receipts, not independently joined here and not broad visual acceptance.

Admission remains bounded: source/config parity and OFF membership are proven; ON measured timing is Root's separate evidence; the reported on->off control supports package restoration/removal and dynamic preservation. Baked/pre-local numeric image comparison and any broader visual acceptance remain separate. No raw full-frame depth/stencil off/on equality or cross-backend exact image oracle is claimed here.
