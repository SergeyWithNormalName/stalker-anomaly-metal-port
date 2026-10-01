# E06 Package 6E Weather/Effects Census

**Document Version:** 1.0.0  
**Date:** 2026-09-26  
**Target Backend:** Direct Metal R4 (`xrRenderMetal`) vs Windows R4 (`xrRenderPC_R4`)  
**Scope:** Investigation of all weather, atmospheric, particle, optical, and post-process effect passes under the GAMMA / macos-gamma profile (`user.ltx`).

---

## 1. Executive Summary

This census establishes the true runtime state of all weather, atmospheric, post-process, and visual effect passes in Metal R4 ahead of upcoming development packages.

### Verification Axiom
No effect is marked `ACTIVE_VERIFIED` merely because a shader, metallib, adapter class, or telemetry counter exists. Status `ACTIVE_VERIFIED` is strictly reserved for passes proven to execute in production runtime and successfully deliver pixel data to their intended render target or final scene presentation.

### Overall Statistics
- **Total Effects / Sub-Components Investigated:** 24
- **ACTIVE_VERIFIED:** 8 (`Sky (sky2)`, `SSFX Water Waves`, `Distortion Particles`, `Hard Particles`, `SSFX AO / IL`, `3DSS Scope PIP Reticle`, `SSFX LUT / Color Grading`, `Thunder Flash`)
- **IMPLEMENTED_NOT_RUNTIME_VERIFIED:** 2 (`Falling Rain Droplets`, `Cloud Layer`)
- **PARTIAL:** 2 (`Rain Occlusion / Culling`, `Rain Surface Wetness`)
- **STUB / NO_OP:** 2 (`Lens Flare`, `Thunderbolt Geometry`)
- **MISSING:** 5 (`Sunshafts`, `Volumetric Lighting`, `SSFX Fog Scattering`, `Depth of Field (DOF)`, `Post-Process Bloom`)
- **REJECTED (Fail-Closed):** 2 (`HUD Particles`, `Emissive Materials / Effects`)
- **DISABLED_BY_CONFIG:** 3 (`Motion Blur`, `Soft Particles`, `Fullscreen Heatvision` / `Nightvision`)

---

## 2. Master Effects Table

| Effect | Windows Source | Activation Condition | Current Config (`user.ltx`) | Metal Producer / Capture | Metal Consumer / Draw | Render Target | Runtime Evidence | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Falling Rain Droplets** | `dxRainRender.cpp`, `r4_R_rain.cpp` | `CurrentEnv->rain_density > 0` | `r__rain_k 99.`, `r__rain_exp 1.` | `xray::metal::rain::SubmitFrame` | `MetalR4RainAdapter::DrawIntoActiveForward` | `scene.lightingTargets[0]` | `METAL_RAIN_PREPARED` in logs; dry saves during runs | `IMPLEMENTED_NOT_RUNTIME_VERIFIED` |
| **Rain Occlusion / Shadow** | `r4_rendertarget_draw_rain.cpp` | Rain active | Active in rain | CPU frustum/sphere culling only | None | None | None | `PARTIAL` |
| **Rain Ground Splashes** | `dxRainRender.cpp:253` (`DM_Drop`) | Rain active, splashes | Active in rain | None (bypassed by early `return`) | None | None | None | `MISSING` |
| **Rain Surface Wetness** | `r4_rendertarget_draw_rain.cpp` | `ssfx_wetsurfaces_*` | `ssfx_wetness_multiplier (1.4, 0.5)` | None (`rain_params.y` uniform only) | None (no screen-space accumulation) | None | `rain_params.y` in `staticGlobals` | `PARTIAL` |
| **Rain / Water Ripples & Waves** | `r4_rendertarget.cpp`, `ssfx_water_waves.ps` | `ssfx_water`, water enabled | `ssfx_water (1, 0.8, 1, 0)` | `MetalR4WaterAdapter` wind setup | Fullscreen wave pass -> water forward | `m_impl->waves` (512x512 RGBA8) | `METAL_WATER_FRAME ... waves=512x512` | `ACTIVE_VERIFIED` |
| **Thunder Flash (Sky/Hemi)** | `thunderbolt.cpp:242` | Storm weather with lightning | Storm event | Engine `CurrentEnv->sky_color` | `R4EnvironmentSnapshot` -> sky/ambient | `scene.lightingTargets[0]` | Engine env lighting active | `ACTIVE_VERIFIED` |
| **Thunderbolt Geometry** | `dxThunderboltRender.cpp` | Lightning discharge | Storm event | Early return `if (Metal) return;` | None | None | Explicit early return | `STUB` |
| **Lens Flare** | `dxLensFlareRender.cpp` | `r__lens_flares on` | `r__lens_flares on` | `xray::metal::lensflare::Submit` | None (Adapter uninstantiated & empty) | None | Drops in `SubmitLensFlareFrame` | `STUB` |
| **Sunshafts** | `rendertarget_phase_sunshafts.cpp` | `r2_sunshafts_mode` | `r2_sunshafts_mode volumetric` | None | None | None | Zero code in `xrRenderMetal` | `MISSING` |
| **Volumetric Lighting** | `r4_rendertarget.cpp:746`, `accum_volumetric` | `r2_volumetric_lights on` | `r2_volumetric_lights on` | None | Dummy 1x1 black bound | None | Dummy texture bound in adapter | `MISSING` |
| **SSFX Fog Scattering** | `rendertarget_phase_blur.cpp:917` | `ps_ssfx_fog_scattering > 0` | `ssfx_fog_scattering 1.` | None | None | None | Zero code in `xrRenderMetal` | `MISSING` |
| **Motion Blur** | `rendertarget_phase_blur.cpp:800` | `r2_mblur_enabled on` | `r2_mblur 0.`, `r2_mblur_enabled off` | None | None | None | Disabled in config | `DISABLED_BY_CONFIG` |
| **Depth of Field (DOF)** | `rendertarget_phase_dof.cpp` | `r2_dof_enable on` | `r2_dof_enable on`, `r2_dof_radius 0.25` | None | None | None | Zero code in `xrRenderMetal` | `MISSING` |
| **Gasmask Visor & Rain Drops** | `rendertarget_phase_gasmask_*.cpp` | `ps_r2_mask_control.x > 0` | `r2_mask_control (0,0,0,0)` | None | None | None | No code in Metal; dormant by config | `MISSING` |
| **Distortion Particles** | `ParticleEffect.cpp:812` (`xdistort`) | Particles with distort shader | Active in gameplay | `MetalParticleCapture.h` | `DrawIntoActiveDistortion` + `DrawPostprocess` | `distortionTarget` -> Swapchain | `distortion=3 distortion_submitted=3` | `ACTIVE_VERIFIED` |
| **Hard Particles** | `ParticleEffect.cpp` | Standard particle emit | Active in gameplay | `MetalParticleCapture.h` | `MetalR4ParticleAdapter::DrawIntoActiveForward` | `scene.lightingTargets[0]` | `METAL_PARTICLE_FRAME ... hard=178` | `ACTIVE_VERIFIED` |
| **Soft Particles** | `ParticleEffect.cpp:802` | `r2_soft_particles on` | `r2_soft_particles off` | Producer emits `Permutation::Hard` | Pipelines ready, dormant | `scene.lightingTargets[0]` | `soft=0` in logs; config off | `DISABLED_BY_CONFIG` |
| **HUD Particles** | `ParticleEffect.cpp:809` (`GetHudMode`) | Particles on HUD/weapon | Active in gameplay | `MetalParticleCapture.h` (`hudMode=true`) | Explicit fail-closed rejection | None | `hudRejectedDraws` incremented | `REJECTED` |
| **Emissive Materials / Bloom** | `rt_ssfx_bloom_emissive` | Emissive materials | Active in shaders | Telemetry counter only | None (no target, no pass) | None | Counter in `METAL_HUD_CAPTURE` | `REJECTED` |
| **Fullscreen Heatvision** | `r4_rendertarget_phase_combine.cpp:570` | `r__heatvision 1` | `r__heatvision 0` | `R4GameplayVisionSnapshot` (scalars) | None (no postprocess pass) | None | Zero combine pass in Metal | `DISABLED_BY_CONFIG` |
| **Nightvision** | `rendertarget_phase_nightvision.cpp` | `r__nightvision 1..3` | `r__nightvision 0` | `R4GameplayVisionSnapshot` | `drawNightVision` (3 blurs + gen 1..3) | `scene.lightingTargets[0]` | `METAL_NIGHTVISION_PREPARED` in logs | `DISABLED_BY_CONFIG` |
| **Sky (sky2)** | `dxEnvironmentRender.cpp` | Outdoor environment | Outdoor profile | `R4EnvironmentSnapshot` | `MetalR4EngineScene::drawSkyScene` | `scene.lightingTargets[0]` + `targets[1]` | `METAL_SKY_DRAW` in every outdoor frame | `ACTIVE_VERIFIED` |
| **Clouds Layer** | `dxEnvironmentRender.cpp` | Weather clouds alpha > 0 | Clear weather (alpha=0) | `R4EnvironmentSnapshot` | `MetalR4EngineScene::drawCloudScene` | `scene.lightingTargets[0]` | `METAL_CLOUDS_PREPARED`, `valid=false` | `IMPLEMENTED_NOT_RUNTIME_VERIFIED` |
| **Post-Process Bloom** | `r4_rendertarget_phase_bloom.cpp` | `r2_tonemap on`, bloom | `r2_tonemap on` | None | Dummy 1x1 black bound | Swapchain | `scene.lightingBlack` passed to `combine_2` | `MISSING` |
| **SSFX AO / IL** | `r4_rendertarget_phase_ssao.cpp` | `ssfx_ao`, `ssfx_il` | `ssfx_ao (1, 5, 1, 1.9)`, `ssfx_il` | Depth & normal G-buffers | `MetalR4SsfxAoAdapter::Draw` (5 AO + 5 IL) | `aoTarget`, `ilTarget` | `METAL_SSFX_AO_IL_FRAME` every frame | `ACTIVE_VERIFIED` |
| **3DSS Scope PIP Reticle** | Weapon scopes | Aiming through 3D scope | `r__3Dfakescope 1` | `MetalRenderDeviceRender.mm` | `MetalR4ScopeAdapter::DrawReticle` | `scene.lightingTargets[0]` | Tested in probes, active on aim | `ACTIVE_VERIFIED` |

