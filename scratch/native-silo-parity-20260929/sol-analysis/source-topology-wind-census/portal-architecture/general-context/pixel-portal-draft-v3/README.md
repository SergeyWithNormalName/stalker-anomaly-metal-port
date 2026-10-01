# Scene-owned portal pixel-oracle draft v3

This immutable scratch package starts from root's composed stage06 Scene snapshot and keeps its callback-name fixes and `R4FloraWindEnvelope.h` helper. It changes only the candidate-population gate relative to that snapshot. V1 and v2 remain unchanged.

The oracle still compares the source-order OFF branch with an ON branch that filters only validated candidate IDs after normal camera clipping. It runs inside the scene-owned G-buffer pass before the unchanged production callback, which executes exactly once in source order. Shadow paths remain untouched.

The v2 gate incorrectly treated the 5,316 IDs observed in the captured visible frame as the adapter's entire prepared population. Stage06 runtime disproved that assumption: the adapter prepared 11,663 Flora draws while the captured frame had 5,316 main draws, with 6,347 removed by ordinary camera clipping. V3 therefore has no hardcoded prepared-total count. It requires a nonempty rest-record vector whose size exactly matches the adapter's `admittedVisuals` telemetry, derives the complete expected prepared set from retained type-7 geometry plus the captured B_TREE leaf/trunk and non-Object-LOD material flags, and requires exact unique-ID set equality. The full pool is validated; per-frame camera clipping later decides which excluded IDs can actually be skipped. Unknown individual rest bounds or owner/frustum data stay visible. Any missing, duplicate, extraneous, or unjoined prepared source ID disables the whole mask.

The same-frame `floraMotion` and `floraEnvironment` checks, source-order transform validation, source portal broad phase, outward-rounded rest-plus-wind envelope, and all-owner frustum checks remain. Candidate IDs are sorted and unique for the adapter's membership test; draw order remains the original source order.

This is a static scratch draft only. It has not been compiled or run. The adapter v3 API remains separately owned and requires `XRAY_METAL_FLORA_REST_METADATA=1`. No general culling safety, shipping, pixel-quality, or performance claim is made.
