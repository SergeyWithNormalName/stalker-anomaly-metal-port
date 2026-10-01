# Immutable source certificate: bounded CPU result

This scratch-only probe tests whether `CheckR4ComputeSkinBounds` can reuse source-only facts for one immutable canonical vertex stream. The original production routine remains untouched. The candidate privately owns the canonical vertex vector and caches only source basis/weight validation, source bounds, source radius, maximum basis component, and maximum absolute weight sum. It retains per-call bone-matrix validation, every bone-index check, all skinned/world position math, finite checks, radius checks, and the existing fallback reasons. A vector or influence-count mismatch uses the original routine.

The certificate loop preserves the original `#pragma clang fp contract(off)` scope and explicit `std::fma` transform sequence. The probe compares statuses, failure strings, and output bound float bits.

## Evidence

- Optimized CPU-only scratch build succeeded with `-O2 -DNDEBUG`; it linked existing libraries but did not rebuild the app/engine target.
- Existing 512 randomized bounds cases and guard cases passed bitwise/status/reason parity, including a changed-vector ownership mismatch that falls back to the original routine.
- The installed fixture corpus passed for 30 admitted leaves / 82,530 vertices; one fixture remained skipped by the existing admission path. The bounds from both implementations matched each other and the existing CPU-repack position bounds.
- The fixture timing used deterministic synthetic bone poses, not captured gameplay poses. Build+copy+qualification of the 30 certificates took 1.083 ms total in this run. Measured per-vertex p50 / p95 nanoseconds were:

| Influences | Vertices | Original p50 / p95 | Certified p50 / p95 | p50 ratio |
|---:|---:|---:|---:|---:|
| 1 | 40,831 | 11.596 / 12.770 | 3.754 / 4.350 | 3.09x |
| 2 | 30,995 | 11.989 / 12.438 | 5.137 / 5.272 | 2.33x |
| 3 | 676 | 13.262 / 14.102 | 6.290 / 8.174 | 2.11x |
| 4 | 10,028 | 14.597 / 14.610 | 7.784 / 7.793 | 1.88x |

Weighted over this particular fixture corpus, the measured p50 estimates are about 1.00 ms for the original checks and 0.40 ms for the certified checks. At the measured certificate creation cost, that implies roughly two reuses to amortize setup. This is only a CPU microbenchmark estimate for these fixtures and this host; it is not an application-frame estimate.

## Limits

This proves parity and a potential CPU reduction for repeated bounds checks over the same owned canonical vector. It does not prove that the live renderer invokes the check often enough with a reusable vector to repay certificate construction, nor that this affects frame time. No production wiring, game run, GPU work, FPS claim, or shipping recommendation is included. Any integration would need to ensure the same canonical vector instance persists across calls; rebuilding or copying it per pose would erase the measured advantage.
