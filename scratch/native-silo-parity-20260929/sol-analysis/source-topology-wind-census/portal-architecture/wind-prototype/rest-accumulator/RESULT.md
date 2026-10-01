# Init-time rest-position margin prototype

`RestAccumulator.h` is a portable, init-only CPU accumulator for one Flora visual. It accepts each referenced local P3 float vertex, its already-decoded shader `tc.y`, the row-major array representation of the R4 column-major 4x4 model matrix, and the authored OGF sphere center/radius. It emits an outward float radius around that authored center, widened H extrema, and a widened maximum `abs(1-tc.y)`. Any unsupported numeric domain marks the visual unknown; callers must then keep it visible.

The matrix indexing matches the existing source extractor and adapter contract: world X/Y/Z are `m[0,4,8,12]`, `m[1,5,9,13]`, and `m[2,6,10,14]`; R4 stores translation at `[12,13,14]`. Evidence is pinned to `R4LevelGeometry.cpp` SHA-256 `1aff3bda26194596a0d855c19810349fa457b08625c7896f25935e839a3ba3f8`, `MetalR4FloraAdapter.cpp` `3cd57e6f84069698619358337d660bd6e159357ed455c3769ee5b584c47d58dc`, and the extractor `mesh_extrema.cpp` `43c2e4465b8953ed2fa08d5a06b6a87cd02f6182690e1fb724d68e3524fdd5d4`. The bound uses `gamma8` (`u=2^-24`) over each four-term dot-product absolute sum by default. It also includes the binary64 reference-sum error, H subtraction error, and an extra outward float ULP on emitted bounds. Signed S4 `tc.y` converts exactly to float and scales exactly by `1/1024`; only `1-tc.y` needs a float-rounding allowance.

## Required assumptions and limits

- The mathematical position input is `(x,y,z,1)`, matching the pinned `v_tree.P` comment in `common_iostructs.h`. The adapter source repacks `target.position[3]=1.0f` and declares a Float4 position attribute; the source contract is therefore explicit. This prototype still has no runtime vertex-fetch/shader ABI capture, so it makes no GPU-enclosure claim. `gamma8` is deliberately the default operation-count bound; it does not cover a fetch that supplies `w != 1`. A future `gamma6` reduction would require runtime proof plus an exact three-product-plus-three-add contract.
- The GPU error estimate assumes finite normal-range IEEE binary32 values, round-to-nearest arithmetic, and no unmodeled shader reassociation beyond the gamma bound. It does not silently tolerate denormals or flush-to-zero. Dot-product terms and all possible partial sums are checked against a 4× minimum-normal guard; an exact multi-term cancellation is admitted only for two exactly representable opposite products. Uncertain H cancellation near zero also fails open. Overflow and nonfinite values fail open.
- The CPU reference products of float32 inputs are exact in binary64; the four-term sum allowance uses `gamma3` for binary64 rounding. Bounds are widened again when converted to float. This is a numeric model, not proof of the compiled GPU instruction sequence.
- The center is the authored OGF sphere center and must be retained with a valid nonnegative sphere radius. `RestAccumulator.h` does not parse OGF metadata or integrate with a draw path; the separate `real_rest_bounds.cpp` qualification driver processes the pinned 5,316-visual input.

## Real indexed-geometry qualification

The separate `real_rest_bounds.cpp` driver reuses the previously validated `R4LevelGeometry` parser and exact frame-700 ID/sphere fixture; it adds only an accumulator call inside the existing unique-index vertex walk. It parsed 5,316 exact visual IDs from the active `l01_escape` inputs and processed **4,116,410 unique indexed vertices**. All IDs had the prior exact P3/C4/C4/C4/S4 layout and source-sphere match.

The helper returned 5,303 bounded visuals and 13 fail-open visuals. All 13 were rejected because the conservative H interval reached the near-zero/FTZ uncertainty zone (`h_near_zero_uncertain_ftz`): IDs 2554, 3702, 6402, 6538, 6677, 6746, 7158, 7714, 10540, 11141, 16815, 16873, and 16997. There were no nonfinite, overflow, dot-subnormal, dot-cancellation, or UV-near-zero failures.

For every known visual, the emitted radius contains the prior indexed rest radius, widened H interval contains the prior per-vertex transformed-H extrema, and UV maximum contains the prior `max(abs(1-tc.y))`; there were zero coverage failures across the 5,303 known rows. The smallest outward gaps were `1.49e-8` for radius, `3.35e-8` for minimum H, `8.34e-7` for maximum H, and `1.19e-7` for max-absolute UV. The 13 unknowns must stay visible.