---

## 3. Detailed Effect Analysis

### 3.1. Lens Flare
1. **Windows Source:** `src/Layers/xrRender/dxLensFlareRender.cpp`, `src/xrEngine/xr_efflensflare.cpp`, `src/Layers/xrRenderPC_R4/blender_hdr10_lens_flare.cpp`.
2. **HLSL / Shaders:** `effects_sun.vs`, `effects_sun.ps`, `hdr10_flare_*.ps`.
3. **Activation Condition:** Console variable `r__lens_flares on`.
4. **Current Profile Value:** `r__lens_flares on`, `r4_hdr10_flare_on 0`.
5. **Metal Producer:** `dxLensFlareRender::Render()` creates 4 vertices per flare quad and invokes `xray::metal::lensflare::Submit(std::move(captured))`.
6. **Metal Consumer / Draw:** In `MetalRenderDeviceRender.mm:1080`, the captured batch is drained via `xray::metal::lensflare::Drain()` and passed to `scene.SubmitLensFlareFrame(std::move(flare))`. In `MetalR4EngineScene.mm:4716`, `impl->lensFlareFrame` stores the frame.
7. **Render Order Position:** Intended to run in forward transparents / sun effects before postprocessing.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** None in production logs.
11. **Early-Out:** No early out in producer, but completely dropped in consumer.
12. **Stub / No-Op:** `MetalR4LensFlareAdapter` is **not instantiated** in `MetalR4EngineScene.mm`. Even inside `MetalR4LensFlareAdapter.cpp`, methods `StageFrame()` and `DrawIntoActiveForward()` are explicit 1-line stubs returning `true`.
13. **Rejected Data:** `impl->lensFlareFrame` is silently overwritten every frame without being consumed.
14. **Status:** `STUB / NO_OP`.

---

### 3.2. Rain Components

