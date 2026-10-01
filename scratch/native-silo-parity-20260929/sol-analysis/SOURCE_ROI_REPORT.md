# One bounded MSC candidate for native/Silo continuation

Status: **source-qualified scratch draft; no compilation, GPU execution, game launch, or FPS acceptance performed by this worker.** All production sources remained byte-identical to the five prestate hashes at the last read-only check. Root owns application, builds, probes and runtime tests.

The first candidate removes **two VS stage binds and one duplicate mask PS stage bind per submitted local light**, strictly within that light's three immediate draws in one encoder. It preserves every pipeline, stencil reference, vertex-buffer bind, indexed draw and the final accumulation PS bind. The flag is default-off `XRAY_METAL_LOCAL_LIGHT_BIND_REUSE=1`. There is no bound-resource cache across lights, batches, encoders or acquisitions.

## Evidence and the actual scope of the expense

The saved V7 combined steady sample is `scratch/gameplay-rescue-20260929/runs/v7-profile-combined-01-watcher/sample-during-measure.txt`, candidate executable SHA `43c07e4d7a043d44f01d675554aa30a16d5bd09f76a9e99468317d5983fc2bdd`. It sampled live gameplay during measurement and is diagnostic-only. The main thread has 1,163 snapshots; `Scene::Render` appears in 897 and `renderProductionScene` in 351. The local-shadow subtree at sample line 32 has 143; its Flora shadow child has 95. The flat top-of-stack list reports `BindMscResources` 52, `_platform_memmove` 50, `R4StaticShadowIndex::Visit` 39 and `MTLResourceListAddResource` 35. These are stack observations with nesting; they are neither milliseconds nor additive budgets.

Current source hot paths:

| Source | Confirmed work and ownership |
|---|---|
| `MetalR4Backend.mm:2873` `BindMscResources` | Validates resources, snapshots every persistent Shared CB at each bind, declares indirect reads, retains resources, creates immutable TLAB range and binds stage root slot 2. |
| `MetalR4LocalLightAdapter.cpp:645` `bindDraw` | Front/back/final draws each rebind identical per-light VS resources. Front/back also rebind identical mask PS resources. VS payloads already use immutable acquired-frame slices. |
| `MetalR4FloraAdapter.cpp:1058` `DrawShadowCasters` | Already uploads 320-byte globals, per-view 528-byte static globals and optional model arrays as frame slices. Eliminating persistent Shared snapshots cannot remove these uploads. |
| `MetalR4EngineScene.mm:3226` `renderLocalShadowBatch` | Static BVH query, exact corner admission, transform-byte table preparation, existing static merged-index cache, then draws. This is separate from the selected accumulation candidate. |
| `MetalBootstrap.mm:4504` `drawSunShadowMap` | One mutable 256-byte Shared transform is updated and then snapshotted per static caster. This path has few snapshots in the bounded sample and remains outside this candidate. |

The same saved run's `output/engine.log:10401` records frame 1527 with 311 local lights submitted, including 303 shadowed. Source-level work counts for that receipt are:

| Local-light stage binds | Current | Candidate |
|---|---:|---:|
| VS | 933 | 311 |
| PS | 933 | 622 |
| Total | 1,866 | 933 |

All 933 omitted TLABs fit one 256-byte allocation quantum; the current allocator would reserve 238,848 fewer TLAB bytes for this work count. Removing the duplicate back-mask PS bind also removes 311 redundant 528-byte snapshots of `inputs.staticGlobals` (164,208 copied bytes). The final PS bind and its snapshot remain. These are deterministic work reductions for the stated receipt, not savings in measured CPU/GPU time or FPS.

## Why the removed calls do not change the shader inputs

`vsBindings` is built once per light from the current WVP/world-view/model payload and newly uploaded slices (`LocalLightAdapter.cpp:550–585`). Neither the slices nor those binding values change during the immediate triplet. `maskPsBindings` is also unchanged between the first two calls. The final PS layout can change to a shadowed permutation and is always rebound.

`BindGraphicsPipeline` at backend line 2788 sets PSO, depth state, cull mode, front winding and `mscPipelineBound`; it does not write a stage buffer slot. PS-only `BindMscResources` writes `setFragmentBuffer` at root slot 2 and does not touch the vertex root buffer. The pinned **actual** MSC 4.0.1 companion header is `build/direct-metal-toolchain/metal-shaderconverter-4.0.1/include/metal_irconverter_runtime/metal_irconverter_runtime.h`: constants at lines 732–739 specify root 2, draw arguments 4, uniforms 5, vertex fetch 6; its indexed helper at 944–967 only writes vertex slots 4/5 and issues the indexed draw. All three slot-6 binds remain in the patch.

Front/back PSOs use the same `accum_volume.vs` and the same unshadowed fragment metallib per `shaderKind` (`LocalLightAdapter.cpp:338–390`). Point and OmniFace share shaderKind 0 but retain their distinct geometry and volumeKind; Spot uses shaderKind 1. The first bind declares indirect reads and retains exactly the resources needed for subsequent identical stage use in the same encoder. The retained buffers, samplers and immutable TLAB allocation survive command completion. No further writes to these buffers occur in the triplet.

Marker reset runs only after the final accumulation. The next light always binds both stages again. `BeginRenderPass`, acquired-frame retry/discard, slot wrap and a new `DrawBatch` likewise cannot inherit a skip decision: only the configuration flag is stored on the adapter; the reuse decision is explicit in the three local call sites.

