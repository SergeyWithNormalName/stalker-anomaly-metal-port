# Cordon load freeze and reboot: macOS evidence

Collected read-only from the host after the 2026-09-27 incident. No game was launched for this inspection and no OS settings were changed. Timestamps below are local time (`+0700`). The panic report has been retired by macOS since collection; its current path is recorded below.

## Event sequence

1. macOS recorded a kernel panic at **23:27:21**. The panic header identifies `watchdog timeout: no checkins from watchdogd in 90 seconds` (`/Library/Logs/DiagnosticReports/Retired/panic-full-2026-09-27-232721.0002.panic`, first metadata line and `panicString`, line 8).
2. The accompanying reset counter at **23:27:26** records `Reset count: 1` and `Boot faults: wdog,reset_in1` (`/Library/Logs/DiagnosticReports/ResetCounter-2026-09-27-232726.diag`, lines 5–7). This confirms a watchdog-triggered reset/reboot, rather than an ordinary app exit.
3. In the panic stackshot, the `AnomalyDX11` process had been running for 293 seconds, putting its approximate start near **23:22:28**, about five minutes before the panic. It was the largest resident process in the captured process list.
4. A separate `AnomalyDX11` crash report starts at **23:27:29**, after the reset, and is captured at **23:27:56** (`<PRIVATE_LOCAL_PATH>`, lines 15 and 21). It belongs to the post-reboot launch and did not cause the preceding reset.

## Panic and resource measurements

The panic was a watchdog timeout. Its kernel backtrace names `AppleARMWatchdogTimer` and `AppleInterruptControllerV2`; it does not contain an AGX/Apple GPU driver frame (`panic-full…panic`, line 8). The report states `100% of segments limit (BAD)`, `28 swapfiles`, and `LOW swap space`. Its stackshot memory snapshot reports 753,595 wired pages and 906 free pages at 16 KiB per page, approximately 12.35 GB wired and 14 MiB free. The same snapshot's `memoryPressure` boolean is `false`; this does not negate the explicit segment-limit and low-swap indications (`panic-full…panic`, lines 8 and 25).

The `AnomalyDX11` entry in `processByPid` (line 533) reports PID 81509 with `residentMemoryBytes: 18107676880` (about 16.85 GiB), 149,501 page-ins, and process uptime 293.08 seconds. The app had 33 threads. Its `X-RAY Primary thread` had not run for about 116.44 seconds before capture; at capture it and several other app threads were waiting on semaphores owned by PID 81509. The process accumulated only about 49.62 seconds of user CPU and 31.28 seconds of system CPU during its 293-second lifetime. Together, this is strong evidence of a multi-minute app stall alongside extreme app memory growth and system-wide resource exhaustion.

## Causal assessment and limits

The evidence establishes a watchdog panic/reboot while the game was running, with the game using about 16.85 GiB resident and system memory under severe swap/segment pressure. It rules against interpreting the event as a normal thermal shutdown or clean power-off. The saved panic backtrace does not identify a GPU driver panic, and this report alone cannot prove whether the pink flash was the macOS panic UI, a display/GPU artifact during reset, or what code path directly caused the process memory to grow.

Renderer investigation should start at the in-session telemetry for the failing load: repeated scene recreation after a missing volumetric-shadow record and the rapidly growing `METAL_PRIVATE_UPLOAD_RETRY` count. Check why the missing record triggers full-scene creation each frame, whether each failed upload attempt retains CPU/GPU backing resources, and whether retry work is bounded and reused across frames. The host panic supplies strong circumstantial evidence for an unbounded resource-growth path, but does not label that specific Metal resource as the owner of the 16.85 GiB.

## Separate post-reboot app crash

The post-reboot `.ips` reports `EXC_BAD_ACCESS` at address `0x8` while executing `CScriptStorage::print_stack()` on the main thread. The stack reaches it during error reporting for `CLocatorAPI::exist` called by `CPatternFunction::vfLoadEF`, then `CEF_Storage::CEF_Storage()` and `CAI_Space::init()` (`AnomalyDX11-2026-09-27-232806.ips`, line 50). This is a separate startup failure after reboot, apparently while reporting a missing file; it is not evidence for the earlier watchdog cause. Its crash reporter itself dereferences null while collecting the error stack, masking the original missing-file diagnostic.

## Earlier reset record

A read-only search of `/Library/Logs/DiagnosticReports`, its `Retired` directory, `<PRIVATE_LOCAL_PATH>` and its `Retired` directory, plus `/private/var/db`, found only one retained `panic-full` report: the September 27 panic above. macOS's `.contents.panic` index also points only to that same report. The older retained reset evidence is:

- `/Library/Logs/DiagnosticReports/ResetCounter-2026-09-23-074101.diag` records **Reset count: 1** and **Boot faults: wdog,reset_in1** at 07:41:01. This classifies that earlier restart as watchdog-triggered too, but the matching `panic-full` report is absent, so its panic reason, game involvement, memory state, and GPU state cannot be established.
- `/Library/Logs/DiagnosticReports/DumpPanic_2026-09-23-074052_MacBook-Pro-Sereza.cpu_resource.diag` is a CPU resource report for the macOS `DumpPanic` utility, not the missing panic log. It records **ThermalPressure: 0**, **16 GB** memory, and operation on **AC power** in its sampled interval (lines 1–44 and 78–94). Its separate `kernel_task` sample includes one `AGXFirmware::drainFirmwareRings` frame (lines 153–203); this is a sampled kernel stack in a CPU report, not a kernel panic backtrace or evidence that AGX caused the reset. The report does not list `AnomalyDX11` as one of its sampled processes.