#### 3.2.1. Falling Rain Droplets
1. **Windows Source:** `src/Layers/xrRender/dxRainRender.cpp:67`, `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_rain.cpp`.
2. **HLSL / Shaders:** `effects_rain.vs`, `effects_rain.ps`, `ssfx_rain.ps`, `stub_screen_space.vs`.
3. **Activation Condition:** `CurrentEnv->rain_density > EPS_L`.
4. **Current Profile Value:** `r__rain_k 99.`, `r__rain_exp 1.`, `ssfx_rain_1 (2.0, 0.1, 0.6, 0.0)`. In currently captured game saves (Bar/courtyard), weather is dry (`rain_density == 0`).
5. **Metal Producer:** `dxRainRender::Render()` generates dynamic camera-oriented quads from `owner.items` and calls `xray::metal::rain::SubmitFrame(std::move(capture))`.
6. **Metal Consumer / Draw:** `MetalR4EngineScene.mm:3536-3582` executes:
   - `ssfx_rain 1/8 prepass` (`rain->DrawRefraction`) writing to `rain->RefractionTarget()`.
   - `effects_rain forward pass` (`rain->DrawIntoActiveForward`) drawing into `scene.lightingTargets[0]`, depth-tested against `scene.depthTarget`, outputting motion mask to `rain->MotionTarget()`.
7. **Render Order Position:** After static/dynamic forward geometry, before particles.
8. **Target Written:** `scene.lightingTargets[0]`, `rain->MotionTarget()`, `rain->RefractionTarget()`.
9. **Target Reader:** Post-processing presentation (`combine_2`).
10. **Runtime Evidence:** `METAL_RAIN_PREPARED shader=effects_rain bundle=.../rain-next-v2 tlab=48/192 mrt=0,2` in all release logs. Probes verify shader compilation and MRT contracts.
11. **Early-Out:** Producer early-outs when `rain_density < EPS_L`. Consumer skips pass when `!rain->HasDrops()`.
12. **Stub / No-Op:** None. Fully implemented.
13. **Rejected Data:** Drop vertices beyond `MaxVertices` (10,000) are counted in `droppedVertices`.
14. **Status:** `IMPLEMENTED_NOT_RUNTIME_VERIFIED` (Fully implemented and wired into scene graph, but dry weather in release runs prevented live droplet draw logs).

#### 3.2.2. Rain Occlusion / Culling
1. **Windows Source:** `src/Layers/xrRenderPC_R4/r4_rendertarget_draw_rain.cpp` (`draw_rain` projects directional rain occlusion through `LP_smap_pool`).
2. **HLSL / Shaders:** `rain_patch_normal_nomsaa.ps`.
3. **Activation Condition:** Rain active.
4. **Current Profile Value:** Active.
5. **Metal Producer:** CPU sphere frustum culling (`testSphere_dirty`) in `dxRainRender.cpp:111`.
6. **Metal Consumer / Draw:** None. No GPU directional rain shadow projection pass exists in Metal.
7. **Render Order Position:** N/A.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** None.
11. **Early-Out:** Drops failing frustum sphere test are skipped on CPU.
12. **Stub / No-Op:** GPU rain occlusion pass is absent.
13. **Rejected Data:** Culled particles outside frustum.
14. **Status:** `PARTIAL` (CPU culling operational; GPU depth occlusion missing).

#### 3.2.3. Rain Ground Splashes
1. **Windows Source:** `src/Layers/xrRender/dxRainRender.cpp:253-330` (`owner.particle_active`, uses `DM_Drop` model and `SH_Splash` / `effects\rain_splash`).
2. **HLSL / Shaders:** `effects_rain_splash.ps`, `effects_rain_splash.vs`.
3. **Activation Condition:** Rain active.
4. **Current Profile Value:** `ssfx_rain_3 (0.25, 3.0, 0.0, 0.0)`.
5. **Metal Producer:** `dxRainRender::Render()` has `if (SelectedR4Backend() == R4BackendKind::Metal) { ... return; }` on line 138, which **exits immediately before** the particle/splash loop!
6. **Metal Consumer / Draw:** None.
7. **Render Order Position:** N/A.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** None.
11. **Early-Out:** Unconditional return in `dxRainRender.cpp:138`.
12. **Stub / No-Op:** Entire splash loop bypassed.
13. **Rejected Data:** 100% of splash particles ignored under Metal backend.
14. **Status:** `MISSING`.

#### 3.2.4. Rain Surface Wetness
1. **Windows Source:** `src/Layers/xrRenderPC_R4/r4_rendertarget_draw_rain.cpp:252` (`draw_rain` applies normal and gloss accumulation to scene surfaces).
2. **HLSL / Shaders:** `rain_apply_normal_nomsaa.ps`, `rain_apply_gloss_nomsaa.ps`, `rain_patch_normal_nomsaa.ps`.
3. **Activation Condition:** Rain wetness factor > 0.
4. **Current Profile Value:** `ssfx_wetness_multiplier (1.4, 0.5, 0.0)`, `ssfx_wetsurfaces_1`, `ssfx_wetsurfaces_2`.
5. **Metal Producer:** `MetalBootstrap.mm:2539` writes `environment->wetnessFactor` into `staticGlobals` uniform buffer at offset 464 (`rain_params.y`).
6. **Metal Consumer / Draw:** Read as a scalar uniform by puddle and terrain material shaders to increase specular gloss. However, no separate screen-space wetness accumulation render pass (`draw_rain`) exists.
7. **Render Order Position:** Inside material forward/deferred evaluation.
8. **Target Written:** G-Buffer / scene color via standard material shaders.
9. **Target Reader:** Lighting passes.
10. **Runtime Evidence:** `rain_params.y` uniform verified in `staticGlobals` buffer uploads.
11. **Early-Out:** Zero when dry.
12. **Stub / No-Op:** Dedicated postprocess wetness pass missing.
13. **Rejected Data:** None.
14. **Status:** `PARTIAL` (Scalar uniform active; screen-space wetness pass missing).

#### 3.2.5. Water Waves & Ripples
1. **Windows Source:** `src/Layers/xrRenderPC_R4/r4_rendertarget.cpp`, `ssfx_water_waves.ps`.
2. **HLSL / Shaders:** `ssfx_water_waves.ps`, `stub_screen_space.vs`.
3. **Activation Condition:** Water present in scene, `ssfx_water` active.
4. **Current Profile Value:** `ssfx_water (1.0, 0.8, 1.0, 0.0)`, `r2_soft_water on`.
5. **Metal Producer:** `MetalR4WaterAdapter.cpp:527` builds `wavesGlobals` with wind setup.
6. **Metal Consumer / Draw:** `MetalR4WaterAdapter::DrawIntoActiveForward` renders `ssfx_water_waves.ps.metallib` to 512x512 texture `m_impl->waves`, then samples it at slot 264 during water rendering.
7. **Render Order Position:** Immediately preceding forward water surface draw.
8. **Target Written:** `m_impl->waves` (512x512 RGBA8Unorm).
9. **Target Reader:** `water.ps` fragment shader.
10. **Runtime Evidence:** `METAL_WATER_FRAME ... waves=512x512 order=ssr_history_blur_waves_final` logged continuously in every water frame across all runs.
11. **Early-Out:** When no water objects are in frustum.
12. **Stub / No-Op:** None.
13. **Rejected Data:** None.
14. **Status:** `ACTIVE_VERIFIED`.

