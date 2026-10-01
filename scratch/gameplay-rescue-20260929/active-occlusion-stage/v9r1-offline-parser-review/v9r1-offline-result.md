# V9r1 offline reprocessing result

Execution integrity passed for the single bounded V9r1 observer run. The first parser result was inconclusive because it treated the per-grid `query_invalid` boolean as a fatal grid-wide error. Production sets this flag by boolean OR when any query has invalid input; it does not count invalid groups or invalidate independent queries. In frames 302 and 308, exactly one omni-parent (parent `515321385984`) produced six `unknown/query-invalid-input` faces and each grid flag was 1. Those two groups remain inconclusive; valid other groups in those grids are retained.

The corrected parser is a separate offline copy. It requires the `query_invalid` bit to match the presence of `query-invalid-input` FACE evidence, keeps affected unknown faces out of behind predictions, and preserves strict CPU/GPU identity, catalog, grid, and false-hidden checks. The original parser, original summary, and raw engine log remain unchanged.

The original report had 10/12 grids qualified and correlated 80 groups (72 zero, 8 nonzero, 16 incomplete). Offline reprocessing qualifies 12/12 grids and correlates 94 groups: 86 zero-sample, 8 nonzero-sample, and 2 incomplete groups. There are 0 false-hidden faces. Of the 86 complete GPU-zero groups, 28 had every CPU face predicted behind and 58 did not. The 8 nonzero groups were CPU `visible_or_unknown`. The corrected parser exits 2 because two groups remain inconclusive; this is not a clean complete-observations verdict.

All 12 grids report 1088 draw receipts, 29 selected/used draws, a 4096-triangle selected build, 1180 source pairs, 416 accepted paired unions, 2683 cells marked, and 32699 primitive-cell tests per frame. Across 12 frames the correct totals are 13056 receipts, 348 selected/used draws, 4418736 receipt triangles, 49152 selected triangles, 49152 rasterized candidate triangles, 14160 source pairs, 4992 accepted unions, 32196 marked cells, 392388 primitive-cell tests, and 4642 query-cell tests. Budget omissions total 12696 draw candidates; outside-viewport selection count is 12.

Grid CPU timings over 12 samples: build p50 4.180 ms, nearest-rank p95 4.309 ms; query p50 0.195 ms, nearest-rank p95 0.277 ms. These are isolated observer timings, not saved-time or FPS measurements.

V9r1 is accepted only as opt-in observe-only diagnostic infrastructure. Suppression/culling remains disabled, the observer defaults off, and static constant grouping remains off. No presented-gameplay correctness, culling effectiveness, or FPS claim follows from this run.

## Provenance

- Raw run: `<XRAY_SOURCE_ROOT>/scratch/gameplay-rescue-20260929/runs/v9-r1-active-occlusion-cordon-01/output`; engine log SHA-256 `cc57fd77ca82f32bf0a1af2bf33bb20798c0155bad5a13afc0f585c2506e6e89`.
- Original parser summary retained at `<XRAY_SOURCE_ROOT>/scratch/gameplay-rescue-20260929/runs/v9-r1-active-occlusion-cordon-01/output/observer-summary.json`; SHA-256 `46d57602fff8df85d1e693ff712e6b1ba112e75d93b17012dcaeeb31dd2d6f88`.
- Offline parser `<XRAY_SOURCE_ROOT>/scratch/gameplay-rescue-20260929/active-occlusion-stage/v9r1-offline-parser-review/summarize_observation_v9r1_offline.py` SHA-256 `0a591365604fd5163a069acaea649f365dc88b50144981b6788f9cf27dd321cf`; fixtures exit `0` with PASS marker and stdout SHA-256 `9f6a060f70a51b61746191fe8f2a9be69f8525c324dd89a52d24dfbb6505585e`.
- Corrected summary `<XRAY_SOURCE_ROOT>/scratch/gameplay-rescue-20260929/active-occlusion-stage/v9r1-offline-parser-review/reprocessed-v9-r1-output/observer-summary.json`; parser exit 2; SHA-256 `2a8a1f69978d6f0e9420ae0c31c141882aacef812b6ce9bbab41df5a9489ba83`.
- Live V9r1 source files: 47; exact manifest matches: True. Manifest SHA-256 `e1b3c9d037ac3d4662dd5a52ca4a0041629aa5e0afa291ab5b7262b53e4323fb`.
- Build/restore receipt `<XRAY_SOURCE_ROOT>/scratch/gameplay-rescue-20260929/active-occlusion-stage/v9r1-transaction/attempt-02/v9r1-build-restore-receipt.json` has status `PASS`. Current original app/bare executable SHA-256 `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`; retained V9r1 executable SHA-256 `f868820558752e6bf97f42eba0c9e7b2ed01b6c6114cf6a2ca030954ff4588ba`.
