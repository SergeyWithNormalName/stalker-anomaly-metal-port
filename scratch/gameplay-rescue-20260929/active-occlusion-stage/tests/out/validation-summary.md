# Active-occlusion CPU validation receipt

CPU-only scratch validation; no GPU, CMake, production source, app, or game activity.

## Frozen inputs

- Backend manifest: `draft/coverage-foundation-sha256.txt`, SHA-256 `effc4592644e0f6774b6c7eceaf9a63f59a222eb1a67c851a4c02aa99f09158f`.
- Header SHA-256 `436a7601d0afb4a2e1ef702f0835c909f292bf163b13d18143dc6a1e91320731`.
- Helper implementation SHA-256 `786ba433a3352c7b676dddd184c2f069308f3b9e6f6c707acebfb07faae477e4`.
- Structural input probe SHA-256 `441dcae256dbfb58c20f0645d24fee106a5d2135a546a0d72034371a34f4cb77`.
- Independent full-resolution fixture oracle SHA-256 `32495b58e2513579b7aa10ffd6d88e9e3ab7f81f62cfc41cc47e4faee545fe8d`.

## Results

Both O2 strict-FP probes passed, and both UBSan plus float-cast-overflow variants passed. Structural input probe reported `active-occlusion-coverage-input-probe=PASS failures=0`. The independent 128×128 fixture oracle reported `PASS cases_hidden=30 cases_visible=69 failures=0`; this is not a native 1920×1200 or Metal-equivalence result.

Required positive witnesses passed: one unpaired triangle query; three adjacent-pair seam queries (normal order, reversed order, reversed winding) each returned hidden and asserted `pairedUnionsAccepted > 0`. The deliberately cracked pair asserted rejected/not accepted and its seam query remained visible. Eight signed, shared-vertex-consistent perturbations on non-unit-W geometry retained full pixel-center/subpixel coverage and depth safety. Triangle-cap tests reached the triangle cap without the primitive-cell cap; a safe prefix remained useful while an unproven region stayed visible. Primitive-cell-cap tests retained only useful prefix coverage and left the unproven region visible. The query traversal stayed within its public 4096-cell bound; the cap guard itself is structurally unreachable through the clamped one-visit-per-cell API.

## Exact build commands

Run from the repository root:

```sh
/usr/bin/clang++ -std=c++17 -O2 -DNDEBUG -fno-fast-math -ffp-contract=off -I scratch/gameplay-rescue-20260929/active-occlusion-stage/draft scratch/gameplay-rescue-20260929/active-occlusion-stage/draft/tests/R4ActiveOcclusionCoverageInputProbe.cpp scratch/gameplay-rescue-20260929/active-occlusion-stage/draft/R4ActiveOcclusionCoverage.cpp -o scratch/gameplay-rescue-20260929/active-occlusion-stage/tests/out/R4ActiveOcclusionCoverageInputProbe
/usr/bin/clang++ -std=c++17 -O2 -DNDEBUG -fno-fast-math -ffp-contract=off -I scratch/gameplay-rescue-20260929/active-occlusion-stage/draft scratch/gameplay-rescue-20260929/active-occlusion-stage/tests/R4ActiveOcclusionCoverageOracleProbe.cpp scratch/gameplay-rescue-20260929/active-occlusion-stage/draft/R4ActiveOcclusionCoverage.cpp -o scratch/gameplay-rescue-20260929/active-occlusion-stage/tests/out/R4ActiveOcclusionCoverageOracleProbe
```

The sanitizer variants used the same sources and include path with `-std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all -fno-fast-math -ffp-contract=off`, producing `R4ActiveOcclusionCoverageInputProbe-ubsan` and `R4ActiveOcclusionCoverageOracleProbe-ubsan` in this directory.

## Preserved initial failures and final logs

The first structural command used the wrong manifest-relative path and failed before compilation; its log remains `input-o2-compile.log`. After correcting the path, structural O2 passed. The first oracle compile exposed a stale `reference()` parameter list; `oracle-o2-corrected-path-compile.log` preserves that failure. The signature-only repair was followed by a successful oracle run; the triangle-cap fixture was then narrowed so that the triangle cap, rather than the primitive-cell cap, is exercised, and final O2 plus sanitizer runs passed. See `input-o2-corrected-path-run.log`, `oracle-o2-triangle-cap-fix-run.log`, `input-ubsan-run.log`, and `oracle-ubsan-run.log`; per-command argv/exit receipts are adjacent JSON files.
