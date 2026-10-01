# Replay from the live active-input observer

`run_active_frame700.py` replays the copied engine-winning `level` bytes and same-run primary-view inputs, rather than taking `ViewportNear` and the start sector from saved configuration. It also checks the observer log’s successful generation-1 copy receipt and joins the existing 5,316 Flora ID/geometry fixture plus same-frame wind census.

## Inputs and source qualification

- Active reader copy: generation 1, `copy_ok=1`, suppression 0; SHA-256 `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`, equal to the known winning `l01_escape/level` bytes.
- Primary eye/view/projection/full-transform bits match frame 700’s earlier source/capture fixtures. `saved_camera_eye_bits` equals current eye bits on this frame. `ViewportNear=0.200000003`; source inputs are `r_ssaDISCARD=1.68038387e-5`, `r_ssaLOD_A=1.56073773e-4`, `r_ssaLOD_B=8.77914863e-5`, `rsDrawPortals=off`.
- Live source CFORM down/up rays both hit sector 25. The copied winning portal polygons produce no vertical ray intersection at this eye, so the source portal-first test has no portal hit to supersede the CFORM sector for this frame.
- Same-frame Flora wind census is present and finite, but its own receipt says `wind_verified=false`; this replay does not use those arrays to expand bounds.

## CPU replay

The replay runs the options-6 HOM-off superset with the **actual** saved camera eye and live near/SSA values. It reaches sectors 17, 21, 22, 24, 25, 30 with frustum counts `1,1,1,1,1,3`, matching the earlier source-screen graph. Per-ID owner-frustum tests match every prior raw OGF visibility flag: **4,427 outside and 889 intersecting**, with zero mismatches across 5,316 IDs. `active-frame700-per-id.csv` contains the joined live/prior flags; the JSON summary pins all input receipts and walker counters.

All five parser/walker selftests still pass, including fail-open malformed refs/cycles, multiple paths/frusta, missing ownership and nonfinite camera input. This remains a raw OGF sphere+AABB candidate census. It does not recalculate the 4,086 indexed-mesh-plus-wind envelope exclusions, instantiate an engine CDB model, apply HOM or source root LOD/ancestor decisions, or suppress a draw.

## Next C++ shape

A concrete, source-math-reusing integration sketch is in [CPP_INTEGRATION_OUTLINE.md](../CPP_INTEGRATION_OUTLINE.md). It keeps the C++ portal graph/walker proposal separate from this Python replay and uses `CPortal::Setup`, `CDB`, and `CFrustum` APIs instead of porting their float formulas. No source, build, cook, or game files were changed for this replay.
