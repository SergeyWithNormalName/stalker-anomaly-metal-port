# Flora shadow state order: census admitted, runtime draft provisional

2026-09-29. Source/evidence review and scratch drafts only. No production application, build, game launch or GPU execution by Sol. Root owns those actions. The prior local-light MSC reuse candidate remains rejected; its frozen artifacts are unchanged.

The concrete native difference is group order. Windows R4 sorts shadow graph packets by generated state keys and checks cached state/VS/PS/constants/textures before changing them ([r__dsgraph_render.cpp:78](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/r__dsgraph_render.cpp:78)). Native Flora creates identical-resource instancing groups in first asset occurrence order ([MetalR4FloraAdapter.cpp:847](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp:847)) and traverses selected groups in that order ([line 1248](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp:1248)). It changes PSO/VB only when they differ from the immediately preceding Flora group. Interleaved trunk/leaf groups can therefore issue avoidable PSO calls. This is a state-order candidate, not a new geometry grouping, caster-selection or recurrence cache.

Saved Cordon evidence has 487 prepared groups / 11,663 visuals. Root's measured off medians are 21,117 indexed draws, 15,036 PSO requests and 28,123,884 updated bytes/frame. Local shadow CPU 14.076 ms, sun 2.589 ms and dynamic shadow 2.752 ms are overlapping scopes. They do not predict this candidate's saving.

The fresh v9r1 CPU sample further limits the hypothesis: `BindGraphicsPipeline` has 60/2,264 inclusive main-thread samples (2.65%); only 8 (0.35%) have Flora `DrawShadowCasters` ancestry. The previous parser's zero was a symbol matcher miss. The exact symbol and 27 source entries are preserved in [pipeline-sample-evidence.json](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/flora-shadow-order/pipeline-sample-evidence.json). This is a perturbed CPU sample, not an FPS ceiling or forecast. The runtime draft removes no MSC bindings, copies, declarations, uploaded bytes or indexed draws. A neutral screen should reject it.

## Frozen artifacts

Both patches are independent diffs from the same source prestate. Both passed `git apply --check`. They should not be applied together as if one depended on the other.

| Draft | Scope | SHA256 |
|---|---|---|
| [flora-shadow-state-census.patch](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/flora-shadow-order/flora-shadow-state-census.patch) | Observer only; Flora cpp/h + two scene call-site provenance arguments | `f5f85252e3f0c66840794e332f3a035249fb0b5757000baa65da22252bc23883` |
| [flora-shadow-pso-order.patch](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/flora-shadow-order/flora-shadow-pso-order.patch) | Default-off reorder; Flora cpp only | `fd25377018a43ab1ad1ec65a6668a254bfc80e73bbee49336c17279dd262e4b0` |

[source-manifest.json](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/flora-shadow-order/source-manifest.json) pins original/draft source hashes. [reviewed-source-and-shader-hashes.json](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/flora-shadow-order/reviewed-source-and-shader-hashes.json) pins the inspected pipeline/Windows sources and qualified shadow shader inputs. The copied prestate/draft sources and generator stay in this scratch folder.

## One cheap observation

Root's chosen existing matched native harness receives only these environment additions:

```text
XRAY_METAL_FLORA_SHADOW_STATE_CENSUS=1
XRAY_METAL_FLORA_SHADOW_STATE_CENSUS_FRAME=360
```

Absent the exact-frame option, the observer selects its first production shadow frame divisible by 120 and then stops. It records actual admitted instanced groups after encoding; it uses their already-filled `group.models` and the exact pipeline/VB objects. No clock, backend wrapper, extra geometry query or draw is added. The provenance context contains engine frame, local light/face or sun cascade; standalone/validation calls carry family zero and are excluded. Duplicate exact context+LVP observations are omitted; 1,024 unique views is an explicit truncation cap. A `DONE` line appears on the next production shadow frame.

Each row gives total/selected groups, broad-phase candidates, submitted visual instances and current/hypothetical PSO/VB binds. The hypothetical partition uses first-seen exact pipeline keys from **all** immutable groups, matching the runtime draft even if the earliest group is culled. Counts include the first bind in each view. The summarizer also derives transitions by subtracting one from each nonempty view; saved bind differences equal saved transition differences.

```sh
python3 <XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/flora-shadow-order/summarize_census.py \
  "$XRAY_CENSUS_OUTPUT/engine.log" \
  --e01 "$XRAY_CENSUS_OUTPUT/e01-frames.csv" \
  --device-phases "$XRAY_CENSUS_OUTPUT/phases.csv.device.csv" \
  --engine-frame 360 \
  --output "$XRAY_CENSUS_OUTPUT/flora-shadow-state-summary.json"
```

`XRAY_CENSUS_OUTPUT` is the root-owned run's native output directory. The parser requires one finished untruncated census, contiguous unique view rows, consistent counts, exactly one completed E01 acquisition at the observed engine frame, and (when supplied) device phase 1. It reports local/sun sums, avoided PSO binds, extra VB binds and PSO-request fractions of that **same** E01 frame. It does not convert any count to ms or FPS. The small `parser-fixture/` only validates parser arithmetic and an empty-view case; it is synthetic and is not game evidence.

Acceptance for this observation requires the expected production local faces and all three active sun cascades, completed E01 and no error/discard/retry at the sampled frame. The observer does not add an acquire-epoch/commit-serial backend hook; ambiguous multiple attempts at the same engine frame must be rejected. Its logging/allocation overhead makes the observed frame unsuitable for a performance comparison. If the actual current order already has approximately two PSO binds per view, or PSO savings are small relative to the frame and fresh sample, reject without applying the reorder draft. A substantial VB increase also weakens admission. No universal time benefit follows from lower PSO request counts.

