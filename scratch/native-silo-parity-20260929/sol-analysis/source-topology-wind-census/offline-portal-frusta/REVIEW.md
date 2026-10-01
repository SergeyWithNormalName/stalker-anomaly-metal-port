# Independent portal and envelope review

## Findings

The original candidate count of 4,427 was produced before the exact C++ condition was parsed correctly. It was superseded. The corrected live expression in `src/Layers/xrRender/r__sector.cpp` is exactly:

```cpp
DM.i_options & CDSGraphManager::VQ_FADE | CDSGraphManager::VQ_SSA && psDeviceFlags.test(rsDrawPortals)
```

Since bitwise `&` has higher precedence than bitwise `|`, and bitwise `|` has higher precedence than logical `&&`, C++ parses the condition as `((DM.i_options & VQ_FADE) | VQ_SSA) && rsDrawPortals`. `VQ_SSA` is the unmasked enum constant in the actual source. With the captured run's `rs_render_portals off`, options 6/7 take the `else` path and run the SSA checks. Source then skips when `ssa < r_ssaDISCARD`; with `VQ_FADE`, it also calls `fade_portal` below `r_ssaLOD_A` and stops traversal below `r_ssaLOD_B`. This corrected path is used by `portal_frusta_roi.py` and the 4,427 OGF candidate result was recomputed.

The capture's `source-topology.json` says traversal options 7, but its live sector/visual vectors are empty on Metal. The offline traversal explicitly removes the HOM bit (options 6) as requested. The graph/frusta set is therefore a conservative no-HOM expansion of the captured options-7 configuration, not an observed Windows graph. The source file hash is recorded in the result JSON and checked at run time.

The corrected portal walk preserves the source's per-sector visited portal set and appends every reached sector frustum. `CPortal::Setup` builds its source-side plane by averaging normalized fan-triangle normals and its sphere from the polygon AABB; neighbor selection uses that plane's camera classification. The created portal frustum independently recomputes the clipped polygon plane, reverses the polygon if the camera classifies on the negative side, builds camera-to-edge side planes, then appends the oriented portal near plane and the full-transform far plane. The mirror follows this orientation and keeps the source front/back rule.

The source near-camera dual-render selection branch is implemented, but no portal-sphere center contained this eye in the run, so the branch was unused. `viewport_near=0.2` agrees in the captured run's saved `user.ltx` and launch log. The frame packet itself does not record this field, so exact live near-plane provenance is not claimed; it could not affect this particular portal walk because the branch predicate was never reached.

Source `CreateFrustumFromPortal` simplifies clipped polygons above six vertices. This frame produced six clipped quads and one pentagon, so the simplification path did not run. For a later input that does exceed six vertices, the mirror intentionally falls back to the incoming parent frustum, which broadens reachability rather than rejecting geometry on an unmirrored approximation.

`CFrustum::testSAABB` checks the source sphere first and then the negative AABB support point for each still-active plane. `sphere_and_aabb_hits` uses that structure with a small outward guard. The wind qualification is broader: it checks a sphere centered at the exact source OGF sphere center and radius `max(source radius, indexed rest max-distance) + bounded SSFX displacement`. The mesh artifact visits all referenced unique indices after exact level-geometry buffer/matrix validation. Source mesh and shader motion envelopes therefore have a concrete per-ID bound; the 15 possible-zero-normalize cases are not admitted and fail open.

The corrected per-ID partition is 4,086 bounded candidates still outside every retained owner-sector frustum, 330 raw exclusions whose bounded motion envelope intersects a retained frustum, and 11 raw exclusions with unknown motion bound. Four additional unknowns were already raw-visible. All 5,316 IDs reconcile uniquely to owner sectors 3 or 30.

## Remaining limits

The result still does not include static ancestor `add_Static`/`add_Static_MultiFrustum` full-visible shortcuts, source HOM portal/static tests, `MT_LOD` behavior, per-visual shader/optimization exceptions, or other source draw gates. It reports a fixed-frame geometry candidate screen only. No suppression, performance, or pixel-equivalence claim follows.

A prior review file, `REVIEW_WIND_AGENT.md`, correctly identified that the old 4,427 run was blocked but quoted the expression with an extra `DM.i_options &` before `VQ_SSA`. The exact live source/hash above is authoritative; this review and the corrected run use the unmasked `VQ_SSA` constant.
