# CPU-only portal graph prototype

This is a bounded, value-owned prototype for source topology parsing and HOM-off portal visibility. It does not link engine objects and cannot cull a game draw.

Run from this directory:

```sh
python3 -m unittest -v test_portal_graph.py
python3 run_frame700.py
python3 run_active_frame700.py
```

`run_frame700.py` reads the pinned extracted `level`; `run_active_frame700.py` replays the generation-1 engine-winning level copy, actual live primary view/saved eye/near/SSA values, CFORM sector rays, and same-frame Flora wind receipt. Both validate the geometry/ID receipts and write per-ID CSV plus summary files. The start sector 25 is an external receipt from `offline-start-sector/RESULT.md`; this prototype does not own or rebuild the CDB portal model. All output remains in this directory.

The Python walker mirrors `CPortal::Setup`, `CreateFrustumFromPortal`, `CSector::traverse`, and the relevant `CFrustum` operations with explicit float32 round trips for vectors and planes. It uses the exact live SSA early-continue order. It conservatively preserves the incoming frustum at ambiguous plane boundaries, unsupported clipped polygons over six vertices, malformed inputs, and missing owners. Homogeneous visibility is off. This is a source-anchored mirror, not a bit-exact C++ execution or runtime consumer.

The fixture has no `Device.vCameraPosition_saved` bitstream. The run temporarily reuses `eye_bits` for the dual-render near-camera check because the prior frame-700 screen found no portal sphere centers enclosing that same eye. Treat that as a fixture limitation; the active-input observer must capture and compare saved camera position before any runtime use.