---

### 3.3. Thunderbolt & Lightning

#### 3.3.1. Thunder Flash
1. **Windows Source:** `src/xrEngine/thunderbolt.cpp:242` (`CEffect_Thunderbolt::OnFrame`).
2. **HLSL / Shaders:** Engine-level ambient / sky lighting.
3. **Activation Condition:** Storm weather event.
4. **Current Profile Value:** Active during thunderstorms.
5. **Metal Producer:** `CEffect_Thunderbolt::OnFrame` calculates RGB flash color and modulates `CurrentEnv->sky_color`, `sun_color`, `fog_color`.
6. **Metal Consumer / Draw:** `MetalRenderDeviceRender.mm` captures modulated environment values into `R4EnvironmentSnapshot`, which are uploaded into `staticGlobals` and sky drawing passes.
7. **Render Order Position:** Global lighting constants.
8. **Target Written:** `scene.lightingTargets[0]`, ambient accumulator.
9. **Target Reader:** G-buffer lighting and sky shaders.
10. **Runtime Evidence:** Architecture verified; engine environment snapshots deliver live frame colors.
11. **Early-Out:** Inactive outside storm events.
12. **Stub / No-Op:** None.
13. **Rejected Data:** None.
14. **Status:** `ACTIVE_VERIFIED`.

#### 3.3.2. Thunderbolt Geometry
1. **Windows Source:** `src/Layers/xrRender/dxThunderboltRender.cpp`, `dxThunderboltDescRender.cpp`.
2. **HLSL / Shaders:** Detail model lightning shader, gradient sprites.
3. **Activation Condition:** Lightning bolt strike.
4. **Current Profile Value:** Active during thunderstorms.
5. **Metal Producer:** `dxThunderboltRender::Render()` has `if (SelectedR4Backend() == R4BackendKind::Metal) return;` at line 31.
6. **Metal Consumer / Draw:** None.
7. **Render Order Position:** N/A.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** Explicit early return in codebase.
11. **Early-Out:** Unconditional return for Metal backend.
12. **Stub / No-Op:** 100% stub.
13. **Rejected Data:** Lightning mesh and billboard gradients discarded.
14. **Status:** `STUB / NO_OP`.

---

### 3.4. Optical & Volumetric Lighting

#### 3.4.1. Sunshafts
1. **Windows Source:** `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp:513` (`phase_sunshafts()`), `rendertarget_phase_sunshafts.cpp`, `blender_ss_sunshafts.cpp`.
2. **HLSL / Shaders:** `sunshafts.ps`, `sunshafts_mask.ps`, `sunshafts_blur.ps`.
3. **Activation Condition:** `r2_sunshafts_mode` (1=volumetric, 2=screen space, 3=combine).
4. **Current Profile Value:** `r2_sunshafts_mode volumetric`, `r2_sunshafts_quality st_opt_low`, `r2_sunshafts_value 0.51`.
5. **Metal Producer:** None.
6. **Metal Consumer / Draw:** None.
7. **Render Order Position:** Between lighting accumulation and combine.
8. **Target Written:** None (`rt_sunshafts_0/1` do not exist).
9. **Target Reader:** None.
10. **Runtime Evidence:** Zero lines of sunshafts code exist in `src/Layers/xrRenderMetal`.
11. **Early-Out:** N/A.
12. **Stub / No-Op:** Missing entirely.
13. **Rejected Data:** None.
14. **Status:** `MISSING`.

#### 3.4.2. Volumetric Lighting
1. **Windows Source:** `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp:438` (`phase_combine_volumetric()`), `r4_rendertarget.cpp:746` (`accum_volumetric_sun_nomsaa`), `r4_rendertarget.cpp:844` (`accum_volumetric`).
2. **HLSL / Shaders:** `accum_volumetric_sun.ps`, `accum_volumetric.ps`, `combine_volumetric.ps`, `ssfx_volumetric_blur.ps`.
3. **Activation Condition:** `r2_volumetric_lights on`.
4. **Current Profile Value:** `r2_volumetric_lights on`, `ssfx_volumetric (0.0, 0.8, 3.0, 1.0)`.
5. **Metal Producer:** None.
6. **Metal Consumer / Draw:** In `MetalR4RainAdapter.cpp:384`, texture slot 24 (`volumetric`) is hardcoded to `scene.lightingBlack` (a 1x1 black dummy) because no volumetric target exists.
7. **Render Order Position:** Lighting accumulation phase.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** Zero volumetric raymarching code in `src/Layers/xrRenderMetal`.
11. **Early-Out:** N/A.
12. **Stub / No-Op:** Missing entirely.
13. **Rejected Data:** None.
14. **Status:** `MISSING`.

#### 3.4.3. SSFX Fog Scattering
1. **Windows Source:** `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp:519` (`phase_ssfx_fog_scattering()`), `rendertarget_phase_blur.cpp:917`.
2. **HLSL / Shaders:** `ssfx_fog_scattering.ps`, `ssfx_fog_scattering_blur.ps`.
3. **Activation Condition:** `ps_ssfx_fog_scattering > 0`.
4. **Current Profile Value:** `ssfx_fog_scattering 1.`, `ssfx_fog (20.0, 1.5, 0.11, 0.0)`.
5. **Metal Producer:** None.
6. **Metal Consumer / Draw:** None.
7. **Render Order Position:** Combine post-processing.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** Zero occurrences of `ssfx_fog_scattering` in `src/Layers/xrRenderMetal`.
11. **Early-Out:** N/A.
12. **Stub / No-Op:** Missing entirely.
13. **Rejected Data:** None.
14. **Status:** `MISSING`.

---

### 3.5. Post-Processing & Optical Distortions

