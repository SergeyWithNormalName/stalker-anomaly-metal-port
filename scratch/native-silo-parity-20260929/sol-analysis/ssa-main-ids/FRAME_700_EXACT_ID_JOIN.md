# Frame 700: actual main Flora IDs joined to source SSA

The observer's 5,316 successfully encoded main G-buffer Flora visual IDs all
join uniquely to the archive-winning `l01_escape/level` (SHA-256
`1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`).
Every observer leaf/trunk label agrees with its source OGF shader ID. The
observer reported 11,663 admitted and 6,347 rejected by its existing Metal
clip test; those counts reconcile to 5,316 emitted IDs. Its `.submitted`
marker and completed E01 row agree on frame/serial 700, and device phase is
measured phase 1. The recorded view maps the recorded eye to the origin;
projection agrees with FOV 80°/1920×1200, and bit-exact `g_fSCREEN` and
`r_ssaDISCARD` reproduce `CRender::Calculate`.

| Material | Actual IDs | Below hard SSA | Hash fade rejects | Keeps in fade | Above fade |
|---|---:|---:|---:|---:|---:|
| Trunk | 2,931 | 653 | 232 | 326 | 1,720 |
| Leaf | 2,385 | 93 | 221 | 353 | 1,718 |
| Total | 5,316 | 746 | 453 | 679 | 3,438 |

Thus **1,199 actual native main Flora draws** are numerically in the Windows
source SSA reject set if their exemption flags are false: 22.6% of Flora draws
and 8.1% of this frame's 14,864 indexed draws. These are work-count ratios,
not CPU/GPU time or FPS savings. The prior sphere-frustum proxy predicted
1,202; the exact-ID join removes that camera-intersection approximation.

The parser mirrors `CalcSSA` with the exact OGF_HEADER float32 sphere and the
recorded float32 eye, effective threshold and fade settings, and mirrors
`GetFvectorHash` on the sphere-center bits. It separately checks a double
calculation and marks values within 64 float32 ULP of either SSA threshold or
within 1e-6/64 ULP of the hash decision. **Uncertain arithmetic boundary:
zero IDs** under that stated guard. This does not prove bit-identical Windows
`_powf` for every input, but no decision is close to that approximation here.

**No ID is qualified for suppression yet.** The flattened native record does
not contain the runtime `eIgnoreOptimization` bit or selected shader E[4]
`bEmissive` flag that bypass the Windows source gate. The static visual
constructor initializes flags to zero and the visible source call sites of
`MarkIgnoreOptimization` target HUD/dynamic objects, but that is not a
per-visual runtime receipt for this GAMMA level. Shader E[4] emissive status
also needs an exact archive-winning shader/material provenance check. Until
both are established, all 1,199 numerical rejects remain fail-open/unknown.
Shadow casters, forward, HUD, volumetric and secondary consumers were not
observed or changed by this analysis.

Inputs: `runs/ssa-main-ids-observe-01/native/output/main-flora-ids/` CSV
(SHA-256 `dca66b5c94dffae895bc33ba77a4cdc57881cce7c5252b07ba823b2dd6bdcdbc`),
same-run E01 and phase CSV, and the archive-winning level. Reproduction is in
`join_main_flora_ssa.py`; structured evidence is in
`frame-700-exact-id-join.json`. The observer frame is diagnostic and cannot
be used as a performance arm.
