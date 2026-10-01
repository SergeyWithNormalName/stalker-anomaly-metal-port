# V7 central validation and app-copy plan

Preparation only. No production source was applied, no build was started, and the existing original/V4/V5/V6 app bundles were not changed. Execute only after root accepts one frozen combined integration/correlation/CMake manifest.

## Scratch CMake proposal

`CMakeLists.v7.patch` is a scratch-only delta against the current V6 CMake prestate SHA-256 `ef6c1002abe825fa1ff284743f7eaae916369e723ebcf8f0f611a516330fa872`. `git apply --check` passed without applying it. The proposal adds the coverage and scene-bridge translation units once to `XRay.Metal.R4Backend`, and adds three bounded CPU probes: `XRay.Metal.ActiveOcclusionCoverageInputProbe`, `XRay.Metal.ActiveOcclusionCoverageOracleProbe`, and `XRay.Metal.ActiveOcclusionSceneBridgeProbe`. Strict floating-point flags are source-scoped to the two arithmetic helpers and their three probe translation units: `-fno-fast-math -fno-finite-math-only -ffp-contract=off`. It does not add or repeat the standalone native Metal pixel probe.

The integration map is expected to place the bridge and coverage source/headers in `src/Layers/xrRenderMetal/`, the scene-bridge probe under `tests/`, and the already-qualified independent CPU coverage oracle under `tests/`. The scene `IR4Backend.h` must not replace the correlation author's version; the frozen combined manifest must merge that one header with the matching backend/ledger changes. Reconfirm the final source paths and probe target names against the combined freeze before applying anything.

## Focused probe transaction

After freeze, first verify all current production prestate hashes and new-path absence against the author's combined manifest. Snapshot the complete current original app bundle and bare `build/Binaries/RelWithDebInfo/AnomalyDX11` into a new V7 transaction directory using `ditto`; record recursive file/type/symlink/mode/hash inventory and verify the original app executable remains SHA-256 `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`. Preserve V4 `2bb3dc6b4b35a805a991792864c98f40fb7df128f8431da306d731a88d517f71`, V5 `5fd0bde4a9d1662fd51ec332e90957caa7984726d20415966de5679412afc44f`, and V6 `238db09155f62caf726f53917cf82876aa12b06ee60c8ea03d157b24731c26dd` before/after.

Apply only the reviewed mapping after all prestate hashes match. Build the bounded test targets in one command:

```sh
cmake --build build --target \
  XRay.Metal.ActiveOcclusionCoverageInputProbe \
  XRay.Metal.ActiveOcclusionCoverageOracleProbe \
  XRay.Metal.ActiveOcclusionSceneBridgeProbe \
  XRay.Metal.WholeLightVisibilityLedgerProbe \
  XRay.Metal.WholeLightVisibilitySelectionProbe \
  XRay.Metal.WholeLightVisibilityBackendGpuProbe \
  XRay.Metal.LightHomBoundsProbe \
  XRay.Metal.LocalLightFaceVisibilityProbe \
  XRay.Metal.LocalLightFoundationProbe \
  XRay.Metal.LocalLightAdapterProbe -j4
```

Run CPU fixtures serially before touching the one GPU test slot:

```sh
ctest --test-dir build --output-on-failure --parallel 1 \
  -R '^(XRay\.Metal\.ActiveOcclusion\.CoverageInput|XRay\.Metal\.ActiveOcclusion\.CoverageOracle|XRay\.Metal\.ActiveOcclusionSceneBridge\.SelfTest|XRay\.Metal\.WholeLightVisibilityLedger\.SelfTest|XRay\.Metal\.WholeLightVisibilitySelection\.SelfTest|XRay\.Metal\.LightHomBounds\.SelfTest|XRay\.Metal\.LocalLightFaceVisibility\.SelfTest|XRay\.Metal\.LocalLightFoundation\.SelfTest|XRay\.Metal\.LocalLightAdapter\.SelfTest)$'
```

Only after all CPU tests pass, run the scope-aware ledger/backend GPU regression once, with no other GPU workload concurrent:

```sh
ctest --test-dir build --output-on-failure --parallel 1 \
  -R '^XRay\.Metal\.WholeLightVisibility\.BackendGpu$'
```

Do not rerun the already-qualified standalone native pixel probe. If a focused target fails, preserve the exact command/log/hash and stop for a mechanical owner fix plus new freeze; do not broaden the test matrix.

## Candidate V7 copy and recursive restore

Only after all focused probes pass, build just:

```sh
cmake --build build --target Anomaly.DX11 -j4
```

Copy the complete post-build `build/Binaries/RelWithDebInfo/Anomaly.app` to `scratch/gameplay-rescue-20260929/candidate-v7/Anomaly.app` with `ditto`, then verify the candidate executable hash and write a manifest containing the exact frozen source hashes, probe results, build command/exit, candidate bundle path, and executable hash. Do not launch the candidate.

Restore the original app from the transaction's pre-build bundle snapshot via a sibling restore-temp bundle: copy snapshot to `Anomaly.app.restore-tmp`, verify its recursive inventory, move the build-produced `Anomaly.app` aside only after candidate V7 is sealed, then atomically rename the restore-temp bundle back to `Anomaly.app`. Compare the restored recursive inventory, symlinks, modes, and executable SHA-256 against the prestate; separately compare the bare build executable and restore it from its own snapshot only if it changed. Verify V4/V5/V6 and original hashes again and emit a restore receipt. This follows the prior V6 `V6_BUILD_COPY_RESTORE_PASS` evidence while adding a fresh transaction-local full-bundle prestate. Candidate game launch remains a separate root decision after build/source receipt review.

## Current evidence and open freeze data

- V6 source/CMake prestate: candidate manifest SHA `0e11d7a0b31c81106512de202a5bd36732c9128eb0a35d09bce9560361172c03`; CMake file SHA above.
- Existing V6 build command: `cmake --build build --target Anomaly.DX11 -j4`.
- Existing V6 recursive restore receipt: `scratch/gameplay-rescue-20260929/whole-light-visibility-stage/out/v6-build-restore-receipt.json`.
- Existing original app transaction snapshot: `scratch/gameplay-rescue-20260929/compute-adapter-wiring/prestate/Anomaly-baseline.app`; restored bundle inventory verified equal and executable SHA verified.
- Integration bridge source has a prior numeric-review note requiring outward rounding for each absolute-dot-term sum. Verify the final frozen `R4ActiveOcclusionSceneBridge.cpp` includes this correction before source admission.
- Backend currently has no final accepted CMake registration delta; the combined freeze must provide target/test names and hashes. The bridge author proposed CTest name `XRay.Metal.ActiveOcclusionSceneBridge.SelfTest` and strict flags adding `-fno-finite-math-only`; this scratch proposal uses those names/flags.
