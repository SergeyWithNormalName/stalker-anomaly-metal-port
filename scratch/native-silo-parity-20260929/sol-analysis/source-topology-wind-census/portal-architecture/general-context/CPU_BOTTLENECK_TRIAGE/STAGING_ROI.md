# Flora CPU staging ROI: bounded source audit

## Avoid repeating prior work

The saved broad resource-dedup screen is not a Flora staging test. `v9r1-dedup-on-screen-01` overlaps CUA observation; its own receipt says to exclude on-glass performance and reports no useful callback signal. Its note says resource dedup remained off, so it cannot qualify either side. The earlier `SOURCE_ROI_REPORT.md` is a **local-light** bind-reuse candidate, not Flora. Its same-camera bind-reuse-off/on callback means are 19.3114/19.1844 FPS with the same 52 ms callback median; the on-screen arm is invalid. Do not port that result into Flora, but it supplies no reason to repeat a general binding cache experiment.

A separate main-Flora pipeline grouping pair did not show gain: 26.1754 OFF vs 26.0972 ON on-glass FPS; CPU encode p50 34.2207 vs 34.0939 ms. It cut periodic main pipeline calls from 5,316 to 2 but kept every resource/constant bind and draw. This rejects another broad bind/order hypothesis for now. It does not test CPU staging allocation or zero fill.

## What is actually mutable per draw

Stage07 already contains an opt-in CPU staging reuse path: `m_impl->reuseVisibleGlobals` selects persistent `visibleVertexGlobals`, `visibleFragmentGlobals`, and `visibleTransforms`; when unset, the loop uses three local vectors. Both paths still call `assign(size, 0)` for every submitted visual. I found no saved run receipt that sets `XRAY_METAL_E08_FLORA_REUSE_GLOBALS`, so this specific Flora staging control has no comparable A/B result in the saved runs.

The three arrays are scratch sources only. The main loop rewrites per visual:

- Vertex globals: current/previous VP, model and view-model, fixed S4 scale, tree lighting constants, time/wind values, and the leaf bender block when `view.windValid` is true. The only unconditionally untouched vertex span is bytes 128–159. For a leaf with invalid wind, bytes 160–1215 also need to stay zero; they cannot retain a previous leaf's benders.
- Fragment globals: only the first 128 bytes (WVP and previous VP) are written. The rest of the 176-byte trunk or 224-byte leaf buffer must remain zero, matching current source behavior.
- Dynamic transforms: bytes 0–207 are written (three matrices plus the final float4); bytes 208–255 must remain zero.

So repeated heap allocation and clearing are avoidable in principle. The safe narrow version is persistent, per-material CPU staging buffers initialized to zero once, with all changing fields rewritten every draw, explicit zeroing of the leaf bender span only when wind is invalid, and untouched padding/tails kept at their initial zero value. A single reused set of vectors would need careful leaf/trunk size transitions; separate leaf/trunk buffers make that state contract easier to review.

## Ownership boundary and recommendation

Do **not** reuse or mutate GPU slices. Keep the three existing `UploadFrameData` calls: `MetalFrameUploadArena::Upload` copies the supplied bytes into the active acquisition's buffer at a fresh aligned offset and returns that immutable slice. Reusing only the CPU source buffers preserves that contract and removes neither uploads nor bytes; it can only reduce CPU vector allocation and `assign(...,0)` work. This is consequently a small CPU hypothesis, not GPU bandwidth or draw-count savings.

The least invasive first experiment is already present: compare the existing `XRAY_METAL_E08_FLORA_REUSE_GLOBALS` flag off/on under a clean same-binary pair, leaving all three immutable uploads and every draw unchanged. It tests heap allocation/reuse only; because `assign` still zeros the full buffers, it does not test clear elimination. If the existing switch has no repeatable CPU/whole-frame benefit, stop. Only with a positive signal should a second scratch candidate replace per-draw full `assign` with persistent zero-initialized per-material buffers and targeted invalid-wind clearing. Measure staging CPU separately from `UploadFrameData` so the immutable copy cost is not misattributed. No new launch or code change is recommended from this read-only review.

## Source anchors

- Stage07 `MetalR4FloraAdapter.cpp`: reuse flag/state at 643; per-draw staging and zeroing at 1177–1187; dynamic writes at 1190–1259; three uploads at 1263–1268.
- `MetalR4Backend.mm`: `UploadFrameData` calls `MetalFrameUploadArena::Upload` at 1914–1931; the arena copies into a fresh aligned offset and advances its cursor at 954–1005.
- Existing saved comparison: `sol-analysis/flora-main-material-order/RESULT.md`; local-light bind result: `sol-analysis/SOURCE_ROI_REPORT.md`; dedup screen contamination: `runs/v9r1-dedup-on-screen-01/native/output/ui-observation-contamination.md`.
