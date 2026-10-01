# V5 conditional stage attribution

Run: one direct V5 executable process (PID 31363), exit 0, no watchdog timeout. This is a locked-display, fixed-scene timing diagnostic only; it does not establish visible correctness, motion acceptance, presented-frame latency, or a gameplay FPS claim.

The cloned canonical runtime loaded `l01_escape` / `w_clear2` at device time factor 1 and world factor 6, 1920x1200. The measured marker interval was 10.016 s (frame 305 through 504); statistics below use complete interior samples `camera_frame` / `engine_frame` 306–503, 198 rows in both E01 and E07. E01 reports all 198 as `completed`; GPU time is `gpu_end_s - gpu_start_s` only for those rows with a positive finite interval. Percentiles use nearest-rank p95 and the ordinary median p50.

| Stage / field | Samples | p50 ms | p95 ms |
|---|---:|---:|---:|
| E01 wait | 198 | 0.000209 | 0.000416 |
| E01 drawable | 198 | 0.029604 | 0.034417 |
| E01 scene | 198 | 44.217771 | 45.905666 |
| E01 grass | 198 | 0.000583 | 0.000833 |
| E01 whole dynamic prewarm | 198 | 5.192229 | 5.406584 |
| E01 encode | 198 | 45.645396 | 47.509834 |
| E01 present encode | 198 | 0.017417 | 0.019542 |
| E01 commit | 198 | 0.010291 | 0.014208 |
| E01 completed GPU interval | 198 | 34.443167 | 34.819750 |
| E07 CPU fallback skin | 198 | 0.662458 | 0.676833 |
| E07 CPU fallback bounds | 198 | 0.016521 | 0.016917 |
| E07 CPU fallback upload | 198 | 0.127771 | 0.161833 |
| E07 shadow CPU | 198 | 2.434225 | 2.633320 |
| E07 local shadow CPU | 198 | 13.401500 | 14.180700 |
| E07 sun shadow CPU | 198 | 2.531440 | 2.694540 |

E07 reported a median 3,851 computed vertices and 138,636 uploaded bytes per sample, with 702 pose reuses, 754 cache hits, 1 cache miss, 749 skinned draws, and 2,226 shadow draws per sample. These are the existing adapter counters; they do not separately attribute compute-check-bound work. Per root's instrumentation interpretation, E07 skin/bounds/upload times are CPU fallback only. The whole E01 dynamic-prewarm duration is the CPU stage boundary for this architecture; no nested timings are added together.

The run enabled `XRAY_METAL_COMPUTE_SKIN=1` and `XRAY_METAL_DYNAMIC_SHADOW_INDEX=1`, with oracle and cold-upload probes disabled. It collected no per-job compute-route counter, so route activation in this particular run cannot be independently counted from the timing files; the separate preceding real-game numeric oracle run is the evidence for actual submitted compute jobs. There were no screenshots, motion, or presentation measurements. These timings are conditional on the locked-display environment and instrumentation.

Integrity: candidate V5 executable remained SHA-256 `5fd0bde4a9d1662fd51ec332e90957caa7984726d20415966de5679412afc44f`. Original build executable remained `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`; V4 remained `2bb3dc6b4b35a805a991792864c98f40fb7df128f8431da306d731a88d517f71`. The canonical base `user.ltx` and save hashes matched the runtime clone inputs after the run.

Raw inputs: `e01-frames.csv`, `e07-dynamic-world.csv`, `attribution-markers.csv`, `environment.txt`, `input-sha256.txt`, `post-run-app-sha256.txt`, `engine.log`, and `exit-status.txt` in this directory.
