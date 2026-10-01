# Prepared-motion experiment result

The narrow hoist is behavior-equivalent on the current real and synthetic corpus, and it measurably reduces helper-only CPU time. This supports considering the API in the per-frame Flora loop; it does not establish a full-mask or in-game saving.

With Apple clang 21, `-O2 -std=c++17 -Wall -Wextra -Wpedantic`, old and prepared helpers returned bit-identical `known`, reason strings, radii, displacements, and bender values for all 5,303 bounded rows from `real-rest-bounds.csv`, under both `frame700-motion.csv` and `frame700-motion-active.csv`. The remaining 13 rows were already `rest_status != bounded`, so both callers leave them fail-open without entering the helper.

All 23 synthetic comparison cases passed. They cover leaf/trunk paths, current and previous bender displacement, zero-normalize and zero-denominator guards, negative-denominator behavior, invalid-motion versus invalid-rest precedence, each nonfinite Motion-array failure reason, reason ordering, extreme H inputs, and leaf/adjusted-radius overflow. The bender loop and its guards are unchanged in the draft. `negative_wind_speed` and `trunk_wind_bound_unknown` remain defensive helper outcomes that this valid finite fixture/synthetic input set did not trigger; the source checks remain in the proposed overload.

The active-motion helper-only median dropped from 1.2329 ms to 0.8050 ms for 5,303 bounded Rest rows (34.7%). For a repeated two-copy corpus of 10,606 rows, it dropped from 2.3784 ms to 1.6469 ms (30.8%). The prepared snapshot is built once outside those timed loops. This measures repeated helper calls only; source-level full join, frame preparation, rendering, and GPU work are excluded. The 10,606 rows are a repeated fixture, not unique game data. No FPS or full-frame claim is made.

The implementation is a candidate for a small source refactor: construct `PreparedMotion` once after same-frame Motion validation, then call the prepared overload in the existing visual loop. Preserve the current `compute(Rest, Motion)` overload as compatibility. Before any engine integration, root should review this isolated patch and then validate the full per-frame mask cost; this result does not authorize culling or game integration.

The exact helper/probe output is `equivalence-run.txt`. No engine build or game launch occurred in this experiment.
