# R4 Windows vs Native shadow-resolution triage

**Verdict: reject a shadow-resolution reduction as a Native performance candidate.** This check uses the Windows **R4/DX11** source path, not R2 alone. It is a source/configuration comparison; it does not establish same-frame visual parity or measured GPU cost.

## R4 source comparison

| Shadow family | Windows R4/DX11 | Direct Metal R4 | Finding |
|---|---|---|---|
| Direct sun | R4 startup defaults `o.smapsize` to 2048. R4 permits explicit `-smap1536`, `-smap2048`, `-smap2560`, `-smap3072`, and `-smap4096` overrides. R4 render-target allocation uses `o.smapsize`. | `MetalRenderDeviceRender` passes 2048 to the cascade builder; `MetalBootstrap` allocates the three cascade R32 maps and shared depth at 2048. | Equal at the canonical default, unless an unrecorded Windows startup override changed its pool size. |
| Local spot/omni | R4 `r2_types.h` sets adaptive per-light bounds to 768 minimum, 768 optimal, 1536 maximum. The shared `light.cpp` size function is included by the R4 target and clamps each visible non-HUD light into that range. `r2_ls_squality` and camera/light factors choose a point in the range. The shared light-render loop is also included by R4; it packs rectangles into `o.smapsize` pools and repeats pool IDs until every visible shadowed light has been assigned. | Fixed 2048x2048 atlas with 512x512 tiles, capacity 16; `PlanR4LocalShadowBatches` groups lights, while the scene planner excludes inactive/invalid and conservatively outside-main-view lights. | Native local shadows are already below the Windows R4 per-light minimum. No pool-capacity admission advantage to recover was found. |

The canonical settings file is `scratch/gameplay-rescue-20260929/runs/v2-canonical-oracle-01/runtime/appdata/user.ltx`, SHA-256 `b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334`. It sets `r2_ls_squality 1.` and `ssfx_shadow_cascades (20,60,160)`. Those cascade numbers are world-space extents, not texel dimensions; prior Native `METAL_SUN_CASCADE_FRAME` records show those same sizes. The file's `ssfx_shadows (256,1536,0)` is not read by the current C++ source under `src`; the operative local-map formula is `ps_r2_ls_squality` plus the hard 768..1536 clamp.

The R4-specific source facts are:

- `src/Layers/xrRenderPC_R4/r4.cpp:230-231,368-373`: Windows R4 defaults the pool to 2048 and applies only explicit startup `-smap*` overrides; the Direct Metal branch sets `o.smapsize=2048` before its early return.
- `src/Layers/xrRenderPC_R4/r4_rendertarget.cpp:710-720`: the Windows R4 shadow target dimensions derive from `o.smapsize`.
- `src/Layers/xrRenderPC_R4/r2_types.h:127-129`: R4 local-shadow adaptive bounds are 768..1536.
- `src/Layers/xrRender/light.cpp:491-539`: shared R4-compiled `optimize_smap_size()` uses camera distance, light sphere/range, camera-light direction, cone, and `r2_ls_squality`, then clamps to those bounds. Its only early bypass is HUD mode.
- `src/Layers/xrRender/lights_render.cpp:92-137`: shared R4-compiled loop calls that size selector for visible shadowed lights and continues allocating additional pool IDs until the complete visible set has been assigned.
- `src/Layers/xrRenderPC_R4/CMake/XRay.Render.R4.cmake:358-374`: confirms the R4 target compiles the shared `light.cpp` and `lights_render.cpp` used above.
- `src/Layers/xrRenderMetal/MetalRenderDeviceRender.mm:1536-1538` and `src/Layers/xrRenderMetal/MetalBootstrap.mm:3182-3220`: Native direct-sun cascade resolution is 2048, and Native sun maps/depth are allocated at 2048.
- `src/Layers/xrRenderMetal/MetalR4EngineScene.mm:251-252,2518-2527`: Native local atlas is 2048 with 512 tiles and capacity 16. `MetalR4EngineScene.mm:2590-2630` shows its candidate planning/filtering path.

`runs/portal-upper-bound-on-01/native/output/engine.log` provides bounded historical corroboration, not current-run or visual evidence: five sampled `METAL_LOCAL_SHADOW_FRAME` records report candidates equal to rendered (69/69, 63/63, 63/63, 75/75, 75/75) with 512 tiles and 5–6 batches. Timings were not combined or used in this verdict.

## Decision and limits

Reducing Native local tiles below 512 would further reduce light-map detail against a Windows R4 path whose minimum is 768. The likely player-facing consequence is lower-resolution/softer or more visibly texel-aliased local shadows on Native; this is a source-based quality risk, not a measured matched-image result. Direct-sun dimensions already match the R4 default, so reducing them has no source-supported parity case either.

No resolution/admission optimization, generic quality adjustment, or new observer is recommended for this branch. A final matched visual-quality claim would require an actual stock Windows R4/DX11 runtime capture with effective `o.smapsize`/per-light `X.S.size`, joined to the same Native lights and camera. The canonical `user.ltx` does not record whether a Windows process supplied a `-smap*` startup override, so only the source-default comparison is proven here.

No source, build output, or game state was changed. This report is scratch-only and read-only source/log analysis; no new build or run was performed.

## Input hashes

| Input | SHA-256 |
|---|---|
| `src/Layers/xrRenderPC_R4/r4.cpp` | `65500f4ca09f208dc4643643aa8ab4944bc42741ce0be1348d8a8d848d0f650c` |
| `src/Layers/xrRenderPC_R4/r4_rendertarget.cpp` | `07f3a2ed748eeccc7a970bca68987f94438350988604a908637a3ac1523aa531` |
| `src/Layers/xrRenderPC_R4/r2_types.h` | `63ec508a6248c9ebf8331eca612703f90a07d16533a13174281d3c5e88d286e2` |
| `src/Layers/xrRender/light.cpp` | `f7f47e5f459f8d6c0b0911b642b77d474622ba4587a802f63660549bdb80b5a0` |
| `src/Layers/xrRender/lights_render.cpp` | `cb78a4447055824e293a3ca924a789e5313548e305c496928da9e1d7a3536dac` |
| `src/Layers/xrRenderPC_R4/CMake/XRay.Render.R4.cmake` | `1ed95055161b5ff85c2ed9d25452a6372ec2f56527c89cf6b3fc37fa633e6c97` |
| `src/Layers/xrRenderMetal/MetalRenderDeviceRender.mm` | `1087cfd01656b83d5fe0386d7730d193cea329ff3d2d86810f97b907edd048f9` |
| `src/Layers/xrRenderMetal/MetalBootstrap.mm` | `fb1b1052704eab1756bb12bdbc8bcdd46f96c5962481de1aa5e59c2b793ad27c` |
| `src/Layers/xrRenderMetal/MetalR4EngineScene.mm` | `c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53` |
| `scratch/gameplay-rescue-20260929/runs/v2-canonical-oracle-01/runtime/appdata/user.ltx` | `b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334` |
| `scratch/native-silo-parity-20260929/runs/portal-upper-bound-on-01/native/output/engine.log` | `926c5be0b9051ac55f53846ebf2caa44d9274707a0b644724c2b1df306cb2e34` |
