# Compute-skin output oracle and transaction proof

Read-only source review, 2026-09-29. This is a bounded proposal for the later adapter route. It does not claim rendered or gameplay validation.

## Current lifecycle hooks

- `MetalR4DynamicWorldAdapter::Prewarm` assigns a fresh `R4DynamicWorldPrewarmState::epoch`; `mainPreparedEpoch` and `skinPreparedEpoch` gate draws. The cached output key includes geometry content/index, object identity, and acquired backend slot.
- `IR4Backend::EncodeComputeSkinJobs` has a strict `false => no compute skin output writes` contract. `MetalR4Backend` retains buffer wrappers and native version handles on the active frame context. This is the required synchronous fallback boundary.
- `MetalR4Backend::SubmitFrame` can reject a late drawable after scene encoding. That path abandons the uncommitted command buffer and reports `LastFrameSubmitWasRetry`. `MetalRenderDeviceRender` then calls `scene.DiscardUnsubmittedFrame`; `scene.OnFrameSubmitted` runs only after `SubmitFrame` succeeds.
- `MetalR4DynamicWorldAdapter::CommitSubmittedFrame` currently commits object motion only. GPU skin pose needs a separate pending journal and promotion on every successful submission. In `MetalR4EngineScene::OnFrameSubmitted`, call that skin promotion before the `viewHistory.HasPending()` early return; keep the existing temporal/object-motion commit behind the current history gate. This covers valid-view dynamic skinning when temporal history was not staged.
- A failed late submit must cancel pending GPU pose stamps. The next `Prewarm` gets a new readiness epoch; it may reuse only a previously submitted pose whose output buffer/version is still the same. A newly encoded but unsubmitted pose is never a cache hit.

## Small production output oracle

Run only with both the compute route and a separate oracle opt-in enabled. Let the adapter select a fixed list of installed-leaf keys using `geometryKey` (asset content hash plus geometry index), influence class, vertex count, and pose hash. Bound selection by leaf count and total bytes. For each selected changed-pose leaf, compute the reference with the existing `repackR4DynamicWorldSkinnedVertices` CPU function; do not read back or compute references for ordinary frames.

Add one narrow per-job oracle request carrying the stable key and owned expected `SKIN_0` bytes. The native backend preallocates bounded Shared staging buffers before dispatch, then after the compute encoder ends, encodes Private-output-to-Shared blit copies on the same command buffer. A diagnostic copy failure makes that receipt inconclusive; it must not convert an already-dispatched batch into `false` or trigger CPU fallback. Retain staging buffers, output wrappers/version handles, expected bytes, and keys in the frame context through completion.

Compare only in the existing command-buffer completion handler when status is `Completed`. Require exact equality of all 36 `SKIN_0` bytes against the original CPU output; the earlier `1e-6` position tolerance is superseded by the exact arithmetic qualification. An asynchronous command-buffer error is the existing fatal backend path. An abandoned retry has no completion and must discard its receipts without promotion. Do not add a gameplay wait, `getBytes`, or an always-on output copy.

The completion receipt should include stable geometry/instance key, influence count, vertex count, pose hash, output version/write serial, frame serial, status, byte mismatch count, and position max absolute/relative error. Keep a strict maximum number of leaves and bytes so the oracle has a clear memory bound.

## Wiring proof before the GPU oracle

Use a small `IR4Backend`/`IR4Resources` test double in the existing `RunR4DynamicWorldAdapterSelfTest` path. It only needs fake buffers that store bytes, fake resources/pipelines, a deterministic current-frame bone slice/epoch, and a compute encoder that records jobs and can simulate a successful write or a pre-dispatch `false`; unrelated pure virtual methods can fail closed. The fake must defer simulated GPU output writes until successful fake `SubmitFrame`; `DiscardFrame`/retry drops queued writes. Supply one real-format in-memory geometry fixture and DDS provider. This tests actual `Prewarm`, `Draw`, `CommitSubmittedSkin`, `CommitSubmittedFrame`, and `DiscardUnsubmittedFrame` without opening a render pass or creating a fake renderer.

Required order cases:

1. Pose A computes, successful submit promotes A, then the same A is a cache hit and Draw binds the matching output.
2. Pose B computes, late-submit retry discards B; another B must dispatch again. A later successful B submit promotes B; only then may B be reused.
3. One Prewarm sees duplicate same-key snapshot entries, with changed pose B first and committed pose A later. Cancel queued B; only committed A is drawn/submitted, and a later B must dispatch.
4. Pre-dispatch backend failure leaves output bytes unchanged and the real CPU repack/upload fallback becomes the prepared stream.
5. Output replacement, same-key CPU fallback, acquired-slot wrap, and repeated engine frame invalidate only the stamps whose output/version/epoch no longer matches.

The production scene currently calls dynamic `Prewarm` once from `MetalR4EngineScene::Render`; a temporal-producing second primary Render is rejected by `viewHistory.HasPending`. Treat one `Prewarm` per acquired gameplay command buffer as the route contract and test that call sequence. Do not claim arbitrary repeated direct adapter `Prewarm` calls on one unsubmitted command buffer are cancelable: already-encoded compute writes cannot be removed by clearing a CPU journal. Do not treat a standalone journal-helper test as proof of adapter wiring. Actual late-drawable retry and completion-copy comparison remain opt-in runtime gates after the mock transaction path passes.
