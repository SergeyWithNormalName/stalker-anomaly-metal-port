# Local static shadow adjacent PSO calls

2026-09-29. Scratch drafts and read-only source review. No production application/build/game/GPU action by Sol. Root owns the next observation and any runtime gate.

The narrow defect is in [MetalR4EngineScene.mm:3350](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4EngineScene.mm:3350): `drawStaticTile` checks adjacent vertex-buffer changes but unconditionally binds either `scene.sunShadowPipeline` or `scene.sunShadowArefPipeline` before every static entry/merged run. Windows R4's sorted graph also checks cached state/VS/PS before state calls ([r__dsgraph_render.cpp:98](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/r__dsgraph_render.cpp:98)). A local exact-object PSO check removes an accidental native API repetition while preserving the existing draw order.

The independent [runtime patch](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/static-shadow-adjacent-pso/local-static-pso-reuse.patch) is strict default-off `XRAY_METAL_LOCAL_STATIC_PSO_REUSE=1`. Its shared_ptr cache starts null inside each `drawStaticTile` invocation and updates only after a successful actual bind. The disabled path does not assign this pointer. It preserves all VS/alpha PS MSC bindings, VB6 checks, indexed arguments, merge choice, work/transform preparation, resource admission, pass/viewport/scissor calls and draw order. It changes only consecutive bindings of the same exact pipeline object.

| Frozen patch | SHA256 |
|---|---|
| `local-static-pso-reuse.patch` | `cc2ddb594786947bf490cca04b31400c0c17e8bddf290af95d4d6993aae68ad6` |
| `local-static-pso-census.patch` | `303803d3a208d970daedf8474afeb664d11d976447d85e567a6f52549b3c8b11` |

Both patches touch only EngineScene and independently pass `git apply --check`. They are based on original source SHA `c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53`, copied from the recorded Flora prestate, rather than its temporary observer application. [source-manifest.json](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/static-shadow-adjacent-pso/source-manifest.json) pins exact original/draft content. Apply only the observer for initial count admission, restore that source, then consider the runtime patch.

## Why cached state survives this loop

[BindGraphicsPipeline:2788](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4Backend.mm:2788) sets native PSO, depth-stencil state, cull, winding and `mscPipelineBound`. The pipeline object and its descriptor-derived state are immutable. The exact shared_ptr comparison conservatively repeats a bind for every distinct object, even if two objects might have equivalent native descriptors.

Between consecutive static-loop binds, the only backend operations are:

| Operation | State written |
|---|---|
| [BindMscResources:2880](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4Backend.mm:2880) | Validation/residency/ownership, immutable TLAB allocation, vertex or fragment root buffer at slot2. No PSO/depth/cull/winding or `mscPipelineBound` write. The actual VS CB is already an acquired-frame upload slice; alpha PS only binds texture/sampler. TLAB block growth allocates a buffer; it does not reopen the render encoder. |
| [BindVertexBuffer:2816](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4Backend.mm:2816) | Vertex buffer6 and retention only. |
| [DrawIndexed:3085](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4Backend.mm:3085) | Indexed draw plus index-buffer/version retention and counters. MSC [IRRuntimeDrawIndexedPrimitives:944](<XRAY_SOURCE_ROOT>/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/include/metal_irconverter_runtime/metal_irconverter_runtime.h:944) updates auxiliary vertex slots4/5 then draws; it does not replace the PSO. |

No other adapter, pipeline bind, encoder begin/end, viewport/scissor change, stencil-reference change, shader selection or compute operation occurs inside the loop. Dynamic, defVertex, defObjectsLod, Flora and grass run after it; next tile starts with a new null cache. Isolated merge-oracle tiles/calls also start null. Error/abort exits destroy the cache; a subsequent attempt binds its first PSO normally. There is no cache across tiles, atlases, passes, oracle modes, acquisitions, retries or frame-slot wrap.