## Ordering contract and update risk

| Property | Evidence and implication |
|---|---|
| Color/depth | Production Flora shadow PSO is single-sample R32Float + Depth32Float, `Less`/depth-write, back cull/CW ([pipeline setup:569](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp:569)). Both qualified fragment permutations return post-raster `SV_Position.z`; the alpha version first calls the original alpha/discard function ([compiler wrapper:85](<XRAY_SOURCE_ROOT>/script/compile_r4_shadow_casters_msc.sh:85)). Successful fragments therefore contribute minimum depth; exact-depth ties have the same color depth value. |
| Blend/stencil | Both are disabled by descriptor defaults ([IR4Pipeline.h:98](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/IR4Pipeline.h:98)); production shadow attachment has no stencil aspect. G-buffer draws and their stencil behavior are outside the patch. |
| Alpha/wind | `shadow_direct_base_aref.ps` only samples alpha and clips before its result. No UAV/atomic/ordering side effect was found. The instanced VS retains original wind and per-instance modelY phase ([cook script:21](<XRAY_SOURCE_ROOT>/script/compile_r4_flora_instanced_shadow_msc.py:21)). Instance membership/order, instance ID to model association, material resources and wind inputs stay unchanged. |
| State boundaries | Every group retains its original MSC VS bind and required leaf PS bind. First PSO/VB are still bound in every view. The following detail-grass shadow draw binds its own PSO, VB6, VS and PS ([DetailGrassAdapter.cpp:1255](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4DetailGrassAdapter.cpp:1255)); otherwise the pass ends. Local tile scissors are disjoint; viewport/scissor and pass ordering are unchanged. |
| Ownership | Group index permutations are immutable after Prepare. Pipeline objects are adapter-owned shared objects. Group models are rebuilt per view and copied into fresh acquired-frame immutable upload slices before encoding. All per-group TLAB/upload/declaration/retention operations remain; no binding state or resource address survives into another view or acquisition. |

The default-off switch is strict `XRAY_METAL_FLORA_SHADOW_PSO_ORDER=1`. It prepares stable index permutations by exact selected pipeline object once, for normal alpha and the existing validation alpha-ignore variant. Only the final instanced group traversal changes; noninstanced, visible G-buffer and other shadow adapters retain their order.

This source proof is conditional on the pinned raster-depth bundle. The older zero/texture-red shadow fragment outputs would invalidate the equal-depth color argument. Reflection/name admission alone does not prove the fragment's meaning. Changes to fragment side effects, blend/stencil/depth rules, sample count, dynamic material parameters, per-instance shader semantics or a consumer that relies on prior stage bindings require a fresh review and GPU oracle. Reordering also changes upload addresses and the final bound leaf/trunk PSO; immutable slices and complete consumer bindings are essential, not optional.

## Minimum exact GPU oracle before a runtime screen

The existing `XRay.Metal.FloraInstancingGpuProbe --gpu <legacy-shadow-dir> <instanced-shadow-dir> <artifact-dir>` remains a shader/instancing qualification only. Its six cases each use one material and nonoverlapping instances; its diagnostic depth format is Depth32FloatStencil8. It does not exercise this production adapter reorder branch and cannot admit this change.

The shortest production oracle route is the existing scene-owned `runFloraShadowPixelOracle` plus `DrawShadowCastersForValidation` ([scene helper:3081](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4EngineScene.mm:3081)). A separate opt-in validation selector should save/restore order mode with RAII, render **instanced off-order versus instanced on-order** through the same prepared adapter/actual pipeline objects, and prepare the index partitions once before those isolated passes. Do not reuse its present legacy-versus-instanced comparison as ordering proof.

Use one actual local spot view, one OmniFace view and the three current sun cascades, choosing views with both exact PSOs and at least three current PSO binds so the order changes. Freeze matrices, model lists, textures, sampler and wind inputs for each pair; repeat with two wind states. Both modes use independent R32Float and Depth32Float attachments, clear depth/color to 1, store, then enqueue raw color **and shadow-depth** readbacks before either attachment is reused. Current helper exports only R32; a bounded validation-only shadow-depth readback is still needed. No production binding hook, clock or midframe wait is required.

Require byte-exact R32/depth pairs, equal submitted visual/draw/cull counts, finite nonzero coverage, and a real alpha-ignore control whose coverage increases. A small mixed trunk/leaf fixture through the same adapter branch should additionally include shared VB, occluding overlaps, exact-depth ties and a near-plane straddler; a homogeneous or blank pair is insufficient. Production Depth32Float has no stencil aspect; main G-buffer stencil is not attached to these passes. Capture it separately if claiming a whole-scene oracle. Frame-slot wrap and Discard/BeginFrame Retry should leave no stale permutation/mode/telemetry state; RAII must restore modes on every failure.

No new oracle implementation or GPU acceptance is claimed here. If the census passes economic triage, implement that bounded oracle separately, then use one same-binary fixed-condition off/on screen with every observer disabled. Current-frame command-buffer elapsed time is neither active GPU time nor this candidate's CPU cost. Require valid displayed/callback timing and comparable draw/update populations before calling any FPS advantage.

## Root's completed census receipt

Root admitted engine frame360 / E01 serial360, device phase1, with DONE=288 unique views. Local PSO requests were 1,848→448; sun 272→6; total 2,120→454. The hypothetical partition avoids 1,666 requests (11.3% of the same frame's 14,693 E01 PSO requests), while VB requests remain 3,432→3,432. These are observed structural counts, not a performance result. The runtime order draft remains unapplied. Root prioritized the simpler local static-shadow adjacent PSO candidate, whose draw order is unchanged; its separate drafts/report are in `../static-shadow-adjacent-pso/`.
