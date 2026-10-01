# Stage 13: general LMH main-pass portal draft

## Proposed seam

`XRAY_METAL_LMH_PORTAL_PERFRAME` is a strict, default-off gate accepting only `observe` or `on`. It consumes the already-produced per-frame portal graph from the existing `XRAY_METAL_FLORA_PORTAL_PERFRAME` observer path; that existing graph flag must also be enabled. For a non-suppressing run, use `XRAY_METAL_FLORA_PORTAL_PERFRAME=observe` and `XRAY_METAL_LMH_PORTAL_PERFRAME=observe`. For a later isolated suppression test, use Flora `observe` and LMH `on`, so the existing Flora route stays unsuppressed.

The main G-buffer draw receives a borrowed context only for the synchronous `renderProductionScene` call. It is enabled only when generation, frame, source-order projection-times-view validation, current eye/view/projection bits, target extent, graph validity, and exact scene/cache owner maps match. Invalid, stale, or unavailable context means no LMH draw is suppressed. The static motion probe still calls the existing draw function without this context.

Inside `drawLevelScene`, eligibility requires the actual admitted visual-to-`TextureMaterial` lookup, matching visual/shader IDs, and the source `lmap#` third texture slot. The predicate runs after the existing camera-frustum test and before uploads/binds/draw. It uses indexed source position bounds only when they exactly match the prepared binding bounds, transforms all eight AABB corners through that draw's model matrix, and tests each reached frustum for every validated owner sector. Unknown bounds, transforms, owner data, or frusta keep the draw. A proven outside visual is still drawn in `observe`; `on` skips only that individual main-pass draw in the existing source order. No shadow call receives this context.

The counter seam distinguishes prepared LMH eligibility, ordinary-camera survivors, portal `would_skip`, and actual skipped draws. It does not reorder or batch draws. The only portal call site is the main opaque G-buffer level pass.

## Evidence and limits

The indoor07 frame-700 offline source-family screen found 2,185 shader-slot-2 `lmap#` visuals; 1,205 survived the ordinary camera clip and 908 were outside every reached owner frustum. Those 908 are a source-family upper bound, not runtime prepared-material admission or a real skip count. The related report now correctly records 45 source-observer validation checks.

This draft has not been compiled or run. It does not establish image equivalence, temporal safety, a Windows comparison, or a performance gain. Pixel-oracle admission and runtime profiling remain separate gates.

## Review points

- `R4PortalMainPassContext` pointers are borrowed from the scene implementation and used only synchronously during the current frame.
- The owner map comparison currently walks the complete vectors once per main pass; if its cost is material, a cached exact geometry/topology identity is a later review item.
- Strict affine transform validation and exact prepared/source bound equality fail open; these may reduce eligibility but cannot make the candidate more aggressive.
- Source-order and shadow behavior are structurally unchanged in this draft, but must be confirmed by the root's diff/build review before any runtime test.
