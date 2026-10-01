# Flora shadow pipeline order: measured structure, weak initial economics

The observe-only candidate `d5cdf01b95f62e70e95b305dd26637e7a82a3d7e3ab4ea3846d72790c45a6db9` completed the pinned Cordon run. Census frame 360 has exactly one completed E01 acquisition (serial 360) and one measured-phase device row. All 288 production shadow views were recorded without truncation: three sun cascades and 285 local-light faces. Rendering and draw ordering were unchanged.

| Count on frame 360 | Current order | Hypothetical exact-pipeline partition |
|---|---:|---:|
| Flora shadow PSO requests | 2120 | 454 |
| Flora shadow VB requests | 3432 | 3432 |
| Flora shadow draws | 3432 | 3432 |

This could avoid 1666 PSO requests, 11.34% of the complete frame's 14693 requests. It does not remove any MSC table binding, upload, indexed draw, caster, light or shadow face. The separate perturbed CPU sample attributed only 8/2264 inclusive main-thread stack samples to pipeline binding under Flora shadow drawing. Request counts are not time savings, and this makes the isolated order candidate a weak initial FPS target.

No ordering patch was applied or qualified. The independent default-off draft remains archived; no performance improvement or visual/motion acceptance is claimed. The three observer source files were restored exactly after capture. Production bundle and executable remain the saved original `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`.

Next bounded candidate is stronger structurally: the static shadow tile loop binds a pipeline before every run even when its exact pipeline is unchanged from the immediately preceding run. A cache confined to one loop invocation preserves all draw ordering and resets before other caster families or a new tile can change state. This is a hypothesis to count and test, not an accepted optimization.

Raw counts and completed-frame admission are in `runs/flora-census-01/native/output/flora-census-summary.json`. CUA inspected the window only after the measurement phase ended; its image is a perspective thumbnail rather than a full-resolution viewport, so it does not close visual or motion acceptance.