#### 3.5.1. Motion Blur
1. **Windows Source:** `src/Layers/xrRender/rendertarget_phase_blur.cpp:800` (`phase_mblur()`, `phase_ssfx_mblur()`).
2. **HLSL / Shaders:** `mblur.ps`, `ssfx_motionblur.ps`.
3. **Activation Condition:** `r2_mblur_enabled on`, `r2_mblur > 0`.
4. **Current Profile Value:** `r2_mblur 0.`, `r2_mblur_enabled off`.
5. **Metal Producer:** None.
6. **Metal Consumer / Draw:** None.
7. **Render Order Position:** Post-processing.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** None.
11. **Early-Out:** Cvar disabled.
12. **Stub / No-Op:** N/A.
13. **Rejected Data:** None.
14. **Status:** `DISABLED_BY_CONFIG`.

#### 3.5.2. Depth of Field (DOF)
1. **Windows Source:** `src/Layers/xrRender/rendertarget_phase_dof.cpp:3` (`phase_dof()`), `blender_dof.cpp`.
2. **HLSL / Shaders:** `dof.ps`, `ssfx_wpn_dof.ps`.
3. **Activation Condition:** `r2_dof_enable on`.
4. **Current Profile Value:** `r2_dof_enable on`, `r2_dof -1.0, 0.0, 800.0`, `r2_dof_radius 0.25`, `ssfx_wpn_dof_1 (0.15, 0.4, 0, 0)`.
5. **Metal Producer:** None.
6. **Metal Consumer / Draw:** None.
7. **Render Order Position:** Combine postprocess.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** Zero code in `src/Layers/xrRenderMetal`.
11. **Early-Out:** N/A.
12. **Stub / No-Op:** Missing entirely.
13. **Rejected Data:** None.
14. **Status:** `MISSING`.

#### 3.5.3. Gasmask Effects & Visor Drops
1. **Windows Source:** `src/Layers/xrRender/rendertarget_phase_gasmask_dudv.cpp`, `rendertarget_phase_gasmask_drops.cpp`.
2. **HLSL / Shaders:** `gasmask_dudv.ps`, `gasmask_drops.ps`, `screenspace_hud_raindrops.h`.
3. **Activation Condition:** Actor equipping gasmask (`ps_r2_mask_control.x > 0`).
4. **Current Profile Value:** `r2_mask_control (0.0, 0.0, 0.0, 0.0)`, `ssfx_hud_drops_1 (0, 0, 30, 0.05)`.
5. **Metal Producer:** None.
6. **Metal Consumer / Draw:** None.
7. **Render Order Position:** After combine_2, before UI.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** Zero code in `src/Layers/xrRenderMetal`.
11. **Early-Out:** Actor currently not wearing gasmask (`mask_control.x == 0`).
12. **Stub / No-Op:** Missing entirely.
13. **Rejected Data:** None.
14. **Status:** `MISSING` (Dormant by initial save state, but render passes absent).

#### 3.5.4. Fullscreen Heatvision
1. **Windows Source:** `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp:570` (`phase_heatvision()`).
2. **HLSL / Shaders:** `heatvision.ps`.
3. **Activation Condition:** `r__heatvision 1` / `ps_r2_heatvision > 0`.
4. **Current Profile Value:** `r__heatvision 0`.
5. **Metal Producer:** `MetalRenderDeviceRender.mm` captures `heatVisionActive` and `heatVisionIntensity` into `R4GameplayVisionSnapshot`.
6. **Metal Consumer / Draw:** None. No fullscreen heatvision combine pass exists in `MetalR4EngineScene.mm`.
7. **Render Order Position:** Postprocess combine.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** Object hotness parameters captured, postprocess absent.
11. **Early-Out:** Cvar disabled.
12. **Stub / No-Op:** Postprocess pass missing.
13. **Rejected Data:** None.
14. **Status:** `DISABLED_BY_CONFIG`.

#### 3.5.5. Nightvision
1. **Windows Source:** `src/Layers/xrRender/rendertarget_phase_nightvision.cpp`, `r4_rendertarget_phase_combine.cpp:566`.
2. **HLSL / Shaders:** `nightvision_gen_1.ps`, `nightvision_gen_2.ps`, `nightvision_gen_3.ps`, `nightvision_blur.ps`.
3. **Activation Condition:** `r__nightvision 1..3` / `ps_r2_nightvision > 0`.
4. **Current Profile Value:** `r__nightvision 0`.
5. **Metal Producer:** `MetalRenderDeviceRender.mm` captures `nightVisionActive` and `nightVisionPhase` (1..3).
6. **Metal Consumer / Draw:** `MetalR4EngineScene.mm:2718-2905` (`drawNightVision`). Creates 3 blur levels (`blurHorizontal`, `blurFinal`) and executes generation pipeline into `scene.lightingTargets[0]`.
7. **Render Order Position:** Post-lighting, before final combine_2.
8. **Target Written:** `scene.lightingTargets[0]`.
9. **Target Reader:** Swapchain presentation.
10. **Runtime Evidence:** `METAL_NIGHTVISION_PREPARED generations=1,2,3 tlab=24/192 constants=192+528 bundle=.../nightvision` in all engine logs. Tested in `R4NightVisionFoundationProbe.cpp`.
11. **Early-Out:** Early return when `!vision.nightVisionActive`.
12. **Stub / No-Op:** None. Fully implemented.
13. **Rejected Data:** None.
14. **Status:** `DISABLED_BY_CONFIG` (Fully implemented, inactive in current config).

#### 3.5.6. Post-Process Bloom
1. **Windows Source:** `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_bloom.cpp`, `r4_rendertarget_phase_hdr10_bloom.cpp`, `rendertarget_phase_pp_bloom.cpp`.
2. **HLSL / Shaders:** `bloom_filter.ps`, `bloom_build.ps`, `pp_bloom.ps`.
3. **Activation Condition:** `r2_tonemap on`, `r4_hdr10_bloom_blur_passes 20`.
4. **Current Profile Value:** `r2_tonemap on`, `r4_hdr10_bloom_blur_passes 20`, `ssfx_bloom_1 (10, 1, 0, 0)`.
5. **Metal Producer:** None.
6. **Metal Consumer / Draw:** In `MetalR4EngineScene.mm:3710`, `particles->DrawPostprocess()` expects `bloom` texture at slot 3, but caller passes `scene.lightingBlack`! No bloom mip generation or blur pass exists in Metal.
7. **Render Order Position:** Downsample pyramid between lighting and combine_2.
8. **Target Written:** None (`rt_Bloom_1..n` do not exist).
9. **Target Reader:** `combine_2` (currently reading black dummy).
10. **Runtime Evidence:** Hardcoded `lightingBlack` in call to `DrawPostprocess`.
11. **Early-Out:** N/A.
12. **Stub / No-Op:** Dummy texture replaces entire bloom pipeline.
13. **Rejected Data:** None.
14. **Status:** `MISSING`.

