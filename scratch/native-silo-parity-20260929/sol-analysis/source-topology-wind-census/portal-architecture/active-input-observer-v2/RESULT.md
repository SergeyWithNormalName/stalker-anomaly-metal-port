# Active-engine portal input observer

The frozen candidate02 completed `portal-active-inputs-01/native` with exit0. It writes no visibility decisions and skips zero draws. Exact result and hashes are in `runtime-result.json`.

The copied active engine IReader exactly matches the pinned winning level; generation1 and the engine marker `copy_ok=1` must both be present. The reader cursor is unchanged. Frame700 primary Calculate eye/view/projection/full matches the older fixed geometry fixture bitwise; saved-camera eye is equal. Live ViewportNear/SSA/LOD thresholds and CFORM downward/upward hit metadata are now measured directly, rather than inferred from config/offline double geometry math. Current/previous wind and benders are captured from the actual same-frame Flora callback.

This does not populate source CSector/Visual arrays, build a runtime graph, qualify animated bounds, prove visible rendering, or establish FPS/Silo superiority. General replay must consume these actual inputs and fail open on any missing/mismatched provenance. Sources and ordinary outputs were restored before gameplay; candidate01 was superseded to add saved-camera eye.
