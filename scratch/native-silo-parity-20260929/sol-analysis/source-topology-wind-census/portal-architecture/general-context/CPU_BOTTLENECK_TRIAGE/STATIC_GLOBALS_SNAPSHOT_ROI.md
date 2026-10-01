# Candidate CPU target: repeated frame snapshots of Flora static globals

## Source contract

`LevelScene::baseLightingStaticGlobals` is a 528-byte Shared constant buffer created during Metal resource preparation (`MetalBootstrap.mm:2661-2669`). `updateBaseLightingConstants` constructs its 528-byte CPU image and writes that resource in the preparation path (`MetalBootstrap.mm:3292-3302,3472-3479`); its only callsite in this file is `MetalBootstrap.mm:3700`. In the live render path, the per-frame vision updates at `MetalR4EngineScene.mm:7428-7437` target the separate 384-byte `baseLightingGlobals` buffer, not `baseLightingStaticGlobals`. A repository-wide search finds no other writes to `baseLightingStaticGlobals` after preparation. The frame render runs on one renderer thread, so the static buffer's bytes are stable while main Flora is submitted.

The scene passes this same shared buffer into `makeFloraView` (`MetalR4EngineScene.mm:7648`). Main Flora binds it once in the trunk VS table, and twice for leaf draws (leaf VS and PS) (`MetalR4FloraAdapter.cpp:1013-1031`). Current `MetalR4Backend::BindMscResources` snapshots every Shared non-frame-upload constant binding by calling `UploadFrameData` on `buffer->Native().contents + binding.bufferOffset` (`MetalR4Backend.mm:2925-2952`). Consequently each Flora stage bind copies the same 528 bytes into a fresh frame-arena range before descriptor binding. For a 5,316-draw census, this is between 5,316 and 10,632 repeated copies (about 2.7–5.3 MiB/frame), depending on leaf/trunk mix. This is a source-count ceiling/estimate, not measured traffic or FPS savings.

## Smallest candidate

Snapshot the 528-byte base-lighting static buffer once into the active acquisition's frame upload arena immediately before the main Flora draw loop, then bind the returned `R4FrameBufferSlice` at the same VS/PS TLAB slots for every Flora draw in that callback. Keep all three per-visual uploads (`vertexGlobals`, `fragmentGlobals`, `transforms`) as fresh immutable slices; leave textures, sampler, draws, order, shadow paths and pipeline bindings unchanged. The static slice is callback-local and must be regenerated on each acquisition; never retain it across frame-slot reuse. Its returned `acquireEpoch` should be checked against the active backend acquisition before binding.

The current `IR4Backend` exposes `UploadFrameData(data, size, alignment, slice)` but does not expose read access to an `IR4Buffer`'s Shared bytes. The adapter cannot safely `memcpy` from an abstract `IR4Buffer`. The minimal seam is one narrowly scoped backend helper that accepts a Shared constant-buffer handle/range, validates it, snapshots its native shared bytes once to `MetalFrameUploadArena`, and returns a frame-epoch slice. It must not cache across acquisitions. Flora then binds that slice's buffer/offset/size, so `BindMscResources` sees a frame-upload buffer and skips the repeated Shared snapshot path.

## Proof boundary and previous work

This does not change constant values: the source bytes are copied verbatim once, and every shader still reads the same 528-byte block. It changes the address to a current-acquisition immutable slice. The present upload ownership probe deliberately accepts only exact ranges returned by `Upload`; it rejects subranges/superspans (`MetalR4Backend.mm:1035-1054`). The proposed static snapshot uses the exact returned slice, so it fits that contract; do not manually manufacture smaller or stale slices.

The earlier `SOURCE_ROI_REPORT.md` local-light bind-reuse candidate removed one duplicate 528-byte static-global snapshot per eligible light, about 164,208 copied bytes for its logged 311-light frame. Its short OFF/ON screen did not establish a performance gain, and the UI-contaminated on-glass arm was invalid. This new path is Flora-specific and has a much larger repeated count, but the prior result is a caution that saving snapshot copies may not move whole-frame FPS.

`XRAY_METAL_E08_DEDUP_RESOURCES` only avoids repeated `useResource` declarations and resource-retention work. `BindMscResources` performs the Shared-buffer `UploadFrameData` copy before `declareIndirectRead`, so that switch does not remove these 528-byte snapshots. The Flora CPU vector-reuse flag likewise addresses staging storage, not this backend snapshot copy.

## Minimal experiment plan

1. Add the helper only behind a validation/default-off flag and enable it for the main Flora callback; report snapshot count/bytes and require exactly one valid current-epoch slice per callback.
2. Compare source and cached-slice paths with the same frame's four G-buffer MRTs plus D32S8; require byte-exact equality and preserve the existing `UpdateBuffer`/render order.
3. Read the existing E08 bind/upload CPU counters (slots 1 and 2) and complete one clean same-binary paired screen only if the copy count and CPU counter show a meaningful reduction. Keep the oracle and resource dedup controls identical.

No code, build, launch, or new profiling was done for this read-only screen.