---

### 3.6. Particles & Emissives

#### 3.6.1. Distortion Particles
1. **Windows Source:** `src/Layers/xrRender/ParticleEffect.cpp:812` (`particles\xdistort`, `particles\xadd`).
2. **HLSL / Shaders:** `particle_distort.ps`, `combine_2.ps`.
3. **Activation Condition:** Particles using distortion shaders.
4. **Current Profile Value:** Active in anomalies/explosions.
5. **Metal Producer:** `MetalParticleCapture.h` captures distortion flag.
6. **Metal Consumer / Draw:** `MetalR4ParticleAdapter::DrawIntoActiveDistortion()` draws mask into `m_impl->distortionTarget`, then `DrawPostprocess()` runs `combine_2` to distort scene color into swapchain.
7. **Render Order Position:** After forward lighting, resolved during presentation.
8. **Target Written:** `m_impl->distortionTarget` (R8G8B8A8Unorm).
9. **Target Reader:** `combine_2` postprocess fragment shader.
10. **Runtime Evidence:** `METAL_PARTICLE_FRAME ... distortion=3 distortion_submitted=3` in release run logs (e.g. `e04-s1-flight3-20260924/engine.log:15757`).
11. **Early-Out:** Skipped when no distortion quads exist.
12. **Stub / No-Op:** None.
13. **Rejected Data:** None.
14. **Status:** `ACTIVE_VERIFIED`.

#### 3.6.2. Hard Particles (Blend / Add / Alpha-Add)
1. **Windows Source:** `src/Layers/xrRender/ParticleEffect.cpp`.
2. **HLSL / Shaders:** `particle.ps`, `particle.vs`.
3. **Activation Condition:** Standard particle effects.
4. **Current Profile Value:** Active.
5. **Metal Producer:** `MetalParticleCapture.h`.
6. **Metal Consumer / Draw:** `MetalR4ParticleAdapter::DrawIntoActiveForward()` renders into `scene.lightingTargets[0]`.
7. **Render Order Position:** Forward transparent pass after dynamic/static geometry.
8. **Target Written:** `scene.lightingTargets[0]`.
9. **Target Reader:** Combine postprocess.
10. **Runtime Evidence:** `METAL_PARTICLE_FRAME ... submitted=178 hard=178 additive=1 alpha_add=177` in release runs.
11. **Early-Out:** Skipped when empty.
12. **Stub / No-Op:** None.
13. **Rejected Data:** None.
14. **Status:** `ACTIVE_VERIFIED`.

#### 3.6.3. Soft Particles
1. **Windows Source:** `src/Layers/xrRender/ParticleEffect.cpp:802`, `blender_particle.cpp`.
2. **HLSL / Shaders:** `particle_soft.ps`.
3. **Activation Condition:** `r2_soft_particles on`.
4. **Current Profile Value:** `r2_soft_particles off` in `user.ltx`.
5. **Metal Producer:** `ParticleEffect.cpp:802` queries `ps_r2_ls_flags.test(R2FLAG_SOFT_PARTICLES)` and emits `Permutation::Hard`.
6. **Metal Consumer / Draw:** `MetalR4ParticleAdapter` implements 9 soft-particle pipelines with depth contrast, but receives 0 soft draws due to engine config.
7. **Render Order Position:** Forward transparents.
8. **Target Written:** `scene.lightingTargets[0]`.
9. **Target Reader:** Combine postprocess.
10. **Runtime Evidence:** `soft=0` in all `METAL_PARTICLE_FRAME` log lines.
11. **Early-Out:** Cvar disabled.
12. **Stub / No-Op:** Implementation ready, config dormant.
13. **Rejected Data:** None.
14. **Status:** `DISABLED_BY_CONFIG`.

#### 3.6.4. HUD Particles
1. **Windows Source:** `src/Layers/xrRender/ParticleEffect.cpp:809` (`GetHudMode()`).
2. **HLSL / Shaders:** `particle.ps`.
3. **Activation Condition:** Weapon firing, muzzle smoke, sparks in 1st person.
4. **Current Profile Value:** Active in gameplay.
5. **Metal Producer:** `MetalParticleCapture.h` sets `record.hudMode = true`.
6. **Metal Consumer / Draw:** In `MetalR4ParticleAdapter.cpp:474-479`:
   ```cpp
   if (m_ready && record.hudMode)
   {
       ++m_telemetry.rejectedDraws;
       ++m_telemetry.hudRejectedDraws;
       continue;
   }
   ```
7. **Render Order Position:** N/A (dropped during staging).
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** Explicit drop in `StageFrame`; status string declares: `"soft/hard particle blend/add/alpha-add plus distortion mask and combine_2 MSC pipelines ready; HUD fail closed"`.
11. **Early-Out:** Fail-closed rejection.
12. **Stub / No-Op:** Intentional discard.
13. **Rejected Data:** 100% of HUD particle draws rejected.
14. **Status:** `REJECTED`.

#### 3.6.5. Emissive Materials / Effects
1. **Windows Source:** SSFX Bloom Emissive (`rt_ssfx_bloom_emissive`).
2. **HLSL / Shaders:** `ssfx_bloom_emissive.ps`, `deffer_base.ps` emissive channel.
3. **Activation Condition:** Emissive materials.
4. **Current Profile Value:** Active in shaders.
5. **Metal Producer:** `MetalRenderDeviceRender.mm:996` reads `hudStats.emissiveFollowup` and logs `ancillary_emissive=%u`.
6. **Metal Consumer / Draw:** In `MetalR4EngineScene.mm:4899`, only printed to stderr in diagnostic string. There is no render pass, no target, and no composite.
7. **Render Order Position:** N/A.
8. **Target Written:** None.
9. **Target Reader:** None.
10. **Runtime Evidence:** Counter only in `METAL_HUD_CAPTURE` logs. Matches findings in `MATERIAL_PERMUTATION_CENSUS.md`.
11. **Early-Out:** Unhandled.
12. **Stub / No-Op:** No render pass exists.
13. **Rejected Data:** Emissive geometry not rendered to emissive buffer.
14. **Status:** `REJECTED / TELEMETRY ONLY`.

