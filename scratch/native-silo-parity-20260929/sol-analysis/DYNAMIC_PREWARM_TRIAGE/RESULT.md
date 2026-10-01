# DynamicWorld Prewarm residual CPU triage

## Measured scope

The stage-10 run is `runs/portal-perframe-full-on-indoor-10/native/output`. Its exact measurement markers are engine frames 537–1092; I excluded the later post-window tail. The 554 completed E01 rows in that range show `dynamic_prewarm_ms` mean 5.025 ms, median 5.005 ms, p90 5.211 ms. This is nested within scene CPU timing and is attribution, not additive frame cost.

The sampled Dynamic OGF log lines report roughly 754 cache hits, one miss, 753 skinned draws, and 700 pose reuses around frames 627/927. Those counts do not mean that Prewarm was skipped.

## What the pose reuse count means

`pose_reused` is `m_telemetry.skinnedVerticesReused`, incremented only when `R4ComputeSkinPoseMatches` accepts a cached output in `MetalR4DynamicWorldAdapter.cpp:2492-2496, 2785-2795`. The instance key includes geometry, stable object ID (or snapshot index), and the acquired backend slot (`:2351-2352`); the slot is selected from `ActiveFrameContextIndex` (`:2113-2123`). Thus the cache can reuse output when a slot cycles back to the same pose, but this counter is a per-Prewarm tally and the code explicitly handles a later duplicate snapshot entry reusing a same-key provisional pose (`:2787-2792`). It is not a measurement of cross-frame-only reuse or of total Prewarm work eliminated.

Even on a reuse hit, Prewarm still walks renderables and geometries, builds string keys, checks frustum/cache state, computes view/world matrices, and updates each cached instance’s vertex globals, transforms, and fragment globals (`:2234-2351`, `:2374-2460`, `:2800-2820`). The cache hit suppresses CPU skin repacking/output upload for that matching geometry; it does not reuse the frame’s transforms or skip dynamic draw preparation. The separate `METAL_DYNAMIC_OGF_FRAME` lines at frames 627 and 927 report the high reuse counts during the measured interval; they remain periodic aggregate receipts, not sub-timing attribution.

Pose reuse also still pays the pose comparison setup. The engine capture creates and fills a fresh immutable bone-transform vector under the kinematics lock (`src/Layers/xrRender/r__dsgraph_build.cpp:369-400`); Prewarm hashes the full float bytes once per renderable that has skinned geometry (`MetalR4DynamicWorldAdapter.cpp:2374-2378`, `:457-474`) before testing `R4ComputeSkinPoseMatches`. Pointer-identity reuse would not be a safe substitute because the producer publishes a new vector each capture and its values may change.

## Candidate decision

I do not see a low-risk optimization supported by this evidence alone. Exact-byte suppression of unchanged per-instance uniform `UpdateBuffer` calls could be semantically safe only after maintaining CPU mirrors, but it would skip updates (outside this request), still requires rebuilding/comparing current values, and later MSC binds continue snapshotting Shared constants. String-key/map work is repeated, but replacing keys is a broader cache refactor with no measured attribution.

Recommendation: reject a code change for now. The stage-10 receipts establish a roughly 5 ms Prewarm scope and substantial pose-output reuse, not a small, proven remaining cost that can be removed without changing model/update work. A future diagnostic should first use existing E07 timers to attribute skin hash/repack, bounds, upload, or other work, then revisit only the measured component.
