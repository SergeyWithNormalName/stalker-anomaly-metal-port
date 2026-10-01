# Scratch Flora rest metadata and validation draw adapter v3

V3 preserves the v2 opt-in rest metadata behavior and adds two validation-only wrappers over the same main G-buffer draw loop:

- `DrawIntoActiveGBufferForValidation(..., groupExactKeys, ...)` refuses `groupExactKeys=true`; false submits the normal unclipped source list through existing clip behavior.
- `DrawIntoActiveGBufferForPortalValidation(..., excludeVisualIds, ...)` requires the exclusion IDs to be sorted and unique, then uses binary search without copying or sorting. It tests ordinary Metal clip visibility first and applies the exclusion only afterward. `actuallySkipped` counts only IDs that survived ordinary clip and were actually omitted.

Both validation calls restore the adapter's full telemetry state on every return path and report a sampled telemetry value. Validation mode disables selected-frame main-ID CSV output, so a pixel-oracle pass cannot overwrite the source receipt. The production entry point calls the shared loop with no mask and `validationOnly=false`; it does not copy or sort any vector. Existing main source order, per-draw constants/uploads/resources, and all shadow paths stay intact. This is an unbuilt scratch draft, not a production change.

Rest metadata remains default-off unless `XRAY_METAL_FLORA_REST_METADATA=1`. See the v2 README for rest center, indexed-position, S4 decode, and helper details. The output remains metadata only; no portal visibility is applied by the adapter unless a separately guarded validation caller explicitly supplies exclusions.

## Validation scope and limitations

No build, game launch, or pixel comparison was run here. The Scene oracle must provide an already sorted unique membership list generated only from complete known rest records and complete portal ownership/frusta. If it cannot prove those inputs, it must pass an empty list. The validator's sorted/unique check is a structural contract; it does not prove the caller's classification. Exact-key grouping remains intentionally unavailable.

## Files

The header and implementation are the only changed adapter files, derived from v2. The top-level rest/wind helper headers are copied unchanged for staging. `adapter.patch` is the diff against the immutable v2 adapter copies.
