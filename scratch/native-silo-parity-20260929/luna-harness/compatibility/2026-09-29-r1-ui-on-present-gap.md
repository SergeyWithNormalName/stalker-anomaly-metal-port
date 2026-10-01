# R1 UI-on-04 presentation gap diagnosis (offline)

Read-only review of `runs/r1-policy-ui-on-04/native/output` versus `runs/r1-policy-on-01/native/output`; no source, build, runtime, or GPU actions. Presentation lifecycle reviewed with `presenting-metal-drawables/SKILL.md`; implementation is Metal 3.

## Saved evidence

UI-on-04's trimmed selection is epoch 1790699269–1790699286. Its phase log has 416 contiguous engine callbacks, 24.43 FPS, and no engine-frame gaps. `summarize_run.py:62-84` computes displayed rate from `On-Glass Drawable Timeline` row count divided by the sum of each row's `Duration MCT Seconds`, not from callbacks. That cohort is 225 rows and 16.912 s coverage (13.304 rows/s), with on-glass dwell P95 204.17 ms, P99 287.50 ms, and max 379.17 ms. Every row is `Detached=false`; per-row `In-Flight Drawable Info.Command Buffer Count` is 181 rows of 1 and 44 rows of 0. The 44 zero-CB rows cover 3.079 s total.

R1 policy ON-01's trimmed cohort is 398 rows, all with command-buffer count 1, over 15.975 s (24.914 rows/s); dwell P95/P99 are 45.83/50.00 ms. The strong difference is specific to the UI/capture run, but does not by itself identify a cause.

The System Trace `Stats Timeline` exposes separate Presented and Skipped buckets. Summing buckets overlapping UI-on-04's window yields 240 presented-frame and 93 skipped-frame counts (CB counts 194 and 72), which does not reconcile with the 225 drawable rows. Treat these as bucket statistics, not unique counts, until aggregation/boundary semantics are verified. The saved evidence confirms a lower layer on-glass sample rate and many zero-CB associations; it does not prove that exactly 44 engine presentations were skipped, nor that the compositor alone delayed 44 submitted frames.

## Source attribution

`src/xrEngine/device.cpp:79-107,614-29` calls the renderer's void `Begin()`, then continues `seqRender` and `End()` whenever the device is active. `MetalRenderDeviceRender.mm:883-928` handles backend `BeginFrame` Retry without setting `frameActive`; `:942-44` then makes renderer `End()` return before scene submission. `MetalR4Backend.mm:1403-09` returns Retry if ordinary `nextDrawable` acquisition yields no drawable, releasing its frame semaphore. This is a real path for CPU callbacks without a command buffer or present, but UI-on-04 has no log/counter proving that path occurred.

On successful frames, `MetalR4Backend.mm:2413-27,2537-45` registers the presented handler, encodes `[commandBuffer presentDrawable:]`, commits, and clears `context.drawable`. The layer uses `displaySyncEnabled=YES` and `presentsWithTransaction=NO` at `:1231-32`. No drawable-retention or missing-normal-present defect is evident in this path.

E01 is not enabled in UI-on-04 (there is no `e01-frames.csv`). Its existing rows are written only from the submitted command buffer completion handler (`MetalR4Backend.mm:2399-55`) and therefore cover committed frames, but no ordinary BeginFrame no-drawable Retry/status row is emitted (`:1403-09`; only the forced diagnostic Retry logs). A future matched run can use E01 to align completed submits/presented times with callback frame IDs, but E01 alone cannot distinguish acquisition Retry from another unsubmitted-frame cause.

Focus evidence is also absent. SDL focus gained/lost is mapped to `Device.OnWM_Activate` in `src/xrEngine/Xr_input.cpp:1024-27`; `src/xrEngine/device.cpp:955-1006` updates `Device.b_is_Active`, with startup/always-active force paths at `:790,963`. The `render_device` Lua binding in `src/xrGame/script_render_device_script.cpp:65-95` exposes no active/focus field. No SDL input-focus field appears in the saved phase receipt, so CUA AX Raise and a captured full window do not prove NSApp activation or SDL input focus.

## Bounded next diagnostic

Repeat the same-policy short run. Use a fresh AX title-bar element to click and activate the game app/window (Raise alone may not activate NSApp); obtain only a pre-READY visibility/focus receipt, then reset/stop CUA and take no screenshots or UI actions during measurement. Start a full 60-second warm-up after that reset, then collect the same fixed-camera callback, E01, and Metal timeline data. Compare callback frame IDs with E01 submitted-frame rows and per-row command-buffer counts. If the gap vanishes, the prior result was tied to focus/capture context; if it persists, acquire-versus-submit attribution needs a minimal counter for ordinary `BeginFrame` Retry before any presenter change is proposed.

UI-on-04's screenshot ended at 16:27:34.209Z, READY was 16:27:45.614Z, and the measured selection began at 16:27:49Z. Thus no UI ran during measurement, but only about 14 seconds elapsed after the screenshot and 4 seconds after READY; the stated 60-second warm-up began before those CUA actions.
