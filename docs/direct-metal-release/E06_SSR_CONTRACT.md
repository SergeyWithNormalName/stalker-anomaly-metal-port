# E06 6D — general SSFX SSR audit (read-only)

Status at original audit: contract-only, with no general SSR Metal pass admitted. The dated packet 5 execution record at the end supersedes that implementation status; water SSR remains a separate path.

## Windows producer, targets, and order

The R4 option is enabled when `r3\\ssfx_ssr.ps` exists (`src/Layers/xrRenderPC_R4/r4.cpp:501`); `phase_combine` invokes it only when that option is enabled and this is not an SVP frame, then marks previous-position data required (`src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp:337-341`). The preceding combine copies the just-combined `rt_Generic_0` into `rt_Generic_temp` (`:331-335`). General SSR therefore consumes a scene-color snapshot and finishes before the distinct water SSR/final-water section (`:343-385`).

Both final water and the preceding general SSR are inside `phase_combine`; Windows reaches `phase_bloom()` after water and forward/rain (`r4_rendertarget_phase_combine.cpp:378-385,446-459`), and `phase_bloom()` calls `phase_luminance()` at `r4_rendertarget_phase_bloom.cpp:146-149`. Therefore Windows order is general SSR → water SSR/final water → forward families → bloom/luminance.

`CRenderTarget::phase_ssfx_ssr()` executes the active sequence:

1. E5 `ssfx_ssr_gloss` → `rt_ssfx_temp3` at full extent, depth attached (`src/Layers/xrRender/rendertarget_phase_blur.cpp:206-222`).
2. E0 `ssfx_ssr` → `rt_ssfx`; `ScaleFactor=clamp(ps_ssfx_ssr.x,1,2)`, reduced viewport only when >1, full-sized quad with UV extent `1/ScaleFactor` (`:196-200,225-255`).
3. Copy raw result to `rt_ssfx_ssr` history (`:257-258`).
4. E1 horizontal blur → `rt_ssfx_temp`, `blur_params=(1,0,W/scale,H/scale)` (`:263-281`). The reduced viewport and `1/scale` UV extent established for trace remain active.
5. E2 vertical blur → `rt_ssfx_temp2`, `blur_params=(0,1,W,H)` (`:284-302`), still under that same reduced viewport and UV extent; only the blur parameter dimensions switch to full `W,H`. Blur is unconditional in this function; the apparent condition is commented (`:260-262`).
6. Restore full viewport and E3 `ssfx_ssr_combine` into `rt_Generic_0` (or MSAA `rt_Generic_0_r`) (`:305-332`).

All five named R4 color targets (`rt_ssfx`, `temp`, `temp2`, `temp3`, history `rt_ssfx_ssr`) allocate at full `W×H` as `D3DFMT_A8R8G8B8` (`src/Layers/xrRenderPC_R4/r4_rendertarget.cpp:595-598,601`). Keep the raw target allocation full-size even for reduced viewport; do not silently reinterpret the authored coordinate/UV contract as a reduced-size target. `rt_ssfx_ssr` is a prior-frame temporal input copied from the current raw trace before blur.

## Active source passes and binding contract

The authoritative per-element resource wiring is `src/Layers/xrRenderPC_R4/blender_blur.cpp:87-170`:

| Element | Active shader | Textures | Samplers | Output |
|---|---|---|---|---|
| 5 | `ssfx_ssr_gloss.ps` | `s_position` (`r2_RT_P`), `s_diffuse` (`r2_RT_albedo`), `s_motion_vectors`, `env_s0`, `env_s1`, `sky_s0`, `sky_s1` | `smp_nofilter` | `rt_ssfx_temp3` |
| 0 | `ssfx_ssr.ps` | `s_position`, previous `ssr_image` (`rt_ssfx_ssr`), `s_rimage` (`$user$generic_temp`), `s_motion_vectors`, `s_prev_pos`, `s_gloss_data` (`temp3`), `blue_noise` (`fx\\blue_noise`) | `smp_nofilter`, `smp_rtlinear`, `smp_linear` | `rt_ssfx` |
| 1 | `ssfx_ssr_blur.ps` | `ssr_image` (`rt_ssfx`), `s_diffuse` | `smp_nofilter` | `rt_ssfx_temp` |
| 2 | `ssfx_ssr_blur.ps` | `ssr_image` (`rt_ssfx_temp`), `s_diffuse` | `smp_nofilter` | `rt_ssfx_temp2` |
| 3 | `ssfx_ssr_combine.ps` | `s_position`, `s_rimage` (`generic_temp`), `ssr_image` (`temp2`), `s_gloss_data` (`temp3`) | `smp_linear`, `smp_nofilter` | current `rt_Generic_0` |

