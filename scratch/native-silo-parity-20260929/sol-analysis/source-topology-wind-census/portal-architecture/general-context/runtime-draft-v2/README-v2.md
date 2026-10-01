# Runtime portal census candidate v2 (scratch only)

Apply `runtime-observer.patch` from the v1 folder, then `runtime-observer-v2.patch` here. V1 remains untouched. The v2 delta corrects the R4 CMake source path and makes `complete=true` require a structural join for every one of the 5,316 same-frame Flora IDs.

A joined ID must map uniquely to a source `MT_TREE_ST` visual (type 7), have finite ordered parser bounds and authored tree lighting, valid vertex/index ranges and a source shader/base texture, agree with the captured leaf/trunk material and archive-winning B_TREE alpha/Object-LOD flags, and have a nonempty owner-sector list whose IDs are valid. Unjoined/unknown IDs remain raw-visible in the receipt and have zero owner tests; they increment `geometry_join_unknown_ids` and prevent `complete=true`. `geometry_join_scope` explicitly states this is structural metadata only, not GPU or wind-enclosure proof.

The native ID capture directory must be fresh per run, as the launch wrapper already supplies. The engine still checks the selected frame, extent, unique IDs, terminal footer, view/projection/eye/SSA bits, and exact indices before it can complete a receipt. V2 remains default-off, observe-only, and `suppressed=0`. No build or game was run for this review.
