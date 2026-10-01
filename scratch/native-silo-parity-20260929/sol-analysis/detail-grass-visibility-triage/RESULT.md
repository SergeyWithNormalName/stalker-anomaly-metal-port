# Detail-grass visibility ROI triage

Read-only source and existing native-run review. No edits to renderer sources, builds, game state, or runtime traces.

## Finding

There is a real visibility-contract difference, but no cheap portal ROI is supported by current retained data. The original `CDetailManager::UpdateVisibleM` tests cached-grid spheres against the view frustum, tests individual detail-slot spheres when the grid result is partial, then applies `RImplementation.HOM.visible(S.vis)` per slot before distance fading and per-item SSA/dither rejection (`src/Layers/xrRender/DetailManager.cpp:297-459`). This path does not itself query portal visibility for each detail slot.

The Metal detail builder reconstructs candidates from `level.details`, the current cache footprint, static-CDB box/ray queries, and a distance-sphere reject (`src/Layers/xrRenderMetal/MetalR4EngineFs.cpp:585-889`). When `pOutdoorSector` exists it discards ground hits whose CDB triangle sector differs from that outdoor sector (`:746-750`). It does not run the source manager's view-sphere tests or HOM test. It groups grounded instances by model and 4x4-slot world chunk, in batches up to 61, and computes a world AABB (`:844-889`). The adapter then rejects a batch only if its AABB is wholly outside the six Metal clip planes (`src/Layers/xrRenderMetal/MetalR4DetailGrassAdapter.cpp:64-87, 975-981`). There is no retained batch sector, portal, or HOM-visible bit.

The CDB triangle sector is momentarily available during ground probing, but accepted instances are restricted to the single `pOutdoorSector` when that pointer is set; the sector identity is not copied into instances or batches. That does not provide useful per-portal sector partitioning for the admitted outdoor grass. Recovering HOM behavior would require validated per-slot/instance occlusion bounds and a qualified HOM query; the existing visibility code alone does not prove those native inputs or their correctness. I would not propose portal/HOM suppression from this evidence.

## Existing-frame ceiling

The pinned native engine log already gives an upper-bound count for additional culling after the current AABB frustum test. At staging it formed 1,606 drawable batches from 11,017 grounded/admitted instances. The same log reports actual main-view draws of 554 at frame 27, then 552 at frames 327, 627, and 927. Since `DrawIntoActiveGBuffer` has only the batch clip rejection between the staged batch list and incrementing `draws`, this implies 1,052–1,054 batches were already removed by the existing clip test in those samples. Any additional visibility test could remove at most the remaining 552–554 draws per sampled frame, and actual saved GPU work is unknown because batches differ in size and visibility cost. This is a hard count ceiling for those frames, not an estimate of plausible portal/HOM savings or an FPS claim.

The source telemetry does not log `frustumRejectedBatches`, candidate IDs, or per-batch instance counts alongside those frames, so the subtraction cannot identify which batches survived or estimate the cost they represent. Existing stage counts and draws are sufficient only for this coarse ceiling. There is no fixed-frame batch/sector fixture to replay without new observation.

## Decision

Reject a portal-grass ROI for now. A future observe-only fixture would need to retain per-batch instance membership plus a justified sector/HOM association and same-frame camera visibility evidence. Sector-only annotation is unlikely to help the current admitted set because the source-compatible ground filter restricts it to `pOutdoorSector`; HOM remains unqualified. The source manager also has per-item SSA/dither decisions that the native batch count does not expose, so this review does not quantify that separate difference.

Evidence inputs:

- Native run `scratch/native-silo-parity-20260929/runs/native-vs-matched-mo2-baseline-01/native/output/engine.log`, SHA-256 `520e9264f9b286fc6ddb9777d6efacd31ba2d287a51cdc86a54905b88b929b39`.
- `src/Layers/xrRender/DetailManager.cpp`, SHA-256 `e5d407c975e8a37e676007654c0f868f6806cfbf635acd0745812340d17d8435`.
- `src/Layers/xrRenderMetal/MetalR4EngineFs.cpp`, SHA-256 `224f005037e52a379543f3c7fd041e94048ff070d7db2b65f191c6c97018387b`.
- `src/Layers/xrRenderMetal/MetalR4DetailGrassAdapter.cpp`, SHA-256 `d5e027f879f6636e85e71bc68417955f09fcdf466dec46baf779e242c6bf8178`.
- `src/Layers/xrRenderMetal/MetalR4EngineScene.mm`, SHA-256 `c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53`.