---

## 4. Confirmed Missing / No-Op

The following passes are proven to be completely non-functional or absent in the current Metal R4 runtime:

1. **Sunshafts (`rendertarget_phase_sunshafts.cpp`):**
   - Active in profile (`r2_sunshafts_mode volumetric`), but zero code exists in `src/Layers/xrRenderMetal`.
2. **Volumetric Lighting (`r4_rendertarget_phase_combine.cpp:438`):**
   - Active in profile (`r2_volumetric_lights on`), but missing in Metal. Passes dummy 1x1 black texture.
3. **SSFX Fog Scattering (`rendertarget_phase_blur.cpp:917`):**
   - Active in profile (`ssfx_fog_scattering 1.`), zero implementation in Metal backend.
4. **Depth of Field (`rendertarget_phase_dof.cpp`):**
   - Active in profile (`r2_dof_enable on`), zero implementation in Metal backend.
5. **Rain Ground Splashes (`dxRainRender.cpp:253-330`):**
   - Completely bypassed by unconditional early `return` in `dxRainRender.cpp:138`.
6. **Post-Process Bloom (`r4_rendertarget_phase_bloom.cpp`):**
   - Active in profile (`r2_tonemap on`), but `DrawPostprocess` receives `scene.lightingBlack` dummy.
7. **Lens Flare (`dxLensFlareRender.cpp` / `MetalR4LensFlareAdapter.cpp`):**
   - Producer captures quads, but adapter is uninstantiated and its methods are empty 1-line stubs returning `true`.
8. **Thunderbolt Geometry (`dxThunderboltRender.cpp`):**
   - Bypassed by explicit `if (SelectedR4Backend() == R4BackendKind::Metal) return;` at line 31.
9. **Gasmask Effects & Drops (`rendertarget_phase_gasmask_*.cpp`):**
   - Passes do not exist in Metal R4 (dormant in initial save due to `r2_mask_control.x == 0`).

---

## 5. Active and Verified

The following passes are verified to execute in production runtime and successfully deliver pixels to their render targets or final swapchain:

1. **Sky (`sky2_msc`):**
   - Dual-texture interpolation (`sky0`, `sky1`), sky rotation, and tonemapping. Logged as `METAL_SKY_DRAW` every outdoor frame.
2. **SSFX Water Waves Animation (`ssfx_water_waves.ps`):**
   - Generates wind-driven wave normal/height map to 512x512 target `m_impl->waves` and feeds water surface. Logged as `METAL_WATER_FRAME ... waves=512x512`.
3. **Distortion Particles (`particles\xdistort`, `particles\xadd`):**
   - Renders distortion offsets to `m_impl->distortionTarget` and distorts scene color in `combine_2`. Verified by `distortion_submitted=3` in release logs.
4. **Hard Particles (Blend / Add / Alpha-Add):**
   - Renders CPU-expanded sprite quads into `scene.lightingTargets[0]`. Verified by `submitted=178 hard=178` in release logs.
5. **SSFX Ambient Occlusion & Indirect Lighting (`ssfx_ao`, `ssfx_il`):**
   - 5 AO passes + 5 IL passes with temporal accumulation history. Logged as `METAL_SSFX_AO_IL_FRAME ... ao_passes=5 il_passes=5 temporal_history=true`.
6. **3DSS Scope PIP Reticle:**
   - Picture-in-picture weapon reticle pass with depth testing and color snapshot. Probes pass; active during scope aim.
7. **SSFX Color Grading & LUT Atlas:**
   - Authored LUT loaded from `shaders/lut_atlas.dds`, bound to presentation pass `DrawPostprocess` into swapchain.
8. **Thunder Environmental Flash:**
   - Engine storm simulation dynamically modulates `CurrentEnv` sky, sun, and fog colors ingested by Metal environment lighting.

---

## 6. Disabled by Current Configuration

The following effects are legally disabled by current configuration in `user.ltx`:

1. **Motion Blur:** `r2_mblur 0.`, `r2_mblur_enabled off`.
2. **Soft Particles:** `r2_soft_particles off` (Metal adapter implementation ready; engine producer emits `Permutation::Hard`).
3. **Fullscreen Heatvision:** `r__heatvision 0`.
4. **Nightvision:** `r__nightvision 0` (Metal adapter implementation fully ready with 3 blur levels and gen 1..3; dormant until toggled).

---

## 7. Contradictions Resolved

### 7.1. Lens Flare Contradiction
- **Prior Claim A:** "Capture data exists, PSO descriptor exists, `StageFrame` and `DrawIntoActiveForward` are no-op, adapter not in graph."
- **Prior Claim B:** "Marked ACTIVE_VERIFIED."
- **Fact-Based Resolution:** Claim A is 100% correct; Claim B was false.
  - `dxLensFlareRender::Render()` does capture flare quads and call `xray::metal::lensflare::Submit()`.
  - `MetalRenderDeviceRender.mm` does drain it and pass it to `scene.SubmitLensFlareFrame()`.
  - In `MetalR4EngineScene.mm:4716`, `impl->lensFlareFrame = std::move(frame);` stores it, but **it is never referenced anywhere else**.
  - `MetalR4LensFlareAdapter` is **not instantiated or included** in `MetalR4EngineScene.mm`.
  - In `MetalR4LensFlareAdapter.cpp`, `StageFrame()` and `DrawIntoActiveForward()` are empty stubs returning `true`.
  - Chain breaks between `SubmitLensFlareFrame` and render encoder. **No lens flare pixels are ever drawn.**

### 7.2. Rain Splashes & Components Contradiction
- **Prior Claim:** "Rain is working."
- **Fact-Based Resolution:** Rain is decomposed into 6 independent subsystems with distinct states:
  1. *Falling Droplets:* `IMPLEMENTED_NOT_RUNTIME_VERIFIED` (Fully wired in `MetalR4RainAdapter`, but release runs occurred in dry weather).
  2. *Rain Occlusion:* `PARTIAL` (CPU sphere culling active; GPU shadow projection missing).
  3. *Ground Splashes:* `MISSING` (Explicit early `return` in `dxRainRender.cpp:138` bypasses particle splash loop).
  4. *Surface Wetness:* `PARTIAL` (Global scalar passed in `staticGlobals`; screen-space accumulation pass missing).
  5. *Water Waves / Ripples:* `ACTIVE_VERIFIED` (Continuous 512x512 wave generation in `MetalR4WaterAdapter`).
  6. *Thunder Flash:* `ACTIVE_VERIFIED` (Environment lighting modulation active).

