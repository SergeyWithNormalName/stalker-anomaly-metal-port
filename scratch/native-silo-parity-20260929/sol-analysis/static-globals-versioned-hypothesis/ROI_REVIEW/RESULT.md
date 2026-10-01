# Versioned static-globals: paired 20-second diagnostic

Both runs passed the same executable, source manifest, 20-second profile, save, user.ltx, weather/clock/camera and drawable gates. The normalized environment comparison found exactly one setting change: `XRAY_METAL_VERSIONED_STATIC_GLOBALS=0` to `1`.

| Metric | Off | On |
|---|---:|---:|
| On-Glass interval throughput | 25.18 FPS | 21.47 FPS |
| On-Glass interval p50 / p99 | 37.50 / 50.00 ms | 50.00 / 54.17 ms |
| On-Glass 1% low | 18.00 FPS | 17.91 FPS |
| Lua callback throughput | 25.21 FPS | 21.57 FPS |
| Callback interval p50 / p99 | 39.00 / 44.00 ms | 48.00 / 54.00 ms |
| Callback 1% low | 20.62 FPS | 17.61 FPS |
| E01 scene CPU p50 / encode CPU p50 | 33.944 / 35.370 ms | 32.937 / 34.275 ms |
| E01 indexed draws p50 / updated bytes p50 | 14932 / 24727224 | 14226 / 19980472 |
| GPU elapsed p50 / p99 | 30.131 / 30.582 ms | 51.378 / 56.351 ms |
| In-flight CPU end-to-end p50 / p99 | 37.253 / 41.107 ms | 128.615 / 141.856 ms |
| E01 GPU elapsed p50 / p99 | 30.136 / 30.582 ms | 51.087 / 55.165 ms |

On-Glass throughput is selected drawable count divided by the sum of its MetalPerfTrace interval durations (`N / sum(interval)`), not a mean of reciprocal FPS. Both On-Glass and callback 1% low use `1000 / mean(slowest ceil(1% N) interval_ms)`.

E01 CPU stages, E01 per-row GPU intervals, MetalPerfTrace On-GPU elapsed, and in-flight CPU end-to-end times are separate/non-additive measurements. E01 timing and per-frame CSV logging add diagnostic overhead. A lower updated-byte count alone does not establish FPS benefit or full data-quality equivalence.

System snapshots show AC power at 100% and no thermal/performance warning in either arm. Starting memory state differed: free swap was 1739 MB off vs 745 MB on, and `vm_stat` free pages were 361,099 vs 197,933. Swap used was similar and unchanged within each run. This makes the single-pair attribution uncertain, though it does not erase the observed GPU/in-flight regression.

The observed On-Glass interval throughput is 14.8% lower with the flag on. E01 scene/encode CPU medians are slightly lower, while On-GPU elapsed rises from 30.13 to 51.38 ms and in-flight CPU end-to-end from 37.25 to 128.62 ms. E01 indexed draws also differ (14,932 off vs 14,226 on), so per-window AI/render workload was not identical despite matched camera and save. These results reject the candidate as a renderer ROI for further admission on current evidence; they do not isolate the cause or establish a player-visible comparison.

The separate E02 uniform ownership GPU probe passed, but it does not prove the runtime shader/TLAB consumes the intended versioned uniform or that all scene data remains equivalent. This is one 20/20 pair and does not establish full output equivalence, a user-facing winner, or shipping admission.
