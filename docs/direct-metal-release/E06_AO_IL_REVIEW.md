# E06 6D — SSFX AO/IL contract review (read-only)

Status: source/current-run audit with a narrowly scoped adapter correction. No cook, build, game, or capture was performed. Adapter prestate copies are in `scratch/release_runs/20260927-e06-6d-ao-il-prestate/`.

## Windows contract and active fixture

In `r4_rendertarget_phase_combine.cpp:116-125`, Windows runs AO when `o.ssfx_ao && ps_ssfx_ao.y > 0`, then IL when `o.ssfx_il && ps_ssfx_il.y > 0`, before `combine_1`. Shader-presence flags come from `r4.cpp:505-506`. `phase_ssfx_ao/il` are in `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_ssao.cpp`; their six shader elements and source bindings are declared in `blender_blur.cpp:252-349`.

S3 capture settings are AO `(1,5,1,1.9)`, AO quality `2`, AO setup `(150,1,0.6,0.21)` and IL `(6.666667,0.8,1,5)`, IL quality `16`, IL setup `(150,1,1,0)` (`scratch/release_runs/20260927-e06-S3-capture/telemetry.json:167-172`). The cooked bundle receipt confirms `SSFX_AO_QUALITY=2` and `SSFX_IL_QUALITY=16` (`build/metal-runtime-shaders/ssfx-ao-il-next/ssfx-ao-msc-receipt.txt:12`); R4 clamps/sets these shader defines in `r4.cpp:1839-1851`.

Windows allocates full-resolution A8R8G8B8 histories/temporaries for both paths (`r4_rendertarget.cpp:603-604`). AO is five passes: raw trace at clamped `W/aoScale × H/aoScale`, UV extent `1/aoScale`, copy raw history, then four full-viewport blur passes (`phase_ssao.cpp:177-213,218-303`). IL is also five passes: raw trace at clamped `W/ilScale × H/ilScale`, UV extent `1/ilScale`, copy raw history, followed by four blur passes. Crucially, IL does not restore its viewport or UV extent before blurring: the intended reset statements are commented at `phase_ssao.cpp:356-360`; the active blur draws keep the reduced viewport and low-resolution UVs until the reset at `:445`, with blur levels 0.25/0.5/0.75/1.0 at `:362-442`.

The producer outputs are real combine inputs, not telemetry-only: `blender_combine.cpp:48-49` binds AO/IL, `combine_1.ps:232-249` samples IL at `I.tc0 / ssfx_il_setup.x` and AO at full UV. The Metal scene stores adapter outputs before base lighting (`MetalR4EngineScene.mm:3392-3442`); base-lighting TLAB binds AO at byte offset 120 and IL at 144 with white/black neutral fallbacks (`MetalBootstrap.mm:3480-3498`).

## Metal adapter findings

The adapter carries the correct active quality values in its receipted cook and reports five AO plus five IL passes at 1920×1080 during S3 (`engine.log:14760`); its TLAB sizes match cooked reflection (VS 24, AO 240, AO blur 120, IL 264, IL blur 72; `MetalR4EngineScene.mm:4418-4421`). AO/IL history is cleared on first target setup, camera cut, or invalid previous view; raw outputs are copied into histories before blur; previous position is copied after both chains; pending state is committed only through `CommitSubmittedFrame()` (`MetalR4SsfxAoAdapter.cpp:296-307,347-365,468-489,565-608`; submit hook `MetalR4EngineScene.mm:5382-5388`). This preserves the intended temporal ownership structure.

**IL scale/UV/viewport correction applied:** Windows IL scale is 6.666667 in the recorded fixture. Windows leaves IL blur on the reduced viewport and retains UV extent `1/scale` (`phase_ssao.cpp:319-342,356-360,362-445`; `ssfx_il_blur.ps:22,28-48`). `MetalR4SsfxAoAdapter.cpp` now sends all four IL blur passes the same reduced viewport (`ilScale`), UV extent (`1/ilScale`) and `blur_setup.x=1` as Windows. Targets remain full `W×H`; the raster footprint is approximately `W/scale×H/scale`, then `combine_1` samples at UV divided by that scale. At observed 1920×1080 and scale 6.666667, the intended viewport footprint is 288×162 pixels. Viewport width/height now use the same float division as the Windows caller and are not floored before Metal receives them. This is source-level contract correction only; the parent owns build/runtime validation.

**AO scale contract correction applied, rechecked during circle regression:** Windows uses `blur_setup.x=ps_ssfx_ao.x` only for the first full-size blur; the next three use `1` (`phase_ssao.cpp:236-237,257-258,278-279,298-299`). The initial review incorrectly described all four as using the scale. Current Metal supplies scale/1/1/1. The recorded AO scale is 1, so this follow-up does not change that fixture's coordinates. Matrix reprojection, history resets/copies and submit/commit ownership are unchanged.