Thus, the retained evidence shows at least two watchdog resets (September 23 and September 27). The September 27 event is strongly associated with the game consuming nearly all available memory; the September 23 event cannot be attributed to the game or to GPU/memory failure from the artifacts still present. No retained report establishes a distinct GPU panic or a thermal/power shutdown for either event.

## Identified port defects and repairs

The original runtime logs were preserved in `scratch/cordon-watchdog-20260927/original-engine.log` and `original-stderr.log` before test launches. The engine log first publishes a valid environment at frame 2562, prepares `l01_escape`, and then attempts gameplay weather/rendering at frame 2592 while loading phases are still in progress. `CApplication::LoadDraw` (`src/xrEngine/x_ray.cpp`) increments `Device.dwFrame` and calls the renderer without running the normal world/environment update in `CRenderDevice::on_idle` (`src/xrEngine/device.cpp`). The prior Metal admission checked only the existence of a level/prepared scene. Thus loading frames could feed new local-shadow records to a volumetric consumer holding an older environment frame, causing the reported `volumetric pass has no matching rendered local shadow record` failure. The original composite error did not print individual stamps; the source ordering and saved phase/frame sequence identify the loading transition defect, and new diagnostics include the exact mismatched fields.

After each rejected render, private-upload closures retained native destinations and staging. The next frame replayed all earlier uploads, and scene producers reissued their uncommitted initial data. The saved stderr journal grew by 5,907 records per attempt, reaching 88,735 records. This is a concrete unbounded-retention defect consistent with the panic's application footprint.

Repairs:

- Loading admission now respects `g_appLoaded`, queued loading events, level readiness, and current camera/environment stamps. Loading frames submit clear/UI work without gameplay resource producers. Gameplay submission additionally requires current world/lighting snapshots.
- The volumetric consumer checks source eligibility before requiring shadow metadata for an effect that is disabled. Eligible effects still reject inconsistent metadata and print their exact identity/frame/face/depth/atlas values.
- The upload journal tracks destination owners weakly, removes uploads for discarded assets, and coalesces repeated exact writes to a live destination while preserving write order. A live texture view retains its source wrapper so valid parent uploads survive a retry.
- Three consecutive aborted upload-bearing frames terminate recovery and release the journal. The renderer also stops after three consecutive scene failures, retaining the original error, rather than resetting/reallocating indefinitely.

Metal retains encoded resources in ordinary command buffers; journal pruning happens after abandoning the command buffer and its temporary resource holds. Reference: [Apple command buffer retention documentation](https://developer.apple.com/documentation/metal/mtlcommandbufferdescriptor/retainedreferences).

## Verification

Three focused tests passed with `MTL_DEBUG_LAYER=1`: `XRay.Metal.FrameUploadGpu.SelfTest`, `XRay.Metal.PrivateUploadRecovery`, and `XRay.Metal.VolumetricCapture.SelfTest` (2.65 seconds total). The recovery probe uses tiny offscreen resources and tests actual GPU readback of the newest replayed write, dead-owner pruning, texture-view ownership, and the third-abort stop. The capture probe tests loading/stale-frame admission. The full New Game runtime verification is recorded below.

### First guarded New Game run

The rebuilt binary created a new stalker character at `rookie_village` / `l01_escape` in an isolated runtime. It exited normally (exit 0) after 75.74 seconds; peak sampled process RSS was 3.033 GiB, dropping to about 1.1 GiB after loading. Neither the 5 GiB guard nor the 180-second timeout fired. The frame CSV records 102 completed gameplay frames. There were no `Render production R4 scene failed`, private-upload retry, or recovery-stop markers.

The first diagnostic script's `printf` markers were suppressed by the effective GAMMA configuration, so the scripted actor-update duration was not admitted from this run alone. Its original summary remains `passed=false` for missing instrumentation, despite normal gameplay progress and clean exit. The follow-up run uses a separate durable actor report and leaves the first logs/CSV/summary unchanged.

### Final guarded New Game run

The final executable and DEV `Anomaly.app` bundle are synchronized (`cmp` passed); Ninja reports no pending executable work. Three focused tests passed on this final build with Metal API Validation (1.71 seconds total).

The second isolated new stalker game at Cordon completed normally with exit 0 in 68.62 seconds. The actor-side durable report is `CORDON_SAFETY_WORLD_PASS level=l01_escape ticks=102 seconds=20`. It confirms at least 20 seconds of actor updates after gameplay began; the frame CSV contains 103 completed gameplay frames. Peak sampled process RSS was 2.74 GiB, versus about 16.85 GiB in the original panic's game entry. No guard fired, and no scene-error, upload-retry, or recovery-stop message occurred.

Artifacts: `scratch/cordon-watchdog-20260927/new-game-summary.json`, `actor-report.txt`, `new-game.log`, `frames.csv`, and `final-probes.log`. Test settings, configuration writes and saves were isolated from the player's profile. The shader bundle and ordinary quality settings were used.

This validates the previously failing New Game/Cordon transition and a short gameplay interval; it is not a long-session stability claim or a Windows visual-parity acceptance. The September 23 watchdog's cause remains unknown because its full panic report is no longer retained.
