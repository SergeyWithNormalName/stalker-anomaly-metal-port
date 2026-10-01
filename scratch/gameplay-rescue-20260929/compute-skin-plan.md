# Compute skin once: source contract and staged implementation

2026-09-29. Isolated prototype source implemented; no build/GPU/game run by this worker yet. Production skinning and its CPU bounds guard remain unchanged. Existing-file prestate is `compute-prestate/manifest.json`; task-only delta is `compute-only.patch`.

## Concrete budget

Current full GAMMA new-game CPU skin repack p50 is 5.11 / 6.67 ms and heavy saved-game diagnostic 9.91 ms. Local-shadow CPU is a different inclusive stage and cannot be added. The prototype targets changed-pose skinning once, preserving output reuse between G-buffer, sun/local shadows, emissive and lightplanes. GPU frame is already material (22–38 ms elapsed), so no net frame gain is assumed from moving this CPU budget.

## Existing source contract

- `MetalR4DynamicWorldAdapter.cpp:688–743`: raw skinned format admission is 1W stride60, 2W stride64, 3W stride70, 4W stride76. Three-weight data is unaligned. Format aliases 1/2/3/4 and engine IDs 0x12071980/0x240e3300/0x481c6600/0x5a237f80 are accepted.
- `:1123–1220`, `:1228–1429`: current CPU reference reads raw vertices, computes weighted current-pose P/N/T/B, normalizes basis, then emits exact 36B SKIN_0. Two weights use [1-w,w]; 3/4W last weight is signed 1-sum(explicit), never clamped. Nonfinite T/B in some shipped multiweight meshes triggers whole-geometry tangent rebuild on deformed geometry, not rejection.
- `:774`: packing is floor((clamp(component,-1,1)+1)*127.5), followed by clamp to [0,255]. Output basis bytes are BGR for MSC's legacy .zyx swizzle. All alpha bytes are zero, position W=1, UV is copied exactly.
- `MetalR4DynamicWorldAdapter.h:51`: output ABI is float P[4] + uchar N/T/B[4] + float UV[2], stride36. Native MSL float4/float3 members would introduce unwanted alignment; scalar-array layout is explicit.
- `:2037–2050`: current output cache keys geometry/instance plus the acquired backend context slot, not engine-frame modulo. Hash reuse and a bounded instance cache already exist; misses are rare, changed poses still produce substantial work.
- `:2370–2490`: current CPU skinned output is uploaded to Private buffers. GPU consumers reuse the same cached buffer across every pass. CPU hasSkinPose is marked during Prewarm, which is currently safe because private-buffer uploads have retry journals.
- `:2404–2438`: exact per-vertex world bounds reject nonfinite/implausible output. These bounds do not supply scene/light admission; both later draw paths continue using the snapshot renderable sphere. Removing this admission guard blindly would change behavior.
- `:2554–2584`: Commit/Discard currently cover object-motion history only. World snapshot has current boneTransforms, no previous-bone snapshot. Existing SKIN_0 motion uses previous camera/object transforms with current skinned vertex positions. This migration preserves that contract; true animated-deformation motion is a separate feature.
- `IR4Backend.h:529–645`: no scene compute API is exposed. `MetalR4Backend.mm:3898` has a narrow internal thumbnail kernel. The backend uses ordinary tracked MTLCommandBuffer/encoders, not MTL4CommandQueue; integration must follow its existing ownership rather than importing Metal 4 barriers blindly.
- `MetalR4Backend.mm:1137–1144`: frame-context acquisition waits for previous use before resource/arena reset. `:1631` creates immutable frame upload slices; `:2852` creates Private/Shared buffers; `:2975` stores Private upload retry journals. `:1759` late-drawable retry may abandon a fully encoded command buffer. This is the critical compute-cache transaction boundary.

## Stage 1: isolated kernel and independent oracle (implemented, unverified)

Files: `R4ComputeSkinFoundation.h/.cpp`, `R4ComputeSkin.metal`, `tests/R4ComputeSkinGpuProbe.mm`. New CMake target `XRay.Metal.ComputeSkinGpuProbe`; focused CTest `XRay.Metal.ComputeSkin.GpuOracle`.

