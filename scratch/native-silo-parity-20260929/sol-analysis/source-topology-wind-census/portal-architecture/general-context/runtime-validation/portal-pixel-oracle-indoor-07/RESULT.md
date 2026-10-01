# Main Flora portal pixel-oracle review

The independent readback review passes for both captured frame-700 poses. In each run, all 12 GPU readback checks completed: four pre-Flora MRT copy identities, four source-vs-candidate MRT comparisons, the pre-Flora D32S8 copy identity, the source-vs-candidate D32S8 comparison, and nonblank color/depth delta checks. All four MRTs and D32S8 are bitwise identical between the source-order and portal-candidate branches. The saved 8-bit RGB PPMs also match for every MRT. The source Flora draw produces a nonblank position/depth delta against the pre-Flora base.

| Captured pose | Source draws | Candidate draws | Actual post-clip skips | Prepared exclusions | Nonblank Flora delta | Observer receipt |
|---|---:|---:|---:|---:|---:|---|
| Indoor, sector 25 | 5,316 | 1,261 | 4,055 | 10,372 | 169 pixels | Complete for the pinned 5,316-ID capture; independent observer validator passes |
| Outdoor, sector 30 | 5,308 | 5,300 | 8 | 6,207 | 1,032,130 pixels | Incomplete under the observer's pinned 5,316-ID capture contract; no observer-complete claim |

Both runs used the same prepared population of 11,663 Flora draws. Ordinary camera clipping removed 6,347 indoors and 6,355 outdoors. The portal candidate omitted 4,055 camera-visible draws indoors but only 8 outdoors. Unknown wind records stayed visible (40 indoors, 32 outdoors); owner-frustum uncertainty stayed visible for eight records indoors and none outdoors. Source-order full-transform matching is true in both pair receipts. The paired output comparison is therefore meaningful at each recorded pose, but does not imply that the candidate has comparable value in other views.

The outdoor pixel pair passes even though its separate source-topology ID observer is not complete. It reports 5,308 joined IDs and zero unknown joins, with the current view, transform, SSA, and suppression fields consistent. Its completeness flag is false because the observer/validator still pins the captured ID population to 5,316: `validate_runtime_portal_receipt.py` sets `EXPECTED_IDS = 5316` and rejects another footer; the stage06 capture code also requires `encode_complete,5316` and exactly 5,316 IDs. The outdoor file ends with `encode_complete,5308`, matching the 5,308 source draws. The generated observer-validation artifact is explicitly `passed=false`. This does not invalidate the pixel oracle's separate frame-matched graph/rest candidate pair, but it prevents claiming a complete observer receipt for that outdoor capture.

The indoor pair demonstrates a large draw omission opportunity for this indoor pose; the outdoor pair shows almost no post-camera-clip draw reduction. These are fixed-pose correctness results, not an FPS measurement. No general view coverage, global culling safety, shipping admission, or performance claim is made.

## Reproduction artifacts

- `validate_pixel_pair.py` validates all 12 readback reports, exact MRT/depth/stencil equality, the nonblank delta, branch draw accounting, matching RGB PPMs, process exit, and frame-scoped observer fields. It emits the per-run JSON receipts below.
- `pixel-pair-validation.json` records the indoor run's passed pixel oracle and the separately passing observer receipt.
- `outdoor-pixel-pair-validation.json` records the outdoor run's passed pixel oracle and separately records `source_observer_status: incomplete` and `source_observer_validator_passed: false`.
- `source-observer-validation.json` is the independent indoor observer validation. `../portal-pixel-oracle-outdoor-07-observer-validation.json` records the expected failure of the pinned observer validator on the 5,308-ID outdoor capture.

Both runs used the same candidate executable (`5ae8a00337b33e38484f7976244e30cd9393cc7a36c48429aba24e7bb9b88c42`) and source manifest (`d46282849982be012f405d99894e7d7be756efddc59f550857f9e1cb58764323`). The validation JSON files include SHA-256 values for these files plus every pixel-pair receipt, source topology, active-reader bytes, ID capture, E01 rows, run metadata, and compared PPMs. They make no FPS or general-safety claim.

Review artifact hashes:

- `validate_pixel_pair.py`: `7d59421afe622353713c108b413b92b9c78f6f2ee641b39c4353b2cec7bc76b3`
- Indoor pixel validation: `3be40d2e751d94b1acbe471f27f8037ff1fd73416479c4941cacdd9aacba7523`
- Outdoor pixel validation: `e587c4028afe72dd484cbbdf59493a215bf03cd72d523a93d3503299a5fe04b1`
- Indoor source-observer validation: `b7bd77bdc3a9a85227778ee2c883f2b4f545cc736f60d5266f8d8820fbb43000`
- Outdoor pinned-validator failure receipt: `dc5346dd6b883f154b3a8ef4fb2f1296e442567e0ebfe24c44cf24088b0cc38f`
