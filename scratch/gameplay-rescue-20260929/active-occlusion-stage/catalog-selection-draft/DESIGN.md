# Exact source catalog and current-view selection draft

This scratch-only stage addresses the V7 source-prefix failure. It does not
change `Scene`, `MetalBootstrap`, rendering, or culling. The existing 64x64
coverage foundation remains observe-only and receives only exact source
triangles selected from successful main-G-buffer draw receipts.

## V7 input and bounded storage

The exact V7 level parse reproduced the old first-file budget:

```
auto_admitted=2052 retained_visuals=109 retained_triangles=32768
omitted_visuals=1851 omitted_triangles=521609 excluded_materials=92
excluded_trees=0 invalid_visuals=0
```

Thus the eligible default-material source has 554,377 triangles in 1,960 visual
ranges (109 retained plus 1,851 omitted); the separate 92 materials are
excluded. The selector utility reported 2,052 automatic admissions, all with
one bitwise-identical 64-byte model matrix and 2,051 repeats; that grouping
includes the excluded materials. The parser/input hashes and output are
preserved in `../tools/out/model-groups/`.

`SourceTriangle` is a 60-byte exact record: visual ID, local triangle ID,
reciprocal optional mate ID, three original source vertex IDs, and the three
original local Float3 positions. At 554,377 triangles plus 2,052 12-byte visual
ranges, its calculated logical catalog payload is 33,286,140 bytes (31.744
MiB), assuming exact vector capacities. Sizing with all 2,052 automatic
admissions as ranges would be a conservative upper bound of 33,287,244 bytes.
This is a count-based estimate; no retained catalog has been constructed yet.
Both estimates are below the 64 MiB cap, without compression or an index
indirection layer.
The catalog validates all range boundaries, finite positions, source UInt16
vertex-ID extent, and reciprocal within-visual mate links before accepting the
whole catalog.

The production producer should count eligible triangles/ranges first, perform
checked byte arithmetic, reserve the exact vector capacities, and then copy
into one catalog before the level-upload path releases its transient geometry
bytes. `Catalog::Build` checks actual vector capacities, not only logical
sizes. If allocation fails or either capacity exceeds the limit, discard the
entire candidate catalog and leave the observer unavailable for that level;
never keep a file-order prefix. Do not keep the 79 MiB source `level.geom` as
the retained representation. The existing upload commit clears transient
`geometryBytes` after successful upload; the catalog must independently own
only the bounded copied records and reset with its `LevelScene`.

## Current-view subset

Use only the backend's existing immutable draw receipts emitted after a
successful ordinary main G-buffer indexed draw. A receipt must match the
current nonzero observation scope, engine frame, viewport, visual range, and
the already-qualified opaque/depth/stencil/front-face state. No engine-frame
guess, source-file admission alone, shadow draw, mask draw, or failed draw can
introduce a candidate. Receipt overflow, catalog loss, or allocation failure
means no observer result for that frame.

For each valid receipt, project its existing local AABB through the exact
uploaded WVP and jitter and compute its clamped viewport rectangle and nearest
normal-depth value. The AABB supplies only a ranking score; it is never
expanded into coverage. Nonfinite transform/bounds inputs or empty viewport
intersection exclude that draw from the budget. A fully behind-camera bound or
wholly out-of-range normal-depth bound may be omitted.
If a finite AABB crosses W<=0, rank it at full viewport area and depth zero;
if it crosses near/far depth, clamp its ranking depth to zero. These choices
keep potentially useful room geometry from disappearing because a coarse box
straddles a clip plane. They only spend selection budget: the downstream
coverage builder still subjects every original triangle to strict W, depth,
uncertainty, winding, and cell-coverage checks. Rank by
projected rectangle area per exact source triangle, then nearer depth, then
stable draw ID and visual ID. This view-specific, deterministic heuristic
contains no level IDs or authored-name special cases. A loose bound can waste
budget, but it cannot create coverage because the downstream builder receives
the original selected triangles only.

The V7 offline ranking provides a concrete reason to retain this behavior:
wallpaper visual 22052 (78 triangles) and stucco visual 22051 (48 triangles)
have local bounds spanning the frame-300 camera Z, while other nearby room
surfaces are small (brick 22061: 8 triangles; concrete 22062: 26; metal 22066:
24). An all-corners-positive rule would drop those coarse bounds. These IDs
motivate the generic crossing-bound fixture only; runtime selection has no
level-specific IDs or material-name priority.

Select whole visuals only, initially to 4,096 triangles (with 32,768 remaining
the hard safety ceiling). If a candidate does not fit, omit it and consider the
next ranked draw; never truncate a visual or synthesize a hull. Mate IDs remain
local to their original visual and the coverage foundation already requires
the reciprocal same-visual pair before using a two-triangle union. Omissions
can only reduce coverage. Exact tie behavior and failed/ambiguous receipt
handling are exercised by `tests/R4ActiveOcclusionCatalogSelectionProbe.cpp`.

## Integration and measurement gate

At level admission, create the catalog from the same automatic-material,
non-tree, ABI-qualified immutable ranges the current source collector uses.
At frame build, pass the existing after-success receipts to
`SelectCurrentView`, then append each selected draw's catalog range in its
original order to the existing coverage input. Keep existing scope, matrix,
jitter, transform uncertainty, winding/cull, raster guard, and fixed cell-test
limits unchanged. Selection does not modify draw order, resources, or pixels.

For one later bounded observe-only run, report catalog state/bytes, draw
receipts, selected and omitted triangles/visuals, transform and cell-test
counts, cells marked, complete/incomplete groups, and build time. The existing
V7 inputs (23,733 receipt triangles; 9,605 cell tests; zero cells marked) show
that a 4,096-triangle selection would sharply bound work, but do not establish
that useful cells or group counts will survive. Do not claim saved CPU/GPU time
or enable suppression from this stage. If the selected subset remains
unproductive, stop this CPU-grid route and compare a separately reviewed
same-frame GPU prequery design.

## Validation boundary

The focused CPU probe covers exact range/pair retention, all-or-none budget
failure, successful/current receipt filtering, deterministic ranking, whole
draw budget behavior, projection uncertainty, and duplicate attribution. Build
it as strict floating-point code (`-O2 -fno-fast-math -ffp-contract=off`). It
does not prove Metal raster conservatism, runtime admission wiring, visual
correctness, or performance. The following standalone command is suitable for
runtime-owned validation after review:

```
clang++ -std=c++17 -O2 -fno-fast-math -ffp-contract=off \
  -I scratch/gameplay-rescue-20260929/active-occlusion-stage/catalog-selection-draft \
  scratch/gameplay-rescue-20260929/active-occlusion-stage/catalog-selection-draft/R4ActiveOcclusionCatalogSelection.cpp \
  scratch/gameplay-rescue-20260929/active-occlusion-stage/catalog-selection-draft/tests/R4ActiveOcclusionCatalogSelectionProbe.cpp \
  -o /tmp/R4ActiveOcclusionCatalogSelectionProbe
/tmp/R4ActiveOcclusionCatalogSelectionProbe
```