The foundation decodes immutable raw OGF bytes into an 88B scalar-array canonical input (P/N/T/B/UV, uint bone IDs, float weights). It checks exact payload sizes, local indices, referenced bones, finite weights and P/N/UV. Nonfinite T/B returns `CpuFallback` for the entire geometry. This is a data-driven admission path for installed/modded meshes, not an asset allowlist or replacement mod tree. Production integration must route ANY non-Ready GPU admission to the existing CPU path, including Invalid: only the original CPU verdict may reject a model. This matters for 1W normal/T/B NaNs which existing normalized() can replace with {0,1,0}; multiweight nonfinite T/B uses whole-geometry repair. GPU eligibility can be stricter without losing compatibility.

The native MSL kernel skins all 1–4 influences and writes the proven 36B SKIN_0 stream. Runtime pipeline uses MTLMathModeSafe plus MTLMathFloatingPointFunctionsPrecise (older SDK runtime fallback disables fast math). Kernel and C++ static_assert both input/output strides. P and basis operation order mirrors the existing CPU reference.

The offscreen probe uses an independent MTL device/queue, Shared immutable input/pose, Private output, guarded bytes around the output span, ordered blit readback and completion wait only in the oracle. It runs existing exported `RepackR4DynamicWorldSkinnedVertices` as the reference. Receipts report exact-vertex mismatches, position max absolute/relative error, N/T/B byte deltas, UV and alpha errors. Packing deltas must be exactly zero; no implicit one-byte tolerance. Position relative acceptance is 1e-6 and all numeric errors are explicitly reported. If packing thresholds differ due float arithmetic, preserve evidence for an architecture decision instead of silently relaxing the gate.

Fixtures cover raw 1–4W, unaligned 3W, ordinary/signed residuals, rotated/sheared/translated bones, varied basis, nonfinite-T/B fallback and malformed stride. CPU bounds and render targets are not part of this standalone admission proof; they remain separate integration gates.

`--timing` is one bounded arithmetic screen at 131,073 vertices per influence class. It reports one CPU-repack sample and ten GPU command-buffer elapsed samples after two warmups, plus CPU encode. GPU elapsed includes output guard-clear and readback, so it is not a pure-kernel counter and cannot be interpreted as renderer speedup. Pipeline/buffer creation is outside that timed command window. No CPU+GPU sum or occupancy claim.

Validation command, only after current gameplay screen is finished:

```sh
cmake --build build --target XRay.Metal.ComputeSkinGpuProbe
ctest --test-dir build/src/Layers/xrRenderMetal -R '^XRay.Metal.ComputeSkin.GpuOracle$' --output-on-failure
build/src/Layers/xrRenderMetal/XRay.Metal.ComputeSkinGpuProbe --timing
```

Use actual configured build/probe paths if this repository's generated layout differs. Build should also run existing `XRay.Metal.DynamicWorldAdapter.SelfTest` to preserve independent raw/repair contracts.

## Stage 2: bounded production integration, only after architecture review

Ownership: native backend API/kernel encoding in IR4Backend.h + MetalR4Backend.mm; adapter job/cache/CPU bounds fallback in MetalR4DynamicWorldAdapter.cpp/.h; no engine capture/model-loading changes; scene Prewarm already occurs outside the G-buffer callback before all consumers.

Add a narrow `EncodeSkinnedVertices`/`EncodeSkinJobs` descriptor rather than broad general-compute machinery. Each job references immutable canonical input buffer, current bone upload slice, exact output buffer/range, vertex count and influence count. Native implementation checks active acquired context and no active render encoder, closes pending Private uploads before starting compute, binds all resources on the same current frame command buffer, retains wrappers/native version handles until completion, and ends compute before G-buffer/shadow consumers. Default unsupported backend capability falls back to CPU.

Decode and upload canonical geometry once under existing geometryKey/content hash. Retain original XRFS source identity and existing texture/shader admission. Store maximum referenced bone ordinal to validate live pose length cheaply. Validate captured bone matrix finite data once per renderable; upload current bones once through UploadFrameData outside the inner geometry loop, sharing that immutable slice among all jobs for that renderable. Compute pose hash once per renderable. Source nonfinite T/B and unsafe transform magnitude stay on full current CPU skin+repair.

