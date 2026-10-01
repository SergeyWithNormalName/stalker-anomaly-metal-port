# Runtime portal receipt validator

`validate_runtime_portal_receipt.py` is a read-only, fail-closed checker for one frame-700 observer run. It does not build, launch, or modify the game. The intended run directory contains:

```text
run-start.json
source-topology.json
main-flora-ids/main-flora-ids-frame-700.csv
active-engine-level.bin
engine.log
e01-frames.csv
```

The run directory must be fresh. The validator uses `run-start.json` (`portal-runtime-run-start/v1`) when present; otherwise it requires the run wrapper's `launch-epoch.txt` marker and rejects core outputs whose modification times predate launch. Inputs may not be missing or symlinked, except that the runtime `level.geom` symlink is resolved and hashed. The engine log's latest `METAL_PORTAL_ACTIVE_READER` record must report a successful copy whose generation and byte count match the receipt and captured level file.

The validator pins the independent C++ per-ID CSV to SHA-256 `f882ab4e87da7fc8a384e615b703e9e8cceb54e69e50946fb3ea66935cf3397f`, then checks all 5,316 unique IDs, materials, raw visibility flags, and owner-frustum test counts. It requires start sector 25, six nonempty reached sectors, eight frusta, 889 visible rows, complete structural geometry joins, and `suppressed=0`. It compares CSV view, projection, eye, and SSA bit arrays with their receipt counterparts.

The observer receipt does not currently emit raw `ssa_settings_bits` or `level_geom_sha256`. It does report `csv_ssa_match=true`, which the C++ observer computes by comparing all seven captured SSA bit values internally; the validator records that assertion but lists external SSA recheck as unknown. The runtime geometry symlink currently hashes to the pinned level.geom bytes, but the receipt does not bind that hash to the generation cache, so that link remains unknown. The E01 record is separately required to contain exactly one completed engine frame 700 with serial 700.

Passing this check would validate source-observer receipt integrity against the pinned offline C++ replay. It would not establish GPU culling correctness, visual equivalence, shipping admission, or FPS benefit.

Example after a successful fresh run:

```sh
python3 validate_runtime_portal_receipt.py /path/to/fresh/run \
  --emit /path/to/reports/portal-validation.json
```
