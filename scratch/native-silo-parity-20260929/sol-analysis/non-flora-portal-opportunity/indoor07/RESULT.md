# Indoor07 LMH source-family portal census

## Result

Using the exact indoor07 frame-700 observer receipt, the offline source census found **2,185** shader-slot-2 `lmap*` visuals in the winning level. All 2,185 have valid indexed POSITION bounds and a known sector-owner list. Under the captured main-camera transform, the conservative ordinary clip leaves **1,205** candidates. Testing each transformed indexed AABB against every reached frustum for every owner rejects **908** of those candidates; **297** still intersect a reached owner frustum.

This is a **source-family upper bound**, not an actual prepared LMH draw skip count. The run receipt does not include the runtime `textureMaterialByVisual` membership or per-visual pipeline-admission list. A current log sample reports 1,147 LMH submissions, but its `frame=900` is a family telemetry counter, not a joined engine-frame record; do not use it to scale or subtract from this census. No Windows count, image quality, FPS, or shipping-admission claim follows from 908.

## Inputs and integrity gates

The census used `runs/portal-pixel-oracle-indoor-07/native/output/source-topology.json`, not the later fast10 run. It pins generation 1/frame 700, live and saved eyes, view, projection, full-transform and SSA-setting bits, start sector 25, no suppression, and active-source hashes. The independent runtime validator passes all 45 frame/source/view checks with no unknown fields; it confirms six reached sectors and eight frusta.

- Active engine-winning level bytes SHA-256: `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`
- Winning `level.geom` SHA-256: `a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a`
- Observer receipt SHA-256: `7071f33ac28ff3c3b0a0a4918397eeb50ceac86dcbdaccd9b3bda6ff4069fa5f`
- Runtime validator SHA-256: `b7bd77bdc3a9a85227778ee2c883f2b4f545cc736f60d5266f8d8820fbb43000`

## Method

A small standalone CPU emitter compiled the stage10 portable `R4LevelGeometry` parser. It read the winning `level.geom` and copied active `level` bytes, parsed sector/portal/root metadata, and emitted every valid static visual whose third shader texture begins with `lmap`, including the indexed POSITION bounds, model transform, dense source visual ID, and all owner sectors. This deliberately uses a broader source-family predicate than the runtime prepared `textureMaterialByVisual` set.

The camera test follows the production conservative eight-corner Metal clip check, using the exact captured full-transform bits and source model matrices; uncertain numeric boundaries stay visible. For portal tests, the census uses the captured C++ source frustum plane values. It applies the rejecting half of `CFrustum::AABB_OverlapPlane` (negative support point) with a small fail-open tolerance, to transformed indexed geometry AABBs. It retains a visual if any frustum of any owner intersects. It ignores the sphere early-out, which can only keep additional candidates here. Unknown ownership or malformed bounds remain visible.

## Artifacts

- [Per-visual census CSV](lmh-indoor07-opportunity-upperbound.csv): candidate ID, shader/source class, owner sectors, ordinary-camera result, and portal upper-bound result.
- [Summary JSON](lmh-indoor07-opportunity-summary.json): pinned hashes, gates, counts, exact excluded visual IDs, and interpretation.
- `emit_lmh_source_upperbound.cpp` and `classify_lmh_upperbound.py` reproduce the parse and classification from the two pinned inputs. The parser emitter is standalone and does not link or alter engine code.

SHA-256 receipts:

- staged parser source `R4LevelGeometry.cpp`: `c7cd61b4da497dcf53cc5a19205c0daab52d6180568d0c1a88ba5a323c5d0349`
- emitter source: `c40e656034d43905069c013557517bbfc02a47f0bd0bb4f5d2e8df702b4e1c48`
- emitter executable: `3ac13e4e03f6766780e9969879b53c341d971795f63c72de140a20775c33cba4`
- census script: `830bb00a8e052e2d52d43de16d8da5f20043320f7c367da546e1d1c9ca08ccc4`
- per-visual CSV: `071ce4751a01b7d07c6c47d172a5aeb855c536c2328e005f03540eb906765dc2`
- summary JSON: `d7ca13330e7e47d55af3d5bb4f102486857da9be691a16671011187e9b0e6aec`

No game was run, no production source was changed, and no GPU or Windows visibility test was performed.
