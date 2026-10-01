# Compute-skin adapter wiring and bounded native oracle result

Date: 2026-09-29 (local machine time, Asia/Novosibirsk).

## Frozen-source focused build

Command:

```sh
cmake --build build --target XRay.Metal.R4Backend XRay.Metal.DynamicWorldAdapterProbe XRay.Metal.ComputeSkinBoundsProbe XRay.Metal.ComputeSkinPoseJournalProbe XRay.Metal.ComputeSkinBackendGpuProbe XRay.Metal.FrameUploadGpuProbe -j4
```

Result: passed. One preceding attempt failed at adapter oracle’s four-argument internal `repackR4DynamicWorldSkinnedVertices` call; the adapter owner changed it to the existing exported wrapper, froze the new adapter hash, and the retry passed. Logs and before/after production-source hashes are retained beside this file.

CPU CTests ran from the renderer build directory and passed 3/3:

```sh
ctest --test-dir build/src/Layers/xrRenderMetal -j1 -R '^(XRay\.Metal\.DynamicWorldAdapter\.SelfTest|XRay\.Metal\.ComputeSkin\.Bounds|XRay\.Metal\.ComputeSkinPoseJournal\.SelfTest)$' --output-on-failure
```

## Scratch actual-adapter mock

`R4ComputeSkinAdapterWiringProbe.cpp` compiled and linked against the just-built `XRay.Metal.R4Backend` and existing `XRay.Core` static archives, then ran on CPU. It invoked the real adapter’s `Configure`, `Prewarm`, `Draw`, `CommitSubmittedSkin`, and `DiscardUnsubmittedFrame` using qualified installed-content snapshots:

- 1W: `model-02-root-1.r4skin`, 123 vertices, 60-byte source stride.
- 2W: `model-00-root-2.r4skin`, 167 vertices, 64-byte source stride.

`Configure` used the actual `build/metal-runtime-shaders/dynamic-world` receipt bundle and adjacent reflections. The mock supplied only a minimal valid DXT1 DDS fixture. Compute jobs were deferred until fake `SubmitFrame`; discard removed queued commands. The probe passed: initial submit/commit and slot cache hit, three-slot wrap with repeated engine frame, B discard and redispatch, same-key B then committed A cancellation, synchronous pre-dispatch false causing full CPU fallback and adapter-lifetime latch-off, draw-buffer ownership, and two-sample/per-weight-class oracle quota with discard/cancellation not consuming quota. Oracle CPU bytes were checked against the legacy `RepackR4DynamicWorldSkinnedVertices` output, exact owned request size, and 4 MiB cap.

Result line:

```text
ADAPTER_WIRING PASS actual Prewarm/Draw/CommitSubmittedSkin/DiscardUnsubmittedFrame; qualified 1W/2W; submit/cache/discard/re-dispatch/cancel/retry/latch/slot-wrap/repeated-engine-frame; oracle quota/discard/cancel<=4MiB
```

The fake output bytes are pose-tagged and deliberately do not claim shader arithmetic correctness. This mock is CPU transaction/wiring proof only.

## Bounded native GPU regressions

One serialized CTest batch (`-j1`) passed 3/3:

```sh
ctest --test-dir build/src/Layers/xrRenderMetal -j1 -R '^(XRay\.Metal\.ComputeSkin\.BackendGpu|XRay\.Metal\.FrameUploadGpu\.SelfTest|XRay\.Metal\.PrivateUploadRecovery)$' --output-on-failure
```

- `FrameUploadGpu.SelfTest`: PASS; E02 arena test and existing E04 reprojection check passed.
- `PrivateUploadRecovery`: PASS; exercised the existing repeated-abort recovery stop.
- `ComputeSkin.BackendGpu`: PASS. Actual production MSL produced exact 36/36-byte positive 1W output (`mismatch_bytes=0`). The deliberately one-bit-negative oracle correctly reported `status=FAIL`, one mismatch at byte 0, and the encompassing probe reported `COMPUTE_SKIN_BACKEND_GPU_PROBE status=PASS`. This is expected negative-oracle evidence, not a failing test.

These GPU probes do not exercise a presented game frame, forced scene submit retry, or gameplay/visual acceptance. No application candidate or game had been built or launched at the time of those probe results.

## V5 application candidate build

After root accepted the focused gates, the normal required application target completed:

```sh
cmake --build build --target Anomaly.DX11 -j4
```

Ninja completed 231/231 steps. The linked V5 executable SHA-256 is `5fd0bde4a9d1662fd51ec332e90957caa7984726d20415966de5679412afc44f`; it is preserved at `scratch/gameplay-rescue-20260929/candidate-v5/Anomaly.app`. The candidate source manifest is `scratch/gameplay-rescue-20260929/candidate-v5/source-manifest.json`.

Before the build, the original app bundle was copied to `compute-adapter-wiring/prestate/Anomaly-baseline.app` and verified as `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`. The existing V4 candidate still hashes to `2bb3dc6b4b35a805a991792864c98f40fb7df128f8431da306d731a88d517f71`. After copying the built V5 bundle out, both the build-tree app bundle and standalone `AnomalyDX11` were restored from the original snapshot. A recursive file/symlink manifest confirms exact equality between the saved and restored original bundles; receipt: `out/app-restore-verification.json`.

V5 was not launched. No save, installed profile, mod files, or shared launcher was modified. The locked-display condition still blocks GUI/visual/motion acceptance; the numeric-only real-game oracle remains separately gated for review.
