# Active-occlusion coverage foundation limits

This is a scratch-only CPU observation foundation. It does not connect to the
renderer, cull work, change pixels, or establish a performance result.

The helper accepts post-WVP, post-TAA-jitter clip vertices. Callers must supply
an upward-safe bound for each WVP row's sum of absolute products and an absolute
error bound for CPU/GPU WVP differences, jitter operations, and other upstream
arithmetic. The implementation adds a binary32 dot-product error bound and an
absolute `8 * FLT_MIN` allowance for the bounded four-product/three-sum dot.
Bounds that cannot be represented safely must make the primitive/query
uncertain. Compile without fast-math or finite-math-only; the CPU oracle uses
`-ffp-contract=off`.

The 64x64 grid requires an explicit clip-to-viewport Y direction, pixel-area to
front-winding relation, cull mode, depth compare, opaque/depth-write state, and
guaranteed stencil pass. Unknown state contributes no occlusion. Clip W and
normal depth (0..1) near/far crossings are skipped instead of clipped. A
positive `rasterEdgeGuardPixels` is mandatory; it is an explicit empirical
assumption, not a bound guaranteed by Apple documentation. Native Metal
orientation, coverage, interpolation, and raster-boundary equivalence remain
unqualified and require a later native probe.

Each grid is tied to one nonzero acquisition/view/projection/jitter scope ID.
Triangle input is capped at 32768 and primitive-cell tests at 262144. Exceeding
either cap retains only already certified occluder coverage; omitted geometry
cannot create additional positives. Query-cell work is capped at 4096; an
exhausted query is visible/unknown. Same-draw triangle pairing is optional and
requires reciprocal source-table references, exact shared original vertex IDs,
identical shared clip/error data, opposite sides of the shared edge, and a
certified convex projected union. Failed pairing leaves singles only; it never
fills a gap or uses a convex hull of unrelated geometry.

For each fully covered cell, depth stores the outward-rounded maximum projected
vertex depth of that triangle/quad plus its supplied numerical bound. When
multiple independent full-cell covers exist, the nearest such safe far bound
is retained. Queries project all eight corners of an already padded world AABB,
expand screen bounds by the numerical uncertainty and empirical edge guard, and
require every touched cell to be covered with strict `farDepth < nearestDepth`.
Any invalid input, scope mismatch, near/far/W uncertainty, or query budget
exhaustion returns visible/unknown.