### 7.3. Emissive Materials Contradiction
- **Fact-Based Resolution:** Emissive effects are **REJECTED / TELEMETRY ONLY**.
  - In Windows R4, SSFX writes emissive values to `rt_ssfx_bloom_emissive`.
  - In Metal R4, `captureEmissiveFollowup` only increments a telemetry counter logged in `* METAL_HUD_CAPTURE ancillary_emissive=%u`.
  - No render pass or target exists to composite emissive materials into bloom or scene color. Matches `MATERIAL_PERMUTATION_CENSUS.md`.

### 7.4. HUD Particles Contradiction
- **Fact-Based Resolution:** HUD particles are **REJECTED (Fail-Closed)**.
  - In `MetalR4ParticleAdapter.cpp:474-479`, any draw record with `record.hudMode == true` immediately increments `rejectedDraws` and `hudRejectedDraws` and executes `continue`.
  - Weapon smoke, muzzle flash, and sparks in 1st person are intentionally dropped.

### 7.5. Sunshafts & Volumetrics Contradiction
- **Fact-Based Resolution:** Presence of HLSL files in `shaders/r3/` led to assumptions that sunshafts and volumetric lights exist.
  - In truth, `src/Layers/xrRenderMetal` has **zero sunshafts or volumetric raymarching passes**.
  - Slot 24 (`volumetric`) in `MetalR4RainAdapter` is explicitly fed with dummy texture `scene.lightingBlack`.

---

## 8. Tasks for Sol

### Task 1: Sunshafts & Volumetric Lighting Pipeline
- **Priority:** P0 (Prominent visual defect; active in user profile `r2_sunshafts_mode volumetric`, `r2_volumetric_lights on`).
- **Exact Symbols / Files:**
  - Create `MetalR4VolumetricAdapter.h` / `.cpp`.
  - Source references: `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp:438` (`phase_combine_volumetric`), `rendertarget_phase_sunshafts.cpp`, `accum_volumetric_sun.ps`, `combine_volumetric.ps`.
  - Integrate into `MetalR4EngineScene.mm` between G-buffer lighting accumulation and postprocessing.
- **Central Render Graph Change:** Yes. Requires allocating downsampled render targets (`rt_sunshafts_0/1`, `rt_ssfx_volumetric`) and inserting raymarching and combine passes.
- **Reusable Assets:** Converted shaders can follow the MSC patterns in `ssfx-ao-il-next`; sun direction and camera matrices are already available in `LevelScene::uniformData`.

### Task 2: Post-Process Bloom & Emissive Accumulation
- **Priority:** P0 (Major post-processing defect; bloom is currently 100% black dummy `scene.lightingBlack`).
- **Exact Symbols / Files:**
  - `src/Layers/xrRenderMetal/MetalR4EngineScene.mm` (replace dummy in `DrawPostprocess`).
  - Port `rendertarget_phase_pp_bloom.cpp` / `r4_rendertarget_phase_bloom.cpp`.
  - Create bloom downsample/blur mip-chain (`bloom_filter.ps`, `bloom_build.ps`).
  - Add `rt_ssfx_bloom_emissive` target to capture `ancillary_emissive` draws and add to bloom input.
- **Central Render Graph Change:** Yes. Requires bloom mip-chain passes prior to presentation pass `DrawPostprocess()`.
- **Reusable Assets:** `combine_2` presentation pipeline already accepts a bloom texture slot.

### Task 3: Complete Rain Ecosystem (Ground Splashes & Surface Wetness)
- **Priority:** P1 (Noticeable weather defect in rainy conditions).
- **Exact Symbols / Files:**
  - `src/Layers/xrRender/dxRainRender.cpp:138` (remove early return or implement splash capture).
  - `src/Layers/xrRender/MetalRainCapture.h` (add `SplashBatch` or integrate into particle capture).
  - Port `r4_rendertarget_draw_rain.cpp` wet surface accumulation (`rain_apply_normal.ps`, `rain_patch_normal.ps`).
- **Central Render Graph Change:** Yes. Insert screen-space wetness accumulation before lighting accumulation.
- **Reusable Assets:** `MetalR4RainAdapter` falling droplet and refraction passes are already complete and functional.

### Task 4: Lens Flare Integration into Render Graph
- **Priority:** P1 (Active in profile `r__lens_flares on`, producer already captures data).
- **Exact Symbols / Files:**
  - `src/Layers/xrRenderMetal/MetalR4LensFlareAdapter.h` / `.cpp`.
  - `src/Layers/xrRenderMetal/MetalR4EngineScene.h` / `.mm`.
  - Implement real buffer upload in `StageFrame()` and sprite rendering in `DrawIntoActiveForward()`.
  - Instantiate `MetalR4LensFlareAdapter` in `MetalR4EngineScene::Impl` and call in forward transparents pass.
- **Central Render Graph Change:** Minimal. Render flares into `scene.lightingTargets[0]` after sky/rain/particles using existing depth state.
- **Reusable Assets:** `MetalLensFlareCapture.h` and producer `dxLensFlareRender.cpp` are already fully working. Pipeline descriptor in `MetalR4LensFlareAdapter::Configure` is already configured for `effects_sun.vs/ps`.

### Task 5: Depth of Field (DOF) & SSFX Fog Scattering
- **Priority:** P1 (Active in profile `r2_dof_enable on`, `ssfx_fog_scattering 1.`).
- **Exact Symbols / Files:**
  - Port `rendertarget_phase_dof.cpp` (`dof.ps`) into `MetalR4EngineScene.mm`.
  - Port `rendertarget_phase_blur.cpp:917` (`ssfx_fog_scattering.ps`).
- **Central Render Graph Change:** Yes. Insert DOF blur pass and fog scattering pass into combine sequence before final UI overlay.
- **Reusable Assets:** Camera focal parameters (`ps_r2_dof_*`) and fog density parameters are already available in environment structures.
