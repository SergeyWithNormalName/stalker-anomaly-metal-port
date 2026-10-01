# Matched native/Silo measured-window diagnostics (v2)

These are read-only summaries of the completed fixed-camera windows. Native spans 2026-09-30T22:04:59+00:00–2026-09-30T22:05:19+00:00; Windows/Silo spans 2026-09-30T22:11:31+00:00–2026-09-30T22:11:51+00:00. Both use the same Lua SHA-256 `41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb`. All 520 native and 754 Windows measured callback rows passed level, calendar, factors, weather, camera, and 1920x1200 gates. The saved-game bytes match SHA-256 `4558915180d6fdc11d22006b4f9dff850c581fc15c57514b1f8cec39252b9225`.

| Measured metric | Native R1 Metal | Windows/Silo D3DMetal |
|---|---:|---:|
| On-Glass interval throughput (`N / sum(interval)`) | 26.00 FPS | 37.06 FPS |
| On-Glass frame interval p50 / p99 | 37.50 / 45.83 ms | 25.44 / 93.37 ms |
| On-Glass 1% low (`1000 / mean(slowest 1% intervals)`) | 20.28 (6 intervals) | 6.23 (8 intervals) |
| Lua callback throughput | 25.99 FPS | 37.69 FPS |
| Callback interval p50 / p99 | 38 / 43 ms | 22 / 88 ms |
| Callback 1% low (`1000 / mean(slowest 1% intervals)`) | 22.99 | 6.26 |
| On-GPU elapsed p50 / p99 | 29.867 / 30.919 ms | 21.627 / 43.949 ms |
| In-flight CPU end-to-end p50 / p99 | 36.155 / 40.840 ms | 14.509 / 31.937 ms |

On-Glass interval throughput uses selected drawable count divided by the sum of their recorded MCT interval durations; it is not the mean of per-event reciprocal FPS. The marker epochs have one-second timestamp resolution, so event inclusion is bounded by those markers and the trace dates, while throughput uses only included event intervals. The On-Glass 1% low is `1000 / mean(slowest ceil(0.01*N) interval_ms)`. The callback 1% low applies the same definition to per-engine-frame intervals. The v2 JSON retains the arithmetic mean of lowest instantaneous FPS as a separately labeled supplemental metric. Frame-interval percentiles use nearest-rank quantiles. Callback throughput remains frame deltas divided by wall-clock span.

On-GPU elapsed and in-flight CPU end-to-end measurements are shown separately; they overlap in the frame pipeline and must not be added. Native On-GPU samples include 0 zero values of 519; its p50/p99 therefore need the E01/instrumentation caveat below and are not a direct cross-backend work attribution. The native trace has E01 enabled and the Windows trace does not. E01 conditionally reads high-resolution clocks and writes a completion CSV; both runs also have Lua per-frame CSV logging. These are short, instrumented diagnostics, not an uninstrumented acceptance A/B.

Both traces report a 1920x1200 BGRA8 output layer. MetalFX scaling, frame interpolation, and auto exposure are false in every measured drawable; input and target dimensions are zero. The Windows environment set `D3DM_ENABLE_METALFX=1`, but the trace confirms MetalFX was not active. Internal render-target dimensions are not independently certified. Committed weather fields match, including clear2 weight 0, far plane 648, fog distance 638, wind 430, and the recorded lighting/fog vectors. Initial loaded weather names differ before both reach paused `w_clear2`.

Native executable `d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0` is the R1 baseline with a 48-input selected-source receipt that does not prove the full build dependency closure. Windows used stock executable `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3` through the reviewed MO2/Wine D3DMetal route. Primary-integrity checks passed. No visual/quality proof or effective-shadow counter comparison is available, so this comparison declares no overall winner.
