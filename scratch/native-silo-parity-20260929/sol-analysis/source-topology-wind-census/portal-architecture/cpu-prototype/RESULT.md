# Frame-700 CPU topology/walker result

## Result

The new parser/walker reads the pinned active level `l01_escape`, takes externally validated start sector 25, and reproduces the corrected HOM-off (`options=6`) reached graph:

- sectors 17, 21, 22, 24, and 25 each retain one frustum;
- sector 30 retains three frusta;
- 8 sector-frusta total, 7 recursive portal crossings, 361 portal checks, 338 portal-sphere rejects, 1 SSA hard-discard, 5 per-sector already-visited portal skips;
- 5,316 captured Flora IDs preserve source-root ownership (5,308 in sector 30; 8 in sector 3);
- source OGF sphere + AABB checks classify 4,427 IDs outside all owner-sector frusta and 889 as intersecting at least one. The per-ID results match all 5,316 `raw_ogf_bounds_intersect_any_frustum` flags in the prior wind-envelope screen.

This is the *raw static OGF bounds* candidate count. The prior separate mesh+frame-wind envelope screen reopens 341 of those candidates (330 bounded motion, 11 unknown fail-open), leaving 4,086 static+wind candidate exclusions. This prototype does not recompute or admit that wind result and does not make temporal claims.

`frame700-per-id.csv` reports each visual ID, material, all owning sector IDs, number of retained owner-sector frusta tested, `visible_any`, and reason. `frame700-per-id.summary.json` pins input hashes, graph counts, and qualification. The hard result check refuses mismatched active `level`, `level.geom`, frame, or Flora ID receipts.

## Source and arithmetic contract

Input evidence and source anchors:

- `level` SHA-256 `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`; `level.geom` receipt `a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a`; ID capture SHA-256 `dca66b5c94dffae895bc33ba77a4cdc57881cce7c5252b07ba823b2dd6bdcdbc`.
- Active source uses `b_portal` and sector chunks in `src/Layers/xrRenderPC_R4/r4_loader.cpp:357-405`; `CSector::load` and portal setup/traversal are in `src/Layers/xrRender/r__sector.cpp:101-284`; source `CreateFromMatrix`, `ClipPoly`, and `testSAABB` are in `src/xrCDB/Frustum.cpp:92-177,329-390,410-468`.
- `CPortal::Setup` accumulates normalized fan normals in float, divides by the valid fan count, then `Fplane::build(point, normal)` normalizes the averaged normal (`src/xrCore/_plane.h:41-44`; `src/xrCore/_vector3d.h:435-444,556-569`). The parser rounds each vector multiply/add/divide to float32 and uses the normal `Fvector::normalize` route here; portal-frustum side planes use `build_precise` semantics.
- SSA options use the corrected live C++ grouping `((options & VQ_FADE) | VQ_SSA) && rsDrawPortals`; captured config and engine log agree on `rs_render_portals off`, `viewport_near=0.2`, and `r2_ssa_lod_b=48`, while frame metadata reports options 7. The explicit walk uses options 6, a no-HOM visibility superset. It computes LOD_B as source `_sqr(r2_ssa_lod_b / 3.f) / g_fSCREEN` with float32 stores.

The old offline Python counter reported one `ssa_lod_reject`; source tracing found that portal 36 (SSA `4.8313813e-6`) had already failed `r_ssaDISCARD`. The old mirror held a reject flag and still counted LOD_B before skipping. Live `CSector::traverse` continues immediately at the SSA discard (`r__sector.cpp:205-209`), so this source-faithful walker reports one discard and zero LOD_B discards. The reached sectors and per-ID candidate flags are unchanged. This correction is control-flow accounting, not a float32-threshold result.

## Fail-open tests and limits

`test_portal_graph.py` verifies: malformed portal references disable exclusions; linked-visual cycles invalidate topology; a cyclic two-path portal graph terminates while preserving two separate frusta for the shared sector and per-sector visited sets; and a missing owner never excludes a visual. Run status for all five tests: passed, including nonfinite primary-camera input.

The walker widens to the incoming frustum if source clip classification is within a conservative guard, portal plane math is degenerate, or a clipped portal exceeds six vertices (the source `SimplifyPoly_AABB` route is not reimplemented). A malformed topology makes every query fail open. It keeps multiple owners and frusta; it does not assume a single sector per visual.

The source walker uses `Device.vCameraPosition_saved` for the near-camera dual-render decision, distinct from `DM.i_vBase`. This captured fixture stores only the latter eye. The prior saved-frame screen found no portal-sphere center within that same eye, but the prototype records this as a limitation, not live proof; the new input observer should pin `saved_camera_eye_bits` and live `ViewportNear` before any new admission work. The start sector is an external CFORM receipt; portal-CDB nearest-ray ordering and CFORM lifetime are out of scope.

There is no render suppression, CDB ownership, root visual GPU creation, engine integration, build, cook, or game run. The result is a bounded CPU candidate census only. It does not qualify wind over time, HOM, `add_Static` ancestor shortcuts, source LOD, dynamic objects, lights, shadows, HUD, pixels, or shipping behavior.
