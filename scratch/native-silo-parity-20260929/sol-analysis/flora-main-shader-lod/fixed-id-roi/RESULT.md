# Leaf LQ fixed-ID ROI: first same-binary pair

The isolated candidate02 completed both `leaf-lq-on-01` and `leaf-lq-off-01` with exit0. API/shader diagnostics were off. The intended 5,316 Flora main draws stay unchanged; 1,856 fixed Leaf IDs choose the original source LQ permutation only on exact camera match. All shadow routing remains the baseline.

| Metric | HQ OFF | LQ ON |
|---|---:|---:|
| On-Glass FPS | 25.8807 | 26.9286 |
| Callback FPS | 25.8860 | 26.9249 |
| CPU encode p50 ms | 34.2619 | 32.9688 |
| GPU command-buffer elapsed p50 ms | 30.0038 | 25.4854 |
| Display p99 ms | 50.0000 | 41.6666 |
| Display 1% low FPS | 20.0000 | 23.5294 |

On-Glass change is **+4.05%**. This is a positive hypothesis screen, not an accepted shipping improvement. Whole-frame median draws are 14,442 OFF and 14,190 ON, although Flora main draw count intentionally stays fixed; dynamic/shadow work differs by252. That limits causal attribution from one pair. Numeric weather fields at commit/start/end, calendar08:09:47, Device factor1, experimental calendar/ALife factor1e-6 and1920x1200 match. Live AI still updates. No CUA interaction occurred in either numeric run.

Normal source and app/bare output were restored exactly before games; normal object code was recompiled from the restored source bytes. Candidate01 was not run, because stale-object provenance could not be trusted. Candidate02 EXE `ecf55ea1...`, manifest `0dd12fef...` and all 48 source inputs are frozen together.

Independent ABI/fixture review and bounded runtime validation support pipeline admission. Validation was a separate run; requested per-pipeline GPU instrumentation scope was not independently confirmed. LQ deliberately differs in parallax, branch bending and color variation at distance. A varied-camera rendered/motion review and a general distance policy are still required before player-facing admission. This fixed-ID oracle is not installed into the game.

Exact metrics/limits: `roi-pair-result.json`; fixture and shader receipts are retained nearby. No Windows performance-win or ordinary gameplay acceptance is claimed.
