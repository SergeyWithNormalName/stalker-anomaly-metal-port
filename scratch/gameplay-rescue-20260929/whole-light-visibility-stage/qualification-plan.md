# Whole-light visibility observer qualification plan

Status: **PREP ONLY — no production integration, build, or game run authorized here.** The Mac's last read-only CUA state check reported locked. Numerical observation is permitted by the current scope; visual/motion acceptance remains locked out.

## Frozen-source integration gate

Wait for the final source-owner/root-reviewed manifest. Apply only the accepted patch after the central owner compares every modified source to `prestate/` and confirms every new destination is absent. Map draft sources as follows:

| Draft | Production destination |
|---|---|
| `draft/IR4Backend.h`, `draft/MetalR4Backend.mm`, `draft/R4WholeLightVisibilityLedger.h` | `src/Layers/xrRenderMetal/` |
| `draft/R4WholeLightVisibilitySelection.h/.cpp` | `src/Layers/xrRenderMetal/` |
| `draft/scene/MetalR4EngineScene.mm` | `src/Layers/xrRenderMetal/MetalR4EngineScene.mm` |
| `draft/adapter/MetalR4LocalLightAdapter.h/.cpp` | matching `src/Layers/xrRenderMetal/` paths |
| `draft/tests/*.cpp` | `src/Layers/xrRenderMetal/tests/` |
| final frozen draft CMake | `src/Layers/xrRenderMetal/CMakeLists.txt` |

Revalidate the final hash manifest after apply. Do not build a moving or mixed source state.

## One serialized qualification batch

1. Build the backend, whole-light selection CPU probe, ledger CPU probe, native whole-light visibility probe, and relevant existing backend regression probes using the normal focused renderer build and `-j4`. The final CMake must register `R4WholeLightVisibilitySelection.cpp/.h` in `XRay.Metal.R4Backend` and the selection probe/test; the early snapshot lacked that registration, though root reports the CMake has since been updated. Do not use a full unrelated test matrix.
2. Run CPU-only tests first: whole-light selection, whole-light ledger, and existing dynamic-world/compute-pose/bounds CPU guards if their shared headers/lifecycle change in the frozen patch.
3. Run native GPU tests sequentially with CTest `-j1`: `WholeLightVisibility.BackendGpu`, `FrameUploadGpu.SelfTest`, `PrivateUploadRecovery`, and `ComputeSkin.BackendGpu` because the backend implementation is shared. Keep the native visibility oracle strict: query and ordinary attachment arrays must match exactly for color, depth, and stencil; a stencil-equal witness must count while a rejected witness does not; abandoning an encoded query must leave no accepted receipt. A GPU test failure stops the gate; do not relax comparisons.
4. After the focused gates pass, build `Anomaly.DX11 -j4` once from the reviewed tree into a fresh V6 candidate copy (root has already authorized this; do not wait for duplicate approval). Record the candidate source manifest/executable SHA. The V6 manifest must include all accepted V5 source hashes and all 12 observer source files. Preserve V5 and V4. Save the current build-tree app recursively before the normal target build; after copying V6 out, restore the original build app and verify its file/symlink manifest plus executable SHA match the saved prestate. Never promote V6 into a shared launcher/default.
5. A single observer run is a separate explicit root authorization after build/test review. Use the runner in `observer-harness/`; it refuses to start without a reviewed candidate V6 hash, a fresh run ID, matching source manifest, and unchanged original/V4/V5 app hashes.

## One bounded observation run

Use one cloned `runs/v2-canonical-oracle-01/runtime`, its `rescue-cordon-control` save, the recorded Cordon corridor camera, clear2 / 08:03, Device factor 1 and world factor 6. Directly execute the candidate V6 binary; do not use LaunchServices. Cap the owned process at 175 seconds, hard outer maximum 180 seconds. Run no build, benchmark, screenshot, capture, E01/E07 timing, E08 GPU pass, or other GPU workload concurrently.

Environment: set `XRAY_METAL_WHOLE_LIGHT_VISIBILITY=1`, `XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME=300`, `XRAY_METAL_COMPUTE_SKIN=1`, and the already qualified corrected Flora depth/instancing shader paths. Leave compute-skin oracle, `XRAY_METAL_E08_GPU_PASSES`, E01/E07 timers, Metal validation/capture, local-light HOM diagnostics, and all other extra GPU timing flags **unset**. Set dynamic shadow index to its V5 value. The Lua observer stops after engine frame 311 has passed and allows a short GPU-completion drain; the backend itself must cap at 12 selected acquisitions, no more than 8 complete groups / 48 face records per acquisition.

## Receipt interpretation

Parse the engine log as bytes (the prior numeric runner's `awk` failure on non-UTF-8 host text is avoided by the included Python parser). Require run-integrity markers, game exit 0, no watchdog timeout, unchanged candidate hash, and no `FAILED_` harness marker. Preserve the raw log even if observer data are incomplete.

For each whole-light group:

- `status=complete`, all expected face records valid exactly once (one spot record or all six omni faces 0–5), shared submitted serial/acquisition epoch/frame, and matching selection/tile metadata means **complete observation**.
- A complete group with `zero_deferred_receiver_samples=true` is only **zero deferred receiver samples** for this production depth/stencil and this accumulation draw. It does not establish invisibility, absence of other consumers, or permission to cull.
- A complete group with `zero_deferred_receiver_samples=false` is a **nonzero deferred receiver sample**.
- Invalid/missing/duplicate faces, noncompleted receipts, selection without matching completion, capacity overflow, or acquisition-cap skip is **incomplete/inconclusive**, never zero.

Retain exact per-frame selection stats, IDs/face/batch/serial/epoch, and each selected tile's existing `static_rows`, `static_encoded_draws`, `dynamic_submitted_draws`, and `static_transform_bytes`. Correlate only records actually present in the logs. Do not sum nested times, infer missing work, calculate per-parent milliseconds, estimate savings, or turn zero samples into a culling decision. No FPS, gameplay performance, visual correctness, or motion claim follows from this observer.
