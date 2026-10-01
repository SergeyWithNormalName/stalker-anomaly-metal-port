# Main Flora material grouping: stop after hypothesis screen

The same frozen candidate `caf17ba06ee35482f88be278711fb90747cfd43d89b3a46c1037ddca47f97549` (manifest `21f302c3e4f76449ce85a2988952faf82693d50e7ba8cd1c6c835eb593d18f8a`) ran ON then OFF without CUA. Both exits 0, fixed Cordon camera 1920x1200, warm/measure 20/20 seconds, MetalFX off, AI active. Measured weather fields matched, calendar start differed by eight seconds. On-glass coverage valid and timeline gap zero in both selected 17-second windows.

| Metric | OFF | ON |
|---|---:|---:|
| On-glass FPS | 26.1754 | 26.0972 |
| Callback FPS | 26.2320 | 26.2117 |
| CPU encode p50 ms | 34.2207 | 34.0939 |
| Whole-frame indexed draws p50 | 14254 | 14893 |
| Whole-frame PSO calls p50 | 10713 | 5868 |
| GPU command-buffer elapsed p50 ms | 29.8875 | 28.8097 |
| Displayed p99 interval ms | 45.8333 | 45.8333 |
| Displayed 1% low FPS | 18.1814 | 17.6471 |

Actual main Flora stayed at 5,316 draws. ON main pipeline calls were two versus 5,316 OFF at periodic snapshots; every per-draw resource/constant binding remained. Whole-frame dynamic/shadow work differed by 639 draws at median, so neither the -0.30% displayed change nor GPU elapsed difference is a clean causal estimate. Elapsed is not occupancy. This pair establishes no demonstrated FPS gain, not proof that this idea can never help. Given that weak result and equal-depth G-buffer ordering risk, stop this branch without building the complex main pixel oracle or promoting it.

No source-order change remains in production. Source prestate restored exactly and normal app/bare SHA `d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0` restored before runs. `restore-receipt.json`, `REVIEW.md`, draft/source manifests and `pair-result.json` preserve the scope. This does not compare against Silo or certify pixel/motion quality.