E4 `ssfx_ssr_noblur.ps` is declared by the blender, but the active phase does not select it; the blur guard is commented out (`rendertarget_phase_blur.cpp:260-262`). It is not part of the current active cook set.

Per-pass constants explicitly set by C++ are: E0 `m_current`, `m_previous`, jitter `cam_pos`, `ssr_setup=ps_ssfx_ssr`; E1/E2 `blur_params` as above plus `ssr_setup`; E3 `ssr_setup` (`rendertarget_phase_blur.cpp:247-253,276-280,297-301,328-331`). `ssfx_ssr_2` and `ssfx_is_underground` are shader globals: standard binding registers `ssfx_ssr_2` and the underground flag (`src/Layers/xrRender/Blender_Recorder_StandartBinding.cpp:919-925,1023-1029,1476,1488`). Static/common screen values also referenced by these shaders include `screen_res`, `timers`, `pos_decompression_params`, `m_P`, `m_inv_V`, `fog_params`, and `rain_params` via `screenspace_common.h`/`screenspace_reflections.h`; the final MSC reflection must confirm which survive in each optimized pass.

The runtime capture records `ssfx_ssr=(1.0,0.25,0,0)`, `ssfx_ssr_2=(1.15,1.3,0.45,0.05)`, and `ssfx_ssr_quality=0` (`scratch/release_runs/20260927-e06-S3-capture/telemetry.json:180-182`). This means full-resolution trace for that observed fixture, blur-intensity input 0.25 (blur passes still execute), and quality permutation 0. R4 derives compile define `SSFX_SSR_QUALITY` by clamping this console value to [0,5] (`src/Layers/xrRenderPC_R4/r4.cpp:1818-1822`); quality 0 uses table entry `(8,72)`—8 trace steps, max hit distance 72 (`build/metal-shader-sources/r3/screenspace_reflections.h:22-30`; lookup in `ssfx_ssr.ps:45-58`). This capture telemetry demonstrates configured values, not a Windows visual match.

Cook candidates for the active route are `stub_screen_space.vs`, `ssfx_ssr_gloss.ps`, `ssfx_ssr.ps`, `ssfx_ssr_blur.ps`, and `ssfx_ssr_combine.ps`, from active `build/metal-shader-sources/r3`. Use the active shared R4 define receipt as the starting point (it includes `SSFX_SSR_QUALITY=0`; see `src/Layers/xrRenderMetal/shaders/terrain-l05-bar-msc-receipt.md:26`) and select the profile's `USE_MSAA` branch deliberately. Cook must preserve each shader's DXC reflection + MSC reflection and receipts, then validate every admitted resource/register and constant layout against this table and the HLSL, rather than reusing water/AO bindings by analogy.

### Exact cook attempt (2026-09-27)

`script/compile_r4_e06_effects_msc.py` was run by the integrator for output directory `build/metal-runtime-shaders/general-ssr-e06`; evidence is `scratch/release_runs/20260927-e06-start/cook-general-ssr.log`. `stub_screen_space.vs` and `ssfx_ssr_gloss.ps` compiled and emitted reflection/TLAB data; the script stopped when DXC compiling `ssfx_ssr.ps` exited 5. The reported diagnostic is at `screenspace_reflections.h:57`, on `[unroll (q_ssr_steps[SSFX_SSR_QUALITY].x)]` (`expected ')'` before the closing `]`). The included source tree was byte-identical to the winning profile for all five named shaders; no HLSL was rewritten. Consequently blur/combine were not reached, and this is not a usable full bundle or evidence of an admitted SSR pass. A follow-up should first resolve compiler compatibility while preserving source semantics and receipt provenance, then cook/reflection-check all five active pass artifacts. The raw log records the failing DXC command/exit status; the diagnostic text is recorded from the integrator's observed compiler output.

## Current Metal graph and proposed next leaf boundary

