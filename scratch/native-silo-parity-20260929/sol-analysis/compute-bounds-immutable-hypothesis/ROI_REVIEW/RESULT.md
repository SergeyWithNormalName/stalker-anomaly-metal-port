# Compute-bounds source-certificate pair: independent review

The normalized run environments differ only in `XRAY_METAL_COMPUTE_BOUNDS_SOURCE_CERTIFICATE` (0 off, 1 on). Both arms used the same candidate executable/source manifest, matched profile/save/user.ltx, R4 scene, 20-second marker window, fixed camera, weather/clock, 1920×1200 drawable, compute-skin and compute-skin oracle diagnostics. The eight pre-window GPU oracle samples passed in each arm (two samples per influence count 1–4).

| Metric | Certificate off | Certificate on |
|---|---:|---:|
| On-Glass throughput (N / summed intervals) | 28.62 FPS | 29.64 FPS |
| On-Glass interval p50 / p99 | 33.333 / 41.667 ms | 33.333 / 54.167 ms |
| On-Glass 1% low | 16.55 FPS | 16.36 FPS |
| Device callback throughput | 28.65 FPS | 29.72 FPS |
| Device callback interval p50 / p99 | 35.000 / 40.000 ms | 33.000 / 48.000 ms |
| Callback 1% low | 20.41 FPS | 17.29 FPS |
| E01 dynamic prewarm p50 | 4.975 ms | 4.043 ms |
| E01 indexed draws p50 | 10851 | 10245 |
| E07 certified bounds compute p50 / mean | 1.910 / 1.960 ms | 0.973 / 0.971 ms |
| E07 certified bounds per-frame ns / vertex p50 | 13.013 ns/vertex | 5.956 ns/vertex |
| E07 certified bounds aggregate ns / vertex | 13.015 ns/v | 5.890 ns/v |
| E07 certified bounds vertices / frame p50 | 146276 | 165188 |
| E07 existing bounds routine p50 / p99 | 0.016 / 0.026 ms | 0.016 / 0.024 ms |
| GPU elapsed p50 / p99 | 22.299 / 23.011 ms | 22.027 / 23.794 ms |
| In-flight CPU end-to-end p50 / p99 | 32.597 / 39.111 ms | 31.056 / 39.586 ms |

For the bounds helper, normalized cost is the median of per-frame `compute_bounds_ms × 1,000,000 / compute_bounds_vertices` across measured E07 rows. It fell from 13.013 to 5.956 ns per vertex (54.2% lower), while median processed vertices rose from 146,276 to 165,188 per frame. The ratio-of-sums is also retained in the JSON. This supports a reduction in the instrumented source-bounds helper cost in this pair.

Whole-frame On-Glass throughput changed by 3.54% (off 28.62 FPS, on 29.64 FPS); the 1% low moved from 16.55 to 16.36 FPS. Indexed draw work also differed (median 10851 vs 10245), so scene workload was not identical. This single pair does not establish that the helper change caused the whole-frame rate movement or a repeatable FPS gain. E01 stage CPU, GPU elapsed, and in-flight CPU end-to-end timings are separate and non-additive.

Both arms recorded AC power and no thermal warning. Swap snapshots were unchanged within each arm; their absolute starting values are context, not controls. The measured phases include E01/E07 instrumentation and CSV logging. The source certificate is an immutable-source reuse hypothesis; these runtime results do not establish broader data-quality equivalence or shipping admission.