No focused AO/IL adapter probe exists in the Metal tests/CMake inventory, so no implementation-mirroring test was added. Static review should be followed by the owning root build and existing test/runtime gates.

## Historical SSR compiler-annotation failure — superseded by packet 5

At the time of this audit, the original `general-ssr-e06` cook was blocked: the log shows the VS and gloss prefix succeeded, then DXC exit 5 on `ssfx_ssr.ps` at the indexed `[unroll]` expression in `screenspace_reflections.h:57`. This is historical compiler-failure evidence. Packet 5 later integrated the quality-0 annotation wrapper with a validated receipt; see the follow-up below.


## Circle regression: named resource bindings and combine controls

A later user screenshot reports overlapping light circles on the street of Bar (`l05_bar`). The named resource tables of the active DXIL expose semantic binding errors that the MSC descriptor counts and empty resource names alone do not catch. AO raw had history/motion and nofilter/linear swapped; AO blur had position/AO image and nofilter/linear swapped; IL raw had its first four textures in the wrong order. Current mappings follow the actual compiler tables. Combine globals at offsets288/304/320/336 now carry the full frame-stamped AO/IL control vectors; the old hardcoded IL scale1 disagreed with the producer's6.666667 footprint. See [independent Luna review](../../scratch/release_runs/20260927-e06-circles/review-luna.md). No shader source or artifact was changed for these binding fixes. Static confirmation and the successful Bar validation run are separate from exact Street image acceptance. The user provisionally observed circles gone and requested stopping; [safe stop and remaining comparison](E06_CIRCLES_FIX.md).

## Packet 5 follow-up — independent controls and general SSR

This follow-up supersedes the earlier “proposal — not admitted” SSR status above.
The five-pass general SSR path is integrated separately from water SSR. It owns
scene targets and previous-position history, and promotes temporal state only
after accepted frame submission. The quality-0 DXC adjustment changes the loop
annotation only; source receipt and recooked bundle bytes match. `p5-final`
completed with exit 0 and telemetry status OK.

The final readback set includes per-pass gloss, trace, horizontal blur, vertical
blur, and combine captures. All reports pass finite-channel checks. Trace, both
blur outputs, and combine contain nonzero RGB signal; the gloss result carries
signal in alpha (roughly 292k–400k nonzero alpha pixels across captured frames).
This confirms the passes execute with data in the tested scene, not Windows
image parity or coverage of every active profile. The reduced-scale blur
viewport/UV correction follows the Windows contract; the final p5-s3 run exercised the scale-2 transition at 1920×1080.

AO and IL now have independent availability/enabled gates and neutral history
behavior. The neutral IL value is `(0,0,0,1)`. Packet 5's converted AO/IL GPU
probe passed 18 control states with 34 readbacks. Its production witness shows
the IL-off low/high outputs exactly match black neutral references; stale-white
positive controls differ substantially. These are branch and history checks,
not the exact Street image gate. Windows reference and human acceptance remain
open.

## Packet 5 acceptance follow-up — 2026-09-27

The frozen `p5-accept` run (revision `b0ddc94…`) completed with exit 0: 514/514 validation reports
passed across 659 texture reports, with zero nonfinite channels. This evidence
supersedes the earlier p5-final runtime for current acceptance; that run predates
the SSR history correction and had incorrect SSR validation labels.

The production IL-off witness exactly matches neutral references at frames 89
and 93. Stale-white positive controls remain sensitive: low/high mismatch counts
are 909,366 / 612,459 at frame 89 and 895,795 / 519,905 at frame 93. The
independent lifecycle scenario exercises skip at frame 31, reload at 36, discard
at 41, a second view at 46, resize at 51, and restore at 56. SSR reuses one
normal frame and restarts cold after those events and on resume.

For SSR, `p5-accept` provides shader validation and temporal lifecycle evidence; the
later same-save `p5-s3` run completed the native scale-transition witness. The
earlier p5-final per-stage captures are not treated as current acceptance. Exact
Street image comparison and visual acceptance remain open.


## Final same-save S3 follow-up — 2026-09-27

The completed `p5-s3` run used the same source save as the historical 38-report
validation case and exited 0 with scenario status OK. It reports 179/179
validation reports PASS across 220 textures, with zero nonfinite channels. This
confirms the AO/IL runtime run and SSR scale-transition scenario on this save; it
does not explain the historical reports or establish Windows image parity.
