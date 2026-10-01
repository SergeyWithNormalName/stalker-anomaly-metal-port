# V5 numeric-only compute-skin runtime harness

This is a draft runner for root review. It launches nothing. Runtime owner may execute it only after root authorization:

```sh
bash scratch/gameplay-rescue-20260929/compute-oracle-stage/numeric-harness/run_numeric_oracle.sh
```

The runner launches the direct `candidate-v5/Anomaly.app/Contents/MacOS/AnomalyDX11` executable only if its SHA-256 is `5fd0bde4a9d1662fd51ec332e90957caa7984726d20415966de5679412afc44f`. It clones the canonical V2 runtime and `rescue-cordon-control.scop` into a fresh V5 run directory with `ditto`, matching the V4 runtime layout and preserving its GAMMA/mod symlinks. It pins the same Cordon camera/environment and selects the corrected Flora depth bundle plus V4 dynamic-shadow-index/Flora-instancing bundles.

Both compute flags and the real-scene `XRAY_METAL_TEMPORAL_COLD_UPLOAD_PROBE=1` are enabled. Late submit retry is explicitly unset: V4 startup logs first prepared gameplay at engine frame 27, but this harness does not guess a retry threshold. The first actual `scene.Render` is discarded by the cold-upload probe; at least one exact oracle PASS must complete in a submitted frame after that abort. The post-load capture window is 15 seconds. The 180-second owned-process ceiling includes launch and reserves five seconds for TERM/KILL cleanup.

Outputs are engine/stdout logs, simple harness state markers, input hashes (app, save, user settings, both corrected Flora shader stages and bundle receipts), and an oracle-only summary with per-influence PASS counts and explicit missing classes. There is no screenshot/window capture, UI automation, PERF_GAP trace, accepted-FPS calculation, E01/E07 profiling, or ordinary timing output. FAIL/INCONCLUSIVE/DISABLED oracle statuses, missing successful post-abort PASS, missing cold-abort/capture markers, or nonzero game exit fail the harness; SKIPPED remains separately reported and never counts as PASS.

Raw engine logs may contain invalid UTF-8 from hardware/system diagnostics. Future runner post-processing sets `LC_ALL=C` only on the `awk`/`grep` commands that inspect log/marker files; it does not export a locale change to the game. The already-executed run copy and its receipts remain unchanged, and this parser-only repair does not require a rerun.

Static validation: `bash -n` is required before root review. No launch or game run has been performed by the draft author.
