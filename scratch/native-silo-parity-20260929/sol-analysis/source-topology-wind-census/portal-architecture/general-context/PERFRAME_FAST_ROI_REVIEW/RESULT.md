# Stage-10 per-frame portal fast-path diagnostic

Both arms ran the same candidate binary with per-frame suppression ON. The fast arm enables population-join reuse and prepared-motion reuse together; the full arm leaves both off. This pair estimates only their combined overhead change.

| Metric | Fast combined reuse | Full join + motion path |
|---|---:|---:|
| Portal-qualified frames / rows | 583 / 583 | 556 / 556 |
| Actual skips per-frame p50 / p99 | 4056 / 4056 | 4056 / 4056 |
| Candidate draws per-frame p50 | 1260 | 1260 |
| Graph CPU p50 / p99 | 0.716 / 0.754 ms | 0.715 / 0.755 ms |
| Mask CPU p50 / p99 | 1.806 / 1.880 ms | 4.550 / 4.841 ms |
| Graph + mask CPU p50 / p99 | 2.522 / 2.609 ms | 5.266 / 5.580 ms |
| On-Glass interval throughput / 1% low | 29.06 / 24.00 FPS | 27.61 / 19.20 FPS |
| Callback throughput / 1% low | 29.05 / 26.09 FPS | 27.71 / 23.81 FPS |
| Callback interval p50 / p99 | 34.00 / 38.00 ms | 36.00 / 40.00 ms |
| E01 scene CPU p50 / encode CPU p50 | 28.395 / 30.425 ms | 30.138 / 32.271 ms |
| E01 indexed draws p50 / updated bytes p50 | 10715 / 15988688 | 10249 / 15621800 |
| On-GPU elapsed p50 / p99 | 22.477 / 23.678 ms | 22.182 / 23.181 ms |
| In-flight CPU end-to-end p50 / p99 | 32.277 / 33.617 ms | 34.071 / 37.634 ms |

Graph and mask timers are disjoint Calculate/callback stages, so their per-frame sums are reported separately from the total frame. GPU elapsed and in-flight CPU end-to-end overlap pipeline work and are not additive; E01 CPU stages and frame-upload counters are also separate diagnostics.

On-Glass throughput is `N / sum(interval_seconds)` from selected drawable durations, not mean reciprocal FPS. Both runs stayed on AC power, reported no thermal/performance warning, and showed no swap-use change at the boundaries. The window is a single matched 20/20 sample; normal run-order and variance limits remain. It cannot isolate population reuse from prepared-motion reuse, establish motion/visual safety, or support an FPS acceptance or shipping claim.