Use the existing Private skinnedVertices per-instance acquired-slot buffer. On pose match skip kernel exactly as the CPU path skips repack/upload; on change encode once, and every consumer binds that same output. The first implementation may dispatch each changed geometry through one compute encoder; measure dispatch count and scheduling cost before adding a packed multi-job scheduler.

Preserve CPU exact position-only world-bounds validation as the first integration guard: compute weighted positions and their world transform using current CPU semantics, without basis transforms, normalization, byte packing, repacked output allocation or vertex upload. It remains O(vertices) but avoids most arithmetic/packing work. Compare its bounds/rejection result to the full CPU reference. Nonfinite source/pose/basis, implausible bounds, GPU allocation/pipeline failure or unsupported format uses the current full CPU fallback. Do not loosen guard thresholds. A later per-bone conservative bound shortcut is allowed only with exact-CPU fallback whenever its looser bounds cannot prove admission.

## Transactional GPU pose cache

Each instance has a committed GPU pose stamp and a pending stamp associated with the acquired slot, output buffer version and unique encoding epoch. During Prewarm a newly encoded output may serve all consumers in the same command buffer, but must not update the committed reusable-pose flag. The adapter stores `PendingGpuSkin` journal entries by stable instance key plus expected buffer/version (avoid raw unordered_map element pointers).

On successful Submit, CommitSubmittedFrame promotes pending stamps. On late-drawable retry, failed encode, unsubmitted discard, shutdown or asset replacement, DiscardUnsubmittedFrame clears pending entries and retains/invalidates the previous committed output as appropriate. An abandoned command buffer never executed compute, so a matching next-frame pose must redispatch unless it matches the prior genuinely submitted output. New allocations with no committed stamp cannot be reused. Existing CPU upload journal behavior remains unchanged for fallback entries.

A GPU command-buffer failure after submit is already fatal through backend HasCommandBufferError/BeginFrame checks; do not continue using that failed output. Repeat engine frames/second viewport still use acquired context identity and encoding epoch. Cache pruning and model swaps must preserve retained resources until their submitted frame completes.

## Integration proof and economic gates

1. Independent oracle over representative installed meshes from actual gameplay captures, all 1–4 weights, fallback tangent repair, aliases, rotated/scaled bones and signed residuals. Keep exact packing gate until evidence justifies an explicit tolerance decision.
2. GPU/CPU production pair against the same immutable state: isolated G-buffer normal/position/motion and R32 sun/local shadow output, including emissive/lightplanes; no separate-world screenshots as proof.
3. Forced `XRAY_METAL_E08_FORCE_SUBMIT_RETRY`, `XRAY_METAL_TEMPORAL_COLD_UPLOAD_PROBE`, repeated engine frame, second viewport and slot-wrap: no unwritten/reused wrong pose. Inspect committed/pending stamps and count redispatch; no GPU wait in production.
4. Full-GAMMA same-binary CPU/GPU A/B with ordinary installed XRFS assets: changed-pose jobs, fallback share, bone/upload bytes and end-to-end critical path. Gross 5–10 ms CPU budget is only a ceiling. Added compute/dispatch/bounds overhead and existing GPU load determine net value.

## Exact arithmetic qualification update

The adversarial ±1 normal-byte gate is repaired without widening tolerance. Explicit observed CPU transform/dot FMA plus disabled implicit weighted-blend contraction yields bitwise equality on all 24 synthetic/stress cases and 30 GPU-eligible installed leaves under the generated palette; the remaining 17,139-vertex installed leaf correctly remains CPU fallback. See `compute-numeric-qualification.md` and its frozen-reference receipts. Old prototype timing is historical; remeasure this revision after the build is quiet. Pending GPU stamps must reuse scene_cpu current unique Prewarm readiness epoch (`mainPreparedEpoch` / `skinPreparedEpoch`); no separate epoch. Production shader embedding derives from canonical R4ComputeSkin.metal with CMake dependency, matching safe/precise oracle flags.
