# Per-frame portal ROI: matched 20-second indoor diagnostic

The three runs passed the same binary/source, Lua profile, user.ltx/save, window-state, and exit gates. This is a diagnostic comparison only; the observer-only, suppression-on, and feature-off runs are each one short window.

| Metric | Observe | Suppress on | Feature off |
|---|---:|---:|---:|
| On-Glass interval throughput | 22.07 FPS | 26.82 FPS | 25.23 FPS |
| On-Glass frame interval p50 / p99 | 45.83 / 50.00 ms | 37.50 / 41.67 ms | 37.50 / 45.83 ms |
| On-Glass 1% low | 20.00 FPS (5 intervals) | 19.46 FPS (6 intervals) | 19.20 FPS (6 intervals) |
| Lua callback throughput | 22.06 FPS | 26.84 FPS | 25.18 FPS |
| Callback interval p50 / p99 | 45.00 / 49.00 ms | 37.00 / 41.00 ms | 39.00 / 44.00 ms |
| Callback 1% low | 20.08 FPS | 24.29 FPS | 19.29 FPS |
| E01 scene CPU p50 / encode CPU p50 | 38.683 / 40.757 ms | 31.287 / 33.352 ms | 33.998 / 35.328 ms |
| On-GPU elapsed p50 / p99 | 29.922 / 30.476 ms | 22.287 / 22.971 ms | 29.947 / 30.904 ms |
| In-flight CPU end-to-end p50 / p99 | 42.685 / 46.142 ms | 35.173 / 37.957 ms | 37.237 / 40.964 ms |
| Portal-qualified frames / measured rows | 443 / 443 | 538 / 538 | 0 / 0 |
| Actual portal skips per frame p50 / p99 | 0 / 0 | 4056 / 4056 | n/a |
| Graph + mask CPU p50 / p99 | 5.250 / 5.607 ms | 5.382 / 5.638 ms | n/a |

On-Glass throughput is selected drawable count divided by the sum of selected MetalPerfTrace interval durations; it is not the mean of reciprocal FPS samples. The 1% low is `1000 / mean(slowest ceil(1% N) frame intervals in ms)`. Callback throughput is frame delta divided by callback wall-time delta; callback 1% low uses the same slow-interval formula.

E01 CPU stages, On-GPU elapsed, and in-flight CPU end-to-end are separate measurements; do not add them. Only graph and mask CPU times are summed, because they are disjoint Calculate and callback phases. CSV/file logging and E01 instrumentation remain diagnostic overhead.

The run trio is limited to the first 20/20 pair. It establishes neither motion/visual acceptance across changing views nor shipping safety, and it makes no FPS winner claim. The portal hypothesis remains default-off outside this controlled diagnostic.
