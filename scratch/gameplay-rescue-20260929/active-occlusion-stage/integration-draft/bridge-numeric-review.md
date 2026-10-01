# Scene bridge numeric review

Reviewed read-only snapshot: `draft/R4ActiveOcclusionSceneBridge.cpp`, SHA-256
`dbe84b8f93ab8c1395a6770168148c7881cda66f680bbb65872d8c44794d11d4`.

One concrete strict-bound issue was found at lines 45, 60, and 65. Four
nonnegative float-product magnitudes are accumulated with ordinary binary64
addition, then only one `nextafter` is applied before conversion. The products
are exactly representable in binary64 for float inputs, but up to three rounded
additions can undershoot by more than one binary64 ulp. At a float rounding
boundary, `toUpperFloat` can therefore return a value below the true absolute
term sum, violating the `clipAbsDotTerms` upper-bound contract. The same sum
feeds `cpuDotError`. Suggested correction: round each addition outward with
`nextafter(sum + term, +infinity)`, or apply a proven `gamma3` binary64
inflation before conversion.

No separate nonzero-jitter underbound was identified in lines 72-89 under the
declared strict-FP assumptions. The bound includes base X CPU error, CPU and
GPU W-dot uncertainty scaled by `|j|`, fused/unfused jitter arithmetic error,
and an FTZ allowance. This is a code review only; no build, test, game, or GPU
validation was run.
