# Matched native/Silo measured-window diagnostics

These are read-only summaries of the two completed fixed-camera windows. Native spans 2026-09-30T22:04:59+00:00–2026-09-30T22:05:19+00:00; Windows/Silo spans 2026-09-30T22:11:31+00:00–2026-09-30T22:11:51+00:00. Both are 20 seconds and use the same Lua SHA-256 `41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb`. All 520 native and 754 Windows measured callback rows passed the level, calendar, factors, weather, camera, and 1920x1200 gates. Marker/weather checks passed in both. The saved-game bytes are identical by SHA-256 `4558915180d6fdc11d22006b4f9dff850c581fc15c57514b1f8cec39252b9225`; the loaded filename differs by backend.

| Measured metric | Native R1 Metal | Windows/Silo D3DMetal |
|---|---:|---:|
| On-Glass drawable-event rate over 20s | 25.95 FPS | 37.00 FPS |
| On-Glass frame interval p50 / p99 | 37.50 / 45.83 ms | 25.44 / 93.37 ms |
| On-Glass 1% low | 20.30 (6 intervals) | 7.42 (8 intervals) |
| Lua callback mean throughput | 25.99 FPS | 37.69 FPS |
| Callback frame interval p50 / p99 | 38 / 43 ms | 22 / 88 ms |
| Callback 1% low | 23.00 (6 intervals) | 7.33 (8 intervals) |

The On-Glass sample contains 519 native and 740 Windows drawable intervals. Event rate is event count divided by the exact 20-second marker window. The `1% low` is the arithmetic mean of the lowest `ceil(0.01*N)` instantaneous FPS samples. The arithmetic mean of reciprocal frame intervals is retained in JSON but not used as primary displayed rate because very short intervals can inflate it. On-Glass frame interval percentiles use the MetalPerfTrace drawable duration in milliseconds. Callback throughput is `(last engine_frame - first engine_frame) / ((last wall_ms - first wall_ms) / 1000)`. Callback p50/p99 frame intervals use nearest-rank quantiles of `1000 * delta_wall_ms / delta_engine_frame`; callback 1% low is the mean FPS over the same slowest `ceil(0.01*N)` callback intervals. Native has 520 measured Lua rows and Windows has 754; drawable and callback counts differ.

Both MetalPerfTrace layers report 1920x1200 BGRA8 output. In every measured drawable, MetalFX and frame interpolation are false, auto exposure is false, and input/target dimensions are zero; the trace shows no MetalFX scaling or frame interpolation. Windows received `D3DM_ENABLE_METALFX=1`, but that environment flag did not mean MetalFX was active. The trace does not directly certify every internal render-target dimension.

The committed weather fields match numerically, including clear2 weight 0, far plane 648, fog distance 638, wind 430, and the recorded ambient/hemisphere/sun/fog vectors. The loaded-state weather name differs (`w_cloudy2` native; `w_cloudy1` Windows) before both transition to paused `w_clear2` before measurement. Both traces show the same drawable/pixel format configuration.

Native E01 is enabled and has 521 completed records across its marker frame range; Windows did not enable E01. E01 conditionally reads high-resolution clocks at frame stages and writes a mutex-protected completion row for every frame. The Lua observer also writes one row per frame in both runs. This instrumentation asymmetry and the short, single window make the numbers diagnostic, not a clean acceptance A/B. The Windows On-Glass 1% low is driven by a small tail of long intervals (slowest measured intervals: 337.7 ms, 221.4 ms, 154.3 ms); all such rows still passed the state checks.

Native executable `d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0` is the R1 baseline with a 48-input selected-source receipt; that receipt explicitly does not prove the full repository/build dependency closure. Windows used stock executable `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3` through the reviewed MO2/Wine D3DMetal route. Both primary-integrity checks passed. No visual/quality proof, effective-shadow counter comparison, or global winner conclusion is included.