The separate volumetric route is unchanged. `EngineScene.mm:5179–5201` invokes `MetalR4VolumetricAdapter::DrawSpot` after the batch, and that adapter binds its own VS/PS (`VolumetricAdapter.cpp:566,658,719,753,808`). The candidate neither suppresses a light nor removes its shadow/volumetric work. Final receiver visibility-query routing is unchanged.

## Windows source comparison

The native Windows-style renderer in this repository already avoids redundant constant work: `dx10ConstantBuffer.cpp:84` uploads only while `m_bChanged`, and clears the flag afterwards; `dx10R_Backend_Runtime.h:505–538` checks CB identity ranges and returns early for an unchanged constant table. This supports reducing native Metal binding work while retaining immutable inputs. It is a source comparison, not a measured Windows CPU budget or proof that the installed Silo executable exactly matches every current source detail.

`xrRender/lights_render.cpp:190–199` additionally retains a stationary light's static graph after its moving-frame threshold; dynamic capture is separate. Native Metal already has caster BVHs and a merged-index cache, so describing it as lacking shadow caching would be wrong. A new preparation cache would need stable source/light generations and its own repetition evidence; it is not implemented or selected here. HOM suppression, parallel static packets, V8 static groups and V9 grid suppression remain excluded.

## Frozen patches and oracle

`local-light-bind-reuse.patch`: SHA-256 `28fb7a09c5669e0f489606b15da5d8ab8bfcf1d83f0e572267a7a279a4b2f308`.

`local-light-bind-reuse-oracle.patch`: SHA-256 `1144978d135776fd8e5cedb552b5f68b5bf290ae9979617dc2ce26697d1fb567`.

Both passed `git apply --check` against the current production source at preparation time. `source-manifest.json` records five exact prestate/draft source hashes; `draft/` and `prestate/` contain copies. The runtime diff changes only the adapter and its telemetry. The separate oracle diff extends the existing local-light rollover probe, adds a test-function friend for raw aspect readback, and keeps the ordinary backend binding hot path unchanged. No dropped-binding countdown or fault hook was added.

The oracle uses **qualified production MSC local-light PSOs through the real adapter**, with its existing geometry fixture and preservation witness. It creates an off adapter and an on adapter, compares eight pairs (two camera poses × bulk/chunk/prefix/isolated-tail), and requires byte-identical raw RGBA16F, depth32 and stencil8, identical admissions/resets, and exact bind counters (off 3VS/3PS; on 1VS/2PS per submitted light). It varies per-light position/range, hence WVP and dynamic light bytes. Inputs can select individual Point/Spot/OmniFace families and synthetic R32 shadow atlas inputs for the actual shadowed Spot/Omni PS permutations; shadow sampling uses the production LessEqual comparison policy.

Every shadowed fixture adds a seventeenth capture through the same on-path adapter, PSOs, light transforms, camera, receivers and shadow-record matrices as its lit capture. Only the sampled atlas changes: depth `.95` to depth `0.0`. The gate requires equal raw depth/stencil and preservation witnesses, equal admitted/reset counts, finite RGBA, and at least one geometry RGB pixel whose lit contribution exceeds the blocked contribution by `0.001`. This proves that the actual shadowed shader output responds to sampled depth; a constant-atlas positive image alone does not. The report exposes `shadow_signal_pixels`, `max_shadow_lit_minus_blocked_delta` and `shadow_depth_stencil_preserved`. This remains a draft until root executes it successfully.

Before compared submissions it actually encodes and discards an on-path triplet, forces one production `BeginFrame` Retry, reacquires successfully, and then renders 16 separately submitted acquisitions to cover slot wrap, plus one submitted blocked-depth capture for a shadowed fixture. Existing witnesses require finite/nonzero receiver output, preserved sky/geometry depth and bit 0, a nonzero prefix, and a nonzero final-light contribution. The missing-tail/prefix control is a **dropped-draw/nonblank control**, not a dropped-binding negative control. A deliberate dropped-binding negative is absent. Intermediate masks are assessed by unchanged actual PSO/state code and final pixel/stencil results; the oracle does not separately read back front/back intermediate surfaces.

## Initial execution and rejection gate

See `PROBE_COMMANDS.md` for exact commands, for root execution only. First compile the scoped probe, then run mixed 127/128 and three positive single-family cases. Run shadowed Spot/Omni cases before accepting the candidate for those routes. A fixture FAIL cannot be renamed a PASS or used to expand visual acceptance.

After correctness, one short ordinary same-binary off/on screen using the matched native harness is warranted. Keep compute/BVH/Flora controls identical between arms, observers and detailed timers off, and verify the telemetry route plus resource/weather/camera receipts. The deterministic work count is sufficient to test this candidate, but its whole-frame economics may be small. Reject a neutral/negative screen instead of broadening it into a general binding cache.

An epoch-scoped immutable Shared-CB snapshot cache is deferred. Current `R4MscResourceBinding` does not carry the upload epoch, and MSC binding does not perform the compute route's ownership admission. `OwnsUploadedRange` currently accepts exact full upload spans, while static local shadow draws bind subranges of a batch upload; blanket transplantation of that check would reject valid draws. A future cache must first solve this contract, discard/retry lifetime, write-generation invalidation and measured hit rate without per-bind hashing/maps. The selected local triplet candidate introduces no cross-acquisition state and does not depend on that larger change.
