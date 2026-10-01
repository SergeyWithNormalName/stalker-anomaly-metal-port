# Portal-pixel Scene draft review

The scene integration is small in ownership terms: the OFF/ON oracle runs in the active G-buffer pass immediately before the unchanged scene-owned world callback. It reuses the existing frame-local texture targets, base-copy identity checks, four-MRT comparator, and D32S8 readback. The normal production callback remains below the diagnostic and still runs once in source order.

The candidate mask uses only source portal-false records as a broad-phase set. For each known rest record it calls the existing wind-envelope helper with the frozen frame motion, then excludes only when the union sphere misses every reached frustum of every resolved owner. Unknown records, wind bounds, owners, or frusta remain visible. Both a nonempty candidate ID set and at least one actual camera-visible skip are required before pixel comparison can be treated as meaningful.

The frame observer draft also serializes all seven SSA bits, hashes the exact retained `scene.geometryBytes`, and requires `mFullTransform` to bit-match `mProject * mView` using the source `Fmatrix::mul` order before a portal result or candidate is qualified.

This draft is intentionally not build-ready. It references the adapter agent's pending `R4FloraRestRecord`/`RestRecordsForValidation()` metadata API and an unresolved validation-only `DrawIntoActiveGBufferForPortalValidation` call. Root must join that seam; this package does not alter adapter files or the production draw callback. No build or game run was performed.

Even a later strict pixel pass would only qualify that frame and pose. The raw source portal flag alone is not GPU wind proof, and this draft makes no general safety, shipping, or FPS claim.
