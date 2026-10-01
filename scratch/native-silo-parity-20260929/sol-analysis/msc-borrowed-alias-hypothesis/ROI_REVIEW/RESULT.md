# Borrowed MSC typed-alias pair: ROI review

Both arms ran the same candidate executable and controlled scene. The sole environment difference was `XRAY_METAL_E08_BORROW_MSC_TYPES=0` versus `1`. The candidate changes local typed aliases in `BindMscResources`; it keeps owners, descriptors, arena snapshots, GPU addresses, and indirect-read declarations unchanged.

| Metric | Flag off | Flag on |
|---|---:|---:|
| On-Glass throughput (N / summed drawable intervals) | 25.23 FPS | 25.48 FPS |
| On-Glass 1% low | 20.57 FPS | 20.00 FPS |
| Callback throughput | 25.22 FPS | 25.47 FPS |
| Callback 1% low | 23.26 FPS | 21.82 FPS |
| Callback interval p50 / p99 | 40.00 / 43.00 ms | 39.00 / 43.00 ms |
| E01 scene CPU p50 / encode p50 | 34.212 / 35.499 ms | 33.660 / 34.961 ms |
| E01 draws p50 / updated bytes p50 | 14923 / 24734148 | 14849 / 24693184 |
| GPU elapsed p50 / p99 | 29.928 / 30.543 ms | 30.027 / 30.753 ms |
| In-flight CPU p50 / p99 | 37.361 / 38.704 ms | 36.830 / 40.106 ms |

The pair collected 504 versus 508 On-Glass intervals. GPU elapsed and in-flight CPU overlap pipeline work and are not additive; E01 phase values are separate diagnostics.

The result is a small, single 20-second pair: a roughly 1% On-Glass difference does not establish a repeatable gain. AI-driven draw counts differ slightly, so the actual work was not identical. The build's GPU fixture probe did run with the candidate flag set to 1 and passed the flora-instancing fixture; that does not measure or prove the lifetime behavior of the live game scene. No shipping/FPS acceptance follows from this pair.


## Separate resource-dedup diagnostic

This is an independent comparison against the alias-off baseline above. Borrowed aliases remain off in both arms; only `XRAY_METAL_E08_DEDUP_RESOURCES` changes from its default-off absence to `1`. Do not combine these values with the alias-on pair or attribute the alias-on results to dedup.

| Metric | Dedup default off | Dedup enabled |
|---|---:|---:|
| On-Glass throughput | 25.23 FPS | 27.07 FPS |
| On-Glass 1% low | 20.57 FPS | 19.46 FPS |
| Callback throughput | 25.22 FPS | 27.16 FPS |
| Callback 1% low | 23.26 FPS | 24.39 FPS |
| Scene CPU p50 / encode CPU p50 | 34.212 / 35.499 ms | 31.640 / 32.914 ms |
| Indexed draws p50 / updated bytes p50 | 14923 / 24734148 | 14311 / 24329348 |
| GPU elapsed p50 | 29.928 ms | 29.754 ms |

This second single 20-second screen shows a larger throughput difference, but its measured draw count is substantially lower with dedup enabled. That scene-work difference prevents attributing the result to resource deduplication; it is a hypothesis signal only, not an ROI or FPS acceptance.