The timed region measured **1,001.68 ms** across all 5,316 visuals. It includes indexed vertex reads, unique-index deduplication, existing rest/H/UV calculations, and accumulator calls; parser/file loading, fixture parse, output, and compilation are outside it. This is one-time initialization CPU work, not frame time or an FPS claim.

Inputs are pinned in `real-rest-bounds.summary.json`: `actual-frame-700.fixture` SHA-256 `7e64394fe35749e3980d1eb9d26870f7f93651b22bd1324ad6984f81476ae080`, `level` `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`, `level.geom` `a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a`, and prior extrema CSV `3d15d90548265cb9be3ec9b01c4e45ecfae4f240dada0a157346dee858f83605`. The parser source is SHA-256 `ee261880fd761340178c0de0b946ddc9abdb3a077fd798c9a2446f85100125e4`; parser header `ac6718bab0d3ed7a45f4754a6cb4f4ddb3a23d115fc78adb62b6fb782cba6a06`.

## Frame-700 wind and portal candidate follow-up

The new widened per-visual rows were passed to the existing `FloraWindEnvelope` with the pinned active-input current/previous frame-700 uniform snapshot, then into the existing same-camera owner-sector frusta reclassifier. The camera eye/view/projection words match the frozen frame fixture exactly. This is an analytical CPU candidate count only.

| Stage | Count |
|---|---:|
| Real indexed rest rows bounded | 5,303 |
| Real indexed rest rows fail open at uncertain near-zero H | 13 |
| Wind bound known after bender normalize/range guard | 5,290 |
| Wind bound unknown after rest + wind checks | 26 (13 rest, 13 bender) |
| Raw OGF owner-frusta exclusions | 4,427 |
| Excluded by widened rest + frame-700 wind envelope | 4,077 |
| Raw exclusions reopened | 350 |

The earlier rest-margin-free screen excluded 4,086 candidates; adding the indexed rest transform/H/UV margins reopens nine more. Unknown rest and wind rows remain candidates. These are per-frame geometry candidates: no source HOM, hierarchy, full-visible shortcut, source draw eligibility, GPU enclosure, future-frame motion, or FPS claim is established.

Receipts: real rest rows `cad960fb9664dac1b6d74010a1c11095b8a7929a491240e2ab94de6b9dd4afe1`; active motion CSV `41d4d32306706f54097be0c1f6411fadf53f45bae9ba3847af5c7c30d13c09c1`; active wind JSON `59fdd9315c49fe3c978a6dbae3dac22ed44f801f4fcf30dabff733631f56009e`; replay output `f2d900d1ffc1a7f74ecf20d533de76ceabc10cc86145e56b8b8d692390d5a7fb`; candidate classification `e0e9cef8de6bae76cac8f24c0ea684b28e3f0998f7ceb57dbb36be71ca795575`. The detailed classification counts and fail-open reasons are in `real-rest-active-wind-candidates.summary.json`.

## Synthetic verification

Standalone C++17 tests passed for large translation (error grows with matrix magnitude), a 90-degree column-major rotation, exact translation-only H cancellation, nonfinite input, subnormal/FTZ-domain fail-open, nontrivial near-zero H cancellation fail-open, transform overflow, and invalid authored sphere. Command:

```sh
clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic test.cpp -o rest_accumulator_test
./rest_accumulator_test
```

Output: `synthetic_cases=8/8 pass`. The real-geometry driver was also compiled as a standalone CPU parser/analysis utility only; no application, production source, game, or shader cook was built or run. No GPU enclosure, culling admission, FPS result, or temporal promise is claimed.

## Narrow runtime receipt that would resolve P3/W

Source shows `RepackR4FloraStaticVertices` writes `position[3]=1.0f`, and pipeline setup declares a Float4 position attribute from buffer 6 at offset 0 with stride `sizeof(R4FloraStaticVertex)`. This is a source contract, not a live binding proof. A compact observe-only receipt can close that gap without a full frame capture: record the selected Flora pipeline UID plus its frozen vertex reflection and descriptor (`position0` Float4, attribute 11/buffer 6/offset 0; vertex stride), then for each fixed visual record the draw's immutable vertex-slice identity/hash, local index-slice identity/hash and base/count, and a CPU check that byte 12 of every referenced 72-byte vertex is the exact float bit pattern for `1.0f`. Tie the receipt to the same pipeline and resource IDs used by that draw. If resource-slice identity or bytes are unavailable/ambiguous, retain fail-open. Gamma8 stays the bound; this receipt only validates the W fetch contract for the observed adapter path, not GPU arithmetic, temporal motion, or general culling safety.
