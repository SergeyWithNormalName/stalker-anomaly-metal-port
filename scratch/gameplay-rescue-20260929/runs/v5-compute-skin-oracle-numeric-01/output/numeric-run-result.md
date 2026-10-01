# V5 submitted compute-skin numeric run

This is the single authorized numerical-only V5 run. It produced actual post-submit renderer oracle receipts after the cold encoded-frame discard. It does not provide visual, motion, presentation, or FPS acceptance.

## Gates and outcome

- Direct candidate executable: V5 SHA-256 `5fd0bde4a9d1662fd51ec332e90957caa7984726d20415966de5679412afc44f`; post-run hash is identical.
- Runtime and save: isolated clone of `runs/v2-canonical-oracle-01/runtime`, save `rescue-cordon-control.scop`. The source runtime’s `user.ltx` remains `b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334` and save remains `a21f09761ca6e7c3633e0b002b34d760ea30ece41f6542a1814eabba5a0ac3c4`.
- Owned game PID: 30536. Game exited 0 inside the 175-second watchdog; no timeout file was created.
- Exactly one real `METAL_TEMPORAL_COLD_UPLOAD_ABORT` occurred at engine frame 27, submitted serial 26. The harness recorded one capture-start and one capture-end marker.
- After the abort, eight native `frame_serial_submitted` exact-byte oracle receipts passed: two each for 1W, 2W, 3W, and 4W. They span serials 27–34 and engine frames 28–35; each reports `mismatch_bytes=0`. See `engine.log` and `oracle-summary-byte-safe.json`.
- No compute-backend disable, oracle fail/inconclusive, harness `FAILED_` marker, screenshot, or FPS measurement occurred. One geometry entry used the designed per-object CPU fallback because its UV was nonfinite; this did not disable compute or prevent any influence class coverage.

## Runner postprocessing issue

The game itself exited 0 and the numeric receipts passed. The frozen shell runner returned status 1 because macOS `/usr/bin/awk` failed multibyte conversion on a non-UTF-8 hardware-identification line in `engine.log` while generating its summary. The original runner and raw failure are preserved (`run_numeric_oracle.sh`, `runner-postprocess-failure.txt`). A separate byte-safe Python postprocess produced `oracle-summary-byte-safe.json`; it does not change or overwrite the original runner exit status. No second game run was made.

The engine log also contains six nonfatal script load errors for `a_faction_prices`, `magazines_loot`, `mags_patches`, `zz_ui_inventory_better_stats_bars`, `zzz_mspizza_godis_zoom_control`, and `zzz_mspizza_godis_zoomcalc`; four report missing optional globals. Despite those messages, the canonical save loaded, the Lua harness reached both markers, and the native renderer submitted the eight exact oracle passes. Preserve these errors for root review; they are separate from compute arithmetic correctness.