**Historical audit snapshot before packet 5:** there was no general SSR adapter, target, `ssfx_ssr` bundle option, or SSR draw in `MetalR4EngineScene` or the Metal adapter sources (excluding the explicitly separate water adapter). The current graph performs base `combine_1` into `scene.lightingTargets` (`MetalR4EngineScene.mm:3453-3465`), then sky/clouds and lens overlay, **luminance adaptation before** dedicated water (`:3470-3554,3578-3609`); later it runs bloom/combine presentation (`:3875-3905`). General SSR is absent, not currently bound to a neutral SSR dummy. `scene.lightingBlack` is reused for other explicit gaps (e.g. rain's unused input, bloom HUD mask, presentation fallback), but no SSR shader consumes it (`:3682-3705,3875-3901`). This also exposes an existing relative-order mismatch: Metal luminance adaptation precedes water, while Windows water precedes `phase_bloom`/luminance. The root has since moved Metal luminance after water and forward families, and bloom before night vision; the cited lines describe the historical audit snapshot, not current ordering. The SSR leaf can establish its own required order (after the scene-color snapshot, before water), but exact graph parity requires the integrator to reconcile the remaining graph dependencies; this audit does not change central ordering.

Original suggested leaf boundary (implemented by packet 5): add an independently admitted `MetalR4SsfxSsrAdapter` with only its own pipeline/target/frame-history ownership and a typed input contract for G-buffer position/albedo, motion vectors, previous-position texture, env/sky cube pairs, noise, current combined-color snapshot, immutable static globals, view reprojection matrices, and the captured SSR vectors. Allocate five full-resolution logical RGBA8 intermediates/history (native equivalent of the Windows `A8R8G8B8` contract); use immutable per-pass constant/vertex slices; reset history on invalid previous view/camera cut and commit history only after frame submission. Run the gloss/trace/history/blur/combine chain after the combined scene source is ready and before water's SSR/final draw, before the later Windows bloom/luminance phase; keep its contribution in the combined color so later presentation sees it. The leaf must not modify central scene ordering/graph; integrator owns that binding step. At that audit snapshot, general SSR remained **OPEN / not implemented / no visual parity claim**. The dated execution follow-up below records the later integration and runtime evidence.

## Packet 5 execution follow-up — 2026-09-27

The general SSR leaf is now integrated after the pre-SSR scene-color snapshot
and before water. It owns the five pass targets and previous-position history;
position history is cleared for reset/re-enable and promoted only after accepted
frame submission. The five active passes remain gloss, trace, history copy,
horizontal blur, vertical blur, and combine, with the history copy between trace
and the two blurs as in the Windows graph.

The compatibility wrapper substitutes the active quality-0 table value
`q_ssr_steps[0].x = 8` only in the DXC loop annotation. Original loop body and
shader calculations are retained. The cook receipt records the transform, and
the recook compared byte-identically with the receipted bundle. This admission
covers quality 0; it does not establish other quality variants or Windows image
parity.

`p5-final` completed with exit 0 and telemetry status OK. Per-pass native
readbacks show finite values and nonempty signal through trace, both blurs, and
combine; gloss carries signal in alpha. These readbacks establish pass execution
for the captured scene. The corrected reduced-scale blur viewport/UV contract
now matches Windows for both blur draws. The final p5-s3 witness exercises the
scale transition at 1920×1080; exact image and broader profile acceptance remain
open.

## Final packet 5 acceptance update — 2026-09-27

`p5-accept` completed with exit 0 and 514/514 validation reports PASS across 659
texture reports, with zero nonfinite channels. Its SSR acceptance includes
shader validation and temporal lifecycle evidence: one normal reuse frame,
then cold history after skip/reload/discard/second-view/resize/restore and on
resume. The old `p5-final` per-stage SSR readbacks predate the history fix and
used incorrect validation labels, so they are not current acceptance.

The source fix invalidates history when the SSR scale changes through `1→2→1`.
The final same-save `p5-s3` run completed at 1920×1080, exit 0, scenario OK,
with 179/179 reports PASS across 220 textures and zero nonfinite channels. At
scale2/frame90, the cold trace is 960×540 with 414 nonzero RGB pixels (max
0.6863), blur H has 3,205 and blur V 3,436 nonzero RGB pixels. At scale1/frame91,
the cold trace has 868 nonzero RGB pixels and both blur outputs are positive.
