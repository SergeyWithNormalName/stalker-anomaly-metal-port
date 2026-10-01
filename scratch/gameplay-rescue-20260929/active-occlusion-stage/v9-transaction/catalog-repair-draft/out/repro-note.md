# V9 catalog validation failure reproduction

This is an offline CPU-only replay against the exact installed level directory copied into the V9 run. The level geom/geomx/cform SHA-256 values match the V7 installed input. The diagnostic uses `LoadR4LevelGeometryWithBytes`, the same LMH/default source eligibility and byte/index/position reads as the active producer, copies the producer edge-mate collector loop, then invokes the actual `active_occlusion_catalog::Catalog::Build` validator. It does not load/run the game or use the GPU.

Before normalization, the replay reproduces candidate cardinality 2,052 automatic admissions, 92 excluded non-default materials, 1,960 candidate visuals / 554,377 triangles and zero invalid visuals. It reports `Catalog::Build=InvalidTriangle`; first offending record is visual 0, local triangle 3, `mate=13`, with triangle 13 pointing to 14 rather than 3. There are 187,365 nonreciprocal final mate-hint fields. Reserved catalog payload is 24,624 range bytes + 33,262,620 triangle bytes, below the 64 MiB limit.

The scratch repair clears only mate-hint fields which are self, out of range, or nonreciprocal after the producer has finished assigning them. It does not change source triangle order, IDs, vertex indices, or positions and does not invent new pairs. The same replay then reports 187,365 cleared fields, zero remaining reciprocity failures, and `Catalog::Build=Ready`.

Evidence logs: `repro-before.log`, `repro-after.log`. Exact command lines and build flags: `commands.txt`. Hashes: `source-input-sha256.txt`.

The V9 run itself remains inconclusive for grid usefulness: preparation had disabled before the grid stage; the 12 grids were skipped and the 516 CPU face rows were `unknown/static-source-not-enabled`. Raw GPU counts are separately summarized by the parent and cannot be correlated to CPU prediction at scope 0.
