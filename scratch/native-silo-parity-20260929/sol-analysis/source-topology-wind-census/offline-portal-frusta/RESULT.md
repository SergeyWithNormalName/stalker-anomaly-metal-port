# Frame-700 portal-frustum wind-envelope ROI

## Corrected result

The corrected options-6 no-HOM traversal starts from sector 25 and reaches sectors 17, 21, 22, 24, 25, and 30. It retains three distinct frusta for sector 30. All 5,316 captured main-Flora IDs have a unique source-sector owner: 5,308 in sector 30 and 8 in sector 3.

Direct OGF sphere+AABB candidate testing against every frustum of each ID's owner sector reproduces 4,427 raw geometry exclusions (4,419 in sector 30 and 8 in sector 3). I then checked the exact indexed world-vertex bounds plus the pinned frame-700 SSFX wind/bender envelope for every captured ID. The 15 wind-envelope cases whose bender `normalize` domain could include zero were retained as unknown. The conservative envelope result is:

| Frame-700 candidate set | IDs |
|---|---:|
| Raw OGF candidates excluded from all no-HOM owner frusta | 4,427 |
| Still excluded after indexed-mesh and wind-envelope expansion | **4,086** |
| Reopened by a bounded wind envelope | 330 |
| Fail-open unknown among raw exclusions | 11 |
| Raw OGF candidates that intersect a no-HOM owner frustum | 889 |
| Unknown wind envelopes among those raw-visible IDs | 4 |

The partition reconciles: 4,086 + 330 + 11 = 4,427. All 15 unknowns fail open and are never counted as excluded. The bounded envelope is available for 5,301 of 5,316 IDs. Per-ID data is in `flora-portal-envelope-roi.csv`; its aggregate receipts are in `flora-portal-envelope-roi.summary.json`. The all-ID wind interval input/output is in `all-geometry-envelopes.csv` and `all-geometry-envelopes.summary.json`.

This is an offline geometry-candidate screen only. It does not establish actual Windows draw eligibility, visual correctness, a performance saving, or permission to suppress a draw. In particular, it omits HOM, source static hierarchy ancestor/full-visible shortcuts, LOD behavior, and other draw gates. A 4,086 exclusion count must not be applied to a runtime path.

## Portal traversal contract and source correction

The active `src/Layers/xrRender/r__sector.cpp` SHA-256 used for this screen is `bdb7fdbd7b3be3dc333a252744af8f0e0f6fc50e877db793640e64abf06823b5`. Its live expression is:

```cpp
if (DM.i_options & CDSGraphManager::VQ_FADE | CDSGraphManager::VQ_SSA && psDeviceFlags.test(rsDrawPortals))
```

C++ precedence groups this as `((DM.i_options & VQ_FADE) | VQ_SSA) && rsDrawPortals`; the source has no second `DM.i_options &` before `VQ_SSA`. With recorded `rs_render_portals off`, it enters the SSA branch. For options 6 and 7, SSA hard-discard is followed by the fade-mode `r_ssaLOD_B` recursion rejection; `r_ssaLOD_A` only queues fade-list work. The corrected walk observed one hard-discard rejection and one `r_ssaLOD_B` rejection. Fade-list `HOM.visible` is a side effect and does not control recursion. The previous 4,427 report used the wrong expression grouping; its number happened to match, but its method claim is superseded by this corrected run.

The capture metadata reports traversal options 7, while this explicitly requested offline pass uses options 6 (`VQ_SSA | VQ_FADE`, HOM disabled). Thus this is a no-HOM visibility superset, not the captured options-7 HOM result. The start sector is sector 25 from the unique downward CFORM hit; the active `level.cform` and previous start-sector receipt anchor that value independently of root-box containment.

The walk mirrors source portal sphere tests, front/back neighbor selection, the near-camera dual-render condition, polygon clipping, frustum construction, per-sector visited-portal sets, source SSA thresholds, and multiple frusta per sector. Current frame inputs yielded zero portal-sphere centers containing the eye, so the near-camera branch was unused. `viewport_near` is 0.2 in both the run's saved `user.ltx` and launch log, though the frame record itself does not contain that live field. Seven recursed portal polygons clipped to four or five vertices; the source `>6` polygon simplification branch was not exercised. The tool fails open to the incoming frustum if that branch is ever encountered, or if portal-plane/frustum construction is uncertain.

The direct source OGF candidate check follows `CFrustum::testSAABB`: normalized-plane sphere rejection first, then AABB support-point rejection when needed. The wind-expanded test uses a sphere that encloses every referenced indexed world vertex plus its analytical same-frame shader displacement. A candidate is called excluded only if that expanded sphere is outside every retained owner-sector frustum with a conservative plane guard and its displacement status is bounded.

## Frame and motion evidence

- Level file SHA-256: `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`.
- Exact Flora ID capture SHA-256: `dca66b5c94dffae895bc33ba77a4cdc57881cce7c5252b07ba823b2dd6bdcdbc`.
- Indexed world-mesh extrema SHA-256: `3d15d90548265cb9be3ec9b01c4e45ecfae4f240dada0a157346dee858f83605`.
- Frame-700 live wind JSON SHA-256: `7515eff2f59b25f5357fb3a02cf5e9f3b48acecb8a9dc4dd1607fe2fc8101643`; callback view is frame 700 with previous frame 699.
- The envelope uses exact indexed P3 world transforms, signed-S4/1024 H/UV extrema, the pinned `SSFX_WIND=1` leaf/trunk shader receipts, the normalized wind-wave texture receipt, and the current/previous captured bender arrays. It bounds all possible wave texture samples/phases at this captured state and adds outward float32 rounding.
- 15 leaf rows remain unknown because one or more current/previous bender `normalize` inputs can include zero within the evolving bound. Eleven are in the raw 4,427 exclusions; four were already raw-visible.

## Reproduction and limits

Run `./run_qualification.sh` from this directory. It uses Python/NumPy only and writes scratch artifacts here; no game, renderer, build, cook, or production source mutation is involved. The script verifies the pinned level, frame capture, mesh, wind fixture, shader/texture receipts, and live `r__sector.cpp` hash before counting.

Relevant code anchors: `src/Layers/xrRender/r__sector.cpp` (`CPortal::Setup`, `CSector::traverse`), `src/xrCDB/Frustum.cpp` (`CreateFromMatrix`, `ClipPoly`, `testSAABB`), `src/xrCDB/Frustum.h` (`testSphere_dirty`), `src/Layers/xrRender/r__dsgraph_build.cpp` (`add_Static` and multi-frustum traversal), and `src/Layers/xrRenderPC_R4/r4_loader.cpp` (`LoadSectors`). The single-frame wind bound is not a temporal/world guarantee and the no-HOM pass is not a source-visible runtime census.
