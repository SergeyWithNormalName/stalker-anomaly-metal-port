# Independent review: offline portal/frustum ROI

## Verdict

**The first traversal was wrong, but the author has corrected it.** The corrected options-6/offline-no-HOM run still reports 4,427 raw per-visual OGF exclusions, while reaching a different graph: sector 20 is no longer reached, and the walk records one portal SSA discard and one `r_ssaLOD_B` reject. Treat 4,427 as a per-frame geometry-candidate count only; it is not an actual source draw exclusion or admission result because source ancestor shortcuts are still omitted.

This review is read-only except for this report. I inspected `portal_frusta_roi.py`, its `RESULT.md`, `r__sector.cpp`, `r__sector.h`, `Frustum.cpp/.h`, `r__dsgraph_build.cpp`, and the saved frame fixture. I also ran the existing script once under a temporary in-memory wrapper solely to count clipped polygon sizes; no source or build files were changed.

## Original blocking defect and correction: options-6 operator precedence

The original script comment and original `RESULT.md` parsed the source expression as:

```cpp
(options & VQ_FADE) | ((options & VQ_SSA) && rsDrawPortals)
```

That grouping was wrong. My first review corrected the grouping but also mistakenly added a second `options &` around the source's unmasked `VQ_SSA` constant. The live expression is exactly:

```cpp
DM.i_options & CDSGraphManager::VQ_FADE | CDSGraphManager::VQ_SSA && psDeviceFlags.test(rsDrawPortals)
```

Bitwise `&` and `|` bind more tightly than logical `&&`, and the second `VQ_SSA` is an unmasked constant. The exact grouping is therefore:

```cpp
((DM.i_options & CDSGraphManager::VQ_FADE) | CDSGraphManager::VQ_SSA) && psDeviceFlags.test(rsDrawPortals)
```

With explicit options 6 and `rsDrawPortals off`, the condition is false. Source enters the `else`, tests the options-masked `VQ_SSA`, and computes portal SSA; it rejects below `r_ssaDISCARD`, fades below `r_ssaLOD_A`, and rejects below `r_ssaLOD_B` when fade is enabled. The author's corrected `portal_frusta_roi.py` now spells out the actual source expression and gates with the saved portals setting. The original method should remain documented as a rejected first pass, not as the current result.

The frame fixture records `traversal_options: 7`, while the offline run explicitly selects options 6 as an HOM-off visibility superset. The updated script and envelope summary expose both values rather than conflating them. This is acceptable for the stated candidate screen as long as results remain described as options-6/offline no-HOM analysis, not the captured options-7 source walk. The renderer's live `r__sector.cpp` SHA-256 was verified as `bdb7fdbd7b3be3dc333a252744af8f0e0f6fc50e877db793640e64abf06823b5` during this correction. At review time, `RESULT.md` still contained the rejected first-pass parse and sector totals; use the updated script plus `flora-portal-envelope-roi.summary.json` until the author refreshes that report.

## Parts that match source

- The per-sector visited set is modeled in the right place: `DM.m_sector_frustums` stores a `FixedSet<CPortal*>` per current sector, and source inserts the portal in the current sector only after clipping/HOM passes, immediately before recursion (`r__sector.cpp:117,160-162,228-257`). Python keeps `visited[sid]`, marks after clipping, and recurses (`portal_frusta_roi.py:321-377`).
- For the near-portal dual-render branch, the source tests portal-sphere-center distance `< S.R` plus absolute plane distance `<= VIEWPORT_NEAR + EPS_L`, then calls `getSector(this)` (`r__sector.cpp:145-146,178-180`). The script uses the equivalent front/back owner switch when near (`portal_frusta_roi.py:343-349`; `r__sector.h:28`). Away from that case, its `getSectorBack` side selection matches `r__sector.h:36-40`.
- The captured initial frustum plane equations and row-major far-plane equation match `CreateFromMatrix` / `CreateFrustumFromPortal`; both normalize plane normals before sphere tests (`Frustum.cpp:410-485`, `r__sector.cpp:74-98`). The script's frame matrix composition check is a useful consistency gate.
- Leaf sphere+AABB rejection has the same structural order as `CFrustum::testSAABB` and `AABB_OverlapPlane`: sphere plane reject first, then the AABB negative support point only when the sphere intersects that plane (`Frustum.cpp:135-158`, `Frustum.h:46-58`; Python `sphere_and_aabb_hits`). Its boundary widening generally biases toward retaining a candidate.

## Limits that are stated correctly, with one caveat

The report correctly labels 4,427 as raw per-leaf geometry candidates, not actual draw eligibility, and notes that it does not reproduce HOM, ancestor hierarchy tests, LOD behavior, or `add_leafs_Static` shortcuts. This matters: source `add_Static` can mark a hierarchy fully visible and call `add_leafs_Static`, bypassing descendant frustum tests (`r__dsgraph_build.cpp:1297-1360`). A direct leaf OGF test is therefore not a source exclusion count even after portal traversal is corrected. Keep that caveat prominent; do not turn these candidate rows into a suppression list.

The corrected frame-700 walk reports clipped portal polygons of sizes 4 and 5 only (6 and 1 respectively), so source's `CreateFrustumFromPortal` `>6` simplification branch was not exercised in this run. The Python `from_portal` path does omit that source simplification, so its general “mirrors source” claim is not valid for any traversal that clips a portal to more than six vertices (`r__sector.cpp:78-81`; `Frustum.cpp` `SimplifyPoly_AABB`). This did not affect the corrected observed recursions.

The corrected script now requires the saved user setting and same-run launch log to agree on `viewport_near=0.2` and uses that value (`portal_frusta_roi.py:470-475`). The frame fixture does not record live `Device.ViewportNear` itself (`device.h:19,106`; `r__sector.cpp:146`), so this remains same-run configuration evidence rather than a value joined directly to frame 700. It is a residual provenance limit, not evidence of a branch mismatch in the bounded run.

## Review recommendation

Use the corrected explicit options-6/offline-no-HOM traversal for bounded candidate analysis, and retain the distinction from captured options 7. Keep the no-HOM and no-hierarchy limitations attached to any result. A source-equivalent exclusion claim still requires reproducing ancestor `add_Static`/`add_leafs_Static` behavior and joining live `ViewportNear` to the frame.

## Post-repair envelope result

The author's corrected traversal retained the same raw OGF count (4,427), but this numerical equality does not validate the earlier graph: the corrected pass reaches sectors 17, 21, 22, 24, 25, and 30, with three frusta for sector 30, and records `ssa_discard_reject=1` and `ssa_lod_reject=1`; sector 20 is dropped. The corrected all-indexed-geometry wind-envelope summary reports **4,086** candidates still excluded after conservative per-frame motion bounds, with **341** of the raw exclusions reopened by wind or unknown bounds and **15** unknown visuals. Those 4,086 are conservative frame-700 geometry candidates against the corrected offline no-HOM owner-sector frusta, not source draw exclusions.

This distinction is decisive: `r__dsgraph_build.cpp:add_Static` can mark a hierarchy fully visible and use `add_leafs_Static`, bypassing descendant frustum checks. Because the envelope output still does not reproduce those ancestor/full-visible shortcuts, do not call its 4,086 rows safe source omissions and do not use them for suppression or performance claims. It is suitable as a bounded per-frame ROI hypothesis for the next source-equivalent comparison. The summary itself also records the options-6 versus captured-options-7 relationship, the geometry/wind provenance, and these limitations.
