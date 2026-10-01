# Authoritative active-geometry occlusion: bounded observe-only outline

Read-only source design, 2026-09-29. No implementation or runtime claim. The concrete gap remains main static rendering and whole-light work without screen occlusion; the inside-house gameplay camera can still plan roughly 23 batches / 285 shadow records. Archive HOM assets are not an authority for active mod geometry.

## Source authority and lifecycle

`MetalBootstrap.mm` owns the actually loaded `R4LevelGeometry` metadata and `geometryBytes`. Each admitted `TextureMaterial` has exact visual/shader identity, vertex/index spans, model transform and local bounds. The GPU upload consumes the same geometry buffers. Raw CPU geometryBytes are cleared and shrunk at the end of upload (`:3046`), so an occluder extractor belongs in Prepare after material/pipeline admission and before that clear. It must use the XRFS-winning bytes already loaded for rendering, with the same local indices/base vertex/model transform; neither base archive level.hom nor collision CDB is sufficient by itself.

First occluder family: only admitted static opaque LMH/default mesh surfaces whose receipted vertex and fragment permutations prove unchanged positions and opaque coverage. Exclude alpha-tested, transparent, emissive/forward, water, terrain/displacement, wind/tree/flora/grass, dynamic doors/objects and every uncertain shader. Ordinary default bump VS in the current receipt writes mul(m_WVP,I.P); material-family names alone still need permutation/coverage qualification. Preserve exact triangles and holes: no bounding-box occluders, convex-hull filling, centimeter vertex packing or mesh simplification which adds coverage.

Cache compact world triangles and a spatial broad phase once per level. Generated cache key includes hashes of active winning geometry/level payloads, exact admitted shader permutations/coverage classification, model transforms, renderer version and algorithm version. Missing/stale/unsupported state falls open and regenerates from installed data. A small bounded appdata cache replaces no mod files and follows ordinary upstream GAMMA updates. An initial in-memory observe-only version needs no disk cache at all.

## What current HOM reuse would require

`CHOM` has no public in-memory mesh admission. Load(false) still consumes level.hom and uses `CDB::Collector::add_face_packed_D(...,0.01)`, so it is not an authoritative-geometry loader. RenderDiagnostic already provides exact-current-view synchronous rendering without legacy scheduler/RNG and its projection/test guards are reusable.

The legacy Raster is only 64x64. Both Raster and raster-core scratch are global, forbidding concurrent owners or second viewport reuse without a rebuild. More seriously, `occRasterizer::propagade()` fills 1–2 scanline holes between shared/adjacent triangles and core scanline connectors also extend coverage. Simply supplying exact installed triangles does NOT prove windows/holes remain open. A qualified conservative mode must prevent adding coverage across holes and bias depth away from the camera. Fresh in-memory source also must avoid collector packing which can move vertices and close small gaps.

Lowest-risk first reuse is observe-only, with no admission changes, comparing legacy diagnostic output against a conservative coverage mode or independent actual rendered-depth oracle. Production requires a conservative raster certificate, not only a matching geometry hash. Full-cell coverage must be proven by triangle coverage, unresolved edge/seam cells remain visible; the resulting false-visible rate is itself an economic screen. A broad coplanar merge which fills holes is excluded. If a strict mode is too leaky or too expensive, stop and review a different method rather than quietly using legacy hole-fill heuristics.

## One bounded screen

Build a capped set of exact large opaque triangles, spatially selected for the current camera. Omitting small/uncertain triangles is conservative: it can miss opportunities but cannot create false hidden geometry. Use independent view/sample identity for every render, second viewport and retry. Cap raster candidates and CPU interval; budget exhaustion leaves all remaining objects/lights visible.

Render everything as today, recording admitted/rejected occluder families and source hashes; extraction/cache bytes and one-time load cost; candidates/triangles rasterized, covered/uncertain raster cells, query cost; main static material/draw candidates marked hidden; whole parents and faces marked hidden; complete shadow views, post-merge static encoded draws and dynamic/flora/definition work associated with hidden records. Reuse existing whole-light TILE/BATCH timing receipts, preserving inclusive/mixed-batch attribution rules. Do not label light-count fractions as saved CPU time.

The screen uses the same installed save/config/camera as current gameplay evidence and a short warmup/window. Economic result is known removable preparation/encoding minus conservative raster/query overhead, with GPU work counted separately. Continue only if meaningful hidden work exists across ordinary gameplay views and the cost does not consume it. A raster which merely marks many objects hidden without surface/coverage qualification is not evidence of a safe speedup.

## Correctness gates before suppression

- Qualify opacity/deformation from actual receipted shaders and exact installed triangle spans; valid load/hash alone is insufficient.
- Independent GPU oracle against production G-buffer depth and world positions for the same immutable camera/state: no test box classified hidden if it produces actual pixels. Same-state dual target handles alpha/holes and confirms depth bias.
- Camera motion around doors/windows, near/camera plane and occluder edges, inside/outside house and through second viewport; all uncertain or stale states remain visible.
- Whole-light parent/face volume includes every affected accumulator/volumetric pass; HUD and volumetric exemptions remain until separately qualified.
- Main-camera hidden static materials only affect main-camera draws. Off-camera static and dynamic shadow casters keep their light/cascade visibility path unchanged.
- Ordinary same-binary A/B after detailed diagnostics/capture are off; report actual CPU/GPU/pacing and active scene counters, preserve mod/update compatibility.