The omitted backend resource retention is safe because the first successful bind retains that same PSO for the acquired frame; Scene and the local cache also own it. All resource/constant/table binds still validate, declare and retain their data. No CB/TLAB address is reused through this candidate. Draw/alpha/depth/blend/stencil ordering is identical to baseline, so it does not require the Flora sorting contract.

Update risk: any future call inserted inside this loop that reopens the encoder or changes PSO/depth/cull/winding/`mscPipelineBound` must invalidate this local cache or repeat the bind. Mutating an existing pipeline object's native state would also invalidate the proof. Keeping the cache local to the lambda is essential.

## One count-only frame

The independent [observer patch](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/static-shadow-adjacent-pso/local-static-pso-census.patch) uses strict `XRAY_METAL_LOCAL_STATIC_PSO_CENSUS=1` and exactly engine frame360. It runs only after successful `account=true` production tile draws. It scans the same immutable entries or actual merge runs afterward, using each run's firstRow and the exact current alpha-to-pipeline decision. There are no backend wrappers, clocks, extra GPU commands or counters inside the real draw loop. Each row includes frame/batch/tile/light/face, actual merge-path use, draws, old PSO binds and the adjacent-cache bind count. Empty tiles are reported as zero/zero. `account=false` isolated oracle calls are excluded.

Root can use its existing matched harness with that single environment addition, then:

```sh
python3 <XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/static-shadow-adjacent-pso/summarize_census.py \
  "$XRAY_CENSUS_OUTPUT/engine.log" \
  --e01 "$XRAY_CENSUS_OUTPUT/e01-frames.csv" \
  --device-phases "$XRAY_CENSUS_OUTPUT/phases.csv.device.csv" \
  --output "$XRAY_CENSUS_OUTPUT/local-static-pso-census-summary.json"
```

`XRAY_CENSUS_OUTPUT` is that run's native output directory. The parser rejects duplicated tile/face observations, inconsistent counts, absent/multiple completed E01 acquisitions and an unmeasured device phase. Require the expected completed local-shadow faces and no discard/error/retry at frame360; observer rows do not carry a separate acquire epoch. This diagnostic frame has extra scan/logging cost and is not an FPS sample. Compare old-minus-cached calls with same-frame total E01 PSO requests; do not convert them to time.

The fresh CPU sample has 60/2,264 inclusive main samples in `BindGraphicsPipeline`, with 52 outside Flora. This remains a narrow cost center. Indexed draws, MSC calls, uploaded bytes, culling, merge planning and prewarm remain unchanged; local-shadow 14.076 ms is not this candidate's saving. Lower PSO counts justify only a bounded correctness/performance gate.

## Minimal exact gate if count economics pass

The existing static merge pixel-oracle block [EngineScene:3427](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalR4EngineScene.mm:3427) already renders source static materials through `drawStaticTile` into isolated real R32 atlases. For a separate validation draft, make a saved/restored off/on cache pair while holding **the same production merge mode**, tile list, transforms, mesh/index data, textures, samplers and PSO objects fixed. Each invocation still resets its local cache. Keep the actual qualified MSC shaders and production R32Float/Depth32Float PSOs. No production BindMscResources hook is needed.

Require exact raw R32 and shadow-depth pairs, positive coverage from both opaque and alpha-tested casters, an alpha-discard witness, and an affected sequence with at least two adjacent same-PSO draws. Include opaque→opaque→alpha→alpha→opaque, differing CB offsets, changed vertex buffers and merged/fallback paths. The existing oracle exports R32 only; a bounded validation-only shadow-depth readback with depth Store is still needed for a depth claim. Check expected PSO call counts while every MSC/VB/DrawIndexed call and argument sequence is identical. Exercise a second tile after a different caster-family PSO, plus abort/retry and frame-slot wrap, to prove the first bind always repeats.

No GPU oracle or runtime acceptance is claimed by these drafts. After the exact gate, one same-binary fixed-condition off/on screen with both census flags off should show the predicted PSO-request change and comparable draws/updates. Require valid callback/displayed timelines before an FPS claim; reject a neutral result. Current-frame command-buffer elapsed is neither active GPU time nor this cache's CPU cost.
