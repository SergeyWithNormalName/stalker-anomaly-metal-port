# Per-frame portal + wind candidate: minimal wiring and gates

Read-only review of `runtime-root-stage-08/stage`; no production source, build, or game changes. The draft is default-off and preserves the ordinary Flora path unless every frame gate passes. Its current purpose—retain the complete source/population join and measure cost before simplifying—is appropriate.

## Once per level generation

- Parse the active `level`/`level.geom` and qualify sector roots, portal topology, owner lists, portal planes, and generation identity. The prepared portal cache owns only value data and its portal CDB model; frame traversal borrows the static CFORM only for the synchronous start-sector query.
- Build the admitted main Flora draw inventory from the adapter's actual prepared draw records, not a saved frame-700 ID file. Join each unique visual ID to shader/material family, geometry/index ranges, all owner sectors, and the init-time indexed rest record (world enclosing center/radius, transformed H interval, UV maximum). Any missing/duplicate/unsupported row remains visible; a population mismatch disables the whole candidate.
- Retain the generation key and validated array/index relationships. The current stage still recomputes this complete join per frame intentionally so `mask_cpu_ms` measures the full first hypothesis before any caching/refactor.

## Required mutable inputs per frame

Capture the current engine frame and level generation with eye, saved eye, view, projection, full transform, drawable dimensions, source SSA/portal settings, and a synchronously borrowed static CFORM. Recompute starting sector and reachable portal frusta from that view. Require the callback view/frame/generation/extent to match the captured values exactly.

Use the same-frame Flora wind/environment snapshot: `windValid`, wind/tree setup, bender setup, and current plus previous benders. Recompute each possible exclusion with the current/previous wind envelope and every owner-sector frustum; do not carry an earlier frame's exclusions forward. Unknown/nonfinite bounds, normalize-domain uncertainty, empty/unreached ownership, stale frame/view/cache, or exceptions must keep the draw or fall back to the full ordinary Flora draw.

Apply qualified IDs only in the existing source-order Flora loop, after its current camera clip rejection. The stage-08 adapter does this and checks sorted/unique membership; other Flora draws, source order, shaders, uploads, and shadow callbacks remain on their existing path. Keep the `observe` mode as the default diagnostic; only the explicit `on` mode reaches the mask path.

## Cost and evidence limits

The prepared portal graph currently takes about 127–130 ms to initialize once per generation. The stage-07 frame observer measured 0.73–1.18 ms for a complete frame graph/owner pass; its repeated 5,316-row source join took about 1.1–1.2 ms. The standalone wind-envelope helper costs 1.27–1.30 ms for 5,316 compute-only rows; actual stage-08 `mask_cpu_ms` is the measurement needed before deciding whether the repeated join or whole-level graph mask needs optimization. These are diagnostic host CPU costs, not an FPS result. Indexed rest accumulation is a separate roughly 1.0 s one-time pass over 4.1 million unique indexed vertices if not amortized in adapter preparation.

Frame-700 GPU oracles passed all 12 readback checks in both tested poses. The indoor pose had 4,055 actual post-camera-clip skips and a complete 5,316-ID observer receipt. The outdoor pixel pair had only 8 actual skips; its observer receipt covers 5,308 IDs and is explicitly incomplete. These results establish only those two captured poses, not general view safety or performance value. No production admission follows from this draft.
